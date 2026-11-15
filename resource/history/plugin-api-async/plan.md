# 插件接口表异步化设计

> 目标读者: 修改插件接口表 / vtable / kit 的开发者
> 相关文档: [plugins.md](../../../docs/zh-cn/design/plugins.md) (插件系统总览与 §8 接口表分类) · [js-runtime plan](../js-runtime-singleton/plan.md) (JS 运行时单例重构) · [index.md](../../../docs/zh-cn/design/index.md) (主程序架构)
>
> 本文件是任务规划 (未实施), 实施完成后结论搬进 [plugins.md](../../../docs/zh-cn/design/plugins.md) §8 与 `AGENTS.md`。

## 1. 背景与目标

插件接口表 (C ABI) 目前混用三种调用模型，其中"同步返回"占绝大多数。同步模型的问题在两类调用方上表现不同:

- **C++ 插件**: 主程序 io 线程直接进入插件发起的表调用并执行表实现。实现本身若碰到磁盘 (sqlite)、大对象序列化、目录扫描、起会话/起 agent，就把 io 线程占住；插件作者要避开只能自己套 `offload` / `polled_tool` 一类样板。
- **JS 插件**: 脚本跑在引擎自己的线程上，任何同步表调用都必须"JS 线程阻塞 → io 线程执行 → 返回"(`ioCallSyncKeep`)。这带来三个具体限制:
  1. 脚本侧一次同步调用会让两条线程互相等，回调里再调表就形成互等 (client 侧因此加了 150 ms 上限，见 plugins.md §12)；
  2. 引擎必须维护"io→JS 同步桥"，是实现里最容易出错的一处；
  3. 表实现慢的时候，被卡住的是脚本线程 + io 线程两条。

**目标**: 把"插件调用主程序"的接口表统一成**异步操作契约** (受理即可返回，结果经回调交回)，让主程序 io 线程只做受理与投递，重活在任何情况下都不占 io 线程；同时让 JS 桥彻底摆脱同步往返。

**不是目标**: 不引入"并行执行同一实例的表调用"；不改"插件提供实现、主程序调用"那一侧 (工具/能力/图节点/生命周期已经是 notify 契约)。

## 2. 现状: 三种模型

| 模型 | 形态 | 典型成员 |
|---|---|---|
| 同步返回 | `int32_t f(host, scope, args..., PluginxxString* out)`，结果在调用栈上拿到 | 业务表绝大多数成员 |
| 回调式异步 (插件调主程序) | 返回 `PluginxxOperatorHandle*` + `PluginxxOperatorCallback cb, void* ud, PluginxxString* error_out` | `call_tool_async`、`invoke_capability(_ex)`、`scheduler.sleep`、`scheduler.offload`、`net.call_async`、`net.fetch` |
| 提供方通知契约 (主程序调插件) | 返回 op 句柄 + `const PluginxxOperatorNotify* notify`，插件稍后 `notify->done(status, payload)` | 工具 `execute_start`、`tasks.register_task`、`start/stop/mount/unmount`、能力行 `run_start`、图节点 `run_start` |

异步化的目标是让第一类向第二类靠拢，第三类保持不变。

## 3. 问题场景 (为什么必须改)

1. **JS 桥互等**: 主程序 io 线程等 JS 线程的结果时 (client 渲染/命令回调)，脚本里再调一次表 (`uiRegistry`、`config`) 就是双向等待，只能靠 150 ms 超时兜底。表异步化后脚本拿到 Promise，io 线程从不等 JS，问题从根上消失。
2. **io 线程被重活占住**: `get_share_store` 走 sqlite、`get_messages` 序列化整段上下文、`register_skill_dir` 扫目录、`mount_root` 起 agent、`open_channel` 起会话 —— 都是同步调用栈上的活。
3. **插件作者样板**: 现在想在 `start`/`mount` 里做一次"读配置 + 注册工具 + 读会话"的串行逻辑，要么同步 (占 io 线程)，要么套 `offload` + 手动续接。

## 4. 设计原则

1. **能异步的一律异步**，只有"必须同步"的语义才保留同步 (成员清单见 §5, 回调清单见 §6)。
2. **异步 ≠ 并行**: 同一实例的调用按受理顺序执行 (io 线程 FIFO)，顺序语义不因异步化改变。
3. **错误分层**: 参数/作用域/状态类错误在**入口**同步返回 (`NULL` + `error_out`)；执行期错误经回调 `status = FAILED`；取消走 `CANCELED`。这样"写错参数"仍然是立刻可见的编译期/运行期错误，不需要等一次往返。
4. **payload 语义不变**: 回调载荷就是原来 `out` 的内容 (JSON 或纯文本)，迁移时下游解析代码不用改。
5. **取消统一**: 所有异步成员返回的 op 句柄都交给 `pluginxx.cancel.op_cancel`，不再每张表各留一个取消入口。
6. **C++ 侧不牺牲可读性**: kit 同时提供 `co_await` 形态与"阻塞便捷形态 (仅限非 io 线程)"，作者不写回调也能用。

## 5. 逐表盘点与结论

图例: ✅ 改为异步 · ⏸ 保持同步 · 🔁 已是异步 (只统一取消) · ❌ 保持同步且明确不可异步化

### 5.1 `agentxx.agent.tools` / `permission` / `session` / `context` / `model` / `prompt` / `resources`

| 成员 | 结论 | 依据 |
|---|---|---|
| `register_tool` | ✅ | schema 解析 + 工具对象构建 + 工具提示词合成；注册量随插件工具数增长 |
| `unregister_tool` | ⏸ | 记账 (erase)，O(1)，且"注销后立刻不可见"需要顺序保证 |
| `call_tool_async` | 🔁 | 已是回调式异步 |
| `register_tool_permission` / `unregister_tool_permission` | ⏸ | 记账 |
| `check_paths` | ✅ | 逐路径 × 规则判定，批量查询可到数百条；被工具实现高频调用 |
| `get_share_store` | ✅ | sqlite 读 (磁盘) |
| `add_share_store` | ✅ | sqlite 写 (磁盘) |
| `emit_message_tip` | ⏸ | 只做投递 (入队给会话端点) |
| `get_session_work_dir` | ⏸ | 读内存里的会话配置 |
| `is_canceled` | ❌ | advisory 查询，语义要求同步且无锁 |
| `get_messages` | ✅ | 序列化整段上下文 (可达 MB 级) |
| `messages_count` | ⏸ | O(1) 计数，专门用于"只想要数量" |
| `get_config` | ⏸ | 读内存里的模型配置快照 (序列化很小) |
| `get_prompt` / `set_prompt` | ✅ | 合成 (遍历贡献并重排) / 回写并触发刷新 |
| `get_tool_prompt` | ⏸ | 读内存配置项 |
| `register_skill_dir` | ✅ | 需要扫描目录、建立索引 |
| `unregister_skill_dir` | ⏸ | 记账 |
| `register_memory_file` | ✅ | 需要读文件、校验内容 |
| `unregister_memory_file` | ⏸ | 记账 |
| `register_mcp_server` | ✅ | 需要建连接/握手，最慢的一个 |
| `unregister_mcp_server` | ✅ | 需要断开连接、等收尾 |
| `get_own_resources` | ✅ | 序列化资源清单 |

### 5.2 `agentxx.agent.graph` / `mount` / `runtime` / `async work`

| 成员 | 结论 | 依据 |
|---|---|---|
| `register_node_type` / `unregister_node_type` | ⏸ | 记账 + 顺序要求 (类型必须在建图前登记) |
| `get_graph_json` | ✅ | 序列化整张图定义 |
| `get_graph_name` | ⏸ | 读一个字符串 |
| `set_graph_json` | ✅ | 解析 + 校验 + 保存定义 |
| `list` / `get` (mount) | ✅ | 序列化挂载清单 (含所有插件工具名)，随插件数增长 |
| `mount_root` | ✅ | 起一个新的 agent 根 (重) |
| `unmount_root` | ✅ | 等根收尾、释放资源 |
| `open_channel` | ✅ | 起会话 (含模型/工具初始化) |
| `close_channel` | ✅ | 等会话收尾 |
| `send_input` / `cancel` / `respond_interrupt` / `select_model` | ⏸ | 只做投递/切换句柄；语义上"提交成功即完成" |
| `get_channel_state` / `get_context_messages` / `list_runtime` | ✅ | 序列化会话状态 / 上下文 / 清单 |
| `register_trigger` / `unregister_trigger` / `register_timer` / `unregister_timer` / `emit_event` | ⏸ | 记账与投递；"注册后立刻生效"需要顺序保证 |
| `submit_async_work` | ✅ | 创建任务会话、可能落盘 |
| `cancel_async_work` | ⏸ | 置取消标记 |
| `get_async_work` / `list_async_work` | ✅ | 序列化任务表 |

### 5.3 `agentxx.client.*`

| 成员 | 结论 | 依据 |
|---|---|---|
| `ui.register_status_item` / `update_status_item` / `register_panel` / `update_panel` / `register_info_section` / `update_info_section` | ✅ | 解析 UI JSON (单条上限 1 MiB) + 建渲染项 |
| `ui.unregister_*` | ⏸ | 记账 |
| `ui.register_command` / `bind_action_handler` / `register_tool_renderer` | ✅ | 解析规格 + 建对象 (与渲染首帧相关) |
| `ui.unregister_command` / `unbind_action_handler` / `unregister_tool_renderer` | ⏸ | 记账 |
| `ui.show_toast` / `open_overlay` / `close_overlay` / `update_tool_decor` | ⏸ | 投递 |
| `session.get_client_state` | ✅ | 序列化界面状态快照 |
| `session.send_user_input` / `request_cancel` | ⏸ | 投递 |
| `wire.send_plugin_data` | ⏸ | 投递 |
| `self.get_own_info` | ✅ | 序列化插件清单信息 |
| `self.get_plugin_args` / `get_plugin_config_path` / `get_language` / `set_language` | ⏸ | 读内存配置 |
| `timer.set_timer` / `cancel_timer` / `is_visible` | ⏸ | 记账 / 查询可见性 |
| `keybind.register_keybind` / `unregister_keybind` | ⏸ | 键位规范化 + 冲突检测 (小) |
| `keybind.list_keybinds` | ✅ | 序列化清单 |

### 5.4 通用表 (框架侧)

| 成员 | 结论 | 依据 |
|---|---|---|
| `broadcasts.declare_broadcast` / `publish_broadcast` / `subscribe_broadcast(_ex)` / `unsubscribe_broadcast` / `read_broadcast` / `end_broadcast` | ❌ | 顺序语义 (订阅先于发布可见) + O(1) 缓存读；异步化会破坏"注册后立刻收到" |
| `capabilities.register_capability` / `unregister_capability(_ex)` | ⏸ | 记账；登记后需立刻可调用 |
| `capabilities.invoke_capability(_ex)` | 🔁 | 已是回调式异步 |
| `capabilities.capability_info` / `list_capabilities` | ✅ | 序列化清单 |
| `scheduler.is_io_thread` | ❌ | 语义必须同步 |
| `scheduler.post_to_io` | ❌ | 本身就是 fire-and-forget |
| `scheduler.sleep` / `offload` | 🔁 | 已是回调式异步 |
| `cancel.op_cancel` | ❌ | 必须同步非阻塞 |
| `tasks.register_task` / `cancel_task` | ⏸ | 记账；`register_task` 本身是提供方通知契约 |
| `coroutine_runtime.request_driver` / `cancel_driver` / `is_io_thread` | ❌ | driver/wake 协议本身就是异步握手 |
| `plugins.list_plugins` / `get_plugin` / `get_own_info` | ✅ | 序列化清单 (随插件数增长) |
| `config.*` | ⏸ | 读进程配置 (内存) |
| `json.*` / `log.log` | ❌ | 纯函数 / 写日志 |
| `net.*` | ✅/⏸ | `call` 统一成异步并合并 `call_async` (名字见 §7.4)；`cancel` 是记账保持同步 |

**统计** (agent 侧 53 + client 36 + 通用 37 成员): 改异步 ≈ 45 项，保持同步 ≈ 55 项，已是异步 ≈ 12 项，明确不可异步 ≈ 14 项。

回调函数不在上面四张表里，单独盘点见 §6: **需要改的只有 3 个回答型回调**。

## 6. 回调函数盘点与结论

接口表里的回调分两个方向。判断改不改的标准不是"实现得慢不慢"，而是**调用方是否必须在本帧/本次调用里拿到返回值**:

- **通知型** (`void`，只把事件交给插件): 主程序不等结果，回调慢只拖住它自己那条线程 → **保持同步**。正确写法是"拷走数据、立刻返回，重活投递回自己的线程"（相关结构体注释已经是这么要求的）。
- **回答型** (返回 `int32_t` + 出参，调用方要用这个值继续算): 调用方被迫等 → **改异步**；同时保留"回调内部立刻 `notify->done` 就当同步完成"的快路径，本来就在微秒级算完的插件行为不变。

### 6.1 主程序调用插件的回调 (插件注册 / 传入)

| 回调 | 调用方与线程 | 现状 | 结论 |
|---|---|---|---|
| `start` / `stop` (`PluginxxStartFn` / `PluginxxStopFn`) | 主程序 → 插件, io | notify/op | 🔁 已是异步 |
| `mount` / `unmount` (`AgentxxPluginAgentMountFn` / `UnmountFn`) | 主程序 → 插件, io | notify/op | 🔁 已是异步 |
| 工具 `execute_start` / `execute_cancel` | 主程序, io | notify/op + 取消 | 🔁 已是异步 |
| 图节点 `run_start` / `run_cancel` | 主程序, io | notify/op + 取消 | 🔁 已是异步 |
| 能力行 `run_start` / `cancel` | 主程序, io | notify/op + 取消 | 🔁 已是异步 |
| `tasks.register_task` 的 `cancel_fn` + `notify` | 主程序, io | notify/op | 🔁 已是异步 |
| **工具折叠内容 `collapsed_fn`** | client io | **同步返回文本** | ✅ **改异步** |
| **工具展开内容 `expanded_fn`** | client io (仅展开时) | **同步返回 items JSON** | ✅ **改异步** |
| **斜杠命令 `execute`** | client io | **同步返回动作 JSON** | ✅ **改异步** |
| 动作处理 `AgentxxUiActionFn` | client io | void 投递 | ⏸ 保持 |
| 定时器 `on_timer` / 快捷键 `on_keybind` | client io | void 投递 | ⏸ 保持 |
| 广播订阅 `handler` / `on_end` (`subscribe_broadcast(_ex)`) | 派发线程 | void 投递 | ⏸ 保持 |
| 会话通道 `on_event` (`AgentxxPluginChannelEventFn`) | 根所属 io | void 投递 | ⏸ 保持 |
| 后台任务 `on_event` (`AgentxxPluginAsyncWorkEventFn`) | 容器 io | void 投递 | ⏸ 保持 |
| 驱动 `drive_once` (`PluginxxDriveOnceFn`) | 主程序 io | void 单步推进 | ⏸ 保持 |
| 取消查询 `is_requested` (`PluginxxCancelToken`) | 任意 | 同步查询 | ❌ 必须同步 |
| 入口 `get_info` / `create` / `destroy` | 加载（agent 侧现在在 io，见 §6.5） | 静态元数据 / 构造 / 释放 | ⏸ 保持 (原因见 §6.5) |
| host vtable `alloc` / `free` / `query_interface` | 任意 | 内存与查表基础 | ❌ 必须同步 |

### 6.2 插件传给表成员的完成回调

| 回调 | 用在哪 | 现状 | 结论 |
|---|---|---|---|
| `PluginxxOperatorCallback cb` | `call_tool_async` / `invoke_capability(_ex)` / `scheduler.sleep` / `scheduler.offload` | 回调式异步 | 🔁 |
| `PluginxxOperatorNotify*` (`done(status, payload)`) | 所有操作契约的入参与出参 (`register_task` 是出参) | 恰好一次, 任意线程 | 🔁 |
| `scheduler.post_to_io(fn, ud)` | 投递一次 | fire-and-forget | ⏸ |
| `coroutine_runtime.request_driver(drive_once, ud)` | 协程推进 | 主程序异步投递 | ⏸ |
| `net.call_async` 的 `notify` | 网络请求 | 已完成 | 🔁 |
| `pluginxx.cancel.op_cancel` | 取消 | 同步非阻塞 | ❌ 必须同步 |

结论: 回调里只有 **3 个回答型** 要改（都是 client 侧），其它要么已经是操作契约，要么本来就是通知，改了只多一次往返。

### 6.3 三个回答型回调的新契约

三者统一成与工具 `execute_start` 同构的形态:

```c
/// 结果经 notify 交回: status = OK 时 payload 是内容；其它值等于"这次没有自定义内容"
/// - 回调内部立刻 notify->done(...) → 用本次内容 (快路径, 与改之前行为一致)
/// - 也可以返回后稍晚再 done → 主程序先用默认内容/上次缓存渲染, 结果到达时刷新该行
/// - 仍返回 `int32_t`: 0 = 已受理; 非 0 = 没受理 (写 error_out), 按默认渲染处理
typedef int32_t(PLUGINXX_CALL* AgentxxToolCollapsedRenderFn)(
    void*                         user_data,
    const AgentxxToolRenderInput* input,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
);
```

| 回调 | `done` 的 payload | 主程序拿到后 |
|---|---|---|
| 折叠内容 | 一行纯文本 | 替换该行折叠态的内容并沿用缓存 |
| 展开内容 | items JSON 数组 | 填充展开体 (只在展开时请求) |
| 命令 `execute` | 动作 JSON | 按动作执行 (与现在收到返回值后的处理相同) |

- **没有自定义内容**: `done(OK, 空)` 或 `done(FAILED, ...)` 都按默认渲染处理（与现在"出参留空"一致）;
- **失效与代际**: 每行的折叠内容与展开内容各自有请求代际 (就是现有缓存键 `#c` / `#e`)。参数签名、版本号变化或行被删除时，旧代际的 `done` 只被丢弃，不会写回界面;
- **不做请求超时/取消**: 插件不答就一直用默认内容，界面不受影响（脚本侧另有任务预算，超时按失败收尾）；实例卸载后仍有 `done` 到达也只被丢弃。

**为什么这样不损失体验**: 现有 C++ 渲染体都在微秒到毫秒级，`done` 在回调内部同步发出，主程序当帧就能取到内容 —— 缓存命中、内容、刷新次数与现在完全一样；只有真的慢（脚本、网络、大文件差异）才走"先默认、后刷新"。

**连带收益**: client 侧 JS 不再需要 150 ms 同步桥 (`invokeRenderSync` / `invokeCallbackSync` / `kClientSyncTimeoutMs`)。脚本里这两个回调可以直接返回字符串 / 数组，也可以返回 Promise (引擎在 resolve 后 `done`)。

### 6.4 受影响的声明

| 声明 | 改动 |
|---|---|
| `AgentxxToolCollapsedRenderFn` / `AgentxxToolExpandedRenderFn` | 追加 `notify` + `error_out` 参数 |
| `AgentxxToolRenderSpec` | `version` 1 → 2 (两个回调 typedef 换签名) |
| `AgentxxClientUiIface::register_command` 的 `execute` | 换成同一形态的内联函数指针 (随该表 VERSION → 2 + BASELINE 重置) |
| kit (`plugin_kit.h`) | 包一层"现在就算完"与"稍后 `done`"两种写法；脚本侧对应返回字符串/数组/Promise |

kit 里的工具写法不受影响: `fast_tool` 的业务体本来就是同步返回结果，内部等价于
"回调里立刻 `done`"；`blocking_tool` / `polled_tool` 本来就走 offload 与 op。只有渲染器
与命令的新 kit 写法需要区分"当场给"与"晚点给"两种。

### 6.5 入口符号为什么保持同步 (`get_info` / `create` / `destroy`)

这三个不是表成员，是 `dlsym` 出来的入口符号。三者的共同点: **契约内没有可等待的工作**，
异步化不会让调用方少等，只会把"必须立刻知道结果"的地方变成状态机。

| 入口 | 现在做什么 | 为什么不做成异步 |
|---|---|---|
| `get_info` | 返回一份静态常量结构（名字/版本/描述/API 版本） | 调用方拿到它才决定"继续加载还是拒绝"，是加载流程的第一个分支；契约明写"纯静态元数据, 不得读取/依赖任何实例状态"。异步化要先给它一个通知器上下文，而此刻实例还没建（没有 `host_ud`）；换来的只是"允许在这里读盘"，而那正好是不想允许的 |
| `create` | `new Ctx` + 存 `host` | 契约规定只构造、不注册、不起线程；重活属于 `start`（已是通知契约）。异步化会多出"半构造实例"的等待态与回滚分支。**真正要改的是执行线程**：客户端已把 `create` 卸到加载线程池，agent 侧现在直接在 io 线程调用（`lifecycle.h` 的 `createFn(...)`），本次一并挪到线程池 |
| `destroy` | 释放本地对象 | 它出现在**不能等待**的路径上: 加载失败回滚 (`rollbackLoad`) 与"实例最后一个引用析构"（那里还要决定是否 `dlclose`）。异步化会产生"`destroy` 已返回、插件代码还在跑、库不能卸"的新状态。重活必须在 `stop`（已异步，且 stop 完成后才 destroy） |

结论: 保持同步签名，但把 `create`（连同 `dlopen`）挪到加载线程池 —— "插件代码不占 io 线程"
这个目标照样达成，且不动 ABI。将来如果确实有插件必须在创建期等待 I/O，再**新增**入口符号
`<前缀>_create_async`（有则用、无则走同步），不推翻现有符号；`get_info` 与 `destroy`
不开这条路。

## 7. 目标 ABI 形态

### 7.1 成员签名

保持"平铺参数 + 尾部追加回调"，避免为每个成员新增 args 结构 (参数已稳定，扩展走 `_ex` 惯例):

```c
/// 一次异步表调用的结果 (回调参数; 只在回调期间有效)
typedef struct AgentxxPluginOpResult {
    int32_t status;    ///< PLUGINXX_OPERATOR_OK / FAILED / CANCELED
    int32_t _reserved; ///< 8 字节补齐
    /// 结果载荷: 与旧版出参 `PluginxxString* out` 的内容一致 (JSON 或纯文本)
    /// - status != OK 时是错误说明 (可空)
    PluginxxStringView payload;
} AgentxxPluginOpResult;

/// 完成回调 (可从任意线程发出; 主程序侧保证恰好一次)
typedef void(PLUGINXX_CALL* AgentxxPluginOpCallback)(
    void*                        ud,
    const AgentxxPluginOpResult* result
);

/// 示例: 会话上下文查询
PluginxxOperatorHandle*(PLUGINXX_CALL* get_messages)(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   scope,
    const PluginxxStringView* session_id,
    AgentxxPluginOpCallback   cb,          ///< 非空
    void*                     ud,
    PluginxxString*           error_out    ///< 受理失败时写入原因 (NULL 返回值)
);
```

- 返回 `NULL` = **未受理** (参数错 / 作用域失效 / 实例正在关闭)，`cb` 不会被调用；
- 返回非 NULL = 已受理，结果经 `cb` 恰好一次交回；
- `pluginxx.cancel.op_cancel` 可取消尚未完成的调用 (协作式)；
- 主程序保证回调在它自己的 io 线程上发出 (插件不必关心线程)，与现有 `invoke_capability` 一致。

### 7.2 顺序与并发

- 同一实例的表调用按**受理顺序**执行 (投递队列 FIFO)；跨实例互不影响。
- 成员实现内部可以 `co_await` 真正的异步动作 (磁盘/网络/等待会话)，但下游队列仍按受理顺序取，不会乱序。
- 不提供"并行执行同实例的表调用"开关：需要并发就用 `agentxx_async_run` / `submitAsyncWork` 那套后台任务，语义更清楚。

### 7.3 接口表版本策略

改签名属于破坏性变更。当前全局 `AGENTXX_PLUGIN_API_VERSION` = 5 (未发布)，本次直接:

- 全局抬到 **6**；
- 受影响的表**保留 IID 名字**，`VERSION` = 各表**现有值 + 1**（如 `agentxx.agent.tools` 3 → 4、`agentxx.client.ui` 1 → 2）、`MIN_VERSION` = 新的 `VERSION`、`BASELINE_SIZE` 重置为新的完整布局；
- 未受影响的表不动。

理由: 项目约定"重排/改签名 → 换 IID"的目的是在已发布版本下避免老插件拿到不匹配的表；本项目当前**未发布且明确不兼容旧插件**，重置 version/baseline 更简单，也不必发明第二套 IID 命名。等首次发布后如再改签名，才走"换 IID"。

`boundaries` 测试逐表校验四件套与成员顺序清单，改完必须同步更新它的期望表。

### 7.4 命名: 异步是默认，成员名不再带 `async`

大部分成员改造完后，"回调式异步"就是默认契约，名字里再带 `async` 只是重复。规则:

- **默认异步**: 表成员、kit 函数、脚本 API 一律不带 `async` 后缀；
- **只有真正阻塞的那个才加后缀**: `*Blocking`（如 `netCallBlocking` / `netFetchBlocking` / `tableOpBlocking`）；
- **后台任务域保留 `async`**: 那里的 async 指"放到后台跑"，与"回调式异步"是两件事 —— `agentxx_async_run/query/await/manager/emit`、`submit_async_work` / `cancel_async_work` / `get_async_work` / `list_async_work`、`AgentxxPluginAsyncWorkSpec`、`agentxx.agent.async.done`、`async_work.*` 配置键都不改。

改名清单:

| 位置 | 现名 | 新名 |
|---|---|---|
| 表成员 (`agentxx.agent.tools`) | `call_tool_async` | `call_tool` |
| 表成员 (`pluginxx.net`) | `call_async` | 删除，合并进 `call`（`call` 本身改成异步契约） |
| C++ kit | `netCallAsync(...)` | `netCall(...)`（返回 awaiter / 回调形态） |
| C++ kit | `asyncNetFetch(ex, ...)` | `netFetch(ex, ...)` 重载 |
| JS SDK | `netCallAsync` | 删除，`netCall` 改成返回 Promise |
| 框架注释 | `tables.h` 里引用的 `invoke_capability_async` | 该名字不存在，改为 `invoke_capability` |

`net.call` 由同步改成异步属于框架侧改动 (`cxx_pluginxx`)，单独一步做；改完 `net` 表只剩一个
入口，脚本侧也不再需要区分"快操作同步调、慢操作异步调"。

## 8. kit 层: 三种消费形态

| 形态 | 入口 | 约束 |
|---|---|---|
| 协程 | `co_await kit::tableOp(ctx, &Iface::member, args...)` | 生命周期入口 (start/mount) 与工具协程体里都可用 |
| 回调 | `kit::tableOpCallback(ctx, ..., [](result){...})` | 任意线程；回调由主程序投递回实例 io 线程 |
| 阻塞便捷 | `kit::tableOpBlocking(...)` | **禁止在 io 线程调用** (运行时检测 `is_io_thread` 直接返回错误)；用于插件自己的线程 / `blocking_tool` 体内 |

实现要点: `co_await` 形态与现有网络表的协程形态（改名后是 `netCall`，见 §7.4）
`co_await invoke_cap(...)` 用同一套挂起协议 (回调不直接恢复插件协程，而是经实例 io 线程恢复)，因此不用新造机制。

## 9. 分阶段实施

| 阶段 | 范围 | 产出 |
|---|---|---|
| A 骨架 | 新增 `AgentxxPluginOpResult`/`AgentxxPluginOpCallback`、全局 API 版本 6；挑 `agentxx.agent.session` 的 `get_share_store`/`add_share_store` 做成试点；已异步的成员先去后缀 (`call_tool_async` → `call_tool`) | kit 三种形态 + JS 桥试点 (Promise 化，删除该两处的同步桥) + 测试 |
| A2 加载线程 | agent 侧 `dlopen` + `create` 挪到加载线程池（与客户端一致，见 §6.5） | 插件加载/回滚用例；日志能看到入口不在 io 线程 |
| B 业务批 1 | `tools.register_tool`、`check_paths`、`context.get_messages`、`prompt.*`、`resources.*`、`graph.get/set_graph_json`、`mount.list/get`、`model.get_config` | 12 个内置插件与示例按需迁移 |
| C runtime 批 | `mount_root`、`open_channel`、`close_channel`、`unmount_root`、`get_channel_state`、`get_context_messages`、`list_runtime`、`submit_async_work`、`*-async_work` 查询 | 会话运行时相关测试 (`plugin_agent_runtime` / `plugin_example_runtime`) 全绿 |
| C2 net 统一 | 框架侧: `pluginxx.net` 的 `call` 改异步、删 `call_async`；kit 的 `netCallAsync` / `asyncNetFetch` 改名 (§7.4) | 4 个网络插件与 JS 桥用例 |
| D client 批 | `ui.*` 注册/更新、`session.get_client_state`、`self.get_own_info`、`keybind.list_keybinds`；三个回答型回调改异步 (工具折叠/展开渲染、命令 `execute`, 见 §6) | `client_plugins` / `plugin_ui` 相关用例 |
| E 收尾 | 删除残留同步路径与 `ioCallSyncKeep`、删除 client 侧 150 ms 同步桥 (`invokeRenderSync` / `invokeCallbackSync` / `kClientSyncTimeoutMs`)、更新 `plugins.md` §8.3 全表、`AGENTS.md`、`config-keys` 若涉及 | 全量回归 + 性能对比 |

每阶段独立可编译、可测；阶段 A 的试点决定后续所有成员的模板，先定稿再铺开。

## 10. 验证

- **功能**: 每阶段跑该表相关测试模块；阶段 E 跑 `plugins`、`client_plugins`、`plugin_*` 全量。
- **io 线程占用**: `KeyMetrics` 增加 `table_calls` (按表名 + 成员名累计次数/总耗时/最大耗时，仅在 `dev_mode` 收集)，加载快照输出。验收标准: 单次表调用在 io 线程上的耗时 P99 低于 2 ms (磁盘类成员经异步后不再计入 io 线程时间)。
- **JS 侧**: 表异步化后，`client_plugins` 里"脚本在渲染回调里调表"的用例应不再依赖 150 ms 兜底；引擎的 `ioCallSyncKeep` 调用点应归零。
- **回答型回调 (§6)**: 快路径 (回调内同步 `done`) 下渲染次数与缓存命中与改之前一致；慢路径 (稍后 `done`) 先显示默认内容、到达后刷新同一行；参数或版本变化后旧代际的 `done` 被丢弃；行删除/实例卸载后的 `done` 不写回界面。
- **顺序语义**: 新增用例 —— 连续 `register_tool(A)`→`register_tool(B)`→查询，两工具都可见且顺序稳定；`set_prompt`→`get_prompt` 看到新值。
- **命名 (§7.4)**: 全量检索确认表头、kit、脚本 API 里不再有"只为区分回调式异步"的 `_async` 名；`*Blocking` 与后台任务域的名字逐项核对。

## 11. 风险与对策

| 风险 | 对策 |
|---|---|
| ≈45 个成员要改签名，涉及 12 个内置插件与示例 | 分 5 阶段；阶段 A 定模板；每阶段只改一组表 |
| C++ 作者使用成本上升 | kit 三种形态；`start`/`mount` 体本来就是协程锚定，`co_await` 直接可用 |
| 顺序语义被误用 (注册后立刻用) | 成员内部保证按受理顺序执行；文档与 kit 示例强调"注册类调用要 `co_await` 完再继续" |
| 每次调用多一次 op 分配 | 高频只读成员 (messages_count / is_canceled / get_language) 保持同步；其余调用频度低 |
| 错误定位变分散 | 入口错误仍同步返回；回调里带 `status` + 文本，`resultName()` 统一翻译 |
| `create` 挪到线程池后，插件在 `create` 里直接调"仅 io 线程"的表成员 | kit 的 `ioCallSync` 已经是这条路径（客户端插件早就是同一个处境）；kit 注释与 `plugins.md` 各加一句 |
| 破坏性变更 | 未发布 + 明确不兼容；全局版本抬到 6，老插件在加载期被拒并给出"请重新编译"的日志 |

## 12. 不做的事

- 不把 `log` / `json.*` / `broadcasts.*` / `is_io_thread` / `op_cancel` / `tasks` 记账类改成异步 (语义要求同步，或改动没有收益)。
- 不把通知型回调 (动作/定时器/快捷键/广播订阅/通道事件/后台任务事件) 改成异步 —— 它们本来就不等结果，改了只多一次往返。
- 不给渲染/命令这种回答型回调加"请求超时与取消": 不答就一直用默认内容，行删除或实例卸载后迟到的结果直接丢弃。
- 不把 `get_info` / `destroy` 改成异步；`create` 只改执行线程，不改签名（理由见 §6.5）。
- 不引入"同一实例并行执行表调用"。
- 不为旧签名保留兼容成员 (不做双份接口)。
- 不改"插件提供实现"那一侧的通知契约 (工具/能力行/图节点/生命周期已经是 notify 形态)。
