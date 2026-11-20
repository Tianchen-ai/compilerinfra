# 主仓与子仓

`compilerinfra/` 自身是 Git 仓库，拥有基座。工作区内的编译器与 Bench 各自有自己的 Git 历史，可以分别构建、使用和演进。

```text
compilerinfra/                     基座主仓
  README.md                        产品定位与安装入口
  AGENTS.md                        人和 agent 共用的开发原则
  CLAUDE.md                        引用 AGENTS.md
  CMakeLists.txt                   SDK 构建与安装
  cmake/                           find_package 配置
  agent/                           可安装的作用域、动作、默认插件、CLI 与 MCP 入口
  hosts/dsh/                       DSH 的工具与技能宿主插件
  include/compilerinfra/           组件装配、编译会话、IR 对照和产品驱动的公共接口
  lib/                             组件装配、编译会话和可链接驱动
  doc/                             框架设计与使用文档
  .work/<任务>/                    本地草稿、模型审阅和临时探索
  projects/
    intentdsl/                     子仓 · IntentDSL 编译器
    intentdsl-bench/               子仓 · IntentDSL Bench
    triton-weft/                   子仓 · Triton–Weft 编译器
    triton-weft-bench/             子仓 · Triton–Weft Bench
```

**路径表达协作关系，依赖由实际使用决定。** 母仓拥有框架的公共接口、编译生命周期、开发工具入口和设计文档。子仓拥有自己的源码、文档、构建与产品入口。每个 Bench 是完整工具，通过相应产品的公开入口执行，自己管理工作负载、数值对照、计时和输出。[现有 IntentDSL Bench](https://github.com/Tianchen-ai/Intent-Bench) · [现有 Triton–Weft Bench](https://github.com/Tianchen-ai/triton-weft-bench)

**本地工作与入库内容分开。** 基座设计的草案和审阅可以在 `.work/<任务>/` 反复修改。决定保留的内容，按归属进入母仓的 `doc/` 或相应子仓。Bench 运行产生的 IR、日志、性能数据留在发起运行的 Bench 输出目录。

IntentDSL 的迁移源码位于独立子仓 `projects/intentdsl/`。其 `product.json` 选择插件，`plugins/default.json` 选择产品默认策略。测量使用执行机上已有的 IntentDSL Bench。母仓 Git 忽略 `projects/` 和 `.work/`，单独克隆母仓只会取得基座文件。
