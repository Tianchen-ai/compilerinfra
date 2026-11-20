# Agent 组件开发入口

产品拥有一份 `Assembly`。给一个目标增加共享优化时，改动的单位是一个可链接的源码组件。优化 agent 注册它，母仓把它装进适用目标的真实 pipeline，Bench agent 通过同一个产品入口取得产物，交给独立 Bench。两个编译器可以分别采用这套方式，各自使用原有 IR 和下游工具链。

## 一次扩展需要写什么

| Agent 要做的事 | 提交到产品的内容 | 框架承担的工作 |
| --- | --- | --- |
| 改善一个已有模型的程序 | 一项 `Contribution`，给出实际 pass builder、lowering 上的扩展点、适用特性和次序 | 在经过该点且具备特性的目标上装配一次；提供启停、替换、查询 |
| 添加可以消费已有模型的后端 | 一项 `Provider`，选一条路线，声明 emitter 接收的模型、实际能力及目标实现 | 检查边界，接上已有 lowering 和优化，登记到相同驱动和 MCP 工具 |
| 引入新的模型组织 | `Model` 的 verifier 与 dialect，加上连接前后模型的 `Lowering` | 注册和复用具名阶段、组合路线、校验输入及输出 |
| 复用一段已有编译过程 | 在 `Route::steps` 中按顺序引用 `UseRoute` 和 `UseLowering` | 展开共享前缀、中段和后缀，检查循环、重复的阶段名和不匹配的模型 |

组件的 C++ 声明见 [Assembly.h](../include/compilerinfra/Assembly.h)。`Assembly::add(value, COMPILERINFRA_SOURCE)` 记录注册点的文件与行号。声明顺序不决定运行次序。插件内部继续按真实 IR 做适用性分析、改写与验证。有意义的算法或硬件差异仍然需要写实际实现。

## 路线和扩展点的语义

- `Model::verify` 检查当前 module 是否属于相应表示，可使用显式目标 profile。源模型和阶段输出都用这些检查。注册 `Model` 时可以就近提供 dialect。
- `Lowering` 声明输入、输出模型以及依次执行的步骤。`ModelPasses` 添加必需 pass，`ExtensionPoint` 在该位置接收独立贡献。多个 lowering 可产生同一种模型，产品选择具名路线。添加一条新边不会让已有目标改道。
- `Contribution` 在一个 lowering 内命名扩展点。在同一点，`after` 引用必须已登记的同点贡献，`afterIfPresent` 允许可选组件缺席。二者都只约束次序，启用一项优化不会隐式启用前驱。无先后要求时按组件名确定次序。同一正确性单元中的相互依赖步骤应放在同一 builder。
- `requiresFeatures` 采用 provider 声明的实际能力名，要求全部满足。量化资源和目标实现可用 `TargetProfile::services.provide<Interface>` 绑定领域强类型接口，公共 lowering 用 `get<Interface>()` 获取绑定。共享代码可以由各 provider 分别提供实现。领域 IR 实际使用的资源仍须在 IR 上兑现。
- `Assembly::link(importer, inputModel)` 解析源码组件。模型衔接、发射器输入、重名和顺序错误报告具体注册位置。它返回原有的 `Compiler`，选中目标后才运行 pass builder。产品选项和硬件事实由产品提供。若产品构造配置需要启动设备，应把纯声明查询与设备探测分开。
- 在已有槽位试用原生 MLIR pipeline 可用 `--replace NAME=PIPELINE`。另一个已编入的同点贡献可用 `--replace NAME=@OTHER`：框架关闭 NAME，按 OTHER 自己的次序启用它。可用特性、显式覆盖冲突和执行范围仍接受检查。

## 链接源码组件

产品通过 `find_package(CompilerInfra CONFIG REQUIRED)` 或 `add_subdirectory` 获得以下 CMake 函数。

1. `compilerinfra_add_component(TARGET ENTRY FUNCTION SOURCES ... [LINK_LIBRARIES ...] [REGISTER_PASSES FUNCTION])`。组件实现 `void compilerinfra_components::FUNCTION(compilerinfra::Assembly &)`，声明与依赖留在对应源码旁。可选的原生 pass 登记函数签名为 `void compilerinfra_components::FUNCTION()`，供文本替换等 MLIR 操作使用。
2. `compilerinfra_assemble(BUNDLE PRODUCT product.json COMPONENTS ...)` 读取产品插件选择和默认 preset，生成静态强引用与头文件 `<BUNDLE/Components.h>`。也可直接指定 `PLUGINS` 和 `PRESET`。产品 JIT 调用 `BUNDLE::registerComponents(assembly)`，再由 `BUNDLE::link(assembly, importer, inputModel)` 应用与产品驱动相同的内置默认值。需要 MLIR 全局 pass 名称时调用 `BUNDLE::registerPasses()`。打包为产品 SDK 时可将 bundle 的 `PUBLIC_HEADER` 安装到 `include/BUNDLE/`。
3. 原生 MLIR 输入的工具可以直接使用 `compilerinfra_add_driver(TOOL ASSEMBLY BUNDLE INPUT_MODEL MODEL)`。使用自有语言前端或产品专用命令行选项时，由产品提供 importer，仍使用同一个 bundle、`Assembly::link` 和 `driverMain`。组件需要运行时配置时，可在 JIT 装配组合时用普通 C++ 注册捕获产品配置的 provider。

一次新增通常改组件源码及所属产品的构建清单。已有 N 个目标并不需要 N 份登记。新模型间的 lowering、显式路线选择和真正的硬件专用实现仍有成本。完整的会话和检查点接口见[框架接口](framework.md)。

## MCP 与命令行

安装一次 `python -m pip install ./agent`，从 MCP 客户端以 stdio 启动 `compilerinfra-agent --driver /absolute/product-tool --workspace /product/checkout --output-dir /chosen/output`。产品专用的固定参数可重复传 `--driver-arg=VALUE`。作用域绑定启动时的定义，`derive` 为新源码或配置分配新的编译器引用。[候选生命周期与完整动作](derivation.md)

| 工具 | 相同的产品驱动能力 |
| --- | --- |
| `components(kind, contains, file)` | `--components`。按类型、名字或源码文件发现组件、注册位置、扩展点、声明依赖和次序 |
| `impact(component)` | `--impact KIND:NAME`。区分路线可用、缺少目标特性、未走该路线；声明给出默认启停 |
| `plan(target)` | `--plan TARGET`。查看选定路线及实际的必需/可选槽位顺序 |
| `pipeline(target, enable, disable, replace)` | `--target TARGET --pipeline`。执行 builder，查看这些具体设置产生的原生 MLIR pass 序列 |
| `compare_ir(target, source, through, …)` | `--compare-ir`。源程序只导入一次，共同前缀只执行一次。在第一项干预之前生成一份 bytecode，两个独立会话从同一份内容分别运行默认和修改后的后缀。MCP 返回指定边界的文本 IR 是否变化，以及首处变化的有限摘录 |
| `compile_program(target, source, output, …)` | 从正常入口编译，支持 `after`、`through` 和同一组优化控制。返回产物路径、大小、停止边界与编译诊断 |

`impact` 描述登记和目标条件。一个可用贡献可以默认关闭；实际启用的 pass 也可以因为 IR 不匹配而没有做改写。要看具体程序，使用 pipeline 和前后阶段产物，要报速度则运行独立 Bench。输入路径可以在工作区或输出目录中，产物写入指定输出目录。多个并发实验选择不同文件名。

原生 [compareIR](../include/compilerinfra/Comparison.h) 返回两份完整的 MLIR 汇编，CLI 的 JSON 也保留这两份内容。比较只对给定输入、目标、阶段与设置负责。文本改变本身并不证明数值变化或性能收益。
