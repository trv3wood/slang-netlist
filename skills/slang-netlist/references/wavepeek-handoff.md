# 波形交接

静态路径在 State 停止后，记录 boundary 的信号、事件边沿、源码位置和
`artifact_id`。若环境提供 WavePeek，围绕对应边沿查询 State 输入的 pre-edge 样本
和输出的 post-edge 样本，再判断该静态候选是否在目标周期生效。

没有波形工具或用户未提供波形时，只报告“跨越该 State 的静态候选”，不得推断具体
周期、分支是否 active 或复位是否实际触发。
