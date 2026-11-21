# 候选及其生命周期

一次已安装构建与一份有效配置标识一台编译器。Compiler 持有库内的原生句柄。derive(configuration) 在原进程中建立新句柄，两者都可描述、编译和继续派生。JIT、driver 和开发 worker 调用同一组 ci_* ABI。

开发服务的 derive 支持 configuration、产品源码 patch，以及按扩展名传入的 extension_patches。源码候选有独立的源码快照、CMake 构建、安装目录和 worker。相同构建的配置候选共用 worker。父候选已打开的句柄可以继续使用。将编译输出放在产品仓之外，或者放在被 Git 忽略的 .work 下。

compare 在同一输入和具名边界比较两个候选的实际 IR。explain 从一次编译获取真实决策事件。impact 分列组件可到达的出口、配置实际选中的出口、最近一次 explain 中观察到的命中。没有该次观察时，hits 为 null。

adopt 在产品工作树的锁内核对基线指纹及源码差异，将源码与有效配置写回，重新构建并安装到公共位置，再打开基线。下一次普通 JIT 打开该库即使用新默认配置。构建或激活失败会撤销此次写回。源仓的提交由维护者决定。discard 释放候选，在同一构建的最后一个句柄释放后清理临时文件。
