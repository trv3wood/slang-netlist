# 查询选择

- 名称未知或可能有 generate/interface 层次：先 `--find`。
- “谁直接驱动这些位”：`--drivers signal[hi:lo]`。
- “哪些输入、状态或条件可能影响它”：`--fan-in`。
- “该信号可能影响哪里”：`--fan-out`。
- “A 是否能静态到达 B”：`--from A --to B`。
- “由什么时钟或事件控制”：`--sensitivity`。
- “是否完全由常量驱动”：`--constant-drivers`。

同一路径可能有 Port 和 State。先查看 find 返回的 `id/kind/bounds`，节点级问题再用
明确的节点选择参数。不要把空结果自动解释成没有硬件关系；同时检查 diagnostics。
