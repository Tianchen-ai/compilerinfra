"""Public operations shared by MCP, the DSH scope and the headless CLI."""

from typing import Any, Literal, Optional, get_type_hints
import inspect
from functools import wraps

from .plugins import Actions


def edits(enable, disable, replace):
    options = []
    for name in enable or ():
        options += ["--enable", name]
    for name in disable or ():
        options += ["--disable", name]
    for name, pipeline in (replace or {}).items():
        options += ["--replace", f"{name}={pipeline}"]
    return options


def excerpt(baseline, candidate, shared, through):
    result = {
        "shared_through": shared,
        "through": through,
        "changed": baseline != candidate,
    }
    if result["changed"]:
        left, right = baseline.splitlines(keepends=True), candidate.splitlines(
            keepends=True
        )
        line = next(
            (i for i, (a, b) in enumerate(zip(left, right)) if a != b),
            min(len(left), len(right)),
        )
        result.update(
            first_line=line + 1,
            baseline_excerpt="".join(left[line : line + 6])[:4096],
            candidate_excerpt="".join(right[line : line + 6])[:4096],
        )
    return result


def catalog(scope):
    actions = Actions(None)

    @actions.action
    async def components(
        kind: Optional[str] = None,
        contains: str = "",
        file: Optional[str] = None,
        compiler: str = "base",
    ) -> list[dict[str, Any]]:
        """Find registered components and their source locations."""
        async with scope.use(compiler) as c:
            items = await c.query("--components")
        return [
            i
            for i in items
            if contains in i["id"]
            and (kind is None or i["id"].partition(":")[0] == kind)
            and (file is None or i["source"]["file"] == file)
        ]

    @actions.action
    async def plan(target: str, compiler: str = "base") -> dict[str, Any]:
        """Inspect the actual route, strategies, realizers and optimization slots."""
        async with scope.use(compiler) as c:
            return await c.query("--plan", target)

    @actions.action
    async def impact(component: str, compiler: str = "base") -> dict[str, Any]:
        """Find eligible targets for a kind:name and distinguish off-route targets."""
        async with scope.use(compiler) as c:
            return await c.query("--impact", component)

    @actions.action
    async def inspect_compiler(compiler: str = "base") -> dict[str, Any]:
        """Inspect this definition, its parent, plugin actions, services and skills."""
        async with scope.use(compiler) as c:
            return {
                "compiler": compiler,
                "parent": c.value.parent,
                "preset": c.value.preset,
                "targets": await c.query("--list"),
                **scope.plugins[compiler].describe(),
            }

    @actions.action
    async def compile_program(
        target: str,
        source: str,
        output: str,
        after: Optional[str] = None,
        through: Optional[str] = None,
        encoding: Literal["asm", "bytecode"] = "asm",
        enable: Optional[list[str]] = None,
        disable: Optional[list[str]] = None,
        replace: Optional[dict[str, str]] = None,
        compiler: str = "base",
    ) -> dict[str, Any]:
        """Compile through the candidate's normal entry, optionally resume/stop at a verified boundary."""
        async with scope.use(compiler) as c:
            path, dest = c.source(source), c.output(output)
            if path == dest:
                raise ValueError("input and output must be distinct")
            args = ["--target", target, "-o", str(dest)]
            if after:
                args += ["--after", after]
            if through:
                args += ["--through", through, "--format", encoding]
            _, diagnostic = await c.run(
                *args, *edits(enable, disable, replace), "--", str(path)
            )
            return {
                "compiler": compiler,
                "target": target,
                "through": through,
                "path": str(dest),
                "bytes": dest.stat().st_size,
                "diagnostics": diagnostic,
            }

    @actions.action
    async def read_skill(skill: str, compiler: str = "base") -> str:
        """Load one skill from the active compiler's plugin scope."""
        async with scope.use(compiler):
            return scope.plugins[compiler].skills[skill].read_text()

    @actions.action
    async def call_action(
        name: str, arguments: dict[str, Any], compiler: str = "base"
    ) -> Any:
        """Execute a typed plugin action listed by inspect_compiler on the chosen compiler."""
        async with scope.use(compiler):
            return await scope.plugins[compiler].invoke(name, arguments)

    if scope.bundle == "runtime":
        return bind_plugins(actions, scope)

    @actions.action
    async def derive(
        parent: str = "base",
        preset: Optional[dict[str, Any]] = None,
        source_patch: str = "",
    ) -> dict[str, Any]:
        """Derive a new compiler from a product preset and/or a Git source patch."""
        return await scope.derive(parent, preset or {}, source_patch)

    @actions.action
    async def adopt(compiler: str) -> dict[str, Any]:
        """Apply the candidate's cumulative changes to the product working tree without staging or committing."""
        return await scope.adopt(compiler)

    @actions.action
    async def discard(compiler: str) -> dict[str, Any]:
        """Release a candidate and its scoped registrations; descendants retain their installation."""
        return await scope.discard(compiler)

    @actions.action
    async def pipeline(
        target: str,
        enable: Optional[list[str]] = None,
        disable: Optional[list[str]] = None,
        replace: Optional[dict[str, str]] = None,
        compiler: str = "base",
    ) -> str:
        """Print the assembled native MLIR pass pipelines without executing an input."""
        async with scope.use(compiler) as c:
            text, _ = await c.run(
                "--target", target, "--pipeline", *edits(enable, disable, replace)
            )
            return text.decode()

    @actions.action
    async def explain(
        target: str, source: str, compiler: str = "base", through: Optional[str] = None
    ) -> dict[str, Any]:
        """Compile and report observed decision matches, preserved values and realizations."""
        async with scope.use(compiler) as c:
            return await c.query(
                "--explain",
                "--target",
                target,
                *(["--through", through] if through else []),
                "--",
                str(c.source(source)),
            )

    @actions.action
    async def compare_ir(
        target: str,
        source: str,
        through: str,
        after: Optional[str] = None,
        enable: Optional[list[str]] = None,
        disable: Optional[list[str]] = None,
        replace: Optional[dict[str, str]] = None,
        compiler: str = "base",
    ) -> dict[str, Any]:
        """Compare this compiler's ordinary route against slot edits from a shared checkpoint."""
        async with scope.use(compiler) as c:
            pair = await c.query(
                "--compare-ir",
                "--target",
                target,
                "--through",
                through,
                *(["--after", after] if after else []),
                *edits(enable, disable, replace),
                "--",
                str(c.source(source)),
            )
            return excerpt(
                pair["baseline"], pair["candidate"], pair["shared_through"], through
            )

    @actions.action
    async def compare(
        parent: str, candidate: str, target: str, source: str, through: str
    ) -> dict[str, Any]:
        """Compare two compiler definitions; independent native builds both receive the original input."""
        async with scope.use(parent) as a, scope.use(candidate) as b:
            path = a.source(source)
            args = ["--target", target, "--through", through]
            if a.value.driver == b.value.driver:
                empty = scope.private / "empty-preset.json"
                if not empty.exists():
                    empty.write_text("{}")
                pair = await a.query(
                    "--compare-ir",
                    "--against-preset",
                    str(b.value.preset_file or empty),
                    *args,
                    "--",
                    str(path),
                )
                return excerpt(
                    pair["baseline"],
                    pair["candidate"],
                    pair["shared_through"] or None,
                    through,
                )

            async def boundary(compiler):
                route = await compiler.query("--plan", target)
                if through == "input":
                    return route.get("input_model")
                for stage in route["stages"]:
                    if stage["boundary"] == through:
                        return (
                            stage.get("component", {}).get("declaration", {}).get("to")
                        )
                raise ValueError(f"{target} has no boundary {through}")

            if await boundary(a) != await boundary(b):
                raise ValueError(f"different output models at {target}/{through}")
            data = path.read_bytes()
            x, _ = await a.run(*args, "--format", "asm", "-", data=data)
            y, _ = await b.run(*args, "--format", "asm", "-", data=data)
            return excerpt(x.decode(), y.decode(), None, through)

    return bind_plugins(actions, scope)


def bind_plugins(actions, scope):
    def project(name, fn):
        @wraps(fn)
        async def dispatch(*, compiler="base", **kwargs):
            async with scope.use(compiler):
                return await scope.plugins[compiler].invoke(name, kwargs)

        sig = inspect.signature(fn)
        if "compiler" in sig.parameters or name in actions.actions:
            raise ValueError(f"plugin action collides with a framework binding: {name}")
        hints = get_type_hints(fn)
        dispatch.__signature__ = sig.replace(
            parameters=[
                *(p.replace(annotation=hints[p.name]) for p in sig.parameters.values()),
                inspect.Parameter(
                    "compiler",
                    inspect.Parameter.KEYWORD_ONLY,
                    default="base",
                    annotation=str,
                ),
            ],
            return_annotation=hints.get("return", sig.return_annotation),
        )
        dispatch.__annotations__ = {**hints, "compiler": str}
        actions.action(dispatch)

    for name, (fn, _) in scope.plugins["base"].actions.items():
        project(name, fn)
    return actions
