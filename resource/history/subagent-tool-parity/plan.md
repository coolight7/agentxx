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

## 8. 结论: 不做"工具特化渲染跨端"

曾计划过方案 B —— agent 侧新增 `agentxx.agent.tool_render` 接口表, 把渲染器登记搬到
agent 侧, 客户端需要内容时经 wire 回问 (解决"JS 脚本注册不了 client 侧渲染器")。
该方案已实现并验证通过, 之后按设计判断**整体回退** (`git revert 5f5e8a0a`)。

### 8.1 判断依据

- 工具行是客户端的东西: 显示名在注册时确定、内容在客户端算; 注册面只应有一处
  (`agentxx.client.ui` 的 `register_tool_renderer`)。
- 跨端方案为绕开一个缺口付出的代价不小: 多一张接口表 (agent 侧 21 张)、三条 wire 消息、
  本地直连也要走一次往返、远程条目要跟着快照做缓存失效、同名工具还要再定一条优先规则。
- 缺口本身 ("JS 引擎只有 agent 侧入口") 应当从**引擎侧**解决, 而不是让 agent 承担客户端的职责。

### 8.2 正解方向

**让 JS 脚本也能作为 client 侧插件运行** (client 侧 JS 引擎):

- 引擎复用现有 `agentxx_javascript_engine` 的脚本执行与桥核心, 增加 client 侧入口
  (`agentxx_plugin_client_create/start/stop/destroy`), 由 `ClientPluginManager` 加载;
- 脚本侧的 SDK 表换成 client 侧那套 (`agentxx.client.ui` 等), 因此
  `registerToolRenderer` 与 C++ client 插件完全同义、同线程语义;
- 一客户端一份实例 (客户端只有一个界面, 不像 agent 侧"一脚本 × 一 agent 挂载");
- 回调仍在 client IO 线程同步执行, 脚本侧要保证尽快返回 (与 C++ 插件同一约定)。

### 8.3 在那之前的过渡

需要工具行特化渲染的 JS 插件, 把渲染器写在它的 C++ 壳的 client 入口里 (壳声明
`interfaces` 含 `agentxx.client.ui`), 脚本只负责工具逻辑与数据。