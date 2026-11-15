# 实施记录：JS 插件 SDK 与 C++ 插件 SDK 对齐

## 进度

- [x] 逐表盘点（业务表 / 通用表 / client 表）与缺口决策，见 `plan.md` §2~§3
- [x] agent 侧：`getOwnInfo` / `getPluginConfigPath` / `checkPaths` / `filterPaths`
- [x] 两侧：`subscribe(topic, handler, {intervalMs, mode, onEnd})`
- [x] agent 侧：能力表全套（`provideCapability` / `unprovideCapability` / `providePoint` /
      `listCapabilities` / `capabilityInfo` / `invokeCapability`）
- [x] 示例脚本 `example_js/plugin.js`：agent 侧 `js_parity_probe`，client 侧订阅自检
- [x] 测试：`plugins` 模块新增 28 条断言，`client_plugins` 新增 3 条（含两轮派发自检）
- [x] 文档：`plugins.md` §12、`AGENTS.md`

## 每步改了什么

### 1. agent 侧三个小接口

`agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`

- 新增 magic `B_GET_OWN_INFO` / `B_GET_CONFIG_PATH` / `B_CHECK_PATHS` / `B_FILTER_PATHS`
  与 `def(...)` 注册（`getOwnInfo` / `getPluginConfigPath` / `checkPaths` / `filterPaths`）。
- `checkPaths`：按 512 项分批调 `permission.check_paths`，三态映射成 `"allow"`/`"deny"`/`"ask"`；
  `filterPaths` 只回允许项；主程序无权限中间件或查询失败时给 `null`（调用方按不过滤处理）。

### 2. 订阅：`options` 与流结束

- `JsHookBinding` 增加 `token`（订阅表下标）。
- `subscribeChannel` 多两个参数：`onEnd`（有就走 `subscribe_broadcast_ex`）+ `options_json`
  （`{"intervalMs":n,"mode":"delay|drop"}`，由新helper `subscribeOptionsJsonOf` 生成）。
- 新增 `JsEngine::eventEndFire` + `doEventEndFire`：流结束时调该订阅的 `onEnd(reason)`。
- client 侧 `B_SUBSCRIBE` 同步扩展；`clientSubs` 从 `map<token, 句柄>` 改成
  `map<token, {句柄, 订阅表下标}>`，退订时把订阅表里的处理器置空。

### 3. 能力行（最大的部分）

- 新增 `JsCapImpl`（一行脚本实现，主程序持有其地址）与 `JsCapCall`（一次在飞调用）。
- `capImplStart`（任意线程）：复制参数与 `notify`，建 `JsCapCall` 注册进 `capCalls_`，
  投递到 JS 线程；`capImplCancel` 按 op 句柄取消；`finishCapCall` 保证 `notify->done`
  恰好一次；引擎停止时 `drainCapOps()` 把在飞调用按取消收尾。
- `doCapCall`（JS 线程）：找实例 → 找回调 → 调 → `drivePromise` → 结果回主程序；
  `pointMode` 时按 `pointAnswer` 规范化（`{"value":…}` / `{"disable":true}` 原样）。
- `provideCapabilityRow` / `unprovideCapabilityRow`：登记/撤销一行，回调按 token 存在
  `JsPluginCtx::capCallbacks`；撤销动作进 `rollbackActions`（脚本卸载/顶层异常时清掉）。
- `invokeCapability`：建 `JsCapInvokeBridge`，按 `invoke_capability_ex`（有则用）/
  `invoke_capability` 调，完成回调投递回 JS 线程 settle 成 `{ok,status,value|error}`。
- **登记用脚本实例的 host**（`pctx->host`）而不是引擎自己的 host：首版用 `engineHost_`
  导致能力声明落到 `plugin.agentxx_javascript_engine.*` 名下，`invokeCapability` 直接报
  "capability is not declared"（已修）。

### 4. 顺带修掉的两处旧 bug

1. **实例订阅表从未初始化**：`JsPluginCtx::agents` 声明为 `JS_UNDEFINED` 且没有赋值点，
   于是 `subscribeChannel`/client 订阅的 `JS_SetPropertyUint32(agents, …)` 全是空操作 ——
   主程序订阅登记成功，回调派发时 `agents` 长度为 0，脚本的 `subscribe` / `onHook`
   处理器**永远不会执行**。修法：建 JSContext 后 `pctx->agents = JS_NewArray(ctx)`。
2. **一次派发调了同频道的所有处理器**：`doEventFire` 原先按 topic 匹配全部条目，
   同一频道有 N 个订阅时每个处理器被调 N 次（实测 `seenAfterPublish=2`）。修法：
   绑定带订阅下标，`doEventFire`/`doEventEndFire` 只处理本订阅那一条（`token<0` 时退回
   按 topic 匹配）。

## 验证

Windows Debug，`agent/build/windows-debug/exec/agentxx_test.exe`：

| 模块 | 结果 |
|---|---|
| `plugins` | 677 / 0（改前 649，新增 28 条） |
| `client_plugins` | 832 / 0（改前 829，新增 3 条含两轮派发） |
| `plugin_broadcast` | 90 / 0 |
| `plugin_point` | 68 / 0 |
| `plugin_feature` | 34 / 0 |
| `plugin_sdk` | 133 / 0 |
| `plugin_bridge` | 193 / 0 |
| `plugin_runtime` | 730 / 0 |
| `plugin_host_mount` | 146 / 0 |
| `plugin_multi_mount` | 47 / 0 |
| `plugin_multi_instance` | 80 / 0 |
| `plugin_cleanup` | 90 / 0 |
| `plugin_dispatch` | 112 / 0 |
| `plugin_resources` | 101 / 0 |
| `plugin_net` | 35 / 0 |
| `boundaries` | 19 / 0 |

`js_parity_probe` 实测返回（`plugins` 模块）：

```json
{"ownId":"example_js","configPathKind":"string","checkPathsIsArray":false,
 "checkPathDecision":null,"filterPathsIsArray":false,"filterPathsCount":-1,
 "provided":true,"invoke":{"ok":true,"status":"ok","value":{"echo":"pong","from":"js"}},
 "capInfoIsObject":true,"capProviders":1,"listCapabilitiesKind":"object",
 "unprovided":true,"invokeAfterUnprovide":{"ok":false,"status":"failed",
 "error":"invoke_capability: capability has no implementation: plugin.example_js.js.selftest"},
 "pointProvided":true,"pointProviders":1,"subscribeTokenKind":"number",
 "readAfterPublish":"object","seenAfterPublish":1,"plainSeenAfterPublish":1,
 "streamSeen":1,"streamEndReason":{"why":"probe done"}}
```

`checkPaths` 在该用例环境里是 `null`：测试装置没有加载权限中间件，属于契约里的
"查询不可用 → 不过滤"分支，断言按两种形态都接受。

## 留给后面的注意事项

- 引擎自己的日志走 `agentxx::plugin::logTo`，它的输出格式固定是
  `[插件名] exception: <msg>`（`logTo` 本是异常上报用的），所以临时诊断日志看起来像异常；
  排查时不要被字面误导。
- 能力行的 `invokeCapability` 一律异步（Promise）：不要改成 `drivePromise` 同步等，
  同引擎里 A 脚本调 B 脚本时会把 JS 线程堵在等自己。
- 脚本里的能力回调允许返回 Promise，`drivePromise` 会泵任务/定时器/job；但长时间阻塞
  会拖住同引擎其它脚本，功能埋点实现应快速返回。
