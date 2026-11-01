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
- [x] 测试模块 `event_hub` / `event_tools` / `event_delivery` (204 / 80 / 45 条断言全部通过)
- [x] 文档: `docs/zh-cn/design/events.md` (新) + index.md / plugins.md / test-modules.md /
      AGENTS.md / config-keys.md (生成物)

### P2 子代理异步派发 + 后台工具形态

- [x] `SubagentBatchItem` 追加 `async` / `eventName`; `RespSubagentBatchItem` 追加
      `started` / `eventName` / `subagent`
- [x] `tools/subagent.cpp`: 解析 `async` / `event_name` + schema + 传给中断参数;
      非 toolcall 路径 (压缩等) 用 `async` 直接报参数错误
- [x] `agent_runner.cpp`: **无需改动** —— async 任务在 `spawnBatch` 里就被立即受理,
      纯异步批次直接返回 (同步任务照旧等结果)
- [x] `agent_host.cpp`: async 受理 + 作业登记/取消 + 完成后 `postValue`/`postEnd`;
      子代理上下文继承父 hub (`subCtx->eventHub = parentCtx->eventHub`, 嵌套不覆盖)
- [x] `BaseAgent::shutdownAsync`: 先 `host->cancelAsyncJobs(ctx)` 再 `hub->closeAll`
- [x] 提示词 (`prompt.cpp`): `agentxx_subagent` 的 `async` / `event_name` 参数说明与
      depict 补充; 工具结果文本 `[Subagent started] ...`
- [x] 测试模块 `event_subagent` (52 条断言全部通过)
- [ ] 插件 `agentxx_execute_command` 的 `background:true` (后台命令 → 事件)

实现要点 (与 plan 的差异):

- plan 写“AgentRunner 不等 spawnBatch”, 实际做在 `spawnBatch` 内部: 同一批里同步任务
  照旧等, 异步任务立即返回 `{started:true, eventName}`。这样 `service.subagent` 只有
  一条路径, 混合批次 (同步 + 异步) 也天然成立。
- 异步作业的取消令牌**新建一个** (不复用轮次令牌): 轮次结束/取消不应带走后台任务;
  由 `AgentHost::cancelAsyncJobs(ctx)` (agent 关停) 与 `cancelAsyncJobsOfSession` 驱动。
- 作业跑在父事件中枢的 executor 上 (收尾发布与中枢状态同线程), 结果发布用
  `postValue`/`postEnd` (任意线程安全)。
- 事件名默认 `subagent.done.<result_id>` (非法字符换 `_`), 空 id 用任务序号; 同一批内
  重名直接报错 (否则后到的值会把前一个等待提前满足)。

### P3 插件接口表 `agentxx.agent.event`

**本期未实施** (原因与替代方案见第 4 节)。

- [ ] 新增接口表 + SDK + `boundaries` 台账 + 示例插件 + 文档

### P4 展示与诊断

- [x] `KeyMetrics`: `events.delivered` / `events.dropped` / `events.wait_timeouts` 三个计数
      (hub 投递/丢弃/等待超时三处接入)
- [x] 加载快照新增 `event_hub` 段 (`statsJson`), `renderAssemblySnapshot` 输出 `event_hub:` 与
      `metrics` 下的 `events:` 行
- [x] `--dump-diagnostics` 新增 `## event hub` 段 (JSON, 不含值正文)
- [x] 测试: `observability` 补事件计数断言, `event_delivery` 断言投递后
      `metrics->eventDelivered()` 增长

计划中“TUI 给投递消息加来源标记”**未做**: 投递文本已带 `[Event] <名字>` 前缀 (模型与
用户都能看出来), 加 `ViewMessage` 来源字段会动 wire 结构, 收益不值。

## 2. 测试现状 (2026-11-01)

| 模块 | 结果 | 耗时 |
|---|---|---|
| `event_hub` | passed=204 failed=0 (含总线桥接) | 0.66 s |
| `event_tools` | passed=80 failed=0 | 0.31 s |
| `event_delivery` | passed=44 failed=0 | 17.4 s (每个用例起一次 HTTP 模拟服务 + 端点) |
| `config_keys` | passed=196 failed=0 (生成物已重生成) | 0.8 s |
| `event_subagent` | passed=52 failed=0 | 26.2 s (6 个用例各起一次模拟服务) |
| `observability` | passed=114 failed=0 (含新增事件计数) | 2.7 s |
| `agent_host` | passed=158 failed=0 (回归: spawnBatch 改造) | 14.5 s |
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
12. **不要用“文本替换”把 `++stats_.delivered;` 换成函数调用**: 新写的 `noteDelivered()`
    里也有一句 `++stats_.delivered;`, 脚本会把函数体自己换成 `noteDelivered();` ——
    结果是无限递归, ASan 报 `stack-overflow` 且没有调用栈 (现象: 模块跑到某一用例
    突然崩, 先以为是环境假阳性)。改成函数调用只能手工改, 或替换后立即回读函数体。
13. **总线桥接用例可以写**: 早先删掉的 `test_hub_bus_prefix` 用的就是
    `EventBus::publish` + 前缀订阅 (`std::any` 载荷), 后来以
    `test_hub_bus_bridge` 重写 (`evt.` 与 `plugin.evt.` 两个前缀 + JSON/纯文本载荷 +
    等待被总线值满足), 连续跑 3 次稳定通过 —— 当初的崩溃是环境干扰, 不是这段代码的问题。
    注意 `mode=each` + `replay=true` 会先回放历史值 (两条投递), 只想看新值时用 `replay=false`。

## 4. 未实施事项 (及后续建议)

### 4.1 P3 插件接口表 `agentxx.agent.event`

**没做**。插件“往中枢发值”今天已经可用, 而且有测试覆盖:

- 插件用 `agentxx.agent.runtime` 的 `emit_event("evt.<名字>", payload)` (SDK `emitEvent`);
  插件管理器把主题原样发到该 agent 的总线, 中枢订阅的 `evt.` 前缀就能收到;
- 有 `plugin.evt.` 前缀兜底 (`qualifyEventTopic` 补前缀的场景);
- 用例 `event_hub` 的 `test_hub_bus_bridge` 覆盖: `evt.` / `plugin.evt.` 两个前缀、
  JSON 与纯文本载荷、以及“等待被总线值满足 → 变成会话输入”。

缺的是**插件侧“定义事件/登记等待/查询等待”**（插件想被模型 emit 的事件叫醒时需要）。
做它要按 `docs/zh-cn/design/plugins.md` §17 的接口表规则来:

1. 新表 = 新 IID + `struct_size` + `version`/`_VERSION`/`_MIN_VERSION`/`_BASELINE_SIZE` 四件套;
2. `plugin_iface_meta.h` 加一条 `IfaceMeta`;
3. 主程序侧 vtable 实现 (`plugin_manager_vtable.cpp`) + SDK 便捷函数 (`plugin_kit.h`);
4. `boundaries` 的**成员顺序台账**要同步加一行 (那个测试会逐表校验偏移);
5. 示例插件 + `plugin_*` 测试模块 + 文档一节。

### 4.2 插件 `agentxx_execute_command` 的 `background:true`

**没做**。后台命令需要插件自己持子进程 + 进程收尾时往中枢发结果事件, 与 P3 的
“插件能发值”相关 (现在可以经 `emit_event`, 但插件侧还要自己管进程生命周期、
超时、输出截断、取消), 单独立项比塞在本期更安全。

### 4.3 TUI 给事件输入加来源样式

**没做**。投递文本已带 `[Event] <名字>` 前缀, 模型与用户都能分辨; 加
`ViewMessage` 来源字段要动 wire 结构 (客户端结构体 + 同步负载), 收益不够。

## 5. 提交记录

| 提交 | 内容 |
|---|---|
| `3e2f6c9a` | P1 核心: EventHub/等待/投递 + `agentxx_event` 工具 + `event.*` 配置 |
| `b955eef4` | P1 收尾: 三个测试模块全绿 + `event.*` 进配置键目录 |
| `c2b0a26c` | P2: 异步子代理 (受理即返回, 结果经事件回到父会话) |
| `67dab2a9` | P4: 关键指标与诊断段 |
| `265eda49` | 文档: events.md + index/plugins/AGENTS/test-modules |

