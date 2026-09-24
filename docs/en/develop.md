# Development Guide

> Related: [design](/docs/en/design/index.md) (Architecture) · [plugins.md](/docs/en/design/plugins.md) (Plugins) · [ffi.md](/docs/en/design/ffi.md) (FFI)

## 1. Testing

### Running Tests

```bash
# Run all tests (synchronous + asynchronous, including client side)
./agent/build/linux-debug/exec/agentxx_test

# Stop immediately upon failure (fail-fast)
./agent/build/linux-debug/exec/agentxx_test -f

# Run specified test modules only
./agent/build/linux-debug/exec/agentxx_test string_util regex agent plugins
```

Test module names are listed in the registry table at the top of `agent/test/test.cpp`. Under `AGENTXX_BUILD_CLIENT`, 17 additional client-side modules are also compiled: `config_loader` `tui_settings` `update_check` `tui_input` `tui_interrupt` `tui_scroll` `tui_sidebar` `tui_context_overlay` `tui_form` `tui_stream` `tui_surface` `tui_theme` `tui_tool_header` `tui_ui_items` `tui_widget` `sessionId` `mermaid_state`.
The synchronous group also contains `json` `json_view` `json_reflection` `interrupt_ui` `ui_items` `plugin_runtime` `plugin_sdk` `plugin_bridge` (see the synchronous section of `test.cpp`).

### Conventions for Adding New Test Modules

- Header location: `agent/test/include/agentxx-test/<core|plugin|client>/test_xxx.h` (same module name as `agent/test/<core|plugin|client>/test_xxx.cpp`); the shared `test_framework.h` lives directly under `agent/test/include/agentxx-test/`.
- Includes always carry the `agentxx-test/` prefix (the only include root is `agent/test/include/`): `#include "agentxx-test/core/test_xxx.h"` / `#include "agentxx-test/test_framework.h"`. Source directories are no longer include roots, so test headers can never clash with same-named headers from other libraries.
- Headers must only contain function declarations. Assertion counters should be defined in anonymous namespaces within `*.cpp`, with `#define XX_TEST_PASSED g_xxx_passed` / `XX_TEST_FAILED` mapping to `test_framework.h` macros in the `cpp` file; end with `return TestResult{g_xxx_passed, g_xxx_failed};`.
- Macro overrides or `extern` exporting of counters in header files are strictly forbidden (macro leakage across translation units has previously caused counter mix-ups across modules).
- Signature for async test modules: `asio::awaitable<TestResult> run_xxx_tests()`; for sync test modules: `TestResult testXxx()`.

### Mock LLM Server (DaSimServer)

`DaSimServer` provided by `test_agent.h` serves as a shared mock LLM Server reused by other modules (`agent_host/session_persistence/remote_agent/cancel/memgrowth`, etc.). It supports returning `content/thinking/tool_calls` as SSE streams based on request bodies.

## 2. Adding Tools and Middlewares

- **Built-in Tools**: All have been migrated to plugins (`agent/plugins/agentxx_*`). New tools should preferably be implemented as plugins (registered via `tool/fast_tool/blocking_tool` in `plugin_kit.h`), keeping identical names and behavior; unit tests directly test the same `*_impl.h` implementation.
- **Middlewares**: Inherit from `MiddlewareWrapHandle<TState>` and select the hooks to mount via `MiddlewareHooks` (`onAgentcallStart/End`, `onModelcallStart/Run/End`, `onToolcallStart/End`; unmounted stages never run). Register them in stack order in `BaseAgent::initMiddleware` / `CodeAgent::initMiddleware`; middlewares' own toolcalls are collected automatically by `initMiddlewareTools`.
- **Permissions**: Filesystem permissions use longest-prefix matching (`utilxx::XXRouter`) + wildcards `*` in `PermissionMiddlewareHandle`. The default rule comes from `permission.mode`. Plugins declare the permission restrictions of their own tools through the `agentxx.agent.permission` interface table after registering them; tools without a declaration are not checked at all.
- **Tool errors**: Throw `std::invalid_argument` for argument-check failures and `std::runtime_error` for runtime errors — `ToolcallWrapNode` formats them uniformly into `[Exception aborted: ...]` result text. Do not return `{"error": ...}` payloads (see [plugins.md](/docs/en/design/plugins.md) §8).

## 3. Plugin Development

- **Declarative Export**: Prefer `AGENTXX_PLUGIN_AGENT_EXPORT` (client side `AGENTXX_PLUGIN_CLIENT_EXPORT`), which automatically generates C entry points with ABI exception guards and manages context lifecycle.
- **Fluent Schema & Tolerant Reader**: Declare tool argument schemas via `ctx.schema(toolName)` (auto-merged with host `toolPrompt`); use `ArgReader` inside execution callbacks for error-tolerant and type-safe argument extraction.
- **Event-Driven Cancellation**: Register cancellation actions for subprocesses and long-running tasks via `ctx.cancelRegistry.registerCallback(sessionKey, cb)` (yielding an RAII `ScopedRegistration` guard) for millisecond-level instant termination.
- **Follow the Three Iron Rules**: No mutable global statics / State recovered via `user_data` closures / Interface tables cached into instance context.
- **Record Failed Components**: External resource failures (MCP, Skills, Memory) should be recorded in `appendComponentInfo.failedComponents` for client UI reporting.
- **Reusing `cxx_utilxx_base` / `cxx_utilxx`**: Built-in plugins use `find_package(cxx_utilxx_base|cxx_utilxx)` + `target_link_libraries(PRIVATE cxx_utilxx_base_static|cxx_utilxx_static)` (third-party plugins need only the pure C ABI header).
- **Platform Matrix**: Evaluated at the start of each plugin's `CMakeLists.txt` via `plugin_platform_support.cmake`.

For details, see [plugins.md](/docs/en/design/plugins.md).

## 4. Debugging and Logging

- Uniformly use `XX_LOG*` (see `agent/third_party/cxx_utilxx_base/include/utilxx_base/log.h`) rather than `std::cout/cerr`, avoiding interference with TUI rendering.
- `TUILogSink` connects to the right-hand log panel; `TestWarnErrorLogSink` exposes Warn/Error to stderr during tests.
- For catching exceptions, prefer `agentxx::util::catchError/catchErrorAsync` (which lets `CancelledException/NodeInterrupt` pass through). Never swallow cancellations with `catch(...)` in coroutines.

## 5. Network and Reconnection Testing

You can use proxies like `clash` to manually sever network connections at runtime to test automatic reconnection and incremental replay:

```sh
export http_proxy=http://127.0.0.1:7980
export https_proxy=http://127.0.0.1:7980
agentxx_cli tui
# Close the agent connection in Clash's "Connections" tab, and observe reconnection, Hello (lastSeq/tailHash), and Delta deduplication.
```

## 6. Encoding and Paths

- Prefer `std::string_view` over `const std::string&`.
- Paths must uniformly use `toCurrentSystemAbsolutePath` / `normalizePermissionPath` from `string_util` for `~/ ${VAR}` expansion and `unix/windows/auto` normalization. The session working directory should always be retrieved via `AgentContext::getSessionWorkDir(sessionId)`.

## 7. Build Acceleration and Troubleshooting

- Debug builds enable `ccache` + `mold/gold` + `PCH` + single-pass 62-source-file compilation by default. These can be overridden via environment variables / CMake options (see `build/linux.md`).
- On compiler ICE, retry first or clear page cache (`echo 3 > /proc/sys/vm/drop_caches`). Only investigate code if repeated failures occur.
- Windows forbids `/FS` and `/MP` flags (see AGENTS.md). Do not manually edit files in `build` directories (they may be overwritten).
