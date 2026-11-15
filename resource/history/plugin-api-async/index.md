# 插件接口表异步化

## 目标

把"插件调用主程序"的接口表从"同步返回"统一成**异步操作契约**（受理即返回，
结果经回调交回），让主程序 io 线程只做受理与投递：

- 表实现里的磁盘（sqlite）、大对象序列化、目录扫描、起会话/起 agent 都不再占 io 线程；
- JS 插件的桥不再需要"JS 线程与 io 线程互等"，client 侧 150 ms 同步桥上限从根上消失；
- C++ 插件作者不必再为慢活自己套 `offload` / `polled_tool` 样板。

## 方案

见 [plan.md](plan.md)。要点：

- 逐表逐成员的结论清单（改异步 ≈45 / 保持同步 ≈55 / 已是异步 ≈12 / 明确不可 ≈14）；
- 目标 ABI：`AgentxxPluginOpResult{status, payload}` + `AgentxxPluginOpCallback`，
  成员签名平铺参数 + 表尾追加 `cb/ud/error_out`（返回 `NULL` = 未受理）；
- 顺序语义：同实例按受理顺序执行（FIFO），不提供并行；
- 版本策略：全局 `AGENTXX_PLUGIN_API_VERSION` 5 → 6，受影响表保留 IID、`VERSION` → 2、
  `BASELINE_SIZE` 重置（未发布 + 明确不兼容旧插件）；
- kit 三形态：`co_await` / 回调 / 阻塞（仅非 io 线程）；
- 分阶段 A（骨架 + `get_share_store`/`add_share_store` 试点）→ B 业务批 1 →
  C runtime 批 → D client 批 → E 收尾删同步桥。

## 范围

做:
- 新 ABI 类型与全局版本抬升；
- 逐表成员改造（按 [plan.md](plan.md) §5 的四张清单）；
- 插件 SDK kit 的三种消费形态；
- JS 桥逐表改成 Promise（删掉对应位置的 `ioCallSyncKeep`）；
- 12 个内置插件与示例按需迁移；
- `boundaries` 的成员顺序期望表同步；
- `KeyMetrics` 增加表调用耗时（`dev_mode` 才收集），加载快照输出。

不做:
- 不改"插件提供实现、主程序调用"那一侧（工具/能力行/图节点/生命周期已是 notify 契约）；
- 不做同一实例并行执行表调用；
- 不为旧签名保留兼容成员；
- 不改 `log` / `json.*` / `broadcasts.*` / `is_io_thread` / `op_cancel` / 记账类成员。

## 状态

规划中（2026-11）。方案已写入 [plan.md](plan.md)，实施未开始。

前置: 无。后续依赖它的任务: [js-runtime-singleton](../js-runtime-singleton/index.md)
（JS 运行时单例重构建议在它之后做，可少写一半同步桥过渡代码）。

## 关键决策

1. **异步 ≠ 并行**：异步化只改"入口返回时机"，同实例仍按受理顺序执行，注册类调用的
   顺序语义不因异步化改变。
2. **错误分层**：参数/作用域/状态类错误在入口同步返回（`NULL` + `error_out`），
   执行期错误经回调 `status = FAILED`，取消走 `CANCELED`。写错参数仍然立刻可见。
3. **payload 语义不变**：回调载荷就是旧出参 `out` 的内容，迁移时下游解析不用改。
4. **取消统一走 `pluginxx.cancel.op_cancel`**，不再每张表各留取消入口。
5. **高频只读成员保持同步**（`messages_count` / `is_canceled` / `get_language` /
   `is_io_thread`）：每次调用多一次 op 分配的代价不值得。
6. **版本不发明第二套 IID**：未发布阶段直接重置 version/baseline，首次发布后再改签名
   才走"换 IID"。

## 风险

- 126 个成员一次性改动：分 5 阶段，阶段 A 定模板。
- 顺序语义被误用（注册后立刻用）：成员内部按受理顺序执行 + 文档强调"注册类调用要
  `co_await` 完再继续"。

## 结论

待实施后补充。
