# 图语义

节点是源码级端口、赋值、条件、合流、状态和常量，不是综合门级单元。边的 `role`
区分 data、control、index、address、event、clock、reset 和 port_connection。

精度从强到弱：

- `exact`：逐 bit 映射已证明。
- `range`：只保证连续范围。
- `signal`：动态选择或 opaque 表达式使整个信号成为候选。
- `unknown`：旧工件或无法判定。

`edge_kind` 表示 posedge/negedge 等事件边沿。只有明确语法证据才会把 event 细分为
clock/reset。State 是时序边界；默认 path 和组合 cone 不穿过它。
