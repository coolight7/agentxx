# 方案：client 侧 JS 引擎

## 1. 背景

`agentxx_javascript_engine` 是"能力 `interpreter.js` + 专用 JS 线程"的脚本宿主，
目前只有 agent 侧入口：脚本顶层跑在 agent 上，`agentxx.*` 桥全部对应 agent 表。
client 侧插件（`agentxx.client.*`）的能力因此对脚本不可见，工具特化渲染
（`register_tool_renderer`）也就无从注册。

2026-11 曾按"agent 侧登记 + wire 往返"实现过一版（`agentxx.agent.tool_render` 表 +
三条 wire 消息），评估后整体回退：渲染器注册面只在 client 侧一处，跨端登记等于把
两层注册表都留着。正解是让脚本本身能跑成 client 侧插件。

## 2. 现状调查（文件 / 函数级）

### 2.1 引擎 (`agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`)

- 生命周期状态 `JsEngine::State`（447 起）；`start()` / `requestStop()` /
  `stopAndWait()` / `finishStop()`；`jsThreadMain()`（1449）跑任务队列与定时器
- 实例：`JsPluginCtx`（379，实例键 = `<脚本名>@<作用域键>`）、
  `createInstanceOnJsThread()`（1542，新建 JSContext + `injectBridge` + `JS_Eval`）、
  `destroyInstanceOnJsThread()`（1621）/`destroyScriptInstancesOnJsThread()`（1641）/
  `destroyScopeInstancesOnJsThread()`（1659）
- 脚本登记：`loadScriptAsync()`（~800）/`unloadScriptAsync()`（~836），作用域由
  `attachMountScope`/`detachMountScope` 维护（`scopes_`）
- 桥：`injectBridge()`（2206，`def("名字", magic, 参数个数)` + `JS_SetPropertyStr(ctx,
  global, "agentxx", bridge)`）、`bridgeCall()`（2603 起，按 `BridgeMagic` 分派）
- 跨线程：`post()`（1285，非阻塞）、`postSync()`（1299，阻塞且无超时，当前仅
  `loadedToolsJson` 的任意线程版本在用）、`eventFire()`（2535，io → JS 线程投递）
- 入口符号：`agentxx_plugin_agent_{get_info,create,start,stop,mount,unmount,destroy}`
  （4417~4838）；能力注册在 `jsEngineStart()`（4604），能力处理在 `jsCapStart()`
  （4439，`op = load|unload`，参数经 `agentxx.agent.json` 表解析）
- `jsEngineStart`/`jsCapStart` 只用到**通用表**（`pluginxx.capabilities` / `log` /
  `json`），这些表 client 侧同样由 `PluginHostCore` 提供，因此这两段无需按侧改写

### 2.2 client 侧契约 (`agent/lib/include/agentxx/plugin/api/client_plugin_api.h`)

- client 表成员签名**没有** `PluginxxScopeRef` 参数（与 agent 表不同）
- `AgentxxClientUiIface`：`register_tool_renderer(host, const AgentxxToolRenderSpec*)` /
  `unregister_tool_renderer(host, tool_name)`；`update_tool_decor` / `show_toast` /
  `open_overlay` / `close_overlay`
- `AgentxxToolRenderSpec`：`tool_name` / `display_name` / `collapsed_fn` /
  `expanded_fn` / `user_data`
- `AgentxxToolRenderInput`（只读）与 `AgentxxToolCollapsedOutput{text}` /
  `AgentxxToolExpandedOutput{items_json}`：输出字符串由主程序 `free`（`hostMemoryFree`
  = `free`）
- `AgentxxClientSessionIface`（`get_client_state` / `send_user_input` /
  `request_cancel`）、`AgentxxClientWireIface`（`send_plugin_data`）、
  `AgentxxClientSelfIface`（`get_own_info` / `get_plugin_args` /
  `get_plugin_config_path` / `get_language` / `set_language`）
- 入口符号：`agentxx_plugin_client_{get_info,create,start,stop,destroy}`，
  `get_info` 返回 `AgentxxClientPluginInfo`

### 2.3 主程序侧调用路径 (`agent/lib/src/plugins/client_plugin_manager.cpp`)

- 渲染注册表：`xx_cregister_tool_renderer`（1911）→ `onClientIo` →
  `ioCallSyncKeep`（投递到 client io 线程并**阻塞等待**，无超时）
- 渲染执行：`requestToolRender`（~1040，UI 线程只读缓存，未命中投递到 io 线程）→
  `performToolRender`（1074）：先查注册表（插件优先，其次主程序内置），带
  `InflightGuard` 保护后调用 `collapsedFn`/`expandedFn`，输出字符串用
  `hostMemoryFree` 释放，结果写缓存并通知适配器重绘
- 加载：`loadNativeAsync`（418）探测 `agentxx_plugin_client_create/start/stop`；
  有 create 缺 start/stop 直接拒绝；`sides == Auto` 且无 client 入口时静默跳过
- 清单协商：`checkInterfacesForSide(..., agentSide=false)` 只比较 client 侧声明的接口；
  `requiredEntrySides()` 会把 `agentxx.client.*` 记为"需要 client 入口"
- 通用表（capabilities/log/json/plugins/config/broadcasts）：`xx_cquery_interface`
  （1707）先查 `pluginxx::queryGenericPluginIface`，因此 client 侧同样具备

### 2.4 示例插件 (`agent/plugins/example_js/`)

- `libexample_js.cpp`：`ShellCtx{host, AgentIfaces iface, name, dir, scriptPath,
  scriptLoaded, opToken}` + `agentxx_plugin_agent_*` 手写入口；经能力全名
  `plugin.agentxx_javascript_engine.interpreter.js` 调 `load` / `unload`
- `plugin.yaml`：`entry: libexample_js.so`、`depends: [agentxx_javascript_engine]`、
  `interfaces.require: [agentxx.agent.core]`
- `plugin.js`：顶层无条件 `agentxx.registerTool(...)` × 9 + 通知/事件/异步初始化

## 3. 方案

### 3.1 引擎侧（单文件）

1. `enum class Side { Agent, Client };`，`explicit JsEngine(Side side = Side::Agent)`，
   `side()` / `isClientSide()`；成员 `side_`
2. `injectBridge`：
   - 两侧都注入 `agentxx.side`（`"agent"` / `"client"`）与 `setTimeout`/`clearTimeout`
   - client 侧只注入 client 桥子集（见 3.2），不注入 agent 专有函数
3. `bridgeCall` 开头按 `isClientSide()` 转 `clientBridgeCall(ctx, argc, argv, magic, pctx)`
   （私有静态成员），agent 分支一行不改
4. client 实例：`loadScriptAsync` 在 client 侧使用固定伪作用域
   `MountScope{"client", "", 0}`（`pctx->scope()` 返回 `nullptr`）；不使用 `scopes_`
5. 渲染注册项 `JsRenderReg{engine, instanceKey, toolName, displayName,
   collapsedToken, expandedToken, dead}`，引擎持有
   `std::mutex renderMtx_` + `std::map<std::string, std::shared_ptr<JsRenderReg>> renders_`；
   JS 函数体存 `JsPluginCtx::renderCallbacks`（`map<int64_t, JSValue>`，随实例析构释放）
6. 同步桥 `callJsSync(body, timeout)`：堆上结果槽 + `post()` + `wait_for`；
   超时置 `abandoned`（任务稍后执行时直接跳过）
7. `jsHostCallDepth_`（原子计数）：`clientBridgeCall` 全程 +1，io 线程的渲染回调看到
   非零即快速失败
8. 入口：导出 `agentxx_plugin_client_get_info/create/start/stop/destroy`，
   `create` 构造 `JsEngine(Side::Client)`，`start`/`stop` 复用 `jsEngineStart`/`jsEngineStop`

### 3.2 client 桥子集（第一步）

| JS 方法 | 表成员 |
| --- | --- |
| `log(level, msg)` | `pluginxx.log` |
| `getOwnInfo()` / `getPluginArgs()` / `getPluginConfigPath()` / `getLanguage()` / `setLanguage()` | `agentxx.client.self` |
| `getClientState()` | `agentxx.client.session.get_client_state` |
| `sendUserInput(sessionId, text)` / `requestCancel(sessionId)` | `agentxx.client.session` |
| `sendPluginData(topic, json)` | `agentxx.client.wire` |
| `showToast(text, level)` | `agentxx.client.ui.show_toast` |
| `updateToolDecor(toolCallId, decor)` | `agentxx.client.ui.update_tool_decor` |
| `registerToolRenderer({tool, displayName, collapsed, expanded})` / `unregisterToolRenderer(tool)` | `agentxx.client.ui.register/unregister_tool_renderer` |
| `openOverlay({type,title,payload,extra})` / `closeOverlay()` | `agentxx.client.ui.open/close_overlay` |
| `subscribe(topic, fn)` / `unsubscribe(token)` / `publish(topic, value)` | `pluginxx.broadcasts`（无作用域） |

参数解析沿用 agent 分支的写法（`jsToCppString` / `JS_ParseJSON` / `valueToJsonString`）。

### 3.3 示例插件

- `ShellCtx` 同时持有 `AgentIfaces` 与 `ClientIfaces`、`capabilities`/`json`/`log`
  三个通用表指针，`clientSide` 标记；`create` 按入口填一侧
- 新增 `agentxx_plugin_client_get_info/create/start/stop/destroy`（手写，
  `create` 走 `ClientIfaces::query`，其余复用同一套 load/unload 逻辑）
- `plugin.yaml` 增加 `agentxx.client.msg_decor` 到 `interfaces.require`
- `plugin.js`：agent 分支保持原内容，新增 client 分支（注册 `js_hello` 的渲染器 +
  一条日志），用 `agentxx.side` 判定

### 3.4 测试

`agent/test/plugin/test_client_plugins.cpp` 新增第 30 段：
1. 先加载 `plugins/agentxx_javascript_engine`（client 侧），再加载 `plugins/example_js`
2. 断言 UI 注册表里出现 `js_hello` 的渲染项、显示名为脚本给的名字
3. 用现有 `renderToolAsync` 驱动折叠/展开内容，断言文本/items 来自脚本回调
4. 卸载后注册项消失（回退默认渲染）

回归：`plugins`（agent 侧 JS 引擎与示例插件）、`plugin_cleanup`、`boundaries`、
`config_keys`。

## 4. 边界与风险

- **io 线程被 JS 阻塞**：只在"JS 线程空闲"时同步等待，最长 150 ms；JS 线程在等主程序
  时直接失败。多数场景一次就命中缓存，不会重复等待。
- **嵌套调用**：渲染回调里再调 client 表会等 io 线程（io 线程正等 JS）→ 双方超时后
  各自返回；不给渲染回调提供"再入"保证，文档写明。
- **JS 异常 / 超时 / 引擎已停**：一律当作"没有自定义内容"，界面回退默认渲染。
- **不做旧版本兼容**：client 入口是新符号，老引擎 DLL 没有它 → client 侧按
  "无 client 入口"跳过（`sides: Auto`），不影响 agent 侧。
- **渲染值的内存**：输出字符串用 `host->vtable->alloc` 分配，与主程序
  `hostMemoryFree`（`free`）配对。

## 5. 分步实施

1. 引擎 `Side` + client 桥 + 渲染同步桥 + client 入口
2. `example_js` 壳双入口 + 清单 + 脚本分支
3. 编译插件（内层 lib 构建目录点名目标）、客户端与测试
4. `client_plugins` 第 30 段 + 回归
5. 文档（`plugins.md` §12、`AGENTS.md`）+ `work.md`

## 6. 验证

- `agentxx_test client_plugins`：新增断言全绿
- `agentxx_test plugins`：agent 侧 JS 引擎/示例插件行为不变
- `agentxx_test boundaries config_keys plugin_cleanup`：契约与文档自检
- 手工：`agentxx_test --log-level warn client_plugins plugins` 无警告

## 7. 第二批（同日追加）

第一批只做到“渲染器 + 展示类调用”，client 表里需要长期持有句柄或回调登记的部分
留在了 C++ 壳里。第二批沿用同一套桥把它们补齐，设计上没有新概念：

- **句柄**：C 表的不透明指针存不进 JS 的 number → 实例侧 `clientHandles` 记
  token → {类别, 指针, id}，`0` = 不支持或失败；类别校验后的更新/注销返回布尔值
- **回调**：`JsClientCallback`（不含 `JSValue`）与路由键一起登记；摘注册不就地释放
  （注册项作为 `user_data` 被主程序持有，可能还有在途投递）
- **两条调用路径**：命令 `execute` 走同步桥（与渲染同一 150 ms 上限）；动作/定时器/
  快捷键只投递，主程序不等结果
- **撤销**：所有 client 注册也进 `pctx->rollbackActions`，脚本顶层失败或卸载时逆序撤销

验证：`client_plugins` 817 / 0（该段两批共添 43 条断言：首批 23 + 展示项/交互 12 +
注销与查询 8，覆盖七类面）；其余插件/配置/边界模块与第一批结果一致。
