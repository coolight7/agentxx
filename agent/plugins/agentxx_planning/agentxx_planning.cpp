// agentxx_planning —— 两层任务规划工具插件
// - 从 libagentxx src/tools/planning 拆分独立 (同名同行为):
//   - agentxx_planning_write (原 WritePlanningTool)
// - 规划 state 写入经宿主 agentxx.agent.planning 接口表 (原实现直接写
//   PlanningMiddlewareHandle 的 plannings map, 行为一致);
//   thread_id 取 execute 回调入参 (宿主 ToolcallNode 注入的 sessionId)
#include "agentxx_planning_plugin.h"
#include <cstring>
#include <string>
#include <vector>

using namespace agentxx_planning_plugin;

namespace {

constexpr auto kNamePlanning = "agentxx_planning_write";

constexpr auto kDepictPlanning =
    R"(Two-level task planning tool for complex multi-step work sessions.

=== Strategic Layer: `roadmap` (required) ===
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

=== Tactical Layer: `todos` (optional) ===
A short list of IMMEDIATE and NEXT-STEP tasks only. Do NOT list every state
from the diagram — only the tasks you are actively working on or about to start.
Each item records execution details, lessons learned, and issues encountered
to help with re-planning.

=== MEMO Layer: `notes` (optional) ===
Record any important information, tips, reminders, or identity/role-playing prompts.

Example for a "fix a bug" workflow:
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
  {"state":"in_progress", "content":"Reproduce the crash with provided stack trace",
   "summary":"Found that it crashes on null pointer at line 342"},
  {"state":"pending", "content":"Locate root cause by tracing the null pointer source"}
]
- notes:
    - Follow user code style guide.
    - Add unit tests after change.
)";

std::string argDesc(const ToolPromptText& p, const char* key, const char* fallback) {
    auto it = p.args.find(key);
    if (it != p.args.end() && !it->second.empty()) {
        return it->second;
    }
    return fallback;
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理);
    // 本边界为纯静态元数据, 无实例上下文可捕获 → 空操作日志闭包
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
        static const AgentxxPluginInfo info{
            AGENTXX_PLUGIN_API_VERSION,
            AGENTXX_SV("agentxx_planning"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV("Two-level task planning tool: roadmap + todo list + memo"),
        };
        return &info;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: create 内含 JSON schema 构建等可抛操作, 异常返回 -1;
    // 守卫日志闭包捕获局部裸指针 (ctx 装配前置空 → 异常路径静默丢弃)
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* msg) noexcept { pluginLog(raw, 4, msg ? msg : ""); },
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) {
            return -1;
        }
        auto ctx   = std::make_unique<PluginCtx>();
        ctx->host  = host;
        ctx->iface = agentxx::plugin::AgentIfaces::query(host);
        raw        = ctx.get();

        if (!ctx->iface.planning || !ctx->iface.planning->set_planning) {
            pluginLog(
                ctx.get(),
                3,
                fmt::format("agentxx_planning: host planning iface unavailable, `{}' will register but fail at runtime",
                            kNamePlanning)
            );
        }

        {
            ToolPromptText p      = readToolPrompt(ctx->host, ctx->iface, kNamePlanning);
            std::string    depict = p.depict;
            if (depict.empty()) {
                depict = kDepictPlanning;
            }
            auto& storage = ctx->storage;
            storage.push_back(std::move(depict));
            // schema 与 lib AgentPrompt 条目一致 (roadmap/todos/notes)
            // schema 与 lib AgentPrompt 条目一致 (roadmap/todos/notes);
            // 经 json 对象构造后序列化 (避免手写转义)
            std::string schema = neograph::json{
                {"type", "object"},
                {"required", neograph::json::array({"roadmap"})},
                {"properties",
                 {
                     {"roadmap",
                      {
                          {"type", "string"},
                          {"description",
                           argDesc(p,
                                   "roadmap",
                                   "STRATEGIC LAYER: Mermaid stateDiagram-v2 of the overall workflow.")},
                      }},
                     {"todos",
                      {
                          {"type", "array"},
                          {"items", neograph::json{{"type", "object"}}},
                          {"description",
                           argDesc(p,
                                   "todos",
                                   "TACTICAL LAYER: Near-term task items (state/content/summary).")},
                      }},
                     {"notes",
                      {
                          {"type", "string"},
                          {"description",
                           argDesc(p,
                                   "notes",
                                   "MEMO LAYER: Any additional notes, tips, reminders.")},
                      }},
                 }},
            }
                                  .dump();
            storage.push_back(std::move(schema));

            // 垫片适配器: 实例内嵌存储 (ctx 持有, 随实例销毁释放; 多实例契约)
            auto shim = std::make_unique<AgentxxInlineToolShim>();

            AgentxxInlineToolSpec spec{};
            spec.name        = agentxx_plugin_sv(kNamePlanning, std::strlen(kNamePlanning));
            spec.description = agentxx_plugin_sv(storage[0].data(), storage[0].size());
            spec.parameters_json
                = agentxx_plugin_sv(storage[1].data(), storage[1].size());
            spec.user_data = ctx.get();
            spec.flags     = AGENTXX_TOOL_FLAG_NONE;
            // 内联完成型: 快 JSON 写入 (set_planning 在 io 线程直接执行, 无跨线程开销)
            spec.execute   = [](void*                   user_data,
                              AgentxxPluginStringView args_json,
                              AgentxxPluginStringView thread_id,
                              AgentxxPluginStringView tool_call_id,
                              char**                  error_out) -> char* {
                auto* ctx = static_cast<PluginCtx*>(user_data);
                (void)tool_call_id;
                try {
                    std::string argsStr(args_json.data ? args_json.data : "", args_json.size);
                    auto arguments
                        = argsStr.empty() ? neograph::json::object() : neograph::json::parse(argsStr);

                    auto roadmap = arguments.value("roadmap", std::string{});
                    if (roadmap.empty()) {
                        return pluginStrdup(
                            ctx ? ctx->host : nullptr,
                            R"({"error":"Arg `roadmap` is empty, must provide a stateDiagram-v2 planning string"})"
                        );
                    }

                    if (!ctx || !ctx->iface.planning || !ctx->iface.planning->set_planning) {
                        return pluginStrdup(ctx ? ctx->host : nullptr,
                                            R"({"error":"planningContext is null"})");
                    }

                    // todos 为数组时序列化透传; notes 为字符串
                    std::string todosJson;
                    if (arguments.contains("todos") && arguments["todos"].is_array()) {
                        todosJson = arguments["todos"].dump();
                    }
                    std::string notes = arguments.value("notes", std::string{});

                    auto rc = ctx->iface.planning->set_planning(
                        ctx->host,
                        thread_id,
                        agentxx_plugin_sv(roadmap.data(), roadmap.size()),
                        agentxx_plugin_sv(todosJson.data(), todosJson.size()),
                        agentxx_plugin_sv(notes.data(), notes.size())
                    );
                    if (rc != 0) {
                        return pluginStrdup(ctx->host,
                                            R"({"error":"planningContext is null"})");
                    }
                    return pluginStrdup(ctx->host, "success");
                } catch (const std::exception& ex) {
                    if (error_out) {
                        *error_out = pluginStrdup(ctx ? ctx->host : nullptr, ex.what());
                    }
                    return nullptr;
                } catch (...) {
                    if (error_out) {
                        *error_out = pluginStrdup(ctx ? ctx->host : nullptr, "unknown exception");
                    }
                    return nullptr;
                }
            };
            if (agentxx_register_inline_tool(host, &spec, shim.get()) != 0) {
                pluginLog(ctx.get(), 3, fmt::format("agentxx_planning: register tool {} failed", kNamePlanning));
            } else {
                ctx->inline_tool_shims.push_back(std::move(shim));
            }
        }

        *plugin_ctx = ctx.release(); ///< 所有权移交宿主 (destroy 时取回归还)
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_destroy(void* plugin_ctx) {
    // C ABI 边界异常守卫: 销毁回调异常不得外泄
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* msg) noexcept { pluginLog(ctx, 4, msg ? msg : ""); },
        [&] { delete ctx; }); // 垫片适配器为 ctx 内嵌存储, 随 ctx 一并释放
}
