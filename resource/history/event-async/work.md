# 实施记录: toolcall 异步挂起与事件中枢 (event-async)

方案见同目录 [plan.md](plan.md)。本文只记录**实际实施状态** (已完成 / 待完成 / 注意事项),
每完成一段任务提交一次 git。

- 开始时间: 2026-11-01
- 分支: 当前工作树
- 分四期: P1 事件中枢+等待+投递+工具 / P2 异步子代理 / P3 插件接口表 / P4 展示与诊断

## 0. 现状核对结论 (实施前逐条核对代码)

| plan 中的依据 | 核对结果 |
|---|---|
| `Session::enqueuePendingInput` / `takePendingInputs` (`context.h:498/502`) | 一致, 实现在 `agent/lib/src/agent/context.cpp`; 只有 `next-step` / `inject` 两个投递模式被取用 |
| `drainPendingSessionInputs` 在 modelcall 开头 | 一致 (`agent/lib/src/nodes/session_context.cpp:247`), `next-step` 写上下文+展示历史, `inject` 只进本次请求 |
| `SessionServerAgentIO::pushMessageQueueItem` | 一致 (`session_server_agent_io.cpp:459`); **MessageQueueItem 参与 wire 序列化**, 新增 `source` 字段不能改 JSON 输出 (会动到客户端结构), 只在服务端内部记录 |
| `run()` 驱动循环 / `turnActive_` | 一致; `turnActive()` 已有同名 public 方法 (`noexcept`), 直接作为 `SessionInputSink::turnActive` 的覆写可用, 但语义按计划改为"轮次在跑 **或** 队列还有待跑条目" |
| EventBus 前缀订阅 / typed 流 | 一致 (`event_stream.h`); `Topic::AgentTurnStart/End`、`ToolCallStart/End` 只有声明没发布 —— 本次补上 (先 `hasListeners`) |
| 插件 `qualifyEventTopic` 补 `plugin.` 前缀 | 一致, 所以 hub 必须同时监听 `evt.` 与 `plugin.evt.` 两个前缀 |
| `toolResultTextIsFailure` 前缀集合 | 一致 (`toolcall.cpp:60`), 事件投递文本以 `[Event` 开头, 不会被判为失败 |
| toolcall prepare→run→finalize 全链 await | 一致 (`toolcall.cpp:1380` 起), 因此"异步挂起"只能是"工具立即返回占位结果" |
| `AgentConfig` 无 event.* 键 | 一致, 需新增 6 个键 + `config_loader.cpp` + `test_config_keys.cpp` 目录 + 文档表 |
| 子代理同步等待 (`agent_runner.cpp` 的 `service.subagent` RR) | 一致 (P2 才动) |

补充发现 (实施时需要照顾):

1. `AgentContext` 里没有"跑一轮"的能力 (只有会话/总线/中间件), 所以
   `event.allow_headless_turn` 需要 BaseAgent 在 init 时把一个"跑一轮"的回调注入 hub,
   否则该配置项无法生效。
2. `Session` 的可变状态要求 io 线程 (`assertIoThread`), 因此 `Session::inputSink`
   的读写一律在 io 线程; hub 投递时先查 `session->isIoThread()`。
3. 会话端点 `SessionServerAgentIO` 已继承 `enable_shared_from_this`, 直接
   `weak_from_this()` 即可作为 sink; 但 `stop()` 是 `dispatch` 到 ex_ 线程执行的,
   解绑要放在 `stopImpl()`。
4. `MessageQueueItem` 出现在 `WireSyncPayload` / `WireMessageQueueUpdate` 里, 其
   `to_json`/`from_json` 在 `conversation_types.h`; `source` 不加进 JSON
   (协议不变, 客户端不受影响)。

## 1. 实施进度

### P1 事件中枢 + 等待 + 投递 + 工具

- [x] 现状核对 (本文第 0 节)
- [x] `agent/lib/include/agentxx/event/naming.h` 事件名/主题词表 (校验/前缀规范化/保留前缀)
- [x] `agent/lib/include/agentxx/agent/session_input_sink.h` 会话输入入口抽象
- [x] `agent/lib/include/agentxx/event/hub.h` + `src/event/hub.cpp` + `src/event/hub_text.cpp`
- [x] `Session::inputSink` / `Session::pendingInputCount(prefix)` / `AgentContext::eventHub`
- [x] `SessionServerAgentIO` 实现 sink + `MessageQueueItem.source` + run/switchSession/stop 绑定解绑
- [x] `BaseAgent`: `event_hub` 初始化步骤 (+回滚) / 发布 `turn.start|end` / 轮末 next-step 兜底 /
      `shutdownAsync` 里 `closeAll` / 无驱动端跑轮次回调
- [x] `ToolcallWrapNode` 发布 `tool.start` / `tool.end` (含 `tool.end.<工具名>` hub 事件)
- [x] `agentxx_event` 工具 (watch/await/status/list/cancel/emit/end) + 提示词 + 系统提示词 Context Markers
- [x] `event.*` 配置 (6 键) + `config_loader.cpp` + `test_config_keys.cpp` 目录 + 文档表 + 加载快照
- [x] 测试模块 `event_hub` / `event_tools` / `event_delivery` (192 / 80 / 44 条断言全部通过)
- [ ] 文档: `docs/zh-cn/design/events.md` (新) / index.md §4 / plugins.md / test-modules.md / AGENTS.md

### P2 子代理异步派发 + 后台工具形态

- [ ] `SubagentBatchItem` 追加 `async` / `eventName`
- [ ] `agentxx_subagent` 工具解析 + `AgentRunner` 不等 + `AgentHost` 完成后发事件
- [ ] 测试模块 `event_subagent`

### P3 插件接口表 `agentxx.agent.event`

- [ ] 新增接口表 + SDK + `boundaries` 台账 + 示例插件 + 文档

### P4 展示与诊断

- [ ] `KeyMetrics` 事件计数 / 诊断快照 `eventHub` 段

## 2. 测试现状 (2026-11-01)

| 模块 | 结果 | 耗时 |
|---|---|---|
| `event_hub` | passed=192 failed=0 | 0.65 s |
| `event_tools` | passed=80 failed=0 | 0.31 s |
| `event_delivery` | passed=44 failed=0 | 17.4 s (每个用例起一次 HTTP 模拟服务 + 端点) |
| `config_keys` | passed=196 failed=0 (生成物已重生成) | 0.8 s |
| `boundaries` | passed=18 failed=1 | 13 s |

`boundaries` 的失败是**既有问题**, 与本期改动无关: 规则 15 (称呼与用词) 命中
`agent/third_party/cxx_pluginxx_ui` 里 5 处 `载体` (README.md / docs/integration.md /
include/pluginxx/ui/item.h)。该目录是**独立仓库** (嵌套 git 仓库, HEAD = c40759a),
按约定不在本次修改; 如需片子干净, 应到那个仓库改文案后更新子模块指针。

## 3. 注意事项 (实施中记下的坑)

1. **投递文本前缀**: 必须 `[Event` 开头, 且不得命中 `toolResultTextIsFailure` 的四个失败前缀。
2. **不让 hub 自激**: hub 自己的生命周期事件发布在 `agent.hub.*`, 不带 `evt.` 前缀,
   否则会被自己的前缀订阅再吃回去。
3. **`next-step` 残留兜底**: `runTurnAsync` 轮末把残留 `next-step` 提升为下一轮输入,
   但**只在有驱动端 (sink) 时**提升 —— 否则会把输入丢掉 (headless 场景没人接)。
4. **`event.allow_headless_turn`** 需要 BaseAgent 注入"跑一轮"回调才生效, 默认关。
5. **预算口径**: 事件 256 / 等待 256 / 单会话等待 32 / 单会话待运行事件输入 64 /
   单值 32 KB (超阈值转 share store) / 硬上限 1 MB。
6. 事件值一律按 JSON 存; 非 JSON 文本按 `{"text": "<payload>"}` 处理。
7. `Session::inputSink` 是 `weak_ptr`, 端点停止时必须清空, 否则会话会握着已停的端点
   (`submitTurnInput` 会被拒 + 计数丢弃)。
8. **待注入输入只在 modelcall 开头被取用**: 想造出"轮末残留"来测轮末兜底, 不能
   在开轮之前入队 (那样第一条 modelcall 就把它取走了), 必须等本轮请求已经发出
   (等 `g_da_sim_request_count` 增长, 同时把 `g_da_sim_delay_ms` 调大保持请求在飞)
   再入队。现象: 只跑一轮、`turnCount() == 1`。
9. **`event.*` 键要进 `config_keys` 目录**: 加载器解析的键名若没登记到
   `configKeyEntries()`, 该模块会直接失败 (`parses key(s) not registered`)。
   新增后要 `AGENTXX_UPDATE_CONFIG_KEYS=1 agentxx_test config_keys` 重新生成
   `agent/schema/config-keys.json` 与 `docs/zh-cn/design/config-keys.md` (生成物, 不改手写部分)。
10. **列表文本断言看整段不看前缀**: `opt=list` 的事件列表行本身带 `waits=N`,
    `scope=events` 时只能断言"没有 `[Event list] waits=` 段标题", 不能断言不含 `waits=`。
11. **行尾**: 本仓库部分文件是 CRLF (`conversation_types.h` 原本 904 行 CRLF),
    `agentxx_filesystem_edit` 会把整文件转成 LF, 造成整文件 diff; 该文件已在 P1 提交里
    变为 LF (内容未变), 后续编辑不会再产生同样的噪声。
