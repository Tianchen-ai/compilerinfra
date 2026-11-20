"""Call the IntentDSL Bench product CLI with the selected compiler installation."""

from contextlib import ExitStack
import json
import os
from pathlib import Path
from tempfile import TemporaryDirectory
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
        root = cfg.get("output_root") or os.environ.get("INTENT_BENCH_OUTPUT_ROOT")
        if not root:
            raise ValueError("set Intent Bench output_root or INTENT_BENCH_OUTPUT_ROOT")
        dest = output({"output_root": root}, output_name)
        with ExitStack() as scope:
            worker = registry.services.get("intentdsl.bench.worker")
            command = [cfg["executable"]]
            flags = []
            if worker:
                data, _ = await execute(
                    [*command, "env", "show", environment], compiler.scope.workspace
                )
                worker_env = json.loads(data)
                if worker_env.get("host") or worker_env.get("transport") == "ssh":
                    raise ValueError(
                        "open the candidate scope on the execution host to measure its installed Python and native runtime"
                    )
                scratch = Path(
                    scope.enter_context(TemporaryDirectory(dir=compiler.scope.private))
                )
                python = await worker(compiler, worker_env.get("python"), scratch)
                worker_env.update(python=str(python), compiler=None, intent_root=None)
                environment = str(scratch / "environment.json")
                Path(environment).write_text(json.dumps(worker_env))
                command = [str(python), "-m", "intent_bench.cli"]
            else:
                binary = (
                    value.install / relative(cfg["installed_compiler"])
                    if value.install
                    else Path(cfg["base_compiler"])
                ).resolve(strict=True)
                flags = [
                    "--compiler",
                    str(
                        compiler.executable(
                            binary, wrapper_name="intent-bench-compiler"
                        )
                    ),
                ]
            argv = [
                *command,
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
                *flags,
                "--output-root",
                str(root),
                "--output",
                str(dest),
            ]
            stdout, diag = await execute(argv, compiler.scope.workspace)
        return {
            "compiler": value.name,
            "output": str(dest),
            "stdout": stdout.decode(),
            "diagnostics": diag,
        }
