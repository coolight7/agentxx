# Agentxx 插件系统架构与开发规范 (API v1)

> 状态: 定稿实现（2026-08 API v1 架构：锚定协程模型 / 两件套协议 / COM 风格接口表 / 纯 C ABI）
> 关联文档: [design.md](design.md) (主程序架构设计)、[plugins.md](../zh-cn/plugins.md) (用户开发入门指南)
> 本文为 Agentxx 插件系统的**核心技术与架构设计文档**，以最新代码实现为准。

---

## 目录

- [1. 目标与不变量](#1-目标与不变量)
- [2. 核心模型：锚定协程](#2-核心模型锚定协程)
- [3. 线程契约、交错与生命周期安全](#3-线程契约交错与生命周期安全)
- [4. 纯 C ABI 契约 (v1)](#4-纯-c-abi-契约-v1)
- [5. plugin_kit.h 开发 SDK](#5-plugin_kith-开发-sdk)
- [6. 宿主侧核心实现](#6-宿主侧核心实现)
- [7. 内置插件迁移矩阵](#7-内置插件迁移矩阵)
- [8. 统一脚本插件模型 (QuickJS 引擎)](#8-统一脚本插件模型-quickjs-引擎)
- [9. Client 侧插件系统](#9-client-侧插件系统)
- [10. 构建、分发与工具复用](#10-构建分发与工具复用)
- [11. 演进历史与废弃设计对照](#11-演进历史与废弃设计对照)

---

## 1. 目标与不变量

### 1.1 核心设计目标

| # | 目标要求 | 技术落点 |
|---|---|---|
| **G1** | **协程异步为一等公民**：插件代码默认跑在与主程序相同的线程上 | **锚定协程 (Anchored Coroutines)**：插件协程物理执行于宿主 IO 线程 |
| **G2** | **原生协程交错**：插件协程与宿主 Asio 协程互不阻塞、公平让出 | 挂起=让出 IO 线程，恢复=完成回调在 IO 线程派发 |
| **G3** | **真事件驱动零轮询** | 彻底删除寄生轮询层（poll loop），全链路通过 channel kick / 完成回调 / cancellation slot 驱动 |
| **G4** | **严格 ABI 隔离**：接口采用纯 C 规范，禁止跨动态库边界传递 C++ 对象 | 核心 vtable 契约冻结（四成员）+ COM 风格接口表查询，边界仅传基础类型、函数指针与字符串视图 |
| **G5** | **跨编译器与标准库兼容** | 插件的协程帧、C++ awaiter、STL 容器完全封装于插件动态库内部，永不穿透边界 |
| **G6** | **精简操作协议，降低复杂度** | 旧三件套 (start/poll/cancel) 简化为两件套 (start/cancel)，SDK 提供单套标准化 API |

### 1.2 架构不变量

- **核心 vtable 四成员冻结**：`alloc` / `free` / `strdup` / `query_interface` 永不再增删。
- **无锁单线程会话模型**：Session 的可变状态仅在宿主主 IO 线程读写（`assertIoThread()` 强制校验）；所有插件注册、注销、会话状态访问均串行化于 IO 线程。
- **卸载生命周期安全闭环**：`InflightGuard` 引用计数贯穿整个异步操作生命周期；`unloadAsync` 必须等待在途引用归零才执行 `destroy` 和 `dlclose`。
- **统一插件模型**：所有插件物理上都是 C++ 动态库；脚本语言（如 JS）通过普通 C++ 插件外壳委派给引擎插件承载，宿主内核不感知具体脚本类型。

---

## 2. 核心模型：锚定协程

### 2.1 锚定原理

C++20 协程帧属于插件本地的堆内存分配。**`std::coroutine_handle::resume()` 在哪个线程被调用，该协程段就在哪个线程物理执行**。

> 只要插件协程的所有挂起与恢复均经由「宿主保证在宿主 IO 线程派发的 C 回调」完成，插件协程段便天然与主程序的 Asio 协程在同一线程上原生交错执行，无需在跨编译器边界共享复杂执行器对象，也无需私有事件循环。

```
宿主 IO 线程                                                         时间 →
────────────────────────────────────────────────────────────────────────►
 Toolcall 协程 ──co_await──► op_driver park(chan)              （让出 IO 线程）
                                 ▲
 plugin start():                 │ kick
   创建 Task 协程帧               │
   resume() #1                   │
     ├─ 段 1: 解析参数...         │
     ├─ co_await c.sleep(50) ──┐ │
     │     挂起 (登记宿主定时器)  │ │
     ▼                         │ │
 ... 其他协程交错执行 ...          │ │
                               │ │
 宿主 sleep 到点 ───────────────┘ │
   cb() → h.resume()             │
     ├─ 段 2: 执行业务逻辑         │
     ├─ co_await c.offload(f) ─┐ │ （下工作线程池，IO 线程继续让出）
     ▼                         │ │
 ... 宿主处理其他网络/会话 IO ... │ │
 阻塞池完成 → done 投递回 IO 线程 ─┘ │
   cb() → h.resume()
     ├─ 段 3: 提取计算结果
     └─ co_return 结果 → notify.done(OK, payload) ──► channel kick
 op_driver 被唤醒取结果 → Toolcall 协程恢复
```

### 2.2 两件套操作协议 (start / cancel)

废弃旧版 `poll` 机制后，所有异步操作（工具执行、中间件钩子、能力调用）统一为 `start/cancel` 两件套：

```c
/// 被调方操作 = start(启动, 可内联完成) + cancel(协作式取消请求);
/// 终结状态由 AgentxxOpNotify.done(status, payload) 恰好一次上报。
```

- **内联完成型 (快同步, <~1ms)**：在 `start` 内直接同步计算完毕，调用 `notify->done(AGENTXX_OP_OK, result)` 并返回 `NULL`。
- **锚定协程型 (标准推荐)**：创建 `Task` 协程帧并 `resume()` 到首个挂起点，返回 job 不透明句柄。
- **阻塞委托型**：在 `start` 中通过 `scheduler->offload` 将重型计算卸载至线程池，完成时回调派发回 IO 线程唤醒。
- **自管线程型**：登记任务到插件自有线程（如 JS 引擎），执行完成后任意线程调用 `notify->done`。

---

## 3. 线程契约、交错与生命周期安全

### 3.1 线程契约矩阵

| 交互上下文 | 线程归属与调度约定 |
|---|---|
| `start` / `cancel` 回调 | **仅宿主 IO 线程调用**；必须为非阻塞快速返回（单次 ≤~1ms，看门狗 >100ms 触发告警）。 |
| 宿主派发给插件的原语回调 (`sleep` cb / `offload` done / `call_tool` cb / `invoke_cap` cb) | **宿主保证在 IO 线程派发，一律经 `asio::post` 入队，禁止同步重入**。 |
| 插件向宿主上报的 `AgentxxOpNotify.done` | **插件任意线程可调**（线程安全）。内部通过 CAS 保证恰好调用一次，并通过 channel 唤醒宿主等待方。 |
| 插件内部多协程并发 | 同一实例内的多个锚定协程在单线程上交错协作运行；跨 `co_await` 点不持有对共享状态的独占假设。 |
| 同步阻塞 / CPU 密集操作 | 必须使用 `co_await agentxx::kit::offload(ctx, fn)` 卸载至阻塞线程池，完成后回 IO 线程继续执行。 |
| 异步套接字 / 子进程管道 | 插件自备专用线程与私有 `io_context`（`reactor_tool`），或使用独立线程池。 |

### 3.2 零轮询取消传播机制

1. **宿主精确中断（主通道）**：会话取消 → `CancelToken.emit()` → asio cancellation signal 沿引擎绑定的 `co_await` 链传递 → `op_driver` 停靠中的 `chan.async_receive` 立即以 `operation_aborted` 返回 → 调用插件 `cancel` 回调 → 插件协程在挂起点提前被唤醒退出。
2. **有界停靠兜底**：`chan.async_receive ‖ 100ms 定时器` 竞速，确保极端情况下 100ms 内必能捕获取消信号。
3. **插件主动检查点**：`ctl.cancelled()` / `ctl.throw_if_cancelled()` 供插件在长耗时循环切片中主动检测退出。

### 3.3 卸载安全与防 UAF 设计

- **InflightGuard 守护**：`OpCore` 存活 ⇔ 引用计数 > 0 ⇔ 插件代码段保活。
- **放弃路径哨兵协程 (`sentinelReap`)**：当等待方因超时或上层取消提前退出时，后台启动轻量哨兵协程停靠等待迟到的 `done` 回调到达，确保内存释放安全，绝不悬垂。
- **定时器 `std::weak_ptr` 防线**：`PluginSleepTimer` 内部持有 `std::weak_ptr<PluginInstance>`，在定时器触发与取消回调中通过 `.lock()` 确认实例有效性，彻底消除卸载竞态下的 Use-After-Free。
- **卸载主动取消闭环**：`PluginManager::detachAll` 在卸载前遍历实例的未决操作与后台任务统一发送 cancel，挂起中的长 sleep 定时器与阻塞任务即时中断。

---

## 4. 纯 C ABI 契约 (v1)

### 4.1 入口符号与全局版本号

```c
#define AGENTXX_PLUGIN_API_VERSION 1
```

插件必须导出以下 C ABI 入口符号（通过 `AGENTXX_PLUGIN_EXPORT` 控制可见性）：
- `const AgentxxPluginInfo* agentxx_plugin_get_info(void);`
- `int agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx);`
- `void agentxx_plugin_destroy(void* plugin_ctx);`

### 4.2 接口表清单 (IID)

每个接口表第一项为 `int version = 1`：

| IID 标识 | 结构体类型 | 核心职能 |
|---|---|---|
| `agentxx.agent.tools` | `AgentxxToolsIface` | 工具注册、注销，完成回调形 `call_tool_async` 与句柄取消 `op_cancel` |
| `agentxx.agent.hooks` | `AgentxxHooksIface` | 7 个生命周期中间件钩子的两件套注册与注销 |
| `agentxx.agent.events` | `AgentxxEventsIface` | 跨插件与端点的主题发布/订阅 (`publish` / `subscribe` / `unsubscribe`) |
| `agentxx.agent.capabilities` | `AgentxxCapabilitiesIface` | 能力声明、两件套能力注册、跨插件异步调用 `invoke_capability_async` |
| `agentxx.agent.scheduler` | `AgentxxSchedulerIface` | `is_io_thread`, `post_to_io`, `pump_io`, `sleep`, `cancel_sleep`, `offload` |
| `agentxx.agent.session` | `AgentxxSessionIface` | 会话级 `share_store` 访问与消息提示推送 `emit_message_tip` |
| `agentxx.agent.plugins` | `AgentxxPluginsIface` | 插件互查 API (`list_plugins`, `get_plugin`, `get_own_info`) |
| `agentxx.agent.config` | `AgentxxConfigIface` | 获取宿主配置、插件专属参数、toolPrompt、工作目录与会话工作目录 |
| `agentxx.agent.model` | `AgentxxModelIface` | 获取宿主主模型与 WebSearch 模型配置 |
| `agentxx.agent.cancel` | `AgentxxCancelIface` | 查询会话轮次取消状态（建议性查询） |
| `agentxx.agent.planning` | `AgentxxPlanningIface` | 读写两级任务规划状态 |
| `agentxx.agent.prompt` | `AgentxxPromptIface` | 动态读写宿主提示词及备份回滚 |
| `agentxx.agent.json` | `AgentxxJsonIface` | 跨边界 JSON 字符串安全提取与转义 |
| `agentxx.agent.log` | `AgentxxLogIface` | 实例级日志输出通道 |
| `agentxx.agent.resources` | `AgentxxResourcesIface` | 动态注入 MCP / Skill / 规则 / 会话环境资源 |

---

## 5. plugin_kit.h 开发 SDK

SDK 位于 `agent/lib/include/agentxx/plugin/plugin_kit.h`，header-only，仅依赖 `plugin_api.h`。

### 5.1 基础基类与 Logger 闭包

- **`PluginBase`**：每个插件实例上下文的基类，提供 `init(host)`、`workDir(tid)`、`toolPrompt(name)`、`argsJson()`、`jsonEscape()`、`storeShim()` 等公共辅助。
- **`Logger`**：实例绑定的日志闭包对象，彻底废除全局日志指针，支持多实例并发独立输出。

### 5.2 `Task<T>` 与完成协议

```cpp
template<typename T = void> struct Task;
```
- **帧先销毁后通知**：`detail::finishIfDone` 在协程执行完毕后：
  1. 提取结果或捕获异常；
  2. **显式调用 `h.destroy()` 销毁协程帧及其内部全部局部对象**；
  3. 调用 `notify.done(status, payload)` 上报宿主。
  此举保证了在 `notify.done` 释放 `InflightGuard` 的瞬间，插件代码段已无任何正在执行的析构逻辑。

### 5.3 注册工具族

- `agentxx::kit::tool`：包装返回 `Task<T>` 的锚定协程工具。
- `agentxx::kit::fast_tool`：包装快速同步计算工具（<~1ms，直接在 IO 线程计算并返回）。
- `agentxx::kit::blocking_tool`：包装同步阻塞工具，自动通过 `offload` 卸载至阻塞线程池。
- `agentxx::kit::reactor_tool`：用于自备私有 `io_context` + 专用线程的异步 IO 插件。
- `agentxx::kit::spawn`：启动后台长驻协作任务（内部通过 `sleep` 循环调度）。
- `agentxx::kit::hook`：注册中间件生命周期钩子。
- `agentxx::kit::capability`：注册命名能力方法处理器。

### 5.4 阻塞便捷助手与死锁消除机制

对于在工作线程中调用其他插件的场景，SDK 提供了基于条件变量的阻塞包装：
- `call_tool_blocking`
- `invoke_capability_blocking`

**死锁消除**：当在宿主 IO 线程（如插件同步初始化 `agentxx_plugin_create` 阶段）调用阻塞助手时，SDK 在 `cv.wait_for` 循环中主动触发 `sched->pump_io(host)`，派发执行当前排队的 IO 任务，彻底解决跨引擎反向注册时的单线程死锁问题。

---

## 6. 宿主侧核心实现

### 6.1 模块结构拆分

宿主端核心代码拆分为 5 个结构清晰的编译单元（位于 `agent/lib/src/plugins/`）：
1. `plugin_manager_lifecycle.cpp`：生命周期管理（加载、卸载、级联依赖、提示词备份与恢复）。
2. `plugin_manager_vtable.cpp`：C ABI 各接口表具体函数实现及派发。
3. `plugin_manager_adapters.cpp`：`PluginTool` 与 `PluginMiddlewareHandle` 适配器。
4. `plugin_manager_scheduler.cpp`：定时器 `sleep`、`cancelSleep` 及线程池 `offload`。
5. `plugin_manager_capability.cpp`：能力注册表及跨插件异步调用驱动器。

### 6.2 异步驱动核心 (`op_driver.h`)

- **`OpCore`**：持有 `atomic<bool> notified`、channel、`InflightGuard` 及完成回调。
- **`awaitPluginOp`**：在 IO 线程启动 `OpDrive`，结合 `CancelToken` 监听进行停靠等待与状态转换。
- **`sentinelReap`**：放弃路径的哨兵协程，负责在操作中断时静默收割迟到的 `done` 并安全释放资源。

---

## 7. 内置插件迁移矩阵

全部 14 个内置插件及示例均已按照 API v1 架构完成迁移：

| 插件名称 | 迁移形态 | 实现机制 |
|---|---|---|
| **agentxx_filesystem** | `blocking_tool` | 文件读写、glob、grep 均通过阻塞池安全执行，路径全面支持 UTF-8 |
| **agentxx_execute_command** | `reactor_tool` / `blocking_tool` | 优先采用 Boost.Process v2 专用 loop 线程异步管道；无进程支持时回退 popen blocking |
| **agentxx_websearch** | `reactor_tool` | 基于 Asio HTTP 客户端挂载至插件专属 Reactor 线程 |
| **agentxx_string** | `blocking_tool` | HTML2Markdown 转换与正则匹配处理 |
| **agentxx_system** | `fast_tool` | 获取当前时间戳与日期（<1ms 内联直接返回） |
| **agentxx_rag_search** | `blocking_tool` | 向量数据库索引与搜索操作 |
| **agentxx_planning** | `fast_tool` | 任务规划读写与校验 |
| **agentxx_codegraph** | `blocking_tool` | 符号定义查找、调用图遍历，实例级 Logger 隔离 |
| **agentxx_computer_use** | `blocking_tool` | 桌面 UI 模拟控制 |
| **agentxx_screen_capture** | `blocking_tool` | 屏幕截图采集与图像保存 |
| **agentxx_audio_stream** | `blocking_tool` | 音频采集控制与事件流推送 |
| **agentxx_system_monitor** | `blocking_tool` + `capability` + `spawn` | 工具同步采样 + 后台周期采集任务 (`spawn` + `sleep` + `offload`) |
| **agentxx_text_selection_monitor** | `blocking_tool` | 划词监听控制与事件流推送 |
| **agentxx_javascript_engine** | 专用自管线程 | QuickJS 运行时引擎，注册 `interpreter.js` 能力 |
| **example_plugin** | 综合演示 | 覆盖 fast_tool、Task 协程工具、call_tool、sleep、hook、capability |
| **example_js** | C++ 壳 + JS 脚本 | 声明依赖 JS 引擎，通过能力调用由引擎执行 `plugin.js` |
| **example_resources** | 声明式/编程式资源 | 演示 MCP、Skill 与会话提示词资源贡献 |

---

## 8. 统一脚本插件模型 (QuickJS 引擎)

### 8.1 模型架构

```
宿主 (libagentxx)
  └── 加载 example_js 动态库
        └── example_js::agentxx_plugin_create
              └── 调用 invoke_capability_async("interpreter.js", "load", { name, path })
                    └── agentxx_javascript_engine (QuickJS 引擎插件)
                          └── 在 JS 线程中解析执行 plugin.js
                                └── JS 代码调用 agentxx.registerTool
                                      └── 经 C 桥反向调用宿主 register_tool
```

- **统一性**：宿主内核无需针对 JS 语言编写任何特殊分支，所有脚本插件都是标准的 C++ 动态库。
- **解耦性**：QuickJS 引擎本身作为一个独立插件存在，可随时被替换为其他引擎（如 V8、Python 等）。

---

## 9. Client 侧插件系统

Client 侧（TUI / CLI）保持同步回调模型，采用对称的 C ABI 接口（`client_plugin_api.h`）：
- **入口符号**：`agentxx_client_get_info` / `agentxx_client_create` / `agentxx_client_destroy`。
- **UI 扩展点**：
  - `register_status_item`：向底部状态栏添加动态条目。
  - `register_panel`：注册弹窗与浮层面板。
  - `register_info_section`：向侧边栏添加信息段落。
  - `register_command`：向命令菜单注册用户指令。
- **双端插件**：单一动态库可同时导出 Agent 端与 Client 端两套符号，单文件分发。

---

## 10. 构建、分发与工具复用

### 10.1 导出符号隔离配置

构建系统统一采用隐藏规则：
- **Linux/Android**：`-fvisibility=hidden` + Version Script 白名单。
- **macOS**：`-exported_symbols_list`。
- **Windows**：默认不导出，仅通过 `AGENTXX_PLUGIN_EXPORT`（`__declspec(dllexport)`）显式导出指定符号。

### 10.2 工具库复用 (`agentxx_util`)

- 面向内置插件提供 `agentxx_util` 静态库，包含字符串、编码检测、UTF-8 路径规范化、Base64、正则等基础功能。
- 符号严格隐藏于插件动态库内部，互不干扰。

### 10.3 内置合并编译模式

通过 CMake 选项 `-DAGENTXX_ENABLE_PLUGIN_BUILTIN=ON`，可将指定或全部插件直接编译合并至 `libagentxx`，运行期无需外部动态库即可零开销运行，适用于单文件分发与嵌入式场景。

---

## 11. 演进历史与废弃设计对照

| 历史设计（旧版本） | API v1 架构（当前实现） | 演进原因与优势 |
|---|---|---|
| `start / poll / cancel` 三件套 | **`start / cancel` 两件套** | 删除无用的轮询开销，全链路基于事件驱动与协程挂起/恢复 |
| 寄生轮询层 (`plugin_poll_loop.h`) | **彻底废弃移除** | 消除定时轮询带来的 CPU 占用与响应延迟 |
| `AgentxxHostOp` 轮询句柄族 | **`AgentxxOpNotify.done` 单次回调** | 接口语义清晰，由宿主管理生命周期，彻底杜绝句柄泄漏 |
| `add_timer / cancel_timer` 周期定时器 | **`kit::spawn` + `sleep` 循环** | 统一为协程后台任务，支持标准取消传播与 RAII 资源回收 |
| 全局日志指针 `g_log_sink` | **实例成员 `Logger` 闭包** | 彻底修复同进程内多实例并发运行时的日志串扰缺陷 |
| 手拼 JSON Schema 字符串 | **`neograph::json` 结构化导出** | 杜绝描述文本中因未转义换行/引号引发的 JSON 解析崩溃 |
| Windows ANSI 窄字符路径处理 | **`utf8ToPath` 宽字符转换** | 杜绝非 GBK 字符在 Windows 下触发 `ERROR_NO_UNICODE_TRANSLATION` |
