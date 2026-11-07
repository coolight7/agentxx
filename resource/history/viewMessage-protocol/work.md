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
    - `plugin_channel_agent_io.cpp`: `emitDeltaText(msgId, kind, text)`; TURN_START/TOOL_START/TOOL_END
      由 Insert/Update 行派生 (工具行插入=调用开始, 更新=结果到达)
    - `agentxx_planning` 插件 client 侧: 从工具行推导装饰 (原先等 tool_start/tool_end 增量)
- [x] S6 测试改造
    - 新增 `agent/test/core/test_view_replica.cpp` (13 组, 108 条断言)
    - 重写 `test_tui_stream.cpp` (每个 op 的副本效果 + 定稿收敛 + 本地行)
    - `test_tui_scroll/…/tui_interrupt/tui_tool_header` 夹具改为经 `insert_message` 写入副本,
      流式项用 `stream_begin`/token; `test_event_bridge`/`wire_schema`/`remote_agent`/
      `session_runtime`/`client_plugins`/`agent` 断言改到新增量集合
    - `wire-schema.json` 与 `wire-protocol-fields.md` 重新生成; `test-modules.md` 重新生成
- [x] S7 文档与 AGENTS.md
    - `docs/zh-cn/design/index.md` (Delta 章节重写 + 新增"展示历史 (viewMessages) 与客户端副本"),
      `tui.md` (§2.14 占位/§2.19 分段与消息成型重写), `plugins.md` (装饰推送来源/通道事件字段),
      `ffi.md` (API v3 + 事件字段), `docs/en/design/{index,ffi}.md`, `AGENTS.md` (新增"展示历史归属"一节)
- [x] S8 全量回归 (基准抽查未做, 见"剩余事项")

## 验证清单

| 模块 | 命令 | 结果 |
|---|---|---|
| 编译 (Debug) | `cmake --build agent/build/windows-debug --config Debug` | 通过 (无 error) |
| 全量测试 | `agentxx_test --log-level warn` | 通过 42115 条; 仅 `boundaries` 规则 15 失败 5 条 (全部在 `agent/third_party/cxx_pluginxx_ui` 那个独立仓库的文档/注释里, 与本次改动无关, 见"剩余事项") |
| 协议与事件 | `agentxx_test event_bridge wire_schema wire_roundtrip session_schema session_persistence session_sync` | 全部 0 failed |
| 副本语义 | `agentxx_test view_replica` | 108 passed / 0 failed |
| TUI | `agentxx_test tui_stream tui_scroll tui_interrupt tui_tool_header tui_form tui_widget tui_surface tui_theme tui_settings` | 全部 0 failed |
| FFI / jsonl | `agentxx_test ffi_c_api jsonl_mode jsonl_runner` | 0 failed |
| agent 端到端 | `agentxx_test agent` | 198 passed / 0 failed |
| 插件 | `pwsh -File agent/script/build_plugins.ps1 agentxx_planning` + `agentxx_test plugin_*` (全量运行内) | 通过 |

## 偏差与注意事项

- 删除 thinkSeg* 段计时: 段耗时改由 `flushOpenStream()` 用段起止时间直接写行 (`durationMs`),
  不再有"结算包"式空 `ThinkToken` 增量 (协议里 token 不带耗时)
- `WireDelta` 保留 `toolName` (GenStatus 的"正在调用工具"需要), 其余工具字段 (toolCallId/
  arguments/result/status) 全部删除 —— 工具行是普通历史行 (`insert_message` / `update_message`)
- `stream_end` 只在整段视觉为空时用 (空段不留行); 非空段一律定稿成行
- 取消/出错的半截内容定稿成行并落盘: 展示历史比 LLM 上下文多一条, 已在 index.md 写明
- 折叠默认值从"客户端 `pushCurrentTokenLocked` 决定"改为 agent 端 `defaultCollapsedOf(role)`;
  客户端只在"定稿行到达"时把用户对当前流的点击覆盖写回该行
- `reconcileSegment` 的时间戳语义: 只有"本步骤没有流式段"(非流式/网关缓冲)才按节点计时回退;
  已有段时只接受消息 JSON 里显式给出的 `startTimeMs`/`durationMs` (否则会多发一条无意义增量,
  并把流式段的起始时间改成节点开始时间)
- 整份同步 (onSync) 会重建副本 => 本地行 (中断表单/瞬时提示) 随界面态丢弃, 与改造前一致;
  `ViewReplica::reset` 自身保留"副本里已有的本地行", 这条性质由 `view_replica` 模块覆盖
- `third_party/cxx_pluginxx_ui/...` 里 5 处"载体"是那个独立仓库的历史用词, 本次未改
  (`boundaries` 规则 15 因此仍报 5 条); 需要在该库仓库自身修掉
- 基准抽查 (S8 后半) 未做: `agentxx_benchmark` 只在 Release 配置编译, 本次验证在 Debug;
  协议层每 token 的 JSON 体积比改造前更小 (无 historyCount/tailHash/toolCallId 等冗余字段),
  客户端每 token 不再复制整段 token 文本以外的新增开销 (COW 与改造前同款)

## 剩余事项

1. `agent/third_party/cxx_pluginxx_ui` 仓库内 5 处 `载体` 用词待那个仓库自行修掉 (本仓库不改第三方)
2. Release 配置的基准抽查 (TUI 帧耗时 / 逻辑内存) 未做, 需要在 Release 构建下跑
   `agentxx_benchmark` 的 `resource*` 场景与改动前对比
3. 后续如需"重连时保留中断表单", 需要让 onSync 把旧副本的本地行搬进新副本
   (现在直接丢弃, 与改造前一致)
