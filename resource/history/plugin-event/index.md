# 插件内驱动 agent 会话：多根配置隔离 + 会话通道 + 统一运行时

- 难度: A
- 类型: 设计（待实现）
- 时间: 2026-10-22
- 状态: 方案已收敛（本文档只做设计与调研，不含代码改动）

> **前置依赖**：本篇排在 `../plugin-multi-agentnode/` 之后实施。那篇把插件加载上移到进程级
> 宿主、每 agent 挂载、agent 级接口显式带引用（`PluginxxScopeRef`）、接口契约升到 v2。
> 本篇所有结论都建立在那套模型上，跨篇约束见 `../plugin-multi-agentnode/02-host-mount-design.md`
> §16 的 S1~S8（进程一份插件宿主、根也走同一条挂载路径、在 `mount` 里建根被拒绝等）。

## 需求原文

```md
请仔细思考分析一下当前项目的插件框架和现有接口，我计划实现一个插件，支持接入
QQ、微信聊天对话和群聊，这需要修改框架支持 插件可以创建 AgentHost、或是在现有
AgentHost 上创建 root AgentNode，然后输入 input 到 baseAgent
去运行会话、绑定相关的状态响应回调和中断对接、控制，不同群聊单独一个根
AgentNode，互不影响，且也不影响 agentxx 内置默认创建的 AgentNode；而群聊消息触发 agent
input 执行，相当于是事件驱动，这个也需要思考分析是让 baseagent 支持 事件驱动 还是
插件内接收消息后直接调用 input 执行会话比较好，似乎事件驱动应该是更好的设计，可以未来扩展到绑定时间点触发、外部信号等
情况；几乎可以说是把 ffi 接口的大部分功能都搬到插件框架支持，要规划好代码复用，简化、清晰明了地实现整体的架构
设计，请逐步规划、写入、更新到设计文档
```

后续确认（2026-10-22）：

- 配置（系统提示词、工具白名单、**插件挂载策略**）是 per-agent 的，会话级表达不了 →
  **插件需要自己的根 agent**，与内置根并存
- 不创建独立的 `AgentHost`，也**不新建第二份插件宿主**：挂到现有容器（共享 ioCtx /
  threadPool / hostBus / 子代理预算 / 已加载的插件实例）
- 群对话按会话隔离：同一个根下，每群一个 `sessionId`；不每群一个根
- 执行图暂不碰

## 结论摘要

五件事，各自落在不同层：

| 编号 | 要做的事 | 落在哪一层 | 关键决定 |
|---|---|---|---|
| 一 | CLI / FFI / 插件用同一套"启动 + 驱动 + 控制" | 新增 lib 公共类 `AgentSessionRuntime` | 启动壳只有一份；端点仍按宿主各自实现 |
| 二 | 插件能用自己的配置（提示词/挂载/工具） | `AgentHost` 从单根改为多根 + `mountRoot` | 配置类差异用根；不动内置根 |
| 三 | 群对话互不影响 | 会话通道（`sessionId`） | 对话类差异用会话；一个根服务多个群 |
| 四 | 群消息触发会话（可扩展到定时/外部信号） | 运行时层（通道之上） | 不改 `BaseAgent` ReAct 循环 |
| 五 | 同一根下不同群微调措辞（可选） | 会话级提示词来源 | 主路径是根配置；这条是补充 |

关键判断：

1. **核心已经统一，外壳没有统一。** `SessionServerAgentIO::run()` 是唯一的"输入 → `runTurnAsync` → 事件"循环；`AgentIOBase` + `AgentIOTransportBase` 是唯一的端点/传输契约；中断走同一条 `WireInterruptRequest/Response`。重复的是**启动壳**：`mode_runners.cpp`、`ffi_runtime.cpp`、以及插件将要做的事情，三处各写一遍装配顺序。提炼成 `AgentSessionRuntime` 后，各方只剩端点适配。

2. **多根是必须的，但它是"配置隔离"的手段，不是"群隔离"的手段。**
   `AgentPrompt::systemPrompt`、`appendSystemPrompts`、`toolPrompt`、`toolWhitelist`、`enableToolFiltering`、
   `pluginMountPolicy`、中间件栈、执行图全都挂在 `AgentConfig` / `AgentContext` 上，会话级只能覆盖
   上下文、模型、工作目录、权限边界这类"对话状态"。
   所以：QQ 机器人 = 一个自己的根（独立人设与工具集）；它的所有群 = 这个根下的多个会话。

3. **插件不创建 `AgentHost`，也不新建插件宿主。** 进程内插件宿主只有一份（由 `AgentHost` 持有，
   见上一篇）。插件把自己的根挂进现有容器即可；多根改造在容器内部完成，插件实例全程只有一份。

4. **根的工具来自"插件挂载"，不是"再加载一遍插件"。** 上一篇之后，插件
   `create/start` 各只一次；根要有文件/命令工具，就在挂根的 spec 里声明挂载策略，
   宿主按策略把已加载的插件 `mount` 到这个根上。因此不存在"根里跑第二份插件实例"。

5. **事件驱动做在会话之上，不动 `BaseAgent`。** 触发一轮本来就是投递一条输入；把触发策略塞进执行核心会污染内置会话的取消/中断/子代理路径。

6. **提示词的主路径是根配置。** `config_json.prompt.systemPrompt` 直接换掉 coding 人设，
   `config_json.pluginMount` / `toolWhitelist` 换工具集，完全不影响内置根。会话级提示词来源
   （`PromptSourceRegistry` + `Base` 落点）只用于"同一根下多群不同群规"，列为可选阶段。

## 术语

| 词 | 含义 | 不混淆 |
|---|---|---|
| **根（Root）** | 一个挂载到 `AgentHost` 的根 agent 节点，持有独立 `AgentConfig` | 不是图节点（`AgentNode` 是容器里的登记项） |
| **挂载根** | 插件经 `runtime.mount_root` 创建、带自己配置的根 | 与内置根 `"root"` 并存，互不影响 |
| **插件挂载** | 上一篇的 `PluginMount`：把一个已加载插件挂到某个 agent（含挂载根） | 与"挂载根"是两件事 |
| **agent 引用** | 上一篇的 `PluginxxScopeRef`（agentId + 挂载序号） | 只在 agent 级接口上用；runtime 表用 handle（`root_id` / `channel_id`） |
| **会话通道（Channel）** | 一个 `sessionId` 的驱动通道：输入、事件、中断、状态 | 不是网络连接；进程内 Channel 传输只是它的实现细节 |
| **触发器（Trigger）** | 把事件/定时转成通道输入的策略层 | 不是中间件钩子 |
| **运行时（Runtime）** | 两个含义，看上下文：① 插件接口表 `agentxx.agent.runtime` 的实现；② lib 侧启动壳 `AgentSessionRuntime` | 文档里写全名区分 |
| **`config_json`** | `mount_root` 传入的 `AgentConfig` 覆盖（见 [02](02-runtime-design.md) §6、[04](04-prompt-and-impl.md) §2） | 不是 yaml 配置文件 |

> `root_id` 与 `agentId` 是同一个值：`mount_root` 返回的 id 就是该根在 `AgentRegistry` 里的
> agentId，可以直接传给上一篇的 `mount.get` 查它挂了哪些插件。

## 文档索引

| 文档 | 内容 |
|---|---|
| [01-analysis.md](01-analysis.md) | 三方启动路径对比（"是否已统一"）、插件框架能力边界、FFI 清单、AgentHost 单根原因与多根取舍、八个约束、方案对比 |
| [02-runtime-design.md](02-runtime-design.md) | `AgentSessionRuntime`、`AgentHost` 多根语义、新表 `agentxx.agent.runtime` 的完整 C ABI、事件、线程、时序、SDK、预算 |
| [03-event-driven.md](03-event-driven.md) | 事件驱动：触发器、策略、定时源、QQ 群完整流程、扩展方向 |
| [04-prompt-and-impl.md](04-prompt-and-impl.md) | 提示词与配置归属（根配置为主、会话来源可选）+ 六阶段实现规划与测试 |

## 范围与非目标

**覆盖**：

- `AgentSessionRuntime`：CLI 本地直连、FFI、插件共用的启动壳
- `AgentHost` 多根：内置根 + 插件挂载根，每根独立配置、独立子代理记账；挂载根复用同一份插件宿主
- 挂载根的插件挂载策略（`none` / `all` / `listed`）与递归防护
- 插件在挂载根上开会话通道：输入、取消、切模型、应答中断、查询状态与上下文
- 会话事件回调（增量、工具、轮次、统计、回执、中断）
- 事件触发器（排队/合并/丢弃）与定时触发
- 插件禁用/卸载时根与通道自动回收

**不做**：

- 不做"每群一个根"（成本线性增长，且无必要）
- 不做插件自建 `AgentHost`、也不做第二份插件宿主
- 不在挂载根上换"插件加载名单"（加载是进程级；根只能选挂载哪些）
- 不做插件自定义执行图（本期；后续可在 `MountRootSpec` 追加 `graph_json`）
- 不做跨进程会话（远程已有 WS）
- 不改 `pluginxx.*` 通用表（新表是 `agentxx.agent.*`）
- 不做 QQ/微信协议本身

## 待确认问题

| 问题 | 建议默认值 | 影响 |
|---|---|---|
| 挂载根默认挂哪些插件 | **默认 `none`**（只有内置工具），创建者显式给 `listed`/`all`；创建者自己不挂到自己的根（`include_creator=false`） | 少挂 = 根没工具；多挂 = 把 coding 工具塞进聊天机器人 |
| QQ/微信群数量级 | 插件实测后反馈 | 决定"一个根 64 通道"是否够 |
| 是否同一插件挂多个根 | 默认 1 个（客服/运维分人设时才多） | 进程内根上限 `AgentHost::maxRoots`（建议 8，含内置根） |
| 群会话是否都持久化 | 默认开启（`sessionId` 由插件命名） | 关掉则重启丢上下文 |
| 会话级提示词来源本期是否做 | 不做（人设在根配置里） | 只影响"同根多群不同群规" |

## 风险摘要

| 风险 | 说明 | 处置 |
|---|---|---|
| `AgentSessionRuntime` 波及 CLI/FFI | 启动顺序与停止顺序可能回归 | 先 FFI，后 CLI；保留原路径直到验证 |
| `AgentHost` 多根改动子代理 | 内置功能回归 | 单独一阶段；`attachRoot` 语义不变；跑全部子代理测试 |
| 每根一次完整 init | 多根时启动慢、内存高 | 默认一个根；`maxRoots` 上限；注意 init 里**包含插件挂载**（一次 mount × 挂载数），不含 `dlopen`/`create`/`start` |
| 在 `mount` 回调里建根 | 无限嵌套 | 三层：① 建根只能在 `start`；② 宿主在驱动 `mount` 期间拒绝 `mount_root`；③ 创建者默认不挂到自己的根 |
| 根忘记声明挂载 | 根只有内置工具，模型能看到"没有文件工具" | 文档示例给全；`list_runtime` 输出每根的挂载摘要，便于自查 |
| 回调里阻塞 | 所有会话都在同一 io 线程 | 文档禁止；调试构建对超时回调告警 |
| 通道泄漏 | 关闭顺序错会留挂起协程 | 关闭集中一个函数；ASan 下反复开关 |
| 多发送者抢同一会话 | 两个通道驱动同一 `sessionId` 会互相覆盖 | 占用键是 `(存储根, sessionId)`，由 `AgentHost` 全局登记（`02` §8.2） |
