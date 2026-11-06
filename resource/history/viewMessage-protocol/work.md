# 实施记录: 展示历史单写者 (viewMessage-protocol)

方案见 [plan.md](./plan.md)。每完成一步就补"改了什么 + 怎么验证", 不攒到最后。

## 进度

- [ ] S1 协议与模型 + `ViewReplica` + 版本抬升
- [ ] S2 `Session` 直播槽 + `appendViewMessage(reuseId)`
- [ ] S3 `EventBridge` 分段/定稿/对账 + `BaseAgent` 时序 + `SessionServerAgentIO`
- [ ] S4 TUI 换副本 + `onDelta` 收敛 + 消息列表流式/折叠
- [ ] S5 stdio / FFI / 插件通道 / jsonl
- [ ] S6 文档与 AGENTS.md
- [ ] S7 全量回归 + 基准抽查

## 验证清单

| 模块 | 命令 | 结果 |
|---|---|---|
| （待填） | | |

## 偏差与注意事项

- （待填）
