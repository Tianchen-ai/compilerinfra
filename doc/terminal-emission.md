# CompilerInfra v0.4 设计稿：从出口终端 IR 发射

**状态：v0.4 已批准。** 出口的最后一个边界固定执行结构。产品沿用编译树、会话、检查点、产物及 JIT 接口。新出口登记 `TerminalPrinter`，旧模块回调在 layout 中显示为 legacy。[装配入口](../include/compilerinfra/Assembly.h) · [打印协议](../include/compilerinfra/Terminal.h)

## 1. 终端边界

| 终端 IR 的生产者 | 必须交给打印器及下游的内容 |
|---|---|
| 节点 realizer、出口 legalize | 循环范围、步长、尾部、条件、工作归属、region 参数、carry 与 yield；软件流水的前导／主循环／排空。融合后的生产者和消费者已在同一可遍历 region，或已合为一个满足第 2 节的目标操作。 |
| 存储与供数变换 | 分配及其寿命、地址空间、视图、打包和复用位置，读写所及缓冲，搬运的发起、完成值、等待和同步作用域。寄存器分配、指令内部布局等已委托给下游的决定仍由下游完成。 |
| 出口 ABI 与地址合法化 | 参数、结果、launch/host 入口、每个 writeback 的投影和有效域。地址默认使用 i64，先扩展再乘加。仅由持有静态范围证明的优化 pass 收窄，证明覆盖负步长、间接索引和尾部的中间值。委托给下游的 array view、block pointer 或 tile index 以该粒度的 IR 操作表达，标明索引宽度、单位和约束。 |
| 实现选择与物化 | 选中的实现符号、匹配的 typed operands/results、已绑定的行参数和所需依赖。被多个操作观察的准备、同步、状态更新、尾处理和释放都占有自己的 IR 位置。 |

**拟议打印接口。** 出口登记可接受的 op 和打印规则。框架负责遍历 module、函数、block、region 和终结符，管理 SSA 名字、块标签、缩进、符号引用、源码位置及诊断。结构 op 提供局部语法（含 region 前后的语法与块参数／结果绑定），框架按 IR 的语义顺序在这些位置逐一递归，各 region、block、op 恰好访问一次。显式分支打印其 successor，绝不把 CFG 的静态打印次序当作运行次序。叶规则只接收本 op 的已解析属性、操作数／结果和已绑定符号。内建表达式可内联；跨 op 重排、克隆或分配在 legalize 完成。一个栈上临时、一次设备写回或一个新 barrier 都不能因打印回调看到了兄弟 op 才出现。

**多文件产物。** 若出口产生 host 与多个内核，终端 module 以具名符号／region 表示各自的有序程序以及它们的调用 ABI。框架逐个输出，出口封装器只接收已打印的片段、入口、行、guard、符号和版本化 ABI 以组装既有交付格式。封装器没有访问整 module 或改写执行文本的接口。可直接交付 MLIR 的出口优先使用 MLIR 的已有打印／字节码能力。

**检查责任。**

| 时机 | 框架和产品 verifier 能执行的检查 | 需要轻量人工审查的判断 |
|---|---|---|
| 链接 | 每出口声明唯一发射模式。新模式的 op、目标实现、依赖、打印规则均可解析，登记位置符合树的归属。拒绝重复或缺失注册。 | 合同作用域是否诚实，哪一层真正拥有这个决定。 |
| 完成终端边界或由其恢复 | 先执行 MLIR verifier 及出口的完整合法化检查，再逐层走访子 region。检查 SSA 支配、类型／region 签名、效果接口、显式地址宽度、绑定的实现及前提。产品 verifier 按模型检查 bounds/guard、资源寿命和完成依赖；未知证明应选择宽实现或给出定位到 op 的错误。 | 一个已声明的效果是否完整，范围证明是否适用于真实输入，库 ABI 和数值许可是否吻合。 |
| 打印 | 未登记 op、未消费 region、无法解析的操作数、符号或产物引用就地报错。普通编译、`--through` 和 `--after` 使用同一边界规则。 | C++ 打印函数和外部库的任意行为无法靠注册签名证明。抽查终端 IR 与目标片段，尤其检查新出现的循环、scratch、同步、初始化和写回。 |

**兼容窗口。** 原整 module 回调标记为 deprecated；已有出口在 `inspect/layout` 中显示 legacy，新登记默认使用逐 op 模式，迁移期间同一出口只启用一种。先用原产物 ABI 跑通新出口，再删除该出口旧回调和旁路。最后一个在用出口迁移后删除框架回调、兼容装配分支和旧脚手架说明。缺少打印规则直接诊断，不能静默退回旧路径。

## 2. 有合同的目标操作

源码注册表由出口拥有，扩展包沿同一作用域补充。**语义 op 描述做什么；实现项描述谁在什么条件下可以完成它。** 一个 op 可有多个实现，源、名称和局部估算器随候选安装解析。选择后由 realizer 在终端 IR 上留下可核验的符号引用。注册表只有三种实现来源：

| 来源 | 实现和交付位置 | 调用处的检查 |
|---|---|---|
| 目标语言内建 | 下游方言 op、语言原语或 intrinsic，由目标打印规则拼写 | 操作数／结果及下游版本、设备特性 |
| 产品微核源码 | `runtime/<exit>/` 下随候选安装的实现文件；出口的公开 ABI 联系实现，编译单元不反向链接 runtime。产品构建或下游 JIT 编译相应源码 | 候选安装内的符号、ABI、布局、scratch、同步和有效域 |
| 厂商／外部库 | 已声明依赖导出的符号或已链接的薄 ABI 包装 | 版本、编译与加载 ABI、执行设备、数值能力和流／线程所有权 |

普通算术、数学函数、逐元素和 load/store 内建直接登记类型与打印规则。多实现操作、微核、库调用与 collective 才登记以下实现合同，领域语义沿用 MLIR 的类型、属性和 interface。

1. 语义名、实现名和来源；输入／输出／累加类型；逻辑形状关系、实现粒度、支持的动态维度及尾部行为。
2. stride、对齐、布局／packing、地址空间和别名条件。显式 scratch 操作数或内部私有空间的上界、寿命与可见性。
3. 对哪些资源读、写、分配、释放；同步参与者、作用域和完成点。异步调用返回由消费者等待的完成值。内部同步可以实现一个 collective；跨两个操作的等待要连接在 IR 上。
4. 累加顺序、溢出、舍入、NaN、次正规数和允许的近似。实例化时与当前 IR 的数值许可核对。
5. 可用条件从带单位的机器事实、已探测工具链／依赖和输入事实求值。声明出口局部估算器及参数，例如工作量、读写字节、固定调用开销、吞吐和并行度。

边界按**可观察语义和副作用**划分。一次归约可用 shuffle、多条指令及内部屏障；库的整区域实现亦可参与选择，条件是完整匹配该 op 的值、外部 effects 和调用合同。连接两次调用的 packing 可被多个消费者复用，必须由外层持有；跨 op 的 producer/consumer、launch 拆分、隐藏的持久权重缓存或线程池重派发不能封进微核。

**接入 v0.3。** 决策家族为每种合法实现及其外层组织提出候选；`verifyCandidate` 检查实参、事实和数值前提。节点模型通过 `CostContext::local` 调用出口 `cost/` 中的估算器，再加上外层访存、packing、并行、等待及尾部；共享准备按寿命计一次。缺参数可保留合法但无估时的候选。局部上／下界只解释排序，仍只有相同完整调用口径的证明可硬淘汰。开发态 `explain` 按候选显示用户档位、实现及来源、依赖/能力/数值拒绝原因、估算器及参数来源、分项和最终 IR 决定；运行时仍只在已选拓扑的合法行中调优。[已有成本合同](performance.md)

例如两个消费者需同一种 packed view：候选 A 将 pack 写在共同支配点，节点模型为一次读写估价，加两次出口点积估价。候选 B 各自使用原 stride 的点积，计两次带 stride 的局部成本。选中 A 后，检查点中应有一个分配、一个 pack、两条对同一结果的 use 和最后一次 use 后的释放。若观察到两个隐含 pack，就应修正 lowering 或实现合同；不能只扣掉估算里的重复费用。

**微核随包方式。** 目标操作源码登记引用所属 runtime 单元内的相对资源。构建期解析归属、公开 ABI 和实现符号，安装器将下游 JIT 还需编译的源码、头及必要的数据作为该候选的安装资产交付。仅构建时需要的源码可以编为该 runtime 的对象。JIT 只从本次候选安装加载，采用／撤回沿用已有代际和句柄寿命，旧会话继续调用旧符号。为每个微核再建一个运行时单元没有必要。

## 3. 成段匹配的准入条件

匹配可以跨完整 region。pattern 必须验证形状、访问、别名、控制和数值许可，用正常 rewrite 构造 typed IR，连接输入、所有活结果和外部 effects。给定替换结果，后续 pass 必须能够读取并改写跨 op 的结构，独立 verifier 能判定修改是否合法，恢复检查点不需要原匹配器再次猜测。

- **允许**：验证广播乘法和指定轴归约可换为收缩后产生 `dot`，保留累加类型、有效域和尾部；后续 pass 可以更改分块或替换 dot 实现。若在线归一化需要额外重结合，先查 IR 许可，再生成带状态的循环。
- **禁止**：匹配五个参数和两个 fold 后只写 `fast_attention=true`，在发射器里重建 DMA、片上槽、两次收缩与写回。即使一个用例数值正确，调度和存储仍无法由后续 pass 处理。

## 4. 可选外部依赖

拟在 `product.json` 增加可选列表，例如以下产品无关的示意。列表给出依赖的身份及拥有其绑定的单元；符号 ABI、版本范围和可用条件由一个对应的探测实现解释，`doctor`、构建与 JIT 共用其结果。

```json
{
  "optional_dependencies": [
    {"id": "matrix-kernels", "owner": "exits/backend",
     "package": "MatrixSDK", "version": ">=3.2,<4",
     "cmake_target": "MatrixSDK::kernels", "probe": "exits/backend::matrix-abi"}
  ]
}
```

配置构建时安静查询包、版本、头、功能宏和必要符号，可用时只链接对应的 imported target。对执行主机可选的库，应隔离在可选适配模块或原生 JIT 产物，避免产品主共享库产生无条件的装载依赖。安装后在实际执行主机解析已安装版本、ABI、加载符号和设备功能；交叉或离线生成只能报告已知的构建事实。声明查询不触发探测。安装检查 `doctor` 按出口报告期望／实得版本、resolved target／路径、关键符号、探测阶段和失败原因，区分未安装、版本不符、能力缺失和可用。缓存使用的有效工具链／依赖身份随变动失效；不增加独立台账。

例如候选 A 要求库的矩阵入口，B 使用内建收缩。A 缺符号时，其条件失败进入 explain；模型仍可正常选择 B 并在 IR 中绑定 B。若用户强制 A 或当前无任何合法实现，最早能解释问题的决定／legalize 边界报告出口、op、缺失条件和已检查的实现。已经编译的 A 在加载时发现 ABI 变化，应请求按新事实重新编译或报告加载错误；已绑定调用不切换到 B。

## 5. 复用档位与发行顺序

建议 SDK 在已有 configuration 上定义统一的有序上限 `reuse=native|vendor|library`，可由配置派生覆盖；产品在默认配置选 `vendor`，并负责给自己的实现分类。档位是实现选择策略，来源是代码所有权，两者分别记录。例如产品自写的收缩微核一般属 native，包装厂商微核的产品源码至少属 vendor，调用外部整算子库须属 library；包装不得降低所依赖实现的档位。

| 档位 | 可参加本次决定的实现 |
|---|---|
| native | 下游语言常规原语、指令选择、产品自有源码路径；正常使用所选下游编译器 |
| vendor | 加入显式选择的厂商微核／低层构件；外层调度、共享准备和写回仍按 IR |
| library | 再加入与当前区域语义、ABI 和 effects 全部匹配的库调用；包括匹配的整算子 |

先按档位过滤，再查合法性、依赖与机器能力，最后在可行集合上估价。高档位允许低档位胜出，配置的基线也受同样过滤。强制超档候选应诊断。explain 分列“超出档位”“依赖不可用”“数值或布局不兼容”和“合法但估价较高”；普通 JIT 无需打开观察。

实施顺序建议为注册与诊断、单出口递归发射、逐出口迁移和删除旧 API。利用已有 derive/compile/explain，检查同一终端检查点恢复后的产物和原生 ABI；在产品上改变一个可见循环或换一个实现，确认后续 pass、下游编译和 Bench 能观察到相应变化。v0.4 设计阶段不变更 SDK 版本、构建和运行时。

### 对照依据

Triton 的广播乘归约改写生成 dot；下层 reduce 可以根据实现需要生成屏障。TileLang 的 codegen 按节点分派，目标模板承担有作用域的 collective。LLVM 将机器级程序交给 MC 发射，MLIR 的 conversion target 和效果接口分别支持检查合法操作集合与声明资源效果。这些机制分别用于区分“程序已形成”“一个目标操作如何兑现”和“如何拼写”。[Triton Combine](https://github.com/triton-lang/triton/blob/ef08c7fc412737f1663dbfc38a8bed3e9fbf82da/lib/Dialect/Triton/Transforms/Combine.cpp#L112) · [Triton Reduce](https://github.com/triton-lang/triton/blob/ef08c7fc412737f1663dbfc38a8bed3e9fbf82da/lib/Conversion/TritonGPUToLLVM/ReduceOpToLLVM.cpp#L75) · [TileLang CodeGen](https://github.com/tile-ai/tilelang/blob/f35443064013a94e758147d60fdd36685e8be738/src/cpu/codegen/codegen_c.cc#L386) · [TileLang AllReduce](https://github.com/tile-ai/tilelang/blob/f35443064013a94e758147d60fdd36685e8be738/src/tl_templates/cuda/reduce.h#L211) · [LLVM Code Generator](https://llvm.org/docs/CodeGenerator.html) · [MLIR Dialect Conversion](https://mlir.llvm.org/docs/DialectConversion/) · [MLIR Side Effects](https://mlir.llvm.org/docs/Rationale/SideEffectsAndSpeculation/)
