# agentxx_chatbot 实施记录

对应设计文档：[plan.md](plan.md)

## 已完成

### 阶段 1：骨架与纯逻辑（完成）

- 目录 `agent/plugins/agentxx_chatbot/`，注册到 `agent/plugins/CMakeLists.txt`（`agentxx_load_plugin`）
  与顶层 `agent/CMakeLists.txt`（`AGENTXX_ENABLE_PLUGIN_CHATBOT`，默认 ON）。
- `plugin.yaml`：`interfaces.require: [agentxx.agent.runtime, agentxx.agent.tools]`。
- `chatbot_types.{h,cpp}`：`ChatType` / `ChatMode` / `MediaKind` / `PlatformState`、`MediaRef`、
  `InboundEvent`、`OutboundMessage`、`SendResult`/`SendResultFn`、`ChatKey`（`key()` / `parseChatKey`）、
  `PlatformCaps`。
- `chatbot_core.{h,cpp}`：UTF-8 计数与截断、文本规整、长文本切分（段落 → 句子 → 硬切，
  围栏保护、分段标记、`maxParts`）、命令解析（最长匹配、大小写不敏感）、模板渲染、
  输入块渲染（`[#n] 说话人(角色) @机器人:`）、引用编号映射、去重环。
- `chatbot_config.{h,cpp}`：四段配置（accounts / personas / behavior / commands）解析 + 校验；
  `appId` 接受字符串与整数、浮点拒绝并提示 `appIdFile`；`appIdFile` / `appSecretFile`（.env 形态）；
  `mergeConfigFile`（JSON 或 YAML，映射逐键合并、列表覆盖，args 覆盖文件）；`RouterTable`
  （精确 / 通配 / 运行期 mode 覆盖）。
- `resource/history/chatbot/work.md`（本文件）。

### 阶段 2：假平台跑通完整流程（代码完成，待端到端用例）

- `chatbot_router.{h,cpp}`：路由 + 懒创建通道 + LRU 回收 + `sessionId` 定长散列 +
  `chatKeysOf` / `setModeOverride` / `rootIdOf` / `titleOf`。
- `chatbot_outbound.{h,cpp}`：切分、每会话 FIFO、`sendIntervalMs`、被动回复 `msg_seq` 递增与
  用尽降级、`maxRepliesPerTurn`、队列上限丢最旧、计数、`noteFallback` / `noteActiveBlocked`。
- `chatbot_engine.{h,cpp}`：去重 → 路由 → 命令 → 聚合缓冲 → 组装 → `send_input`；
  `mention` 立即唤醒、`always` 静默窗口、`turn_end` 补投、缓冲背压；通道事件
  (`READY`/`TURN_START`/`TURN_END`/`CLOSED`/`INTERRUPT`/`DELTA_TEXT`)；`fallbackSend`；
  命令 local/model/action；`statusJson` / `chatInfoJson` 等查询面。
- `chatbot_tools.{h,cpp}`：`send_message` / `history` / `chat_info` / `status` / `set_mode` 五个工具实现。
- `platform/platform.{h,cpp}` + `platform/fake/fake_platform.{h,cpp}`：`IPlatform` 契约与内存假平台
  （注入事件、记录出站、可配延迟/失败/在线状态）。
- `agentxx_chatbot.cpp`：插件入口（自建 io_context + 线程、`runOnIo` / `runOnIoBlocking`、
  账号表、人设 root 表、工具依赖）；`chatbotStart` 校验 → 起 io 线程 → 建平台 → `mountRoot`
  （`includeCreator = true` + 挂载名单含本插件）→ `engine.init`；`chatbotStop` 停平台 → 关通道 →
  逆序卸根；5 个工具只注册到人设 root（内置 root 仅按 `exposeToolsOnHost` 暴露）。

### 阶段 3：真 QQ 接入（代码完成，待真实账号联调）

- `platform/qq/qq_api.{h,cpp}`：token 缓存与提前刷新、gateway、发消息、撤回、预签名 PUT、取 URL。
- `platform/qq/qq_gateway.{h,cpp}`：WS 连接、Hello(op10)、Identify(op2, intents = 1<<25)、
  心跳 0.8× 与连续 2 次无 ACK 重连、Resume(op6)、op7/op9、指数退避。
- `platform/qq/qq_media.{h,cpp}`：分片上传（prepare → 逐片 PUT + part_finish → 合并 → 缓存 file_info），
  md5 / sha1 / md5_10m 摘要。
- `platform/qq/qq_platform.{h,cpp}`：事件归一化、发送、附件下载落盘到
  `{data_dir}/chatbot/media/{accountId}/{kind}/`。

### 测试与构建

- `agent/test/core/test_chatbot_core.cpp` + `include/agentxx-test/core/test_chatbot_core.h`：
  504 条检查全部通过（`agentxx_test chatbot_core`）。
- `agent/test/CMakeLists.txt` 把 `chatbot_core.cpp` / `chatbot_types.cpp` / `chatbot_config.cpp`
  编进 `agentxx_test`（测插件真实实现，与插件是否启用编译无关）。
- `agent/plugins` 独立构建通过：`libagentxx_chatbot.dll` 正常产出到
  `agent/build/windows-debug/exec/plugins/agentxx_chatbot/`。

### 阶段 2 收尾：端到端用例与被动回复修复（完成）

- `agent/test/plugin/test_plugin_example_chatbot.cpp` + `include/agentxx-test/plugin/`：
  用真实插件 DLL（假平台账号）跑完整流程，共 **102 项检查全部通过**。覆盖：
  1. `start` 后 1 个人设根、根上 7 个工具（5 业务 + 2 调试）、提示词从 `config_json` 来、
     内置根上看不到这些工具；
  2. 私聊一条 → 通道懒创建 → 模型收到带说话人前缀的输入 → 工具发送 → 假平台收到（带被动回复 id）；
  3. `mention` 模式：3 条无 @ 只缓冲，第 4 条带 @ 时一次投递带上全部 4 条；
  4. 同 `msg_id` 注入两次只处理一次（`duplicated` +1）；
  5. 长文本（300 字 / 上限 120）切成多条，分段标记齐全；
  6. 命令：`local`（不进模型、模板变量渲染）/ `model`（进模型）；
  7. 附件：图片作为多模态附件投给模型，文本里有附件行；
  8. 700 字回复 → 按上限切段且受 `maxRepliesPerTurn` 限制；
  9. 一轮连调 7 次工具 → 出站不超 5 条；
  10. `mention` 立即唤醒 + 被动回复 id 落在最后那条被 @ 的消息上；
  11. 工具查询面：`chatbot_history` / `chatbot_chat_info` / `chatbot_set_mode`（运行期生效）；
  12. 兜底发送：模型只输出正文不调工具 → 被 @ 时兜底发出；
  13. 卸载插件 → 通道关闭、根回收、工具撤销。
- 调试辅助（`behavior.debugTools` 默认关，只在测试/本地演示打开）：
  - `chatbot_debug_inject`：把一条假消息包直接投给调度层（支持附件 / 引用 / 发言角色）；
  - `chatbot_debug_outbox`：回看平台侧收到的出站（含 `passiveTo` / `passiveSeq` / `replyToRef`），
    由 `IPlatform::debugOutbox` 提供（真平台返回空表）。

### 本阶段修掉的四个缺陷

1. **被动回复 id 丢失**：`ChatbotOutbound::submit` 构造 `Job` 时只拷了 `replyToRef` / `markdown`，
   **没拷 `req.passiveMsgId`** —— 工具路径的出站永远没有 `passiveReplyTo`（`enqueueDirect`
   直接传 `OutboundMessage`，所以那条路径正常）。现象：`passiveTo` 空、`passiveSeq = 0`。
2. **兜底发送永远不生效**：`onChannelTurnEnd` 里 `fallbackSend` 在本轮正文写入
   `turn.lastAssistantText` **之前**调用，读到空文本直接返回。改为先写正文再兜底。
3. **主程序：插件在自己 `start` 里挂根拿不到 `mountFn`**（详见下节）。
4. **测试装置：模拟模型按字节切流式正文**：`test_agent.cpp` 的 SSE 分片按 10 **字节**切，
   切在多字节 UTF-8 字符中间时上游解析为空正文 → “LLM stream completed with empty response”
   → 重试 5 次。之前只用 ASCII 短文本，没暴露。已改为按 UTF-8 码点边界切。

### 主程序框架修复（`agent/lib`，阶段 2 暴露的问题）

插件在自己的 `start` 里调 `runtime.mount_root` 时，本次加载还没收尾，`onInstanceLoaded`
（解析 `agentxx_plugin_agent_mount` / `_unmount`）尚未执行 —— 于是 `inst->mountFn == nullptr`，
新建的人设根被当成「无 agent 侧能力」，插件 `mount` 回调不执行，人设根上一个工具都没有。两处修：

1. `plugin_manager_runtime.cpp` 的 `runtimeMountRoot`：**init 之前**就把 `ctx->agentId` 设成
   `rootId`（否则 `init` 的 plugin_mount 步骤拿空 id 会落到内置根 `root` 的挂载表项上），
   并在 `mountFn`/`unmountFn` 都为空时补一次 `onInstanceLoaded(*inst)`（幂等）。
2. `plugin_mount.{h,cpp}`：表项新增 `mountInFlight`。同一个根被并发调用两次
   （根 init 的 plugin_mount 与运行时挂载驱动）时，后到者直接返回成功、**不重复回调插件**；
   各返回路径都会清回 `false`。

### 阶段 3 真机联调：启动崩溃与四个联调阻塞（本轮修复）

真账号首次联调时 `agentxx_cli server` 在插件 `start` 里直接 abort，随后又连不上网关。四处缺陷：

1. **锁重入导致进程 abort**（`qq_platform.cpp`）：`QqPlatform::start` 全程持 `mutex_`，
   里面调 `gateway_->start()`，而网关 `start` 同步回调 `hooks.onState`，那个回调又去锁同一把
   `mutex_`。MSVC 的 `std::mutex` 对同线程重入直接 `_Throw_Cpp_error`，异常从 asio handler
   冲出 `io->run()`，那条线程没有外层 catch → `std::terminate`（实测信号 22）。
   改为「锁只护对象成员、回调与网关启动都放到锁外」，`stop` 同样先 move 出网关再在锁外销毁。
2. **io 线程异常直接杀进程**（`agentxx_chatbot.cpp`）：`startIo()` 的线程体与
   `runOnIoBlocking` 的 posted lambda 之前都没有异常保护。前者改为 `run()` 外层 try/catch 循环，
   后者保证 `done` 一定置位。
3. **插件内 `XX_LOG*` 全部被丢弃**：插件静态链接自己那份 `cxx_utilxx_base`，`LogDispatcher`
   是**插件本地**单例、且没有任何 sink —— 平台层与调度层全用 `XX_LOG*`，于是网关连接、
   状态变化这些关键日志一条都看不到（现象：`server` 起来了但完全看不出 QQ 在做什么）。
   装一个 `HostLogSink`（`ThreadedLogSink` 派生）把本地 dispatcher 的记录转给主程序
   `pluginxx.log` 接口，`destroy` 时先摘 sink 再释放实例。
4. **`wsConnect` 不支持不带端口的 URL**（`agent/third_party/cxx_utilxx/src/ws_client.cpp`）：
   解析 host 时只在 `rfind(':')` 命中分支赋值，没写端口（`wss://api.sgroup.qq.com/websocket`，
   QQ 网关就是这种形式）时 `host` 留空，直接报 `empty host in ws url`。所有既有测试都用
   `ws://127.0.0.1:<port>/ws`，所以一直没暴露。补上无端口分支 + `test_ws_url_without_port` 回归用例。

修完后真账号链路打通：token 获取 → 网关 Hello（心跳 41250 ms）→ Identify → `ready`，
日志可见 `网关已就绪 (session ...)`。

首次真实会话又暴露两处网关问题：

5. **`RESUMED` 被当成普通事件忽略**：重连时服务端回 `RESUMED`（一条 Dispatch），代码只对
   `READY` 置在线，于是状态永远停在 `reconnecting`；上层"在线才发"的判断会把后续出站全挡掉。
   现在收到 `RESUMED` 也置 `online`、清零重连计数并触发 `onReady`。
6. **重连计数恒为 0**：只有"连接失败"那条路径在调 `backoffMs(++reconnectCount_)`，`op 7`/`op 9`
   给常量延迟，日志永远显示"第 0 次"。计数改为在 `scheduleReconnect` 内统一递增，
   传 `delayMs < 0` 表示按当前次数算指数退避（`op 7`/`op 9` 保持固定 1 秒）。

另：命令回复里的 `{channelId}` 以前渲染成孤立的 `-`（本地命令不建通道，首条命令必然如此，
`-` 看不出原因），改为 `未建`。

### 图片消息：三个叠加缺陷（真机联调最后一块）

发图片后模型完全没有回复，请求报 `HTTP 400 media[0].url must use the http or https scheme`。
是三个独立缺陷叠加，且**第三个会让会话永久坏掉**，值得单独记：

1. **附件路径在文件写完前就填上了**：`QqPlatform::downloadAttachment` 用 `co_spawn` 异步下载，
   但函数返回时立刻写 `ref.localPath`。消息马上投给模型 → 主程序读文件报
   `cannot get file size for server attachment ... system cannot find the file specified`
   → `dataUrl` 为空 → 回退把本地路径当 URL。改为同步下载（一次性 io_context 跑到完成），
   落盘成功后才回填路径。
2. **加载失败被当成"用本地路径当 URL"**（`base_agent.cpp`）：这类值会被上游整轮拒绝，
   连文字回复一起丢。改为跳过该附件并记警告，文字部分照常发送。
3. **脏数据永久残留**（最关键）：某一轮把本地路径写进会话的 `llm_context` 之后，
   该会话**此后每一轮请求都带着它**，与端点/provider 都无关 —— 换 `/responses` 或
   `/chat/completions`、换模型类型都照报同样的错，而拿同一个 data URL 直连上游却成功。
   排查时差点误判成"上游网关不支持 data URL"（实测 `openai-responses` 的 TUI 发图是正常的，
   这个对照是决定性线索）。现在 `modelcall` 在组请求前剔除 `image_urls`/`audio_urls`/
   `video_urls` 里非 `data:`、非 `http(s)` 的值并记条数，老会话可自行恢复。

另外补了一条"附件加载成功"的日志：以前只在失败时打日志，成功时无从判断到底转换没有，
排查走了不少弯路。

### 真账号联调结论（2026-10-29）

实测通过（日志可查）：

| 项 | 结果 |
|----|------|
| token 获取 / 自动刷新 | ✅ 剩余不足 90 秒时刷新，实测拿到 7200 秒有效期 |
| 网关 Hello / Identify / READY | ✅ 心跳间隔 41250 ms |
| 私聊普通消息 → 模型 → 工具回复 | ✅ 通道 lazily 创建（`通道 2`），投递给 人设根会话，模型调 `chatbot_send_message`，被动回复 81 字符送达 |
| 图片消息 → 模型看图 → 回复 | ✅ 下载落盘 → 转 Base64 Data URL → 模型正确描述图片内容 → 回复 150 字符 |
| 命令 `local`（`/axx-ping` / `/axx-info`） | ✅ 命中即回，不进模型，带被动回复 id |
| 会话统计 | ✅ `/axx-info` 的接收计数随消息递增 |

### 主程序框架修复（第二处：插件建根时机）

`AgentHost::create` 之前不给 `primaryAgent->host` 赋值，容器引用只在
`attachRoot → mountRoot → injectShared` 里给；但 `attachRoot` 在 `loadPluginsAsync` **之后**才调，
于是插件在 `start` 里 `mount_root` 必然拿到空容器
（`[runtime] mount_root 被拒 (-6): 本进程没有 agent 容器`）。
`AgentHost::create` 现在在创建后立刻把容器与共享线程池挂到 `primaryAgent` 上
（两处赋值同一个值，后到的 `injectShared` 不产生变化）。
三个用例因此把原先手写的 `primaryCtx->host = host` 改成传 `hostCfg.primaryAgent`
并断言容器自己挂上了引用。

### 根 id 不能直接当目录名

人设根 id 形如 `ext:<来源>:<n>`，被直接拼成会话库目录
（`{sessions}/{rootId}`）。冒号在 Windows 上是非法文件名字符，实测报
`session root ... is not usable: The filename, directory name, or volume label syntax is incorrect`，
根 init 失败、整个插件起不来。改为经 `sessionDirNameOf` 转成安全名
（只留字母数字与 `_`/`-`，其余换 `_`；序号部分不动，不同根仍落不同目录）。
用例：`plugin_example_chatbot` 现在给人设开 `enableSessionStore` + 主配置临时 `dataDir`，
断言目录名不含冒号且目录真的建得出来。

### 撤回工具：支持"倒数第几条"（2026-10-29）

真机实测暴露的问题：用户说"把倒数第二条撤回"，模型只能回"只能把最后两条都撤回"。

原因：`chatbot_recall_message` 原来只有 `count`（从最近一条往前数 N 条），
表达不了"跳过最近那条"。加两个入口：

- `target`：`-1` = 最后一条（默认），`-2` = 倒数第二条，`-3` = 倒数第三条…
  内部换算成 `skip = N - 1`。
- `skip`：跳过最近 N 条再开始撤，与 `count` 组合可表达任意区间。

顺带修掉的两处：

- 以 `#` 开头的 `target` 明确报错（`#N` 是**引用入站消息**的写法，与撤回自己的
  第几条无关），错误文本直接给出 `target="-2"` 的正确写法，避免模型反复试错。
- 去掉了"`count` 最多 8"这种按撤回条数的限制，改为"`skip` 不得越过已记录条数"
  （超出时报"要跳过的条数超出可撤回范围 (本会话只记录了 N 条)"），
  因为最近 id 表本身就有上限（`kMaxRecentIds = 8`）。

索引方向也一并核对清楚：`ChatbotOutbound::recentMessageIds` 返回的是**旧→新**，
末尾才是刚发的那条，取第 `k` 条（跳过 `skip`）的下标是
`recent.size() - 1 - skip - i`。注释原先写成"新→旧"，已改正。

用例（`plugin_example_chatbot`，新增 U10 会话）：一轮连发 3 条 →
`target="-2"` 撤中间那条 → `skip=1` 撤最旧那条 → 再要"倒数第二条"被拒
（报超出范围，不会退回去撤最近那条）→ `#2` 报错并给出正确写法 →
`target="-1"` 撤掉始终没被动过的最近那条。

### 群文件附件：真实结构拿到后修掉（2026-10-29）

真机现象：群里发文件后引用该消息让它描述内容，模型答"看不到文件"；日志里连附件下载
记录都没有。

拿到真实事件后（诊断日志打出来的原文，已原样存进 `chatbot_qq` 用例）：

```json
{
  "content": "  看看这个文件的内容是什么",
  "message_type": 103,
  "message_scene": {"ext": ["ref_msg_idx=REFIDX_…", "msg_idx=REFIDX_…", "auth_token=…"]},
  "msg_elements": [{
    "content": "",
    "message_type": 103,
    "msg_idx": "REFIDX_…",
    "attachments": [{"content_type": "file", "filename": "README.md", "size": 1700,
                     "url": "https://gzc-download.ftn.qq.com/ftn_handler/…?fname=README.md"}]
  }],
  "parallel_message": {"msg_nodes": [{"content": "[文件]", "message_type": 7}]}
}
```

三个结论，全部改掉：

1. **群文件不在顶层 `attachments`，在被引用消息的 `msg_elements[].attachments` 里**。
   原实现只读 `msg_elements[0].content`（文件消息没有正文 → 空串），永远看不到附件。
   现在把被引用消息的附件并入本条消息的附件（模型才能真正拿到文件）。
2. **引用内容为空时用附件描述顶上**：否则渲染出来是 `(回复 寒光: )`，模型只知道
   "有人引用了什么"却不知道是文件。现在拼成 `(回复 寒光: [文件] README.md)`。
3. **`message_scene.ext` 是字符串数组**（`["ref_msg_idx=…","msg_idx=…","auth_token=…"]`），
   原实现 `scene.value("ext", std::string{})` 对数组取到空值 → `replyRef` 永远为空。
   这正是之前"引用回复没生效"的原因。现在数组形态与字符串形态都解析；
   `msg_idx`（本条自己的索引）用于回复时引用，`ref_msg_idx` 是被引用的那条，别弄反。

另外两件事：

- **文件内容不只是给路径**：`file` 类附件按设计不作为多模态附件投递（框架的 MediaType
  只有 image/audio/video），只在输入里给路径 —— 而人设根常见 `mount.mode: none`
  （没挂 `agentxx_filesystem`），模型根本读不到路径。现在小文本文件（默认 ≤ 64 KiB，
  `behavior.attachments.inlineTextMaxBytes`，0 = 关）的正文直接**内联进输入**，
  用 `[#n:附件内容开始]` / `[#n:附件内容结束]` 围起来；判文本的规则是"不含 NUL 且是合法
  UTF-8"，二进制或超限仍旧只给路径。
- **归一化拆成纯函数** `platform/qq/qq_normalize.{h,cpp}`（不碰网络、不碰连接状态），
  这样真实事件可以直接进测试。新增测试模块 `chatbot_qq`：用上面那份真机 JSON 逐字段断言
  （附件类型/文件名/大小/URL、`quotedText`、`replyRef` 取的是 `msg_idx`），另覆盖
  ext 两种形态、全量消息不算 @、图片/视频/文件分类、语音识别文本进正文、引用图片消息、
  引用有正文的消息、私聊会话 id、该丢的事件（机器人自己/缺群号/空内容）、卡片消息、
  文件名兜底。

### 阶段 4：打磨与文档（进行中）

- 路由器的额度与回收现在有直接用例（`chatbot_core`）：会话 id 稳定性、活跃计数、
  LRU 回收最久未活动的那一条、被回收的群重新打开、通道/会话 id 反查、就绪与忙状态、
  主动回收与全部回收、不属于任何人设的会话拒开、标题生成。
- 用户接入说明：[agent/plugins/agentxx_chatbot/README.md](../../../agent/plugins/agentxx_chatbot/README.md)
  （配置四段、三种触发模式、凭证写法、输入形态、五个工具、排查顺序、扩展新平台）。
- 设计文档：[docs/zh-cn/design/plugins.md](../../../docs/zh-cn/design/plugins.md) 示例索引加
  `agentxx_chatbot` 一行，并新增 §13.1 概述（人设根 / 会话划分 / 回复出口 / 聚合 / 命令 /
  线程与调试工具）。

### 仓库配置补齐 chatbot 四段（2026-10-30）

`agentxx-config.yaml` 里原来只有一行 `path: .../agentxx_chatbot/` + `enabled: true`，
账号段是空的，`start` 会在校验阶段直接失败（`accounts 不能为空`），也就是说开箱即用的
配置其实开不起来。本轮把四段写全：

- **账号**：`qq_main` / `type: qq`，凭证走 `${QQBOT_APPID}` / `${QQBOT_APPSERCRET}`
  （本仓库 `.env` 用的是带 "S" 的那种拼写，`appSecretFile` 两种都认），注释里给了
  `appIdFile` 的替代写法。
- **人设**：`assistant`，`mount.mode: listed` + `[agentxx_filesystem, agentxx_websearch]`
  （不挂 `execute_command`，注释说明群里任何人能执行命令的风险；也不给 `all`，
  避免每个根各建一份 codegraph 索引），`bind` 用通配把"所有群被 @ 才回 / 所有私聊每条都回"
  都接上，`tools` 白名单以注释形式给了只读示例。
- **行为**：聚合窗口、发送间隔、切分上限、一轮回复数、`humanInLoop: auto-allow`
  （群里弹不出权限询问）、`attachments.{fetch,maxPerMessage,inlineTextMaxBytes}`。
- **命令**：`/axx-info`（local）、`/axx-ping`（local，带 `{channelId}`）、
  `/axx-stop`（action cancel）。

顺带修掉两处会误导的文档/示例：

- README 的行为段示例把附件策略写成平铺的 `attachmentsMaxPerMessage`，而解析只看
  `behavior.attachments.maxPerMessage`（平铺键会被**静默忽略**）。已改成嵌套写法，
  并补上 `humanInLoop` / `injectContextHint`。
- README 与配置示例都写着 `action: clear`，但 `clear` 目前只打一行"本期未实现"的日志，
  实际不生效。已在两处注明，只保留 `cancel`。

另外记一条验证经验：**`--dump-config` 不能验证人设根**。这条路径里插件确实会被加载并
`start`，但主程序此时没有 `AgentHost` 容器，`mount_root` 一律被拒：

```
[runtime] mount_root 被拒 (-6): mount_root: 本进程没有 agent 容器 (插件管理器未挂到 AgentHost 上)
start: 人设 'assistant' 的根挂载失败 (见主程序日志: 配置非法或根预算用尽)
Plugin `agentxx_chatbot` start failed: ...
```

看到这条不用怀疑配置写法；用 `server`（或把账号改成 `type: fake` 避免连真实平台）
起一次，日志里出现下面三行才算通：

```
人设 'assistant' 的根已挂载: ext:agentxx_chatbot:1
已启动: 1 个账号, 1 个人设, 3 条命令
AgentHost: mounted root 'ext:agentxx_chatbot:1' (owner='agentxx_chatbot', ..., roots=1/8)
```

本轮实测：`roots=1/8`、人设根 3 个实例（chatbot + filesystem + websearch）、16 个工具、
`config issues[0]`。

还要记住 `--dump-config` 有副作用：它是真的把插件加载并 `start` 了一遍，所以 chatbot 会
**连一次平台**（日志里有 `账号 'qq_main' 已启动 (appId ***1230, ...)`），跑完就退出。
不想连平台时把账号 `type` 临时改成 `fake` 再 dump。

### 命令改成插件内置，去掉 `commands` 配置段（2026-10-30）

原来的 `commands` 段让每个部署自己写命令名、`mode`、回复模板。想法是灵活，实际代价是：
命令名与文案一旦可配置，README 的示例、`plugin_example_chatbot` 的用例、排错说法
（"发 `/axx-info` 看状态"）都得跟着配置走，谁改了都不同步。命令本来就是插件自己的
交互约定，改成写死：

- `chatbot_core.{h,cpp}` 新增 `builtinCommands()`，四条：`/axx-help`（local，列命令）、
  `/axx-info`（local，会话状态）、`/axx-hello`（model，让模型说一句）、
  `/axx-stop`（action cancel，不回复文本）。
- `chatbot_config.cpp` 的 `parseCommands` 换成 `applyBuiltinCommands`：装配内置表，并且
  配置里若还留着 `commands` 段就记一条警告（"已不再生效: 命令由插件内置 (…)"），避免
  "改了配置却没反应"。`commandModeFromString` 随解析一起删掉（只有它用过）。
- `validateConfig` 里那段命令校验保留，改成对内置表的自检 —— 以后改表写错名字/动作，
  启动时就能看到。
- 配置从"四段"回到"三段"（`accounts` / `personas` / `behavior`）。

用例：新增 `chatbot_core` 的 `testBuiltinCommands`（名字前缀/去重/各模式必填项/四条都在/
`{commandList}`/能被匹配器吃下），另加"配置里写 `/custom-cmd` 不生效、表仍是内置那几条"；
删掉两个只对配置生效的校验用例。`plugin_example_chatbot` 的测试配置去掉了 `commands` 段，
但它对 `/axx-info`（要含 "人设 assistant"、`{chat}` 渲染成会话 id）与 `/axx-hello`
（要含 "请向" 与 `{args}`）的断言**没动** —— 内置文案就是按这两条断言写的。

实测：`chatbot_core` + `chatbot_qq` + `plugin_example_chatbot` 792 项全过，
`boundaries` + `test_modules` 24 项全过；fake 平台起 server 日志为
`已启动: 1 个账号, 1 个人设, 4 条命令`。

## 待完成

- [x] 阶段 2 收尾：`plugin_example_chatbot` 端到端用例（假平台注入 → 聚合 → 工具回复 →
      切分多条 → 被动回复配额），以及「人设 root 上确有 `chatbot_*` 工具」的用例。
- [x] 阶段 3 收尾：真实账号联调（网关鉴权、群/私聊事件、被动回复配额、图片收发、撤回）。
      私聊与群聊文字、图片收发、被动回复序号、命令、撤回都已实测通过。
- [x] 群文件附件：已按真实事件结构解析（见上节"群文件附件：真实结构拿到后修掉"）。
- [x] 引用回复没生效：根因是 `message_scene.ext` 为字符串数组，已修并进 `chatbot_qq` 用例；
      真机上"引用哪条"的实际效果待下一次联调确认。
- [ ] 阶段 4：还剩「被动→主动降级」与「sendIntervalMs 频控」的真实时序验证（假平台可以测，
      但需要模拟平台侧返回主动消息被拒）。
- [ ] 阶段 5：可选（TTS / 微信等其它平台）。

## 测试现状（本轮结束时）

| 模块 | 项数 |
|------|------|
| `chatbot_core`（纯逻辑 + 路由/额度） | 557 |
| `chatbot_qq`（QQ 事件归一化，真实事件回归） | 63 |
| `plugin_example_chatbot`（真实插件 + 假平台 + 模拟模型） | 153 |
| `plugin_agent_runtime` / `plugin_example_runtime` | 178 / 50 |
| `websocket`（含无端口 URL 回归） | 242 |
| `boundaries` / `test_modules` | 19 / 5 |
| 本轮相关模块合计 | 1044 项全通过 |

## 注意事项

- 插件只能 `#include "agentxx/plugin/api/*"` 与 `agentxx/util/exception.h`（`boundaries` 模块会检查）。
- 工具必须注册在**人设 root** 上：`mountRoot` 要 `includeCreator = true`，且挂载名单要含
  `agentxx_chatbot`（`mode: listed` 时自动补上）。
- 主程序 io 线程上的通道事件回调里只做「拷贝 → post 到插件自己的 io 线程」，不得阻塞。
- 新增测试模块后要：`agent/test/test.cpp` 注册 + `AGENTXX_UPDATE_TEST_MODULES=1` 重生成
  `docs/zh-cn/design/test-modules.md`。
- **`utilxx_base::Json` 的构造陷阱**：`Json{x}` 会命中 `initializer_list<Json>` 构造，得到
  **单元素数组**（`["123456"]`）而不是标量。构造标量必须写圆括号 `Json(x)`；对象 / 数组
  请用 `Json::object()` / `Json::array()` 或 `Json{{"k", v}}`。
- `chatbot_config.cpp` 的 `parseBindRule` 不再要求 `account` 字段（`bind[].chats[]` 形态由外层
  继承），简化写法仍自带 `account`。
- 运行期 `mode` 覆盖由 `RouterTable::route` 统一施加（`ChatbotRouter::route` 会再叠加一次
  `effectiveMode`，两处一致，改动时保持同步）。
- **平台事件的字段解析集中在 `platform/qq/qq_normalize.{h,cpp}`**（纯函数，不碰网络）。
  平台对不同附件的字段布局并不一致，改这里必须同时补 `chatbot_qq` 用例 ——
  该模块用的就是真机抓到的事件 JSON，别用自己编的结构替换它。
- **普通文件的正文要么内联、要么没用**：人设根常见 `mount.mode: none`（没挂文件系统插件），
  只给本地路径模型读不到。加新附件类型时想清楚"模型最终看到什么"。
- 出站字段传递只有一个入口：`OutboundRequest` → `ChatbotOutbound::submit` 里逐个拷进
  `Job.message`。新增出站字段时两处都要加，否则工具路径静默丢字段（被动回复 id 就这样丢过），
  而 `enqueueDirect` 路径（直接给 `OutboundMessage`）看不出问题。
- 改 `agent/lib` 之后**必须重建 `agentxx_lib_repo` 与 `agentxx_test_repo` 两个目标**，
  否则测试仍在用旧库（本阶段踩过）。
- 插件独立构建（在 superbuild 之外单独编 `agent/plugins`）必须带上 `PLUGINXX_API_VERSION=3`
  与静态 OpenSSL 指向，否则运行时是「API version too old」或 `LoadLibrary error 126`。
- 调试探针别用 `XX_LOGW`：插件日志经 `pluginLog` 转发受模块/级别过滤，看不到时容易误判成
  「代码没执行到」。改用测试可读的查询面（如 `statusJson()` / `debugOutbox`）或 `fprintf(stderr)`。
  另：插件日志走的是主程序宿主的日志出口，而测试进程的 stdout/stderr 里插件日志与测试自己的
  输出顺序会错乱 —— 看时序问题时直接写文件更可靠。
- **模拟模型的正文分片必须按 UTF-8 码点切**：`test_agent.cpp` 的 SSE 分片之前按字节切，
  多字节字符被切开后上游解析为空正文并重试 5 次（耗时十几秒且看不出原因）。
