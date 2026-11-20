# compilerinfra

compilerinfra 是为 coding agent 设计的 MLIR 编译器开发框架。一个优化 agent 可以查询产品的执行模型和源码位置，在已有扩展点贡献一个变换，直接让采用相同路线且能力匹配的后端使用它。一个新后端可以引用已有模型、路线和优化，提供自己的目标实现。产品 JIT、命令行和 MCP 使用同一份源码装配结果。

从[组件开发与 MCP](doc/components.md)开始。文档列出了新增优化、模型和后端各要写哪些代码，以及哪些工作由框架承担。

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

这版交付母仓 SDK。IntentDSL 将作为首个接入产品，Triton–Weft 随后独立接入。正确性与性能测量分别由 IntentDSL Bench、Triton–Weft Bench 负责。
