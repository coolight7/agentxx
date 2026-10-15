# 网络/传输层错误单列一类并一律可重试 (只区分等待时长)
- 难度: B
- 类型: 优化
- 基于commit: d31b21a29503d86721e0cb7aa84e7b1d5a0277e0
- 时间: 2026-10-15 23:26
- 需求:
```md
请将所有网络错误都归入可重试错误，仅区分重试等待的时长
```

## 背景

`classifyLlmError` 原来把错误分成 未知/鉴权/额度/请求非法/上下文超限/限流/超时/服务端 八类，
其中只有 鉴权/额度/请求非法 不重试。网络与传输层故障没有自己的类别，散落在两个位置:

- 一部分文本命中"超时"类 (关键词里有 `connection reset` / `connection closed` / `broken pipe` /
  `eof` / `network`)，等待基数 2 秒；
- 更多文本谁都不命中，落到"未知"类 (也可重试，等待基数 2 秒)，用户在 UI 上只能看到
  "unknown error"，看不出是网络问题。

统计本机历史日志里 121 条 `failed after N attempt(s)` 提示: 未知 48 / 请求非法 41 /
超时 15 / 限流 6 / 鉴权 5 / 服务端 5 / 额度 1。其中 `stream truncated [asio.ssl.stream:1]`
一条就出现 35 次 (现在归"未知")，另有 `远程主机强迫关闭了一个现有的连接。`、
`由于目标计算机积极拒绝，无法连接。`、`不知道这样的主机。`、
`由于线程退出或应用程序请求，已中止 I/O 操作。` 等中文系统文案同样落到"未知"。

另外存在一类隐患: 网络故障文本里常夹带地址与端口数字。例如
`https://api.example.com:10403 connect failed` 含 `403` 子串，会被"鉴权"类抢走判成
"重试也没用"——正是"网络故障却直接停止"的成因。

## 修改

- `agent/lib/include/agentxx/nodes/llm_error.h`
  - `LlmErrorKind` 增加 `Network` (网络/传输层故障)，位置在 `Timeout` 与 `Server` 之间
  - 类注释补"可重试规则: 只有鉴权/额度/请求非法不重试，其余一律重试，类别只影响等待时长"
  - `classifyLlmError` / `isLlmErrorRetryable` / `llmRetryDelaySeconds` 注释同步
    (判定顺序、各基数)
- `agent/lib/src/nodes/llm_error.cpp`
  - 新增网络类判定块，位置在"上下文超限"之后、"鉴权"之前 (先判网络，避免端口数字
    被状态码类抢走)；关键词覆盖:
    - 连接层: `connection reset/closed/refused/aborted/terminated/failed/error/timed out/timeout`、
      `cannot connect`、`could not connect`、`failed to connect`、`unable to connect`、
      `connect failed/error/timeout`、`broken pipe`、`stream truncated`、`stream closed`、
      `end of file`、`eof`、`asio.ssl`、`asio.netdb`、`network`、`unreachable`、
      `no route to host`、`transport error`、`i/o error`
    - 域名解析: `name resolution`、`no such host`、`host not found`、`could not resolve`、
      `resolve host`、`getaddrinfo`、`nodename nor servname`
    - TLS 握手: `handshake`、`ssl error`、`tls error`
    - 中文系统文案: `网络`、`无法连接`、`无法解析`、`连接被拒绝/被重置/重置/被关闭/中断/超时/失败/异常`、
      `连接尝试失败`、`基础连接已经关闭`、`意外关闭`、`远程主机强迫关闭`、`这样的主机`、
      `积极拒绝`、`没有反应`、`已中止 i/o`、`握手失败`
  - "超时"类不再含 `connection reset/closed`、`broken pipe`、`eof`、`network` (已归网络类)
  - 类别文本: 新增 `Network` → "network or transport error"；`Timeout` 由
    "timeout or connection problem" 改为 "timeout" (连接问题已归网络类)
  - 等待基数改为按类别取值: 网络 1 / 超时·未知·上下文超限 2 / 服务端 3 / 限流 5 秒
    (不重试的三类不会走到这里，与"未知"同基数只为避免出现 0 秒)
    其余不变: 指数增长、上限 60 秒、按错误文本加 0~2 秒抖动、`retry-after` 优先
- `agent/lib/src/nodes/modelcall.cpp`: 重试分支注释更新为"其余一律自动重试，类别差别只是等待时长"
- `agent/test/core/test_llm_error.cpp`
  - 新增网络类用例 (含真实文本: `stream truncated [asio.ssl.stream:1]`、
    `SSE stream truncated: missing [DONE] marker`、`asio.netdb:2 Host not found`、
    `asio.misc:2 End of file`、`write: broken pipe`、`SSL handshake failed` 与四条中文系统文案)
  - 增补"端口含 403 的 connect failed 必须判网络而不是鉴权"
  - `connection reset by peer` 由超时类用例移到网络类；超时类补 `SSE read-header timeout`
  - 退避用例改为断言基数次序 (网络 ≤ 超时 = 未知 ≤ 服务端 ≤ 限流，用同一 errInfo 抹平抖动)
    与 `Network` 也认 `retry-after`
- `docs/zh-cn/design/roadmap.md`: LLM-2 行补一句"网络类一律可重试，只影响等待基数"

## 验证

- 构建: `agent/script/windows_debug_build.bat` (Debug) → 无 error
- `agentxx_test.exe llm_error` → 全部通过
- 相关模块回归 + 全量 `agentxx_test.exe` 见本次会话记录 (仅剩既有的 `ftxui_text` ASan 用例失败)

## 备注

- 判定顺序上，网络类排在鉴权/额度/请求非法/限流之前: 前者的收益是防止端口/地址数字
  把网络故障误判成不可重试；代价是同时含网络关键词与 `429` 的文本会按网络类等待
  (更短，且不再优先采信 `retry-after`)。实测文本里没有这种混合形态。
- 未改 `Unknown` 的语义: 仍按可重试处理 (与历史行为一致)，避免出现"没认出来的错误直接停"。
