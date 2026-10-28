/// example_runtime —— 插件内驱动 agent 会话的最小示例 (`agentxx.agent.runtime`)
///
/// 演示一条完整调用路径, 也是写"聊天平台机器人插件"的骨架:
/// 1. `start` 里挂一个**自己的根 agent** (`mountRoot`):
///    一份独立 `AgentConfig` (自定义系统提示词 / 工具白名单 / 模型), 与内置会话互不影响;
/// 2. 在根上开一个**会话通道** (`openChannel`): 一个 `sessionId` 的驱动壳,
///    输入 / 事件 / 中断都走它; 事件回调里打印正文与最终回答;
/// 3. 给通道配一个 **COALESCE 触发器** (`addTrigger`): 同一主题的连发在窗口内合并;
/// 4. 可选 **定时器** (`addRuntimeTimer`): 到点把一条输入投进通道;
/// 5. 在被挂载的 agent 上注册两个工具 (`mount` 回调里), 让用户能从主会话
///    往这个通道投消息、发事件、查状态 —— 真实插件里换成平台的消息收发即可。
///
/// 参数 (yaml `plugin.list[].args`, 全部可选):
/// ```yaml
/// args:
///   prompt: "你在群里回答问题, 回答不超过三句话。"
///   toolWhitelist: ["agentxx_share_store"]
///   mountPlugins: {"mode": "none"}          # 根挂哪些插件 (空 = 只有内置工具)
///   sessionId: "example_runtime_demo"       # 通道的会话 id
///   timerDelayMs: 0                         # > 0 时登记一个一次性定时器 (默认关闭)
///   timerText: "定时检查: 现在几点?"
/// ```
///
/// 说明: 根的插件挂载策略里**默认不含本插件** (`include_creator=false`),
/// 否则根又会有"往通道投消息"的工具, 容易形成自己给自己发消息。
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

/// 每实例上下文 (进程一份; 根与通道都是"主程序侧对象", 这里只记句柄)
struct RuntimeCtx : public agentxx::plugin::PluginBase {
    /// 本插件挂的根 id (空 = 未挂或挂失败)
    std::string rootId;
    /// 通道 id (0 = 未打开)
    int64_t channelId = 0;
    /// 触发器 / 定时器 id (0 = 未登记)
    int64_t triggerId = 0;
    int64_t timerId   = 0;
    /// 触发器订阅的主题
    std::string triggerTopic;

    /// 通道事件回调与工具回调都在这条 io 线程上跑, 但测试/诊断可能从别的线程读,
    /// 因此状态用一把锁保护
    std::mutex  mutex;
    bool        channelReady = false;
    int         turnCount    = 0;
    std::string lastAnswer;
    std::string lastError;

    /// 取构造时的参数 (yaml `plugin.list[].args`; 主程序原样转交)
    utilxx_base::Json args() const {
        try {
            return utilxx_base::Json::parse(argsJson());
        } catch (const std::exception&) {
            return utilxx_base::Json::object();
        }
    }
};

namespace {

auto guardLogger(RuntimeCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            agentxx::plugin::pluginLog(ctx, 4, msg ? msg : "");
        }
    };
}

/// 通道事件名的可读文本 (日志用)
const char* eventName(int32_t kind) {
    switch (kind) {
        case AGENTXX_PLUGIN_CHANNEL_EVT_READY:
            return "READY";
        case AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START:
            return "TURN_START";
        case AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_TEXT:
            return "DELTA_TEXT";
        case AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_THINKING:
            return "DELTA_THINKING";
        case AGENTXX_PLUGIN_CHANNEL_EVT_TOOL_START:
            return "TOOL_START";
        case AGENTXX_PLUGIN_CHANNEL_EVT_TOOL_END:
            return "TOOL_END";
        case AGENTXX_PLUGIN_CHANNEL_EVT_GEN_STATUS:
            return "GEN_STATUS";
        case AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END:
            return "TURN_END";
        case AGENTXX_PLUGIN_CHANNEL_EVT_CONTEXT_STATS:
            return "CONTEXT_STATS";
        case AGENTXX_PLUGIN_CHANNEL_EVT_INPUT_ACK:
            return "INPUT_ACK";
        case AGENTXX_PLUGIN_CHANNEL_EVT_QUEUE_STATE:
            return "QUEUE_STATE";
        case AGENTXX_PLUGIN_CHANNEL_EVT_INTERRUPT:
            return "INTERRUPT";
        case AGENTXX_PLUGIN_CHANNEL_EVT_INTERRUPT_EXPIRED:
            return "INTERRUPT_EXPIRED";
        case AGENTXX_PLUGIN_CHANNEL_EVT_TIP:
            return "TIP";
        case AGENTXX_PLUGIN_CHANNEL_EVT_ERROR:
            return "ERROR";
        case AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED:
            return "CLOSED";
        default:
            return "UNKNOWN";
    }
}

/// 根配置: 自定义提示词 + 工具白名单 (可选模型)
utilxx_base::Json rootConfigOf(const utilxx_base::Json& args) {
    utilxx_base::Json prompt = utilxx_base::Json::object();
    prompt["systemPrompt"]   = args.value(
        "prompt",
        std::string{"你是通过插件通道运行的助手。回答简短, 不超过三句话。"}
    );

    utilxx_base::Json cfg = utilxx_base::Json::object();
    cfg["agentName"]      = "example_runtime";
    cfg["prompt"]         = std::move(prompt);
    // 会话不落盘: 示例插件不产生磁盘副作用 (真实插件建议开启)
    cfg["enableSessionStore"] = false;
    // 对外应答的根不需要子代理
    cfg["enableSubagent"] = false;
    if (args.contains("model") && args["model"].is_string()) {
        cfg["model"] = args["model"];
    }
    if (args.contains("toolWhitelist") && args["toolWhitelist"].is_array()) {
        cfg["toolWhitelist"]       = args["toolWhitelist"];
        cfg["enableToolFiltering"] = true;
    }
    return cfg;
}

/// 通道事件回调 (主程序 io 线程; **必须快速返回**)
void onChannelEventImpl(RuntimeCtx& ctx, const agentxx::plugin::ChannelEvent& ev) {
    switch (ev.kind) {
        case AGENTXX_PLUGIN_CHANNEL_EVT_READY: {
            // 载荷里有 rootId / sessionId / model: 插件据此确认"这条通道已可用"
            std::lock_guard lock(ctx.mutex);
            ctx.channelReady = true;
            ctx.log.info(fmt::format(
                "channel {} ready: {}",
                ev.channelId,
                std::string{ev.payloadJson}
            ));
            break;
        }
        case AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END: {
            std::lock_guard lock(ctx.mutex);
            ctx.turnCount++;
            ctx.lastAnswer.assign(ev.text.data(), ev.text.size());
            ctx.log.info(fmt::format(
                "turn done (channel {}): {}",
                ev.channelId,
                std::string{ev.text}
            ));
            break;
        }
        case AGENTXX_PLUGIN_CHANNEL_EVT_ERROR: {
            std::lock_guard lock(ctx.mutex);
            ctx.lastError.assign(ev.text.data(), ev.text.size());
            ctx.log.warn(fmt::format("channel {} error: {}", ev.channelId, ctx.lastError));
            break;
        }
        case AGENTXX_PLUGIN_CHANNEL_EVT_INTERRUPT: {
            // 需要人工答复 (权限询问等): 真实插件应把问题发到群里, 收到回复再
            // respondInterrupt。示例里直接按"取消"应答, 免去等人。
            ctx.log.warn(fmt::format(
                "channel {} needs an answer (interrupt {}); replying with cancel",
                ev.channelId,
                ev.interruptId
            ));
            agentxx::plugin::respondInterrupt(ctx, ev.channelId, ev.interruptId, R"({"values":{}})");
            break;
        }
        case AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED: {
            std::lock_guard lock(ctx.mutex);
            ctx.channelReady = false;
            ctx.log.info(fmt::format(
                "channel {} closed: {}",
                ev.channelId,
                std::string{ev.payloadJson}
            ));
            break;
        }
        default:
            // 正文增量、工具事件等在真实插件里转发给聊天平台; 示例只记 debug 日志
            ctx.log.debug(fmt::format(
                "channel {} event {}",
                ev.channelId,
                eventName(ev.kind)
            ));
            break;
    }
}

/// 给被挂的 agent 注册的工具 (mount 回调里做)
void registerHostTools(RuntimeCtx& ctx, const agentxx::plugin::AgentRef& ref) {
    // 1) 往通道投一条消息 (等同平台收到一条用户消息)
    agentxx::plugin::fast_tool(
        ctx,
        ref,
        "example_runtime_say",
        "把一句话投给 example_runtime 插件自己的会话通道, 返回通道 id 与受理结果",
        R"({"type":"object","properties":{"text":{"type":"string","description":"要投递的消息正文"}},"required":["text"]})",
        [](RuntimeCtx& c, std::string_view argsJson) -> std::string {
            const auto args = [&argsJson]() {
                try {
                    return utilxx_base::Json::parse(std::string{argsJson});
                } catch (const std::exception&) {
                    return utilxx_base::Json::object();
                }
            }();
            const std::string text = args.value("text", std::string{});
            if (text.empty()) {
                throw std::invalid_argument("text 不能为空");
            }
            const int64_t channelId = c.channelId;
            if (channelId <= 0) {
                throw std::runtime_error("会话通道尚未打开 (插件未启动或根挂载失败)");
            }
            const int32_t rc = agentxx::plugin::sendText(c, channelId, text);
            return fmt::format(
                R"({{"channelId":{},"accepted":{},"rc":{}}})",
                channelId,
                rc == AGENTXX_PLUGIN_RUNTIME_OK ? "true" : "false",
                rc
            );
        }
    );

    // 2) 发一条事件 (由 COALESCE 触发器合并后投进通道)
    agentxx::plugin::fast_tool(
        ctx,
        ref,
        "example_runtime_ping",
        "发一条 example_runtime 事件, 触发器把窗口内的多条合并成一条投入通道",
        R"({"type":"object","properties":{"text":{"type":"string","description":"事件正文, 缺省为 ping"}}})",
        [](RuntimeCtx& c, std::string_view argsJson) -> std::string {
            std::string text = "ping";
            try {
                auto args = utilxx_base::Json::parse(std::string{argsJson});
                if (args.contains("text") && args["text"].is_string()) {
                    text = args["text"].get<std::string>();
                }
            } catch (const std::exception&) {
                // 参数不是对象时按默认文本
            }
            if (c.triggerTopic.empty()) {
                throw std::runtime_error("触发器尚未登记");
            }
            utilxx_base::Json payload = utilxx_base::Json::object();
            payload["text"]           = text;
            const int32_t rc = agentxx::plugin::emitEvent(c, c.triggerTopic, payload.dump());
            return fmt::format(
                R"({{"topic":"{}","rc":{}}})",
                c.triggerTopic,
                rc
            );
        }
    );

    // 3) 运行时状态 (插件自查用: 根 / 通道 / 触发器 / 定时器一览)
    agentxx::plugin::fast_tool(
        ctx,
        ref,
        "example_runtime_status",
        "返回 example_runtime 插件的根、通道、触发器、定时器与最近一轮回答",
        R"({"type":"object","properties":{}})",
        [](RuntimeCtx& c, std::string_view) -> std::string {
            std::string rootId;
            std::string lastAnswer;
            int64_t     channelId = 0;
            int         turns     = 0;
            bool        ready     = false;
            {
                std::lock_guard lock(c.mutex);
                rootId     = c.rootId;
                channelId  = c.channelId;
                lastAnswer = c.lastAnswer;
                turns      = c.turnCount;
                ready      = c.channelReady;
            }
            utilxx_base::Json out = utilxx_base::Json::object();
            out["rootId"]          = rootId;
            out["channelId"]       = channelId;
            out["channelReady"]    = ready;
            out["turnCount"]       = turns;
            out["lastAnswer"]      = lastAnswer;
            out["triggerId"]       = c.triggerId;
            out["timerId"]         = c.timerId;
            // 主程序侧的运行时清单: 插件排查"为什么没投递"的第一现场
            out["runtime"]         = utilxx_base::Json::parse(agentxx::plugin::listRuntimeJson(c));
            return out.dump();
        }
    );
}

} // namespace

// ===========================================================================
// 入口: get_info / create / start / stop / mount / unmount / destroy
// ===========================================================================

extern "C" PLUGINXX_EXPORT const PluginxxInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const PluginxxInfo* {
            static const PluginxxInfo info{
                PLUGINXX_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("example_runtime"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Example runtime plugin: own root agent + chat channel + trigger + timer"
                ),
            };
            return &info;
        }
    );
}

extern "C" PLUGINXX_EXPORT int
    agentxx_plugin_agent_create(const PluginxxHost* host, void** plugin_ctx) {
    RuntimeCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            guardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx = std::make_unique<RuntimeCtx>();
            ctx->init(host);
            raw = ctx.get();
            // create 只构造与查询接口: 建根/开通道是进程级事务, 一律放 start
            if (!agentxx::plugin::hasRuntime(*ctx)) {
                ctx->log.error(
                    "example_runtime requires the agentxx.agent.runtime interface "
                    "(main program does not provide it)"
                );
                return -1;
            }
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

/// 进程级启动: 挂根 → 开通道 → 配触发器/定时器。
///
/// 这一步的顺序是有讲究的: 挂根之后根会异步 init, 通道可以先开 (就绪后自动启动);
/// 触发器与定时器都要求通道 id 存在, 所以放在开通道之后。
static void*
    runtimeStart(RuntimeCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString* error) {
    if (!notify || !notify->done) {
        agentxx::plugin::PluginString::set(ctx.host, error, "start: notify required");
        return nullptr;
    }
    const auto args = ctx.args();

    // ---- 1. 挂自己的根 ----
    agentxx::plugin::MountRootOptions rootOpt;
    rootOpt.name = args.value("rootName", std::string{"example_runtime"});
    rootOpt.config = rootConfigOf(args);
    if (args.contains("mountPlugins") && args["mountPlugins"].is_object()) {
        rootOpt.mountPlugins = args["mountPlugins"].dump();
    }
    ctx.rootId = agentxx::plugin::mountRoot(ctx, rootOpt);
    if (ctx.rootId.empty()) {
        agentxx::plugin::PluginString::set(
            ctx.host,
            error,
            "start: mountRoot failed (see main program log: invalid config or root budget)"
        );
        return nullptr;
    }
    ctx.log.info(fmt::format("root mounted: {}", ctx.rootId));

    // ---- 2. 在根上开一个会话通道 ----
    agentxx::plugin::ChannelOptions chOpt;
    chOpt.rootId    = ctx.rootId;
    chOpt.sessionId = args.value("sessionId", std::string{"example_runtime_demo"});
    chOpt.title     = "example_runtime 示例会话";
    ctx.channelId   = agentxx::plugin::openChannel(
        ctx,
        chOpt,
        [&ctx](const agentxx::plugin::ChannelEvent& ev) {
            onChannelEventImpl(ctx, ev);
        }
    );
    if (ctx.channelId == 0) {
        agentxx::plugin::PluginString::set(
            ctx.host,
            error,
            "start: openChannel failed (see main program log: session occupied or budget)"
        );
        return nullptr;
    }
    ctx.log.info(fmt::format("channel opened: {}", ctx.channelId));

    // ---- 3. COALESCE 触发器: 窗口内的连发合并成一条输入 ----
    ctx.triggerTopic = "plugin.example_runtime.ping";
    agentxx::plugin::TriggerOptions trigger;
    trigger.topic      = ctx.triggerTopic;
    trigger.channelId  = ctx.channelId;
    trigger.policy     = AGENTXX_PLUGIN_TRIGGER_POLICY_COALESCE;
    trigger.debounceMs = args.value("triggerDebounceMs", 200);
    if (args.contains("triggerMaxQueue") && args["triggerMaxQueue"].is_number_integer()) {
        trigger.maxQueue = args["triggerMaxQueue"].get<int32_t>();
    }
    ctx.triggerId = agentxx::plugin::addTrigger(ctx, trigger);
    if (ctx.triggerId == 0) {
        ctx.log.warn("trigger registration failed; event-driven demo is disabled");
    } else {
        ctx.log.info(fmt::format("trigger registered: {} on `{}`", ctx.triggerId, ctx.triggerTopic));
    }

    // ---- 4. 可选定时器 (默认关闭: 到点跑一轮会产生模型调用) ----
    const int64_t timerDelayMs = args.value("timerDelayMs", int64_t{0});
    if (timerDelayMs > 0) {
        utilxx_base::Json input = utilxx_base::Json::object();
        input["text"] = args.value("timerText", std::string{"定时检查: 现在几点?"});
        agentxx::plugin::RuntimeTimerOptions timer;
        timer.channelId    = ctx.channelId;
        timer.inputJson    = input.dump();
        timer.firstDelayMs = timerDelayMs;
        timer.intervalMs   = args.value("timerIntervalMs", int64_t{0});
        timer.policy       = AGENTXX_PLUGIN_TRIGGER_POLICY_DROP_IF_BUSY;
        ctx.timerId        = agentxx::plugin::addRuntimeTimer(ctx, timer);
        if (ctx.timerId == 0) {
            ctx.log.warn("timer registration failed");
        } else {
            ctx.log.info(fmt::format("timer registered: {} (first delay {} ms)", ctx.timerId, timerDelayMs));
        }
    }

    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

/// 进程级停止: 撤销自管资源 (通道 → 触发器/定时器 → 根)
///
/// 主程序在禁用/卸载时也会按归属整批回收, 所以这里的清理是"自己先收干净";
/// 全部入口都是幂等的, 重复清理不会出错。
static void* runtimeStop(RuntimeCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    if (ctx.timerId != 0) {
        agentxx::plugin::removeRuntimeTimer(ctx, ctx.timerId);
        ctx.timerId = 0;
    }
    if (ctx.triggerId != 0) {
        agentxx::plugin::removeTrigger(ctx, ctx.triggerId);
        ctx.triggerId = 0;
    }
    if (ctx.channelId != 0) {
        agentxx::plugin::closeChannel(ctx, ctx.channelId);
        ctx.channelId = 0;
    }
    if (!ctx.rootId.empty()) {
        agentxx::plugin::unmountRoot(ctx, ctx.rootId);
        ctx.rootId.clear();
    }
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

/// 每个 agent 一份: 注册"投消息 / 发事件 / 查状态"三个工具
static void* runtimeForAgent(
    RuntimeCtx&                        ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
) {
    if (!notify || !notify->done) {
        agentxx::plugin::PluginString::set(ctx.host, error, "mount: notify required");
        return nullptr;
    }
    if (!spec) {
        agentxx::plugin::PluginString::set(ctx.host, error, "mount: spec is null");
        return nullptr;
    }
    registerHostTools(ctx, agentxx::plugin::AgentRef(*spec));
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

/// 拆 agent 挂载: 工具由主程序按挂载记录撤销, 这里没有额外状态
static void* runtimeForAgentEnd(
    RuntimeCtx&, void*, const PluginxxOperatorNotify* notify, PluginxxString*
) {
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(
    RuntimeCtx,
    runtimeStart,
    runtimeStop,
    runtimeForAgent,
    runtimeForAgentEnd
)

extern "C" PLUGINXX_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<RuntimeCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(guardLogger(ctx), [&] {
        if (!ctx) {
            return;
        }
        delete ctx;
    });
}
