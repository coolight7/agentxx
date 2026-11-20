/// agentxx_text_selection_monitor —— 系统级文本选择事件流插件
#include "fmt/format.h"
#include "text_selection_monitor.h"
#include "text_selection_monitor_plugin.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
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

/// 在工作线程上等一次网络调用的结果 (回调 + 条件变量)
///
/// - 主程序把完成回调派发在主程序 io 线程上, 所以在这里等不会自锁;
/// - 在主程序 io 线程上等会把整个主程序卡住, 这种情况直接按错误返回;
/// - 本插件的 CDP 交互本来就是阻塞形态 (在工作线程上等事件), 所以可以这么等。
static NetResult netCallWait(
    const PluginxxHost*           host,
    const PluginxxNetIface*       net,
    const PluginxxSchedulerIface* scheduler,
    std::string_view              op,
    std::string_view              argsJson
) {
    NetResult result;
    if (!host || !net || !net->call || op.empty()) {
        return result;
    }
    if (scheduler && scheduler->is_io_thread && scheduler->is_io_thread(host) != 0) {
        result.code = PLUGINXX_RESULT_ERR_STATE;
        result.json = R"({"ok":false,"error":"wrong_thread"})";
        return result;
    }

    struct WaitState {
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    done   = false;
        int32_t                 status = PLUGINXX_OPERATOR_FAILED;
        std::string             payload;
    } state;

    PluginxxOperatorCallback cb
        = [](void* ud, int32_t status, const PluginxxStringView* payload) {
        auto* s = static_cast<WaitState*>(ud);
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->done   = true;
            s->status = status;
            if (payload && payload->data && payload->size > 0) {
                s->payload.assign(payload->data, static_cast<size_t>(payload->size));
            }
        }
        s->cv.notify_one();
    };

    const auto     opSv   = PluginStringView::from(op.data(), op.size());
    const auto     argsSv = PluginStringView::from(argsJson.data(), argsJson.size());
    PluginxxString err{nullptr, 0};
    int64_t        requestId = 0;
    auto*          handle = net->call(host, &opSv, &argsSv, &requestId, cb, &state, &err);
    if (handle == nullptr) {
        result.code = PLUGINXX_RESULT_ERR_STATE;
        if (err.data) {
            result.json.assign(err.data, static_cast<size_t>(err.size));
            PluginString::free(host, &err);
        }
        return result;
    }
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.cv.wait(lock, [&state]() {
            return state.done;
        });
    }
    result.code = PLUGINXX_RESULT_OK;
    if (state.status == PLUGINXX_OPERATOR_OK) {
        result.json = std::move(state.payload);
    } else if (state.status == PLUGINXX_OPERATOR_CANCELED) {
        result.json = R"({"ok":false,"error":"canceled"})";
    } else {
        result.json = state.payload.empty() ? std::string{R"({"ok":false,"error":"failed"})"}
                                            : std::move(state.payload);
    }
    return result;
}

/// 一次 WebSocket 往返的交互状态
///
/// 网络事件在插件执行线程到达 (广播回调), 发起往返的工作线程在这上面等条件变量;
/// `alive` 在退订前置 false, 之后到达的发布在主程序侧就不会再回调进来。
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

/// 订阅结果: 完成回调写回, 发起订阅的工作线程在 [CdpSubWait::cv] 上等
struct CdpSubWait {
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    done = false;
    PluginxxSubscription*   sub  = nullptr;
};

/// 订阅完成回调的上下文 (**回调自己释放**)
///
/// 第一个字段是“不是主程序同步等待”的标记: 主程序会按这个指针读 8 字节判断
/// 要不要就地回调。
struct CdpSubPending {
    uint64_t    magic = 0;
    CdpSubWait* wait  = nullptr;
};

/// 订阅完成回调 (插件执行线程): 句柄以十进制文本放在 `payload` 里交回
void PLUGINXX_CALL onCdpSubscribed(void* ud, int32_t status, const PluginxxStringView* payload) {
    std::unique_ptr<CdpSubPending> keep(static_cast<CdpSubPending*>(ud));
    if (!keep || keep->wait == nullptr) {
        return;
    }
    PluginxxSubscription* sub = nullptr;
    if (status == PLUGINXX_OPERATOR_OK && payload != nullptr && payload->data != nullptr
        && payload->size > 0) {
        sub = pluginxx::pluginxxSubscriptionFromText(
            std::string_view{payload->data, static_cast<size_t>(payload->size)}
        );
    }
    {
        std::lock_guard<std::mutex> lock(keep->wait->mtx);
        keep->wait->sub  = sub;
        keep->wait->done = true;
    }
    keep->wait->cv.notify_all();
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
    const auto result = netCallWait(
        ctx.host,
        ctx.iface.net,
        ctx.iface.scheduler,
        pluginxx::net_op::httpFetch,
        options.toArgsJson(url)
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
    agentxx::plugin::declarePluginBroadcastNoWait(ctx.host,
        ctx.iface.broadcasts,
        topic,
        /*anyonePublish=*/false,
        /*cacheLatest=*/false
    );
    PluginxxSubscription* sub = nullptr;
    {
        // 订阅走“提交 + 完成回调”: 句柄只能经 payload 交回。完成回调派发在插件执行线程上,
        // 这里不是那条线程, 所以可以等它; 提交被拒时不会有回调。
        CdpSubWait    subWait;
        auto*         pending = new CdpSubPending{0, &subWait};
        const auto    topicSv = PluginStringView::from(topic.data(), topic.size());
        PluginxxString err{nullptr, 0};
        auto*         op = ctx.iface.broadcasts->subscribe_broadcast(
            ctx.host,
            nullptr,
            &topicSv,
            &onCdpNetEvent,
            &state,
            nullptr,
            &onCdpSubscribed,
            pending,
            &err
        );
        if (op == nullptr) {
            delete pending;
            if (err.data != nullptr) {
                PluginString::free(ctx.host, &err);
            }
            return {};
        }
        std::unique_lock<std::mutex> lock(subWait.mtx);
        subWait.cv.wait(lock, [&subWait]() {
            return subWait.done;
        });
        sub = subWait.sub;
    }
    if (!sub) {
        return {};
    }

    // 退订与“置 alive=false”成对出现: 置位后再退订, 之后到达的发布不会再回调进来
    auto teardown = [&]() {
        {
            std::lock_guard<std::mutex> lock(state.mtx);
            state.alive = false;
        }
        // 退订是提交即返回: 调用方不在这条线程上等主程序的簿记
        agentxx::plugin::unsubscribePluginBroadcastNoWait(ctx.host, ctx.iface.broadcasts, sub);
    };

    std::string out;
    // connect / send 要完整结果 (`{ok:false,error:"not_connected"}` 这类), 所以等结果
    auto connectArgs          = utilxx_base::Json::object();
    connectArgs["url"]        = std::string{url};
    connectArgs["id"]         = state.connId;
    connectArgs["timeoutMs"]  = static_cast<int64_t>(waitMs);
    const auto connectResult = netCallWait(
        ctx.host,
        ctx.iface.net,
        ctx.iface.scheduler,
        pluginxx::net_op::wsConnect,
        connectArgs.dump()
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

    {
        auto sendArgs      = utilxx_base::Json::object();
        sendArgs["id"]     = state.connId;
        sendArgs["text"]   = std::string{message};
        const auto sent    = netCallWait(
            ctx.host,
            ctx.iface.net,
            ctx.iface.scheduler,
            pluginxx::net_op::wsSend,
            sendArgs.dump()
        );
        if (!sent.ok()) {
            netWsClose(ctx.host, ctx.iface.net, state.connId, "send-failed");
            teardown();
            return {};
        }
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
                agentxx::plugin::publishPluginBroadcastNoWait(ctx->host, ctx->iface.broadcasts, topicSv, payloadSv);
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

static void textSelectionAgentStart(
    TextSelectionPluginCtx&       ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    ctx.holder.ctx = &ctx;
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

/// 挂载事务 (**协程形态**): 取工具提示词 + 建 schema + 注册工具
static pluginxx::Task<void> textSelectionAgentSetup(
    TextSelectionPluginCtx& ctx,
    agentxx::plugin::AgentRef ref
) {
    auto schemaBuilder = co_await agentxx::plugin::schemaOf(ctx, "agentxx_text_selection_monitor");
    auto schema
        = schemaBuilder
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

    co_await tool(
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
    co_return;
}

static void* textSelectionAgentMount(
    TextSelectionPluginCtx&            ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    err
) {
    if (!notify) {
        agentxx::plugin::PluginString::set(
            ctx.host,
            err,
            "text_selection_monitor mount: notify required"
        );
        return nullptr;
    }
    if (!ctx.iface.tasks || !ctx.iface.tasks->register_task) {
        agentxx::plugin::PluginString::set(
            ctx.host,
            err,
            "text_selection_monitor mount: host has no pluginxx.tasks (spawn unavailable)"
        );
        return nullptr;
    }
    auto later = agentxx::plugin::finishLater(notify);
    // 工具注册交给协程: schema/提示词读取与注册逐条 co_await, 全部生效后再放行挂载
    const agentxx::plugin::AgentRef agentRef(*spec);
    ctx.spawn([later, agentRef](TextSelectionPluginCtx& c, agentxx::plugin::OpCtl&)
                  -> pluginxx::Task<void> {
        try {
            co_await textSelectionAgentSetup(c, agentRef);
            later.ok();
        } catch (const std::exception& e) {
            later.failed(e.what());
        }
        co_return;
    });
    return later.provider();
}

static void textSelectionAgentUnmount(
    TextSelectionPluginCtx&,
    void*,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

/// stop: 停止监听并摘除监听器 (可重复调用), 之后主程序才会调用 destroy
static void    textSelectionAgentStop(TextSelectionPluginCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    try {
        ctx.holder.stop();
    } catch (...) {
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

AGENTXX_AGENT_PLUGIN_EXPORT(
    TextSelectionPluginCtx,
    "agentxx_text_selection_monitor",
    "1.0.0",
    "System-wide text selection monitor event stream",
    textSelectionAgentStart,
    textSelectionAgentStop,
    textSelectionAgentMount,
    textSelectionAgentUnmount
);
