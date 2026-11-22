# 候选及其生命周期

一次已安装构建与一份有效配置标识一台编译器。Compiler 持有库内的原生句柄。derive(configuration) 在原进程中建立新句柄，两者都可描述、编译和继续派生。JIT、driver 和开发 worker 调用同一组 ci_* ABI。

开发服务的 derive 支持 configuration、产品源码 patch，以及按扩展名传入的 extension_patches。源码候选有独立的源码快照、CMake 构建、安装目录和 worker。相同构建的配置候选共用 worker。父候选已打开的句柄可以继续使用。将编译输出放在产品仓之外，或者放在被 Git 忽略的 .work 下。

compile 返回 compilation_id。explain(compilation_id) 返回该次编译的候选、出口、根 IR 字节的 SHA-256、边界及事件。impact(component, compilation_id) 分列可达出口、配置选择和该次输入的实际命中。普通变换的 executed 表示是否执行，changed 表示 MLIR 指纹是否改变；关闭的变换和执行后 IR 未变的变换均不计作命中。compare 返回两次编译各自的 ID。每个句柄和开发会话各自保留最近 128 条观察，超出后先淘汰最早的记录。

普通 Python JIT 只返回产物并更新 last_compilation_id。显式传 explain=True 或通过 development 宿主编译时才采集变换前后指纹，并可用 explain(id) 回查。跨句柄恢复检查点时传 root_sha256；当前句柄能唯一识别检查点来源时自动沿用原根 IR 哈希。driver 默认向标准错误写入编译 ID，标准输出保留给产物；--explain 输出包含该 ID 的完整观察记录。

adopt 在产品工作树的锁内核对基线指纹及源码差异，将源码与有效配置写回，重新构建并安装到公共位置，再打开基线。下一次普通 JIT 打开该库即使用新默认配置。构建或激活失败会撤销此次写回。源仓的提交由维护者决定。discard 释放候选，在同一构建的最后一个句柄释放后清理临时文件。

从 0.3.4 起，成功采用候选时，产品安装目录保存上一份安装及此次源码改动的反向补丁。开发宿主的 `revert` 动作恢复这份源码和已编译的安装。每次采用保留一步撤回机会，跨开发会话有效。已有句柄继续使用打开时的编译器。

普通安装和 adopt 均先暂存新文件，再按产品的安装清单发布。清单只记录该产品拥有的路径；相邻安装不会随它回收。公共库使用原子更换的链接，已加载库保持映射，Python 前端的已校验源码随打开的句柄持有。
