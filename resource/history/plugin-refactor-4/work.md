# 实施记录: 适配 cxx_pluginxx 0.3.0 的插件体系重构

> 方案见同目录 `plan.md`；任务卡片见 `index.md`。
> 计划分 8 步：步骤 0 改库 → 步骤 1~7 改主程序/插件/测试/文档（见 `plan.md` §6）。

## 当前进度

| 阶段 | 状态 |
|---|---|
| 方案设计（plan.md） | 已完成 |
| 步骤 0 库改动（`cxx_pluginxx` L1~L4/L6/L7、`cxx_utilxx` L5）+ 子模块指针 | 已完成 |
| 步骤 1 构建与接口表骨架 | 已完成 |
| 步骤 2 agent 侧（DomainServices + 广播 + 中枢） | 已完成（广播通道打通，事件中枢对接完成，`plugins` 模块全绿） |
| 步骤 3 功能埋点（替换钩子与功能点） | 进行中（声明表 + 分发入口 + `plugin_point` 模块 + 7 个通知型埋点已接到派发；旧三表与取值型埋点待改） |
| 步骤 4 网络（`agentxx.net` + 4 个插件迁移） | 已完成（agent/client 双侧表 + SDK 便捷层 + 4 个插件全迁：websearch / rag_search / text_selection_monitor / chatbot） |
| 步骤 5 client 侧（广播 + net） | 进行中（net 已提供；广播与 DomainServices 待做） |
| 步骤 6 插件全量迁移 + JS 引擎 | 进行中（能力表 APIs 已迁：JS 引擎 + 3 个示例插件） |
| 步骤 7 测试、文档、收尾 | 待开始 |

## 实施记录（按步骤回填）

### 步骤 0：库改动

- 库提交号：
  - `cxx_pluginxx` `8417ff1`（0.3.0 准备：能力行带作用域、宿主发起调用、广播流结束、功能埋点原语）
    + `5f05060`（修正无主操作句柄前置声明与能力调用 label 类型）
  - `cxx_utilxx` `4dc209f`（插件网络 `http.request`/`http.fetch` 支持 `format: markdown`）
  - 主仓子模块指针 `5370b1b2`
- 改动（`cxx_pluginxx`）：
  - L1 能力行带作用域：`CapabilityProvider::scopeId`，覆盖键 `(provider, scopeId, ownerTag, phase)`，
    `rowsOf(id, phase, scope)` / `removeScope` / `unregisterScopedProvider`；
    `capabilities` 表 v5→**v6** 尾追 `unregister_capability_ex`（基线仍停在 `invoke_capability_ex`）
  - L2 宿主发起调用：`OpCore::createDetached` + `PluginRuntime::detachedHandles`
    （`kDetachedHandleKeep = 512` 保活无主句柄）；`invokeCapabilityAsync` 允许 `caller == nullptr`
  - L3 广播流结束：`BroadcastRegistry::end/isEnded`、`BroadcastSub::onEnd`、
    `PLUGINXX_RESULT_ERR_ENDED(-5)`；`broadcasts` v2→**v3** 尾追 `end_broadcast` / `subscribe_broadcast_ex`
  - L4 功能埋点原语：`PluginPointDecl` + `declarePoint` / `emitPointAsync` / `endPoint`；
    `appendStage` 增加可选 `scopeId`
  - `api_abi_check.c` 补基线断言；CHANGELOG / README 更新
- 改动（`cxx_utilxx`）：`plugin_net_hub.cpp` 支持 `format:"markdown"`（html2md，`splitLines=false`，
  结果回填 `format` 字段）
- 验证：`cxx_pluginxx_repo`、`cxx_utilxx_repo` 单独构建通过（只剩原有 C4275 警告）
- 与计划的偏差：
  - 库自带的 `pluginxx.events` 通用表在 0.3.0 里已删除，`host/event_bus.h` / `host/domain_hooks.h`
    也一并删除 —— 主程序侧必须同轮改完，不能再"边留边改"

### 步骤 1：构建与接口表骨架

- 改动文件：
  - `agent/CMakeLists.txt`：`AGENTXX_PLUGIN_API_VERSION` 4→**5**（含 cache 纠正块：
    `set(... CACHE ...)` 不会覆盖旧缓存，必须显式 `FORCE` 纠正，否则框架/插件编到不同代次）；
    新增 `AGENTXX_PLUGIN_APP_NAME=agentxx` 并以 `-DPLUGINXX_APP_NAME=\"agentxx\"` 追加到
    `CMAKE_C_FLAGS/CXX_FLAGS`（**必须带引号**，否则 `app.h` / `kit.h` 里 `PLUGINXX_APP_ID(...)`
    拼出来的 `agentxx` 被当成标识符）；`cxx_pluginxx_repo` 传 `-DPLUGINXX_APP_NAME=...`；
    ExternalProject 顺序重排为 base → pluginxx → utilxx → ui（`cxx_utilxx_repo` 依赖 `cxx_pluginxx_repo`）
  - `agent/plugins/CMakeLists.txt`：新增应用名一致性检查（不是 `agentxx` 直接 `FATAL_ERROR`）
  - `agent/lib/include/agentxx/plugin/api/plugin_api.h`：10 个 IID 宏改成 `PLUGINXX_APP_ID("agent.tools")` 形式
  - `agent/lib/include/agentxx/plugin/api/client_plugin_api.h`：6 个 IID 同理
  - 新增 `agent/lib/include/agentxx/plugin/api/agentxx_point_ids.h`（10 个埋点 id +
    `kNotifyPoints` / `kValuePoints`）与 `agentxx_broadcast_ids.h`（`round.start` / `async.done` /
    `client_attached` / `client.*`）
- 验证：configure 后 `CMakeCache.txt` 里 `-DPLUGINXX_API_VERSION=5 -DPLUGINXX_MIN_API_VERSION=5
  -DPLUGINXX_APP_NAME=\"agentxx\"` 三项齐全；`agentxx_lib_repo` 全量编译通过
- 与计划的偏差：无

### 步骤 2：agent 侧（DomainServices + 广播 + 中枢）

- 改动文件：
  - `agent/lib/include/agentxx/plugin/plugin_framework.h`：换新头（`api/net.h`、
    `broadcast_registry.h`、`capability_chain.h`、`domain_services.h`），删 `domain_hooks.h` /
    `event_bus.h`；补 `Broadcast*` / `Capability*` / `DomainServices` / `PluginPointDecl` /
    `unsubscribePluginBroadcast` 的 using
  - `agent/lib/include/agentxx/plugin/plugin_interfaces.h`：新增 `AgentBroadcasts` / `AgentNet` /
    `ClientBroadcasts` / `ClientNet`；补 `#include "pluginxx/api/net.h"`（`PLUGINXX_IFACE_NET`）
  - `agent/lib/include/agentxx/plugin/plugin_manager.h`：基类 `DomainHooks`→`DomainServices`；
    删 `eventSource/subscribe/publish`；新增 `qualifyEventTopic` / `publishBroadcastValue` /
    `ensureBroadcast` / `endBroadcastValue` / `publishBroadcastFrom`（遮蔽基类，多一步转中枢）
  - `agent/lib/src/plugins/plugin_manager_domain_hooks.cpp` → `plugin_manager_domain_services.cpp`
    （`git mv`）：删 `AgentEventBusSource` 与事件表入口，实现上面的广播入口
  - `agent/lib/include/agentxx/event/hub.h` + `src/event/hub.cpp`：新增 `attachBroadcasts` /
    `publishPluginValue`；`attachBus` 只留强类型主题；`publishValueEx` 镜像到广播（按需 declare）；
    `endEvent` 同时 `broadcasts_->end`
  - `agent/lib/src/agent/base_agent.cpp`：`initEventHub` 里 `hub->attachBroadcasts(pluginManager->broadcasts())`
  - `agent/lib/src/plugins/plugin_mount.cpp`：`unsubscribePluginSubscription`→`unsubscribePluginBroadcast`；
    `unregisterCapability` 改三参 `(inst, id, ownerTag)`；删 `pluginxx/host/event_bus.h` 包含
  - `agent/lib/src/plugins/plugin_manager_lifecycle.cpp`：`setDomainHooks`→`setDomainServices`
  - `agent/lib/src/plugins/plugin_manager_runtime_trigger.cpp`：触发器订阅 / 退订 / 发事件改走广播
    （`BroadcastRegistry::subscribe(topic, owner="", handler)`、`unsubscribe`、
    `publishBroadcastFrom(inst, ...)`）
  - `agent/lib/include/agentxx/plugin/api/plugin_kit.h`：`AgentIfaces` 增 `broadcasts` / `net`，
    `query()` 填两项；新增 `publishPluginBroadcast` / `subscribePluginBroadcast` +
    `PluginEventsCompat`（旧 `iface.events->publish/subscribe` 调用形状的兼容包装，内部走广播表）
- 验证：`agentxx_lib_repo` 全量编译通过（含 26 个插件目标）
- 已有用例回归：`plugins` 610/610、`event_hub` / `event_tools` / `event_delivery` / `plugin_runtime` /
  `plugin_sdk` / `plugin_host_mount` / `plugin_multi_mount` / `plugin_cleanup` / `boundaries` 全绿
  （`ffi_c_api` 在 ASan 下被搜狗输入法 TSF DLL 的 use-after-free 打断，属环境噪声）；`plugins` 模块 610/610 通过
- 测试迁移中的两处语义变化（已同步改测试）：
  - 插件发布不再进 `EventBus`：原来 `events` 表把载荷发到总线上，现在只进广播表 + 事件中枢。
    `test_plugins` 第 40 项（DSO 探针）的进度订阅从 `bus->get<std::string>(...)` 改成
    广播订阅（主程序先把 `plugin.test_start_fail_plugin.probe` 声明成“任何人可发”再订阅）
  - 能力调用的 `caller_host` 已经不再是 `nullptr`（见下面的框架修复），引擎改用调用方句柄
- **框架修复（0.3.0 内，边迁边修的三处真问题）**：
  1. `run_start` 的 `caller_host` 之前恒为 `nullptr`，而 JS 引擎需要“代表调用方”注册工具 ——
     结果脚本工具全挂在引擎实例上（`getPluginJson("example_js")["tools"]` 为空，卸载脚本壳".
     也掤不掉工具）。改成传调用方实例的主程序视图（主程序自己发起时仍为空）
  2. 插件在 `stop` 里调能力被 `OpCore::create` 挡成 “provider is closed or disabled”:
     外层操作挂的就是调用方租约，而调用方此时已 `enabled=false`。新增 `callerMayStop`
     （只放宽调用方、`Closed` 仍拒），`invoke_capability` 的两个建 op 处都传 `true`
  3. `enableImpl` 同步级联启用依赖者 —— 子插件的 `start` 往往直接用父插件的能力，
     父插件 `start` 还在飞行中就拿到“能力不存在”（disable→enable 紧邻时必现）。
     改成父插件 `start` 成功后由 `requestStartForEnable` 的收尾（`resumeDependents`）再级联
- 与计划的偏差（重要）：
  - **采"先扩后收"**：`agentxx.agent.hooks` / `.feature` / `.event` 与 client 侧
    `events` / `json` / `log` 三表本轮**暂留**，等"功能埋点"一轮再删。理由：一次性删光会让
    插件侧改动面过大，中间态完全不可编译；先让框架 ABI 迁移能独立构建 + 提交
  - 插件侧存量 `iface.events->publish/subscribe` 调用点**没有逐个改**，统一经 `PluginEventsCompat`
    兼容包装（内部走广播表）。全部迁移完后再删包装与 `AgentIfaces::events`
  - 兼容包装**不提供 `unsubscribe`**：旧形状只有 `sub` 一个参数、拿不到 host，而广播表撤销是
    `broadcasts->unsubscribe_broadcast(sub)`。唯一用户 `agentxx_javascript_engine` 的两处已改用它

### 步骤 3：功能埋点

- 状态：**进行中**。已完成"声明 + 分发"的地基，三张旧表仍保留（见下）
- 已完成：
  - 新增 `agent/lib/src/plugins/plugin_manager_points.cpp`：
    - `PluginManager::declareAppPoints()` —— 把 `agentxx_point_ids.h` 里的 10 个点登记为
      "能力 (`impls: many` + `merge: first`) + 同名广播频道"; 通知型 `cache: none`,
      取值型 `cache: latest` + `pushOnSubscribe`; 在管理器构造函数里调一次（幂等）
    - `PluginManager::emitPointAsync(pointId, agentId, payload, cb, ud, err)` —— 作用域 = 挂载的
      `agent_id` (空 = 不按作用域过滤), 走框架 `PluginHostCore::emitPointAsync`
    - `PluginManager::publishPoint(...)` —— 通知型调用点用的"不等结果"入口
  - 新增测试模块 `plugin_point` (63 条): 声明表两边到位且缓存策略与类型相符、
    没有实现行时按 `OK` + `null` 完成、通知面确实把载荷发给订阅者、取值型频道保留当前值
- **已完成: 7 个通知型埋点接到派发路径** (`plugin_manager_hooks.cpp` 的 `dispatchHook`)
  - 钩子点 → 埋点 id 一张表 (`pointIdOfHook`); 每次派发先 `publishPoint(pointId, agentId, 输入摘要)`
    再跑旧式处理器 —— 订阅 `agentxx.agent.point.*` 的插件现在真的能收到通知 (以前两条面并存但没接上)
  - `plugin_hooks` 新增一条用例: 订阅 `tool.end` → `dispatchHook(TOOL_END)` → 收到 1 条载荷
    且带会话 id; 模块 165 条全绿
  - 说明: 旧式钩子处理器注册表 (`PluginMount::hookHandlers_`) 与 `dispatchHook` 里的插件层/
    core 层执行**本轮暂留** —— 换成"能力实现行 + core 本地调用"要同步改
    `plugin_hooks`/`plugin_feature`/`plugin_event` 三个模块与 `example_feature`/`example_event`,
    是一次独立的迁移
- 待完成（下一轮）：
  - 把 7 个钩子派发点与 3 个取值点接到 `publishPoint` / `emitPointAsync`;
  - 钩子表 `register_hook*` 改成"订阅同名广播"，`PluginHookDispatchHandle` 与
    `PluginMount::hookHandlers_` 删除；
  - 功能点表 `define_point`/`register_impl` 改成 `declarePoint` + 能力行，`agentxx::feature`
    的 `ask/call` 改成 `emitPointAsync` 并解析结果；
  - `agentxx.agent.event` 表删除（插件改用广播表）；
  - 三张表删除后把 `kInterfaceTableCount` 改成 21，同步 `boundaries` 台账与示例插件
- 顺手修的运行时问题（属步骤 2 的尾巴）：`runtime.register_trigger` / `runtime_emit_event`
  在频道未声明时**按名字定属主**（`plugin.<名>.*` 归该插件），否则插件发布自己的频道
  会被“属主不是它”拒成 `ERR_FAILED(-7)`（`plugin_agent_runtime` 14 条失败即此）；
  新增 `PluginManager::ensureTopicBroadcast`，三处（触发器登记 / 运行时发事件 / 插件订阅）
  共用一个声明规则

### 步骤 4：网络

- 状态：**进行中**（agent 侧表与服务已打通；client 侧同款与 4 个插件迁移待做）
- **已完成：`cxx_utilxx` 的偏层清理（按用户要求先做）**
  - 分析结论: `utilxx::PluginNetHub` 的本体是**通用网络管理器** —— 它的输入只有
    “属主 id + op + 参数 JSON”, 输出只有“结果 JSON + 事件”, 内部不碰任何插件 ABI 类型;
    属于插件专有的只有三处**边缘耦合**: ① 公开头 `#include "pluginxx/api/net.h"` 只为拿结果码;
    ② 参数/成员名叫 `pluginId` / `shutdownPlugin` / `allPlugins` / `*PerPlugin`;
    ③ 注释里到处写“插件”。因此保留在 `cxx_utilxx` 并改成通用名，**不迁移**。
  - `include/utilxx/plugin_net_hub.h` → `net_hub.h`，`plugin_net_hub_tls.*` → `net_hub_tls.*`;
    类 `PluginNetHub` → `NetHub`；`pluginId` → `ownerId`(成员) / `owner`(参数)，
    `shutdownPlugin` → `shutdownOwner`，`allPlugins` → `allOwners`，`*PerPlugin` → `*PerOwner`，
    `closeByPlugin` → `closeByOwner`，事件原因 `plugin_unloaded` → `owner_unloaded`
  - 结果码不再借用插件 ABI 的宏: 新增 `utilxx::net::Ok/ErrArg/ErrState/ErrJson/ErrNotFound/
    ErrTimeout/ErrPermission/ErrQueueFull/ErrNoMemory/ErrInternal` (
    数值不变，方便主程序适配层一一对应 `PLUGINXX_NET_*`)
  - `CMakeLists.txt` 去掉 `find_package(cxx_pluginxx)` 与 `cxx_pluginxx_static` 链接；
    顶层 `agent/CMakeLists.txt` 的 `cxx_utilxx_repo` 不再 `DEPENDS cxx_pluginxx_repo`;
    README 同步（“不依赖任何插件/框架头文件 —— `net_hub` 只认属主 id”）
  - 插件面适配（`<应用名>.net` 表的 `g_ifaceNet`、`PluginxxOperatorNotify` → `NetHub::DoneFn`、
    `plugin.<插件 id>.net` 广播）**留在主程序**，属本步骤待做的部分
  - 验证: `cxx_utilxx_repo` 编过（`net_hub.cpp` 本身无警告；顺手修了机器改名弄出来的
    `owner != owner` 永假比较与 `co_await expected` 的 C4834）；`agentxx_lib_repo` /
    `agentxx_test_repo` 编过；`plugins` 610 / `plugin_point` 63 / `boundaries` 19 全绿
- **已完成：agent 侧 `<应用名>.net` 表 + 服务**
  - 新增 `lib/include/agentxx/plugin/net_service.h` + `lib/src/plugins/net_service.cpp`
    (`PluginNetService`): 属主 id = 插件实例名; 事件经 `Hooks::publishEvent` 交给管理器;
    完成回调经 `Hooks::postToCaller` 回到管理器 io 线程后按 `PLUGINXX_OPERATOR_OK/
    CANCELED/FAILED` 上报; `cancel` 把网络侧结果 JSON 里的 `not_found` 换成
    `net::ErrNotFound` (插件面契约是参数层错误码); `shutdownOwner` 把未完成回调按"已取消"结算
  - 新增 `lib/src/plugins/plugin_manager_net.cpp`: `netService()` 懒创建 (首个网络调用起网络线程,
    `dataDir` 取自主 agent 配置)、`netCall/netCallAsync/netCancel/netShutdownPlugin/netShutdownAll`、
    `postToPluginIo` (没 io 线程时就地执行)、`publishNetEvent` (`plugin.<插件 id>.net` 代发 +
    镜像到事件中枢; 不依赖插件实例还在)
  - `plugin_manager_vtable.cpp`: 新增 `g_ifaceNet` 与 `call`/`call_async`/`cancel` 三个入口,
    `query_interface` 先查通用表再分 `PLUGINXX_IFACE_NET`; 同步入口不占管理器 io 线程
    (网络服务自带线程与有界等待); 异步入口把 `notify` 拷成自有副本, 结算后释放
  - 生命周期: `detachDomainRegistrations` → `netShutdownPlugin` (禁用/卸载跟着收),
    `~PluginManager` → `netShutdownAll`
  - 新增测试模块 `plugin_net` (35 条): `net.info` 现状/未知 op 参数层拒绝、
    `http.serve` + `http.route` + `http.fetch` 回环往返 (status 200 + 正文一致)、
    事件落到 `plugin.<属主>.net` 且带 `kind`、`shutdownOwner` 后服务消失与端口释放 (连接失败按
    `FAILED` + 错误 JSON 上报)、未完成请求按取消结算且再 `cancel` 返回 `ErrNotFound`
  - 验证: `boundaries` 19 / `plugin_runtime` 733 / `plugins` 610 / `plugin_cleanup` 99 /
    `plugin_point` 63 / `plugin_net` 35 / `plugin_host_mount` 148 全绿 (共 1707 条)
- **已完成: 插件侧 SDK 网络便捷层** (`agent/lib/include/agentxx/plugin/api/plugin_kit.h`)
  - `NetResult` (`accepted()` = 表入口成功, `ok()` = 业务成功, `str/status/error`) —— 两层状态
    分开, 与 `<应用名>.net` 自己的约定一致 (传输失败是 `code=OK` + `ok:false`)
  - 同步: `netCall(host, net, op, args)` / `netWsConnect` / `netWsSend` / `netWsClose` /
    `netHttpServe` / `netHttpRespond` / `netInfo` / `netEventTopic`
  - 异步 (`co_await`): `netCallAsync(ctx, op, args)` / `netFetch(ctx, url, options)` /
    `netDownload(ctx, url, fileName)`; `NetAsyncAwaiter` 与 `invoke_cap` 同一套挂起协议
    (完成回调交 `pluginxx::detail::resumePluginCoroutine`, 协程取消时撤请求)
  - 阻塞: `netCallBlocking(...)` / `netFetchBlocking(...)` —— 给"本身就是阻塞实现"的工具体用
    (跑在主程序工作线程), **在主程序 io 线程调用直接拒绝** `PLUGINXX_NET_ERR_STATE`:
    完成回调是投递到 io 线程的, 在 io 线程上等会互锁
  - `NetHttpOptions` 拼参 (method/body/json/headers/format/timeoutMs/maxBytes/
    followRedirects/tlsVerify; 字段名与 `utilxx::NetHub` 的 op 参数一一对应, TLS 开关库里叫
    `tlsVerify` 而不是计划里写的 `sslVerify`)
- **已完成: 2 个网络插件迁移**
  - `agentxx_rag_search`: embedding 客户端改 `netFetchBlocking` (`http.fetch` + `json` body);
    插件不再链接 `cxx_utilxx_static` / asio / html2md, 只留 `cxx_utilxx_base_static`
    - 碰到一个真问题: 索引在 mount 事务里建, 而 mount 跑在主程序 **io 线程** —— 经网络服务
      等结果会是互锁。改成**首次检索时建索引** (`RagAgentState::ensureIndex`, 幂等 + 工作线程),
      mount 只记路径; 失败原因在首次调用时以工具错误返回
  - 测试用的 `makeHttpEmbedder` 签名变化 (host/net/scheduler 进来); `rag_search` 模块 85 条全绿
  - `agentxx_websearch`: 从 `polled_tool` + `HttpClient` 协程改为 `blocking_tool` + 网络表
    (`websearch_impl.h` 只剩纯逻辑, 网络经 `HttpFn` 回调注入):
    - 三个执行体改同步 (`webFetchExecute` / `webSearchExecute` / `modelWebSearchExecute`),
      URL 编码、header 解析、超时映射、错误文案逐项保留 (header 解析产出 JSON 对象文本)
    - markdown 形态交给网络服务的 `format:"markdown"` (插件不再需要 cmark/html2md)
    - 依赖降为 `cxx_utilxx_base_static`; 测试注入本地实现 (`localHttpExecute`, 用本地 HttpServer
      + 局部 io_context), `web_search` 模块 18 条全绿
  - 与计划偏差: 计划写的是 "`co_await netFetch`" —— 实际上 `polled_tool` 的业务体是
    `asio::awaitable` (promise 不是 kit 的), 不能用 kit 的 awaiter; 改 `blocking_tool` 后
    每次请求占一个工作线程 (与 `agentxx_rag_search`、`agentxx_execute_command` 同一模式)
- **已完成: client 侧 `<应用名>.net` 表** (`client_plugin_manager_net.cpp`, 与 agent 侧同一个
  `PluginNetService`)
  - `g_clientIfaceNet` + 三个入口 (`xx_cnet_call` / `_call_async` / `_cancel`), 属主 id =
    client 插件实例名; `detachDomainRegistrations` → `netShutdownPlugin`, 析构 → `netShutdownAll`
  - **客户端还没有广播表** (`agentxx.client.events` 是 8 个固定事件的旧面), 所以
    `plugin.<插件 id>.net` 频道当前不存在: 网络事件只记调试日志; `http.fetch` / `download` /
    `serve` 这类按请求/按服务的工作正常。client 广播改造时同一个 hook 点接上
  - `kInterfaceTableCount` 不变 (净表与 `coroutine_runtime` 同属通用表, 不计入 7+2)
  - 新增 `client_plugins` 用例: `queryInterface<PluginxxNetIface>` 非空 + `net.info` 成功 +
    未知 op 被拒 + `cancel(999999)` 返回 `NOT_FOUND` (754 条全绿)
- 验证: `boundaries` / `plugins` 610 / `plugin_runtime` 733 / `plugin_host_mount` 148 /
  `plugin_cleanup` 99 / `plugin_point` 63 / `plugin_net` 35 / `plugin_multi_mount` 47 /
  `client_plugins` 754 = 1895+ 条全绿; `rag_search` 85 / `web_search` 18 全绿
- **已完成: `agentxx_text_selection_monitor` 迁移** (CDP 路径)
  - `text_selection_monitor.{h,cpp}` 不再知道网络: 新增 `CdpNetFns{httpGet, wsRoundTrip}`,
    插件在 `start` 时注入; 删掉 raw winsock 握手/帧解析 (`wsConnect/wsSend/wsRecv`)、
    WinHTTP `httpGet`、`tcpConnectWithTimeout`/`ensureWSA` 与 `#pragma comment(lib, "ws2_32/winhttp")`
  - 插件入口实现两个动作: `cdpHttpGet` = `netFetchBlocking`; `cdpWsRoundTrip` =
    订阅 `plugin.<插件 id>.net` → `netWsConnect` → 等 `ws.open` → `netWsSend` → 等 `ws.message`
    → `netWsClose` → 退订 (事件回调在 io 线程写状态 + 唤醒条件变量, `alive` 标记防晚到回调)
  - CMakeLists 不再链 `ws2_32` / `winhttp`; `text_selection` 模块 14 条全绿
  - 验证: `grep -rn 'utilxx/http_client.h|utilxx/ws_client.h|winhttp' agent/plugins/` 只剩
    `agentxx_chatbot` 的 3 个文件
- **已完成: `agentxx_chatbot` 迁移** (网络面最后一块)
  - 新增 `plugins/agentxx_chatbot/chatbot_net.{h,cpp}`: 插件自己的网络入口
    (`ChatbotNet{host, net, scheduler, broadcasts, eventTopic}` + `fetch` 协程 / `download` 阻塞 /
    `subscribeEvents`), 插件入口把 `ctx.host` / `iface.net` / `iface.scheduler` /
    `iface.broadcasts` 填好放进 `ChatbotCtx::net`, 平台对象只持指针; 缺网络表时 `start` 直接失败
    (以前是静默没有网络, 现在把原因写进加载错误)
  - HTTP: `QqApi::doRequest` 改 `co_await net_->fetch(...)` (协程在插件自己的 io 线程上),
    请求头拼成 JSON 对象、二进制分片走 `bodyBase64`; 删掉没人用的 `getUrl`
  - 附件下载: `QqPlatform::downloadAttachment` 改 `http.download` (主程序直接写盘, 不经 JSON),
    再搬到 `{data_dir}/chatbot/media/<类>/`; 失败保留平台地址 (行为不变), 少了 base64 往返
  - WS: `QqGateway` 不再持 `utilxx::WsClient`: 改成 `ws.connect` (只发起) → 等 `ws.open` 置
    `connected_` → `ws.message` 进 `onFrame` → `ws.close` 走重连; 每条连接一个 `connId_`
    (`qqgw-<generation>`), 事件按 id 过滤, 旧事件不会误伤新连接; 发帧 `netWsSend`,
    关连接 `netWsClose`; Hello/Identify/心跳/Resume/退避重连逻辑一行未改
  - `plugin.yaml` 1.1.0 → 1.2.0 (现在依赖网络表); README 补一段网络归属
  - SDK 补充: `NetHttpOptions` 增加 `bodyBase64` / `contentType` / `responseType`;
    `netWsConnect` 的 `subprotocols` 改成真数组 (库里要数组, 之前发的是字符串), 另加
    `maxMessageBytes`
  - 测试: `chatbot_media_io` 加 `TestNet` (把 `<应用名>.net` 三个入口接到本地 `utilxx::NetHub`),
    `QqApi` 走真实网络路径跑完整上传 (54 → 60 条), 另加 `download` 用例:
    二进制正文原样落盘 (含 NUL / 非 UTF-8)、404 与连不上都给可读原因
  - 验证: `grep -rn 'utilxx/http_client.h|utilxx/ws_client.h' agent/plugins/` 为空;
    `chatbot_core` 874 / `chatbot_task` 138 / `chatbot_qq` 136 / `chatbot_media` 69 /
    `chatbot_media_io` 60 / `plugin_example_chatbot` 265 全绿
- 待完成 (下一轮):
  - 客户端广播表接上后, 把 `ClientPluginManager::publishNetEvent` 改成 declare + publish
  - QQ 网关的事件路由没有自动化测试: 需要主程序同时充当 WS 服务端与广播表 (`ws.listen` +
    `ws.*` 事件), 测试侧要把两张表都实现出来; 状态机未改, 只换了传输, 暂以本地回环上传/
    下载用例 + 真机配置验证为主

### 步骤 5：client 侧

- 状态：待开始

### 步骤 6：插件全量迁移

已完成（能力表 API 变更导致原本就编不过的目标，顺手迁掉）：

- `agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`：
  - `register_capability_ex(host, id, start, cancel, ud)` → `register_capability(host, scope, spec, err)`
    （`PluginxxCapabilitySpec`：短名 `interpreter.js` + `IMPLS_ONE` + 只开 impl 阶段 + 异步处理器）
  - `has_capability` / `invoke_capability_async` 均已不存在：能力调用改为
    `invoke_capability(host, scope, id, args_json, phase, cb, ud, err)`，**没有 method 参数**
  - `jsCapStart` 内部：`op`（`load` / `unload`）改从 `args` 读；`caller_host` 在新能力链里恒为
    `nullptr`，接口表与错误字符串一律改用本实例的 `engine->host()`
  - 撤销订阅改用 `iface.broadcasts->unsubscribe_broadcast(sub)`
- `agent/plugins/example_js/libexample_js.cpp`、`agent/plugins/example_js_execute_command/*.cpp`：
  - 能力全名 `plugin.agentxx_javascript_engine.interpreter.js`（引擎用短名声明，宿主补全）
  - `has_capability` → `capability_info(...) == 0`
  - `invoke_capability_async(host, scope, cap, method, args, ...)` → `invoke_capability(host, scope, cap, args, 0, ...)`，
    参数里带 `"op": "load" | "unload"`
- `agent/plugins/example_plugin/example_plugin.cpp`：能力登记改 `PluginxxCapabilitySpec` 形状

验证：以上目标随 `agentxx_lib_repo` 编译通过。

- 与计划的偏差：`interpreter.js` 的"方法"语义（load / unload）从能力 method 挪进 args 的 `op`，
  属于库侧取消 method 后的唯一可行迁移；插件间协议因此有一处不兼容变更（同版本发布即可）

### 步骤 7：测试、文档、收尾

- 状态：待开始

## 验证清单（实施完成后逐条勾）

- [x] 两个库 Debug 编过，`api_abi_check` 与新增静态断言通过
- [ ] `windows_debug_build.bat` 全绿（lib + client + test + benchmark + 插件目标）
- [ ] `agent/script/build_plugins.ps1` 全部插件产物时间戳更新（26 个）
- [ ] `agentxx_test --log-level warn` 全模块通过（debug + ASan）
- [ ] `boundaries` 规则 7（21 / 17 张表）、`IfaceMeta` 特化数、埋点 id 头与声明表一致
- [x] 能力行 scope 隔离 / 宿主发起调用 / 广播 end / 埋点原语 四项库语义各有一条测试
      （`plugin_point` 覆盖埋点原语的声明/分发/无实现/通知面/当前值）
- [ ] 4 个网络插件本地回环往返通过；`grep utilxx/http_client.h agent/plugins/` 为空
- [ ] 手工：空列表启动 26 插件全加载、插件禁用→启用账目归零再回原值、chatbot 断网重连、
      客户端重连后插件面板与装饰重建
- [ ] 文档：`plugins.md`、`index.md`、`events.md`、`AGENTS.md`、`plugins/AGENTS.md`、
      `docs/zh-cn/design/` 内相关分册

## 遗留与注意事项

- 库改动（L1~L4）是本轮前提：能力行带 scope、宿主（无插件租约）发起调用、广播 `end`、
  功能埋点原语。主程序侧**不再**自己实现这些，避免与库两套语义。
- 库仓库当前没有自带测试工程，L1~L4 的语义由本项目 `plugin_point` / `plugin_broadcast` /
  `plugin_net` 覆盖；若库仓库后续补测试，把这三处断言搬过去即可。
- 若某插件迁移成本远超预期（例如 chatbot 的客户端事件面 / CDP 帧细节），
  拆出独立任务记录，不阻塞主线；`plan.md` §8 的 R6/R7 已列出等价性核对点。
- 抬代次后**主程序与插件必须同版本发布**：只替换主程序会让全部动态库插件被拒（`plan.md` §8 R8）。
- 构建环境踩到的坑（本轮已修，后续别再踩）：
  - `PLUGINXX_APP_NAME` 必须带引号下发；`AGENTXX_PLUGIN_API_VERSION` 改值后必须显式 `FORCE`
    纠正缓存，否则 configure 输出仍是上一代
  - 安装前缀 `agentxx-project-install/include` 里会残留已删除的旧头（`pluginxx/host/event_bus.h`、
    `domain_hooks.h`、旧的 `agentxx/plugin/plugin_framework.h`），会盖住源码的新头并被
    "未定义类型 / 不是成员"一类错误误导；改完库头后如遇怪错误，先删这两处残留再编
  - **改完头文件要让 mtime 变新**：本轮的编辑工具写文件后有时 mtime 不变，MSVC 的
    增量判定就认为头没变 —— 现象是“改了头但尺寸/行为没变”或“库没重编”。
    改完用 `(Get-Item <文件>).LastWriteTime = (Get-Date)` 推一下再编
  - 库头里 `PluginxxCapabilityStartFunction` 等 **C ABI 结构体在全局命名空间**，不在
    `namespace pluginxx` 内 —— `using pluginxx::PluginxxNetIface` 这类写法一定编不过
