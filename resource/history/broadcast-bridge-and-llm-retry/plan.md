# 设计方案：插件广播两向转发 + LLM 重试误判 + 声明重复日志

## 1. 背景与现象

### 1.1 codegraph 侧边栏停在 `wait for load`

`agentxx_codegraph` 的 client 侧注册 Info 段落，初始内容就是 `|- wait for load`，
之后靠两条广播频道更新：`plugin.agentxx_codegraph.status` / `.progress`。
agent 侧在 `mount` 里订阅 `agentxx.agent.host.client_attached`，客户端接入时重发一次快照。

用户看到的内容永远是初始占位，说明客户端一次状态值都没收到。

### 1.2 网络错误偶现不重试

```
[提示] 错误
Node 'llm' failed after 1 attempt(s): SSE read-body timeout after 60000
recv=403684B events=565 idle=60013ms finish=none tools=0 args=0B name=
```

`attempts = 1` 表示 `ModelCallWrapNode::baseRun` 的重试循环第一次就退出了，退出条件只有四个：
取消 / 不可重试类别 / 上下文超限已用掉机会 / `retry >= llmMaxRetry`。

### 1.3 声明重复日志

`plugin.agentxx_system_monitor.usage` 周期发布，每次发布前 SDK 都调一次 `declare`，
注册表把"同一属主、同一属性"的重复声明也按"替换"打日志。

## 2. 现状调查

### 2.1 广播与总线是两套东西（agent 侧）

| 方向 | 现状 | 文件 |
|---|---|---|
| 插件发布 → 注册表 | `PluginManager::publishBroadcastFrom` → `PluginHostCore::publishBroadcastFrom` | `lib/src/plugins/plugin_manager_domain_services.cpp:145` |
| 插件发布 → 事件中枢 | 同上，再 `eventHub->publishPluginValue`（只在有人等 / 有记录时写入） | 同上 |
| 插件发布 → 客户端 | **没有**。`SessionServerAgentIO::subscribePluginEvents` 只订阅总线前缀 `plugin.` / `agentxx.agent.host.` | `lib/src/agent/io/session_server_agent_io.cpp:2109` |
| 主程序发布 → 客户端 | 总线前缀转发（`client_attached` / `server_plugins` / 上行 `agentxx.client.*`） | 同上 |
| 主程序发布 → 插件订阅 | **没有**。插件只能经 `<应用名>.broadcasts` 表订阅，而总线上的 `agentxx.*` 话题没有落到注册表 | `lib/src/plugins/plugin_manager_domain_services.cpp` |
| 事件中枢的值 → 注册表 | 有（`EventHub::publishValueEx` 镜像到同名频道） | `lib/src/event/hub.cpp:557` |

重构计划 `resource/history/plugin-refactor-4/plan.md` §4.8 里写过要新增
`plugin_broadcast_bridge.cpp`（把 `round.start` / `client_attached` 镜像到广播）、
§3.10 写过 `WirePluginData` 上下行改广播，但两个方向都没实现：

- `PluginManager::publishBroadcastValue` 全仓**没有调用点**（本该是桥的入口）；
- 客户端侧的上行转发（`WirePluginDataUp` → 总线）仍在，但没有总线→广播这一跳，
  所以 agent 侧插件同样收不到（`agentxx_system_monitor` 的 usage_enabled 开关、
  `agentxx_chatbot` 的面板 refresh 都受影响）。

### 2.2 缺少的两次跳

```
client  ──WirePluginDataUp──▶ 服务端端点 ──总线(agentxx.client.*)──▶ ✗ ──▶ 插件订阅
插件发布 ──▶ 广播注册表(本进程) ──✗──▶ 总线(plugin.*) ──▶ 服务端端点 ──WirePluginData──▶ client
```

### 2.3 `classifyLlmError` 的判定顺序与依据

`lib/src/nodes/llm_error.cpp`：上下文超限 → 网络 → 鉴权 → 额度 → 请求非法 → 限流 → 超时 → 服务端，
每一类都是 `text.find(needle)` 裸子串。状态码类直接写了 `"413"` / `"401"` / `"403"` / `"402"` /
`"400"` / `"429"` / `"500"` 等。

`protocol/provider_common.h::streamFailureSuffix` 把 `recv=<字节>B events=... args=<字节>B` 拼进错误文本，
字节数完全不受控：

| `recv=` 取值 | 命中 | 结果 |
|---|---|---|
| 403684B | `403` | Auth（不可重试） |
| 400123B | `400` | InvalidRequest（不可重试） |
| 402001B | `402` | Quota（不可重试） |
| 413777B | `413` | ContextOverflow（多一次压缩） |
| 429001B | `429` | RateLimit（等 5 秒，能重试但判类错） |

即"偶现"取决于收到的字节数里是否恰好出现这三位数。

### 2.4 重复声明

`third_party/cxx_pluginxx/src/broadcast_registry.cpp:96`：只要 id 已存在就 `XX_LOGI("... replaced ...")`。
SDK `plugin_kit.h::publishPluginBroadcast` 每次发布前都 `declarePluginBroadcast`（无法预知是否已声明），
于是"每次发布都刷一条日志"。

## 3. 方案

### 3.1 广播 ↔ 总线两向转发（`PluginManager`）

新增两个内部入口，`agent/lib/src/plugins/plugin_manager_broadcast_bridge.cpp`：

1. `void PluginManager::attachEventBus(const std::shared_ptr<events::EventBus>& bus)`（幂等）
   - 订阅总线前缀 `agentxx.agent.` 与 `agentxx.client.`；
   - 回调只接受 `std::string` 载荷（插件/主程序约定事件的 JSON 原文；强类型事件的载荷类型不同，天然跳过）；
   - **只镜像已经声明为广播频道的名字**（插件订阅时按需声明，`declareBuiltinBroadcasts` 也先声明），
     其余名字一次哈希查找就跳过 —— 避免把 `agentxx.agent.io.delta` 这类高频强类型话题搬进注册表；
   - 以"应用身份"（publisher 为空）发布：这两族都是应用频道。
2. `void PluginManager::forwardBroadcastToBuses(std::string_view id, std::string_view payload)`
   - 在 `publishBroadcastFrom` 成功后调用：把同一主题投递到主 agent 与各挂载 agent 的总线上
     （`asio::co_spawn`，与既有 `EventBus` 用法一致），供 `SessionServerAgentIO` 的 `plugin.` 前缀转发；
   - 同一总线只投一次。

两向的前缀**互不重叠**（`agentxx.agent.` / `agentxx.client.` 对 `plugin.`），因此不会形成环。

`publishBroadcastValue`（主程序代发应用频道）改成"有总线就发总线（由镜像落回注册表），
没有总线时直接写注册表"，避免同一条值被投递两次。

挂接点：

- `PluginManager` 构造函数：主 agent 的总线（`AgentContext::bus` 在 `BaseAgent` 构造函数里就建好了）；
- `PluginManager::attachMountSync`：每个挂载 agent 的总线。

### 3.2 状态码按独立 token 匹配

`llm_error.cpp` 新增：

```cpp
bool containsStatusCode(std::string_view text, std::initializer_list<std::string_view> codes);
```

命中要求：代码前后都不是 ASCII 字母/数字（`recv=403684B` 的 `403` 后面是 `6`，不命中；
`HTTP 401`、`(402)`、`code=429`、`status 503` 都命中）。

状态码集合改为走这个函数：`413`（上下文超限）、`401/403`（鉴权）、`402`（额度）、
`400`（请求非法）、`429`（限流）、`500/502/503/504`（服务端）。注意 `bad request` /
`internal server error` 一类**文本**关键字保持原样。

分类前先切掉我们自己生成的诊断后缀（`" | stream: "` 起到结尾）：后缀里的
`recv/args` 字节数、`finish` 原文都不该参与判类（`streamFailureSuffix` 的注释本来就写着
"不影响判类"，实际不成立）。

### 3.3 相同声明 = 幂等成功

`BroadcastRegistry::declare`：已存在同名频道且新声明与旧声明逐字段相同（cache / publish /
pushOnSubscribe / coalesce / scope / owner）时直接返回成功，不打日志、不清缓存。
频道已 `end` 的情况保持原语义（再次声明 = 开新流，照常打 `re-opened` 日志）。

## 4. 边界与兼容

- 只动 `.cpp`（`cxx_pluginxx` 的头文件与 ABI 不变），接口表版本不变；
- 多实例/多挂载：总线按 agent 分别挂，注册表仍是一份（插件侧按载荷 `agentId` 过滤，与既有约定一致）；
- 插件只在 `start`/`mount` 里订阅，镜像在挂载建立后即可用；建立之前的发布会由
  `client_attached` / 周期发布这类重复发布补齐（与本项目既有"晚接入兜底"一致）。

## 5. 分步实施

1. `plugin_manager.h` 加声明 + 新文件 `plugin_manager_broadcast_bridge.cpp` 实现两向转发；
2. `publishBroadcastFrom` / `publishBroadcastValue` 接入；
3. `PluginManager` 构造函数与 `attachMountSync` 挂总线；
4. `llm_error.cpp` 状态码 token 匹配 + 切诊断后缀；
5. `cxx_pluginxx` 声明幂等；
6. 测试：`plugin_broadcast` 增加总线↔广播两向用例，`llm_error` 增加真实错误文本与字节数用例。

## 6. 验证

- `agentxx_test plugin_broadcast`（含新用例）
- `agentxx_test llm_error`
- `agentxx_test plugin_point plugin_feature plugin_dispatch plugin_broadcast plugin_cleanup`（广播相关回归）
- 手工：TUI 启动后看 CodeGraph 段落是否从 `wait for load` 变为 `available · N`（或索引进度）

## 7. 风险

- 镜像是在总线发布的调用栈里同步执行插件回调：与"插件自己发布"的线程模型一致（都在 io 线程），
  但若插件回调很慢会拖慢总线发布 —— 既有插件回调本来就是"快速返回"约定；
- 广播上总线会让所有 `plugin.*` 值都走一次端点转发（重构前的行为就是这样），
  频率仍由插件控制。
