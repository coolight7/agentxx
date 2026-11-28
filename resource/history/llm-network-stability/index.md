# LLM API 在差网络下的稳定性（建连重试 + SSE 保活）

## 目标

提高 LLM 请求在弱网（手机热点限速 + VPN、丢包严重）下的成功率：

1. 建连阶段不再"丢一个 SYN 就整次请求作废"，并且同一请求内会把解析出的多个地址都试一遍；
2. LLM 的 SSE 请求结束后连接能回到连接池复用，下一次请求不必重新建连（建连是最容易失败的一步）；
3. 失败时错误文本带阶段（DNS / TCP / TLS / 写请求 / 读响应头 / 读响应体），日志里能直接看出卡在哪一步。

## 范围

- `agent/third_party/cxx_utilxx`：`include/utilxx/http_client.h`、`src/http_client.cpp`
  （连接建立、DNS 解析、SSE 收尾、连接池）
- `agent/lib`：`agent/config.h`（`connect_timeout` 默认值）、`util/diagnostics.cpp`（连接池统计进诊断包）
- `agent/test`：`http`、`network_timeout`、`llm_error` 三个模块
- 文档：`docs/zh-cn/design/config-keys.md`（生成物，改默认值后需重新生成）、
  `docs/zh-cn/design/index.md`（弱网行为说明）

## 状态

- 分析：已完成（会话实测 + 现场网络探测，结论见下）
- 实施：未开始
- 验证：未开始

## 关键决策（用户确认）

| 议题 | 决定 |
|---|---|
| `connect_timeout` 默认值 | 改成 **30 秒**（现在是 16 秒，比 Windows 自己的 SYN 超时 21 秒还短） |
| 自写逐地址拨号循环 / 单次拨号短超时 | 不做（继续用 `asio::async_connect` 的地址列表 + 单一总预算） |
| 生成期间预热备用连接 | 不做 |
| `network_retry_seconds`（按墙钟给网络错误更长的重试预算） | 不做 |
| 修改 `agent/third_party/cxx_utilxx` | 允许 |
| 监听系统网卡/连通性变化 | 不做 |
| SSE 收尾（等终止块） | 由 HTTP 层在**后台协程**里读完再回池（宽限 1 秒），请求立即返回；provider 不调用任何新接口 |

## 结论（事后来看这一页就够）

现象：会话 `sess-cf62b88a1b24-19652-a55e364c-0000` 里 9 次 `Node 'llm' failed after 1 attempt(s)`，
每次都是 6 次尝试（1 次原始 + `llm.max_retry` 5 次）全失败，单轮白等 127~143 秒，然后用户手动打"继续"。

根因按影响排序：

1. **`connect_timeout` 默认 16 秒 < 系统 SYN 超时 21 秒**：丢一个 SYN 时，`asio::async_connect`
   还在等系统重传，我们自己的 16 秒计时器先到，`cancel_after` 把整次建连取消 →
   `operation_aborted`（Windows `WSA_OPERATION_ABORTED` = 995）→ 报成 `timeout: …已中止 I/O 操作`。
   会话里 7 次失败的单次耗时都是 16.00~16.09 秒，与此完全吻合。
2. **预算被 16 秒切断时，下一个地址也就没机会了**：`asio::async_connect` 本身会依次尝试解析出的
   每个地址（`opencode.ai` 有 104.18.11.66 / 104.18.10.66 两个 A 记录），但取消是整体取消，
   第一个地址还在等重传时预算就没了，第二个等于从未被尝试。
3. **DNS + TCP + TLS 共用一个 16 秒预算**：会话里有一次 DNS 花了 11.1 秒（`11001`），
   即使解析成功，留给 TCP + TLS 的也只有几秒。
4. **SSE 请求结束后连接不回池**：收到 `[DONE]` 就停止读取，HTTP 正文的终止块（chunked 的 0 长度块）
   还没读完，连接只能关掉。于是每次模型调用都要重新建连 —— 正好每次都要过第 1、2 条那道坎。
5. **错误文本丢阶段信息**：`operation_aborted` 被记成 `timeout: <本地化系统文案>`，
   看不出是 DNS、TCP、TLS 还是读超时；也没有"每次尝试一行"的日志。

现场实测（同一台机器、同一网络，`opencode.ai:443`）：

| 探测 | 结果 |
|---|---|
| 15 次纯 TCP 连接 | 8 成功（中位 203 ms，最大 2265 ms），7 失败（每次都是 21.0 s，`10060`），成功率 53.3% |
| 8 次完整 HTTPS（DNS→TCP→TLS→POST→首字节） | 2 次死在 TCP（21 s），6 次成功（TCP 0.27~1.31 s，TLS ~0.2 s，首字节 0.5~0.8 s） |
| 15 次 DNS 解析 | 15 成功（0~79 ms，命中 Windows 解析缓存） |

即：瓶颈是**建连阶段的丢包**，不是 3 Mbps 的带宽（成功时首字节只要 0.5 秒，37K token 的请求体约 150 KB）。

改法与预期：把默认预算调回 30 秒（让 OS 跑完一次完整的 SYN 窗口，并把剩余时间留给第二个地址）、
给 DNS 加缓存与上次可用地址兜底、SSE 结束后把连接交回池，具体见 `plan.md`，实施与验证见 `work.md`。
