# agent 端 viewMessages 分页（窗口化）设计方案

# agent 端 viewMessages 分页（窗口化）设计方案

- 任务目录: `resource/history/agent-view-paging/`
- 状态: **方案已确认，待实施**（决策见 §8）
- 相关文档: `docs/zh-cn/design/index.md`（"展示历史 (viewMessages) 与客户端副本"、附录 A）、
  `docs/zh-cn/design/benchmark.md` 第 8 节

## 0. 本轮版本策略（2026-11 调整）

本轮允许**抬最低支持版本、不兼容旧版本**，因此不再为"老数据/老对端"做兼容分支：

| 项 | 处理 |
|---|---|
| wire 协议 | `WireProtocol::kVersion` / `kMinVersion` 4 → **5**；v4 及更早对端握手直接拒绝（现成机制） |
| 会话库 | **不改 schema**（`seq` 直接当绝对展示下标，见 §3.3），因此不需要迁移 |
| 展示历史语义 | 不再在加载期过滤"视觉为空"的行（过滤会打乱下标，见 §2.7）；老库里的空行进入序号空间，客户端仍会过滤展示 |
| 链式哈希 | 本次**移除**（已确认；字段/代码/注释/文档/schema 全部清理，见 §3.8） |
| 窗口上限 | 只按条数（默认 400），**不按字节** |
| 接口代次 | 只抬 wire 协议到 5；FFI API 与插件接口表无成员变化，代次不动（决策 6） |

## 1. 背景与目标

现状：客户端侧分页已经可用（尾窗同步 + 向上滚动按 `WireGetViewMessages` 拉更早历史），
但 agent 侧 `Session::viewMessages` 仍是**全量加载、全量保留**：

- `SessionsManager::getOrCreate(Async)` 调 `SessionStore::loadSession()`，
  一次 `SELECT seq, json FROM view_message ORDER BY seq` 把全部展示历史读进内存；
- `Session::restore()` 重建 `msgId → 下标` 索引，并对全部消息重算链式哈希；
- 运行期 `viewMessages` 只追加、永不淘汰；
- 端点的分页处理（`buildTailSync` / `handleGetViewMessages`）只从内存切片，
  所以分页只是"传输分页"，不是"内存分页"。

目标：

1. agent 端只常驻**尾部窗口**（条数上限可配），更早的展示历史按需从 `session.db` 读；
2. 客户端行为与 wire 语义（绝对下标、总条数、尾窗/增量补拉）保持不变；
3. 没有会话库（`enableSessionStore=false`、embedder 未开）时退化为"全部保留"；
4. 不改 LLM 上下文（模型上下文必须完整，压缩是它自己的机制）。

非目标：LLM 上下文分页；view/llm 正文合并（benchmark.md 第 8 节的另一个方向）；
传输层分页本身（已实现）。

## 2. 现状调查

### 2.1 存储

`agent/lib/src/agent/session_store.cpp`（`kSessionSchema`，schema v3）：

```sql
CREATE TABLE view_message (seq INTEGER PRIMARY KEY AUTOINCREMENT, json TEXT NOT NULL, msg_id TEXT);
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
```

- `seq`：展示历史持久化序号，写入时由 `Session::viewSeqCounter_` 显式给出（1 起、单调递增、**不删行**）；
- `msg_id`：消息 id（有索引），供 `updateViewMessage` 按 id 定位；
- `meta`：`msgIdCounter` / `viewSeqCounter` / `title` / `lastActiveMs` / `sessionId`；
- 落库时 `stripAttachmentDataUrl(msg)`：**附件 dataUrl 不入库**；
- 每条消息一个事务（`appendViewMessage`），失败只记日志并写 `lastWriteError_`。

### 2.2 加载与常驻

| 位置 | 现状 |
|---|---|
| `SessionStore::loadSession` | 全表按 `seq` 读；逐行解析（失败跳过）；**视觉为空的行被过滤**；`msgIdCounter` 缺失时用解析行数推算；`lastViewSeq` 缺失时取 `MAX(seq)` |
| `SessionsManager::getOrCreate` / `getOrCreateAsync` | `loadSession` → `session->restore(...)` → `replaceMessagesFromJson(llm 上下文)` → `setStoreHooks(...)`；异步版把加载卸载到线程池 |
| `Session::restore` | `chainHash` 从零对全部消息重算；重建 `msgIndex_`；`viewSeqCounter_ = max(lastViewSeq, messages.size())` |
| `Session::viewMessages` | **public `std::vector<ViewMessage>`**，只追加，没有淘汰路径 |

### 2.3 同步/分页读路径（`session_server_agent_io.cpp`）

| 函数 | 读法 |
|---|---|
| `buildFullSync` | `getFullViewMessagesCopy()` 整份拷贝，`fromIndex=0` |
| `buildTailSync(tailCount)` | `viewMessageCount()` + `getViewMessagesRange(total-tail, total)` |
| `buildIncrementalSync(rows, afterSeq)` | rows 来自 `store->loadViewMessagesAfter(...)`（**本来就走库**），`fromIndex = total - rows` |
| `handleGetViewMessages` | `viewMessageCount()` + `getViewMessagesRange(before-count, before)` |

握手同步策略（`handleHello`）：① delta 重放 → ② 增量补拉（库读，读库前需先 flush，见 §3.5）→ ③ 尾窗/完整同步（内存）。
尾窗大小：本地 TUI 固定 100（`client/src/mode_runners.cpp` 的 `kTuiInitialSyncTailCount`）；
远程 server、stdio、jsonl、FFI 默认 0 = **完整同步**（靠 `SessionServerAgentIO::Config::initialSyncTailCount`）。

### 2.4 写路径与节流

- `appendViewMessage`：`chainHash.append(msg.toJson().dump())`（**每条消息多一次完整序列化**）→
  分配 `msg_{:06d}` → 压入 `viewMessages` → 维护 `msgIndex_` → 压 `PendingViewOp`（append）；
- `updateViewMessage`：按 id 走 `msgIndex_` 定位（回退线性扫描）→ 整条覆盖 → 压 `PendingViewOp`（update）；
- 节流：`PendingViewOp` **只记内存下标**（不持副本），首次触发立即整队回放，窗口内（3s）合并，
  轮末 `flushViewMessages()` 补存；回放时按"当下内容"落库；
- 直播槽 `streamingView` 不落盘、不占 seq、不进 `viewMessages`（定稿时才 `appendViewMessage`）。

### 2.5 链式哈希

- `Session::chainHash`（FNV-1a，逐条 `toJson()` 后 append），对外只暴露 `HashInfo{count, tailHex}`；
- 用于 `WireHelloAck.tailHash`、`WireSyncPayload.tailHash`，客户端把它存进 `WireHello.tailHash` 回传；
- **服务端不校验**，客户端只是存下来再回传（`ws_io_transport.cpp` 的 `lastTailHash_`）；
- 重启后靠 `restore()` 重算，因此隐含要求"加载到全部消息"（与窗口化直接冲突）。

### 2.6 使用方清单（都要跟着改）

| 使用方 | 用法 | 改造方向 |
|---|---|---|
| `event_stream.cpp` `routeStreamThink` / `appendToStepRow` | `session_->viewMessages[row.index]` 取当前行再整条覆盖 | 改为按 id 取（`StepRow` 已带 `msgId`） |
| `event_stream.cpp` `commitStreamRow` | `index = viewMessages.size()-1` | 改为记录 `msgId`（`StepRow` 不再存下标） |
| `event_stream.cpp` `handleChannelWrite` | 追加/回填 | 用新接口 |
| `base_agent.cpp` / `summarization.cpp` / `session_context.cpp` | `appendViewMessage` 追加提示行/用户行 | 返回值仍是 id，内部走窗口 |
| `util/diagnostics.cpp` | 计数、全文加载、增量预览 | 计数用总数；加载改尾窗 |
| `benchmark/bench_resource*.cpp` | `sess->viewMessages.size()` / 估算字节 | 换访问器（窗口条数 + 总数） |
| `agent/test/**`（`test_session_persistence` 86 处、`test_event_bridge` 45 处等） | 直接读写 vector | 换访问器 |

### 2.7 客户端分页模型的硬约束（`client/.../model/history_window.cpp`）

`HistoryWindow::applyPage` 要求 `startIndex + count == windowStart()`（页尾紧贴当前窗口首条），
否则丢弃（`NonContiguous`）；`startIndex == 0` 表示"上方没有更早历史"。因此：

- agent 的绝对下标必须**与返回的页严格对应**：请求 `[start, end)` 就返回 `end-start` 条（中间不删行）；
- `totalCount` 必须是**同一个下标空间**里的总数。

推论：**加载期不能过滤行**（老实现会过滤"视觉为空"的行，使后续下标整体前移、页长度与请求不符）。
本轮改为"每一行占一个下标，视觉为空的行照常传输，由客户端过滤展示"。

### 2.8 与其它子系统的边界

- **LLM 上下文**：`Session::messages_` 仍全量，不在本次范围；
- **share store**：已是"库为准 + 小 LRU 缓存"，可照抄该模式；
- **delta 重放缓冲**：只保留最近 4096 条，与历史长度无关，不受影响；
- **附件 dataUrl**：下发副本清空、库里也剥离 —— "按需读回原图"今天就不成立（重启后同样拿不到），
  窗口化后限制从"重启后"扩展到"淘汰后"，写进文档。

### 2.9 现有测试/基准依赖

- `test_remote_agent.cpp` 第 22 组：造 200 条内存历史 → 尾窗 hello + 4 种分页请求（内存模式，应继续通过）；
- `test_session_persistence.cpp`（含 `getHashInfo()` 断言）/ `test_session_schema.cpp`：直接引用 `viewMessages`；
- `client/test_tui_model.cpp`：`HistoryWindow` 纯模型测试，不受影响；
- `benchmark/bench_resource*.cpp`：逻辑内存统计依赖 `viewMessages.size()`。

## 3. 方案设计（简化版）

### 3.1 思路与取舍

| 方案 | 做法 | 结论 |
|---|---|---|
| **A′ 尾部窗口 + 库直读分页（采用）** | 内存留最近 N 条；更早的页按 `seq` 区间从 `view_message` 读；`seq` 直接当绝对下标 | 无 schema 变更、无迁移、无兼容分支；改动集中在 Session/Store/端点 |
| A 原方案（上一版） | 新增 `disp_index` 列 + 迁移回填 + 能力协商 + 字节上限 | 需要兼容旧库/旧对端才有意义；本轮放弃 |
| B 只做懒加载（仍全量保留） | 启动只读尾窗，拉到更早页时读库并**追加回内存** | 只省启动开销，不省运行期内存；可作为第一步过渡 |
| C 分段文件 / D 合并 view+llm 正文 | 另立存储层 / 引用共享 | 与现有 SQLite 设计重复 / 跨压缩语义，均不采用 |

关键简化来自两条前提：

1. `view_message` **不删行**，`seq` 由 `Session::viewSeqCounter_` 单调分配 ⇒ `absIndex = seq - 1` 恒成立，
   不需要额外的"展示下标"列；总数就是 `meta.viewSeqCounter`（已存在）。
2. 允许不兼容旧版本 ⇒ 去掉"加载期过滤视觉为空的行"（它会打乱下标），空行照常占位、由客户端过滤展示；
   也去掉"老客户端不声明分页能力就不截断"的协商分支。

### 3.2 内存模型（`Session`）

```
viewWindow_       std::vector<ViewMessage>  仅保留尾部若干条（窗口内 absIndex 连续）
viewWindowBase_   uint64_t                  窗口首条的绝对下标（= 首条消息的 seq - 1）
viewWindowCap_    size_t                    窗口条数上限（0 = 不淘汰，内存模式）
```

总数直接复用已有的 `viewSeqCounter_`（`absIndex = seq - 1`），不新增字段。

- `absIndex = seq - 1`；`viewMessageCount()` 返回 `viewSeqCounter_`（**总数，不是窗口条数**）；
- `viewMessageAt(absIndex)`：落在 `[base, base + viewWindow_.size())` 返回指针，否则 `nullptr`（不在内存）；
- `viewMessageById(id)`：走 `msgIndex_`，现在只装窗口内的 id（内存随之下降）；
- `viewWindowBase()` / `viewWindowSize()`：诊断与"这一页是否全在内存"判断；
- 淘汰：`viewWindow_.size() > cap` 时**成批**从头部删除（每次删 `cap/4`，避免逐条 `erase(begin())`），
  同步维护 `viewWindowBase_` 与 `msgIndex_`；
- 删掉 public `std::vector<ViewMessage> viewMessages`，改为上面几个访问器
  （测试/诊断需要时给一个 `viewWindowSnapshot()` 取窗口副本）。

### 3.3 存储与读接口（不改 schema）

新增 `SessionStore` 读接口（与 `loadSession` / `loadViewMessagesAfter` 同一套只读临时连接）：

```cpp
// LoadedSession 扩展（不新增平行结构）：
// - viewMessages 变为"窗口内容"（按 seq 升序）
// - 新增 viewWindowBase（窗口首条的 absIndex）
// - lastViewSeq 同时就是历史总条数（absIndex = seq - 1）
struct LoadedSession {
    std::vector<ViewMessage> viewMessages;
    utilxx_base::Json        llmMessages;   // 上下文仍全量
    uint64_t viewWindowBase = 0;
    uint64_t msgIdCounter   = 0;
    uint64_t lastViewSeq    = 0;            // 历史总条数
    UsageSummary usage;
    std::string  title;
};
LoadedSession loadSession(std::string_view sessionId, size_t tailCount = 0); // 0 → 全量

std::vector<ViewMessage> loadViewMessagesPage(std::string_view sessionId,
                                             uint64_t startIndex, uint32_t count);
// SQL: WHERE seq > startIndex AND seq <= startIndex + count ORDER BY seq
```

- 窗口加载用 `ORDER BY seq DESC LIMIT ?` 再反转，只读尾部 N 行（不动 LLM 上下文/usage/title 的读取）；
- 历史总条数取 `meta.viewSeqCounter`（放在 `LoadedSession::lastViewSeq`），该键缺失（极老库）时用
  `SELECT COALESCE(MAX(seq),0)`；
- `msgIdCounter` **只取 meta**；缺失时用 `SELECT COUNT(*), MAX(seq)` 推算，
  **不能**再用"解析行数"（只读尾窗时行数≠总行数）；
- 不在读路径做"视觉为空"过滤（理由见 §2.7），`clearRunningTipFlag` 照旧；
- `loadSession(sessionId, 0)` 等价于原来的全量加载（诊断/测试沿用），调用方按需传窗口大小。

### 3.4 读路径（内存 + 库拼接）

`Session` 上提供统一取段（io 线程；DB 访问经 hooks，保持 `context.h` 不依赖 sqlite）：

```cpp
/// 取 [start, end)（absIndex 区间）；内存优先、缺口回库
/// - 返回 false 表示有一段读不到（无库/读失败），调用方按"能取多少取多少"降级
bool collectViewRange(uint64_t start, uint64_t end, std::vector<ViewMessage>& out);
```

拼接规则：

1. `[start, min(end, viewWindowBase_))` 回库读：`hooks_.onLoadViewPage(start, count)`；
2. 与窗口重叠的部分用内存副本覆盖（内存里的 tool 回填比库新）；
3. 库不可用时只返回内存能连续覆盖的**尾部后缀**（保持 `pageEnd == beforeIndex` 成立）；
   完全取不到就回空页，客户端按"没有更早历史"收敛。

端点改造：

| 函数 | 改造 |
|---|---|
| `buildTailSync(tailCount)` | `total = sess->viewMessageCount()`；`collectViewRange(max(0,total-tail), total)`，通常整段命中内存 |
| `buildFullSync()` | `collectViewRange(0, total)`（整段回库读一次，stdio/jsonl/FFI 用） |
| `handleGetViewMessages` | 用 `collectViewRange` 取页；`startIndex`/`totalCount` 语义不变 |
| `buildIncrementalSync` | `fromIndex = 首行 seq - 1`（现在可以精确算，不再用 `total - rows`） |

阻塞边界：页读在会话 io 线程（与今天的 `handleGetViewMessages` 同位置），量级是一条走索引的
`LIMIT 100` + 100 条 JSON 解析。若实测卡顿，再把页读经 `utilxx::offloadAsync` 卸到线程池并改协程
（页是只读幂等的，顺序无关）。

### 3.5 重连同步：增量补拉前先刷展示历史

重连的增量补拉（`handleHello` 的第 ② 级，`store->loadViewMessagesAfter`）读的是会话库，
而展示历史落盘有 3 秒节流窗口（`kPersistThrottleMs`）：刚定稿、还没 flush 的行只在内存里。
一旦同时出现"delta 缓冲未覆盖"（离线略久，或单条 op 超过 `kDeltaBufferMaxMsgBytes` 没进缓冲）
和"这批行还在节流窗口内"，它们既不在重放里、也不在补拉结果里，而 `buildIncrementalSync`
会把 `deltaSeq` 设成当前值 —— 客户端据此认为已收全，表现为静默缺一两条。
（这是现有行为，不是窗口化引入的，但正好在本次改动的路径上。）

本次一并修：**在 ② 读库之前对会话做一次 `flushViewMessages()`**。

- 位置：`handleHello` 里 `hello.afterViewSeq > 0` 分支，取到 `sess` 之后、`loadViewMessagesAfter` 之前；
- 线程：`handleHello` 本来就在会话 io 线程，`flushViewMessages()` 允许在此调用；
- 代价：一次同步批量写（上限是这 3 秒内的追加量），只在真正走增量补拉时发生；
- ①（delta 重放）与 ③（尾窗/完整同步）不需要 flush：① 用的是内存缓冲，③ 的尾窗本来就含未落盘行；
- 还在直播槽里的半截内容不在库里，但定稿时会用 `insert_message` 整条 upsert，客户端最终一致。

### 3.6 写路径与淘汰

`appendViewMessage`：

1. 分配 id（`msg_{:06d}`）、`seq = ++viewSeqCounter_`（`absIndex = seq-1`）；
2. 压入窗口、更新 `msgIndex_`；
3. 压 `PendingViewOp{isAppend, absIndex, counter, seq}` 并按节流规则决定是否立即落盘；
4. `maybeEvictViewWindow()`。

- `PendingViewOp.index` 从"内存下标"改为**绝对下标**（窗口基准会移动，绝对下标才稳定）；
  回放时 `pos = index - viewWindowBase_`，越界按防御记警告（正常不会发生）；
- **淘汰只发生在待落盘队列为空时**：非空就置 `viewEvictPending_`，在 `flushPendingViewOps()` 之后重试。
  这样既不会在 io 线程强做批量写库，也不会出现"窗口里删了、库里还没有"；
- 内存模式（`hooks_` 为空）时 `cap = 0`，永不淘汰。

### 3.7 更新路径

- `updateViewMessage(msg)`：按 id 定位窗口内行 → 覆盖 + 压 update 队（节流）；
- 定位不到（已淘汰）：**直接写库**（`onUpdateViewMessage` 本就按 `msg_id` 定位），
  不改内存、只记 debug 日志；
- `EventBridge` 全部改成按 id 访问（`StepRow` 只保留 `msgId`，删掉 `index`）：取当前行 → 拼接/覆盖 → `updateViewMessage`，
  单轮内淘汰也不会写错行；按 id 取不到（已淘汰）时跳过内存增量、只写库，由定稿的整条 upsert 收敛客户端显示。

### 3.8 链式哈希：本轮移除（已确认）

`ChainHash` / `tailHash` 的作用只是"内容指纹"，当前**没有任何校验方**（服务端不看 `hello.tailHash`），
而它带来两项成本：每条消息追加时多一次 `msg.toJson().dump()`（与落库序列化重复），
以及恢复时必须加载全部消息。窗口化后继续维护它只能靠"持久化哈希状态"或"改成语义不同的窗口指纹"，
两者都比直接删掉复杂。

决策：**全部清理** —— 不留"窗口指纹"变体，也不做哈希状态持久化。清理范围（按层）：

| 层 | 位置 | 处理 |
|---|---|---|
| 会话 | `context.h`：`Session::chainHash` 成员、`HashInfo`、`getHashInfo()`、类头注释里的链式哈希描述 | 删除 |
| 会话 | `context.cpp`：`appendViewMessage` 的 `chainHash.append`、`restore` 里的重算与相关注释 | 删除 |
| 类型 | `conversation_types.h` 的 `ChainHash` 类（含注释）、`conversation_types.cpp` 的实现 | 删除 |
| wire | `wire_protocol.h/.cpp`：`WireHello.tailHash`、`WireHelloAck.tailHash`、`WireSyncPayload.tailHash` 字段与序列化/反序列化、`makeHello` 参数与注释 | 删除（协议 v5） |
| 传输 | `ws_io_transport.h/.cpp`：`lastTailHash_`、`makeHello` 实参、重连注释 | 删除 |
| 运行时 | `session_runtime.h/.cpp` 的 `cfg.tailHash`；`session_server_agent_io.h/.cpp` 的 `currentTailHash()`（无人调用）、三处 `p.tailHash = ...`、`handleHello` 的局部变量 | 删除 |
| 客户端 | `client/src/mode_runners.cpp` 里 hello 初始化的 `tailHash` | 删除 |
| 测试 | `test_session_persistence`（`getHashInfo()` 断言）、`test_wire_roundtrip`、`test_wire_schema`、`test_remote_agent`、`test_misc_fixes` 的相关断言 | 删除或改为"字段不存在" |
| schema | `agent/schema/wire-schema.json` | 重新生成 |
| 文档 | `docs/zh-cn/design/index.md`（附录 A 的 `ChainHash`/`SyncPayload.tailHash`、展示历史小节）、`wire-protocol-fields.md`、`tui.md` | 改写/重新生成 |

收尾自检：`grep -rE "tailHash|ChainHash|getHashInfo"`（排除 `agent/build`、`agent/third_party`、`resource/history`）应为空。
FFI 与插件接口表没有成员变化，因此不抬代次（决策 6）。

### 3.9 恢复

```cpp
auto loaded = sessionStore->loadSession(sessionId, cfg.viewWindowMessages);
session->restoreWindow(std::move(loaded.viewMessages), loaded.viewWindowBase,
                      loaded.lastViewSeq, loaded.msgIdCounter);
session->replaceMessagesFromJson(loaded.llmMessages); // 上下文仍全量
```

`Session::restore(messages, msgIdCounter, lastViewSeq)` 保留为
`restoreWindow(messages, /*base=*/0, /*total=*/messages.size(), msgIdCounter)` 的包装，
供测试与内存模式使用。

- `restoreWindow` 沿用现有约束：整体替换窗口前先清空待落盘队列（加载阶段不存在未落盘增量）；
- 加载阶段不重建 `msgIndex_` 以外的任何全量结构（哈希已删）。

### 3.10 协议：抬到 v5，尾窗大小由客户端声明

- `WireProtocol::kVersion = 5`、`kMinVersion = 5`：v4 及更早对端在 hello 阶段被拒（现成机制，
  客户端会看到明确错误而不是"历史缺一段"）；
- `WireHello` 新增 `historyTail`（`int32_t`，默认 `-1`，为 `-1` 时 JSON 不携带该字段）：`-1` = 用服务端默认，
  `0` = 完整同步，`>0` = 尾窗条数；由客户端声明"首次接入/切换会话要多少条历史"，服务端记住并用于之后每次 Sync；
  - 本地/远程 TUI：`100`（可在客户端侧配置）；
  - stdio / jsonl / FFI：`0`（完整同步，界面没有"向上翻页"）；
  - 未发 hello 的内部通道：`-1`（用 `initialSyncTailCount`）；
- `SessionServerAgentIO::Config::initialSyncTailCount` 保留为"hello 未声明时的默认值"
  （插件运行时通道、未发 hello 的内部端点），不再需要为远程部署手工配置；
- `buildTailSync` / `handleSwitchSession` 都用"记住的 `historyTail`"；
- 声明的条数超过会话历史时自然退化为完整同步（`start = max(0, total - tail)`）；
- 重新生成 `wire-schema.json` 与 `wire-protocol-fields.md`；
- 只抬 wire 协议代次；FFI API 与插件接口表没有成员变化，代次不动（决策 6）。

### 3.11 配置与降级矩阵

新增 `AgentConfig`（与 `enableSessionStore` 同级，yaml 键同步登记）：

| 键 | 默认 | 说明 |
|---|---|---|
| `session.view_window_messages` | 400（约 4 页，已确认） | 0 = 不淘汰（老行为）；有会话库才生效 |

取值链路：`AgentConfig` → `SessionsManager`（`BaseAgent` 初始化时写入）
→ 每个会话 `restoreWindow` 时使用；运行期不再改变（切会话/重启按当时配置）。

降级矩阵（显著变短）：

| 场景 | 行为 |
|---|---|
| 未启用 `enableSessionStore` | `cap = 0`，全量保留，分页仍从内存切片 |
| 库不可读（只读目录/租约被占/库版本高于本程序支持） | 更早的页回空页或短页，客户端停止上拉；写路径仍按 `lastWriteError_` 提示 |
| 老库里有视觉为空的行 | 正常占下标、照常传输；TUI 侧 `ViewReplica` 过滤不显示（CLI 可能打印一行空行） |
| 会话重建（seq 回退） | 现有逻辑已处理（`deltasSince` 回退完整同步） |

### 3.12 可观测性

- 加载快照 / `--dump-diagnostics`：`view window: base/size/total/cap`、`view pages: db/memory`；
- `KeyMetrics`：`view.page.db`、`view.page.memory`、`view.evict.count`、`view.collect.partial`；
- 日志：淘汰、窗口外更新（debug）；页读失败（warn，去重）。

### 3.13 预期收益（量级）

- 纯文本：benchmark.md 第 8 节记录"view/llm 各持正文，200K token 历史约 1.9 MB"，
  窗口化后省掉展示侧一份；长会话（数千条）在个位数 MB；
- 带附件：单条用户消息的 dataUrl 可达 10/25/50 MB，多轮贴图后这部分可整段淘汰，收益远大于文本；
- 启动：不再解析全部历史 JSON、不再重算哈希，大库从数百 ms 降到毫秒级（估算，待 benchmark 复测）；
- 追加：去掉每条消息一次的哈希序列化，CPU 与临时分配略微下降（估算，待 benchmark 复测）。

## 4. 关键不变量

1. `absIndex = seq - 1`；`viewWindowBase_ + viewWindow_.size() == viewSeqCounter_`；
2. 窗口内下标连续；窗口外的下标只能从库取；页 `[start,end)` 必须恰好返回 `end-start` 条（不删行、不过滤）；
3. 淘汰只发生在待落盘队列为空时；
4. `msgId` 全局唯一且单调（恢复规则不变）；
5. 窗口内更新用内存副本覆盖库内容（内存比库新）。

## 5. 分步实施（每步可独立编译 + 通过测试）

### 步骤 1：存储侧

- `loadSession` 增加 `tailCount` 参数（0 = 全量），返回窗口内容 + `viewWindowBase`；
  新增 `loadViewMessagesPage`；
- 去掉读路径的"视觉为空"过滤；`msgIdCounter` 改为 meta/`COUNT(*)` 回退；
- `appendViewMessage` / `updateViewMessage` 接口不变（seq 已由调用方给出）。

验证：`test_session_persistence`（窗口加载内容/总数/base、按页读取、空行不再被丢弃）、
`test_session_schema`（无需迁移，确认 v3 库直接可读）。

### 步骤 2：`Session` 窗口模型

- 去掉 public `viewMessages`，加 `viewWindow_`/`viewWindowBase_`/`viewWindowCap_` 与访问器；
- `appendViewMessage` / `updateViewMessage` / `restoreWindow` / `collectViewRange` / `maybeEvictViewWindow`；
- `PendingViewOp.index` 改绝对下标；淘汰与节流顺序按 §3.6；
- `SessionStoreHooks` 增 `onLoadViewPage`；
- 删除链式哈希（§3.8）。

验证：新增用例（并入 `test_session_persistence`）：超过 cap 后总数不变、`viewMessageAt(base-1)` 为空、
`collectViewRange` 跨窗口边界拼出完整序列、内存模式不淘汰、"待落盘不为空时不淘汰"、窗口外更新走库。

### 步骤 3：端点 + 协议 v5

- `buildTailSync` / `buildFullSync` / `handleGetViewMessages` 改走 `collectViewRange`；
  增量补拉的 `fromIndex` 用首行 seq 精确计算；
- 增量补拉前先 `flushViewMessages()`（§3.5，堵住"节流窗口内断线重连丢行"）；
- `kVersion`/`kMinVersion` 抬到 5；`WireHello.historyTail` + 服务端记忆；删除 `tailHash` 字段；
- 重新生成 wire schema 与字段文档。

验证：`test_remote_agent` 第 22 组 + 新增"有库 + 窗口 50 + 200 条历史"（尾窗、跨窗口页、重启后继续分页、
`historyTail` 生效、客户端声明 0 时仍完整同步）；再加"追加后 3 秒内断线"（清空 delta 缓冲）
走增量补拉能拿到全部已定稿行；`test_wire_roundtrip` / `test_wire_schema`；
`test_protocol_version`（若存在）确认 v4 被拒。

### 步骤 4：写入方改造 + 测试/基准适配

- `EventBridge` 改按 id 访问；`BaseAgent` / `summarization` / `session_context` 用新接口；
- `diagnostics.cpp` 用总数 + 尾窗加载；`bench_resource*.cpp` 换访问器；
- 测试里的 `session->viewMessages` 全部替换，`getHashInfo()` 断言删除；
- 按 §3.8 清单清理 `tailHash` / `ChainHash` 的字段、注释、schema 与文档引用；

验证：全量 `agentxx_test`（Debug/ASan）通过。

### 步骤 5：配置、可观测性、文档、基准

- `session.view_window_messages` + 加载快照 / `--dump-diagnostics` / `KeyMetrics`；
- 更新 `docs/zh-cn/design/index.md`（展示历史小节、附录 A、协议版本）、`config-keys.md`、`wire-protocol-fields.md`；
- benchmark 复测（长会话 / 多图附件两组），数字补进 `benchmark.md`。

## 6. 验证计划

| 用例 | 关注点 |
|---|---|
| 窗口上限 | 追加超过 cap：总数正确、窗口条数 ≤ cap、`msgIndex_` 只留窗口内 id |
| 淘汰顺序 | 节流窗口内连续追加 3×cap：落盘前不淘汰、最终全部落库、无丢消息 |
| 跨边界取段 | `[base-30, base+30)` 拼出 60 条、顺序正确、内存覆盖库内旧版本 |
| 窗口外更新 | 淘汰后 `updateViewMessage(oldId)`：库内行被更新、内存不变 |
| 恢复 | 重启后 base/总数/msgIdCounter 与重启前一致（哈希断言已删除） |
| 分页 | 页 / 尾窗 / 完整同步三入口结果一致（内容 + 起始下标 + 总数） |
| 重连补拉 | 追加后 3 秒内断开且 delta 缓冲不可用 → 增量补拉仍能拿到全部已定稿行（§3.5） |
| 版本 | v4 客户端握手被拒；`historyTail=0` 时完整同步；`historyTail=100` 时尾窗 |
| 降级 | 内存模式全量；库不可读时短页/空页且客户端能收敛 |
| 空行 | 老库里视觉为空的行占下标但不显示（TUI），页长度与请求一致 |
| 清理自检 | `grep` 搜 `tailHash` / `ChainHash` / `getHashInfo` 均无命中（排除 build/third_party/history）；wire schema 无该字段 |

## 7. 风险与备选

| 风险 | 说明 | 处置 |
|---|---|---|
| public `viewMessages` 改动面 | 跨模块 + 测试/基准大量直接引用 | 先加访问器逐处替换，最后删成员；编译错误即清单 |
| 淘汰丢消息 | 待落盘队列按下标引用内存，淘汰后下标失效 | 只在队列为空时淘汰 + 单测覆盖 |
| 老库空行 | 不再加载期过滤，CLI 可能打印空行 | 文档写明；如需清理，另做一次性"重排 seq"维护脚本（不在本轮） |
| 页读阻塞 io 线程 | 与写库共用 SessionStore 互斥锁 | 先同步实现并测耗时；必要时改协程 + 线程池 |
| 附件 dataUrl | 落库已剥离，淘汰后读不回原图 | 与今天"重启后同样拿不到"一致，文档写明 |
| 协议 v5 不兼容 | 老客户端/老服务端无法互连 | 现成机制会给出明确错误；同上代次调整惯例（升版本 + 更新 schema） |
| 哈希清理遗漏 | `tailHash`/`ChainHash` 散落在 20+ 文件（含注释、schema、文档） | 按 §3.8 清单逐项改；收尾 grep 自检 + `test_wire_schema` |

备选（想先小步落地）：只做步骤 1 的 `loadSession(…, tailCount)`（启动只读尾窗、运行期仍全量），即方案 B；
收益只有启动开销，风险最小，后续再切 A′。

## 8. 已确认的决策（2026-11）

| # | 决策 | 落地位置 |
|---|---|---|
| 1 | **移除链式哈希**：字段、代码、注释、文档、schema、测试断言全部清理，不留"窗口指纹"变体 | §3.8 |
| 2 | **窗口默认 400 条**（页大小 100、尾窗 100） | §3.11 |
| 3 | **尾窗大小由客户端在 hello 里声明**（`WireHello.historyTail`） | §3.10 |
| 4 | **接受老库"视觉为空"的行进入序号空间**（TUI 过滤展示，CLI 可能多一行空行） | §3.11 降级矩阵 |
| 5 | **`EventBridge` 改为按 id 访问**（`StepRow` 去掉 `index`） | §3.7 |
| 6 | **只抬 wire 协议到 5**；FFI API 与插件接口表无成员变化，代次不动 | §0 / §3.10 |

实施顺序按 §5（每步独立可验证）；实施记录写同目录的 `work.md`（进度、每步改动与验证结果、
与计划不一致的地方）；完成后把结论回收进 `docs/zh-cn/design/index.md`。
