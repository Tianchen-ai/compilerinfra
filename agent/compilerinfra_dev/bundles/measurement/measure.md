# 比较候选

用 inspect_compiler 核对候选及可用的测量动作。将执行环境、已有 workload、输入和同设备 baseline 交给产品的 Bench。原生编译、首次调优、热调用和数值检查分别阅读 Bench 的结果。按设备占用约定避让外部负载；编译器返回的 IR 差异用于定位，速度来自 Bench 的完整调用计时。
