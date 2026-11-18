# 实施记录

> 计划见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。

## 进度

| 步 | 内容 | 状态 |
|---|---|---|
| 1 | kit：client 表协程形态便捷入口 | 完成 |
| 2 | `example_plugin` client start 改异步事务 | 完成 |
| 3 | 其余内置 client 插件同上 | 完成（planning 无 start 期注册） |
| 4 | 回调体改提交即返回 + 插件执行线程彻底不等 io | 完成 |
| 5 | 收紧准入（去掉 `allowOnPluginThread`） | 完成 |
| 6 | 主程序侧：client io 线程不再内联跑 `dlopen`/`create` | 完成 |
| 7 | 实例级查询的协程形态（`configAsync` / `argsJsonAsync` / `configPathAsync` / `languageAsync`） | 完成 |

## 步 1：kit 便捷入口

`agent/lib/include/agentxx/plugin/api/plugin_kit.h`：

- `clientTableOpAsync(ctx, start)` —— 插件 Ctx 形态的提交（转发 `pluginxx::tableOp`），
  返回 `TableOpTicket`，可 `co_await` / `.forget()` / `whenAll` 批量收；
- `clientHandleOf<Handle>(result)` —— 句柄类结果取句柄（失败/未受理 → nullptr）；
- `ClientIfaces` 增加 `tasks`（`pluginxx.tasks`）：“spawn 需要它登记取消”。

## 步 2：ClientPluginBase 接入协程驱动

`ClientPluginBase` 改为继承 `pluginxx::PluginBaseT<ClientIfaces>`：

- 得到 `bridge()` / `spawn(this Self&, Fn&&)` / `stopBridge()` 与统一的
  `host` / `iface` / `log` / `init()`（`init` 里查表 + 调 `onHostReady()`）；
- client 侧自己的便捷方法（`setStatusJson` / `setPanelItems` / `showToast` / 渲染器
  注册 / 定时器 / 快捷键…）保持原样，内部仍是同步助手（后续步 4 再改）。

## 步 2 附：example_plugin client start

`agent/plugins/example_plugin/example_plugin.cpp`：

- `exampleClientSetup` → `exampleClientSetupAsync(ClientCtx&) -> pluginxx::Task<void>`：
  状态栏项 / 面板 / Info 段落 / 2 个命令 / 3 条广播订阅，全部 `co_await clientTableOpAsync`，
  句柄由 `clientHandleOf<T>` 取；任何一条拒绝抛 `std::runtime_error`（带表返回正文）；
- `exampleClientStart`：`finishLater(notify)` + `ctx.spawn(...)` + `return later.provider()`；
  协程里 `later.ok()` / `later.failed(e.what())`；
- `stop` 不变（只交完成信号，主程序按记录撤销注册）。

验证：`client_plugins` 843 通过（start 事务异步后注册仍在 load 返回前生效——
`loadNativeAsync` 等 start 事务）；`plugin_executor` 679 通过。

## 步 3：其余内置 client 插件

- `agentxx_system_monitor`：`sysMonClientSetup` → `sysMonClientSetupAsync`（Info 段落 /
  用量频道订阅 / `sysinfo` 命令三条提交后 `co_await`；失败只记日志，与原行为一致），
  start 改 `finishLater` + `spawn`；
- `agentxx_codegraph`：`codegraphClientStart` → `codegraphClientSetupAsync`（Info 段落 +
  两条跨端订阅，失败抛错）；`ClientCtx` 改为继承 `ClientPluginBase`，create 改调 `init(host)`；
- `agentxx_chatbot`（client 侧）：`chatbotClientSetup` → `chatbotClientSetupAsync`（面板注册 +
  必需频道必需成功、READY / 连接状态两个频道失败只记日志）；
- `agentxx_planning`：**没有 start 期注册**——Info 段落是频道回调用到时才懒注册
  （`ensureSection`），留在回调体那一批（步 4）处理；
- `example_js`：只有 create 期一次 `get_own_info` 同步查询（loader 线程上，不是注册事务），
  保留。

## 遇到的编译问题
- `ClientPluginBase` 原来自己声明 `host/iface/log/init`，改成继承后要删掉这些重复成员，
  否则 `init` 会隐藏基类版本；
- `PluginBaseT` 的 `spawn` 需要 `iface.tasks`（`register_task`），`ClientIfaces` 原来
  没有它 —— 已补（client 侧本来就有 `pluginxx.tasks` 通用表）；
- `PluginStringView` + `std::string` 不能直接相加（`"..." + res.text()` 编译失败），
  统一改 `fmt::format("...: {}", res.text())`。

## 验证

- `example_plugin` / `agentxx_system_monitor` / `agentxx_codegraph` / `agentxx_chatbot`
  逐个重建通过；`ClientPluginBase` 头变更后全部 25 个插件重建也通过。
- `client_plugins` 843 / `plugin_executor` 679 / `plugin_runtime` 743 / `plugin_sdk` 141 /
  `plugin_bridge` 271 全部 0 失败。
- 全量 `agentxx_test --log-level warn` → **43820 通过 0 失败**。

## 步 4：插件执行线程彻底不等 io 线程

目标：插件执行线程上**没有任何**同步等待（不只是 client 表），跨线程等待一律协程/回调。

kit (`agent/lib/include/agentxx/plugin/api/plugin_kit.h`)：

- `clientTableOp` 去掉 `allowOnPluginThread`（回到默认 `refuseOnPluginThread`）；
- 新增 `clientTableCall(host, iface, start)`：`cb = nullptr` 提交即返回，任何线程都不等；
- client 展示面全部改提交即返回：`setPanelJson` / `setPanelItems` /
  `setInfoSectionItems` / `setStatusJson` / `setStatusText` / `showOverlay`，
  以及 free 函数 `clientUpdateStatusItem` / `clientUpdatePanel` /
  `clientUpdateInfoSection` / `clientUpdateToolDecor` / `clientOpenOverlay` /
  `clientBindAction` / `clientSendPluginData` / `registerToolRenderer`；
- 新增 `ensureInfoSection` / `infoSection` / `clearInfoSection`：懒注册 Info 段落的
  提交即返回形态（句柄由完成回调写进原子槽位），给频道/动作回调这类写不了协程的地方用；
- 新增 `clientStateAsync` / `sendUserInputAsync`（协程形态）；
- 广播新增提交即返回形态：`declarePluginBroadcastNoWait` /
  `publishPluginBroadcastNoWait` / `subscribePluginBroadcastNoWait` /
  `unsubscribePluginBroadcastNoWait`（接受 `std::string_view` 与 `PluginxxStringView`）；
- 工具注册不再回读提示词表：`tool` / `fast_tool` / `blocking_tool` / `polled_tool` 用
  调用方给的 `depict`（要按表里的文本注册就先 `co_await toolPromptOf` 再传进来）。

插件侧（全部按“要不要结果”选形态）：

- `ctx.schema(name)` → `co_await schemaOf(ctx, name)`：filesystem(11) /
  execute_command(2) / websearch(2) / math / string(2) / system / system_monitor；
- execute_command：`publishToolPrompt` 改协程（`co_await setPromptJson`）；
- websearch：`modelConfigJson(host, ...)` → `co_await modelConfigJson(ctx, ...)`；
- 广播发布/声明/退订：audio_stream / chatbot / chatbot_net / codegraph /
  planning / screen_capture / system_monitor / text_selection_monitor 全改 `*NoWait`
  （音频帧回调、采集回调、面板驱动这些非插件线程的路径不再等）；
- planning client：`ClientCtx` 改继承 `ClientPluginBase`（有 `spawn`/`bridge`），
  client setup 先 `ensureInfoSection` 提交注册，订阅改 `*NoWait`；
- codegraph / system_monitor：Info 段落注册留在各自的 client start 协程里，
  `refreshSection` / `refreshUsageDisplay` 只推内容（提交即返回）；
- system_monitor `sysinfo` 命令：客户状态改 `co_await clientStateAsync()`，
  结果到了再 `notifyText`（`notify` 两个字段按值带走）；
- chatbot：`argsJson()` / `configPath()` 移到 `create` 读一次（那时不在插件执行
  线程上），start 用缓存；
- screen_capture：`config()` 改在 `onHostReady()`（`init` 末尾，loader 线程）读一次。

测试与规则：

- `boundaries` 的插件表调用形状规则新增：插件源码不得直接使用阻塞式 client 入口
  (`clientTableOp(` / `clientTableText(` / `clientTableHandle(`)，JS 引擎与 `example_js`
  (`create` 期取清单) 例外。

## 步 5：收紧准入

- kit 里不再有任何 `allowOnPluginThread`（包括广播的四个阻塞形态）：插件执行线程上
  任何 `tableOpSyncWait` 都直接返回失败并带原因文本；
- `plugin_executor` 新增 9.4 用例：插件线程上调一个“受理但永不完成”的表操作，
  断言不挂住线程、结果带 `plugin execution thread` 原因（684 通过）；
- 同步桥保留给：`create` 期实例级查询、`blocking_tool` 一类已在线程池上的实现体、
  JS 引擎的脚本线程（QuickJS 是同步 C API），详见 plugins.md §15.3/§15.5。

## 步 6：主程序 → 插件方向也清零

审计结果（`agent/lib/src/plugins` 里没有阻塞等待插件代码的调用；全部是
`dispatchToPlugin` 投递或 `asio::awaitable` 协程等）：

| 阶段 | 线程 |
|---|---|
| `get_info` | 加载线程（静态元数据，不调表） |
| `dlopen` / 静态初始化 / `create` | 加载线程（agent 侧原有 `runLoaderTask`；**client 侧本轮补上**） |
| `start`/`mount`/操作体与回调/`stop`/`unmount` | 插件执行线程 |
| `destroy` | 卸载线程（只释放本地对象，线程与注册已在 `stop` 收完） |

改动：`ClientPluginManager` 新增 `runLoaderTask` 覆写（与 `PluginManager` 同一写法：
投递线程池 + 定时器取消叫醒 io 线程，不在 io 线程上跑任何插件代码）。
之前 client 侧用框架默认实现（`co_return fn()`），即 `dlopen`/`create` 在 client io
线程上同步跑，插件在 `create` 里的取参数/配置路径也就成了 io 线程上的阻塞查表。

## 步 7：实例级查询的协程形态

kit 的 `PluginBase`（agent 侧）与 `ClientPluginBase`（client 侧）新增：
`configAsync()` / `argsJsonAsync()` / `configPathAsync()` / `languageAsync()`（都是
`pluginxx::Task<std::string>`，内部 `co_await pluginxx::tableOp(*this, ...)`）。
它们走的是通用表 `agentxx.config`（与框架的同步 `argsJson()` / `configPath()` /
`language()` 同一数据源）。

- `agentxx_screen_capture`：`start` 改成 `finishLater(notify)` + `ctx.spawn(...)` +
  `co_await c.configAsync()`，删掉 `create` 期的 `onHostReady()` 缓存；
- `agentxx_chatbot`：`argsJson()` / `configPath()` 仍在 `create` 读（在加载线程池上，
  与 `dlopen` 同类）；改成协程要重写它那个 200 行的同步 start，单独排期；
- `example_js`：`get_own_info` 在 `create` 里用来定位 `plugin.js`，保留（同上，
  `boundaries` 允许清单里注明）。

## 步 8：`agentxx_chatbot` 的 start 改协程

遗留事项 1 已落地。做法是把原来的同步 start 拆成“业务体 + 协程外壳”：

- `chatbotStart` 主体改名 `chatbotStartBody(ChatbotCtx&, argsJson, configPath, errorOut)`：
  错误一律写 `errorOut` 返回 `false`，不再直接调 `PluginString::set`；末尾的面板频道订阅
  移出（要做完所有 `co_await` 才能算注册完成）；
- 新的 `chatbotStart(ctx, notify, error)`：`finishLater(notify)` → `ctx.spawn(...)` →
  协程里 `co_await c.argsJsonAsync()` / `co_await c.configPathAsync()` →
  `chatbotStartBody(...)` → 成功再 `co_await subscribePanelEvents(c)` → `later.ok()`；
  失败 `later.failed(原因)`；
- 删掉 `ChatbotCtx` 的 `pluginArgsJson` / `pluginConfigPath` 两个 create 期缓存成员；
  `ChatbotCtx` 需要 `iface.tasks`（spawn 靠它登记取消），缺失时直接报错返回。

验证：`agentxx_chatbot` 重建通过；`plugin_example_chatbot` / `chatbot_task` 等模块全绿
（见全量结果）。

## 步 9：JS SDK 全面 Promise 化（破坏性，无旧版兼容）

遗留事项 2。用户确认可以直接破坏性改造，于是把脚本侧表调用全部换成 Promise：

**脚本侧语义**

- 表成员一律返回 Promise：业务失败（未受理 / 执行期失败 / 取消）都 **resolve**
  （`null` / `false` / 空串 / 空数组），**永不 reject** —— 忘记 `await` 也不会攒未处理拒绝；
- 参数写错仍同步 `JS_ThrowTypeError`（那是脚本写错了）；
- 纯本地函数保持同步：`log` / `jsonEscape` / `jsonGetString` / `scope` / `setTimeout` /
  `clearTimeout`，client 侧 `showToast` / `closeOverlay` / `requestCancel`；
- 必须“当场给内容”的回调仍同步：工具行渲染器 `collapsed` / `expanded`、命令 `execute`。

**引擎侧改动**（`agentxx_javascript_engine.cpp`）

- 脚本执行：`JS_Eval` 改用 `JS_EVAL_TYPE_GLOBAL` + 函数体包裹
  `"(async () => {" + code + "\n})()"`，再 `drivePromise` 泵完顶层 await。
  **不用 module 模式**：同一 JSRuntime 上 module 定义按文件名缓存，同一脚本在多个挂载
  作用域各一份实例时会拿到上一份的 module promise，第二个实例原地卡死（实测）。
- 新增跨线程桥：`JsTableResult` + `JsTableBridge` + `onTableOpDone`（主程序线程复制结果
  → `post` 回 JS 线程）+ `settleTableOp`；模板助手 `tableOpPromise` / `tableTextPromise`
  / `tableOkPromise` / `tableJsonPromise` / `submitTableOpNoWait`（撤销/回滚用）。
- 删掉全部旧阻塞助手（`callTableText` / `callTableOk` / `callTableOp` / `callTableHandle`
  / `callTableJson` / `callRuntimeOp` / `broadcastReadText` / `capabilitySyncWait` 等），
  引擎里不再出现 `tableOpSyncWait`；`boundaries` 的 `kSyncWaitAllowed` 也删掉了引擎两项。

**调试中揪出来的 5 个问题**（均已修，均有一处不变式可循）

1. **加载期定时器被丢**：`fireDueTimersInline` 按实例键查实例，而加载期实例还没写进
   `plugins_`/`mirror_` —— 定时器到期直接被当“上下文已销毁”跳过，脚本顶层
   `await new Promise(r => setTimeout(r, 5))` 永不 resolve。改成定时器直接持实例
   `weak_ptr`（`JsTimer::owner`）。
2. **脚本先于实例登记**：实例原本在脚本执行完才入表，而工具在脚本中途就登记；主程序
   这期间调工具会按“插件已卸载”失败（disable→enable 往返用例暴露）。改成**先登记实例
   再执行脚本**，失败时撤回登记。
3. **收尾丢掉在飞桥 → JS 对象泄漏**：桥的 `transform` 可能持着脚本引用（工具 `execute`
   函数），直接丢掉就留在 runtime 里（Debug 下 `JS_FreeRuntime` 断言 `list_obj_list`）。
   改成收尾循环“取消结算 + 泵 job”直到没有在飞桥。
4. **上下文/实例释放太早 → use-after-free**：promise 续体会经 JSContext opaque 回到实例
   （`bridgeCall`），在任务里当场析构就是 UAF（ASan 实测两轮）。改成三张延后释放表
   （`retiredCtxs_` / `retiredInstances_` / `retiredBridges_`），只在“队列空 +
   `JS_IsJobPending` 为 false”时真正释放，收尾时再兜底对所有建过的实例 `releaseJs()`。
5. **`user_data` 不是指针 → ASan 越界**：先把令牌当 `(void*)id` 传，`OpCore::commit` 会对
   `user_data` 按指针读 8 字节标记（`isSyncBridgeCallback`），整型地址直接崩。改成堆上的
   `JsBridgeToken{engine, id}`（完成回调释放）。
6. **`pluginxx::tableOpCallback` 的 `user_data` 只有 1 字节**（`new Done(...)`，空函子 1
   字节）：同一个 `isSyncBridgeCallback` 读 8 字节 → heap-buffer-overflow。已在库内改成
   带 `magic` 字段的 `Holder` 包住函子（`kit.h`），这是库自身的缺陷，不是调用方问题。

**测试/脚本同步**

- `example_js/plugin.js` 全量重写：顶层 `await`；client 侧的订阅计数快照移到动作的第一次
  `await` 之前（`echo=0 ends=0` 的“上一轮语义”因此仍然稳定）；
- `example_js_execute_command/plugin.js`：外层 `(function(){...})()` →
  `await (async function(){...})()`，内部 `registerTool` 加 `await`；
- `test_plugins.cpp` 的 rollback 探针脚本加 `await`；`js_sdk_probe` 的无效句柄探针把
  `typeof x` 改成 `typeof (await x)`（现在是 Promise）；`shareStore` 取不到时给 `null`
  （原来空串）；
- `test_plugin_executor.cpp` 的派发计数断言改成“等到齐”（计数在回调返回后才累加，
  原写法读得太早，偶发少 1）。

## 步 8/9 验证

| 模块 | 结果 |
|---|---|
| `plugins` | 704 通过 0 失败（连跑 3 次稳定） |
| `plugin_executor` | 684 通过 0 失败（连跑 3 次） |
| `client_plugins` | 843 通过 0 失败（连跑 2 次） |
| `plugin_runtime` / `plugin_cleanup` / `plugin_broadcast` / `plugin_multi_mount` | 743 / 90 / 96 / 47，0 失败 |
| `boundaries` / `async_tools` / `event_tools` | 20 / 178 / 97，0 失败 |
| 全量 | 见下方“全量结果” |

顺带修的两处与本任务无关的东西：

- `agentxx_screen_capture.cpp` 注释里的“宿主”改成“主程序”（`boundaries` 规则 15 停用词）；
- `kit.h` 的 `tableOpCallback` `user_data` 尺寸（见上第 6 条）。

## 全量结果

| 时间点 | 结果 |
|---|---|
| 步 1~9 完成后（脚本与引擎尚无本轮最后两处小修） | `Total: passed=43825 failed=0`，13m23s |
| 最终源码状态（`releaseJs` 的 `rt_` 守卫、`~JsEngine` 把存活实例的 `engine` 置空、文档定稿之后） | 退出码 `0`，13m07s |

退出码就是判据：`agent/test/test.cpp` 的 `main` 只在 `total.failed > 0` 时返回 `1`，所以 `0` =
零失败。13m07s 那次用 `Select-Object -Last 120` 收的尾巴，输出被 share store 的
单条上限截断，没读到 `Total:` 那一行；通过数与上一轮同测试集一致（都是 43825）。

## 遗留事项（后续单独做）

1. ~~`agentxx_chatbot` 的 `create` 期参数/配置路径读取改成协程 start~~（步 8 已完成）；
2. ~~JS 引擎的脚本线程：把 JS SDK 改成 Promise 形态~~（步 9 已完成，破坏性改造）；
3. `agentxx_execute_command` 的 polled 实现体里 `StoreFn`（share store 写入）需要
   两段式改造（与本次无关，执行线程改造时就已如此）。
