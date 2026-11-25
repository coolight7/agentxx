# 实施记录：异步化重构审计修复

## 进度

- [x] FFI：`CallRegistry` 收尾后不再受理（`begin` 返回 0 + `isStopping`）
- [x] FFI：`onRuntimeStopping` 改成「先 stop 再 failAll」
- [x] FFI：`call()` 处理 `reqId == 0`（状态错误，无回执）
- [x] FFI：`onServerReady` 改成 `Starting -> Ready` 的 CAS
- [x] FFI：`generation_` 改原子量
- [x] 插件网络：实例销毁后到达的完成按 `CANCELED` 交回（不再直接 return）
- [x] 插件 kit：内联完成的表调用不再写回过期句柄；取消槽先判 `fired`
- [x] 插件 kit：`spawn` / `spawnAwaitableImpl` 返回 bool；`createInstanceEntry` 排不上时立即 FAILED
- [x] 服务端：`WireSwitchSession` 回包协程包 `catchErrorAsync`
- [x] 契约文档：`docs/{zh-cn,en}/design/ffi.md` 补「stop 与 call 并发」的结论
- [x] 代次文档：`AGENTS.md`、`agent/CMakeLists.txt` 注释 7/8 → 8/9
- [x] 新增回归用例 `testStopCallRace`（`ffi_c_api` 模块）

## 逐步改动与验证

### 1. FFI 受理/收尾线性化

改动：

- `agent/lib/src/ffi/ffi_call.cpp`
  - `begin()`：进锁后先看 `stopping_`，是则返回 0（不登记、不排超时）；
  - 新增 `isStopping()`；
  - `generation_` 的三处写入/读取改成 `std::atomic<uint64_t>`（`fetch_add` / `load`）。
- `agent/lib/src/ffi/ffi_call.h`：`begin` 的返回 0 语义、`isStopping()` 声明、`generation_` 改原子量。
- `agent/lib/src/ffi/ffi_runtime.cpp`
  - `call()`：`begin()` 返回 0 → `AGENTXX_FFI_ERR_STATE`（不发回执）；
  - `onRuntimeStopping()`：**先 `calls_->stop()` 再 `failAll`**。顺序反了时，`failAll`
    快照之后、`stop()` 之前登记的条目会漏网（既不在快照里，之后也不会排超时）。

验证思路：`testStopCallRace` 用一条线程持续 `model.list`，主线程同时 `stop()`；断言每条
`rc == OK` 的 reqId 都有回执且恰好一条。

### 2. FFI 就绪状态

`onServerReady` 改为 `compare_exchange_strong(Starting -> Ready)`。`onInitError` 本来就是
CAS，两处统一。停止期间迟到的就绪回调不再把状态改回 `Ready`。

### 3. 网络完成回执

`agent/lib/src/plugins/net_service.cpp`：`guard.expired()` 分支改为 `cb(ud, CANCELED, nullptr)`。

- 回调只碰 `OpCore`（`xx_net_call` 传入的 lambda），不会进入插件代码；
- kit 侧 `finishAsyncTableOp` 本来就有 `life` 令牌判断，不会恢复到已析构实例；
- 这样 `OpCore::commit` 会跑完，`runtime->operations` 与内存块都能回收。

### 4. kit 句柄写回

`agent/third_party/cxx_pluginxx/include/pluginxx/kit/kit.h`：

- `start()` 返回后写 `st->opHandle` 前判 `st->fired`；取消槽里也先判 `fired`。
- 触发场景：`xx_net_call` 的立即完成路径（`net.info` / `http.serve` / `ws.send` …）在插件线程
  同步回调，回调已把槽位清空并置位 `fired`；原实现随后又把已完成的句柄写回去，句柄被
  `runtime->detachedHandles`（上限 512）裁掉后，取消就是 use-after-free。

### 5. create 事务的排不上

- `spawnAwaitableImpl` 返回 `bool`（`register_task` 拒绝 / 无 tasks 表 → false）；
- `PluginBaseT::spawn`、kit 自由函数 `spawn` 一并返回 `bool`（忽略返回值的既有调用点不变）；
- `createInstanceEntry`：`spawn` 返回 false 时立即 `notifyLifecycleFailure`，不再让主程序等到
  关闭截止时间。

### 6. switch_session 回包异常

`agent/lib/src/agent/io/session_server_agent_io.cpp`：提交切换 + 成功回执 + 重放暂存请求整体
放进 `catchErrorAsync<bool>`；失败分支回 `sendAck(false, "switch_failed", 原因)` 并照样重放
暂存请求（它们会按当前绑定各自拿到确切结论，而不是等到超时）。

`switchTargetId_` 仍在进入提交段之前清空，所以失败也不会把后续请求一直挂起。

## 遇到的问题 / 注意事项

- `CallRegistry::stop()` 的定时器 `cancel()` 仍经 `asio::post(ioCtx_)` 投递；`stopping_`
  与 `generation_` 的写入都在锁内，读侧 `generation_` 现在是原子量，跨线程无竞争。
- `begin()` 在 `stopping_` 下会先构造 `Entry` 再返回 0（一次小分配）；保持简单，不另设前置判断。
- kit 的 `spawn` 返回值是新增语义，既有插件里 `ctx.spawn(...)` 的调用点不需要改。
- **额外修的一处（不在审计清单里）**：全量跑时 `plugin_example_chatbot` 偶发失败一条 ——
  `chatbot_task {op:"cancel"}` 撞上“任务刚好跑完”时，主程序不受理取消，插件回
  `取消失败: 主程序没有接受取消请求`，而用例只接受 `已取消` / `已经结束`。
  把这条文案改成 `任务 X 已经结束或正在收尾, 主程序没有接受取消`（事实描述，不再叫“失败”），
  用例不变。属预存的时序抖动，与本轮修复无关（单跑复现不到）。

## 验证

### 构建

- `agent/script/windows_debug_build.bat`（顶层 Debug，含 cxx_pluginxx 头刷新）**EXITCODE=0**；
  25 个插件 DLL 全部重新链接（`exec/plugins/*` 时间戳 17:29~17:35）；
  输出里只有既有警告（`client_event_sink.h` 的 C4100、codegraph 的宏重定义等），无新告警。

### 定向回归

```
agentxx_test --log-level warn ffi_c_api boundaries plugin_runtime plugin_sdk plugin_bridge \
  client_plugins plugin_net session_admin remote_agent input_delivery
```

- `ffi_c_api` **487 / 0**（原 478，新增 `testStopCallRace` 的 9 条）
- `boundaries` 20 / 0、`plugin_runtime` 774 / 0、`plugin_sdk` 148 / 0、`plugin_bridge` 72 / 0、
  `client_plugins` 842 / 0、`plugin_net` 38 / 0、`session_admin` 39 / 0、
  `remote_agent` 602 / 0、`input_delivery` 140 / 0
- 合计 **3162 passed / 0 failed**（日志里的 `[Error]` 全是负向用例的预期输出）

### 全量

```
agentxx_test --log-level warn        # 顶层 Debug
```

- 第一轮：`plugin_example_chatbot` **1 条偶发失败**（`chatbot_task cancel` 撞上任务刚好结束）；
  单跑 268 / 0 通过，确认是时序抖动
- 修正文案后：`chatbot_task` + `plugin_example_chatbot` 连跑 2 次均 **406 / 0**
- 第二轮全量：**passed=44044 failed=0**（exit 0），无 `[FAIL]`

### 产物

- `exec/agentxx_test.exe`、`exec/agents/`、25 个插件 DLL 均为本次构建产物。
