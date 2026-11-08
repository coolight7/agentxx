# agent 端 viewMessages 分页（窗口化）— 实施记录

- 计划: [plan.md](plan.md)
- 任务卡片: [index.md](index.md)
- 开始时间: 2026-11-08

## 当前进度

| 步骤 | 状态 | 说明 |
|---|---|---|
| 0 现状调查 | 已完成 | 见下表"现状调查结论" |
| 1 存储侧（SessionStore） | 已完成 | `loadSession(tailCount)` / `loadViewMessagesPage` / 去空行过滤 |
| 2 Session 窗口模型 | 已完成 | 窗口/基准/访问器/淘汰/`collectViewRange`/删链式哈希/指标 |
| 3 端点 + 协议 v5 | 已完成 | `collectViewRange` 取段、`historyTail`、增量补拉前 flush、删 `tailHash` |
| 4 写入方 + 测试/基准适配 | 部分完成 | EventBridge 按 id、diagnostics、基准与测试已改；**新用例待补** |
| 5 配置/可观测/文档/schema | 部分完成 | 配置键 + 快照 + 指标已完成；wire schema 已重生成；**docs 待改** |
| 6 全量验证 | 待开始 | `agentxx_test` 全量 + benchmark 复测 |

## 现状调查结论（步骤 0）

### 存储（`agent/lib/src/agent/session_store.cpp`）

- `view_message(seq INTEGER PRIMARY KEY AUTOINCREMENT, json TEXT, msg_id TEXT)`；
  `meta` 里已有 `viewSeqCounter` / `msgIdCounter` / `title` / `lastActiveMs` / `sessionId`。
- `loadSession()` 全表读；**过滤"视觉为空"的行**（`isDisplayableViewMessage`），
  且 `msgIdCounter` 缺失时用"解析行数"推算 —— 只读尾窗后行数不再等于总条数。
- `loadViewMessagesAfter()` 同样过滤空行。

### 会话（`agent/lib/src/agent/context.cpp`）

- `Session::viewMessages` 是 public `std::vector`，`appendViewMessage` 只 `push_back`；
  `chainHash.append(msg.toJson().dump())` 每条多一次序列化。
- `restore()` 对全部消息重算链式哈希、重建 `msgIndex_`。
- `PendingViewOp.index` 是**内存下标**；节流 3s，`flushPendingViewOps()` 按当下内容落库。

### 端点（`agent/lib/src/agent/io/session_server_agent_io.cpp`）

- `buildFullSync` / `buildTailSync` / `handleGetViewMessages` 全部从内存切片；
- `buildIncrementalSync` 的 `fromIndex = total - rows.size()`（已改精确计算）；
- `handleHello` 第 ② 级（`loadViewMessagesAfter` 读库）**之前没有 flush**
  —— 节流窗口内刚定稿的行可能既不在 delta 缓冲、也不在库里。

### 链式哈希散落点（已全部清理）

`ChainHash`（conversation_types）、`Session::chainHash`/`HashInfo`/`getHashInfo`、
`WireHello.tailHash` / `WireHelloAck.tailHash` / `WireSyncPayload.tailHash`、
`ws_io_transport` 的 `lastTailHash_`、`session_runtime` 的 `cfg_.tailHash`、
`session_server_agent_io` 的 `currentTailHash()`、client `mode_runners.cpp` 的 hello 初值、
测试（`test_session_persistence` / `test_wire_roundtrip` / `test_wire_schema` /
`test_remote_agent` / `test_misc_fixes`）、`agent/schema/wire-schema.json`、设计文档。

## 实际改动清单

### 存储（`agent/lib/{include/agentxx/agent,src/agent}/session_store.*`）

- `LoadedSession` 新增 `viewWindowBase`（窗口首条绝对下标），`lastViewSeq` 语义改为
  "历史总条数"（`absIndex = seq - 1`）；`loadSession(sessionId, tailCount = 0)`，
  `tailCount > 0` 时用 `ORDER BY seq DESC LIMIT ?` 再反转，只解析尾部 N 行。
- 新增 `loadViewMessagesPage(sessionId, startIndex, count)`：
  `WHERE seq > startIndex AND seq <= startIndex + count ORDER BY seq`（走主键），
  只读临时连接、不取写租约。
- 删除 `isDisplayableViewMessage`：读取期不再过滤空行（会打乱绝对下标）。
- `msgIdCounter` / `lastViewSeq` 的回退从"解析行数"改成
  `SELECT COUNT(*), MAX(seq)`（只读尾窗时行数 != 总行数）。

### 会话（`agent/lib/{include/agentxx/agent,src/agent}/context.*`）

- 去掉 public `std::vector<ViewMessage> viewMessages`，改为
  `viewWindow()`（只读借用，仅窗口）/ `viewWindowBase()` / `viewWindowCap()` /
  `viewMessageCount()`（总数）/ `viewMessageAt(abs)` / `viewMessageById(id)` /
  `collectViewRange(start,end,out)` / `setViewWindowCap`。
- `restoreWindow(messages, base, total, msgIdCounter)`；`restore(...)` 变成等价包装。
- `msgIndex_` 改存**绝对下标**，只装窗口内 id；`absoluteIndexOfId` 命中不到时线性
  扫描窗口并修索引；淘汰时同步 erase。
- `PendingViewOp.index` → `absIndex`（绝对下标）；回放按
  `pos = absIndex - viewWindowBase_` 取行。
- 淘汰：`maybeEvictViewWindow()` 成批删除（每次至少 `cap/4`），
  **待落盘队列非空时不删**（置 `viewEvictPending_`，flush 后再试）。
- `updateViewMessage` 命中不到窗口（已淘汰）时只写库、不改内存（debug 日志）。
- `SessionStoreHooks` 新增 `onLoadViewPage`（读库页），由 `makeSessionStoreHooks` 接到
  `SessionStore::loadViewMessagesPage`。
- `SessionsManager` 新增 `viewWindowMessages`（上限）与 `metrics`（弱引用），
  创建/恢复会话时 `setViewWindowCap` + `setMetrics`；`getOrCreate(Async)` 传尾窗条数。
- 删除 `ChainHash` 成员/`getHashInfo`/`getFullViewMessagesCopy`/`getViewMessagesRange`。

### 端点与协议（`session_server_agent_io.*` / `wire_protocol.*` / `agent_io_transport.h`）

- `WireProtocol::kVersion` / `kMinVersion` → **5**。
- `WireHello.tailHash` → 删除；新增 `historyTail`（`int32_t`，默认 -1 = 未声明，
  JSON 不携带）；`WireHelloAck.tailHash`、`WireSyncPayload.tailHash` 删除。
- 端点记住 `clientHistoryTail_`，`effectiveSyncTailCount()` 决定每次 Sync/切换会话的尾窗；
  `Config::initialSyncTailCount` 退化为"hello 未声明时的默认值"。
- `buildFullSync` / `buildTailSync` / `handleGetViewMessages` 全部走 `collectViewRange`；
  取不到时回空页（客户端停止上拉）。
- `buildIncrementalSync` 的 `fromIndex` 精确化为 `rows.front().seq - 1`。
- `handleHello` 第 ② 级（增量补拉）读库前先 `sess->flushViewMessages()`。

### 写入方 / 展示

- `EventBridge`：`StepRow` 只剩 `role + msgId`；`toolCallHistoryIndex_` →
  `toolCallMsgIds_`；取行统一走 `viewMessageById`；`commitStreamRow` 返回 msgId。
- `ws_io_transport`：`lastTailHash_` → `helloHistoryTail_`（重连沿用声明）。
- `session_runtime`：`cfg.tailHash` → `cfg.historyTail`。
- client：远程 TUI hello 声明 `historyTail = 100`（同本地 `kTuiInitialSyncTailCount`），
  远程 CLI 声明 `0`（无翻页界面），本地 TUI 走 `cfg.historyTail`。

### 配置 / 可观测

- `AgentConfig::sessionViewWindowMessages`（默认 400，0 = 不淘汰）+ yaml
  `session.view_window_messages` + `YamlAppConfig` 同名字段 + `main.cpp` 应用；
  `BaseAgent` 构造时写入 `SessionsManager::viewWindowMessages`（仅在建了会话库时生效）。
- 加载快照 `features.view_window`；`--dump-diagnostics` 增加
  `view window: base/size/cap`，`persisted view messages` 改用总条数（只读尾窗 100 条）。
- `KeyMetrics` 新增 `view_window` 段：`pages_memory` / `pages_store` / `evicted` /
  `collect_partial`；`summary()` 同步追加。

### 测试 / 基准

- `->viewMessages` → `->viewWindow()`（会话对象）；`SessionStore::LoadedSession` 的
  `viewMessages` 保留原样。
- 删掉链式哈希断言（`test_session_persistence` / `test_misc_fixes` 的 `test_chain_hash`）
  与 `tailHash` 字段用法（`test_remote_agent` / `test_wire_roundtrip` / `test_wire_schema`）。
- "视觉为空的行不进历史"用例改为"照样占位、可被按页读回"。
- `wire-schema.json` / `wire-protocol-fields.md` 已用 `AGENTXX_UPDATE_WIRE_SCHEMA=1` 重生成。

## 与计划不一致的地方

1. 计划里 `Session` 暴露 `viewWindowSnapshot()`；实际提供的是**只读引用**
   `viewWindow()`（省一次整窗拷贝，测试/诊断/基准都只需读）。
   淘汰计数等另给了 `viewWindowBase()` / `viewWindowCap()`。
2. `collectViewRange` 遇到库页不全时**整页丢弃**并返回 false（不是"能取多少取多少"）：
   客户端 `HistoryWindow::applyPage` 要求 `startIndex + count == windowStart()`，
   短页本来就会被丢弃，明确回空页语义更简单。
3. `KeyMetrics` 的淘汰计数记的是**条数**（`view.evicted`）而不是"淘汰次数"，
   更能反映释放了多少内存（计划里写的是 `view.evict.count`）。

## 注意事项 / 踩坑

- `msgIndex_` 需要 `mutable`：`viewMessageById` 是 const 查询，但索引不一致时会在
  查询路径里修索引。
- `std::unordered_map<std::string, ...>` 没有透明哈希，`string_view` 查表要先构一个
  `std::string` 键（否则 MSVC 报 C2665）。
- 只读尾窗后 **`msgIdCounter` 不能用"解析行数"回退**：尾窗行数 != 总行数，
  必须走 `SELECT COUNT(*), MAX(seq)`（已改）。
- Windows 下 `Set-Content -Encoding utf8` 在 PowerShell 7 是 **UTF-8 无 BOM**，符合本项目约定。
