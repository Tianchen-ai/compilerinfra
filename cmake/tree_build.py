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
                ".hpp",
                ".cpp",
                ".cc",
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
                path = next((p for p in candidates if p.is_file()), None)
                public_abi = (
                    folder in public
                    and u["path"].startswith("exits/")
                    and f.is_relative_to(folder / "abi")
                )
                if path is None:
                    if (
                        (public_abi or u["path"].startswith("runtime/"))
                        and str(inc).startswith("compilerinfra/")
                        and str(inc) != "compilerinfra/Runtime.h"
                    ):
                        raise ValueError(
                            f"{key}: {f}:{line_no}: forbidden runtime dependency {inc}"
                        )
                    continue
                pub, private = owner(path, public), owner(path, dirs)
                if public_abi and not path.is_relative_to(folder / "abi"):
                    raise ValueError(
                        f"{key}: {f}:{line_no}: public exit ABI imports compiler implementation {inc}"
                    )
                if (
                    pub == key
                    or private == key
                    or pub in u["allowed"]
                    or u["path"].startswith("tools/")
                ):
                    continue
                if u["path"].startswith("runtime/") and pub:
                    exit_path = u["path"].replace("runtime/", "exits/", 1)
                    if units[pub]["path"] == exit_path and path.is_relative_to(
                        Path(units[pub]["package"])
                        / "include"
                        / product
                        / exit_path
                        / "abi"
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
