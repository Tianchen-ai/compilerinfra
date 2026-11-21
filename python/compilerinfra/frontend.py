"""Load one installed frontend in a private Python package with verified sources."""

import builtins
import hashlib
import importlib
from io import BytesIO
from importlib.abc import ResourceReader
from importlib.abc import MetaPathFinder, Loader
from importlib.util import spec_from_loader
from pathlib import Path
import sys
from uuid import uuid4


class PackageResources(ResourceReader):
    def __init__(self, files, directory):
        self.files, self.directory = files, directory

    def open_resource(self, name):
        path = self.directory / name
        if path not in self.files:
            raise FileNotFoundError(name)
        return BytesIO(self.files[path])

    def resource_path(self, name):
        raise FileNotFoundError(name)

    def is_resource(self, name):
        return self.directory / name in self.files

    def contents(self):
        return {
            p.relative_to(self.directory).parts[0]
            for p in self.files
            if p.is_relative_to(self.directory)
        }


class Frontend(MetaPathFinder, Loader):
    def __init__(self, library, description):
        self.namespace = "_compilerinfra_" + uuid4().hex
        self.modules = {"": (None, True, "")}
        self.files = {}
        frontend = description["product"]["frontend"]
        root = (
            library.parent
            / (description["product"]["name"] + ".assets")
            / "frontend"
            / frontend["language"]
        )
        for asset in description["assets"]:
            if asset.get("kind") != "frontend":
                continue
            rel = Path(asset["path"]).relative_to("frontend", frontend["language"])
            path = (root / rel).resolve(strict=True)
            if not path.is_relative_to(root):
                raise ValueError(f"frontend escapes installation: {path}")
            source = path.read_bytes()
            if hashlib.sha256(source).hexdigest() != asset["sha256"]:
                raise ValueError(f"installed frontend differs from manifest: {rel}")
            self.files[rel] = source
            if rel.suffix != ".py":
                continue
            parts = rel.with_suffix("").parts
            package = parts[-1] == "__init__"
            if package:
                parts = parts[:-1]
            for i in range(1, len(parts)):
                self.modules.setdefault(".".join(parts[:i]), (None, True, ""))
            self.modules[".".join(parts)] = (source, package, str(path))
        self.entry, self.symbol = frontend["entry"].split(":")
        if self.entry not in self.modules:
            raise ValueError(
                f"missing frontend entry {self.entry} in installed manifest"
            )
        sys.meta_path.insert(0, self)

    def find_spec(self, fullname, path=None, target=None):
        if fullname == self.namespace or fullname.startswith(self.namespace + "."):
            key = fullname[len(self.namespace) :].lstrip(".")
            if key not in self.modules:
                raise ModuleNotFoundError(f"undeclared frontend module {key}")
            return spec_from_loader(fullname, self, is_package=self.modules[key][1])
        return None

    def create_module(self, spec):
        return None

    def get_resource_reader(self, fullname):
        key = fullname[len(self.namespace) :].lstrip(".")
        return PackageResources(self.files, Path(*key.split(".")))

    def exec_module(self, module):
        source, _, file = self.modules[
            module.__name__[len(self.namespace) :].lstrip(".")
        ]
        if source is None:
            return
        module.__file__ = file
        module.__builtins__ = dict(vars(builtins), __import__=self.local_import)
        exec(compile(source, file, "exec"), module.__dict__)

    def local_import(self, name, globals=None, locals=None, fromlist=(), level=0):
        top = name.partition(".")[0]
        if level == 0 and top and top in self.modules:
            absolute = self.namespace + "." + name
            result = builtins.__import__(
                absolute, globals, locals, fromlist or ("*",), 0
            )
            return result if fromlist else sys.modules[self.namespace + "." + top]
        return builtins.__import__(name, globals, locals, fromlist, level)

    def __call__(self, value):
        module = importlib.import_module(self.namespace + "." + self.entry)
        result = getattr(module, self.symbol)(value)
        if not isinstance(result, bytes):
            raise TypeError(f"{self.entry}:{self.symbol} must return bytes")
        return result

    def close(self):
        if sys.meta_path and self in sys.meta_path:
            sys.meta_path.remove(self)
        for key in list(sys.modules or {}):
            if key == self.namespace or key.startswith(self.namespace + "."):
                del sys.modules[key]
