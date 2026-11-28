# 实施记录：LLM API 在差网络下的稳定性

## 当前进度

- [x] 现象定位：从会话库 `view_message` 里统计失败样本与耗时
- [x] 现场网络探测：TCP 建连成功率、完整 HTTPS 请求各阶段耗时、DNS 成功率
- [x] 根因确认（16 秒默认建连预算 + 单一地址 + SSE 连接不回池 + 错误无阶段）
- [x] 方案与决策记录（`plan.md`，用户已确认取舍）
- [x] cxx_utilxx：`RequestConfig.sseFinishGrace`、DNS 缓存与兜底、建连阶段错误、SSE 后台收尾、池空闲窗口 60 秒
- [x] agentxx：`connectTimeoutSeconds` 默认 30、配置键文档与 schema 重生成、`--dump-diagnostics` 加 `## http pool`
- [x] 测试：`http`（SSE 收尾复用/零延迟、DNS 缓存）、`llm_error`（阶段化文本）、`config_keys`（默认值）
- [x] 构建与模块回归（`http` / `network_timeout` / `llm_error` / `config_keys` / provider 三模块 / `plugin_executor` / `boundaries` 等）
- [x] 全量回归（`agentxx_test --log-level warn`）：44253 条通过，仅 `input_delivery` 3 条时序抖动
- [ ] 真机弱网复测（需要用户在实际网络下观察 `## http pool` 的 `reused` 与失败文本）

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

### 第 1 步：cxx_utilxx 的 HTTP 连接层（子模块提交 `6bdbc4c`）

改动文件：`include/utilxx/http_client.h`、`src/http_client.cpp`。

| 改动 | 说明 |
|---|---|
| `RequestConfig::sseFinishGrace`（默认 1 秒） | 放在结构体末尾，现有聚合初始化不受影响；0 = 旧行为 |
| DNS 缓存（60 秒）+ 上次可用地址兜底 | 缓存放进 `asyncResolveWithDeadline`，`createConnection` / `tcp.cpp` / `ws_client.cpp` 都受益；解析失败或超时用旧结果并记 WARN；新增 `dnsCacheStats` / `clearDnsCache` / `setDnsCacheTtl` |
| 建连错误带阶段 | `runConnectStage` 包住 connect / TLS 握手：`connect timeout after 30000 ms (stage=connect host=opencode.ai:443 endpoints=2) | <原文>`；超时机制不变（仍是 `cancel_after` + 单一总预算） |
| 建连日志 | 成功 `XX_LOGD("[http] connected …")`，失败 `XX_LOGW("[http] connect/tls …")` |
| SSE 收尾后台化 | `SseTailDrain` + `drainSseTail`：应用层结束回调返回 true 后，把连接/解析器/读缓冲交给 `co_spawn(detached)`，最多读 `sseFinishGrace`，读到 chunked 终止块就 `pool.release(..., true)`，否则关闭；RAII 析构保证 `release` 恰好一次 |
| 解析器用 `shared_ptr` 持有 | beast 的 `parser` 不可移动（编译期实测 `error C2280`），所有权只能共享 |
| 池空闲窗口 120 → 60 秒 | 落在常见网关 keep-alive 空闲超时（60~75 秒）之前 |

验证：`cmake --build ... --target agentxx_lib_repo` 通过；`http` 329→389 条断言全通过；
`network_timeout` 49 条全通过（总预算仍受 `connectTimeout` 约束）。

### 第 2 步：agentxx 侧（主仓提交 `9f54b90a`）

| 改动 | 文件 |
|---|---|
| `connectTimeoutSeconds` 默认 16 → 30，补注释说明"总预算"与"要覆盖系统 SYN 超时" | `agent/lib/include/agentxx/agent/config.h` |
| `--dump-diagnostics` 增加 `## http pool`（每个端点的 active/idle/created/reused/peak/queued + DNS 缓存命中） | `agent/lib/src/util/diagnostics.cpp` |
| 配置键目录与 schema 重生成（默认值 16 → 30） | `docs/zh-cn/design/config-keys.md`、`agent/schema/config-keys.json`、`agent/test/core/test_config_keys.cpp` |
| SSE 收尾用例（零延迟 / 复用 / 关闭缓存三态） | `agent/test/core/test_http.cpp` |
| 阶段化错误文本仍判网络类 | `agent/test/core/test_llm_error.cpp` |

验证：`agentxx_test http network_timeout llm_error config_keys` 529 条全通过；
`config_keys` 更新模式下重生成通过（220 条）。

### 第 3 步：回归

`agentxx_test http network_timeout llm_error config_keys config_validation assembly_snapshot
openai_provider anthropic_provider fake_provider provider_call_scope event_hub async_tools usage_ledger`
→ 2244 条断言全通过。

`boundaries` 首次失败是**既有问题**（`docs/zh-cn/design/benchmark.md` 里两处停用词 `口径`），
顺手改成 `定义` / `规则` 后通过（20 条）。

### 第 4 步：全量回归

命令：`agentxx_test.exe --log-level warn`（Debug、ASan 构建，构建目录 `agent/build/windows-debug`）。

| 结果 | 说明 |
|---|---|
| `Total: passed=44253 failed=3` | 3 条全在 `input_delivery`（line 1186/1189/1208） |
| `input_delivery` 单独运行 | 140 条全通过（45540 ms） |

`input_delivery` 这三条断言校验“注入的输入是否落在观察到的最后一次请求上”，用例注释里已写明
全量跑时机器忙会晚一轮到达；与本次改动无关（单独跑稳定通过）。

本轮全量之前，上一轮全量还有 5 个模块失败，逐个查清后：

| 模块 | 断言数 | 结论 |
|---|---|---|
| `ffi_c_api` | 2 | 全量下时序抖动，单独跑 487 条全通过 |
| `config_loader` | 1 | 本次改的默认值（`connectTimeoutSeconds` 16→30）没同步断言，改后通过 |
| `tui_ui_items` | 上一轮全量失败 | 单独跑 277 条全通过（本轮全量也通过） |
| `tui_tool_header` | 4 | 既有 bug，修后 166 条全通过 |
| `client_plugins` | 5 | 同一个既有 bug，修后 925 条全通过 |

### 顺手修掉的既有 bug（提交 `47a5c013`）

`agent/lib/src/plugins/builtin_tool_renderers.cpp` 的 `agentxx_share_store` 工具行摘要：

| 问题 | 影响 |
|---|---|
| `id` 只按字符串读（`value<std::string>`） | 参数里的 `id` 是数字（`{"id":12}`）、`new` 结果里的 `id` 也是数字（`{"id":7}`），`Json::value` 类型不匹配时安静返回默认值 → id 空 |
| 判空用参数 id（`if (id.empty() == false)`） | `new` 的参数本来就没有 id，就算从结果里取到了 id 也不会显示 |
| 结果 | 工具行永远只有 `new` / `get` / `update`，看不出操作的是哪一条；`tui_tool_header`(4) 与 `client_plugins`(5) 因此长期失败 |

改法：新增 `idTextOf()`（字符串/整数/浮点都能取），`new` 完成后用结果 id，判空改成看
“实际要显示的那个 id”。

## 遗留与注意事项

- `llm.max_retry` 没有 yaml 键，用户无法自己调大重试次数；本次不改，需要时另开任务
  （加键要同步 `config_keys` 用例与生成文档）。
- 弱网下"一次尝试最坏 30 秒"是默认值下的代价：两个地址都不通时，一轮 6 次失败约 211 秒。
  需要更快放弃就按模型把 `connect_timeout` 调到 24 秒左右（仍覆盖一次完整的 SYN 窗口）。
- SSE 收尾期间连接仍占一个并发名额（最长 1 秒）；LLM 端点默认 5 个名额，感知不到。
- 连接池空闲窗口 60 秒意味着"间隔超过 1 分钟"的请求仍会重新建连；工具调用之间通常只隔几秒，
  这条链路上复用的收益最大。
- VPN 出口本身的稳定性不在应用层可控范围：若两个 A 记录长期都不可达，换节点/协议比改代码更有效。
