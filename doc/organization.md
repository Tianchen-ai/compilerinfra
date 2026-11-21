# 产品编译树

`product.json` 声明产品名、版本和 SDK 版本、frontend、levels、nodes、exits、runtimes、configuration、build 和 install。可另外声明 extensions 和 bench。使用 `init` 创建这份文件，用 `add-level`、`add-node`、`add-exit`、`add-runtime` 逐步补充。即使还没有安装产品，开发宿主也能调用这些动作。

级的顺序决定父子表示可以如何排列，每一级有一句职责。节点具有唯一名字、表示合同、父节点和非空有序边界。根的父为保留字 `entry`，其他节点选择更早一级上的父节点。出口从根开始，经过祖先的全部边界，止于自身声明的 `node@boundary`，依次经过出口声明的 exit@boundary，最后 emit。每个出口配一个运行时。

| 路径 | 职责及可用区域 |
|---|---|
| frontend/<language>/ | 安装包中的入口把作者程序转成根 IR 字节。entry 取 module:callable。模块路径从本单元目录起算，Python 源码随候选安装并逐文件核对摘要；候选内的模块可以用相对导入。原生绑定可以登记根输入校验及 dialect。 |
| levels/<level>/<node>/ | ir、lowering、analysis、transforms、decisions、agent。子节点拥有从父到子的 lowering。 |
| exits/<exit>/ | ir、legalize、transforms、decisions、capabilities、emit、abi、agent。一个出口独用的表示在此维护；多个出口共用的表示提升为节点。 |
| runtime/<exit>/ | binding、launch、cache。只使用所绑定出口的公开 ABI 和精简 Runtime。 |
| include/<product>/<unit>/ | 供其他单元包含的公开接口。出口 ABI 位于 exits/<exit>/abi/。 |
| agent/ | 产品级动作和 skill。纯开发扩展可使用 agent/<name>/。 |
| tools/<command>/ | 产品工具。可以使用所有编译单元。 |
| cmake/、doc/、examples/、test/ | 构建配置、手册、使用示例和按单元路径镜像的已有测试。 |

目录按需建立。一个原生单元目录调用一次 `compilerinfra_unit(SOURCES ... [LINK_LIBRARIES ...])`。目标名和 C++ 登记命名空间由路径生成，可用 `compilerinfra_target(out unit-path)` 取得 CMake 目标名。产品的根 CMakeLists 调用一次 `compilerinfra_product()`。每个节点可以依赖祖先，出口可以依赖挂接节点及其祖先，frontend 可以使用根的公开接口。配置结束时检查单元源码和传递链接闭包。库安装位置从实际构建目标取得。开发进程按安装的解释器入口启动；该环境需要已安装的 SDK 运行时和开发 worker。组件目录及登记位置相对于产品源码根，扩展路径保留其相对位置。

扩展包在产品的 extensions 数组中声明 name 和 path，包根的 product.json 可以补充 levels、nodes、exits 和 runtimes。复用已有逻辑节点或出口的目录表示对它的贡献。扩展单元以 extensions/<name>/ 开头获得单独的登记命名空间。产品与扩展各自保有 Git 根目录。
