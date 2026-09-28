# 定位记录: MSVC 为什么给这些协程帧预留异常大的空间 (LTO 相关)

- 承接: [memory-1/work.md §3.1](../memory-1/work.md) —— 上一轮把每轮 11 次 ≥512 KiB
  的分配定位为 **asio awaitable 协程帧** (2.5 MB / 824 KB), 但"为什么 MSVC 给这些
  帧算这么大"未解
- 类型: 只读分析 (未改动主仓源码与构建脚本; 未提交任何构建产物)
- 时间: 2026-09-27
- 状态: **原因已定位** (与 LTO 内联面直接相关), 缓解手段已在真实 release 产物上
  验证 (见 §2.5); 是否落地为构建改动待定 (见 §4)

## 1. 结论

一句话: **帧尺寸是编译器写死的常量, 它取决于"链接期有多少代码内联进这个协程";
把协程所在的目标文件也交给 `/GL` 做全程序优化时, MSVC 给这些帧预留的字节数会
放大到 MB 级, 而生成的代码只用到其中几百字节。**

支持结论的四组事实 (全部可复现):

1. **尺寸来自编译器常量, 不是数据结构大小**: 反汇编里帧分配点都是
   `mov ecx, <imm32>; call <帧分配器>`, 紧接着把 `_ResumeCoro$1` / `_DestroyCoro$2`
   两个函数地址写进新帧。`imm32` 与 mimalloc 侧记录的请求量只差 31 字节
   (asio 的 `thread_info_base::allocate` 附加的头部/对齐), 说明尺寸就是编译器算出来的。
2. **与 LTO 的"内联面"正相关** (`base_agent.cpp` 同一份 release 参数):

   | 构建方式 | 该 TU 里最大协程帧 | ≥384 KiB 的帧 (整个程序) |
   |---|---|---|
   | 去掉 `/GL` 单 TU 编译 | 11,680 B (0x2DA0) | – (未链接) |
   | `/GL` 单 TU 单独 `/LTCG` 链接成 DLL | 12,624 B (41 个帧, 39 个 <4 KiB) | 0 |
   | 全程序 `/GL` + `/LTCG` (`agentxx_cli.exe`) | – | **94 个 / 164.6 MiB, 最大 6.45 MiB** |
   | debug 构建 (无 `/GL`, `/Od`) | 最大 ≈17.7 KiB | 0 (>64 KiB 的一个都没有) |

3. **巨帧基本没被使用**: 以 2,506,432 B 的那个帧为例, 它的起始代码只写到了新帧的
   偏移 `0x10 ~ 0x3A0` 一带 (promise + 形参 + 少量局部), 其余 2.5 MB 从未被访问;
   上一轮对已释放块的扫描也显示帧内除头部几百字节外全是零 (memory-1 §3.1)。
4. **不是"帧里装了个大对象"**: 项目自身源码里最大的栈上数组是 8 KiB
   (`neograph/src/core/tool_execution.cpp:548` 的 `char buffer[8192]`, 另外两个 4 KiB),
   全项目没有 MB 级局部对象; 且帧尺寸随被内联的代码量变化 (§2.5), 而对象大小不会。

由此得到的机制解释 (推断, 与上面四组事实一致): `/GL` 构建里协程帧的布局/尺寸是在
**链接期代码生成 (LTCG) 阶段、全程序内联之后**决定的; 此时协程体里已经塞进了大量
内联代码, MSVC 为这些内联进来的局部/临时对象预留了帧空间, 却没有像非 LTO 路径那样
把"不跨挂起点存活"的局部收回栈上。程序的协程都是长链 (`runTurnAsync` →
`catchErrorAsync` → `catchErrorAsyncImpl` → `lambda` → `AgentRunner::run` → …), 链上
每一层都会内联下面若干层, 于是**同一条链上的帧尺寸彼此接近 (外层 = 内层 + 自身少量局部)**
—— 这正好解释了上一轮"一个只有 `co_return co_await X()` 的薄包装协程也拿到 2.5 MB"
的现象。

## 2. 证据

### 2.1 方法

1. 在 PE 的 `.text` 里按字节模式 `B9 <imm32> E8 <rel32>` 扫出所有
   "把常量装进 ECX 再调用"的站点, 按调用目标分组;
2. 目标是**协程帧分配器**的特征: 尺寸种类多 (几百种)、最小尺寸很小 (几十字节) ——
   用 `/MAP` 查符号名确认 (`??2?$awaitable_frame_base@Vany_io_executor@asio@boost@@…`)。
   这一步很必要: 同样的字节模式会命中 `ERR_set_error` (OpenSSL)、`??2@YAPEAX_K@Z`
   (全局 operator new)、`CRYPTO_*`、`sqlite3Malloc`、`cmark_utf8proc_*` 等, 它们
   的"尺寸"与协程帧无关 (报告里的 `>=384 KiB` 统计已按符号名过滤掉这些);
3. 需要把具体帧归属到函数时, 用 `link.exe` 重链一次 `/DEBUG:FULL /MAP`
   (不改代码, 只加调试信息; 实测 **145 s**, 产物放在系统临时目录), 再按地址在
   `.map` 里查符号名。
4. 工具已收进仓库: [`resource/benchmark/harness/scan_coro_frames.py`](../../benchmark/harness/scan_coro_frames.py)
   (`--map` 可选), 可直接对 release 产物输出下面的分布表。

### 2.2 尺寸分布 (release `agentxx_cli.exe`, 全 `/GL` + `/LTCG`)

协程帧分配点 891 个:

| 尺寸区间 | 个数 |
|---|---|
| < 4 KiB | 552 |
| 4 ~ 64 KiB | 221 |
| 64 ~ 384 KiB | 24 |
| **384 KiB ~ 1 MiB** | **46** |
| **1 ~ 4 MiB** | **41** |
| **4 ~ 16 MiB** | **7** |
| ≥ 384 KiB 合计 | **94 个 / 164.6 MiB, 最大 6,766,896 B (6.45 MiB)** |

同一份代码的两个对照:

- `libagentxx.dll` (release, 同样 LTO): 同类分布, ≥384 KiB 的约 60 个, 最大 ≈4.9 MiB;
- `libagentxxd.dll` (debug, 无 LTO): 帧分配点 7436 个, 其中 7408 个 < 4 KiB,
  最大约 17.7 KiB —— **没有任何 ≥64 KiB 的协程帧**。

### 2.3 具体帧的归属 (来自 §2.1 的 `/MAP`)

| 帧尺寸 | 该帧属于 | 分配点所在函数 (inline 展开后) |
|---|---|---|
| 2,506,432 | `AgentRunner::run` 的协程 | `BaseAgent::runTurnAsync` 内 `lambda_3` 的 `_ResumeCoro$1` |
| 823,584 | `GraphEngine::run_async_with_runtime` 的协程 | `GraphEngine::run_stream_async` 的 `_ResumeCoro$1` |
| 6,766,896 等 6 个 | 训练 (`[Training] …` / `[EvolutionTraining] …`) 相关协程 | 客户端 `resources/train` 一侧 |
| 4,987,600 | subgraph 运行 (`subgraph/` 字符串) | `GraphEngine::run_subgraph_async` 一带 |

这些函数体看起来都很小 (例如 `GraphEngine::run_stream_async` 只有一行
`co_return co_await run_stream_async(std::move(config), …)`), 却拿到 824 KB 的帧 ——
即"帧尺寸由被内联进来的东西决定, 而不是函数自己写了多少局部变量"。

### 2.4 单 TU 实验 (确认"非 LTO 路径不放大")

用 release 的原始编译参数 (仅去掉 `/GL`) 单独编译 `base_agent.cpp`:

- 该 TU 里 39 个帧分配点, 尺寸 208 B ~ 11,680 B, 其中 `runTurnAsync` 及其
  `catchErrorAsync` / `lambda` 链上的帧都很小 (几百 ~ 3.3 KiB);
- 把该 `.obj` 单独 `/LTCG` 链接成 DLL (其他 TU 不参与) 后, 全文件没有任何
  `mov ecx, ≥64 KiB` 的帧分配点。

⇒ 巨帧不是"某个函数写法"或"帧类型本身大", 而是链接期全程序内联的产物。

### 2.5 去掉 `/GL` 的验证 (真实产物, 可落地)

只把 `base_agent.cpp` 改成不加 `/GL` (用 §2.4 编出的 `.obj` 顶掉静态库里同名目标文件),
其余对象与链路参数完全不变, 重链 `agentxx_cli.exe` (161 s):

| 指标 | 全部 `/GL` | `base_agent.cpp` 去掉 `/GL` |
|---|---|---|
| 帧分配点 | 891 | 884 |
| ≥384 KiB 的帧 | **94 个 / 164.6 MiB** | **51 个 / 73.7 MiB** |
| 1 ~ 4 MiB 的帧 | 41 | 24 |
| 4 ~ 16 MiB 的帧 | 7 | 1 |
| 最大帧 | 6,766,896 B | 4,987,600 B |
| 上一轮记录的那 4 个 2.5 MB 帧 (`0x263EC0/0x2646C0/0x2649C0/0x264AC0`) | 存在 | **全部消失** (按字节搜索 0 命中) |
| 6.77 MB 组 (`0x674130` 等) | 存在 | 全部消失 |

⇒ 巨帧的**产生源头就在协程所在 TU 是否参与 LTO**: 只把 `base_agent.cpp` 排除出 LTO,
上一轮量到的那条链 (runTurnAsync → catchErrorAsync → AgentRunner::run) 的 2.5 MB 帧
就整条消失, 连"稍远处的"训练协程帧 (它们内联了 `AgentRunner::run`) 也跟着从 6.7 MB
降到 ≤4.9 MB。

## 3. 对内存账的影响与处置建议

- 这份预留**只在分配器会保留/提交大块时才变成内存账** (memory-1 §3.1 的表):
  Windows 默认 (mimalloc 关闭) 走 `VirtualAlloc` 路线、未触碰的页不驻留, 曲线与
  Linux 同量级; mimalloc 会整块提交并留在页队列里, 于是"每轮 22 MB"被跨轮保留。
- 建议 (按代价从低到高):
  1. **保持 mimalloc 默认关闭** —— 现状即如此, 无需改动; 这条开销在默认路径上几乎
     不体现为内存 (只是每轮多 11 次 MB 级 `VirtualAlloc`/`VirtualFree` 的系统调用);
  2. 若要开 mimalloc: 配 `MIMALLOC_PAGE_COMMIT_ON_DEMAND=1` + `MIMALLOC_PURGE_DELAY=0`
     (上一轮实测提交量 224.4~238.9 MB → 43.8 MB ≈ 基线 + 1 轮残留);
  3. 想**彻底消除**帧预留: 让"长链协程所在的目标文件"不参与 LTO (`/GL` 单独关掉),
     §2.5 已验证效果 (94 → 51 个巨帧、2.5 MB 帧整条消失)。代价是这些 TU 失去跨 TU
     内联 —— 需要先量一下性能影响再决定; 候选目标: `agent/lib/src/agent/base_agent.cpp`、
     `neograph/src/core/graph_engine.cpp`、客户端 `resources/train` 一侧
     (按 §2.3 的归属, 这三处贡献了绝大多数巨帧);
  4. 也可以反过来减少每轮**新建协程数** (把 `catchErrorAsync` 这类薄包装摊平进调用者),
     但单帧尺寸不变, 收益上限只有 memory-1 记录的那 11 帧里的一部分, 优先级低于 3。
- 回归指标 (可直接用 §2.1 的脚本): `agentxx_cli.exe` 中 **≥384 KiB 的帧分配点数与最大帧**;
  当前 release 基线 = 94 个 / 最大 6.45 MiB (memory-1 的验收目标"每轮 ≤3 块 huge"
  是运行时口径, 两者不冲突)。

## 4. 复现与工具

```powershell
# 尺寸分布 (带 --map 时会按符号名过滤掉非帧分配器)
python resource/benchmark/harness/scan_coro_frames.py <agentxx_cli.exe> [--map <ltcg_cli.map>]

# 需要函数级归属时: 用构建目录里的 link.command.1.tlog 重链一次 (不改代码)
#   在 rsp 里追加 /DEBUG:FULL /MAP:<map> /LTCG, 输出改到临时目录
```

分析过程中搭过但**未入库**的临时手段 (做法记在这里即可):

- 单 TU 编译脚本: 从 `agentxx_static.dir/Release/agentxx_static.tlog/CL.command.1.tlog`
  取原始 cl 参数, 去掉 `/GL`、改 `/Fo`/`/Fd` 到临时目录, 22 s 出一个可反汇编的 obj;
- 微基准 (ascii/自定义 promise) 用于验证"内联的大局部是否会进帧": 结论是**不会**,
  但发现 MSVC 14.51 在 release 下会做**协程帧消除** —— 帧被放到调用者的栈上,
  promise 的 `operator new` 一次都不调用 (即使把句柄地址交给另一个 TU 的不透明函数)。
  所以想用微基准量帧尺寸, 必须用 `co_spawn` 这类能保证帧逃逸的写法, 否则量到的是 0;
  最终的探针工程 (小复现) 见 `resource/benchmark/harness/min_coro_frame/`。

## 5. 未做 / 后续

- 未实施构建改动 (是否对个别 TU 关闭 LTO, 需要先量吞吐影响再定; 见 §3 第 3 条);
- 未进一步下探 MSVC 内部到底是哪个 pass 把内联局部记进帧 (二进制层面看不到,
  需要 MSVC 源码或官方 issue; 现有证据只能给出"与 LTO 内联面正相关"的规律);
- 未在 Linux/macOS 上重复本套扫描 (memory-1 已有 GCC 侧运行时对照: ≥512 KiB 为 0),
  如需 CI 卡口, 可把这脚本接到现有 benchmark 产物上。

## 6. VS 更新后的复测 (2026-09-27 第 2 轮)

工具链: VS 18.10.2 (18.10.12217.157), 编译器 **19.51.36260** (更新前 19.51.36231),
链接器 14.51.36260.0。三种测法都做了, 结论: **行为与数字都未变**。

### 6.1 小复现 (未复现, 与"需要大内联面"的结论一致)

新增探针工程 `resource/benchmark/harness/min_coro_frame/` (asio awaitable 三层链:
内层 → 一行转发 → 三层转发, 与 `catchErrorAsync` / `GraphEngine::run_stream_async`
同构, 外加若干可内联的"重"函数; 用 `co_spawn` 保证帧逃逸, 不会被 MSVC 的帧消除挪到
调用者栈上)。同一份源码编两遍, 用全局 `operator new` 观测帧尺寸:

| 构建 | 帧尺寸 |
|---|---|
| `/O2` (无 LTO) | 1,532 B 与 2,064~2,080 B |
| `/O2 /GL` + `/LTCG` | 完全相同 (1,532 B 与 2,080 B), 静态扫描也只有 <2.1 KiB 的帧 |

⇒ 小工程**复现不出来**: 巨帧要的是"全程序 LTO 的大内联面", 不是某个写法。
这个工程留作探针, 以后换编译器可直接重跑 (`build.bat` + `scan_coro_frames.py`)。

### 6.2 真实代码复测 (问题原样存在)

方法: 用新编译器的 release 参数把 `base_agent.cpp` 编成 `/GL` 目标文件, 顶掉静态库里
同名目标文件, 其余对象与参数不变重链 `agentxx_cli.exe` (`/LTCG /DEBUG:FULL /MAP`,
167 s; 该替换法本轮先做过对照, 见 §6.3)。

| 构建 | ≥384 KiB 的帧 | 最大帧 | 上一轮的 2.5 MB / 6.77 MB 常量 |
|---|---|---|---|
| 原 release (老 cl, 全 `/GL`) | 94 个 / 164.6 MiB | 6,766,896 | 存在 |
| 老 cl 目标文件 + **新链接器**重链 | 94 个 / 164.6 MiB | 6,766,896 | 存在 |
| **新 cl `/GL`** 目标文件替换后重链 | 94 个 / 164.6 MiB | 6,766,896 | 存在 (`0x263EC0` ×4、`0x674130` ×1 …) |
| **新 cl 非 `/GL`** 目标文件替换后重链 | 51 个 / 73.7 MiB | 4,987,600 | 全部消失 (0 命中) |

⇒ 更新 VS 后**问题原样存在**(尺寸常量逐位相同), 而"该 TU 不加 `/GL`"这个缓解手段
同样仍然有效。

### 6.3 单 TU 数据点修正 + 方法提醒

- §1 表里原先那行"去掉 `/GL` 单 TU + `/LTCG` 单独链接"当时实际链接的是**非** `/GL`
  的目标文件 (临时脚本里的 `-KeepGL` 没生效, `/GL` 早已被从参数里删掉)。本次用真
  `/GL` 目标文件单独 `/LTCG` 链接成 DLL 重测: 41 个帧, 39 个 <4 KiB, 最大 **12,624 B**,
  没有 ≥384 KiB 的帧 ⇒ 结论不变 (单 TU 可内联面小, 放大不起来), 表已按实测改准。
- 方法提醒: 用"obj 替换 + 重链"做 A/B 之前先确认目标文件类型 ——
  `dumpbin /headers x.obj | findstr /i "File Type"`, 只有 `ANONYMOUS OBJECT` 才是
  `/GL` 产物; `EXTENDED COFF OBJECT` 是 `/bigobj` 的普通机器码目标文件。
- 附带发现: 老编译器产出的 `/GL` 目标文件可以被**新链接器**正常 LTCG 重链
  (14.51.36231 → 14.51.36260 同 minor 版本), 所以"只用新链接器重链"也是一种
  低成本复测手段 (本次结果与新旧编译器全套重编一致)。

## 7. 后续: 机制量化与处置落地 (2026-09-28 / c957b504)

本节由排查"每轮 11 次 ≥512 KiB 分配"直接接续, 补上了 §1 里"机制"的最后一环
(**外层协程帧把每个被 `co_await` 的内层协程帧又算了一遍**), 并把 §3 建议的第 3 条
换成了不牺牲 LTO 的代码级处置 (已落地)。

### 7.1 机制 (反汇编证据)

反汇编 `AgentRunner::run` 的恢复函数 (release `/LTCG`) 可见:

```
mov     ecx,0C9EF0h                   ; 825,584 B = 内层 GraphEngine::run_stream_async 帧大小
call    awaitable_frame_base<...>::operator new
mov     qword ptr [rbx+19A000h],rax   ; 内层帧指针写进**本函数自己的帧**的 1.6 MB 偏移处
```

统计该函数对自身帧的全部 1017 个内存操作数: 634 个 < 4 KiB, 365 个在 4~64 KiB,
0 个在 64 KiB~1 MiB, 18 个在 1~1.6 MiB —— 即 2.5 MB 里只有前 64 KiB 密集使用,
后半段是给两个被 `co_await` 的内层协程留的状态区。尺寸逐位吻合:
2,506,528 B ≈ 825,584 (run_stream_async) + 1,661,232 (resume_async) + 自身 ≈19,712 B。

⇒ §1 的"与 LTO 内联面正相关"应表述为: 内层协程被内联进来时, 外层帧会**再算一份
内层帧**, 而内层帧本身仍单独分配; 链条越深、内层越大, 外层帧越大 (与 §1 的事实的
2/4 一致, 也解释了"薄包装协程拿到 2.5 MB"与训练链上 11 个同级帧)。

### 7.2 处置 (已落地, 不关 LTO)

- `AgentRunner::run` 打 `__declspec(noinline)` (宏 `AGENTXX_NOINLINE` 定义在
  agent/lib/include/agentxx/agent/agent_runner.h);
- neograph 的引擎调用用**非协程 noinline 转发函数**包住
  (agent/lib/src/agent/agent_runner.cpp 的 `engineRunStreamAsync` / `engineResumeAsync`):
  包装自身没有帧, 内层帧在包装里单独分配, 调用者只拿到一个 awaitable 句柄。

效果 (同一构建, 只加上述改动): 静态 ≥384 KiB 的帧 96 → 66 个 (165.6 → 92.9 MiB),
1~4 MiB 的帧 41 → 11, **生产路径 (agent 轮次) 不再有 ≥384 KiB 的帧**;
运行时 (mimalloc, 100×8KB) `huge` 累计 2.3 GiB → 433 MiB、峰值提交 149.5 → 89.1 MiB、
稳态提交 148.8 → 89.1 MB、专用工作集 59.8 → 42.0 MB。
完整表格与复现口径见 [benchmark.md 第 13 节](../../../docs/zh-cn/design/benchmark.md)。

未处置: 训练模式 (`runTrainingMode` 6.77 MB / `runEvolutionLoop` 1.67 MB) 与
neograph 子图 (`SubgraphNode::run` 4.98 MB / `run_subgraph_async` 1.66 MB),
都只在非默认路径上分配, 需要时用同样的两种做法处理。

### 7.3 与 §3 建议的关系

§3 建议的第 3 条 (把长链协程所在 TU 排除出 `/GL`) 仍是兜底手段, 但会牺牲跨 TU 内联;
本轮改用"在重边界打断内联"的代码级处置, 代价只有两处调用不再内联, 效果与
"排除 base_agent.cpp 出 LTO" 同量级 (§2.5: 94 → 51), 且不依赖构建开关。
