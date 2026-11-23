"""One spawned process owns one native build and all its configuration handles."""

import asyncio
from multiprocessing import Pipe
from multiprocessing.connection import Connection
from pathlib import Path
import subprocess
import os
import sys
from compilerinfra import CompilationError, Compiler
from .plugins import installed


def serve(pipe, path, profile):
    # Keep product diagnostics off the host's stdio protocol.
    os.dup2(sys.stderr.fileno(), sys.stdout.fileno())
    handles, catalogs = {}, {}
    try:
        while True:
            op, key, args = pipe.recv()
            try:
                if op == "close":
                    return
                if op in {"open", "derive"}:
                    handles[key] = (
                        Compiler(path, args.get("configuration"))
                        if op == "open"
                        else handles[args["parent"]].derive(args["configuration"])
                    )
                    for delta in args.get("changes", []):
                        current = handles[key]
                        handles[key] = current.derive(delta)
                        current.close()
                    result = handles[key].describe()
                elif op == "activate" and profile == "development":
                    catalogs[key] = installed(handles[key])
                    result = None
                elif op == "discard":
                    if key in catalogs:
                        catalogs.pop(key).close()
                    handles.pop(key).close()
                    result = None
                elif op == "compile":
                    if args.pop("author_input", False):
                        args["source"] = handles[key].root_ir(args["source"])
                    try:
                        if args.get("explain") and not args.get("through") and not args.get("after"):
                            result = handles[key].compile_measured(args["source"], exit=args["exit"])
                        else:
                            result = handles[key].compile(**args)
                        if not args.get("explain"):
                            result = (result, handles[key].last_compilation_id)
                    except CompilationError as exc:
                        result = (None, exc.report)
                elif op == "doctor":
                    result = handles[key].doctor()
                elif op == "catalog":
                    result = (
                        catalogs[key].describe()
                        if key in catalogs
                        else {"actions": {}, "skills": []}
                    )
                elif op == "skill":
                    result = catalogs[key].skills[args["name"]].read_text()
                elif op == "action":
                    result = asyncio.run(
                        catalogs[key].invoke(args["name"], args["arguments"])
                    )
                else:
                    raise ValueError(f"unknown worker operation {op}")
                pipe.send((True, result))
            except Exception as exc:
                pipe.send((False, f"{type(exc).__name__}: {exc}"))
    except EOFError:
        pass
    finally:
        for catalog in catalogs.values():
            catalog.close()
        for compiler in handles.values():
            compiler.close()
        pipe.close()


class Worker:
    def __init__(self, library, profile):
        self.library = library
        interpreter = Path(str(library) + ".python").read_text().strip()
        self.interpreter = interpreter
        env = os.environ.copy()
        env.pop("PYTHONHOME", None)
        env.pop("PYTHONPATH", None)
        bin_dir = Path(interpreter).parent
        if (bin_dir.parent / "pyvenv.cfg").is_file():
            env["VIRTUAL_ENV"] = str(bin_dir.parent)
            env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
        self.pipe, child = Pipe()
        try:
            self.process = subprocess.Popen(
                [
                    interpreter,
                    "-m",
                    __name__,
                    str(child.fileno()),
                    str(library),
                    profile,
                ],
                pass_fds=(child.fileno(),),
                stdin=subprocess.DEVNULL,
                stdout=sys.stderr,
                env=env,
                cwd=Path(library).parent,
            )
        finally:
            child.close()

    def call(self, op, key, **args):
        try:
            self.pipe.send((op, key, args))
            ok, result = self.pipe.recv()
        except (EOFError, ConnectionError) as exc:
            raise RuntimeError(
                f"compiler worker for {self.library} exited using {self.interpreter}"
            ) from exc
        if not ok:
            raise ValueError(result)
        return result

    def close(self):
        try:
            if self.process.poll() is None:
                self.pipe.send(("close", "", {}))
        except BrokenPipeError:
            pass
        finally:
            self.pipe.close()
            self.process.wait()


if __name__ == "__main__":
    serve(Connection(int(sys.argv[1])), sys.argv[2], sys.argv[3])
