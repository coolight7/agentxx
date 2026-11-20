#include "chatbot_net.h"

#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <system_error>

namespace fs = std::filesystem;

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

/// 在本线程上等一次网络调用的结果 (回调 + 条件变量)
///
/// - 主程序把完成回调派发在主程序 io 线程上, 所以在这里等不会自锁;
/// - 在主程序 io 线程上等会把整个主程序卡住, 这种情况直接按错误返回;
/// - 插件自己的协程里不要这么写, 那里用 `co_await netFetch(...)`。
agentxx::plugin::NetResult netCallWait(
    const PluginxxHost*           host,
    const PluginxxNetIface*       net,
    const PluginxxSchedulerIface* scheduler,
    std::string_view              op,
    std::string_view              argsJson
) {
    agentxx::plugin::NetResult result;
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

    const auto     opSv   = agentxx::plugin::PluginStringView::from(op.data(), op.size());
    const auto     argsSv = agentxx::plugin::PluginStringView::from(argsJson.data(), argsJson.size());
    PluginxxString err{nullptr, 0};
    int64_t        requestId = 0;
    auto*          handle = net->call(host, &opSv, &argsSv, &requestId, cb, &state, &err);
    if (handle == nullptr) {
        result.code = PLUGINXX_RESULT_ERR_STATE;
        if (err.data) {
            result.json.assign(err.data, static_cast<size_t>(err.size));
            agentxx::plugin::PluginString::free(host, &err);
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

/// 主程序结果 JSON → 平台层结构
///
/// 两层状态: 表入口 ([plugin::NetResult::accepted]) 是"受理没受理", 结果 JSON 里的
/// `ok` 才是"请求成不成功"。`http.fetch` 拿到任何 HTTP 响应都是 `ok:true`,
/// 所以 `status == 0` 就是传输层失败 (连不上 / 超时 / TLS)。
ChatbotHttpResult httpResultOf(const agentxx::plugin::NetResult& result) {
    ChatbotHttpResult out;
    if (!result.accepted()) {
        out.error = std::string{"网络入口不可用 ("}
                    + std::string{agentxx::plugin::detail::netCodeName(result.code)} + ")";
        return out;
    }
    out.status = static_cast<int>(result.status());
    out.body   = result.str("body");
    if (out.status == 0) {
        out.error = result.error();
    }
    return out;
}

} // namespace

std::optional<utilxx_base::Json> ChatbotHttpResult::json() const {
    if (body.empty()) {
        return std::nullopt;
    }
    try {
        return utilxx_base::Json::parse(body);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

asio::awaitable<ChatbotHttpResult> ChatbotNet::fetch(
    asio::any_io_executor     ex,
    std::string               url,
    const ChatbotHttpRequest& request
) const {
    ChatbotHttpResult out;
    if (!ready()) {
        out.error = "主程序没有提供网络能力 (check `.net` 表的 call)";
        co_return out;
    }

    agentxx::plugin::NetHttpOptions options;
    options.method     = request.method.empty() ? std::string{"GET"} : request.method;
    options.body       = request.body;
    options.bodyBase64 = request.bodyBase64;
    options.contentType = request.contentType;
    if (!request.headersJson.empty()) {
        options.headers = request.headersJson;
    }
    options.timeoutMs = request.timeoutMs;
    options.maxBytes  = request.maxBytes;

    auto result = co_await agentxx::plugin::netFetch(
        std::move(ex),
        host,
        net,
        url,
        options,
        asio::use_awaitable
    );
    co_return httpResultOf(result);
}

std::string ChatbotNet::download(
    std::string_view   url,
    const std::string& targetPath,
    int64_t            maxBytes,
    int32_t            timeoutMs
) const {
    if (!ready()) {
        return "主程序没有提供网络能力";
    }
    if (url.empty() || targetPath.empty()) {
        return "下载参数不完整";
    }

    // 落盘文件名用目标路径的文件名: 主程序按它写到自己的下载目录, 之后我们再搬走
    std::string fileName;
    {
        const fs::path target = utilxx_base::utf8ToPath(targetPath);
        fileName              = utilxx_base::pathToUtf8Generic(target.filename());
    }

    utilxx_base::Json args = utilxx_base::Json::object();
    args["url"]            = std::string{url};
    args["timeoutMs"]      = static_cast<int64_t>(timeoutMs > 0 ? timeoutMs : 300000);
    if (!fileName.empty()) {
        args["fileName"] = fileName;
    }
    if (maxBytes > 0) {
        args["maxBytes"] = maxBytes;
    }

    auto result = netCallWait(
        host,
        net,
        scheduler,
        pluginxx::net_op::httpDownload,
        args.dump()
    );
    if (!result.accepted()) {
        // 表入口拒绝时结果 JSON 里带可读原因 (例如 write_failed / 只支持 GET)
        std::string reason;
        const auto  doc = agentxx::plugin::detail::parseNetJson(result.json);
        if (doc.has_value() && doc->is_object()) {
            reason = doc->value("message", doc->value("error", std::string{}));
        }
        if (reason.empty()) {
            reason = std::string{agentxx::plugin::detail::netCodeName(result.code)};
        }
        return "网络入口不可用 (" + reason + ")";
    }
    if (!result.ok()) {
        return result.error();
    }
    const auto status = result.status();
    if (status < 200 || status >= 300) {
        return "HTTP " + std::to_string(status);
    }
    const std::string downloaded = result.str("path");
    if (downloaded.empty()) {
        return "下载结果里没有落盘路径 (主程序可能没有数据目录)";
    }

    // 从主程序给的下载目录搬到插件自己的媒体目录: 同盘就是改名, 跨盘才复制
    std::error_code ec;
    fs::rename(utilxx_base::utf8ToPath(downloaded), utilxx_base::utf8ToPath(targetPath), ec);
    if (ec) {
        ec.clear();
        fs::copy_file(
            utilxx_base::utf8ToPath(downloaded),
            utilxx_base::utf8ToPath(targetPath),
            fs::copy_options::overwrite_existing,
            ec
        );
        if (ec) {
            return "搬移下载文件失败: " + ec.message();
        }
        std::error_code ignore;
        fs::remove(utilxx_base::utf8ToPath(downloaded), ignore);
    }
    XX_LOGD("[{}] 附件已下载到 {} ({} 字节)", kLogTag, targetPath, result.json.size());
    return {};
}

bool ChatbotNet::subscribeEvents(
    ChatbotNetEventHandler handler,
    void*                  ud,
    NetSubCallback         onReady
) const {
    if (!host || !broadcasts || !broadcasts->subscribe_broadcast || eventTopic.empty() || !handler) {
        return false;
    }
    // 频道要先声明才能订阅; 网络事件的频道归本插件 (主程序代发时按属主身份发布)
    agentxx::plugin::declarePluginBroadcastNoWait(
        host,
        broadcasts,
        eventTopic,
        /*anyonePublish=*/false,
        /*cacheLatest=*/false
    );
    const auto idSv = agentxx::plugin::PluginStringView::from(
        eventTopic.data(),
        eventTopic.size()
    );
    /// 完成回调上下文: 第一个字段放"不是同步桥"的标记 (主程序会按指针读 8 字节)
    struct Pending {
        uint64_t       magic = 0;
        NetSubCallback done;
    };
    auto*          pending = new Pending{0, std::move(onReady)};
    PluginxxString err{nullptr, 0};
    auto*          op = broadcasts->subscribe_broadcast(
        host,
        nullptr,
        &idSv,
        handler,
        ud,
        nullptr,
        [](void* cbUd, int32_t status, const PluginxxStringView* payload) {
            std::unique_ptr<Pending> keep(static_cast<Pending*>(cbUd));
            PluginxxSubscription*    sub = nullptr;
            if (status == PLUGINXX_OPERATOR_OK && payload && payload->data && payload->size > 0) {
                sub = pluginxx::pluginxxSubscriptionFromText(
                    std::string_view{payload->data, static_cast<size_t>(payload->size)}
                );
            }
            if (keep->done) {
                keep->done(sub);
            }
        },
        pending,
        &err
    );
    if (!op) {
        std::unique_ptr<Pending> keep(pending);
        if (err.data) {
            XX_LOGW("[{}] 订阅网络事件被拒: {}", kLogTag, std::string_view{err.data, err.size});
            agentxx::plugin::PluginString::free(host, &err);
        }
        return false;
    }
    return true;
}

bool ChatbotNet::unsubscribeEvents(PluginxxSubscription* sub) const {
    // 阻塞形态退订 (调用方是插件 io 线程, 不是主程序 io 线程)
    return agentxx::plugin::unsubscribePluginBroadcastNoWait(host, broadcasts, sub);
}

} // namespace chatbot
} // namespace agentxx
