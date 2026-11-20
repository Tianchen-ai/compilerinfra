"""Pass an exported candidate to Triton–Weft Bench through its artifact ABI."""

from pathlib import Path
from compilerinfra_dev.bench import settings, output
from compilerinfra_dev.process import execute


def register(registry):
    compiler = registry.compiler

    @registry.action
    async def measure_weft(
        workload: str, target: str, inputs: str, baseline: str, output_name: str
    ) -> dict:
        """Export this candidate through the product's public AOT API and run Triton–Weft Bench."""
        cfg = settings(compiler, "triton-weft")
        dest = output(cfg, output_name)
        # The product owns triton.compile/export and all ABI metadata.
        export = registry.services.get("triton-weft.export")
        if export is None:
            raise ValueError(
                "this product must bind triton-weft.export to its native AOT exporter"
            )
        artifact = Path(await export(compiler, workload, target, inputs)).resolve(
            strict=True
        )
        if not artifact.is_dir():
            raise ValueError("Triton–Weft export must return an AOT directory")
        argv = [
            cfg["executable"],
            "op",
            workload,
            "--target",
            target,
            "--inputs",
            inputs,
            "--weft",
            str(artifact),
            "--baseline-impl",
            baseline,
            "--output",
            str(dest),
        ]
        if cfg.get("config"):
            argv += ["--config", cfg["config"]]
        stdout, diag = await execute(argv, compiler.scope.workspace)
        return {
            "compiler": compiler.value.name,
            "artifact": str(artifact),
            "output": str(dest),
            "stdout": stdout.decode(),
            "diagnostics": diag,
        }
