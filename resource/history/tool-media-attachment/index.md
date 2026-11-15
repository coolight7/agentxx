# 支持模型通过文件路径读取多模态附件 (tool-media-attachment)

## 目标

为 agentxx 补充实现多模态工具结果机制：允许工具（如文件系统插件、截屏工具等）产出多模态附件（图片、音频、视频），并通过新增工具 `agentxx_filesystem_read_media` 支持模型通过本地文件路径自主读取媒体附件，将附件安全、高效地投递给 LLM 上下文与客户端展示层。

## 范围

- 基础设施层：`agentxx::util::ToolResult` 增加附件容器支持；插件 Tool C ABI 改为**两段上报**（正文原样字节 + 元数据 JSON），正文不再进 JSON；
- 执行与协议层：`ToolcallWrapNode` 支持收集工具输出附件，在线程池中异步转为 Base64 Data URL，以紧随 tool 结果的观察型 `role: "user"` 消息安全注入 LLM 上下文，避开上游 OpenAI 兼容接口对 `role: "tool"` 多模态的 400 校验限制；
- 插件层：在 `agentxx_filesystem` 插件中实现并注册新工具 `agentxx_filesystem_read_media`（支持 `path` 参数），声明读路径权限，进行类型与大小配额检查；
- 展示层：`EventBridge` 支持提取 tool 附件回填至 `ViewMessage`，供客户端（TUI 等）直接展示媒体卡片；
- 验证：单元测试、插件编译及真实链路测试。

## 状态

已完成。设计方案见 [plan.md](plan.md)，实施记录见 [work.md](work.md)。

## 关键决策

1. **两段上报，正文不进 JSON**（最终方案，替代阶段一的信封）：`AgentxxPluginToolNotify` 的 `done` 收 `result` / `payload` 两段 —— `result` 是 `ToolResult.text`，原样裸字节传，主程序再按 `needsUtf8Conversion` 决定要不要 `autoConvertToUtf8`（本地代码页输出因此无损）；`payload` 是除正文外的元数据 JSON（如 `{"attachments":[…]}`），没有元数据就是空串，零开销。`agentxx.agent.tools` 抬到 v4、全局 API 代次抬到 v6。
   阶段一曾用 `__agentxx_tool_media__` 信封把元数据塞进正文串，问题正是正文被迫参与 JSON 转义/转码，主程序再也拿不回原始字节，故在阶段二整体删除（`packEnvelope` / `unpackEnvelope` 已不存在）。
   `PluginxxOpCore` 的完成包只有一个字符串且第三方库不可改，因此两段在 SDK ↔ 主程序之间用 `[uint32 正文长][uint32 元数据长][正文][元数据]` 的裸帧承载（`plugin/tool_result_frame.h`），帧只在本进程内流动；`done_text` / `done_frame` 两个入口让没有元数据的调用点不必组帧；通知器入口全部由 host 侧提供、Job 只存值 —— `destroyFrame()`（→ cleanup → delete Job）先于 `notifyHost()`，通知器引用 Job 就是 use-after-free（ASan 已实测）。
2. **LLM 上下文安全注入**：为保证 100% 兼容上游 OpenAI 兼容接口（大部分上游对 `role: "tool"` 只认 string，传入 content array 会直接 400 报错），框架采用“标准 tool 结果文本 + 紧跟其后的观察型 user 消息（带 `AutoInserted` 标记与 `image_urls`）”的双段注入策略；
3. **展示与上下文解耦**：展示层 `ViewMessage` 直接将附件挂在 Tool 行上，渲染端展示原汁原味的工具附件卡片；LLM 上下文层则消费 user 观察消息，两层各司其职；
4. **配额与能力受控**：执行前通过 `PermissionMiddleware` 检查路径读取权限；执行后检查模型是否声明了多模态能力开关（`image_input` 等），未声明时提前拦截并不转换超大 Base64，避免浪费内存与上游报错。

## 结论

- 扩展了 `ToolResult`，支持携带多模态附件；插件与主程序之间的通道改成 **两段上报**：正文原样裸字节（不在插件侧猜编码，主程序按需 `autoConvertToUtf8`）+ 元数据 JSON（附件等，空则零开销）。阶段一的 JSON 信封已整体删除；
- 两段要穿过只有一个字符串完成包的 `PluginxxOpCore`，用长度前缀裸帧承载（`plugin/tool_result_frame.h`），只在本进程内用；`done` / `done_text` / `done_frame` 三个入口 + `makeToolNotify` 把主程序侧改动压在几处；
- `agentxx.agent.tools` 抬到 v4（签名破坏性改动，`MIN_VERSION` 同步）、全局 API 代次抬到 v6，25 个内置插件（含 JS 引擎与 `example_js`）已全部重建；
- 在 `agentxx_filesystem` 插件中上线了 `agentxx_filesystem_read_media` 工具（入参 `path`），完善了类型、大小、存在性校验以及路径读权限声明；
- 在 `ToolcallWrapNode` 中实现了基于模型能力白名单的多模态异步转码与观察型 user 消息注入机制，规避了上游网关对 tool 消息多模态报 400 的风险；
- 在 `EventBridge` 中补齐了 Tool 消息展示层的附件回填；
- 插件互调新增 `call_tool_result`（取完整 `ToolResult`，失败看状态不抛），`call_tool` 保持返回正文；JS 插件可直接返回 `{status, text, attachments}`，调用方 `callTool` 在有元数据时 resolve 成 `{...元数据, text}`；
- 验证：`filesystem` 285 项、`plugin_runtime` 730 项、`plugin_sdk` 134 项、`plugins` 708 项、`client_plugins` 835 项等均 0 失败；全量 `passed=43031 failed=0`。
  全量首跑有 7 条 `tui_scroll` 失败，是 `tui_i18n.cpp` 的附件文案用了 ASCII 括号
  （文档与测试都是全角）的既有问题，顺带改成全角后全绿（计划外的一行改动，见 work.md）。

