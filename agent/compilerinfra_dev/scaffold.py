"""Create product sources and edit the single tree declaration."""

import os
import json
from pathlib import Path
import shutil
import subprocess
from compilerinfra.tree import name as identifier, read, tree, write


def unit(root, folder, source, body):
    directory = root / folder
    directory.mkdir(parents=True, exist_ok=False)
    path = directory / source
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body)
    (directory / "CMakeLists.txt").write_text(f"compilerinfra_unit(SOURCES {source})\n")


def registration(body):
    return (
        '#include "compilerinfra/Assembly.h"\n'
        '#include "mlir/IR/Verifier.h"\n\n'
        "namespace CI_UNIT_NAMESPACE {\nusing namespace compilerinfra;\n"
        "void bind(Unit &unit) {\n" + body + "}\n}\n"
    )


def init(directory: str, name: str, version: str = "0.1.0", language: str = "mlir"):
    """Initialize a product. The default frontend accepts root MLIR bytes."""
    identifier(name)
    identifier(language)
    root = Path(directory).resolve()
    root.mkdir(parents=True, exist_ok=True)
    if any(p.name not in {".git", ".work"} for p in root.iterdir()):
        raise ValueError(f"expected empty product directory: {root}")
    subprocess.run(["git", "init", "-q", "-b", "main", root], check=True)
    (root / ".gitignore").write_text("/.work/\n__pycache__/\n")
    config = "cmake/default.json"
    write(root / config, {"nodes": {}, "exits": {}})
    write(
        root / "product.json",
        dict(
            name=name,
            version=version,
            compilerinfra="0.3.0",
            levels=[],
            nodes=[],
            exits=[],
            runtimes=[],
            frontend=dict(language=language, entry="author:root_ir"),
            configuration=config,
            build=dict(configure_args=[]),
            install=dict(python=os.sys.executable),
        ),
    )
    (root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.22)\n"
        "find_package(CompilerInfra 0.3.0 EXACT REQUIRED CONFIG COMPONENTS Project)\n"
        "compilerinfra_project_name(product_name)\nproject(${product_name} LANGUAGES C CXX)\n"
        "find_package(CompilerInfra 0.3.0 EXACT REQUIRED CONFIG)\ncompilerinfra_product()\n"
    )
    (root / "AGENTS.md").write_text(
        "Read product.json and the installed compiler layout. "
        "Register changes in their owning unit. Use the development skill for derivation and measurement.\n"
    )
    unit(root, f"frontend/{language}", "Input.cpp", registration("  (void)unit;\n"))
    (root / f"frontend/{language}/author.py").write_text(
        "def root_ir(source: bytes) -> bytes:\n    return source\n"
    )
    return str(root / "product.json")


def update(directory, field, item, folder=None, source=None, body=None):
    root = Path(directory).resolve()
    file = root / "product.json"
    before = file.read_bytes()
    product = read(file)
    product[field].append(item)
    if folder:
        unit(root, folder, source, body)
    try:
        write(file, product)
        tree(root, complete=False)
    except BaseException:
        file.write_bytes(before)
        if folder:
            shutil.rmtree(root / folder)
        raise
    return str(root / (folder or "product.json"))


def add_level(directory: str, name: str, responsibility: str):
    """Append an ordered level and its responsibility."""
    return update(
        directory, "levels", dict(name=identifier(name), responsibility=responsibility)
    )


def add_node(
    directory: str,
    level: str,
    name: str,
    parent: str,
    contract: str,
    boundaries: list[str],
):
    """Create a representation with its incoming lowering and ordered boundaries."""
    for key in [level, name, *parent.split("@"), *boundaries]:
        identifier(key)
    body = (
        "  auto lower = [](mlir::OpPassManager &, const TargetProfile &) { return llvm::Error::success(); };\n"
        "  auto verify = [](mlir::ModuleOp m, const TargetProfile &) { return mlir::verify(m); };\n"
    )
    for b in boundaries:
        body += f'  unit.boundaries.push_back({{{{"{b}", lower, verify}}, COMPILERINFRA_SOURCE}});\n'
    return update(
        directory,
        "nodes",
        dict(
            name=name,
            level=level,
            parent=parent,
            contract=contract,
            boundaries=boundaries,
        ),
        f"levels/{level}/{name}",
        "lowering/Boundaries.cpp",
        registration(body),
    )


def add_exit(
    directory: str,
    name: str,
    at: str,
    description: str,
    boundaries: list[str] | None = None,
):
    """Attach an exit. Its initial emitter returns verified textual MLIR."""
    boundaries = (
        ["ready"] if boundaries is None else [identifier(b) for b in boundaries]
    )
    body = (
        "  auto lower = [](mlir::OpPassManager &, const TargetProfile &) { return llvm::Error::success(); };\n"
        "  auto verify = [](mlir::ModuleOp m, const TargetProfile &) { return mlir::verify(m); };\n"
    )
    for b in boundaries:
        body += f'  unit.boundaries.push_back({{{{"{b}", lower, verify}}, COMPILERINFRA_SOURCE}});\n'
    body += (
        "  ExitImplementation value;\n"
        "  value.emit = [](mlir::ModuleOp m, llvm::raw_pwrite_stream &out) { m.print(out); return llvm::Error::success(); };\n"
        "  unit.exit = Located<ExitImplementation>{std::move(value), COMPILERINFRA_SOURCE};\n"
    )
    return update(
        directory,
        "exits",
        dict(
            name=identifier(name), at=at, description=description, boundaries=boundaries
        ),
        f"exits/{name}",
        "emit/Exit.cpp",
        registration(body),
    )


def add_runtime(directory: str, owner: str | None = None, exit: str | None = None):
    """Add runtime code owned by a node or an exit."""
    if (owner is None) == (exit is None):
        raise ValueError("supply one runtime owner")
    owner = identifier(owner or exit)
    return update(
        directory,
        "runtimes",
        owner,
        f"runtime/{owner}",
        "binding/Binding.cpp",
        '#include "compilerinfra/Runtime.h"\nnamespace CI_UNIT_NAMESPACE {}\n',
    )


def host_config(host: str, library: str, workspace: str, output_dir: str):
    """Generate the stdio MCP server configuration for Codex or Claude."""
    launch = dict(
        command="compilerinfra-agent",
        args=[
            "--library",
            str(Path(library).parent.resolve() / Path(library).name),
            "--workspace",
            str(Path(workspace).resolve()),
            "--output-dir",
            str(Path(output_dir).resolve()),
        ],
    )
    if host == "codex":
        return (
            "[mcp_servers.compilerinfra]\n"
            'command = "compilerinfra-agent"\n'
            + "args = "
            + json.dumps(launch["args"], ensure_ascii=False)
            + "\n"
        )
    if host == "claude":
        return {"mcpServers": {"compilerinfra": launch}}
    raise ValueError("expected codex or claude")
