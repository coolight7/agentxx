# 插件系统开发指南

> 关联: [design](index.md) (主程序架构) · [ffi.md](ffi.md) (FFI) · 源码: [agent/plugins/](/agent/plugins/) · C ABI 契约: [plugin_api.h](/agent/lib/include/agentxx/plugin/api/plugin_api.h) / [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h) / SDK: [plugin_kit.h](/agent/lib/include/agentxx/plugin/api/plugin_kit.h)

---

## 1. 总览

Agentxx 插件系统采用 **纯 C ABI + COM 风格接口表查询**：

- **纯 C 边界**：跨边界仅传递纯 C 基本类型、函数指针、不透明句柄与 `PluginxxStringView` (data+size 只读借用，不要求 NUL 结尾)，严禁直接传递 `std::string/vector/function` 或 C++ 异常
- **跨编译器/标准库/语言兼容**：主程序与插件可由不同编译器、不同 STL (libstdc++/libc++/MSVC STL) 或不同语言独立编译，运行时稳定兼容
- **内存所有权**：所有跨边界堆内存统一经 `host->alloc/free` (核心 vtable 内存管理操作) 管理，接收方用后 `host->free`；字符串复制采用头文件内联助手 `pluginxx_strdup(host, ...)`
- **原生协程异步支持**：经 `plugin_kit.h` 的 `Task<T>`，插件协程执行于宿主 IO 线程，挂起让出、完成经 IO 线程回调唤醒，宿主与插件的协程执行可互相交错切换，且运行于同一线程无锁，无轮询、无私有事件循环
- **单线程会话**：宿主会话可变状态仅在主 IO 线程串行访问；插件注册/状态访问由宿主内部按需 `post` 回 IO 线程，插件无感

---

## 2. 核心架构与兼容性准则

```
宿主 (libagentxx / agentxx_cli)
  核心 vtable (冻结) ── alloc / free / query_interface (IID → 接口表)
                       │
         ┌─────────────┼─────────────┬──────────────┬─────────────┐
         │ tools       │ hooks       │ events       │ scheduler   │  ...17 张 agent + 7 张 client
         │ register/   │ 7 钩子点     │ publish/     │ sleep/      │  capabilities/
         │ call_tool   │             │ subscribe    │ offload     │  session/plugins/
         └─────────────┘             └──────────────┘             │  config/model/cancel/...
插件动态库 (任意编译器) ── PLUGINXX_EXPORT 入口 ── PluginBase 上下文堆 ── SDK 注册族
```

- **核心 vtable 冻结**：仅 `alloc/free + query_interface`，永不增删；一切宿主能力按稳定 `IID` 字符串查询独立接口表获取 (`AGENTXX_PLUGIN_QUERY_IFACE` 宏)
- **严格 ABI 规约**：
  - 8 字节结构体对齐：头文件统一包含 `#pragma pack(push, 8)` / `#pragma pack(pop)`
  - 定长基础数据类型：禁止无修饰 `int/long/size_t`，跨边界统一采用 `int32_t`、`int64_t`、`uint64_t` 等定长类型
  - 明确调用约定：跨边界导出符号与函数指针一律携带宏 `PLUGINXX_CALL` (Windows 平台定义为 `__stdcall`，x64 Unix 平台为空)
  - 结构体传参与返回值：跨边界禁止值传递聚合结构体，入参一律为指针 (`const Struct*`)；结构体返回值一律改为指针出参 (`Struct* out`) 并返回 `int32_t` 状态码 (0 表示成功)
  - 核心 vtable 精简：移除原 `strdup` 槽位，改为基于 `alloc` 的头文件内联实现 `pluginxx_strdup`
  - C++ 辅助便捷层：`PluginxxStringView` 与 `PluginxxString` 内置 `operator const T*()` 隐式取址转换与 `empty()` 方法，文件尾部提供值传兼容重载与 `pluginxx_string_free` 重载
- **接口表独立演进**：每张表首字段 `int32_t version` 独立版本号；表内函数指针可能为 `NULL` (宿主未实现该子能力，调用前判空)
- **版本限制**：全局 `PLUGINXX_API_VERSION` / `AGENTXX_CLIENT_PLUGIN_API_VERSION` 均重置为 1，加载时要求 `>=` 宿主版本否则拒绝；新增能力 = 新增接口表或表内追加成员并递增该表版本，全局版本号不动
- **线程约定**：`query_interface/alloc` 任意线程；注册类与 session/config/prompt 等 IO 约束操作由宿主内部投递同步等待；操作 `start/cancel` 由宿主在 IO 线程驱动 (单次 <~1ms)；`PluginxxOperatorNotify.done` 可任意线程回调；宿主派发给插件的完成回调 (`AgentxxOpCb`/sleep/offload done) 保证在 IO 线程 `post` 入队
- **实例生命周期、Operation 终态与租约**：见第 15 节（实例生命周期契约，所有插件必须遵守）

---

## 3. 多实例三铁律

同一插件动态库在单进程内可能被多个独立 Agent 宿主分别加载并创建多个并存实例 (如 FFI 多句柄、AgentHost 子代理)：

1. **禁止可变全局/函数级 static**：所有可变状态必须封装在随实例创建的上下文堆对象 (`*plugin_ctx`，通常继承 `kit::PluginBase`)
2. **状态经上下文闭包恢复**：所有工具/钩子/事件回调必须通过 `spec.user_data` 恢复当前实例上下文
3. **接口表缓存存入实例上下文**：`AgentIfaces` 查询结果存实例成员，各实例互不干扰；offload线程池异步接口 (`plugin_kit.h`) 适配器为调用方内嵌存储，随实例销毁释放

**唯一例外：进程级单调计数器**。用于生成进程内唯一名称/序号（如
`agentxx_filesystem` 的编辑临时文件名 `s_editTmpSeq`）的 `static std::atomic<uint64_t>`
是允许的：它不承载实例语义，改成每实例计数反而会让两个实例生成同名临时文件。
例外只适用于"只增不减、只用于唯一性"的原子计数器；任何缓存、状态机、配置副本
仍然必须放在实例上下文中。

---

## 4. 导出符号控制

插件动态库默认隐藏全部符号，仅导出宿主按名查找的入口符号。入口函数必须以 `PLUGINXX_EXPORT` 标记 (位于 `extern "C"` 内)：

```c
#include "agentxx/plugin/api/plugin_api.h"
extern "C" PLUGINXX_EXPORT const PluginxxInfo* agentxx_plugin_agent_get_info(void);
extern "C" PLUGINXX_EXPORT int32_t agentxx_plugin_agent_create(const PluginxxHost* host, void** plugin_ctx);
extern "C" PLUGINXX_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx);
```

- **入口符号集**：
  - Agent 侧：`agentxx_plugin_agent_get_info` / `agentxx_plugin_agent_create` / `agentxx_plugin_agent_destroy`
    (+ 可选的 `agentxx_plugin_agent_start` / `agentxx_plugin_agent_stop`)
  - Client 侧 (双端/纯 UI)：`agentxx_plugin_client_get_info` / `agentxx_plugin_client_create` / `agentxx_plugin_client_destroy`
    (+ 可选的 `agentxx_plugin_client_start` / `agentxx_plugin_client_stop`)
- **构建侧自动化**：`plugins/CMakeLists.txt` 统一配置 ELF `-fvisibility=hidden` + version script 白名单 (通配符 `agentxx_plugin_agent_*`/`agentxx_plugin_client_*`，兼容单端插件在 Android lld 下链接)，macOS `-exported_symbols_list`，MSVC `dllexport`；第三方静态库符号自动隐藏
- **校验脚本**：`agent/script/check_plugin_exports.sh [plugin-dir]` 用 `nm -D --defined-only`
  遍历构建产物，要求每张插件库只导出上述入口符号；出现任何其他导出符号即失败
  (用于确认第三方静态依赖与 `cxx_utilxx`/`cxx_utilxx_base` 的符号确实被隐藏)

---

## 5. 工具与框架复用 (`cxx_utilxx_base` / `cxx_utilxx` / `cxx_pluginxx`)

面向项目内置插件，可通过独立静态库复用主程序的全部基础工具 (字符串/编码检测/UTF-8
转换/路径规范化/Base64/日志/JSON + HTTP/正则/差异/worktree 等) 与**插件框架内核**
(C ABI 基座 / SDK 基座 / 宿主运行时 / 清单解析)。三者是 `agent/third_party/` 下本项目
自研的独立 CMake 工程 (与 fmt/simdjson 同级)，经 superbuild 先构建安装，再由 libagentxx /
各插件 `find_package` 引用其**静态变体**：

```cmake
# 基础件 (无重依赖): 日志/JSON/字符串/容器/环境/系统探测/取消令牌/异步卸载/异常分类
find_package(cxx_utilxx_base REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE cxx_utilxx_base_static)

# 重依赖工具 (依赖基础件): HTTP/WS/正则/路由/差异/worktree/散列
find_package(cxx_utilxx REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE cxx_utilxx_static)

# 插件框架内核 (依赖基础件, 仅 Boost 头 + yaml-cpp): 见下表
find_package(cxx_pluginxx REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE cxx_pluginxx_static)
```

> 数据库相关工具不在上述库内: SQLite 封装 (`SqliteDb`) 与全局设置库 (`SettingsDb`) 属
> **宿主专用**代码 (`agentxx/util/sqlite.h` / `settings_db.h`, 依赖宿主的数据目录约定与
> `AgentConfigStatic`), 工具库 (含 cxx_utilxx) 不链接 SQLite; 插件需要数据库时自行链接
> 或经宿主能力表申请数据落盘。

```cpp
#include "utilxx_base/string_util.h"
auto b64 = utilxx_base::base64Encode(data);
#include "utilxx_base/json.h"
#include "utilxx_base/json_view.h"
// 业务/插件统一用 utilxx_base::Json/JsonView (simdjson 驱动); 高频只读先 JsonView::parse 路由, 命中后 to_json() 物化
```

### 5.1 三库归属 (`cxx_pluginxx` 目录结构)

| 路径 | 命名空间 | 内容 |
|---|---|---|
| `pluginxx/api/` | 纯 C (`Agentxx*`) | 跨边界契约: `abi.h` (导出宏/调用约定/字符串/操作原语/宿主 vtable/入口符号)、`tables.h` (通用接口表: events/capabilities/scheduler/coroutine_runtime/plugins/config/cancel/json/log/tasks) |
| `pluginxx/kit/` | `pluginxx` | 插件侧 C++ SDK (header-only): `kit.h` (通用部分: 跨边界字符串工具 `PluginStringView`/`PluginString`、通用接口表聚合 `PluginIfaceCore`、实例级 `Logger`、`Task<T>` 锚定协程与锚定原语 `sleep`/`yield`/`offload`/`invoke_cap`、`CancelRegistry`/`OpCtl`/`ArgReader`、后台任务 `spawn`、能力注册 `capability`、实例上下文基类 `PluginBaseT<IfacesT>`、通用导出宏)、`guard.h` (C ABI 边界异常守卫 `guardCall`/`guardCallVoid`/`logTo`) |
| `pluginxx/runtime/` | `pluginxx` | 宿主侧运行时: `runtime.h` (实例状态机/执行 lease/投递通道)、`driver.h` (协程驱动 ticket)、`instance_base.h` (实例基类 + 宿主控制块 + C ABI 内存 + 通用表相关登记: 事件订阅/睡眠句柄/能力声明)、`manager_base.h` (管理器基类 + vtable 入口上下文)、`op_driver.h` (统一 Operation 驱动器) |
| `pluginxx/host/` | `pluginxx` | 宿主侧通用设施: `loader.h` (dlopen/LoadLibrary 封装)、`manifest.h` (plugin.yaml 解析/名称推导/拓扑排序)、`abi_util.h` (C 串转换/异常兜底/io 线程同步投递)、`capability_registry.h` (能力注册表: 能力名 → 提供者插件 + 启动/取消回调)、`event_bus.h` (事件表的事件后端抽象 `EventSource` + 订阅句柄实现体 `PluginxxSubscription` + 幂等撤销)、`domain_hooks.h` (领域钩子 `DomainHooks`: 通用表需要宿主数据的入口)、`host_core.h` (宿主核心 `PluginHostCore<InstanceT>`: 通用表的状态与方法实现)、`tables_impl.h` (十张通用表的 vtable 入口 trampoline + `queryGenericPluginIface<I, M>(iid)`)、`lifecycle.h` (宿主生命周期骨架 `PluginHostLifecycle<InstanceT>`: 装载/启停/禁用启用/卸载/级联依赖) |

- **通用表实现整体在 `cxx_pluginxx`**: log/json/config/plugins/events/scheduler/
  coroutine_runtime/tasks/cancel/capabilities 十张表的**定义与实现**都在内核 ——
  `PluginHostCore<InstanceT>` 提供方法实现 (能力登记/事件订阅/睡眠/委托/任务托管/取消
  投递), `tables_impl.h` 提供 C ABI 入口 (解析宿主控制块 → 投递 IO 线程 → 调用同名方法);
  宿主只需 `class MyManager : public pluginxx::PluginHostCore<MyInstance>` +
  实现 `pluginxx::DomainHooks` (事件后端、工作线程池、配置/语言/会话工作目录/取消状态/
  插件清单) + 在 `query_interface` 里先调 `queryGenericPluginIface` 再分发自己的领域表。
  agentxx 侧对应实现见 `agent/plugin/plugin_manager_domain_hooks.cpp` (事件后端
  `AgentEventBusSource` 包装 `agentxx::events::EventBus`, 主题命名空间补齐等)

- **生命周期骨架整体在 `cxx_pluginxx`** (`pluginxx/host/lifecycle.h` 的
  `PluginHostLifecycle<InstanceT>`, 继承 `PluginHostCore`): 动态库装载与入口符号校验、
  `create`/`start` 事务、启用与禁用级联、`stop` 事务补齐、inflight 归零等待、`destroy`
  与动态库卸载、失败回滚与关闭超时重试全部只有一份实现 —— agent 侧 `PluginManager`
  与 client 侧 `ClientPluginManager` 都继承它。宿主只提供少量接缝:
  - 纯虚: `selfRef()` (管理器自引用, 因内核不能自己继承 `enable_shared_from_this`,
      否则与宿主管理器的同名基类形成多基类歧义)、`createInstance(name)` (生成领域实例)、
      `hostVtable()` (交给自己插件的 vtable);
  - 可选覆写: `detachDomainRegistrations` (摘除领域注册: 工具/权限/钩子/图/提示词 或
      client 侧 UI 注册表)、`clearDomainRegistrations`、`detachDomainOwnedResources`、
      `applyDeclaredResources`、`releaseInstanceResources`、`onInstanceEnabledChanged`、
      `onInstanceLoaded`/`onInstanceUnloaded`、`cascadeUnloadEnabledOnly`、`logTag`。

  > client 侧**不复用**装载骨架: 其装载有独立语义 (dlopen 卸载到内部线程池执行、
  > 接口协商限制、agent/client 双入口探测), 见 `ClientPluginManager::loadNativeAsync`;
  > 但卸载/启停/级联/关闭等待与 agent 侧共用同一实现。
  >
  > 实例的 `destroyPlugin()` (destroy 入口查找 + 调用 + 宿主控制块退休) 已上移到
  > `pluginxx::PluginInstanceBase`, agent/client 两侧实例只需给出本端符号名
  > (`pluginDestroySymbol()`)。

- 领域表 (tools/permission/hooks/session/model/prompt/resources/graph 与 client 侧全部表)
  由宿主定义与实现，见 `agentxx/plugin/api/plugin_api.h` / `client_plugin_api.h`
- **SDK 分层**: 通用部分 (`pluginxx/kit/kit.h` / `guard.h`) 在 `cxx_pluginxx`，领域 helper
  (工具注册 `tool`/`fast_tool`/`blocking_tool`/`polled_tool`、`ToolSchemaBuilder`、`hook`、
  图节点、工具权限声明、`call_tool_blocking`、client 侧渲染与 `ClientPluginBase`) 在
  `agentxx/plugin/api/plugin_kit.h` / `plugin_guard.h`；后者是 **umbrella** 头 (包含
  pluginxx 头 + 领域部分)，并把通用名以逐条 `using` 引入 `agentxx::plugin` —— 因此
  插件源码的 `#include` 路径与 `agentxx::plugin::Xxx` 写法**零改动**；
  通用基类 `pluginxx::PluginBaseT<IfacesT>` 以宿主接口表聚合为模板实参，agentxx 侧
  `PluginBase : PluginBaseT<AgentIfaces>` 只补领域 helper 并覆写 `onHostReady()` 挂钩领域事件
- agentxx 侧配套头: `agentxx/plugin/plugin_framework.h` (把内核类型以逐条 `using` 引入
  `agentxx::plugin`)、`agentxx/plugin/plugin_interfaces.h` (接口协商/清单目录)、
  `agentxx/util/cancel_adapter.h` (图引擎令牌与统一取消抽象互适配)
- 库归属与命名空间：`cxx_utilxx_base` → `utilxx_base` (另含跨库共享契约 `utilxx::CancelToken`
  与 `utilxx::offloadAsync*`, 定义在 `utilxx/cancel.h` / `utilxx/async_offload.h`)；
  `cxx_utilxx` → `utilxx` (http/ws/regex/router/diff/worktree/crypto)
- 每个库同时产出静态库与动态库，命名规则同 libagentxx
  (Release: `libcxx_utilxx.so` / `libcxx_utilxx_static.a`；Debug 追加 `d`)
- **独立发布约束**: 三个库按独立工程对外发布, 库内不使用宿主专名 —— 构建变量统一
  `XX_*` 前缀 (`XX_INSTALL_DIR` / `XX_EXEC_INSTALL_PREFIX` /
  `XX_LINUX_IO_URING_SUPPORTED`, 平台/编译器宏 `XX_IS_*_D`), 未传入时各自回退默认值;
  宿主 superbuild 把同名取值经 `XX_*` 下发给它们 (见 `agent/CMakeLists.txt` 公共参数),
  agentxx 侧的开关名 `AGENTXX_*` 只出现在宿主自己的构建目录里
- libagentxx 与各插件各自静态链接一份副本，符号经导出控制隐藏互不冲突；
  依赖全部 `PUBLIC` 传递 (fmt/simdjson/uchardet/iconv + OpenSSL/html2md/Boost 头 +
  yaml-cpp；`pluginxx` 仅 Boost 头，不链接 Boost 编译库)
- **条件依赖只声明库名, 由使用方在本机 find (顺序: 先依赖库的依赖, 再依赖库本体)**:
  导出接口里出现的是 `PkgConfig::uring` (io_uring, 属 `cxx_utilxx_base` 的 asio 文件
  异步 I/O 探测) / `PkgConfig::hyperscan` + 裸库名 `hs_runtime` (HyperScan, 属 `cxx_utilxx`
  的正则实现), **不含库文件路径**; 静态库不携带依赖二进制, 谁链接谁解析 —— 使用方须
  **先** `pkg_check_modules` 出这些目标 (导出目标会校验其 INTERFACE 引用的目标是否已存在),
  **再** `find_package` 工具库 / `agentxx_static`: lib/client/test/benchmark/plugins 按顶层
  `AGENTXX_LINUX_IO_URING_SUPPORTED` / `AGENTXX_ENABLE_HYPERSCAN` 开关判断 (与工具库
  构建开关同源), plugins 目录查找一次即覆盖全部插件目标 (插件自身无需任何配置);
  两个工具库自身构建按中性开关 `XX_LINUX_IO_URING_SUPPORTED` 判断
  (superbuild 把顶层 `AGENTXX_LINUX_IO_URING_SUPPORTED` 的取值经该变量下发) ——
  漏查找会在 configure 期报目标不存在, 或运行期 dlopen 报
  `undefined symbol: io_uring_queue_init`
- **通用库不得引用宿主符号**: 内置插件表由宿主生成，故 `cxx_pluginxx` 经
  `pluginxx::setBuiltinPluginProvider` 注册点取数 (宿主静态初始化期登记)，内核自身不链接
  `agentxx_plugin_get_builtin_*`
- 定位为内置插件便捷库 (与主程序同一 superbuild 构建、依赖齐全)；第三方插件仅需纯 C 头
  `plugin_api.h` / SDK `plugin_kit.h`，无需链接宿主库
- 未引用模块按目标文件提取自动裁剪 (9 插件 `DT_NEEDED` 仅系统库)
- 拆分背景与迁移记录：`resource/history/split-util-plugin-core/plan.md`

---

## 6. C++ 插件开发方式 (SDK `plugin_kit.h`)

推荐使用官方 header-only SDK `plugin_kit.h` (位于 `agentxx/plugin/api/plugin_kit.h`)。
最新框架提供了开箱即用的声明式导出宏、链式 Schema 构建器、宽容参数提取器与通用取消注册中心。
其中 `Task<T>` 协程 (以及 `sleep`/`yield`/`offload`/`call_tool`/`invoke_cap` 原语) 的推进
交由宿主的**协程驱动桥**调度：见 §16 与 §15 的生命周期契约。

该 SDK 由两部分组成 (插件源码只需包含上面的 umbrella 头，写法不变)：

| 部分 | 位置 | 内容 |
|---|---|---|
| 通用 (与宿主领域无关) | `pluginxx/kit/kit.h`、`pluginxx/kit/guard.h` | `PluginStringView`/`PluginString`、通用接口表查询与聚合 `PluginIfaceCore`、`Logger`、`Task<T>` 与锚定原语 (`sleep`/`yield`/`offload`/`invoke_cap`)、`CancelRegistry`/`OpCtl`/`ArgReader`、后台任务 `spawn`、能力注册 `capability`、实例上下文基类 `PluginBaseT<IfacesT>`、通用导出宏、边界异常守卫 |
| agentxx 领域 | `agentxx/plugin/api/plugin_kit.h`、`plugin_guard.h` | 接口表聚合 `AgentIfaces`/`ClientIfaces`、工具注册族 (`tool`/`fast_tool`/`blocking_tool`/`polled_tool`)、`ToolSchemaBuilder`、`hook`、图节点、工具权限声明、`call_tool`/`call_tool_blocking`、client 侧渲染与 `ClientPluginBase` |

`agentxx::plugin::PluginBase` 即 `pluginxx::PluginBaseT<AgentIfaces>` 的 agentxx 派生类
(补领域 helper 并在 `onHostReady()` 中挂钩会话轮次开始事件)。插件源码继续写
`agentxx::plugin::Xxx` 即可 —— umbrella 头已用逐条 `using` 把通用名引入该命名空间。

```cpp
#include "agentxx/plugin/api/plugin_kit.h"

using namespace agentxx::plugin;

struct MyPluginCtx : public PluginBase {};

// 使用声明式导出宏 (自动封装入口符号、C ABI 边界异常守卫与实例上下文生命周期)
AGENTXX_PLUGIN_AGENT_EXPORT(
    MyPluginCtx,
    "my_plugin",
    "1.0.0",
    "My awesome plugin description",
    [](MyPluginCtx& ctx) -> int32_t {
        // 1. ToolSchemaBuilder: 链式构建 JSON Schema (自动与宿主 toolPrompt 提示词覆盖融合)
        auto mySchema = ctx.schema("my_blocking_tool")
                            .string("path", "Target file path", /*required=*/true)
                            .integer("timeout", "Timeout in seconds", false, 60)
                            .boolean("all_output", "Return all output", false, true)
                            .build();

        // 2. 阻塞工具 (自动卸载到宿主 blockingPool 线程池，不占 IO 线程)
        // 回调直接注入: (ctx, args_json, tid, workDir, cancel_flag)
        blocking_tool(
            ctx,
            "my_blocking_tool",
            "Perform heavy work in worker thread",
            mySchema,
            [](MyPluginCtx&     c,
               std::string_view args_json,
               std::string_view tid,
               std::string_view workDir,
               volatile int32_t* cancel_flag) -> std::string {
                // ArgReader: 宽容类型提取 (支持 string/number/bool/json 宽容转换与自愈)
                ArgReader args(args_json);
                auto path = args.require<std::string>("path");
                if (!args.ok()) {
                    return args.errorMessage();
                }

                // 配合 CancelRegistry 检查取消 (事件驱动或状态查询)
                if ((cancel_flag && *cancel_flag != 0) || c.sessionCancelled(tid)) {
                    return "cancelled";
                }

                return R"({"status":"done"})";
            }
        );

        // 3. 快同步内联工具 (<~1ms, IO 线程直接计算并返回)
        fast_tool(ctx, "my_fast_tool", "Fast tool depict", R"({"type":"object","properties":{}})",
            [](MyPluginCtx& c, std::string_view args_json, std::string_view tid) -> std::string {
                return R"({"result":42})";
            }
        );

        // 4. Task 锚定协程工具 (可精确 co_await sleep / yield / call_tool / offload)
        tool(ctx, "my_async_tool", "Async coroutine tool", R"({"type":"object","properties":{}})",
            [](MyPluginCtx& c, std::string_view args_json, OpCtl ctl) -> Task<std::string> {
                co_await sleep(c, 100);
                ctl.throw_if_cancelled();
                // 跨插件互调: co_await call_tool(c, "other_tool", "{}", ctl.sessionId());
                co_return R"({"status":"ok"})";
            }
        );

        // 4b. 受控轮询工具: 业务体是 asio 协程, 等待插件本地 reactor 上的内核就绪事件
        //     (socket/子进程管道/文件/本地 timer)。插件注册时声明"需要受控轮询驱动",
        //     桥据此在有在途操作时继续申请请求 (有进展立即续 / 无进展退避 10ms /
        //     空闲零开销), 不占宿主工作线程。详见 §16.5。
        polled_tool(
            ctx,
            "my_polled_tool",
            "Async IO tool driven by controlled polling",
            R"({"type":"object","properties":{"url":{"type":"string"}}})",
            [](MyPluginCtx&     c,
               std::string_view args_json,
               std::string_view tid,
               std::string_view workDir,
               const PluginxxCancelToken* cancel) -> asio::awaitable<std::string> {
                ArgReader args(args_json);
                std::string workDirStr(workDir);
                if (pluginxx_cancel_is_requested(cancel)) {
                    throw CancelledException("cancelled");
                }
                // 业务体直接 co_await 现成的 asio 协程实现 (无需局部 io_context + run())
                co_return co_await doHttpGetAsync(args.raw(), workDirStr);
            }
        );

        // 5. 后台协作任务 (宿主托管: 自动注册 agentxx.agent.tasks, 卸载时宿主统一取消并精确等待退出)
        spawn(ctx, [](MyPluginCtx& c, OpCtl ctl) -> Task<void> {
            while (!ctl.cancelled()) {
                co_await sleep(c, 5000);
                if (ctl.cancelled()) break;
                // 周期采集并发布事件
            }
        });

        // 6. 钩子 (7 个钩子点: agent_start/end, model_start/run/end, tool_start/end)
        hook(ctx, AGENTXX_PLUGIN_HOOK_MODEL_START, [](MyPluginCtx& c, std::string_view in) {
            // ...
        });

        // 7. 能力 (跨插件通用 RPC 通道)
        capability(ctx, "my.cap", [](MyPluginCtx& c, const PluginxxHost* caller,
                                     std::string_view method, std::string_view args) {
            return "{}";
        });

        return 0;
    }
);
```

### SDK 核心基础设施组件

1. **导出宏 (`AGENTXX_PLUGIN_AGENT_EXPORT` / `AGENTXX_PLUGIN_CLIENT_EXPORT`)**:
   - 自动生成 `agentxx_plugin_agent_get_info` / `create` / `destroy` (及 client 侧对应符号)
   - 包含完整的 `guardCall` 异常守卫（向日志报告 C++ 异常，阻止异常越界穿透 C ABI）
   - 自动创建实例上下文堆对象（继承自 `PluginBase`），调用 `ctx->init(host)` 并挂载资源
2. **链式 Schema 构建器 (`ToolSchemaBuilder`)**:
   - 经 `ctx.schema("tool_name")` 获得构建器实例，通过 `.string()`, `.integer()`, `.number()`, `.boolean()`, `.array()`, `.stringArray()`, `.enumString()` 等链式声明参数
   - 自动从宿主 `toolPrompt` 中提取当前语言下的参数说明并优先覆盖默认注释，最后调用 `.build()` 输出标准 JSON Schema 字符串
3. **强类型参数提取器 (`ArgReader`)**:
   - 宽容解析输入 JSON：容忍格式宽松，并在类型不匹配时尝试智能自愈（如字符串 `"true"`/`"1"` 转布尔，数字转字符串等）
   - 提供 `args.require<T>("key")`（若缺少则置错误标记）、`args.get<T>("key")`（返回 `std::optional<T>`）
   - `args.ok()` 与 `args.errorMessage()` 方便快速校验和返回参数错误
4. **事件驱动取消注册中心 (`CancelRegistry`)**:
   - 已提升至插件开发框架通用基础设施（`agentxx::plugin::CancelRegistry`），各实例独立持有 `ctx.cancelRegistry`
   - `ctx.init()` 自动订阅 `plugin.agentxx.round_start` 事件，新轮次开始时自动清除对应会话的历史已取消标记
   - 支持 `registerCallback(key, cb)` 注册基于会话标识的取消回调，支持 RAII `ScopedRegistration` 守卫，提供排他互斥与防悬挂锁保护，避免回调访问已析构的局部资源
   - 适用于长时间运行的外部进程或底层阻塞 IO（如 `agentxx_execute_command`），一旦宿主发起取消即可毫秒级即时终止子进程组，无需等待轮询间隔
5. **统一异步操作模型 (操作 start/cancel)**：
   - 工具/钩子/能力均为 `start` (IO 线程非阻塞启动) + `cancel` (协作式) 操作，终结经 `PluginxxOperatorNotify.done(status,payload)` 恰好一次上报
   - `Task` 协程帧先销毁后 `done` 上报，支持 `offload` 阻塞池委托与 `call_tool`/`invoke_cap` 锚定互调
   - `polled_tool`（受控轮询）与 `blocking_tool`/`fast_tool` 并列：业务体是 `asio::awaitable`，
     等待插件本地 reactor 上的内核就绪事件，由桥按声明式受控轮询推进（§16.5）
   - hook / capability 的 SDK helper 按**返回类型严格分发**：返回 `void`/字符串的同步业务在
     调用内完成；返回 `Task<T>` 的异步业务由统一 root adapter 收束（provider 句柄可取消，
     完成通知在协程真正结束后发出，输入视图由拥有型 `Request` 保证跨挂起点有效）
   - `graph_node` 使用同一 root adapter 注册自定义图节点类型：快同步节点返回节点输出
     JSON（`std::string`），异步节点返回 `Task<std::string>`；node/config/state/thread_id
     由拥有型 `RootRequest` 保证跨挂起点有效，`run_cancel` 置取消标志并取消嵌套 awaiter
6. **公共便捷助手 (各插件重复手写的三件小事)**:
   - `pluginLog(ctx, level, msg)` / `pluginLog(host, logIf, level, msg)`: 实例日志输出
     (上下文为空静默; 后者供直接持有宿主 + 日志接口表的插件使用)
   - `pluginStrdup(host, s)`: 经宿主 alloc 复制 C 串, 供 C ABI `char*` 出参直接赋值
   - `ctxGuardLogger(ctx)`: `guardCall`/`guardCallVoid` 异常守卫的日志闭包 (error 级)
   - `jsonEscape(host, jsonIface, text)`: 文本 → JSON 字符串字面量 (含引号);
     优先经宿主 `json_escape` 接口, 接口缺失/失败时回退本地转义, 供手工拼装 JSON 文本
   - 插件内 `using agentxx::plugin::xxx;` 引入后按原名调用 (内置插件已统一改用)

**后台任务 spawn (宿主托管)**：`spawn` 启动的后台协作任务 (如周期采集 `while(!cancelled()) { offload; sleep; }`) 自 API v1 起注册到宿主 `agentxx.agent.tasks` 接口表，与工具/能力 op 同构管理：

- **注册**：`spawn()` 内部自动调 `register_task` (io 线程) → 宿主把句柄推入实例 `outstandingOps` (与工具 op 同列表) 并持 `inflight` (存活标记)
- **运行**：协程照常经 `sleep`/`offload` 挂起于宿主；宿主无感，句柄静默
- **卸载**：插件卸载时宿主 `detachAll` 统一取消 (调插件 cancel_fn: 置 cancelFlag + 唤醒挂起的 sleep/offload) → 协程 `while(!cancelled())` 退出 → `finishIfDone` (帧销毁后经 `notify.done` 恰好一次上报) → 宿主 `guard.reset` (inflight-1) + 回收句柄 → `waitInflightZero` 精确等待归零 → `dlclose` 安全，无协程帧悬挂/UAF
- **无降级**：宿主无 `agentxx.agent.tasks` 表或注册失败时 `spawn` 直接失败 (不提供无人托管的自管协程退化路径)
- **线程约束**：`cancel_fn` 由宿主在 io 线程回调 (协作式)；`notify.done` 可从插件任意线程上报 (宿主 `OpCore::onDone` 原子 CAS + 投递回 io)；kit 协程完成路径恒在 io 线程


---

## 7. 插件分类与编译模式

1. **按功能划分**：
   - **Agent 插件**：扩展会话执行流 (Tool / Hook / Event / Capability / Resources)
   - **Client 插件**：扩展 TUI/CLI 界面 (StatusItem / Panel / InfoSection / Command / ToolDecor)
   - **双端插件**：同时导出 Agent 与 Client 入口，一份动态库同时服务两端；跨端通信统一走 Wire (`PluginData` agent→client / `PluginDataUp` client→agent)

2. **编译与分发模式**：
   - **独立动态库 (默认)**：编译为独立动态库，经 `agentxx-config.yaml` 的 `plugins` 字段按 `path` 动态加载 (支持插件目录含 `plugin.yaml` 清单分派)
   - **内置合并编译**：`AGENTXX_PLUGIN_BUILTIN_LIST` 指定插件源码直接编译进 `libagentxx`，运行期无需外部动态库零开销调用；此时 `plugins` 段仍可配置参数，`path` 可简写为 `builtin://<name>` 或 `name: <name>` (无需外部目录)，并可通过 `config` 指定插件配置文件所在目录/文件路径

---

## 8. Agent 侧接口表一览

| IID | 版本 | 能力 |
|-----|------|------|
| `agentxx.agent.tools` | 1 | `register_tool/unregister_tool`, `call_tool_async/op_cancel` (插件互调, cb 保证 IO 线程 post) |
| `agentxx.agent.permission` | 1 | `register_tool_permission/unregister_tool_permission` (工具权限限制由工具来源方声明) + `check_paths` (批量路径权限三态查询, 不发起询问/中断; 见下方说明) |
| `agentxx.agent.hooks` | 1 | `register_hook/unregister_hook` (7 钩子点, 操作) |
| `agentxx.agent.events` | 1 | `subscribe/unsubscribe/publish` (topic 自动加 `plugin.` 前缀, 载荷 JSON) |
| `agentxx.agent.capabilities` | 1 | `register_capability(_ex)/unregister/has_capability`, `invoke_capability_async/op_cancel` |
| `agentxx.agent.scheduler` | 1 | `is_io_thread/post_to_io/sleep/op_cancel/offload` (sleep=宿主计时器; offload=阻塞池委托, 需 cancel_token) |
| `agentxx.agent.coroutine_runtime` | 1 | 通用协程驱动: `request_driver/cancel_driver/is_io_thread` (driver ticket/wake 协议, 见 §16) |
| `agentxx.agent.session` | 1 | `get_share_store/add_share_store/emit_message_tip` (IO 线程) |
| `agentxx.agent.plugins` | 1 | `list_plugins/get_plugin/get_own_info` (JSON) |
| `agentxx.agent.config` | 1 | `get_config/get_plugin_args/get_tool_prompt/get_session_work_dir/get_plugin_config_path/get_language/set_language` (get_session_work_dir session_id 为空时返回默认会话工作目录；`get_plugin_config_path` 返回 yaml `config` 归一化绝对路径，可指向文件/目录；`get_language/set_language` 查询或指定运行时生效语言) |
| `agentxx.agent.model` | 1 | `get_config` (主模型及关联配置 JSON) |
| `agentxx.agent.cancel` | 1 | `is_cancelled(sessionId)` (advisory, 权威通知为 cancel 回调) |
| `agentxx.agent.prompt` | 1 | `get_prompt/set_prompt` (宿主提示词读写) |
| `agentxx.agent.json` | 1 | `json_get_string/json_escape` |
| `agentxx.agent.log` | 1 | `log(level, msg)` (0 trace .. 4 error) |
| `agentxx.agent.resources` | 1 | `register_skill_dir/memory_file/mcp_server` (仅初始化阶段) + `get_own_resources` (冻结后不可变) |
| `agentxx.agent.graph` | 1 | 执行图扩展: `register_node_type/unregister_node_type` (插件自定义节点类型, 注入 per-agent GraphRegistry) + `get_graph_json/get_graph_name/set_graph_json` (查看/修改宿主执行图, 默认名 `agentxx.default`; 插件加载阶段生效, 宿主构建 engine 前处理) |
| `agentxx.agent.tasks` | 1 | 后台任务宿主托管: `register_task/cancel_task` (kit `spawn` 自动注册; 宿主登记句柄 + 持 inflight + `notify.done` 完成通知 —— 卸载时 detachAll 统一取消 + `waitInflightZero` 精确等待, 无协程帧悬挂; `notify` 为出参, `notify.done` 可从插件任意线程回调) |

### 工具权限声明的语义 (agentxx.agent.permission)

工具权限限制**由工具来源方 (插件) 声明**, 宿主权限中间件只负责按声明执行统一判定:

- **声明内容** (`AgentxxPluginToolPermissionSpec`): 工具名 (须为本实例已注册的工具)、
  作用域 (读/写, 各自一套规则)、目标来源 (`无` / `路径` / `文本`)、目标参数名
  (工具 args 中的字段名; 目标值按**参数实际 JSON 类型**处理 —— 字符串为单目标,
  数组 (如 `file_patterns`) 自动逐项判定) 与可选的权限分类文本 (权限询问卡片显示,
  留空按作用域生成)
- **声明落地**: 宿主把声明交给权限中间件 (工具名 → 声明表)。工具调用时中间件按声明
  从 args 解析目标 (路径目标按会话生效工作目录规范化为绝对路径), 再按既有规则判定
  (白/黑名单、`permission.mode` 默认规则、用户"记住本次选择"、工作区隔离、完全授权);
  插件不参与判定
- **未声明的工具不参与权限判定** (直接放行): 权限限制随工具来源走, 与仅加载部分插件
  的场景一致
- **生命周期**: 工具注销、插件禁用/卸载时宿主自动撤销对应声明 (重新 start 时按新声明
  恢复); 声明属于附加能力, 宿主未装配权限中间件时注册返回非 0, 插件可忽略
- **失败拒绝**: 非本实例所有的工具名、未知作用域/目标来源取值都会被拒绝并记日志
- **模式/前缀参数工具的分工** (`agentxx_filesystem` 的 `glob` / `grep` / `list`):
  声明式目标 (如 `file_patterns`、可为通配模式的 `path`) 决定"是否询问用户一次"
  (目标是模式表达的扫描起点); 模式/遍历实际产生出的每条路径必须在工具内用
  `check_paths` 逐项复核后再访问或输出 —— 否则 `**` 之类模式会绕过"针对子目录的
  拒绝规则"(允许 `/x/a/*` 但拒绝 `/x/a/b/c` 时, 只检查扫描起点 `/x/a` 会放行 `b/c`
  下的文件)。`list` 采用**待处理缓冲 + 满批复核**: 条目先缓冲 (256 条/批), 批量查询
  后只输出"已明确允许"的条目, 因此 `limit` 只统计真正输出的条目 (被拒条目不会
  占掉额度), 且不会退化成"每个条目一次跨线程查询"
- **`check_paths` 语义**: 批量入参 (路径数组 + 作用域 + 会话), 出参三态
  (`DENY` 已明确拒绝 / `ALLOW` 已明确允许 / `ASK` 未获批准); 判定口径与工具调用
  权限检查一致 (工作区隔离 → 配置拒绝 → 完全授权 → 规则表 → `noRuleOperator`),
  区别仅在于 `ASK` 不发起询问。工具侧约定: `DENY` 丢弃, `ASK` 同样不作为
  (未获批准的范围不进入结果); 空路径/规范化失败按 `ASK` 返回 (不按已批准处理)。
  任意线程可调用 (非 io 线程由宿主投递到 io 线程同步等待), 单次批量建议 ≤ 512 项
  (宿主在 io 线程执行), 宿主上限 16384; 宿主未装配权限中间件时返回非 0, 工具应
  跳过过滤 (保持原行为) 而不是把所有路径当成拒绝
- **kit 便捷层**: 声明侧 `registerReadPathPermission(ctx, tool, "path")` /
  `registerWritePathPermission(...)` (最常用的"路径参数 + 读/写"形态) 与通用
  `registerToolPermission(ctx, ToolPermissionSpec{...})`; 查询侧
  `filterPathPermissions` (分批 + 同路径去重, 返回等长允许标记) /
  `checkPathDecisions` (原始三态) / `checkPathDecision` (单路径)

### 工具错误与参数检查约定 (抛异常)

工具的参数检查失败与其它错误**统一走异常**, 不再返回编码后的错误 JSON:

- **参数检查失败抛 `std::invalid_argument`** (消息写清是哪个参数、什么原因, 例如
  ``Arg `path` is empty``); 运行期/内部错误 (环境缺失、命令执行失败、上下文不可用)
  抛 `std::runtime_error`。取消路径抛取消异常 (`neograph::graph::CancelledException`;
  插件侧抛 `pluginxx::CancelledException`, 由 SDK 边界转换为 CANCELLED)。
- **宿主统一格式化**: 工具抛出的普通异常由 `ToolcallWrapNode` (`lib/src/nodes/toolcall.cpp`)
  捕获, 工具结果文本写为 `[Exception aborted: <what>]`; 取消/中断异常按控制流处理
  (取消轮次而不是记成工具错误)。因此工具不需要自己拼错误结构, 模型也能稳定识别
  错误结果。
- **插件侧写法**: 在工具回调里直接 `throw` 即可 —— SDK 的 `fast_tool` /
  `blocking_tool` / `polled_tool` 边界会捕获异常并上报 FAILED (取消异常上报
  CANCELLED), 宿主 `awaitPluginOp` 再以 `std::runtime_error(<原消息>)` 抛出, 最终落到
  同一格式。**不要**返回 `{"error":"..."}` 这类字符串: 错误与正常结果会混在同一形态里,
  调用方 (含模型) 无法可靠区分。
- **例外 (属正常结果, 保持为结果文本)**: 无匹配、空结果、查询目标不存在之类"调用成功但
  没有内容"的情况按普通结果返回 (如 `No match found`, 不带 error JSON 包裹); 权限
  拒绝由框架提前返回 `[Permission denied]`, 工具无需处理。
- 内置工具 (`lib/src/tools`) 与内置插件 (`agent/plugins/*`) 已按该约定改造 (2026-09);
  `agentxx_filesystem` 在异步/受控轮询包装层把异常统一转回 `[Error] <msg>` 文本
  (保持该插件历史结果形态与渲染约定), 但工具实现内部同样以抛异常方式报告参数错误。

---

## 9. Client 侧接口表一览

| IID | 版本 | 能力 |
|-----|------|------|
| `agentxx.client.ui` | 1 | `register_status_item/update/unregister`, `register_panel/update/unregister`, `register_info_section/update/unregister`, `register_command/unregister`, `show_toast`, `update_tool_decor(tool_call_id, decor_json)`, `register_tool_renderer(spec)/unregister_tool_renderer(tool_name)`, `bind_action_handler/unbind_action_handler`, `open_overlay/close_overlay` |
| `agentxx.client.events` | 1 | `subscribe/unsubscribe` (事件见 `AgentxxClientEvent`: READY/CONN_STATE/USER_INPUT/DELTA/TURN_END/SESSION_SWITCH/PLUGIN_DATA) |
| `agentxx.client.session` | 1 | `get_client_state` (快照 JSON), `send_user_input`, `request_cancel` |
| `agentxx.client.wire` | 1 | `send_plugin_data(event, json)` → 服务端 `client.{插件}.{event}` |
| `agentxx.client.self` | 1 | `get_own_info/get_plugin_args/get_plugin_config_path` (后者返回 yaml `config` 归一化绝对路径) |
| `agentxx.client.json` | 1 | `json_get_string/json_escape` |
| `agentxx.client.log` | 1 | `log(level, msg)` |

**版本口径**: `agentxx.client.ui` 表自身 `version` 恒为 1 (表结构未变)。新增展示能力一律走
**数据层** (新组件 kind / 新字段) 或**新增接口表**, 不在表尾追加成员 —— SDK 侧的接口校验是
`version != 1 || struct_size < sizeof(Iface)` 即整表判为不可用, 表尾追加成员会让新插件在
老宿主上丢掉整张表 (面板/状态栏/渲染器全部失效)。

清单声明层约定 (`plugin.yaml` 的 `interfaces.require/optional`, 按前缀归属侧):

- client 侧应声明**细粒度能力名** (`agentxx.client.msg_decor` / `...panel` / `...toast`
  等, 各自映射到 ui 表的非空成员), 宿主据此判断插件功能是否可用; 未实现的预留名
  一律视为"宿主不支持" (保守失败)
- `agentxx.client.ui` (ui 表 IID) 表示**表整体**: 只有覆盖表内全部子能力的宿主才声明
  它 (TUI); 插件声明表整体等价于"需要全部子能力", 子能力不全的宿主 (CLI) 会按缺失
  处理 (`optional` 告警 / `require` 跳过加载)。因此只用到工具特化渲染的插件应声明
  `agentxx.client.msg_decor`, 而不是表整体 —— 否则会被误报为"缺少 agentxx.client.ui,
  相关功能停用"
- 展示扩展表另有细粒度能力名用于"组件集合"判断 (与表成员无关, 只作能力协商用):
  `agentxx.client.components` = 宿主能渲染 `agentxx.ui.item` 全量组件 (含表格/树/图表/
  容器), `agentxx.client.form` = 宿主支持插件表单 (控件 + 提交回传),
  `agentxx.client.layout` = 宿主会上报展示区域尺寸 (事件 + 快照)。插件在
  `get_client_state().interfaces` 里查这些名字决定推送"新组件"还是降级为旧 kind;
  老宿主不认识这些名字 → 插件降级, 不报错、不静默丢内容。

### 9.1 客户端 UI 组件描述 schema (`agentxx.ui.item`)

面板 (`update_panel`)、Info 段落 (`update_info_section`)、工具装饰 (`update_tool_decor`)、
工具渲染器 (`items_json`)、自定义 overlay (`open_overlay(CUSTOM)` 的 payload) 与中断描述的
内容块都使用同一套组件描述: JSON 数组 (或 `{"items":[...]}`)，每项一个组件。

通用字段 (所有 kind 可用): `kind` / `id` / `indent` / `color`(或旧写法 `role`) / `bold` /
`dim` / `wrap` / `fallback` / `action` / `args` / `w`(横排内的列宽) / `when`(预留: 条件
显示表达式, 当前只做解析与往返保留)。
`color`/`role` 取语义色名 (`normal`/`hint`/`accent`/`error`/`tool`/`thinking`/...);
`hint` 只表示取色 (本身就是弱化灰), 不隐含弱化, 需要弱化的项显式写 `"dim": true`;
`title` 隐含加粗, 不需要时显式写 `"bold": false`。

| kind | 用途 | 关键字段 |
|------|------|----------|
| `text` | 文本行 | `text` |
| `markdown` | markdown 富文本 | `text` |
| `diff` | 差异对比 (自适应 side-by-side) | `path` / `old_str` / `new_str` |
| `separator` / `gap` | 分隔线 / 空行 | `lines` |
| `badge` | 状态点 + 文本 | `text` |
| `diagram` | 内联 mermaid 状态图 | `mermaid` |
| `button` (旧名 `action`) | 可点按钮 | `label` / `action` / `args` / `prefix` / `style` |
| `progress` (旧) / `meter` | 条形计量 (阈值配色) | `value` / `total` / `width` / `label` / `unit` / `thresholds` |
| `sparkline` | 迷你趋势图 (块字符; 宽度不足自动分桶) | `data` / `height` / `min` / `max` / `colors` / `showLast` |
| `kv` | 键值对 (两列对齐) | `items:[{k,v,vColor}]` / `sep` / `kw` |
| `table` | 表格 (表头/列对齐/截断/可点单元格) | `columns:[{title,align,w,color}]` / `rows` / `header` |
| `tree` | 层级列表 (连接线; 节点可点; 有子节点的行在宿主提供折叠状态时可点击展开) | `nodes:[{label,color,action,children}]` / `connector` |
| `row` | 横向组合 (列宽权重 + 对齐; `align:"stretch"` 铺满可用宽度) | `items` / `gap` / `align` |
| `box` | 分组框 (标题 + 边框 + 内边距) | `title` / `border`(none/square/round/light) / `pad` |
| `collapse` | 可折叠分组 (宿主维护展开状态) | `id` / `title` / `expanded` |
| `control` | 交互控件 (checkbox/select/buttons/number/text) | `id` / `control` / `label` / `options` / `default` / `commitOnPick` / `min` / `max` / `step` / `integer` |
| `checkbox` / `select` / `buttons` / `number` / `input` | 控件的短写法 (等价 `control` + 对应形态) | 同 `control` |
| `submit` | 表单提交行 | `label` / `cancelLabel` |
| `custom` | 派发到内置组件或组件树 | `component`(空或 `"components"` 用 `props.items`) / `props` / `fallback` |
| `canvas` | **预留**: 完全自绘 (本版只解析与降级) | `fallback` (渲染降级文本) |

约束与降级:

- 解析上限: 嵌套深度 8 / 单层元素 512 / 表格 512 行 16 列 / 树 1024 节点 /
  趋势图 4096 点 / 单项文本 64 KiB; 越界按"截断或丢弃"处理, 不使整份描述失效
- **单条描述体积上限 1 MiB** (`agentxx::plugin::kUiJsonMaxBytes`): 面板/Info/状态栏/
  装饰/overlay/工具渲染结果的入口在越界时**拒绝整条更新** (返回非 0) 并记日志 ——
  注册表保持上一次成功内容, 不会留下半截状态
- 注册表条目带内容 `version` (面板/Info/状态栏每次成功更新递增; 工具装饰与工具渲染
  缓存同理), 供缓存 key 与诊断使用
- 未知 kind: 渲染 `fallback` 文本 (无 `fallback` 则跳过), 老宿主同样按此降级 —— 插件推送
  新组件时应带 `fallback`, 并按 `get_client_state().interfaces` 判断宿主能力
- 行式前端 (CLI / 日志 / FFI 文本宿主) 走 `agentxx::ui::plainText`: 表格转列对齐文本、
  树转连接线前缀、趋势图转块字符 + 末值、计量条转 `[####----] 72%`, 控件转
  "标签: 候选项/默认值 (形态)"; 中断描述里的扩展组件块同样经该路径输出
- 中断描述与组件层之间只有一份映射: `agentxx::middleware::itemOf(block)` /
  `blockOf(item)`(+ `preset::blocksOf(ui)` 用构建器拼中断块), 新增组件不必改中断层
- 插件构建组件树不必手写 JSON: `agentxx/ui/build.h` 的 `agentxx::ui::Items` 提供链式构建器
  (`text/kv/meter/sparkline/table/tree/row/box/collapse/checkbox/select/number/submit/...`),
  `ClientPluginBase` 提供 `setPanelItems/setInfoSectionItems/showItemsOverlay/panelItems/`
  `setToolDecor/form` 直接提交:

  ```c++
  agentxx::ui::Items ui;
  ui.box("System", agentxx::ui::Items{}
          .kv({{"Model", model}, {"Tokens", tokens}})
          .meter(cpuPct, 100, {.width = 20, .label = "CPU", .unit = "%"}))
    .table({.columns = {{"File", "left", 0}, {"Size", "right", 8}}, .rows = rows})
    .checkbox("detail", "显示细节", detailOn)
    .submit("应用", "取消");
  ctx.setPanelItems(ctx.panel, ui);
  ```


### 9.2 插件表单 (控件与结果回传)

- 面板 / Info 段落 / 自定义 overlay 里都可以放控件 (上表的 `control` / `submit`);
  **控件状态由宿主维护** (值、勾选、选中项、校验提示、焦点), 插件代码不进入 UI 线程。
- 结果经既有的动作通道回传 (`bind_action_handler` 绑定的回调):
  - 提交: `action_id = "__submit"`, `action_args = {"values": {控件 id: 值}}`
    (checkbox→布尔 / select|buttons→候选项 `value` / number→数值 / text→字符串)
  - 取消: `action_id = "__cancel"` (无参数)
  - `commitOnPick: true` 的候选项点击即提交: `action_id = 控件 id`, 参数同上
- 交互: 点击控件即聚焦 (字符键进入输入框), `Tab`/`Shift+Tab` 在控件间移动,
  `Esc` 释放焦点, 回车提交; 数值控件提交前校验 `min/max/integer` (失败显示提示且不提交)
- 键盘 (焦点控件上的按键, 面板/Info/overlay 与中断表单完全一致):
  - `buttons`: `←`/`→` 循环切换选中项; `select`: `↑`/`↓` 移动选中项;
    `number`: `↑`/`↓` 按 `step` 步进 (受 `min`/`max` 约束); `checkbox`: 空格翻转
  - 输入框 (`text`/`number`): 首次输入替换缺省值, `Backspace` 删除一个字符,
    `Delete` 清空; 数值框只接受数字与小数点/正负号 (其余按键消费但不改动)
- 中断表单 (`interrupt_ui.h`) 使用同一套控件语义与外观, 只是结果去向不同
  (回传中断结果 `{"values":{...}}`, 由 agent 侧中间件消费): 描述里的控件与提交行
  由 `ui_components` 渲染 (标签/说明的 i18n 键在转换时解析), 点击/键盘/校验/取值
  全部复用同一实现; 命中为"行元素框 + 行内区域"两级判定
  (提交行缺省文案沿用中断词表 `确认` / `✕`, 与插件表单的 `提交` / `取消` 区分)

### 9.3 通用 overlay (`open_overlay`)

- `type`: `MERMAID` (payload = mermaid 源码) / `TEXT` (payload = 原文, `extra.markdown`
  控制是否按 markdown 渲染) / `DIFF` (payload = `{path,old_str,new_str}`) /
  `CUSTOM` (payload = `{"items":[...]}` 组件树, 见 9.1)。
- `extra_json` 支持尺寸与外观选项 (数据层, 老宿主忽略未知键):

  | 键 | 取值 | 说明 |
  |---|---|---|
  | `size` | `auto` / `compact` / `normal`(缺省) / `large` / `full` | 预设占屏比例 |
  | `width_frac` / `height_frac` | 0~1 | 显式比例, 与 `size` 同时给出时以它为准 |
  | `footer` | 布尔 (缺省 true) | 是否显示底栏提示 |
  | `scroll` | 布尔 (缺省 true) | `false` 表示内容不需滚动 (隐藏滚动提示) |
  | `stack` | 布尔 (缺省 false) | 已有 overlay/核心弹窗时: `false` 替换 (last-wins), `true` 保留现有并丢弃本次 |

- 在 overlay 里可以放控件与提交行 (见 9.2): 点击/键盘由宿主的表单状态处理, 提交经
  动作通道回传 `__submit`, 取消回传 `__cancel` (owner 固定 `__overlay`, 由实例级
  动作绑定接住)。

### 9.4 展示区域尺寸感知

面板与 Info 段落由宿主布局, 插件默认拿不到可用宽度。宿主 (声明能力名
`agentxx.client.layout`) 提供两件套:

- **事件 `AGENTXX_CLIENT_EVT_UI_LAYOUT`**: 载荷
  `{"regions":[{"id":"<面板/段落 id>","w":60,"h":20}]}`
  - 首次布局完成、终端尺寸变化、侧边栏宽度拖拽、面板激活/打开时投递
  - 仅在数值变化时投递 (每帧上报会自然合并), 投递线程仍是 client io 线程
- **快照查询**: `get_client_state().regions` 与 SDK
  `ClientPluginBase::regionSize(regionId)` (返回 `{width, height}`; 未上报为 0)

### 9.5 定时器 (`agentxx.client.timer`)

插件不能自己起线程/定时器 (`agentxx.agent.scheduler` 的 sleep 只在 agent 侧);
client 侧需要"每隔一段时间刷新面板"的能力时用本表 (能力名
`agentxx.client.timer`, 表 IID 同名):

```c
typedef struct AgentxxClientTimerIface {
    int32_t  version; uint32_t struct_size;
    AgentxxTimer*(*set_timer)(const PluginxxHost*, const AgentxxTimerSpec*);
    void        (*cancel_timer)(const PluginxxHost*, AgentxxTimer*);
    int32_t     (*is_visible)(const PluginxxHost*, const PluginxxStringView* owner_id);
} AgentxxClientTimerIface;
```

- `AgentxxTimerSpec`: `interval_ms` (下限 50, 更小按 50 收敛) / `repeat`
  (0 = 一次性, > 0 = 周期触发次数上限) / `pause_when_hidden` /
  `owner_id` (关联的面板/Info 段落 id) / `on_timer` + `user_data`
- **回调线程**: client io 线程 (与其它插件回调一致, 插件代码永不进 UI 线程);
  回调内可直接更新面板/Info/overlay 描述 (注册入口自身会投递到 io 线程)
- **门控**:
  - 宿主动画等级为 `Disabled` 时**拒绝注册** (返回 NULL, 插件降级为静态展示)
  - `pause_when_hidden != 0` 且 `owner_id` 当前不可见时跳过本次回调并**顺延**
    (计时继续, 顺延期间不消耗触发次数 —— 一次性定时器会在区域可见后的下一次
    到时触发) —— 高频刷新 (< 200ms) 的面板应打开它, 避免用户切走后白耗 CPU
  - `is_visible` 查询当前可见性 (宿主未上报过 → 0 = 不可见)
- **上限与清理**: 单实例 ≤ 8 个定时器; 插件禁用/卸载时宿主全部取消
  (禁用后重新启用需由插件 `start` 事务重新注册)
- SDK: `ClientPluginBase::registerTimer(intervalMs, fn, opts)` 返回
  `std::shared_ptr<TimerHandle>` (析构自动 `cancel_timer`) —— 插件把它存进自己的
  实例上下文即可, 无需在 `stop` 里逐个取消

### 9.6 全局快捷键 (`agentxx.client.keybind`)

插件要响应按键 (打开自己的 overlay/触发刷新等) 时用本表 (能力名
`agentxx.client.keybind`, 表 IID 同名):

```c
typedef struct AgentxxClientKeybindIface {
    int32_t  version; uint32_t struct_size;
    AgentxxKeybind*(*register_keybind)(const PluginxxHost*, const AgentxxKeybindSpec*);
    void           (*unregister_keybind)(const PluginxxHost*, AgentxxKeybind*);
    int32_t        (*list_keybinds)(const PluginxxHost*, PluginxxString* out);
} AgentxxClientKeybindIface;
```

- 键位描述: 修饰键 + 主键, `+` 连接, 不区分大小写;
  修饰键 `ctrl`/`alt`/`shift`/`super` (别名 `control`/`cmd`/`win`/`meta`);
  主键单字符或 `f1`~`f24` / `esc` / `enter` / `tab` / `space` / `backspace` /
  `delete` / `insert` / 方向键 / `home` / `end` / `pageup` / `pagedown`
  (例: `"ctrl+alt+k"` / `"f9"` / `"ctrl+shift+space"`)
- **规范化**: 宿主按"小写 + 修饰键固定顺序 (ctrl/alt/shift/super)"存储;
  界面侧把按键事件转成同一格式后比较 (TUI 见 `tui_keybind.h`,
  `keybindOfEvent`), 两侧规则一致
- **限制**: 无修饰键的可打印字符**不能**作为全局快捷键 (注册失败, 避免与输入框
  抢字符); 同键位只允许一个注册者 (先注册者优先, 后来者拿到 NULL);
  单实例 ≤ 16 个
- **优先级**: 全局快捷键 > 表单控件焦点 > 普通按键 (有控件焦点时快捷键仍生效);
  模态弹窗打开时快捷键不触发 (弹窗优先)
- **回调线程**: client io 线程; 派发路径为"UI 线程查表 → 投递 io 线程 → 复查
  插件存在/启用 → 回调"
- SDK: `ClientPluginBase::registerKeybind(keys, description, fn)` 返回
  `std::shared_ptr<KeybindHandle>` (析构自动注销);
  `keybindListJson()` 取当前已注册列表 (排查冲突用)
- **界面查看**: TUI 设置弹窗的「快捷键」条目显示插件已注册的快捷键条数, 激活后打开
  只读列表弹窗 (键位 + 说明 + 归属插件); 抢不到键位的尝试以"键位冲突"段列出
  (申请方 · 占用方), 便于用户排查"插件文档里的快捷键为什么没生效"。冲突记录随
  占用方注销/禁用/卸载 (键位空出) 或请求方禁用/卸载自动清除

插件按新宽度重新排列自己的组件并 `update_panel` 即可; 老宿主订阅该事件会失败
(返回 NULL), 此时按固定宽度排版。

### 9.7 状态栏项的富展示片段

`register_status_item` / `update_status_item` 的 JSON 除 `text` / `tooltip` 外，还可带
**单行**富展示片段（状态栏高度固定一行；`text` 作为无法渲染时的降级文本）：

| 键 | 形态 | 说明 |
|---|---|---|
| `segments` | `[{text,color}]` | 分色文本片段（按顺序拼接） |
| `sparkline` | `{data,min,max,color,colors,unit,showLast}` | 迷你趋势图（`height` 固定为 1） |
| `meter` | `{value,total,width,label,unit,thresholds}` | 条形计量（阈值配色） |

三者按 `segments → sparkline → meter` 顺序以空格拼接为一行，渲染走共享组件层；
只给 `text` 时行为与之前完全一致（纯文本 + 24 字截断）。

### 工具特化渲染架构 (Tool Rendering & Decor)

Agentxx 客户端采用统一的分层工具特化渲染机制，TUI 核心层完全解耦，不包含任何具体工具名称的硬编码：

1. **类型级工具渲染器 (`register_tool_renderer`)**：
   - 插件在 client 初始化时按 `tool_name` 注册特化渲染定义 (`AgentxxToolRenderSpec`)，TUI 渲染该工具消息 (实时流式或历史回溯) 时统一生效。
   - **历史回溯是主要场景之一**：会话重启恢复/重连/切换会话后回放的 `Tool` 消息没有实例级装饰
     (见第 3 条), 特化渲染完全依赖此处注册的渲染器 (输入只有消息自带的 `args_json`/`result_text`)。
   - 框架规则差异：**展开状态的头部显示名只由实例级装饰覆盖** (类型级渲染器命中时展开头保持
     原始 `toolName`)，渲染器始终覆盖**折叠头显示名/摘要**与**展开体 items**；折叠态是历史消息的
     默认形态 (见 `event_stream` 历史展开时 `collapsed=true`)，因此历史回溯观感与实时一致。
   - **双轨机制**：
     - **`<key, render_fn>` 回调函数**：提供 `AgentxxToolRenderFn`，接收 `AgentxxToolRenderInput` (`tool_name`, `args_json`, `result_text`, `is_finished`, `is_error`, `max_width`)，输出 `AgentxxToolRenderOutput` (`displayName`, `summary`, `items_json`)。适用于需要复杂参数解析、条件格式化或动态生成 UI 项的工具 (如 `read` 区间参数、`glob`/`grep` 模式与文件摘要、`edit` diff 差异对比)。
     - **预设模版 (`template_json`)**：当 `render_fn == NULL` 时，宿主按声明式模板自动从 `args_json` 中提取字段并格式化摘要，如 `{"displayName":"Search","summaryKey":"query"}` 或 `{"displayName":"Bash","summaryKey":"command"}`。
   - **通用 Diff 渲染**：展开体 `items_json` 新增支持 `{"kind":"diff","path":"...","old_str":"...","new_str":"..."}`，TUI 会通用化渲染为自适应屏幕宽度的 side-by-side 或统一差异对比，任何插件均可自由复用。
   - **SDK 辅助函数 (`registerToolRenderer`)**：
     `plugin_kit.h` 提供了基于现代 C++ Lambda 的辅助封装，抹平 C ABI 结构体与内存分配细节：
     ```cpp
     agentxx::plugin::registerToolRenderer(host, ui, "agentxx_filesystem_read",
         [](const agentxx::plugin::ToolRenderInput& in, agentxx::plugin::ToolRenderOutput& out) {
             out.displayName = "Read";
             ArgReader args(in.argsJson);
             out.summary = args.get<std::string>("path").value_or("");
         },
         ctx.shimStorage
     );
     ```
2. **宿主内置工具渲染器 (lib 内置工具, 无对应插件)**：
   - 少数工具由 **lib 内置实现**提供、没有对应插件 (当前为 `agentxx_share_store` 会话共享存储
     与 `agentxx_subagent` 子代理委派)，其特化渲染由宿主自身注册：
     `agentxx::plugin::registerBuiltinToolRenderers(mgr)`
     (见 [builtin_tool_renderers.h](/agent/lib/include/agentxx/plugin/builtin_tool_renderers.h))，
     内部经 `ClientPluginManager::registerBuiltinToolRenderer` 存入 UI 注册表的
     `builtinToolRenderers` 分表 (归属名固定为 `agentxx.core`)。
   - client 侧在 `setupClientPlugins` (agent/client/src/mode_runners.cpp) 装配时注册一次
     (本地/远程、TUI/CLI 各模式共用)；渲染与插件渲染器同一条路径 (client IO 线程执行 +
     语义渲染缓存), 展开体保持宿主通用展示 (不提供 items)。
   - 匹配优先级低于插件注册项 (插件可为同名工具注册渲染器覆盖内置渲染)；
     内置项与进程同生命周期，不随插件禁用/卸载失效。
   - 当前覆盖：`agentxx_share_store` → `Store` (`insert 152 lines → #7` / `get #7 [0, 100]` /
     `set #7` / `delete #7`)、`agentxx_subagent` → `Subagent`
     (`explorer · <任务首行>` / `4 tasks: explorer, coder, planner, ...`)。
3. **实例级工具装饰 (`update_tool_decor`)**：
   - 订阅 `EVT_DELTA` 的 `tool_start`/`tool_end` 后，按特定调用 `tool_call_id` 推送语义 JSON (优先级高于类型级渲染器)；
     典型实现见 `agentxx_planning` (Mermaid 状态图 + 动态待办列表)。
   - **只覆盖本进程内实时发生的调用**：重启客户端恢复会话、重连、切换会话时，历史 `Tool` 消息由
     `WireSyncPayload`/`WireViewMessagesPage` 回放，此时没有任何 delta 事件与装饰推送 ——
     这类消息的特化渲染必须由**类型级渲染器**从消息自带的参数/结果推导
     (见第 1 条; `agentxx_planning` 因此同时注册类型级渲染器与实时装饰, 两条路径共用同一内容构建函数)。
4. **优先级与降级路径**：
   - 渲染时查询顺序：`toolDecors` (按 `tool_call_id`) > `toolRenderers` (按 `tool_name`, 插件注册) >
     `builtinToolRenderers` (按 `tool_name`, 宿主内置) > 通用兜底展示 (原始 `toolName` + 参数/结果文本)。
   - 插件卸载/禁用时宿主自动摘除注册并还原兜底展示，启用时无损恢复 (宿主内置渲染器不受影响)。

5. **items 渲染与中断内容的复用边界 (重构后)**：
   - **静态块同一实现**：插件的 `items` (面板/Info 段/工具装饰/overlay) 与中断描述
     的内容块 (`agent/lib/include/agentxx/middlewares/interrupt_ui.h`) 在 TUI 侧
     复用同一套块渲染实现
     ([ui_items_render.h](/agent/client/include/agentxx-client/io/tui/ui_items_render.h),
     渲染产出"行模型", 渲染与高度估算同源); 颜色 role 映射 / 按钮配色 / diff 渲染
     仍来自
     [plugin_ui_items.h](/agent/client/include/agentxx-client/io/tui/plugin_ui_items.h)。
     两套数据结构**不共用同一 schema**。
   - **中断描述由生产者用预设模板生成** (权限卡片 `preset::permissionCard` /
     类型化输入表单 `preset::inputForm` / 确认卡片 `preset::confirmCard`,
     见 [interrupt_presets.h](/agent/lib/include/agentxx/middlewares/interrupt_presets.h));
     **插件自带中断 UI 的能力暂未开放**。描述块类型: 内容块 `text`/`markdown`/
     `diff`/`separator`/`gap`, 控件块 `control` (`buttons`/`select`/`text`/
     `number`/`checkbox`), 提交行 `submit`, 预留 `custom` (组件名 + 属性; 客户端
     组件渲染器未实现前渲染 `fallback` 文本)。
   - **协议内没有"参数类型"概念** (bool/int/enum 等已删除): 需要类型化输入时由
     生产者调用预设模板生成描述 (类型→控件的映射只存在于预设内)。
   - **仅插件 items 有此 kind**：`button`(`action_id` 派发到 `bind_action_handler`) /
     `progress` / `diagram`。
   - 结果契约: 中断结果恒为 `{"values": {"<控件 id>": 值}}` (空对象 = 未应答)。

6. **语义渲染缓存**：
   - 自定义 `render_fn` **只在 client IO 线程执行**：UI 线程提交
     `ClientToolRenderRequest` (tool_call_id / tool_name / args / result / 宽度等拥有型拷贝)，
     宿主在 IO 线程复查 renderer lease、实例 `enabled` 与可注册状态后持 lease 调用，
     把 `displayName/summary/items` 拷成宿主对象写入 `ClientToolRenderCache`；
     输出 JSON 的每个分配字段在成功/失败/异常路径都由宿主释放。
   - UI 线程只读缓存：未命中时本次通用回退，结果写入后由
     `PluginUiAdapter::onToolRenderUpdated` 通知重绘；消息块缓存 key 计入
     `ClientToolRenderCache::version(key)`，因此"回退 → 语义内容"能及时上屏。
   - 缓存条目记录产出插件与实例代次；禁用/卸载按插件失效并递增版本号，会话切换清空。
     旧 UI 快照因此只能回退，不会调用已卸载插件的函数指针。缓存条目与版本记录
     按写入顺序有界回收（默认上限 512，远超单屏可见块数），长时间会话不会按
     `tool_call_id` 无限增长。
   - 预设模版与 `toolDecors` 是纯宿主数据，仍在 UI 线程直接计算。

---

## 10. 会话资源贡献 (Skill / Memory / MCP)

- **声明式**：插件目录随 `plugin.yaml` 声明资源 (框架在 entry 成功后经 `AgentResourceApplier` 统一 `applyDecls` 应用，卸载/禁用时摘除)
- **编程式**：运行时经 `agentxx.agent.resources` 接口表动态注册/注销 (如 `agentxx_codegraph` 按 args 动态注册索引路径)
- 宿主对声明式+编程式资源做去重与生命周期管理 (所有权语义见 `resource_applier.h`)；失败项经 `AppendComponentNotification` 单独统计 (供客户端 Failed 组展示)

### 插件配置文件 (`config` 字段)

- `agentxx-config.yaml` 中每个 `plugins` 条目可通过 `config` 指定插件的配置文件所在目录或文件路径 (可指向文件或目录；支持 `~`/`${VAR}`/相对路径，宿主归一化为绝对路径后透传)
- 插件经 `agentxx.agent.config` (agent 侧) / `agentxx.client.self` (client 侧) 的 `get_plugin_config_path` 查询该路径 (返回 `NULL` 表示未配置)，自行判断类型并加载 (如目录下扫描 `*.yaml`、读取单个文件等)
- 典型用法：`config: ${AGENTXX_WORK_DIR}/config/my_plugin.yaml` 或 `config: ./my_plugin_config/` (相对工作目录)；SDK 中 `PluginBase::configPath()` 提供便捷封装

---

## 11. Worktree 与会话工作目录

- 插件经 `AgentxxConfigIface::get_session_work_dir(host, thread_id)` 取当前会话生效工作目录 (worktree 绑定优先；thread_id 为空时返回默认会话工作目录)
- `blocking_tool` 的 `workDir` 参数由 SDK 在 IO 线程预取并注入，避免 worker 线程跨线程 `ioCallSync`
- 文件系统 / 命令执行插件每次 `execute` 按注入 `sessionId` 动态解析路径，会话绑定后基准即时切换

---

## 12. Javascript 插件 (基于 QuickJS 引擎插件)

Agentxx 仅维护单一 C++ 插件基础设施；JS 脚本插件经内置 `agentxx_javascript_engine` 引擎插件承载：

- **统一插件模型**：所有插件都是 C++ 插件；JS 插件表现为标准 C++ 动态库外壳 (如 `example_js`) 附带 `plugin.js`
- **执行流程**：宿主加载 JS 插件壳 → 壳 `create` 只构造上下文 (解析自身 `plugin.js` 路径) → 壳 `start` 校验 `interpreter.js` 能力后经该能力把 `plugin.js` 交给引擎 → 引擎在专用线程中解析并执行脚本，把脚本声明的工具/钩子反向注册到宿主；脚本注册属于壳实例的 start 事务，因此壳的 start 完成必须等脚本加载结束 (异步 done)，失败由宿主回滚
- **引擎线程属于 start 事务**：`agentxx_javascript_engine` 的 `create` 只构造，`start` 才创建 JSRuntime 与专用 JS 线程并注册 `interpreter.js` 能力，`stop` 停止线程并释放 runtime；`disable → enable` 往返即一次 `stop + start`，引擎线程与脚本上下文按事务重建 (壳插件在自身 start 中重新加载脚本)
- **stop 不阻塞 IO 线程**：JS 线程可能在宿主 vtable 调用中等待 IO 线程，因此 `stop` 由独立收尾线程完成 `join` 与 runtime 释放，完成通知从该线程上报；JS 线程空闲时走调用线程直接收尾的快路径，使紧邻的 stop→start 顺序确定。停止后不再执行插件 JS：新任务被拒绝，队列任务按 `CANCELLED`/`FAILED` 终结，事件投递丢弃
- **`agentxx.callTool` 始终返回 Promise**：命中本引擎工具时同线程执行并把结果/内部 Promise 链到外层 Promise；命中宿主插件工具时经 `call_tool_async` 异步互调，完成/失败/取消事件投递回 JS 线程 settle。JS 线程不会同步等待宿主，A/B 脚本互调不存在线程自锁
- **脚本初始化是注册事务**：脚本顶层注册的工具/钩子/订阅/资源/定时器在顶层异常或脚本卸载时统一回滚（先撤销宿主注册，再释放 JSContext），不留悬垂 user_data；`hookStart` 与能力 `unload` 的完成通知在 JS 执行真正结束后发出
- 可自研脚本引擎插件 (Python/Lua 等) 替换或扩充脚本能力

---

## 13. 插件示例索引

| 插件 | 说明 |
|------|------|
| `example_plugin` | 原生 C++ 综合示例 (fast_tool/Task 协程/call_tool/sleep/**协程驱动桥**/**受控轮询 polled_tool**/钩子/事件/能力/client 入口) |
| `example_graph_node` | Graph 扩展示例 (自定义节点类型 + set_graph_json 改图, 需 `agentxx.agent.graph` 接口) |
| `example_js` | JS 脚本插件示例 (C++ 壳 + `plugin.js`) |
| `example_resources` | 会话资源贡献示例 (声明式与编程式 MCP/Skill/规则/会话环境) |
| `agentxx_filesystem` | 文件系统 6 工具 (list/read/write/edit/glob/grep, 含 `*_impl.h` 直测实现) |
| `agentxx_execute_command` | 命令执行 2 工具 (bash/windows, 含超时); 在 start 事务内探测 python/node (Windows 侧含 PowerShell) 并把结果写入自身工具提示词 |
| `agentxx_websearch` | 网络搜索 3 工具 (search/fetch/fetch_markdown) |
| `agentxx_rag_search` | 向量语义搜索 |
| `agentxx_string` | 字符串 2 工具 (html_to_markdown/regexp) |
| `agentxx_system` | 系统时间 (`get_current_datetime`) |
| `agentxx_system_monitor` | 系统资源监控 (Windows/Linux/Android/macOS; 工具 + 周期采集 + client 侧 Info/状态栏渲染) |
| `agentxx_planning` | 规划工具 + client 侧 Plan 渲染 (类型级工具渲染器「实时 + 历史回溯」+ 实时装饰 + Info 段落) |
| `agentxx_math` | 数学计算工具 (`agentxx_math_calculate`, 支持四则/幂/阶乘/位运算/逻辑/三角/双曲/对数/组合排列等函数与隐式乘法) |
| `agentxx_codegraph` | 代码索引 5 工具 (search/context/callers/callees/path) + client Info 栏 |
| `agentxx_screen_capture` | 屏幕捕获 (仅 Windows) |
| `agentxx_computer_use` | 键鼠控制 (仅 Windows, depends: screen_capture) |
| `agentxx_audio_stream` | 音频流捕获 (**全平台跳过构建**: WASAPI 实现未启用, 当前仅桩实现) |
| `agentxx_text_selection_monitor` | 文本选择监听 (仅 Windows UIAutomation) |
| `agentxx_javascript_engine` | QuickJS 引擎 (能力 `interpreter.js`) |
| `example_js_execute_command` | JS 代码执行工具 (`example_js_execute_command`, 依赖 `agentxx_javascript_engine`) |

> **插件自管工具提示词 (含运行环境探测)**：工具定义 (`description` / `parameters`) 在
> `register_tool` 时固化，因此插件必须在自己注册工具**之前**完成一切探测/决策
> （注册后再刷新提示词不会改变已经固化的工具定义）。`agentxx_execute_command` 即按此在
> `start` 事务内探测 python/node 可用性与版本（Windows 侧另探测 PowerShell 可执行文件名与
> 版本），随后把生成好的 depict 与参数描述经 `agentxx.agent.prompt` 接口表作为本实例贡献注入
> 宿主；`disable` / 卸载时该贡献自动撤销并恢复基础值（见 §15.4）。
> 探测结果只存实例上下文 (`ExecPluginCtx::env`)，不使用进程级可变静态量（多实例约束，见 §4）。

---

## 14. 构建与平台

> SDK 类型约束的编译期验证：`agent/script/check_sdk_negative_compile.sh` 从测试构建的
> `compile_commands.json` 提取真实编译环境，编译 `agent/test/plugin/negative_compile/`
> 下的片段并断言行为（`positive_control.cpp` 必须编译成功；错误签名如 hook 返回 `int`、
> capability 返回 `int`、tool 返回普通值、跨边界传 STL 参数必须编译失败）。
>
> 插件框架定向探针：`AGENTXX_ENABLE_SANITIZER=ON`（默认开启，与 ASan/UBSan 同一开关）
> 且**非 Release 配置**（`XX_IS_RELEASE_D=0`）时，对 `lib/src/plugins/*.cpp` 与
> `test/plugin/*.cpp` 追加 `-fsanitize=undefined`
> （运行库经顶层非 Release 的 sanitizer 链接参数注入），不重编第三方依赖；用于 `PluginManager`/
> `ClientPluginManager` 与 header-only 的 Operation/InstanceLifetime 运行时的未定义行为验证。
> Release 配置不含 sanitizer 编译/链接参数，探针一并跳过（否则插桩 TU 会因缺少
> `__ubsan_handle_*` 运行库符号而链接失败）。

- **平台矩阵**：各插件在自身 `CMakeLists.txt` 开头经 `plugin_platform_support.cmake` 的 `gate` 函数判定，复用顶层 `XX_IS_*_D` 变量；不支持的平台跳过编译 (screen_capture/computer_use/text_selection_monitor 仅 Windows, audio_stream 全平台未实现等)
- **内置合并编译**：按 `AGENTXX_PLUGIN_BUILTIN_LIST` 合并进 `libagentxx`；此时 `test_ffi_c_api` 与 `client_plugins` 测试按条件跳过动态库路径
- **产物布局**：独立动态库模式产物统一输出到 `{build}/exec/plugins/<插件名>/` (含 `plugin.yaml` 清单时按目录分派)

---

## 15. 实例生命周期与异步契约

> 本节描述宿主 (agent 侧 `PluginManager` / client 侧 `ClientPluginManager`) 与插件之间的
> 实例生命周期、Operation 终态与线程契约。它是插件实现的**必须遵守项**；
> 迁移方案与验收矩阵见 `resource/history/plugin-refactor-2/plugin.md`。

### 15.1 入口与状态机

```
create (纯构造) → start (注册事务) → Ready
Ready ⇄ Disabled            (用户或依赖级联)
Ready/Disabled → Closing → Closed
Closing → CloseFailed → Closing (可重试)
```

- `create`：只分配上下文、查询接口、初始化纯本地字段；不提交工具/hook/能力/事件/
  UI/prompt/graph 注册，不启动不受托管的线程。
- `start`：在宿主 IO 线程执行注册事务；同步 `notify.done` 返回即视为成功。
  失败时返回 `NULL + error`（视为拒绝，宿主回滚本次已生效的注册并撤销实例）。
  回滚范围覆盖工具、hook、能力、事件订阅、资源、prompt 贡献、图类型槽位与
  Client UI 项/命令/订阅（回归含真实 DSO 双端：全量注册后失败 → 无残留 →
  再次加载同名注册全部成功）。
- `stop`：实例停用或关闭时调用，用于撤销插件自管的线程/定时器/订阅；
  可重复尝试，失败时实例保持 `Disabled`/`CloseFailed` 且保留上下文与动态库。
- `destroy`：只在 `stop` 完成且租约归零后调用；不得创建异步工作、不得调用宿主
  注册接口。
- **start/stop 是必备入口**：插件(无论 agent 侧/client 侧)都必须导出
  `agentxx_plugin_{agent,client}_{start,stop}`；缺失时宿主拒绝加载并给出明确原因。
  加载路径不再支持"create 期注册、无生命周期入口"的旧形态。
- SDK：`AGENTXX_PLUGIN_AGENT_EXPORT(Ctx, Name, Ver, Desc, StartFn, StopFn)` /
  `AGENTXX_PLUGIN_CLIENT_EXPORT(...)` 一次生成 `get_info/create/start/stop/destroy`
  五个入口符号 (带异常兜底)；手写 `create/destroy` 的插件用
  `AGENTXX_PLUGIN_{AGENT,CLIENT}_LIFECYCLE_EXPORT(Ctx, StartFn, StopFn)` 只生成
  `start/stop` trampoline。

### 15.2 Operation 终态协议

- `start` 返回 `NULL + error` 只表示**拒绝**，此时不得调用 `notify.done`，宿主不产生
  回调；一旦接受（无论同步还是异步 done），宿主保证恰一次完成回调。
- `done` 可从任意线程调用；payload 仅在本次调用内借用，宿主第一步复制。
- 一个 Operation 只产生一个终态：`OK` / `CANCELLED` / `FAILED`；终态之后的
  `cancel` 为空操作，不再进入插件代码。
- 取消通过不透明 `PluginxxCancelToken` 查询（`is_cancelled`），插件不得把
  `volatile` 标志或 ABI 原子地址当作跨线程同步手段。
- 跨插件互调同时持有 **caller 与 provider 两侧租约**：caller 的完成回调返回前，
  caller 不会被卸载/destroy。

### 15.3 线程与租约

- 注册表、生命周期、Operation 状态与默认插件业务只在所属 IO 线程；
  worker/JS/平台线程是显式例外。
- 宿主在卸载/关闭时先阻止新进入，再取消旧操作，再等待全部插件代码与回调返回
  （事件式 idle 通知 + 绝对截止时间），最后 stop/destroy/dlclose；
  超时进入 `CloseFailed` 并保留上下文与动态库（可重试），不会静默泄漏。
- 插件持有宿主分配的 token（如 `host->opaque`）在实例卸载后继续使用时，
  宿主保证**安全失败**：入口返回失败值，不访问已释放对象。

### 15.4 启用/禁用事务

- `disable(name)`：宿主同步摘除该实例的注册（工具/hook/能力/事件/资源/prompt 贡献/
  graph/UI），并把 `stop` 事务投递到所属 IO 线程；级联按直接依赖者递归处理。
- `enable(name)`：宿主恢复启用状态后，由插件的 start 事务重新声明注册
  （start 成功前不恢复宿主侧记录）；启用顺序为"先依赖、后依赖者"，
  用户显式禁用的插件不被级联恢复。
- prompt 以 `(owner, key, sequence, value)` 贡献模型合成：卸载/禁用只删除该 owner 的
  贡献并重新合成，不覆盖其他 owner，也不写回已卸载 owner 的旧值。
- Client 侧动作按钮在渲染时记录 `plugin/generation/owner`，派发到 IO 线程复查：
  同名插件重载后，旧点击一律丢弃。

### 15.5 自管线程插件 (以 JS 解释器引擎为例)

`agentxx_javascript_engine` 是"插件自带专用线程"的参考实现，约束同样适用于其他
自管线程/定时器的插件：

- **线程与运行时都属于 start 事务**：`create` 不启动线程；`start` 创建运行时并启动
  线程、提交运行时注册 (能力)；`stop` 停止线程并释放运行时；`destroy` 只释放本地状态，
  不调用宿主接口。
- **stop 必须可重复且不能被调用线程自锁**：插件线程若可能反向调用宿主 vtable
  (宿主在非 IO 线程上以 `ioCallSync` 投递回 IO 线程)，则 `stop` 不得在 IO 线程直接
  `join`；应把 `join` 交给独立收尾线程，并在收尾完成后上报完成通知
  (完成通知允许来自任意线程)。线程空闲时可在调用线程直接收尾 (省一次线程切换)。
- **停止即拒绝新工作**：停止标志生效后不再接受新 Operation；已入队但未开始的任务
  必须在插件线程内按终态终结 (不得执行插件 JS 代码)，已开始的任务观察停止标志后
  按取消完成 —— 满足"所有已接受操作必须终结"。
- **线程空闲判定必须有不变式**：宿主线程据"无在手中任务 + 队列为空"判定可直接收尾，
  因此插件线程取任务/执行定时器与置忙标记必须在同一临界区内完成。
- **脚本/子对象注册归属调用方实例**：引擎承载的脚本注册挂在调用方 (壳插件) 实例上，
  由宿主在该实例停用/卸载时统一撤销；引擎自身的 `stop` 只释放运行时，不假设宿主
  注册表的清理顺序。

### 15.6 平台支持矩阵与验证状态

- **平台 gate**：各插件在自身 `CMakeLists.txt` 开头调用 `agentxx_plugin_platform_gate`
  声明支持平台，列表为空表示**全平台跳过**（无实现）。跳过发生在 `add_subdirectory`
  入口，内置合并清单登记也随之天然跳过。
- 当前矩阵：`screen_capture` / `computer_use` / `text_selection_monitor` 仅 Windows；
  `agentxx_audio_stream` **全平台跳过**（WASAPI 实现未启用，`audio_stream.cpp` 中该分支
  带 `&& false`，仅剩桩实现；实现可用后声明 `windows` 并同步本节）。
  `agentxx_system_monitor` 覆盖 windows/linux/android/macos：Windows 用 PDH + DXGI，
  Linux/Android 解析 `/proc` + sysfs，macOS 用 mach `host_statistics`/`host_statistics64`
  与 IOKit `IOAccelerator`（`PerformanceStatistics`）；其余平台跳过编译。
- **已验证平台**（当前实现的验收范围）：
  - Windows（MSVC 14.51 / VS18，Debug + ASan）：全插件构建（19 个 DSO）、插件专项
    1765/0、扩展回归 2251/0、工具模块 360/0；
  - Linux（GCC 16.1，Debug + ASan/LSan、定向 UBSan、定向 TSan）：插件框架 TSan 0 告警，
    ASan 扩展回归 2179/0；
  - macOS（Apple M1、Apple clang、Debug + ASan）：`agentxx_system_monitor` 平台矩阵回归
    cpu_gpu 32/0、plugin_multi_instance 80/0、client_plugins 537/0
    （CPU 利用率按 1s 负载对照校验：空载 23%、4 线程 63.8%、8 线程 100%；GPU 利用率与
    显存经 Metal 计算负载对照校验：空载 8% → 满载 100%，GPU 占用系统内存同步上升）；
  - Android：未验证。
- **测试二进制在 Windows 上的运行前提**：工作目录必须是可执行文件所在目录
  （`exec/`），因为插件目录按 `GetModuleFileNameW` 推导；Linux 用 `/proc/self/exe`。
- 平台矩阵的迁移记录与逐条验证结果见
  `resource/history/plugin-refactor-2/work.md`（1.10、3.6 节）。

---

## 16. 协程驱动 (通用 pump/wake 协议 + `PollOneBridge`)

> 本节描述**协程驱动协议**：插件协程与宿主协程在同一宿主 IO 执行
> 序列中交错推进，不额外开线程、不阻塞 IO；等待以"宿主可见唤醒源"为主，插件本地 reactor 上的
> 内核就绪等待由**声明式受控轮询**（`polled_tool`）驱动，见 §16.5。方案与阶段划分见
> `resource/history/plugin-refactor-3/plugin.md`，实施记录见
> `resource/history/plugin-refactor-3/work.md`。

### 16.1 为什么需要它

插件可以自带协程库、事件循环与第三方异步库，而宿主不能把插件私有 reactor 的等待
对象接进自己的执行序列（一个私有 `io_context` 没有跨平台公共 API 能把它注册给另一个
`io_context`）。因此两端只经两类动作协作：

- **driver / pump**：插件申请宿主异步执行一次**有界回调**（推进本地运行时一个有限
  步骤：例如一次 `poll_one`，即一个就绪 handler）；
- **wake**：插件适配器知道本地已有可运行工作时（新 root 首步、宿主回调投递的
  continuation、本地 post），向宿主请求一次 driver 请求；重复 wake 由适配器合并。

宿主不需要知道插件用哪种 coroutine/future/actor，插件也拿不到宿主 executor。

```
宿主 IO 线程                       插件适配器/桥                    插件本地运行时
  │ request_driver ───────────────────▶│ 登记请求 (持实例 lease)
  │◀──────────────────── 请求 (异步) ──│
  │ drive_once() ─────────────────────▶│ poll_one() ───────────▶ 推进一个 continuation
  │                                    │ ◀── 外部完成回调 (post + wake)
  │ drive_once() ─────────────────────▶│ poll_one()
  │◀──────────── done(status, payload) │ 根操作终结 (exactly-once)
```

### 16.2 C ABI (`agentxx.agent.coroutine_runtime`, version 1)

```c
typedef struct PluginxxDriver PluginxxDriver;
typedef void(PLUGINXX_CALL* PluginxxDriveOnceFn)(void* user_data);

typedef struct PluginxxCoroutineRuntimeIface {
    int32_t  version;      // == 1
    uint32_t struct_size;
    PluginxxDriver* (PLUGINXX_CALL* request_driver)(
        const PluginxxHost*, PluginxxDriveOnceFn, void* user_data, PluginxxString* error_out);
    void   (PLUGINXX_CALL* cancel_driver)(PluginxxDriver*);
    int32_t(PLUGINXX_CALL* is_io_thread)(const PluginxxHost*);
} PluginxxCoroutineRuntimeIface;
```

**契约（宿主与插件共同遵守）**

1. `request_driver` **任意线程可调用、永不内联**回调：即使调用者就在 IO 线程，请求也
   经宿主任务队列异步投递。否则 root start / completion / cancel 会形成意外重入，
   并失去交错执行的公平性。
2. 一次请求**至多执行一次**回调，且回调只推进一个有限步骤：**不得阻塞、不得等待
   事件、不得同步调用宿主业务接口**；异常必须由插件自己捕获。
3. 插件侧适配器**自行合并 wake**（同一实例同时只登记一次请求）；宿主仍会做去重
   （同一时刻只保留一次已登记的请求）与关闭救援作为最后防线。
4. `cancel_driver` **幂等、非阻塞**：尚未开始的请求之后不再执行回调；正在执行的
   回调不会被强行中断，由插件自己的 root 收束协议收尾。
5. 请求在**排队与执行期间持有实例执行 lease**，因此 `dlclose` 不会越过仍在排队或
   正在执行的插件代码（卸载的 idle 等待必然覆盖它）。
6. 失败语义：`request_driver` 返回 `NULL + error_out` 表示宿主不再提供驱动；插件必须
   把受影响的操作以失败/取消终结，不得静默丢弃（kit 会自动如此处理）。

### 16.3 宿主侧实现语义

- **请求状态机**：`Idle → Running → Finished` 或 `Idle → Finished`（取消），迁移由
  一次 CAS 仲裁，保证"取消后不再执行回调"与"lease 恰好释放一次"。
- **句柄校验**：`cancel_driver` 的 ABI 形态不含 host 参数，宿主无法从实例反查合法
  句柄，因此请求在创建时登记进**进程级地址注册表**（只存 `weak_ptr`，收束时按地址
  摘除）。伪造/过期指针只会被安全忽略并记日志，**绝不解引用**。
- **admission 模式**：请求采用 `lifecycle` 模式的实例 lease —— 实例进入 `Closing`
  后仍允许驱动。这是必需的：关闭的第一步是取消全部 Operation，而插件的取消收束
  （取消回调 → 唤醒 → 下一个有限步骤）必须能继续跑完，否则关闭必然超时。
  `Disabled`/`Closed` 一律拒绝。
- **关闭救援**：宿主只在**已判定关闭失败**（`waitInflightZero` 超时）时调用
  `cancelPendingDrivers()`，取消仍未开始的请求以免 lease 永久残留并记日志。
  正常关闭不依赖它：kit 在实例上下文销毁时自行 `cancel_driver`。

### 16.4 kit 侧实现 (`detail::PollOneBridge` + `detail::BridgeRoot` / `detail::PolledRoot`)

`PluginBase::bridge()` 返回本实例的桥
（每实例一份，无任何进程级可变状态）。

**状态机（三个竞态窗口都覆盖）**

| 状态 | 含义 |
|------|------|
| `readySteps_` | 已投递但尚未执行的本地步骤数（每一步需要一次 `poll_one`） |
| `wakePending_` | 一次显式 `wake` 尚未被请求覆盖（兼容"插件直接向 `local_executor` 投递"） |
| `driverQueued_` / `driverRunning_` | 已申请请求（含 `request_driver` 正在返回的窗口）/ 回调正在执行 |
| `pendingEpoch_` / `nextEpoch_` | 请求世代，用于识别"回调已消费本轮请求"的窗口 |
| `polledRoots_` | 在途**受控轮询根**数（声明式 `polled_tool`；0 = 不轮询、不建定时器） |
| `pumpPending_` | 受控轮询判定"该继续驱动"（第二类申请请求理由） |
| `pumpWaitScheduled_` / `pumpWaitOp_` | 在途退避定时器（宿主 `scheduler.sleep`）与其句柄 |
| `pollBurst_` | 连续"有进展"步数（用于突发上限让出） |

关键不变量：

1. 每张请求**恰好一次** `localIo_.poll_one()`（一个 host driver ⇔ 一个局部 continuation）；
2. 只有确实还有可运行工作（`readySteps_ > 0`、未覆盖的显式 wake，或有在途 polled 操作
   且轮询策略判定需要续票）才申请下一次请求，`poll_one()==0` **不会**在无工作的状态下
   重新排队 —— 空闲时既不占宿主任务队列也不建定时器；
3. 宿主回调完成后只做 `postToLocal(continuation) + wake()`，**绝不在宿主回调栈内
   恢复插件协程**，也不在宿主 IO 线程上跑插件业务代码；
4. 并发投递 N 个步骤最终会得到 N 张请求（不会因 wake 合并只推进一个）；
5. 宿主拒绝驱动/桥停止时，所有活跃根（含受控轮询根）被终结为失败（`FAILED`），
   宿主 Operation 因此不会悬挂。

**受控轮询（pump）调度策略**（只作用于 `polled_tool` 的根，见 §16.5）：

```
本轮 poll_one 执行到了 handler（有进展）且 pollBurst_ < kPollBurstMax(256)
    → 立即申请下一次请求        （等待中的 socket/管道/文件就绪能被尽快收走）
否则（无进展 / 达到突发上限）
    → scheduler.sleep 安排一次退避（无进展 10ms；突发上限后让出 1ms）
      → 到期回调只 request_driver（不恢复插件协程）
polledRoots_ 归零
    → 取消在途退避，之后不再申请请求（空闲零开销）
```

**根的生命周期（`BridgeRoot` / `PolledRoot`）**

- 根对象由**桥持有强引用**（`roots_`）：挂起中的根除了"下一次恢复任务"没有任何持有者，
  桥若不持有，排队任务执行完就会销毁帧，之后到达的宿主回调就会访问已释放的协程帧；
- 完成/放弃的仲裁是 **exactly-once**（CAS）：正常完成走 `finishIfDone`，宿主拒绝
  驱动走 `abandon`，两条路径只有一个能上报终态；
- 被放弃的根移入 `abandonedRoots_`，**帧活到桥销毁为止**（那时实例已无未完成的宿主
  操作），期间迟到的宿主回调因 `shouldAdvance()==false` 安全跳过；
- 协程帧的销毁与 op 句柄（`Job`）的释放统一发生在一个"协程已终止或不可能再被恢复"
  的时点（`destroyFrame()`）：正常完成时在 `finishIfDone` 内；放弃路径在桥销毁时。
  释放动作**不能**放在 `abandon` 里 —— 被放弃的根可能正在 driver 内执行，其输入
  (`RootRequest`/`OpCtl`) 仍被协程以引用使用。

受控轮询根用同一套仲裁语义但形态更简单（`PolledRoot`，见 `polled_tool`）：

- 协程是 `asio::awaitable`，其**帧由 asio 自己持有**（completion handler / 本地 reactor
  销毁时统一释放），因此 `PolledRoot` 不销毁帧，只做"终态上报 + Job 回收"的
  exactly-once 仲裁（正常完成 vs 桥停止时放弃，用一次 CAS 决定）；
- 正常完成：`detail::runPolledPumpJob` 认领 → 注销登记（`polledRoots_` 递减，必要时
  停止 pump）→ 上报终态 → 执行清理回收 `Job`；
- 桥停止：`failAllPolledRoots` 把在途根整批摘下 → 每个根按 `FAILED` 上报一次 →
  执行清理回收 `Job`；挂起的帧随本地 reactor 销毁而释放。

**已接桥的 kit 路径**

| kit 组件 | 桥接语义 |
|----------|----------|
| `tool` / `hook` / `capability` / `graph_node`（返回 `Task<T>`） | 首步由 host driver 推进；start 内不跑插件协程 |
| `spawn`（后台任务） | 同上（首步不在调用方栈内执行） |
| `sleep(ctx, ms)` | 宿主计时器适配：到期回调只 post + wake |
| `call_tool` / `invoke_cap` | 宿主回调式互调：完成回调只 post + wake |
| `yield(ctx)` | 让出一轮：投递 continuation 后由下一次请求推进 |
| `offload(ctx, work)` | **工作体仍在宿主工作线程池**（显式例外），只有完成后的恢复回到 driver 序列 |
| `polled_tool(ctx, name, …)` | **声明式受控轮询**：业务体是 `asio::awaitable<std::string>`，跑在桥的本地执行器上，由"有进展立即续 + 无进展 10ms 退避 + 突发上限 256 后让出 1ms"推进（见 §16.5） |
| `fast_tool` / 同步 hook / `blocking_tool` | 不变（同步路径本就不需要驱动） |

`bridge().local_executor()` 暴露本地执行器，插件可用 `asio::co_spawn` 把自己的
`asio::awaitable` 排进同一序列；这些 awaitable 需要等待"有宿主可见唤醒源"的操作
（宿主回调适配器 / 宿主计时器 / 已 post 的 continuation），或者改用 `polled_tool`
声明"该操作用受控轮询驱动"（见 §16.5）。

### 16.5 受控轮询（`polled_tool`）与明确不支持

**为什么需要它**：`poll_one()` 只能"执行已就绪的 handler"，不会让**插件本地 reactor**
上的等待对象到期。因此"业务体本来就是 asio 协程、等待的是 socket/管道/文件/本地 timer
的内核就绪事件"这类工具（websearch 的 HTTP、execute_command 的子进程管道、filesystem
的 `asio::stream_file`），在没有 wake source 时无法被推进。

**方案：声明式受控轮询（`polled_tool`）**。插件在注册时就**声明**"该工具需要受控轮询
驱动"（而不是隐藏轮询），参数显式且可观测：

| 项 | 值 | 说明 |
|---|---|---|
| 触发条件 | `polledRoots_ > 0` | 只在有在途 polled 操作时轮询；空闲零请求、零定时器 |
| 有进展 | 立即续票 | 本轮 `poll_one` 执行到了 handler（就绪事件被收走） |
| 无进展 | 退避 10ms | `PollOneBridge::kPollIntervalMs`，经宿主 `scheduler.sleep`；到期回调只 `request_driver` |
| 突发上限 | 连续 256 步后让出 1ms | `kPollBurstMax` / `kPollBurstYieldMs`，避免同实例自循环独占 IO 线程 |
| 取消 | 置取消标志 + 取消在途退避 + 取消写入 `CancelRegistry` | 插件不必等满一个退避量子即可看到取消并收束根 |
| 无 driver 的宿主 | 不支持 | `coroutine_runtime` 是宿主必备能力：缺失时 kit 无法推进协程（驱动请求失败会终结在途根并记日志），不再有 offload 降级路径 |

业务签名与 `blocking_tool` 同形（只是返回 `asio::awaitable<std::string>`），因此迁移
通常只是换一个注册函数名：

```cpp
polled_tool(ctx, name, depict, schema,
    [](Ctx& c, std::string_view args, std::string_view tid, std::string_view workDir,
       const PluginxxCancelToken* cancel) -> asio::awaitable<std::string> {
        ArgReader reader(args);
        co_return co_await doSomethingAsync(reader.raw(), c.workDir(tid));
    });
```

代价与约束（必须与实现一起遵守）：

1. **10ms 量子**：单实例等待期间 ≈100 次/秒驱动（每次 ≈1 个宿主 post + 1 次
   `epoll_wait(0)`），等待阶段最坏多 10ms 延迟（网络/子进程可接受）；实现为常量便于调优。
2. **重 CPU 段不得留在 polled 协程里**：它运行在宿主 IO 线程上（>100ms 会触发宿主
   看门狗告警）。目录遍历 / 全文件扫描 / 向量相似度继续用 `blocking_tool`；确需在
   polled 协程内做重计算的用宿主 `offload` 显式卸载。
3. **polled 协程内不使用 kit 的 `Task` 型原语**（`sleep`/`call_tool`/`invoke_cap`）：
   它们的 awaiter 依赖 kit 自有 promise 接口，而 polled 协程是 `asio::awaitable`。
   需要计时用 asio 原生 `steady_timer`（pump 下可用），需要宿主回调式接口时经
   `bridge().local_executor()` 自行投递后续步骤（后续可选补 asio 版适配器）。
4. **可替换性**：将来实现 `wait_source`（宿主等待插件交出的 fd/handle）或宿主侧 IO
   服务后，只需把"何时申请下一次请求"的策略从"10ms 退避"换成"就绪通知"，polled 工具
   的业务代码与 ABI 形态不变。

**仍然明确不支持 / 需要显式例外**：

- **未声明 polled 却依赖私有 reactor**：仅依赖私有 `io_context` 的 socket/timer/
  process/file 等待**不会**被桥推进。桥在同一实例持续 8 次 driver 无进展且仍有活跃根时
  输出一次警告（"awaited work has no host-visible wake source"），提示改用
  ①宿主回调式完成（适配器 post + wake）②宿主计时器（`co_await sleep`）
  ③`polled_tool` 声明式受控轮询 ④显式受限工作线程（`offload` / `blocking_tool`，并在
  能力说明里标注）；它不会自旋，因此警告之后请求数不再增长。
- **误用检查**：首次驱动会校验是否运行在宿主 IO 线程（`is_io_thread`），不符时记日志。

### 16.6 内置插件迁移状态

- **所有使用 kit 的插件自动接桥**：`tool`/`hook`/`capability`/`graph`/`spawn` 与
  `sleep`/`call_tool`/`invoke_cap`/`yield`/`offload` 的恢复路径都已走驱动序列，插件
  业务代码无需改动。
- **已迁移到受控轮询（`polled_tool`）**：

  | 插件 / 工具 | 依据 |
  |---|---|
  | `agentxx_websearch`：`web_search` / `web_fetch` / `web_fetch_markdown` | 实现体本就是 asio 协程（`co_await HttpClient::*Async`）；网络等待不再占用宿主工作线程池，同实例的 HTTP keep-alive 连接池天然复用 |
  | `agentxx_execute_command`：`execute_bash_command` / `execute_windows_command`（Boost.Process v2 分支） | 子进程管道/计时器绑定协程 executor；并发多命令共享同一 poll 序列与同一个本地 reactor，不再各占一个池线程直到超时 |
  | `agentxx_filesystem`：`read` / `write` / `edit` | `asio::stream_file` 异步读写；文件 IO 真异步（可用性经 `utilxx_base::isAsyncFileIoSupported()` 判断：编译期宏 + 运行时 io_uring 探测），避免大文件读写占用池线程 |

- **保持 `blocking_tool`（显式例外）**：
  - `agentxx_filesystem`：`list` / `glob` / `grep` —— 目录遍历 + 全文件扫描 + 正则/编码
    转换属 CPU/阻塞 IO（asio 无异步目录 API），放进 pump 只会阻塞同实例其它工具；
  - `agentxx_execute_command`：非 Boost.Process v2 的 popen 回退分支（同步实现）；
  - `agentxx_filesystem`：文件异步 I/O 不可用环境（同步回退，注册侧自动切回
    `blocking_tool`）—— 可用性统一经 `utilxx_base::isAsyncFileIoSupported()` 判断：
    编译期未启用 asio 文件 I/O（`ASIO_HAS_FILE` / `BOOST_ASIO_HAS_FILE` 均未定义），
    或 Linux/Android 上运行时无法创建 io_uring 环（容器/虚拟化的 seccomp 过滤
    ——`/proc/self/status` 的 `Seccomp: 2`——会拦截 `io_uring_setup`，内核过旧返回
    `ENOSYS`）；判断结果按进程缓存，测试可经 `setAsyncFileIoSupported(false)` 强制
    关闭以覆盖同步兜底路径；
  - `agentxx_rag_search`：embedding 网络段先把实现体恢复为协程形态（其注释记录了
    "原版 asio 协程接口改为同步实现"）再迁移；分块/相似度等 CPU 段继续 offload（**二期**）。
- **保持现状（无私有 reactor 等待）**：codegraph / planning / system_monitor / math /
  string / system / JS 系插件；JS 引擎是既有"自管线程 + notify"正确形态。
- **`ClientPluginManager` 的 `asio::thread_pool(1)`**：只用于 `dlopen`/entry 这类不可避免的
  阻塞动态库工作（entry 内的注册动作仍经 vtable 投递回 IO 线程），保持为独立、可关闭的
  后台设施；client 侧插件不产生需要驱动的协程根，因此 client kit 不创建桥
  （宿主已暴露同一 IID，后续需要时可直接接入）。
- **样例**：
  - `example_bridge`：两个"真实唤醒源"（宿主计时器 + 宿主回调式互调）与桥诊断字段；
  - `example_polled_timer`：**受控轮询**样例 —— 在插件本地 executor 上 `co_await`
    3 次 asio `steady_timer`，返回实测耗时与 `driverAvailable/onHostIoThread/pumpOnStart`。

### 16.7 验证

| 层次 | 用例 |
|------|------|
| C ABI | `test_plugin_abi_c17.c`：协程驱动表 8 字节对齐、`version/struct_size` 偏移、版本号；C++ 侧逐项对照（`plugin_runtime`） |
| 宿主请求 | `plugin_runtime`：恒异步、每票至多一次、取消后不再执行、排队持 lease、幂等取消、伪造句柄安全忽略、Closing 允许 / Closed 拒绝、空回调返回 `NULL + error_out` |
| kit 桥接 | `plugin_bridge`（伪宿主 C ABI 驱动）：不内联、每票一次 `poll_one`、空闲不自旋、wake 三个窗口不丢、宿主回调不重入、取消唯一终态、拒绝驱动即终结、stop 取消排队请求、多实例隔离 |
| kit 受控轮询 | `plugin_bridge`：首步不内联、有进展立即续票、无进展恰好一次 10ms 退避（不新增请求）、根结束即停止轮询（取消在途退避）、取消会取消在途退避并只产生一个 `CANCELLED` 终态、`stop` 时在途 polled 根按 `FAILED` 终结一次并回收 `Job`、突发上限触发 1ms 让出 |
| 端到端 | `plugins`：`example_bridge` 与 `example_polled_timer` 经真实宿主执行，断言 `driverAvailable/onHostIoThread/pumpOnStart`、"插件挂起期间宿主任务仍在推进"（同一 IO 序列交错执行）与 asio 原生 timer 真正到期；1000 并发工具调用压力用例 |
| 端到端（迁移插件） | `plugins`：`agentxx_filesystem` read/write/edit（受控轮询）+ list（offload）同一实例共存；`agentxx_websearch` 经本地回环 HTTP 服务完成 fetch/fetch_markdown 与 6 路并发（互不阻塞）；`agentxx_execute_command` 在 `sleep 5` 挂起期间卸载 —— 取消收束、pump 停止、inflight 归零且耗时远小于命令自身超时 |
| 内存 | `plugin_bridge` 单独运行 0 泄漏；插件专项 ASan+LSan 与重构前基线逐项一致（4480 字节 / 64 处），含受控轮询新增用例（在途卸载/放弃路径）后不变 |
