# 异步化重构审计修复 (插件接口 + FFI)

## 目标

对「插件接口 / FFI 完全异步化重构」做一次独立审计，把查出的契约漏洞修掉。审计报告列出 7 项
问题 + 3 项留意点，本任务处理其中需要动代码的 6 项与文档代次，外加服务端 `switch_session`
回包协程的异常保护。

## 范围

- FFI：`agent/lib/src/ffi/{ffi_call.h,ffi_call.cpp,ffi_runtime.cpp}`
- 服务端回包：`agent/lib/src/agent/io/session_server_agent_io.cpp`（`WireSwitchSession`）
- 插件框架 kit：`agent/third_party/cxx_pluginxx/include/pluginxx/kit/kit.h`
- 插件网络：`agent/lib/src/plugins/net_service.cpp`
- 文档/构建注释：`AGENTS.md`、`agent/CMakeLists.txt`

## 状态

**已完成（2026-11-25）**，详细实施与验证见 [work.md](work.md)。

改动文件：

- `agent/lib/src/ffi/{ffi_call.h,ffi_call.cpp,ffi_runtime.cpp}`（受理/收尾线性化、状态机 CAS、代次原子量）
- `agent/lib/src/agent/io/session_server_agent_io.cpp`（`switch_session` 回包串入 `catchErrorAsync`）
- `agent/lib/src/plugins/net_service.cpp`（实例已销毁时按取消交回完成）
- `agent/third_party/cxx_pluginxx/include/pluginxx/kit/kit.h`（句柄写回/取消判 `fired`；`spawn` 返回 bool；create 排不上立即 FAILED）
- `agent/test/test_ffi_c_api.cpp`（新增 `testStopCallRace`）
- `agent/plugins/agentxx_chatbot/chatbot_task.cpp`（取消撞上“任务刚好结束”时的文案，修全量偶发失败）
- `AGENTS.md`、`agent/CMakeLists.txt`、`docs/{zh-cn,en}/design/ffi.md`

验证：顶层 Debug 构建 EXITCODE=0（25 个插件全部重链）；定向模块 3162 / 0；全量见 work.md。

## 关键决策

| 编号 | 问题 | 修法 |
|---|---|---|
| D1 | `stop()` 与 `call()` 并发时，登记可能落在 `failAll` 之后 → 永远没有回执 | `CallRegistry::begin` 在 `stopping_` 下返回 0（与收尾判定同一把锁）；`call()` 拿到 0 直接按状态错误返回；`onRuntimeStopping` 改为**先 stop 再 failAll**，消除"快照之后登记"的窗口 |
| D2 | `onServerReady` 无条件写 `Ready`，能把 `Stopping` 复活 | 改成 `Starting -> Ready` 的 CAS |
| D3 | 网络请求在插件实例销毁后完成时直接 return，OpCore 永不终结（泄漏 + 关闭等满超时） | 改为仍然回调 `PLUGINXX_OPERATOR_CANCELED`（回调只碰 OpCore，不进入插件） |
| D4 | 表调用在 `start()` 内联完成时，kit 会把已完成的句柄写回槽位，句柄被回收后 `op_cancel` 是 UAF | 写回前判 `fired`，取消槽也先判 `fired` |
| D5 | `CallRegistry::generation_` 跨线程读写无锁 | 改成 `std::atomic<uint64_t>` |
| D6 | create 事务 `spawn` 失败时永不完成（主程序等到关闭截止时间） | `spawn`/`spawnAwaitableImpl` 返回 bool，`createInstanceEntry` 排不上时立即回 FAILED |
| D7 | `WireSwitchSession` 回包协程没有异常保护 | 提交切换 + 回执 + 重放整体走 `catchErrorAsync`，失败回 `switch_failed` 并把暂存请求放出去 |
| D8 | 代次文档停在 7/8 | `AGENTS.md` 与 `agent/CMakeLists.txt` 注释改为 8/9，补 v8 说明 |

## 结论

六处契约漏洞均按"不改变对外 ABI、只补内部判定"的方式修掉：

- 受理与收尾的先后由同一把锁线性化，`call()` 的结论只有三种：受理（有回执）/ 同步失败（无回执）/
  状态错误（无回执）；
- 完成回调一律"恰好一次"，唯一例外（实例已销毁的网络完成）也改成按取消交回；
- 插件侧句柄在完成后不再回写，取消路径先看完成标记；
- `switch_session` 回包协程抛异常时也会回一条 `switch_failed` 并把暂存请求放出去；
- `create` 事务排不上时立即回 FAILED，不再让主程序等到关闭截止时间。

验证：顶层 Debug 构建 EXITCODE=0（25 个插件全部重链），定向模块 3162 / 0，全量
**44044 passed / 0 failed**。

顺带修的一处不在审计清单内：`chatbot_task` 的取消撞上"任务刚好结束"时不再回
"取消失败"（那条文案会让全量跑偶发失败）。
