# 机器输出

JSON schema 1 顶层字段为 `schema_version/tool/artifact_id/command/query/data/
diagnostics/summary`，请求统计时另有 `stats`。stdout 必须是一个 JSON 文档。

退出码：0 完整非空；3 合法空结果；4 截断；5 编译分析失败；6 无效查询；7 节点
歧义；8 图 schema 不兼容；1/2 分别为内部错误和用法错误。0、3、4 都应解析
envelope。`complete=false` 时缩小 scope、range 或 depth 后重试，不能静默忽略。
