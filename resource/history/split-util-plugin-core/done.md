# 拆分 `cxx_utilxx_base` / `cxx_utilxx` / `cxx_pluginxx` —— 实施记录

> 关联方案: [plan.md](./plan.md)
> 记录时间: 2026-09-18 (Windows Debug 全量构建 + 全量测试验证)

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

### 验证 (Windows Debug, MSVC, ASan)

| 项目 | 结果 |
|---|---|
| 全量构建 | 通过 (libagentxx + 20 插件 + client + test) |
| 全量测试 | `Total: passed=21164 failed=9` —— 与拆分前基线 (10 失败) 相比**无新增失败** |
| 失败项 | 全部为既有 Windows 平台/环境问题 (POSIX 绝对路径断言、临时目录文件占用等) |
| 插件测试模块 | `plugins` 542/542 通过 (与基线一致) |
| ABI 冻结 | **拆分前编译的 `agentxx_execute_command.dll` 被新宿主加载并正常执行** (542/542) |
| 产物命名 | `libcxx_utilxx_based.dll`、`libcxx_utilxx_base_staticd.lib`、`libcxx_utilxxd.dll`、`libcxx_utilxx_staticd.lib`、`libcxx_pluginxxd.dll`、`libcxx_pluginxx_staticd.lib` |
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

## 未完成 (后续实施)

### P3-3b 运行时搬迁 (`pluginxx/runtime/*`)

- `plugin_runtime.h` / `plugin_driver.h` / `op_driver.h` / `plugin_common.h` /
  `plugin_manager_base.h` → `pluginxx/runtime/*`, 命名空间 `agentxx::plugin` → `pluginxx`
- `op_driver.h` 的取消令牌改 `utilxx::CancelTokenPtr` (agentxx 侧经
  `agentxx/util/cancel_adapter.h` 适配), 抛 `utilxx::CancelledException`

### P3-3c SDK 与通用实现搬迁

- `plugin_kit.h` (6246 行) 拆分: 通用部分 → `pluginxx/kit/kit.h` (含 `PluginStringView`/
  `PluginString`/`PluginBase`/`Task`/Awaiter/导出宏), agentxx 侧保留领域 helper
  (ToolSchemaBuilder/blocking_tool/hook/capability/graph_node/权限声明辅助)
- `plugin_guard.h` → `pluginxx/kit/guard.h` (通用 `logTo`/`guardCall`; client 侧
  `AgentxxClientLogIface` 重载留在 agentxx)
- `plugin_common.cpp` 的通用函数 (pluginNameFromPath/parsePluginManifest/resolvePluginEntryPath/
  topoSortPlugins/ioCallSync*/collectReverseRequiredDeps) 与 `NativeLoader` → `pluginxx/src/`
- 通用表实现 (log/json/config/plugins/events/scheduler/coroutine_runtime/tasks/cancel/
  capabilities) → `pluginxx/src/tables_impl.cpp`, 宿主数据经 `DomainHooks` 取数
- `client_plugin_api.h` 的 include 收窄为 pluginxx 头 + client 领域表

### P4 `pluginxx::PluginHostCore` 抽取

见 plan.md §5.6 / §7 P4 (装载/启停/租约/inflight/op 记账/事件总线/能力注册表 +
`PluginManager` / `ClientPluginManager` 改为组合 host core)。

### P5 清理

- 删除 `agent/build/*/exec` 与安装树中的历史残留 (`libagentxx_util.lib`、`lib/cmake/agentxx_util`)
- 插件源码注释中残留的 `agentxx_util` 字样; `docs/en/**` 同步更新

### P6 下游与发布

- 三库 README 已落地 (`cxx_utilxx_base` / `cxx_utilxx` / `cxx_pluginxx`),
  musicxx 接入说明仍待补 (plan.md §11)
