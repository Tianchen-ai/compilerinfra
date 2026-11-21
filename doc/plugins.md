# 插件、开发动作与宿主

## 从一个产品启动

```sh
python -m pip install ./agent
compilerinfra-dev --driver /product/install/bin/product-opt \
  --workspace /product/src --product /product/src/product.json \
  --output-dir /product/work --bundle development
```

CLI 在同一进程中逐行读入 JSON 并立即返回结果。MCP 客户端以相同参数启动 `compilerinfra-agent`，通过 stdio 使用同一动作目录。CLI 的 `{"action":"actions"}` 返回类型和说明。例如先发 `{"action":"inspect_compiler","arguments":{}}`，再发 `{"action":"plan","arguments":{"target":"triton"}}`。派生返回的 `compiler` 引用可在后续请求中使用。作用域与普通产品 JIT 共享原生装配和驱动。

| 发行组合 | 可调用的能力 |
| --- | --- |
| 运行 | C++ SDK 在进程内或原生产品 CLI 编译，无须 Python、MCP、模型服务。宿主选择 `runtime` 可查询和编译 |
| 研发 | `development` 在运行能力上增加 derive、explain、compare、adopt、discard、检查点、动作和 skill 发现 |
| 测量接入 | `measurement` 增加两个独立 Bench 的动作和测量 skill |

## C++ 开发宿主

Linux 的原生研发工具可以链接 `CompilerInfra::Development`，通过 `DevelopmentScope::open(argv)` 启动同一个持久 CLI，使用 `scope->call("actions")` 获取完整目录，通过 `call(action, arguments)` 操作真实候选和插件。对象析构时关闭输入并等待作用域清理。产品热路径直接使用 `CompilerInfra::Core`。[C++ 开发接口](../include/compilerinfra/Development.h)

## 产品装配

产品在 Git 根目录放开发配置。例如下面的产品将默认 preset 编入装配，并把原生工具安装到开发作用域能够定位的位置。路径和名字按实际安装替换。

```json
{
  "compilerinfra": "0.2",
  "plugins": ["dialect/blocks/plugin.json"],
  "build": {
    "target": "product-opt",
    "driver": "bin/product-opt",
    "preset": "presets/default.json",
    "configure_args": ["-GNinja", "-DCMAKE_BUILD_TYPE=Release"]
  }
}
```

产品在 CMake 中先定义组件 target，然后 `compilerinfra_assemble(product_bundle PRODUCT product.json)`，或者用 `COMPONENTS` 补充现有独立组件。产品 JIT 和生成的原生 CLI 都通过 `product_bundle::link` 读取内置默认值。产品须提供安装规则，`build.driver` 是相对于安装前缀的可执行文件路径。动态库和工具链按产品的正常安装约定解析。多组件安装可用 `build.component` 指定 CMake install component。源码候选从私有快照增量构建，初始作用域也据此构建基线。已有与源码对应的整套安装时可设置 `base_install` 为其绝对前缀，`--driver` 必须在该前缀下，作用域复制这份安装。无源码构建配置时直接使用所给工具并保留一份可执行文件，系统 SDK 和外部依赖由调用环境提供。

## 插件源码包

每个描述文件只引用所属包的资源和真实 CMake target。开发脚本通过公开注册对象取得当前候选。

```json
{
  "id": "product.blocks",
  "dependencies": [],
  "native": ["block_decisions"],
  "actions": "actions.py:register",
  "skills": {"read-block": "read-block.md"}
}
```

`id` 必填。只提供 skill、服务、宿主工具或原生代码都可以。插件依赖先于使用者激活；重复包、缺少依赖和循环均会报错。原生 target 由 `compilerinfra_add_component` 提供真实登记函数。模型、路线、pass 顺序和硬件决策只在原生实现中定义。含 development／measurement 依赖的包需要对应的宿主组合。

Python 插件的 `register(registry)` 接收候选作用域，支持下面的操作。

| 接口 | 合同 |
| --- | --- |
| `@registry.action` | 注册有类型注解、具名参数的同步或异步函数；参数签名生成实际调用 schema |
| `registry.compiler` | 当前候选、产品配置、原生查询与编译入口 |
| `registry.provide(name, service)` | 提供该作用域内唯一的服务；依赖它的插件通过 `registry.services[name]` 取得 |
| `registry.on_close(cleanup)` | 注册同步释放动作，卸载时按逆序调用 |
| `inspect_compiler` / `read_skill` | 查询所属插件及 schema，按包名和 skill 名读取当前候选的内容 |

基线插件的动作直接成为 CLI 和 MCP 工具。候选新增加的动作用 `call_action(name, arguments, compiler)` 按自身 schema 调用，保持其他候选的工具集。模块支持包内相对导入，每个候选独立加载和释放。开发插件是受信任的本地源码。

## DSH 宿主

把 `hosts/dsh/` 作为 npm 源码包装入 DSH 工程，加载 `compilerinfra-dsh`，注入 DSH 的 `tools` 和 `skills` 服务。插件配置提供 `driver`、`workspace`、`outputs`，可指定 `product`、`bundle`、`namespace`、`launcher` 和 `driverArgs`。例如设置 `bundle: "measurement"` 会同时提供编译和测量动作。DSH 使用自己的 MCP client 注册类型化工具，并将选中组合的 skill 登记到当前 DSH 作用域。候选专属内容可按 `compiler` 引用读取。卸载该插件释放工具、skill 与编译会话。

## 接入独立 Bench

Intent 产品插件可以绑定 `intentdsl.bench.worker(compiler, interpreter, scratch)`，返回使用该候选完整安装的本机 Python 入口。测量动作从 Bench 读取所选环境，保留设备与工具链设置，让控制端和 worker 都从这份安装导入前端、runtime 并使用候选 preset。远端候选在实际主机打开作用域，避免将控制机的安装路径当作远端可执行文件。已有仅选择原生编译工具的安装沿用显式 `--compiler`。输出根也可由 `INTENT_BENCH_OUTPUT_ROOT` 指定，短根下的临时路径可用于 cuTile 的 Unix socket。

使用 `measurement` 组合并在产品配置的 `bench` 对象内配置相应工具。结果路径由 Bench 接收，保存在其输出根下。基座不实现采样、设备选择、数值裁判或 baseline。

| 产品配置键 | 所需绑定与实际调用 |
| --- | --- |
| `intentdsl` | `executable`、`output_root`、`base_compiler` 和相对安装路径 `installed_compiler`。测量动作将当前候选的 **Intent 产品编译器** 传给 `intent-bench run --compiler`，并转交 workload、environment、case、implementations 和 baseline。配置型候选使用带有其 preset 的产品入口 |
| `triton-weft` | `executable`、`output_root` 和可选 `config`。产品插件绑定异步服务 `triton-weft.export(compiler, workload, target, inputs)`，用候选的公开产品入口生成完整 AOT 目录。`measure_weft` 把这个目录和同一输入交给 `weft-bench op --weft` |

接入产品时应提供 Bench 原本可调用的编译入口、安装路径或导出服务。Intent 的原生开发驱动与 Bench 使用的 Intent CLI 可以是不同的可执行文件，但必须来自同一候选安装并支持同一套产品 preset。宿主配置的输出名相对所选 Bench 输出根解析。
