# 插件独立线程（插件执行器）设计

> 目标读者: 修改插件框架 / `PluginManager` / 插件生命周期与回调派发的开发者
> 相关文档: [plugins.md](../../../docs/zh-cn/design/plugins.md) (插件系统总览) · [plugin-api-async plan](../plugin-api-async/plan.md) (接口表异步化, 本任务的前置) · [js-runtime plan](../js-runtime-singleton/plan.md) (JS 运行时单例重构)
>
> 本文件是任务规划 (未实施), 实施完成后结论搬进 [plugins.md](../../../docs/zh-cn/design/plugins.md) 与 `AGENTS.md`。

## 1. 背景: 现在谁在执行插件代码

主程序每秒都在把执行权交给插件代码。今天的线程归属是这样的:

| 线程 | 数量 | 今天会执行插件代码吗 | 典型入口 |
|---|---|---|---|
| agent io 线程 | 每 `AgentHost` 1 条 | **会** | 工具 `execute_start`、图节点 `run_start`、能力行 `run_start`、`start` / `mount` / `unmount`、通道 `on_event`、后台任务 `on_event`、广播订阅 `handler`、`sleep` / `offload` 的完成回调 |
| 后台任务线程池 | 注入的共享池 | 会 (插件显式提交) | `blocking_tool` / `polled_tool` 业务体、`scheduler.offload` 的 work |
| JS 线程 | 每 host 1 条 | 会 (脚本) | 全部脚本回调 |
| client io 线程 | 每进程 1 条 | **会** | UI 动作 `on_action`、定时器 `on_timer`、快捷键 `on_keybind`、工具渲染 `collapsed_fn` / `expanded_fn`、命令 `execute`、广播 `handler` |
| client UI 线程 | 每进程 1 条 | 不会 | 渲染与输入 (经 `postToUi` 投递) |

结论: **io 线程今天就是插件的执行线程之一**。接口表里那些"「io 线程约束」, 非 io 线程由主程序投递同步等待"的注释, 以及 `is_io_thread` / `post_to_io` / `ioCallSync` 这一套, 都是这个事实的产物。

## 2. 为什么这是个问题

1. **只靠约定**: 所有回调的注释都写着"必须快速返回, 不得阻塞", 但没有强制。一个插件在 `execute_start` 里同步读一次慢盘、在 `on_action` 里等一次网络, 卡住的是整条 io 线程 ——
   模型流、其它会话、工具调度、会话端点全部停摆。
2. **违约成本与插件作者无关**: 出问题的是主程序, 现场还很难看出是哪个插件。
3. **作者被迫自己想办法**: 想安全就得套 `offload` / 自己起线程 / 自己加锁, 而回调却可能来自
   io 线程、容器 io 线程、client io 线程 (双端插件 = 至少两条线程) —— 心智负担很重。
4. **与异步化只解决一半**: 接口表异步化后, 插件调主程序不再等待; 但主程序调插件仍然是
   "在 io 线程上直接调用插件函数", 这一半只有换执行线程能解决。

## 3. 结论: 值得做, 但不要和异步化合在一起

**值得做**, 收益是结构性的 (见 §5)。**但要作为异步化之后的独立任务**:

- 异步化本身已经要改 125 个成员里的 104 个; 两项合在一起, 出问题时无法判断是"契约改坏了"
  还是"线程模型改坏了", 回滚也只能整体回滚。
- 异步化是它的**前置**: 表没异步化之前把插件搬走, 每次表调用都会变成"插件线程阻塞等 io",
  串行队列里一个慢调用会把所有插件卡住, 比现状更差。
- 唯一可以提前做的是**生命周期那一批** (`start` / `stop` / `mount` / `unmount`), 它们数量少、
  语义清楚, 可以作为阶段 A 验证线程模型 (见 §8)。

## 4. 现状调查: 现成的基础设施比想象的多

框架已经具备三块可复用的地基, 这是本任务可行的关键:

| 现有设施 | 位置 | 对本任务的意义 |
|---|---|---|
| `PluginRuntime` 持有**一个** executor + 操作表 + 待重放动作队列 | `pluginxx/runtime/runtime.h` | 只需把它拆成"io 执行器 + 插件执行器"两个, 其余结构不动 |
| `enqueueRuntimeAction(runtime, fn, retainWhenStopped)` + `replayRuntimeActions` | 同上 | 现成的"投递一个动作到 executor, 停用期保留、恢复后重放"队列, 正是派发需要的语义 |
| `InstanceLifetime` / `InstanceLease` (`leases_`) | 同上 | 已经能表达"还有执行进入插件代码, 关闭要等它退出" —— 派发项持 lease 即可保证 dlclose 安全 |
| `OpCore::create(runtime, provider, caller, ...)` + `setCompletionHandler` | `pluginxx/runtime/op_driver.h` | 一次异步操作的完成投递已经统一走 `runtime->executor`, 改指插件执行器即完成迁移 |
| `PluginHostCore::postCallback` / `sleep` / `offload` | `pluginxx/host/host_core.h` | 已经用 OpCore 承担"在 IO 线程上回调插件", 目标执行器换了就能复用 |

反过来, 有三处**必须改**, 不能只换 executor:

1. `enterHost` 与 `PluginHostCore` 里的 `isIoThread()` 判定 (`host_core.h` 的 `postCallback` /
   `sleep` / `offload` / `register_task` 等) —— 它们要求"调用发生在 io 线程", 换成插件线程后
   全部失效;
2. 表成员入口里的 `ioCallSyncKeep`(把 body 拉到 io 执行) —— 异步化之后应改为"投递 + 完成",
   过渡期允许"插件线程同步等 io" (可接受, 因为卡的是插件线程);
3. `stop` / 卸载路径 (`PluginHostLifecycle::requestClose` → `rollbackLoad`) 是同步等
   `destroyPlugin()`, 换线程后必须是异步 drain + 超时。

## 5. 目标模型

```
                     ┌──────────────────────────────┐
  插件代码 ──────────▶│ 插件线程 (每管理器 1 条)      │──── 表调用 (异步) ──▶ io 线程
                     │ 所有实例的回调串行执行        │◀─── 结果回调 (投递) ──
                     └──────────────────────────────┘
  主程序状态 (会话 / 注册表 / 图 / 定时器) 仍在 io 线程, 不跨线程
```

- **不变量**: 插件代码只在"该实例的执行器"上运行; io 线程只做受理、投递、状态机推进;
  状态永远不跨线程 (插件执行器上不碰任何 io 拥有的对象)。
- **三种执行器来源**: 默认 → 管理器的插件线程; 实例声明自带执行器 → 直接投递 (JS 引擎用
  它的 JS 线程); 将来若要"每插件一线程", 只是第三种配置, 模型不变。

### 5.1 线程清单 (目标态)

agent 端 (每 `AgentHost`):

| 线程 | 数量 | 职责 | 不再承担 |
|---|---|---|---|
| io | 1 | 会话、模型调用、工具调度、会话端点、全部状态机 | 执行插件代码 |
| 后台任务池 | 注入的共享池 | `offload`、`blocking_tool`、文件扫描、sqlite 等委托工作 | — |
| 插件线程 | 1 | 所有 C++ 插件回调 + 表调用入口 | — |
| JS 线程 | 1 (JS 引擎插件私有) | 所有脚本 | — |

client 端 (每进程):

| 线程 | 数量 | 职责 |
|---|---|---|
| io | 1 | WS 收发、消息分发、插件表实现、界面状态机 |
| UI | 1 | FTXUI 渲染与输入 (现有 `uiThread_`) |
| 插件线程 | 1 | 所有 client 插件回调 (动作/定时器/快捷键/渲染器/命令/广播) |
| 后台任务池 | 1 个池 | `dlopen`、目录扫描、磁盘读写 |
| JS 线程 | 1 | client 侧脚本 |

### 5.2 派发与顺序

- 派发入口统一为 `dispatchToPlugin(instance, fn)`: 经 `enqueueRuntimeAction` 投到该实例的
  执行器, 执行体**先取 `InstanceLease`** 再进插件代码 (拿不到 lease = 实例已在关闭, 直接丢弃
  并记日志)。
- 同一实例严格 FIFO (单执行器天然保证); 跨实例共享线程时只保证"各自 FIFO"。
- `stop` 也走同一条队列, 因此它一定排在已受理的全部回调之后; `stop` 之后不再受理派发。
- 回调里再调表 = 投递到 io (异步), 不等待 → 不会形成 io ↔ 插件线程互等。

### 5.3 生命周期与卸载

```
io: requestClose → 投递 stop 到插件执行器 (FIFO 排在已派发项之后)
插件线程: 之前派发项依次执行 → stop 执行 → notify->done
io: 收到 done → destroy → 最后一个 lease 释放 → dlclose
超时 (默认沿用现有 close 超时): 记 CloseFailed, 不 dlclose, 记日志并禁用
```

- 队列里还没开始执行的派发项: 在进入 `Closing` 后按失败/取消收尾, 不再进插件代码;
- `destroy` 仍然同步执行 (见 plugin-api-async plan §6.5), 但它排在 stop 之后、且此时已经没有
  在途回调 (lease 归零), 因此不再需要"边跑边卸"的兜底。

### 5.4 自带执行器 (JS 引擎)

JS 引擎已经有自己的 JS 线程: 它今天收到 `execute_start` 时在 io 上做一次"投递到 JS 线程"。
如果框架再把它派发到共享插件线程, 就会变成 `io → 插件线程 → JS 线程` 三跳。

因此给实例一个可选的**执行器声明** (`PluginxxInstanceSpec` 增加 `executor` 回调, 或
`start` 时登记): 声明过的实例, 框架直接把派发投到它的执行器上; 未声明的走共享插件线程。
JS 引擎声明后仍是两跳, 与现状一致。同一套机制也留给"自带 io_context 的 C++ 插件"。

### 5.5 可观测性

- 每个派发项记录: 排队时长、执行时长、所属实例与回调种类;
- 超过预算 (`plugin.callback_budget_ms`, 默认 100 ms) 记 WARN + `KeyMetrics` 计数
  (`plugin.callback_over_budget`), `dev_mode` 下加载快照输出每实例的分布;
- 日志统一带线程标识 (io / plugin / js / ui), 便于判断"卡在谁身上"。

## 6. 需要改的地方

| 位置 | 改动 |
|---|---|
| `pluginxx/runtime/runtime.h` | `PluginRuntime` 增加 `pluginExecutor`; `enqueueRuntimeAction` 支持指定执行器 |
| `pluginxx/runtime/op_driver.h` | `OpCore` 完成投递改到 `pluginExecutor`; 状态机仍在 io (注意加锁/断言) |
| `pluginxx/host/host_core.h` | 去掉 `isIoThread()` 前置判定, 改为"任意线程受理 → 派发"; `postCallback` / `sleep` / `offload` / `register_task` 的完成都经插件执行器 |
| `pluginxx/host/lifecycle.h` | `start` / `stop` / `mount` / `unmount` 派发化; 关闭改为异步 drain + 超时; `rollbackLoad` 不再同步 `destroyPlugin` |
| `pluginxx/host/tables_impl.h` | 表入口由"io 上执行 body"改为"受理 + 投递"; `postToIoEntry` 删除 (已在 plugin-api-async §5.6) |
| 主程序侧线程 | `AgentHost` / `ClientPluginManager` 各起一条插件线程 (io_context + 单线程 run), 共享同一个 `pluginExecutor` 给其下所有实例 |
| 派发点 | 工具 `execute_start` (`plugin_manager_adapters.cpp`)、能力行 (`plugin_manager_capability.cpp`)、图节点 (`plugin_graph_node.cpp`)、通道 `on_event`、后台任务 `on_event`、client 动作/定时器/快捷键/渲染器/命令 |
| 测试 | 新增 `plugin_executor` 模块: 顺序、租约、卸载超时、自带执行器; 扩 `plugin_cleanup` / `plugins` / `client_plugins` |
| 文档 | `plugins.md` 增"线程模型"一节; `AGENTS.md` 的线程清单; `plugin-api-async` plan 里 `is_io_thread` 的去留改为"本任务完成后删除" |

## 7. 与其它任务的关系

- **前置**: [plugin-api-async](../plugin-api-async/index.md) (表异步化)。它完成后
  `is_io_thread` / `ioCallSync` 只剩"阻塞形态的安全闸", 本任务把它们彻底删掉。
- **并行**: [js-runtime-singleton](../js-runtime-singleton/index.md) 的 mount 桥改造与本任务
  互不冲突; JS 引擎通过 §5.4 的自带执行器继续用自己的线程, 不受共享插件线程影响。
- **不受影响**: 后台任务线程池 (`offload` / `blocking_tool`)、模型调用、会话运行时。

## 8. 分阶段实施

| 阶段 | 内容 | 验收 |
|---|---|---|
| A 地基 | `PluginRuntime` 双执行器 + `dispatchToPlugin` (含 lease) + 预算打点; 只接管**生命周期** (`start` / `stop` / `mount` / `unmount`) | `plugins` / `plugin_cleanup` / `plugin_host_mount` 全绿; 日志能看到 `start` 跑在 plugin 线程 |
| B 工具与执行体 | 工具 `execute_start` / `cancel`、图节点 `run_start` / `cancel`、能力行 `run_start` / `cancel` | `plugins` / `plugin_feature` / `event_tools` / `async_tools` 全绿 |
| C 通知类 | 通道 `on_event`、后台任务 `on_event`、广播订阅 `handler` / `on_end`、工具渲染与命令回调 | `plugin_agent_runtime` / `plugin_broadcast` / `plugin_point` 全绿 |
| D client 侧 | client 独立插件线程; 动作/定时器/快捷键/渲染器/命令全部派发化 (依赖 async plan 阶段 D 的渲染与命令契约) | `client_plugins` / `plugin_ui` / `tui_*` 全绿 |
| E 收尾 | 删除 `is_io_thread`、`ioCallSync` 残余、`ioCallSyncKeep` 的插件侧用法; 文档与 `AGENTS.md`; 基准对比 | 全量回归 + 基准 (io 线程被插件占用的时长归零) |

每阶段独立可回归; 阶段 A 决定派发与 drain 的模板, 先定稿再铺开。

## 9. 验证

- **隔离**: 造一个故意 `sleep(5s)` 的插件回调, 期间 agent 仍能处理其它会话的模型流与工具;
  io 线程占用时长不因它变化 (基准对比)。
- **顺序**: 同一实例连续派发 1000 次, 执行顺序与受理顺序一致; `stop` 之后没有任何回调再执行。
- **卸载**: 回调里阻塞 → `stop` 超时 → `CloseFailed` 且**没有 `dlclose`** (ASan 下无 use-after-free);
  随后禁用/启用能恢复。
- **自带执行器**: JS 引擎声明后, 脚本回调回调路径仍是两跳 (io → JS 线程), 未出现第三跳。
- **死锁**: 插件在回调里 `co_await` 一次表调用 (异步) 不应与 io 形成互等; 用超时用例覆盖。
- **开销**: 每回调派发开销 (排队 + 唤醒) 在微秒级; 与现状对比每轮耗时, 允许小幅回退,
  io 线程占用必须明显下降。

## 10. 风险与对策

| 风险 | 对策 |
|---|---|
| 一条插件线程一条队列: 一个插件阻塞拖住其它插件 | 预算 + 指标 + 日志点名; 预留 `plugin.per_plugin_thread` 开关 (模型不变, 只是执行器多几个) |
| 每次交互多一跳 | 只对"主程序 → 插件"加一跳, 且是投递不是等待; 高频 (200ms 定时器) 也无感 |
| `OpCore` 的完成投递改线程后状态机出问题 | 完成仍在 io 完成状态机, 只把"回调插件"那一步投到插件执行器; 加断言校验线程 |
| 卸载路径复杂 (drain / 超时 / 迟到派发) | 复用 `InstanceLease` + 现有 close 超时重试; 覆盖阻塞插件的用例 |
| 调试难度上升 | 日志带线程标识; 加载快照输出派发统计 |
| 与异步化同时改导致定位困难 | 严格串行: 异步化全部完成并稳定后再开工; 只提前做阶段 A |

## 11. 不做的事

- 不做"每插件一条线程" (先共享一条; 需要时按开关加, 模型不变)。
- 不把 io 状态 (会话/注册表/图/定时器) 搬到插件线程 —— 状态不跨线程是本设计的前提。
- 不做进程级共享执行器 (agent 与 client 各自一份, 生命周期各管一套)。
- 不取消后台任务线程池: `offload` / `blocking_tool` / 磁盘与目录扫描仍在那里跑。
- 不为插件提供"在 io 线程上执行任意代码"的能力 (那就是 `post_to_io`, 已在
  plugin-api-async §5.6 删除)。
