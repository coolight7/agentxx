# 插件宿主与多 agent 挂载（plugin-multi-agentnode）

> 设计文档。本次是**破坏性重构**：不保留旧插件、旧接口表、旧配置写法的兼容性。
> 开工按 `04-implementation.md` 的阶段推进。

## 0. 怎么读

| 文档 | 内容 |
|---|---|
| `index.md`（本文） | 需求、结论、术语、范围、待确认、风险 |
| `01-analysis.md` | 现状分析（管理器与 agent 的耦合、启动路径、现存兼容负担清单）、方案对比 |
| `02-host-mount-design.md` | 主体设计：宿主/挂载分层、agent 引用、生命周期、逐表语义、配置、ABI v2、兼容清零清单 |
| `03-plugin-side.md` | 插件侧契约：入口职责、`mount` 写法、状态与缓存、迁移与示例 |
| `04-implementation.md` | 阶段拆分、文件级改动、测试计划、风险与工作量 |

## 1. 需求

第一条（2026-10-22，原话）：

> 插件的加载可能得考虑重构了，这个应当重构成在 AgentHost 实现插件加载，让整个进程只有一份，
> 只加载一次、统一释放、管理和配置，然后每个 agentnode 创建时再挂载需要的插件，注册 tool
> 和各种接入点。插件也需要重构设计思路为 一次加载、可被多个 agentnode 挂载，如果有设计调整
> prompt、配置 等 agent 相关内容，则应当在挂载上做，且如果有缓存、变量存储，也需要设计支持
> 不同 agent 同时挂载的情况。

第二条（本轮追加）：

> 本次重构不用保留旧版本和旧插件的兼容性，可以修改接口表；趁着本次破坏性重构，把之前的旧版本
> 兼容性处理之类的去掉，清理代码和文档，让整体更简洁清晰。

第三条（本轮追加，版本与传参）：

> 插件获取接口表的 version 校验的流程不应该是：插件传入编译时使用的框架 SDK 版本、最小兼容的
> 框架版本，然后框架判断插件使用的框架版本是否 >= 当前框架的 MIN-Version，如此即可判断是否
> 返回接口表，然后插件再通过结构体大小等判断取成员变量，这样应该能做到双向兼容，所以我认为是
> 直接提升表的 VERSION 和 MIN_VERSION 就可以了，iid 什么的都不需要修改，而且本次重构我们还
> 更新了框架的 MIN_VERSION，那旧插件应该一开始就拒绝加载了。重构接口如果是需要区分 agent 的，
> 都应当带上 agentid 参数，避免影响其他 agent，因为未来可能子 agent 也会需要区分，要理清设计，
> 挂载时就应当记录 agentid，后续 agent 相关的接口都需要通过这个结构体或传 agentid 来确定。

合起来是七条可验收的要求：

1. 加载上移到 `AgentHost`，整个进程一份，只加载一次。
2. 释放、启停、禁用、配置由这一份统一处理。
3. 每个 agent 创建时挂载自己需要的插件，注册工具与各类接入点。
4. 插件一次加载、可被多个 agent 挂载；缓存/变量要支持多 agent 同时挂载。
5. **`prompt`、配置等 agent 相关内容一律在挂载阶段做**（不在加载阶段登记、之后再补）。
6. **不保留兼容**：接口表可以改、可以删字段、可以合并成员；旧插件必须重新编译。
7. 顺手清掉历史兼容处理与相关文档，整体更简单。

## 2. 核心结论

| # | 结论 | 落在哪 |
|---|---|---|
| R1 | 插件宿主上移：`AgentHost` 持唯一 `PluginManager`，一插件一份实例，`create/start` 各一次 | `02` §2、§4 |
| R2 | 每个 agent 一份 `PluginMount`；**所有 agent 侧注册都发生在 `mount` 回调里**（工具/权限/钩子/提示词/功能点/图/资源/事件） | `02` §4.2、§5 |
| R3 | 阶段职责硬性划分：`start` 只做进程级；在 `start` 里调 agent 侧注册接口 → 返回 `AGENTXX_PLUGIN_ERR_NO_AGENT`（不再有"声明层"） | `02` §3、`03` §2 |
| R4 | **需要区分 agent 的接口一律显式带 agent 引用**（`PluginxxScopeRef` = agentId + 挂载序号）；宿主不保存"当前挂载"，未带或带错 → `ERR_NO_AGENT` | `02` §3 |
| R5 | `mount`/`unmount` 成为与 `start`/`stop` 并列的入口（同一套操作协议），`mount` 缺省 = 该插件无 agent 侧能力 | `02` §4.2、`03` §2 |
| R6 | 配置拆两段：`plugin.list`（加载，进程一次）+ `plugin.mount`（每 agent 的挂载策略：`all`/`none`/列表 + 参数覆盖） | `02` §9 |
| R7 | **接口契约换代到 v2**：全局 `PLUGINXX_API_VERSION`/`MIN_API_VERSION` 提到 2（拒绝 v1 插件）；改动过的 14 张表 `VERSION`/`MIN_VERSION` 提到 2、基线重置为新 `sizeof`，**IID 一个都不改**。**完整保留** `version`/`MIN_VERSION`/`BASELINE`/`struct_size`、`ifaceHas`、`specSizeOk`/`specHas`、`validateInterface` 等演进机制 | `02` §10 |
| R8 | 一次性删除历史兼容：`promptBackup`、`attachRootAgentHost` 与宿主收养、legacy 配置键告警、hooks 基础/扩展双入口、重复的 `op_cancel` | `01` §4.3、`02` §11 |
| R9 | 会话定位 = (agent 引用, sessionId) 两层；不再有"按 sessionId 猜 agent"，只剩"同一存储根被两个 agent 打开"这种配置错误，由 `AgentHost` 拒绝 | `02` §7 |

一句话：**加载一次是进程的事，注册一次是 agent 的事；不在挂载里的注册直接报错，不再假装兼容。**

## 3. 术语

| 术语 | 含义 | 代码落点 |
|---|---|---|
| 插件宿主 | 进程级插件容器：插件表、动态库、生命周期、配置 | `agentxx::plugin::PluginManager`（语义变为宿主） |
| 加载 | `dlopen` + `create` + `start`（+ 清单解析），每插件一次 | `PluginHostLifecycle` |
| 插件挂载 | 某个 agent 使用某个已加载插件的实体 | `agentxx::plugin::PluginMount`（新增） |
| agent 引用 | 指向"哪个 agent 的挂载"的值：agentId + 挂载序号（防 id 复用） | `PluginxxScopeRef`（+ SDK `AgentRef`），见 `02` §3 |
| 根挂载 | 把额外 root agent 挂到 `AgentHost`（下一篇 `../plugin-event/` 才做） | `AgentHost::mountRoot` |
| ABI v2 | 本次的接口契约换代：API/MIN 提到 2；改动过的 14 张表 VERSION/MIN 提到 2，IID 不变；`version`/`struct_size`/`ifaceHas` 等机制保留 | `02` §10 |
| 自建宿主 | `BaseAgent::init` 没有外部宿主时自建一份（单 agent 用法 / 测试） | `PluginManager::createSelfHosted` |

> 本篇"挂载"一律指**插件挂载**；涉及额外 root agent 时写"根挂载"。

## 4. 范围与非目标

**做**：

- `PluginManager` per-agent → per-host；`PluginMount` 承载全部 agent 侧注册。
- `mount`/`unmount` 入口、`MountSpec`、`agentxx.agent.mount` 表、SDK（`MountStore` 等）。
- 全部 agent 级接口加 `agent` 引用参数（21 张表里 14 张改签名 + 1 张新增）。
- 接口契约换代到 v2（`PLUGINXX_API_VERSION` 2、`MIN_API_VERSION` 2；14 张表 `VERSION`/`MIN` 提到 2，**IID 不变**），**保留全部面向未来的演进机制**。
- 配置两段（`plugin.list` / `plugin.mount`），删除 legacy 键与告警。
- 五处启动路径改为"先建宿主 → 加载 → init（挂载）→ attachRoot"。
- 内置插件全部改到 `mount` 里注册（机械改动，一次性）。
- 测试与文档同步（含 `plugins.md` §2/§8/§17 与 `AGENTS.md` 的契约段）。

**不做**：

- 插件创建/回收 root agent、事件驱动输入、群聊通道（`../plugin-event/`）。
- 客户端侧的表结构调整（客户端插件跟随 API/MIN 换代，成员不动）。
- MCP 连接上提为宿主级共享（仍按挂载各连一条）。
- 插件的跨进程部署、插件市场、依赖版本约束。

## 5. 关键取舍

| 取舍 | 选择 | 理由 |
|---|---|---|
| 注册时机 | 只能在 `mount` | 需求第 5 条；删掉"声明层"后语义唯一，不再出现"这里注册和那里注册效果不同" |
| 阶段越界 | 直接报错 `ERR_NO_AGENT` | 静默降级会隐藏 bug；错误信息直接告诉作者"请在 mount 里注册" |
| 传参 | **显式 agent 引用**（每个 agent 级接口多一个参数） | 隐式"当前挂载"在多 agent 下必然出错：插件自建线程、定时器、任务回调都得猜，猜错就串会话。签名改动是一次性的机械工作 |
| 版本策略 | **保留全套演进机制**；用 `MIN_API` 1→2 + 14 张表 `VERSION`/`MIN`→2（IID 不变）来抛弃历史兼容 | 抬下限挡住旧插件，升表版本挡住旧主程序；`struct_size`/`ifaceHas` 仍是以后追加成员唯一的手段 |
| hooks 入口 | 合并基础/扩展为一种 | 覆盖式语义可用"撤销 + 再登记"表达，少一张分支与一个 `base` 标志 |
| `op_cancel` | 收拢到 `pluginxx.cancel` | 三张表各一份同语义成员是重复 |

## 6. 待确认问题

| # | 问题 | 默认取法 | 影响 |
|---|---|---|---|
| Q1 | `plugin.mount[].args` 之外，是否还需要框架级的"挂载级工具裁剪"？ | 不需要：插件在 `mount` 里按本挂载配置自行决定注册哪些工具 | 框架少一份语法 |
| Q2 | 用户手工改过提示词后，插件贡献是否仍然叠在上面？ | 沿用贡献模型（基础值 → 插件贡献） | 只影响可预期性 |
| Q3 | 多根同名会话怎么处理？ | ~~已关闭~~：显式 agent 引用后不再按 sessionId 猜 agent；只剩"同一存储根被两个 agent 打开"，由 `AgentHost` 拒绝（`02` §7.3） | — |
| Q4 | v1 插件被拒绝时的提示文案 | `plugin built with API v1, host requires >= v2; rebuild the plugin` | 只影响日志文案 |
| Q5 | 客户端侧是否跟随换代？ | 是：`AGENTXX_CLIENT_PLUGIN_API_VERSION` 与 MIN 都提到 2；表布局未变则不动 | 客户端插件需重编 |
| Q6 | 布局变化的表是否换 IID？ | ~~已关闭~~：**不换**。本次每张改动表 `sizeof` 都变了，旧插件被 `BASELINE` 拦；两个方向另有 `MIN_API` / `MIN_VERSION` 兜底（`02` §10.3） | — |

## 7. 风险摘要

| 风险 | 影响 | 缓解 | 详见 |
|---|---|---|---|
| 破坏性变更 | 旧的预编译插件全部失效 | 只影响外部插件；仓库内插件同 superbuild 一起重编；加载时给出明确错误 | `02` §10.1 |
| 内置插件要逐个迁到 `mount` | 20 个插件的机械改动 | 逐个改、逐个跑测试；先做示例再批量 | `04` §1、§3 |
| 删除兼容机制时漏改调用点 | 编译失败 / 测试红 | 机制删除与调用点在同一提交；`boundaries`/`plugin_runtime` 精简后仍守着"表数量与版本台账" | `04` §5 |
| 阶段越界报错误伤 | 插件作者踩坑 | `ERR_NO_AGENT` 常量 + 明确日志 + `03` 决策表 | `03` §5 |
| 漏带 agent 引用 | 运行期 `ERR_NO_AGENT` / `-1` | SDK 把引用放进 per-agent 状态对象（`MountStore`）；迁改清单逐条对照 | `03` §6、`04` §3 |
| 改动面大 | 回归 | 阶段 0/1 行为等价；每阶段独立提交 | `04` |

## 8. 与相邻设计的关系

| 设计 | 关系 |
|---|---|
| `../plugin-event/`（下一篇：插件建根 + 会话通道 + 事件触发） | 本篇是它的前提：插件加载一次后可挂到 N 个 agent；它在此基础上加“多根 + 通道”。两边接口约束已在本篇 `02` §16 固定（S1~S8） |
| `docs/zh-cn/design/plugins.md` | §2/§8/§17 按本篇重写（版本契约、表数量 21、agent 引用）；新增"插件宿主与多 agent 挂载"一节 |
| `docs/zh-cn/design/index.md` §6/§7 | 补"插件宿主归 AgentHost" |
| `AGENTS.md` | 插件段重写：一次加载/多挂载；agent 级接口显式带 agent 引用；ABI 换代（`MIN_API` 1→2、14 张表 `VERSION`/`MIN`→2、IID 不变、机制保留）；接口表 21 张 |
