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
| 步骤 3 功能埋点（替换钩子与功能点） | 已完成（删 `agentxx.agent.event` / `agentxx.agent.feature` / `agentxx.agent.hooks` 三张表；通知型扩展点只剩“订阅同名广播”，见步骤 3-A/3-2/3-3） |
| 步骤 4 网络（`agentxx.net` + 4 个插件迁移） | 已完成（agent/client 双侧表 + SDK 便捷层 + 4 个插件全迁：websearch / rag_search / text_selection_monitor / chatbot） |
| 步骤 5 client 侧（广播 + DomainServices） | 已完成（17 张表 = 10 框架通用 + `net` + 6 自有；`agentxx.client.*` 频道 7 条；实例内订阅簿记与三张自有表删除） |
| 步骤 6 插件全量迁移 + JS 引擎 | 已完成（26 个插件全部重编；`PluginEventsCompat` 与 `iface.events` 删除，发布/订阅只剩广播表） |
| 步骤 7 测试、文档、收尾 | 已完成（全量构建 + 全模块测试 + `plugins.md`/`index.md`/`events.md`/`AGENTS.md`/英文分册同步） |

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
  - `agentxx.agent.event` 表删除（插件改用广播表）—— **已完成，见下**；
  - 三张表删除后把 `kInterfaceTableCount` 改成 21，同步 `boundaries` 台账与示例插件

#### 3-A 删 `agentxx.agent.event`（已完成）

- 接口表与实现：
  - `plugin/api/plugin_api.h` 删整段事件中枢表（IID/版本/基线/返回码/发布掩码/种类/等待模式/
    `AgentxxPluginEventSpec`/`WaitSpec`/`Iface` 共 ~200 行）；`plugin_spec.h` 删 `EventSpec`
    基线特化；`plugin_iface_meta.h` 删该表的 `IfaceMeta` 与两条 `static_assert`
  - `plugin_manager_vtable.cpp` 删 `xx_event_*`9 个入口 + `g_ifaceEvent` + `query_interface`
    分支（注意：`g_ifaceRuntime` 原本紧跟在事件段后面，删段时一并删了，已补回）
  - `plugin_mount.h`：`RegistrationInventory` 去掉 `eventRecords` / `eventWaits`；
    `plugin_manager_lifecycle.cpp` 的汇总与 `plugin_manager_adapters.cpp` 的统计同步删；
    `plugin_mount.cpp` 的 `revokeEntry` 去掉“按 owner 删中枢记录”的最后一步
  - SDK `plugin_kit.h`：删 `AgentIfaces::event` 与 `hasEventHub`/`defineEvent`/`emitEventValue`/
    `endEvent`/`removeEvent`/`queryEvent`/`listEvents`/`registerEventWait`/`cancelEventWait`/
    `listEventWaits` + 两个选项结构（`detail::takeHostString` 被一起删掉，已补回：其他查询
    辅助还在用它）
- **新增的回收动作**：`PluginManager::detachDomainRegistrations` 里按属主删频道
  （`BroadcastRegistry::removeChannelsOwnedBy`）—— 之前库里实现了但没人调，插件禁用/
  卸载后频道一直留着（订阅方等一个永不更新的值）。现在与订阅回收一起做，启用时插件
  在自己的 `mount` 里重新声明
- 示例插件 `example_event` 2.0.0：改为广播示例（声明两条自己的频道、发布初始值、读回
  当前值、订阅自己的进度频道 + 主程序 `agentxx.agent.round.start`），`plugin.yaml` 的
  依赖从 `agentxx.agent.event` 改成 `agentxx.broadcasts`
- 测试：删 `test_plugin_event.{h,cpp}`（124 条，测的是已删的表）→ 新增
  `test_plugin_broadcast.{h,cpp}`（模块名 `plugin_broadcast`，73 条）：声明与发布、
  发布权限、订阅与读（`pushOnSubscribe` / 退订 / 无缓存 / 未声明）、结束流
  （`on_end` 恰好一次、幂等、再声明开新流）、禁用→启用→卸载的可逆性
- 台账与文档：`kInterfaceTableCount` 23→**22**（10 + 12），`boundaries` 规则 7/8/9/10 的
  数量与成员顺序台账同步；`plugins.md` §8 表清单去掉事件行 + 新增广播行、§20 重写为
  “插件通知面：广播”、§19 的“与事件中枢的关系”段改写；`events.md` §12 重写（插件侧只有
  广播与触发器两条路）、测试模块表同步；`test-modules.md`、`index.md`、根 `AGENTS.md`
  的数量与表清单同步
- 验证：`agentxx_lib_repo` / `agentxx_test_repo` / `agentxx_client_repo` 编过（含 26 个插件
  目标，`example_event` 产物刷新）；`plugin_broadcast` 73 / `boundaries` 19 / `plugin_point` 63 /
  `plugin_net` 35 / `plugin_host_mount` 147 / `plugin_multi_mount` 47 / `plugin_runtime` 733 /
  `plugins` 610 / `plugin_sdk` 141 / `plugin_cleanup` 99 / `plugin_feature` 145 /
  `feature_points` 136 全绿
- 已知：`plugin_agent_runtime` 在本机 Debug+ASan 下没有得到结论（跑 20 分钟以上仍未到结尾，stderr
  在固定的同一处停下、stdout 因重定向被缓冲看不到进度；不是本次改动新增的模块，
  上一轮回归也没包含它）；下一轮请用控制台直跑（不加管道）并给足超时再确认
- 后续事项（步骤 5 收尾时做）：客户端侧补上广播表后，把 `example_event/plugin.yaml`
  的 `agentxx.broadcasts` 加回 `require`（现在只靠代码里的“表缺失就空转”降级，
  因为客户端接口协商在步骤 5 之前没有这张表，声明成 require 会让它的客户端侧检查失败）

- **步骤 3-2 已完成：删 `agentxx.agent.feature` 表，实现行 = 能力行**
  - 库：`pluginxx::capability()` 增加 `CapabilityOptions`（impls/merge/phases/phase/priority/
    timeoutMs/ownerTag/scope）与 `error` 出参，返回登记结果码（原来恒为 void 且丢掉错误）；
    老的调用形状不变（默认值 = 声明自己的名字 + 单行覆盖 + 只开 impl 阶段）
  - SDK：`plugin_kit.h` 的“功能扩展点”段改为两件东西 —— `PointImplOptions`（优先级 / 超时 /
    ownerTag）与 `providePoint(ctx, ref, pointId, fn, opts, error)`：把 `(ctx, 载荷)` 形态的
    实现体适配成能力表的三参处理器，登记为 `impls: many` + `merge: first` + impl 阶段，
    **带本次挂载的作用域**（行只对这个 agent 生效）；`pointAnswer()` 保持不变
  - 主程序：`PluginManager::emitPoint(agentId, pointId, payload)`（`asio::awaitable`，
    用 `asio::async_initiate` + `use_awaitable`，完成回调自己释放状态）；
    `feature::PointBase` 新增 `pluginPointId`（库内点 id 与插件面扩展点 id 的配对）与
    `PluginEmitFn` 注入点，`runChain` = 插件层 (`runPluginLayer`) → 本点自登记的实现
    (`runLocalImpls`) → 核心实现
  - 点 id 配对：`agentxx.agent.context.countTokens`/`.summarize` 两个库内点分别对应插件面
    `agentxx.agent.point.*`（`registerContextPoints` 里 `setPluginPointId`）
  - 插件层载荷：`{"point": <扩展点 id>, "args": <argsJson>, "request": <requestJson>}`，
    另一份缺省时不出现；回答解析保留 `{"disable":true}` / `{"value":...}` / 整份当值
  - 删表：`plugin_api.h` 的 feature 整段（IID/版本/基线/`FeaturePointSpec`/`FeatureImplSpec`/
    `Iface`）、`plugin_iface_meta.h` 特化、`plugin_spec.h` 基线、`plugin_manager_feature.cpp`
    （`git rm`）、vtable 的 6 个成员与 query 分支、`PluginMount` 的 feature 记账、
    统一注册清单的 `featurePoints/featureImpls`、`Registry::definePluginPoint/undefinePluginPoint`
    （`removePointsOwnedBy` 保留：按 `origin` 撤销点，单测用）
  - 示例：`example_feature` 2.0.0 改为“声明并实现自己的扩展点 + 给主程序点登记实现行”，
    `plugin.yaml` 只 require `agentxx.agent.core`（旧的 `agentxx.agent.feature` 没了）
  - 测试：`plugin_feature` 重写为 34 条（实现行随挂载生效 / 禁用回落核心实现 / 启用重新登记 /
    卸载回基线 / 自己的扩展点同步与异步行 / 未声明的点按没意见返回 / 核心实现不进能力调用链）；
    `feature_points` 的插件点用例改成“为主程序点登记实现 + 按归属撤销”；
    `boundaries` 21 张（10 通用 + 11 业务）、合计 30、台账删 `AgentxxPluginFeatureIface`；
    `plugin_host_mount` 21 张、`plugin_cleanup`/`plugin_runtime` 删 feature 计数与 sizeof
  - 文档：`plugins.md`（21 张 / 11 业务 / 表格删 feature 行 / 架构图 21）、`index.md`（21 张 +
    业务表清单去 feature）、根 `AGENTS.md`（21 张 / 11 业务）
  - 验证：lib/plugins/test 全编过（26 个插件目标重建）；`boundaries` 19、`plugin_feature` 34、
    `plugin_point` 63、`feature_points` 129、`plugins` 610、`plugin_host_mount` 146、
    `plugin_multi_mount` 47、`plugin_cleanup` 93、`plugin_sdk` 141、`plugin_runtime` 731、
    `client_plugins` 754、`summarization` 454、`observability` 114、`event_hub` 268、
    `event_tools` 97、`event_delivery` 48 全绿
  - 踩到的坑（记录下来免得下次再踩）：
    - 用行区间删除 vtable 里的大段时把 `xx_get_prompt` 与 8 张表的 const 定义一起切掉了 ——
      已用 `git checkout` 恢复后按锚点重删；批量删除后必须重编整个 lib 目标确认
    - `asio::awaitable` 的 `await_transform` 不接受自定义 awaiter：等 C 回调要写
      `asio::async_initiate<use_awaitable_t<>, void(结果)>(initiation, token)`，且 token 必须
      是非 const 变量（`const` 令牌是左值，`async_initiate` 第二参要求非 const 引用）
    - `PointBase` 的 `id_` 与插件面扩展点 id 不是一个字符串（`agentxx.agent.context.*` vs
      `agentxx.agent.point.*`），必须显式配对，否则插件登记的实现行永远查不到
    - 能力表的处理器固定收 `(ctx, method, args)`：两参 lambda 会被 `invokeCap` 判成
      “不可调用”并退到 `fn(method, args)` 分支报 C2664，SDK 侧要用适配器统一成三参
    - 统一注册清单的 `capabilities` 计数只统计主程序侧登记记录（框架能力表里直接登记的
      实现行不计），断言别指望它反映实现行条数
    - `boundaries` 规则 15 会拦停用词（`埋点`/`宿主`/`链路`/`领域`），注释里也要改成
      “扩展点 / 主程序 / 流程 / 业务”

- **步骤 3-3 已完成：删 `agentxx.agent.hooks` 表，通知型扩展点只剩“订阅同名广播”**
  - 表与实现：`plugin_api.h` 删 hooks 整段（IID / 版本 / 基线 / `AgentxxPluginHookPoint` /
    `AgentxxPluginHookSpec` / `SpecEx` / `Iface`）、`plugin_iface_meta.h` 特化、`plugin_spec.h` 基线；
    `plugin_manager_vtable.cpp` 删 `xx_register_hook` / `xx_unregister_hook` / `xx_list_hooks` 与 query 分支
  - `plugin_manager_hooks.cpp` → `plugin_manager_dispatch.cpp`（重写）：7 个点各一个中间件回调，
    `dispatchHook(HookPoint, NodeInput)` 只 `publishPoint(点 id, agentId, {agentId, sessionId, point})`
    （开发者模式才记次数/耗时）；`hooksJson()` 给点清单；`ensureHookDispatch()` 挂句柄 `plugin_points`、
    `detachHookDispatch()` 摘掉
  - 派发器归属说清：属于**挂载**而不是插件 —— `mountAgentAsync` 建挂载时挂上，
    `unmountAgentAsync` / `unmountAllAgentsAsync` 与挂载析构时摘掉；插件禁用/启用/卸载/轮末清理
    都不动它（`flushPendingCleanup` 只清排队中的清理）
  - 删注册表与记录：`PluginMount` 的 `HookHandlerView` / `HookHandlerEntry`、
    `registerHook/unregisterHook/addCoreHookHandler/removeCoreHookHandler/handlersOf/orderedHandlers/...`、
    `hookHandlers_`、`Entry::hookHandles`、`RegistrationInventory::hooks`；
    `PluginManager::handlersOf/addCoreHookHandler/removeCoreHookHandler`、
    `PluginInstance::allHookHandles`、快照的 `hook_count` / `hook_priorities`
  - SDK：`plugin_kit.h` 删 `AgentIfaces::hooks` 与 `HookOptions` / `hook` / `hookEx` / `unhook` /
    `unhookEx` / `listHooks`（含 `HookShim` / `HookJob`），插件改成订阅频道
  - JS 引擎：`onHook`/`offHook` 改成订阅/退订同名广播（`hookPointIdOf` 0..6 映射点 id），
    `JsHookBinding` 变成 `{engine, plugin, topic}`
  - 示例：`example_plugin` 订阅 `points::ToolEnd`（第三条订阅），`example_feature` 的演示
    从“登记钩子”改成订阅 `points::TurnStart`，`plugin.yaml` 去掉 `agentxx.agent.hooks` 依赖
  - 快照与诊断：`assembly_snapshot` 段名 `hooks` → `hook_points`，渲染改为
    `hookPoints[N] devMode=...`（非开发者模式只有一行标题）；`diagnostics` 与适配器的清单
    字段去掉 hooks
  - 测试：`test_plugin_hooks`（917 行）换成 `test_plugin_dispatch`（112 条：点清单 / 7 点载荷 /
    无订阅方 / 派发器常驻与拆挂载 / 链上句柄入口 / 开发者模式记录 / 多挂载数组）；
    `plugin_host_mount` 的“钩子只在本 agent 派发”改成“扩展点实现行只对登记的 agent 生效”
    （登记能力行 → A 有结果、B 没有 → 注销后都没有），订阅数 2→3；
    `plugin_runtime` 删 `AgentxxPluginHookSpec` 布局台账（ABI 编号 8 改成 `PluginxxCapabilitySpec`，
    C 侧同步）；`plugin_sdk` 删伪 hooks 表与两个 hook 用例，补 `providePoint` 两种业务体形态；
    `test_plugins` 的两段（轮次中禁用 / mount 失败回滚）改成“派发器常驻”断言；
    DSO 回滚夹具的第 5 步从“注册 hook”改成“订阅 `agentxx.agent.point.model.start`”；
    `negative_compile` 的 `stl_param_hook` → `stl_param_capability`、删 `wrong_hook_return`、
    `positive_control` 改用 `providePoint`
  - 台账与文档：`kInterfaceTableCount` 22→**20**（10 + 10），`boundaries` 10/20/29 与台账删
    `AgentxxPluginHooksIface`；`plugins.md`（20 张 agent / 10 业务 / 表格删 hooks 行 / 架构图 /
    §SDK 示例 / 各处“钩子”改成扩展点或订阅 / 测试模块名）、`index.md`（20 张 + 目录树）、
    根 `AGENTS.md`（20 张 / 10 业务 / “钩子处理器清单与优先级”整段换成“通知型扩展点派发”）
  - 验证：lib/plugins/test 全编过；`boundaries` 19、`plugin_dispatch` 112、`plugin_host_mount` 146、
    `plugin_sdk` 133、`plugin_cleanup` 90、`plugin_feature` 34、`plugin_point` 63、
    `assembly_snapshot_io` 57、`plugins` 599、`plugin_multi_mount` 47、`summarization` 454、
    `observability` 114、`event_hub` 268、`event_tools` 97、`event_delivery` 48、`plugin_runtime` 726、
    `plugin_example_runtime` 50、`client_plugins` 754、`view_replica` 114 全绿
    （`async_work` 本机被 ASan + 输入法 TSF DLL 的 use-after-free 打断，未得结论，属环境噪声）
  - 踩到的坑：
    - 中间件链上多了常驻句柄：`assembly_snapshot_io` 原断言“最后一个中间件是 LogPrint”不再成立
      （派发器建挂载时追加在它之后）；改断言“最后一个非派发句柄是 LogPrint”
    - 派发器只按挂载析构摘会漏：正常路径里 `AgentContext::pluginMount` 还持着旧挂载，析构不跑
      → 必须在 `unmountAgentAsync` / `unmountAllAgentsAsync` 显式摘；否则重复挂载会在链上越积越多
    - 多挂载要求 `AgentContext::agentId` 互不相同（默认都是 `root`，第二个会被拒
      “传入的上下文不是同一个 agent”）

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

- 状态：**完成**（client 侧表、广播、DomainServices；6 处客户端订阅点已迁频道）

已改动（按文件）：

- `agent/lib/include/agentxx/plugin/client_plugin_manager.h`
  - 继承 `pluginxx::DomainServices`；`kInterfaceTableCount = 11 + 6`（17 张）
  - 删实例内订阅簿记（`Subscription` / `clientSubscriptions` / `subHandles` /
    `ClientSubscriptionImpl`）与 `subscribe` / `unsubscribe` 声明
  - 新增：`ensureClientBroadcast` / `publishClientChannelValue`（公开）、
    `publishClientChannel` / `declareClientChannels` / `ensureClientChannelDeclared`（私有）、
    遮蔽基类的 `subscribeBroadcast`（先补声明再订阅，跨进程频道才收得到）
  - 新增 `workerPool_`（懒建 2 线程）与 6 个 DomainServices 覆写
  - `setLanguage` 就地覆写 `DomainServices::setLanguage`（原来重复声明，MSVC 报 C2535）
  - `broadcasts()` 不再重复声明（基类 `PluginHostCore` 已有同一实现）
- `agent/lib/src/plugins/client_plugin_manager_domain_services.cpp`（新）
  - `postToWorkerThread`（懒建池）/ `configJson`（language + platform；路径类配置属于
    agent 侧，界面进程不读）/ `language` / `pluginsJson` / `pluginJson`（基于 `list()`）
  - `declareClientChannels`：7 个客户端频道（状态类 `cache: latest`，事件类 `none`）
  - `ensureClientChannelDeclared`：按名字形态补声明 ——
    `agentxx.client.plugin.<插件名>.*`（属主=该插件，两端可发，不缓存）、
    `plugin.<插件名>.*`（属主发布，`cache: latest`，与 agent 侧 `ensureTopicBroadcast` 一致）、
    其他应用频道（任何人可发）
  - `publishClientChannel`：按形态取发布者身份（应用 / 代哪个插件发）后发布
- `agent/lib/src/plugins/client_plugin_manager.cpp`
  - 删 `xx_csubscribe` / `xx_cunsubscribe` / `xx_clog` / `xx_cjson_*` /
    `g_clientIfaceEvents` / `g_clientIfaceJson` / `g_clientIfaceLog` /
    `g_clientIfaceCoroutineRuntime` / `ClientPluginManager::subscribe|unsubscribe`
    （约 600 行）
  - `xx_cquery_interface` 先查 `pluginxx::queryGenericPluginIface<ClientPluginInstance,
    ClientPluginManager>`，再分发 6 张自有表 + `agentxx.net`
  - 6 处端点事件改 `publishClientChannel`（ready / connection_state / user_input / delta /
    turn_end / session_switch）；`reportRegionSize` 改发 `ui_layout`
  - `onPluginData`：订阅方判定改用 `broadcasts()->listJson(topic)`，载荷改发到同名频道
- `agent/lib/src/plugins/client_plugin_manager_net.cpp`：`publishNetEvent` 改发
  `plugin.<插件名>.net`（原先只记调试日志）
- `agent/lib/include/agentxx/plugin/api/client_plugin_api.h`：删 `agentxx.client.events`
  / `.json` / `.log` 三张表与 `AgentxxClientEvent` 枚举（原枚举处留一张频道对照表）
- `agent/lib/include/agentxx/plugin/api/plugin_iface_meta.h`：删三个 `IfaceMeta` 特化
- `agent/lib/include/agentxx/plugin/api/plugin_guard.h`：删 client 日志表重载（两侧统一
  `pluginxx.log`，`using pluginxx::logTo` 即可）
- `agent/lib/include/agentxx/plugin/api/plugin_kit.h`：`ClientIfaces` 删 `events`、
  `json`/`log` 改指 `PluginxxJsonIface`/`PluginxxLogIface`、新增 `broadcasts`；
  `logClientCreateFailure` 改查 `PLUGINXX_IFACE_LOG`
- 客户端插件订阅点改频道（6 个文件）：
  - `agentxx_chatbot/chatbot_client.cpp`：`plugin.agentxx_chatbot.panel` + `client.ready` +
    `client.connection_state`；载荷不再包 `{topic,data}`，直接是快照 JSON
  - `agentxx_planning/agentxx_planning.cpp`：`client.delta` + `plugin.agentxx_planning.planning`
    + `client.session_switch`（规划频道值就是规划 JSON）
  - `agentxx_codegraph/agentxx_codegraph.cpp`：拆成 `status` / `progress` 两条频道订阅
  - `agentxx_system_monitor/agentxx_system_monitor.cpp`：`plugin.agentxx_system_monitor.usage`
  - `example_plugin/example_plugin.cpp`：`client.ready` / `client.turn_end` /
    `plugin.example_plugin.progress`；agent 侧两条订阅改用 `subscribePluginBroadcast`
  - `test/plugin/dso_plugins/test_client_start_fail/test_client_start_fail.cpp`：订阅
    `agentxx.client.ready`
- 测试与文档：
  - `test/core/test_boundaries.cpp`：client 表 9 → 6、合计 29 → 26、台账删三行、
    `kInterfaceTableCount` 17、文档数字校验改 17
  - `test/plugin/test_plugin_abi_c17.c`：删 `ClientEventsIface` 对齐断言
  - `test/plugin/test_plugin_sdk.cpp`：老主程序模拟改为"不提供 client 表 + 通用 log/json/
    broadcasts"
  - `test/plugin/test_client_plugins.cpp`：原"订阅生命周期安全"段（多次订阅扩容/派发中订阅/
    派发中退订）整体换成广播语义（频道已声明、一次发布多订阅、退订后不到达、
    未声明跨进程频道由订阅入口补声明、卸载撤销订阅）；11.3 的 READY 用例改频道订阅
  - `test/core/test_observability.cpp`：`hookHandlers[` → `hookPoints[`（步骤 3-3 重命名时漏改）
  - `docs/zh-cn/design/plugins.md` §9 重写（17 张表 + 客户端频道表）；
    `tui.md`、`index.md`、根 `AGENTS.md` 数字与频道名同步

验证：

- `agentxx_static` / `agentxx_lib_repo` / `agentxx_client_repo` / `agentxx_test_repo` 编过
- `build_plugins.ps1`：26 个插件全部重新产出
- `agentxx_test --log-level warn`：`boundaries` 19、`client_plugins` 763、
  `plugin_sdk` 133、`plugin_bridge` 193、`tui_widget` 208、`tui_form` 84、
  `plugins`/`plugin_multi_mount` 47、`plugin_cleanup` 90、`plugin_dispatch` 112、
  `plugin_feature` 34、`plugin_point` 63、`plugin_host_mount` 146、`plugin_runtime` 726、
  `plugin_example_runtime` 50、`event_hub` 268、`event_delivery` 48、`event_tools` 97、
  `view_replica` 114、`summarization` 454、`observability` 114、`assembly_snapshot_io` 57
  全部通过

与计划的偏差 / 注意：

- `pluginxx.json` / `pluginxx.log` 两张通用表在 client 侧的成员与原 `agentxx.client.json` /
  `.log` 逐字相同，所以插件源码只需换指针类型名（`ClientIfaces` 已换）
- 跨端插件数据的**下行**方向：`WirePluginData.topic` 是 agent 侧主题
  （`plugin.<插件名>.<事件>`），主程序按原样发布到同名频道（不是改写成
  `agentxx.client.plugin.*`）；**上行**方向仍走 `agentxx.client.wire` 的
  `send_plugin_data`（要求 `agentxx.client.plugin.<插件名>.<事件>` 全名）
- client 侧广播注册表**没有**注入 scheduler（与 agent 侧一致），订阅限频的 `delay`
  补发退化为"下一次发布时顺便补"；当前没有插件用限频订阅
- `configJson()` 只给 `language` / `platform`：客户端的路径类配置不属于本进程
  （见 configuration.md 第 5 节），需要路径的客户端插件应查 `get_client_state()`
- `example_event/plugin.yaml` 里"客户端侧要到步骤 5 才提供广播表"的注释已是旧信息，
  随步骤 6 一起改

### 步骤 6：插件全量迁移

- 状态：**完成**（iface.events 兼容层已删除，agent 侧插件全部改走广播表）

删掉的兼容层：

- plugin_kit.h 的 PluginEventsCompat / pluginEventsCompat() / detail::eventsCompat* /
  AgentIfaces::events（detail::takeHostString 保留，它不是兼容层的一部分）
- publishPluginBroadcast / subscribePluginBroadcast 各加一个 PluginxxStringView
  重载（插件里普遍持有 ABI 视图，省掉调用点的 PluginStringView::str(...)）

迁移的发布点（iface.events->publish(host, &id, &payload) →
publishPluginBroadcast(host, iface.broadcasts, id, payload)）：

- gentxx_audio_stream（音频帧）、gentxx_chatbot（面板快照）、
  gentxx_codegraph（status / progress ×2）、gentxx_javascript_engine（JS mit）、
  gentxx_planning（规划）、gentxx_screen_capture（帧）、
  gentxx_system_monitor（usage ×2）、gentxx_text_selection_monitor（选中文本）

迁移的订阅点（→ subscribePluginBroadcast(host, iface.broadcasts, scope, id, handler, ud)）：

- gentxx_codegraph（gentxx.agent.host.client_attached）、
  gentxx_planning（同上）、gentxx_chatbot（面板 refresh + client_attached）、
  gentxx_system_monitor（客户端开关 + client_attached）、xample_feature（point.turn.start）、
  xample_plugin（自己的 demo 频道 + client 上行频道）
- 守卫条件同步改成 iface.broadcasts && iface.broadcasts->publish_broadcast /
  subscribe_broadcast（PluginxxBroadcastsIface 的成员是 *_broadcast，
  原来写的 ->publish / ->subscribe 不存在）

其它：

- gent/plugins/example_event/plugin.yaml：去掉"客户端侧要到步骤 5 才提供这张表"的过时注释
  （客户端现在提供广播表，但仍不写成 equire，精简主程序可能没有）
- gent/test/plugin/test_plugins.cpp 第 30 段（"禁用插件 publish 被拒"）改用
  PluginxxBroadcastsIface（先 declare_broadcast 再 publish_broadcast，禁用后返回非 OK）
- docs/zh-cn/design/plugins.md 里 pluginxx/api/tables.h 的表清单改成当前一组
  （broadcasts/tasks 替换掉已删的 events）

验证：

- uild_plugins.ps1：26 个插件全部重新产出
- gentxx_test --log-level warn：plugins 599、plugin_multi_mount 47、
  plugin_cleanup 90、plugin_feature 34、plugin_point 63、plugin_dispatch 112、
  client_plugins 763、plugin_sdk 133、plugin_runtime 726、plugin_bridge 193、
  oundaries 19、vent_hub 268 全部通过

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

- 状态：**已完成**

**测试**

- `cmake --build build/windows-debug --config Debug`（全量，含 lib / client / test / benchmark /
  插件目标）：exit=0
- `pwsh -File agent/script/build_plugins.ps1`：26 个插件全部重新产出
- `agentxx_test --log-level warn` 全模块：除 `test_modules` 外无 `[FAIL]`；`test_modules`
  失败原因是 `docs/zh-cn/design/test-modules.md` 落后（新增了 `plugin_point` / `plugin_net` /
  `plugin_dispatch` 三个模块）—— 用 `AGENTXX_UPDATE_TEST_MODULES=1` 重新生成（143 个模块）后
  重跑通过
- 一批启子进程 / 开服务端的模块在 ASan 构建下 abort：`ffi_c_api` / `websocket` /
  `remote_agent` / `network_timeout` / `mcp` / `input_delivery` / `session_sync` /
  `shutdown_stages` / `async_work` / `event_subagent`。堆栈里的分配、释放、读取全部落在
  搜狗输入法的 `SogouPY.ime` / `SogouTSF.ime` 上（进程加载了 IME 的 TSF DLL），没有任何
  agentxx 帧；abort 之前这些模块无 `[FAIL]`（`cancel` 第二次单独跑 45 条全过）。属于环境噪声
- 逐模块跑完剩下的全部模块：25468 passed / 0 failed

**文档**

- `docs/zh-cn/design/plugins.md`
  - §2：版本限制与“不改库”的实施方式 4 → 5（v5 = 迁到 `cxx_pluginxx` 0.3.0）
  - §5.1：`pluginxx/host/` 文件清单改成当前一组（`broadcast_registry` / `capability_chain` /
    `domain_services`，去掉已删的 `event_bus` / `domain_hooks`）；十张通用表清单把 `events`
    换成 `broadcasts`，主程序侧实现指向 `plugin_manager_domain_services.cpp`
  - §8：拆成“8.1 业务表（10 张）/ 8.2 通用表（10 张）”两段，通用表 IID 改成真实值
    `agentxx.<表名>`（原文写 `pluginxx.<表名>`，按那个名字查询不会命中），版本号按头文件
    改成 broadcasts 3 / capabilities 6 / config 3 / cancel 3，删掉已删的 `agentxx.agent.events`
    行，补“另一张 `agentxx.net` 不计入 20 张”；代次说明改成当前 v5 并补 v3/v4 历史
  - §9：表数量写法改成“10 张框架通用表 + 1 张网络表 + 6 张自有表”，`pluginxx.json/log`
    改成 `agentxx.json` / `agentxx.log`
  - §17.7：`IfaceMeta` 特化数 29 → 26，长度门槛用例 `plugin_feature` → `plugin_runtime`
  - §18.6：`pluginxx.plugins.list` → `agentxx.plugins.list`
  - §20.3：删掉“`iface.events` 兼容层仍可用”一段（兼容层已在步骤 6 删除）
  - §21/§21.4：标题“功能点 id” → “扩展点 id”，内容改成当前模型（一个 id = 一条能力行 +
    一个同名广播频道，SDK `providePoint` / `pointAnswer`），不再提 `definePluginPoint`
  - §8 内置能力段：能力登记/调用改成 v6 语义（`capability(..., opts)`、`invoke_capability`、
    主程序侧 `capabilities()->infoJson` / `emitPoint`）
- `docs/zh-cn/design/index.md`：附录 B 的两侧表数量与清单、目录树里的 plugin/host 文件清单
  （`event_bus`/`domain_hooks` → `broadcast_registry`/`capability_chain`/`domain_services`）、
  `DomainHooks` → `DomainServices`、Client 侧“9 张表” → 17 张
- `docs/zh-cn/design/events.md`：“功能点 id” → “扩展点 id”
- `docs/en/design/plugins.md`：§8/§9 的表清单按 20/17 重写（去掉 `hooks` / `client.events` /
  `client.json` / `client.log`），补广播频道清单与能力调用说明，翻译状态说明的“16 节” → 22 节
- 根 `AGENTS.md`：功能点体系段改成“扩展点体系（插件面走能力行）”、注册可逆的类型清单、
  相对表的 IID 命名（`pluginxx.*` → `agentxx.*`）、当前代次 3 → 5（补 v4/v5 历史）、
  `DomainHooks`/`event_bus` 相关描述、扩展点 id 规则
- `agent/plugins/AGENTS.md`：无需改动（未提四张已删表）

**代码注释同步**（只改注释，无行为变化）

- `plugin_manager.h`：类头注释里的通用表清单（`events` → `broadcasts`）与 `DomainHooks` →
  `DomainServices`（两处）
- `agentxx_system_monitor.cpp`：能力调用入口注释里的 `invoke_capability_async` /
  `PluginManager::invokeCapabilityAsync` → `invoke_capability` / `capabilities()->infoJson`

**静态自检（plan §7.7）**

- `grep -rn 'utilxx/http_client\.h|utilxx/ws_client\.h' agent/plugins/`：无输出
- `grep -rn 'iface\.events|feature_points|hookEx' agent/plugins/ agent/lib/src/plugins/`：无输出
- `grep -rn '23 张|9 张接口表|13 张业务' docs/ AGENTS.md agent/plugins/AGENTS.md`：无输出
- `boundaries` / `config_keys` / `test_modules` 重跑：241 passed / 0 failed

**与计划的偏差**

- 表数量：计划写“agent 侧 21 张（10 框架 + net + 10 业务）”，实际常量为 20 —— `agentxx.net`
  在 agent 侧不计入 `kInterfaceTableCount`（头文件与文档都写明“单独一张”），client 侧 17 张
  里则包含 net。两侧口径不同但各自与常量、`boundaries` 规则一致；若以后要统一成
  “11 通用 + 10 业务 = 21”，改常量、`boundaries` 规则 7 与四处文档即可
- 计划里的“功能埋点”在全项目统一写成**扩展点**（`boundaries` 规则 15 拦停用词），
  `plugin_hooks` 合并进 `plugin_point`；`plugin_feature` 保留为“插件面登记实现行”的用例
- 计划里的 `PluginNetHub` 落到两处：`cxx_utilxx` 的 `utilxx::NetHub`（与插件无关的通用网络
  服务，按 owner 记账）+ 主程序的 `agentxx::plugin::PluginNetService`（适配接口表、事件、租约）

## 验证清单（实施完成后逐条勾）

- [x] 两个库 Debug 编过，`api_abi_check` 与新增静态断言通过
- [x] `windows_debug_build.bat` 全绿（lib + client + test + benchmark + 插件目标）
- [x] `agent/script/build_plugins.ps1` 全部插件产物时间戳更新（26 个）
- [x] `agentxx_test --log-level warn` 全模块通过（debug + ASan；此前被搜狗输入法 TSF DLL
      带崩的一批模块已定位并修好触发源，见"补充: 搜狗输入法崩溃排查"）
- [x] `boundaries` 规则 7（20 / 17 张表，net 在 agent 侧单独计）、`IfaceMeta` 特化数 26、
      扩展点 id 头与声明表一致
- [x] 能力行 scope 隔离 / 宿主发起调用 / 广播 end / 扩展点原语 四项库语义各有一条测试
      （`plugin_point` 覆盖扩展点原语的声明/分发/无实现/通知面/当前值）
- [x] 4 个网络插件本地回环往返通过；`grep utilxx/http_client.h agent/plugins/` 为空
- [ ] 手工：空列表启动 26 插件全加载、插件禁用→启用账目归零再回原值、chatbot 断网重连、
      客户端重连后插件面板与装饰重建（需人工开 TUI / 真实平台，自动化用例已覆盖等价语义）
- [x] 文档：`plugins.md`、`index.md`、`events.md`、`AGENTS.md`、`plugins/AGENTS.md`、
      `docs/zh-cn/design/` 内相关分册（另同步了 `docs/en/design/plugins.md` 的表清单）

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

## 补充: 搜狗输入法崩溃排查与三处修复 (全量测试收尾)

### 现象

debug + ASan 下全量跑 `agentxx_test`, 若干模块 (`ffi_c_api` / `websocket` / `remote_agent` /
`network_timeout` / `mcp` / `shutdown_stages` 等) 直接 abort, 控制台只有一行
`AddressSanitizer: heap-use-after-free ... SogouPY.ime+0x180382b7e` 之类的报告, 断言/真正的
失败信息全被它盖掉; 单独跑同一模块有时又不复现。

### 因果链 (cdb 抓栈 + 独立探针证实)

1. `utilxx::HttpServer` 析构时成员声明顺序问题: `workers_`(io_context) 在 `connMutex_` /
   `trackedConns_` **之前**声明, 析构逆序先把 mutex/vector 拆掉, 再销毁 `workers_` —— 挂起的
   `serveTcp` 协程帧在这时销毁, 其 `Untracker` 回调 `untrackConnection` 去碰已析构成员,
   debug STL 断言 `vector erase iterator outside range` (release 下是 UB)。
2. 调试 CRT 的断言默认**弹模态对话框** (`ucrtbased!CrtDbgReport` → `USER32!MessageBoxW`)。
   MessageBox 创建窗口 → `IMM32!CtfImmDispatchDefImeMessage` 延迟加载 `MSCTF.dll` →
   `SogouTSF.ime` / `SogouPY.ime` → 搜狗自身在 IME 窗口过程里 heap-use-after-free,
   ASan 抓住即 abort, 断言文本反而看不到。
3. 独立探针 (`CoInitializeEx` + `TF_CreateThreadMgr` + `Activate`/`Deactivate` +
   `CoUninitialize`, MSVC ASan, 不含任何本项目代码) 稳定复现**同一偏移**
   `SogouPY.ime+0x180382b7e` 的 UAF → 搜狗自身缺陷, 本项目只能避免触发它。
4. 另有本项目真实缺陷 `WrapHandleBaseNode::run`: 缓存 `len = handles.size()` 后跨 `co_await`
   用实时列表 `handles[i]` 索引, 回合中插件禁用/卸载会缩短列表 → `vector subscript out of
   range` (`shutdown_stages` 触发)。

### 修复

- `agent/third_party/cxx_utilxx/src/http_server.cpp`: `~HttpServer()` 在 `stop()` 之后先
  `acceptor_.reset(); workers_.clear();`, 让协程帧在其它成员仍存活时销毁 (子模块提交, 见下)
- `agent/test/test.cpp`: `main` 开头把 `_CRT_WARN/_CRT_ERROR/_CRT_ASSERT` 的 report mode 改成
  `_CRTDBG_MODE_FILE` + `_CRTDBG_FILE_STDERR` —— 断言文本进日志, 终止语义不变
  (`_STL_REPORT_ERROR` 报完仍 abort), 也不再拉起输入法
- `agent/lib/include/agentxx/nodes/wrap_handle.h`: `run` 进入时拷一份 handle 列表作为本轮
  快照, start/baseRun/end 三段都用它 (`handles[i]` / `baseRun(handles, ...)`), 越界消失,
  且 start/end 配对不受中途列表变化影响
- `cpu_gpu` 的 4 条失败 (`capability is not declared: agentxx.system_usage`): 插件框架要求
  `agentxx.*` 这类官方 id **由应用先声明**, 本项目的 `PluginManager` 没声明 →
  - 新增 `agent/lib/include/agentxx/plugin/api/agentxx_capability_ids.h` (官方能力 id 清单)
  - `PluginManager::declareAppCapabilities()` (构造函数调用): 按 `impls: many` +
    `merge: first` + 只开 impl 阶段声明, 与功能埋点同形
  - `agentxx_system_monitor` 改用头里的常量登记实现行
  - `plugin_point` 加一条用例: 构造后这些 id 已声明、`has()` 为假 (还没人实现)、
    自省里 `declaredBy` 是 `app`

### 验证

- 修完逐个重跑此前崩溃的模块: `ffi_c_api 208` / `websocket 254` / `remote_agent 602` /
  `network_timeout 49` / `mcp 404` / `input_delivery 139` / `session_sync 30` / `async_work 138` /
  `event_subagent 52` / `shutdown_stages 17` 全绿, 无 ASan 报告
- `cpu_gpu 55` / `plugin_point 68` 全绿
- 全量 `agentxx_test --log-level warn`: 见下方"当前进度"与 `index.md` 的结论
