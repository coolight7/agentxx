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
3. **回复只有一个出口：`chatbot_send_message`**。想让对方看到的话必须调这个工具。
   模型不调工具就等于「不说话」，但注意 `behavior.fallbackReply`（默认 `mention`）会在
   模型漏调时把**本轮最后一段正文**兜底发出去 —— 所以「不发言」要表达清楚：**正文只写
   `[静默]` 这一个标记**（插件认这个标记，见 §3）。不写标记又写在正文里的东西（例如
   `（保持安静）`）在兜底生效的会话里会被当成回复发出去；`fallbackReply: never` 时
   正文一律不发。

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
          fallbackReply: mention        # 模型没调工具时: mention (被 @ 或 always 会话) | always | never; 正文只有 [静默] 时不发 (见 §3.1)
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

#### `always` 的前置条件：平台要给机器人「接收所有消息」

QQ 群里默认**只有 @ 机器人的消息**会推送（事件 `GROUP_AT_MESSAGE_CREATE`）。群里
的普通消息走另一个事件（`GROUP_MESSAGE_CREATE`），必须先给机器人开「接收所有消息」
（也叫「全量消息」）才会推。没开时，插件根本收不到非 @ 的消息，群里 `always`
在效果上等同 `mention`，而且**插件这边改不了**。

**这个开关不在开发者控制台**，而是**群主**在 QQ 里给机器人授的群内权限 —— 官方
「Agent 接入」文档与 FAQ 两处都这么写（FAQ「在群里@机器人不回复，但私聊可以」：

> 用 iOS 或 Android 手机升级最新版手机 QQ，使用**群主身份**在添加 QQBot 入群时，
> 点击「设置权限」：①接收全量消息能力 ②主动推送消息能力 ③机器人撤回成员消息能力。

已经进群的机器人，按官方「Agent 接入」文档给的路径改：

1. 用**群主**账号（不是管理员、也不是机器人创建者账号）打开这个群；
2. 群聊右上角「≡」→ **群管理** → **群成员** → 点机器人的头像进资料页；
3. 资料页**右上角「设置」** → 把「**机器人可获取的群聊消息范围**」设为
   「**获取群内全部消息**」，同页打开「允许主动发送消息」；
4. 授权后让插件重连一次（或等网关重连），`chatbot_status` 的 `fullModeMessage`
   就会开始增长。

几个容易踩的地方：

- 从聊天窗口点机器人头像弹出的是**资料卡**，不是上面说的「资料页 → 右上角设置」，
  那里只显示「仅@回复」这类状态文字，改不了；
- 那一项**只对群主显示**，管理员进去只能看到「允许主动发送消息」；
- 如果群主进去也没有「机器人可获取的群聊消息范围」这一行：把这个机器人**移出群再
  重新添加**（群主添加时会弹「设置权限」面板），或换一台装最新版手机 QQ 的设备再看
  （客户端有灰度差异）；
- 控制台里找不到是正常的：开放平台**旧版管理端**的「功能配置」已停止（官方 FAQ
  明说），新版管理端管的是机器人本身（名称/头像/凭证/使用范围），群内权限只由群主授予。

拿不到这项权限时也别把群聊绑成 `always`：群里用 `mention` 就好 —— 未被 @ 的消息
仍会进缓冲，跟着下一次 @ 一起带上去（相当于「@ 一次就能看到前面所有对话」）；
私聊不受这条限制，`always` 在私聊里照常生效。

怎么确认是这种情况（不用翻日志）：

1. 调 `chatbot_status`，看 `accounts[].state`：
   - `fullModeMessage`：平台推过来的群全量消息条数；
   - `atMessage` / `c2cMessage`：@ 机器人与单聊消息的条数。
2. 群里发一条**不带 @** 的消息，再调一次 `chatbot_status`：
   - `fullModeMessage` 不涨 → 平台没推，就是没开「接收所有消息」；
   - 涨了但没回 → 看 `chatbot_chat_info` 的 `counters.fullMode` 与本轮日志里的投递原因。
3. 插件上线 3 分钟后如果仍然没收到任何全量消息、而配置里有群聊绑定成 `always`，
   启动日志会主动给一条 `W` 级提示（只提示一次）。

开了「接收所有消息」以后，群里的 @ 也有变化：@ 人不再写成去掉前缀的正文，而是
写成 `<@成员openid>`（@ 全体是 `<@all>`）。插件会把它们换成 `@昵称` 再给模型，
并按 `mentions[]` / openid 判断「这条是不是 @ 了我」（详见 §2.4）。

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

### 2.4 被 @ 的判定（开了全量消息的群）

开了「接收所有消息」后，@ 机器人不再单独成一个事件，而是混在 `GROUP_MESSAGE_CREATE`
里。插件用两条线索判断这条是不是 @ 了自己：

1. `mentions[]` 里有没有 `bot: true` 的成员（群里通常只有本机器人一个）——不能提前
   知道机器人的群 openid 时靠它；
2. 正文里的 `<@成员openid>` 标记是否等于自己的 openid。自己的 openid 按顺序攒：
   - 网关 READY 的 `d.user`（`id` / `union_openid`）；
   - 账号配置的 `selfOpenid`（可写字符串或数组，手工钉住）；
   - 机器人自己消息的回声（`author.member_openid`，按群不同，看到一次就记住）。

```yaml
accounts:
  - id: qq_main
    type: qq
    appId: "${QQBOT_APPID}"
    selfOpenid: ["7A3B9C1D5E2F4A6B8C0D1E3F5A7B9C2D"]   # 可选
```

什么时候需要填 `selfOpenid`：开了全量消息后，@ 机器人的消息总是不立刻唤醒（只能等
聚合窗口）。把日志里正文标记的 openid（或 `chatbot_status` 的 `selfOpenid` 里已经攒到
的值）填进来即可。正文里的 `@` 标记一定会被换成 `@昵称` 再给模型，`<@all>` 记成
`@全体成员` 且不算「@ 了我」。

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

### 3.1 让它「这轮不说话」

模型的正文有两个去处：调 `chatbot_send_message` 才真正发出；没调用时由
`behavior.fallbackReply` 把**本轮最后一段正文**兜底发出去（`mention` = 被 @ 或 `always`
会话才兜底，`always` = 都兜底，`never` = 正文一律不发）。兜底是为了救「忘了调工具」的轮次，
代价是正文变成了隐式出口 —— 模型想表达「不说话」时写的备注（`（保持安静）`、
`（收到，这条不回复）`）会被当成回复发到群里。

所以插件给了一个明确的约定：**这轮不发言 = 不调工具 + 正文只有 `[静默]` 这一个标记**。
不用模型记，插件自己把这句话作为独立提示词段 `chatbot-silence` 追加进人设根（要改文案就在
`prompt.append` 里用同名键覆盖；`fallbackReply: never` 时不追加，因为那时正文本来就不发）。
插件识别（`agent/plugins/agentxx_chatbot/chatbot_core.cpp` 的 `isSilenceReply`）两种写法：

1. 整段只有一个标记词：`静默` / `保持安静` / `沉默` / `不回复` / `已读` / `略` /
   `silent` / `silence` / `no_reply` / `skip` / `quiet` ……（允许外面套一层括号，大小写、
   空白、标点不敏感，`[静默]` / `（SILENT）` / `NO_REPLY.` 都算）；
2. 整段就是一句**不超过 100 字**的括号备注，且备注里出现了「不发言」的说法
   （`（收到，这条不回复）`、`（这次真的不发了）`）—— 实测模型想沉默时最常这么写。

命中后插件什么都不发（兜底跳过、`chatbot_send_message` 也不发，工具结果里说明「静默标记，
没有发送」）。混合了正文的不算标记：`静默模式怎么关` 这种正常消息照常发。

## 4. 工具

| 工具 | 用途 |
|------|------|
| `chatbot_send_message` | **唯一的回复出口**。参数：`text`（正文）、`attachments`（本地路径或 http(s) 地址数组）、`replyTo`（引用本轮第 n 条）、`markdown`、`chat`（跨会话时必须显式给）。正文只有 `[静默]` 时不发送（见 §3.1） |
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

**让它别说话，它还是发了一条 `（保持安静）` / `（收到，这条不回复）`。** 这是兜底发送
（`behavior.fallbackReply`）把本轮正文当回复发了出去。插件已按 §3.1 的约定处理：正文只有
静默标记（`[静默]` 或一句短的括号备注，例如 `（这条不回复）`）时不发送。仍然出现时按顺序查：

1. `chatbot_status` 的 `fallback` 计数有没有涨 —— 涨了说明是兜底发的，看正文是不是长备注
   （超过 100 字不算标记，因为那更可能是真要说的话）；
2. 人设在自己 `prompt.append` 里用了 `chatbot-silence` 这个键，把插件自动追加的「不发言只写
   `[静默]`」那段顶掉了（键相同以用户的为准）；
3. 模型这轮其实调了 `chatbot_send_message`（工具结果里没有「静默标记」字样），那是模型主动发的，
   从提示词/人设上让它少说话。

**回复发不出去，日志里有「被动回复次数用尽」。** 同一条消息平台允许的被动回复次数有限，
用尽后按 `overflowPolicy` 处理（默认降级为主动消息；`truncate` 放弃、`fail` 报给模型）。

**凭证泄露。** 用 `appIdFile` / `appSecretFile` 指向 `.env` 文件，并把那个文件加进 `.gitignore`。
日志里不会打印凭证取值。

**群里发文件给它看，它说看不到。** 群文件的附件挂在**被引用那条消息**的
`msg_elements[].attachments` 里（不是顶层 `attachments`），插件已按这个结构解析。
落盘后小文本文件（默认 ≤ 64 KiB，`behavior.attachments.inlineTextMaxBytes`）的正文会
**直接内联进输入**，所以人设根没挂文件系统插件也能读；更大的或二进制文件只在输入里给路径，
要靠人设根挂了 `agentxx_filesystem` 才能读。

**主动发文件失败，提示 `upload_part_finish 失败 (status 500)`。** 这是富媒体上传协议的问题，
不是权限或频控。QQ 的 `upload_part_finish` 要求四项：`upload_id`、`part_index`、
`block_size`（该片**实际字节数**，字符串）、`md5`（该片内容的 md5）；另外 `file_size` /
`block_size` 是**字符串**数字（平台按字符串下发，也要求按字符串上报）。插件已按文档逐项给齐，
对应的回归是用例 `chatbot_media`（请求体与分片切法）和 `chatbot_media_io`（本地模拟服务端
跑完整流程，模拟服务端会像真机一样对缺字段回 500）。

**想在不连平台的情况下试一遍。** 把账号 `type` 改成 `fake`、打开 `behavior.debugTools`，
然后用工具 `chatbot_debug_inject` 注入消息、`chatbot_debug_outbox` 看回复。

## 6. 扩展新平台

`platform/platform.h` 的 `IPlatform` 是唯一需要实现的东西（`start` / `stop` / `send` /
`caps` / `statusJson`），把平台事件归一化成 `chatbot_types.h` 的 `InboundEvent`、把出站收成
`OutboundMessage` 即可。平台差异写成 `PlatformCaps` 的数据（markdown / 被动回复 / 引用 / 单条上限），
不在业务代码里写 if。

当前实现：`platform/qq/`（QQ），`platform/fake/`（内存假平台，测试与本地演示）。
