# 比较并积累一种结构优化

1. 用 layout 找到决定所属的节点，在产品的 doc/design/performance-<node>.md 写下能被实测区分的性能假设、估算项和适用程序。
2. 用 derive 派生另一个合法结构、估算模型或校准参数。保持作者程序、输入、数值选项与基线一致。
3. compile 时请求 explain。记录候选 ID、适用域、预测的瓶颈、机器事实版本和保留理由。compare 检查两个编译器在兑现边界生成的实际 IR。
4. 对父、子分别调用 measure。用同一独立 Bench 检验数值并记录完整调用、winner 和产物 SHA-256。跨拓扑用独立候选各自运行。
5. 用 model-accuracy 读取 Bench 的交换记录（测量协议通过 check_protocol 与运行回执核对），分别查看校准与留出集的排序、top-k 和所选／最佳耗时。用 inspect readiness 检查各出口校准来源、留出误差、回滚指纹与缺项。检查单程序回退以及冷启动工作量。
6. 根据实测 adopt 或 discard。采用时在正常提交说明中附结果路径或 ID、指标、收益和适用范围。校准数据在对应出口随源码交付。
