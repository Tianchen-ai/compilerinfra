# 宿主与开发资产

运行时代码随所属候选安装并逐文件核对摘要。Python JIT 与 worker 都可以调用 `compiler.runtime("owner", "binding.entry")` 取得模块。产品代码可用 `runtime.<owner>.<module>` 访问当前候选中的依赖。运行时之间的导入遵守与原生单元相同的祖先关系。

runtime 宿主档提供 inspect、layout、compile。development 增加派生、解释、对比、影响查询、测量、采用与撤回，并提供脚手架、host-config、read-skill。compilerinfra-dev 的 actions 打印实际 JSON schema。未指定动作时，每行输入形如 `{"action":"layout","arguments":{"compiler":"base"}}`，响应带 result 或 error。开发会话持有返回的候选 ID。

MCP 服务器为 compilerinfra-agent。host-config 接受 host、library、workspace、output_dir，返回 Codex 或 Claude 的 stdio 配置结构。DSH 适配位于 hosts/dsh，通过 MCP 读取同一目录与 skill。安装产品前，可以仅指定工作目录使用开发脚手架。

在产品或单元的 agent/skills/ 下放置 Markdown，或在 agent/actions.py 中导出 register(registry)，以 registry.action 登记带类型注解的函数。产品级纯开发插件可以在 agent/<name>/ 下提供同样的结构。开发作用域加载随该次构建安装的文件，使用库内嵌清单定位和校验内容。在启动开发作用域时核对产品及扩展工作树的 Git 版本和未提交差异。inspect 返回 skill 和动作；read-skill 读取指定内容。

bench 字段采用 `{"package":"external_package"}`。该 Python 包导出 measure(request)，可以返回结果或 awaitable。请求包含 candidate、library、configuration、python、workload、output_dir。适配器用指定解释器或环境入口加载该安装和配置。workload 原样传递。产物、数值判定、基准选择及计时由外部 Bench 负责。
