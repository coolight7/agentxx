# 实施记录：client 侧 JS 引擎

## 进度

- [x] 现状调查（引擎 / client 契约 / 主程序调用路径 / 示例插件）
- [x] 任务卡与方案（`index.md` / `plan.md`）
- [x] 引擎：Side 分流 + client 桥子集 + 渲染同步桥 + client 入口
- [x] `example_js`：双入口壳 + 清单 + 脚本分支
- [x] 编译（插件 / 测试）
- [x] 测试：`client_plugins` 第 34 段 + 回归
- [x] 文档：`plugins.md` §12/§15.5、`index.md` 目录注释、`AGENTS.md`、示例配置注释
- [x] 第二批：展示项（状态栏/面板/Info 段落）、命令、动作绑定、客户端定时器、快捷键
      + 句柄 token + 通用同步回调桥（第七节）

## 一、引擎：`agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`

改了什么：

1. `JsEngine::Side { Agent, Client }` + `explicit JsEngine(Side = Agent)`、
   `side()` / `isClientSide()`；`clientScope()` 给出固定伪作用域
   `MountScope{"client", "", 0}`（`JsPluginCtx::scope()` 因此返回 `nullptr`）
2. `loadScriptAsync`：client 侧用伪作用域（一份脚本一个实例），agent 侧仍按 `scopes_`
3. `injectBridge`：两侧都注入 `agentxx.side`（`"agent"` / `"client"`）与裸
   `setTimeout`/`clearTimeout`；client 侧只注入 client 桥子集（agent 专有函数不出现）
4. `bridgeCall` 开头按侧分流到新的 `clientBridgeCall`；agent 分支逻辑未改（只把
   `auto* engine` 提前了一行）
5. client 桥子集：`log` / `getOwnInfo` / `getPluginArgs` / `getPluginConfigPath` /
   `getLanguage` / `setLanguage` / `getClientState` / `sendUserInput` / `requestCancel` /
   `sendPluginData` / `showToast` / `updateToolDecor` / `openOverlay` / `closeOverlay` /
   `registerToolRenderer` / `unregisterToolRenderer` / `subscribe` / `unsubscribe` /
   `publish` / `setTimeout` / `clearTimeout`
6. 工具特化渲染：`JsRenderReg`（不含 `JSValue`，可被任意线程持有）+ 引擎侧
   `renderMtx_`/`renders_`/`retiredRenders_`；脚本函数按 token 存在
   `JsPluginCtx::renderCallbacks`（随实例析构释放）
7. io→JS 同步桥：`invokeRenderSync`（最多等 `kRenderSyncTimeoutMs` = 150ms，超时置
   `abandoned`）+ `runRenderSyncOnJsThread`（组装 input、调用脚本、转换结果）+
   `renderCollapsed` / `renderExpanded`（C ABI 回调，异常不出边界）
8. 防互等：`jsHostCallDepth_` 在 `clientBridgeCall` 全程 +1；io 线程的回调看到非零即返回
   `ERR_STATE`（回退默认渲染）
9. 停止路径：`jsThreadMain` 退出前把 `renders_` 全部标记 `dead` + 抹零 token，脚本函数由
   `JsPluginCtx` 析构释放；`retireRenderOnJsThread` 摘注册时把注册项移进 `retiredRenders_`
   （不就地释放 —— 它作为 `user_data` 被主程序持有）
10. client 入口：`agentxx_plugin_client_{get_info,create,start,stop,destroy}`；`start`/`stop`
    直接复用 `jsEngineStart`/`jsEngineStop`（能力注册只用到通用表 `capabilities`/`log`/`json`）

验证：`agentxx_javascript_engine` 编译通过；`plugins`（agent 侧 JS 用例）649/0 不变；
`client_plugins` 第 34 段（真实插件加载 + 渲染回调）通过。

## 二、示例插件：`agent/plugins/example_js/`

- `libexample_js.cpp`：`ShellCtx` 同时保存 `AgentIfaces` 与 `ClientIfaces` 与三个通用表指针
  （`capabilities`/`json`/`log`）+ `clientSide` 标记；`createShell(host, clientSide, ...)`
  按侧查 `get_own_info`；`shellStart`/`shellStop` 两侧共用；入口用
  `AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT` 与 `AGENTXX_PLUGIN_CLIENT_LIFECYCLE_EXPORT`
  各生成一套 start/stop，手写两套 `get_info`/`create`/`destroy`
- 路径解析修正：`path` 在 client 侧是**插件目录**（agent 侧是库文件），先判断是不是目录
  再用它当脚本目录（之前只按库文件取父目录，client 侧会找不到 `plugin.js`——这是本轮实测
  发现的唯一功能性问题，已修）
- `plugin.yaml`：`interfaces.require` 同时声明 `agentxx.agent.core`（agent 侧能力调用）与
  `agentxx.client.msg_decor`（client 侧工具行定制），两侧协商各自只比较自己那部分
- `plugin.js`：顶层按 `agentxx.side` 分支（老引擎没有该字段时按 agent 侧处理）；client 侧
  给 `js_hello` 注册渲染器（显示名 `JS Hello`，折叠内容区分运行中/已完成，展开体给三条 Text）

## 三、测试：`agent/test/plugin/test_client_plugins.cpp` 第 34 段

覆盖：

1. 先加载 `plugins/agentxx_javascript_engine`（client 入口），再加载 `plugins/example_js`
   —— 走依赖顺序那条真实路径
2. UI 注册表里出现 `js_hello` 的注册项：归属 `example_js`、显示名 `JS Hello`、两个回调都非空
3. 折叠内容（运行中 `greeting Ada...` / 已完成 `greeted Ada`）—— 两次内容不同，证明回调每次
   真的执行；展开体前两项文本与脚本一致
4. 未注册的工具（`js_async_wait`）不命中 → 仍走默认渲染
5. `disable` 摘除注册项 → `enable` 后 start 重跑脚本重新注册，回调照常可用
6. `unload example_js` 后注册项消失；最后卸载引擎

> 插入时踩了一次坑：`old_str` 匹配到函数内**第一处** `co_return TestResult{...}`（第 1 段的
> 提前返回），整段被塞进了 `if (!inst)` 分支里，测试计数没变才发现。已把整段挪到函数末尾。

## 四、验证结果（Windows，Debug + ASan）

| 模块 | 结果 |
| --- | --- |
| `client_plugins` | 797 / 0（基线 774，新增 23 条断言） |
| `plugins` | 649 / 0（与基线一致，agent 侧未受影响） |
| `plugin_cleanup` | 90 / 0 |
| `plugin_host_mount` | 146 / 0 |
| `plugin_runtime` | 730 / 0 |
| `plugin_sdk` / `plugin_bridge` | 133 / 0、193 / 0 |
| `plugin_multi_instance` | 80 / 0 |
| `config_loader` / `config_validation` / `config_keys` | 444 / 47 / 217，全 0 失败 |
| `boundaries` | 19 / 0 |
| `wire_schema` | 1325 / 0 |

编译：`agentxx_javascript_engine`、`example_js`、`agentxx_test` 全部通过。lib 头文件未改，
不需要 `cmake --install` 刷新安装前缀。

## 五、与计划的偏差 / 注意事项

- **没有做 agent 侧任何改动**（计划中的第 5 条"client 分支代码独立"）：agent 桥只多了一行
  提前的 `auto* engine`，其余分支一行未动，`plugins` 模块计数不变可以佐证
- **`MountScope` 伪作用域**：client 侧实例键是 `<脚本名>@client`；没有 mount/unmount 入口
  （client 表成员不带 `PluginxxScopeRef`，也不需要）
- **渲染回调必须同步返回**：返回 Promise 会按"没有自定义内容"处理（计划里就是这么定的），
  文档已写明；耗时逻辑应改成订阅事件 + `updateToolDecor`
- **`retiredRenders_` 为 `user_data` 续命**：注册项摘除后不就地释放，留到引擎析构。代价是每次
  注册/注销留一个几十字节的结构，可忽略
- **引擎被卸载而壳仍启用**时（正常路径不会：依赖级联会把壳一起停），注册项仍在但回调
  立即失败（`dead`），界面回退默认渲染，不会崩
- **MSVC 对 `AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(..., nullptr, nullptr)` 报 C4353**
  （`(nullptr)(args)` 形态）：`example_js_execute_command` 早就是这样，属既有告警，本轮未新增
  其它告警
- 一次构建 `agentxx_test` 时曾因内存不足失败（`LNK1102`），空闲后重跑通过，与代码无关

## 六、留给后续

1. ~~把 client 侧其余面搬上桥~~ —— 已在本轮第二批完成，见第七节
2. 若某些场景确实需要"回调里 await"，再考虑扩 ABI 的"先 pending、稍后回填缓存"形态；
   当前 150ms 同步桥已覆盖渲染与命令这类毫秒级需求
3. `plugin.js` 的 agent 分支被整体缩进一层，diff 较大；后续改动注意别把两侧分支写串

## 七、第二批：展示项 / 命令 / 动作 / 定时器 / 快捷键（同日）

第一批只把“渲染器 + 展示类调用”搬上桥，client 表里需要长期持有句柄或回调登记的部分
（状态栏项 / 面板 / Info 段落 / 命令 / 动作 / 定时器 / 快捷键）留在了 C++ 壳里。
本轮按同一套桥补齐，client 六张表现在全部可从脚本使用。

### 7.1 引擎：`agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`

1. 桥魔法号新增 19 个：`B_CLIENT_REGISTER/UPDATE/UNREGISTER_STATUS_ITEM`、
   `..._PANEL`、`..._INFO_SECTION`、`B_CLIENT_REGISTER/UNREGISTER_COMMAND`、
   `B_CLIENT_BIND/UNBIND_ACTION`、`B_CLIENT_SET/CANCEL_TIMER`、`B_CLIENT_IS_REGION_VISIBLE`、
   `B_CLIENT_REGISTER/UNREGISTER_KEYBIND`、`B_CLIENT_LIST_KEYBINDS`；JS 侧方法名与 C++ 表成员
   一一对应（`registerStatusItem` / `registerPanel` / `registerInfoSection` / ...）
2. 句柄 token：client 表的句柄是主程序侧的不透明指针，JS 的 number 存不下。因此加
   `enum class ClientHandleKind : int32_t { StatusItem = 1, Panel, InfoSection, Timer, Keybind }`，
   实例里用 `JsPluginCtx::ClientHandle{kind, handle, id}` 记 token → 记录（`clientHandles`）；
   token 由 `nextCallbackToken` 自增，`0` 表示主程序不支持该子能力或注册失败；
   更新/注销先校验类别，类别不符或已注销返回 false（不抛异常）
3. 回调登记：`JsClientCallback{engine, instanceKey, fnToken, dead, label}` 不含 `JSValue`，
   可被任意线程持有；引擎侧 `clientCbMtx_` + `clientCbs_`（`unique_ptr`，一直活到引擎析构
   —— 注册项作为 `user_data` 被主程序持有，摘注册后可能还有在途投递）；脚本函数按 token
   存在 `JsPluginCtx::clientCallbacks`；`clientCbByKey` 用 `cmd:<名>` / `act:<归属>` /
   `timer:<token>` / `keybind:<token>` 做键，支持“同名重复注册就丢旧的”
4. 两条调用路径：
   - **同步**（只有命令 `execute`）：`invokeCallbackSync` → `runCallbackSyncOnJsThread`，
     与渲染同一条 150ms 上限（原 `kRenderSyncTimeoutMs` 改名 `kClientSyncTimeoutMs`）；
     返回值直接当动作 JSON 交回主程序（格式与 C++ 插件一致）
   - **投递**（按钮动作 / 定时器 / 快捷键）：`postCallback` → `runCallbackOnJsThread`，
     回调参数分别是 `{ownerId, actionId, args}` / `{}` / `{keys}`
5. 撤销登记：所有 client 注册也进 `pctx->rollbackActions`（脚本顶层失败或卸载时逆序撤销），
   与 agent 侧一致；句柄类（状态项/面板/段落/定时器/快捷键）的撤销先按 token 判活，
   脚本自己注销过的不再交一次（主程序侧的重复注销本身也安全，但没必要多跑一轮 io）；
   停止路径把 `clientCbs_` 全部标 `dead`，实例析构清 `clientHandles`（句柄本身由主程序
   按挂载记录摘除）
6. `isRegionVisible(ownerId)` 走 `agentxx.client.timer` 的 `is_visible`（区域可见性由界面
   每帧上报，见 tui.md §2.2 的可见性快照）

### 7.2 示例 `agent/plugins/example_js/plugin.js`（client 分支）

| 面 | 示例内容 |
| --- | --- |
| 状态栏项 | `js.demo`（右侧，order 50）初始 `js: ready`；按键后改成 `js: key N` |
| Info 段落 | `js.demo.info`（标题 `JS Demo`），定时器 tick 时重写第二行为 `timer tick N` |
| 面板 | `js.demo.panel`：标题文本 + 按钮「打个招呼」（动作 `js.demo.hello`） |
| 斜杠命令 | `/jsdemo` → `{action:"toast", text:"hello from JS"}` |
| 动作绑定 | `bindAction("js.demo.panel")` → toast `JS action: <actionId>` |
| 快捷键 | `ctrl+alt+j` → 计数 +1 并更新状态栏；`ctrl+alt+u` → 注销展示项并 toast 结果（`listKeybinds` / `isRegionVisible` / `unregister*` / `cancelClientTimer`） |
| 定时器 | 60ms × 2 次，`pauseWhenHidden: true`，`ownerId = js.demo.info` |

### 7.3 测试：`client_plugins` 第 34 段（+20 条断言）

在原有渲染器断言之后追加：状态栏项（`js: ready`）/ Info 段落（标题 `JS Demo`）/ 面板
三项出现在注册表；`hasCommand("jsdemo")`；`invokeCommand("jsdemo")` 的 toast 文本为
`hello from JS`；`postKeybindInvocation("ctrl+alt+j")` 后状态栏文本变成 `js: key 1`
（证明回调真的执行并改了注册项）；`dispatchAction("example_js", "js.demo.panel",
"js.demo.hello")` 的 toast 为 `JS action: js.demo.hello`；`reportRegionVisible("js.demo.info",
 true)` 后等定时器跑完，Info 段落内容出现 `timer tick 2`；`ctrl+alt+u` 触发一次性注销
（状态栏项 / Info 段落 / 面板都从注册表消失，`ctrl+alt+j` 键位已注销而 `ctrl+alt+u` 仍在，
toast 回带 `listKeybinds()` 条数与 `isRegionVisible()` 结果）。禁用/卸载后注册项与命令一并消失。

### 7.4 验证结果（Windows，Debug + ASan）

| 模块 | 结果 |
| --- | --- |
| `client_plugins` | 817 / 0（第一批 797 + 展示项/交互 12 + 注销与查询 8） |
| `plugins` | 649 / 0（agent 侧未受影响） |
| `plugin_cleanup` / `plugin_runtime` | 90 / 0、730 / 0 |
| `plugin_host_mount` / `plugin_multi_instance` | 146 / 0、80 / 0 |
| `plugin_sdk` / `plugin_bridge` | 133 / 0、193 / 0 |
| `boundaries` / `config_keys` / `config_loader` / `config_validation` | 19 / 0、217 / 0、444 / 0、47 / 0 |

`wire_schema` 本轮未重跑（改动不涉及 wire 协议；第一批时 1325 / 0）。

### 7.5 偏差 / 注意事项

- **命令 `execute` 是同步的**：它与渲染回调共用 150ms 上限。脚本里做耗时查询会超时按失败
  处理（主程序给用户报命令失败），不是崩溃
- **动作/定时器/快捷键回调不设上限**：它们是投递式的，主程序不等待结果；回调里抛异常
  只记日志
- **`unregister*` 不释放回调登记**：`clientCbs_` 里的记录留到引擎析构（与
  `retiredRenders_` 同一考虑），每次注册/注销留几十字节，可忽略
- **重复注册同一命令名**：主程序返回 `ERR_CONFLICT`，脚本拿到 `false`（不抛异常）；
  动作绑定是“新的覆盖旧的”语义（与 C++ 表一致），定时器则每次都是新句柄，旧的需自己
  `cancelClientTimer`
- **示例脚本里按钮动作名带面板前缀**（`js.demo.hello`），测试断言里也按这个全名匹配

### 7.6 实施中发现的一个时序细节（已写进文档）

client 表的注册/更新入口（`xx_cregister_status_item` 等）走 `onClientIo` → `ioCallSyncKeep`：
**在 JS 线程上同步等 io 线程做完**。因此脚本顶层注册展示项的那段时间，JS 线程始终在
“等主程序”（`jsHostCallDepth_ > 0`），这期间到达的渲染请求按设计直接放弃，并把结果按
“没有自定义内容”写进缓存（同一 `tool_call_id` + 同输入特征不会再请求）。

用例里“`enable` 之后重新渲染”原本沿用同一个 `call-js-4`：因为脚本还在注册后面的
展示项，那一次渲染确定性拿不到内容（808/1）。现改为换新的 `tool_call_id` 重试到脚本跑完
（真实界面下一帧本来就会再请求一次），809/0 且连跑 3 次稳定。

同一原因推出两条使用约束，已写进 `plugins.md` §12 与 `AGENTS.md`：
- 渲染/命令回调里不要再注册或更新展示项（两边互等，只能等到 150 ms 上限后各自退回）
- 一次失败的渲染会按“没有自定义内容”缓存，界面走默认渲染

### 7.7 注销与查询入口的覆盖

第一轮只覆盖了注册与回调，`unregister*` / `cancelClientTimer` / `isRegionVisible` /
`listKeybinds` 没有任何用例。现用一个 `ctrl+alt+u` 快捷键（投递式回调，io 线程不在等，
里面调表不会互等）在脚本里一次性注销状态栏项 / Info 段落 / 面板 / 另一个键位、取消定时器，
并把 `listKeybinds()` 条数与 `isRegionVisible()` 结果带回 toast；用例断言注册表里三项已消失、
`ctrl+alt+j` 键位已注销而 `ctrl+alt+u` 仍在（`client_plugins` 809 → 817）。

### 7.8 没做的部分（有意留白）

- **网络不在 client 桥上**：`agentxx.net`（HTTP/WS/TCP/UDP/服务端）只在 agent 侧与 C++ client 插件里可用。客户端脚本要拿外部数据，就用 `sendPluginData` 通知 agent 侧脚本去取，或直接写成 C++ client 插件（已写进 `plugins.md` §12）
- **异步回调**：命令 `execute` / 渲染回调仍然只能同步返回（150 ms）；“先 pending、稍后回填”的桥没有做，因为当前没有这类需求（耗时活放到动作/定时器回调里推数据即可）
- **`clientCbs_` 不回收**：每次注册/注销留几十字节到引擎析构（与 `retiredRenders_`、`renders_` 一致），长时间反复 enable/disable 的脚本会缓慢累积，可忽略
