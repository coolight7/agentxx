# plugin-async-unify —— 插件侧接口彻底统一成异步

## 目标

插件调主程序的接口**只保留一种模型**：受理即返回、结果经完成回调交回；SDK 不再提供
"阻塞等结果"的入口。三种消费形态：`co_await xxx(...)`（要结果 / 要顺序点）、
`xxxSubmit(...)` 返回可等待的 ticket（先发出去、稍后等）、`xxxCallback(..., done)`
（没有协程的上下文）。为此 `start` / `mount` / `stop` / `unmount` 允许异步完成，
析构不再调接口表。

## 范围

做：

- 框架 kit：`tableOpSubmit` + `TableOpTicket`；`tableOpBlocking*` / `invoke_capability_blocking`
  先标废弃、最后删除；生命周期超时；
- 主程序 kit（`plugin_kit.h`）：全部同步包装改名为 `*Blocking` 并补 awaiter / submit / callback
  形态（广播 4、提示词与图 4、模型与自身信息 2、资源登记 3、权限 4、share store 1、
  能力行 2、运行时与后台任务 6、网络 1）；`LifecycleFinisher` / `finishLater`；
- 25 个插件按编译器报错迁移；删掉上一批临时加的 4 份私有 `netCallWait`；
  `websearch` / `rag_search` / `text_selection_monitor` 的工具体改 `polled_tool`（CPU 重活走 `offload`）；
- `boundaries` 新增两条静态检查（析构里不许调表、插件源码不许用 `*Blocking` 表调用）；
- 文档：`plugins.md` §8.0 调用模型、§22、`AGENTS.md`。

不做：

- 不改任何接口表成员（**不抬代次，停在 7**），只做 SDK 源码级破坏；
- 不动"插件提供实现、主程序调用"那一侧（工具 / 能力行 / 图节点 / 生命周期回调形态）；
- 不删 `blocking_tool`（工具体形态，与表等待无关）；
- 不碰 `isCanceled` 这类本地查询（不走接口表，保持同步）；
- client 侧三个回答型回调与 150 ms 同步桥：列为批 F，需确认后才做。

## 状态

规划完成（2026-11-17），待确认 §8 的五个决策点后从批 A 开始。

## 关键决策

1. **一个操作三种形态、命名带后缀**：`xxx` = awaiter，`xxxSubmit` = ticket，
   `xxxCallback` = 回调；旧同步名迁移期叫 `xxxBlocking`，最后整批删除。
2. **ticket 在构造时就提交**，`co_await` 只是"等已提交的那次操作"；丢弃 = fire-and-forget。
   抽屉里的句柄按 ABI 只能取消，所以"稍后再等"必须由 SDK 侧的状态机实现。
3. **顺序有两层**：执行顺序由宿主 IO 线程队列保证（FIFO，调用方已在 IO 线程时内联）；
   完成顺序只有 `co_await` 能保证。交给别的线程的操作（网络）只保证入口按序。
4. **生命周期改成异步完成**：宿主 `awaitPluginLifecycle` 早就支持 notify，本轮补超时
   （`plugin.lifecycle_timeout_ms`，默认 30 s），失败按回滚 / 收尾按记日志继续。
5. **析构不调表**：收尾搬到 `stop` / `unmount`，用静态检查兜住漏改。

## 结论

（完成后补：实际删了多少入口、迁移了多少调用点、测试与性能数据）
