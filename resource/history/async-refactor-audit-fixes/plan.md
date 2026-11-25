# 方案：异步化重构审计修复

## 背景

`ffi-async-refactor`（2026-11-25）与 `plugin-api-rebuild` / `coroutine-unify`（2026-11）把 FFI 与
插件接口改成了「提交 + 回执 / 异步操作契约」。审计发现 6 处内部判定与 1 处回包协程缺保护，
都是在「并发时序」或「失败分支」上破坏「恰好一次回执/完成」的约定。

## 现状调查（文件与函数）

| # | 位置 | 现状 |
|---|---|---|
| 1 | `FfiAgentRuntime::call` / `CallRegistry::begin` | `acceptsCalls()` 前置检查后还有 JSON 解析等；`begin` 不看 `stopping_` |
| 1b | `FfiAgentRuntime::onRuntimeStopping` | 先 `failAll`（快照）后 `stop`，两者之间的登记会漏网 |
| 2 | `FfiAgentRuntime::start` 的 `onServerReady` | 无条件 `store(Ready)` |
| 3 | `PluginNetService::callAsync` 的 `done` | `guard.expired()` 直接 return，不回调 |
| 4 | `AsyncTableOpInitiation::operator()` | `start` 返回后无条件写 `st->opHandle` |
| 5 | `CallRegistry::generation_` | `stop()`/析构在锁内写，`armTimerLocked` 在 io 线程无锁写 |
| 6 | `createInstanceEntry` | `spawn` 失败（`register_task` 拒绝）时完成通知永不发出 |
| 7 | `session_server_agent_io.cpp` 的 `WireSwitchSession` 分支 | 回包协程没有 `catchErrorAsync` |

## 方案

1. **受理线性化**：`begin` 与收尾判定放进同一把锁（`stopping_` 下返回 0）；`onRuntimeStopping`
   先 `stop()` 再 `failAll()`。`call()` 对 0 返回 `ERR_STATE`。
2. **状态机**：`onServerReady` 用 CAS，只允许 `Starting -> Ready`。
3. **完成契约**：网络完成在实例已销毁时仍回调 `CANCELED`（回调路径只碰 OpCore）。
4. **句柄有效期**：写回与取消都以 `fired` 为准。
5. **原子量**：`generation_` 改 `std::atomic<uint64_t>`。
6. **create 排不上**：`spawn` 返回 bool，`createInstanceEntry` 立即回 FAILED。
7. **回包异常**：提交段整体 `catchErrorAsync`，失败回 `switch_failed` 并重放暂存请求。

## 边界与兼容

- 不改任何 C ABI / 表布局；`spawn` 返回值是新增语义，既有调用点不受影响。
- 旧行为里「受理后可能永远没有回执」的路径被消除；调用方只用 `rc` 判断是否受理。
- `EVT_STOPPED` 仍是最后一个事件；收尾线程上发出的排队回执仍在它之前。

## 验证

- Windows Debug 顶层构建通过（含 cxx_pluginxx 头刷新 + 全部插件重新链接）。
- `agentxx_test ffi_c_api plugin_runtime plugin_sdk plugin_bridge client_plugins boundaries`。
- 新增 `testStopCallRace` 覆盖「stop 与 call 并发」。
- 收尾跑全量 `agentxx_test --log-level warn`。

## 风险

- 取消槽判 `fired` 会漏掉「完成回调已投递但尚未执行」的窗口：那时 `opHandle` 已被清空，
  本来也拿不到句柄，行为不变。
- 网络完成按取消交回后，kit 侧不再恢复协程（实例已析构），只是把 OpCore 记账收尾。
