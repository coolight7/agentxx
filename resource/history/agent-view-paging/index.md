# agent 端 viewMessages 分页（窗口化）

| 项 | 内容 |
|---|---|
| 任务短名 | `agent-view-paging` |
| 状态 | **方案已确认，待实施**（决策见 plan.md §8） |
| 范围 | `agent/lib`（Session / SessionStore / SessionServerAgentIO / EventBridge）+ wire 协议 v5；测试、文档 |
| 目标 | agent 端只常驻尾部窗口，更早的展示历史按需从 `session.db` 读；客户端分页行为不变 |

## 目标（一句话）

把"agent 端 `Session::viewMessages` 全量常驻"改成"尾部窗口常驻 + 更早页按需回库读取"，
让内存与启动耗时不再随会话历史长度线性增长。

## 现状（要点）

- `SessionsManager::getOrCreate(Async)` → `SessionStore::loadSession()` 一次读全表
  → `Session::restore()` 重建 `msgId→下标` 索引 + 对全部消息重算链式哈希。
- `Session::viewMessages` 是 public `std::vector`，只追加、永不淘汰。
- 客户端分页已可用（尾窗同步 + `WireGetViewMessages`），但端点只从内存切片，所以内存必须完整。
- 重连增量补拉（`loadViewMessagesAfter`）本来就走库，不依赖内存。

## 结论（简化后的方案）

1. `seq` 直接当绝对展示下标（表不删行 ⇒ `absIndex = seq-1`），**不改 schema、不做迁移**。
2. `Session` 改为"窗口 + 基准"：`viewWindow_` / `viewWindowBase_` / `viewWindowCap_`，总数复用 `viewSeqCounter_`。
3. 读路径统一走 `collectViewRange(start,end)`：内存优先、缺口按 `seq` 区间回库取页。
4. 写路径：追加时入窗口；超限**成批**淘汰，且只在待落盘队列为空时淘汰；更新按 id，落到窗口外直接写库。
5. 无会话库（内存模式）时 `cap = 0`，行为与今天一致。
6. 顺带修一个小洞：重连走增量补拉（读的是库）之前先 `flushViewMessages()`，
   避免"刚定稿还没落盘"的行在断线重连时静默丢失。

## 本轮版本策略（允许不兼容旧版本）

| 项 | 处理 |
|---|---|
| wire 协议 | `kVersion`/`kMinVersion` → **5**；v4 及更早对端握手被拒 |
| 会话库 | 不改 schema，无迁移；加载期不再过滤"视觉为空"的行（它占下标、客户端过滤展示） |
| 链式哈希 | **移除** `ChainHash` 与 `tailHash`（无人校验、与窗口化冲突、每条消息省一次序列化）；字段/代码/注释/文档/schema 全部清理 |
| 尾窗大小 | 新增 `WireHello.historyTail`（`-1` 用服务端默认 / `0` 完整同步 / `>0` 尾窗），由客户端声明（TUI=100，stdio/jsonl/FFI=0） |
| 接口代次 | 只抬 wire 协议到 5；FFI API 与插件接口表无成员变化，代次不动 |
| 窗口上限 | 只按条数（`session.view_window_messages`，默认 400），**不按字节** |

## 主要改动面

| 文件 | 改动 |
|---|---|
| `agent/lib/src/agent/session_store.cpp` / `.h` | `loadSession(sessionId, tailCount)` 返回窗口 + `viewWindowBase`；新增 `loadViewMessagesPage`；读路径去过滤 |
| `agent/lib/src/agent/context.cpp` / `context.h` | 窗口模型、淘汰、按 id 更新、`collectViewRange`、删哈希 |
| `agent/lib/src/agent/io/session_server_agent_io.cpp` | 页/尾窗/完整同步统一取段；增量补拉前先 flush；增量补拉 `fromIndex` 精确化 |
| `agent/lib/include/agentxx/agent/io/*.h` + `wire_protocol.*` | 协议 v5、`WireHello.historyTail`、删 `tailHash` |
| `agent/lib/src/event/event_stream.cpp` | 改按 id 访问（`StepRow` 去掉 `index`） |
| `agent/lib/src/agent/{base_agent.cpp,middlewares/*,nodes/*}` | 追加/更新调用点按新接口改 |
| `agent/client/src/**` | hello 声明 `historyTail`；删 `lastTailHash_` |
| `agent/test/core/*`、`agent/benchmark/*` | 访问器替换 + 新用例 + 删哈希断言 |
| `agent/schema/*`、`docs/zh-cn/design/*` | 重新生成 wire schema；改展示历史/附录 A/协议字段文档 |

## 风险

- `Session::viewMessages` 是跨模块 public 成员，改造面广（测试 86 处、benchmark 20 余处引用）。
- 淘汰与节流落盘队列的先后顺序必须保证"未落盘的消息不会被丢"。
- 老库里的空行会占下标；TUI 正常（`ViewReplica` 过滤），CLI 可能打印空行。
- 附件 `dataUrl` 落库时被剥离，淘汰后分页读回拿不到原图（与今天"重启后拿不到"一致）。
- 协议抬到 v5，老客户端/老服务端不能互连（现成机制会给出明确错误）。

详见 [plan.md](plan.md)。
