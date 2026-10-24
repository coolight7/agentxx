# 实施记录：插件宿主与多 agent 挂载（plugin-multi-agentnode）

> 依据同目录 `index.md` / `01-analysis.md` / `02-host-mount-design.md` /
> `03-plugin-side.md` / `04-implementation.md` 实施。
> 本文只记录**实际做了什么、还剩什么、要注意什么**（设计理由见那五篇）。

## 0. 状态总览

| 阶段 | 内容 | 状态 |
|---|---|---|
| A | 契约基础（新类型/新表/新入口/版本基线/删旧分支） | 完成 |
| C | agent 引用显式化（逐表加 `PluginxxScopeRef` 参数） | 完成（契约/宿主/插件全迁） |
| E | SDK（`AgentRef` / `MountStore` / 导出宏） | 完成（示例插件已迁） |
| T | 测试模块与测试插件迁移（`agentxx_test`） | 完成（全部模块通过） |
| B | 宿主化与挂载（per-host 管理器 + `PluginMount`） | **未完成**（当前是「一份管理器服务一个 agent」） |
| D | 宿主接线与配置（`AgentHost`、五处启动路径、`plugin.mount`） | 未开始 |
| F | 会话定位与诊断 | 未开始 |
| G | 文档清理与最终核对 | 未开始 |

**当前代码树状态：lib + client + 21 个插件目标 + `agentxx_test` 全部编译通过；
插件相关测试模块与 `boundaries` / `config_keys` / `assembly_snapshot` 全绿。**

下一步：阶段 B（插件管理器上提到宿主、把 agent 级注册收进 `PluginMount`）→ D → F → G。

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

### 1.4 插件迁移（全部 21 个 Windows 目标编译通过）

`agent/plugins/**` 全部按"start 只做进程级工作 + mount 做 agent 级注册"改完，
逐个 target 编译通过（`agentxx_audio_stream` 在 Windows 不参与编译）：

| 插件 | mount 做的事 | per-agent 状态 |
|---|---|---|
| `agentxx_math` / `agentxx_string` / `agentxx_system` | 工具注册 | 无 |
| `agentxx_filesystem` | 9 个工具 + 权限声明；`makeReadPathFilter` / `destinationAllowed` 带引用 | 无 |
| `agentxx_execute_command` | 提示词注入 + 4 个工具；`storeFn` 捕获引用 | 无 |
| `agentxx_codegraph` | 5 个工具 + 提示词 + `client_attached` 订阅（索引预热留在 start，索引数据实例级） | 无 |
| `agentxx_planning` | 提示词 + 规划工具 + `client_attached` 订阅 | 无 |
| `agentxx_system_monitor` | 系统信息工具（能力/事件订阅/采样任务留在 start） | 无 |
| `agentxx_computer_use` / `agentxx_rag_search` / `agentxx_websearch` / `agentxx_llm_uncensored` / `agentxx_screen_capture` / `agentxx_text_selection_monitor` | 工具注册 | rag 用 `AgentRef` 存挂载状态 |
| `agentxx_javascript_engine` | `setMountScope(spec)`：之后脚本注册的工具/钩子/订阅都带该引用 | 引擎内 `mountAgentId_` + `mountRef_` |
| `example_*`（6 个） | 工具/钩子/功能点/节点类型/资源 | 无 |

同时改了 SDK（`agent/lib/include/agentxx/plugin/api/plugin_kit.h`）：

- 新增 `using pluginxx::ScopeRef;`（此前缺失，插件侧写不出 `ScopeRef`）；
- `provideFeature` / `unprovideFeature` / `listFeaturePoints` 增加 `const ScopeRef& agent`；
- `PluginBase::onHostReady` 的 `round_start` 订阅显式传 `nullptr`（基类不知道挂载引用，
  订阅主程序总线；会话事件一并到达）；
- 导出宏的空指针判定由 `if (!(MountFn))` 改为 `if ((MountFn) == nullptr)`
  （前者对函数名会触发 MSVC C4551 警告）。

**JS 引擎的已知限制**（记入 §2.3 后续项）：一份 JS 脚本目前只对应**一个**挂载作用域，
再次 mount 会覆盖引用；真正的"每 agent 一份 JS 脚本实例"需要把 `JsPluginCtx` 按挂载拆开。

### 1.5 宿主侧编译验证（lib + client）

`agentxx_static` 与 `agentxx_cli` 均已编译通过。修掉的问题：

- `PluginInstance` 补回 `uint64_t mountSeq`（`mountAgentAsync` 记录本次挂载序号）；
- `plugin_manager_vtable.cpp` 删除重复定义的 `xx_unregister_node_type`；
  `g_ifaceFeature` 去掉已删除的 `op_cancel` 项；
- `pluginxx/runtime/op_driver.h`（框架）：`awaitPluginLifecycle` 的 `if (!hook || ...)`
  对 lambda 非法（`!lambda` 无意义），改为只校验 `runtime`/`instance`；
  该模板要求 `HookFn` 形如 `(void* ctx, notify, err)`，`mountAgentAsync` /
  `unmountAgentAsync` 的 lambda 已补首个 `void* /*ctx*/` 参数。

**改了 `cxx_pluginxx` 或 `agent/lib/include` 的头后，两处 install 都要重跑**，否则
`agentxx_client_repo` / `agentxx_plugins_builtin` 用的是 `agentxx-project-install` 里的旧副本：

```powershell
cmake --build agent/build/windows-debug/cxx_pluginxx_repo-prefix/src/cxx_pluginxx_repo-build --target INSTALL
cmake --build agent/build/windows-debug/agentxx_lib_repo-prefix/src/agentxx_lib_repo-build --target INSTALL
```

单独编译某个插件（比整树快很多）：

```powershell
cmake --build agent/build/windows-debug/agentxx_lib_repo-prefix/src/agentxx_lib_repo-build/agentxx_plugins_builtin/<插件目录> --target <插件目录>
```

### 1.6 测试迁移与宿主挂载行为补齐

测试模块（`agent/test`）从「不可编译」迁到全绿，过程中把宿主侧的挂载/拆挂载行为
补成了真实可用的增量实现（原来只有「一次性挂载、不能重挂」）。

#### 测试侧

| 文件 | 改动 |
|---|---|
| `test_plugin_sdk.cpp` | `CapturedRegistration.hook` 改用 `AgentxxPluginHookSpecEx`; fake 表函数补 `agent` 参; 工具表去 `op_cancel`、hooks 表改三成员; kit 调用补 `g_agentRef` |
| `test_plugin_bridge.cpp` | `fakeIsCancelled(host, agent, sid)` / `fakeRegisterTool(host, agent, spec)`; `op_cancel` 从工具表挪到 cancel 表 |
| `test_plugin_runtime.cpp` | `LifecycleProbe` 记 `agentId` + `PluginxxScopeRef`; `restorePromptBackup` → `removePromptContributions(inst->name)`; 订阅/工作线程补引用 |
| `test_plugins.cpp` | 新增 `scopeRefOf(const PluginManager&)`; 工具/权限/能力/钩子调用逐处补引用 |
| `test_plugin_hooks.cpp` | 用例改为「同点多个独立句柄 + 优先级越界裁剪 + 逐句柄撤销」 |
| `test_plugin_abi_c17.c` | 断言全局与各表 API 版本 = 2 |
| 各 fixture | `setIoExecutor` 后补 `mountAgentAsync()`（未挂载时 agent 级注册一律被拒） |

#### 宿主侧（`plugin_manager_lifecycle.cpp` / `plugin_manager_vtable.cpp`）

- `mountAgentAsync()`：改为**增量 + 幂等** —— 只补挂尚未挂载的实例，`mountSeq_`
  只在首次（`==0`）自增；
- 新增 `mountOneAsync` / `unmountOneAsync`：挂载时解冻 + 应用清单资源 → 驱动
  `mountFn` → 成功后置 `pluginMounted`/`mountUserData` 并**重新冻结**；失败逐项回滚；
  拆挂载时先解冻 → 调 `unmountFn` → 摘除 agent 级注册与清单资源
  （**不动**能力声明与主程序总线订阅 —— 它们属于 `start`/`stop`）；
- `loadPluginAsync()`：已挂载时新加载的插件自动补挂当前 agent；
- 禁用 / 启用新增 `onInstanceDisabledAsync` / `onInstanceEnabledAsync` 扩展点
  （框架 `pluginxx::host::lifecycle.h`），保证 disable/enable 交错后恰好挂载一次；
- `scopeOk()` 失败时记 WARN，带收到的 `agent_id`/`seq` 与期望值（便于插件自查）；
- `detachDomainRegistrations` 的提示词 owner 键从 `plugin:<名>` 改回实例名（旧写法
  导致提示词贡献撤不掉）。

#### SDK 修正

- `plugin_kit.h`：`CallToolState` / `FeatureCallState` 改存 `ScopeRef` 值
  （原来存 `raw()` 临时量的指针 → 悬垂，表现为 `call_tool_async: agent reference is invalid`）；
- `AGENTXX_PLUGIN_AGENT_MOUNT_EXPORT`：没有 `mountFn` 时按契约 `notify->done(OK)` 后返回
  （原来直接返回 `nullptr`，主程序判为 `protocol violation: null operation without
  done/error`，导致无 per-agent 状态的 JS 壳插件全部挂载失败）。

#### 边界/文档同步

- `boundaries`：接口表数量 20 → **21**（新增 `agentxx.agent.mount`，通用 10 + 业务 11），
  成员台账同步四张 v2 表的成员变化（tools/feature 去 `op_cancel`、cancel 表加
  `op_cancel`、hooks 合三成员）；词表规则新增「行内例外」，并把新写注释里的
  `宿主`/`载入`/`口径`/老`内核` 改回项目统一用词；
- `docs/zh-cn/design/plugins.md`：§8 表数改 21、补 `mount` 表、版本列改 v2；
- 根 `AGENTS.md`：接口表数量与版本基线说明同步。

#### 已知限制

- 一份 JS 脚本目前只对应一个挂载作用域：`plugins` 模块与其他模块串跑时，
  `example_js` 偶发钩子/订阅登记失败（单跑全绿），阶段 B/D 处理。

## 2. 待完成任务

### 2.1 插件迁移（已完成，见 §1.4）

22 个插件全部要改（`agent/plugins/**`）：

1. 导出宏从 6 参数改成 8 参数（无 per-agent 状态传 `nullptr, nullptr`）；
2. 把 `start` 里的 agent 级注册搬到新的 `mount` 回调；
3. per-agent 状态存 `AgentRef`（`MountStore<T>` 或自有 map）；
4. 所有 agent 级调用补 `st->ref`；
5. 钩子改句柄式（`hook` / `hookEx` + `unhook`）；
6. `op_cancel` 从 `iface.tools` / `iface.feature` 改到 `iface.cancel`。

要点：`fast_tool` / `blocking_tool` / `polled_tool` 的 `ScopeRef` 是**第一个**参数
（`ctx` 之后）；`hook(ctx, ref, point, fn)`；`graph_node(ctx, ref, type, schema, fn)`。

#### 已迁移的插件

- `agentxx_math`（**模板**，可直接照抄）：`start` 只剩 `notify->done`，注册搬到
  `mount`，`unmount` 只给完成信号。

```cpp
static void* mathStart(MathPluginCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);   // 无进程级工作
    (void)ctx;
    return nullptr;
}

static void* mathMount(
    MathPluginCtx&                     ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
) {
    if (!notify) { /* 写 error 并返回 nullptr */ }
    const AgentRef ref(*spec);                    // 拷 agentId + 挂载序号
    if (mathSetup(ctx, ref) != 0) { /* 写 error 并返回 nullptr */ }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;                               // 无 per-agent 状态 = nullptr
}

static void* mathUnmount(
    MathPluginCtx&, void*, const PluginxxOperatorNotify* notify, PluginxxString*
) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    MathPluginCtx, "agentxx_math", "1.0.0", "...",
    mathStart, mathStop, mathMount, mathUnmount
);
```

- 需要 per-agent 状态时（工具计数、群列表等）：`mount` 里 `auto* st = new State{AgentRef(*spec)};`
  → 注册时传 `st->ref` → `return st;`；`unmount` 里 `delete static_cast<State*>(ud);`。
  多挂载共用一个 `plugin_ctx` 时用 `MountStore<State>`（已在 SDK 里）。

### 2.2 测试模块与测试插件（已完成，见 §1.6）

先做测试插件（`agent/test/plugin/dso_plugins/**`）：

- `test_start_fail/test_start_fail.cpp`：把工具/图节点/订阅/prompt/hook/资源/能力
  注册从 `start` 搬到新的 `mount` 入口（导出 `agentxx_plugin_agent_mount` /
  `_unmount`），全部调用补 agent 引用；hook 改句柄式（`register_hook(host, agent,
  &specEx, &handle)`）。该夹具用于“某一步失败 → 主程序回滚已生效注册”，失败点
  会从 start 事务变成 mount 事务，`test_plugin_runtime` 里对应的断言要同步改。
- `test_client_start_fail`：client 侧只需版本号（已提）与入口不变，检查是否编译通过。

然后改测试模块：

`cmake --build … agentxx_test` 当前的错误分布（共 111 处，均在同一批文件里）：

| 文件 | 错误数 | 主要问题 |
|---|---|---|
| `test/plugin/test_plugins.cpp` | 26 | 接口表调用补 `agent`；`loadConfiguredPlugins` 新形态 |
| `test/plugin/test_plugin_hooks.cpp` | 24 | `registerHookEx`/`unregisterHookEx` → 句柄式；`HookRegistration.base` 已删；`register_hook_ex` → `register_hook` |
| `test/plugin/test_plugin_bridge.cpp` | 18 | 接口表调用补 `agent` |
| `test/plugin/test_plugin_runtime.cpp` | 15 | 删 `restorePromptBackup` 用例；表数量 21；v2 基线断言 |
| `test/plugin/test_plugin_sdk.cpp` | 14 | kit 辅助函数新增的 `agent` 参数 |

已迁完：`test_cpu_gpu_use.cpp`（`invokeCapabilityAsync` 补 `nullptr`）与
`test_plugin_resources.cpp`（用管理器造 `PluginxxScopeRef`，9 处资源接口补引用）。

做法建议：在测试公共头（`agent/test/include/agentxx-test/...`）里加一个小助手，
从管理器造引用，避免每处手写：

```cpp
inline PluginxxScopeRef scopeRefOf(const PluginManager& mgr) {
    PluginxxScopeRef ref{};
    ref.struct_size = sizeof(PluginxxScopeRef);
    ref.id          = PluginStringView::from(mgr.agentId());
    ref.seq         = mgr.mountSequence();   // 未挂载时为 0 = 不校验
    return ref;
}
```

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

## 3. 当前进度说明

契约、插件、测试三个阶段都已迁完（§2.1 / §2.2 是机械改动，已完成）。
接下来是唯一有设计取舍的**阶段 B**：把插件实例表上提到宿主（`AgentHost`）、
把 agent 级注册收进 `PluginMount`，并按 §2.3 的六步改造；之后再接 D / F / G。

在阶段 B 完成前，`PluginManager` 仍是「一份管理器服务一个 agent」，因此同进程
内两个 agent 共用插件时仍需各自建管理器（测试里的多实例场景）。此中间态可编译、
测试全绿。

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

## 5. 阶段 B 完成记录（宿主上提 / 挂载拆分）

代码：

- 新增 `agentxx/plugin/plugin_mount.h` + `src/plugins/plugin_mount.cpp`：一个 agent 的
  全部 `agent 级` 注册搬到 `PluginMount`（`Entry` 按实例分开记账：工具/权限声明/钩子
  句柄/提示词贡献/功能点/图节点槽位/图定义占用/资源/配置 JSON/`mountUserData`）。
- `PluginManager` 只留宿主侧：实例表 `plugins_`、挂载表 `mounts_`、挂载序号
  `nextMountSeq_`、主 agent 上下文 `primaryAgent_`、vtable 分发与加载。
  `mountAgentAsync(ctx)` / `attachMountSync(ctx)` / `unmountAgentAsync` /
  `unmountAllAgentsAsync` 驱动挂载；`PluginInstance` 只剩加载元信息 + `mountFn/unmountFn`
  + 弱引用挂载表，并提供只读视图 `anyMounted/allToolNames/allToolObjects/allHookHandles/allGraphSlots`。
- `AgentContext` 增加 `pluginMount`（本 agent 的挂载）与 `agentId`；
  `base_agent.cpp` 的 `plugin_mount` 步骤从 `pluginManager->mountAgentAsync(ctx)` 取挂载。
- vtable 入口按 `PluginxxScopeRef` 解析挂载（`PluginManager::mountOf`），解析不到返回
  `AGENTXX_PLUGIN_ERR_NO_AGENT`；无引用的宿主级表项取主 agent 的挂载。
- 禁止在 `start` 里做 agent 级注册：此时没有挂载上下文，调用会被拒。

禁用/启用语义（与设计文档一致，改动集中在 `detachInstance` / `mountOneAsync` / `unmountOneAsync`）：

- `disable` = 每个挂载**同步**拆掉注册（`PluginMount::detachInstance` → `revokeEntry`），
  并把插件的 `unmount` 回调记为"欠着"（`Entry::unmountPending`），实例本身不 `stop`；
- 之后 `onInstanceDisabledAsync` 在 IO 线程补调插件 `unmount`（此前若又被启用并重新挂载，
  这次补充自动作废）；
- `enable` = 只做"没挂就挂"（`mountOneAsync`，幂等），插件在自己 `mount` 回调里重新声明，
  `start` 事务照常跑（进程级资源）；
- `unload` = 全部挂载 `unmountOneAsync` → `stop` → `destroy` → `dlclose`。

同时修掉的两处真实缺陷：

- `PluginMount::entryOf` 建记录时没登记反向引用（`inst->mounts`），导致实例侧只读视图
  （`allToolNames/allGraphSlots`）看不到 `attachMountSync` 之后登记的项；
- `PluginManager::getPluginJson` 的 `tools` 恒为空（未按挂载汇总），改为与
  `listPluginsJson` 同一来源（`fillInstanceView` + `mergeMountView`）。

测试侧修正：

- `test_plugin_runtime` 夹具的构造参数曾与成员同名（`ctx`），成员恒为空、临时
  `AgentContext` 在构造结束时析构并关掉宿主上的全部实例 —— 改名 `existingCtx`；
- 若干夹具改为显式 `mountAgentAsync` / `attachMountSync` 后再断言实例级记录。

- 插件契约版本由 cmake 覆盖：`cxx_pluginxx` 代码里的 `PLUGINXX_API_VERSION` /
  `PLUGINXX_MIN_API_VERSION` 默认是 1（该库被多个主程序共用，不跟随某个主程序的契约），
  agentxx 在顶层 `CMakeLists.txt` 往全局 `CMAKE_C_FLAGS/CMAKE_CXX_FLAGS` 追加
  `-DPLUGINXX_API_VERSION=2 -DPLUGINXX_MIN_API_VERSION=2`；嵌套构建（框架/插件/测试/
  客户端）都经 `_AGENTXX_COMMON_CMAKE_ARGS` 继承同一份 FLAGS，插件只用头文件也能拿到。
  改完必须让嵌套工程重新配置，否则旧 cache 里没有这两个定义，插件会报
  `API version too old`（同时发现该报错分支会崩，见下）。
- 订阅句柄的 `agentId` 字段在 `cxx_pluginxx` 里已改名 `scopeId`（框架不能出现
  agentxx 专有命名），agentxx 侧使用点同步改。

已知遗留（下一阶段处理）：

- 阶段 D/F/G 已过一轮（见下节）。

## 6. 阶段 D / F / G 记录

**D. 管理器上提与配置**

- `AgentHost::Config::pluginList` + `pluginHost()` + `loadPluginsAsync()`：进程一份管理器，
  加载阶段不持 agent 上下文；成员声明在 `rootAgent_` 之前（逆序析构：先拆挂载再关管理器）。
- 启动顺序统一为 `create → loadPluginsAsync → 注入 ctx->pluginManager → agent->init()`
  （内部只挂载）`→ attachRoot`；`attachRootAgentHost` 删除。
  已改：`mode_runners.cpp`（TUI/CLI）/`jsonl_mode.cpp`/`main.cpp`（server）/`ffi_runtime.cpp`。
- `BaseAgent`：有注入时不自建、不重复加载（`ownsPluginHost_`）；无注入时自建自加载
  （单 agent / 测试）。
- `AgentConfig::pluginMountPolicy`（`All`/`None`/`Listed`，默认 All）+ `pluginMountList`
  + `pluginMountArgs`；`mountAgentAsync` 按策略选区，`mountOneAsync` 合并挂载级 args；
  子代理默认 `None`。
- yaml 解析：`plugin.mount` 两种写法（`[{name,args}]` 或 `{policy,list,plugins[]}`）；
  `plugin` 段合并时把 `mount` 一起并入（列表段合并会把它丢掉）；`config_keys` 增两条
  并重生成 `agent/schema/config-keys.json` 与 `docs/zh-cn/design/config-keys.md`。

**F. 诊断**

- `--dump-diagnostics` 详细列表新增 `## plugin mounts`（agentId / seq / 已挂数量 / 插件名）。

**G. 文档**

- `AGENTS.md`、`docs/zh-cn/design/index.md`（架构第 7 小节）、`plugins.md`（§2 版本与
  加载/挂载分离）已同步新顺序与 `plugin.mount`、版本宏的 cmake 覆盖方式。

- 进程退出时若仍有协程持有 `AgentContext`（MCP 连接收尾）会走到
  `~AgentContext` 的告警日志，此时日志子系统已析构，ASan 报 heap-use-after-free；
  属退出期日志顺序问题，不影响功能；
- `ffi_c_api` 模块在本机（搜狗输入法 + ASan）会被 IME 的 USE_AFTER_FREE 打断，与本项目无关。

