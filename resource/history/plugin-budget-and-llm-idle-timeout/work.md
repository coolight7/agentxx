# 实施记录

## 进度

- [x] 现状调查：插件超预算告警来源、LLM 流超时链路
- [x] `cxx_pluginxx`：超预算告警限频 + 记录表 + 快照
- [x] `provider_common.h`：`StreamProgress` 输出打点 + `StreamOutputWatchdog`
- [x] 三个 provider 接线（openai chat / openai responses / anthropic）
- [x] 配置键 `model.list[].output_idle_timeout`（默认 600，0=关闭）
- [x] 单测：`openai_provider` 的看门狗语义与打点、`anthropic_provider` 的 ping 不算输出、
      `plugin_executor` 的超预算记录
- [x] 编译（windows-debug）+ 全量测试通过
- [x] 文档：`index.md`（中/英 yaml 样例 + 超时链路小节）、`plugins.md` §15.3、`AGENTS.md`、
      配置键生成物

## 每步改了什么 / 怎么验证

### 1. 插件回调超预算告警限频（`agent/third_party/cxx_pluginxx`）

- `include/pluginxx/runtime/runtime.h`：`PluginCallbackStats` 增加
  `OverBudgetRecord`（窗口内次数 / 最长耗时 / 窗口起点 / 上次 WARN 时刻）、
  `overBudgetMutex` + `overBudgetRecords`（key = `插件名|回调名`）与
  `overBudgetTop(limit)` 快照。
- `include/pluginxx/runtime/instance_base.h`：新增 `notePluginCallbackOverBudget`；
  `runPluginDispatch` 的 `overBudget++` 保持不变，告警改为经它限频：
  窗口 5 分钟内第 1 次只记 DEBUG，第 2 次起每个窗口最多一条 WARN（带次数与最长耗时）。
  记录表上限 512，满了先清过期项。
- `agent/lib/src/agent/assembly_snapshot.cpp`：`plugin_executor` 段追加
  `over_budget_top`（按最长耗时降序，最多 5 条）；`--dump-diagnostics` 文本各列前 3 条。

验证：`agentxx_test plugin_executor`（第 3 段扩了断言：同一 label 再超预算两次后
`overBudget == 3`、`overBudgetTop` 记到 `provider|test slow` count=3、max ≥ 7 ms）。
实跑日志里只剩第 2 次开始的那条 WARN（
`callback `test slow` exceeded the 1 ms budget 2 time(s) in 5 min (max 22 ms)`），
单次超预算不再出 WARN。

### 2. LLM 流"无有效输出"看门狗（`agent/lib`）

- `include/agentxx/protocol/provider_common.h`：
  - `StreamProgress` 增加 `lastOutputAt` / `noteOutput()` / `idleSinceOutput()`；
  - 新增 `StreamOutputWatchdog`：`limit` / `logInterval` / `model` /
    `nextReadTimeout(progress, base)` / `tripped(progress)`，等待期每 60 s 一条 WARN（最多 8 条）。
- `OpenAIProvider::processSseLine`（chat completions）：思考分片、正文分片、
  `tool_calls` 的名称/参数分片处 `noteOutput()`。
- `OpenAIProvider::processResponsesSseLine`：`emitContent` / `emitThinking`、
  `response.function_call_arguments.delta`。
- `AnthropicProvider::processSseBlock`：`text_delta` / `thinking_delta` / `input_json_delta`。
- 三个流式入口：`readChunkTimeoutPerRead` 统一走看门狗
  （`base = finishSeen ? 收尾宽限 : readChunkTimeout`），Responses 路径原本没有该钩子，现补上；
  失败分支里 `tripped()` 时改写错误文本为
  `LLM stream idle timeout: no output for N ms (limit M ms) | model=...` 并记 WARN。
- `ModelConfig::outputIdleTimeoutSeconds`（默认 600）+ `config_loader.cpp` + `config_keys` 条目；
  重新生成 `agent/schema/config-keys.json` 与 `docs/zh-cn/design/config-keys.md`
  （`AGENTXX_UPDATE_CONFIG_KEYS=1`）。

验证：
- `agentxx_test openai_provider`（564 通过）：新增 `test_stream_output_watchdog`
  覆盖"关闭上限原样返回 / 上限内取较小者 / 到期返回 1 ms 且 tripped /
  心跳与空 delta 不刷新输出 / 正文与工具参数分片刷新输出"；新增
  `test_stream_idle_timeout_end_to_end`（真实 TCP 服务端按 chunked 每 50 ms 只发
  `: keep-alive` 注释与空 delta 事件、`output_idle_timeout: 1`）——实跑日志:
  `LLM stream idle timeout: no output for 1002 ms (limit 1000 ms) | model=gpt-4o-mini
  recv=420B events=7`，请求按失败结束而不是一直挂着。
- `agentxx_test anthropic_provider`（292 通过）：新增 `test_anthropic_ping_is_not_output`。
- 既有用例 `test_streaming_finish_reason_without_done_accepted` 等仍按原语义收尾（salvage 在
  idle 判定之前），日志与过去一致。

### 3. 编译与全量测试

- `agent/script/windows_debug_build.bat`（`AGENTXX_BUILD_PARALLEL=16`）增量构建通过；
  仅既有警告，无新增错误。
- `agentxx_test --log-level error` 全量：`Total: passed=44076 failed=0`。

## 遇到的问题 / 备注

- 第一次写单测时工具调用的 JSON 少了一个闭合 `}`（raw string 拼接漏了根对象的 `}`），
  `processSseLine` 按"畸形 JSON 跳过"处理导致"没有刷新输出"断言失败；已改成完整转义字符串
  并补上 `tcMap[0].arguments` 断言。
- `cxx_pluginxx` 是子模块：改动落在 `agent/third_party/cxx_pluginxx` 的工作树里，
  父仓库显示为 `m`。提交时需要在子模块内先提交。
- 没有加"单次尝试总时长"硬顶：会误砍合法的长生成。需要更强约束时用
  `output_idle_timeout` 调小即可（它只按"没有有效输出"计时）。

## 剩余（可选）事项

- TUI「添加模型配置」表单暂未暴露 `output_idle_timeout`（yaml 可配，默认 600 生效）。
- 若后续出现"上游持续发**空 delta 事件**但流仍不结束"的真实样本，可考虑在用满
  `output_idle_timeout` 后再加一层总时长上限（需用户确认默认值）。
