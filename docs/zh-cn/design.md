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
- **流式输出**: LLM 响应以增量 Delta 事件推送 (TextToken / ThinkingToken / ToolStart / ToolEnd / TurnStart / TurnEnd)
- **多模型支持**: 运行时按会话 (thread_id) 动态切换模型，支持 OpenAI 和 Anthropic 两种 Provider 协议
- **上下文压缩**: SummarizationMiddleware 在上下文接近模型 token 上限时自动压缩历史消息，支持 toolcall 输出去重与截断
- **思维链展示**: 支持 LLM 的 thinking/reasoning_content 流式输出与展示

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
| **SummarizationMiddleware** | 上下文 token 统计与自动压缩，防止超出模型上下文窗口 |
| **PlanningMiddleware** | 任务规划状态管理，将 planning 数据注入 system prompt |
| **SubagentSupervisor** | 子代理生命周期管理与结果收集 |
| **EventBridge** | 将 GraphEngine 事件翻译为 EventBus 强类型事件 |

### 事件系统

- **EventBus**: 强类型事件总线，支持单向事件流 (`EventStream<T>`) 和请求-响应流 (`RequestResponseStream<Req, Resp>`)
- **事件主题**: AgentTurnStart/End、ModelCallStart/End、ModelToken、ToolCallStart/End、SubagentProgress、Display、UserInput、Cancel、Error、Interrupt、Permission、Subagent
- **HIL (Human-in-the-Loop)**: 中断/权限请求经 RequestResponseStream 派发到客户端 UI，支持超时
- **定时器**: EventBus 内置定时器事件流，支持 once/repeat 模式

### 多会话与并发

- **Session 隔离**: 每个 thread_id 独立的 Session，包含 IO、EventBus、ContextStats、CancelToken、模型选择、消息历史
- **SessionStore**: 线程安全的会话存储，按 thread_id 取/建 Session
- **活动状态**: Idle / Streaming / ExecutingTool / WaitingInput 四种状态
- **链式哈希**: fullHistory 使用 FNV-1a 链式哈希校验一致性

### 远程通信

- **WebSocket 服务**: AgentServer 提供 WS/WSS 服务，支持 token 鉴权
- **Wire Protocol**: 双向 JSON 消息协议 (Hello/UserInput/Cancel/SelectModel/Delta/Sync/InterruptRequest/InterruptResponse/TurnResult/ContextStats)
- **断线重连**: 客户端自动重连，携带 lastSeq 供增量 Delta 重放，seq 不连续时回退全量 Sync
- **Grace Period**: 断线后会话保持运行的宽限期，避免误取消进行中的轮次
- **进程内直连**: ChannelAgentIOTransport 零序列化 Channel 传输，同进程内 client 与 agent 直连

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
  - Thinking/Tool 消息自动折叠/展开
  - 流式 token 实时渲染
  - 权限请求弹窗
  - 模型选择器 (运行时切换)
  - 右侧边栏 (日志窗口 / 信息面板 / Planning 展示)
  - 待发送消息队列 (执行中排队，轮次结束自动派发)
  - 文件编辑 diff 对比渲染
  - 上下文 token 占用状态栏
  - 主题切换
- **CLI 模式**: 基于 stdin/stdout 的简洁命令行交互

### 训练系统

- **进化训练**: EvolutionTrainingAgent 实现提示词自动优化
  - 变异策略: 字符级随机变异 + LLM 生成变异
  - 评估: 运行测试用例集，支持精确匹配和 LLM 评分
  - 优化: 基于反馈的 LLM 提示词补丁生成
  - 收敛检测与去重

### 扩展能力

| 模块 | 说明 |
|------|------|
| **ScreenCapture** | 屏幕截图与流式捕获 (多屏支持) |
| **AudioStream** | 系统音频/麦克风/程序音频流捕获 |
| **TextSelectionMonitor** | 系统级文本选择事件监听 (Windows UI Automation) |
| **CpuGpuMonitor** | CPU/内存/GPU 使用率查询 |
| **CodeGraphManager** | 代码索引与符号分析 (基于 codegraph-cpp) |

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
path/to/agentxx_test string_util regex deepagent
```

可用测试模块: `string_util` `regex` `diff_util` `events` `concurrency` `misc_fixes` `event_stream` `event_bridge` `interrupt_bus` `subagent_bus` `crossagent` `string_tools` `share_store` `rag_search` `datetime` `filesystem` `command` `web_search` `codegraph` `cpu_gpu` `http` `websocket` `remote_agent` `mcp` `acp` `a2a` `openai_provider` `anthropic_provider` `deepagent` `screen_capture` `text_selection`

### 配置文件

配置文件为 YAML 格式 (默认 `agentxx-config.yaml`)，支持 `${VAR}` 环境变量替换：

```yaml
models:
  - name: "my-model"
    type: "openai"              # 或 "anthropic"
    base_url: "https://api.example.com"
    api_key: "${MY_API_KEY}"    # 从 .env 或系统环境变量解析
    model_name: "gpt-4"
    send_thinking: false
    connect_timeout: 16
    read_timeout: 24
    model_support_max_token: 128000
    extra_api_config:           # 合并到请求 body 的扩展配置
      temperature: 0.7

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
| `--agent <url>` | 远程 agent server 地址 (ws://host:port/deepagent) |
| `--token <token>` | 认证 token |
| `--model <model>` | 远程模型名称 |
| `--host <host>` | 服务监听地址 (默认: 127.0.0.1) |
| `--port <port>` | 服务监听端口 (默认: 17000) |
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
agentxx_cli tui --agent ws://192.168.1.100:17000/deepagent --token xxx

# 连接远程 agent (CLI)
agentxx_cli cli --agent ws://192.168.1.100:17000/deepagent?token=xxx

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
│                      │ (onDelta/onSync/getInput/handleInterrupt)│
├──────────────────────┼──────────────────────────────────────────┤
│               Transport 层                                      │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  ChannelAgentIOTransport (进程内, 零序列化)              │    │
│  │  WsAgentIOTransport (跨进程/设备, JSON over WebSocket)   │    │
│  └─────────────────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────────────────┤
│                        Agent 层                                 │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  AgentServer (WS 服务) / SessionController (会话驱动)    │    │
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
│  │  │  Permission → Skill → Summarization → Planning    │   │    │
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
└──────────────────────────────────────────────────────────────────┘
```

### 数据流

#### 同进程模式 (Channel 直连)

```
User Input → AgentTUI/AgentStdIO
    → ChannelAgentIOTransport (client 端)
    → ChannelAgentIOTransport (server 端)
    → SessionController.onPeerMessage()
    → SessionController.run() → BaseAgent.runConversationTurnAsync()
        → GraphEngine (ReAct Loop)
            → ModelCallWrapNode → OpenAI/Anthropic Provider → LLM API
            → ToolcallWrapNode → Tools (filesystem/command/web/...)
        → Delta 事件流
    → SessionController.onDelta()
    → ChannelAgentIOTransport → AgentTUI/AgentStdIO.onDelta()
    → UI 渲染
```

#### 远程模式 (WebSocket)

```
User Input → AgentTUI/AgentStdIO
    → WsAgentIOTransport (client, JSON 序列化)
    → WebSocket 网络传输
    → AgentServer.handleWs()
    → WsAgentIOTransport (server, JSON 反序列化)
    → SessionController.onPeerMessage()
    → ... (同上)
    → SessionController.onDelta()
    → WsAgentIOTransport (server, JSON 序列化)
    → WebSocket 网络传输
    → WsAgentIOTransport (client, JSON 反序列化)
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

#### 2. 栈式中间件 (WrapHandleBaseNode)

中间件以栈式顺序执行，类似 HTTP 中间件：

```
start1 → start2 → start3
            ↓         ↓
          error     baseRun
            ↓         ↓
end1  ←   end2  ←   end3
```

- 每个中间件实现 `onHandleStart` / `onHandleEnd` 钩子
- start 阶段异常时跳过 baseRun，直接执行对应的 end
- 支持 CancelledException / NodeInterrupt 的重新抛出
- 中间件按会话 (thread_id) 维护独立 State

#### 3. AgentIOBase 端点模型

Client 和 Server 都继承 `AgentIOBase`，通过 Transport 组合关系通信：

```
AgentIOBase (客户端端点)
    ├── onDelta()          ← 接收增量事件
    ├── onSync()           ← 接收全量同步
    ├── getInput()         ← 提供用户输入
    ├── handleInterrupt()  ← 处理 HIL 交互
    ├── sendToPeer()       → 发送命令到对端
    └── runTransportLoop() ← 接收循环

AgentIOBase (服务端端点: SessionController)
    ├── onDelta()          ← BaseAgent 产出，经 transport 发给客户端
    ├── getInput()         ← 从 transport 等待客户端输入
    ├── handleInterrupt()  ← 发送 InterruptRequest，等待客户端响应
    └── run()              ← 驱动循环: 取输入 → 执行轮次 → 推送结果
```

#### 4. EventBus 强类型事件

```cpp
// 单向事件流
auto& stream = bus.get<EventModelToken>("model.token");
stream.subscribe([](const EventModelToken& e) -> asio::awaitable<void> {
    // 处理 token
});
co_await stream.publish(EventModelToken{.token = "hello"});

// 请求-响应流 (HIL)
auto& rr = bus.getRR<ReqPermission, RespPermission>("permission");
rr.serve([](const ReqPermission& req, size_t corrId) -> asio::awaitable<RespPermission> {
    co_return RespPermission{.decision = RespPermission::Decision::Allow};
});
auto resp = co_await rr.request(ReqPermission{.category = "filesystem_write"});
```

#### 5. 会话隔离

```
AgentContext
    ├── agentConfig          (全局共享配置)
    ├── middlewareHandleContext (中间件句柄)
    ├── bus                  (全局事件总线)
    ├── modelRegistry        (模型注册表)
    └── sessions (SessionStore)
         ├── "thread_1" → Session
         │     ├── io
         │     ├── bus (会话级事件总线)
         │     ├── contextStats
         │     ├── activity
         │     ├── fullHistory + chainHash
         │     ├── llmMessages
         │     ├── cancelToken
         │     └── modelName
         └── "thread_2" → Session
               └── ...
```

### 连接与重连机制

```
Client                              Server
  │                                    │
  │──── Hello (thread, token, seq) ───→│
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
  │──── Hello (seq=3) ───────────────→│ 增量重放 seq>3 的 delta
  │←── HelloAck + Delta replay ───────│
  │                                    │
  │──── Cancel ──────────────────────→│ 取消当前轮次
  │                                    │
  │──── SelectModel (model) ─────────→│ 切换会话模型
```

---

## 代码结构

```
agent/
├── lib/                          # libagentxx 核心库
│   ├── include/agentxx/
│   │   ├── agentxx.h             # 库总入口头文件
│   │   ├── agent/                # Agent 核心
│  │   │   ├── base_agent.h     # BaseAgent 基类 (核心基础设施 + ReAct 循环 + 会话执行)
│  │   │   ├── code_agent.h     # CodeAgent (继承 BaseAgent, 编程工具/中间件)
│   │   │   ├── agent_io.h        # AgentIOBase 端点基类 (client/server 操作契约)
│   │   │   ├── agent_io_transport.h # 传输层抽象基类
│   │   │   ├── channel_io_transport.h # 进程内 Channel 传输 (零序列化)
│   │   │   ├── ws_io_transport.h # WebSocket 传输 (JSON 编解码/心跳/重连)
│   │   │   ├── config.h          # AgentConfig / ModelConfig 配置
│   │   │   ├── config_static.h   # 静态路径配置
│   │   │   ├── context.h         # AgentContext / Session / SessionStore / ContextStats
│   │   │   ├── conversation_types.h # Delta / SyncPayload / HistoryMessage / ChainHash
│   │   │   ├── model_registry.h  # ModelProviderRegistry (运行时模型切换)
│   │   │   ├── prompt.h          # AgentPrompt / ToolPrompt 提示词管理
│   │   │   ├── training.h        # EvolutionTrainingAgent 进化训练
│   │   │   └── remote/           # 远程通信
│   │   │       ├── agent_server.h    # AgentServer (WS 服务, token 鉴权)
│   │   │       ├── session_controller.h # SessionController (会话驱动, delta 缓冲, 重连重放)
│   │   │       └── wire_protocol.h   # Wire Protocol 消息类型与序列化
│   │   ├── nodes/                # Graph 节点
│   │   │   ├── wrap_handle.h     # WrapHandleBaseNode 栈式中间件基类
│   │   │   ├── modelcall.h       # ModelCallWrapNode (LLM 调用, 动态模型切换)
│   │   │   ├── toolcall.h        # ToolcallWrapNode (工具分发, 自动压缩)
│   │   │   └── agentcall.h       # AgentStart/EndCallWrapNode (会话生命周期)
│   │   ├── middlewares/          # 中间件
│   │   │   ├── middleware.h      # BaseMiddlewareHandle / MiddlewareContext 基类
│   │   │   ├── events.h          # 事件类型定义 (Topic / Event structs)
│   │   │   ├── event_stream.h    # EventBus / EventStream / RequestResponseStream
│   │   │   ├── permission.h      # PermissionMiddleware (工具权限 HIL)
│   │   │   ├── skill.h           # SkillMiddleware (技能发现与加载)
│   │   │   ├── summarization.h   # SummarizationMiddleware (上下文压缩)
│   │   │   ├── planning.h        # PlanningMiddleware (任务规划状态)
│   │   │   └── subagent_supervisor.h # SubagentSupervisor (子代理管理)
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
│   │       ├── string_util.h     # 字符串工具 (编码转换/路径标准化/base64/自然排序等)
│   │       ├── http_client.h     # HTTP 客户端 (基于 Boost.Beast)
│   │       ├── http_server.h     # HTTP 服务器 (路由/WS/SSE/SSL)
│   │       ├── ws_client.h       # WebSocket 客户端
│   │       ├── exception.h       # 异常处理工具
│   │       ├── lru_cache.h       # LRU 缓存
│   │       ├── diff_util.h       # 行级 diff (unified diff 格式)
│   │       ├── regex.h           # 正则引擎 (hyperscan/vectorscan)
│   │       ├── aho_corasick.h    # Aho-Corasick 多模式匹配
│   │       ├── router.h          # HTTP 路由器
│   │       ├── async_mutex.h     # 协程感知异步互斥锁
│   │       └── util.h            # 通用工具 (系统检测等)
│   └── src/                      # 实现文件 (与 include 目录结构对应)
│
├── client/                       # agentxx_cli 可执行程序
│   ├── main.cpp                  # 入口: 参数解析 → 配置加载 → 模式分发
│   ├── include/agentxx-client/
│   │   ├── config_loader.h       # YAML 配置加载 / .env 解析 / 环境变量替换
│   │   ├── mode_runners.h        # 运行模式入口 (local/remote × tui/cli)
│   │   ├── io/
│   │   │   ├── stdio/
│   │   │   │   ├── agent_stdio.h # AgentStdIO (stdin/stdout 交互)
│   │   │   │   └── stdin_reader.h # 异步 stdin 读取器
│   │   │   └── tui/
│   │   │       ├── agent_tui.h   # AgentTUI (FTXUI 终端 UI)
│   │   │       └── tui_theme.h   # TUI 主题配色
│   │   ├── train/                # 训练模式
│   │   └── util/                 # 客户端工具
│   └── src/                      # 实现文件
│
├── test/                         # agentxx_test 测试程序
│   ├── test.cpp                  # 测试入口: 模块注册与调度
│   ├── test_framework.h          # 测试框架 (断言宏 / TestResult)
│   ├── test_deepagent.*          # CodeAgent 集成测试 (模拟 LLM Server)
│   ├── test_events.*             # 事件类型测试
│   ├── test_event_stream.*       # EventBus / EventStream / RequestResponseStream 测试
│   ├── test_event_bridge.*       # EventBridge 事件翻译测试
│   ├── test_interrupt_bus.*      # 中断总线 HIL 测试
│   ├── test_subagent_bus.*       # 子代理总线测试
│   ├── test_crossagent.*         # 跨代理通信测试
│   ├── test_concurrency.*        # 并发测试
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
  │     ├── ModelProviderRegistry
  │     └── EventBus
  └── AgentConfig → ModelConfig / AgentPrompt

CodeAgent (继承 BaseAgent)
  ├── 工具: Filesystem | Command | Web | RAG | SubAgent | MCP | ...
  └── 中间件: Permission | Skill | MemoryFile | Summarization | Planning | LogPrint

Client (agentxx_cli)
  ├── AgentTUI / AgentStdIO → AgentIOBase
  ├── ChannelAgentIOTransport / WsAgentIOTransport → AgentIOTransportBase
  ├── ConfigLoader → YAML + .env
  └── ModeRunners → local/remote × tui/cli 组合
```
