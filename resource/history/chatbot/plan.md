# `agentxx_chatbot`：把人设接到聊天平台（先做 QQ）

- 类型：设计（待实现）
- 时间：2026-10-27
- 状态：方案待确认
- 相关：[plugin-event/index.md](../plugin-event/index.md)（会话运行时 `agentxx.agent.runtime`）、
  [plugins.md](../../../docs/zh-cn/design/plugins.md) §19、[example_runtime](../../../agent/plugins/example_runtime/example_runtime.cpp)

## 文档索引

| 章节 | 内容 |
|---|---|
| §1 | 目标、必须支持的要点、不做什么 |
| §2 | 复用现状（运行时表 / 工具库）与「不改主程序」的边界 |
| §3 | QQ 接入事实：凭证 / 网关 / 事件 / 发消息 / 频控 / 富媒体 / 平台侧开关 |
| §4 | 总体架构：分层、线程模型、死锁分析、目录结构 |
| §5 | 配置：四段 yaml、环境变量陷阱、独立配置文件、启动校验 |
| §6 | 路由与会话映射：`ChatKey`、通道懒创建与 LRU、预算 |
| §7 | 入站流水线：触发模式、聚合、输入组装、去重 |
| §8 | 让模型自己决定怎么回：工具、编号引用、人设 → 根配置、默认提示词 |
| §9 | 关键词命令 |
| §10 | 出站：切分、多条、频控、被动→主动降级、引用 |
| §11 | 富媒体收发 |
| §12 | 可靠性与可观测（重连、计数、隐私） |
| §13 | 跨平台预留（`IPlatform` / 能力表 / 接微信要补什么） |
| §14 | 模块职责与关键实现约束 |
| §15 | 五个实施阶段 |
| §16 | 测试方案（新增模块 + 24 条用例 + 手工验收） |
| §17 | 待确认问题与校正方式 |
| §18 | 风险与取舍 |

---

## 1. 目标与范围

### 1.1 一句话

**一个插件** `agentxx_chatbot`：把聊天平台（先只做 QQ）的群聊与私聊消息投给 agent 会话，
再把 agent 的回复发回平台。每个「平台账号 + 群或私聊」独立一个会话；每个 root 是一个
**人设**，人设可以指定自己对接哪些「平台账号 + 会话」。

### 1.2 必须支持的要点

> 要求 11 不是新增需求，而是把「每个人设自己声明对接范围」与「逐条回 / 合并回」两件事
> 单独列出来，因为它们是本设计里最容易被实现成“写死”的两处。

| # | 要求 | 落点 |
|---|---|---|
| 1 | 只做一个插件，先只做 QQ，结构上预留其它平台 | §4 平台适配层、§13 |
| 2 | 由 `agentxx-config.yaml` 的 `plugin.list` 加载，`appId` / `appSecret` 写在 yaml 里并引用环境变量 | §5 |
| 3 | 先声明「平台 + 账号」列表，再给每个人设声明对接哪些账号与会话（仿 `model.list` / `model.use`） | §5.2、§6 |
| 4 | 每个群 / 私聊 id 独立一个会话 | §6.2 |
| 5 | 可配「被 @ 才回」与「新消息就回」两种触发；一轮耗时期间积累的多条新消息**在下一轮全部带上** | §7 |
| 6 | 不写死回复行为：回不回、引用哪条、一轮回几条，交给模型决定 | §8 |
| 7 | 关键词触发（`/axx-info`、`/axx-hello` …） | §9 |
| 8 | 单条消息有长度上限，超长**自动切成多条**发送 | §10.2 |
| 9 | 图片与文件附件的接收与发送 | §11 |
| 10 | 与已有框架零冲突：不改主程序代码、不改 `agentxx.agent.runtime` 表 | §2.3 |
| 11 | 人设可指定对接哪些账号与会话；一轮里多条消息可以逐条回、也可合并回 | §5.1、§7.3、§8.4 |

### 1.3 明确不做

- 不做微信 / 其它平台的具体实现（只留接口与接入说明）
- 不做 QQ 频道（Guild）、频道私信、卡片/按钮键盘、表情表态、语音合成（TTS）
- 不做「每群一个 root」（成本线性增长；人设用 root，群用会话来隔离）
- 不做多进程部署、不做跨设备会话同步（已有 WS 端点，与本插件无关）
- 不改 `BaseAgent` 的 ReAct 循环、不改插件框架

---

## 2. 复用现状与差距

### 2.1 主程序已经提供的能力（直接使用，不改）

上一轮已完成的会话运行时表 `agentxx.agent.runtime`（见 plugins.md §19）正好覆盖本插件需要的
全部主程序侧动作：

| 需要做的事 | 已有入口（SDK 便捷层 `plugin_kit.h`） |
|---|---|
| 建一个人设（独立提示词 / 工具白名单 / 插件挂载 / 模型） | `mountRoot(ctx, MountRootOptions)`，只能在 `start` 调 |
| 每个群 / 私聊一个会话 | `openChannel(ctx, ChannelOptions, onEvent)` |
| 往会话投一条用户消息（含附件） | `sendInput` / `sendText`（输入 JSON 与 FFI 同构） |
| 拿 agent 的输出 | 通道事件 `TURN_END` / `DELTA_TEXT` / `TOOL_END` |
| 中途打断一轮 | `cancelChannel` |
| 让模型主动发消息 | 在 root 上注册工具（`fast_tool` / `blocking_tool` / `polled_tool`） |
| HIL（权限询问）交给人 | 通道事件 `INTERRUPT` + `respondInterrupt` |

`plugin_kit.h` 里的 `PluginBaseT<AgentIfaces>` 还直接给了 `argsJson()` / `configPath()` /
`log` / `offload` / `bridge()`，不必自己写跨边界代码。

### 2.2 工具库

| 需要 | 用哪个 |
|---|---|
| HTTP（openapi、上传、下载） | `utilxx::HttpClient::requestAsync / getAsync / postAsync`（协程） |
| WebSocket（网关长连接） | `utilxx::wsConnect` + `utilxx::WsClient`（`sendText` / `recv` / `abort`） |
| JSON | `utilxx_base::Json`（`nlohmann` 兼容接口） |
| base64 | `utilxx_base::base64Encode / base64Decode` |
| MD5 / SHA1（分片上传要校验值） | `utilxx::md5Hex`（`utilxx/crypto.h`，返回小写 hex）或直接 OpenSSL EVP（`cxx_utilxx` 已依赖 OpenSSL，插件可直接链） |
| 路径编码（Windows 中文路径） | `utilxx_base::pathToUtf8Generic / utf8ToPath` |
| CPU 中等活（切分长文本、算散列） | `offload`（主程序工作线程池） |

### 2.3 与主程序的边界：本设计不加表、不改表

盘点后确认**不需要**新增接口表成员。下面几处「差一点」的地方，一律在插件内部消化：

| 缺口 | 影响 | 处理 |
|---|---|---|
| `send_input` 只有同步版（无「发后不等」） | 平台 io 线程会阻塞到主程序 io 线程受理 | 受理是入队，耗时微秒级；且平台 io 线程不与主程序 io 线程互相等待（§4.3 死锁分析） |
| 一次输入最多 5 个附件（`kMaxAttachmentsPerMessage`） | 聚合多条消息时附件可能超过 5 个 | 只把前 5 个作为附件，其余降级成文本里的本地路径（§11.2） |
| 不支持会话级提示词 | 同一个群里多个人说话只能共享一套提示词 | 人设 = root；群内差异靠每轮输入里的元信息段（§7.4），不需要会话级提示词 |
| 没有「原始消息流水」查询 | 模型想回看更早的聊天记录 | 框架的 `get_context_messages` 给的是 **LLM 上下文**（含 system/tool 消息、可能已被压缩），不是群里的原话。插件自己维护每会话最近 N 条原始消息（含被 `mode` 过滤掉的），经工具提供（§8.3） |
| 每根最多 64 通道、进程最多 8 根（含内置根） | 人设 ≤ 7 个，每人设 ≤ 64 个活跃群 | §6.3：通道懒创建 + LRU 回收（会话落库，重开会话上下文仍在） |

> 为什么通道可以放心回收：会话（含 LLM 上下文）落在 `{data_dir}/sqlite/sessions/{sessionId}/`，
> 关通道只是停驱动；下次对同一 `sessionId` 开通道会从会话库恢复上下文。

---

## 3. QQ 平台接入事实（已按官方文档核实）

> 来源：`bot.q.qq.com/wiki/develop/api-v2/*`（2026-10 抓取）。
> 扫码绑定 SDK `@tencent-connect/qqbot-connector` 只负责「在控制台出二维码 → 扫码换回
> `appId` / `appSecret`」，**不是消息协议**；消息协议由下面这些 HTTP 与 WebSocket 接口组成。
> 因此本项目不需要 Node 运行时，也不需要那个 SDK。

### 3.1 凭证与鉴权

```
POST https://api.bot.qq.com/app/getAppAccessToken
     {"appId": "...", "clientSecret": "..."}
  →  {"access_token": "...", "expires_in": "7200"}          # 字符串数字
```

- 之后所有 openapi 带请求头 `Authorization: QQBot <access_token>`；
- token 有效期 7200 秒，**不会自动续**；在到期前 60 秒内再请求会拿到新 token（旧的在 60 秒内仍有效）；
- 老式的 `Bot <appid>.<token>` 签名鉴权已废弃，不用。

### 3.2 网关（WebSocket）

```
GET  https://api.bot.qq.com/gateway/bot      → {url, shards, session_start_limit{total,remaining,reset_after,max_concurrency}}
```

连接后按 Discord 风格的 opcode 走：

| op | 方向 | 说明 |
|---|---|---|
| 10 Hello | 收 | `{heartbeat_interval}`（毫秒）；连接后的第一条 |
| 2 Identify | 发 | `{token: "QQBot <access_token>", intents, shard: [0,1], properties}` |
| 0 Dispatch | 收 | `t` = 事件名；`s` = 递增序号；`d` = 事件体；最外层 `id` = 事件 id |
| 11 Heartbeat ACK | 收 | 心跳回执 |
| 1 Heartbeat | 发 | `{d: 最近一次收到的 s}`（首帧 `null`），按 `heartbeat_interval` 发 |
| 6 Resume | 发 | `{token, session_id, seq}`：断线重连补发遗漏事件 |
| 7 Reconnect | 收 | 服务端要求重连 |
| 9 Invalid Session | 收 | 必须重新 Identify |
| 12 / 13 | — | 仅 webhook 模式 |

错误码处置：`4009`（连接过期）→ Resume；`4006`（session 失效）/`4007`（seq 错）→ Identify；
`4914`（已下架，只能连沙箱）/`4915`（已封禁）→ 停止并写错误日志，不重试；
`4008`（发得太快）→ 退避后重连；`4900~4913` → 重连。

`session_start_limit.max_concurrency` 是「每 5 秒可建几个 session」；`remaining` 是当日余量。
重连优先 Resume（不消耗 Identify 配额）。

### 3.3 事件（`intents` 位）

```
GROUP_AND_C2C_EVENT (1 << 25)
  - C2C_MESSAGE_CREATE        单聊消息
  - GROUP_AT_MESSAGE_CREATE   群里 @ 机器人
  - GROUP_MESSAGE_CREATE      群全量消息（需群主放开「接收全部消息」权限）
  - GROUP_ADD_ROBOT / GROUP_DEL_ROBOT / FRIEND_ADD / FRIEND_DEL
  - C2C_MSG_RECEIVE / C2C_MSG_REJECT / GROUP_MSG_RECEIVE / GROUP_MSG_REJECT
```

本插件只需要 `1 << 25`（频道相关位不订阅，避免无权限时连接被直接关闭）。

事件体（三个消息事件的字段一致）：

| 字段 | 说明 |
|---|---|
| `id` | 消息 id（被动回复与撤回要用） |
| `author` | `{id, member_openid(群), user_openid(单聊), union_openid, username, bot, member_role}` |
| `content` | 文本；群 @ 事件里**已去掉 @ 前缀** |
| `group_openid` | 群 id（群消息有） |
| `timestamp` | RFC3339 |
| `message_type` | `0` 文本 / `3` 卡片 / `103` 引用消息 |
| `message_scene.ext` | `msg_idx=REFIDX_...`（本条消息索引，引用回复用）、`ref_msg_idx=...`（被引用的）、`auth_token=...` |
| `attachments[]` | `{url, filename, content_type, size, width, height, voice_wav_url, asr_refer_text}`；`content_type` 取 `image/jpeg`、`image/png`、`image/gif`、`video/mp4`、`voice`、`file` |
| `mentions[]` | 被 @ 的用户（**不含机器人自身**） |
| `msg_elements[]` | `message_type=103` 时是被引用的内容（递归结构） |
| `ark_data` | `message_type=3` 时的卡片数据 |

**去重**：同一 `msg_id` 可能重复推送；「全量模式」下 @ 消息可能同时以
`GROUP_MESSAGE_CREATE` 与 `GROUP_AT_MESSAGE_CREATE` 到达（待实测，见 §17）。按 `msg_id` 去重即可。

### 3.4 发消息

```
POST /v2/groups/{group_openid}/messages      # 群
POST /v2/users/{user_openid}/messages        # 单聊
{
  "msg_type": 0,                          # 0 文本 / 2 markdown / 7 富媒体
  "content": "...",                       # msg_type=0
  "markdown": {"content": "..."},         # msg_type=2
  "media": {"file_info": "..."},          # msg_type=7（先上传）
  "msg_id": "<事件 d.id>",                 # 被动回复（5 分钟内，群最多 5 次）
  "msg_seq": 1,                           # 同一 msg_id 的顺序号，重复的 msg_id+msg_seq 会失败
  "message_reference": {"message_id": "REFIDX_..."},   # 引用回复
  "event_id": "<事件最外层 id>"            # 被动回复事件（与 msg_id 二选一）
}
```

- **群消息不支持流式**，单聊有流式接口（本插件不用流式，逐条整段发）；
- 响应里 `ext_info.ref_idx` 是自己发出的那条第 `REFIDX_`，可用于「引用机器人自己以前的话」；
- 撤回：`DELETE .../messages/{message_id}`，发送后 **2 分钟内**有效。

### 3.5 频控与时效（决定出站排队策略）

| 场景 | 被动回复有效期 | 每条消息可回复次数 |
|---|---|---|
| 单聊 | 60 分钟 | 4 次 |
| 群聊 | 5 分钟 | 5 次 |

主动消息（不带 `msg_id`）：群 `60 qpm`（机器人维度）、`20 qpm`（单关系维度）、每群每天 1000 条；
单聊 `10 qps`、`20 qpm`、每用户每天 1000 条；未认证账号更低（群 30 qpm、单聊 5 qps / 30 qpm）。
用户可在 QQ 客户端关闭「允许主动发送」，关闭后主动消息一律失败。

### 3.6 富媒体

发送前必须先上传拿 `file_info`，再用 `msg_type=7`：

| file_type | 类型 | 格式 | 软限制 | 硬限制 |
|---|---|---|---|---|
| 1 | 图片 | png / jpg | 20 MB | 200 MB |
| 2 | 视频 | mp4 | 30 MB | 200 MB |
| 3 | 语音 | silk（也收 mp3/wav） | 20 MB | 200 MB |
| 4 | 文件 | 任意 | 200 MB | 200 MB |

超软限制降级成「文件」，超硬限制报错。三种上传方式：

1. **URL 上传**（`url`）：平台自己来下载 → 需要公网可达地址，本机文件用不了；
2. **分片上传**（推荐，本地文件走这条）：
   `POST /v2/groups/{group_id}/upload_prepare`
   `{file_type, file_size, file_name, md5, sha1, md5_10m}` →
   `{upload_id, block_size, parts:[{index, presigned_url, block_size}], upload_config:{concurrency, retry_timeout, retry_delay}}`；
   然后按 `block_size` 切片 `PUT` 到每个 `presigned_url`，每片成功后
   `POST .../upload_part_finish`，最后 `POST .../files {upload_id, file_name, srv_send_msg:false}`
   拿 `file_info`；
3. **二进制直传**（`file_data`）：≤ 5 MB，字段内容的确切编码文档未写明（见 §17）。

上传接口 QPS 限制 50，预上传 10；`file_info` 带 `ttl`（过期要重传）；单聊与群聊的上传结果
**不互通**（群文件不能发到单聊）。

### 3.7 平台侧必须由人（群主 / 机器人管理员）打开的开关

| 想做的事 | 平台侧要打开 |
|---|---|
| 群里 @ 机器人就能对话 | 把机器人拉进群即可（默认行为） |
| 「新消息就回」（always 模式） | 群管理 → 机器人资料页 → 「机器人可获取的群聊消息范围」= 获取群内全部消息；并打开「机器人主动在群聊内发言」 |
| 群内被动回复（而非只能主动发） | 管理端消息能力配置里放开被动回复 |
| 用沙箱环境调试 | 开放平台管理端切沙箱（`4914` 表示当前连接不允许）

这几条要写进给用户的接入说明；插件只能检测「收不到全量消息」并记警告。

---

## 4. 总体架构

### 4.1 分层

```
                     ┌──────────────────── 主程序 io 线程 ────────────────────┐
                     │ AgentHost: 内置根("root") + 插件根 ext:chatbot:<n>(人设) │
                     │ 每个「平台账号+会话」= 一条 channel (sessionId)          │
                     └───────────▲───────────────────────────┬────────────────┘
            通道事件(READY/TURN_END/…)│                    │ send_input(文本+附件)
                     ┌───────────────┴───────────────────────▼────────────────┐
                     │  编排层 (chatbot/ 目录, 跑在自己的 io 线程, 无锁)       │
                     │  ├ router    ChatKey → 人设(root) + 通道(懒创建/LRU)    │
                     │  ├ engine    归一化 → 去重 → 命令 → 聚合 → 投递          │
                     │  ├ outbound  切分 / 引用 / 多条 / 频控 / 降级            │
                     │  └ tools     在 root 上注册的工具 (send_message 等)     │
                     └───────────────▲───────────────────────┬────────────────┘
                     InboundEvent    │                       │ OutboundMessage
                     ┌───────────────┴───────────────────────▼────────────────┐
                     │  平台适配层 platform/ (接口 IPlatform)                  │
                     │  └ qq/: 账号 actor · 网关 WS · openapi · 分片上传        │
                     └────────────────────────────────────────────────────────┘
```

两条硬边界：

- **编排层与平台层之间只有两个数据结构**：`InboundEvent`（平台 → 编排）与
  `OutboundMessage`（编排 → 平台）。加新平台只实现 `IPlatform`，编排层一行不改。
- **编排层与主程序之间只经 `agentxx.agent.runtime` 表**（SDK 便捷函数）。平台协议细节
  不出现在编排层。

### 4.2 线程模型

| 谁 | 在哪条线程 | 为什么 |
|---|---|---|
| 平台网络（WS 收发、HTTP、心跳、定时器） | 插件自建的 io 线程（每账号一条 `asio::io_context`，可合并） | 长连接 + 心跳 + 重连属于「插件自管资源」，`stop` 里撤销；不占用主程序 io 线程 |
| 编排层（去重表、聚合缓冲、发送队列、频控桶） | 同上（同一 `io_context`） | 单线程跑 → 不需要锁；`stop` 时一并停 |
| 通道事件回调 | **主程序 io 线程** | 框架约定；回调里只做「拷贝 → post 到编排线程」，立即返回 |
| root 上注册的工具 | 主程序工作线程池（`blocking_tool`） | 工具里要发消息 / 等上传，不能在主程序 io 线程阻塞 |

> 不用 `polled_tool` 那种「受控轮询桥」跑网关：桥的设计前提是「一次有限步骤」，
> 长连接 + 心跳 + 重连是持续存在的工作，放进去会不停申请 driver。

### 4.3 死锁分析（为什么同步调用 `send_input` 是安全的）

三条可能成环的路径，逐一排除：

1. **编排线程 → `send_input` → 主程序 io 线程**：主程序侧只是把输入塞进会话队列，
   不回头等编排线程 → 不成环。
2. **工具（工作池）→ 平台 send → 编排/平台线程**：等待期间不持有主程序 io 线程 → 不成环。
3. **主程序 io 线程 → 通道事件回调 → post 到编排线程**：`post` 不等待 → 不成环。

规则写进代码约定：**任何运行在主程序 io 线程上的回调都不得同步等待编排线程或平台线程**。

### 4.4 目录结构

```
agent/plugins/agentxx_chatbot/
  plugin.yaml                    # interfaces.require: [agentxx.agent.runtime, agentxx.agent.tools]
  CMakeLists.txt                 # 与 example_runtime 同模板 (独立 SHARED + 内置 OBJECT 两分支)
  agentxx_chatbot.cpp            # C ABI 入口: get_info / create / start / stop / mount / unmount / destroy
  chatbot_types.h                # ChatKey / InboundEvent / OutboundMessage / MediaRef 等归一化结构
  chatbot_config.h/.cpp          # args + 可选 config 文件 → Config (账号 / 人设 / 命令 / 限额)
  chatbot_core.h/.cpp            # 纯逻辑, 可单测: 去重 / 聚合 / 命令解析 / 长文本切分 / 引用编号解析
  chatbot_router.h/.cpp          # ChatKey → 人设 + 通道; 懒创建; LRU 回收; 命令 /axx-info 的数据源
  chatbot_engine.h/.cpp          # 入站流水线: 平台事件 → 命令 → 聚合 → send_input
  chatbot_outbound.h/.cpp        # 出站队列: 切分 / 引用 / 多条 / 频控 / 被动→主动降级
  chatbot_tools.cpp              # root 上注册的工具 (send_message / history / chat_info / status)
  platform/platform.h            # IPlatform 接口 + PlatformEventSink
  platform/fake/fake_platform.*  # 内存假平台 (测试与本地演示)
  platform/qq/qq_platform.*      # 账号 actor: 编排 IPlatform 的实现
  platform/qq/qq_api.*           # access token / 发消息 / 撤回 / 网关地址 / 上传接口
  platform/qq/qq_gateway.*       # WS 连接 / Hello / Identify / 心跳 / Resume / 重连退避
  platform/qq/qq_media.*         # 分片上传 + md5/sha1/md5_10m + 格式判定
```

`chatbot_core.*` 刻意做成**不碰网络、不碰主程序**的纯函数集合，测试可以像
`agentxx_websearch/websearch_impl.h` 那样直接被 `agentxx_test` 包含
（测试 include 根已是 `agent/plugins/`）。

---

## 5. 配置设计

### 5.1 yaml 写法（与 `model.list` / `model.use` 同构）

```yaml
plugin:
  list:
    - path: ${AGENTXX_EXEC_DIR}/plugins/agentxx_chatbot
      enabled: true
      args:
        # ---- 第一段: 声明有哪些平台账号 (类比 model.list) ----
        accounts:
          - id: qq_main                     # 账号标识, 后面人设按它对接
            type: qq                        # 平台类型 (预留 wechat 等)
            appId: "${QQBOT_APPID}"          # 本机已写入 .env 与 resource/keys/qq_bot.env
            appSecret: "${QQBOT_APPSERCRET}"
            sandbox: false                  # true = 沙箱环境
            # 可选: 覆盖网关/接口地址 (灰度或抓包调试用)
            # apiBase: "https://api.bot.qq.com"
            # wsShard: [0, 1]
            # 可选: 主动消息频控覆盖 (默认取平台默认值, 略保守)
            # rateLimit: {groupPerMinute: 20, singlePerMinute: 20}

        # ---- 第二段: 人设 (每项 = 一个 root), 各自声明对接哪些账号与会话 ----
        personas:
          - name: assistant                 # 人设名 (root 展示名, 也是会话 id 前缀)
            model: chat_fast                # 可选; 必须是 model.list 里的某个 name; 空 = 用主程序当前模型
            prompt:
              systemPrompt: |
                你是 QQ 群里的助手, 说话简短自然, 不用列表堆砌。
                多条消息一起到达时, 逐条回应用户关心的问题; 同一话题可合并成一条回复。
                不想回复时不要调用任何工具。
              append:                       # 可选: 追加段 (key → 内容)
                group_rules: "群里不要发超过 500 字。"
            tools:                          # 可选: 工具白名单 (工具名, 不是插件名; 只过滤内置/中间件工具)
              - agentxx_share_store
            mount:                          # 可选: 插件挂载策略 (同 yaml plugin.mount)
              mode: listed                  # 本插件自己会被自动补进名单 (见 §8.5)
              list: [agentxx_filesystem, agentxx_websearch]
            # 对接哪些「账号 + 会话」
            bind:
              - account: qq_main
                chats:
                  - {type: group,   id: "B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5", mode: mention}
                  - {type: group,   id: "A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4", mode: always}
                  - {type: private, id: "C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6", mode: always}
                # 也可用通配 (同一账号下所有会话都交给这个人设)
                # chats: [{type: any, id: "*", mode: mention}]

        # ---- 第三段: 通用行为 ----
        behavior:
          aggregateWindowMs: 800     # 多条消息合并窗口 (仅 always 模式; 见 §7.3)
          sendIntervalMs: 350        # 一轮内多条回复之间的间隔 (防刷屏/风控)
          messageSplitLimit: 1500    # 单条发送文本上限 (按字符计, 见 §10.2)
          splitMarker: true          # 切出多条时末尾加 (2/3) 分段标记
          maxSplitParts: 8           # 单条最多切成几段
          markdown: false            # 群消息是否用 msg_type=2 发 (单聊恒为纯文本)
          maxRepliesPerTurn: 5       # 一轮最多发几条 (再多则返回失败文本让模型合并, 见 §10.3)
          maxChatsPerPersona: 64     # 每人设活跃会话上限 (= 每根 64 通道)
          historyKeep: 40            # 每会话保留最近多少条原始消息 (供 chatbot_history)
          attachments:
            fetch: auto              # auto | url | download (见 §11.1)
            maxPerMessage: 5         # 单次投给会话的附件上限 (主程序上限就是 5)
          exposeToolsOnHost: false   # 是否把 chatbot_* 工具也注册到内置 root (调试用; 见 §8.5)
          overflowPolicy: active     # 被动回复次数用尽后: active(转主动) | truncate | fail
          fallbackReply: mention     # 模型没调工具时是否兜底发正文: mention | always | never
          injectContextHint: true    # 每轮输入头部是否带「你在哪个群」的元信息段
          flushStaleOnNextMessage: true  # 有旧内容没发出时, 下次有人说话顺带补 1 条
          humanInLoop: auto-allow    # 工具要人批准时怎么办: auto-allow | auto-deny (见 §12.4)

          # 以下是进阶项 (不写就用默认值, 写在这里只为说明可调)
          # aggregateMaxItems: 30        # 每会话聚合缓冲最多几条 (达到先投再开窗口)
          # aggregateMaxChars: 12000     # 每会话聚合缓冲最多多少字符
          # outboundQueueMax: 200        # 出站队列长度上限 (超出丢最旧)
          # dedupeCapacity: 512          # 每账号去重环容量

        # ---- 第四段: 关键词命令 (见 §9) ----
        commands:
          - name: /axx-info
            mode: local
            reply: |
              平台: {platform}  账号: {account}  会话: {chat}
              人设: {persona}  通道: #{channelId}  模式: {mode}
              收到 {received} 条 · 回复 {sent} 条 · 丢弃 {dropped} 条 · 失败 {failed} 条
          - name: /axx-hello
            mode: model
            prompt: "用户想让你打招呼。用一句话向 {sender} 问好, 不要寒暄超过一句。"
          - name: /axx-help
            mode: local
            reply: "可用指令: {commandList}"
          - name: /axx-stop
            mode: action
            action: cancel               # 打断当前轮 (调 cancelChannel)
            reply: "已请求停止当前这轮。"
```

### 5.2 为什么这样分四段

- **`accounts`**：一个 QQ 机器人 = 一份 `appId`/`appSecret`/一条网关连接。先声明清楚
  「有哪些账号」，后面人设只引用 `id`，改凭证不影响人设。
- **`personas`**：人设 = root（提示词 / 工具 / 挂载 / 模型都是 `AgentConfig` 级），
  它自己声明「我服务哪些账号下的哪些会话」—— 这样**反向也一目了然**：
  「这个群由谁说话」= 在 `personas[].bind` 里搜它。
- **`behavior` / `commands`**：与具体人设无关的公共行为，写一处，全部人设共享。

### 5.3 环境变量展开（一个必须注意的陷阱）

`plugin.list[].args` 由 `config_loader.cpp` 的 `yamlToJsonResolveEnv` 处理：**先展开 `${VAR}`，
再把展开结果按标量重新判类型**。于是

```yaml
appId: ${QQBOT_APPID}        # 1906091230 → 先展开成 "1906091230" → 再被 parseFullNumber 命中
appId: "${QQBOT_APPID}"      # 同上: 引号只影响 YAML 解析, 展开后的类型判定照样命中
```

**两种写法都会把这个值变成 JSON 数字**。因此插件侧读取时必须：

- `appId`：接受 `string` / `number_integer` / `number_unsigned`，取整数值后转文本；
  若遇到 `number_float`（超出 `int64` 的长数字会被判成浮点，精度可能已丢失）→ 拒绝启动并
  提示「请在 yaml 里给 appId 加一个非数字前缀或改用 `appIdFile`」（见下）；
- `appSecret`：字母数字混合，不会被误判，按字符串取。

为稳妥起见同时提供一个**不经过类型判定的入口**：

```yaml
accounts:
  - id: qq_main
    type: qq
    appSecret: "${QQBOT_APPSERCRET}"
    appIdFile: "${AGENTXX_WORK_DIR}/resource/keys/qq_bot.env"   # 可选: 从 .env 形态的文件读 QQBOT_APPID
```

`appIdFile` / `appSecretFile` 指向一个 `.env` 形态的文本文件（`KEY=VALUE` 每行一条），
插件自己解析，完全不经过 yaml 的类型判定。密钥场景下这比在 yaml 里展开更明确
（也便于把凭证文件排除在版本库外）。

`accounts[]` 里两者都给时：显式键优先，文件只补齐缺失项。

> 本机现状（已确认）：`resource/keys/qq_bot.env` 与仓库根 `.env` 都有
> `QQBOT_APPID=1906091230` / `QQBOT_APPSERCRET=...`，因此默认配置直接写 `${QQBOT_APPID}` 即可；
> 插件侧按 §5.3 的类型兼容逻辑读它（数字形态也接受）。

### 5.4 独立配置文件（可选）

`plugin.list[].config` 可以指向一个 yaml 文件（主程序归一化为绝对路径后经
`get_plugin_config_path` 交给插件）。规则：

- `args` 与 `config` 文件**不引入第二套语义**：config 文件里写的还是上面那四段；
- 合并顺序：`args` 覆盖文件，映射逐键、列表整体覆盖（与主程序的分层配置同一规则）；
- 用途：多人设时把 `personas` 单独放一个文件，`agentxx-config.yaml` 里只写 `path` + `config`。

### 5.5 配置校验（启动即失败，不留半启动状态）

`start` 里在**建根之前**做完全部校验，任何一条不过就 `error` 返回、由主程序回滚：

| 检查 | 失败处理 |
|---|---|
| `accounts` 非空；`id` 唯一且只含 `[A-Za-z0-9_-]` | 拒绝启动 |
| `type` 已注册（当前只有 `qq`） | 拒绝启动 |
| `qq` 账号的 `appId`/`appSecret` 可解析 | 拒绝启动（附上「取不到值」的具体键名） |
| `personas` 至少一个；`name` 唯一 | 拒绝启动 |
| `personas[].bind[].account` 必须存在于 `accounts` | 拒绝启动 |
| 同一个 `(account, chatType, chatId)` 只能出现在一个人设里 | 拒绝启动（否则「这个群归谁」不确定） |
| 同一个 `(account, chatType)` 下最多一条 `id: "*"` 通配项 | 拒绝启动（多条通配无法确定优先级） |
| 人设数量 + 1（内置根） ≤ `AgentHost::Config::maxRoots`（默认 8） | 拒绝启动，提示最多几个人设 |
| `commands[].name` 以 `/` 开头且不重复 | 拒绝启动 |
| `messageSplitLimit` ≥ 100、`aggregateWindowMs` ≥ 0、`sendIntervalMs` ≥ 0 | 裁剪到合法范围并警告 |
| `commands[].mode` 只能是 `local` / `model` / `action`；`action` 只能是 `cancel` / `clear` | 拒绝启动 |

校验通过后再 `mountRoot` × N（通道懒创建，见 §6.2）。根建完把每个账号的
平台连接拉起来；只要有账号连不上，插件**不整体失败**，只在状态里标 `连接失败 + 原因`
（一个账号挂了不该拖垮其它账号）。

---

## 6. 路由与会话映射

### 6.1 数据结构

```cpp
struct ChatKey {                     // 一个会话的唯一身份
    std::string accountId;           // 配置里的 accounts[].id
    ChatType    type;                // Group / Private
    std::string chatId;              // group_openid 或 user_openid

    std::string key() const;         // "qq_main|group|B2C3…"  (路由表 / sessionId 的原料)
};

struct ChatBinding {                 // 配置解析产物
    std::string personaName;         // 归属人设
    ChatMode    mode;                // Mention / Always / Off
};
```

路由表在 `start` 时一次性建好（`map<key, ChatBinding>`），运行期只读 → 不需要锁。
通配项 `{type: any, id: "*"}` 作为兜底，精确项优先。

### 6.2 通道（会话）的创建与回收

- **懒创建**：`openChannel` 推迟到某会话**第一条要投递的消息**时。理由：配置里可能列了几十个群，
  大部分一天也不说话，占着 64 通道额度不划算；懒创建让额度跟着实际活跃度走。
- **sessionId**：`chatbot_{persona}_{accountId}_{type}_{chatId 前 16 位 md5}`
  —— 可读、定长、跨重启稳定（同一个群永远同一个会话）。
- **title**：`{群或用户显示名}`（群成员昵称 / 群名拿不到时用 id 前 8 位），
  便于在 TUI 会话列表里认出是谁。
- **LRU 回收**：活跃通道数达到 `maxChatsPerPersona` 时，关掉最久没有新消息的那条通道，
  并在需要时重新打开（上下文从会话库恢复）。回收日志写清 `chatId` 与原因。
- **前提：必须有 `data_dir`**。框架的派生规则是「`dataDir` 为空就没有可共享的会话根，
  根退化为内存会话」（`enableSessionStore` 被强制关掉并 warn）。此时关通道 = 丢上下文，
  因此 LRU 回收退化为「不回收」：达到上限后新会话直接拒绝并告警，
  提示用户配 `data_dir` 或调大 `maxChatsPerPersona`。
- **关闭顺序**：`stop` / 禁用 / 卸载时 `closeChannel` 全部通道 → `unmountRoot` 全部根；
  两个入口都是幂等的，框架也会按实例名兜底回收。

### 6.3 预算对照

| 限额 | 值 | 本插件的用法 |
|---|---|---|
| 进程内根 | 8（含内置根） | 人设 ≤ 7 |
| 每根通道 | 64 | 每人设 ≤ 64 个活跃会话（LRU 兜住） |
| 每实例触发器 + 定时器 | 256 | 本插件不用触发器与定时器（聚合窗口用自己的 `steady_timer`），额度全空 |
| 进程内插件挂载 | 无硬上限 | 每个人设 root 上挂 `agentxx_chatbot` + `personas[].mount` 里的那些 |

> 聚合窗口为什么不用 `register_timer`：定时器是「按固定间隔往通道投一条固定输入」，
> 而聚合需要「每来一条消息就重置窗口、按消息条数决定投什么」。用插件自己的
> `steady_timer` 更直接，也不消耗框架额度。

### 6.4 多人设共用一个账号

同一个 QQ 账号的不同群可以交给不同人设（各自一个 root）；同一个群不能出现在两个人设里
（配置校验已拒）。如果有人确实想让「同一个群里两个角色」——那不是一个群的两种模式，
而是应该用 `/axx-xxx` 命令切换人设（本期不做，列为后续）。

---

## 7. 入站流水线（平台消息 → 会话输入）

### 7.1 一条消息的完整路径

```
网关事件
  │ ① 归一化 (qq → InboundEvent: 谁/在哪/什么/带什么/引用谁/消息 id/时间)
  ▼
去重表 (§7.5: 同 msgId 只处理一次)
  │ ②
  ▼
路由 (ChatKey → 人设 + 模式; 不在配置里的会话 → 丢弃并 debug 日志)
  │ ③
  ▼
关键词命令 (§9: 命中即执行, 默认不再进模型)
  │ ④
  ▼
聚合缓冲 (按 ChatKey 一桶; §7.3)
  │ ⑤ 被 @ 立刻唤醒 / always 模式窗口到期 / 上一轮结束时清空积压
  ▼
组装一次输入 (文本块 + 附件; §7.4、§7.6)
  │ ⑥
  ▼
send_input (delivery = next-turn) → 通道队列 → 一轮 ReAct
```

### 7.2 触发模式（`mode`）

| 模式 | 什么时候投递 | 适用 |
|---|---|---|
| `mention`（默认） | 只处理「@了机器人」的消息；全量消息只入缓冲不唤醒 | 群聊 |
| `always` | 任何新消息都投递 | 私聊、机器人专属群、需要它主动接话的群 |
| `off` | 该会话完全不处理（配置里可以先写好、暂时关掉） | 灰度 |

两个特殊规则：

- **私聊里 `mention` 等同 `always`**：单聊消息本来就是直接发给机器人的，没有「@」这回事；
  配成 `mention` 时插件自动升为 `always` 并 debug 日志一次（避免用户以为私聊坏了）；
- **「@」判定只看事件类型**：`GROUP_AT_MESSAGE_CREATE` → 视为被 @；
  `GROUP_MESSAGE_CREATE`（全量）→ 视为未 @；`C2C_MESSAGE_CREATE` → 视为私聊直呼。
  不解析文本里的 `@名字`（群里 @ 别人时平台不把它们算进来，`mentions[]` 只给非机器人成员）。

**关键点：`mention` 模式下，全量消息仍然要进缓冲。** 因为「@之前的那几句话」是上下文
（「你们看看这个 …」+「@bot 你怎么看」）。所以：

- `mode = mention`：全量消息 → 只进缓冲（`mentionedMe = false`）；被 @ 的消息 → 进缓冲并**立即唤醒**；
- `mode = always`：任何消息都进缓冲；唤醒条件是「窗口到期」（见 §7.3）；
- 缓冲里没有被唤醒的消息不会丢，会跟着下一次唤醒一起带上去（这正是要求 5 要的行为）。

### 7.3 聚合：一轮耗时期间积累的多条消息怎么办

这是本插件最核心的一条行为，用一个「**每会话一个缓冲 + 一个静默窗口**」实现：

```
buffer[chat] = { items: [P1, P2, ...], timer }

onMessage(p):
    buffer.items.push_back(p)
    if p.mentionedMe:                          # 被 @ 立刻唤醒 (不等窗口)
        flush(chat)                            # 立即投递
    else if binding.mode == Always:
        timer.reset(aggregateWindowMs)         # 每来一条重置窗口
        onTimerExpire → flush(chat)
    else:                                      # Mention 模式下非 @ 消息: 只入缓冲
        (不设 timer)

onTurnEnd(chat):                              # 通道事件回调里只 post
    if buffer.items not empty: flush(chat)     # 上一轮结束时把积压的一次性带走

flush(chat):
    if channel.turnBusy: return                # 在忙: 留着, 等下一轮的 onTurnEnd
    if buffer.items empty: return
    input = assemble(buffer.items); buffer.items.clear()
    send_input(chat.channel, input, delivery=next-turn)
```

投递时机的判断依据是「**通道是否正在忙**」，而不是「是否正在等 LLM」：

| 场景 | 行为 |
|---|---|
| flush 被触发（被 @ / `always` 窗口到期 / 上一轮结束）且通道空闲 | 立刻组装并 `send_input` |
| flush 被触发但通道正在跑一轮（`turnBusy`） | 不投；缓冲继续积累，等本轮 `TURN_END` 再 flush |
| 通道空闲，但缓冲里只有未 @ 的消息（`mention` 模式） | **不投**（flush 没被触发）；它们在下一次被 @ 时一起带上 |
| 一轮里连续 @ 了 3 次 | 第一次 @ 立即投；第 2、3 次进缓冲；本轮结束时一次带上（模型看到三条，自行决定回一条还是三条） |

`turnBusy` 只从通道事件 `TURN_START` / `TURN_END` 维护（框架内部已有同名状态，插件侧照抄一份
即可，避免跨线程查询）。`TURN_END` 回调里 flush 需要 post 到编排线程执行（回调必须立即返回）。

> 缓冲上限：每会话最多 `aggregateMaxItems`（默认 30）条 / `aggregateMaxChars`（默认 12000）字符；
> 达到上限先投再开新窗口（与框架的 COALESCE 背压同一思路），避免刷屏把上下文一次打满。

### 7.4 一次输入的组装形态（模型看到什么）

```text
你正在 QQ 群「后端讨论组」(群 id 尾号 B2C3…)；下面是最近收到的消息：
[#1] 小明(member): 你们看看这个报错
[#2] 小红(member): +1 我也遇到了
[#3] 小明(member) @机器人: 帮我们看看 (引用了 小红 的消息)
[#3:附件] photo.png (本地: C:/…/qqbot/qq_main/group-B2C3D4E5/photo.png)
```

规则：

- 每条消息一行，以 `[#n]` 开头（`n` 就是 `chatbot_send_message` 的 `replyTo` 编号，见 §8.3），
  后面带**说话人**与**角色**（`owner`/`admin`/`member`），@ 到机器人的那条标 `@机器人`，
  引用了别的消息的标 `(引用了 <说话人> 的消息)`；
- 附件单独成行 `[#n:附件] 文件名 (本地: <路径>)`（同时按 §11 转成 `attachments` 投递）；
  超过 5 个的降级成路径文本；
- 私聊不带「群聊」前缀，只带说话人；
- 最前面可选加一段**会话元信息**（`behavior.injectContextHint`，默认开）：
  `你正在 QQ 群「<群名或 id 前 8 位>」, 当前成员包括 …；你有修改设置与查看历史的工具。`
  这一段每轮都带，成本极低，但让模型知道「自己在哪个群、能不能主动发」。

不在输入里塞「请回复第几条」这类指令 —— 回复哪条由模型自己决定（要求 6）。

### 7.5 去重与乱序

- **去重键**：`(accountId, msgId)`。同一 `msg_id` 可能被平台重复推送；
  保留最近 512 个键的环形集合（每账号一份），命中即丢并计数 `duplicated`。
- **全量模式下的双事件**：开启「接收全部消息」后，@ 的消息可能既来 `GROUP_MESSAGE_CREATE`
  又来 `GROUP_AT_MESSAGE_CREATE`（见 §17 待确认）。两者 `id` 相同 → 天然被去重键吃掉；
  若实测 id 不同，则以 `message_scene.ext` 里的 `msg_idx` 作为第二去重键（两者都记）。
- **乱序**：以平台 `timestamp` 为序；同一 `timestamp` 保持到达顺序。缓冲内排序后再组装。
- 单条超长原文（> 20 KB）先截断到 20 KB 并标注「（内容过长已截断）」，避免一条消息把上下文
  顶满（原文仍留在历史里，模型可经 `chatbot_history` 取）。

### 7.6 投给框架的输入 JSON 原文

`send_input` 的形态与 FFI/CLI 完全一致（主程序 `parseUserInputJson` 校验）：

```json
{
  "text": "你正在 QQ 群「后端讨论组」……\n[#1] 小明(member): …\n[#3] 小明(member) @机器人: …",
  "attachments": [
    {"type": "image", "display_name": "photo.png", "mime_type": "image/png",
     "path_or_url": "C:/…/qqbot/qq_main/group-B2C3D4E5/photo.png", "size_bytes": 256000}
  ],
  "delivery": "next-turn"
}
```

- `type` 只取 `image` / `audio` / `video`（框架的 `MediaType` 只有这三种）：普通文件**不作为附件**，
  只把本地路径写进 `text`（模型可以用 `agentxx_filesystem_read` 读它）；
- `delivery` 固定 `next-turn`（`next-step` 会让消息插进正在跑的那一轮，破坏「一轮处理一批」的语义；
  `collect` 的重合窗口与插件自己的聚合窗口重复，也不使用）；
- 校验失败（附件超上限、条数超上限、`text` 与附件都空）时按「去掉附件只发文本」重试一次；
  仍失败则记 `failed` 日志，并把「上一条消息投递失败」写进**下一轮**输入的元信息段
  （插件没有向通道插提示的能力，只能靠下一轮输入告知模型）。

---

## 8. 让模型自己决定怎么回（不写死）

### 8.1 三条原则

1. **程序不做「回不回」的判断**（除了触发模式与命令）：`mention`/`always` 决定「这条要不要叫醒
   模型」，叫醒之后回不回、回几条、引用谁，都是模型的事。
2. **回复的唯一出口是工具**：agent 的正文输出默认**不自动发到群里**（因为 agent 的正文往往是
   「思考给自己看的过程」）。发消息必须显式调用 `chatbot_send_message`。
3. **有兜底，但兜底也可关**：`behavior.fallbackReply`：
   - `mention`（默认）：只有当本轮输入里有 @ 机器人、且模型**一次都没调** `chatbot_send_message`
     时，把本轮最后一段正文原样发出（并记 `fallback` 计数）。避免「模型想了半天什么都没说」；
   - `always`：无论有没有 @，没调工具就发最后一段正文；
   - `never`：完全按模型意愿（它不调工具就是「已读不回」）。

### 8.2 为什么把「发消息」做成工具而不是自动转发

| 自动转发正文 | 工具发送（本设计） |
|---|---|
| 无法引用某条消息 | 工具参数带 `replyTo` |
| 一轮只能发一条 | 工具可调多次，每次一条 |
| 模型无法「已读不回」 | 不调工具即不回 |
| 无法带附件 | 工具参数带 `attachments` |
| 无法插话到多个话题 | 一次调用一个话题，多条 |

代价是模型可能「忘了调工具」（见 §8.1 兜底）与「一轮调太多次」（§10.3 限额）。

### 8.3 工具清单（在 root 上注册，`mount` 回调里做）

| 工具 | 作用 | 实现方式 |
|---|---|---|
| `chatbot_send_message` | 往当前会话（或指定会话）发一条消息；支持 `replyTo` / `attachments` / `split` | `blocking_tool`（内部发 HTTP、等上传，会在工作池上阻塞） |
| `chatbot_history` | 取当前会话最近 N 条原始消息（含被过滤的、带说话人与时间） | `fast_tool`（纯内存读，不阻塞） |
| `chatbot_chat_info` | 当前会话信息：平台、账号、群/私聊、成员角色、触发模式、通道状态 | `fast_tool` |
| `chatbot_status` | 本插件全局状态：账号连接、每人设通道数、计数（收到/回复/丢弃/失败） | `fast_tool` |
| `chatbot_set_mode` | 把当前会话的触发模式改成 `mention`/`always`/`off`（**运行期生效，不写回 yaml**） | `fast_tool` |

工具的参数说明（depict）写在插件自己的字面量里（AGENTS.md 已约定：插件工具提示词归属插件）。

`chatbot_send_message` 参数：

```json
{
  "text": "要发送的文本（可含 markdown，插件按平台能力降级）",
  "replyTo": "3",           // 引用本轮输入里的第几条 (1 起); 空 = 不引用
  "attachments": [          // 可选; 本地路径或 http(s) URL
    "C:/tmp/chart.png"
  ],
  "split": true,            // 超长是否自动切分 (默认 true; false = 超长报错)
  "chat": "可选: 指定会话 (默认当前)"
}
```

`chat` 只能指向**同一个人设**下的其他会话（形如 `qq_main|group|B2C3…`），
不能跨人设：工具是挂在人设的 root 上的，跨人设发送应该由那个人设自己处理。
填了不存在的会话（或别的人设的会话）直接返回错误文本。

`replyTo` 的编号来源：本轮输入里每条消息前面的 `[#n]`。程序侧映射关系：
`#n` → `(msgId, msgIdx)`，出站时填 `msg_id`（被动回复）与 `message_reference.message_id`。
编号在本轮结束后失效（下一轮从 `[#1]` 重新开始）。

### 8.4 「逐条回」与「同一话题只回一条」怎么落地

两种都是模型的选择，程序只提供能力：

- 想逐条回：对 `[#1]` `[#2]` `[#3]` 各调一次 `chatbot_send_message`（各带自己的 `replyTo`）；
- 想合并成一条：调一次，`text` 里把三件事一起说，`replyTo` 留空或指最有代表性的那条。

提示词里给一句引导（不是强制）：

> 一次收到多条消息时，先判断它们是不是同一个话题：不同话题分别回复（多次调用工具，
> 各自引用对应消息）；同一话题合并成一条回复即可。

### 8.5 人设 → 根配置（`config_json`）的派生

`personas[]` 一项映射成一个 `mountRoot` 调用：

| `config_json` 字段 | 值 |
|---|---|
| `agentName` | `chatbot-{personaName}` |
| `prompt.systemPrompt` | `personas[].prompt.systemPrompt` 优先；未给则用默认模板（§8.6） |
| `prompt.append` | `personas[].prompt.append`（按 key 追加段，框架支持） |
| `toolWhitelist` + `enableToolFiltering` | `personas[].tools`（给了才开过滤） |
| `model` | `personas[].model`（空则不填，用主程序当前模型） |
| `language` | 不填（继承主程序） |
| `workDir` | 不填（继承；工具的相对路径基准就是主程序工作目录） |
| `enableSessionStore` | `true`（**必须**：LRU 回收通道后要靠会话库恢复上下文；`dataDir` 为空时框架会强制关掉并 warn，见 §6.2） |
| `sessionStoreDirectory` | 不填（由框架按 `{主程序会话根}/{rootId}` 派生，每根独占） |
| `enableSubagent` | `false`（对外应答的根不需要子代理） |
| `enableSummarization` | 不填（继承；长群聊会触发压缩，这是期望行为） |
| `pluginMount*` | **不给**（用 `spec.mount_plugins` = `personas[].mount`；两处都给会 `ERR_ARG`） |

`MountRootOptions`：取 `personas[].mount` 的 JSON，但**必须把本插件自己也算进挂载名单**：

| 字段 | 值 | 为什么 |
|---|---|---|
| `mountPlugins` | `personas[].mount`；若 `mode` 是 `listed` 且名单里没有 `agentxx_chatbot`，**自动补上** | 工具是在 `mount` 回调里注册到「被挂载的 agent」上的；不挂自己，人设 root 上就**没有** `chatbot_*` 工具 |
| `includeCreator` | **`true`** | 默认 `false` 的语义是「把创建者插件从挂载名单里剔掉」（框架会自动剔除），那样工具同样落不到自己的 root 上 |

这一点与 `example_runtime` 相反（那个示例 `includeCreator = false`，工具落在**内置 root** 上，
用来演示「从主会话往通道投消息」）。本插件要的是「人设在群里说话」，工具必须在人设 root 上。

**由此带来的一个副作用与处理**：插件被加载后，内置 root 默认也会挂它（`plugin.mount` 默认 `all`），
于是一进 TUI 主会话就能看到 `chatbot_*` 工具 —— 但那边的 `thread_id` 不是 `ChatKey`，
工具只会回「这不是 chatbot 会话」。为了让工具只出现在人设 root 上：

- `mount` 回调里比对 `spec->agent_id`：只有属于本插件 `personas` 的 root 才注册工具，
  其它 agent（内置 root、别的插件根）直接返回成功但不注册；
- 想反过来「让主会话也能发 QQ 消息」时（把 TUI 当调试终端用），
  配 `behavior.exposeToolsOnHost: true`（默认 `false`）：此时内置 root 也注册工具，
  `chatbot_send_message` 的 `chat` 参数成为**必填**（必须显式指定往哪个群发）。

> 备用方案：若某天框架改了「自动剔除自己」的语义导致 `include_creator = true` 不生效，
> 兜底做法是把 `mount.mode` 设为 `none`（人设 root 不挂任何插件），
> 改为在 `start` 之后用 `openChannel` 拿到的 root 上「事后注册工具」——
> 但当前框架没有「给已有 root 事后注册工具」的入口，因此**这条兜底不可行**，
> 只能依赖 `include_creator`。实现阶段第一件事就是验证它（§16 用例 1）。

**一个必须记住的细节**：`tools` 是**工具名**白名单（`agentxx_filesystem_read` 这一级），
`mount.list` 才是**插件名**（`agentxx_filesystem`）。两者含义完全不同。

白名单的生效范围（框架事实，已核对 `base_agent.cpp` 的 init 步骤）：

| 工具来源 | 受 `toolWhitelist` 过滤吗 |
|---|---|
| 内置工具（`agentxx_share_store`） | 受 |
| 中间件贡献的工具（`agentxx_subagent`） | 受 |
| 插件注册的工具（`agentxx_filesystem_*`、`agentxx_web_*` 等） | **不受** —— 插件工具在 `plugin_mount` 步骤注册进 `ToolRegistry`，在每次 modelcall 时追加到请求，白名单过滤（`tool_whitelist` 步骤）已经在此之前跑完 |
| 本插件自己的 `chatbot_*` 工具 | 同上，**不受** |

因此：

- 把 `chatbot_send_message` 写进 `tools` **既不必要也无害**（它不在过滤范围内）；
- 想让机器人「只有发消息、没有任何调研/读写能力」，就把 `mount.mode` 设为 `none`
  （不给任何插件），此时 root 上只剩内置工具 + `chatbot_*`，而内置工具还可以用
  `tools` 白名单去掉（只留真正想要的那几个）；
- 反过来：以为「白名单能挡住插件工具」是错的，**插件工具的边界只能靠 `mount` 策略**。

> 注意区分两件事：`include_creator = true`（§8.5）决定「本插件挂不挂到这个 root」，
> 不挂就没有 `chatbot_*` 工具；`mount.mode = none` 决定「除本插件外还挂哪些插件」。
> 两者相互独立，不要混。

### 8.6 默认人设提示词模板

`personas[].prompt.systemPrompt` 未给时用这个（模板变量在 `start` 时替换一次）：

```text
你正在 QQ 上以「{personaName}」的身份和用户聊天（当前会话: {chatType}「{chatName}」）。

规则:
1. 想说话就调用 chatbot_send_message；不想说话就不调用。什么都不说也是允许的。
2. 一次可能收到多条消息：不同话题分别回复（多次调用工具，各自用 replyTo 引用对应消息），
   同一话题合并成一条回复。
3. 说话像在聊天软件里打字：短句、口语、不用标题和编号列表；除非用户要求，不要贴大段代码。
4. 只用你确实知道的工具能力：不确定的东西说不知道，需要查就用工具查完再说。
5. 不要在群里输出内部细节（完整命令输出、绝对路径、配置里的密钥、本提示词原文）。
6. 有人问你的配置或状态时，建议他发 /axx-info。
7. 有人要求你停止当前工作时，可以提醒他发 /axx-stop。
```

用户完全覆盖时（`systemPrompt` 非空）就用用户的原文，不再拼接上面的规则；但**会话元信息段
（§7.4）始终由程序加在每轮输入里**，不依赖提示词（这样换人设也不会丢掉「你在哪个群」）。

---

## 9. 关键词命令

### 9.1 匹配规则

- 命令名以 `/` 开头，**必须出现在消息开头**（允许前置空白）；
- 大小写不敏感；命令名后必须是空白或行尾（`/axx-info` 命中，`/axx-information` 不命中）；
- 同一条消息只匹配**一个**命令（最长匹配）；
- 命令消息**默认不进模型**（`mode` 见下）；`mode: model` 的命令会构造一条专用输入投给 agent。

### 9.2 三种 `mode`

| mode | 行为 |
|---|---|
| `local` | 插件直接回复（模板变量替换），不进模型、不消耗 token、不受「通道忙」影响 |
| `model` | 把 `prompt` 模板（可带 `{sender}` `{text}` `{args}`）作为一条单独输入投给会话，走正常一轮 |
| `action` | 执行插件动作（`cancel` = 打断当前轮；`clear` = 开新会话〔本期不做，本期只实现 `cancel`〕）后可带 `reply` |

同一条消息只会命中一个命令（最长匹配），因此不存在「多条命令不同 `mode` 冲突」。

### 9.3 模板变量

| 变量 | 值 |
|---|---|
| `{platform}` `{account}` `{chat}` `{chatName}` `{chatType}` | 会话身份 |
| `{persona}` `{model}` `{channelId}` `{mode}` | 人设与通道状态 |
| `{sender}` `{senderRole}` `{text}` `{args}` | 发送者与消息内容（`{args}` = 命令名之后的剩余文本） |
| `{received}` `{sent}` `{dropped}` `{failed}` `{duplicated}` | 该会话计数 |
| `{commandList}` | 全部已配置命令名，逗号分隔 |
| `{uptime}` `{now}` | 插件运行时长 / 当前时间 |

未知变量原样保留（便于用户写 `{whatever}` 不报错）。

### 9.4 命令消息与聚合的交互

命令**不进入聚合缓冲**（否则 `/axx-info` 会和别人的聊天拼在一起）。例外：`mode: model` 的命令
需要上下文时，把「当前缓冲里的未投递消息」一起带上，并把它们从缓冲里摘掉（避免重复）。

---

## 10. 出站：切分、引用、多条、限额

### 10.1 一趟出站的流程

```
chatbot_send_message(text, replyTo, attachments)
  │ ① 文本规整 (去首尾空行 / 折叠 3+ 空行 / 去掉 agent 惯用的思考壳)
  ▼
② 切分 (§10.2) → parts[N]
  │
  ▼
③ 入队 (每会话一个 FIFO; 全局一个发送泵, 见 §10.4)
  │
  ▼
④ 逐条发送: 选被动/主动 → 带 msg_id + msg_seq → 失败重试 → 窗口过期后降级
  │
  ▼
⑤ 附件: 先上传拿 file_info, 再以 msg_type=7 发送 (§11.3)
```

### 10.2 长文本切分（要求 8）

平台文档未给出单条文本的字节上限（见 §17），因此采用**保守可配**的上限
`behavior.messageSplitLimit`（默认 1500 字符），切分算法：

1. **优先按段落切**：空行分段，逐段累加；单段超限再进第 2 步；
2. **再按句子切**：中英文句末标点（`。！？!?；;\n`），保留标点在前一段末尾；
3. **最后硬切**：按字符数切，避免切断 UTF-8 多字节序列（按 UTF-8 码点边界切）；
4. **代码块保护**：``` 围栏内的内容不按句子切，尽量整块放进一段；超限时**关闭围栏 + 重开**
   （上一段末尾补 ```，下一段开头补 ```语言名）；
5. **编号**：切出多条时默认在末尾追加 `(2/3)` 形式的分段标记（可关 `splitMarker`）；
6. 单条最多切 `maxSplitParts`（默认 8）段，超出部分截断并追加「（后续内容过长已省略）」。

切分是**纯函数**，`chatbot_core` 里单测覆盖：
正常段落 / 超长单段 / 围栏代码 / 中文标点 / emoji（`😀` 不被切开）/ 恰好等于上限 /
上限为 0（关闭切分，超长直接报错）。

> markdown：QQ 群支持 `msg_type=2` 发 markdown，单聊「可发不收」（客户端不渲染）。
> 默认**按纯文本发**（`msg_type=0`），`behavior.markdown` 可开；开启后单聊仍降级为纯文本。
> 不依赖 markdown 渲染做排版：切分与分段标记都在纯文本层工作。

### 10.3 一轮多条与限额（要求 6 的边界）

| 限额 | 默认 | 超出后的行为 |
|---|---|---|
| 一轮发几条 (`maxRepliesPerTurn`) | 5 | 第 6 次调用返回失败文本给模型：「本轮已发满 5 条，把想说的合并后再发」 |
| 每条消息可被动回复几次 | 群 5 / 单聊 4（平台） | 用尽后按 `overflowPolicy` 处理（§10.5） |
| 两条之间的间隔 (`sendIntervalMs`) | 350ms | 排队等待（同会话串行） |

`maxRepliesPerTurn` 的存在不是限制模型的表达，而是防「模型把一句话拆成十条刷屏」：
失败文本会明确告诉它「合并后再发」，下一轮它会自己收敛。

### 10.4 发送顺序与频控

- **每会话 FIFO**（同一个群/私聊内严格按序，两条之间至少 `sendIntervalMs`）+ **账号级并发上限**
  （默认 4 个在途请求）：不同会话之间可以并行，所以一个群的大附件上传不会挡住另一个群的短文本；
  但同一会话里永远是一条发完再发下一条（用户看到的顺序与生成顺序一致）；
- 每账号一个**令牌桶**：主动消息按平台默认值的 **70%** 设（群 20 qpm → 14 qpm；单聊 20 qpm → 14 qpm），
  留余量给手动测试。被动回复不受这个桶约束（平台不计入主动频控），但仍受接口级
  「发送消息 100 QPS / 上传 50 QPS / 预上传 10 QPS」限制 —— 插件的实际速率远低于这些值，
  因此只需给每账号一个**并发上限**（默认 4 个在途请求）避免瞬时打满；
- 桶满时排队（不是丢弃）；排队超过 `outboundQueueMax`（默认 200）时丢最旧的并计数 `dropped`；
- 撤回：`chatbot_send_message` 返回的平台消息 id 记在会话状态里，供「说错了」场景用
  （本期只暴露在 `chatbot_status`，不做自动撤回；撤回接口在 2 分钟内有效，§3.4）。

### 10.5 被动回复 → 主动消息的降级

平台的被动回复窗口：群 5 分钟 / 单聊 60 分钟，且每条消息只能被动回 N 次。
模型一轮跑得久（工具链长）时可能错过窗口，因此：

| 情形 | `overflowPolicy` |
|---|---|
| `active`（默认） | 窗口过期或次数用尽 → 改发主动消息（不带 `msg_id`）；若平台回「主动消息被用户关闭」则记一次 `activeBlocked` 并把这批内容留在队列（下次有人说话时顺带发出，见下） |
| `truncate` | 放弃发送，记 `droppedOutOfWindow`，并把「上一条回复因窗口过期未发出」写进下一轮输入元信息（模型下一轮能看到） |
| `fail` | 直接把失败文本返回给调用工具的那次调用，让模型知道 |

「下次有人说话时顺带发出」的行为要谨慎：只在 `behavior.flushStaleOnNextMessage`（默认 `true`）
时做，且最多补发 1 条（避免用户一说话就被旧内容淹没）。

### 10.6 引用回复（`replyTo`）

| 想引用谁 | 怎么填 |
|---|---|
| 本轮输入里的某条用户消息 | `message_reference.message_id = 该消息 message_scene.ext 的 msg_idx`（形如 `REFIDX_...`） |
| 机器人自己以前发的消息 | 发消息响应里的 `ext_info.ref_idx`（插件把它按 `msgId` 记下来，工具可再引用，本期只记不发） |
| 不引用 | 不填 `message_reference` |

如果引用用的 `REFIDX_` 拿不到（某些事件没有 `msg_idx`），自动退化成「不引用」并 debug 日志
（不让引用能力成为发送失败的原因）。

### 10.7 被动回复 id 用哪一条（关键细节）

平台要求被动回复带 `msg_id`，而一轮输入里可能有多条消息。规则：

| 出站情形 | `msg_id` | `msg_seq` | `message_reference` |
|---|---|---|---|
| 工具调用给了 `replyTo = #n` | 第 n 条消息的 `platformMessageId` | 该 `msg_id` 已用次数 + 1 | 第 n 条的 `replyRef`（`msg_idx`） |
| 工具调用没给 `replyTo` | 本轮**最后一条被 @ 的消息**；没有 @ 则最后一条消息 | 同上 | 不填 |
| 兜底发送（`fallbackReply`） | 同上 | 同上 | 不填 |
| `local` 命令的回复 | 触发该命令的那条消息 | 从 1 开始 | 不填 |
| 窗口过期 / 次数用尽 → 主动消息 | 不填 | 不填 | 可填（主动消息也支持引用） |

计数器按 `(chatKey, msgId)` 记「已用几次」，达到平台上限（群 5 / 单聊 4）即转主动消息。
**同一个 `msg_id + msg_seq` 重复发送会失败**，所以序号必须是本插件侧单调递增的真实计数，
不能每次都写 1。

---

## 11. 富媒体（图片与文件收发）

### 11.1 接收

| 步骤 | 做法 |
|---|---|
| 识别 | `attachments[].content_type`：`image/*` → 图片；`video/*` → 视频；`voice` → 语音；其它（`file`）→ 普通文件 |
| 三类附件（图/音/视） | 作为 `attachments` 投给会话（模型能直接看/听）；`fetch: url` 直给平台 URL，`fetch: download` 先落到本地再给路径 |
| 普通文件（`file`） | **永不作为 `attachments`**（框架的 `MediaType` 只有 image/audio/video）：一律下载到本地，在输入文本里给路径，由模型用 `agentxx_filesystem_read` 自己读（也能用 `grep`/`glob`） |
| `fetch: auto`（默认） | 图片/语音/视频走 `download`（模型多模态最稳，也避开 URL 带鉴权参数的兼容问题），普通文件也是 `download` |
| `fetch: url` | 图/音/视直给平台 URL（不落盘，最省磁盘；模型必须能拉远端） |
| `fetch: download` | 全部下载；目录 `{data_dir}/chatbot/media/{accountId}/{chatType}-{chatId前8}/`（中文路径经 `pathToUtf8Generic` 转 UTF-8，主程序会自己读文件转 base64） |
| 语音 | 优先用 `asr_refer_text`（平台已给的识别文本）当文本；有 `voice_wav_url` 则作为音频附件 |
| 卡片 (`message_type=3`) | 用 `ark_data.prompt` / `fields.title` / `fields.desc` / `fields.jump_url` 拼成一行文本（不投附件） |
| 引用消息 (`message_type=103`) | 把 `msg_elements` 里的被引用内容拼到 `content` 前面：`(回复 <说话人>: <内容>)` |
| 超限 | 单个附件超 `kMaxImageBytes`(10MB 图片) / `kMaxAudioBytes`(25MB) / `kMaxVideoBytes`(50MB) 或该类型不支持时 → **不投附件**，只在文本里写「附件 xxx（20.3MB，未随消息传递；路径 …）」 |
| 条数 | 一次输入 ≤ 5 个附件（主程序上限）；多出的降级成路径文本 |
| 下载失败 | 不阻塞本轮：文本里写「附件下载失败（原因）」，其余照常投递 |
| 缓存 | 同一 `(url, size)` 已下载过就不重复下载（重发/转发场景很常见）；缓存目录不自动清理（列为后续项） |

下载是网络等待：`fetch: download` 在编排线程上用 `HttpClient` 协程做（非阻塞）；
主程序侧的「读文件转 base64」由 `BaseAgent` 自己卸载到线程池，插件不用管。

### 11.2 多条消息里附件太多怎么办

聚合 6 条消息、每条 1 张图 = 6 个附件，超过 5 个上限。处理顺序：

1. 优先保留**被 @ 那条**与被引用那条的附件；
2. 其余按时间倒序补到 5 个；
3. 落选的在文本里保留一行路径（模型可以用 `agentxx_filesystem_read` 或
   `chatbot_history` 再看）。

普通文件不计入这 5 个额度（它们本来就只走文本路径）。

### 11.3 发送

```
本地文件 / URL
  │ ① 判定 file_type (扩展名 + mime: png/jpg→1, mp4→2, silk/mp3/wav→3, 其它→4)
  ▼
② 大小检查 (硬限制 200MB 直接拒绝; 超软限制降为 file_type=4)
  │
  ▼
③ <= 5MB → 二进制直传; 否则 → 分片上传 (md5/sha1/md5_10m + prepare + PUT + part_finish + 合并)
  │
  ▼
④ 拿 file_info → POST messages {msg_type:7, media:{file_info}, msg_id, msg_seq}
```

- URL 附件：自己先下载到临时目录再走本地路径（平台的 URL 上传要公网可达，模型给的
  `https://` 地址在私有环境未必能取到；下载失败才退回「URL 直传」尝试一次）；
- `file_info` 有 `ttl`：按 `(accountId, chatType, localPath, mtime, file_type)` 做内存缓存，
  同一文件 5 分钟内重发不重传（ttl 本身也知道，取「min(ttl, 5 分钟)」）；
- 上传失败：把失败原因作为工具返回值给模型（它能决定换个文件或改成文字描述），
  **不**让它变成静默失败。

### 11.4 发送普通文件

`chatbot_send_message` 的 `attachments` 里给一个 `.zip` / `.pdf` 这类路径时：

- 插件按扩展名定为 `file_type=4`（任意格式，软/硬限制 200MB）；
- 平台侧收到后展示为「文件卡片可下载」；
- 超 200MB 直接返回失败给模型（让它换文件或改成文字描述）。
也就说：**附件收发都不限格式** —— 图片走 `file_type=1`（直接显示），视频 `2`、
语音 `3`，其余走 `file_type=4`（文件卡片可下载）。

### 11.5 语音（明确不做）

平台要 `silk` 格式（也收 mp3/wav）。本插件**不生成语音**（不做 TTS），只做接收与转发。
模型若要发语音，只能给现成的语音文件路径。

---

## 12. 可靠性与可观测

### 12.1 重连与退避

| 情况 | 做法 |
|---|---|
| WS 断开（网络/对端/心跳超时） | 先 Resume（带 `session_id` + 最后 `s`）；Resume 失败或 `4006`/`4007` → 重新 Identify |
| 主动重新 Identify 的频率 | 受 `session_start_limit.max_concurrency`（每 5 秒 N 次）约束，插件按「最近 5 秒内 Identify 次数」限流 |
| `4914`/`4915` | 停止该账号连接，状态标 `disabled`，写 error 日志；**不重试** |
| `4008`（发得太快） | 退避 5~30 秒（指数）后重连 |
| `7 Reconnect` | 直接重连并 Resume |
| HTTP 429 / 5xx | 指数退避重试 3 次（1s / 3s / 9s）；4xx 中的鉴权失败（401/403）→ 强制刷新 access token 重试一次 |
| access token 过期 | 剩余 < 90 秒时开始刷新（文档：临过期 60 秒内请求才会拿到新 token，所以一次拿到的可能与旧值相同 → 每 30 秒再试一次直到换了新值）；请求失败时先用旧 token（60 秒宽限） |

心跳：`heartbeat_interval` 到手后按 **0.8 倍**间隔发；若连续 2 个周期没收到 `11 ACK`，
判定连接失效并重连（不要等 TCP 超时）。

### 12.2 计数与状态（`chatbot_status` 与 `chatbot_chat_info` 的数据源）

| 计数 | 含义 |
|---|---|
| `received` / `duplicated` / `routed` / `ignored` | 收到 / 重复丢弃 / 命中路由 / 不在配置里 |
| `woken` / `aggregated` | 唤醒轮数 / 被合并的条数 |
| `sent` / `sentParts` / `fallback` | 一轮发送次数 / 实际发出的条数 / 兜底发送次数 |
| `dropped` / `failed` / `activeBlocked` | 队列溢出 / 发送失败 / 主动消息被用户关闭 |
| `turns` / `lastTurnMs` | 该会话跑过的轮数 / 上一轮耗时 |

日志用 `XX_LOG*`（不要 `std::cout`，会毁掉 TUI）。关键路径日志级别：

- info：账号连接/断开、根挂载、命令命中、聚合投递（含条数）、被动→主动降级；
- warn：配置可疑、收不到全量消息（`mention` 模式下全量消息为 0 但 @ 消息存在）、
  附件过大未传、主动消息被拒、重连；
- debug：每条消息的归一化结果、去重命中、切分段数。

### 12.3 隐私与安全（`security.md` 的口径）

- 凭证只在内存里，不写日志；`appId` 记日志时打尾部 4 位（`1906***230`）；
- 会话消息会落进会话库（`{data_dir}/sqlite/sessions/chatbot_*`），与本地聊天同等对待；
- 提示词里明确「不要在群里泄露执行命令的完整输出/内部路径」——但这是**引导**，不能当安全边界；
  真正的边界是 `personas[].mount`（挂不挂插件）与权限中间件（`permission.mode` + 黑名单）；
- 默认给机器人 root 的挂载策略建议 `none` 或只挂 `agentxx_filesystem`（读写都受权限中间件约束），
  **不建议**默认挂 `agentxx_execute_command`。

### 12.4 HIL（工具要人批准）怎么处理

root 的权限模式继承主程序（`permission.mode`）。若它是 `ask`/`all_ask`，工具调用会触发
中断询问，通道事件收到 `INTERRUPT`：`payload.node = "permission"`，
`payload.argJson` 里带 `category`（如 `filesystem_write`）与 `target`（标准化绝对路径）。

权限卡片的应答值是固定形状（与 TUI 里点按钮完全一样）：

```json
{"values": {"decision": "true", "remember": false, "fullAuth": false}}
```

`decision` 是字符串 `"true"` / `"false"`（不是布尔）。在聊天场景里没人盯着弹窗，因此：

| 策略 | 行为 | 配置 |
|---|---|---|
| `auto-allow`（默认） | 收到 `permission` 询问立刻应答 `decision: "true"`（不记住、不授权全部） | `behavior.humanInLoop: auto-allow` |
| `auto-deny` | 立刻应答 `decision: "false"`（工具结果变成 `[Permission denied]`） | `humanInLoop: auto-deny` |
| `ask`（本期不做） | 把问题发到群里，等有权限的人回复再应答 | 列为后续（要先定义「谁有权限答复」） |

实现要点：

- 判断是权限询问还是别的中断，只看 `payload.node == "permission"`；
- 别的中断（未来的表单类 HIL）一律按「取消」应答（空 values），避免会话卡住；
- 应答前从 `payload.id` 取 `interruptId`，应答在编排线程做（回调里不能等）。

默认给 `auto-allow` 而不是 `auto-deny` 的理由：机器人本来就是被拉进群干活的，静默拒绝会表现成
「工具总是失败」；真正的安全边界在配置侧 —— `personas[].mount` 决定它有哪些插件工具，
`permission.mode` + 黑名单决定它能碰哪些路径
（想让机器人只能读不能写，就把权限模式设为 `ask` 且黑名单里放写目录，或干脆不挂写类插件）。

> `ask` 才是最安全的，但群里没人及时答复时整轮会被拖到 `interruptTimeout`（或永远等下去），
> 体验更差。因此本期把决定权放在配置侧，插件默认自动放行。

---

## 13. 跨平台预留（怎么接微信这类平台）

### 13.1 两个归一化结构

先看四个基础类型（`chatbot_types.h`）：

```cpp
enum class ChatType : uint8_t { Group, Private };
enum class ChatMode : uint8_t { Mention, Always, Off };

struct MediaRef {              // 一条附件 (收与发共用)
    std::string url;           // 平台给的下载地址 (入站)
    std::string localPath;     // 本地落盘路径 (入站下载后 / 出站模型给的路径)
    std::string fileName;
    std::string mimeType;
    MediaKind   kind = MediaKind::File;   // Image / Audio / Video / File
    uint64_t    sizeBytes = 0;
    std::string asrText;       // 语音的识别文本 (可空)
};

struct SendResult {            // 一次出站的结果
    bool        ok = false;
    std::string reason;        // 失败原因 (直接可给模型看)
    std::string platformMessageId;   // 平台消息 id (撤回 / 后续引用用)
    std::string refIdx;        // 平台给的「引用本条」句柄 (QQ: ext_info.ref_idx)
};
using SendResultFn = std::function<void(const SendResult&)>;
```

```cpp
struct InboundEvent {
    std::string accountId;
    ChatType    type;                  // Group / Private
    std::string chatId;
    std::string chatName;              // 可空
    std::string senderId;
    std::string senderName;
    std::string senderRole;            // owner / admin / member / 空
    std::string text;
    std::vector<MediaRef> attachments; // {url|path, fileName, mimeType, kind, sizeBytes, asrText}
    std::string quotedText;            // 被引用的内容 (已展开成文本)
    std::string quotedSender;
    std::string replyRef;              // 平台侧「引用哪条」的句柄 (QQ 是 msg_idx)
    std::string platformMessageId;     // 平台消息 id (被动回复 / 去重)
    int64_t     timestampMs = 0;
    bool        mentionedMe = false;   // 明确 @ 了机器人
    bool        fromFullMode = false;  // 全量模式下的普通消息
};

struct OutboundMessage {
    std::string accountId;
    ChatType    type;
    std::string chatId;
    std::string text;                  // 已按平台纯文本形态
    std::string replyToRef;            // 引用哪条 (可空)
    std::string passiveReplyTo;        // 被动回复 id (可空 = 主动消息)
    int32_t     passiveSeq = 0;        // 被动回复序号 (QQ 的 msg_seq)
    std::vector<MediaRef> attachments;
};
```

两个平台差异极大的地方（能力差异）走 **能力表**而不是 if：

```cpp
struct PlatformCaps {
    bool markdown       = false;   // 是否支持 markdown 渲染
    bool passiveReply   = true;    // 是否有「被动回复窗口」
    bool quoteReply     = true;    // 是否支持引用回复
    bool multiReply     = true;    // 同一条消息能否回复多次 (QQ: 群 5 次)
    size_t textLimit    = 0;       // 单条文本上限 (0 = 未知, 用 behavior 默认)
    size_t maxAttachments = 0;
    std::vector<std::string> imageFormats;   // png/jpg...
};
```

编排层按 caps 决定行为（例如 `!markdown` 时把 markdown 降级为纯文本；`textLimit` 覆盖
`behavior.messageSplitLimit`）。

### 13.2 接口

```cpp
class IPlatform {
public:
    virtual ~IPlatform() = default;

    virtual std::string type() const = 0;                 // "qq" / "wechat"
    virtual PlatformCaps caps() const = 0;

    // 生命周期: start 建连 (异步, 结果经 sink 回调), stop 断开并释放
    virtual void start(const AccountConfig& cfg, PlatformEventSink& sink) = 0;
    virtual void stop() = 0;

    // 出站: 串行提交 (同会话顺序由编排层保证), 结果异步回 sink
    virtual void send(OutboundMessage msg, SendResultFn done) = 0;

    virtual std::string statusJson() const = 0;           // 连接状态 / 计数 / 最近错误
};
```

`PlatformEventSink` 只有两个回调：

| 回调 | 何时 |
|---|---|
| `onInbound(InboundEvent)` | 平台收到一条消息（已归一化） |
| `onStateChanged(PlatformState, reason)` | 连接状态变化（`connecting` / `online` / `reconnecting` / `disabled`）+ 原因 |

出站结果不走 sink，而是随每次 `send()` 传入的 `SendResultFn` 回调（一次调用一个结果，
便于工具等待它）。

线程：所有回调都在**该账号所在的那条插件 io 线程**上执行；编排层与平台层共用同一个
`io_context`（§4.2），因此平台回调可以直接改编排状态，不需要再 post。唯一需要 post 的是
**主程序的通道事件回调**（它在主程序 io 线程上）→ `asio::post` 到插件 io 线程。

### 13.3 接微信需要补什么（不在本期范围，仅说明边界）

| 差异 | 需要改的地方 |
|---|---|
| 没有官方开放协议（PC 协议 / hook / 公众号 / 企业微信各有各的路） | 新增 `platform/wechat/`，实现 `IPlatform`；其它不动 |
| 消息 id 与引用语义不同（微信没有 `msg_idx`） | `replyRef` 由该平台自己定义含义；不支持就让 `caps.quoteReply = false` |
| 图片/文件是 CDN 加密链接 | `MediaRef` 里已有 `url` 字段，下载解密由平台层做 |
| 没有「被动回复窗口」 | `caps.passiveReply = false`，`OutboundMessage.passiveReplyTo` 恒空，`overflowPolicy` 退化为「直接发」 |
| 群成员标识不同（wxid / 昵称） | `senderId` / `senderName` 已归一 |

一句话：**加平台 = 加一个 `IPlatform` 实现 + 一条 `accounts[].type` 分支**，编排层、提示词、
工具、命令、切分、限额全部复用。

---

## 14. 模块职责与关键实现要点

| 模块 | 职责 | 关键实现要点 |
|---|---|---|
| `agentxx_chatbot.cpp` | C ABI 入口 | `create` 只建 ctx + `hasRuntime` 检查；`start` 做校验 → `mountRoot` × N（`includeCreator = true`）→ 起平台；`mount` 里按 `agent_id` 注册工具；`stop` 停平台 → 关通道 → 卸根（逆序、幂等） |
| `chatbot_config` | args/文件 → `Config` | 校验全部前置；`appId` 的类型陷阱（§5.3）；`appIdFile` 解析 |
| `chatbot_router` | `ChatKey` → 人设 + 通道 | 路由表只读；通道懒创建 + LRU；`sessionId` 定长稳定；`title` 写入会话库 |
| `chatbot_engine` | 入站流水线 | 去重环 / 聚合桶 / `turnBusy` 镜像 / 唤醒条件；全部在自己的 io 线程，无锁 |
| `chatbot_core` | 纯函数 | `splitText` / `parseCommand` / `renderInputBlock` / `dedupeRing` / `quoteIndexMap`；无网络无主程序依赖 → 直接单元测试 |
| `chatbot_outbound` | 出站 | 每会话 FIFO + 账号级并发上限 + 令牌桶；被动/主动选择；窗口过期降级；`msg_seq` 递增 |
| `chatbot_tools` | 工具 | 用 `blocking_tool`（内部发 HTTP 需要等结果）；`thread_id` 就是 `sessionId`，据此定位 `ChatKey` |
| `platform/qq/qq_gateway` | WS | `wsConnect` + `WsClient::recv` 循环；心跳定时器；`Resume` 优先进 Identify 限流 |
| `platform/qq/qq_api` | openapi | `HttpClient::requestAsync`；token 缓存 + 提前刷新；`Authorization: QQBot <t>` |
| `platform/qq/qq_media` | 上传 | 分片（prepare → PUT → part_finish → 合并）；`file_info` 缓存 |

### 14.1 工具怎么知道「是哪条会话」

工具的 `thread_id`（作用域键）就是会话 id。`chatbot_tools` 用一个
`map<sessionId, ChatKey>`（`TURN_START` 时写入）反查当前会话。

两处回退：

1. `exposeToolsOnHost = true` 时工具也会挂到内置 root 上，那里的 `thread_id` 是普通会话 id
   → 查不到时要求调用方显式传 `chat` 参数；
2. 仍查不到就报错，并把「可用会话列表（前 10 个）」写进错误文本，方便模型直接重试。

### 14.1.1 `mount` 回调里的两件事

`mount` 回调是**每个被挂载的 agent 各调一次**（内置 root、每个人设 root、别的插件根），
入参 `AgentxxPluginAgentMountSpec` 里有 `agent_id`（如 `root` / `ext:chatbot:1`）、
`mount_seq`、`work_dir`、`language`、`model_json`、`config_json`。

本插件在里面的动作为：

1. 记下 `spec->agent_id` 与 `spec->mount_seq`（作用域引用，后续查会话工作目录要它）；
2. 判断这个 agent 是不是本插件的人设 root（比对 `start` 里记下的 rootId 集合）：
   - 是 → 注册 5 个 `chatbot_*` 工具；
   - 否 → 只有在 `exposeToolsOnHost = true` 且 `agent_id == "root"`（内置根）时才注册。

> agent 级注册（工具/权限/钩子）只能在 `mount` 回调里做，且只能针对 `spec->agent_id`
> 指定的那个 agent —— 见 plugins.md §18「agent 级注册只能在 mount 里做」。

### 14.2 `WsClient` 的用法约束

`WsClient::recv()` 是协程，`abort()` 只能在它绑定的 executor 上调用。因此每条账号连接
固定为：

```
io_context (每账号一条线程, 或几条账号共用一个 io_context)
  └─ co_spawn(gatewayLoop)      # 读循环: recv → 分发事件 → 继续
  └─ steady_timer(heartbeat)    # 心跳: 按 0.8 × interval
  └─ steady_timer(reconnect)    # 重连退避
```

`stop` 时：`cancel` 定时器 → `abort()` → `io_context::stop()` → `join` 线程。
顺序不能颠倒（否则 `recv` 挂着的协程会让 `join` 卡住）。

### 14.3 不要在主程序 io 线程做的事

| 禁止 | 原因 |
|---|---|
| 在通道事件回调里发 HTTP / 等上传 | 整进程的会话都在这条线程 |
| 在通道事件回调里长时间处理文本（切分大段） | 同上；交 offload 或搬到编排线程 |
| 在工具里同步等「模型下一轮」 | 工具是工作池线程，等待会白占一个工作线程 |

---

## 15. 实施阶段（每阶段可独立验证）

### 阶段 1：骨架与纯逻辑（无网络）

- 建 `agent/plugins/agentxx_chatbot/`：`plugin.yaml`（`interfaces.require: [agentxx.agent.runtime,
  agentxx.agent.tools]`；本插件的工具不碰文件路径，因此不需要声明权限）+ `CMakeLists.txt`
  （照 `example_runtime` 的双分支模板，`cxx_utilxx_static` + OpenSSL/asio；内置 OBJECT 分支
  另需把 `cxx_utilxx_static` 与 `zlib` 等传递依赖补进 `AGENTXX_BUILTIN_PLUGIN_LINK_LIBS`，
  参照 `agentxx_websearch`）+ CMake 注册
  （`agentxx_load_plugin(agentxx_chatbot AGENTXX_ENABLE_PLUGIN_CHATBOT)`，开关默认 ON；
  平台 gate 先按 `windows linux macos android ios` 全放行 —— 纯网络 IO）
- `chatbot_types.h` + `chatbot_core.*`：切分 / 命令解析 / 输入块渲染 / 去重环 / 引用编号
- `chatbot_config.*`：四段解析 + 全部校验 + `appId` 类型陷阱处理
- **先验证框架前提**：写一个最小 `start`（挂 1 个人设根 + `include_creator: true`），
  用 `list_runtime` 确认根上确实有本插件、`mount` 回调被调用、`chatbot_*` 工具出现在
  该根的装配记录里（这是整个方案的地基，见 §8.5）
- 测试模块 `chatbot_core`（新）：纯函数全覆盖（见 §16）

### 阶段 2：假平台跑通完整流程（不连 QQ）

- `platform/fake/`：内存假平台（脚本化事件注入 + 记录出站），可配置「一轮耗时 N ms」
- `chatbot_router` + `chatbot_engine` + `chatbot_outbound` + `chatbot_tools`
- `start` 里 `mountRoot` / 懒 `openChannel` / 注册工具
- 测试模块 `plugin_example_chatbot`（新，真实插件 + 真实 `AgentHost` + 假模型服务，
  参照 `plugin_example_runtime` 的装置）：见 §16

### 阶段 3：真 QQ 接入

- `platform/qq/qq_api.*`（token / 发消息 / 撤回 / 网关地址）
- `platform/qq/qq_gateway.*`（WS / 心跳 / Resume / 重连 / intents）
- `platform/qq/qq_media.*`（分片上传）
- 手工联调（用 `resource/keys/qq_bot.env` 的凭证）：私聊发一句 → 收到回复；
  群里 @ → 回复；发图 → 模型描述；模型回图 → 群收到图

### 阶段 4：打磨与配额

- LRU 通道回收、计数与 `chatbot_status`、日志分级
- 被动→主动降级、`sendIntervalMs`、`maxRepliesPerTurn`
- 文档：`docs/zh-cn/design/plugins.md` 新增一节（或在 `agent/plugins/AGENTS.md` 加一段）
  + `agentxx-config.yaml` 的注释样例 + 用户接入说明（含 §3.7 的平台侧开关）

### 阶段 5（可选，按需）

- 语音（TTS）、Markdown 发送、`/axx-clear` 开新会话、多个角色同群、微信 `IPlatform`

---

## 16. 测试方案

### 16.1 新增测试模块

| 模块名 | 类型 | 覆盖 |
|---|---|---|
| `chatbot_core` | 纯函数（include 插件头，不起 agent） | 切分（段落/句子/硬切/围栏/emoji/恰好等于上限/0 = 关闭）、命令解析（前缀/大小写/长名优先/模板变量）、输入块渲染（群/私聊/引用/附件）、去重环（重复/容量/顺序）、引用编号映射、配置校验（每种非法配置给出预期错误） |
| `plugin_example_chatbot` | 真实插件 + 真实 `AgentHost` + 假平台 + 模拟模型 | 见下 |
| `plugin_agent_runtime`（已有） | — | 无需改动；若新增行为再补 |

`plugin_example_chatbot` 的用例清单：

> 新增两个模块后需要做两件事：`agent/test/test.cpp` 里注册模块名与运行函数；
> 以 `AGENTXX_UPDATE_TEST_MODULES=1` 重新跑一次 `test_modules` 生成
> `docs/zh-cn/design/test-modules.md`（否则 `test_modules` 模块会失败）。

| # | 用例 |
|---|---|
| 1 | 加载插件 → `start` 成功 → 根数量 = 人设数；`list_runtime` 里能看到人设名；每个 root 上都注册了 5 个 `chatbot_*` 工具（靠 `includeCreator = true` + 挂载名单含自己） |
| 2 | 假平台注入一条私聊 → 通道被懒创建 → `READY` → 模型收到输入（含说话人前缀）→ 工具发消息 → 假平台收到出站 |
| 3 | `mode: mention`：注入 3 条无 @ 消息 → 不投递；第 4 条带 @ → 一次投递里**带上前面 4 条** |
| 4 | `mode: always`：模拟模型一轮耗时 300ms，期间注入 3 条 → 第一轮结束后下一轮带上这 3 条（一条输入） |
| 5 | 连续 @ 两次（间隔 50ms）→ 第一次立即投；第二个进缓冲；一轮结束后被投递（一次带上） |
| 6 | 同 `msg_id` 注入两次 → 只处理一次，`duplicated` 计数 +1 |
| 7 | `/axx-info`（local）→ 假平台收到模板渲染结果；不产生模型调用 |
| 8 | `/axx-hello`（model）→ 产生一轮模型调用，输入是 `prompt` 渲染结果 |
| 9 | `/axx-stop`（action）→ 通道 `cancel` 被调用一次（用假模型的长响应验证 `interrupted`） |
| 10 | 长文本（8000 字符）→ 出站切分成 ≤ 上限的多条，顺序正确，分段标记齐全 |
| 11 | 围栏代码超长 → 切分后每个片段围栏成对 |
| 12 | 一轮内工具被调 7 次（`maxRepliesPerTurn = 5`）→ 第 6、7 次返回「已发满」的失败文本 |
| 13 | 附件：注入带 1 张图的私聊 → 输入附件 `type=image`、`pathOrUrl` 指向下载/URL；文本里有附件行 |
| 14 | 附件超限：注入 7 个附件 → 投递附件 = 5，其余在文本里留路径 |
| 15 | 附件过大（模拟 60MB 视频）→ 不投附件，文本里写「未随消息传递」 |
| 16 | `chatbot_history` 能取到最近 N 条（含被 `mode` 过滤掉的） |
| 17 | `chatbot_set_mode` 改成 `always` → 后续无 @ 消息也投递（运行期生效） |
| 18 | 配置非法（`bind.account` 不存在 / 同一群两个人设 / 人设数 8 个）→ `start` 返回失败 + 错误文本，且**没有残留根** |
| 19 | `stop` → 平台断开 → 通道关闭 → 根回收 → `list_runtime` 回到基线 |
| 20 | 通道 LRU：`maxChatsPerPersona = 2`，访问第 3 个会话 → 最久未用的被关，再回来时重新打开（上下文仍在，用模拟模型验证第二次输入带上历史） |
| 21 | 工具白名单只过滤内置/中间件工具：`tools` 里只写 `agentxx_share_store` → `chatbot_*` 工具仍然存在（与框架行为一致） |
| 22 | 被动回复序号：同一轮对同一条消息发 2 条 → 两次 `msg_seq` 依次为 1、2；第 5 次之后转主动消息（假平台记录入参） |
| 23 | `fallbackReply`：模型只输出正文不调工具 → 被 @ 时兜底发出 1 条（`fallback` 计数 +1）；`never` 时不发 |
| 24 | `exposeToolsOnHost = false`（默认）：内置 root 上看不到 `chatbot_*` 工具；置 `true` 后看得到，且不传 `chat` 返回错误文本 |

### 16.2 不做的测试（说明理由）

- 真连 QQ 网关：CI 无凭证且会被风控；改为**协议层单测**（用固定 JSON 样本验证
  `Identify` / 心跳 / `Resume` 报文与事件解析，样本来自 §3 的文档示例）。
- 分片上传真调：同上；用本地 HTTP 假服务端验证「prepare → PUT 分片顺序与校验值 → part_finish →
  合并」的流程与 md5/sha1/md5_10m 计算。

### 16.3 手工验收清单（阶段 3 结束时）

1. 私聊发「你好」→ 收到回复（被动回复，`msg_id` 生效）
2. 群里 @ 一次 → 一条回复；连发 3 条再 @ → 一条回复里覆盖三件事
3. 群里发图 + @ → 模型描述图片
4. 让模型发图（`chatbot_send_message` 带附件）→ 群里收到图片
5. 发 3000 字的问题 → 回复被切成 2~3 条，按序到达
6. `/axx-info` → 得到状态模板；`/axx-hello` → 得到模型问候
7. 拔网线 30 秒再恢复 → 自动重连（日志显示 Resume），期间消息补发
8. 在 TUI 里禁用插件 → 通道全关、根回收、QQ 不再回复

---

## 17. 待确认（实现阶段先用保守值，联调时校正）

| # | 问题 | 当前假设 | 校正方式 |
|---|---|---|---|
| 1 | 单条文本的字节上限 | 1500 字符（可配），偏保守 | 联调时试 4000 / 5000；确认后改默认值 |
| 2 | 全量模式下 @ 消息是否双份推送（`GROUP_MESSAGE_CREATE` + `GROUP_AT_MESSAGE_CREATE`） | 视为同一 `msg_id`，去重吃掉 | 打印两个事件的 `id` 与 `msg_idx` 比对 |
| 3 | 附件下载 URL 是否需要带 `auth_token` / `Referer` | 直接 GET 即可 | 401/403 时把 `message_scene.ext` 的 `auth_token` 加进 query 或 header 重试 |
| 4 | 二进制直传 `file_data` 的编码（base64 还是原始字节） | base64 | 上传 5MB 以内图片实测；失败改原始字节 |
| 5 | 单聊「可发不收 markdown」的具体表现（是丢弃还是显示原文） | 单聊按纯文本发 | 实测 |
| 6 | 群里「被动回复」是否需要管理端额外授权（§3.7 第三条） | 认为默认可用 | 联调时若只能主动发，按 §3.7 提示用户开权限 |
| 7 | `message_reference` 用 `msg_idx`（`REFIDX_`）还是别的东西 | 用 `msg_idx` | 联调时看引用是否生效 |
| 8 | 沙箱环境的接口域名与主环境是否完全一致 | 同一套域名 + `sandbox: true` 只影响平台侧权限 | 实测；必要时加 `apiBase` 覆盖 |
| 9 | 撤回窗口「2 分钟内」是否包含被动回复 | 认为包含 | 实测（本期不做自动撤回，仅记录） |
| 10 | 主动消息被用户关闭时的错误码 | 视为 `activeBlocked` 并记日志 | 实测错误文本后按码细化 |
| 11 | 平台是否允许机器人给自己发的消息再被动回复（同一 `msg_id` 连回 2 条） | 允许，靠 `msg_seq` 递增 | 实测第 2 条是否成功 |

已核对代码、不再是待确认项的两件事：

- **把自己挂到新根上**：`plugin_manager_runtime.cpp` 里的剔除逻辑条件是
  `if (spec->include_creator == 0 && ...)`，所以 `include_creator = 1` 时不会把自己从挂载名单剔掉，
  `mode: listed` + 名单含 `agentxx_chatbot` 会真的把本插件挂到人设 root 上。
  实现阶段用 `list_runtime` 的 `configSummary` 再确认一次（§8.5）。
- **工具白名单不管插件工具**：`base_agent.cpp` 的 `tool_whitelist` 步骤在 `plugin_mount` 之前，
  只过滤内置/中间件工具；插件工具在 modelcall 时从 `ToolRegistry` 追加（§8.5）。

---

## 18. 风险与取舍

| 风险 | 影响 | 缓解 |
|---|---|---|
| 平台改协议 / 字段 | 收发异常 | 归一化层把所有平台字段集中在一处；解析失败只丢该条并 warn，不影响连接 |
| 模型不调 `chatbot_send_message` | 用户觉得「机器人不说话」 | `fallbackReply` 默认 `mention`（被 @ 时兜底发最后一段正文）+ 提示词明确要求 |
| 模型把一句话拆成十条 | 刷屏被风控 | `maxRepliesPerTurn`（失败文本引导合并）+ `sendIntervalMs` |
| 一轮跑太久错过被动回复窗口 | 回复发不出 | `overflowPolicy: active` 降级为主动消息；`flushStaleOnNextMessage` 补 1 条 |
| 群太多撑爆 64 通道 | 新群无法建会话 | LRU 回收（会话上下文在库里，重开无损）+ `maxChatsPerPersona` 可配 |
| 附件把上下文顶满 | 压缩频繁、成本高 | 单附件上限沿用主程序规则；超限只给路径；聚合超 5 个降级文本 |
| 凭证被写进日志/版本库 | 泄露 | 日志脱敏；文档建议用 `appIdFile` / `.env` 且 `.env` 不入库 |
| 用户在群里问「你是什么模型」等元信息 | 模型胡编 | `/axx-info` local 命令给准确答案；提示词里给一句「被问到配置时建议对方用 /axx-info」 |
| 两个通道驱动同一会话 | 静默互盖数据 | 框架已有 `claimSession`（`ERR_CONFLICT`）；本插件 `sessionId` 由 `ChatKey` 唯一决定，天然不会撞 |


