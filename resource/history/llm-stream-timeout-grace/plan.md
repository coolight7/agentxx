# 长工具调用被判为读取超时 — 处理方案

## 背景

排查「较长的 tool 生成（例如 `write` 写大文件）容易判为超时」时确认的事实（会话库实测）：

- LLM 请求路径上没有总时长上限，只有 `connect_timeout`、`read_chunk_timeout`（块间）与
  发送期限；`read_chunk_timeout` 默认 60 秒，每次 `async_read_some` 成功都会重新计时，
  SSE 注释心跳（`: ping`）与 tool 参数增量都算数据，都会被计时器认可。
- 实测失败集中在**上游在生成长工具参数期间一个字节都不发**：会话库里
  `SSE read-body timeout after 60000 ms` 出现 12 次，位置都在「准备写大文件」的下一轮
  （aborted 的 assistant 消息正文为空，说明这 60 秒没收到任何 body 数据）。
- 另外 `SSE write-request timeout after 30000 ms` 出现 8 次：发送期限公式
  `max(30s, body/64KiB 秒)` 要求上行 ≥ 64 KiB/s，慢链路/网关暂不读 body 时必然误判。

由此确定三处改动（P4「按静默阶段拆分阈值」本轮不做）：

- **P1 收尾判定**：已经见到 `finish_reason`（Anthropic 是 `message_delta.stop_reason`）后，
  等待正式结束标记（`[DONE]` / `message_stop`）改用**短上限**
  `max(10s, read_chunk_timeout/5)`，且不超过 `read_chunk_timeout` 本身；宽限到了或连接
  在结束标记前断掉，都可以按「已完整」收尾，不再丢掉一份可执行的 tool_call。
- **P2 诊断信息**：流超时/传输失败的错误文本里附上收包进度（已收字节、事件数、距上次
  数据时长、是否已见结束原因、已积累工具调用数与参数长度），使下次能一眼分清
  「上游一直没发」与「发到一半停了」。
- **P3 发送期限**：改为 `max(60s, ceil(body/16KiB) 秒)`（要求上行 ≥ 16 KiB/s）。

## 现状调查（文件 / 函数一级）

### 超时来源（都在工具库侧）

- `agent/third_party/cxx_utilxx/include/utilxx/http_client.h`
  - `RequestConfig`：`connectTimeout` / `sendTimeout` / `readChunkTimeout`
    （注释已写明 `readChunkTimeout` 是「块间」而不是总时长）
- `agent/third_party/cxx_utilxx/src/http_client.cpp`
  - `runSseOpWithTimeout()`（约 76 行）：每次 SSE 阶段操作临时建一个 `steady_timer`，
    超时则 `close` 底层连接并抛 `SSE <stage> timeout after N ms`
  - `requestSseAsync()`（约 1332 行）：`write-request` / `read-header` / `read-body`
    三个阶段；`read-body` 循环里每次读都取 `config.readChunkTimeout`
  - `calcTimeoutBySize()`（约 1154 行）：`max(30s, ceil(body/64KiB) 秒)`，为发送期限兜底
  - `exchange()`（http_client.h 内的模板，约 161 行）：非流式请求，`sendTimeout` 同一来源
- `agent/lib/include/agentxx/agent/config.h`：`connectTimeoutSeconds = 16`、
  `readChunkTimeoutSeconds = 60`（`ModelConfig`）

### 协议层（要改的地方）

- `agent/lib/src/protocol/openai_provider.cpp`
  - `doStream()`（约 1327 行）：Chat Completions 流式；`on_chunk` 里 `processSseBuffer`
    返回 true（收到 `[DONE]`）即通知 HTTP 层停止；异常回调里只按 `doneReceived` 决定
    是否可以放过传输层错误；末尾 `if (!doneReceived) throw "missing [DONE]"`
  - `processSseBuffer()` / `processSseLine()`（约 1597 / 1643 行）：逐行解析，
    `finish_reason` 在里面映射成 `completion.stop_reason`（原文丢失）
  - `doStreamResponses()`（约 1470 行）：Responses API 流式，结束标记是
    `response.completed` / `response.incomplete`；没有 `finish_reason` 这种中间信号
  - `completeAsync()` / `completeAsyncResponses()`：非流式，`sendTimeout` 走默认公式
  - 4 处 `HttpClient::RequestConfig{...}`（约 1042 / 1167 / 1357 / 1492 行）
- `agent/lib/include/agentxx/protocol/anthropic_provider.h`
  - `processSseBuffer()` / `processSseBlock()`（约 130 / 199 行，**实现内联在头文件里**）：
    `message_delta` 只取 usage，**完全不看** `delta.stop_reason`；结束标记是 `message_stop`
- `agent/lib/src/protocol/anthropic_provider.cpp`
  - `completeAsync()`（约 500 行）与 `doStream()`（约 575 行），2 处 `RequestConfig{...}`
- `agent/lib/include/agentxx/protocol/provider_common.h`：两个 provider 共用的纯逻辑，
  新加的公式与诊断组装都放这里（便于单测）

### 既有测试

- `agent/test/core/test_http.cpp`：有 `HttpServer` 与原始 socket 服务器样例
  （如 `OneShotKeepAliveServer`），`test_http_client_sse_interruption` 覆盖 SSE 中断
- `agent/test/core/test_openai_provider.cpp`：`MockOpenAIServer`（把整段 SSE 一次写出后
  关闭连接），已有 `test_streaming_missing_done_throws`
- `agent/test/core/test_anthropic_provider.cpp`：同名 mock
- 测试模块名：`http` / `openai_provider` / `anthropic_provider`

## 方案

### P1 结束原因之后的短超时（chat completions / anthropic）

新增纯函数（`provider_common.h`）：

```cpp
/// 已收到结束原因后, 等正式结束标记的块间上限
/// - 取 max(10s, readChunkTimeout/5): 常规间隔越宽, 收尾允许等得越久
/// - 以 readChunkTimeout 为上界: 配置比 10 秒更短时, 收尾不能比正常间隔还长
inline std::chrono::milliseconds streamFinishGraceTimeout(std::chrono::milliseconds readChunkTimeout);
// 60s -> 12s; 100s -> 20s; 10s -> 10s; 3s -> 3s; 200s -> 40s
```

HTTP 层新增一个「每次读之前问一次本轮超时」的钩子（`RequestConfig` 末尾追加字段，
不影响已有调用方）：

```cpp
/// 每次读响应体之前取样一次本轮的块间超时; 空 / 返回 nullopt 时用 readChunkTimeout
std::function<std::optional<std::chrono::milliseconds>()> readChunkTimeoutPerRead;
```

`requestSseAsync` 的 `read-body` 循环每次读之前调用它。这样**不需要**改连接关闭时机、
不需要额外取消信号，也不影响已有的 `[DONE]` 快速停止路径。

provider 侧：

- `processSseLine` / `processSseBlock` 在解析到非空 `finish_reason`（Anthropic 为
  `message_delta.delta.stop_reason`）时，把「已见结束原因 + 原文」写进新的进度结构
- `RequestConfig.readChunkTimeoutPerRead = [&] { return progress.finishSeen
      ? std::optional{streamFinishGraceTimeout(readChunkTimeout)}
      : std::nullopt; };`
- 收尾判定改为：
  - `doneReceived` 为真：保持现状（连接层错误放过）
  - **`finishSeen` 为真**：结束标记前超时/断连 → 记一条 warning（含结束原因与进度），
    按「已完整」返回，不再抛 `missing [DONE]` / `missing message_stop`
  - 两者都假：保持现状（抛截断/超时，交给 modelcall 重试）
- Responses API 没有「生成结束」的中间事件，本轮 P1 不动它（其结束标记就是
  `response.completed`），只享受 P2 / P3

### P2 超时诊断

`provider_common.h` 新增：

```cpp
/// 流式收包进度 (超时/传输失败时附到错误信息末尾)
struct StreamProgress {
    std::chrono::steady_clock::time_point startedAt, lastDataAt;
    size_t      bytes  = 0;    ///< 已收字节数
    size_t      events = 0;    ///< 已解析 SSE 事件数
    bool        finishSeen = false;
    std::string finishReason;  ///< finish_reason / stop_reason 原文
    void noteChunk(size_t n);  ///< onChunk 里调用 (累计字节 + 刷新 lastDataAt)
};

/// 组装单行后缀, 例:
/// " | stream: recv=20480B events=133 idle=60003ms finish=tool_calls tools=1 args=2048B name=agentxx_filesystem_write"
inline std::string streamFailureSuffix(const StreamProgress&, const std::map<int, neograph::ToolCall>&);
```

- 事件计数由 `processSseBuffer` / `processResponsesSseBuffer` / `processSseBlock` 的可选
  出参（`StreamProgress*`，默认 nullptr）累加，不改已有调用语义
- 附加位置：两个 provider 的流式异常处理里，**只对传输/超时类错误**附加
  （`API error (HTTP ...)` 这类上游业务错误保持原文，避免把上游的 error body 弄乱）
- 后缀全 ASCII，不影响 `classifyLlmError` 的关键词判类（`timeout` 仍在原文本里）

### P3 发送期限

```cpp
/// 发送请求体的期限: max(60s, ceil(body/16KiB) 秒) —— 要求上行 >= 16 KiB/s
inline std::chrono::milliseconds llmSendTimeout(size_t bodyBytes);
```

两个 provider 的 6 处 `RequestConfig{...}` 都显式传 `.sendTimeout = llmSendTimeout(...)`；
流式路径要在 `std::move(bodyStr)` **之前**算好（函数实参求值顺序不确定，移动后 size 会变 0）。
`HttpClient::calcTimeoutBySize()` 本身不动（其他调用方如 web_fetch 保持原语义）。

## 边界与不做的事

- **P4 不做**：按静默阶段（等首字节 / 思考 / 正文 / 工具参数）拆分不同阈值，等 P1~P3
  上线观察后再评估
- Responses API 的「`response.output_item.done` 之后静默」不特殊处理（要改动参数拼接
  与完成判定，风险大于收益）
- Anthropic 的 `completion.stop_reason` 仍不会被流式路径填写（历史上一直是 `unknown`，
  与本次问题无关）；本次只在进度结构里记录原文，供诊断使用
- 不新增配置键（避免 `config-keys.md` 与 `config_keys` 归属校验的连锁改动）：
  宽限与发送期限都由现有 `read_chunk_timeout` / body 大小推导

## 兼容性 / 影响面

- `RequestConfig` 末尾追加 `std::function` 字段：源码兼容（聚合初始化不受影响），
  **二进制不兼容**。本仓库内三个工具库都是随主程序源码构建，插件静态链接
  自己的那份，因此无需额外处理；改完要删掉对应 build 目录（见实施步骤）
- 错误文本变长（附加诊断后缀）：`classifyLlmError` 用子串匹配，前缀语义不变；
  UI 提示会多一段进度信息，属预期
- 行为变化：`finish_reason` 后不再 60 秒空等，也不再把「已完整但没有结束标记」当失败
  （会话上下文里不会再插入 `[Exception aborted]` 并整段重发）

## 分步实施

1. `resource/history/llm-stream-timeout-grace/` 建 `index.md` / `plan.md` / `work.md`
2. 工具库：`RequestConfig.readChunkTimeoutPerRead` + `read-body` 循环取样；删除
   `agent/build/windows-debug` 下 cxx_utilxx 相关构建目录以强制重编
3. `provider_common.h`：`StreamProgress`、`streamFinishGraceTimeout`、`llmSendTimeout`、
   `streamFailureSuffix`
4. `openai_provider`：统计出参、宽限钩子、`finishSeen` 收尾、诊断后缀、4 处
   `sendTimeout`
5. `anthropic_provider`（含内联解析器）：同上，`stop_reason` 记录
6. 测试：
   - `http`：`readChunkTimeoutPerRead` 生效（首读正常超时、翻转为短超时后报短时限）
   - `openai_provider`：`finish_reason` 后无 `[DONE]` 且连接关闭 → 成功；
     `finish_reason` 后连接保持但静默 → 宽限到点成功（`read_chunk_timeout: 2`，约 2 秒）；
     无 `finish_reason` 且静默 → 报错且带诊断后缀；两个纯函数取值
   - `anthropic_provider`：`message_delta.stop_reason` 后无 `message_stop` 且连接关闭 → 成功
7. 文档：`docs/zh-cn/design/index.md` 的模型配置段补「收尾宽限 + 发送期限」一行说明
8. 全量回归：`agentxx_test` 的 `http` / `openai_provider` / `anthropic_provider` /
   `llm_error` / `modelcall`（若存在该模块名则以 test.cpp 为准）

## 验证

- 构建 `agent/script/windows_debug_build.bat`（Debug）无 error
- `agentxx_test --log-level warn http openai_provider anthropic_provider`
- 重点断言：
  - `finish_reason` + 断连：返回工具调用，`stop_reason` 正确，无异常
  - 静默超时：错误文本同时含 `timeout` 与 `recv=` / `events=` / `idle=`
  - 宽限取值表：60s→12s、100s→20s、10s→10s、3s→3s、200s→40s
  - 发送期限：0B→60s、1MiB→64s、16MiB→1024s

## 风险与回退

- 风险 1：宽限太短导致「结束标记晚于 12 秒」的网关被判成完整（丢 usage）。
  缓解：`doneReceived` 优先；宽限只影响「已见到结束原因」之后，丢掉的最多是 usage 统计，
  正文与工具参数不会丢；日志里会留下 warning
- 风险 2：`readChunkTimeoutPerRead` 里访问 provider 栈上的 `StreamProgress` 需保证生命
  周期覆盖整次请求 —— 进度对象与 `co_await` 同栈帧，lambda 只在请求内部同步调用，
  满足要求（与现有 `on_chunk` 捕获方式一致）
- 回退：三个改动互相独立，任一处可单独还原；宽限钩子为空即退回原行为
