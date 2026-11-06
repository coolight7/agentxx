# 任务卡片: 展示历史单写者 (viewMessage-protocol)

## 目标

把"一条展示消息 (ViewMessage) 怎么成型"从 agent/client 两处收敛到 agent 一处：

- agent 端决定消息边界、正文归组、耗时、可见性、折叠默认值与 id；
- 客户端只做副本应用（按 id 追加文本 / 整条 upsert / 丢弃流式项），不再有归组/合并/回填启发式；
- 协议只做固定操作集合 + 分段整条检查点（丢包、乱序、重连都能收敛）；
- 本轮不做旧版本兼容：wire 协议 v4（`kVersion`/`kMinVersion` 同抬）、FFI API v3，旧路径全删。

## 范围

| 层 | 文件 |
|---|---|
| 协议 | `agent/lib/include/agentxx/agent/conversation_types.h`、`agent/io/wire_protocol.h`、`agent/io/agent_io_transport.h` |
| agent 侧 | `agent/lib/include/agentxx/agent/context.h`、`src/agent/context.cpp`、`include/agentxx/event/event_stream.h`、`src/event/event_stream.cpp`、`src/agent/base_agent.cpp`、`src/agent/io/session_server_agent_io.cpp` |
| 客户端副本 | 新增 `agent/lib/include/agentxx/agent/io/view_replica.h` + `src/agent/io/view_replica.cpp` |
| TUI | `agent/client/{include,src}/io/tui/agent_tui.*`、`include/agentxx-client/io/tui/framework/tui_state.h`、`src/io/tui/components/message_list.cpp` |
| 其它端点 | `agent/client/src/io/stdio/agent_stdio.cpp`、`agent/lib/src/agent/io/embedded_client_agent_io.cpp`、`agent/lib/src/ffi/ffi_client_io.cpp`、`agent/lib/include/agentxx/ffi_api.h`、`agent/lib/src/plugins/plugin_channel_agent_io.cpp` |
| 测试 | `view_replica`(新) / `event_bridge` / `session_schema` / `session_persistence` / `wire_schema` / `wire_roundtrip` / `remote_agent` / `tui_stream` / `tui_scroll` / `tui_widget` / `tui_tool_header` / `tui_form` / `tui_interrupt` / `ffi_c_api` / `plugin_agent_runtime` / `plugin_example_runtime` / `jsonl_mode` / `jsonl_runner` / `boundaries` |
| 文档 | `docs/zh-cn/design/{index,tui,plugins,events}.md`、`docs/en/design/index.md`、`AGENTS.md` |

## 状态

规划完成，等待开工（实施进度见 [work.md](./work.md)）。

## 关键决策

1. **直播槽不进 `viewMessages`**: `Session::streamingView` 单独存，不参与链式哈希 / 持久化序号 /
   落盘 / 分页；定稿时才用预留 id 走正常 `appendViewMessage`。持久化、重连补拉、分页语义零改动。
2. **一段一行，不做跨段合并**: 思考→正文→思考→正文 交错就是 4 行；这样"流式项"永远在列表末尾，
   客户端不需要"往中间某一行追加"的能力。定稿对账只在"该角色本步骤只有一段"时用定稿文本覆盖。
3. **op 集合固定 6 个**: `stream_begin` / `text_token` / `thinking_token` / `insert_message` /
   `update_message` / `stream_end`；`ToolStart`/`ToolEnd` 删除（工具行就是普通历史行）。
   追加只传文本，其它变化传整条 → 无部分结构一致性问题。
4. **id 是一切**: 客户端自造行用 `local_` 前缀（中断表单、瞬态提示），永不被 agent 命中；
   `insert`/`update` 都是 by-id upsert（重放与增量补拉重复投递必须幂等）。
5. **折叠与今天一致**: 折叠状态只存在客户端行对象 (`msg.collapsed`) 与流式项的 `streamThinkOverride`
   上，不上报、不落库、不引入覆盖表；整条覆盖 / 整份 sync 会恢复成默认折叠态（与今天相同）。
6. **取消/出错保留半截内容**: `finishTurn()` 照常 `flushOpenStream()`，已产出的文本定稿成行、进历史并落盘，
   客户端像普通消息一样显示（与现有实现观感一致）；代价是展示历史比 LLM 上下文多一条，待文档写明。
7. **不做兼容**: 版本双抬，不新增能力位、不保留降级分支；stdio / FFI / 插件通道 / jsonl 一起改。

## 结论

待实现。完成后在此补：实际改动与偏差、验证结果、遗留事项。
