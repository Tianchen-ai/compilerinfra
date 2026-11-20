"""Call the IntentDSL Bench product CLI with the selected compiler installation."""

from pathlib import Path
from compilerinfra_dev.bench import settings, output
from compilerinfra_dev.source import relative
from compilerinfra_dev.process import execute


def register(registry):
    compiler = registry.compiler

    @registry.action
    async def measure_intent(
        workload: str,
        environment: str,
        implementations: list[str],
        baseline: str,
        case: str,
        output_name: str,
    ) -> dict:
        """Run IntentDSL Bench on a registered case using this candidate's product compiler."""
        cfg = settings(compiler, "intentdsl")
        if not implementations or baseline not in implementations:
            raise ValueError("select a baseline among the measured implementations")
        if not any(v.startswith("intent:") for v in implementations):
            raise ValueError("select an Intent implementation for the candidate")
        value = compiler.value
        binary = (
            value.install / relative(cfg["installed_compiler"])
            if value.install
            else Path(cfg["base_compiler"])
        ).resolve(strict=True)
        # Intent Bench starts the compiler itself. Give it the preset-bound product entry.
        binary = compiler.executable(binary, wrapper_name="intent-bench-compiler")
        dest = output(cfg, output_name)
        argv = [
            cfg["executable"],
            "run",
            workload,
            "--env",
            environment,
            "--impl",
            ",".join(implementations),
            "--baseline",
            baseline,
            "--case",
            case,
            "--compiler",
            str(binary),
            "--output-root",
            str(dest),
        ]
        stdout, diag = await execute(argv, compiler.scope.workspace)
        return {
            "compiler": value.name,
            "output": str(dest),
            "stdout": stdout.decode(),
            "diagnostics": diag,
        }
