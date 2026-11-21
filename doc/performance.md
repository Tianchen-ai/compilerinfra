# 性能决定从哪里来

在每个编程模型的 `doc/design/performance-<node>.md` 中，把一个性能原则写成一行可执行的对应关系。记录作者语义和适用域、负责的 family、需要比较的候选、带单位的估算项、实际所需的机器事实，以及能区分这些选择的作者程序。例如，驻留减少重复读取，也会占用本地空间。可以比较驻留与流式，用重复读取字节数、有效带宽和并发实例数估时，在两个不同的归约程序上观察宽度变化的影响。硬件内部布局继续由接手该表示的下游决定。

## 在节点上选择结构

| 登记对象 | 输入与职责 |
|---|---|
| Strategy::enumerate | 对一个 op 或 region 提供有序的有限集。每项有家族内唯一且可复现的 id、拓扑名、完整的 typed 属性（含参数行域）、前提说明。真正的前提须由家族 verifier 检查。空集表示不匹配。 |
| DecisionFamily | 检查每项是否在语义、设备和资源约束内，写入最终的一个属性。登记 enumerate 的家族必须用 verifyCandidate 核对拓扑标签、前提与该属性之间的关系。已有的作者决定优先经过验证。 |
| 节点 costModels | 接受同一节点的 family、当前 op、候选和 CostContext。通过 fact(name,unit) 读取机器事实，通过 local(qualified_name) 使用出口估算器。 |
| 出口 costModels | 估算指定计算区域中的 primitive 等局部工作，返回与节点相同类型的估算，由节点组合各项。 |
| Realizer | 将一个决定兑现为可执行的 IR。运行时只会收到这个控制拓扑的参数行。 |

一个旧的 `propose` 对应一个候选，候选 ID 和默认拓扑为完整策略名。采用新接口时可为同一拓扑提供多项选择。模型按 `Unit::costModels` 登记，给出名称、所属 family、版本和估算函数。唯一的节点模型自动启用，有多个时应在配置中指定。没有节点模型时按配置的策略顺序采用首个合法结构。查询误配、不可用的派生选择和编译组件自身的错误都会报告。

在已有节点配置的 `decisions[family]` 条目中，保留完整的 `strategies` 和 `realizer`。可加 `model`（完整登记名）、`uncertainty`（非负相对阈值，例如 0.05）、`candidate`（派生编译器要采用的合法结构 ID）。配置派生替换整条 family 选择。默认基线取第一个合法提案。预测时间用 ns，可为空。模型还应给出各分项及单位、估算适用范围、瓶颈、缺失事实。可在 `CostEstimate.rows` 按原生行 ID 返回逐行预测。

只有 family 判为非法或一项已证明的下界严格大于一个可行对手的已证明上界，才硬淘汰。上下界各自声明 ns、scope、comparison 和 proof。硬淘汰只在同次决定中比较 scope=full_call 且 comparison 相同的界。comparison 标识调用口径，如冷热状态及计时包含的阶段。局部界单独列入 explain。界必须附推导依据，实测置信区间和启发式预测不能当作证明。其余候选按预测时间升序、提案次序打破平局。无估时的项排在有估时的项之后。保留第一名、基线、每种拓扑的第一名、缺事实及位于阈值内的结构。排名靠后的合法项仍可显式派生。全部缺估时就按提案顺序。编译器只物化选中的结构，同拓扑内的试跑由已有 runtime 完成。

## 查询事实与校准

出口通过 `TargetServices::facts.query` 提供查询到的具名值、单位、版本和来源，通过 `calibrationProfile` 选择与现场设备和工具链匹配的已安装配置。构建时读取该出口及其扩展的 `capabilities/<profile>.calibration.json`，把数据嵌入产品库。不同源码候选的校准相互独立。每条记录含 `value`、`unit`、`source`，文件有非空 `version` 和 `facts` 对象。来源字段为 `device`、`isa`、`toolchain`、`conditions`（工作频率或并发条件）、`benchmark`、`error`。用测量工具运行微基准，再将新的参数文件作为源码候选派生、比较、采用。指定了不存在的 profile 时出口报告错误。query 和校准加载入口分别写入来源类别，输入自填 kind 会报错。查询不到的名称记录在 missing；单位不符在 unit_mismatches 分别列出预期与实际单位。

开启 explain 后查看 `action=candidate`。每条都有完整决定属性、分项、预测 ns、适用范围、缺失事实、估算器与机器事实版本、保留或淘汰理由。对普通调用，框架直接按选定配置编译。

## Bench 交换格式与准确度

Bench 产出 UTF-8 JSON，顶层是 `{"version":2,"compilations":[...],"rows":[...]}`。compilations 的每项包含 batch、family 和 measure 收到的原始 prediction。编译器对最终发射字节串计算 SHA-256。同一产物可包含多行，通过行 ID 区分。测量端读取实际执行产物的摘要，并附 compilation_id 与 compiler_candidate。准确度动作以对应实际编译的 selected 候选行域连接 measured rows，拒绝缺行、多行、摘要或身份错配的批次；excluded 附原因。

| 字段 | 含义 |
|---|---|
| batch、split | 同设备、编译环境、工作负载、实际输入和测量协议的一次可比批次。独立划为 calibration 或 holdout。 |
| candidate、row_id | 可派生的结构 ID 和该结构内的原生行 ID。 |
| terms、predicted_ns | 行级分项映射，每项是带 value、unit 的对象。预测时间单位 ns，未知用 null。可额外填 structure_ns 记录编译时结构估时。 |
| measured_ns、winner | Bench 实测的完整调用时间和每个结构唯一的运行时选中行。 |
| selected、artifact_sha256 | 编译器选择的结构（同结构的所有行相同）以及实际被测的产物摘要。 |

开发动作 `measure` 可传 `compilation_id`。宿主检查编译归属于当前候选，随测量请求附上根 IR 摘要、出口和这次编译的候选预测。Bench 按 `explain` 的结构和行 ID 关联试跑与完整调用测量。单行实测、完整调用、预测和候选生成规模应各自可辨。不同拓扑分别派生并测量。开发动作 `model-accuracy` 接受 `records` 文件路径和正整数 `k`。每个结构的实测值取实际 winner 的完整调用时间。优先采用显式的结构预测，否则取完整逐行预测的最小值，缺行估时视作未知。按校准和留出分别报告：

- 成对排序正确率，排除实测平局，预测平局计半分，分母是可比对数。
- top-k 召回率，用预测与实测前 k 项的交集除以实测前 k 项数。边界同分的结构平分入选权，未预测项也参与实际排名。
- 每批所选结构耗时除以同批实测最佳结构耗时，以及各批的算术平均。保留逐批结果和缺估时数。

采用代价模型时，在正常提交说明中记录 Bench 输出路径或 ID、校准和留出的指标及完整调用收益。既有 adopt 只应用候选并重新安装，Git 提交由操作者完成。
