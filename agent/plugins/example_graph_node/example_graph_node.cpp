/// example_graph_node —— 执行图插件示例
///
/// 演示能力 (经 graph 接口表 agentxx.agent.graph):
/// 1. 注册自定义节点类型:
///    - example_intent_router (意图识别): 读取会话上下文里最后一条 assistant
///      消息 (经 agentxx.agent.context 接口表, 上下文由会话持有, 不在图状态里),
///      按 config 中定义的意图枚举 (intents) 匹配, 写 __route__ channel,
///      由图的 conditional edge (route_channel) 决定路由; 非枚举值取
///      fallback (默认 "normal"); 已检查过的轮次退化为 xx_has_tool_calls 语义路由
///    - example_datetime (时间输出): 将当前系统日期时间作为 assistant
///      消息写入 messages channel —— 主程序把该通道的写入转成会话上下文写入
///      (并同步 viewMessages / llmMessages)
/// 2. 修改执行图 JSON: 将默认图 (agentxx.default) 改为
///    用户输入 → llm (识别意图) → intent_router (路由) → datetime 直接结束
///    轮次 / normal 进入原 agent loop (llm→tool→llm→...→end)
///    llm 节点置 xx_autoRoute=false: 由本图的 intent_router 决定路由,
///    不使用主程序默认的"按 tool_calls 直接跳转"行为
///
/// 节点实现遵循"统一异步操作模型" (操作 run_start/run_cancel):
/// - run_start 在主程序 io 线程同步调用 (快同步节点: 直接算完 done 返回 NULL)
/// - state_json 为 GraphState::serialize() 结果, 只读; 修改经返回 writes
///   (其中 messages 通道的写入由主程序转成会话上下文写入)
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"

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

extern "C" PLUGINXX_EXPORT int32_t agentxx_agent_plugin_get_info(PluginxxInfo* out) {
    return agentxx::plugin::detail::fillAgentPluginInfo(
        out,
        "example_graph_node",
        "1.0.0",
        "Example graph node plugin: intent router + datetime node, modifies agent graph"
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

/// 从主程序查询会话 LLM 上下文 (json 数组; 图状态不再包含 messages 通道)
///
/// - 上下文由会话持有 (唯一权威), 经 agentxx.agent.context 接口表查询;
/// - **协程形态**: 节点执行业务跑在插件自己的执行线程上, 那里不能同步等表调用
///   (kit 的同步入口在插件线程上直接拒绝), 因此这里 `co_await` 凭据;
/// - 接口不可用/查询失败时返回空数组 (老主程序降级: 路由退化为 fallback)
pluginxx::Task<utilxx_base::Json> sessionMessages(
    AgentCtx&                          ctx,
    const agentxx::plugin::ScopeRef&   ref,
    const agentxx::plugin::RootRequest& req
) {
    if (!ctx.iface.context || !ctx.iface.context->get_messages) {
        co_return utilxx_base::Json::array();
    }
    auto result = co_await ctx.getSessionMessages(ctx, ref, req.scope());
    const auto text = result.text();
    if (!result.ok() || text.empty()) {
        co_return utilxx_base::Json::array();
    }
    try {
        auto msgs = utilxx_base::Json::parse(std::string{text});
        if (msgs.is_array()) {
            co_return msgs;
        }
    } catch (...) {
        // 解析失败按空上下文处理
    }
    co_return utilxx_base::Json::array();
}

/// 获取最后一条 assistant 消息内容 (无则返回空)
std::string lastAssistantContent(const utilxx_base::Json& messages) {
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
bool lastAssistantHasToolCalls(const utilxx_base::Json& messages) {
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

/// 意图识别节点执行 (**协程节点**):
/// - 第一次经过: 读最后 assistant 消息内容, 匹配 config intents 枚举 →
///   写 __route__ (命中) / fallback (未命中); 同时标记 __intent_checked=true
/// - 后续轮次: 按 "最后一条 assistant 是否带 tool_calls" 写 __route__ (tools/end), 保持原
///   agent loop 行为
/// - config: {"intents": ["datetime", "normal"], "fallback": "normal"}
pluginxx::Task<std::string> intentRouterRun(
    AgentCtx&                          ctx,
    const agentxx::plugin::ScopeRef&   ref,
    const agentxx::plugin::RootRequest& req
) {
    // 输入由 RootRequest 拥有, 挂起期间仍然有效; 先拷一份避免依赖这个细节
    const std::string configJson{agentxx::plugin::nodeConfigOf(req)};
    const std::string stateJson{agentxx::plugin::nodeStateOf(req)};
    auto              state    = utilxx_base::Json::parse(stateJson);
    auto              messages = co_await sessionMessages(ctx, ref, req);

    // 解析 config: intents 枚举 + fallback
    std::vector<std::string> intents;
    std::string              fallback = "normal";
    if (!configJson.empty()) {
        auto cfg = utilxx_base::Json::parse(configJson);
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
        // - 对 messages 通道的 overwrite 写由主程序转成会话上下文替换
        //   (图状态不再持有上下文), 语义与旧版一致
        if (route != fallback && messages.is_array() && !messages.empty()) {
            const auto   origin  = co_await sessionMessages(ctx, ref, req);
            messages             = utilxx_base::Json::array();
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
                    R"_({{"writes":[{{"channel":"__route__","value":{}}},{{"channel":"__intent_checked","value":true}},{{"channel":"messages","value":{},"mode":"overwrite"}}]}})_",
                    utilxx_base::Json(route).dump(),
                    messages.dump()
                );
                co_return payload;
            }
        }
        // 未命中/未移除: 仅写路由标记
        const std::string payload = fmt::format(
            R"_({{"writes":[{{"channel":"__route__","value":{}}},{{"channel":"__intent_checked","value":true}}]}})_",
            utilxx_base::Json(route).dump()
        );
        co_return payload;
    }

    // 后续轮次: 等价图条件 xx_has_tool_calls (插件自己从会话上下文判断)
    route                     = lastAssistantHasToolCalls(messages) ? "tools" : "end";
    const std::string payload = fmt::format(
        R"_({{"writes":[{{"channel":"__route__","value":{}}}]}})_",
        utilxx_base::Json(route).dump()
    );
    co_return payload;
}

/// 时间输出节点执行 (快同步): 写当前系统日期时间到 messages channel
/// - 主程序把 messages 通道写入转成会话上下文写入 (assistant 角色消息),
///   并发出与主程序节点同形的通道写事件 → EventBridge 同步 viewMessages 与
///   节流持久化, 满足"添加到 viewMessages、llmMessages"
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
    utilxx_base::Json msg = utilxx_base::Json::object();
    msg["role"]           = "assistant";
    msg["content"]        = text;
    msg["startTimeMs"]    = nowMs;
    msg["durationMs"]     = int64_t{0};

    utilxx_base::Json msgs = utilxx_base::Json::array();
    msgs.push_back(std::move(msg));

    return fmt::format(R"_({{"writes":[{{"channel":"messages","value":{}}}]}})_", msgs.dump());
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
///     default   → agent_end                          (默认结束)
///
/// 读当前图定义 → 改写为意图路由流程 (**协程形态**)
/// - 读与写都是“提交 + `co_await`”: 返回时定义已写入, `errOut` 给出失败原因
static pluginxx::Task<bool> modifyGraphToIntentFlow(
    AgentCtx&                        ctx,
    const agentxx::plugin::ScopeRef& agent,
    std::string&                     errOut
) {
    if (!ctx.iface.graph || !ctx.iface.graph->get_graph_json || !ctx.iface.graph->set_graph_json) {
        errOut = "graph iface not available";
        co_return false;
    }
    auto readResult = co_await agentxx::plugin::graphJson(ctx, agent.ptr());
    std::string jsonStr{readResult.text()};
    if (!readResult.ok() || jsonStr.empty()) {
        errOut = "get_graph_json returned no definition";
        co_return false;
    }

    utilxx_base::Json def;
    try {
        def = utilxx_base::Json::parse(jsonStr);
    } catch (const std::exception& e) {
        errOut = fmt::format("default graph JSON parse failed: {}", e.what());
        co_return false;
    }
    if (!def.is_object()) {
        errOut = "default graph JSON is not an object";
        co_return false;
    }

    // 仅处理默认图 (agentxx.default); 已被其他插件修改/自定义的图不动
    const std::string name = def.contains("name") && def["name"].is_string()
                                 ? def["name"].get<std::string>()
                                 : std::string{};
    if (!name.empty() && name != "agentxx.default") {
        errOut = fmt::format("graph name `{}` is not agentxx.default, skip", name);
        co_return false;
    }

    // ---- 组装新图 ----
    utilxx_base::Json graph = utilxx_base::Json::object();
    graph["name"]           = "example_graph_node.intent";

    // channels: 原 channels + 路由 channel
    utilxx_base::Json channels = utilxx_base::Json::object();
    if (def.contains("channels") && def["channels"].is_object()) {
        channels = def["channels"];
    }
    if (!channels.contains("__route__")) {
        channels["__route__"] = utilxx_base::Json{
            {"reducer", "overwrite"}
        };
    }
    if (!channels.contains("__intent_checked")) {
        channels["__intent_checked"] = utilxx_base::Json{
            {"reducer", "overwrite"}
        };
    }
    graph["channels"] = std::move(channels);

    // nodes: 原 4 节点 + intent_router + datetime_node
    utilxx_base::Json nodes = utilxx_base::Json::object();
    if (def.contains("nodes") && def["nodes"].is_object()) {
        nodes = def["nodes"];
    }
    // llm 节点关闭"自动路由" (主程序默认按 tool_calls 直接返回 Command):
    // 本图把 llm 接到 intent_router, 由路由节点按 __route__ 条件边决定后续
    if (nodes.contains("llm") && nodes["llm"].is_object()) {
        nodes["llm"]["xx_autoRoute"] = false;
    }
    nodes["intent_router"] = utilxx_base::Json{
        {"type",     "example_intent_router"                                                   },
        {"intents",
         utilxx_base::Json::array({utilxx_base::Json("datetime"), utilxx_base::Json("normal")})},
        {"fallback", "normal"                                                                  },
    };
    nodes["datetime_node"] = utilxx_base::Json{
        {"type", "example_datetime"},
    };
    graph["nodes"] = std::move(nodes);

    // edges: 意图路由流程
    utilxx_base::Json edges = utilxx_base::Json::array();
    edges.push_back(utilxx_base::Json{
        {"from", "__start__"  },
        {"to",   "agent_start"}
    });
    edges.push_back(utilxx_base::Json{
        {"from", "agent_start"},
        {"to",   "llm"        }
    });
    edges.push_back(utilxx_base::Json{
        {"from", "llm"          },
        {"to",   "intent_router"}
    });
    edges.push_back(utilxx_base::Json{
        {"from",      "intent_router"},
        {"type",      "conditional"  },
        {"condition", "route_channel"},
        {"routes",
         utilxx_base::Json{
             {"datetime", "datetime_node"},
             {"normal", "llm"},
             {"tools", "tools"},
             {"end", "agent_end"},
             {"default", "agent_end"},
         }                           },
    });
    edges.push_back(utilxx_base::Json{
        {"from", "tools"},
        {"to",   "llm"  }
    });
    edges.push_back(utilxx_base::Json{
        {"from", "datetime_node"},
        {"to",   "__end__"      }
    });
    edges.push_back(utilxx_base::Json{
        {"from", "agent_end"},
        {"to",   "__end__"  }
    });
    graph["edges"] = std::move(edges);

    const std::string newJson = graph.dump();
    auto              wr      = co_await agentxx::plugin::setGraphJson(ctx, agent.ptr(), newJson);
    if (!wr.ok()) {
        errOut = wr.accepted() ? std::string(wr.text()) : wr.startError;
        if (errOut.empty()) {
            errOut = "set_graph_json failed";
        }
        co_return false;
    }
    co_return true;
}

/// ---------------- entry / lifecycle ----------------
///
/// 入口语义 (见 docs/zh-cn/design/plugins.md 第 15 节):
/// - `create`: 只构造上下文与查询接口, 不注册节点类型、不修改执行图。
/// - `start`: 注册事务 (两个节点类型 + 执行图修改); 注册失败返回 NULL + error,
///   主程序回滚已生效的节点类型与图类型槽位。
/// - `stop`: 只给出完成信号; 节点类型注册记录由主程序在 stop 后统一撤销
///   (GraphTypeSlot 失效使旧编译节点安全失败), 执行图保持当前定义。
/// - `destroy`: 只释放本地内存。

static void exampleGraphAgentStart(AgentCtx&, const PluginxxOperatorNotify*, PluginxxString*);

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_create(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    agentxx::plugin::detail::createCtxEntry<AgentCtx>(
        host,
        plugin_ctx,
        notify,
        err,
        "example_graph_node",
        [](AgentCtx& ctx) -> std::string {
            return {};
        },
        [](AgentCtx& c, const PluginxxOperatorNotify* n, PluginxxString* e) {
            (void)exampleGraphAgentStart(c, n, e);
        }
    );
}


/// 挂载事务 (mount): 登记自定义节点类型并改写执行图 (**协程形态**)
/// - `ref` 为本次挂载的 agent 引用, 节点类型与该 agent 的图定义都记在它名下
/// - 每条登记都是“提交 + `co_await`”: 提交就已经生效, 等结果只为了拿到失败原因
static pluginxx::Task<void>
    exampleGraphAgentSetup(AgentCtx& ctx, agentxx::plugin::AgentRef ref) {
    // 1. 注册意图识别节点类型 (统一 root adapter: 输入拥有化 + provider 句柄)
    auto nodeResult = co_await agentxx::plugin::graph_node(
        ctx,
        ref,
        "example_intent_router",
        R"_({"type":"object","properties":{"intents":{"type":"array","items":{"type":"string"}},"fallback":{"type":"string"}}})_",
        [ref](AgentCtx& c, const agentxx::plugin::RootRequest& req) {
            return intentRouterRun(c, ref, req);
        }
    );
    if (!nodeResult.ok()) {
        throw std::runtime_error{
            "example_graph_node: register node type example_intent_router failed: "
            + (nodeResult.accepted() ? std::string(nodeResult.text()) : nodeResult.startError)
        };
    }
    // 2. 注册时间输出节点类型
    auto datetimeResult = co_await agentxx::plugin::graph_node(
        ctx,
        ref,
        "example_datetime",
        R"_({"type":"object"})_",
        datetimeNodeRun
    );
    if (!datetimeResult.ok()) {
        throw std::runtime_error{
            "example_graph_node: register node type example_datetime failed: "
            + (datetimeResult.accepted() ? std::string(datetimeResult.text())
                                         : datetimeResult.startError)
        };
    }

    // 3. 修改执行图: 默认图 → 意图路由流程
    std::string err;
    if (!co_await modifyGraphToIntentFlow(ctx, ref, err)) {
        ctx.log.warn("modify graph failed: " + err);
        // 节点已注册, 图修改失败不阻止插件加载 (主程序回退默认图)
    }
    co_return;
}

static void exampleGraphAgentStart( 
    AgentCtx&                     ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_graph_node start: notify required"
            );
        }
        return;
    }
    // 进程级启动: 本插件没有自管线程/定时器, 注册全在挂载阶段做
    ctx.log.info("example_graph_node started");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

static void* exampleGraphAgentMount(
    AgentCtx&                          ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_graph_node mount: notify required"
            );
        }
        return nullptr;
    }
    if (!ctx.iface.tasks || !ctx.iface.tasks->register_task) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_graph_node mount: host has no pluginxx.tasks (spawn unavailable)"
            );
        }
        return nullptr;
    }
    auto later = agentxx::plugin::finishLater(notify);
    // 注册事务交给协程: 节点类型/图定义逐条“提交 + co_await”, 全部生效后再放行挂载
    const agentxx::plugin::AgentRef agentRef(*spec);
    ctx.spawn([later, agentRef](AgentCtx& c, agentxx::plugin::OpCtl&) -> pluginxx::Task<void> {
        try {
            co_await exampleGraphAgentSetup(c, agentRef);
            c.log.info("example_graph_node mounted, graph modified");
            later.ok();
        } catch (const std::exception& e) {
            later.failed(e.what());
        }
        co_return;
    });
    return later.provider();
}

static void exampleGraphAgentUnmount(
    AgentCtx&, void*, const PluginxxOperatorNotify* notify, PluginxxString*
) {
    // 节点类型与图定义由主程序按挂载记录撤销
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

static void    exampleGraphAgentStop(AgentCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // 没有自管线程/定时器; 注册记录由主程序在 stop 后撤销。
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

AGENTXX_AGENT_PLUGIN_LIFECYCLE_EXPORT(
    AgentCtx,
    exampleGraphAgentStop,
    exampleGraphAgentMount,
    exampleGraphAgentUnmount
)

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_destroy(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    agentxx::plugin::detail::destroyCtxEntry<AgentCtx>(
        plugin_ctx,
        notify,
        err,
        [](AgentCtx* ctx) { delete ctx; }
    );
}
