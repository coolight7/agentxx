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

- JS SDK 与 C++ 侧仍不对齐的面: `net` (网络)、`runtime` (自建根/会话通道)、
  `async work`、`graph` (改图/节点类型)、工具特化渲染、`scheduler`/`tasks` 的协程驱动。
  这些目前必须写在 C++ 壳里; 后续按需逐表补齐。
- 未加多 agent 视角的 JS 自动化用例 (`plugins` 模块覆盖了单 agent 的 load/disable/enable
  全流程; 多挂载由 `ScopeHandle` + `scopes_` 逻辑保证, 建议后续在 `plugin_multi_mount`
  里加一条 JS 插件双 agent 用例)。
- 每个子代理多一次 N 个插件 mount (纯注册) 与 MCP 重新握手; 只在 `agentxx_subagent`
  路径发生, 压缩/内部路径不受影响。需要时可用性能基准测一轮 spawn 增量。
