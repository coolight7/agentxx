# 拆分 `cxx_utilxx_base` / `cxx_utilxx` / `cxx_pluginxx` —— 实施记录

> 关联方案: [plan.md](./plan.md)
> 记录时间: 2026-09-18 (第一次) / 2026-09-18 续 (P3-3b：插件框架内核运行时搬迁)
> 2026-09-18 第三次更新: P3-3c 主体完成 (SDK 通用/领域分层) + P4-1 部分 (能力注册表下沉)
> 2026-09-18 第四次更新: P3-3c 收尾 (十张通用表**实现**整体下沉) + P4-1 完成
> (`PluginHostCore` + `DomainHooks` + 通用表 vtable 入口装配); P4-2/P4-3 待实施

## 已完成

### P0 骨架与构建接入 ✅

- 三个独立 CMake 工程落在 `agent/third_party/` 下 (与本项目维护的第三方库同级):
  - `cxx_utilxx_base/` (静态 + 动态双产物, 手写 config 导出, 无 CPM/网络依赖)
  - `cxx_utilxx/`
  - `cxx_pluginxx/`
- 命名规则沿用 libagentxx: Release `libcxx_utilxx_base.so` / `libcxx_utilxx_base_static.a`,
  Debug 追加 `d` → `libcxx_utilxx_based.so` / `libcxx_utilxx_base_staticd.a` (三个库同理)
- superbuild (`agent/CMakeLists.txt`) 新增三个 `ExternalProject_Add`
  (`BUILD_ALWAYS`/`INSTALL_ALWAYS`, 便于迭代) 并加入 `agentxx_lib_repo` 的 DEPENDS
- 安装树: `include/{utilxx_base,utilxx,pluginxx}`、`lib/cmake/{cxx_utilxx_base,cxx_utilxx,cxx_pluginxx}`、
  产物落 `exec/` (与 libagentxx 同目录, 便于 `$ORIGIN` 复用)

### P1 + P2 工具库搬迁 ✅ (两个阶段合并实施, 避免中间态 include 路径不一致)

- 13 个基础头 + 6 个源 → `cxx_utilxx_base` (`utilxx_base` 命名空间):
  log/json/json_view/string_util/env/container_util/hash/lru_cache/path_sanitize/
  stream/async_mutex/asio_error + 新拆出的 `system.h`, 另新增 `exception.h`(通用 catchError 家族)、
  `version.h`
- 13 个重依赖头 + 8 个源 → `cxx_utilxx` (`utilxx` 命名空间):
  http_client/http_header/http_error/http_server/ws_client/router/sqlite/settings_db/
  regex/aho_corasick/diff_util/worktree + 新拆出的 `crypto.h` (md5Hex/getDeviceId)
- `agentxx/util/async_offload.h` → `utilxx/async_offload.h`, 取消令牌统一为
  `utilxx::CancelTokenPtr` (`utilxx/cancel.h`: 抽象 + `SignalCancelToken`)
- `agentxx/util/util.h` / `util.cpp` 删除 (内容分别进 `utilxx_base/system.*` 与
  `utilxx/crypto.*`); agentxx 侧只保留 `exception.h` / `neograph_json_bridge.h` /
  新增 `cancel_adapter.h`
- 全仓替换 (无转发头): `#include "agentxx/util/X"` → `utilxx_base/X` | `utilxx/X`;
  `agentxx::util::` / 短形式 `util::` / `using namespace agentxx::util;` /
  `neograph_asio_*` 与 `agentxx_asio_*` 别名 → `utilxx_base::AsioErrorCode|AsioSystemError`
- `agentxx_util` 目标退役; libagentxx / 插件 / test 改经 `find_package` 链接
  `cxx_utilxx_base_static` / `cxx_utilxx_static` (插件按是否用重依赖工具区分)

### P3-3a C ABI 分层 ✅

- `pluginxx/api/abi.h`: 与领域无关的纯 C 基座 (导出宏/调用约定/API 版本/对齐/StringView/String/
  Info/统一操作原语/事件订阅前向声明/宿主 vtable/入口符号名宏/内置合并描述) —— **C 名零变化**
- `pluginxx/api/tables.h`: 通用接口表 (log/json/config/plugins/events/capabilities/
  scheduler/coroutine_runtime/tasks/cancel), IID 与结构体名零变化
- `agentxx/plugin/api/plugin_api.h` 改为 umbrella (包含上述两个头 + 保留 agent 领域表:
  tools/permission/hooks/session/model/prompt/resources/graph), **插件源码零改动**
- `pluginxx/src/api_abi_check.c`: 以 C 编译校验两个头可被 C 直接包含 + 8 字节对齐/定长类型断言

### P3-3b 宿主运行时搬迁 ✅ (本次完成)

把插件框架的**宿主侧运行时**整体搬入 `cxx_pluginxx`, 命名空间 `agentxx::plugin` → `pluginxx`;
agentxx 只保留领域实现 (工具/hook/能力/会话/图/资源/prompt 与 client 侧 UI 表)。

新增 (`agent/third_party/cxx_pluginxx/`):

| 新文件 | 来源 | 说明 |
|---|---|---|
| `include/pluginxx/runtime/runtime.h` | `agentxx/plugin/plugin_runtime.h` | 实例状态机 / 执行 lease / 投递通道 (`PluginRuntime` `InstanceLifetime` `InstanceLease` `enqueueRuntimeAction` `replayRuntimeActions` `isRuntimeIoThread`) |
| `include/pluginxx/runtime/driver.h` | `agentxx/plugin/plugin_driver.h` | 协程驱动 ticket (`AgentxxPluginDriver`), 句柄校验注册表 |
| `include/pluginxx/runtime/instance_base.h` | `agentxx/plugin/plugin_manager_base.h` 前半 | `PluginInstanceBase` / `PluginHostControl` (host 视图 tombstone) / `hostMemory*` / `getExecutableDirPath` |
| `include/pluginxx/runtime/manager_base.h` | `plugin_manager_base.h` 后半 | `PluginManagerBase<InstanceT>` / `PluginHostCall` / `enterPluginHost` / `collectReverseRequiredDeps` |
| `include/pluginxx/runtime/op_driver.h` | `agentxx/plugin/op_driver.h` + `plugin_manager.h` 的句柄 | `OpCore` 完成协议 / `cancelPluginOperation` / `awaitPluginOp` / `awaitPluginLifecycle`; **两个 ABI 不透明句柄移到此处仍留在全局命名空间** |
| `include/pluginxx/host/loader.h` + `src/loader.cpp` | `plugin_manager_lifecycle.cpp` 的 `NativeLoader` | dlopen/LoadLibrary 封装 |
| `include/pluginxx/host/manifest.h` + `src/manifest.cpp` | `agentxx/plugin/plugin_common.h` + `plugin_common.cpp` 通用部分 | 插件名推导 / `plugin.yaml` 解析 / 入口路径解析 / 拓扑排序 / 内置清单查找 |
| `include/pluginxx/host/abi_util.h` | `plugin_common.h` | C 串转换 (`svToSv`/`svToStr`/`strToSv`)、`guardVtableCall*`、`ioCallSync*` |

agentxx 侧新增 / 保留:

| 文件 | 说明 |
|---|---|
| `agentxx/plugin/plugin_framework.h` (新) | 把内核类型以**逐条 using 声明**引入 `agentxx::plugin`, 宿主侧代码无需到处写 `pluginxx::`; 类型本体仍只有一份 (非转发头) |
| `agentxx/plugin/plugin_interfaces.h` + `src/plugins/plugin_interfaces.cpp` (新) | 接口协商 (三层协商的声明/校验) 与接口名目录 —— 需要知道本宿主实现了哪些表, 属宿主领域 |
| `agentxx/util/cancel_adapter.h` | 新增 `awaitHostPluginOp`: 把内核抛出的 `utilxx::CancelledException` 转换为 `neograph::graph::CancelledException`, 保持宿主侧取消判定口径 |
| `agentxx/util/exception.h` | 分类器的 `utilxx::CancelledException` 分支把 `exPtr` 归一化为图引擎取消异常 |

关键解耦 (框架内核零宿主依赖):

1. **`OpCore` 不再回查管理器类型**: `PluginInstanceBase` 新增 `std::weak_ptr<PluginRuntime> runtime`,
   由 `PluginManagerBase::makeLifetime(inst)` 装配; `awaitPluginOp` 经
   `isRuntimeIoThread(runtime)` 校验 io 线程, 因此 `PluginOpAwaitArgs` 保持非模板, 调用点零改动。
2. **内置插件表经注册点取数**: 内核不引用宿主符号 `agentxx_plugin_get_builtin_*`, 改由
   `pluginxx::setBuiltinPluginProvider` 注册点提供; 宿主在
   `plugins/builtin_plugins.cpp.in` 生成的 `builtin_plugins.cpp` 里静态初始化期登记。
3. **`PluginRuntime::pendingOperationSummary()`** 改为 `pluginxx/runtime/op_driver.h` 内的
   inline 定义 (定义处需 `OpCore` 完整类型)。
4. **`PluginBase` 与领域 helper 暂留 agentxx**: `plugin_kit.h` 的通用/领域拆分属 P3-3c
   (见"未完成"), 现阶段宿主侧文件按需 include `agentxx/plugin/api/plugin_kit.h` 取
   `PluginStringView`/`PluginString`。

依赖变更: `cxx_pluginxx` 新增 `yaml-cpp` (清单解析) —— CMake `find_package(yaml-cpp)` +
config 的 `find_dependency(yaml-cpp)` + superbuild `cxx_pluginxx_repo` DEPENDS 追加 `yaml_cpp_repo`。

验证 (Windows Debug, MSVC, ASan, 全量构建 + 全量测试):

| 项目 | 结果 |
|---|---|
| 全量构建 | 通过 (3 库 + libagentxx + 20 插件 + client + test) |
| 全量测试 | `Total: passed=21164 failed=9` —— 与拆分前基线 (passed=21164 failed=9) **完全一致** |
| 插件测试模块 | `plugins` 542/542 · `plugin_runtime` 672/672 · `plugin_sdk` 75/75 · `plugin_bridge` 193/193 · `plugin_resources` 83/83 · `plugin_multi_instance` 80/80 · `client_plugins` 531/531 |
| 产物命名 | `libcxx_utilxx_based.dll` / `libcxx_utilxx_base_staticd.lib` / `libcxx_utilxxd.dll` / `libcxx_utilxx_staticd.lib` / `libcxx_pluginxxd.dll` / `libcxx_pluginxx_staticd.lib` |
| 插件导出面 | `example_plugin.dll` 仅导出 10 个 `agentxx_plugin_{agent,client}_*` 入口 |
| 插件依赖面 | 插件 DLL 依赖仅系统库 + CRT + ASan 运行库 (无 cxx_* 动态依赖) |

实施中发现并修复的关键点 (供后续阶段参考):

1. **特性宏不再是 INTERFACE 传递**: 拆分前 `AGENTXX_ENABLE_BOOST_PROCESS` 等宏经
   `agentxx_util` 的 PUBLIC 定义传播给插件; 拆分后必须在 `plugins/CMakeLists.txt` 显式定义,
   否则 `agentxx_execute_command` 会退化为 `_popen` 回退路径 (表现为命令执行超时)
2. **命名空间不等于库**: `cxx_utilxx_base` 中 `include/utilxx/*` 属于 `utilxx` 命名空间
   (取消/卸载契约), 其余属于 `utilxx_base`; 搬迁脚本按库分类时须单独处理
3. **asio 别名**: 使用 `asio::` 的头必须能见到 `utilxx_base/asio_error.h` (它提供全局
   `namespace asio = ::boost::asio`), 新增文件需显式 include
4. **跨行限定名**: 仓库中 clang-format 会把 `agentxx::util::X` 拆到多行, 替换脚本需容忍换行
5. **通用库不得引用宿主异常类型**: `utilxx_base` 的分类器通过
   `setExtraExceptionClassifier` 注册宿主 (agentxx) 的 neograph 取消/中断识别,
   agentxx 侧在 `classifyCurrentException` 首用与 `AgentHost` 初始化处注册
6. **安装树不会清理陈旧头文件**: `install(DIRECTORY include/ ...)` 只增量覆盖,
   删除源文件后旧头仍留在 `agentxx-project-install/include/` 并被优先命中
   (症状: 旧 `agentxx/plugin/op_driver.h` 与新的 `pluginxx/runtime/op_driver.h` 类型重定义)。
   搬迁/删除头文件后必须手动清理安装树对应文件 (或整体删除 `include/agentxx`)
7. **测试里的伪实例要装配运行时**: `PluginInstanceBase::runtime` 由
   `PluginManagerBase::makeLifetime` 写入; 测试中手工构造实例的夹具需显式
   `inst->runtime = manager->runtime();`, 否则 `awaitPluginOp` 会以"缺少 IO 执行器"失败

## 第四次实施: P3-3c 收尾 (通用表实现整体下沉) + P4-1 ✅

目标: 把**十张通用表的定义与实现** (log/json/config/plugins/events/scheduler/
coroutine_runtime/tasks/cancel/capabilities) 整体搬入 `cxx_pluginxx`, 使新宿主只需
"继承宿主核心 + 实现领域钩子 + 装配 vtable" 即可复用; agentxx 只保留领域表与宿主数据。

### 新增 (agent/third_party/cxx_pluginxx/)

| 文件 | 内容 |
|---|---|
| `include/pluginxx/host/event_bus.h` | 事件表后端抽象 `EventSource` (subscribe/unsubscribe/publish)、订阅句柄实现体 `AgentxxPluginSubscription` (原在 agentxx/plugin_manager.h 的全局结构体; 字段改为内核类型: `EventSource` + `weak_ptr<PluginInstanceBase>` + `weak_ptr<PluginRuntime>`)、幂等撤销 `unsubscribePluginSubscription` |
| `include/pluginxx/host/domain_hooks.h` | `DomainHooks`: 事件后端/主题命名空间补齐/工作线程池/配置 JSON/工具提示词/会话工作目录/语言读写/会话取消状态/插件清单 JSON |
| `include/pluginxx/host/host_core.h` | `PluginHostCore<InstanceT>`: 通用表状态 (能力注册表) 与方法实现 —— 能力登记/撤销/查询/调用、事件订阅/撤销/发布、`postCallback`/`sleep`/`offload`、`registerTask`、实例级撤销辅助 (`revokeInstanceSubscriptions`/`unregisterInstanceCapabilities`) |
| `include/pluginxx/host/tables_impl.h` | `GenericTableEntries<InstanceT, ManagerT>`: 十张通用表的 C ABI 入口 (解析宿主控制块 → 投递 IO 线程 → 调用同名方法) + 表结构体 (函数内静态) + `queryGenericPluginIface<I,M>(iid)` |
| `include/pluginxx/host/capability_registry.h` | 新增实例侧声明记录 `PluginCapabilityRegistration` (名称/启动/取消/上下文) |
| `include/pluginxx/runtime/instance_base.h` | 新增通用表相关登记 (`subscriptions`/`subscriptionHandles`/`sleepTimers`/`capabilityRegistrations`)、`sharedSelf<InstanceT>()`、`detail::revokeSubscription` 定义 (事件撤销簿记; 定义放此处因本头才完整见到实例类型) |

### agentxx 侧变化

| 文件 | 变化 |
|---|---|
| `agentxx/plugin/plugin_manager.h` | `PluginManager` 改继承 `pluginxx::PluginHostCore<PluginInstance>` + 实现 `pluginxx::DomainHooks`; 删除已下沉的方法/字段 (能力/事件/调度/任务/capabilities_); `PluginInstance` 删除已上移的登记字段, `CapabilityRegistration` 改为内核类型别名 |
| `agentxx/plugin/plugin_manager_domain_hooks.cpp` (新) | `DomainHooks` 全部实现 + 事件后端 `AgentEventBusSource` (包装 `agentxx::events::EventBus`; 订阅为handler 转发, 发布经 co_spawn 异步) + 主题命名规则 (`plugin.`/`client.` 前缀) |
| `plugin_manager_vtable.cpp` | 删除十张通用表的入口与表结构体 (共 885 行); `query_interface` 先经 `queryGenericPluginIface` 查通用表, 再分发领域表 (tools/permission/hooks/session/model/prompt/resources/graph) |
| `plugin_manager_capability.cpp` | 仅保留领域部分 (`callToolAsync`); 能力/事件/调度/任务/协程驱动实现全部移除 |
| `plugin_manager_scheduler.cpp` / `plugin_manager_tasks.cpp` | 删除 (内容整体进 `host_core.h`) |
| `plugin_manager_adapters.cpp` | 删除 `subscribe`/`unsubscribe`/`publish` 实现 (事件表下沉) |
| `plugin_manager_lifecycle.cpp` | 构造函数注入领域钩子 (`setDomainHooks(this)`); `detachAll` 改用核心的 `revokeInstanceSubscriptions` / `unregisterInstanceCapabilities` |
| `agentxx/plugin/plugin_framework.h` | 追加 `using`: `DomainHooks`/`EventSource`/`PluginHostCore`/`PluginCapabilityRegistration`/`unsubscribePluginSubscription`/`GenericTableEntries`/`queryGenericPluginIface` |
| `agent/test/plugin/test_plugin_runtime.cpp` | 夹具补 `inst->ownerSelf = inst` (通用表实现经 `ownerSelf` 取实例自引用; 生产路径 `makeInstance` 已设置两个引用) |
| `agent/test/plugin/test_plugins.cpp` | 同上 (伪实例夹具) |

### 关键设计点 (供后续参考)

1. **不新增 `enable_shared_from_this`**: 通用方法需要"管理器自引用"时会与非模板
   `PluginManager::enable_shared_from_this` 形成多基类歧义 (libstdc++/MSVC 下 `weak_this`
   两个都不初始化 → `bad_weak_ptr`)。因此**投递职责放在 vtable 入口** (入口已持
   `shared_ptr<ManagerT>` 与 admission lease), 核心方法统一要求在 IO 线程调用;
   `invoke_capability_async` 的跨线程投递改由 `ioCallSyncKeep` 完成 (与其它入口一致,
   排队期间仍持 admission lease, 语义等价)。
2. **事件撤销的 io 线程投递**: 句柄内保存 `weak_ptr<PluginRuntime>`, 撤销时若不在 IO 线程
   经 `enqueueRuntimeAction` 投递并同步等待 (运行时不可投递时就地完成); `alive` 先置 false,
   因此迟到事件立即短路, 不必等簿记完成。
3. **`PluginInstanceBase` 承载通用登记**: 事件订阅/睡眠句柄/能力声明都上移到基类, 通用表
   实现因此只依赖基类 —— 新增宿主无需重复实现同一套登记 (ClientPluginInstance 自有同名
   `subscriptions` 成员, 会隐藏基类同名成员; 客户端不使用宿主核心, 不受影响)。
4. **表的静态实例**: 表结构体用函数内静态 (首次查询构造, C++11 起线程安全), 支持插件线程
   并发 `query_interface`。
5. **offload 失败文案保持** `"plugin offload: no thread pool"` (既有测试断言该子串)。

### P4-1 验收 (Windows Debug / MSVC / ASan, 全量构建 + 全量测试)

| 项目 | 结果 |
|---|---|
| 全量构建 | 通过 (3 库 + libagentxx + 20 插件 + client + test) |
| 全量测试 | `Total: passed=21164 failed=9` —— 与拆分前基线 (`21164/9`) **完全一致** |
| 插件相关模块 | `plugins 542 · plugin_runtime 672 · plugin_sdk 75 · plugin_bridge 193 · plugin_resources 83 · plugin_multi_instance 80 · client_plugins 531` 全绿 (合计 2176) |
| 已知失败 | `config_loader` 路径格式 1 例、`interrupt_bus` 3 例、`agent` 4 例、`session_persistence` 环境 1 例 (均为既有平台/时序敏感用例, 与本次改动无关) |

---

## 未完成 (后续实施)

### P3-3c SDK 与通用表实现搬迁 ✅ (已完成)

已搬迁: `plugin_common.cpp` 的通用函数 (名称推导/清单解析/入口路径/拓扑排序/内置清单查找)、
`NativeLoader`、C ABI 辅助 (`abi_util.h`) —— 见 P3-3b 表。

**本次完成 (SDK 通用/领域分层)**:

| 项 | 结果 |
|---|---|
| `pluginxx/kit/kit.h` (新增, 3945 行) | 通用插件 SDK: `PluginStringView` / `PluginString` / `queryInterface`、**新增** 通用表聚合 `PluginIfaceCore` (10 张通用表 + query)、`CancelledException`、`Logger` / `pluginLog` / `pluginStrdup` / `ctxGuardLogger` / `jsonEscape`、协程驱动桥 `detail::{PollOneBridge,BridgeRoot,PolledRoot}`、`CancelRegistry` / `OpCtl`、`ArgReader`、`Task<T>` 与完成协议、锚定原语 awaiter (`sleep` / `yield` / `offload` / `invoke_cap`)、后台任务 `spawn`、能力注册 `capability`、阻塞能力调用 `invoke_capability_blocking`、生命周期守卫 `detail::{callLifecycleEntry,autoStopSpawns}` / `logCreateFailure`、导出宏 `AGENTXX_PLUGIN_AGENT_EXPORT` / `AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT` |
| `pluginxx::PluginBaseT<IfacesT>` | 原 `PluginBase` 的通用部分改为**模板基类**: 持有宿主句柄/接口表聚合/`Logger`/`CancelRegistry`, 提供 config / workDir / argsJson / configPath / language / sessionCancelled / jsonEscape / jsonGetString / 宿主堆字符串 / 协程桥 / 后台任务; 新增 `protected virtual onHostReady()` 领域挂钩 (在 `init()` 末尾调用); `spawn` 改为类内联定义 (不再需要 out-of-class 定义) |
| `pluginxx/kit/guard.h` (新增) | C ABI 边界异常守卫: `logTo` (agent 侧日志表) / `reportCurrentException` / `guardCall` / `guardCallVoid` |
| `agentxx/plugin/api/plugin_kit.h` (2497 行) | 改为**领域 + umbrella**: 内含 pluginxx 头, 并以逐条 `using` 把通用名引入 `agentxx::plugin` (插件源码零改动); 保留领域部分: `AgentIfaces` / `ClientIfaces`、`ToolPromptText` / `ToolSchemaBuilder`、`call_tool` / `tool` / `fast_tool` / `blocking_tool` / `polled_tool`、`hook`、图节点、工具权限声明、`call_tool_blocking`、client 渲染适配、`kit::ActionController`、`ClientPluginBase`; **新增** `PluginBase : pluginxx::PluginBaseT<AgentIfaces>` (领域 helper + `onHostReady()` 订阅会话轮次开始事件) |
| `agentxx/plugin/api/plugin_guard.h` | 改为 umbrella: 含 `pluginxx/kit/guard.h` + 通用名 `using` 引入 + **client 侧** `AgentxxClientLogIface` 的 `logTo` 重载 |
| `agentxx/plugin/api/client_plugin_api.h` | include 收窄: 不再包含 agent 侧 `plugin_api.h`, 改为直接包含 `pluginxx/api/{abi,tables}.h` + 仅声明 client 领域表 |
| 命名空间分层细节 | 通用部分命名空间 `pluginxx`; `detail` 中的通用设施 (`PollOneBridge` / `AwaiterState` / `PromiseBase` / `RootRequest` / `CompletionGuard` / `jsonGet` / awaiters / `invokeCap` / `spawnTaskImpl` / `callLifecycleEntry` / `autoStopSpawns` …) 由 agentxx 侧 `namespace detail { using pluginxx::detail::X; }` 引入, 领域 helper 定义仍在 `agentxx::plugin::detail` |

**本次完成 (P4-1 部分 — 能力注册表下沉)**:

- `CapabilityRegistry` (能力名 → 提供者插件 + 启动/取消回调 + 上下文) 从
  `agentxx/plugin/plugin_manager.h` 迁到 `pluginxx/host/capability_registry.h` +
  `src/capability_registry.cpp` (纯领域无关: 仅依赖 C ABI 类型 + `utilxx_base`)
- agentxx 侧经 `agentxx/plugin/plugin_framework.h` 的 `using pluginxx::CapabilityRegistry;`
  继续以原名使用 (宿主领域实现零改动)

仍待实施: 无 —— **通用表实现的整体下沉已完成** (见上节"第四次实施"): 十张通用表的
状态/方法 (`pluginxx/host/host_core.h`) 与 C ABI 入口 (`pluginxx/host/tables_impl.h`) 都在
`cxx_pluginxx`, 领域数据经 `DomainHooks`; agentxx 侧只保留领域表与宿主数据实现。

## P4 `pluginxx::PluginHostCore` 抽取 (P4-1 已完成; P4-2/P4-3 待实施)

现状: P3-3b 已把**运行时**下沉 (`PluginRuntime` / `InstanceLifetime` /
`PluginManagerBase<InstanceT>` / op_driver / loader / manifest / abi_util),
P4-1 已把**能力注册表**下沉; 第四次实施已完成 P4-1/P4-2 (通用表的**状态与方法实现**
`PluginHostCore<InstanceT>` + 通用表入口 `tables_impl.h` + 领域钩子 `DomainHooks`);
仍留在宿主侧的是**加载/启停生命周期骨架**与 **client 侧管理器**:

1. `DomainHooks` (plan.md §5.6): 领域表查询路由 / 实例注册摘除 / 领域配置 JSON /
   接口需求解析;
2. `PluginHostCore<InstanceT>`: 事件总线 (订阅簿记 + 发布) / 任务托管
   (`sleep` / `offload` / `registerTask` / `postCallback` 的 op 记账) / 通用表装配
   (vtable 入口 trampoline + 表结构体静态实例) / 装载启停骨架 (级联依赖);
3. `PluginManager` / `ClientPluginManager` 改为继承 host core + 实现 `DomainHooks`,
   领域表实现 (tools/hooks/session/prompt/graph 与 client UI) 原样保留。

**建议实施顺序 (每步都能单独验收)**:

- 步 1 (低风险) ✅ **已完成** (第四次实施): 十张通用表的入口改为
  `pluginxx::host::tables_impl.h` 的 `GenericTableEntries<I, M>`; agentxx 的
  `xx_query_interface` 先调 `pluginxx::queryGenericPluginIface<I, M>(iid)` 再分发领域表;
- 步 2 ✅ **已完成**: `PluginManager` 的通用方法 (`subscribe`/`unsubscribe`/`publish`/
  `registerTask`/`sleep`/`offload`/`postCallback`/`registerCapability*`/
  `invokeCapabilityAsync`) 与其状态 (`CapabilityRegistry`、实例侧事件订阅/睡眠/能力登记)
  整体移入 `PluginHostCore`, 领域数据经 `DomainHooks` 取;
- 步 3 (待实施): 装载/启停骨架 (`plugin_manager_lifecycle.cpp` 的通用部分) 下沉,
  agentxx 只保留 `DomainHooks` 实现与领域表 (风险最高, 需跑
  `plugin_multi_instance` / 卸载重载 / 取消路径回归);
- 步 4 (待实施): `ClientPluginManager` 同样处理 (client UI 表留在 agentxx);
  可复用 `GenericTableEntries<ClientPluginInstance, ClientPluginManager>` 装配
  client 侧的 log/json 等通用表 (需先让 client 实例满足实例基类契约)。

**验收提示 (本次已复现的基线)**: 全量 `agentxx_test` 为
`passed=21163~21164 / failed=9~10` —— 失败数波动来自已知的时序敏感用例
(`config_loader` 路径格式断言 / `interrupt_bus` 3 例 / `agent` 4 例, 单模块重跑稳定),
插件相关模块固定为 `plugins 542 · plugin_runtime 672 · plugin_sdk 75 ·
plugin_bridge 193 · plugin_resources 83 · plugin_multi_instance 80` 全绿 (合计 1645)。


### P5 清理 (本次部分完成)

- 已更新文档: `docs/zh-cn/design/plugins.md` (§5.1 目录结构表 + §6 SDK 分层说明)、
  `docs/zh-cn/design/index.md` (代码结构章节的插件与三库部分)、根 `AGENTS.md`
  (cxx_pluginxx 条目)、`cxx_pluginxx/README.md` (目录结构与落地进度)
- **第四次实施已更新**: 上述四份文档补充宿主核心与通用表实现下沉的说明
  (`pluginxx/host/{host_core,tables_impl,domain_hooks,event_bus}.h`、
  `agent/plugin/plugin_manager_domain_hooks.cpp`)
- **第四次实施已完成**: 插件源码注释中残留的 `agentxx_util` 字样已清理
  (改为 `cxx_utilxx` / `cxx_utilxx_base` / `utilxx_base::detectPowerShell`)
- 待办: 删除 `agent/build/*/exec` 与安装树中的历史残留 (`libagentxx_util.lib`、
  `lib/cmake/agentxx_util`); **注意**: 安装树的 `include/agentxx`、`include/pluginxx`
  需按源码核对, 陈旧头会导致难以理解的类型重定义错误 (见上文实施要点 6)。
  本次新增的头 (`pluginxx/kit/{kit,guard}.h`、`pluginxx/host/*.h`) 随构建自动安装,
  无需手工清理
- 待办: `docs/en/**` 同步更新 (英文设计文档尚未同步三次/四次实施的变更)

### P6 下游与发布 (本次部分完成)

- 三库 README 已落地; `cxx_pluginxx/README.md` 本次补齐 kit/guard/capability_registry 说明,
  并在第四次实施补充 `host_core.h` / `domain_hooks.h` / `tables_impl.h` / `event_bus.h`
  与"宿主接入形态"示例 (`PluginHostCore` + `DomainHooks` + `queryGenericPluginIface`)
- 待办: musicxx 接入说明 (plan.md §11)

---

## 本次验证记录 (Windows Debug / MSVC / ASan, 全量构建 + 全量测试)

| 项目 | 结果 |
|---|---|
| 全量构建 | 通过 (3 库 + libagentxx + 20 插件 + client + test) |
| 全量测试 | `Total: passed=21163 failed=10` (基线 `21164/9`; 差异来自时序敏感用例, 单模块重跑稳定); 插件相关模块与基线**完全一致**: `plugins 542/0 · plugin_runtime 672/0 · plugin_sdk 75/0 · plugin_bridge 193/0 · plugin_resources 83/0 · plugin_multi_instance 80/0` (合计 1645) |
| `remote_agent` 单模块重跑 3 次 | 415/0 · 415/0 · 415/0 (全量运行时受资源竞争影响偶发 1 例, 与本次改动无关) |
| 插件导出面 (`dumpbin /exports` on `libexample_plugin.dll`) | 仅 10 个入口符号: `agentxx_plugin_agent_{get_info,create,start,stop,destroy}` + `agentxx_plugin_client_{...}` |
| 插件依赖面 (`dumpbin /dependents`) | 仅 `WS2_32`/`KERNEL32`/`MSVCP140D`/`VCRUNTIME140D`/`VCRUNTIME140_1D`/`ucrtbased`/`clang_rt.asan-*` —— 无任何 cxx_* 动态依赖 (静态变体生效) |
| `check_plugin_exports.sh` / `check_sdk_negative_compile.sh` | 未运行: 二者依赖 Linux 构建产物 (`readelf`/`nm` 与 `compile_commands.json` 的 GCC 风格命令), 本机为 Windows/MSVC; 导出面已用 `dumpbin` 等价核对 |
| 结构检查 | `cxx_pluginxx` 头/源中无 `agentxx/` 头引用与 `agentxx::` 限定名 (仅注释中指向宿主侧配套头的说明) |

### 本次实施要点 (供后续 P4 参考)

1. **SDK 分层的关键是 umbrella + using**: 插件源码零改动靠
   `agentxx/plugin/api/{plugin_api.h,client_plugin_api.h,plugin_kit.h,plugin_guard.h}`
   四个文件路径与文件名不变, 内部改为 "包含 pluginxx 头 + 领域部分 + 逐条 `using`";
   `using` 声明不产生新类型 (无 ODR 风险), 且同一函数名可跨命名空间共同参与重载决议
   (client 侧 `logTo` 与通用 `logTo` 即按此共存)
2. **模板化基类优于参数化整个 SDK**: `PluginBaseT<IfacesT>` 只要求实参提供
   10 张通用表的成员; `AgentIfaces` 天然是超集, 故 agentxx 侧只需
   `class PluginBase : pluginxx::PluginBaseT<AgentIfaces>` + 领域 helper +
   `onHostReady()` 覆写, 无需改动任何调用点
3. **自由函数改为模板入参**: `sleep`/`yield`/`offload`/`invoke_cap` 的
   `const PluginBase&` 改成 `template<typename Ctx> ... (const Ctx&)`,
   既能接受 agentxx 派生类, 也能服务其他宿主
4. **`detail` 的跨层引用用 using 桥接**: 通用 detail 设施整体留在
   `pluginxx::detail`, agentxx 侧在 `agentxx::plugin::detail` 中以
   `using pluginxx::detail::X;` 引入, 领域 helper 继续写 `detail::X` 即可;
   `AwaiterState` 由通用与领域两条 awaiter 路径共用, 故归 pluginxx
5. **迁移脚本必须先备份源文件**: 本次拆分用一次性脚本按原始行区间搬运
   (脚本运行前把源头备份为 `_plugin_kit.orig.h`); 曾因把输出路径写成源文件同一路径,
   第二次运行读到已拆分的文件导致边界断言失败 —— 脚本已按"输出与输入分离 + 行号断言"
   修正, 任务结束后随中间备份一并删除 (需要再拆分时从 git HEAD 取原始版本)
6. **`PluginBase::spawn` 需随基类模板化调整为类内联定义**: 原实现是
   类外 `void PluginBase::spawn(...)` — 模板基类的类外定义需额外
   `template<typename IfacesT> template<...>` 前缀, 故把 `detail::spawnTaskImpl`
   的段落到类之前, `spawn` 改成类内联; `spawnTaskImpl` 里的
   `PluginBase::SpawnRecord` 改经 `std::remove_reference_t<Ctx>::SpawnRecord` 取得
