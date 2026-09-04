> 文档自动翻译自[zh-cn](/docs/zh-cn/)版 (This document is automatically translated from the [zh-cn](/README.md) version.)

# Agentxx
[Github agentxx](https://github.com/coolight7/agentxx)

- **README.md**: [中文 zh-cn](/README.md) | [English en](/docs/en/README.md)

```text
 █████╗  ██████╗ ███████╗███╗   ██╗████████╗      ╔══╗     ╔══╗
██╔══██╗██╔════╝ ██╔════╝████╗  ██║╚══██╔══╝   ╔══╬══╬═════╬══╬══╗
███████║██║  ███╗█████╗  ██╔██╗ ██║   ██║    ╔═╬  ║++║     ║++║  ╬═╗
██╔══██║██║   ██║██╔══╝  ██║╚██╗██║   ██║    ╚═╬       \_/       ╬═╝
██║  ██║╚██████╔╝███████╗██║ ╚████║   ██║      ╚═══════   ═══════╝
╚═╝  ╚═╝ ╚═════╝ ╚══════╝╚═╝  ╚═══╝   ╚═╝         ╚══╝     ╚══╝   
```
- An AI Agent implemented asynchronously using C++ coroutines, compilable as a `single executable` or `dynamic shared library` for direct execution. Designed for resource-constrained devices like standard PCs and mobile phones, minimizing memory footprint, binary size, and shedding heavy dependencies like shared dynamic runtimes, Python, and Node.js.
- Designed to be embedded into apps to provide high-performance Agent capabilities, alongside an out-of-the-box CLI / TUI Code Agent. GUI clients are planned to be powered by [Lumenxx](https://github.com/coolight7/lumenxx-docx), with upcoming support for audio/video processing and automation control Agents.
- Tested and verified running single-turn autonomous tasks continuously for over 5 hours. Agentxx itself has actively participated in its own codebase development.

> The agent core, server services, TUI, plugin interface, and FFI interfaces are preliminarily implemented, but may still undergo significant refactoring and API adjustments.

- [Features](#features)
- [Compatibility](#compatibility)
    - [Cross-Platform Support](#cross-platform-support)
    - [Binary Size and Dependencies](#binary-size-and-dependencies)
- [Roadmap & Planned Features](#roadmap--planned-features)
    - [Core Modules](#core-modules)
    - [Prompt Training](#prompt-training)
    - [Plugin Support](#plugin-support)
    - [FFI Dynamic Library Interface](#ffi-dynamic-library-interface)
    - [Capabilities](#capabilities)
    - [Testing](#testing)
- [Directory Structure](#directory-structure)
- [Compilation](#compilation)
- [Configuration & Running](#configuration--running)

## Features
- **Asynchronous C++ Coroutine Architecture**: Compact binary size and low memory consumption with high performance. Non-blocking coroutine network and file I/O, with optional hardware acceleration libraries such as Hyperscan.
- **Data Privacy & Security**: Agentxx does not upload your data. When connected to a local area network LLM API server, it can run completely offline. Note: Agentxx cannot guarantee the safety of third-party LLM APIs, MCP servers, or Skills; please audit them before importing.
- **Cross-Platform Compatibility**: Enhanced Windows compatibility. Under WSL, you can execute Windows commands directly, launch Windows programs, and automatically translate file paths.
- **Rich Built-in Tools**: Built-in file I/O, command-line execution, task planning, and more. Highly composable at compile time, with automatic correction of LLM argument types and character encodings.
- **C++ Plugins / JS Plugins / FFI Support**: Includes codegraph code analysis, system CPU/GPU/RAM monitoring, screen capture, mouse text selection event streaming, and more. JavaScript plugins can be loaded via the C++ QuickJS plugin.
- **Decoupled UI & Agent Architecture**: Native support for TUI, CLI, GUI integration, WebSocket API, FFI invocations, and dynamic/static embedding into applications. Supports single-process mode or separate processes for the UI client and the Agent WebSocket server.
- **Automatic Interruption and Error Recovery**: Robust long-running stability, network retry mechanisms, dynamic timeouts, role order validation and correction in message contexts, automatic character encoding repair, automatic retry on empty responses, and detection of repetitive tool call loops.

## Compatibility
### Cross-Platform Support
- Can be compiled into standalone executables or dynamic libraries without extra dynamic dependencies, relying only on basic system libraries.
- Supported operating systems:

| Status | System | Notes |
|---|---|---|
| ✅ | Windows 10+ | Windows MSVC native build / Linux cross-compilation |
| ✅ | Linux | Under WSL, additionally supports executing Windows programs and commands directly |
| ✅ | Android 5.0+ | Linux cross-compilation |
| ⬜ | macOS | Compatibility testing pending |
| ⬜ | iOS | Compatibility testing pending |

### Binary Size and Dependencies
- Both the executable `agentxx_cli` and the shared library `libagentxx` statically link dependencies where possible to minimize dynamic library requirements. Symbol visibility control and pruning are applied during build.
- Below are measurements with `only agentxx compiled, unneeded dependencies stripped`, tested on `Date: 2026/08/22, commit: b35b226399062dc1196bced06d2c4f209de9e0fa`:
- Memory Footprint:

| agentxx_cli Target | Init RAM | 100K Context | 200K Context | Notes |
|---|---|---|---|---|
| **Win / TUI** | 3.1 MB | 12.2 MB | 19.6 MB | Measured via Task Manager |
| **Linux / TUI** | 1.9 MB | 8.9 MB | 14.1 MB | RES-SHR measured via `top`. Only depends on system libs, so measures exclusive memory |

- Executable & Shared Library Binary Sizes:

| System | agentxx_cli | libagentxx | Compiler | Notes |
|---|---|---|---|---|
| **Windows** | 18.3 MB | 12.6 MB | MSVC 19.51.36247.0 / Visual Studio 18 2026 · x86_64 · -O2 | Recommend bundling MSVC runtime |
| **Linux** | 13.3 MB | 17.8 MB | GCC 16.1.0 · x86_64 · -O3 · --strip-all | Recommend bundling libstdc++.so.6, libgcc_s.so.1 |
| **Android** | - | 14.0 MB | NDK-r29 · Clang 21.0.0 · android-21-arm64-v8a · -O3 · --strip-all | Recommend bundling libc++.so |

- Built-in Plugin Shared Library Sizes (`-` indicates unsupported on this platform):

| Plugin | Windows (.dll) | Linux (.so) | Android (.so) | Notes |
|---|---|---|---|---|
| agentxx_codegraph | 36.9 MB | 38.2 MB | 37.6 MB | Code analysis & navigation to quickly locate symbol definitions and references |
| agentxx_computer_use | 379 KB | - | - | Provides mouse and keyboard control tools |
| agentxx_javascript_engine | 1.1 MB | 1.2 MB | 1.1 MB | JS execution engine allowing custom JavaScript plugins |
| agentxx_screen_capture | 383 KB | - | - | Captures screen image frames |
| agentxx_system_monitor | 428 KB | 629 KB | - | Reads system CPU, RAM, and GPU utilization |
| agentxx_text_selection_monitor | 399 KB | - | - | Listens to system-wide text selection event streams |
| agentxx_string | 8.9 MB | - | - | String processing and HTML-to-Markdown conversion |
| agentxx_rag_search | 6.7 MB | - | - | RAG retrieval |
| agentxx_math | 346 KB | - | - | Mathematical computation tools |

- Default compilation optimizations favor performance. For minimal binary footprint, optional dependencies like Hyperscan/Boost.Process can be omitted and `-Os`/`-Oz` optimization flags applied.

## Roadmap & Planned Features
### Core Modules
- **Toolcall**
    - ✅ Automatic character encoding conversion of return values to UTF-8
    - ✅ Repetitive loop call detection on selected tools
    - ✅ Output interception: automatically compresses and saves truncated summaries to `agentxx_share_store` when exceeding length limits
    - ✅ Automatic type coercion (interconverting String, Array, Number) to enhance LLM tolerance
    - ⬜ Asynchronous / chunked streaming result retrieval based on `Event Streams`
    - ✅ filesystem (`Synchronous` / `asio io_uring / IOCP coroutine async` file I/O with timeout enforcement)
        - list (file/dir/recursive-dir/limit)
        - read (full / offset-limit)
        - write
        - edit
        - glob
        - grep (multi text/regex + multi-filepath)
        - Automatic Windows path translation under WSL environment
        - Automatic UTF-8 conversion when reading file content
        - ⬜ Preserve original file encoding when writing files
    - ✅ execute_command (`Synchronous` / `Boost.Process coroutine async` execution with timeout enforcement)
        - execute_bash_command
        - execute_windows_command (automatically enabled under WSL/Linux to execute Windows commands directly)
        - Timeout enforcement
        - Distinguishes stdout and stderr, auto-converting output encoding to UTF-8
    - ✅ web_search (asio coroutine async network requests)
        - web_search (built-in HTML to Markdown conversion, supports regular web search APIs)
        - web_fetch_url_markdown (HTML to Markdown)
        - web_fetch_url (raw response body)
        - ⬜ Subagent integration with external LLM search APIs
    - ✅ planning
        - Two-level task planning (Strategic Roadmap + Tactical Tasks) + Notes/Memo
        - Mermaid / stateDiagram-v2 state machine diagrams describing macro task flow
        - todos describing immediate granular next steps
    - ✅ RAG
        - ✅ Text splitting and chunking with default 20% adjacent chunk overlap
        - ✅ Splitting strategies:
            - Fixed-length chunking
            - Character-based chunking
            - Structural chunking (recursive splitting for oversized chunks)
            - ⬜ Semantic chunking
    - ✅ Sub-Agent (supports concurrent coroutine execution while preserving return order; default `subagent_task` with batched `tasks` parallelism)
    - `tool_skill_search` logic inlined as a lazy-retrieval template (not a standalone tool)
    - ✅ get_current_datetime (retrieves system timestamp, local time, and UTC time)
- ✅ **Tree-Messages**
    - agentxx_share_store (allows storing and retrieving variables across LLM messages, skills, and tools)
        - Supports `line_offset` / `line_limit` paginated text reading
        - Automatically offloads pruned messages to `agentxx_share_store` during context compaction
        - Automatically intercepts tool/subagent outputs; large responses are stored in `agentxx_share_store` with summary and ID left in context
    - Message branching, supporting historical message edits and model regeneration
    - Multi-session management and session history
- ✅ **EventBus Event Stream**
    - Event registration and subscription mechanism notifying subscribers upon trigger
    - Preset features:
        - Interruption handling
        - Timed notifications
        - Periodic delayed loop notifications
    - ⬜ Asynchronous task completion notifications for LLM tasks
    - ⬜ Chunked streaming output for task results
    - ⬜ External application notifications and data insertion
- ✅ **Interruption and Recovery**
    - Powered by the `Event Stream`, supports initiating interruptions within Nodes or toolcalls, awaiting user response, and resuming execution. Reruns pre-interruption node logic without repeating successfully completed toolcalls.
    - Supports multiple simultaneous toolcall interruptions, and iterative `Interrupt - User Response` rounds.
    - Human-in-the-Loop (HITL) support with customizable handlers (built-in confirmation prompts, arbitrary user inputs, etc.).
    - Supports user-initiated execution cancellation.
- ✅ **Permission Control** (`PermissionMiddleware`)
    - Built atop `Interruption and Recovery`. Intercepts tool invocations before execution to allow, deny, or trigger an interactive user prompt.
    - Built-in file read/write permission rules.
    - ⬜ Sandboxed execution for Shell and File I/O.
- ✅ **Exception Handling and Auto-Retry**
    - Automatic parameter type conversion in Toolcalls (String, Array, Number).
    - Automatic retry on Toolcall / LLM nodes with configurable retry counts.
    - On node exceptions, automatically determines whether to retain generated messages and repairs context role sequence.
    - Validates and fixes role ordering and content at the start of each turn.
    - Ensures correct order of execution and error propagation across Middlewares.
- ✅ **Sub-Agent**
    - Implemented as a `Toolcall`, allowing the LLM or code to launch SubAgents asynchronously.
    - Concurrent toolcalls enable parallel execution of multiple SubAgents.
    - Built-in implementations:
        - subagent_task (isolated context execution, default registered name; supports batched parallel delegation via `tasks`).
- ✅ **Middleware**
    - Hierarchical stack interception: Executes `start` in order, pushes corresponding `end` onto stack, and unwinds stack upon completion: `agentCallStart`, `agentCallEnd`, `modelCallStart`, `modelCallEnd`, `toolCallStart`, `toolCallEnd`.
    - Actual stack: SubagentManager → Summarization → Permission → Skill → MemoryFile → LogPrint.
- ✅ **Task Planning** (`agentxx_planning` dual-sided plugin)
    - Two-layer planning.
    - Mermaid stateDiagram-v2 state machine diagrams representing overall milestones.
    - todo_list tracking immediate tactical execution items.
    - Plans persist under `{dataDir}/plans/`; client renders via tool decor + Info section.
- ✅ **Context Compaction** (`SummarizationMiddleware`)
    - Token usage tracking / heuristic estimation; initiates automatic compaction at configured thresholds.
    - Tool-specific compaction handlers:
        - Prunes obsolete historical messages (filesystem I/O, planning state, share_store variables).
    - Offloads critical long messages to `agentxx_share_store` instead of lossy compression, retrievable on demand.
    - LLM-assisted summarization.
    - Preserves recent conversation window.
- ✅ **Memory and Context Management**
    - ✅ Custom YAML configuration for loading Memory files.
    - ✅ SQLite persistence for session context and recovery after process restarts.
    - Summarized shared memory.
- ✅ **Skill Support** (`SkillMiddleware`)
    - Directory scanning, metadata collection, file inspection via `filesystem`, and execution via `execute_command`.
- ✅ **MCP Support**
    - Broad protocol compatibility (2024-11-05 ~ 2026-07-28).
    - MCP Client:
        - Namespace isolation
        - Configurable timeout limits
        - HTTP / stdio transports
    - MCP Server:
        - ⬜ CodeGraph
        - ⬜ Websearch
- ⬜ **A2UI Support**
    - Unified client plugins utilizing A2UI data structures.
- ⬜ **Self-upgrade**
    - Autonomous loop to evaluate and refine system prompts and tool descriptions.
    - Automated testing.
    - Idle-time optimization of skills and prompts.
- ✅ **LLM API Integrations**
    - OpenAI API
    - OpenAI Responses API
    - Anthropic API
    - Captures reasoning/thinking tokens, echoes Thinking content, supports encrypted Thinking messaging.
    - Configurable `BaseUrl`, `ApiKey`, `ModelName`, and `ExtraConfig`.
    - Broad schema compatibility, empty response checks, and automatic retries.
- ✅ **Custom Configuration**
    - Loads configuration from `agentxx-config.yaml` and `.env` at startup.
    - YAML configuration supports configuring Memory, MCP, Skills, and Plugins.
    - Decoupled System / Tool Prompts into standalone configs to facilitate customization and `Self-upgrade` tuning.
- ✅ **Network Timeouts & SSL Verification**
    - Configurable connection timeouts and dynamic timeouts (dynamically calculated based on payload size and streaming chunk intervals).
    - Option to disable SSL verification.
- ✅ **Queued User Input**
    - Server-IO user input queue; automatically feeds pending inputs once the active turn completes. Client UI can trigger immediate interruptions to inject messages.

### UI
- ⬜ GUI
- ✅ TUI: `agentxx_cli tui`
    - Client / plugin architecture.
    - Light and Dark themes.
    - Local pagination for message histories and session lists.
    - Displays thinking process, streaming text, Markdown formatting, and state diagrams.
    - Configurable streaming render modes for thinking blocks (single-line tail clipping vs. auto-expanded full view, folding upon completion).
    - Displays LLM token speeds, latency, and turn finish timestamps.
    - Configurable animation levels.
- ✅ CLI: `agentxx_cli cli`
    - Primarily intended for auxiliary debugging and testing.

### Server
- ✅ MCP Server
- ✅ ACP Server
- ✅ A2A Server

### Prompt Training
- System and tool prompts substantially impact LLM performance, especially for local small models. The `Prompt Training` module is tailored to optimize prompts for specific models.
- Through cyclical multi-turn `Generate -> Evaluate -> Refine` loops, custom prompts are trained for targeted models and dynamically loaded at runtime based on `ModelName`.
- ✅ Implemented cyclical prompt training workflow.
- ⬜ Train general-purpose prompt suites + model-specific prompt adaptations.
- ⬜ Dynamic loading based on `ModelName`, falling back to default prompts when unmatched.

### Plugin Support
- ✅ C/C++ plugin support allowing extensible modifications to both Agent and Client UI. See [Plugin Documentation](/docs/en/design.md/plugins.md); [Built-in Plugins](/agent/plugins/); [Example Plugin](/agent/plugins/example_plugin/).
    - Can be compiled either as standalone dynamic shared libraries or statically embedded into `libagentxx`.
    - High compatibility achieved via pure C APIs, COM-style querying, explicit 8-byte structure alignment, fixed-width integer types (`int32_t`, `int64_t`), unified calling conventions, and passing structs by pointer rather than value. This ensures runtime compatibility across different compilers, standard library implementations, and dependency versions; validated by mixing Debug/Release binaries of `agentxx_cli` with plugin dynamic libraries.
    - Native asynchronous interfaces supporting cooperative non-blocking coroutine execution between host and plugins within a single thread without locking.
- Multi-Language Plugin Ecosystem:
    - Modelled after `agentxx_javascript_engine`, execution engines can be built as plugins. New plugins can depend on the engine to execute code snippets at runtime, or directly invoke host-installed `nodejs`, `python3`, etc.
    - ✅ `agentxx_javascript_engine`: C++ plugin offering JS plugin extensibility. [JS Plugin Example](/agent/plugins/example_js/).
    - ⬜ `agentxx_python_engine`
- ✅ `agentxx_codegraph`:
    - Code symbol parsing, indexing, and lookup.
    - Saves analysis indices to SQLite.
    - Configurable inclusion/exclusion directories.
    - Automatically respects `.gitignore` rules and `.gitmodules` submodules by default (`use_gitignore` configurable).
- ✅ `agentxx_system_monitor`: Reads CPU, RAM, GPU, and VRAM utilization on Windows and Linux.
    - Tool: `get_system_core_info`.
- ✅ `agentxx_screen_capture`: Screen frame capture using DXGI / GDI.
- ⬜ `agentxx_audio_stream`: Captures system audio output, target process audio, and microphone input.
- ✅ `agentxx_text_selection_monitor`: Listens to system-wide text selection events across applications and browsers.
- ✅ `agentxx_computer_use`: Mouse and keyboard control on Windows.
- ⬜ PaddleOCR (image-to-text extraction).
- ⬜ SD.cpp (image and video generation).
- ⬜ FunASR (speech recognition).
- ⬜ Qwen3-TTS (text-to-speech).

### FFI Dynamic Library Interface
- ✅ [FFI C API Symbols Export](/agent/ffi/); [Design Document](/docs/en/design.md/ffi.md); [Examples](/agent/example/ffi/).
- Allows other programming languages to invoke `libagentxx` to create agents, run sessions, and manage tools via SDK wrappers or direct dynamic library loading (`dlopen` / `LoadLibrary`).
- Language SDKs:
    - ✅ Flutter / Dart; [SDK](/agent/ffi/dart/); [Example](/agent/example/ffi/dart/).
    - ⬜ JavaScript
    - ⬜ Python
- Difference between Plugins and FFI: With Plugins, Agentxx is the host process that loads third-party plugins and invokes them at specific lifecycle hooks. With FFI, the third-party application acts as host, embedding Agentxx as an AI engine library.

### Capabilities
- ✅ **Mouse & Keyboard Automation**
    - Implemented via `agentxx_computer_use` plugin.
- ⬜ **Translation / Selection Translation**
    - Screen text OCR, allowing copy, analysis, and translation.
- ⬜ **Visual Grounding & Content Annotation**
    - Generates interactive prompt points over images linking to annotations or expanded explanations.
- ⬜ **Commentary / Danmaku Generation for Media**
- ⬜ **Image & Video Generation**
    - Prompt enhancement via LLM, automated output verification, and prompt iterative refinement.
- ⬜ **ASR / TTS**
- ⬜ **Lyric Synchronization**
- ⬜ **Live2D / 3D Model Motion Control**
- ⬜ Compile selected extensions into standalone executables to support WSL integration.

### Testing
- Agent Core Reliability Tests:
    - ✅ Preserves context role ordering and message integrity across node interruptions and exceptions.
    - ✅ UTF-8 validation and auto-repair.
    - Crash recovery on sudden termination and restart:
        - ✅ Resumes interruption handlers and execution from interrupted nodes.
        - ✅ Automatically repairs role ordering in message history.

## Directory Structure
- For detailed architecture, see [design.md](/docs/en/design.md/index.md).
- `agent`:
    - C++ Agent implementation.
    - Modular extensions, AI inspection, and regression tests built on core framework.
- `agent/script`:
    - Build scripts for verified platforms. See [Build Guides](/docs/en/build/).
- `agent/lib`: libagentxx
    - Core library containing toolcalls, nodes, middlewares, etc., cleanly decoupled for embedding.
- `agent/client`: agentxx_cli
    - Executable client powering CLI, TUI, and server initialization.
    - `agent/client/include/agentxx-client/io`: Implements stdio and TUI Agent invocation.
    - `agent/client/include/agentxx-client/train`: Prompt training module.
- `agent/plugins`:
    - Built-in plugins.
- `agent/ffi`:
    - FFI symbol exports for `libagentxx` shared library integration.
- `agent/example`:
    - Examples and demos.
- `agent/test`: agentxx_test
    - Test suites.
- `agent/third_party`:
    - `neograph`: Graph execution engine.
        - [Original Repository](https://github.com/fox1245/NeoGraph)
        - [Fork & Modifications](https://github.com/coolight7/NeoGraph):
            - Toolcall: Defaulted to asynchronous execution; enabled concurrent execution; added `thread_id` parameter; added async support to `McpTool`.
            - `NodeInput`: Allowed modifying state to manipulate message contexts.
            - `ChatMessage`: Added modification history tracking for message edits and model regeneration.
            - `LLMCallNode`: Skips injecting redundant system messages when one is already present.
            - `GraphState`: Added `overwrite` method for mandatory variable assignment.
    - `codegraph-cpp`: Code and Markdown relationship parser.
        - [Original Repository](https://github.com/plutoaac/codegraph-cpp)
        - [Fork & Modifications](https://github.com/coolight7/codegraph-cpp):
            - Expanded parsing from C++/Python to 20+ languages and formats including JS, TS, Dart, Rust, Go, Java, Kotlin, Bash, and Markdown.
            - Added Windows build and runtime support.
    - `Regex Engine Support`: Selectable via CMake configuration:
        - Hyperscan: Supported on x86 Windows / Linux.
        - std::regex: Fallback.

## Compilation
- C++ Standard: Requires C++26+.
- Recommended Compilers:
    - Linux: GCC 16.1. (Certain coroutine functions caused compiler ICE with GCC 13.2).
    - Windows: MSVC / Visual Studio 2026. (Older versions like VS 2022 unverified).
- Clone repository and submodules:
```sh
git clone https://github.com/coolight7/agentxx
cd agentxx
git submodule update --init
```
- Verify dependencies in `{PROJECT_ROOT}/agent/third_party/`. If empty, rerun `git submodule update --init`.
- Install codegraph-cpp dependencies:
```sh
cd {PROJECT_ROOT}/agent/third_party/codegraph-cpp
npm install --legacy-peer-deps
```
- Select target build guide:
    - [Linux / WSL Executable / Shared Library (.so) / Static Library (.a)](/docs/en/build/linux.md)
    - [Android Shared Library (.so) / Static Library (.a)](/docs/en/build/android.md)
    - [Windows Executable (.exe) / Shared Library (.dll) / Static Library (.lib)](/docs/en/build/windows.md)
- Generated Library Linking:
    - Shared library: `libagentxx` (Debug adds `d`: `libagentxxd`). Unified multi-platform naming, differing only in extensions (`.so`, `.dll`, `.dylib`).
    - Static library: `libagentxx_static` (Debug adds `d`: `libagentxx_staticd`). Unified naming across platforms (`.a`, `.lib`). Supports static linking of all dependencies to produce standalone executables like `agentxx_cli` (verified on Linux and Windows).
    - Builds default to shared `libagentxx` and static `libagentxx_static`, dynamically linking runtime libraries (libstdc++, libgcc, msvcrt `/MD` | `/MDd`).

## Configuration & Running
- Edit `agentxx-config.yaml` in `{PROJECT_ROOT}` to configure your model API credentials. Run `agentxx_cli` from that directory.
- (Optional) Store API keys in `.env` in the same directory. Copy `.env.example` to `.env` and configure environment variables.
```sh
cd {PROJECT_ROOT}
# Edit agentxx-config.yaml
# Optional:
#       cp .env.example .env
#       Edit .env with your API keys

# client handles UI rendering and user I/O.
# server-io runs the agent-loop server, executing turns and invoking LLM APIs.
# agentxx_cli can launch UI+server together, UI connecting to remote server, or server standalone.

# Single-process mode (Client + Server-IO in one process):
agentxx_cli tui # Launches TUI interface + server-io
agentxx_cli cli # Launches CLI + server-io

# Separated processes via WebSocket:
# Useful for custom GUI clients connecting to the server.
agentxx_cli server --host 0.0.0.0 --port 7007 --token passwd # Starts server-io
agentxx_cli tui --agent ws://127.0.0.1:7007/agent --token passwd # Starts TUI client connecting to server-io
```

## LICENSE & THIRD_PARTY
- [MIT License](LICENSE)
- Open source licenses of statically/dynamically linked third-party libraries apply according to their terms.
- Thanks to the following open source projects:
    - [boost](https://github.com/boostorg/boost) (asio, beast, process, exception)
    - [codegraph-cpp](https://github.com/plutoaac/codegraph-cpp)
    - [curl](https://github.com/curl/curl)
    - [fmt](https://github.com/fmtlib/fmt)
    - [FTXUI](https://github.com/ArthurSonzogni/FTXUI)
    - [glob](https://github.com/p-ranav/glob)
    - [html2md](https://github.com/tim-gromeyer/html2md)
    - [hyperscan](https://github.com/intel/hyperscan)
    - [iconv](https://www.gnu.org/software/libiconv/) | [libiconv-native](https://github.com/hesphoros/libiconv-native)
    - [liburing](https://github.com/axboe/liburing)
    - [MarkdownFTXUI](https://github.com/coolight7/MarkdownFTXUI)
    - [NeoGraph](https://github.com/fox1245/NeoGraph)
    - [OpenSSL](https://www.openssl.org/)
    - [simdjson](https://github.com/simdjson/simdjson/)
    - [sqlite3](https://github.com/sqlite/sqlite) | [sqlite3-cmake](https://github.com/sjinks/sqlite3-cmake)
    - [uchardet](https://www.freedesktop.org/wiki/Software/uchardet/)
    - [yaml-cpp](https://github.com/jbeder/yaml-cpp)
    - [zlib](https://github.com/madler/zlib) | [zlib-ng](https://github.com/zlib-ng/zlib-ng)
