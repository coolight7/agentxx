> 文档自动翻译自[zh-cn](/docs/zh-cn/design.md/ffi.md)版 (This document is automatically translated from the [zh-cn](/docs/zh-cn/design.md/ffi.md) version.)

# libagentxx FFI C API Export Layer

> Implemented and verified by tests (`agentxx_test ffi_c_api`)
> Related docs: [design.md](index.md) (Core Architecture) · [plugins.md](plugins.md) (Pure C ABI Plugin Paradigm)
- When modifying FFI source code or design, this document must be updated. If the FFI interface changes, synchronously update the [exported bindings for other programming languages](/agent/ffi/) and [examples](/agent/example/ffi/).

## 1. Overview & Objectives

Provides a **strictly controlled and standardized C ABI export surface** for `libagentxx_shared.so`, allowing other programming languages (Python, Rust, Go, C#, Java, Node.js, etc.) via FFI (ctypes, JNA, jextract, wasm-abi, etc.) to **embed agent session runtimes within the host process**, reusing all Agentxx capabilities (ReAct loop, tools, MCP, plugins, sub-agent delegation, session persistence, HITL permissions / interruptions).

Core specifications and goals:

1. **Strictly Converged Export Symbols**: The shared library export surface is pruned from ~170,000 C++ symbols (default full export) down to **only 26 top-level C symbols**, adhering to the hard requirement that "exported symbols must be pure C APIs" (standard runtime dependencies like `libstdc++` remain `DT_NEEDED`, which is normal dynamic linking).
2. **Strict C ABI Conventions**:
   - 8-byte struct alignment: Global `#pragma pack(push, 8)`.
   - Fixed-width primitive types: Exclusively uses `int32_t`, `int64_t`, `uint32_t`, `uint64_t`.
   - Explicit calling convention: Cross-boundary exported symbols use `AGENTXX_FFI_CALL` (`__stdcall` on Windows, empty on x64 Unix).
   - Pointer-only struct parameters (`const Struct*`), struct returns replaced by pointer out-parameters (`Struct* out`) returning an `int32_t` status code.
   - Unified string boundaries: Read-only views `AgentxxStringView` and cross-heap allocated strings `AgentxxString` replace raw `const char*` and `char*`.
3. **Client-Parity Design**: The FFI layer implements a custom `AgentIOBase` client endpoint (`FfiClientAgentIO`) that communicates with `SessionServerAgentIO` (driven by `BaseAgent`) via an in-process `ChannelAgentIOTransport`—**completely isomorphic** to the TUI/CLI client, requiring zero changes to the agent core.
4. **Dual-Channel Error Handling**: Synchronous errors = return status code `int32_t` + optional `AgentxxString* log` out-parameter (internally populated with execution logs/error details, freed by caller with `agentxx_ffi_string_free`); Asynchronous errors = `EVT_ERROR` / `EVT_TURN_END {has_error}` events.

## 2. Overall Architecture & Thread Topology (Scheme A: Independent Dual-Thread Model)

Each `FfiAgentRuntime` instance is fully self-contained and strongly isolated, equipped with two dedicated IO threads:

```
┌─────────────────────────────────────────────────────────────┐
│ Host Application (Python/Rust/Go/... via FFI bindings)      │
│   Holds multiple AgentxxFFIAgent* handles, registers        │
│   AgentxxFFICallbacks C callbacks                           │
└──────────────┬──────────────────────────────────────────────┘
               │ extern "C" API (agentxx/ffi_api.h, pure C)
┌──────────────▼──────────────────────────────────────────────┐
│ FfiAgentRuntime (Each instance owns 1 Client + 1 Agent thrd)│
│                                                             │
│  ┌─────────────────────────┐  ┌───────────────────────────┐ │
│  │ Client-IO Thread        │  │ FfiClientAgentIO          │ │
│  │ (dedicated clientIoCtx_)│◄─┤ (AgentIOBase client endpt)│ │
│  │                         │  │  onDelta→JSON→on_event    │ │
│  │ - Runs client rx loop   │  │  WireInterruptRequest pend│ │
│  │ - Event dispatch & C cb │  │  Sync query promise route │ │
│  └─────────────┬───────────┘  └─────────────┬─────────────┘ │
│                │ ChannelAgentIOTransport    │ (In-process   │
│                │                            │  x-thread)    │
│  ┌─────────────▼────────────────────────────▼─────────────┐ │
│  │ Server-IO Thread (dedicated agentIoCtx_)               │ │
│  │ SessionServerAgentIO + CodeAgent + AgentHost           │ │
│  │ - ReAct loop / LLM streaming / Tool calls / SQLite DB  │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

Assembly Sequence (Dual-thread coordination, clientEx ↔ agentEx):

1. `ChannelAgentIOTransport::makePair(clientEx, agentEx)` → `setTransport` on both endpoints.
2. **On Agent Thread**: First call `co_spawn(serverIO->runTransportLoop())`: Client requests during initialization (`WireHello`/`WireGetModel`) are actively consumed without queue backlog.
3. **On Agent Thread**: Call `co_spawn(runAgentMain)`: `init()` → `AgentHost::create+attachRoot` (subagent delegation) → Cross-thread request for component info → `notifyServerReady` (triggers `EVT_READY`) → `serverIO->run()` (session driver loop).
4. **On Client Thread**: Call `co_spawn(clientIO->runTransportLoop())`: Event dispatch → C callbacks.
5. `sendToPeer(WireHello)` triggers full Sync / incremental replay; `WireGetModel` prefetches model information.

Graceful Shutdown (`stopInternal`, executed on caller's thread):

1. `clientIO->failAllPendingInterrupts()` (fails pending interrupts, ending awaiting coroutines).
2. `clientIO->transport()->close()` (terminates client receive loop).
3. `serverIO->stop()` (dispatched to agent IO thread `stopImpl`: closes input channel / cancels active turn / fails server-side pendings) → `run()` loop exits.
4. Polls `serverIO->running() == false` (timeout 20s).
5. `removeSink` → stops and joins `agentThread_` → stops and joins `clientThread_`.
6. Explicitly resets `clientIO_`, `serverIO_`, `agent_` and other objects, ensuring channels are destroyed before their parent `io_context`.

## 3. Thread Model Characteristics

- **Completely Decoupled Dual Threads**: Each `FfiAgentRuntime` maintains a dedicated `server-io` thread and a dedicated `client-io` thread.
- **Protecting Agent Core**: Any time-consuming operations performed by the host inside the `on_event` callback run strictly on the `client-io` thread, never blocking the `server-io` thread's ReAct loop, LLM streaming, or tool execution.
- **Strong Instance Isolation**: When multiple Runtimes are instantiated concurrently, their lifecycles, event pipelines, and session states are completely orthogonal, without global lock contention.
- **Thread-Safe C APIs**: Session interaction APIs are dispatched via `asio::post` to the `client-io` thread for serial execution; synchronous query APIs block via promise/future (timeout 10s).
- **Constraints**: `agentxx_ffi_stop` / `agentxx_ffi_destroy` must NEVER be called from within internal IO threads (i.e. inside client/agent callbacks); doing so returns `AGENTXX_FFI_ERR_STATE` (preventing self-join deadlocks). The host must invoke these from its own external thread.

## 4. C API Contract (`agent/lib/include/agentxx/ffi_api.h`)

### 4.1 Core Data Structures & Calling Conventions

```c
#pragma pack(push, 8)

// String view (borrowed ownership, 8-byte aligned)
typedef struct AgentxxStringView {
    const char* data;
    uint64_t    size;
} AgentxxStringView;

// Cross-CRT heap allocated string (explicit ownership, freed with agentxx_ffi_string_free)
typedef struct AgentxxString {
    char*    data;
    uint64_t size;
} AgentxxString;

// Error codes (int32_t)
#define AGENTXX_FFI_OK              0
#define AGENTXX_FFI_ERR_INVALID    -1   /* Invalid arguments (NULL handle, empty strings, etc.) */
#define AGENTXX_FFI_ERR_STATE      -2   /* Invalid state (not started / already stopped / stop called in IO thread) */
#define AGENTXX_FFI_ERR_JSON       -3   /* JSON parse error */
#define AGENTXX_FFI_ERR_CONFIG     -4   /* Invalid config (missing model / invalid field type) */
#define AGENTXX_FFI_ERR_INIT       -5   /* Agent initialization failed */
#define AGENTXX_FFI_ERR_INTERRUPT  -6   /* Interrupt ID invalid / already answered / expired */
#define AGENTXX_FFI_ERR_TIMEOUT    -7   /* Synchronous query timed out (10s) */
#define AGENTXX_FFI_ERR_OOM        -8
#define AGENTXX_FFI_ERR_INTERNAL  -99
```

### 4.2 Async-Safe Event Queue (For runtimes like Dart/ctypes unable to synchronously copy payloads)

`on_event` is invoked synchronously on the internal `client-io` thread; its payload is only valid during the callback lifetime. If the host forwards callbacks to another thread or event loop (e.g. Dart `NativeCallable.listener`, Python queue forwarding), the payload is invalidated before processing. Such hosts should bridge via the built-in event queue:

```c
AgentxxFFIEventQueue* q = agentxx_ffi_event_queue_create();
AgentxxFFICallbacks cb;
cb.on_event  = agentxx_ffi_event_queue_on_event; /* Built-in bridge: synchronous copy into queue */
cb.user_data = q;
/* ... After agentxx_ffi_create/start, consume events serially on host thread: */
int32_t type; AgentxxString json;
while ((rc = agentxx_ffi_event_queue_pop(q, &type, &json, 50)) == AGENTXX_FFI_OK) {
    /* Consume type / json.data (size json.size); free when done: */
    agentxx_ffi_string_free(&json);
}
agentxx_ffi_event_queue_free(q);
```

- `agentxx_ffi_event_queue_pop`: Blocks up to `timeout_ms`; returns `AGENTXX_FFI_ERR_TIMEOUT` when empty or timed out; returns `AGENTXX_FFI_ERR_STATE` if queue is destroyed.
- Bounded Queue (capacity 16,384): Drops oldest events when host stops polling, injecting an `EVT_ERROR` warning event.
- Implementation: `agent/lib/src/ffi/event_queue.cpp`.

### 4.3 Exported Symbol Inventory (26 symbols, see whitelist in `agent/lib/ffi_symbols.map`)

| Category | Symbols | Description |
|---|---|---|
| Memory | `agentxx_ffi_malloc` / `agentxx_ffi_free` / `agentxx_ffi_string_free` / `agentxx_ffi_strdup_n` | Sole allocation and deallocation channel across CRT heap boundaries |
| Version | `agentxx_ffi_api_version` / `agentxx_ffi_library_version` | API version check / Library version string view out-parameter |
| Error | `agentxx_ffi_strerror` | Error code → static string view out-parameter |
| Lifecycle | `agentxx_ffi_create` / `agentxx_ffi_start` / `agentxx_ffi_stop` / `agentxx_ffi_destroy` | Create (does not start threads) / Async start (`EVT_READY`) / Sync stop (idempotent) / Destroy (auto-stops if running) |
| Session (Async) | `agentxx_ffi_send_input` / `agentxx_ffi_cancel` / `agentxx_ffi_select_model` / `agentxx_ffi_set_permission` / `agentxx_ffi_switch_session` | Dispatched to IO thread for serial execution; inputs sent before READY are auto-queued |
| Synchronous Queries | `agentxx_ffi_get_model_info` / `agentxx_ffi_get_context_messages` / `agentxx_ffi_list_sessions` | Blocks waiting for server response (max 10s); results written to `AgentxxString* out` (freed via `agentxx_ffi_string_free`); only one in-flight query per handle |
| HITL Response | `agentxx_ffi_interrupt_respond` | Submits response for `EVT_INTERRUPT_REQ` (`values_json` array maps 1:1 to inputs order) |
| Logging | `agentxx_ffi_drain_logs` | Drains pending logs `[{"level","message"},...]` into `AgentxxString* out` (for post-failure diagnostics) |
| Event Queue | `agentxx_ffi_event_queue_create` / `agentxx_ffi_event_queue_free` / `..._on_event` / `..._pop` | See Section 4.2 |
| Built-in Plugins | `agentxx_plugin_get_builtin_plugins` | Manifest entry for monolithic embedded plugin mode (used by PluginManager; 26th whitelist symbol, hiding 170k C++ symbols) |

Version Policy: Global `AGENTXX_FFI_API_VERSION` is reset to 1. Adding non-breaking symbols/fields does not increment it; breaking removals, renames, or semantic parameter modifications will increment it.

### 4.4 Event Types (`AgentxxFFIEventType`, payloads are `const AgentxxStringView*` JSON)

| Event | Payload | Description |
|---|---|---|
| `EVT_READY` | `{"sessionId"}` | Server is ready; input transmission can begin |
| `EVT_SYNC` | wire sync JSON | Full or partial history sync (supports `fromIndex` tail-window semantics) |
| `EVT_DELTA` | wire delta JSON | Streaming delta (`kind=text_token/thinking_token/tool_start/tool_end/turn_end/...`) |
| `EVT_TURN_END` | wire turn_result JSON | Turn finished (`has_error` field reports asynchronous errors) |
| `EVT_CONTEXT_STATS` | wire context_stats JSON | Context token statistics (including TPS) |
| `EVT_MODEL_INFO` | wire model_info JSON | Current model information (query/switch result) |
| `EVT_COMPONENTS` | wire append_component_info JSON | Startup components (MCP, Skills, Memory, Plugins) loading status |
| `EVT_INTERRUPT_REQ` | `{"interruptId","sessionId","node","value","argJson"}` | HITL interrupt prompt (permission confirmation, user input collection); `argJson.inputs` describes input fields (`bool`/`int`/`double`/`string`/`enum` + `defaultValue`/`enumValues`) |
| `EVT_INTERRUPT_EXPIRED` | `{"interruptId"}` | Interrupt expired or cancelled; can no longer be answered |
| `EVT_PLUGIN_DATA` | wire plugin_data JSON | Agent-side plugin event forwarding (`{plugin,event,data}`) |
| `EVT_ERROR` | `{"code","message"}` | Internal error |

### 4.5 Configuration and Model JSON (`agentxx_ffi_create` parameters)

```c
// config_json (Can be NULL; unknown fields are ignored):
{ "dataDir": "~/.agentxx",            // Empty = no persistence (default)
  "workDir": "/abs/project/dir",      // Session working directory; empty = process cwd (default).
                                      // Effective scope: permission Ask approval boundary /
                                      // base directory for filesystem & permission checks /
                                      // initial working directory for spawned child processes /
                                      // plugin projectRoot.
                                      // Supports ~/relative paths (expanded against process cwd);
                                      // Multiple embedded runtime handles can bind separate project directories.
  "enableSessionStore": false,
  "sessionStoreDirectory": "",        // Defaults to {dataDir}/sqlite/sessions/ when empty
  "permissionMode": "ask",            // ask|all_ask|pass|deny (matches yaml permission.mode; ask = ALLOW inside workDir, INTERRUPT elsewhere)
  "permissionAllowPaths": ["..."],    // Permission whitelist
  "permissionDenyPaths": ["..."],     // Permission blacklist
  "skills": ["..."],                  // List of skill directories
  "memoryFiles": ["..."],             // List of context memory files
  "mcpServers": {"ns": {"url": "...", "timeoutSec": 120}},
  "plugins": [{"path":"...", "enabled":true, "sides":"agent|client|auto", "args":{}}],
  "llmMaxRetry": 5,
  "agentName": "Agentxx",
  "interruptTimeoutSec": 0 }          // HITL timeout waiting for host response, 0 = unlimited (default)

// model_json (Recommended; isValid if baseUrl is non-empty or apiKey != "EMPTY"):
{ "name": "Display Name", "type": "openai|anthropic|openai-responses",
  "baseUrl": "...", "apiKey": "...", "modelName": "(Target model name)",
  "apiPath": "", "connectTimeoutSeconds": 16, "readChunkTimeoutSeconds": 100,
  "sslVerify": true|null, "maxConcurrentConnections": 5,
  "anthropicVersion": "2023-06-01", "modelContextMaxToken": 0,
  "extraHeaders": {"k":"v"}, "extraConfig": {} }
```

## 5. Language Bindings & Examples

| Directory | Description |
|---|---|
| [`agent/ffi/dart/`](/agent/ffi/dart/) | Dart FFI binding package: `ffigen.yaml` auto-generates bindings from `ffi_api.h` (`dart run ffigen --config ffigen.yaml`, outputting `lib/agentxx_ffi_bindings.dart`). |
| [`agent/example/ffi/dart/`](/agent/example/ffi/dart/) | Dart CLI example (`agentxx_dart_cli`): Streaming rendering, HITL permissions, session switching, `/model`, `/sessions`, `/logs` commands, Ctrl+C graceful exit, and mock LLM smoke checks (`example/smoke_check.dart`). See its README for details. |

Other languages integrate via the same pattern: load whitelisted symbols via `dlopen`/`dlsym` (or platform equivalent) and register `AgentxxFFICallbacks`. For runtimes sensitive to payload lifetimes, bridge through the event queue described in Section 4.2.

## 6. Testing

- `agent/test/test_ffi_c_api.cpp` (Module name `ffi_c_api`): Full test coverage across lifecycle, interactions, HITL, event queues, and synchronous queries, verified end-to-end with an embedded mock LLM Server.

---

## 7. Implementation Notes (Updated 2026-08)

- **Direct Channel Connection**: The FFI layer utilizes `ChannelAgentIOTransport::makePair` (zero-serialization `concurrent_channel`) instead of WebSocket. `SessionServerAgentIO` serves as the server endpoint and `FfiClientAgentIO` as the client endpoint, adhering completely to the TUI/CLI transport abstraction.
- **Working Directory Resolution**: `config_json.workDir` supports `~`/`${VAR}` expansion and relative paths (resolved to absolute against process `cwd`). If unspecified, it falls back to process `cwd`, matching `AgentConfig::resolvedWorkDir()` semantics. Session-level worktree bindings (`Session::WorktreeBinding`) and multi-source fallbacks via `AgentContext::getSessionWorkDir` operate identically for FFI handles (all relative paths within the session adapt dynamically).
- **Plugin Sides Option**: `plugins[].sides` accepts `auto` (default, detected automatically via `agentxx_plugin_client_create` export), `agent` (loaded only on agent side), or `client` (loaded only on client side; FFI typically uses `agent`).
- **Synchronous Query Concurrency**: For `get_model_info`/`get_context_messages`/`list_sessions`, only one in-flight request per handle is permitted at any given time (server protocols are strictly sequential). On 10s timeout, it returns `AGENTXX_FFI_ERR_TIMEOUT`, and an `EVT_ERROR` payload `{"code","message"}` is also dispatched.
- **HITL Input Schema**: In `EVT_INTERRUPT_REQ`, `argJson` is the serialized `InterruptHandleArg`. Each element of `inputs[]` contains `label`, `depict`, `type` (`bool`/`int`/`double`/`string`/`enum`), `defaultValue`, and `enumValues`. An empty `type` indicates no input is required (respond with an empty array `[]`).
- **Cross-CRT Heap Management**: All `char*` return values and `char** log` pointers are allocated via `agentxx_ffi_malloc`; hosts must release them using `agentxx_ffi_free`. `agentxx_ffi_strdup_n` is the standard copy helper.
