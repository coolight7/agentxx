# Agentxx 插件共享功能设计方案（功能点统一：两类型 + 值缓存 + 自省 + 对外开放调用）

> 目标产物：本文件（`resource/history/plugin-share-feature/plan.md`）。
> 参考：musicxx 的 `resource/history/extern-plugin-share-feature/plan.md`（本轮：埋点对外开放调用 +
> 插件自定义功能点 + 能力调用全面异步化）与 `resource/history/future-bind/plan.md`（上一轮：
> 埋点统一成三个类型 + 值缓存 + 自省）。
> 关联：根 `AGENTS.md`（插件设计约束）、`docs/zh-cn/design/plugins.md`（§4 导出与生命周期、§8 agent 侧接口表、
> §15 实例生命周期、§16 协程驱动）、`docs/zh-cn/design/index.md`（整体架构 / 中间件 / 事件系统）、
> `docs/zh-cn/design/ffi.md`（§4.9 宿主工具）。
> 时间：2026-10-10。
> 状态：**设计已定稿，未开始实施**；实施按 §15 的 7 个阶段逐个提交，记录写到同目录 `work.md`（实施时创建）。
>
> 二稿修订（按反馈）：① 层只分 `plugin` / `core`（agentxx 只有一套插件框架，`builtin://` 与动态库同层）；
> ② "值仓"改称**值缓存**（`CacheMode` / `cache` 字段）；③ 去掉 `allowPluginImpl`（埋点天生接受外部实现）；
> ④ 压缩暂不对外开放调用（进 §18 路线图）；⑤ 钩子补处理器清单与优先级（§6）；
> ⑥ 功能点暴露给 FFI 与命令行（§9.5、§9.6）；⑦ 工具输出摘要不缓存、不设上限；
> ⑧ 插件点固定不缓存（进 §18 路线图）；⑨ 新增 §18 路线图。
>
> 三稿修订（按第二轮反馈）：① `priority` 越界**裁剪到上下限 + 警告日志**，不拒绝登记；
> ② **不限制**钩子处理器与功能点实现的注册数量（非必要不加检查与限制）；
> ③ FFI 宿主实现属于 `plugin` 层、**默认优先级带 1000**（排在插件之后、core 之前）；
> ④ **超时两处都实现、默认 0 = 不限**（点的 `implTimeoutMs` / 调用方 `timeoutMs`），由调用方自行决定；
> ⑤ 命令行做成子命令 `agentxx_cli feature --list / --call / --dump-config`；
> ⑥ 新增**开发者模式**（`dev_mode`，启动期冻结）：开启才收集调用统计 / 派发记录 / 成功调用日志（§11.4）。
>
> 术语对应：musicxx 里的「埋点 / feature point」= 本设计的**功能点**。本设计沿用它的分层、
> 值缓存、优先级、置空与自省口径，按 agentxx 的插件框架（C ABI 接口表 + 协程 + IO 线程）落地。

需求原文：

```md
请参考 musicxx 的插件框架重构（extern-plugin-share-feature/plan.md），将功能埋点的处理统一起来，
仔细分析 agentxx 目前的架构设计，以及 musicxx 的插件框架重构中有哪些设计可以参考同步优化重构，
让 agentxx 也统一设计思路和代码实现。
```

| 部分 | 内容 | 章节 |
|---|---|---|
| **A. 功能点子系统** | 点 / 实现 / 调用三层；`provide`（取值）与 `decide`（裁决）两个类型；实现层 `plugin → core`，层内按优先级；显式置空；值缓存（不保留 / 只留最新 / 按身份 LRU） | §3 |
| **B. 对外开放调用** | 点声明 `callable` 才可被插件调用；`call()` 只读值缓存、不记置空、不改应用状态；三道保护（`caller`、全是调用方就不问插件层、同点同调用方重入 → `busy`） | §4 |
| **C. 插件自定义功能点** | 插件声明自己的点（命名空间限定 `plugin.<自己>.*`）、别的插件登记实现、任何一方调用；实现注册走功能点表本身，不新增钩子点 | §5 |
| **D. 钩子处理器清单与优先级** | 钩子从"只有计数、按装载顺序"变成"有清单、有优先级"；`register_hook_ex` + `list_hooks`，core 与插件都能读 | §6 |
| **E. 核心实现不再绑定"当前会话状态"** | 迁移后的核心实现必须由请求显式表达"算哪一份数据"（会话 / 消息 / 模型），只算数据、不写会话 | §7 |
| **F. 第一批功能点** | `agentxx.context.countTokens`、`agentxx.context.summarize`、`agentxx.tool.summarizeOutput`（逐个给请求、结果、实现、值缓存、调用约定） | §8 |
| **G. 跨边界接口** | 新接口表 `agentxx.agent.feature` v1（agent 领域表 9 → 10，总数 19 → 20）+ kit 糖 + **FFI 与命令行接入**（wire 消息、FFI 导出、子命令 `feature --list / --call / --dump-config`） | §9 |
| **H. 线程、超时、体积** | 全部异步（宿主 IO 线程 + 操作协议）；超时两处都实现、默认 0 = 不限（`implTimeoutMs` / 调用方 `timeoutMs`）；不提供同步等待入口；大结果一次拷贝 + 上限 | §10 |
| **I. 自省与排障** | `list_points` / `list_hooks` + 装配快照 / `--dump-diagnostics` 的 `features`·`hooks` 段；调用统计与成功调用日志**仅在开发者模式**下收集（§11.4） | §11 |
| **J. 与既有机制的分工** | 工具 / 钩子 / 事件 / 能力 / 事件总线服务 / 提示词 / 资源 / FFI 宿主工具各自保留什么、哪些被功能点吸收 | §12 |
| **K. 与 musicxx 的对照** | 逐条列出 musicxx 本轮与上一轮的设计项在 agentxx 的现状与处置（采纳 / 适配 / 暂不搬） | §13 |
| **L. 路线图** | 本轮不做的项（通知型功能点、压缩对外开放、插件点值缓存、界面展示、第二批点、核心侧钩子处理器） | §18 |

---

## 0. 一页结论

1. **问题**：agentxx 现有七条"某一方能提供功能"的通道（工具、钩子、事件、能力、事件总线服务、
   提示词贡献、资源与图节点），语义各不相同；核心功能（token 估算、上下文压缩、工具输出摘要、
   权限判定、消息修复、工具过滤）写死在中间件与节点里，插件既**提供不了**也**调用不了**，
   没有统一的清单、顺序、置空与调用口径；钩子的顺序也还是"装载顺序"的副产物。
2. **做法**：在 libagentxx 侧引入**功能点（feature point）子系统**（`agentxx::feature`），
   对外经接口表 `agentxx.agent.feature` v1 暴露给插件，并经 wire + FFI + 命令行暴露给宿主与脚本。
   功能点是"核心在关键位置主动调用的扩展点"，实现可以有多个，按链条顺序问，第一个给出值的生效。
3. **两个类型**：`provide`（取值：请求 → 值）与 `decide`（裁决：通过 / 拒绝 / 弃权）。
   埋点的动机有三类 —— 取一个值、裁决控制流、广播一次事件；**广播这一类已由事件表与 7 个固定钩子承担**
   （同一动机、不同实现），本轮不新增第三类型（枚举值 `2` 预留，协议上不封口）。
4. **埋点天生接受外部实现**：核心既然在这些位置留出了扩展点，就是希望外部参与 ——
   因此**没有"只允许核心实现"这类开关**（不设 `allowPluginImpl`）；`core` 实现只是"没人给值时的兜底"。
   是否允许被外部**主动调用**（`callable`）是另一件事，由点自己声明。
5. **实现分层与顺序**：agentxx 只有一套插件框架（`builtin://` 只是把同一个框架编进 libagentxx，
   行为与动态库装载一致），所以**只分两层**：`plugin`（插件与 FFI 宿主提供的外部实现）→
   `core`（libagentxx 自身实现）。层内按 `(priority 升序, 登记顺序)`，其中**默认优先级带**为：
   插件实现 `0`、**FFI 宿主实现 `1000`**（宿主本质是"直接改 agentxx / 把 agentxx 当库引入"，
   所以默认排在插件之后、core 之前）。顺序是声明出来的；`priority` 越界时裁剪到上下限并记警告。
6. **显式置空**：实现返回 `{"disable": true}` 表示"这次不再问后面的实现"；`ask` 会记下置空标记
   （清单里能看到 `disabledBy`），实现被摘除或插件禁用/卸载后自动还原；`call` 不记标记。
7. **值缓存**：每个点可以选择不缓存 / 只留最新 / 按身份 LRU（条数 + 字节双上限）；同一身份并发只跑一次实现
   （in-flight 去重）；记录产出方 `by`；产出方被摘除时按来源失效。**值缓存只在内存、不落盘、按 agent 实例隔离**；
   已经自带持久化的功能（工具输出摘要）不重复缓存。
8. **对外开放调用**：点自己声明 `callable` 与 `callDoc`（默认不可调；插件声明的点在其声明期间可调）。
   `call()` 与 `ask()` 的区别只有两条：**只读值缓存、不写值缓存**；**不记置空标记**。
   加上"不改调用方会话 / 不发界面提示 / 不落盘"，合成一条纪律：**调用只拿数据**。
9. **三道保护**：① 实现收到 `caller` 与 `viaCall`，可判断"这是不是我自己发起的"；
   ② 该点的实现全部属于调用方时，这一次不问插件层；③ 同一 `(点, 调用方)` 上一次没结束又来一次 → `busy`
   （把环路变成一次有边界的失败）。
10. **插件自定义功能点**：`define_point`（id 必须落在 `plugin.<自己>.<名字>`）→ 任何插件（含自己）
    用 `register_impl` 登记实现 → 任何一方用同一入口调用。声明与实现跟着插件实例走：禁用摘生效、
    卸载全摘、重新启用时由 `start` 事务重新声明。**不限制**注册数量（点的个数、每点的实现数都不设上限）。
11. **不新增钩子点**：agentxx 的钩子是**固定枚举**（7 个点），插件不得制造宿主不认识的钩子点。
    插件自定义功能点的"实现通道"就是功能点表本身（不像 musicxx 需要借一条通用通道钩子）。
12. **钩子补清单与优先级**：7 个点、载荷与"结果丢弃"的语义都不变；只是把顺序从"装载顺序"改成
    声明顺序（`priority`，越界裁剪 + 警告），并对外提供 `list_hooks`（插件读）与核心侧读数入口 ——
    core 与插件都能"读清单判断"（§6）。默认 `priority = 0` 时顺序与现状一致；同样**不限制**处理器数量。
13. **全异步 + 超时按需**：功能点的每次调用都是宿主 IO 线程上的操作（插件实现按已有 `execute_start`
    操作协议驱动，回调经 `notify` 回到 IO 线程）；**不提供同步等待入口**。超时**两处都实现、默认都 0 = 不限**：
    点的声明方可以给 `implTimeoutMs`（等单个插件实现，到点取消当次实现并按"没意见"继续链），
    调用方可以给 `timeoutMs`（等整次调用，到点取消当次实现并返回 `failed`）—— 谁等谁定，框架不替调用方猜。
14. **多方统一**：核心（C++ 直接调用）、插件（`agentxx.agent.feature` 接口表）、FFI 宿主
    （wire 消息 + FFI 导出）、命令行（子命令 `feature --list / --call / --dump-config`）、
    脚本（`jsonl` 同一份 wire 消息）、远程客户端（wire）都用同一套"点 / 实现 / 调用 / 清单"语义
    （§9.5、§9.6）。
15. **自省进同一份清单**：`list_points`（功能点）+ `list_hooks`（钩子）+ 装配快照 / `--dump-diagnostics`
    的 `features` 与 `hooks` 段：显示每个点能不能调、怎么调、谁在提供、谁生效、谁把它关掉了。
    其中"被调过几次、最近谁调、耗时多少"这类**记录数据只在开发者模式**（`dev_mode`，启动期冻结）下收集
    （§11.4）：关闭时不累加、不占内存，清单只给当前状态。
16. **与既有机制分工**（§12）：工具仍是"给模型调用的函数"；钩子仍是"生命周期观察"；事件仍是"通知"；
    能力与工具互调仍是"点名一个插件"；事件总线服务（`service.*`）是核心内部通道，**逐步被功能点吸收**。
17. **分 7 个阶段**：子系统骨架 → 第一批核心点迁移 → C ABI 与 kit → 对外开放调用与插件点 →
    钩子清单与优先级 → 命令行与 FFI 接入 → 第二批点与文档收尾。每阶段一个提交、各自可验证（§15）；
    本轮不做的项登记在 §18 路线图。

## 1. 需求与口径

### 1.1 需求 → 落地

| 需求 | 落地 | 章节 |
|---|---|---|
| 把功能埋点的处理统一起来 | 一个子系统、一份清单、一套顺序/置空/缓存/调用规则；散在各处的 `service.*` 服务与写死的核心功能逐步迁进来 | §3、§8、§12 |
| 参考 musicxx 的插件框架重构 | 逐条对照：采纳 / 适配 / 暂不搬，见对照表 | §13 |
| 插件能提供应用留出的功能 | 插件经 `register_impl` 为应用点登记实现（`plugin` 层最优先） | §3.3、§9 |
| 插件能调用应用留出的功能 | 点声明 `callable`，插件经 `call_point_async` 调用，参数与结果都走 JSON | §4、§9 |
| 插件能自定义功能点并被其他插件调用 | `define_point` / `undefine_point` + 同一调用入口 | §5 |
| 调用不能覆盖"最新值"、不能改应用状态 | `call()` 只读值缓存、不记置空、不写会话 / 不发提示 / 不落盘 | §4.2、§7 |
| 各方注册的实现都能参与 | 实现链按层与优先级依次问，异常只等于"没意见" | §3.3 |
| 钩子要能读清单与顺序 | 钩子处理器进有序注册表 + `list_hooks`（插件）与核心侧读数入口 | §6 |
| 能看出"谁在提供、谁生效、能不能调" | `list_points` / `list_hooks` + 装配快照 / 诊断 `features`·`hooks` 段 | §11 |
| 核心 / 插件 / FFI 宿主 / 命令行统一 | wire 消息 + FFI 导出 + 子命令 `feature --list / --call / --dump-config`，共用同一套语义 | §9.5、§9.6 |
| 超时可按需实现 | 两处超时都实现、默认 0 = 不限：`implTimeoutMs`（等实现）/ `timeoutMs`（等整次调用） | §4.4、§10 |
| 记录数据按需收集 | **开发者模式**（yaml `dev_mode`，启动期冻结）：开启才累计调用统计、派发记录与成功调用日志 | §11.4 |

### 1.2 已确认口径（本次按此实施）

1. **子系统归属**：注册表、值缓存、清单都在 libagentxx（`agentxx::feature`），随 `AgentContext` 一份
   （多 agent 实例互不影响，不使用全局变量）。
2. **两类型**：`provide` / `decide`；`notify` 预留枚举值但本轮不实现（理由见 §1.3 第 1 条，路线图见 §18）。
3. **层与顺序**：只分 `plugin` → `core` 两层，层内 `(priority 升序, 登记顺序)`；
   **默认优先级带**：插件实现 `0`、FFI 宿主实现 `1000`（后者默认排在插件之后、core 之前 ——
   宿主本质是"直接改 agentxx / 把 agentxx 当库引入"，与 libagentxx 是同一份代码的两个入口）。
   owner 前缀区分来源（`plugin:<名>` / `host:<名>` / `core:<域>:<实现>`）；
   `builtin://` 装载的插件与动态库插件同层、同默认带（同一套框架，只是编译方式不同）。
   `priority` **越界裁剪到上下限并记一条警告日志**，不拒绝登记。
4. **不设注册数量上限**：点的个数、每个点的实现数、每个实例的钩子处理器数都不设上限 ——
   非必要不加检查与限制；只保留必要校验（命名空间、id 非空、参数形状）与运行期保护（重入 `busy`）。
5. **"可调用"是点自己的声明**，不是全局开关；默认不可调。插件声明的点在被声明期间默认可调。
6. **埋点默认接受外部实现**：埋点就是核心主动调用的扩展点（取值 / 裁决 / 广播三类动机，§3.1），
   因此**没有"只允许核心实现"这类开关**；`core` 实现是"没人给值时的兜底"。
7. **`call` 零副作用**：不写值缓存、不记置空标记、不改调用方会话与上下文、不发界面提示、不落盘。
   实现内部派生"用完即弃"的临时子代理（例如核心压缩实现）算实现细节，不算修改应用状态（§8.2）。
8. **超时两处都实现、默认 0 = 不限**：
   `implTimeoutMs`（点的声明方，等**单个**插件实现；到点取消当次实现并按"没意见"继续链）、
   `timeoutMs`（调用方，等**整次**调用；到点取消当次实现并返回 `failed`）。
   各入口（插件糖 / FFI / 命令行 / `jsonl`）都接受调用方超时；**框架不替调用方猜超时**。
9. **插件自定义点只允许 `provide` 类型**（本轮）。`decide` 需要明确的合并策略与安全边界，
   等第一批应用点的裁决型跑稳再开放；`define_point` 收到 `decide` 直接拒绝并记日志。
10. **插件的点不写进核心点常量表**：核心点的 id 是稳定清单（代码里的常量 + 文档表格），
    插件的点是运行时数据，跟着插件生命周期来去。
11. **值缓存不落盘、不跨进程、不跨 agent 实例**；进程重启即空（插件点本轮固定不缓存，§18 路线图）。
12. **不搬"链式传值"**（把上一个实现的结果交给下一个继续处理）——与 musicxx 口径一致，理由见 §1.3。
13. **不做运行时权限校验**：功能点的声明、调用与自省不额外加权限层；插件之间的调用沿用现有框架口径
    （谁能加载、谁能注册由既有机制决定）。
14. **钩子补清单与优先级，但不改语义**：7 个固定点、载荷、结果丢弃的约定不变；只把"顺序"从装载顺序
    换成声明顺序，并提供清单（§6）。默认 `priority = 0` 时顺序与现状一致。
15. **开发者模式**（本轮新增）：`dev_mode`（yaml / FFI `devMode` / CLI `--dev`）**在启动时读取后冻结**，
    开启才收集调用统计、钩子派发记录与成功调用日志；关闭时热路径只多一次只读布尔判断，
    不分配统计结构、不保留历史（§11.4）。它**只控制"记录数据的收集"，不改变任何行为**。
16. **多方接入统一走 wire 与命令行**（§9.5、§9.6）：FFI 宿主、JSONL 脚本、远程客户端、本地子命令
    都经同一套语义读清单与调用功能点；插件侧仍是接口表。

### 1.3 明确不做的事

1. **不搬通知型（observe）**：agentxx 已有 `pluginxx.events` 事件表（`plugin.*` 主题、异步、无返回值）
   与 7 个固定钩子点，通知型功能点没有新增价值；将来真出现"应用广播、实现方各自处理、不等结果、
   还要进同一份清单"的用例，再按同一套点表加第三类（协议预留，不改已有字段；见 §18 路线图）。
2. **不用功能点承载"给模型调用的工具"**：工具的参数与结果面向模型（进提示词、要模式校验、有
   `parallel_safe` / `repeat_call_check` 这类工具语义），功能点面向程序；两者不合并。
3. **不让插件调用钩子**：钩子仍是宿主内部派发；插件想影响流程要走功能点（提供值 / 裁决）。
4. **不开放插件定义 `decide` 类型的点**（本轮）：裁决型的合并与安全边界先只用于应用留出的点。
5. **不做"插件直接把值写进值缓存"**：值缓存只由 `ask()` 与实现回答写入；调用方拿到值自己缓存（结果里给 `identity`）。
6. **不做注册数量与配额限制**：点的个数、每个点的实现数、每个实例的钩子处理器数都不设上限
   （非必要不加检查与限制）；只保留必要的合法性校验（命名空间 / id 非空 / 参数形状）与重入保护（`busy`）。
7. **不做取消语义的对外承诺**：`call` 超时/放弃后丢弃晚到的结果即可（宿主会取消当次实现）；
   调用方要真正中断目标插件请另开机制（与 musicxx §6.5 同口径）。FFI / 命令行侧同样不做取消，
   晚到结果按 `reqId` 丢弃。
8. **不改事件总线服务的现状定义**：`service.*` 请求-响应服务在迁移期内保留（`service.token.count` 等）；
   迁移完成后，被功能点取代的服务再按单独的提交删除（见 §8.1 的"同步快路径"处理）。
9. **不给客户端（client 侧）加功能点**：client 侧插件的接口表不变；将来若出现"界面侧共享功能"的真实用例再评估。
10. **不做跨 agent 实例的功能点调用**：功能点是实例内部机制；跨实例/跨进程查询继续走
    `service.crossagent`（既有语义，本轮不动）。
11. **不改钩子的载荷与结果语义**：只加清单与优先级（§6），不加返回值合并、不增加钩子点、不给钩子"置空"。
12. **不做功能点清单的界面展示**：本轮只做快照、诊断、CLI 与 FFI 出口（§18 路线图登记 TUI 展示）。

## 2. 现状勘察

### 2.1 现在有哪些"能提供功能"的通道

| 方向 | 机制 | 形态 | 能不能承载"应用留出的功能" |
|---|---|---|---|
| 核心内部 | 事件总线请求-响应 `service.*`（`agentxx/event/events.h`） | `co_await bus->request<Req,Resp>(topic)`（带可选超时） | ✓ 现在核心功能都靠它（压缩、权限、子代理、中断），但**插件看不到、不能替换、没有清单** |
| 核心内部 | 同线程同步服务（`registerService` / `callService`，如 `service.token.count`） | 同线程直调、无返回值包装 | ✓ 用于高频热路径（TPS 计算）；**同步调用，插件实现无法参与** |
| 应用 → 插件 | 钩子（`agentxx.agent.hooks`，7 个固定点） | 中间件链顺序 `co_await` 派发；载荷只有 `{sessionId, point}`；返回值丢弃；单插件单点一个处理器 | ✗ 只能"看"：无返回值、无优先级（顺序 ≈ 装载顺序）、清单里只有计数 → 本轮补清单与优先级（§6） |
| 插件 → 插件 | 能力表 `invoke_capability_async` | 点名 + 单实现（`pluginxx.capabilities`，SDK 有 `co_await invoke_cap`） | ✗ 点名语义，接不上"按链条问"，也没有值缓存与清单 |
| 插件 → 插件 | 工具表 `call_tool_async` | 点名一个工具、等一次结果 | ✗ 同上（工具本身是给模型的） |
| 插件 → 所有人 | 事件表 `pluginxx.events`（主题自动加 `plugin.` / `client.` 前缀） | 异步发布订阅，无返回值 | ✗ 通知通道，不是"要一个值" |
| 插件 → 应用 | 提示词表 `agentxx.agent.prompt`（贡献 + 按键合成）、资源表、图节点表 | 注册 / 贡献 | ✗ 各自领域专用，不是通用功能点 |
| FFI 宿主 → agent | `agentxx_ffi_tool_register`（宿主工具，见 ffi.md §4.9） | 模型调用 → `EVT_HOST_TOOL_CALL` → `tool_respond` | ✗ 只能作为工具给模型；要让宿主**提供或调用功能**，走本设计的 FFI 接入（§9.5） |

### 2.2 写死在核心里的功能（迁移清单）

| 功能 | 现状落点 | 形态 | 迁移去向 |
|---|---|---|---|
| 文本 Token 估算 | `SummarizationMiddlewareHandle::countTokensForUtf8Str` / `countTokens`；经 `service.token.count` 同步服务供 TPS 用 | 同步 | `agentxx.context.countTokens`（provide）；TPS 保留同步快路径直连核心实现（§8.1） |
| 上下文压缩 | `SummarizationMiddlewareHandle`（自动 + `compactSessionContext` 手动；`doSummarizeWithLLM` 经子代理） | 中间件内实现 | `agentxx.context.summarize`（provide）；编排（提示消息 / 写回 / 硬截断兜底）留在中间件（§8.2） |
| 工具输出自动摘要 | 工具 `AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY` + `summarizationToolHandles` + share store 卸载 | 节点内实现 | `agentxx.tool.summarizeOutput`（provide）（§8.3） |
| 权限判定（工具调用 / 路径三态） | `PermissionMiddlewareHandle::checkToolPermission` + `service.permission.check` / `service.permission.reverify` | 请求-响应服务 | 后续批次：`agentxx.permission.decide`（decide，只允许收紧，§8.4） |
| 消息修复（空消息 / UTF-8 / 相邻 user 合并 / 哈希与条数检查） | `ModelCallWrapNode::baseRun` + `xx_messageCheckInfo` | 节点内实现 | 后续批次：`agentxx.context.repairMessages`（provide，不可调）（§8.4） |
| 工具集过滤（白名单） | `AgentConfig::enableToolFiltering` + `toolWhitelist`（`base_agent.cpp`） | 配置 + 装配期过滤 | 后续批次：`agentxx.tool.select`（provide，不可调）（§8.4） |
| 记忆文件 / 技能注入 | `memory_file` / `skill` 中间件（写稳定 system 段） | 中间件内实现 | 后续批次：`agentxx.memory.retrieve` / `agentxx.skill.match` |
| 子代理执行 | `service.subagent(.batch/.execute)`（宿主应答） | 请求-响应服务 | 后续批次：`agentxx.subagent.run`（provide） |
| 模型选择 / 用量 | 客户端 MRU + `AgentConfig` / `KeyMetrics` | 配置 | 暂不迁移（选择属界面偏好） |

### 2.3 四个关键事实（设计就是由它们推出来的）

1. **宿主只认固定钩子表**：`AgentxxPluginHookPoint` 是编译期枚举（7 个点），
   `register_hook` 校验 `point ∈ [0, COUNT)`，越界直接拒绝。
   **结论**：插件自定义功能点**不能**用"每个点一条钩子"的做法，必须由功能点表自己承载实现注册（§5）。
2. **钩子派发没有优先级、没有返回值**：`PluginMiddlewareHandle::dispatch` 只把 `{sessionId, point}` 交给
   插件，`awaitHostPluginOp` 的结果被丢弃；插件首次 `registerHook` 时宿主才把它的中间件句柄
   `push_back` 到中间件链（见 `PluginManager::registerHook`），因此顺序 ≈ 插件装载顺序，
   声明不了优先级。失败只记一条警告日志，链继续。
   **结论**：① 功能点必须自己带"层 + 优先级 + 结果"三件事（§3.3）；
   ② 钩子要能"读清单、排顺序"，就得把处理器抽到一份有序注册表里、由一个派发器执行（§6）。
3. **插件的事件主题被加上 `plugin.` / `client.` 前缀**（`PluginManager::qualifyEventTopic`），
   因此插件**无法**订阅或发布 `service.*` 主题。
   **结论**：核心现有的 `service.*` 服务对插件是不可见的；要让插件参与或调用这些功能，只能经功能点这条新通道（§12）。
4. **插件调用一律经宿主 IO 线程 + 操作协议**：领域表的入口先把请求投递到 IO 线程（`ioCallSync` 系列），
   异步部分用 `execute_start` / `impl_cancel` + `notify` 回调，回调保证在 IO 线程发布。
   **结论**：功能点的插件实现直接复用这套协议，不引入新的线程模型，也不允许同步等待版本（§10）。
   FFI 宿主与远程客户端同理：客户端 → 服务端的调用一律经 wire 异步消息（§9.5）。

### 2.4 上一轮的设施（本轮直接复用）

* **注册可逆与统一清单**：`PluginManager::registrationInventory` + `detachDomainRegistrations` /
  `clearDomainRegistrations` + `PluginListView` 的诊断字段 —— 功能点的声明与实现登记按同一模式记账（§9.2）。
* **操作驱动器**：`OpCore` / `OpDrive` / `awaitPluginOp`（`pluginxx/runtime/op_driver.h`）——
  插件实现的调用与取消直接复用，不新写一套等待逻辑。
* **协程驱动桥与 kit 原语**：`Task` / `invoke_cap` / `offload` / `sleep` / `spawn` / `PollOneBridge` ——
  kit 侧的功能点实现糖可以用与工具一样的形态写协程（§9.3）。
* **装配快照与诊断**：`buildRuntimeSnapshot` / `renderAssemblySnapshot` / `buildDiagnosticsText`
  （`--dump-diagnostics`）—— 功能点清单直接进这两处，不再另开一套导出（§11）。
* **接口协商三层机制**：`plugin_interfaces.h` 的 `plugin_interfaces::*` 常量 +
  `checkInterfacesForSide` —— 新表要在这一层登记名字（§9.1）。
* **启动期只读标记的先例**：`agentxx::agent::AgentConfigStatic::enableBenchmark`（默认关闭，
  基准程序启动时打开）—— 本设计的**开发者模式**（§11.4）用同一做法：配置里声明、启动时镜像到
  进程级只读标记，库内热路径只读这一个标记，避免到处取 `AgentContext`。

---

## 3. 设计 A：功能点子系统

### 3.1 什么是埋点：点 / 实现 / 调用三层

**埋点（功能点）是核心代码在关键位置主动调用的扩展点**。核心在三种动机下调用它：

| 动机 | 核心这边 | 点这边 | 本设计的类型 |
|---|---|---|---|
| **取一个值** | "这一步需要 X（token 数 / 摘要 / 校正后的消息数组）" | 谁实现都行，第一个给出值的生效 | `provide` |
| **裁决控制流** | "这一步要不要继续 / 怎么继续" | 外部可以参与决定（本轮只采纳"收紧"方向） | `decide` |
| **广播一次事件** | "这里发生了一件事" | 每个实现各自处理，不等结果 | 已由事件表 + 7 个钩子承担（不新增类型） |

由此推出两条口径：

* 核心既然在关键位置留出了扩展点，就**默认期望外部实现参与** —— 没有"只允许核心实现"的开关
  （不设 `allowPluginImpl`）；`core` 实现只是"没人给值时的兜底"。
* 是否**允许被外部主动调用**（`callable`）与"有没有实现"是两件不同的事：前者是点的声明（§4.1），
  后者由实现链决定。

三层结构：

```text
             ┌──────────────────── 埋点 / 功能点 (Point) ────────────────────┐
             │ id / 类型 / 展示名 / 说明 / callable / callDoc / 值缓存策略 / 身份规则 │
             └───────────────┬───────────────────────────────┬───────────────┘
                             │                               │
                 实现 (Impl) 链                          调用 (Call)
     plugin（插件 / FFI 宿主）→ core，层内 (priority, 登记序)   ask()（核心自己取）/ call()（外部主动调用）
```

* **一个点是一个对象**（`ProvidePoint<TReq,TValue>` / `DecidePoint<TReq,TVerdict>`），
  它自己持有实现列表、值缓存、统计与说明；不把"点"散成几个静态变量。
* **点的请求与结果是强类型**（核心侧）；**跨插件边界一律 JSON**（请求由点编码、结果由点解码并校验）。
* **调用方永远不会拿到"一个类型字段再 switch"**：用哪个类由代码决定；`type` 只用于展示与日志。

### 3.2 类型：provide 与 decide

| | `provide`（取值） | `decide`（裁决） |
|---|---|---|
| 问题 | "这个功能给我一个值" | "这件事可以继续吗 / 要怎么处理" |
| 回答 | 值（任意 JSON 形状，由点定义） | `abstain`（弃权）/ `deny`（拒绝，带理由） |
| 合并 | 第一个给出合法值的实现生效（`firstNonNull`） | 策略由点声明：`firstDecisive`（第一个非弃权生效）或 `anyVeto`（任一 `deny` 即拒绝，其余弃权） |
| 终止 | 显式置空 `{"disable": true}` | 明确的非弃权回答 |
| 值缓存 | 可选（不保留 / 最新 / 按身份） | 不使用（裁决不缓存） |
| 本轮用例 | token 计数、上下文压缩、工具输出摘要 | （后续批次）权限收紧、消息修复的拒绝分支 |

裁决型的纪律（写进点的说明与实现文档）：**插件实现只被采纳"收紧"方向**——
`deny` 生效，`abstain` 弃权；插件返回 `allow` 会被忽略并记一条日志（应用点是否需要"允许"语义由点的
声明决定，本轮不开放）。这样"插件不能替用户放行"是协议层的性质，而不是靠实现自觉。

### 3.3 实现层与顺序

**层只有两个**：`plugin` 与 `core`。

* agentxx 只有**一套插件框架**：`builtin://<name>` 只是把同一个插件（同一份入口、同一套接口表、
  同一条操作协议）编进 libagentxx，行为与按路径装载动态库一致；因此不单列"内置插件"层
  （musicxx 有两套互不兼容的框架才需要区分）。清单里可以顺带显示装载方式（`dynamic` / `builtin`），
  **仅作展示，不影响顺序**。
* **FFI 宿主登记的实现在同一层（`plugin`），但默认优先级带不同**：宿主侧本质上是"直接改 agentxx
  或把 agentxx 当库引入"的开发方式，与 libagentxx 是同一份代码的两个入口，所以它的实现默认排在
  **插件之后、core 之前**。

排序键（从小到大）：

```text
(层: plugin = 0, core = 1) → (层内 priority 升序) → (seq 登记顺序)

plugin 层内的默认 priority 带：
  · 插件实现（动态库 / builtin）  默认 0
  · FFI 宿主实现                  默认 1000

=> 默认顺序：插件 → FFI 宿主 → core（显式声明 priority 可以覆盖这条默认带）
```

* **顺序必须声明**，不能是装载顺序的副产物；同优先级时才退回登记顺序（= 登记 / 装载先后）。
* `priority` **越界时裁剪到上下限并记一条警告日志**，不拒绝登记（少检查、少拒绝）。
* **不限制注册数量**：点的个数、每个点的实现数都不设上限；插件想注册多个实现是它自己的事
  （同一实例、同一个点仍只保留一个实现，重复登记为覆盖，见 §5.2）。
* 每次询问一个实现；它给出的回答**不合法**（JSON 解码失败 / 形状不符）等价于"没意见"，
  必须记一条警告并继续问下一个实现（不允许用异常终止链条）。
* **不链式传值**：下一个实现看不到上一个的结果（与 musicxx 同口径；真实用例出现前不加）。

### 3.4 显式置空（disable）

* 实现回答 `{"disable": true}`：本次调用以 `disabled` 结束，**不再问后面的实现**。
* `ask()`：记下置空标记（含 `disabledBy`，用于清单显示）；标记在以下时刻清除：
  产出方实现被摘除 / 产出方插件禁用或卸载 / 点被重新声明。
* `call()`：只影响本次结果，**不记标记**（下次 `ask()` 照旧问实现链）。
* 置空是"这一次到此为止"，不是"永久关闭这个功能"；要看"谁把它关掉了"读清单（§11）。

### 3.5 值缓存

| 策略 | 用途 | 说明 |
|---|---|---|
| `None`（默认） | 结果进会话 / 结果很大 / 结果天生每次不同 / 已自带持久化 | 不缓存 |
| `Latest` | 只关心"最近一次算出来的值" | 单槽 |
| `ByIdentity` | 同一份输入会被反复问（token 计数） | 按 `identity` 存若干条（条数 + 字节双上限） |

硬要求（与 musicxx §3.5 对齐，落地时按 agentxx 的实现写注释）：

1. **同一身份并发只跑一次实现**（in-flight 去重）：并发的 `ask` / `call` 共用一次计算；
   谁发起决定要不要写值缓存（`ask` 写、`call` 不写，见 §4.2）。
2. **记录产出方 `by`**（`plugin:<名>` / `host:<名>` / `core:<域>:<实现>`），用于自省与失效。
3. **按来源失效**：产出方实现被摘除 / 插件禁用卸载时，删除它产出的条目（`by` 匹配）。
4. **条数 + 字节双上限**：达到上限按最近最少使用淘汰；`max_bytes` 是硬上限（单条超限不入缓存）。
5. **只在内存、按 agent 实例隔离、不落盘**；进程关闭即清空。
6. **插件声明的点本轮固定不缓存**（策略 `None`）；将来是否按点声明缓存见 §18 路线图。
7. **已经自带持久化的功能不重复缓存**：工具输出摘要的原文由 share store / 会话库管理，
   点本身不缓存、不设上限（§8.3）。

### 3.6 身份（identity）

身份是"这次要算的是哪一份输入"的字符串，用于值缓存与 in-flight 去重，也是给调用方自己缓存用的线索。

* 由点声明身份规则（`identityOf(request)`），调用方可以在参数里显式覆盖（`identity` 字段）。
* 例子：
  * `agentxx.context.countTokens` → `model | kind | fnv1a64(text)`；
  * `agentxx.context.summarize` → `sessionId | messagesVersion | model`（值缓存 `None`，只用于去重）；
  * `agentxx.tool.summarizeOutput` → `toolCallId | fnv1a64(content) | maxTokens`。
* 身份**不参与参数校验**；不含"这次要多长"这类与该份输入无关的参数（要不同参数就显式给不同身份或 `refresh`）。

### 3.7 一处实现、两处入口：ask 与 call

把执行逻辑写成一个函数，入口只有两个（与 musicxx §3.2 相同的做法，不用全局开关传状态）：

```cpp
// agentxx::feature::ProvidePoint<TReq,TValue>（节选）
asio::awaitable<AskResult<TValue>>
    ask(const TReq& req, AskOptions opts = {});   // 应用自己取：读值缓存 → 跑链 → 写值缓存、记置空

asio::awaitable<CallResult<TValue>>
    call(const TReq& req, CallOptions opts = {});  // 主动调用：读值缓存（可选）→ 跑链 → 不写、不记
```

内部：

```cpp
struct ResolveFlags {
    bool        persist;         // 拿到值写不写值缓存（ask = true，call = false）
    bool        rememberDisable; // 置空记不记标记（ask = true，call = false）
    std::string caller;          // 空串 = 应用自己
    std::string identity;        // 空串 = 由点自己算
    bool        refresh;         // true = 跳过值缓存重算
};
```

* **`ask()` 是应用自己的入口**：中间件 / 节点 / 工具用它；`caller` 为空；写值缓存、记置空。
* **`call()` 是主动调用入口**：插件、后续的 CLI / FFI 用它；只读值缓存、不记置空。
* 两者共用同一条实现链与同一份异常隔离、并发合并、置空裁决逻辑，因此"无论各方注册了什么处理器"，
  调用都能拿到当前最优结果。

---

## 4. 设计 B：对外开放调用

### 4.1 声明：`callable` 与 `callDoc`

* 应用留出的点在声明时给出 `callDoc`（参数有哪些、结果是什么，一段纯文本）；
  `callable == false` 时调用直接返回 `not_callable` + 说明。
* 插件声明的点在存活期间恒为可调（`callDoc` 用 `args_doc` / `result_doc` 拼接）。
* 自省里两者都会显示 `callable` / `callDoc`（§11），文档与命令帮助共用同一份文本。

### 4.2 调用语义：`call()`

```text
call(req, caller, opts):
  1. 点可调用？                       否 → not_callable（带说明）
  2. 请求参数合法？                   否 → bad_args（带说明）
  3. identity = opts.identity 或 identityOf(req)
  4. 三道保护（§4.3）：调用方自己的实现、环路、重入
  5. 命中值缓存（refresh = false）？    → 直接返回（fromCache = true，by = 仓里记的产出方）
  6. 同一身份正在算？                 → 复用（不重复算）
  7. 依次问实现链（plugin → core）；单个实现超过 implTimeoutMs 就取消它并继续问（默认不限）
  8. 都没有                            → no_impl
  ※ 任何一步拿到值都不写值缓存；拿到 disable 也不记标记；不写会话、不发提示、不落盘
  ※ 调用方给了 timeoutMs 时，到点取消当次实现并返回 failed（默认 0 = 不限）
```

必须写进代码注释的三条规则：

1. **写不写值缓存由"发起这次计算的一方"决定**：`ask` 发起 → 写；`call` 发起 → 不写。
   同一身份并发共用一次计算时按"发起方"算（`ask` 发起就写，`call` 只是搭顺风车）。
2. **置空标记同理**：`call` 看到 `disable` 只影响本次。
3. **身份只含"这份输入"**，不含"这次要多长"；要不同长度/参数请自己给 `identity` 或 `refresh`。

### 4.3 三道保护

| 保护 | 规则 | 为什么 |
|---|---|---|
| ① 载荷带 `caller` / `viaCall` | 实现收到的调用上下文里有 `caller`（插件名，空串 = 应用自己）与 `viaCall`（true = 外部主动调用） | 实现要能判断"这是不是我自己发起的"，也要能区分"应用取值"和"外部调用"（零副作用契约只对后者严格） |
| ② 全是自己就不问插件层 | 该点的插件实现全部属于 `caller` 时，这一次跳过插件层（插件点则直接 `no_impl`） | 避免"把自己的实现又问回自己"的无用往返（压缩这类重活尤其值钱） |
| ③ 同一 `(点, 调用方)` 不可重入 | 上一次没结束又来一次相同 `(id, caller)` → 立即 `busy`（不排队） | 把任何环路（A→P→A→P…）变成一次有边界的失败，而不是无限递归或死等 |

三道保护只影响 `call()`；应用自己的 `ask()` 没有 `caller` 概念，不受影响。

### 4.4 结果与错误码

```cpp
enum class CallError : int32_t {
    None = 0,     // ok
    NotCallable,  // 点没有对外开放调用
    BadArgs,      // 参数不合法
    NoImpl,       // 现在没有实现（没有任何实现给出值）
    Disabled,     // 被某个实现显式置空
    Busy,         // 同一 (点, 调用方) 上一次还没结束
    Failed,       // 实现抛异常 / 结果不合法 / 调用方自己的超时
};

struct CallResult<TValue> {
    CallError  error = CallError::None;
    std::optional<TValue> value;
    std::string by;        // 谁给的（core:<域>:<实现> / plugin:<插件名>）
    std::string identity;  // 本次身份（调用方可据此自己缓存）
    bool        fromCache = false;
    int64_t     ms        = 0;
    std::string message;   // 失败时一定有可读说明
};
```

给插件的 JSON 形态（调用结果，与 musicxx 同形）：

```jsonc
// 成功
{ "ok": true, "id": "agentxx.context.countTokens", "identity": "m1|text|8f3a…",
  "value": { "tokens": 1234 }, "by": "core:context:tokenEstimate",
  "fromCache": false, "ms": 3 }
// 失败
{ "ok": false, "id": "agentxx.context.countTokens", "error": "no_impl",
  "message": "现在没有 token 计数实现：检查插件是否加载，或看功能点清单确认谁注册了实现" }
```

错误码的字符串（对外契约的一部分，改动即改契约）：`not_callable` / `bad_args` / `no_impl` /
`disabled` / `busy` / `failed`。

**超时（两处，都实现、默认都 0 = 不限）**：

| 参数 | 谁给 | 作用范围 | 到点怎么处理 |
|---|---|---|---|
| `implTimeoutMs` | 点的声明方（`PointOptions`） | 等**单个**插件实现 | 取消当次实现（`impl_cancel`），按"没意见"继续问下一个实现；不判定整个点失败 |
| `timeoutMs` | 调用方（`CallOptions` / 插件糖 / wire / FFI / 命令行） | 等**整次**调用 | 取消当次实现并返回 `failed`，`message` 写清限时值（`"timeout after 5000ms"`） |

* 两者互不替代：前者防"某个插件实现卡住整条链"，后者是"调用方自己不想等"。
* **生效值**：宿主等某个插件实现的上限 = 点的 `implTimeoutMs` 与该实现自报的 `default_timeout_ms` 中
  **非 0 的较小者**；两者都为 `0`（默认）= 不限。
* **默认都不限**（`0`）：不给超时也能跑完；要限时由调用方自己决定（脚本 / 宿主 / 客户端各按自己的耐心）。
* 超时只是"不再等"：宿主取消当次实现；实现晚到的结果直接丢弃，不重放、不落库。

---

## 5. 设计 C：插件自定义功能点

三件事：**声明 → 实现 → 调用**。

### 5.1 声明：`define_point` / `undefine_point`

```jsonc
// define_point 的参数（C ABI 用结构体，语义与 JSON 一致）
{ "id": "plugin.my_plugin.beat",          // 必须 plugin.<本实例插件名>.<名字>
  "type": "provide",                      // 本轮只允许 provide（decide 被拒绝并记日志）
  "title": "节拍检测",                     // 展示名（缺省用 id）
  "depict": "算出这段音频的 BPM 与节拍时刻",  // 一句话说明
  "args": "path（本地文件路径），可选 minBpm/maxBpm",
  "result": "{bpm, beats:[ms,…], confident}" }
```

* **命名校验**：id 必须以 `plugin.<自己>.<` 开头、非空、不含空白字符；不合法直接拒绝并记日志。
  同一 id 重复声明 = 覆盖（记一条日志，方便排查两个插件抢同一个点）。
* **不设数量上限**：一个插件可以声明任意多个点（非必要不加检查与限制）。
* **生命周期**：声明跟着插件实例走——禁用摘生效、卸载全摘；重新启用时由 `start` 事务重新声明
  （因此不持久化）。`undefine_point` 给"运行期自己收回"用。
* **等实现超时**（`impl_timeout_ms`，可选，默认 `0 = 不限`）：声明方可以给自己这个点的插件实现
  设一个上限，到点取消当次实现并按"没意见"继续链（§4.4）。

### 5.2 实现：`register_impl`

任何插件都可以为**任何点**（应用留出的点、别人声明的点、自己的点）登记实现，条件是：

* 埋点默认接受外部实现（没有"只允许核心实现"的开关，见 §3.1）；登记被拒只有两种情况：
  点不存在（应用点未装配 / 插件点未 `define_point`）、实例正在关闭或已禁用；
* 登记的 `point_id` 必须已经声明（应用点在装配期声明，插件点必须先 `define_point`）；
* 同一 `(点, 插件实例)` 只保留一个实现（重复登记 = 覆盖，记日志）；
* **不设数量上限**：同一个点可以被任意多个实例实现，一个实例也可以为任意多个点提供实现；
* `priority` 越界不是拒绝，而是**裁剪到上下限并记一条警告**（默认带见 §3.3：插件 0、FFI 宿主 1000）；
* 等实现超时：点声明方给 `impl_timeout_ms`，实现方可以自报 `default_timeout_ms`，
  宿主取两者中非 0 的较小值作为等该实现的上限；都为 `0`（默认）= 不限（§4.4）。

实现体与工具同一套操作协议：

```c
void*(PLUGINXX_CALL* impl_start)(
    void*                         user_data,
    const PluginxxStringView*     point_id,
    const PluginxxStringView*     call_json,   // 调用上下文（见下）
    const PluginxxOperatorNotify* notify,      // 完成/失败/取消都经它回到宿主 IO 线程
    PluginxxString*               error_out);  // 非要拒绝这次调用时写这里（一般不用）
void(PLUGINXX_CALL* impl_cancel)(void* user_data, void* op);
```

实现收到的调用上下文（JSON）：

```jsonc
{ "point": "agentxx.context.countTokens",
  "args":    { "text": "…", "model": "m1" },   // 调用方原样给的参数（插件点里面就是全部信息）
  "request": { "text": "…", "model": "m1" },   // 应用点解析后的规范请求（插件点为空对象）
  "caller":  "my_plugin",                      // 空串 = 应用自己
  "viaCall": true,                             // true = 外部主动调用（零副作用契约）
  "identity": "m1|text|8f3a…",                 // 本次身份（实现可据此自己缓存）
  "sessionId": "s1" }                          // 规范请求里带的会话（可空）
```

实现的回答（JSON 文本）：

| 回答 | 含义 |
|---|---|
| `{"value": <任意 JSON>}` | 给出了值（`value` 为 `null` 也算"算过了，结果是空"） |
| `{"verdict": "deny", "reason": "…"}` | 裁决型：拒绝（`abstain` 表示弃权） |
| `{"disable": true}` | 取值型：显式置空（不再问后面的实现） |
| `{}`（空对象）或 status = 失败 | 没意见（等于不提供），继续问下一个实现 |

### 5.3 调用：与应用点同一个入口

* 调用方不关心点是谁声明的：`call_point_async(point_id, args_json, …)` 对所有点一致。
* 调用一个**插件点**时，宿主按实现链问（plugin → core，插件点的"core 层"为空）；
  插件的实现通常就是声明方自己。
* 插件点的结果不缓存（点可声明值缓存策略，但本轮插件点固定 `None`；调用方要缓存用结果里的 `identity`）。
* 插件点没有实现时返回 `no_impl`，说明里提示"先看功能点清单确认谁在实现"。

---

## 6. 设计 D：钩子处理器清单与优先级

钩子（`agentxx.agent.hooks`，7 个固定点）是"核心在某一步通知所有关心的人"的机制。
本轮**不改它的语义**（载荷仍是 `{sessionId, point}`、结果仍丢弃、仍是 7 个点），只补两件"能读、能排"的事。

### 6.1 有序注册表 + 单一派发器

* 宿主维护一份**有序注册表**：`point → 处理器列表`，条目为
  `{handle, 所属实例, point, priority, seq, ownerTag, depict, enabled, load}`，按 `(priority 升序, seq)` 排序；
  `seq` 是登记顺序计数器。
* 中间件链上**只挂一个宿主级派发器**（`PluginHookDispatchHandle`：首次登记时插入、没有处理器时移除），
  它按注册表顺序依次执行处理器：同一个点的处理器串行、都在宿主 IO 线程、异常只记日志继续
  （与现状完全一致）。旧的"每插件一个中间件句柄、按链顺序执行"随之移除。
* 层级与顺序：`plugin` 层（插件 / FFI 宿主登记的处理器）按 `(priority 升序, seq)`，其后是 `core` 层
  （核心自己登记的处理器，owner 前缀 `core:<模块>`；本轮不新增核心处理器）。与功能点同一口径：
  **默认优先级带** = 插件 `0`、FFI 宿主 `1000`（宿主默认排在插件之后、core 之前），显式声明可覆盖；
  `priority` **越界裁剪到上下限并记一条警告**。
* **不限制处理器数量**：一个实例可以在同一个点登记任意多个处理器（各自 `handle`），
  也可以登记任意多个点 —— 非必要不加检查与限制。
* **默认行为不变**：不声明 `priority`（= 0）时顺序就是登记顺序 = 插件装载顺序，与现状一致 ——
  这是本节的"行为不变"验收点。

### 6.2 C ABI：`register_hook_ex` 与 `list_hooks`

`AgentxxPluginHooksIface` 尾部追加三项（表版本仍是 1，靠 `struct_size` 守卫：插件报的 `struct_size`
小于新增后的尺寸时按"没有这三项"处理，老插件只看到 v1 的那两项、行为不变）：

```c
typedef struct AgentxxPluginHookSpecEx {
    uint32_t           struct_size;   /* sizeof(...); 0 = 当前布局 */
    int32_t            point;         /* AgentxxPluginHookPoint */
    int32_t            priority;      /* 层内顺序, 小者先 (默认 0) */
    int32_t            flags;         /* 预留 (AGENTXX_PLUGIN_HOOK_FLAG_*) */
    PluginxxStringView owner_tag;     /* 展示归属标签 (可空; 清单里显示) */
    PluginxxStringView depict;        /* 一句话说明 (可空; 清单里显示) */
    void*(PLUGINXX_CALL* hook_start)(
        void*                         user_data,
        int32_t                       point,
        const PluginxxStringView*     node_input_json,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out);
    void(PLUGINXX_CALL* hook_cancel)(void* user_data, void* op);   /* 可为 NULL */
    void* user_data;
} AgentxxPluginHookSpecEx;

/* 追加到 AgentxxPluginHooksIface 尾部 (struct_size 守卫) */
int32_t(PLUGINXX_CALL* register_hook_ex)(
    const PluginxxHost* host, const AgentxxPluginHookSpecEx* spec, int64_t* out_handle);
int32_t(PLUGINXX_CALL* unregister_hook_ex)(const PluginxxHost* host, int64_t handle);
int32_t(PLUGINXX_CALL* list_hooks)(const PluginxxHost* host, PluginxxString* out_json);
```

* v1 的 `register_hook` / `unregister_hook` 语义不变（同一实例同一个点一个处理器、`priority = 0`、覆盖式）；
  `register_hook_ex` 允许同一实例在同一个点登记**多个**处理器（各自 `handle`），
  用 `unregister_hook_ex(handle)` 精确撤销。
* kit 侧糖：`hookEx(ctx, point, fn, HookOptions{.priority = …, .ownerTag = …, .depict = …})` 返回句柄；
  `listHooks(ctx)` 读清单。
* 拒绝登记的情形只有：`point` 越界、实例正在关闭 / 已禁用。
  `priority` 越界（超出允许范围，如 ±1000）**裁剪到上下限并记一条警告日志**，不拒绝登记；
  处理器数量不设上限。

### 6.3 清单形状（`list_hooks` 与诊断共用）

```jsonc
{ "count": 7,
  "points": [ {
      "point": 0, "name": "AGENT_START",
      "handlers": [
        { "handle": 7, "layer": "plugin", "owner": "plugin:my_plugin", "ownerTag": "…",
          "priority": -10, "seq": 3, "enabled": true, "depict": "…", "load": "dynamic" },
        { "handle": 9, "layer": "core", "owner": "core:pluginmanager", "priority": 0,
          "seq": 0, "enabled": true } ] } ] }
```

* `layer` / `owner` 的取值与功能点一致（§3.3）：`plugin:<插件名>` / `host:<宿主名>` / `core:<模块>`。
* `load`（`dynamic` / `builtin`）只说明插件是怎么装载的，**不影响顺序**。
* 读取入口：插件侧 `list_hooks`；core 侧 `PluginManager::hooksJson()` 与 `handlersOf(point)`
  （内部使用，并进装配快照与 `--dump-diagnostics`）。**清单只读**：不参与派发判定，
  派发只看注册表当前状态。
* 每次派发失败仍只记一条警告日志（与现状一致）；**派发记录**（每次派发的点、处理器顺序、耗时）
  与对应的日志**只在开发者模式**（§11.4）收集，用于排查"为什么这个处理器先跑"。

### 6.4 生命周期与记账

* 处理器跟着实例走：插件禁用 → 该实例的处理器不生效（派发时跳过）；卸载 → 全部摘除；
  重新启用时由 `start` 事务重新登记（与工具 / 权限声明 / 功能点实现同一套规则）。
* `RegistrationInventory::hooks` 改为"注册表里该实例生效的处理器数"；
  `middlewareAttached` 在钩子这条路上不再有意义（派发器是宿主级单例），实现时按新语义改名或删除，
  并同步装配快照与诊断输出。
* `PluginListView` 增加 `hookPriorities`（或 `hookCount` + 首条优先级）便于一眼看出顺序。

### 6.5 不做的事

* 钩子返回值合并（仍丢弃结果）：要"要一个值 / 一次裁决"请用功能点（§3）。
* 钩子点增量（仍 7 个固定点）与钩子的"置空"语义：钩子不是功能点，不参与功能点实现链。
* 钩子顺序的运行时调整：改优先级 = 重新登记（`unregister_hook_ex` + `register_hook_ex`）。

---

## 7. 设计 E：核心实现不再绑定"当前会话状态"

musicxx 本轮设计 C 的结论（"请求模型要能说明算哪一张图，实现要分清算数据与写应用状态"）
在 agentxx 的对应物是：

1. **请求显式**：迁移后的每个点，请求里必须能看出"算哪一份数据"——
   会话 id / 消息数组 / 模型名 / 目标长度至少给全；实现不允许自己去猜"当前会话"或"当前上下文"。
2. **实现只算数据**：核心实现（与插件实现）**不写会话、不写值缓存、不发界面提示、不落盘**；
   写回（替换消息、写 share store、更新提示消息）由**编排方**（中间件 / 节点）在拿到结果后做。
   这条同时服务两个目的：调用零副作用（§4.2）与"实现可以被插件替换"（替换者不该被要求复刻写回逻辑）。
3. **参数与身份的边界**：请求里带"这次算哪一份输入"，`identity` 只由这一份输入决定；
   "这次要多长/多严"这类策略参数不进身份。
4. **热路径单列**：确有"必须同线程同步、不能等待"的调用点（TPS 的 token 估算），
   保留一条直连核心实现的同步快路径，并在文档里写清"插件实现不参与这条路径"（§8.1）。

---

## 8. 设计 F：第一批功能点

### 8.1 `agentxx.context.countTokens`（provide，首个落地点）

| 项 | 内容 |
|---|---|
| 类型 | `provide` |
| 展示名 / 说明 | 文本 Token 估算 / 按模型口径估算一段文本或一组消息的 token 数 |
| 实现层 | `plugin`（插件 / FFI 宿主）与 `core` 都允许 |
| 值缓存 | `ByIdentity`（`max_items = 64`，`max_bytes = 256 KiB`；数值见 §19.1） |
| 身份 | `model / kind / fnv1a64(text)`（messages 形态按拼接后的文本哈希） |
| 可调用 | **是**（`callDoc` 写明参数与返回） |
| 超时 | `implTimeoutMs = 0`（等实现，默认不限）；TPS 那条同步快路径不受超时影响 |
| 核心实现 | 把 `SummarizationMiddlewareHandle::countTokensForUtf8Str` / `countTokens` 的估算规则（`ascii_chars_per_token` / `unicode_chars_per_token` / `tokens_per_image` / `extra_tokens_per_message`）搬进点实现，**只保留一份**；中间件上的旧函数改为转发 |

请求（规范形态，插件边界为 JSON）：

```jsonc
{ "text": "…",            // 与 messages 二选一
  "messages": [ /* ChatMessage */ ],
  "model": "m1",           // 可选：不同模型口径不同
  "kind": "text" }         // text | messages（缺省按给的字段判断）
```

结果：`{ "tokens": 1234 }`。

**同步快路径的处理**（必须写清楚，否则会踩坑）：

* `service.token.count` 现在被 `event_stream.cpp` 的 TPS 计算**同步**调用（每个增量 token 一次）。
  功能点是异步的，不能直接接到这条路上。
* 做法：点的核心实现是**一个普通函数对象**，同步服务与功能点都调用它——
  `event_stream` 继续走 `callService`（未注册时回退 `utilxx_base::estimateTokenCount`），
  只是实现体改成"从功能点的核心实现取数"。**一份实现、两个入口**，插件实现不参与同步路径。
* 文档写明：插件提供的 tokenizer 会被预算计算、插件调用与自省看到，但不会影响 TPS 显示。

### 8.2 `agentxx.context.summarize`（provide）

| 项 | 内容 |
|---|---|
| 类型 | `provide` |
| 展示名 / 说明 | 上下文压缩 / 把一段上下文压成一段摘要文本（不写回会话） |
| 实现层 | `plugin` / `core` |
| 值缓存 | `None`（结果由编排方写回会话；值缓存只用于**同身份并发去重**） |
| 身份 | `sessionId / messagesVersion / model` |
| 可调用 | **否**（本轮暂不对外开放调用；见 §18 路线图） |
| 超时 | `implTimeoutMs = 0`（默认不限）；压缩是重活，调用方（CLI / FFI / 脚本）按自己的耐心给 `timeoutMs` |
| 核心实现 | `SummarizationMiddlewareHandle::doSummarizeWithLLM`（同上下文压缩，经子代理）+ `fitSummaryMaxTokens` 截断；**编排留在中间件**：提示消息、`NodeInterrupt` 派生与 resume、写回（`[system] [user 压缩指令] [assistant 摘要]`）、硬截断兜底、失败计数与冷却 |

请求：

```jsonc
{ "sessionId": "s1",          // 与 messages 二选一（两个都给时以 messages 为准）
  "messages": [ … ],          // 已经过编排方清理（噪音清理 / thinking 清理 / 工具折叠 / 多模态降级）
  "model": "m1",
  "targetTokens": 120000,     // 压缩目标（可选）
  "maxSummaryTokens": 65536,  // 摘要长度上限（可选，缺省用内置值）
  "language": "zh-cn" }
```

结果：`{ "summary": "…", "truncated": false }`。

两条实现约束：

* **核心实现只产出摘要文本**：不替换会话消息、不发提示消息、不写 share store（这些是编排方的事）。
  它内部派生一个"用完即弃"的压缩子代理（自建自毁）属于实现细节，不算修改应用状态；
  但**在 AgentHost 不可用**（没有宿主、也不在轮次内）时核心实现返回失败 + 说明，
  而不是改成同步阻塞等模型。
* **插件实现**：可以在收到请求后自己算（本地模型 / 抽取式摘要 / 规则法等），
  只要按 §5.2 的形状回答；`viaCall = true`（外部调用）时同样不许写会话——
  实现不需要也不应该知道调用方的会话（请求里给了消息）。

行为不变的验收点：把中间件改成"经点取值"之后，`test_summarization` 的既有用例必须全绿
（自动压缩、手动压缩、失败兜底、提示消息复用、上下文写回形状都不变）。

### 8.3 `agentxx.tool.summarizeOutput`（provide，阶段 7 落地）

| 项 | 内容 |
|---|---|
| 类型 | `provide` |
| 展示名 / 说明 | 工具输出摘要 / 把过长的工具输出压成一段摘要（原文由编排方卸载） |
| 实现层 | `plugin` / `core` |
| 值缓存 | 无（不缓存、不设上限；工具输出原文由 share store / 会话库管理，见 §3.5 第 7 条） |
| 身份 | `toolCallId / fnv1a64(content) / maxTokens` |
| 可调用 | **否**（应用流程专用；将来有需要再开） |
| 超时 | `implTimeoutMs = 0`（默认不限） |
| 核心实现 | 现有的自动摘要逻辑（`AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY` 路径）：产出摘要文本；share store 卸载与消息替换留在调用方 |

请求：`{ "toolName": "…", "toolCallId": "…", "content": "…", "maxTokens": 2000, "sessionId": "s1" }`；
结果：`{ "summary": "…" }`。

### 8.4 后续批次（登记在案，本轮不实现）

| 点 id | 类型 | 请求 / 结果（摘要） | 备注 |
|---|---|---|---|
| `agentxx.permission.decide` | decide（`anyVeto`） | 请求 `{sessionId, toolName, scope, targets[], args}`；结果 `{verdict:"deny"/"abstain", reason}` | 插件只能收紧；核心实现 = 现有规则引擎（allow / ask / deny 判定留在核心实现里） |
| `agentxx.context.repairMessages` | provide | 请求 `{sessionId, messages[], model}`；结果 `{messages[], changed, notes[]}` | 不可调；把 `modelcall.cpp` 的修复逻辑搬进核心实现 |
| `agentxx.tool.select` | provide | 请求 `{sessionId, tools[{name,source}]}`；结果 `{enabled[]}` | 不可调；吸收 `enableToolFiltering` / `toolWhitelist` 的装配期过滤 |
| `agentxx.memory.retrieve` | provide | 请求 `{sessionId, query, limit}`；结果 `{snippets[]}` | 动态段会破坏提示词稳定前缀，落点与缓存口径要先单独设计 |
| `agentxx.skill.match` | provide | 请求 `{sessionId, query}`；结果 `{skills[]}` | 同上 |
| `agentxx.subagent.run` | provide | 请求 `{parentSessionId, task, tools[], model}`；结果 `{resultText, sessionId}` | 吸收 `service.subagent.execute`；要处理中断与嵌套派生的既有语义 |
| `agentxx.model.select` | provide | 请求 `{sessionId, purpose}`；结果 `{modelName}` | 暂缓（属界面偏好与配置） |

---

## 9. 设计 G：跨边界接口（插件表 / kit / 核心 API / FFI / 命令行）

### 9.1 新接口表 `agentxx.agent.feature` v1

```c
/* ==================== 接口表: 功能点 (agentxx.agent.feature) ==================== */
#define AGENTXX_PLUGIN_IFACE_AGENT_FEATURE         "agentxx.agent.feature"
#define AGENTXX_PLUGIN_IFACE_AGENT_FEATURE_VERSION 1

#define AGENTXX_PLUGIN_FEATURE_TYPE_PROVIDE 0
#define AGENTXX_PLUGIN_FEATURE_TYPE_DECIDE  1

/* 插件声明自己的功能点（本轮只允许 PROVIDE；应用留出的点在宿主代码里声明，不在此列） */
typedef struct AgentxxPluginFeaturePointSpec {
    uint32_t           struct_size;   /* sizeof(...); 0 = 当前布局 */
    int32_t            type;          /* AGENTXX_PLUGIN_FEATURE_TYPE_* */
    int32_t            impl_timeout_ms; /* 等实现方的超时(ms); 0 = 不限(默认) */
    int32_t            _reserved;       /* 8 字节补齐 */
    PluginxxStringView id;            /* 必须 "plugin.<本实例插件名>.<名字>" */
    PluginxxStringView title;         /* 展示名（可空，缺省用 id） */
    PluginxxStringView depict;        /* 一句话说明 */
    PluginxxStringView args_doc;      /* 参数说明（文本，进清单与作者文档） */
    PluginxxStringView result_doc;    /* 结果说明（文本） */
} AgentxxPluginFeaturePointSpec;

/* 功能点实现登记（与工具 execute_start 同一套操作协议） */
typedef struct AgentxxPluginFeatureImplSpec {
    uint32_t           struct_size;
    int32_t            priority;          /* 层内顺序，小者先 */
    int32_t            default_timeout_ms;/* 自报的最大耗时(ms); 0 = 不限(默认); 与点的 impl_timeout_ms 取较小非 0 值 */
    PluginxxStringView point_id;
    void*              user_data;
    void*(PLUGINXX_CALL* impl_start)(
        void*                         user_data,
        const PluginxxStringView*     point_id,
        const PluginxxStringView*     call_json,   /* {args, request, caller, viaCall, identity} */
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out);
    void(PLUGINXX_CALL* impl_cancel)(void* user_data, void* op);
} AgentxxPluginFeatureImplSpec;

typedef struct AgentxxPluginFeatureIface {
    int32_t  version;     /* 必须 == AGENTXX_PLUGIN_IFACE_AGENT_FEATURE_VERSION */
    uint32_t struct_size;

    /* 功能点清单（JSON 文本；出参经 pluginxx 字符串释放约定释放） */
    int32_t(PLUGINXX_CALL* list_points)(const PluginxxHost* host, PluginxxString* out_json);

    /* 声明 / 撤销插件自己的功能点 */
    int32_t(PLUGINXX_CALL* define_point)(
        const PluginxxHost* host, const AgentxxPluginFeaturePointSpec* spec);
    int32_t(PLUGINXX_CALL* undefine_point)(
        const PluginxxHost* host, const PluginxxStringView* id);

    /* 登记 / 撤销实现（一个点一个实现；重复登记 = 覆盖） */
    int32_t(PLUGINXX_CALL* register_impl)(
        const PluginxxHost* host, const AgentxxPluginFeatureImplSpec* spec);
    int32_t(PLUGINXX_CALL* unregister_impl)(
        const PluginxxHost* host, const PluginxxStringView* point_id);

    /* 调用一个功能点（异步；完成回调经 notify 在宿主 IO 线程发布） */
    PluginxxOperatorHandle*(PLUGINXX_CALL* call_point_async)(
        const PluginxxHost*       host,
        const PluginxxStringView* point_id,
        const PluginxxStringView* args_json,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out);
    void(PLUGINXX_CALL* op_cancel)(PluginxxOperatorHandle* op);
} AgentxxPluginFeatureIface;
```

### 9.2 宿主实现落点（改哪些文件）

| 内容 | 落点 |
|---|---|
| 接口表结构体与 IID 常量（功能点） | `agent/lib/include/agentxx/plugin/api/plugin_api.h` |
| 钩子新结构体与钩子表尾部三项（§6.2） | 同上（`AgentxxPluginHookSpecEx` + `AgentxxPluginHooksIface` 追加项） |
| 接口名常量（协商用） | `agent/lib/include/agentxx/plugin/plugin_interfaces.h` 的 `plugin_interfaces::AgentFeature` |
| 表实例化 + `query_interface` 分支 | `agent/lib/src/plugins/plugin_manager_vtable.cpp`（新增静态表 + 一处 IID 比较） |
| 功能点表入口实现（声明 / 实现登记 / 调用 / 清单） | 新文件 `agent/lib/src/plugins/plugin_manager_feature.cpp` |
| 钩子注册表与派发器（§6.1） | 新文件 `agent/lib/src/plugins/plugin_manager_hooks.cpp`；`plugin_manager_adapters.cpp` 里旧的按插件派发随之移除 |
| 实例侧记录与清理 | `PluginInstance`（`featurePoints` / `featureImpls` / 钩子句柄列表）、`detachDomainRegistrations`、`clearDomainRegistrations`、`RegistrationInventory`、`PluginListView` 诊断字段（`plugin_manager.h` / `plugin_manager_lifecycle.cpp` / `plugin_manager_adapters.cpp`） |
| 核心侧子系统 | `agent/lib/{include/agentxx/feature,src/feature}`（§14 布局） |
| 清单进快照与诊断 | `agent/lib/src/agent/assembly_snapshot.cpp`（`features` / `hooks` 段）、`agent/lib/src/util/diagnostics.cpp`（`--dump-diagnostics`） |
| wire 消息（FFI / JSONL / 远程） | `agent/lib/include/agentxx/agent/io/wire_protocol.h`（MsgType + 编解码）、`agent/lib/include/agentxx/agent/io/agent_io_transport.h`（结构体 + 消息联合体）、`agent/lib/src/agent/io/session_server_agent_io.cpp`（分发与处理） |
| FFI 导出（§9.5） | `agent/lib/include/agentxx/ffi_api.h`、`agent/lib/src/ffi/{ffi_api.cpp,ffi_client_io.cpp,ffi_runtime.cpp}`、`agent/lib/ffi_symbols.map`、`docs/zh-cn/design/ffi.md`、`agent/ffi/dart` 绑定重新生成 |
| 命令行入口（§9.6） | `agent/client/main.cpp`（子命令 `feature` 的三个动作 + `--dev` + 帮助文本） |
| 接口表数量 | `PluginManager::kInterfaceTableCount`（`10 + 9` → `10 + 10`）、`docs/zh-cn/design/plugins.md` §8、根 `AGENTS.md` 三处一起改（测试会校验一致） |

### 9.3 kit 糖（`agent/lib/include/agentxx/plugin/api/plugin_kit.h`）

```cpp
// 声明 / 撤销插件自己的功能点（本轮只允许 provide）
int32_t defineFeaturePoint(const FeaturePointSpec& spec);
int32_t undefineFeaturePoint(std::string_view id);

// 登记实现：同步形态（快、直接给结果；由 kit 合成一个立即完成的操作）
int32_t provideFeature(std::string_view pointId, SyncFeatureFn&& fn,
                       FeatureImplOptions opts = {});
// 登记实现：协程形态（长任务；与 fast_tool / tool 相同的驱动方式，不阻塞 IO 线程）
int32_t provideFeature(std::string_view pointId, AsyncFeatureFn&& fn,
                       FeatureImplOptions opts = {});
int32_t unprovideFeature(std::string_view pointId);

// 调用：协程形态（推荐）与回调形态（都异步；timeout 可省 = 不限时）
asio::awaitable<FeatureCallResult> callFeature(std::string_view id, std::string_view argsJson,
                                               std::chrono::milliseconds timeout = {});
void callFeatureAsync(std::string_view id, std::string_view argsJson,
                      FeatureCallback cb, std::chrono::milliseconds timeout = {});
```

约定：

* 同步形态的 `fn` 必须**快速返回**（不做 IO、不等待）；需要等待就用协程形态。
* 协程形态里的等待用 kit 既有原语（`co_await sleep / offload / invoke_cap`），
  与 `fast_tool` / `polled_tool` 的写法一致；宿主不会为某个实现单独开线程。
* **kit 自动做两件事**：回答自动包一层（`{"value": …}` / `{"verdict": …, "reason": …}`）；
  `caller == 自己的插件名` 时自动不回答（保护 ①，可用 `opts.answerSelf = true` 关掉）。
* **不提供同步等待的调用糖**（没有 `callFeatureBlocking`），从接口面上断绝"阻塞 IO 线程"的写法。
* 钩子的糖（§6.2）：`hookEx(ctx, point, fn, HookOptions{.priority = …, .ownerTag = …})` 返回句柄，
  `unregisterHookEx(handle)` 精确撤销，`listHooks(ctx)` 读钩子清单。

### 9.4 核心侧 C++ API（`agentxx::feature`）

```cpp
namespace agentxx::feature {

enum class CacheMode : int32_t { None = 0, Latest, ByIdentity };

struct PointOptions {
    std::string_view title;               // 展示名
    std::string_view depict;              // 一句话说明
    std::string_view callDoc;             // 参数 / 结果说明（空 = 不允许调用）
    bool             callable      = false;
    CacheMode        cache         = CacheMode::None;
    size_t           maxItems      = 0;   // ByIdentity 生效
    size_t           maxBytes      = 0;   // 值缓存字节上限（0 = 按实现内置默认）
    int32_t          implTimeoutMs = 0;   // 等插件实现的超时（0 = 不限）
};

template<class TReq, class TValue>
class ProvidePoint {
public:
    asio::awaitable<AskResult<TValue>>  ask(const TReq& req, AskOptions opts = {});
    asio::awaitable<CallResult<TValue>> call(const TReq& req, CallOptions opts = {});
    void addCoreImpl(std::string_view owner, int32_t priority,
                     std::function<asio::awaitable<std::optional<TValue>>(
                         const TReq&, const ImplContext&)> fn);
};

class Registry {
public:
    template<class TReq, class TValue>
    ProvidePoint<TReq, TValue>&
        provide(std::string_view id, PointOptions opts,
                RequestCodec<TReq> codec, ValueCodec<TValue> codec,
                IdentityFn<TReq> identityOf);
    template<class TReq, class TVerdict>
    DecidePoint<TReq, TVerdict>& decide(std::string_view id, PointOptions opts, /* codecs */);

    Json listPointsJson() const;               // 清单（插件 list_points 与诊断共用）
    Stats statsOf(std::string_view id) const;  // 调用统计
};

} // namespace agentxx::feature
```

* 注册表挂在 `AgentContext`（`ctx->features`），在 `BaseAgent::init` 里创建；
  核心点的声明放在各自中间件 / 节点的装配代码里（`initMiddleware` / 节点注册），
  **早于插件装载**（`PluginManager::loadConfiguredPlugins`），因此插件登记实现时点已存在。
* **没有"只允许核心实现"的开关**：所有点都接受插件 / FFI 宿主登记实现（§3.1、§3.3）。
* `ImplContext` 给核心实现：`{layer, owner, caller, viaCall, identity, sessionId}`。
* 核心点的 id / 展示名 / 说明用 `agentxx::feature::points` 里的常量集中声明，文档表格与测试引用同一份。

### 9.5 FFI 宿主接入（wire 消息 + FFI 导出）

FFI 是"client 端点"的 C 抽象（见 `docs/zh-cn/design/ffi.md`）：它经进程内 Channel 与
`SessionServerAgentIO` 通信，宿主工具就是走 wire 消息（`host_tool_register` / `host_tool_call` /
`host_tool_result`）实现的。功能点按同一套路扩展，因此**宿主、脚本、远程客户端共用一份协议**。

**wire 消息**（加进 `wire_protocol.h` 的 `MsgType` + 编解码，`agent_io_transport.h` 的结构体与消息联合体，
`session_server_agent_io.cpp` 的分发）：

| 消息 | 方向 | 载荷 |
|---|---|---|
| `feature_list` / `feature_list_result` | client → server / 回 | `{}` / `{reqId, ok, points:[…]}`（清单形状同 §11.1） |
| `feature_call` / `feature_call_result` | client → server / 回 | `{reqId, id, args, refresh?, timeoutMs?}` / `{reqId, ok, id, value?, error?, by?, identity?, fromCache?, ms, message?}` |
| `feature_impl_register` / `feature_impl_unregister` | client → server | `{pointId, priority, timeoutSec, declare?:{title, depict, args, result}}` / `{pointId}`（`declare` 非空时宿主声明自己的点，id 必须 `host.<宿主名>.<名>`） |
| `feature_impl_call` / `feature_impl_result` / `feature_impl_cancel` | server → client | `{callId, pointId, callJson, sessionId, timeoutSec}` / `{callId, isError, resultJson, ms}` / `{callId}` |

* `reqId` 沿用 `list_dir` / `list_dir_result` 的既有约定（同一条消息里回显），宿主据此配对。
  调用方可以在 `feature_call` 里给 `timeoutMs`（**默认 0 = 不限**）：到点服务端取消当次实现并回
  `failed`（`message` 写清限时值）；宿主自己不等了也可以直接丢弃晚到结果（结果按 `reqId` 找不到
  请求就忽略）。
* 客户端断开连接 / 关闭时：它登记的实现按"来源失效"处理（§3.5 第 3 条），
  未应答的 `feature_impl_call` 由服务端按"没意见"继续链，并记一条日志
  （与宿主工具断开时的清理同一套做法）。
* 客户端登记的实现在 `plugin` 层（owner `host:<客户端标识>`），与插件实现按同一套优先级排序（§3.3）。

**FFI 导出**（加进 `ffi_api.h` + `ffi_symbols.map` + `ffi.md`，并用 ffigen 重新生成 Dart 绑定）：

```c
#define AGENTXX_FFI_CAP_FEATURES "features"   /* agentxx_ffi_get_capabilities 上报 */

/* 读功能点清单 (同步查询, 与 list_models 同构; out 经 agentxx_ffi_string_free 释放) */
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
agentxx_ffi_feature_list(AgentxxFFIAgent* a, AgentxxString* out, AgentxxString* log);

/* 调用一个功能点: 受理即返回 (out_request_id 用于配对), 结果经 EVT_FEATURE_RESULT */
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_feature_call(
    AgentxxFFIAgent* a, const AgentxxStringView* point_id, const AgentxxStringView* args_json,
    int64_t* out_request_id, AgentxxString* log);

/* 宿主提供实现 (spec_json: {"point","priority","timeoutSec","declare"?}) */
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_feature_register(
    AgentxxFFIAgent* a, const AgentxxStringView* spec_json, AgentxxString* log);
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_feature_unregister(
    AgentxxFFIAgent* a, const AgentxxStringView* point_id, AgentxxString* log);
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_feature_respond(
    AgentxxFFIAgent* a, int64_t call_id, int32_t is_error,
    const AgentxxStringView* result_json, AgentxxString* log);
```

**新事件**：`AGENTXX_FFI_EVT_FEATURE_RESULT`（宿主发起调用的结果：
`{requestId, ok, id, value?, error?, by?, identity?, fromCache?, ms, message?}`）、
`AGENTXX_FFI_EVT_FEATURE_CALL`（服务端要宿主实现点：`{callId, pointId, callJson, sessionId, timeoutSec}`）、
`AGENTXX_FFI_EVT_FEATURE_CANCELLED`（宿主实现调用取消）。

约定：

* `feature_call` 的失败（`not_callable` / `bad_args` / `no_impl` / `disabled` / `busy` / `failed`）
  **经事件回**（`ok:false` + `error`），不用同步错误码；同步错误码只表示受理失败
  （未 start、参数非法、宿主太旧没有该符号）。
* 宿主提供的实现在宿主自己的线程上算，算完用 `feature_respond` 回；**不允许**在回调线程里做长任务
  （与宿主工具同一约定）。
* 老宿主（没有这些符号/消息）→ 宿主侧按"功能不可用"降级：`feature_list` 返回空清单、
  `feature_call` 报 `no_impl`，其余功能照常（与接口表的"门槛与降级"口径一致，§13 第 20 条）。

### 9.6 命令行接入（子命令 `feature`）

命令行统一做成一个**子命令**，与全局的 `--dump-config` / `--dump-diagnostics` 分工清楚：
那两个是"整个装配"的快照，这里是"功能点"这一块的清单与调用。

```bash
# 读清单（JSON；含 callable / callDoc / 实现链与顺序；开发者模式开启时含 stat）
agentxx_cli feature --list [--json | --text]

# 调用一个功能点（结果 JSON 原样打到 stdout；--timeout-ms 不给 = 不限时）
agentxx_cli feature --call agentxx.context.countTokens \
    --args '{"text":"hello","model":"m1"}' [--timeout-ms 5000] [--session <id>]

# 打印功能点相关的配置 / 装配段（与全局 --dump-config 里的 features 段同一份实现）
agentxx_cli feature --dump-config

# 远程 / 跨进程：与 tui / cli / server 一样加 --agent
agentxx_cli feature --list --agent ws://192.168.1.100:17000/agent?token=xxx

# 本次运行打开开发者模式（收集统计与派发记录）
agentxx_cli feature --list --dev
```

* **本地执行**（不带 `--agent`）：装配后直接读 `ctx->features`（不经 wire，少一层往返），
  跑完即退出，与 `--dump-config` 同一条装配路径；
* **远程执行**（带 `--agent`）：经 wire 走 §9.5 的消息；
* **`jsonl` 模式**天然可用（同一份 wire 协议）：脚本 / stdin 直接发
  `{"type":"feature_list","reqId":1}` 或
  `{"type":"feature_call","reqId":1,"id":"agentxx.context.countTokens","args":{"text":"..."},"timeoutMs":5000}`，
  从 stdout 读对应的 `..._result`；其他语言、CI、shell 都不必链接库；
* `--timeout-ms` 是本子命令的参数（**默认 0 = 不限**）：本地执行时给 `call()` 的 `timeoutMs`，
  远程执行时放进 `feature_call` 的 `timeoutMs`（§4.4）；
* 输出纪律：结果 JSON 只走 stdout（可重定向 / 管道），日志走 `XX_LOG`（不影响 stdout）；
  失败时 stdout 里也是结果 JSON（`ok:false` + `error`），进程退出码非 0 并在 stderr 写一行原因；
* `agentxx_cli feature --help` 列出三个动作与可选参数（`--args` / `--timeout-ms` / `--session` / `--agent` / `--dev`）。

## 10. 设计 H：线程、超时、体积与取消

| 环节 | 线程 / 等待 | 说明 |
|---|---|---|
| 核心调用点（中间件 / 节点） | 宿主 IO 线程上的协程 `co_await` | 与现有 `awaitPluginOp` 同一条执行序列 |
| 核心实现 | 就地执行或 `offload` 到工作线程池 | 阻塞工作必须 `offload`（既有约定） |
| 插件实现（异步形态） | 宿主 IO 线程 → 插件本地运行时（driver/wake 协议） | 复用 `OpCore` / `OpDrive`，不阻塞 IO 线程 |
| 插件实现（同步形态） | 就地执行，**必须快速返回** | SDK 文档写明；长任务用协程形态 |
| 插件调用功能点 | `call_point_async` + `notify` 回调 | 不阻塞调用方；协程糖内部经桥接等待 |
| FFI 宿主 / 远程客户端调用功能点 | wire `feature_call` + `feature_call_result`（按 `reqId` 配对） | 宿主不阻塞；调用方可给 `timeoutMs`（默认 0 = 不限），到点服务端取消当次实现并回 `failed` |
| FFI 宿主 / 远程客户端提供实现 | wire `feature_impl_call` → 宿主线程 → `feature_impl_result` | 客户端断开时未应答的调用按"没意见"继续链并记日志 |
| 命令行一次性调用 | 本地：装配后直接调 `ctx->features`；`jsonl`：走 wire | 结果 JSON 走 stdout，日志走 `XX_LOG` |
| 超时（等实现） | 点的声明方可给 `implTimeoutMs`（**默认 0 = 不限**） | 到点取消当次实现（`impl_cancel`）并按"没意见"继续链；某个实现卡住不再拖死整条链 |
| 超时（整次调用） | 调用方给 `timeoutMs`（**默认 0 = 不限**） | 到点取消当次实现并返回 `failed` + `message`；插件糖 / FFI / 命令行 / `jsonl` 都收这个参数 |
| 取消 | 宿主取消当次实现；调用方放弃后丢弃晚到结果 | 不做"取消调用方整条流程"的承诺 |
| 体积 | 大结果只经一次跨边界拷贝（JSON 文本） | 单次请求 / 结果设上限（如 8 MiB，超过拒绝并说明），值缓存另有字节上限 |
| 调用日志 | 失败与生命周期事件始终记（warning / info）；**成功调用一行 info 只在开发者模式**（§11.4） | `功能点调用：<id> ← <caller / app>：<by / -> <ms> ms <ok / error>`；不记参数正文（可能含路径） |

---

## 11. 设计 I：自省与排障

### 11.1 清单（`list_points` 与诊断共用一份实现）

```jsonc
{ "count": 3,
  "points": [ {
      "id": "agentxx.context.countTokens",
      "type": "provide",
      "title": "文本 Token 估算",
      "depict": "按模型口径估算一段文本或一组消息的 token 数",
      "origin": "core",                       // core | plugin:<插件名> | host:<宿主名>
      "callable": true,
      "callDoc": "参数：text 或 messages（二选一）、model（可选）；返回：{tokens}",
      "cache": { "mode": "by_identity", "maxItems": 64, "maxBytes": 262144 },
      "effectiveBy": "plugin:my_plugin",      // 当前会生效的实现（层 + 优先级最小）
      "disabledBy": "",                       // 最近一次置空是谁做的（空 = 没有置空）
      "impls": [
        { "layer": "plugin", "owner": "plugin:my_plugin", "priority": 0, "enabled": true,
          "effective": true, "note": "", "load": "dynamic" },
        { "layer": "core", "owner": "core:context:tokenEstimate", "priority": 0,
          "enabled": true, "effective": false, "note": "" }
      ],
      "stat": { "asks": 12, "calls": 3, "errors": 0,
                "lastBy": "plugin:my_plugin", "lastCaller": "other_plugin",
                "lastMs": 4 } } ] }
```

* `impls` 只列**当前生效**的实现（禁用 / 已摘除的不列）；`layer` 只有 `plugin` / `core` 两种取值；
  `priority` 是生效值（越界已被裁剪到上下限）。
* `load`（`dynamic` / `builtin`）只说明插件怎么装载的，不影响顺序。
* `stat` 段**只在开发者模式**（§11.4）出现；关闭时清单只给"当前状态"
  （点 / 实现 / 顺序 / 可调性 / `disabledBy` 当前值），不累计任何记录数据。
* 钩子清单（`list_hooks`）形状见 §6.3，两份清单在装配快照与诊断里并列出。

### 11.2 出口

| 出口 | 内容 |
|---|---|
| 插件 `list_points` / `list_hooks` | 功能点清单与钩子清单（插件用来判断"这个功能谁在提供、能不能调、谁排在前面"） |
| 装配快照 | `buildRuntimeSnapshot` 增加 `features` 与 `hooks` 段：点数 / 各点生效实现 / 可调点数 / 处理器数与顺序；`renderAssemblySnapshot` 出人读文本 |
| `--dump-diagnostics` | `buildDiagnosticsText` 增加同一份清单（截断到可读长度） |
| 命令行 | 子命令 `feature --list` / `feature --call <id>` / `feature --dump-config`（§9.6）；`jsonl` 模式经 wire 读清单与调用 |
| FFI | `agentxx_ffi_feature_list`（同步查询）与 `EVT_FEATURE_RESULT` / `EVT_FEATURE_CALL`（§9.5） |
| 日志 | 失败与生命周期事件始终记（warning / info）；**成功调用一行 info 只在开发者模式**（§11.4） |
| 开发者模式 | `dev_mode`（yaml / FFI `devMode` / CLI `--dev`）：开启才累计统计与派发记录（§11.4）；装配快照里有 `dev_mode` 一行 |
| 界面展示 | 本轮不做；§18 路线图登记"TUI 里显示功能点清单" |

### 11.3 统计口径（仅在开发者模式下收集）

* 每个点记 `asks` / `calls` / `errors` / `lastBy` / `lastCaller` / `lastMs`；
  按会话细分不做（点表是 agent 级，会话信息在 `identity` 与日志里）。
* 统计只做展示与排障，不参与任何判定（不因失败次数自动禁用实现、不影响实现链顺序）。
* **开关**：只有开发者模式（§11.4）下才累加；关闭时这些字段不存在于清单里（不是"全 0"，而是没有 `stat` 段）。
* 内存：每个点一组固定大小的计数（无历史数组、无环形缓冲）；点表本身是装配期确定的，不会增长。
* 钩子侧同理：派发次数、上一轮派发顺序与耗时只在开发者模式下记录（§6.3）。

### 11.4 开发者模式（记录数据的开关）

**动机**：调用统计、派发记录、每次成功调用的日志都是排障用的，平时没有价值，却要在热路径上累加、
占内存。因此把它们统一收到一个**启动期开关**后面：只有开启时才收集。

* **开关**：yaml `dev_mode`（默认 `false`）；FFI 配置 JSON `devMode`；命令行 `--dev`（§9.6）；
  启动时读取一次，随后**冻结不可变** —— 与 `AgentConfigStatic::enableBenchmark` 同一做法：
  配置进 `AgentConfig::devMode`，启动时镜像到进程级只读标记（`AgentConfigStatic::devMode`），
  库内热路径只读这个标记，避免到处取 `AgentContext`。
* **开启时收集**：
  1. 功能点调用统计（`asks` / `calls` / `errors` / `lastBy` / `lastCaller` / `lastMs`）与清单的 `stat` 段；
  2. 钩子派发记录（每次派发的点、处理器顺序、耗时）与对应日志（§6.3）；
  3. 功能点成功调用的一行 info 日志（关闭时只记失败与生命周期事件）；
  4. 清单里的调用历史字段（`lastCaller` 等）与 `disabledBy` 的变更历史。
* **不门控**（始终可用）：清单的静态部分（点 / 实现 / 优先级 / 可调性 / `disabledBy` 当前值）、
  值缓存（它是功能不是统计）、失败与生命周期的 warning / info 日志、`--dump-diagnostics` 的导出。
* **关闭时的开销**：热路径只多一次只读布尔判断（与 benchmark 标记同一做法）；不分配统计结构、不保留历史。
* **只控制收集，不改变行为**：开发者模式不改实现链、不改值缓存策略、不改超时、不改日志级别；
  想同时打开 benchmark 统计或更详细的日志，由使用者在配置里分别打开，本设计不把它们绑在一起。
* **可见性**：装配快照里有 `dev_mode: true/false` 一行（便于确认这次启动收不收数据）；
  任何出口在关闭时都不出现 `stat` 段，只给"当前状态"。

## 12. 设计 J：与既有机制的分工

| 机制 | 保留什么 | 与功能点的关系 |
|---|---|---|
| 工具（`agentxx.agent.tools`） | 给模型调用的函数（模式、提示词、权限声明、并行/重复调用语义） | 不合并。工具是"面向上游模型的接口"，功能点是"面向程序的功能"；两者可以互相调用（工具内部可以 `co_await` 功能点） |
| 钩子（`agentxx.agent.hooks`） | 7 个固定点的生命周期观察；载荷与"结果丢弃"的语义不变 | 不合并。本轮补处理器清单与优先级（§6）：core 与插件都能读顺序 |
| 事件（`pluginxx.events`） | 通知（`plugin.*` / `client.*` 主题） | 不合并。通知型功能点不搬（§1.3 第 1 条） |
| 能力（`pluginxx.capabilities`） | 点名调用一个插件的具名能力 | 不合并。判定口诀与 musicxx 相同：**"我知道问哪个插件、要一个小结果" → 能力；"我只知道要这个功能、谁实现都行、可能很慢" → 功能点** |
| 工具互调（`call_tool_async`） | 插件之间点名调用工具 | 同上 |
| 事件总线服务（`service.*`） | 迁移期内保留（压缩手动触发、权限询问、子代理、中断、跨 agent、TPS 同步服务） | **逐步被功能点吸收**：先接功能点、行为不变；被取代的服务在单独的提交里删除（§8.1 是第一个例子） |
| 提示词（`agentxx.agent.prompt`） | 贡献 + 按键合成（已是很干净的贡献模型） | 不合并；提示词片段不是"功能" |
| 资源（skill / memory / MCP） | 装配期资源贡献 | 后续批次的 `memory.retrieve` / `skill.match` 会读它们，但仍各管各的 |
| 图节点（`agentxx.agent.graph`） | 插件自定义节点类型 + 执行图定义 | 不合并；节点内部可以调用功能点 |
| FFI 宿主工具（`agentxx_ffi_tool_register`） | 宿主（嵌入方）把自己的函数给模型用 | 不合并。宿主想"提供 / 调用功能"走功能点接入（wire + FFI 导出，§9.5）：读清单、调用点、登记实现；宿主实现默认优先级带 `1000`（比插件低、比 core 高，§3.3） |
| 命令行 / JSONL / 远程客户端 | `jsonl` 与 `server` 共用同一份 wire 协议；脚本只发 JSON | 复用。功能点的清单与调用就是几条新 wire 消息（§9.5），脚本与远程客户端零新增概念 |

---

## 13. 从 musicxx 本轮重构同步的设计（逐条对照）

| # | musicxx 设计 | agentxx 现状 | 处置 |
|---|---|---|---|
| 1 | **三类型**（provide / decide / observe），不做 `kind` 字段，用不同 class 表达 | 无 | **采纳两类型**（provide / decide）；observe 不搬（事件 + 钩子已覆盖，§18 路线图），枚举预留 |
| 2 | **点是一个对象**，登记项挂在点上，不散成静态变量 | 无 | 采纳（`ProvidePoint` / `DecidePoint` 持有实现列表、值缓存、统计） |
| 3 | **层间固定 extern → builtin → core，层内 (priority, 登记序)** | 钩子只有"装载顺序"；能力/工具是点名 | **适配**：agentxx 只有一套插件框架（`builtin://` 是同框架内联编译），所以层只分 `plugin → core`；层内**默认带** = 插件 `0` / FFI 宿主 `1000`（宿主排在插件之后、core 之前）；"顺序必须声明"与"越界裁剪 + 警告"的结论采纳 |
| 4 | **值缓存**：不保留 / 最新 / 按身份 LRU；in-flight 去重；按来源失效；条数 + 字节双上限；记产出方 | 无 | 采纳（`CacheMode` + `by` + 双上限 + 按 owner 失效） |
| 5 | **显式置空** `{"disable": true}` + `disabledBy` + 实现摘除后自动还原 | 无 | 采纳（`ask` 记标记、`call` 不记） |
| 6 | **声明才可调用**（`callSpec`），调用与取值的区别只有"不写值缓存、不记置空" | 无 | 采纳（`callable` / `callDoc` / `call()`） |
| 7 | **调用零副作用**：不写值缓存、不写媒体缓存、不改应用状态、不落盘 | 无（插件根本调不到核心功能） | 采纳，并扩展为"不写调用方会话 / 不发提示消息"（§7） |
| 8 | **三道保护**：载荷带 `caller`；全是调用方就不问插件层；同 `(点, 调用方)` 重入 → `busy` | 无 | 采纳（§4.3） |
| 9 | **结果与出处一起返回**（`by` / `identity` / `fromCache` / `ms`） | 无 | 采纳（§4.4，字段同名同义，方便两个项目对照） |
| 10 | **插件自定义功能点**：define / undefine + 别的插件实现 + 同一调用入口 | 无 | 采纳（§5）；**适配**：实现注册走功能点表，不借"通用通道钩子"（agentxx 钩子是固定枚举，§2.3 第 1 条） |
| 11 | **命名空间**：插件点必须 `plugin.<自己>.*`；owner 记 `extern:` / `builtin:` / `core:` | 事件主题有 `plugin.` 前缀规则；能力名无前缀约束 | 采纳（插件点 id 强校验；owner 用 `plugin:<名>` / `host:<宿主名>` / `core:<域>:<实现>`，层由 `layer` 字段表达） |
| 12 | **生命周期跟着插件走**：禁用摘生效、卸载全摘、重新启用重声明 | 已有同类机制（注册清单 / detach / clear） | 直接复用；功能点加进 `RegistrationInventory` 与诊断字段 |
| 13 | **能力调用全面异步化**：四层接口统一异步、删同步 ABI、调用方自备超时、不做取消承诺 | 插件调用已经是协程 / 操作协议；但存在同步等待入口（`invoke_capability_blocking`、`check_paths` 非 IO 线程同步等待、C ABI 的 `ioCallSync`） | **部分采纳**：功能点只提供异步形态，不新增同步入口；存量入口保留（有明确约束：不得在 IO 线程阻塞），把"功能点不阻塞"写进文档 |
| 14 | **一条通用实现通道**（不新增函数点） | 钩子点固定，插件不能自造 | 适配：功能点表本身就是通用通道（§5.2） |
| 15 | **自省进同一份清单**（能不能调 / 怎么调 / 谁在提供 / 被调几次） | 装配快照有插件注册计数，无功能清单 | 采纳（`list_points` + `list_hooks` + 快照 `features` / `hooks` 段 + 诊断） |
| 16 | **不设框架级超时** | 工具有 `default_timeout_ms`（插件自选） | **适配**：本轮把超时**实现出来但默认不限**（点声明方 `implTimeoutMs` / 实现自报值 / 调用方 `timeoutMs`，都为 0 = 不限）；"框架不替调用方猜超时"的口径不变 |
| 17 | **不链式传值**（本轮不做） | 无 | 采纳（不做） |
| 18 | **不做运行时权限校验**（框架口径） | 框架无运行期权限层 | 采纳（功能点不加权限层） |
| 19 | **调用日志一行**、参数不落日志 | 插件相关日志已有风格 | 采纳（§10） |
| 20 | **门槛与降级**（旧宿主没有该钩子 → 注册失败 / `available` 假） | 接口表协商机制已有（`interfaces.require/optional`） | 适配：插件声明 `agentxx.agent.feature`（建议先 `optional`），宿主表为空指针时 SDK 给出可读错误；清单缺失只影响自省 |
| 21 | **钩子处理器清单与优先级**（上一轮加的宿主能力） | agentxx 钩子清单只有计数、无优先级 | **采纳，本轮做**（§6）：有序注册表 + 单派发器 + `register_hook_ex` / `list_hooks`；默认顺序与现状一致 |
| 22 | **"能力"与"功能点"的语义区分写进文档** | 文档里没有这条对照 | 采纳（§12 的口诀 + 作者文档） |
| 23 | **宿主（嵌入方）也能提供与调用功能** | FFI 宿主只能注册工具给模型用 | 采纳（§9.5）：wire + FFI 导出让宿主读清单、调用点、登记实现；宿主实现在 `plugin` 层、**默认带 1000**（比插件低、比 core 高）；命令行同时给子命令（§9.6） |
| 24 | **值仓命名** | —— | 本项目用**值缓存**（`CacheMode` / `cache` 字段），与 musicxx 的 `ValueStore` 只是叫法不同 |
| 25 | **记录数据按需收集**（本轮追加，非 musicxx 项） | 无（本项目此前只有 `AgentConfigStatic::enableBenchmark` 这一先例） | 采纳：新增**开发者模式**（`dev_mode`，启动期冻结），开启才累计调用统计 / 派发记录 / 成功调用日志（§11.4） |
| 26 | **优先级越界与注册数量**（本轮追加） | 无 | 采纳"少检查、少限制"：`priority` 越界裁剪到上下限 + 警告；**不限制**处理器与实现的注册数量 |

---

## 14. 目录与文件布局

```text
agent/lib/include/agentxx/feature/
  feature.h            # 类型 / 选项 / 结果 / 错误码 / ImplContext / ProvidePoint / DecidePoint 模板
  registry.h           # Registry：点表（类型擦除）、清单、统计、插件面入口
  value_cache.h        # 值缓存：三策略 + 双上限 + in-flight 去重 + 按来源失效
  points.h             # 核心点的 id / 展示名 / 说明常量 + 请求与结果结构
agent/lib/src/feature/
  registry.cpp
  value_cache.cpp
  points_context.cpp   # countTokens / summarize 的核心实现（后续点各自加文件）
agent/lib/include/agentxx/plugin/api/plugin_api.h    # 功能点表 + 钩子 Ex 结构体 + 钩子表尾部三项
agent/lib/include/agentxx/plugin/plugin_interfaces.h # 接口名常量（协商）
agent/lib/include/agentxx/plugin/api/plugin_kit.h    # kit 糖（define / provide / unprovide / call + hookEx / listHooks）
agent/lib/src/plugins/plugin_manager_feature.cpp     # 功能点表入口实现（宿主侧）
agent/lib/src/plugins/plugin_manager_hooks.cpp       # 钩子注册表 + 派发器（默认带 / 裁剪 / 清单）
agent/lib/src/plugins/plugin_manager_vtable.cpp      # 表实例化 + query_interface 分支
agent/lib/include/agentxx/plugin/plugin_manager.h    # 实例记录字段 + 清单字段 + 计数
agent/lib/src/plugins/plugin_manager_lifecycle.cpp   # detach / clear / inventory 扩展
agent/lib/src/agent/assembly_snapshot.cpp            # features / hooks 段
agent/lib/src/util/diagnostics.cpp                   # --dump-diagnostics 接清单
agent/lib/include/agentxx/agent/io/wire_protocol.h   # wire 消息（list / call / impl）
agent/lib/include/agentxx/agent/io/agent_io_transport.h
agent/lib/src/agent/io/session_server_agent_io.cpp   # 服务端处理与派回客户端
agent/lib/include/agentxx/ffi_api.h                  # FFI 导出（list / call / register / respond + 新事件）
agent/lib/src/ffi/ffi_api.cpp / ffi_client_io.cpp / ffi_runtime.cpp
agent/lib/ffi_symbols.map                            # 导出白名单
agent/lib/include/agentxx/agent/config.h / config_static.h  # dev_mode（开发者模式，启动期冻结）
agent/client/main.cpp                                # 子命令 feature --list / --call / --dump-config + --dev
agent/lib/src/middlewares/summarization.cpp/.h       # 改为经功能点取 token 数 / 摘要（行为不变）
agent/lib/src/nodes/toolcall.cpp                     # （阶段 7）工具输出摘要接点
agent/test/include/agentxx-test/core/test_feature_points.h     # 核心侧测试
agent/test/include/agentxx-test/plugin/test_plugin_feature.h   # 插件侧端到端测试
agent/test/include/agentxx-test/plugin/test_plugin_hooks.h     # 钩子清单与优先级测试
agent/test/include/agentxx-test/plugin/test_plugin_cleanup.h   # 扩展：登记计数回基线
agent/test/include/agentxx-test/core/test_assembly_snapshot.h  # 扩展：features / hooks 段
agent/test/include/agentxx-test/core/test_ffi_c_api.h          # 扩展：FFI 功能点导出
agent/test/include/agentxx-test/core/test_wire_roundtrip.h     # 扩展：wire 消息往返
agent/test/include/agentxx-test/core/test_jsonl_mode.h         # 扩展：jsonl 里的 feature 消息
docs/zh-cn/design/feature-points.md                  # 新设计文档（作者指南 + 清单）
docs/zh-cn/design/plugins.md                         # §8 接口表 + 新一节"功能点"
docs/zh-cn/design/ffi.md                             # §4.x 功能点 FFI 导出
AGENTS.md                                            # 接口表数量与功能点摘要
```

## 15. 迁移计划（7 个阶段，每阶段一个提交）

**阶段 1：子系统骨架（纯 lib 内部，无插件面）**

* `agentxx/feature/`：`ProvidePoint` / `DecidePoint` 模板、`Registry`、值缓存、身份、置空、
  两层顺序（`plugin` → `core`）与**默认优先级带**（插件 0 / 宿主 1000）与裁剪、两处超时、
  清单与统计；`AgentContext` 增加 `features`，`BaseAgent::init` 创建；
* **开发者模式开关**：`AgentConfig::devMode`（yaml `dev_mode`）+ 启动时镜像到
  `AgentConfigStatic::devMode`，统计与派发记录按它门控（§11.4）；
* 测试模块 `feature_points`：用假的 C++ 实现覆盖顺序 / 默认带 / 裁剪 / 置空 / 值缓存 / in-flight /
  调用保护 / 两处超时 / 清单与开发者模式门控；
* 验证：`agentxx_test feature_points` + 全量测试不回归（此时还没有任何核心功能迁移）。

**阶段 2：第一批核心点迁移（行为不变）**

* `agentxx.context.countTokens`：估算规则搬进核心实现（只留一份），中间件旧函数改为转发，
  `service.token.count` 同步服务改为直连核心实现（TPS 不受影响）；
* `agentxx.context.summarize`：中间件改为经点取摘要文本，编排（提示消息 / 写回 / 兜底 / 冷却）留在中间件；
  点先声明为 `callable = false`（§8.2），`implTimeoutMs = 0`（默认不限）；
* 验证：`agentxx_test feature_points summarization event_stream agent`；
  手工：跑一轮超长上下文确认自动压缩与手动压缩（TUI 的压缩入口）行为不变。

**阶段 3：C ABI 与 kit（插件登记实现）**

* `plugin_api.h` 功能点表 + `plugin_interfaces.h` 名字 + vtable 分支 + `plugin_manager_feature.cpp` +
  kit 糖（`defineFeaturePoint` / `provideFeature` / `unprovideFeature` / `callFeature`）；
* 登记策略：**不设数量上限**；`priority` 越界裁剪 + 警告；超时字段透传（默认不限）；
* 实例记录与清理（`detachDomainRegistrations` / `clearDomainRegistrations` / `registrationInventory` /
  `PluginListView`）；`kInterfaceTableCount` 与三处文档同步；
* 测试模块 `plugin_feature`：测试插件登记实现、核心 `ask` 拿到插件值、`list_points` 读清单；
  扩展 `plugin_cleanup`（禁用 / 卸载后登记计数回 0）与 `plugin_multi_instance`（同一动态库两个实例各自登记）；
* 验证：`agentxx_test plugin_feature plugin_cleanup plugin_multi_instance plugin_sdk`。

**阶段 4：对外开放调用与插件自定义点**

* `callable` / `callDoc` / `call()` / 三道保护 / 结果与错误码 / **两处超时**（`implTimeoutMs`、
  `timeoutMs`，都默认 0 = 不限）；把 `countTokens` 声明为可调；
* `define_point` / `undefine_point`（命名空间校验 + `impl_timeout_ms`）+ 插件点调用；
* 清单进装配快照与 `--dump-diagnostics`；调用统计与成功调用日志按开发者模式门控（§11.4）；
* 测试：`feature_points` 扩展（`not_callable` / `bad_args` / `busy` / 调用不写值缓存 / 不记置空 /
  调用方全是自己 / 超时两条路径 / 开发者模式开关 / 插件点端到端）+ `plugin_feature` 扩展；
* 验证：`agentxx_test feature_points plugin_feature assembly_snapshot observability`。

**阶段 5：钩子处理器清单与优先级（独立，可与 1~4 并行）**

* `AgentxxPluginHookSpecEx` + 钩子表尾部三项 + `plugin_manager_hooks.cpp`（注册表 + 单派发器）+
  旧的按插件派发移除；kit `hookEx` / `listHooks`；
* 默认带与裁剪、不设数量上限；派发记录按开发者模式门控；清单进装配快照 `hooks` 段与诊断；
  `RegistrationInventory` / `PluginListView` 字段同步；
* 测试模块 `plugin_hooks`：默认顺序与现状一致（同优先级按登记顺序）、显式优先级生效、
  同一实例同一点多个处理器、优先级越界裁剪 + 警告、撤销与禁用摘生效、清单字段、
  开发者模式开关对派发记录的影响；
* 验证：`agentxx_test plugin_hooks plugin_runtime plugin_cleanup plugin_multi_instance boundaries`。

**阶段 6：命令行与 FFI 接入（多方统一）**

* wire 消息（`feature_list` / `feature_call` / `feature_impl_*`）+ 服务端分发 + 客户端侧实现
  （`timeoutMs` 透传：到点取消当次实现并回 `failed`）；
* FFI 导出（`feature_list` / `feature_call` / `feature_register` / `feature_unregister` / `feature_respond`）
  + 新事件 + `ffi_symbols.map` + ffi.md + Dart 绑定重新生成；
* 命令行子命令 `feature`：`--list` / `--call <id>`（`--args` / `--timeout-ms` / `--session`）/
  `--dump-config`，以及 `--dev` 与 `--agent`（§9.6）+ 帮助文本；
* 测试：`ffi_c_api`（导出存在、清单、调用结果事件、宿主登记实现的回派与断开清理、超时分支）、
  `jsonl_mode` / `wire_roundtrip`（消息往返与清单）、手工跑一次 `feature --call` 与 `jsonl` 调用；
* 验证：`agentxx_test ffi_c_api jsonl_mode wire_roundtrip wire_schema test_framework` + 手工核对。

**阶段 7：第二批点与文档收尾**

* 选做（按价值与风险排序，可在实施时砍项）：`agentxx.tool.summarizeOutput`、
  `agentxx.context.repairMessages`、`agentxx.permission.decide`（只允许收紧）、`agentxx.tool.select`；
* 文档：新增 `docs/zh-cn/design/feature-points.md`；`plugins.md` §8 与新增一节；
  `ffi.md` 功能点一节；根 `AGENTS.md`（接口表数量 20、功能点摘要、作者口径、钩子清单口径、开发者模式）；
  插件作者文档补"provide / decide 的写法、`value` 形状、`caller` 纪律、不阻塞 IO 线程、
  钩子优先级、默认优先级带与超时口径"；
* 示例：在某个内置插件或测试插件里保留一个"声明点 + 实现 + 调用应用点"的最小例子；
* 验证：全量测试 + 手工核对清单与诊断输出（含 `list_hooks`）；开发者模式开 / 关各跑一次对比输出差异。

依赖关系：阶段 2 依赖阶段 1；阶段 3 依赖阶段 2 的至少一个真实点；阶段 4 依赖阶段 3；
**阶段 5 与 1~4 互不依赖**（只动钩子这条路径）；阶段 6 依赖阶段 3、4；阶段 7 最后做，
可分批提交（每个点一个提交，方便回退）。

## 16. 测试点

**核心侧（模块 `feature_points`）**

* **顺序与默认带**：两层实现按 `plugin → core` 问；层内按 `(priority 升序, 登记顺序)`；
  用一个"记录调用顺序"的假实现验证；**FFI 宿主登记的实现默认排在插件之后、core 之前**（默认带 1000），
  显式声明 `priority` 可以覆盖默认带；`priority` 越界被裁剪到上下限（用清单里读回的生效值断言）且记警告。
* **不设数量上限**：同一个点登记多个实现、一个实例登记多个点都能成功（不因数量被拒）。
* **异常隔离**：实现抛异常 / 返回不合法 JSON → 记警告、继续问下一个；链条全无 → `no_impl`。
* **置空**：实现返回 `disable` → 本次 `disabled` 且不再问后面；`ask` 之后清单有 `disabledBy`；
  摘除该实现后标记自动清掉；`call` 不记标记（随后 `ask` 仍会问实现链）。
* **值缓存**：`ByIdentity` 命中（`fromCache`）；换 `identity` 不命中；`refresh = true` 跳过；
  条数与字节双上限（构造超限值验证不入缓存 / 淘汰）；
  **`call` 前后值缓存条数与产出方不变**（核心用例）；in-flight 去重（同一身份并发只跑一次）；
  `None` 策略的点不产生任何条目（含工具输出摘要，§8.3）。
* **来源失效**：实现摘除 / 插件禁用后，它产出的条目被删除。
* **身份**：显式 `identity` 优先；默认身份由点算；身份不含"长度参数"。
* **调用保护**：载荷带 `caller` / `viaCall`；实现全是调用方时跳过插件层（仍然拿到 core 值）；
  同一 `(点, 调用方)` 重入 → `busy`。
* **超时**（两条路径分开测）：① 实现方挂住 + `implTimeoutMs` 到点 → 取消该实现、继续链、最终拿到 core 值
  （整体成功，不是失败）；② 调用方给 `timeoutMs` 到点 → 结果 `failed` 且 `message` 含限时值，
  当次实现被取消；③ 都不给（默认 0）→ 慢实现最终仍能给出值（默认不限）。
* **声明校验**：`callable = false` → `not_callable`（含 `summarize`，§8.2）；`callDoc` 出现在清单里；
  插件实现的登记不再需要任何"允许开关"（没有 `allowPluginImpl` 这类字段）。
* **清单与统计**：`list_points` 的字段齐全（type / callable / callDoc / cache / impls / effectiveBy）；
  `layer` 只有 `plugin` / `core`；**开发者模式关**时没有 `stat` 段与派发记录，**开**时
  `asks` / `calls` / `errors` / `lastBy` / `lastCaller` 计数正确。
* **开发者模式**：开关在启动时读取后**冻结**（运行期改配置不生效）；关闭时成功调用不记 info 日志、
  钩子派发不记录（可断言日志条数）；开关状态出现在装配快照（`dev_mode`）。

**核心点迁移（扩展现有模块）**

* `summarization`：迁移后自动压缩、手动压缩、失败兜底、提示消息复用、写回形状全部不变；
  token 计数与中间件旧函数结果一致（抽样对比）。
* `event_stream`：TPS 计算在缺少 / 存在同步服务时行为与迁移前一致（快路径不受功能点与超时影响）。

**插件侧（模块 `plugin_feature`）**

* 测试插件为 `agentxx.context.countTokens` 登记实现 → 核心 `ask` 拿到插件给的值，`by` 是 `plugin:<名>`；
  摘除实现后回到核心实现。
* 插件调用应用点（`call_point_async`）：拿到值 / `not_callable` / `no_impl` / `disabled` / `busy` 五条路径；
  调用**不写值缓存**（调用前后 `list_points` 的 cache 统计与核心 `ask` 的缓存命中行为不变）；
  调用方 `timeoutMs` 到点 → `failed`。
* 插件定义点（`define_point`）→ 另一个插件（或同一插件的另一个实例）登记实现 → 调用拿到值；
  `plugin.<别人>.*` 的 id 被拒绝；重复声明覆盖；`undefine_point` 后调用 `no_impl`；
  点声明里的 `impl_timeout_ms` 生效（到点取消当次实现）。
* 清理：禁用 → 点与实现摘生效；卸载 → 全摘；重新启用 → `start` 重新声明；
  `plugin_cleanup` 的登记计数回 0。
* 多实例：同一动态库在两个 agent 实例里各自声明与实现，互不影响（`plugin_multi_instance`）。
* 旧宿主降级：表指针为空时 kit 返回可读错误（不崩溃）；`interfaces.optional` 声明时插件整体照常加载。

**钩子（模块 `plugin_hooks`，扩展 `plugin_runtime`）**

* 默认顺序：都不声明 `priority` 时，处理器顺序 = 登记顺序 = 装载顺序（与迁移前一致）。
* 显式优先级：`-10` 的处理器先于 `0`；同优先级按登记顺序；`unregister_hook_ex(handle)` 精确撤销。
* **不设数量上限**：同一实例同一点登记多个处理器、一个实例登记多个点都成功；
  优先级越界被裁剪 + 记警告（清单里读回的生效值可断言）。
* 禁用 → 该实例处理器不生效（派发时跳过）且不改变其他处理器相对顺序；卸载 → 全部摘除。
* 清单：`list_hooks` 字段齐全（point / name / handlers / handle / layer / owner / priority / seq / enabled）；
  `hooks` 段进装配快照；`RegistrationInventory::hooks` 计数正确；
  开发者模式关时清单里没有派发记录。

**命令行与 FFI（扩展 `ffi_c_api` / `jsonl_mode` / `wire_roundtrip` / `wire_schema`）**

* wire 消息往返（`feature_list` / `feature_call` / `feature_impl_register` / `feature_impl_call` / 结果）
  与 `wire-schema.json` 同步（schema 校验用例）；`feature_call` 带 `timeoutMs` 时服务端到点回 `failed`。
* FFI 导出存在且白名单包含（`ffi_symbols.map`）；`agentxx_ffi_feature_list` 返回清单；
  `agentxx_ffi_feature_call` 受理 → `EVT_FEATURE_RESULT`（`requestId` 配对、`ok:false` 分支）；
  宿主登记实现 → `EVT_FEATURE_CALL` → `feature_respond` → 得值；客户端断开 → 实现摘除 + 未应答调用按"没意见"。
* `jsonl` 模式：`{"type":"feature_list"}` / `{"type":"feature_call"}` 有对应 `..._result` 行；
  老客户端不认识新消息时不出错（忽略）。
* 命令行子命令：`feature --list` 输出清单后退出；`feature --call` 成功 / 失败两种退出码与 stdout 内容；
  `--timeout-ms` 生效；`feature --dump-config` 输出与装配快照 `features` 段一致；`--agent` 走 wire；
  `feature --help` 列出三个动作。

**快照与诊断**

* `assembly_snapshot`：`features` / `hooks` 段存在，点数 / 可调点数 / 生效实现 / 处理器顺序与预期一致；
  含 `dev_mode` 一行。
* `observability`（`--dump-diagnostics`）：输出里有功能点与钩子清单，且不含参数正文。

## 17. 风险与坑

1. **行为不变的风险**（阶段 2 最重）：压缩是长链路（中断 / resume / 子代理 / 写回 / 兜底），
   改接点时必须保持编排逻辑与顺序不变；用 `test_summarization` 全量回归 + 手工长上下文验证兜底。
2. **热路径被拖慢**：TPS 的 token 估算不能变成异步；保留同步快路径（§8.1），
   并加一条测试守住"同步服务直连核心实现"；超时检查与统计累加都不能进这条路径。
3. **钩子顺序改变的影响**（阶段 5）：默认 `priority = 0` 时顺序与现状一致，但一旦有插件声明了优先级，
   其他插件观察到的相对顺序会变。缓解：清单与派发记录（开发者模式）能看出实际顺序；
   文档写明"顺序是声明出来的"。
4. **接口表数量三处一致**：`kInterfaceTableCount` / `plugins.md` §8 / 根 `AGENTS.md`
   与 `plugin_interfaces.h` 要一起改（测试会校验）。
5. **表结构体尾部扩展**：钩子表新增三项依赖 `struct_size` 守卫；实现时要按"插件报的 `struct_size`
   小于新增后尺寸 → 当没有这三项"处理，否则老插件会读到越界指针。
6. **多实例与全局状态**：注册表、值缓存都在 `AgentContext` 上，**禁止函数级 static 缓存**
   （多实例三铁律）；唯一的进程级状态是开发者模式那个只读布尔标记（启动期写入后不再改）。
   测试用 `plugin_multi_instance` 覆盖。
7. **多入口一致性**：核心调用、插件接口表、wire（FFI / 远程）、命令行子命令五条入口必须打到同一份语义
   （同一实现链、同一清单、同一错误码、同一超时口径）。任何"某条入口自己处理一遍"的写法都是 bug；
   用同一份 `Registry` + 同一份清单实现来保证（§9.5、§9.6、§11.1）。
8. **跨边界拷贝**：请求与结果都是 JSON 文本；大请求（整段上下文）与大会话并发时要设上限并记日志，
   避免一次压缩把内存抬高。
9. **值缓存内存**：默认策略是 `None`；开了 `ByIdentity` 的点必须给 `maxBytes`，且单条超限不入缓存。
10. **插件覆盖核心功能**：`plugin` 层最优先，用户装的插件可以替换压缩这类核心行为；
    清单与日志要能立刻看出"谁在生效"（`effectiveBy`），避免出现"行为变了但没人知道"。
11. **插件实现卡住**：默认不限时（`implTimeoutMs = 0`），所以实现返回慢就会拖住这一轮 ——
    这是"默认不限制、由调用方决定"的代价。缓解：声明方按需给 `implTimeoutMs`（一行改动），
    调用方按需给 `timeoutMs`；开发者模式下有耗时记录可看出是哪个实现慢。
12. **安全边界**：裁决型只采纳"收紧"（插件不能放行）；功能点调用不做运行时权限校验（框架口径），
    但要点也要清楚"实现能看到请求里的数据"（例如权限请求里的路径），文档写明。
    宿主登记的实现（FFI）同样能看到请求内容 —— 宿主本来就在同一个进程里，不额外放大权限。
13. **开发者模式的边界**：① 必须**启动期冻结**（运行期切换会让数据半途开始 / 停止，也会让热路径分支抖动）；
    ② 只能控制"记录数据的收集"，**不得**顺手改变行为（实现链、缓存、超时、日志级别）；
    ③ 任何出口都要能在关闭时正常出清单（不能出现"只有开了才能看清单"的依赖）。
14. **提示词稳定性**：不要把功能点结果直接写成每轮变化的 system 段（会打破 KV 前缀缓存）；
    记忆 / 技能类点要落在"稳定前缀 + 显式刷新时刻"上（后续批次单独设计）。
15. **协程帧与 LTO**：插件实现与核心调用都在既有操作协议与桥接上跑，不新增跨边界协程帧；
    核心侧新增协程若帧很大，按 `AGENTS.md` 的 `AGENTXX_NOINLINE` 约定处理。
16. **命名与文档漂移**：点 id / 展示名 / 说明集中在 `points.h`，文档表格与测试引用同一份常量；
    插件点 id 的命名空间校验必须与事件主题前缀规则保持一致的心智模型；
    FFI 与 wire 的字段名跟着 `wire-schema.json` 走，改一处要同步生成物与文档。

## 18. 路线图（本轮不做，登记在案）

| 项 | 说明 | 前缀条件 |
|---|---|---|
| 通知型功能点（`observe`） | 让"广播一次事件"也进同一份清单（当前由事件表 + 钩子承担） | 出现"要清单、要顺序、要统一写法"的真实用例 |
| `agentxx.context.summarize` 对外开放调用 | 本轮 `callable = false`：核心实现依赖宿主派生压缩子代理，语义要先想清（谁付 token、失败怎么回） | 评估成本后再定 |
| 插件点的值缓存策略 | 本轮插件点固定 `None`；将来按应用点同一套策略开放（按点声明） | 出现"插件点被反复调用"的实测数据 |
| 第二批点 | `agentxx.tool.summarizeOutput`（阶段 7 落地）、`agentxx.context.repairMessages`、`agentxx.permission.decide`、`agentxx.tool.select`、`agentxx.memory.retrieve`、`agentxx.skill.match`、`agentxx.subagent.run`（见 §8.4） | 按价值排序，逐个提交 |
| 核心侧钩子处理器 | 核心经同一注册表登记处理器（owner `core:<模块>`），让钩子清单完整反映"谁会处理" | 出现"核心也要插一手"的用例 |
| FFI 宿主登记钩子处理器 | 宿主经同一份 wire 登记钩子处理器（同表尾部三项 + 默认带 `1000`），语义与功能点实现一致 | 出现"宿主也要看生命周期"的用例 |
| 功能点的界面展示 | TUI 的 Info / 设置页 / 诊断弹窗里显示功能点与钩子清单（含调用统计） | 用户需要"看得见谁在提供功能" |
| 值缓存的持久化 | 跨进程 / 跨会话复用（当前明确不落盘） | 有实测收益再说（内存与失效复杂度都要重新评估） |
| 宿主点的自定义声明 | FFI 宿主用 `declare` 声明自己的点（`host.<宿主名>.*`）已在设计里（§9.5），先只做"声明 + 实现 + 调用"的最小闭环 | 阶段 6 完成后再评估扩展 |
| 开发者模式扩展 | 更多子系统接入（例如工具分发、模型调用的按需记录），或与 benchmark 标记合并成一个"诊断级别"配置 | 出现第二个需要"按需收集"的子系统 |

## 19. 口径记录

### 19.1 已定（本次按此实施）

1. 功能点子系统在 libagentxx（`agentxx::feature`），随 `AgentContext` 一份；插件面为新接口表
   `agentxx.agent.feature` v1，agent 侧接口表 9 → 10（总数 19 → 20）。
2. 两类型：`provide` / `decide`；`notify` 预留枚举值，本轮不实现（路线图 §18）。
3. 层顺序只分 `plugin` → `core`（agentxx 只有一套插件框架；`builtin://` 与动态库同层）；
   层内**默认优先级带** = 插件 `0` / FFI 宿主 `1000`（宿主默认排在插件之后、core 之前）；
   层内 `(priority, 登记顺序)`；**`priority` 越界裁剪到上下限 + 警告日志**（不拒绝）；不链式传值。
4. **没有 `allowPluginImpl`**：埋点天生接受外部实现（§3.1）；`callable` 只决定"能不能被外部调用"。
5. **不设注册数量上限**：点的个数、每个点的实现数、每个实例的钩子处理器数都不限；
   只保留必要校验（命名空间 / id 非空 / 参数形状）与重入保护（`busy`）。
6. 值缓存：`None` / `Latest` / `ByIdentity`；in-flight 去重；按来源失效；双上限；只在内存；
   插件点本轮固定不缓存；工具输出摘要不缓存不设上限（原文由它自己的库管理）。
7. 显式置空：`ask` 记标记（清单可读 `disabledBy`）、`call` 不记；实现摘除自动还原。
8. `callable` 默认关闭，点自己声明；`call` 零副作用（不写值缓存 / 不记置空 / 不写会话 / 不发提示 / 不落盘）。
9. 三道保护（`caller` 与 `viaCall` 入载荷、实现全是调用方就不问插件层、同 `(点, 调用方)` → `busy`）。
10. **超时两处都实现、默认都 0 = 不限**：`implTimeoutMs`（点声明方，到点取消当次实现并按"没意见"继续链）、
    `timeoutMs`（调用方，到点取消当次实现并返回 `failed`）；插件糖 / FFI / 命令行 / `jsonl` 都接受调用方超时。
11. 插件自定义点只允许 `provide`；命名空间强校验；声明与实现随实例生命周期；点声明可带 `impl_timeout_ms`。
12. 全异步、不提供同步等待入口。
13. 钩子补处理器清单与优先级（§6）：`register_hook_ex` / `unregister_hook_ex` / `list_hooks`，
    有序注册表 + 单派发器；7 个点、载荷、结果丢弃语义不变；默认顺序与现状一致；处理器数量不限。
14. 多方接入（§9.5、§9.6）：wire 消息 + FFI 导出（list / call / register / respond）+
    子命令 `feature --list / --call / --dump-config`（含 `--timeout-ms` / `--dev` / `--agent`）与 `jsonl`；
    核心、插件、宿主、脚本同一套语义。
15. **开发者模式**（§11.4）：`dev_mode`（yaml / FFI `devMode` / CLI `--dev`）启动期读取并冻结；
    开启才收集调用统计、钩子派发记录、成功调用日志；只控制收集、不改变行为；
    装配快照里可见 `dev_mode`。
16. 工具 / 事件 / 能力 / 提示词 / 资源 / 图节点不合并；`service.*` 逐步被功能点吸收。
17. 第一批点：`agentxx.context.countTokens`（可调）、`agentxx.context.summarize`（不可调）、
    `agentxx.tool.summarizeOutput`（阶段 7 落地，不可调）。
18. `agentxx.context.countTokens` 的值缓存参数：`ByIdentity`、`max_items = 64`、`max_bytes = 256 KiB`（已确认）。
19. 旧宿主判定：表指针为空 = 宿主太旧，插件按"功能不可用"降级并记日志；不停用整个插件；
    FFI / wire 侧老宿主同样降级（清单为空、调用 `no_impl`）。
20. 清单（功能点 + 钩子）进装配快照与 `--dump-diagnostics`；界面展示进 §18 路线图。

### 19.2 待确认（可以推翻，成本写在括号里）

1. **开发者模式的开关命名与入口**：草案是 yaml `dev_mode` + FFI `devMode` + CLI `--dev`；
   若想沿用别的名字（如 `diagnostics.mode: basic|full`）或再加 TUI 设置项，改动只在配置解析与文案（成本低）。
2. **`priority` 允许范围**：草案 ±1000（越界裁剪 + 警告）；数值可按实际需要调整（成本低）。
3. **是否给压缩点设 `implTimeoutMs`**：草案保持 `0`（不限），由调用方按需给 `timeoutMs`；
   若实测出现"插件实现卡住压缩"，再给该点加一个声明值（成本低）。
4. **`feature --dump-config` 的输出边界**：草案是与装配快照 `features` 段一致的文本；
   若希望它顺带打印 `hooks` 段或完整快照，改动在渲染函数（成本低）。
5. **FFI 宿主实现的 owner 命名**：草案 `host:<客户端标识>`，`declare` 声明的宿主点用
   `host.<宿主名>.<名>`（成本低，但要同步文档与测试）。
6. **开发者模式是否与其他诊断开关联动**：草案是完全独立（`--dev` 不带动 benchmark、不改日志级别）；
   若希望一个开关同时打开 benchmark 统计，需要把两者接到同一处解析（成本低）。
