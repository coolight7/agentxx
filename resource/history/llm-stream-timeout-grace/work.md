# 实施记录: 长工具调用被判为读取超时

进度: 8 / 8

## 任务列表

- [x] 1. 建任务目录（index/plan/work）
- [x] 2. cxx_utilxx: `RequestConfig.readChunkTimeoutPerRead` + `read-body` 循环取样
- [x] 3. `provider_common.h`: `StreamProgress` / `streamFinishGraceTimeout` /
      `llmSendTimeout` / `streamFailureSuffix`
- [x] 4. `openai_provider`: 统计出参 + 宽限钩子 + `finishSeen` 收尾 + 诊断 + 发送期限
- [x] 5. `anthropic_provider`（含内联解析器）: 同上 + `stop_reason` 记录
- [x] 6. 测试（http / openai_provider / anthropic_provider）
- [x] 7. 文档 `docs/zh-cn/design/index.md`
- [x] 8. 构建 + 相关模块回归 + 全量回归

## 步骤记录

### 1. 任务目录

`index.md` / `plan.md` / `work.md` 建立, 提交 `f471140e`。

### 2. 工具库 `cxx_utilxx`（独立子模块）

- `include/utilxx/http_client.h`: `RequestConfig` 末尾追加
  `std::function<std::optional<std::chrono::milliseconds>()> readChunkTimeoutPerRead`
  （结构体注释同步补一条语义说明；放末尾、有默认值，已有聚合初始化不受影响）
- `src/http_client.cpp`: `requestSseAsync` 的 `read-body` 循环前新增
  `nextBodyReadTimeout()` —— 每次读之前取样，空/返回 `nullopt`/非正值都退回
  `readChunkTimeout`
- 子模块内提交 `847b0de`，父仓库记录新指针
- 验证: 顶层 `cxx_utilxx_repo` 带 `BUILD_ALWAYS/INSTALL_ALWAYS`，构建时自动重编重装
  （无需手工删构建目录，这一点比预期省事）

### 3. `provider_common.h`

新增 4 个共用件（都 `inline`，便于单测直接断言）：

- `struct StreamProgress`：`startedAt` / `lastDataAt` / `bytes` / `events` /
  `finishSeen` / `finishReason` + `noteChunk()` / `idleMs()`
- `streamFinishGraceTimeout(readChunkTimeout)`：`max(10s, t/5)`，并以 `t` 为上界
  （见"与计划不一致"第 1 条）
- `llmSendTimeout(bodyBytes)`：`max(60s, ceil(body/16KiB) 秒)`
- `streamFailureSuffix(progress, toolCalls)`：单行 ASCII 诊断后缀

### 4. `openai_provider`

- 头文件：`processSseBuffer` / `processSseLine` / `processResponsesSseBuffer` /
  `processResponsesSseLine` 末尾加 `StreamProgress* progress = nullptr`；
  头文件改为包含 `provider_common.h`
- `processSseLine`：解析成功后 `++progress->events`；`finish_reason` 非空时记录
  `finishSeen` + 原文（`completion.stop_reason` 的映射行为不变）
- `processResponsesSseLine`：只记事件数（Responses 没有"生成结束"中间事件）
- `doStream`（chat completions）：
  - `StreamProgress progress` + `salvagedOnError`（避免收尾日志重复）
  - `.sendTimeout = llmSendTimeout(bodyStr.size())`（在 `std::move(bodyStr)` 之前取）
  - `.readChunkTimeoutPerRead`：`finishSeen` 之前返回 `nullopt`，之后返回收尾宽限
  - 异常回调：`doneReceived` → 原行为；`finishSeen` → 记 warning 并按完整收尾；
    其余 → 附诊断后缀后抛出（`API error (HTTP ...)` 不加后缀）
  - 末尾判定：`!doneReceived && finishSeen` → warning 后继续；两者都假 → 抛截断（带后缀）
- `doStreamResponses`：发送期限 + 事件统计 + 诊断后缀（结束判定不变）
- 非流式两处 `RequestConfig`：显式 `sendTimeout`

### 5. `anthropic_provider`

- 头文件（解析器内联在头里）：`processSseBuffer` / `processSseBlock` 加
  `StreamProgress*`，逐事件计数；`message_delta` 里读 `delta.stop_reason` 记录
  `finishSeen` + 原文（`completion.stop_reason` 仍不写，保持历史行为）
- `doStream`：宽限钩子 + `finishSeen` 收尾 + 诊断后缀 + 发送期限，与 openai 同构
- 非流式一处 `RequestConfig`：显式 `sendTimeout`

### 6. 测试

- `test_http.cpp`：`test_http_client_sse_per_read_timeout` —— 首事件前用默认 5s，
  之后收紧到 300ms，服务端静默 → 报 `timeout after 300 ms` 且总耗时 < 3s
- `test_openai_provider.cpp`
  - `test_stream_finish_grace_and_send_timeout_values`（纯函数取值表：
    60→12s / 100→20s / 10→10s / 3→3s / 200→40s；发送期限 0→60s / 1MiB→64s / 16MiB→1024s）
  - `test_sse_progress_finish_reason_and_events`（事件计数 + `finishReason` 原文）
  - `test_streaming_finish_reason_without_done_accepted`（结束原因到了但没有 `[DONE]`）
  - `test_streaming_stall_after_finish_accepted`（结束原因后静默，宽限到点按完整收尾）
  - `test_streaming_stall_timeout_diagnostics`（无结束原因的静默仍报错，且带
    `recv=` / `events=` / `idle=` / `finish=none`）
  - 新增 `StallSseServer`（原始 TCP：先写 SSE 再保持静默），复用 `MockOpenAIServer`
- `test_anthropic_provider.cpp`：`test_streaming_stop_reason_without_message_stop_accepted`
- 既有用例 `test_streaming_missing_done_throws` / `test_streaming_missing_message_stop_throws`
  不含结束原因，语义不变，未改动

### 7. 文档

`docs/zh-cn/design/index.md`：
- 模型配置示例的 `read_chunk_timeout` 补充块间超时语义、收尾宽限与调大建议
- 新增 `#### 8. LLM 流式请求的超时与收尾 (2026-11)`（发送期限 / 收尾宽限 / 收尾判定 / 诊断）

## 验证

- 构建：`cmake --build agent/build/windows-debug --config Debug --target agentxx_lib_repo`
  → 通过（只有既有的 C4100/C4456/C4702 警告）
- `cmake --build ... --target agentxx_test_repo` → 通过
- `agentxx_test --log-level warn http openai_provider anthropic_provider llm_error`
  → `passed=1322 failed=0`（改完收尾与诊断的整理后又跑了一次，同样全过）
- 全量 `agentxx_test --log-level warn` → **`Total: passed=42753 failed=0`**
  （Debug + ASan，约 23 分钟）
- 实跑日志确认三条路径都生效：
  - `LLM stream missing [DONE] after finish_reason=tool_calls; accepted | ...
    recv=374B events=3 idle=0ms finish=tool_calls tools=1 args=16B name=agentxx_filesystem_write`
  - `LLM stream ended without [DONE] after finish_reason=stop; accepted | ...
    err=SSE read-body timeout after 2000 ms`
  - `LLM stream missing message_stop after stop_reason=end_turn; accepted | ...`
- 全量 `agentxx_test` 结果见下（后台运行）

### 复审后又改的两处

1. **取消优先**：两个 provider 的流错误回调开头加
   `params.cancel_token->throw_if_cancelled("llm stream")`。不加的话，用户取消恰好
   落在“结束原因已到、结束标记未到”之间时，会先命中我们的收尾分支，把一次已取消的
   调用当成成功结果继续往下跑。`catchErrorAsync` 只把控制流异常交给 `onRethrow`，
   而取消真实发生时底层错误可能已被本地 `close` 转成普通超时文本，所以显式查一次令牌。
2. `streamFailureSuffix` 遍历改用 `entry.second`（去掉 `(void)idx;`），宽限函数的
   中间量命名理顺。

## 与计划不一致的地方

1. **收尾宽限加了上界**：计划写的是 `max(10s, t/5)`，实现为
   `max(min(10s, t), t/5)`。原因：`read_chunk_timeout` 配得比 10 秒还短时，
   原式会让"收尾"比正常间隔等得更久，与"短超时"的意图相反；默认 60 秒仍是 12 秒，
   100/200 秒分别仍是 20/40 秒。副作用是"宽限比基准更短"的场景只能出现在
   `t > 10s`，因此没有写"宽限到点比基准更早"的集成用例（等待时间至少 10 秒），
   改为：值为纯函数单测 + HTTP 层钩子单测 + 静默集成用例三层覆盖。
2. Anthropic 的 `completion.stop_reason` 仍不被流式路径填写（历史遗留，与本次问题无关），
   只在 `StreamProgress.finishReason` 里记录原文。
3. Responses API 只加了发送期限与诊断，没有 P1（它没有"生成结束"的中间事件）。

## 剩余事项

- P4：按静默阶段拆分阈值（等本轮上线后评估）
- 可选后续：Anthropic 流式路径把 `stop_reason` 也写进 `completion.stop_reason`
  （现在只有非流式路径写）
