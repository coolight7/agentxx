# 拆分 `cxx_utilxx_base` / `cxx_utilxx` / `cxx_pluginxx` —— 实施记录

> 关联方案: [plan.md](./plan.md)
> 记录时间: 2026-09-18 (第一次) / 2026-09-18 续 (P3-3b：插件框架内核运行时搬迁)

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

## 未完成 (后续实施)

### P3-3c SDK 与通用表实现搬迁 (部分完成)

已搬迁: `plugin_common.cpp` 的通用函数 (名称推导/清单解析/入口路径/拓扑排序/内置清单查找)、
`NativeLoader`、C ABI 辅助 (`abi_util.h`) —— 见 P3-3b 表。

仍待实施:

- **`plugin_kit.h` (6246 行) 通用/领域拆分**: 通用部分 → `pluginxx/kit/kit.h`
  (`PluginStringView`/`PluginString`/`Logger`/`logTo`/`jsonEscape`/`Task`/协程驱动桥/
  锚定 awaiter 族/`ArgReader`/`CancelRegistry`/`OpCtl`/导出宏), agentxx 侧保留领域 helper
  (ToolSchemaBuilder / blocking_tool / fast_tool / polled_tool / hook / capability /
  graph_node / 权限声明辅助) 并与 `pluginxx/kit/kit.h` 组成 umbrella
  - **难点**: `PluginBase` 持有的 `AgentxxAgentInterfaces` / `AgentxxClientInterfaces`
    聚合了领域表, 与通用基座相互纠缠。可行方向: 把聚合体拆为
    `pluginxx::PluginIfaceCore` (通用表) + agentxx 派生聚合; `PluginBase` 参数化为
    `template<typename AgentIface, typename ClientIface>` 并用
    `using PluginBase = pluginxx::PluginBase<AgentxxAgentInterfaces, AgentxxClientInterfaces>`
    维持插件源码零改动
- `plugin_guard.h` → `pluginxx/kit/guard.h` (通用 `logTo`/`guardCall`; client 侧
  `AgentxxClientLogIface` 重载留在 agentxx)
- `client_plugin_api.h` 的 include 收窄为 pluginxx 头 + client 领域表
- 通用表**实现** (log/json/config/plugins/events/scheduler/coroutine_runtime/tasks/cancel/
  capabilities) → `pluginxx/src/tables_impl.cpp`, 宿主数据经 `DomainHooks` 取数

### P4 `pluginxx::PluginHostCore` 抽取

见 plan.md §5.6 / §7 P4 (装载/启停/租约/inflight/op 记账/事件总线/能力注册表 +
`PluginManager` / `ClientPluginManager` 改为组合 host core)。

### P5 清理

- 删除 `agent/build/*/exec` 与安装树中的历史残留 (`libagentxx_util.lib`、`lib/cmake/agentxx_util`);
  **注意**: 安装树的 `include/agentxx`、`include/pluginxx` 也需要按源码核对, 陈旧头会导致
  难以理解的类型重定义错误 (见上文实施要点 6)
- 插件源码注释中残留的 `agentxx_util` 字样; `docs/en/**` 同步更新
- `design/index.md` 代码结构章节的插件框架部分 (本次只更新了 `design/plugins.md` §5)

### P6 下游与发布

- 三库 README 已落地 (`cxx_utilxx_base` / `cxx_utilxx` / `cxx_pluginxx`),
  musicxx 接入说明仍待补 (plan.md §11)
