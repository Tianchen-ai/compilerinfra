"""In-process binding for an installed product compilation library."""

import ctypes as C
import json
from .modules import ProductModules
from .observations import Observations
from pathlib import Path


class Buffer(C.Structure):
    _fields_ = [("data", C.c_void_p), ("size", C.c_size_t)]


class CompilationError(ValueError):
    def __init__(self, report):
        self.report = report
        super().__init__(f"{report['compilation_id']}: {report['error']}")


class Library:
    def __init__(self, path):
        self.path = Path(path).resolve(strict=True)
        self.api = C.CDLL(str(self.path))
        ptr, string = C.c_void_p, C.c_char_p
        buf = C.POINTER(Buffer)
        signatures = {
            "ci_abi_version": (C.c_uint32, []),
            "ci_open": (ptr, [string, C.POINTER(ptr)]),
            "ci_derive": (ptr, [ptr, string, C.POINTER(ptr)]),
            "ci_describe": (ptr, [ptr, buf]),
            "ci_compile": (
                ptr,
                [ptr, string, ptr, C.c_size_t, string, string, C.c_int, buf, buf],
            ),
            "ci_close": (None, [ptr]),
            "ci_free": (None, [ptr]),
        }
        signatures["ci_compile_plain"] = signatures["ci_compile"]
        signatures["ci_compile_measured"] = (ptr, [ptr, string, ptr, C.c_size_t, buf, buf])
        signatures["ci_compile_runtime"] = signatures["ci_compile_measured"]
        signatures["ci_compile_trace"] = (
            ptr,
            [ptr, string, ptr, C.c_size_t, string, string, C.c_int, string, buf, buf],
        )
        for name, (result, args) in signatures.items():
            fn = getattr(self.api, name)
            fn.restype, fn.argtypes = result, args
        if self.api.ci_abi_version() != 1:
            raise ValueError(f"unsupported compiler ABI: {path}")

    def __del__(self):
        if getattr(self, "modules", None):
            self.modules.close()

    def check(self, error):
        if error:
            try:
                raise ValueError(C.string_at(error).decode())
            finally:
                self.api.ci_free(error)

    def take(self, buffer):
        try:
            return C.string_at(buffer.data, buffer.size)
        finally:
            self.api.ci_free(buffer.data)


class Compiler:
    def __init__(self, library, configuration=None):
        self.library = Library(library)
        self.observations = Observations()
        self.last_compilation_id = None
        self.handle = C.c_void_p()
        self.library.check(
            self.library.api.ci_open(
                json.dumps(configuration or {}).encode(), C.byref(self.handle)
            )
        )

        self.library.modules = ProductModules(self.library.path, self.describe())

    def derive(self, configuration):
        child = object.__new__(Compiler)
        child.observations = Observations()
        child.last_compilation_id = None
        child.library, child.handle = self.library, C.c_void_p()
        self.library.check(
            self.library.api.ci_derive(
                self.handle, json.dumps(configuration).encode(), C.byref(child.handle)
            )
        )
        return child

    def describe(self):
        out = Buffer()
        self.library.check(self.library.api.ci_describe(self.handle, C.byref(out)))
        return json.loads(self.library.take(out))

    def root_ir(self, author_input):
        """Run the installed product's frontend and return root IR bytes."""
        return self.library.modules(author_input)

    def runtime(self, owner, module=""):
        """Import a runtime module from this compiler installation."""
        return self.library.modules.runtime(owner, module)

    def compile(
        self,
        source: bytes,
        *,
        exit: str,
        through=None,
        after=None,
        text=False,
        explain=False,
        root_sha256=None,
    ):
        out, events = Buffer(), Buffer()
        if not explain:
            error = self.library.api.ci_compile_plain(
                self.handle,
                exit.encode(),
                source,
                len(source),
                after and after.encode(),
                through and through.encode(),
                text,
                C.byref(out),
                C.byref(events),
            )
            self.last_compilation_id = (
                self.library.take(events).decode() if events.data else None
            )
            self.library.check(error)
            return self.library.take(out)
        error = self.library.api.ci_compile_trace(
            self.handle,
            exit.encode(),
            source,
            len(source),
            after and after.encode(),
            through and through.encode(),
            text,
            root_sha256 and root_sha256.encode(),
            C.byref(out),
            C.byref(events),
        )
        if not events.data:
            self.library.check(error)
        report = json.loads(self.library.take(events))
        self.last_compilation_id = report["compilation_id"]
        self.observations[self.last_compilation_id] = report
        if error:
            self.library.api.ci_free(error)
            raise CompilationError(report)
        artifact = self.library.take(out)
        return (artifact, report) if explain else artifact

    def compile_runtime(self, source: bytes, *, exit):
        """Publish a launch receipt without collecting or retaining observations."""
        out, receipt = Buffer(), Buffer()
        self.library.check(self.library.api.ci_compile_runtime(
            self.handle, exit.encode(), source, len(source), C.byref(out), C.byref(receipt)))
        identity = json.loads(self.library.take(receipt))
        self.last_compilation_id = identity["compilation_id"]
        return self.library.take(out), identity

    def compile_measured(self, source: bytes, *, exit):
        """Emit a self-identifying artifact and the independent native explain record."""
        out, events = Buffer(), Buffer()
        error = self.library.api.ci_compile_measured(
            self.handle, exit.encode(), source, len(source), C.byref(out), C.byref(events)
        )
        report = json.loads(self.library.take(events)) if events.data else {}
        if error:
            self.library.check(error)
        self.last_compilation_id = report["compilation_id"]
        self.observations[self.last_compilation_id] = report
        return self.library.take(out), report

    def explain(self, compilation_id):
        return self.observations[compilation_id]

    def close(self):
        if getattr(self, "handle", None):
            self.library.api.ci_close(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        self.close()
