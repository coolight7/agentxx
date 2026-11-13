#include "chatbot_net.h"

#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

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
    if (!asyncReady()) {
        out.error = "主程序没有提供网络能力 (check `.net` 表的 call_async)";
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

    auto result = co_await agentxx::plugin::asyncNetFetch(
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

    auto result = agentxx::plugin::netCallBlocking(
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

PluginxxSubscription* ChatbotNet::subscribeEvents(
    ChatbotNetEventHandler handler,
    void*                  ud
) const {
    if (!host || !broadcasts || !broadcasts->subscribe_broadcast || eventTopic.empty() || !handler) {
        return nullptr;
    }
    // 频道要先声明才能订阅; 网络事件的频道归本插件 (主程序代发时按属主身份发布)
    agentxx::plugin::declarePluginBroadcast(
        host,
        broadcasts,
        eventTopic,
        /*anyonePublish=*/false,
        /*cacheLatest=*/false
    );
    return agentxx::plugin::subscribePluginBroadcast(
        host,
        broadcasts,
        nullptr,
        eventTopic,
        handler,
        ud
    );
}

} // namespace chatbot
} // namespace agentxx
