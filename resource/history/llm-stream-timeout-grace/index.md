# 长工具调用被判为读取超时（收尾宽限 + 诊断 + 发送期限）
- 难度: B
- 类型: bug修复 + 行为调整
- 时间: 2026-11-09
- 需求:
```md
请按以下决定更新设计方案：P1 同意，但见到 finish_reason 后短超时限制为 max(10, {超时间隔}/5),
也就是他们之间的较大值; P2 同意; P3 调整发送超时判断为 max(60s, body/16KiB 秒);
P4 暂不考虑，等P1 P2 P3实施完成后再尝试思考对策。
```

## 现象与定位

- 现象：生成较长的工具调用（典型是 `write` 写大文件）时容易报
  `Node 'llm' failed ... SSE read-body timeout after 60000 ms`，整段生成被丢弃后重试。
- 定位结论：**上游在生成长工具参数期间一个字节都不发**（agentxx 侧任何字节都会重置块间
  计时器，`tool_calls.function.arguments` 增量与 SSE 注释心跳都算数据；provider 也是增量
  拼接参数，没有「攒完再处理」）。会话库实测：12 次 `read-body timeout` 全部落在
  「准备写大文件」的下一轮，aborted 的 assistant 消息正文为空（60 秒零字节）。
- agentxx 侧的放大问题：① 结束标记缺失时把「已生成完」当失败并把完整 tool_call 扔掉；
  ② 超时错误没有收包进度，无法区分「上游一直没发」与「发到一半停了」；
  ③ 发送期限 `max(30s, body/64KiB 秒)` 要求上行 ≥ 64 KiB/s，实测误判 8 次。

## 结论（本次改动）

- P1：见到 `finish_reason`（Anthropic 为 `message_delta.stop_reason`）后，等
  `[DONE]` / `message_stop` 的块间上限收到 `max(10s, read_chunk_timeout/5)`
  （以 `read_chunk_timeout` 为上界）；宽限到点或结束标记前断连都按「已完整」收尾。
- P2：流超时/传输失败的错误文本附收包进度（字节数、事件数、静默时长、结束原因、
  工具调用数与参数长度）。
- P3：发送期限改为 `max(60s, ceil(body/16KiB) 秒)`。
- P4（按静默阶段拆阈值）不做，等本轮上线后评估。

## 范围

- 工具库 `cxx_utilxx`：`RequestConfig` 新增 `readChunkTimeoutPerRead`，`read-body`
  循环每次读之前取样（不新增超时总开关、不改连接关闭时机）
- `agent/lib`：`provider_common.h` 新增三个纯函数/结构；openai（chat + responses）与
  anthropic 两个 provider 接入宽限、收尾判定、诊断后缀、发送期限
- 测试：`http` / `openai_provider` / `anthropic_provider`
- 文档：`docs/zh-cn/design/index.md` 模型配置段一行说明

## 偏差与遗留

- 宽限公式在原式 `max(10s, t/5)` 之上加了「不超过 `t`」的上界：`read_chunk_timeout`
  配得比 10 秒还短时，「短超时」不能反而比正常间隔更长（默认 60s 时仍是 12s）。
- Responses API 没有「生成结束」的中间事件，P1 不适用（只享受 P2/P3）。
- Anthropic 流式路径仍不写 `completion.stop_reason`（历史遗留，与本次问题无关），
  本次只在诊断里记录 `stop_reason` 原文。

详见 [plan.md](plan.md)，实施记录见 [work.md](work.md)。
