# 修复展示历史把交错的思考/正文切成多行 (TUI 消息看起来被拆开)

- 难度: B
- 类型: bug修复
- 基于commit: 2e2f1ee92dc0ebdeb0fc5ce960b56c1d2d056a01
- 时间: 2026-11-08
- 需求:
```md
请修复 tui 消息又一次出现bug，还是llm api 返回思考和内容时交错，导致了tui增量添加消息时
也把一条 <思考+content> 但是交错返回的消息，切成了 <思考1+content1+思考2+content2>：
+ [思考] 0.9s Everything is in place. Temp logs cleaned.
崩溃根因
+ [思考] 0.2s but complete, in Chinese (user wrote in Chinese).
```

## 现象与根因

- 上游 (gpt-oss / harmony 形态, 部分网关转发的 `reasoning_content`) 会在**同一次模型调用**里
  把思考与正文分成多段交替送出: 思考 → 正文 → 思考 → 正文。
- 协议 v4 把"展示历史的唯一写者"移到 agent 端 `EventBridge`, 同时定下"一段一行" (类型切换
  即切段, 交错就是 4 行)。实际观看时: 两条 `[思考]` 折叠行夹着正文, 与"一条思考 + 一条正文"
  的预期不一致; 重开会话后历史里也是 4 条。
- v4 之前客户端 `TUIClientAgentIO::findStepMessageLocked` 做过按步骤归组 (同角色并回一条),
  v4 重构把这段删掉, 归组能力没有在 agent 端补回 —— 同一个问题第二次出现。

## 结论

- 归组回到 agent 端 `EventBridge`: 同一模型步骤 (NODE_START 到 NODE_END) 内, 思考/正文
  **各至多一条行**; 角色切回已出现过的角色时, 文本追加到那条已定稿的行上, 只下发该 id 的
  token 增量, 不新开流式段。
- 客户端 `ViewReplica` 的 token 追加从"只认直播槽 id"放宽为"直播槽优先, 否则按 id 找
  已定稿行", 与它文档里写的"按 id 追加文本"一致 (换新对象, 渲染缓存以指针失效)。
- 一条行只有一个正文槽, 所以"思考 + 正文"仍是两条行 (思考在前、正文在后), 与
  "一个 assistant 消息 = reasoning_content + content"对上; 先正文后思考的罕见顺序
  按到达顺序落行 (已知差异, 文字一致、块顺序不同)。

## 偏差

- 顺带把 `Session::enqueueViewPersist` 的"同一行连续覆盖"去重了: 交错段逐 token 追加到
  同一行会调用很多次 `updateViewMessage`, 不去重会把待落盘队列撞大、回放时重复写同一行。
  行为变化: 同一行在窗口内多次 update 只回放一次 (读的是行的当前内容, 中间状态无落库价值);
  `test_session_persistence` Case 1 的断言随之调整, 另加 Case 1b 校验不同行各自回放。
- 未改 wire 协议版本: op 集合与字段没变, 只是文本增量的目标行不再限定尾部流式项
  (只有 TUI 用副本, 与 agent 同版本发布)。

## 验证

- `event_bridge` / `view_replica` / `tui_stream` / `session_persistence` / `remote_agent` /
  `session_sync` / `tui_*` / `ui_items` / `event_*` / `async_tools` 全部通过。
- 唯一失败是 `boundaries` 规则 15 的 5 条 `载体` 用词, 全在 `third_party/cxx_pluginxx_ui`
  (与本次改动无关, 上一次提交的回归记录里已记为未修)。
