# agentxx_chatbot

把「人设」接到聊天平台（当前实现 QQ），把群聊 / 私聊消息投给 agent 会话，再把模型写的回复发回平台。

- 设计文档：[resource/history/chatbot/plan.md](../../../resource/history/chatbot/plan.md)
- 实施记录与已知问题：[resource/history/chatbot/work.md](../../../resource/history/chatbot/work.md)
- 插件机制总览：[docs/zh-cn/design/plugins.md](../../../docs/zh-cn/design/plugins.md) §13.1

---

## 1. 它怎么工作

```
QQ 群/私聊 ──► IPlatform (qq) ──► ChatbotEngine ──► 会话通道 (一个群/私聊一个会话)
                    ▲                  │  ▲                    │
                    │                  │  └── 聚合 / 去重 / 路由 ◄┤ 模型输出
                    └── 出站队列 ◄──────┴── chatbot_send_message ◄┘
```

三条规则先记住：

1. **每个人设 = 一个根 agent**。插件在自己的 `start` 里对人设调 `mount_root`，提示词、模型、
   工具白名单都来自人设配置。内置的那个主会话与它无关。
   要给这个根**更多工具**（文件读写、执行命令、联网……），用 `personas[].mount` 把对应插件
   挂上去，见 §2；不配就只有 `chatbot_*` 这一套。
2. **每个「账号 + 群 或 私聊」= 一个独立会话**。会话 id 形如
   `chatbot_<人设>_<账号>_<group|private>_<chatId 的 16 位散列>`，跨重启稳定 —— 同一个群永远
   是同一段上下文。
3. **回复只有一个出口：`chatbot_send_message`**。模型不调这个工具就等于「不说话」，
   agent 的正文不会自动发到群里。`behavior.fallbackReply` 可以让插件在模型漏调时兜底。

## 2. 配置

配置写在 `agentxx-config.yaml` 的 `plugin.list[]` 里，两段来源都支持：

- 直接写在 `args` 下（推荐，改动集中在一处）；
- 或写进单独的配置文件，用 `config` 指过去（映射逐键合并、列表整体覆盖，`args` 覆盖文件）。

```yaml
plugin:
  list:
    - name: agentxx_chatbot
      path: plugins/agentxx_chatbot
      args:
        # ---------- 第一段: 平台账号 ----------
        accounts:
          - id: qq_main                 # 人设按这个 id 对接; 只能用 [A-Za-z0-9_-]
            type: qq                    # qq | fake (fake 是内存假平台, 测试用)
            appId: ${QQBOT_APPID}
            appSecret: ${QQBOT_APPSECRET}
            # 也可以从 .env 形态的文件读, 避免凭证进版本库:
            # appIdFile: resource/keys/qq_bot.env      # 读其中的 QQBOT_APPID
            # appSecretFile: resource/keys/qq_bot.env  # 读其中的 QQBOT_APPSECR(S)ET
            sandbox: false
            # 主动消息频控覆盖 (0 = 用平台默认值)
            groupPerMinute: 0
            singlePerMinute: 0

        # ---------- 第二段: 人设 ----------
        personas:
          - name: assistant             # 会话 id 前缀; 只能用 [A-Za-z0-9_-]
            # model: some-model         # 留空 = 用主程序当前模型
            prompt:
              systemPrompt: |
                你是群里的一个技术助手, 说话简短口语化。
            # tools: [agentxx_share_store]   # 工具白名单 (工具名); 留空 = 不过滤
            mount:                       # 这个根上除此之外还挂哪些插件 (决定模型能用哪些工具)
              mode: none                 # none | all | listed
              # - none:   只有 chatbot_* 工具
              # - all:    挂进程里加载的全部插件 (与内置主会话看到的工具一致)
              # - listed: 只挂 list 里的; chatbot 自己会被自动补进名单
              list: [agentxx_filesystem, agentxx_execute_command, agentxx_websearch]
              # args:                    # 给被挂插件一份专属配置 (键 = 插件名, 覆盖主配置)
              #   agentxx_filesystem: { ... }
            bind:
              - account: qq_main
                chats:
                  - { type: group, id: "123456789", mode: mention }
                  - { type: private, id: "ABCDEF...", mode: always }
                  - { type: group, id: "*", mode: mention }   # 通配: 该账号下所有群

        # ---------- 第三段: 通用行为 ----------
        behavior:
          aggregateWindowMs: 800        # always 模式的静默窗口 (毫秒)
          sendIntervalMs: 350           # 一轮内多条回复之间的间隔
          messageSplitLimit: 1500       # 单条文本上限 (UTF-8 码点; 0 = 不切分)
          maxSplitParts: 8              # 单条最多切成几段
          maxRepliesPerTurn: 5          # 一轮最多发几条
          maxChatsPerPersona: 64        # 每人设活跃会话上限 (超了回收最久没动的)
          fallbackReply: mention        # 模型没调工具时: mention | always | never
          overflowPolicy: active        # 被动回复次数用尽后: active | truncate | fail
          humanInLoop: auto-allow       # 群里的权限询问弹不出来: auto-allow | auto-deny
          injectContextHint: true       # 每轮输入开头带「你在哪个群」
          attachments:                  # 附件策略 (注意这层嵌套: 平铺写在 behavior 下不生效)
            fetch: auto                 # auto | url | download
            maxPerMessage: 5            # 单条消息最多处理几个附件 (主程序上限 5)
            inlineTextMaxBytes: 65536   # 小文本文件正文直接内联进输入 (0 = 关闭)
          exposeToolsOnHost: false      # 是否把 chatbot_* 工具也注册到内置主会话
          debugTools: false             # 是否注册 chatbot_debug_* 调试工具
```

配置只有三段：`accounts`（账号）/ `personas`（人设）/ `behavior`（通用行为）。
关键词命令**不在配置里**，由插件内置（见 §2.3）；
启动时只在日志里提一句「命令由插件内置」。

### 2.1 三种触发模式（`bind.chats[].mode`）

| mode | 行为 |
|------|------|
| `mention` | 只有被 @ 的消息唤醒一轮；期间未被 @ 的消息**仍进缓冲**，跟着下一次被 @ 一起带上去 |
| `always` | 任何新消息都投递；多条消息按 `aggregateWindowMs` 静默窗口合并成一次输入 |
| `off` | 该会话完全不处理（也可以运行期用 `chatbot_set_mode` 切换） |

私聊里 `mention` 按 `always` 处理（单聊消息本来就是直接发给机器人的）。

### 2.2 凭证

`appId` 是长数字，直接写进 yaml 容易被 YAML 解析成整数甚至浮点（精度丢失）。三种写法任选：

- `appId: 123456789`（纯数字没问题，但太长时请用下面两种）；
- `appId: "${QQBOT_APPID}"`（环境变量展开）；
- `appIdFile: resource/keys/qq_bot.env`（从 `.env` 形态的文件读 `QQBOT_APPID`）。

`appSecretFile` 同理，键名是 `QQBOT_APPSECR(S)ET`（两种拼写都接受，平台文档里两种都出现过）。

### 2.3 内置命令

命令写在 `chatbot_core.cpp` 的 `builtinCommands()` 里，统一格式 `/agentxx-cmd <子命令>`
（单独发 `/agentxx-cmd` 等同于 `help`；大小写不敏感，子命令后面可以跟一段文字，作为 `{args}` 用）：

| 命令 | 行为 |
|------|------|
| `/agentxx-cmd` / `/agentxx-cmd help` | 列出全部命令（`{commandList}` 由同一张表拼出来） |
| `/agentxx-cmd info` | 当前会话的人设 / 模式 / 通道 / 计数，排查「为什么不回复」先看它 |
| `/agentxx-cmd hello` | 让模型向发言人打个招呼（`model` 模式：走一轮模型） |
| `/agentxx-cmd stop` | 打断当前这一轮（`action: cancel`；不回复任何文本） |

未知子命令由根命令接住回帮助（不会当成聊天内容投给模型）。

**命令是插件自己的交互约定**，所以没有做配置项：每个部署各写一套名字与文案，
文档、用例、排错说法就对不上了。要改命令请改 `builtinCommands()`（改完补
`chatbot_core` 的 `builtinCommands` 用例）。

## 3. 模型看到的输入

每轮输入是一段结构化文本，首行是「你在哪个群」，之后每条消息一行：

```
QQ群「技术交流群」(群 id 尾号 6789); 下面是最近收到的消息：
[#1] 小张(member): 这个报错怎么解
[#2] 小李(member) @机器人: (回复 小张: 这个报错怎么解) 我猜是权限问题
```

- `[#n]` 是本轮编号，`chatbot_send_message` 的 `replyTo` 就填它（引用第 n 条）；
- 图片 / 音频 / 视频作为多模态附件随输入投给模型，文本里另有对应行；普通文件只给本地路径
  （模型可以自己用文件工具读）。

## 4. 工具

| 工具 | 用途 |
|------|------|
| `chatbot_send_message` | **唯一的回复出口**。参数：`text`（正文）、`attachments`（本地路径或 http(s) 地址数组）、`replyTo`（引用本轮第 n 条）、`markdown`、`chat`（跨会话时必须显式给） |
| `chatbot_history` | 取该会话最近 N 条原始消息（含被 mode 过滤掉的） |
| `chatbot_chat_info` | 会话状态：路由到哪个人设、当前模式、通道 id、计数、缓冲条数 |
| `chatbot_status` | 插件全局状态：账号连接、人设、出站队列、计数 |
| `chatbot_set_mode` | 运行期改触发模式（不写回配置） |
| `chatbot_recall_message` | 撤回自己刚发出的消息（平台时限内，QQ 为 2 分钟）。参数：`target`（`-1` 最后一条、`-2` 倒数第二条…）、`skip` + `count`（跳过最近几条再连撤几条）、`messageId`、`chat` |
| `chatbot_debug_inject` / `chatbot_debug_outbox` | 仅 `behavior.debugTools: true` 时注册：直接注入假消息 / 回看假平台收到了什么 |

工具只注册在**人设根**上。内置主会话默认看不到它们（`exposeToolsOnHost: true` 才会注册）。

## 5. 常见问题

**群里完全不回复。** 按顺序查：

1. `chatbot_status` 里账号状态是不是 `online`（`connecting` / `reconnecting` 说明网关没连上）；
2. 该群有没有在某个 `personas[].bind` 里（`chatbot_chat_info` 的 `persona` 为空就是没路由到）；
3. 模式是不是 `mention` 而这条消息没 @ 机器人；或者被 `chatbot_set_mode` 改成了 `off`；
4. 模型有没有调 `chatbot_send_message`（`fallbackReply: never` 时不调就真的不说话）。

**回复被切成好几条。** 正文超过 `messageSplitLimit` 会自动切段并带 `(i/n)` 标记；
一轮超过 `maxRepliesPerTurn` 的部分会丢弃，工具结果里会说明「已发满」。

**回复发不出去，日志里有「被动回复次数用尽」。** 同一条消息平台允许的被动回复次数有限，
用尽后按 `overflowPolicy` 处理（默认降级为主动消息；`truncate` 放弃、`fail` 报给模型）。

**凭证泄露。** 用 `appIdFile` / `appSecretFile` 指向 `.env` 文件，并把那个文件加进 `.gitignore`。
日志里不会打印凭证取值。

**群里发文件给它看，它说看不到。** 群文件的附件挂在**被引用那条消息**的
`msg_elements[].attachments` 里（不是顶层 `attachments`），插件已按这个结构解析。
落盘后小文本文件（默认 ≤ 64 KiB，`behavior.attachments.inlineTextMaxBytes`）的正文会
**直接内联进输入**，所以人设根没挂文件系统插件也能读；更大的或二进制文件只在输入里给路径，
要靠人设根挂了 `agentxx_filesystem` 才能读。

**想在不连平台的情况下试一遍。** 把账号 `type` 改成 `fake`、打开 `behavior.debugTools`，
然后用工具 `chatbot_debug_inject` 注入消息、`chatbot_debug_outbox` 看回复。

## 6. 扩展新平台

`platform/platform.h` 的 `IPlatform` 是唯一需要实现的东西（`start` / `stop` / `send` /
`caps` / `statusJson`），把平台事件归一化成 `chatbot_types.h` 的 `InboundEvent`、把出站收成
`OutboundMessage` 即可。平台差异写成 `PlatformCaps` 的数据（markdown / 被动回复 / 引用 / 单条上限），
不在业务代码里写 if。

当前实现：`platform/qq/`（QQ），`platform/fake/`（内存假平台，测试与本地演示）。
