> 文档自动翻译自[zh-cn](/docs/zh-cn/design.md/index.md)版 (This document is automatically translated from the [zh-cn](/docs/zh-cn/design.md/index.md) version.)

# Agentxx Comprehensive Architecture Design Document
> Related docs: [design.md](index.md) (Architecture) · [plugins.md](plugins.md) (Pure C ABI Plugin Paradigm) · [ffi.md](ffi.md) (FFI Interface Design)

## Table of Contents

- [Overview](#overview)
- [Features & Capabilities](#features--capabilities)
- [Usage Guide](#usage-guide)
- [Architectural Design](#architectural-design)
- [Code Structure](#code-structure)

---

## Overview

Agentxx is an AI Agent framework implemented in C++23, compiled with C++26/C17 standard options enabled. Core design objectives:

- **Cross-Platform**: Supports Linux x86_64 (including WSL extensions), Windows 10+ x86_64, and Android 5.0+.
- **Multi-Form Compilation**: Can be compiled as a standalone executable, dynamic shared library, or static library, depending only on basic system libraries.
- **High Concurrency**: Single-threaded / multi-coroutine interleaved execution across multiple sessions, requiring no thread locks.
- **Layered Decoupling**: Client handles UI rendering and user interaction; Agent (BaseAgent / CodeAgent) manages session execution, LLM API calls, and ToolCall execution.
- **Multi-Connection Modes**: Supports single-process client+agent (in-process Channel direct connection) and remote client connecting to an agent server over WebSocket.

---

## Features & Capabilities

### Core Conversational Capabilities

- **Multi-Turn Conversations**: Comprehensive multi-turn conversation management, maintaining dual message sets: `viewMessages` (append-only complete historical log) and `llmMessages` (compressible LLM context).
- **Streaming Output**: LLM responses are streamed as incremental Delta events (TextToken / ThinkToken / ToolStart / ToolEnd / TurnStart / TurnEnd / NodeStart / NodeEnd / MessageUITip / InsertMessage), with each Delta bearing a monotonically increasing `seq` for replay and synchronization. Turn statistics, errors, cancellation notifications, and interrupt headers are constructed by the agent thread as complete ViewMessages and inserted into session history via InsertMessage (carrying `msgId`), guaranteeing that `viewMessages` stay perfectly synchronized with the UI.
- **Multi-Model Support**: Dynamic per-session (`sessionId`) model switching at runtime, supporting OpenAI Chat Completions, Anthropic Messages, and OpenAI Responses (Codex) provider protocols.
- **Context Compaction**: `SummarizationMiddleware` automatically compresses historical messages when context approaches the model token limit, supporting tool call deduplication and truncation.
- **Chain-of-Thought (CoT) Display**: Streams and renders LLM reasoning/thinking content in real time.
- **Node-Level Events**: `NodeStart`/`NodeEnd` events delineate execution lifecycles of Graph nodes, making progress tracking intuitive on the UI.

### Tool Invocations (ToolCall)

Rich suite of tools organized by functional categories. Core programming utilities (filesystem, command execution, web search, RAG retrieval, strings, system time, planning writes) have been refactored from library internals into **standalone plugins** (identical names and behaviors; see `agent/plugins/agentxx_*`), loaded dynamically via the YAML `plugins` section or compiled directly into `libagentxx` via `AGENTXX_PLUGIN_BUILTIN_LIST` (not embedded by default). The core library retains only `share_store`, `subagent`, `tool_skill_search`, and lazy-loading wiring:

| Category | Tool | Description |
|---|---|---|
| **Filesystem** | `agentxx_filesystem_list` | Lists file and directory metadata (size, type, modification time), supporting recursive traversal. |
| | `agentxx_filesystem_read` | Reads text files line by line, supporting offset and limit parameters. |
| | `agentxx_filesystem_write` | Creates or overwrites text files. |
| | `agentxx_filesystem_edit` | Performs exact string replacement edits on text files. |
| | `agentxx_filesystem_glob` | Searches files matching glob patterns. |
| | `agentxx_filesystem_grep` | Searches file contents via literal text (`text_patterns`) or regular expressions (`regex_patterns`) (can search both simultaneously). |
| **Command Execution** | `agentxx_execute_bash_command` | Executes Linux shell commands with timeout enforcement (Linux / macOS). |
| | `agentxx_execute_windows_command` | Executes Windows commands, defaulting to PowerShell (auto-probes pwsh/powershell and injects version into prompts), falling back to cmd.exe (Windows / callable under WSL). |
| | `agentxx_execute_javascript` | Executes JavaScript code via the QuickJS interpreter (the JS equivalent of execute_bash_command; depends on `agentxx_javascript_engine` plugin). |
| **Mathematics** | `agentxx_math_calculate` | Mathematical expression parsing and evaluation (arithmetic, exponentiation, factorials, bitwise, logic, constants, trigonometric/hyperbolic/logarithmic/combinatorial functions, implicit multiplication). |
| **Network** | `agentxx_web_search` | Web search (DuckDuckGo / Model Search). |
| | `agentxx_web_fetch` | HTTP GET retrieving raw webpage content. |
| | `agentxx_web_fetch_markdown` | Fetches webpage content and converts it to Markdown. |
| **Knowledge Retrieval** | `agentxx_rag_search` | Vector semantic similarity search across knowledge bases. |
| **Code Analysis** | `agentxx_codegraph_search` | Searches code symbols by name. |
| | `agentxx_codegraph_context` | Retrieves symbol definitions, callers, and callees. |
| | `agentxx_codegraph_callers` / `agentxx_codegraph_callees` | Forward and reverse call-graph tracing. |
| | `agentxx_codegraph_path` | Finds call-chain paths connecting two symbols. |
| | | `agentxx_codegraph_*` tools are provided by the `agentxx_codegraph` plugin: registered only when configured in the YAML `plugins` section and compiled with `AGENTXX_ENABLE_PLUGIN_CODEGRAPH`. |
| **Planning** | `agentxx_planning` | Two-layer task planning (Mermaid state diagrams + Todo lists + Memo notes). |
| **Sub-Agent** | `agentxx_subagent` | Spawns and manages subagents for delegated task execution. |
| | `tool_skill_search` | Search and discovery for lazy-loaded tools and skills. |
| **Data** | `agentxx_share_store` | Session-level text storage for context economy. |
| | `agentxx_string_html_to_markdown` | Converts HTML to Markdown. |
| | `agentxx_string_regexp` | Regular expression matching, replacement, and extraction. |
| **System** | `agentxx_get_current_datetime` | Obtains current system date and time. |
| | `agentxx_get_system_core_info` | Retrieves CPU, memory, and GPU utilization metrics. |
| **UI Control** | `agentxx_ui_control_keyboard_mouse` | Mouse and keyboard automation on Windows (Windows only). |

Tool Characteristics:
- **Automatic Compaction**: Automatically prompts LLM summarization when tool outputs exceed length thresholds.
- **Lazy Loading**: Tools initially register only names, loading full parameter schemas on demand via `tool_skill_search`.
- **Deduplication**: Filesystem tools integrate with `SummarizationToolHandle`, pruning older duplicate call results.
- **MCP Extensibility**: Connects to external MCP Servers via MCP Client, dynamically registering remote tools over HTTP SSE and stdio transports.

### Git Worktree Mode (YAML `worktree.enable`, Disabled by Default)

Enabling registers the `agentxx_git_worktree` tool (`agentxx::tools::GitWorktreeTool`). Design goal: Multiple agent sessions started within the same repository directory develop in isolated worktrees simultaneously without mutual interference (inspired by Claude Code `--worktree` / `EnterWorktree` layered isolation design).

- **Tool Operations**: `create` (create + bind) / `info` (list + current binding) / `status` (uncommitted changes & unmerged commits summary) / `remove` (delete; the sole deletion entry under the default retention policy).
- **Bind on Creation**: A successful `create` writes directly to `Session::WorktreeBinding` (`{repoRoot}/.agentxx/agent/worktrees/{name}`, branch `agentxx/wt-{name}`, based on `base_ref` defaulting to current HEAD). All subsequent relative path resolutions and child process working directories for the session automatically switch to the worktree—ensuring correct behavior is platform-enforced rather than model-dependent. Simultaneously appends `.agentxx/` to `.git/info/exclude` (effective locally without polluting tracked files).
- **Permission Boundary Enforcement** (Code-level constraint): Binds per-session isolation (`SessionFsIsolation`) into `PermissionMiddleware`: write operations in the main checkout tree are DENIED (reads remain unrestricted), with isolation taking precedence over whitelists and mode defaults. Filesystem tool relative paths resolve against the worktree via `normalizePermissionPath(path, sessionId)`, ensuring stable path-permission synchronization.
- **Plugin Chain Propagation**: The `AgentxxConfigIface` plugin interface was upgraded to v3, adding `get_session_work_dir(host, session_id)` (prioritizing worktree bindings). Filesystem and execute_command plugins resolve paths dynamically per invocation using injected session IDs rather than static initialization caches.
- **Subagent Inheritance**: When `AgentHost` derives subagents, they inherit parent session worktree bindings (subagent `config.workDir` is preset to the worktree path with `inheritedWorktreePath` flag set). Permission Ask default rules and all toolchains automatically target the inherited worktree.
- **Lifecycle Management**: Worktrees are preserved under a keep policy and do not auto-delete on session termination. Deletion via `remove` runs dual-layer safety checks (uncommitted changes, untracked files, unmerged commits); if work artifacts exist, deletion is rejected with instructions to commit first, requiring `force=true` to override.
- Git underlying encapsulation: See `agent/lib/include/agentxx/util/worktree.h` (direct argv execution bypassing shells, terminating entire process group on timeout; tested in `worktree`).

### Middleware System

Stacked middleware architecture intercepting execution before and after Graph node runs:

| Middleware | Function |
|---|---|
| **PermissionMiddleware** | Tool execution authorization via EventBus requesting user decisions (HITL). Filesystem rules use longest-prefix matching with `*` wildcard support: rules on `/data/projects` apply to all nested children with fallback to parent chain rules (see `XXRouter::get`'s `prefix_fallback`). Default behavior governed by YAML `permission.mode` (Ask = ALLOW in workDir + INTERRUPT elsewhere / AllAsk = all INTERRUPT / Pass = all ALLOW / Deny = all DENY). Whitelists always allow; blacklists always deny (blacklists take priority on collision); `noRuleOperator` provides final fallback. Clients can select "remember this choice" (`WireSetPermission` persists rule back to server). |
| **SkillMiddleware** | Progressive discovery and loading of skill definitions (`SKILL.md`). |
| **MemoryFileMiddleware** | Reads and caches context memory files, injecting them into system prompts before each model call. |
| **SummarizationMiddleware** | Token tracking and automatic context compaction, preventing model window overflow. |
| **PlanningMiddleware** | Manages hierarchical planning state, injecting planning data into system prompts. |
| **WorktreeMiddleware** | Injects Git worktree prompts (YAML `worktree.enable`): Appends operational guidelines each turn based on binding state (unbound → suggest creation / bound → commit & finalize guidelines / subagent → isolation reminder) via `graphDataKey_appendSystemMessage`. |
| **AgentHost** | Process-level agent host: Registers root agent and subagents as equal peers (`AgentNode`), spawning independent agents for subagent execution, serving `service.subagent` on both root and child global buses (flattening nested and root delegations into identical paths), enforcing depth and concurrency budgets, routing inter-agent messages via `HostBus`, and handling lifecycle cleanup. |
| **EventBridge** | Translates internal GraphEngine events into strongly-typed EventBus events. |
| **LogPrint** | Debug logging middleware (conditionally compiled, log levels controlled by configuration). |

### Event System

- **EventBus**: Strongly-typed event bus supporting unidirectional event streams (`EventStream<T>`) and request-response streams (`RequestResponseStream<Req, Resp>`).
- **Event Topics**: Organized under `Topic` namespace constants covering `agent.*`, `service.*`, and `io.*`.
- **Subscription Model**: Supports permanent subscriptions and self-expiring subscriptions with hit limits (`execHit`).
- **HITL (Human-in-the-Loop)**: Interrupt and permission requests are dispatched to client UI via `RequestResponseStream`, supporting configurable timeouts.
- **Timers**: EventBus provides built-in timer streams supporting both one-shot (`once`) and periodic (`repeat`) modes.

### Multi-Session & Concurrency

- **Session Isolation**: Each `sessionId` maintains an isolated `Session` (mapped to `thread_id` in Graph context), encapsulating IO, EventBus, ContextStats, CancelToken, model selection, and message history.
- **SessionsManager**: Manages sessions by `sessionId` (accessed exclusively by the agent `io_context` thread, lock-free); `SessionStore` specifically refers to the attached SQLite persistence backend.
- **Single Checkpoint Storage**: Engine utilizes `InMemorySingleCheckpointStore` (inheriting from `SingleCheckpointStore` strategy base class, with template method `save = saveImpl` persisting latest and `evictImpl` pruning history). Agentxx relies only on `load_latest` checkpoints and attached pending writes for resume/interrupt recovery, eschewing forks or historical travel (`get_state_history`). Retaining only the latest checkpoint per thread drops storage overhead from O(super-steps) to O(threads) with zero turn-end pruning needed.
- **Activity States**: `Idle`, `Streaming`, `ExecutingTool`, `WaitingInput` (currently `ExecutingTool` and `Idle` are logged by `LogPrint`, while `Streaming` and `WaitingInput` are reserved; client activity awareness is driven via Delta event streams).
- **Chained Hashing**: `viewMessages` consistency is verified using FNV-1a chained hashes.
- **Thread Binding (Single-Thread Read/Write)**: Sessions bind to the IO thread via `bindIoThread()`; `assertIoThread()` enforces that mutable state (`viewMessages`, `llmMessages`, `chainHash`) is accessed solely on the IO thread. Clients and UI never read this memory directly, receiving wire-transferred copies (Sync / Delta) instead, obviating snapshot locks.
- **Cancellation & Model Switching**: Client-initiated cancellations and model switches are dispatched to the agent thread via Wire messages (`WireCancel`, `WireSelectModel`), preventing cross-thread race conditions.
- **Asynchronous Mutex**: `AsyncMutex` provides coroutine-aware mutual exclusion using asio `concurrent_channel`, suspending without thread blocking across `co_await` boundaries.

#### Session SQLite Persistence (Messages Context / viewMessages / share_store)

- Toggle: `AgentConfig::enableSessionStore` (disabled by default; enabled by `agentxx_cli` in `buildDefaultConfig`). When enabled, `BaseAgent` instantiates `SessionStore` and injects it into `AgentContext` (`AgentContext::sessions->sessionStore`; requires non-empty `dataDir` or explicit `sessionStoreDirectory`, otherwise falls back to memory-only with startup warning):
  - Recovers and persists session message state (attached via `SessionsManager` when creating a Session).
  - Supplies `MiddlewareContext` construction parameters (write-through share store).
- Data Directory: `{dataDir}/sqlite/sessions/{sessionId}/` (`sessionId` is sanitized via `sanitizeSessionId` to replace illegal characters, truncate length, and avoid Windows reserved names, appending an FNV hash suffix on collision; `dataDir` configured by YAML `data_dir`; **if neither dataDir nor sessionStoreDirectory is set, persistence is disabled** and settings, sessions, and codegraph indices remain in-memory only).
- Dual-Database Design (both use WAL mode + `busy_timeout`):
  - `session.db`: `viewMessages` (append-only, one JSON line per message) + `llmMessages` (single-row whole-table replacement) + `meta` (`msgIdCounter`, `title`, `lastActiveMs`). Sharing a common lifecycle and IO thread, these are committed together transactionally.
  - `share_store.db`: `agentxx_share_store` KV entries (auto-incrementing ID = current max ID + 1, continuing across restarts). Because random KV read/writes differ from message append logs and serve as context offload caches that can be purged independently, keeping them in an isolated file prevents large text blobs from inflating WAL checkpoint latency.
- Integration Points:
  - `SessionsManager::getOrCreate`: Recovers `viewMessages`/`llmMessages`, rebuilds chained hashes, restores `msgIdCounter`, and binds `SessionStoreHooks` (`std::function` callbacks decoupled from SQLite headers).
  - `Session::appendViewMessage` / `updateViewMessage`: Appends and backfills are persisted via a throttler (messages + counter committed transactionally; updates modify matching rows by `msg.id`, such as `toolFinished`/`toolResult`/`collapsed`, keeping on-disk history aligned with memory).
  - **Persistence Throttling** (`Session::kPersistThrottleMs` = 3s): First trigger flushes immediately; subsequent triggers within the window are coalesced (view operations are queued in order, while LLM context updates memory only), converging on the next trigger or turn end. If a process is terminated mid-turn (e.g. killed via taskkill), at most one throttling window (<3s) of settled messages is lost, avoiding empty LLM context anomalies.
  - `EventBridge::handleChannelWrite`: Channel writes signify settled LLM context batches (completed assistant turns / tool outputs, rather than streaming tokens), appended via `Session::appendSettledLlmMessages` and triggering throttled persistence.
  - `BaseAgent::runTurnAsync`: Persists authoritative final `llmMessages` at turn end and flushes unwritten `viewMessages`.
  - `MiddlewareContext` Share Store Methods: Uses in-memory map as read cache (hydrating all entries from DB on first session access), with write operations updating DB synchronously.
- Fault Tolerance: Database write failures log errors without interrupting conversational flow (best-effort persistence). Reads against non-existent directories return empty results without creating empty directories or files.
- Thread Safety: Internal mutexes protect `SessionStore`; under standard usage, invocations occur on the agent IO thread.

#### Session Switching (TUI Session Selector Modal)

```
TUI [F4] opens Session Selector Modal → WireListSessions (blocking I/O offloaded to blockingPool)
  → Server returns WireSessionList (persisted sessions sorted descending by lastActiveMs)
  → User confirms → WireSwitchSession(newSessionId)
  → SessionServerAgentIO::switchSession:
      Rebinds config_.sessionId → Clears delta replay buffer (new session uses independent seq numbering)
      → Resets firstTurn_ (first input takes resume_if_exists=true restoration path)
      → Pushes new session Sync (tail-window paginated by initialSyncTailCount; 0=full)
        + WireModelInfo + WireContextStats
  → Client (TUI) updates local sessionId binding; in WS mode, concurrently calls
    transport->updateReconnectSessionId() (resets reconnect handshake sessionId/lastSeq/tailHash)
```

- Allowed only when no active turn is executing (guarded on client and server).
- New session history is restored from `SessionStore` (or initialized empty if non-existent).
- Data source: Directory scan of `{dataDir}/sqlite/sessions/` + `meta` table (`sessionId`, `title`, `lastActiveMs`).

#### Subagent Execution Pipeline (NodeInterrupt → Bus Dispatch → Host Spawns Independent Agent)

```
Parent Agent LLM calls agentxx_subagent (single task = tasks array with 1 item, batch = multiple tasks)
  → SubagentManagerMiddlewareHandle (independent middleware owning SubAgentManagerTool singleton;
    injects agentxx_subagent into tools based on AgentConfig::enableSubagent; bus service always registered)
      → SubAgentManagerTool::execute_async
          → Validates parameters (valid subagent name + message/messages provided)
          → MiddlewareContext::requestInterrupt: saves interrupt args ({tasks: [...]})
            into graphData, throws NodeInterrupt → engine checkpoint pauses parent graph
  → AgentRunner (unified interrupt loop shared by root agent and subagents):
      → Parses interrupt args in graphData, dispatching by handle name:
        - "subagent" interrupt (unified batch semantics): parses ReqSubagentBatch
          (shared parseSubagentBatchFromInterrupt), requests delegation via local bus service.subagent
        - Other interrupts (permission prompts, etc.): requested via session bus service.interrupt,
          timing out per IO endpoint interruptTimeout; root agent inserts MessageTip header
  → AgentHost::spawnBatch → spawnOneTask:
      → Spawns an "independent agent" per task (isolated AgentContext / engine / SessionStore /
        middleware stack), registered as equal peer (AgentNode). Configured as lightweight subagent:
        no MCP connections / no plugins / no RAG / no CodeGraph, no parent Skill/Memory injected,
        no persistence, defaulting to configured subagent model
      → Host enforces nesting depth (maxDepth) and concurrency limits (maxConcurrentSubagents)
      → After subagent construction, host serves service.subagent on its global bus:
        subagent-scoped "subagent" interrupts (nested delegations) route via this bus,
        sharing identical paths with root delegations (flat delegation, no nested direct host calls)
      → HITL bubbling: subagent sessions inherit parent session IO and bus (permission/prompts reach user);
        parent session looked up from parent agent (parentAgentCtx) SessionStore (resolves to immediate parent)
      → Driven by same AgentRunner interrupt loop (no checkpoint persistence / no MessageTip)
      → Cancellation propagation: parent cancellation cascades to subagent (engine run cancelled)
      → Progress reported via hostBus agent.progress; completion notified via agent.done
      → On completion (success/error/cancel), host immediately reclaims AgentNode: session and middleware
        states are freed alongside AgentContext destruction, preventing thread-accumulated leaks
  → Results written back to graphData via interruptResult channel
  → AgentRunner resume_async resumes parent graph; execute_async extracts results keyed by
    (tool_call_id + "_") + (result_id | task_index) (plain text for single task, JSON array for batch)
```

- Subagents are fully independent agents: Isomorphic to root agent (`AgentNode`), message contexts are completely isolated.
- Single unified interrupt implementation (`AgentRunner`): Shared between root and subagents, with behavioral differences parameterized via hooks (checkpoint persistence / interrupt headers / event callbacks / pre-and-post resume logic). Delegation timeout is unlimited, preventing premature 30s timeouts on long tasks.
- Shared resume key generation: `(tool_call_id + "_") + (result_id | task_index)` is centralized in `makeSubagentResumeKey` / `buildSubagentResumeValues`, eliminating collisions during concurrent multi-task runs.
- Subagent tool injection and bus registration are decoupled from context compaction (which invokes `service.subagent.execute` directly).
- Cleans up interrupt args in `graphData` once resolved, preventing redundant processing.
- Cross-agent messaging (`agent.message`): Routed via local mailboxes or forwarded to remote agents via A2A bridge (`registerRemoteAgent`).

### Remote Communication

- **WebSocket Service**: `AgentServer` provides WebSocket services with token authentication.
- **Wire Protocol**: Bidirectional JSON message protocol (Hello, HelloAck, UserInput, Cancel, SelectModel, GetModel, Delta, Sync, InterruptRequest, InterruptResponse, InterruptExpired, TurnResult, ContextStats, Error, Log, ModelInfo, GetAppendComponentInfo, AppendComponentInfo, GetContext, ContextMessages, Ping, Pong, SetPermission, ListSessions, SessionList, SwitchSession, GetViewMessages, ViewMessagesPage, ClearMessageQueue, RemoveQueueItem, InterruptAndRunNext, MessageQueueUpdate, PluginData, PluginDataUp).
  - Queued message management: Active turn queues are maintained per session by the server and synchronized via `MessageQueueUpdate`. Clients can remove individual items (`RemoveQueueItem`), clear the queue (`ClearMessageQueue`), or interrupt the active turn to immediately run the front queue item (`InterruptAndRunNext`).
  - Plugin events are forwarded transparently via `PluginData` (agent→client downlink) and `PluginDataUp` (client→agent uplink).
- **Automatic Reconnection**: Clients reconnect automatically with `lastSeq` for incremental Delta replay; reverts to full `Sync` if `seq` continuity is broken.
- **History Pagination (viewMessages Tail-Window Sync)**: On long session recovery, the server initially syncs only a trailing window (`SessionServerAgentIO::Config::initialSyncTailCount`, local TUI mode = 100, remote configured via `AgentServer::Config`, 0 = full). `SyncPayload.fromIndex` specifies the window's starting absolute index, and `totalMessages` reports total session message count. When the client (TUI) scrolls near the top of the loaded window, it sends `GetViewMessages(beforeIndex, count)` to paginate older history, which the server returns via `ViewMessagesPage(startIndex, totalCount, messages)`. Since `viewMessages` is append-only, absolute indices are immutable and race-free. On the TUI side, prepended items invoke `LazyScrollable::notifyPrepended` for scroll anchoring (existing item layout caches/heights shift seamlessly, compensating view offsets by the prepended height so the visible viewport remains completely stable).
- **Grace Period**: Keeps sessions executing during disconnections, preventing accidental cancellations of active turns.
- **In-Process Direct Connection**: `ChannelAgentIOTransport` provides zero-serialization Channel transmission, directly linking client and agent in single-process mode.
- **Transport Layer Abstraction**: `AgentIOTransportBase` offers a unified `connect/recv/send/close/alive` interface, hiding network transmission details.

### Protocol Support

| Protocol | Role | Description |
|---|---|---|
| **OpenAI API** | Client | Compatible with OpenAI Chat Completions API (streaming / non-streaming), supporting thinking / reasoning_content. |
| **Anthropic API** | Client | Anthropic Messages API, supporting extended thinking and tool_use. |
| **MCP** | Client + Server | Model Context Protocol, supporting multi-version negotiation (2024-11-05 to 2025-11-25) over HTTP SSE + stdio transports. |
| **A2A** | Client + Server | Agent-to-Agent protocol v1.0, managing tasks (SendMessage, GetTask, CancelTask, ListTasks). |
| **ACP** | Server | Agent Communication Protocol in stdio service mode. |

### Client UI

- **TUI Mode**: FTXUI-based terminal interface featuring:
  - Message list supporting User, Assistant, Thinking, Tool, System, and Interrupt roles.
  - Automatic folding/expansion of Thinking and Tool blocks (expanded while executing, folded upon completion).
  - Interactive click-to-fold/expand: Clicking finalized messages toggles `msg.collapsed`; clicking the tail Thinking block during active streaming toggles in-component override state (`MessageListComponent::streamThinkOverride_`, cycling: unset follows TailThinkingMode / folded / expanded), resetting on stream start and finish. Hit areas are mapped from the previous frame's `visibleBoxes` (`collapsibleBoxes_` + `collapsibleIsStream_`).
  - Real-time streaming token rendering with Copy-on-Write (COW) semantics, preventing O(n²) string accumulation.
  - Permission dialogs with "Remember this choice" (registered via `WireSetPermission` into the server permission middleware).
  - Runtime model selector dialog.
  - Right-hand sidebar (Log console, Information panels, Planning visualization).
  - Pending user input queue: Inputs submitted during active turns queue up and dispatch automatically when the turn finishes. Synchronized via `MessageQueueUpdate`, supporting single item removal, full clear, and immediate interruption execution ("insert" button via `WireInterruptAndRunNext`).
  - Deferred model application: Confirming a model change in the selector dialog does not switch immediately; it attaches to the next user message (`WireUserInput.model`), applying automatically when the agent starts the next turn (mirroring remote `--model` parameter behavior); immediate switching remains available via `WireSelectModel`.
  - File edit diff comparison rendering.
  - Mermaid stateDiagram-v2 state machine rendering (renders ```mermaid blocks in messages, and Plan dialog roadmap state diagrams).
  - Context token utilization status bar.
  - Theme switching (persisted to `{dataDir}/sqlite/global.db`).
  - Session selector modal (F4): Lists persisted sessions (`WireListSessions`), switching via `WireSwitchSession`, with server pushing new session Sync (tail-window paginated), model info, and context statistics.
  - Paginated history loading: Restoring long sessions initially renders only the server's trailing window (100 items locally). Scrolling upward automatically paginates older history via `WireGetViewMessages`, anchoring scroll position stably until reaching session beginning (`historyWindowStart=0`).
  - Connection status banner: Displays server-io connection states (Connecting / Failed with clickable [Retry] button / Connected). Local mode is set ready prior to `SessionServerAgentIO` driver loop; remote mode is driven via `mode_runners` connection coroutines (`TUIRenderState::connState`).
  - Step-by-step startup progress banner: Initialization phases in `server-io init()` (environment checks, model registry, middlewares, loading MCP/RAG/plugins) report via `AgentContext::initNotifier` → `AgentIOBase::onServerProgress`. The banner dynamically reflects the active startup task, switching to keyboard shortcuts once ready.
  - Top-of-screen toast notifications.
  - Mouse drag-selection copying: Left-click drag and release copies text to the system clipboard (Win32 API on Windows, OSC 52 escape sequences on Linux/macOS subject to terminal support, confirmed via toast).
  - Auto-scroll lock to bottom (`Scrollable` component).
  - System resource metrics (CPU/RAM) & CodeGraph indexing status: Rendered via client-side plugin extensions (`agentxx_system_monitor` sidebar Info section + `/sysinfo` command / `agentxx_codegraph` Info section). Sampling and scheduling occur within plugins (`agentxx_system_monitor` samples periodically on agent side, publishing usage events forwarded to client plugins via `WirePluginData`). TUI core never initiates resource requests or parses plugin-specific payloads.
  - Plugin Info section injection: Client plugins inject custom sections (title + items conforming to panel schema) into the sidebar Info tab via `register_info_section`, rendered below the component list directly from client plugin registry snapshots.
- **Modular TUI Rendering**: Message list, sidebar, overlays, and diff tools are cleanly split into dedicated source files.
- **LazyScrollable (Flutter ListView.builder Paradigm)**: Message lists use a lazy virtual rendering architecture—describing lists via `itemCount`, `itemKey`, `estimateHeight`, and `buildItem` callbacks. Only viewport-intersecting children are constructed, laid out, and painted. Constructed children are cached in a bounded LRU cache (budgeted by both item count and source byte size), purging off-screen messages to decouple memory consumption from conversation length. Off-screen items utilize estimated heights, updated with measured heights upon entering view. Cache invalidation keys leverage message pointers combined with cheap O(1) mutations (`TUISharedState::mutableMessage`), avoiding expensive per-frame full-text hashing. Two-phase layout: builds and measures visible children (correcting estimated total heights) before calculating unified positioning, eliminating 1-line layout jitter during token streaming.
- **TUILogSink**: Diverts `XX_LOG` output directly into the TUI right-hand log panel.
- **CLI Mode**: Streamlined stdin/stdout terminal interaction.

### Training System

- **Evolutionary Training**: `EvolutionTrainingAgent` powers autonomous prompt optimization:
  - Mutation Strategies: Character-level random mutations (UTF-8 codepoint safe) + LLM-generated mutations; pre-deduplication before offspring evaluation.
  - Evaluation: Evaluates test suites supporting exact matching and LLM-assisted grading; test cases are randomized to avoid early-termination bias; single-case failures score 0 without halting training; evaluation sessions are immediately cleared (preventing memory/SQLite accumulation).
  - Optimization: Feedback-driven LLM prompt patch generation; patches normalize and filter empty fields ("" indicates keep unchanged), preventing accidental prompt clearing.
  - Elite Re-evaluation: Top N elites are re-evaluated each generation with Exponential Moving Average (EMA) smoothing (`PromptVariant.smoothedScore`), mitigating LLM grading noise in ranking and convergence checks.
  - Convergence Detection & Deduplication: Hash checks followed by full field comparisons prevent collision misdeletions.
  - Cancellation Support: Inspects `cancelToken` across generation and test case boundaries, saving progress for graceful termination (train mode handles SIGINT/SIGTERM via `asio::signal_set`, with repeated Ctrl+C triggering hard exits).
  - Persistence: Atomic saving (temporary file + rename) with backup rotation; empty population archives are rejected.
- **Training Configuration**: Supports independent models for training, scoring, and optimization; scorers and optimizers utilize lightweight `BaseAgent` instances (no toolchains), while the training agent runs full `CodeAgent`.

### Extension Capabilities

The following capabilities have been decoupled from the core library into standalone plugins under `agent/plugins/` (configured via YAML `plugins`; see `plugins.md`):

| Module | Description |
|---|---|
| **ScreenCapture** | Screen capture and streaming (multi-monitor support; plugin `agentxx_screen_capture`, Windows only). |
| **AudioStream** | System audio, microphone, and application audio stream capture (plugin `agentxx_audio_stream`, Windows WASAPI only). |
| **TextSelectionMonitor** | System-wide text selection event listener (plugin `agentxx_text_selection_monitor`, Windows UI Automation only). |
| **CpuGpuMonitor** | CPU, RAM, and GPU utilization inspection (plugin `agentxx_system_monitor`; tool + periodic sampling + client rendering). |
| **CodeGraphManager** | Code indexing and symbol analysis based on `codegraph-cpp` (plugin `agentxx_codegraph`): Indexing scope configured via plugin args in YAML `plugins` (`paths` directory list, defaulting to current working directory if empty; `ignore_paths` with wildcard support; `use_gitignore` respecting `.gitignore` rules, `.gitmodules`, and `.git`). Traversal prunes ignored subtrees immediately, and filesystem watchers apply identical filters. SQLite indices reside at `{dataDir}/sqlite/codegraph/<hashed_path>/index.db`; skipped if `dataDir` is unconfigured. |

### Dependency Injection

- **DependencyContainer**: Lightweight DI container supporting registration and resolution by type and name.
- **Factory Methods**: Supports factory function registrations returning `std::any`.
- **Singleton Management**: Defaults to lazy initialization, resolving circular dependency risks.
- **Named Registrations**: Supports multiple named dependencies under the same type.

---

## Usage Guide

### Compilation

```bash
# Linux Debug
bash agent/script/linux_debug_build.sh

# Linux Release
bash agent/script/linux_release_build.sh

# Windows Debug
agent\script\windows_debug_build.bat

# Windows Release
agent\script\windows_release_build.bat

# Windows Release (Cross-compilation on Linux)
bash agent/script/cross_windows_release_build.sh

# Android (Cross-compilation on Linux)
bash agent/script/cross_android_release_build.sh
```

Compilation artifacts output to the `agent/build/{platform}-{mode}/exec/` directory.

### Running Tests

```bash
# Run all tests
path/to/agentxx_test

# Stop immediately upon first failure (fail-fast)
path/to/agentxx_test --fail-fast

# Run specified test modules only
path/to/agentxx_test string_util regex agent
```

Available test modules (matching the registry list in `agent/test/test.cpp`):
- Synchronous modules: `string_util`, `regex`, `diff_util`, `events`, `concurrency`, `misc_fixes`, `aho_corasick`, `util_misc`, `training`, `settings_db`, `toolcall_args`, `ffi_c_api` (and client-side: `config_loader`, `tui_settings`, `tui_input`, `tui_interrupt`, `tui_scroll`, `tui_sidebar`, `tui_stream`, `tui_tool_header`, `sessionId`, `mermaid_state`).
- Asynchronous modules: `event_stream`, `event_bridge`, `interrupt_bus`, `subagent_bus`, `subagent_tool`, `agent_host`, `string_tools`, `math_tools`, `share_store`, `session_persistence`, `rag_search`, `datetime`, `filesystem`, `command`, `worktree`, `web_search`, `codegraph`, `screen_capture`, `cpu_gpu`, `text_selection`, `http`, `network_timeout`, `websocket`, `remote_agent`, `mcp`, `acp`, `a2a`, `openai_provider`, `anthropic_provider`, `plugins`, `plugin_resources`, `plugin_multi_instance`, `client_plugins`, `cancel`, `message_supplement`, `summarization`, `checkpoint_store`, `agent`, `memgrowth`.
- Platform modules: `screen_capture`, `text_selection`.

Test source directory layout: Root (entry + framework) / `core/` (lib core) / `plugin/` (plugin system & integrations) / `client/` (TUI/CLI, compiled only under `AGENTXX_BUILD_CLIENT`).
Conventions for adding new test modules: Headers contain only function declarations; assertion counters are defined in anonymous namespaces within the module's `.cpp`, with `#define XX_TEST_PASSED g_xxx_passed` / `#define XX_TEST_FAILED g_xxx_failed` mapping assertion macros from `test_framework.h`, returning `TestResult{g_xxx_passed, g_xxx_failed}` at function end—macro overrides or `extern` exports in headers are strictly forbidden to prevent cross-TU macro contamination.

### Configuration File

Configuration files use YAML format (defaults to `{program_cwd}/agentxx-config.yaml`, overrideable via `agentxx_cli --config <path>`). Selected values support `${VAR}` environment variable expansion:

```yaml
models:
  - name: "my-model"
    type: "openai"              # "openai" / "anthropic" / "openai-responses"
    base_url: "https://api.example.com"
    api_key: "${MY_API_KEY}"    # Resolved from .env or system environment
    model_name: "gpt-4"
    api_path: ""                # Custom API endpoint path (e.g. "/v1/chat/completions"); empty uses default
    send_thinking: false        # Whether to echo thinking/reasoning_content back in context to the model
    request_reasoning_summary: true # When send_thinking is true, requests reasoning summary from upstream
                                # (Responses API include parameter). Gateways like opencode-muse-spark do not
                                # support the reasoning.summary_text variant and require this to be false
                                # to avoid HTTP 400 errors; can also be overridden via extra_api_config include array
    ssl_verify: null            # Explicitly toggle TLS certificate verification (true/false); omitted uses default
    connect_timeout: 16
    read_chunk_timeout: 60
    max_concurrent_connections: 5   # Maximum concurrent HTTP connections for this model endpoint (default 5, 0=unlimited)
                                    # LLM requests use an HTTP keep-alive connection pool: idle connections are reused;
                                    # excess concurrent requests queue for available connections
    model_context_max_token: 128000
    extra_headers:              # Additional HTTP request headers (e.g. custom authentication/gateway headers)
      x-custom-header: "value"
    extra_api_config:           # Extended configuration merged into request body
      temperature: 0.7
    # Max output tokens automatically selects parameter name: standard models send max_tokens,
    # newer reasoning models (o1/o3/o4/gpt-5) automatically map to max_completion_tokens

use_model:
  default: "my-model"           # Primary model
  subagent: "my-model"          # Subagent model (falls back to primary if unspecified)
  web_search: ""                # Model-driven search (empty uses traditional search)
  acp: "my-model"               # ACP service mode model
  train: "my-model"             # Training agent model
  train_scorer: "my-model"      # Training scoring model
  train_optimizer: "my-model"   # Training prompt optimizer model

mcp:
  - namespace: "my_mcp"
    url: "http://localhost:3000/mcp"

# Unified data root directory (leave empty/unconfigured = no persistence: settings/sessions/codegraph
# remain strictly in memory and will not survive restarts; supports ~ and ${VAR} expansion, relative
# paths resolve against working directory)
# Special keyword `default` (tui/cli modes only): uses the system data directory
#   - Linux/macOS: ~/.agentxx/
#   - Windows: %APPDATA%/agentxx/
# Sub-paths created under data_dir:
#   - {data_dir}/sqlite/global.db                     Global settings (TUI settings, themes, etc.)
#   - {data_dir}/sqlite/sessions/{sessionId}/          Session data (session.db / share_store.db)
#   - {data_dir}/sqlite/codegraph/<hashed_path>/index.db CodeGraph index database
# data_dir: ~/.agentxx

# Session working directory (leave empty/unconfigured = uses current process working directory)
# - Supports ~ and ${VAR} expansion, relative paths resolve against startup directory
# - Effective scope: default approval boundary for permission.mode=ask / base directory for filesystem
#   tools and permission checks / initial working directory for spawned child processes /
#   plugin projectRoot (get_config; e.g. codegraph indexes this path by default if paths is unconfigured)
# - Use case: In server deployments and multi-instance FFI embeddings, binds agent execution to a specific
#   directory independent of process launch path (empty behaves identically to default cwd)
# work_dir: ${AGENTXX_WORK_DIR}

# Skill directory list (progressive discovery and loading of SKILL.md; relative paths resolve to work_dir)
skill:
  - "./skills"

# Context file list (Memory; injected into system prompts prior to every model call)
memory:
  - "./AGENT.md"

# Subagent delegation toggle (default true; when false, SubagentManagerMiddleware does not inject
# the `agentxx_subagent` tool, preventing model-initiated delegations; event bus service
# service.subagent.execute remains registered for internal uses like context compaction)
subagent:
  enable: true

# Git worktree mode (default false)
# - When enabled, registers `agentxx_git_worktree` tool + injects worktree behavior prompts each turn:
#   instructs models to create dedicated worktrees and bind sessions when starting code modification tasks,
#   enabling concurrent multi-session development in a shared repository (see "Git Worktree Mode" below)
worktree:
  enable: false

# Plugin configuration (all plugins loaded dynamically via path; relative paths resolve to absolute
# against process working directory; build artifacts reside under exec/plugins/<plugin_name>/;
# CodeGraph loaded here: requires AGENTXX_ENABLE_PLUGIN_CODEGRAPH at compile time)
plugins:
  - path: "./plugins/agentxx_codegraph"  # Path to plugin shared library or directory (with plugin.yaml)
    enabled: true                        # Default true
    sides: auto                          # auto|agent|client (for dual-sided plugins; auto detects via exports)
    args:                                # Plugin arguments (passed verbatim to plugin, schema defined by plugin)
      # ---- agentxx_codegraph args ----
      paths:                             # Indexing paths list (optional, multiple directories)
        - "/path/to/proj_a"
      ignore_paths:                      # Ignore patterns (supports * wildcards; skipped on match)
        - "**/third_party/**"
      load_cwd: true                     # Indexes current working directory if paths is omitted
      use_gitignore: true                # Respects .gitignore rules, .gitmodules submodules, and .git
                                         # Index database: {data_dir}/sqlite/codegraph/<hashed_path>/index.db
                                         # (skipped if data_dir is unconfigured)

# Permission prompt mode (default ask; see PermissionMode)
# - ask:     Allows reads/writes within working directory, prompts user for external paths (default)
# - all_ask: Prompts user for all file reads and writes
# - pass:    Allows all operations without prompt
# - deny:    Denies all operations without prompt
permission:
  mode: ask
  whitelist: []   # Always allowed paths (longest prefix matching with * wildcard; overrides mode default)
  blacklist: []   # Always denied paths (blacklists take precedence over whitelists on collision)
```

> **Codex (Responses API) Configuration Example**:
> ```yaml
> models:
>   - name: "openai-responses"
>     type: "openai-responses"          # OpenAI Responses API (/responses)
>     base_url: "https://api.openai.com"  # Or ChatGPT Codex compatible gateway
>     api_key: "${CODEX_API_KEY}"
>     model_name: "gpt-5-codex"
>     extra_api_config:                 # Optional overrides: reasoning effort, persistence, etc.
>       reasoning:
>         effort: "high"
> ```

Environment variable precedence: `Built-in variables` > `--env override file` > `.env file` > `System environment variables` > Retain `${VAR}` literal.
Built-in variables (injected by `main` at startup for YAML expansion):
- `${AGENTXX_WORK_DIR}`: Process working directory after launch (forward slash format).
- `${AGENTXX_EXEC_DIR}`: Directory containing the `agentxx_cli` binary (forward slash format).

### Command-Line Usage

```bash
agentxx_cli [mode] [options]
```

**Modes:**

| Mode | Description |
|---|---|
| `tui` | Interactive TUI mode (default). |
| `cli` | Interactive stdio command-line mode. |
| `server` | Launches WebSocket agent service. |
| `acp` | ACP stdio service mode. |
| `train` | Evolutionary prompt training mode. |

**Options:**

| Option | Description |
|---|---|
| `-h, --help` | Displays help message. |
| `--config <path>` | Configuration file path (default: `agentxx-config.yaml`). |
| `--env <path>` | Path to override environment variables file. |
| `--agent <url>` | Remote agent server address (`ws://host:port/agent`). |
| `--token <token>` | Authentication token. |
| `--model <model>` | Remote model name. |
| `--host <host>` | Server bind address (default: `127.0.0.1`). |
| `--port <port>` | Server bind port (default: `7007`). |

**Typical Invocations:**

```bash
# Local TUI mode (Client + Agent in single process)
agentxx_cli tui --config agentxx-config.yaml

# Local CLI mode
agentxx_cli cli --config agentxx-config.yaml

# Launch WebSocket Server
agentxx_cli server --host 0.0.0.0 --port 17000 --config agentxx-config.yaml

# Connect to remote agent (TUI)
agentxx_cli tui --agent ws://192.168.1.100:17000/agent --token xxx

# Connect to remote agent (CLI)
agentxx_cli cli --agent ws://192.168.1.100:17000/agent?token=xxx

# ACP stdio service
agentxx_cli acp --config agentxx-config.yaml

# Prompt training mode
agentxx_cli train --config agentxx-config.yaml
```

### Using as a Library

```cpp
#include "agentxx/agent/code_agent.h"

auto config = std::make_shared<agentxx::agent::AgentConfig>();
config->model.baseUrl   = "https://api.openai.com";
config->model.apiKey    = "sk-...";
config->model.modelName = "gpt-4";

agentxx::agent::CodeAgent agent(config);

asio::co_spawn(*agent.ioCtx, [&]() -> asio::awaitable<void> {
    co_await agent.init();

    // Single-turn conversation (non-streaming, returns full output text)
    auto result = co_await agent.runSingleInputAsync("session_1", "Hello!");

    // Session-based conversational turn (streaming deltas pushed via IO endpoint; nullptr for headless)
    auto turn1 = co_await agent.runTurnAsync("session_1", "Hi", io);
    auto turn2 = co_await agent.runTurnAsync("session_1", "Tell me more", io);

    // Custom message turn (can supply custom system prompt, returns complete output)
    std::vector<neograph::ChatMessage> msgs = {
        {.role = "system", .content = "You are helpful."},
        {.role = "user", .content = "Hello"},
    };
    auto output = co_await agent.runOverMsgsTurnAsync("session_2", msgs);
}, asio::detached);

agent.ioCtx->run();
```

---

## Architectural Design

### Overall Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                              Client Layer                              │
│  ┌────────────────────┐  ┌────────────────────┐  ┌──────────────────┐  │
│  │ TUIClientAgentIO   │  │ StdIOClientAgentIO │  │ Remote Client(WS)│  │
│  │     (FTXUI)        │  │     (stdio)        │  │WsAgentIOTransport│  │
│  └─────────┬──────────┘  └─────────┬──────────┘  └────────┬─────────┘  │
│            │                       │                      │            │
│            └───────────────────────┼──────────────────────┘            │
│                                    ▼                                   │
│                               AgentIOBase                              │
│                     (sendToPeer / onPeerMessage /                      │
│                      getInput / handleInterrupt)                       │
├────────────────────────────────────┬───────────────────────────────────┤
│                     Transport Layer (AgentIOTransportBase)             │
│  ┌─────────────────────────────────┴────────────────────────────────┐  │
│  │  ChannelAgentIOTransport (In-process, zero-serialization Channel)│  │
│  │  WsAgentIOTransport (Cross-process/network, JSON over WebSocket) │  │
│  │  connect() / recv() / send() / close() / alive()                 │  │
│  └─────────────────────────────────┬────────────────────────────────┘  │
├────────────────────────────────────┼───────────────────────────────────┤
│                               Agent Layer                              │
│  ┌─────────────────────────────────┴────────────────────────────────┐  │
│  │  AgentServer (WS Service) / SessionServerAgentIO (Session Driver)│  │
│  │  SessionServerAgentIO: delta buffer / replay / grace period      │  │
│  └─────────────────────────────────┬────────────────────────────────┘  │
│                                    ▼                                   │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                      BaseAgent / CodeAgent                       │  │
│  │  ┌────────────────────────────────────────────────────────────┐  │  │
│  │  │                    GraphEngine (ReAct Loop)                │  │  │
│  │  │                                                            │  │  │
│  │  │  __start__ → agent_start → llm → [has_tools?]              │  │  │
│  │  │                             ↑         │                    │  │  │
│  │  │                             └── tools ┘                    │  │  │
│  │  │                                  │                         │  │  │
│  │  │                              agent_end                     │  │  │
│  │  │                                  │                         │  │  │
│  │  │                               __end__                      │  │  │
│  │  └────────────────────────────────────────────────────────────┘  │  │
│  │                                                                  │  │
│  │  BaseAgent: Core infrastructure + ReAct loop + Session execution │  │
│  │  CodeAgent: Inherits BaseAgent, adds programming tools/middleware│  │
│  │                                                                  │  │
│  │  ┌──────────────┐ ┌──────────────┐ ┌──────────────────────────┐  │  │
│  │  │  ModelCall   │ │   Toolcall   │ │     AgentStart/End       │  │  │
│  │  │  WrapNode    │ │   WrapNode   │ │        WrapNode          │  │  │
│  │  └──────┬───────┘ └──────┬───────┘ └────────────┬─────────────┘  │  │
│  │         └────────────────┼──────────────────────┘                │  │
│  │                          ▼                                       │  │
│  │  ┌────────────────────────────────────────────────────────────┐  │  │
│  │  │                     Middleware Stack                       │  │  │
│  │  │  Permission → Skill → MemoryFile → Summarization           │  │  │
│  │  │  → Planning → LogPrint                                     │  │  │
│  │  └────────────────────────────────────────────────────────────┘  │  │
│  │                                                                  │  │
│  │  ┌────────────────────────────────────────────────────────────┐  │  │
│  │  │                          Tools                             │  │  │
│  │  │  Filesystem | Command | Web | RAG | CodeGraph              │  │  │
│  │  │  Planning | SubAgent | ShareStore | MCP | ...              │  │  │
│  │  └────────────────────────────────────────────────────────────┘  │  │
│  │                                                                  │  │
│  │  ┌────────────────────────────────────────────────────────────┐  │  │
│  │  │                        Providers                           │  │  │
│  │  │  OpenAIProvider | AnthropicProvider                        │  │  │
│  │  │  ModelProviderRegistry (Runtime dynamic switching)         │  │  │
│  │  └────────────────────────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                       EventBus (Event Bus)                       │  │
│  │  EventStream<T> (Unidirectional) | RequestResponseStream<Req,Resp│  │
│  │  Topics: Token / ToolCall / Interrupt / Permission / Subagent... │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                       Protocol Servers                           │  │
│  │  McpServer | A2aServer | StdioAcpServer                          │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                     Dependency Injection                         │  │
│  │  deps::DependencyContainer (Factory / Singleton / Named Registry)│  │
│  └──────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
```

### Data Flow

#### Local Mode (Direct In-Process Channel)

```
User Input → TUIClientAgentIO / StdIOClientAgentIO
    → AgentIOBase.sendUserInput()
    → ChannelAgentIOTransport::send() (client side, in-process channel)
    → ChannelAgentIOTransport::recv() (server side)
    → SessionServerAgentIO.onPeerMessage()
    → SessionServerAgentIO.run() → BaseAgent.runTurnAsync()
        → GraphEngine (ReAct Loop)
            → ModelCallWrapNode → OpenAI/Anthropic Provider → LLM API
            → ToolcallWrapNode → Tools (filesystem/command/web/...)
        → Delta Event Stream
    → SessionServerAgentIO.sendToPeer() (new deltas recorded to replay buffer and forwarded)
    → ChannelAgentIOTransport::send() (server side)
    → ChannelAgentIOTransport::recv() (client side)
    → TUIClientAgentIO / StdIOClientAgentIO.onPeerMessage() → onDelta() (protected passive callback)
    → UI Rendering
```

#### Remote Mode (WebSocket)

```
User Input → TUIClientAgentIO / StdIOClientAgentIO
    → AgentIOBase.sendUserInput()
    → WsAgentIOTransport::send() (client, JSON serialization)
    → WebSocket Network Transmission
    → AgentServer.handleWs()
    → WsAgentIOTransport::recv() (server, JSON deserialization)
    → SessionServerAgentIO.onPeerMessage()
    → ... (same execution flow as local mode)
    → SessionServerAgentIO.sendToPeer() (new deltas recorded to replay buffer and forwarded)
    → WsAgentIOTransport::send() (server, JSON serialization)
    → WebSocket Network Transmission
    → WsAgentIOTransport::recv() (client, JSON deserialization)
    → TUIClientAgentIO / StdIOClientAgentIO.onPeerMessage() → onDelta() (protected passive callback)
    → UI Rendering
```

> The topology in both modes is identical (client endpoint + server endpoint + transport), differing only in transport implementation. **Mandatory Transport Policy**: All communication between endpoints must pass through a transport; direct connection without transport does not exist, except in headless `runTurnAsync(io=nullptr)` execution (where no IO implies no event output).

### Core Design Patterns

#### 1. Graph Engine + ReAct Loop

BaseAgent's core execution pipeline is built on the NeoGraph engine, implementing a ReAct (Reasoning + Acting) loop:

```
__start__ → agent_start → llm → [conditional: has_tool_calls?]
                                  ├─ yes → tools → llm (loop)
                                  └─ no  → agent_end → __end__
```

- **agent_start**: Initializes session state, injects system prompt, flushes ephemeral scratchpads.
- **llm (ModelCallWrapNode)**: Dispatches LLM API calls with dynamic model switching, message repair, and retries.
- **tools (ToolcallWrapNode)**: Dispatches tool executions, automatically compressing verbose outputs.
- **agent_end**: Cleans up ephemeral state and persists finalized outputs.
- **Node events**: Each node execution emits `NodeStart`/`NodeEnd` events carrying node names.

#### 2. Stacked Middleware (WrapHandleBaseNode)

Middlewares execute in an interceptor stack similar to HTTP middleware:

```
start1 → start2 → start3
            ↓         ↓
          error     baseRun
            ↓         ↓
end1  ←   end2  ←   end3
```

- Each middleware implements `onHandleStart` / `onHandleEnd` hooks (mountable to `agent_start`, `modelcall`, or `toolcall`).
- On exceptions during `start`, `baseRun` is bypassed, jumping directly to unwind corresponding `end` handlers.
- Explicitly rethrows `CancelledException` and `NodeInterrupt`.
- Middlewares maintain isolated `State` per session (`sessionId`).
- Middleware stack registered in CodeAgent: Permission → Skill → MemoryFile → Summarization → Planning → LogPrint.

#### Cancellation Design (CancelToken Dual Channels)

Cancellation relies on `neograph::graph::CancelToken` (created per turn, stored in `Session`) with two propagation channels:

1. **Polling Checks (Primary Path)**: Explicit calls to `throw_if_cancelled()` / `is_cancelled()` at logical boundaries provide deterministic cancellation points and expected exception paths:
   - Between graph super-steps (engine).
   - Before toolcall dispatch and before/after each tool execution (`ToolcallWrapNode`).
   - Before LLM invocations and after retry delays (`ModelCallWrapNode`).
2. **asio Signal Interruption (For safely abortable long-running IO)**: `CancelToken::cancel()` emits a `cancellation_signal` via the bound executor, interrupting in-flight LLM HTTP streams, socket IO, and timers. This manifests at `co_await` points as `system_error(operation_aborted)`. This is the only context where `operation_aborted` is expected.

Boundary conversion rules (`exception.h` `isCancelAbort` / `wrap_handle.h`):

- `operation_aborted` + token is cancelled ⇒ converted to `CancelledException` propagating upward (cancellation semantics); `catchError/catchErrorAsync` with `cancelToken` enables this conversion.
- `operation_aborted` + token not cancelled/missing ⇒ treated as timeout error ("timeout: ...").
- All catch-all / `catch(std::exception&)` blocks must rethrow `CancelledException` and `NodeInterrupt`, never swallowing cancellation signals (`EventStream::publish`, `WrapHandleBaseNode`, `catchError`, etc.).
- Tools implement `ContextualAsyncTool` to receive `ToolExecutionContext{cancel_token}`, allowing them to poll cancellation or propagate it to underlying transports. On cancellation, completed tool results are preserved in `graphData` (`interruptToolcallCache`) before rethrowing.
- Thread-pool offloading (`offloadCancellableAsync`): Worker threads execute synchronously without suspending, immune to asio signals. Overloads accepting `CancelToken` spawn a watcher coroutine polling the token, asserting `cancelFlag` upon cancellation to notify worker threads to exit (integrated in `filesystem_list`/`glob`/`grep`).

Critical cleanup/persistence phases must explicitly guard against cancellations using `asio::this_coro::reset_cancellation_state` or `catch(CancelledException) -> finalize -> rethrow`.

#### 3. AgentIOBase Endpoint Model + Transport Layer

Both Client and Server inherit from `AgentIOBase`, communicating via `AgentIOTransportBase`. Communication between endpoints is symmetrical: **Outbound via `sendToPeer()` (sole outbound entry), Inbound via `runTransportLoop()` → `onPeerMessage()` dispatching to protected passive callbacks**.
Interface methods categorized by role: [Bidirectional] / [Client] / [Server]:

```
AgentIOBase (Public Contract)
    ├── sendToPeer() [Bidirectional]   → Sends WireMessage to peer (virtual, sole outbound port;
    │                                     requires configured transport, otherwise logs error and drops)
    ├── requestCancel() [Client]       → Requests cancellation
    ├── requestSelectModel() [Client]  → Switches model
    ├── requestAppendComponentInfo() [Client] → Queries startup info (Plugin/MCP/Skill/Memory)
    ├── sendUserInput() [Client]       → Sends user input
    ├── getInput() [Bidirectional]     → Supplies user input (polled by BaseAgent on server,
    │                                     invoked by local input loop on client)
    ├── handleInterrupt() [Bidirectional] → Handles HITL interactions (invoked by BaseAgent on server,
    │                                     called upon WireInterruptRequest on client)
    ├── registerOnBus() [Server]       → Binds to session EventBus (registers interrupt/permission handlers)
    ├── setTransport()/runTransportLoop() [Bidirectional] → Transport configuration & receive loop
    └── (protected)
        ├── onPeerMessage() [Bidirectional] → Inbound dispatch (routes to 4 callbacks below; overrideable)
        ├── onDelta() [Client]         ← Incremental event (dispatched only by onPeerMessage)
        ├── onSync() [Client]          ← Full sync calibration
        ├── onTurnResult() [Client]    ← Turn completion notification
        └── onContextStats() [Client]  ← Context token stats update

AgentIOBase (Client Endpoints: TUIClientAgentIO / StdIOClientAgentIO)
    ├── onDelta/onSync/onTurnResult/onContextStats (protected) ← Receives peer events → Render
    ├── getInput()         → Reads input from stdin / FTXUI
    ├── handleInterrupt()  → Displays interactive dialog to collect user response
    └── onPeerMessage()    → Overridden: handles InterruptRequest / Log / ModelInfo, etc.

AgentIOBase (Server Endpoint: SessionServerAgentIO)
    ├── sendToPeer()       → Overridden: new Delta events (with monotonic seq guard) are written
    │                          to replay buffer before transmission; replayed deltas skip buffer
    ├── onDelta/onSync     → Protected no-op implementations (satisfies pure virtual contract)
    ├── getInput()         → Awaits client input from inputChannel_
    ├── handleInterrupt()  → Sends InterruptRequest, awaiting client response (with timeout/expiration)
    ├── onPeerMessage()    → Overridden: processes Hello / UserInput / Cancel / SelectModel /
    │                          InterruptResponse / GetModel / GetAppendComponentInfo / GetContext /
    │                          ListSessions / SwitchSession / SetPermission / GetViewMessages /
    │                          ClearMessageQueue / RemoveQueueItem / InterruptAndRunNext / PluginDataUp
    ├── run()              → Driver loop: fetch input → execute turn → push result
    ├── stop()             → Halts driver loop (closes channel / cancels turn / fails pending)
    ├── onDisconnect()     → Triggers grace period timer on disconnect (cancels turn if grace expires)
    ├── handleGetViewMessages() → History pagination: slices [before-count, before) by absolute index,
    │                              returning WireViewMessagesPage (append-only index is race-free;
    │                              mismatched session returns empty page, count=0 defaults to 100)
    └── switchSession()    → Switches session: rebinds sessionId, clears delta replay buffer,
                             resets firstTurn_, returns new session Sync (tail-window paginated) +
                             model info + context stats

AgentIOTransportBase (Transport Layer Abstraction)
    ├── connect(hello)     → Establishes transport connection and sends initial handshake
    ├── recv()             → Receives WireMessage (coroutine blocking)
    ├── send(msg)          → Sends WireMessage
    ├── close()            → Shuts down transport
    └── alive()            → Checks transport vitality
```

Event Emission Flow: `BaseAgent` directly invokes its driven server endpoint in-process—deltas push via `io->sendToPeer(Delta)` (buffered and sent over transport to client), and stats push via `io->sendToPeer(WireContextStats)`; `BaseAgent` remains agnostic of transport specifics. Headless `runTurnAsync(io=nullptr)` produces zero events.

##### EventBridge: GraphEvent → Incremental Delta + EventBus Adapter

`EventBridge` serves as the sole event translator between BaseAgent and the NeoGraph engine:

```
NeoGraph GraphEngine (run_stream_async)
    └── GraphStreamCallback (invoked per event)
          └── EventBridge::operator()(GraphEvent)
                ├── 1. Forwards original callback origCb (if present)
                ├── 2. Dispatches by event type:
                │     ├── LLM_TOKEN     → publishModelToken (bus event, zero-cost if no subscribers)
                │     │                   + emitDelta(TextToken/ThinkToken, with intra-node timing)
                │     ├── CHANNEL_WRITE → handleChannelWrite:
                │     │                   - "message_tip" channel → Delta::MessageTip
                │     │                   - "messages" channel:
                │     │                     assistant(tool_calls) → appendViewMessage
                │     │                       + Delta::ToolStart stream
                │     │                     tool → appendViewMessage + backfill update
                │     │                       (edit tools include diff fields) + Delta::ToolEnd stream
                │     │                     assistant → appendViewMessage
                │     │                   - Pushes WireContextStats when LLM output is present
                │     ├── NODE_START/END → Node timing + Delta::NodeStart/NodeEnd
                │     └── ERROR          → publishError (bus event; reported via WireTurnResult)
                └── emitDelta: Allocates monotonic seq per session (via Session::nextDeltaSeq,
                       shared by EventBridge and SessionServerAgentIO), pushing via io->sendToPeer
```

- Stateful: Tracks active chunk types (content/thinking transitions) and node start timestamps.
- Lifetime: `makeCallback()` captures `shared_from_this()`; `AgentContext` holds a `weak_ptr`, locking prior to bus publishing.
- New GraphEvent types can be supported purely by extending `EventBridge` without modifying `BaseAgent`.

**TPS (Token/s) Generation Rate Tracking** (Dual-level statistics in EventBridge):
- Stream Level (Windowed Pushing): Timed from the start of each ModelCall stream (first token after node start), pushing the average rate of the **most recent window** every `tpsPushIntervalSec_` (default 3s) via `WireContextStats.tps` (tokens in window / window duration, reflecting real-time speed rather than cumulative average). Finalized upon stream completion or error.
- Turn Level (TurnEnd Reporting): Cumulative estimated tokens across all ModelCalls in a turn divided by cumulative streaming duration, embedded in the TurnEnd Delta and rendered in turn summary banners.
- Token estimation uses `countTokensForUtf8Str` (matching `SummarizationMiddleware`: ASCII ≈ 4 chars/token, non-ASCII ≈ 1.1 chars/token; internal fallback if summarization is absent).

#### 4. EventBus Strongly-Typed Events

```cpp
// Unidirectional event stream
auto& stream = bus.get<EventModelToken>("agent.model.token");
stream.subscribe([](const EventModelToken& e) -> asio::awaitable<void> {
    // Process token
});
co_await stream.publish(EventModelToken{.token = "hello"});

// Request-Response stream (HITL)
auto& rr = bus.getRR<ReqPermission, RespPermission>("service.permission");
rr.serve([](const ReqPermission& req, size_t corrId) -> asio::awaitable<RespPermission> {
    co_return RespPermission{.decision = RespPermission::Decision::Allow};
});
auto resp = co_await rr.request(ReqPermission{.category = "filesystem_write"});
```

Topic Naming Convention: `<scope>.<subject>[.<detail>]`

| Topic | Event Type | Direction | Description |
|---|---|---|---|
| `agent.turn.start` | EventAgentTurnStart | Unidirectional | Conversational turn begins |
| `agent.turn.end` | EventAgentTurnEnd | Unidirectional | Conversational turn ends |
| `agent.model.start` | EventModelCallStart | Unidirectional | Model invocation begins |
| `agent.model.token` | EventModelToken | Unidirectional | Model output token |
| `agent.model.end` | EventModelCallEnd | Unidirectional | Model invocation ends |
| `agent.tool.start` | EventToolCallStart | Unidirectional | Tool invocation begins |
| `agent.tool.end` | EventToolCallEnd | Unidirectional | Tool invocation ends |
| `subagent.progress` | EventSubagentProgress | Unidirectional | Subagent progress update |
| `io.display` | EventDisplay | Unidirectional | Generic display output |
| `io.user_input` | EventUserInput | Unidirectional | User input |
| `io.cancel` | EventCancel | Unidirectional | Cancellation signal |
| `agent.error` | EventError | Unidirectional | Error notification |
| `service.interrupt` | ReqInterrupt / RespInterrupt | Req/Resp | HITL Interrupt |
| `service.permission` | ReqPermission / RespPermission | Req/Resp | Permission check |
| `service.subagent` | ReqSubagentBatch / RespSubagentBatch | Req/Resp | Subagent delegation (batch semantics; single task = 1 item) |
| `service.crossagent` | ReqCrossAgent / RespCrossAgent | Req/Resp | Cross-agent query |

Host Bus (`HostBus`, owned by `AgentHost` for inter-agent routing):

| Topic | Type | Description |
|---|---|---|
| `agent.spawn` | ReqHostSpawn / RespHostSpawn | Req/Resp | Spawns subagents (host enforces depth and concurrency budgets) |
| `agent.message` | ReqHostMessage / RespHostMessage | Req/Resp | Inter-agent messaging (mailbox / A2A bridge) |
| `agent.progress` | EventHostProgress | Unidirectional | Progress events from any agent |
| `agent.done` | EventHostDone | Unidirectional | Agent completion (signals host to reclaim AgentNode) |

#### 5. Session Isolation & Lock-Free Design

```
AgentHost (Process-Level Host)
    ├── ioCtx                (Shared io_context, single-threaded interleaved multi-agent coroutines)
    ├── blockingPool         (Shared thread pool for blocking operations, avoiding per-agent redundancy)
    ├── hostBus              (Host bus: agent.spawn / message / progress / done)
    └── AgentRegistry        (AgentNode registry: root agent and subagents are equal peers)
         ├── "root" → AgentNode (Root agent, dedicated BaseAgent)
         └── "agent_N" → AgentNode (Subagent, dedicated BaseAgent)

AgentContext
    ├── agentConfig          (Global shared configuration)
    ├── middlewareHandleContext (Middleware handles)
    ├── bus                  (Global event bus)
    ├── modelRegistry        (Model registry)
    ├── host                 (Host reference injected during attachRoot or spawning)
    └── sessions (SessionsManager)
         ├── "session_1" → Session
         │     ├── io                    (AgentIOBase)
         │     ├── bus                   (Session-level event bus)
         │     ├── contextStats          (std::atomic fields, cross-thread safe)
         │     ├── activity              (Activity)
         │     ├── viewMessages + chainHash (IO thread exclusive read/write; client receives wire copies)
         │     ├── deltaSeq              (Standard uint64_t, incremented only on IO thread)
         │     ├── cancelToken           (IO thread exclusive)
         │     └── modelName             (IO thread exclusive, switched via Wire)
         └── "session_2" → Session
               └── ...

Thread-Safety Policy:
  - IO Thread: Reads and writes viewMessages / llmMessages / chainHash / deltaSeq (assertIoThread enforced).
  - Client / UI: Never reads internal memory directly, receiving wire copies (Sync / Delta).
  - Cancellation & Model Switching: Dispatched via Wire messages to the agent thread.
  - SessionsManager: Accessed strictly on the agent io_context thread, requiring no locks.
  - contextStats: std::atomic fields, safe for cross-thread reads (written by Summarization, pushed by IO).
  - AsyncMutex: Coroutine-aware mutex protecting critical sections across co_await suspensions.
```

### Connection & Reconnection Mechanism

#### WsAgentIOTransport Internal Architecture (Client Mode)

```
Client (WsAgentIOTransport)
  ├── establishConnection(): wsConnect + backoff retry (reconnectBackoff, cancellable)
  ├── writeLoop():    writeQueue (concurrent_channel, cap=4096) → ws send
  ├── readLoop():     ws recv → deserialization → recvQueue (concurrent_channel, cap=256)
  │                     → consumed by recv(); on disconnect, enters auto-reconnect loop:
  │                       reconnect → reconstruct writeQueue → send Hello(lastSeq, tailHash)
  │                       → server incremental replay (falls back to full Sync if seq broken)
  ├── heartbeatLoop(): sends Ping every heartbeatInterval
  └── Delta Deduplication: updates lastDeltaSeq_ on receipt, dropping replayed
      deltas where seq <= last to prevent duplicate UI rendering
```

- Write and read queues use bounded `concurrent_channel` buffers; `try_send` drops messages when saturated. Closing channels causes pending `async_receive` calls to throw exceptions, terminating loops cleanly.
- `HelloAck` is consumed during the `connect()` handshake and does not enter the `runTransportLoop` message stream.
- Server mode (WsClient injected by AgentServer): does not send `Hello` or initiate reconnects; handshake is managed by `AgentServer::serveTransport`.

```
Client                              Server
  │                                    │
  │──── Hello (thread, token, seq,    │
  │      tailHash, model) ───────────→│
  │                                    │ Validates token
  │                                    │ Finds/creates SessionServerAgentIO
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
  │  [Connection Severed]              │ Grace timer started
  │                                    │
  │──── Hello (seq=3, tailHash) ─────→│ Incremental replay of deltas where seq > 3
  │←── HelloAck + Delta replay ───────│ Falls back to full Sync if seq discontinuous
  │                                    │
  │──── Cancel ──────────────────────→│ Cancels active turn
  │                                    │
  │──── SelectModel (model) ─────────→│ Switches session model
  │                                    │
  │──── GetModel ────────────────────→│ Queries current model information
  │←── ModelInfo ─────────────────────│
  │                                    │
  │──── GetAppendComponentInfo ──────→│ Queries Plugin/MCP/Skill/Memory components
  │←── AppendComponentInfo ───────────│
  │                                    │
  │──── GetContext ──────────────────→│ Queries active llmMessages
  │←── ContextMessages ───────────────│
  │                                    │
  │ (Optional) Log Forwarding          │
  │←── Log (level, message) ─────────│ Real-time server log streaming
  │                                    │
  │ (Optional) Context Statistics      │
  │←── ContextStats ──────────────────│ Token usage & windowed TPS updates
  │                                    │
  │ (Optional) Remember Permission     │
  │──── SetPermission (path, allow) ──│ Registers path rule in server PermissionMiddleware
  │                                    │
  │ (Optional) Session Modal (TUI F4)  │
  │──── ListSessions ─────────────────│ Lists persisted sessions (offloaded to thread pool)
  │←── SessionList ───────────────────│
  │──── SwitchSession (sessionId) ───│ Switches session: clears replay buffer,
  │                                   │ pushes new session Sync (tail paginated) + model + stats
  │                                    │
  │ (Optional) History Pagination      │
  │──── GetViewMessages (before, n) ──│ Slices [max(0, before-n), before)
  │←── ViewMessagesPage ──────────────│ (startIndex/totalCount/messages);
  │                                   │ Client prepends with scroll anchoring
  │                                    │
  │ (Optional) Interrupt Expiration    │
  │←── InterruptExpired ──────────────│ On timeout, grace expiry, or cancel, client
  │                                   │ marks prompt as expired and ends waiting state
```

### Dependency Injection Container

```
deps::DependencyContainer
    ├── registerSingleton<T>(factory)       → Registers default singleton
    ├── registerNamedSingleton<T>(name, fn) → Registers named singleton
    ├── resolve<T>()                        → Resolves default instance
    ├── resolveNamed<T>(name)               → Resolves named instance
    └── hasType<T>()                        → Checks type existence
```

---

## Code Structure

```
agent/
├── lib/                          # Core libagentxx library
│   ├── include/agentxx/
│   │   ├── agentxx.h             # Master library header
│   │   ├── ffi_api.h             # Pure C ABI FFI export contract (stable cross-language interface; see ffi.md)
│   │   ├── agent/                # Agent Core
│   │   │   ├── base_agent.h      # BaseAgent base class (infrastructure, ReAct loop, session management)
│   │   │   ├── code_agent.h      # CodeAgent (inherits BaseAgent, adds programming tools and middlewares)
│   │   │   ├── agent_host.h      # AgentHost process-level host (manages root agent and subagents as peers)
│   │   │   │                     #   AgentNode / AgentRegistry / spawnBatch / HostBus / A2A bridge
│   │   │   ├── agent_runner.h    # AgentRunner unified "run + interrupt + resume" execution loop
│   │   │   ├── config.h          # AgentConfig / ModelConfig configurations
│   │   │   ├── config_static.h   # Static paths configuration
│   │   │   ├── context.h         # AgentContext / Session / SessionsManager / ContextStats
│   │   │   │                     #   Session: thread binding (viewMessages/chainHash single-thread access)
│   │   │   ├── checkpoint_store.h # Single checkpoint storage: SingleCheckpointStore strategy base +
│   │   │   │                     #   InMemorySingleCheckpointStore (retains latest per thread,
│   │   │   │                     #   auto-evicts history on save, O(super-steps) -> O(threads))
│   │   │   ├── conversation_types.h # Delta (NodeStart/End/seq/timing) / SyncPayload /
│   │   │   │                     #   ViewMessage (UI presentation message, role-specific sub-structs) /
│   │   │   │                     #   ChainHash / AppendComponentNotification
│   │   │   ├── model_registry.h  # ModelProviderRegistry (runtime model switching)
│   │   │   ├── session_store.h   # SQLite session persistence: per-sessionId directories,
│   │   │   │                     #   isolated session.db and share_store.db, non-creating reads
│   │   │   ├── prompt.h          # AgentPrompt / ToolPrompt prompt management
│   │   │   ├── training.h        # EvolutionTrainingAgent (mutation, evaluation, optimization, convergence)
│   │   │   └── io/               # Remote Communication & IO Endpoints
│   │   │       ├── agent_server.h    # AgentServer (WS server with token auth; serveTransport for in-process)
│   │   │       ├── session_server_agent_io.h # SessionServerAgentIO (session driver, delta buffering, grace,
│   │   │       │                          #   server message queue, plugin event forwarding via WirePluginData)
│   │   │       ├── wire_protocol.h   # Wire Protocol message schemas and serialization
│   │   │       ├── agent_io.h        # AgentIOBase endpoint base class (client/server contract)
│   │   │       ├── client_event_sink.h # Client event sink (forwards endpoint events to client plugins)
│   │   │       ├── agent_io_transport.h # Transport abstraction base class (connect/recv/send/close/alive)
│   │   │       ├── channel_io_transport.h # In-process zero-serialization Channel transport
│   │   │       └── ws_io_transport.h  # WebSocket transport (JSON codec, heartbeat, auto-reconnect)
│   │   ├── deps/                 # Dependency Injection
│   │   │   └── injector.h        # DependencyContainer (factory, singleton, named registries)
│   │   ├── event/                # Strongly-Typed Event System
│   │   │   ├── events.h          # Event definitions (Topic namespaces, event structs)
│   │   │   ├── event_stream.h    # EventBus / EventStream / RequestResponseStream
│   │   │   └── event_host.h      # HostBus event definitions (agent.spawn/message/progress/done)
│   │   ├── nodes/                # Graph Nodes
│   │   │   ├── wrap_handle.h     # WrapHandleBaseNode stacked middleware base class
│   │   │   ├── modelcall.h       # ModelCallWrapNode (LLM invocations, dynamic model switching)
│   │   │   ├── toolcall.h        # ToolcallWrapNode (tool dispatch, automatic compression)
│   │   │   └── agentcall.h       # AgentStart/EndCallWrapNode (session lifecycle hooks)
│   │   ├── plugin/               # Plugin System (native C++ plugins, pure C ABI, API v1: frozen core vtable + 13 tables)
│   │   │   ├── api/              # Public plugin headers (C ABI contract + SDK; included via api/ prefix)
│   │   │   │   ├── plugin_api.h      # Pure C ABI contract (frozen core vtable + COM QueryInterface; see plugins.md)
│   │   │   │   ├── client_plugin_api.h # Client-side plugin pure C ABI contract (UI-agnostic semantic layer)
│   │   │   │   ├── plugin_kit.h      # C++ SDK header-only (PluginBase/Task/awaiters/tools/hooks/spawn in agentxx::plugin)
│   │   │   │   ├── plugin_guard.h    # Plugin C ABI boundary exception handler header-only
│   │   │   │   ├── plugin_iface_helper.h # Interface table query caching (AgentIfaces/ClientIfaces)
│   │   │   │   └── plugin_tool_sync.h # Offload thread-pool adapter for async interfaces (embedded caller storage)
│   │   │   ├── op_driver.h       # Async operation driver (AgentxxOpNotify Done protocol)
│   │   │   ├── plugin_manager.h  # PluginManager lifecycle (load/enable/disable/unload) /
│   │   │   │                     #   PluginTool (C callback → thread-pool execution) /
│   │   │   │                     #   PluginMiddlewareHandle (7 hooks → C callbacks) /
│   │   │   │                     #   CapabilityRegistry / NativeLoader (dlopen ↔ LoadLibraryW)
│   │   │   ├── plugin_manager_base.h # Common plugin manager base (shared agent/client: instances, IO dispatch, memory)
│   │   │   ├── plugin_common.h   # Host-side common plugin utilities
│   │   │   ├── client_plugin_manager.h # ClientPluginManager (client-side loading, UI registry, command pipeline)
│   │   │   └── tool_registry.h   # Dynamic plugin tool lookup (shared_ptr lifecycle, static name collision checks)
│   │   ├── middlewares/          # Middlewares
│   │   │   ├── middleware.h      # BaseMiddlewareHandle / MiddlewareContext / State base classes
│   │   │   ├── permission.h      # PermissionMiddleware (tool permission HITL)
│   │   │   ├── skill.h           # SkillMiddleware (skill discovery and loading)
│   │   │   ├── memory_file.h     # MemoryFileMiddleware (context memory injection)
│   │   │   ├── summarization.h   # SummarizationMiddleware (context compaction)
│   │   │   ├── planning.h        # PlanningMiddleware (task planning state)
│   │   │   └── worktree.h        # WorktreeMiddleware (git worktree prompt injection)
│   │   ├── tools/                # Tools
│   │   │   ├── tool.h            # XXToolBase / XXToolWrap tool base classes
│   │   │   ├── filesystem.h      # Filesystem tools (list/read/write/edit/glob/grep)
│   │   │   ├── execute_command.h # Command execution tools (bash/windows/javascript)
│   │   │   ├── web_search.h      # Web search tools (search/fetch/fetch_markdown/model_search)
│   │   │   ├── rag_search.h      # RAG semantic search (EmbeddingClient / VectorStore)
│   │   │   ├── planning.h        # Planning tool (planning_write)
│   │   │   ├── subagent.h        # Subagent management tool
│   │   │   ├── tool_skill_search.h # Tool/skill lazy loading search
│   │   │   ├── share_store.h     # Session-level text storage
│   │   │   ├── string.h          # String tools (html2md / regexp)
│   │   │   ├── system.h          # System tools (datetime; cpu_gpu moved to plugin agentxx_system_monitor)
│   │   │   └── git_worktree.h    # Git Worktree tool (create/info/status/remove)
│   │   ├── protocol/             # Protocol Implementations
│   │   │   ├── openai_provider.h  # OpenAI Chat Completions API (streaming/non-streaming/SSE)
│   │   │   ├── anthropic_provider.h # Anthropic Messages API (thinking/tool_use)
│   │   │   ├── mcp_client.h      # MCP Client (HTTP SSE + stdio, multi-version negotiation)
│   │   │   ├── mcp_server.h      # MCP Server (HTTP + stdio, tool/resource/prompt)
│   │   │   ├── a2a_client.h      # A2A Client (Agent Card / SendMessage / Task management)
│   │   │   ├── a2a_server.h      # A2A Server (JSON-RPC, task state machines)
│   │   │   ├── acp_server.h      # ACP Server (stdio mode)
│   │   │   └── protocol_base.h   # Protocol base classes
│   │   └── util/                 # Utility Classes
│   │       ├── log.h             # Logging system (XX_LOG macros, LogDispatcher, LogSink)
│   │       ├── string_util.h     # String utilities (encoding conversion, normalization, base64, natural sort)
│   │       ├── http_client.h     # HTTP Client based on Boost.Beast (keep-alive pool, per-endpoint concurrency limit,
│   │       │                     #   per-io_context connection bucketing via HttpPoolContextGuard)
│   │       ├── http_server.h     # HTTP Server (routing, WS, SSE, SSL)
│   │       ├── http_header.h     # HeaderMap (case-insensitive HTTP header management)
│   │       ├── ws_client.h       # WebSocket Client
│   │       ├── exception.h       # Exception handling utilities
│   │       ├── lru_cache.h       # LRU Cache
│   │       ├── diff_util.h       # Line-level diff (unified diff format)
│   │       ├── regex.h           # Regex engine (Hyperscan)
│   │       ├── aho_corasick.h    # Aho-Corasick multi-pattern string matching
│   │       ├── router.h          # HTTP Router
│   │       ├── sqlite.h          # SQLite RAII wrapper (SqliteDb/Stmt, WAL + busy_timeout)
│   │       ├── async_mutex.h     # Coroutine-aware async mutex (based on concurrent_channel)
│   │       ├── async_offload.h   # Thread-pool offloading (offloadAsync/offloadCancellableAsync)
│   │       ├── worktree.h        # Git worktree wrapper (direct argv execution with timeout termination)
│   │       └── util.h            # General system detection utilities
│   └── src/                      # Source implementations (mirrors include directory hierarchy)
│
├── client/                       # agentxx_cli Executable
│   ├── main.cpp                  # CLI entry: argument parsing → config loading → mode dispatch
│   ├── include/agentxx-client/
│   │   ├── config_loader.h       # YAML config loading, .env parsing, variable expansion
│   │   ├── mode_runners.h        # Execution mode dispatchers (local/remote × tui/cli)
│   │   ├── io/
│   │   │   ├── stdio/
│   │   │   │   ├── agent_stdio.h # StdIOClientAgentIO (stdin/stdout interaction)
│   │   │   │   └── stdin_reader.h # Asynchronous stdin reader
│   │   │   └── tui/
│   │   │       ├── agent_tui.h   # TUIClientAgentIO (FTXUI UI, rendering, input queues, dialogs)
│   │   │       ├── scrollable.h  # Scrollable (full-build scroll container for sidebars)
│   │   │       ├── lazy_scrollable.h # LazyScrollable (virtual lazy rendering + LRU bounded cache)
│   │   │       ├── tui_theme.h   # TUI theme palettes
│   │   │       ├── framework/    # TUI Framework Layer
│   │   │       │   ├── tui_state.h       # TUI state aggregation
│   │   │       │   ├── tui_context.h     # TUI rendering context (theme, state, dimensions)
│   │   │       │   ├── tui_settings.h    # TUI global settings singleton
│   │   │       │   └── modal_container.h # Overlay modal containers
│   │   │       └── components/   # TUI UI Components
│   │   │           ├── message_list.h # Message list rendering
│   │   │           ├── sidebar.h      # Right-hand sidebar (logs, info, planning)
│   │   │           ├── overlays.h     # Overlays (permissions, interrupts, model picker)
│   │   │           ├── input_bar.h    # Input bar
│   │   │           └── status_bar.h   # Status bar (token metrics, activity state)
│   │   ├── train/                # Training mode
│   │   └── util/                 # Client utilities
│   └── src/                      # Source files matching headers
│
├── test/                         # agentxx_test Test Suites
│   ├── test.cpp                  # Test entry point and test module scheduling
│   ├── test_framework.h          # Lightweight test framework (assertions, TestResult)
│   ├── test_toolcall_args.*      # Tool argument automatic correction tests
│   ├── test_ffi_c_api.*          # FFI C API test suite (lifecycle, interaction, HITL, queues)
│   ├── core/                     # Core library tests (agent, hosts, providers, protocols, storage)
│   ├── plugin/                   # Plugin tests (system, resources, multi-instance, built-in plugins)
│   └── client/                   # Client tests (config, TUI state, input, streaming, scrolling)
│
├── benchmark/                    # Performance benchmarks
│
├── third_party/                  # Third-Party Dependencies
│   ├── boost/                    # asio, beast, process, exception
│   ├── codegraph-cpp/            # Code graph parser
│   ├── curl/                     # HTTP
│   ├── fmt/                      # Formatting
│   ├── FTXUI/                    # Terminal UI
│   ├── glob/                     # Glob matching
│   ├── html2md/                  # HTML to Markdown
│   ├── hyperscan/                # Regex engine
│   ├── iconv/                    # Character encoding conversion
│   ├── liburing/                 # io_uring
│   ├── markdown_ftxui/           # Markdown & Mermaid state diagram rendering
│   ├── NeoGraph/                 # Graph execution engine
│   ├── OpenSSL/                  # TLS/SSL
│   ├── quickjs/                  # QuickJS engine
│   ├── simdjson/                 # High-performance JSON parsing
│   ├── sqlite3/                  # Database engine
│   ├── uchardet/                 # Encoding detection
│   ├── yaml-cpp/                 # YAML parser
│   └── zlib/                     # Compression
│
├── ffi/                          # Foreign Language Bindings
│   └── dart/                     # Dart FFI bindings (ffigen.yaml generating agentxx_ffi_bindings.dart)
│
├── example/                      # Examples
│   └── ffi/dart/                 # Dart CLI example driving libagentxx via FFI
│
├── plugins/                      # Dynamic Plugins (built to exec/plugins/<plugin_name>/)
│   ├── example_plugin/           # Dual-sided C++ plugin template
│   ├── example_js/               # JavaScript plugin template
│   ├── example_resources/        # Declarative and programmatic resource contribution template
│   ├── agentxx_javascript_engine/ # QuickJS engine plugin
│   ├── agentxx_execute_javascript/ # JS code execution tool plugin
│   ├── agentxx_codegraph/        # CodeGraph analysis plugin (8 tools + sidebar info)
│   ├── agentxx_filesystem/       # Filesystem tools plugin
│   ├── agentxx_execute_command/  # Command execution tools plugin
│   ├── agentxx_math/             # Math expression evaluation tool plugin
│   ├── agentxx_websearch/        # Web search tools plugin
│   ├── agentxx_rag_search/       # Vector semantic search plugin
│   ├── agentxx_string/           # String utilities plugin
│   ├── agentxx_system/           # System time plugin
│   ├── agentxx_planning/         # Planning tool and TUI plan decoration plugin
│   ├── agentxx_screen_capture/   # Screen capture plugin (Windows only)
│   ├── agentxx_computer_use/     # Mouse/keyboard control plugin (Windows only)
│   ├── agentxx_system_monitor/   # Resource monitor plugin (tool + background sampling + TUI status)
│   ├── agentxx_audio_stream/     # Audio stream capture plugin (Windows WASAPI)
│   └── agentxx_text_selection_monitor/ # Text selection listener plugin (Windows UI Automation)
│
└── script/                       # Build & Run Scripts
    ├── linux_debug_build.sh
    ├── linux_release_build.sh
    ├── windows_debug_build.bat
    ├── windows_release_build.bat
    ├── cross_windows_release_build.sh
    └── cross_android_release_build.sh
```

### Key Dependencies

```
BaseAgent (Base Class)
  ├── GraphEngine (NeoGraph) + per-agent GraphRegistry
  │     ├── ModelCallWrapNode → OpenAIProvider / AnthropicProvider
  │     ├── ToolcallWrapNode → XXToolBase tool collection
  │     └── AgentStart/EndCallWrapNode
  ├── AgentRunner (Unified execution/interrupt loop shared between root and subagents)
  ├── MiddlewareContext → Middleware Stack
  ├── AgentContext
  │     ├── sessions (SessionsManager) → Session (per sessionId)
  │     │     ├── viewMessages + chainHash (IO thread exclusive; client receives Wire copies)
  │     │     ├── llmMessages (IO thread exclusive)
  │     │     ├── cancelToken / modelName (IO thread exclusive)
  │     │     ├── activity / contextStats (atomic, cross-thread safe)
  │     │     ├── deltaSeq (uint64_t, incremented only on IO thread)
  │     │     └── io / bus (session-level)
  │     ├── sessions->sessionStore (SessionStore SQLite persistence; optional)
  │     ├── ModelProviderRegistry
  │     └── EventBus
  └── AgentConfig → ModelConfig / AgentPrompt

CodeAgent (Inherits BaseAgent)
  ├── Tools: Filesystem | Command | Web | RAG | SubAgent | MCP | ... (CodeGraph injected via plugins)
  └── Middlewares: Permission → Skill → MemoryFile → Summarization → Planning → LogPrint

SessionServerAgentIO (Remote Session Driver)
  ├── AgentIOBase (Server endpoint)
  ├── BaseAgent.runTurnAsync()
  ├── deltaBuf (Disconnection buffer) + grace timer
  └── AgentIOTransportBase (Injected from AgentServer)

Client (agentxx_cli)
  ├── TUIClientAgentIO / StdIOClientAgentIO → AgentIOBase
  ├── ChannelAgentIOTransport / WsAgentIOTransport → AgentIOTransportBase
  ├── ConfigLoader → YAML + .env
  └── ModeRunners → local/remote × tui/cli permutations
     ├── runLocalTuiUnified / runLocalCliUnified
     └── runRemoteTui / runRemoteCli

EventBus (Event Bus)
  ├── EventStream<T> (Unidirectional: publish/subscribe/unsubscribe)
  ├── RequestResponseStream<Req, Resp> (Bidirectional: request/serve)
  └── Topic table (Topic namespace constants)
```

---

## Appendix A: Core Data Models (conversation_types.h)

### ViewMessage (UI Presentation Message)
- Common fields: `id` (`msgId`, allocated by `appendViewMessage`), `role`, `text`, `startTimeMs`/`durationMs`, `collapsed`.
- Role enumeration (`Role`): `User` / `Assistant` / `Think` / `System` / `Tool` / `Interrupt` / `Tip`.
- Role-specific optional sub-structures:
  - Tool: `ToolData` (`toolName`, `toolCallId`, `toolResult`, `diff`, `toolFinished`).
  - Think: `ThinkData` (`reasoningTokens`, `isEncrypted`).
  - Tip: `TipData` (`tipLevel`: `Info`/`Warning`/`Error`)—system notifications and turn statistics are atomically inserted via `InsertMessage`.
  - Interrupt: `InterruptData` (`interruptId`, input schema `inputLabel`/`Depict`/`Type`/`Default`/`Enums`, `inputIndex`/`Total`, status `Waiting`/`Confirmed`/`Cancelled`/`Expired`, `interruptResult`).
- Serialization: `toJson`/`fromJson` shared between Wire Sync and chained hashing.

### Delta (Streaming Incremental Event, Unified seq)
- Type: `TextToken` / `ThinkToken` / `ToolStart` / `ToolEnd` / `TurnStart` / `TurnEnd` / `NodeStart` / `NodeEnd` / `MessageUITip` / `InsertMessage`.
- Common fields: `seq` (monotonically increasing per session, shared by `EventBridge` and `SessionServerAgentIO` via `Session::nextDeltaSeq`), `text`, `msgId`, `toolName`/`toolCallId`/`arguments`/`result`/`hasError`, `nodeName`, `think` (`ThinkData`), `tipType` (for `MessageUITip`), `message` (contains full `ViewMessage` pointer for `InsertMessage`), `historyCount`/`tailHash`, `startTimeMs`/`durationMs`, `tps` (turn-level average rate for `TurnEnd`).
- `MessageUITip`: Transient message rendered in UI without persisting to `viewMessages`.
- `InsertMessage`: Atomically inserts a complete `ViewMessage` (tips, statistics) into history and synchronizes it across clients.

### SyncPayload / MessageQueueItem
- `SyncPayload`: `{fromIndex, messages[], tailHash, totalMessages, messageQueue[]}`
  - Full Sync: `fromIndex=0`, `totalMessages == messages.size()`.
  - Tail-window Sync: `fromIndex > 0` (indicating older unrendered messages exist upstream), `totalMessages` reflects full conversation count; client paginates older messages via `WireGetViewMessages`.
- `MessageQueueItem`: `{id, text, model, createdAtMs}`.
- `ChainHash`: FNV-1a chained hash accumulated via `append(string)`, exposing `tailHex` for `Hello`/`Sync` validation.

### Wire Protocol Pagination Semantics
- `ListSessions`: Keyset cursor `{beforeMs, beforeId, limit}` paginates descending by `lastActiveMs`; server replies with `WireSessionList {sessions[], totalCount, hasMore}`.
- `GetViewMessages`: Requests slice `[max(0, before-count), before)` via `{beforeIndex, count}`; server replies with `WireViewMessagesPage {startIndex, totalCount, messages[]}` (append-only indices are immutable and race-free).
- `GetContext` and synchronous queries: Blocks waiting for server reply (max 10s timeout; only one concurrent in-flight query per handle).

---

## Appendix B: Plugin System v1 Key Concepts (See plugins.md)

- COM-style interface table query: Frozen core vtable (`alloc`/`free` + `query_interface`), querying dedicated interface tables via string `IID`s (with independent table version fields).
- Agent side: 14 interface tables (`tools`, `hooks`, `events`, `capabilities`, `scheduler`, `session`, `plugins`, `config`, `model`, `cancel`, `prompt`, `json`, `log`, `resources`, `graph`, `tasks`).
- Client side: 7 interface tables (`ui`, `events`, `session`, `wire`, `self`, `json`, `log`; see `client_plugin_api.h`).
- SDK (`plugin_kit.h`): `PluginBase` state base + `Task<T>` coroutines + awaiters (`sleep`/`yield`/`offload`/`call_tool`/`invoke_cap`) + registration family (`tool`/`fast_tool`/`blocking_tool`/`hook`/`capability`/`spawn`). Background tasks spawned via `spawn` register to host `agentxx.agent.tasks`, allowing clean shutdown without dangling frames.
- Three Iron Rules of Multi-Instance Safety: No mutable global statics / State recovered via `user_data` closures / Cache interface tables in instance contexts.
- Export control: `-fvisibility=hidden` + version script whitelist (`AGENTXX_PLUGIN_EXPORT`).
- Platform matrix: Evaluated at the start of each plugin's `CMakeLists.txt` via `plugin_platform_support.cmake`.
- Utility reuse: Built-in plugins link statically against `agentxx_util` (symbols hidden without conflict).

---

## Appendix C: Session Working Directory Multi-Source Fallback (AgentContext::getSessionWorkDir)

Fallback precedence (highest to lowest):
1. `Session::WorktreeBinding.path` (Worktree mode active and bound).
2. `AgentContext::sessionWorkDirs_[sessionId]` (Session-level override, e.g. client cwd passed in ACP `session/new`, thread-safe via mutex).
3. `AgentConfig::resolvedWorkDir()` (YAML `work_dir` / process cwd).
- Callers uniformly retrieve paths via `getSessionWorkDir(sessionId)` rather than querying process `cwd` or `workDir` directly.
- Returns an empty string on invalidation, prompting caller-defined fallback (e.g. Permission Ask mode registers no default allow rules if workDir is empty).
