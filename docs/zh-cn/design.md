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

- **多轮对话**: 支持完整的多轮对话管理，维护 `viewMessages` (append-only 完整历史) 和 `llmMessages` (可压缩的 LLM 上下文) 双消息集
- **流式输出**: LLM 响应以增量 Delta 事件推送 (TextToken / ThinkingToken / ToolStart / ToolEnd / TurnStart / TurnEnd / NodeStart / NodeEnd / MessageTip / SystemMessage)，每个 Delta 携带单调递增 seq 用于重放与同步; 轮次统计/错误/取消提示/中断头消息由 agent 线程构造为 SystemMessage 插入会话历史并推送 (携带 msgId), 保证 viewMessages 与 UI 展示一致
- **多模型支持**: 运行时按会话 (thread_id) 动态切换模型，支持 OpenAI Chat Completions、Anthropic Messages、OpenAI Responses (Codex) 三种 Provider 协议
- **上下文压缩**: SummarizationMiddleware 在上下文接近模型 token 上限时自动压缩历史消息，支持 toolcall 输出去重与截断
- **思维链展示**: 支持 LLM 的 thinking/reasoning_content 流式输出与展示
- **节点级事件**: NodeStart/NodeEnd 事件标记 Graph 节点执行生命周期，便于 UI 展示进度

### 工具调用 (ToolCall)

内置丰富的工具集，按功能分类：

| 分类 | 工具 | 说明 |
|------|------|------|
| **文件系统** | `agentxx_filesystem_list` | 列出文件/文件夹信息 (大小、类型、修改时间)，支持递归 |
| | `agentxx_filesystem_read` | 按行读取文本文件，支持 offset/limit |
| | `agentxx_filesystem_write` | 创建/覆盖文本文件 |
| | `agentxx_filesystem_edit` | 精确字符串替换编辑文本文件 |
| | `agentxx_filesystem_glob` | 按 glob 模式搜索文件 |
| | `agentxx_filesystem_grep` | 按正则/文本搜索文件内容 |
| **命令执行** | `agentxx_execute_bash_command` | 执行 Linux shell 命令，支持超时控制 (Linux/macOS) |
| | `agentxx_execute_windows_command` | 执行 Windows 命令，默认 PowerShell (自动探测 pwsh/powershell 并注入版本号到提示词)，未找到时回退 cmd.exe (Windows / WSL 下调用) |
| **网络** | `agentxx_web_search` | 网络搜索 (DuckDuckGo / 模型搜索) |
| | `agentxx_web_fetch_url` | HTTP GET 获取网页原文 |
| | `agentxx_web_fetch_url_markdown` | 获取网页并转为 Markdown |
| **知识检索** | `agentxx_rag_search` | 基于向量相似度的知识库语义搜索 |
| **代码分析** | `agentxx_codegraph_search` | 按名称搜索代码符号 |
| | `agentxx_codegraph_context` | 获取符号的定义、调用者、被调用者 |
| | `agentxx_codegraph_callers` / `agentxx_codegraph_callees` | 调用图正向/反向追踪 |
| | `agentxx_codegraph_impact` | 修改影响分析 |
| | `agentxx_codegraph_index` | 索引目录构建符号数据库 |
| | `agentxx_codegraph_path` | 查找两符号间的调用链路径 |
| | `agentxx_codegraph_status` | 索引统计信息 |
| | | `agentxx_codegraph_*` 系列 tool 仅在配置 `codegraph.enable: true` 且编译启用 `AGENTXX_ENABLE_CODEGRAPH` 时注册 |
| **规划** | `agentxx_planning_write` | 两层任务规划 (Mermaid 状态图 + Todo List + 备忘录) |
| **子代理** | `agentxx_subagent_switch` | 创建和管理子代理执行委派任务 |
| | `tool_skill_search` | 延迟加载工具/技能的搜索与发现 |
| **数据** | `agentxx_share_store` | 会话级文本寄存，节省上下文 |
| | `agentxx_string_html_to_markdown` | HTML 转 Markdown |
| | `agentxx_string_regexp` | 正则搜索/替换/移除 |
| **系统** | `agentxx_get_current_datetime` | 获取当前日期时间 |
| | `agentxx_get_system_core_info` | 获取 CPU/内存/GPU 使用率 |
| **UI 控制** | `agentxx_ui_control_keyboard_mouse` | Windows 键鼠控制 (仅 Windows) |

工具特性：
- **自动压缩**: 工具输出超过阈值时自动调用 LLM 压缩摘要
- **延迟加载**: 工具初始仅注册名称，经 `tool_skill_search` 检索后才加载全量定义
- **去重机制**: 文件读写等工具支持 SummarizationToolHandle，重复调用时截断旧结果
- **MCP 扩展**: 通过 MCP Client 连接外部 MCP Server，动态注册远程工具 (支持 HTTP SSE 和 stdio 传输)

### 中间件系统

采用栈式中间件架构，在 Graph 节点执行前后插入处理逻辑：

| 中间件 | 功能 |
|--------|------|
| **PermissionMiddleware** | 工具调用权限控制，经事件总线向用户请求授权 (HIL)。文件系统权限按最长前缀匹配文件夹规则，支持 `*` 通配符：`/data/projects` 的规则对其下任意子路径生效，且父链规则可回退 (见 `XXRouter::get` 的 `prefix_fallback`)。默认规则由 yaml `permission.mode` 决定 (Ask=工作目录内 ALLOW + 其余 INTERRUPT / AllAsk=全部 INTERRUPT / Pass=全部 ALLOW / Deny=全部 DENY)，白名单 (whitelist) 始终放行、黑名单 (blacklist) 始终拒绝 (同路径黑名单优先)，未命中任何规则时由 `noRuleOperator` 兜底。客户端可"记住本次选择" (WireSetPermission 将路径规则注册回服务端，后续直接放行/拒绝不再询问) |
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
- **单检查点存储**: engine 使用 `InMemorySingleCheckpointStore` (SingleCheckpointStore 策略基类,
  模板方法 save = saveImpl 持久化最新 + evictImpl 淘汰历史)。agentxx 只依赖
  load_latest 的最新 checkpoint 与挂载其上的 pending writes (中断/resume 恢复),
  不使用 fork / 时间旅行 (get_state_history), 因此每 thread 仅保留最新 checkpoint,
  存储开销从 O(super-steps) 降为 O(threads), 轮末无需手动裁剪
- **活动状态**: Idle / Streaming / ExecutingTool / WaitingInput 四种状态
  (注: 目前仅 ExecutingTool/Idle 被 LogPrint 中间件实际写入, Streaming/WaitingInput
  为预留; UI 活动感知实际经 Delta 事件流完成)
- **链式哈希**: viewMessages 使用 FNV-1a 链式哈希校验一致性
- **线程绑定 (单线程读写)**: Session 通过 `bindIoThread()` 绑定 io 线程，`assertIoThread()` 强制校验可变状态 (viewMessages/llmMessages/chainHash) 仅在 io 线程读写；client/UI 不直接读取，需要时由 io 线程拷贝后经 Wire 消息 (Sync/Delta) 传输，因此无需快照/锁同步
- **取消/切模型**: UI 线程的取消/切模型操作通过 Wire 消息 (WireCancel/WireSelectModel) 发往 agent 线程处理，避免跨线程竞争
- **异步互斥锁**: `AsyncMutex` 基于 asio concurrent_channel 实现协程感知互斥，不会阻塞线程，适用于协程跨越 co_await 临界区

#### 会话 SQLite 持久化 (消息上下文 / viewMessages / share_store)

- 开关: `AgentConfig::enableSessionPersistence` (默认关闭; agentxx_cli 在 `buildDefaultConfig` 中开启)。
  开启后由 BaseAgent 创建 `SessionPersistence` 并注入 AgentContext:
  - `AgentContext::sessionPersistence` + `SessionStore::persistence` (会话消息状态)
  - `MiddlewareContext` 构造参数 (share store 写穿)
- 数据目录: `{dataDir}/sqlite/sessions/{threadId}/` (threadId 经 `sanitizeThreadId` 清洗为安全目录名:
  非法字符替换/超长截断/Windows 保留名规避, 发生改写时附加 FNV hash 尾缀防碰撞;
  dataDir 由 yaml `data_dir` 指定, **未配置 dataDir 时不持久化**:
  设置/会话/codegraph 数据仅存内存, BaseAgent 初始化时输出警告;
  sessionPersistenceRoot 显式指定时即使无 dataDir 仍落盘)
- 分库设计 (两个 DB 文件, 均启用 WAL + busy_timeout):
  - `session.db`: viewMessages (append-only, 每消息一行 JSON) + llmMessages
    (单行整体替换, 每轮结束保存) + meta (msgIdCounter)
    —— 同属"会话消息状态", 同一生命周期 (随 thread 创建/删除), 同一 io 线程写入,
    一轮对话结束时消息与计数可事务性一起提交
  - `share_store.db`: agentxx_share_store KV 条目 (id 自增 = 现有最大 id + 1,
    重启后延续) —— KV 随机读改写与消息追加模式不同, 本质是上下文卸载缓存,
    内容可丢弃/可清理, 生命周期独立于消息历史; 可能存放大型文本, 独立文件
    避免其膨胀拖慢消息库 WAL checkpoint, 也便于未来独立裁剪/归档
- 接入点:
  - `SessionStore::getOrCreate`: 创建 Session 时从 SQLite 恢复 viewMessages/llmMessages,
    重建链式哈希 (对不含 id 的消息内容, 与 appendHistory 语义一致),
    恢复 msgIdCounter 保证新消息 id 不冲突; 并绑定 `SessionPersistenceHooks`
    (std::function 回调, context.h 不依赖 sqlite 头)
  - `Session::appendHistory`: 追加后回调落库 (消息 + 计数事务提交)
  - `BaseAgent::runConversationTurnAsync`: 轮末回调保存 llmMessages (整表替换)
  - `MiddlewareContext` share store 四方法: 内存 map 作读缓存 (首次访问某 thread
    时从 DB 恢复全部条目与 id 计数器), 写操作同步写穿 DB
- 容错: 所有落库失败仅记录错误日志, 不影响内存状态与对话主流程 (尽力而为持久化);
  读取路径在目录不存在时直接返回空, 不创建目录/空文件 (避免 subagent 等
  只读访问产生垃圾目录)
- 线程安全: `SessionPersistence` 内部互斥锁保护; 常规使用下调用发生在 agent io
  线程 (Session 绑定线程/工具执行), 锁仅在多线程并发访问时生效

#### 会话切换 (TUI 会话选择弹窗)

```
TUI [F4] 打开会话选择弹窗 → WireListSessions (服务端阻塞 I/O 卸载到 blockingPool)
  → 服务端回 WireSessionList (持久化会话列表, 按最近活动时间降序)
  → 用户确认 → WireSwitchSession(newThreadId)
  → SessionServerAgentIO::switchSession:
      重绑定 config_.threadId → 清空 delta 重放缓冲 (新会话 seq 独立编号)
      → 重置 firstTurn_ (首条输入走 resume_if_exists=true 恢复路径)
      → 回推新会话全量 Sync + WireModelInfo + WireContextStats
  → 客户端 (TUI) 更新本地 threadId 绑定; WS 模式同时
    transport->updateReconnectThreadId() (复位重连握手的 threadId/lastSeq/tailHash)
```

- 仅当无进行中轮次时切换 (客户端前置拦截 + 服务端双重保护)
- 新会话历史由 SessionStore 从持久化恢复 (不存在时创建空会话)
- 会话列表数据源: `{dataDir}/sqlite/sessions/` 目录扫描 + meta 表 (threadId/title/lastActiveMs)

#### Subagent 执行链路 (NodeInterrupt → 总线派发 → 独立引擎运行)

```
父 agent LLM 发起 agentxx_subagent_switch
  → SubAgentManagerTool::execute_async
      → MiddlewareContext::requestInterrupt: 首次存储中断参数到 graphData,
        抛出 NodeInterrupt → engine checkpoint 暂停父图
  → BaseAgent::runConversationTurnAsync 中断处理循环:
      → 逐个解析 graphData 中的 interrupt args
      → "subagent" 参数: 经全局总线 service.subagent 委派
      → "subagent_batch": 经 service.subagent.batch 批量并发委派
  → SubagentSupervisor::runSubagent:
      → 从 subAgentList 取 SubAgentNormalTask (独立编译的 subgraph,
        复用父 agent 的 tools 指针列表与 graphRegistry)
      → 以独立 thread_id (session_subagent_{name}) 运行 subgraph
        (resume_if_exists=false, 每次全新运行; 继承父会话 io 供权限/中断交互)
      → 结果经 interruptResult channel 写回 graphData
  → engine->resume_async 恢复父图, execute_async 从 interruptResult 按
    resultId 提取结果返回
```

- 父图与 subagent 图是两套 GraphEngine 实例, 消息上下文完全隔离
- 中断结果按 resultId (默认取 tool_call_id) 关联, 支持同轮多个 subagent
- 跨 agent 查询 (service.crossagent) 当前为显式 "not implemented"
  (被动消息注入需持久会话模式, 未实现)

### 远程通信

- **WebSocket 服务**: AgentServer 提供 WS/WSS 服务，支持 token 鉴权
- **Wire Protocol**: 双向 JSON 消息协议 (Hello/HelloAck/UserInput/Cancel/SelectModel/GetModel/Delta/Sync/InterruptRequest/InterruptResponse/InterruptExpired/TurnResult/ContextStats/Error/Log/ModelInfo/GetAppendComponentInfo/AppendComponentInfo/GetContext/ContextMessages/Ping/Pong/SetPermission/ListSessions/SessionList/SwitchSession)
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
  - 消息列表 (User/Assistant/Thinking/Tool/System/Interrupt 角色)
  - Thinking/Tool 消息自动折叠/展开 (执行中展开，完成后折叠)
  - 流式 token 实时渲染 (COW 按需拷贝避免 O(n²) 累积拷贝)
  - 权限请求弹窗 + "记住本次选择" (经 WireSetPermission 将路径规则注册到服务端权限中间件)
  - 模型选择器 (运行时切换)
  - 右侧边栏 (日志窗口 / 信息面板 / Planning 展示)
  - 待发送消息队列 (执行中排队，轮次结束自动派发)
  - 文件编辑 diff 对比渲染
  - Mermaid stateDiagram-v2 状态图渲染 (消息中 ```mermaid 代码块 / Plan 弹窗显示 roadmap 状态图)
  - 上下文 token 占用状态栏
  - 主题切换 (持久化到 {dataDir}/sqlite/global.db)
  - 会话选择弹窗 (F4): 列出持久化会话 (WireListSessions), 确认后经 WireSwitchSession 切换, 服务端回推新会话全量 Sync/模型/上下文统计
  - 启动连接状态 (banner 提示): TUI 启动后消息列表 banner 按 agent-server
    连接状态显示 —— 启动中 (Connecting, 输入进入待发送队列, 连接完成后自动发送) /
    连接失败 (Failed, 显示"连接失败 + [重试]"可点击按钮重新连接) / 已连接 (正常输入);
    本地模式由 SessionServerAgentIO 驱动循环启动前回调 onServerReady 置就绪,
    远程模式由 mode_runners 连接协程驱动 (ConnState 存于 TUIRenderState::connState)
  - 启动进度逐步展示: agent-server init() 各阶段 (检测系统环境/模型注册表/中间件/
    加载 MCP server/RAG/CodeGraph 等) 经 AgentContext::startupNotifier →
    AgentIOBase::onServerProgress 上报, "启动中"banner 同步显示当前执行的操作,
    完成后显示按键提示 (banner itemKey 计入
    connState+startupProgress 使 LazyScrollable 缓存失效重建)
    远程模式由 mode_runners 连接协程驱动 (ConnState 存于 TUIRenderState::connState)
  - 屏幕上方 toast 提示
  - 自动滚动吸附底部 (Scrollable 组件)
- **TUI 渲染模块化**: 将消息列表、侧边栏、浮层、编辑工具渲染拆分到独立文件
- **LazyScrollable (Flutter ListView.builder 风格)**: 消息列表采用懒构建渲染架构 ——
  通过 itemCount/itemKey/estimateHeight/buildItem 回调描述列表，仅构建与视口相交的
  可见子项并局部布局/绘制；已构建子项按 LRU 有界缓存 (条数 + 源字节双预算)，
  窗口外旧消息缓存被淘汰，内存占用与对话长度解耦；未进入视口的子项使用估算高度，
  进入视口后实测修正。失效 key 采用消息指针 + 廉价 O(1) 特征 (内容变化必然伴随
  指针变化，见 TUISharedState::mutableMessage)，避免对全部消息文本逐帧哈希。
  布局分两阶段: 先构建/实测可见子项 (修正估算高度)，再以修正后的总高度与滚动
  偏移统一定位 —— 避免基于估算偏移定位导致当前帧与后续帧位置不一致 (流式输出
  逐 token 高度估算偏差会造成帧间 ±1 行抖动)
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
| **CodeGraphManager** | 代码索引与符号分析 (基于 codegraph-cpp)；索引范围由 yaml `codegraph` 块配置 (默认关闭)：`paths` 加载路径列表 (可多个目录，未配置时按 `load_cwd` 默认索引当前工作目录)、`ignore_paths` 忽略路径 (支持 `*` 通配符)、`use_gitignore` 默认忽略 `.gitignore` 规则与 `.gitmodules` 子模块目录；遍历按目录剪枝 (忽略目录整棵子树不进入)，文件监听增量索引应用同一套过滤；sqlite 数据库存于 `{dataDir}/sqlite/codegraph/<折叠路径>/index.db`（深层折叠 + 单段截断控制长度，路径前缀匹配复用；dataDir 由 yaml `data_dir` 指定，未配置 dataDir 时不注册 codegraph 工具，索引不落盘） |

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

可用测试模块 (与 `agent/test/test.cpp` 注册列表一致):
- 同步模块: `string_util` `regex` `diff_util` `events` `concurrency` `misc_fixes` `aho_corasick` `util_misc` `settings_db` `toolcall_args` (及 client 侧: `config_loader` `tui_settings` `tui_input` `tui_interrupt` `tui_scroll` `tui_stream` `thread_id` `mermaid_state`)
- 异步模块: `event_stream` `event_bridge` `interrupt_bus` `subagent_bus` `string_tools` `share_store` `session_persistence` `rag_search` `datetime` `filesystem` `command` `web_search` `codegraph` `cpu_gpu` `http` `network_timeout` `websocket` `remote_agent` `mcp` `acp` `a2a` `openai_provider` `anthropic_provider` `cancel` `message_supplement` `summarization` `checkpoint_store` `agent` `memgrowth`
- 平台模块: `screen_capture` `text_selection`

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
    send_thinking: false        # 是否把 thinking/reasoning_content 随上下文发送给模型
    ssl_verify: null            # true/false 显式控制 TLS 证书验证; 省略用默认策略
    connect_timeout: 16
    read_chunk_timeout: 24
    max_concurrent_connections: 5   # 该模型 API 端点的最大并发连接数 (默认 5, 0=不限制)
                                    # LLM 请求启用 HTTP keep-alive 连接池: 空闲连接复用,
                                    # 超过上限的并发请求排队等待空闲连接
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

mcp:
  - namespace: "my_mcp"
    url: "http://localhost:3000/mcp"

# 统一数据根目录 (留空/不配置 = 不持久化: 设置/会话/codegraph 仅存内存,
# 重启后无法恢复; 支持 ~ 与 ${VAR} 展开, 相对路径按工作目录解析)
# 特殊关键字 `default` (仅 tui/cli 模式): 使用当前系统数据目录
#   - Linux/macOS: ~/.agentxx/
#   - Windows: %APPDATA%/agentxx/
# 配置后数据子路径:
#   - {data_dir}/sqlite/global.db                     全局设置 (TUI 设置等)
#   - {data_dir}/sqlite/sessions/{threadId}/          会话数据 (session.db/share_store.db)
#   - {data_dir}/sqlite/codegraph/<折叠路径>/index.db CodeGraph 索引
# data_dir: ~/.agentxx

# CodeGraph 代码分析 (需编译启用 AGENTXX_ENABLE_CODEGRAPH; 默认关闭)
codegraph:
  enable: false             # true 时 CodeAgent 注册 codegraph 系列 tool
  # paths:                  # 加载(索引)路径列表 (可选; 相对路径按工作目录解析;
  #   - "/path/to/proj_a"   #   非空时按此列表索引, 可多个目录)
  #   - "relative/proj_b"
  # ignore_paths:           # 忽略路径列表 (可选; 相对路径按工作目录解析,
  #   - "/path/to/proj_a/third_party"   #   支持 * 通配符; 命中即跳过)
  #   - "**/generated/**"
  load_cwd: true            # 未配置 paths 时默认加载当前工作目录 (默认 true;
                            #   false 时仅可手动 agentxx_codegraph_index)
  use_gitignore: true       # 默认忽略 .gitignore 规则与 .gitmodules 子模块目录
                            #   (逐层读取各级目录的规则) 及 .git 元数据目录
                            # 数据库: {data_dir}/sqlite/codegraph/<折叠路径>/index.db
                            # - 前缀复用: 子目录工作自动复用最近父级索引
                            # - 长度控制: 深层路径折叠为 hash 段, 单段超长截断,
                            #   保证不超系统路径限制 (Windows MAX_PATH=260)

# 权限询问处理模式 (默认 ask, 见 PermissionMode)
# - ask:     当前工作目录内允许读写, 其他路径询问用户 (默认)
# - all_ask: 所有路径读写均询问用户
# - pass:    全部放行, 不询问
# - deny:    全部拒绝, 不询问
permission:
  mode: ask
  whitelist: []   # 始终放行路径 (最长前缀匹配, 支持 * 通配; 优先级高于模式默认规则)
  blacklist: []   # 始终拒绝路径 (与白名单同路径时黑名单优先)
```

> **Codex (Responses API) 配置示例**:
> ```yaml
> models:
>   - name: "openai-responses"
>     type: "openai-responses"                       # 使用 OpenAI Responses API (/responses)
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
│  │ TUIClientAgentIO │  │StdIOClientAgentIO│  │   Remote Client (WS)         │  │
│  │  (FTXUI) │  │ (stdio)  │  │   WsAgentIOTransport         │  │
│  └────┬─────┘  └────┬─────┘  └──────────┬───────────────────┘  │
│       │              │                   │                      │
│       └──────────────┼───────────────────┘                      │
│                      │ AgentIOBase                              │
│                      │ (sendToPeer/onPeerMessage/getInput/      │
│                      │  handleInterrupt/registerOnBus)          │
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
│  │  AgentServer (WS 服务) / SessionServerAgentIO (会话驱动)    │    │
│  │  SessionServerAgentIO: delta 缓冲/重连重放/grace period    │    │
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
User Input → TUIClientAgentIO/StdIOClientAgentIO
    → AgentIOBase.sendUserInput()
    → ChannelAgentIOTransport::send() (client 端, 零序列化)
    → ChannelAgentIOTransport::recv() (server 端)
    → SessionServerAgentIO.onPeerMessage()
    → SessionServerAgentIO.run() → BaseAgent.runConversationTurnAsync()
        → GraphEngine (ReAct Loop)
            → ModelCallWrapNode → OpenAI/Anthropic Provider → LLM API
            → ToolcallWrapNode → Tools (filesystem/command/web/...)
        → Delta 事件流
    → SessionServerAgentIO.sendToPeer() (新 delta 写入重放缓冲后转发)
    → ChannelAgentIOTransport::send() (server 端)
    → ChannelAgentIOTransport::recv() (client 端)
    → TUIClientAgentIO/StdIOClientAgentIO.onPeerMessage() → onDelta() (protected 被动回调)
    → UI 渲染
```

#### 远程模式 (WebSocket)

```
User Input → TUIClientAgentIO/StdIOClientAgentIO
    → AgentIOBase.sendUserInput()
    → WsAgentIOTransport::send() (client, JSON 序列化)
    → WebSocket 网络传输
    → AgentServer.handleWs()
    → WsAgentIOTransport::recv() (server, JSON 反序列化)
    → SessionServerAgentIO.onPeerMessage()
    → ... (同上)
    → SessionServerAgentIO.sendToPeer() (新 delta 写入重放缓冲后转发)
    → WsAgentIOTransport::send() (server, JSON 序列化)
    → WebSocket 网络传输
    → WsAgentIOTransport::recv() (client, JSON 反序列化)
    → TUIClientAgentIO/StdIOClientAgentIO.onPeerMessage() → onDelta() (protected 被动回调)
    → UI 渲染
```

> 两种模式拓扑完全一致 (client 端点 + server 端点 + transport), 仅 transport
> 实现不同。**强制 transport**: 端点间通信必须经 transport, 不存在无 transport
> 的直连模式; `runConversationTurnAsync(io=nullptr)` 的 headless 场景除外
> (无 io 即无事件输出)。

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

#### 取消设计 (CancelToken 双通道)

取消基于 `neograph::graph::CancelToken` (每轮次创建, 存于 Session)，两条传播路径：

1. **轮询埋点 (主路径)**: 在逻辑边界手动调用 `throw_if_cancelled()` / `is_cancelled()`，
   取消点可控、异常路径可预期：
   - graph 每个 super-step 之间 (engine)
   - toolcall 分发前 / 每个 tool 执行前后 (ToolcallWrapNode)
   - LLM 调用前、重试等待后 (ModelCallWrapNode)
2. **asio 信号中断 (仅限可安全中止的耗时 IO)**: `CancelToken::cancel()` 经绑定的
   executor emit `cancellation_signal`，中断在途 LLM HTTP 流、socket 读写、定时器等，
   在 co_await 点表现为 `system_error(operation_aborted)`。这是唯一允许产生
   `operation_aborted` 的场景。

边界转换规则 (exception.h `isCancelAbort` / wrap_handle.h)：

- `operation_aborted` + 令牌已取消 ⇒ 转换为 `CancelledException` 向上传播 (取消语义)；
  `catchError/catchErrorAsync` 传入 cancelToken 参数启用该转换
- `operation_aborted` + 令牌未取消/无令牌 ⇒ 按超时错误处理 ("timeout: ...")
- 所有 catch-all / `catch(std::exception&)` 站点必须先重抛
  `CancelledException` / `NodeInterrupt`，禁止吞掉取消信号 (EventStream::publish、
  WrapHandleBaseNode、catchError 等均遵循)
- tool 经 `ContextualAsyncTool` 接收 `ToolExecutionContext{cancel_token}`，
  可轮询取消或传播到其传输层；toolcall 被取消时已完成的 tool 结果暂存
  graphData (interruptToolcallCache) 后再重抛
- 线程池卸载 (`offloadCancellableAsync`): 工作线程同步执行不挂起, asio 信号无法抢占,
  等待方 co_await 也不会提前返回 —— 带 CancelToken 的重载额外启动 watcher 协程
  轮询令牌, 取消时置位 cancelFlag 打通 "会话取消 -> 工作线程轮询退出" 通知链
  (filesystem_list/glob/grep 已接入)

不可中断段 (收尾/持久化) 不依赖"异常恰好没传到"，需显式防护：
catch `CancelledException` → 完成必要收尾 → rethrow，或用
`asio::this_coro::reset_cancellation_state` 过滤取消信号。

#### 3. AgentIOBase 端点模型 + Transport 层

Client 和 Server 都继承 `AgentIOBase`，通过 `AgentIOTransportBase` 传输层通信。
两端点之间为对称消息传递: **发送经 `sendToPeer()` (唯一出站口), 接收经
`runTransportLoop()` → `onPeerMessage()` 分发到 protected 被动回调**。
接口按角色标注: [双向] / [client] / [server]：

```
AgentIOBase (公共契约)
    ├── sendToPeer() [双向]        → 发送 WireMessage 到对端 (virtual, 唯一出站口;
    │                                 须已设置 transport, 否则记错误日志并丢弃)
    ├── requestCancel() [client]   → 请求取消
    ├── requestSelectModel() [client] → 切换模型
    ├── requestAppendComponentInfo() [client] → 拉取启动信息 (MCP/Skill/Memory)
    ├── sendUserInput() [client]   → 发送用户输入
    ├── getInput() [双向]          → 提供用户输入 (server 侧被 BaseAgent 驱动循环拉取,
    │                                 client 侧被本端输入循环调用)
    ├── handleInterrupt() [双向]   → HIL 交互 (server 侧经会话总线被 BaseAgent 调用,
    │                                 client 侧收到 WireInterruptRequest 后调用)
    ├── registerOnBus() [server]   → 与会话级 EventBus 绑定 (注册 interrupt/permission handler)
    ├── setTransport()/runTransportLoop() [双向] → transport 装配与接收循环
    └── (protected)
        ├── onPeerMessage() [双向] → 收消息分发 (默认分发到下面四个回调, 子类覆写扩展)
        ├── onDelta() [client]     ← 增量事件 (仅由 onPeerMessage 分发, 外部不得直调)
        ├── onSync() [client]      ← 全量同步 (校准)
        ├── onTurnResult() [client] ← 轮次结束通知
        └── onContextStats() [client] ← 上下文统计更新

AgentIOBase (客户端端点: TUIClientAgentIO / StdIOClientAgentIO)
    ├── onDelta/onSync/onTurnResult/onContextStats (protected) ← 收对端事件 → 渲染
    ├── getInput()         → 从 stdin/FTXUI 读输入
    ├── handleInterrupt()  → 弹出交互框收集用户响应
    └── onPeerMessage()    → 覆写: 额外处理 InterruptRequest/Log/ModelInfo 等

AgentIOBase (服务端端点: SessionServerAgentIO)
    ├── sendToPeer()       → 覆写: 新产出的 Delta (seq 单调守卫) 先写入重放缓冲再转发,
    │                          重放 delta (seq <= 缓冲尾) 不重复入缓冲
    ├── onDelta/onSync     → protected 空实现 (server 不会从 client 收到, 满足纯虚契约)
    ├── getInput()         → 从 inputChannel_ 等待客户端输入
    ├── handleInterrupt()  → 发送 InterruptRequest，等待客户端响应 (超时/过期通知)
    ├── onPeerMessage()    → 覆写: 处理 Hello/UserInput/Cancel/SelectModel/InterruptResponse/
    │                          GetModel/GetAppendComponentInfo/GetContext/ListSessions/
    │                          SwitchSession/SetPermission 等
    ├── run()              → 驱动循环: 取输入 → 执行轮次 → 推送结果
    ├── stop()             → 停止驱动循环 (关闭输入 channel/取消轮次/fail pending)
    ├── onDisconnect()     → 传输断开时启动 grace 定时器 (宽限期满且无连接则取消轮次)
    └── switchSession()    → 会话切换: 重绑定 threadId, 清空 delta 缓冲, 重置 firstTurn_,
                             回推新会话全量 Sync + 模型信息 + 上下文统计

AgentIOTransportBase (传输层抽象)
    ├── connect(hello)     → 建立连接并发送握手
    ├── recv()             → 接收 WireMessage (协程阻塞)
    ├── send(msg)          → 发送 WireMessage
    ├── close()            → 关闭传输
    └── alive()            → 传输是否存活
```

事件产出路径: `BaseAgent` 进程内直调其驱动的端点 (server 端点) ——
增量事件经 `io->sendToPeer(Delta)` 推送 (server 端点缓冲并经 transport 转发
client), 上下文统计经 `io->sendToPeer(WireContextStats)`; BaseAgent 不感知
transport 细节。`runConversationTurnAsync(io=nullptr)` 为 headless 模式, 不产出事件。

##### EventBridge: GraphEvent → 会话增量 Delta + EventBus 适配层

`EventBridge` 是 BaseAgent 与 neograph 引擎之间的唯一事件翻译器
(替代旧版散落在 BaseAgent 里的 llm callback 逻辑):

```
neograph GraphEngine (run_stream_async)
    └── GraphStreamCallback (每事件一次)
          └── EventBridge::operator()(GraphEvent)
                ├── 1. 转发原始回调 origCb (若存在)
                ├── 2. 按事件类型分派:
                │     ├── LLM_TOKEN     → publishModelToken (总线, 无订阅者零开销)
                │     │                   + emitDelta(TextToken/ThinkingToken,
                │     │                   切换 chunk 类型时附带节点内计时)
                │     ├── CHANNEL_WRITE → handleChannelWrite:
                │     │                   - "message_tip" 通道 → Delta::MessageTip
                │     │                   - "messages" 通道:
                │     │                     assistant(tool_calls) → appendHistory
                │     │                       + Delta::ToolStart 流
                │     │                     tool → appendHistory (edit 工具附带
                │     │                       diff 渲染字段) + Delta::ToolEnd 流
                │     │                     assistant → appendHistory
                │     │                   - 含 LLM 输出时推送 WireContextStats
                │     ├── NODE_START/END → 节点计时 + Delta::NodeStart/NodeEnd
                │     └── ERROR          → publishError (总线, 不产 Delta,
                │                         由 WireTurnResult 统一报告)
                └── emitDelta: 分配会话级单调递增 seq (统一经
                       Session::nextDeltaSeq, EventBridge 与 SessionServerAgentIO
                       的新产出 Delta 共用入口)
                               后经 io->sendToPeer 发送; io 为空 (headless) 时丢弃
```

- 有状态: 维护最近 chunk 类型 (content/thinking 切换计时)、节点开始时间
- 生命周期: `makeCallback()` 经 `shared_from_this` 持有, 回调期间本对象存活;
  AgentContext 以 weak_ptr 持有, 总线发布前 lock 检查
- 新增 GraphEvent 处理只需扩展本类, 无需修改 BaseAgent

**tps (token/s) 生成速度统计** (EventBridge 内置双级统计):
- 流级 (窗口推送): 每次 ModelCall 流开始 (节点开始后首个 token) 计时, 每
  `tpsPushIntervalSec_` (默认 3s) 推送一次**最近窗口**内的平均速度 (窗口内
  token 增量 / 窗口时长, 而非自流开始以来的累计平均, 反映当前实际速度),
  经 WireContextStats.tps 下发; 每个流结束 (节点结束/出错) 结算一次
- 轮级 (TurnEnd 展示): 一轮内所有 ModelCall 的累计估算 token / 累计流式耗时,
  TurnEnd Delta 携带 tps 字段, 并显示在轮次统计系统提示中
- token 估算与 SummarizationMiddleware 共用 `countTokensForUtf8Str` 口径
  (ascii ≈ 4 字符/token, 非 ascii ≈ 1.1 字符/token; 无 summarization 时内置回退)

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
         │     ├── activity              (Activity)
         │     ├── viewMessages + chainHash (仅 io 线程读写, client 经 Wire 拷贝传输)
         │     ├── deltaSeq              (普通 uint64_t, 仅 io 线程递增; EventBridge 分配)
         │     ├── cancelToken           (仅 io 线程读写)
         │     └── modelName             (仅 io 线程读写, 经 Wire 切换)
         └── "thread_2" → Session
               └── ...

线程安全策略:
  - io 线程: 读写 viewMessages/llmMessages/chainHash/deltaSeq (assertIoThread 强制校验)
  - client/UI: 不直接读取, 由 io 线程拷贝后经 Wire 消息 (Sync/Delta) 传输
  - 取消/切模型: 经 Wire 消息发往 agent 线程处理
  - SessionStore: 仅在 agent io_context 线程访问, 无需锁
  - contextStats: std::atomic 字段, 跨线程可读 (Summarization 写, IO 经 Wire 推送)
  - AsyncMutex: 协程感知互斥锁, 用于跨越 co_await 的临界区保护
```

### 连接与重连机制

#### WsAgentIOTransport 内部结构 (客户端模式)

```
客户端 (WsAgentIOTransport)
  ├── establishConnection(): wsConnect + 失败重试 (退避 reconnectBackoff, 可取消)
  ├── writeLoop():    writeQueue (concurrent_channel, cap=4096) → ws send
  ├── readLoop():     ws recv → 反序列化 → recvQueue (concurrent_channel, cap=256)
  │                     → recv() 消费; 断线后进入自动重连循环:
  │                       重连 → 重建 writeQueue → 发送 Hello(lastSeq, tailHash)
  │                       → 服务端增量重放 (seq 不连续时回退全量 Sync)
  ├── heartbeatLoop(): 每 heartbeatInterval 发送 Ping
  └── Delta 去重: 收到 delta 时更新 lastDeltaSeq_, 重放重复投递的
      seq <= last 直接丢弃, 避免 UI 重复渲染
```

- 写/读队列均为有界 concurrent_channel, `try_send` 失败即丢弃 (见"已知问题"
  问题 3); 队列关闭使挂起的 async_receive 抛异常, 循环自然退出
- HelloAck 在 connect() 握手阶段被消费, 不进入 runTransportLoop 的消息流
- 服务端模式 (AgentServer 注入已建立的 WsClient): 不发送 hello, 不重连,
  握手由 AgentServer::serveTransport 完成

```
Client                              Server
  │                                    │
  │──── Hello (thread, token, seq,    │
  │      tailHash, model) ───────────→│
  │                                    │ 验证 token
  │                                    │ 查找/创建 SessionServerAgentIO
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
  │←── ContextStats ──────────────────│ token 用量推送 (含流式期间窗口平均 tps)
  │                                    │
  │ (可选) 客户端记住权限选择
  │──── SetPermission (path, allow) ──│ 注册路径规则到服务端权限中间件
  │                                    │
  │ (可选) 会话选择弹窗 (TUI F4)
  │──── ListSessions ─────────────────│ 列举持久化会话 (阻塞 I/O 卸载到线程池)
  │←── SessionList ───────────────────│
  │──── SwitchSession (threadId) ────│ 切换会话绑定: 清空 delta 缓冲,
  │                                   │ 回推新会话全量 Sync + 模型信息 + 上下文统计
  │                                    │
  │ (可选) 中断过期通知
  │←── InterruptExpired ──────────────│ 中断超时/断线宽限期满/会话取消时,
  │                                   │ 客户端将对应中断消息标记为过期并结束等待
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
│   │   │   ├── config.h          # AgentConfig / ModelConfig 配置
│   │   │   ├── config_static.h   # 静态路径配置
│   │   │   ├── context.h         # AgentContext / Session / SessionStore / ContextStats
│   │   │   │                     #   Session: 线程绑定 (viewMessages/chainHash 单线程读写)
│   │   │   ├── checkpoint_store.h # 单检查点存储: SingleCheckpointStore 策略基类 +
│   │   │   │                     #   InMemorySingleCheckpointStore (每 thread 仅最新,
│   │   │   │                     #   save 时自动淘汰历史, O(super-steps) -> O(threads))
│   │   │   ├── conversation_types.h # Delta(含NodeStart/End/seq/timing) / SyncPayload
│   │   │   │                     #   ViewMessage (UI 展示消息, role 拆分子结构) /
│   │   │   │                     #   ChainHash / AppendComponentNotification
│   │   │   ├── model_registry.h  # ModelProviderRegistry (运行时模型切换)
│   │   │   ├── session_persistence.h # 会话 SQLite 持久化: 按 threadId 分目录,
│   │   │   │                     #   session.db (viewMessages+llmMessages+meta) 与
│   │   │   │   │                   share_store.db 分库, 读取路径不创建目录
│   │   │   ├── prompt.h          # AgentPrompt / ToolPrompt 提示词管理
│   │   │   ├── training.h        # EvolutionTrainingAgent 进化训练 (变异/评估/优化/收敛检测)
│   │   │   └── io/           # 远程通信
│   │   │       ├── agent_server.h    # AgentServer (WS 服务, token 鉴权)
│   │   │       ├── session_server_agent_io.h # SessionServerAgentIO (会话驱动, delta 缓冲/重放, grace)
│   │   │       ├── wire_protocol.h   # Wire Protocol 消息类型与序列化
│   │   │       ├── agent_io.h        # AgentIOBase 端点基类 (client/server 操作契约)
│   │   │       ├── agent_io_transport.h # 传输层抽象基类 (connect/recv/send/close/alive)
│   │   │       ├── channel_io_transport.h # 进程内 Channel 传输 (零序列化)
│   │   │       └── ws_io_transport.h  # WebSocket 传输 (JSON 编解码/心跳/重连)
│   │   ├── deps/                 # 依赖注入
│   │   │   └── injector.h        # DependencyContainer (工厂/单例/有名称注册)
│   │   ├── nodes/                # Graph 节点
│   │   │   ├── wrap_handle.h     # WrapHandleBaseNode 栈式中间件基类
│   │   │   ├── modelcall.h       # ModelCallWrapNode (LLM 调用, 动态模型切换)
│   │   │   ├── toolcall.h        # ToolcallWrapNode (工具分发, 自动压缩)
│   │   │   └── agentcall.h       # AgentStart/EndCallWrapNode (会话生命周期)
│   │   ├── plugin/               # 插件系统 (热插拔原生 C++ 插件, 纯 C ABI)
│   │   │   ├── plugin_api.h      # 纯 C ABI 契约 (唯一跨版本稳定接口, 见 docs/zh-cn/plugins.md)
│   │   │   ├── plugin_manager.h  # PluginManager 生命周期 (load/enable/disable/unload) /
│   │   │   │                     #   PluginTool (C 回调→线程池卸载执行) /
│   │   │   │                     #   PluginMiddlewareHandle (7 钩子→C 回调) /
│   │   │   │                     #   CapabilityRegistry / NativeLoader (dlopen↔LoadLibraryW)
│   │   │   └── tool_registry.h   # 动态插件工具查表 (shared_ptr 保活, 静态工具名冲突检测)
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
│   │       │                     #   连接池: keep-alive 空闲连接复用 + 每端点并发上限
│   │       │                     #   (maxConcurrentConnections, 默认 5), 复用失效自动重试;
│   │       │                     #   空闲连接按 io_context 分桶 (跨上下文复用 socket 是 UB),
│   │       │                     #   HttpPoolContextGuard 服务随 io_context 销毁自动释放
│   │       │                     #   该上下文上的空闲连接, 避免悬挂 reactor 的 use-after-free
│   │       ├── http_server.h     # HTTP 服务器 (路由/WS/SSE/SSL)
│   │       ├── http_header.h     # HeaderMap (忽略大小写的 HTTP 头部管理)
│   │       ├── ws_client.h       # WebSocket 客户端
│   │       ├── exception.h       # 异常处理工具
│   │       ├── lru_cache.h       # LRU 缓存
│   │       ├── diff_util.h       # 行级 diff (unified diff 格式)
│   │       ├── regex.h           # 正则引擎 (hyperscan)
│   │       ├── aho_corasick.h    # Aho-Corasick 多模式匹配
│   │       ├── router.h          # HTTP 路由器
│   │       ├── sqlite.h          # SQLite 轻量 RAII 封装 (SqliteDb/Stmt, WAL+busy_timeout)
│   │       ├── async_mutex.h     # 协程感知异步互斥锁 (基于 concurrent_channel)
│   │       ├── async_offload.h   # 阻塞操作线程池卸载 (offloadAsync /
│   │       │                     #   offloadCancellableAsync / asyncWithTimeout)
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
│   │   │   │   ├── agent_stdio.h # StdIOClientAgentIO (stdin/stdout 交互)
│   │   │   │   └── stdin_reader.h # 异步 stdin 读取器
│   │   │   └── tui/
│   │   │       ├── agent_tui.h   # TUIClientAgentIO (FTXUI 终端 UI, 接收/显示/排队/权限/日志)
│   │   │       ├── scrollable.h  # Scrollable (全量构建的可滚动容器, 侧边栏等短列表用)
│   │   │       ├── lazy_scrollable.h # LazyScrollable (懒构建+LRU有界缓存+视口局部渲染)
│   │   │       ├── tui_theme.h   # TUI 主题配色
│   │   │       ├── framework/    # TUI 框架层
│   │   │       │   ├── tui_state.h       # TUI 状态聚合 (消息/侧边栏/排队输入等)
│   │   │       │   ├── tui_context.h     # TUI 渲染上下文 (theme/state/尺寸)
│   │   │       │   ├── tui_settings.h    # TUI 全局设置单例 (主题/系统资源显示开关等)
│   │   │       │   └── modal_container.h # 浮层容器 (权限/中断弹窗)
│   │   │       └── components/   # TUI 渲染组件
│   │   │           ├── message_list.h # 消息列表渲染
│   │   │           ├── sidebar.h      # 右侧边栏 (日志/信息/Planning)
│   │   │           ├── overlays.h     # 浮层 (权限/中断/模型选择)
│   │   │           ├── input_bar.h    # 输入栏
│   │   │           └── status_bar.h   # 状态栏 (上下文占用/活动状态)
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
│       │       ├── scrollable.cpp
│       │       ├── lazy_scrollable.cpp  # LazyScrollable 懒构建渲染实现
│       │       ├── tui_sidebar_content.cpp # 侧边栏内容 (日志/信息/Planning)
│       │       ├── tui_log_sink.cpp        # TUI 日志接收器
│       │       ├── framework/              # TUI 框架层实现 (tui_state/modal_container/...)
│       │       └── components/             # 渲染组件实现: message_list / sidebar /
│       │                                   #   overlays / input_bar / status_bar
│       ├── train/train.cpp        # 训练实现
│       └── util/util.cpp          # 客户端工具实现
│
├── test/                         # agentxx_test 测试程序
│   ├── test.cpp                  # 测试入口: 模块注册与调度 (同步/异步/平台模块分组)
│   ├── test_framework.h          # 测试框架 (断言宏 / TestResult)
│   ├── core/test_plugins.*       # 插件系统测试 (加载/工具/钩子/事件/热插拔, 模块名 `plugins`)
│   ├── test_agent.*              # CodeAgent 集成测试 (模拟 LLM Server: 工具调用/多轮/权限模式/重试耗尽/异常拦截)
│   ├── test_events.*             # 事件类型测试
│   ├── test_event_stream.*       # EventBus / EventStream / RequestResponseStream 测试
│   ├── test_event_bridge.*       # EventBridge 事件翻译测试
│   ├── test_interrupt_bus.*      # 中断总线 HIL 测试
│   ├── test_subagent_bus.*       # 子代理总线测试 (含批量委派/跨 agent 路由)
│   ├── test_concurrency.*        # 并发测试
│   ├── test_cancel.*             # 取消语义测试 (CancelToken 双通道/operation_aborted 转换)
│   ├── test_message_supplement.* # 消息补全/修复测试
│   ├── test_summarization.*      # 上下文压缩测试 (token 统计/去重/LLM 压缩)
│   ├── test_checkpoint_store.*   # 单检查点存储测试 (InMemorySingleCheckpointStore)
│   ├── test_memgrowth.*          # 多轮内存增长测试 (泄漏检测)
│   ├── test_toolcall_args.*      # 工具参数类型自动修正测试
│   ├── test_session_persistence.* # 会话 SQLite 持久化测试 (消息/上下文/share store 落库与重启恢复)
│   ├── test_remote_agent.*       # 远程 Agent (WS 传输 / SessionServerAgentIO) 测试
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
│   ├── test_aho_corasick.*       # Aho-Corasick 多模式匹配测试
│   ├── test_util_misc.*          # 杂项 util 测试
│   ├── test_settings_db.*        # 全局设置 SQLite 测试
│   ├── test_network_timeout.*    # 网络超时行为测试
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
│   ├── test_misc_fixes.*         # 杂项修复测试
│   └── client/                   # client 侧测试 (AGENTXX_BUILD_CLIENT 条件编译)
│       ├── test_config_loader.*  # YAML 配置加载测试
│       ├── test_mermaid_state.*  # Mermaid 状态图解析测试
│       ├── test_thread_id.*      # threadId 生成唯一性测试
│       ├── test_tui_input.*      # TUI 输入测试
│       ├── test_tui_interrupt.*  # TUI 中断交互测试
│       ├── test_tui_scroll.*     # TUI 滚动测试
│       ├── test_tui_settings.*   # TUI 设置持久化测试
│       └── test_tui_stream.*     # TUI 流式渲染测试
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
│   ├── markdown_ftxui/           # markdown-ui: Markdown 渲染 (cmark-gfm 解析 + FTXUI DOM),
│   │                            #   含 Mermaid stateDiagram-v2 状态图渲染 (state_diagram.*)
│   ├── NeoGraph/                 # 图引擎 (LLM 调用/工具分发)
│   ├── OpenSSL/                  # TLS/SSL
│   ├── quickjs/                  # QuickJS (JS 插件引擎, submodule quickjs-ng, AGENTXX_ENABLE_PLUGIN_JS)
│   ├── simdjson/                 # JSON 解析
│   ├── sqlite3/                  # 数据库
│   ├── uchardet/                 # 编码检测
│   ├── yaml-cpp/                 # YAML 解析
│   └── zlib/                     # 压缩
│
├── plugins/                      # 插件 (独立动态库/目录, 仅依赖 plugin_api.h)
│   ├── example_native/           # 一期示例: C++ 插件 (工具/钩子/事件/能力)
│   ├── javascript_engine/                # 二期: JS 解释器插件 (QuickJS, interpreter.js 能力)
│   └── example_js/               # 二期示例: JS 插件 (plugin.yaml + plugin.js)
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
  │     │     ├── viewMessages + chainHash (仅 io 线程读写, client 经 Wire 拷贝传输)
  │     │     ├── llmMessages (io 线程读写)
  │     │     ├── cancelToken / modelName (io 线程读写)
  │     │     ├── activity / contextStats (atomic, 跨线程安全)
  │     │     ├── deltaSeq (普通 uint64_t, 仅 io 线程递增)
  │     │     └── io / bus (会话级)
  │     ├── ModelProviderRegistry
  │     └── EventBus
  └── AgentConfig → ModelConfig / AgentPrompt

CodeAgent (继承 BaseAgent)
  ├── 工具: Filesystem | Command | Web | RAG | SubAgent | MCP | CodeGraph | ...
  └── 中间件: Permission → Skill → MemoryFile → Summarization → Planning → LogPrint

SessionServerAgentIO (远程会话驱动)
  ├── AgentIOBase (服务端端点)
  ├── BaseAgent.runConversationTurnAsync()
  ├── deltaBuf (断线缓冲) + grace timer
  └── AgentIOTransportBase (从 AgentServer 传入)

Client (agentxx_cli)
  ├── TUIClientAgentIO / StdIOClientAgentIO → AgentIOBase
  ├── ChannelAgentIOTransport / WsAgentIOTransport → AgentIOTransportBase
  ├── ConfigLoader → YAML + .env
  └── ModeRunners → local/remote × tui/cli 组合
     ├── runLocalTuiUnified / runLocalCliUnified
     └── runRemoteTui / runRemoteCli

EventBus (事件总线)
  ├── EventStream<T> (单向: publish/subscribe/unsubscribe)
  ├── RequestResponseStream<Req, Resp> (双向: request/serve)
  └── 主题表 (Topic 命名空间常量)
