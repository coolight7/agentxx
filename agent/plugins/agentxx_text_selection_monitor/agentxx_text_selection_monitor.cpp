/// agentxx_text_selection_monitor —— 系统级文本选择事件流插件
#include "fmt/format.h"
#include "text_selection_monitor.h"
#include "text_selection_monitor_plugin.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

using namespace agentxx_text_selection_monitor_plugin;
using namespace agentxx::plugin;

namespace {

const char* sourceName(agentxx_text_selection_monitor_plugin::TextSource s) {
    switch (s) {
        case agentxx_text_selection_monitor_plugin::TextSource::Unknown:
            return "unknown";
        case agentxx_text_selection_monitor_plugin::TextSource::TextPattern:
            return "text_pattern";
        case agentxx_text_selection_monitor_plugin::TextSource::TextChildPattern:
            return "text_child_pattern";
        case agentxx_text_selection_monitor_plugin::TextSource::ValuePattern:
            return "value_pattern";
        case agentxx_text_selection_monitor_plugin::TextSource::EmGetSel:
            return "em_get_sel";
        case agentxx_text_selection_monitor_plugin::TextSource::AccessibleObject:
            return "accessible_object";
        case agentxx_text_selection_monitor_plugin::TextSource::WmGetText:
            return "wm_get_text";
        case agentxx_text_selection_monitor_plugin::TextSource::DevTools:
            return "devtools";
        case agentxx_text_selection_monitor_plugin::TextSource::FlutterAccessibility:
            return "flutter_accessibility";
        case agentxx_text_selection_monitor_plugin::TextSource::Clipboard:
            return "clipboard";
    }
    return "unknown";
}

struct TextSelectionPluginCtx;

// ==================== CDP 网络 (经主程序 `<应用名>.net` 表) ====================

/// 本插件 id (网络事件频道 `plugin.<插件 id>.net` 用它拼)
constexpr std::string_view kPluginId = "agentxx_text_selection_monitor";

/// 一次 WebSocket 往返的交互状态
///
/// 事件在管理器 io 线程到达 (广播回调), `run()` 在工作线程上等条件变量;
/// `alive` 在退订前置 false, 保证晚到的回调不会访问已销毁的状态。
struct WsRoundTripState {
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    alive   = true;
    bool                    opened  = false;
    bool                    closed  = false;
    bool                    hasNews = false;
    std::string             connId;
    std::string             message;
    std::string             closeError;
};

/// 网络事件回调 (频道 `plugin.<插件 id>.net`; `ud` 是 [WsRoundTripState])
void PLUGINXX_CALL onCdpNetEvent(const PluginxxStringView* payloadJson, void* ud) {
    auto* state = static_cast<WsRoundTripState*>(ud);
    if (!state || !payloadJson || !payloadJson->data) {
        return;
    }
    utilxx_base::Json event;
    try {
        event = utilxx_base::Json::parse(
            std::string_view{payloadJson->data, static_cast<size_t>(payloadJson->size)}
        );
    } catch (...) {
        return;
    }
    if (!event.is_object()) {
        return;
    }
    const std::string kind = event.value("kind", "");
    const std::string id   = event.value("id", "");

    std::lock_guard<std::mutex> lock(state->mtx);
    if (!state->alive || id != state->connId) {
        return;
    }
    if (kind == "ws.open") {
        state->opened = true;
    } else if (kind == "ws.message") {
        state->message = event.value("text", "");
        state->hasNews = true;
    } else if (kind == "ws.close") {
        state->closed = true;
        if (state->message.empty()) {
            state->closeError = event.value("error", "");
        }
    }
    state->cv.notify_all();
}

/// 组装给文本选择器用的 CDP 动作 (定义在插件上下文之后, 因为它要用到 ctx 的接口表)
CdpNetFns makeCdpNetFns(TextSelectionPluginCtx& ctx);

struct TextSelectionHolder {
    bool start(int debounceMs);
    void stop();

    agentxx_text_selection_monitor_plugin::TextSelectionMonitor monitor_;
    TextSelectionPluginCtx*                                     ctx = nullptr;
};

struct TextSelectionPluginCtx : public PluginBase {
    TextSelectionHolder holder;

    ~TextSelectionPluginCtx() override {
        holder.stop();
    }
};

/// HTTP GET (只取响应体; 失败返回空串)
std::string cdpHttpGet(TextSelectionPluginCtx& ctx, std::string_view url, int timeoutMs) {
    if (!ctx.host || !ctx.iface.net) {
        return {};
    }
    agentxx::plugin::NetHttpOptions options;
    options.method    = "GET";
    options.timeoutMs = timeoutMs > 0 ? timeoutMs : 3000;
    const auto result = netFetchBlocking(
        ctx.host,
        ctx.iface.net,
        ctx.iface.scheduler,
        url,
        options
    );
    if (!result.ok()) {
        return {};
    }
    const int64_t status = result.status();
    if (status < 200 || status >= 300) {
        return {};
    }
    return result.str("body");
}

std::string cdpWsRoundTrip(
    TextSelectionPluginCtx& ctx,
    std::string_view        url,
    std::string_view        message,
    int                     timeoutMs
) {
    if (!ctx.host || !ctx.iface.net || !ctx.iface.broadcasts) {
        return {};
    }
    static std::atomic<uint64_t> seq{0};
    const int                    waitMs = timeoutMs > 0 ? timeoutMs : 5000;

    WsRoundTripState state;
    state.connId = fmt::format("{}-{}", kPluginId, seq.fetch_add(1) + 1);

    const std::string topic = agentxx::plugin::netEventTopic(kPluginId);
    agentxx::plugin::declarePluginBroadcast(
        ctx.host,
        ctx.iface.broadcasts,
        topic,
        /*anyonePublish=*/false,
        /*cacheLatest=*/false
    );
    PluginxxSubscription* sub = agentxx::plugin::subscribePluginBroadcast(
        ctx.host,
        ctx.iface.broadcasts,
        nullptr,
        topic,
        &onCdpNetEvent,
        &state
    );
    if (!sub) {
        return {};
    }

    // 退订与“置 alive=false”成对出现: 置位后再退订, 晚到的回调只会看到 alive=false
    auto teardown = [&]() {
        {
            std::lock_guard<std::mutex> lock(state.mtx);
            state.alive = false;
        }
        ctx.iface.broadcasts->unsubscribe_broadcast(sub);
    };

    std::string out;
    const auto  connectResult = netWsConnect(
        ctx.host,
        ctx.iface.net,
        url,
        state.connId,
        /*headersJson=*/{},
        /*subprotocols=*/{},
        waitMs
    );
    if (!connectResult.ok()) {
        teardown();
        return {};
    }

    {
        std::unique_lock<std::mutex> lock(state.mtx);
        state.cv.wait_for(lock, std::chrono::milliseconds{waitMs}, [&state]() {
            return state.opened || state.closed;
        });
        if (!state.opened) {
            lock.unlock();
            netWsClose(ctx.host, ctx.iface.net, state.connId, "open-failed");
            teardown();
            return {};
        }
    }

    if (!netWsSend(ctx.host, ctx.iface.net, state.connId, message).ok()) {
        netWsClose(ctx.host, ctx.iface.net, state.connId, "send-failed");
        teardown();
        return {};
    }

    {
        std::unique_lock<std::mutex> lock(state.mtx);
        state.cv.wait_for(lock, std::chrono::milliseconds{waitMs}, [&state]() {
            return state.hasNews || state.closed;
        });
        out = std::move(state.message);
    }

    netWsClose(ctx.host, ctx.iface.net, state.connId, "done");
    teardown();
    return out;
}

/// 组装给文本选择器用的 CDP 动作 (两个函数都在工作线程上调用)
CdpNetFns makeCdpNetFns(TextSelectionPluginCtx& ctx) {
    CdpNetFns fns;
    fns.httpGet = [&ctx](std::string_view url, int timeoutMs) {
        return cdpHttpGet(ctx, url, timeoutMs);
    };
    fns.wsRoundTrip = [&ctx](std::string_view url, std::string_view message, int timeoutMs) {
        return cdpWsRoundTrip(ctx, url, message, timeoutMs);
    };
    return fns;
}
bool TextSelectionHolder::start(int debounceMs) {
    if (monitor_.isRunning()) {
        return false;
    }
    if (debounceMs > 0) {
        monitor_.setDebounceMs(debounceMs);
    }
    // CDP 网络动作每次 start 时注入 (主程序网络能力不可用时为空, 浏览器路径自动跳过)
    if (ctx) {
        monitor_.setCdpNetFns(makeCdpNetFns(*ctx));
    }
    monitor_.addListener(
        [ctx = this->ctx](const agentxx_text_selection_monitor_plugin::TextSelectionEvent& evt) {
            try {
                if (!ctx || !ctx->host || !ctx->iface.broadcasts || !ctx->iface.broadcasts->publish_broadcast) {
                    return;
                }
                auto tsMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                evt.timestamp.time_since_epoch()
                )
                                .count();
                std::string payload = fmt::format(
                    R"_({{"text":{},"source":{},"timestamp_ms":{}}})_",
                    ctx->jsonEscape(evt.text),
                    ctx->jsonEscape(sourceName(evt.source)),
                    tsMs
                );
                auto topicSv
                    = PluginStringView::fromCstr("plugin.agentxx_text_selection_monitor.selection");
                auto payloadSv = PluginStringView::from(payload.data(), payload.size());
                agentxx::plugin::publishPluginBroadcast(ctx->host, ctx->iface.broadcasts, topicSv, payloadSv);
            } catch (...) {
            }
        }
    );
    return monitor_.start();
}

void TextSelectionHolder::stop() {
    monitor_.stop();
    monitor_.removeAllListeners();
}

} // namespace

/// ==================== 生命周期 (create 只构造, start 进程级, mount 注册, stop 撤销) ====================

static void* textSelectionAgentStart(
    TextSelectionPluginCtx&       ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    ctx.holder.ctx = &ctx;
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* textSelectionAgentMount(
    TextSelectionPluginCtx&            ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    err
) {
    const agentxx::plugin::AgentRef ref(*spec);

    auto schema
        = ctx.schema("agentxx_text_selection_monitor")
              .enumString(
                  "command",
                  "Operation command: start listening, stop listening, or query running status.",
                  {"start", "stop", "status"},
                  /*required=*/true
              )
              .integer(
                  "debounce_ms",
                  "Debounce interval in milliseconds (default: 150). Only applies to start command."
              )
              .build();

    blocking_tool(
        ctx,
        ref,
        "agentxx_text_selection_monitor",
        "Monitor system-wide text selection events. Supports start, stop, and status query.",
        schema,
        [](TextSelectionPluginCtx& c, std::string_view args_json) -> agentxx::plugin::ToolResult {
            ArgReader args(args_json);
            auto      command = args.require<std::string>("command");
            if (!args.ok()) {
                return agentxx::plugin::ToolResult::fail(args.errorMessage());
            }

            TextSelectionHolder& holder = c.holder;

            if (command == "start") {
                int64_t debounceMs = args.value("debounce_ms", int64_t{0});
                bool    ok         = holder.start(static_cast<int>(debounceMs));
                return agentxx::plugin::ToolResult::ok(
                    fmt::format(R"_({{"ok":{},"running":true}})_", ok ? "true" : "false")
                );
            }

            if (command == "stop") {
                holder.stop();
                return agentxx::plugin::ToolResult::ok(R"_({"ok":true,"running":false})_");
            }

            if (command == "status") {
                bool running = holder.monitor_.isRunning();
                return agentxx::plugin::ToolResult::ok(
                    fmt::format(R"_({{"ok":true,"running":{}}})_", running ? "true" : "false")
                );
            }

            return agentxx::plugin::ToolResult::fail(R"_(unknown command: use start/stop/status)_");
        }
    );

    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* textSelectionAgentUnmount(
    TextSelectionPluginCtx&,
    void*,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

/// stop: 停止监听并摘除监听器 (可重复调用), 之后主程序才会调用 destroy
static void*
    textSelectionAgentStop(TextSelectionPluginCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    try {
        ctx.holder.stop();
    } catch (...) {
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    TextSelectionPluginCtx,
    "agentxx_text_selection_monitor",
    "1.0.0",
    "System-wide text selection monitor event stream",
    textSelectionAgentStart,
    textSelectionAgentStop,
    textSelectionAgentMount,
    textSelectionAgentUnmount
);
