"""Canonical action catalog used by every host."""

import base64
import difflib
from pathlib import Path
from typing import Any
from . import inspection, scaffold
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

        def inspect_compiler(
            compiler: str = "base",
            view: str | None = None,
            exit: str | None = None,
            records: str | None = None,
        ):
            """Inspect the installed compiler, or read-only 'parity' / 'readiness' views."""
            if view == "parity":
                return inspection.build_parity_report(
                    scope, compiler=compiler, exit_filter=exit, records=records
                )
            if view == "readiness":
                return inspection.build_readiness_report(
                    scope, compiler=compiler, exit_filter=exit, records=records
                )
            if view is not None:
                raise ValueError(f"unknown inspect view {view!r}; expected 'parity' or 'readiness'")
            return dict(
                layout(compiler),
                source_locations=scope.source_locations(compiler),
                **self.plugins(compiler),
            )

        def inspect_parity(
            compiler: str = "base",
            exit: str | None = None,
            records: str | None = None,
        ):
            """Read-only parity report across node capabilities and sibling exits."""
            return inspection.build_parity_report(
                scope, compiler=compiler, exit_filter=exit, records=records
            )

        def inspect_readiness(
            compiler: str = "base",
            exit: str | None = None,
            records: str | None = None,
        ):
            """Read-only per-exit readiness summary: v0.5 coverage, calibration, holdout accuracy and provenance."""
            return inspection.build_readiness_report(
                scope, compiler=compiler, exit_filter=exit, records=records
            )

        def compile_ir(
            input: str,
            exit: str,
            compiler: str = "base",
            through: str | None = None,
            after: str | None = None,
            output: str | None = None,
            author_input: bool = False,
            root_sha256: str | None = None,
        ):
            """Compile root IR or resume bytecode at a named boundary."""
            artifact, cid = scope.compile(
                compiler,
                Path(input).read_bytes(),
                exit,
                through,
                after,
                author_input=author_input,
                root_sha256=root_sha256,
            )
            if output:
                Path(output).write_bytes(artifact)
                return {"compilation_id": cid, "output": output, "bytes": len(artifact)}
            return {
                "compilation_id": cid,
                "base64": base64.b64encode(artifact).decode(),
            }

        action("layout", layout)
        action("inspect", inspect_compiler)
        action("inspect-parity", inspect_parity)
        action("inspect-readiness", inspect_readiness)
        def doctor(compiler: str = "base"):
            """Probe this installed compiler's optional target dependencies."""
            key = scope.key(compiler)
            return scope.values[key].worker.call("doctor", key)
        action("doctor", doctor)
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

        def explain(compilation_id: str):
            """Read the transformation events for this compilation."""
            return scope.observations[compilation_id][0]

        def compare(
            input: str, exit: str, through: str, compiler: str, baseline: str = "base"
        ):
            """Compare two candidates at the same representation boundary."""

            def contract(id):
                desc = layout(id)
                owner = through.partition("@")[0]
                if owner in desc["nodes"]:
                    return ("node", desc["nodes"][owner]["contract"])
                return (
                    "exit",
                    desc["exits"][owner]["at"],
                    desc["exits"][owner]["description"],
                )

            if contract(compiler) != contract(baseline):
                raise ValueError(f"different representation contracts at {through}")
            data = Path(input).read_bytes()
            results = [
                scope.compile(id, data, exit, through) for id in (baseline, compiler)
            ]
            a, b = [
                artifact.decode().splitlines(keepends=True) for artifact, _ in results
            ]
            return {
                "compilation_ids": [cid for _, cid in results],
                "changed": a != b,
                "diff": "".join(difflib.unified_diff(a, b, baseline, compiler)),
            }

        def impact(
            component: str,
            compiler: str | None = None,
            compilation_id: str | None = None,
        ):
            """Inspect reachability, configuration selection and hits for a compilation ID."""
            return scope.impact(compiler, component, compilation_id)

        def read_skill(name: str, compiler: str = "base"):
            """Read a framework or installed product skill."""
            if name in {"compiler-development", "performance-iteration"}:
                return (Path(__file__).parent / "skills" / (name + ".md")).read_text()
            key = scope.key(compiler)
            return scope.values[key].worker.call("skill", key, name=name)

        def model_accuracy(records: str, k: int = 1):
            """Compare structural predictions to Bench measurements, separated by data split."""
            import json
            from compilerinfra.performance import accuracy

            return accuracy(json.loads(Path(records).read_text()), k)

        async def measure(
            workload: Any,
            output_dir: str,
            compiler: str = "base",
            compilation_id: str | None = None,
        ):
            """Hand this installed candidate and the opaque workload to the product's Bench."""
            return await scope.measure(compiler, workload, output_dir, compilation_id)

        def adopt(compiler: str):
            """Apply the candidate to the product sources, rebuild and reinstall the baseline."""
            return scope.adopt(compiler)

        def discard(compiler: str):
            """Release one candidate handle and any build it alone owns."""
            return scope.discard(compiler)

        def revert():
            """Restore the sources and installation that preceded the latest adoption."""
            return scope.revert()

        for fn in [
            derive,
            explain,
            compare,
            impact,
            read_skill,
            model_accuracy,
            measure,
            adopt,
            revert,
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
            catalog["skills"].extend(("compiler-development", "performance-iteration"))
        return catalog

    def schemas(self):
        result = self.builtins.describe()["actions"]
        variants = {}
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
                selection = {"type": "string", "enum": [key]}
                if key == self.scope.base_key:
                    selection["enum"].append("base")
                    selection["default"] = "base"
                else:
                    schema.setdefault("required", []).append("compiler")
                schema["properties"]["compiler"] = selection
                variants.setdefault(name, []).append(entry)
        for name, entries in variants.items():
            result[name] = (
                entries[0]
                if len(entries) == 1
                else {
                    "description": entries[0]["description"],
                    "parameters": {
                        "type": "object",
                        "oneOf": [e["parameters"] for e in entries],
                    },
                }
            )
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
