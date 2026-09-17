# Agentxx 整体设计文档
> 相关文档: [plugins.md](plugins.md) (纯 C ABI 插件范式) · [ffi.md](ffi.md) (FFI 接口设计) · [tui.md](tui.md) (TUI 实现与架构)

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
- **流式输出**: LLM 响应以增量 Delta 事件推送 (TextToken / ThinkToken / ToolStart / ToolEnd / TurnStart / TurnEnd / NodeStart / NodeEnd / MessageUITip / InsertMessage)，每个 Delta 携带单调递增 seq 用于重放与同步; 轮次统计/错误/取消提示/中断头消息由 agent 线程构造为完整 ViewMessage 经 InsertMessage 插入会话历史并推送 (携带 msgId), 保证 viewMessages 与 UI 展示一致
- **多模型支持**: 运行时按会话 (sessionId) 动态切换模型，支持 OpenAI Chat Completions、Anthropic Messages、OpenAI Responses (Codex) 三种 Provider 协议
- **上下文压缩**: SummarizationMiddleware 在上下文接近模型 token 上限时自动压缩历史消息，支持 toolcall 输出去重与截断; 压缩完成后**立即**回写会话 `llmMessages` 并请求节流落盘 (崩溃/被杀时不丢压缩结果, 重启后不会因上下文重新超限而反复压缩), 同一会话压缩互斥 (手动 Summy Context 与轮内自动压缩不并发), 压缩提示消息按挂起 id 复用 (中断续跑不产生重复提示)
- **思维链展示**: 支持 LLM 的 thinking/reasoning_content 流式输出与展示
- **节点级事件**: NodeStart/NodeEnd 事件标记 Graph 节点执行生命周期，便于 UI 展示进度

### 工具调用 (ToolCall)

丰富的工具集，按功能分类。编程基础类工具 (文件系统 / 命令执行 / 网络 /
知识检索 / 字符串 / 系统时间 / 规划写入) 已从 lib 内置实现拆分为**独立插件**
(同名同行为, 见 `agent/plugins/agentxx_*`)，经 yaml `plugin.list` 段配置加载，
或构建期经 `AGENTXX_PLUGIN_BUILTIN_LIST`
合并编译进 libagentxx (默认不内置)；lib 内仅保留 share_store / subagent /
git_worktree 及延迟加载装配 (`ToolSkillSearchSubAgentTask` 模板类, 当前未独立注册为 tool)：

| 分类 | 工具 | 说明 |
|------|------|------|
| **文件系统** | `agentxx_filesystem_list` | 列出文件/文件夹信息 (大小、类型、修改时间)，支持递归与 `path` 通配 (`*` / `?` / `[...]` / `**`：含通配符时展开模式并列出匹配条目本身，`recursive` 再下钻匹配到的目录) |
| | `agentxx_filesystem_read` | 按行读取文本文件，支持 offset/limit |
| | `agentxx_filesystem_write` | 创建/覆盖文本文件 |
| | `agentxx_filesystem_edit` | 精确字符串替换编辑文本文件 |
| | `agentxx_filesystem_glob` | 按 glob 模式搜索文件 |
| | `agentxx_filesystem_grep` | 按纯文本 (text_patterns) / 正则 (regex_patterns) 搜索文件内容 (可同时指定取并集) |
| **命令执行** | `agentxx_execute_bash_command` | 执行 Linux shell 命令，支持超时控制 (Linux/macOS)；插件启动时探测 python/node 可用性与版本并写入工具提示词 |
| | `agentxx_execute_windows_command` | 执行 Windows 命令，默认 PowerShell (插件启动时自动探测 pwsh/powershell 并注入可执行文件名与版本号到工具提示词)，未找到时回退 cmd.exe (Windows / WSL 下调用) |
| | `example_js_execute_command` | 通过 QuickJS 解释器执行 JavaScript 代码 (execute_bash_command 的 JS 等价物, 依赖插件 `agentxx_javascript_engine`) |
| **数学计算** | `agentxx_math_calculate` | 数学表达式解析与计算 (四则运算、幂、阶乘、位运算、比较逻辑、常量、三角/双曲/对数/组合排列等函数、隐式乘法) |
| **网络** | `agentxx_web_search` | 网络搜索 (DuckDuckGo / 模型搜索) |
| | `agentxx_web_fetch` | HTTP GET 获取网页原文 |
| | `agentxx_web_fetch_markdown` | 获取网页并转为 Markdown |
| **知识检索** | `agentxx_rag_search` | 基于向量相似度的知识库语义搜索 |
| **代码分析** | `agentxx_codegraph_search` | 按名称搜索代码符号 |
| | `agentxx_codegraph_context` | 获取符号的定义、调用者、被调用者 |
| | `agentxx_codegraph_callers` / `agentxx_codegraph_callees` | 调用图正向/反向追踪 |
| | `agentxx_codegraph_path` | 查找两符号间的调用链路径 |
| | 实际注册 5 工具 (search/context/callers/callees/path; 日志 `loaded (6 tools)` 中的第 6 个为计数口径含 client 侧 Info 段, 非 agent 工具); 仅当该插件经 yaml `plugin.list` 段配置加载且编译启用 `AGENTXX_ENABLE_PLUGIN_CODEGRAPH` 时注册 |
| **规划** | `agentxx_planning` | 两层任务规划 (Mermaid 状态图 + Todo List + 备忘录; 双端插件: 规划持久化到 `{dataDir}/plans/{thread}.json`, 发布 `agentxx_planning.planning` 事件, client 侧经类型级工具渲染器 (实时+历史回溯) 与实时装饰+Info 段落渲染, 订阅 `agentxx_host.client_attached` 做接入重发自愈) |
| **子代理** | `agentxx_subagent` | 创建和管理子代理执行委派任务 (单任务字段 subagent/message, 或批量 tasks 数组并行; 由 SubagentManager 中间件持有单实例注入, 默认注册 `subagent_task`) |
| | `tool_skill_search` 逻辑 | 延迟加载工具/技能的搜索: `ToolSkillSearchSubAgentTask` 仅为 system prompt 模板 (当前未独立注册为 tool, 由 subagent 按需内联检索逻辑) |
| **数据** | `agentxx_share_store` | 会话级文本寄存，节省上下文 |
| | `agentxx_string_html_to_markdown` | HTML 转 Markdown |
| | `agentxx_string_regexp` | 正则搜索/替换/移除 |
| **系统** | `agentxx_get_current_datetime` | 获取当前日期时间 |
| | `agentxx_get_system_core_info` | 获取 CPU/内存/GPU 使用率 |
| **UI 控制** | `agentxx_ui_control_keyboard_mouse` | Windows 键鼠控制 (仅 Windows, 由 `agentxx_computer_use` 插件提供, depends: screen_capture) |
| **屏幕捕获** | `agentxx_screen_capture` | 屏幕截图/流式捕获 (仅 Windows) |
| **音频流** | `agentxx_audio_stream` | 系统/程序/麦克风音频流捕获 (**全平台跳过构建**: WASAPI 实现未启用, 当前仅桩实现; 平台矩阵见 plugins.md) |
| **文本选择监听** | `agentxx_text_selection_monitor` | 系统级文本选择事件流 (仅 Windows UIAutomation) |
| **JS 执行** | `example_js_execute_command` | QuickJS 执行 JS 代码 (depends: `agentxx_javascript_engine` 的 `interpreter.js` 能力) |

工具特性：
- **自动压缩**: 工具输出超过阈值 (`toolcallSummaryLimitOutputLength`, 默认 2K) 且该 tool 启用 `autoSummaryOutput` 时压缩摘要 (经 share_store 卸载原文)
- **延迟加载**: 插件工具按需注册；`XXToolBase::canDelayLoad` 标记可延迟工具 (默认 true), 初始仅名称注入 system prompt
- **参数自愈**: `ToolcallWrapNode::autoFixArgsType` 按 JSON Schema 自动修正参数类型 (string↔数组/数值/布尔互转), 提高模型兼容性
- **重复调用检查**: 启用 `repeatCallCheck` 的 tool 在同一 llm↔tool 链内连续同名同参调用达阈值 (`toolcallRepeatCheckThreshold`, 默认 5, 0=禁用) 时经 permission 总线询问用户确认，防止模型陷入死循环；重置轮次时自愈
- **去重机制**: 文件读写等工具支持 SummarizationToolHandle，重复调用时截断旧结果
- **上下文修复优化**: `ModelCallWrapNode::repairMessages` 在调用 LLM 前自动检查和修复上下文结构（合并连续同角色消息、规范化 tool_call 与 tool_result 配对），采用按需验证与最小拷贝优化，显著降低深轮次对话的开销
- **事件驱动取消**: 命令执行等重型工具接入插件开发框架通用设施 `CancelRegistry`，支持跨线程排他防悬挂锁与即时回调通知，在 Windows 与 Linux/POSIX 下毫秒级即时终止子进程组与管道，而非单纯依赖休眠轮询
- **MCP 扩展与容错**: 通过 MCP Client 连接外部 MCP Server，动态注册远程工具 (支持 HTTP SSE 和 stdio 传输, 命名空间前缀隔离, 默认 120s 初始化与调用超时)；初始化失败的组件统一记录于 `appendComponentInfo.failedComponents`，供 UI 集中查看和统计

### Git Worktree 模式 (yaml `worktree.enable`, 默认关闭)

开启后注册 `agentxx_git_worktree` 工具 (`agentxx::tools::GitWorktreeTool`) 。设计目标: 同一仓库
目录启动的多个 agent 会话各自在独立 worktree 内开发, 代码修改互不影响
(参考 Claude Code `--worktree` / `EnterWorktree` 的分层约束设计)。

- **工具操作**: `create` (创建+绑定) / `info` (列表+当前绑定) / `status`
  (未提交变更与未合并提交摘要) / `remove` (删除; 默认保留策略下唯一删除入口)
- **创建即绑定**: create 成功即写入 `Session::WorktreeBinding`
  (`{repoRoot}/.agentxx/agent/worktrees/{name}`, 分支 `agentxx/wt-{name}`,
  基于 base_ref 参数默认当前 HEAD), 该会话后续所有相对路径解析基准、
  命令执行子进程初始目录自动切换到 worktree —— 正确行为是平台默认行为而非依赖模型自觉;
  同时向 `.git/info/exclude` 追加 `.agentxx/` (本地生效不污染 tracked 文件)
- **权限隔离边界** (代码级约束): 绑定时向 PermissionMiddleware 注册每会话隔离
  (`SessionFsIsolation`): 主检出子树写操作 DENY (读不受限), 隔离优先于白名单/
  模式默认规则; worktree 子树 (`allowPath`) 是该约束的例外 —— worktree 位于主检出
  `.agentxx/agent/worktrees/` 内, 其读写按已注册规则照常处理 (否则会话对自身工作区
  的写操作也会被拒绝); filesystem 工具的相对路径经 `normalizePermissionPath(path,
  sessionId)` 按 worktree 解析, 权限规则与实际访问路径稳定匹配
- **插件链路跟随**: 插件接口表 `agentxx.agent.config` 的
  `get_session_work_dir(host, session_id)` (worktree 绑定优先, 为空回退默认会话工作目录); filesystem 插件
  改为每次 execute 按注入的 sessionId 动态解析 (原 entry 时静态缓存),
  execute_command 插件同语义 —— 会话绑定后两个插件的路径基准即时切换
- **子代理继承**: AgentHost 派生子代理时继承父会话绑定 (子代理 config.workDir
  预置为 worktree 路径 + `inheritedWorktreePath` 标记), 其 permission Ask 默认
  规则/全部工具链自动落入同一 worktree
- **生命周期**: worktree 始终保留 (keep 策略), 不随会话结束删除; remove 操作
  经双层脏检查 (未提交变更/未跟踪文件/未合并提交, 无上游时回退统计分支全部提交),
  有工作成果时拒绝并提醒先 commit, 仅 force=true 可强制删除
- 底层 git 封装见 `agent/lib/include/agentxx/util/worktree.h` (argv 直调不经
  shell, 超时整组终止; 测试模块 `worktree`)

### 中间件系统

采用栈式中间件架构，在 Graph 节点执行前后插入处理逻辑：

| 中间件 | 功能 |
|--------|------|
| **PermissionMiddleware** | 工具调用权限控制，经事件总线向用户请求授权 (HIL)。**工具权限限制由工具来源方声明**: 插件在注册工具后经 `agentxx.agent.permission` 接口表声明 (作用域 读/写、目标参数名、目标类型 路径/文本/无, 见 [plugins.md](plugins.md) §8)，未声明的工具不参与权限判定 (直接放行)；中间件内不再硬编码任何工具名。文件系统权限按最长前缀匹配文件夹规则，支持 `*` 通配符：`/data/projects` 的规则对其下任意子路径生效，且父链规则可回退 (见 `XXRouter::get` 的 `prefix_fallback`)。默认规则由 yaml `permission.mode` 决定 (Ask=工作目录内 ALLOW + 其余 INTERRUPT / AllAsk=全部 INTERRUPT / Pass=全部 ALLOW / Deny=全部 DENY)，白名单 (whitelist) 始终放行、黑名单 (blacklist) 始终拒绝 (同路径黑名单优先)，未命中任何规则时由 `noRuleOperator` 兜底。客户端可"记住本次选择"：权限询问卡片由声明式 UI 描述渲染 (见 [interrupt_ui.h](/agent/lib/include/agentxx/middlewares/interrupt_ui.h) 与预设模板 `preset::permissionCard`)，勾选值经中断结果 `values.remember` 回传，IO 端点据此在应答 (`RespPermission.remember`) 中带回用户意图，由权限中间件注册路径规则 (后续直接放行/拒绝不再询问；客户端与端点都不参与权限语义，规则表归中间件所有)。记住目录时该规则自动覆盖其全部子目录与文件 (同一最长前缀匹配语义)，读、写各自一套规则互不影响 |
| **SkillMiddleware** | 技能文件 (SKILL.md) 的渐进式发现与加载 |
| **MemoryFileMiddleware** | 上下文文件 (Memory) 读取与缓存，每次模型调用时注入系统提示词 |
| **SummarizationMiddleware** | 上下文 token 统计与自动压缩，防止超出模型上下文窗口 |
| **SubagentManagerMiddleware** | 子代理委派管理: 持有 `SubAgentManagerTool` 单实例 (`agentxx_subagent`), 按 yaml `subagent.enable` 决定是否注入给模型; 事件总线服务 `service.subagent.execute` 始终注册 (供上下文压缩等内部路径调用) |
| **AgentHost** | 进程级 agent 宿主: 主 agent 与子代理平等注册 (AgentNode), 派生独立 agent 运行子代理, 在根与每个子代理的全局总线上 serve service.subagent (委派扁平化: 嵌套委派与根委派同路径), 强制深度/并发预算, HostBus 跨 agent 消息路由, 生命周期回收 |
| **EventBridge** | 将 GraphEngine 事件翻译为 EventBus 强类型事件 |
| **LogPrint** | 调试日志输出中间件 (条件编译，按配置控制日志级别) |

### 事件系统

- **EventBus**: 强类型事件总线，支持单向事件流 (`EventStream<T>`) 和请求-响应流 (`RequestResponseStream<Req, Resp>`)
- **事件主题**: 按 `Topic` 命名空间常量组织，范围覆盖 `agent.*`、`service.*`、`io.*`
- **订阅机制**: 支持常驻订阅和执行次数限制 (execHit) 的自动移除订阅
- **HIL (Human-in-the-Loop)**: 中断/权限请求经 RequestResponseStream 派发到客户端 UI，支持超时
- **定时器**: EventBus 内置定时器事件流，支持 once/repeat 模式

### 多会话与并发

- **Session 隔离**: 每个 sessionId 独立的 Session (graph ctx 字段名为 thread_id, 两者同值)，包含 IO、EventBus、ContextStats、CancelToken、模型选择、消息历史
- **SessionsManager**: 会话管理器，按 sessionId 取/建 Session (仅 agent io_context 线程访问，无需锁保护); `SessionStore` 特指其挂接的 SQLite 持久化实例
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

- 开关: `AgentConfig::enableSessionStore` (默认关闭; agentxx_cli 在 `buildDefaultConfig` 中开启)。
  开启后由 BaseAgent 创建 `SessionStore` 并注入 AgentContext
  (`AgentContext::sessions->sessionStore`; 要求 dataDir 非空或显式指定了
  `sessionStoreDirectory`, 否则自动禁用仅存内存):
  - 会话消息状态恢复/落库 (经 `SessionsManager` 在创建 Session 时挂接)
  - `MiddlewareContext` 构造参数 (share store 写穿)
- 数据目录: `{dataDir}/sqlite/sessions/{sessionId}/` (sessionId 经 `sanitizeSessionId` 清洗为安全目录名:
  非法字符替换/超长截断/Windows 保留名规避, 发生改写时附加 FNV hash 尾缀防碰撞;
  dataDir 由 yaml `data_dir` 指定, **未配置 dataDir 且未指定 sessionStoreDirectory 时不持久化**:
  设置/会话/codegraph 数据仅存内存, BaseAgent 初始化时输出警告)
- 分库设计 (单库 `session.db`, 四表, 启用 WAL + busy_timeout):
  - `view_message` 表: viewMessages (append-only, 每消息一行 JSON)
  - `llm_context` 表: llmMessages (单行整体替换)
  - `meta` 表: msgIdCounter/title/lastActiveMs
  - `store` 表: agentxx_share_store KV 条目 (id 自增 = 现有最大 id + 1,
    重启后延续) —— 与消息历史同一生命周期 (随 session 创建/删除),
    同一 io 线程写入, 互斥锁串行保护
- 接入点:
  - `SessionsManager::getOrCreate`: 创建 Session 时从 SQLite 恢复 viewMessages/llmMessages,
    重建链式哈希 (对不含 id 的消息内容, 与 appendViewMessage 语义一致),
    恢复 msgIdCounter 保证新消息 id 不冲突; 并绑定 `SessionStoreHooks`
    (std::function 回调, context.h 不依赖 sqlite 头)
  - `Session::appendViewMessage` / `updateViewMessage`: 追加/回填经节流器落库
    (消息 + 计数事务提交; update 按 msg.id 更新对应行, 如 tool 结果回填
    toolFinished/toolResult/collapsed, 保证重启恢复的历史与内存状态一致)
  - **持久化节流** (`Session::kPersistThrottleMs` = 3s): 首次触发立即落盘,
    窗口内的后续触发合并 (view 压入待落盘操作队列保持 append/update 顺序回放;
    llm 仅更新内存), 待下次触发或轮末强制补存收敛。目的: 进程在轮次中途
    被杀/崩溃/自杀 (如 agent 执行 taskkill 清理自身) 时, 已结算的消息最多丢失
    一个节流窗口 (<3s), 而非整轮 —— 如果 viewMessages 逐条即时落库、
    llmContext 仅轮末保存, 反复中途被杀的会话会出现"view 完整而 llm 上下文
    滞后/为空"
  - `EventBridge::handleChannelWrite`: LLM 上下文增量的结算挂点 —— 节点对
    messages channel 的写入事件即该批消息定稿 (assistant 回复完成 / tool 结果
    写回, 非流式 token 粒度), 经 `Session::appendSettledLlmMessages` 追加并触发
    节流保存。input 注入 / 节点内 overwrite (system 注入、压缩) / cancel 直写
    不产生该事件, 不会重复追加; 与引擎状态的短暂漂移由轮末权威同步收敛
  - `BaseAgent::runTurnAsync`: 轮末回调保存 llmMessages (整表替换, 权威终态) +
    flushViewMessages (补存节流窗口内未落盘的 view 操作)
  - `MiddlewareContext` share store 四方法: 内存 map 作读缓存 (首次访问某 session
    时从 DB 恢复全部条目与 id 计数器), 写操作同步写穿 DB
- 容错: 所有落库失败仅记录错误日志, 不影响内存状态与对话主流程 (尽力而为持久化);
  读取路径在目录不存在时直接返回空, 不创建目录/空文件 (避免 subagent 等
  只读访问产生垃圾目录)
- 连接缓存 (LRU): 写路径按 sessionId 缓存已打开的 `session.db` 连接
  (`SessionStore::dbs_`), 同时保持打开的连接数上限 `kMaxOpenSessionDbs` (32),
  超出时关闭最久未使用的连接 —— 每个连接占用 fd + WAL + page cache, 长期运行
  (会话很多) 时不淘汰会持续占用文件描述符 (Linux 默认 `ulimit -n` 常为 1024)
  直至无法打开新库; 淘汰只关连接不丢数据 (WAL 自动 checkpoint, 下次写入重开)
- 线程安全: `SessionStore` 内部互斥锁保护; 常规使用下调用发生在 agent io
  线程 (Session 绑定线程/工具执行), 锁仅在多线程并发访问时生效

#### 会话切换 (TUI 会话选择弹窗)

```
TUI [F4] 打开会话选择弹窗 → WireListSessions (服务端阻塞 I/O 卸载到 blockingPool)
  → 服务端回 WireSessionList (持久化会话列表, 按最近活动时间降序)
  → 用户确认 → WireSwitchSession(newSessionId)
  → SessionServerAgentIO::switchSession:
      重绑定 config_.sessionId → 清空 delta 重放缓冲 (新会话 seq 独立编号)
      → 重置 firstTurn_ (首条输入走 resume_if_exists=true 恢复路径)
      → 回推新会话 Sync (按 initialSyncTailCount 尾窗分页; 0=全量)
        + WireModelInfo + WireContextStats
  → 客户端 (TUI) 更新本地 sessionId 绑定; WS 模式同时
    transport->updateReconnectSessionId() (复位重连握手的 sessionId/lastSeq/tailHash)
```

- 仅当无进行中轮次时切换 (客户端前置拦截 + 服务端双重保护)
- 新会话历史由 SessionStore 从持久化恢复 (不存在时创建空会话)
- 会话列表数据源: `{dataDir}/sqlite/sessions/` 目录扫描 + meta 表 (sessionId/title/lastActiveMs)
- 回推的 WireModelInfo 与客户端接入时的 WireGetModel 响应同构 (含各模型多模态能力
  `capabilities`): 客户端据此判断输入框右侧 [+ 📎︎︎ 附件] 按钮是否展示; 客户端侧
  模型能力表与会话无关 (来自 agent 配置), 跨 Sync 保留不清空

#### Subagent 执行链路 (NodeInterrupt → 总线派发 → 宿主派生独立 agent)

```
父 agent LLM 发起 agentxx_subagent (单任务 = tasks 数组含 1 项, 批量 = 多任务)
  → SubagentManagerMiddlewareHandle (独立中间件, 持有 SubAgentManagerTool
    单实例; 按 AgentConfig::enableSubagent 决定是否把 agentxx_subagent
    注入 toolcalls, 事件总线服务始终注册)
      → SubAgentManagerTool::execute_async
          → 校验任务参数 (subagent 名合法 + message/messages 至少其一)
          → MiddlewareContext::requestInterrupt: 首次存储中断参数 ({tasks: [...]})
            到 graphData, 抛出 NodeInterrupt → engine checkpoint 暂停父图
  → AgentRunner (统一中断循环, 主 agent 与子代理共用同一实现):
      → 逐个解析 graphData 中的 interrupt args, 按 handle name 分派:
        - "subagent" 中断 (统一批量语义): 解析 ReqSubagentBatch (共享实现
          parseSubagentBatchFromInterrupt), 经本 agent 的全局总线
          service.subagent 请求委派 (旧单发 ReqSubagentStart 已合并)
        - 其他中断 (权限询问等 HIL): 经会话总线 service.interrupt 请求,
          超时统一取 IO 端点 interruptTimeout 配置; 根 agent 同时插入
          中断头 MessageTip
  → AgentHost::spawnBatch → spawnOneTask:
      → 每个任务派生"独立 agent" (独立 AgentContext / engine / SessionStore /
        中间件栈), 与主 agent 完全平等 (AgentNode); 配置为轻量子代理:
        不建 MCP 连接 / 不加载插件 / RAG / CodeGraph, 不注入父级 Skill/Memory,
        不持久化, 默认使用配置的 subagent 模型
      → 宿主强制嵌套深度 (maxDepth) 与并发预算 (maxConcurrentSubagents)
      → 子代理构造完成后, 宿主在其全局总线上 serve service.subagent
        (与根 agent 总线 attachRoot 挂接对称): 子代理作用域内的 "subagent"
        中断 (嵌套委派) 经本总线请求, 与根委派完全同路径 (扁平化,
        无逐级宿主函数直调)
      → HIL 冒泡: 子代理会话继承父会话的 io 与总线 (权限/中断询问直达用户);
        父会话从父 agent (parentAgentCtx) 的 SessionStore 查找, 嵌套时
        是上一级子代理而非根
      → 子代理由同一 AgentRunner 驱动中断循环 (无 checkpoint 持久化 /
        无 MessageTip; 中断未完成时才报错)
      → 取消令牌透传: 父取消级联中止子代理 (engine run 取消)
      → 进度经 hostBus agent.progress 发布, 结束经 agent.done 通知
      → 运行结束 (成功/错误/取消) 宿主立即回收 AgentNode: 会话与中间件状态
        随 AgentContext 析构整体释放, 无按 thread 累积泄漏
  → 结果经 interruptResult channel 写回 graphData
  → AgentRunner resume_async 恢复父图, execute_async 按
    (tool_call_id + "_") + (result_id | 任务序号) 提取结果返回
    (单任务返回纯文本, 多任务返回 json 数组)
```

- 子代理是独立 agent: 与根 agent 同构 (AgentNode), 消息上下文完全隔离
- 中断处理循环唯一实现 (AgentRunner): 主 agent 与子代理共用, 差异收敛为
  hooks (checkpoint 持久化 / 中断头消息 / 事件回调 / resume 前后处理);
  委派超时不限制 (子代理可能长时间运行), 否则可能导致根 agent 总线请求
  默认 30s 截断长任务的问题
- 中断结果 key 规则 (tool_call_id + "_") + (result_id | 任务序号) 收敛到
  共享实现 (makeSubagentResumeKey / buildSubagentResumeValues), 写入侧
  (AgentRunner) 与读取侧 (SubAgentManagerTool) 同一函数, 前缀避免同一轮
  多个中断的序号 key 互相覆盖, 支持同轮多任务并发
- subagent 工具注入与事件总线注册由独立中间件
  (SubagentManagerMiddlewareHandle) 管理: 按配置 (subagent.enable) 决定
  工具是否注入给模型, 与上下文压缩 (summarization 经 service.subagent.execute
  直接调用) 解耦
- 中断处理完成后清理 graphData 中的 interrupt args (避免同轮再次中断时
  重复处理已完成的任务)
- 跨 agent 消息 (agent.message): 本地 mailbox 路由 (持久会话 agent 扩展点),
  或经 A2A 桥接转发远程 agent (registerRemoteAgent); 未注册目标返回明确的
  not-implemented 错误
```

### 远程通信

- **WebSocket 服务**: AgentServer 提供 WS 服务，支持 token 鉴权
- **Wire Protocol**: 双向 JSON 消息协议 (Hello/HelloAck/UserInput/Cancel/SelectModel/GetModel/Delta/Sync/InterruptRequest/InterruptResponse/InterruptExpired/TurnResult/ContextStats/Error/Log/ModelInfo/GetAppendComponentInfo/AppendComponentInfo/GetContext/ContextMessages/Ping/Pong/ListSessions/SessionList/SwitchSession/GetViewMessages/ViewMessagesPage/ClearMessageQueue/RemoveQueueItem/InterruptAndRunNext/MessageQueueUpdate/PluginData/PluginDataUp);
  排队消息管理: 执行中排队由服务端按会话维护并经 MessageQueueUpdate 同步,
  客户端可删除单条 (RemoveQueueItem) / 清空队列 (ClearMessageQueue) /
  打断当前轮次立即执行队列首条 (InterruptAndRunNext); 插件事件经
  PluginData (agent→client 下行) / PluginDataUp (client→agent 上行) 原样转发
- **断线重连**: 客户端自动重连，携带 lastSeq 供增量 Delta 重放，seq 不连续时回退全量 Sync;
  客户端水位高于服务端当前 seq (服务端进程重启/会话重建后 seq 从 0 重新计数) 时同样回退全量 Sync,
  SyncPayload.deltaSeq 携带快照水位 (快照已含 seq <= deltaSeq 的全部增量), 客户端据此复位去重水位
- **历史分页 (viewMessages 尾窗同步)**: 长会话恢复时服务端仅同步末尾窗口
  (SessionServerAgentIO::Config::initialSyncTailCount, 本地 TUI 模式 =100,
  远程经 AgentServer::Config 透传, 0=全量); SyncPayload.fromIndex 携带窗口
  起始绝对下标、totalMessages 携带会话总消息数。客户端 (TUI) 向上滚动接近
  窗口顶部时发送 GetViewMessages(beforeIndex, count) 分页拉取更早历史, 服务端
  以 ViewMessagesPage(startIndex, totalCount, messages) 回应; viewMessages 为
  append-only, 绝对下标恒定, 前插不影响既有下标 (无竞态)。TUI 侧前插后经
  LazyScrollable::notifyPrepended 做滚动锚定 (既有条目缓存/实测高度随索引
  平移保留, 偏移按新增区高度在 prepareLayout 内以新快照口径全额补偿并随
  实测增量收敛), 视口内容保持稳定
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
  - 消息与流式末尾 Thinking 支持点击折叠/展开: 已提交消息点击切换
    msg.collapsed; 流式输出中的末尾 Think 子项点击切换组件内覆盖态
    (MessageListComponent::streamThinkOverride_, 三态: 未点击跟随
    TailThinkingMode 设置 / 点击折叠 / 点击展开), 新流开始与流结束时重置,
    命中区域由上一帧 visibleBoxes 反推 (collapsibleBoxes_ +
    collapsibleIsStream_ 区分消息区/流式区)
  - 流式 token 实时渲染 (COW 按需拷贝避免 O(n²) 累积拷贝)
  - 权限询问卡片 + "记住本次选择" (中断 UI 描述驱动; 勾选值经结果 options 回传,
    由权限处理器在 agent 侧注册路径规则)
  - 模型选择器 (运行时切换)
  - 右侧边栏 (日志窗口 / 信息面板 / Planning 展示)
  - 待发送消息队列 (执行中排队，轮次结束自动派发; 队列由服务端按会话维护并经
    MessageQueueUpdate 同步展示, 支持删除单条 / 清空队列 / 打断当前轮次立即执行首条
    —— insert 按钮, 经 WireRemoveQueueItem/WireClearMessageQueue/WireInterruptAndRunNext)
  - 模型选择待应用机制: 模型选择弹窗确认后不即时切换, 而是随下一条发送的用户消息
    (WireUserInput.model) 携带, agent 执行新一轮时自动应用 (远程 --model 参数同路径);
    立即切换仍可经 WireSelectModel
  - 多模态文件输入 (yaml `models[].image_input/audio_input/video_input`):
    仅当前模型支持任一多模态输入时, 输入框右侧展示 [+ 📎︎︎ 附件] 按钮
    (无键盘快捷键, 鼠标点击经 `modal_->pushModal` 打开 FilePickerOverlay);
    弹窗按模型能力过滤可选类型 (图片 png/jpg/jpeg/webp/gif/bmp; 音频
    wav/mp3/ogg/m4a/aac/flac; 视频 mp4/mov/webm/mkv; 非媒体不展示,
    不支持类型灰显不可选), 目录导航 (↑/↓ + Enter + Esc + 鼠标; 弹窗为
    纯导航列表, 不含文件名过滤输入框);
    选中后客户端读取并 Base64 编码为 RFC 2397 Data URL, 经大小预检
    (图像 ≤10MB / 音频 ≤25MB / 视频 ≤50MB / 单次 ≤5, 超限 toast 拒绝)
    挂载到输入框上方附件托盘 (✕ 可移除, Enter 随文本打包经
    WireUserInput.attachments 发送); 服务端排队保留附件并组装为
    ChatMessage image/audio/video_urls 送入 Provider; 消息列表以卡片展示
    附件元信息 (点击调系统查看器打开, 远端 dataUrl 先落盘临时目录);
    SQLite 落库剥离 dataUrl 仅留元数据; 上下文压缩时旧附件降级为
    [用户附带了图片/音频/视频] 纯文本标签
  - 文件编辑 diff 对比渲染
  - 中断询问的**声明式 UI 描述**渲染 (通用机制, TUI 不含任何具体询问类型
    ——含权限询问——的特化分支): agent 侧在 `InterruptHandleArg.ui`
    (schema 见 [interrupt_ui.h](/agent/lib/include/agentxx/middlewares/interrupt_ui.h))
    声明头行分段与**有序块列表**, 客户端 `InterruptView` 统一负责渲染/高度估算/
    命中区域/交互/结果组装:
    - **一条中断请求 = 一条消息 = 一份表单**: 描述可含多个控件块, 用户一次提交
      全部值; 描述**自包含** (客户端不读取消息上的其他字段)
    - **内容块可自由组合** (自定义渲染): `text` (样式/缩进/硬折行) / `markdown`
      (富文本, 客户端按 markdown 渲染; 行式前端打印原文) / `diff` / `separator` /
      `gap`; 预留 `custom` 块 (组件名 + 属性; 客户端组件渲染器未实现前渲染
      `fallback` 文本)
    - **控件块按交互形态声明** (`control`): `buttons` (候选项按钮,
      `commitOnPick` 点击即提交) / `select` (竖直单选列表) / `checkbox` /
      `text` / `number` (`integer`/`min`/`max`/`step`); 校验失败在该控件下方提示
      并阻止提交; 键盘作用于最近点击的控件 (焦点由点击切换)。**协议内没有
      "参数类型" (bool/int/enum...) 概念** —— 需要"若干类型化输入 + 确认"形态时由
      生产者用预设模板 ([interrupt_presets.h](/agent/lib/include/agentxx/middlewares/interrupt_presets.h):
      `preset::inputForm`/`permissionCard`/`confirmCard`) 生成描述
    - 结果**恒为对象形态** `{"values": {"<控件 id>": 值}}` (复选框=布尔/数值控件=
      数值/按钮与列表=候选项原始值/文本=字符串); 空对象 = 未应答/取消。权限询问的
      "记住此选择" 即勾选控件 (id `remember`), **规则注册在 agent 侧完成**
      (权限处理器按 `values.remember` 经总线注册路径规则), 客户端不参与权限语义
    - 描述**必填**: 走客户端 HIL 的中断必须携带描述 (生产者用预设模板/组装器构造);
      经总线由宿主处理、不进入客户端渲染路径的中断 (如 subagent 委派) 可为空。
      描述缺失/非法按契约错误处理 (诊断行且不可交互); 未知块类型忽略/降级
      (向前兼容); **不保留历史版本兼容**
    - 命中区域按**描述块下标** + 控件 id + 子序号定位; 渲染与估算同源: 单一布局
      过程 (行模型) 同时产出渲染元素与行数, 避免布局与估算两处漂移
      (见 [interrupt_view.h](/agent/client/include/agentxx-client/io/tui/components/interrupt_view.h));
      内容块渲染与插件工具装饰 items 复用同一实现
      ([ui_items_render.h](/agent/client/include/agentxx-client/io/tui/ui_items_render.h))
  - Mermaid stateDiagram-v2 状态图渲染 (消息中 ```mermaid 代码块 / Plan 弹窗显示 roadmap 状态图)
  - 上下文 token 占用状态栏
  - 主题切换 (持久化到 {dataDir}/sqlite/global.db)
  - 弹窗面性风格: 不使用边框与分割线, 标题栏/内容区/底部提示栏分别以主题的
    `surfaceHeaderColor`/`surfaceColor`/`surfaceFooterColor` 背景色区分 (错误类弹窗标题栏用
    `surfaceErrorHeaderColor` + 错误色标题文字); 弹窗打开时屏幕下层以 `surfaceScrimColor`
    铺满衬托弹窗表面
  - 弹窗外框 (+ 角标 + 统一留白): 弹窗是纯色填充的矩形, 四角以 "+" 角标绘制 (字符前景取
    内容区背景色、背景取弹窗外部色, 角标横向占 2 列: "+" 与其后/前的留白), 且由外框统一提供
    留白 —— 上下左右各 2 格内边距、
    标题栏/内容区/底部提示栏之间各 1 行间距, 因此标题文本与内容行都不需要自带首尾空格
    (翻译表相应键也已去掉装饰性空格); 弹窗按自然尺寸居中, 高度被约束时由内容区吸收多余高度;
    外框见 [surface.h](/agent/client/include/agentxx-client/io/tui/surface.h)
    (`TuiSurfaceStyle` + `tuiSurfacePopup`/`tuiSurfaceFrame`),
    配色定义见 [tui_theme.h](/agent/client/include/agentxx-client/io/tui/tui_theme.h)
  - 会话选择弹窗 (F4): 列出持久化会话 (WireListSessions), 确认后经 WireSwitchSession 切换, 服务端回推新会话 Sync (尾窗分页)/模型/上下文统计
  - 历史分页加载: 恢复长会话时初始仅展示服务端末尾窗口 (本地模式 100 条),
    向上滚动接近窗口顶部时经 WireGetViewMessages 自动分页拉取更早历史,
    前插后滚动锚定保持视口稳定; 到达会话开头 (historyWindowStart=0) 后不再请求
  - 启动连接状态 (banner 提示): TUI 启动后消息列表 banner 按 server-io
    连接状态显示 —— 启动中 (Connecting, 输入进入待发送队列, 连接完成后自动发送) /
    连接失败 (Failed, 显示"连接失败 + [重试]"可点击按钮重新连接) / 已连接 (正常输入);
    本地模式由 SessionServerAgentIO 驱动循环启动前回调 onServerReady 置就绪,
    远程模式由 mode_runners 连接协程驱动 (ConnState 存于 TUIRenderState::connState)
  - 启动进度逐步展示: server-io init() 各阶段 (模型注册表/中间件/加载 MCP server/RAG/
    加载插件(含运行环境检测: python/node 与 PowerShell 探测)等) 经
    AgentContext::ThreadSafeInitNotifier (互斥锁保证线程安全)
    → AgentIOBase::onServerProgress 上报, "启动中"banner 同步显示当前执行的操作,
    完成后显示按键提示 (banner itemKey 计入
    connState+startupProgress 使 LazyScrollable 缓存失效重建)
    远程模式由 mode_runners 连接协程驱动 (ConnState 存于 TUIRenderState::connState)
  - 多语言与国际化 (TuiI18n): 界面显示语言已完全与 YAML 解耦，统一由 TUI 设置窗口直接切换并持久化到 `{dataDir}/sqlite/global.db` 的 `tui.lang` (支持 Auto 自动识别系统语言 / ZhCn 简体中文 / EnUs 英文)，运行时无锁查表且支持格式化占位符。会话与模型提示词语言则由 Agent 端独立支持 `getLanguage/setLanguage`
    消息列表的角色标签与 Tip 级别文本同样随语言切换 (折叠态形如
    `+ [Think] ...` / `+ [Tip] # Warn · ...`，中文为 `+ [思考] ...` /
    `+ [提示] # 警告 · ...`；展开态只显示 `- [Tip] # Warn` 前缀, 正文在下一行;
    Warning/Error 才附加级别文本, Info 级别不附加 (前缀为 `[Tip] #` 后直接接预览分隔符))，
    头部前缀的列宽预算按标签实际显示宽度计算 (markdown::utf8_display_width),
    不按英文宽度写死; 切换语言时消息列表渲染缓存整体失效重建
    (agent_tui 的 onLanguageChange → MessageListComponent::invalidateCache)
  - 屏幕上方 toast 提示
  - 鼠标拖选复制: 左键拖选后松开复制到系统剪贴板 (Windows 走 Win32 API,
    其他平台走 OSC 52 转义序列, 依赖终端支持; 复制结果经 toast 提示)
  - 自动滚动吸附底部 (Scrollable 组件)
  - 系统资源占用 (CPU/内存) 与 CodeGraph 索引状态: 渲染已迁移到对应插件的
    client 侧 (agentxx_system_monitor 侧边栏 Info 栏段落 + 命令 /sysinfo /
    agentxx_codegraph Info 栏段落); 采集/定时亦在插件内
    (agentxx_system_monitor 在 agent 侧周期采集并 publish usage 事件, 经
    WirePluginData 通道回传 client 插件渲染), TUI 不发起资源请求、不解析/
    不渲染插件载荷
  - 插件 Info 栏段落扩展: client 插件可经 register_info_section 向侧边栏
    Info tab 注入段落 (标题 + items, items schema 与面板一致), 渲染在
    Append 组件列表之后; TUI 每帧从 client 插件注册表快照读取, 无需缓存
- **TUI 渲染模块化**: 将消息列表、侧边栏、弹窗、编辑工具渲染拆分到独立文件
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
  - 变异策略: 字符级随机变异 (UTF-8 码点安全) + LLM 生成变异; 子代评估前预去重
  - 评估: 运行测试用例集，支持精确匹配和 LLM 评分; 用例顺序随机打乱消除早终偏置;
    单用例失败计 0 分不终止训练; 评估后立即清理会话 (内存/SQLite 不随训练累积)
  - 优化: 基于反馈的 LLM 提示词补丁生成; patch 经规范化过滤空串字段
    (约定 ""=保持不变) 防止 prompt 被误清空
  - 精英复评: 每代对 top N 精英复评并做 EMA 平滑 (PromptVariant.smoothedScore),
    降低 LLM 评分噪声对排序/收敛判定的影响
  - 收敛检测与去重 (hash 相同再做完整字段比对防碰撞误删)
  - 取消支持: 配置 cancelToken 在代/用例边界轮询, 取消后保存进度优雅退出
    (train 模式经 asio::signal_set 接入 SIGINT/SIGTERM, 再次 Ctrl+C 强制终止)
  - 持久化: 原子保存 (临时文件+rename) + 备份轮转; 空 population 存档拒绝加载
- **训练配置**: 支持独立的训练模型、评分模型、优化模型;
  评分器/优化器使用轻量 BaseAgent (无工具链), 训练代理使用完整 CodeAgent

### 扩展能力

以下能力均已从 lib 内置拆分为 `agent/plugins/` 下的独立插件 (经 yaml `plugin.list` 段配置加载, 见 plugins.md):

| 模块 | 说明 |
|------|------|
| **ScreenCapture** | 屏幕截图与流式捕获 (多屏支持; 插件 `agentxx_screen_capture`, 仅 Windows) |
| **AudioStream** | 系统音频/麦克风/程序音频流捕获 (插件 `agentxx_audio_stream`, 全平台跳过构建: 实现未启用) |
| **TextSelectionMonitor** | 系统级文本选择事件监听 (插件 `agentxx_text_selection_monitor`, 仅 Windows UI Automation) |
| **CpuGpuMonitor** | CPU/内存/GPU 使用率查询 (插件 `agentxx_system_monitor`; 工具 + 周期采集 + client 侧渲染) |
| **CodeGraphManager** | 代码索引与符号分析 (基于 codegraph-cpp; 已拆分为插件 `agentxx_codegraph`): 索引范围由插件参数配置 (yaml `plugin.list` 段该插件条目的 `args`，字段语义由插件定义)：`paths` 加载路径列表 (可多个目录，未配置时按 `load_cwd` 默认索引当前工作目录)、`ignore_paths` 忽略路径 (支持 `*` 通配符)、`use_gitignore` 默认忽略 `.gitignore` 规则与 `.gitmodules` 子模块目录；遍历按目录剪枝 (忽略目录整棵子树不进入)，文件监听增量索引应用同一套过滤；sqlite 数据库存于 `{dataDir}/sqlite/codegraph/<折叠路径>/index.db`（深层折叠 + 单段截断控制长度，路径前缀匹配复用；dataDir 由 yaml `data_dir` 指定，未配置 dataDir 时插件自动跳过、索引不落盘） |

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

# Windows Release (Linux 交叉编译)
bash agent/script/cross_windows_release_build.sh

# Android (Linux 交叉编译)
bash agent/script/cross_android_release_build.sh
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
- 同步模块: `string_util` `regex` `diff_util` `events` `concurrency` `misc_fixes` `aho_corasick` `util_misc` `training` `settings_db` `toolcall_args` `ffi_c_api` (及 client 侧 `AGENTXX_BUILD_CLIENT`: `config_loader` `tui_settings` `tui_input` `tui_interrupt` `tui_scroll` `tui_sidebar` `tui_context_overlay` `tui_stream` `tui_tool_header` `sessionId` `mermaid_state`)
- 异步模块: `event_stream` `event_bridge` `interrupt_bus` `subagent_bus` `subagent_tool` `agent_host` `string_tools` `math_tools` `share_store` `session_persistence` `rag_search` `datetime` `filesystem` `command` `worktree` `web_search` `codegraph` `screen_capture` `cpu_gpu` `text_selection` `http` `network_timeout` `websocket` `remote_agent` `mcp` `acp` `a2a` `openai_provider` `anthropic_provider` `plugins` `plugin_resources` `plugin_multi_instance` `client_plugins` `cancel` `message_supplement` `summarization` `checkpoint_store` `agent` `memgrowth`
- 平台限定: `screen_capture` / `text_selection` 仅 Windows 有真实实现 (其余平台跳过); 测试入口另有 Warn/Error 透出 sink (`TestWarnErrorLogSink`), 插件加载失败等库内错误不再静默丢失

测试源目录划分: 根目录 (入口 `test.cpp`) / `core/` (lib 核心) / `plugin/` (插件系统与具体插件集成) / `client/` (TUI/CLI, 仅 `AGENTXX_BUILD_CLIENT` 编译);
测试头文件统一放在 `include/agentxx-test/` 下与源码同名的子目录 (`core/` `plugin/` `client/`), 源码一律以
`#include "agentxx-test/<子目录>/test_xxx.h"` 引用 —— 不以源码目录作为 include 根, 避免与其它库的同名头文件产生歧义。
新增测试模块约定: 头文件仅保留函数声明; 断言计数器定义在模块 cpp 的匿名命名空间内,
并在 cpp 内 `#define XX_TEST_PASSED g_xxx_passed` / `#define XX_TEST_FAILED g_xxx_failed`
映射 test_framework.h 断言宏, 测试函数末尾 `return TestResult{g_xxx_passed, g_xxx_failed};`
—— 禁止在头文件做宏覆盖或 extern 导出计数器 (跨 TU 宏泄漏曾导致多模块计数错乱)。

### 配置文件

配置文件为 YAML 格式 (默认 `{程序运行目录}/agentxx-config.yaml`, 支持 agentxx_cli --config 指定文件路径)，
按 base + overlay 两层加载 (见下方"分层配置"; base 为数据目录下的 `agentxx-config.yaml`, overlay 覆盖 base)，
其中部分变量支持 `${VAR}` 环境变量替换：

```yaml
model:
  # 合并策略 (可选; 与 base 层叠加时生效, 默认 merge = 继承并叠加)
  # overwrite:
  #   mode: merge          # merge(默认) | replace(整段只用本层, 不继承 base)
  #   remove:              # 从合并结果中剔除 base 的指定模型 (按 name)
  #     - some-old-model
  list:
    - name: "my-model"
      type: "openai"              # "openai" / "anthropic" / "openai-responses"
      base_url: "https://api.example.com"
      api_key: "${MY_API_KEY}"    # 从 .env 或系统环境变量解析
      model_name: "gpt-4"
      api_path: ""                # 自定义 API 路径 (如 "/v1/chat/completions"); 空则用默认
      send_thinking: false        # 是否把 thinking/reasoning_content 随上下文发送给模型
      request_reasoning_summary: true # send_thinking 开启时, 是否请求上游返回思考摘要 (Responses
                                  # API 的 include 参数)。opencode-muse-spark 等网关不支持
                                  # reasoning.summary_text 变体, 需设 false, 否则 API 400
                                  # (unknown variant reasoning.summary_text);
                                  # 也可用 extra_api_config 显式指定 include 数组覆盖
      ssl_verify: null            # true/false 显式控制 TLS 证书验证; 省略用默认策略
      connect_timeout: 16
      read_chunk_timeout: 60
      max_concurrent_connections: 5   # 该模型 API 端点的最大并发连接数 (默认 5, 0=不限制)
                                      # LLM 请求启用 HTTP keep-alive 连接池: 空闲连接复用,
                                      # 超过上限的并发请求排队等待空闲连接
      image_input: false              # 是否支持图像输入 (多模态; 默认 false)
      audio_input: false              # 是否支持音频输入 (多模态; 默认 false)
      video_input: false              # 是否支持视频输入 (多模态; 默认 false)
                                      # 任一为 true 时 TUI 输入框右侧展示 [+ 📎︎︎ 附件] 按钮;
                                      # 文件选择弹窗按此过滤可选类型 (图片 png/jpg/jpeg/webp/gif/bmp
                                      # ≤10MB; 音频 wav/mp3/ogg/m4a/aac/flac ≤25MB;
                                      # 视频 mp4/mov/webm/mkv ≤50MB; 单次消息附件 ≤5)。
                                      # 客户端读取并 Base64 编码为 RFC 2397 Data URL 传输;
                                      # SQLite 落库剥离 dataUrl 仅留元数据; 上下文压缩时旧附件
                                      # 降级为 [用户附带了图片/音频/视频] 纯文本标签
      model_context_max_token: 128000
      extra_headers:              # 额外 HTTP 请求头 (如自定义鉴权/网关透传)
        x-custom-header: "value"
      extra_api_config:           # 合并到请求 body 的扩展配置
        temperature: 0.7
      # 输出 token 上限自动发送 params.max_tokens: 普通模型发送 max_tokens,
      # 新模型 (o1/o3/o4/gpt-5 等) 自动切换为 max_completion_tokens 字段

  # 各模式/用途使用的模型名 (对应上面 list 中的 name; 原顶层 `use_model` 段)
  use:
    default: "my-model"           # 主模型
    subagent: "my-model"          # 子代理模型 (未指定时用主模型)
    web_search: ""                # 模型搜索 (空则用传统搜索)
    acp: "my-model"               # ACP 服务模式模型
    train: "my-model"             # 训练模型
    train_scorer: "my-model"      # 训练评分模型
    train_optimizer: "my-model"   # 训练优化模型

mcp:
  list:
    - namespace: "my_mcp"
      url: "http://localhost:3000/mcp"

# 统一数据根目录 (留空/不配置 = 不持久化: 设置/会话/codegraph 仅存内存,
# 重启后无法恢复; 支持 ~ 与 ${VAR} 展开, 相对路径按工作目录解析)
# 特殊关键字 `default` (仅 tui/cli 模式): 使用当前系统数据目录
#   - Linux/macOS: ~/.agentxx/
#   - Windows: %APPDATA%/agentxx/
# 配置后数据子路径:
#   - {data_dir}/sqlite/global.db                     全局设置 (TUI 设置等)
#   - {data_dir}/sqlite/sessions/{sessionId}/          会话数据 (session.db, 含 store 表)
#   - {data_dir}/sqlite/codegraph/<折叠路径>/index.db CodeGraph 索引
# data_dir: ~/.agentxx

# 会话工作目录 (留空/不配置 = 使用进程当前工作目录, 即启动目录)
# - 支持 ~ 与 ${VAR} 展开, 相对路径按程序工作目录解析
# - 生效范围: permission.mode=ask 默认放行范围 / filesystem 工具与权限校验的
#   相对路径解析基准 / 命令执行子进程初始目录 / 插件 projectRoot (get_config;
#   如 codegraph 未配置 paths 时默认索引该目录)
# - 用途: server 部署与嵌入多实例 (FFI 多句柄) 场景下, agent 的工作目录跟随
#   配置而非进程启动目录 (workDir 为空时行为与旧版完全一致)
# work_dir: ${AGENTXX_WORK_DIR}

# 技能目录列表 (SKILL.md 渐进式发现与加载; 相对路径按工作目录解析)
skill:
  list:
    - "./skills"

# 上下文文件列表 (Memory; 每次模型调用时内容注入系统提示词)
memory:
  list:
    - "./AGENT.md"

# 子代理委派开关 (默认 true; false 时 subagent 管理中间件不注入
# `agentxx_subagent` 工具, 模型无法发起子代理委派; 事件总线服务
# service.subagent.execute 仍注册, 程序内部路径如上下文压缩不受影响)
subagent:
  enable: true

# git worktree 模式 (默认 false)
# - 开启后注册 `agentxx_git_worktree` 工具 + 每轮注入 worktree 行为提示词:
#   提示模型在涉及代码修改的任务开始时创建独立 worktree 并绑定会话,
#   实现同仓库多会话并行开发互不影响 (详见下方 "Git Worktree 模式")
worktree:
  enable: false

# 插件配置 (列表段 `plugin.list`; 所有插件统一经 path 外置指定加载, 不区分内置/外置;
# 相对路径按程序工作目录解析为绝对路径; 编译产物位于 exec/plugins/<插件名>/;
# CodeGraph 即由此加载: 需编译启用 AGENTXX_ENABLE_PLUGIN_CODEGRAPH)
# - 段结构: `overwrite` 合并策略 (merge 默认 / replace) + `remove` 按 path 剔除继承项
plugin:
  list:
    - path: "./plugins/agentxx_codegraph"  # 插件动态库路径 或 插件目录 (含 plugin.yaml 时按清单分派)
      enabled: true                        # 默认 true
      sides: auto                          # auto|agent|client (双端插件用; 默认 auto 按导出符号自动决定)
      args:                                # 插件参数 (宿主原样保存并整体传递, 字段语义由插件定义)
        # ---- agentxx_codegraph 参数 ----
        paths:                             # 加载(索引)路径列表 (可选, 可多个目录)
          - "/path/to/proj_a"
        ignore_paths:                      # 忽略路径列表 (支持 * 通配符; 命中即跳过)
          - "**/third_party/**"
        load_cwd: true                     # 未配置 paths 时默认加载当前工作目录
        use_gitignore: true                # 默认忽略 .gitignore 规则/.gitmodules 子模块/.git
                                           # 索引库: {data_dir}/sqlite/codegraph/<折叠路径>/index.db
                                           # (data_dir 未配置时不落盘、插件自动跳过)

# 权限询问处理模式 (默认 ask, 见 PermissionMode)
# - ask:     当前工作目录内允许读写, 其他路径询问用户 (默认)
# - all_ask: 所有路径读写均询问用户
# - pass:    全部放行, 不询问
# - deny:    全部拒绝, 不询问
permission:
  mode: ask
  whitelist:      # 始终放行路径 (最长前缀匹配, 支持 * 通配; 优先级高于模式默认规则)
    list: []
  blacklist:      # 始终拒绝路径 (与白名单同路径时黑名单优先)
    list: []
```

> **Codex (Responses API) 配置示例**:
> ```yaml
> model:
>   list:
>     - name: "openai-responses"
>       type: "openai-responses"          # 使用 OpenAI Responses API (/responses)
>       base_url: "https://api.openai.com"  # 或 ChatGPT Codex 兼容网关
>       api_key: "${CODEX_API_KEY}"
>       model_name: "gpt-5-codex"
>       extra_api_config:                 # 可选覆盖: 推理强度 / 是否落盘等
>         reasoning:
>           effort: "high"
> ```

环境变量加载优先级: `程序内置变量` > `--env 覆盖文件` > `.env 文件` > `系统环境变量` > 保留 `${VAR}` 原样。
程序内置变量 (main 启动时注入, 供 yaml `${VAR}` 展开使用):
- `${AGENTXX_WORK_DIR}`: 程序启动后的工作目录 (正斜杠格式)
- `${AGENTXX_EXEC_DIR}`: agentxx_cli 可执行程序所在目录 (正斜杠格式)

**分层配置 (base + overlay)**:

agentxx_cli 启动时按两层加载配置, 用于"全局默认配置 + 项目局部覆盖":

| 层 | 配置文件 | .env |
|----|----------|------|
| overlay (上层) | `--config` 指定 (默认 `{工作目录}/agentxx-config.yaml`) | `{工作目录}/.env`, 其次 overlay 配置所在目录/.env |
| base (底层) | `{data_dir}/agentxx-config.yaml` | `{data_dir}/.env` |

- base 目录取自 overlay 的 `data_dir` (支持 `~` / `${VAR}` / 相对路径 / `default` 关键字);
  overlay 未配置 `data_dir` 或 overlay 配置不存在时取系统数据目录
  (Linux/macOS: `~/.agentxx/`, Windows: `%APPDATA%/agentxx/`) —— 即工作目录无配置时
  直接加载数据目录下的配置
- 只加载一层 base (base 内的 `data_dir` 不再向下查找)
- base 与 overlay 指向同一文件 (如 `--config <data_dir>/agentxx-config.yaml`) 时只加载一次,
  不会因"同一配置当成两层"而出现重复条目
- base 配置缺失/解析失败仅告警跳过 (不影响 overlay 启动); overlay 配置解析失败则报错退出
- 定位 base 用的 `data_dir` 仅由 overlay 的环境变量展开 (base 的 `.env` 此时尚未加载);
  两层合并后的最终 `data_dir` 用合并后的环境变量解析

**列表段结构** (`model` / `plugin` / `mcp` / `skill` / `memory` /
`permission.whitelist` / `permission.blacklist`):

```yaml
model:
    overwrite:                 # 与 base 的合并策略 (可省略; 默认 mode: merge)
        mode: merge            # merge(默认: 继承并叠加 base) | replace(整段只用本层)
        remove:                # 可选: 从合并结果中剔除的条目 (按身份匹配)
            - old-model
    list:                      # 本层条目 (可省略 = 空列表)
        - name: my-model
          type: openai
    use:                       # model 段专属: 各用途使用的模型名 (原顶层 use_model)
        default: my-model
plugin:
    overwrite: {mode: merge, remove: [agentxx_codegraph]}
    list:
        - path: builtin://agentxx_filesystem
skill:
    overwrite: {mode: replace}  # 只用本层技能列表 (不写 list = 清空继承)
    list: [./skills]
permission:
    mode: ask
    whitelist:
        overwrite: {mode: merge, remove: [/home/other]}
        list: [/workspace]
```

- 段值必须是映射; 旧写法直接给列表 (`skill: [a, b]`) 或空字符串 (`skill: ""`)
  已不再支持, 会记警告并忽略该段
- `remove` 匹配身份: `model` 按 `name`; `plugin` 按 `path`
  (`name` 与 `builtin://<name>` 写法也可匹配); `mcp` 按 `namespace`;
  `skill` / `memory` / 权限名单按字符串本身 (按原始文本比较, 不展开 `${VAR}`)
- `remove` 在两种模式下都生效 (从最终结果中剔除, 含本层 `list` 中的条目);
  未匹配到任何条目时记警告
- 策略只由 overlay 层生效 (base 作为底层没有继承对象, 其 `overwrite` 忽略)
- 清空 base 的某个列表段: `overwrite: {mode: replace}` + 不写 `list`

**其他配置段的合并规则** (base 为底, overlay 覆盖; 仅 overlay 出现的键才覆盖 base):

| yaml 键 | 规则 |
|---------|------|
| 标量 (`data_dir` / `work_dir` / `subagent.enable` / `worktree.enable` / `permission.mode` 等) | overlay 覆盖 |
| 映射 (`permission` 其余键 / `subagent` / `worktree`; 模型条目的 `extra_headers`、`extra_api_config`; 插件条目的 `args`) | 逐键递归合并, 同键 overlay 覆盖; 映射内的列表整体覆盖 |
| 列表段 (见上方段结构) | 按 `overwrite` 策略: 默认按键归并 (身份同 `remove` 匹配规则) 或追加去重; `mode: replace` 则整段不继承 |
| 其他列表 | overlay 整体覆盖 |
| 显式空值 (`key:` 无内容 = null) | 视为该层未配置, base 项保留 |

`.env` 变量: 同名变量舍弃 base 值 (overlay 优先), 查找顺序扩展为
`程序内置变量` > `--env 覆盖文件` > `overlay .env` > `base .env` > `系统环境变量`;
合并后的变量同时用于展开**两层** yaml 的 `${VAR}` (即 base 配置可引用项目 `.env` 中的密钥)。

示例 (全局配置放数据目录, 项目配置放项目目录):

```yaml
# ~/.agentxx/agentxx-config.yaml (base: 全局模型/插件/技能)
data_dir: default
model:
    list:
        - name: gpt-5
          type: openai-responses
          base_url: https://api.example.com
          api_key: ${LLM_API_KEY}      # 密钥放 ~/.agentxx/.env
          model_name: gpt-5
    use:
        default: gpt-5
skill:
    list:
        - ~/.agentxx/skills
plugin:
    list:
        - path: builtin://agentxx_codegraph
        - path: builtin://agentxx_system_monitor
```

```yaml
# {项目}/agentxx-config.yaml (overlay: 仅写项目差异)
data_dir: default                # 用于定位 base (~/.agentxx/)
skill:
    overwrite:
        mode: merge              # 默认值, 可省略
        remove: [~/.agentxx/skills]  # 本项目不加载全局技能
    list:
        - ./skills
permission:
    mode: pass                   # 覆盖 base 的 permission.mode
model:
    list:
        - name: gpt-5
          model_name: gpt-5.1    # 同名模型: 只覆盖该字段, base_url/api_key 保留
plugin:
    overwrite:
        remove: [agentxx_codegraph]  # 本项目不需要 base 的代码索引插件
    list:
        - path: builtin://agentxx_planning
```

启动日志会输出实际生效的两层路径与 `.env` 合并统计:

```
[Config] Loaded layered config: base=~/.agentxx/agentxx-config.yaml + overlay=agentxx-config.yaml (overlay overrides base)
[Config] Loaded 2 variables from .env (base .env: 2, dropped by overlay: 1)
```

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

    // 单轮对话 (非流式, 返回完整输出文本)
    auto result = co_await agent.runSingleInputAsync("session_1", "Hello!");

    // 会话执行一轮对话 (流式增量经 io 端点推送; io 传 nullptr 为 headless 模式)
    auto turn1 = co_await agent.runTurnAsync("session_1", "Hi", io);
    auto turn2 = co_await agent.runTurnAsync("session_1", "Tell me more", io);

    // 自定义消息调用 (可带 system prompt, 返回完整输出)
    std::vector<neograph::ChatMessage> msgs = {
        {.role = "system", .content = "You are helpful."},
        {.role = "user", .content = "Hello"},
    };
    auto output = co_await agent.runOverMsgsTurnAsync("session_2", msgs);
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
│  │  │  SubagentManager → Summarization → Permission      │   │    │
│  │  │  → Skill → MemoryFile → LogPrint                   │   │    │
│  │  │  (planning/worktree 为插件+工具形态, 非中间件)       │   │    │
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
    → SessionServerAgentIO.run() → BaseAgent.runTurnAsync()
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
> 的直连模式; `runTurnAsync(io=nullptr)` 的 headless 场景除外
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
- 中间件按会话 (sessionId) 维护独立 State
- 实际注册栈 (按 `handles` 压入顺序; BaseAgent 先压 SubagentManager/Summarization/Permission,
  CodeAgent 再压 Skill/MemoryFile/LogPrint): SubagentManager → Summarization → Permission
  → Skill → MemoryFile → LogPrint。planning (`agentxx_planning` 插件, 经 prompt 接口表
  注入 `appendSystemPrompts[planning]`) 与 worktree (CodeAgent 初始化期静态追加 system
  prompt + `agentxx_git_worktree` 工具) 均为插件/工具形态, 不再是独立中间件
  (历史 `PlanningMiddleware` / `WorktreeMiddleware` 已移除)

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
- **插件事件驱动取消 (`CancelRegistry`)**: 针对跨进程命令执行 (如 `agentxx_execute_command` 的 bash/PowerShell 子进程) 与长耗时插件任务，单纯休眠轮询无法及时打断阻塞中的子进程。框架在 `plugin_kit.h` 提供了实例级 `CancelRegistry` 机制：
  - 任务启动时通过 `registerCallback(sessionKey, cb)` 注册取消动作（如向子进程组发送 `SIGKILL` / `TerminateProcess` 并关闭输出管道），获取 `ScopedRegistration` RAII 守卫；
  - 宿主触发取消或重置时，即时调用已注册的取消动作，内部通过排他互斥锁与生命周期状态标记保证即使任务正在退出也不会发生回调访问已悬挂对象的竞争；
  - 若注册时会话已处于取消态，立即在锁外同步执行取消回调，避免错过时机；新轮次开始时由 `PluginBase` 自动监听 `plugin.agentxx.round_start` 清空会话取消标记。

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
    ├── requestAppendComponentInfo() [client] → 拉取启动信息 (Plugin/MCP/Skill/Memory)
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
    ├── handleInterrupt()  → 在消息列表内联渲染中断表单并等待提交/取消
    │                        (TUI: Role::Interrupt 消息 + InterruptView; CLI: 逐项问答)
    └── onPeerMessage()    → 覆写: 额外处理 InterruptRequest/Log/ModelInfo 等

AgentIOBase (服务端端点: SessionServerAgentIO)
    ├── sendToPeer()       → 覆写: 新产出的 Delta (seq 单调守卫) 先写入重放缓冲再转发,
    │                          重放 delta (seq <= 缓冲尾) 不重复入缓冲
    ├── onDelta/onSync     → protected 空实现 (server 不会从 client 收到, 满足纯虚契约)
    ├── getInput()         → 从 inputChannel_ 等待客户端输入
    ├── handleInterrupt()  → 发送 InterruptRequest，等待客户端响应 (超时/过期通知)
    ├── onPeerMessage()    → 覆写: 处理 Hello/UserInput/Cancel/SelectModel/InterruptResponse/
    │                          GetModel/GetAppendComponentInfo/GetContext/ListSessions/
    │                          SwitchSession/GetViewMessages/ClearMessageQueue/
    │                          RemoveQueueItem/InterruptAndRunNext/PluginDataUp 等
    ├── run()              → 驱动循环: 取输入 → 执行轮次 → 推送结果
    ├── stop()             → 停止驱动循环 (关闭输入 channel/取消轮次/fail pending)
    ├── onDisconnect()     → 传输断开时启动 grace 定时器 (宽限期满且无连接则取消轮次)
    ├── handleGetViewMessages() → 历史分页请求: 按绝对下标切片 [before-count, before)
    │                              回应 WireViewMessagesPage (append-only 下标恒定无竞态;
    │                              会话不匹配回空页, count=0 用默认页大小 100)
    └── switchSession()    → 会话切换: 重绑定 sessionId, 清空 delta 缓冲, 重置 firstTurn_,
                             回推新会话 Sync (按 initialSyncTailCount 尾窗分页) + 模型信息 + 上下文统计

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
transport 细节。`runTurnAsync(io=nullptr)` 为 headless 模式, 不产出事件。

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
                │     │                   + emitDelta(TextToken/ThinkToken,
                │     │                   切换 chunk 类型时附带节点内计时)
                │     ├── CHANNEL_WRITE → handleChannelWrite:
                │     │                   - "message_tip" 通道 → Delta::MessageTip
                │     │                   - "messages" 通道:
                │     │                     assistant(tool_calls) → appendViewMessage
                │     │                       + Delta::ToolStart 流
                │     │                     tool → appendViewMessage + 回填更新
                │     │                       (edit 工具附带 diff 渲染字段) + Delta::ToolEnd 流
                │     │                     assistant → appendViewMessage
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
| `service.subagent` | ReqSubagentBatch / RespSubagentBatch | RR | Subagent 委派 (统一批量, 单任务 = 1 个 task) |
| `service.crossagent` | ReqCrossAgent / RespCrossAgent | RR | 跨 agent 查询 |

宿主总线 (HostBus, AgentHost 持有, 跨 agent 路由):

| Topic | 类型 | 说明 |
|---|---|---|
| `agent.spawn` | ReqHostSpawn / RespHostSpawn | RR | 派生子代理 (宿主强制深度/并发预算) |
| `agent.message` | ReqHostMessage / RespHostMessage | RR | 任意→任意 agent 消息 (mailbox / A2A 桥接) |
| `agent.progress` | EventHostProgress | 单向 | 任意 agent 进度事件 |
| `agent.done` | EventHostDone | 单向 | agent 运行结束 (宿主据此回收 AgentNode) |

#### 5. 会话隔离与无锁设计

```
AgentHost (进程级宿主)
    ├── ioCtx                (共享 io_context, 多 agent 单线程/多协程交错)
    ├── blockingPool         (共享阻塞操作线程池, 避免每 agent 一份)
    ├── hostBus              (宿主总线: agent.spawn/message/progress/done)
    └── AgentRegistry        (AgentNode 注册表: 主 agent 与子代理平等)
         ├── "root" → AgentNode (主 agent, 独立 BaseAgent)
         └── "agent_N" → AgentNode (子代理, 独立 BaseAgent)

AgentContext
    ├── agentConfig          (全局共享配置)
    ├── middlewareHandleContext (中间件句柄)
    ├── bus                  (全局事件总线)
    ├── modelRegistry        (模型注册表)
    ├── host                 (宿主引用, attachRoot/派生时注入)
    └── sessions (SessionsManager)
         ├── "session_1" → Session
         │     ├── io                    (AgentIOBase)
         │     ├── bus                   (会话级事件总线)
         │     ├── contextStats          (std::atomic 字段, 跨线程安全)
         │     ├── activity              (Activity)
         │     ├── viewMessages + chainHash (仅 io 线程读写, client 经 Wire 拷贝传输)
         │     ├── deltaSeq              (普通 uint64_t, 仅 io 线程递增; EventBridge 分配)
         │     ├── cancelToken           (仅 io 线程读写)
         │     └── modelName             (仅 io 线程读写, 经 Wire 切换)
         └── "session_2" → Session
               └── ...

线程安全策略:
  - io 线程: 读写 viewMessages/llmMessages/chainHash/deltaSeq (assertIoThread 强制校验)
  - client/UI: 不直接读取, 由 io 线程拷贝后经 Wire 消息 (Sync/Delta) 传输
  - 取消/切模型: 经 Wire 消息发往 agent 线程处理
  - SessionsManager: 仅在 agent io_context 线程访问, 无需锁
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
  │                     → recv() 处理; 断线后进入自动重连循环:
  │                       重连 → 重建 writeQueue → 发送 Hello(lastSeq, tailHash)
  │                       → 服务端增量重放 (seq 不连续时回退全量 Sync)
  ├── heartbeatLoop(): 每 heartbeatInterval 发送 Ping
  └── Delta 去重: 收到 delta 时更新 lastDeltaSeq_, 重放重复投递的
      seq <= last 直接丢弃, 避免 UI 重复渲染
      收到 Sync 时按 SyncPayload.deltaSeq **覆盖**水位 (快照已含
      seq <= deltaSeq 的全部增量): 服务端重启/会话重建后 seq 从 0 重新
      计数, 若保留旧水位 (如 100) 会把新增量全部判为重复而丢弃 ——
      表现为重连成功、历史快照也拿到了, 但界面再也不刷新
```

- 写/读队列均为有界 concurrent_channel, `try_send` 失败即丢弃; 队列关闭使挂起的 async_receive 抛异常, 循环自然退出
- HelloAck 在 connect() 握手阶段被处理, 不进入 runTransportLoop 的消息流
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
  │                                    │ runTurnAsync()
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
  │←── HelloAck + Delta replay ───────│ seq 不连续时回退全量 Sync;
  │                                    │ 客户端水位 > 服务端当前 seq
  │                                    │ (进程重启) 同样回退全量 Sync,
  │                                    │ 客户端按 Sync.deltaSeq 复位水位
  │                                    │
  │──── Cancel ──────────────────────→│ 取消当前轮次
  │                                    │
  │──── SelectModel (model) ─────────→│ 切换会话模型
  │                                    │
  │──── GetModel ────────────────────→│ 查询当前模型信息
  │←── ModelInfo ─────────────────────│
  │                                    │
  │──── GetAppendComponentInfo ──────→│ 查询 Plugin/MCP/Skill/Memory 组件加载信息
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
  │ 无上行权限规则消息: "记住本次选择" 随    │
  │ 中断结果 values.remember 回传,         │
  │ 规则由服务端权限处理器注册              │
  │                                    │
  │ (可选) 会话选择弹窗 (TUI F4)
  │──── ListSessions ─────────────────│ 列举持久化会话 (阻塞 I/O 卸载到线程池)
  │←── SessionList ───────────────────│
  │──── SwitchSession (sessionId) ───│ 切换会话绑定: 清空 delta 缓冲,
  │                                   │ 回推新会话 Sync (尾窗分页) + 模型信息 + 上下文统计
  │                                    │
  │ (可选) 历史分页 (TUI 向上滚动到窗口顶部)
  │──── GetViewMessages (before, n) ──│ 切片 [max(0,before-n), before) 回应
  │←── ViewMessagesPage ──────────────│ (startIndex/totalCount/messages);
  │                                   │ 客户端前插 + 滚动锚定, 视口内容稳定
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

#### 关闭顺序 (资源释放)

Agent 与插件涉及 io_context / 线程池 / 动态库, 释放顺序有隐式约定, 按以下顺序执行:

1. 停止输入与轮次: 断开客户端 (或收到退出信号后取消当前轮次), 使会话驱动循环退出;
2. `co_await agent->shutdownAsync()`: 在 agent io 线程上执行插件 stop → lease 归零
   → destroy/dlclose 全链路 (失败仅记日志: 未完成的实例保持 CloseFailed 并保留
   上下文/动态库, 越权 dlclose 更危险); 必须在 io_context 停止之前完成, 否则
   插件的 stop 事务没有可用的 io 线程;
3. `agent->ioCtx->stop()` 并 join/等待 `run()` 返回: 停止会话协程、定时器与线程池任务;
4. 释放持有者: `BaseAgent`/`AgentContext` 析构 (AgentContext 析构时会告警仍有
   未关闭的插件实例 —— 说明第 2 步未完成), 之后才释放 transport / 客户端对象。

参考实现: `agent/client/src/mode_runners.cpp` (`shutdownAgentPlugins` + 各模式的收尾)。

---

## 代码结构

```
agent/
├── lib/                          # libagentxx 核心库
│   ├── include/agentxx/
│   │   ├── agentxx.h             # 库总入口头文件
│   │   ├── ffi_api.h             # FFI 纯 C ABI 导出契约 (唯一跨语言稳定接口, 见 ffi.md)
│   │   ├── agent/                # Agent 核心
│   │   │   ├── base_agent.h      # BaseAgent 基类 (核心基础设施 + ReAct 循环 + 会话执行)
│   │   │   ├── code_agent.h      # CodeAgent (继承 BaseAgent, 编程工具/中间件)
│   │   │   ├── agent_host.h      # AgentHost 进程级宿主 (主 agent 与子代理平等注册/派生/回收)
│   │   │   │                     #   AgentNode / AgentRegistry / spawnBatch / HostBus / A2A 桥接
│   │   │   ├── agent_runner.h    # AgentRunner 统一 "引擎运行+中断处理+恢复" 循环 (主 agent 与子代理共用)
│   │   │   ├── config.h          # AgentConfig / ModelConfig 配置
│   │   │   ├── config_static.h   # 静态路径配置
│   │   │   ├── context.h         # AgentContext / Session / SessionsManager / ContextStats
│   │   │   │                     #   Session: 线程绑定 (viewMessages/chainHash 单线程读写)
│   │   │   ├── checkpoint_store.h # 单检查点存储: SingleCheckpointStore 策略基类 +
│   │   │   │                     #   InMemorySingleCheckpointStore (每 thread 仅最新,
│   │   │   │                     #   save 时自动淘汰历史, O(super-steps) -> O(threads))
│   │   │   ├── conversation_types.h # Delta(含NodeStart/End/seq/timing) / SyncPayload
│   │   │   │                     #   ViewMessage (UI 展示消息, role 拆分子结构) /
│   │   │   │                     #   ChainHash / AppendComponentNotification
│   │   │   ├── model_registry.h  # ModelProviderRegistry (运行时模型切换)
│   │   │   ├── session_store.h   # 会话 SQLite 持久化: 单库 session.db
│   │   │   │                     #   (view_message/llm_context/meta/store 四表, 含 share store KV),
│   │   │   │                     #   按 sessionId 分目录, 读取路径不创建目录
│   │   │   ├── prompt.h          # AgentPrompt / ToolPrompt 提示词管理
│   │   │   ├── training.h        # EvolutionTrainingAgent 进化训练 (变异/评估/优化/收敛检测)
│   │   │   └── io/               # 远程通信
│   │   │       ├── agent_server.h    # AgentServer (WS 服务, token 鉴权; serveTransport 供进程内复用)
│   │   │       ├── session_server_agent_io.h # SessionServerAgentIO (会话驱动, delta 缓冲/重放, grace,
│   │   │       │                          #   服务端消息队列, 插件事件转发 WirePluginData)
│   │   │       ├── wire_protocol.h   # Wire Protocol 消息类型与序列化
│   │   │       ├── agent_io.h        # AgentIOBase 端点基类 (client/server 操作契约)
│   │   │       ├── client_event_sink.h # 客户端事件接收器 (向 client 插件系统转发端点事件)
│   │   │       ├── agent_io_transport.h # 传输层抽象基类 (connect/recv/send/close/alive)
│   │   │       ├── channel_io_transport.h # 进程内 Channel 传输 (零序列化)
│   │   │       └── ws_io_transport.h  # WebSocket 传输 (JSON 编解码/心跳/重连)
│   │   ├── deps/                 # 依赖注入
│   │   │   └── injector.h        # DependencyContainer (工厂/单例/有名称注册)
│   │   ├── event/                # 强类型事件系统
│   │   │   ├── events.h          # 事件类型定义 (Topic 命名空间 / Event structs)
│   │   │   ├── event_stream.h    # EventBus / EventStream / RequestResponseStream
│   │   │   └── event_host.h      # HostBus 事件类型 (agent.spawn/message/progress/done)
│   │   ├── nodes/                # Graph 节点
│   │   │   ├── wrap_handle.h     # WrapHandleBaseNode 栈式中间件基类
│   │   │   ├── modelcall.h       # ModelCallWrapNode (LLM 调用, 动态模型切换)
│   │   │   ├── toolcall.h        # ToolcallWrapNode (工具分发, 自动压缩)
│   │   │   └── agentcall.h       # AgentStart/EndCallWrapNode (会话生命周期)
│   │   ├── plugin/               # 插件系统 (热插拔原生 C++ 插件, 纯 C ABI, API v1 —— 冻结核心 vtable + 17 张 agent 接口表 + 7 张 client 接口表)
│   │   │   ├── api/              # 插件 API 头 (插件/宿主共用 C ABI 契约 + 插件 SDK; 宿主侧引用也走 api/ 前缀)
│   │   │   │   ├── plugin_api.h      # 纯 C ABI 契约 (唯一跨版本稳定接口, 见 docs/zh-cn/plugins.md) — 核心 vtable 冻结 + COM QueryInterface
│   │   │   │   ├── client_plugin_api.h # client 侧插件纯 C ABI 契约 (UI 无关语义层)
│   │   │   │   ├── plugin_kit.h      # C++ SDK header-only (PluginBase/Task/awaiters/tool/hook/capability/spawn, 命名空间 agentxx::plugin; 含原 plugin_iface_helper.h 接口表聚合与同步工具适配器)
│   │   │   │   └── plugin_guard.h    # 插件 C ABI 边界异常处理 header-only (命名空间 agentxx::plugin)
│   │   │   ├── op_driver.h       # 异步操作驱动 (AgentxxOpNotify Done 协议)
│   │   │   ├── plugin_manager.h  # PluginManager 生命周期 (load/enable/disable/unload) /
│   │   │   │                     #   PluginTool (C 回调→线程池卸载执行) /
│   │   │   │                     #   PluginMiddlewareHandle (7 钩子→C 回调) /
│   │   │   │                     #   CapabilityRegistry / NativeLoader (dlopen↔LoadLibraryW)
│   │   │   ├── plugin_manager_base.h # 插件管理器公共基类 (agent/client 共用: 实例基类/io 投递/等待/内存三件套)
│   │   │   ├── plugin_common.h   # 插件宿主侧通用工具
│   │   │   ├── client_plugin_manager.h # ClientPluginManager (client 侧加载/UI 注册表/命令管线)
│   │   │   └── tool_registry.h   # 动态插件工具查表 (shared_ptr 保活, 静态工具名冲突检测)
│   │   ├── middlewares/          # 中间件
│   │   │   ├── middleware.h      # BaseMiddlewareHandle / MiddlewareContext / State 基类
│   │   │   ├── permission.h      # PermissionMiddleware (工具权限 HIL)
│   │   │   ├── skill.h           # SkillMiddleware (技能发现与加载)
│   │   │   ├── memory_file.h     # MemoryFileMiddleware (上下文文件注入)
│   │   │   ├── subagent_manager.h # SubagentManagerMiddleware (子代理委派管理, 持有 agentxx_subagent)
│   │   │   └── summarization.h   # SummarizationMiddleware (上下文压缩)
│   │   ├── tools/                # 工具 (lib 内仅保留 share_store / subagent / tool_skill_search 模板 / git_worktree;
│   │   │                         #   文件系统/命令/网络/知识检索/字符串/系统时间/规划等已拆分为 agent/plugins 下独立插件)
│   │   │   ├── tool.h            # XXToolBase / XXToolWrap 工具基类
│   │   │   ├── share_store.h     # 会话级文本寄存 (agentxx_share_store)
│   │   │   ├── subagent.h        # 子代理管理工具 (agentxx_subagent)
│   │   │   ├── tool_skill_search.h # 工具/技能延迟加载搜索子代理任务 (逻辑已内联, 当前未独立注册为 tool)
│   │   │   └── git_worktree.h    # Git Worktree 工具 (create/info/status/remove, yaml worktree.enable 开启)
│   │   ├── protocol/             # 协议实现
│   │   │   ├── openai_provider.h  # OpenAI Chat Completions API (流式/非流式/SSE)
│   │   │   ├── anthropic_provider.h # Anthropic Messages API (thinking/tool_use)
│   │   │   ├── mcp_client.h      # MCP Client (HTTP SSE + stdio, 多版本协商)
│   │   │   ├── mcp_server.h      # MCP Server (HTTP + stdio, tool/resource/prompt)
│   │   │   ├── a2a_client.h      # A2A Client (Agent Card / SendMessage / Task 管理)
│   │   │   ├── a2a_server.h      # A2A Server (JSON-RPC, 任务状态机)
│   │   │   ├── acp_server.h      # ACP Server (stdio 模式)
│   │   │   ├── provider_common.h # 各 LLM Provider 与模型调用节点共用 helper
│   │   │   │                     #   (唯一 tool_call id 生成 / 空响应判定)
│   │   │   └── protocol_base.h   # 协议基类
│   │   └── util/                 # 工具类
│   │       ├── log.h             # 日志系统 (XX_LOG 宏, LogDispatcher, LogSink)
│   │       ├── string_util.h     # 字符串工具 (编码转换/路径标准化/base64/自然排序/IgnoreCaseMap 等)
│   │       ├── path_sanitize.h   # 路径段安全化 (非法字符替换/超长截断+哈希尾缀/
│   │       │                     #   Windows 保留设备名判定; 会话与索引目录名构造共用)
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
│   │       ├── worktree.h        # Git worktree 封装 (argv 直调/超时整组终止)
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
│   │   │   │   ├── cli_plugin_adapter.h # CLI 插件适配器 (命令管线/client 事件转发)
│   │   │   │   └── stdin_reader.h # 异步 stdin 读取器
│   │   │   └── tui/
│   │   │       ├── agent_tui.h   # TUIClientAgentIO (FTXUI 终端 UI, 接收/显示/排队/权限/日志)
│   │   │       ├── tui_plugin_adapter.h # TUI 插件适配器 (UI 注册表/命令管线/client 事件转发)
│   │   │       ├── scrollable.h  # Scrollable (全量构建的可滚动容器, 侧边栏等短列表用)
│   │   │       ├── lazy_scrollable.h # LazyScrollable (懒构建+LRU有界缓存+视口局部渲染)
│   │   │       ├── scroll_common.h # 两个滚动容器共用逻辑 (元素布局测量/滚轮事件)
│   │   │       ├── tui_theme.h   # TUI 主题配色 (含弹窗 surface* 面性风格配色)
│   │   │       ├── surface.h     # 弹窗面性风格外框 (+ 角标 + 内外留白 + 三区域组装)
│   │   │       ├── framework/    # TUI 框架层
│   │   │       │   ├── tui_state.h       # TUI 状态聚合 (消息/侧边栏/排队输入等)
│   │   │       │   ├── tui_context.h     # TUI 渲染上下文 (theme/state/尺寸)
│   │   │       │   ├── tui_settings.h    # TUI 全局设置单例 (主题/动画/日志等级)
│   │   │       │   ├── modal_container.h # 弹窗容器 (权限/中断弹窗)
│   │   │       │   └── tui_i18n.h       # 界面翻译表 (en/zh 两列, 缺键回退)
│   │   │       ├── text_layout.h # 文本布局辅助 (行数估算/硬折行; 消息列表与中断视图共用)
│   │   │       └── components/   # TUI 渲染组件
│   │   │           ├── message_list.h # 消息列表渲染
│   │   │           ├── interrupt_view.h # 中断输入项通用视图 (渲染/估算/交互; 形态由 UI 描述数据决定)
│   │   │           ├── sidebar.h      # 右侧边栏 (日志/信息/Planning)
│   │   │           ├── overlays.h     # 弹窗 (权限/中断/模型选择)
│   │   │           ├── input_bar.h    # 输入栏
│   │   │           ├── status_bar.h   # 状态栏 (上下文占用/活动状态)
│   │   │           └── spinner.h      # 加载动画
│   │   ├── train/                # 训练模式 (train.h: EvolutionTrainingAgent 装配/用例加载/进化循环入口)
│   │   └── util/                 # 客户端工具 (util.h: 通用小工具)
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
│       │                                   #   overlays / input_bar / status_bar / spinner
│       ├── train/train.cpp        # 训练实现
│       └── util/util.cpp          # 客户端工具实现
│
├── test/                         # agentxx_test 测试程序
│   ├── test.cpp                  # 测试入口: 模块注册与调度 (同步/异步模块分组)
│   ├── core/                     # lib (agentxx 核心) 测试实现 (*.cpp)
│   ├── plugin/                   # 插件测试实现 (*.cpp; dso_plugins/ 与 negative_compile/ 下为独立编译的用例源码)
│   ├── client/                   # client 侧测试实现 (*.cpp; 仅 AGENTXX_BUILD_CLIENT 编译)
│   └── include/agentxx-test/     # 测试头文件统一入口 (源码一律以 "agentxx-test/<组>/<模块>.h" 前缀引用, 与源码目录分离避免重名歧义)
│       ├── test_framework.h                   # 测试框架 (断言宏 / TestResult)
│       ├── test_toolcall_args.h               # 工具参数类型自动修正测试
│       ├── test_ffi_c_api.h                   # FFI C API 测试 (生命周期/交互/HIL/事件队列)
│       ├── core/                              # lib (agentxx 核心) 测试头 (与 core/*.cpp 同名模块)
│       │   ├── test_agent.h                   # CodeAgent 集成测试 (模拟 LLM Server: 工具调用/多轮/权限模式/重试耗尽/异常拦截)
│       │   │                                  #   test_agent.h 同时提供共享 LLM 模拟器 (DaSimServer),
│       │   │                                  #   被 agent_host/session_persistence/remote_agent/cancel/memgrowth 等复用
│       │   ├── test_agent_host.h              # AgentHost 宿主测试 (子代理派生/深度并发预算/回收)
│       │   ├── test_training.h                # 进化训练测试 (变异/评估/优化/收敛/持久化)
│       │   ├── test_events.h                  # 事件类型测试
│       │   ├── test_event_stream.h            # EventBus / EventStream / RequestResponseStream 测试
│       │   ├── test_event_bridge.h            # EventBridge 事件翻译测试
│       │   ├── test_interrupt_bus.h           # 中断总线 HIL 测试
│       │   ├── test_subagent_bus.h            # 子代理总线测试 (含批量委派/跨 agent 路由)
│       │   ├── test_concurrency.h             # 并发测试
│       │   ├── test_cancel.h                  # 取消语义测试 (CancelToken 双通道/operation_aborted 转换)
│       │   ├── test_message_supplement.h      # 消息补全/修复测试
│       │   ├── test_summarization.h           # 上下文压缩测试 (token 统计/去重/LLM 压缩)
│       │   ├── test_checkpoint_store.h        # 单检查点存储测试 (InMemorySingleCheckpointStore)
│       │   ├── test_memgrowth.h               # 多轮内存增长测试 (泄漏检测)
│       │   ├── test_session_persistence.h     # 会话 SQLite 持久化测试 (消息/上下文/share store 落库与重启恢复)
│       │   ├── test_remote_agent.h            # 远程 Agent (WS 传输 / SessionServerAgentIO) 测试
│       │   ├── test_mcp.h                     # MCP 协议测试 (多版本/HTTP/stdio)
│       │   ├── test_a2a.h                     # A2A 协议测试
│       │   ├── test_acp.h                     # ACP 协议测试
│       │   ├── test_websocket.h               # WebSocket 测试
│       │   ├── test_http.h                    # HTTP 客户端/服务器测试
│       │   ├── test_network_timeout.h         # 网络超时行为测试
│       │   ├── test_openai_provider.h         # OpenAI Provider 测试 (SSE/thinking/tool_calls/限流)
│       │   ├── test_anthropic_provider.h      # Anthropic Provider 测试
│       │   ├── test_string_util.h             # 字符串工具测试
│       │   ├── test_regex.h                   # 正则引擎测试
│       │   ├── test_diff_util.h               # Diff 工具测试
│       │   ├── test_aho_corasick.h            # Aho-Corasick 多模式匹配测试
│       │   ├── test_util_misc.h               # 杂项 util 测试
│       │   ├── test_settings_db.h             # 全局设置 SQLite 测试
│       │   ├── test_misc_fixes.h              # 杂项修复测试
│       │   ├── test_share_store.h             # ShareStore 测试
│       │   ├── test_subagent_tool.h           # 子代理工具参数校验与恢复测试
│       │   ├── test_worktree.h                # Git worktree 封装与隔离测试
│       │   ├── test_filesystem_tools.h        # 文件系统工具测试 (直测插件同一 *_impl.h 实现)
│       │   ├── test_command_tools.h           # 命令执行工具测试 (直测插件同一实现)
│       │   ├── test_math_tools.h              # 数学计算工具测试 (直测插件同一实现)
│       │   ├── test_web_search_tools.h        # 网络搜索测试 (直测插件同一实现)
│       │   ├── test_rag_search_tools.h        # RAG 搜索测试 (直测插件同一实现)
│       │   ├── test_string_tools.h            # 字符串工具测试 (html2md/regexp, 直测插件同一实现)
│       │   └── test_datetime_tool.h           # 日期时间工具测试 (直测插件同一实现)
│       ├── plugin/                            # 插件测试头 (与 plugin/*.cpp 同名模块)
│       │   ├── test_plugins.h                 # 插件系统测试 (加载/工具/钩子/事件/热插拔, 模块名 `plugins`)
│       │   ├── test_plugin_resources.h        # 插件会话资源扩展测试 (Skill/Memory/MCP 声明式+运行时)
│       │   ├── test_plugin_multi_instance.h   # 插件多实例隔离测试 (三铁律)
│       │   ├── test_client_plugins.h          # client 侧插件测试 (内置合并编译时跳过)
│       │   ├── test_codegraph_tools.h         # CodeGraph 插件集成测试 (search/context/callers/callees/path 共 5 工具)
│       │   ├── test_cpu_gpu_use.h             # system_monitor 插件集成测试 (系统资源监控)
│       │   ├── test_screen_capture.h          # screen_capture 插件集成测试 (仅 Windows)
│       │   └── test_text_selection_monitor.h  # text_selection_monitor 插件集成测试 (仅 Windows)
│       └── client/                            # client 侧测试头 (与 client/*.cpp 同名模块)
│           ├── test_config_loader.h           # YAML 配置加载测试
│           ├── test_mermaid_state.h           # Mermaid 状态图解析测试
│           ├── test_thread_id.h               # sessionId 生成唯一性测试 (模块名 `sessionId`)
│           ├── test_tui_input.h               # TUI 输入测试
│           ├── test_tui_interrupt.h           # TUI 中断交互测试
│           ├── test_tui_scroll.h              # TUI 滚动测试
│           ├── test_tui_settings.h            # TUI 设置持久化测试
│           ├── test_tui_sidebar.h             # TUI 侧边栏内容与段落测试
│           ├── test_tui_stream.h              # TUI 流式渲染测试
│           └── test_tui_tool_header.h         # TUI 工具消息头部渲染测试
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
├── ffi/                          # 其他编程语言绑定生成配置
│   └── dart/                     # Dart FFI 绑定 (ffigen.yaml 由 ffi_api.h 生成
│                                 #   agentxx_ffi_bindings.dart; dart pub get + ffigen)
│
├── example/                      # 嵌入/绑定使用示例
│   └── ffi/dart/                 # Dart CLI 示例 (经 FFI 驱动 libagentxx:
│                                 #   流式渲染/HIL 权限与会话切换/mock LLM 冒烟检查)
│
├── plugins/                      # 插件 (独立动态库/目录, 仅依赖 plugin_api.h;
│                                 #   编译产物统一输出到 exec/plugins/<插件名>/)
│   ├── example_plugin/           # 示例 C++ 插件 (双端): 工具/钩子/事件/能力/client 入口
│   ├── example_graph_node/       # Graph 扩展示例插件 (自定义节点类型 + set_graph_json 改图, 依赖 agent.graph 接口)
│   ├── example_js/               # JS 示例插件 (C++ 壳 + plugin.js; depends: javascript_engine)
│   ├── example_js_execute_command/ # JS 代码执行工具插件 (example_js_execute_command; depends: javascript_engine)
│   ├── example_resources/        # 会话资源贡献示例 (声明式与编程式 MCP/Skill/规则)
│   ├── agentxx_javascript_engine/ # QuickJS 引擎插件 (能力 interpreter.js; 专用 JS 线程+沙箱)
│   ├── agentxx_codegraph/        # CodeGraph 代码分析插件 (search/context/callers/callees/path 共 5 工具 + client Info 栏段落)
│   ├── agentxx_filesystem/       # 文件系统 6 工具 (list/read/write/edit/glob/grep)
│   ├── agentxx_execute_command/  # 命令执行 2 工具 (bash/windows)
│   ├── agentxx_math/             # 数学计算工具 (agentxx_math_calculate)
│   ├── agentxx_websearch/        # 网络搜索 3 工具 (search/fetch/fetch_markdown)
│   ├── agentxx_rag_search/       # 向量语义搜索
│   ├── agentxx_string/           # 字符串 2 工具 (html_to_markdown/regexp)
│   ├── agentxx_system/           # 系统时间 (get_current_datetime)
│   ├── agentxx_planning/         # 规划工具 + client 侧 Plan 渲染 (类型级渲染器/实时装饰/Info 段落)
│   ├── agentxx_screen_capture/   # 屏幕捕获插件 (仅 Windows)
│   ├── agentxx_computer_use/     # 键鼠控制插件 (仅 Windows; depends: screen_capture)
│   ├── agentxx_system_monitor/   # 系统资源监控插件 (工具 + 周期采集 + client 状态栏渲染)
│   ├── agentxx_audio_stream/     # 音频流捕获插件 (全平台跳过构建: 实现未启用)
│   └── agentxx_text_selection_monitor/ # 文本选择监听插件 (仅 Windows UIAutomation)
│
└── script/                       # 编译/测试脚本
    ├── linux_debug_build.sh
    ├── linux_release_build.sh
    ├── windows_debug_build.bat
    ├── windows_release_build.bat
    ├── cross_windows_release_build.sh
    └── cross_android_release_build.sh
```

### 关键依赖关系

```
BaseAgent (基类)
  ├── GraphEngine (NeoGraph) + per-agent GraphRegistry
  │     ├── ModelCallWrapNode → OpenAIProvider / AnthropicProvider
  │     ├── ToolcallWrapNode → XXToolBase 工具集
  │     └── AgentStart/EndCallWrapNode
  ├── AgentRunner (统一中断循环, 主 agent 与子代理共用)
  ├── MiddlewareContext → 中间件栈
  ├── AgentContext
  │     ├── sessions (SessionsManager) → Session (per sessionId)
  │     │     ├── viewMessages + chainHash (仅 io 线程读写, client 经 Wire 拷贝传输)
  │     │     ├── llmMessages (io 线程读写)
  │     │     ├── cancelToken / modelName (io 线程读写)
  │     │     ├── activity / contextStats (atomic, 跨线程安全)
  │     │     ├── deltaSeq (普通 uint64_t, 仅 io 线程递增)
  │     │     └── io / bus (会话级)
  │     ├── sessions->sessionStore (SessionStore, SQLite 持久化; 可空)
  │     ├── ModelProviderRegistry
  │     └── EventBus
  └── AgentConfig → ModelConfig / AgentPrompt

CodeAgent (继承 BaseAgent)
  ├── 工具: lib 内仅 share_store/subagent/git_worktree (+延迟加载装配); 文件系统/命令/网络/
  │   RAG/字符串/系统时间/规划/CodeGraph 等全部经插件注入 (yaml plugin.list 段)
  └── 中间件: SubagentManager → Summarization → Permission → Skill → MemoryFile → LogPrint
      (planning/worktree 为插件+工具形态, 非中间件)

SessionServerAgentIO (远程会话驱动)
  ├── AgentIOBase (服务端端点)
  ├── BaseAgent.runTurnAsync()
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
---

## 附录 A: 核心数据模型 (conversation_types.h)

### ViewMessage (UI 展示消息)
- 通用字段: id (msgId, appendViewMessage 分配), role, text, startTimeMs/durationMs, collapsed
- 角色枚举 Role: User / Assistant / Think / System / Tool / Interrupt / Tip
- 角色专属 optional 子结构:
  - Tool: ToolData {toolName, toolCallId, toolResult, diff (edit diff 预留), toolFinished}
  - Think: ThinkData {reasoningTokens, isEncrypted}
  - Tip: TipData {tipLevel: Info/Warning/Error} — 系统提示与 Turn 统计经 InsertMessage 原子插入
  - Interrupt: InterruptData {interruptId, ui (声明式表单描述), interruptStatus (Waiting/Confirmed/Cancelled/Expired), interruptResult (提交值展示文本)}
- 序列化: toJson/fromJson 供 Wire Sync 与链式哈希共用; 对应 role 下保证子结构非空

### Delta (流式增量事件, 统一 seq)
- Type: TextToken / ThinkToken / ToolStart / ToolEnd / TurnStart / TurnEnd / NodeStart / NodeEnd / MessageUITip / InsertMessage / UpdateMessage
- 公共字段: seq (会话级单调递增, EventBridge 与 SessionServerAgentIO 共用 Session::nextDeltaSeq 分配), text, msgId, toolName/toolCallId/arguments/result/hasError, nodeName, think (ThinkData), tipType (MessageUITip 时), message (InsertMessage/UpdateMessage 时携带完整 ViewMessage 指针), historyCount/tailHash, startTimeMs/durationMs, tps (TurnEnd 轮级平均速度)
- MessageUITip: 瞬态提示, 仅 UI 展示不入 viewMessages; InsertMessage: 原子插入完整 ViewMessage (如 Tip/统计), 入历史并同步; UpdateMessage: 按 msgId 原地更新已插入消息 (如 tool 结果回填)

### SyncPayload / MessageQueueItem
- SyncPayload {fromIndex (窗口首条绝对下标), messages[], tailHash, totalMessages, deltaSeq, messageQueue[]}
  - 全量同步: fromIndex=0, totalMessages==messages.size()
  - 尾窗同步: fromIndex=窗口起始下标 (>0 表示上方还有更早消息), totalMessages=会话总消息数, 客户端按 WireGetViewMessages 分页拉取
  - deltaSeq: 快照水位 (= Session::deltaSeq, 即快照已包含 seq <= deltaSeq 的全部增量);
    客户端据此**覆盖**去重水位 —— 服务端进程重启/会话重建后 seq 从 0 重新计数,
    客户端若保留旧水位 (如 100) 会把之后 seq=1,2,... 的增量全部判为重复丢弃
    (表现为重连成功但界面永不刷新); 0 表示未提供 (客户端按 0 处理, 放行后续增量)
- MessageQueueItem {id, text, model (待应用模型, 空=默认), createdAtMs}
- ChainHash: FNV-1a 链式哈希, append(string) 累积, tailHex 供 Hello/Sync 校验

### Wire 协议分页语义
- ListSessions: keyset 游标 {beforeMs, beforeId, limit} 按 lastActiveMs 降序分页; 服务端回 WireSessionList {sessions[], totalCount, hasMore}
- GetViewMessages: {beforeIndex, count} 请求 [max(0,before-count), before) 区间; 服务端回 WireViewMessagesPage {startIndex, totalCount, messages[]} (append-only 下标恒定, 无竞态)
- GetContext 等同步查询: 阻塞等待服务端响应 (最长 10s, 同一句柄同一时刻仅允许一个在途)

---

## 附录 B: 插件系统 v1 要点 (详见 plugins.md)

- COM 风格接口表查询: 核心 vtable 冻结 (alloc/free + query_interface; 原 strdup 槽位已移出 vtable, 改为基于 alloc 的头文件内联 `agentxx_plugin_strdup`), 能力按 IID 字符串查询独立接口表 (首字段 version 独立演进, 当前全为 1)；加载阶段要求 API 版本 `>=` 宿主版本以保障向前兼容
- Agent 侧 16 张接口表: tools / hooks / events / capabilities / scheduler / session / plugins / config (含 get/set_language) / model / cancel / prompt / json / log / resources / graph / tasks (tasks 表 `notify` 为出参, 供宿主托管后台任务)
- Client 侧 7 张接口表: ui (v2, 含工具特化渲染器与实例装饰) / events / session / wire / self / json / log (详见 client_plugin_api.h)
- 现代 SDK (plugin_kit.h): 
  - `AGENTXX_PLUGIN_AGENT_EXPORT` / `AGENTXX_PLUGIN_CLIENT_EXPORT` 声明式导出宏, 自动包裹 C ABI 异常守卫与实例上下文生命周期
  - `ToolSchemaBuilder` 链式参数构建器，自动与宿主 `toolPrompt` 提示词覆盖融合
  - `ArgReader` 宽容类型解析与参数智能自愈
  - `CancelRegistry` 框架级事件驱动取消注册表，提供会话取消即时回调与 `ScopedRegistration` 防悬挂互斥保护
  - `PluginBase` 状态基类 + `Task<T>` 锚定协程 + `sleep/yield/offload/call_tool/invoke_cap` awaiter
  - `tool/fast_tool/blocking_tool/hook/capability/spawn` 便捷注册族
- 多实例三铁律: 禁止可变全局 static / 状态经 user_data 闭包恢复 / 接口表缓存入实例上下文
- 导出控制: -fvisibility=hidden + version script 白名单 (AGENTXX_PLUGIN_EXPORT), 单端插件兼容 Android lld
- 平台矩阵: 各插件 CMakeLists 开头经 plugin_platform_support.cmake 判定 (screen_capture/computer_use/text_selection_monitor 仅 Windows 等)
- 工具复用: 内置插件经 agentxx_util 静态库复用全部 util (各自静态链接, 符号隐藏互不冲突)

---

## 附录 C: 会话工作目录多源回退 (AgentContext::getSessionWorkDir)

回退链 (优先级从高到低):
1. Session::WorktreeBinding.path (worktree 模式已绑定)
2. AgentContext::sessionWorkDirs_[sessionId] (会话级覆写, 如 ACP session/new 携带的客户端 cwd, mutex 保护任意线程注入)
3. AgentConfig::resolvedWorkDir() (yaml work_dir / 进程 cwd)
- 调用方统一经 getSessionWorkDir(sessionId) 取值, 不直接读进程 cwd / workDir
- 失效时返回空串由调用方兜底 (如 Permission Ask 模式无 workDir 时不注册默认放行规则)

