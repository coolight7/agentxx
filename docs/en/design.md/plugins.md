> 文档自动翻译自[zh-cn](/docs/zh-cn/design.md/plugins.md)版 (This document is automatically translated from the [zh-cn](/docs/zh-cn/design.md/plugins.md) version.)

# Plugin System Development Guide

> Related: [design.md](index.md) (Core Architecture) · [ffi.md](ffi.md) (FFI) · Source: [agent/plugins/](/agent/plugins/) · C ABI Contracts: [plugin_api.h](/agent/lib/include/agentxx/plugin/api/plugin_api.h) / [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h) / SDK: [plugin_kit.h](/agent/lib/include/agentxx/plugin/api/plugin_kit.h)

---

## 1. Overview

The Agentxx plugin system is built on a **pure C ABI + COM-style interface table query** architecture:

- **Pure C Boundary**: Only pure C primitive types, function pointers, opaque handles, and `AgentxxPluginStringView` (data + size borrowed read-only view, NUL-termination not required) cross boundaries. Direct transfer of `std::string`, `std::vector`, `std::function`, or C++ exceptions is strictly forbidden.
- **Cross-Compiler / Cross-STL / Cross-Language Compatibility**: Host and plugins can be compiled independently using different compilers, different standard C++ libraries (libstdc++, libc++, MSVC STL), or entirely different programming languages, guaranteeing stable runtime binary compatibility.
- **Memory Ownership**: All cross-boundary heap allocations are strictly managed via `host->alloc/free` (the core vtable memory pair). The receiver is responsible for releasing memory using `host->free`. String cloning uses the header-inlined helper `agentxx_plugin_strdup(host, ...)`.
- **Native Coroutine Asynchrony**: Using `Task<T>` from `plugin_kit.h`, plugin coroutines execute directly within the host's IO thread. Coroutines yield cooperatively on suspension and wake up via IO thread callbacks. Host and plugin coroutines interleave cooperatively without thread locks, polling, or private event loops.
- **Single-Threaded Session State**: Mutable host session state is accessed serially only on the primary IO thread. Plugin registration and state queries are automatically posted back to the IO thread by the host when necessary, completely transparent to the plugin.

---

## 2. Core Architecture & Compatibility Standards

```
Host (libagentxx / agentxx_cli)
  Core vtable (Frozen) ── alloc / free / query_interface (IID → interface table)
                          │
         ┌────────────────┼────────────────┬─────────────────┐
         │ tools          │ hooks          │ events          │ scheduler   ...14 tables
         │ register/      │ 7 hook points  │ publish/        │ sleep/      capabilities/
         │ call_tool      │                │ subscribe       │ offload     session/plugins/
         └────────────────┘                └─────────────────┘             config/model/cancel/...
Plugin Shared Library (Any compiler) ── AGENTXX_PLUGIN_EXPORT ── PluginBase context ── SDK registry
```

- **Frozen Core vtable**: Contains only `alloc`, `free`, and `query_interface`. Will never be modified. All host capabilities are retrieved by querying dedicated interface tables via stable `IID` string identifiers (`AGENTXX_PLUGIN_QUERY_IFACE` macro).
- **Strict ABI Specification**:
  - 8-byte struct alignment: Headers uniformly wrap definitions in `#pragma pack(push, 8)` / `#pragma pack(pop)`.
  - Fixed-width primitive types: Naked `int`, `long`, and `size_t` are forbidden across boundaries; `int32_t`, `int64_t`, `uint64_t`, etc., are strictly required.
  - Explicit calling conventions: Exported symbols and function pointers crossing boundaries must carry the `AGENTXX_PLUGIN_CALL` macro (`__stdcall` on Windows, empty on x64 Unix).
  - Passing structs by pointer: Passing aggregate structs by value is prohibited. Input parameters must be passed by pointer (`const Struct*`); struct return values are converted into pointer out-parameters (`Struct* out`), with the function returning an `int32_t` status code (0 for success).
  - Streamlined core vtable: Removed the previous `strdup` slot in favor of a header-inlined `agentxx_plugin_strdup` based on `alloc`.
  - C++ helper convenience layer: `AgentxxPluginStringView` and `AgentxxPluginString` provide implicit `operator const T*()` address conversions and `.empty()` helpers, along with value-passing compatibility overloads and `agentxx_plugin_string_free` overloads.
- **Independent Interface Table Evolution**: The first field of each table is an `int32_t version` indicating its independent version number. Function pointers within tables may be `NULL` (indicating the host has not implemented that specific sub-capability; check before calling).
- **Version Gatekeeping**: Global `AGENTXX_PLUGIN_API_VERSION` and `AGENTXX_CLIENT_PLUGIN_API_VERSION` are reset to 1. An exact match is enforced upon load; mismatches are rejected. New features introduce new interface tables or append fields with incremented table versions, leaving the global version untouched.
- **Threading Rules**: `query_interface` and `alloc` can be called from any thread. Registration and IO-constrained operations (session, config, prompt) are dispatched internally by the host with synchronous waiting. The two-piece `start/cancel` operations are driven by the host on the IO thread (< ~1ms). `AgentxxPluginOperatorNotify.done` may be called from any thread. Host completion callbacks dispatched to the plugin (`AgentxxOpCb`, sleep/offload done) are guaranteed to be queued on the IO thread via `post`.

---

## 3. The Three Iron Rules of Multi-Instance Safety

A single plugin shared library may be loaded simultaneously by multiple independent Agent hosts within a single process, creating multiple concurrent instances (e.g. multiple FFI handles, AgentHost subagents):

1. **No Mutable Global / Function-Static State**: All mutable state must be encapsulated within a heap context object allocated per instance (`*plugin_ctx`, typically inheriting from `kit::PluginBase`).
2. **State Recovered via Context Closures**: All tools, hooks, and event callbacks must recover their instance context via `spec.user_data`.
3. **Cache Interface Tables in Instance Context**: Results of `AgentIfaces` queries must be stored as instance members, preventing interference across instances. Adapters for offload thread pool asynchronous interfaces (`plugin_tool_sync.h`) must be embedded within the caller's instance context and destroyed alongside the instance.

---

## 4. Exported Symbol Visibility

Plugin shared libraries hide all symbols by default, exporting only the entry-point symbols looked up by name by the host. Entry functions must be declared with `AGENTXX_PLUGIN_EXPORT` (inside `extern "C"`):

```c
#include "agentxx/plugin/api/plugin_api.h"
extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void);
extern "C" AGENTXX_PLUGIN_EXPORT int32_t agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx);
extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx);
```

- **Entry Symbol Sets**:
  - Agent side: `agentxx_plugin_agent_get_info` / `agentxx_plugin_agent_create` / `agentxx_plugin_agent_destroy`
  - Client side (Dual-sided or UI-only): `agentxx_plugin_client_get_info` / `agentxx_plugin_client_create` / `agentxx_plugin_client_destroy`
- **Automated Build Configuration**: `plugins/CMakeLists.txt` configures ELF `-fvisibility=hidden` + version script whitelists (wildcarding `agentxx_plugin_agent_*` / `agentxx_plugin_client_*`, allowing single-sided plugins to link under Android lld), macOS `-exported_symbols_list`, and MSVC `dllexport`. Third-party static library symbols are hidden automatically.

---

## 5. Tool Function Reuse (`agentxx_util`)

Built-in plugins can reuse all core utility functions (string manipulation, encoding detection, UTF-8 conversion, path normalization, Base64, HTTP, SQLite, regex, logging, etc.) via the standalone static library `agentxx_util`:

```cmake
find_package(agentxx_util REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE agentxx_util)
```

```cpp
#include "agentxx/util/string_util.h"
auto b64 = agentxx::util::base64Encode(data);
```

- `agentxx_util` is compiled from all source files in `agent/lib/src/util/`. Both `libagentxx` and individual plugins statically link their own copy; symbols are hidden via export visibility control without conflict. Dependencies are transitively propagated as `PUBLIC` (fmt, sqlite3, uchardet, iconv + neograph, yyjson, OpenSSL, hyperscan, uring).
- Intended as a convenience library for built-in plugins (built within the same superbuild with full dependencies). Third-party plugins only need the pure C header `plugin_api.h` / SDK `plugin_kit.h` without linking against host libraries.
- Unreferenced modules are automatically pruned based on object file extraction (9 built-in plugins have `DT_NEEDED` pointing only to system libraries).

---

## 6. C++ Plugin Development Workflow (SDK `plugin_kit.h`)

We recommend using the official header-only SDK `plugin_kit.h` (depends only on `plugin_api.h`):

```cpp
#include "agentxx/plugin/api/plugin_kit.h"
struct MyPluginCtx : public agentxx::plugin::PluginBase {};

extern "C" AGENTXX_PLUGIN_EXPORT int32_t agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    auto ctx = std::make_unique<MyPluginCtx>();
    ctx->init(host);

    // Coroutine tool anchored by Task (supports sleep / yield / call_tool / offload)
    agentxx::plugin::tool(*ctx, "my_async_tool", "desc", R"({"type":"object","properties":{}})",
        [](MyPluginCtx& c, std::string_view args_json, agentxx::plugin::OpCtl ctl) -> agentxx::plugin::Task<std::string> {
            co_await agentxx::plugin::sleep(c, 100);
            ctl.throw_if_cancelled();
            // Cross-plugin call: co_await agentxx::plugin::call_tool(c, "other_tool", "{}", threadId);
            co_return R"({"status":"ok"})";
        });

    // Fast synchronous inline tool (<~1ms, executed and returned directly on IO thread)
    agentxx::plugin::fast_tool(*ctx, "my_fast_tool", "desc", R"({"type":"object","properties":{}})",
        [](MyPluginCtx& c, std::string_view args_json, std::string_view tid) -> std::string {
            return R"({"result":42})";
        });

    // Blocking tool (automatically offloaded to host blockingPool, non-blocking to IO thread)
    agentxx::plugin::blocking_tool(*ctx, "my_blocking_tool", "desc", R"({"type":"object","properties":{}})",
        [](MyPluginCtx& c, std::string_view args_json) -> std::string {
            // Heavy computation / synchronous file I/O / blocking network calls
            return R"({"done":true})";
        });

    // Background cooperative task (Host-managed: auto-registered with agentxx.agent.tasks;
    // on plugin unload, the host cancels and cleanly waits for exit with no dangling frames)
    agentxx::plugin::spawn(*ctx, [](MyPluginCtx& c, agentxx::plugin::OpCtl ctl) -> agentxx::plugin::Task<void> {
        while (!ctl.cancelled()) {
            co_await agentxx::plugin::sleep(c, 5000);
            if (ctl.cancelled()) break;
            // Collect and publish events
        }
    });

    // Hooks (7 hook points: agent_start/end, model_start/run/end, tool_start/end)
    agentxx::plugin::hook(*ctx, AGENTXX_HOOK_MODEL_START, [](MyPluginCtx& c, std::string_view in){ /*...*/ });

    // Capabilities (Generic RPC channel across plugins)
    agentxx::plugin::capability(*ctx, "my.cap", [](MyPluginCtx& c, const AgentxxPluginHost* caller, std::string_view method, std::string_view args){ return "{}"; });

    *plugin_ctx = ctx.release();
    return 0;
}
extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    delete static_cast<MyPluginCtx*>(plugin_ctx);
}
```

**Unified Asynchronous Operation Model (Two-piece start/cancel)**: Tools, hooks, and capabilities all adhere to the `start` (non-blocking invocation on IO thread) + `cancel` (cooperative cancellation) lifecycle. Completion is reported exactly once via `AgentxxOpNotify.done(status, payload)`. `Task` coroutine frames are destroyed before invoking `done`, supporting `offload` blocking-pool delegation and `call_tool` / `invoke_cap` cross-plugin invocations.

**Host-Managed Background Task Spawning (`spawn`)**: Background cooperative tasks started via `spawn` (e.g. periodic collection `while(!cancelled()) { offload; sleep; }`) are registered to the host `agentxx.agent.tasks` interface table since API v1, managed isomorphically with tool/capability ops:

- **Registration**: `spawn()` automatically calls `register_task` (on the IO thread) → Host records the handle into the instance's `outstandingOps` (shared with tool ops) and holds an `inflight` reference.
- **Execution**: The coroutine suspends via `sleep`/`offload` through the host; transparent to the host.
- **Unload Coordination**: When the plugin unloads, the host's `detachAll` cancels all registered tasks (invoking the plugin's `cancel_fn`, setting `cancelFlag` and waking sleeping/offloaded tasks) → Coroutine exits `while(!cancelled())` loop → `finishIfDone` (after coroutine frame destruction, reports completion exactly once via `notify.done`) → Host decrements `inflight` (`guard.reset`) and releases handle → `waitInflightZero` cleanly awaits zero active operations → `dlclose` occurs safely without dangling frames or UAF.
- **Graceful Fallback**: If the host lacks the `agentxx.agent.tasks` table or registration fails, `spawn` degrades to an unmanaged coroutine (warns via log, cannot be cleanly reclaimed on unload)—a known constraint when running across mismatched versions.
- **Threading Rules**: `cancel_fn` is invoked by the host on the IO thread (cooperative); `notify.done` may be reported from any plugin thread (host handles atomic CAS in `OpCore::onDone` and posts back to IO); `kit` coroutine completion is guaranteed on the IO thread.

---

## 7. Plugin Classification & Compilation Modes

1. **By Functional Scope**:
   - **Agent Plugins**: Extends session execution flows (Tools, Hooks, Events, Capabilities, Resources).
   - **Client Plugins**: Extends TUI/CLI user interface (StatusItems, Panels, InfoSections, Commands, ToolDecors).
   - **Dual-Sided Plugins**: Exports both Agent and Client entry points in a single binary, servicing both sides simultaneously; cross-side communication utilizes Wire messages (`PluginData` agent→client / `PluginDataUp` client→agent).

2. **By Compilation & Distribution**:
   - **Standalone Shared Library (Default)**: Compiled as a separate dynamic shared library, dynamically loaded via the `plugins` array in `agentxx-config.yaml` based on `path` (supports directories containing `plugin.yaml` manifests).
   - **Monolithic Built-in Compilation**: Plugins specified in `AGENTXX_PLUGIN_BUILTIN_LIST` are compiled directly into `libagentxx`, providing zero-overhead in-process calls without external `.so`/`.dll` files. Configuration in `plugins` remains supported, with `path` specified as `builtin://<name>` or `name: <name>` (external path not required), and `config` specifying plugin configuration directory or file paths.

---

## 8. Agent-Side Interface Tables

| IID | Version | Capability |
|---|---|---|
| `agentxx.agent.tools` | 1 | `register_tool/unregister_tool`, `call_tool_async/op_cancel` (cross-plugin invocation; completion guaranteed posted to IO thread). |
| `agentxx.agent.hooks` | 1 | `register_hook/unregister_hook` (7 hook points, two-piece start/cancel lifecycle). |
| `agentxx.agent.events` | 1 | `subscribe/unsubscribe/publish` (topics automatically prefixed with `plugin.`, JSON payloads). |
| `agentxx.agent.capabilities` | 1 | `register_capability(_ex)/unregister/has_capability`, `invoke_capability_async/op_cancel`. |
| `agentxx.agent.scheduler` | 1 | `is_io_thread/post_to_io/pump_io`, `sleep/cancel_sleep`, `offload` (delegates to blocking pool, requires `cancel_flag`). |
| `agentxx.agent.session` | 1 | `get_share_store/add_share_store/emit_message_tip` (IO thread). |
| `agentxx.agent.plugins` | 1 | `list_plugins/get_plugin/get_own_info` (JSON). |
| `agentxx.agent.config` | 1 | `get_config/get_plugin_args/get_tool_prompt/get_session_work_dir/get_plugin_config_path` (`get_session_work_dir` returns default workdir when session ID is empty; `get_plugin_config_path` returns normalized absolute path configured via YAML `config`, pointing to a file or directory). |
| `agentxx.agent.model` | 1 | `get_config` (Active model and associated config JSON). |
| `agentxx.agent.cancel` | 1 | `is_cancelled(threadId)` (Advisory polling; authoritative notification comes via cancel callback). |
| `agentxx.agent.prompt` | 1 | `get_prompt/set_prompt` (Host prompt read/write access). |
| `agentxx.agent.json` | 1 | `json_get_string/json_escape`. |
| `agentxx.agent.log` | 1 | `log(level, msg)` (0: trace .. 4: error). |
| `agentxx.agent.resources` | 1 | `register_skill_dir/memory_file/mcp_server` (Initialization phase only) + `get_own_resources` (Immutable after freeze). |
| `agentxx.agent.graph` | 1 | Execution Graph Extensibility: `register_node_type/unregister_node_type` (Injects custom node types into per-agent `GraphRegistry`) + `get_graph_json/get_graph_name/set_graph_json` (Inspect/modify host execution graph, default name `agentxx.default`; active during plugin load, consumed prior to host engine construction). |
| `agentxx.agent.tasks` | 1 | Host-Managed Background Tasks: `register_task/cancel_task` (Auto-registered via kit `spawn`; host tracks handle, holds inflight count, and receives `notify.done`—on unload, host cancels via `detachAll` and cleanly awaits zero active ops via `waitInflightZero`; `notify` is an out-parameter whose `done` method can be invoked from any thread). |

---

## 9. Client-Side Interface Tables

| IID | Version | Capability |
|---|---|---|
| `agentxx.client.ui` | 1 | `register_status_item/update/unregister`, `register_panel/update/unregister`, `register_info_section/update/unregister`, `register_command/unregister`, `show_toast`, `update_tool_decor(tool_call_id, decor_json)`. |
| `agentxx.client.events` | 1 | `subscribe/unsubscribe` (See `AgentxxClientEvent`: `READY`, `CONN_STATE`, `USER_INPUT`, `DELTA`, `TURN_END`, `SESSION_SWITCH`, `PLUGIN_DATA`). |
| `agentxx.client.session` | 1 | `get_client_state` (snapshot JSON), `send_user_input`, `request_cancel`. |
| `agentxx.client.wire` | 1 | `send_plugin_data(event, json)` → Dispatched to server as `client.{plugin}.{event}`. |
| `agentxx.client.self` | 1 | `get_own_info/get_plugin_args/get_plugin_config_path` (Returns normalized absolute path configured via YAML `config`). |
| `agentxx.client.json` | 1 | `json_get_string/json_escape`. |
| `agentxx.client.log` | 1 | `log(level, msg)`. |

`update_tool_decor` allows plugins to drive tool call rendering decorations: Subscribing to `tool_start` in `EVT_DELTA` (containing full `arguments`), the plugin pushes `{displayName, summary, items[]}` JSON keyed by `tool_call_id` (items schema mirrors panel, with additional `diagram` kind). The host automatically manages cleanup and restoration during plugin unload/disable; a prominent example is `agentxx_planning` (where plan visualization is entirely plugin-driven).

---

## 10. Session Resource Contributions (Skills / Memory / MCP)

- **Declarative**: Resources declared alongside `plugin.yaml` in the plugin directory (applied via `AgentResourceApplier::applyDecls` upon successful entry, removed on unload/disable).
- **Programmatic**: Dynamically registered/unregistered at runtime via the `agentxx.agent.resources` interface table (e.g. `agentxx_codegraph` registering index paths based on configuration args).
- The host handles deduplication and lifecycle management for both declarative and programmatic resources (ownership semantics in `resource_applier.h`); failed items are tracked in `AppendComponentNotification` for UI reporting.

### Plugin Configuration Path (`config` field)

- Each plugin entry in `agentxx-config.yaml` can specify a configuration directory or file path via `config` (supports `~`, `${VAR}`, and relative paths, normalized to absolute paths by the host).
- Plugins query this path via `get_plugin_config_path` in `agentxx.agent.config` (agent side) or `agentxx.client.self` (client side), returning `NULL` when unconfigured. Plugins can determine file type and load accordingly (e.g. scanning `*.yaml` files or reading a specific file).
- Typical usage: `config: ${AGENTXX_WORK_DIR}/config/my_plugin.yaml` or `config: ./my_plugin_config/` (relative to work directory); wrapped conveniently as `PluginBase::configPath()` in the SDK.

---

## 11. Worktrees & Session Working Directories

- Plugins query the currently active session working directory via `AgentxxConfigIface::get_session_work_dir(host, thread_id)` (session worktree binding takes precedence; returns default working directory when `thread_id` is empty).
- The `workDir` argument for `blocking_tool` is prefetched and injected on the IO thread by the SDK, avoiding cross-thread `ioCallSync` in worker threads.
- Filesystem and command execution plugins dynamically resolve paths using the injected `sessionId` on each invocation; relative path references switch immediately upon session binding.

---

## 12. JavaScript Plugins (Powered by QuickJS Engine Plugin)

Agentxx maintains a single unified C++ plugin infrastructure. JavaScript script plugins are hosted via the built-in `agentxx_javascript_engine` plugin:

- **Unified Plugin Model**: All plugins are fundamentally C++ plugins; a JS plugin is packaged as a standard C++ dynamic library shell (e.g. `example_js`) bundled with `plugin.js`.
- **Execution Flow**: Host loads the JS plugin shell → Shell invokes `interpreter.js` capability during `create` to pass `plugin.js` to the QuickJS engine → Engine parses and runs the script on a dedicated worker thread, registering tools and hooks declared in JS back to the host.
- Custom script engines (Python, Lua, etc.) can be developed similarly to extend scripting capabilities.

---

## 13. Plugin Reference & Examples

| Plugin | Description |
|---|---|
| `example_plugin` | Comprehensive native C++ example (fast_tool, Task coroutines, call_tool, sleep, hooks, events, capabilities, client entry). |
| `example_js` | JavaScript script plugin example (C++ shell wrapper + `plugin.js`). |
| `example_resources` | Session resource contribution example (declarative & programmatic MCP, Skills, rules, session environments). |
| `agentxx_filesystem` | Filesystem tools (list, read, write, edit, glob, grep; includes unit-tested `*_impl.h`). |
| `agentxx_execute_command` | Command execution tools (bash, windows; includes timeout handling and PowerShell detection). |
| `agentxx_websearch` | Web search and retrieval tools (search, fetch, fetch_markdown). |
| `agentxx_rag_search` | Vector semantic search. |
| `agentxx_string` | String tools (html_to_markdown, regexp). |
| `agentxx_system` | System clock tool (`get_current_datetime`). |
| `agentxx_system_monitor` | System resource monitor (tool + background periodic sampling + client Info/Status bar rendering). |
| `agentxx_planning` | Task planning tool + client-side Plan visualization decor. |
| `agentxx_math` | Math computation tool (`agentxx_math_calculate`; supports arithmetic, powers, factorials, bitwise, logic, trig, hyperbolic, log, combinations/permutations, implicit multiplication). |
| `agentxx_codegraph` | Code index & navigation (8 tools + client Info panel). |
| `agentxx_screen_capture` | Screen capture (Windows only). |
| `agentxx_computer_use` | Mouse and keyboard control (Windows only; depends on `screen_capture`). |
| `agentxx_audio_stream` | Audio stream capture (Windows WASAPI only). |
| `agentxx_text_selection_monitor` | Text selection event listener (Windows UIAutomation only). |
| `agentxx_javascript_engine` | QuickJS execution engine (exports `interpreter.js` capability). |
| `agentxx_execute_javascript` | JS code execution tool (`agentxx_execute_javascript`; depends on `agentxx_javascript_engine`). |

---

## 14. Build System & Platform Support

- **Platform Matrix**: Each plugin determines platform compatibility at the start of its `CMakeLists.txt` via the `gate` function in `plugin_platform_support.cmake`, leveraging top-level `XX_IS_*_D` flags. Unsupported platforms are skipped during compilation (`screen_capture`, `computer_use`, and `text_selection_monitor` are Windows only; `audio_stream` is not yet implemented across platforms, etc.).
- **Monolithic Built-in Compilation**: Plugins specified in `AGENTXX_PLUGIN_BUILTIN_LIST` are merged into `libagentxx`. In this mode, `test_ffi_c_api` and `client_plugins` tests conditionally bypass dynamic library path checks.
- **Artifact Layout**: Standalone shared libraries output to `{build}/exec/plugins/<plugin_name>/` (organized into subdirectories when accompanied by a `plugin.yaml` manifest).
