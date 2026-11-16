# 插件接口表异步化

## 目标

把"插件调用主程序"的接口表从"同步返回"统一成**异步操作契约**（受理即返回，
结果经回调交回）：

- 表实现里的磁盘（sqlite）、大对象序列化、目录扫描、起会话/起 agent 都不再占 io 线程；
- **插件的线程也不再阻塞等待另一条线程** —— 跨执行体往返与实现快慢无关，连记账类成员
  也改异步（只留 10 项能在调用方线程当场算完的）；
- JS 插件的桥不再需要"JS 线程与 io 线程互等"，client 侧 150 ms 同步桥上限从根上消失；
- C++ 插件作者不必再为慢活自己套 `offload` / `polled_tool` 样板。

## 方案

见 [plan.md](plan.md)。要点：

- 逐表逐成员的结论清单（[plan.md](plan.md) §5）：**125 个成员里改异步 104 / 已是异步 6 /
  保持同步只有 10 项 / 删除或合并 5 项** —— 判据从"实现重不重"换成"调用方线程要不要等
  另一条线程"（§5.0）；保留同步的 10 项列在 §5.5（纯函数 / 线程本地判定 /
  本地原子标记 / 无结果投递 / 返回票不等待）；
- **槽位清理**（§5.6 / §5.7）：删 `scheduler.post_to_io`（它自己就是一次阻塞往返，改用
  `request_driver`）、把 `coroutine_runtime` **整张表并回 `scheduler`**（通用表 10 → 9）、
  三处 `op_cancel` 合并为 `cancel.op_cancel`；`scheduler.is_io_thread` 保留一份，
  职责从"要不要投递"变成"能不能阻塞"；
- **回调函数单独盘点**（§6）：主程序调插件的回调里只有 3 个属于"回答型"（工具折叠内容、
  展开内容、斜杠命令 `execute`）要改异步，其余要么已是操作契约（工具/图节点/能力行/
  生命周期/任务），要么是通知型（动作/定时器/快捷键/广播/通道/后台任务事件）；
  新契约保留"回调内部立刻 `notify->done` 就当同步完成"的快路径，快插件行为不变；
- **入口符号 `get_info`/`create`/`destroy` 保持同步**（§6.5）：契约内没有可等待的工作，
  `create` 只改执行线程（agent 侧 `dlopen` + `create` 一并挪到加载线程池，客户端已经如此），
  `destroy` 出现在不能等待的回滚与析构路径上；
- **命名**（§7.4）：异步是默认契约，成员名不再带 `async` —— `call_tool_async` → `call_tool`、
  `net.call_async` 合并进异步化后的 `net.call`、kit/JS 的 `netCallAsync` 同理；
  `*Blocking` 与后台任务域（`agentxx_async_*` / `submit_async_work` / `async_work.*`）不动；
- 目标 ABI：**完成回调直接用框架已有的 `PluginxxOperatorCallback`**（不新增双胞胎类型），
  成员签名平铺参数 + 表尾追加 `cb/ud/error_out`（返回 `NULL` = 未受理）；**`cb` 可为 `NULL`**
  —— 不关心结果时只出去不回程，与 fire-and-forget 等价；
- 顺序语义：同实例按受理顺序执行（FIFO），不提供并行；
- 版本策略：全局 `AGENTXX_PLUGIN_API_VERSION` 5 → 6，受影响表保留 IID、`VERSION` 各自 +1、
  `BASELINE_SIZE` 重置（未发布 + 明确不兼容旧插件）；
- kit 三形态：`co_await` / 回调 / 阻塞（仅非 io 线程）；
- 分阶段 A（骨架 + `get_share_store`/`add_share_store` 试点）→ A2（加载线程）→
  B 业务批 → C runtime 批 → C2 框架表批 → D client 批 → E 收尾删同步桥。

## 范围

做:
- 全局版本抬升（回调用框架已有的 `PluginxxOperatorCallback`，不新增类型）；
- 逐表成员改造（按 [plan.md](plan.md) §5 的四张清单 + §5.5 的 10 项例外）；
- 3 个回答型回调改异步（[plan.md](plan.md) §6：工具折叠/展开渲染、命令 `execute`），
  连带删除 client 侧 150 ms 同步桥；
- 命名去 `async` 后缀（[plan.md](plan.md) §7.4）与 `create` 执行线程调整（§6.5）；
- 插件 SDK kit 的三种消费形态；
- JS 桥逐表改成 Promise（删掉对应位置的 `ioCallSyncKeep`）；
- 17 个内置插件与 9 个示例按需迁移（4 个网络插件随 C2 批一起改）；
- `boundaries` 的成员顺序期望表同步；
- `KeyMetrics` 增加表调用耗时（`dev_mode` 才收集），加载快照输出。

不做:
- 不改"插件提供实现、主程序调用"那一侧（工具/能力行/图节点/生命周期已是 notify 契约）；
- 不做同一实例并行执行表调用；
- 不为旧签名保留兼容成员；
- 不改 §5.5 那 10 项（纯函数 / 线程本地判定 / 本地原子标记 / 无结果投递）；
- 不新增 `AgentxxPluginOpResult` / `AgentxxPluginOpCallback` 这类双胞胎类型。

## 状态

实施中（2026-11）：阶段 A（骨架 + `session` 试点）、A2（加载线程）、B（`agentxx.agent`
九张业务表）、C（`runtime` 21 个成员）、C2 的第一批（`config` v4 / `plugins` v2）、
第二批（`broadcasts` v4）、第三批（`capabilities` v8）已完成，进度与验证见 [work.md](work.md)。
剩下 C2 最后一项（`net.call` 合并 `call_async`）、D（`agentxx.client.*` 六张 + 三个回答型回调）、
E（删同步路径与 client 侧 150 ms 同步桥）。全局 API 代次实际是 **6 → 7**（第 6 代已被
"工具结果两段上报"占用）。

前置: 无。后续依赖它的任务: [js-runtime-singleton](../js-runtime-singleton/index.md)
（JS 运行时单例重构建议在它之后做，可少写一半同步桥过渡代码）与
[plugin-executor](../plugin-executor/index.md)（插件独立线程；表没异步化之前搬线程只会更慢）。

## 关键决策

1. **判据是"要不要等另一条线程"**：一次跨线程往返 = 两次投递 + 两次线程唤醒，
   与成员本体快慢无关；所以连记账类成员也改异步，只留能在调用方线程当场算完的 10 项。
2. **异步 ≠ 并行**：异步化只改"入口返回时机"，同实例仍按受理顺序执行，注册类调用的
   顺序语义不因异步化改变。
3. **错误分层**：参数/作用域/状态类错误在入口同步返回（`NULL` + `error_out`），
   执行期错误经回调 `status = FAILED`，取消走 `CANCELED`。写错参数仍然立刻可见。
4. **payload 语义不变**：回调载荷就是旧出参 `out` 的内容，迁移时下游解析不用改。
5. **`cb` 可空**：不关心结果的成员传 `NULL`，只出去不回程（与 fire-and-forget 等价）；
   高频值（`get_language` / `is_visible`）由插件自己在 `start`/`mount` 里取一次缓存。
6. **回调按"是否要回答"分类**：只有调用方必须当场拿到返回值的 3 个回调改异步，
   通知型回调（void 投递）一律不动 —— 它们本来就不等结果，改了只多一次往返。
7. **回答型回调保留同步快路径**：回调内部立刻 `notify->done` 时当帧就用这份内容，
   所以现有 C++ 插件的渲染/命令行为、缓存命中与刷新次数与改之前完全一致。
8. **"异步"只标在真正阻塞的形态上**：默认异步、名字不带 `async`；只有 `*Blocking` 与
   后台任务域保留这个字（那个 async 是"放后台跑"，不是回调式异步）。
9. **版本不发明第二套 IID**：未发布阶段直接重置 version/baseline，首次发布后再改签名
   才走"换 IID"。
10. **不新增双胞胎类型**：完成回调直接用框架的 `PluginxxOperatorCallback`。

## 风险

- 125 个成员里 104 个要改签名，同时要改 3 个回答型回调、改 2 处成员名、并掉 1 张表：
  分 7 个阶段（A/A2/B/C/C2/D/E），阶段 A 定模板。
- 顺序语义被误用（注册后立刻用）：成员内部按受理顺序执行 + 文档强调"注册类调用要
  `co_await` 完再继续"。

## 结论

待实施后补充。
