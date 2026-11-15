# 插件接口表异步化设计

> 目标读者: 修改插件接口表 / vtable / kit 的开发者
> 相关文档: [plugins.md](../../../docs/zh-cn/design/plugins.md) (插件系统总览与 §8 接口表分类) · [js-runtime plan](../js-runtime-singleton/plan.md) (JS 运行时单例重构) · [index.md](../../../docs/zh-cn/design/index.md) (主程序架构)
>
> 本文件是任务规划 (未实施), 落地后结论搬进 [plugins.md](../../../docs/zh-cn/design/plugins.md) §8 与 `AGENTS.md`。

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

1. **能异步的一律异步**，只有"必须同步"的语义才保留同步 (清单见 §6)。
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
| `net.*` | ⏸/🔁 | `info`、`call` 是快操作；`call_async`/`fetch`/`download` 已是异步 |

**统计** (agent 侧 53 + client 36 + 通用 37 成员): 改异步 ≈ 45 项，保持同步 ≈ 55 项，已是异步 ≈ 12 项，明确不可异步 ≈ 14 项。

## 6. 目标 ABI 形态

### 6.1 成员签名

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

### 6.2 顺序与并发

- 同一实例的表调用按**受理顺序**执行 (投递队列 FIFO)；跨实例互不影响。
- 成员实现内部可以 `co_await` 真正的异步动作 (磁盘/网络/等待会话)，但下游队列仍按受理顺序取，不会乱序。
- 不提供"并行执行同实例的表调用"开关：需要并发就用 `agentxx_async_run` / `submitAsyncWork` 那套后台任务，语义更清楚。

### 6.3 接口表版本策略

改签名属于破坏性变更。当前全局 `AGENTXX_PLUGIN_API_VERSION` = 5 (未发布)，本次直接:

- 全局抬到 **6**；
- 受影响的表**保留 IID 名字**，`VERSION` 抬到 2、`MIN_VERSION` 同步抬到 2、`BASELINE_SIZE` 重置为新的完整布局；
- 未受影响的表不动。

理由: 项目约定"重排/改签名 → 换 IID"的目的是在已发布版本下避免老插件拿到不匹配的表；本项目当前**未发布且明确不兼容旧插件**，重置 version/baseline 更简单，也不必发明第二套 IID 命名。等首次发布后如再改签名，才走"换 IID"。

`boundaries` 测试逐表校验四件套与成员顺序清单，改完必须同步更新它的期望表。

## 7. kit 层: 三种消费形态

| 形态 | 入口 | 约束 |
|---|---|---|
| 协程 | `co_await kit::tableOp(ctx, &Iface::member, args...)` | 生命周期入口 (start/mount) 与工具协程体里都可用 |
| 回调 | `kit::tableOpCallback(ctx, ..., [](result){...})` | 任意线程；回调由主程序投递回实例 io 线程 |
| 阻塞便捷 | `kit::tableOpBlocking(...)` | **禁止在 io 线程调用** (运行时检测 `is_io_thread` 直接返回错误)；用于插件自己的线程 / `blocking_tool` 体内 |

实现要点: `co_await` 形态与现有 `co_await netCallAsync(ctx, op, args)`、`co_await invoke_cap(...)` 用同一套挂起协议 (回调不直接恢复插件协程，而是经实例 io 线程恢复)，因此不用新造机制。

## 8. 分阶段实施

| 阶段 | 范围 | 产出 |
|---|---|---|
| A 骨架 | 新增 `AgentxxPluginOpResult`/`AgentxxPluginOpCallback`、全局 API 版本 6；挑 `agentxx.agent.session` 的 `get_share_store`/`add_share_store` 做成试点 | kit 三种形态 + JS 桥试点 (Promise 化，删除该两处的同步桥) + 测试 |
| B 业务批 1 | `tools.register_tool`、`check_paths`、`context.get_messages`、`prompt.*`、`resources.*`、`graph.get/set_graph_json`、`mount.list/get`、`model.get_config` | 12 个内置插件与示例按需迁移 |
| C runtime 批 | `mount_root`、`open_channel`、`close_channel`、`unmount_root`、`get_channel_state`、`get_context_messages`、`list_runtime`、`submit_async_work`、`*-async_work` 查询 | 会话运行时相关测试 (`plugin_agent_runtime` / `plugin_example_runtime`) 全绿 |
| D client 批 | `ui.*` 注册/更新、`session.get_client_state`、`self.get_own_info`、`keybind.list_keybinds` | `client_plugins` / `plugin_ui` 相关用例 |
| E 收尾 | 删除残留同步路径与 `ioCallSyncKeep`、更新 `plugins.md` §8.3 全表、`AGENTS.md`、`config-keys` 若涉及 | 全量回归 + 性能对比 |

每阶段独立可编译、可测；阶段 A 的试点决定后续所有成员的模板，先定稿再铺开。

## 9. 验证

- **功能**: 每阶段跑该表相关测试模块；阶段 E 跑 `plugins`、`client_plugins`、`plugin_*` 全量。
- **io 线程占用**: `KeyMetrics` 增加 `table_calls` (按表名 + 成员名累计次数/总耗时/最大耗时，仅在 `dev_mode` 收集)，加载快照输出。验收标准: 单次表调用在 io 线程上的耗时 P99 低于 2 ms (磁盘类成员经异步后不再计入 io 线程时间)。
- **JS 侧**: 表异步化后，`client_plugins` 里"脚本在渲染回调里调表"的用例应不再依赖 150 ms 兜底；引擎的 `ioCallSyncKeep` 调用点应归零。
- **顺序语义**: 新增用例 —— 连续 `register_tool(A)`→`register_tool(B)`→查询，两工具都可见且顺序稳定；`set_prompt`→`get_prompt` 看到新值。

## 10. 风险与对策

| 风险 | 对策 |
|---|---|
| 89 个成员一次性改动，迁移面大 | 分 5 阶段；阶段 A 定模板；每阶段只改一组表 |
| C++ 作者使用成本上升 | kit 三种形态；`start`/`mount` 体本来就是协程锚定，`co_await` 直接可用 |
| 顺序语义被误用 (注册后立刻用) | 成员内部保证按受理顺序执行；文档与 kit 示例强调"注册类调用要 `co_await` 完再继续" |
| 每次调用多一次 op 分配 | 高频只读成员 (messages_count / is_canceled / get_language) 保持同步；其余调用频度低 |
| 错误定位变分散 | 入口错误仍同步返回；回调里带 `status` + 文本，`resultName()` 统一翻译 |
| 破坏性变更 | 未发布 + 明确不兼容；全局版本抬到 6，老插件在加载期被拒并给出"请重新编译"的日志 |

## 11. 不做的事

- 不把 `log` / `json.*` / `broadcasts.*` / `is_io_thread` / `op_cancel` / `tasks` 记账类改成异步 (语义要求同步，或改动没有收益)。
- 不引入"同一实例并行执行表调用"。
- 不为旧签名保留兼容成员 (不做双份接口)。
- 不改"插件提供实现"那一侧的通知契约 (工具/能力行/图节点/生命周期已经是 notify 形态)。
