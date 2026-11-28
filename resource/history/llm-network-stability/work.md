# 实施记录：LLM API 在差网络下的稳定性

## 当前进度

- [x] 现象定位：从会话库 `view_message` 里统计失败样本与耗时
- [x] 现场网络探测：TCP 建连成功率、完整 HTTPS 请求各阶段耗时、DNS 成功率
- [x] 根因确认（16 秒默认建连预算 + 单一地址 + SSE 连接不回池 + 错误无阶段）
- [x] 方案与决策记录（`plan.md`，用户已确认四项取舍）
- [ ] 实施第 1~4 步（cxx_utilxx 连接层）
- [ ] 实施第 5~6 步（agentxx 默认值、诊断）
- [ ] 测试与验证

## 分析阶段做了什么

### 会话数据

数据源：`{data_dir}/sqlite/sessions/sess-cf62b88a1b24-19652-a55e364c-0000/session.db`
的 `view_message` 表（5399 条），筛 `role=tip` 且文本含 `failed after`。

- 9 条 `Node 'llm' failed after 1 attempt(s)`，是模型节点把重试用光的终态提示
  （`llm.max_retry = 5` → 每次终态 = 6 次 HTTP 尝试）；
- 错误只有三类：`[system:995]` ×7（单次 16.0 s）、`[system:11001]` ×1（11.1 s）、
  `SSE read-header timeout` ×1（61.2 s）；
- 整轮耗时 127.1 / 129.1 / 130.3 / 135.4 / 135.5 / 138.1 / 138.3 / 142.8 / 180.8 秒；
- 6 × 16 s + 退避 1+2+4+8+16 = 127 s，与最短的 127.1 s 对上，说明"每次尝试都死在 16 秒"。

### 现场探测（同一台机器、同一网络）

脚本：`%TEMP%/agentxx/sess-15e1e469b2fbc-34272-7a1d3f07-0000/net_stats.py`（纯 TCP）、
`net_probe.py`（DNS→TCP→TLS→POST→首字节）。不属于仓库内容，可随时删除。

| 探测 | 结果 |
|---|---|
| 15 次 TCP 连接 | 8 成功（中位 203 ms / 最大 2265 ms），7 失败（每次 21.03 s，10060），成功率 53.3% |
| 8 次完整 HTTPS | 2 次死在 TCP（21 s），6 次成功（TCP 0.27~1.31 s、TLS ~0.2 s、首字节 0.48~0.83 s） |
| 15 次 DNS | 15 成功（0~79 ms） |

结论：与"3 Mbps 限速"无关（成功时首字节 0.5 秒），是建连阶段丢 SYN；而且系统自己的 SYN 超时
（21 s）比我们配置的建连预算（16 s）更长，所以我们永远先放弃、也永远试不到第二个地址。

### 代码核对

| 事实 | 位置 |
|---|---|
| `connectTimeoutSeconds` 默认 16 | `agent/lib/include/agentxx/agent/config.h:100` |
| 建连用 `cancel_after(remainingMs())`，DNS/TCP/TLS 共用一个预算 | `cxx_utilxx/src/http_client.cpp` `createConnection` |
| `async_connect` 整体取消，失败不切下一个 endpoint | 同上 |
| DNS 每次重新解析，无缓存 | `asyncResolveWithDeadline` / `waitDnsResolve` |
| `[DONE]` 后未读完 chunked 终止块，连接直接 close | `requestSseAsync` 的 `doSseExchange` / `stopReading` |
| `kPoolIdleTimeout = 120 s` | `http_client.cpp` 常量区 |
| `operation_aborted` 无取消令牌时写成 `timeout: <系统文案>` | `agent/lib/include/agentxx/util/exception.h` |
| `llm.max_retry` 只来自结构体默认值，yaml 无此键 | `config.h:400`，`config_loader.cpp` 无解析 |

## 实施记录

（尚未开始；每完成一步在这里补"改了哪些文件 + 怎么验证"。）

## 遗留与注意事项

- `llm.max_retry` 没有 yaml 键，用户无法自己调大重试次数；本次不改，需要时另开任务
  （加键要同步 `config_keys` 用例与生成文档）。
- 会话库里的重试提示（"LLM API 请求失败 …, N 秒后自动重试"）是瞬态 UI 提示，不落展示历史，
  所以事后只能看到终态失败；本次加的拨号日志可以补上这一段（走 `enableLogCapture` 的环形缓冲）。
- VPN 出口本身的稳定性不在应用层可控范围：若同一地址两个 A 记录都长期不可达，
  换 VPN 节点/协议比改代码更有效。
