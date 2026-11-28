# 设计方案：LLM API 在差网络下的稳定性（建连重试 + SSE 保活）

## 1. 背景与现象

会话 `sess-cf62b88a1b24-19652-a55e364c-0000`（Windows + 手机热点限速 3 Mbps + VPN）在 5 小时里
出现 9 次模型节点硬失败，每次都是"1 次原始 + `llm.max_retry` 5 次"全部失败，单轮白等 127~143 秒，
随后由用户手动输入"继续"。展示历史里的错误只有三类：

| 次数 | 错误文本（结尾的 ` &#124; stream: …` 是 provider 的诊断后缀） | 单次尝试耗时 |
|---|---|---|
| 7 | `timeout: 由于线程退出或应用程序请求，已中止 I/O 操作。 [system:995]` | 16.00~16.09 s |
| 1 | `不知道这样的主机。 [system:11001]` | 11.1 s |
| 1 | `SSE read-header timeout after 60000 ms` | 61.2 s |

算一遍就对上了：6 次尝试 × 16 s + 退避（1+2+4+8+16，`llmRetryDelaySeconds` 的网络类基数 1 s）= **127 s**，
会话里最短的一次正好是 127.1 s。

同一台机器、同一网络、分析时现场实测（`opencode.ai:443`）：

| 探测 | 结果 |
|---|---|
| 15 次纯 TCP 连接 | 8 成功（中位 203 ms，最大 2265 ms），7 失败（每次都是 21.03 s，`WSAETIMEDOUT` 10060），成功率 53.3% |
| 8 次完整 HTTPS（DNS→TCP→TLS→POST→首字节） | 2 次死在 TCP（21 s），6 次成功（TCP 0.27~1.31 s，TLS ~0.2 s，首字节 0.48~0.83 s） |
| 15 次 DNS 解析 | 15 成功（0~79 ms，命中 Windows 解析缓存） |

结论：瓶颈是**建连阶段的丢包**，不是带宽。成功时首字节只有 0.5 秒，37K token 的请求体约 150 KB，
3 Mbps 下也就 1~2 秒；失败时系统要等满 21 秒的 SYN 重传窗口才报错。

## 2. 现状调查

### 2.1 建连（`cxx_utilxx/src/http_client.cpp` 的 `createConnection`）

```cpp
auto connectDeadline = steady_clock::now() + config.connectTimeout;   // DNS+TCP+TLS 共用
auto endpoints = co_await HttpClient::asyncResolveWithDeadline(...);  // 后台 getaddrinfo + 轮询
...
co_await asio::async_connect(lowest_layer, endpoints,
                             asio::cancel_after(remainingMs(), asio::use_awaitable));
co_await stream.async_handshake(asio::ssl::stream_base::client,
                                asio::cancel_after(remainingMs(), asio::use_awaitable));
```

三处问题：

1. `cancel_after` 到点把**整次建连**取消，抛 `boost::system::system_error`（`operation_aborted`，Windows 995），
   `agentxx/util/exception.h::classifyCurrentException` 在没有取消令牌时把它写成 `timeout: <系统文案>`，
   于是日志里只剩"已中止 I/O 操作"，看不出阶段。
2. `asio::async_connect` 的取消是整体取消，**不会**在失败后切到下一个 endpoint。`opencode.ai` 解析出
   `104.18.11.66` / `104.18.10.66` 两个 A 记录，第二个从未被尝试。
3. 预算是一个整体：DNS 慢一次（会话里的 11.1 s）就吃掉大半，TCP/TLS 只剩几秒。

### 2.2 `connect_timeout` 默认值

`agent/lib/include/agentxx/agent/config.h:100` `int connectTimeoutSeconds = 16;` —— 比 Windows
自己的 SYN 超时（21.0 s）还短，所以"丢一个 SYN"必然变成"我们主动放弃"，且一次预算内放不下第二次拨号。
样例 yaml 的注释写的是 30 秒，与代码默认值不一致（`config-keys.md` 生成物里记的是 16）。

### 2.3 DNS（`waitDnsResolve` / `asyncResolveWithDeadline`）

每次建连都重新 `getaddrinfo`，没有任何缓存：解析器抖动一次（`11001`，本机实测 11 秒）就废掉一次尝试。
解析失败时也没有"上次成功的地址"兜底。

### 2.4 SSE 收尾（`requestSseAsync` → `doSseExchange`）

`onChunk` 在解析到 `data: [DONE]` 时返回 true，随后：

```cpp
auto stopReading = [&]() {
    if (parser.is_done()) { reusable = parser.get().keep_alive(); }
    else { stream.lowest_layer().close(ignore); }   // ← 连接直接关掉
};
```

`parser.is_done()` 只有在读完 **HTTP 正文**后才为真。SSE 是 `Transfer-Encoding: chunked` 的长正文，
`[DONE]` 只是应用层约定，正文还要靠 0 长度块收尾。停在 `[DONE]` 时正文没读完 → 连接必须关 →
下一次模型调用又要走 2.1 的建连（最容易失败的一步）。

`kPoolIdleTimeout = 120 s` 也偏长：常见网关 keep-alive 空闲超时在 60~75 s，复用到已死连接的概率偏高。

### 2.5 诊断

- `HttpClient::poolStats(url)` 存在，但没有任何地方输出它（`assembly_snapshot` /
  `buildDiagnosticsText` 都没有），无法确认"连接到底有没有复用"。
- 没有"每次拨号"的日志，只有失败后 provider 拼的一行诊断后缀。

## 3. 方案

### 3.1 建连：把预算调回 30 秒，错误带阶段

**不做**自己的逐地址拨号循环（用户确认）：`asio::async_connect` 本来就会在第一个地址失败后
接着试下一个，只需要给它足够的总预算。

现在的问题不是“没有重拨”，而是**预算（16 s）比系统自己的 SYN 超时（21 s）还短**：
第一个地址还在等重传，我们的计时器先把整次建连取消，于是第二个地址连一次机会都没有。

| 改动 | 内容 |
|---|---|
| 默认预算 | `ModelConfig::connectTimeoutSeconds` 16 → 30（见 3.5） |
| 错误带阶段 | `createConnection` 里给 connect / TLS 各包一层 catch，把 `stage` / `endpoint` / `elapsed` 拼进错误文本；超时机制仍是 `cancel_after` + 单一总预算，不改行为 |
| 拨号日志 | 建连成功 `XX_LOGT`（endpoint + 耗时），失败 `XX_LOGW` |
| DNS | 缓存 + 上次可用地址兜底（见 3.3），与拨号策略无关 |

效果（按第 1 节的实测）：成功时一次建连约 200 ms；失败必然等满 OS 的 21 s SYN 窗口。
预算给到 30 s 后，第一个地址吃满 21 s 失败时，`async_connect` 还能用剩下的约 9 秒试第二个
地址（SYN 在 3 s / 6 s 的重传窗口里通了就够用）。还失败的，是两个地址在同一时间窗里都不通，
这一类只能靠重试跨过去，不是建连策略能解决的。

**代价**：丢包时单次尝试从 16 s 变成长达 21~30 s，一轮 6 次失败的等待从约 127 s 变成约 211 s。
觉得太久时可以按模型把 `connect_timeout` 调到 24 秒左右（仍覆盖一次完整的 SYN 窗口）。

`RequestConfig` 只新增一个字段（放在末尾，保持聚合初始化可用）：

| 字段 | 默认 | 含义 |
|---|---|---|
| `sseFinishGrace` | 1 s | SSE 收尾宽限，见 3.2；0 = 旧行为（立即关连接） |

DNS 缓存的有效期不做成每请求参数（`asyncResolveWithDeadline` 没有 `RequestConfig`，`tcp.cpp` /
`ws_client.cpp` 也调它），用进程级常量 60 s + `HttpClient::setDnsCacheTtl()`（测试用，0 = 不缓存）。

`createConnection` 的改动（诊断为主，不改建连机制）：

```cpp
try {
    co_await asio::async_connect(lowest_layer, endpoints,
                                 asio::cancel_after(remainingMs(), asio::use_awaitable));
} catch (const utilxx_base::AsioSystemError& e) {
    throw stageError("connect", key, elapsed, e.code(), e.what());   // 例：connect timeout after 30000 ms (stage=connect endpoint=...:443)
}
// TLS 握手同理（stage=tls）；剩余 budget 仍是同一个 connectDeadline
try {
    co_await stream.async_handshake(asio::ssl::stream_base::client,
                                    asio::cancel_after(remainingMs(), asio::use_awaitable));
} catch (const utilxx_base::AsioSystemError& e) {
    throw stageError("tls", key, elapsed, e.code(), e.what());
}
```

要点：

- `stageError` 把 `code == operation_aborted`（Windows 995）写成 `… timeout after N ms`，
  其余错误保留原文；这样日志里能分清“我们等超时了”与“对端/系统报错”，也不会再只有
  `timeout: 已中止 I/O 操作` 这一句。
- 文本里保留 `connect` / `timeout` / `handshake` 关键词，`classifyLlmError` 仍落到
  `LlmErrorKind::Network`（最短退避），重试策略不变。
- 每次建连只在结尾记一条日志，不引入额外拨号与额外流量。

### 3.2 SSE 收尾：把连接还给池

先说清楚两个"结束"：

- `data: [DONE]`：**OpenAI Chat Completions / Responses 的应用层约定**（一条普通 SSE data 事件）。
  SSE 协议本身没有结束事件，事件流是靠 **HTTP 正文结束**收尾的（浏览器 `EventSource` 也是靠连接关闭）。
- 0 长度块（`0\r\n\r\n`，可能带 trailer）：**HTTP/1.1 chunked 正文的结束标记**，即"这条连接回到消息边界"。

连接要复用就必须把上一条响应读完整，否则残留字节会与下一条响应混在一起、解析错位，所以
"收到 `[DONE]` 但没读到终止块"的连接只能关掉，不能复用。

实现（`doSseExchange`，**不阻塞调用方**）：

```cpp
// onChunk 返回 true（应用层认为流结束）后
if (parser.is_done()) {
    reusable = parser.get().keep_alive();          // 终止块与 [DONE] 一起到了，直接回池
} else if (config.sseFinishGrace.count() > 0) {
    // 连接、解析器、缓冲区整体交给后台收尾协程，本请求立即返回
    deferStreamTail(std::move(conn), std::move(parser), std::move(buf), poolKey,
                    config.sseFinishGrace);
    conn = nullptr;                               // 归还/关闭都由收尾协程负责
}
// 其余情况（用户取消 / 看门狗到点 / 读超时）：直接关连接，不进收尾
```

- `deferStreamTail` 用 `asio::co_spawn(executor, …, asio::detached)`（与本文件 `startDnsResolve`
  同一套做法），协程里用已有的 `runSseOpWithTimeout` 循环读到 `parser.is_done()` 或超过宽限：
  读到就 `pool.release(key, conn, keep_alive)`，否则 `pool.release(…, false)`。
- 收尾期间的字节只丢弃，不再交给 `onChunk`（应用层已收工；`usage` 一类事件总在 `[DONE]` 之前）。
- 归还恰好一次：协程帧里放一个 RAII guard，正常收尾时置位；未置位就由析构调用
  `release(…, false)`；池已销毁（`g_poolAlive == false`，进程或上下文退出）时跳过。
- 收尾期间连接仍占一个并发名额（最长 1 s）：LLM 端点默认 5 个名额，感知不到；
  将来真给某端点配 1 个名额时，再把“收尾中”拆成单独记账。
- **provider 侧不需要任何改动**：它现在就是在 `[DONE]` / `message_stop` / `response.completed`
  时让 `onChunk` 返回 true，HTTP 层据此判定“应用层已收工”。
- 为什么不让 provider 调 `delayClose()`：`onChunk` 返回 true 已经是“我不再要数据”的唯一信号，
  再加一个显式调用会多一套“谁在什么时候调、忘了调怎么办”的约定；把决定权留在 HTTP 层，
  开关就是 `sseFinishGrace`（0 = 旧行为，立即关连接）。

同时 `kPoolIdleTimeout` 120 s → 60 s：宁可新建连接，也不撞上已经被网关关掉的空闲连接。

不做"复用前 peek/select 探活"：Windows 上区分"没数据"与"对端已 FIN"要 `select + FIONREAD` 组合，
收益不如缩短空闲窗口 + 现有的一次失效重试（`isPoolRetryableError`）。

### 3.3 DNS 缓存与兜底

- 进程级缓存：`host:port → {endpoints, 解析时刻}`，`std::mutex` 保护，TTL 60 s（进程常量）。
- 解析失败但缓存里有旧结果（无论是否过期）→ 用旧结果并 `XX_LOGW` 记一条，不废掉这次请求。
- 解析失败且无缓存 → 抛带阶段的错误：`dns resolve failed after 11108 ms: ...`。
- 缓存放在 `asyncResolveWithDeadline` 里：`createConnection`、`tcp.cpp`、`ws_client.cpp` 都走它，一处改动全部受益。
- 新增 `HttpClient::clearDnsCache()` / `setDnsCacheTtl()` 供测试用；`clearConnectionPool()` 不动 DNS 缓存（两者无关）。

### 3.4 阶段化错误与拨号日志

阶段名固定为 `dns` / `connect` / `tls` / `write-request` / `read-header` / `read-body` / `finish`，
错误文本形如：

```text
connect timeout after 30000 ms (stage=connect endpoint=104.18.11.66:443)
tls handshake failed after 320 ms (stage=tls endpoint=104.18.10.66:443): <asio 原文>
dns resolve failed after 11108 ms (stage=dns host=opencode.ai:443): 不知道这样的主机。 [system:11001]
SSE read-header timeout after 60000 ms                       ← 已有实现（runSseOpWithTimeout）
```

日志：建连失败 `XX_LOGW("[http] connect failed stage=... endpoint=... elapsed=...ms err=...")`，
成功 `XX_LOGT("[http] connected endpoint=... tls=... elapsed=...ms")`。

文本里保留 `connect` / `timeout` / `handshake` 这些关键词，`classifyLlmError` 仍落到
`LlmErrorKind::Network`（最短退避），不改变重试策略。

### 3.5 agentxx 侧

- `ModelConfig::connectTimeoutSeconds` 默认 16 → **30**（用户确认）。
- 重新生成 `docs/zh-cn/design/config-keys.md` 与 `agent/schema/config-keys.json`
  （`AGENTXX_UPDATE_CONFIG_KEYS=1` 跑 `config_keys` 模块）。
- `buildDiagnosticsText`（`agent/lib/src/util/diagnostics.cpp`）增加 `## http pool` 段：
  遍历 `AgentConfig` 里出现过的 `baseUrl`，输出 `HttpClient::poolStats(url)` 的
  active/idle/created/reused/peak/queued，用于确认"连接真的复用了"。

## 4. 边界与非目标

- 不改 wire 协议、插件 API 版本、FFI。
- 不新增 yaml 键；`sseFinishGrace` 先做成 `RequestConfig` 默认值，DNS 缓存只给两个进程级入口，都不上配置面（需要时再加）。
- 不做（用户确认）：自写逐地址拨号循环与单次拨号短超时、生成期间预热备用连接、
  `network_retry_seconds`、网卡连通性监听；另不做：备用模型自动切换、HTTP/2、复用前探活。
- 不改 `llm_error.cpp` 的重试策略（任何错误都重试，只由 `llm.max_retry` 限次）。
- `llm.max_retry` 目前没有 yaml 键（只有结构体默认 5），本次不动，只在文档里记一笔。
- `agent/third_party/cxx_utilxx` 允许修改（用户确认），但只改上面点到的文件与函数。

## 5. 数据兼容

- DNS 缓存、连接池都是进程内状态，不落盘，无迁移。
- 默认值变化只是"更宽松的建连预算"，不影响历史会话与协议。

## 6. 分步实施

1. cxx_utilxx：`RequestConfig` 末尾追加 `sseFinishGrace`（默认 1 s）。
2. cxx_utilxx：DNS 缓存 + 上次可用兜底，落在 `asyncResolveWithDeadline` 内部（新增 `clearDnsCache` / `setDnsCacheTtl`）。
3. cxx_utilxx：`createConnection` 的错误加阶段（connect / tls）、endpoint 与耗时，加一条建连日志；建连机制不变。
4. cxx_utilxx：`doSseExchange` 收尾后台化（`deferStreamTail`：读完终止块再回池）；`kPoolIdleTimeout` 120 → 60。
5. agentxx：`connectTimeoutSeconds` 16 → 30；重新生成配置键文档与 schema。
6. agentxx：`buildDiagnosticsText` 增加 `## http pool`。
7. 测试：`http`（SSE 收尾复用与零延迟、DNS 缓存与兜底）、`network_timeout`（总预算仍受
   `connectTimeout` 约束）、`llm_error`（阶段化文本仍判为 Network）。
8. 构建 + 跑模块 + 真机观察。

## 7. 验证

- `agentxx_test http network_timeout llm_error config_keys --log-level warn`
- `http` 的 SSE 收尾用例要断言两件事：① 服务端延迟 300 ms 才发终止块时，`requestSseAsync`
  的返回时间不受影响（不包含那 300 ms）；② 紧接着的下一次请求 `poolStats().reused` 增长。
- `agentxx_test`（全量；连接池与超时的用户面很广，必须回归）
- 手工：连续多轮工具调用后看 `--dump-diagnostics` 的 `## http pool`，`reused` 应随请求数增长
- 真机弱网：确认建连失败的文本带 `stage=connect` / `stage=tls`，不再只有 `timeout: 已中止 I/O 操作`

## 8. 风险

- `createConnection` 是 LLM / MCP / `web_fetch` / 插件网络 / 更新检查共用的路径，改错影响全部网络功能；
  本次不改建连机制（不增加拨号次数），只改默认预算与错误文本。
- 默认预算 16 → 30 s 后，丢包时单次尝试最坏从 16 s 变成 30 s（一轮 6 次约 127 s → 约 211 s）；
  这是“让 OS 跑完一次完整 SYN 窗口并给第二个地址留时间”的必要代价，可按模型下调 `connect_timeout`。
- SSE 收尾是后台协程：`pool.release` 必须恰好一次（RAII guard 兜底）；io_context 停止时协程不跑，
  连接随协程帧析构关闭，计数由 guard 归还。收尾期间的连接仍占一个并发名额（最长 1 s）。
- 连接池空闲窗口缩短后，"间隔超过 60 秒"的请求会重新建连 —— 这是有意的取舍。
