# 实施记录: 展示历史按模型步骤归组 (思考/正文交错)

## 当前进度

- [x] 现状调查 (v4 分段逻辑 / 客户端副本语义)
- [x] EventBridge 按步骤归组
- [x] ViewReplica 支持 token 落到已定稿行
- [x] 测试 (event_bridge / view_replica / tui_stream / session_persistence)
- [x] 文档 (index.md Delta 段 / tui.md §2.19 / wire-protocol-fields.md)
- [x] 编译与相关模块回归

## 每步改动与验证

### 1. EventBridge 按模型步骤归组 (agent 侧)

文件: `agent/lib/src/event/event_stream.cpp`, `agent/lib/include/agentxx/event/event_stream.h`

- `handleLLMToken` 改为经 `routeStreamText` / `routeStreamThink` 决定文字落到哪条行:
  - 直播槽正是该角色 → 追加到直播槽 (原行为);
  - 该角色在本步骤已有行 (交错返回的后一段) → `appendToStepRow`: 追加到那条行、
    刷新行耗时、下发同 id 的 token 增量;
  - 该角色在本步骤还没出现 → `beginStream` 定稿当前段并新开一段。
- 新增 `findStepRow(role)`: 只在 `stepRows_` (本步骤已定稿行) 里按角色找, NODE_START 清空。
- `routeStreamThink`: 元数据包 (加密思考/token 统计) 落到已定稿的思考行时整条覆盖下发
  `update_message` (低频); 没出现时才新开思考段。
- `beginStream` / `flushOpenStream` / `commitStreamRow` / 定稿对账 (`reconcileSegment`) 未改逻辑,
  一条行只有一个正文槽, 所以仍是"思考行 + 正文行"两条 (与 LLM 上下文一一对应)。

### 2. Session 待落盘队列去重 (避免流式逐 token 追加撞大队列)

文件: `agent/lib/src/agent/context.cpp`

- `enqueueViewPersist`: 同一行 (同一下标) 的"覆盖"操作只保留一条 —— 回放时读的是行的当前内容,
  中间状态没有落库价值; 交错段逐 token 追加到同一行会调用很多次 `updateViewMessage`。

### 3. 客户端副本按 id 追加 (TUI)

文件: `agent/lib/src/agent/io/view_replica.cpp`, `agent/lib/include/agentxx/agent/io/view_replica.h`

- `appendToken`: `stream_.id` 不符时先按 id 找已落定的行 —— 找到就换新对象追加
  (渲染缓存以消息指针失效), 找不到才按"stream_begin 丢失"自建流式项。
- 只影响 TUI (`agent_tui.cpp` 是唯一用 `ViewReplica` 的端点); stdio 等线性输出客户端不变。

### 4. 测试

- `agent/test/core/test_event_bridge.cpp`: 新增 `test_eventbridge_interleaved_segments`
  (交错两轮 + messages 定稿对账; 无 messages 写入的节点结束路径)。
- `agent/test/core/test_view_replica.cpp`: 用例 5 改为"同 id 的迟到 token 追加到已落定行",
  并覆盖"已落定行 + 新的直播槽共存"。
- `agent/test/client/test_tui_stream.cpp`: 场景 17 改为按 agent 新下发的 op 序列断言
  (两条行, 文本合并)。
- `agent/test/core/test_session_persistence.cpp`: Case 1 改为"同一行的连续覆盖只回放一次",
  新增 Case 1b (不同行各自回放)。

### 5. 文档

- `docs/zh-cn/design/index.md`: "展示历史与客户端副本" 段由"一段一行"改为"按模型步骤归组"。
- `docs/zh-cn/design/tui.md` §2.19: 归组规则、直播槽、客户端按 id 追加、已知差异。
- `docs/zh-cn/design/wire-protocol-fields.md`: 文本增量目标行可以是已落定的行。

## 验证

- `agentxx_test --log-level warn event_bridge view_replica tui_stream`
  → passed=523 failed=0
- `agentxx_test --log-level warn session_persistence event_bridge view_replica tui_stream`
  → passed=1167 failed=0
- `agentxx_test --log-level warn session_persistence event_stream event_tools tui_widget tui_form
   tui_surface ui_items tui_ui_items async_tools boundaries`
  → 仅 `boundaries` 失败 5 条 (全部在 `agent/third_party/cxx_pluginxx_ui` 的"载体"用词,
     与本次改动无关; 上一次提交的回归记录里也记着这条未修)
- `agentxx_test --log-level warn ... remote_agent session_sync` → 通过
  (remote_agent 551 / session_sync 30)

## 遇到的问题 / 注意事项

- 定稿前只有思考行进 `viewMessages`, 正文行还在直播槽: 单测断言要按这个时点写
  (先断言 1 条, messages 通道写入后再断言 2 条)。
- `boundaries` 规则 15 会扫 `docs/`: 新增文字不能出现"载体/链路/落地"这类停用词。
