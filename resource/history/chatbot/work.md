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

### 阶段 4：打磨与文档（进行中）

- 路由器的额度与回收现在有直接用例（`chatbot_core`）：会话 id 稳定性、活跃计数、
  LRU 回收最久未活动的那一条、被回收的群重新打开、通道/会话 id 反查、就绪与忙状态、
  主动回收与全部回收、不属于任何人设的会话拒开、标题生成。
- 用户接入说明：[agent/plugins/agentxx_chatbot/README.md](../../../agent/plugins/agentxx_chatbot/README.md)
  （配置四段、三种触发模式、凭证写法、输入形态、五个工具、排查顺序、扩展新平台）。
- 设计文档：[docs/zh-cn/design/plugins.md](../../../docs/zh-cn/design/plugins.md) 示例索引加
  `agentxx_chatbot` 一行，并新增 §13.1 概述（人设根 / 会话划分 / 回复出口 / 聚合 / 命令 /
  线程与调试工具）。

## 待完成

- [x] 阶段 2 收尾：`plugin_example_chatbot` 端到端用例（假平台注入 → 聚合 → 工具回复 →
      切分多条 → 被动回复配额），以及「人设 root 上确有 `chatbot_*` 工具」的用例。
- [ ] 阶段 3 收尾：真实账号联调（网关鉴权、群/私聊事件、被动回复配额、图片与文件收发）。
      需要 `resource/keys/qq_bot.env` 里的凭证，仓库内暂无该文件。
- [ ] 阶段 4：还剩「被动→主动降级」与「sendIntervalMs 频控」的真实时序验证（假平台可以测，
      但需要模拟平台侧返回主动消息被拒）。
- [ ] 阶段 5：可选（TTS / 微信等其它平台）。

## 测试现状（本轮结束时）

| 模块 | 项数 |
|------|------|
| `chatbot_core`（纯逻辑 + 路由/额度） | 545 |
| `plugin_example_chatbot`（真实插件 + 假平台 + 模拟模型） | 102 |
| `boundaries` / `test_modules` | 19 / 5 |
| 全量 `agentxx_test` | 39979 项全通过 |

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
- **出站字段传递只有一个入口**：`OutboundRequest` → `ChatbotOutbound::submit` 里逐个拷进
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
