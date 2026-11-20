"""Source plugin declarations, activated once per compiler candidate."""

import inspect
import json
import importlib.util
import shutil
import sys
import types
from contextlib import ExitStack
from uuid import uuid4
from pathlib import Path
from typing import get_type_hints

from pydantic import ConfigDict, create_model

from .source import relative


class Actions:
    def __init__(self, compiler):
        self.compiler = compiler
        self.actions = {}
        self.skills = {}
        self.services = {}
        self.owners = {}
        self.resources = ExitStack()
        self.namespace = "_ci_" + uuid4().hex

    def action(self, fn):
        name = fn.__name__
        if name in self.actions:
            raise ValueError(f"duplicate plugin action {name}")
        hints = get_type_hints(fn)
        for parameter in inspect.signature(fn).parameters.values():
            if (
                parameter.kind
                not in (parameter.POSITIONAL_OR_KEYWORD, parameter.KEYWORD_ONLY)
                or parameter.name not in hints
            ):
                raise ValueError(
                    f"{name}: action parameter {parameter.name} requires a named, typed argument"
                )
        fields = {
            p.name: (hints[p.name], ... if p.default is p.empty else p.default)
            for p in inspect.signature(fn).parameters.values()
        }
        schema = create_model(name, __config__=ConfigDict(extra="forbid"), **fields)
        self.actions[name] = (fn, schema)
        return fn

    def provide(self, name, service):
        if name in self.services:
            raise ValueError(f"duplicate plugin service {name}")
        self.services[name] = service

    def on_close(self, cleanup):
        self.resources.callback(cleanup)

    def close(self):
        try:
            self.resources.close()
        finally:
            for k in list(sys.modules):
                if k == self.namespace or k.startswith(self.namespace + "."):
                    sys.modules.pop(k, None)
            self.services.clear()
            self.actions.clear()

    def describe(self):
        return {
            "plugins": self.owners,
            "skills": list(self.skills),
            "services": list(self.services),
            "actions": {
                k: {
                    "description": inspect.getdoc(fn),
                    "parameters": schema.model_json_schema(),
                }
                for k, (fn, schema) in self.actions.items()
            },
        }

    async def invoke(self, name, args):
        fn, schema = self.actions[name]
        data = schema.model_validate(args)
        value = fn(**{k: getattr(data, k) for k in type(data).model_fields})
        return await value if inspect.isawaitable(value) else value


def load(paths, compiler):
    entries = {}
    for path in paths:
        meta = json.loads(path.read_text())
        name = meta["id"]
        if name in entries:
            raise ValueError(f"duplicate plugin {name}")
        entries[name] = (path, meta)
    registry = Actions(compiler)
    package = types.ModuleType(registry.namespace)
    package.__path__ = []
    sys.modules[registry.namespace] = package
    pending = set()

    def activate(name):
        if name in registry.owners:
            return
        if name in pending or name not in entries:
            raise ValueError(f"cyclic or missing plugin dependency: {name}")
        pending.add(name)
        descriptor, meta = entries[name]
        folder = descriptor.parent

        def resource(name):
            file = (folder / relative(name)).resolve(strict=True)
            if not file.is_relative_to(folder.resolve()):
                raise ValueError(f"plugin resource escapes package: {name}")
            return file

        for dep in meta.get("dependencies", []):
            activate(dep)
        for skill, path in meta.get("skills", {}).items():
            registry.skills[f"{name}/{skill}"] = resource(path)
        previous = set(registry.actions)
        provided = set(registry.services)
        if action := meta.get("actions"):
            path, function = action.rsplit(":", 1)
            source = resource(path)
            package_name = registry.namespace + ".p" + str(len(registry.owners))
            package = types.ModuleType(package_name)
            package.__path__ = [str(source.parent)]
            sys.modules[package_name] = package
            spec = importlib.util.spec_from_file_location(
                package_name + "." + source.stem, source
            )
            module = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = module
            spec.loader.exec_module(module)
            getattr(module, function)(registry)
        registry.owners[name] = {
            "descriptor": str(
                descriptor.relative_to(compiler.value.source_root)
                if descriptor.is_relative_to(compiler.value.source_root)
                else descriptor
            ),
            "native": meta.get("native", []),
            "actions": sorted(set(registry.actions) - previous),
            "entry": meta.get("actions"),
            "services": sorted(set(registry.services) - provided),
            "skills": meta.get("skills", {}),
            "dependencies": meta.get("dependencies", []),
        }
        pending.remove(name)

    try:
        for name in entries:
            activate(name)
    except BaseException:
        registry.close()
        raise
    return registry


def snapshot_plugins(workspace, dest, descriptors):
    for rel in descriptors:
        src = (workspace / relative(rel)).parent
        out = (dest / relative(rel)).parent

        def ignore(folder, names):
            return {
                n
                for n in names
                if n in {".git", ".work", "build", "__pycache__"}
                or dest.is_relative_to((Path(folder) / n).resolve())
            }

        shutil.copytree(src, out, ignore=ignore, dirs_exist_ok=True, symlinks=True)
