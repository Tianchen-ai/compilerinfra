"""One spawned process owns one native build and all its configuration handles."""

import asyncio
import multiprocessing
import os
import sys
from compilerinfra import Compiler
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
                        Compiler(path, args["configuration"])
                        if op == "open"
                        else handles[args["parent"]].derive(args["configuration"])
                    )
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
                    result = handles[key].compile(**args)
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
        ctx = multiprocessing.get_context("spawn")
        self.pipe, child = ctx.Pipe()
        self.process = ctx.Process(
            target=serve, args=(child, str(library), profile), daemon=True
        )
        self.process.start()
        child.close()

    def call(self, op, key, **args):
        self.pipe.send((op, key, args))
        try:
            ok, result = self.pipe.recv()
        except EOFError as exc:
            raise RuntimeError(f"compiler worker for {self.library} exited") from exc
        if not ok:
            raise ValueError(result)
        return result

    def close(self):
        if self.process.is_alive():
            self.pipe.send(("close", "", {}))
        self.pipe.close()
        self.process.join()
