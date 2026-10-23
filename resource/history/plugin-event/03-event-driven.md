# 事件驱动设计

> 关联: [index.md](index.md) · [02-runtime-design.md](02-runtime-design.md) · [04-prompt-and-impl.md](04-prompt-and-impl.md)

## 1. 结论

- **不修改 `BaseAgent` 的执行循环**。事件驱动做在"会话通道"之上，是运行时层的一个策略组件。
- 原语只有两个：`send_input(channel, json)`（立即投递一条）与 `register_trigger(spec)`（按规则投递）。
- 触发器只做三件事：**订阅一个主题**、**按策略决定何时投递**、**把载荷转成输入**。
- 定时触发是"事件源换成定时器"的同一种触发器，不引入第二套机制。

## 2. 为什么不在 `BaseAgent` 里加事件驱动

| 方案 | 会发生什么 |
|---|---|
| 在 ReAct 循环里"等事件再跑" | 内置会话的空闲语义被改变：现在 `runTurnAsync` 返回即结束，改成"等事件"后，谁来决定何时退出？取消、进程关闭、子代理复用全都受影响 |
| 在 `AgentRunner` 里加触发分支 | `AgentRunner` 同时服务主 agent 和子代理，二者对"为什么跑这一轮"的理解不同；触发策略会渗进中断处理与错误路径 |
| 在 `SessionServerAgentIO` 里加事件源 | 这个类已经承担"队列 + 中断 + 统计 + 历史同步"，再加触发规则会变成第二个调度器，且它按会话工作、触发器天然可按主题跨会话 |

反过来，做在通道之上有三个好处：

1. **零影响**：内置 agent 的路径一行不改，事件驱动出问题只会影响用它的人。
2. **可组合**：一个通道既能被 `send_input` 直接驱动，也能被触发器驱动，还能两者混用（人在群里说话 + 定时提醒）。
3. **可扩展**：新增事件源不需要动 agent，只需让某个东西调用 `emit_event`。

## 3. 事件模型

```
  事件源                          主题 (std::string 流)              触发器                通道
┌──────────────┐            ┌──────────────────────────┐      ┌────────────┐      ┌──────────┐
│ QQ/微信消息   │──emit────▶ │ plugin.qq.group_message.<gid> │──▶ │ COALESCE   │──▶   │ Channel  │
│ 定时器        │            │ plugin.qq.private_message.<uid> │   │ QUEUE      │      │ (会话)   │
│ 其它插件      │            │ plugin.xxx.<anything>        │      │ DROP_IF_BUSY│     └──────────┘
│ 将来的 webhook│            └──────────────────────────┘      └────────────┘
└──────────────┘
```

### 3.1 事件源

不引入"事件源对象"，只有一条约定：**能发布 `std::string` 事件的东西就是事件源**。

- 插件自己：`runtime.emit_event(topic, payload)`（任意线程，立即返回）或 `pluginxx.events.publish`。
- 其它插件：订阅同一主题即可串联（例如"邮件插件"收到新邮件后触发"QQ 插件"的通道）。
- 定时器：框架内部用 `asio::steady_timer` 产生，走同一条投递路径（不需要事件主题）。
- 将来的外部信号（webhook / 文件变更 / 系统通知）：由某个插件接收后 `emit_event`，框架不需要改。

主题命名沿用现有约定：不以 `plugin.` / `client.` 开头的自动补 `plugin.` 前缀（见 `PluginManager::qualifyEventTopic`）。

### 3.2 载荷约定

触发器不解析业务字段，只认两个可选键：

```json
{ "text": "要发给会话的文本", "attachments": [ ... ], "model": "…" }
```

- 有 `text` / `attachments` 时，框架把它们当作输入的正文与附件（与 `send_input` 的输入 JSON 同构）。
- 都没有时，框架把整条事件 JSON 文本作为输入（调试方便，但不推荐）。
- 其余字段原样忽略；插件要写"发言人: 内容"这类格式，在 `emit` 时自己拼好。

**不做过滤表达式**：需要按群/人分流时用主题分层（`qq.group_message.<群号>`）。这样框架里没有第二个 DSL，也不用担心解析开销与注入。

## 4. 投递策略

| 策略 | 语义 | 适用 |
|---|---|---|
| `QUEUE`（默认） | 每条事件立即 `send_input`；由会话队列保证串行 | 私聊消息、必须逐条处理的事件 |
| `COALESCE` | 在 `debounce_ms` 窗口内累积，窗口结束合并成一条输入 | 群聊刷屏、连续状态更新 |
| `DROP_IF_BUSY` | 通道正在跑（`running` / `waiting_input`）时丢弃，空闲才投 | 定时巡检、状态刷新 |

### 4.1 三种策略的实现位置

```
trigger.onEvent(payload)
    ├─ QUEUE        → channel.sendInput(renderInput(payload))
    ├─ COALESCE     → buffer.push(renderInput(payload)); restart(debounceTimer)
    │                  到期 → channel.sendInput(join(buffer, "\n")); buffer.clear()
    └─ DROP_IF_BUSY → if (channel.state == Idle) channel.sendInput(...) else dropped++
```

- 合并只发生在**同一个触发器**的缓冲里，不同触发器互不影响。
- 合并后仍走 `send_input`，因此 `input_ack`、队列状态、中断全部与直接投递一致。
- 丢弃会记一条 debug 日志并计数；诊断可见（`get_channel_state` 的 `dropped` 字段，后续追加）。

### 4.2 背压

| 位置 | 上限 | 超限行为 |
|---|---|---|
| 触发器合并缓冲 | 8 KB 文本 / 64 条 | 先投递已累积内容，再开始新窗口 |
| 通道输入队列 | `max_queue`（默认 100） | 丢掉最旧的一条，发 `TIP`（warning）告知调用方 |
| 会话上下文 | 由既有压缩中间件控制 | 与内置会话一致 |

群里连续几百条消息时，`COALESCE` + 8 KB 上限能保证不把上下文一次性打满。

## 5. 定时触发

> 以下代码为示意，用来确定接口形状；实际签名以 02 §10 的 SDK 为准。

```cpp
// 每 30 分钟提醒一次（只在通道空闲时投递）
int64_t t = addTimer(ctx, TimerOptions{
    .channelId = ch,
    .inputJson = R"({"text":"整理一下本群今天的待办","delivery":"next-turn"})",
    .firstDelayMs = 60 * 1000,
    .intervalMs = 30 * 60 * 1000,
    .policy = DROP_IF_BUSY,
});
```

- 一次性（`interval_ms = 0`）在触发后自动注销。
- 周期定时器在通道关闭、根卸载、插件禁用/卸载时取消；不依赖插件自己记得清。
- 定时器实现放在运行时（每个实例一个 `steady_timer` 池，或在 io 线程上每个定时器一个 `steady_timer`）。数量上限 256，成本可忽略。
- 不提供 cron 表达式：需要的插件可自行算下一次时间，用一次性定时器 + 重新注册实现。

`first_delay_ms` 用相对时间而不是绝对时间戳，避免插件与主程序时钟/时区不一致的问题。

## 6. 与核心事件的关系（`core.*` 保留但本次不接）

`EventBus` 同一主题只能有一种载荷类型（约束 C3）。核心事件是强类型（`EventAgentTurnEnd` 等），插件事件是 `std::string`，两者不能共用主题。

本次做法：

- 触发器只接 `std::string` 流（即 `plugin.*` 主题）。
- 保留 `core.` 前缀作为将来的"核心事件镜像"命名空间（框架在发布核心事件时，若检测到 `core.*` 有订阅者，再额外发一份 JSON）。本次不实现，避免为暂时用不到的功能增加热路径开销。

对 QQ/微信插件没有影响：它关心的是**自己创建的通道**，而通道事件回调（`TURN_END` 等）已经覆盖了这些信息。

## 7. 完整流程：QQ 群接入

### 7.1 进程级（`start`）：建根 + 连协议

> 示例里的 `sendToGroup` / `personaConfig` 等是示意函数，重点在"哪段代码在哪个阶段跑"。
> 记住上一篇的两条硬规则：**`create`/`start` 全程各一次**；**工具只能在 `mount` 里注册**。

```cpp
class QqPluginCtx : public agentxx::plugin::PluginBase {
public:
    // ---- 进程级（start 里准备）----
    std::string                              rootId;        // 本插件的根（自定义配置）
    std::map<std::string, int64_t>           groupChannel;  // 群号 → 通道
    std::map<int64_t, PendingAsk>            pendingAsks;   // interruptId → 待人工答复

    // ---- 挂载级（每个 agent 一份，mount 返回）----
    struct AgentState { AgentRef ref; };   // 只用来记"给哪个 agent 注册了工具"

    // 群消息进入（插件自己的协议线程）
    void onGroupMessage(const std::string& gid, const std::string& sender,
                        const std::string& text) {
        Json ev = {
            {"groupId", gid}, {"sender", sender}, {"text", text},
            {"input", fmt::format("{}: {}", sender, text)},  // 框架取 text/input
        };
        emitEvent(*this, "qq.group_message." + gid, ev.dump());  // 任意线程，立即返回
    }

    void onChannelEvent(const ChannelEvent& ev);  // 见 7.3
};

// 进程级：建根 + 连协议，只跑一次
int32_t start(QqPluginCtx& ctx) {
    if (!ctx.hasRuntime()) {              // 老主程序：没有 agentxx.agent.runtime
        return 0;
    }
    // 自己的根：独立系统提示词 + 独立工具集，不影响内置 coding 会话
    // 根默认不挂任何插件（只有内置工具），要把文件工具给根就在这里列出来
    ctx.rootId = mountRoot(ctx, MountRootOptions{
        .name = "qq",
        .config = personaConfig(),
        .mountPlugins   = R"({"mode":"listed","list":["agentxx_filesystem"]})",
        .includeCreator = true,           // 根也要能用 qq_send_message
    });
    if (ctx.rootId.empty()) {
        return -1;
    }
    return startQqProtocol(ctx);          // 连 QQ：进程级，只一次
}

// 每个 agent 一次：只注册工具，不建根、不建连
void* mount(QqPluginCtx& ctx, const AgentxxPluginAgentMountSpec* spec,
            const PluginxxOperatorNotify* notify, PluginxxString*) {
    auto* st = new QqPluginCtx::AgentState{AgentRef(*spec)};
    tool(ctx, st->ref, "qq_send_message", "把一条消息发到指定 QQ 群。",
         R"({"type":"object","properties":{"group_id":{"type":"string"},
                                            "text":{"type":"string"}}})",
         [st](std::string_view args) { return sendToGroup(st, args); });
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return st;                            // = mount_ud
}

void* unmount(QqPluginCtx&, void* ud,
              const PluginxxOperatorNotify* notify, PluginxxString*) {
    delete static_cast<QqPluginCtx::AgentState*>(ud);
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

// 进程级：拆根（其下通道自动关闭）
void stop(QqPluginCtx& ctx) {
    if (ctx.rootId.empty()) {
        return;
    }
    unmountRoot(ctx, ctx.rootId);
    ctx.rootId.clear();
}
```

这段拆分的意义：

| 代码 | 阶段 | 为什么不能放错 |
|---|---|---|
| `mountRoot` / `unmountRoot` / 连 QQ | `start` / `stop`（进程级） | 放 `mount` 里会每个 agent 建一次根 → 无限嵌套（宿主也会拒绝） |
| `tool(...)` 注册工具 | `mount`（每个 agent） | 放 `start` 里会被拒（`ERR_NO_AGENT`）；而且工具本来就是"某个 agent 的工具" |
| `openChannel` / `addTrigger` | 拿到 `rootId` 之后（`start` 尾部或首次消息） | 通道绑定在根上，根先有 id |

### 7.2 每个群一个通道（首次收到消息时懒开）

```cpp
int64_t ensureGroupChannel(QqPluginCtx& ctx, const std::string& gid) {
    auto it = ctx.groupChannel.find(gid);
    if (it != ctx.groupChannel.end()) {
        return it->second;
    }
    ChannelOptions opt;
    opt.rootId    = ctx.rootId;                  // 挂在自己的根上
    opt.sessionId = "qq_group_" + gid;           // 稳定 id：重启后继续同一段上下文
    opt.title     = "QQ群 " + gid;
    opt.interruptTimeout = std::chrono::minutes(10);
    opt.maxQueue  = 50;

    int64_t ch = openChannel(ctx, opt, [&ctx](const ChannelEvent& ev) {
        ctx.onChannelEvent(ev);
    });
    if (ch > 0) {
        ctx.groupChannel[gid] = ch;
        addTrigger(ctx, TriggerOptions{
            .topic = "qq.group_message." + gid,
            .channelId = ch,
            .policy = TriggerPolicy::Coalesce,
            .debounceMs = 800,
        });
    }
    return ch;
}
```

要点：

- **配置类差异用根**：系统提示词、`toolWhitelist` 写在 `mountRoot` 的 `config` 里，与内置会话完全分离。
- **工具类差异用挂载**：根要有文件工具就得在 `mountPlugins` 里列出来；本插件自己的工具在 `mount` 里注册。
- **对话类差异用会话**：一个根服务所有群，每群一个 `sessionId` → 上下文、模型选择、工作目录、权限边界、share_store 互不影响。
- 建根只在 `start`；在 `mount` 里建根会被宿主拒绝（`ERR_STATE`），且默认不把创建者挂到自己的根上（`include_creator=false`）。
- 只有“客服群”与“运维群”这种需要不同人设/工具的组合才各挂一个根；不要每群一个根。
- `sessionId` 由插件命名并保存到插件自己的配置里 → 重启后接着上一段上下文。
- 触发器按群分主题 → 框架不需要“事件属于哪个群”的知识。

### 7.3 处理通道事件

```cpp
void QqPluginCtx::onChannelEvent(const ChannelEvent& ev) {
    switch (ev.kind) {
    case ChannelEventKind::TurnEnd: {
        if (!ev.text.empty()) {
            sendToGroup(groupOf(ev.channelId), std::string{ev.text});  // 最终回答
        } else {
            auto p = Json::parse(ev.payloadJson);
            if (p.value("hasError", false)) { /* 群里提示失败原因 */ }
        }
        break;
    }
    case ChannelEventKind::ToolStart: {
        auto p = Json::parse(ev.payloadJson);
        showTyping(groupOf(ev.channelId), p.value("toolName", ""));
        break;
    }
    case ChannelEventKind::Interrupt: {
        // 权限确认/需要补充信息：把问题发到群里或转给管理员
        auto p   = Json::parse(ev.payloadJson);
        auto ask = postQuestionAndWait(groupOf(ev.channelId), p.value("argJson", ""));
        pendingAsks[ev.interruptId] = ask;   // 人工答复到达时再 respond
        break;
    }
    case ChannelEventKind::Error:
    case ChannelEventKind::Closed:
        // 记日志；Closed 时从 groupChannel 里摘掉
        break;
    default:
        break;
    }
}

// 管理员在群里回复 "同意" 后
void QqPluginCtx::onAdminReply(const std::string& gid, const std::string& text) {
    auto chIt = groupChannel.find(gid);
    if (chIt == groupChannel.end()) return;
    for (auto it = pendingAsks.begin(); it != pendingAsks.end(); ++it) {
        if (it->second.gid != gid) continue;
        Json values = {{"values", Json::array({text})}, {"options", Json::object()}};
        respondInterrupt(*this, chIt->second, it->first, values.dump());
        pendingAsks.erase(it);
        break;
    }
}
```

中断的三种常见处置：

| 场景 | 做法 |
|---|---|
| 群里问"是否允许执行命令" | 把 `argJson` 转成可读文本发群，等管理员回复，`respond_interrupt` |
| 私聊场景无人应答 | 配 `interrupt_timeout_ms`，超时按取消处理，插件收到 `INTERRUPT_EXPIRED` |
| 全自动机器人 | 插件直接按策略应答（如全部允许/全部拒绝），不发群消息 |

## 8. 并发、隔离与背压

### 8.1 不同群之间

- 每群一个通道 → `SessionServerAgentIO` 的队列天然串行，群内消息不会交叉。
- 不同通道是独立协程，同一个 io 线程交错执行：A 群等 LLM 时 B 群的输入、工具、回调都能跑。
- 一个群的长任务不会阻塞其它群（除非插件在回调里同步阻塞，文档明确禁止）。

### 8.2 同群多条消息

| 场景 | 结果 |
|---|---|
| 两条消息间隔 > 合并窗口 | 两轮，按序 |
| 两条消息在窗口内 | 合并成一条输入，一轮 |
| 一轮进行中又来消息（QUEUE） | 进队列，本轮结束后执行 |
| 一轮进行中又来消息（DROP_IF_BUSY） | 丢弃并计数 |

### 8.3 与 `next-step` / `inject` 的配合

- `next-step`：群里补充一句"顺便看下日志"时，可在当前轮的下一个模型调用边界注入，不必等整轮结束。
- `inject`：只在下一次请求里出现一次，不改写会话上下文——适合"临时提示"。

触发器与 `send_input` 都支持这三种模式，插件按业务选。

## 9. 扩展方向（不改本次接口）

| 方向 | 做法 |
|---|---|
| 定时提醒 | 已有 `register_timer` |
| webhook / 外部信号 | 某个插件监听端口，收到后 `emit_event`；或后续在 `runtime` 追加 `register_http_source` |
| 文件变更 | 插件用现有能力监听后 `emit_event` |
| 跨插件编排 | 插件 A 的事件 → 插件 B 的触发器（同一主题总线上直接串起来） |
| 核心事件触发 | 后续在 02 的表尾追加 `register_core_trigger`（订阅 typed 流并转 JSON），本次保留命名空间不实现 |
| 按条件过滤 | 需要时再引入，优先让插件在 `emit` 前自己判断（避免框架里出现第二套表达式） |

## 10. 明确不做

- 不做"事件驱动的 `BaseAgent`"：执行核心保持现状。
- 不做 cron 表达式解析（插件用一次性定时器自行计算下次时间）。
- 不做事件回放/持久化：事件是易失的；需要持久化的业务由插件自己存。
- 不做跨进程事件：远程场景已有 WS 端点，插件场景是同进程。
- 不做"事件 → 任意工具调用"（只到"会话输入"这一层，边界清晰）。
