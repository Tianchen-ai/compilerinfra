"""SDK build hook. Configure-time graph checks and build-time embedded provenance."""

import os
import json
from pathlib import Path
import re
import sys

# Installed next to the SDK hook, also usable from the source SDK.
sys.path.insert(0, str(Path(__file__).resolve().parent / "python"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from compilerinfra.tree import AREAS, provenance, tree


def ident(key):
    return "ci_" + key.encode().hex()


def quote(text):
    if any(c in text for c in ('"', ";", "$", "\n")):
        raise ValueError(f"unsupported CMake path {text!r}")
    return '"' + text + '"'


def save(path, data):
    if not path.exists() or path.read_text() != data:
        path.write_text(data)


def source_directories(model):
    # Use the declared package location even when building from private snapshots.
    root = Path(model["packages"]["."])
    packages = {".": "."}
    for ext in model["product"].get("extensions", []):
        packages[ext["name"]] = os.path.relpath(root / ext["path"], root)
    return packages


def unit_directory(model, u):
    key = next(k for k, v in model["packages"].items() if v == u["package"])
    return key, str(Path(source_directories(model)[key]) / u["path"])


def configure(model, out):
    units = model["units"]
    lines = []
    for key, u in units.items():
        target = ident(key)
        lines.extend(
            [
                f"set(ci_unit {quote(key)})",
                f"set(ci_target {target})",
                f"set(ci_owner {quote(u['owner'])})",
                f"set(ci_source_ref {quote(unit_directory(model, u)[1])})",
                f"set(ci_source_package {quote(source_directories(model)[unit_directory(model, u)[0]])})",
                f"set(ci_package {quote(u['package'])})",
                f"add_subdirectory({quote(u['directory'])} units/{target})",
                f"if(NOT TARGET {target})",
                f'  message(FATAL_ERROR "{key}: missing compilerinfra_unit()")',
                "endif()",
                f"set_property(TARGET {target} PROPERTY CI_ALLOWED "
                + " ".join(ident(d) for d in u["allowed"])
                + ")",
            ]
        )
    lines.append("set(ci_targets " + " ".join(map(ident, units)) + ")")
    save(out / "units.cmake", "\n".join(lines) + "\n")


def generated(model, out):
    declarations, calls = [], []
    for key, u in model["units"].items():
        if u["path"].startswith(("runtime/", "tools/")):
            continue
        ns = ident(key)
        declarations.append(f"namespace {ns} {{ void bind(compilerinfra::Unit &); }}")
        calls.append(f'  registry.bind("{key}", {ns}::bind);')
    model["fingerprint"] = provenance(model)
    model["library"] = (out / "library.path").read_text().strip()
    for u in model["units"].values():
        u["package"], u["directory"] = unit_directory(model, u)
    model["packages"] = source_directories(model)
    for ext in model["product"].get("extensions", []):
        ext["path"] = model["packages"][ext["name"]]
    model["product"]["install"] = {"environment": model["library"] + ".python"}
    model["product"]["build"] = {"entry": "CMakeLists.txt"}
    for asset in model["assets"]:
        del asset["source"]
    save(
        out / "Product.cpp",
        '#include "compilerinfra/Product.h"\n'
        + "\n".join(declarations)
        + "\nvoid ci_register(compilerinfra::Assembly &registry) {\n"
        + "\n".join(calls)
        + "\n}\nconst char *ci_manifest() {return "
        + json.dumps(json.dumps(model, ensure_ascii=True, separators=(",", ":")))
        + ";}\n",
    )


def audit(model):
    units = model["units"]
    product = model["product"]["name"]
    allowed_surface = {
        "CMakeLists.txt",
        "product.json",
        "AGENTS.md",
        "CLAUDE.md",
        "frontend",
        "levels",
        "exits",
        "runtime",
        "include",
        "agent",
        "tools",
        "doc",
        "examples",
        "test",
        "cmake",
    }
    public = {}
    dirs = {}
    for key, u in units.items():
        folder = Path(u["directory"])
        dirs[folder] = key
        package = Path(u["package"])
        public[package / "include" / product / u["path"]] = key
        for f in folder.iterdir():
            if f.name == "plugin.json":
                raise ValueError(f"{key}: {f} duplicates product.json registration")
            if (
                f.is_dir()
                and u["path"].split("/")[0] in AREAS
                and f.name not in AREAS[u["path"].split("/")[0]]
            ):
                raise ValueError(f"{key}: unknown unit area {f}")
        for f in folder.rglob("CMakeLists.txt"):
            if f.parent != folder:
                raise ValueError(f"{key}: nested registration at {f}")

    abi_files = {}
    for key, u in units.items():
        if not u["path"].startswith(("levels/", "exits/")):
            continue
        source = Path(u["directory"]) / "abi"
        published = Path(u["package"]) / "include" / product / u["path"] / "abi"
        for area in (source, published):
            for f in area.rglob("*"):
                if not f.is_file():
                    continue
                if f.suffix not in {
                    ".h",
                    ".hh",
                    ".hpp",
                    ".hxx",
                    ".inc",
                    ".json",
                    ".yaml",
                    ".yml",
                    ".td",
                    ".def",
                    ".proto",
                }:
                    raise ValueError(
                        f"{key}: ABI must be a header or data description: {f}"
                    )
                ref = Path(product) / u["path"] / "abi" / f.relative_to(area)
                if ref in abi_files:
                    raise ValueError(f"{key}: duplicate public ABI path {ref}")
                abi_files[ref] = f.resolve()
    abi_sources = set(abi_files.values())

    def owner(file, roots):
        return next((roots[p] for p in file.parents if p in roots), None)

    for package in map(Path, model["packages"].values()):
        for f in package.iterdir():
            if (
                f.is_dir()
                and not f.name.startswith(".")
                and f.name not in allowed_surface
            ):
                # Extensions may live in a neighbouring or nested independent package.
                if not any(
                    Path(p).is_relative_to(f) for p in model["packages"].values()
                ):
                    raise ValueError(f"{f}: unknown product directory")
        for f in (package / "include").rglob("*"):
            if f.is_file() and not owner(f, public):
                raise ValueError(f"{f}: public interface has no tree unit")
    for folder, key in (public | dirs).items():
        u = units[key]
        for f in folder.rglob("*"):
            if not f.is_file() or f.suffix not in {
                ".h",
                ".hh",
                ".hxx",
                ".hpp",
                ".cpp",
                ".cc",
                ".cxx",
                ".c",
                ".inc",
                ".td",
            }:
                continue
            for line_no, line in enumerate(f.read_text().splitlines(), 1):
                m = re.match(r'\s*(?:#\s*include|include)\s*[<"]([^">]+)', line)
                if not m:
                    continue
                inc = Path(m[1])
                candidates = [(f.parent / inc).resolve()] + [
                    (Path(p) / "include" / inc).resolve()
                    for p in model["packages"].values()
                ]
                path = next((p for p in candidates if p.is_file()), abi_files.get(inc))
                public_abi = u["path"].startswith(
                    ("levels/", "exits/")
                ) and f.is_relative_to(folder / "abi")
                runtime = u["path"].startswith("runtime/")
                if path is None:
                    if (
                        (public_abi or runtime)
                        and inc.parts[:1] == (product,)
                        and inc.parts[1:2] in {("levels",), ("exits",)}
                    ):
                        raise ValueError(
                            f"{key}: {f}:{line_no}: undeclared ABI header {inc}"
                        )
                    if (
                        (public_abi or runtime)
                        and str(inc).startswith(("compilerinfra/", "mlir/", "llvm/"))
                        and str(inc) != "compilerinfra/Runtime.h"
                    ):
                        raise ValueError(
                            f"{key}: {f}:{line_no}: forbidden compiler dependency {inc}"
                        )
                    continue
                pub, private = owner(path, public), owner(path, dirs)
                dest = pub or private
                if dest and path in abi_sources:
                    allowed = u.get("abi", []) if runtime else [key, *u["allowed"]]
                    if units[dest]["owner"] in {
                        units[k]["owner"] for k in allowed
                    } or u["path"].startswith("tools/"):
                        continue
                if public_abi:
                    raise ValueError(
                        f"{key}: {f}:{line_no}: ABI imports implementation {inc}"
                    )
                if (
                    pub == key
                    or private == key
                    or pub in u["allowed"]
                    or u["path"].startswith("tools/")
                ):
                    continue
                if (
                    pub
                    or private
                    or any(
                        path.is_relative_to(Path(p)) for p in model["packages"].values()
                    )
                ):
                    raise ValueError(f"{key}: {f}:{line_no}: forbidden include {inc}")


if __name__ == "__main__":
    if sys.argv[1] == "publish":
        from compilerinfra.installation import publish
        from tempfile import TemporaryDirectory
        import shutil

        stage, prefix, library, previous = map(Path, sys.argv[2:])
        with TemporaryDirectory(dir=stage.parent) as tmp:
            with publish(stage, prefix, Path(tmp), library, previous_manifest=previous):
                pass
        shutil.rmtree(stage)
        sys.exit(0)
    mode, root, out = sys.argv[1:]
    model = tree(
        root, extension_roots=json.loads(os.environ.get("CI_EXTENSION_ROOTS", "{}"))
    )
    if mode == "configure":
        audit(model)
        configure(model, Path(out))
        save(Path(out) / "layout.json", json.dumps(model))
    else:
        audit(model)
        generated(model, Path(out))
