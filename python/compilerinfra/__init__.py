"""In-process binding for an installed product compilation library."""
import ctypes as C
import json
from pathlib import Path


class Buffer(C.Structure):
    _fields_ = [("data", C.c_void_p), ("size", C.c_size_t)]


class Library:
    def __init__(self, path):
        self.path = Path(path).resolve(strict=True)
        self.api = C.CDLL(str(self.path))
        ptr, string = C.c_void_p, C.c_char_p
        buf = C.POINTER(Buffer)
        signatures = {
            "ci_abi_version": (C.c_uint32, []),
            "ci_open": (ptr,[string,C.POINTER(ptr)]),
            "ci_derive": (ptr,[ptr,string,C.POINTER(ptr)]),
            "ci_describe": (ptr,[ptr,buf]),
            "ci_compile": (ptr,[ptr,string,ptr,C.c_size_t,string,string,C.c_int,buf,buf]),
            "ci_close": (None,[ptr]), "ci_free": (None,[ptr]),
        }
        for name,(result,args) in signatures.items():
            fn = getattr(self.api,name)
            fn.restype,fn.argtypes = result,args
        if self.api.ci_abi_version() != 1:
            raise ValueError(f"unsupported compiler ABI: {path}")

    def check(self, error):
        if error:
            try:
                raise ValueError(C.string_at(error).decode())
            finally:
                self.api.ci_free(error)

    def take(self, buffer):
        try:
            return C.string_at(buffer.data,buffer.size)
        finally:
            self.api.ci_free(buffer.data)


class Compiler:
    def __init__(self, library, configuration=None):
        self.library = Library(library)
        self.handle = C.c_void_p()
        self.library.check(self.library.api.ci_open(
            json.dumps(configuration or {}).encode(), C.byref(self.handle)))

    def derive(self, configuration):
        child = object.__new__(Compiler)
        child.library,child.handle = self.library,C.c_void_p()
        self.library.check(self.library.api.ci_derive(
            self.handle,json.dumps(configuration).encode(),C.byref(child.handle)))
        return child

    def describe(self):
        out = Buffer()
        self.library.check(self.library.api.ci_describe(self.handle,C.byref(out)))
        return json.loads(self.library.take(out))

    def compile(self, source: bytes, *, exit: str, through=None, after=None, text=False, explain=False):
        out, events = Buffer(), Buffer()
        self.library.check(self.library.api.ci_compile(
            self.handle,exit.encode(),source,len(source),after and after.encode(),
            through and through.encode(),text,C.byref(out),C.byref(events) if explain else None))
        artifact = self.library.take(out)
        return (artifact,json.loads(self.library.take(events))) if explain else artifact

    def close(self):
        if getattr(self,"handle",None):
            self.library.api.ci_close(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self,*exc):
        self.close()

    def __del__(self):
        self.close()
