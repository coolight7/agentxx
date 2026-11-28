# 实施记录

## 进度

- [x] 现状调查 (场景代码 + 四个上限机制 + 轮次时间地板标定)
- [x] plan.md 设计
- [x] 改 `bench_resource_real.cpp` (采样点/驱动方式/会话库字节/客户端重同步/输出)
- [x] 改场景说明文字 (`bench_resource.h`, `benchmark_main.cpp`)
- [x] release 编译 `agentxx_benchmark`
- [x] 标定: 20M/100M 两档 + 8M 小规模探针 + 20M 三条对照 (关会话库/关插件)
- [x] 完整 5 档实测 (两次运行: `bench_20261128_061417`, `bench_20261128_062419`)
- [x] 内存根源分析 + 报告 ([report.md](report.md))
- [x] `benchmark.md` 第 20 节 + `AGENTS.md` 场景描述更新

## 实施日志

### 1. 现状调查

- 场景 5 `benchResourceTuiLongCtx` (`agent/benchmark/bench_resource_real.cpp`): 真实
  `AgentHost` + 5 插件 + `Channel` + `SessionServerAgentIO` + 真实 FTXUI + 会话库, 逐轮流式对话,
  采样点 100K/200K/1M/2M token。
- 四个"只保留一部分"的机制: 服务端展示尾窗 400 条 (按条数)、客户端 100 条 (按条数)、
  LLM 上下文 75% 压缩 (按 token)、单条输入/工具输出转存 share store (只作用于 LLM 上下文)。
- 时间地板标定 (现有 release 产物, 活上下文 ~20K token, 1 个 SSE 帧, 无帧间停顿):
  **140~148 ms/轮**, 与上下文大小几乎无关。

### 2. 第一版实现: 采样点改字节 + 一轮注入 32 条 (已放弃)

20M/100M 实测 (modelMax=300000, 48 帧): 2156 ms/批 = 67 ms/条消息 (2 GB 要 4.9 小时);
查会话库发现**展示行被合并**: 用户行 264 条 (每条 8 KB, 正常), 另有 52 条 `role=event` 行,
每条 **57 KB**, `source=event:pending-rollover` —— 没赶上本轮 modelcall 边界的 next-step
输入会被 `rolloverLeftoverNextStepInputs` 合并成一条, 400 条窗口内存因此从预期 3.2 MB 涨到
12~15 MB。

### 3. 第二版实现 (最终): `history` 模式

真实轮次累积 `AGENTXX_TUI_CTX_REAL_BYTES` (默认 2 MB), 其余按采样点差额注入展示历史
(`Session::appendViewMessage`, 每 1024 条 flush 一次), 注入后让客户端重新尾窗同步
(`WireHello{historyTail=100}`)。

冒烟 (8M/100M, modelMax=30000): 内存内容恒定 2.8 MB, 会话库 13.6 → 111.7 MB,
RSS 40.59 → 41.36 (+0.77 MB) —— 方向正确。

### 4. 完整 5 档实测

两次运行 (参数相同, 各约 580 s):

| 累计内容 | 会话库 | 内存内容 | RSS (A/B) | 峰值 (A/B) |
|---|---|---|---|---|
| 20 MB | 27.9 MB | 3.61 MB | 47.09 / 48.12 | 55.22 / 57.00 |
| 100 MB | 113.0 MB | 3.62 MB | 49.00 / 50.77 | 55.22 / 57.44 |
| 200 MB | 220.1 MB | 3.63 MB | 48.22 / 49.59 | 55.22 / 57.44 |
| 1 GB | 1.07 GB | 3.64 MB | 49.91 / 52.75 | 55.22 / 57.61 |
| 2 GB | 2.14 GB | 3.65 MB | 54.84 / 53.05 | 55.55 / 57.75 |

对照 (20 MB): 关会话库 RSS 65.27 (+18.98, 内存里 21.9 MB 正文); 关 5 插件 RSS 41.27
(−5.02)。

### 5. 遇到的问题与处置

| 问题 | 处置 |
|---|---|
| 首次 `cmake --build --target agentxx_benchmark` 报"项目文件不存在" | 顶层目标是 `agentxx_benchmark_repo`; 之后直接编内层工程 `agentxx_benchmark_repo-build` 做增量 (约 2 分钟) |
| 外层构建一次跑了 45 分钟 (重新编译 lib/client) | 内层构建目录生成后, 后续只编内层 `agentxx_benchmark.vcxproj`, 编完手工 `Copy-Item` 到 `exec/` |
| `--next-step` 批量注入变成合并的 event 行 | 见第 2 节, 改成"真实轮次 + 历史注入" |
| 客户端窗口在注入后仍是注入前那批 | 注入后发一次 `WireHello{lastSeq=0, afterViewSeq=0, historyTail=100}` 触发尾窗同步, 客户端从此持有 100 条 / 0.81 MB |
| Windows 侧 `PSS/私脏/匿名/模块分解` 全为 0 (`n/a`) | smaps 只在 Linux 有效; 本报告的拆解改用"逻辑内存 + 分阶段 RSS + 对照实验" |
| 2 GB 档 RSS 两次运行差 5.8 MB | 如实记录两次结果; 归因为活上下文位置、sqlite 页缓存/WAL 与批量写入留下的空闲页 |

### 6. 验证

- 编译: release `agentxx_benchmark` 通过, 本次改动没有新增告警 (见 `bench_resource_real.cpp`
  行 `sendProbe`/`seedHistory` 附近);
- 数据自洽: 内存内容 = 展示窗口 + LLM 上下文 + 客户端窗口, 与逻辑内存行逐项对得上;
  会话库 2.14 GB / 累计内容 2.00 GB = 1.07 倍;
- 行为: 5 档都跑通, 每档注入后的一轮真实对话无超时 (930~1090 ms/轮);
- 对照: 关会话库/关插件的差值符合预期 (窗口线性放大 / 插件底座固定)。
