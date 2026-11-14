# 方案: 子代理环境继承 + JS 插件多挂载作用域

- 时间: 2026-11-14
- 代码依据: 主仓当前工作树 (行号指本次核对位置)

## 1. 背景与现状

### 1.1 子代理配置派生 (`AgentHost::makeSubagentConfig`, agent_host.cpp:934)

复制父配置后强制轻量化:

| 字段 | 现状 | 影响 |
|---|---|---|
| `pluginMountPolicy` | 强制 `None` | 插件不挂载 → 插件工具 (per-agent `ToolRegistry` 动态工具) 不存在 |
| `pluginMountList` | 清空 | 同上 |
| `mcpServerUrls` | 清空 | MCP 工具不存在 |
| `skillDirPaths` / `memoryFilePaths` / `ragDocsPaths` | 清空 | 提示词附加段缺失 |
| `plugins` | 清空 | 子代理不自己加载插件 (用进程共享管理器时本来也不加载) |
| `enableSessionStore` / `sessionStoreDirectory` | 关闭 | 一次性运行, 不落盘 |

工具装配顺序 (base_agent.cpp): `plugin_system` (建 per-agent `ToolRegistry`) → `middleware` →
`tools` (`initTools` + 中间件 toolcalls) → `tool_whitelist` (只过滤**静态**工具) →
`plugin_mount` (挂插件, 插件工具进 `toolRegistry`) → `engine_build` → `bind_tools`
(记录 `toolNames` = 静态工具名) 。

请求侧: 静态 `NodeContext.tools` + `toolRegistry->appendDefinitions` (modelcall.cpp:434) 一起下发给模型;
执行侧 `toolRegistry->find` 兜底 (toolcall.cpp:609)。`toolNames` 不含动态名 (base_agent.cpp:846) 。

### 1.2 `agentxx_subagent` 委派链

`SubagentManagerTool::execute_async` (subagent.cpp:311) → `requestInterrupt` →
`AgentRunner` 读中断参数 → `parseSubagentBatchFromInterrupt` (subagent.cpp:19) →
`AgentHost::spawnBatch` → `spawnOneTask` (agent_host.cpp:985) → `makeSubagentConfig`。

注意: `ToolcallWrapNode` 会给工具参数补 `sessionId` (toolcall.cpp:632), `parseTask` 读它
(subagent.cpp:344) ⇒ **单发调用默认走"同上下文模式"** (运行在父 session id 上, 共享父 share store 与模型);
`tasks` 批量模式才是独立 session。异步入口 `agentxx_async_run {tool:"agentxx_subagent"}`
经 `AsyncRunTool` 转成 `AsyncWorkSpec{kind=Agent}` (async_tools.cpp:503), 由 `AsyncWorkManager` 调
`spawnOneTask(..., spec)` 派生, 配置取 `spec->baseConfig` 快照。

### 1.3 JS 插件 (`agentxx_javascript_engine`)

- JS 插件 = C++ 薄壳 (start 经 `interpreter.js` 能力 `{op:"load", name, path}` 把 `plugin.js`
  交给引擎) + 脚本; 脚本顶层 `agentxx.registerTool(...)` 注册工具。
- 引擎只有一个"当前挂载作用域" (`setMountScope`, agentxx_javascript_engine.cpp:349; 注释自陈
  "再次 mount 会覆盖")。所有桥注册都用 `engine->agentRef()` 取这个唯一作用域
  (register_tool:1921 / subscribe:1779 / timer / resources:2268 ...)。
- 结论: 一旦同一进程里有第二个 agent (本次要做的"子代理挂同样插件"), JS 插件的注册会串到
  最后挂载的那个 agent 上, 必须重构。

## 2. 需求

1. `makeSubagentConfig` 增加显式参数, **默认值 = 现有轻量行为**。
2. 仅 `agentxx_subagent` 派生 (含经 `agentxx_async_run` 提交的同类任务) 要求
   插件/工具/MCP/skill/memory 与主会话**完全一致**。
3. JS 引擎支持"一脚本多 agent 挂载", 逻辑与 C++ 插件一致; JS SDK 向 C++ 侧对齐。

## 3. 方案: 子代理环境继承

### 3.1 新类型 `SubagentInherit` (新文件 `agentxx/agent/subagent_inherit.h`)

```cpp
struct SubagentInherit {
    bool plugins = false; ///< 插件挂载策略/名单/排除/挂载参数
    bool mcp     = false; ///< MCP server 清单
    bool skills  = false; ///< skill 目录
    bool memory  = false; ///< memory 文件
    bool rag     = false; ///< RAG 文档目录
    static SubagentInherit none();
    static SubagentInherit full();
    bool any() const;
};
```

放在独立头里 (`events.h` 与 `agent_host.h` 都要引用, 避免互相包含)。

### 3.2 传递路径

- `events::SubagentBatchItem` 追加 `SubagentInherit inherit{};`
- `AsyncWorkSpec` 追加 `SubagentInherit inherit{};` (异步委派)
- `SubAgentManagerTool::execute_async` 写中断参数 `"inherit": true` (对象或 `true` 均接受)
- `AsyncRunTool` (`tool == "agentxx_subagent"`) 写 `spec.inherit = SubagentInherit::full()`
- `parseSubagentBatchFromInterrupt` 解析回 `SubagentBatchItem::inherit`
- `spawnOneTask` 合成: `inherit = task.inherit | (spec ? spec->inherit : {})`

### 3.3 `makeSubagentConfig(parentConfig, const SubagentInherit& inherit = {})`

按开关逐项处理 (默认全 false ⇒ 与现在逐字一致):

- `plugins`: 保留 `pluginMountPolicy` / `pluginMountList` / `pluginMountExclude` / `pluginMountArgs`;
  否则按现状强制 `None` + 清列表。`plugins` 加载表仍然清空 (子代理用进程共享管理器, 不自己加载)。
- `mcp`: 保留 `mcpServerUrls`。
- `skills` / `memory` / `rag`: 保留对应路径列表。
- 其余不变 (`enableSessionStore=false`, worktree 绑定, subagent 模型)。

### 3.4 `spawnOneTask` 里的取舍

- `tools == []` 或 `toolExecutionDisabled == true` (`tools==[]` 的压缩路径) ⇒ 强制
  `inherit.plugins = false` (纯文本运行不付挂载成本)。
- 其它情况按 `inherit` 生效。
- 记日志: `spawnOneTask `x`: inherit = plugins/mcp/skills/memory/rag`。

### 3.5 工具集一致性 (两处必须补, 否则"一致"是假的)

1. **`toolNames` 收动态工具名** (`bind_tools`): 静态名 + `toolRegistry->names()` (去重)。
   否则 `tools:["*"]` 解析出的白名单永远缺插件工具。
2. **动态工具也受白名单约束**: `ToolRegistry` 加 `setNameFilter(std::optional<std::vector<std::string>>)`,
   在 `find` / `contains` / `appendDefinitions` / `names` 应用; `BaseAgent` 的 `tool_whitelist` 步骤
   在 `enableToolFiltering` 时注入。否则挂了插件之后 `tools:[]` / 自定义名单拦不住插件工具。

### 3.6 不改的部分

- `agents`/`agentxx.agent.runtime` 的 `mount_plugins` (插件自建根) 语义不变。
- 压缩子代理 (`tools: []` + `toolExecutionDisabled`) 行为不变。
- `subagentModel` (子代理模型) 语义不变。
- 不新增 yaml 配置键: 继承意图来自委派调用本身。

## 4. 方案: JS 引擎多挂载作用域

### 4.1 数据模型

- `scripts_`: `name -> {path, code, scriptHost}` (进程级脚本登记, `load` 时写入)
- `scopes_`: `scopeKey -> PluginxxScopeRef` (引擎自身 mount/unmount 维护, 有序)
- `plugins_` / `mirror_`: 键从 `name` 改成 **实例键** `name + '\x1f' + scopeKey`
- `JsPluginCtx` 追加 `PluginxxScopeRef scope`; 桥里所有 `engine->agentRef()` 改成
  `pctx->scopeRef()`
- `JsToolBinding` / `JsHookBinding` / `timers_` 记实例键, 执行/回调按实例键找 ctx

### 4.2 生命周期

- 引擎 `mount(scope)`: 记入 `scopes_`; 对每个已登记脚本 → 投递 JS 线程创建实例 (执行脚本,
  注册归属该 scope)
- 引擎 `unmount(scope)`: 移出 `scopes_`; 对每个脚本 → 销毁该 scope 的实例
  (主程序已按挂载撤销注册, 引擎侧只释放 JSContext/定时器)
- `load` 能力: 登记脚本 + (每个当前 scope 建实例); 完成 payload 合并各实例的工具名
- `unload` 能力: 逐 scope 显式注销 (脚本先于 agent 卸载时) + 销毁实例
- 停止: 全部实例随 JS 线程退出释放 (既有路径)

### 4.3 JS 侧语义 (与 C++ 对齐)

- 脚本顶层 = "该 agent 的插件实例初始化" (每个 agent 一份独立 JSContext, 全局状态天然按 agent 隔离)
- 注册类 API 只能在挂载实例里调用 (顶层即可); 进程级初始化应放在 C++ 薄壳
- 新增 `agentxx.scope()` → `{agentId, mountSeq}`; 新增与 C++ kit 对应的权限/配置 API:
  `registerToolPermission` / `registerReadPathPermission` / `registerWritePathPermission`,
  `getConfig` / `getPluginArgs`
- 已有 `registerTool` / `unregisterTool` / `callTool` / `subscribe` / `publish` / `log` /
  `emitMessageTip` / `getShareStore` / `setTimeout` / `clearTimeout` / `addSkillDir` 等保留

### 4.4 兼容

- C++ 薄壳 (start=load / stop=unload) 不需要改动: 每 agent 的实例由引擎按自身 mount 派生。
- `tools` payload 从"单实例工具名"变为"各实例工具名并集", 调用方仅用于日志。

## 5. 分步实施

1. 新增 `subagent_inherit.h`; `events.h` / `async_work.h` / `agent_host.h` 追加字段与签名
2. `makeSubagentConfig` 按开关实现; `spawnOneTask` 合成 inherit + 无工具时禁用 plugins
3. `subagent.cpp` (工具侧) + `async_tools.cpp` 写入 inherit; `parseSubagentBatchFromInterrupt` 解析
4. `ToolRegistry` 名称过滤 + `base_agent.cpp` 注入与 `toolNames` 收动态名
5. 工具提示词 (`prompt.cpp`) 更新 `tools` 缺省语义说明
6. 测试: `test_host_spawn_tool_policy` 增补 (缺省/继承/[] 三态, 有插件时断言动态工具)
7. JS 引擎多作用域重构 + 示例脚本/薄壳适配
8. 测试: JS 插件挂两个 agent, 各自注册、单独卸载互不影响
9. 构建 + 相关测试模块全跑; 文档更新 (`plugins.md` / `AGENTS.md` 摘要)

## 6. 验证

- `agentxx_test agent_host plugin_multi_mount plugin_runtime plugin_sdk plugin_cleanup`
- 新增用例 (见 5.6 / 5.8)
- 编译: windows debug 构建脚本

## 7. 风险

- 每个子代理多一次 N 个插件 mount (纯注册; 但 JS 插件会多建 JSContext) — 只在
  `agentxx_subagent` 路径发生, 压缩/内部路径不受影响
- MCP 继承 = 每个子代理重新握手; 保持开关可关 (调用方不写 inherit 即关闭)
- JS 引擎重构触及桥的所有注册路径, 需全量跑插件类测试

---

## 8. 方案 B: 工具特化渲染跨端 (agent 侧插件与 JS 脚本也能特化渲染)

### 8.1 现状 (代码级)

- 注册面在 **client 侧**: `AgentxxToolRenderSpec` 与 `register_tool_renderer` 属于
  `agentxx.client.ui` (`plugin/api/client_plugin_api.h`), `ClientPluginManager::registerToolRenderer`
  写 `ClientUiRegistry::toolRenderers`。
- 显示名: 注册时归一 (`displayName` 空 → 工具名), UI 线程经 `toolDisplayNameFor` 同步读, 首帧就能显示。
- 内容: `message_list.cpp::queryToolCollapsed/Expanded` → `renderClientToolCollapsed/Expanded`
  只读 `ClientToolRenderCache`; 未命中时 `toolRenderInFlight` 去重后 `requestToolRender()`
  → `postToIo` → **client io 线程** 的 `performToolRender()` 调 `collapsedFn`/`expandedFn`
  → 写入缓存 → `PluginUiAdapter::onToolRenderUpdated` 通知 UI 重绘。
- 因此 agent 侧插件 (C++ 与 JS 脚本) 今天完全够不到这套: 既没有 agent 侧注册表,
  也没有把"算内容"的请求送回 agent 的通道。

### 8.2 目标 / 非目标

目标:

- agent 侧插件 (C++ 与 JS 脚本) 能注册工具特化渲染: 显示名 + 折叠内容 + 展开内容;
  **远程 TUI 与同进程 TUI 走同一条路**。
- 复用现有的 pending / 语义缓存 / 重绘机制, 不改 `update_tool_decor` 的优先级语义。
- 注册跟着挂载 (per-agent) 生命周期走, 禁用/卸载自动撤销。

非目标:

- 客户端插件本地的渲染器路径不变 (本地注册优先, 行为不动)。
- 不做流式/增量渲染: 一次请求给一份完整内容。
- 不在 UI 线程执行任何回调 (保持现状)。

### 8.3 数据结构

**(1) agent 侧接口表 `agentxx.agent.tool_render` v1** (`plugin/api/plugin_api.h`)

```c
typedef struct AgentxxPluginToolRenderInput {
    uint32_t struct_size; int32_t _reserved;
    PluginxxStringView tool_call_id;
    PluginxxStringView tool_name;
    PluginxxStringView args_json;
    PluginxxStringView result_text;
    int32_t is_finished;
    int32_t is_error;
    int32_t max_width;     // 终端列宽 (GUI 为逻辑像素)
} AgentxxPluginToolRenderInput;

typedef struct AgentxxPluginToolRenderOutput {
    uint32_t struct_size; int32_t _reserved;
    PluginxxString text;        // 折叠内容 (一行纯文本; 空 = 这次没有自定义内容)
    PluginxxString items_json;  // 展开内容 (界面描述层 items 数组; 空 = 没有)
} AgentxxPluginToolRenderOutput;

typedef int32_t(PLUGINXX_CALL* AgentxxPluginToolRenderFn)(
    void* ud, const AgentxxPluginToolRenderInput*, AgentxxPluginToolRenderOutput*);

typedef struct AgentxxPluginToolRenderSpec {
    uint32_t struct_size; int32_t version;   // version = 1
    PluginxxStringView tool_name;
    PluginxxStringView display_name;         // 空 = 用工具名
    AgentxxPluginToolRenderFn collapsed;     // 可空
    AgentxxPluginToolRenderFn expanded;      // 可空
    void* user_data;
} AgentxxPluginToolRenderSpec;

typedef struct AgentxxPluginToolRenderIface {
    int32_t version; uint32_t struct_size;
    int32_t (*register_tool_renderer)(host, const PluginxxScopeRef* agent, const Spec*);
    int32_t (*unregister_tool_renderer)(host, const PluginxxScopeRef* agent, const PluginxxStringView* tool_name);
} AgentxxPluginToolRenderIface;
```

- 归属 = 调用方的挂载; 禁用/卸载由主程序按挂载记录撤销, JS 引擎实例销毁也走同一条。
- 同 `tool_name` 已被**别的插件**注册 → `ERR_CONFLICT`; 自己重复注册 = 覆盖自己的项 (幂等)。
- 回调**可在任意线程调用** (实现方内部要不要阻塞自己决定; 主程序不会在 io 线程直接调,
  见 8.4)。 JS 引擎在回调里 postSync 到 JS 线程并带预算。

**(2) wire 消息** (`agent/io/agent_io_transport.h` + `wire_protocol.h` + `src/agent/wire_protocol.cpp`)

| 方向 | type | 字段 |
|---|---|---|
| agent → client | `tool_renderer_register` | `sessionId`, `renderers:[{toolName, displayName, collapsed:bool, expanded:bool, owner}]` (全量快照; 空数组 = 清空) |
| client → agent | `tool_render_request` | `requestId`, `sessionId`, `toolCallId`, `toolName`, `part` (`collapsed`/`expanded`), `inputHash`, `argsJson`, `resultText`, `isFinished`, `isError`, `maxWidth` |
| agent → client | `tool_render_result` | `requestId`, `ok`, `text`, `itemsJson`, `errorMessage` |

未知消息类型在 WS 传输里已经"解析失败即跳过"(`ws_io_transport` 的 `deserialize` 返回 nullopt → `continue`),
所以新增消息对旧端是安全降级, 不改 `WireProtocol::kVersion`。

**(3) client 侧注册表与缓存** (`plugin/client_plugin_manager.h`)

- `ClientToolRenderReg` 追加: `bool remote` / `bool hasCollapsed` / `bool hasExpanded`
  (remote 项两个回调为空, 只表达"这个名字有远程渲染器")。
- `ClientUiRegistry` 新增 `remoteToolRenderers` 表; 查找顺序
  **实例装饰 > 本地插件注册 > 远程注册 > 内置** (同 toolName 本地赢: 那是用户装的客户端插件)。
- 远程项缓存未命中 → `pending = true` 且新增 `remote = true`, UI 侧照旧提交请求。

### 8.4 线程、超时、失败回退

- UI 线程: 只读注册表与缓存 (不变)。
- client io 线程 `performToolRender`:
  - 命中远程项 → 生成 `requestId` (进程内自增), 记 `pendingRemote[requestId] = {key, inputHash}`,
    投递 `tool_render_request`, **保持** in-flight 标记 (等结果);
  - 无 sender (FFI/嵌入式未接线) → 视同"无内容", 直接写 miss 条目, 避免永久 pending;
  - 超时 (默认 3000 ms, `ClientPluginManager` 内的一个 steady_timer) → 写 miss + `endRequest` + 重绘。
- agent 侧 (`SessionServerAgentIO` 或渲染 sink):
  - 收到 `tool_render_request` → 查该 agent 的渲染表 → **投递到工作线程池** 执行回调
    (不占 agent io 线程) → 回 `tool_render_result`;
  - 找不到渲染器 / 回调失败 / 超过 `tool_render.timeout_ms` (默认 2000) → `ok=false`;
  - 单次传输上限 256 KiB (args/result 或输出超限按"无内容"并记日志)。
- JS 引擎回调: worker/agent 线程上 `postSync` 到 JS 线程, 预算 200 ms;
  脚本函数返回字符串 (折叠) / 数组或对象 (展开, 序列化成 items JSON); 抛异常或超时 → 空内容。

### 8.5 注册下发与失效

- agent 侧注册表变化 (注册/注销/插件禁用/卸载) → 通知 `SessionServerAgentIO`
  (新增 `SessionToolRenderSink`, 与 `SessionInputSink` 同样弱引用挂在 `AgentContext` 上)
  → 向已连接的客户端推一次全量快照 (sessionId = 该客户端绑定的会话)。
- 客户端收到快照 → 替换 `remoteToolRenderers` (COW) + 对消失的 owner 调
  `ClientToolRenderCache::invalidatePlugin(owner)` 让旧内容立刻回退默认渲染。
- 客户端切会话/重连: 握手 (HelloAck) 后 agent 端再推一次快照; 客户端切会话时先清空远程表。

### 8.6 分步实施

1. **接口表 + agent 注册表**: `plugin_api.h` 新表四件套、`plugin_iface_meta.h`、`AgentIfaces`、
   `PluginManager` 的 per-mount 渲染表 (注册/注销/清理/快照/执行)、`boundaries` 自检更新。
2. **wire 消息**: struct + variant + `toJson`/`fromJson` + 反序列化表 + `wire-schema.json` 与
   `wire-protocol-fields.md` 重生成。
3. **agent 端点**: 渲染 sink (推快照) + `tool_render_request` 处理 (线程池 + 超时) + 握手后补推。
4. **client 端点与渲染分支**: 应用快照/结果的两个 helper (`plugin/client_tool_render_wire.h`)、
   `AgentTui`/`AgentStdio` 接线 + sender、`performToolRender` 远程分支 + 超时。
5. **JS 引擎 + SDK**: `registerToolRenderer` / `unregisterToolRenderer`, 回调桥 (postSync + 预算),
   `example_js` 用例 (`js_tool_render_probe` 之类)。
6. **测试 + 文档**: `plugins`/`client_plugins`/`tui_widget` 断言; `plugins.md` §9.6/§12、`AGENTS.md`、
   `work.md` 记录。

### 8.7 验证

- 远程路径: agent 侧注册渲染器 → 客户端收到快照 → UI 查询 pending → 请求往返 → 内容进缓存。
- 超时/失败: 渲染器抛异常、超时、无 sender 三种情况都回退默认渲染且不残留 pending。
- 回归: `plugins` / `plugin_*` / `client_plugins` / `tui_widget` / `tui_ui_items` / `boundaries`。

### 8.8 风险

- agent io 线程阻塞 → 强制走线程池 + 超时; JS 回调再加一层预算。
- 大文本跨线 → 256 KiB 上限 + 超限降级。
- 无 sender 环境 (FFI/嵌入式) → 直接 miss, 不排队等。
- 旧客户端/旧服务端 → 新消息被跳过, 行为等同"没有远程渲染器" (默认渲染), 不需要版本号变更。

### 8.9 实施结果 (2026-11-15)

已完成第 1~6 步, 与计划的差异:

- **agent 侧不另设超时**: `tool_render.timeout_ms` 配置键没加。客户端 3 秒超时会先收尾,
  插件回调跑完再回的结果按"请求不存在"丢弃 (记日志)。单个回调真卡死时应做成“取消执行”,
  而不是再来一次超时收尾。
- **回调线程**: 不在 io 线程、也不是随意线程 —— io 线程只取凭据, 回调在
  `AgentContext::threadPool` 上执行, 执行期间持插件实例租约 (`InflightGuard`), 因此不会
  被卸载打断。
- **客户端匹配优先级**: 实例装饰 > 本进程插件注册项 > 主程序内置渲染器 > 远程登记项。
  内置项排在远程之前, 是为了让主程序对内置工具的渲染不被 agent 侧同名登记盖掉。
- **登记项释放**: 注销只置 `dead` 并保留到挂载/引擎析构 —— 另一个线程可能正拿着
  `user_data` 在执行, 直接释放就是 use-after-free。
- **只接线 TUI 端点**: CLI 行式输出没有工具行, 不消费远程渲染器。

实施记录与验证结果见 `work.md` 的"第五步"。
