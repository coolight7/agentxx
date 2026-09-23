/// agentxx_planning —— 两层任务规划工具插件 (双端: agent 工具 + client 渲染)
/// - 从 libagentxx src/tools/planning 拆分独立:
///   - agent 侧工具 agentxx_planning (原 agentxx_planning_write 改名):
///     mode=write 写入会话规划 state (经通用接口: 持久化到
///     {dataDir}/plans + 发布 planning 事件, 不再依赖专用 planning 接口表) 并持久化到
///     {dataDir}/plans/{thread_id}.json (供 read 模式跨轮次读取);
///     mode=read 返回本会话此前保存的规划内容
///   - 规划写入成功后发布 "agentxx_planning.planning" 插件事件 (载荷为完整
///     规划 JSON); 订阅宿主约定事件 agentxx_host.client_attached, 客户端接入/
///     重连时重发当前会话已保存规划 (状态快照自愈, 见
///     [plugins.md](/docs/zh-cn/design/plugins.md) 7.3.1)
///   - client 侧入口 (agentxx_plugin_client_create): Plan 渲染完全由插件驱动 ——
///     ① 工具消息渲染两条路径, 内容同源 (同一构建函数):
///        a. 类型级渲染器 (register_tool_renderer, 按 tool_name): 由消息自带的
///           参数/结果推导折叠头显示名/摘要与展开体 items (状态图/todos/notes),
///           **实时调用与历史回溯 (重启/重连/切换会话后 Sync 回放) 统一生效**;
///        b. 运行时装饰 (update_tool_decor, 订阅 EVT_DELTA): 仅覆盖本进程实时
///           调用, 用于展开头显示语义名 ("Plan")
///        TUI 按通用渲染器展示, 无任何 plan 特化代码;
///     ② Info 栏段落渲染最近一次规划概览
#include "agentxx_planning_plugin.h"
#include "utilxx_base/json.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fmt/ranges.h>
#include <fstream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <vector>

using namespace agentxx_planning_plugin;

namespace {

constexpr std::string_view kNamePlanning = "agentxx_planning";

constexpr std::string_view kDepictPlanning =
    R"(Two-level task planning tool for complex multi-step work sessions.

=== Modes (`mode`, required) ===
- `write`: Save/update the planning content (requires `roadmap`; optional
  `todos`/`notes`). The plan is applied to the session context and persisted,
  so a later `read` can retrieve it even after context compaction.
- `read`: Return the planning content previously saved in this session
  (by an earlier `write`). No other arguments needed.

=== Strategic Layer: `roadmap` (write, required) ===
A Mermaid stateDiagram-v2 capturing the OVERALL workflow — the big picture.
This is your roadmap: major phases, dependencies, error recovery paths, and the
start-to-finish flow. Update this diagram whenever the plan changes (new tasks,
completed phases, dead ends). After execution is completed, make an overall summary.

State diagram conventions:
- Use `[*]` for start/end pseudo-states
- Name state nodes like `phase_N_description` (e.g. `phase_1_search_codebase`)
- Status transitions: pending → in_progress → completed | failed
- Show branching: what happens on success vs failure
- Replace the entire diagram each call

=== Tactical Layer: `todos` (write, optional) ===
A short list of IMMEDIATE and NEXT-STEP tasks only. Do NOT list every state
from the diagram — only the tasks you are actively working on or about to start.

=== MEMO Layer: `notes` (write, optional) ===
Record any important information, tips, reminders, or identity/role-playing prompts.

Example for a "fix a bug" workflow:
- mode: write
- roadmap:
```mermaid
stateDiagram-v2
    [*] --> 1_reproduce_bug
    1_reproduce_bug --> 1_in_progress: start
    1_in_progress --> 1_completed: reproduced
    1_in_progress --> 1_failed: cannot reproduce
    1_completed --> 2_locate_root_cause
    2_locate_root_cause --> 2_in_progress: analyze
    2_in_progress --> 2_completed: found cause
    2_completed --> 3_implement_fix
    3_implement_fix --> 3_in_progress: coding
    3_in_progress --> 3_completed: fix works
    3_completed --> [*]
```
- todos (only current + next):
[
  {"state":"in_progress", "content":"Reproduce the crash with provided stack trace"},
  {"state":"pending", "content":"Locate root cause by tracing the null pointer source"}
]
- notes:
    - Follow user code style guide.
    - Add unit tests after change.
)";

constexpr std::string_view kSystemPlanningPrompt = R"_(
## Planning

You have access to the `agentxx_planning` tool to manage and plan complex objectives.
Use this tool for multi-step tasks to ensure you track each necessary step.
It helps break down large objectives into smaller, manageable steps.

- Mark todos as completed as soon as you finish a step. Do NOT batch completions.
- For simple objectives (few steps), skip planning and execute directly.
- Planning costs tokens — use it only for complex, many-step problems.

### Important Notes

- Call with `mode="write"` to save/update the planning content (provide `roadmap`,
  optional `todos`/`notes`); call with `mode="read"` to retrieve the planning
  content previously saved in this session (e.g. after context compaction).
- Never call `agentxx_planning` multiple times in parallel.
- Revise the plan as new information emerges. Remove irrelevant tasks, add newly discovered ones.

### Finishing a Task

When all work is done, write your final answer in the message AFTER your last `agentxx_planning` call — not in the same turn.
Start the final message with the substantive content the user asked for (data, computation, summary, or analysis).
The user wants the result, not confirmation that the work is done.
)_";

constexpr std::string_view kArgModeDesc = R"(Operation mode:
`write`: Save/update the planning content (requires `roadmap`; optional `todos`/`notes`).
`read`: Return the planning content previously saved in this session (no other arguments).)";

constexpr std::string_view kArgRoadmapDesc =
    R"((write only) STRATEGIC LAYER: Mermaid stateDiagram-v2 of the overall workflow.
Include ALL phases even if not yet started. Each phase gets state nodes for its
statuses (pending/in_progress/completed/failed) with transitions showing
dependencies and error recovery paths. Use `[*]` for start/end.
Replace the entire diagram each call.)";

constexpr std::string_view kArgTodosDesc = R"((write only) TACTICAL LAYER: Near-term task items.
Focus on what you are actively doing NOW and what comes NEXT.
Do NOT list all phases from the diagram — only immediate execution items.

Item struct:
{
    "state": "pending",   // enum: pending, in_progress, completed, failed
    "content": ""         // task description
})";

constexpr std::string_view kArgNotesDesc = R"((write only) MEMO LAYER: Any additional notes.
Use this to record important information, tips, reminders, or identity/role-playing prompts.)";

/// ==================== 规划持久化 ({dataDir}/plans/) ====================
/// - 文件名: thread_id 经字符清洗 (非 [A-Za-z0-9._-] → '_') 截断后追加
///   FNV1a-32 哈希后缀, 确定且无碰撞歧义 (同 thread_id 恒定同名)
/// - 写入原子化: 先写 .tmp 再 rename (Windows 下 rename 不覆盖已有目标,
///   先 remove 旧文件; 极端崩溃窗口最多丢失一次更新, 可接受)

/// FNV1a-32 哈希 (thread_id 全原文; 与清洗后的 base 无关, 防截断碰撞)
constexpr uint32_t fnv1a32(const char* s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint8_t>(s[i]);
        h *= 16777619u;
    }
    return h;
}

/// thread_id → 安全文件名 (base_XXXXXXXX.json)
std::string planningFileName(const std::string& tid) {
    std::string base;
    base.reserve(tid.size());
    for (char c : tid) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                        || c == '-' || c == '_' || c == '.';
        base += ok ? c : '_';
    }
    if (base.size() > 48) {
        base.resize(48);
    }
    char suffix[16]{};
    std::snprintf(suffix, sizeof(suffix), "%08x", fnv1a32(tid.data(), tid.size()));
    base += "_";
    base += suffix;
    base += ".json";
    return base;
}

/// 宿主数据目录 (AgentConfig dataDir; 经 agentxx.agent.config 接口表)
std::string hostDataDir(const PluginCtx& ctx) {
    if (!ctx.host || !ctx.iface.config || !ctx.iface.config->get_config) {
        return {};
    }
    PluginxxString j{nullptr, 0};
    ctx.iface.config->get_config(ctx.host, &j);
    if (!j.data) {
        return {};
    }
    std::string s(j.data, static_cast<size_t>(j.size));
    agentxx::plugin::PluginString::free(ctx.host, &j);
    try {
        auto o = utilxx_base::Json::parse(s);
        return o.value("dataDir", std::string{});
    } catch (...) {
        return {};
    }
}

/// 规划保存目录 ({dataDir}/plans; dataDir 不可用时为空)
std::filesystem::path plansDir(const PluginCtx& ctx) {
    const auto dir = hostDataDir(ctx);
    if (dir.empty()) {
        return {};
    }
    return std::filesystem::path{dir} / "plans";
}

/// 保存规划 JSON (返回 false = 目录不可用/写失败; 调用方记日志降级)
bool savePlanningFile(const PluginCtx& ctx, const std::string& tid, const std::string& planJson) {
    const auto dir = plansDir(ctx);
    if (dir.empty() || tid.empty() || planJson.empty()) {
        return false;
    }
    try {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto path  = dir / planningFileName(tid);
        auto tmp   = path;
        tmp       += ".tmp";
        {
            std::ofstream o{tmp, std::ios::binary | std::ios::trunc};
            if (!o) {
                return false;
            }
            o.write(planJson.data(), static_cast<std::streamsize>(planJson.size()));
            o.close();
            if (!o) {
                std::error_code rmEc;
                std::filesystem::remove(tmp, rmEc);
                return false;
            }
        }
        // B5: 原子替换（POSIX rename 原子覆盖；Windows 用 MoveFileExW 原子替换）
#ifdef _WIN32
        {
            std::wstring wTmp  = tmp.wstring();
            std::wstring wPath = path.wstring();
            if (::MoveFileExW(
                    wTmp.c_str(),
                    wPath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                )) {
                return true;
            }
            // 回退：先删后改名（极端权限场景）
            std::error_code rmEc;
            std::filesystem::remove(path, rmEc);
            ec.clear();
            std::filesystem::rename(tmp, path, ec);
            if (ec) {
                std::error_code cleanEc;
                std::filesystem::remove(tmp, cleanEc);
                return false;
            }
            return true;
        }
#else
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::error_code cleanEc;
            std::filesystem::remove(tmp, cleanEc);
            return false;
        }
        return true;
#endif
    } catch (...) {
        return false;
    }
}

/// 读取已保存规划 JSON (不存在/读失败返回空串)
std::string loadPlanningFile(const PluginCtx& ctx, const std::string& tid) {
    const auto dir = plansDir(ctx);
    if (dir.empty() || tid.empty()) {
        return {};
    }
    try {
        std::ifstream i{dir / planningFileName(tid), std::ios::binary};
        if (!i) {
            return {};
        }
        std::string s{std::istreambuf_iterator<char>{i}, std::istreambuf_iterator<char>{}};
        return s;
    } catch (...) {
        return {};
    }
}

} // namespace

namespace {

/// ==================== 插件事件发布 ====================
/// 规划内容变化 → 发布 "agentxx_planning.planning" (载荷为完整规划 JSON)。
/// server 侧经事件总线转发为 WirePluginData{plugin, event, data}, client 侧
/// 插件订阅 EVT_PLUGIN_DATA 处理并渲染 Info 栏段落 (见下方 client 入口)。
void publishPlanningEvent(PluginCtx& ctx, const std::string& planJson) {
    if (!ctx.iface.events || !ctx.iface.events->publish || planJson.empty()) {
        return;
    }
    auto topicSv = agentxx::plugin::PluginStringView::fromCstr("agentxx_planning.planning");
    auto planSv  = agentxx::plugin::PluginStringView::from(planJson.data(), planJson.size());
    ctx.iface.events->publish(ctx.host, &topicSv, &planSv);
}

/// 宿主约定事件 client_attached: 客户端接入/重连 → 重发当前会话已保存规划
/// (修复 "事件先于客户端订阅而丢失 → UI 永久空白", 见
/// [plugins.md](/docs/zh-cn/design/plugins.md) 7.3.1)
void PLUGINXX_CALL on_client_attached(const PluginxxStringView* event_json, void* ud) {
    auto* ctxRaw = static_cast<PluginCtx*>(ud);
    // C ABI 回调异常守卫 (agent io 线程派发直调)
    agentxx::plugin::guardCallVoid(
        [ctxRaw](const char* msg) noexcept {
            pluginLog(ctxRaw, 4, msg ? msg : "");
        },
        [&] {
            auto* ctx = static_cast<PluginCtx*>(ud);
            if (!ctx || !ctx->host || agentxx::plugin::PluginStringView::empty(event_json)) {
                return;
            }
            std::string sessionId;
            try {
                sessionId = utilxx_base::Json::parse(
                                std::string{event_json->data, static_cast<size_t>(event_json->size)}
                )
                                .value("sessionId", std::string{});
            } catch (...) {
                return;
            }
            const auto saved = loadPlanningFile(*ctx, sessionId);
            if (!saved.empty()) {
                publishPlanningEvent(*ctx, saved);
            }
        }
    );
}

} // namespace

/// =====================================================================
///  agent 侧入口

/// =====================================================================

extern "C" PLUGINXX_EXPORT const PluginxxInfo* PLUGINXX_CALL agentxx_plugin_agent_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理);
    // 本边界为纯静态元数据, 无实例上下文可捕获 → 空操作日志闭包
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const PluginxxInfo* {
            static const PluginxxInfo info{
                PLUGINXX_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("agentxx_planning"),
                agentxx::plugin::PluginStringView::fromCstr("1.2.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Two-level task planning tool (write/read modes) + client-side Plan rendering"
                ),
            };
            return &info;
        }
    );
}

/// 注册事务 (start 的实际内容): prompt 贡献 + 规划工具注册 + client_attached 订阅。
static int planningSetup(PluginCtx* ctx) {
    const PluginxxHost* host = ctx->host;
    // 注入 planning 附加提示词至宿主 (经通用 appendSystemPrompts 与 toolPrompt)
    if (ctx->iface.prompt && ctx->iface.prompt->set_prompt) {
        utilxx_base::Json j;
        j["appendSystemPrompts"]             = utilxx_base::Json::object();
        j["appendSystemPrompts"]["planning"] = std::string{kSystemPlanningPrompt};

        utilxx_base::Json toolPrompt           = utilxx_base::Json::object();
        utilxx_base::Json planningPrompt       = utilxx_base::Json::object();
        planningPrompt["depict"]               = std::string{kDepictPlanning};
        utilxx_base::Json args                 = utilxx_base::Json::object();
        args["mode"]                           = std::string{kArgModeDesc};
        args["roadmap"]                        = std::string{kArgRoadmapDesc};
        args["todos"]                          = std::string{kArgTodosDesc};
        args["notes"]                          = std::string{kArgNotesDesc};
        planningPrompt["args"]                 = std::move(args);
        toolPrompt[std::string{kNamePlanning}] = std::move(planningPrompt);
        j["toolPrompt"]                        = std::move(toolPrompt);

        std::string js       = j.dump();
        auto        promptSv = agentxx::plugin::PluginStringView::from(js.data(), js.size());
        if (ctx->iface.prompt->set_prompt(host, &promptSv) != 0) {
            pluginLog(ctx, 3, "agentxx_planning: set prompts failed");
        } else {
            pluginLog(
                ctx,
                2,
                "agentxx_planning: appendSystemPrompts[planning] & toolPrompt injected via prompt iface"
            );
        }
    }

    // 规划持久化 + 事件发布为通用接口 (不再依赖专用 planning iface)

    {
        std::string schema = ctx->schema(kNamePlanning)
                                 .enumString(
                                     "mode",
                                     kArgModeDesc,
                                     {"write", "read"},
                                     /*required=*/true
                                 )
                                 .string("roadmap", kArgRoadmapDesc)
                                 .array("todos", kArgTodosDesc, "object")
                                 .string("notes", kArgNotesDesc)
                                 .build();

        agentxx::plugin::fast_tool(
            *ctx,
            kNamePlanning,
            kDepictPlanning,
            schema,
            [](PluginCtx& c, std::string_view args_json, std::string_view thread_id
            ) -> std::string {
                std::string argsStr(args_json.data() ? args_json.data() : "", args_json.size());
                auto        arguments = argsStr.empty() ? utilxx_base::Json::object()
                                                        : utilxx_base::Json::parse(argsStr);

                const auto mode = arguments.value("mode", std::string{});
                if (mode != "write" && mode != "read") {
                    // 参数检查失败抛异常 (宿主统一按工具错误结果回给模型),
                    // 不再返回编码后的错误 JSON
                    throw std::invalid_argument{fmt::format(
                        "Arg `mode` must be \"write\" or \"read\", got \"{}\"",
                        mode
                    )};
                }

                if (mode == "read") {
                    const std::string tid{
                        thread_id.data() ? thread_id.data() : "",
                        thread_id.size()
                    };
                    auto saved = loadPlanningFile(c, tid);
                    if (saved.empty()) {
                        throw std::runtime_error{
                            "No saved planning in this session. Call with mode=\"write\" first."
                        };
                    }
                    try {
                        auto v = utilxx_base::Json::parse(saved);
                        return v.dump(2);
                    } catch (...) {
                        throw std::runtime_error{
                            "Saved planning is corrupted. Rewrite it with mode=\"write\"."
                        };
                    }
                }

                auto roadmap = arguments.value("roadmap", std::string{});
                if (roadmap.empty()) {
                    throw std::invalid_argument{
                        "Arg `roadmap` is empty, must provide a stateDiagram-v2 planning string "
                        "in write mode"
                    };
                }

                std::string todosJson;
                if (arguments.contains("todos") && arguments["todos"].is_array()) {
                    todosJson = arguments["todos"].dump();
                }
                std::string notes = arguments.value("notes", std::string{});

                utilxx_base::Json planStore = utilxx_base::Json::object();
                planStore["roadmap"]        = roadmap;
                if (!todosJson.empty()) {
                    try {
                        planStore["todos"] = utilxx_base::Json::parse(todosJson);
                    } catch (...) {
                        throw std::invalid_argument{"Arg `todos` is not valid JSON"};
                    }
                }
                if (!notes.empty()) {
                    planStore["notes"] = notes;
                }
                std::string planJson = planStore.dump();

                const std::string tid{thread_id.data() ? thread_id.data() : "", thread_id.size()};
                if (!savePlanningFile(c, tid, planJson)) {
                    pluginLog(
                        &c,
                        3,
                        fmt::format("agentxx_planning: persist planning failed tid={}", tid)
                    );
                }

                // 通用接口: 持久化到 {dataDir}/plans + 事件发布 (宿主/客户端通用处理)
                publishPlanningEvent(c, planJson);

                return "success";
            }
        );
    }

    // 宿主约定事件 client_attached 订阅: 客户端接入/重连时重发当前会话快照
    if (ctx->iface.events && ctx->iface.events->subscribe) {
        auto topicSv = agentxx::plugin::PluginStringView::fromCstr("agentxx_host.client_attached");
        if (!ctx->iface.events->subscribe(host, &topicSv, on_client_attached, ctx)) {
            pluginLog(
                ctx,
                3,
                "agentxx_planning: subscribe client_attached failed (UI resnapshot disabled)"
            );
        }
    }

    return 0;
}

extern "C" PLUGINXX_EXPORT int32_t PLUGINXX_CALL
    agentxx_plugin_agent_create(const PluginxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: create 只构造上下文与接口查询, 注册事务由 start 执行;
    // 守卫日志闭包捕获局部裸指针 (ctx 装配前置空 → 异常路径静默丢弃)
    PluginCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            pluginLog(raw, 4, msg ? msg : "");
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx   = std::make_unique<PluginCtx>();
            ctx->host  = host;
            ctx->iface = agentxx::plugin::AgentIfaces::query(host);
            raw        = ctx.get();

            *plugin_ctx = ctx.release(); ///< 所有权移交宿主 (destroy 时取回归还)
            return 0;
        }
    );
}

extern "C" PLUGINXX_EXPORT void* PLUGINXX_CALL agentxx_plugin_agent_start(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        static_cast<void*>(nullptr),
        [&]() -> void* {
            auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
            if (!ctx) {
                agentxx::plugin::PluginString::set(
                    nullptr,
                    error_out,
                    "planning start: null context"
                );
                return nullptr;
            }
            if (!notify) {
                agentxx::plugin::PluginString::set(
                    ctx->host,
                    error_out,
                    "agentxx_planning start: notify required"
                );
                return nullptr;
            }
            if (planningSetup(ctx) != 0) {
                agentxx::plugin::PluginString::set(
                    ctx->host,
                    error_out,
                    "agentxx_planning start: registration failed"
                );
                return nullptr;
            }
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
            return nullptr;
        }
    );
}

extern "C" PLUGINXX_EXPORT void* PLUGINXX_CALL
    agentxx_plugin_agent_stop(void*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        static_cast<void*>(nullptr),
        [&]() -> void* {
            // 无自管线程/定时器; 注册记录由宿主在 stop 后统一撤销。
            if (notify && notify->done) {
                notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
            }
            return nullptr;
        }
    );
}

extern "C" PLUGINXX_EXPORT void PLUGINXX_CALL agentxx_plugin_agent_destroy(void* plugin_ctx) {
    // C ABI 边界异常守卫: 销毁回调异常不得外泄
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(ctxGuardLogger(ctx), [&] {
        delete ctx;
    });
}

/// =====================================================================
/// client 侧入口 (agentxx_plugin_client_create) —— Plan 渲染 (原 TUI 硬编码的
/// 消息列表特化 + Info 侧边栏段落全部拆分至本插件, TUI 无任何 plan 概念)
///
/// - 工具消息渲染 (两条路径内容同源, 见 buildPlanDecorParts):
///   ① 类型级渲染器 (register_tool_renderer, 按 tool_name): 由工具参数/结果
///      直接推导折叠头 displayName/summary 与展开体 items (状态图/todos/notes)。
///      历史回溯路径走这里 —— 重启恢复/重连/切换会话后, 工具消息由 Sync 回放,
///      没有 update_tool_decor 推送, 依赖类型级渲染器才能特化渲染
///   ② 运行时装饰 (update_tool_decor): 订阅 EVT_DELTA, tool_start 按 arguments
///      推送装饰 (折叠头显示名 + 展开体 items), tool_end 以最终内容刷新;
///      本进程内实时调用的展开头因此显示语义名 ("Plan")
/// - Info 栏段落 "Plan" (懒注册): EVT_PLUGIN_DATA planning 事件驱动,
///   展示最近一次规划概览; client_attached 重发快照自愈
/// - EVT_SESSION_SWITCH → 清理装饰与段落
/// =====================================================================

/// client 侧每实例上下文 (多实例契约: 状态挂本实例, 回调经 ud 恢复)
struct ClientCtx {
    const PluginxxHost*           host = nullptr;
    agentxx::plugin::ClientIfaces iface{};
    /// "agentxx.client.ui" 展示接口表 (Info 段落/工具装饰/action/overlay;
    /// CLI 等不支持时成员 NULL 降级: planning 仍推送内容, 只是按钮不可点)
    const AgentxxClientUiIface* ui      = nullptr;
    AgentxxInfoSection*         section = nullptr; ///< 懒注册句柄 (null=未展示)
    std::string                 last_plan_json;    ///< 最近一次规划内容缓存 (Info 段落)
    /// 进行中 planning 工具调用的参数缓存 (tool_call_id → arguments JSON;
    /// tool_start 缓存, tool_end 刷新最终装饰后摘除)
    std::map<std::string, std::string> pending_args;
    /// 通用交互动作控制器 (action_id → handler; dispatch 经 ud 恢复本实例)
    agentxx::plugin::kit::ActionController actions;
    /// 类型级渲染器回调 shim 存储 (registerToolRenderer 创建; 实例销毁时释放。
    /// 宿主在插件停止/卸载时先摘除渲染器注册, 不会回调已释放的 shim)
    std::vector<std::unique_ptr<void, void (*)(void*)>> renderShims;
};

/// planning 状态图动作 id (Info 段/decor 按钮共用; bind 一次永久生效)
constexpr const char* kActionOpenGraph = "planning.open_graph";

/// client 侧守卫日志闭包工厂
static auto clientGuardLogger(ClientCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
            auto sv = agentxx::plugin::PluginStringView::from(msg, std::strlen(msg));
            ctx->iface.log->log(ctx->host, 4, &sv);
        }
    };
}

namespace {

constexpr const char* kSectionId   = "agentxx_planning.plan";
constexpr const char* kDisplayName = "Plan";

/// todo 状态 → 图标 (与历史 TUI 渲染一致)
std::string_view todoIcon(std::string_view state) {
    if (state == "in_progress") {
        return "[~]";
    }
    if (state == "completed") {
        return "[#]";
    }
    if (state == "failed") {
        return "[!]";
    }
    return "[ ]";
}

/// todo 状态 → items role (title=强调/completed, normal=进行中, hint=其余)
std::string_view todoRole(std::string_view state) {
    if (state == "completed") {
        return "title";
    }
    if (state == "in_progress") {
        return "normal";
    }
    return "hint";
}

} // namespace

/// 折叠头一行摘要: todos 格式化为 "[~] a; [ ] b" (与历史 TUI 预览一致)
static std::string buildTodosSummary(const utilxx_base::Json& plan) {
    std::string summary;
    if (!plan.contains("todos") || !plan["todos"].is_array()) {
        return summary;
    }
    for (const auto& td : plan["todos"]) {
        std::string item;
        if (td.is_object()) {
            const auto state   = td.value("state", std::string{});
            const auto content = td.value("content", std::string{});
            if (content.empty()) {
                continue;
            }
            item = fmt::format("{} {}", todoIcon(state), content);
        } else if (td.is_string()) {
            item = td.get<std::string>();
        }
        if (item.empty()) {
            continue;
        }
        if (!summary.empty()) {
            summary += "; ";
        }
        summary += item;
    }
    return summary;
}

/// 追加 Todo 列表与 Note 备忘 items (消息装饰/类型级渲染器/Info 段落共用,
/// 避免多处渲染漂移; 调用方见 [buildPlanItems])
static void appendTodoAndNoteItems(const utilxx_base::Json& plan, utilxx_base::Json& items) {
    auto textItem = [&](const std::string& text, const std::string& role) {
        utilxx_base::Json item = utilxx_base::Json::object();
        item["kind"]           = "text";
        item["role"]           = role;
        item["text"]           = text;
        items.push_back(std::move(item));
    };

    // ---- Todo: 待办列表 ----
    const bool hasTodos
        = plan.contains("todos") && plan["todos"].is_array() && !plan["todos"].empty();
    if (hasTodos) {
        textItem("|- Todo", "normal");
        for (const auto& td : plan["todos"]) {
            if (td.is_object()) {
                const auto state   = td.value("state", std::string{});
                const auto content = td.value("content", std::string{});
                if (!content.empty()) {
                    textItem(
                        fmt::format("{} {}", todoIcon(state), content),
                        std::string{todoRole(state)}
                    );
                }
            } else if (td.is_string()) {
                textItem(fmt::format("[ ] {}", td.get<std::string>()), "hint");
            }
        }
    }

    // ---- Note: 备忘 (与 Todo 分区独立渲染, 避免交错) ----
    if (plan.contains("notes")) {
        const auto& nv = plan["notes"];
        if (nv.is_string()) {
            textItem("|- Note", "normal");
            textItem(nv.get<std::string>(), "hint");
        }
    }
}

/// 规划 JSON → 展开体 items 数组 (Graph / Todo / Note 三段式, 参考剥离前的
/// TUI appendPlanToolBody + Info 侧边栏 Plan 渲染; 见 [plugins.md] §9 items schema)
/// - `graphAsButton` false: 内联状态图 ({"kind":"diagram","mermaid":...};
///   工具消息展开体, 状态图随消息一起渲染)
/// - `graphAsButton` true:  "|- " 前缀 + 可点 Graph 按钮 (Info 段落, 点击经
///   action_id="planning.open_graph" 弹窗; owner_id 由宿主组装)
/// - plan 显式带 items 数组时直接透传 (read 模式占位提示等自定义内容)
static utilxx_base::Json buildPlanItems(const utilxx_base::Json& plan, bool graphAsButton) {
    if (plan.contains("items") && plan["items"].is_array()) {
        return plan["items"];
    }
    utilxx_base::Json items = utilxx_base::Json::array();

    // ---- Graph: 状态图 ----
    const auto roadmap = plan.value("roadmap", std::string{});
    if (!roadmap.empty()) {
        if (graphAsButton) {
            utilxx_base::Json prefix = utilxx_base::Json::object();
            prefix["kind"]           = "text";
            prefix["role"]           = "normal";
            prefix["text"]           = "|- ";
            items.push_back(std::move(prefix));

            utilxx_base::Json button = utilxx_base::Json::object();
            button["kind"]           = "button";
            button["label"]          = "Graph";
            button["action_id"]      = kActionOpenGraph;
            button["args"]           = utilxx_base::Json::object();
            button["role"]           = "normal";
            items.push_back(std::move(button));
        } else {
            utilxx_base::Json diagram = utilxx_base::Json::object();
            diagram["kind"]           = "diagram";
            diagram["mermaid"]        = roadmap;
            items.push_back(std::move(diagram));
        }
    }

    // ---- Todo & Note 渲染 ----
    appendTodoAndNoteItems(plan, items);

    return items;
}

/// read 模式结果尚未返回时的占位内容 (展开体一行提示; 结果到达后渲染器/
/// 装饰按新的输入特征重新计算)
static utilxx_base::Json makeReadingPlaceholderPlan() {
    utilxx_base::Json plan = utilxx_base::Json::object();
    plan["items"]          = utilxx_base::Json::array();
    utilxx_base::Json hint = utilxx_base::Json::object();
    hint["kind"]           = "text";
    hint["role"]           = "hint";
    hint["text"]           = "Reading saved planning...";
    plan["items"].push_back(std::move(hint));
    return plan;
}

/// 工具消息渲染三要素 (折叠头显示名/摘要 + 展开体 items)
struct PlanDecorParts {
    std::string       displayName = kDisplayName;
    std::string       summary;
    utilxx_base::Json items = utilxx_base::Json::array();
};

/// 规划 JSON → 渲染三要素 (实时装饰推送与类型级渲染器共用, 保证两条路径内容一致)
static PlanDecorParts buildPlanDecorParts(const utilxx_base::Json& plan) {
    PlanDecorParts parts;
    parts.summary = buildTodosSummary(plan);
    parts.items   = buildPlanItems(plan, /*graphAsButton=*/false);
    return parts;
}

/// 工具调用参数/结果 → 规划 JSON (实时装饰推送与类型级渲染器共用同一解析):
/// - `mode=write`: 规划内容即参数本体 (roadmap/todos/notes)
/// - `mode=read`:  规划内容即工具结果 (调用时保存的规划 JSON dump);
///   结果未返回时置 `readPlaceholder=true` (调用方使用 [makeReadingPlaceholderPlan])
/// - return: 参数/结果无法解析为对象时返回 false (调用方按通用渲染降级)
static bool planFromToolCall(
    std::string_view   argsJson,
    std::string_view   resultText,
    utilxx_base::Json& plan,
    bool&              readPlaceholder
) {
    readPlaceholder = false;
    utilxx_base::Json args;
    try {
        args = argsJson.empty() ? utilxx_base::Json::object() : utilxx_base::Json::parse(argsJson);
    } catch (...) {
        return false;
    }
    if (!args.is_object()) {
        return false;
    }
    if (args.value("mode", std::string{}) != "read") {
        plan = std::move(args);
        return true;
    }
    if (resultText.empty()) {
        readPlaceholder = true;
        return true;
    }
    try {
        plan = utilxx_base::Json::parse(resultText);
    } catch (...) {
        return false;
    }
    return plan.is_object();
}

/// 推送/更新工具消息装饰 (client io 线程; ui 成员判空降级)
static void
    pushToolDecorParts(ClientCtx& ctx, const std::string& toolCallId, const PlanDecorParts& parts) {
    if (!ctx.ui || !ctx.ui->update_tool_decor || !ctx.host || toolCallId.empty()) {
        return;
    }
    utilxx_base::Json decor     = utilxx_base::Json::object();
    decor["displayName"]        = parts.displayName;
    decor["summary"]            = parts.summary;
    decor["items"]              = parts.items;
    const std::string decorJson = decor.dump();

    auto tcidSv  = agentxx::plugin::PluginStringView::from(toolCallId.data(), toolCallId.size());
    auto decorSv = agentxx::plugin::PluginStringView::from(decorJson.data(), decorJson.size());
    ctx.ui->update_tool_decor(ctx.host, &tcidSv, &decorSv);
}

/// 按规划 JSON 推送装饰 (实时路径; 内容与类型级渲染器同源)
static void
    pushToolDecor(ClientCtx& ctx, const std::string& toolCallId, const utilxx_base::Json& plan) {
    pushToolDecorParts(ctx, toolCallId, buildPlanDecorParts(plan));
}

/// 类型级工具渲染器 (按 tool_name 注册, 宿主在需要渲染 planning 工具消息时于
/// client io 线程调用; 实现无实例状态):
/// - 内容完全由消息自身 (参数/结果) 推导, 不依赖运行时事件 —— 重启恢复/重连/
///   切换会话后由 Sync 回放的历史工具消息没有 update_tool_decor 推送,
///   经本渲染器同样特化渲染 (见 [plugins.md] §9 工具特化渲染架构)
/// - write 模式取参数, read 模式取结果 (结果未返回时占位提示)
/// - 参数/结果无法解析时只保留显示名, 展开体回退通用参数/结果展示
/// - 注意 (框架规则): 展开状态的头部显示名仅由实例级装饰覆盖, 类型级渲染器
///   只覆盖折叠头显示名与展开体 —— 历史恢复场景展开头保留原始工具名
///   ("agentxx_planning"), 折叠头与展开体与实时调用一致
static void buildPlanningToolRender(
    const agentxx::plugin::ToolRenderInput& in,
    agentxx::plugin::ToolRenderOutput&      out
) {
    out.displayName = kDisplayName;

    utilxx_base::Json plan;
    bool              readPlaceholder = false;
    if (!planFromToolCall(in.argsJson, in.resultText, plan, readPlaceholder)) {
        return;
    }
    if (readPlaceholder) {
        plan = makeReadingPlaceholderPlan();
    }
    const auto parts = buildPlanDecorParts(plan);
    out.summary      = parts.summary;
    out.items        = parts.items;
}

/// 清理本插件全部装饰 (会话切换时调用)
static void clearToolDecors(ClientCtx& ctx) {
    ctx.pending_args.clear();
    if (!ctx.ui || !ctx.ui->update_tool_decor || !ctx.host) {
        return;
    }
    PluginxxStringView emptyTcid{nullptr, 0};
    PluginxxStringView emptyDecor{nullptr, 0};
    ctx.ui->update_tool_decor(ctx.host, &emptyTcid, &emptyDecor);
}

/// 懒注册 Info 栏段落 (宿主不支持 info_section 时保持 NULL 静默降级)
static void ensureSection(ClientCtx& ctx) {
    if (ctx.section || !ctx.ui || !ctx.ui->register_info_section || !ctx.host) {
        return;
    }
    auto idSv    = agentxx::plugin::PluginStringView::fromCstr(kSectionId);
    auto propsSv = agentxx::plugin::PluginStringView::fromCstr(R"({"title":"Plan"})");
    ctx.section  = ctx.ui->register_info_section(ctx.host, &idSv, &propsSv);
}

/// 注销 Info 段落并清空缓存 (回到 "无规划" 态: 无 plan 时侧栏不显示空段落)
static void clearSection(ClientCtx& ctx) {
    if (ctx.section && ctx.ui && ctx.ui->unregister_info_section && ctx.host) {
        ctx.ui->unregister_info_section(ctx.host, ctx.section);
    }
    ctx.section = nullptr;
    ctx.last_plan_json.clear();
}

/// 用最近缓存内容刷新 Info 段落 (client io 线程调用)
///
/// - Todo: title + 各 todo 行 (icon+content)
/// - Note: title + 内容
static void refreshPlanSection(ClientCtx& ctx) {
    if (!ctx.host || !ctx.ui || !ctx.ui->update_info_section) {
        return;
    }
    ensureSection(ctx);
    if (!ctx.section || ctx.last_plan_json.empty()) {
        return;
    }
    utilxx_base::Json plan;
    try {
        plan = utilxx_base::Json::parse(ctx.last_plan_json);
    } catch (...) {
        return;
    }
    if (!plan.is_object()) {
        return;
    }

    // 段落 items 与工具消息展开体同源 (同一构建函数), 仅 Graph 表现形式不同:
    // 段落用可点按钮 (点击经 action_id 派发弹窗), 消息展开体用内联状态图
    const auto items = buildPlanItems(plan, /*graphAsButton=*/true);
    if (items.empty()) {
        return; // 内容为空不推送, 避免出现只有标题的空段落
    }
    utilxx_base::Json payload = utilxx_base::Json::object();
    payload["items"]          = items;
    const std::string json    = payload.dump();
    auto              jsonSv  = agentxx::plugin::PluginStringView::from(json.data(), json.size());
    ctx.ui->update_info_section(ctx.host, ctx.section, &jsonSv);
}

/// EVT_PLUGIN_DATA: 过滤本插件规划事件 {plugin:"agentxx_planning", event:"planning"}
/// → 更新 Info 栏段落
static void PLUGINXX_CALL on_client_plugin_data(const PluginxxStringView* payload_json, void* ud) {
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    agentxx::plugin::guardCallVoid(clientGuardLogger(ctxRaw), [&] {
        auto* ctx = static_cast<ClientCtx*>(ud);
        if (!ctx || !ctx->host) {
            return;
        }
        PluginxxString plugin{nullptr, 0};
        PluginxxString event{nullptr, 0};
        PluginxxString data{nullptr, 0};
        auto           kPlugin = agentxx::plugin::PluginStringView::fromCstr("plugin");
        auto           kEvent  = agentxx::plugin::PluginStringView::fromCstr("event");
        auto           kData   = agentxx::plugin::PluginStringView::fromCstr("data");
        if (ctx->iface.json && ctx->iface.json->json_get_string && payload_json) {
            ctx->iface.json->json_get_string(ctx->host, payload_json, &kPlugin, &plugin);
            ctx->iface.json->json_get_string(ctx->host, payload_json, &kEvent, &event);
            ctx->iface.json->json_get_string(ctx->host, payload_json, &kData, &data);
        }
        const bool mine
            = plugin.data && event.data && data.data
              && agentxx::plugin::PluginStringView::str(plugin)
                     == std::string_view{"agentxx_planning"}
              && agentxx::plugin::PluginStringView::str(event) == std::string_view{"planning"};
        if (mine) {
            ctx->last_plan_json.assign(data.data, static_cast<size_t>(data.size));
            refreshPlanSection(*ctx);
        }
        if (plugin.data) {
            agentxx::plugin::PluginString::free(ctx->host, &plugin);
        }
        if (event.data) {
            agentxx::plugin::PluginString::free(ctx->host, &event);
        }
        if (data.data) {
            agentxx::plugin::PluginString::free(ctx->host, &data);
        }
    });
}

/// EVT_DELTA: planning 工具生命周期 → 工具消息装饰 (实时调用路径; 历史回溯
/// 由类型级渲染器覆盖, 见 buildPlanningToolRender)
/// - tool_start: 缓存参数; write 推送完整装饰 (折叠头 Plan · todos 摘要 +
///   展开体 状态图/todos/notes); read 推送占位 (结果未返回)
/// - tool_end: 以缓存的最终参数重建装饰 (覆盖流式期间的不完整内容);
///   read 模式此时展示结果摘要
static void PLUGINXX_CALL on_client_delta(const PluginxxStringView* payload_json, void* ud) {
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    agentxx::plugin::guardCallVoid(clientGuardLogger(ctxRaw), [&] {
        auto* ctx = static_cast<ClientCtx*>(ud);
        if (!ctx || !ctx->host || !ctx->ui || !ctx->ui->update_tool_decor) {
            return;
        }
        if (agentxx::plugin::PluginStringView::empty(payload_json)) {
            return;
        }
        const std::string_view raw{payload_json->data, static_cast<size_t>(payload_json->size)};
        // 快速预过滤: 仅 planning 工具相关 delta 才值得完整解析
        // (deltaToJson 字段序固定 type 在前)
        if (raw.find("\"tool_start\"") == std::string_view::npos
            && raw.find("\"tool_end\"") == std::string_view::npos) {
            return;
        }
        if (raw.find("\"agentxx_planning\"") == std::string_view::npos) {
            return;
        }
        utilxx_base::Json d;
        try {
            d = utilxx_base::Json::parse(raw);
        } catch (...) {
            return;
        }
        const auto type = d.value("type", std::string{});
        if (type != "tool_start" && type != "tool_end") {
            return;
        }
        if (d.value("tool_name", std::string{}) != "agentxx_planning") {
            return;
        }
        const auto callId = d.value("tool_call_id", std::string{});

        if (type == "tool_start") {
            const auto argsStr = d.value("arguments", std::string{});
            // 与类型级渲染器同一解析 (mode=read 时结果未返回 → 占位提示)
            utilxx_base::Json plan;
            bool              readPlaceholder = false;
            if (!planFromToolCall(argsStr, std::string_view{}, plan, readPlaceholder)) {
                return;
            }
            ctx->pending_args[callId] = argsStr;
            pushToolDecor(*ctx, callId, readPlaceholder ? makeReadingPlaceholderPlan() : plan);
            return;
        }

        // tool_end: 以缓存的最终参数重建 (write 完整内容; read 展示已保存规划)
        auto it = ctx->pending_args.find(callId);
        if (it != ctx->pending_args.end()) {
            utilxx_base::Json plan;
            bool              readPlaceholder = false;
            if (planFromToolCall(
                    it->second,
                    d.value("result", std::string{}),
                    plan,
                    readPlaceholder
                )) {
                // read 结果非法时保持 tool_start 的占位装饰 (不覆盖为空内容)
                if (!readPlaceholder) {
                    pushToolDecor(*ctx, callId, plan);
                }
            }
            ctx->pending_args.erase(it);
        }
    });
}

/// EVT_SESSION_SWITCH: 会话切换 → 清理装饰与段落 (新会话尚未有规划)
static void PLUGINXX_CALL
    on_client_session_switch(const PluginxxStringView* payload_json, void* ud) {
    (void)payload_json;
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    agentxx::plugin::guardCallVoid(clientGuardLogger(ctxRaw), [&] {
        auto* ctx = static_cast<ClientCtx*>(ud);
        if (!ctx) {
            return;
        }
        clearSection(*ctx);
        clearToolDecors(*ctx);
    });
}

extern "C" PLUGINXX_EXPORT const AgentxxClientPluginInfo* PLUGINXX_CALL
    agentxx_plugin_client_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL; 本边界为纯静态元数据 → 空操作日志
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxClientPluginInfo* {
            static const AgentxxClientPluginInfo info{
                AGENTXX_CLIENT_PLUGIN_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("agentxx_planning"),
                agentxx::plugin::PluginStringView::fromCstr("1.2.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Plan rendering driven entirely by plugin: tool renderer (live + restored "
                    "history) + message decor + sidebar overview"
                ),
            };
            return &info;
        }
    );
}

/// client 侧注册事务 (start 的实际内容): 动作绑定 + 工具渲染器 + client 事件订阅。
static int planningClientSetup(ClientCtx* ctx) {
    const PluginxxHost* host = ctx->host;

    // 类型级工具渲染器 (按 tool_name 注册): 由工具参数/结果直接推导折叠头与
    // 展开体, **重启恢复/重连/切换会话后由 Sync 回放的历史工具消息同样特化
    // 渲染** (这些消息没有 EVT_DELTA 装饰推送); 实时调用也走同一渲染器, 与
    // 装饰推送 (展开头显示语义名) 内容同源。宿主无消息渲染面 (cli) 或未装配
    // ui 表时注册失败 → 仅记日志, 不影响其他功能
    if (ctx->ui && ctx->ui->register_tool_renderer) {
        const int32_t rc = agentxx::plugin::registerToolRenderer(
            host,
            ctx->ui,
            kNamePlanning,
            &buildPlanningToolRender,
            ctx->renderShims
        );
        if (rc != 0) {
            if (ctx->iface.log && ctx->iface.log->log) {
                auto warnSv = agentxx::plugin::PluginStringView::fromCstr(
                    "agentxx_planning client: register tool renderer failed "
                    "(restored history keeps generic tool body)"
                );
                ctx->iface.log->log(host, 3, &warnSv);
            }
        }
    }
    // 通用交互绑定 (方案 A fallback: target_id="" 本实例兜底, 一次永久生效):
    // - Info 段 / decor 按钮均以 action_id="planning.open_graph" 声明,
    //   owner_id 由宿主组装 (section_id / tool_call_id), 此处无需逐个 bind
    // - ui 缺失或 bind 为 NULL (CLI/老宿主) 时静默降级: 内容仍推送, 按钮不可点
    if (ctx->ui && ctx->ui->bind_action_handler) {
        ctx->actions.on(kActionOpenGraph, [ctxPtr = ctx](const utilxx_base::Json&) {
            auto* c = ctxPtr;
            if (!c || !c->host || !c->ui || !c->ui->open_overlay || c->last_plan_json.empty()) {
                return;
            }
            std::string roadmap;
            try {
                roadmap
                    = utilxx_base::Json::parse(c->last_plan_json).value("roadmap", std::string{});
            } catch (...) {
                return;
            }
            if (roadmap.empty()) {
                return;
            }
            AgentxxOverlaySpec spec{};
            spec.version = 1;
            spec.type    = AGENTXX_OVERLAY_MERMAID;
            spec.title   = agentxx::plugin::PluginStringView::fromCstr("Planning Roadmap");
            spec.payload = agentxx::plugin::PluginStringView::from(roadmap.data(), roadmap.size());
            spec.extra_json = agentxx::plugin::PluginStringView::fromCstr("{}");
            c->ui->open_overlay(c->host, &spec);
        });
        auto emptySv = agentxx::plugin::PluginStringView::from("", 0);
        if (ctx->ui->bind_action_handler(
                host,
                &emptySv,
                &agentxx::plugin::kit::ActionController::dispatch,
                &ctx->actions
            )
            != 0) {
            auto warnSv = agentxx::plugin::PluginStringView::fromCstr(
                "agentxx_planning client: bind open_graph failed (buttons static)"
            );
            if (ctx->iface.log && ctx->iface.log->log) {
                ctx->iface.log->log(host, 3, &warnSv);
            }
        }
    }

    // 事件订阅 (卸载时宿主自动退订); events/ui 缺失时仅失去渲染能力,
    // 不阻塞加载 (CLI 等精简宿主场景)
    auto subWarn = [ctxPtr = ctx](const char* what) {
        if (ctxPtr && ctxPtr->host && ctxPtr->iface.log && ctxPtr->iface.log->log) {
            auto sv = agentxx::plugin::PluginStringView::from(what, std::strlen(what));
            ctxPtr->iface.log->log(ctxPtr->host, 3, &sv);
        }
    };
    if (ctx->iface.events && ctx->iface.events->subscribe) {
        if (!ctx->iface.events->subscribe(host, AGENTXX_CLIENT_EVT_DELTA, on_client_delta, ctx)) {
            subWarn("agentxx_planning client: subscribe DELTA failed");
        }
        if (!ctx->iface.events
                 ->subscribe(host, AGENTXX_CLIENT_EVT_PLUGIN_DATA, on_client_plugin_data, ctx)) {
            subWarn("agentxx_planning client: subscribe PLUGIN_DATA failed");
        }
        if (!ctx->iface.events->subscribe(
                host,
                AGENTXX_CLIENT_EVT_SESSION_SWITCH,
                on_client_session_switch,
                ctx
            )) {
            subWarn("agentxx_planning client: subscribe SESSION_SWITCH failed");
        }
    }

    if (ctx->iface.log && ctx->iface.log->log) {
        auto loadedSv
            = agentxx::plugin::PluginStringView::fromCstr("agentxx_planning client plugin loaded");
        ctx->iface.log->log(host, 2, &loadedSv);
    }
    return 0;
}

extern "C" PLUGINXX_EXPORT int32_t PLUGINXX_CALL
    agentxx_plugin_client_create(const PluginxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: 异常返回 -1 (加载失败); 日志闭包捕获局部裸指针
    ClientCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            clientGuardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx   = std::make_unique<ClientCtx>();
            ctx->host  = host;
            ctx->iface = agentxx::plugin::ClientIfaces::query(host);
            ctx->ui    = ctx->iface.ui;
            raw        = ctx.get();

            *plugin_ctx = ctx.release(); ///< 所有权移交宿主 (destroy 时取回归还)
            return 0;
        }
    );
}

extern "C" PLUGINXX_EXPORT void* PLUGINXX_CALL agentxx_plugin_client_start(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        static_cast<void*>(nullptr),
        [&]() -> void* {
            auto* ctx = static_cast<ClientCtx*>(plugin_ctx);
            if (!ctx) {
                agentxx::plugin::PluginString::set(
                    nullptr,
                    error_out,
                    "planning client start: null context"
                );
                return nullptr;
            }
            if (!notify) {
                agentxx::plugin::PluginString::set(
                    ctx->host,
                    error_out,
                    "agentxx_planning client start: notify required"
                );
                return nullptr;
            }
            if (planningClientSetup(ctx) != 0) {
                agentxx::plugin::PluginString::set(
                    ctx->host,
                    error_out,
                    "agentxx_planning client start: registration failed"
                );
                return nullptr;
            }
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
            return nullptr;
        }
    );
}

extern "C" PLUGINXX_EXPORT void* PLUGINXX_CALL
    agentxx_plugin_client_stop(void*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        static_cast<void*>(nullptr),
        [&]() -> void* {
            // UI 注册与订阅由宿主在 stop 后统一撤销。
            if (notify && notify->done) {
                notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
            }
            return nullptr;
        }
    );
}

extern "C" PLUGINXX_EXPORT void PLUGINXX_CALL agentxx_plugin_client_destroy(void* plugin_ctx) {
    // C ABI 边界异常守卫: 销毁回调异常不得外泄
    auto* ctx = static_cast<ClientCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(clientGuardLogger(ctx), [&] {
        if (!ctx || !ctx->host) {
            delete ctx;
            return;
        }
        // 主动反注册段落 (装饰由宿主卸载路径自动摘除; 成员判空遵循扩展表契约)
        clearSection(*ctx);
        if (ctx->iface.log && ctx->iface.log->log) {
            auto unloadedSv = agentxx::plugin::PluginStringView::fromCstr(
                "agentxx_planning client plugin unloaded"
            );
            ctx->iface.log->log(ctx->host, 2, &unloadedSv);
        }
        delete ctx;
    });
}
