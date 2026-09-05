---
name: slang-netlist
description: Analyze SystemVerilog RTL static connectivity, drivers, bounded fan-in/fan-out, paths, combinational loops, and sequential boundaries with slang-netlist. Use for RTL root-cause and dependency questions; do not use it to claim runtime activity without waveform evidence.
---

# Slang Netlist

使用 `slang-netlist` 0.12.x 的 JSON schema 1 查询 RTL 静态依赖。开始前运行
`slang-netlist --version`；版本或 schema 不兼容时停止并说明问题。

## 工作流

1. 不确定层次名时先用 `--find '<pattern>' --format json`。
2. driver 问题使用 `--drivers`；组合根因范围使用 `--fan-in`；影响范围使用
   `--fan-out`；两点关系使用 `--from/--to`。
3. 所有 Agent 查询使用 `--format json --max-results 200 --max-depth 64`，并检查
   `summary.complete`、`diagnostics` 和每条边的 `precision`。
4. 多轮查询先显式生成 `--save-netlist`，之后用 `--load-netlist`；节点 ID 只能在
   相同 `artifact_id` 中复用。
5. 默认不跨 State。遇到 sequential boundary 时停止静态归因；只有用户要求结构
   可达性时才使用 `--cross-state once|unlimited`。

静态可达不表示该路径在某次运行中生效。`precision=signal|unknown` 只能报告为候选
依赖，不能称为精确根因。

按任务读取：

- 查询选择：[references/query-selection.md](references/query-selection.md)
- 节点、边、精度和时序：[references/graph-semantics.md](references/graph-semantics.md)
- JSON 契约与退出码：[references/machine-output.md](references/machine-output.md)
- CLI 参数：[references/cli-reference.md](references/cli-reference.md)
- 跨寄存器波形交接：[references/wavepeek-handoff.md](references/wavepeek-handoff.md)
