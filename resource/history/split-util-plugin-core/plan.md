# 拆分 `cxx_utilxx_base` / `cxx_utilxx` / `cxx_pluginxx` 实施方案

> 定位: 待实施的执行计划 (本文件不含已落地代码)
> 目标读者: 实施者 (含 AI Agent)
> 关联文档: [design/index.md](/docs/zh-cn/design/index.md) · [design/plugins.md](/docs/zh-cn/design/plugins.md) · [design/ffi.md](/docs/zh-cn/design/ffi.md)
> 现状代码: [agent/lib/include/agentxx/util](/agent/lib/include/agentxx/util/) · [agent/lib/include/agentxx/plugin](/agent/lib/include/agentxx/plugin/) · [agent/lib/src/plugins](/agent/lib/src/plugins/)
>
> 修订记录 (确认结论):
> ① C 符号冻结; ② neograph 留 agentxx (`agentxx::util` 目录保留), 仅 `async_offload` 迁出并统一为 `utilxx::CancelToken`; ③ 通用表进 pluginxx;
> ④ asio = Boost.Asio (Boost find + include 根前缀映射), 与 neograph 无关;
> ⑤ **不保留 `agentxx/util/*.h` 转发头**; ⑥ **client 表留在 agentxx**;
> ⑦ **三库同时产出静态库与动态库**, 命名规则仿 libagentxx (debug 加 `d`、静态库加 `_static`);
> ⑧ **不新增检查脚本**; ⑨ **utilxx/base 可直接依赖 Boost**, 不替换已有实现 (如 base64)。

---

## 1. 目标与非目标

### 1.1 目标

1. 从 `libagentxx` 中拆出三个**独立可复用**的 CMake 工程, 落在
   `agent/third_party/` 下 (与 fmt/simdjson 等同级, 视作本项目维护的第三方依赖):

   | 项目目录 | 包名 (find_package) | 命名空间 | 定位 |
   |---|---|---|---|
   | `agent/third_party/cxx_utilxx_base/` | `cxx_utilxx_base` | `utilxx_base` (基础件) + `utilxx` (跨库共享契约) | 无重依赖基础工具: 日志 / JSON / 字符串 / 容器 / 环境 / 系统探测 / **取消令牌** / **异步卸载** |
   | `agent/third_party/cxx_utilxx/` | `cxx_utilxx` | `utilxx` | 重依赖工具: HTTP / WS / SQLite / 正则 / 路由 / 差异 / worktree / 散列 (依赖 base) |
   | `agent/third_party/cxx_pluginxx/` | `cxx_pluginxx` | `pluginxx` | 插件框架内核: 纯 C ABI 基座 + **通用接口表定义及实现** + 宿主运行时 + 插件 SDK 核心 (依赖 base) |

   > **命名空间与库不是一对一**: `cxx_utilxx_base` 承载绝大多数 `utilxx_base::` 基础件,
   > 另承载两个**跨库共享契约**头 (命名空间 `utilxx`): `utilxx::CancelToken` 取消抽象与
   > offload 协程工具。它们必须落在**无重依赖**的库里 —— 因为 `cxx_pluginxx` 与 agentxx
   > 都要使用, 而 pluginxx 不允许引入 OpenSSL/SQLite 等重依赖。
   > `cxx_utilxx` 在同一 `utilxx` 命名空间内继续扩展 (http/sqlite/regex/...)。

2. `agent/CMakeLists.txt` (superbuild) 导入这三个工程; `lib` / `plugins` /
   `client` / `test` / `benchmark` / `ffi` / `example` 各模块**一律经
   `find_package` 引用**, 不再直接 `add_library(agentxx_util ...)`
   (现有 `agentxx_util` 目标整体退役)。

3. **每个工程同时产出静态库与动态库**, 命名规则**照搬 libagentxx**:
   Release ≡ `lib<name>.so` / `lib<name>_static.a`, Debug ≡ `lib<name>d.so` /
   `lib<name>_staticd.a` (详见 §6.1)。调用方按需选择链接变体。

4. **行为零回归**: 现有测试全绿; 插件动态库导出符号面不变; 拆分前编译的
   插件二进制可被拆分后的宿主直接加载 (C ABI 冻结)。

### 1.2 非目标 (本次不做)

- **不改跨边界 C 符号名** (`agentxx_plugin_agent_*` / `agentxx_plugin_client_*`)
  与 C 结构体名 (`AgentxxPluginHost` 等) —— 已确认冻结 (§5.1)。
- **不保留 `agentxx/util/*.h` 转发头**: 迁出的头一律做真实路径替换,
  不提供 `agentxx/util/json.h` → `utilxx_base/json.h` 之类的转发/别名头 (§5.4)。
- 不引入任何 musicxx 相关代码、表或配置; 三库保持宿主无关。
- 不改插件框架的领域功能语义 (工具 / 钩子 / 能力 / 事件 / 权限 / 图节点)。
- **不重构 neograph**: neograph 留在 agentxx —— 依赖 neograph 类型的
  `agentxx/util/exception.h` 与 `agentxx/util/neograph_json_bridge.h`
  **原地保留** (路径与 `agentxx::util` 命名空间都不变, 调用点零改动);
  agentxx 继续保有自己的 `agentxx/util/` 目录, 用于存放这类宿主/图引擎耦合件。
- 不替换 Boost / Asio 的既有来源: 三库**直接依赖 Boost** (含头文件实现细节,
  如 `boost/beast/core/detail/base64.hpp`), 不做替代实现 (§5.3)。
- 不新增隔离检查脚本 / 门禁 (§4.3 以代码评审约定替代)。

---

## 2. 现状盘点 (实施前基线)

### 2.1 代码规模

| 区域 | 文件 | 行数 (约) | 说明 |
|---|---|---|---|
| `agent/lib/include/agentxx/util/` | 28 头 | 5 956 | 全部 `namespace agentxx::util` |
| `agent/lib/src/util/` | 13 源 | 7 004 | 静态库 `agentxx_util` |
| `agent/lib/include/agentxx/plugin/api/` | 4 头 | 7 058 | `plugin_api.h` 815 · `client_plugin_api.h` 445 · `plugin_kit.h` 5 635 · `plugin_guard.h` 163 |
| `agent/lib/include/agentxx/plugin/` (非 api) | 11 头 | 4 872 | 其中**无领域依赖**的运行时头共 2 541 行 (`plugin_runtime.h` 445 · `plugin_driver.h` 248 · `op_driver.h` 596 · `plugin_common.h` 502 · `plugin_manager_base.h` 750) |
| `agent/lib/src/plugins/` | 11 源 | 9 574 | `client_plugin_manager.cpp` 3 724 · `plugin_manager_vtable.cpp` 2 087 · `plugin_manager_lifecycle.cpp` 1 429 · `plugin_manager_adapters.cpp` 817 等 |

### 2.2 依赖现状

- `agentxx_util` 依赖 **PUBLIC 全传递**: fmt / SQLite3 / uchardet / iconv /
  simdjson / OpenSSL (+条件 hyperscan / uring), 另经 `link_directories` 解析 `hs_runtime`。
- **`asio` 就是 Boost.Asio, 与 neograph 无关**: 源码统一写 `asio/...` 前缀, 真实文件是
  `${BOOST_ROOT}/include/boost/asio/...`; 构建侧把 `${Boost_INCLUDE_DIRS}/boost/`
  一并加入 include 根, 于是 `asio/xxx.hpp` 命中 `boost/asio/xxx.hpp`
  (而 boost asio 头内部的 `#include <boost/asio/...>` 再由 `${Boost_INCLUDE_DIRS}` 命中),
  即用 include 根实现了 `boost::asio` → `asio` 的前缀映射; 再配合全局宏
  `NEOGRAPH_USE_BOOST_ASIO` (`namespace asio = ::boost::asio;`) 完成统一。
  参考: [lib/CMakeLists.txt](/agent/lib/CMakeLists.txt) 的 `${Boost_INCLUDE_DIRS}/boost/`
  与 [plugins/agentxx_filesystem/CMakeLists.txt](/agent/plugins/agentxx_filesystem/CMakeLists.txt) 顶部注释。
  → 三库只需 `find_package(Boost CONFIG REQUIRED)` + 同样的 include 根, **不需要任何
  neograph 依赖, 也不需要独立 asio 依赖** (§5.3)。
- **Boost 是预构建外部目录** (`BOOST_ROOT`, 见顶层 CMakeLists 的 `Boost_USE_*` 与
  `BOOST_ROOT` 校验), 顶层经 `_AGENTXX_COMMON_CMAKE_ARGS` 透传 `-DBOOST_ROOT/-DBoost_ROOT`,
  嵌套工程自行 `find_package(Boost)` 解析; 没有 boost 的 ExternalProject。
- 依赖 neograph 类型且**留在 agentxx** 的头 (路径与命名空间不变):
  `exception.h` (CancelToken / CancelledException / NodeInterrupt 分类)、
  `neograph_json_bridge.h` (`neograph/json.h`);
  插件框架侧另有 `plugin_graph_node.h` / `tool_registry.h` (图节点类型, 留 agentxx),
  以及改为 `utilxx::CancelToken` 的 `op_driver.h` (§5.2)。
- **迁出项**: `async_offload.h` (依赖 `neograph/define.h` + `graph/cancel.h`) 迁到
  `cxx_utilxx_base` 的 `utilxx/async_offload.h`, 改用 `utilxx::CancelToken`。

### 2.3 迁移影响面 (替换规模, 用于估算与自查)

| 检索项 | 文件数 | 出现次数 |
|---|---|---|
| `agentxx::util::` | 226 | 3 586 |
| `#include "agentxx/util/` | 222 | 457 |
| `agentxx::plugin::` | 76 | 942 |
| `#include "agentxx/plugin/` | 108 | 225 |
| `AGENTXX_PLUGIN_` 宏 | 79 | 1 256 |
| `AgentxxPlugin` C 类型 | 55 | 1 958 |

迁移后仍保留的 `agentxx::util::` 只剩三类文件:
`exception.h`(neograph 分类)、`neograph_json_bridge.h`、新增的取消适配器
(§5.2), 以及它们的调用点 (合计约 70 处)。**不提供转发头**, 其余调用点全部改为
`utilxx_base::` / `utilxx::`。

各 util 头被 include 的频次 (高 → 低, 决定迁移批次):
`log.h` 89 → `json.h` 62 → `string_util.h` 60 → `exception.h` 47 →
`neograph_json_bridge.h` 22 → `container_util.h` 19 → `http_client.h` 19 →
`http_server.h` 18 → `util.h` 15 → `asio_error.h` 13 → `async_offload.h` 13 →
`env.h` 13 → `json_view.h` 7 → `ws_client.h` 7 → `hash.h` 6 → `diff_util.h` 6 →
`regex.h`/`settings_db.h`/`sqlite.h` 5 → `http_header.h`/`router.h`/`aho_corasick.h` 4 →
`path_sanitize.h`/`worktree.h` 3 → `http_error.h`/`lru_cache.h`/`async_mutex.h` 2 → `stream.h` 1

### 2.4 插件侧现状 (20 个插件目标)

`example_plugin` `example_resources` `example_graph_node` `agentxx_javascript_engine`
`example_js` `example_js_execute_command` `agentxx_screen_capture` `agentxx_computer_use`
`agentxx_codegraph` `agentxx_system_monitor` `agentxx_audio_stream`
`agentxx_text_selection_monitor` `agentxx_string` `agentxx_math` `agentxx_planning`
`agentxx_filesystem` `agentxx_execute_command` `agentxx_system` `agentxx_websearch`
`agentxx_rag_search`

- 使用**重依赖 util** 的插件: `agentxx_websearch`、`agentxx_rag_search` (`http_client`);
  `agentxx_string`、`agentxx_filesystem` (`regex` / `aho_corasick`)。
- 使用 `exception.h` 的插件: `agentxx_filesystem`、`agentxx_rag_search`
  (该头留在 agentxx, 插件 include 路径与行为**保持不变**)。
- 无插件使用 `async_offload.h` (仅 lib 内 13 处), 故该项迁移不影响插件源码。
- 插件一律链接**三库的静态变体** (保持 `DT_NEEDED` 仅系统库)。

---

## 3. 目标结构

### 3.1 目录布局

```
agent/third_party/cxx_utilxx_base/
├── CMakeLists.txt
├── README.md
├── cmake/
│   ├── cxx_utilxx_baseConfig.cmake.in      # 手写 config (不依赖 CPM/网络)
│   ├── xx_platform_macros.cmake            # XX_IS_* / 编译器宏 推导与统一注入
│   ├── xx_rpath.cmake                      # 共享库 $ORIGIN 运行期搜索路径 (独立构建可用)
│   └── xx_export_symbols.map.in            # 共享库导出白名单 (ELF version script)
├── include/utilxx_base/                    # 基础件 (13 个)
│   ├── log.h  json.h  json_view.h  string_util.h  env.h  system.h
│   ├── container_util.h  hash.h  lru_cache.h  path_sanitize.h  stream.h
│   ├── async_mutex.h  asio_error.h
├── include/utilxx/                         # 跨库共享契约 (2 个, 命名空间 utilxx)
│   ├── cancel.h                            # utilxx::CancelToken / CancelTokenPtr /
│   │                                       #   CancelledException / SignalCancelToken
│   └── async_offload.h                     # 由 agentxx 迁出, 改用 utilxx::CancelToken
└── src/                                    # 6 个源
    ├── env.cpp  json.cpp  json_view.cpp  log.cpp  string_util.cpp  system.cpp

agent/third_party/cxx_utilxx/
├── CMakeLists.txt  README.md  cmake/{cxx_utilxxConfig.cmake.in, xx_rpath.cmake, ...}
├── include/utilxx/                         # 13 个
│   ├── http_client.h  http_header.h  http_error.h  http_server.h  ws_client.h
│   ├── router.h  sqlite.h  settings_db.h  regex.h  aho_corasick.h
│   ├── diff_util.h  worktree.h  crypto.h
└── src/                                    # 8 个源
    ├── http_client.cpp  http_header.cpp  http_server.cpp  ws_client.cpp
    ├── sqlite.cpp  settings_db.cpp  regex.cpp  crypto.cpp

agent/third_party/cxx_pluginxx/
├── CMakeLists.txt  README.md  cmake/{cxx_pluginxxConfig.cmake.in, xx_rpath.cmake, ...}
├── include/pluginxx/
│   ├── api/            # 纯 C ABI 通用基座 (C 头, 无 C++ 依赖)
│   │   ├── abi.h           # 调用约定/导出宏/版本/对齐/StringView/String/Info/Op/入口符号名
│   │   └── tables.h        # 通用接口表结构体 + IID: log/json/config/plugins/events/
│   │                       #   scheduler/coroutine_runtime/tasks/cancel/capabilities
│   ├── kit/            # 插件侧 C++ SDK 核心 (header-only)
│   │   ├── kit.h           # PluginBase / Task / 导出宏 / ArgReader / CancelRegistry / OpCtl
│   │   └── guard.h         # 边界异常守卫 (原 plugin_guard.h)
│   ├── runtime/        # 宿主侧运行时 (header-only, 原 2 541 行)
│   │   ├── runtime.h       # 原 plugin_runtime.h
│   │   ├── driver.h        # 原 plugin_driver.h
│   │   ├── op_driver.h     # 原 op_driver.h (取消令牌 = utilxx::CancelTokenPtr)
│   │   └── manager_base.h  # 原 plugin_manager_base.h + plugin_common.h 通用部分
│   └── host/           # 宿主侧实现 (源文件)
│       ├── loader.h        # NativeLoader (dlopen/LoadLibrary) + 入口符号查找
│       ├── manifest.h      # plugin.yaml 解析 / 名字推导 / 拓扑排序 / 反向依赖
│       ├── registry.h      # 实例表 / InflightGuard / 注册簿记
│       ├── tables_impl.h   # 通用表实现装配 (log/json/config/plugins/events/scheduler/
│       │                   #   coroutine_runtime/tasks/cancel/capabilities)
│       └── host_core.h     # PluginHostCore<InstanceT> (P4: 通用装载/启停/租约/op 记账)
└── src/
    ├── loader.cpp  manifest.cpp  registry.cpp  tables_impl.cpp  host_core.cpp
```

### 3.2 依赖矩阵

| 库 | 依赖 (PUBLIC 传递) | 条件/可选 |
|---|---|---|
| `cxx_utilxx_base` | fmt, simdjson, **Boost(头; 经 `find_package(Boost CONFIG)`)** | iconv + uchardet (`CXX_UTILXX_BASE_ENABLE_CHARSET`, 默认 ON) |
| `cxx_utilxx` | `cxx_utilxx_base`, Boost(beast / process), OpenSSL::SSL/Crypto, SQLite::SQLite3, fmt | hyperscan (`CXX_UTILXX_ENABLE_HYPERSCAN`) / io_uring / html2md |
| `cxx_pluginxx` | `cxx_utilxx_base`, fmt, Boost(头) | 无 (禁止依赖 neograph / OpenSSL / SQLite / Boost 编译库) |

说明: 三库都**不依赖 neograph**; `asio` 经 Boost 头 + `${Boost_INCLUDE_DIRS}/boost/`
include 根映射获得 (§5.3); Boost 头依赖 (含 `boost/beast` 的 base64 等内部头) 是允许的。

`find_dependency` 链: 处理方 `find_package(cxx_utilxx)` 时自动解析
`cxx_utilxx_base` 与其底层依赖 (见 §6.2)。

---

## 4. 文件归属清单 (逐文件)

### 4.1 `agentxx/util/` → 三处

**→ `cxx_utilxx_base`**

| 原文件 | 新路径 | 命名空间 | 备注 |
|---|---|---|---|
| `log.h` / `log.cpp` | `utilxx_base/log.h` `src/log.cpp` | `utilxx_base` | fmt + 文件轮转 |
| `json.h` / `json.cpp` | `utilxx_base/json.h` `src/json.cpp` | `utilxx_base` | simdjson 驱动 DOM |
| `json_view.h` / `json_view.cpp` | `utilxx_base/json_view.h` `src/json_view.cpp` | `utilxx_base` | 同上 |
| `string_util.h` / `string_util.cpp` | `utilxx_base/string_util.h` `src/string_util.cpp` | `utilxx_base` | iconv/uchardet 走开关; base64 继续用 `boost/beast/core/detail/base64.hpp` (允许直接依赖 Boost) |
| `container_util.h` `hash.h` `lru_cache.h` `path_sanitize.h` `stream.h` `async_mutex.h` | 同名 (头) | `utilxx_base` | 无重依赖 |
| `env.h` / `env.cpp` | `utilxx_base/env.h` `src/env.cpp` | `utilxx_base` | `.env` 解析 + 环境变量访问 |
| `asio_error.h` | `utilxx_base/asio_error.h` | `utilxx_base` | 宏改 `UTILXX_USE_BOOST_ASIO` (默认 ON, 兼容旧 `NEOGRAPH_USE_BOOST_ASIO`); `neograph_asio_*` 存量别名挪到 agentxx 侧 |
| **新增** `system.h` / `system.cpp` | `utilxx_base/system.h` `src/system.cpp` | `utilxx_base` | 从 `util.h/util.cpp` 拆出 `getSystemName` / `isRunningInWSL` / `detectPowerShell` / `isAsyncFileIoSupported` 三件套 |
| **迁入** `async_offload.h` | `utilxx/async_offload.h` | **`utilxx`** | 原 `agentxx/util/async_offload.h`; 去 neograph, 改用 `utilxx::CancelToken` (§5.2) |
| **新增** `cancel.h` | `utilxx/cancel.h` | **`utilxx`** | `CancelToken` 抽象 + `SignalCancelToken` 默认实现 + `CancelledException`; pluginxx 与 agentxx 共用 (§5.2) |

**→ `cxx_utilxx` (命名空间 `utilxx`)**

| 原文件 | 新路径 | 备注 |
|---|---|---|
| `http_client.h` / `.cpp` | `utilxx/http_client.h` `src/http_client.cpp` | Boost.Beast + OpenSSL + html2md |
| `http_server.h` / `.cpp` | `utilxx/http_server.h` `src/http_server.cpp` | Boost.Beast + asio |
| `ws_client.h` / `.cpp` | `utilxx/ws_client.h` `src/ws_client.cpp` | Beast SSL |
| `http_header.h` / `.cpp` `http_error.h` | 同名 | |
| `router.h` | `utilxx/router.h` | 依赖 `lru_cache.h` (base) |
| `sqlite.h` / `.cpp` `settings_db.h` / `.cpp` | 同名 | sqlite3 |
| `regex.h` / `.cpp` `aho_corasick.h` | 同名 | hyperscan 可选 |
| `diff_util.h` | `utilxx/diff_util.h` | fmt |
| `worktree.h` | `utilxx/worktree.h` | asio + boost.process |
| **新增** `crypto.h` / `crypto.cpp` | `utilxx/crypto.h` `src/crypto.cpp` | 从 `util.h/util.cpp` 拆出 `md5Hex` / `getDeviceId` (OpenSSL EVP) |

**→ 留在 agentxx (路径与命名空间都不变, 零改动)**

| 文件 | 说明 |
|---|---|
| `agent/lib/include/agentxx/util/exception.h` | neograph 分类 (`CancelToken` / `CancelledException` / `NodeInterrupt`); 内部 include 改指 `utilxx_base/string_util.h`; 分类器**新增** `utilxx::CancelledException` 分支 (视为 Cancelled, 见 §5.2) |
| `agent/lib/include/agentxx/util/neograph_json_bridge.h` | `neograph::json` ↔ `utilxx_base::Json` |
| **新增** `agent/lib/include/agentxx/util/cancel_adapter.h` | `NeographCancelTokenAdapter`: `std::shared_ptr<neograph::graph::CancelToken>` → `utilxx::CancelToken` (供 op_driver / offload 使用) |
| **新增** `agent/lib/include/agentxx/util/asio_compat.h` (可选) | `neograph_asio_system_error` / `neograph_asio_error_code` 存量别名 (若存量代码仍在用) |
| `util.h` / `util.cpp` | 拆分后删除 (内容分别进 `utilxx_base/system.*` 与 `utilxx/crypto.*`) |

> `agentxx/util/` 下**只保留上述 agentxx 自有头**, 且**不提供任何转发头**
> (不会出现 `agentxx/util/json.h` 之类的转发文件)。

### 4.2 `agentxx/plugin/` → 两处

**→ `cxx_pluginxx` (命名空间 `pluginxx`)**

| 原文件/片段 | 新位置 | 备注 |
|---|---|---|
| `api/plugin_api.h` 的**通用部分** (导出宏/调用约定/API 版本/对齐/StringView/String/Info/Op notify+handle+callback/DriveOnce/HostVtable/入口符号名宏/内联 helper) | `pluginxx/api/abi.h` | **C 名一律不变** |
| `api/plugin_api.h` 的**通用表** (log / json / config / plugins / events / scheduler / coroutine_runtime / tasks / cancel / capabilities) | `pluginxx/api/tables.h` | 表结构体名与 IID 字符串不变 |
| `api/plugin_kit.h` 的**通用部分** (PluginBase / Task / `sleep` `yield` `offload` `call_tool` `invoke_cap` / ArgReader / CancelRegistry / OpCtl / 导出宏 / PluginString 工具 / iface 查询) | `pluginxx/kit/kit.h` | |
| `api/plugin_guard.h` | `pluginxx/kit/guard.h` | |
| `plugin_runtime.h` | `pluginxx/runtime/runtime.h` | 无领域依赖 |
| `plugin_driver.h` | `pluginxx/runtime/driver.h` | 无领域依赖 |
| `op_driver.h` | `pluginxx/runtime/op_driver.h` | 取消令牌改为 **`utilxx::CancelTokenPtr`**; 抛出 `utilxx::CancelledException` (§5.2) |
| `plugin_common.h` + `plugin_manager_base.h` | `pluginxx/runtime/manager_base.h` | 无领域依赖 (750 + 502 行) |
| `src/plugins/plugin_common.cpp` 的通用函数 (`pluginNameFromPath` / `parsePluginManifest` / `resolvePluginEntryPath` / `topoSortPlugins` / `ioCallSync*` / `collectReverseRequiredDeps` / builtin manifest 查找) | `pluginxx/src/manifest.cpp` | |
| `src/plugins/plugin_manager_lifecycle.cpp` 的 `NativeLoader` 三函数 | `pluginxx/src/loader.cpp` | dlopen / LoadLibrary 封装 |
| `src/plugins/plugin_manager_{vtable,scheduler,tasks,capability}.cpp` 的**通用表实现** (log / json / config 骨架 / plugins / events / scheduler / coroutine_runtime / tasks / cancel / capabilities + 能力注册表) | `pluginxx/src/tables_impl.cpp` (+ 头) | **通用表的实现进 pluginxx**; 需要宿主数据的部分经 `DomainHooks` 取数 (§5.6) |
| `PluginHostCore` (P4 新增) | `pluginxx/include/pluginxx/host/host_core.h` | 通用装载 / 启停 / 租约 / inflight / op 记账 / 事件总线 |

**→ 留在 agentxx**

| 原文件 | 新位置 | 说明 |
|---|---|---|
| `plugin_api.h` 的**agent 领域表** (tools / permission / hooks / session / model / prompt / resources / graph) | `agent/lib/include/agentxx/plugin/api/plugin_api.h` (umbrella: `#include "pluginxx/api/abi.h"` + `"pluginxx/api/tables.h"` + 领域表) | **插件源码 include 路径不变**, 见 §5.5 |
| `client_plugin_api.h` 的**client 领域表** (client.ui / client.events / client.session / client.wire / client.self) | `agent/lib/include/agentxx/plugin/api/client_plugin_api.h` (umbrella, 同上) | **client 表留 agentxx (已确认)** |
| `plugin_kit.h` 的**领域部分** (ToolSchemaBuilder / blocking_tool / fast_tool / polled_tool / hook / capability / spawn / graph_node / registerToolRenderer / 权限声明辅助) | `agent/lib/include/agentxx/plugin/api/plugin_kit.h` (umbrella: `#include "pluginxx/kit/kit.h"` + 领域 helper) | 同上 |
| `plugin_manager.h` / `client_plugin_manager.h` / `tool_registry.h` / `builtin_tool_renderers.h` / `plugin_graph_node.h` | 原地 (`agent/lib/include/agentxx/plugin/`) | 领域宿主 |
| `src/plugins/{plugin_manager_vtable,plugin_manager_adapters,plugin_manager_capability,plugin_manager_lifecycle,plugin_manager_scheduler,plugin_manager_tasks,client_plugin_manager,tool_registry,builtin_tool_renderers,plugin_graph_node}.cpp` | 原地, 按 P4 改为组合 `pluginxx::PluginHostCore`; 领域表实现留在本处 | 领域实现 |

### 4.3 归属判据 (单一规则) 与守门方式

> **通用 = 与"会话/模型/工具/提示词/图"无关的表与类型; 领域 = 需要 agentxx 语义才能工作的表。**

拆完后应满足: `cxx_pluginxx` 源码中无 neograph 引用、不 include 任何 `agentxx/` 头;
`cxx_utilxx_base` / `cxx_utilxx` 源码中无 `agentxx/` / `neograph/` 引用。

**守门方式 (已确认不新增脚本)**: 依赖上述规则 + 编码约定 —— include 白名单
(`pluginxx` / `utilxx_base` / `utilxx` / `boost` / `asio` / `fmt` / 标准库),
由代码评审把关; 违反时通常会在编译期直接暴露 (找不到 `agentxx/...` 头)。

---

## 5. 关键设计决策

### 5.1 C 符号冻结 (已确认)

- 跨边界 **C 符号名** (`agentxx_plugin_agent_get_info/create/start/stop/destroy`,
  `agentxx_plugin_client_*`)、C 结构体名 (`AgentxxPluginHost` / `AgentxxPluginStringView` …)、
  宏名 (`AGENTXX_PLUGIN_CALL` / `AGENTXX_PLUGIN_EXPORT` / 各 IID 字符串) **全部保持不变**。
- 只改 **C++ 命名空间** (`agentxx::plugin` → `pluginxx`) 与头文件路径。
- 收益: 已编译插件二进制零改动即可继续工作; `plugins/CMakeLists.txt` 的 version script
  白名单、`lib/ffi_symbols.map`、内置清单生成、`check_plugin_exports.sh` 全部不动。
- 可选增强 (不阻塞): 在 `pluginxx/api/abi.h` 提供 `PLUGINXX_*` 宏别名与
  `typedef AgentxxPluginHost PluginxxHost;` 之类纯别名, 供第三方/后续宿主使用中性名。

### 5.2 取消抽象统一为 `utilxx::CancelToken` (已确认)

**问题**: 插件框架内核 (op_driver / runtime) 与 offload 工具都用
`neograph::graph::CancelToken` (成员类型 `std::shared_ptr<...>`, 调用
`fork()` / `bind_executor()` / `slot()` / `is_cancelled()`, 抛 `CancelledException`),
而 `cxx_pluginxx` 不能依赖 neograph。

**方案**: 在 `cxx_utilxx_base` 提供 `utilxx/cancel.h` (仅依赖 asio 与标准库):

```cpp
namespace utilxx {

/// 取消令牌抽象 (宿主可实现/适配任意取消源)
class CancelToken {
public:
    virtual ~CancelToken() = default;
    virtual bool isCancelled() const noexcept = 0;              // 轮询路径
    virtual void cancel() noexcept = 0;                         // 幂等, 任意线程
    virtual asio::cancellation_slot slot() noexcept = 0;        // asio 传播路径
    virtual void bindExecutor(asio::any_io_executor ex) = 0;    // 绑定 emit 的执行器
    virtual std::shared_ptr<CancelToken> fork() = 0;            // 子令牌, 级联取消
};
using CancelTokenPtr = std::shared_ptr<CancelToken>;

/// 默认实现: 与 neograph CancelToken 行为对齐 (atomic 标志 + asio::cancellation_signal,
/// cancel 幂等、可跨线程, bindExecutor 后经 post 在正确执行器上 emit, fork 级联),
/// 供 agentxx(无 neograph 场景) / musicxx 等宿主直接使用
class SignalCancelToken : public CancelToken { /* ... */ };

class CancelledException : public std::runtime_error { /* ... */ };

} // namespace utilxx
```

配套改动:

1. **pluginxx**: `runtime/op_driver.h` / `runtime/runtime.h` 的取消令牌类型改为
   `utilxx::CancelTokenPtr`, 抛出 `utilxx::CancelledException`; 其余逻辑 (slot 绑定、
   fork 子令牌、取消回调 weak 捕获) 保持不变。
2. **agentxx 适配**: 新增 `agentxx/util/cancel_adapter.h`
   (`NeographCancelTokenAdapter`: 包装 `std::shared_ptr<neograph::graph::CancelToken>`,
   转发 `isCancelled/cancel/slot/bindExecutor/fork`), 在 `plugin_manager_adapters.cpp`
   (现 `getSessionCancelToken` 处, 88–90 行) 与任何把 neograph 令牌传入 plugin 框架的位置
   套一层适配器。
3. **异常分类**: `agentxx/util/exception.h` 的 `classifyCurrentException` /
   `catchError*` **新增** `utilxx::CancelledException` 分支 (归为 `Cancelled`,
   与 neograph 版同语义), 保证 op_driver 抛出的取消异常仍被识别为控制流而非错误。
4. **offload 迁出**: `agentxx/util/async_offload.h` → `utilxx/async_offload.h`
   (`utilxx::offloadAsync` / `offloadCancellableAsync`), 取消路径改走
   `utilxx::CancelToken::slot()`, 业务体示例中的 `neograph::graph::CancelledException`
   改 `utilxx::CancelledException`; 13 处 include 与限定名替换。
   agentxx 侧**不加转发头**, 调用点直接改 (`utilxx::offloadAsync(...)`)。

### 5.3 asio 与 Boost 的导入方式 (已澄清)

- **asio 不是 neograph 提供的**: 源码里的 `asio/...` 前缀是构建侧做的"前缀映射" ——
  把 `${Boost_INCLUDE_DIRS}/boost/` 作为 include 根, 使 `asio/awaitable.hpp` 命中
  `boost/asio/awaitable.hpp`; 而 boost asio 内部 `<boost/asio/...>` 由
  `${Boost_INCLUDE_DIRS}` 命中; 二者**缺一会导致内部头回退到系统安装的 Boost,
  与宿主 asio 混用崩溃** (插件 CMakeLists 顶部注释已记录该坑)。
- 全局宏 `NEOGRAPH_USE_BOOST_ASIO` 提供 `namespace asio = ::boost::asio;`,
  使 `asio::` 与 boost asio 类型统一。
- **三库的做法**:

```cmake
find_package(Boost CONFIG REQUIRED)                        # cxx_utilxx 另加 COMPONENTS process
target_include_directories(cxx_utilxx_base SYSTEM PUBLIC
  ${Boost_INCLUDE_DIRS}            # boost asio 内部 <boost/asio/...>
  ${Boost_INCLUDE_DIRS}/boost/     # 源码 asio/... 前缀映射
)
target_compile_definitions(cxx_utilxx_base PUBLIC
  UTILXX_USE_BOOST_ASIO=1          # utilxx_base/asio_error.h 的 asio 别名开关 (兼容 NEOGRAPH_USE_BOOST_ASIO)
)
```

- **Boost 头依赖是允许的**: `utilxx_base` / `utilxx` / `pluginxx` 可直接使用 Boost
  头文件实现 (如 `boost/beast/core/detail/base64.hpp`、`boost/system`、`boost/process`),
  不做"去 Boost"替代实现; 仅 `pluginxx` 限制为**只用 Boost 头**, 不链接 Boost 编译库
  (process 等编译库仅 `utilxx` 使用)。
- `Boost_USE_STATIC_LIBS` / `BOOST_ROOT` 等既有设置由 superbuild 经
  `_AGENTXX_COMMON_CMAKE_ARGS` 继续透传; 三库只做 `find_package(Boost)` 与 include 传播。
- 交叉编译/其他宿主: 只需保证 `${Boost_INCLUDE_DIRS}` 与 `${Boost_INCLUDE_DIRS}/boost/`
  两个 include 根可用 (musicxx 同理)。

### 5.4 遗留 `agentxx::util` 与"不保留转发头" (已确认)

- agentxx **保留自己的 `agent/lib/include/agentxx/util/` 目录与 `agentxx::util` 命名空间**,
  只用于存放宿主/图引擎耦合件: `exception.h`、`neograph_json_bridge.h`、
  新增 `cancel_adapter.h` (与可选 `asio_compat.h`)。
- 这些文件**原地不动**, 因此相关调用点 (约 70 处) 零改动; 仅其内部 include 指向新的
  `utilxx_base/*`。
- **迁出的头一律用真实新路径**: 全仓替换
  `#include "agentxx/util/<头>"` → `#include "utilxx_base/<头>"` 或 `"utilxx/<头>"`,
  限定名 `agentxx::util::<符号>` → `utilxx_base::` / `utilxx::`; **不提供任何转发头**
  (不新建 `agentxx/util/json.h` 等, 也不提供 `using namespace` 别名头)。
- 结果: `agentxx::util::` 的检索命中数应从 3 586 降到 ~70 (仅剩上述 3 个自有头及其调用点)。

### 5.5 插件源码零改动的关键是 umbrella 头

- `agentxx/plugin/api/plugin_api.h`、`client_plugin_api.h`、`plugin_kit.h`
  **保留原路径与原文件名**, 内部改为 "include pluginxx 通用头 + 声明领域表/领域 helper"。
  → 20 个插件的 `#include "agentxx/plugin/api/plugin_kit.h"` 等**无需改动**。
- 插件侧若要用轻量库, 逐步把 `agentxx/util/json.h` 等改为 `utilxx_base/json.h`:
  迁移清单 = 各插件实际 include 的头 (见 §2.4), 其余保持不变。
- **不设 `agentxx/util/alias.h` 之类的过渡别名头** (已确认)。

### 5.6 `pluginxx::PluginHostCore` 与领域钩子 (P4 核心设计)

```cpp
namespace pluginxx {

/// 领域钩子: 由宿主 (agentxx / musicxx) 实现
class DomainHooks {
public:
    virtual ~DomainHooks() = default;
    /// 领域表查询路由 (通用表由 host core 处理, 领域表交给宿主)
    virtual const void* queryDomainIface(std::string_view iid, void* instance) = 0;
    /// 实例注册清理 (禁用/卸载时摘除该实例的领域注册)
    virtual void detachInstanceRegistrations(void* instance) = 0;
    /// 领域配置数据 (config 表 get_config / get_language / get_session_work_dir 等内容)
    virtual std::string domainConfigJson() = 0;
    /// 清单接口需求解析 (require/optional 的侧与能力名)
    virtual std::vector<std::string> requiredInterfaces(std::string_view side) = 0;
};

/// 通用宿主核心: 装载/卸载/启用/禁用/租约/inflight/op 记账/事件总线/能力注册表/日志/调度
template<typename InstanceT> class PluginHostCore { /* ... */ };

} // namespace pluginxx
```

- agentxx `PluginManager` / `ClientPluginManager` 改为
  `class PluginManager : public pluginxx::PluginHostCore<PluginInstance>,
   public pluginxx::DomainHooks`; 领域表实现 (tools/hooks/session/…) 留在 agentxx。
- **通用表的实现进 `cxx_pluginxx` (已确认)**: log / json / config 骨架 / plugins / events /
  scheduler / coroutine_runtime / tasks / cancel / capabilities; 其中需要宿主数据的部分
  经 `DomainHooks` 取数 (config 的模型/工作目录/语言、plugins 的插件清单等)。
- 该阶段是本次拆分**风险最高**的一步, 单独成阶段并单独验收 (§7 P4)。

### 5.7 表归属一览 (最终, 已确认)

| 归属 | 表 |
|---|---|
| `cxx_pluginxx` (通用, 定义**与实现**) | `log` `json` `config` `plugins` `events` `scheduler` `coroutine_runtime` `tasks` `cancel` `capabilities` |
| agentxx (领域, 定义与实现) | `tools` `permission` `hooks` `session` `model` `prompt` `resources` `graph` |
| **agentxx** (client 领域, 留在 agentxx) | `client.ui` `client.events` `client.session` `client.wire` `client.self` |

说明: `events` / `capabilities` 归通用 —— 二者只是"带 JSON 载荷的订阅/发布与 RPC",
不携带 agent 语义; 具体事件名 (如 `plugin.agentxx.round_start`) 由宿主定义。

---

## 6. 构建集成

### 6.1 三个工程的 CMakeLists 规约 (静态 + 动态双产物)

**产物命名 (照搬 libagentxx 规则)**:

| 目标 | Release | Debug |
|---|---|---|
| 动态库 | `libcxx_utilxx_base.so` / `.dylib` / `.dll` | `libcxx_utilxx_based.so` … |
| 静态库 | `libcxx_utilxx_base_static.a` / `.lib` | `libcxx_utilxx_base_staticd.a` … |

`cxx_utilxx` / `cxx_pluginxx` 同理 (`libcxx_utilxx.so` / `libcxx_utilxx_static.a`,
`libcxx_pluginxx.so` / `libcxx_pluginxx_static.a`; Debug 追加 `d`)。
> 备注: 依此规则 `cxx_utilxx_base` 的 Debug 动态库名为 `libcxx_utilxx_based.so`,
> 与 `based` 一词形近; 若实施时认为易误读, 可改为 `libcxx_utilxxd_base.so`
> (把 `d` 插在 `_base` 前) —— 属命名微调, 不改变本节其余规则。

```cmake
cmake_minimum_required(VERSION 3.10)
project(cxx_utilxx_base VERSION ${AGENTXX_VERSION} LANGUAGES C CXX)   # 独立构建时在工程内回退版本号

# 1) 平台/编译器宏: 优先用上层传入的 XX_IS_*_D, 未传入时本地推导 (cmake/xx_platform_macros.cmake)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/xx_platform_macros.cmake")

# 2) 开关 (无 shared/static 二选一开关: 两者都产出)
option(CXX_UTILXX_BASE_ENABLE_CHARSET "Enable iconv/uchardet support." ON)
option(CXX_UTILXX_BASE_USE_BOOST_ASIO "Use boost::asio error types."   ON)

# 3) 依赖 (imported target, 由 superbuild 安装树 / BOOST_ROOT 解析)
find_package(fmt REQUIRED)
find_package(simdjson REQUIRED)
find_package(Boost CONFIG REQUIRED)
if (CXX_UTILXX_BASE_ENABLE_CHARSET)
  find_package(iconv CONFIG REQUIRED)
  find_package(uchardet REQUIRED)
endif()

# 4) 目标: 静态 + 动态同时产出
#    - GCC/Clang: OBJECT 库编译一次, 共享/静态复用同一批 .o (与 lib/CMakeLists.txt 同法)
#    - MSVC: 双编译路径 (dllexport 等符号语义差异, 与 libagentxx 保持一致)
set(_CXX_UTILXX_BASE_SOURCES
  src/json.cpp src/json_view.cpp src/log.cpp src/string_util.cpp src/env.cpp src/system.cpp)
if (MSVC)
  add_library(cxx_utilxx_base_shared SHARED ${_CXX_UTILXX_BASE_SOURCES})
  add_library(cxx_utilxx_base_static STATIC ${_CXX_UTILXX_BASE_SOURCES})
else()
  add_library(cxx_utilxx_base_obj OBJECT ${_CXX_UTILXX_BASE_SOURCES})
  set_target_properties(cxx_utilxx_base_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(cxx_utilxx_base_shared SHARED $<TARGET_OBJECTS:cxx_utilxx_base_obj>)
  add_library(cxx_utilxx_base_static STATIC $<TARGET_OBJECTS:cxx_utilxx_base_obj>)
endif()
set(_CXX_UTILXX_BASE_TARGETS cxx_utilxx_base_shared cxx_utilxx_base_static
                            cxx_utilxx_base_obj)   # 去重/条件化见实现

# 名称与后缀: debug 加 d; 静态库加 _static; 统一 lib 前缀 (Windows 亦然)
if (XX_IS_DEBUG_D)
  set_target_properties(cxx_utilxx_base_shared PROPERTIES OUTPUT_NAME "cxx_utilxx_based")
  set_target_properties(cxx_utilxx_base_static PROPERTIES OUTPUT_NAME "cxx_utilxx_base_staticd")
else()
  set_target_properties(cxx_utilxx_base_shared PROPERTIES OUTPUT_NAME "cxx_utilxx_base")
  set_target_properties(cxx_utilxx_base_static PROPERTIES OUTPUT_NAME "cxx_utilxx_base_static")
endif()
set_target_properties(${_CXX_UTILXX_BASE_TARGETS} PROPERTIES
  PREFIX "lib" POSITION_INDEPENDENT_CODE ON CXX_STANDARD 26 CXX_STANDARD_REQUIRED OFF
  LINK_LIBRARIES_STRATEGY "REORDER_MINIMALLY")

# usage requirements (include/宏/依赖) 统一施加到全部目标 (静态库处理者需连带链接)
foreach(_t ${_CXX_UTILXX_BASE_TARGETS})
  target_include_directories(_t SYSTEM PUBLIC
    ${Boost_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS}/boost/)      # §5.3 asio 前缀映射
  target_compile_definitions(_t PUBLIC
    UTILXX_USE_BOOST_ASIO=1
    XX_IS_LINUX_D=${XX_IS_LINUX_D} ... )                     # 与现有开源头一致
  target_link_libraries(_t PUBLIC fmt::fmt simdjson::simdjson ...)  # 全部 PUBLIC 传递
endforeach()

# 运行期搜索路径 (仅 shared 需要): $ORIGIN 相对 exec 目录, 见 cmake/xx_rpath.cmake
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/xx_rpath.cmake")
cxx_utilxx_set_own_dir_rpath(cxx_utilxx_base_shared)

# 5) 导出: 手写 config (不经 CPM/PackageProject, 避免网络依赖), 静态/动态都安装
install(TARGETS cxx_utilxx_base_shared cxx_utilxx_base_static
        EXPORT cxx_utilxx_baseTargets
        DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}")        # 与 libagentxx 一致: 产物落 exec
install(DIRECTORY include/ DESTINATION include/)             # include/utilxx_base/ 与 include/utilxx/
install(EXPORT cxx_utilxx_baseTargets NAMESPACE "" DESTINATION lib/cmake/cxx_utilxx_base)
configure_package_config_file(cmake/cxx_utilxx_baseConfig.cmake.in ...)
```

要点:

1. **不使用 CPM / PackageProject** (libagentxx 现在经 CPM 下载 PackageProject.cmake,
   需要网络); 三库改用手写 `*-config.cmake.in` + `install(EXPORT)`, 零网络依赖。
2. **动态库导出面控制** (与 `plugins/CMakeLists.txt` / `ffi_symbols.map` 同思路):
   - ELF (Linux/Android): `-fvisibility=hidden -fvisibility-inlines-hidden` +
     version script 白名单 (`global: *utilxx*; *pluginxx*; local: *` 之类按 mangled 名
     通配), 隐藏静态链入的第三方符号;
   - macOS: `-exported_symbols_list`;
   - MSVC: 默认 `WINDOWS_EXPORT_ALL_SYMBOLS=OFF`; 动态库打开
     `WINDOWS_EXPORT_ALL_SYMBOLS=ON` 自动生成 .def (实施时需验证导出面只含本库符号,
     若第三方符号一并导出则退回显式 `__declspec(dllexport)` 标注)。
3. 头文件 install 到 `<prefix>/include/{utilxx_base,utilxx,pluginxx}/...`,
   与 `agentxx/` 并列, 互不遮蔽。
4. PCH / sanitizer / LTO 由上层 `CMAKE_CXX_FLAGS*` 与 `_AGENTXX_COMMON_CMAKE_ARGS` 透传;
   三库各自提供 `CXX_*_ENABLE_PCH` (默认 OFF, 只预编译 std 头)。
5. `cxx_pluginxx` 必须**不链接** Boost 编译库 (仅头) 与 OpenSSL/SQLite。

### 6.2 `find_dependency` 链 (config 文件内容)

```cmake
# cxx_utilxx_baseConfig.cmake.in
find_dependency(fmt)
find_dependency(simdjson)
find_dependency(Boost CONFIG)                  # asio 前缀映射所需的 include 根 (§5.3)
if (@CXX_UTILXX_BASE_ENABLE_CHARSET@)
  find_dependency(iconv CONFIG); find_dependency(uchardet)
endif()
# 导出目标: cxx_utilxx_base_shared / cxx_utilxx_base_static (含 INTERFACE 依赖)

# cxx_utilxxConfig.cmake.in
find_dependency(cxx_utilxx_base)               # 经 CMAKE_PREFIX_PATH(安装树) 解析
find_dependency(Boost CONFIG COMPONENTS process)
find_dependency(OpenSSL); find_dependency(SQLite3 CONFIG)

# cxx_pluginxxConfig.cmake.in
find_dependency(cxx_utilxx_base); find_dependency(fmt); find_dependency(Boost CONFIG)
```

- 处理方按需链接:`cxx_utilxx_base_static` / `cxx_utilxx_base_shared`
  (与现有 `agentxx_static` / `agentxx_shared` 用法一致)。
- 注意沿用 `lib/CMakeLists.txt` 已踩过的坑: `iconv` 必须写 `iconv CONFIG`
  (大写 `Iconv` 在 CMake ≥ 3.28 会命中内置 FindIconv 模块导致 `iconv_FOUND` 误判)。

### 6.3 `agent/CMakeLists.txt` (superbuild) 改动

在 fmt/simdjson/uchardet/libiconv/sqlite3/openssl/hyperscan/uring 之后、
`agentxx_lib_repo` 之前插入三个 `ExternalProject_Add`:

```cmake
# ===== cxx_utilxx_base / cxx_utilxx / cxx_pluginxx (本项目自研基础库) =====
ExternalProject_Add(cxx_utilxx_base_repo
  DEPENDS fmt_repo simdjson_repo libiconv_repo uchardet_repo
  SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/cxx_utilxx_base/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS ${_AGENTXX_COMMON_CMAKE_ARGS}
    -DCXX_UTILXX_BASE_ENABLE_CHARSET=ON
    -DCXX_UTILXX_BASE_USE_BOOST_ASIO=ON
  CMAKE_CACHE_ARGS ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}     # 含 -DBOOST_ROOT/-DBoost_ROOT
  TEST_COMMAND "")

ExternalProject_Add(cxx_utilxx_repo
  DEPENDS cxx_utilxx_base_repo sqlite3_repo fmt_repo
  SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/cxx_utilxx/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS ${_AGENTXX_COMMON_CMAKE_ARGS}
    -DCXX_UTILXX_ENABLE_HYPERSCAN=${AGENTXX_ENABLE_HYPERSCAN}
  CMAKE_CACHE_ARGS ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND "")

ExternalProject_Add(cxx_pluginxx_repo
  DEPENDS cxx_utilxx_base_repo fmt_repo
  SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/cxx_pluginxx/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS ${_AGENTXX_COMMON_CMAKE_ARGS}
  CMAKE_CACHE_ARGS ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND "")

# 条件依赖: hyperscan / io_uring 由 cxx_utilxx 使用
if (AGENTXX_ENABLE_HYPERSCAN) add_dependencies(cxx_utilxx_repo hyperscan_repo) endif()
if (AGENTXX_LINUX_IO_URING_SUPPORTED) add_dependencies(cxx_utilxx_repo liburing_repo) endif()
```

要点:

- **Boost 无需新增任何参数**: 顶层已把 `BOOST_ROOT` / `Boost_USE_*` 放进
  `_AGENTXX_COMMON_CMAKE_ARGS(_CACHE_ARGS)`, 三库自行 `find_package(Boost CONFIG)`。
- 在 `agentxx_lib_repo` 的 `DEPENDS` 追加
  `cxx_utilxx_base_repo cxx_utilxx_repo cxx_pluginxx_repo` (client/test/benchmark
  经 `agentxx_lib_repo` 间接依赖)。
- 交叉编译脚本 (`cross_android_release_build.sh` / `cross_windows_release_build.sh`)
  同步检查 Boost 相关透传项即可 (无新增 asio 参数)。

### 6.4 各模块 `find_package` 改造清单

| 模块 | 改动 |
|---|---|
| `agent/lib/CMakeLists.txt` | 删除 `agentxx_util` 目标与 `list(FILTER agentxx_SOURCES EXCLUDE REGEX "/src/util/.*")` (util 源已不在本仓库); 新增 `find_package(cxx_utilxx_base REQUIRED)` / `find_package(cxx_utilxx REQUIRED)` / `find_package(cxx_pluginxx REQUIRED)`; **链接静态变体** (`*_static`), 策略沿用 `obj PRIVATE` + `shared/static PUBLIC`; `agentxx_DEPENDENCIES` 增加三库 (转 `find_dependency`) |
| `agent/plugins/CMakeLists.txt` + 20 个插件 | `find_package(cxx_pluginxx REQUIRED)` (取得 `pluginxx/api/*.h` 与 kit); 用 util 的插件改 `find_package(cxx_utilxx_base REQUIRED)`; `agentxx_websearch`/`agentxx_rag_search` 用 `find_package(cxx_utilxx REQUIRED)`; **一律链接 `*_static`** (保持 `DT_NEEDED` 仅系统库); asio 头继续用 `${_BOOST_INCLUDES}` (`${BOOST_ROOT}/include/` + `${BOOST_ROOT}/include/boost/`), 现状不变; `AGENTXX_LIB_INCLUDE_DIR` 保留 (装 `agentxx/plugin/**` umbrella 头与 `agentxx/util/exception.h`) |
| `agent/client`, `agent/test`, `agent/benchmark`, `agent/ffi`, `agent/example` | 经 `find_package(agentxx_static)` 的 `find_dependency` 链已能拿到三库; 仅"直接 include utilxx/pluginxx 头"的源文件所在模块需显式 `find_package` 并 `target_link_libraries` |
| 需要"进程内单副本"的宿主 (后续 musicxx) | 改用 `cxx_utilxx_base_shared` / `cxx_pluginxx_shared` (动态变体), 避免同进程出现两份实现 |
| `agent/script/*` | `check_plugin_exports.sh` / `check_sdk_negative_compile.sh` 只需更新注释与 include 提示 (符号面不变) |

### 6.5 安装布局 (安装树视角)

```
<AGENTXX_INSTALL_DIR>/
├── include/{utilxx_base,utilxx,pluginxx,agentxx,neograph,fmt,simdjson,...}/
├── lib/cmake/{cxx_utilxx_base,cxx_utilxx,cxx_pluginxx,agentxx_shared,agentxx_static}/
│   （每个包导出 shared/static 两个 imported target）
└── exec/                       # 与 libagentxx 同目录, 便于 $ORIGIN 复用运行动态库
    ├── libcxx_utilxx_base.so          libcxx_utilxx_base_static.a
    ├── libcxx_utilxx.so               libcxx_utilxx_static.a
    ├── libcxx_pluginxx.so             libcxx_pluginxx_static.a
    └── libagentxx.{so,a} + 插件目录 plugins/<插件名>/
   (Debug 构建: libcxx_utilxx_based.so / libcxx_utilxx_base_staticd.a 等)
```

---

## 7. 实施阶段 (P0–P6: 每阶段可编译、可回滚)

> 总体原则: **先搬迁、后重构**。P0–P3 是"机械搬迁 + 命名空间/路径替换", 行为零变化;
> P4 才做宿主核心抽取。每个阶段结束都必须能完整跑通 §8 的验收项。

### P0 骨架与构建接入 (工作量: S)

1. 新建三个目录与 `CMakeLists.txt` / `cmake/*.in` / `README.md`, 先放**空目标 + 头文件占位**;
   静态与动态两种产物一次到位 (命名规则 §6.1)。
2. `agent/CMakeLists.txt` 加三个 `ExternalProject_Add` 与 `agentxx_lib_repo` 的 DEPENDS。
3. 各模块加 `find_package` 调用但暂不链接新库 (保持旧 `agentxx_util` 仍可用)。

**验收**: 配置 + 构建通过; `<install>/lib/cmake/...` 下三个包可见且各自导出
shared/static 目标; `exec/` 下六个产物 (3 库 × 2 变体) 存在且命名符合规则;
现有测试与插件产物完全不变。
**回滚**: 删除三目录 + 还原 `agent/CMakeLists.txt`。

### P1 `cxx_utilxx_base` 搬迁 (工作量: M)

1. `git mv` 13 头 + 6 源到 `cxx_utilxx_base/`, 命名空间 `agentxx::util` → `utilxx_base`,
   宏 `XX_*` 保持, `NEOGRAPH_USE_BOOST_ASIO` → `UTILXX_USE_BOOST_ASIO`;
   `git mv` `async_offload.h` 到 `include/utilxx/` 并改 `utilxx::CancelToken`;
   新增 `include/utilxx/cancel.h`(抽象 + `SignalCancelToken`)。
2. 全仓替换 include 与限定名 (**不建转发头**):
   `agentxx/util/<base 头>` → `utilxx_base/<头>`;
   `agentxx/util/async_offload.h` → `utilxx/async_offload.h`;
   `agentxx::util::<符号>` → `utilxx_base::` / `utilxx::`。
3. `util.h/util.cpp` 拆分: `system.*` 进 base, `md5Hex`/`getDeviceId` 暂留待 P2。
4. agentxx 侧新增 `agentxx/util/cancel_adapter.h`, 并在
   `agentxx/util/exception.h` 的分类器加入 `utilxx::CancelledException` 分支。

**验收**: Windows/Linux Debug 构建通过; `agentxx_test string_util json* plugin_*`
全绿; `agentxx_test` 全量无新增失败; 取消/中断相关用例 (interrupt/cancel) 无回归;
两个内置插件的 DT_NEEDED 不变。

### P2 `cxx_utilxx` 搬迁 (工作量: M)

1. `git mv` 13 头 + 8 源 (含新拆的 `crypto.*`), 命名空间 → `utilxx`。
2. 删除 `agentxx_util` 目标与相关 CMake 分支; `lib/CMakeLists.txt` 改用
   `cxx_utilxx_static` / `cxx_utilxx_base_static` 链接; 更新 `agentxx_DEPENDENCIES`。
3. 插件侧: `agentxx_websearch` / `agentxx_rag_search` / `agentxx_string` /
   `agentxx_filesystem` 的 include 与 `find_package` 适配。
4. agentxx 侧 `agentxx/util/` 只剩 `exception.h` / `neograph_json_bridge.h` /
   `cancel_adapter.h` (内部 include 指向 `utilxx_base/*`)。

**验收**: 上述 + `agentxx_test regex plugin_bridge` 等全绿; 插件 `DT_NEEDED`
仅系统库; `check_plugin_exports.sh` 通过。

### P3 `cxx_pluginxx` Tier-1 搬迁 (机械, 工作量: L)

按顺序三个检查点, 每个检查点都要构建通过:

1. **3a ABI 分层**: 从 `plugin_api.h` / `client_plugin_api.h` / `plugin_guard.h`
   抽出通用部分到 `pluginxx/api/abi.h` + `pluginxx/api/tables.h` + `pluginxx/kit/guard.h`;
   agentxx 侧三个头改为 umbrella (include pluginxx + 领域表), **插件源码零改动**。
2. **3b 运行时搬迁**: `plugin_runtime.h` / `plugin_driver.h` / `op_driver.h` /
   `plugin_common.h` / `plugin_manager_base.h` → `pluginxx/runtime/*`,
   命名空间 `agentxx::plugin` → `pluginxx`; `op_driver.h` 的取消令牌换成
   `utilxx::CancelTokenPtr`, 异常换 `utilxx::CancelledException`;
   agentxx 侧在传令牌处套 `NeographCancelTokenAdapter`。
3. **3c SDK 与通用实现搬迁**: `plugin_kit.h` 拆成 `pluginxx/kit/kit.h` + agentxx 领域
   helper (umbrella 保留路径); `plugin_common.cpp` 通用函数与 `NativeLoader` 进
   `pluginxx/src/`; 通用表实现迁入 `pluginxx/src/tables_impl.cpp`
   (config/plugins 等需要宿主数据处经 `DomainHooks`, 该接口可在本步先给最小实现,
   P4 再收敛)。

**验收**:
- 20 个插件全部编译通过; 内置合并模式同样通过 (`-DAGENTXX_PLUGIN_BUILTIN_LIST=all` 与具名名单各一次)。
- `agentxx_test plugin_runtime plugin_sdk plugin_bridge plugins plugin_resources plugin_multi_instance` 全绿。
- `check_plugin_exports.sh` / `check_sdk_negative_compile.sh` 通过。
- **拆分前编译的示例插件 `.so`/`.dll` 能被新宿主加载并正常工作** (ABI 冻结证明)。
- `-DAGENTXX_PLUGIN_UBSAN_PROBE=ON` 跑一次插件测试。

### P4 `pluginxx::PluginHostCore` 抽取 (重构, 工作量: L)

1. 定义 `DomainHooks` (见 §5.6) 与 `PluginHostCore<InstanceT>`; 先只搬
   "实例表 / inflight / 租约 / op 记账 / 事件总线 / 能力注册表 / 调度 / 任务托管 /
   通用表装配", 暂不动装载流程。
2. `PluginManager` 改为组合/继承 host core, 领域表实现保持不动; 跑全量回归。
3. 再把装载/卸载/启用/禁用/级联依赖 (即 `plugin_manager_lifecycle.cpp` 的
   通用骨架) 搬进 host core; agentxx 只保留 `DomainHooks` 实现与领域表。
4. `ClientPluginManager` 同样处理 (client 侧 UI 表留在 agentxx)。

**验收**: `plugin_*` 全量测试 + FFI 测试 (`ffi_c_api`) 全绿; TUI/CLI 手工冒烟
(插件加载、禁用、卸载、重载、级联依赖、插件工具调用、client 面板/状态栏);
插件取消/中断路径回归 (含 `utilxx::CancelToken` 适配路径); 无新增 UBSan 报告。

### P5 清理与文档 (工作量: S)

1. 删除 `agentxx_util` 残留引用、旧 `agent/lib/src/util/` 空目录、
   `agent/lib/include/agentxx/util/` 下已迁出的头 (保留 3 个 agentxx 自有头)、
   注释中的历史说明。
2. 更新 [design/plugins.md](/docs/zh-cn/design/plugins.md) §5 (工具函数复用 → 三库结构
   与 `utilxx::CancelToken`、静态/动态双产物)、[design/index.md](/docs/zh-cn/design/index.md)
   代码结构章节、`AGENTS.md` 的"代码结构""编译"章节。
3. 记录本目录的 `done.md` (实施记录 + 验收输出摘要)。

### P6 下游与发布 (工作量: S, 可并行)

1. 三库 `README.md`: 定位、依赖、`find_package` 用法、命名空间示例、
   静态/动态变体选择指引 (宿主内单副本 → 动态变体)、平台支持
   (含 iOS 无 dlopen 的限制说明)。
2. 更新 `agent/ffi/dart` 绑定与 [design/ffi.md](/docs/zh-cn/design/ffi.md) 中与
   头文件路径相关的说明 (若有)。
3. 给出 musicxx 接入说明 (见 §11), 不实施。

---

## 8. 验证与验收矩阵

| 类别 | 命令/方法 | 通过标准 |
|---|---|---|
| 构建 (Win Debug) | `agent\script\windows_debug_build.bat` | 无 error; 关键词 `Built target` 全覆盖 |
| 构建 (Linux Debug) | `agent/script/linux_debug_build.sh` | 同上 |
| 产物命名 | 检查 `exec/` 下六个文件 | `libcxx_utilxx_based.so` / `libcxx_utilxx_base_staticd.a` 等符合 §6.1 规则 (Debug) |
| 全局测试 | `{build}/exec/agentxx_test -f` | 全部模块通过 |
| 定向测试 | `agentxx_test string_util regex plugin_runtime plugin_sdk plugin_bridge plugins plugin_resources plugin_multi_instance ffi_c_api` | 全绿 |
| 取消抽象回归 | interrupt / cancel / offload 相关用例 + 插件取消路径 (含 neograph 适配令牌) | 行为与拆分前一致 |
| 导出面 (插件) | `agent/script/check_plugin_exports.sh` | 插件仅导出 `agentxx_plugin_{agent,client}_*` |
| 导出面 (三库动态变体) | `readelf -d --dyn-syms` 三库 `.so` | 仅导出本库命名空间符号; 无第三方静态库符号; `DT_NEEDED` 白名单 |
| SDK 约束 | `agent/script/check_sdk_negative_compile.sh` | 正向片段编译成功; 负向片段按预期失败 |
| ABI 冻结 | 用拆分**前**构建的 `example_plugin` 动态库放入 `exec/plugins/`, 由新宿主加载并调用一次工具 | 加载成功、调用返回预期结果 |
| 依赖面 (静态变体) | `readelf -d *.so` / `dumpbin /dependents *.dll` | 插件与静态链接方仅依赖系统库 |
| 未定义行为 | `-DAGENTXX_PLUGIN_UBSAN_PROBE=ON` + 插件测试 | 无 UBSan 报告 |
| 交叉编译 | `cross_android_release_build.sh` (Linux 主机) | 配置/构建通过 (Boost 透传正常) |
| 结构 | `grep -rn "agentxx::util::" cxx_utilxx*/ cxx_pluginxx/` | 0 命中 (仅 agentxx 侧保留) |

---

## 9. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 命名空间/路径批量替换误伤 (注释、字符串、C 名) | 编译错误或 ABI 破坏 | 只替换 `agentxx::util::` / `agentxx::plugin::` / `#include "agentxx/util/` / `#include "agentxx/plugin/` 四类精确模式; C 名 (`AgentxxPlugin*`, `agentxx_plugin_*`) 与保留的 3 个 agentxx 头列入**禁改清单**; 每阶段全量构建 |
| 动态库导出面污染 (静态链入的第三方符号被导出 / 本库符号被隐藏) | 符号冲突、宿主误绑定 | ELF: `-fvisibility=hidden` + version script 白名单; macOS: `-exported_symbols_list`; MSVC: 先试 `WINDOWS_EXPORT_ALL_SYMBOLS` 并**验证导出面**, 不符预期则改显式 `dllexport`; 验收表含 `readelf -d --dyn-syms` 检查 |
| 静态/动态变体混用导致同进程两份实现 | 状态不一致、体积翻倍 | 明确链接策略: agentxx 内部与插件一律静态; 需要单副本的宿主用动态变体 (README 写明) |
| 取消抽象迁移引入行为偏差 (adapter/fork/slot 语义) | 中断/取消路径异常或卡死 | `SignalCancelToken` 行为对齐 neograph 实现 (atomic 标志 + `cancellation_signal` + fork 级联); adapter 单测 (cancel 幂等/跨线程/emit 时机) + interrupt/cancel 用例回归 + UBSan 探针 |
| `pluginxx` 使用 `utilxx::CancelledException` 后 agentxx 分类器漏认 | 取消被当成错误上报 | §5.2 第 3 条: `agentxx/util/exception.h` 分类器显式新增分支; 回归用例覆盖 |
| CMake 目标改名导致旧 build 目录缓存失效/异常 | 构建报错、增量失效 | 每阶段前删除 `agent/build/<platform>-<cfg>` 或至少清理 `agentxx_lib_repo-prefix`; 文档明确要求 |
| `plugin_kit.h` (5 635 行) 拆分破坏插件 SDK | 20 个插件编译失败 | umbrella 头保持路径与 API 不变; 先机械搬迁后分层; `check_sdk_negative_compile.sh` 兜底 |
| `PluginHostCore` 抽取 (P4) 触及核心生命周期 | 卸载崩溃 / `dlclose` UAF | 拆成 4 个小步各自验收; 复用 `plugin_multi_instance` / 卸载重载测试; 保留 P3 结束点为稳定回退点 |
| 无隔离检查脚本, 依赖约定 | 三库反向依赖宿主不易及时发现 | 编译期天然暴露 (找不到 `agentxx/...` 头); 代码评审按 §4.3 白名单把关; 三库 README 写明"禁止依赖 agentxx/neograph" |
| 迁移期 rebase 冲突 (大范围替换) | 开发摩擦 | 一次性小步提交 (每阶段 1–3 个 commit), 期间尽量不并行大改 lib/plugins |

---

## 10. 决策状态

### 10.1 已确认 (本方案按此实施)

| # | 决策 | 结论 |
|---|---|---|
| 1 | C 符号 / C 结构体名 | **冻结**: 只改 C++ 命名空间与头路径; 可选提供 `PLUGINXX_*` 纯别名 |
| 2 | neograph 归属 | **留在 agentxx**: `exception.h` / `neograph_json_bridge.h` 原地保留 (agentxx 保有 `agentxx/util/` 目录与 `agentxx::util` 命名空间); 仅 `async_offload` 迁出 |
| 3 | 取消抽象 | **统一为 `utilxx::CancelToken`** (定义在 `cxx_utilxx_base` 的 `utilxx/cancel.h`), pluginxx 与 agentxx 共用; agentxx 侧加 neograph 适配器 |
| 4 | 通用表 | **定义与实现都进 `cxx_pluginxx`** (log/json/config/plugins/events/scheduler/coroutine_runtime/tasks/cancel/capabilities), 宿主数据经 `DomainHooks` 提供 |
| 5 | asio 来源 | **Boost.Asio**: `find_package(Boost CONFIG)` + `${Boost_INCLUDE_DIRS}` 与 `${Boost_INCLUDE_DIRS}/boost/` 两个 include 根实现 `asio/...` 前缀映射; 与 neograph 无关 |
| 6 | 转发头 | **不保留** `agentxx/util/*.h` 转发/别名头; 迁出项一律真实路径替换 |
| 7 | client 侧表 | **留在 agentxx** (client.ui / events / session / wire / self) |
| 8 | 库产物形态 | **静态 + 动态同时产出**, 命名仿 libagentxx: Debug 加 `d`, 静态库加 `_static`, 统一 `lib` 前缀 (§6.1) |
| 9 | 检查脚本 | **不新增**隔离检查脚本/门禁; 以 §4.3 的 include 白名单约定 + 代码评审把关 |
| 10 | Boost 依赖 | 三库**可直接依赖 Boost 头** (含 base64 等实现细节), 不做替代实现; `pluginxx` 仅用 Boost 头不链接 Boost 编译库 |
| 11 | 三库无需提供 `*_ENABLE_PCH` | - |
| 12 | `agentxx/util/asio_compat.h` (`neograph_asio_*` 存量别名) 不写入文件 | 删除neograph_asio_*别名，彻底替代 |

---

## 11. 拆分完成后的复用形态 (预告, 本次不实施)

1. **其他宿主 (musicxx 等) 只需** `find_package(cxx_utilxx_base)` +
   `find_package(cxx_pluginxx)`, 按需选择静态/动态变体, 即可获得插件装载/生命周期/
   租约/取消/协程驱动全部能力; `cxx_utilxx` 按需 (HTTP/WS/SQLite/正则)。
2. 取消语义直接复用 `utilxx::CancelToken` / `SignalCancelToken` (`cxx_utilxx_base` 提供),
   宿主无需自造取消机制。
3. musicxx 侧只需实现自己的 `DomainHooks` + 领域表 (链接解析 / 声明式 UI / 配置 schema),
   即可复用同一套插件运行时与 `dlopen` 装载纪律 (导出白名单 + 清单 + 级联依赖 + 卸载安全)。
4. 与内嵌 agent 的协同: 内嵌 `libagentxx` 的插件宿主 (agent 侧表) 与 musicxx 宿主
   (音乐侧表) 在同进程内各自独立; 两侧经 capability / 事件表做桥接
   (例如把 musicxx 插件能力注册为 agent 工具)。同进程要避免两份实现时,
   统一使用三库的动态变体 (单副本)。
5. 客户端 UI 扩展在 Flutter 侧只能走**声明式 schema + 动作回调**
   (与 agentxx `agentxx.client.ui` 同构的声明式风格, 词汇表由 musicxx 定义);
   该结论在设计 musicxx 插件框架时复用, 与本拆分方案无冲突。

---

## 附: 实施检查清单 (可直接勾选)

- [ ] P0 三目录骨架 (静态+动态双产物、命名规则) + superbuild 接入 + 三包可 `find_package`
- [ ] P1 `cxx_utilxx_base` 搬迁 (含 `utilxx/cancel.h`、`utilxx/async_offload.h`) + 全仓替换 (无转发头) + 测试全绿
- [ ] P1 `agentxx/util/cancel_adapter.h` + 分类器新增 `utilxx::CancelledException` 分支 + 取消用例回归
- [ ] P2 `cxx_utilxx` 搬迁 + `agentxx_util` 退役 + agentxx 侧仅留 3 个自有头 + 测试全绿
- [ ] P3-3a ABI 分层 (`pluginxx/api/*`) + 插件源码零改动构建通过
- [ ] P3-3b 运行时搬迁 (`pluginxx/runtime/*`) + `utilxx::CancelTokenPtr` 生效
- [ ] P3-3c SDK 与通用表实现搬迁 + 内置合并模式构建通过
- [ ] P3 验收: 旧插件二进制可加载 + 导出检查 + 负向编译 + UBSan 探针
- [ ] P4-1 `DomainHooks` + `PluginHostCore` 骨架 + 回归
- [ ] P4-2 装载/启停/级联搬迁 + 回归
- [ ] P4-3 `ClientPluginManager` 适配 + TUI/CLI 手工冒烟
- [ ] P5 清理残留 + 文档更新 (`plugins.md` / `index.md` / `AGENTS.md`) + `done.md`
- [ ] P6 三库 README (含静态/动态选择指引) + 下游/musicxx 接入说明
