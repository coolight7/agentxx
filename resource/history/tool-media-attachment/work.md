# 实施记录: 模型通过文件路径读取多模态附件 (tool-media-attachment)

## 当前进度

- [x] 阶段一: 多模态附件读取原型验证完成 (基础通道打通)
  - 已按阶段二方案回退阶段一的 JSON 信封实现 (阶段二替代了那条路)
- [x] 阶段二: 重构 Tool C API 为双参数分离返回 (`result` raw text + `payload` metadata JSON)
  - [x] 步骤 1: `plugin_api.h` 定义 `AgentxxPluginToolNotify` 与 `AgentxxPluginToolCallback`
  - [x] 步骤 2: `ToolResult` 移除信封，增加 `metadataPayloadJson` 与 `fromParts`
  - [x] 步骤 3: 插件 SDK `plugin_kit.h` 适配双参数分离上报与互调
  - [x] 步骤 4: 主程序 `plugin_manager_adapters.cpp` 与 `plugin_manager_capability.cpp` 适配
  - [x] 步骤 5: JS 插件引擎 `agentxx_javascript_engine.cpp` 适配
  - [x] 步骤 6: 更新测试用例并全量编译验证
  - [x] 步骤 7: 清理过时代码与收尾总结

---

## 步骤详情与记录

### 建立任务文档 (2026-11-20)
- 创建 `index.md`, `plan.md`, `work.md`。
- 确认方案范围与技术路线。

### 第一步: 扩展 `ToolResult` 与插件信封解包逻辑 (2026-11-20)
- 创建 `agent/lib/include/agentxx/agent/media_attachment.h`，从 `conversation_types.h` 抽取 `MediaType`、`MediaAttachment` 及配额、后缀识别辅助函数，解耦头文件循环依赖；
- 修改 `agent/lib/include/agentxx/agent/conversation_types.h` 包含 `media_attachment.h`；
- 扩展 `agent/lib/include/agentxx/util/tool_result.h`：增加 `attachments` 字段、`withAttachments` 工厂方法、`packEnvelope()` 与 `unpackEnvelope()` 信封转换；
- 更新 `agent/lib/include/agentxx/plugin/api/plugin_kit.h` 中的 `toolBodyResultTo`，在插件返回带附件结果时自动打包为信封；
- 更新 `agent/lib/src/plugins/plugin_manager_adapters.cpp` 中的 `mapPluginOpResult`，主程序接收到插件 payload 时自动通过 `unpackEnvelope` 还原附件。

### 第二步: 在 `agentxx_filesystem` 插件中实现 `agentxx_filesystem_read_media` (2026-11-20)
- 在 `agent/plugins/agentxx_filesystem/filesystem_impl.h` 中实现 `fileReadMediaExecuteImpl` 与 `fileReadMediaExecute`：
  - 路径解析与有效性验证；
  - 扩展名白名单校验与 MIME 映射（支持图片、音频、视频常见格式）；
  - 文件大小配额校验（图片 10MB，音频 25MB，视频 50MB）；
  - 组装 `MediaAttachment` 并以 `ToolResult::withAttachments` 返回；
- 在 `agent/plugins/agentxx_filesystem/agentxx_filesystem.cpp` 中定义常量并注册 `agentxx_filesystem_read_media` 工具（`blocking_tool`）；
- 声明读路径权限 `registerReadPathPermission(ctx, ref, kNameReadMedia, "path")`；
- 更新 `agent/plugins/agentxx_filesystem/plugin.yaml` 中的工具描述。

### 第三步: 在 `ToolcallWrapNode` 中实现附件异步转换与安全注入 (2026-11-20)
- 创建 `agent/lib/include/agentxx/agent/media_attachment_loader.h`，抽取通用的 `loadLocalMediaAttachmentAsync`，并在 `base_agent.cpp` 复用；
- 在 `ToolcallWrapNode::runPreparedToolCall` 中，将结果中的附件序列化存入 `tool_msg.extra["attachments"]`；
- 在 `ToolcallWrapNode::baseRun` 收集工具执行返回的附件：
  - 依据模型多模态能力开关进行白名单过滤；
  - 经线程池异步读取本地媒体文件并转换为 RFC 2397 Data URL；
  - 生成 `role: "user"` 的观察消息（包含 Data URLs 与 AutoInserted 标记），追加至 `typedResults`。

### 第四步: 在 `EventBridge` 中实现 Tool 附件回填至 `ViewMessage` (2026-11-20)
- 在 `EventBridge::handleChannelWrite` 收到 `role == "tool"` 消息时，解析 `jm["extra"]["attachments"]`；
- 将附件数组填充到展示消息的 `row.attachments` 中，并通过 `updateViewMessage` 和 `UpdateMessage` 同步更新会话库与客户端视图。

### 第五步: 编译、运行单元测试与端到端验证 (2026-11-20)
- 编写测试用例 (`agent/test/core/test_filesystem_tools.cpp`)：
  - `test_tool_result_envelope_pack_unpack`: 测试多模态信封往返打包与解包；
  - `test_read_media_tool_basic`: 测试读取图片、音频、视频的 MIME、类型与属性；
  - `test_read_media_tool_errors`: 测试空路径、不存在文件、目录路径、不支持的扩展名；
  - `test_read_media_relative_workdir`: 测试相对工作目录路径解析；
  - `test_plugin_real_link`: 真实加载动态库插件并调用 `agentxx_filesystem_read_media` 工具验证。
- 编译与测试执行：
  - `agentxx_static`、`agentxx_filesystem` 插件库以及 `agentxx_test` 均成功编译通过；
  - 运行 `filesystem` 测试模块（包含 273 项断言），全部通过 (passed=273 failed=0)；
  - 运行 `toolcall_args`、`toolcall_parallel`、`event_stream`、`event_delivery`、`permission` 测试模块，全部通过；
  - 运行 `boundaries` 边界合规测试模块，19 项检查全部通过；
  - 检查代码行尾：所有修改与新增代码均严格遵循 UTF-8 (无 BOM) + LF 格式。

---

## 阶段二实施记录 (2026-11-20)

阶段一用 JSON 信封 (`__agentxx_tool_media__` 包在正文里) 把附件从插件带到主程序，
问题是正文与元数据挤在同一个字符串里：正文可能是本地代码页的原始字节，一旦为了塞进
JSON 而转码，主程序侧的 `autoConvertToUtf8` 就再也拿不回原样字节。阶段二把两段拆开传。

### 步骤 1: C ABI 定义新通知器 (`plugin_api.h`)

改动文件：`agent/lib/include/agentxx/plugin/api/plugin_api.h`

- 新增 `AgentxxPluginToolNotify`，四个字段：
  - `done(host_ud, status, result, payload)` —— 两段上报主入口；
  - `done_text(host_ud, status, result)` —— 没有元数据时的便捷入口；
  - `done_frame(host_ud, status, frame, frame_len)` —— 收一整帧裸字节 (SDK 内部用)；
  - `host_ud` —— 主程序侧不透明指针，由主程序填。
- 新增 `AgentxxPluginToolCallback`（4 参：`host_ud` / `status` / `result` / `payload`），
  替换 `AgentxxPluginToolsIface::call_tool_async` 原来的单字符串回调。
- `AgentxxPluginToolSpec::execute_start` 第 5 参由旧完成回调改为
  `const AgentxxPluginToolNotify* notify`。
- `AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION` / `_MIN_VERSION` 3 → 4：成员顺序没变，
  但函数签名属破坏性改动，按规则抬 `MIN_VERSION`。

### 步骤 2: 接口代次抬到 v6，`ToolResult` 去信封

改动文件：`agent/CMakeLists.txt`、`agent/lib/include/agentxx/plugin/api/client_plugin_api.h`、
`agent/lib/include/agentxx/util/tool_result.h`

- `AGENTXX_PLUGIN_API_VERSION` 5 → 6（FORCE 校正逻辑照旧）；client 侧
  `AGENTXX_CLIENT_PLUGIN_API_VERSION` / `_MIN_VERSION` 同步 5 → 6。两侧都补了 v6 的历史说明。
- `tool_result.h`：删除 `kToolMediaEnvelopeKey` / `packEnvelope()` / `unpackEnvelope()`；
  改为三段接口：
  - `metadataPayloadJson()` —— 附件等元数据转 JSON 串，无元数据返回空串（零开销）；
  - `loadMetadataPayloadJson(std::string_view)` —— 反向恢复，坏 JSON 直接忽略、不动正文；
  - `static fromParts(resultText, status = Ok, payloadJson = {})` —— 两段拼回 `ToolResult`。

### 新增帧编解码：`agent/lib/{include/agentxx/plugin/tool_result_frame.h,src/plugins/tool_result_frame.cpp}`

`PluginxxOpCore` 的完成包只有一个字符串，而且 cxx_pluginxx 不能改，于是两段用长度前缀
裸字节帧承载，只在本进程的 SDK ↔ 主程序之间用，不上线、不做 JSON：

```text
[uint32 正文字节数][uint32 元数据字节数][正文][元数据]
```

- `kToolResultFrameHeaderSize` / `encodeToolResultFrame(result, metadata)`；
- `ToolResultFrameView{result, metadata}` / `decodeToolResultFrame(frame)`
  —— 长度对不上就把整段当正文（老主程序/异常输入都不至于丢内容）；
- `toolNotifyDone` / `toolNotifyDoneSingle` / `toolNotifyFrame`：三个 host 侧实现，
  内部转发到 `pluginxx::OpCore::onEndpointDone`；
- `makeToolNotify(const PluginxxOperatorNotify&)`：把三个函数指针 + `host_ud` 填进
  `AgentxxPluginToolNotify`，主程序侧只调这一个。

### 步骤 3: 插件 SDK 适配 (`plugin_kit.h`)

改动文件：`agent/lib/include/agentxx/plugin/api/plugin_kit.h`

- `toolBodyResultTo(value, outResult, outMetadata, outStatus)` 改成三段输出：正文、
  元数据 JSON、状态。
- `detail::textOnlyNotify(tool)` / `detail::frameNotify(tool)`：从 `AgentxxPluginToolNotify`
  里只取函数指针 + `host_ud`。**SDK 头里不能引用 `agentxx::plugin::toolNotify*` 这类主程序符号**
  —— 插件不链接 libagentxx。
- `fast_tool` / `blocking_tool` / `tool` / `polled_tool` 全部适配：
  - 后台任务体里 `notify` 由值保存成 `AgentxxPluginToolNotify`，另存 `resultText` / `resultMetadata`；
  - `tool` 的 Job 只存通知器，`p.notify_` 走 `textOnlyNotify`；
  - `polled_tool` 的轮询结束在 `runPolledPumpJob` 里
    `encodeToolResultFrame(job->resultText, job->resultMetadata)` 后交给 `notifyHost`；
  - 取消时连元数据一起清掉。
- 插件互调：`call_tool` 保持返回 `std::string`（模板 `detail::CallToolAwaiter<bool WithResult>`，
  `WithResult=false`），新增 `call_tool_result(...)` 返回完整 `ToolResult`
  （`WithResult=true`；工具失败不抛、看 `status`，取消仍抛 `CanceledException`）；
  `call_tool_blocking` 适配 4 参回调。

### 步骤 4: 主程序适配

改动文件：`agent/lib/src/plugins/plugin_manager_adapters.cpp`、
`agent/lib/src/plugins/plugin_manager_capability.cpp`、
`agent/lib/src/plugins/plugin_manager_vtable.cpp`、`agent/lib/include/agentxx/plugin/plugin_mount.h`

- `mapPluginOpResult`：先 `decodeToolResultFrame` 拆两段，正文按
  `needsUtf8Conversion` 判定后才 `autoConvertToUtf8`（本来就合法且纯 ASCII 的正文一字不动），
  元数据走 `loadMetadataPayloadJson`。
- 工具启动处把 `makeToolNotify(drive.notify)` 交给 `execute_start`。
- `plugin_manager_capability.cpp` 的 `setCompletionHandler` 同样解帧后再调 `cb`；
  `plugin_manager_vtable.cpp` 的 `xx_call_tool_async` 与 `PluginMount::callToolAsync`
  换成 4 参回调类型。

### 步骤 5: JS 引擎适配

改动文件：`agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`

- `toolExecuteStart` 改 4 参通知器，用一个局部 `report()` 把两段一起上抛。
- `ToolExecReq` 加 `payload` 字段；`applyValueOutcome` 调新增的 `applyAttachmentsPayload`
  —— 取脚本返回值里的 `attachments` 数组，包成 `{"attachments":…}` 当第二段。
- `onCallToolDone` / `settleCall` 收两段：有元数据时 Promise resolve 成
  `{...元数据, text: 正文}`（元数据里已有 `text` 时不覆盖）；无元数据保持原行为
  （能解析成 JSON 给值，否则给字符串）。
- 同步返回的 `done` 分支改用新通知器的 `done` / `done_text`。

### 步骤 6: 测试与编译验证

改动文件：`agent/test/core/test_filesystem_tools.cpp`、`agent/test/plugin/test_plugin_bridge.cpp`、
`agent/test/plugin/test_plugin_sdk.cpp`、`agent/test/plugin/test_plugin_runtime.cpp`、
`agent/test/plugin/test_plugins.cpp`、`agent/test/plugin/test_plugin_abi_c17.c`、
`agent/test/plugin/test_plugin_host_mount.cpp`、`agent/test/plugin/dso_plugins/test_start_fail/test_start_fail.cpp`、
`agent/plugins/example_js/plugin.js`

- `test_filesystem_tools.cpp`：原信封往返测试改成 `test_tool_result_metadata_payload`
  —— `fromParts` ↔ `metadataPayloadJson` 往返、帧编解码往返、坏元数据 JSON 不影响正文；
  真实插件链路的断言改成"`read_media` 的附件确实到达 `ToolResult.attachments`"。
- `test_plugin_bridge.cpp`：`NotifyProbe` 改成三段回调（正文 / 元数据 / 单段），
  断言 `probe.payload` 与 `probe.result`。
- `test_plugin_sdk.cpp`：新增 `ToolNotifyProbe` 覆盖 `fast_tool` / `blocking_tool` / `polled_tool`
  两条上报路径。
- `test_plugin_runtime.cpp`：`fakeTool` 与 `CallbackState::done` / `onOpDone` 改成两段，
  `setCallback` 走 `onOpDone`。
- `test_plugins.cpp`：4 处 `AgentxxPluginToolSpec` 初始化、5 处回调签名；新增 JS 附件用例。
- `test_plugin_abi_c17.c`：版本断言改 6，补 `AgentxxPluginToolNotify` 的 32 字节与字段偏移断言。
- `example_js/plugin.js`：新增 `js_media_demo` 工具，返回 `{status, text, attachments}`。

验证（Windows Debug）：

```bash
# 1. 内层 lib 编译 + 刷新安装头 (改了 lib 头必须先做这一步, 否则 test/client 用旧头)
cmake --build agent/build/windows-debug/agentxx_lib_repo-prefix/src/agentxx_lib_repo-build \
  --config Debug --target agentxx_static
cmake --install agent/build/windows-debug/agentxx_lib_repo-prefix/src/agentxx_lib_repo-build --config Debug

# 2. 插件全部重编 (API 代次变了, 旧 DLL 会被拒)
pwsh -NoProfile -File agent/script/build_plugins.ps1 -KeepGoing     # 25 个 ok
pwsh -NoProfile -File agent/script/build_plugins.ps1 example_js

# 3. 顶层重配 + 构建
cmake agent/build/windows-debug            # -DPLUGINXX_API_VERSION=6 生效
cmake --build agent/build/windows-debug --config Debug --parallel 16
```

测试结果（单模块，全部 0 失败）：`plugin_runtime` 730、`plugin_sdk` 134、`plugin_bridge` 193、
`plugin_host_mount` 146、`plugin_multi_mount` 47、`filesystem` 285、`plugins` 708、
`boundaries` 19、`client_plugins` 835、`toolcall_args` 256、`toolcall_parallel` 58、
`event_stream` 34、`event_delivery` 62、`event_bridge` 241、`permission` 45、
`async_work` 138、`async_tools` 178、`plugin_agent_runtime` 221、`plugin_example_runtime` 50、
`ffi_c_api` 208、`agent` 223。

全量：`passed=43031 failed=0`。

首轮全量是 `passed=43024 failed=7`，7 条全在 `tui_scroll`（998/999/1000/1111/1112/1134/1135 行），
与本次改动无关：`tui_i18n.cpp` 的 `msg.attachOpen` 中文是 ASCII 括号 `(点击打开)`，
而 `tui.md` §2.18 与测试都按全角 `（点击打开）`。按文档与测试改成全角后
`tui_scroll` 1004 项全过、全量转绿；这行改动不在原计划内，是为让测试全过顺带修的。

### 步骤 7: 文档与收尾

改动文件：`docs/zh-cn/design/plugins.md`、`AGENTS.md`

- `plugins.md`：§8.1 tools 表版本改 4 并补 v4 说明；"统一异步操作模型"条目补两段上报；
  代次段落改 v6；新增 **§8.4 工具结果的两段上报**（三个入口表、为何不用信封、
  帧格式、`call_tool_result`）；§12 补 JS `callTool` 的 resolve 形态与
  `{status, text, attachments}` 返回值写法。
- `AGENTS.md`：接口表代次 5 → 6、`agentxx.agent.tools` 版本 4；"工具结果与错误约定"
  段落补两段上报的约定。

### 与计划的偏差

1. **OpCore 只有一个完成字符串**：计划里假设两段能直接交给 OpCore，实际只能交一个串，
   于是加了长度前缀帧（`tool_result_frame.h`）。帧只在本进程内流动，不影响插件 ABI。
2. **通知器三个入口而不是一个**：`done_text` 让没有元数据的绝大多数调用点不组帧，
   `done_frame` 给 SDK 内部的轮询泵直接递整帧。
3. **通知器不能碰插件侧对象**：`destroyFrame()`（→ cleanup → delete Job）**先于**
   `notifyHost()` 执行，最初把 `AgentxxPluginToolNotify` 的回调写成 SDK 内引用 Job 的
   lambda，ASan 直接报 heap-use-after-free。现在三个入口都由 host 侧提供，Job 只存值。
4. **`call_tool` 不改成返回 `ToolResult`**：既有调用点很多，改返回类型会大面积重写；
   保持返回 `std::string`，另加 `call_tool_result`。
5. **正文的 UTF-8 转换留在主程序**：插件侧不猜编码，`mapPluginOpResult` 里由
   `needsUtf8Conversion` 判定后再 `autoConvertToUtf8`，合法 UTF-8 原样透传。

### 遗留事项

- `call_tool_result` 目前只有编译覆盖，没有专门用例（插件互调取附件这条路径）。
- 旧的插件 DLL 必须按 API v6 重建，否则加载时被拒（内置 25 个插件已全部重编）。
- 计划外的顺带修改：`agent/client/src/io/tui/tui_i18n.cpp` 的 `msg.attachOpen` 中文
  改为全角 `（点击打开）`（与 `tui.md` §2.18 和测试一致，修好 `tui_scroll` 的 7 条既有失败）；
  因此 client 也重编了一次，`exec/agentxx.exe` 已刷新。






