"""Product declarations shared by SDK configuration, installation and development."""
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path

SDK_VERSION = "0.2.0"
AREAS = {
    "levels": {"ir", "lowering", "analysis", "transforms", "decisions", "agent"},
    "exits": {"ir", "legalize", "transforms", "decisions", "capabilities", "emit", "abi", "agent"},
    "runtime": {"binding", "launch", "cache"},
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
    def git(*args):
        return subprocess.check_output(["git", "-C", root, *args])
    if Path(os.fsdecode(git("rev-parse", "--show-toplevel").strip())).resolve() != root.resolve():
        raise ValueError(f"expected an independent Git product at {root}")
    head = subprocess.run(["git", "-C", root, "rev-parse", "--verify", "--quiet", "HEAD"],
                          capture_output=True)
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


def tree(root, complete=True):
    root = Path(root).resolve()
    product = read(root / "product.json")
    name(product["name"])
    if product["compilerinfra"] != SDK_VERSION or not product["version"]:
        raise ValueError("incompatible compilerinfra version or missing product version")
    config = relative(product["configuration"])
    conf = read(root / config)
    if not isinstance(conf, dict):
        raise ValueError("configuration must be an object")
    frontend = name(product["frontend"]["language"])
    if not product["frontend"]["entry"] or not product["build"] or not product["install"]:
        raise ValueError("expected frontend, build and install entries")
    parts = [("", root, product)]
    for ext in product.get("extensions", []):
        alias = name(ext["name"])
        package = (root / ext["path"]).resolve()
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
        units[key] = dict(directory=str(path), package=str(package), path=directory,
                          owner=owner, allowed=allowed)
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
            if key in nodes or n["level"] not in levels or not n["contract"].strip():
                raise ValueError(f"{key}: duplicate node, unknown level or missing contract")
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
            exits[key] = dict(e, unit=unit(prefix + path, package, path, path, []))
        for key in decl.get("runtimes", []):
            name(key)
            if key in runtimes:
                raise ValueError(f"duplicate runtime {key}")
            path = f"runtime/{key}"
            runtimes[key] = unit(prefix + path, package, path, path, [])
    roots = [k for k,v in nodes.items() if v["parent"] == "entry"]
    if (nodes or complete) and len(roots) != 1:
        raise ValueError("expected one root with parent 'entry'")
    def ancestors(k):
        chain = []
        while k != "entry":
            if k in chain or k not in nodes:
                raise ValueError(f"invalid parent {k} in {chain}")
            chain.append(k)
            k = nodes[k]["parent"]
        return chain[::-1]
    for k,n in nodes.items():
        chain = ancestors(k)
        indices = [list(levels).index(nodes[i]["level"]) for i in chain]
        if indices != sorted(set(indices)) or indices[0] != 0:
            raise ValueError(f"{k}: parent must precede child in level order")
        units[n["unit"]]["allowed"] = [nodes[i]["unit"] for i in chain]
    if roots:
        units[front]["allowed"] = [nodes[roots[0]]["unit"]]
    for k,e in exits.items():
        node, sep, stop = e["at"].partition("@")
        if not sep or node not in nodes or stop not in nodes[node]["boundaries"]:
            raise ValueError(f"{k}: unknown attachment {e['at']}")
        chain = ancestors(node)
        route = [i+"@"+b for i in chain for b in nodes[i]["boundaries"]]
        e["boundaries"] = route[:route.index(e["at"])+1]
        units[e["unit"]]["allowed"] = [nodes[i]["unit"] for i in chain]
    if not set(runtimes) <= set(exits) or complete and (not exits or runtimes.keys() != exits.keys()):
        raise ValueError("expected one runtime for each declared exit")
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
                owners = [u for u,v in units.items() if v["owner"] == rel]
                if rel.startswith("tools/"):
                    unit(key, package, rel, rel, list(units))
                elif alias and owners and rel.split("/")[0] in {"levels", "exits"}:
                    unit(key, package, rel, rel, list(units[owners[0]]["allowed"]))
                else:
                    raise ValueError(f"{path}: undeclared unit")
        for folder in [package/"agent", *(Path(v["directory"])/"agent" for v in units.values()
                                            if v["package"] == str(package))]:
            if not folder.exists():
                continue
            for f in sorted(folder.rglob("*")):
                if not f.is_file() or "__pycache__" in f.parts:
                    continue
                rel = f.relative_to(package).as_posix()
                if f.suffix == ".py" or f.parent.name == "skills" and f.suffix == ".md":
                    assets.append(dict(path=prefix+rel, source=str(f)))
    return dict(product=product, levels=list(levels.values()), nodes=nodes, exits=exits,
                runtimes=runtimes, units=units, assets=assets, configuration=conf,
                packages={a or ".":str(p) for a,p,_ in parts})


def provenance(layout):
    return {k:fingerprint(Path(p)) for k,p in layout["packages"].items()}
