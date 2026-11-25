# libagentxx FFI C API Export Layer

> Implemented and verified by tests (`agentxx_test ffi_c_api`, 451 assertions)
> Related docs: [design](index.md) (Core Architecture) · [plugins.md](plugins.md) (Pure C ABI Plugin Paradigm)
- When modifying FFI source code or design, this document must be updated. If the FFI interface changes, synchronously update the [exported bindings for other programming languages](/agent/ffi/) and [examples](/agent/example/ffi/).

## 1. Overview & Objectives

Provides a **strictly controlled C ABI export surface** for `libagentxx_shared.so`, letting
other programming languages (Python, Rust, Go, C#, Java, Node.js, Dart, ...) embed an agent
session runtime **inside the host process** via FFI (ctypes / JNA / jextract / ffigen), reusing
all Agentxx capabilities (ReAct loop, tools, MCP, plugins, sub-agent delegation, session
persistence, HITL permissions / interruptions).

Core specifications and goals:

1. **Converged export surface**: only **15** C symbols (`agent/lib/ffi_symbols.map`
   whitelist); every other C++ symbol is hidden. **Business capabilities add no symbols** —
   a new capability is a new op name.
2. **Submit + reply call model**: the single business entry point is
   `agentxx_ffi_call(op, args)`; it returns a `reqId` immediately and the outcome arrives as
   an `EVT_RESULT` event. Every accepted reqId gets **exactly one** reply.
3. **Strict C ABI conventions**:
   - 8-byte struct alignment: global `#pragma pack(push, 8)`
   - Fixed-width primitive types: `int32_t` / `int64_t` / `uint32_t` / `uint64_t`
   - Explicit calling convention: `AGENTXX_FFI_CALL` (`__stdcall` on Windows, empty on
     x64 Unix)
   - Pointer-only struct parameters (`const Struct*`), struct returns replaced by pointer
     out-parameters (`Struct* out`) plus an `int32_t` status code
   - Unified string boundary: read-only view `AgentxxStringView` and cross-heap allocated
     `AgentxxString` replace raw `const char*` / `char*`
4. **Event payloads belong to the callback**: the `AgentxxString*` handed to `on_event`
   is one heap block allocated by the library (struct + JSON body); after reading the body
   the callback releases it with `agentxx_ffi_free(payload)`. Because the struct itself
   lives in that block, the callback may forward the payload to its own thread / event loop
   (Dart `NativeCallable.listener`, Node threadsafe function, a Python queue, ...).
5. **Client-parity design**: the FFI layer implements a custom `AgentIOBase` client endpoint
   (`FfiClientAgentIO`) talking to `SessionServerAgentIO` (driven by `BaseAgent`) over an
   in-process `ChannelAgentIOTransport` — **isomorphic** to the TUI/CLI client, zero changes
   to the agent core.

## 2. Overall Architecture & Thread Topology

Each `FfiAgentRuntime` instance is self-contained and strongly isolated:

```
┌─────────────────────────────────────────────────────────────┐
│ Host Application (Python/Rust/Go/Dart/... via FFI bindings) │
│   Holds multiple AgentxxFFIAgent* handles, registers        │
│   AgentxxFFICallbacks C callbacks                           │
└──────────────┬──────────────────────────────────────────────┘
               │ extern "C" API (agentxx/ffi_api.h, pure C)
┌──────────────▼──────────────────────────────────────────────┐
│ FfiAgentRuntime (client thread + server thread + finish thd)│
│                                                             │
│  ┌─────────────────────────┐  ┌───────────────────────────┐ │
│  │ Client-IO Thread        │  │ FfiClientAgentIO          │ │
│  │ (dedicated clientIoCtx_)│◄─┤ (AgentIOBase client endpt)│ │
│  │                         │  │  reply routing by reqId   │ │
│  │ - Event dispatch & C cb │  │  interrupt parking/batch  │ │
│  │ - Timeout check (1 tmr) │  │  log pump (1s)            │ │
│  └─────────────┬───────────┘  └─────────────┬─────────────┘ │
│                │ ChannelAgentIOTransport    │ (In-process   │
│                │                            │  x-thread)    │
│  ┌─────────────▼────────────────────────────▼─────────────┐ │
│  │ Server-IO Thread (dedicated serverIoCtx_)              │ │
│  │ SessionServerAgentIO + CodeAgent + AgentHost           │ │
│  │ - ReAct loop / LLM streaming / Tool calls / SQLite DB  │ │
│  └────────────────────────────────────────────────────────┘ │
│                                                             │
│  Finish thread (created only for stop): wait session end →   │
│  plugin shutdown → stop both io threads → Stopped →          │
│  emit EVT_STOPPED                                            │
└─────────────────────────────────────────────────────────────┘
```

Assembly sequence (clientEx ↔ agentEx):

1. `ChannelAgentIOTransport::makePair(clientEx, agentEx)` → `setTransport` on both endpoints.
2. **On the server thread**: `co_spawn(serverIO->runTransportLoop())` first, so client
   requests during init (`WireHello` / `WireGetModel`) are consumed instead of queued.
3. **On the server thread**: `co_spawn(runAgentMain)`: `init()` → `AgentHost::create` +
   `attachRoot` (sub-agent delegation) → cross-thread component info request → ready
   (`EVT_READY`) → `serverIO->run()`.
4. **On the client thread**: `co_spawn(clientIO->runTransportLoop())`: event dispatch → C
   callbacks.
5. `sendToPeer(WireHello)` triggers the tail-window sync (`syncTailCount`) / incremental
   replay; `WireGetModel` prefetches model info (`EVT_MODEL_INFO`).

Stop sequence (`stopInternal`, runs on the finish thread):

1. `runtime_->stop()`: fail parked interrupts + flush batching + close transport + stop the
   server endpoint
2. Client-io barrier (within 5s): settle every still-pending call as `canceled` — those
   replies must precede `EVT_STOPPED`
3. Wait for `serverIO->running() == false` (up to 20s)
4. Remove the log sink → `agent_->shutdownAsync` (plugin shutdown, 25s) → stop + join the
   server io thread
5. Stop + join the client io thread
6. Explicitly release `clientIO_` / `runtime_` / `agent_` / `calls_` so channels die before
   their `io_context`
7. Move to `Stopped` → emit `EVT_STOPPED` (the very last event)

## 3. Thread Model Characteristics

- **Two fully decoupled IO threads**: each runtime owns one server-io thread and one
  client-io thread; the finish thread exists only while stopping.
- **Agent core protected**: slow host code inside `on_event` runs on the client-io thread
  only, and never blocks the ReAct loop, LLM streaming or tool execution.
- **Strong instance isolation**: concurrent runtimes are fully orthogonal (no global locks).
- **No cross-thread waiting on the calling thread**: `agentxx_ffi_call`, `call_cancel` and
  `stop` all return as soon as they are accepted, so they are safe to call from inside an
  event callback (the v3 synchronous queries deadlocked there).
- **Any thread**: `call` / `call_cancel` / `stop` / `drain_logs` / `get_capabilities` may be
  called from any thread. Only `destroy` requires "not an internal thread" (event callback /
  finish thread) and the state to be Created or Stopped.

## 4. C API Contract (`agent/lib/include/agentxx/ffi_api.h`)

### 4.1 Core Data Structures & Calling Conventions

```c
#pragma pack(push, 8)

// Read-only string view (borrowed ownership, 8-byte aligned)
typedef struct AgentxxStringView {
    const char* data;
    uint64_t    size;
} AgentxxStringView;

// Cross-CRT heap string (explicit ownership; free with agentxx_ffi_string_free)
typedef struct AgentxxString {
    char*    data;
    uint64_t size;
} AgentxxString;

// Error codes (int32_t)
#define AGENTXX_FFI_OK              0
#define AGENTXX_FFI_ERR_INVALID    -1   /* bad argument (NULL handle/empty/unknown op/type) */
#define AGENTXX_FFI_ERR_STATE      -2   /* state not allowed (not started/stopped/destroy) */
#define AGENTXX_FFI_ERR_JSON       -3   /* JSON parse failed */
#define AGENTXX_FFI_ERR_CONFIG     -4   /* bad configuration (missing model/field type) */
#define AGENTXX_FFI_ERR_INIT       -5   /* agent init failed */
#define AGENTXX_FFI_ERR_INTERRUPT  -6   /* interrupt id invalid/answered/expired */
#define AGENTXX_FFI_ERR_TIMEOUT    -7   /* call timeout (also appears inside EVT_RESULT) */
#define AGENTXX_FFI_ERR_OOM        -8
#define AGENTXX_FFI_ERR_CANCELED   -9   /* canceled by call_cancel / still pending at stop */
#define AGENTXX_FFI_ERR_REJECTED  -10   /* server rejected (error.name says why) */
#define AGENTXX_FFI_ERR_INTERNAL  -99

// Event callbacks (struct_size decides which fields this version may look at)
typedef struct AgentxxFFICallbacks {
    uint32_t struct_size;   /* = sizeof(AgentxxFFICallbacks) */
    uint32_t reserved;      /* must be 0 */
    void(AGENTXX_FFI_CALL* on_event)(int32_t type, AgentxxString* payload, void* user_data);
    void* user_data;
} AgentxxFFICallbacks;
```

- `AgentxxFFICallbacks` is copied by value and may be released right after `create`;
  `struct_size` must be `sizeof(AgentxxFFICallbacks)` and `reserved` must be 0 (otherwise
  `create` returns NULL).
- `on_event == NULL` means **headless**: no event serialization at all (no payload is
  allocated).

### 4.2 Call Model (submit + reply + cancel)

```c
uint64_t reqId = 0;
AgentxxString log = {NULL, 0};
AgentxxStringView req = {"{\"op\":\"model.list\",\"args\":{}}", 27};
int32_t rc = agentxx_ffi_call(agent, &req, &reqId, &log);   /* accepted immediately */
/* ... later on_event delivers EVT_RESULT {reqId, op, ok, result?|error?} ... */
```

- `rc == AGENTXX_FFI_OK` means **accepted**: the outcome arrives as an `EVT_RESULT` carrying
  that `reqId`.
- `rc != 0` is a synchronous failure (bad argument / bad state); there will be **no reply**,
  and `log` explains why.
- Any number of calls may be in flight on one handle; replies are matched by `reqId`.
- When "the previous step must be effective first", await its reply before submitting the
  next call. Concurrently submitted calls **do not complete in submission order**.
- `timeoutMs` defaults to the per-op value in the table (`0` = no limit); a timeout arrives
  as `ok:false, error:{code:-7, name:"timeout"}`.

Cancellation:

```c
agentxx_ffi_call_cancel(agent, reqId, &log);   /* any thread, idempotent */
```

- Stops **this wait only**; it does not retract the operation already handed to the server
  (use `input.cancel` to retract an input).
- The call settles immediately as `code=-9 canceled`; a late server reply is dropped (no
  second reply is produced).
- An unknown `reqId` (already done / never issued) returns `OK` with "already completed" in
  `log`, so races do not have to treat an idempotent cancel as an error.

### 4.3 Exported Symbol Inventory (15 symbols, see `agent/lib/ffi_symbols.map`)

| Group | Symbols | Notes |
|-------|---------|-------|
| Memory | `agentxx_ffi_malloc` / `agentxx_ffi_free` / `agentxx_ffi_string_free` / `agentxx_ffi_strdup_n` | The only allocation/free channel across the CRT boundary |
| Version | `agentxx_ffi_api_version` / `agentxx_ffi_library_version` | API version check / library version string-view out-parameter |
| Errors | `agentxx_ffi_strerror` | Error code → static string view |
| Capabilities | `agentxx_ffi_get_capabilities` | `{"apiVersion","libraryVersion","ops":[...],"features":[...]}`; works with a NULL handle too |
| Lifecycle | `agentxx_ffi_create` / `agentxx_ffi_start` / `agentxx_ffi_stop` / `agentxx_ffi_destroy` | Create (no threads) / async start (EVT_READY) / async stop (EVT_STOPPED) / destroy (pure release) |
| Calls | `agentxx_ffi_call` / `agentxx_ffi_call_cancel` | The only business entry point and the cancel entry point |
| Logs | `agentxx_ffi_drain_logs` | Drain buffered logs `[{"level","message"},...]` |

Version policy: `AGENTXX_FFI_API_VERSION` is **4**

- 1 → 2: the text parameter of `agentxx_ffi_send_input` became input JSON (attachments)
- 2 → 3: single-writer view history (wire v4), deltas addressed by message id
- 3 → 4: **the surface collapsed into "submit + reply"**: 35 symbols → 15; all synchronous
  queries, `send_wire` and `event_queue_*` removed; `call` / `call_cancel` and `EVT_RESULT` /
  `EVT_STOPPED` added; `on_event` payloads became callback-owned; `stop` became async and
  `destroy` became a pure release

Bindings should verify `agentxx_ffi_api_version() >= AGENTXX_FFI_API_VERSION` when loading.

### 4.4 Op Table (everything goes through `agentxx_ffi_call`)

"Completion" column: `ack` = server-side generic acknowledgment; `resp:<type>` = the matching
wire response; `local` = never touches the wire. "Default timeout" applies when the request
omits `timeoutMs` (`0` = no limit). The session scope is filled in by the FFI layer (do not
pass `sessionId` in `args`; `session.switch` is the exception).

| op | args | Completion | Default timeout | result / notes |
|---|---|---|---|---|
| `input.send` | `text` / `attachments` / `model` / `delivery` | `input_ack` | 10s | `{status, delivery, itemId?}`; `status=rejected` → `ok:false`, `error.name=<InputRejectReason>` |
| `input.cancel` | – | ack | 10s | Accepted cancel request (`WireCancel` only sets the flag) |
| `input.queue.clear` | – | ack | 10s | – |
| `input.queue.remove` | `itemId` | ack | 10s | Unknown item → `ok:false, error.name:"item_not_found"` |
| `input.queue.run_next` | – | ack | 10s | Interrupt the current turn and start the next queued input |
| `model.list` | – | resp:`model_info` | 10s | `{currentModel, models[], capabilities[]}` |
| `model.select` | `name` (empty = keep current) | ack | 10s | result `{model}`: the effective model name; unknown name → `error.name:"model_not_found"` |
| `model.add` | model config object (same fields as `create`'s `model_json`) | resp:`add_model_result` | 30s | Failure `code=-4` (it writes the yaml config) |
| `model.remove` | `name` | resp:`remove_model_result` | 30s | Removing the current model is rejected |
| `session.list` | `beforeMs?` / `beforeId?` / `limit?` (default 50) / `keyword?` | resp:`session_list` | 10s | `{sessions[], totalCount, hasMore}`; if the fetch fails (broken store / unreadable dir) the server replies with an empty list and logs the reason — **a reply always arrives** |
| `session.switch` | `sessionId` | ack | 60s | Acknowledged once the switch is **submitted** (result `{sessionId}`); the history `EVT_SYNC` / `EVT_MODEL_INFO` / `EVT_CONTEXT_STATS` were sent before it |
| `session.rename` | `title` | resp:`rename_session_result` | 10s | Empty title rejected |
| `session.compact` | – | ack | 0 (no limit) | Acknowledged **after** compaction really finished (result `{ok}`) |
| `context.get` | `tail?` (last N messages only) | resp:`context_messages` | 10s | `{messages[]}`; without `tail` the whole context is returned |
| `context.view_page` | `beforeIndex?` / `count?` | resp:`view_messages_page` | 10s | `{startIndex, totalCount, messages[]}` |
| `components.get` | – | resp:`append_component_info` | 10s | `{notifications[]}`; still fetched automatically at startup (→ `EVT_COMPONENTS`) |
| `permission.get_state` | – | resp:`permission_state` | 10s | `{fullAuth}` |
| `permission.set_full_auth` | `fullAuth` | resp:`permission_state` | 10s | The reply carries the **effective** state; without a permission middleware → `error.name:"rejected"` |
| `fs.list_dir` | `path?` / `allowedExtensions?` | resp:`list_dir_result` | 30s | For cross-device attachment picking |
| `interrupt.respond` | `interruptId` / `values` (object) | ack | 10s | Unknown/answered/expired interrupt → synchronous `code=-6`; server-side "not found" → `error.name:"interrupt_not_found"` |
| `hosttool.register` | `tools[]` (same entries as `tool_register`) | ack | 10s | result `{registered[], failed[{name,error}]}` |
| `hosttool.unregister` | `names[]` (or a single `name`) | ack | 10s | result `{removed[]}` |
| `hosttool.respond` | `callId` / `ok` / `result` or `error` | ack | 10s | Unknown callId → `error.name:"call_not_found"` |
| `plugin.data_up` | `topic` / `data` | ack | 10s | Acknowledged once published to the agent bus |
| `delta.set_batch` | `maxDelayMs` | local | – | `{maxDelayMs}`; `0` disables batching |
| `language.get` | – | local | – | `{language}` |
| `language.set` | `language` | local | – | `{language}` (normalized); posted to the server io to set the session language |

Ops completed by `resp:*` / `ack` emit **only one `EVT_RESULT`**; they no longer emit a
`EVT_MODEL_INFO`-style broadcast event. Broadcasts exist only for downlink messages with
`reqId == 0` (other clients' requests, server-initiated pushes).

### 4.5 Event Types (`AgentxxFFIEventType`)

Payloads are `AgentxxString*` JSON and **owned by the callback**: struct and body are one
heap block, released with `agentxx_ffi_free(payload)` after reading the body
(not `agentxx_ffi_string_free`).

| Event | Payload | Notes |
|-------|---------|-------|
| `EVT_READY` | `{"sessionId"}` | Server ready; calls and inputs may be submitted |
| `EVT_STOPPED` | `{}` | Teardown finished — **the last event**, no callback follows |
| `EVT_RESULT` | `{reqId, op, ok, result?}` or `{reqId, op, ok:false, error:{code,name,message}}` | Call reply (exactly one per accepted reqId) |
| `EVT_ERROR` | `{code, name?, message}` | Internal error unrelated to a specific call |
| `EVT_SYNC` | wire sync JSON | Full / tail-window history sync (`fromIndex` / `totalMessages`) |
| `EVT_DELTA` | wire delta JSON | Streaming delta: message-level ops (stream_begin/text_token/thinking_token/insert_message/update_message/stream_end, all carrying `msgId`) plus UI-state ops; a tool call is an ordinary history row |
| `EVT_DELTA_TEXT_BATCH` | `{"msgId","kind","text"}` | Batched text/thinking delta (only with `delta.set_batch(>0)`) |
| `EVT_TURN_END` | wire turn_result JSON | Turn finished (`hasError` reports async errors) |
| `EVT_CONTEXT_STATS` | wire context_stats JSON | Context token stats + session totals (`promptTokens`/`completionTokens`/`totalTokens`/`cachedPromptTokens`) + session title (`sessionTitle`) |
| `EVT_MODEL_INFO` | wire model_info JSON | Model-state **broadcast** (after a session switch or model add/remove); its `capabilities` describe per-model multimodal input |
| `EVT_COMPONENTS` | wire append_component_info JSON | Startup components (MCP/Skill/Memory/plugins) |
| `EVT_MESSAGE_QUEUE` | wire message_queue_update JSON | Pending-input queue changes `{sessionId, items[], state?}` |
| `EVT_PERMISSION_STATE` | `{"fullAuth"}` | Full-authorization **broadcast** |
| `EVT_INTERRUPT_REQ` | `{"interruptId","sessionId","node","value","argJson"}` | HIL interrupt; `argJson` is a serialized `InterruptHandleArg` (`{name,arg,resultId,ui}`), rendering guide in 4.7 |
| `EVT_INTERRUPT_EXPIRED` | `{"interruptId"}` | Interrupt expired/canceled, cannot be answered anymore |
| `EVT_HOST_TOOL_CALL` | `{"callId","name","argsJson","sessionId","timeoutSec"}` | The server asks the host to run a registered host tool (see 4.8) |
| `EVT_HOST_TOOL_CANCELED` | `{"callId","reason"}` | Host-tool call canceled (unanswered calls are reported on stop/destroy) |
| `EVT_PLUGIN_DATA` | wire plugin_data JSON | Agent-side plugin event forwarding (`{topic,data}`) |

### 4.6 Configuration and Model JSON (`agentxx_ffi_create` parameters)

```c
// config_json (may be NULL; unknown fields are ignored):
{ "dataDir": "~/.agentxx",            // empty = no persistence (default)
  "workDir": "/abs/project/dir",      // session working directory; empty = process cwd.
                                      // Applies to: the permission Ask allow-range /
                                      // filesystem & permission relative-path base /
                                      // child-process initial dir / plugin projectRoot.
                                      // Supports ~ and relative paths (resolved against
                                      // the process cwd); multiple handles may bind
                                      // different project directories
  "enableSessionStore": false,
  "sessionStoreDirectory": "",        // empty → {dataDir}/sqlite/sessions/
  "syncTailCount": 200,               // first screen syncs only the last N view messages
                                      // (0 = full sync; older history via context.view_page)
  "permissionMode": "ask",            // ask|all_ask|pass|deny (same as yaml permission.mode;
                                      // ask = ALLOW inside the working dir, INTERRUPT elsewhere)
  "permissionAllowPaths": ["..."],    // permission allow list
  "permissionDenyPaths": ["..."],     // permission deny list
  "skills": ["..."],                  // skill directories
  "memoryFiles": ["..."],             // context files
  "mcpServers": {"ns": {"url": "...", "timeoutSec": 120}},
  "plugins": [{"path":"...", "enabled":true, "sides":"agent|client|auto", "args":{}}],
  "llmMaxRetry": 5,
  "agentName": "Agentxx",
  "language": "zh-cn",                // UI/session language (default en)
  "devMode": false,
  "interruptTimeoutSec": 0 }          // HIL answer timeout, 0 = unlimited (default)

// model_json (recommended; isValid: baseUrl non-empty or apiKey != "EMPTY"):
{ "name": "display name", "type": "openai|anthropic|openai-responses",
  "baseUrl": "...", "apiKey": "...", "modelName": "(request model field)",
  "apiPath": "", "connectTimeoutSeconds": 16, "readChunkTimeoutSeconds": 100,
  "sslVerify": true|null, "maxConcurrentConnections": 5,
  "anthropicVersion": "2023-06-01", "modelContextMaxToken": 0,
  "imageInput": false, "audioInput": false, "videoInput": false,
  // ↑ multimodal input capability (mirrors yaml image_input/audio_input/video_input;
  //   the underscore spelling is accepted too): exposed through the `capabilities`
  //   of model.list / EVT_MODEL_INFO so the host can decide whether to offer
  //   image/audio/video entry points
  "extraHeaders": {"k":"v"}, "extraConfig": {} }
```

### 4.7 Interrupt UI Descriptor Rendering Guide (zero-semantics host rendering)

`EVT_INTERRUPT_REQ.argJson.ui` is a **self-contained form descriptor**: the host does not need
to know any specific question kind (permission, repeat prompt, future additions) — render the
block types below in order. Answer through `interrupt.respond` with
`{"interruptId":N,"values":{"<control id>": value}}` (the FFI layer wraps it into the
`{"values": {...}}` shape the server expects).

> `version` is currently 1 (first schema). **No backward compatibility is kept**: a missing or
> invalid descriptor is a contract error (show a notice, stay non-interactive); unknown block
> kinds/fields are ignored or degraded to their `fallback` text (forward compatible — do not
> raise errors).

**Shape**

```
ui = { "version": 1,
       "header": { "segments": [ {"text","labelKey","color","bold","dim"}, ... ] },  // empty = default prefix
       "blocks": [ ... ] }                                    // rendered in order (content and control blocks mixed)
```

**Content blocks (`blocks[].kind`)**

| kind | fields | rendering |
|------|--------|-----------|
| `text` | `text`/`textKey`, `color`, `bold`, `dim`, `wrap`, `indent` | A text line (`wrap` = hard wrap by width; empty text is not rendered) |
| `markdown` | `text`, `indent` | Markdown rich text (hosts without markdown may print the raw text) |
| `diff` | `path`, `oldStr`, `newStr` | Diff view (may degrade to a unified diff) |
| `image` | `dataUrl` or `path`, `alt`, `maxHeight` | Image preview (hosts without images degrade to the `alt` line) |
| `progress` | `value`, `total`, `label`/`labelKey`, `text`/`textKey` | Progress bar (`total == 0` = indeterminate; may degrade to `value/total` text) |
| `separator` | `indent` | Separator line |
| `gap` | `lines` | Blank lines |
| `custom` | `component`, `props`, `fallback` | **Reserved**: client-defined component; hosts without it render `fallback` |

**Control blocks (`blocks[].kind == "control"`)**

| `control` | fields | rendering | result value |
|-----------|--------|-----------|--------------|
| `buttons` | `options[{value,label,labelKey,color}]`, `defaultValue`, `commitOnPick` | Button row; `commitOnPick=true` submits the whole form on click | Chosen `value` (raw JSON) |
| `select` | `options`, `defaultValue` | Vertical single-select list | Chosen `value` |
| `checkbox` | `label`/`labelKey`, `defaultValue`(bool) | Check row | Boolean |
| `text` | `label`/`labelKey`, `help`/`helpKey`, `defaultValue`, `multiline`(reserved) | Text input | String |
| `number` | `defaultValue`, `integer`, `min`, `max`, `step` | Numeric control | Number (integer when `integer=true`) |
| `path` | `label`/`labelKey`, `mode`(`file`/`dir`/`multi`), `filter`, `defaultValue` | Path picker (degrades to text input) | Path string (array when multi) |

Common control fields: `id` (result key, unique within one descriptor; defaults to "value"),
`label`/`labelKey`, `help`/`helpKey`, `indent`.

**Submit row (`blocks[].kind == "submit"`**

`label`/`labelKey` = confirm button (default "confirm"), `cancelLabel`/`cancelLabelKey` =
cancel. Confirm submits the whole form; cancel cancels the interrupt request.

- `color` is one of `error`/`accent`/`hint`/`normal`/`thinking`/`tool`; map it to your theme
- The result is always an object `{"values": {"<control id>": value}}`; **an empty object means
  "not submitted / canceled"** (treated as unanswered)
- Content and control blocks may be interleaved freely; unknown block kinds are ignored

**Example (permission answer with remember)**

```jsonc
// EVT_INTERRUPT_REQ.argJson.ui (excerpt; produced by preset::permissionCard)
// Fixed copy only carries i18n keys (text/label/help are empty), hence no literal text here
{ "version": 1,
  "header": { "segments": [ {"labelKey":"interrupt.permissionBadge",
                             "color":"error","bold":true},
                            {"text":"read_file","color":"accent","bold":true},
                            {"text":" filesystem_read","color":"hint"} ] },
  "blocks": [ {"kind":"text","text":"• /workspace/data/x.txt","color":"hint","wrap":true,"indent":2},
              {"kind":"gap"},
              {"kind":"control","id":"remember","control":"checkbox",
               "labelKey":"interrupt.remember","defaultValue":false},
              {"kind":"control","id":"fullAuth","control":"checkbox",
               "labelKey":"interrupt.fullAuth","defaultValue":false},
              {"kind":"gap"},
              {"kind":"control","id":"decision","control":"buttons",
               "options":[{"value":"true","labelKey":"interrupt.allow"},
                          {"value":"false","labelKey":"interrupt.deny",
                           "color":"error"}],
               "defaultValue":"false","commitOnPick":true} ] }

// Host answer (allow + remember this choice):
{"op":"interrupt.respond","args":{"interruptId":1,
  "values":{"decision":true,"remember":true}}}
```

> **Copy convention**: fixed copy generated by the built-in presets declares **only i18n keys**
> (`labelKey`/`helpKey`/`textKey`) and leaves literal text empty — the client word list owns
> the wording. Producer-defined text has no key and is rendered literally (a missing key falls
> back to the literal text). Hosts without a word list may use the key names directly; the key
> names are a stable contract.

> `remember: true` means "remember this decision": the permission middleware registers an
> allow/deny rule for the current target, so later accesses to it are decided by the rule
> without asking. For a directory target (the described path ends with a slash) allowing also
> grants every sub-directory and file — the producer then appends a `textKey =
> "interrupt.rememberDir"` scope line after the target line.

**Example (multi-control form + submit row)**

```jsonc
// control ids: path / count / force
{"values":{"path":"/tmp/a.txt","count":4,"force":true}}
```

Pseudocode (any language): iterate `blocks`, render content blocks directly, render `control`
blocks by their `control` field while keeping an `id → value` map; on confirm (or a
`commitOnPick` button) encode the map as `{"interruptId":N,"values":{...}}` and pass it to
`agentxx_ffi_call`; on cancel send `{"values":{}}`.

> Reference implementations: the console host in `agent/example/ffi/dart/` (a minimal
> question-per-control form); the full descriptor-driven renderer is the TUI `InterruptView`
> (`agent/client/.../components/interrupt_view.cpp`).

### 4.8 Lifecycle & Host Tools

**Lifecycle state machine**

```
Created ──start()──► Starting ──ready──► Ready ──stop()──► Stopping ──done──► Stopped
                        └──init failed──► Failed ──stop()──► Stopping
```

- `create`: any thread, synchronous (parse config + construct the agent; no threads started)
- `start`: accepted immediately; readiness arrives as `EVT_READY`, failure as `EVT_ERROR`
- `stop`: accepted immediately (any thread, including inside an event callback); `EVT_STOPPED`
  is emitted when teardown finishes. It is **idempotent** (repeat calls emit nothing);
  calls submitted while Starting/Ready are still accepted, after Stopping they return
  `ERR_STATE`
- `destroy`: succeeds **only in Created / Stopped**, and is a pure release (it never blocks on
  teardown). Other states return `ERR_STATE` with "stop first and wait for EVT_STOPPED";
  calling it from an internal thread (event callback / finish thread) is rejected as well.
  Calls still pending during teardown are settled as `canceled` (before `EVT_STOPPED`)

**Host tools**

```
hosttool.register {tools:[...]}        → registered into the server-side ToolRegistry
input.send(...)                        → the model decides to call a host tool
EVT_HOST_TOOL_CALL {callId,name,argsJson,sessionId,timeoutSec}
hosttool.respond {callId,ok,result}    → result goes back, the server coroutine continues
```

- Each tool needs `name` (required) and may set `description` / `inputSchema` (JSON Schema) /
  `timeoutSec` (0 = no limit) / `maxConcurrent`; the reply is
  `{registered[], failed[{name,error}]}`
- Registering a name that already exists unregisters the old one first
- Exceeding `timeoutSec` makes the server treat the call as a failed tool result and continue
  the turn, so the session is never stuck
- When the client disconnects, the host tools it registered are unregistered; on
  `stop` / `destroy` every unanswered call is reported as
  `EVT_HOST_TOOL_CANCELED {callId, reason}`
- Host tools live in the `ToolRegistry` behind a `HostTool` wrapper; a call sends
  `host_tool_call` to the registering client and awaits the answer

### 4.9 Delta Batching (`delta.set_batch`)

The only genuinely high-frequency path across the language boundary is per-token text.
`delta.set_batch(maxDelayMs)` accumulates text/thinking tokens of one stream within the window
and emits a single `EVT_DELTA_TEXT_BATCH {msgId, kind, text}`:

```
delta.set_batch {maxDelayMs: 24}      → enable batching (16~33ms recommended)
EVT_DELTA_TEXT_BATCH {kind,text} × N  → append the text directly
delta.set_batch {maxDelayMs: 0}       → disable (default: off, one event per delta)
```

- **Off by default**: batching reduces cross-boundary events for high-rate streams, but text
  then arrives in windowed chunks — do not enable it when you need "all deltas before a reply
  are already delivered" ordering.
- Batching **does not change ordering semantics**: tool start/end, turn end and interrupts
  remain separate, strictly ordered events. When such an event arrives the pending text is
  flushed first, then the event is emitted in order.
- Thinking and body text are batched separately (`kind: "thinking"` / `"text"`).
- Pending text is flushed on turn end / stop / destroy, so nothing at the tail is lost.

### 4.10 User Input (`input.send` args)

```json
{ "text": "look at this picture",         // text and attachments: at least one non-empty
  "attachments": [                        // optional; at most 5 per message
    { "type": "image",                    // image|audio|video (alias: kind)
      "display_name": "diagram.png",      // alias: name
      "mime_type": "image/png",           // alias: mimeType
      "data_url": "data:image/png;base64,...",  // alias: dataUrl; inline the bytes
      "path_or_url": "/abs/photo.png",    // alias: path; either this or data_url
      "size_bytes": 68 }                  // alias: size
  ],
  "model": "m2",                          // optional; empty = keep the current model
  "delivery": "next-turn" }               // optional; empty = next-turn
```

Limits and rejections (same rules as the wire side; exceeding a limit is a **synchronous**
failure `AGENTXX_FFI_ERR_INVALID` and `log` names the offending item):

| Item | Limit | Notes |
| --- | --- | --- |
| Attachments per message | 5 | Rejected outright, the turn does not start |
| Image | 10 MB | Uses `size_bytes`; falls back to the Base64 length of `data_url`, then to the local file size |
| Audio | 25 MB | Same |
| Video | 50 MB | Same |

- When `data_url` is empty and `path_or_url` is a local path, **the server reads and encodes
  the file**, so the host does not have to (cross-device setups pick a server file through
  `fs.list_dir` and pass only the path)
- Attachments are merged into `ChatMessage`'s `image_urls` / `audio_urls` / `video_urls` by type
- The acceptance result is the `input.send` reply itself: `{status, delivery, itemId?}` with
  `status` being `started|queued|steered|rejected`; rejection reasons follow
  `InputRejectReason` (`empty_content` / `attachment_too_large` / `too_many_attachments` /
  `session_mismatch` / `server_stopped` / `bad_delivery` / `queue_cleared`)
- Inputs with attachments are queued **next-turn** (the server loads and encodes them at turn
  start)
- Model capability: read `capabilities` from `model.list` / `EVT_MODEL_INFO` to know whether
  the current model accepts image/audio/video input before offering an entry point. The server
  also guards this: types the model does not declare are never forwarded (the body gets a
  "[attachment not sent to the model] ..." line keeping the file names)

### 4.11 Ordering, Timeouts and Cancellation

- **Ordering**: concurrently submitted calls complete in no particular order (each is
  acknowledged as soon as the server finishes it). To enforce order, await the previous reply
  before submitting the next call — that is the only reliable mechanism. `input.send` texts
  are enqueued in submission order regardless of reply order.
- **Timeouts**: each call's initial limit is `timeoutMs` (default from the op table). When it
  expires the FFI layer produces a `code=-7 timeout` reply itself and forgets the reqId, so a
  server reply arriving later is dropped. A timeout does **not** retract the submitted
  operation (an input still runs, nobody waits for its result).
- **Cancellation**: `call_cancel` only ends the wait, sharing the timeout path — so
  "timeout then cancel" and "cancel then timeout" both produce exactly one reply.
- **Teardown**: calls still pending during `stop` are settled as `canceled` (before
  `EVT_STOPPED`), hence nothing is left pending after `EVT_STOPPED`.
- **Calling from a callback**: `call` / `call_cancel` / `stop` / `drain_logs` are all safe
  inside an event callback (no cross-thread waiting). Do not block there for long: it blocks
  all event dispatch.

### 4.12 Performance Defaults and Recommendations

- `syncTailCount` defaults to 200: the first screen only syncs the last 200 view messages and
  older history is pulled with `context.view_page` (0 = full sync, which is much slower and
  larger on long sessions)
- Batching is off by default: per-token events cost a fixed amount across the language
  boundary, so enable `delta.set_batch {maxDelayMs: 24}` for high-rate streams
- headless (`callbacks == NULL`) does no event serialization at all; hosts that only need
  results and no UI should use that form
- Resident threads per runtime: client io + server io (plus the finish thread while stopping);
  logs are pumped once per second on the client io instead of a dedicated thread

## 5. Language Bindings & Examples

| Directory | Description |
|-----------|-------------|
| [`agent/ffi/dart/`](/agent/ffi/dart/) | Dart FFI bindings package: `ffigen.yaml` generates symbol definitions from `ffi_api.h` (`dart run ffigen --config ffigen.yaml`, output `lib/agentxx_ffi_bindings.dart`) |
| [`agent/ffi/dart_wire/`](/agent/ffi/dart_wire/) | Pure-Dart wire client package: full message model + JSON codec + WebSocket connection management (handshake/heartbeat/event stream/host-tool answers); does not depend on FFI or the native library |
| [`agent/example/ffi/dart/`](/agent/example/ffi/dart/) | Dart CLI example (`agentxx_dart_cli`): submit + `Future` reply wrapper / cancel tokens / event-stream rendering / HIL permission interaction / `/model` `/sessions` `/compact` `/batch` commands; includes a mock-LLM smoke check (`example/smoke_check.dart`); see its README |

Dart SDK usage (v4 shape):

```dart
final client = AgentClient();
client.events.listen((e) { /* ReadyEvent / ResultEvent / DeltaEvent / ... */ });
await client.start(configJson: cfg, modelJson: model, syncTailCount: 200);
await client.ready;

await client.sendInput('hello');                       // submit + wait for acceptance
final models = await client.listModels();              // submit + wait for the result
await client.enableDeltaBatch(maxDelayMs: 24);         // local op
final token = AgentxxCancelToken();
final pending = client.call('session.compact', cancel: token);
token.cancel();                                        // → AgentxxCanceledException

await client.stop();                                   // wait for EVT_STOPPED
await client.dispose();                                // destroy + release callbacks
```

Other languages follow the same pattern: resolve the whitelisted symbols with
dlopen/dlsym (or the platform equivalent), register `AgentxxFFICallbacks`, free each payload
right inside the callback (or copy what is needed), and turn `EVT_RESULT` back into the
language's Future/Promise by `reqId`. **Waiting synchronously for a native call to return a
result is not possible** — all waiting happens in the host's own event loop.

## 6. Testing

- `agent/test/test_ffi_c_api.cpp` (module `ffi_c_api`, 451 assertions): lifecycle + 17 cases
  covering 20 concurrent calls, cancel with late replies, timeouts, ordering, calling from a
  callback, payload ownership, concurrent handles and headless; a built-in mock LLM server
  drives the full flow
- Benchmark: the `agentxx_benchmark resource_ffi` scene (call throughput / batching comparison
  / first-screen tail window)

---

## 7. Implementation Notes

- **Direct channel**: the FFI layer reuses `ChannelAgentIOTransport::makePair` (zero
  serialization concurrent_channel), not WS; `SessionServerAgentIO` is the server endpoint and
  `FfiClientAgentIO` the client endpoint, exactly like the TUI/CLI transports
- **Working directory fallback**: `config_json.workDir` expands `~` / `${VAR}` and relative
  paths; when unset it falls back to the process `cwd`. Session-level worktree binding
  (`Session::WorktreeBinding`) and the multi-source fallback of
  `AgentContext::getSessionWorkDir` apply to FFI handles too
- **Plugin sides**: `plugins[].sides` is `auto` (default, decided by the presence of the
  `agentxx_client_plugin_create` export) / `agent` / `client` (FFI is usually `agent`)
- **Call table**: `CallRegistry` keeps `reqId → (op, deadline)`; timeouts use **one shared
  `steady_timer`** plus an index sorted by deadline (not one timer per call), and `settle`
  uses a CAS so exactly one reply is produced (whoever arrives first wins)
- **Reply delivery**: already on the client io thread (server reply / timeout / teardown) it is
  emitted inline; other threads (`call_cancel`, local ops) `asio::post` it — this is why
  "cancel/teardown replies precede `EVT_STOPPED`" needs no extra synchronization
- **`input.send` uses its FFI reqId as the wire `requestId`**, so the server's `input_ack` is
  matched directly and no second map is needed
- **HIL input descriptor**: `EVT_INTERRUPT_REQ.argJson` is a serialized `InterruptHandleArg`
  (`{name,arg,resultId,ui}`); `ui` is the mandatory interrupt UI descriptor (schema in
  `agent/middlewares/interrupt_ui.h`), rendering guide in 4.7
- **Cross-CRT heap**: every `AgentxxString` out-parameter and every event payload comes from
  `agentxx_ffi_malloc` and is released with `agentxx_ffi_string_free`;
  `agentxx_ffi_strdup_n` is the single copy helper
