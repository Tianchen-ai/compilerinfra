"""Load one installed product in a private Python package with verified sources."""

import builtins
import hashlib
import importlib
from io import BytesIO
from importlib.abc import ResourceReader
from importlib.abc import MetaPathFinder, Loader
from importlib.util import spec_from_loader
from pathlib import Path
import sys
from types import ModuleType
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


class ProductModules(MetaPathFinder, Loader):
    def __init__(self, library, description):
        self.namespace = "_compilerinfra_" + uuid4().hex
        self.modules = {"": (None, True, "")}
        self.files = {}
        frontend = description["product"]["frontend"]
        root = library.parent / (description["product"]["name"] + ".assets")
        self.runtime_owners = {}
        for owner, key in description["runtimes"].items():
            allowed = description["units"][key]["allowed"]
            self.runtime_owners[owner] = {
                owner,
                *[description["units"][k]["owner"].split("/")[-1] for k in allowed],
            }
        for asset in description["assets"]:
            kind = asset.get("kind")
            if kind not in ("frontend", "runtime"):
                continue
            installed = Path(asset["path"])
            if kind == "frontend":
                rel = Path("frontend") / installed.relative_to(
                    "frontend", frontend["language"]
                )
            else:
                unit = asset["unit"]
                rel = Path(description["units"][unit]["owner"]) / installed.relative_to(
                    unit
                )
            path = (root / installed).resolve(strict=True)
            if not path.is_relative_to(root):
                raise ValueError(f"frontend escapes installation: {path}")
            source = path.read_bytes()
            if hashlib.sha256(source).hexdigest() != asset["sha256"]:
                raise ValueError(
                    f"installed product module differs from manifest: {rel}"
                )
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
        self.entry = "frontend." + self.entry
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
        module.__builtins__ = dict(vars(builtins), __import__=self.importer(module))
        exec(compile(source, file, "exec"), module.__dict__)

    def qualified(self, name):
        top = name.partition(".")[0]
        if top in {"frontend", "runtime"}:
            return self.namespace + "." + name
        if "frontend." + top in self.modules:
            return self.namespace + ".frontend." + name
        return name

    def check_dependency(self, caller, dest):
        if not caller.startswith("runtime.") or not dest.startswith(
            self.namespace + "."
        ):
            return
        owner = caller.split(".")[1]
        parts = dest[len(self.namespace) + 1 :].split(".")
        if parts == ["runtime"]:
            return
        if parts[0] != "runtime" or parts[1] not in self.runtime_owners[owner]:
            raise ImportError(f"runtime/{owner} cannot import {'.'.join(parts)}")

    def importer(self, module):
        caller = module.__name__[len(self.namespace) + 1 :]

        def resolve(name, package=None):
            if name.startswith("."):
                name = importlib.util.resolve_name(
                    name, self.qualified(package) if package else package
                )
            qualified = self.qualified(name)
            self.check_dependency(caller, qualified)
            return qualified

        def dynamic(name, package=None):
            dest = resolve(name, package)
            if dest in interfaces:
                return interfaces[dest]
            return importlib.import_module(dest)

        def find_spec(name, package=None):
            return importlib.util.find_spec(resolve(name, package))

        def view(original, **updates):
            facade = ModuleType(original.__name__)
            facade.__dict__.update(vars(original))
            facade.__dict__.update(updates)
            return facade

        util = view(importlib.util, find_spec=find_spec)

        def load(name, globals=None, locals=None, fromlist=(), level=0):
            if level:
                dest = resolve(
                    "." * level + name,
                    (globals or {}).get("__package__", module.__package__),
                )
            else:
                dest = resolve(name)
            if dest.startswith(self.namespace + ".") or dest == self.namespace:
                for item in fromlist or ():
                    self.check_dependency(caller, dest + "." + item)
                result = builtins.__import__(
                    dest, globals, locals, fromlist or ("__name__",), 0
                )
                if fromlist or level:
                    return result
                return sys.modules[self.qualified(name.partition(".")[0])]
            if name == "importlib":
                return lib
            if name == "importlib.util":
                return util if fromlist else lib
            if name == "builtins":
                return local_builtins
            if name.startswith("importlib."):
                result = builtins.__import__(name, globals, locals, fromlist, 0)
                if not fromlist:
                    setattr(
                        lib, name.split(".")[1], getattr(importlib, name.split(".")[1])
                    )
                    lib.util = util
                    return lib
                return result
            return builtins.__import__(name, globals, locals, fromlist, level)

        lib = view(importlib, import_module=dynamic, util=util, __import__=load)
        local_builtins = view(builtins, __import__=load)
        interfaces = {
            "importlib": lib,
            "importlib.util": util,
            "builtins": local_builtins,
        }
        return load

    def runtime(self, owner, module):
        if owner not in self.runtime_owners:
            raise ValueError(f"unknown runtime {owner}")
        key = f"runtime.{owner}" + ("." + module if module else "")
        if key not in self.modules:
            raise ModuleNotFoundError(key)
        return importlib.import_module(self.qualified(key))

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
