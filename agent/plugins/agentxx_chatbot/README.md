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

网络（HTTP 接口 + 网关 WebSocket）不自己建连接：一律经主程序的网络表（`agentxx.net`，
插件侧入口见 `chatbot_net.h`），连接、TLS、超时与取消都由主程序统一管，插件只跑协议状态机
（token 缓存 / Hello / Identify / 心跳 / Resume / 重连退避）。

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
                  # id 是**平台侧的会话 id**, 不是群号也不是 QQ 号 (见 §2.2):
                  #   群聊 = group_openid, 私聊 = 对方的 user_openid, 都是 32 位十六进制串
                  - { type: group, id: "3F2A1B9C8D7E4F5A6B0C1D2E3F4A5B6C", mode: mention }
                  - { type: private, id: "8C0D1E3F5A7B9C2D4E6F8A0B1C3D5E7F", mode: always }
                  - { type: group, id: "*", mode: mention }   # 通配: 该账号下所有群

        # ---------- 第三段: 通用行为 ----------
        behavior:
          aggregateWindowMs: 800        # always 模式的静默窗口 (毫秒)
          sendIntervalMs: 350           # 一轮内多条回复之间的间隔
          sendResultWaitMs: 4000        # chatbot_send_message 等平台发送结果的上限 (0 = 不等, 直接回"已排队")
          messageSplitLimit: 1500       # 单条文本上限 (UTF-8 码点; 0 = 不切分)
          maxSplitParts: 8              # 单条最多切成几段
          maxRepliesPerTurn: 5          # 一轮最多发几条
          maxChatsPerPersona: 64        # 每人设活跃会话上限 (超了回收最久没动的)
          fallbackReply: mention        # 模型没调工具时: mention (被 @ 或 always 会话) | always | never; 正文只有 [静默] 时不发 (见 §3.1)
          overflowPolicy: active        # 被动回复次数用尽 / 窗口过期后: active(改发主动消息, 群里会 @ 原说话人) | truncate | fail
          humanInLoop: auto-allow       # 群里的权限询问弹不出来: auto-allow | auto-deny
          timezone: auto                # 消息时间的显示时区: auto | system | +08:00 | Asia/Shanghai (见 §3)
          injectContextHint: true       # 每轮输入开头带「你在哪个群」
          attachments:                  # 附件策略 (注意这层嵌套: 平铺写在 behavior 下不生效)
            fetch: auto                 # auto | url | download
            maxPerMessage: 5            # 单条消息最多处理几个附件 (主程序上限 5)
            inlineTextMaxBytes: 65536   # 小文本文件正文直接内联进输入 (0 = 关闭)
          exposeToolsOnHost: false      # 是否把 chatbot_* 工具也注册到内置主会话
          debugTools: false             # 是否注册 chatbot_debug_* 调试工具
```

配置只有三段：`accounts`（账号）/ `personas`（人设）/ `behavior`（通用行为）。
关键词命令**不在配置里**，由插件内置（见 §2.4）；
启动时只在日志里提一句「命令由插件内置」。

### 2.1 三种触发模式（`bind.chats[].mode`）

| mode | 行为 |
|------|------|
| `mention` | 只有被 @ 的消息唤醒一轮；期间未被 @ 的消息**仍进缓冲**，跟着下一次被 @ 一起带上去 |
| `always` | 任何新消息都投递；多条消息按 `aggregateWindowMs` 静默窗口合并成一次输入 |
| `off` | 该会话完全不处理（也可以运行期用 `chatbot_set_mode` 切换） |

私聊里 `mention` 按 `always` 处理（单聊消息本来就是直接发给机器人的）。

#### 一轮还在跑时来的新消息：插进这一轮（`next-step`）

一轮跑得久（工具链长、模型慢）时，群里可能又来了消息。插件的投递规则是：

| 状态 | 投递方式 | 效果 |
|------|----------|------|
| 会话空闲 | `next-turn` | 排队成新的一轮 |
| 会话正在跑一轮 | `next-step` | 插进**当前这一轮**，主程序在下一个 modelcall 边界写进上下文 |

也就是说模型不必等这一轮跑完才知道群里又说了什么：它下一次请求里就能看到这段新消息，
输入块里另有一句「这一轮还在执行中, 下面这段是中途插进来的新消息」。
`[#n]` 编号接着前面那段往下排（`replyTo` 用同一套编号），`maxRepliesPerTurn`
额度也仍按「一轮」算，不会因为插进来就重置。
插失败（运行时拒绝）时这批消息会回到聚合缓冲，等本轮 `TURN_END` 再投，内容不丢；
`chatbot_chat_info` 的 `counters.injected` 是「中途插入」的条数。

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
并按 `mentions[]` / openid 判断「这条是不是 @ 了我」（详见 §2.5）。

### 2.2 会话 id 是什么（`bind.chats[].id`）

`id` 是**平台侧的会话 id**，不是群号、也不是 QQ 号：

| 会话 | `id` 取值 | 形如 |
|------|-----------|------|
| 群聊（`type: group`） | 事件里的 `group_openid` | `3F2A1B9C8D7E4F5A6B0C1D2E3F4A5B6C`（一串十六进制） |
| 私聊（`type: private`） | 对方在这个机器人下的 `user_openid` | 同上，每个用户一个 |
| 通配 | `"*"` | 该账号下同 `type` 的全部会话 |

两点最容易踩：

- **群号 / QQ 号填进来一定匹配不上**：平台推送里没有这两个数字，插件能拿到的只有 openid；
- **侧边栏每行标题的 `群聊(尾号 xxxxxxxx)` 只是显示名**（群名拿不到时用 id 末 8 位凑的），
  不是 id；紧挨着的下一行 `id …` 才是要抄的值。

拿 id 的办法（任选一种）：

1. **看侧边栏**（最快）：「机器人」栏里每个会话下面是三行 —— 标题、`人设 · 群聊/私聊 · 模式`、
   `id <会话 id>`。那一行就是 `bind.chats[].id`，直接拖选复制（收过消息的会话都会列出来，
   包括还没匹配到 bind 的）。窄栏放不下时长 id 会折成两行显示，仍可整串选中。
2. **先通配、再收窄**：先配上 `- { type: group, id: "*", mode: mention }`，
   在那个群 @ 机器人发一条 `/agentxx-cmd info`，它会本地回一句
   `会话 3F2A1B9C… (group) · 账号 qq_main …` —— 那个 `chat` 就是要填的 `id`。
   私聊同理（`type: private`，`info` 里的 `chat` 就是对方的 `user_openid`）。
3. **看日志**：某个会话第一次收到消息却没有匹配的 `bind` 时，插件会打一条 info：
   `会话 'qq_main|group|3F2A…' 没有匹配的 bind, 消息已丢弃; 要接它就在 personas[].bind.chats
   里加一项: type=group, id="3F2A…"` —— 直接抄这行。之后同一会话降为 debug，不会刷屏。
4. **会话已经能路由**时问模型 `chatbot_chat_info`：返回的 `chat` 是 chatId，`chatKey` 是
   `账号|类型|chatId` 整串。

openid 是平台按机器人（`appId`）分配的，换了 `appId` 之后这些绑定通常要重新取一遍。
只想临时改某会话的行为时不必动配置：用 `chatbot_set_mode` 改（不写回 yaml）。

### 2.3 凭证

`appId` 是长数字，直接写进 yaml 容易被 YAML 解析成整数甚至浮点（精度丢失）。三种写法任选：

- `appId: 123456789`（纯数字没问题，但太长时请用下面两种）；
- `appId: "${QQBOT_APPID}"`（环境变量展开）；
- `appIdFile: resource/keys/qq_bot.env`（从 `.env` 形态的文件读 `QQBOT_APPID`）。

`appSecretFile` 同理，键名是 `QQBOT_APPSECR(S)ET`（两种拼写都接受，平台文档里两种都出现过）。

### 2.4 内置命令

命令写在 `chatbot_core.cpp` 的 `builtinCommands()` 里，统一格式 `/agentxx-cmd <子命令>`
（单独发 `/agentxx-cmd` 等同于 `help`；大小写不敏感，子命令后面可以跟一段文字，作为 `{args}` 用）：

| 命令 | 行为 |
|------|------|
| `/agentxx-cmd` / `/agentxx-cmd help` | 列出全部命令（`{commandList}` 由同一张表拼出来） |
| `/agentxx-cmd info` | 当前会话的人设 / 模式 / 通道 / 计数 / 启动时间 / 当前时间与时区，排查「为什么不回复」先看它 |
| `/agentxx-cmd hello` | 让模型向发言人打个招呼（`model` 模式：走一轮模型） |
| `/agentxx-cmd stop` | 打断当前这一轮（`action: cancel`；不回复任何文本） |
| `/agentxx-cmd task <submit\|list\|status\|cancel> [参数]` | 后台任务：`task submit 把仓库巡一遍` 提交；`task list` 列本会话的任务；`task status [id]` 看进度；`task cancel [id\|all]` 取消（不给 id = 取消本会话全部）。结果跑完由插件自己发回群里 |

未知子命令由根命令接住回帮助（不会当成聊天内容投给模型）。

**命令是插件自己的交互约定**，所以没有做配置项：每个部署各写一套名字与文案，
文档、用例、排错说法就对不上了。要改命令请改 `builtinCommands()`（改完补
`chatbot_core` 的 `builtinCommands` 用例）。

### 2.5 被 @ 的判定（开了全量消息的群）

开了「接收所有消息」后，@ 机器人不再单独成一个事件，而是混在 `GROUP_MESSAGE_CREATE`
里。插件用下面三条线索判断这条是不是 @ 了自己：

1. 正文里的 `<@成员openid>` 标记是否等于**已知的**自己的 openid；
2. `mentions[]` 里有没有条目的 openid 等于自己的 openid；
3. 都没有时，看 `mentions[]` 里有没有条目的昵称等于**机器人昵称**（READY 的
   `d.user.username`）—— 自身 openid 还没学到时的兜底，命中后顺手把这个群的 openid
   学下来，之后就走 1、2 精确判定。

**不把 `mentions[]` 里的 `bot: true` 当依据**：群里往往还有别的机器人（元宝之类），
@ 它的时候平台同样报 `bot: true`。早先拿它当依据，结果是群里只要 @ 任何机器人，
插件都以为是 @ 自己，模型抢着回。现在只有上面 1~3 任一命中才算，认不出来就当没 @
（宁可漏一次唤醒，也不要对着别人说话）。

```yaml
accounts:
  - id: qq_main
    type: qq
    appId: "${QQBOT_APPID}"
    selfOpenid: ["7A3B9C1D5E2F4A6B8C0D1E3F5A7B9C2D"]   # 可选
```

什么时候需要填 `selfOpenid`：机器人昵称也拿不到时（例如 Resume 重连没有 `d.user`、
机器人改过昵称），全量消息里的 @ 判定就只能靠它。查当前状况看两处：

- `chatbot_status` 的 `botName` / `selfOpenid` / `selfOpenidByGroup`：自己是谁、已经学到哪些群；
- 同一个 JSON 的 `mention`：`byEventType` / `byOpenid` 是硬证据，`byNickname` 是兜底
  （一直涨说明还没学到 openid），`selfUnknown` 一直涨说明根本认不出自己 —— 就要填
  `selfOpenid`。

正文里的 `@` 标记一定会被换成 `@昵称` 再给模型（认不出昵称的写成 `@其他成员`，
认得出是自己但没昵称的写成 `@你`），`<@all>` 记成 `@全体成员` 且**不算**「@ 了我」
（否则天天被 @全体成员 叫醒）。

## 3. 模型看到的输入

每轮输入是一段结构化文本，首行是「你在哪个群」，之后每条消息一行：

```
QQ群「技术交流群」(群 id 尾号 6789); 下面是最近收到的消息：
当前时间 2026-10-29 12:05 (自动 UTC+08:00); 每条消息前的 [MM-DD HH:MM] 是平台给的发送时间, 时区取自平台时间戳
本轮唤醒原因: 有人 @ 了你 ([#2]), 这一轮是给你看的; 其余 1 条是同一时间段的上下文
[#1] 小张(member): [10-29 12:03] 这个报错怎么解
[#2] 小李(member) @机器人: [10-29 12:04] (回复 小张: 这个报错怎么解) 我猜是权限问题
[#3] 小吴(member) @别人(小郑): [10-29 12:05] @小郑 你也看下
[#4] 小许(member) @别的机器人(元宝): [10-29 12:05] @元宝 帮写个正则
```

- `[#n]` 是本轮编号，`chatbot_send_message` 的 `replyTo` 就填它（引用第 n 条）；
- 发送者后面的「被 @ 的人」只有三种写法：`@机器人`（这条确定在叫你）、
  `@别人(名字)` / `@别的机器人(名字)`（@ 的是群里的其他人，不是在叫你）；
- 「本轮唤醒原因」说明这轮为什么开始：有人 @ 了你、`always` 模式的静默窗口到期、
  缓冲攒满、通道刚就绪…… 不是被 @ 叫起来的轮次，模型不必抢着回；
- 会话正在跑一轮时插进来的那段（§2.1）会多一句「这一轮还在执行中, 下面这段是中途插进来的
  新消息」，编号接着前面那段往下排（`replyTo` 用同一套编号）；
- `[MM-DD HH:MM]` 是平台给的**消息发送时间**；紧随其后的是「当前时间」一行，
  它带时区说明，模型据此判断“现在”与“消息多久之前”；
- 时间点来自平台时间戳自带的时区（`2026-10-29T12:03:09+08:00` 就是东八区 12:03），
  服务器本身跑在哪个时区都不影响显示；
- 图片 / 音频 / 视频是否随输入投给模型，看模型条目声明的多模态能力
  （`image_input` / `audio_input` / `video_input`）：没声明的类型只把文件名/大小/本地路径写在输入里
  （正文另有一行「[附件未传给模型] N 个图片 (模型配置 image_input = false)…」说明原因），
  模型据此自己决定用工具处理还是问对方；普通文件只给本地路径（模型可以自己用文件工具读）。
  这样做的原因：没声明的模型收到 `image_url`/`input_audio`/`video_url` 会被上游整轮拒绝
  （HTTP 400，表现为“发了附件之后机器人再也不出声”），而且 base64 附件会永久留在会话上下文里，
  之后每轮都重发一次。

这几条标记的读法同时写在人设提示词的 `chatbot-input-guide` 段里（`prompt.append` 里
用同名键可覆盖）—— 光靠模型自己从正文猜“这条是不是在叫我”会误判，尤其是群里
@ 别人、@ 另一个机器人、@全体成员 这三种情况。

### 3.0.1 同一条消息两份事件（去重改为合并）

全量模式下「@ 了我」的消息可能按 `GROUP_MESSAGE_CREATE` 与 `GROUP_AT_MESSAGE_CREATE`
各推一次，两份携带的信息不一样（一份有正文/附件，另一份一定带 @ 语义）。按 `msgId`
去重时**不再直接丢弃后到的那份**：能补的地方（@ 判定、被 @ 的人、正文、引用、附件、
时间）会合进缓冲里那条；如果因此从「没 @ 我」变成「@ 了我」，立刻唤醒 —— 否则会出现
「被 @ 了却不回」。`chatbot_status` 的 `counters.duplicated` 现在是合并（而不是丢弃）的条数。

### 3.0 时间与时区（`behavior.timezone`）

时间分两层，分开处理：

| 层 | 做什么 | 出错会怎样 |
|----|--------|------------|
| **时间点** | 按平台时间戳自带的时区后缀算出 epoch | 当 UTC 处理会让整段时间偏一个时区（模型据此谈时间、算“几点了”就错了） |
| **显示时区** | 把时间点转成给人看的钟点 | 用进程 `TZ` 而不是用户所在地区时，群里显示的钟点与手机上的对不上 |

`behavior.timezone` 决定第二层：

| 取值 | 含义 |
|------|------|
| `auto`（默认） | 用平台消息时间戳自带的偏移；平台没给时退回系统时区 |
| `system` / `local` | 系统时区（跟随运行环境，夏令时按该时间点算） |
| `+08:00` / `-05:00` / `+0800` / `UTC+8` / `8` | 固定偏移 |
| `Asia/Shanghai` | IANA 时区名（运行平台没有时区库时退回系统时区，启动日志会提示） |

写错（例如 `+25:00`）会在启动时直接报配置错误，不会运行期静默按系统时区。
`chatbot_status` 的 `timezone` / `configTimezone` / `now` 三项可以确认当前实际用的是哪个时区。

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
| `chatbot_send_message` | **唯一的回复出口**。参数：`text`（正文）、`attachments`（本地路径或 http(s) 地址数组）、`replyTo`（引用本轮第 n 条）、`markdown`、`chat`（跨会话时必须显式给）。正文只有 `[静默]` 时不发送（见 §3.1）。返回里给出真实发送结果（`sent` / `messageId`），被动回复过期改发 @ 时另带 `warning`（见 §4.2） |
| `chatbot_history` | 取该会话最近 N 条原始消息（含被 mode 过滤掉的），每条带消息时间（时区同 §3.0） |
| `chatbot_chat_info` | 会话状态：路由到哪个人设、当前模式、通道 id、计数（含 `injected` 中途插入条数）、缓冲条数 |
| `chatbot_status` | 插件全局状态：账号连接、人设、出站队列、计数（`outbound.passiveFallback` = 被动过期改发 @ 的条数） |
| `chatbot_set_mode` | 运行期改触发模式（不写回配置） |
| `chatbot_recall_message` | 撤回自己刚发出的消息（平台时限内，QQ 为 2 分钟）。参数：`target`（`-1` 最后一条、`-2` 倒数第二条…）、`skip` + `count`（跳过最近几条再连撤几条）、`messageId`、`chat` |
| `chatbot_task` | 后台任务（`op` = `submit` / `status` / `cancel` / `list`）。`submit` 把要跑很久的事交给后台 agent（它看不到本群聊天记录，所以 `prompt` 要写完整），提交后模型可以继续说话；跑完结果作为一条新输入回到这个会话，由人设组织语言回复 |
| `chatbot_debug_inject` / `chatbot_debug_outbox` / `chatbot_debug_platform` | 仅 `behavior.debugTools: true` 时注册：直接注入假消息 / 回看假平台收到了什么 / 拨假平台开关（`op=expire_passive` 让被动回复按 `msg_id` 过期失败） |

工具只注册在**人设根**上。内置主会话默认看不到它们（`exposeToolsOnHost: true` 才会注册）。

### 4.1 后台任务（`chatbot_task`）

长活（巡仓库、查资料、算很久）交给主程序的后台任务机制跑，不在聊天通道里阻塞：

- **并发与排队在主程序**：每个聊天人设一条队列，额度用主配置的 `async_work.max_concurrent`
  （插件提交时带 `queueKey = chatbot:<人设名>`）；插件侧再加一道每会话上限
  （同时进行 4 条，超出直接回给模型「先等它们出结果」）；
- **提交快照**：工具策略取人设白名单（没配就继承 `["*"]`），插件挂载取人设现有清单
  并**把自己排除**（`exclude`）—— 后台任务没有聊天会话，`chatbot_*` 工具在那里定位不到
  平台对象；中断策略固定 `allow`（群里不弹权限窗）；
- **结果怎么回来**：任务跑完，插件在任务事件里把结果写成一条「系统提示」输入投回
  聊天会话（`next-turn`），由人设决定要不要回给用户；通道已经回收/从未建立时直接发一条
  消息到群里（被动→主动降级）。用户自己取消的任务不再发消息；
- **可见**：TUI 侧边栏「机器人」栏多一段「后台任务」（`async-N 1m20s 名字`，最多 6 条），
  与 Info 面板的全局「后台任务」段不是同一份数据（那边是主程序台账）；
- **看不到能力时**：老主程序没有 `submit_async_work`，`chatbot_task` 会直接告诉模型
  「不支持，请在本轮直接做完」。
- **它手上有什么工具**：人设的 `tools` 白名单（或 `["*"]` 继承）+ 人设 `plugin.mount`
  列出的插件（不含本插件）。人设只用默认的 `mount: none` 时，后台任务只有内置工具
  （读写文件/执行命令都属于插件工具）—— 要让后台任务能干活，把人设的
  `plugin.mount` 配上 `agentxx_filesystem` / `agentxx_execute_command` 等。
- **开关**：主配置的 `async_work.enable` 只管"模型侧的委派工具要不要异步"；插件主动
  提交不受它影响（插件自己就是决定者）。额度 `async_work.max_concurrent` 照旧生效。

### 4.2 发送结果与「被动回复过期」的降级

`chatbot_send_message` 不再只是「已排队」：

- 插件在后台把消息交给平台，工具在**工作线程**上等这趟结果（上限
  `behavior.sendResultWaitMs`，默认 4000 ms，0 = 不等）；
- 拿到成功 → 返回里带 `sent` 与平台 `messageId`；拿到失败 → 按工具失败上报原因
  （模型能知道这句没出去）；等不到 → 仍旧说「已排队」，不拖住工具。

关键的一条：**模型跑完长任务再回复时，引用的 `msg_id` 可能已经过了平台的被动回复窗口**
（群 5 分钟 / 单聊 60 分钟）。平台会拒绝这条被动回复，以前只记一条失败日志 —— 消息丢掉，
模型还以为发出去了。现在的处理：

| 平台返回 | 处理 |
|---|---|
| 被动回复窗口过期 / `msg_id` 无效 | 按 `overflowPolicy`：`active`（默认）**改发主动消息**并 **@ 原说话人**；`truncate` 放弃；`fail` 按失败上报 |
| 其它失败 | 照旧计 `failed`，工具拿到失败原因 |

改发时群消息会变成 markdown（QQ 群里「@用户」只在 markdown 消息里生效，写法
`<qqbot-at-user id="member_openid" />`，id 取入站事件里那个人的 `member_openid`）；
拿不到 openid 时退化成正文前缀 `@昵称 `，单聊不 @（本来就是对着他说）。
工具返回里会带上警告，模型下次就知道该说什么：

```json
{"queued":1,"parts":1,"chat":"qq_main|group|…","sent":1,"messageId":"…",
 "warning":"引用的 msg_id 已过期 (平台被动回复窗口已过), 已改为主动消息重发并 @ 了小张"}
```

降级次数看 `chatbot_status` 的 `outbound.passiveFallback`。

## 5. 常见问题

**群里完全不回复。** 按顺序查：

1. `chatbot_status` 里账号状态是不是 `online`（`connecting` / `reconnecting` 说明网关没连上）；
2. 该群有没有在某个 `personas[].bind` 里（`chatbot_chat_info` 的 `persona` 为空就是没路由到）；
3. 模式是不是 `mention` 而这条消息没 @ 机器人；或者被 `chatbot_set_mode` 改成了 `off`；
4. 模型有没有调 `chatbot_send_message`（`fallbackReply: never` 时不调就真的不说话）。

**不知道 `bind.chats[].id` 填什么 / 填了不生效。** `id` 是平台侧的会话 id（群聊
`group_openid`、私聊 `user_openid`），**不是群号也不是 QQ 号**。最快的拿法是看侧边栏：
「机器人」栏每个会话下面有一行 `id …`（标题里的 `群聊(尾号 xxxxxxxx)` 只是显示名，别抄）。
其他办法见 §2.2：配 `- { type: group, id: "*", mode: mention }` 通配 → 在目标群发
`/agentxx-cmd info` 抄 `chat`；或看日志里「没有匹配的 bind … 要接它就加一项」那条 info。

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

**它说的“现在几点” / 消息时间跟你手机上对不上。** 看 `chatbot_status` 的 `timezone` 与
`configTimezone`：前者是实际生效的时区，后者是配置原文。默认 `auto` 跟平台时间戳走
（群里消息几点就是几点）；服务器跑在与用户不同时区、或者平台只给不带时区的时间时，
直接固定下来更可靠：

```yaml
behavior:
  timezone: "+08:00"      # 固定东八区; 也可以用 Asia/Shanghai
```

以前这里有个 bug：平台时间戳的 `+08:00` 被当成 UTC 解析，再按本地时区显示，
消息时间整体偏了一个时区。现在时间戳按自带时区解析，显示时区由 `behavior.timezone` 定。

**回复发不出去，日志里有「被动回复次数用尽」。**** 同一条消息平台允许的被动回复次数有限，
用尽后按 `overflowPolicy` 处理（默认降级为主动消息；`truncate` 放弃、`fail` 报给模型）。

**日志里出现「被动回复窗口已过期, 改发主动消息 (@某某)」。** 这是正常降级：模型跑得久
（工具链长、后台任务刚回来），那条消息的被动回复窗口已经过了，平台不认原来的 `msg_id`。
插件把同一条回复改成主动消息重发，并把原说话人 @ 回来；工具返回里带 `warning`，
`chatbot_status` 的 `outbound.passiveFallback` 计数会涨（详见 §4.2）。真要彻底避开：

- 让模型别攒太久再回（长活走 `chatbot_task`，跑完尽快回一句）；
- 群主给机器人打开「允许主动发送消息」（§2.1），否则降级后的主动消息会被平台拒；
- 主动消息有每日条数上限（群每群每天 1000 条），别把降级当常规通道。

**凭证泄露。** 用 `appIdFile` / `appSecretFile` 指向 `.env` 文件，并把那个文件加进 `.gitignore`。
日志里不会打印凭证取值。

**群里发文件给它看，它说看不到。** 群文件的附件挂在**被引用那条消息**的
`msg_elements[].attachments` 里（不是顶层 `attachments`），插件已按这个结构解析。
落盘后小文本文件（默认 ≤ 64 KiB，`behavior.attachments.inlineTextMaxBytes`）的正文会
**直接内联进输入**，所以人设根没挂文件系统插件也能读；更大的或二进制文件只在输入里给路径，
要靠人设根挂了 `agentxx_filesystem` 才能读。

**发了附件之后机器人不再出声 / 日志里是模型请求 400。** 图片/音频/视频曾经被无条件当作多模态
附件投给模型，而模型不支持的类型会让上游整轮报错，base64 附件还会永久留在会话上下文里、
每轮重发一次。现在只有模型条目声明了对应能力（`image_input` / `audio_input` / `video_input`）
才投，否则只给文件名/大小/路径，并补一行「[附件未传给模型]」。另一种情境是附件超了主程序的
单文件上限（图片 10MB / 音频 25MB / 视频 50MB），那会让**整条输入**（含正文）被拒；插件现在会自动去掉
附件重投一次（日志里记一条「带附件的输入被拒…已去掉附件重投」），正文不会丢。

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
