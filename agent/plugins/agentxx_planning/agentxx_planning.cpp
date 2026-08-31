// agentxx_planning —— 两层任务规划工具插件 (双端: agent 工具 + client 渲染)
// - 从 libagentxx src/tools/planning 拆分独立:
//   - agent 侧工具 agentxx_planning (原 agentxx_planning_write 改名):
//     mode=write 写入会话规划 state (经通用接口: 持久化到
//     {dataDir}/plans + 发布 planning 事件, 不再依赖专用 planning 接口表) 并持久化到
//     {dataDir}/plans/{thread_id}.json (供 read 模式跨轮次读取);
//     mode=read 返回本会话此前保存的规划内容
//   - 规划写入成功后发布 "agentxx_planning.planning" 插件事件 (载荷为完整
//     规划 JSON); 订阅宿主约定事件 agentxx_host.client_attached, 客户端接入/
//     重连时重发当前会话已保存规划 (状态快照自愈, 见 docs/agent/plugins.md 7.3.1)
//   - client 侧入口 (agentxx_plugin_client_create): Plan 渲染完全由插件驱动 ——
//     ① 工具消息装饰: 订阅 EVT_DELTA 经 update_tool_decor 推送语义层装饰
//     (折叠头显示名/摘要 + 展开体 items: 状态图/todos/notes), TUI 按通用
//     渲染器展示, 无任何 plan 特化代码; ② Info 栏段落渲染最近一次规划概览
#include "agentxx_planning_plugin.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fmt/ranges.h>
#include <fstream>
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
Each item records execution details, lessons learned, and issues encountered
to help with re-planning.

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
Start the final message with the substantive content the user asked for (data, computation, or analysis).
The user wants the result, not confirmation that the work is done.
)_";

/* ==================== 规划持久化 ({dataDir}/plans/) ====================
 * - 文件名: thread_id 经字符清洗 (非 [A-Za-z0-9._-] → '_') 截断后追加
 *   FNV1a-32 哈希后缀, 确定且无碰撞歧义 (同 thread_id 恒定同名)
 * - 写入原子化: 先写 .tmp 再 rename (Windows 下 rename 不覆盖已有目标,
 *   先 remove 旧文件; 极端崩溃窗口最多丢失一次更新, 可接受)
 */

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
    char* j = ctx.iface.config->get_config(ctx.host);
    if (!j) {
        return {};
    }
    std::string s{j};
    ctx.host->vtable->free(j);
    try {
        auto o = neograph::json::parse(s);
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

/* ==================== 插件事件发布 ====================
 * 规划内容变化 → 发布 "agentxx_planning.planning" (载荷为完整规划 JSON)。
 * server 侧经事件总线转发为 WirePluginData{plugin, event, data}, client 侧
 * 插件订阅 EVT_PLUGIN_DATA 消费并渲染 Info 栏段落 (见下方 client 入口)。
 */
void publishPlanningEvent(PluginCtx& ctx, const std::string& planJson) {
    if (!ctx.iface.events || !ctx.iface.events->publish || planJson.empty()) {
        return;
    }
    ctx.iface.events->publish(
        ctx.host,
        agentxx_plugin_sv_cstr("agentxx_planning.planning"),
        agentxx_plugin_sv(planJson.data(), planJson.size())
    );
}

/// 宿主约定事件 client_attached: 客户端接入/重连 → 重发当前会话已保存规划
/// (修复 "事件先于客户端订阅而丢失 → UI 永久空白", 见 plugins.md 7.3.1)
void on_client_attached(AgentxxPluginStringView event_json, void* ud) {
    auto* ctxRaw = static_cast<PluginCtx*>(ud);
    // C ABI 回调异常守卫 (agent io 线程派发直调)
    agentxx::plugin_guard::guardCallVoid(
        [ctxRaw](const char* msg) noexcept {
            pluginLog(ctxRaw, 4, msg ? msg : "");
        },
        [&] {
            auto* ctx = static_cast<PluginCtx*>(ud);
            if (!ctx || !ctx->host || agentxx_plugin_sv_empty(event_json)) {
                return;
            }
            std::string sessionId;
            try {
                sessionId = neograph::json::parse(std::string{event_json.data, event_json.size})
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

/* =====================================================================
 * agent 侧入口
 * ===================================================================== */

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理);
    // 本边界为纯静态元数据, 无实例上下文可捕获 → 空操作日志闭包
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                agentxx_plugin_sv_cstr("agentxx_planning"),
                agentxx_plugin_sv_cstr("1.1.0"),
                agentxx_plugin_sv_cstr(
                    "Two-level task planning tool (write/read modes) + client-side Plan rendering"
                ),
            };
            return &info;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: create 内含 JSON schema 构建等可抛操作, 异常返回 -1;
    // 守卫日志闭包捕获局部裸指针 (ctx 装配前置空 → 异常路径静默丢弃)
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
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

            // 注入 systemPlanningPrompt 至宿主提示词 (替代已移除的 PlanningMiddleware)
            if (ctx->iface.prompt && ctx->iface.prompt->set_prompt) {
                neograph::json j;
                j["systemPlanningPrompt"] = kSystemPlanningPrompt;
                std::string js            = j.dump();
                if (ctx->iface.prompt->set_prompt(host, agentxx_plugin_sv(js.data(), js.size()))
                    != 0) {
                    pluginLog(ctx.get(), 3, "agentxx_planning: set systemPlanningPrompt failed");
                } else {
                    pluginLog(
                        ctx.get(),
                        2,
                        "agentxx_planning: systemPlanningPrompt injected via prompt iface"
                    );
                }
            }

            // 规划持久化 + 事件发布为通用接口 (不再依赖专用 planning iface)

            {
                agentxx::kit::ToolPromptText p      = ctx->toolPrompt(kNamePlanning);
                std::string                  depict = p.depict;
                if (depict.empty()) {
                    depict = kDepictPlanning;
                }
                std::string schema = neograph::json{
                {"type", "object"},
                {"required", neograph::json::array({"mode"})},
                {"properties",
                 {
                     {"mode",
                      {
                          {"type", "string"},
                          {"enum", neograph::json::array({"write", "read"})},
                          {"description",
                           agentxx::kit::toolPromptArgDesc(p,
                                   "mode",
                                   "Operation mode: `write` saves/updates the planning content "
                                   "(requires `roadmap`); `read` returns the previously saved "
                                   "planning content of this session.")},
                      }},
                     {"roadmap",
                      {
                          {"type", "string"},
                          {"description",
                           agentxx::kit::toolPromptArgDesc(p,
                                   "roadmap",
                                   "(write only, required) STRATEGIC LAYER: Mermaid stateDiagram-v2 of the overall workflow.")},
                      }},
                     {"todos",
                      {
                          {"type", "array"},
                          {"items", neograph::json{{"type", "object"}}},
                          {"description",
                           agentxx::kit::toolPromptArgDesc(p,
                                   "todos",
                                   "(write only) TACTICAL LAYER: Near-term task items (state/content).")},
                      }},
                     {"notes",
                      {
                          {"type", "string"},
                          {"description",
                           agentxx::kit::toolPromptArgDesc(p,
                                   "notes",
                                   "(write only) MEMO LAYER: Any additional notes, tips, reminders.")},
                      }},
                 }},
            }
                                  .dump();

                agentxx::kit::fast_tool(
                    *ctx,
                    kNamePlanning,
                    depict,
                    schema,
                    [](PluginCtx& c, std::string_view args_json, std::string_view thread_id
                    ) -> std::string {
                        std::string argsStr(
                            args_json.data() ? args_json.data() : "",
                            args_json.size()
                        );
                        auto arguments = argsStr.empty() ? neograph::json::object()
                                                         : neograph::json::parse(argsStr);

                        const auto mode = arguments.value("mode", std::string{});
                        if (mode != "write" && mode != "read") {
                            return R"({"error":"Arg `mode` must be \"write\" or \"read\""})";
                        }

                        if (mode == "read") {
                            const std::string tid{
                                thread_id.data() ? thread_id.data() : "",
                                thread_id.size()
                            };
                            auto saved = loadPlanningFile(c, tid);
                            if (saved.empty()) {
                                return R"({"error":"No saved planning in this session. Call with mode=\"write\" first."})";
                            }
                            try {
                                auto v = neograph::json::parse(saved);
                                return v.dump(2);
                            } catch (...) {
                                return R"({"error":"Saved planning is corrupted. Rewrite it with mode=\"write\"."})";
                            }
                        }

                        auto roadmap = arguments.value("roadmap", std::string{});
                        if (roadmap.empty()) {
                            return R"({"error":"Arg `roadmap` is empty, must provide a stateDiagram-v2 planning string in write mode"})";
                        }

                        std::string todosJson;
                        if (arguments.contains("todos") && arguments["todos"].is_array()) {
                            todosJson = arguments["todos"].dump();
                        }
                        std::string notes = arguments.value("notes", std::string{});

                        neograph::json planStore = neograph::json::object();
                        planStore["roadmap"]     = roadmap;
                        if (!todosJson.empty()) {
                            try {
                                planStore["todos"] = neograph::json::parse(todosJson);
                            } catch (...) {
                                return R"({"error":"Arg `todos` is not valid JSON"})";
                            }
                        }
                        if (!notes.empty()) {
                            planStore["notes"] = notes;
                        }
                        std::string planJson = planStore.dump();

                        const std::string tid{
                            thread_id.data() ? thread_id.data() : "",
                            thread_id.size()
                        };
                        if (!savePlanningFile(c, tid, planJson)) {
                            pluginLog(
                                &c,
                                3,
                                fmt::format("agentxx_planning: persist planning failed tid={}", tid)
                            );
                        }

                        // 通用接口: 持久化到 {dataDir}/plans + 事件发布 (宿主/客户端通用消费)
                        publishPlanningEvent(c, planJson);

                        return "success";
                    }
                );
            }

            // 宿主约定事件 client_attached 订阅: 客户端接入/重连时重发当前会话快照
            if (ctx->iface.events && ctx->iface.events->subscribe) {
                if (!ctx->iface.events->subscribe(
                        host,
                        agentxx_plugin_sv_cstr("agentxx_host.client_attached"),
                        on_client_attached,
                        ctx.get()
                    )) {
                    pluginLog(
                        ctx.get(),
                        3,
                        "agentxx_planning: subscribe client_attached failed (UI resnapshot disabled)"
                    );
                }
            }

            *plugin_ctx = ctx.release(); ///< 所有权移交宿主 (destroy 时取回归还)
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    // C ABI 边界异常守卫: 销毁回调异常不得外泄
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(ctxGuardLogger(ctx), [&] {
        delete ctx;
    });
}

/* =====================================================================
 * client 侧入口 (agentxx_plugin_client_create) —— Plan 渲染 (原 TUI 硬编码的
 * 消息列表特化 + Info 侧边栏段落全部拆分至本插件, TUI 无任何 plan 概念)
 *
 * - 工具消息装饰 (update_tool_decor): 订阅 EVT_DELTA, tool_start 时按
 *   arguments 构建装饰 (折叠头 displayName + 展开体 items:
 *   状态图/todos/notes), tool_end 时以最终内容刷新
 * - Info 栏段落 "Plan" (懒注册): EVT_PLUGIN_DATA planning 事件驱动,
 *   展示最近一次规划概览; client_attached 重发快照自愈
 * - EVT_SESSION_SWITCH → 清理装饰缓存与段落
 * ===================================================================== */

/// client 侧每实例上下文 (多实例契约: 状态挂本实例, 回调经 ud 恢复)
struct ClientCtx {
    const AgentxxClientHost*      host = nullptr;
    agentxx::plugin::ClientIfaces iface{};
    /// "agentxx.client.ui" 展示接口表 (Info 段落/工具装饰; CLI 等不支持时成员 NULL 降级)
    const AgentxxClientUiIface* ui      = nullptr;
    AgentxxInfoSection*         section = nullptr; ///< 懒注册句柄 (null=未展示)
    std::string                 last_plan_json;    ///< 最近一次规划内容缓存 (Info 段落)
    /// 进行中 planning 工具调用的参数缓存 (tool_call_id → arguments JSON;
    /// tool_start 缓存, tool_end 刷新最终装饰后摘除)
    std::map<std::string, std::string> pending_args;
};

/// client 侧守卫日志闭包工厂
static auto clientGuardLogger(ClientCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
            ctx->iface.log->log(ctx->host, 4, agentxx_plugin_sv(msg, std::strlen(msg)));
        }
    };
}

/// 字符串 → JSON 字符串字面量 (经宿主 agentxx.client.json 接口表; 结果含引号)
static std::string clientJsonEscape(const ClientCtx& ctx, const std::string& s) {
    if (!ctx.host || !ctx.iface.json || !ctx.iface.json->json_escape || s.empty()) {
        return "\"\"";
    }
    char* esc = ctx.iface.json->json_escape(ctx.host, agentxx_plugin_sv(s.data(), s.size()));
    if (!esc) {
        return "\"\"";
    }
    std::string out{esc};
    ctx.host->vtable->free(esc);
    return out;
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
static std::string buildTodosSummary(const neograph::json& plan) {
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

/// 组装展开体 items JSON 数组元素 (Graph / Todo / Note 三段式, 参考剥离前的
/// TUI appendPlanToolBody + Info 侧边栏 Plan 渲染; Graph 为按钮弹窗)
static std::string buildDecorItems(const ClientCtx& ctx, const neograph::json& plan) {
    if (plan.contains("items") && plan["items"].is_array()) {
        return plan["items"].dump();
    }
    std::vector<std::string> items;
    auto                     textItem = [&](const std::string& text, const std::string& role) {
        items.push_back(fmt::format(
            R"({{"kind":"text","role":{},"text":{}}})",
            clientJsonEscape(ctx, role),
            clientJsonEscape(ctx, text)
        ));
    };
    auto buttonItem = [&](const std::string& label, const std::string& mermaid) {
        // Graph 按钮: 点击弹窗渲染状态图 (mermaid 源完整透传)
        items.push_back(fmt::format(
            R"({{"kind":"button","label":{},"mermaid":{}}})",
            clientJsonEscape(ctx, label),
            clientJsonEscape(ctx, mermaid)
        ));
    };

    // ---- Graph: 状态图 (按钮，点击弹窗) ----
    const auto roadmap = plan.value("roadmap", std::string{});
    if (!roadmap.empty()) {
        textItem("State Diagram:", "title");
        buttonItem(" Graph ", roadmap);
        // 兼容旧 TUI: 保留 inline diagram 供不支持 button 的客户端回退渲染；
        // 新 TUI 优先将 button 渲染为可点击弹窗，旧测试仍可通过 diagram 断言。
        items.push_back(
            fmt::format(R"({{"kind":"diagram","mermaid":{}}})", clientJsonEscape(ctx, roadmap))
        );
    }

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

    return fmt::format(R"([{}])", fmt::join(items, ","));
}

/// 推送/更新工具消息装饰 (client io 线程; ui 成员判空降级)
static void
    pushToolDecor(ClientCtx& ctx, const std::string& toolCallId, const neograph::json& plan) {
    if (!ctx.ui || !ctx.ui->update_tool_decor || !ctx.host || toolCallId.empty()) {
        return;
    }
    const std::string decorJson = fmt::format(
        R"({{"displayName":{},"summary":{},"items":{}}})",
        clientJsonEscape(ctx, kDisplayName),
        clientJsonEscape(ctx, buildTodosSummary(plan)),
        buildDecorItems(ctx, plan)
    );
    ctx.ui->update_tool_decor(
        ctx.host,
        agentxx_plugin_sv(toolCallId.data(), toolCallId.size()),
        agentxx_plugin_sv(decorJson.data(), decorJson.size())
    );
}

/// 清理本插件全部装饰 (会话切换时调用)
static void clearToolDecors(ClientCtx& ctx) {
    ctx.pending_args.clear();
    if (!ctx.ui || !ctx.ui->update_tool_decor || !ctx.host) {
        return;
    }
    ctx.ui->update_tool_decor(ctx.host, AgentxxPluginStringView{}, AgentxxPluginStringView{});
}

/// 懒注册 Info 栏段落 (宿主不支持 info_section 时保持 NULL 静默降级)
static void ensureSection(ClientCtx& ctx) {
    if (ctx.section || !ctx.ui || !ctx.ui->register_info_section || !ctx.host) {
        return;
    }
    ctx.section = ctx.ui->register_info_section(
        ctx.host,
        agentxx_plugin_sv_cstr(kSectionId),
        agentxx_plugin_sv_cstr(R"({"title":"Plan"})")
    );
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
/// 三段式: Graph(按钮弹窗) / Todo / Note — 参考剥离前的 TUI renderPlanningInfo
/// (Plan 标题 + Graph 按钮 + todos 列表 + notes 段)，经通用 items 表达:
/// - Graph: title + button{label,mermaid} + steps hint
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
    neograph::json plan;
    try {
        plan = neograph::json::parse(ctx.last_plan_json);
    } catch (...) {
        return;
    }
    if (!plan.is_object()) {
        return;
    }

    std::vector<std::string> items;
    auto                     textItem = [&](const std::string& text, const std::string& role) {
        items.push_back(fmt::format(
            R"({{"kind":"text","role":{},"text":{}}})",
            clientJsonEscape(ctx, role),
            clientJsonEscape(ctx, text)
        ));
    };
    auto buttonItem = [&](const std::string& label, const std::string& mermaid) {
        items.push_back(fmt::format(
            R"({{"kind":"button","label":{},"mermaid":{}}})",
            clientJsonEscape(ctx, label),
            clientJsonEscape(ctx, mermaid)
        ));
    };

    // ---- Graph: 状态图按钮 + 概要 ----
    const auto roadmap = plan.value("roadmap", std::string{});
    if (!roadmap.empty()) {
        textItem("|- Graph", "normal");
        buttonItem(" Graph ", roadmap);
    }

    // ---- Todo: 待办列表 (独立分区) ----
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

    // ---- Note: 备忘 (独立分区, 与 Todo 分隔) ----
    if (plan.contains("notes")) {
        const auto& nv = plan["notes"];
        if (nv.is_string()) {
            textItem("|- Note", "normal");
            textItem(nv.get<std::string>(), "hint");
        }
    }

    if (items.empty()) {
        return; // 内容为空不推送, 避免出现只有标题的空段落
    }
    const std::string json = fmt::format(R"({{"items":[{}]}})", fmt::join(items, ","));
    ctx.ui->update_info_section(ctx.host, ctx.section, agentxx_plugin_sv(json.data(), json.size()));
}

/// EVT_PLUGIN_DATA: 过滤本插件规划事件 {plugin:"agentxx_planning", event:"planning"}
/// → 更新 Info 栏段落 (最近一次规划概览)
static void on_client_plugin_data(AgentxxPluginStringView payload_json, void* ud) {
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    // C ABI 回调异常守卫 (client io 线程派发直调)
    agentxx::plugin_guard::guardCallVoid(clientGuardLogger(ctxRaw), [&] {
        auto* ctx = static_cast<ClientCtx*>(ud);
        if (!ctx || !ctx->host) {
            return;
        }
        char*      plugin = ctx->iface.json && ctx->iface.json->json_get_string
                                ? ctx->iface.json->json_get_string(
                                 ctx->host,
                                 payload_json,
                                 agentxx_plugin_sv_cstr("plugin")
                             )
                                : nullptr;
        char*      event  = ctx->iface.json && ctx->iface.json->json_get_string
                                ? ctx->iface.json->json_get_string(
                                ctx->host,
                                payload_json,
                                agentxx_plugin_sv_cstr("event")
                            )
                                : nullptr;
        char*      data   = ctx->iface.json && ctx->iface.json->json_get_string
                                ? ctx->iface.json->json_get_string(
                               ctx->host,
                               payload_json,
                               agentxx_plugin_sv_cstr("data")
                           )
                                : nullptr;
        const bool mine   = plugin && event && data && std::strcmp(plugin, "agentxx_planning") == 0
                          && std::strcmp(event, "planning") == 0;
        if (mine) {
            ctx->last_plan_json = data;
            refreshPlanSection(*ctx);
        }
        if (plugin) {
            ctx->host->vtable->free(plugin);
        }
        if (event) {
            ctx->host->vtable->free(event);
        }
        if (data) {
            ctx->host->vtable->free(data);
        }
    });
}

/// EVT_DELTA: planning 工具生命周期 → 工具消息装饰
/// - tool_start: 缓存参数; write 推送完整装饰 (折叠头 Plan · todos 摘要 +
///   展开体 状态图/todos/notes); read 推送占位 (结果未返回)
/// - tool_end: 以缓存的最终参数重建装饰 (覆盖流式期间的不完整内容);
///   read 模式此时展示结果摘要
static void on_client_delta(AgentxxPluginStringView payload_json, void* ud) {
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    agentxx::plugin_guard::guardCallVoid(clientGuardLogger(ctxRaw), [&] {
        auto* ctx = static_cast<ClientCtx*>(ud);
        if (!ctx || !ctx->host || !ctx->ui || !ctx->ui->update_tool_decor) {
            return;
        }
        if (agentxx_plugin_sv_empty(payload_json)) {
            return;
        }
        const std::string_view raw{payload_json.data, payload_json.size};
        // 快速预过滤: 仅 planning 工具相关 delta 才值得完整解析
        // (deltaToJson 字段序固定 type 在前)
        if (raw.find("\"tool_start\"") == std::string_view::npos
            && raw.find("\"tool_end\"") == std::string_view::npos) {
            return;
        }
        if (raw.find("\"agentxx_planning\"") == std::string_view::npos) {
            return;
        }
        neograph::json d;
        try {
            d = neograph::json::parse(raw);
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
            const auto     argsStr = d.value("arguments", std::string{});
            neograph::json args;
            try {
                args = argsStr.empty() ? neograph::json::object() : neograph::json::parse(argsStr);
            } catch (...) {
                return;
            }
            if (!args.is_object()) {
                return;
            }
            ctx->pending_args[callId] = args.dump();
            if (args.value("mode", std::string{}) == "read") {
                // read: 结果尚未返回, 占位提示 (tool_end 时替换为结果摘要)
                neograph::json placeholder = neograph::json::object();
                placeholder["items"]       = neograph::json::array();
                neograph::json hint        = neograph::json::object();
                hint["kind"]               = "text";
                hint["role"]               = "hint";
                hint["text"]               = "Reading saved planning...";
                placeholder["items"].push_back(hint);
                pushToolDecor(*ctx, callId, placeholder);
                return;
            }
            pushToolDecor(*ctx, callId, args);
            return;
        }

        // tool_end: 以缓存的最终参数重建 (write 完整内容; read 展示已保存规划)
        auto it = ctx->pending_args.find(callId);
        if (it != ctx->pending_args.end()) {
            try {
                auto args = neograph::json::parse(it->second);
                if (args.value("mode", std::string{}) == "read") {
                    // read 结果即保存的规划 JSON (见 agent 侧 execute)
                    const auto result = d.value("result", std::string{});
                    if (!result.empty()) {
                        try {
                            pushToolDecor(*ctx, callId, neograph::json::parse(result));
                        } catch (...) {
                            // 非法结果保持占位装饰
                        }
                    }
                } else {
                    pushToolDecor(*ctx, callId, args);
                }
            } catch (...) {
            }
            ctx->pending_args.erase(it);
        }
    });
}

/// EVT_SESSION_SWITCH: 会话切换 → 清理装饰与段落 (新会话尚未有规划)
static void on_client_session_switch(AgentxxPluginStringView payload_json, void* ud) {
    (void)payload_json;
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    agentxx::plugin_guard::guardCallVoid(clientGuardLogger(ctxRaw), [&] {
        auto* ctx = static_cast<ClientCtx*>(ud);
        if (!ctx) {
            return;
        }
        clearSection(*ctx);
        clearToolDecors(*ctx);
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo* agentxx_plugin_client_get_info(void
) {
    // C ABI 边界异常守卫: 异常返回 NULL; 本边界为纯静态元数据 → 空操作日志
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxClientPluginInfo* {
            static const AgentxxClientPluginInfo info{
                AGENTXX_CLIENT_PLUGIN_API_VERSION,
                agentxx_plugin_sv_cstr("agentxx_planning"),
                agentxx_plugin_sv_cstr("1.1.0"),
                agentxx_plugin_sv_cstr(
                    "Plan rendering driven entirely by plugin: message decor + sidebar overview"
                ),
            };
            return &info;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_client_create(const AgentxxClientHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: 异常返回 -1 (加载失败); 日志闭包捕获局部裸指针
    ClientCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
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

            // 事件订阅 (卸载时宿主自动退订); events/ui 缺失时仅失去渲染能力,
            // 不阻塞加载 (CLI 等精简宿主场景)
            auto subWarn = [ctxPtr = ctx.get()](const char* what) {
                if (ctxPtr && ctxPtr->host && ctxPtr->iface.log && ctxPtr->iface.log->log) {
                    ctxPtr->iface.log
                        ->log(ctxPtr->host, 3, agentxx_plugin_sv(what, std::strlen(what)));
                }
            };
            if (ctx->iface.events && ctx->iface.events->subscribe) {
                if (!ctx->iface.events
                         ->subscribe(host, AGENTXX_CLIENT_EVT_DELTA, on_client_delta, ctx.get())) {
                    subWarn("agentxx_planning client: subscribe DELTA failed");
                }
                if (!ctx->iface.events->subscribe(
                        host,
                        AGENTXX_CLIENT_EVT_PLUGIN_DATA,
                        on_client_plugin_data,
                        ctx.get()
                    )) {
                    subWarn("agentxx_planning client: subscribe PLUGIN_DATA failed");
                }
                if (!ctx->iface.events->subscribe(
                        host,
                        AGENTXX_CLIENT_EVT_SESSION_SWITCH,
                        on_client_session_switch,
                        ctx.get()
                    )) {
                    subWarn("agentxx_planning client: subscribe SESSION_SWITCH failed");
                }
            }

            if (ctx->iface.log && ctx->iface.log->log) {
                ctx->iface.log
                    ->log(host, 2, agentxx_plugin_sv_cstr("agentxx_planning client plugin loaded"));
            }
            *plugin_ctx = ctx.release(); ///< 所有权移交宿主 (destroy 时取回归还)
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_client_destroy(void* plugin_ctx) {
    // C ABI 边界异常守卫: 销毁回调异常不得外泄
    auto* ctx = static_cast<ClientCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(clientGuardLogger(ctx), [&] {
        if (!ctx || !ctx->host) {
            delete ctx;
            return;
        }
        // 主动反注册段落 (装饰由宿主卸载路径自动摘除; 成员判空遵循扩展表契约)
        clearSection(*ctx);
        if (ctx->iface.log && ctx->iface.log->log) {
            ctx->iface.log->log(
                ctx->host,
                2,
                agentxx_plugin_sv_cstr("agentxx_planning client plugin unloaded")
            );
        }
        delete ctx;
    });
}
