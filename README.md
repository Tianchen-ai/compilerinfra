# compilerinfra

compilerinfra 是为 coding agent 设计的可派生 MLIR 编译器框架。给一台已链接的 `Compiler` 一个作者程序，它生成目标产物。给同一台编译器一项策略、实现或源码改动，它派生出另一台完整的编译器。父、子可以同时编译同一输入，也可以继续派生。接受的改动进入产品源码和默认组合，后续普通 JIT 会使用它。

产品第一次接入时，从[装配与组件开发](doc/components.md)开始。要改变已有产品的生成方式，见[决定、候选与采用](doc/derivation.md)。要让 agent 在 CLI、MCP 或 DSH 里实际操作编译器、连接独立 Bench，见[插件和宿主手册](doc/plugins.md)。

## 构建与安装

需要 C++17、CMake 3.20 及以上，以及带开发头文件和 CMake 配置的 LLVM/MLIR 20 SDK。将两个目录替换为本机 SDK 和期望安装位置。

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/llvm-20 \
  -DCMAKE_INSTALL_PREFIX=/path/to/compilerinfra \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build
```

产品的 CMake 工程用 `find_package(CompilerInfra REQUIRED CONFIG)` 查找安装包。将框架安装前缀和同一 MLIR SDK 的前缀加入 `CMAKE_PREFIX_PATH`。JIT 链接 `CompilerInfra::Core`。需要命令行开发入口的产品链接 `CompilerInfra::Driver`，它会传递核心依赖。

维护自有 LLVM/MLIR 构建的产品也可以在已有 SDK 配置后使用 `add_subdirectory` 编译框架源码，消费相同的两个 CMake target。这种方式默认关闭 SDK 安装。构建框架与产品应使用同一份 SDK 和同一 MLIR 链接方式。可用 `COMPILERINFRA_LINK_MLIR_DYLIB` 明确选择共享库（ON）或组件库（OFF）。组件库模式需要安装相应的传递开发依赖。本版已验证的 SDK 为 MLIR 20.1.8。

## 开发入口

| 入口 | 内容 |
| --- | --- |
| [框架设计](doc/foundation.md) | 加法式扩展、框架掌握的生命周期、agent 与 Bench 的合作方式 |
| [框架接口](doc/framework.md) | 产品装配、JIT、阶段边界、优化控制、驱动参数和对象寿命 |
| [仓库组织](doc/workspace.md) | 母仓代码、四个独立子仓、文档和临时工作区 |
| [开发原则](AGENTS.md) | 代码结构、错误处理、少量针对性验证及性能口径 |

IntentDSL 是首个接入产品。在其独立子仓中，`product.json` 将 KIR、Block、CPU、DSA 和 provider 组件链接到命令行与 Python JIT。Triton–Weft 作为另一台编译器独立接入。两者分别使用 IntentDSL Bench 和 Triton–Weft Bench 检查数值与测量性能。[工作区约定](doc/workspace.md)
