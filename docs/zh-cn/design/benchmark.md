# 资源基准测试 (agentxx_benchmark)

性能基准程序 `agentxx_benchmark` 除微基准 (字符串/正则/路由/CodeAgent 初始化等) 外,
还包含一组**资源基准测试** (`resource_*` 模块), 目标是在尽量贴近真实运行的形态下
采集内存/CPU/渲染数据, 并把内存归属拆到模块级别, 供优化前后对比。

- 编译: 顶层 `-DAGENTXX_BUILD_BENCHMARK=ON` (release 脚本默认开启), 产物 `{build}/exec/agentxx_benchmark`
- 报告目录: `{build}/exec/bench/` (可用 `AGENTXX_BENCH_OUTPUT_DIR` 覆盖)
- 每次运行输出两份报告: `bench_<时间戳>.json` (机器对比) 与 `bench_<时间戳>.md` (人工阅读)
- 数据标注: 文档内的实测数据一律标注**日期 + 主仓 commit** (规范见第 0 节);
  当前最新一批为 `2026-09-26 / 88fee6e0` (neograph `1522761`), 原始文件在
  [`resource/benchmark/`](../../../resource/benchmark/README.md)

```bash
# 全部场景 (每个场景由独立子进程执行), 并与上一次报告对比
./agentxx_benchmark resource --baseline exec/bench/bench_20260101_120000.123456.json

# 单个场景 (在当前进程内直接运行)
./agentxx_benchmark resource_real_tui
./agentxx_benchmark --list
```

## 0. 数据标注规范

本文档里的实测数据要能被复查、也能和以后的数据对比, 因此遵守以下约定
(新增数据一律照此标注):

1. **日期与 commit 成对出现**: 每处数据标注采集日期与产生它的主仓 git commit
   (短哈希 7 位), 写法如 `2026-09-26 / 88fee6e0`; 只写日期不写 commit 的数据
   不作为对比基线;
2. **依赖变了要一起标**: 关键依赖是子模块 (neograph / cxx_utilxx / cxx_utilxx_base /
   cxx_pluginxx), 若与上一次数据之间依赖 commit 变了, 在标注后追加其 commit
   (写法如 `neograph 1522761`), 否则两组数据不可直接比较;
3. **同时标出构建与运行配置**: 平台/编译器/构建类型 (Release/Debug) 以及影响结果的
   开关 (`AGENTXX_ENABLE_MIMALLOC`、`AGENTXX_MIMALLOC_LINK` 等); 开关不同的数据
   分开列, 不要混在同一张表里比较;
4. **原始数据入库**: 值得后续对比的原始文件 (bench 报告 `bench_*.json` / `.md`、
   Windows 侧采样 `*.summary.json` 与 `MIMALLOC_SHOW_STATS` 退出统计、驱动脚本)
   复制到 `resource/benchmark/<日期>_<commit>_<平台或场景>/`, 该目录的 `README.md`
   写同样的标注; 文档引用报告时给出该目录内的路径 (例如
   `resource/benchmark/2026-09-26_88fee6e0_linux-resource/`);
5. **历史数据**: 已有的、未标注 commit 的旧数据保留原样, 但在标注处写明
   "commit 未记录", 并且只作量级参考。

> 每份 bench 报告 (`{exec}/bench/bench_<时间戳>.json`) 自带的 `timestamp` / `build` /
> `host` 只有时间与构建信息, 不含 git commit —— 入库时按第 1、2 条在 `README.md`
> 里补上, 报告文件本身保持原样。

## 1. 场景矩阵

| 模块名 | 形态 | 说明 |
|---|---|---|
| `resource_cli` | 同进程 CLI + Channel | 进程内 stdio 客户端 + agent, 真实轮次 + 100K/200K 上下文注入 |
| `resource_tui` | 同进程(无界面) TUI + Channel | TUI 端点仅作协议端点 (不启动 FTXUI), 测同步/分页/消息窗口 |
| `resource_split_cli` | 真实两进程 | `agentxx_cli server` + `agentxx_cli cli --agent ws://...`, 分别采样 |
| `resource_split_tui` | 真实 server + 连接客户端 | server 子进程 + bench 进程内 headless TUI 端点 (WS) |
| `resource_ffi` | 动态库调用 | `dlopen(libagentxx.so)` + C ABI 驱动轮次 (对照组) |
| `resource_real_tui` | 同进程 + **真实 TUI** | `TUIClientAgentIO::start()` 启动 FTXUI 界面线程, 采集帧耗时/帧数/渲染字节 |
| `resource_server_only` | 真实 server 单独运行 | 无客户端常驻; 空载漂移、真实 WS 轮次负载、客户端断开后回收 |
| `resource_real_tui_child` | 真实 TUI 子进程 + 真实 server | 客户端经**伪终端 (PTY)** 启动, 用"打字"驱动轮次 (仅 POSIX) |
| `resource_plugin_attrib` | 同进程 | 逐个加载/卸载 5 个常用插件, 量化每个插件的边际内存与回收量 |

聚合模块 `resource` 默认把**每个场景放到独立子进程**执行 (父进程合并各场景报告):
内存基准要求各场景从相同的干净进程基线开始, 同进程连续运行时前一场景的堆 arena/
页驻留/峰值 RSS 会污染后续场景的 startup 数据 (实测同进程连跑时 `plugin_attrib`
基线由 12MB 变为 41MB, 峰值 RSS 也会继承前者)。`AGENTXX_BENCH_NO_ISOLATE=1`
可退回同进程顺序运行 (快速冒烟)。

真实两进程场景的轮次负载可用 `AGENTXX_BENCH_SCALE` (0.01~1.0, 默认 1.0) 缩放,
报告 note 会标注实际使用的系数与目标 token 数。

## 2. 采集指标

### 2.1 进程级 (`bench_mem_probe.h`)

| 指标 | 来源 | 说明 |
|---|---|---|
| RSS / 私有 / 匿名 / 文件 / shmem / swap | `/proc/<pid>/status` | 常驻构成 |
| PSS / 私有脏页 / Anonymous | `/proc/<pid>/smaps_rollup` | 私有脏页最接近"真实独占内存" |
| 峰值 RSS / 峰值虚拟内存 | `VmHWM` / `VmPeak` | 优化前后峰值对比 |
| 线程数 / 文件描述符 | `/proc/<pid>/status` + `/proc/<pid>/fd` | 资源泄漏辅助信号 |
| glibc 堆在用/空闲/arena/mmap | `mallinfo2` (**仅自身进程**) | 子进程采样显示 n/a |
| 堆碎片率 | `空闲 / (在用 + 空闲)` | 高碎片 = 可优化点 |
| `malloc_trim(0)` 可回收 | RSS 前后差值 (仅自身进程) | 已 free 但未归还系统的量 |
| CPU 利用率 / 用户态 / 内核态时间 | `/proc/<pid>/stat` | 优化 CPU 用 |

Windows 平台取可获取的部分 (工作集/私有/峰值/句柄数/线程数), 堆与 smaps 相关指标不可用。

### 2.2 模块级内存分解 (`/proc/<pid>/smaps`)

按映射归属类别合并, 输出每个模块的 RSS/PSS/私有脏页/映射大小/段数:

| 类别 | 含义 |
|---|---|
| `exe` | 可执行文件自身 (静态链接进主程序的库代码/数据也在其中) |
| `project-lib` | libagentxx / libcxx_utilxx / libcxx_pluginxx |
| `plugin-lib` | 每个插件动态库单独一行 (插件名) |
| `system-lib` | libc/libstdc++/libgcc/ld 等 |
| `heap` | `[heap]` (glibc 主堆) |
| `anon` | 无路径匿名映射 (mmap 区/线程栈/缓冲) |
| `data-file` / `stack` / `vdso` / `shm` | 其他文件映射/栈/虚拟 syscall/共享内存 |

这一层回答"内存被哪些动态库/堆/匿名区占用"; 子进程同样可采集 (权限允许时)。

### 2.3 逻辑内存 (`bench_mem_logical.h`)

直接统计各模块**数据结构自身**的字节数 (与 OS 级互为补充):

- `agent.session.view_messages` / `agent.session.llm_messages` / `agent.session.msg_index`
- `agent.middleware.share_store` / `agent.plugins.registry` / `agent.plugins.tool_schema`
- `agent.graph.definition` / `agent.tool_names` / `agent.components.info` / `agent.config`
- `client.tui.messages` / `client.tui.stream_token` / `client.tui.context_snapshot` /
  `client.tui.session_list` / `client.tui.append_components` / `client.tui.pending_inputs`
- `__store.*` (会话库/索引库/全局库的**磁盘**占用, 不计入逻辑合计)

口径: 消息按字段长度求和 + 对象开销 (不调用 `toJson().dump()`, 避免统计本身产生
大量临时分配而干扰 RSS 采样)。

### 2.4 分阶段增量 (`MemPhaseTracker`)

每个场景在关键节点采样并记录相对上一步的 RSS/PSS/堆/匿名增量与耗时, 用于回答
"哪一步吃掉多少内存"。例如真实 TUI 场景:

```
process_base → agent_constructed → tui_started → agent_init_done
  → warmup_turn_done → ctx100k_ready → ctx200k_ready
```

插件边际成本场景逐项标记 `plugin_<name>`, 卸载后再标记 `unloaded`。

### 2.5 渲染性能 (真实 TUI)

- 帧数 / 平均帧耗时 / 最大帧耗时 (组件树构建耗时, 见 `TUIClientAgentIO::frameStats()`)
- 渲染输出字节: 同进程场景经 stdout 捕获文件统计; 子进程场景统计 PTY 读走字节
- 传输消息条数: 计数传输代理统计 client→server / server→client 条数, 用于确认
  "客户端确实参与了同步/流式接收"

帧统计由 libagentxx 的全局标记 `AgentConfigStatic::enableBenchmark` 控制, 默认关闭:

- 关闭时 (正常使用): 渲染每帧只做一次无等待原子读判断, 不读时钟、不累加计数,
  `frameStats()` 恒为 0
- 打开时: 每帧记录组件树构建耗时 (帧数/累计/最大)
- 基准程序在 `main()` 启动时打开该标记, 子进程场景 (每个场景独立进程) 同样生效;
  `agentxx_cli` / `agentxx_test` 不打开, 保持正常运行时的开销

## 3. 报告与对比

- JSON: `results` (微基准) + `resource` (每个采样点含 `mem`/`cpu`/`modules`/`logical`/`phases`)
- Markdown: 运行环境 → 总览 → 分阶段 → 模块分解 → 逻辑内存 → 与基线对比 (`--baseline`)
- 控制台: 每个采样点一行 + 分阶段表 + 结束时"资源对比总览"与基线差值/模块级差异

对比用法:

```bash
# 优化前
./agentxx_benchmark resource
# 优化后再跑, 并以上一次报告为基线 → 报告与控制台给出 ΔRSS/ΔPSS/Δ堆 与模块级差异
./agentxx_benchmark resource --baseline exec/bench/bench_<优化前>.json

# 基线也可以取仓库里留存的历史报告 (不依赖构建目录)
./agentxx_benchmark resource --baseline \
    resource/benchmark/2026-09-26_88fee6e0_linux-resource/bench_20260926_023051.685359718.json
```

留存的历史报告目录见 [resource/benchmark/README.md](../../../resource/benchmark/README.md)。

实测重复运行的稳定性: 进程内生场景 ΔRSS 在 ±0.3MB 内; 真实两进程 (WS 轮次驱动
200K token 上下文) 的波动几乎全部落在 `[heap]` 列 (±1MB), 这也是后续优化的重点。
(2026-09-26 / 88fee6e0 复查: Linux 侧本轮每个变体只跑一次聚合; Windows 长上下文场景
重复 3 次时提交量稳定在 ±3%, 而专用工作集/工作集随分配器归还时机有 ±20~30% 波动,
对照数值时应以提交量为主, 见 10.2 与 11 节。)

## 4. 已知口径与限制

- `mallinfo2` / `malloc_trim` 只能作用于**自身进程**: 子进程场景 (split_*、
  real_tui_child、server_only) 的堆列显示 `n/a`。
- `resource_real_tui_child` 依赖伪终端 (PTY), Windows 上自动跳过。
- 子进程场景只能取 OS 级指标 (模块分解可用), 逻辑内存只对进程内对象可统计。
- 帧耗时只统计组件树构建 (`Renderer` lambda) 耗时, 不含 FTXUI 布局绘制与终端写出;
  终端写出量用"渲染输出字节"近似。
- 帧统计仅在 `AgentConfigStatic::enableBenchmark` 打开时采集 (基准程序启动时打开);
  正常运行时 TUI 无统计代码执行。
- 场景耗时基准 (本机 12 核 release, 聚合运行): 全部场景约 1~3 分钟
  (2026-09-26 / 88fee6e0 复查: 系统分配器构建约 2.7 分钟, mimalloc 构建约 5 分钟)

## 5. 实测结果示例 (本机 12 核 / release / 聚合运行, 2026-09-26 / 88fee6e0 重测)

> 本节数据为 **2026-09-26 重新实测**: 主仓 commit `88fee6e0`, neograph `1522761`
> (依赖重写后); 环境: WSL Ubuntu 22.04, 12 核 / GCC 16.1.0 / Release, 聚合运行。
> 当次完整报告 (glibc 构建) 存于
> `resource/benchmark/2026-09-26_88fee6e0_linux-resource/bench_20260926_023051.*`;
> 与依赖更新前数值的差异见第 11 节。

内存归属 (RSS, MB):

| 场景 | 采样点 | RSS | PSS | 匿名 | 堆在用 | 堆空闲(碎片) | 可回收 |
|---|---|---|---|---|---|---|---|
| cli (同进程) | startup | 26.68 | 22.12 | 5.70 | 1.30 | 3.93 (75%) | - |
| cli (同进程) | ctx200k | 31.30 | 26.76 | 10.32 | 3.55 | 6.88 (66%) | 3.10 |
| tui (同进程, headless) | ctx200k | 31.70 | 26.99 | 10.42 | 4.16 | 6.17 (60%) | 2.05 |
| split_cli (真实 server) | startup | 24.12 | 14.84 | 2.29 | n/a | n/a | - |
| split_cli (真实 server) | ctx200k | 36.90 | 28.13 | 12.20 | n/a | n/a | - |
| split_cli (真实 client) | ctx200k | 18.12 | 8.96 | 1.88 | n/a | n/a | - |
| real_tui (真实 TUI) | ctx200k | 34.72 | 30.16 | 11.27 | 4.81 | 6.43 (57%) | 1.98 |
| server_only (真实 server) | 空载 | 20.50 | 17.42 | 2.00 | n/a | n/a | - |
| server_only (真实 server) | 235 轮 / 200K token | 41.26 | 38.33 | 15.91 | n/a | n/a | - |
| real_tui_child (真实 TUI 客户端) | 235 轮 | 26.74 | 15.49 | 4.31 | n/a | n/a | - |

模块级归属 (真实 TUI 场景 200K 上下文, 前 10 项):

| 类别 | 模块 | RSS | PSS | 私脏 |
|---|---|---|---|---|
| exe | agentxx_benchmark (静态链接 libagentxx/client/ftxui) | 14.78 | 12.49 | 0.23 |
| anon | `[anon]` (mmap 区/线程栈/缓冲) | 7.48 | 7.48 | 7.48 |
| plugin-lib | agentxx_filesystem | 2.74 | 2.74 | 0.46 |
| heap | `[heap]` | 2.40 | 2.40 | 2.40 |
| plugin-lib | agentxx_websearch | 2.12 | 2.12 | 0.49 |
| system-lib | libstdc++.so.6 | 2.06 | 0.88 | 0.07 |
| system-lib | libc.so.6 | 1.68 | 0.08 | 0.02 |
| plugin-lib | agentxx_execute_command | 0.94 | 0.94 | 0.04 |
| plugin-lib | agentxx_system | 0.49 | 0.48 | 0.01 |
| plugin-lib | agentxx_planning | 0.42 | 0.42 | 0.01 |

插件边际成本 (`resource_plugin_attrib`, 逐个加载):

| 插件 | 边际 ΔRSS | ΔPSS | Δ堆 |
|---|---|---|---|
| agentxx_filesystem | +4.12 | +4.02 | +0.13 |
| agentxx_websearch | +2.12 | +2.16 | +0.02 |
| agentxx_execute_command | +1.00 | +1.00 | +0.05 |
| agentxx_system | +0.62 | +0.53 | +0.02 |
| agentxx_planning | +0.50 | +0.46 | +0.04 |
| 全部卸载后回收 | -6.44 | - | - |

可直接用于优化的结论:

1. **可执行文件段是最大单项, 但几乎不含私有脏页**: exe RSS 14.78MB (未加载任何插件
   的基线进程仅 11.12MB), 私脏只有 0.23MB —— 这一项主要是代码/只读页的映射,
   压缩它的收益在"减少映射与文件页" (减小体积、延迟初始化、避免启动即触碰),
   而不是"减少独占内存"; 真正独占的是 `[anon]` 7.48MB (私脏 7.48) +
   `[heap]` 2.40MB + 各插件库私脏。
2. **glibc 堆碎片仍然高**: 场景内堆空闲 6.43MB (占该进程堆 57%), `malloc_trim`
   可回收 1.98MB —— 优化方向仍是按大小类缓存/对象池复用长生命周期缓冲、
   避免反复"增长-释放"造成的 arena 空洞 (第 11 节给出了当前每轮分配次数的归因)。
3. **插件成本可量化**: 5 个常用插件合计 +8.36MB (其中 filesystem 4.12MB /
   websearch 2.12MB 是主要来源), 卸载后可回收 6.44MB; 插件内静态依赖 (curl/正则等)
   与工具 schema 是主要构成, 可按需加载 (lazy load) 降低常驻。
4. **客户端内存与服务端上下文解耦**: 真实两进程下服务端 200K 上下文时 RSS 36.90MB,
   客户端只有 18.12MB; headless TUI 客户端在 100K 上下文时
   `client.tui.messages` = 231,881 字节 / 468 条 —— 长会话场景建议按客户端类型
   启用尾窗同步 (`initialSyncTailCount`), 与本地模式的策略保持一致。

## 6. 相关实现

| 文件 | 内容 |
|---|---|
| `bench_resource.cpp` | M1~M5 场景 + 聚合运行 (子进程隔离/合并) |
| `bench_resource_real.cpp` | 真实运行场景 (真实 TUI / server 单独 / PTY TUI 子进程 / 插件边际) |
| `bench_resource_util.h` | 内存/CPU 采集入口、子进程与 PTY、mock LLM 服务、固定负载模板、token 校准、yaml 配置生成 |
| `bench_mem_probe.h` | 进程内存细项、smaps 模块分解、阶段追踪、CPU 细项 |
| `bench_mem_logical.h` | 逻辑内存统计 (agent/TUI/持久化文件) |
| `bench_util.h` | 结果结构、报告 (JSON/Markdown)、基线对比、微基准计时与上报 |

## 7. 与测试的分工

- `agentxx_test memgrowth` (`agent/test/core/test_memgrowth.cpp`): 内存增长/泄漏回归
  (多轮对话逐轮采样), 用于 CI 式快速验证;
- `agentxx_benchmark resource_*`: 性能与内存归属分析, 用于优化前后对比与容量评估。

## 8. 一轮内存/体积优化实测 (2026-09, commit 未记录)

> 本节是 2026-09 那一轮优化**当时的记录** (数值来自当时的构建, 优化前后同环境对比);
> 当时未记录 commit, 只作量级参考。依赖库重写后的重新实测见第 5 节与第 11 节
> (2026-09-26 / 88fee6e0)。保留本节是为了说明改动的对应关系与体积收益。

优化项 (代码位置见 `docs/zh-cn/design/index.md` "内存占用与分配器调整"):

| 项 | 内容 |
|---|---|
| 链接期 ICF | `-Wl,--icf=all` (仅 mold/gold/lld; 顶层 `AGENTXX_ENABLE_ICF`, 默认 ON) |
| 符号裁剪 | 发布脚本 `strip` (与链接期无关, 产物已在 `exec/` 裁剪) |
| 分配器调整 | glibc `M_ARENA_MAX=1` + 轮末 `malloc_trim(0)`; Windows `_heapmin` |
| 持久化队列 | `PendingViewOp` 只存消息下标, 不再深拷贝消息 (会话库开启时生效) |
| 重连重放缓冲 | 条数上限之外增加 4 MB 估算字节上限 (`deltaBufferBytesCap`) |

产物体积 (Release, x86_64, 已 strip):

| 产物 | 优化前 | 优化后 | 变化 |
|---|---|---|---|
| `agentxx_cli` | 33.24 MB (未 strip) / 23.17 MB (strip) | 22.42 MB | -32.5% / -3.2% |
| `libagentxx.so` | 22.18 MB (未 strip) / 16.91 MB (strip) | 16.46 MB | -25.8% / -2.7% |
| `agentxx_benchmark` | 38.83 MB (未 strip) | 26.88 MB | -30.8% |

> 未 strip 的产物是构建方式差异 (直接 `cmake --build` 后未跑脚本的 strip 步骤), 
> 正常发布流程产物均已裁剪; ICF 在已 strip 产物上的净收益为 3.2% (cli) / 2.7% (.so)。

资源基准 (聚合 `resource`, 与优化前报告 `/tmp/base_A.json` 同环境对比; Δ 为 RSS):

| 场景 | 采样点 | 优化前 RSS | 优化后 RSS | ΔRSS | ΔVmSize |
|---|---|---|---|---|---|
| cli (同进程) | startup | 27.50 | 24.95 | **-2.55** | 611 → 290 MB |
| cli (同进程) | ctx200k | 32.15 | 29.53 | **-2.62** | 611 → 290 MB |
| tui (同进程) | ctx200k | 32.49 | 29.63 | **-2.86** | 539 → 282 MB |
| ffi (动态库调用) | ctx200k | 41.94 | 37.30 | **-4.64** | 1285 → 387 MB |
| real_tui (真实 TUI) | ctx200k | 33.38 | 30.38 | **-3.00** | 611 → 290 MB |
| split_cli (真实 server) | ctx200k | 35.49 | 33.79 | **-1.70** | 391 → 133 MB |
| split_tui (真实 server) | ctx200k | 36.07 | 33.78 | **-2.29** | 391 → 134 MB |
| split_tui (真实 client) | ctx200k | 19.32 | 17.84 | **-1.48** | 387 → 86 MB |
| server_only | 235 轮 / 200K | 35.68 | 33.65 | **-2.03** | 390 → 134 MB |
| real_tui_child (server) | ctxScaled2 | 36.21 | 33.46 | **-2.75** | 391 → 133 MB |
| plugin_attrib | 未加载任何插件 | 12.12 | 10.50 | **-1.62** | 153 → 89 MB |
| plugin_attrib | 5 插件全载 | 20.38 | 18.62 | **-1.76** | - |

CPU 与耗时 (同场景 user+sys / wall, 单位秒): 服务端场景 user 时间下降 5%~15%
(3.70→3.30 / 3.84→3.27 / 3.45→3.23), wall 时间持平或略降 (6.3→6.1 / 6.4→6.0),
即本轮优化未引入 CPU 代价。VmSize 的大幅下降来自 arena 数量收敛 (未固定
mmap/trim 阈值, 后者实测会增加 15% 场景耗时)。

优化后复测的稳定性: 同一产物连续两次聚合运行, ΔRSS 均在 ±0.4 MB 内
(模块级差异表为空), 与第 5 节记录的稳定性一致。

### 后续可优化方向

1. **会话消息双份存储**: `viewMessages` (展示) 与 `llmMessages` (LLM 上下文)
   各自持有正文, 200K token 历史约 1.9 MB; 若要合并需让 LLM 侧引用展示侧内容
   (涉及压缩改写语义), 改动面大
2. **`msgIndex_` 与 `WireDelta` 的 id/文本副本**: 每条消息的 id 在 `viewMessages`
   与 `msgIndex_` 中各存一份 (长会话约 100 KB 级)
3. **插件静态依赖**: 每个插件各带一份 fmt/simdjson/正则/curl; 5 个常用插件合计
   ~8 MB RSS, codegraph 单独 ~6 MB (tree-sitter 语法表 + 后台预索引)
4. **线程栈**: 默认 8 MB 线程栈使 VmSize 仍有百 MB 级冗余 (RSS 影响很小)


## 9. 内存分配器 mimalloc 实测 (2026-09-26 / 88fee6e0 重测)

构建开关与接入范围见 `docs/zh-cn/design/index.md` "内存占用与分配器调整"
(`AGENTXX_ENABLE_MIMALLOC` —— **默认关闭**, 依据是第 10 节 Windows 侧的
长上下文实测; `AGENTXX_MIMALLOC_LINK`, 默认 STATIC)。
对比对象是**调优后的 glibc** (即第 8 节的产物: `M_ARENA_MAX=1` + 轮末
`malloc_trim(0)`), 因此本节的差值是两个都已针对常驻内存优化过的分配器之间的差值,
不是 mimalloc 与"未调优 glibc"的差值。

> 本节数据为 **2026-09-26 重新实测**: 主仓 commit `88fee6e0`, neograph `1522761`;
> 两侧在**同一构建目录**内只切换 `AGENTXX_ENABLE_MIMALLOC` (mimalloc 为 STATIC 链接,
> 版本 3.5.3); 环境: WSL Ubuntu 22.04 / 12 核 / GCC 16.1.0 / Release, 聚合运行。
> 两次完整报告存于 `resource/benchmark/2026-09-26_88fee6e0_linux-resource/`:
> `bench_20260926_023051.*` (glibc) 与 `bench_20260926_024639.*` (mimalloc)。

RSS (Release, 同机同场景, 单位 MB):

| 场景 | 采样点 | glibc (基线) | mimalloc (STATIC) | ΔRSS |
|---|---|---|---|---|
| cli (同进程) | startup | 26.68 | 30.62 | +3.94 |
| cli (同进程) | ctx100k | 28.80 | 33.00 | +4.20 |
| cli (同进程) | ctx200k | 31.30 | 36.62 | +5.32 |
| split_cli (真实 server) | startup | 24.12 | 25.62 | +1.50 |
| split_cli (真实 server) | ctx100k | 34.11 | 46.65 | +12.54 |
| split_cli (真实 server) | ctx200k | 36.90 | 72.25 | +35.35 |
| split_cli (真实 client) | ctx200k | 18.12 | 19.38 | +1.26 |
| server_only (真实 server) | 235 轮 / 200K token | 41.26 | 71.91 | +30.65 |

CPU 与耗时 (真实 server 进程, `split_cli` 场景 235 轮 WebSocket 会话, 200K 上下文):

| 指标 | glibc (基线) | mimalloc (STATIC) | 变化 |
|---|---|---|---|
| user | 4360 ms | 3270 ms | -25% |
| sys | 1580 ms | 510 ms | -68% |
| wall | 5794 ms | 3852 ms | -34% |

同批 `server_only` 235 轮场景方向一致 (user 4600 → 4270 ms, sys 1410 → 580 ms,
wall 6288 → 4737 ms)。

口径说明与结论:

- mimalloc 侧的 `堆在用` / `堆空闲` / `可回收` 恒为 0 (这些指标读的是
  glibc `mallinfo2`), 内存都在 mimalloc 自己的页管理中; 模块分解里表现为
  `[heap]` 归零、`[anon]` 上升 (mimalloc 的段都是匿名映射)
- mimalloc 的常驻内存更高, 且**随上下文/轮次放大**: 同进程 cli 场景 +4~5 MB,
  真实 server 场景 200K 上下文时 +35.35 MB (上一轮实测 +6.57 MB) —— 空闲页保留在
  各线程堆的页队列里 (清空延迟默认 1s), 而 glibc 侧在轮末被 `malloc_trim(0)` 主动归还;
  每轮分配次数越多, 被"上一轮峰值"钉住的量越大 (第 11 节)
- 系统态时间的下降主要来自不再有 glibc arena 的 mmap/munmap 与轮末 trim 抖动
- **THP 必须关闭**: 上游 Linux 默认 `MI_ALLOW_THP=FULL`, 该模式下 mimalloc 以
  2 MB 大页为单位保留内存、释放小对象后不拆页, 同一场景 RSS 从 ~19.6 MB 涨到
  ~37.4 MB (匿名页); 本项目构建时固定 `-DMI_ALLOW_THP=OFF`
- 清空延迟调参 (Windows 侧实测, 见 10.4): `PURGE_DELAY=0` +
  `PAGE_COMMIT_ON_DEMAND=1` 把 100K 组的提交量从 224.4 降到 43.8 MB; Linux 侧本轮
  未重测该组合 (上一轮实测回落 ~1 MB, 同时 CPU 略升)
- 结论: 用 `AGENTXX_ENABLE_MIMALLOC` 切换两类分配器, 需要常驻内存取 glibc 组合
  (OFF), 需要 CPU/延迟与长跑抗碎片取 mimalloc (ON)。CPU 收益在 Linux 真实
  server 场景为 user −25% / sys −68% (wall −34%), Windows 长上下文场景为
  user −12% / sys −26% (总 CPU −19%), 而常驻内存代价在两平台都随会话长度放大
  (Linux +35 MB / Windows +200 MB 量级, 见第 10 节), 故默认值保持关闭

## 10. 长上下文内存: mimalloc 开关对照 (Windows release, 2026-09-26 / 88fee6e0 重测)

第 9 节量级在几 MB, 只覆盖 Linux 侧的短/中上下文场景。本节针对更贴近日常使用的
"长上下文 + 每轮大量临时缓冲"形态 (每轮都要把整段上下文序列化给 LLM、渲染一遍、
写入会话库): **同一份源码、同一个构建目录, 只切换 `AGENTXX_ENABLE_MIMALLOC`**,
配置 (无插件 + 单个 mock 模型) 与驱动脚本完全相同, 因此差值是分配器本身的差异。

> 本节数据为 **2026-09-26 重新实测**: 主仓 commit `88fee6e0`, neograph `1522761`;
> 环境: Windows release / VS18 (MSVC) / AMD Ryzen 5 5600G (6C12T) / 52 GB;
> 负载与采样口径见 §10.5; 原始采样 (每配置的 `*.summary.json` 与
> `MIMALLOC_SHOW_STATS` 统计) 存于 `resource/benchmark/2026-09-26_88fee6e0_windows-longctx/`。
> 与 2026-09-25 上一轮实测 (commit 未记录, neograph 为 `11764c5`, 即依赖重写前) 的差异见第 11 节。

四组对照:

| 组 | 说明 |
|---|---|
| 旧版 9/17 | `agent/build/output/agentxx-0.3.0-windows-x64.zip` 内的 `agentxx_cli.exe` (系统分配器 + 旧代码), 作为"上一个版本"基准 |
| mimalloc 默认 | 当前代码 + `AGENTXX_ENABLE_MIMALLOC=ON` (当时 Windows release 脚本默认 `MIMALLOC_LINK=SHARED`) |
| mimalloc 调参 | 当前代码 + mimalloc + 运行期 `MIMALLOC_PURGE_DELAY=0` + `MIMALLOC_PAGE_COMMIT_ON_DEMAND=1` |
| no-mimalloc | 当前代码 + `-DAGENTXX_ENABLE_MIMALLOC=OFF` (系统分配器; 与"mimalloc 默认"只差这一个开关) |

口径与负载:

- 专用工作集 = 任务管理器"内存"列 (`Working Set - Private`); WS = 工作集;
  提交 = 私有字节; 单位 MB
- 负载: `agentxx_cli cli` + 本地 mock LLM (SSE 立即返回, 无 toolcall); 50 条 8KB
  用户消息 ≈ 100K token 上下文, 100 条 ≈ 200K token; 另加"5 条 × 80KB"作为
  "同样上下文、更少轮次"的对照
- 每轮都要把整段上下文 (含历史) 重新序列化并发给 LLM, 是该负载内存与 CPU 的主要来源

### 10.1 启动与空闲 (无插件, 2026-09-26 / 88fee6e0 重测)

| 场景 | 旧版 9/17 (0.3.0) | mimalloc 默认 | no-mimalloc (同代码) |
|---|---|---|---|
| server 启动峰值: 峰值提交 / 峰值虚拟内存 | 2.86 / 4.12 GB | 13.77 / 5.12 GB | **3.34 / 4.12 GB** |
| server 空闲: WS / 专用 / 提交 | 10.96 / 1.59 / 2.86 | 12.22~12.23 / 2.70~2.71 / 13.77 | **10.89 / 1.61 / 2.94** |
| TUI 空闲: WS / 专用 / 提交 | 14.52 / 2.71 / 4.22 | 16.94 / 5.07 / 41.85 | **14.41 / 2.82 / 5.05** |

> `--version` 峰值提交/峰值虚拟内存本次未重测: 该命令启动后立即退出 (进程存活
> 时间短于一次采样), 采不到可靠峰值; 表中改用 server 启动峰值 (含同样的进程
> 初始化与库加载)。TUI 空闲一行为 2026-09-25 的数据 (commit 未记录; 需要真实终端,
> 未重测)。

关掉 mimalloc 后空闲数据与旧版一致 (server 专用工作集 1.61 MB): 第 9 节
记录的"启动 +5.6 MB"在 Windows 上同样存在, 且全部来自分配器 —— 1.0 GiB 的 arena 预留
只占虚拟内存 (不影响任务管理器"内存"列), 提交量来自它按 64 KiB 片整块提交页、释放后
至少 1 s 才归还 (且归还只在下一次分配时被触发)。

### 10.2 长上下文 (专用工作集 / WS / 提交, 2026-09-26 / 88fee6e0 重测, 50 × 8KB 组为 3 次运行取范围, 其余单次)

| 场景 | 旧版 9/17 | mimalloc 默认 | mimalloc 调参 | no-mimalloc (同代码) |
|---|---|---|---|---|
| ≈100K token (50 × 8KB) | 6.1~10.5 / 24.6~24.9 / 12.7~13.5 | 31.8~47.6 / 62.0~64.6 / 224.4~238.9 | 14.5 / 43.7 / 43.8 | **8.2~8.5 / 25.5~25.9 / 21.0~24.1** |
| ≈100K token (5 × 80KB) | 6.2 / 20.4 / 8.0 | 31.2 / 45.6 / 176.1 | – | **9.2 / 21.3 / 16.9** |
| ≈200K token (100 × 8KB) | 15.8 / 32.6 / 20.3 | 83.9 / 117.5 / 288.6 | – | **19.9 / 34.0 / 30.8** |

50 × 8KB 组的 `MIMALLOC_SHOW_STATS=1` 退出统计: `malloc req~ 3.1 GiB` (累计分配)、
`binned current 631.8 KiB` (真正存活)、`binned 累计 2.0 GiB + huge (≥512 KiB) 累计 1.1 GiB`、
`arenas reserved 1.0 GiB / committed peak 220.3 MiB / current 8.9 MiB`、`purged 211.3 MiB`、
线程峰值 26、用时 5.376 s (user 0.343 s / sys 0.562 s)、进程峰值 RSS 63.1 MiB /
峰值提交 224.9 MiB —— "分配-释放-保留"的量级远大于存活数据, 而系统分配器对大块释放
会直接交还系统。分配次数的逐 bin 归因与根因见第 11 节。

> 表内同一格是"专用工作集 / WS / 提交"。同一配置重复运行之间, 提交量稳定 (±3%),
> 专用工作集/WS 随分配器归还时机有 ±20~30% 波动, 表中范围包含全部重复运行
> (其中首轮与后台构建并行, 数值偏高; 括号内单独标注的即为该次)。

### 10.3 CPU (100 轮 × 8KB, 进程 CPU 时间, 2026-09-26 / 88fee6e0 重测)

| | 总 CPU | user | kernel |
|---|---|---|---|
| mimalloc | 2.515 s | 1.359 s | 1.156 s |
| no-mimalloc | 3.109 s | 1.547 s | 1.562 s |

用户态 −12%、内核态 −26%、总 CPU 低约 19% (50 轮组同向: mimalloc 0.88 s vs
no-mimalloc 1.06~1.09 s)。第 9 节 Linux 真实 server 场景的同向结果是
user −25% / sys −68% (235 轮 WebSocket 长跑), 且 glibc arena 的 mmap/munmap
抖动比 Windows CRT 明显。

### 10.4 结论

1. **长上下文内存差距全部来自 mimalloc**: 同一份代码关掉它时 100K 上下文专用工作集
   8.2~8.5 MB (与旧版 9/17 的 6.1~10.5 MB 同量级), 打开它变成 31.8~47.6 MB;
   200K 上下文 19.9 → 83.9 MB (4.2 倍)
2. **额外占用由"每轮峰值"决定, 随每轮缓冲变大而增长**: 50 条消息时比 no-mimalloc
   多约 200 MB 提交, 100 条时多约 258 MB (轮次翻倍, 额外只增 29% —— 因为决定它的是
   单轮峰值而不是轮数; 而单轮分配量随上下文增长: 62 MB/轮 → 99 MB/轮, 见第 11 节)。
   机制是 mimalloc 把释放的页留在自己的队列里: 进程空闲时没有分配动作, 惰性 purge
   不会触发, 常驻就被"上一轮峰值"钉住
3. **调参只能缓解**: `PURGE_DELAY=0` + `PAGE_COMMIT_ON_DEMAND=1` 把 100K 组的提交
   从 224.4 降到 43.8 MB (专用工作集 32.0~47.6 → 14.5), 仍高于系统分配器
   (21.0~24.1)。想保留 mimalloc 又要压住提交量, 可在构建 mimalloc 时固化
   `-DMI_EXTRA_CPPDEFS="page_commit_on_demand=1;purge_delay=0"`, 并在轮末 (或上下文
   超过阈值时) 调用 `mi_collect(true)`, 等价于系统分配器路径的 `malloc_trim(0)`
4. **收益与代价不匹配**: 总 CPU 只省约 19%, 换来 4~6 倍的专用工作集 (100K 组)
   与约 9 倍的提交量
5. 因此 2026-09 起**默认关闭**: 顶层 cmake option 默认 `OFF`, 各构建脚本
   (`windows_{debug,release}_build.bat`、`linux_{debug,release}_build.sh`、
   `macos_{debug,release}_build.sh` 以及 Android/交叉编译脚本) 默认也不打开;
   需要时用 `AGENTXX_ENABLE_MIMALLOC=ON` 显式打开 (Windows 上要真正接管分配器还需
   `AGENTXX_MIMALLOC_LINK=SHARED`)
6. 与 2026-09-25 的上一轮实测 (commit 未记录, neograph 为 `11764c5`) 相比,
   同一负载的数值整体升高 (mimalloc 提交
   69.9 → 224.4 MB, 系统分配器提交 10.36 → 21.0~24.1 MB): 依赖库重写后每轮的
   分配次数增加了约 1.4 倍, 归因与优化方向见第 11 节

### 10.5 复现方式

- 两个变体都在**同一个构建目录**里产出 (只差一个开关, 其余参数/依赖完全一致):

```bash
# no-mimalloc
cmake -B agent/build/windows-release -S agent -DAGENTXX_ENABLE_MIMALLOC=OFF
cmake --build agent/build/windows-release --config Release --parallel 6

# mimalloc (对照)
cmake -B agent/build/windows-release -S agent -DAGENTXX_ENABLE_MIMALLOC=ON
cmake --build agent/build/windows-release --config Release --parallel 6
```

- 负载侧: 起一个本地 OpenAI 兼容 mock 服务 (只回 SSE), 配置里写
  `base_url: http://127.0.0.1:<port>/v1` 且 `plugin.list: []`, 然后用
  `agentxx_cli cli --config <cfg>` 逐条送入固定大小的用户消息 (每条一轮),
  其间按固定间隔采样专用工作集/工作集/提交
- Windows 专用工作集口径: `Win32_PerfFormattedData_PerfProc_Process.WorkingSetPrivate`
  (任务管理器"内存"列); WS / 提交取 `WorkingSet64` / `PrivateMemorySize64`
- 本次 (2026-09-26 / 88fee6e0) 使用的驱动: mock 为最小 Python SSE 服务 (立即返回),
  驱动逐条写被测进程的 stdin (每行一条消息 = 一轮), 轮次全部被 mock 服务收到后
  空转 4 秒再读稳态值; 采样项 (WS/专用工作集/提交/峰值提交/CPU 用户态与内核态)
  与复现步骤见 `resource/history/memory-1/plan.md` 第 4 节; 脚本与用法见
  `resource/benchmark/harness/README.md`
- 若要把该场景纳入常驻基准, 可参考 `resource_cli` 的 mock LLM 与上下文模板, 新增一个
  "逐轮长上下文" 场景 (现有 `resource_cli` 是注入上下文, 序列化次数比真实逐轮对话少)

## 11. 依赖重写后的重新实测与分配次数归因 (2026-09-26 / 88fee6e0)

2026-09-25 把 neograph 子模块更新为上游重写后的 master + 重写后的 fork 补丁
(`1522761`, 主仓提交 `88fee6e0`) 之后, 同一负载的每轮分配次数与常驻内存都上升了。
本节记录重新实测的数据与归因; 针对性的优化设计 (零拷贝读取、就地 append、
收敛整段状态序列化) 与验收指标见
[resource/history/memory-1/plan.md](../../../resource/history/memory-1/plan.md)。

标注: 重测数据 = 2026-09-26, 主仓 `88fee6e0`, neograph `1522761`;
"上一轮" = 2026-09-25 的实测, **commit 未记录** (当时 neograph 为 `11764c5`,
即依赖重写前), 只作量级对照。原始数据存于 `resource/benchmark/` 下两个目录
(`2026-09-26_88fee6e0_windows-longctx/`、`2026-09-26_88fee6e0_linux-resource/`)。

负载与方法 (与 §10 同一套台账): Windows release, `agentxx_cli cli` + 本地 mock LLM
(只回 SSE 且立即返回), 无插件, data_dir 指向临时目录, 经 stdin 逐条送入固定大小消息
(每条一轮), 全部轮次被 mock 服务收到后空转 4 秒取稳态。分配次数取自
`MIMALLOC_SHOW_STATS=1` 的退出统计 (只有 mimalloc 变体能取到)。

每轮分配次数 (退出时的 `malloc req~`, 累计):

| 组 | 重测 (2026-09-26 / 88fee6e0) | 上一轮 (2026-09-25, commit 未记录) | 变化 |
|---|---|---|---|
| 50 轮 × 8KB (≈100K token, 请求体 ≈410 KB) | 3.1 GiB (62 MB/轮) | 1.3 GiB (26 MB/轮) | 2.4 倍 |
| 50 轮 × 100B (小上下文, 请求体 ≈15 KB) | 1.3 GiB (26 MB/轮) | 290 MiB (5.8 MB/轮) | 4.5 倍 |
| 100 轮 × 8KB (≈200K token) | 9.9 GiB (99 MB/轮) | – | – |
| 5 轮 × 80KB (≈100K token, 更少轮次) | 364.8 MiB (73 MB/轮) | – | – |

50 轮 × 8KB 组的分 bin 归因 (累计量 / 每轮块数):

| 块大小 (bin) | 累计量 | 块数 | 每轮块数 | 备注 |
|---|---|---|---|---|
| ≥512 KiB (`huge`) | 1.1 GiB | 570 | ≈11 | 平均 ≈2 MB/块 |
| 257.0 KiB | 486.5 MiB | 1.9K | ≈38 | |
| 128.5 KiB | 393.8 MiB | 3.1K | ≈62 | |
| 10 KiB | 419.1 MiB | 42.9K | ≈858 | |
| 64.2 KiB | 243.3 MiB | 3.8K | ≈76 | |
| 32.1 KiB | 132.5 MiB | 4.2K | ≈84 | |
| 16.0 KiB | 67.5 MiB | 4.3K | ≈86 | |
| 384 B | 59.4 MiB | 162.3K | ≈3.2K | |
| 6.0 KiB | 47.7 MiB | 8.1K | ≈162 | |
| 128 B / 32 B | 19.8 / 7.2 MiB | 162.8K / 237.7K | ≈3.3K / ≈4.8K | |

归因 (代码位置): 重写后的一次运行里图状态被反复整段序列化 ——
`neograph/src/core/graph_engine.cpp:1370 / 1404 / 1460 / 1505 / 1541 / 1627 / 1637`
(`state.serialize()`), 以及 `graph_executor.cpp:222`
(`hash_state_for_cache(state.serialize())` —— 只为算一个缓存键就整段序列化) 与
`graph_executor.cpp:772` (`state_snapshot`); 这些序列化都包含完整的 messages 通道,
与实测的 `huge` + 128/257 KiB 三档块对应。旧基线里占大头的 8 KiB 档
(当时推断"每条消息文本被拷贝约 13 次/轮") 现在只有 2.8 MiB / 365 块。
`10 KiB` 与 `384 B` 两档的来源尚未定位 (协议帧/日志缓冲/流式渲染缓冲待确认)。

常驻内存对照 (系统分配器口径, 专用工作集 / WS / 提交, MB):

| 组 | 重测 | 上一轮 |
|---|---|---|
| 50 轮 × 8KB | 8.2~8.5 (首次运行 11.8) / 25.5~25.9 / 21.0~24.1 | 7.95 / 21.95 / 10.36 |
| 100 轮 × 8KB | 19.9 / 34.0 / 30.8 | 13.75 / 28.29 / 17.79 |
| 50 轮 × 100B | 4.6 / 19.3 / 14.9 | – |

Linux 侧同批重测 (完整表见第 5 节; 与上一轮对照 RSS, MB):

| 场景 | 采样点 | 重测 | 上一轮 |
|---|---|---|---|
| cli (同进程) | startup | 26.68 | 28.05 |
| cli (同进程) | ctx200k | 31.30 | 32.70 |
| real_tui (真实 TUI) | ctx200k | 34.72 | 33.29 |
| server_only (真实 server) | 空载 | 20.50 | 19.88 |
| server_only (真实 server) | 235 轮 / 200K token | 41.26 | 36.27 |
| real_tui_child (真实 TUI 客户端) | 235 轮 | 26.74 | 23.87 |

> Linux 列的两侧来自不同构建 (依赖重写前/后各一次完整构建), 除代码差异外还包含
> 少量构建差异, 只作量级对照; Windows 列两侧为同一构建目录内切换开关, 可直接比较。

结论:

1. **每轮分配次数上升约 2~4 倍**: 小上下文组的固定开销从 5.8 MB/轮 涨到 26 MB/轮,
   说明增加的主要是"每轮固定要做的事"(状态序列化/协议/持久化), 不是随上下文增长的部分;
2. **长会话与真实 server 侧常驻增长最明显**: `server_only` 235 轮 +5.0 MB,
   `real_tui_child` 客户端 +2.9 MB, 而同进程短场景基本持平 (cli startup 反而略降);
3. **方向上先动"整段状态序列化"**: 它同时是分配次数 (huge 1.1 GiB) 与每轮固定开销的
   主要来源, 收敛它比继续抠请求体链路的收益更大 (详见 plan.md 的 P0)。

