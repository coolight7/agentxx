# 实施记录：插件宿主与多 agent 挂载（plugin-multi-agentnode）

> 依据同目录 `index.md` / `01-analysis.md` / `02-host-mount-design.md` /
> `03-plugin-side.md` / `04-implementation.md` 实施。
> 本文只记录**实际做了什么、还剩什么、要注意什么**（设计理由见那五篇）。

## 0. 状态总览

| 阶段 | 内容 | 状态 |
|---|---|---|
| A | 契约基础（新类型/新表/新入口/版本基线/删旧分支） | **基本完成**（见 §1.1） |
| C | agent 引用显式化（逐表加 `PluginxxScopeRef` 参数） | **契约与宿主完成，插件侧未迁** |
| B | 宿主化与挂载（per-host 管理器 + `PluginMount`） | **未完成**（见 §3） |
| D | 宿主接线与配置（`AgentHost`、五处启动路径、`plugin.mount`） | 未开始 |
| E | SDK 与示例（`AgentRef` / `MountStore` / 示例插件 / 端到端测试） | SDK 完成、示例/测试未做 |
| F | 会话定位与诊断 | 未开始 |
| G | 文档清理与最终核对 | 未开始 |

**当前代码树状态：不可编译。** 原因见 §3「为什么停在这里」——本次是破坏性 ABI
重构，签名改动一次性波及 22 个插件与 10+ 测试模块，中间态无法保持可编译。

**下一步第一件事**：按 §3 的顺序继续（先插件迁移 → 测试模块 → 宿主上提），
否则编译不通过。

## 1. 已完成

### 1.1 契约换代到 v2（`agent/third_party/cxx_pluginxx`）

| 项 | 做法 |
|---|---|
| `PLUGINXX_API_VERSION` / `MIN_API_VERSION` | 1 → **2**（v1 插件在加载阶段被拒） |
| `PluginxxScopeRef` | 新增（`struct_size` / `flags` / `id` / `seq` + 基线 + `pluginxx_scope_ref_ok`） |
| `integ PluginxxBuiltinInfo` | 尾部追加 `mount` / `unmount`（`void*`，主程序解释），供内置插件给出挂载入口 |
| `pluginxx.events.subscribe` | 插入 `const PluginxxScopeRef* agent`（可空 = 宿主总线），VERSION/MIN 2，基线 = 新 `sizeof` |
| `pluginxx.capabilities.invoke_capability_async` | 同上（agent 可空，仅归因） |
| `pluginxx.config.get_session_work_dir` | 加 `agent`，VERSION/MIN 2 |
| `pluginxx.cancel` | `is_cancelled` 加 `agent`；表尾追加 `op_cancel`；VERSION/MIN 2 |
| 其余 6 张通用表 | 未动（仍 v1） |
| IID | **一个都没改**（按 `02` §10.3） |

框架宿主侧同步改动：

- `pluginxx/host/domain_hooks.h`：`eventSource(agent)` / `sessionWorkDir(agent, sid)` /
  `isSessionCancelled(agent, sid)` 三个入口带引用；
- `pluginxx/host/host_core.h`：`subscribe(inst, agent, topic, handler, ud)`、
  `invokeCapabilityAsync(caller, agent, ...)`；
- `pluginxx/host/tables_impl.h`：四张表的 ABI 入口透传引用（跨线程投递前复制成自有结构）；
  `cancel` 表补 `op_cancel`；
- `pluginxx/host/event_bus.h`：`PluginxxSubscription` 增加 `agentId`（挂载卸载时按来源筛）；
- `pluginxx/kit/kit.h`：新增 `ScopeRef` 值类型（拥有 id+seq，`raw()` 给借用视图）；
  `OpCtl` 增加 `agent` 字段；`PluginBaseT::workDir(agent, tid)` /
  `sessionCancelled(agent, tid)`；`invoke_cap` 增加带引用重载；
  `invoke_capability_blocking` 增加 `agent` 参数。

### 1.2 agent 业务契约（`agent/lib/include/agentxx/plugin/api`）

- `plugin_api.h`：10 张 `agentxx.agent.*` 表全部加 `agent` 参数并升 VERSION/MIN 到 2；
  `tools` / `feature` 删 `op_cancel`（统一用 `pluginxx.cancel`）；
  `hooks` 合并为三个句柄式成员（`register_hook(host, agent, spec_ex, out_handle)` /
  `unregister_hook(host, handle)` / `list_hooks(host, agent, out)`）；
  **新增 `agentxx.agent.mount` 表**（`list` / `get`，v1）；
  新增 `AgentxxPluginAgentMountSpec`、`AgentxxPluginAgentMountFn` /
  `AgentxxPluginAgentUnmountFn`、`AGENTXX_PLUGIN_ERR_NO_AGENT (-2)`、
  入口符号 `agentxx_plugin_agent_{mount,unmount}`。
- `plugin_iface_meta.h`：补 `AgentxxPluginMountIface` 特化。
- `plugin_spec.h`：补 `AgentxxPluginAgentMountSpec` 基线。
- `plugin_kit.h`：
  - `AgentIfaces` 增加 `mount`；
  - 新增 `AgentRef`（`ScopeRef` 派生，可从挂载入参构造）与 `MountStore<T>`；
  - `listMounts()` / `mountInfoOf()`；
  - `tool` / `fast_tool` / `blocking_tool` / `polled_tool` / `hook` / `hookEx` /
    `graph_node` / 权限 helper / `call_tool` / `callFeature` / `defineFeaturePoint` 等
    全部改为带 `const ScopeRef& agent`；
  - `PluginBase::addShareStore` / `getSessionMessages` / `sessionMessagesCount` 带引用；
  - 导出宏统一 **8 参数**（`AGENTXX_PLUGIN_AGENT_EXPORT(..., Start, Stop, Mount, Unmount)`、
    `AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(Ctx, Start, Stop, Mount, Unmount)`），
    新增 `AGENTXX_PLUGIN_AGENT_MOUNT_EXPORT` 生成 mount/unmount 入口。
- `client_plugin_api.h`：`AGENTXX_CLIENT_PLUGIN_API_VERSION` / MIN 提到 2（布局未变）。

### 1.3 宿主侧

- `PluginInstance`：删 `PromptBackup`；`HookRegistration` 去掉 `base` 标志（句柄式唯一）；
  新增 `mountFn` / `unmountFn` / `mountUserData` / `pluginMounted` / `mountSeq`。
- `PluginManager`：`kInterfaceTableCount` = 10 + 11；新增
  `agentId()` / `mountSequence()` / `hasMount()` / `mountOf(ref)` /
  `mountAgentAsync()` / `unmountAgentAsync()` / `listMountsJson()` / `mountInfoJson()`；
  `registerHook` / `unregisterHook` 合并为句柄式；`restorePromptBackup` 删除
  （改用 `removePromptContributions`）；覆写 `onInstanceLoaded` 解析
  mount/unmount 入口（动态库按符号名、内置插件按 `PluginxxBuiltinInfo`）；
  `plugin_system` 步骤内 `agentId_` 暂固定 `"root"`。
- `plugin_manager_vtable.cpp`：新签名 + 引用校验（`scopeOk` / `onScopedInstanceIo` /
  `queryScopedStringIo`）；`tools`/`hooks` 表结构体初始化同步；新增挂载表入口与分发。
- `plugin_manager_hooks.cpp`：基础/扩展双入口合并为单一 `registerHook`。
- `plugin_manager_lifecycle.cpp`：删 `restorePromptBackup` 调用，改用贡献模型；
  实现 `mountOf` / `mountAgentAsync` / `unmountAgentAsync` / 挂载摘要 JSON /
  `onInstanceLoaded`。
- `plugin_manager_domain_hooks.cpp`：三个 `DomainHooks` 入口带引用并在引用无效时失败。
- `base_agent.cpp`：init 步骤 `plugins_load` 改名 `plugin_mount`（加载 + `mountAgentAsync`），
  `engine_build` 的依赖同步。
- 内置插件清单生成（`agent/plugins/CMakeLists.txt`）：内置描述表补 `mount`/`unmount`；
  22 个插件子目录 `CMakeLists.txt` 补两个符号改名定义。

## 2. 待完成任务

### 2.1 插件迁移（阻塞编译，先做）

22 个插件全部要改（`agent/plugins/**`）：

1. 导出宏从 6 参数改成 8 参数（无 per-agent 状态传 `nullptr, nullptr`）；
2. 把 `start` 里的 agent 级注册搬到新的 `mount` 回调；
3. per-agent 状态存 `AgentRef`（`MountStore<T>` 或自有 map）；
4. 所有 agent 级调用补 `st->ref`；
5. 钩子改句柄式（`hook` / `hookEx` + `unhook`）；
6. `op_cancel` 从 `iface.tools` / `iface.feature` 改到 `iface.cancel`。

要点：`fast_tool` / `blocking_tool` / `polled_tool` 的 `ScopeRef` 是**第一个**参数
（`ctx` 之后）；`hook(ctx, ref, point, fn)`；`graph_node(ctx, ref, type, schema, fn)`。

### 2.2 测试模块（阻塞编译）

| 模块 | 要做 |
|---|---|
| `test_plugin_hooks.cpp` | `registerHookEx`/`unregisterHookEx`/按点 `unregisterHook` → 统一句柄式 |
| `test_plugin_runtime.cpp` | 删 `restorePromptBackup` 用例；接口表数量 21；各表长度/版本断言按 v2 基线 |
| `test_plugin_resources.cpp` / `test_plugins.cpp` | 各接口表调用补 `agent` 引用（用 `mgr->agentId()` 造） |
| `test_boundaries.cpp` | 逐表四件套 + 成员顺序台账按新布局更新；表数量 21 |
| `test_client_plugins.cpp` | client API 版本 2 |
| 新增 | `plugin_host_mount`（14 例）与 `plugin_multi_mount`（端到端） |

测试里构造引用的推荐做法：

```cpp
PluginxxScopeRef ref{};
ref.struct_size = sizeof(PluginxxScopeRef);
ref.id          = PluginStringView::from(mgr->agentId());
ref.seq         = mgr->mountSequence();   // 未挂载时为 0 = 不校验
```

### 2.3 宿主上提（阶段 B，本次未做）

当前粒度是**一份 `PluginManager` 服务一个 agent**（`agentId_` 固定 `"root"`，
`mountOf` 只认本管理器的引用）。要做成"进程一份宿主 + 多挂载"，需要：

1. 新建 `agentxx/plugin/plugin_mount.h`（`PluginMount` + `Entry`）；
2. 把 `PluginManager` 的 agent 级状态整体搬到 `PluginMount`：
   `hookHandlers_` / `hookDispatch_` / `hookStats_` / `hookSeq_` / `nextHookHandle_`、
   `promptKeys_` / `promptSequence_`、`graphTypeSlots_` / `graphDefinition*`、
   `runningTurns_` / `pendingCleanups_`、`registry_`；
   以及 `PluginInstance` 上的 `toolNames` / `permissionToolNames` /
   `hookRegistrations` / `graphNodeTypes` / `featurePoints` / `featurePointImpls`
   → `PluginMount::Entry`；
3. `PluginManager` 只留实例表 + 挂载表（`vector<shared_ptr<PluginMount>>`）+
   vtable 分发 + `DomainHooks`（按引用转发给对应挂载）；
4. `mountOf()` 从挂载表里按 `id` 查，再校验 `seq`；
5. `disable` = 所有挂载 unmount（实例不 stop）；`enable` = 重新 mount；
   `unload` = unmount 全部 → stop → destroy → dlclose；
6. `AgentContext` 增加 `pluginMount` 字段，`base_agent.cpp` / `assembly_snapshot.cpp` /
   `agent_host.cpp` 的调用点改指挂载。

### 2.4 其余阶段

- 阶段 D：`AgentHost::Config::pluginList` / `pluginHost()` / `loadPluginsAsync()`、
  删 `attachRootAgentHost` 与五处启动路径改造；`AgentConfig::plugins` 改名
  `pluginLoadList`（注意 `config_loader.cpp` 与 `config_keys` 测试）；
  `plugin.mount` 解析（`all` / `none` / 列表 + 参数覆盖），子代理设 `None`。
- 阶段 F：同一存储根被两个 agent 打开时拒绝；`list()` 加挂载字段；加载快照加
  `plugins_agent` 段；日志补 `agent_id` / `seq`。
- 阶段 G：`docs/zh-cn/design/plugins.md` §2/§8/§17/§18、`index.md` §6/§7、根 `AGENTS.md`。

## 3. 为什么停在这里（重要）

本次重构是**破坏性 ABI 换代**：表结构体新增了 `agent` 参数位置，C 函数指针的
类型随之改变。只要插件源码不跟着改，编译一定失败；而 22 个插件 + 10 个测试模块
的迁移量远大于契约与宿主侧。

因此当前提交是一个**契约/宿主侧完成、插件与测试未迁**的中间态，**不可编译**。
继续做时按 §2.1 → §2.2 → §2.3 的顺序推进即可（§2.1/§2.2 是机械改动，
§2.3 才有设计取舍）。

## 4. 注意事项

- `agent/third_party/cxx_pluginxx` 改了头文件：构建前建议删掉
  `agent/build/windows-debug/cxx_pluginxx*`（或对应平台的依赖构建目录）让 cmake 重编；
  否则可能出现"改了头没生效"的假象。
- `pluginxx.kit.h` 的 `ScopeRef::raw()` 返回**借用视图**（指向自身 `std::string`），
  只能在本对象存活期间使用；不要把 `raw()` 的返回值存起来。
- `mountOf` 的 `seq == 0` 视为"不校验"，留给 FFI / 测试自行构造引用（正式插件一律
  用挂载时下发的 `AgentRef`）。
- 内置插件命中 `mount` 入口靠 `PluginxxBuiltinInfo::mount/unmount`，因此**每个内置
  插件子目录的 `CMakeLists.txt` 必须补两个符号改名定义**（已批量补上）；漏了会链接
  失败（`agentxx_plugin_agent_builtin_mount_<name>` 未定义）。
- 迁移插件时注意：`mount` 回调里**不能创建 root agent**（下一篇的约束 S5），当前
  未做该项校验。
