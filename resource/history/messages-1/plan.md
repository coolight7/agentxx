# 方案: 把 LLM 上下文从 neograph `messages` 通道迁出 (会话为唯一权威)

- 难度: B (跨 lib 的节点/中间件/事件桥 + 图定义, 不改插件 ABI, 但改变"插件可见的图状态"内容)
- 类型: 架构调整 (本文仅为设计方案, **未实施**)
- 时间: 2026-09-26 (基于 [memory-1 方案](../memory-1/plan.md) 的代码核对与复测结论)
- 状态: 设计完成, 待确认 (§7 列出必须先回答的 5 个问题), 未实施
- 相关文档:
    - [memory-1/plan.md](../memory-1/plan.md): 每轮上下文拷贝与分配次数归因 (§0.4 两条更正)
    - [benchmark.md §11](../../../docs/zh-cn/design/benchmark.md): 长上下文分配次数实测与分档归因
    - [index.md](../../../docs/zh-cn/design/index.md): EventBridge 事件流与 UI 增量 (§EventBridge 段)
    - [plugins.md](../../../docs/zh-cn/design/plugins.md): 插件接口与图状态/图节点契约
- 依据数据: 主仓 `88fee6e0` / neograph `1522761`, 2026-09-26 复测 (差分口径, 见 memory-1 §0.4)

## 0. 背景与判断

### 0.1 现状: 同一份对话在 4 处各存一份, 靠"轮末覆盖"对账

| # | 存放位置 | 写入方 | 说明 |
|---|---|---|---|
| 1 | 图状态 `messages` 通道 | 各节点 (append / overwrite) | LLM 输入装配、路由、checkpoint 载荷 |
| 2 | 会话 `Session::llmMessages` (`Json`) | `agent_runner.cpp:55` 轮末整体覆盖 | 持久化 + `WireGetContext` + 子代理播种的权威源 |
| 3 | checkpoint `channel_values` | `graph_coordinator.cpp:258 / 290` 每 super-step | `InMemorySingleCheckpointStore` 每线程保留最新一份 |
| 4 | 运行结果 `result.output` + typed 临时对象 | `graph_engine.cpp:1637` / 各处 `get_messages()` | 结束后再转回会话 |

对账逻辑散落在两处, 且是**单向覆盖**:
- `agent_runner.cpp:55`: `session->llmMessages = fromNeographJson(result.channel_raw("messages"))`;
- `event_stream.cpp:465-468` 的注释写明: "input 注入 / 节点内 overwrite (system 注入、压缩) /
  cancel 直写不产生本事件, 不会重复追加; 与引擎最终状态可能存在的短暂漂移由轮末 BaseAgent
  以 `result.channel_raw("messages")` 整体覆盖收敛 (权威同步)"。

也就是说: **会话侧本来就是权威, 图里的通道是第三份镜像**, 今天靠"轮末覆盖"保持一致。

### 0.2 实测代价 (2026-09-26 复测)

- 每条消息正文每轮被拷贝 **7~9 次**: 50 × 8 KB 组 `bin S 37` (10 KiB 档) 858 块/轮 ≈ 8.6 次/条;
  50 × 1 KB 组落到 `bin S 25` (1.2 KiB) 716 块/轮 ≈ 7.2 次/条 (memory-1 §0.4 更正 1);
- 与消息条数成正比的小对象档 32 B / 128 B / 384 B 合计 ≈ 11K 次分配/轮 (typed 层逐条
  `from_json` / `to_json`);
- 只要上下文在 state 里, 下列操作都随上下文线性增长:
  `graph_coordinator.cpp:258 / 290` (每 super-step checkpoint)、
  `graph_engine.cpp:1503-1505` (VALUES 事件, 宿主不消费)、
  `graph_engine.cpp:1637` + `agent_runner.cpp:55` (运行结束双拷贝)、
  `plugin_graph_node.cpp:76` (每次插件图节点执行整份 state serialize 给插件);
- 稳态内存里还多出若干份整份副本 (会话 + 图 state + 每线程 checkpoint + 运行结果)。

> 与 `huge` (≥512 KiB, ≈11 块 × ≈2 MB/轮) 无关: 实测该档与上下文大小无关, 属独立未定位项
> (memory-1 §0.4 更正 2 / §6 问题 3)。

### 0.3 判断

**可行, 且比"就地 append"更彻底**: 把上下文归会话后, 每轮内容拷贝可降到 2~3 次
(provider typed 一次 + 请求体一次), 并且上述所有 state 序列化都变成 O(1) (与上下文无关)。
代价是要替换 `messages` 通道承担的 3 个职责 (输入装配 / 循环路由 / 消息事件与插件可见状态),
并按 §7 的 5 个问题先定策略。

## 1. 目标与非目标

目标:

1. 图状态不再持有 LLM 上下文: `state.serialize()` 的载荷与上下文大小无关 (O(1));
2. 每条消息正文的每轮拷贝次数从 7~9 次降到 **≤3 次** (验收用档位块数 ÷ 消息条数, 见 §6);
3. 会话成为唯一权威: 去掉"轮末 `channel_raw("messages")` 整体覆盖"这类对账逻辑;
4. 中断/异常回滚快照从"整份消息"改为 O(1) (消息条数 + 版本);
5. 行为不变: LLM 请求内容 (逐字节)、SSE 语义、工具循环与中断/恢复语义、消息顺序与持久化结果一致。

非目标:

- 不改 `viewMessages` / `llmMessages` 双消息集 (UI 展示历史与 LLM 上下文是两个独立数据集,
  是该设计的必要部分);
- 不改 SSE 协议、请求体字段、wire 消息格式;
- 不改插件 C ABI / 接口表版本 (但插件可见的**图状态内容**会变, 见 §3.6);
- 不处理 `huge` (未定位, 与本方案无关)。

## 2. 现状: `messages` 通道的职责与全部依赖点 (已逐点核对)

| 职责 | 依赖点 |
|---|---|
| ① LLM 输入装配 | `lib/src/nodes/modelcall.cpp:153` (`build_params` → `state.get_messages()`); system 消息替换 `:647` 读 / `:677` 覆写; utf-8 修复回写 `:633`; 异常回滚快照 `include/agentxx/nodes/wrap_handle.h:262` |
| ② ReAct 循环路由 | 图定义条件边 `lib/src/agent/base_agent.cpp:507` (`has_tool_calls`); 实现 `neograph/src/core/graph_loader.cpp:119` (读最后一条 assistant 的 `tool_calls`) |
| ③ 工具结果回传 | `lib/src/nodes/toolcall.cpp:1112` (`state.write`) / `:1130` (ChannelWrite) |
| ④ 每轮播种 | 根: `base_agent.cpp:983` (`RunConfig.input`) → 引擎 `apply_input` (`neograph/src/core/graph_engine.cpp:1328`); 子代理: `lib/src/agent/agent_host.cpp:603-608` |
| ⑤ 每步 checkpoint / resume | `neograph/src/core/graph_coordinator.cpp:258 / 290`; `graph_engine.cpp:1278 / 1323` (`restore`); resume 值写入 `:1304` (已有 `state.has_channel("messages")` 守卫) |
| ⑥ UI 事件 + 节流持久化源 | `lib/src/event/event_stream.cpp:206-239` (`CHANNEL_WRITE("messages")` → view delta / `appendViewMessage` / toolCallId→view 索引 `:359-366` / `appendSettledLlmMessages` `:468`); 文档 `index.md` 的 EventBridge 段 |
| ⑦ 插件可见契约 | `lib/src/plugins/plugin_graph_node.cpp:76` 每次执行把 `in.state.serialize().dump()` 交给插件; 示例插件读写 `channels.messages` (`plugins/example_graph_node/example_graph_node.cpp:93-95 / 208 / 263`) |
| ⑧ 中间件 | 压缩: `lib/src/middlewares/summarization.cpp:721` (`get_messages`) / `:754` (`countTokens`) / `:988` `:1058` (覆写回写) / `:1074`; 消息修复: `modelcall.cpp:251-277` (`repairMessages`) / `:633`; 日志: `code_agent.cpp:141` (默认关) |
| ⑨ 引擎内置读点 (实际不走) | `neograph/src/core/graph_node.cpp:80 / 178 / 239` (`LLMCallNode` / `ToolDispatchNode` / `IntentClassifierNode` — agentxx 的包装节点自实现 `baseRun`/`build_params`), `plan_execute_graph.cpp:104` (未使用), `graph_engine.cpp:1648` (`final_response`, agentxx 不消费) |

不涉及的点 (已核对, 无命中): 权限 / skill / memory 中间件不读 state 消息。

## 3. 方案

### 3.1 会话为唯一权威 (typed 缓存 + 单一写入 API)

`Session` 保留现有 `llmMessages` (`Json`, 用于 sqlite 持久化与 `WireGetContext`), 新增一份
typed 视图与版本号; 所有消息变更只经会话 API:

```cpp
// agent/lib/include/agentxx/agent/context.h (草案)
class Session {
    // 唯一权威 (线程约束与现有 llmMessages 一致: 仅 io 线程)
    const std::vector<neograph::ChatMessage>& messages() const;
    uint64_t messagesVersion() const;                     // 单调递增, 供快照/失效判断
    void appendMessages(std::vector<neograph::ChatMessage> msgs, bool persistThrottled = true);
    void replaceMessages(std::vector<neograph::ChatMessage> msgs);

    // 边界形态 (不改语义): sqlite llm_context / WireGetContext / wire delta
    utilxx_base::Json llmMessages;
};
```

- provider 直接从会话取 typed 消息 (一次转换), 不再 `state.get_messages()`;
- `llmMessages` 与 typed 缓存由同一个写入口同时更新, 避免再次出现"两份 + 对账"。

### 3.2 循环路由改为命令式 (去掉 `has_tool_calls` 条件)

`NodeResult` 已有 `command` 并可覆盖静态边 (`neograph/include/neograph/graph/types.h:277-292`,
`struct NodeResult` 见 `:529-539`; 引擎按 `command_goto` 优先):

```cpp
// modelcall 节点: 自己知道本轮是否产出 tool_calls, 不再依赖消息内容做路由
out.command = neograph::graph::Command{
    .goto_node = hasToolCalls ? "tools" : "agent_end",
};
```

图定义 (默认图) 相应调整:

```cpp
"channels": {
    // "messages" 不再声明 (见 §3.5); 只保留控制类通道
    { channel_savedGraphData, {{"reducer", "overwrite"}} },
},
"edges": {
    {{"from","__start__"},     {"to","agent_start"}},
    {{"from","agent_start"},   {"to","llm"}},
    {{"from","llm"},           {"to","agent_end"}},   // 静态默认边: 无 Command 时结束
    {{"from","tools"},         {"to","llm"}},
    {{"from","agent_end"},     {"to","__end__"}},
},
```

- 需要显式处理今天靠"补一条无 tool_calls 的 assistant 消息"来兜路由的三条路径
  (`modelcall.cpp:688 / 778` 注释处): 正常、重试耗尽、取消/中断 —— 现在直接在 `command`
  里给出目标节点, 逻辑更直白;
- `has_tool_calls` 条件与 `messages` 通道一起从图定义中移除 (条件注册表本身不动)。

### 3.3 消息事件显式化 (替换 `CHANNEL_WRITE("messages")`)

`EventBridge` 现在从"通道写入事件"里反解消息 (`event_stream.cpp:206-239`)。改为显式入口,
由产出消息的节点直接调用:

```cpp
// EventBridge (草案)
void emitAssistantMessage(const neograph::ChatMessage& msg);      // → view delta + 落库节流
void emitToolResults(std::span<const neograph::ChatMessage> msgs);
void emitMessageTip(utilxx_base::Json value);                     // 现有 message_tip 语义
```

- 内部逻辑 (view 消息展开、Think 段结算、toolCallId → view 索引、`appendSettledLlmMessages`)
  保持不动, 只是触发源从"通道事件"换成显式调用;
- `message_tip` 通道保持原样 (它本来就是控制类通道)。

### 3.4 中断/异常回滚快照改为 O(1)

- 现状: `wrap_handle.h:262` 在异常重抛前把整份 `messages` 存进
  `graphDataKey_tempMessages`; `agent_runner.cpp:44-53` 用它回滚;
- 改为记录 `{messagesVersion, count}` (或 `appendMessages` 前返回的版本), 回滚 = 会话侧
  `truncateTo(version/count)`; 压缩中间件的"原始消息副本"同理只在需要时按版本读取。

### 3.5 checkpoint 只存控制数据, resume 以会话为准

- `messages` 不再声明为通道 ⇒ `channel_values`、VALUES 事件、插件 stateJson 都与上下文无关;
- `resume` 前的处理: 今天 `agent_runner.cpp:30-31` 先用 `update_state` + `overwrite("messages", ...)`
  把会话内容塞回图状态, 然后把 resume 值作为一条 user 消息写入 `graph_engine.cpp:1304`
  (该写入已有 `has_channel("messages")` 守卫)。迁移后:
  - resume 值与 `ctx.resume_value` 一起交给会话 (`appendMessages`), 或由中断处理方直接追加;
  - 上下文完整性由会话保证 (中断期间已按节流窗口 + 轮末权威保存落盘, 见
    `context.h:258-277` / `session_store.cpp:704`);
- `RunConfig.input` 不再携带 `messages` (引擎 `apply_input` 只写已声明通道,
  `graph_executor.cpp:177-186`), 播种改为调用会话 API。

### 3.6 插件侧契约 (必须一并设计)

- 现状: 插件图节点每次执行都收到整份 state JSON (`plugin_graph_node.cpp:76`), 示例插件直接
  读写 `channels.messages` (见 §2 ⑦);
- 迁移后 `stateJson` 只含控制通道 ⇒ 依赖 `messages` 的插件会读到"没有该字段"。
  处理方式 (三选一, 建议 (b)):
    - (a) 在 stateJson 里保留一个**只读影子字段** (如 `messages_meta`: 条数 / 版本 / 最后一条
      的角色与 tool_calls), 老插件不至于崩, 但语义已变;
    - (b) 新增宿主服务 (如 `agentxx.agent.context` 接口表的 `get_messages_json` /
      `messages_count` 按需查询), 并把 stateJson 收敛为控制通道; 通过**能力协商**
      (capability) 告知插件宿主形态;
    - (c) 保持 `messages` 通道存在但只存"引用/版本", 内容由宿主服务提供 (兼容性最好, 但引入
      二次语义);
- 无论哪种方式, `plugins.md` 里凡涉及"图状态 / 图节点可见字段"的段落都要同步更新。

## 4. 收益 (量化对照)

| 项 | 现在 | 迁移后 |
|---|---|---|
| 每条消息正文拷贝次数/轮 | 7~9 次 (实测) | **≤3 次** (provider typed 1 + 请求体 1, 视日志/埋点另加) |
| `state.serialize()` 载荷 | O(上下文) × 每 super-step + 每次插件节点 | **O(1)** (只有控制通道) |
| checkpoint 内存 | 每线程保留 1 份完整上下文 | 控制数据 (几 KB) |
| 运行结束 | `result.output` 整份 + `channel_raw` + `fromNeographJson` | 会话直接作为权威, 无往返 |
| 异常回滚快照 | 整份消息 (低频) | O(1) 版本号 |
| 对账逻辑 | 轮末整体覆盖 + "漂移"注释 | 删除 (单一权威) |
| 子代理播种 | `neograph::json::parse(task.messages->dump())` 文本中转 (`agent_host.cpp:592-595`) | 直接写入子会话 typed 列表 |

## 5. 代价与风险

| 风险 | 说明 | 回退/缓解 |
|---|---|---|
| 插件可见契约变更 | 插件从 stateJson 读 `messages` 会失效 (§3.6) | 能力协商 + 影子字段/宿主服务; 老插件至少不崩 |
| checkpoint 语义变化 | 引擎不能再"离线重放对话" (fork/时间旅行今天未使用) | 保留控制类通道的 replay 语义; 在文档里明确"上下文不在 checkpoint 内" |
| 中断一致性边界 | 恢复程度取决于会话落盘时机 (节流窗口 + 轮末) | 中断点即落盘一次 (沿用 `graphDataKey_savedGraphData` 的写法); 恢复测试覆盖"中断前/中/后" |
| 路由改造遗漏路径 | 重试耗尽 / 取消 / 异常三条路径今天依赖"兜底 assistant 消息" | 路由收敛到 `Command.goto`, 并为三条路径各加一条测试 |
| 子代理"同上下文模式" | `agent_host.cpp:451-465` 子代理直接跑在父 session id 上, "共享前缀 + 各自追加"的语义需重新定义 | 明确写入规则 (只追加 / 版本校验), 或该模式先保持"前缀拷贝 + 独立 session" |
| 迁移面较大 | summarization / repairMessages / EventBridge / 播种 / 测试模块 | 用开关 (`context_owner`) 双轨并存, 先 POC 再翻默认 |
| 隐藏消费者 | 任何按"图里一定有 messages"假设写的代码 (含第三方插件) | 迁移前全库 grep `"messages"` 通道 + 插件接口表审计 |

## 6. 实施顺序与验收

建议分三步 (每步可独立验证、可回退):

| 步骤 | 内容 | 验收信号 |
|---|---|---|
| 0 | **零风险项 (与本方案无关, 建议先做)**: 去掉 `StreamMode::VALUES` (`base_agent.cpp:986`)、运行结束双拷贝合并、`plugin_graph_node` 的 state 序列化按需/裁剪 | M/L 档块数下降 (差分口径), 行为无差异 |
| 1 | 会话侧 typed 缓存 + 单一写入 API (保持图通道仍在, 双写一段时间) | 请求体逐字节 diff 一致; 消息顺序/持久化结果一致 |
| 2 | 路由改 `Command.goto`; 节点写点改为会话 API + 显式事件; 快照 O(1); 图定义去掉 `messages` 通道 (开关 `context_owner=session`) | 每条消息正文拷贝 ≤3 次 (`bin S 37`/对应档位 ÷ 条数); state 序列化载荷与上下文无关 |
| 3 | 插件契约 (§3.6) + 文档更新 + 翻默认 | 插件端到端测试 (含老宿主/老插件降级路径) 全过 |

测试矩阵 (必须覆盖):

- 图/会话: 工具循环 (含工具失败、取消、重试耗尽)、中断 (interrupt_before/after、NodeInterrupt)
  与恢复、压缩触发与回写、多会话并发 (不同 `thread_id` 不串);
- 子代理: 独立模式与同上下文模式 (含 `messages` 结构化透传);
- 插件: `plugin_sdk` / `client_plugins` / 示例图节点插件 (读写 messages 的那个用例要改写并保留回归);
- 持久化: 轮末权威保存 + 节流窗口中断进程后的恢复结果;
- 既有测试模块: `test_message_supplement` (`agent/test/core/test_message_supplement.cpp:660 / 823 / 928`)、
  `test_summarization` (`:328`)、`session_persistence`、`memgrowth`、`interrupt_ui` 需要按新语义调整。

测量方法: 沿用 memory-1 §4 台账 (harness + `MIMALLOC_SHOW_STATS=1`), 但**每轮量用 N 轮与 N+1 轮
差分**, 不要用"累计 ÷ 轮数"; 指标看"消息正文档位块数 ÷ 消息条数"与 state 序列化载荷大小。

## 7. 待确认问题

1. **插件契约走哪条路** (§3.6 a/b/c) —— 决定是否需要插件接口表新增能力与服务;
2. **中断一致性边界**: 中断瞬间的会话状态是否需要"立即落盘一次"以保证恢复完整;
3. **子代理同上下文模式**的写入规则 (共享 session 时父子如何追加/截断);
4. **checkpoint 语义**是否需要在文档/测试里显式声明"上下文不在 checkpoint 内",
   以及 `fork` / 时间旅行是否仍要支持 (今天未使用);
5. **双轨开关的形态**: `AgentConfig::context_owner` (graph|session) 还是编译期开关;
   两种图定义 shape 由同一个工厂产出, 避免分叉。

## 8. 与 [memory-1 方案](../memory-1/plan.md) 的关系

- **P2 (图状态零拷贝访问 + 就地 append) 降级为过渡方案**: 若本方案落地, `messages` 通道不再
  持有上下文, P2 的"回调式读取 / 就地 append"基本失去对象;
- **P1 (请求体链去重) 仍然有效**: 请求体仍是最外层的整份序列化, body move /
  去掉 `fromNeographJson` 与本方案互不冲突, 可先做;
- **P0-1 (去掉 `StreamMode::VALUES`) 建议先做**: 与两条路线都兼容, 且零风险;
- 验收指标统一为"**消息正文档位块数 ÷ 消息条数**"(当前 7~9, 目标 ≤3) 与"state 序列化载荷
  与上下文无关 (O(1))"; `huge` 作为独立未定位项继续跟踪 (memory-1 §6 问题 3)。
