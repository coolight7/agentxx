- 难度: B
- 类型: bug修复
- 基于commit: 49ce3578
- 时间: 2026-11-04
- 需求:
```md
请修复 agentxx_chatbot 插件在消息上下文中出现视频时出错的问题
```
（后续追加: 图片、音频同理按模型能力处理）

## 现象

群里有人发了一段视频, 机器人这条消息没有回复, 之后**同一会话里的消息也全都不回了**;
日志里能看到模型请求失败 (HTTP 400 一类), 换模型端点也没有改善。发图片/语音到不支持对应
输入能力的模型上也是同一套症状。

## 根因 (三处叠加)

1. **附件被无条件当多模态附件投给模型。** `assembleInput` (`chatbot_engine.cpp`) 把所有
   `MediaKind != File` 的附件都写进 `input.attachments`; `BaseAgent::runTurnAsync` 再把
   本地文件读成 base64 填进 `image_urls` / `audio_urls` / `video_urls`, 最终由 provider
   生成 `image_url` / `input_audio` / `video_url` 内容块。但模型配置里的
   `image_input` / `audio_input` / `video_input` 只被客户端文件选择弹窗用过, 服务端完全没看 ——
   没声明的类型上游对未知内容块直接**整轮 400** (视频最典型: 几乎没有端点认 `video_url`),
   用户看到的就是"发了视频后机器人再也不出声"。
2. **base64 附件永久留在会话上下文里。** 用户消息写进会话时连这些 URL 一起落盘, 之后
   **每一轮请求都重发** (图片几 MB, 视频可达数十 MB)。于是这条会话从此跑不通, 与"哪一轮
   出错"无关 —— 换上能看视频的模型也一样被撑爆。
3. **超限附件会让整条输入被丢掉。** 主程序对附件有条数与单文件上限 (图片 10MB /
   音频 25MB / 视频 50MB, 见 `conversation_types.h`); `parseUserInputJson` 超限时按参数
   错误拒绝**整条输入**, 而插件把 `sendInput` 的失败只记一条日志就作罢 —— 正文一起丢,
   机器人同样一声不出。QQ 视频常见几十 MB, 很容易踩到。

## 修改

### 框架 (`agent/lib`)

- `include/agentxx/agent/conversation_types.h`: 新增判据与文案
  - `mediaInputEnabled(type, imageInput, audioInput, videoInput)`: 三类开关 → 该媒体类型能否投递;
  - `MediaSkipCounts` + `describeSkippedMedia`: 未投递条数 → 一句话
    (`1 个图片 (模型配置 image_input = false)、2 个视频 (…)`), 日志与正文共用同一份文案。
- `src/agent/base_agent.cpp`: 组用户消息前按**当前会话模型的声明**过滤附件 ——
  未声明的类型不投 (连文件都不读, 不去白白 base64 一遍), 并在模型看到的正文末尾补一行
  `[附件未传给模型] …: 当前模型未声明对应的多模态输入能力, 只保留文件名与路径`, 同时记警告
  日志。展示用的 `attachments` / `TurnStart` 增量不变, 客户端照旧显示附件卡片。
  三个开关在 `co_await` 前就取成值 (不跨挂起点持有模型注册表的引用)。
- `src/nodes/modelcall.cpp`: 在已有的"剔除本地路径形式附件地址"兜底后面再加一段 ——
  模型未声明的类型, 把上下文里已有的对应 URL 全部剔掉并记条数。这样**已经写坏的会话**
  能自行恢复 (与图片那次修复的兜底同一思路)。

### 插件 (`agent/plugins/agentxx_chatbot`)

- `chatbot_engine.cpp`
  - `flush`: 带附件的输入被拒时, 去掉 `attachments` 再投一次 (`withoutAttachments`),
    保证正文照常进上下文并记一条警告; 只有两次都失败才走原来的失败计数。
  - `assembleInput` 增加可选出参 `hasAttachments`, 供上面那次重试判断要不要重投。
- `agentxx_chatbot.cpp`: 输入格式说明段 (`chatbot-input-guide`) 补两句 ——
  `[#n:附件]` 行的含义, 以及只有模型声明了能力的类型才会随输入直接给, 只给路径时不要假装看过。
- `README.md`: §3 说明附件投递取决于模型声明的能力; 常见问题里加"发了附件后不回复"一条。

## 验证

新增用例 (`plugin_example_chatbot` 第 7e~7g 段, 走真实插件动态库 + 真实 `AgentHost` + 模拟模型;
测试模型默认 `image_input: true`、音频/视频默认 false, 用例内按需切换):

1. **视频**未声明 → 模型请求里没有 `video_url`, 但正文里有文件名 `b.mp4`、
   「附件未传给模型」与 `video_input = false`; 打开 `videoInput` 后重发, `video_url` 重新出现;
2. **音频**未声明 → 请求里没有 `input_audio`, 有 `audio_input = false` 与文件名, 正文照常;
3. **图片**关掉 `image_input` → 请求里没有 `image_url`, 有 `image_input = false`;
   随后恢复 (后面的用例要求图片能投)。

回归: `chatbot_core` / `chatbot_qq` / `chatbot_media` / `chatbot_media_io` /
`plugin_example_chatbot` / `agent` / `input_delivery` / `summarization` / `session_runtime` /
`openai_provider` / `anthropic_provider` / `fake_provider` / `remote_agent` 全绿。

## 备注

- **默认三个开关都是 false (= 纯文本模型)**, 所以"发图给机器人看"要求模型条目里
  `image_input: true` —— 与 TUI 附件按钮的判据完全一致, 不再出现"客户端能选、服务端照发、
  上游拒绝"的分歧。
- 附件仍会正常下载到 `{data_dir}/chatbot/media/<account>/<类型>/`, 输入里给的是本地路径,
  人设根挂了命令行/文件系统工具时模型可以自己读文件或抽帧。
- 客户端 (TUI) 本来就按这组开关渲染附件入口, 行为不变; FFI/插件等其它入口现在也遵守同一契约
  (见 `docs/zh-cn/design/ffi.md` 的附件一节)。
