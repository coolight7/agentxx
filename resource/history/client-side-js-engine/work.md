# 实施记录：client 侧 JS 引擎

## 进度

- [x] 现状调查（引擎 / client 契约 / 主程序调用路径 / 示例插件）
- [x] 任务卡与方案（`index.md` / `plan.md`）
- [x] 引擎：Side 分流 + client 桥子集 + 渲染同步桥 + client 入口
- [x] `example_js`：双入口壳 + 清单 + 脚本分支
- [x] 编译（插件 / 测试）
- [x] 测试：`client_plugins` 第 34 段 + 回归
- [x] 文档：`plugins.md` §12/§15.5、`index.md` 目录注释、`AGENTS.md`、示例配置注释

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

1. 把 client 侧其余面搬上桥：状态栏项 / 面板 / Info 段落 / 斜杠命令 / 客户端定时器 /
   全局快捷键 / 动作回调绑定（都需要长期持有句柄或回调登记）
2. 若某些场景确实需要"回调里 await"，再考虑扩 ABI 的"先 pending、稍后回填缓存"形态；
   当前 150ms 同步桥已覆盖渲染这类毫秒级需求
3. `plugin.js` 的 agent 分支被整体缩进一层，diff 较大；后续改动注意别把两侧分支写串
