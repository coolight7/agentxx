# plugin-async-unify —— 插件侧接口彻底统一成异步

## 目标

插件调主程序的接口**只保留一种模型**：受理即返回、结果经完成回调交回；SDK 不再提供
"阻塞等结果"的入口。只剩两种形态：

- `xxx(...)` → **调用即提交**，返回 `TableOpTicket`：`co_await` 拿结果（要顺序点就 await）、
  存起来稍后再 await、`.forget()` 提交完不管；`co_await whenAll(t1, t2, …)` 等一批全部完成；
- `xxxCallback(..., done)`：没有协程的上下文（`start` / `mount` / `stop` / 通知型回调）。

为此 `start` / `mount` / `stop` / `unmount` 允许异步完成，析构不再调接口表。

## 范围

做：

- 框架 kit：`tableOpSubmit` + `TableOpTicket`；`tableOpBlocking*` / `invoke_capability_blocking`
  先标废弃、最后删除；
- 主程序 kit（`plugin_kit.h`）：全部同步包装改名为 `*Blocking` 并补 awaiter / submit / callback
  形态（广播 4、提示词与图 4、模型与自身信息 2、资源登记 3、权限 4、share store 1、
  能力行 2、运行时与后台任务 6、网络 1）；`LifecycleFinisher` / `finishLater`；
- 25 个插件按编译器报错迁移（含 `chatbot` 的 10 个工具改 `polled_tool`）；删掉上一批临时加的
  4 份私有 `netCallWait`；`websearch` / `rag_search` / `text_selection_monitor` 的工具体改
  `polled_tool`（CPU 重活走 `offload`）；
- `boundaries` 新增两条静态检查（析构里不许调表、插件源码不许用 `*Blocking` 表调用）；
- 文档：`plugins.md` §8.0 调用模型、§22、`AGENTS.md`。

不做：

- 不改任何接口表成员（**不抬 API 代次，停在 7**），只做 SDK 源码级破坏；
- **不做生命周期超时**：不同插件"正常需要多久"差别太大，定一个数只会逼插件去适配；
  谁发起异步操作，谁负责给它分自己需要的限时；
- 不动"插件提供实现、主程序调用"那一侧（工具 / 能力行 / 图节点 / 生命周期回调形态）；
- 不删 `blocking_tool`（工具体形态，与表等待无关）；
- 不碰 `isCanceled` 这类本地查询（不走接口表，保持同步）；
- client 侧三个回答型回调与 150 ms 同步桥：不在本轮，上一个任务（`plugin-api-async`）
  剩下的阶段 D/E 在本轮完成后再继续。

## 与插件独立线程重构的顺序

本轮先做：[`plugin-executor`](../plugin-executor/index.md) 要把插件代码搬到独立线程，
那之后"调用方就在 io 线程上、表操作内联执行"这条快路径消失，阻塞形态会退化成真实的
跨线程往返，并且 `stop` 时容易与 io 线程互等。所以顺序是：
`plugin-api-async`（进行中）→ 本轮（统一异步）→ `plugin-executor`（插件线程）。
本轮不引入任何"是否在 io 线程"的新判断，那套机制在插件线程落地后要删。

## 状态

已完成（2026-11-17）：批 A/B/C/D/E 全部落地，只差全量回归收尾。

## 关键决策

1. **一个操作两种形态**：`xxx(...)` 调用即提交并返回 ticket（`[[nodiscard]]`，放弃结果要写
   `.forget()`），`xxxCallback(...)` 给没有协程的上下文；不设 `xxxSubmit`。
   旧同步名迁移期叫 `xxxBlocking`，最后整批删除。
2. **ticket 在构造时就提交**，`co_await` 只是"等已提交的那次操作"；`.forget()` = 提交完不管。
   接口表句柄按 ABI 只能取消，所以"稍后再等"必须由 SDK 侧的状态机实现。
   因为提交不阻塞，`whenAll` 只需逐个 await，总耗时 = 最慢那条。
3. **顺序有两层**：执行顺序由宿主 IO 线程队列保证（FIFO，调用方已在 IO 线程时内联）；
   完成顺序只有 `co_await` 能保证。交给别的线程的操作（网络）只保证入口按序。
4. **生命周期改成异步完成、不设时限**：宿主 `awaitPluginLifecycle` 早就支持 notify；
   插件自己决定"多久算失败"（自己发起的异步操作上做限时）。
5. **析构不调表**：收尾搬到 `stop` / `unmount`，用静态检查兜住漏改。

## 结论

2026-11-17 完成。插件侧只剩两种形态，阻塞包装全删：

- **框架**: `tableOp(ctx, start)` 返回 `TableOpTicket`（调用即提交）+ `whenAll`；
  `tableOpCallback` 保留为回调形态；`tableOpBlocking(ctx, ...)`、
  `invoke_capability_blocking`、`call_tool_blocking` 删除；`tableOpBlockingRaw` 改名
  `tableOpSyncWait`，只给“必须同步回答外部同步 API”的桥用（JS 运行时插件的 QuickJS 回调）
  与 kit 内部几个同步便捷函数。
- **主程序 kit**: `tool` / `fast_tool` / `blocking_tool` / `polled_tool` / `graph_node` /
  `providePoint` / `capability` / 权限三件套 / 广播与提示词/图/资源类便捷函数全部只剩
  ticket 形态（`co_await` 拿 `TableOpResult`），新增 `toolPromptOf` / `schemaOf` /
  `subscribePluginBroadcastEx`；kit 侧 `*Blocking` 重复形态全部删除。
- **生命周期**: `start` / `mount` 的注册事务写成协程（`finishLater` + `ctx.spawn` +
  `return later.provider()`），带挂载级状态的入口返回自己的状态对象；没有
  `pluginxx.tasks` 的宿主直接报错，不再保留“同步注册回退”副本。
- **迁移范围**: 25 个内部插件；没动的只有 JS 运行时插件与两个 JS 壳（它们的桥必须同步）。
  `agentxx_audio_stream` 在当前平台不参与编译，改动与其余插件同形。
- **静态检查**: `boundaries` 新增规则 16「插件表调用形态」——已删除的四个名字回潮即失败，
  `tableOpSyncWait` 只允许出现在 JS 运行时插件与 kit 内部。

验证：`boundaries` 20 / `plugins` 704 / `plugin_host_mount` 147 / `plugin_bridge` 271 /
`plugin_runtime` 742 / `plugin_sdk` 141 / `plugin_resources` 101 / `plugin_broadcast` 96 /
`plugin_point` 68 / `plugin_feature` 34 / `plugin_cleanup` 90 / `plugin_multi_mount` 47 /
`plugin_agent_runtime` 221 / `plugin_example_runtime` 50 / `plugin_example_chatbot` 265 /
`client_plugins` 841 / `event_hub` 288 全绿（Debug + ASan）。接口表成员与 API 代次未变。
