# CLI 速查

```bash
slang-netlist -f files.f --top top --find '**.name*' --format json
slang-netlist -f files.f --top top --drivers top.u.sig[7:0] --format json
slang-netlist -f files.f --top top --fan-in top.u.sig --format json \
  --max-results 200 --max-depth 64
slang-netlist -f files.f --top top --from top.a --to top.b --format json \
  --cross-state never
slang-netlist -f files.f --top top --save-netlist design.netlist.json
slang-netlist --load-netlist design.netlist.json --fan-out top.req --format json
slang-report -f files.f --top top --variables --scope top.u --format json
```

表格格式供人阅读；Agent 始终使用 JSON。`--stats-json` 是兼容别名，优先使用
`--format json --stats`。
