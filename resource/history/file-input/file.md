# 多模态文件输入与 TUI 交互设计方案

> **目标**: 为 Agentxx 构建端到端的多模态（图像、音频、视频）文件输入架构，并在 TUI（终端用户界面）中实现基于模型能力驱动的交互式文件选择、预览管理与消息呈现。
>
> **已确认决策**:
> 1. **交互入口**: TUI 中采用模态文件选择弹窗（FilePickerOverlay），**不绑定键盘快捷键**，在输入框右侧新增点击按钮（`[+ 📎︎︎]`）触发打开。
> 2. **能力门控与过滤**: 在模型配置（YAML / `ModelConfig`）中显式声明是否支持 `image_input`、`audio_input`、`video_input`。仅当当前选中的模型支持其中至少一种多模态输入时，输入框右侧才展示/启用文件选择按钮；同时，文件选择弹窗严格依据当前模型所支持的媒体类型对文件进行过滤和可选性约束。
> 3. **全链路架构**: 客户端本地读取并转换为 RFC 2397 Data URL（Base64 编码），通过增强的 `WireUserInput` 传输至服务端；底层与已支持多模态的 `neograph::ChatMessage` 和 `OpenAIProvider` / `AnthropicProvider` 对齐。

---

## 1. 背景与现状剖析

### 1.1 底层已就绪能力（NeoGraph & Provider）
通过代码勘查，Agentxx 底层通信与模型适配层已具备完善的多模态数据转换逻辑：
- **数据结构**: `neograph::ChatMessage`（见 `agent/third_party/neograph/include/neograph/types.h`）原生包含 `image_urls`、`audio_urls`、`video_urls` 容器。
- **OpenAI 兼容协议**: `neograph::messages_to_json` 已实现多模态消息拆分为 `image_url`、`input_audio`（自动解析 RFC 2397 Data URL 并提取音频格式）、`video_url` 的转换。
- **Anthropic 协议**: `agentxx::server::AnthropicProvider` 已实现将多模态数据转换为 `image`、`audio`、`video` content block。

### 1.2 现有断层与改造点
1. **模型配置缺乏能力标识**: `ModelConfig` 未区分模型是否具备视觉、音频、视频理解能力，客户端无法获知当前模型支持哪些多模态输入。
2. **传输协议单一**: `WireUserInput` 仅支持 `text` 和 `model` 字段；服务端排队机制 `MessageQueueItem` 及驱动循环仅支持文本流转。
3. **Agent 执行层硬编码**: `BaseAgent::runTurnAsync` 仅接收 `std::string_view userInput`，直接向 `session->llmMessages` 组装纯文本 user message，忽略了附件。
4. **会话历史与展示结构缺失**: `ViewMessage` 和 `TUIMessage` 仅包含 `text`，无附件结构，导致消息持久化（SQLite）和历史同步（`WireSync`）丢失多媒体上下文。
5. **TUI 缺乏直观的文件指定与管理界面**: 输入框仅有文本编辑，缺少多媒体附加入口、待发附件托盘以及模型能力的感知联动。

---

## 2. 模型配置与多模态能力感知体系

### 2.1 YAML 配置文件与 `ModelConfig` 扩展
在 `agentxx-config.yaml` 的模型条目中新增 `image_input`、`audio_input`、`video_input` 字段（默认均为 `false`，完全向后兼容）：

```yaml
models:
  - name: gpt-4o
    type: openai
    model_name: gpt-4o
    base_url: https://api.openai.com/v1
    api_key: ${OPENAI_API_KEY}
    image_input: true      # 支持图片输入
    audio_input: true      # 支持音频输入
    video_input: false     # 暂不支持视频输入

  - name: claude-3-5-sonnet
    type: anthropic
    model_name: claude-3-5-sonnet-20241022
    api_key: ${ANTHROPIC_API_KEY}
    image_input: true      # 支持图片输入
    audio_input: false
    video_input: false

  - name: deepseek-chat
    type: openai
    model_name: deepseek-chat
    base_url: https://api.deepseek.com/v1
    api_key: ${DEEPSEEK_API_KEY}
    image_input: false     # 纯文本模型
    audio_input: false
    video_input: false
```

在 `agent/lib/include/agentxx/agent/config.h` 中为 `ModelConfig` 扩充：
```cpp
class ModelConfig {
public:
    // ... 原有字段 ...

    /// 是否支持图像输入 (多模态)
    bool imageInput = false;
    /// 是否支持音频输入 (多模态)
    bool audioInput = false;
    /// 是否支持视频输入 (多模态)
    bool videoInput = false;

    /// 是否支持任何一种多模态输入
    bool hasMultimodalInput() const noexcept {
        return imageInput || audioInput || videoInput;
    }
};
```
在 `agent/client/src/config_loader.cpp` 的 `loadYamlConfig` 中解析这三个布尔选项。

### 2.2 协议层能力下发与客户端状态联动
为了让独立的客户端（无论本地直连模式或远程 WebSocket 模式）实时感知当前选中的模型能力：

1. **协议结构扩充 (`agent_io_transport.h`)**：
   ```cpp
   struct ModelCapabilityInfo {
       std::string name;
       bool imageInput = false;
       bool audioInput = false;
       bool videoInput = false;
   };

   struct WireModelInfo {
       std::string                      currentModel;
       std::vector<std::string>         models;
       std::vector<ModelCapabilityInfo> capabilities; // 各模型的多模态能力清单
   };
   ```
2. **服务端填充 (`SessionServerAgentIO::onPeerMessage`)**：
   在响应 `WireGetModel`、`WireHelloAck` 时，遍历 `availableModels` 提取每个模型的 `imageInput`、`audioInput`、`videoInput` 并随模型列表下发。
3. **客户端状态同步 (`TUIRenderState`)**：
   客户端接收到能力列表后，缓存在 `TUIRenderState::modelCapabilities` 中。当用户切换当前模型时，实时计算当前活动模型的能力，并触发界面局部刷新。

---

## 3. 全链路多模态输入架构

### 3.1 核心数据结构设计

#### (1) 通用附件对象 `MediaAttachment` (`conversation_types.h`)
```cpp
enum class MediaType : uint8_t {
    Image,
    Audio,
    Video
};

struct MediaAttachment {
    MediaType   type        = MediaType::Image;
    std::string displayName; // 文件名 (展示用，如 "chart.png")
    std::string mimeType;    // MIME 类型 (如 "image/png", "audio/wav")
    std::string pathOrUrl;   // 本地绝对路径或 HTTP(S) URL
    std::string dataUrl;     // RFC 2397 格式: "data:<mime>;base64,<payload>"
    uint64_t    sizeBytes   = 0;

    neograph::json toJson() const;
    static MediaAttachment fromJson(const neograph::json& j);
};
```
- **职责分离**:
  - `displayName`、`pathOrUrl`、`sizeBytes`: 供 UI 紧凑呈现、会话持久化和本地点击调起查看器。
  - `dataUrl`: 传输二进制 Base64 数据，供服务端直接送入 LLM API。

#### (2) 传输协议与排队结构扩充
- `WireUserInput` (`agent_io_transport.h`):
  ```cpp
  struct WireUserInput {
      std::string                  sessionId;
      std::string                  text;
      std::string                  model;
      std::vector<MediaAttachment> attachments; // 携带附件
  };
  ```
- `MessageQueueItem` (`session_server_agent_io.h`):
  ```cpp
  struct MessageQueueItem {
      std::string                  id;
      std::string                  text;
      std::string                  model;
      std::vector<MediaAttachment> attachments; // 排队项保留附件
      int64_t                      createdAtMs = 0;
  };
  ```
- `ViewMessage` (`conversation_types.h`):
  ```cpp
  struct ViewMessage {
      // ... 通用字段 ...
      std::vector<MediaAttachment> attachments; // 展示端附件
  };
  ```
  在 `ViewMessage::toJson()` / `fromJson()` 中支持 `attachments` 序列化，确保断线重连、`WireSync` 与 SQLite 存储完整还原。

### 3.2 数据流与执行流转
```text
[ 用户点击右侧按钮选择文件 ]
          │
          ▼
[ FilePickerOverlay 动态过滤并选中文件 ]
          │
          ▼
[ Client 本地预检: 大小限制、MIME 探测、Base64 编码为 Data URL ]
          │
          ▼
[ 挂载入 InputComponent 待发附件托盘 (Attachment Tray) ]
          │
          ▼ (用户敲入提示词，按 Enter 发送)
[ TUIClientAgentIO 发送 WireUserInput(text, model, attachments) ]
          │
          ▼ (Transport: Channel / WebSocket)
[ SessionServerAgentIO::pushMessageQueueItem 压入队列 ]
          │
          ▼ (驱动循环弹出)
[ BaseAgent::runTurnAsync(sessionId, text, attachments, io, model) ]
          │
          ├─► 生成 ViewMessage(User, text, attachments) 插入会话历史
          │
          ├─► 组装 neograph::ChatMessage (按类型填入 image_urls / audio_urls / video_urls)
          │
          └─► 写入 session->llmMessages -> 触发 NeoGraph ModelCall
                    │
                    ▼
          [ OpenAI / Anthropic Provider 自动序列化为 API Payload ]
```

### 3.3 编码、安全与持久化策略
1. **客户端侧编码原则**:
   - 本地文件系统访问仅存在于客户端运行的机器。如果采用远程 WebSocket 部署，服务端无法读取客户端路径。因此，**文件读取、MIME 判定、大小安全校验与 Base64 Data URL 转换一律在 Client 侧完成**。
2. **大小硬限制（客户端预拦截）**:
   - 单张图像限制：<= 10MB
   - 单个音频限制：<= 25MB
   - 单个视频限制：<= 50MB
   - 单次对话总附件数：<= 5
   - 超过限制时在客户端直接弹出 Toast 告警，拒绝挂载，防止过大 Payload 导致内存激增或网络超时。
3. **持久化与上下文压缩优化**:
   - **SQLite 持久化轻量化**: `ViewMessage` 落盘时，仅持久化 `attachments` 的元数据（`displayName`、`pathOrUrl`、`mimeType`、`sizeBytes`），清空庞大的 `dataUrl`，避免数据库体积膨胀数十倍。
   - **上下文压缩 (Summarization)**: 在执行上下文压缩时，旧轮次消息的多模态附件在总结提炼后降级为纯文本标签（如 `[用户附带了图片: architecture.png]`），不再保留 Base64，避免耗尽上下文 Token。

---

## 4. TUI 交互与输入栏改造

### 4.1 输入框右侧按钮与状态控制
在 `agent/client/include/agentxx-client/io/tui/components/input_bar.h` 与 `input_bar.cpp` 中改造：

#### (1) 布局呈现
```text
┌────────────────────────────────────────────────────────────────────────┐
│  > 请根据架构图分析模块间依赖关系...                     [+ 📎︎︎ 附件]   │
└────────────────────────────────────────────────────────────────────────┘
```
- **条件显隐**:
  - `currentModel.hasMultimodalInput() == false`（纯文本模型）：右侧不显示按钮，保持干净的纯文本输入状态。
  - `currentModel.hasMultimodalInput() == true`（支持多模态）：在输入框内部右侧渲染 `[+ 📎︎︎ 附件]` 或 `[+ 📎︎︎]`，文本使用主题的高亮色（`theme.accentColor`）。
- **事件绑定**:
  - **不绑定键盘快捷键**（完全避免热键冲突）。
  - 通过 FTXUI 的 `Box::Contain(x, y)` 监听鼠标左键释放事件（Released），点击命中时触发回调 `config.onOpenAttachPicker()`。

### 4.2 待发附件挂载托盘 (Attachment Tray)
在输入框上方动态扩展附件挂载托盘：
```text
┌────────────────────────────────────────────────────────────────────────┐
│ 📎︎︎ 待发附件 (2):                                                        │
│  [📷︎ architecture.png 1.2MB ✕]   [🎵︎ requirement.wav 420KB ✕]          │
├────────────────────────────────────────────────────────────────────────┤
│  > 请根据架构图和录音分析重构要点...                     [+ 📎︎︎ 附件]   │
└────────────────────────────────────────────────────────────────────────┘
```
- **托盘交互特性**:
  - 初始无附件时，高度为 0，不占空间。
  - 用户选定文件后，上方展开标签栏，展示格式图标（📷/🎵︎/🎬）、文件名、缩略大小，以及删除按钮 `✕`。
  - 鼠标点击 `✕` 可随时移除对应附件。
  - 用户按 Enter 发送时，将当前文本与托盘内的所有附件打包发送，发送成功后清空文本框与托盘。

---

## 5. 多模态文件选择弹窗 (FilePickerOverlay) 设计

点击输入框右侧的 `[+ 📎︎︎]` 按钮后，通过 `modal_->pushModal(...)` 弹出全屏居中的模态文件选择弹窗。

### 5.1 动态类型限制与过滤
弹窗初始化时接收当前模型的 `ModelCapabilityInfo`，动态计算允许的后缀白名单：

| 模型支持项 | 允许选择的文件扩展名 |
| :--- | :--- |
| `image_input == true` | `.png`, `.jpg`, `.jpeg`, `.webp`, `.gif`, `.bmp` |
| `audio_input == true` | `.wav`, `.mp3`, `.ogg`, `.m4a`, `.aac`, `.flac` |
| `video_input == true` | `.mp4`, `.mov`, `.webm`, `.mkv` |

### 5.2 弹窗界面与操作机制
```text
┌── 选择文件 [当前模型支持: 图像 📷︎ | 音频 🎵︎] ──────────────────────────┐
│ 路径: /home/user/workspace/agentxx                                    │
│ 过滤: [                                                        ]       │
├───────────────────────────────────────────────────────────────────────┤
│  📁 [..] 上级目录                                                     │
│  📁 assets/                                                           │
│  📷︎ ui_layout.png                                         (420 KB)    │
│  📷︎ error_trace.jpg                                       (1.1 MB)    │
│  🎵︎ voice_memo.wav                                        (3.4 MB)    │
│  🎬︎ demo_run.mp4                                 [当前模型不支持视频] │
├───────────────────────────────────────────────────────────────────────┤
│ [↑/↓] 移动光标  |  [Enter] 确认选择/进入目录  |  [Esc] 取消/关闭        │
└───────────────────────────────────────────────────────────────────────┘
```

1. **项目与交互状态**:
   - **目录项**：`📁 [..]` 与子目录，按 Enter 进入下级目录或返回上级。
   - **受支持的多媒体文件**：以高亮颜色显示，包含类型 Emoji 和格式化文件大小；按 Enter 或鼠标点击即完成选择。
   - **不受支持的媒体文件**（例如模型仅支持图片，但目录下存在视频）：以暗灰色（dim）显示，后置标注 `[当前模型不支持视频]`，处于禁用（Disabled）状态，不可选中。
   - **其他非媒体文件**（代码、文档）：默认过滤不显示，避免杂乱。
2. **操作逻辑**:
   - 支持 `↑` / `↓` 键盘导航以及鼠标滚轮滚动。
   - 按 `Esc` 或点击遮罩层外部调用 `modal_->popModal()` 关闭弹窗。
   - 确认选中文件后，弹窗关闭，触发预处理并挂载到输入栏的附件托盘中。

---

## 6. 消息列表 (MessageList) 中的多媒体渲染与打开

用户发送多媒体文件后，在会话消息历史列表（`MessageListComponent`）中进行规范化呈现。

### 6.1 结构化卡片渲染
在对应消息气泡中以卡片展示附件条目：
```text
[User] 15:02:40
┌── 📷︎ 图像附件: ui_layout.png ─────────────────────────────────────┐
│ 尺寸: 1.2 MB  |  类型: image/png  |  状态: 已发送                  │
│ [Enter / 点击] 使用系统查看器打开原文件                           │
└───────────────────────────────────────────────────────────────────┘
请根据架构图分析模块间依赖关系...
```

### 6.2 本地调用系统默认程序打开
点击附件卡片或光标聚焦按 Enter 时，通过调用系统原生接口打开文件：
- **Windows**: `ShellExecuteEx` / `start ""`
- **Linux**: `xdg-open <path>`
- **macOS**: `open <path>`

若文件为远程模式下由模型生成的网络图片或 Base64 产物，客户端先将其落盘至本地临时缓存目录后再调起系统查看器打开。

---

## 7. 代码改动与文件影响清单

| 层级 | 目标文件 | 主要修改内容 |
| :--- | :--- | :--- |
| **模型配置** | `agent/lib/include/agentxx/agent/config.h`<br>`agent/client/src/config_loader.cpp` | 扩充 `ModelConfig` 的 `imageInput`、`audioInput`、`videoInput` 字段及 YAML 解析器 |
| **会话模型** | `agent/lib/include/agentxx/agent/conversation_types.h`<br>`agent/lib/src/agent/wire_protocol.cpp` | 定义 `MediaAttachment`；在 `ViewMessage` 中集成 `attachments` 及 JSON 序列化 |
| **传输协议** | `agent/lib/include/agentxx/agent/io/agent_io_transport.h`<br>`agent/lib/include/agentxx/agent/io/wire_protocol.h` | 扩充 `WireModelInfo`（携带各模型能力）与 `WireUserInput`（携带附件数组） |
| **服务端驱动**| `agent/lib/include/agentxx/agent/io/session_server_agent_io.h`<br>`agent/lib/src/agent/io/session_server_agent_io.cpp`<br>`agent/lib/src/agent/base_agent.cpp` | `MessageQueueItem` 支持附件；`BaseAgent::runTurnAsync` 组装 `neograph::ChatMessage` 多模态 URL |
| **TUI 输入栏**| `agent/client/include/agentxx-client/io/tui/components/input_bar.h`<br>`agent/client/src/io/tui/components/input_bar.cpp` | 输入框右侧根据模型能力条件渲染 `[+ 📎︎︎]` 按钮，监听鼠标点击，集成待发附件托盘 |
| **TUI 弹窗**  | `agent/client/include/agentxx-client/io/tui/components/overlays.h`<br>`agent/client/src/io/tui/components/file_picker_overlay.cpp` (新增) | 实现 `FilePickerOverlay`，支持目录导航、按模型能力动态过滤、选中回调 |
| **TUI 消息渲染**| `agent/client/src/io/tui/components/message_list.cpp` | 渲染消息历史中的多媒体附件卡片，集成系统默认查看器打开动作 |
| **持久化**   | `agent/lib/src/agent/session_store.cpp` | 会话 SQLite 落库时剥离 Base64，仅保留附件路径与元数据，保障存储轻量 |
