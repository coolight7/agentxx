# 实施记录: 展示历史单写者 (viewMessage-protocol)

方案见 [plan.md](./plan.md)。每完成一步就补"改了什么 + 怎么验证", 不攒到最后。

## 进度

- [x] S1 协议与模型 + `ViewReplica` + 版本抬升
    - `conversation_types.h`: `WireDelta::Type` 收敛为 12 个 (StreamBegin/TextToken/ThinkToken/
      InsertMessage/UpdateMessage/StreamEnd/TurnStart/TurnEnd/NodeStart/NodeEnd/MessageUITip/GenStatus);
      字段只剩 text/msgId/toolName/nodeName/seq/startTimeMs/durationMs/tps/message/streamRole/think/
      type/tipType/genPhase; `WireSyncPayload` 增 `std::optional<ViewMessage> streaming`
    - `wire_protocol.h`: `deltaTypeToString/FromString`、`deltaToJson/FromJson`、`syncToJson/FromJson` 改写
    - `agent_io_transport.h`: `WireProtocol::kVersion = 4` / `kMinVersion = 4`
    - 新增 `agent/lib/{include/agentxx/agent/io,src/agent/io}/view_replica.*`:
      `ViewStreamTail` + `kLocalRowIdPrefix` + `ViewReplica` (applyDelta/reset/appendIncremental/
      prependRows/trimHead/upsertLocalRow/seedStream/clearStream/replaceRow/setRowCollapsed/
      rows/rowById/stream/hasStream/lastViewSeq, 内部 id→下标索引)
- [x] S2 `Session` 直播槽 + `appendViewMessage(reuseId)`
    - `AgentContext`/`Session`: `streamingView` + `beginStreamingView/appendStreamingText/
      mergeStreamingThink/finalizeStreamingView/clearStreamingView`, `appendViewMessage(msg, reuseId)`;
      `restore()` 清空直播槽
- [x] S3 `EventBridge` 分段/定稿/对账 + `BaseAgent` 时序 + `SessionServerAgentIO`
    - `event_stream.{h,cpp}`: `OpenStreamKind`/`StepRow`/`beginStream/appendStreamText/mergeStreamThink/
      commitStreamRow/flushOpenStream/finishTurn`; 删 `finalizeThinkSegment` 与 thinkSeg 计时
      (段耗时改由 `flushOpenStream` 用段起止直接写行); `handleLLMToken` 按角色切段;
      `handleChannelWrite` 开头 `flushOpenStream()`, assistant 走 `reconcileSegment`
      (无段→新行 / 单段→整条覆盖同 id / 多段→只并元数据到第一段), tool_calls→InsertMessage,
      工具结果→UpdateMessage; `handleNodeStart` 清 stepRows, `handleNodeEnd`/`handleError` flush
    - `base_agent.cpp`: 用户行先 `InsertMessage` (副本清空 attachments.dataUrl) 后 `TurnStart{startTimeMs}`;
      `finishTurn()` 放在 `persistNow("turn-end")` 之前; `TurnEnd` 去掉 tailHash/historyCount
    - `session_server_agent_io.cpp`: `buildFullSync`/`buildTailSync` 带 `p.streaming = sess->streamingView`;
      `sendToPeer` 用 `deltaTooLargeForReplay` (64 KiB 上限) 跳过入重放缓冲
- [x] S4 TUI 换副本 + `onDelta` 收敛 + 消息列表流式/折叠
    - `tui_state.h`: `messages` → `ViewReplica replica` + `messages()`/`stream()` 只读访问器,
      删 currentToken*, 新增 `localRowSeq`, `mutableMessage` 改 `replica.replaceRow`
    - `agent_tui.cpp`: `onDelta` 只做 6 个消息级 op 交 replica + 界面状态 (tip/gen/折叠覆盖/吸底);
      `onSync` 用 `replica.reset` + streaming 种子; 删 pushCurrentTokenLocked/resetTrailingRunningToolsLocked/
      findStepMessageLocked/mergeThinkData/isBlankContentMessage; 中断表单 id = `local_interrupt_<wireId>`;
      本地提示行 id = `local_tip_<n>`
    - `message_list.cpp`: 读 `messages()`/`st.stream()`, `streamEpoch_` → `streamId_`
- [x] S5 stdio / FFI / 插件通道 / jsonl
    - `agent_stdio.cpp`: 改为"打印一次"语义 (streamId_/streamPrintedLen_/printRow)
    - `embedded_client_agent_io.cpp`: 合批键改 `(msgId, kind)`
    - `ffi_client_io.cpp`: `emitDeltaText(msgId, kind, text)`; `ffi_api.h` 抬到 v3 + `AGENTXX_FFI_CAP_VIEW_REPLICA`
    - `plugin_channel_agent_io.cpp`: `emitDeltaText(msgId, kind, text)`; 事件由 Insert/Update 行派生
- [ ] S6 测试改造 (进行中)
    - [x] `test_tui_stream.cpp` 重写 (20 个场景 + markdown 场景)
    - [x] `test_tui_scroll.cpp` / `test_tui_interrupt.cpp` / `test_tui_tool_header.cpp` 夹具改副本写入
    - [ ] core 侧 `event_bridge` / `wire_schema` / `wire_roundtrip` / `session_*` / `remote_agent` /
      `ffi_c_api` / `jsonl_*` / `plugin_*` / `memgrowth`
    - [ ] 新增 `view_replica` 测试模块
- [ ] S7 文档与 AGENTS.md
- [ ] S8 全量回归 + 基准抽查

## 验证清单

| 模块 | 命令 | 结果 |
|---|---|---|
| 编译 (lib+client) | `cmake --build agent/build/windows-debug --config Debug` | lib/client 通过; 测试待改 |

## 偏差与注意事项

- 删除 thinkSeg* 段计时: 段耗时改由 `flushOpenStream()` 用段起止时间直接写行 (`durationMs`),
  不再有"结算包"式空 `ThinkToken` 增量
- `WireDelta` 保留 `toolName` (GenStatus 的"正在调用工具"需要), 其余工具字段 (toolCallId/
  arguments/result/status) 全部删除 —— 工具行是普通历史行 (`insert_message` / `update_message`)
- `stream_end` 只在整段视觉为空时用 (空段不留行); 非空段一律定稿成行
- 取消/出错的半截内容定稿成行并落盘: 展示历史比 LLM 上下文多一条, 需在文档写明
