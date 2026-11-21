"""Canonical action catalog used by every host."""

import base64
import difflib
from pathlib import Path
from typing import Any
from . import scaffold
from .plugins import Actions


class Catalog:
    def __init__(self, scope):
        self.scope = scope
        self.builtins = Actions(None)

        def action(name, fn):
            self.builtins.action(fn)
            self.builtins.actions[name] = self.builtins.actions.pop(fn.__name__)

        def layout(compiler: str = "base"):
            """Describe the linked tree, native owners, source locations and actual configuration."""
            return scope.values[scope.key(compiler)].description

        def inspect_compiler(compiler: str = "base"):
            """Inspect the installed compiler and its scoped development assets."""
            return dict(layout(compiler), **self.plugins(compiler))

        def compile_ir(
            input: str,
            exit: str,
            compiler: str = "base",
            through: str | None = None,
            after: str | None = None,
            output: str | None = None,
            author_input: bool = False,
        ):
            """Compile root IR or resume bytecode at a named boundary."""
            artifact = scope.compile(
                compiler,
                Path(input).read_bytes(),
                exit,
                through,
                after,
                author_input=author_input,
            )
            if output:
                Path(output).write_bytes(artifact)
                return {"output": output, "bytes": len(artifact)}
            return {"base64": base64.b64encode(artifact).decode()}

        action("layout", layout)
        action("inspect", inspect_compiler)
        action("compile", compile_ir)
        if scope.profile == "runtime":
            return

        def derive(
            configuration: dict | None = None,
            patch: str = "",
            compiler: str = "base",
            extension_patches: dict[str, str] | None = None,
        ):
            """Derive a compiler configuration or build an independent patched source candidate."""
            return scope.derive(compiler, configuration or {}, patch, extension_patches)

        def explain(
            input: str, exit: str, compiler: str = "base", through: str | None = None
        ):
            """Collect the strategy and realization events produced by this input."""
            scope.compile(
                compiler, Path(input).read_bytes(), exit, through, observe=True
            )
            return scope.values[scope.key(compiler)].events

        def compare(
            input: str, exit: str, through: str, compiler: str, baseline: str = "base"
        ):
            """Compare two candidates at the same representation boundary."""
            node = through.partition("@")[0]
            if (
                layout(compiler)["nodes"][node]["contract"]
                != layout(baseline)["nodes"][node]["contract"]
            ):
                raise ValueError(f"different representation contracts at {through}")
            data = Path(input).read_bytes()
            a, b = [
                scope.compile(id, data, exit, through)
                .decode()
                .splitlines(keepends=True)
                for id in (baseline, compiler)
            ]
            return {
                "changed": a != b,
                "diff": "".join(difflib.unified_diff(a, b, baseline, compiler)),
            }

        def impact(component: str, compiler: str = "base"):
            """Inspect reachability, configuration selection and hits in the last observed input."""
            return scope.impact(compiler, component)

        def read_skill(name: str, compiler: str = "base"):
            """Read a framework or installed product skill."""
            if name == "compiler-development":
                return (
                    Path(__file__).parent / "skills" / "compiler-development.md"
                ).read_text()
            key = scope.key(compiler)
            return scope.values[key].worker.call("skill", key, name=name)

        async def measure(workload: Any, output_dir: str, compiler: str = "base"):
            """Hand this installed candidate and the opaque workload to the product's Bench."""
            return await scope.measure(compiler, workload, output_dir)

        def adopt(compiler: str):
            """Apply the candidate to the product sources, rebuild and reinstall the baseline."""
            return scope.adopt(compiler)

        def discard(compiler: str):
            """Release one candidate handle and any build it alone owns."""
            return scope.discard(compiler)

        for fn in [
            derive,
            explain,
            compare,
            impact,
            read_skill,
            measure,
            adopt,
            discard,
            scaffold.init,
            scaffold.add_level,
            scaffold.add_node,
            scaffold.add_exit,
            scaffold.add_runtime,
            scaffold.host_config,
        ]:
            action(fn.__name__.replace("_", "-"), fn)

    def plugins(self, id):
        key = self.scope.key(id)
        catalog = self.scope.values[key].worker.call("catalog", key)
        if self.scope.profile == "development":
            catalog["skills"].append("compiler-development")
        return catalog

    def schemas(self):
        result = self.builtins.describe()["actions"]
        for key in self.scope.values:
            for name, entry in self.plugins(key)["actions"].items():
                if name in self.builtins.actions:
                    raise ValueError(
                        f"installed action collides with SDK action {name}"
                    )
                schema = entry["parameters"]
                if "compiler" in schema["properties"]:
                    raise ValueError(
                        f"{name}: compiler is reserved for candidate selection"
                    )
                schema["properties"]["compiler"] = {"type": "string", "default": "base"}
                if name in result and result[name] != entry:
                    raise ValueError(f"candidate action schemas differ for {name}")
                result[name] = entry
        return result

    async def invoke(self, name, args):
        if name == "actions":
            return self.schemas()
        if name in self.builtins.actions:
            return await self.builtins.invoke(name, args)
        args = dict(args)
        key = self.scope.key(args.pop("compiler", "base"))
        return self.scope.values[key].worker.call(
            "action", key, name=name, arguments=args
        )
