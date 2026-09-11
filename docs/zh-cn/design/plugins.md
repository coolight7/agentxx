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
最新框架提供了开箱即用的声明式导出宏、链式 Schema 构建器、宽容参数提取器与通用取消注册中心：

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
| `agentxx.agent.scheduler` | 1 | `is_io_thread/post_to_io/pump_io`, `sleep/cancel_sleep`, `offload` (阻塞池委托, 需 cancel_flag) |
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
     旧 UI 快照因此只能回退，不会调用已卸载插件的函数指针。
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
- **执行流程**：宿主加载 JS 插件壳 → 壳在 `create` 阶段调用 `interpreter.js` 能力将 `plugin.js` 交给 QuickJS 引擎 → 引擎在专用线程中解析并执行脚本，将脚本中声明的工具/钩子反向注册到宿主
- **`agentxx.callTool` 始终返回 Promise**：命中本引擎工具时同线程执行并把结果/内部 Promise 链到外层 Promise；命中宿主插件工具时经 `call_tool_async` 异步互调，完成/失败/取消事件投递回 JS 线程 settle。JS 线程不会同步等待宿主，A/B 脚本互调不存在线程自锁
- **脚本初始化是注册事务**：脚本顶层注册的工具/钩子/订阅/资源/定时器在顶层异常或脚本卸载时统一回滚（先撤销宿主注册，再释放 JSContext），不留悬垂 user_data；`hookStart` 与能力 `unload` 的完成通知在 JS 执行真正结束后发出
- 可自研脚本引擎插件 (Python/Lua 等) 替换或扩充脚本能力

---

## 13. 插件示例索引

| 插件 | 说明 |
|------|------|
| `example_plugin` | 原生 C++ 综合示例 (fast_tool/Task 协程/call_tool/sleep/钩子/事件/能力/client 入口) |
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
| `agentxx_audio_stream` | 音频流捕获 (仅 Windows WASAPI) |
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
