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
  kit/    kit.h    插件侧 C++ SDK 通用部分 (header-only): PluginStringView/PluginString、
                   通用表聚合 PluginIfaceCore、Logger、Task<T> 锚定协程与锚定原语
                   (sleep/yield/offload/invoke_cap)、CancelRegistry/OpCtl/ArgReader、
                   后台任务 spawn、能力注册 capability、实例上下文基类
                   PluginBaseT<IfacesT>、通用导出宏 AGENTXX_PLUGIN_AGENT_EXPORT
          guard.h  C ABI 边界异常守卫 (logTo/reportCurrentException/guardCall/guardCallVoid)
  runtime/宿主侧运行时 (runtime/driver/instance_base/manager_base/op_driver)
  host/   loader.h (dlopen/LoadLibrary)、manifest.h (plugin.yaml/名称推导/拓扑排序)、
          abi_util.h (C 串转换/异常兜底/io 线程同步投递)、
          capability_registry.h (能力注册表: 能力名 → 提供者插件 + 启动/取消回调)、
          event_bus.h (事件后端抽象 EventSource + 订阅句柄 AgentxxPluginSubscription)、
          domain_hooks.h (领域钩子 DomainHooks: 通用表需要宿主数据的入口)、
          host_core.h (宿主核心 PluginHostCore<InstanceT>: 通用表状态与方法实现)、
          tables_impl.h (十张通用表的 vtable 入口 + queryGenericPluginIface<I,M>)、
          lifecycle.h (宿主生命周期骨架 PluginHostLifecycle<InstanceT>: 装载/启停/
                   禁用启用/卸载/级联依赖; 宿主经少量接缝注入领域动作)
src/      对应实现 (version.cpp / loader.cpp / manifest.cpp / capability_registry.cpp /
          api_abi_check.c C 兼容与对齐校验)
```

## 宿主接入形态

```cpp
class MyInstance : public pluginxx::PluginInstanceBase {
    const char* pluginDestroySymbol() const noexcept override { return "..._destroy"; }
};

class MyManager : public pluginxx::PluginHostLifecycle<MyInstance>,   // 含 PluginHostCore
                  public std::enable_shared_from_this<MyManager>,
                  public pluginxx::DomainHooks {
public:
    MyManager(asio::any_io_executor ex) : PluginHostLifecycle<MyInstance>(ex) { setDomainHooks(this); }
    // DomainHooks: 事件后端 / 工作线程池 / 配置 JSON / 语言 / 会话工作目录 / 取消状态 / 插件清单
protected:
    // 生命周期接缝 (纯虚): 管理器自引用 / 生成领域实例 / 交给自己插件的 vtable
    std::shared_ptr<pluginxx::PluginHostLifecycle<MyInstance>> selfRef() override;
    std::shared_ptr<MyInstance> createInstance(std::string name) override;
    const AgentxxHostVtable*    hostVtable() override;
    // 生命周期接缝 (可选): 领域注册摘除与清空 / 清单资源应用与释放 / 启停状态通知 /
    // 装载卸载收尾 / 卸载级联口径 / 日志前缀
    void detachDomainRegistrations(MyInstance* inst) override;
};
```

- **通用表 (定义与实现都在本库)**: log / json / config / plugins / events / scheduler /
  coroutine_runtime / tasks / cancel / capabilities —— 宿主经 `DomainHooks` 提供领域数据；
- **生命周期骨架 (在本库)**: 动态库装载与入口校验、create/start 事务、启用与禁用级联、
  stop 事务补齐、inflight 归零等待、destroy 与动态库卸载、失败回滚、关闭超时重试；
  宿主只提供少量接缝 (见上)，因此新宿主不必重写这套纪律；
- **领域表 (宿主自己实现)**: agentxx 的 tools/permission/hooks/session/model/prompt/
  resources/graph 与 client 侧的 ui/events/session/wire/self 表；
- `query_interface` 里先调 `pluginxx::queryGenericPluginIface<InstanceT, ManagerT>(iid)`
  取通用表，未命中再分发宿主的领域表 (agentxx 侧见
  `agent/lib/src/plugins/plugin_manager_vtable.cpp`)。

> 已落地: `api/` 两个纯 C 头、`kit/` (插件 SDK 通用部分 + 边界守卫)、
> `runtime/` (实例状态机/执行 lease/协程驱动/Operation 驱动器/管理器基类)、
> `host/` (装载/清单/ABI 辅助/能力注册表/事件后端/领域钩子/宿主核心/通用表入口/
> 生命周期骨架) —— agentxx 侧的 `agentxx/plugin/api/plugin_kit.h` 与 `plugin_guard.h`
> 作为 umbrella 引用它们 + 各自的领域 helper (工具/钩子/图节点/权限/client UI)，
> 插件源码无需改动; agent 侧 `PluginManager` 与 client 侧 `ClientPluginManager`
> 都已接入 `PluginHostLifecycle` (client 侧仅"装载"保留自己的实现，因为 dlopen 卸载到
> 内部线程池、接口协商限制与双端入口探测属 client 特有语义)。

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
