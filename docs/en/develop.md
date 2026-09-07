# Development Guide

> Related: [design.md](/docs/en/design.md/index.md) (Architecture) · [plugins.md](/docs/en/design.md/plugins.md) (Plugins) · [ffi.md](/docs/en/design.md/ffi.md) (FFI)

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

Test module names are listed in the registry table at the top of `agent/test/test.cpp`. Under `AGENTXX_BUILD_CLIENT`, 11 additional client-side modules are also compiled: `config_loader/tui_settings/tui_input/tui_interrupt/tui_scroll/tui_sidebar/tui_context_overlay/tui_stream/tui_tool_header/sessionId/mermaid_state`.

### Conventions for Adding New Test Modules

- Headers must only contain function declarations. Assertion counters should be defined in anonymous namespaces within `*.cpp`, with `#define XX_TEST_PASSED g_xxx_passed` / `XX_TEST_FAILED` mapping to `test_framework.h` macros in the `cpp` file; end with `return TestResult{g_xxx_passed, g_xxx_failed};`.
- Macro overrides or `extern` exporting of counters in header files are strictly forbidden (macro leakage across translation units has previously caused counter mix-ups across modules).
- Signature for async test modules: `asio::awaitable<TestResult> run_xxx_tests()`; for sync test modules: `TestResult testXxx()`.

### Mock LLM Server (DaSimServer)

`DaSimServer` provided by `test_agent.h` serves as a shared mock LLM Server reused by other modules (`agent_host/session_persistence/remote_agent/cancel/memgrowth`, etc.). It supports returning `content/thinking/tool_calls` as SSE streams based on request bodies.

## 2. Adding Tools and Middlewares

- **Built-in Tools**: All have been migrated to plugins (`agent/plugins/agentxx_*`). New tools should preferably be implemented as plugins (registered via `tool/fast_tool/blocking_tool` in `plugin_kit.h`), keeping identical names and behavior; unit tests directly test the same `*_impl.h` implementation.
- **Middlewares**: Inherit from `MiddlewareHandleBase` and register in stack order in `CodeAgent::initMiddleware`. `onHandleStart/End` can be hooked into the three nodes: `agent_start/modelcall/toolcall`.
- **Permissions**: Filesystem permissions use longest prefix matching (`XXRouter`) + wildcards `*` via `PermissionMiddleware`. The default rule is determined by `permission.mode`. When introducing new protected resources, define a `category` and query through `service.permission`.

## 3. Plugin Development

- **Declarative Export**: Prefer `AGENTXX_PLUGIN_AGENT_EXPORT` (client side `AGENTXX_PLUGIN_CLIENT_EXPORT`), which automatically generates C entry points with ABI exception guards and manages context lifecycle.
- **Fluent Schema & Tolerant Reader**: Declare tool argument schemas via `ctx.schema(toolName)` (auto-merged with host `toolPrompt`); use `ArgReader` inside execution callbacks for error-tolerant and type-safe argument extraction.
- **Event-Driven Cancellation**: Register cancellation actions for subprocesses and long-running tasks via `ctx.cancelRegistry.registerCallback(sessionKey, cb)` (yielding an RAII `ScopedRegistration` guard) for millisecond-level instant termination.
- **Follow the Three Iron Rules**: No mutable global statics / State recovered via `user_data` closures / Interface tables cached into instance context.
- **Record Failed Components**: External resource failures (MCP, Skills, Memory) should be recorded in `appendComponentInfo.failedComponents` for client UI reporting.
- **Reusing `agentxx_util`**: Built-in plugins use `find_package(agentxx_util)` + `target_link_libraries(PRIVATE agentxx_util)` (third-party plugins need only the pure C ABI header).
- **Platform Matrix**: Evaluated at the start of each plugin's `CMakeLists.txt` via `plugin_platform_support.cmake`.

For details, see [plugins.md](/docs/en/design.md/plugins.md).

## 4. Debugging and Logging

- Uniformly use `XX_LOG*` (see `agent/lib/include/agentxx/util/log.h`) rather than `std::cout/cerr`, avoiding interference with TUI rendering.
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
