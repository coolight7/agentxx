# cxx_pluginxx

插件框架内核 (纯 C ABI 基座 + 通用接口表 + 宿主运行时 + 插件 SDK 核心)。

## 定位

- **用途**: 与宿主领域无关的插件框架 —— 动态库装载、生命周期 (create/start/stop/destroy)、
  清单与依赖拓扑、接口表查询、协程驱动、取消与卸载 (> 通用接口表实现见下)
- **宿主领域内容不在此**: agent 侧的工具/权限/钩子/会话/模型/提示词/资源/图,
  client 侧的 UI/事件/会话/线路/自身信息, 均由宿主 (agentxx、musicxx 等) 自行定义与实现
- **依赖**: `cxx_utilxx_base` (取消令牌/日志/JSON)、fmt、Boost (仅头文件, 不链接 Boost 编译库)
  —— **禁止**依赖 neograph / OpenSSL / SQLite / agentxx 头文件

## 跨边界 C ABI (不可变契约)

- 符号名: `agentxx_plugin_agent_get_info/create/start/stop/destroy`、
  `agentxx_plugin_client_*` (改动即破坏已编译插件)
- 结构体名 / 宏名 / IID 字符串: `AgentxxPluginHost`、`AgentxxPluginString`、
  `AGENTXX_PLUGIN_CALL`、`AGENTXX_PLUGIN_EXPORT`、`"agentxx.agent.tools"` 等
- 调用约定与对齐: `AGENTXX_PLUGIN_CALL` (Windows `__stdcall`)、8 字节对齐、定长基础类型
- 版本: `AGENTXX_PLUGIN_API_VERSION` (接口表 `version` / `struct_size` 自校验)

## 目录结构

```
include/pluginxx/
  api/    abi.h    纯 C ABI 基座 (导出宏/调用约定/版本/对齐/StringView/String/
                   Info/统一操作原语/事件订阅前向声明/宿主 vtable/入口符号名/
                   内置合并描述) —— 与宿主领域无关
          tables.h 通用接口表 (log/json/config/plugins/events/capabilities/
                   scheduler/coroutine_runtime/tasks/cancel)
  kit/    插件侧 C++ SDK (PluginBase/Task/导出宏/参数提取/取消注册/边界异常守卫)
  runtime/宿主侧运行时 (runtime/driver/op_driver/manager_base)
  host/   宿主侧通用实现 (loader/manifest/registry/tables_impl/host_core)
src/      对应实现 (当前: version.cpp 版本标识, api_abi_check.c C 兼容与对齐校验)
```

> 已落地: `api/` 两个纯 C 头 (agentxx 侧 `agentxx/plugin/api/plugin_api.h` 与
> `client_plugin_api.h` 作为 umbrella 引用它们 + 各自的领域表), 以及构建/安装/导出骨架。
> `kit/` `runtime/` `host/` 的搬迁进度见
> `resource/history/split-util-plugin-core/done.md`。

领域表归属 (由宿主定义与实现):

| 宿主 | 领域表 |
|---|---|
| agentxx (agent 侧) | tools / permission / hooks / session / model / prompt / resources / graph |
| agentxx (client 侧) | client.ui / client.events / client.session / client.wire / client.self |

## 构建与使用

```cmake
find_package(cxx_pluginxx REQUIRED)
target_link_libraries(your_target PRIVATE cxx_pluginxx_static)  # 或 cxx_pluginxx_shared
```

- 产物命名 (同 libagentxx): Release `libcxx_pluginxx.so` / `libcxx_pluginxx_static.a`,
  Debug 追加 `d` → `libcxx_pluginxxd.so` / `libcxx_pluginxx_staticd.a`
- 同一进程内需要单份框架实现 (宿主与插件共享状态) 时用动态变体

## 导出面

动态变体仅导出本库命名空间符号 (`*pluginxx*` / `*utilxx*`), ELF 上经 version script
白名单控制, MSVC 上仅导出本目标自有符号 —— 静态链入的第三方符号不外泄。
插件动态库的导出面由宿主构建侧另行约束 (仅 `agentxx_plugin_*` 入口)。
