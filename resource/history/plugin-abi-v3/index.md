# 插件 ABI v3：通用表去业务成员 + 框架用词泛化

## 1. 背景

`cxx_pluginxx` 是按独立工程维护的通用插件框架（多个主程序共用），
`cxx_utilxx_base` / `cxx_utilxx` 同理。上一轮已经把库里的项目专名、项目路径、
`worktree` 封装等迁回主程序；本轮继续处理"库里有主程序业务概念"这一类：

- 通用表 `pluginxx.config` 上挂着 `get_tool_prompt` / `get_session_work_dir`；
- 通用表 `pluginxx.cancel` 上挂着 `is_cancelled`（会话取消查询）；
- 通用表实现与 SDK 里到处是 `agent` 用词（参数名、`OpCtl::agent`、
  `RootRequest::sessionId`、`InvokeCapState::agentId` …）；
- 插件框架的清单资源结构体写死了 `skillDirs` / `memoryFiles` / `mcpServers`。

这些都是 agentxx 的业务概念（工具提示词、会话工作目录、会话取消、skill/memory/mcp），
不该由通用框架定义。用户批准把插件全局 API 版本从 2 抬到 3，允许破坏性改造。

## 2. 改动清单

### 2.1 框架侧（`agent/third_party/cxx_pluginxx`）

| 位置 | 改动 |
|---|---|
| `api/tables.h` | `pluginxx.config` 删 `get_tool_prompt` / `get_session_work_dir`；`pluginxx.cancel` 删 `is_cancelled`（只剩 `op_cancel`）；`config`/`cancel`/`events`/`capabilities` 的 `VERSION`/`MIN_VERSION` → 3；参数名 `agent` → `scope` |
| `host/domain_hooks.h` | 删 `toolPromptJson` / `sessionWorkDir` / `isSessionCancelled` 三个钩子（通用表不再需要主程序业务数据） |
| `host/tables_impl.h` | 删三个 vtable 入口与接线；参数改名 |
| `host/host_core.h` | 参数改名 + 日志文案 `(scope ref {})` |
| `kit/kit.h` | 删 `workDir()` / `sessionCancelled()` 便捷方法；`OpCtl` 精简为 `{cancelFlag, cancelRegistry, scopeKey, scope}`（去掉 `host`/`cancelIface` 与 `is_cancelled` 分支，取消查询由主程序业务表负责）；`RootRequest` 通用化：删 `forTool/forHook/forGraphNode` 与图节点专用字段，改 `extraFields` + `setExtra/extra`，`sessionId` → `scopeKey`、`session()` → `scope()`；`InvokeCapState`/`invoke_cap` 的 `agent*` → `scope*` |
| `api/abi.h` | **默认值不改**（库被多主程序共用）: `PLUGINXX_API_VERSION`/`PLUGINXX_MIN_API_VERSION` 仍为 1, 只调整注释说明“由主程序构建期宏覆盖” |
| `host/manifest.h` / `src/manifest.cpp` | `PluginManifestResources` 改成通用条目 `{type, path, argsJson}` 列表；解析 `resources:` 列表；旧顶层 `skill:`/`memory:`/`mcp:` 段归一成同 type 条目（老清单不用改）；新增 `yamlToJson()` |
| 其余文件 | 注释里"agent 侧 / client 侧"改为"各端 / 主程序两端 / 作用域级" |

### 2.2 主程序侧（agentxx）

- **业务成员迁入 agentxx 业务表**（表尾追加，`VERSION` → 3、`MIN_VERSION` → 3、
  基线随本代重置为完整布局）：
  - `agentxx.agent.session` += `get_session_work_dir(agent, session_id)`、`is_cancelled(agent, thread_id)`
  - `agentxx.agent.prompt` += `get_tool_prompt(tool_name)`
- `plugin_manager_vtable.cpp` 新增三个入口：
  - `xx_session_work_dir`（`queryScopedStringIo` + `PluginManager::sessionWorkDir`）
  - `xx_session_cancelled`（IO 线程 + `PluginManager::isSessionCancelled`，引用无效返回 0）
  - `xx_prompt_tool`（`queryStringIo` + `PluginManager::toolPromptJson`）
- `PluginManager` 里三个 `DomainHooks` 覆写改成普通业务查询方法（`sessionWorkDir` /
  `isSessionCancelled` / `toolPromptJson`），`getToolPromptJson` 保留为内部实现；
  原先主 agent 专用、已无调用方的 `getSessionWorkDir()` / `getSessionWorkDir(sessionId)`
  两个副本删除（工作目录只留 `sessionWorkDir(agent, sessionId)` 一条路径，避免两套算法走偏）。
- SDK（`agentxx/plugin/api/plugin_kit.h`）：
  - `PluginBase::toolPrompt` 改读 `iface.prompt->get_tool_prompt`；
  - 新增 `PluginBase::workDir(scope, key)` / `sessionCancelled(scope, key)`
    （后者保留"先查本地登记"的快速路径），插件源码调用点不变；
  - 新增 `detail::toolRootRequest/hookRootRequest/graphNodeRootRequest`，
    图节点字段写入 `extraFields`（键 `node_name` / `config_json` / `state_json`），
    公开读取入口 `nodeNameOf/nodeConfigOf/nodeStateOf`；
  - `OpCtl` 聚合初始化改 4 字段。
- 清单资源消费：`PluginMount::applyManifestResourcesNow` 按 `type` 分发
  （`skill` / `memory` / `mcp`，mcp 从 args JSON 取 `namespace`/`url`/`timeout`(秒)）；
  未知种类只记警告。`example_resources/plugin.yaml` 改用 `resources:` 写法。
- 版本下发：`agent/CMakeLists.txt` 新增选项 `AGENTXX_PLUGIN_API_VERSION`（当前 3），
  向全局 `CMAKE_C_FLAGS/CMAKE_CXX_FLAGS` 宏覆盖（先剔旧值再追加），并随 `CMAKE_ARGS`
  传给 `cxx_pluginxx` 嵌套构建（其 installed targets 的 INTERFACE 定义随之变成 3）；
  **库里的 `abi.h` 默认值仍为 1，不改库**；`AGENTXX_CLIENT_PLUGIN_API_VERSION` 同步到 3；
  `test_plugin_abi_c17.c` 的 `_Static_assert` 改 3。

### 2.3 测试与文档

- `test_boundaries.cpp` 成员顺序台账：`PluginxxConfigIface` / `PluginxxCancelIface` /
  `AgentxxPluginSessionIface` / `AgentxxPluginPromptIface` 四行更新。
- `test_plugin_bridge.cpp` 的假 `pluginxx.cancel` 表去掉 `is_cancelled` 槽位与假实现。
- `test_plugin_resources.cpp` 改为按条目断言，并新增旧写法兼容断言。
- `test_plugin_sdk.cpp` / `example_graph_node.cpp` 改用 `nodeNameOf/nodeConfigOf/
  nodeStateOf` 与 `req.scope()`。
- 文档：`docs/{zh,en}/design/plugins.md`（表清单、v3 换代说明、清单资源写法、
  worktree 一节）、`docs/{zh,en}/design/index.md`、`AGENTS.md`。

## 3. 踩到的坑

1. **构建缓存里的旧代次**：顶层 CMake 原来只在 `CMAKE_*_FLAGS` 里"没有
   `PLUGINXX_API_VERSION` 就追加"。上一代留下的 `-DPLUGINXX_API_VERSION=2` 常驻
   cache，条件永不成立，嵌套 ExternalProject（插件/客户端/测试）于是继续按 2 编译，
   主程序按 3 判版本 → 运行时"API version too old (got 2)"。现在改成**先剔除旧值
   再追加**，代次集中在 `AGENTXX_PLUGIN_API_VERSION` 一个选项上，幂等；
   **库代码里的默认值保持 1 不动**（通用库不跟随某一个主程序的契约）。
2. **`yamlToJson` 的初始化列表陷阱**：`utilxx_base::Json{true}` 会匹配
   `Json(std::initializer_list<Json>)`，把标量包成 `[true]`；必须用圆括号
   `Json(true)`。另外 yaml-cpp 对单标量节点也能按"长度 1 的序列"迭代，判断顺序必须
   先 `IsScalar()` 再 `IsSequence()`。
3. `OpCtl` 字段减少后，`spawn` 里的聚合初始化要同步改（框架内部一处）。

## 4. 验证

- Windows Debug（ASan）全量构建通过；插件与主程序同一代头文件重新编译。
- 全量测试 `agentxx_test`：38806 passed / 7 failed —— 失败项均为既有环境或用例问题
  （`console_state` 2 条需真实控制台；`screen_capture` 3 条、`text_selection` 2 条
  因用例未调用 `mountAgentAsync`）。
- 专项回归：`boundaries`(14) `plugin_runtime`(736) `plugin_sdk`(115)
  `plugin_bridge`(193) `plugin_host_mount`(146) `plugin_multi_mount`(47)
  `plugin_multi_instance`(80) `plugin_cleanup`(99) `plugin_feature`(145)
  `plugin_hooks`(160) `plugin_resources`(101) `plugins`(595, 新增 v3 追加成员用例)
  `client_plugins`(716) `util_misc`(236) `worktree`(25) 合计 3408 断言全过。
