# 可派生的编译器

产品以 `Assembly` 登记领域、路线、provider 和实现，链接得到 `Compiler`。定义包含目标事实、每条路线的必需阶段、每个可选变换的默认状态及决定的选择方式。宿主在一个定义上创建目标 session。研发作用域可同时持有多个可运行定义。

## 决定如何变成程序

| 登记 | 领域组件实现 | 链接或运行时的检查 |
| --- | --- | --- |
| `DecisionFamily` | 用 `applies` 找到适用的 op／region 所属 op，`read`、`write` 访问领域表示，`verify` 检查完整候选和目标事实 | 框架读回所写值并在兑现前校验 |
| `Strategy` | 通过匹配 op 的原生 `mlir::AnalysisManager` 查询分析，读取 `TargetProfile`，返回一个领域 MLIR attribute。策略只提议 | 空 attribute 表示不匹配，继续下一项。LLVM Error 中止并保留 op 位置。所有策略落空会报告缺少实现 |
| `Realizer` | 收到已验证的值、目标接口和 MLIR rewriter，生成领域 IR 或下游 primitive，消费决定 | 物化后扫描程序，遗留决定会导致编译失败 |
| `Contribution` | 在具名扩展点组装已有 MLIR pass | 适合无需领域决定的等价变换。关闭可选项仍会执行必需 lowering |

领域包可以直接使用自己的 dialect 属性、操作或 MLIR interface。`read` 对任意 operation 有定义，无待兑现值时返回空 attribute。`applies` 在选择和物化两处识别适用结构；编译器保留并验证输入中已有的决定。物化按后序进行，删除或替换操作通过传入的 rewriter，顶层 module 归 session 所有。决定的完整数据存放于当前 IR，检查点序列化时一并保留。诊断与 explain 是观测结果。

lowering 在它实际拥有的位置插入 `Decide{"family"}` 和 `Materialize{"family"}`，两处可以分属不同阶段。链接器展开所选路线，检查配对、顺序、策略归属、重复项和兑现者。Provider 的 `decisions` 给出可用的默认链和兑现者。产品 preset 可以整体替换某个 family 的选择，也可以选择具名路线、设置可选优化或指定产品选项。领域类型、合法性和设备服务由插件提供。

## 原生定义、会话和 JIT

下面的调用使用产品已装配的 `compiler`，错误由调用方按 `llvm::Expected` 处理。

```cpp
CompilerPreset change;
change[target].decisions[family] = DecisionChoice{strategyOrder, realizer};
auto next = compiler.derive(change);
if (!next)
  return next.takeError();
return next->compile(target, input);
```

要复用 MLIR context，分别在定义上 `open(target)`，再用 `Session::compile` 传入内存 module 或字节输入。原定义及旧 session 在派生后仍可使用。Provider 通过 `configurePreset` 解释本产品允许的选项和目标绑定，该回调返回完整的 `TargetProfile`。回调与目标服务自行持有不可变的配置，不捕获会随全局 driver 切换的可变状态。对已经打开的原生 JIT，接受一个配置派生的方式是将新 `Compiler` 用于后续新建的 session。持久采用见下节。

## 源码候选的生命周期

1. 启动研发作用域并调用 `inspect_compiler`。默认引用为 `base`。有源码构建定义时，工作区现有已跟踪及未忽略的未跟踪源码构成起点，包含尚未提交的修改。同一作用域使用同一初始快照。
2. 调用 `derive(parent, preset, source_patch)`。返回的 `driver` 已绑定候选设置，外部调用者可在返回的 `cwd` 中直接执行，也可取得 `installation` 与 `preset_file` 供产品 JIT 接入。只改 preset 时复用原生代码。源码改动用 Git 格式补丁表达，作用域独占一个增量 CMake 构建目录，每个原生候选使用独立的安装前缀和源码资源。构建、安装、链接或插件加载失败会返回原生错误。
3. 使用新引用调用 `compile_program`、`explain` 或再次 `derive`。`plan` 展示静态顺序，`explain` 报告本次输入实际出现的 `preserved`、`no_match`、`selected` 和 `materialized`。按操作的源位置和决定值可以回到领域 IR。
4. `compare(parent, candidate, target, source, through)` 在同一输出边界比较。共享原生定义的公共阶段可复用进程内可确认的 bytecode 前缀。独立构建分别接收同一份原始字节，校验输出模型，报告首处 IR 差异。编译生成的源码也可以分别写出，交给独立 Bench 使用。
5. `adopt(compiler)` 把累计源码改动和选定的组合写到产品工作区。重新构建后，普通产品入口使用新组合。若所涉及文件已被其他人改动，返回冲突位置。Git 索引、提交和远端由产品维护者管理。`discard` 撤回该引用并卸载其插件；仍在使用同一原生安装的后代可继续工作。关闭作用域后临时引用失效，显式保存的产物继续留在所选输出目录。

## 可保存的组合

preset 是以目标名为键的 JSON 对象。下列名字需与选中产品的登记相符。

```json
{
  "triton": {
    "decisions": {
      "row-selection": {"strategies": ["first"], "realizer": "bind"}
    }
  }
}
```

每个目标可包含 `route`、`optimizations`（名字到布尔值）、`decisions` 和 `options`（产品解释的键值）。派生覆盖指定的项目。每个 family 的有序链和 realizer 一起替换，产品选项逐键覆盖。数值许可由产品解释，目标的经验候选和 autotuner 仍归相应产品。源码布局与各宿主入口见[插件手册](plugins.md)。
