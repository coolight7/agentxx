# 内存优化方案: 降低每轮上下文拷贝 (请求体 JSON / SSE 解析 / 图状态消息数组)

- 难度: C (跨 lib + neograph, 需保持 checkpoint/协议语义不变)
- 类型: 性能与内存优化 (本文仅为设计方案, **未实施**)
- 时间: 2026-09-25 (2026-09-26 / 88fee6e0 按更新后的 neograph 重新实测基线并核对调用点)
- 状态: 设计完成, 基线已按新依赖重测 (旧数据作废), 待实施
- 核对: 2026-09-26 逐点复核代码与台账脚本 (neograph `1522761` / 主仓 `88fee6e0`):
  §2 行号全部一致 (补正见 §2 表后说明), §0.2 的两条结论有误并已更正 (§0.4),
  §6 待确认问题收敛为"未定位项 + 采样方法"
- 相关文档: [benchmark.md 第 10 / 11 节](../../../docs/zh-cn/design/benchmark.md) (mimalloc 开关对照与重测记录)
- 原始数据: [resource/benchmark/](../../benchmark/) (`2026-09-26_88fee6e0_windows-longctx/` 采样与分配统计、
  `2026-09-26_88fee6e0_linux-resource/` 基准报告、`harness/` 驱动脚本)

## 0. 背景与实测数据

### 0.1 依赖更新与本次重新实测

neograph 子模块已更新为上游重写后的 master + 重写后的 fork 补丁
(`1522761`, 主仓提交 `88fee6e0`, 2026-09-25)。重写后图状态的读写实现、
内置 reducer 与各调用点行号均有变化, 因此本文的基线数据整体重测了一遍,
**下面所有数字都来自 2026-09-26 的这次重测**, 早期版本的数字已作废。

测量环境与负载 (与 benchmark.md 第 11 节同一套台账):

- 构建: Windows release (VS18 / MSVC), 同一构建目录内只切换
  `AGENTXX_ENABLE_MIMALLOC` (ON + `MIMALLOC_LINK=SHARED`, 或 OFF 即系统分配器);
- 负载: `agentxx_cli cli` + 本地 OpenAI 兼容 mock 服务 (只回 SSE 且立即返回),
  配置无插件、单个 mock 模型; 经 stdin 逐条送入固定大小的用户消息 (每条一轮),
  全部轮次完成后空转 4 秒再取稳态值;
- 对照组: 上一发布版本 `agentxx-0.3.0-windows-x64` (2026-09-17, 系统分配器),
  用同一套负载驱动;
- 采样口径与复现步骤见 §4;
- 标注: 日期 `2026-09-26`, 主仓 commit `88fee6e0`, neograph `1522761`; 原始数据与
  驱动脚本见 `resource/benchmark/2026-09-26_88fee6e0_windows-longctx/` 与
  `resource/benchmark/harness/`。

结论先说: **重写后的每轮分配次数比旧版本高约 2~4 倍**
(50 × 8KB 组: `malloc req~` 1.3 GiB → **3.1 GiB**; 小上下文组每轮固定开销
5.8 MB → **26 MB**), 常驻内存也相应升高 (系统分配器口径 100K 上下文
专用工作集 7.95 MB → **8.2~8.5 MB**, 提交 10.36 MB → **21~24 MB**)。

### 0.2 每轮分配次数 (mimalloc `MIMALLOC_SHOW_STATS=1`, 退出统计)

| 组 | `malloc req~` (累计) | 每轮 | 对照: 旧基线 |
|---|---|---|---|
| 50 轮 × 8KB (≈100K 上下文, 请求体 ≈410 KB) | **3.1 GiB** | 62 MB | 1.3 GiB / 26 MB |
| 100 轮 × 8KB (≈200K 上下文, 请求体 ≈813 KB) | **9.9 GiB** | 99 MB | – |
| 5 轮 × 80KB (≈100K 上下文) | **364.8 MiB** | 73 MB | – |
| 50 轮 × 100B (小上下文, 请求体 ≈15 KB) | **1.3 GiB** | 26 MB | 290 MiB / 5.8 MB |

> 口径提示: 上表"每轮" = 累计 ÷ 轮数, 含进程启动成本 (单进程启动自身就是
> `malloc req~` 35.9 MiB / `huge` 14 块); 改用 N 轮与 N+1 轮相减的差分口径后
> 得到的每轮增量、以及分档归因的更正见 §0.4。

50 × 8KB 组的分配器自身明细: 活跃数据仅 632 KiB (`binned current`),
`binned` 累计 2.0 GiB + `huge` (≥512 KiB) 累计 1.1 GiB, 线程峰值 26,
用时 5.38 s (user 0.343 s / sys 0.562 s), 进程峰值 RSS 63.1 MiB /
峰值提交 224.9 MiB。

分 bin 归因 (50 × 8KB 组, 取累计量与块数较大的若干 bin):

| 块大小 (bin) | 累计量 | 块数 | 每轮块数 | 推测对应 |
|---|---|---|---|---|
| 10 KiB (`bin S 37`) | 419.1 MiB | 42.9K | ≈858 | **逐条消息正文拷贝** (8 KB 正文 + 1 字节结尾落到该档; 见 §0.4 更正 1) |
| 128.5 KiB (`bin L 52`) | 393.8 MiB | 3.1K | ≈62 | 整段上下文级缓冲 (请求体/状态) |
| 257.0 KiB (`bin L 56`) | 486.5 MiB | 1.9K | ≈38 | 整段上下文级缓冲 (请求体/状态) |
| 64.2 KiB (`bin M 48`) | 243.3 MiB | 3.8K | ≈76 | 中间缓冲 |
| 32.1 KiB (`bin M 44`) | 132.5 MiB | 4.2K | ≈84 | 中间缓冲 |
| 16.0 KiB (`bin M 40`) | 67.5 MiB | 4.3K | ≈86 | 中间缓冲 |
| 384 B (`bin S 18`) | 59.4 MiB | 162.3K | ≈3.2K | typed 层逐条解析的小对象 (与消息**条数**成正比, 见 §0.4) |
| 6 KiB (`bin S 34`) | 47.7 MiB | 8.1K | ≈162 | 单条消息级 |
| 128 B (`bin S 12`) | 19.8 MiB | 162.8K | ≈3.3K | 同上 (与消息条数成正比) |
| 32 B (`bin S 4`) | 7.2 MiB | 237.7K | ≈4.8K | 同上 (与消息条数成正比) |
| ≥512 KiB (`huge`) | 1.1 GiB | 570 | ≈11 (平均 ≈2 MB/块) | **与上下文大小无关** (每轮固定 ≈22 MB), 来源待定位; 不是状态序列化产物 (见 §0.4 更正 2) |

与旧基线相比原本写的两点变化, 经 2026-09-26 复核**两条都需更正**
(依据见 §0.4):

1. ~~**8 KiB 块不再是主要项**: 旧基线里 "每条消息文本被拷贝约 13 次/轮"
   (8 KiB bin 264 MiB / 33.8K 块) 的推理在重写后不成立 —— 现在
   `bin S 36` (8 KiB) 只有 2.8 MiB / 365 块~~
   → 更正: 逐条消息正文的拷贝**依然存在**, 只是落到相邻档位 (8 KB 正文 + 1 字节
   结尾 = 10 KiB 档 `bin S 37`, 每轮 858 块 ≈ 8.6 次/条消息);
2. ~~**占比最大的是整段状态序列化**: `huge` 块 1.1 GiB / 570 块 ...~~
   → 更正: `huge` (≥512 KiB) 与上下文大小**无关** —— 5 KB 上下文与 400 KB
   上下文的 `huge` 都是 1.1 GiB / 553~570 块 (每轮固定 ≈11 块 × ≈2 MB),
   它不是上下文序列化的产物; 真正的每步序列化点是
   `graph_engine.cpp:1505` (VALUES 事件, 宿主不消费) 与
   `graph_coordinator.cpp:258 / 290` (每 super-step checkpoint)。
   `graph_executor.cpp:222 / 772` 在 agentxx 路径上不执行 (§2 补正)。

### 0.3 常驻内存 (Windows release, 稳态值, MB)

系统分配器 (release 默认, `AGENTXX_ENABLE_MIMALLOC=OFF`):

| 组 | 专用工作集 | 工作集 | 提交 | 旧基线 (同口径) |
|---|---|---|---|---|
| 50 轮 × 8KB | 8.2~8.5 (首次运行 11.8) | 25.5~25.9 | 21.0~24.1 | 7.95 / 21.95 / 10.36 |
| 100 轮 × 8KB | 19.9 | 34.0 | 30.8 | 13.75 / 28.29 / 17.79 |
| 5 轮 × 80KB | 9.2 | 21.3 | 16.9 | 6.09 / 20.09 / – |
| 50 轮 × 100B | 4.6 | 19.3 | 14.9 | – |

mimalloc 打开时 (`AGENTXX_ENABLE_MIMALLOC=ON`, 同一构建目录):

| 组 | 专用工作集 | 工作集 | 提交 | 调参后提交 |
|---|---|---|---|---|
| 50 轮 × 8KB | 31.8~47.6 | 62.0~64.6 | 224.4~238.9 | 43.8 |
| 100 轮 × 8KB | 83.9 | 117.5 | 288.6 | – |
| 5 轮 × 80KB | 31.2 | 45.6 | 176.1 | – |
| 50 轮 × 100B | 9.1 | 25.1 | 112.6 | – |

调参 = 运行期 `MIMALLOC_PURGE_DELAY=0` + `MIMALLOC_PAGE_COMMIT_ON_DEMAND=1`
(分配次数不变, 但释放的页及时归还, 提交量回落)。

对照: 上一发布版本 (2026-09-17, 系统分配器) 同负载 50 轮 × 8KB 为
专用工作集 6.1~10.5 / 工作集 24.6~24.9 / 提交 12.7~13.5 MB —— 即当前代码的
每轮分配量与提交量都高于上一版本, 而工作集相近, 说明**多出来的部分主要是
分配器保留/提交的临时缓冲, 不是稳态数据**。

因此本轮优化无论用哪个分配器都受益: 把 churn 降下来 (尤其是逐条消息拷贝与
每步序列化), 提交量与 peak 才能跟着降。

### 0.4 代码核对与复测 (2026-09-26, 主仓 88fee6e0 / neograph 1522761)

本节是 §0.2 两条结论的更正记录。数据来自同一台机器、同一份 harness 台账脚本的
复跑, 并且**改用"N 轮与 N+1 轮相减"的差分口径**:

> 台账里的"每轮 = 累计 ÷ 轮数"会把**启动成本**和**累积项**混在一起。实测
> 1 轮 × 100 B 的进程自身就是 `malloc req~` 35.9 MiB / `huge` 14 块 (启动开销),
> 占 50 轮组总量的 2~3%。下文"每轮"一律指差分增量。

| 组 | `malloc req~` | `huge` 块数 | 每轮 `huge` 增量 |
|---|---|---|---|
| 1 轮 × 100 B | 35.9 MiB | 14 | – (启动基线) |
| 2 轮 × 100 B | 61.3 MiB | 25 | 11 块 / ≈22 MB |
| 3 轮 × 100 B | 86.7 MiB | 36 | 11 块 / ≈22 MB |
| 5 轮 × 100 B | 138.0 MiB | 58 | 11.8 块 / ≈24 MB |
| 50 轮 × 100 B (台账) | 1.3 GiB | 553 | ≈11 块 |
| 50 轮 × 1 KB (本次) | 1.6 GiB | 553 | ≈11 块 |
| 50 轮 × 8 KB (台账) | 3.1 GiB | 570 | ≈11 块 |
| 5 轮 × 80 KB (台账) | 364.8 MiB | 63 | ≈12.6 块 |

**更正 1 (原文 §0.2 第 1 点): "8 KiB 块不再是主要项, 旧推理不成立" 不成立。**
每条消息正文每轮仍被拷贝 7~9 次, 只是落到了相邻档位:

- 50 轮 × 8 KB: `bin S 37` (10 KiB, 覆盖请求 8193~10240 B) 42.9K 块
  ⇒ **858/轮 ≈ 8.6 次/条消息** —— 8 KB 正文的字符串拷贝带 1 字节结尾,
  落到 10 KiB 档而不落到 `bin S 36` 的 8 KiB 档 (后者只剩 365 块);
- 50 轮 × 1 KB: `bin S 37` 降到 332 块, `bin S 25` (1.2 KiB) 升到 35.8K
  ⇒ **716/轮 ≈ 7.2 次/条**。

所以 P1/P2 要收敛的"整段/逐条消息拷贝"依旧是主要项, 且可以直接用
`bin S 37` (或按消息大小对应的档位) 的每轮块数 ÷ 消息条数 做验收指标。

**更正 2 (原文 §0.2 第 2 点): `huge` (≥512 KiB) 不是"整段状态序列化产物"。**
`huge` 与上下文大小无关: 5 KB 上下文 (50 × 100 B) 与 400 KB 上下文 (50 × 8 KB)
的 `huge` 都是 1.1 GiB / 553~570 块, 每轮固定 ≈11 块 × ≈2 MB。
已逐一排除、都不改变 `huge` 的因素:

- 会话 SQLite 持久化 (配置去掉 `data_dir` 后 122.5 MiB / 5 轮, 与开启时
  119.6 MiB / 5 轮相同);
- `model_context_max_token` (10K 与 1M 完全相同);
- SSE 事件数 (mock 改成每响应 100 个 chunk 后仍是 25 块 / 2 轮)。

也就是说它既不是上下文序列化, 也不是流式事件或持久化的产物。定位之前,
本方案 P0 的 `huge` 验收指标 (≤100 块 / ≤200 MiB) 不能成立。

**小块档 (32 B / 128 B / 384 B) 随消息条数, 不随内容大小**: 50 轮 × 100 B 与
50 轮 × 8 KB 的三档块数逐位相同 (237.7K / 162.8K / 162.3K), 5 轮 × 80 KB
(只有 10 条消息) 降到约 1/3 ⇒ 每轮 ≈11K 次小对象分配来自 typed 层逐条
`from_json` / `to_json` (与 P2-1 / P2-3 的目标一致)。

## 1. 目标与非目标

目标 (同一 mock 负载):

1. 50 轮 × 8KB 组 `malloc req~` 从 **3.1 GiB 降到 1.2 GiB 以内** (每轮 ≤24 MB)
   → 注: 每轮固定项中 ≈22 MB 来自尚未定位的 `huge` (见目标 3 注), 故本目标需
   与 `huge` 定位一起看;
2. ~~其中的整段状态序列化类结果 (`huge` ≥512 KiB) 从 **1.1 GiB / 570 块
   降到 200 MiB / 100 块以内** (每轮 ≤2 块)~~
   → 2026-09-26 更正: `huge` 与上下文大小无关 (每轮固定 ≈11 块 × ≈2 MB, §0.4
   更正 2), 该指标改为"**消息正文档位 (8 KB 消息 → `bin S 37`) 与 M/L 档
   (16 KiB~449 KiB) 块数下降**"; `huge` 单独定位后再定指标;
3. 小上下文组 (50 × 100B) 的**每轮固定开销**从 **26 MB 降到 10 MB 以内**
   → 2026-09-26 拆分: 其中 ≈22 MB/轮 来自尚未定位的 `huge`, 仅 ≈4 MB/轮 是本轮
   P1/P2/P3 能直接降下的部分; `huge` 定位后本目标才可能整体达成;
4. 系统分配器口径: 100K 上下文的专用工作集/提交再降 3~5 MB (重测后基线
   8.2~8.5 / 21~24 MB);
5. **行为不变**: LLM 请求内容、SSE 语义、checkpoint/恢复、消息顺序与 reducer
   语义一致。

非目标:

- 不更换/不恢复默认分配器 (mimalloc 仍默认关闭, 需要时用选项打开);
- 不改 SSE 协议与请求体字段;
- 不改 `viewMessages` / `llmMessages` 双消息集 (UI 展示历史与 LLM 上下文各自
  独立维护与持久化, 是该设计的必要组成部分, 不在本轮范围);
- 不重构图引擎整体状态模型, 只加零拷贝访问 API 并收敛调用点。

## 2. 现状链路 (每轮发生的上下文级拷贝, 行号已按更新后的依赖核对)

| # | 步骤 | 位置 (2026-09-26 核对) | 粒度 | 说明 |
|---|---|---|---|---|
| 1 | `state.get_messages()` | `lib/src/nodes/modelcall.cpp:153` (`build_params`) | 整段 + 每条 | 内部 `get("messages")` 整数组深拷贝 + 逐条 `from_json` |
| 2 | `messages_to_json(params.messages)` | `lib/src/protocol/openai_provider.cpp:504` | 整段 + 每条 | typed → neograph json |
| 3 | `fromNeographJson(...)` | 同上 / `lib/include/agentxx/util/neograph_json_bridge.h:54` | 整段 + 每条 | neograph json → `utilxx_base::Json` 逐节点深拷贝 |
| 4 | `body.dump()` | `openai_provider.cpp:764 / 889 / 1069 / 1202` | 整段 | 序列化成请求体文本 (含 Responses / 非流式路径) |
| 5 | `req.body() = body;` | `third_party/cxx_utilxx/src/http_client.cpp:1358` (重定向路径 `:1201`) | 整段 | `string_view` → beast body 再拷一份 (请求期间两份并存) |
| 6 | `in.state.get("messages")` + 重建 + `overwrite` | `lib/src/nodes/modelcall.cpp:647` 读, `:677` 写 (同文件 `:633` 为修复路径) | 整段 | 每轮都把 system 消息替换/插入: 读一份深拷贝, 再整份覆盖写回 |
| 7 | 每次写 messages 通道 | `lib/.../modelcall.cpp:230 / 267 / 701`、`toolcall.cpp:546 / 1112 / 1131`, reducer 见 `neograph/src/core/graph_loader.cpp:30` (`reducer_append`), 落地见 `graph_state.cpp:96 / 149` (`write` / `apply_writes`) | 整段 | `reducer_append` 每次 `json result = current;` 深拷贝整个数组, 每轮 2~4 次 |
| 8 | 会话/运行起始播种 | `lib/src/agent/agent_runner.cpp:31`、`base_agent.cpp:983 / 1090` (`state.overwrite("messages", toNeographJson(...))`) | 整段 | 每次进入运行把会话侧消息整体转 JSON 再写进图状态 |
| 9 | `get_messages()` 其它调用点 | `modelcall.cpp:277`、`toolcall.cpp:527 / 846 / 1140` (另 `:1125` 走 `state.get("messages")`)、`summarization.cpp:721 / 1074`、`code_agent.cpp:141`(默认关) | 整段 + 每条 | 每轮 5~8 次 |
| 10 | neograph 内部 `get_messages()` | `graph_node.cpp:81 / 179 / 240`、`graph_loader.cpp:119` (`has_tool_calls` 条件)、`plan_execute_graph.cpp:104`、`graph_engine.cpp:1648` (运行结束取 final_response) | 整段 + 每条 | LLM/ToolDispatch/Intent 等节点各自再读一次 |
| 11 | 每步整段状态序列化 | `neograph/src/core/graph_engine.cpp:1370 / 1404 / 1460 / 1505 / 1541 / 1627 / 1637`, `graph_executor.cpp:222 / 772` | 整段 (含全部消息) | 只为缓存键/事件/结果就整段 `serialize()`; 其中 `graph_executor.cpp:222 / 772` 在 agentxx 路径上**不执行**, 真实的每步点是 `graph_engine.cpp:1505` (VALUES 事件) 与 coordinator 的 checkpoint (见下方补正) |
| 12 | 异常路径整段读取 | `lib/include/agentxx/nodes/wrap_handle.h:262` (`state.get("messages")`) | 整段 | 仅在节点异常重抛时发生, 低频 |
| 13 | SSE 逐行解析 | `openai_provider.cpp:1330` (`processSseBuffer`) / `:1355` (`processSseLine`) | 单事件 | 每事件 3 次堆分配 + `buf.erase(0,n)` 逐行搬移 (字节量小, 分配次数多) |

**2026-09-26 行号核对与补正** (逐点在源码中确认, neograph `1522761` / 主仓 `88fee6e0`):

- 上表 13 行的行号**全部一致**; 需要补进来的上下文级拷贝点:
    - **每 super-step 的 checkpoint**: `graph_coordinator.cpp:258`
      (`save_super_step_async`) 与 `:290` (`commit_super_step_async`, 由
      `graph_engine.cpp:1615` 每步调用) 各一次 `channel_values = state.serialize()`;
      admin 路径 `graph_engine.cpp:530 / 573` (`update_state` / `update_state_writes`) 同;
    - **运行结束的权威结果**: `graph_engine.cpp:1637 result.output = state.serialize()`,
      随后 `agent_runner.cpp:55 channel_raw("messages")` (整份值拷贝) +
      `fromNeographJson` (逐节点) —— 同一份上下文两次深拷贝 (表中未列);
    - **压缩回写**: `summarization.cpp:988 / 1058` 是 `overwrite` 整份写;
    - **resume 前**: `agent_runner.cpp:30-31` 的 `update_state` + `overwrite` 整份写;
- `graph_executor.cpp:222` (`hash_state_for_cache`) **不会执行**: 前置条件是
  `node_cache_ && !cb`, 而 agentxx 从未启用节点缓存 (全库无 `node_cache` 命中),
  且每轮都传流式回调; `graph_executor.cpp:772` (`state_snapshot`) 只在 Send 扇出
  (多 worker) 路径, 而 agent 图定义只有 `messages` / `channel_savedGraphData`
  两个通道 (`base_agent.cpp:456-467`), 无 Send;
- `graph_executor.cpp:455 / 526` 的 serialize 只在 `NodeInterrupt` 时发生 (冷路径);
- `wrap_handle.h:262` (`state.get("messages")`) 只在节点异常重抛时发生, 与表中一致;
- 全部 `messages` 通道写点 (含绕过 reducer 的 `overwrite` / `restore`) 见 §6 问题 2。

已经做过优化、本轮保持的部分: SSE 用 `utilxx_base::JsonView` 零拷贝路由
(`openai_provider.cpp:1389` 起)、HTTP 层 `respBody.clear()` 复用容量
(`http_client.cpp` `flushBody`)、工具结果裁剪、`viewMessages`/`llmMessages`
双消息集 (设计需要, 保持不变)。

## 3. 方案

### P0. 收敛"每步整段状态序列化" (neograph 侧)

重测确认这项确实每步都在发生, 但**发生点与原文不同**; 按"实际执行频次 × 改动风险"
重排如下 (2026-09-26 核对, 依据见 §2 补正与 §0.4):

1. **去掉 `StreamMode::VALUES` (性价比最高, 零行为风险)**:
   `base_agent.cpp:986` 传入了 `StreamMode::VALUES`, 于是
   `graph_engine.cpp:1503-1505` 每 super-step 都执行
   `cb(GraphEvent{CHANNEL_WRITE, "__state__", state.serialize()})`;
   而 agentxx 侧 `EventBridge::handleChannelWrite` (`event_stream.cpp:206-239`)
   只处理 `message_tip` 与 `messages`, `__state__` 直接 return —— 载荷无人消费。
   去掉该模式即省掉每步一次整段序列化, 不动协议/回调语义;
2. **checkpoint 落盘的整段序列化**: coordinator 启用
   (`base_agent.cpp:312` 挂 `InMemorySingleCheckpointStore`) 时每 super-step
   都有 `channel_values = state.serialize()`
   (`graph_coordinator.cpp:258 / 290`, admin 路径见 `graph_engine.cpp:530 / 573`)。
   可只对参与缓存的通道取内容 (例如新增 `GraphState::hash_channels(names)`),
   或对消息通道用"长度 + 尾部若干条摘要 + 版本号"的廉价指纹;
   **注意** `resume` 依赖 `state.restore(channel_values)`
   (`graph_engine.cpp:1278 / 1323`), 指纹化后仍需能重建可恢复状态;
3. **运行结束的权威结果**: `graph_engine.cpp:1637 result.output = state.serialize()`
   与 `agent_runner.cpp:55` 的 `channel_raw("messages") + fromNeographJson`
   是同一份上下文的两次深拷贝 → 合并为"直接把权威消息写进会话, 不再经 output 往返";
4. **不必改的**: `graph_executor.cpp:222 / 772` 在 agentxx 路径上不执行,
   `graph_executor.cpp:455 / 526` 只在 `NodeInterrupt` 时 serialize (§2 补正)。

验收信号: M/L 档 (16 KiB~449 KiB) 块数下降; 用**单轮差分口径**对比
(见 §4 测量方法), 不要再用"huge ≤100 块"作为本项指标 —— 实测 `huge`
与上下文大小无关, 需要先单独定位 (§0.4 更正 2 / §6 问题 3)。

### P1. 请求体链去重 (agentxx 侧, 低风险)

实测支撑 (§0.4 更正 1): 每条消息正文每轮仍被拷贝 **7~9 次** (8 KB 消息 →
`bin S 37` 858 块/轮; 1 KB 消息 → `bin S 25` 716 块/轮), 所以本节的"省掉 1~2 份
整段拷贝"是能直接量出来的收益, 验收只看对应档位块数的下降。

**P1-1 `requestSseAsync` 支持移动 body**

- 现状: `HttpClient::requestSseAsync(..., std::string_view body, ...)` → `req.body() = body;`
- 改法: 增加 `std::string&&` 重载 (或 `ByValue`/`TakeBody` 显式参数), 内部
  `req.body() = std::move(body);`, 之后 `prepare_payload()`; 调用方
  (`OpenAIProvider::doStream`) 传 `std::move(bodyStr)`
- 收益: 省 1 份整段拷贝 + 降峰值 1 份 (100K ≈ 0.4 MB/轮, 200K ≈ 0.8 MB/轮)
- 注意: 重定向/重试路径 (`requestAsync` 的同名逻辑, 见 `http_client.cpp:1201`)
  需要 body 存活 → 仅在"不跟随重定向/一次性发送"的 SSE 路径移动, 其它路径保持拷贝

**P1-2 删掉 `fromNeographJson` 这一整份深拷贝**

- 现状: `body["messages"] = fromNeographJson(messages_to_json(params.messages));`
  (`openai_provider.cpp:504`) —— neograph json 先建一棵树, 再逐节点深拷贝到
  `utilxx_base::Json`, 最后 `dump()` 成文本
- 改法: 让 provider 直接持有 neograph json 作为 body (`body["messages"] =
  messages_to_json(...)`), 用 `neograph::json::dump()` 产出请求体文本;
  `utilxx_base::Json` 只在需要写日志/埋点的裁剪路径上按需构造
- 收益: 省 1 份整段 + 每条的深拷贝 (≈0.4~0.8 MB/轮)
- 注意: 目前 provider 的 body 类型是 `utilxx_base::Json`, 需同时改
  `buildBody` / `buildResponsesBody` / `completeAsync` / `doStream` 的签名与
  `applyHeaders` 无关部分; `params.extra_fields` 等字段构造要逐一核对

**P1-3 (依赖 P2) `build_params` 不再做 typed ↔ JSON 往返**

- 现状: `state.get_messages()` (JSON→typed) → 插 system 消息 → `messages_to_json` (typed→JSON)
- 改法: provider 直接接收 state 的 messages JSON 引用 (P2 的零拷贝 API),
  system 消息替换在 JSON 层完成 (或由调用方一次性给出 `messages_override`)
- 收益: 省 2 份整段 (含 P1-2 合计每轮省 3~4 份上下文拷贝)

### P2. 图状态零拷贝访问 + 就地 append (neograph 侧)

注: 重写后的 neograph 已经有 `ChannelWrite::Mode::Overwrite`
(`graph_state.cpp:129` `apply_writes`)、`GraphState::overwrite(json&&)`
(`:105`) 与 `GraphState::remove()` (`:121`), 但仍**没有**本节要加的
"回调式只读访问"和"就地 append" —— 这两项加进去与现有 API 并列即可。

**P2-1 零拷贝读取 API**

`GraphState::get()` (`graph_state.cpp:64`) 按值返回 `json`, `get_messages()`
(`:71`) 再叠加一次逐条反序列化。新增 (保留旧接口不删, 老调用点可逐步迁移):

```cpp
// 读取: 回调形式, 锁在回调期间持有 (不能返回裸指针/引用, 否则解锁后可能被写者失效)
template <class Fn>
decltype(auto) with_json(std::string_view channel, Fn&& fn) const;

// 消息专用: 逐条给出 ChatMessage (不整体拷贝消息数组)
template <class Fn>
void for_each_message(Fn&& fn) const;          // fn(const ChatMessage&) 或 fn(ChatMessage&&)

// 需要整体视图时 (只读、不跨写操作): 快照句柄, 内部持有 json 的所有权
std::shared_ptr<const json> messages_snapshot() const;
```

- `get_messages()` 内部改走 `for_each_message` 以省掉"整数组 json 拷贝"这一步
  (逐条 `from_json` 仍然需要, 但不再整体复制文档);
- 注意锁语义: `get()` 目前用 `shared_lock` 后返回副本; 新 API 必须把锁的持有期
  限定在回调内, 文档里明确"回调中不得再写同名通道"。

**P2-2 `reducer_append` 就地追加**

- 现状 (每轮 2~4 次整段深拷贝, `graph_loader.cpp:30`):

```cpp
static json reducer_append(const json& current, const json& incoming) {
    json result = current.is_array() ? current : json::array();   // 整份深拷贝
    for (const auto& item : incoming) result.push_back(item);     // 逐条再拷贝
    return result;
}
```

- 改法 (二选一, 建议先做 (a)):
  - (a) 新增 `ChannelWrite::Mode::Append`: 在 `apply_writes`
    (`graph_state.cpp:129`) 里对 append 模式直接在通道值上
    `append_in_place(w.value)` (yyjson 可变文档 `yyjson_mut_arr_add_val`,
    必要时先 `reserve`); 结果值与现有 `reducer_append` 完全一致, 写日志/checkpoint
    重放语义不变;
  - (b) 新增 reducer 名 `"append_inplace"` (base_agent 的图定义里 messages 通道改用),
    语义与 (a) 等价, 改动更局部;
- **实现约束 (2026-09-26 核对)**: 内置 reducer 签名是
  `json(const json&, const json&)`, 无法在原地改通道值, 所以 (a) 必须落在
  `apply_writes` (`graph_state.cpp:129`); 此外 `in.state.write("messages", ...)`
  的直写点 (`modelcall.cpp:267 / 701`、`toolcall.cpp:1112`) 与运行起始的
  `apply_input` (`graph_engine.cpp:1328`) 仍会走 reducer 深拷贝, 需要改成
  `ChannelWrite{..., Mode::Append}` 或新增 `GraphState::append_in_place()` 才生效;
- 注意: 就地修改会让此前通过 `with_json`/`messages_snapshot` 获得的视图失效 →
  文档要求"视图生命周期内不得写入", 并且在 type/结构检查失败时回退到深拷贝路径;
- 注意: yyjson 文档扩容会搬移内部缓冲, 因此不要长期持有 `json` 内部指针 (现有封装
  `operator[]` 句柄已隐含这一点, 新 API 要写清楚)。

**P2-3 调用点收敛 (agentxx 侧)**

| 位置 | 现状 | 改法 |
|---|---|---|
| `modelcall.cpp:153` `build_params` | 读整份 messages | 交给 provider 直接读 (P1-3), 或只读一次后复用 |
| `modelcall.cpp:647`→`:677` (system 消息替换) | 每轮读一份深拷贝 + 整份覆写 | 用回调式 API 只判断首条是否是 system; 覆写改为"只改首条"的写入 (新增 `update_first` 或 `Mode::Overwrite` 的局部版本) |
| `modelcall.cpp:277` `repairMessages` | 再读一份, 可能整体覆写回 | 先判定"是否需要修复"(轻量检查: 遍历 + 早退), 需要时才取可写视图 |
| `toolcall.cpp:527 / 846 / 1140` | 3 次整份读取 | 抽一次读取结果在同一次 tool 执行内复用; 只读部分 (取最后一条 tool 消息等) 改 `for_each_message` 反向查找 |
| `summarization.cpp:721 / 1074` | 读整份 + `countTokens` | 用 `for_each_message` 累加 token; 只在真正触发压缩时才取整份 |
| `agent_runner.cpp:31` / `base_agent.cpp:983 / 1090` | 运行起始整段播种 | 与 P2-2 的就地 append 结合, 或改成"只在确实变了"时才覆写 |
| neograph `graph_node.cpp:81 / 179 / 240`、`graph_loader.cpp:119`、`plan_execute_graph.cpp:104`、`graph_engine.cpp:1648` | 每个节点各读整份 | 改 `for_each_message` 或加"节点内一次性读取"缓存 (节点运行期 state 不会被别的节点写) |

- 目标: 每轮整份读取次数从 10+ 次降到 2~3 次 (进入 LLM 前一次、写回一次)。

### P3. SSE 行解析 (低风险, 主要降 CPU / 分配次数)

- `processSseBuffer` (`openai_provider.cpp:1330`): 用 `std::string_view` 就地在
  缓冲上找 `\n` 并解析, 记录"已消费偏移"; 缓冲只在 `offset > 阈值` (如 8KB) 或
  整段处理完时 `erase(0, offset)` 一次, 去掉"每行搬移剩余缓冲";
- `processSseLine` (`:1355`): 去掉 `std::string line{line_in}` 与
  `payload = line.substr(5)` 两次分配, 改 `string_view` 就地裁剪 (`\r`、前导空格、尾部空白);
- 快速路径: 仅当行长含 `"usage"` / `"tool_calls"` / `finish_reason` 时才走
  `JsonView`; 纯 `delta.content` 行用字符串扫描取引号内文本 (注意转义 `\"`、`\\`、`\uXXXX`);
  保留现有 `JsonView` 分支作为慢路径 (出现意外字段时回退);
- 收益: 每个事件 3 次堆分配 → 事件数 × 3 次堆分配被消除; 字节量小, 主要体感在 CPU 与分配器压力;
- **验收提示 (2026-09-26)**: 台账用的 `harness/mock_llm.py` 每个响应只发 **4 个 SSE 事件**
  (role / content / finish / `[DONE]`), 所以本项在现有台账上量不出差别 —— 验收要么让 mock
  真流式 (每响应数百 chunk; 已实测 100 chunk/响应时小档位块数明显上升), 要么改用真实模型。
  原文"每轮约 2000 个事件"与方案自己使用的测量负载不符;
- 测试: 覆盖 `\r\n`、跨块切断、`data:` 后无空格、`[DONE]` 无尾随换行、
  超长单事件、`\uXXXX` 转义等边界 (现有 `openai` 相关测试模块 + 新增用例)。

### P4. 次要项 / 已核对项

- **会话持久化 (2026-09-26 核对)**: 展示历史确实逐条写
  (`lib/src/agent/session_store.cpp:138` `updateViewMessage`, DB 连接按会话缓存,
  不是每次 open); 但 **LLM 上下文是整份 dump** —— `session_store.cpp:704`
  `saveLlmMessages` 把整个 `llmMessages` 序列化后写 `llm_context` 表, 且
  `base_agent.cpp:1146` 每轮结束都会调用一次 (另有节流窗口内的
  `appendSettledLlmMessages`)。属持久化语义的一部分, 本轮不改;
- 工具 schema 每轮重建: `build_params` 每次重建 `tool_defs`
  (`modelcall.cpp:180-186`) 并追加插件工具 (`:191 appendDefinitions`,
  实现 `tool_registry.cpp:54`) → 若工具数多可缓存, 待量化后再定;
- `bin S 37` (10 KiB, 每轮 ≈858 块) 与 `bin S 18` (384 B, 每轮 ≈3.2K 块)
  **已定位, 无需再采样** (见 §0.4 更正 1 与 §6 问题 3): 前者是逐条消息正文拷贝,
  后者是 typed 层逐条解析的小对象;
- **仍待定位: `huge`** (≥512 KiB, 每轮 ≈11 块 × ≈2 MB) —— 已排除会话持久化、
  `model_context_max_token`、SSE 事件数; 采样步骤与坑见 §6 问题 3;
- wire 协议侧是否存在整份上下文推送 (TUI 的 LLMContext 视图) → **已确认没有按轮
  推送**: 客户端只在"LLM 上下文"菜单动作时发 `WireGetContext`
  (`agent_tui.cpp:1184-1187`), 服务端 `session_server_agent_io.cpp:495-510`
  才整份拷贝 `llmMessages`;
- `LogPrint` 中间件默认关闭 (`include/agentxx/agent/config.h:356`), 不在本轮范围。

## 4. 实施顺序与验收

建议顺序 (每步独立可验证、可回退):

| 步骤 | 内容 | 验收信号 |
|---|---|---|
| 1 | P0-1 去掉 `StreamMode::VALUES` (`base_agent.cpp:986`) + P0-2 checkpoint 序列化指纹化 | M/L 档 (16 KiB~449 KiB) 块数下降 (单轮差分口径); **不设 `huge` 指标** |
| 2 | P1-1 body move | `malloc req~` 再降 ~20 MiB (50 轮); 无行为差异 |
| 3 | P1-2 去掉 `fromNeographJson` | 再降 ~20~40 MiB; 请求体与改造前逐字节一致 (mock 记录 body 做 diff) |
| 4 | P3 SSE 行解析 | 事件数不变; 每轮小分配明显减少; 边界用例全过 (**需先换真流式 mock**, 见 P3 验收提示) |
| 5 | P2-1 零拷贝读取 API + 调用点收敛 | 整段读取次数 ≤3 次/轮; 对应档位块数下降 |
| 6 | P2-2 就地 append | 消息正文档位 (8 KB 消息 → 10 KiB 档) 与 32/128/384 B 档块数下降; checkpoint 恢复测试通过 |

并行推进: **`huge` (每轮 ≈11 块 × ≈2 MB) 的定位** (§6 问题 3) —— 它是"每轮固定开销"
里最大的一块 (≈22 MB/轮), 不定位就无法达成 §1 目标 1/3 的下降幅度。

测量方法 (2026-09-26 / 88fee6e0 实际使用的台账, 可复现; 驱动脚本见
`resource/benchmark/harness/`):

1. 起本地 OpenAI 兼容 mock 服务 (Python, 只回 SSE, 立即返回), 配置
   `base_url: http://127.0.0.1:<port>/v1`、`plugin.list: []`、
   `model_context_max_token` 取 1M (避免压缩介入), data_dir 指向临时目录;
2. `agentxx_cli cli --config <cfg>` 经 stdin 逐条送入固定大小的用户消息
   (每条一行, 每条一轮), 全部轮次被 mock 服务收到后空转 4 秒;
3. 采样 (300 ms 一次): 工作集 (`WorkingSet64`)、专用工作集
   (`Win32_PerfFormattedData_PerfProc_Process.WorkingSetPrivate`, 即任务管理器
   "内存"列)、提交 (`PrivateMemorySize64`)、峰值提交
   (`PeakPagedMemorySize64`)、CPU (`UserProcessorTime` / `PrivilegedProcessorTime`);
4. 关闭 stdin 让进程正常退出, 取 `MIMALLOC_SHOW_STATS=1` 的退出统计
   (`malloc req~` 与分 bin 明细);
5. 每个配置至少跑 3 次, 取范围 (本次基线: 50 轮 × 8KB 组 mimalloc
   `malloc req~` 3.1 GiB; 系统分配器专用工作集 8.2~8.5 MB, 提交 21~24 MB);
6. **（2026-09-26 补充）每轮量必须用差分口径**: 再跑 N=1/2/3 轮的短组, 用
   (N+1 轮 − N 轮) 得到"每轮增量", 不要用"累计 ÷ 轮数" —— 单进程启动自身就是
   `malloc req~` 35.9 MiB / `huge` 14 块 (占比 2~3%), 会把"每轮固定项"和"累积项"
   混在一起 (本次据此修正了 §0.2 的两条结论)。

回归测试 (必须覆盖):

- 图 state: messages 通道 append 后的值/顺序与旧 reducer 一致; checkpoint 写出→恢复→
  继续 append 的结果一致; 并发读写 (不同 thread_id 的会话) 不串;
- provider: 请求体字段与旧实现逐字节 diff 一致 (含 tools/extra_fields/stream_options);
- SSE: 上述边界用例 + 截断检测 (`missing [DONE]`) 行为不变;
- 既有测试模块: `openai` 相关、`session_persistence`、`memgrowth`、`interrupt_ui`。

## 5. 风险与回退

| 风险 | 说明 | 回退 |
|---|---|---|
| 就地 append 破坏 checkpoint 语义 | 结果值等价, 但写日志/重放依赖 reducer 契约 | 用新增 `Mode::Append`/新 reducer 名, 旧路径保留; 出问题切回 `"append"` |
| 零拷贝视图被写失效 | 回调后返回引用会悬空 | 只提供回调式 API + 快照句柄; 文档明确锁与生命周期 |
| 请求体改造引入字段差异 | body 类型从 `utilxx_base::Json` 换到 `neograph::json` | 用 mock 服务记录 body 逐字节 diff, 差异即回退信号 |
| SSE 快速路径漏转义 | `\uXXXX`/转义处理不完整会导致文本错误 | 只在"纯 content 且无转义序列"时走快速路径, 其余回退 JsonView |
| 减少序列化影响上游语义 | 缓存键/事件载荷是上游重写后新增的机制, 直接删可能影响节点缓存命中与可观测性 | 先按"键的等价替换"(只换成轻量指纹) 做, 保留开关; 事件载荷方面: 已确认 agentxx 侧不消费 `__state__` (`EventBridge` 只处理 `message_tip` / `messages`), 故去掉 `StreamMode::VALUES` 无消费方风险 |
| 依赖库继续演进导致行号漂移 | 本次已按 `1522761` 核对全部调用点 | 方案按函数名/符号定位; 每次更新依赖后重跑 §4 台账 |
| `huge` (≥512 KiB) 未定位 | 实测每轮固定 ≈11 块 × ≈2 MB, 与上下文大小、消息大小、SSE 事件数、`model_context_max_token`、会话持久化都无关; 不定位则 §1 目标 2/3 无法达成 | 先做一次真实调用栈采样 (方法与坑见 §6 问题 3), 定位后单独作为一项处理 |
| 采样方法本身的坑 | `cdb` 的 `bp` 命令串里 `||` / `&&` 会被当命令分隔符; `-p PID` 附加过早会让目标进程退出; 有断点时 kill cdb 会连带杀掉目标, 于是拿不到 `MIMALLOC_SHOW_STATS`, 也无法确认该轮是否跑完 | 从启动就 `cdb -cf script.txt <exe> <args>` 并把 stdin 指向文件; 条件改用嵌套 `.if`; 采样前先确认"被调试状态下 `huge` 仍出现" |

## 6. 待确认问题 (2026-09-26 核对结论)

1. **neograph 侧是否接受 P0 与 P2 的新 API —— 可以, 且有先例**:
   `ChannelWrite::Mode::Overwrite`、`GraphState::overwrite(json&&)`
   (`graph_state.cpp:105`)、`remove()` (`:121`)、`apply_writes` (`:129`)、
   `GraphEngine::update_state(_writes)` (`graph_engine.cpp:497-573`) 都是本仓
   fork 加进去的。实现约束见第 2 条后半;
2. **`messages` 通道的写入路径 —— 已核对清单如下, P2-2 需一并适配**:
    - 走 reducer (append): `NodeOutput.writes` (`modelcall.cpp:229`、
      `toolcall.cpp:546 / 1130`、`plugin_graph_node.cpp:142`)、
      `in.state.write` (`modelcall.cpp:267 / 701`、`toolcall.cpp:1112`)、
      运行起始 `apply_input` (`graph_engine.cpp:1328`)、resume 值写入
      (`graph_engine.cpp:1304`);
    - 绕过 reducer (整份替换): `state.overwrite` (`modelcall.cpp:633 / 677`、
      `summarization.cpp:988 / 1058`、`base_agent.cpp:1090`、
      `agent_runner.cpp:31`)、checkpoint 恢复 `state.restore`
      (`graph_engine.cpp:1278 / 1323`)、`update_state` 内部
      "restore → 写 → 再整份序列化成新 checkpoint";
    - 实现约束: 内置 reducer 签名是 `json(const json&, const json&)`, 不能原地改
      通道值 ⇒ P2-2(a) 必须落在 `apply_writes`, 并让 `state.write` 的直写点改用
      `ChannelWrite{Mode::Append}` 或新增 `GraphState::append_in_place()`;
3. **`bin S 37` / `bin S 18` 的来源 —— 已定位, 不再需要采样**:
    - `bin S 37` (10 KiB) = **逐条消息正文拷贝**: 50 × 8 KB 组 858 块/轮
      (≈8.6 次/条消息, 8 KB 正文 + 1 字节结尾落到该档); 50 × 1 KB 组则落到
      `bin S 25` (1.2 KiB) 716 块/轮 (≈7.2 次/条) —— 见 §0.4 更正 1;
    - `bin S 18` (384 B) / `bin S 12` (128 B) / `bin S 4` (32 B) = typed 层逐条
      `from_json` / `to_json` 的小对象, 与消息**条数**成正比 (50×100B 与 50×8KB
      逐位相同, 5×80KB 只有约 1/3);
    - **仍待定位的是 `huge`** (≥512 KiB, 每轮 ≈11 块 × ≈2 MB): 已排除会话持久化
      (去掉 `data_dir` 后不变)、`model_context_max_token` (10K 与 1M 相同)、
      SSE 事件数 (每响应 100 chunk 时不变)。采样建议
      `cdb` 断点: `bp mimalloc!mi_theap_malloc ".if (@rdx > 0x80000) { .printf ...; kb 16; g } .else { gc }"`
      (同法加 `mi_malloc_aligned` / `mi_theap_malloc_aligned`); 坑: bp 命令串里
      `||` / `&&` 会被 cdb 当命令分隔符 (改用嵌套 `.if`), `-p PID` 附加过早会让
      进程退出, 有断点时 kill cdb 会连带杀掉目标 (拿不到 `MIMALLOC_SHOW_STATS`,
      也无法确认该轮是否跑完) —— 建议从启动就 `cdb -cf script.txt <exe> <args>`
      并把 stdin 指向文件。附加调试器期间只在 `mi_malloc` 观察到 <90 KB 的分配,
      所以采样前要先确认"被调试状态下这 11 块 `huge` 仍出现";
4. **是否新增"逐轮长上下文"基准场景 —— 需要, 且现有负载量不到 P3**:
   `resource_cli` 是注入上下文 (`bench_resource.cpp:375 / 453`
   "injected fixed groups"), 序列化次数少于真实逐轮对话; 另外
   `harness/mock_llm.py` 每个响应只发 4 个 SSE 事件, 与 P3 假设的
   "每轮约 2000 个事件"不符, P3 验收前需让 mock 真流式 (每响应数百 chunk)
   或改用真实模型。
