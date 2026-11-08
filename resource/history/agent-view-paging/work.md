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
| 4 写入方 + 测试/基准适配 | 已完成 | EventBridge 按 id、diagnostics、基准适配；新用例已补（见“新增用例”） |
| 5 配置/可观测/文档/schema | 已完成 | 配置键 + 快照 + 指标 + wire schema + 设计文档（index/tui/benchmark）均已改 |
| 6 全量验证 | 已完成 | 全量 42544 通过（1 例既有偶发重跑通过）；release 侧已编过并跑过 `resource_real_tui`；窗口收益需新场景，见“待完成” |

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

## 文档更新（步骤 5）

### `docs/zh-cn/design/index.md`

- 能力概述：“多轮对话”改成“展示历史落库为 append-only 完整历史, 会话内存只常驻尾部窗口”
- “多会话与并发”：`链式哈希` 条目换成 `展示历史尾部窗口`（默认 400 / 0 = 不淘汰 /
  无库不淘汰）；线程绑定条目去掉 `chainHash`
- “会话 SQLite 持久化”小节：
  - `view_message` 表写明 `seq` 主键 = 绝对下标 + 1、加载只解析尾窗、淘汰不删行；
    `meta` 表补 `viewSeqCounter`（历史总条数）
  - 新增 `展示历史窗口化` 块：写/淘汰/读三条路径，`collectViewRange` +
    `onLoadViewPage` 回库、短页整页失败、老库空行不再过滤、附件 `dataUrl` 淘汰后取不回
  - `getOrCreate` 改写为“恢复尾窗 + `COUNT(*)/MAX(seq)` 取总数与 msgIdCounter”；
    `PendingViewOp.index` → `absIndex`（绝对下标），淘汰只在待落盘队列为空时做
  - 开关条目补 `session.view_window_messages`
- 连接与重连：“历史分页”条目改成“条数取自 Hello 的 `historyTail`（远程 TUI 100 /
  远程 CLI 0）”，并说明“服务端内存窗口”与“同步窗口”是两个量
- 会话切换、时序图、目录树：`tailHash` → `historyTail`、`viewMessages + chainHash` →
  `展示历史尾部窗口`、删 `ChainHash` 条目；Hello/重连两条框线的短横数量按字宽重算，
  保证框宽不变（用脚本校长度）
- 端点描述：`handleGetViewMessages` / `switchSession` 注明经 `collectViewRange`、回空页、
  `historyTail`
- 附录 A：`toJson/fromJson` 去掉“与链式哈希共用”；`SyncPayload` 字段删 `tailHash`；
  `ChainHash` 条目换成 Hello 的 `historyTail` 说明；展示历史小节加“内存只常驻尾部窗口”
  与协议 v5 / FFI 3 的说明

### `docs/zh-cn/design/tui.md`

- 直播槽描述去掉“不计链式哈希”
- §3.5 去掉“服务端 `Session::viewMessages` 与会话历史同增”，改为“服务端自己的窗口
  有 `session.view_window_messages` 上限，更早的回库读”
- “可重新拉回”条目：绝对下标 = `seq - 1`，内存里没有时回库读

### `docs/zh-cn/design/benchmark.md`

- 第 5 节结论 4：尾窗同步改为“Hello 的 `historyTail`”，并补一句“服务端自己的内存窗口由
  `session.view_window_messages`（默认 400）控制，两者无关”

### 自动生成物

- `agent/schema/wire-schema.json` / `docs/zh-cn/design/wire-protocol-fields.md`
  （`AGENTXX_UPDATE_WIRE_SCHEMA=1`）
- `agent/schema/config-keys.json` / `docs/zh-cn/design/config-keys.md`
  （`AGENTXX_UPDATE_CONFIG_KEYS=1`，新键 `session.view_window_messages` 归属“服务端”）

## 新增用例

- `test_session_persistence`：`testViewMessagesPageRead`（按页读、越界、空行也行）、
  `testViewWindowPaging`（窗口上限/淘汰延迟到 flush/跨边界取段/淘汰后更新只写库/
  重启后 base+总数一致/内存模式不淘汰/没 `onLoadViewPage` 时取段失败）
- `test_remote_agent`：`test_view_paging_with_store`（真实 SessionStore + 窗口 50 +
  200 条历史：尾窗同步、跨窗口分页、重启后继续分页、页不连续时的收敛）
- `test_wire_roundtrip`：`kVersion`/`kMinVersion` = 5、v4 被拒、`historyTail` 往返
- `test_config_keys`：登记 `session.view_window_messages`（schema 随之重生成）

## 额外改动（与功能本体无关，但为了 `boundaries` 通过）
`boundaries` 模块会扫源码与设计文档里的禁用词，本轮清理了这些早已超期的词：

- 代码/文档：`载入` → `加载`；`宿主契约` → `主程序契约`；`宿主运行时` → `主程序运行时`
  （`prompt.h` / `prompt.cpp` / `training.cpp` / `test_agent.cpp` / `index.md` / `plugins.md`）
- `agent/third_party/cxx_pluginxx_ui`：`通用载体` → `通用数据字段`
  （`docs/integration.md`、`include/pluginxx/ui/item.h`、`README.md`）——
  **改动落在子模块工作区**，需在子模块仓库自行提交（本次未提交）

## release 侧复测与 benchmark 编译修复（2026-11-08）

benchmark 模块只在 release 编，且上次构建产物是 10 月 26 日的 —— 改动后先试编了一次，
**发现模块本身已经编译不过**（与本次改动无关的旧毛病 + 本次的 `WireHello` 字段变化）：

| 问题 | 位置 | 修法 |
|---|---|---|
| `agentxx::agent::estimateViewMessageBytes` 不存在（库里没有这个函数） | `bench_mem_logical.h:94,305` | 在头文件里就地实现一个估算（结构体尺寸 + id/text/source + tool 子结构 + attach 估算） |
| `WireHello{sessionId, "", 0, "", "", "en"}` 比新布局多一个字段 | `bench_resource_real.cpp:306,574,713` | 按新顺序改为 `{sessionId, "", 0, "", "en"}`（`tailHash` 已删，`historyTail` 默认 -1 → 用服务端默认 100） |

修完后 `agentxx_static`（Release）+ `agentxx_benchmark`（Release）均编译通过，
`agentxx_benchmark --list` 与 `resource_real_tui` 跑通（产物：`exec/bench/bench_20261108_211454.{json,md}`）。

实测（Release, 同机，跑时全量测试已结束、CPU 空载）：

| 采样点 | RSS | 私脏 | 展示历史 (view) | LLM 上下文 | token |
|---|---|---|---|---|---|
| startup | 22.79 MB | 7.70 MB | 3 条 / 1 KB | 3 条 / 12 KB | 0 |
| ctx100k | 27.38 MB | 11.23 MB | 354 条 / 524 KB | 354 条 / 395 KB | 99216 |
| ctx200k | 30.79 MB | 14.22 MB | 708 条 / 1051 KB | 708 条 / 781 KB | 199280 |

帧耗时 0.19~0.21 ms，尾窗 100 + 分页拉取 300 条正常。

**关键发现：这个场景里 `view` 条数 = 全量（354/708），因为 benchmark 的所有资源场景都
`enableSessionStore = false`** —— 没库就 `cap = 0`、不淘汰，窗口根本不会生效。所以现有
场景量不出本次改动的收益；ctx200k 那 708 条若开了库 + 窗口 400，展示侧会从
1051 KB 降到 600 KB 量级（按 1.48 KB/条估算）。要量真实收益需新增“开库 + N 条历史 +
重启加载”的场景（对比 `session.view_window_messages` = 400 与 0），已列入下面“待完成”。

## 验证结果

| 模块 | 结果 |
|---|---|
| `session_persistence` | passed=726 failed=0 |
| `remote_agent` | passed=602 failed=0 |
| `wire_roundtrip` | passed=262 failed=0 |
| `wire_schema` | passed=1325 failed=0 |
| `boundaries` | passed=19 failed=0（术语清理后） |
| `config_keys` | passed=217 failed=0（登记新键并重生成 schema 后） |
| **全量** `agentxx_test` | passed=42544 failed=1 —— 唯一失败是 `plugins` 第 1114 行
（JS 引擎 disable/enable 紧邻后执行 `js_hello` 文本里没拿到入参）；
单独重跑 `plugins` 为 609/0 通过，判定为并发压力下的既有用例偶发，与本次改动无关 |

构建方式（顶层一次全量构建会超时，所以分两步）：

```powershell
# 1) 库：编译 + 安装头文件到 {build}/agentxx-project-install
cmake --build agent\build\windows-debug\agentxx_lib_repo-prefix\src\agentxx_lib_repo-build `
  --config Debug --target agentxx_static
cmake --install agent\build\windows-debug\agentxx_lib_repo-prefix\src\agentxx_lib_repo-build --config Debug

# 2) 测试：编译 + 安装 exe 到 {build}/exec
cmake --build agent\build\windows-debug\agentxx_test_repo-prefix\src\agentxx_test_repo-build `
  --config Debug --target agentxx_test
cmake --install agent\build\windows-debug\agentxx_test_repo-prefix\src\agentxx_test_repo-build --config Debug

# 3) 跑
cd agent\build\windows-debug\exec
.\agentxx_test.exe --log-level error
```

## 待完成 / 后续

1. **新场景才能量到窗口收益**：benchmark 模块已修到可编可跑（见上一节），但现有资源场景全部
   `enableSessionStore = false`（`bench_resource.cpp:138` / `:619`，`bench_resource_real.cpp:443` /
   `:1470`），没有会话库就 `cap = 0`、不淘汰，窗口根本不会生效 —— 直接跑 `resource` 得到的
   `viewCount`/`viewBytes` 仍是全量，无法反映本次改动。要量出收益需要新增一个
   “开库 + N 条历史 + 重启加载”的场景（对比 `session.view_window_messages` = 400 与 0）。
   release 侧已就绪（已编过 `agentxx_static` 并 install、编过 `agentxx_benchmark`），
   补场景后一条命令即可跑：
   ```powershell
   cd agent\build\windows-release\exec
   .\agentxx_benchmark.exe resource
   ```
2. **页读是同步的**：`collectViewRange` 在 io 线程上直接查库（与写库共用
   `SessionStore` 互斥锁）。实测单页 100 条很快，但库被长事务占住时会顶住 io 线程 ——
   需要时改成协程 + 线程池（计划 §7 已列该风险）。
3. **老库视觉为空的行**：读取期不再过滤，CLI 可能打印一行空行；要清理需另做一次性
   “重排 seq”维护脚本（不在本轮）。
4. `agent/third_party/cxx_pluginxx_ui` 的术语清理还在子模块工作区，需在子模块仓库单独提交。
