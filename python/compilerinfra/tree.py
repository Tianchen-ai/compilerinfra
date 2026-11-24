"""Product declarations shared by SDK configuration, installation and development."""

import hashlib
import json
import os
import re
import subprocess
from pathlib import Path

SDK_VERSION = "0.5.1"
AREAS = {
    "levels": {
        "abi",
        "ir",
        "lowering",
        "analysis",
        "transforms",
        "decisions",
        "cost",
        "agent",
    },
    "exits": {
        "ir",
        "legalize",
        "transforms",
        "decisions",
        "capabilities",
        "cost",
        "emit",
        "abi",
        "agent",
    },
    "runtime": {"binding", "launch", "cache", "kernels"},
}


def name(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", value):
        raise ValueError(f"invalid tree identifier {value!r}")
    return value


def relative(value):
    path = Path(value)
    if path.is_absolute() or ".." in path.parts or path == Path("."):
        raise ValueError(f"expected a package-relative path: {value}")
    return path


def read(path):
    return json.loads(path.read_text())


def write(path, data):
    text = json.dumps(data, indent=2, ensure_ascii=False) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def fingerprint(root):
    if not (root / ".git").exists() and (root / "PKG-INFO").is_file():
        product = read(root / "product.json")
        digest = hashlib.sha256()
        for p in sorted(root.rglob("*")):
            rel = p.relative_to(root)
            if (
                any(part == "__pycache__" or part.endswith(".egg-info") for part in rel.parts)
                or p.suffix in {".pyc", ".pyo"}
            ):
                continue
            if p.is_symlink():
                digest.update(rel.as_posix().encode() + b"\0L" + os.fsencode(os.readlink(p)))
            elif p.is_file():
                digest.update(rel.as_posix().encode() + b"\0")
                digest.update(b"X" if p.stat().st_mode & 0o111 else b"F")
                with p.open("rb") as stream:
                    for block in iter(lambda: stream.read(1 << 20), b""):
                        digest.update(block)
        return {"revision": f"sdist-{product.get('version', '0')}", "changes": digest.hexdigest()}

    def git(*args):
        return subprocess.check_output(["git", "-C", root, *args])

    if (
        Path(os.fsdecode(git("rev-parse", "--show-toplevel").strip())).resolve()
        != root.resolve()
    ):
        raise ValueError(f"expected an independent Git product at {root}")
    head = subprocess.run(
        ["git", "-C", root, "rev-parse", "--verify", "--quiet", "HEAD"],
        capture_output=True,
    )
    rev = head.stdout.decode().strip() or "unborn"
    digest = hashlib.sha256()
    # The working files, including staged edits and untracked source, are the build inputs.
    if rev != "unborn":
        digest.update(git("diff", "--no-ext-diff", "--binary", "HEAD", "--"))
    files = git("ls-files", "--others", "--exclude-standard", "-z")
    if rev == "unborn":
        files += git("ls-files", "--cached", "-z")
    for item in sorted(set(files.split(b"\0")) - {b""}):
        p = root / os.fsdecode(item)
        digest.update(item + b"\0")
        if p.is_symlink():
            digest.update(b"L" + os.fsencode(os.readlink(p)))
        elif p.is_file():
            digest.update(b"X" if p.stat().st_mode & 0o111 else b"F")
            with p.open("rb") as stream:
                for block in iter(lambda: stream.read(1 << 20), b""):
                    digest.update(block)
    return {"revision": rev, "changes": digest.hexdigest()}


def tree(root, complete=True, extension_roots=None):
    root = Path(root).resolve()
    product = read(root / "product.json")
    name(product["name"])
    if product["compilerinfra"] != SDK_VERSION or not product["version"]:
        raise ValueError(
            "incompatible compilerinfra version or missing product version"
        )
    config = relative(product["configuration"])
    conf = read(root / config)
    if not isinstance(conf, dict):
        raise ValueError("configuration must be an object")
    frontend = name(product["frontend"]["language"])
    if not re.fullmatch(
        r"[A-Za-z_]\w*(\.[A-Za-z_]\w*)*:[A-Za-z_]\w*", product["frontend"]["entry"]
    ):
        raise ValueError("expected Python frontend entry module:callable")
    args = product["build"]["configure_args"]
    if not isinstance(args, list) or not all(isinstance(a, str) for a in args):
        raise ValueError("expected build.configure_args array of strings")
    if "library" in product["install"]:
        raise ValueError(
            "install.library is derived by the build; remove it from product.json"
        )
    if (
        not isinstance(product["install"]["python"], str)
        or not product["install"]["python"]
    ):
        raise ValueError("expected install.python interpreter or environment entry")
    parts = [("", root, product)]
    for ext in product.get("extensions", []):
        alias = name(ext["name"])
        package = (root / (extension_roots or {}).get(alias, ext["path"])).resolve()
        if any(p == package or key == alias for key, p, _ in parts):
            raise ValueError(f"duplicate extension {alias}")
        parts.append((alias, package, read(package / "product.json")))
    levels, nodes, exits, runtimes, units = {}, {}, {}, {}, {}
    assets = []

    def unit(key, package, directory, owner, allowed):
        path = (package / directory).resolve()
        if not path.is_relative_to(package) or not (path / "CMakeLists.txt").is_file():
            raise ValueError(f"{key}: expected {path}/CMakeLists.txt")
        if key in units:
            raise ValueError(f"duplicate unit {key}")
        units[key] = dict(
            directory=str(path),
            package=str(package),
            path=directory,
            owner=owner,
            allowed=allowed,
        )
        return key

    front = f"frontend/{frontend}"
    unit(front, root, front, front, [])
    for alias, package, decl in parts:
        prefix = f"extensions/{alias}/" if alias else ""
        for level in decl.get("levels", []):
            key = name(level["name"])
            if key in levels or not level["responsibility"].strip():
                raise ValueError(f"duplicate or undescribed level {key}")
            levels[key] = level
        for n in decl.get("nodes", []):
            key = name(n["name"])
            if (
                key == "entry"
                or key in nodes
                or n["level"] not in levels
                or not n["contract"].strip()
            ):
                raise ValueError(
                    f"{key}: duplicate node, unknown level or missing contract"
                )
            bs = [name(b) for b in n["boundaries"]]
            if not bs or len(bs) != len(set(bs)):
                raise ValueError(f"{key}: expected ordered unique boundaries")
            path = f"levels/{n['level']}/{key}"
            nodes[key] = dict(n, unit=unit(prefix + path, package, path, path, []))
        for e in decl.get("exits", []):
            key = name(e["name"])
            if key in exits or not e["description"].strip():
                raise ValueError(f"duplicate or undescribed exit {key}")
            path = f"exits/{key}"
            if not e["boundaries"] or len(set(e["boundaries"])) != len(e["boundaries"]):
                raise ValueError(f"{key}: expected unique exit and ordered boundaries")
            for boundary in e["boundaries"]:
                name(boundary)
            exits[key] = dict(e, unit=unit(prefix + path, package, path, path, []))
        for key in decl.get("runtimes", []):
            name(key)
            if key in runtimes:
                raise ValueError(f"duplicate runtime {key}")
            path = f"runtime/{key}"
            runtimes[key] = unit(prefix + path, package, path, path, [])
    roots = [k for k, v in nodes.items() if v["parent"] == "entry"]
    if (nodes or complete) and len(roots) != 1:
        raise ValueError("expected one root with parent 'entry'")

    if nodes.keys() & exits.keys():
        raise ValueError(
            f"node and exit names overlap: {sorted(nodes.keys() & exits.keys())}"
        )
    if (
        set(runtimes) - (nodes.keys() | exits.keys())
        or complete
        and (not exits or not exits.keys() <= runtimes.keys())
    ):
        raise ValueError(
            "runtime requires a node or exit owner, and every exit requires a runtime"
        )

    chains = {}

    def attach(key, declaration, parent, node):
        before = []
        ancestors = []
        if parent != "entry":
            owner, sep, point = parent.partition("@")
            if owner not in chains:
                raise ValueError(
                    f"{key}: parent {owner} must be a node on an earlier level"
                )
            source = nodes[owner]
            if node and source["level"] == declaration["level"]:
                raise ValueError(f"{key}: parent must precede child in level order")
            if not sep and node:
                point = source["boundaries"][-1]
            if point not in source["boundaries"]:
                raise ValueError(f"{key}: unknown attachment {parent}")
            parent = owner + "@" + point
            ancestors = chains[owner]
            before = source["path"][: source["path"].index(parent) + 1]
        elif not node or declaration["level"] != next(iter(levels)):
            raise ValueError(f"{key}: only a first-level root may attach at entry")
        declaration["at"] = parent
        declaration["path"] = before + [
            key + "@" + b for b in declaration["boundaries"]
        ]
        chain = [*ancestors, key] if node else ancestors
        if node:
            chains[key] = chain
        units[declaration["unit"]]["allowed"] = [nodes[a]["unit"] for a in chain]
        if key in runtimes:
            runtime = units[runtimes[key]]
            runtime["allowed"] = [runtimes[a] for a in ancestors if a in runtimes]
            runtime["abi"] = [nodes[a]["unit"] for a in ancestors] + [
                declaration["unit"]
            ]

    for level in levels:
        for key, n in nodes.items():
            if n["level"] == level:
                attach(key, n, n["parent"], True)
    if roots:
        units[front]["allowed"] = [nodes[roots[0]]["unit"]]
    for key, e in exits.items():
        attach(key, e, e["at"], False)
    for alias, package, decl in parts:
        prefix = f"extensions/{alias}/" if alias else ""
        for pattern in ("levels/*/*", "exits/*", "runtime/*", "frontend/*", "tools/*"):
            for path in sorted(package.glob(pattern)):
                if not path.is_dir():
                    continue
                rel = path.relative_to(package).as_posix()
                key = prefix + rel
                if key in units:
                    continue
                owners = [u for u, v in units.items() if v["owner"] == rel]
                if rel.startswith("tools/"):
                    unit(key, package, rel, rel, list(units))
                elif alias and owners and rel.split("/")[0] in {"levels", "exits"}:
                    unit(key, package, rel, rel, list(units[owners[0]]["allowed"]))
                else:
                    raise ValueError(f"{path}: undeclared unit")
        for folder in [
            package / "agent",
            *(
                Path(v["directory"]) / "agent"
                for v in units.values()
                if v["package"] == str(package)
            ),
        ]:
            if not folder.exists():
                continue
            for f in sorted(folder.rglob("*")):
                if not f.is_file() or "__pycache__" in f.parts:
                    continue
                rel = f.relative_to(package).as_posix()
                if f.suffix == ".py" or f.parent.name == "skills" and f.suffix == ".md":
                    assets.append(
                        dict(
                            path=prefix + rel,
                            source=str(f),
                            sha256=hashlib.sha256(f.read_bytes()).hexdigest(),
                        )
                    )
    deps = {}
    for alias, _, declaration in parts:
        for dep in declaration.get("optional_dependencies", []):
            key = name(dep["id"])
            owner = dep["owner"]
            unit_id = ("extensions/" + alias + "/" if alias else "") + owner
            if (key in deps or unit_id not in units or not owner.startswith("exits/")
                    or not dep["probe"].startswith(unit_id + "::")
                    or not re.fullmatch(r"[\w.+-]+", dep["package"])
                    or not re.fullmatch(r"[\w.+-]+(?:::[\w.+-]+)+",dep["cmake_target"])
                    or not re.fullmatch(r">=\d+(?:\.\d+)*(?:,<\d+(?:\.\d+)*)?",dep["version"])):
                raise ValueError(f"{unit_id}: invalid optional dependency {key}")
            deps[key] = dep
    if deps:
        product["optional_dependencies"] = list(deps.values())
    for f in sorted((root / front).rglob("*")):
        rel = f.relative_to(root / front)
        if (
            f.is_file()
            and rel.parts[0] != "agent"
            and "__pycache__" not in rel.parts
            and not any(p.endswith(".egg-info") for p in rel.parts)
            and f.suffix != ".pyc"
        ):
            assets.append(
                dict(
                    path=f"{front}/{rel}",
                    kind="frontend",
                    source=str(f),
                    sha256=hashlib.sha256(f.read_bytes()).hexdigest(),
                )
            )
    for owner, key in runtimes.items():
        directory = Path(units[key]["directory"])
        for f in sorted(directory.rglob("*")):
            rel = f.relative_to(directory)
            if not f.is_file() or "__pycache__" in rel.parts:
                continue
            if f.suffix != ".py" and rel.parts[0] != "kernels":
                continue
            assets.append(
                dict(
                    path=key + "/" + str(rel),
                    kind="runtime" if f.suffix == ".py" else "kernel-source",
                    unit=key,
                    source=str(f),
                    sha256=hashlib.sha256(f.read_bytes()).hexdigest(),
                )
            )
    module = product["frontend"]["entry"].split(":")[0].replace(".", "/")
    if not any(
        item["path"]
        in (front + "/" + module + ".py", front + "/" + module + "/__init__.py")
        for item in assets
    ):
        raise ValueError(f"{front}: missing installed frontend module {module}")
    return dict(
        product=product,
        levels=list(levels.values()),
        nodes=nodes,
        exits=exits,
        runtimes=runtimes,
        units=units,
        assets=assets,
        configuration=conf,
        packages={a or ".": str(p) for a, p, _ in parts},
    )


def provenance(layout):
    return {k: fingerprint(Path(p)) for k, p in layout["packages"].items()}
