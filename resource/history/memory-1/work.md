# 实施记录: 降低每轮上下文拷贝 (请求体 JSON / SSE 解析 / 图状态消息数组)

- 方案: [plan.md](plan.md)
- 类型: 性能与内存优化 (不改协议/checkpoint 语义)
- 时间: 2026-09-26 (承接 [messages-1](../messages-1/work.md) 的上下文迁出改造)
- 状态: 计划中仍适用的项 (P1-1 / P1-2 / P3) 已实施、编译与测试通过、并完成
  release 台账复测; P0 / P2 经逐项复核**已由 messages-1 达成或失效** (见 §1.5);
  P1-3 只达成一半 (见 §1.3); `huge` **已定位**: 是 asio 协程帧 (每轮 11 帧
  ≈22 MB), MSVC 构建给这些帧预留的尺寸远超实际用量, GCC 构建上不存在 (见 §3.1)

## 1. 已完成任务

### 1.1 P1-1 HTTP 请求体移动 (cxx_utilxx)

- `requestSseAsync` 的请求体参数由 `std::string_view` 改为**按值 `std::string`**
  (接住后 `req.body() = std::move(body)`)。
  - 原签名必然要在 `req.body() = body;` 处再拷一份; 改为按值后, 调用方传右值时
    不再产生额外拷贝, 传左值/字面量时行为与原先一致 (拷一份)。
  - 曾尝试"新增 `std::string&&` 重载 + 保留 `std::string_view` 版本", 但传
    字符串字面量 (如 mcp_client 的 `""`) 时两个重载都是用户自定义转换 → 调用
    不明确 (C2668); 单一按值签名同时解决歧义与拷贝两个问题。
- 调用点: `openai_provider.cpp` 的 `doStream` / `doStreamResponses` 传
  `std::move(bodyStr)`; 其它协议与测试路径不变 (左值传入仍然只拷一份)。
- 非流式路径 (`requestAsync` / `postAsync`) 保持原样: 它按重定向次数复用
  `currentBody`, 省一份拷贝的收益小、改动面大, 按方案建议不做。

### 1.2 P1-2 请求体消息链去重

- 新增 `agentxx/protocol/provider_common.h` 的
  `chatMessagesToOpenAIJson(messages, sendThinking)` / `chatToolsToOpenAIJson(tools)`:
  由 typed 上下文**直接构造** `utilxx_base::Json` 的 messages / tools 数组。
  - 去掉 `neograph::messages_to_json` -> `fromNeographJson` 的中转:
    即一份整段 neograph DOM + 其中每条消息的中间对象。
  - 关闭思考时不再"构造后逐条删 `reasoning_content`", 直接在构造时跳过。
  - 与 `neograph::messages_to_json` / `tools_to_json` 的输出逐字段一致
    (角色/正文/tool_calls/tool 应答/多模态分片顺序/无参数 schema 回退空对象)。
- `OpenAIProvider::buildBody` 改用上述入口。
- 字节等价由新增用例
  `test_request_body_messages_json_matches_legacy_path` /
  `test_request_body_tools_json_matches_legacy_path` 覆盖 (转义字符/中文/emoji/
  控制字符、空正文 + tool_calls、tool 应答、reasoning_content 与
  reasoning_details、4 种多模态分片、空上下文; 对 `sendThinking` 两种取值分别
  与旧路径 `dump()` 逐字节比对)。

### 1.3 P3 SSE 行解析

- `processSseBuffer` / `processResponsesSseBuffer`: 记录"已处理偏移",
  整块只 `erase` 一次剩余的不完整行; 旧实现每解析一行就
  `erase(0, n)` 把剩余缓冲整段前移 (事件密度高时是纯浪费)。
  缓冲消费语义 (保留未收完的行、`finalFlush` 补解析末行) 不变。
- `processSseLine` / `processResponsesSseLine`: 行与负载就地用 `string_view`
  裁剪 (`\r`、`data:` 前缀、前导空格、行尾空白), 去掉每事件两次 `std::string` 堆分配。
- **未做**: 方案 §P3 第 3 条"纯 content 行用字符串扫描取引号内文本"的快速路径。
  原因: (a) 现有 `JsonView` 已经是零拷贝导航 (只多一份 padded 输入 + tape),
  收益只剩每事件 1~2 次小分配; (b) 手写扫描要覆盖转义、`arguments` 里内嵌
  JSON 文本等情形, 出错的后果是"静默丢内容", 风险明显大于收益; (c) 本机台账的
  mock 每个响应只发 4 个 SSE 事件, 量不出差别 (方案 §P3 验收提示已指出)。

### 1.4 逐轮长上下文基准场景 + 真流式 mock (方案 §6 问题 4)

- `agentxx_benchmark` 的 `resource_cli` 场景末尾新增**真实逐轮往返**段 (采样点
  `rounds<N>x<bytes>`): 每轮都走完整链路 (用户输入 → LLM 请求/SSE → 工具调用与
  结果回写 → 消息落库 → 事件回传), 与真实对话一致; 此前的 P1/P2 采样点是直接
  注入历史, 序列化与事件次数都少于真实逐轮。
  - 轮数/消息大小: `AGENTXX_BENCH_ROUNDS` (默认 20) / `AGENTXX_BENCH_ROUND_BYTES`
    (默认 8192); 报告 note 记录轮数与平均/最大单轮耗时。
  - 实测 (本机, 200K 上下文后 2 轮 × 1KB): RSS 38.75 → 53.12 MB (+14.6 MB),
    单轮约 43 ms —— 说明真实逐轮的开销明显高于注入历史的负载 (正是本节要补的量)。
- mock LLM 现在支持"回复切成 N 个 SSE 事件": 基准侧 `AGENTXX_BENCH_STREAM_CHUNKS`,
  台账脚本侧 `mock_llm.py --chunks N` (默认 1 = 单事件), 用于量出 P3 逐事件解析
  路径的收益 (方案 §P3 的验收提示: 原来的 mock 每响应只发 4 个事件, 量不出差别)。
- 文档: `docs/zh-cn/design/benchmark.md` 第 1 节补上新场景与环境变量说明,
  第 10.5 / 11 节把"逐轮长上下文场景未补"和"`huge` 未定位"的旧结论更新为现状。


### 1.5 P0 / P2 复核: 已由 messages-1 达成或失效

| 计划项 | 复核结论 |
|---|---|
| P0-1 去掉 `StreamMode::VALUES` | **已完成** (messages-1 阶段 0); 现在 `base_agent.cpp:1027` 只传 `EVENTS \| TOKENS \| UPDATES` |
| P0-2 checkpoint 每步整段序列化指纹化 | **不再需要**: 默认图只有 `channel_savedGraphData` 与 `xx_messagesMeta` 两个通道 (`base_agent.cpp:486-498`, reducer 均为 overwrite, 注释明确"上下文本身不在图状态里"), `state.serialize()` / checkpoint 载荷与上下文大小无关 |
| P0-3 运行结束双拷贝合并 | **已完成** (messages-1 阶段 0, 删除 `channel_raw + fromNeographJson` 往返) |
| P2-1 图状态零拷贝读取 API | **失效**: `messages` 通道已不存在, 没有可零拷贝读取的对象 |
| P2-2 `reducer_append` 就地追加 | **失效**: 同上 (reducer append 不再承载消息) |
| P2-3 调用点收敛 (`state.get_messages()` 10+ 次/轮) | **失效**: 调用点已随 messages-1 改为会话 typed 读取 |

### 1.6 P1-3 复核: 达成一半

- 方案的原始描述是 "`build_params` 不再做 typed ↔ JSON 往返": 迁移后 `modelcall`
  已直接取会话 typed 上下文 (不再 JSON→typed), 本轮又去掉了 provider 侧的
  typed→JSON→JSON 双转换 ⇒ "往返"这一项已完成。
- 仍保留的一步: `build_params` 把会话 `std::vector<ChatMessage>` **拷贝**一份给
  `CompletionParams::messages` (值语义容器, 每轮 1 份)。
  没有改为借用引用是刻意的: 借用指针要跨 `co_await` 存活, 期间插件/子代理
  若写入会话 (realloc) 就会悬空, 属生产环境的隐性挂点; 收益只是"省 1 份",
  不值得引入该风险。若要继续降, 建议走"会话侧不可变快照
  (`shared_ptr<const vector<ChatMessage>>`)"而不是裸借用。

## 2. 复测数据 (release + mimalloc, 50 轮 × 8KB, 各跑 2 次)

环境与台账与 plan §4 一致 (`resource/benchmark/harness/run_load.ps1`,
`MIMALLOC_SHOW_STATS=1`, 无插件 / 单 mock 模型 / 1M 上下文 / 空闲 4 秒取稳态);
"改造前" = 本次提交前的 release 产物 (同一构建目录、同一运行环境)。

| 指标 | 改造前 | 改造后 | 变化 |
|---|---|---|---|
| `binned` 累计 | 593.6 MiB / 415.6K 块 | 462.3 MiB / 344.9K 块 | **−131.3 MiB (−22.1%) / −70.7K 块 (−17.0%)** |
| `bin S 37` (10.0 KiB, 逐条消息正文) | 298.9 MiB / 30.6K 块 | 210.7 MiB / 21.5K 块 | **−88.2 MiB (−29.5%) / −9.1K 块 (−29.7%)** |
| `bin S 26` (1.5 KiB) | 933 块 | 584 块 | −349 块 |
| `bin M 40` (16.0 KiB) | 263 块 | 213 块 | −50 块 |
| `bin M 44` (32.1 KiB) | 214 块 | 166 块 | −48 块 |
| `bin L 52` (128.5 KiB) | 80 块 | 41 块 | −39 块 |
| `bin L 56` (257.0 KiB) | 120 块 | 94 块 | −26 块 |
| `huge` (≥512 KiB) | 1.1 GiB / 571 块 | 1.1 GiB / 571 块 | 不变 (与请求链无关, 见 §3.1) |
| `malloc req~` | 1.6 GiB | 1.5 GiB | −0.1 GiB |
| 峰值提交 | 114.5 MiB | 114.5 MiB | 不变 |
| 峰值 RSS | 50.3~50.8 MiB | 51.5~52.7 MiB | 噪声范围内 |
| CPU user / system | 0.296~0.343 s / 0.468~0.546 s | 0.203~0.218 s / 0.546~0.671 s | 仅参考 (单轮时长毫秒级, 噪声大) |

- 两次运行的关键数字**逐位相同** (binned / S 37 / huge), 即改造后结果可复现。
- 原始采样数据放在本地台账目录
  `resource/benchmark/2026-09-26_f59814d6_windows-longctx-request-chain/`
  (4 份 `summary.json` + 说明; 与既有台账同放, 按 `.gitignore` 约定不入库)。
- 验收信号与方案 §4 一致: 消息正文档位 (`bin S 37`) 与 M/L 档块数下降;
  `huge` 不作为本轮指标 (方案 §0.4 更正 2 已说明它需单独定位)。

### 每条消息每轮的拷贝次数 (承接 messages-1 §1.1)

| 项 | messages-1 后 | 本轮后 |
|---|---|---|
| `build_params` 取会话上下文 (`messages.assign`) | 1 | 1 (§1.6, 未改) |
| provider: typed -> neograph json (`messages_to_json`) | 1 | 0 |
| provider: neograph json -> utilxx Json (`fromNeographJson`) | 1 | 0 (直接构造 utilxx Json) |
| 请求体 `dump()` | 1 | 1 |
| HTTP `req.body()` | 1 | 0 (移动) |
| 合计 | **5** | **3** |

即 [messages-1 §2](../messages-1/work.md) 里"还差 provider 请求体链两步才能到
≤3 次/条/轮"的目标本轮达成。

## 3. 待完成任务

### 3.1 `huge` (≥512 KiB) 分配定位: 已定位 = asio 协程帧 (MSVC 侧超大预留)

**结论**: 每轮的 11 次 ≥512 KiB 分配全部是 **asio awaitable 协程帧**
(`awaitable_frame_base<Executor>::operator new` 分配的协程帧内存), 一次协程调用
一次, 与上下文大小无关 (帧尺寸是编译期常量)。同样的协程在 Linux/GCC 构建上帧
尺寸很小 (同一负载下 ≥512 KiB 的分配为 **0**), 所以这份开销是
**Windows/MSVC 构建特有**的。

定位过程与证据 (Windows release, VS18/MSVC + LTO; 同一台机器、同一 mock 负载):

1. 插桩点选在 `mimalloc/src/page.c:_mi_malloc_generic` (所有大块分配的必经点),
   记录"尺寸 + 各栈帧的 模块+RVA"; 为拿到符号, 用
   `cmake -DCMAKE_EXE_LINKER_FLAGS_RELEASE="/DEBUG:FULL /MAP:..."` 重新链接
   `agentxx_cli` (不改代码), 得到 PDB + MAP, 再用 cdb 的 `ln` / `u` 解析地址。
2. 命中统计 (3 轮 × 8 KB): 共 36 次 = **每轮 11 次** + 启动 3 次 (与 §0.4 一致)。
   尺寸与调用点的对应 (每轮各 1 次; "帧尺寸" = 反汇编里 `mov ecx, imm32` 的立即数,
   即协程帧的编译期尺寸):

   | mimalloc 请求 | 帧尺寸 | 发起协程 (该帧所属的协程) |
   |---|---|---|
   | 2506463 | 2506432 | `BaseAgent::runTurnAsync` 内 lambda_3 的协程 |
   | 2508511 | 2508480 | `agentxx::util::catchErrorAsync<bool, ...>` (runTurnAsync 处) |
   | 2509279 | 2509248 | `utilxx_base::catchErrorAsyncImpl<...>` (runTurnAsync 处) |
   | 2509535 | 2509504 | `BaseAgent::runTurnAsync` 自身 |
   | 2520687 | 2520656 | `SessionServerAgentIO::run` 内 lambda_2 的协程 |
   | 2521615 | 2521584 | `agentxx::util::catchErrorAsync<bool, ...>` (SessionServerAgentIO::run 处) |
   | 2523311 | 2523280 | `utilxx_base::catchErrorAsyncImpl<...>` (SessionServerAgentIO::run 处) |
   | 2523567 | 2523536 | `SessionServerAgentIO::run` 自身 |
   | 823615 | 823584 | `GraphEngine::run_stream_async` 的协程 (由 `AgentRunner::run` 发起) |
   | 825535 | 825504 | `GraphEngine::run_async_with_runtime` 的协程 |
   | 827119 | 827088 | `AgentRunner::run` 自身 |
   | (启动) 2527263 / 2549599 / 2550447 | — | `setupLocalUnifiedDirect` 的三次 `co_spawn` |

   反汇编样例 (`GraphEngine::run_stream_async` 的 resume 函数内):

   ```asm
   call RunConfig::RunConfig
    mov  rbx, rax
    mov  qword ptr [rdi+6F0h], rax
    mov  ecx, 0C9120h                 ; 823584 = 本次协程帧尺寸
    call awaitable_frame_base<...>::operator new
    lea  rax,[GraphEngine::run_async_with_runtime$_DestroyCoro$2]
    lea  rax,[GraphEngine::run_async_with_runtime$_ResumeCoro$1]
   ```

3. **帧是"预留很大、实际几乎没写到"**: 在 `mi_free` 侧插桩扫描被释放块的非零字节
   分布 (临时插桩, 已回退), 典型结果:

   ```
   FREE used=2508470 size=2621440
     nz first=8 segs: 122 0 0 ... 0 12     ; 每项一段 64 KiB, 数字为该段非零字节数
   FREE used=823558 size=851968
     nz first=8 segs: 355 0 0 0 0 6
   ```

   即帧开头几百字节有数据 (promise + 形参 + 局部), 之后整段为零, 帧尾只有 asio
   帧回收器写的 1 字节块计数 (`thread_info_base::allocate` 的 `mem[size] = chunks`)
   —— **2.5 MB 中真正用到的只有几百字节**; 用 PDB 反查协程帧类型
   (`dt -v ...::__coro_frame_type`) 也显示该帧类型只有 ~0x1f4 (500 字节量级),
   与 2.5 MB 的分配量差约 5000 倍。
4. 因此这不是"某个数据结构太大", 而是 **MSVC 给这些协程帧预留了远超所需的
   空间** (帧尺寸是编译期常量, 与运行期数据无关 —— 这也解释了此前"与上下文
   大小/模型配置/SSE 事件数都无关"的观察结果)。
5. Linux 侧对照 (同一负载、同一源码, GCC release): 把插桩阈值降到 16 KiB 后,
   整个运行只有 28 次命中, 最大 87 KiB, **没有** 任何 ≥512 KiB 分配。

**为什么每轮都重新分配** (没有复用 asio 的帧缓存): asio 的协程帧走
`thread_info_base::allocate(awaitable_frame_tag, ...)`, 而**回收只对小块生效** ——
`deallocate` 里判断 `size <= chunk_size * UCHAR_MAX` (x64 上 `chunk_size=4`,
即 **≤ 1020 字节**) 才把块放进线程缓存, 更大的块直接 `aligned_delete`。我们的帧
是 2.5 MB / 824 KB, 全部走"每次重新 malloc"这条路; 每个 tag 的缓存槽位
(`BOOST_ASIO_RECYCLING_ALLOCATOR_CACHE_SIZE`, 默认 2) 与它们无关。
⇒ **调大该缓存不解决问题**, 唯一的办法是让帧本身变小 (编译器侧)。

可选的后续动作 (本轮未实施, 需要时单独立项):

- 让"帧尺寸"变小: 需要继续定位 MSVC 为何给这些协程预留 2.5 MB (最小复现尚未
  构造出来: 同一份代码 GCC 帧很小, 且帧内几乎没被写, 已排除"帧里放了大对象");
  可先按"逐个协程注释掉局部/内联点看帧尺寸变化"的方式二分。
- 若帧尺寸无法减小, 可考虑**减少每轮新建的协程数** (例如把 `catchErrorAsync`
  这类薄包装改为不产生新协程的写法), 但收益只有 11 帧中的一部分, 需要先量化。
- 验收指标 (定位后新增): 用 **mimalloc 退出统计的 `huge` 块数/每轮** 作为本项
  指标 —— 当前 Windows release 为 11 块/轮 (≈22 MB/轮), 目标设为"每轮 ≤ 3 块"
  (启动阶段 3 块不可消除); Linux/Android 上该项本来就为 0, 不设指标。


**定位用的临时改动 (已全部回退, 不入库)**: `mimalloc` 的 `page.c` / `free.c` 插桩
(按环境变量 `AGENTXX_HUGE_LOG` / `AGENTXX_HUGE_MIN` 生效), 以及
`agentxx_client_repo-build` 里临时加的 `/DEBUG:FULL /MAP` 链接参数。


### 3.2 其它已识别、未处理项

- `build_params` 的 1 份 typed 拷贝 (§1.6): 需要会话侧不可变快照才能真正去掉。
- 会话持久化每轮整份 dump: `base_agent.cpp` 轮末 `saveLlmMessages` 会把整份
  `llmMessages` 序列化后写 `llm_context` 表 (方案 §P4 已判定"属持久化语义的
  一部分, 本轮不改")。注意 messages-1 之后它是"typed -> Json -> 落库",
  比改造前多一次 typed->Json; 若要降, 建议在会话侧维护惰性 Json 快照并做
  "脏才写"判断。
- P3 的 content 快速路径 (§1.3 说明为何暂不做)。
- ~~方案 §6 问题 4 的"逐轮长上下文"基准场景仍未补~~ → **本轮已补** (见 §1.4):
  `resource_cli` 增加真实逐轮往返段 (采样点 `rounds<N>x<bytes>`),
  mock LLM 支持把回复切成 N 个 SSE 事件 (`AGENTXX_BENCH_STREAM_CHUNKS`;
  台账 mock 侧对应 `--chunks`), P3 的收益现在可以量出来。

## 4. 注意事项

- `requestSseAsync` 现在是**按值**收请求体, 传右值 (`std::move`) 才省拷贝;
  传左值仍会拷一份, 传 `std::string_view` 变量需要显式 `std::string{sv}`
  (隐式转换是 explicit 的)。
- `chatMessagesToOpenAIJson` / `chatToolsToOpenAIJson` 与 neograph 的
  `messages_to_json` / `tools_to_json` 是**双实现**: 任何一侧改了字段/顺序/回退
  规则, 必须同步另一侧, 否则请求体会静默变化 —— `openai_provider` 测试模块里的
  两个逐字节对照用例是这条约定的守卫。
- 该入口依赖 `agentxx/util/neograph_json_bridge.h` (typed `neograph::json` ->
  `utilxx_base::Json`), 目前桥接头注释里列的合法包含点需要补上
  "provider 请求体组装" (已在本轮更新)。
- 本轮改动过 `agent/third_party/cxx_utilxx` (自研库子模块), 已在其自身仓库提交;
  主仓只更新子模块指针。重新编译前若 cmake 未重新 configure, 记得让
  `cxx_utilxx_repo` 的 build+install 步骤先跑 (lib 与 client/test 都从安装前缀
  取头文件与库)。
- 复测口径: `binned` / `huge` / 分 bin 块数来自 mimalloc 退出统计
  (`MIMALLOC_SHOW_STATS=1`); `huge` 与上下文无关, **不要**把它算进"请求链优化"
  的收益里。

## 5. 提交记录

| 提交 | 内容 |
|---|---|
| `agent/third_party/cxx_utilxx` 6bc7f72 | `perf(http): requestSseAsync 请求体改为按值接收, 支持移动进请求体` |
| 主仓 f59814d6 | `perf(protocol): 请求体消息链去重 + SSE 行解析零拷贝 (memory-1 P1/P3)` (含子模块指针) |

### 测试结论 (2026-09-26, Windows debug, ASan+UBSan)

| 模块 | 结果 |
|---|---|
| openai_provider (含 2 个新增对照用例) | 474 通过 / 0 失败 |
| http / mcp / anthropic_provider | 367 / 391 / 251 全通过 |
| 全量回归 (本机可跑部分) | plugin_runtime 672、plugin_sdk 115、plugin_bridge 193、config_loader 302、tui_* 全通过、sessionId 10015、event_stream 34、event_bridge 109、interrupt_bus 207、subagent_bus 21、subagent_tool 127、agent_host 95、share_store 22、session_persistence 621、rag_search 85、filesystem 128、command 49、web_search 19、codegraph 27、screen_capture 46、cpu_gpu 36、text_selection 13、http 367、network_timeout 49、websocket 230、client_plugins 667、cancel 45、message_supplement 95、summarization 445、checkpoint_store 52、agent 192 —— 全部 0 失败 |
| memgrowth | **未完成**: 与 messages-1 相同, 进程被输入法 DLL (`SogouPY.ime` / `SogouTSF.ime` / `MSCTF.dll`) 触发的 AddressSanitizer heap-use-after-free 打断 (环境问题, 调用栈全在输入法 DLL 内) |
