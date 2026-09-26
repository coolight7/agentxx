# 实施记录: 降低每轮上下文拷贝 (请求体 JSON / SSE 解析 / 图状态消息数组)

- 方案: [plan.md](plan.md)
- 类型: 性能与内存优化 (不改协议/checkpoint 语义)
- 时间: 2026-09-26 (承接 [messages-1](../messages-1/work.md) 的上下文迁出改造)
- 状态: 计划中仍适用的项 (P1-1 / P1-2 / P3) 已实施、编译与测试通过、并完成
  release 台账复测; P0 / P2 经逐项复核**已由 messages-1 达成或失效** (见 §1.4);
  P1-3 只达成一半 (见 §1.3); `huge` 定位有明确进展但**未定位到调用点** (见 §3.1)

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

### 1.4 P0 / P2 复核: 已由 messages-1 达成或失效

| 计划项 | 复核结论 |
|---|---|
| P0-1 去掉 `StreamMode::VALUES` | **已完成** (messages-1 阶段 0); 现在 `base_agent.cpp:1027` 只传 `EVENTS \| TOKENS \| UPDATES` |
| P0-2 checkpoint 每步整段序列化指纹化 | **不再需要**: 默认图只有 `channel_savedGraphData` 与 `xx_messagesMeta` 两个通道 (`base_agent.cpp:486-498`, reducer 均为 overwrite, 注释明确"上下文本身不在图状态里"), `state.serialize()` / checkpoint 载荷与上下文大小无关 |
| P0-3 运行结束双拷贝合并 | **已完成** (messages-1 阶段 0, 删除 `channel_raw + fromNeographJson` 往返) |
| P2-1 图状态零拷贝读取 API | **失效**: `messages` 通道已不存在, 没有可零拷贝读取的对象 |
| P2-2 `reducer_append` 就地追加 | **失效**: 同上 (reducer append 不再承载消息) |
| P2-3 调用点收敛 (`state.get_messages()` 10+ 次/轮) | **失效**: 调用点已随 messages-1 改为会话 typed 读取 |

### 1.5 P1-3 复核: 达成一半

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
| `build_params` 取会话上下文 (`messages.assign`) | 1 | 1 (§1.5, 未改) |
| provider: typed -> neograph json (`messages_to_json`) | 1 | 0 |
| provider: neograph json -> utilxx Json (`fromNeographJson`) | 1 | 0 (直接构造 utilxx Json) |
| 请求体 `dump()` | 1 | 1 |
| HTTP `req.body()` | 1 | 0 (移动) |
| 合计 | **5** | **3** |

即 [messages-1 §2](../messages-1/work.md) 里"还差 provider 请求体链两步才能到
≤3 次/条/轮"的目标本轮达成。

## 3. 待完成任务

### 3.1 `huge` (≥512 KiB) 分配定位: 有进展, 仍未定位到调用点

本轮用临时插桩的 mimalloc (`mi_huge_page_alloc` 里打印请求大小 + 调用栈所属模块)
测出的确定结论:

- 每轮固定 **11 次**: 8 次 ~2.5 MiB + 3 次 ~824 KiB; 另有 3 次 2.5 MiB 在**启动**阶段。
- **与上下文完全无关**: 50 轮 × 100 B 与 50 轮 × 8 KB 的尺寸集合**逐位相同**
  (2506463 / 2508511 / 2509279 / 2509535 / 2520687 / 2521615 / 2523311 /
  2523567 / 2527263 / 2549599 / 2550447 与 823615 / 825535 / 827119)。
- 与**会话持久化无关**: 把 `data_dir` 指到不存在的盘符 (持久化全部失败) 后,
  每轮次数与尺寸集合完全不变。
- 尺寸全部 ≡ 15 (mod 16) ⇒ 请求量 = `16k - 1`, 是 MSVC `std::string`
  "为容量申请 `capacity + 1` 字节"的形态; 出现时机在**轮次开始 (`agent_start`
  之前)**, 与 LLM 请求/SSE 无关。
- 这些尺寸在可执行文件里**不是编译期常量** (按 4 字节小端搜索 14 个尺寸的
  立即数, 命中 0 次) ⇒ 不是协程帧 (帧大小必然编译期确定), 只能是运行期算出的
  数据长度。
- 归因尝试与排除: 会话 SQLite / `model_context_max_token` / SSE 事件数 /
  plugins 目录 DLL 文件大小 / `LogPrint` 日志缓冲 / agentxx 运行期 regex
  (仅 `modelocall.cpp` 的静态 `AhoCorasick`, 构造一次) 均对不上。

未定位的原因与下一步建议:

- release 构建**不产 PDB**, `/MAP` 生成的符号表在 LTO/ICF 下与真实函数边界不符
  (按它解析出来的"分配点"落在 `std::string` 代码里, 与 `huge` 分配点无关),
  所以拿不到可信调用栈;
- 本轮试过在 **debug (ASan) 构建 + cdb** 上做条件断点: 该 exe 的 `malloc` 确实来自
  `clang_rt.asan_dynamic-x86_64.dll` (可断), 但 (a) 该 DLL 加载后的模块名是
  `clang_rt_asan_dynamic_x86_64`, 未加模块限定的 `bp malloc` 解析不了
  (`Bp expression 'malloc ' could not be resolved`); (b) ASan 在调试器下会先抛
  自己的 first-chance 异常 (Unknown exception `e0736170` + access violation),
  `-o` 批处理脚本会在这里结束, 拿不到后续断点输出 —— 该路线不可行, 需要换 release;
- 下一步建议: **release 额外带符号** (`/DEBUG` 重链 `agentxx_cli` 以及被动态链接的
  `libagentxx.dll`, 生成 PDB) + cdb 条件断点
  (`bp mimalloc!mi_malloc ".if (@rcx > 0x80000) { kb 24; g } .else { gc }"`,
  注意 plan §6 问题 3 记录的 cdb 坑: bp 命令串里 `||`/`&&` 会被当命令分隔符),
  或在 Linux 侧用 heaptrack / valgrind-massif 直接拿调用栈 (那边有完整符号)。

### 3.2 其它已识别、未处理项

- `build_params` 的 1 份 typed 拷贝 (§1.5): 需要会话侧不可变快照才能真正去掉。
- 会话持久化每轮整份 dump: `base_agent.cpp` 轮末 `saveLlmMessages` 会把整份
  `llmMessages` 序列化后写 `llm_context` 表 (方案 §P4 已判定"属持久化语义的
  一部分, 本轮不改")。注意 messages-1 之后它是"typed -> Json -> 落库",
  比改造前多一次 typed->Json; 若要降, 建议在会话侧维护惰性 Json 快照并做
  "脏才写"判断。
- P3 的 content 快速路径 (§1.3 说明为何暂不做)。
- 方案 §6 问题 4 的"逐轮长上下文"基准场景仍未补: 现有台账 mock 每响应只发
  4 个 SSE 事件, P3 的收益量不出来。

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
