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
- [ ] 10. 后续项: JS SDK 剩余面 (net / runtime / async work / graph / 工具特化渲染)

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

- JS SDK 与 C++ 侧仍不对齐的面: `runtime` (自建根 / 会话通道 / 触发器 / 定时器)、
  工具特化渲染 (UI 线程回调)、`scheduler`/`tasks` 的协程驱动。这些目前必须写在 C++ 壳里;
  后续按需逐表补齐。(`net` / 后台任务 / 提示词 / 会话 / 上下文 / 执行图 / 挂载查询已补齐, 见下节)
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
- 工具特化渲染未做。

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
