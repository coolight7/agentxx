# Agentxx 整体代码审查报告

- 审查范围：`agent/lib`、`agent/client`、`agent/plugins`、`agent/test`、`docs/zh-cn/design.md(+plugins.md)`

---

## 总体评价

Agentxx 的架构是成熟的：

- ReAct 图引擎 + 栈式中间件 + 双端点 / Transport 对称模型 + 强类型 EventBus + 纯 C ABI 插件系统，方向正确、分层清晰。
- 取消语义（轮询埋点 + asio 信号双通道）、会话隔离（io 线程绑定 + `assertIoThread`）、持久化节流、插件多实例三铁律、连接池生命周期守卫等，都有深思熟虑的设计。
- 注释和文档在同类 C++ 项目里属于非常好的水平，关键决策基本都有注释说明“为什么”。

下面的问题按严重度排序，全部可定位到具体文件。P0 为确认的 Bug，建议优先修；P1 为高风险隐患；P2 为性能 / 体验优化；P3 为结构层重构建议。

---

## P0：确认的 Bug（建议优先修）

### P0-1 `ToolcallWrapNode` 重复调用检测传错了下标 — `repeatCallCheck` 实际大面积失效

位置：

- `agent/lib/src/nodes/toolcall.cpp`（`baseRun`，查找 last assistant 处）
- `agent/lib/src/nodes/toolcall.cpp`（`findConsecutiveRepeatCallKeys`，约 210 行附近）
- `agent/lib/include/agentxx/nodes/toolcall.h`（`findConsecutiveRepeatCallKeys` 声明，约 89 行）

现状：

```cpp
size_t assistantMsgIndex = 0;
for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
    if (it->role == "assistant" && !it->tool_calls.empty()) { assistantMsg = &(*it); break; }
    ++assistantMsgIndex;   // ← 这是“倒数距离”，不是正向下标
}
repeatTriggeredKeys = findConsecutiveRepeatCallKeys(messages, assistantMsgIndex, repeatThreshold);
```

而 `findConsecutiveRepeatCallKeys(messages, assistantMsgIndex, ...)` 内部是 `messages[i]` 正向索引、从 `assistantMsgIndex` 往前回溯。传入倒数距离后，绝大多数情况下查的是错误的窗口（末尾 assistant 在最后一条时传入 `0`，只查 `messages[0]` 一条）。

后果：

- `AgentConfig::toolcallRepeatCheckThreshold`（默认 5 次）形同虚设。
- LLM 陷入同参循环调用时不会弹窗询问用户，循环调用无看守是线上事故位。

修复建议：

- `assistantMsgIndex = messages.size() - 1 - assistantMsgIndex`，或直接在正向循环里找 last assistant。
- 必须补单测：`test/` 里没有任何模块覆盖 repeatCallCheck（全库 grep 无命中），这正是 bug 能存活的原因。

---

### P0-2 取消后消息队列可能永久卡死（非空队列 + 暂停态）

位置：

- `agent/lib/src/agent/io/session_server_agent_io.cpp`（`run()` + `pushMessageQueueItem()`）

现状：

- 一轮 `hasError / interrupted`（含取消）→ `queuePaused_ = true`。
- `pushMessageQueueItem` 只有“空闲 **且队列为空**”时才解暂停并 `wake`；队列非空时只 `sendMessageQueueUpdate`，不唤醒。
- `run()` 循环顶部 `if (queuePaused_ || empty) → 等 wake`。

组合起来：取消时若队列里还有积压消息，之后用户再发新消息，消息只会堆积，循环永远在等 `wake`，必须点 insert（`interruptAndRunNext`）才能恢复。注释里自己都写过“异常中断后 TUI 发送消息卡在队列的根因”，但只修了空队列分支。

修复建议：

- 空闲态的新用户输入一律解暂停（或 TUI 明确提示“已暂停，点击继续”）。
- 补语义测试：backlog + 暂停 + 新输入，断言不死锁。

---

### P0-3 子代理“是否被取消”靠错误文本子串判断，误判率高

位置：

- `agent/lib/src/agent/agent_runner.cpp`（subagent 委派结果处理分支）

现状：

```cpp
if (r.hasError && (find("cancel") || find("Cancel") || find("取消") || find("中断")))
    → 抛 CancelledException
```

中文里“网络中断”“中断向量”这类正常错误文本也会命中，把普通错误扭转为取消（取消会跳过结果写回，行为完全不同）。

修复建议：

- `RespSubagentBatchItem` 加一个结构化的 `bool cancelled` 字段，`spawnOneTask` 的取消分支填它，而不是让上层猜字符串。
- 上层只认结构化字段，文本匹配仅保留作日志关键词，不参与控制流。

---

### P0-4 `Session` 析构里刷持久化队列，线程不对

位置：

- `agent/lib/src/agent/context.cpp`（`Session::~Session() { flushPendingViewOps(); }`）

现状：

- `pendingViewOps_` 约定“仅 io 线程访问”，但 `SessionsManager::remove` / `AgentHost::destroyAgent` 可能在其他线程触发析构 → 对 vector 的并发读写是 UB。

修复建议：

- 析构里只做同线程判断，非 io 线程直接丢弃并记日志（反正进程在拆了），或强制要求 remove 只发往 io 线程。
- 与 `Session` 的 io 线程绑定约定保持一致，不要在析构路径上破例。

---

## P1：高风险的设计隐患（现在没炸，但迟早会）

### P1-1 `AgentServer::controllers_` 自称“单线程无需锁”，前提未被强制

位置：

- `agent/lib/src/agent/io/agent_server.cpp`
- `agent/lib/include/agentxx/agent/io/session_server_agent_io.h`（“所有成员仅 ex_ 线程访问”）

现状：

- `getOrCreateController / stop / serveTransport` 都直接碰 `controllers_` 和 `SessionServerAgentIO`。
- 这个“单线程”成立**当且仅当** `HttpServer` 把 WS handler 调度在 agent 的 `ioCtx` 上。一旦 HTTP 层用自己的线程池（或以后换实现），就是整片 data race。

建议：

- 给 `AgentServer` 加 strand，或在 `serveTransport` 入口 `co_await dispatch(ex_, ...)` 把后续逻辑钉到 agent 线程，并加注释把前提写死。

同理 `EventBus` 全库“无锁靠单线程”，但插件后台线程（offload / JS 引擎）调 `publish` 时是否都 post 回了 io 线程，需要把 `plugin_manager_vtable.cpp` 的 publish 路径再审计一遍——这是插件生态扩大后最容易踩的雷。

---

### P1-2 会话首次打开在 io 线程上做同步 SQLite / 文件 IO

位置：

- `agent/lib/src/agent/context.cpp`（`SessionsManager::getOrCreate` → `SessionStore::loadSession`）

现状：

- `loadSession`（open + SELECT 全同步）在 io 线程执行。每个新会话的第一条消息都会卡住整个 agent 事件循环（目录创建、DB open、WAL 建表）。
- `ListSessions` 已经正确 offload 到 threadPool，`loadSession` 也应该走同一路径（或启动时预加载 + 懒恢复结合）。

---

### P1-3 Transport 静默丢消息

位置：

- `agent/lib/src/agent/io/ws_io_transport.cpp`（`send()`）
- `agent/lib/src/agent/io/channel_io_transport.cpp`（`send()`）

现状：

- 两处都是 `try_send` 失败就吞掉（WS 侧连日志都没有，Channel 侧 cap=4096）。
- 突发流量（全量 Sync + delta burst + 队列更新）下丢的是 `TurnResult / MessageQueueUpdate` 这类**不可恢复**的消息，而 delta 丢了只会回退全量 Sync。
- 设计文档自己都承认这是“已知问题 3”。

建议：

- 至少加计数器 + `XX_LOGW`（采样限频）。
- ContextStats 类高频消息做合并-coalesce，Sync / 结果类消息走阻塞发送或独立高优先级队列。

WS 客户端的 delta 去重（`seq <= cur` 丢弃）只处理了“重复”，没处理“空洞”：中间丢了一个 seq，`lastDeltaSeq_` 照样前移到最大值，空洞永久存在，直到下次重连才用 Sync 修复。建议检测到 `seq > cur + 1` 记一次 warn（采样），给以后做 gap-triggered Sync 留钩子。

---

### P1-4 压缩模块的两个自相矛盾 + 一个死循环风险

位置：

- `agent/lib/src/middlewares/summarization.cpp`
- `agent/lib/include/agentxx/middlewares/summarization.h`
- `agent/lib/src/event/event_stream.cpp`（`EventBridge::countTokens` 回退口径）

现状：

- **token 口径三处不统一**：`SummarizationMiddlewareHandle` 默认 `unicode=1.5`，`EventBridge::countTokens` 回退口径注释写着“完全一致”实际是 `1.1`，而 tps 估算直接 `bytes/4.0`（CJK token 被低估 3~4 倍）。三处应统一到同一个函数。
- **用户可见拼写错误**：`"Summarizizing LLM Context..."` / `"Summarizied LLM Context ..."`，两处都拼错了（应为 Summarizing / Summarized）。
- **压缩后仍 >75% 会每轮都压缩**：成功路径只在 `>=95%` 才硬截断；若一次压缩后稳定在 80%，之后**每个 modelcall 都要派生一个 subagent 做压缩**（failCount 还被清零了）。应该压缩后若仍超阈值直接硬截断，或加“上次压缩后消息增长不足 N 才允许再压”的冷却。

---

### P1-5 `Session::nextDeltaSeq` 被旁路

位置：

- `agent/lib/src/middlewares/summarization.cpp`（约 757 / 909 / 1018 / 1116 行，共四处）
- `agent/lib/include/agentxx/agent/context.h`（`Session::nextDeltaSeq`）

现状：

- 四处直接 `++session->deltaSeq` 而不是调 `nextDeltaSeq()`。
- 今天两者等价，但 `sendToPeer` 的重放缓冲、注释里反复强调的“统一入口”就被架空了，且绕过了 `assertIoThread`。

修复：统一改掉，一行一个，共四行。

---

### P1-6 `RequestResponseStream::request` 自称轮询，实际永远取第一个 server

位置：

- `agent/lib/include/agentxx/event/event_stream.h`

现状：

```cpp
auto serverIt = servers_.begin();
```

现在每 topic 基本只有一个 server 所以没事，但注释写“轮询派发”是误导。要么实现真 round-robin（atomic 下标），要么改注释。

---

## P2：可优化点（性能 / 体验）

### P2-1 Toolcall 串行执行

位置：`agent/lib/src/nodes/toolcall.cpp`（`baseRun`，注释自带 `TODO: 真正并行`）

- 现状是把 awaitable 攒进 vector 再逐个 `co_await`。LLM 一次并行调 5 个 read / grep 时延迟 ×5。
- 建议用 `parallel_group` 或逐个 `co_spawn` + channel 回收，并发度可配（默认 `min(工具数, 8)`），注意中断时未完成的补 `[User canceled]` 语义保留。

---

### P2-2 HTTP 连接池可能根本没被 LLM 流量用到

位置：

- `agent/lib/include/agentxx/util/http_client.h`（`RequestConfig::keepAlive` 默认 `false`）
- `agent/lib/src/util/http_client.cpp`（连接池实现）
- `agent/lib/src/protocol/openai_provider.cpp` / `anthropic_provider.cpp`（调用侧待确认）

- 文档写“LLM 请求启用 keep-alive 连接池”，但 `keepAlive` 默认 `false`。去确认 provider 是否显式开了 keepAlive；如果没开，每次 LLM 调用都付全套 DNS + TCP + TLS 握手，是头号延迟来源。
- 建议 LLM 端点默认开启（池子实现质量是够的：SNI、TLS 版本协商、`HttpPoolContextGuard` 防 UAF 都考虑到了）。

---

### P2-3 连接池满等待是 50ms 忙轮询

位置：`agent/lib/src/util/http_client.cpp`（`HttpConnectionPool::acquire`）

- `acquire` 在并发打满时每个等待者每 50ms 醒一次。突发 100 个 subagent 时是惊群。
- 建议加一个 `release` 时 notify 的等待队列（channel / condition），把 `queuedWaits` 变成真正的排队。

---

### P2-4 跨线程打 `ctx->initNotifier`

位置：`agent/client/src/mode_runners.cpp`（`setupLocalUnifiedDirect`）

- 在 client 线程给 `AgentContext::initNotifier`（裸 `std::function`）赋值，agent 线程读。
- 加个 mutex 或改成 atomic 交换，顺手的事。

---

### P2-5 `repairMessages` 全量拷贝

位置：`agent/lib/src/nodes/modelcall.cpp`（`repairMessages`，合并连续 user 消息分支）

- 每次 LLM 调用前合并连续 user 消息用 `push_back(m)` 拷贝（含 `image_urls` 大 vector）。
- 改成“只在真正发生合并时才搬”，无合并零拷贝。

---

### P2-6 `execute_command` 取消靠 20ms 轮询

位置：`agent/plugins/agentxx_execute_command/execute_command_impl.h`（`procCancelWatchLoop`）

- 每个运行中的命令一个 watcher 协程每 20ms 查一次 `isCancelled()`（后者可能同步 post 到 io 线程）。并行命令多了就是持续的 io 线程抖动。
- 长远看值得做事件驱动的取消注册表；短期把间隔提到 100ms 也行。

---

### P2-7 TUI 已有的 LazyScrollable / LRU 是对的，别动

- 历史分页（尾窗 100 + `GetViewMessages`）和滚动锚定设计也是对的，P0 相关传输可靠性修好后长会话体验会更稳。

---

## P3：重构建议（结构层面）

### P3-1 拆大文件

- `http_client.cpp`（66KB，含 DNS / 连接池 / SSE）
- `session_server_agent_io.cpp`（1142 行：驱动循环 + 队列 + grace + 分页 + 插件转发）
- `toolcall.cpp`（1081 行：参数自愈 + 重复检测 + 执行 + 重试 + 压缩）
- `plugin_manager_vtable.cpp`（61KB）

建议至少把“消息队列管理”“历史分页”“插件事件转发”从 SessionServerAgentIO 里拆成独立单元；toolcall 的 `autoFixArgsType` / repeat-check / 结果压缩各自独立可单测（现在 repeat-check 零测试就是拆得不够的后果）。

---

### P3-2 补最缺的两块测试

现有测试广度是够的（sync / async / TUI / 插件 / 协议全有，`test.cpp` 调度也干净），缺的是语义角落：

1. `findConsecutiveRepeatCallKeys` 单测（正向下标、阈值边界、多 key 并行）。
2. 取消后队列语义（backlog + 暂停 + 新输入，断言不死锁）。

---

### P3-3 `fail-fast` 用 `_Exit`

位置：`agent/test/test.cpp`

- 失败直接 `_Exit(1)`，跳过析构和 stdout 刷新，末尾输出可能丢。改成抛异常 / return 非零让 main 正常返回。

---

### P3-4 小而散的

- `yamlToJson` 整数先按 `int` 解析（大整数精度丢到 double，改 `long long`）。
- `extractTokenFromUrl` 不做 URL 解码。
- `fileEdit` 的 `.agentxx_edit_tmp_*` 残留文件无启动清理。
- `generateUniqueSessionId` 没问题，保持。

---

### P3-5 planning 插件 client 侧渲染重复

位置：`agent/plugins/agentxx_planning/agentxx_planning.cpp`

- `buildDecorItems` 与 `refreshPlanSection` 的 todo / note 渲染重复：抽一个公共 `plan→items` 函数，两处共用，避免以后改一样忘一样。

---

## 附录：审查方法

- 通读 `docs/zh-cn/design.md` 与 `plugins.md` 建立架构基线（ReAct 循环、双消息集、Wire 协议、插件 C ABI、TUI 懒渲染）。
- 精读 `agent/lib` 核心：`base_agent.cpp`、`context.cpp`、`event_stream.cpp`、`toolcall.cpp`、`modelcall.cpp`、`session_server_agent_io.cpp`、`ws_io_transport.cpp`、`agent_io.cpp`、`agent_server.cpp`、`agent_runner.cpp`、`agent_host.cpp`、`session_store.cpp`、`summarization.cpp`、`permission.cpp`、`middleware.cpp/h`、`wrap_handle.h`、`config.h`、`conversation_types.h`、`wire_protocol.h`、`http_client.h/cpp`、`async_offload.h`、`exception.h`、`log.h`、`code_agent.cpp`。
- 通读 `agent/client`（`main.cpp`、`mode_runners.cpp`、`config_loader.cpp`）、`agent/plugins`（`execute_command_impl.h`、`filesystem_impl.h`、`agentxx_planning.cpp` 等）与 `agent/test/test.cpp` 入口及模块划分。
