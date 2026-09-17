# Agentxx 源码通读与审查报告

> 审查对象: 当前工作目录 `/home/coolight/program/agentxx` 的 C++ 源码
> (重点: `agent/lib` 核心库、`agent/client` CLI/TUI、`agent/plugins` 插件、`agent/test` 测试)
> 审查方式: 分模块通读源码 + 交叉验证设计文档 (`docs/zh-cn/design/*.md`)，对可疑点回看调用方与测试
> 本报告按模块分节，每节包含"该模块做什么/怎么做"与"发现的问题/优化点"。
> 问题编号规则: `M<模块号>-<序号>`，严重程度分 `高 / 中 / 低`。
>
> **结论来源说明**: 全部结论来自静态阅读 (源码 + 设计文档 + 现有测试) 的逻辑推导，
> 其中 M11-1 依据仓库内已有的崩溃/ASan 产物 (`crash-456116.log`、`agentxx_asan.456116`)。
> 本次未编译运行 (仓库已有 debug/release 构建产物，但改动验证需重编)，因此各条的
> "影响"均为按代码路径推导的结果；修复时请按每条给出的"验证方式/建议用例"复现确认。

---

## 目录

- [0. 总体架构与审查结论摘要](#0-总体架构与审查结论摘要)
- [1. util 工具层](#1-util-工具层)
- [2. 事件系统 (EventBus / EventStream / EventBridge)](#2-事件系统-eventbus--eventstream--eventbridge)
- [3. Agent 核心 (BaseAgent / AgentContext / Session / AgentRunner / AgentHost)](#3-agent-核心)
- [4. Graph 节点 (wrap_handle / modelcall / toolcall / agentcall)](#4-graph-节点)
- [5. 中间件 (permission / summarization / skill / memory / subagent)](#5-中间件)
- [6. 会话 IO 与传输 (AgentIOBase / SessionServerAgentIO / transport / wire 协议)](#6-会话-io-与传输)
- [7. LLM Provider 与其它协议 (openai / anthropic / mcp / a2a / acp)](#7-llm-provider-与其它协议)
- [8. 会话持久化 (SessionStore / SettingsDb)](#8-会话持久化)
- [9. 插件系统 (C ABI v1 / PluginManager / client 插件)](#9-插件系统)
- [10. client (main / config_loader / stdio / TUI)](#10-client)
- [11. plugins 目录 (内置插件)](#11-plugins-目录)
- [12. test / benchmark / 构建系统](#12-test--benchmark--构建系统)
- [13. 缺陷汇总与修复优先级建议](#13-缺陷汇总与修复优先级建议)
- [14. FFI / 训练 / lib 内置工具 / client 插件](#14-ffi--训练--lib-内置工具--client-插件)

---

## 0. 总体架构与审查结论摘要

### 0.1 架构分层 (读代码后的实际形态)

```
                         ┌──────────────── client (agentxx_cli) ────────────────┐
                         │  main.cpp → config_loader → mode_runners            │
                         │    ├── StdIOClientAgentIO   (stdin/stdout 行式)      │
                         │    └── TUIClientAgentIO     (FTXUI 全屏)             │
                         │  client 侧插件: ClientPluginManager (UI 注册表/命令) │
                         └───────────────┬──────────────────────────────────────┘
                                         │ AgentIOBase (端点基类, sendToPeer/onPeerMessage)
                                         │ AgentIOTransportBase
                                         │   ChannelAgentIOTransport (同进程, 零序列化)
                                         │   WsAgentIOTransport      (跨进程 JSON over WS)
                         ┌───────────────▼──────────────────────────────────────┐
                         │ SessionServerAgentIO (服务端端点, 1:N 客户端)         │
                         │   delta 重放缓冲 / grace 宽限 / 消息队列 / HIL 转发   │
                         └───────────────┬──────────────────────────────────────┘
                                         │ BaseAgent::runTurnAsync(sessionId, input, io)
                         ┌───────────────▼──────────────────────────────────────┐
                         │ BaseAgent / CodeAgent                                │
                         │  ├─ AgentContext: config/modelRegistry/bus/sessions  │
                         │  │     pluginManager/toolRegistry/graphRegistry      │
                         │  │     middlewareHandleContext/threadPool/host       │
                         │  ├─ GraphEngine (neograph) 图: start→llm→tools→end   │
                         │  │     ModelCallWrapNode / ToolcallWrapNode ...      │
                         │  ├─ 中间件栈 (栈式 onHandleStart/End 包裹节点执行)    │
                         │  ├─ AgentRunner: run→interrupt→HIL→resume 循环       │
                         │  └─ EventBridge: GraphEvent → WireDelta + EventBus   │
                         └───────────────┬──────────────────────────────────────┘
                                         │ AgentHost (进程级宿主, 主子代理平等注册)
                                         │   spawnBatch / HostBus / 深度并发预算 / 回收
```

关键机制（读码确认，与文档一致）：

1. **单 io_context 多协程无锁会话**：`BaseAgent` 持有独立 `io_context`；`Session` 用
   `bindIoThread()/assertIoThread()` 强制 `viewMessages/llmMessages/chainHash/cancelToken` 只在 io 线程读写
   (`agent/lib/include/agentxx/agent/context.h:150-190`)。
2. **消息双集**：`viewMessages`(append-only 展示历史 + 链式哈希) 与 `llmMessages`(可压缩的 LLM 上下文)分离；
   增量经 `WireDelta`(会话级单调 `seq`) 推送，服务端保留重放缓冲支持断线增量重放。
3. **栈式中间件**：`WrapHandleBaseNode::run` 实现 `start1..startN → baseRun → endN..end1`，
   用 `startedIdxs` 记录实际执行过的下标，运行中禁用插件不破坏 start/end 配对 (`nodes/wrap_handle.h:160-260`)。
4. **取消双通道**：轮询埋点 (`cancel_token->throw_if_cancelled`) + asio cancellation_signal 打断在途 IO；
   `classifyCurrentException` 统一把 `operation_aborted + token 已取消` 归类为控制流取消 (`util/exception.h:47-105`)。
5. **插件 ABI**：纯 C ABI v1 + 生命周期契约 (create/start/stop/destroy) + 多实例三铁律；
   宿主侧 `ioCallSync` 把插件线程的宿主调用投递回 io 线程串行化。
6. **持久化节流**：`Session` 对 view/llm 落库做 3s 窗口节流，轮末强制 flush (`context.h:250-270`, `context.cpp:170-215`)。

### 0.2 结论摘要 (详细见各模块与第 13 节)

| 编号 | 严重 | 模块 | 一句话描述 |
|------|------|------|-----------|
| M3-1 | 高 | AgentRunner | `unresolvedInterrupt` 恒为 false（死条件），子代理未处理中断被当作成功返回 |
| M4-1 | 高 | toolcall | 重复调用确认的取值口径错误，用户点"允许"也永远被当成拒绝 |
| M3-2 | 高 | BaseAgent | 图构建回退分支复用了已被 move 的 `nodeContext`，回退后 agent 无 tools/provider |
| M11-1 | 中高 | 插件/glob | 现场 ASAN/崩溃报告指向 `agentxx_filesystem_grep` 的递归 glob 路径堆破坏，需按报告给的步骤复现定位 |
| M1-1 | 中 | util/json | `Json::dump` 浮点用 `%.17g` 且未裁尾零，`0.7` 输出成 `0.69999999999999996` |
| M1-2 | 中 | util/util | `detectPowerShell/systemName/isRunningInWSL` 的 static 缓存无锁，跨线程 UB |
| M6-1 | 中 | SessionServerAgentIO | `startGraceTimer` 覆盖旧定时器不取消，宽限期可能被前一个定时器提前结束 |
| M2-1 | 中 | EventBus | 同 topic 类型不一致仅 `assert` 防护，Release 下是 UB `static_cast` |

> 说明: 下表未列出的模块（协议 provider、TUI、测试等）见各自小节。

---

## 1. util 工具层

范围: `agent/lib/include/agentxx/util/*` + `agent/lib/src/util/*`
(string_util / json / json_view / log / regex / http_client / http_server / ws_client /
sqlite / settings_db / async_offload / exception / lru_cache / container_util / path_sanitize / util)

### 1.1 模块职责与实现要点

- **string_util.h** (1351 行): 全部 `constexpr` 友好的字符/UTF-8 工具。UTF-8 处理按"前导字节跳步"实现
  (`utf8GetLength` / `findIndexByUtf8Length` / `utf8GetLengthCheckAvail` / `utf8Repair` / `estimateTokenCount`)；
  路径工具 (`toCurrentSystemAbsolutePath` 系列，含 `~` 展开/Windows 盘符↔`/mnt/` 转换/词法规范化)；
  `IgnoreCaseMap/Set` (透明哈希) 供 HTTP 头、枚举匹配等使用。
- **json.h/json.cpp** (1327 行): 自研保序紧凑 DOM，`variant` 风格 union + 显式 `Type`，
  simdjson 解析、手写 dump (支持 indent)、`items()` 迭代、`oobNull()` 越界哨兵。
- **json_view**: simdjson 之上的零拷贝只读视图，用于 `wire_protocol::deserialize` 与 provider 的 SSE 高频路径
  (先 View 路由 type，命中后再物化 JSON)。
- **regex.h/regex.cpp**: hyperscan (默认开启) 后端 + `std::regex` 回退，统一 `XXRegex::{match,replace,remove}`。
- **http_client/http_server/ws_client**: Boost.Beast 实现；http_client 带 keep-alive 连接池
  (按 io_context 分桶，`HttpPoolContextGuard` 防悬挂 reactor)、并发上限、失效重试。
- **async_offload.h**: `offloadAsync` / `offloadCancellableAsync`(带取消 watcher) / `asyncWithTimeout`，
  把阻塞操作丢到 `asio::thread_pool`。
- **exception.h**: `classifyCurrentException` / `catchError` / `catchErrorAsync` /
  `catchErrorToUnexpectedAsync` / `catchErrorToOptionalAsync`，把取消/中断与普通异常区分开。
- **util.cpp**: 系统探测 (Linux `/etc/os-release`、WSL、PowerShell 版本探测)、io_uring 可用性探测、
  设备 id、md5、`formatSize` 等。

### 1.2 问题与优化

#### M1-1 (中) `Json::dump` 浮点输出不是最短表示，且注释与实现不符
- 位置: `agent/lib/src/util/json.cpp:139-156` (`appendDouble`)
- 现状: `%.17g` 输出后只判断"是否含 `.`/`e`"，不足时补 `.0`；注释写"再裁掉多余尾零"但**没有实现裁剪**。
- 影响: 任何 double 走 JSON 都会变长且"不自然"，例如 `0.7` → `0.69999999999999996`；
  这些数值会进入发给 LLM 的请求体 (`temperature`/`top_p` 等) 与 SQLite 设置表，
  严格校验的网关/前端展示可能出现意外；同时请求体体积白白增大。
- 建议: 改用最短往返表示 `std::to_chars(buf, buf+n, v)` (GCC 11+/libc++ 均有)，
  仍保留"整数值补 `.0`"的行为以兼容既有测试快照；顺带补一个 `0.1/0.7/1e10/1e-7` 的单测。

#### M1-2 (中) 系统探测 static 缓存无任何同步
- 位置: `agent/lib/src/util/util.cpp:61-63` (`systemName_`/`isRunningInWSL_`/`psInfo_`)、
  `util.cpp:287`、`agent/lib/src/util/util.cpp:318` (Windows 分支 `psInfo_`)、
  `detectPowerShell()` (util.cpp:262 / 336)
- 现状: `std::optional<std::string>` / `std::optional<bool>` / `std::optional<PowerShellInfo>`
  都是"读-判断-写"三段式，没有 `once_flag` 或互斥。
- 影响: `detectPowerShell()` 由 `agentxx_execute_command` 插件在**插件线程池**调用
  (插件工具默认 offload 执行)，而同进程可能有多个 agent 实例/多个工具调用并发进入；
  写 `std::string` 成员与并发读同一对象是数据竞争 (UB，极端情况下读到半写入的 string 会崩)。
- 建议: 统一改为 `std::once_flag`/`call_once` 或加 `std::mutex`；
  `PowerShellInfo` 内部字符串可用 `char[16]` 或返回 `const&` 到进程级常量。

#### M1-4 (低) 设备 id / md5 的静默失败
- 位置: `agent/lib/src/util/util.cpp:556-585` (`md5Hex`)、`getDeviceId`
- 现状: `EVP_DigestInit_ex` 失败时 `digestLen` 保持 0，函数返回空串，`getDeviceId()` 也返回空串。
- 影响: `WireHelloAck.deviceId` 为空，客户端无法区分设备；没有日志，排查困难。
- 建议: 失败时记 `XX_LOGE` 并回退 `readPlatformRawDeviceId()` 原文的哈希 (或返回常量)。

#### M1-5 (低) `getFileName` / `getParentDirPath` 用 `int` 下标 + 多分支推导
- 位置: `string_util.h:919-1010`
- 现状: 用 `int` 倒序扫描 `size()`，靠 `isContinueDot`/`leftDotIndex` 等状态位推导隐藏文件/多后缀；
  边界 (尾部全斜杠、`/`、`.`、`..`) 靠分支顺序保证，可维护性差且难以自证正确。
- 建议: 基于 `std::string_view` 的"最后一段 + 从右往左第一个 `.`(跳过前导点)"重写，并补齐
  `"a/"`、`"/"`、`"..."`、`".gitignore"`、`"a.tar.gz"` 等用例 (现测试集中在 `test_string_util.cpp`)。

#### M1-6 (低) `parseNumberFromString` 在 libc++ 下把 `strtod` 的 `errno` 当失败
- 位置: `string_util.h:733-770`
- 现状: 回退实现要求 `errno == 0` 才算成功；`strtod` 对**下溢** (`1e-320`) 会置 `ERANGE` 并返回
  非 0 值，此时被判定为解析失败。
- 影响: 仅在 llvm-mingw/libc++ 目标上出现，表现为极小浮点参数被当作非法。
- 建议: 与 `toolcall.cpp` 的 `parseFullNumber` 一致，用 `end == c_str()+size` 判定消费完整，
  仅对 `ERANGE + HUGE_VAL` 视为失败。

#### M1-7 (优化) 热点路径上的多余拷贝
- `Json::dump` 对字符串节点做了一次 `std::string s = j.get<std::string>();` 拷贝
  (`json.cpp:170-175`)，大数据 dump (tool 结果、wire 消息) 可改为引用/`string_view`。
- `ConstItemsIterable::iterator::operator*` 返回 `std::pair<std::string, Json>` (键与值双拷贝)，
  与可变版语义不一致，遍历大对象时开销明显 (`json.cpp:1200-1210`)。
- `string_util.h` 的 `isIgnoreCaseContains/isIgnoreCaseEqual` 每次调用都分配两份 `toLower` 结果；
  在 HTTP 头/枚举匹配等热路径可改为逐字符比较 (已有 `IgnoreCaseEqual::equal` 可直接用)。

### 1.3 正向确认 (值得保留的设计)

- `Session`/`EventBus` 等明确"单线程约定 + assert 校验"，比"看起来线程安全但实际不是"要好。
- `JsonView` 的"先零拷贝路由、命中再物化"在 SSE 高频路径上是有效优化 (`protocol/openai_provider.cpp` 使用)。
- `offloadCancellableAsync` 的取消 watcher 协程解决了"线程池里同步代码无法被 asio 信号抢占"的真实问题。

---

## 2. 事件系统 (EventBus / EventStream / EventBridge)

范围: `agent/lib/include/agentxx/event/event_stream.h` (674 行)、`src/event/event_stream.cpp` (711 行)、
`include/agentxx/event/events.h`、`event_host.h`

### 2.1 模块职责与实现要点

- `EventStream<T>`: 单向强类型流，`subscribe(handler, execHit)` (execHit=0 常驻，>0 触发 N 次自动移除)，
  `publish` 先对订阅者做**快照**再逐个 `co_await`，单个订阅者异常被 `catchErrorAsync` 吞掉并记日志。
- `RequestResponseStream<Req,Resp>`: 多 server 轮询 (`rrIndex_++ % servers_.size()`)，`request()` 支持超时
  (0 = 不限)，异常经 `catchErrorToUnexpectedAsync` 转成 `unexpected` (控制流异常仍会重抛)。
- `EventBus`: `map<string, shared_ptr<EventStreamInterface>>` 类型擦除注册表 + 前缀订阅 + 同步服务
  (`registerService/callService`，用于 token 计数等无 IO 的同步服务)。
- `EventBridge`: GraphEvent → ① 原始回调 ② WireDelta(含 think 段计时/tps 统计) ③ EventBus 发布，
  并负责把 `messages` channel 写入**展开成 ViewMessage** (`Think/Assistant/Tool`)、回填 tool 结果、
  增量持久化 `llmMessages`。
- `TimerEventStream::once/onceWeak`: detached 定时器协程，支持 weak 捕获防悬垂。

### 2.2 问题与优化

#### M2-1 (中) 同 topic 类型不一致时 Release 下是 UB
- 位置: `event_stream.h:325-345` (`get<T>`)、`event_stream.h:352-370` (`getRR`)
- 现状: 已有 topic 的类型与请求类型不符时只 `assert(...)`；`NDEBUG` 构建下 `assert` 被移除，
  紧接着 `static_cast<EventStream<T>&>(*it->second)` 是类型双关 UB。
- 影响: 任何"同一 topic 名被两处用不同类型注册/发布"的笔误会静默破坏内存
  (插件、宿主约定事件都动态拼 topic 名，容易踩到)。
- 建议: Release 下也做 `elementType_` 校验：不匹配时记 `XX_LOGE` 并返回一个进程级空流
  (或抛异常)，保证不 UB。

#### M2-2 (低) `hasListeners` 与 `publish` 的类型判定不一致的隐患
- 位置: `event_stream.h:392-402` vs `get<T>`
- 现状: `hasListeners<T>` 类型不符返回 false (安全)，而 `publish` 直接走 `get<T>` (assert/UB)，
  于是"`hasListeners` 为 false"并不能保证"publish 安全"。修 M2-1 后此条自然消解。

#### M2-3 (低) `EventStream::publish` 的 execHit 递减发生在调用前
- 位置: `event_stream.h:130-160`
- 现状: 先用快照把 execHit 递减/移除，再逐个 `co_await`；若中途抛出 (理论上被 `catchErrorAsync` 吞掉，
  但外层协程取消仍可能提前退出)，剩余订阅者不会再被调用，而它们的 execHit 已经被扣掉。
- 影响: 一次性订阅在取消/异常路径上可能"少执行一次就消失"。对现有用法 (adapter 注册的一次性订阅) 影响很小。
- 建议: 先派发、派发成功后再递减 (或用 `execHit` 副本判定)。

#### M2-4 (优化) `EventBus::publish` 每次都构造 `std::any` 做前缀分发
- 位置: `event_stream.h:378-390`
- 现状: 只要存在前缀监听 (>0) 就对每个事件构造 `std::any(data)` (含字符串拷贝，可能堆分配)。
  前缀监听几乎总是存在 (`SessionServerAgentIO::subscribePluginEvents` 订阅了 `"plugin."`)，
  因此**所有**总线事件都会付一次 any 构造；`agent.model.token` 这类高频事件因
  `EventBridge::publishModelToken` 已用 `hasListeners` 短路，默认配置下不触发 (有插件订阅
  ModelToken 时才会命中)，实际影响面取决于各 topic 的流量。
- 建议: 先判断"是否存在匹配该 topic 的前缀"再构造 `std::any` (把前缀按首段索引，
  避免每次全表 `compare` 扫描)。

#### M2-5 (优化) `RequestResponseStream::request` 的 server 选择器
- 位置: `event_stream.h:236-275`
- 现状: `rrIndex_.fetch_add(1) % servers_.size()` 每次取 `servers_.begin()` 后 `std::advance`，
  是 O(servers) 且不支持"声明式优先级"；`registerServer` 返回的 id 也没有暴露"指定 server"能力
  (权限/中断/子代理三类请求都只有一个 server，暂无实际影响)。
- 建议: 用 `std::vector<std::pair<id, handler>>` 或按 id 取模直接 `find`，顺便让错误信息带上 server 数量。

#### M2-6 (优化) `EventBridge::handleChannelWrite` 的 tool 结果回填是 O(n) 二次扫描
- 位置: `src/event/event_stream.cpp:330-420`
- 现状: 已有 `toolCallHistoryIndex_` 做 O(1) 定位，但命中后 `updateViewMessage(*target)` 内部
  仍按 `msg.id` **线性重扫** `viewMessages` (`context.cpp:120-145`)。
- 影响: 长会话 (数千条历史) + 大量 tool 调用时，每条 tool 结果都要多扫一遍历史。
- 建议: `Session` 内维护 `msgId → index` 的索引，或让 `updateViewMessage` 接受下标重载。

---

## 3. Agent 核心

范围: `agent/lib/include/agentxx/agent/{base_agent,code_agent,context,agent_runner,agent_host,model_registry,session_store,config,prompt,training}.h`
与对应 `src/agent/*.cpp`

### 3.1 职责与实现要点

- `BaseAgent`: io_context + AgentContext + GraphEngine 所有权；`init()` 顺序为
  modelRegistry → EventBus → MiddlewareContext → pluginManager/graphRegistry → initMiddleware →
  initTools → 白名单过滤 → summarization handles → 工具 schema 校验 → 图定义 → 加载插件 →
  编译/校验/构建图 → `engine->own_tools`。
- `runTurnAsync`: 绑定 io 线程 → 取/建 Session → 注册 io 到会话总线 → 构造 EventBridge →
  resume 判断(graphData) → 追加 user 消息 → TurnStart Delta → `AgentRunner.run` (含中断循环) →
  异常/取消路径回滚 `llmMessages` → 清理 checkpoint 的 savedGraphData → 轮次统计提示 →
  `saveLlmMessages` + `flushViewMessages` → TurnEnd Delta。
- `AgentContext`: 全局共享配置/总线/modelRegistry/sessions/pluginManager/toolRegistry/graphRegistry/
  middlewareHandleContext/threadPool/host，以及会话级工作目录覆写表 (mutex 保护)。
- `Session`: 见 3.1；持久化节流 (3s) + 轮末 flush。
- `AgentRunner`: 统一"首跑 → 中断检测 → 记录中断信息到 checkpoint → 按 handle 名分派
  (subagent 走 `service.subagent`，其它走 `service.interrupt` HIL) → 写回 resumeValues → resume"，
  根 agent 与子代理共用 (差异收敛为 Hooks)。
- `AgentHost`: 主子代理平等注册 (AgentNode)，`spawnBatch` 强制深度/并发预算，
  HostBus (`agent.spawn/message/progress/done`)，子代理结束回收 (SpawnCleanup guard)。

### 3.2 问题与优化

#### M3-1 (高) `AgentRunner::Outcome::unresolvedInterrupt` 永远为 false —— 子代理未处理中断被当成成功
- 位置: `agent/lib/src/agent/agent_runner.cpp:278`；消费点 `agent/lib/src/agent/agent_host.cpp:675`
- 现状:
  ```cpp
  while (result.has_value() && result->interrupted) { ... }         // 退出条件保证 interrupted == false
  outcome.unresolvedInterrupt = result.has_value() && result->interrupted;  // 恒为 false
  ```
  而"无处理者/未响应"分支恰好把 `result` 置为 `nullopt` 后退出循环 (同文件 255-275 行注释明确写
  "按中断未完成处理")。
- 影响:
  1. `agent_host.cpp:675` 的 "interrupt not handled in subagent scope" 分支是**死代码**；
  2. 子代理在 HIL 无处理者/超时/未响应时，会走 `else` 分支返回 `content = oss.str()` (可能只是部分输出)
     且 `hasError = false` —— 父 agent 会把"没跑完的任务"当成成功结果继续推理。
- 建议: 在 `resumeValues.empty()` 的退出路径上显式置位，例如用一个局部 `bool interruptedUnresolved`
  在 `if (false == resumeValues.empty())` 的 else 分支置 true，循环结束后
  `outcome.unresolvedInterrupt = interruptedUnresolved;`；并补一个"子代理 HIL 无处理者"的用例。

#### M3-2 (高) 图构建回退分支复用已 move 的 `nodeContext`
- 位置: `agent/lib/src/agent/base_agent.cpp:274-332`
- 现状:
  ```cpp
  neograph::graph::NodeContext nodeContext{};
  ... nodeContext.tools = std::move(toolPtrs); ...
  engineConfig.node_context = std::move(nodeContext);        // 293: nodeContext 被 move
  try { engine = GraphEngine::link(std::move(validated), std::move(engineConfig), ...); }
  catch (const std::exception& e) {
      ... engineConfig2.node_context = nodeContext;          // 324: 用的是 moved-from 对象
  }
  ```
- 影响: 当插件通过 `set_graph_json` 改坏的图导致 `link` 抛异常时，回退构建出的默认图
  **没有 instructions / 没有 provider / 没有 tools** (`std::shared_ptr` 与 `vector` 均已被 move 空)，
  agent 能启动但首次 modelcall 就失败，且现象难以定位 (日志只提示回退成功)。
- 建议: 保留一份 `NodeContext` 拷贝用于回退分支 (或在 `catch` 内重新组装 provider/tools/instructions)，
  并补一个"插件写入非法图 → 回退后仍能正常跑一轮"的测试。

#### M3-4 (中) `SessionServerAgentIO::session()` / `BaseAgent::selectModel` 在 io 线程做同步 SQLite 加载
- 位置: `agent/lib/src/agent/io/session_server_agent_io.cpp:1300-1330` (`session()`)、
  `agent/lib/src/agent/base_agent.cpp:1000-1010` (`selectModel` → `agentContext->getSession`)
- 现状: `SessionsManager::getOrCreate` 未命中时会**同步** `sessionStore->loadSession()` (SQLite 读 + 反序列化)；
  而 `getOrCreateAsync` 才是卸载到线程池的版本。`handleHello`/`handleGetViewMessages`/`WireSelectModel`
  等入口都走同步版。
- 影响: 首轮连接 (或断线重连) 时若会话历史很大，io 线程被阻塞，所有会话的 LLM 流/工具执行全部停摆。
- 建议: 这些入口改为 `getSessionAsync` (先预热再处理)，或让 `SessionServerAgentIO` 在 `run()` 启动时
  预取一次会话；至少在 `XX_LOGD` 里记录加载耗时便于发现。

#### M3-5 (中) `runTurnAsync` 在 io 线程同步读附件文件并 base64
- 位置: `agent/lib/src/agent/base_agent.cpp:560-620`
- 现状: 服务端自主加载附件 (`pathOrUrl` 是服务端本地路径) 时，用 `std::ifstream` + `istreambuf_iterator`
  读整个文件，再做 `base64Encode`；上限为 `maxBytesForMediaType` (MB 级)。
- 影响: io 线程被数 MB 的文件读 + base64 (CPU) 阻塞，多会话场景下表现为"卡住几百毫秒"。
- 建议: 在 util 增加 readFileAsync 替换，优先使用 asio 异步文件读取，否则走 `util::offloadAsync(threadPool, ...)`；base64 与 MIME 推断也可一并搬到线程池。

#### M3-9 (优化) `AgentHost` 与 `BaseAgent` 的生命周期仍有隐式约定
- 现象: `BaseAgent::~BaseAgent()` 仅 `engine = nullptr`；`AgentContext::~AgentContext` 依赖
  `PluginManager::shutdownAll()`，并在仍有 close pending 时告警 "owner should await shutdownAsync()"。
- 建议: 在文档中给出标准的关闭顺序 (shutdownAsync → ioCtx.stop → 释放)。

---

## 4. Graph 节点

范围: `nodes/wrap_handle.h`(中间件栈基类)、`nodes/agentcall.cpp`、`nodes/modelcall.cpp`、
`nodes/toolcall.cpp`、`nodes/wrap_handle.h` 与 `nodes/*.h`

### 4.1 职责与实现要点

- `WrapHandleBaseNode<T>::run`: 取消埋点 → `onNodeStart` → 依次 `onHandleStart(handles[i])`
  (记录 `startedIdxs`) → 若 start 全部成功则 `baseRun` → 逆序 `onHandleEnd` → 按
  `interceptOrdinaryError_` 决定"普通异常吞掉(节点内已消息化)"还是重抛；重抛前把当前
  `messages` 存到 `graphDataKey_tempMessages` 供上层回滚/恢复。
- `ModelCallWrapNode`: `build_params`(注入唯一 system 消息 + 工具 schema + 会话级模型解析) →
  `repairMessages`(悬挂 tool_calls 清理 / tool_call_id 去重 / 连续 user 合并 / 空消息补齐 /
  UTF-8 修复) → `callLLM` → 失败重试(带限速退避/部分输出保留/`llmMaxRetry` 上限)。
- `ToolcallWrapNode`: 重复调用检测(`findConsecutiveRepeatCallKeys`) → 每个 tool 参数
  `autoFixArgsType`(按 JSON Schema 修类型/枚举大小写) → 权限总线询问 → 重复调用 HIL 询问 →
  执行(带 `maxRetry`) → 超长输出卸载到 share store 并截断 → 结果写回 `messages` channel；
  取消时补齐 `[User canceled]`，中断时把已完成结果缓存到 `graphDataKey_interruptToolcallCache`。
- `AgentStartCallWrapNode`: 每轮清理 `graphData[thread_id]` 并发布 `plugin.agentxx.round_start`。

### 4.2 问题与优化

#### M4-1 (高) 重复调用确认取值口径错误 —— 用户点"允许"也一律被拒绝
- 位置: `agent/lib/src/nodes/toolcall.cpp:656-696` (重复调用 HIL)，对照
  `agent/lib/src/agent/agent_runner.cpp:238-250`、`agent/lib/src/agent/io/agent_io.cpp:190-200`、
  `agent/lib/src/tools/subagent.cpp:405-425`
- 现状: 三处契约拼接后语义不一致：
  1. 端点把客户端结果转成 **values 对象** 写回 `RespInterrupt.resultJson`
     (`resultJson = result["values"].dump()` → `{"allow":"true"}`)；
  2. `AgentRunner` 把它按 `resultId`(= 工具调用 id) 包一层写进 `resumeValues`
     → `graphDataKey_interruptResult = {"call_xxx": {"allow":"true"}}`；
  3. `MiddlewareContext::requestInterrupt` 原样返回**整个映射**；
  4. `subagent.cpp` 正确地按 `makeSubagentResumeKey(...)` 下钻取值，
     而 `toolcall.cpp:682` 直接 `interruptValueBool(result, "allow", false)` ——
     `interruptValueBool` 会向下找 `values` 键(不存在)，再在顶层找 `"allow"`(不存在)，
     于是**永远**返回默认值 `false`。
- 影响: `repeatCallCheck` 功能实际是"一律拒绝"：用户在弹窗点"允许"，工具仍返回
  `[Repeated call denied by user: ...]`，模型被迫换方案；同时误导用户以为自己的选择生效。
  该路径没有任何集成测试 (`test_toolcall_args.cpp:965` 只验证 `extra` 标记)。
- 建议: 与 subagent 保持一致，先按 resultId 下钻再取值，例如
  ```cpp
  const auto rid = args.value("tool_call_id", std::string{});
  const auto& values = (result.is_object() && result.contains(rid)) ? result[rid] : result;
  bool allow = agentxx::middleware::interruptValueBool(values, "allow", false);
  ```
  并补一个"阈值触发 → 点允许 → 工具真的执行"的集成测试 (可用 `test_interrupt_bus.cpp`
  的总线桩实现应答)。

#### M4-4 (低) `tool->extra["..."]` 用 `operator[]` 读，会隐式插入空条目
- 位置: `toolcall.cpp:652` (`extra.find("repeatCallCheck")` 用了 find，正确) vs
  `toolcall.cpp:718` (`tool->extra["maxRetry"]`)、`toolcall.cpp:800` (`tool->extra["autoSummaryOutput"]`)
- 影响: 每次执行都会在共享的 tool 定义 map 上插入缺失键 (`std::map` 非 const `operator[]`)；
  多次调用/多会话下无内存泄漏但语义不干净 (查询不该改状态)，且 `extra` 被 P2 读时存在潜在竞争。
- 建议: 统一改用 `find` 或 `at`。

#### M4-6 (低) toolcall 重试日志与实际等待时长不一致
- 位置: `modelcall.cpp:1040-1060`
- 现状: 实等待 `retry*3 + appendDelay` 秒，日志打印 `retry + appendDelay`。
- 建议: 打印同一个 `delaySec` 变量。

### 4.3 正向确认

- `repairMessages` 的"悬挂 tool_calls → 清空 + 删除孤儿 tool 结果"逻辑考虑周全，
  且与 `has_tool_calls` 条件路由、`appendAbortMessage` 兜底形成闭环，避免死循环与无限消息堆积。
- `autoFixArgsType` 的类型修正矩阵 (string↔number/bool、单元素数组解包、枚举大小写) 覆盖面很好，
  对弱模型兼容性帮助大。

---

## 5. 中间件

范围: `middlewares/middleware.{h,cpp}` (上下文/graphData/共享存储)、`permission.{h,cpp}`、
`summarization.{h,cpp}`、`skill|memory_file|subagent_manager|interrupt_ui|interrupt_presets`

### 5.1 职责与实现要点

- `MiddlewareContext`: 每会话 `graphData`(`std::any` 字典，跨 checkpoint 以 JSON 存取)、
  `shareStore`(id→文本，可落 SQLite)、`handles` 栈；`requestInterrupt` 是"抛 NodeInterrupt
  前先把询问参数存起来、resume 后从 `interruptResult` 取值"的统一入口。
- `PermissionMiddlewareHandle`: 工具权限由**工具来源方(插件)**用 `ToolPermissionSpec` 声明
  (作用域读/写 + 目标参数名 + 目标类型 路径/文本/无)；`decideTarget` 做 worktree 会话隔离、
  配置黑名单、完全授权、最长前缀路由匹配、`noRuleOperator` 兜底；`decidePaths` 供模式类工具
  批量复核展开出的真实路径；`checkToolPermission` 注册在全局总线 `service.permission.check`。
- `SummarizationMiddlewareHandle`: token 估算(与 EventBridge 共用 `TokenCount` 同步服务)、
  确定性压缩(tool 结果去重/探索性调用折叠/噪音清理) + LLM 同上下文压缩(派生子代理，
  经 `AgentHost::spawnBatch` 直派或总线降级) + 冷却期 + 失败计数 + 硬截断兜底；
  压缩结果立即回写 `session->llmMessages` 并节流落盘。
- `SkillMiddleware`/`MemoryFileMiddleware`: SKILL.md 渐进式发现、Memory 文件注入 system prompt。
- `SubagentManagerMiddlewareHandle`: 持有 `agentxx_subagent` 工具并按配置注入。

### 5.2 问题与优化

#### M5-1 (高) 客户端重连后增量 delta 被静默丢弃 (seq 水位不回退)
- 位置: `agent/lib/src/agent/io/ws_io_transport.cpp:290-305` (去重判定)、
  `ws_io_transport.cpp:180-183` (仅在会话切换时复位)、
  `agent/lib/src/agent/io/session_server_agent_io.cpp:820-845` (握手重放/全量 sync)、
  `agent/client/src/io/tui/agent_tui.cpp:2236` (`onSync` 不触碰 transport 水位)
- 现状: 客户端 transport 用 `lastDeltaSeq_` 丢弃"已见过的 seq"，该值**只在会话切换**
  (`updateReconnectSessionId`) 时清零。而服务端 `Session::deltaSeq` 只在内存中，进程重启/会话
  重建后从 0 重新计数。重连时客户端带着旧水位发 hello，服务端因 `deltasSince()` 返回
  `nullopt` 而回退**全量 Sync** —— 但客户端水位依然停留在旧值 (例如 5000)，于是新会话产出的
  `seq = 1,2,3,...` 的 delta **全部被判为重复而丢弃**。
- 影响: 服务端重启 (或会话数据被清空重建) 后，已连接客户端自动重连成功、历史快照也拿到了，
  但之后 **UI 再也收不到实时增量** (流式文本/工具开始结束/节点事件全部不动)，
  直到服务端 seq 重新涨过旧水位 (可能上千轮对话之后)。表现为"重连后卡住不刷新"。
- 建议: 二选一
  1. 客户端在收到 `WireSyncPayload`(全量/尾窗同步) 时复位 `lastDeltaSeq_`/`lastTailHash_`；
  2. 服务端在 `WireHelloAck` 中回带"当前 seq 水位"(服务端是权威)，客户端据此设置水位。
  并在 `test_remote_agent.cpp` (已有 WS 测试) 补一条"服务端重启后重连仍能收到增量"的用例。

#### M5-4 (中) 压缩成功/失败路径都会整表重写 LLM 上下文
- 位置: `agent/lib/src/agent/session_store.cpp:627-665` (`saveLlmMessages` = `DELETE` + `INSERT`)
- 现状: 上下文是一个大 JSON blob，每次落盘整表替换；节流窗口 3s，长会话下上下文可达数百 KB~MB。
- 影响: 每 3s 一次"删除+插入整块 JSON + WAL 写放大"，对 SSD 寿命与延迟都不友好；
  同时 `updateViewMessage` 用 `json_extract(json,'$.id')` 做全表扫描 + 逐行 JSON 解析。
- 建议: ① `view_message` 增加独立的 `msg_id TEXT`(带索引) 列，`updateViewMessage` 改走索引；
  ② `llm_context` 改为按消息行存储(或以 `seq` 追加 + 定期压实)，避免整表重写。

#### M5-5 (中) `SessionStore` 的连接缓存永不淘汰
- 位置: `agent/lib/include/agentxx/agent/session_store.h:150-165` (`dbs_` map)、
  `agent/lib/src/agent/session_store.cpp:199-220` (`dbs()` 懒创建后不清理)
- 现状: 每个被写过的 sessionId 都常驻一个 `SqliteDb` (fd + WAL + page cache)，进程内不释放。
- 影响: 长期运行的服务端 (会话很多) 会持续占用文件描述符 (Linux 默认 `ulimit -n` 常为 1024)
  与内存，最终出现"打不开数据库/无法写入"。
- 建议: 引入 LRU (库内已有 `lru_cache.h`)：保留最近 N 个 (如 32) 会话连接，
  淘汰时 `close()` (SQLite 关闭是安全的，WAL 会自动 checkpoint)。

### 5.3 正向确认

- 压缩的"冷却期 + 失败计数 + 硬截断 + 压缩结果立即回写"是一套完整的工程化防护，
  设计文档与代码一致，避免了历史上反复压缩/上下文超限的死循环。
- 权限声明的"未声明即放行 + 逐路径复核 (check_paths)"分层是正确的：
  询问粒度按声明目标，安全边界按实际路径。

---

## 6. 会话 IO 与传输

范围: `agent/io/agent_io.h|cpp`、`session_server_agent_io.h|cpp`、`agent_server.h|cpp`、
`wire_protocol.h|cpp`、`channel_io_transport`、`ws_io_transport`

### 6.1 职责与实现要点

- `AgentIOBase`: 端点公共契约 (sendToPeer / getInput / handleInterrupt / registerOnBus /
  runTransportLoop / 4 个 protected 回调)；`registerOnBus` 在会话总线上注册 `service.interrupt`
  与 `service.permission` server，把 HIL 转成 `handleInterrupt` 调用。
- `SessionServerAgentIO`: 1:N 客户端管理、`WireDelta` 重放缓冲(按 seq 单调入缓冲，重放不入缓冲)、
  grace 宽限期、消息队列(暂停/唤醒/插入)、历史分页、会话切换、插件事件转发(前缀订阅 `plugin.`)。
- `AgentServer`: WS 服务 + token 鉴权 + 每会话一个 controller + 日志转发 sink。
- `wire_protocol`: `std::variant<...>` 消息 + 手写 JSON 序列化/反序列化 (先 `JsonView` 路由 type)。

### 6.2 问题与优化

#### M6-1 (中) `startGraceTimer` 覆盖旧定时器但没有取消
- 位置: `agent/lib/src/agent/io/session_server_agent_io.cpp:1419-1430`
- 现状: 每次调用都 `graceTimer_ = timer` (旧定时器对象被丢弃且未 `cancel`)，旧协程仍会到期：
  到期时若仍无客户端在线就取消轮次。
- 影响: 多个客户端相继断开 (1:N 模式常见) 时，宽限期实际由**第一个**定时器决定，
  比配置的 `gracePeriod` 提前触发取消；旧协程还会多持有一份 `shared_from_this` 直到到期。
- 建议: 函数开头 `cancelGraceTimer()` (或复用同一 timer `expires_after`)。

#### M6-4 (低) `session()` 走同步 `getOrCreate`，未加载会话会阻塞 io 线程
- 见 M3-4 (同一问题在 IO 层的表现)，这里补充：`handleHello`/`handleGetViewMessages`/`switchSession`
  都直接使用 `session()`。

---

## 7. LLM Provider 与其它协议

范围: `protocol/openai_provider.cpp`(2183)、`anthropic_provider.cpp`、`mcp_client.cpp`(2383)、
`mcp_server.cpp`、`a2a_client/server.cpp`、`acp_server.cpp`、`provider_common.h`

### 7.1 职责与实现要点

- OpenAI Provider: Chat Completions (流式 SSE + 非流式) 与 Responses (Codex) 两套；
  SSE 解析采用"先 `JsonView` 零拷贝路由/取标量，命中后再物化"的两级策略；
  `[DONE]`/usage/thinking/tool_calls 增量合并、`makeUniqueToolCallId` 补齐缺失 id、
  usage 与 reasoning_tokens 归一化写进 `message.extra`。
- Anthropic Provider: Messages API (thinking blocks / tool_use / tool_result 双向映射)。
- MCP Client: HTTP SSE 与 stdio 两种传输、多版本协商、命名空间前缀隔离工具、初始化/调用超时。
- MCP Server / A2A / ACP: 对外暴露 agent 能力的协议端点。

### 7.2 问题与优化 (基于通读与抽查)

#### M7-1 (低) 大量 `catch (...)` 只做静默降级
- 位置: `protocol/openai_provider.cpp` 内约 15 处 (如 241/248/255/262/1409/1440/1460/1469/1479/1517/1536/1543/1683/1696/1703/1711/1743)、
  `protocol/mcp_client.cpp:1306`
- 现状: 这些是"字段缺省即当未提供"的宽容解析 (与 JSON 解析兼容性要求一致)，
  但完全没有日志；一旦上游 API 改字段名，表现只是"某功能静默失效"。
- 建议: 对"结构不符合预期"的分支加日志，便于发现 provider 兼容性问题。

#### M7-3 (优化) `JsonView` 与 `Json` 双解析路径的可维护性
- 现状: 同一字段既有 `jsonStrField`(Json) 又有 `viewStrField`(JsonView) 两套实现，
  注释也强调"语义严格一致"，属于典型的双份维护 (已有测试 `test_openai_provider.cpp` 4654 行覆盖)。
- 建议: 抽出一层"字段读取接口"同时支持两种后端 (模板或 lambda)，避免后续修改只更新一边。

---

## 8. 会话持久化

范围: `agent/session_store.{h,cpp}`、`util/settings_db.{h,cpp}`、`util/sqlite.{h,cpp}`

### 8.1 职责与实现要点

- 每会话一个 `session.db` (WAL)，四张表：`view_message`(逐条 JSON)、`llm_context`(单行整表)、
  `meta`(msgIdCounter/sessionId/title/lastActiveMs)、`store`(share store KV)。
- 目录名经 `sanitizeSessionId` (非法字符替换 / Windows 保留名 / 超长截断 + 哈希尾缀)；
  读取路径先判目录存在，避免只读访问创建空库；分区恢复 (单行解析失败只跳过该行)。
- 会话列表：`listSessions()` 全量 + `listSessionsPage()` keyset 分页 (mtime 近似排序 + 精确 meta 收集)。
- 写入侧由 `Session` 的 3s 节流队列驱动，轮末强制 flush (见 3.7)。

### 8.2 问题与优化

（M5-4 / M5-5 的两条 (整表重写 / 连接不淘汰) 属于本模块，见上）

#### M8-3 (优化) 会话列表扫描成本
- 现状: `listSessions` 对每个会话目录打开独立 SQLite 连接读取 meta；
  `listSessionsPage` 先 stat 全部目录再按序打开 (注释解释了 mtime 只作启发)。
- 建议: 在 `{root}` 下维护一个 `index.db` (sessionId/title/lastActiveMs 单表)，
  写入时同步 upsert，会话列表退化为一次索引查询；`session.db` 仍是数据真相，
  索引损坏时可重建 (目录扫描兜底)。

---

## 9. 插件系统

范围: `include/agentxx/plugin/api/plugin_api.h`(952)、`plugin_kit.h`(6245, SDK)、
`src/plugins/plugin_manager_{lifecycle,capability,scheduler,tasks,vtable,adapters}.cpp`、
`tool_registry.cpp`、`client_plugin_manager.cpp`(3976)

### 9.1 职责与实现要点

- 纯 C ABI (固定宽度类型 + 8 字节对齐 + `AGENTXX_PLUGIN_CALL`)；
  `create/start/stop/destroy` 四段生命周期，`start` 是注册事务且在宿主 io 线程执行；
  多实例三铁律 (禁可变全局 / 状态放 ctx / 接口表存上下文)。
- 宿主侧 `PluginManager`: 生命周期事务、依赖与卸载顺序、inflight lease (卸载前等回调归零)、
  `ioCallSync/ioCallSyncKeep` 把插件线程的宿主调用投递回 io 线程、
  `offload` 把插件的阻塞工具丢到线程池、能力/接口表按需下发。
- 插件工具 `PluginTool`: C 回调经 offload 执行，结果经 `done` 回调回投 io 线程。
- 权限声明 (`register_tool_permission`) 与模式类工具的 `check_paths` 批量三态查询。

### 9.2 问题与优化

#### M9-2 (中) `XXRegex` 的 hyperscan scratch 不具备并发安全性
- 位置: `util/regex.cpp:14-200` (`XXRegexHP` 单 `hs_scratch` + `match()` const)
- 现状: `hs_alloc_scratch` 在构造时创建**一个** scratch，`match/replace/remove` 全程复用它。
  Hyperscan 文档明确 scratch 不可跨线程并发使用 (多线程需 `hs_clone_scratch`)。
- 影响: 目前所有调用点 (文件系统 grep、string 插件) 都是"每次调用新建 XXRegex"，
  暂未触发；但 `XXRegex` 类型本身没有注释说明该限制，一旦后续把它放进插件上下文/静态缓存
  (多线程调用) 就会变成难以复现的堆破坏。
- 建议: 在类注释中显式写明"实例不可跨线程并发使用"

### 9.3 正向确认

- 卸载流程的 lease/inflight 计数、"stop 事务未完成则拒绝 destroy/dlclose"、
  依赖插件的级联卸载与超时回滚 (`unloadAsyncUntil`) 都处理得相当严谨，
  比多数 C 插件主机的实现更完整。
- `ioCallSyncKeep` 在处理"宿主对象可能在投递期间析构"时用 keep 引用保活，避免了 UAF。

---

## 10. client

范围: `agent/client/main.cpp`(749)、`src/config_loader.cpp`(1550)、`src/mode_runners.cpp`(645)、
`src/io/stdio/*`、`src/io/tui/*`(agent_tui 2451 / components ~6000)、`src/train/*`

### 10.1 职责与实现要点

- `main.cpp`: 参数解析 (模式/模型/配置路径) → 内置环境变量注入 (可执行目录等) →
  分层加载配置 (overlay: 工作目录或 `--config`；base: `data_dir` 下) → 模型校验并给出引导 →
  `CodeAgent` 装配 → 模式分发 (local/remote × tui/cli、server、acp、train)。
  含 `__asan_default_options` (abort_on_error + log_path) 与信号处理。
- `config_loader`: YAML → Json 转换 (含 `${VAR}` 展开)、列表段 `overwrite{mode,remove}` 合并、
  路径列表/键控序列合并、旧键告警、`data_dir` 解析。
- `TUIClientAgentIO`: 双线程模型 (FTXUI UI 线程 + client io 线程)，`TUIRenderState` 快照 +
  `sharedState_.mutate()` 防死锁；消息列表用 `LazyScrollable` (懒构建 + LRU 缓存 + 视口渲染)；
  中断/权限走 `InterruptView` (声明式 UI 描述驱动)；排队输入、会话弹窗、上下文查看弹窗、
  日志侧栏、状态栏 (上下文占用/活动/tps)。
- `StdIOClientAgentIO`: 行式交互 (含逐项问答式 HIL)。

### 10.2 问题与优化

#### M10-1 (中) 客户端重连后不再接收增量 (与 M5-1 同一问题，客户端侧修复点)
- 位置: `agent/lib/src/agent/io/ws_io_transport.cpp:290-305` (去重) 与
  `agent/client/src/io/tui/agent_tui.cpp:2236` (`onSync` 不清水位)
- 建议 (客户端侧): `onSync` 时同步复位 transport 的 delta 水位 (新增
  `AgentIOTransportBase::resetDeltaWatermark()` 之类的接口)，或由服务端在 ack/Sync 里下发水位。

> 说明: TUI 渲染细节 (组件布局/折行/滚动缓存) 属"有完整测试覆盖 + 纯 UI"区域，本次仅按
> 测试模块与关键路径抽查 (`tui_scroll/tui_stream/tui_input/tui_interrupt/tui_sidebar/surface/widget`)，
> 未逐行审查其 6000 行渲染代码；建议后续若要深挖，从 `message_list.cpp`、`overlays.cpp`、
> `lazy_scrollable.cpp` 三个文件入手。

---

## 12. test / benchmark / 构建系统

### 12.1 现状

- `agent/test`: 60+ 模块 (同步 + 异步两套调度)，覆盖面广：string_util/json/json_view/regex/
  diff_util/aho_corasick/settings_db、event_stream/event_bridge/interrupt_bus/subagent_bus、
  agent/agent_host/session_persistence/concurrency/cancel/memgrowth、filesystem/command/math/
  web_search/rag/string/datetime/share_store/worktree、http/websocket/mcp/a2a/acp/
  openai_provider/anthropic_provider/remote_agent(3195 行)、plugin* 系列、TUI 系列、
  config_loader(2056 行)、ffi_c_api、toolcall_args、training。
- 大量"直测插件同一实现"的做法 (`filesystem_impl.h`/`command_impl.h`/`math_impl.h` 被测试直接 include)
  是很聪明的选择：插件行为有单测覆盖，且不依赖动态库加载。
- `agent/benchmark`: 仅 release 编译；`bench_resource.cpp` 等。

### 12.2 覆盖缺口 (与本次发现一一对应)

| 缺口 | 对应问题 |
|------|---------|
| `repeatCallCheck` 端到端 (询问 → 允许/拒绝) 无测试 | M4-1 |
| 子代理 HIL 无处理者 / 未响应路径 | M3-1 |
| 插件把图改非法 → 回退默认图后仍能跑一轮 | M3-2 |
| 服务端重启后客户端重连仍能收到增量 | M5-1 |
| `Json::dump` 浮点最短表示 | M1-1 (无 golden 测试) |
| 插件 offload 超时/取消 | M9-1 |

### 12.3 建议

1. 把上表用例补进现有模块 (多数可在现有测试框架内用桩/mock LLM 完成)；
2. `agentxx_test` 增加 `--sanitizer` 友好的"小规模并发"模式 (现有 `concurrency`/`memgrowth`
   模块很好，可扩展为"多会话同时工具调用"的场景，正好覆盖 M9-2/M11-1 类并发缺陷)；

## 附录 A: 各模块审查深度说明 (透明化)

| 模块 | 深度 | 说明 |
|------|------|------|
| util (string/json/json_view/regex/http/ws/sqlite/async_offload/exception) | 深 | 逐文件通读 + 交叉调用方 |
| event (EventBus/EventBridge) | 深 | 逐行通读 |
| agent 核心 (BaseAgent/Context/Session/AgentRunner/AgentHost) | 深 | 逐行通读 + 中断/取消/持久化路径重点核对 |
| nodes (wrap_handle/modelcall/toolcall/agentcall) | 深 | 逐行通读 |
| middlewares (permission/summarization/middleware) | 深 | 主流程逐行；skill/memory 按结构抽查 |
| io/transport/wire | 深 | 端点/重放/重连/鉴权与 wire 编解码逐段核对 |
| protocol (openai/anthropic/mcp/a2a/acp) | 中 | 结构 + 关键路径 (SSE 解析/工具映射/错误处理) 抽查；未逐行 |
| session_store/settings_db | 中 | 表结构 + 读写事务 + 分页路径逐条核对 |
| plugin 系统 (宿主侧) | 中 | 生命周期/卸载/调度/vtable 契约逐段核对；ABI 头按结构抽查 |
| client (main/config_loader/stdio) | 中 | 主流程 + 合并规则结构核对；yaml 逐条合并细节未逐行 |
| TUI 渲染组件 | 浅 (声明) | 仅关键路径抽查，见 §10 说明 |
| plugins 各插件实现 | 中 | filesystem 深查 (崩溃相关)；其余按工具实现与提示词结构抽查 |
| FFI / training / lib 内置工具 | 中 | 线程拓扑与 API 契约核对；training 按结构抽查 |
| test/benchmark/构建 | 浅-中 | 模块清单 + 覆盖缺口比对 |

## 附录 B: 建议立即执行的三条命令

```bash
# 1) 复现 M11-1 (ASan 下跑文件检索，注意保留完整 stderr/日志)
ASAN_OPTIONS=detect_stack_use_after_return=1:quarantine_size_mb=512:malloc_context_size=30 \
  path/to/agentxx_test filesystem

# 2) 验证 P0 修复后回归 (关注的模块)
path/to/agentxx_test interrupt_bus subagent_bus agent_host toolcall_args remote_agent session_persistence

# 3) 全量回归 (失败即停，便于定位)
path/to/agentxx_test --fail-fast
```
