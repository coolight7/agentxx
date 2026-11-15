# 实施记录: 模型通过文件路径读取多模态附件 (tool-media-attachment)

## 当前进度

- [x] 阶段一: 多模态附件读取原型验证完成 (基础通道打通)
- [ ] 阶段二: 重构 Tool C API 为双参数分离返回 (`result` raw text + `payload` metadata JSON)
  - [ ] 步骤 1: `plugin_api.h` 定义 `AgentxxPluginToolNotify` 与 `AgentxxPluginToolCallback`
  - [ ] 步骤 2: `ToolResult` 移除信封，增加 `metadataPayloadJson` 与 `fromParts`
  - [ ] 步骤 3: 插件 SDK `plugin_kit.h` 适配双参数分离上报与互调
  - [ ] 步骤 4: 主程序 `plugin_manager_adapters.cpp` 与 `plugin_manager_capability.cpp` 适配
  - [ ] 步骤 5: JS 插件引擎 `agentxx_javascript_engine.cpp` 适配
  - [ ] 步骤 6: 更新测试用例并全量编译验证
  - [ ] 步骤 7: 清理过时代码与收尾总结

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





