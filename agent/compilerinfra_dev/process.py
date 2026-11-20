import asyncio
import os
import signal
from pathlib import Path


async def execute(argv, cwd: Path, *, data: bytes | None = None, env=None):
    proc = await asyncio.create_subprocess_exec(
        *(str(x) for x in argv),
        cwd=cwd,
        env=env,
        start_new_session=True,
        stdin=(
            asyncio.subprocess.PIPE if data is not None else asyncio.subprocess.DEVNULL
        ),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        stdout, stderr = await proc.communicate(data)
    except asyncio.CancelledError:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        await proc.wait()
        raise
    if proc.returncode:
        raise ValueError(
            f"{argv[0]} exited with {proc.returncode}\n"
            + (stderr + stdout).decode(errors="replace")
        )
    return stdout, stderr.decode(errors="replace")
