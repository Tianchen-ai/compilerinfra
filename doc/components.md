# 登记和运行编译单元

节点、出口或前端的登记文件包含 `compilerinfra/Assembly.h`，在 `CI_UNIT_NAMESPACE` 中实现 `void bind(compilerinfra::Unit &unit)`。框架生成对这些函数的强引用。使用 `COMPILERINFRA_SOURCE` 记录登记位置。查看已安装 driver 的 `--layout` 或宿主的 `layout`，可以定位实际登记点、边界次序和生效配置。

节点和出口分别为每个边界登记构造 MLIR pipeline 的函数和 verifier。在边界之前执行 Contribution，使用类型、访问或目标能力决定何时改写。after 引用同一边界上的已登记贡献，afterIfPresent 允许指定缺席的贡献。启用的变换按依赖顺序执行。

DecisionFamily 定义在真实 IR 上读取、验证和写入决定的合同。FamilyBinding 指定作决定的边界和兑现决定的边界。Strategy 返回候选属性、未匹配或错误。Realizer 负责改写并消费已选属性。产品在默认配置的 nodes.<node>.decisions 中为每个家族选择完整的 strategies 列表与 realizer，名称使用 unit-path::local-name。一个出口可以在 exits.<exit>.decisions 中替换该条完整选择。可选变换在相应 scope 的 optimizations 映射中启停。目标选项属于 exits.<exit>.options。边界校验和必需变换保留在所有配置里。

`Compiler.compile` 接受文本或 bytecode 形式的根 IR。`through` 返回该边界的检查点；设置 `text=True` 可查看 MLIR 文本。将检查点和 `after` 交给同一条合法的出口路径，可以恢复编译。出口声明有序的 boundaries 并登记对应的合法化实现，最后一个出口边界仍位于 emitter 之前。扩展在已有出口的同形目录内登记变换或策略。子节点与出口服从同一条接出规则：接出点落在决定和物化之间时，接收单元必须提供私有 realizer，在自己的第一个边界之前兑现。单一私有实现自动选中；存在多个时，在接收单元的 decisions 中按家族指定 realizer。缺少实现的分支及其后代在 layout 中带有 valid=false 和原因。正常编译返回原生产物字节。

每个编译单元拥有一个可链接目标。生成规则可以在 compilerinfra_unit 中用 TABLEGEN 列出，每项采用 "ir/Input.td Output.inc -gen-..." 格式。SDK 调用 mlir_tablegen，并让单元依赖自己的 IncGen 目标。使用上游生成宏时也可用 DEPENDS 指定辅助目标。生成目录传播给链接该单元的消费者。公开头建议按 <product>/<unit>/<output>.inc 引用，使同一头文件可用于开发和安装。TABLEGEN 的输出自动安装；使用外部生成宏时用 PUBLIC_GENERATED_HEADERS 列出需要发布的 .inc。构建生成源码的登记位置使用 @generated/<unit>/ 前缀。LINK_LIBRARIES 接受 CMake 目标；外部库先声明为 imported target。
