# 方案

## 1. 背景

两个现象来自同一台机器上的同一份 release 构建：

```text
[W] Plugin `agentxx_system_monitor`: callback `broadcast plugin.agentxx_system_monitor.usage`
    blocked the plugin thread for N ms (budget 100 ms)
```
（偶现，不固定时间）

```text
卡在 llm 节点等了很久，没有判定为超时也没有重试；日志里没有警告也没有错误；
手动取消可以正常结束。
```

## 2. 现状调查

### 2.1 插件超预算告警的来源

| 位置 | 作用 |
| --- | --- |
| `agent/third_party/cxx_pluginxx/include/pluginxx/runtime/instance_base.h` 的 `runPluginDispatch` | 唯一测量点：`steady_clock` 包住 `fn()`，计入 `callbacks/totalMicros/maxMicros`，`micros > budgetMs*1000` 时 `overBudget++` 并 `XX_LOGW` |
| `.../host/host_core.h` 的 `subscribeBroadcast` | 广播订阅回调经 `dispatchToPlugin(inst, "broadcast " + id, ...)` 投到订阅方插件的执行线程（告警里的 label） |
| `.../runtime/runtime.h` 的 `PluginCallbackStats` | 统计容器（budget 默认 100 ms） |

被点名的回调（`agent/plugins/agentxx_system_monitor/agentxx_system_monitor.cpp`）：

- 发布方：agent 侧实例每 5 s 采一次 `CpuGpuUsage` 并 `publishPluginBroadcastNoWait("plugin.agentxx_system_monitor.usage", ...)`
  （采样本体在阻塞池里跑，插件线程只做发布）。
- 订阅方：client 侧实例的 `usage` 回调 → `parseUsage` + `buildUsageInfoItemsJson` +
  `clientUpdateInfoSection`（`clientTableCall`，提交即返回）。

两侧都是微秒级工作；能凑到 100 ms 的只有两种情况：线程被系统调度挂起，或卡在主程序的锁上。
`startTableOp` 路径上只有 `OpCore::createDetached` 的 `operationsMutex` / `detachedHandlesMutex`，
两把锁都只做一次 `map::emplace` / 一次 `erase_if`，不存在被长期持有的写者。

结论：**测量的是墙钟时间，单次超预算无法区分"插件干了 100 ms"与"线程被挂起 100 ms"**；
而它恰好是整棵树里唯一 5 秒周期的广播回调，所以统计上只有它会碰到抖动。

### 2.2 LLM 节点超时链路

| 位置 | 现有超时 |
| --- | --- |
| `nodes/modelcall.cpp` `baseRun` | 无超时；只按异常重试（`llm.max_retry`），并在 `co_await` 之间做取消检查 |
| `nodes/modelcall.cpp` `callLLM` | `ProviderCallScope` 只做取消域，文件头已注明"不做超时竞速" |
| `protocol/openai_provider.cpp` `doStream` / `doStreamResponses`、`protocol/anthropic_provider.cpp` `doStream` | `connectTimeout` / `sendTimeout` / **`readChunkTimeout`（块间）** + `readChunkTimeoutPerRead`（只在 `finishSeen` 后收紧到收尾宽限） |
| `utilxx/http_client.cpp` `runSseOpWithTimeout` | 每次读一个 `steady_timer`，到期 `close()` 底层连接 → 读以 `operation_aborted` 结束 → 抛 `SSE <stage> timeout ...` |

关键缺口：`StreamProgress::noteChunk` 在**收到任意字节**时刷新 `lastDataAt`，而块间计时器也是
按"这次读有没有返回"重置。上游只要持续发保活（`: ping` 注释行、Anthropic 的
`{"type":"ping"}`、`data: {"choices":[{"delta":{}}]}` 空增量），流就永远不会触发块间超时：
既没有异常（不重试）、也没有日志，节点一直挂在 `co_await provider->invoke_format_data(...)`。
`finishSeen` 之后的阶段同样如此（只差 `[DONE]` 时若有心跳，一直等下去）。

## 3. 方案

### 3.1 插件超预算告警：限频，不放弃计数

`PluginCallbackStats` 增加按 `(插件名, 回调名)` 的记录（窗口 5 分钟）：

- 每次超预算仍 `overBudget++`（测试与诊断依赖计数）；
- 窗口内首次只 `XX_LOGD`（说明单次多为调度抖动），**不 WARN**；
- 之后每 5 分钟最多一条 `XX_LOGW`，带窗口内次数与最长耗时；
- 记录表设上限（512），超限先清过期项；
- 加载快照 `plugin_executor` 段追加 `over_budget_top`（次数/最长耗时），让"到底哪个回调慢"可查。

### 3.2 LLM 流：无有效输出看门狗（语义空闲上限）

- `provider_common.h`：
  - `StreamProgress` 增加 `lastOutputAt` / `noteOutput()` / `idleSinceOutput()`；
  - 新增 `StreamOutputWatchdog`：`limit`（无输出上限，0 = 关闭）、`logInterval`、`model`、
    `nextReadTimeout(progress, base)`、`tripped(progress)`。`nextReadTimeout` 返回
    `min(base, limit - idleSinceOutput())`，到期返回 1 ms 让下一次读立即到期；同时负责
    每 60 s 一条"仍在等待"的 WARN。
- 三个流式解析处把 **有效输出** 记到进度上（正文/思考/工具参数分片），心跳不计：
  - `OpenAIProvider::processSseLine`：`emitThinking`、content 分片、`tool_calls` 的名称/参数分片；
  - `OpenAIProvider::processResponsesSseBuffer`：`emitContent`/`emitThinking`、`function_call_arguments.delta`；
  - `AnthropicProvider::processSseBlock`：`text_delta` / `thinking_delta` / `input_json_delta`。
- 三个 provider 的 `readChunkTimeoutPerRead` 统一走看门狗：
  `base = finishSeen ? 收尾宽限 : readChunkTimeout`；失败分支里若 `tripped()`，
  把错误文本改写成明确的 `LLM stream idle timeout: no output for N ms (limit M ms)` 并记 WARN。
- 配置：`model.list[].output_idle_timeout`（int 秒，默认 600，0 = 关闭）。

不做：单次尝试总时长硬顶（会误砍合法的长生成，等用户需要时再加）。

## 4. 边界与兼容

- 插件告警策略改变只影响日志级别与频率，`overBudget` 计数、`maxMicros`、预算开关语义不变
  （`test_plugin_executor` 的既有断言仍成立）。
- 看门狗默认开启会改变行为：任何 10 分钟没有有效输出的流会被中断并重试。
  合法场景（长思考后一次性出 token）默认 600 s 足够；需要更宽松或关闭用配置键。
- `StreamOutputWatchdog` 只在流式路径使用；非流式请求维持原块间超时。

## 5. 分步实施

1. `cxx_pluginxx`：`PluginCallbackStats` 加记录表 + 限频告警；`assembly_snapshot` 加 `over_budget_top`；
   `test_plugin_executor` 补断言。
2. `provider_common.h`：`StreamProgress` 新字段 + `StreamOutputWatchdog`。
3. 三个 provider 接线（读超时取样 + 有效输出打点 + 错误文案）。
4. `ModelConfig::outputIdleTimeoutSeconds` + `config_loader.cpp` + `config_keys` 测试条目 +
   重新生成 `agent/schema/config-keys.json` 与 `docs/zh-cn/design/config-keys.md`。
5. 编译（windows-debug）+ 跑相关测试模块。
6. 更新 `docs/zh-cn/design/index.md`（LLM 超时链路）与 `configuration.md`（新配置键）。

## 6. 验证

- `agentxx_test plugin_executor`（预算统计、派发、生命周期）
- `agentxx_test openai_provider anthropic_provider llm_error config_keys`
- 新增单测：`StreamOutputWatchdog` 的折算与 `tripped` 语义；`processSseLine`/`processSseBlock`
  对"心跳不刷新有效输出"的判定。

## 7. 风险

- 看门狗把读超时压到 `min(base, 剩余)`：若 `limit` 配得比 `readChunkTimeout` 小，
  块间保护会随之变短 —— 这是预期（上限就是上限），文档里写明。
- 限频后单次卡顿只留 DEBUG：诊断靠 `over_budget_top` 快照与计数补上。
