# 真实 TUI 长上下文: 5 档 GB 级累计内容的内存实测 — 设计方案

## 1. 目标

把 `resource_tui_longctx` 场景的采样点从 4 档 (100K/200K/1M/2M **token**) 改成 5 档
**20M / 100M / 200M / 1G / 2G 累计内容字节**, 实测"会话越聊越长时内存是否还涨", 并给出
内存占用根源分析。

预期 (由用户给出): 服务端与客户端都对消息做了转存, 内存里只留一部分, 所以累计内容涨上去后
内存应当基本不涨, 且应当稳定在 50 MB 以内。

## 2. 现状调查

### 2.1 场景本体

`agent/benchmark/bench_resource_real.cpp` 的 `benchResourceTuiLongCtx()` (场景 5):

| 项 | 现值 |
|---|---|
| 装配 | `AgentHost` + 5 插件 + `Channel` 传输 + `SessionServerAgentIO(tail=100)` + 真实 FTXUI TUI + 会话库开 |
| 每轮输入 | `AGENTXX_TUI_CTX_MSG_BYTES` = 8192 字节 (≈2052 token) |
| 每轮回复 | `AGENTXX_TUI_CTX_REPLY_BYTES` = 600 字节, 切 48 个 SSE 帧, 帧间 1 ms |
| 模型上限 | `AGENTXX_TUI_CTX_MODEL_MAX` = 300000 token (75% = 225000 触发自动压缩) |
| 采样点 | `AGENTXX_TUI_CTX_POINTS` = `100000,200000,1000000,2000000` (**token**) |
| 驱动 | 主线程发 `WireUserInput` (默认投递 = next-turn), 轮询 `KeyMetrics` 轮次终态等本轮结束 |

采样点名字 `tokenPointName()`: 100000 → `ctx100k`, 2000000 → `ctx2m`。

### 2.2 已有的"只保留一部分"机制 (内存上限的四个来源)

| 机制 | 位置 | 上限 |
|---|---|---|
| 服务端展示历史尾窗 | `Session::viewWindowCap_` (`session.view_window_messages`, 默认 400) | **按条数**淘汰, 更早的从会话库分页读 |
| 客户端本地消息窗口 | `message_window.h` 的 `kLocalMessageLimit = 100` (+ 视口上方 3 条) | **按条数**淘汰, 上滚时按页拉回 |
| LLM 上下文自动压缩 | 达模型上限 75% 触发, 压回 `system + 请描述 + 摘要` | 按 token, 300K 模型约 225K |
| 单条输入 / 工具输出转存 | `util::offloadShareStoreIfOversized` (share store, 内存仅 3 条 LRU) | 只作用于 LLM 上下文, **展示历史仍是完整原文** |

结论: 前三个上限都是**按条数/token**的, 没有"按字节"的上限。所以内存 ≈ 窗口条数 × 单条消息
字节 + 活上下文 + 常驻底座 —— 与"累计内容"无关, 与"单条消息多大"成正比。这是本场景要验证的
核心结论, 也决定了下面第 3 节的参数选择。

### 2.3 可行性: 真实轮次的时间地板

用 2026-11-26 的 release 产物标定 (`AGENTXX_TUI_CTX_MODEL_MAX=30000`,
`AGENTXX_TUI_CTX_STREAM_CHUNKS=1`, `CHUNK_DELAY_MS=0`, 活上下文 ~20K token):

```
ctx8k  区间 4 轮 / 每轮均 140 ms
ctx16k 区间 4 轮 / 每轮均 148 ms
```

即"一条 8 KB 消息 = 一轮"的路径下, 每轮 ≈140 ms (几乎与本轮上下文大小无关, 是固定的
轮次机器开销 + 驱动侧轮询粒度)。按这个速率:

- 2 GB ÷ 8 KB = 262144 条消息 → 262144 × 140 ms ≈ **10 小时**, 不可行;
- 把单条消息放大到 256 KB → 8192 轮 (约 20 min), 但服务端尾窗 400×256 KB = 100 MB、
  客户端 100×256 KB = 25 MB, 内存会随单条消息线性涨到 120 MB 以上, 与"50 MB 以内"相悖。

所以两个都不能取。可行方案: **单条消息保持 8 KB (窗口内存不变), 一轮里注入多条消息**,
把"累计内容"与"轮次数"解耦。

### 2.4 批量注入的可行性 (代码路径)

`SessionServerAgentIO::handleUserInput` (`session_server_agent_io.cpp:720`):

- 默认投递 (`next-turn`): 进 `messageQueue_`, 空闲时立即开一轮;
- `delivery = "next-step"` 且 `turnActive_` 为真: 进 `Session::pendingInputs_`
  (`enqueuePendingInput`, 无条数上限), 在**下一次请求加载边界**由
  `nodes::drainPendingSessionInputs` 一次性取出 → 逐条写成 user 消息进 LLM 上下文 +
  展示历史 (落库、发 delta), 与本轮输入走完全相同的后续流程。

所以"一轮注入 N 条"是主程序已有的正式路径 (chatbot 的 next-step 投递就是这条), 不是基准侧
伪造。轮末 `rolloverLeftoverNextStepInputs` 把没赶上本轮的消息提升到下一轮, 不会丢。

## 3. 方案

### 3.1 采样点改为字节 + 新名字

- `AGENTXX_TUI_CTX_POINTS` 默认 `20M,100M,200M,1G,2G`, 支持 `K/M/G` 后缀 (1024 进制);
  纯数字仍按字节解释。
- 采样点名字改成不会与旧 token 口径混淆的写法: `ctx20mb` / `ctx100mb` / `ctx200mb` /
  `ctx1gb` / `ctx2gb` (旧批次的 `ctx2m` 是 2M **token**, 名字必须区分)。
- 累计量 = 用户消息字节 + 回复字节 (两者都是落进会话的内容)。

### 3.2 累计内容怎么到 GB 级: `history` 模式 (真实轮次 + 展示历史注入)

真实轮次有约 140 ms/轮的地板 (活上下文大时约 1 s/轮), 单条 8 KB 到 2 GB 要 26 万轮;
而把单条消息放大到 256 KB 会让 400 条窗口线性涨到 100 MB 以上 (窗口是按条数淘汰的)。
两条都不可行, 所以拆成两段 (环境变量 `AGENTXX_TUI_CTX_MODE`):

- `history` (默认): 先用真实轮次累积到 `AGENTXX_TUI_CTX_REAL_BYTES` (默认 2 MB);
  之后每个采样点把差额**注入展示历史** —— 直接调会话的写入入口
  `Session::appendViewMessage` (与真实轮次同一条内存/落库路径), 每 1024 条 `flushViewMessages()`
  一次 (否则待落盘队列积压, 窗口不淘汰, 内容会堆在内存里)。注入后让客户端重新尾窗同步
  一次 (`WireHello{historyTail=100}`), 使客户端持有"最近 100 条"。回答的是
  "会话历史 N GB 时常驻内存多少"。
- `turns`: 全部用真实轮次累积 (旧行为), 只能跑小型采样点, 用作对照。

为什么不靠"一轮注入多条 next-step 消息"提速 (已实测否决): 没赶上本轮 modelcall 边界的
next-step 输入会被 `rolloverLeftoverNextStepInputs` **合并成一条 event 输入**
(`source=event:pending-rollover`) 交给下一轮 —— 展示行变成 57 KB 的合并行, 400 条窗口
直接 12~15 MB, 而且吞吐只有 67 ms/条 (2156 ms/批), 仍然要 4.9 小时。

### 3.3 采样时一起量"转存"效果

每个采样点除既有指标 (RSS/私有/峰值/线程/fd/帧数/帧耗时/活上下文/压缩次数/窗口条数) 外,
再加:

- `__store.session_db`: 该会话的**会话库磁盘字节** (`collectStoreLogicalMem`, 已有实现);
- 内存里的内容 (展示窗口 + LLM 上下文 + 客户端消息) 汇总。

两者一比就能看出"累计内容全在磁盘、内存只留一个窗口"。

### 3.4 输出

- 每采样点一行 `printResourceResult` (既有), note 里补累计字节/会话库/内存内容;
- 末尾汇总表改成: `point | 累计内容 | 会话库(磁盘) | 内存内容 | 活上下文 | RSS | 私有 |
  峰值RSS | 线程 | 帧ms`;
- 分阶段内存表 (`MemPhaseTracker`) 照旧, 采样点标签用新名字。

## 4. 边界与取舍

- **不按字节做窗口**: 本次不引入"展示历史按字节淘汰"这类新机制, 只测量现状。若结果显示
  单条消息大小是内存主导项, 结论里说明"要支持超长消息需要按字节限额", 由用户决定是否改。
- **注入历史只走展示侧**: LLM 上下文仍只由真实轮次/压缩决定 (一个 GB 级历史会话的模型
  上下文本来也是压缩后的那一小段)。报告里必须把这个分工写清。
- **压缩次数比真实会话高**: 真实轮次阶段每 ~120 轮压缩一次 (每轮 8 KB 输入), 对"内存是否
  随累计内容增长"的判定没有影响 (压缩只会让活上下文更小)。
- **会话库会真的长到 GB 级**: 临时目录在系统 temp, 跑完删除; 实测 2 GB 内容 → 2.14 GB 库。
- **不做重启后冷加载**: 本次不含"2 GB 会话重新打开"的采样 (需要重建 agent/插件), 留作后续。

## 5. 分步实施

1. 本文件 (设计) + `index.md` / `work.md` 骨架;
2. 改 `bench_resource_real.cpp`: 采样点解析/命名、批量驱动、会话库字节采样、输出表格;
3. 改 `bench_resource.h` / `benchmark_main.cpp` 的场景说明文字;
4. release 编译 `agentxx_benchmark`;
5. 标定: 小规模跑 (20M/100M) 看 throughput 与内存, 据此定 `INPUTS_PER_TURN`;
6. 完整 5 档实测 (后台跑, 记录日志与 bench 报告);
7. 分析内存根源 (逻辑内存行 + 模块级 smaps + 会话库/内存内容对比), 写报告;
8. 更新 `docs/zh-cn/design/benchmark.md` (新增一节) 与 `AGENTS.md` 的场景描述。

## 6. 验证

- 编译: release `agentxx_benchmark` 构建通过 (无警告错误);
- 冒烟: `POINTS=20M` + `MODEL_MAX=30000` 跑通, 确认批量注入真的进同一轮 (`压缩次数`/
  `活上下文` 与 `轮次` 数量对得上, 无输入丢失: 总条数 = 发送条数);
- 数据自洽: 累计字节 ≈ 发送条数 × 8192 + 回复字节; 会话库字节 ≥ 累计字节量级;
- 内存: 5 档 RSS 不应随累计内容单调增长 (预期进入平台期);
- 回归: `AGENTXX_TUI_CTX_INPUTS_PER_TURN=1` 时行为与旧版一致 (单条一轮)。

## 7. 风险

| 风险 | 处置 |
|---|---|
| 批量注入期间 `waitRoundDone` 误判提前返回 | 等静默条件里加 `pendingInputs == 0`、`queueSize == 0`、`!turnActive` 与 10 ms 复查 |
| 2 GB 累计内容跑太久 | 标定 throughput 后调 `INPUTS_PER_TURN` (内容/轮), 必要时按档位提高 |
| 会话库过大占满磁盘 | 临时目录跑完删除; 运行前确认磁盘剩余 (本机 C: ~97 GB 可用) |
| 压缩子代理把用量算进本会话导致判据错位 | 沿用"轮次终态计数"判据 (旧版已踩过) |
