# 系统提示词来源注册与压缩后刷新 — 实施记录

对应设计: [plan.md](plan.md)

## 当前状态总览

| 步骤 | 内容 | 状态 |
|---|---|---|
| S0 | 通读相关源码 (context/prompt/middleware/memory_file/skill/summarization/modelcall/agent_host/observability/测试) | 完成 |
| S1 | 新增 `PromptSourceRegistry` (`prompt_sources.{h,cpp}`) 并挂到 `AgentContext` | 完成 |
| S2 | memory / skill 改为登记来源；删除每轮读写与纪元字段；按路径集合缓存内容 | 完成 |
| S3 | 组装改读注册表；退役两个段落通道键 | 完成 |
| S4 | 新增刷新入口中间件 (model call start)；接入取消与超时 | 完成 |
| S5 | 压缩写回后标记 dirty (自动 / 手动 / 硬截断) + `noteCompaction` | 完成 |
| S6 | 资源变更标记 dirty；`cleanupSession` 释放会话文本 | 完成 |
| S7 | 压缩请求前缀复用 (保留 system / 工具覆盖 / 不执行工具 / `tool_choice`) | 完成 |
| S8 | 指标、加载快照、诊断包、日志 | 完成 |
| S9 | 测试模块 (`prompt_sources`、`prompt_cache_prefix`) + 既有测试改写 + 文档 | 完成 |

测试现状 (Debug + ASan, Windows): **全量 `agentxx_test.exe` 通过 38119 断言 / 0 失败**，
其中相关模块见下表。

| 模块 | 结果 |
|---|---|
| `prompt_sources` (新) | passed=47 failed=0 |
| `prompt_cache_prefix` (新) | passed=25 failed=0 |
| `prompt_stability` | passed=19 failed=0 |
| `prompt_stability_io` | passed=53 failed=0 |
| `graph_data` | passed=24 failed=0 |
| `plugin_resources` | passed=90 failed=0 |
| `summarization` | passed=462 failed=0 |
| `session_sync` | passed=30 failed=0 |
| `assembly_snapshot_io` | passed=41 failed=0 |
| `agent` | passed=198 failed=0 |

## 已完成任务

### S0 源码核查结论

已确认 plan 的现状描述与代码一致，几个实施时必须知道的事实：

1. 组装链路: `AgentContext::buildSystemPromptStable` = 主 prompt + `prompt.orderedAppendSections()`
   + `buildStableContextSections()` (读 graphData 的 `xx_appendSystemMessageStable`)；
   `buildSystemPrompt` 额外拼 `buildDynamicContextSections()`。
2. 一轮顺序: `AgentStartCall` 清空 graphData → modelcall 的 start 钩子 → `baseRun` 重建 system
   消息 → run 钩子 (压缩在这里) → 发请求。刷新入口放 start 钩子成立。
3. `graphDataKey_appendSystemMessage` (动态段) **当前没有任何写入方**，只有 modelcall 的存在性告警。
4. 压缩子代理走 `AgentHost::spawnOneTask`:
   - `makeSubagentConfig` 清空 `skillDirPaths` / `memoryFilePaths` → 子代理重建 system 时缺运行时段；
   - `tools=[]` → 请求里没有工具定义；两者共同导致压缩请求命中不了父会话前缀。
5. `Session::noteStablePrefixHash` 已经能给出"前缀变化次数"，可直接用于 A2/A3 断言。
6. `KeyMetrics::noteCompaction` 有定义、有测试，主程序从未调用。
7. 测试模拟服务 `startDaSimServer` (`agent/test/core/test_agent.cpp`) 已经记录
   `g_da_sim_last_request` / `g_da_sim_requests`，可以在此加"模拟前缀缓存"。

### S1 注册表

新增 `agent/lib/include/agentxx/agent/prompt_sources.h` 与 `agent/lib/src/agent/prompt_sources.cpp`：

- `PromptSectionScope` (`Stable` / `Dynamic`) 对应"静态附加段 / 运行时文档段"，两者**分开存**，
  只统一视图 `PromptSectionView { key, owner, order, scope, text }`。
- `PromptRefreshReason` (`Explicit` / `ResourcesChanged` / `AfterCompaction`)。
- `PromptRefreshContext { sessionId, workDir, cancelToken }` (超时由来源自己声明)。
- `PromptSource { key, owner, order, scope, perSession, refreshOnCompaction, timeout, build }`，
  `build` 为 `std::function<asio::awaitable<std::optional<std::string>>(const PromptRefreshContext&)>`，
  返回 `nullopt` = 本次失败/保留旧文本，返回空串 = 段落清空。
- `PromptSourceRegistry`:
  - 登记/撤销: `registerSource` / `removeSource` (按 key + owner) / `sourceCount`;
  - 脏标记: `markDirty(sessionId, reason)` / `markAllDirty(reason)` / `isDirty`;
  - 刷新: `ensureFresh(ctx)` —— 先取 `sharedDirty_` 再取该会话的脏标记，聚合原因交给各来源，
    逐来源 `utilxx::asyncWithTimeout` + `utilxx_base` 超时值，异常经 `catchErrorAsync` 放行取消；
  - 读文本: `stableSections` / `dynamicSections` (按 order 排序) / `sectionText(sessionId, key)`;
  - 会话回收: `dropSession(sessionId)`；
  - 观测: `takeRefreshStats()` (取出即清零) / `snapshot()` (JSON, 不含正文)、`sessionCount`。
- 头文件**必须先 include `neograph/define.h`**：项目走 `NEOGRAPH_USE_BOOST_ASIO`，
  `asio` 是 `boost::asio` 的别名，只包含 `<asio/awaitable.hpp>` 拿不到该别名 (MSVC 下直接编译失败)。

`AgentContext` 新增:

- `std::shared_ptr<PromptSourceRegistry> promptSources` (构造即创建，随上下文生命周期)；
- `std::shared_ptr<const std::vector<neograph::ChatTool>> lastRequestTools`
  —— 上一次请求里真正发出去的工具定义，压缩子代理据此对齐前缀；
- `std::vector<PromptSectionView> promptSectionViews(sessionId)` —— 统一视图 (Config 段 + Session 段)。

### S2 来源接入

`MemoryFileMiddlewareHandle`:

- 构造时 `registerSource({key="memory", owner="core:memory", order=100, Stable, perSession=true,
  refreshOnCompaction=true, timeout=3000ms})`，析构 `removeSource`。
- 内容缓存改为 `PathSetCache`: 以"解析后绝对路径集合的 fnv1a64 指纹"为键，值是
  绝对路径 → `FileEntry{stamp(mtime+size), content, ok}`；路径集合超过 64 个时整体清空。
- 逐文件先读 `mtime/size` 快照，未变就不读盘；读盘走 asio `stream_file` 异步，失败兜底同步读。
- `memoryFilePaths` 为空 → 返回空串 (段落清空，而不是保留旧文本)；全部文件读取失败 → `nullopt`。
- `addMemoryFiles` / `removeMemoryFiles` 只改路径列表并 `markAllDirty(ResourcesChanged)`，
  不再在每轮 start 钩子里读盘。
- `onAgentcallStartFunc` 不再覆写 (改为默认空实现)。

`SkillMiddlewareHandle`:

- `registerSource({key="skills", owner="core:skills", order=200, Stable, perSession=false,
  timeout=5000ms})`，析构 `removeSource`。
- `scanSkillsAsync()`: 收集候选目录 + 记录 `SKILL.md` 的 `mtime/size` 快照 `scanSnapshot_`，
  快照未变则跳过扫描；同名裁决结果留在 `shadowedSkills_`；清单文本复用 `cachedListText_`。
- `addSkillDirs` / `removeSkillDirs` 只改目录列表 + `markAllDirty(ResourcesChanged)`。

### S3 组装改造

- `context.cpp`: 删除 `collectAppendSections`；`buildStableContextSections` /
  `buildDynamicContextSections` 改为读注册表 (`stableSections` / `dynamicSections`)；
  新增 `promptSectionViews`。
- `middlewares/middleware.h`: 删除 `graphDataKey_appendSystemMessageStable` /
  `graphDataKey_appendSystemMessage` 两个常量 (留退役说明注释)，段落不再经 graphData 传递。
- `modelcall.cpp`: "动态段来源已被移除"的告警文案改为指向 `PromptSource` / `prompt_sources.h`。

### S4 刷新入口

新增 `agent/lib/include/agentxx/middlewares/prompt_source_refresh.h` +
`agent/lib/src/middlewares/prompt_source_refresh.cpp`:

- `onModelcallStartFunc` 组装 `PromptRefreshContext` (`sessionId` / `workDir`
  = `getSessionWorkDir` / `cancelToken`) → `promptSources->ensureFresh(ctx)`；
- 发生刷新时 `metrics->notePromptRefresh(耗时, 变化段数, 失败数, 段落总字符数)`。
- `BaseAgent::initMiddleware` **最先** push 该 handle，保证刷新早于其它 start 钩子；
- 会话回收: `runInternalAsync` 的 `cleanupAfter` 分支与 `training.cpp` 的同类清理处都加了
  `promptSources->dropSession(sessionId)`。

### S5 压缩后标记

`summarization.cpp`:

- 自动压缩写回 + `persistNow("compaction")` 之后: `markDirty(sessionId, AfterCompaction)` +
  `metrics->noteCompaction(oldTokens, newTokens)`；
- 手动压缩 `persistNow("manual-compaction")` 之后同上。

（硬截断路径本身不改变提示词来源内容，故只记 `noteCompaction`，不额外标脏。）

### S6 资源变更与会话回收

见 S2 (`addMemoryFiles` / `addSkillDirs` 等只改列表 + `markAllDirty`) 与 S4 (`dropSession`)。

### S7 压缩请求前缀复用

目标：压缩请求 = 父会话可缓存前缀的**延长**，而不是另起一段。

- `event/events.h` 的 `SubagentBatchItem` 新增 `keepProvidedSystemMessage` /
  `inheritRequestTools` / `toolExecutionDisabled`。
- `agent_host.cpp` (`spawnOneTask` / `makeSubagentConfig`): 把三个开关写进子配置；
  `inheritRequestTools` 时取父/根会话的 `lastRequestTools`，非空则
  `enableToolFiltering = false`、`toolWhitelist.clear()`、`requestToolsOverride = parentTools`。
- `config.h` 新增 `keepProvidedSystemMessage` / `requestToolsOverride` /
  `toolExecutionDisabled` / `toolChoiceNoneSupported` (默认 true)，属"子代理瞬时字段，不落盘"。
- `modelcall.cpp`:
  - `build_params` 中 `requestToolsOverride` 非空则直接用它 (跳过本地工具与插件工具追加)；
    非空 `params.tools` 时写 `ctxPtr->lastRequestTools`；
  - `toolExecutionDisabled && toolChoiceNoneSupported && 非 anthropic` 时写
    `params.extra_fields["tool_choice"] = "none"` (不在前缀里，加它不影响对齐)；
  - `baseRun` 开头重建 system 时若 `keepProvidedSystemMessage && 已有 system 消息` 则跳过重建；
  - `callLLM` 中 `toolExecutionDisabled` 时清空 `tool_calls` 且按"没有工具调用"结束本轮
    (只有 tool_calls 无正文时记 WARN，不改错误语义)。
- `summarization.cpp`: 直派子代理的任务项显式填 `keep_provided_system` /
  `inherit_request_tools` / `disable_tool_execution` = true；总线 (batch) 路径同样带上这三个参数。
- `tools/subagent.cpp`: 两处解析读三个开关，缺省 false (保持既有语义)。

### S8 观测

- `util/observability.h/.cpp`: 新增 `notePromptRefresh(durationMs, changedSections, failures,
  sectionChars)` 与 getter `promptRefreshes()` / `promptChangedSections()` /
  `promptRefreshFailures()` / `promptRefreshMs()` / `promptSectionChars()`；
  `toJson()["prompt_sources"]`、`summary()` 追加 `prompt_refreshes=` / `prompt_changes=`；`reset()` 清零。
- `agent/assembly_snapshot.cpp`: `buildRuntimeSnapshot` 增加 `prompt_sources` 段
  (来自 `ctx.promptSources->snapshot()`，只有元数据没有正文)；`renderAssemblySnapshot` 增加
  `promptSources[...]` 行，metrics 行追加提示词计数。

### S9 测试

- 新增 `agent/test/include/agentxx-test/core/test_prompt_sources.h` +
  `agent/test/core/test_prompt_sources.cpp` (47 断言):
  T1 首次读取 / 无标记不读盘 / 压缩后内容不变 changed=0；T2 内容变化 changed=1；
  T3 `refreshOnCompaction=false` 与 `ResourcesChanged` / `Explicit`；
  T4 失败 / `nullopt` / 空串的语义；T5 `perSession` 隔离与 `dropSession`；T6 排序 / 撤销 / 快照。
- 新增 `agent/test/include/agentxx-test/core/test_prompt_cache_prefix.h` +
  `agent/test/core/test_prompt_cache_prefix.cpp` (25 断言，A1/A5/A7/A8):
  断言压缩请求的 system 与工具定义与父请求**逐字节相同**、messages 前缀一致、
  模拟缓存命中率 ≥ 0.9、`tool_choice == "none"`、压缩后下一请求 system 不变、
  压缩请求不执行任何工具 (请求数增量恰 1、无 `tool` 角色消息)。
- `agent/test/core/test_prompt_stability.cpp`: `TestDynamicSectionHandle` 改为构造时登记来源、
  析构时撤销；新增 `test_refresh_after_compaction()` (A4 无监听 / A2 压缩后内容不变则 system
  逐字节不变且 `stablePrefixChanges == 0` / A3 内容变化后 `stablePrefixChanges == 1`，
  并检查 metrics 与加载快照的 `prompt_sources` 不含正文)。
- `agent/test/core/test_graph_data.cpp`: 改用局部键 (`xx_testSectionMap` 等)，不再用已删除的通道常量。
- `agent/test/plugin/test_plugin_resources.cpp`: T6 改为 `ensureFresh` +
  `promptSources->sectionText("memory", sessionId)`，并断言中间件析构后
  `removeSource("memory", "core:memory")` 返回 false。
- `agent/test/core/test_agent.cpp` 模拟服务新增"模拟前缀缓存":
  `g_da_sim_prefix_cache_enabled` / `g_da_sim_last_cache_ratio` /
  `g_da_sim_last_cached_tokens` / `g_da_sim_last_prompt_tokens`；按 4 字节 ≈ 1 token 估算，
  工具定义一致时逐条比较 messages 公共前缀，命中量写进 `usage.prompt_tokens_details.cached_tokens`
  (流式与非流式都写)。
- `agent/test/test.cpp` 注册 `prompt_sources` / `prompt_cache_prefix` 两个模块。
- 模拟缓存实测 (测试输出): 第 2 轮普通请求 2060/14379 (0.143), 压缩请求 14386/14842
  (0.969) —— 压缩请求几乎整段复用父会话前缀。

### S9 文档

- `docs/zh-cn/design/index.md`: 架构设计新增 "6. 系统提示词加载: 来源注册与按需刷新"
  (分组、来源字段、唯一刷新入口、会话回收、观测、无文件监听、压缩前缀复用);
  `AgentContext` 结构图补 `promptSources`。
- `docs/zh-cn/design/plugins.md`: 接口表后加"提示词段落来源 (第二期留位)"说明 ——
  插件目前只能经 `set_prompt` 贡献文本, 计划在 `agentxx.agent.prompt` 表尾追加
  `set_session_section` / `remove_session_section` / `request_refresh`。
- `docs/zh-cn/design/benchmark.md`: 新增 "18. 压缩请求的前缀复用: 模拟前缀缓存实测",
  含度量方式、结果表、复现命令与真实 provider 待补表。


### 真实 provider 实测 (2026-10-17)

配置取自工作目录 `agentxx-config.yaml` + `%APPDATA%/agentxx/.env`，模型
`deepseek/deepseek-v4.1-flash`（OpenAI 兼容网关），上游回报缓存命中量。

- 普通轮次（历史越长命中越高）：`prompt 12100 / cached 8576 = 0.709` →
  `prompt 45855 / cached 42368 = 0.924`；相同前缀的重复请求 `25472/25619 = 0.994`。
- 压缩后父会话第一条请求：`cached 5248 / prompt 5726 = 0.917`，命中的正是
  system + 工具定义段 → 压缩前后前缀仍然对齐。
- 压缩子代理请求的用量**没进台账**（子代理 `AgentContext` 没有 `sessionStore`），
  所以 (b) 一栏读不到，已记为待办。
- 实测中暴露两个既有问题（与本方案无关）：
  ① `jsonl` / `server` 模式没有 `AgentHost`，自动压缩委派会报
  `service.subagent ... no server registered` 并退化为硬截断（进程内 `cli`/`tui` 与 FFI 正常）；
  ② `cli` 模式每次启动生成新会话 id，跨进程续聊只能用 `jsonl`，而 `jsonl` 恰好缺 `AgentHost`。

详见 `docs/zh-cn/design/benchmark.md` 第 18.4 节。

### S10 两个后续项 (本轮实施)

#### S10.1 子代理用量计入父会话

- `AgentContext` 新增 `usageAccountSessions` + `usageAccountSessionId` (记账目标)。
- `AgentHost::spawnOneTask`: 派生时把父上下文的记账目标写进子上下文; 嵌套委派沿用
  最外层目标 (一次调用只记一笔); 自动压缩经总线到达 (`spawnBatch(req)` 不带父上下文) 时
  回退到根 agent 的会话存储 + 请求里的 `parentSessionId`。
- `modelcall.cpp` 的 `recordUsage`: 有记账目标就记到它 (台账 `usage` 表 + 会话累计用量),
  否则按本 agent 自己的会话。
- 效果: 压缩请求的 `prompt / cached` 现在能在父会话台账里查到
  (实测 `39300 / 0`, 见下); 子代理工具调用的用量同样归到父会话。

#### S10.2 `jsonl` / `server` 接入主程序 (AgentHost)

- 新增公共入口 `AgentHost::attachRootAgentHost(rootAgent)` (= `create` + `attachRoot`,
  `ioCtx` 取根 agent 的), 注释写明"与 `enableSubagent` 无关, 返回值必须活到会话结束"。
- `jsonl_mode.cpp`: 去掉 `enableSubagent` 门控, 改调公共入口。
- `main.cpp` (server 模式): init 后加载, 句柄由信号协程持有到进程退出, 关机时释放。
- `mode_runners.cpp` (进程内 tui/cli): 改为调用同一入口 (行为不变)。
- 效果: `jsonl` 里自动压缩不再报 `service.subagent ... no server registered`。

#### 测试

- `prompt_cache_prefix`: 打开会话持久化 (`sessionStoreDirectory` = 临时目录), 新增断言
  ⑦ 手动压缩的请求用量落在父会话台账 (prompt/cached 与模拟器上报值一致),
  ⑧ 自动路径 (`spawnBatch(req)` 不带父上下文) 也回退到根会话台账。→ 34 断言通过。
- `jsonl_runner`: 新增用例 `testJsonlRunnerAttachesAgentHost` (关掉 `enableSubagent`,
  在轮次结束那一行写出时检查 `agentContext->host` 仍然存活)。→ 18 断言通过。
- 全量: `passed=38126 failed=1`, 唯一失败是 `boundaries` 的用词检查
  (我在 index.md 写了停用词"宿"+"主"), 已改写; 复跑 `boundaries` 14/14 通过。

#### 重测 (修复后)

| 请求 | prompt | cached | cached/prompt |
|---|---|---|---|
| 前一条普通请求 (第 10 轮) | 39090 | 35712 | 0.914 |
| 压缩请求 (子代理) | 39300 | 0 | **0.000** |
| 压缩后父会话第一条 | 5720 | 5248 | 0.917 |

压缩请求的用量已经记下来了, 但该网关/模型回报的缓存命中为 0, 与模拟器 (0.969) 不一致;
已排除"缓存写入延迟"(上一条 36K 前缀在 2.7 秒前发出且被本次命中), 最可疑的是压缩请求
多出的 `tool_choice: "none"`。复测办法见 `benchmark.md` 18.4 与下方待办。

### S11 压缩请求缓存不命中: 定位与修复 (第三轮)

#### S11.1 先证伪 `tool_choice` 猜测

- 删掉压缩请求注入的 `tool_choice: "none"`, `AgentConfig::toolChoiceNoneSupported`
  字段一并删除 (不执行工具本来靠 `callLLM` 忽略 `tool_calls` 就够), 测试 ⑤ 改为断言
  "压缩请求与父请求的 `tool_choice` 完全一致"。
- 真实 provider 重测: 压缩请求**仍是 `cached = 0`** —— 说明差异不在该字段, 猜测作废。

#### S11.2 加请求前缀指纹日志 (排查手段)

`modelcall.cpp` 的请求加载末尾 (debug 级, 不含正文):

```
[modelcall] request prefix (session=...): msgs= chars= system=<hash> tools=<hash>(<n>)
  head=<hash> toolOverride=<yes|no> toolsDisabled=<yes|no> first=[ i:role:chars/<hash> ... ]
```

用它逐请求对比, 直接看出"工具段数量不同""消息列表少了第 1 条"这类分叉。

#### S11.3 真实原因一: 自动压缩路径丢掉三个前缀对齐开关

- 自动压缩的调用路径: `topic service.subagent.execute` → `SubAgentManagerTool::execute_async`
  → 中断参数 → `AgentRunner` → `parseSubagentBatchFromInterrupt` → `AgentHost::spawnOneTask`。
- `tools/subagent.cpp` 里工具自己的 `TaskArg` **只解析了一部分字段**, 没有
  `keep_provided_system` / `inherit_request_tools` / `disable_tool_execution`;
  这三个开关在写回中断参数时也没带上, 于是 `AgentHost` 一律按 false 处理:
  - 子代理按自己的配置重建 system (缺运行时段落)
  - 子代理没继承父请求的工具定义, 用自己的工具集 (过滤后 middleware 又追加
    `agentxx_share_store`, 共 1 个)
  结果请求从工具段就分叉: 父请求 `tools=…(8)`, 压缩请求 `tools=…(1)`。
- 修复: `TaskArg` 补三个字段 + 解析 + 条件写回中断参数 (缺省不写 = false 保持旧行为);
  `subagent_tool` 新增 S3.C 用例锁住"解析 → 中断参数 → 再解析"整个过程。
- 手动压缩走 `AgentHost::spawnBatch` 直派 (不经过工具), 所以一直是正常的 —— 这也解释了
  "手动命中 0.969、自动命中 0"的差异。

#### S11.4 真实原因二: 压缩载荷裁剪阈值过紧

- 压缩请求载荷 = 父会话刚发成功的同一批消息 + 末尾一条指令, 但裁剪用的是
  `countTokens(...) > modelMaxToken * 0.95`, 而本项目的 token 估算在这类中英混排负载上
  比上游实际计费高约 25%: 自动压缩在 75% 处触发时估算值已越过 0.95 线, 于是每次都
  先丢掉最旧一条消息 (实测压缩请求少了第 1 条 user 消息), 前缀从第一条起就分叉。
- 修复: 裁剪上限改用模型上下文上限本身 (不乘 0.95); 真超限由上游报错后按失败计数
  走硬截断兜底。注释里写明了原因 (避免后人又加回余量)。

#### S11.5 真实原因三: 压缩前清理 thinking

- 修完上面两条后命中仍只有 system + 工具段 (6784/24915)。原因: 模型配置
  `send_thinking: true` 时请求体里每条 assistant 都带 `reasoning_content`
  (`chatMessagesToOpenAIJson` 逐条写入), 而压缩前会调 `cleanThinkingMessages()`
  只留最新一条 thinking → 压缩请求与父会话上一次请求从第一条 assistant 起就不一样。
- 定位手段: 把逐条消息指纹的哈希改成覆盖"正文 + thinking + 工具调用"
  (原来只哈希正文, thinking 的差异看不见), 长度也一并打出来 (`chars+thinking/chars`)。
- 修复: 删除 `cleanThinkingMessages` (函数 + 声明 + 两处调用)。压缩请求就是要原样重发
  父会话刚发的那批消息, thinking 本来就被父请求发过, 不会多出成本。
- 测试: `summarization` 移除 T11a (该函数的用例), T11 的 ⑤ 改为"全部 thinking 原样保留"
  (index 2 = think-1, index 7 = think-3)。

#### S11.6 修复后实测

| 请求 | prompt | cached | cached/prompt | 指纹 |
|---|---|---|---|---|
| 压缩前普通请求 (第 5 轮) | 19071 | 17536 | 0.920 | tools `…(8)` |
| 压缩请求 (子代理) | 20943 | 18944 | **0.905** | tools `…(8)` `toolOverride=yes toolsDisabled=yes`, 前 6 条消息指纹与上行逐条相同 |
| 压缩后父会话第一条 | 6130 | 5248 | 0.856 | — |

- 压缩请求命中 = 父请求的 token 数 (19071 → 18944, 差值是上游缓存块对齐), 整段前缀被复用;
  修复前同一位置是 6784/24915 = 0.272 (只剩 system + 工具段)。
- 回归: `summarization` 457、`subagent_tool` 143、`prompt_cache_prefix` 33、`boundaries` 14 通过。

## 遇到的问题与处理

1. **新头文件在 MSVC 下找不到 `asio` 别名**
   现象: `prompt_sources.h` 里写 `asio::awaitable<...>` 报"未定义标识符 asio"。
   原因: 项目经 `NEOGRAPH_USE_BOOST_ASIO` 让 `neograph/define.h` 定义
   `namespace asio = ::boost::asio`，只包含 `<asio/awaitable.hpp>` 不会建立该别名。
   处理: 新头文件第一行 `#include "neograph/define.h"`。
2. **`utilxx_base::eraseHeterogeneous` 找不到**
   处理: 补 `#include "utilxx_base/container_util.h"`。
3. **测试二进制崩溃 (ASan: heap-buffer-overflow / access-violation)，怀疑 ABI 不一致**
   现象: `agent` 模块 ASan 报 `base_agent.cpp:65` 读 `in_config->enableSessionStore` 越界
   ("14 bytes after 2128-byte region")，`prompt_stability` 在 `AgentConfig` 析构里
   `_Decref` 崩溃，`session_sync` 大片超时失败。
   原因: **测试工程的目标文件是过期产物** —— 测试 include 的是 install 目录里的头文件，
   而 `INSTALL` 复制时保留源文件 mtime，于是"编辑头文件 (17:29) → 更早一次 INSTALL →
   测试在 18:07 编译时用的是旧副本"之后，头文件时间戳仍比 obj 旧，增量构建判定为"已最新"，
   测试 TU 继续按旧 `AgentConfig` 布局编译，而库是新布局。
   处理: 删除测试工程的 `agentxx_test.dir` 中间目录后整体重编 (156 个 obj)，之后
   `agent` / `prompt_stability` / `session_sync` 等全部通过。
   教训: 改过 `agent/lib/include/**` 后，若测试/客户端工程"看起来已最新"却出现诡异的
   内存错误，先删对应工程的中间目录重编。

## 约束 (实施期间遵守)

- 不引入文件监听 / 轮询；`mtime`/`size` 快照只用于"被要求刷新时省去读盘"。
- 刷新唯一入口 = `PromptSourceRefreshHandle::onModelcallStartFunc`。
- 静态附加段与运行时文档段**不合并存储**，只统一视图。
- 不改 `agent/third_party/**`；本期不加插件接口表成员 (插件面留第二期)。
- 段落视图 (`PromptSectionView`) 里的 `string_view` 指向注册表 / `AgentPrompt` 内部存储，
  只在本次调用内有效，不得跨 `co_await` 持有。
- `perSession = true` 的来源按会话存文本，必须由会话回收路径 (`dropSession`) 释放。

## 提交记录

| 提交 | 内容 |
|---|---|
| `3b8a1520` | 提示词来源注册表: 静态附加段改为按需刷新 (S1–S8 库侧改动) |
| `92c920a3` | 测试: 提示词来源刷新与压缩前缀复用 (S9 测试) |
| `3a4656d3` | 文档: 提示词来源注册与压缩前缀复用 (index / plugins / benchmark / work.md / AGENTS.md) |
| `854c2052` | 小修: 段落变化日志与统计单位对齐 |
| `a5129e27` | 记录实施提交与待办 (work.md) |
| `98ba5ff7` | 实测: 真实 provider 的前缀缓存命中 |
| `17702e21` | S10: 子代理用量计入父会话 + `jsonl`/`server` 接入主程序 + 测试 + 文档 |
| `b14d2d76` | S11: 撤销 `tool_choice=none` 注入 + 自动压缩路径补三个前缀对齐开关 + 压缩载荷裁剪阈值 + 请求前缀指纹日志 + 测试 + 文档 |

## 待完成任务

1. 第二期: 插件面提示词来源 (`agentxx.agent.prompt` 表尾三个成员), 见
   `docs/zh-cn/design/plugins.md` 的"提示词段落来源 (第二期留位)"。
2. (可选) 上游网关分流导致的缓存命中波动: 本项目能控制的部分已到位 (请求体逐字节对齐、
   载荷不再无谓裁剪), 剩余波动需要 provider 侧定位, 无代码改动点。
