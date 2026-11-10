# 设计方案: cxx_pluginxx 代次 5 重构（功能埋点 + 广播 + 网络能力）

> 任务目录：`resource/history/plugin-refactor-4/`
> 实施进度与偏差记录见同目录 `work.md`；本文件是设计与实施依据。
> 本轮**不写代码**，只给出可执行的方案。

## 1. 背景与目标

### 1.1 触发变更

`agent/third_party/cxx_pluginxx` 从仓库记录的 `fcb0834` 更新到 `e83f325`（版本 0.2.0），
期间 21 个文件、约 3400 行改动。官方变更点（`CHANGELOG.md` + `README.md`）：

| 变更 | 对本项目意味着什么 |
|---|---|
| **应用名拼接 IID**：新增 `pluginxx/api/app.h`，10 张通用表 IID 由字面量 `pluginxx.<表名>` 改为 `PLUGINXX_APP_ID("<表名>")`（构建期 `PLUGINXX_APP_NAME`，默认 `pluginxx`） | 宿主与插件必须用同一个应用名编译，否则查不到表；本项目固定为 `agentxx` |
| **官方标识前缀可配**：`PluginHostCore::setOfficialPrefix()`，库内不再有具体项目名 | 官方能力 / 广播 id 的判定改由前缀；与 `naming.h` 现有 `agentxx.` 规则一致 |
| **事件表换成广播表**：删除 `pluginxx.events` v3，新增 `pluginxx.broadcasts` v1/v2（频道声明 + 缓存 + 发布权限 + 订阅频率） | 插件面所有"发布/订阅"改走广播 |
| **能力表改多实现行**：`pluginxx.capabilities` v4/v5 —— 声明（`impls`/`merge`/`phases`/`timeoutMs`）+ 实现行（阶段/优先级/同步异步/取消），`invoke_capability` 走 before→impl→after 链并按 JSON 补丁合并 | 钩子与功能点可以合并到同一套机制 |
| **宿主侧缺口补齐**：`CapabilityRegistry`、`BroadcastRegistry`、`capability_chain.h`、`onCapabilityRowAdded/Removed`、`appCapabilityDecl`、`onAppCapabilityStart/Cancel` | 主程序侧注册表、撤销、派发骨架不必自己写 |
| **业务数据入口改名**：`DomainHooks` → `DomainServices`，删掉 `eventSource()` | 已有实现改文件名与接口；事件后端不再由主程序接管 |
| **内置网络能力**：新增 `<应用名>.net` 表（`api/net.h` + `net/ops.h`），实现在 `cxx_utilxx` 的 `utilxx::PluginNetHub` | 新能力要接到宿主与插件实例上（框架只给 ABI） |
| **入口符号前缀化**：`PLUGINXX_EXPORT_PLUGIN(SymbolPrefix, ...)`；`PluginEntrySymbols` 由主程序经 `entrySymbols()` 交出 | 本项目已是此形态，只需确认 |
| **内置插件提供者**：`setBuiltinPluginProvider()` / `BuiltinPluginProvider` | 已实现（`plugins/builtin_plugins.cpp.in`），无需改 |
| 连带：`cxx_utilxx` 现在 `find_package(cxx_pluginxx)` 并链接 `cxx_pluginxx_static` | superbuild 顺序要改：`cxx_pluginxx_repo` 必须早于 `cxx_utilxx_repo` |

### 1.2 本轮范围（按用户确认的七点）

1. **可以改 `cxx_*` 库**：通用能力直接做进 `cxx_pluginxx`（必要时 `cxx_utilxx`），供其它主程序复用，
   本项目不再用"主程序侧绕开"的办法重复实现（§3.11 是库改动清单）。
2. **钩子 + 功能点彻底统一成"能力"**：其中作为扩展点的那部分统一叫 **功能埋点**；
   一个功能埋点 = 一个 id（能力行 + 同名广播频道），插件**两种参与方式**：
   - **实现**：登记一条能力实现行（可返回结果/补丁）；
   - **订阅**：订阅同名广播（只看不问，事后通知）。
3. **插件自定义能力**用 `plugin.<插件 id>.*` 前缀即可，不再需要额外的官方 id 机制。
4. **`cxx_pluginxx` 直接支持"结束"语义**：广播可以显式 `end`，订阅方收到结束回调，
   `mode=all` 的等待据此收尾（替代早期设计的 `{"$end":true}` 约定值）。
5. **内置插件的网络调用必须改走 `agentxx.net`**（chatbot / websearch / rag_search /
   text_selection_monitor 四处），需要的能力缺口补进 `PluginNetHub`。
6. **client 侧也提供 `agentxx.net`**。
7. **`agentxx.agent.mount` 表保留**。

### 1.3 目标

- agent 侧：21 张表（11 通用 = 框架 10 张 + `net`；10 业务）；
  client 侧：6 张自有表 + 11 张通用表；
- 插件扩展面只有三个概念：**功能埋点**（=能力+广播）、**广播**（纯通知/跨端数据）、
  **领域表**（工具/权限/会话/上下文/模型/提示词/资源/图/挂载/运行时）；
- `AGENTXX_PLUGIN_API_VERSION = 5`；库侧 `capabilities` 升 v6、`broadcasts` 升 v3（都只追加成员）。

### 1.4 非目标

- 不改 Wire 协议、TUI 布局、会话存储格式、模型调用链路；
- 不保留旧插件二进制/旧 IID/旧宏别名（见 §5）；
- 不把工具、权限、会话等**领域数据**改造成能力（需要 schema 与强类型查询）；
- 不把 `cxx_pluginxx` 做成"什么都懂"（功能埋点是通用原语，埋点清单仍属各主程序）。

## 2. 现状调查（文件与函数一级）

### 2.1 依赖与构建

| 位置 | 现状 | 影响 |
|---|---|---|
| `agent/CMakeLists.txt` 约 629 行 | `AGENTXX_PLUGIN_API_VERSION "4"`，追加 `-DPLUGINXX_API_VERSION/​_MIN_API_VERSION` | 抬到 5；先剔旧值再追加（现有逻辑已处理） |
| `agent/CMakeLists.txt` `_AGENTXX_COMMON_CMAKE_ARGS` | 没有 `PLUGINXX_APP_NAME` | 必须新增（走全局 FLAGS：插件只包含头、不链接框架目标） |
| `agent/CMakeLists.txt` `cxx_utilxx_repo` | `DEPENDS cxx_utilxx_base_repo fmt_repo html2md_repo` | 补 `cxx_pluginxx_repo`（新版 `cxx_utilxx` 依赖它） |
| `agent/CMakeLists.txt` `cxx_pluginxx_repo` | 只传 API 代次 | 补 `-DPLUGINXX_APP_NAME=agentxx` |
| `agent/plugins/CMakeLists.txt` 约 288-298 行 | `link_libraries(cxx_pluginxx_ui_static)`，插件只拿 include | 应用名靠全局 FLAGS；独立构建插件目录也要有同一份 |
| `agent/plugins/*/CMakeLists.txt` | 内置模式 per-source 宏改名 `agentxx_plugin_agent_*` → `..._builtin_*_<name>` | 新导出宏仍是"前缀 + 后缀"，机制不变 |

### 2.2 主程序侧（agent）

| 文件 | 关键函数 / 结构 | 现状 | 新版框架下的处置 |
|---|---|---|---|
| `lib/include/agentxx/plugin/plugin_framework.h` | 29-31 行 include `host/domain_hooks.h`、`host/event_bus.h` | **这两个头已删除/改名 → 现在编不过** | 换 `domain_services.h` + `broadcast_registry.h` + `api/net.h`，补 using |
| `plugin_manager.h` | `kInterfaceTableCount`、`eventSource()`、`subscribe()`/`publish()`、`handlersOf()`、`hooksJson()`、`listFeaturePoints()`、`registrationInventory()` | 面向旧事件表/钩子表/功能点表 | §4.5~§4.7 |
| `lib/src/plugins/plugin_manager_domain_hooks.cpp`（333 行） | `eventSource` / `qualifyEventTopic` / `subscribe` / `postToWorkerThread` / `configJson` / `language` / `pluginsJson` / `pluginJson` / `sessionWorkDir` / `isSessionCanceled` / `toolPromptJson` | 实现 `DomainHooks` | 改名 `plugin_manager_domain_services.cpp`，删事件入口 |
| `plugin_manager_vtable.cpp`（3071 行） | `xx_query_interface`、`g_ifaceHooks`、`g_ifaceFeature`、`g_ifaceEvent`、`xx_register_hook*`、`xx_feature_*`、`xx_event_*` | 13 张业务表的 C 入口 | 删 3 张表；新增 `net` 分支 |
| `plugin_manager_hooks.cpp`（617 行） | `PluginMount::hookHandlers_`、`orderedHandlers`、`insertHookHandler`、`PluginHookDispatchHandle::on*StartFunc` | 自有钩子注册表 + 单派发器 | 注册表删除；派发器改成"发埋点"（§4.7） |
| `plugin_manager_feature.cpp`（444 行） | `registerFeatureImpl` / `defineFeaturePoint` / `listFeaturePoints` | 自有功能点注册表（每挂载） | 删除；取值缓存与重入保护留在 `agentxx::feature` |
| `plugin_manager_capability.cpp`（126 行） | `PluginMount::callToolAsync` | 工具表 `call_tool_async` | 不变 |
| `plugin_manager_lifecycle.cpp`（1192 行） | `loadBuiltinAsync`、`onInstanceLoaded`（找 `agentxx_plugin_agent_mount/unmount`）、`detachDomainRegistrations` | 已接 `PluginHostLifecycle` | 改为清本地记账 + 框架按 scope 撤行 |
| `plugin_manager_runtime*.cpp`（约 2000 行） | 根/通道/触发器/定时器/后台任务 | `emit_event` 直接 `EventHub::publishValue` | 改"广播发布 + 中枢记录"（§4.8） |
| `plugin_mount.h/.cpp`（589/594 行） | `Entry{hookHandles, featurePoints, featureImpls, promptKeys, graphNodes, permissionTools, tools}` | 每挂载注册记账 | 删前三列，新增广播订阅记账 |
| `plugin_interfaces.h`（166 行） | `AgentEvents = PLUGINXX_IFACE_EVENTS` | **符号不存在 → 编不过** | 换 `Broadcasts` / `Net` |
| `lib/src/event/hub.cpp`（1636 行） | `attachBus`（监听 `plugin.` 前缀）、`publishValue`、`publishValueEx` | 中枢传输走 `EventBus` | 传输改广播；结束用广播 end（§3.6） |
| `lib/include/agentxx/feature/*`（约 1600 行） | `Registry`、`ProvidePoint<TReq,TValue>::ask/call`、`value_cache.h` | 自有注册表 + 取值缓存 | 注册表删除；`ask/call` 走功能埋点 |

### 2.3 主程序侧（client）

| 位置 | 现状 |
|---|---|
| `client_plugin_manager.h`（1079 行） | `kInterfaceTableCount = 7 + 2`；已接 `PluginHostLifecycle` |
| `client_plugin_manager.cpp`（4090 行） | 只分发 9 张自有表 + 自实现 `coroutine_runtime`；`agentxx.client.json`/`log`/`events` 是框架通用表的重复实现；`WirePluginData` 落点 |
| 客户端事件 | 8 个 `AGENTXX_CLIENT_EVT_*`，经 `agentxx.client.events` 订阅；`PluginBase::onHostReady` 用它订阅 `agentxx.agent.round.start` |

### 2.4 SDK 与接口表

| 位置 | 现状 |
|---|---|
| `plugin_kit.h`（4638 行） | `AgentIfaces`（23 成员）/ `ClientIfaces`（9）；`PluginBase : PluginBaseT<AgentIfaces>`；工具/权限/运行时/后台任务/图节点/渲染器/钩子（`hook/hookEx/unhook/listHooks`）/功能点（`defineFeaturePoint/provideFeature/callFeature`）/事件中枢（`defineEvent/...`） |
| `plugin_api.h`（1365 行） | 13 张业务表 + 结构体；`AgentxxPluginHookSpec/Ex`、`AgentxxPluginFeaturePointSpec/ImplSpec`、`AgentxxPluginEventSpec/WaitSpec` 只服务于被删的三张表 |
| `client_plugin_api.h`（627 行） | 9 张 client 表；`Events/Json/Log` 三张与框架重复 |
| `plugin_iface_meta.h`（185 行） | 23 张表的 `IfaceMeta` 特化（`boundaries` 逐表核对） |

### 2.5 插件（17 内置 + 9 示例）

| 类别 | 插件 | 用到的面 |
|---|---|---|
| 纯工具 | `agentxx_math`、`agentxx_string`、`agentxx_system`、`agentxx_filesystem`、`agentxx_execute_command` | tools / permission |
| 通知 | `agentxx_codegraph`、`agentxx_chatbot`、`agentxx_audio_stream`、`agentxx_screen_capture`、`agentxx_text_selection_monitor`、`agentxx_system_monitor` | `iface.events->publish/subscribe`、`iface.capabilities->*` |
| 运行时 | `agentxx_chatbot`（40 文件 14.5K 行） | `mountRoot` / `openChannel` / `addTrigger` / `submitAsyncWork` / 广播 |
| JS 引擎 | `agentxx_javascript_engine`（2693 行） | `register_capability_ex("interpreter.js")`、脚本侧 `emit_event/on_event` |
| 网络（本轮必改，§4.9） | `agentxx_chatbot`（HttpClient + WsClient）、`agentxx_websearch`（HttpClient + fetchMarkdown）、`agentxx_rag_search`（HttpClient POST）、`agentxx_text_selection_monitor`（手写 WS 握手连本地 CDP） | 见下表 |
| 示例 | `example_plugin`、`example_event`、`example_feature`、`example_graph_node`、`example_resources`、`example_runtime`、`example_multi_mount`、`example_js`、`example_js_execute_command` | 覆盖钩子/功能点/事件/能力/资源/运行时全部面 |

网络调用清点（`#include "utilxx/..."` 与手写 socket）：

| 插件 / 文件 | 现有网络调用 | 迁移到 |
|---|---|---|
| `agentxx_websearch/websearch_impl.h` | `HttpClient::getAsync/postAsync/fetchMarkdown`（GET/POST，带 header、超时） | `net.fetch`（需要 `format: markdown`，§3.11 L5） |
| `agentxx_rag_search/rag_search_impl.h` | `HttpClient::postAsync`（JSON body，15s 块超时） | `net.fetch`（`json` + `timeoutMs`） |
| `agentxx_chatbot/platform/qq/qq_api.cpp`、`qq_platform.cpp` | `HttpClient::requestAsync/getAsync`（重试、自定义头、URL 编码） | `net.fetch` / `net.request` |
| `agentxx_chatbot/chatbot_engine.cpp` | `HttpClient`（附件下载落盘） | `net.download`（落 `<dataDir>/<插件>/data/downloads/`） |
| `agentxx_chatbot/platform/qq/qq_gateway.cpp` | `utilxx::WsClient`（Hello/Identify/心跳/Resume/重连） | `net.wsOpen/Send/Ping/Close` + `ws.*` 事件 |
| `agentxx_text_selection_monitor` | 手写 WS 握手与帧解析，连本地 CDP 端口 | `net.wsOpen`（localhost + path + 头）+ `ws.message` |

真实插件里 `hookEx` / `provideFeature` / `defineEvent` 各只有 `example_feature` / `example_event` 在用；
事件 publish/subscribe 有 28 处（10 个插件文件）——**迁移主战场是广播与网络**。

### 2.6 测试与文档

- 插件相关测试模块约 20 个（`plugins`、`plugin_sdk`、`plugin_runtime`、`plugin_host_mount`、
  `plugin_multi_mount`、`plugin_multi_instance`、`plugin_resources`、`plugin_cleanup`、
  `plugin_hooks`、`plugin_feature`、`plugin_event`、`plugin_bridge`、`plugin_agent_runtime`、
  `plugin_example_*`、`client_plugins`、`chatbot_task`、`codegraph_tools`、`cpu_gpu_use`）；
- `test/core/test_boundaries.cpp` 规则 7 与"四件套 + 成员顺序台账"逐表核对
  （常量分别是 `PluginManager::kInterfaceTableCount` / `ClientPluginManager::kInterfaceTableCount`）；
- 文档：`docs/zh-cn/design/plugins.md`（2300 行，§8/§9/§15~§21）、`index.md`、根 `AGENTS.md`、
  `agent/plugins/AGENTS.md`、`roadmap.md`；
- **两个库都没有自带测试用例**（`cxx_pluginxx` 只有 `src/api_abi_check.c`，`cxx_utilxx` 无），
  库改动的验证只能靠本项目的测试模块 + 新增的编译期/运行期自检（§7）。

### 2.7 新框架已提供 vs 已删除

| 旧（现在用的） | 新（0.2.0） |
|---|---|
| `pluginxx.events`（subscribe/publish） | `<app>.broadcasts`（declare/publish/subscribe/read/unsubscribe） |
| `pluginxx.capabilities` v3（`register_capability(host, spec)`、`has_capability`、`invoke_capability_async`） | `<app>.capabilities` v5（`PluginxxCapabilitySpec` + 声明/行、`invoke_capability(_ex)`、`capability_info`、`list_capabilities`） |
| `DomainHooks`（含 `eventSource`） | `DomainServices`（`qualifyEventTopic`/`pluginId`/`postToWorkerThread`/config/language/plugins） |
| `pluginxx/host/event_bus.h` 的 `EventSource` | 已删除；`BroadcastRegistry` 自带频道与订阅 |
| `version != 1` 式判定 | `IfaceMeta` + `ifaceHas` + `MIN_VERSION`/`BASELINE_SIZE` |
| 主程序自写通用表 vtable | `queryGenericPluginIface<InstanceT, ManagerT>(iid)` |
| 无网络能力 | `<app>.net` + `utilxx::PluginNetHub` |

## 3. 目标架构

### 3.1 概念模型：功能埋点 + 广播 + 领域表

| 概念 | 是什么 | 谁实现 | 本项目用途 |
|---|---|---|---|
| **功能埋点**（point） | 一个 id，同时是**能力**与**同名广播频道**：核心走到这个位置时（a）调用能力链拿结果/补丁，（b）发一条同名广播 | `cxx_pluginxx` 提供原语（声明 / 发 / 结束 + 能力行带 scope + 宿主发起调用）；本项目提供**埋点清单**与调用点 | 原钩子点（7 个）+ 原功能点（3 个）合并成 10 个埋点 |
| **广播**（broadcast） | "某频道的值变了"，带缓存 / 发布权限 / 订阅限频 / **显式结束** | 框架 `BroadcastRegistry` + `<app>.broadcasts` 表 | 插件间通知、跨端插件数据、内建事件镜像、`plugin.<id>.net` |
| **能力**（capability） | "某个 id 被调用，可有多个实现行，按阶段/优先级排链，结果按 JSON 补丁合并" | 框架 `CapabilityRegistry` + `<app>.capabilities` 表 | 功能埋点的实现行；插件间能力（JS 解释器）；插件自定义能力 |
| **领域表** | 带 schema 的对象注册或强类型查询 | 本项目 | 工具 / 权限 / 会话 / 上下文 / 模型 / 提示词 / 资源 / 图 / 挂载 / 运行时 |

一条功能埋点的两种参与方式（插件自己选，也可都选）：

```
核心走到埋点 P
   ├─ (a) 能力链: 取 P 的实现行（按 scope 过滤、按 priority 排序）→ 结果/补丁（"我想影响它"）
   └─ (b) 广播: 往频道 P 发一条载荷（带 agentId / sessionId 等）→ 订阅者收到（"我只想知道"）
```

- "实现"适合需要返回值的埋点（`context.countTokens` / `context.summarize` /
  `tool.summarizeOutput`）；
- "订阅"适合纯通知埋点（`turn.start` / `model.run` / `tool.end` …），也是原钩子的等价物；
- 纯通知埋点同样允许登记实现行（结果忽略），保留"以后加返回值"的余地；
- 取值型埋点也允许只订阅（不影响结果）。

被删除的自有机制（连同接口表）：

| 删掉 | 替代 |
|---|---|
| `agentxx.agent.hooks` 表 + `PluginMount::hookHandlers_` + `hooksJson` | 功能埋点（实现行 / 订阅） |
| `agentxx.agent.feature` 表 + `PluginMount` 的功能点注册表 | 功能埋点（能力行 + 取值缓存留在 `agentxx::feature`） |
| `agentxx.agent.event` 表（9 个成员） | 广播 + `end`；中枢保留为内部"记录 + 等待 + 投递" |
| `agentxx.client.events` 表（8 个事件） | 广播频道（`agentxx.client.*`）+ `end` |
| `agentxx.client.json` / `agentxx.client.log` | 框架通用表 `<app>.json` / `<app>.log` |

### 3.2 作用域：能力行带 scope（库改动 L1）

框架现状：`register_capability(host, scope, spec, err)` 收了 `scope` 但内部 `(void)scope`，
实现行不记作用域。本项目的钩子/埋点必须按 agent 隔离，因此**改库**（通用能力，其它主程序同样需要）：

- `CapabilityProvider::scopeId`：登记时从 `PluginxxScopeRef` 拷贝（空 = 进程级）；
- 覆盖判定改为 `(provider, scopeId, ownerTag, phase)`；`impls:one` 的 impl 行按 scope 各一份；
- 行查询与调用按 scope 过滤：`scopeId` 为空的行对所有 scope 生效，
  非空的行只在"调用方给的 scope 相同"时参与；
- 卸载一个 scope（agent 挂载拆除）时，宿主调 `CapabilityRegistry::removeScope(agentId)` 一次清完。

于是派发规则是：

| id 前缀 | 谁声明 | 调用/派发时的行 |
|---|---|---|
| `agentxx.agent.point.*`（应用埋点） | 应用声明表 | 只跑 `scopeId == 当前 agentId` 或空 scope 的行 |
| `plugin.<插件 id>.*`（插件能力） | 插件自己（短名由宿主补全） | 不过滤（进程级） |

### 3.3 接口表清单与 IID

应用名 `agentxx`（`PLUGINXX_APP_NAME`），业务表 IID 用 `PLUGINXX_APP_ID(...)` 拼出
（字符串与原字面量一致，保留的表不改版本）：

**通用表（11 张：框架 10 张 + 本项目实现的 `net`）**

| 表 | IID | 版本 | 说明 |
|---|---|---|---|
| log / json | `agentxx.log` / `agentxx.json` | 1 | 框架实现 |
| config | `agentxx.config` | 3 | 数据经 `DomainServices` |
| plugins | `agentxx.plugins` | 1 | 数据经 `DomainServices` |
| broadcasts | `agentxx.broadcasts` | **3**（库改动 L3 后） | 声明/发布/订阅/读/撤销 + **结束** |
| capabilities | `agentxx.capabilities` | **6**（库改动 L1 后） | 声明 + 行（带 scope）+ 调用 + 自省 |
| scheduler / coroutine_runtime / tasks | `agentxx.scheduler` 等 | 1 | 框架实现 |
| cancel | `agentxx.cancel` | 3 | 框架实现 |
| **net** | `agentxx.net` | 1 | 本项目实现，转发 `utilxx::PluginNetHub` |

**agent 业务表（10 张）**：tools(3) / permission(2) / session(3) / context(2) / model(2) /
prompt(3) / resources(2) / graph(2) / mount(1) / runtime(2) —— 成员与版本都不变，
只有 `runtime.emit_event` 的实现改走广播。

**client 业务表（6 张）**：ui / session / wire / self / timer / keybind（均 v1）。

**删除**：`agentxx.agent.hooks`(v2)、`agentxx.agent.feature`(v2)、`agentxx.agent.event`(v1)、
`agentxx.client.events`(v1)、`agentxx.client.json`(v1)、`agentxx.client.log`(v1)。

数量：agent 侧 23 → **21**；client 侧 9 → **6**（另有 11 张通用表）。
两个数字要同步到 `kInterfaceTableCount`、`boundaries` 规则 7、`plugins.md` §8/§9、`index.md`、根 `AGENTS.md`。

### 3.4 功能埋点清单（唯一真源）

id 规则：`agentxx.agent.point.<域>.<名字>`（能力 id 与广播频道 id 相同）。
插件自定义埋点：`plugin.<插件 id>.point.<名字>`（短名声明）。

| 埋点 id | 类型 | 载荷（广播） | 实现行结果 | 调用点 |
|---|---|---|---|---|
| `agentxx.agent.point.turn.start` | 通知 | `{agentId,sessionId,turnId}` | 忽略 | `BaseAgent` 轮次开始 |
| `agentxx.agent.point.turn.end` | 通知 | `{agentId,sessionId,turnId,hasError,interrupted}` | 忽略 | 轮次结束 |
| `agentxx.agent.point.model.start` | 通知 | `{agentId,sessionId,model}` | 忽略 | modelcall 中间件（调用前） |
| `agentxx.agent.point.model.run` | 通知 | `{agentId,sessionId,model,attempt}` | 忽略 | 单次尝试（重试多次触发） |
| `agentxx.agent.point.model.end` | 通知 | `{agentId,sessionId,model,usage}` | 忽略 | 调用结束 |
| `agentxx.agent.point.tool.start` | 通知 | `{agentId,sessionId,tool,callId}` | 忽略 | 工具分发开始 |
| `agentxx.agent.point.tool.end` | 通知 | `{agentId,sessionId,tool,callId,status,elapsedMs}` | 忽略 | 工具分发结束 |
| `agentxx.agent.point.context.countTokens` | 取值 | `{agentId,model,kind}` | `{tokens}` | token 估算（`agentxx::feature`） |
| `agentxx.agent.point.context.summarize` | 取值 | `{agentId,sessionId,model,targetTokens}` | `{summary,truncated}` | 压缩中间件 |
| `agentxx.agent.point.tool.summarizeOutput` | 取值 | `{agentId,tool,bytes}` | `{summary}` | 工具输出摘要 |

埋点声明（由宿主在启动期一次性登记，`declarePoint`）：

```text
impls = many, merge = first, phases = impl, timeout = 0（调用方按需再给）
broadcast: publish = owner(app), cache = latest + pushOnSubscribe（取值型）
                               cache = none（通知型，高频）
```

插件侧只读常量放在 `agentxx/plugin/api/agentxx_point_ids.h`（`inline constexpr`），
供插件与文档引用；`boundaries` 校验"头文件里的 id 清单 == 宿主声明表"。

### 3.5 广播频道清单

| 频道 | cache | publish | 谁发 | 说明 |
|---|---|---|---|---|
| `agentxx.agent.point.*`（§3.4 的 10 个） | 取值型 latest / 通知型 none | owner(app) | 宿主 | 功能埋点的通知面 |
| `agentxx.agent.round.start` | none | owner(app) | 宿主 | 轮次开始（SDK 自动订阅，清会话取消标记） |
| `agentxx.agent.async.done` | latest | owner(app) | 后台任务 | 结果值（`asyncId` 区分） |
| `agentxx.agent.host.client_attached` | none | owner(app) | 服务端端点 | 客户端接入（chatbot/planning 订阅） |
| `agentxx.client.ready` / `connection_state` / `session_switch` / `ui_layout` | latest | owner(app) | client | 状态类 |
| `agentxx.client.user_input` / `delta` / `turn_end` | none | owner(app) | client | 事件类 |
| `agentxx.client.plugin.<插件名>.<事件>` | none | **anyone** | 两端 | 跨端插件数据（上行/下行），按需声明 |
| `plugin.<插件 id>.net` | none | owner(该插件) | 宿主代发 | 网络事件 |
| `plugin.<插件 id>.*` | 插件自定 | owner（可 `anyone`） | 插件 | 插件自建频道 |

规则：

- 应用频道在启动期（`setDomainServices` 之后、插件加载之前）声明一次；
  带 `<…>` 的动态频道在首次发布前 `declareBroadcastJson`（幂等）；
- 插件订阅应用频道不需要声明；插件自建频道用短名声明；
- **结束**：属主可 `end`（库改动 L3），结束后该频道不再接受发布（`ERR_ENDED`），
  订阅方收到 `on_end`；再次 `declare` 同一 id 视为开一条新流（清空结束标记与缓存）。

### 3.6 事件中枢与广播

中枢（`agentxx/event/hub.*`）保留为"记录 + 等待 + 投递给会话"的内部机制，传输换成广播：

```text
内建事件源（turn/tool/timer/async）
   │ publishValue(name, value)
   ▼
EventHub ──记录(EventRecord)──► 等待(EventWait) ──► SessionInputSink（next-step/inject/next-turn）
   │
   ├─► BroadcastRegistry.publish(name, value)     ← 通知面（插件订阅）
   └─► BroadcastRegistry.end(name, reason)        ← 流结束（mode=all 的等待收尾）
```

- `attachBus(bus)` → `attachBroadcasts(registry)`；启动时按 §3.5 声明/订阅自己的频道；
- `publishValue` = 写记录 + 发广播；`endEvent` = 结束广播 + 结束记录 + 唤醒 `mode=all` 等待；
- **插件频道按名按需订阅**：`BroadcastRegistry` 只有"按 id 订阅"，没有前缀通配，所以中枢在
  第一次为某个名字登记等待（`EventWait`）时 `subscribe` 该频道，等待表里最后一个等待撤销时退订；
  收到值后按载荷里的 `agentId` 过滤（不是本 agent 就忽略）；
- 由此产生一个语义变化：**插件频道不再自动进中枢历史**（以前靠 `listenPrefix("plugin.")` 全收）；
  需要"当前值"就 `readBroadcast`（有 `cache: latest` 的频道），需要"每次通知"就自己订阅。
  `agentxx.agent.point.*` / `agentxx.agent.async.done` 这类宿主自己发的值不受影响（中枢本来就是写者）；
- 插件面**没有**中枢表：发值用 `publish_broadcast`，收值用 `subscribe_broadcast`（可带 `on_end`），
  要当前值用 `read_broadcast`；
- "谁在听"的判定从 `bus->hasListeners` 换成"频道有订阅者"（`BroadcastRegistry::listJson`）；
- `agentxx.agent.async.done` 仍是唯一落在 `agentxx.agent.event.` 段外的内置值事件（`naming.h` 不变）。

### 3.7 旧钩子 / 功能点 → 功能埋点（映射）

| 现状 | 改造后 |
|---|---|
| 表 `agentxx.agent.hooks`（5 成员）、`AgentxxPluginHookSpec/Ex` | 框架 `capabilities.register_capability`（实现行）或 `broadcasts.subscribe_broadcast`（订阅） |
| `AgentxxPluginHookPoint`（7 个枚举） | 7 个通知型埋点（§3.4） |
| 表 `agentxx.agent.feature`（7 成员）、`Registry`、`ProvidePoint` | 3 个取值型埋点；`ProvidePoint::ask/call` 改成调 `emitPoint` 并解析结果 |
| `PluginMount::hookHandlers_`、`orderedHandlers`、`*.hookHandles` 记账 | 删除（框架注册表按 scope 记账） |
| `PluginMount` 的功能点注册表、`featurePoints/featureImpls` | 删除 |
| `PluginHookDispatchHandle` | 保留中间件挂点，动作改成 `co_await manager->emitPoint(scope, id, payload)` |
| core 层钩子处理器（`addCoreHookHandler`） | 直接 C++ 调用（不再进框架表），顺序在插件行之后 |
| 取值缓存、重入 busy、失败=没意见 | 保留在 `agentxx::feature`（调用方语义，与注册表无关） |

### 3.8 网络能力（`agentxx.net`，两侧都提供）

```text
插件 ──C ABI──► PluginxxNetIface（本项目实现 g_ifaceNet）
                    │ call / call_async / cancel
                    ▼
              utilxx::PluginNetHub（每管理器一份，自有网络线程）
                    │ PublishFn → io 线程 → 先 declare("plugin.<id>.net") 再 publish
                    │ PostFn    → io 线程执行异步完成回调
                    │ DoneFn    → PluginxxOperatorNotify.done（恰好一次）
                    ▼
              HTTP（含 markdown 形态）/ TCP / WS / UDP / HTTP 服务端
```

- agent 侧 `PluginManager`、client 侧 `ClientPluginManager` 各一个，构造时
  `setDataDir(<data_dir>)` + `start()`，关闭时 `stop()`；
- 插件 id = `inst->name`（`DomainServices::pluginId` 不覆盖）；
  禁用/卸载 → `shutdownPlugin(id)` + 未完成回调按 `CANCELED` 结算；
- `plugin.<id>.net` 由宿主按需声明并代发事件（插件无需自己声明）；
- 插件侧 SDK 给 `netCall / netFetch / netDownload / netWsOpen / netWsSend / netServeHttp …`
  （高频三个给 `co_await` 版本，底层是 `call_async` + 锚定协程）。

**内置插件网络调用全部改走 `agentxx.net`**（清单见 §2.5），需要的缺口补进 `PluginNetHub`
（库改动 L5：`format: markdown`、`sslVerify`）。

### 3.9 SDK（插件侧）

- `AgentIfaces`：tools / permission / session / context / model / prompt / resources / graph /
  mount / runtime + log / json / config / plugins / broadcasts / capabilities / scheduler /
  coroutineRuntime / tasks / cancel / **net**（删 hooks / events / feature / event）；
- `ClientIfaces`：ui / session / wire / self / timer / keybind + 通用表 + net（删 events / json / log）；
- 功能埋点 API（新增，替代 hook*/feature*）：
  `declarePoint`（插件自定义埋点）、`providePoint`（登记实现）、`onPoint`（订阅，带 `onEnd`）、
  `emitPoint`、`endPoint`、`listPoints`；
- 广播 API：`declareBroadcast / publishBroadcast / subscribeBroadcast / readBroadcast /
  endBroadcast / unsubscribeBroadcast`；
- 能力 API：`capability`（登记，带 scope）、`invokeCapability / invokeCapabilityBlocking /
  capabilityInfo / listCapabilities`；
- 网络 API：`netCall / netFetch / netDownload / netWsOpen / ...`；
- 工具 / 权限 / 运行时 / 后台任务 / 图节点 / 渲染器 API 不变。

### 3.10 客户端侧

- `ClientPluginManager`：`PluginHostLifecycle`（已有）+ `DomainServices`（新）+
  自己的 `BroadcastRegistry`；`query_interface` 先 `queryGenericPluginIface`，再 6 张自有表 + `net`；
- 删掉自有 `json` / `log` / `coroutine_runtime`（改用框架）；
- 8 个 `AGENTXX_CLIENT_EVT_*` → `agentxx.client.*` 频道；`WirePluginData` 上下行改广播；
- `kInterfaceTableCount` = 11 + 6 = **17**；文档"9 张"改为"6 张自有 + 11 张通用"；
- 提供 `agentxx.net`（同一个 `PluginNetService`）。

### 3.11 对 `cxx_pluginxx` / `cxx_utilxx` 的改动（可复用，非本项目专用）

> 原则：只放通用内容（与"会话/工具/模型"无关）；每个改动都要能被另一个主程序直接用。
> 版本规则：追加成员 → 表 `VERSION +1`、`MIN_VERSION` 不变、`BASELINE_SIZE` 改为"第一个追加成员的偏移"；
> 成员签名不改、不重排、不删除。

#### L1 能力行带作用域（`capabilities` v5 → v6）

```cpp
// include/pluginxx/host/capability_registry.h
struct CapabilityProvider {
    ...
    std::string scopeId;   ///< 登记时的作用域（空 = 进程级）
};
// 覆盖键：(provider, scopeId, ownerTag, phase)
// 行查询：
std::vector<const CapabilityProvider*> rowsOf(std::string_view id, CapabilityPhase_e phase) const;             // 全部
std::vector<const CapabilityProvider*> rowsOf(std::string_view id, CapabilityPhase_e phase,
                                             std::string_view scopeId) const;   // scope 过滤（空行 + 同行）
size_t removeScope(std::string_view scopeId);      // 挂载拆除时一次清完
```

- `PluginHostCore::registerCapability`：把 `scope->id` 存进 `row.scopeId`（校验 `pluginxx_scope_ref_ok`）；
- `invokeCapabilityAsync`：用调用方给的 scope 过滤行（`scope == nullptr` = 不过滤）；
- 自省 JSON（`capability_info` / `list_capabilities`）每行带 `scope`；
- ABI 表尾追加：

```c
// 注销一条实现行（带作用域；旧的 unregister_capability 仍是"该 provider+ownerTag 的所有行"）
int32_t (PLUGINXX_CALL* unregister_capability_ex)(
    const PluginxxHost* host, const PluginxxScopeRef* scope,
    const PluginxxStringView* capability_id, const PluginxxStringView* owner_tag);
```

#### L2 宿主（应用）发起能力调用

```cpp
// runtime/op_driver.h：没有实例租约的外层操作（应用侧发起的调用）
static std::shared_ptr<OpCore> createDetached(std::shared_ptr<PluginRuntime> runtime,
                                              std::string label);

// host/host_core.h：caller 允许为空（宿主自己发起）
PluginxxOperatorHandle* invokeCapabilityAsync(
    InstanceT* caller /*可为 nullptr*/,
    const PluginxxScopeRef* scope, std::string_view capability, std::string_view argsJson,
    int32_t phase, int32_t timeoutMs, PluginxxOperatorCallback cb, void* ud,
    PluginxxString* error_out);
```

- `caller == nullptr` 时外层 `OpCore` 用 `createDetached`（不占实例租约，仍可取消/完成）；
  每一行仍然各自持有提供者的 lease；
- 不改 ABI（插件侧调用形状不变），只是 C++ 层放宽。

#### L3 广播结束语义（`broadcasts` v2 → v3）

```cpp
// host/broadcast_registry.h
bool end(std::string_view id, std::string_view reasonJson, std::string_view writer,
         std::string* error = nullptr);        // 属主/应用可结束；返回是否有该频道
// 订阅：BroadcastSub 增加 std::function<void(std::string_view)> onEnd
//（现有 subscribe(id, owner, handler, options, error) 保留，另加一个带 onEnd 的重载）
// 结束后：publish → ERR_ENDED，read → ERR_ENDED，订阅清空（onEnd 恰好一次）
// 再次 declare 同 id = 开新流（清结束标记与缓存）
```

```c
#define PLUGINXX_RESULT_ERR_ENDED (-5)   ///< 频道已结束
// 表尾追加：
int32_t (PLUGINXX_CALL* end_broadcast)(const PluginxxHost*, const PluginxxStringView* id,
                                       const PluginxxStringView* reason_json, PluginxxString* err);
PluginxxSubscription* (PLUGINXX_CALL* subscribe_broadcast_ex)(
    const PluginxxHost*, const PluginxxScopeRef*, const PluginxxStringView* id,
    void (PLUGINXX_CALL* handler)(const PluginxxStringView*, void*),
    void (PLUGINXX_CALL* on_end)(const PluginxxStringView* reason, void*),
    void* ud, const PluginxxStringView* options_json, PluginxxString* err);
```

#### L4 "功能埋点"原语（`PluginHostCore` + kit）

```cpp
// host/host_core.h
struct PluginPointDecl {
    std::string      id;               // 全名（应用）或短名（插件）
    bool             impl   = true;    // 开放实现行（capabilities: impls=many, merge=first）
    bool             notify = true;    // 同名广播频道
    BroadcastCache_e cache  = BroadcastCache_e::Latest;
    bool             pushOnSubscribe = false;
    int32_t          timeoutMs = 0;
};
bool declarePoint(const PluginPointDecl& decl, std::string* error = nullptr);
/// 发一次埋点：先发广播，再按 scope 调实现链；完成后回调恰好一次（空实现 → OK + "null"）
PluginxxOperatorHandle* emitPointAsync(
    const PluginxxScopeRef* scope, std::string_view pointId, std::string_view payloadJson,
    int32_t timeoutMs, PluginxxOperatorCallback cb, void* ud, PluginxxString* error_out);
bool endPoint(std::string_view pointId, std::string_view reasonJson, std::string* error = nullptr);
```

```cpp
// kit/kit.h：插件侧
template<typename Ctx, typename Fn> void point(Ctx&, const ScopeRef&, std::string_view id, Fn&&);   // 实现
template<typename Ctx, typename Fn> PluginxxSubscription* onPoint(Ctx&, const ScopeRef&,           // 订阅
        std::string_view id, Fn&&, FnEnd&& onEnd = {});
template<typename Ctx> std::string emitPoint(Ctx&, std::string_view id, std::string_view payload);
```

- `emitPointAsync` 的 scope 过滤依赖 L1；广播是进程级的，**订阅侧的按 agent 过滤由 kit 的
  `onPoint` 做一行判断**（载荷里有 `agentId`，与订阅时的 scope id 不同就跳过）。

#### L5 `PluginNetHub` 补齐（`cxx_utilxx`）

| 缺口 | 改动 |
|---|---|
| `fetchMarkdown`（HTML→Markdown） | `http.request` / `http.fetch` 新增 `format: "markdown"`（内部走 `HttpClient::fetchMarkdown`，结果里 `body` 为 Markdown、`format` 字段回填） |
| 自签名 / 关闭校验 | http 参数新增 `sslVerify: bool`（映射 `RequestConfig::sslVerify`） |
| （可选）`http.request` 同步形态 | 不加；同步请求会阻塞框架线程，插件用 `call_async` + 锚定协程 |

#### L6 库的构建与文档

- `cxx_pluginxx`：`README.md`/`CHANGELOG.md` 记代次 5 的能力（scope 行、宿主发起、广播结束、埋点原语）；
  `src/api_abi_check.c` 加新增成员的静态断言；
- `cxx_utilxx`：`plugin_net_hub.h` 的 op 表补充 `format` / `sslVerify`；
- 本项目侧：两个子模块指针指向新提交，并在 `plan.md`/`work.md` 记录库侧提交号。

#### L7 库侧验证

- 两个库目前**没有自带测试**，改动靠：本项目 `agentxx_test` 的插件模块 + 新增编译期断言；
- 建议在库仓库补最小自测（`capability_registry` 的 scope 行、`broadcast_registry` 的 end、
  `emitPointAsync` 的空实现/多行/取消）；若库仓库暂无测试工程，退化为本项目测试覆盖并在库
  `CHANGELOG` 里注明。

## 4. 详细设计（按模块）

### 4.1 构建与依赖

1. **先改库**（§3.11），在两个库仓库提交，把 `agent/third_party/cxx_pluginxx`、
   `agent/third_party/cxx_utilxx` 的子模块指针指到新提交；
2. `agent/CMakeLists.txt`：
   - `AGENTXX_PLUGIN_API_VERSION` 4 → **5**；
   - 全局 FLAGS 增加 `-DPLUGINXX_APP_NAME=agentxx`（插件只包含头，拿不到 target 的 PUBLIC 定义），
     `cxx_pluginxx_repo` 的 `CMAKE_ARGS` 同时传 `-DPLUGINXX_APP_NAME=agentxx`；
   - `cxx_utilxx_repo` 的 `DEPENDS` 补 `cxx_pluginxx_repo`；
3. `agent/plugins/CMakeLists.txt` 增加一致性检查：`PLUGINXX_APP_NAME` 不是 `agentxx` 时
   `FATAL_ERROR`（独立构建插件目录同样要拦）；
4. `agent/script/check_plugin_exports.sh` 白名单不变（入口符号名未变）。

### 4.2 `plugin/api/plugin_api.h`

- IID 宏值改成 `PLUGINXX_APP_ID("agent.tools")` 形式（值不变）；
- **删除**：`AgentxxPluginHookPoint`、`AgentxxPluginHookSpec/Ex`、`AgentxxPluginHooksIface`、
  `AgentxxPluginFeaturePointSpec/ImplSpec`、`AgentxxPluginFeatureIface`、
  `AgentxxPluginEventSpec/WaitSpec`、`AgentxxPluginEventIface` 及对应错误码；
- 保留结构体不动（tools / permission / session / context / model / prompt / resources / graph /
  mount / runtime）；
- 新增 `agentxx_point_ids.h`（§3.4 的 10 个埋点 id 常量）与 `agentxx_broadcast_ids.h`
  （§3.5 的频道常量），均为纯常量头。

### 4.3 `plugin_iface_meta.h` / `plugin_interfaces.h` / `plugin_spec.h`

- `plugin_iface_meta.h`：删 6 条特化（hooks/feature/event/client-events/client-json/client-log）；
- `plugin_interfaces.h`：`AgentEvents` → `AgentBroadcasts`（`PLUGINXX_IFACE_BROADCASTS`）、
  新增 `AgentNet`；删 `AgentHooks` / `AgentFeature`；client 侧删 `ClientEvents`，
  新增 `ClientBroadcasts` / `ClientNet`；
- `plugin_spec.h` 只保留仍存活的结构体。

### 4.4 `plugin_framework.h`

```cpp
#include "pluginxx/api/net.h"
#include "pluginxx/host/broadcast_registry.h"
#include "pluginxx/host/capability_chain.h"
#include "pluginxx/host/domain_services.h"     // 替代 domain_hooks.h
// 删掉 "pluginxx/host/event_bus.h"

using pluginxx::BroadcastDecl;
using pluginxx::BroadcastRegistry;
using pluginxx::CapabilityDecl;
using pluginxx::CapabilityProvider;
using pluginxx::DomainServices;               // 替代 DomainHooks
using pluginxx::PluginPointDecl;              // 库改动 L4
using pluginxx::PluginxxNetIface;
using pluginxx::unsubscribePluginBroadcast;
```

`DomainHooks` 的旧 using 删除；`unsubscribePluginSubscription` 换成 `unsubscribePluginBroadcast`。

### 4.5 `PluginManager`（agent 侧）

```cpp
class PluginManager : public pluginxx::PluginHostLifecycle<PluginInstance>,
                      public pluginxx::DomainServices,
                      public std::enable_shared_from_this<PluginManager>
```

- 构造末尾：`setDomainServices(this); setOfficialPrefix("agentxx.");`
  然后（插件加载之前）`declareAppChannels()` + `declareAppPoints()`；
- `DomainServices`：`qualifyEventTopic`（原样返回）、`pluginId`（默认）、`postToWorkerThread`、
  `configJson`、`language`、`setLanguage`、`pluginsJson`、`pluginJson`；
  删 `eventSource()` / `subscribe()` / `publish()`；
- `kInterfaceTableCount` = 11（通用含 net）+ 10（业务）= **21**；
- 对外包装（主干代码用）：

```cpp
/// 发一次功能埋点（内部 = 库的 emitPointAsync + await）
asio::awaitable<std::optional<std::string>> emitPoint(
    std::string_view agentId, std::string_view pointId, std::string_view payloadJson,
    int32_t timeoutMs = 0);

/// 广播（宿主内部用）
void publishBroadcast(std::string_view id, std::string_view payloadJson);
void ensureBroadcast(std::string_view id, pluginxx::BroadcastCache_e cache, bool anyonePublish);
void endBroadcast(std::string_view id, std::string_view reasonJson);
```

- 框架扩展点覆写：

| 覆写 | 用途 |
|---|---|
| `createInstance` / `hostVtable` / `entrySymbols` / `selfRef` | 不变 |
| `appCapabilityDecl(id, out)` | 查 §3.4 埋点表（`impls=many`、`merge=first`、`phases=impl`、timeout） |
| `onCapabilityRowAdded(inst, id, spec, err)` | 只做校验与记账：`agentxx.agent.point.*` 要求 `phase==impl` 且 `scope` 非空（应用埋点必须带挂载引用）；`plugin.*` 不限 |
| `onCapabilityRowRemoved` | 记账清理 |
| `detachDomainRegistrations(inst)` | 从全部挂载拆实例：`capabilities()->removeScope(agentId)`（§3.2 的库能力）+ 撤广播订阅 |
| `clearDomainRegistrations(inst)` | 清 `Entry` 的本地记账（stop 后重新 start 用） |
| `onInstanceDisabledAsync` / `onInstanceUnloadingAsync` | 拆挂载 + `net->shutdownPlugin(inst->name)` |
| `applyDeclaredResources` / `releaseInstanceResources` | 不变（清单资源 + 工具对象） |

### 4.6 `PluginMount`

- `Entry` 删 `hookHandles` / `featurePoints` / `featureImpls`；新增
  `std::vector<PluginxxSubscription*> broadcastSubs`（卸载时逐个 `unsubscribe_broadcast`）；
- 删全部钩子注册表方法与功能点注册方法（`registerHook…/hooksJson/handlersOf/registerFeatureImpl/…`）；
- 保留 `coreHookHandlers_`（`addCoreHookHandler/removeCoreHookHandler` 改成本地 `std::vector`）；
- `RegistrationInventory`：`hookCount/featurePoints/featureImpls` 换成
  `pointImpls`（本挂载的能力行数，从 `capabilities()->rowsOf(id, impl, agentId)` 统计）与
  `broadcastSubs`；
- 挂载拆除（`unmountOneAsync` / `detachInstance`）：
  1. 撤销本挂载的广播订阅；2. `capabilities()->removeScope(agentId)`；
  3. 清提示词贡献 / 图节点 / 资源 / 工具（原有逻辑）。

> 因为作用域记在能力行里（L1），挂载侧不再需要"哪一行是我登记的"这类记账。

### 4.7 功能埋点派发

不再有宿主侧能力链实现（早期设计的 250 行自研链已取消），全部走库：

| 位置 | 改造 |
|---|---|
| `PluginHookDispatchHandle::onAgentcallStartFunc` 等 7 个点 | `co_await mount->manager()->emitPoint(agentId(), kPoint..., payload)`，然后顺序跑 core 处理器 |
| `agentxx::feature::ProvidePoint::ask(req)` | 组请求 JSON → `manager->emitPoint(agentId, 点 id, json)` → 解析 `TValue`；保留值缓存与重入 `busy` |
| 中间件（model/tool 相关） | 同上，一个 `emitPoint` 调用 |
| 埋点载荷 | §3.4 的字段；额外带 `agentId`（订阅侧过滤用） |

埋点调用的语义（库 L4 保证）：先发广播（通知面，失败只记日志），再调实现链
（`impls=many` + `merge=first`：第一个给出非空结果的行生效；没有行 → `OK` + `null`）；
超时/取消 → `CANCELED`，由调用方决定是"当没有意见"还是"中断轮次"（取值型按"没意见"处理）。

### 4.8 广播层与事件中枢

- 新增 `plugin_broadcast_channels.cpp`：§3.5 的应用频道表 + `declareAppChannels()` +
  `ensureBroadcast()/publishBroadcast()/endBroadcast()`；
- 新增 `plugin_broadcast_bridge.cpp`：把两个"只经 `EventBus` 发布"的话题镜像到广播：
  `agentxx.agent.round.start`（`BaseAgent` 轮次开始）、`agentxx.agent.host.client_attached`
  （`SessionServerAgentIO` 端点就绪）；实现 = `EventBus::listenPrefix` + `publishBroadcast`；
- `event/hub.{h,cpp}`：
  - `attachBus` → `attachBroadcasts(registry)`；`EventWait` 登记时按名订阅插件频道、最后一个等待撤销时退订
    （按载荷 `agentId` 过滤），见 §3.6；
  - `publishValue` → 写记录 + `registry->publish`；`endEvent` → `registry->end` + 唤醒 `mode=all`；
  - 删 `valueSubId_` / `listenPrefix("plugin.")`（插件值不再自动进中枢历史）；
- client 侧：`ClientPluginManager` 持 `BroadcastRegistry`，8 个事件发射点改 `publish`，
  `WirePluginData` ↔ `agentxx.client.plugin.*` 频道。

### 4.9 网络（`agentxx.net`）

**新增 `plugin_net_service.{h,cpp}`**（agent/client 共用）：

```cpp
class PluginNetService {
public:
    PluginNetService(asio::any_io_executor io, std::string dataDir,
                     std::shared_ptr<pluginxx::BroadcastRegistry> registry);
    void start(); void stop();
    int32_t call(std::string_view pluginId, std::string_view op,
                 std::string_view argsJson, std::string& outJson);
    int32_t callAsync(std::string_view pluginId, std::string_view op, std::string_view argsJson,
                      const PluginxxOperatorNotify* notify, std::string& outJson, int64_t* outId);
    int32_t cancel(std::string_view pluginId, int64_t requestId, std::string& outJson);
    void shutdownPlugin(std::string_view pluginId);
};
```

- `PluginNetHub::create(PublishFn, PostFn)`：`PublishFn` → `io.post` → `ensureBroadcast("plugin.<id>.net")`
  → `publish`；`PostFn` → `io.post`；
- `PluginxxNetIface g_ifaceNet`：三个入口先 `enterPluginHost` 取实例 → `pluginId` → 转发；
  `call_async` 用一个 `OpCore`（`setCompletionHandler`）保证"恰好一次 + io 线程"；
- 生命周期：`onInstanceDisabledAsync` / `onInstanceUnloadingAsync` → `shutdownPlugin`；
  `shutdownAllAsync` 之后 `stop()`；
- SDK：`NetCallAwaiter`（锚定协程，`call_async` 完成回调恢复）→
  `co_await netFetch(ctx, url, opt)` / `netDownload(...)`；`netCall` / `netCallAsync` 为直接入口。

**内置插件迁移（必做）**

| 插件 | 改动 |
|---|---|
| `agentxx_websearch` | `HttpClient::getAsync/postAsync` → `co_await netFetch(...)`；`fetchMarkdown` → `netFetch(..., {format:"markdown"})`（L5）；header/超时映射到参数字段 |
| `agentxx_rag_search` | `HttpClient::postAsync` → `netFetch(..., {method:"POST", json:{...}, timeoutMs:15000})` |
| `agentxx_chatbot`（HTTP） | `qq_api.cpp` / `qq_platform.cpp` 的请求 → `netFetch`（`headers` / `json` / `method` / `timeoutMs`；结果取 `status`/`headers`/`body`）；附件下载 → `netDownload`（拿 `path`） |
| `agentxx_chatbot`（WS） | `qq_gateway.cpp` 的 `utilxx::WsClient` → `netWsOpen` + `ws.open/message/close` 事件 + `netWsSend/Ping/Close`；重连退避、Hello/Identify/Resume 逻辑留在插件 |
| `agentxx_text_selection_monitor` | 手写 WS 握手/帧解析 → `netWsOpen`（`ws://127.0.0.1:<port>/<path>` + `headers`）+ `ws.message`；CDP 端口探测改为 `netSocketConnect`/`netFetch` 探活 |

迁移后这些插件不再 `#include "utilxx/http_client.h"` / `ws_client.h`（仍可用 `utilxx_base` 的
Json/字符串工具）；`cxx_utilxx` 的静态链接保留（其它功能仍在用）。

### 4.10 `ClientPluginManager`

- 继承加 `pluginxx::DomainServices`，构造体内 `setDomainServices(this)`；
- `xx_cquery_interface` 先 `queryGenericPluginIface<ClientPluginInstance, ClientPluginManager>`，
  再分发 6 张自有表 + `net`；
- 删 `g_clientIfaceEvents` / `g_clientIfaceJson` / `g_clientIfaceLog` / `g_clientIfaceCoroutineRuntime`
  及实现（约 600 行）；
- `kInterfaceTableCount` = 11 + 6 = **17**；
- 插件数据上下行改广播；加 `PluginNetService`（与 agent 侧同一个类）。

### 4.11 `plugin_kit.h`（SDK 重写）

删除（约 1200 行）：`AgentIfaces::hooks/events/feature/event`、`ClientIfaces::events/json/log`、
`detail::hookStart/hookCancel/HookShim/HookJob`、`hook/hookEx/unhook/listHooks`、
`defineFeaturePoint/provideFeature/callFeature/…`、`defineEvent/emitEventValue/endEvent/
registerEventWait/…`。

新增/改写：

| 函数 | 底层 | 备注 |
|---|---|---|
| `providePoint(ctx, agent, id, fn)` | `register_capability`（带 scope） | 实现行；`fn` 返回 JSON 文本或 `Task` |
| `onPoint(ctx, agent, id, fn, onEnd)` | `subscribe_broadcast_ex` | 订阅；按载荷 `agentId` 过滤其它 agent 的通知 |
| `declarePoint(ctx, spec)` / `emitPoint` / `endPoint` / `listPoints` | L4 原语 | 插件自定义埋点 |
| `declareBroadcast / publishBroadcast / subscribeBroadcast / readBroadcast / endBroadcast` | `<app>.broadcasts` | 通用通知 |
| `capability / invokeCapability / invokeCapabilityBlocking / capabilityInfo / listCapabilities` | `<app>.capabilities` | 插件间能力 |
| `netCall / netCallAsync / netFetch / netDownload / netWsOpen / netWsSend / netServeHttp …` | `<app>.net` | 高频三个给 `co_await` |
| `tool / fast_tool / blocking_tool / polled_tool`、权限、运行时、图节点、渲染器 | 原表 | 不变 |

`PluginBase`：`onHostReady()` 里对 `agentxx.agent.round.start` 的订阅改广播（失败只记日志）。

### 4.12 插件迁移清单

| 插件 | 改动 |
|---|---|
| `agentxx_math` / `agentxx_string` / `agentxx_system` / `agentxx_filesystem` / `agentxx_execute_command` | 仅重编（`PluginBase` 变化） |
| `agentxx_websearch` / `agentxx_rag_search` / `agentxx_chatbot` / `agentxx_text_selection_monitor` | 网络改 `agentxx.net`（§4.9）+ 广播 |
| `agentxx_codegraph` / `agentxx_planning` | `iface.events->publish` → `publishBroadcast`（`plugin.<id>.*`）；订阅 `agentxx.agent.host.client_attached` 改广播 |
| `agentxx_audio_stream` / `agentxx_screen_capture` / `agentxx_system_monitor` | publish → `publishBroadcast`；`system_monitor` 的 `capability(...)` 换新签名（能力 id 用 `plugin.agentxx_system_monitor.*`） |
| `agentxx_javascript_engine` | 注册 `agentxx.agent.js.interpreter`（官方能力，非埋点）；脚本 `on_event/emit_event` 改广播 |
| `agentxx_llm_uncensored` / `agentxx_computer_use` | 重编；有事件则改广播 |
| `example_plugin` | 埋点（实现 + 订阅）+ 广播 + 能力四处示范 |
| `example_event` | 广播 + `end`（示范结束语义） |
| `example_feature` | `providePoint` + `onPoint` 的新写法 |
| `example_graph_node` / `example_resources` / `example_multi_mount` | 重编 + 清单接口名更新 |
| `example_runtime` | `emit_event` → 广播；触发器/定时器不变 |
| `example_js` / `example_js_execute_command` | `interpreter.js` → `agentxx.agent.js.interpreter`；`has_capability` → `capability_info` |

### 4.13 JS 引擎的脚本 API

- `invoke_capability(_async)` 底层换成新 `invoke_capability`（完成回调形），Promise 语义不变；
- 脚本 `emit_event(topic, payload)` → `publish_broadcast`；`on_event(topic)` → 先尝试
  `declare_broadcast`（短名，幂等）再 `subscribe_broadcast`；新增 `end_event(topic)`；
- 文档 `plugins.md` §12 的 JS API 表同步。

## 5. 版本与兼容

### 5.1 版本表

| 层 | 项 | 旧 | 新 | 规则 |
|---|---|---|---|---|
| 库 | `<app>.capabilities` | v5 | **v6** | 表尾追加 `unregister_capability_ex`；`MIN_VERSION` 不变；`BASELINE_SIZE` = 追加前 `sizeof` |
| 库 | `<app>.broadcasts` | v2 | **v3** | 表尾追加 `end_broadcast` / `subscribe_broadcast_ex`；同上 |
| 库 | `<app>.net` | — | v1 | 新表（框架只给 ABI，实现由主程序提供） |
| 库 | `PLUGINXX_API_VERSION` / `_MIN_API_VERSION` 默认值 | 1 | **1（不改）** | 三个库被多个主程序共用；本项目用全局 FLAGS 覆盖成 5 |
| 库 | 版本号 | 0.2.0 | **0.3.0** | 代次 5 = 这些追加 + scope/end/埋点原语 |
| 应用 | `AGENTXX_PLUGIN_API_VERSION` | 4 | **5** | `agent/CMakeLists.txt` 约 628 行 |
| 应用 | agent 业务表 | 13 | **10** | 删 hooks / feature / event |
| 应用 | client 自有表 | 9 | **6** | 删 events / json / log |
| 应用 | `kInterfaceTableCount` | 23 / 9 | **21 / 17** | 都含 11 张通用表 |
| 应用 | 保留的表版本 | 见 §3.3 | **全部不变** | 成员与顺序一字不改，`boundaries` 成员台账照旧 |
| Wire | `kVersion` / `kMinVersion` / FFI API | 4 / 4 / 3 | **不变** | 本次不动协议 |

库里"表尾追加成员"不抬 `MIN_VERSION`：代次 5 的老插件仍能加载，只是 `ifaceHas` 判空后降级。
但本项目**不保留旧插件二进制**——`AGENTXX_PLUGIN_API_VERSION` 抬到 5 后，代次 4 的插件被
loader 直接拒绝（日志"插件 API 版本过低"）。

### 5.2 不兼容边界（明确不做兼容的地方）

| 项 | 处理 |
|---|---|
| 代次 4 编译的插件 | 拒绝加载；发布必须同时产出 `exec/plugins/*`（`builtin://` 内置插件随主程序二进制，自动同步） |
| `agentxx.agent.hooks` / `.feature` / `.event`、`agentxx.client.events` / `.json` / `.log` | 表不存在；老插件 `query_interface` 拿到空指针后走"主程序不支持"降级（这条路径原本就有） |
| `AgentxxPluginHookSpec/Ex`、`AgentxxPluginFeaturePointSpec/ImplSpec`、`AgentxxPluginEventSpec/WaitSpec` | 头文件删除，不留别名；插件源码编译期即报错，避免"加载成功但什么都没生效" |
| 能力 id `interpreter.js` | 改 `agentxx.agent.js.interpreter`（官方能力统一 `agentxx.agent.*`），JS 脚本侧常量同步 |
| `plugin_kit.h` 的 `hook*/provideFeature*/defineEvent*` | 删除；附录 A 给出逐条替代写法 |

### 5.3 配置与数据

- **不新增配置键**：`agentxx.net` 与事件中枢同一处理——恒常驻，没有开关；插件被禁用/卸载时
  `shutdownPlugin` 收掉它的连接与未完成请求（§4.9）；
- 下载落盘沿用 `data_dir`：`<data_dir>/<插件 id>/data/downloads/`（`PluginNetHub::setDataDir`）；
- `plugin.yaml` 不变（只有 name/entry/depends/resources/interfaces，没有事件/能力声明段）；
- 会话库、checkpoint、模型台账、`chatbot` 的账号与消息缓存目录都**不变**，升级后可直接继续用；
- 加载快照调整：`hooks` 段换成 `points`（本挂载的能力行数 + 实现者），`events` 段换成
  `broadcasts`（频道数 + 订阅数）。摘要字段变化只影响 `--dump-diagnostics` 与文档 §18，
  不影响插件行为。

## 6. 分步实施

依赖关系（→ 表示先后）：

```text
步骤 0 库改动 ──► 步骤 1 构建/接口表 ──┬─► 步骤 2 agent 侧(能力+广播+中枢) ─► 步骤 3 功能埋点
                                       │                                        │
                                       ├─► 步骤 4 网络(net + 4 插件) ───────────┤
                                       └─► 步骤 5 client 侧 ────────────────────┤
                                                                                 ▼
                                                          步骤 6 插件全量迁移 + JS 引擎
                                                                                 │
                                                                                 ▼
                                                          步骤 7 测试 / 文档 / 收尾
```

### 步骤 0：库改动（5~6 人日）

改动内容 = §3.11 的 L1~L7。建议落地顺序（每步单独能编过、单独提交）：

| 序 | 内容 | 主要文件 |
|---|---|---|
| 0.1 | 能力行带 scope（L1）+ `removeScope` | `host/capability_registry.{h,cpp}`、`host/host_core.h`、`api/tables.h`（v6）、`host/tables_impl.h`、`api/iface_meta.h` |
| 0.2 | 宿主发起调用（L2） | `runtime/op_driver.h`（`createDetached`）、`host/host_core.h`（`caller` 可空） |
| 0.3 | 广播结束（L3） | `host/broadcast_registry.{h,cpp}`、`api/tables.h`（v3）、`host/tables_impl.h`、`api/iface_meta.h` |
| 0.4 | 功能埋点原语（L4） | `host/host_core.h`（`declarePoint/emitPointAsync/endPoint`）、`kit/kit.h`（`point/onPoint/emitPoint`） |
| 0.5 | net 补 `format:"markdown"` + `sslVerify`（L5） | `cxx_utilxx`：`include/utilxx/plugin_net_hub.h`、`src/plugin_net_hub.cpp`、`docs/plugin-api.md` |
| 0.6 | 文档与 ABI 断言（L6） | `CHANGELOG.md`、`README.md`、`src/api_abi_check.c` |
| 0.7 | 库自测或"由主程序覆盖"说明（L7） | `cxx_pluginxx/test/`（若仓库无测试工程，则写进 `CHANGELOG` 并在 §7.2 覆盖） |

验收：

- 两个库各自 Debug/Release 编过（`api_abi_check.c` 的静态断言必须过）；
- 子模块指针更新后，`work.md` 记录两个库的提交号与新 tag；
- 这一步会短暂打破当前代码树（表版本变化），因此**库提交与主程序步骤 1 成对推进**，
  不要在库仓库单独提交后放着。

### 步骤 1：构建与接口表骨架（1 人日）

- `agent/CMakeLists.txt`：`AGENTXX_PLUGIN_API_VERSION` 4→5；全局 `CMAKE_*_FLAGS` 与
  `cxx_pluginxx_repo` 的 `CMAKE_ARGS` 同时加 `-DPLUGINXX_APP_NAME=agentxx`；
  `cxx_utilxx_repo` 的 `DEPENDS` 补 `cxx_pluginxx_repo`；
- `agent/plugins/CMakeLists.txt`：应用名不是 `agentxx` 时 `FATAL_ERROR`（独立构建插件目录同样拦）；
- `plugin/api/plugin_api.h`：IID 改 `PLUGINXX_APP_ID("agent.tools")` 形式（值不变），删三张
  业务表的结构体与错误码；
- `plugin_iface_meta.h` 删 6 条特化；`plugin_interfaces.h` 换 `Broadcasts` / `Net`；
- `plugin_framework.h` 换新头（`domain_services.h`、`broadcast_registry.h`、`api/net.h`）；
- 两张 `kInterfaceTableCount` 改 21 / 17，`boundaries` 规则 7 与 §7 的期望数字同步。

验收：`agentxx_static` 编出（删掉的功能入口先留空实现返回失败）；`boundaries` 通过；
`agentxx_test boundaries` 通过。

### 步骤 2：主程序 agent 侧（3 人日）

- `PluginManager` 加 `DomainServices`（删 `eventSource/subscribe/publish`）、
  `setOfficialPrefix("agentxx.")`、启动期 `declareAppChannels()` + `declareAppPoints()`；
- `plugin_broadcast_channels.cpp`（新）：§3.5 频道表 + `ensureBroadcast/publishBroadcast/endBroadcast`；
- `plugin_broadcast_bridge.cpp`（新）：`agentxx.agent.round.start` / `.host.client_attached`
  两条只经 `EventBus` 的话题镜像到广播；
- `event/hub.{h,cpp}`：`attachBus` → `attachBroadcasts`，`publishValue` 写记录 + 发广播，
  `endEvent` → 广播 end + 唤醒 `mode=all`；插件值不再走 `listenPrefix("plugin.")`；
- `plugin_manager_vtable.cpp`：删 3 张表分支，`query_interface` 先走
  `queryGenericPluginIface`（含 `broadcasts` / `capabilities` / `net`）；
- `plugin_manager_lifecycle.cpp`：`detachDomainRegistrations` 改调
  `capabilities()->removeScope(agentId)` + 撤广播订阅；
- `plugin_mount.{h,cpp}`：`Entry` 删 `hookHandles/featurePoints/featureImpls`，加
  `broadcastSubs`；`RegistrationInventory` 字段同步。

验收：`plugins`、`plugin_host_mount`、`plugin_multi_mount`、`plugin_cleanup`、`event_hub`、
`event_delivery`、`async_work`、`async_tools` 全绿（此时插件侧还没全部迁完，
用改造中的 `example_plugin` 先行验证）。

### 步骤 3：功能埋点（2 人日）

- `plugin_manager_hooks.cpp` → `plugin_manager_points.cpp`：`PluginHookDispatchHandle` 的 7 个
  回调改成 `co_await manager->emitPoint(agentId, id, payload)`；core 层处理器改为本地
  `std::vector`（不进框架表，在插件行之后跑）；
- `agentxx::feature`：`ProvidePoint::ask/call` 改成 `emitPoint` + 解析结果；保留
  `value_cache.h`、重入 `busy`、"无实现 = 没意见"语义；`registry.{h,cpp}` 删除；
- 中间件调用点：`middlewares/summarization.cpp`（`context.summarize`、
  `tool.summarizeOutput`）、token 估算（`context.countTokens`）、modelcall（`model.start/run/end`）、
  `toolcall`（`tool.start/end`）、`BaseAgent`（`turn.start/end`）；
- 埋点 id 常量头 `plugin/api/agentxx_point_ids.h` + 宿主声明表（两处一致性由
  `boundaries` 校验）。

验收：新增 `plugin_point` 模块（§7.2）；`prompt_cache_prefix`、`summarization`、`feature_points`、
`plugin_feature` 改成新写法后全绿。

### 步骤 4：网络（4 人日）

- `plugin_net_service.{h,cpp}`（新，agent/client 共用）+ `g_ifaceNet` 三个入口 +
  `PluginNetService` 生命周期挂到卸载/禁用/停机；
- `plugin_kit.h` 的 `netCall/netFetch/netDownload/netWsOpen/...`（高频三个给 `co_await`）；
- 4 个插件迁移（§4.9 表）：`agentxx_websearch`、`agentxx_rag_search`、`agentxx_chatbot`
  （HTTP + WS）、`agentxx_text_selection_monitor`；
- `plugin_net_hub` 的 `format:"markdown"` 已由步骤 0.5 提供。

验收：`plugin_net` 模块（§7.2）+ 4 个插件的本地回环往返测试（§7.4）；
`grep -r "utilxx/http_client.h\|utilxx/ws_client.h" agent/plugins/` 为空。

### 步骤 5：client 侧（1.5 人日）

- `ClientPluginManager` 加 `DomainServices` + 自建 `BroadcastRegistry` + `PluginNetService`；
- 删 `g_clientIfaceEvents/Json/Log/CoroutineRuntime` 及实现（约 600 行）；
- 8 个 `AGENTXX_CLIENT_EVT_*` 改 `agentxx.client.*` 广播；`WirePluginData` 上下行改广播；
- `PluginBase::onHostReady` 的轮次订阅改广播。

验收：`client_plugins`、`plugin_sdk`、`plugin_bridge`、`tui_widget`、`tui_form` 全绿。

### 步骤 6：插件全量迁移（3 人日）

- 17 个内置 + 9 个示例按 §4.12 表逐项改；`agentxx_javascript_engine` 的脚本 API 按 §4.13；
- 改完跑 `agent/script/build_plugins.ps1 -List` 与全量构建（插件不在 ALL 里，必须点名编）。

验收：`plugins` 空列表启动后 `exec/plugins/*` 全部加载成功（日志无"版本过低"/"接口缺失"）。

### 步骤 7：测试、文档、收尾（2.5 人日）

- 新增/改造测试模块（§7.2）；跑两轮全量 `agentxx_test`（debug，含 ASan）；
- 文档：`plugins.md`（§4/§8/§9/§12/§17~§20 与表清单）、`index.md`、`tui.md`（若涉及 Info 段）、
  根 `AGENTS.md`、`agent/plugins/AGENTS.md`、`docs/zh-cn/design/events.md`；
- `work.md` 回填实际改动与偏差，`index.md` 补结论。

总计约 **22 人日**（单人串行 4~5 周）。

## 7. 验证

### 7.1 构建矩阵

| 配置 | 命令 | 通过标准 |
|---|---|---|
| Windows debug | `agent/script/windows_debug_build.bat` | 无 error/warn（只看关键词） |
| Linux debug | `agent/script/linux_debug_build.sh` | 同上（含 ASan/UBSan） |
| Release（至少一侧） | `windows_release_build.bat` 或 `linux_release_build.sh` | LTO 下能编过（库改动涉及模板与内联） |
| 插件全量 | `agent/script/build_plugins.ps1` / `.sh` | 全部插件产物时间戳刷新 |
| 插件目录独立构建 | `cmake -S agent/plugins -B <tmp>` | 应用名检查生效、能编过 |
| 导出符号 | `agent/script/check_plugin_exports.sh` | 白名单不变、全部通过 |

### 7.2 库能力与新测试模块

| 验证项 | 做法 | 期望 |
|---|---|---|
| 能力行按 scope 隔离 | 两个 agent 挂同一插件，各自登记 `agentxx.agent.point.tool.end`，只触发 A | 只跑 A 的行；`removeScope(A)` 后 A 行消失、B 行仍在 |
| 覆盖键含 scope | 同 provider、同 ownerTag、两个 scope 各登记 `impls:one` | 两行并存（旧实现会互相覆盖） |
| 宿主发起调用（L2） | manager 自己 `emitPointAsync`（插件不参与） | 拿到结果；取消后 `Canceled` 回调**恰好一次** |
| 广播结束（L3） | declare → publish ×2 → end → publish → 再 declare → publish | 第 3 次 `ERR_ENDED(-5)`；`on_end` 一次；`read` 得 `ERR_ENDED`；再 declare 后可发 |
| 埋点原语（L4） | 无行 / 一行 / 两行（`merge=first`） | `"null"` / 行结果 / 第一行的结果 |
| 埋点订阅按 agent 过滤 | 插件在某 agent 上 `onPoint`，另一个 agent 触发 | 不回调 |
| net markdown（L5） | `http.fetch {format:"markdown"}` 打本地 HTML | `body` 是 Markdown、`format=markdown` |

模块安排（`agent/test/test.cpp`）：

| 模块 | 内容 | 来源 |
|---|---|---|
| `plugin_point`（新） | 声明 / 实现行 / 订阅 / 取值缓存 / `busy` / core 顺序 / scope 过滤 | 合并旧 `plugin_hooks` + `plugin_feature` |
| `plugin_broadcast`（新） | 声明 / 发布 / 读取 / 撤销 / 权限 / 限频 / `end` | 改造旧 `plugin_event` |
| `plugin_net`（新） | http 全字段 / ws 收发 / 下载落盘 / 插件卸载取消 / 无 dataDir 报错 | 新 |
| `plugins` / `plugin_sdk` / `plugin_runtime` / `plugin_host_mount` / `plugin_multi_mount` / `plugin_cleanup` / `plugin_resources` / `plugin_multi_instance` | 原样，断言改成新接口 | 改造 |
| `plugin_agent_runtime` / `plugin_example_runtime` / `plugin_example_chatbot` | 运行时与 chatbot，`emit_event` 改广播 | 改造 |
| `boundaries` | 表数量 21 / 17、`IfaceMeta` 特化数、埋点 id 头与宿主声明表一致 | 改造 |

### 7.3 主程序回归

`agentxx_test --log-level warn` 全量跑一遍（debug + ASan），重点模块：`plugins`、`plugin_*`、
`event_hub`、`event_delivery`、`event_tools`、`async_work`、`async_tools`、`chatbot_task`、
`codegraph_tools`、`client_plugins`、`tui_*`、`boundaries`、`config_keys`。

### 7.4 网络插件的本地回环验证

全部打 `127.0.0.1`，不依赖外网（CI 无网）：

| 被测 | 端点 | 校验 |
|---|---|---|
| `agentxx_websearch` | `utilxx::HttpServer`（回 HTML / JSON） | `format` 两档、header 透传、超时生效 |
| `agentxx_rag_search` | 同上（回 JSON） | POST body 与 `json` 字段一致、15s 块超时映射 |
| `agentxx_chatbot`（HTTP） | 同上（回 `access_token`、模拟 4xx/5xx） | 重试与错误分支、`headers`/`maxBytes` 生效 |
| `agentxx_chatbot`（WS） | `PluginNetHub` 的 `ws.serve`（独立端口） | Hello/Identify/心跳/Resume、断线后重连退避 |
| `agentxx_text_selection_monitor` | 同上模拟 CDP `/json/list` | 端口探测、`Runtime.evaluate` 往返 |
| `net` 自身 | `utilxx::HttpServer` + hub 自己的 `ws.serve` | 见 §7.2 最后两行 |

### 7.5 客户端侧

- 远端模式下插件面板/装饰/overlay 仍能显示（`client_plugins` + 手工开一次 TUI）；
- `WirePluginData` 上下行改广播后，插件设置往返一次（例：`system_monitor` 面板）；
- 客户端 `agentxx.net` 可被插件调用（测试里用本地回环起一个请求）。

### 7.6 手工验收清单

| 场景 | 期望 |
|---|---|
| 空列表启动 | 已配置的插件全部加载（17 内置 + 9 示例按配置加入），`exec/plugins` 无"版本过低" |
| 插件禁用 → 启用 | 工具/权限/埋点实现行/广播订阅全部撤掉再重新登记（`registrationInventory` 归零再回原值） |
| 后台任务 + 侧边栏 | 「后台任务」段正常刷新（走广播后仍按秒续推） |
| chatbot 长连 | 断网恢复后自动重连、心跳不中断 |
| 客户端重连 | 插件面板与装饰按 `agentxx.client.*` 广播重建 |

### 7.7 静态自检

```bash
# 表数量与文档一致（规则 7 + 文档 grep）
agentxx_test boundaries

# 插件里不允许再直接建连接（应无输出）
grep -rn 'utilxx/http_client\.h\|utilxx/ws_client\.h' agent/plugins/ || echo ok

# 旧概念残留（应无输出）
grep -rn 'iface\.events\|feature_points\|hookEx' agent/plugins/ agent/lib/src/plugins/ || echo ok

# 过时数字（改完后应无输出）
grep -rn '23 张\|9 张接口表\|13 张业务' docs/ AGENTS.md agent/plugins/AGENTS.md || echo ok
```

## 8. 风险与应对

| # | 风险 | 影响 | 应对 | 步骤 |
|---|---|---|---|---|
| R1 | 跨仓库协调（库改动 + 子模块指针） | 库未落地时主程序编不过，或两边版本错配 | 库仓库打 tag `0.3.0` 后再更新子模块指针，与 `AGENTXX_PLUGIN_API_VERSION=5` 同一次提交；成员缺失会直接编译失败，天然兜底 | 0/1 |
| R2 | L1 覆盖键变化 | 同插件同 owner 在多挂载上的行从"覆盖"变"并存"，可能重复执行 | `impls:one` 的埋点仍按 `(provider, scopeId, ownerTag, phase)` 覆盖（每挂载一行）；重复执行只可能出现在 `impls:many`，由插件自己保证幂等 | 0/3 |
| R3 | L2 无租约操作的生命周期 | manager 析构时未完成回调访问已销毁对象 | `createDetached` 持 runtime 弱引用；`shutdownAllAsync` 之前先取消全部外层操作，并等所有 `done` 回调返回后才析构服务对象 | 0/2 |
| R4 | L3 `end` 的并发边界 | 与进行中的 publish 竞争、end 后旧订阅者残留 | 结束标记与 publish 在同一把登记表锁内判定；end 之前已排队的 publish 照常投递，之后一律 `ERR_ENDED`；end 时同步摘除订阅并回调 `on_end` | 0/3 |
| R5 | 埋点在高频路径上的开销 | 每次模型尝试/每个工具都发埋点，序列化与派发拖慢轮次 | 无订阅者且无实现行时直接返回（`wants()` 判定，零序列化）；通知型默认 `timeoutMs=30`、超时/异常只记日志；取值型才同步等结果 | 3 |
| R6 | 网络迁移等价性 | markdown 转换细节、TLS 默认校验、WS 心跳/重连、CDP 帧格式、下载文件名与原实现不一致 | §4.9 的逐项对照表 + 每个插件一条本地回环测试；WS 的重连退避、心跳定时、Hello/Resume 状态机**留在插件**，net 只负责收发帧 | 4 |
| R7 | 无流式响应 | `PluginNetHub` 只给一次性 body，SSE/增量接口拿不到 | 本轮不改：内置插件无 SSE 需求（chatbot 走 WS + 短轮询）；将来需要再给 hub 加 `http.stream`（不影响本次） | 4 |
| R8 | 插件二进制与发版流程 | 抬代次后旧插件被拒，用户升级只替换主程序会全插件失效 | `build_plugins.ps1/sh` 纳入发版脚本；`windows/linux/macos_release_build` 末尾显式编插件；README 写明"主程序与插件必须同版本" | 1/6 |
| R9 | 库无自带测试 | L1~L4 的语义只能间接验证 | 本项目新增 3 个模块（§7.2）覆盖每条语义；库仓库补最小自测，否则在 `CHANGELOG` 记明"以主程序测试为准" | 0/7 |
| R10 | 文档漂移 | 23/21、9/6/17 分散在 5 处以上 | 收尾按 §7.7 的 grep 清单一次性核对，`boundaries` 兜底 | 7 |
| R11 | 中枢传输换成广播 | "谁在听"判定与投递语义变化，可能丢 next-step 输入；插件频道不再自动进中枢历史 | 中枢保留自己的记录与等待，广播只做通知面；插件频道按名按需订阅（§3.6）并按 `agentId` 过滤；`mode=all` 收尾改用 `on_end`；回归 `event_hub`/`event_delivery`/`event_tools`/`event_subagent`/`async_*` | 2 |
| R12 | 一次改完 26 个插件 | 排查困难，失败点分散 | 按 §4.12 分三批：纯工具（仅重编）→ 广播类 → 网络类；每批后跑一次全量测试 | 6 |

### 8.1 已确认的选择与遗留细节

用户已确认（本轮据此定稿）：

1. 可以修改 `cxx_*` 库，通用功能直接做进库（§3.11）；
2. 钩子与功能点彻底统一到能力，扩展点部分称"功能埋点"，插件可实现或订阅（§3.1）；
3. 插件自定义能力用 `plugin.<插件 id>.*` 前缀即可，不需要额外官方 id 机制；
4. 库直接支持"结束"语义，不用 `{"$end":true}` 约定值（L3）；
5. 内置插件的网络调用必须走 `agentxx.net`（§4.9）；
6. client 侧也提供 `agentxx.net`；
7. `agentxx.agent.mount` 表保留。

实施时才需要定的细节（不阻塞开工）：

| 细节 | 当前取值 | 何时定 |
|---|---|---|
| 埋点 id 形态 | `agentxx.agent.point.<域>.<名字>` | 步骤 3 起名前 |
| 埋点超时默认值 | 通知型 30 ms / 取值型 3000 ms | 步骤 3 实测后 |
| client 侧广播限频 | 框架默认（0 = 不限）；`delta`/`tool.end` 一类在 client 侧限 50 ms | 步骤 5 |
| `text_selection_monitor` 的 CDP 帧 | 先按文本帧用 `ws.open`/`ws.message`；需要二进制帧再补 | 步骤 4 |
| 库 tag | `0.3.0` | 步骤 0 提交时 |

## 附录 A：旧 → 新对照

### A.1 SDK（`plugin_kit.h`）

| 旧 | 新 | 说明 |
|---|---|---|
| `hook(ctx, agent, point, fn)` | `onPoint(ctx, agent, "agentxx.agent.point.<…>", fn)` | 订阅面；原来的"钩子"就是"订阅埋点" |
| `hookEx(...)` / `listHooks` | `providePoint(...)` + `listPoints()` | 实现面（要影响结果时） |
| `unhook(h)` | `unsubscribeBroadcast(sub)` / `unregisterCapabilityRow(...)` | 按当初选的面撤 |
| `defineFeaturePoint(spec)` | `declarePoint({id, impl, notify, …})` | 插件自定义埋点 |
| `provideFeature(point, fn)` | `providePoint(ctx, agent, point, fn)` | 返回 JSON 文本或 `Task` |
| `callFeature(point, args)` | `emitPoint(ctx, point, args)` | 应用侧是 `PluginManager::emitPoint` |
| `defineEvent(spec)` | `declareBroadcast({id, cache, publish})` 或 `declarePoint(...)` | 纯通知用广播；要结果用埋点 |
| `emitEventValue(id, v)` | `publishBroadcast(id, v)` / `emitPoint(...)` | |
| `endEvent(id)` | `endBroadcast(id, reason)` / `endPoint(id, reason)` | 库 L3 |
| `registerEventWait(...)` | — | 等待只有中枢内部用；插件用 `subscribeBroadcast` |
| `iface.events->publish/subscribe` | `iface.broadcasts->publish/subscribe` | 28 处调用点 |
| `capability(spec)` | `capability({id, scope, impls, merge, phases})` | 新签名带 scope |
| `invokeCapability(...)` | `invokeCapability(...)`（完成回调）/ `co_await invokeCapability` | 形状变化 |
| `has_capability(id)` | `capabilityInfo(id)` 判空 | |

### A.2 宿主侧

| 旧 | 新 |
|---|---|
| `pluginxx::DomainHooks` + `eventSource()` | `pluginxx::DomainServices`（无事件后端） |
| `EventSource` / `event_bus.h` | `BroadcastRegistry`（`broadcast_registry.h`） |
| `PluginManager::subscribe/publish` | `publishBroadcast` / `ensureBroadcast` / `endBroadcast` |
| `PluginMount::hookHandlers_` + `orderedHandlers` | 框架 `CapabilityRegistry` 按 scope 排行 |
| `PluginMount::featurePoints/featureImpls` + `registry.{h,cpp}` | 同上（行 + `agentxx::feature` 的取值缓存） |
| `PluginHookDispatchHandle` 的 7 个钩子点 | 同名"功能埋点"（`emitPoint`） |
| `agentxx.agent.event` 的 9 个成员 | `broadcasts` 表 + 中枢内部记录 |
| `AgentxxPluginEventSpec` 的 `$end` 约定 | `endBroadcast` / `endPoint` |

### A.3 表

| 旧 | 新 |
|---|---|
| `agentxx.agent.hooks` v2 | `<app>.capabilities` v6（实现行）+ `<app>.broadcasts` v3（订阅） |
| `agentxx.agent.feature` v2 | 同上 |
| `agentxx.agent.event` v1 | `<app>.broadcasts` v3 |
| `agentxx.client.events` v1 | `<app>.broadcasts` v3 |
| `agentxx.client.json` / `.log` | `<app>.json` / `<app>.log`（框架实现） |
| — | `<app>.net` v1（新） |

## 附录 B：文件改动清单

### B.1 库 `cxx_pluginxx`（含子模块指针）

| 文件 | 动作 |
|---|---|
| `include/pluginxx/host/capability_registry.h`、`src/host/capability_registry.cpp` | 改：`scopeId`、`rowsOf(...scope)`、`removeScope`、覆盖键 |
| `include/pluginxx/host/host_core.h` | 改：`registerCapability` 存 scope、`invokeCapabilityAsync` 允许 `caller=nullptr`、`declarePoint`/`emitPointAsync`/`endPoint` |
| `include/pluginxx/host/broadcast_registry.h`、`src/host/broadcast_registry.cpp` | 改：`end`、`BroadcastSub::onEnd`、`ERR_ENDED`、再 declare 开新流 |
| `include/pluginxx/runtime/op_driver.h` | 改：`OpCore::createDetached` |
| `include/pluginxx/api/tables.h` | 改：capabilities v6 尾追 `unregister_capability_ex`；broadcasts v3 尾追 `end_broadcast`/`subscribe_broadcast_ex`；`PLUGINXX_RESULT_ERR_ENDED` |
| `include/pluginxx/host/tables_impl.h` | 改：新成员实现 |
| `include/pluginxx/api/iface_meta.h` | 改：两张表的 `VERSION` / `BASELINE_SIZE` |
| `include/pluginxx/kit/kit.h` | 改：`point/onPoint/emitPoint/declarePoint/endPoint`、广播 end、能力新签名 |
| `src/api_abi_check.c` | 改：新成员静态断言 |
| `CHANGELOG.md` / `README.md` / `docs/plugin-api.md` | 改 |

### B.2 库 `cxx_utilxx`

| 文件 | 动作 |
|---|---|
| `include/utilxx/plugin_net_hub.h`、`src/plugin_net_hub.cpp` | 改：`format:"markdown"`、`sslVerify` |
| `docs/plugin-api.md`、`CHANGELOG.md` | 改 |

### B.3 主程序（agent 侧）

| 文件 | 动作 |
|---|---|
| `agent/CMakeLists.txt` | 改：API 5、`PLUGINXX_APP_NAME`、`cxx_utilxx_repo` 依赖 |
| `agent/plugins/CMakeLists.txt` | 改：应用名一致性检查 |
| `lib/include/agentxx/plugin/api/plugin_api.h` | 改：IID 宏、删三张表结构体 |
| `lib/include/agentxx/plugin/api/client_plugin_api.h` | 改：删 events/json/log 三表 |
| `lib/include/agentxx/plugin/api/agentxx_point_ids.h`、`agentxx_broadcast_ids.h` | 新 |
| `lib/include/agentxx/plugin/api/plugin_kit.h` | 改：SDK 重写（§4.11） |
| `lib/include/agentxx/plugin/api/plugin_iface_meta.h` | 改：删 6 条特化，补 net |
| `lib/include/agentxx/plugin/api/plugin_spec.h` | 改：删废弃结构体 |
| `lib/include/agentxx/plugin/plugin_framework.h` | 改：换头与 using |
| `lib/include/agentxx/plugin/plugin_interfaces.h` | 改：`Broadcasts` / `Net` |
| `lib/include/agentxx/plugin/plugin_manager.h` | 改：`DomainServices`、`emitPoint`、广播包装、扩展点覆写 |
| `lib/include/agentxx/plugin/plugin_mount.h` | 改：`Entry` 字段、清单字段 |
| `lib/include/agentxx/plugin/client_plugin_manager.h` | 改：同类调整 + `kInterfaceTableCount=17` |
| `lib/src/plugins/plugin_manager_domain_hooks.cpp` | 改名 `plugin_manager_domain_services.cpp`（`git mv`），删事件入口 |
| `lib/src/plugins/plugin_manager_vtable.cpp` | 改：删 3 表分支、加 `net` |
| `lib/src/plugins/plugin_manager_hooks.cpp` | 改名 `plugin_manager_points.cpp`，改派发 |
| `lib/src/plugins/plugin_manager_feature.cpp` | 删 |
| `lib/src/plugins/plugin_manager_lifecycle.cpp` | 改：`removeScope` + 撤订阅 |
| `lib/src/plugins/plugin_manager_runtime*.cpp` | 改：`emit_event` → 广播 |
| `lib/src/plugins/plugin_broadcast_channels.cpp`、`plugin_broadcast_bridge.cpp` | 新 |
| `lib/src/plugins/plugin_net_service.{h,cpp}` | 新（agent/client 共用） |
| `lib/src/plugins/plugin_mount.cpp` | 改 |
| `lib/src/plugins/client_plugin_manager.cpp` | 改：删 4 张重复表实现、加广播与 net |
| `lib/include/agentxx/event/hub.h`、`lib/src/event/hub.cpp` | 改：传输换广播 |
| `lib/include/agentxx/feature/{feature.h,points.h}`、`lib/src/feature/{feature.cpp,points_context.cpp}` | 改：走 `emitPoint` |
| `lib/include/agentxx/feature/registry.h`、`lib/src/feature/registry.cpp` | 删 |
| `lib/src/agent/base_agent.cpp`、`lib/src/nodes/modelcall.cpp`、`lib/src/nodes/toolcall.cpp`、`lib/src/middlewares/summarization.cpp` | 改：埋点载荷 |
| `client/src/.../plugin_*.cpp`（`WirePluginData` 相关） | 改：广播 |

### B.4 插件（17 内置 + 9 示例）

按 §4.12 表逐目录改；网络四个（`agentxx_websearch`、`agentxx_rag_search`、
`agentxx_chatbot`、`agentxx_text_selection_monitor`）改动最大，
`agentxx_javascript_engine` 与三个 `example_*`（`example_plugin` / `example_event` /
`example_feature`）是接口示范，优先改以确定写法。

### B.5 测试与文档

| 文件 | 动作 |
|---|---|
| `agent/test/test.cpp` | 改：模块名（`plugin_point` / `plugin_broadcast` / `plugin_net`） |
| `agent/test/plugin/test_plugin_hooks.cpp` + `test_plugin_feature.cpp` | 合并成 `test_plugin_point.cpp` |
| `agent/test/plugin/test_plugin_event.cpp` | 改名 `test_plugin_broadcast.cpp` 并重写 |
| `agent/test/plugin/test_plugin_net.cpp` | 新 |
| `agent/test/core/test_boundaries.cpp` | 改：21 / 17、`IfaceMeta` 数、埋点 id 一致性 |
| `agent/test/plugin/test_plugin_*.cpp`（其余）、`agent/test/client/test_client_plugins.cpp` | 改：新接口 |
| `docs/zh-cn/design/plugins.md` | 改：§4/§8/§9/§12/§17~§20、表清单、埋点清单 |
| `docs/zh-cn/design/index.md`、`events.md`、`tui.md`（如需） | 改 |
| 根 `AGENTS.md`、`agent/plugins/AGENTS.md` | 改：接口表数量、埋点/广播/网络三段 |
| `resource/history/plugin-refactor-4/{index,plan,work}.md` | 改：随实施回填 |
