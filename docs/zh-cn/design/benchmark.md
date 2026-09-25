# 资源基准测试 (agentxx_benchmark)

性能基准程序 `agentxx_benchmark` 除微基准 (字符串/正则/路由/CodeAgent 初始化等) 外,
还包含一组**资源基准测试** (`resource_*` 模块), 目标是在尽量贴近真实运行的形态下
采集内存/CPU/渲染数据, 并把内存归属拆到模块级别, 供优化前后对比。

- 编译: 顶层 `-DAGENTXX_BUILD_BENCHMARK=ON` (release 脚本默认开启), 产物 `{build}/exec/agentxx_benchmark`
- 报告目录: `{build}/exec/bench/` (可用 `AGENTXX_BENCH_OUTPUT_DIR` 覆盖)
- 每次运行输出两份报告: `bench_<时间戳>.json` (机器对比) 与 `bench_<时间戳>.md` (人工阅读)

```bash
# 全部场景 (每个场景由独立子进程执行), 并与上一次报告对比
./agentxx_benchmark resource --baseline exec/bench/bench_20260101_120000.123456.json

# 单个场景 (在当前进程内直接运行)
./agentxx_benchmark resource_real_tui
./agentxx_benchmark --list
```

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
```

实测重复运行的稳定性: 进程内生场景 ΔRSS 在 ±0.3MB 内; 真实两进程 (WS 轮次驱动
200K token 上下文) 的波动几乎全部落在 `[heap]` 列 (±1MB), 这也是后续优化的重点。

## 4. 已知口径与限制

- `mallinfo2` / `malloc_trim` 只能作用于**自身进程**: 子进程场景 (split_*、
  real_tui_child、server_only) 的堆列显示 `n/a`。
- `resource_real_tui_child` 依赖伪终端 (PTY), Windows 上自动跳过。
- 子进程场景只能取 OS 级指标 (模块分解可用), 逻辑内存只对进程内对象可统计。
- 帧耗时只统计组件树构建 (`Renderer` lambda) 耗时, 不含 FTXUI 布局绘制与终端写出;
  终端写出量用"渲染输出字节"近似。
- 帧统计仅在 `AgentConfigStatic::enableBenchmark` 打开时采集 (基准程序启动时打开);
  正常运行时 TUI 无统计代码执行。
- 场景耗时基准 (本机 12 核 release, 聚合运行): 全部 9 个场景约 65 秒。

## 5. 实测结果示例 (本机 12 核 / release / 聚合运行)

内存归属 (RSS, MB):

| 场景 | 采样点 | RSS | PSS | 匿名 | 堆在用 | 堆空闲(碎片) | 可回收 |
|---|---|---|---|---|---|---|---|
| cli (同进程) | startup | 28.05 | 19.30 | 4.90 | 1.24 | 2.64 (68%) | - |
| cli (同进程) | ctx200k | 32.70 | 23.93 | 9.53 | 3.50 | 5.59 (62%) | 2.67 |
| real_tui (真实 TUI) | ctx200k | 33.29 | 24.66 | 9.99 | 4.24 | 5.11 (55%) | 1.72 |
| server_only (真实 server) | 空载 | 19.88 | 10.00 | 2.02 | n/a | n/a | - |
| server_only (真实 server) | 235 轮 / 200K token | 36.27 | 24.09 | 11.07 | n/a | n/a | - |
| real_tui_child (真实 TUI 客户端) | 235 轮 | 23.87 | 10.70 | 4.11 | n/a | n/a | - |

模块级归属 (真实 TUI 场景 200K 上下文, 前 8 项):

| 类别 | 模块 | RSS | PSS | 私脏 |
|---|---|---|---|---|
| exe | agentxx_benchmark (静态链接 libagentxx/client/ftxui) | 14.45 | 11.24 | 8.04 |
| anon | `[anon]` (mmap 区/线程栈/缓冲) | 6.67 | 6.67 | 6.67 |
| plugin-lib | agentxx_filesystem | 2.76 | 1.61 | 0.46 |
| plugin-lib | agentxx_websearch | 2.05 | 1.27 | 0.49 |
| system-lib | libstdc++.so.6 | 2.03 | 0.78 | 0.07 |
| heap | `[heap]` | 1.95 | 1.95 | 1.95 |
| system-lib | libc.so.6 | 1.62 | 0.07 | 0.02 |
| plugin-lib | agentxx_execute_command | 1.05 | 0.54 | 0.04 |

插件边际成本 (`resource_plugin_attrib`, 逐个加载):

| 插件 | 边际 ΔRSS | ΔPSS | Δ堆 |
|---|---|---|---|
| agentxx_filesystem | +4.12 | +2.81 | +0.13 |
| agentxx_websearch | +2.12 | +1.30 | +0.03 |
| agentxx_execute_command | +1.25 | +0.66 | +0.05 |
| agentxx_planning | +0.50 | +0.27 | +0.04 |
| agentxx_system | +0.38 | +0.24 | +0.02 |
| 全部卸载后回收 | -6.43 | - | - |

可直接用于优化的结论:

1. **静态链接代码的私有脏页是最大单项**: 可执行文件段 RSS 14.45MB / 私脏 8.04MB
   (未加载任何插件的基线进程 RSS 仅 12MB) —— 优化方向是减少启动即触碰的
   代码/数据 (链接期裁剪、延迟初始化、减少全局构造与重定位写入)。
2. **glibc 堆碎片高**: 场景内堆空闲达 5.59MB (占总堆 62%), `malloc_trim` 可回收
   2.67MB —— 优化方向是按大小类缓存/对象池复用长生命周期缓冲、避免反复
   "增长-释放" 造成的 arena 空洞。
3. **插件成本可量化**: 5 个常用插件合计 +8.5MB (其中 filesystem 4.1MB /
   websearch 2.1MB 是主要来源), 卸载后可回收 6.4MB; 插件内静态依赖 (curl/正则等)
   与工具 schema 是主要构成, 可按需加载 (lazy load) 降低常驻。
4. **客户端内存与服务端上下文解耦**: 同进程 TUI (tail=100 + 分页) 与服务端
   200K token 上下文下客户端仍只有 23.9MB; 但连接到真实 server 时
   `AgentServer::Config::initialSyncTailCount` 默认为 0 (全量同步), 客户端会
   持有完整历史 (实测 705 条消息时客户端 `client.tui.messages` = 380KB) ——
   长会话场景建议按客户端类型启用尾窗同步, 与本地模式的策略保持一致。

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

## 8. 一轮内存/体积优化实测 (2026-09)

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


## 9. 内存分配器 mimalloc 实测 (2026-09)

构建开关与接入范围见 `docs/zh-cn/design/index.md` "内存占用与分配器调整"
(`AGENTXX_ENABLE_MIMALLOC` —— **默认关闭**, 依据是第 10 节 Windows 侧的
长上下文实测; `AGENTXX_MIMALLOC_LINK`, 默认 STATIC)。
对比对象是**调优后的 glibc** (即第 8 节的产物: `M_ARENA_MAX=1` + 轮末
`malloc_trim(0)`), 因此本节的差值是两个都已针对常驻内存优化过的分配器之间的差值,
不是 mimalloc 与"未调优 glibc"的差值。

RSS (Release, 同机同场景, 单位 MB):

| 场景 | 采样点 | glibc (基线) | mimalloc (STATIC) | ΔRSS |
|---|---|---|---|---|
| cli (同进程) | startup | 24.91 | 30.50 | +5.59 |
| cli (同进程) | ctx100k | 26.96 | 33.00 | +6.04 |
| cli (同进程) | ctx200k | 29.41 | 37.25 | +7.84 |
| split_cli (真实 server) | startup | 24.62 | 25.00 | +0.38 |
| split_cli (真实 server) | ctx100k | 31.31 | 34.60 | +3.29 |
| split_cli (真实 server) | ctx200k | 33.66 | 40.23 | +6.57 |
| split_cli (真实 client) | ctx200k | 18.00 | 18.62 | +0.62 |

CPU 与耗时 (真实 server 进程驱动 705 轮 WebSocket 会话, 200K 上下文):

| 指标 | glibc (基线) | mimalloc (STATIC) | 变化 |
|---|---|---|---|
| user | 2280 ms | 1670 ms | -27% |
| sys | 1120 ms | 370 ms | -67% |
| wall | 3611 ms | 2700 ms | -25% |

口径说明与结论:

- mimalloc 侧的 `堆在用` / `堆空闲` / `可回收` 恒为 0 / 0 / 0 (这些指标读的是
  glibc `mallinfo2`), 内存都在 mimalloc 自己的页管理中; 模块分解里表现为
  `[heap]` 归零、`[anon]` 上升 (mimalloc 的段都是匿名映射)
- mimalloc 的常驻内存略高: 空闲页保留在各线程堆的页队列里 (清空延迟默认 1s),
  而 glibc 侧在轮末被 `malloc_trim(0)` 主动归还; 把清空延迟改成 0
  (`MIMALLOC_PURGE_DELAY=0`) 实测仅回落 ~1 MB, 同时 CPU 略升, 故保持默认
- 系统态时间的下降主要来自不再有 glibc arena 的 mmap/munmap 与轮末 trim 抖动
- **THP 必须关闭**: 上游 Linux 默认 `MI_ALLOW_THP=FULL`, 该模式下 mimalloc 以
  2 MB 大页为单位保留内存、释放小对象后不拆页, 同一场景 RSS 从 ~19.6 MB 涨到
  ~37.4 MB (匿名页); 本项目构建时固定 `-DMI_ALLOW_THP=OFF`
- 结论: 用 `AGENTXX_ENABLE_MIMALLOC` 切换两类分配器, 需要常驻内存取 glibc 组合
  (OFF), 需要 CPU/延迟与长跑抗碎片取 mimalloc (ON)。本节场景的差值是 MB 量级, 换成
  "长上下文 + 每轮大缓冲"的运行形态后差距放大到 2~3.7 倍且 CPU 收益只剩约 10%,
  故默认值改为关闭 (实测见第 10 节)

## 10. 长上下文内存: mimalloc 开关对照 (Windows release, 2026-09)

第 9 节量级在几 MB, 只覆盖 Linux 侧的短/中上下文场景。本节针对更贴近日常使用的
"长上下文 + 每轮大量临时缓冲"形态 (每轮都要把整段上下文序列化给 LLM、渲染一遍、
写入会话库): **同一份源码、同一个构建目录, 只切换 `AGENTXX_ENABLE_MIMALLOC`**,
配置 (无插件 + 单个 mock 模型) 与驱动脚本完全相同, 因此差值是分配器本身的差异。

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

### 10.1 启动与空闲 (无插件)

| 场景 | 旧版 9/17 | mimalloc 默认 | no-mimalloc (同代码) |
|---|---|---|---|
| `--version` 峰值提交 / 峰值虚拟内存 | 2.17~2.36 / 4210 | 5.36~5.76 / 5238 | **2.18 / 4211** |
| server 空闲: WS / 专用 / 提交 | 11.13 / 1.65 / 2.93 | 12.21 / 2.71 / 13.73 | **10.90 / 1.65 / 2.99** |
| TUI 空闲: WS / 专用 / 提交 | 14.52 / 2.71 / 4.22 | 16.94 / 5.07 / 41.85 | **14.41 / 2.82 / 5.05** |

关掉 mimalloc 后启动/空闲数据与旧版一致 (server 专用工作集同为 1.65 MB): 第 9 节
记录的"启动 +5.6 MB"在 Windows 上同样存在, 且全部来自分配器 —— 1.0 GiB 的 arena 预留
只占虚拟内存 (不影响任务管理器"内存"列), 提交量来自它按 64 KiB 片整块提交页、释放后
至少 1 s 才归还 (且归还只在下一次分配时被触发)。

### 10.2 长上下文 (专用工作集 / WS / 提交)

| 场景 | 旧版 9/17 | mimalloc 默认 | mimalloc 调参 | no-mimalloc (同代码) |
|---|---|---|---|---|
| ≈100K token (50 × 8KB) | 10.57 / 24.78 / 13.41 | 20.14 / 34.39 / 69.91 | 15.07 / 29.32 / 20.36 | **7.95 / 21.95 / 10.36** |
| ≈100K token (5 × 80KB) | 5.97 / 20.18 | 17.94 / 32.18 | 12.61 / 26.86 | **6.09 / 20.09** |
| ≈200K token (100 × 8KB) | – | 51.36 / 66.16 / 95.09 | – | **13.75 / 28.29 / 17.79** |

同一轮量级下 mimalloc 自己报的账 (50 × 8KB, 退出时 `MIMALLOC_SHOW_STATS=1`):
`malloc req~ 1.3 GiB` (累计分配)、`binned current 582.8 KiB` (真正存活)、
`arenas committed peak 67.5 MiB / current 12.3 MiB`、`purged 79.7 MiB` —— "分配-释放-保留"
的量级远大于存活数据, 而系统分配器对大块释放会直接交还系统。

### 10.3 CPU (100 轮 × 8KB, 子进程 CPU 时间)

| | 总 CPU | user | kernel |
|---|---|---|---|
| mimalloc | 1.625 s | 0.672 s | 0.953 s |
| no-mimalloc | 1.797 s | 0.906 s | 0.891 s |

用户态 −26%、内核态 +7%、总 CPU 只低约 10% (第 9 节 Linux 侧 −27%/−67% 出自 705 轮
WebSocket 长跑, 且 glibc arena 的 mmap/munmap 抖动比 Windows CRT 明显)。

### 10.4 结论

1. **长上下文内存差距全部来自 mimalloc, 当前代码本身更省**: 同代码关掉它后
   100K 上下文专用工作集 7.95 MB, 比旧版 9/17 (10.57 MB) 还低约 25%; 打开它变成
   20.14 MB (200K 上下文: 13.75 → 51.36 MB, 3.7 倍)
2. **额外占用随上下文超线性增长**: 50 条消息时比 no-mimalloc 多 12.2 MB, 100 条时
   多 37.6 MB (上下文翻倍 → 额外占用翻 3 倍)。原因是每轮的临时大缓冲随上下文线性
   变大, 而 mimalloc 把释放的页留在自己的队列里: 进程空闲时没有分配动作, 惰性 purge
   不会触发, 常驻就被"上一轮峰值"钉住
3. **调参只能缓解**: `PURGE_DELAY=0` + `PAGE_COMMIT_ON_DEMAND=1` 把 100K 从 20.1 降到
   15.1 MB (提交 69.9 → 20.4 MB), 仍高于系统分配器。想保留 mimalloc 又要压住提交量,
   可在构建 mimalloc 时固化
   `-DMI_EXTRA_CPPDEFS="page_commit_on_demand=1;purge_delay=0"`, 并在轮末 (或上下文
   超过阈值时) 调用 `mi_collect(true)`, 等价于系统分配器路径的 `malloc_trim(0)`
4. **收益与代价不匹配**: 总 CPU 只省约 10%, 换来 2~3.7 倍的常驻内存
5. 因此 2026-09 起**默认关闭**: 顶层 cmake option 默认 `OFF`, 各构建脚本
   (`windows_{debug,release}_build.bat`、`linux_{debug,release}_build.sh`、
   `macos_{debug,release}_build.sh` 以及 Android/交叉编译脚本) 默认也不打开;
   需要时用 `AGENTXX_ENABLE_MIMALLOC=ON` 显式打开 (Windows 上要真正接管分配器还需
   `AGENTXX_MIMALLOC_LINK=SHARED`)

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
- 若要把该场景纳入常驻基准, 可参考 `resource_cli` 的 mock LLM 与上下文模板, 新增一个
  "逐轮长上下文" 场景 (现有 `resource_cli` 是注入上下文, 序列化次数比真实逐轮对话少)

