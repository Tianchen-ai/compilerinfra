# 框架接口

SDK 暴露两个 CMake 目标。[Compiler.h](../include/compilerinfra/Compiler.h) 和 [Session.h](../include/compilerinfra/Session.h) 属于 `CompilerInfra::Core`。[Driver.h](../include/compilerinfra/Driver.h) 属于可选的 `CompilerInfra::Driver`。使用 `open` 创建会话的翻译单元包含 `Session.h`。

## 执行接口

采用基座的产品从 [Assembly](components.md) 登记一次模型、路线和优化，再调用 `link`。下面的 `Target` / `Stage` 是链接后送入同一执行器的低层形式。手中已有显式展开管线的调用方仍可直接使用它；只有经源码装配链接的产物有组件来源和影响查询。

1. 产品提供 `Frontend`。它登记源 dialect，导入源文件或内存输入，并验证源 module 是否属于本产品接受的表示。直接接收 MLIR 文本或 bytecode 的入口可以使用 `importMLIR`。
2. 每个 `Target` 声明名称、自己的 dialect 登记函数、顺序阶段以及最终 emitter。选项、硬件能力和工具链路径可用产品原有的强类型对象捕获到这些回调中。登记只描述能力。只有选中的目标才会装配 pipeline。
3. 每个 `Stage` 持有一组可执行的 builder 和一个 `verifyOutput`。通过 `PassGroup::required` 添加必需 lowering，通过 `PassGroup::optimization` 添加有名称、默认可启停的完整变换。builder 向 MLIR 的 `OpPassManager` 添加原生 pass 或嵌套 pipeline，返回 `llvm::Error` 表示装配结果。
4. 调用 `Compiler::create` 得到产品自己的编译器。目标名在产品内唯一，阶段名和优化名分别在各自目标内唯一。组件在产品代码中组合。框架没有全局目标或插件列表。

## 正常 JIT 调用

用 `Compiler::open` 显式选择目标并传入所需优化设置。会话拥有已装配的各阶段 PassManager。反复调用同一会话会复用 pipeline。产品可以让会话创建 MLIRContext，也可以传入已有 context。前端或目标特有的编译参数改变时，使用对应产品配置重新装配。不同设备之间能否复用也由领域产品判定。

| 调用 | 输入与用途 |
| --- | --- |
| `Session::compile(MemoryBufferRef, …)` | 通过产品 importer 读入源程序。返回 emitter 写出的字节 |
| `Session::compile(OwningOpRef<ModuleOp>, …)` | 直接接过已有 module 的所有权，在内存中运行相同 pipeline 并 emit |
| `Session::lower(ModuleOp, …)` | 调用方保有 module，框架就地改写，可直接交给已有的原生构建或 JIT 集成 |
| `Session::printPipeline` | 打印会话实际组装的阶段和原生 MLIR pipeline |

`Artifact` 是拥有内存的二进制缓冲，可容纳目标源码、IR 或机器产物。产品的 emitter 决定实际格式。需要多对象、设备句柄或产品自有元数据时，使用 `lower` 保留产品原来的 materializer。编译选项、用户缓冲和特殊设备的加载逻辑各自由其拥有者管理。

同一 session 串行使用。需要并发时，各请求使用独立会话及 context。外部 context 必须覆盖相关 module 和 session 的寿命。产品闭包按值或共享所有权持有所需资源，引用捕获的对象必须活到最后一次使用。正常路径的框架不写文件。调用返回的 `llvm::Error` 或 `llvm::Expected` 保留错误上下文和 MLIR 诊断。外部 context 已安装的错误处理器仍会收到原生诊断。就地改写失败后应丢弃该 module。

## 停止、检查、续跑

`StageRange::through` 表示运行至指定边界，`after` 表示从已有的边界 IR 开始。保留名称 `input` 指前端导入后的 module，可以在第一个 pass 前捕获或恢复 JIT 的输入。其余名称指阶段输出。设置 through 时，`compile` 返回阶段 IR，库默认采用 MLIR bytecode，也可以选择文本。只设置 after 则运行剩余阶段并正常 emit。起止相同时仅检查该边界。`Observer` 只读观察输入边界和实际完成的各阶段，可把需要的产物写入调用方指定的位置。

阶段边界首先服从 MLIR 的结构规则，产品的 `verifyOutput` 负责识别实际表示及影响后续执行的目标、类型和资源约束。在正常路径和续跑入口使用同一项产品检查。相同阶段名本身不证明程序适用于另一台设备或另一组配置。续跑使用 IR 中已经兑现的决定，若要改变之前的优化，应从该优化之前的阶段或原始输入开始。可重放的阶段必须把后续所需的执行信息保存在 IR 或显式传给会话的产品配置中。

`SessionOptions::optimizations` 只作用于声明过的优化名。每个 override 可以启用、关闭或提供新的 builder，替换在原槽位生效。必需 lowering 没有可关掉的槽位。声明可选意味着产品已有合法的关闭路径，这由提供该组件的领域编译器负责。会话允许截取任意有效边界。做对照时可调用 `checkOptimizationRange`，确认显式改变的槽位都落在实际执行的区间。`SessionOptions::configureStage` 用于在会话构建时配置原生 PassManager 或附加 MLIR instrumentation。

## 产品开发工具

产品已有命令行程序可以链接 `CompilerInfra::Driver` 并调用 `driverMain(argc, argv, compiler)`。另一重载接受返回 `Expected<Compiler>` 的工厂函数，框架在 LLVM 命令行解析结束后调用它，从而支持产品自己的编译参数。驱动也接入 MLIR 原生的 context、IR 打印和 pass manager 选项。下表中的参数属于这个**产品入口**。

| 参数 | 行为 |
| --- | --- |
| `--list` | JSON 格式列出目标、阶段以及各优化默认状态，查询时不运行 builder |
| `--preset FILE` | 从内置产品组合依次派生，可重复，用于编译和查询 |
| `--explain --target NAME INPUT` | 运行实际编译，返回观察到的 family、策略、决定值、兑现者及产物大小 |
| `--compare-ir --against-preset FILE --target NAME --through STAGE` | 比较选中定义与从同一产品默认组合派生的另一组设置，输出该边界的两份 IR |
| `--target NAME --pipeline` | 组装并打印指定目标的实际 pipeline，可与优化开关组合 |
| `--target NAME [INPUT] -o PATH` | 编译并写出目标产物。输入输出默认使用标准输入输出 |
| `--after STAGE` / `--through STAGE` | 从阶段 IR 续跑，或在阶段输出停止 |
| `--format asm\|bytecode` | 指定 through 输出的编码。命令行默认文本 |
| `--enable NAME` / `--disable NAME` | 针对本次会话修改已声明的优化 |
| `--replace NAME=PIPELINE` | 把产品已注册的 MLIR pass 或嵌套 pass pipeline 装在指定可选槽位，PIPELINE 是 module 内的 pipeline 内容 |
| `--components` / `--impact KIND:NAME` / `--plan TARGET` | JSON 源码组件目录、静态影响范围和链接后的目标管线 |
| `--replace NAME=@OTHER` | 选择同一扩展点上另一个已编入、适用于当前目标的贡献，保持其声明次序 |
| `--compare-ir --target TARGET --through STAGE` 加优化设置 | 从同一前缀 checkpoint 比较默认和修改后的阶段结果，JSON 输出两份 IR |

驱动会诊断实际执行区间之外的显式优化修改，以免把跳过的优化当作已运行的比较。写普通文件时，在目标路径旁产生临时文件，写入成功后替换最终文件。Bench 通过正常产品接口选择编译模式和输出位置，仍以产品的完整运行结果进行比较。

使用产品 preset 的 JIT 先登记 bundle，再调用生成的 `BUNDLE::link`。自定义装配可以直接调用 `Assembly::link(importer, inputModel, preset)`，在检查目标路线前应用初始的策略绑定和目标选项。链接完成后可以通过 `Compiler::derive` 生成其他定义。[决定与派生](derivation.md)
