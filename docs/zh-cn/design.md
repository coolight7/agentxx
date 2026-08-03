# Agentxx 设计文档

## 目录

- [概述](#概述)
- [功能效果](#功能效果)
- [使用方法](#使用方法)
- [架构设计](#架构设计)
- [代码结构](#代码结构)

---

## 概述

Agentxx 是一个使用 C++23 实现的 AI Agent 框架，编译器启用 C++26/C17 标准。核心设计目标：

- **跨平台**: 支持 Linux x86_64 (含 WSL 扩展)、Windows 10+ x86_64、Android 5.0+
- **多形态编译**: 可编译为独立可执行程序、动态库、静态库，仅依赖基本系统库
- **高并发**: 单线程/多协程交错执行多会话，无需线程锁
- **分层解耦**: Client 负责 UI 渲染与用户交互，Agent (BaseAgent/CodeAgent) 负责会话运行、LLM API 调用、ToolCall 执行
- **多连接模式**: 支持 client+agent 同进程 (线程间 Channel 直连) 和 client 经 WebSocket 连接远程 agent server

---

## 功能效果

### 核心对话能力

- **多轮对话**: 支持完整的多轮对话管理，维护 `fullHistory` (append-only 完整历史) 和 `llmMessages` (可压缩的 LLM 上下文) 双消息集
- **流式输出**: LLM 响应以增量 Delta 事件推送 (TextToken / ThinkingToken / ToolStart / ToolEnd / TurnStart / TurnEnd / NodeStart / NodeEnd)，每个 Delta 携带单调递增 seq 用于重放与同步
- **多模型支持**: 运行时按会话 (thread_id) 动态切换模型，支持 OpenAI Chat Completions、Anthropic Messages、OpenAI Responses (Codex) 三种 Provider 协议
- **上下文压缩**: SummarizationMiddleware 在上下文接近模型 token 上限时自动压缩历史消息，支持 toolcall 输出去重与截断
- **思维链展示**: 支持 LLM 的 thinking/reasoning_content 流式输出与展示
- **节点级事件**: NodeStart/NodeEnd 事件标记 Graph 节点执行生命周期，便于 UI 展示进度

### 工具调用 (ToolCall)

内置丰富的工具集，按功能分类：

| 分类 | 工具 | 说明 |
|------|------|------|
| **文件系统** | `filesystem_list` | 列出文件/文件夹信息 (大小、类型、修改时间)，支持递归 |
| | `filesystem_read_text_file` | 按行读取文本文件，支持 offset/limit |
| | `filesystem_read_binary_file` | 按字节读取二进制文件，返回 base64 |
| | `filesystem_write_file` | 创建/覆盖文件，支持二进制 (base64) 写入 |
| | `filesystem_edit_text_file` | 精确字符串替换编辑文本文件 |
| | `filesystem_glob` | 按 glob 模式搜索文件 |
| | `filesystem_grep` | 按正则/文本搜索文件内容 |
| **命令执行** | `execute_linux_command` | 执行 Linux shell 命令，支持超时控制 |
| | `execute_windows_command` | 执行 Windows 命令 (支持 WSL 下调用) |
| | `execute_python_command` | 执行 Python 命令 |
| | `execute_javascript_command` | 执行 JavaScript 命令 |
| **网络** | `web_search` | 网络搜索 (DuckDuckGo / 模型搜索) |
| | `web_fetch_url` | HTTP GET 获取网页原文 |
| | `web_fetch_url_markdown` | 获取网页并转为 Markdown |
| **知识检索** | `rag_search` | 基于向量相似度的知识库语义搜索 |
| **代码分析** | `codegraph_search` | 按名称搜索代码符号 |
| | `codegraph_context` | 获取符号的定义、调用者、被调用者 |
| | `codegraph_callers` / `codegraph_callees` | 调用图正向/反向追踪 |
| | `codegraph_impact` | 修改影响分析 |
| | `codegraph_index` | 索引目录构建符号数据库 |
| | `codegraph_path` | 查找两符号间的调用链路径 |
| | `codegraph_status` | 索引统计信息 |
| **规划** | `planning_write` | 两层任务规划 (Mermaid 状态图 + Todo List + 备忘录) |
| **子代理** | `sub_agent` | 创建和管理子代理执行委派任务 |
| | `tool_skill_search` | 延迟加载工具/技能的搜索与发现 |
| **跨代理** | `cross_agent_query` | Agent 间 Actor 式通信查询 |
| **数据** | `share_store` | 会话级文本寄存，节省上下文 |
| | `string_html_to_markdown` | HTML 转 Markdown |
| | `string_regexp` | 正则搜索/替换/移除 |
| **系统** | `get_current_datetime` | 获取当前日期时间 |
| | `get_system_core_info` | 获取 CPU/内存/GPU 使用率 |
| **UI 控制** | `ui_control_keyboard_mouse` | Windows 键鼠控制 (仅 Windows) |

工具特性：
- **自动压缩**: 工具输出超过阈值时自动调用 LLM 压缩摘要
- **延迟加载**: 工具初始仅注册名称，经 `tool_skill_search` 检索后才加载全量定义
- **去重机制**: 文件读写等工具支持 SummarizationToolHandle，重复调用时截断旧结果
- **MCP 扩展**: 通过 MCP Client 连接外部 MCP Server，动态注册远程工具 (支持 HTTP SSE 和 stdio 传输)

### 中间件系统

采用栈式中间件架构，在 Graph 节点执行前后插入处理逻辑：

| 中间件 | 功能 |
|--------|------|
| **PermissionMiddleware** | 工具调用权限控制，经事件总线向用户请求授权 (HIL) |
| **SkillMiddleware** | 技能文件 (SKILL.md) 的渐进式发现与加载 |
| **MemoryFileMiddleware** | 上下文文件 (Memory) 读取与缓存，每次模型调用时注入系统提示词 |
| **SummarizationMiddleware** | 上下文 token 统计与自动压缩，防止超出模型上下文窗口 |
| **PlanningMiddleware** | 任务规划状态管理，将 planning 数据注入 system prompt |
| **SubagentSupervisor** | 子代理生命周期管理与结果收集，支持批量并发委派和跨 agent 查询路由 |
| **EventBridge** | 将 GraphEngine 事件翻译为 EventBus 强类型事件 |
| **LogPrint** | 调试日志输出中间件 (条件编译，按配置控制日志级别) |

### 事件系统

- **EventBus**: 强类型事件总线，支持单向事件流 (`EventStream<T>`) 和请求-响应流 (`RequestResponseStream<Req, Resp>`)
- **事件主题**: 按 `Topic` 命名空间常量组织，范围覆盖 `agent.*`、`service.*`、`io.*`
- **订阅机制**: 支持常驻订阅和执行次数限制 (execHit) 的自动移除订阅
- **HIL (Human-in-the-Loop)**: 中断/权限请求经 RequestResponseStream 派发到客户端 UI，支持超时
- **定时器**: EventBus 内置定时器事件流，支持 once/repeat 模式

### 多会话与并发

- **Session 隔离**: 每个 thread_id 独立的 Session，包含 IO、EventBus、ContextStats、CancelToken、模型选择、消息历史
- **SessionStore**: 会话存储，按 thread_id 取/建 Session (仅 agent io_context 线程访问，无需锁保护)
- **活动状态**: Idle / Streaming / ExecutingTool / WaitingInput 四种状态
- **链式哈希**: fullHistory 使用 FNV-1a 链式哈希校验一致性
- **线程绑定与无锁快照**: Session 通过 `bindIoThread()` 绑定 io 线程，`assertIoThread()` 强制校验可变状态 (fullHistory/llmMessages/chainHash) 仅在 io 线程写入；UI 线程通过 `getFullHistoryCopy()` / `getHashInfo()` 等原子快照方法 (基于 `std::atomic<shared_ptr<const T>>`) 只读访问，无需加锁
- **取消/切模型**: UI 线程的取消/切模型操作通过 Wire 消息 (WireCancel/WireSelectModel) 发往 agent 线程处理，避免跨线程竞争
- **异步互斥锁**: `AsyncMutex` 基于 asio concurrent_channel 实现协程感知互斥，不会阻塞线程，适用于协程跨越 co_await 临界区

### 远程通信

- **WebSocket 服务**: AgentServer 提供 WS/WSS 服务，支持 token 鉴权
- **Wire Protocol**: 双向 JSON 消息协议 (Hello/UserInput/Cancel/SelectModel/GetModel/Delta/Sync/InterruptRequest/InterruptResponse/TurnResult/ContextStats/Error/Log/ModelInfo/GetAppendComponentInfo/AppendComponentInfo/GetContext/ContextMessages/Ping/Pong)
- **断线重连**: 客户端自动重连，携带 lastSeq 供增量 Delta 重放，seq 不连续时回退全量 Sync
- **Grace Period**: 断线后会话保持运行的宽限期，避免误取消进行中的轮次
- **进程内直连**: ChannelAgentIOTransport 零序列化 Channel 传输，同进程内 client 与 agent 直连
- **传输层抽象**: `AgentIOTransportBase` 提供统一的 `connect/recv/send/close/alive` 接口，对调用方隐藏传输细节

### 协议支持

| 协议 | 角色 | 说明 |
|------|------|------|
| **OpenAI API** | Client | 兼容 OpenAI Chat Completions API (流式/非流式)，支持 thinking/reasoning_content |
| **Anthropic API** | Client | Anthropic Messages API，支持 extended thinking、tool_use |
| **MCP** | Client + Server | Model Context Protocol，支持 2024-11-05 至 2025-11-25 多版本协商，HTTP SSE + stdio 传输 |
| **A2A** | Client + Server | Agent-to-Agent 协议 v1.0，任务管理 (SendMessage/GetTask/CancelTask/ListTasks) |
| **ACP** | Server | Agent Communication Protocol，stdio 服务模式 |

### 客户端 UI

- **TUI 模式**: 基于 FTXUI 的终端 UI，支持：
  - 消息列表 (User/Assistant/Thinking/Tool/System 角色)
  - Thinking/Tool 消息自动折叠/展开 (执行中展开，完成后折叠)
  - 流式 token 实时渲染
  - 权限请求弹窗
  - 模型选择器 (运行时切换)
  - 右侧边栏 (日志窗口 / 信息面板 / Planning 展示)
  - 待发送消息队列 (执行中排队，轮次结束自动派发)
  - 文件编辑 diff 对比渲染
  - 上下文 token 占用状态栏
  - 主题切换
  - 自动滚动吸附底部 (Scrollable 组件)
- **TUI 渲染模块化**: 将消息列表、侧边栏、浮层、编辑工具渲染拆分到独立文件
- **TUILogSink**: XX_LOG 日志输出接入 TUI 右侧日志面板
- **CLI 模式**: 基于 stdin/stdout 的简洁命令行交互

### 训练系统

- **进化训练**: EvolutionTrainingAgent 实现提示词自动优化
  - 变异策略: 字符级随机变异 + LLM 生成变异
  - 评估: 运行测试用例集，支持精确匹配和 LLM 评分
  - 优化: 基于反馈的 LLM 提示词补丁生成
  - 收敛检测与去重
- **训练配置**: 支持独立的训练模型、评分模型、优化模型

### 扩展能力

| 模块 | 说明 |
|------|------|
| **ScreenCapture** | 屏幕截图与流式捕获 (多屏支持) |
| **AudioStream** | 系统音频/麦克风/程序音频流捕获 |
| **TextSelectionMonitor** | 系统级文本选择事件监听 (Windows UI Automation) |
| **CpuGpuMonitor** | CPU/内存/GPU 使用率查询 |
| **CodeGraphManager** | 代码索引与符号分析 (基于 codegraph-cpp) |

### 依赖注入

- **DependencyContainer**: 轻量级 DI 容器，支持按类型和名称注册/解析依赖
- **工厂方法**: 支持工厂函数注册 (返回 `std::any`)
- **单例管理**: 默认延迟初始化，避免循环依赖
- **有名称注册**: 支持同名不同类型的依赖项

---

## 使用方法

### 编译

```bash
# Linux Debug
bash agent/script/linux_debug_build.sh

# Linux Release
bash agent/script/linux_release_build.sh

# Windows Debug
agent\script\windows_debug_build.bat

# Windows Release
agent\script\windows_release_build.bat

# Android (Linux 交叉编译)
bash agent/script/android_release_build.sh
```

编译产物位于 `agent/build/{platform}-{mode}/exec/` 目录。

### 运行测试

```bash
# 运行所有测试
path/to/agentxx_test

# 遇到错误立即终止
path/to/agentxx_test --fail-fast

# 仅运行指定模块
path/to/agentxx_test string_util regex agent
```

可用测试模块: `string_util` `regex` `diff_util` `events` `concurrency` `misc_fixes` `event_stream` `event_bridge` `interrupt_bus` `subagent_bus` `crossagent` `string_tools` `share_store` `rag_search` `datetime` `filesystem` `command` `web_search` `codegraph` `cpu_gpu` `http` `websocket` `remote_agent` `mcp` `acp` `a2a` `openai_provider` `anthropic_provider` `agent` `screen_capture` `text_selection` `lockless` `session_concurrency`

### 配置文件

配置文件为 YAML 格式 (默认 `{程序运行目录}/agentxx-config.yaml`, 支持 agentxx_cli --config 指定文件路径)，其中部分变量支持 `${VAR}` 环境变量替换：

```yaml
models:
  - name: "my-model"
    type: "openai"              # "openai" / "anthropic" / "openai-responses"
    base_url: "https://api.example.com"
    api_key: "${MY_API_KEY}"    # 从 .env 或系统环境变量解析
    model_name: "gpt-4"
    api_path: ""                # 自定义 API 路径 (如 "/v1/chat/completions"); 空则用默认
    send_thinking: false
    send_temperature: true      # 部分推理模型不接受 temperature, 可设 false 关闭
    connect_timeout: 16
    read_chunk_timeout: 24
    model_context_max_token: 128000
    extra_headers:              # 额外 HTTP 请求头 (如自定义鉴权/网关透传)
      x-custom-header: "value"
    extra_api_config:           # 合并到请求 body 的扩展配置
      temperature: 0.7
    # 输出 token 上限自动发送 params.max_tokens: 普通模型发送 max_tokens,
    # 新模型 (o1/o3/o4/gpt-5 等) 自动切换为 max_completion_tokens 字段

use_model:
  default: "my-model"           # 主模型
  subagent: "my-model"          # 子代理模型 (未指定时用主模型)
  web_search: ""                # 模型搜索 (空则用传统搜索)
  acp: "my-model"               # ACP 服务模式模型
  train: "my-model"             # 训练模型
  train_scorer: "my-model"      # 训练评分模型
  train_optimizer: "my-model"   # 训练优化模型

mcp_servers:
  - namespace: "my_mcp"
    url: "http://localhost:3000/mcp"
```

> **Codex (Responses API) 配置示例**:
> ```yaml
> models:
>   - name: "openai-responses"
>     type: "openai-responses"                       # 使用 OpenAI Responses API (/v1/responses)
>     base_url: "https://api.openai.com"  # 或 ChatGPT Codex 兼容网关
>     api_key: "${CODEX_API_KEY}"
>     model_name: "gpt-5-codex"
>     extra_api_config:                   # 可选覆盖: 推理强度 / 是否落盘等
>       reasoning:
>         effort: "high"
> ```

环境变量加载优先级: `--env 覆盖文件` > `系统环境变量` > `.env 文件`

### 命令行使用

```bash
agentxx_cli [mode] [options]
```

**模式:**

| 模式 | 说明 |
|------|------|
| `tui` | TUI 交互模式 (默认) |
| `cli` | 命令行 stdio 交互模式 |
| `server` | 启动 WebSocket agent 服务 |
| `acp` | ACP stdio 服务模式 |
| `train` | 训练模式 |

**选项:**

| 选项 | 说明 |
|------|------|
| `-h, --help` | 显示帮助 |
| `--config <path>` | 配置文件路径 (默认: agentxx-config.yaml) |
| `--env <path>` | 覆盖式环境变量文件路径 |
| `--agent <url>` | 远程 agent server 地址 (ws://host:port/agent) |
| `--token <token>` | 认证 token |
| `--model <model>` | 远程模型名称 |
| `--host <host>` | 服务监听地址 (默认: 127.0.0.1) |
| `--port <port>` | 服务监听端口 (默认: 7007) |
| `--ssl-cert <file>` | SSL 证书文件路径 |
| `--ssl-key <file>` | SSL 私钥文件路径 |

**典型用法:**

```bash
# 本地 TUI 模式 (同进程 client + agent)
agentxx_cli tui --config agentxx-config.yaml

# 本地 CLI 模式
agentxx_cli cli --config agentxx-config.yaml

# 启动 WebSocket 服务
agentxx_cli server --host 0.0.0.0 --port 17000 --config agentxx-config.yaml

# 连接远程 agent (TUI)
agentxx_cli tui --agent ws://192.168.1.100:17000/agent --token xxx

# 连接远程 agent (CLI)
agentxx_cli cli --agent ws://192.168.1.100:17000/agent?token=xxx

# ACP stdio 服务
agentxx_cli acp --config agentxx-config.yaml

# 训练模式
agentxx_cli train --config agentxx-config.yaml
```

### 作为库使用

```cpp
#include "agentxx/agent/code_agent.h"

auto config = std::make_shared<agentxx::agent::AgentConfig>();
config->model.baseUrl   = "https://api.openai.com";
config->model.apiKey    = "sk-...";
config->model.modelName = "gpt-4";

agentxx::agent::CodeAgent agent(config);

asio::co_spawn(*agent.ioCtx, [&]() -> asio::awaitable<void> {
    co_await agent.init();

    // 单轮对话
    auto result = co_await agent.runSingleInputAsync("thread_1", "Hello!");

    // 多轮对话
    auto turn1 = co_await agent.runConversationTurnAsync("thread_1", "Hi", true, io);
    auto turn2 = co_await agent.runConversationTurnAsync("thread_1", "Tell me more", false, io);

    // 非流式调用
    std::vector<neograph::ChatMessage> msgs = {
        {.role = "system", .content = "You are helpful."},
        {.role = "user", .content = "Hello"},
    };
    auto output = co_await agent.runNonStreamAsync("thread_2", msgs);
}, asio::detached);

agent.ioCtx->run();
```

---

## 架构设计

### 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        Client 层                                │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────────────────┐  │
│  │ AgentTUI │  │AgentStdIO│  │   Remote Client (WS)         │  │
│  │  (FTXUI) │  │ (stdio)  │  │   WsAgentIOTransport         │  │
│  └────┬─────┘  └────┬─────┘  └──────────┬───────────────────┘  │
│       │              │                   │                      │
│       └──────────────┼───────────────────┘                      │
│                      │ AgentIOBase                              │
│                      │ (onDelta/onSync/getInput/handleInterrupt │
│                      │  registerOnBus/sendToPeer/requestCancel) │
├──────────────────────┼──────────────────────────────────────────┤
│               Transport 层 (AgentIOTransportBase)               │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  ChannelAgentIOTransport (进程内, 零序列化 Channel)      │    │
│  │  WsAgentIOTransport (跨进程/设备, JSON over WebSocket)   │    │
│  │  connect() / recv() / send() / close() / alive()        │    │
│  └─────────────────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────────────────┤
│                        Agent 层                                 │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  AgentServer (WS 服务) / SessionController (会话驱动)    │    │
│  │  SessionController: delta 缓冲/重连重放/grace period    │    │
│  └──────────────────────┬──────────────────────────────────┘    │
│                         │                                       │
│  ┌──────────────────────▼──────────────────────────────────┐    │
│  │              BaseAgent / CodeAgent                       │    │
│  │  ┌─────────────────────────────────────────────────┐    │    │
│  │  │              GraphEngine (ReAct Loop)            │    │    │
│  │  │                                                  │    │    │
│  │  │  __start__ → agent_start → llm → [has_tools?]   │    │    │
│  │  │                              ↑         │         │    │    │
│  │  │                              └── tools ←┘         │    │    │
│  │  │                                        │         │    │    │
│  │  │                                   agent_end      │    │    │
│  │  │                                        │         │    │    │
│  │  │                                     __end__      │    │    │
│  │  └─────────────────────────────────────────────────┘    │    │
│  │                                                          │    │
│  │  BaseAgent: 核心基础设施 + ReAct 循环 + 会话执行         │    │
│  │  CodeAgent: 继承 BaseAgent, 添加编程工具/中间件          │    │
│  │                                                          │    │
│  │  ┌────────────┐ ┌────────────┐ ┌────────────────────┐   │    │
│  │  │ ModelCall  │ │ Toolcall   │ │ AgentStart/End     │   │    │
│  │  │ WrapNode   │ │ WrapNode   │ │ WrapNode           │   │    │
│  │  └─────┬──────┘ └─────┬──────┘ └────────┬───────────┘   │    │
│  │        │              │                  │               │    │
│  │  ┌─────▼──────────────▼──────────────────▼───────────┐   │    │
│  │  │           Middleware Stack (栈式中间件)             │   │    │
│  │  │  Permission → Skill → MemoryFile → Summarization  │   │    │
│  │  │  → Planning → LogPrint                            │   │    │
│  │  └───────────────────────────────────────────────────┘   │    │
│  │                                                          │    │
│  │  ┌───────────────────────────────────────────────────┐   │    │
│  │  │                    Tools                           │   │    │
│  │  │  Filesystem | Command | Web | RAG | CodeGraph     │   │    │
│  │  │  Planning | SubAgent | ShareStore | MCP | ...     │   │    │
│  │  └───────────────────────────────────────────────────┘   │    │
│  │                                                          │    │
│  │  ┌───────────────────────────────────────────────────┐   │    │
│  │  │                  Providers                         │   │    │
│  │  │  OpenAIProvider | AnthropicProvider                │   │    │
│  │  │  ModelProviderRegistry (运行时模型切换)             │   │    │
│  │  └───────────────────────────────────────────────────┘   │    │
│  └──────────────────────────────────────────────────────────┘    │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │                   EventBus (事件总线)                      │    │
│  │  EventStream<T> (单向) | RequestResponseStream<Req,Resp>  │    │
│  │  Topics: Token/ToolCall/Interrupt/Permission/Subagent/... │    │
│  └──────────────────────────────────────────────────────────┘    │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │                   Protocol Servers                        │    │
│  │  McpServer | A2aServer | StdioAcpServer                  │    │
│  └──────────────────────────────────────────────────────────┘    │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │              Dependency Injection                         │    │
│  │  deps::DependencyContainer (工厂/单例/有名称注册)         │    │
│  └──────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────┘
```

### 数据流

#### 同进程模式 (Channel 直连)

```
User Input → AgentTUI/AgentStdIO
    → AgentIOBase.sendUserInput()
    → ChannelAgentIOTransport::send() (client 端, 零序列化)
    → ChannelAgentIOTransport::recv() (server 端)
    → SessionController.onPeerMessage()
    → SessionController.run() → BaseAgent.runConversationTurnAsync()
        → GraphEngine (ReAct Loop)
            → ModelCallWrapNode → OpenAI/Anthropic Provider → LLM API
            → ToolcallWrapNode → Tools (filesystem/command/web/...)
        → Delta 事件流
    → SessionController.onDelta()
    → ChannelAgentIOTransport::send() (server 端)
    → ChannelAgentIOTransport::recv() (client 端)
    → AgentTUI/AgentStdIO.onDelta()
    → UI 渲染
```

#### 远程模式 (WebSocket)

```
User Input → AgentTUI/AgentStdIO
    → AgentIOBase.sendUserInput()
    → WsAgentIOTransport::send() (client, JSON 序列化)
    → WebSocket 网络传输
    → AgentServer.handleWs()
    → WsAgentIOTransport::recv() (server, JSON 反序列化)
    → SessionController.onPeerMessage()
    → ... (同上)
    → SessionController.onDelta()
    → WsAgentIOTransport::send() (server, JSON 序列化)
    → WebSocket 网络传输
    → WsAgentIOTransport::recv() (client, JSON 反序列化)
    → AgentTUI/AgentStdIO.onDelta()
    → UI 渲染
```

### 核心设计模式

#### 1. Graph Engine + ReAct Loop

BaseAgent 的核心执行引擎基于 NeoGraph 图引擎，构建 ReAct (Reasoning + Acting) 循环：

```
__start__ → agent_start → llm → [conditional: has_tool_calls?]
                                  ├─ yes → tools → llm (循环)
                                  └─ no  → agent_end → __end__
```

- **agent_start**: 初始化会话状态、注入 system prompt、刷新临时数据
- **llm (ModelCallWrapNode)**: 调用 LLM API，支持动态模型切换、消息修复、重试
- **tools (ToolcallWrapNode)**: 分发执行工具调用，支持自动压缩输出
- **agent_end**: 清理临时数据、保存状态
- **Node events**: 每个节点执行时发出 NodeStart/NodeEnd 事件，传递节点名称

#### 2. 栈式中间件 (WrapHandleBaseNode)

中间件以栈式顺序执行，类似 HTTP 中间件：

```
start1 → start2 → start3
            ↓         ↓
          error     baseRun
            ↓         ↓
end1  ←   end2  ←   end3
```

- 每个中间件实现 `onHandleStart` / `onHandleEnd` 钩子 (可分别挂载到 agent_start、modelcall、toolcall)
- start 阶段异常时跳过 baseRun，直接执行对应的 end
- 支持 CancelledException / NodeInterrupt 的重新抛出
- 中间件按会话 (thread_id) 维护独立 State
- CodeAgent 注册的中间件栈: Permission → Skill → MemoryFile → Summarization → Planning → LogPrint

#### 3. AgentIOBase 端点模型 + Transport 层

Client 和 Server 都继承 `AgentIOBase`，通过 `AgentIOTransportBase` 传输层通信：

```
AgentIOBase (客户端端点: AgentTUI / AgentStdIO)
    ├── onDelta()          ← 接收增量事件 (来自对端)
    ├── onSync()           ← 接收全量同步 (校准)
    ├── onTurnResult()     ← 轮次结束通知
    ├── onContextStats()   ← 上下文统计更新
    ├── getInput()         → 提供用户输入 (被对端拉取)
    ├── handleInterrupt()  → 处理 HIL 交互 (权限/中断)
    ├── registerOnBus()    → 与会话级 EventBus 绑定 (注册 interrupt/permission handler)
    ├── sendToPeer()       → 发送 WireMessage 到对端
    ├── requestCancel()    → 主动请求取消
    ├── requestSelectModel() → 切换模型
    └── runTransportLoop() ← 接收循环 (从 transport 读消息 → dispatch)

AgentIOBase (服务端端点: SessionController)
    ├── onDelta()          ← BaseAgent 产出 → 经 transport 发给客户端
    ├── onSync()           ← 同步 fullHistory
    ├── getInput()         → 从 transport 等待客户端输入
    ├── handleInterrupt()  → 发送 InterruptRequest，等待客户端响应
    └── run()              → 驱动循环: 取输入 → 执行轮次 → 推送结果

AgentIOTransportBase (传输层抽象)
    ├── connect(hello)     → 建立连接并发送握手
    ├── recv()             → 接收 WireMessage (协程阻塞)
    ├── send(msg)          → 发送 WireMessage
    ├── close()            → 关闭传输
    └── alive()            → 传输是否存活
```

#### 4. EventBus 强类型事件

```cpp
// 单向事件流
auto& stream = bus.get<EventModelToken>("agent.model.token");
stream.subscribe([](const EventModelToken& e) -> asio::awaitable<void> {
    // 处理 token
});
co_await stream.publish(EventModelToken{.token = "hello"});

// 请求-响应流 (HIL)
auto& rr = bus.getRR<ReqPermission, RespPermission>("service.permission");
rr.serve([](const ReqPermission& req, size_t corrId) -> asio::awaitable<RespPermission> {
    co_return RespPermission{.decision = RespPermission::Decision::Allow};
});
auto resp = co_await rr.request(ReqPermission{.category = "filesystem_write"});
```

事件主题 (Topic) 命名规范: `<scope>.<subject>[.<detail>]`

| Topic | 事件类型 | 方向 | 说明 |
|-------|---------|------|------|
| `agent.turn.start` | EventAgentTurnStart | 单向 | 轮次开始 |
| `agent.turn.end` | EventAgentTurnEnd | 单向 | 轮次结束 |
| `agent.model.start` | EventModelCallStart | 单向 | 模型调用开始 |
| `agent.model.token` | EventModelToken | 单向 | 模型输出 token |
| `agent.model.end` | EventModelCallEnd | 单向 | 模型调用结束 |
| `agent.tool.start` | EventToolCallStart | 单向 | 工具调用开始 |
| `agent.tool.end` | EventToolCallEnd | 单向 | 工具调用结束 |
| `subagent.progress` | EventSubagentProgress | 单向 | Subagent 进度 |
| `io.display` | EventDisplay | 单向 | 通用显示输出 |
| `io.user_input` | EventUserInput | 单向 | 用户输入 |
| `io.cancel` | EventCancel | 单向 | 取消信号 |
| `agent.error` | EventError | 单向 | 错误通知 |
| `service.interrupt` | ReqInterrupt / RespInterrupt | RR | 中断 HIL |
| `service.permission` | ReqPermission / RespPermission | RR | 权限询问 |
| `service.subagent` | ReqSubagentStart / RespSubagentResult | RR | Subagent 委派 |
| `service.subagent.batch` | ReqSubagentBatch / RespSubagentBatch | RR | 批量 subagent |
| `service.crossagent` | ReqCrossAgent / RespCrossAgent | RR | 跨 agent 查询 |

#### 5. 会话隔离与无锁设计

```
AgentContext
    ├── agentConfig          (全局共享配置)
    ├── middlewareHandleContext (中间件句柄)
    ├── bus                  (全局事件总线)
    ├── modelRegistry        (模型注册表)
    └── sessions (SessionStore)
         ├── "thread_1" → Session
         │     ├── io                    (AgentIOBase)
         │     ├── bus                   (会话级事件总线)
         │     ├── contextStats          (std::atomic 字段, 跨线程安全)
         │     ├── activity              (std::atomic<Activity>, 跨线程安全)
         │     ├── fullHistory + chainHash (io 线程写入, atomic snapshot 供 UI 只读)
         │     ├── historySnapshot_      (std::atomic<shared_ptr<const vector>>, 无锁快照)
         │     ├── hashSnapshot_         (std::atomic<shared_ptr<const HashInfo>>, 无锁快照)
         │     ├── deltaSeq              (std::atomic<uint64_t>, 原子递增)
         │     ├── cancelToken           (仅 io 线程读写)
         │     └── modelName             (仅 io 线程读写, 经 Wire 切换)
         └── "thread_2" → Session
               └── ...

线程安全策略:
  - io 线程: 写入 fullHistory/llmMessages/chainHash (assertIoThread 强制校验)
  - UI 线程: 通过 atomic snapshot 只读访问 getFullHistoryCopy() / getHashInfo()
  - 取消/切模型: 经 Wire 消息发往 agent 线程处理
  - SessionStore: 仅在 agent io_context 线程访问, 无需锁
  - AsyncMutex: 协程感知互斥锁, 用于跨越 co_await 的临界区保护
```

### 连接与重连机制

```
Client                              Server
  │                                    │
  │──── Hello (thread, token, seq,    │
  │      tailHash, model) ───────────→│
  │                                    │ 验证 token
  │                                    │ 查找/创建 SessionController
  │←── HelloAck (ok, models, hash) ───│
  │                                    │
  │──── UserInput (text) ────────────→│
  │                                    │ runConversationTurnAsync()
  │←── Delta (text_token, seq=1) ─────│
  │←── Delta (text_token, seq=2) ─────│
  │←── Delta (tool_start, seq=3) ─────│
  │←── Delta (tool_end, seq=4) ───────│
  │←── Delta (text_token, seq=5) ─────│
  │←── TurnResult ────────────────────│
  │                                    │
  │  [连接断开]                         │ 启动 grace 定时器
  │                                    │
  │──── Hello (seq=3, tailHash) ─────→│ 增量重放 seq>3 的 delta
  │←── HelloAck + Delta replay ───────│ seq 不连续时回退全量 Sync
  │                                    │
  │──── Cancel ──────────────────────→│ 取消当前轮次
  │                                    │
  │──── SelectModel (model) ─────────→│ 切换会话模型
  │                                    │
  │──── GetModel ────────────────────→│ 查询当前模型信息
  │←── ModelInfo ─────────────────────│
  │                                    │
  │──── GetAppendComponentInfo ──────→│ 查询 MCP/Skill/Memory 组件加载信息
  │←── AppendComponentInfo ───────────│
  │                                    │
  │──── GetContext ──────────────────→│ 查询当前 llmMessages
  │←── ContextMessages ───────────────│
  │                                    │
  │ (可选) 日志转发
  │←── Log (level, message) ─────────│ 服务端日志实时推送
  │                                    │
  │ (可选) 上下文统计
  │←── ContextStats ──────────────────│ token 用量推送
```

### 依赖注入容器

```
deps::DependencyContainer
    ├── registerSingleton<T>(factory)       → 注册单例 (默认无名称)
    ├── registerNamedSingleton<T>(name, fn) → 注册有名称单例
    ├── resolve<T>()                        → 解析默认实例
    ├── resolveNamed<T>(name)               → 解析有名称实例
    └── hasType<T>()                        → 检查是否存在
```

---

## 代码结构

```
agent/
├── lib/                          # libagentxx 核心库
│   ├── include/agentxx/
│   │   ├── agentxx.h             # 库总入口头文件
│   │   ├── agent/                # Agent 核心
│   │   │   ├── base_agent.h      # BaseAgent 基类 (核心基础设施 + ReAct 循环 + 会话执行)
│   │   │   ├── code_agent.h      # CodeAgent (继承 BaseAgent, 编程工具/中间件)
│   │   │   ├── agent_io.h        # AgentIOBase 端点基类 (client/server 操作契约)
│   │   │   ├── agent_io_transport.h # 传输层抽象基类 (connect/recv/send/close/alive)
│   │   │   ├── channel_io_transport.h # 进程内 Channel 传输 (零序列化)
│   │   │   ├── ws_io_transport.h  # WebSocket 传输 (JSON 编解码/心跳/重连)
│   │   │   ├── config.h          # AgentConfig / ModelConfig 配置
│   │   │   ├── config_static.h   # 静态路径配置
│   │   │   ├── context.h         # AgentContext / Session / SessionStore / ContextStats
│   │   │   │                     #   Session: 线程绑定 + 无锁快照 (atomic snapshot)
│   │   │   ├── conversation_types.h # Delta(含NodeStart/End/seq/timing) / SyncPayload
│   │   │   │                     #   HistoryMessage / ChainHash / AppendComponentNotification
│   │   │   ├── model_registry.h  # ModelProviderRegistry (运行时模型切换)
│   │   │   ├── prompt.h          # AgentPrompt / ToolPrompt 提示词管理
│   │   │   ├── training.h        # EvolutionTrainingAgent 进化训练 (变异/评估/优化/收敛检测)
│   │   │   └── remote/           # 远程通信
│   │   │       ├── agent_server.h    # AgentServer (WS 服务, token 鉴权)
│   │   │       ├── session_controller.h # SessionController (会话驱动, delta 缓冲/重放, grace)
│   │   │       └── wire_protocol.h   # Wire Protocol 消息类型与序列化
│   │   ├── deps/                 # 依赖注入
│   │   │   └── injector.h        # DependencyContainer (工厂/单例/有名称注册)
│   │   ├── nodes/                # Graph 节点
│   │   │   ├── wrap_handle.h     # WrapHandleBaseNode 栈式中间件基类
│   │   │   ├── modelcall.h       # ModelCallWrapNode (LLM 调用, 动态模型切换)
│   │   │   ├── toolcall.h        # ToolcallWrapNode (工具分发, 自动压缩)
│   │   │   └── agentcall.h       # AgentStart/EndCallWrapNode (会话生命周期)
│   │   ├── middlewares/          # 中间件
│   │   │   ├── middleware.h      # BaseMiddlewareHandle / MiddlewareContext / State 基类
│   │   │   ├── events.h          # 事件类型定义 (Topic 命名空间 / Event structs)
│   │   │   ├── event_stream.h    # EventBus / EventStream / RequestResponseStream
│   │   │   ├── permission.h      # PermissionMiddleware (工具权限 HIL)
│   │   │   ├── skill.h           # SkillMiddleware (技能发现与加载)
│   │   │   ├── memory_file.h     # MemoryFileMiddleware (上下文文件注入)
│   │   │   ├── summarization.h   # SummarizationMiddleware (上下文压缩)
│   │   │   ├── planning.h        # PlanningMiddleware (任务规划状态)
│   │   │   └── subagent_supervisor.h # SubagentSupervisor (子代理管理, 批量委派, 跨 agent 路由)
│   │   ├── tools/                # 工具
│   │   │   ├── tool.h            # XXToolBase / XXToolWrap 工具基类
│   │   │   ├── filesystem.h      # 文件系统工具 (list/read/write/edit/glob/grep)
│   │   │   ├── execute_command.h # 命令执行工具 (linux/windows/python/javascript)
│   │   │   ├── web_search.h      # 网络搜索工具 (search/fetch/fetch_markdown/model_search)
│   │   │   ├── rag_search.h      # RAG 语义搜索 (EmbeddingClient / VectorStore)
│   │   │   ├── codegraph_tool.h  # 代码图分析工具 (search/context/callers/callees/impact/index/path/status)
│   │   │   ├── planning.h        # 规划工具 (planning_write)
│   │   │   ├── sub_agent.h       # 子代理管理工具
│   │   │   ├── tool_skill_search.h # 工具/技能延迟加载搜索
│   │   │   ├── share_store.h     # 会话级文本寄存
│   │   │   ├── string.h          # 字符串工具 (html2md / regexp)
│   │   │   ├── system.h          # 系统工具 (datetime / cpu_gpu_info)
│   │   │   ├── cross_agent_query.h # 跨代理查询
│   │   │   └── ui_control.h      # UI 键鼠控制 (Windows)
│   │   ├── protocol/             # 协议实现
│   │   │   ├── openai_provider.h  # OpenAI Chat Completions API (流式/非流式/SSE)
│   │   │   ├── anthropic_provider.h # Anthropic Messages API (thinking/tool_use)
│   │   │   ├── mcp_client.h      # MCP Client (HTTP SSE + stdio, 多版本协商)
│   │   │   ├── mcp_server.h      # MCP Server (HTTP + stdio, tool/resource/prompt)
│   │   │   ├── a2a_client.h      # A2A Client (Agent Card / SendMessage / Task 管理)
│   │   │   ├── a2a_server.h      # A2A Server (JSON-RPC, 任务状态机)
│   │   │   └── acp_server.h      # ACP Server (stdio 模式)
│   │   ├── expand/               # 扩展能力
│   │   │   ├── codegraph_manager.h # 代码索引管理器
│   │   │   ├── screen_capture.h  # 屏幕截图
│   │   │   ├── audio_stream.h    # 音频流捕获
│   │   │   ├── text_selection_monitor.h # 文本选择监听
│   │   │   └── get_cpu_gpu_use.h # CPU/GPU 监控
│   │   └── util/                 # 工具类
│   │       ├── log.h             # 日志系统 (XX_LOG 宏, LogDispatcher, LogSink)
│   │       ├── string_util.h     # 字符串工具 (编码转换/路径标准化/base64/自然排序/IgnoreCaseMap 等)
│   │       ├── http_client.h     # HTTP 客户端 (基于 Boost.Beast)
│   │       ├── http_server.h     # HTTP 服务器 (路由/WS/SSE/SSL)
│   │       ├── http_header.h     # HeaderMap (忽略大小写的 HTTP 头部管理)
│   │       ├── ws_client.h       # WebSocket 客户端
│   │       ├── exception.h       # 异常处理工具
│   │       ├── lru_cache.h       # LRU 缓存
│   │       ├── diff_util.h       # 行级 diff (unified diff 格式)
│   │       ├── regex.h           # 正则引擎 (hyperscan/vectorscan)
│   │       ├── aho_corasick.h    # Aho-Corasick 多模式匹配
│   │       ├── router.h          # HTTP 路由器
│   │       ├── async_mutex.h     # 协程感知异步互斥锁 (基于 concurrent_channel)
│   │       └── util.h            # 通用工具 (系统检测等)
│   └── src/                      # 实现文件 (与 include 目录结构对应)
│
├── client/                       # agentxx_cli 可执行程序
│   ├── main.cpp                  # 入口: 参数解析 → 配置加载 → 模式分发
│   ├── include/agentxx-client/
│   │   ├── config_loader.h       # YAML 配置加载 / .env 解析 / 环境变量替换
│   │   ├── mode_runners.h        # 运行模式入口 (local/remote × tui/cli, 统一调用)
│   │   ├── io/
│   │   │   ├── stdio/
│   │   │   │   ├── agent_stdio.h # AgentStdIO (stdin/stdout 交互)
│   │   │   │   └── stdin_reader.h # 异步 stdin 读取器
│   │   │   └── tui/
│   │   │       ├── agent_tui.h   # AgentTUI (FTXUI 终端 UI, 接收/显示/排队/权限/日志)
│   │   │       ├── scrollable.h  # Scrollable (可复用的自动滚动容器组件)
│   │   │       └── tui_theme.h   # TUI 主题配色
│   │   ├── train/                # 训练模式
│   │   └── util/                 # 客户端工具
│   └── src/                      # 实现文件
│       ├── main.cpp
│       ├── config_loader.cpp
│       ├── mode_runners.cpp
│       ├── io/
│       │   ├── stdio/agent_stdio.cpp, stdin_reader.cpp
│       │   └── tui/
│       │       ├── agent_tui.cpp
│       │       ├── tui_theme.cpp
│       │       ├── tui_render_messages.cpp   # 消息列表渲染
│       │       ├── tui_render_sidebar.cpp    # 右侧边栏渲染
│       │       ├── tui_render_overlays.cpp   # 浮层渲染 (权限/中断)
│       │       ├── tui_render_edittool.cpp   # 文件编辑 diff 渲染
│       │       └── tui_log_sink.cpp          # TUI 日志接收器
│       ├── train/train.cpp        # 训练实现
│       └── util/util.cpp          # 客户端工具实现
│
├── test/                         # agentxx_test 测试程序
│   ├── test.cpp                  # 测试入口: 模块注册与调度
│   ├── test_framework.h          # 测试框架 (断言宏 / TestResult)
│   ├── test_agent.*              # CodeAgent 集成测试 (模拟 LLM Server)
│   ├── test_events.*             # 事件类型测试
│   ├── test_event_stream.*       # EventBus / EventStream / RequestResponseStream 测试
│   ├── test_event_bridge.*       # EventBridge 事件翻译测试
│   ├── test_interrupt_bus.*      # 中断总线 HIL 测试
│   ├── test_subagent_bus.*       # 子代理总线测试
│   ├── test_crossagent.*         # 跨代理通信测试
│   ├── test_concurrency.*        # 并发测试
│   ├── test_lockless.*           # Session 无锁快照测试
│   ├── test_session_concurrency.* # Session 跨线程只读快照测试
│   ├── test_remote_agent.*       # 远程 Agent (WS 传输 / SessionController) 测试
│   ├── test_mcp.*                # MCP 协议测试 (多版本/HTTP/stdio)
│   ├── test_a2a.*                # A2A 协议测试
│   ├── test_acp.*                # ACP 协议测试
│   ├── test_websocket.*          # WebSocket 测试
│   ├── test_http.*               # HTTP 客户端/服务器测试
│   ├── test_openai_provider.*    # OpenAI Provider 测试 (SSE/thinking/tool_calls/限流)
│   ├── test_anthropic_provider.* # Anthropic Provider 测试
│   ├── test_string_util.*        # 字符串工具测试
│   ├── test_regex.*              # 正则引擎测试
│   ├── test_diff_util.*          # Diff 工具测试
│   ├── test_filesystem_tools.*   # 文件系统工具测试
│   ├── test_command_tools.*      # 命令执行工具测试
│   ├── test_share_store.*        # ShareStore 测试
│   ├── test_string_tools.*       # 字符串工具测试
│   ├── test_rag_search_tools.*   # RAG 搜索测试
│   ├── test_web_search_tools.*   # 网络搜索测试
│   ├── test_codegraph_tools.*    # 代码图工具测试
│   ├── test_datetime_tool.*      # 日期时间工具测试
│   ├── test_cpu_gpu_use.*        # CPU/GPU 监控测试
│   ├── test_screen_capture.*     # 屏幕截图测试
│   ├── test_text_selection_monitor.* # 文本选择监听测试
│   └── test_misc_fixes.*         # 杂项修复测试
│
├── benchmark/                    # 性能测试 (一般仅 release 编译)
│
├── third_party/                  # 第三方依赖
│   ├── boost/                    # asio / beast / process / exception
│   ├── codegraph-cpp/            # 代码图分析
│   ├── curl/                     # HTTP
│   ├── fmt/                      # 格式化
│   ├── FTXUI/                    # 终端 UI
│   ├── glob/                     # 文件 glob
│   ├── html2md/                  # HTML 转 Markdown
│   ├── hyperscan/                # 正则引擎
│   ├── iconv/                    # 字符编码转换
│   ├── liburing/                 # io_uring
│   ├── NeoGraph/                 # 图引擎 (LLM 调用/工具分发)
│   ├── OpenSSL/                  # TLS/SSL
│   ├── simdjson/                 # JSON 解析
│   ├── sqlite3/                  # 数据库
│   ├── uchardet/                 # 编码检测
│   ├── vectorscan/               # 正则引擎 (hyperscan 替代)
│   ├── yaml-cpp/                 # YAML 解析
│   └── zlib/                     # 压缩
│
└── script/                       # 编译/测试脚本
    ├── linux_debug_build.sh
    ├── linux_release_build.sh
    ├── linux_test_run.sh
    ├── windows_debug_build.bat
    ├── windows_release_build.bat
    ├── windows_test_run.bat
    └── android_release_build.sh
```

### 关键依赖关系

```
BaseAgent (基类)
  ├── GraphEngine (NeoGraph) + per-agent GraphRegistry
  │     ├── ModelCallWrapNode → OpenAIProvider / AnthropicProvider
  │     ├── ToolcallWrapNode → XXToolBase 工具集
  │     └── AgentStart/EndCallWrapNode
  ├── MiddlewareContext → 中间件栈
  ├── AgentContext
  │     ├── SessionStore → Session (per thread_id)
  │     │     ├── fullHistory + chainHash (io 线程写入, atomic snapshot 供 UI 只读)
  │     │     ├── llmMessages (io 线程读写)
  │     │     ├── cancelToken / modelName (io 线程读写)
  │     │     ├── activity / deltaSeq / contextStats (atomic, 跨线程安全)
  │     │     └── io / bus (会话级)
  │     ├── ModelProviderRegistry
  │     └── EventBus
  └── AgentConfig → ModelConfig / AgentPrompt

CodeAgent (继承 BaseAgent)
  ├── 工具: Filesystem | Command | Web | RAG | SubAgent | MCP | CodeGraph | ...
  └── 中间件: Permission → Skill → MemoryFile → Summarization → Planning → LogPrint

SessionController (远程会话驱动)
  ├── AgentIOBase (服务端端点)
  ├── BaseAgent.runConversationTurnAsync()
  ├── deltaBuf (断线缓冲) + grace timer
  └── AgentIOTransportBase (从 AgentServer 传入)

Client (agentxx_cli)
  ├── AgentTUI / AgentStdIO → AgentIOBase
  ├── ChannelAgentIOTransport / WsAgentIOTransport → AgentIOTransportBase
  ├── ConfigLoader → YAML + .env
  └── ModeRunners → local/remote × tui/cli 组合
     ├── runLocalTuiUnified / runLocalCliUnified
     └── runRemoteTui / runRemoteCli

EventBus (事件总线)
  ├── EventStream<T> (单向: publish/subscribe/unsubscribe)
  ├── RequestResponseStream<Req, Resp> (双向: request/serve)
  └── 主题表 (Topic 命名空间常量)
