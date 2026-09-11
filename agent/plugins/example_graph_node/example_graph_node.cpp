/// example_graph_node —— 执行图插件示例
///
/// 演示能力 (经 graph 接口表 agentxx.agent.graph):
/// 1. 注册自定义节点类型:
///    - example_intent_router (意图识别): 读取上一个 llm 节点输出的意图,
///      按 config 中定义的意图枚举 (intents) 匹配, 写 __route__ channel,
///      由图的 conditional edge (route_channel) 决定路由; 非枚举值取
///      fallback (默认 "normal"); 已检查过的轮次退化为 has_tool_calls 路由
///    - example_datetime (时间输出): 将当前系统日期时间作为 assistant
///      消息写入 messages channel (EventBridge 同步到 viewMessages/
///      llmMessages)
/// 2. 修改执行图 JSON: 将默认图 (agentxx.default) 改为
///    用户输入 → llm (识别意图) → intent_router (路由) → datetime 直接结束
///    轮次 / normal 进入原 agent loop (llm→tool→llm→...→end)
///
/// 节点实现遵循"统一异步操作模型" (两件套 run_start/run_cancel):
/// - run_start 在宿主 io 线程同步调用 (快同步节点: 直接算完 done 返回 NULL)
/// - state_json 为 GraphState::serialize() 结果, 只读; 修改经返回 writes
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "fmt/format.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/// =====================================================================
/// 每实例上下文
/// =====================================================================

struct AgentCtx : public agentxx::plugin::PluginBase {};

static auto agentGuardLogger(AgentCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
            agentxx::plugin::logTo(ctx->host, ctx->iface.log, 4, "example_graph_node", msg);
        }
    };
}

/// ---------------- get_info ----------------

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("example_graph_node"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Example graph node plugin: intent router + datetime node, modifies agent graph"
                ),
            };
            return &info;
        }
    );
}

/// =====================================================================
///  节点实现

/// =====================================================================

namespace {

/// 规范化意图文本: 去首尾空白 + 小写
std::string normalizeIntent(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\'') {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        out.push_back(c);
    }
    return out;
}

/// 从 GraphState::serialize() 结果读取 messages channel 值 (json 数组)
agentxx::util::Json stateMessages(const agentxx::util::Json& state) {
    if (state.is_object() && state.contains("channels") && state["channels"].is_object()
        && state["channels"].contains("messages") && state["channels"]["messages"].is_object()
        && state["channels"]["messages"].contains("value")) {
        return state["channels"]["messages"]["value"];
    }
    return agentxx::util::Json::array();
}

/// 获取最后一条 assistant 消息内容 (无则返回空)
std::string lastAssistantContent(const agentxx::util::Json& messages) {
    if (!messages.is_array()) {
        return {};
    }
    const size_t n = messages.size();
    for (size_t i = n; i > 0; --i) {
        const auto& m = messages[static_cast<int>(i - 1)];
        if (m.is_object() && m.contains("role") && m["role"].is_string()
            && m["role"].get<std::string>() == "assistant") {
            if (m.contains("content") && m["content"].is_string()) {
                return m["content"].get<std::string>();
            }
            return {};
        }
    }
    return {};
}

/// 最后一条 assistant 消息是否含 tool_calls
bool lastAssistantHasToolCalls(const agentxx::util::Json& messages) {
    if (!messages.is_array()) {
        return false;
    }
    const size_t n = messages.size();
    for (size_t i = n; i > 0; --i) {
        const auto& m = messages[static_cast<int>(i - 1)];
        if (m.is_object() && m.contains("role") && m["role"].is_string()
            && m["role"].get<std::string>() == "assistant") {
            return m.contains("tool_calls") && m["tool_calls"].is_array()
                   && !m["tool_calls"].empty();
        }
    }
    return false;
}

} // namespace

/// 意图识别节点执行 (快同步):
/// - 第一次经过: 读最后 assistant 消息内容, 匹配 config intents 枚举 →
///   写 __route__ (命中) / fallback (未命中); 同时标记 __intent_checked=true
/// - 后续轮次: 按 has_tool_calls 语义写 __route__ (tools/end), 保持原
///   agent loop 行为
/// - config: {"intents": ["datetime", "normal"], "fallback": "normal"}
std::string intentRouterRun(AgentCtx& ctx, const agentxx::plugin::RootRequest& req) {
    (void)ctx;
    const std::string_view config_json = req.config();
    auto                   state       = agentxx::util::Json::parse(req.state());
    auto                   messages    = stateMessages(state);

        // 解析 config: intents 枚举 + fallback
        std::vector<std::string> intents;
        std::string              fallback = "normal";
        if (!config_json.empty()) {
            auto cfg = agentxx::util::Json::parse(config_json);
            if (cfg.is_object()) {
                if (cfg.contains("intents") && cfg["intents"].is_array()) {
                    for (const auto& i : cfg["intents"]) {
                        if (i.is_string()) {
                            intents.push_back(i.get<std::string>());
                        }
                    }
                }
                if (cfg.contains("fallback") && cfg["fallback"].is_string()) {
                    fallback = cfg["fallback"].get<std::string>();
                }
            }
        }

        bool intentChecked = false;
        if (state.is_object() && state.contains("channels") && state["channels"].is_object()
            && state["channels"].contains("__intent_checked")
            && state["channels"]["__intent_checked"].is_object()
            && state["channels"]["__intent_checked"].contains("value")
            && state["channels"]["__intent_checked"]["value"].is_boolean()) {
            intentChecked = state["channels"]["__intent_checked"]["value"].get<bool>();
        }

        std::string route;
        if (!intentChecked) {
            // 第一次: 识别意图
            intentChecked      = true;
            const auto content = normalizeIntent(lastAssistantContent(messages));
            route              = fallback;
            for (const auto& i : intents) {
                if (normalizeIntent(i) == content) {
                    route = i;
                    break;
                }
            }
            // 命中意图时移除该纯意图消息 (避免污染后续 agent loop 上下文)
            if (route != fallback && messages.is_array() && !messages.empty()) {
                auto origin          = stateMessages(state);
                messages             = agentxx::util::Json::array();
                const size_t n       = origin.size();
                bool         removed = false;
                for (size_t i = 0; i < n; ++i) {
                    const auto m = origin[static_cast<int>(i)];
                    if (!removed && m.is_object() && m.contains("role") && m["role"].is_string()
                        && m["role"].get<std::string>() == "assistant") {
                        removed = true; // 跳过最后一条 assistant (纯意图输出)
                        continue;
                    }
                    messages.push_back(m);
                }
                if (removed) {
                    // overwrite messages (去掉意图消息)
                    const std::string payload = fmt::format(
                        R"({{"writes":[{{"channel":"__route__","value":{}}},{{"channel":"__intent_checked","value":true}},{{"channel":"messages","value":{},"mode":"overwrite"}}]}})",
                        agentxx::util::Json(route).dump(),
                        messages.dump()
                    );
                    return payload;
                }
            }
            // 未命中/未移除: 仅写路由标记
            const std::string payload = fmt::format(
                R"({{"writes":[{{"channel":"__route__","value":{}}},{{"channel":"__intent_checked","value":true}}]}})",
                agentxx::util::Json(route).dump()
            );
            return payload;
        }

        // 后续轮次: 等价 has_tool_calls 条件
        route                     = lastAssistantHasToolCalls(messages) ? "tools" : "end";
        const std::string payload = fmt::format(
            R"({{"writes":[{{"channel":"__route__","value":{}}}]}})",
            agentxx::util::Json(route).dump()
        );
        return payload;
}

/// 时间输出节点执行 (快同步): 写当前系统日期时间到 messages channel
/// - EventBridge 收到 messages CHANNEL_WRITE 后同步 viewMessages (assistant
///   角色消息) 与 llmMessages, 满足"添加到 viewMessages、llmMessages"
/// - 图配置为执行后直接路由 __end__ 结束轮次
std::string datetimeNodeRun(AgentCtx& ctx, const agentxx::plugin::RootRequest& req) {
    (void)ctx;
    (void)req;
    const auto now  = std::chrono::system_clock::now();
    const auto nowT = std::chrono::system_clock::to_time_t(now);
    const auto nowMs
        = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::tm tm{};
#if XX_IS_WIN_D
    localtime_s(&tm, &nowT);
#else
    localtime_r(&nowT, &tm);
#endif
    char buf[64]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);

    const std::string text = fmt::format("当前系统日期时间: {}", buf);
    // assistant 消息 (role/content 字段, 与图消息 JSON 字段一致)
    agentxx::util::Json msg = agentxx::util::Json::object();
    msg["role"]             = "assistant";
    msg["content"]          = text;
    msg["startTimeMs"]      = nowMs;
    msg["durationMs"]       = int64_t{0};

    agentxx::util::Json msgs = agentxx::util::Json::array();
    msgs.push_back(std::move(msg));

    return fmt::format(R"({{"writes":[{{"channel":"messages","value":{}}}]}})", msgs.dump());
}

/// =====================================================================
/// 修改执行图: 默认 agentxx.default → 意图路由流程
///
/// 新图 (名称 example_graph_node.intent):
///   __start__ → agent_start → llm → intent_router → [conditional route_channel]
///     datetime  → datetime_node → __end__          (时间节点执行后直接结束轮次)
///     normal    → llm                               (回到 agent loop)
///     tools     → tools → llm                        (原 loop: 工具分发后回 llm)
///     end       → agent_end → __end__
///     default   → agent_end                          (兜底结束)
///
/// channels: messages / savedGraphData / __route__ / __intent_checked
static int modifyGraphToIntentFlow(AgentCtx& ctx, std::string& errOut) {
    if (!ctx.iface.graph || !ctx.iface.graph->get_graph_json || !ctx.iface.graph->set_graph_json) {
        errOut = "graph iface not available";
        return -1;
    }
    AgentxxPluginString graphJson{nullptr, 0};
    ctx.iface.graph->get_graph_json(ctx.host, &graphJson);
    if (!graphJson.data) {
        errOut = "get_graph_json returned null";
        return -1;
    }
    std::string jsonStr(graphJson.data, static_cast<size_t>(graphJson.size));
    agentxx::plugin::PluginString::free(ctx.host, &graphJson);

    agentxx::util::Json def;
    try {
        def = agentxx::util::Json::parse(jsonStr);
    } catch (const std::exception& e) {
        errOut = fmt::format("default graph JSON parse failed: {}", e.what());
        return -1;
    }
    if (!def.is_object()) {
        errOut = "default graph JSON is not an object";
        return -1;
    }

    // 仅处理默认图 (agentxx.default); 已被其他插件修改/自定义的图不动
    const std::string name = def.contains("name") && def["name"].is_string()
                                 ? def["name"].get<std::string>()
                                 : std::string{};
    if (!name.empty() && name != "agentxx.default") {
        errOut = fmt::format("graph name `{}` is not agentxx.default, skip", name);
        return -1;
    }

    // ---- 组装新图 ----
    agentxx::util::Json graph = agentxx::util::Json::object();
    graph["name"]             = "example_graph_node.intent";

    // channels: 原 channels + 路由 channel
    agentxx::util::Json channels = agentxx::util::Json::object();
    if (def.contains("channels") && def["channels"].is_object()) {
        channels = def["channels"];
    }
    if (!channels.contains("__route__")) {
        channels["__route__"] = agentxx::util::Json{
            {"reducer", "overwrite"}
        };
    }
    if (!channels.contains("__intent_checked")) {
        channels["__intent_checked"] = agentxx::util::Json{
            {"reducer", "overwrite"}
        };
    }
    graph["channels"] = std::move(channels);

    // nodes: 原 4 节点 + intent_router + datetime_node
    agentxx::util::Json nodes = agentxx::util::Json::object();
    if (def.contains("nodes") && def["nodes"].is_object()) {
        nodes = def["nodes"];
    }
    nodes["intent_router"] = agentxx::util::Json{
        {"type",     "example_intent_router"},
        {"intents",
         agentxx::util::Json::array({agentxx::util::Json("datetime"), agentxx::util::Json("normal")}
         )                                  },
        {"fallback", "normal"               },
    };
    nodes["datetime_node"] = agentxx::util::Json{
        {"type", "example_datetime"},
    };
    graph["nodes"] = std::move(nodes);

    // edges: 意图路由流程
    agentxx::util::Json edges = agentxx::util::Json::array();
    edges.push_back(agentxx::util::Json{
        {"from", "__start__"  },
        {"to",   "agent_start"}
    });
    edges.push_back(agentxx::util::Json{
        {"from", "agent_start"},
        {"to",   "llm"        }
    });
    edges.push_back(agentxx::util::Json{
        {"from", "llm"          },
        {"to",   "intent_router"}
    });
    edges.push_back(agentxx::util::Json{
        {"from",      "intent_router"},
        {"type",      "conditional"  },
        {"condition", "route_channel"},
        {"routes",
         agentxx::util::Json{
             {"datetime", "datetime_node"},
             {"normal", "llm"},
             {"tools", "tools"},
             {"end", "agent_end"},
             {"default", "agent_end"},
         }                           },
    });
    edges.push_back(agentxx::util::Json{
        {"from", "tools"},
        {"to",   "llm"  }
    });
    edges.push_back(agentxx::util::Json{
        {"from", "datetime_node"},
        {"to",   "__end__"      }
    });
    edges.push_back(agentxx::util::Json{
        {"from", "agent_end"},
        {"to",   "__end__"  }
    });
    graph["edges"] = std::move(edges);

    const std::string newJson = graph.dump();
    auto newJsonSv = agentxx::plugin::PluginStringView::from(newJson.data(), newJson.size());
    if (ctx.iface.graph->set_graph_json(ctx.host, &newJsonSv) != 0) {
        errOut = "set_graph_json failed";
        return -1;
    }
    return 0;
}

/// ---------------- entry / lifecycle ----------------
///
/// 入口语义 (见 docs/zh-cn/design/plugins.md 第 15 节):
/// - `create`: 只构造上下文与查询接口, 不注册节点类型、不修改执行图。
/// - `start`: 注册事务 (两个节点类型 + 执行图修改); 注册失败返回 NULL + error,
///   宿主回滚已生效的节点类型与图类型槽位。
/// - `stop`: 只给出完成信号; 节点类型注册记录由宿主在 stop 后统一撤销
///   (GraphTypeSlot 失效使旧编译节点安全失败), 执行图保持当前定义。
/// - `destroy`: 只释放本地内存。

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    AgentCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            agentGuardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx = std::make_unique<AgentCtx>();
            ctx->init(host);
            raw = ctx.get();

            if (!ctx->iface.graph || !ctx->iface.graph->register_node_type) {
                return -1;
            }
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

/// 注册事务 (start 的实际内容); 任一步失败由宿主回滚已生效的注册。
static int exampleGraphAgentSetup(AgentCtx& ctx) {
    // 1. 注册意图识别节点类型 (统一 root adapter: 输入拥有化 + provider 句柄)
    if (agentxx::plugin::graph_node(
            ctx,
            "example_intent_router",
            R"({"type":"object","properties":{"intents":{"type":"array","items":{"type":"string"}},"fallback":{"type":"string"}}})",
            intentRouterRun
        )
        != 0) {
        return -1;
    }
    // 2. 注册时间输出节点类型
    if (agentxx::plugin::graph_node(ctx, "example_datetime", R"({"type":"object"})", datetimeNodeRun)
        != 0) {
        return -1;
    }

    // 3. 修改执行图: 默认图 → 意图路由流程
    std::string err;
    if (modifyGraphToIntentFlow(ctx, err) != 0) {
        ctx.log.warn("modify graph failed: " + err);
        // 节点已注册, 图修改失败不阻止插件加载 (宿主回退默认图)
    }
    return 0;
}

static void* exampleGraphAgentStart(
    AgentCtx& ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(ctx.host, error, "example_graph_node start: notify required");
        }
        return nullptr;
    }
    if (exampleGraphAgentSetup(ctx) != 0) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host, error, "example_graph_node start: registration transaction failed"
            );
        }
        return nullptr;
    }
    ctx.log.info("example_graph_node started, graph modified");
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* exampleGraphAgentStop(
    AgentCtx&, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*
) {
    // 没有自管线程/定时器; 注册记录由宿主在 stop 后撤销。
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(AgentCtx, exampleGraphAgentStart, exampleGraphAgentStop)

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<AgentCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(agentGuardLogger(ctx), [&] {
        if (!ctx) {
            return;
        }
        delete ctx;
    });
}
