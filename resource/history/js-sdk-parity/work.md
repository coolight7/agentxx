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

### 5. 执行图节点类型（`registerNodeType` / `unregisterNodeType`）

原先把它归在“脚本侧不做的事”，理由是“图引擎在自身线程同步调节点执行，脚本只能在自己
的 JS 线程跑”。重新看 `plugin_graph_node.cpp` 后发现节点执行本来就是**异步操作契约**：
`run_start` 返回 op 句柄，结果经 `notify->done` 回来 —— 和能力行是同一套形状，不需要
io→JS 同步桥，于是在同一套跨线程记账上把它做出来了：

- `JsCapCall` 增加 `graphNode` 标记（节点输出不做功能埋点包装）；新增
  `JsGraphNodeType{engine, instanceKey, typeName, fnToken, dead}`（主程序把地址当
  `user_data`，与 `JsCapImpl` 一样由引擎保活到析构）。
- `startScriptCall(...)`：从 `capImplStart` 里抽出来的公共部分（建 `JsCapCall`、进在飞表、
  投递到 JS 线程），能力行与图节点共用。
- `graphNodeStart`：把 `node_name` / `config_json` / `state_json` / `thread_id` 拼成
  `{nodeName, type, config, state, threadId}`（JSON 解析不了就原样给字符串），投给脚本；
  `graphNodeCancel` 按取消收尾。
- `doCapCall` 的节点分支：空回答（`undefined`/`null`）按 `{}`（这个节点什么也不写），
  非对象回答直接以“graph node `x` must return an object”失败，不把非对象塞给图引擎。
- `registerGraphNodeRow` / `unregisterGraphNodeRow`：与能力行同样的登记/撤销与
  `rollbackActions`（脚本卸载、顶层异常都会撤），登记用**脚本实例的 host**。
- 桥：`B_REGISTER_NODE_TYPE` / `B_UNREGISTER_NODE_TYPE` + `def("registerNodeType", …, 1)` /
  `def("unregisterNodeType", …, 1)`（`configSchema` 经 `propJson` 原样交给主程序）。
- 示例 `example_js/plugin.js` 顶层登记 `js_echo_node`（返回 `{writes:[{channel:"js.out",…}]}`），
  `js_parity_probe` 增加“登记 → 同名再登记（覆盖）→ 注销 → 再注销”四项。

测试侧两处配套：`test_plugins` 的最小 `AgentContext` 补了 `graphRegistry`（不补就没有图
注册表，脚本顶层登记会被拒），并新增一段**真实建图执行** —— 用 `GraphCompiler::parse` +
`GraphEngine::link` 跑 `__start__ → js_echo → __end__`，断言写进 `js.out` 通道的
`{node:"js_echo", echo:"hello-js"}`，证明输入拼装与输出解析都真的通。

### 6. 接口级剩余差异补齐（工具选项 / 取消 / JSON / client 能力行）

对齐盘点后还差四处，本轮全部补上：

1. **工具选项**：`registerTool` 现在读 `timeoutMs` / `autoSummary` / `parallelSafe` /
   `repeatCallCheck` → 填 `spec.default_timeout_ms` 与 `spec.flags`（与 `kit::fast_tool` 同一组）；
   主程序 `PluginMount::invokeTool` 本来就会按 `default_timeout_ms` 包一层超时，所以脚本工具
   现在能真的"到点失败"。
2. **协作式取消**：`spec.execute_cancel` 不再留空。op 句柄是**自增 id 编码的指针**，
   取消路径只拿它查在飞表（`toolOps_`）、不解引用 —— 主程序的取消可能落在
   "JS 已 done、完成包还没提交"的窗口里，那里直接解引用会 UAF。脚本侧由
   `ctx.isCanceled()`（`JS_NewCFunctionData` 带本次执行 id）轮询；取消后终态为
   `canceled`，结果不再被采用。
3. **`agentxx.json` 上桥**：`jsonEscape(text)` / `jsonGetString(jsonText, key)`，与
   C++ kit 的 `jsonEscape` 一样返回**带引号**的 JSON 字符串字面量（首版测试按不带引号断言，
   已按主程序契约改正）。
4. **client 侧能力行**：`capabilityProvideValue` / `Unprovide` / `List` / `Info` /
   `Invoke` 五个 helper 从 `bridgeCall` 里抽出来，agent 与 client 两侧共用；client 实例没有
   挂载作用域，`register_capability` 收到 null scope（框架文档：空 = 进程级行），因此两侧
   同一份实现即可。`providePoint` 不上 client 侧（功能埋点是 agent 侧概念）。

测试：`plugins` 691 → 702（工具标志 3 条、超时 2 条、取消被脚本看见 1 条、JSON 2 条等），
`client_plugins` 832 → 835（能力行往返 + JSON）。取消用例是端到端行为断言：
`js_cancel_probe` 带 `timeoutMs: 120`、体内 10ms 轮询一次，主程序超时后返回 `timed out`，
随后 `js_cancel_report` 读到脚本看到的取消位置（`canceledAt >= 0`）。

## 验证

Windows Debug，`agent/build/windows-debug/exec/agentxx_test.exe`：

| 模块 | 结果 |
|---|---|
| `plugins` | 702 / 0（对齐改动 677；加执行图节点 691；本轮接口补齐 702） |
| `client_plugins` | 835 / 0（改前 829；能力行 + JSON 后 835） |
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
- 图节点类型只能在**建图之前**登记才生效（主程序按类型名编译图）：脚本要在顶层登记，
  运行期再登记只影响之后重建的图。注册表本身不提供删除类型，注销只是把槽位失活，
  已编译的引擎不受影响。
- 测试装置里没有 `graphRegistry` 时，脚本顶层调 `registerNodeType` 会被主程序以
  `ERR_UNSUPPORTED` 拒绕（`graphRegistry not ready`）；示例脚本用 try/catch 包住并记一条
  日志，避免一个可选能力让整份脚本加载失败。
- **脚本实例的建立时机决定了登记的确定性**：`attachMountScope` 把实例创建投递到 JS 线程
  就返回了挂载完成 (`notify->done`)，而 `BaseAgent::init` 的顺序是 `plugin_mount` →
  `engine_build`，两者并行。因此脚本顶层登记的节点类型有概率晚于首次建图；要严格进入首次
  建图，只能用 C++ 插件在 mount 回调里同步登记。没有改成“mount 等脚本初始化完再 ack”：
  脚本顶层注册工具会同步等 io 线程 (`ioCallSyncKeep`)，io 线程若在 mount 里等 JS 就是互等。
  该限制同样作用于脚本注册的工具/订阅（它们不在建图时被读取，所以平时看不出来）。
