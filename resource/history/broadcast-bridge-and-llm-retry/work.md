# 实施记录

## 进度

- [x] 步骤 1：广播 ↔ 事件总线两向转发（`PluginManager`）
- [x] 步骤 2：`llm_error` 状态码按独立 token 匹配 + 切掉诊断后缀
- [x] 步骤 3：`BroadcastRegistry` 相同声明幂等（不打日志）
- [x] 步骤 4：测试与构建验证（全量 42820 passed / 3 failed，失败项与本任务无关）
- [x] 步骤 5：文档同步（plugins.md §20.1 / §20.5、roadmap）

## 步骤 1：广播 ↔ 事件总线两向转发

改动：

| 文件 | 内容 |
|---|---|
| `agent/lib/include/agentxx/plugin/plugin_manager.h` | 新增 `attachEventBus`（公开）、`mirrorBusValue` / `forwardBroadcastToBuses` / `publishOnBus` / `detachEventBuses` 与 `BusBridge` 成员 |
| `agent/lib/src/plugins/plugin_manager_broadcast_bridge.cpp`（新） | 两向转发实现 |
| `agent/lib/src/plugins/plugin_manager_lifecycle.cpp` | 构造函数挂主 agent 总线；`attachMountSync` 挂每个挂载 agent 的总线；析构撤订阅 |
| `agent/lib/src/plugins/plugin_manager_domain_services.cpp` | `publishBroadcastFrom` 成功后 `forwardBroadcastToBuses`；`publishBroadcastValue` 见下方"验证时发现的问题" |
| `agent/test/plugin/test_plugin_broadcast.cpp` | 夹具加事件总线；新增 `test_bus_bridge`（两向各一组断言 + 未声明话题不建频道） |

验证：

```
agentxx_test --log-level warn plugin_broadcast
--- plugin_broadcast done: passed=90 failed=0 ---
```

回归（广播/埋点/挂载/运行时/示例插件）：

```
agentxx_test --log-level warn plugin_point plugin_feature plugin_dispatch plugin_cleanup \
    plugin_host_mount plugin_runtime plugins
Total: passed=... failed=0
```

### 验证时发现的问题（已修）

第一次全量跑出现 5 个失败，其中 3 个是本任务的回归：

1. `plugins` 1682 / `plugin_runtime` 2143·2151·2184 —— 我一开始把 `publishBroadcastValue`
   改成"有总线就只发总线"。而总线镜像只覆盖 `agentxx.agent.` / `agentxx.client.` 两族，
   `plugin.*` 频道因此一条值都到不了注册表（代理发布 + 同步派发的既有语义被破坏）。
   现在按 id 前缀分流：
   - `agentxx.agent.*` / `agentxx.client.*`（镜像覆盖）：发总线，由镜像落回广播（不重复投递）；
   - 其余（`plugin.*` 或没有总线）：直接写广播（同步派发给订阅方），再 `forwardBroadcastToBuses`
     让端点能转给客户端。
2. `boundaries` 规则 15（称呼与用词）：注释里的 `落地`、文档里的 `链路` 各一处，已改成
   `执行` / `路径`。

## 步骤 2：LLM 错误分类不再被字节数带偏

用户报的现象：

```
[提示] 错误
Node 'llm' failed after 1 attempt(s): SSE read-body timeout after 60000
recv=403684B events=565 idle=60013ms finish=none tools=0 args=0B name=
```

根因：`streamFailureSuffix` 拼进错误文本的 `recv=403684B` 里有 `403`，而
`classifyLlmError` 用裸子串匹配状态码 → 判成 `Auth`（不可重试）→ 重试循环第一次就退出
（`recv` 字节数里出现 400/401/402/403/413/429/500… 都会触发，所以"偶现"）。

改动：

| 文件 | 内容 |
|---|---|
| `agent/lib/src/nodes/llm_error.cpp` | 新增 `containsStatusCode`（前后不是 ASCII 字母数字才算命中）、`isAsciiAlnum`；六处状态码（413 / 401+403 / 402 / 400 / 429 / 500+502+503+504）改走它；判类前切掉 ` &#124; stream: ` 起的自有诊断后缀 |
| `agent/lib/include/agentxx/nodes/llm_error.h` | `classifyLlmError` 注释补上这两条规则 |
| `agent/test/core/test_llm_error.cpp` | 新增 6 条带诊断后缀的真实文本（字节数含 403/400/402/413/429/500）、4 条无后缀的裸字节数、7 条独立成词的状态码 |

验证：

```
agentxx_test --log-level warn llm_error
--- llm_error done: passed=148 failed=0 (1 ms) ---
agentxx_test --log-level warn agent
--- agent done: passed=225 failed=0 (18632 ms) ---
```

## 步骤 3：相同广播声明不再刷日志

改动：

| 仓库 / 文件 | 内容 |
|---|---|
| `cxx_pluginxx`（子模块，提交 `46a9812`）`src/broadcast_registry.cpp` | 新增 `sameDecl`（逐字段比较 owner/cache/publish/pushOnSubscribe/coalesce/scope）；与现有声明完全相同时不打 `declaration of ... replaced` 日志，状态语义不变（`end` 之后再次声明仍按"开新流"） |
| `cxx_pluginxx` `include/pluginxx/host/broadcast_registry.h` | `declare` 注释补一条幂等说明 |
| 本仓 `agent/third_party/cxx_pluginxx` | 子模块指针前移 |
| `docs/zh-cn/design/plugins.md` §20.1 | 文档同步同一条规则 |

命中场景：SDK 的 `publishPluginBroadcast` 每次发布前都 `declarePluginBroadcast`，
`agentxx_system_monitor` 周期发布的 `plugin.agentxx_system_monitor.usage` 因此每秒刷一条
`[I] BroadcastRegistry: declaration of ... replaced (agentxx_system_monitor -> agentxx_system_monitor)`。

验证：

- `plugin_broadcast` 增加"发布后再次声明，当前值保留"的断言（幂等声明不动状态）；
- 该模块在 `--log-level info` 下不再出现任何 `declaration of ... replaced` 行。

## 收尾：全量测试

```
agentxx_test --log-level warn
Total: passed=42820 failed=3
```

3 个失败与本任务无关（都在 `utilxx_base` 的 UTF-8 处理与 GBK 自动转换上，本任务未触碰）：

| 模块 | 断言 | 内容 |
|---|---|---|
| `string_util` | 481 | `utf8IsAvail` 截断序列 |
| `string_util` | 617 | `utf8Repair` 幂等 |
| `session_persistence` | 1341 | 非法字节被替换为 `GBK: ...` |

## 后续变更一：重试策略一律化（任何错误都重试）

用户反馈：网络错误偶现仍会结束本轮。根因不在分类本身，而在“存在不值得重试的类别”这条规则 ——
只要判类出错，就会把一次本来能恢复的失败变成结束本轮。结论：**删掉“值不值得重试”的判断**。

| 文件 | 改动 |
|---|---|
| `agent/lib/include/agentxx/nodes/llm_error.h` | 删除 `isLlmErrorRetryable` 声明；`LlmErrorKind` 文档改为“任何失败都重试，分类只决定退避”；新增两条警告（不得加不重试判断、不得按错误文本推断上下文超限） |
| `agent/lib/src/nodes/llm_error.cpp` | 删除 `isLlmErrorRetryable(kind)` / `isLlmErrorRetryable(kind, errInfo)` / `looksLikeNetworkFailure`；退避注释改为“类别只决定等多久”；网络判定移到 `classifyLlmError` 最前（在切诊断后缀之前） |
| `agent/lib/src/nodes/modelcall.cpp` | 停止条件只剩 `isCancel` 与 `retry >= llm.max_retry`（`retryable` 项删除） |
| `agent/test/core/test_llm_error.cpp` | 删除对 `isLlmErrorRetryable` 的全部引用（0 处）；`&#124; stream: ...` 用例的期望由 `Timeout` 改为 `Network` |

## 后续变更二：删除“上下文超限 → 压缩后重试”

用户反馈：`isLlmContextOverflow` 这类判断有时会判错，而默认在上下文达到 75% 时就会自动压缩，
真到超限的情况基本遇不到；误判的代价是白压一次历史。结论：**整块删除**，并写明警告。

| 文件 | 改动 |
|---|---|
| `agent/lib/include/agentxx/nodes/llm_error.h` | 删除 `LlmErrorKind::ContextOverflow` 与 `isLlmContextOverflow` 声明；头部警告写清“不要从错误文本/网络响应判断上下文超限，上下文长度只由自动压缩处理（上限 75% 触发）” |
| `agent/lib/src/nodes/llm_error.cpp` | 删除上下文超限判定块（`context_length_exceeded` / `maximum context length` / `request entity too large` / `413` 等关键词）与 `isLlmContextOverflow` 定义；`llmErrorKindText` / `llmRetryDelaySeconds` 去掉该类分支；余下规则重新编号（1 网络 → 2 鉴权 → 3 额度 → 4 请求非法 → 5 限流 → 6 超时 → 7 服务端） |
| `agent/lib/src/nodes/modelcall.cpp` | 删除 `overflowCompactUsed`、`overflow` 判定、停止条件里的溢出项，以及发 `EventCompactContext` 的“压缩后立即重试”分支；注释写清“停止重试只有取消与达到 `llm.max_retry`”与本轮警告；`agentxx/event/{event_stream,events}.h` 两个头随该功能加入，现已无用，一并删除 |
| `agent/test/core/test_llm_error.cpp` | 删除上下文超限用例组；`413 payload too large` 改为断言 `Unknown`（不再单独识别，同样会重试）；模块头注释同步 |
| `agent/test/core/test_agent.cpp` | `test_agent_llm_error_policy` 重写：块 1 由“401 只请求 1 次”改为 `llmMaxRetry = 1` + 一直 401 → 断言 `2` 次且 `hasError`；块 2 原“压缩后重试成功”改为“上游回 `context_length_exceeded` 也只按普通失败重试”（断言 `2` 次 = 没有多出压缩请求）；原块 3（持续超限只压一次）删除；全局 `g_da_sim_fail_*` 注释同步 |
| `agent/test/core/test_fake_provider.cpp` | 3a 由“401 只请求 1 次”改为 `llmMaxRetry = 1` + 两次 401 → 断言 `requestCount() == 2`；删除 `test_fake_provider_overflow_compaction` 及其 runner 调用 |
| `docs/zh-cn/design/index.md` | llm 节点条改为“任何失败都重试 + 分类只决定退避 + 两条不得再加的判定” |
| `docs/zh-cn/design/roadmap.md` | LLM-2 验收描述同步；LLM-3（溢出一次性压缩重试）状态改为**已移除**并写原因 |

注意（留给以后改这段代码的人）：

- 手动 `/compact` 路径仍用 `events::EventCompactContext`（`session_server_agent_io.cpp`、
  `middlewares/summarization.cpp`），**没有**随之删除，删的只是“模型调用失败时自动触发压缩”；
- 现在一轮请求的停止条件只有两个：取消、达到 `llm.max_retry`。任何“按错误文本决定不重试”
  或“按错误文本推断上下文超限”的代码都属于回退，见 `llm_error.h` 头部的两条警告。

验证（已完成）：

- 顶层 `windows-debug` 构建通过（无新增警告/错误）；
- `boundaries llm_error agent fake_provider` → `Total: passed=370 failed=0`；
- 全量 `agentxx_test --log-level warn`：`Total: passed=42749 failed=6`，其中 3 个是既有失败
  （`string_util` 481/617、`session_persistence` 1341）；`boundaries` 的 1 个是本轮新写的
  注释里用了禁用词 `口径`（已改成“判定规则”）；`input_delivery` 的 2 个在单跑该模块时
  `passed=139 failed=0`（全量跑时对 `g_da_sim_last_request` 的时序敏感，与本次改动无关，
  第一轮全量里也是通过的）。

## 与计划的偏差

- 计划里另建 `plugin_broadcast_bridge.cpp` 文件（重构计划 §4.8 的旧名字），实施为
  `plugin_manager_broadcast_bridge.cpp`；
- `publishBroadcastValue` 的"总线优先"方案在验证阶段被改成"按 id 前缀分流"（原因见步骤 1）；
- 没有为"声明幂等"加专门的日志捕获测试（测试框架没有捕获日志内容的能力），改用
  `--log-level info` 人工核对 + "当前值保留"的行为断言。
