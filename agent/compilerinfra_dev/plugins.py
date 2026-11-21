"""Source plugin declarations, activated once per compiler candidate."""

import inspect
import importlib.util
import hashlib
import sys
from contextlib import ExitStack
from uuid import uuid4
from typing import get_type_hints

from pydantic import ConfigDict, create_model

from compilerinfra.tree import relative


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
                if k.startswith(self.namespace + "_"):
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


def installed(compiler):
    """Resolve every product asset from the embedded installation manifest."""
    registry = Actions(compiler)
    description = compiler.describe()
    root = compiler.library.path.parent / (description["product"]["name"] + ".assets")
    try:
        for entry in description["assets"]:
            rel = relative(entry["path"])
            src = (root / rel).resolve(strict=True)
            if (
                not src.is_relative_to(root)
                or hashlib.sha256(src.read_bytes()).hexdigest() != entry["sha256"]
            ):
                raise ValueError(
                    f"{src}: installed asset differs from product manifest"
                )
            if src.suffix == ".md":
                registry.skills[str(rel)] = src
            elif src.name == "actions.py":
                ns = registry.namespace + "_" + str(len(registry.owners))
                spec = importlib.util.spec_from_file_location(
                    ns, src, submodule_search_locations=[str(src.parent)]
                )
                module = importlib.util.module_from_spec(spec)
                sys.modules[ns] = module
                spec.loader.exec_module(module)
                module.register(registry)
                registry.owners[str(rel)] = str(src)
        return registry
    except BaseException:
        registry.close()
        raise
