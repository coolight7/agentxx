# 支持模型通过文件路径读取多模态附件 (tool-media-attachment)

## 目标

为 agentxx 补充实现多模态工具结果机制：允许工具（如文件系统插件、截屏工具等）产出多模态附件（图片、音频、视频），并通过新增工具 `agentxx_filesystem_read_media` 支持模型通过本地文件路径自主读取媒体附件，将附件安全、高效地投递给 LLM 上下文与客户端展示层。

## 范围

- 基础设施层：`agentxx::util::ToolResult` 增加附件容器支持，定义跨插件 C ABI 边界的零开销自愈/解包信封；
- 执行与协议层：`ToolcallWrapNode` 支持收集工具输出附件，在线程池中异步转为 Base64 Data URL，以紧随 tool 结果的观察型 `role: "user"` 消息安全注入 LLM 上下文，避开上游 OpenAI 兼容接口对 `role: "tool"` 多模态的 400 校验限制；
- 插件层：在 `agentxx_filesystem` 插件中实现并注册新工具 `agentxx_filesystem_read_media`（支持 `path` 参数），声明读路径权限，进行类型与大小配额检查；
- 展示层：`EventBridge` 支持提取 tool 附件回填至 `ViewMessage`，供客户端（TUI 等）直接展示媒体卡片；
- 验证：单元测试、插件编译及真实链路测试。

## 状态

已完成。设计方案见 [plan.md](plan.md)，实施记录见 [work.md](work.md)。

## 关键决策

1. **跨 C ABI 零破坏**：不改动 `AgentxxPluginToolsIface` 等纯 C ABI 接口表，使用结构化 JSON 信封在 `notify->done` 的 payload 层面传递多模态元数据，由主程序 `mapPluginOpResult` / `ToolResult` 自动拆包；
2. **LLM 上下文安全注入**：为保证 100% 兼容上游 OpenAI 兼容接口（大部分上游对 `role: "tool"` 只认 string，传入 content array 会直接 400 报错），框架采用“标准 tool 结果文本 + 紧跟其后的观察型 user 消息（带 `AutoInserted` 标记与 `image_urls`）”的双段注入策略；
3. **展示与上下文解耦**：展示层 `ViewMessage` 直接将附件挂在 Tool 行上，渲染端展示原汁原味的工具附件卡片；LLM 上下文层则消费 user 观察消息，两层各司其职；
4. **配额与能力受控**：执行前通过 `PermissionMiddleware` 检查路径读取权限；执行后检查模型是否声明了多模态能力开关（`image_input` 等），未声明时提前拦截并不转换超大 Base64，避免浪费内存与上游报错。

## 结论

- 扩展了 `ToolResult`，支持携带多模态附件，并通过纯 C ABI 兼容的 JSON 信封打通了插件与主程序之间的多模态数据传输通道；
- 在 `agentxx_filesystem` 插件中上线了 `agentxx_filesystem_read_media` 工具（入参 `path`），完善了类型、大小、存在性校验以及路径读权限声明；
- 在 `ToolcallWrapNode` 中实现了基于模型能力白名单的多模态异步转码与观察型 user 消息注入机制，规避了上游网关对 tool 消息多模态报 400 的风险；
- 在 `EventBridge` 中补齐了 Tool 消息展示层的附件回填；
- 相关单元测试（273 项）与边界测试全部 100% 通过。

