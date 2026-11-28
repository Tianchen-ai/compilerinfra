# CompilerInfra 0.6.0

CompilerInfra builds MLIR-based product compilers from a declared compilation tree. A product owns its representations, boundaries, strategies and exits. The SDK links those units into a shared library. A Python JIT can keep the compiler in process, while development hosts can derive, inspect and run candidates from the same library.

SDK 0.6.0 records the reference and origin of resolved numerical settings for product inspection and readiness. [Changelog](CHANGELOG.md) · [IntentDSL target contracts](https://github.com/Tianchen-ai/Intent/blob/main/doc/design/kir.md)

### Install the SDK

From the checkout, with Python ≥3.10, Ninja, CMake, a C++17 compiler and LLVM/MLIR 20.1.8 PIC development archives installed:

```sh
python3 -m venv .venv
. .venv/bin/activate
LLVM_PREFIX=$(llvm-config-20 --prefix)
cmake -S . -B build/sdk -G Ninja -DMLIR_DIR="$LLVM_PREFIX/lib/cmake/mlir" \
  -DCMAKE_BUILD_TYPE=Release -DCOMPILERINFRA_LINK_MLIR_DYLIB=OFF -DCMAKE_INSTALL_PREFIX="$PWD/sdk"
cmake --build build/sdk --parallel 8
cmake --install build/sdk
python -m pip install ./python ./agent
export CMAKE_PREFIX_PATH="$PWD/sdk:$LLVM_PREFIX"
compilerinfra-dev actions
```

The native SDK exports `CompilerInfra::Core` for compilation units and `CompilerInfra::Runtime` for ABI consumers. The Python runtime package can be installed independently with `pip install ./python`. Install a product outside its source tree. Point the JIT at the installed product library.

To build a product, run its package installation in the same environment. [IntentDSL’s complete source-to-device example](https://github.com/Tianchen-ai/Intent#build-from-source) uses this SDK, then checks a user-defined kernel against Torch and measures it with Intent-Bench.

Use `compilerinfra-dev actions` to discover scaffold actions before creating a product. Pass `--workspace`, `--library`, and `--output-dir` to work on an installed product. A CLI process with no positional action accepts JSON lines and retains its candidates until EOF. MCP (`compilerinfra-agent`) exposes the same catalog.

[Product tree](doc/organization.md) · [Component development](doc/components.md) · [Derivation and adoption](doc/derivation.md) · [Host integration](doc/plugins.md) · [Workspace](doc/workspace.md)
