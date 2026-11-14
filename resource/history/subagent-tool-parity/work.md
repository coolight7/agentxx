# 实施记录: 子代理工具对齐 + JS 插件多挂载作用域

- 时间: 2026-11-14

## 进度

- [x] 1. `SubagentInherit` 类型 + 事件/异步任务结构追加字段
- [x] 2. `makeSubagentConfig(parentConfig, inherit)` 按开关派生 (缺省 = 旧行为)
- [x] 3. `agentxx_subagent` / `agentxx_async_run` 写入 inherit, 中断参数解析
- [x] 4. 动态 (插件) 工具受 `tools` 白名单约束 + `toolNames` 收动态名
- [x] 5. 工具提示词更新 (`tools` 缺省语义 + 子代理与主 agent 同一套工具)
- [x] 6. 测试 `test_host_spawn_inherit_env` (父持有插件工具 → 继承带 / 不继承不带)
- [x] 7. JS 引擎改为每 (脚本, agent 挂载) 一份实例 + 权限/配置类 JS API 对齐
- [x] 8. 构建 (lib / 插件 / test) + 相关测试模块全绿
- [x] 9. 文档 (`plugins.md` §12、`index.md` 子代理段、AGENTS.md 摘要)
- [ ] 10. 后续项: JS SDK 剩余面 (graph 节点类型 / scheduler·tasks 协程驱动)
- [x] 11. 方案 B: 工具特化渲染跨端 (`agentxx.agent.tool_render` + 三条 wire 消息 + JS SDK)
  详见文末"第五步"

## 第 1 步: 继承开关的数据结构与传递

改动文件:

- 新增 `agent/lib/include/agentxx/agent/subagent_inherit.h`
  - `SubagentInherit{ plugins, mcp, skills, memory, rag }`, `none()` / `full()` / `any()` / `operator|`
  - 独立头文件: `events.h` 与 `agent_host.h` 都要用, 避免互相包含
- `agent/lib/include/agentxx/event/events.h`
  - `SubagentBatchItem` 追加 `agentxx::agent::SubagentInherit inherit{}` (缺省全关)
- `agent/lib/include/agentxx/agent/async_work.h`
  - `AsyncWorkSpec` 追加 `SubagentInherit inherit{}` (后台任务路径)
- `agent/lib/include/agentxx/agent/agent_host.h`
  - `makeSubagentConfig(parentConfig, const SubagentInherit& = none())`

验证: lib 编译通过 (只有既有 C4100 警告)。

## 第 2 步: 配置派生与工具策略

`agent/lib/src/agent/agent_host.cpp`

- `makeSubagentConfig`: 逐项按开关处理 —— `mcp`/`rag`/`skills`/`memory` 关闭时清空对应列表;
  `plugins` 关闭时强制 `pluginMountPolicy = None` + 清 `pluginMountList`, 打开时**保留**
  策略/名单/排除/挂载参数。`plugins` 加载表始终清空 (子代理只用进程共享管理器)。
  `enableSessionStore=false` / worktree 绑定 / subagent 模型语义不变。
- `spawnOneTask`: 合成 `inherit = task.inherit | spec->inherit`;
  `tools == []` 或 `toolExecutionDisabled` 时强制 `inherit.plugins = false`;
  有继承时打一条 `spawnOneTask ... inherit plugins=... mcp=...` 日志。
- `agent/lib/src/tools/subagent.cpp`
  - `SubAgentManagerTool::execute_async` 给每个任务写 `inherit: {plugins,mcp,skills,memory,rag}=true`
  - 新增 `parseInheritArg` (接受 `true` 与逐项对象, 缺省/非法 = 全关), 两处解析分支都填进
    `SubagentBatchItem::inherit`
- `agent/lib/src/tools/async_tools.cpp`
  - `AsyncRunTool` 在 `tool == "agentxx_subagent"` 时写 `spec.inherit = full()`

## 第 3 步: 工具集真正一致 (两个缺口)

1. `agent/lib/include/agentxx/plugin/tool_registry.h` + `src/plugins/tool_registry.cpp`
   - 新增 `setNameFilter(std::optional<std::vector<std::string>>)` + `nameAllowed()`
   - `find` / `contains` / `appendDefinitions` / `names` 按名单过滤; `size()` 仍返回注册总数
   - 原因: 插件工具是 mount 时注册的, 白名单在挂载前已确定 —— 只能在读取侧过滤,
     否则挂了插件之后 `tools: []` / 自定义名单会失效
2. `agent/lib/src/agent/base_agent.cpp`
   - `tool_whitelist` 步骤: `enableToolFiltering` 时把白名单交给 `ToolRegistry`;
     否则清掉过滤 (保持旧行为)
   - `bind_tools`: `toolNames` = 静态工具名 + `toolRegistry->names()` (去重),
     否则 `tools: ["*"]` 解析出的名单永远缺插件工具

测试补充: `agent/test/core/test_agent_host.cpp` 新增 `test_host_spawn_inherit_env`
- 经容器 `createAgent` 构造根 (才注入共享插件管理器)、加载 `agentxx_math`
- 断言父 `toolNames` 含 `agentxx_math_calculate`
- 派生两次: `inherit=full` 的请求带该工具, `inherit=none` 的请求不带; 两边都有内置
  `agentxx_share_store`
- 插件目录不存在时跳过 (与其它插件测试一致)

## 第 4 步: JS 引擎多挂载作用域

`agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`

数据模型:

- `ScriptEntry{name, path, code, host}` + `scripts_` (脚本登记) —— **仅 io 线程访问**
- `MountScope{key=agentId#seq, agentId, seq}` + `scopes_` —— **仅 io 线程访问**
- `plugins_` / `mirror_` 键从脚本名改成**实例键** `脚本名@agentId#seq`
- `JsPluginCtx` 追加 `instanceKey` / `scopeAgentId` / `scopeSeq` / `scopeRef` + `setScope()` / `scope()`
- `JsToolBinding` / `JsHookBinding` / `JsTimer` 记实例键, 执行/回调按实例键找上下文

生命周期:

- 引擎 `mount` → `attachMountScope`: 登记作用域, 给每个已登记脚本投递一次实例创建,
  返回 `ScopeHandle` 作为 per-agent 状态
- 引擎 `unmount` → `detachMountScope`: 移出作用域, 投递销毁该作用域全部实例
- 能力 `load` → `loadScriptAsync`: 脚本先登记, 再对当前作用域快照各建实例,
  完成通知里给各实例工具名并集
- 能力 `unload` → `unloadScriptAsync`: 逐实例执行 JS 侧注销 + 释放 JSContext
- 引擎 `requestStop` (Running → Stopping) 清空 `scripts_` / `scopes_`:
  重新 start 时壳插件会重新 load, 旧登记留着会命中 "plugin already loaded"
- agent 卸载路径**不执行** JS 侧注销动作 (`runRollback=false`): 主程序已按挂载记录撤销
  全部注册, 再照着回滚会碰到已随实例析构释放的句柄 (实测 ASan heap-use-after-free
  在 `unsubscribePluginBroadcast`)

注册归属: 桥里所有 `engine->agentRef()` 改成 `pctx->scope()` (16 处), 即"本实例所属挂载",
不再是"最后一次 mount 的作用域"。

JS SDK 对齐 (与 C++ 插件 SDK 同名同义):

- 新增 `scope()` / `getConfig()` / `getPluginArgs()` / `getLanguage()` / `setLanguage()`
- 新增 `registerToolPermission(spec)` / `unregisterToolPermission(name)` /
  `registerReadPathPermission(toolName, argName)` / `registerWritePathPermission(...)`
  (后者两个是 C++ 便捷函数的同义简写; 主程序未加载权限中间件时返回 false 并忽略)
- 权限声明在实例卸载时登记撤销动作

脚本语义: 脚本顶层代码 = 该 agent 的插件实例初始化 (每个 agent 独立 JSContext),
进程级状态放 C++ 壳的 `start`。

## 验证

### 构建产物 (本次实际执行)

- 内层 lib 构建目录: `agentxx_static` 重编 + `cmake --install` (刷新安装头)
- 插件: `agent/build/windows-debug/exec/plugins/agentxx_javascript_engine/libagentxx_javascript_engine.dll`
- 测试 exe: 重编后拷到 `agent/build/windows-debug/exec/agentxx_test.exe`
- 客户端: 内层 client 构建目录 `--target agentxx` 链接通过 (未拷到 exec: 当前运行中的
  `agentxx.exe` 占着文件, 属预期)
- 行尾/编码: 新增/修改文件均 UTF-8 无 BOM + LF (`git ls-files --eol` 为 `w/lf`)

### 工作树里的无关改动

`git status` 里 `agent/lib/src/middlewares/summarization.cpp`、`agent/lib/src/nodes/modelcall.cpp`、
`agent/test/core/test_summarization.cpp`、`agent/test/plugin/test_plugins.cpp` 是本次开工前就存在的
本地修改 (取消提示文案与格式化), **不是本任务改的**, 未动。

### 测试结果

构建:

- lib: `cmake --build <内层 lib build> --config Debug --target agentxx_static` + `--install`
- 插件: `pwsh -File agent/script/build_plugins.ps1 agentxx_javascript_engine`
- 测试: `cmake --build <内层 test build> --config Debug --target agentxx_test`

运行 (`agent/build/windows-debug/exec/agentxx_test.exe`):

| 模块 | 结果 |
|---|---|
| `agent_host` (含新增 `test_host_spawn_inherit_env`) | 190 通过 / 0 失败 |
| `plugins` (含 JS 引擎 disable↔enable 风暴) | 599 / 0 |
| `plugin_runtime` / `plugin_sdk` / `plugin_bridge` | 730 / 133 / 193, 全 0 失败 |
| `plugin_host_mount` / `plugin_multi_mount` / `plugin_cleanup` | 146 / 47 / 90, 全 0 失败 |
| `agent` / `async_work` / `async_tools` / `event_subagent` / `event_tools` / `assembly_snapshot` | 1128 合计, 全 0 失败 |
| `boundaries` / `config_keys` | 19 / 217, 全 0 失败 |

## 遇到的问题

1. `test_host_spawn_inherit_env` 第一版直接 `make_shared<CodeAgent>` 构造根, 插件工具没出现 ——
   那样 `init` 时上下文还没有插件管理器, agent 会自建一份空的。改用 `host->createAgent(cfg)`。
2. JS 引擎 `mount`/`unmount` 的日志最初用 `guardLog` (error 级), 输出看着像异常;
   改为新增 `logAt(level, msg)` (info/warn)。
3. `unmount` 入口拿不到 `AgentxxPluginAgentMountSpec` (契约只回 `mount_ud`), 因此
   `mount` 返回 `ScopeHandle{scope}`, `unmount` 从它取作用域。
4. ASan heap-use-after-free: agent 卸载时执行 JS 侧注销 -> 订阅句柄已随实例析构释放。
   区分两条销毁路径 (脚本卸载 = 执行回滚, agent 卸载 = 只释放 JS 侧)。

## 剩余事项

- JS SDK 与 C++ 侧仍不对齐的面: 工具特化渲染 (注册在 client 侧 + 内容回调同步返回,
  不是 UI 线程限制, 见文末"第四步")、`scheduler`/`tasks` 的协程驱动。这些目前必须写在
  C++ 壳里; 后续按需逐表补齐。(`net` / 后台任务 / 提示词 / 会话 / 上下文 / 执行图 /
  挂载查询 / 会话运行时 / 广播写入方已补齐, 见下节)
- 未加多 agent 视角的 JS 自动化用例 (`plugins` 模块覆盖了单 agent 的 load/disable/enable
  全流程; 多挂载由 `ScopeHandle` + `scopes_` 逻辑保证, 建议后续在 `plugin_multi_mount`
  里加一条 JS 插件双 agent 用例)。
- 每个子代理多一次 N 个插件 mount (纯注册) 与 MCP 重新握手; 只在 `agentxx_subagent`
  路径发生, 压缩/内部路径不受影响。需要时可用性能基准测一轮 spawn 增量。

## 第二步: JS SDK 补齐网络 / 后台任务 / 查询面

### 改了什么

- `agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`
    - 新增 bridge magic: 网络 (`B_NET_CALL` / `B_NET_CALL_ASYNC` / `B_NET_CANCEL` /
      `B_NET_FETCH` / `B_NET_DOWNLOAD` / `B_NET_WS_CONNECT` / `B_NET_WS_SEND` /
      `B_NET_WS_CLOSE` / `B_NET_HTTP_SERVE` / `B_NET_HTTP_RESPOND` / `B_NET_INFO`)、
      后台任务 (`B_SUBMIT_ASYNC_WORK` / `B_CANCEL_ASYNC_WORK` / `B_GET_ASYNC_WORK` /
      `B_LIST_ASYNC_WORK`)、查询面 (`B_GET_PROMPT` / `B_SET_PROMPT` / `B_GET_TOOL_PROMPT` /
      `B_GET_SESSION_WORK_DIR` / `B_IS_CANCELED` / `B_GET_MESSAGES` / `B_MESSAGES_COUNT` /
      `B_GET_GRAPH_JSON` / `B_GET_GRAPH_NAME` / `B_SET_GRAPH_JSON` / `B_LIST_MOUNTS` /
      `B_GET_MOUNT`)
    - 网络: 结果始终是对象 (`ok` 是业务结论), Promise 不 reject; 异步完成经
      `JsNetBridge` + `onNetCallDone` 投递回 JS 线程 settle, 受理时给出的请求号补成
      `requestId` 字段 (之后可 `netCancel`)
    - 后台任务: 选项对象 → `AgentxxPluginAsyncWorkSpec`; `onEvent(phase, payload)` 的
      JS 回调存在实例的 `asyncWorkCallbacks` 表, 主程序 io 线程事件经
      `JsAsyncWorkHook` + `onAsyncWorkEvent` 转投 JS 线程; 终态 (`finished` / `canceled`)
      到达即释放登记, 引擎析构清理未收到终态的残留
    - 新增通用小工具: `propString` / `propInt64` / `propBool` / `propJson` /
      `argsJsonText` / `jsonTextToValue` / `netResultValue` / `netOutcomeToValue` /
      `netErrorObject` / `asyncWorkPhaseName` (`valueToJsonString` 改为 static, 供它们复用)
- `agent/plugins/example_js/plugin.js`: 新增 `js_sdk_probe` (查询面形状自查) 与
  `js_net_fetch` (async HTTP; 连不上按 `ok:false` 返回) 两个示例工具
- `agent/test/plugin/test_plugins.cpp`: 新增 `16b` / `16c` 两段断言 (形状约定 +
  网络失败不抛异常), `plugins` 模块 599 → 627
- 文档: `docs/zh-cn/design/plugins.md` §12 增加网络/后台任务/查询面对齐说明与剩余面;
  `AGENTS.md` 同步

### 验证

| 模块 | 结果 |
|---|---|
| `plugins` (含新增 `16b` / `16c`) | 627 通过 / 0 失败 |
| `plugin_runtime` / `plugin_sdk` / `plugin_bridge` | 730 / 133 / 193, 全 0 失败 |
| `plugin_host_mount` / `plugin_multi_mount` / `plugin_cleanup` | 146 / 47 / 90, 全 0 失败 |
| `plugin_dispatch` / `plugin_feature` / `plugin_point` / `plugin_broadcast` / `plugin_agent_runtime` | 112 / 34 / 68 / 90 / 221, 全 0 失败 |
| `async_work` / `async_tools` / `agent` / `event_subagent` / `event_tools` | 138 / 178 / 223 / 52 / 97, 全 0 失败 |
| `boundaries` / `config_keys` | 19 / 217, 全 0 失败 |

插件重编与测试拷贝: 内层 lib 构建目录 `--target agentxx_javascript_engine` 与
`--target example_js` (后者拷贝 `plugin.js`), 内层 test 目录 `--target agentxx_test`,
再把 `agentxx_test.exe` 拷到 `exec/`。

### 遇到的问题

1. `build_plugins.ps1` 经 `pwsh -File` 启动时卡住不动 (无 cmake/cl 子进程, 日志一直为空),
   直接 `cmake --build <内层 lib build> --config Debug --target ... --parallel 12` 正常。
   本次构建与验证都改为直调 cmake。
2. `valueToJsonString` 原本是非静态成员, 而新增的形状小工具是 static 且从 lambda 里调用 →
   MSVC `C2352`; 改成 static (没有状态依赖)。
3. `bridgeCall` 是静态成员函数, lambda 里不能写 `this->settleNetCall(...)`, 改为
   `bridge->engine->settleNetCall(...)`。

### 遗留的小口子

- 脚本实例被销毁 (agent 卸载 / 脚本卸载) 时, 它提交的后台任务**不会**随之取消
  (接口表只有按 asyncId 取消, 而实例销毁时主程序不代怔); 任务会继续跑完, 事件到
  JS 线程后因查不到实例而丢弃。要取消就在卸载前自己调 `cancelAsyncWork`。
- 同一引擎里的所有脚本共享一份网络资源 (属主 = 引擎这个插件实例), `ws.connect` 一类
  连接 id 需要脚本自己加前缀避免相撞。
- 工具特化渲染未做 (注册在 client 侧 + 内容回调同步返回, 不是 UI 线程限制)。

## 第四步: 校对文档 (工具特化渲染为什么没做)

之前写的理由 ("UI 线程回调" / "需 UI 线程上下文") 不对。查代码后确认 UI 线程
**从不**调插件回调:

- `message_list.cpp` 的 `queryToolCollapsed` / `queryToolExpanded` 只读
  `ClientPluginManager::toolRenderCache()`; 未命中先 `toolRenderInFlight` 去重, 再
  `requestToolRender` 把参数拷成拥有型 `ClientToolRenderRequest` 投递到 **client IO 线程**;
- 回调 (`collapsedFn` / `expandedFn`) 在那边跑完把结果写进 `ClientToolRenderCache`,
  再经 `PluginUiAdapter::onToolRenderUpdated` 通知 UI 重绘; 显示名注册时就有、首帧可读。

真正阻塞 JS 的两条是:

1. 渲染器注册在 **client 侧** (`agentxx.client.ui` 的 `register_tool_renderer` /
   `ClientPluginManager::registerToolRenderer`), 而 `agentxx_javascript_engine` 只有
   agent 侧入口 (`agentxx_plugin_agent_*`), 目前也没有 client 侧 JS 引擎;
2. 内容回调是**同步返回**的 (`int32_t(..., *output)`), JS 桥得在 client IO 线程上同步
   转到 JS 线程取值; 脚本里 `await` 会阻塞该线程 → 需超时, 或把 ABI 改成
   "先返回 pending, 稍后回填缓存"。

已改: `docs/zh-cn/design/plugins.md` §12 与 `AGENTS.md` 的措辞 (不再说"UI 线程限制")。

## 第三步: JS SDK 补齐会话运行时 (自建根 / 通道 / 触发器 / 定时器)

### 改了什么

- `agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`
    - 把回调登记结构改名为通用 `JsCallbackHook` (后台任务事件与会话通道事件共用),
      引擎成员改为 `callbackMtx_` / `callbackHooks_`, 实例回调表分
      `asyncWorkCallbacks` / `channelCallbacks`, token 源统一为 `nextCallbackToken`
    - 新增 `onChannelEvent` (主程序 io 线程) + `ChannelEventCopy` + `doChannelEvent`
      (JS 线程): 只拷 `kind` / `channelId` / `interruptId` / `text` / `payload_json`,
      事件对象交给脚本时是 `{channelId, kind, text?, interruptId?, payload}`;
      `closed` 是最后一条, 到那就释放登记
    - 新增 bridge magic: `B_MOUNT_ROOT` / `B_UNMOUNT_ROOT` / `B_OPEN_CHANNEL` /
      `B_CLOSE_CHANNEL` / `B_SEND_INPUT` / `B_CANCEL_CHANNEL` / `B_SELECT_MODEL` /
      `B_RESPOND_INTERRUPT` / `B_GET_CHANNEL_STATE` / `B_GET_CHANNEL_MESSAGES` /
      `B_LIST_RUNTIME` / `B_REGISTER_TRIGGER` / `B_UNREGISTER_TRIGGER` /
      `B_REGISTER_TIMER` / `B_UNREGISTER_TIMER` / `B_EMIT_EVENT`
    - 失败统一降级: `mountRoot` 返回空串、`openChannel` / `registerTrigger` /
      `registerTimer` 返回 0、其余布尔入口返回 `false` (不抛异常)
- `agent/plugins/example_js/plugin.js`: `js_sdk_probe` 增加运行时面的形状检查
  (只用无效句柄, 不真的建根/开通道)
- `agent/test/plugin/test_plugins.cpp`: 16b 段扩充对应断言 (`plugins` 627 → 641)
- 文档: `docs/zh-cn/design/plugins.md` §12 增加运行时面对齐说明 (含事件 `kind` 取值与
  `listRuntime()` 的形状); `AGENTS.md` 同步

### 验证

| 模块 | 结果 |
|---|---|
| `plugins` (含新增运行时形状断言) | 641 通过 / 0 失败 |
| `plugin_runtime` / `plugin_sdk` / `plugin_bridge` | 730 / 133 / 193, 全 0 失败 |
| `plugin_host_mount` / `plugin_multi_mount` / `plugin_cleanup` | 146 / 47 / 90, 全 0 失败 |
| `plugin_dispatch` / `plugin_feature` / `plugin_point` / `plugin_broadcast` | 112 / 34 / 68 / 90, 全 0 失败 |
| `async_work` / `async_tools` / `plugin_agent_runtime` / `boundaries` / `config_keys` | 138 / 178 / 221 / 19 / 217, 全 0 失败 |

### 遇到的问题 / 发现

1. `plugin_api.h` 里 `list_runtime` 的注释写的是数组 (`[{"rootId",...}]`), 实际实现
   (`PluginManager::listRuntimeJson`) 返回的是对象 `{roots, triggers, timers}`。
   本次以实现为准 (JS 侧直接给这个对象), 文档写清; 头文件注释未改 (改它会触发
   全库重编, 另开一次改)。
2. 真建根 / 开通道会真起 agent, 因此自动化只验形状 (无效句柄路径); 真流程仍靠
   `plugin_agent_runtime` (C++ 侧) 覆盖。要加 JS 真流程用例, 需在 `AgentHost` 容器上
   起一个 JS 插件再 `mountRoot`。
3. 补广播写入方与剩余读取面时发现: `declareBroadcast({})` (空声明) 不会报错, 框架
   把它当一次合法声明。因此探针不用空对象, 改为真声明一条自己的频道 (声明 → 发布 →
   读当前值 → 结束 → 再读得 `null`), 这才是有效用例。

### 又补的一小批 (广播写入方 / share_store 写入 / 资源 / 模型配置)

- `declareBroadcast(decl)` (`id` 可短名; 失败抛异常并带主程序给的错误文本) /
  `readBroadcast(id)` (未声明 / 无值 / 已结束 → `null`) / `endBroadcast(id, reason?)`
- `addShareStore(sessionId, content)` (返回条目 id) / `getOwnResources()` /
  `getModelConfig()`
- `example_js` 的 `js_sdk_probe` 增加真广播往返 (声明 → 发布 → 读 → 结束 → 再读)
  与上述三个读取面的形状断言; `test_plugins` 相应断言增加到 649 条

## 第五步: 方案 B —— 工具特化渲染跨端

背景: 之前工具行特化渲染只能由 **client 侧**插件注册 (回调在 client 进程里), agent 侧插件
(包括 JS 脚本) 完全碰不到。方案 B 把登记面也搬到 agent 侧, 内容回到 agent 算, 因此远程
TUI (两个进程) 与本地直连用同一条路径。设计见 `plan.md` §8。

### 改了什么

**1. agent 侧接口表与登记表**
- `agent/lib/include/agentxx/plugin/api/plugin_api.h`: 新表 `agentxx.agent.tool_render` v1
  (`register_tool_renderer` / `unregister_tool_renderer`) + 三个跨边界结构体
  (`AgentxxPluginToolRenderInput` / `...Output` / `...Spec`, 均带 `struct_size`)
- `plugin_iface_meta.h`: `IfaceMeta` 特化 + 两条 `static_assert`
- `plugin/api/plugin_kit.h`: `AgentIfaces::toolRender`
- `plugin/plugin_mount.h` / `src/plugins/plugin_mount.cpp`: `ToolRendererEntry` 表
  (键 = 工具名, 记 owner / 显示名 / 两个回调 / user_data / 实例弱引用),
  `registerToolRenderer` (同名别人的 `ERR_CONFLICT`, 自己的覆盖自己)、
  `unregisterToolRenderer`、`findToolRenderer`、`toolRenderersJson`、`removeToolRenderers`;
  `revokeEntry` 增加第 ⑦ 步 "按 owner 整批删除" (插件禁用/卸载自动撤销)
- `plugin_manager_vtable.cpp`: `xx_tool_render_register` / `xx_tool_render_unregister`
  (都经 `ioCallSyncKeep` 落到 io 线程) + `g_ifaceToolRender` + `query_interface` 分发
- `agent/session_tool_render_sink.h` (新) + `AgentContext::toolRenderSink` (弱引用):
  登记变化唯一的出口; 没端点时只影响本地状态
- 表数量: `PluginManager::kInterfaceTableCount` 10+10 → 10+11

**2. wire 消息 (三条)**
- `agent_io_transport.h`: `WireToolRenderItem` / `WireToolRendererRegister` /
  `WireToolRenderRequest` / `WireToolRenderResult` + 追加进 `WireMessage` 变体末尾
- `wire_protocol.h` / `src/agent/wire_protocol.cpp`: `MsgType` 常量、`make*` / `*FromJson`、
  反序列化表; 未知类型在 WS 层原本就"解析失败即跳过", 因此**不改**协议版本号
- `agent/schema/wire-schema.json` 与 `docs/zh-cn/design/wire-protocol-fields.md` 重生成
  (`AGENTXX_UPDATE_WIRE_SCHEMA=1 agentxx_test wire_schema`)

**3. agent 端点 (`SessionServerAgentIO`)**
- 实现 `SessionToolRenderSink`: `onToolRenderersChanged()` 投递回本端点执行器,
  再向所有存活客户端广播**全量**快照 (空表也发 = 清空); 无客户端时直接忽略
- 握尾 (`handleHello` 尾) 与 `switchSession` 后各补推一次; `stopImpl` 解绑 sink
- `handleToolRenderRequest`: io 线程取出调用凭据 (`findToolRenderCallable`),
  未登记则当场回 `ok=false`; 命中则 `co_spawn` + `offloadAsync` 到
  `AgentContext::threadPool` 执行回调 (无线程池的嵌入环境当场算), 结果回给发起请求的客户端
- 新增 `agent/lib/{include,src}/agent/tool_render_bridge.{h,cpp}`: 快照构造、
  凭据取出 (`ToolRenderCallable`: 回调 + user_data + 实例强引用 + 是否折叠) 与
  回调执行 (输入输出各 256 KiB 上限、折叠文本归一成一行、展开体必须是非空 JSON 数组)

**4. 客户端**
- `ClientUiRegistry` 新增 `remoteToolRenderers` (`ClientRemoteToolRenderer`: 工具名 /
  显示名 / 两个"有没有回调"标志 / owner, **没有回调指针**)
- `ClientPluginManager`: `setRemoteRenderSender` / `applyRemoteToolRenderers` /
  `applyRemoteToolRenderResult` + 等待表 `pendingRemoteRenders_` (每个请求一条
  `steady_timer`, 3 秒超时按"没内容"收尾) + `ClientToolRenderCache::invalidateRemote()`
- `renderClientToolCollapsed` / `renderClientToolExpanded`: 本地插件项、内置项都没命中时
  查远程项 → 同样 `pending = true` (UI 侧行为不变: 先上一份内容过渡, 再请求)
- `performToolRender`: 本地/内置都没命中 → 远程分支发请求并**保留** in-flight 标记
  (改 `PendingGuard` 加 `disarm`), 回包/超时/快照变化时收尾并通知 UI 重绘
- `toolDisplayNameFor`: 名字优先级 实例装饰 > 本地插件 > 内置 > 远程
- 客户端端点: `TUIClientAgentIO::onPeerMessage` 处理两条新消息; `mode_runners.cpp`
  的 `setupClientPlugins` 注入发送通道 (端点 `sendToPeer`, 本地/远程同一套)

**5. JS SDK**
- `agentxx.registerToolRenderer({tool, displayName, collapsed, expanded})` /
  `agentxx.unregisterToolRenderer(toolName)`
- 每个部分一个 token (脚本函数按 token 存在实例上下文里); 主程序回调经
  `JsToolRenderEntry` (任意线程查表校验, 注销后置 dead 不删, 引擎析构时统一释放)
- 回调桥 `postSyncFor(..., 200ms)`: 带超时地把活儿投给 JS 线程, 超时后任务被丢弃;
  任务只经 `shared_ptr` 槽交接数据, 不捕获调用方栈上的引用
- 脚本拿到的 `input` = `{toolCallId, toolName, argsJson, args, result, isFinished, isError, maxWidth}`
  (`args` 已解析成对象); 返回字符串 = 折叠内容, 返回数组/对象 = 展开体 items
- `example_js/plugin.js` 新增工具 10 `js_render_demo` + 对应 `registerToolRenderer`

**6. 测试与文档**
- `test_plugins.cpp`: 新增 16d 段 (登记项存在 / 折叠内容 `echo hi` / 展开体是非空数组 /
  快照里能看到该工具), `plugins` 649 → 660
- `test_client_plugins.cpp`: 新增第 17 段——远程往返的自动化验证 (快照 → UI 查询 pending →
  请求经发送通道发出且 `part`/工具名正确 → 结果写回缓存并命中 → 快照清空后回退默认渲染),
  `client_plugins` 774 → 788
- `test_boundaries.cpp`: 接口表数量 20→21、四件套合计 26→27、成员顺序清单加一行
- `test_plugin_host_mount.cpp`: 可查表数量与 IID 清单加一项
- `test_wire_schema.cpp`: 补三条消息的示例实例 (变体数 / 示例数必须一致)
- 文档: `plugins.md` §8.1 新表一行 + §9 "工具特化渲染架构"第 4 条 (优先级与远程路径)
  + §12 JS 面从"未对齐"改为"已对齐"; `index.md` / `AGENTS.md` 接口表数量与清单

### 验证

| 模块 | 结果 |
|---|---|
| `plugins` (含新增 16d) | 660 / 0 |
| `boundaries` / `config_keys` | 19 / 217, 全 0 失败 |
| `client_plugins` / `ui_items` / `tui_ui_items` / `tui_form` / `tui_widget` / `view_replica` | 788 / 117 / 277 / 84 / 208 / 114, 全 0 失败 |
| `plugin_runtime` / `plugin_sdk` / `plugin_bridge` / `plugin_cleanup` / `plugin_multi_mount` | 730 / 133 / 193 / 90 / 47, 全 0 失败 |
| `plugin_host_mount` / `plugin_dispatch` / `plugin_feature` / `plugin_point` / `plugin_broadcast` / `plugin_agent_runtime` | 147 / 112 / 34 / 68 / 90 / 221, 全 0 失败 |
| `async_work` / `async_tools` / `agent` / `event_subagent` / `event_tools` | 138 / 178 / 223 / 52 / 97, 全 0 失败 |
| `wire_schema` | 1484 / 0 (含生成物逐字节比对) |

构建: 内层 lib (`agentxx_static` + `--install`)、插件 (`agentxx_javascript_engine` /
`example_js`)、客户端 (`--target agentxx`)、测试 (`agentxx_test`) 全部通过。

### 遇到的问题

1. **回调在哪个线程跑**: 挂在 io 线程会阻塞会话推进 (客户端在等结果), 放工作线程又
   有卸载竞态。做法是 io 线程只取"凭据" (回调 + user_data + 实例强引用), 工作线程执行
   时用 `PluginInstanceBase::InflightGuard` 持租约 —— 卸载路径会等它结束, dlclose 不会
   越过执行中的插件代码。
2. **登记项什么时候能删**: 注销时不能直接 `delete` (另一个线程可能正拿着这个
   `user_data`)。改成置 `dead` 标记并保留到引擎/挂载析构; 渲染回调先查表, 查不到就按
   "没内容"返回。
3. `hostMemoryCreateString` 不在插件 SDK 头 (`pluginxx/kit/kit.h`) 里 —— 插件侧要用
   `agentxx::plugin::PluginString::set(host, &out, text)` (经 `host->vtable->alloc`)。
4. `findRemoteToolRenderReg` 定义在 `client_plugin_manager.cpp` 后半段, 而
   `performToolRender` 在前 —— 需要前置声明 (匿名命名空间内)。
5. `for (auto it = ...; ...) { it = erase(it); JS_FreeValue(ctx, it->second); }` 第一版用了
   已失效的迭代器, 改成先收集 token 再逐个释放。
6. `boundaries` 有三处硬编码数量 (常量期望 20 / 合计 26 / 成员清单), 改表后要一起改;
   `plugins.md`、`index.md`、`AGENTS.md` 必须出现 `21 张 agent` 字样。

### 没做的 / 需要知道的

- **agent 侧不另设超时**: 客户端 3 秒超时会先收尾, 插件回调跑完再回的结果按"请求不存在"
  丢弃 (日志记一条)。计划里的 `tool_render.timeout_ms` 配置键没加 —— 真要限制单个回调
  的时间, 应做成“取消执行”而不是再来一次超时收尾。
- **CLI 不消费远程渲染器**: 行式输出没有工具行, 因此只在 TUI 接线; 端点收到这两条消息
  也不会报错 (基类忽略未知分支)。
- **未加界面级自动化**: 现有断言覆盖注册表、凭据、回调输出、客户端往返 (快照→请求→结果→缓存)
  与 UI 查询语义; "真起一个 TUI 看工具行内容"仍靠手工验证 (渲染路径本身已有
  `tui_widget` / `tui_ui_items` 覆盖)。
- **插件同时两侧注册同一工具名**: 以本进程 (client 插件的) 注册项为准, 远程项只当后备。

