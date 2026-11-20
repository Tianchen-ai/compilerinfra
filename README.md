# compilerinfra

compilerinfra 是在 MLIR 上构建领域编译器的 C++ 框架。产品提供前端、目标和 lowering 组件，框架负责选择显式目标、装配真实 pass pipeline、运行和检查各阶段，并调用目标 emitter 返回产物。JIT、优化 agent 和产品的开发工具使用同一条编译路径。

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
| [框架设计](doc/foundation.md) | 框架掌握的生命周期、领域产品的职责、agent 与 Bench 的合作方式 |
| [框架接口](doc/framework.md) | 产品装配、JIT、阶段边界、优化控制、驱动参数和对象寿命 |
| [仓库组织](doc/workspace.md) | 母仓代码、四个独立子仓、文档和临时工作区 |
| [开发原则](AGENTS.md) | 代码结构、错误处理、少量针对性验证及性能口径 |

这版交付母仓 SDK。IntentDSL 将作为首个接入产品，Triton–Weft 随后独立接入。正确性与性能测量分别由 IntentDSL Bench、Triton–Weft Bench 负责。
