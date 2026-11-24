# CompilerInfra 0.2

CompilerInfra builds MLIR-based product compilers from a declared compilation tree. A product owns its representations, boundaries, strategies and exits. The SDK links those units into a shared library. A Python JIT can keep the compiler in process, while development hosts can derive, inspect and run candidates from the same library.

### Install the SDK

```sh
cmake -S . -B /path/to/sdk-build -DMLIR_DIR=/path/to/lib/cmake/mlir \
  -DCMAKE_INSTALL_PREFIX=/path/to/sdk
cmake --build /path/to/sdk-build
cmake --install /path/to/sdk-build
python -m pip install ./python ./agent
export CMAKE_PREFIX_PATH=/path/to/sdk:/path/to/llvm
```

The native SDK exports `CompilerInfra::Core` for compilation units and `CompilerInfra::Runtime` for ABI consumers. The Python runtime package can be installed independently with `pip install ./python`. Install a product outside its source tree. Point the JIT at the installed product library.

```python
from compilerinfra import Compiler

with Compiler(library_path) as compiler:
    tree = compiler.describe()
    root_ir = compiler.root_ir(author_program)
    artifact = compiler.compile(root_ir, exit=exit_name)
```

Use `compilerinfra-dev actions` to discover scaffold actions before creating a product. Pass `--workspace`, `--library`, and `--output-dir` to work on an installed product. A CLI process with no positional action accepts JSON lines and retains its candidates until EOF. MCP (`compilerinfra-agent`) exposes the same catalog.

[Product tree](doc/organization.md) · [Component development](doc/components.md) · [Derivation and adoption](doc/derivation.md) · [Host integration](doc/plugins.md) · [Workspace](doc/workspace.md)
