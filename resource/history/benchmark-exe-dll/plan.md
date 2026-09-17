# benchmark 程序启动 / 大上下文资源占用测量 —— 设计实现方案

- 状态: 设计定稿 (待实施)
- 需求来源: `benchmark` 仿照 `test` 支持模块化; 新增 `agentxx_cli` 加载 5 常用插件时的资源占用测量, 以及 `libagentxx_shared` 动态库同条件对照
- 关联代码:
  - `agent/benchmark/benchmark_main.cpp`、`bench_*.h`、`bench_util.h`、`CMakeLists.txt`
  - `agent/test/test.cpp`、`CMakeLists.txt`、`core/test_memgrowth.*`
  - `agent/client/main.cpp`、`src/mode_runners.cpp`、`src/util/util.cpp`
  - `agent/lib/include/agentxx/agent/{config,context,conversation_types}.h`
  - `agent/lib/include/agentxx/agent/io/{agent_io,agent_io_transport,session_server_agent_io,agent_server}.h`
  - `agent/lib/include/agentxx/middlewares/summarization.h`
  - `agent/lib/include/agentxx/ffi_api.h`、`lib/ffi_symbols.map`、`src/ffi/ffi_runtime.cpp`
  - 5 插件 `agent/plugins/{agentxx_filesystem,agentxx_execute_command,agentxx_system,agentxx_websearch,agentxx_planning}/`

---

## 1. 需求拆解

### 1.1 功能需求

1. **benchmark 模块化 (仿 test)**:
   - 现在 `benchmark_main.cpp` 无条件全量跑 `benchStringUtil/AhoCorasick/Regex/Router + CodeAgent*6`; 要求仿 `test.cpp` 的 `shouldRun(name)` 按 `argv` 模块名过滤, 支持 `--list/--help/--fail-fast`, 无参数=全量 (保持 CI 兼容)。
2. **新增 `bench_resource` 模块**, 测量以下矩阵 (行=模式, 列=时机), 指标=内存占用 + CPU 占用:
   - 插件集合固定 5 个: `agentxx_filesystem`、`agentxx_execute_command`、`agentxx_system`、`agentxx_websearch`、`agentxx_planning`。
   - 模式:
     - M1 同一进程 CLI (`runLocalCliUnified` 等价链路: `CodeAgent + SessionServerAgentIO + StdIOClientAgentIO`, Channel 直连)。
     - M2 同一进程 TUI (`runLocalTuiUnified` 等价链路: `CodeAgent + SessionServerAgentIO + TUIClientAgentIO`, Channel 直连, 尾窗 100)。
     - M3 拆分两进程 CLI+Server (WS): 分别报告 `server` 进程与 `cli` 进程占用。
     - M4 拆分两进程 TUI+Server (WS): 分别报告 `server` 进程与 `tui` 进程占用。
   - 时机 (每模式 3 个采样点):
     - P0 程序启动时 (agent `init` + 插件加载完成 + 首个 `hello/sync` 完成, 零历史)。
     - P1 约 100K token 上下文。
     - P2 约 200K token 上下文。
   - M3/M4 的 server/client 各自在 P0/P1/P2 采样 (共 4 个进程角色 × 3 时机, 其中 server 侧 P1/P2 为同一 server 会话增长后的稳态)。
3. **LLM 消息由程序生成, 不用真实 API**:
   - user / assistant / tool 交替出现, 内容固定 (确定性模板, 跨 exe/dll 一致)。
4. **`libagentxx_shared` 对照组**:
   - 加载同样 5 插件, 同样 3 时机 (加载动态库后 / 100K / 200K, 上下文内容一致), 报告内存 + CPU。

### 1.2 非功能约束

- 跨平台: Linux / Windows (macOS/Android 尽力, 以 `#if` 降级; CPU 采样精度以 Linux/Win 为准)。
- 不破坏现有 bench 输出契约: 控制台人类可读 + `BenchReporter::flushToFile()` JSON (扩展资源结果段, 老字段不动)。
- 不启动真实 TUI 全屏 / 不阻塞 stdin / 不依赖真实 LLM / 不写用户 `~/.agentxx` (bench 用临时 `dataDir`, 测后清理)。
- 插件缺失不静默跳过: 路径探测失败必须记错并标记该行 `pluginsLoaded<5`, 避免"测了个空载"被误读为优化。
- 与 `AGENTS.md` 一致: 注释用 `///`, 不动 `third_party`, 不加 `/FS /MP`, 保持 `Channel` 线程模型不断言 (`Session::assertIoThread` 仅 io 线程写)。

---

## 2. 现状勘察结论 (设计输入)

| 项目 | 现状 | 对设计的含义 |
|---|---|---|
| benchmark 结构 | `bench_*.h` 全 `inline void benchX()` + `benchmark_main.cpp` 顺序调用; 无模块过滤; `BenchReporter` 只记 `BenchResult{ns统计}` | 需新增注册表 + `shouldRun`; 资源型结果 (MB/%) 不适用 `runBench(ns)` → 新增 `ResourceResult` 并复用 reporter 落盘 |
| benchmark 链接 | 只 `find_package(agentxx_static)`, 无 client/ftxui; 顶层已透传 `-DAGENTXX_BUILD_CLIENT` 给 benchmark 子构建 (但 `benchmark/CMakeLists.txt` 未消费) | `bench_resource` 需 TUI/CLI 端点 → benchmark 必须仿 test 消费 `AGENTXX_BUILD_CLIENT`, 条件编译 client 源 + `ftxui/markdown-ui` |
| test 模块化 | `test.cpp`: `selectedModules + shouldRun + runSync/run/runCtx` 三 helper, `--fail-fast/-f`, `AGENTXX_BUILD_CLIENT` 条件注册 | 直接照抄该三段式; bench 为同步 + 协程混合, 需 `runBenchSync/runBenchAsync` 两个 helper |
| 5 插件形态 | 当前 `builtin` 清单为空 (release/debug 构建产物 `builtin_plugins.cpp` 均为 `#if 0`), 插件以 `exec/plugins/<name>/lib<name>.so + plugin.yaml` 目录形态分发 | bench 经**目录路径**加载 (与 `agentxx-config.yaml` 线上形态一致), 不用 `builtin://`; 目录缺失时回退 `builtin://<name>` 再失败才报错 |
| 插件 sides | `filesystem/execute_command/websearch/planning` 有 `agentxx_plugin_client_create` (双端); `system` 无 client 入口 (纯 agent) | agent 侧 5 个全加载; client 侧期望加载 4 个, `system` 跳过属正常, 计数需分 `agentLoaded/clientLoaded` 两列, 不得混为一谈 |
| 同进程链路 | `setupLocalUnifiedDirect`: Channel 对 + `SessionServerAgentIO::Config{initialSyncTailCount: TUI=100/CLI=0}` + 先起 transport 循环再 `init→host→onServerReady→run` | bench 复用**同一装配函数语义** (建议将 `mode_runners.cpp` 的 local 装配抽为 bench 可复用的小头, 或在 bench 内镜像实现并注释引用出处), 否则"测的不是线上链路" |
| 拆分链路 | `AgentServer::serveTransport` + `WsAgentIOTransport` (client 模式自带重连/心跳/握手) | bench 拆分模式**必须真起两个进程** ( fork/exec `agentxx_cli server` + `cli/tui --agent`), 在父进程按 PID 采样, 否则 WS 序列化/双堆开销量不到 |
| TUI 线程模型 | `TUIClientAgentIO::start()` 起 `Fullscreen + Loop` UI 线程; `onDelta/onSync` 在 client 线程 `mutate()` 写 `TUISharedState`, UI 线程 `readSnapshot()` 读 | bench **不得调 `start()`** (无终端/会抢屏); 只构造端点 + `runTransportLoop`, 用 `sharedState().readSnapshot()` 量 client 堆, 这是 TUI 内存的主体; 需在文档明确"不含 FTXUI 帧缓存/字形缓存", 属可接受低估并在报告注明 |
| CLI 输入 | `StdIOClientAgentIO::getInput()` 经 `StdinReader` 单例阻塞读 stdin | bench 不进输入循环; 只起 transport 循环; `sendPluginUserInput/sendToPeer` 由 bench 驱动 |
| FFI | `ffi_api.h` 为唯一稳定 C ABI; `FfiAgentRuntime::buildConfigs` 支持 `plugins/sides/args/dataDir/permissionMode/workDir/modelContextMaxToken`; `ffi_symbols.map` 仅导出 `agentxx_ffi_* + agentxx_plugin_get_builtin_plugins` | bench 经 `dlopen(libagentxx.so)+dlsym` 走公开 C API (与外部语言宿主同路径), 不得直链内部 C++ 符号; 上下文经"同模板 mock LLM 驱动 turns"生成以保证内容一致 |
| token 口径 | 唯一口径 `SummarizationMiddlewareHandle::countTokens({}, msgs, false)` (`ascii/4 + unicode/1.5 + 每消息+3`, 见 `summarization.cpp:119`) | 所有"100K/200K"均指该口径; bench 构造固定模板后**实测校准条数**使误差 ±2%, 并报告实际值; `modelContenxtMaxToken` 设超大 (如 8M) 使测量期**不触发压缩/截断** |
| 内存现状 | `test_memgrowth.cpp`: `getrusage(RUSAGE_SELF)` 取 `ru_maxrss` (峰值, 非瞬时) + `dump()` 估容器字节 | 资源 bench 需**瞬时 RSS** (Linux `/proc/self/statm` 或 `/proc/self/status VmRSS`; Win `GetProcessMemoryInfo(WorkingSet+PrivateUsage)`), 子进程按 `/proc/<pid>/` 同法; 保留 `dump()` 容器拆解作归因列 |
| CPU 现状 | 无进程 CPU 工具; `system_monitor` 插件是整机 CPU, 不可用 | 新增 bench 内采样器: 双点进程时间差 / 墙钟差 (Linux `/proc/<pid>/stat utime+stime`; Win `GetProcessTimes`), idle 500ms 窗口 + 构造/Sync 忙时窗口各一值 |

---

## 3. 总体方案 (一句话)

> **benchmark 内新增 `bench_resource` 模块: 以"与线上同装配"的 in-process 等价链路测 M1/M2, 以"真双进程 `agentxx_cli server + client`"测 M3/M4 (按 PID 分侧采样), 以 `dlopen(libagentxx.so)` + 同模板 mock LLM 测 FFI 对照; 上下文不用真实 LLM, 而用"固定 user/assist/tool 交替模板"经真实 `Session` 写入路径 (view + llm 双集) 构造到 token 校准点, 在 P0/P1/P2 采瞬时 RSS + 进程 CPU% + 容器拆解; 主函数仿 test 做模块过滤。**

### 3.1 为什么不用"真跑多轮 runTurnAsync + mock LLM"来涨上下文

- 真跑 N 轮 ReAct 虽最真实, 但 100K/200K 需数百轮 × 每次图调度/中间件/事件桥, 单次 bench 数分钟且受 mock 分片抖动影响 token 精确度。
- 直接经 `Session::appendViewMessage + llmMessages.push_back` (均在 agent io 线程, 与 `EventBridge::handleChannelWrite` 同语义) 写入, 一次成型、token 精确、可复现, 且 llm JSON 形态与 provider `messages_to_json` 一致, token 口径同源。
- 但**启动 P0 仍走一次真实 `init + hello/sync + 1 轮 mock turn`** (含插件工具注册、图编译、首轮事件桥), 保证插件/引擎已热, P0 不是"冷静态链接体积"而是"可服务稳态基线"。P1/P2 在此会话上追加固定模板 (append-only, 与线上长会话一致)。

### 3.2 为什么 TUI 用 headless 等价而不起全屏

- `start()` 需要 tty + 切备用屏 + 鼠标捕获, CI/无头机必失败, 且帧循环常驻 CPU 会污染被测 CPU。
- TUI 进程内存主体 = `TUISharedState(messages/currentToken/pluginRegistry)` + client 插件 UI 注册表 + transport 缓冲, 均在 headless 下完整保留; 帧 Element 树/字形缓存为渲染派生, 与上下文 token 无关, 忽略后在报告 `note` 列明, 不影响 100K→200K **增量归因** (本需求核心)。

### 3.3 为什么拆分模式必须真双进程

- M3/M4 的价值正是 WS 序列化、心跳、双堆、双插件运行时; 单进程内用两个 `io_context` 模拟量不到 `VmRSS` 分侧。
- 父 bench 进程只做编排 + 采样, 被测 server/client 为真实 `agentxx_cli` 二进制 (与用户运行同一文件), 配置由 bench 生成临时 yaml (5 插件目录路径 + mock LLM endpoint + 临时 dataDir + `permission.mode=pass` 避免 HIL 阻塞)。

---

## 4. benchmark 模块化改造

### 4.1 CLI 契约 (仿 test, 保持兼容)

```text
agentxx_benchmark [module ...] [--list] [--fail-fast|-f] [-h|--help]
无参数              = 全量 (现状行为, CI 不改即兼容)
module              = string_util aho_corasick regex router code_agent_init code_agent_turn resource ...
--list              = 列出全部模块名 + 一行描述后退出
--fail-fast | -f    = 任一模块抛异常/返回 false 即 _Exit(1)
未知模块名          = 报错 + 非零退出 (避免拼写错误被误认为"全过")
```

模块粒度的建议划分 (每个 `bench_*.h` 内函数保持不动, 只在 main 做门控):

```text
string_util | aho_corasick | regex | router |
code_agent_init | code_agent_init_warm | code_agent_turn |
code_agent_simple | code_agent_multi | code_agent_large_history |
resource_cli | resource_tui | resource_split_cli | resource_split_tui | resource_ffi
(resource 下分子模块, 支持只跑单模式; `resource` = 全跑)
```

### 4.2 main 改造要点

- 照抄 `test.cpp`: `selectedModules + shouldRun(name)` lambda; `runBenchSync(name, fn)` 打印 `--- name --- / done` 包裹; 异步资源 bench 用 `runBenchAsync(name, coroFn)` 在独立 `asio::io_context` 上 `co_spawn + run` (不得复用全局 io, 与 `memgrowth` 的 work_guard 教训一致)。
- `BenchReporter` 新增资源结果容器 (见 §8), `flushToFile()` 一起落盘; 控制台另打人类可读大表 (MB/%, 非 ns)。
- `--list` 输出与 `BENCH_MODULES` 注册表同源 (单点维护, 防文档漂移)。

### 4.3 CMake 改造

- `agent/benchmark/CMakeLists.txt`:
  - 新增 `option(AGENTXX_BUILD_CLIENT ...)` 透传消费 (顶层已传 `-DAGENTXX_BUILD_CLIENT`, bench 侧此前未用): `if(AGENTXX_BUILD_CLIENT)` 则 `file(GLOB_RECURSE client_SOURCES ../client/src/*.cpp)` + `target_include_directories(../client/include)` + `find_package(ftxui/markdown-ui)` + `target_link_libraries(ftxui::ftxui markdown::markdown-ui)` + `target_compile_definitions(AGENTXX_BUILD_CLIENT=1)`。关闭时 `bench_resource` 的 TUI/CLI 分支编译为"未启用"桩并在运行时提示跳过 (Android 等无 client 构建仍可跑纯 agent/FFI 子集)。
  - 新增 `bench_resource.h/.cpp` (提交时由 `.h` 内联改为 `.h+.cpp` 分离, 避免每个 TU 重复编译重型 TUI 头; 其他 `bench_*.h` 保持不动以最小 diff)。
  - 测试/benchmark 共用的采样小函数 (`procRSS`, `procCPU`, `findPluginDir`) 放 `bench_resource_util.h` (bench 私有, 不进 lib)。
- `agent/CMakeLists.txt`: 已有 benchmark `ExternalProject_Add` 透传 `AGENTXX_BUILD_CLIENT` + `add_dependencies(benchmark→client)` (若缺则补, 以本次改动为准, 保证 bench 先于 client 产物就绪; 插件目录 `exec/plugins/<name>` 由插件构建产出, bench 运行时探测, 构建期不硬依赖)。
- 不新增三方依赖; 进程 spawn 用 `boost::process` (项目已用, `AGENTXX_ENABLE_BOOST_PROCESS` 门控; 关闭时 split 子模块运行时跳过并说明)。

---

## 5. `bench_resource` 详细设计

### 5.1 目录与文件

```text
agent/benchmark/
  benchmark_main.cpp      # 改造: 模块注册表 + shouldRun + --list/--fail-fast
  bench_resource.h        # 对外入口: void benchResource*() 薄包装 (供 main 注册)
  bench_resource.cpp      # 实现主体 (≈1200行内): 模板/校准/采样/四模式+FFI/报告
  bench_resource_util.h   # 进程 RSS/CPU 采样 + 插件目录探测 + token 口径小封装
  bench_util.h            # 扩展: ResourceResult + reporter 资源段 (向后兼容)
```

### 5.2 固定消息模板 (user/assist/tool 交替, 内容固定)

设计目标: 跨 M1~M4 + FFI **字节一致**, token 可精确预估, 且能真实走 tool 结果回填路径 (含 `tool_call_id` 关联、`updateViewMessage`、`appendSettledLlmMessages`)。

- 单组 = 3 条 llm 消息 + 对应 3~4 条 view 消息:

```jsonc
// llmMessages (provider 原语, 与 OpenAIProvider.buildBody 一致)
{"role":"user","content":"RES-BENCH user turn {i:06d} | The quick brown fox jumps over the lazy dog. 请列出当前目录并读取 README 前 40 行。 #FIXED-9f3a"},
{"role":"assistant","content":"","tool_calls":[{"id":"call-{i:06d}","name":"agentxx_filesystem_read","arguments":"{\"path\":\"README.md\",\"line_offset\":0,\"line_limit\":40}"}]},
{"role":"tool","tool_call_id":"call-{i:06d}","tool_name":"agentxx_filesystem_read","content":"RES-BENCH tool result {i:06d} | line01: agentxx resource benchmark fixed payload ... (固定 512B 可打印 ASCII, 含中文混合行'资源占用固定载荷'以覆盖 unicode 折算分支)"}
```

```text
// viewMessages (与 EventBridge.handleChannelWrite 展开语义一致)
User("RES-BENCH user turn ...") / Tool{toolName=agentxx_filesystem_read, toolCallId, arguments, toolResult, toolFinished=true, collapsed=true} / Assistant("RES-BENCH assist summary {i} | fixed 256B ...")
注: assistant 文本单独一条 view (content 非空才展开), tool 结果回填到 Tool 条目 (updateViewMessage), 与线上渲染拆解同序 (Think缺省/cllapsed=true)。
```

- 固定手段: 序号仅作组标识 (`{i:06d}` 不影响单组字节数, 变长补零定宽); 其余全部字面量常量; tool 固定选 `agentxx_filesystem_read` (5 插件必有, 参数 schema 合法, 不真实执行文件 IO —— bench 走"直接写会话"路径, 不调工具执行体, 故无副作用)。
- 交替性: 组内天然 `user→assistant(tool_calls)→tool`; 组间上一组 tool 后紧跟下一组 user, 全序列无两条同 role 相邻 (除首尾 system 外), 满足"交替出现"字面要求, 且 `splitRecentByTokenBudget` 的 tool/assistant 对齐分支能被覆盖 (压缩虽关闭, 但对齐代码仍可在单测中断言)。
- system prompt: 固定短串 `"You are a helpful assistant. RES-BENCH fixed system v1."` (计入 token, 跨组一致)。

### 5.3 token 校准 (100K / 200K)

- 口径函数: 构造一个常驻 `SummarizationMiddlewareHandle` (或复用 agent 内已注册 handle) 调 `countTokens({}, llmMsgs, false)`; 该值即线上 `contextStats.contextTokens` 的估算源 (API usage 为 0 时)。
- 流程:
  1. 先实测单组 token `g` (含 `extraTokensPerMessage` 开销, 约 400~600)。
  2. `n100 = round(100000/g)`, `n200 = round(200000/g)`; 生成后实测总量, 若超出 ±2% 则 ±1 组微调 (groups 定宽故最多调一次)。
  3. 记录 `actualTokensP1/P2` 进报告 (不强求恰好 100000, 要求**各模式同 n** → 内容字节一致; FFI 侧用同样 `n100/n200` 生成同样模板)。
- `modelContenxtMaxToken = 8<<20` (8M): 200K 仅占 2.4%, 远离 75% 压缩线与 95% 截断线; 同时 `enableSummarization` 保持 true (线上默认) 以保留其内存基线, 仅因阈值够高而不触发 (需在报告注明)。
- Sys 提示: bench 明确"本测量为估算 token (ascii/4, unicode/1.5), 非 tokenizer 精确值; 跨模式可比, 跨模型不可比"。

### 5.4 进程资源采样器 (`bench_resource_util.h`)

```cpp
struct ProcSample { double rssMB=0; double privateMB=0; double cpuPct=-1; uint64_t vmsizeKB=0; };
ProcSample sampleSelf();            // 自身进程瞬时采样
ProcSample samplePid(uint32_t pid); // 任意 pid (split 子进程; 不存在返回 rss=0 + cpu=-1)
struct CpuWindow { /* 双点差分所需快照 */ };
CpuWindow cpuBegin(pid); double cpuEnd(window, wallSec); // 进程时间差/墙钟差*100
```

- Linux: RSS 取 `/proc/<pid>/statm resident*pageSize` (瞬时, 非 `ru_maxrss` 峰值); 私有近似 `VmRSS - RssFile(若可得)` 否则 `=RSS`; CPU 取 `/proc/<pid>/stat utime+stime (USER_HZ)` 双点差分。采样失败 (进程已退) 返回哨兵, 调用方标记。
- Windows: `GetProcessMemoryInfo(WORKING_SET+PRIVATE_USAGE)`; CPU 取 `GetProcessTimes(kernel+user)` 双点差分; 自身句柄 `GetCurrentProcess`, 他进程 `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` 按 pid。
- 采样窗口:
  - `idle CPU`: 被测就绪后静置 500ms (排空 init 毛刺) 再采 500ms 窗口。
  - `busy CPU`: 包住"P2 构造 + 全量 Sync + 一次历史分页拉取"的窗口 (TUI 尾窗/CLI 全量差异在此体现)。
  - 内存取窗口末瞬时值; 为抑抖动, P1/P2 各采 3 次取中位数 (bench 内实现, 报告同时给 `rss_med/min/max`)。
- 容器拆解 (归因列, 与 `memgrowth` 同法):
  - `viewBytes = Σ viewMessage.toJson().dump().size()`, `llmBytes = llmMessages.dump().size()`, `viewCount/llmCount`, `deltaBuffer`, `shareStoreItems`, TUI 侧 `sharedState messages/currentToken/pluginRegistry size` (快照拷贝计数, 不 dump 全量以免 bench 自身放大内存)。

### 5.5 插件装配 (5 插件, 与线上同形)

```cpp
std::vector<agent::PluginConfig> kBenchPlugins5(); // filesystem, execute_command, system, websearch, planning
std::string resolveBenchPluginDir(name);           // exe同目录/plugins/<name> → cwd/plugins/<name> → builtin://<name>
```

- 探测顺序与 `test_plugins/findPluginDir` 一致 (exe 同目录优先, 校验目录内含 `.so/.dll/.dylib`, 源码 `agent/plugins/<name>` 绝不计入)。
- `AgentConfig`: `permissionMode=Pass` (bench 无人应答 HIL, 避免卡死; 与线上 ask 的权限内存差可忽略, 报告注明), `enableSessionStore=false + dataDir=临时空目录` (不污染用户数据; planning 插件 `plansDir` 为空即内存态, 符合"内容固定"要求), `enableSubagent=false/enableWorktree=false` (排除子代理/host 噪声, 聚焦 5 插件; 报告注明), `model.baseUrl=<mock>/apiKey=EMPTY/modelName=bench-sim`, `modelContenxtMaxToken=8M`。
- client 插件: `ClientPluginManager + CliPluginAdapter/ TuiPluginAdapter` + `loadConfiguredClientPlugins(same 5)` (sides=auto; `system` 无 client 入口被跳过为**预期**, 分开计数 `agentLoaded=5/clientLoaded=4`)。
- P0 定义 (所有模式统一): `init()` 完成 + `hello→(helloAck+sync)` 完成 + 首轮 mock turn 成功 + idle 500ms 排空后。此时 `pluginsLoaded` 已定, `view/llm` 仅首轮痕量, 即"启动时"基线。

### 5.6 M1 同进程 CLI (in-process 等价)

```text
clientEx(io_context) + agent(CodeAgent)->ioCtx 两线程 (与 runLocalUnifiedMain 同拓扑)
StdIOClientAgentIO(不进stdin循环) + ClientPluginManager/CliPluginAdapter
setupLocalUnifiedDirect语义: Channel对 + SessionServerAgentIO{tail=0} + transport双循环 + hello
→ P0采样(self RSS/CPU + 容器)
→ agent io线程追加 n100组 (直接写会话, 见§5.2) + 全量Sync拉取 → P1采样
→ 追加至 n200组 + 全量Sync → P2采样 (+busy CPU窗口包住P2构造+Sync)
→ serverIO.stop/run退出等待 → PluginManager.shutdownAll → join → 临时目录清理
```

- 所有 `Session` 写操作 `asio::post` 到 agent io 线程 (满足 `assertIoThread`); 采样读 (`viewMessageCount/dump`) 同样投递取回 (或经 `WireGetContext/Sync` 拿, 与线上同路径; 推荐后者以顺带测量 Sync 开销)。
- CLI 的 Sync 为全量 (`tail=0`), P2 的 Sync 帧即 200K 全量 JSON, 其 CPU/内存峰值是 CLI 与 TUI 的关键差异点之一。

### 5.7 M2 同进程 TUI (headless 等价, 不 start())

与 M1 仅三处不同, 其余全同 (保证增量可比):

1. 端点换 `TUIClientAgentIO(clientEx, sessionId, darkTheme, Pass)` (不调 `start()`, 故无 UI 线程/无 Fullscreen); 仍 `setPluginManager + setEventSink + loadConfiguredClientPlugins(TuiPluginAdapter)`。
2. `SessionServerAgentIO::Config{initialSyncTailCount=100}` (与 `kTuiInitialSyncTailCount` 同值); P1/P2 的 Sync 仅末尾 100 条, 随后 bench 主动发一次 `WireGetViewMessages{beforeIndex=windowStart, count=100}` 分页拉取以测量分页路径 CPU (TUI 线上行为)。
3. client 内存读 `tui.sharedState().readSnapshot()` (`messages.size + currentToken + pluginRegistry`), 与 server 侧 `Session` 计数并列报告 (TUI 双份历史是其内存高于 CLI 的主因, 必须拆开)。

### 5.8 M3/M4 拆分双进程 (真 `agentxx_cli` 二进制)

编排 (父=bench 进程, 子=两个 `agentxx_cli`):

```text
1. 生成临时目录 bench_split_<pid>/ {server.yaml, client.yaml(备用), data_server/, data_client/}
   server.yaml: {data_dir: data_server, permission.mode: pass, models:[{name: bench-sim, base_url: mock, api_key: EMPTY}],
                 use_model.default: bench-sim, plugins: [5目录路径...] (+ codegraph? 不加, 严格5个)}
   mock LLM: bench 父进程起 LlmSimServer (与 bench_code_agent.h 同类, 端口 OS 分配), server.yaml 指向它
      注: split 的 P1/P2 上下文仍用"直接写会话"法经 WS 触发? WS 无直接写会话通道,
      故 split 侧改用"mock LLM 驱动真实 turns + 固定模板回放"涨上下文:
      mock 按组模板循环返回 (user回显→tool_calls(filesystem_read固定参)→tool结果固定512B→assist摘要),
      bench 经 client 发 n 组 user 输入 (内容即模板 user 行), server 自然落盘 view+llm。
      为控制时间, split 可用较小单组但同模板 (token 用同口径折算到≈100K/200K, 允许±5%, 报告实际值;
      与 in-process 的字节一致性降为"模板一致、组数折算", 在报告 explicitly 标注)。
      若时间允许, 更优是 server 侧加 bench 专用"直注"通道? 不建议 (污染线上协议); 接受折算误差。
2. spawn server: agentxx_cli server --config server.yaml --host 127.0.0.1 --port 0? (若cli不支持port0则Bench侧先占空闲端口再传入) --token <rand>
   等待 /health 或 WS helloAck ok (超时 20s 判失败)。
3. spawn client: cli → agentxx_cli cli --config client.yaml(仅client插件+data_client) --agent ws://127.0.0.1:port/agent --token ... ;
   tui → 同形但 mode tui; tui 子进程需伪 tty? 无头下 ftxui Fullscreen 会失败 → split-TUI 测法降级为
   "server 真进程 + client 用 bench 内 headless-TUI 经 WS 连 server"(client 侧仍是 WS 真链路, server 侧为真进程)。
   该降级必须在报告注明 (M4-client ≈ WS client 真实堆, 不含帧缓存)。
   M3-cli 子进程为真全量 (stdio 可重定向到 pipe, 无需 tty)。
4. P0: 双子进程就绪 + hello/sync 完成 + idle 500ms → samplePid(serverPid) + samplePid(clientPid)。
5. P1/P2: 按步骤3驱动上下文增长 (经 client 发 user 模板; 等待 TurnEnd 计数), 每次达标后双采。
6. 清理: 先关 client (close stdin/EOF 或 SIGTERM), 再 server (SIGTERM→SIGKILL兜底), join, 删临时目录, mock stop。
```

- 端口/ token/sessionId: 每次 bench 随机, 防多实例串扰 (仿 `generateUniqueSessionId`)。
- 采样稳健性: 子进程启动抖动大, P0 前静置 1s; 每次采样 3 次中位数; 子进程提前退出即标记失败而非填 0。
- `boost::process` 不可用 (关闭) 时 split 子模块运行时跳过 (`SKIP + 原因`), 不影响 in-process/FFI。

### 5.9 FFI `libagentxx_shared` 对照组

- 加载: `dlopen(<exe同目录>/libagentxx.so (Linux) / agentxx.dll (Win), RTLD_NOW|LOCAL)`; `dlsym` 全部 `ffi_symbols.map` 白名单符号 (缺任一即失败, 防测到桩)。
- 配置 (与 M1/M2 同值, 经 JSON):
  - `config_json`: `{dataDir: 临时空, permissionMode: pass, enableSessionStore: false, llmMaxRetry: 0(省重试时间), plugins: [5目录...]}`。
  - `model_json`: `{name: bench-sim, type: openai, baseUrl: mock, apiKey: EMPTY, modelName: bench-sim, modelContextMaxToken: 8M}`。
- 上下文: 复用同一 mock + 同模板, 经 `send_input(模板user)` 驱动 `n100/n200` 轮 (首轮后 `EVT_READY` 才发; 用 `event_queue` 阻塞等 `TURN_END`; HIL 不应出现, 若出现即自动 `interrupt_respond(["true"])` 并计数, permission=pass 下正常为 0)。
- 采样点: `dlopen+create 后 (尚未 start)` / `start+READY 后 (=加载后)` / `P1` / `P2`; 内存用 `sampleSelf()` (同进程即宿主堆, 含 .so 映射, 与 exe 可比) + `get_context_messages` 的 `dump().size()` 作 llm 字节列 (内容一致性校验: 与 M1 的 llm dump 做哈希比对, 不一致即失败)。
- 报告列与 exe 侧同表 (mode=`ffi`, side=`in-process(.so)`)，`note` 注明"FFI 为纯 client 端点 (FfiClientAgentIO), 无 TUI/CLI 渲染堆"。

---

## 6. 采样点与指标定义 (报告契约)

### 6.1 矩阵与命名

```text
bench.resource.<mode>.<side>.<point>.<metric>
mode: cli | tui | split_cli | split_tui | ffi
side: self | server | client            # in-process/ffi 只有 self
point: startup | ctx100k | ctx200k
metric: rssMB | privateMB | cpuIdlePct | cpuBusyPct | viewCount | viewBytes | llmCount | llmBytes
        | tokens(估算) | pluginsAgent | pluginsClient | note
```

- `ctx100k/ctx200k` 的判定以 `tokens` 列为准 (实测估算值, 目标 100000/200000, 允差 in-process ±2% / split ±5%)。
- M3/M4 的 `server` 行 P1/P2 与 `client` 行 P1/P2 是**同一时刻双采** (先 server 后 client, 间隔 <50ms)。

### 6.2 输出

1. 控制台大表 (示例):

```text
[resource][cli][startup]   rss=86.4MB priv=71.2MB cpuIdle=0.3% view=3/0.01MB llm=3/0.01MB tok=412 plugins=5/4
[resource][cli][ctx100k]   rss=132.7MB ... tok=100231 ...
[resource][ffi][ctx200k]   rss=... (ctxHash==cli: OK)
```

2. `BenchReporter` JSON 扩展 (老 `results[]` 不动, 新增 `resource[]`):

```json
{"timestamp":"...","results":[...],"resource":[
 {"mode":"cli","side":"self","point":"startup","rssMB":86.4,"privateMB":71.2,"cpuIdlePct":0.3,"cpuBusyPct":0.0,
  "viewCount":3,"viewBytes":12345,"llmCount":3,"llmBytes":11200,"tokens":412,
  "pluginsAgent":5,"pluginsClient":4,"note":"headless-cli, Channel, tail=0"},
 ...
]}
```

3. 每次运行同时打印: 模板组 token `g`、组数 `n100/n200`、实际 tokens、插件探测路径、mock 端口、临时目录 (失败可复现)。

---

## 7. 关键边界与风险

| 风险 | 对策 |
|---|---|
| TUI `start()` 被误调导致 CI 挂起/抢屏 | bench 内 `static_assert`/注释 + 代码层面根本不调用 `start/stop` UI; headless TUI 构造后仅 `runTransportLoop`; 报告 `note` 注明口径 |
| `Session::assertIoThread` 在 bench 线程直接写会话触发 abort (debug) | 所有会话写/读一律 `asio::post` 到 agent io 线程 (或经 Wire 往返); bench 不得直接碰 `session->viewMessages` (只经 `getSession` 在 io 线程内) |
| 压缩/截断在 200K 误触发导致 token 对不上 | `modelContenxtMaxToken=8M` + 断言每轮后 `llmMessages.size()` 单调增; 若触发 (size 回落) 即标失败 |
| 插件目录在 bench 工作目录下探测不到 (CI 从 build/exec 运行 vs 源码运行) | 复用 test 的 exe优先探测; 找不到则 `builtin://` 回退; 仍无则该行 `pluginsAgent<5` + 明确 ERROR, 不静默 |
| split 子进程端口占用/启动超时 | 先 `bind(0)` 取空闲端口再传给 server; 启动轮询 helloAck 最长 20s, 超时 kill + 输出子进程 stderr 尾部 |
| TUI 真进程需 tty | M4-client 降级为 bench 内 headless-TUI + 真 WS (server 仍真进程); 文档如实标注, 不伪称全真 |
| FFI 与 exe 上下文哈希不一致 | FFI 侧每次 P 点拉 `get_context_messages` 与 M1 的 llm dump 比哈希 (忽略序号外字段如时间戳? 模板无时间戳, 应完全一致; tool_call_id 含序号则按同 n 生成, 保证一致) |
| Windows `ru_maxrss` 单位/语义差异 | 不用 `getrusage` 作主指标; Win 用 `GetProcessMemoryInfo`, Linux 用 `/proc`, 两者 섹션 far 分开实现并单测 |
| bench 跑太久 (200K×WS turns) | in-process 直注法 P1/P2 秒级; split 用 mock turns 但模板短 + 可配 `--resource-turns-cap`; 默认 split 也全量, 环境变量 `AGENTXX_BENCH_RESOURCE_SPLIT=0` 可跳过拆分 (CI 快速档) |
| ASan 下 TUI/Channel UAF 误报 | 复用 `mode_runners` 的 `serverIO.stop + running轮询` 退出序列; 子进程 bench 不开 ASan 计时 (以 release 为准, debug 仅功能冒烟) |

---

## 8. 实施步骤 (文件级)

1. `bench_resource_util.h` (新): `sampleSelf/samplePid/cpuBegin/cpuEnd/resolveBenchPluginDir/countTokensOf(llmJson)/fixedGroup builders`。
2. `bench_util.h` (改): 新增 `ResourceResult` 结构 + `BenchReporter::addResource/flush` 资源段 (保持老函数签名)。
3. `bench_resource.h/.cpp` (新): `kBenchPlugins5/fixed模板/buildGroups/calibrate/scenarios(M1/M2/M3/M4/FFI)/reportTable`。
4. `benchmark_main.cpp` (改): 模块注册表 + `shouldRun + runBenchSync/Async + --list/--fail-fast` (仿 test 三段式, 行数 ≈ test.cpp 的 1/3)。
5. `benchmark/CMakeLists.txt` (改): `AGENTXX_BUILD_CLIENT` 条件 client 源/ftxui 链接 + `bench_resource.cpp` 编入 (GLOB 已覆盖 `.cpp`, 主要是条件块)。
6. 文档: 本文件 + `docs/zh-cn/benchmark.md` 追加 `resource` 章节 (CLI 示例 + 输出样例 + 口径声明)。
7. 验证:
   - `agentxx_benchmark --list` / `string_util` 单模块 / 全量 (release, 5 分钟内)。
   - `resource_cli/resource_tui/resource_ffi` 在 linux-release `exec/` 下跑通, P1/P2 tokens 达标, FFI 哈希一致。
   - `resource_split_cli` 跑通 (server+client 双行); `resource_split_tui` 跑通 (降级标注行)。
   - 插件缺失场景 (临时改名 plugins 目录) 应报错而非 0 值通过。
   - Windows debug/release 编译通过 (无 `/FS/MP`, `GetProcessMemoryInfo` 分支)。

---

## 9. 与本次需求条文的逐条对应

- "仿照 test 支持模块化" → §4 (注册表 + shouldRun + --list/--fail-fast, 无参全量兼容)。
- "加载 5 常用插件" → §5.5 (目录形态 5 个, agent/client 分计数, 失败显错)。
- "模式 1 同一进程 cli" → §5.6 (Channel + StdIO headless, tail=0 全量 Sync)。
- "模式 2 同一进程 tui" → §5.7 (Channel + TUI headless, tail=100 + 分页拉取)。
- "模式 3 拆分 cli+server 两者分别" → §5.8 M3 (真双进程 + 双 PID 采样)。
- "模式 4 拆分 tui+server 两者分别" → §5.8 M4 (server 真进程 + client WS 真链路, TUI 渲染堆缺失如实标注)。
- "程序启动时 / 100K / 200K 的内存+cpu" → §5.5 P0 定义 + §5.4 采样器 + §6 指标表。
- "llm 消息程序生成不用真实 api, user/assist/tool 交替且内容固定" → §5.2 模板 + §5.3 校准。
- "libagentxx_shared 同样 5 插件同条件 (加载后/100K/200K, 内容一致)" → §5.9 (dlopen 公开 API + 同 n 模板 + 哈希一致性校验)。

---

## 10. 请用户确认

1. split-TUI 的 client 降级 (headless-TUI + 真 WS) 是否可接受? 若坚持"真 Fullscreen 进程", 需约定 bench 必须在可分配 pty 的环境跑 (CI 另配), 默认仍降级。
  - 用户确认 可接受，只要尽量保证最终测试效果接近真实运行的即可
2. split 侧 100K/200K 允差 ±5% (折算组) 是否可接受? 若要求与 in-process 字节完全一致, split 需同样直注 (要给 server 加 bench 直注协议, 污染线上, 不推荐)。
  - 用户认同可接受
3. `permission.mode` bench 固定 `pass`、关闭 subagent/worktree/持久化是否可接受? 若要"与线上默认完全一致", 则 HIL 会卡死, 必须配自动应答器 (复杂度 +1 档)。
  - 用户确认 应当关闭 permission、worktree、持久化、subagent
4. 资源 bench 默认跑 release (计时可比); debug 是否只冒烟 P0? 建议是, 否则 debug 下 200K 太慢。
  - 用户确认 bench 只在 release 运行，debug一般不编译 bench，如果启用编译也仅用于测试编译通过、运行崩溃调试，不用于真实测试性能
