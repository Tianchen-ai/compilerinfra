# v0.2 产品目录与依赖

产品根的 `product.json` 声明 `"compilerinfra": "0.2"` 和选用的 plugin 描述文件。`find_package(CompilerInfra 0.2 REQUIRED CONFIG)` 获取 SDK。每个原生单元有唯一的 `<layer>/<unit>/CMakeLists.txt` 与同级 `plugin.json`。装配按描述文件的依赖顺序进入目录。目录里用 `compilerinfra_add_component(... LAYER ...)` 声明源文件。产品顶层只装配发行组合和工具入口。

| 层 | 单元命名与内部位置 | 所有者及允许的原生依赖 |
| --- | --- | --- |
| dialect | `dialect/<representation>/ir/`，共享分析在同单元的 `analysis/`，接口在 `interfaces/` | 一种程序表示的操作、类型、effects、verifier 与 Model。可以为上游 MLIR 方言登记一个有明确不变量的 Model。 |
| conversion | `conversion/<from>-to-<to>/patterns/`，局部分析在 `analysis/` | 一个有方向的 lowering，依赖源和目标表示。规定扩展点以及进入与退出时的 IR 含义。 |
| strategy | `strategy/<choice-or-rewrite>/transforms/` 或 `analysis/` | 读取模型语义，提出决定或向转换扩展点登记可选变换；可链接 dialect。 |
| target | `target/<provider>/codegen/`、`capabilities/`、`abi/` | Provider、路线、目标能力和发射；可链接 dialect、conversion。ABI 描述编译产物的参数、结果及所有权。 |
| runtime | `runtime/<provider>/binding/`、`launch/`、`cache/` | 依据公开 ABI 绑定、加载与调用；运行时不链接产品编译实现。 |

简单的私有实现与注册入口可以直接放在单元根。按实际职责增加上表的子目录，不预建空层。供其他单元使用的头文件放 `include/<product>/<layer>/<unit>/`，私有头文件跟随实现。生成的 TableGen 头和机器产物进入构建目录。产品工具在 `tools/<command>/` 组合 SDK 与运行接口。作者语法与导入在 `frontend/<language>/`，宿主动作与 skill 在 `agent/` 或所属插件内，文档在 `doc/`，作者程序在 `examples/`，已有回归按所有者放 `test/<layer>/`。测量协议和结果由独立 Bench 持有。

## 多层 MLIR

每个可单独验证、检查或交接的表示都有具名 Model。两个表示间的转换单独占一个 conversion 单元，其 from/to 决定方向。新路线可以在任一支持的表示处接管。中间 IR 的 SSA、类型、访问、副作用和资源所有权由其表示的 verifier 负责；边的合法化由 conversion 负责；目标特有能力归 target。加入第三级时增加相应的表示、转换和路线组合，避免使用 stage1、stage2 等序号。在同一表示上做的普通优化沿用原 Model。

## 构建与查询

SDK 的 `cmake/product-layout.json` 定义层名、允许的组件依赖与目录角色。配置检查归属目录、参与编译的源文件和原生组件的链接边；框架链接还验证登记在正确层的 Model、Lowering、Contribution 和 Provider。上游 SDK 库使用其正常 CMake target。目录检查不推断 C++ 算法的语义，也不替代 MLIR verifier。

`<driver> --layout` 显示该产品采用的规则、组件目录与链接关系；`--components` 定位登记行和所属层；`--plan <target>` 显示真正运行的转换。agent 的 `layout` 动作可以查询 base 或已派生候选。用 `compilerinfra-dev check --product product.json --build-dir /path/to/build` 配置构建、安装到该构建目录、查询结构并通过公开 driver 编译产品 `check` 指定的 input、target 和 through。正常编译和检查使用同一套源码登记。

## 升级产品

在产品构建与描述文件中声明 SDK 版本，按照表示、转换、策略、目标、运行时逐个确定单元归属，登记真实源文件和依赖。多个可以分开替换的 provider 各自拥有目录；有上下游关系的方言通过 conversion 连接。只有同时使用者确实需要的接口才提升到 include。通过实际产物和后端消费者检查边界。
