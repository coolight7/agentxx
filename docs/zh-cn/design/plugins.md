# 插件系统开发指南

> 关联: [design](index.md) (主程序架构) · [ffi.md](ffi.md) (FFI) · 源码: [agent/plugins/](/agent/plugins/) · C ABI 契约: [plugin_api.h](/agent/lib/include/agentxx/plugin/api/plugin_api.h) / [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h) / SDK: [plugin_kit.h](/agent/lib/include/agentxx/plugin/api/plugin_kit.h)

---

## 1. 总览

Agentxx 插件系统采用 **纯 C ABI + COM 风格接口表查询**：

- **纯 C 边界**：跨边界仅传递纯 C 基本类型、函数指针、不透明句柄与 `AgentxxPluginStringView` (data+size 只读借用，不要求 NUL 结尾)，严禁直接传递 `std::string/vector/function` 或 C++ 异常
- **跨编译器/标准库/语言兼容**：主程序与插件可由不同编译器、不同 STL (libstdc++/libc++/MSVC STL) 或不同语言独立编译，运行时稳定兼容
- **内存所有权**：所有跨边界堆内存统一经 `host->alloc/free` (核心 vtable 内存管理两件套) 管理，接收方用后 `host->free`；字符串复制采用头文件内联助手 `agentxx_plugin_strdup(host, ...)`
- **原生协程异步支持**：经 `plugin_kit.h` 的 `Task<T>`，插件协程执行于宿主 IO 线程，挂起让出、完成经 IO 线程回调唤醒，宿主与插件的协程执行可互相交错切换，且运行于同一线程无锁，无轮询、无私有事件循环
- **单线程会话**：宿主会话可变状态仅在主 IO 线程串行访问；插件注册/状态访问由宿主内部按需 `post` 回 IO 线程，插件无感

---

## 2. 核心架构与兼容性准则

```
宿主 (libagentxx / agentxx_cli)
  核心 vtable (冻结) ── alloc / free / query_interface (IID → 接口表)
                       │
         ┌─────────────┼─────────────┬──────────────┬─────────────┐
         │ tools       │ hooks       │ events       │ scheduler   │  ...16 张 agent + 7 张 client
         │ register/   │ 7 钩子点     │ publish/     │ sleep/      │  capabilities/
         │ call_tool   │             │ subscribe    │ offload     │  session/plugins/
         └─────────────┘             └──────────────┘             │  config/model/cancel/...
插件动态库 (任意编译器) ── AGENTXX_PLUGIN_EXPORT 入口 ── PluginBase 上下文堆 ── SDK 注册族
```

- **核心 vtable 冻结**：仅 `alloc/free + query_interface`，永不增删；一切宿主能力按稳定 `IID` 字符串查询独立接口表获取 (`AGENTXX_PLUGIN_QUERY_IFACE` 宏)
- **严格 ABI 规约**：
  - 8 字节结构体对齐：头文件统一包含 `#pragma pack(push, 8)` / `#pragma pack(pop)`
  - 定长基础数据类型：禁止无修饰 `int/long/size_t`，跨边界统一采用 `int32_t`、`int64_t`、`uint64_t` 等定长类型
  - 明确调用约定：跨边界导出符号与函数指针一律携带宏 `AGENTXX_PLUGIN_CALL` (Windows 平台定义为 `__stdcall`，x64 Unix 平台为空)
  - 结构体传参与返回值：跨边界禁止值传递聚合结构体，入参一律为指针 (`const Struct*`)；结构体返回值一律改为指针出参 (`Struct* out`) 并返回 `int32_t` 状态码 (0 表示成功)
  - 核心 vtable 精简：移除原 `strdup` 槽位，改为基于 `alloc` 的头文件内联实现 `agentxx_plugin_strdup`
  - C++ 辅助便捷层：`AgentxxPluginStringView` 与 `AgentxxPluginString` 内置 `operator const T*()` 隐式取址转换与 `empty()` 方法，文件尾部提供值传兼容重载与 `agentxx_plugin_string_free` 重载
- **接口表独立演进**：每张表首字段 `int32_t version` 独立版本号；表内函数指针可能为 `NULL` (宿主未实现该子能力，调用前判空)
- **版本限制**：全局 `AGENTXX_PLUGIN_API_VERSION` / `AGENTXX_CLIENT_PLUGIN_API_VERSION` 均重置为 1，加载时要求 `>=` 宿主版本否则拒绝；新增能力 = 新增接口表或表内追加成员并递增该表版本，全局版本号不动
- **线程约定**：`query_interface/alloc` 任意线程；注册类与 session/config/prompt 等 IO 约束操作由宿主内部投递同步等待；两件套 `start/cancel` 由宿主在 IO 线程驱动 (单次 <~1ms)；`AgentxxPluginOperatorNotify.done` 可任意线程回调；宿主派发给插件的完成回调 (`AgentxxOpCb`/sleep/offload done) 保证在 IO 线程 `post` 入队
- **实例生命周期、Operation 终态与租约**：见第 15 节（Reset-v1 契约，内置插件必须遵守）

---

## 3. 多实例三铁律

同一插件动态库在单进程内可能被多个独立 Agent 宿主分别加载并创建多个并存实例 (如 FFI 多句柄、AgentHost 子代理)：

1. **禁止可变全局/函数级 static**：所有可变状态必须封装在随实例创建的上下文堆对象 (`*plugin_ctx`，通常继承 `kit::PluginBase`)
2. **状态经上下文闭包恢复**：所有工具/钩子/事件回调必须通过 `spec.user_data` 恢复当前实例上下文
3. **接口表缓存存入实例上下文**：`AgentIfaces` 查询结果存实例成员，各实例互不干扰；offload线程池异步接口 (`plugin_kit.h`) 适配器为调用方内嵌存储，随实例销毁释放

**唯一例外：进程级单调计数器**。用于生成进程内唯一名称/序号（如
`agentxx_filesystem` 的编辑临时文件名 `s_editTmpSeq`）的 `static std::atomic<uint64_t>`
是允许的：它不承载实例语义，改成每实例计数反而会让两个实例生成同名临时文件。
例外只适用于"只增不减、只用于唯一性"的原子计数器；任何缓存、状态机、配置副本
仍然必须放在实例上下文中。

---

## 4. 导出符号控制

插件动态库默认隐藏全部符号，仅导出宿主按名查找的入口符号。入口函数必须以 `AGENTXX_PLUGIN_EXPORT` 标记 (位于 `extern "C"` 内)：

```c
#include "agentxx/plugin/api/plugin_api.h"
extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void);
extern "C" AGENTXX_PLUGIN_EXPORT int32_t agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx);
extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx);
```

- **入口符号集**：
  - Agent 侧：`agentxx_plugin_agent_get_info` / `agentxx_plugin_agent_create` / `agentxx_plugin_agent_destroy`
    (+ 可选的 `agentxx_plugin_agent_start` / `agentxx_plugin_agent_stop`)
  - Client 侧 (双端/纯 UI)：`agentxx_plugin_client_get_info` / `agentxx_plugin_client_create` / `agentxx_plugin_client_destroy`
    (+ 可选的 `agentxx_plugin_client_start` / `agentxx_plugin_client_stop`)
- **构建侧自动化**：`plugins/CMakeLists.txt` 统一配置 ELF `-fvisibility=hidden` + version script 白名单 (通配符 `agentxx_plugin_agent_*`/`agentxx_plugin_client_*`，兼容单端插件在 Android lld 下链接)，macOS `-exported_symbols_list`，MSVC `dllexport`；第三方静态库符号自动隐藏
- **校验脚本**：`agent/script/check_plugin_exports.sh [plugin-dir]` 用 `nm -D --defined-only`
  遍历构建产物，要求每张插件库只导出上述入口符号；出现任何其他导出符号即失败
  (用于确认第三方静态依赖与 `agentxx_util` 的符号确实被隐藏)

---

## 5. 工具函数复用 (`agentxx_util`)

面向项目内置插件，可通过独立静态库 `agentxx_util` 复用主程序全部基础工具 (字符串/编码检测/UTF-8 转换/路径规范化/Base64/HTTP/SQLite/正则/日志/JSON 等)：

```cmake
find_package(agentxx_util REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE agentxx_util)
```

```cpp
#include "agentxx/util/string_util.h"
auto b64 = agentxx::util::base64Encode(data);
#include "agentxx/util/json.h"
#include "agentxx/util/json_view.h"
// 业务/插件统一用 agentxx::util::Json/JsonView (simdjson 驱动); 高频只读先 JsonView::parse 路由, 命中后 to_json() 物化
```

- `agentxx_util` 由 `agent/lib/src/util/` 全部源文件编译 (含 `json.cpp`/`json_view.cpp`, simdjson 驱动的自主 `agentxx::util::Json`/`JsonView`)，libagentxx 与各插件各自静态链接一份副本，符号经导出控制隐藏互不冲突；依赖全部 `PUBLIC` 传递 (fmt/sqlite3/uchardet/iconv + simdjson/OpenSSL/hyperscan/uring, 自 JSON 自主化起已彻底移除 neograph 系/yyjson)
- 定位为内置插件便捷库 (与主程序同一 superbuild 构建、依赖齐全)；第三方插件仅需纯 C 头 `plugin_api.h` / SDK `plugin_kit.h`，无需链接宿主库
- 未引用模块按目标文件提取自动裁剪 (9 插件 `DT_NEEDED` 仅系统库)

---

## 6. C++ 插件开发方式 (SDK `plugin_kit.h`)

推荐使用官方 header-only SDK `plugin_kit.h` (位于 `agentxx/plugin/api/plugin_kit.h`)。
最新框架提供了开箱即用的声明式导出宏、链式 Schema 构建器、宽容参数提取器与通用取消注册中心。
其中 `Task<T>` 协程 (以及 `sleep`/`yield`/`offload`/`call_tool`/`invoke_cap` 原语) 的推进
交由宿主的**协程驱动桥** (Reset-v2) 调度：见 §16 与 §15 的生命周期契约。

```cpp
#include "agentxx/plugin/api/plugin_kit.h"

using namespace agentxx::plugin;

struct MyPluginCtx : public PluginBase {};

// 使用声明式导出宏 (自动封装入口符号、C ABI 边界异常守卫与实例上下文生命周期)
AGENTXX_PLUGIN_AGENT_EXPORT(
    MyPluginCtx,
    "my_plugin",
    "1.0.0",
    "My awesome plugin description",
    [](MyPluginCtx& ctx) -> int32_t {
        // 1. ToolSchemaBuilder: 链式构建 JSON Schema (自动与宿主 toolPrompt 提示词覆盖融合)
        auto mySchema = ctx.schema("my_blocking_tool")
                            .string("path", "Target file path", /*required=*/true)
                            .integer("timeout", "Timeout in seconds", false, 60)
                            .boolean("all_output", "Return all output", false, true)
                            .build();

        // 2. 阻塞工具 (自动卸载到宿主 blockingPool 线程池，不占 IO 线程)
        // 回调直接注入: (ctx, args_json, tid, workDir, cancel_flag)
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
                // ArgReader: 宽容类型提取 (支持 string/number/bool/json 宽容转换与自愈)
                ArgReader args(args_json);
                auto path = args.require<std::string>("path");
                if (!args.ok()) {
                    return args.errorMessage();
                }

                // 配合 CancelRegistry 检查取消 (事件驱动或状态查询)
                if ((cancel_flag && *cancel_flag != 0) || c.sessionCancelled(tid)) {
                    return "cancelled";
                }

                return R"({"status":"done"})";
            }
        );

        // 3. 快同步内联工具 (<~1ms, IO 线程直接计算并返回)
        fast_tool(ctx, "my_fast_tool", "Fast tool depict", R"({"type":"object","properties":{}})",
            [](MyPluginCtx& c, std::string_view args_json, std::string_view tid) -> std::string {
                return R"({"result":42})";
            }
        );

        // 4. Task 锚定协程工具 (可精确 co_await sleep / yield / call_tool / offload)
        tool(ctx, "my_async_tool", "Async coroutine tool", R"({"type":"object","properties":{}})",
            [](MyPluginCtx& c, std::string_view args_json, OpCtl ctl) -> Task<std::string> {
                co_await sleep(c, 100);
                ctl.throw_if_cancelled();
                // 跨插件互调: co_await call_tool(c, "other_tool", "{}", ctl.threadId());
                co_return R"({"status":"ok"})";
            }
        );

        // 4b. 受控轮询工具: 业务体是 asio 协程, 等待插件本地 reactor 上的内核就绪事件
        //     (socket/子进程管道/文件/本地 timer)。插件注册时声明"需要受控轮询驱动",
        //     桥据此在有在途操作时继续申请请求 (有进展立即续 / 无进展退避 10ms /
        //     空闲零开销), 不占宿主工作线程; 无 coroutine_runtime 的宿主自动降级为
        //     offload 工作线程跑完。详见 §16.5。
        polled_tool(
            ctx,
            "my_polled_tool",
            "Async IO tool driven by controlled polling",
            R"({"type":"object","properties":{"url":{"type":"string"}}})",
            [](MyPluginCtx&     c,
               std::string_view args_json,
               std::string_view tid,
               std::string_view workDir,
               const AgentxxPluginCancelToken* cancel) -> asio::awaitable<std::string> {
                ArgReader args(args_json);
                std::string workDirStr(workDir);
                if (agentxx_plugin_cancel_is_requested(cancel)) {
                    throw CancelledException("cancelled");
                }
                // 业务体直接 co_await 现成的 asio 协程实现 (无需局部 io_context + run())
                co_return co_await doHttpGetAsync(args.raw(), workDirStr);
            }
        );

        // 5. 后台协作任务 (宿主托管: 自动注册 agentxx.agent.tasks, 卸载时宿主统一取消并精确等待退出)
        spawn(ctx, [](MyPluginCtx& c, OpCtl ctl) -> Task<void> {
            while (!ctl.cancelled()) {
                co_await sleep(c, 5000);
                if (ctl.cancelled()) break;
                // 周期采集并发布事件
            }
        });

        // 6. 钩子 (7 个钩子点: agent_start/end, model_start/run/end, tool_start/end)
        hook(ctx, AGENTXX_PLUGIN_HOOK_MODEL_START, [](MyPluginCtx& c, std::string_view in) {
            // ...
        });

        // 7. 能力 (跨插件通用 RPC 通道)
        capability(ctx, "my.cap", [](MyPluginCtx& c, const AgentxxPluginHost* caller,
                                     std::string_view method, std::string_view args) {
            return "{}";
        });

        return 0;
    }
);
```

### SDK 核心基础设施组件

1. **导出宏 (`AGENTXX_PLUGIN_AGENT_EXPORT` / `AGENTXX_PLUGIN_CLIENT_EXPORT`)**:
   - 自动生成 `agentxx_plugin_agent_get_info` / `create` / `destroy` (及 client 侧对应符号)
   - 包含完整的 `guardCall` 异常守卫（向日志报告 C++ 异常，阻止异常越界穿透 C ABI）
   - 自动创建实例上下文堆对象（继承自 `PluginBase`），调用 `ctx->init(host)` 并挂载资源
2. **链式 Schema 构建器 (`ToolSchemaBuilder`)**:
   - 经 `ctx.schema("tool_name")` 获得构建器实例，通过 `.string()`, `.integer()`, `.number()`, `.boolean()`, `.array()`, `.stringArray()`, `.enumString()` 等链式声明参数
   - 自动从宿主 `toolPrompt` 中提取当前语言下的参数说明并优先覆盖默认注释，最后调用 `.build()` 输出标准 JSON Schema 字符串
3. **强类型参数提取器 (`ArgReader`)**:
   - 宽容解析输入 JSON：容忍格式宽松，并在类型不匹配时尝试智能自愈（如字符串 `"true"`/`"1"` 转布尔，数字转字符串等）
   - 提供 `args.require<T>("key")`（若缺少则置错误标记）、`args.get<T>("key")`（返回 `std::optional<T>`）
   - `args.ok()` 与 `args.errorMessage()` 方便快速校验和返回参数错误
4. **事件驱动取消注册中心 (`CancelRegistry`)**:
   - 已提升至插件开发框架通用基础设施（`agentxx::plugin::CancelRegistry`），各实例独立持有 `ctx.cancelRegistry`
   - `ctx.init()` 自动订阅 `plugin.agentxx.round_start` 事件，新轮次开始时自动清除对应会话的历史已取消标记
   - 支持 `registerCallback(key, cb)` 注册基于会话标识的取消回调，支持 RAII `ScopedRegistration` 守卫，提供排他互斥与防悬挂锁保护，避免回调访问已析构的局部资源
   - 适用于长时间运行的外部进程或底层阻塞 IO（如 `agentxx_execute_command`），一旦宿主发起取消即可毫秒级即时终止子进程组，无需等待轮询间隔
5. **统一异步操作模型 (两件套 start/cancel)**：
   - 工具/钩子/能力均为 `start` (IO 线程非阻塞启动) + `cancel` (协作式) 两件套，终结经 `AgentxxPluginOperatorNotify.done(status,payload)` 恰好一次上报
   - `Task` 协程帧先销毁后 `done` 上报，支持 `offload` 阻塞池委托与 `call_tool`/`invoke_cap` 锚定互调
   - `polled_tool`（受控轮询）与 `blocking_tool`/`fast_tool` 并列：业务体是 `asio::awaitable`，
     等待插件本地 reactor 上的内核就绪事件，由桥按声明式受控轮询推进（§16.5）
   - hook / capability 的 SDK helper 按**返回类型严格分发**：返回 `void`/字符串的同步业务在
     调用内完成；返回 `Task<T>` 的异步业务由统一 root adapter 收束（provider 句柄可取消，
     完成通知在协程真正结束后发出，输入视图由拥有型 `Request` 保证跨挂起点有效）
   - `graph_node` 使用同一 root adapter 注册自定义图节点类型：快同步节点返回节点输出
     JSON（`std::string`），异步节点返回 `Task<std::string>`；node/config/state/thread_id
     由拥有型 `RootRequest` 保证跨挂起点有效，`run_cancel` 置取消标志并取消嵌套 awaiter

**后台任务 spawn (宿主托管)**：`spawn` 启动的后台协作任务 (如周期采集 `while(!cancelled()) { offload; sleep; }`) 自 API v1 起注册到宿主 `agentxx.agent.tasks` 接口表，与工具/能力 op 同构管理：

- **注册**：`spawn()` 内部自动调 `register_task` (io 线程) → 宿主把句柄推入实例 `outstandingOps` (与工具 op 同列表) 并持 `inflight` (存活标记)
- **运行**：协程照常经 `sleep`/`offload` 挂起于宿主；宿主无感，句柄静默
- **卸载**：插件卸载时宿主 `detachAll` 统一取消 (调插件 cancel_fn: 置 cancelFlag + 唤醒挂起的 sleep/offload) → 协程 `while(!cancelled())` 退出 → `finishIfDone` (帧销毁后经 `notify.done` 恰好一次上报) → 宿主 `guard.reset` (inflight-1) + 回收句柄 → `waitInflightZero` 精确等待归零 → `dlclose` 安全，无协程帧悬挂/UAF
- **无降级**：宿主无 `agentxx.agent.tasks` 表或注册失败时 `spawn` 直接失败 (Reset-v1 不再提供无人托管的自管协程退化路径)
- **线程约束**：`cancel_fn` 由宿主在 io 线程回调 (协作式)；`notify.done` 可从插件任意线程上报 (宿主 `OpCore::onDone` 原子 CAS + 投递回 io)；kit 协程完成路径恒在 io 线程


---

## 7. 插件分类与编译模式

1. **按功能划分**：
   - **Agent 插件**：扩展会话执行流 (Tool / Hook / Event / Capability / Resources)
   - **Client 插件**：扩展 TUI/CLI 界面 (StatusItem / Panel / InfoSection / Command / ToolDecor)
   - **双端插件**：同时导出 Agent 与 Client 入口，一份动态库同时服务两端；跨端通信统一走 Wire (`PluginData` agent→client / `PluginDataUp` client→agent)

2. **编译与分发模式**：
   - **独立动态库 (默认)**：编译为独立动态库，经 `agentxx-config.yaml` 的 `plugins` 字段按 `path` 动态加载 (支持插件目录含 `plugin.yaml` 清单分派)
   - **内置合并编译**：`AGENTXX_PLUGIN_BUILTIN_LIST` 指定插件源码直接编译进 `libagentxx`，运行期无需外部动态库零开销调用；此时 `plugins` 段仍可配置参数，`path` 可简写为 `builtin://<name>` 或 `name: <name>` (无需外部目录)，并可通过 `config` 指定插件配置文件所在目录/文件路径

---

## 8. Agent 侧接口表一览

| IID | 版本 | 能力 |
|-----|------|------|
| `agentxx.agent.tools` | 1 | `register_tool/unregister_tool`, `call_tool_async/op_cancel` (插件互调, cb 保证 IO 线程 post) |
| `agentxx.agent.hooks` | 1 | `register_hook/unregister_hook` (7 钩子点, 两件套) |
| `agentxx.agent.events` | 1 | `subscribe/unsubscribe/publish` (topic 自动加 `plugin.` 前缀, 载荷 JSON) |
| `agentxx.agent.capabilities` | 1 | `register_capability(_ex)/unregister/has_capability`, `invoke_capability_async/op_cancel` |
| `agentxx.agent.scheduler` | 1 | `is_io_thread/post_to_io/sleep/op_cancel/offload` (sleep=宿主计时器; offload=阻塞池委托, 需 cancel_token) |
| `agentxx.agent.coroutine_runtime` | 1 | 通用协程驱动: `request_driver/cancel_driver/is_io_thread` (driver ticket/wake 协议, 见 §16) |
| `agentxx.agent.session` | 1 | `get_share_store/add_share_store/emit_message_tip` (IO 线程) |
| `agentxx.agent.plugins` | 1 | `list_plugins/get_plugin/get_own_info` (JSON) |
| `agentxx.agent.config` | 1 | `get_config/get_plugin_args/get_tool_prompt/get_session_work_dir/get_plugin_config_path/get_language/set_language` (get_session_work_dir session_id 为空时返回默认会话工作目录；`get_plugin_config_path` 返回 yaml `config` 归一化绝对路径，可指向文件/目录；`get_language/set_language` 查询或指定运行时生效语言) |
| `agentxx.agent.model` | 1 | `get_config` (主模型及关联配置 JSON) |
| `agentxx.agent.cancel` | 1 | `is_cancelled(threadId)` (advisory, 权威通知为 cancel 回调) |
| `agentxx.agent.prompt` | 1 | `get_prompt/set_prompt` (宿主提示词读写) |
| `agentxx.agent.json` | 1 | `json_get_string/json_escape` |
| `agentxx.agent.log` | 1 | `log(level, msg)` (0 trace .. 4 error) |
| `agentxx.agent.resources` | 1 | `register_skill_dir/memory_file/mcp_server` (仅初始化阶段) + `get_own_resources` (冻结后不可变) |
| `agentxx.agent.graph` | 1 | 执行图扩展: `register_node_type/unregister_node_type` (插件自定义节点类型, 注入 per-agent GraphRegistry) + `get_graph_json/get_graph_name/set_graph_json` (查看/修改宿主执行图, 默认名 `agentxx.default`; 插件加载阶段生效, 宿主构建 engine 前处理) |
| `agentxx.agent.tasks` | 1 | 后台任务宿主托管: `register_task/cancel_task` (kit `spawn` 自动注册; 宿主登记句柄 + 持 inflight + `notify.done` 完成通知 —— 卸载时 detachAll 统一取消 + `waitInflightZero` 精确等待, 无协程帧悬挂; `notify` 为出参, `notify.done` 可从插件任意线程回调) |

---

## 9. Client 侧接口表一览

| IID | 版本 | 能力 |
|-----|------|------|
| `agentxx.client.ui` | 2 | `register_status_item/update/unregister`, `register_panel/update/unregister`, `register_info_section/update/unregister`, `register_command/unregister`, `show_toast`, `update_tool_decor(tool_call_id, decor_json)`, `register_tool_renderer(spec)/unregister_tool_renderer(tool_name)` |
| `agentxx.client.events` | 1 | `subscribe/unsubscribe` (事件见 `AgentxxClientEvent`: READY/CONN_STATE/USER_INPUT/DELTA/TURN_END/SESSION_SWITCH/PLUGIN_DATA) |
| `agentxx.client.session` | 1 | `get_client_state` (快照 JSON), `send_user_input`, `request_cancel` |
| `agentxx.client.wire` | 1 | `send_plugin_data(event, json)` → 服务端 `client.{插件}.{event}` |
| `agentxx.client.self` | 1 | `get_own_info/get_plugin_args/get_plugin_config_path` (后者返回 yaml `config` 归一化绝对路径) |
| `agentxx.client.json` | 1 | `json_get_string/json_escape` |
| `agentxx.client.log` | 1 | `log(level, msg)` |

### 工具特化渲染架构 (Tool Rendering & Decor)

Agentxx 客户端采用统一的分层工具特化渲染机制，TUI 核心层完全解耦，不包含任何具体工具名称的硬编码：

1. **类型级工具渲染器 (`register_tool_renderer`)**：
   - 插件在 client 初始化时按 `tool_name` 注册特化渲染定义 (`AgentxxToolRenderSpec`)，TUI 渲染该工具消息 (实时流式或历史回溯) 时统一生效。
   - **双轨机制**：
     - **`<key, render_fn>` 回调函数**：提供 `AgentxxToolRenderFn`，接收 `AgentxxToolRenderInput` (`tool_name`, `args_json`, `result_text`, `is_finished`, `is_error`, `max_width`)，输出 `AgentxxToolRenderOutput` (`displayName`, `summary`, `items_json`)。适用于需要复杂参数解析、条件格式化或动态生成 UI 项的工具 (如 `read` 区间参数、`glob`/`grep` 模式与文件摘要、`edit` diff 差异对比)。
     - **预设模版 (`template_json`)**：当 `render_fn == NULL` 时，宿主按声明式模板自动从 `args_json` 中提取字段并格式化摘要，如 `{"displayName":"Search","summaryKey":"query"}` 或 `{"displayName":"Bash","summaryKey":"command"}`。
   - **通用 Diff 渲染**：展开体 `items_json` 新增支持 `{"kind":"diff","path":"...","old_str":"...","new_str":"..."}`，TUI 会通用化渲染为自适应屏幕宽度的 side-by-side 或统一差异对比，任何插件均可自由复用。
   - **SDK 辅助函数 (`registerToolRenderer`)**：
     `plugin_kit.h` 提供了基于现代 C++ Lambda 的辅助封装，抹平 C ABI 结构体与内存分配细节：
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
2. **实例级工具装饰 (`update_tool_decor`)**：
   - 订阅 `EVT_DELTA` 的 `tool_start` 后，按特定调用 `tool_call_id` 推送语义 JSON (优先级高于类型级渲染器)；典型实现见 `agentxx_planning` (运行时生成 ASCII/Mermaid 状态图与动态待办列表)。
3. **优先级与降级路径**：
   - 渲染时查询顺序：`toolDecors` (按 `tool_call_id`) > `toolRenderers` (按 `tool_name`) > 通用兜底展示 (原始 `toolName` + 参数/结果文本)。
   - 插件卸载/禁用时宿主自动摘除注册并还原兜底展示，启用时无损恢复。

4. **语义渲染缓存 (Reset-v1)**：
   - 自定义 `render_fn` **只在 client IO 线程执行**：UI 线程提交
     `ClientToolRenderRequest` (tool_call_id / tool_name / args / result / 宽度等拥有型拷贝)，
     宿主在 IO 线程复查 renderer lease、实例 `enabled` 与可注册状态后持 lease 调用，
     把 `displayName/summary/items` 拷成宿主对象写入 `ClientToolRenderCache`；
     输出 JSON 的每个分配字段在成功/失败/异常路径都由宿主释放。
   - UI 线程只读缓存：未命中时本次通用回退，结果写入后由
     `PluginUiAdapter::onToolRenderUpdated` 通知重绘；消息块缓存 key 计入
     `ClientToolRenderCache::version(key)`，因此"回退 → 语义内容"能及时上屏。
   - 缓存条目记录产出插件与实例代次；禁用/卸载按插件失效并递增版本号，会话切换清空。
     旧 UI 快照因此只能回退，不会调用已卸载插件的函数指针。缓存条目与版本记录
     按写入顺序有界回收（默认上限 512，远超单屏可见块数），长时间会话不会按
     `tool_call_id` 无限增长。
   - 预设模版与 `toolDecors` 是纯宿主数据，仍在 UI 线程直接计算。

---

## 10. 会话资源贡献 (Skill / Memory / MCP)

- **声明式**：插件目录随 `plugin.yaml` 声明资源 (框架在 entry 成功后经 `AgentResourceApplier` 统一 `applyDecls` 应用，卸载/禁用时摘除)
- **编程式**：运行时经 `agentxx.agent.resources` 接口表动态注册/注销 (如 `agentxx_codegraph` 按 args 动态注册索引路径)
- 宿主对声明式+编程式资源做去重与生命周期管理 (所有权语义见 `resource_applier.h`)；失败项经 `AppendComponentNotification` 单独统计 (供客户端 Failed 组展示)

### 插件配置文件 (`config` 字段)

- `agentxx-config.yaml` 中每个 `plugins` 条目可通过 `config` 指定插件的配置文件所在目录或文件路径 (可指向文件或目录；支持 `~`/`${VAR}`/相对路径，宿主归一化为绝对路径后透传)
- 插件经 `agentxx.agent.config` (agent 侧) / `agentxx.client.self` (client 侧) 的 `get_plugin_config_path` 查询该路径 (返回 `NULL` 表示未配置)，自行判断类型并加载 (如目录下扫描 `*.yaml`、读取单个文件等)
- 典型用法：`config: ${AGENTXX_WORK_DIR}/config/my_plugin.yaml` 或 `config: ./my_plugin_config/` (相对工作目录)；SDK 中 `PluginBase::configPath()` 提供便捷封装

---

## 11. Worktree 与会话工作目录

- 插件经 `AgentxxConfigIface::get_session_work_dir(host, thread_id)` 取当前会话生效工作目录 (worktree 绑定优先；thread_id 为空时返回默认会话工作目录)
- `blocking_tool` 的 `workDir` 参数由 SDK 在 IO 线程预取并注入，避免 worker 线程跨线程 `ioCallSync`
- 文件系统 / 命令执行插件每次 `execute` 按注入 `sessionId` 动态解析路径，会话绑定后基准即时切换

---

## 12. Javascript 插件 (基于 QuickJS 引擎插件)

Agentxx 仅维护单一 C++ 插件基础设施；JS 脚本插件经内置 `agentxx_javascript_engine` 引擎插件承载：

- **统一插件模型**：所有插件都是 C++ 插件；JS 插件表现为标准 C++ 动态库外壳 (如 `example_js`) 附带 `plugin.js`
- **执行流程 (Reset-v1)**：宿主加载 JS 插件壳 → 壳 `create` 只构造上下文 (解析自身 `plugin.js` 路径) → 壳 `start` 校验 `interpreter.js` 能力后经该能力把 `plugin.js` 交给引擎 → 引擎在专用线程中解析并执行脚本，把脚本声明的工具/钩子反向注册到宿主；脚本注册属于壳实例的 start 事务，因此壳的 start 完成必须等脚本加载结束 (异步 done)，失败由宿主回滚
- **引擎线程属于 start 事务**：`agentxx_javascript_engine` 的 `create` 只构造，`start` 才创建 JSRuntime 与专用 JS 线程并注册 `interpreter.js` 能力，`stop` 停止线程并释放 runtime；`disable → enable` 往返即一次 `stop + start`，引擎线程与脚本上下文按事务重建 (壳插件在自身 start 中重新加载脚本)
- **stop 不阻塞 IO 线程**：JS 线程可能在宿主 vtable 调用中等待 IO 线程，因此 `stop` 由独立收尾线程完成 `join` 与 runtime 释放，完成通知从该线程上报；JS 线程空闲时走调用线程直接收尾的快路径，使紧邻的 stop→start 顺序确定。停止后不再执行插件 JS：新任务被拒绝，队列任务按 `CANCELLED`/`FAILED` 终结，事件投递丢弃
- **`agentxx.callTool` 始终返回 Promise**：命中本引擎工具时同线程执行并把结果/内部 Promise 链到外层 Promise；命中宿主插件工具时经 `call_tool_async` 异步互调，完成/失败/取消事件投递回 JS 线程 settle。JS 线程不会同步等待宿主，A/B 脚本互调不存在线程自锁
- **脚本初始化是注册事务**：脚本顶层注册的工具/钩子/订阅/资源/定时器在顶层异常或脚本卸载时统一回滚（先撤销宿主注册，再释放 JSContext），不留悬垂 user_data；`hookStart` 与能力 `unload` 的完成通知在 JS 执行真正结束后发出
- 可自研脚本引擎插件 (Python/Lua 等) 替换或扩充脚本能力

---

## 13. 插件示例索引

| 插件 | 说明 |
|------|------|
| `example_plugin` | 原生 C++ 综合示例 (fast_tool/Task 协程/call_tool/sleep/**协程驱动桥**/**受控轮询 polled_tool**/钩子/事件/能力/client 入口) |
| `example_graph_node` | Graph 扩展示例 (自定义节点类型 + set_graph_json 改图, 需 `agentxx.agent.graph` 接口) |
| `example_js` | JS 脚本插件示例 (C++ 壳 + `plugin.js`) |
| `example_resources` | 会话资源贡献示例 (声明式与编程式 MCP/Skill/规则/会话环境) |
| `agentxx_filesystem` | 文件系统 6 工具 (list/read/write/edit/glob/grep, 含 `*_impl.h` 直测实现) |
| `agentxx_execute_command` | 命令执行 2 工具 (bash/windows, 含超时与 PowerShell 探测) |
| `agentxx_websearch` | 网络搜索 3 工具 (search/fetch/fetch_markdown) |
| `agentxx_rag_search` | 向量语义搜索 |
| `agentxx_string` | 字符串 2 工具 (html_to_markdown/regexp) |
| `agentxx_system` | 系统时间 (`get_current_datetime`) |
| `agentxx_system_monitor` | 系统资源监控 (工具 + 周期采集 + client 侧 Info/状态栏渲染) |
| `agentxx_planning` | 规划工具 + client 侧 Plan 装饰 |
| `agentxx_math` | 数学计算工具 (`agentxx_math_calculate`, 支持四则/幂/阶乘/位运算/逻辑/三角/双曲/对数/组合排列等函数与隐式乘法) |
| `agentxx_codegraph` | 代码索引 5 工具 (search/context/callers/callees/path) + client Info 栏 |
| `agentxx_screen_capture` | 屏幕捕获 (仅 Windows) |
| `agentxx_computer_use` | 键鼠控制 (仅 Windows, depends: screen_capture) |
| `agentxx_audio_stream` | 音频流捕获 (**全平台跳过构建**: WASAPI 实现未启用, 当前仅桩实现) |
| `agentxx_text_selection_monitor` | 文本选择监听 (仅 Windows UIAutomation) |
| `agentxx_javascript_engine` | QuickJS 引擎 (能力 `interpreter.js`) |
| `agentxx_execute_javascript` | JS 代码执行工具 (`agentxx_execute_javascript`, 依赖 `agentxx_javascript_engine`) |

---

## 14. 构建与平台

> SDK 类型约束的编译期验证：`agent/script/check_sdk_negative_compile.sh` 从测试构建的
> `compile_commands.json` 提取真实编译环境，编译 `agent/test/plugin/negative_compile/`
> 下的片段并断言行为（`positive_control.cpp` 必须编译成功；错误签名如 hook 返回 `int`、
> capability 返回 `int`、tool 返回普通值、跨边界传 STL 参数必须编译失败）。
>
> 插件框架定向 UBSan 探针：`-DAGENTXX_PLUGIN_UBSAN_PROBE=ON` 在常规 Debug/ASan 基线
> 之上，仅对 `lib/src/plugins/*.cpp` 与 `test/plugin/*.cpp` 追加 `-fsanitize=undefined`
> （运行库经顶层 sanitizer 链接参数注入），不重编第三方依赖；用于 `PluginManager`/
> `ClientPluginManager` 与 header-only 的 Operation/InstanceLifetime 运行时的未定义行为验证。

- **平台矩阵**：各插件在自身 `CMakeLists.txt` 开头经 `plugin_platform_support.cmake` 的 `gate` 函数判定，复用顶层 `XX_IS_*_D` 变量；不支持的平台跳过编译 (screen_capture/computer_use/text_selection_monitor 仅 Windows, audio_stream 全平台未实现等)
- **内置合并编译**：按 `AGENTXX_PLUGIN_BUILTIN_LIST` 合并进 `libagentxx`；此时 `test_ffi_c_api` 与 `client_plugins` 测试按条件跳过动态库路径
- **产物布局**：独立动态库模式产物统一输出到 `{build}/exec/plugins/<插件名>/` (含 `plugin.yaml` 清单时按目录分派)

---

## 15. Reset-v1 实例生命周期与异步契约

> 本节描述宿主 (agent 侧 `PluginManager` / client 侧 `ClientPluginManager`) 与插件之间的
> 实例生命周期、Operation 终态与线程契约。它是插件实现的**必须遵守项**；
> 迁移方案与验收矩阵见 `resource/history/plugin-refactor-2/plugin.md`。

### 15.1 入口与状态机

```
create (纯构造) → start (注册事务) → Ready
Ready ⇄ Disabled            (用户或依赖级联)
Ready/Disabled → Closing → Closed
Closing → CloseFailed → Closing (可重试)
```

- `create`：只分配上下文、查询接口、初始化纯本地字段；不提交工具/hook/能力/事件/
  UI/prompt/graph 注册，不启动不受托管的线程。
- `start`：在宿主 IO 线程执行注册事务；同步 `notify.done` 返回即视为成功。
  失败时返回 `NULL + error`（视为拒绝，宿主回滚本次已生效的注册并撤销实例）。
  回滚范围覆盖工具、hook、能力、事件订阅、资源、prompt 贡献、图类型槽位与
  Client UI 项/命令/订阅（回归含真实 DSO 双端：全量注册后失败 → 无残留 →
  再次加载同名注册全部成功）。
- `stop`：实例停用或关闭时调用，用于撤销插件自管的线程/定时器/订阅；
  可重复尝试，失败时实例保持 `Disabled`/`CloseFailed` 且保留上下文与动态库。
- `destroy`：只在 `stop` 完成且租约归零后调用；不得创建异步工作、不得调用宿主
  注册接口。
- SDK：`AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(Ctx, StartFn, StopFn)` 与
  `AGENTXX_PLUGIN_CLIENT_LIFECYCLE_EXPORT(Ctx, StartFn, StopFn)` 生成带异常兜底的
  `agentxx_plugin_{agent,client}_{start,stop}` trampoline；不使用它们的插件保持
  create 期注册的 legacy 行为（宿主按注册记录恢复/摘除）。

### 15.2 Operation 终态协议

- `start` 返回 `NULL + error` 只表示**拒绝**，此时不得调用 `notify.done`，宿主不产生
  回调；一旦接受（无论同步还是异步 done），宿主保证恰一次完成回调。
- `done` 可从任意线程调用；payload 仅在本次调用内借用，宿主第一步复制。
- 一个 Operation 只产生一个终态：`OK` / `CANCELLED` / `FAILED`；终态之后的
  `cancel` 为空操作，不再进入插件代码。
- 取消通过不透明 `AgentxxPluginCancelToken` 查询（`is_cancelled`），插件不得把
  `volatile` 标志或 ABI 原子地址当作跨线程同步手段。
- 跨插件互调同时持有 **caller 与 provider 两侧租约**：caller 的完成回调返回前，
  caller 不会被卸载/destroy。

### 15.3 线程与租约

- 注册表、生命周期、Operation 状态与默认插件业务只在所属 IO 线程；
  worker/JS/平台线程是显式例外。
- 宿主在卸载/关闭时先阻止新进入，再取消旧操作，再等待全部插件代码与回调返回
  （事件式 idle 通知 + 绝对截止时间），最后 stop/destroy/dlclose；
  超时进入 `CloseFailed` 并保留上下文与动态库（可重试），不会静默泄漏。
- 插件持有宿主分配的 token（如 `host->opaque`）在实例卸载后继续使用时，
  宿主保证**安全失败**：入口返回失败值，不访问已释放对象。

### 15.4 启用/禁用事务

- `disable(name)`：宿主同步摘除该实例的注册（工具/hook/能力/事件/资源/prompt 贡献/
  graph/UI），并把 `stop` 事务投递到所属 IO 线程；级联按直接依赖者递归处理。
- `enable(name)`：宿主恢复启用状态后，导出 `start` 的插件由 start 事务重新声明注册
  （未完成前不恢复宿主侧记录），legacy 插件按宿主保存的记录恢复；启用顺序为
  "先依赖、后依赖者"，用户显式禁用的插件不被级联恢复。
- prompt 以 `(owner, key, sequence, value)` 贡献模型合成：卸载/禁用只删除该 owner 的
  贡献并重新合成，不覆盖其他 owner，也不写回已卸载 owner 的旧值。
- Client 侧动作按钮在渲染时记录 `plugin/generation/owner`，派发到 IO 线程复查：
  同名插件重载后，旧点击一律丢弃。

### 15.5 自管线程插件 (以 JS 解释器引擎为例)

`agentxx_javascript_engine` 是"插件自带专用线程"的参考实现，约束同样适用于其他
自管线程/定时器的插件：

- **线程与运行时都属于 start 事务**：`create` 不启动线程；`start` 创建运行时并启动
  线程、提交运行时注册 (能力)；`stop` 停止线程并释放运行时；`destroy` 只释放本地状态，
  不调用宿主接口。
- **stop 必须可重复且不能被调用线程自锁**：插件线程若可能反向调用宿主 vtable
  (宿主在非 IO 线程上以 `ioCallSync` 投递回 IO 线程)，则 `stop` 不得在 IO 线程直接
  `join`；应把 `join` 交给独立收尾线程，并在收尾完成后上报完成通知
  (完成通知允许来自任意线程)。线程空闲时可在调用线程直接收尾 (省一次线程切换)。
- **停止即拒绝新工作**：停止标志生效后不再接受新 Operation；已入队但未开始的任务
  必须在插件线程内按终态终结 (不得执行插件 JS 代码)，已开始的任务观察停止标志后
  按取消完成 —— 满足"所有已接受操作必须终结"。
- **线程空闲判定必须有不变式**：宿主线程据"无在手中任务 + 队列为空"判定可直接收尾，
  因此插件线程取任务/执行定时器与置忙标记必须在同一临界区内完成。
- **脚本/子对象注册归属调用方实例**：引擎承载的脚本注册挂在调用方 (壳插件) 实例上，
  由宿主在该实例停用/卸载时统一撤销；引擎自身的 `stop` 只释放运行时，不假设宿主
  注册表的清理顺序。

### 15.6 平台支持矩阵与验证状态

- **平台 gate**：各插件在自身 `CMakeLists.txt` 开头调用 `agentxx_plugin_platform_gate`
  声明支持平台，列表为空表示**全平台跳过**（无实现）。跳过发生在 `add_subdirectory`
  入口，内置合并清单登记也随之天然跳过。
- 当前矩阵：`screen_capture` / `computer_use` / `text_selection_monitor` 仅 Windows；
  `agentxx_audio_stream` **全平台跳过**（WASAPI 实现未启用，`audio_stream.cpp` 中该分支
  带 `&& false`，仅剩桩实现；实现可用后声明 `windows` 并同步本节）。
- **已验证平台**（Reset-v1 重构验收范围）：
  - Windows（MSVC 14.51 / VS18，Debug + ASan）：全插件构建（19 个 DSO）、插件专项
    1765/0、扩展回归 2251/0、工具模块 360/0；
  - Linux（GCC 16.1，Debug + ASan/LSan、定向 UBSan、定向 TSan）：插件框架 TSan 0 告警，
    ASan 扩展回归 2179/0；
  - Android：未验证。
- **测试二进制在 Windows 上的运行前提**：工作目录必须是可执行文件所在目录
  （`exec/`），因为插件目录按 `GetModuleFileNameW` 推导；Linux 用 `/proc/self/exe`。
- 平台矩阵的迁移记录与逐条验证结果见
  `resource/history/plugin-refactor-2/work.md`（1.10、3.6 节）。

---

## 16. Reset-v2 协程驱动 (通用 pump/wake 协议 + `PollOneBridge`)

> 本节描述 Reset-v2 引入的**协程驱动协议**：插件协程与宿主协程在同一宿主 IO 执行
> 序列中交错推进，不额外开线程、不阻塞 IO；等待以"宿主可见唤醒源"为主，插件本地 reactor 上的
> 内核就绪等待由**声明式受控轮询**（`polled_tool`）驱动，见 §16.5。方案与阶段划分见
> `resource/history/plugin-refactor-3/plugin.md`，实施记录见
> `resource/history/plugin-refactor-3/work.md`。

### 16.1 为什么需要它

插件可以自带协程库、事件循环与第三方异步库，而宿主不能把插件私有 reactor 的等待
对象接进自己的执行序列（一个私有 `io_context` 没有跨平台公共 API 能把它注册给另一个
`io_context`）。因此两端只经两类动作协作：

- **driver / pump**：插件申请宿主异步执行一次**有界回调**（推进本地运行时一个有限
  步骤：例如一次 `poll_one`，即一个就绪 handler）；
- **wake**：插件适配器知道本地已有可运行工作时（新 root 首步、宿主回调投递的
  continuation、本地 post），向宿主请求一次 driver 请求；重复 wake 由适配器合并。

宿主不需要知道插件用哪种 coroutine/future/actor，插件也拿不到宿主 executor。

```
宿主 IO 线程                       插件适配器/桥                    插件本地运行时
  │ request_driver ───────────────────▶│ 登记请求 (持实例 lease)
  │◀──────────────────── 请求 (异步) ──│
  │ drive_once() ─────────────────────▶│ poll_one() ───────────▶ 推进一个 continuation
  │                                    │ ◀── 外部完成回调 (post + wake)
  │ drive_once() ─────────────────────▶│ poll_one()
  │◀──────────── done(status, payload) │ 根操作终结 (exactly-once)
```

### 16.2 C ABI (`agentxx.agent.coroutine_runtime`, version 1)

```c
typedef struct AgentxxPluginDriver AgentxxPluginDriver;
typedef void(AGENTXX_PLUGIN_CALL* AgentxxPluginDriveOnceFn)(void* user_data);

typedef struct AgentxxPluginCoroutineRuntimeIface {
    int32_t  version;      // == 1
    uint32_t struct_size;
    AgentxxPluginDriver* (AGENTXX_PLUGIN_CALL* request_driver)(
        const AgentxxPluginHost*, AgentxxPluginDriveOnceFn, void* user_data, AgentxxPluginString* error_out);
    void   (AGENTXX_PLUGIN_CALL* cancel_driver)(AgentxxPluginDriver*);
    int32_t(AGENTXX_PLUGIN_CALL* is_io_thread)(const AgentxxPluginHost*);
} AgentxxPluginCoroutineRuntimeIface;
```

**契约（宿主与插件共同遵守）**

1. `request_driver` **任意线程可调用、永不内联**回调：即使调用者就在 IO 线程，请求也
   经宿主任务队列异步投递。否则 root start / completion / cancel 会形成意外重入，
   并失去交错执行的公平性。
2. 一次请求**至多执行一次**回调，且回调只推进一个有限步骤：**不得阻塞、不得等待
   事件、不得同步调用宿主业务接口**；异常必须由插件自己捕获。
3. 插件侧适配器**自行合并 wake**（同一实例同时只登记一次请求）；宿主仍会做去重
   （同一时刻只保留一次已登记的请求）与关闭救援作为最后防线。
4. `cancel_driver` **幂等、非阻塞**：尚未开始的请求之后不再执行回调；正在执行的
   回调不会被强行中断，由插件自己的 root 收束协议收尾。
5. 请求在**排队与执行期间持有实例执行 lease**，因此 `dlclose` 不会越过仍在排队或
   正在执行的插件代码（卸载的 idle 等待必然覆盖它）。
6. 失败语义：`request_driver` 返回 `NULL + error_out` 表示宿主不再提供驱动；插件必须
   把受影响的操作以失败/取消终结，不得静默丢弃（kit 会自动如此处理）。

### 16.3 宿主侧实现语义

- **请求状态机**：`Idle → Running → Finished` 或 `Idle → Finished`（取消），迁移由
  一次 CAS 仲裁，保证"取消后不再执行回调"与"lease 恰好释放一次"。
- **句柄校验**：`cancel_driver` 的 ABI 形态不含 host 参数，宿主无法从实例反查合法
  句柄，因此请求在创建时登记进**进程级地址注册表**（只存 `weak_ptr`，收束时按地址
  摘除）。伪造/过期指针只会被安全忽略并记日志，**绝不解引用**。
- **admission 模式**：请求采用 `lifecycle` 模式的实例 lease —— 实例进入 `Closing`
  后仍允许驱动。这是必需的：关闭的第一步是取消全部 Operation，而插件的取消收束
  （取消回调 → 唤醒 → 下一个有限步骤）必须能继续跑完，否则关闭必然超时。
  `Disabled`/`Closed` 一律拒绝。
- **关闭救援**：宿主只在**已判定关闭失败**（`waitInflightZero` 超时）时调用
  `cancelPendingDrivers()`，取消仍未开始的请求以免 lease 永久残留并记日志。
  正常关闭不依赖它：kit 在实例上下文销毁时自行 `cancel_driver`。

### 16.4 kit 侧实现 (`detail::PollOneBridge` + `detail::BridgeRoot` / `detail::PolledRoot`)

`PluginBase::bridgeOrNull()` 在宿主提供 `coroutine_runtime` 时返回本实例的桥
（每实例一份，无任何进程级可变状态）；否则返回 `nullptr`，kit 自动回退到
`post_to_io` 的旧路径（伪宿主/旧宿主）。

**状态机（三个竞态窗口都覆盖）**

| 状态 | 含义 |
|------|------|
| `readySteps_` | 已投递但尚未执行的本地步骤数（每一步需要一次 `poll_one`） |
| `wakePending_` | 一次显式 `wake` 尚未被请求覆盖（兼容"插件直接向 `local_executor` 投递"） |
| `driverQueued_` / `driverRunning_` | 已申请请求（含 `request_driver` 正在返回的窗口）/ 回调正在执行 |
| `pendingEpoch_` / `nextEpoch_` | 请求世代，用于识别"回调已消费本轮请求"的窗口 |
| `polledRoots_` | 在途**受控轮询根**数（声明式 `polled_tool`；0 = 不轮询、不建定时器） |
| `pumpPending_` | 受控轮询判定"该继续驱动"（第二类申请请求理由） |
| `pumpWaitScheduled_` / `pumpWaitOp_` | 在途退避定时器（宿主 `scheduler.sleep`）与其句柄 |
| `pollBurst_` | 连续"有进展"步数（用于突发上限让出） |

关键不变量：

1. 每张请求**恰好一次** `localIo_.poll_one()`（一个 host driver ⇔ 一个局部 continuation）；
2. 只有确实还有可运行工作（`readySteps_ > 0`、未覆盖的显式 wake，或有在途 polled 操作
   且轮询策略判定需要续票）才申请下一次请求，`poll_one()==0` **不会**在无工作的状态下
   重新排队 —— 空闲时既不占宿主任务队列也不建定时器；
3. 宿主回调完成后只做 `postToLocal(continuation) + wake()`，**绝不在宿主回调栈内
   恢复插件协程**，也不在宿主 IO 线程上跑插件业务代码；
4. 并发投递 N 个步骤最终会得到 N 张请求（不会因 wake 合并只推进一个）；
5. 宿主拒绝驱动/桥停止时，所有活跃根（含受控轮询根）被终结为失败（`FAILED`），
   宿主 Operation 因此不会悬挂。

**受控轮询（pump）调度策略**（只作用于 `polled_tool` 的根，见 §16.5）：

```
本轮 poll_one 执行到了 handler（有进展）且 pollBurst_ < kPollBurstMax(256)
    → 立即申请下一次请求        （等待中的 socket/管道/文件就绪能被尽快收走）
否则（无进展 / 达到突发上限）
    → scheduler.sleep 安排一次退避（无进展 10ms；突发上限后让出 1ms）
      → 到期回调只 request_driver（不恢复插件协程）
polledRoots_ 归零
    → 取消在途退避，之后不再申请请求（空闲零开销）
```

**根的生命周期（`BridgeRoot` / `PolledRoot`）**

- 根对象由**桥持有强引用**（`roots_`）：挂起中的根除了"下一次恢复任务"没有任何持有者，
  桥若不持有，排队任务执行完就会销毁帧，之后到达的宿主回调就会访问已释放的协程帧；
- 完成/放弃的仲裁是 **exactly-once**（CAS）：正常完成走 `finishIfDone`，宿主拒绝
  驱动走 `abandon`，两条路径只有一个能上报终态；
- 被放弃的根移入 `abandonedRoots_`，**帧活到桥销毁为止**（那时实例已无未完成的宿主
  操作），期间迟到的宿主回调因 `shouldAdvance()==false` 安全跳过；
- 协程帧的销毁与 op 句柄（`Job`）的释放统一发生在一个"协程已终止或不可能再被恢复"
  的时点（`destroyFrame()`）：正常完成时在 `finishIfDone` 内；放弃路径在桥销毁时。
  释放动作**不能**放在 `abandon` 里 —— 被放弃的根可能正在 driver 内执行，其输入
  (`RootRequest`/`OpCtl`) 仍被协程以引用使用。

受控轮询根用同一套仲裁语义但形态更简单（`PolledRoot`，见 `polled_tool`）：

- 协程是 `asio::awaitable`，其**帧由 asio 自己持有**（completion handler / 本地 reactor
  销毁时统一释放），因此 `PolledRoot` 不销毁帧，只做"终态上报 + Job 回收"的
  exactly-once 仲裁（正常完成 vs 桥停止时放弃，用一次 CAS 决定）；
- 正常完成：`detail::runPolledPumpJob` 认领 → 注销登记（`polledRoots_` 递减，必要时
  停止 pump）→ 上报终态 → 执行清理回收 `Job`；
- 桥停止：`failAllPolledRoots` 把在途根整批摘下 → 每个根按 `FAILED` 上报一次 →
  执行清理回收 `Job`；挂起的帧随本地 reactor 销毁而释放。

**已接桥的 kit 路径**

| kit 组件 | 桥接语义 |
|----------|----------|
| `tool` / `hook` / `capability` / `graph_node`（返回 `Task<T>`） | 首步由 host driver 推进；start 内不跑插件协程 |
| `spawn`（后台任务） | 同上（首步不在调用方栈内执行） |
| `sleep(ctx, ms)` | 宿主计时器适配：到期回调只 post + wake |
| `call_tool` / `invoke_cap` | 宿主回调式互调：完成回调只 post + wake |
| `yield(ctx)` | 让出一轮：投递 continuation 后由下一次请求推进 |
| `offload(ctx, work)` | **工作体仍在宿主工作线程池**（显式例外），只有完成后的恢复回到 driver 序列 |
| `polled_tool(ctx, name, …)` | **声明式受控轮询**：业务体是 `asio::awaitable<std::string>`，跑在桥的本地执行器上，由"有进展立即续 + 无进展 10ms 退避 + 突发上限 256 后让出 1ms"推进（见 §16.5） |
| `fast_tool` / 同步 hook / `blocking_tool` | 不变（同步路径本就不需要驱动） |

`bridge().local_executor()` 暴露本地执行器，插件可用 `asio::co_spawn` 把自己的
`asio::awaitable` 排进同一序列；这些 awaitable 需要等待"有宿主可见唤醒源"的操作
（宿主回调适配器 / 宿主计时器 / 已 post 的 continuation），或者改用 `polled_tool`
声明"该操作用受控轮询驱动"（见 §16.5）。

### 16.5 受控轮询（`polled_tool`）与明确不支持

**为什么需要它**：`poll_one()` 只能"执行已就绪的 handler"，不会让**插件本地 reactor**
上的等待对象到期。因此"业务体本来就是 asio 协程、等待的是 socket/管道/文件/本地 timer
的内核就绪事件"这类工具（websearch 的 HTTP、execute_command 的子进程管道、filesystem
的 `asio::stream_file`），在没有 wake source 时无法被推进。

**方案：声明式受控轮询（`polled_tool`）**。插件在注册时就**声明**"该工具需要受控轮询
驱动"（而不是隐藏轮询），参数显式且可观测：

| 项 | 值 | 说明 |
|---|---|---|
| 触发条件 | `polledRoots_ > 0` | 只在有在途 polled 操作时轮询；空闲零请求、零定时器 |
| 有进展 | 立即续票 | 本轮 `poll_one` 执行到了 handler（就绪事件被收走） |
| 无进展 | 退避 10ms | `PollOneBridge::kPollIntervalMs`，经宿主 `scheduler.sleep`；到期回调只 `request_driver` |
| 突发上限 | 连续 256 步后让出 1ms | `kPollBurstMax` / `kPollBurstYieldMs`，避免同实例自循环独占 IO 线程 |
| 取消 | 置取消标志 + 取消在途退避 + 取消写入 `CancelRegistry` | 插件不必等满一个退避量子即可看到取消并收束根 |
| 无 driver 的宿主 | 自动降级 | 无 `coroutine_runtime`/`scheduler.sleep` 时用 offload 工作线程 + 局部 `io_context` 跑完（等价 `blocking_tool`） |

业务签名与 `blocking_tool` 同形（只是返回 `asio::awaitable<std::string>`），因此迁移
通常只是换一个注册函数名：

```cpp
polled_tool(ctx, name, depict, schema,
    [](Ctx& c, std::string_view args, std::string_view tid, std::string_view workDir,
       const AgentxxPluginCancelToken* cancel) -> asio::awaitable<std::string> {
        ArgReader reader(args);
        co_return co_await doSomethingAsync(reader.raw(), c.workDir(tid));
    });
```

代价与约束（必须与实现一起遵守）：

1. **10ms 量子**：单实例等待期间 ≈100 次/秒驱动（每次 ≈1 个宿主 post + 1 次
   `epoll_wait(0)`），等待阶段最坏多 10ms 延迟（网络/子进程可接受）；实现为常量便于调优。
2. **重 CPU 段不得留在 polled 协程里**：它运行在宿主 IO 线程上（>100ms 会触发宿主
   看门狗告警）。目录遍历 / 全文件扫描 / 向量相似度继续用 `blocking_tool`；确需在
   polled 协程内做重计算的用宿主 `offload` 显式卸载。
3. **polled 协程内不使用 kit 的 `Task` 型原语**（`sleep`/`call_tool`/`invoke_cap`）：
   它们的 awaiter 依赖 kit 自有 promise 接口，而 polled 协程是 `asio::awaitable`。
   需要计时用 asio 原生 `steady_timer`（pump 下可用），需要宿主回调式接口时经
   `bridge().local_executor()` 自行投递后续步骤（后续可选补 asio 版适配器）。
4. **可替换性**：将来实现 `wait_source`（宿主等待插件交出的 fd/handle）或宿主侧 IO
   服务后，只需把"何时申请下一次请求"的策略从"10ms 退避"换成"就绪通知"，polled 工具
   的业务代码与 ABI 形态不变。

**仍然明确不支持 / 需要显式例外**：

- **未声明 polled 却依赖私有 reactor**：仅依赖私有 `io_context` 的 socket/timer/
  process/file 等待**不会**被桥推进。桥在同一实例持续 8 次 driver 无进展且仍有活跃根时
  输出一次警告（"awaited work has no host-visible wake source"），提示改用
  ①宿主回调式完成（适配器 post + wake）②宿主计时器（`co_await sleep`）
  ③`polled_tool` 声明式受控轮询 ④显式受限工作线程（`offload` / `blocking_tool`，并在
  能力说明里标注）；它不会自旋，因此警告之后请求数不再增长。
- **误用检查**：首次驱动会校验是否运行在宿主 IO 线程（`is_io_thread`），不符时记日志。

### 16.6 内置插件迁移状态

- **所有使用 kit 的插件自动接桥**：`tool`/`hook`/`capability`/`graph`/`spawn` 与
  `sleep`/`call_tool`/`invoke_cap`/`yield`/`offload` 的恢复路径都已走驱动序列，插件
  业务代码无需改动。
- **已迁移到受控轮询（`polled_tool`）**：

  | 插件 / 工具 | 依据 |
  |---|---|
  | `agentxx_websearch`：`web_search` / `web_fetch` / `web_fetch_markdown` | 实现体本就是 asio 协程（`co_await HttpClient::*Async`）；网络等待不再占用宿主工作线程池，同实例的 HTTP keep-alive 连接池天然复用 |
  | `agentxx_execute_command`：`execute_bash_command` / `execute_windows_command`（Boost.Process v2 分支） | 子进程管道/计时器绑定协程 executor；并发多命令共享同一 poll 序列与同一个本地 reactor，不再各占一个池线程直到超时 |
  | `agentxx_filesystem`：`read` / `write` / `edit` | `asio::stream_file` 异步读写；本构建启用 io_uring 时文件 IO 真异步，避免大文件读写占用池线程 |

- **保持 `blocking_tool`（显式例外）**：
  - `agentxx_filesystem`：`list` / `glob` / `grep` —— 目录遍历 + 全文件扫描 + 正则/编码
    转换属 CPU/阻塞 IO（asio 无异步目录 API），放进 pump 只会阻塞同实例其它工具；
  - `agentxx_execute_command`：非 Boost.Process v2 的 popen 回退分支（同步实现）；
  - `agentxx_filesystem`：`BOOST_ASIO_HAS_FILE` 不可用平台（同步回退，注册侧自动切回
    `blocking_tool`）；
  - `agentxx_rag_search`：embedding 网络段先把实现体恢复为协程形态（其注释记录了
    "原版 asio 协程接口改为同步实现"）再迁移；分块/相似度等 CPU 段继续 offload（**二期**）。
- **保持现状（无私有 reactor 等待）**：codegraph / planning / system_monitor / math /
  string / system / JS 系插件；JS 引擎是既有"自管线程 + notify"正确形态。
- **`ClientPluginManager` 的 `asio::thread_pool(1)`**：只用于 `dlopen`/entry 这类不可避免的
  阻塞动态库工作（entry 内的注册动作仍经 vtable 投递回 IO 线程），保持为独立、可关闭的
  后台设施；client 侧插件不产生需要驱动的协程根，因此 client kit 不创建桥
  （宿主已暴露同一 IID，后续需要时可直接接入）。
- **样例**：
  - `example_bridge`：两个"真实唤醒源"（宿主计时器 + 宿主回调式互调）与桥诊断字段；
  - `example_polled_timer`：**受控轮询**样例 —— 在插件本地 executor 上 `co_await`
    3 次 asio `steady_timer`，返回实测耗时与 `driverAvailable/onHostIoThread/pumpOnStart`。

### 16.7 验证

| 层次 | 用例 |
|------|------|
| C ABI | `test_plugin_abi_c17.c`：协程驱动表 8 字节对齐、`version/struct_size` 偏移、版本号；C++ 侧逐项对照（`plugin_runtime`） |
| 宿主请求 | `plugin_runtime`：恒异步、每票至多一次、取消后不再执行、排队持 lease、幂等取消、伪造句柄安全忽略、Closing 允许 / Closed 拒绝、空回调返回 `NULL + error_out` |
| kit 桥接 | `plugin_bridge`（伪宿主 C ABI 驱动）：不内联、每票一次 `poll_one`、空闲不自旋、wake 三个窗口不丢、宿主回调不重入、取消唯一终态、拒绝驱动即终结、stop 取消排队请求、多实例隔离、无 `coroutine_runtime` 时回退 |
| kit 受控轮询 | `plugin_bridge`：首步不内联、有进展立即续票、无进展恰好一次 10ms 退避（不新增请求）、根结束即停止轮询（取消在途退避）、取消会取消在途退避并只产生一个 `CANCELLED` 终态、`stop` 时在途 polled 根按 `FAILED` 终结一次并回收 `Job`、突发上限触发 1ms 让出、无 `coroutine_runtime` 时降级为 offload 跑完且不创建桥 |
| 端到端 | `plugins`：`example_bridge` 与 `example_polled_timer` 经真实宿主执行，断言 `driverAvailable/onHostIoThread/pumpOnStart`、"插件挂起期间宿主任务仍在推进"（同一 IO 序列交错执行）与 asio 原生 timer 真正到期；1000 并发工具调用压力用例 |
| 端到端（迁移插件） | `plugins`：`agentxx_filesystem` read/write/edit（受控轮询）+ list（offload）同一实例共存；`agentxx_websearch` 经本地回环 HTTP 服务完成 fetch/fetch_markdown 与 6 路并发（互不阻塞）；`agentxx_execute_command` 在 `sleep 5` 挂起期间卸载 —— 取消收束、pump 停止、inflight 归零且耗时远小于命令自身超时 |
| 内存 | `plugin_bridge` 单独运行 0 泄漏；插件专项 ASan+LSan 与重构前基线逐项一致（4480 字节 / 64 处），含受控轮询新增用例（在途卸载/放弃路径）后不变 |
