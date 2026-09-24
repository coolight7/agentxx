# Plugin System Development Guide

> Related: [design](index.md) (Core Architecture) · [ffi.md](ffi.md) (FFI) · Source: [agent/plugins/](/agent/plugins/) · C ABI Contracts: [plugin_api.h](/agent/lib/include/agentxx/plugin/api/plugin_api.h) / [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h) / SDK: [plugin_kit.h](/agent/lib/include/agentxx/plugin/api/plugin_kit.h)

---

## 1. Overview

The Agentxx plugin system is built on a **pure C ABI + COM-style interface table query** architecture:

- **Pure C Boundary**: Only pure C primitive types, function pointers, opaque handles, and `PluginxxStringView` (data + size borrowed read-only view, NUL-termination not required) cross boundaries. Direct transfer of `std::string`, `std::vector`, `std::function`, or C++ exceptions is strictly forbidden.
- **Cross-Compiler / Cross-STL / Cross-Language Compatibility**: Host and plugins can be compiled independently using different compilers, different standard C++ libraries (libstdc++, libc++, MSVC STL), or entirely different programming languages, guaranteeing stable runtime binary compatibility.
- **Memory Ownership**: All cross-boundary heap allocations are strictly managed via `host->alloc/free` (the core vtable memory pair). The receiver is responsible for releasing memory using `host->free`. String cloning uses the header-inlined helper `pluginxx_strdup(host, ...)`.
- **Native Coroutine Asynchrony**: Using `Task<T>` from `plugin_kit.h`, plugin coroutines execute directly within the host's IO thread. Coroutines yield cooperatively on suspension and wake up via IO thread callbacks. Host and plugin coroutines interleave cooperatively without thread locks, polling, or private event loops.
- **Single-Threaded Session State**: Mutable host session state is accessed serially only on the primary IO thread. Plugin registration and state queries are automatically posted back to the IO thread by the host when necessary, completely transparent to the plugin.

---

## 2. Core Architecture & Compatibility Standards

```
Host (libagentxx / agentxx_cli)
  Core vtable (Frozen) ── alloc / free / query_interface (IID → interface table)
                          │
         ┌────────────────┼────────────────┬─────────────────┐
         │ tools          │ hooks          │ events          │ scheduler   ...18 agent + 9 client tables
         │ register/      │ 7 hook points  │ publish/        │ sleep/      capabilities/
         │ call_tool      │                │ subscribe       │ offload     session/plugins/
         └────────────────┘                └─────────────────┘             config/model/cancel/...
Plugin Shared Library (Any compiler) ── PLUGINXX_EXPORT ── PluginBase context ── SDK registry
```

- **Frozen Core vtable**: Contains only `alloc`, `free`, and `query_interface`. Will never be modified. All host capabilities are retrieved by querying dedicated interface tables via stable `IID` string identifiers (`AGENTXX_PLUGIN_QUERY_IFACE` macro).
- **Strict ABI Specification**:
  - 8-byte struct alignment: Headers uniformly wrap definitions in `#pragma pack(push, 8)` / `#pragma pack(pop)`.
  - Fixed-width primitive types: Naked `int`, `long`, and `size_t` are forbidden across boundaries; `int32_t`, `int64_t`, `uint64_t`, etc., are strictly required.
  - Explicit calling conventions: Exported symbols and function pointers crossing boundaries must carry the `PLUGINXX_CALL` macro (`__stdcall` on Windows, empty on x64 Unix).
  - Passing structs by pointer: Passing aggregate structs by value is prohibited. Input parameters must be passed by pointer (`const Struct*`); struct return values are converted into pointer out-parameters (`Struct* out`), with the function returning an `int32_t` status code (0 for success).
  - Streamlined core vtable: Removed the previous `strdup` slot in favor of a header-inlined `pluginxx_strdup` based on `alloc`.
  - C++ helper convenience layer: `PluginxxStringView` and `PluginxxString` provide implicit `operator const T*()` address conversions and `.empty()` helpers, along with value-passing compatibility overloads and `pluginxx_string_free` overloads.
- **Independent Interface Table Evolution**: The first field of each table is an `int32_t version` indicating its independent version number. Function pointers within tables may be `NULL` (indicating the host has not implemented that specific sub-capability; check before calling).
- **Version Gatekeeping**: Global `PLUGINXX_API_VERSION` and `AGENTXX_CLIENT_PLUGIN_API_VERSION` are reset to 1. Host requires plugin api_version >= host version upon load; lower versions are rejected. New features introduce new interface tables or append fields with incremented table versions, leaving the global version untouched.
- **Threading Rules**: `query_interface` and `alloc` can be called from any thread. Registration and IO-constrained operations (session, config, prompt) are dispatched internally by the host with synchronous waiting. The two-piece `start/cancel` operations are driven by the host on the IO thread (< ~1ms). `PluginxxOperatorNotify.done` may be called from any thread. Host completion callbacks dispatched to the plugin (`AgentxxOpCb`, sleep/offload done) are guaranteed to be queued on the IO thread via `post`.

---

## 3. The Three Iron Rules of Multi-Instance Safety

A single plugin shared library may be loaded simultaneously by multiple independent Agent hosts within a single process, creating multiple concurrent instances (e.g. multiple FFI handles, AgentHost subagents):

1. **No Mutable Global / Function-Static State**: All mutable state must be encapsulated within a heap context object allocated per instance (`*plugin_ctx`, typically inheriting from `kit::PluginBase`).
2. **State Recovered via Context Closures**: All tools, hooks, and event callbacks must recover their instance context via `spec.user_data`.
3. **Cache Interface Tables in Instance Context**: Results of `AgentIfaces` queries must be stored as instance members, preventing interference across instances. Adapters for offload thread pool asynchronous interfaces (`plugin_kit.h`) must be embedded within the caller's instance context and destroyed alongside the instance.

---

## 4. Exported Symbol Visibility

Plugin shared libraries hide all symbols by default, exporting only the entry-point symbols looked up by name by the host. Entry functions must be declared with `PLUGINXX_EXPORT` (inside `extern "C"`):

```c
#include "agentxx/plugin/api/plugin_api.h"
extern "C" PLUGINXX_EXPORT const PluginxxInfo* agentxx_plugin_agent_get_info(void);
extern "C" PLUGINXX_EXPORT int32_t agentxx_plugin_agent_create(const PluginxxHost* host, void** plugin_ctx);
extern "C" PLUGINXX_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx);
```

- **Entry Symbol Sets**:
  - Agent side: `agentxx_plugin_agent_get_info` / `agentxx_plugin_agent_create` / `agentxx_plugin_agent_destroy`
  - Client side (Dual-sided or UI-only): `agentxx_plugin_client_get_info` / `agentxx_plugin_client_create` / `agentxx_plugin_client_destroy`
- **Automated Build Configuration**: `plugins/CMakeLists.txt` configures ELF `-fvisibility=hidden` + version script whitelists (wildcarding `agentxx_plugin_agent_*` / `agentxx_plugin_client_*`, allowing single-sided plugins to link under Android lld), macOS `-exported_symbols_list`, and MSVC `dllexport`. Third-party static library symbols are hidden automatically.

---

## 5. Tool Function Reuse (`cxx_utilxx_base` / `cxx_utilxx`)

Built-in plugins can reuse all core utility functions (string manipulation, encoding detection, UTF-8 conversion, path normalization, Base64, logging, JSON, plus HTTP/regex/diff/worktree) via two standalone static libraries. They live under `agent/third_party/` as project-maintained independent CMake projects (siblings of fmt/simdjson); the superbuild builds and installs them first, then `libagentxx` and each plugin pull the **static variants** via `find_package`:

```cmake
# base utilities (no heavy deps): log/json/string/container/env/system probe/
# cancel token/async offload
find_package(cxx_utilxx_base REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE cxx_utilxx_base_static)

# heavy utilities (depend on the base): HTTP/WS/regex/router/diff/worktree/crypto
find_package(cxx_utilxx REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE cxx_utilxx_static)
```

> Database utilities are **not** part of these libraries: the SQLite wrapper (`SqliteDb`) and
> the global settings store (`SettingsDb`) live in host-only code (`agentxx/util/sqlite.h` /
> `settings_db.h`, tied to the host data-directory convention and `AgentConfigStatic`); the
> utility libraries (including `cxx_utilxx`) do not link SQLite. A plugin needing a database
> links one itself or requests persistence through a host capability table.

```cpp
#include "utilxx_base/string_util.h"
auto b64 = utilxx_base::base64Encode(data);
#include "utilxx_base/json.h"
#include "utilxx_base/json_view.h"
// Business/plugin code uniformly uses utilxx_base::Json/JsonView (simdjson-backed); hot read-only paths route via JsonView::parse first, then to_json() on hit
```

- Library/namespace mapping: `cxx_utilxx_base` → `utilxx_base` (plus the cross-library contract `utilxx::CancelToken` and `utilxx::offloadAsync*` in `utilxx/cancel.h` / `utilxx/async_offload.h`); `cxx_utilxx` → `utilxx` (http/ws/regex/router/diff/worktree/crypto).
- Both build a shared and a static variant with libagentxx-style naming (Release: `libcxx_utilxx.so` / `libcxx_utilxx_static.a`; Debug appends `d`).
- Both `libagentxx` and individual plugins statically link their own copy; symbols are hidden via export visibility control without conflict. Dependencies are transitively propagated as `PUBLIC` (fmt, simdjson, uchardet, iconv + OpenSSL, html2md, Boost headers).
- **Conditional dependencies are declared by name only; each consumer locates them on its own machine (dependency libraries first, then the libraries themselves)**: the exported interfaces contain `PkgConfig::uring` (io_uring — used by `cxx_utilxx_base`'s asio async-file-IO probe) and `PkgConfig::hyperscan` plus the bare name `hs_runtime` (HyperScan — used by `cxx_utilxx`'s regex), never a library file path. Static archives do not carry dependency binaries, so whoever links must resolve them: consumers must run `pkg_check_modules` **before** `find_package` for the util libraries / `agentxx_static`, because the exported targets verify that targets referenced by their `INTERFACE` already exist. lib/client/test/benchmark key off the top-level `AGENTXX_LINUX_IO_URING_SUPPORTED` / `AGENTXX_ENABLE_HYPERSCAN` switches (same source as the util libraries' own build switches); one lookup in the plugins directory covers every plugin target (so plugins need no per-plugin configuration), while the two util libraries themselves key off the neutral `XX_LINUX_IO_URING_SUPPORTED` (the superbuild forwards the top-level value through it). A missing lookup fails loudly at configure time, or at `dlopen` time with `undefined symbol: io_uring_queue_init`.
- **Standalone-release constraint**: the three libraries are published as standalone projects and contain no host-specific names — build variables all use the neutral `XX_*` prefix (`XX_INSTALL_DIR` / `XX_EXEC_INSTALL_PREFIX` / `XX_LINUX_IO_URING_SUPPORTED`, plus the `XX_IS_*_D` platform/toolchain macros) and fall back to defaults when not supplied; the host superbuild forwards its values through those `XX_*` variables (see the common argument list in `agent/CMakeLists.txt`), so `AGENTXX_*` names appear only in the host's own build directories.
- Intended as a convenience library for built-in plugins (built within the same superbuild with full dependencies). Third-party plugins only need the pure C header `plugin_api.h` / SDK `plugin_kit.h` without linking against host libraries.
- Unreferenced modules are automatically pruned based on object file extraction (9 built-in plugins have `DT_NEEDED` pointing only to system libraries).
- Split background and migration record: `resource/history/split-util-plugin-core/plan.md`.

### 5.1 Plugin Framework Kernel (`cxx_pluginxx`)

The plugin framework itself is split into a third standalone project, `agent/third_party/cxx_pluginxx` (namespace `pluginxx`, depends on `cxx_utilxx_base` + fmt + yaml-cpp, Boost headers only; **no** neograph / OpenSSL / SQLite / agentxx headers). Directory layout:

| Path | Contents |
|---|---|
| `pluginxx/api/` | Pure C ABI base (`abi.h`: export macros / calling convention / alignment / `StringView` / `String` / info / unified operation primitives / host vtable / entry symbol names) and the 10 generic interface tables (`tables.h`) |
| `pluginxx/kit/` | Plugin-side C++ SDK (header-only): cross-boundary string helpers, generic table aggregate `PluginIfaceCore`, `Task<T>` anchored coroutines and anchored primitives (`sleep`/`yield`/`offload`/`invoke_cap`), `CancelRegistry`/`OpCtl`/`ArgReader`, background `spawn`, instance context base `PluginBaseT<IfacesT>`, export macros, plus `guard.h` (C ABI exception guard) |
| `pluginxx/runtime/` | Host-side runtime: instance state machine / execution lease / delivery channel (`runtime.h`), coroutine driver tickets (`driver.h`), instance base + host control block (`instance_base.h`), manager base + vtable entry context (`manager_base.h`), unified operation driver (`op_driver.h`) |
| `pluginxx/host/` | Generic host facilities: `loader.h` (dlopen/LoadLibrary), `manifest.h` (plugin.yaml / name inference / topological sort), `abi_util.h`, `capability_registry.h`, `event_bus.h`, `domain_hooks.h`, `host_core.h` (`PluginHostCore<InstanceT>`: generic table state and method implementations), `tables_impl.h` (C ABI entries + `queryGenericPluginIface<I,M>`), `lifecycle.h` (`PluginHostLifecycle<InstanceT>`: the lifecycle skeleton) |

- **Generic tables are defined *and* implemented in the kernel** (log/json/config/plugins/events/scheduler/coroutine_runtime/tasks/cancel/capabilities): a host only implements `pluginxx::DomainHooks` (event back end, worker pool, config/language/session work dir/cancel state/plugin listing) and routes `query_interface` through `queryGenericPluginIface` before dispatching its own domain tables.
- **The lifecycle skeleton is in the kernel too** (`host/lifecycle.h`): library loading and entry-symbol validation, `create`/`start` transactions, enable/disable cascades, `stop` completion, inflight drain waiting, `destroy` + dlclose, failure rollback and close timeout retry all have exactly one implementation. A host supplies a few seams:
  - pure virtual: `selfRef()` (manager self reference; the kernel cannot inherit `enable_shared_from_this` itself without creating an ambiguous multi-base lookup with the host manager), `createInstance(name)`, `hostVtable()`;
  - optional overrides: `detachDomainRegistrations`, `clearDomainRegistrations`, `detachDomainOwnedResources`, `applyDeclaredResources`, `releaseInstanceResources`, `onInstanceEnabledChanged`, `onInstanceLoaded`/`onInstanceUnloaded`, `cascadeUnloadEnabledOnly`, `logTag`.
  - Both agentxx managers derive from it (`PluginManager` and `ClientPluginManager`). The client keeps its own *loading* path (dlopen offloaded to an internal thread pool, interface negotiation, dual agent/client entry probing) but shares unload/start-stop/cascade/close-wait with the agent side.
  - `destroyPlugin()` (destroy entry lookup + call + host control block retirement) lives on `pluginxx::PluginInstanceBase`; both instance classes only provide their own `pluginDestroySymbol()`.
- **SDK layering**: the generic part lives in the kernel, the domain helpers (tool registration / hooks / graph nodes / permission declarations / client rendering and `ClientPluginBase`) live in the host umbrella header `agentxx/plugin/api/plugin_kit.h`, which includes the kernel headers and re-exports the generic names into `agentxx::plugin` — plugin sources stay unchanged.
- Cross-boundary C names / struct names / IID strings are frozen: plugins compiled before the split still load into the new host unchanged.

---

## 6. C++ Plugin Development Workflow (SDK `plugin_kit.h`)

We recommend using the official header-only SDK `plugin_kit.h` (`agentxx/plugin/api/plugin_kit.h`).
The modern framework provides declarative export macros, a fluent schema builder, tolerant argument parsing, and centralized cancellation management:

```cpp
#include "agentxx/plugin/api/plugin_kit.h"

using namespace agentxx::plugin;

struct MyPluginCtx : public PluginBase {};

// Declarative export macro (encapsulates entry symbols, C ABI boundary guards, and context lifecycle)
AGENTXX_PLUGIN_AGENT_EXPORT(
    MyPluginCtx,
    "my_plugin",
    "1.0.0",
    "My awesome plugin description",
    [](MyPluginCtx& ctx) -> int32_t {
        // 1. ToolSchemaBuilder: fluent JSON schema declaration (auto-merged with host toolPrompt)
        auto mySchema = ctx.schema("my_blocking_tool")
                            .string("path", "Target file path", /*required=*/true)
                            .integer("timeout", "Timeout in seconds", false, 60)
                            .boolean("all_output", "Return all output", false, true)
                            .build();

        // 2. Blocking tool (automatically offloaded to host blockingPool, non-blocking to IO thread)
        // Injected callback: (ctx, args_json, tid, workDir, cancel_flag)
        blocking_tool(
            ctx,
            "my_blocking_tool",
            "Perform heavy work in worker thread",
            mySchema,
            [](MyPluginCtx&     c,
               std::string_view args_json,
               std::string_view tid,
               std::string_view workDir,
               volatile int32_t* cancel_flag) -> std::string {
                // ArgReader: tolerant type extraction (smart conversions between string/number/bool/json)
                ArgReader args(args_json);
                auto path = args.require<std::string>("path");
                if (!args.ok()) {
                    return args.errorMessage();
                }

                // Check cancellation via CancelRegistry or flag
                if ((cancel_flag && *cancel_flag != 0) || c.sessionCancelled(tid)) {
                    return "cancelled";
                }

                return R"({"status":"done"})";
            }
        );

        // 3. Fast synchronous inline tool (<~1ms, executed directly on IO thread)
        fast_tool(ctx, "my_fast_tool", "Fast tool depict", R"({"type":"object","properties":{}})",
            [](MyPluginCtx& c, std::string_view args_json, std::string_view tid) -> std::string {
                return R"({"result":42})";
            }
        );

        // 4. Task coroutine tool (supports co_await sleep / yield / call_tool / offload)
        tool(ctx, "my_async_tool", "Async coroutine tool", R"({"type":"object","properties":{}})",
            [](MyPluginCtx& c, std::string_view args_json, OpCtl ctl) -> Task<std::string> {
                co_await sleep(c, 100);
                ctl.throw_if_cancelled();
                // Cross-plugin call: co_await call_tool(c, "other_tool", "{}", ctl.sessionId());
                co_return R"({"status":"ok"})";
            }
        );

        // 4b. Controlled-polling tool: the body is an asio coroutine awaiting kernel readiness
        //     on the plugin's local reactor (socket / subprocess pipe / file / local timer).
        //     Registration declares "this tool needs controlled polling"; the bridge then keeps
        //     requesting tickets while the operation is in flight (immediate on progress, 10ms
        //     backoff when idle, zero overhead when idle) and never occupies a host worker
        //     thread. See §15.5.
        polled_tool(
            ctx,
            "my_polled_tool",
            "Async IO tool driven by controlled polling",
            R"({"type":"object","properties":{"url":{"type":"string"}}})",
            [](MyPluginCtx&     c,
               std::string_view args_json,
               std::string_view tid,
               std::string_view workDir,
               const PluginxxCancelToken* cancel) -> asio::awaitable<std::string> {
                ArgReader args(args_json);
                std::string workDirStr(workDir);
                if (pluginxx_cancel_is_requested(cancel)) {
                    throw CancelledException("cancelled");
                }
                // await the existing asio coroutine implementation directly (no local
                // io_context + run() dance)
                co_return co_await doHttpGetAsync(args.raw(), workDirStr);
            }
        );

        // 5. Host-managed background task
        spawn(ctx, [](MyPluginCtx& c, OpCtl ctl) -> Task<void> {
            while (!ctl.cancelled()) {
                co_await sleep(c, 5000);
                if (ctl.cancelled()) break;
                // Periodic event publishing
            }
        });

        // 6. Hooks (7 hook points)
        hook(ctx, AGENTXX_PLUGIN_HOOK_MODEL_START, [](MyPluginCtx& c, std::string_view in) {
            // ...
        });

        // 7. Capabilities (generic cross-plugin RPC)
        capability(ctx, "my.cap", [](MyPluginCtx& c, const PluginxxHost* caller,
                                     std::string_view method, std::string_view args) {
            return "{}";
        });

        return 0;
    }
);
```

### SDK Core Infrastructure Highlights

1. **Export Macros (`AGENTXX_PLUGIN_AGENT_EXPORT` / `AGENTXX_PLUGIN_CLIENT_EXPORT`)**:
   - Automatically defines exported C entry points (`agentxx_plugin_agent_get_info`, `create`, `destroy`).
   - Wraps all entry calls in `guardCall` exception guards, intercepting C++ exceptions from crossing C ABI boundaries.
   - Manages heap-allocated instance context (`PluginBase`) lifecycle cleanly.
2. **Fluent Schema Builder (`ToolSchemaBuilder`)**:
   - Obtainable via `ctx.schema("tool_name")`, supporting `.string()`, `.integer()`, `.number()`, `.boolean()`, `.array()`, `.stringArray()`, `.enumString()`, and `.build()`.
   - Merges localized descriptions from host `toolPrompt` automatically.
3. **Tolerant Parameter Reader (`ArgReader`)**:
   - Parses input JSON with smart auto-healing (e.g. converting `"true"`/`"1"` to boolean, numbers to strings).
   - Provides `require<T>()`, `get<T>()`, `ok()`, and `errorMessage()` for error-resilient parameter extraction.
4. **Event-Driven Cancellation Registry (`CancelRegistry`)**:
   - Centralized, instance-isolated cancellation manager (`ctx.cancelRegistry`).
   - Automatically clears per-session cancellation flags when `plugin.agentxx.round_start` fires.
   - Supports `registerCallback(sessionKey, cb)` returning an RAII `ScopedRegistration` guard with reentrant mutex and lifetime safety protection to eliminate dangling references during concurrent teardowns.
   - Ideal for sub-processes or long-running blocking operations (`agentxx_execute_command`) to immediately kill process trees and close IO pipes on cancellation.

**Unified Asynchronous Operation Model (Two-piece start/cancel)**: Tools, hooks, and capabilities all adhere to the `start` (non-blocking invocation on IO thread) + `cancel` (cooperative cancellation) lifecycle. Completion is reported exactly once via `AgentxxOpNotify.done(status, payload)`. `Task` coroutine frames are destroyed before invoking `done`, supporting `offload` blocking-pool delegation and `call_tool` / `invoke_cap` cross-plugin invocations.

**Shared Convenience Helpers (the small things every plugin used to hand-write)**:
- `pluginLog(ctx, level, msg)` / `pluginLog(host, logIf, level, msg)`: instance logging
  (silent when the context is null; the 4-arg form serves plugins that only hold host + log iface)
- `pluginStrdup(host, s)`: copy a C string via host `alloc` (for C ABI `char*` out-parameters)
- `ctxGuardLogger(ctx)`: log closure used by `guardCall`/`guardCallVoid` C ABI guards
- `jsonEscape(host, jsonIface, text)`: text to JSON string literal (host `json_escape` when
  available, local escaping as fallback) for hand-assembled JSON text
- Import inside the plugin namespace with `using agentxx::plugin::xxx;` (all built-in plugins do)

**Host-Managed Background Task Spawning (`spawn`)**: Background cooperative tasks started via `spawn` (e.g. periodic collection `while(!cancelled()) { offload; sleep; }`) are registered to the host `agentxx.agent.tasks` interface table since API v1, managed isomorphically with tool/capability ops:

- **Registration**: `spawn()` automatically calls `register_task` (on the IO thread) → Host records the handle into the instance's `outstandingOps` (shared with tool ops) and holds an `inflight` reference.
- **Execution**: The coroutine suspends via `sleep`/`offload` through the host; transparent to the host.
- **Unload Coordination**: When the plugin unloads, the host's `detachAll` cancels all registered tasks (invoking the plugin's `cancel_fn`, setting `cancelFlag` and waking sleeping/offloaded tasks) → Coroutine exits `while(!cancelled())` loop → `finishIfDone` (after coroutine frame destruction, reports completion exactly once via `notify.done`) → Host decrements `inflight` (`guard.reset`) and releases handle → `waitInflightZero` cleanly awaits zero active operations → `dlclose` occurs safely without dangling frames or UAF.
- **No fallback**: `spawn` fails directly when the host lacks the `agentxx.agent.tasks` table or registration is refused (there is no unmanaged-coroutine degradation path).
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
| `agentxx.agent.scheduler` | 1 | `is_io_thread/post_to_io/sleep/op_cancel/offload` (`sleep` = host timer; `offload` = blocking-pool delegation, requires a `cancel_token`). |
| `agentxx.agent.coroutine_runtime` | 1 | Generic coroutine driver: `request_driver/cancel_driver/is_io_thread` (driver-ticket / wake protocol, see §15). |
| `agentxx.agent.session` | 1 | `get_share_store/add_share_store/emit_message_tip` (IO thread). |
| `agentxx.agent.plugins` | 1 | `list_plugins/get_plugin/get_own_info` (JSON). |
| `agentxx.agent.config` | 1 | `get_config/get_plugin_args/get_tool_prompt/get_session_work_dir/get_plugin_config_path/get_language/set_language` (`get_session_work_dir` returns default workdir when session ID is empty; `get_plugin_config_path` returns normalized absolute path configured via YAML `config`, pointing to a file or directory; `get_language/set_language` queries/overrides runtime language). |
| `agentxx.agent.model` | 1 | `get_config` (Active model and associated config JSON). |
| `agentxx.agent.cancel` | 1 | `is_cancelled(sessionId)` (Advisory polling; authoritative notification comes via cancel callback). |
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
| `agentxx.client.ui` | 1 | `register_status_item/update/unregister`, `register_panel/update/unregister`, `register_info_section/update/unregister`, `register_command/unregister`, `show_toast`, `update_tool_decor(tool_call_id, decor_json)`, `register_tool_renderer(spec)/unregister_tool_renderer(tool_name)`, `bind_action_handler/unbind_action_handler`, `open_overlay/close_overlay`. |
| `agentxx.client.events` | 1 | `subscribe/unsubscribe` (See `AgentxxClientEvent`: `READY`, `CONN_STATE`, `USER_INPUT`, `DELTA`, `TURN_END`, `SESSION_SWITCH`, `PLUGIN_DATA`, `UI_LAYOUT`). |
| `agentxx.client.session` | 1 | `get_client_state` (snapshot JSON), `send_user_input`, `request_cancel`. |
| `agentxx.client.wire` | 1 | `send_plugin_data(event, json)` → Dispatched to server as `client.{plugin}.{event}`. |
| `agentxx.client.self` | 1 | `get_own_info/get_plugin_args/get_plugin_config_path` (Returns normalized absolute path configured via YAML `config`). |
| `agentxx.client.json` | 1 | `json_get_string/json_escape`. |
| `agentxx.client.log` | 1 | `log(level, msg)`. |
| `agentxx.client.timer` | 1 | One-shot / periodic timers plus region visibility queries (pause policy: plugins.md §9.5 in the Chinese doc). |
| `agentxx.client.keybind` | 1 | Global keybind register/unregister/list (normalization and priority: §9.6 in the Chinese doc). |

Manifest declaration conventions (`interfaces.require/optional`; the side is determined by prefix):

- The client side should declare **fine-grained capability names** (`agentxx.client.msg_decor`,
  `...panel`, `...toast`, ... — each maps to a non-NULL member of the ui table) so the host can
  decide whether the feature is usable; reserved/unimplemented names are always treated as
  "not supported by host" (fail safe).
- `agentxx.client.ui` (the ui table IID) means the **whole table**: only hosts covering every
  sub-capability advertise it (TUI). Declaring it is equivalent to requiring all sub-capabilities,
  so hosts missing some of them (CLI) treat it as missing (`optional` → warning, `require` → skip
  loading). A plugin that only uses specialized tool rendering should therefore declare
  `agentxx.client.msg_decor` rather than the table IID.

### Specialized Tool Rendering Architecture (Tool Rendering & Decor)

The Agentxx client adopts a unified, layered tool-specialized rendering mechanism. The TUI core is completely decoupled, containing zero hardcoded tool names:

1. **Type-Level Tool Renderers (`register_tool_renderer`)**:
   - During client initialization, plugins register specialized rendering definitions (`AgentxxToolRenderSpec`) keyed by `tool_name`, universally taking effect when the TUI renders messages for that tool (both during real-time streaming and history replay).
   - **History replay is one of the main scenarios**: `Tool` messages replayed after a session restart / reconnect / session switch carry no instance-level decoration (see item 2) — their specialized rendering relies entirely on the renderer registered here, whose only inputs are the message's own `args_json` / `result_text`.
   - Framework nuance: **the expanded-state header display name is overridden only by instance-level decorations** (a type-level renderer match keeps the raw `toolName` in the expanded header), while renderers always drive the **collapsed header name/summary** and the **expanded body items**. Since replayed history messages start collapsed (`collapsed=true` in `event_stream`), the replayed view matches the live one.
   - **Dual-Track Mechanism**:
     - **`<key, render_fn>` Callback Function**: Provides an `AgentxxToolRenderFn` receiving `AgentxxToolRenderInput` (`tool_name`, `args_json`, `result_text`, `is_finished`, `is_error`, `max_width`) and returning `AgentxxToolRenderOutput` (`displayName`, `summary`, `items_json`). Suitable for tools requiring complex argument parsing, conditional formatting, or dynamic UI item generation (e.g. `read` offset-limit parameters, `glob`/`grep` patterns and file summaries, `edit` diff comparisons).
     - **Declarative Template (`template_json`)**: When `render_fn == NULL`, the host automatically extracts fields from `args_json` and formats the summary according to the declarative template, e.g. `{"displayName":"Search","summaryKey":"query"}` or `{"displayName":"Bash","summaryKey":"command"}`.
   - **Generic Diff Rendering**: Expanded `items_json` supports `{"kind":"diff","path":"...","old_str":"...","new_str":"..."}`. The TUI generically renders this as an adaptive side-by-side or unified diff comparison, freely reusable by any plugin.
   - **SDK Helper (`registerToolRenderer`)**:
     `plugin_kit.h` provides an ergonomic modern C++ lambda wrapper:
     ```cpp
     agentxx::plugin::registerToolRenderer(host, ui, "agentxx_filesystem_read",
         [](const agentxx::plugin::ToolRenderInput& in, agentxx::plugin::ToolRenderOutput& out) {
             out.displayName = "Read";
             ArgReader args(in.argsJson);
             out.summary = args.get<std::string>("path").value_or("");
         },
         ctx.shimStorage
     );
     ```
2. **Instance-Level Tool Decorations (`update_tool_decor`)**:
   - Subscribing to `tool_start` / `tool_end` in `EVT_DELTA`, the plugin pushes semantic JSON keyed by the specific invocation's `tool_call_id` (taking higher priority than type-level renderers). A prominent implementation is `agentxx_planning` (Mermaid state diagrams and reactive todo lists).
   - **Only real-time invocations of the current process are covered**: on client restart (session restore), reconnect or session switch, historical `Tool` messages are replayed by `WireSyncPayload` / `WireViewMessagesPage` with no delta events and therefore no pushed decorations. Such messages must be rendered by a **type-level renderer** derived from the message's own arguments/result (see item 1; `agentxx_planning` registers both, sharing one content builder).
3. **Priority Order and Fallback Path**:
   - Lookup order during rendering: `toolDecors` (by `tool_call_id`) > `toolRenderers` (by `tool_name`) > Generic fallback presentation (raw `toolName` + arguments/result text).
   - When a plugin unloads or is disabled, the host automatically strips its registrations and cleanly reverts to the fallback presentation, restoring specialized views losslessly upon re-enablement.
4. **Reuse Boundary: Plugin Items vs. Interrupt Content (after refactor)**:
   - **Static blocks share one implementation**: plugin `items` (panels / Info sections / tool decors / overlays) and interrupt content blocks (`agent/lib/include/agentxx/middlewares/interrupt_ui.h`) are rendered by the same TUI block renderer ([ui_components.h](/agent/client/include/agentxx-client/io/tui/ui_components.h): a row model where rendering and height estimation are single-sourced); role→color mapping, button styling and diff rendering still come from [plugin_ui_items.h](/agent/client/include/agentxx-client/io/tui/plugin_ui_items.h). The two data structures are **not one schema**.
   - **Interrupt descriptors are generated by producers with preset templates** (permission card `preset::permissionCard`, typed input form `preset::inputForm`, confirm card `preset::confirmCard`, see [interrupt_presets.h](/agent/lib/include/agentxx/middlewares/interrupt_presets.h)); **plugin-declared interrupt UI is not open yet**. Block kinds: content `text`/`markdown`/`diff`/`separator`/`gap`, control `control` (`buttons`/`select`/`text`/`number`/`checkbox`), submit row `submit`, reserved `custom` (component + props; renders `fallback` until client component renderers exist).
   - **There is no "parameter type" concept in the protocol** (bool/int/enum etc. were removed): producers that need typed inputs call the preset templates (the type→control mapping lives only in the preset).
   - **Plugin-items-only kinds**: `button` (`action_id` dispatched to `bind_action_handler`) / `progress` / `diagram`.
   - Result contract: an interrupt answer is always `{"values": {"<control id>": value}}` (empty object = not answered).

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
| `example_plugin` | Comprehensive native C++ example (fast_tool, Task coroutines, call_tool, sleep, **coroutine bridge**, **controlled polling `polled_tool`**, hooks, events, capabilities, client entry). |
| `example_graph_node` | Graph extension sample (custom node types + set_graph_json; requires `agentxx.agent.graph`). |
| `example_js` | JavaScript script plugin example (C++ shell wrapper + `plugin.js`). |
| `example_resources` | Session resource contribution example (declarative & programmatic MCP, Skills, rules, session environments). |
| `agentxx_filesystem` | Filesystem tools (list, read, write, edit, glob, grep; includes unit-tested `*_impl.h`). |
| `agentxx_execute_command` | Command execution tools (bash, windows; includes timeout handling); probes python/node (plus PowerShell on Windows) in its start transaction and writes the results into its own tool prompts. |
| `agentxx_websearch` | Web search and retrieval tools (search, fetch, fetch_markdown). |
| `agentxx_rag_search` | Vector semantic search. |
| `agentxx_string` | String tools (html_to_markdown, regexp). |
| `agentxx_system` | System clock tool (`get_current_datetime`). |
| `agentxx_system_monitor` | System resource monitor (Windows/Linux/Android/macOS; tool + background periodic sampling + client Info/Status bar rendering). |
| `agentxx_planning` | Task planning tool + client-side Plan rendering (type-level tool renderer for both live and replayed history + runtime decoration + Info section). |
| `agentxx_math` | Math computation tool (`agentxx_math_calculate`; supports arithmetic, powers, factorials, bitwise, logic, trig, hyperbolic, log, combinations/permutations, implicit multiplication). |
| `agentxx_codegraph` | Code index & navigation (5 tools: search/context/callers/callees/path + client Info panel). |
| `agentxx_screen_capture` | Screen capture (Windows only). |
| `agentxx_computer_use` | Mouse and keyboard control (Windows only; depends on `screen_capture`). |
| `agentxx_audio_stream` | Audio stream capture (**skipped on all platforms**: WASAPI implementation not enabled; stub only). |
| `agentxx_text_selection_monitor` | Text selection event listener (Windows UIAutomation only). |
| `agentxx_javascript_engine` | QuickJS execution engine (exports `interpreter.js` capability). |
| `example_js_execute_command` | JS code execution tool (`example_js_execute_command`; depends on `agentxx_javascript_engine`). |

> **Plugins own their tool prompts (including runtime env probing)**: tool definitions
> (`description` / `parameters`) are frozen by `register_tool`, so a plugin must finish any
> probing/decisions **before** registering its tools (refreshing prompts afterwards does not
> change an already frozen tool definition). `agentxx_execute_command` follows this by probing
> python/node availability + version (and the PowerShell executable name + version on Windows)
> inside its `start` transaction, then injecting the resulting depict and argument descriptions
> as this instance's contribution through the `agentxx.agent.prompt` interface; the contribution
> is removed automatically on `disable` / unload. Probe results live only in the
> instance context (`ExecPluginCtx::env`) — no process-wide mutable statics (multi-instance
> constraint, see §4).

---

## 14. Build System & Platform Support

- **Platform Matrix**: Each plugin determines platform compatibility at the start of its `CMakeLists.txt` via the `gate` function in `plugin_platform_support.cmake`, leveraging top-level `XX_IS_*_D` flags. Unsupported platforms are skipped during compilation (`screen_capture`, `computer_use`, and `text_selection_monitor` are Windows only; `audio_stream` is skipped on all platforms since its WASAPI implementation is not enabled, etc.). An empty platform list means "skip everywhere". `agentxx_system_monitor` covers windows/linux/android/macos: Windows uses PDH + DXGI, Linux/Android parse `/proc` + sysfs, and macOS uses mach `host_statistics`/`host_statistics64` plus IOKit `IOAccelerator` (`PerformanceStatistics`).
- **Verified platforms (current implementation)**: Windows (MSVC 14.51 / VS18, Debug + ASan: full plugin build, plugin-focused 1765/0, extended regression 2251/0), Linux (GCC, Debug + ASan/LSan, targeted UBSan/TSan) and macOS (Apple M1 / Apple clang, Debug + ASan: `agentxx_system_monitor` platform-matrix regression cpu_gpu 32/0, plugin_multi_instance 80/0, client_plugins 537/0). Android is not verified.
- **Running the test binary on Windows**: the working directory must be the executable's directory (`exec/`), because plugin paths are derived from `GetModuleFileNameW` (Linux uses `/proc/self/exe`).
- **Monolithic Built-in Compilation**: Plugins specified in `AGENTXX_PLUGIN_BUILTIN_LIST` are merged into `libagentxx`. In this mode, `test_ffi_c_api` and `client_plugins` tests conditionally bypass dynamic library path checks.
- **Artifact Layout**: Standalone shared libraries output to `{build}/exec/plugins/<plugin_name>/` (organized into subdirectories when accompanied by a `plugin.yaml` manifest).

---

## 15. Coroutine Driver (Generic pump/wake Protocol + `PollOneBridge`)

> This section describes the **coroutine driver protocol**: plugin
> coroutines and host coroutines interleave inside the *same* host IO execution sequence,
> without extra threads and without blocking the IO thread. Wait sources are primarily
> "host-visible"; kernel-readiness waits on a plugin-private reactor are driven by
> **declared controlled polling** (`polled_tool`, see §15.5).
> The design/phasing document lives in `resource/history/plugin-refactor-3/plugin.md`;
> the implementation record is in `resource/history/plugin-refactor-3/work.md`.

### 15.1 Why

A plugin may ship its own coroutine library, event loop and third-party async libraries,
while the host cannot register a plugin-private reactor's wait objects into its own
execution sequence (a private `io_context` exposes no portable API to hand its epoll/kqueue/
IOCP wait object to another `io_context`). Both sides therefore cooperate through exactly
two actions:

- **driver / pump**: the plugin asks the host to run one **bounded callback** asynchronously
  (advancing the local runtime by one finite step, e.g. one `poll_one` == one ready handler);
- **wake**: when the plugin adapter knows the local runtime has work (new root first step,
  a continuation posted by a host completion callback, a local post) it requests a driver
  ticket; duplicate wakes are merged by the adapter.

The host never learns which coroutine/future/actor library the plugin uses, and the plugin
never receives a host executor.

### 15.2 C ABI (`agentxx.agent.coroutine_runtime`, version 1)

```c
typedef struct PluginxxDriver PluginxxDriver;
typedef void(PLUGINXX_CALL* PluginxxDriveOnceFn)(void* user_data);

typedef struct PluginxxCoroutineRuntimeIface {
    int32_t  version;      // == 1
    uint32_t struct_size;
    PluginxxDriver* (PLUGINXX_CALL* request_driver)(
        const PluginxxHost*, PluginxxDriveOnceFn, void* user_data, PluginxxString* error_out);
    void   (PLUGINXX_CALL* cancel_driver)(PluginxxDriver*);
    int32_t(PLUGINXX_CALL* is_io_thread)(const PluginxxHost*);
} PluginxxCoroutineRuntimeIface;
```

**Contract (both sides)**

1. `request_driver` may be called from **any thread** and the callback is **never inlined**
   (even when the caller already runs on the IO thread). Inlining would cause unexpected
   re-entrancy between root start / completion / cancel and destroy interleaving fairness.
2. One ticket runs the callback **at most once**, and the callback advances exactly one
   bounded step: it must not block, must not wait for events and must not call host business
   APIs synchronously. Exceptions must be caught by the plugin.
3. The plugin adapter **merges wakes itself** (one ticket registered at a time per instance);
   the host still de-duplicates tickets and cancels on close as a last line of defence.
4. `cancel_driver` is **idempotent and non-blocking**: a ticket that has not started yet will
   never run its callback; a callback that already started is not interrupted and is closed
   out by the plugin's own root finalisation protocol.
5. A ticket **holds an instance execution lease** while queued and running, so `dlclose` can
   never pass by plugin code that is still queued or executing (the unload idle-wait covers it).
6. Failure: `NULL + error_out` means the host no longer provides drivers; the plugin must
   finalise the affected operations as failed/cancelled instead of silently dropping them
   (the kit does this automatically).

### 15.3 Host-side semantics

- **Ticket state machine**: `Idle → Running → Finished` or `Idle → Finished` (cancelled),
  arbitrated by a single CAS, guaranteeing "no callback after cancel" and "lease released
  exactly once".
- **Handle validation**: `cancel_driver` carries no host argument, so the host cannot resolve
  the owning instance from it. Tickets are therefore registered in a **process-level address
  registry** (holding `weak_ptr` only, removed by address when the ticket finishes); a forged
  or expired pointer is ignored safely with a log line and is **never dereferenced**.
- **Admission mode**: tickets use the `lifecycle` lease mode — drivers are still admitted
  while the instance is `Closing`. This is required: closing starts by cancelling every
  Operation, and the plugin's cancellation drain (cancel callback → wake → next finite step)
  must be able to finish; otherwise closing would always time out. `Disabled`/`Closed` are
  rejected.
- **Close rescue**: the host calls `cancelPendingDrivers()` only after it has already judged
  the close as failed (`waitInflightZero` timeout), cancelling tickets that never started so
  the instance lease cannot leak (with a warning log). The normal close path does not depend
  on it: the kit cancels its own ticket when the plugin context is destroyed.

### 15.4 Kit implementation (`detail::PollOneBridge` + `detail::BridgeRoot` / `detail::PolledRoot`)

`PluginBase::bridge()` returns the per-instance bridge; the kit requires the host
`coroutine_runtime` table and keeps no `post_to_io` fallback path.

**State machine (all three race windows are covered)**

| State | Meaning |
|---|---|
| `readySteps_` | local steps posted but not yet executed (each needs one `poll_one`) |
| `wakePending_` | one explicit `wake` not yet covered by a ticket (covers plugins posting directly to `local_executor`) |
| `driverQueued_` / `driverRunning_` | ticket requested (incl. the `request_driver` return window) / callback executing |
| `pendingEpoch_` / `nextEpoch_` | ticket generation, used to detect "this round's ticket was already consumed" |
| `polledRoots_` | in-flight **polled roots** (declared `polled_tool`; 0 = no polling, no timers) |
| `pumpPending_` | the pump decided "keep driving" (second reason to request a ticket) |
| `pumpWaitScheduled_` / `pumpWaitOp_` | in-flight backoff timer (host `scheduler.sleep`) and its handle |
| `pollBurst_` | consecutive "made progress" steps (used by the burst cap) |

Key invariants:

1. Each ticket performs **exactly one** `localIo_.poll_one()` (one host driver ⇔ one local
   continuation);
2. The next ticket is requested only when there really is work (`readySteps_ > 0`, an
   uncovered explicit wake, or in-flight polled operations whose scheduling policy says
   "keep going"); `poll_one()==0` **never** re-queues itself with no work — an idle plugin
   neither consumes host task-queue slots nor creates timers;
3. Host completion callbacks only do `postToLocal(continuation) + wake()`: a plugin coroutine
   is **never resumed inside a host callback frame** and plugin business code never runs on
   the host IO thread outside a driver;
4. Posting N steps concurrently yields N tickets (they are not collapsed into one);
5. When the host refuses drivers or the bridge stops, every active root (including polled
   roots) is finalised as `FAILED`, so host Operations cannot hang.

**Controlled-polling (pump) scheduling policy** (applies only to `polled_tool` roots, §15.5):

```
this round's poll_one ran a handler (progress) and pollBurst_ < kPollBurstMax(256)
    -> request the next ticket immediately     (ready sockets/pipes/files are reaped promptly)
otherwise (no progress / burst cap reached)
    -> schedule one backoff via scheduler.sleep (10ms when idle, 1ms after the burst cap)
       whose expiry callback only calls request_driver (it never resumes plugin coroutines)
polledRoots_ drops to 0
    -> cancel the in-flight backoff; no further tickets are requested (zero idle overhead)
```

**Root lifetime (`BridgeRoot` / `PolledRoot`)**

- Live roots are **strongly referenced by the bridge** (`roots_`): a suspended root has no
  other owner than "its next resumption task", so without that reference the frame would be
  destroyed as soon as the queued task finished and a later host callback would touch freed
  memory;
- Completion/abandonment arbitration is **exactly-once** (CAS): normal completion goes through
  `finishIfDone`, host-refused driving goes through `abandon`, and only one of them can report
  the terminal state;
- Abandoned roots move to `abandonedRoots_` and their **frame stays alive until the bridge is
  destroyed** (at which point the instance has no outstanding host operations); late host
  callbacks safely skip them because `shouldAdvance()==false`;
- Coroutine-frame destruction and op-handle (`Job`) release happen together at a point where
  "the coroutine has finished or can never be resumed again" (`destroyFrame()`): on normal
  completion inside `finishIfDone`, or at bridge destruction on the abandoned path. This must
  **not** happen inside `abandon` — an abandoned root may still be executing inside a driver,
  with its inputs (`RootRequest`/`OpCtl`) referenced by the coroutine.

Polled roots use the same arbitration but a simpler shape (`PolledRoot`, see `polled_tool`):

- the coroutine is an `asio::awaitable`, so **asio itself owns the frame** (released with the
  completion handler / local reactor); `PolledRoot` therefore never destroys frames and only
  arbitrates "terminal report + Job release" exactly once (normal completion vs bridge stop,
  decided by a single CAS);
- normal completion: `detail::runPolledPumpJob` claims, de-registers the root
  (`polledRoots_` decremented, stopping the pump when it hits 0), reports the terminal state
  and runs the cleanup that releases the `Job`;
- bridge stop: `failAllPolledRoots` detaches every in-flight root at once, reports `FAILED`
  once per root and runs the same cleanup; the suspended frames are released when the local
  reactor is destroyed.

**Kit paths that now go through the bridge**

| Kit component | Bridge semantics |
|---|---|
| `tool` / `hook` / `capability` / `graph_node` (returning `Task<T>`) | first step is driven by a host driver; no plugin coroutine code runs inside start |
| `spawn` (background task) | same (first step never runs in the caller's frame) |
| `sleep(ctx, ms)` | host timer adapter: the expiry callback only posts + wakes |
| `call_tool` / `invoke_cap` | host callback-style interop: the completion callback only posts + wakes |
| `yield(ctx)` | yields one round: posts the continuation, resumed by the next ticket |
| `offload(ctx, work)` | **the work body still runs on the host's worker pool** (explicit exception); only the resumption returns to the driver sequence |
| `polled_tool(ctx, name, ...)` | **declared controlled polling**: the body is `asio::awaitable<std::string>` and runs on the bridge's local executor, advanced by "request immediately on progress + 10ms backoff when idle + 1ms yield after a 256-step burst" (§15.5) |
| `fast_tool` / sync hooks / `blocking_tool` | unchanged (sync paths need no driving) |

`bridge().local_executor()` exposes the local executor so plugins can `asio::co_spawn` their own
`asio::awaitable`s into the same sequence; those awaitables must await operations with a
host-visible wake source (host callback adapters, host timers, posted continuations), or the
operation must be registered as a `polled_tool` (§15.5) so the bridge knows to keep polling.

### 15.5 Controlled polling (`polled_tool`) and remaining explicit limits

**Why it is needed**: `poll_one()` only runs *already ready* handlers; it cannot make waits on
the **plugin-private reactor** expire. Tools whose body already is an asio coroutine and whose
waits are kernel-readiness events (HTTP sockets in websearch, subprocess pipes in
execute_command, `asio::stream_file` in filesystem) therefore cannot be advanced without a
wake source.

**Approach: declared controlled polling (`polled_tool`)**. The plugin *declares* at
registration time that the tool needs controlled-polling drivers (it does not hide polling),
with explicit, observable parameters:

| Item | Value | Note |
|---|---|---|
| Trigger | `polledRoots_ > 0` | polling only while polled operations are in flight; zero tickets / zero timers when idle |
| Progress | request the next ticket immediately | `poll_one` ran a handler this round (a ready event was reaped) |
| No progress | 10ms backoff | `PollOneBridge::kPollIntervalMs` via host `scheduler.sleep`; the expiry callback only calls `request_driver` |
| Burst cap | 1ms yield after 256 consecutive steps | `kPollBurstMax` / `kPollBurstYieldMs`; prevents one instance from monopolising the IO thread |
| Cancel | set the cancel flag + cancel the in-flight backoff + write into `CancelRegistry` | the plugin sees cancellation without waiting a full backoff quantum, then drains its root |
| Host without drivers | unsupported | `coroutine_runtime` is a mandatory host capability; without it a driver request fails, finalises in-flight roots and is logged (no offload downgrade path) |

The business signature matches `blocking_tool` (only the return type becomes
`asio::awaitable<std::string>`), so migration is usually a renamed registration call:

```cpp
polled_tool(ctx, name, depict, schema,
    [](Ctx& c, std::string_view args, std::string_view tid, std::string_view workDir,
       const PluginxxCancelToken* cancel) -> asio::awaitable<std::string> {
        ArgReader reader(args);
        co_return co_await doSomethingAsync(reader.raw(), c.workDir(tid));
    });
```

Costs and rules that must be respected together with the implementation:

1. **10ms quantum**: while an instance waits, that is ≈100 drivers/second (each ≈1 host post
   plus one non-blocking `epoll_wait(0)`); worst-case added latency is 10ms (fine for network
   and subprocess work). Implemented as a constant so the policy can be tuned later.
2. **Heavy CPU must not stay inside a polled coroutine**: it runs on the host IO thread
   (>100ms triggers the host watchdog warning). Directory traversal, whole-file scans and
   vector similarity keep using `blocking_tool`; if heavy computation is genuinely needed
   inside a polled coroutine, offload it explicitly via the host `offload`.
3. **Do not use kit's `Task`-style primitives** (`sleep`/`call_tool`/`invoke_cap`) inside a
   polled coroutine: their awaiters rely on the kit's own promise interface while the polled
   coroutine is an `asio::awaitable`. Use asio's native `steady_timer` for timing (it works
   under the pump) and `bridge().local_executor()` to post follow-up steps when a host
   callback-style API is needed (asio-style adapters are a possible later enhancement).
4. **Replaceable**: once `wait_source` (the host waiting on an fd/handle handed over by the
   plugin) or a host-side IO service exists, only the "when to request the next ticket" policy
   changes from "10ms backoff" to "readiness notification"; polled tool bodies and the ABI
   stay unchanged.

**Still explicitly unsupported / requires an explicit exception**:

- **Depending on a private reactor without declaring polling**: waits that only rely on the
  plugin's private `io_context` (sockets/timers/process/files) are **not** advanced by the
  bridge. After 8 consecutive driver steps without progress while roots are still active the
  bridge logs a single warning ("awaited work has no host-visible wake source") pointing at
  (1) host callback-style completion (post + wake), (2) host timers (`co_await sleep`),
  (3) `polled_tool` declared controlled polling, or (4) an explicit bounded worker
  (`offload` / `blocking_tool`, documented in the capability notes). It does not spin, so the
  ticket count stops growing after the warning.
- **Misuse check**: the first driver verifies it runs on the host IO thread
  (`is_io_thread`) and logs when it does not.

### 15.6 Migration status of built-in plugins

- **Every kit-based plugin is bridged automatically**: the resumption paths of
  `tool`/`hook`/`capability`/`graph`/`spawn` and `sleep`/`call_tool`/`invoke_cap`/`yield`/
  `offload` all use the driver sequence; plugin business code needs no changes.
- **Migrated to controlled polling (`polled_tool`)**:

  | Plugin / tool | Rationale |
  |---|---|
  | `agentxx_websearch`: `web_search` / `web_fetch` / `web_fetch_markdown` | the bodies already were asio coroutines (`co_await HttpClient::*Async`); network waits no longer occupy the host worker pool and the per-instance HTTP keep-alive pool is reused |
  | `agentxx_execute_command`: `execute_bash_command` / `execute_windows_command` (Boost.Process v2 branch) | subprocess pipes and timers bind to the coroutine executor; concurrent commands share one poll sequence and one local reactor instead of each occupying a pool thread until its timeout |
  | `agentxx_filesystem`: `read` / `write` / `edit` | `asio::stream_file` async IO; file IO is genuinely asynchronous (availability decided by `utilxx_base::isAsyncFileIoSupported()`: compile-time macro plus a runtime io_uring probe), avoiding pool-thread occupation for large files |

- **Kept on `blocking_tool` (explicit exceptions)**:
  - `agentxx_filesystem`: `list` / `glob` / `grep` — directory traversal plus whole-file
    scanning plus regex/encoding conversion is CPU/blocking IO (asio has no async directory
    API); putting it under the pump would only block the instance's other tools;
  - `agentxx_execute_command`: the non-Boost.Process-v2 `popen` fallback (synchronous);
  - `agentxx_filesystem`: environments without asynchronous file IO (synchronous fallback;
    the registration side automatically switches back to `blocking_tool`) — availability is
    decided by `utilxx_base::isAsyncFileIoSupported()`: either asio file IO is not enabled
    at compile time (neither `ASIO_HAS_FILE` nor `BOOST_ASIO_HAS_FILE` defined), or on
    Linux/Android no io_uring ring can be created at runtime (the seccomp filter of
    containers/VMs — `Seccomp: 2` in `/proc/self/status` — blocks `io_uring_setup`, and old
    kernels return `ENOSYS`); the result is cached per process and tests may force it off
    via `setAsyncFileIoSupported(false)` to cover the synchronous fallback path;
  - `agentxx_rag_search`: the embedding network request path needs its implementation restored
    to a coroutine shape first (its comments record "the original asio coroutine interface was
    rewritten synchronously"), then it can migrate; chunking/similarity CPU parts keep using
    offload (**phase 2**).
- **Unchanged (no private reactor waits)**: codegraph / planning / system_monitor / math /
  string / system / JS-family plugins; the JS engine already is the correct "own thread +
  notify" shape.
- **`ClientPluginManager`'s `asio::thread_pool(1)`** only performs unavoidable blocking dynamic
  library work (`dlopen`/entry; registrations inside entry are still dispatched back to the IO
  thread through the vtable) and stays an explicit, closable background facility. Client
  plugins create no coroutine roots that need driving, so the client kit does not create a
  bridge (the host already exposes the same IID for future use).
- **Samples**:
  - `example_bridge`: both real wake sources (host timer + host callback-style interop) plus
    bridge diagnostics;
  - `example_polled_timer`: the **controlled polling** sample — `co_await`-ing three asio
    `steady_timer` waits on the plugin's local executor and returning the measured elapsed
    time together with `driverAvailable/onHostIoThread/pumpOnStart`.

### 15.7 Verification

| Layer | Coverage |
|---|---|
| C ABI | `test_plugin_abi_c17.c`: driver table 8-byte alignment, `version/struct_size` offsets, version value; item-by-item C++ cross-check (`plugin_runtime`) |
| Host tickets | `plugin_runtime`: never inlined, at most one execution per ticket, no execution after cancel, lease held while queued, idempotent cancel, forged handle safely ignored, `Closing` admitted / `Closed` rejected, null callback returns `NULL + error_out` |
| Kit bridge | `plugin_bridge` (fake host implementing the C ABI driver): no inlining, one `poll_one` per ticket, no spinning when idle, wake preserved in all three windows, no re-entrancy from host callbacks, single terminal state on cancel, refused driver finalises roots, stop cancels queued tickets, multi-instance isolation |
| Kit controlled polling | `plugin_bridge`: first step never inlined; ticket requested immediately on progress; exactly one 10ms backoff when idle (no extra ticket); polling stops when the root finishes (in-flight backoff cancelled); cancel cancels the in-flight backoff and yields a single `CANCELLED` terminal state; `stop` finalises in-flight polled roots as `FAILED` exactly once and releases the `Job`; the burst cap produces a 1ms yield |
| End-to-end | `plugins`: `example_bridge` and `example_polled_timer` through the real host asserting `driverAvailable/onHostIoThread/pumpOnStart`, that host work keeps progressing while the plugin is suspended (same IO sequence) and that the native asio timer really expires; plus the 1000-concurrent tool-call stress case |
| End-to-end (migrated plugins) | `plugins`: `agentxx_filesystem` read/write/edit (polled) alongside list (offload) in one instance; `agentxx_websearch` fetching through a local loopback HTTP server including 6 concurrent requests (non-blocking); `agentxx_execute_command` unloaded while `sleep 5` is in flight — cancel drain, pump stop, inflight reaching zero well before the command's own timeout |
| Memory | `plugin_bridge` alone reports 0 leaks; plugin-focused ASan+LSan matches the pre-refactor baseline item by item (4480 bytes / 64 allocations) and is unchanged after adding the controlled-polling cases (in-flight unload / abandoned path) |
