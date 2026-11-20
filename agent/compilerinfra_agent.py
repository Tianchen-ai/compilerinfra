"""One agent entry point for any driver linked to CompilerInfra."""

import argparse
import asyncio
import json
from pathlib import Path
from typing import Any, Literal, Optional

from mcp.server.fastmcp import FastMCP


def server(
    driver: Path, workspace: Path, outputs: Path, args: tuple[str, ...]
) -> FastMCP:
    app = FastMCP("CompilerInfra")

    async def invoke(*options: str) -> tuple[str, str]:
        proc = await asyncio.create_subprocess_exec(
            str(driver),
            *args,
            *options,
            cwd=workspace,
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        try:
            stdout, stderr = await proc.communicate()
        except asyncio.CancelledError:
            if proc.returncode is None:
                proc.kill()
            await proc.wait()
            raise
        if proc.returncode:
            diagnostic = stderr.decode(errors="replace")
            raise ValueError(f"compiler exited with {proc.returncode}\n{diagnostic}")
        return stdout.decode(), stderr.decode(errors="replace")

    async def query(*options: str):
        stdout, _ = await invoke(*options)
        return json.loads(stdout)

    def input_path(source: str) -> Path:
        path = (workspace / source).resolve(strict=True)
        if not (path.is_relative_to(workspace) or path.is_relative_to(outputs)):
            raise ValueError("input must be in the workspace or the output directory")
        return path

    @app.tool(structured_output=True)
    async def components(
        kind: Optional[str] = None, contains: str = "", file: Optional[str] = None
    ) -> list[dict[str, Any]]:
        """Find registered models, lowerings, routes, providers or optimizations and their source locations."""
        items = await query("--components")
        return [
            item
            for item in items
            if contains in item["id"]
            and (kind is None or item["id"].partition(":")[0] == kind)
            and (file is None or item["source"]["file"] == file)
        ]

    @app.tool(structured_output=True)
    async def plan(target: str) -> dict[str, Any]:
        """Read a target's linked stages, extension points and ordered optimization slots before editing."""
        return await query("--plan", target)

    @app.tool(structured_output=True)
    async def impact(component: str) -> dict[str, Any]:
        """Find the source, applicable targets, missing features and off-route targets for a kind:name."""
        return await query("--impact", component)

    def edits(enable, disable, replace) -> list[str]:
        options = []
        for name in enable or ():
            options += ["--enable", name]
        for name in disable or ():
            options += ["--disable", name]
        for name, pipeline in (replace or {}).items():
            options += ["--replace", f"{name}={pipeline}"]
        return options

    @app.tool()
    async def pipeline(
        target: str,
        enable: Optional[list[str]] = None,
        disable: Optional[list[str]] = None,
        replace: Optional[dict[str, str]] = None,
    ) -> str:
        """Build and print the actual MLIR pipelines with these edits, without compiling an input."""
        stdout, _ = await invoke(
            "--target", target, "--pipeline", *edits(enable, disable, replace)
        )
        return stdout

    @app.tool(structured_output=True)
    async def compare_ir(
        target: str,
        source: str,
        through: str,
        after: Optional[str] = None,
        enable: Optional[list[str]] = None,
        disable: Optional[list[str]] = None,
        replace: Optional[dict[str, str]] = None,
    ) -> dict[str, Any]:
        """Compare default and edited assembly from the same bytecode checkpoint. Return a bounded excerpt at the first differing line. This does not time kernels."""
        options = ["--compare-ir", "--target", target, "--through", through]
        if after is not None:
            options += ["--after", after]
        pair = await query(
            *options, *edits(enable, disable, replace), "--", str(input_path(source))
        )
        baseline, candidate = pair["baseline"], pair["candidate"]
        result = {
            "shared_through": pair["shared_through"],
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

    @app.tool(structured_output=True)
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
    ) -> dict[str, Any]:
        """Run the real product pipeline. Write an artifact or a stage checkpoint to the configured output directory. Resume by supplying a checkpoint and its 'after' boundary."""
        path = input_path(source)
        dest = (outputs / output).resolve()
        if not dest.is_relative_to(outputs) or dest == outputs or dest == path:
            raise ValueError("choose a distinct file inside the output directory")
        options = ["--target", target, "-o", str(dest)]
        if after is not None:
            options += ["--after", after]
        if through is not None:
            options += ["--through", through, "--format", encoding]
        options += edits(enable, disable, replace)
        dest.parent.mkdir(parents=True, exist_ok=True)
        _, diagnostics = await invoke(*options, "--", str(path))
        return {
            "target": target,
            "through": through,
            "path": str(dest),
            "bytes": dest.stat().st_size,
            "diagnostics": diagnostics,
        }

    return app


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--driver-arg", action="append", default=[])
    config = parser.parse_args()
    server(
        config.driver.resolve(strict=True),
        config.workspace.resolve(strict=True),
        config.output_dir.resolve(),
        tuple(config.driver_arg),
    ).run(transport="stdio")


if __name__ == "__main__":
    main()
