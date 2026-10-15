# 修复 opencode zen (deepseek 思考模式) 回传历史思考缺少 reasoning_text 导致的 HTTP 400
- 难度: B
- 类型: bug修复
- 基于commit: 3b75713a8c49cb8207a698240846b4f90229b8ee
- 时间: 2026-10-15 19:40
- 需求:
```md
请排查，当使用 agentxx-config.yaml 中定义的 opencode-ds-flash 时，偶尔会提示这个然后停止，
请分析是否是 agentxx 的问题，能否优化修复:
[提示] # 错误
Node 'llm' failed after 1 attempt(s): API error (HTTP 400):
{"model":"deepseek-v4.1-flash","error":{"message":"The `reasoning_text` in the thinking mode must
be passed back to the API. (request_id: ...)","type":"invalid_request_error","param":null,
"code":"invalid_request_error"}}；另外这个问题经常多发于多个agent同时使用这个模型配置时
```
（`opencode-ds-flash` = `type: openai-responses` + `send_thinking: true`，上游
`https://opencode.ai/zen/go/v1`）

## 现象与根因

Responses API 的思考模式下，上游要求把历次 assistant 思考内容回传；agentxx 回传时丢掉了
明文思考，上游按"必须回传 reasoning_text"拒绝整个请求。具体两处:

1. **捕获条目时只留指针**: 解析 `output_item`（流式）与 `output`（非流式）时，reasoning item
   只保存 `{type, encrypted_content, id}`，`content` 里的 `reasoning_text` 明文被丢弃
   （明文另存在 `message.reasoning_content`，但回传路径没用它）。
2. **降级分支发的是摘要**: 没有捕获条目的历史消息（跨 provider 历史 / 旧会话数据 / 模型切换后
   的历史），降级分支发的是 `summary:[{type:"summary_text",...}]`，同样不含 `reasoning_text`。

上游的校验是"当前续写所依据的那条 reasoning item 必须带 reasoning_text"（实测：只给最后一条
条目补明文即可通过，全部条目都不补则 400）。

"偶尔出现、多个 agent 同时用更明显"来自上游路由: opencode zen 会把同一模型分派到不同后端
（`x-opencode-session` 会话亲和），部分后端返回带 `encrypted_content` 的指针条目、要求回传
明文，另一部分只当作提示不校验。请求落到严格要求明文的后端时，如果回传的条目里没有
`reasoning_text` 就整轮失败（400 归为"请求非法"，不重试，于是直接停止）。

## 复现与验证（真实上游，非猜测）

用会话 `sess-16eb735745840-21368-e7b04772-0000` 失败前的上下文前缀回放到同一上游，按回传内容
构造不同组合（同一 session id，命中同一后端）:

| 回传方式 | 结果 |
|---|---|
| 现状: 条目只带 `{type, encrypted_content, id, summary:[]}` | **HTTP 400 "The \`reasoning_text\` ... must be passed back"** |
| 同上 + `content:[{type:"reasoning_text",text}]` | 通过 |
| 同上 + content + `status:"completed"` | 通过 |
| 全部条目改成 `summary:[{type:"summary_text",...}]` | **HTTP 400（同上）** |
| 全部条目改成 `content:[{type:"reasoning_text",...}]`（不带指针，仅重建） | 通过 |
| 完全不回传 reasoning item | **HTTP 400（同上）** |
| 只给最后一条条目补 content，其余不补 | 通过 |

结论: 回传条目必须带 `reasoning_text`；重建（无指针）条目同样可用，但内容必须是
`reasoning_text` 而不是 `summary_text`。

## 修改

- `agent/lib/src/protocol/openai_provider.cpp`
  - 新增 `reasoningItemHasText()` / `reasoningTextContent()` 两个内部辅助函数
  - `buildResponsesBody` 回传捕获条目时: 条目里没有明文思考且消息有思考文字
    (`message.reasoning_content`) 时补 `content:[{type:"reasoning_text",text}]`；
    条目自带明文时保持原样（部分网关只在条目里给明文）
  - 降级分支（无捕获条目）: 由 `summary_text` 改为 `content/reasoning_text` + 空 `summary` 数组
- `agent/lib/include/agentxx/protocol/openai_provider.h`
  - `kResponsesReasoningItemsKey` 注释说明"只存最小字段，明文由 `reasoning_content` 回填"
- `agent/lib/include/agentxx/agent/config.h`
  - `requestReasoningSummary` 注释同步（关闭后不再用思考文字重建 reasoning 输入）
- `agent/test/core/test_openai_provider.cpp`
  - 新增 `test_responses_reasoning_text_passed_back_with_item`（4 个场景: 补明文 / 自带明文不覆盖 /
    无明文不补 content / `sendThinking=false` 不回传）
  - 更新 `test_responses_send_thinking`（降级分支改为断言 `reasoning_text`）

## 验证

- `agentxx_test.exe openai_provider` → passed=522 failed=0
- 相关模块回归: `anthropic_provider` / `fake_provider` / `event_bridge` / `event_stream` /
  `summarization` / `session_persistence` / `feature_points` → 合计 passed=2252 failed=0
- 真实上游复核: 见上表（同一会话前缀，改前 400 / 改后通过）

## 备注

- 请求体积/输入 token: 之前条目只带指针, 明文思考不在请求里; 现在每条带条目的历史消息都会把
  明文思考放进请求 (实测只有"最后一条条目"是必须的, 但为避免依赖该细节, 这里逐条补齐)。
  这段文字是稳定的对话前缀, 命中上游前缀缓存时按缓存价计费; 若要在意输入体积, 可改成只给
  最后一条带条目的消息补明文 (需确认目标网关都只校验最后一条)。
- 未做: 条目自带明文而消息 `reasoning_content` 为空的网关 (如部分代理只在
  `output_item.done` 里给明文、不发增量事件) 仍无法补明文；若要覆盖，可在捕获时保留条目
  自带 `content`，代价是思考文字在会话里存两份（长会话里思考文字约占上下文 1/4）。
- 未做: 把该 400 归入可重试错误。修复后同一请求不会再因此失败；重试相同请求仍然会失败。
  （相关: 本轮另加了"网络/传输层故障"错误类 `LlmErrorKind::Network`，见
  `resource/history/llm-network-error-retry/index.md`；该 400 属请求非法，仍不重试。）
