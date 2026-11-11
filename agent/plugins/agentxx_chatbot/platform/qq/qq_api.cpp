#include "qq_api.h"

#include "../../chatbot_net.h"

#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include <chrono>

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

/// token 提前刷新的余量 (设计文档 §12.1: 剩余 < 90 秒开始刷新)
constexpr int64_t kTokenRefreshLeadMs = 90 * 1000;

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

std::string joinUrl(std::string_view base, std::string_view path) {
    std::string out{base};
    if (!out.empty() && out.back() == '/' && !path.empty() && path.front() == '/') {
        out.pop_back();
    }
    out += path;
    return out;
}

/// 请求头 → JSON 对象文本 (网络表的入参形态)
std::string headersJsonOf(const std::vector<std::pair<std::string, std::string>>& headers) {
    if (headers.empty()) {
        return {};
    }
    utilxx_base::Json json = utilxx_base::Json::object();
    for (const auto& [name, value] : headers) {
        json[name] = value;
    }
    return json.dump();
}

/// 网络层结果 → 接口结果 (字段一一对应)
QqResponse toResponse(const ChatbotHttpResult& result) {
    QqResponse out;
    out.status = result.status;
    out.body   = result.body;
    out.error  = result.error;
    return out;
}

} // namespace

std::optional<utilxx_base::Json> QqResponse::json() const {
    if (body.empty()) {
        return std::nullopt;
    }
    try {
        return utilxx_base::Json::parse(body);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

QqApi::QqApi(
    const ChatbotNet* net,
    asio::any_io_executor ex,
    std::string           appId,
    std::string           appSecret,
    std::string           apiBase
) :
    net_(net),
    ex_(std::move(ex)),
    appId_(std::move(appId)),
    appSecret_(std::move(appSecret)),
    apiBase_(apiBase.empty() ? std::string{"https://api.bot.qq.com"} : std::move(apiBase)) {}

void QqApi::invalidateToken() {
    token_.clear();
    tokenExpireAtMs_ = 0;
}

asio::awaitable<QqResponse> QqApi::doRequest(
    std::string_view                                        method,
    std::string_view                                        url,
    std::string_view                                        body,
    std::string_view                                        contentType,
    const std::vector<std::pair<std::string, std::string>>& headers,
    bool                                                    binaryBody
) {
    if (net_ == nullptr) {
        QqResponse out;
        out.error = "插件没有网络入口";
        co_return out;
    }
    ChatbotHttpRequest request;
    request.method      = std::string{method};
    request.contentType = std::string{contentType};
    request.headersJson = headersJsonOf(headers);
    // 上传与带大 JSON 的响应都要留余量; 这是整次请求的预算 (连接 + 收完正文)
    request.timeoutMs = 60000;
    request.maxBytes  = 32 * 1024 * 1024;
    if (binaryBody) {
        // 二进制不能进 JSON 字符串: 交给主程序按 base64 还原
        request.bodyBase64 = utilxx_base::base64Encode(body);
    } else {
        request.body = std::string{body};
    }
    co_return toResponse(co_await net_->fetch(ex_, std::string{url}, request));
}

asio::awaitable<std::optional<std::string>> QqApi::accessToken(bool force) {
    if (!force && !token_.empty() && steadyNowMs() + kTokenRefreshLeadMs < tokenExpireAtMs_) {
        co_return token_;
    }
    utilxx_base::Json body = utilxx_base::Json::object();
    body["appId"]        = appId_;
    body["clientSecret"] = appSecret_;
    const auto url = joinUrl(apiBase_, "/app/getAppAccessToken");
    auto response  = co_await doRequest(
        "POST",
        url,
        body.dump(),
        "application/json",
        {{"Accept", "application/json"}}
    );
    if (!response.ok()) {
        XX_LOGE(
            "[{}] 取 access_token 失败 (status {}, {}): {}",
            kLogTag,
            response.status,
            response.error,
            response.body.substr(0, 200)
        );
        co_return std::nullopt;
    }
    auto json = response.json();
    if (!json.has_value() || !json->is_object()) {
        XX_LOGE("[{}] access_token 响应不是 JSON: {}", kLogTag, response.body.substr(0, 200));
        co_return std::nullopt;
    }
    const auto token = json->value("access_token", std::string{});
    if (token.empty()) {
        XX_LOGE("[{}] access_token 为空: {}", kLogTag, response.body.substr(0, 200));
        co_return std::nullopt;
    }
    // `expires_in` 是**字符串**数字 ("7200"), 解析时两种形态都接受
    int64_t expiresInSec = 7200;
    if (json->contains("expires_in")) {
        const auto& value = (*json)["expires_in"];
        if (value.is_string()) {
            try {
                expiresInSec = std::stoll(value.get<std::string>());
            } catch (const std::exception&) {
                expiresInSec = 7200;
            }
        } else if (value.is_number_integer() || value.is_number_unsigned()) {
            expiresInSec = value.get<long long>();
        }
    }
    token_           = token;
    tokenExpireAtMs_ = steadyNowMs() + std::max<int64_t>(60, expiresInSec) * 1000;
    XX_LOGI(
        "[{}] access_token 已更新 (有效期 {} 秒, 到期前 90 秒自动刷新)",
        kLogTag,
        expiresInSec
    );
    co_return token_;
}

asio::awaitable<QqResponse> QqApi::gatewayUrl() {
    auto token = co_await accessToken();
    if (!token.has_value()) {
        QqResponse out;
        out.error = "取 access_token 失败";
        co_return out;
    }
    co_return co_await doRequest(
        "GET",
        joinUrl(apiBase_, "/gateway/bot"),
        {},
        "application/json",
        {{"Authorization", "QQBot " + *token}, {"Accept", "application/json"}}
    );
}

asio::awaitable<QqResponse> QqApi::request(
    std::string_view                                       method,
    std::string_view                                       path,
    const utilxx_base::Json&                               body,
    const std::vector<std::pair<std::string, std::string>>& extraHeaders
) {
    auto token = co_await accessToken();
    if (!token.has_value()) {
        QqResponse out;
        out.error = "取 access_token 失败";
        co_return out;
    }
    std::vector<std::pair<std::string, std::string>> headers{{"Accept", "application/json"}};
    headers.emplace_back("Authorization", "QQBot " + *token);
    headers.emplace_back("Content-Type", "application/json");
    for (const auto& [name, value] : extraHeaders) {
        headers.emplace_back(name, value);
    }
    auto response = co_await doRequest(
        method,
        joinUrl(apiBase_, path),
        body.is_null() ? std::string{"{}"} : body.dump(),
        "application/json",
        headers
    );
    // 鉴权失败: 强制刷新 token 后重试一次
    if (response.status == 401 || response.status == 403) {
        XX_LOGW(
            "[{}] 接口返回 {}, 强制刷新 access_token 后重试一次 ({})",
            kLogTag,
            response.status,
            std::string{path}
        );
        invalidateToken();
        auto refreshed = co_await accessToken(true);
        if (refreshed.has_value()) {
            headers.erase(
                std::remove_if(
                    headers.begin(),
                    headers.end(),
                    [](const std::pair<std::string, std::string>& item) {
                        return item.first == "Authorization";
                    }
                ),
                headers.end()
            );
            headers.emplace_back("Authorization", "QQBot " + *refreshed);
            response = co_await doRequest(
                method,
                joinUrl(apiBase_, path),
                body.is_null() ? std::string{"{}"} : body.dump(),
                "application/json",
                headers
            );
        }
    }
    co_return response;
}

asio::awaitable<QqResponse> QqApi::sendMessage(
    ChatType         type,
    std::string_view chatId,
    std::string_view text,
    std::string_view passiveMsgId,
    int32_t          passiveSeq,
    std::string_view messageReference,
    int              msgType,
    std::string_view mediaFileInfo,
    std::string_view eventId
) {
    utilxx_base::Json body = utilxx_base::Json::object();
    body["msg_type"]       = msgType;
    switch (msgType) {
        case 2:
            body["markdown"] = utilxx_base::Json{{"content", std::string{text}}};
            break;
        case 7: {
            body["media"] = utilxx_base::Json{{"file_info", std::string{mediaFileInfo}}};
            if (!text.empty()) {
                body["content"] = std::string{text};
            }
            break;
        }
        default:
            body["content"] = std::string{text};
            break;
    }
    if (!passiveMsgId.empty()) {
        body["msg_id"] = std::string{passiveMsgId};
        if (passiveSeq > 0) {
            body["msg_seq"] = passiveSeq;
        }
    }
    if (!messageReference.empty()) {
        body["message_reference"] = utilxx_base::Json{{"message_id", std::string{messageReference}}};
    }
    if (!eventId.empty()) {
        body["event_id"] = std::string{eventId};
    }

    const std::string path
        = type == ChatType::Group
              ? "/v2/groups/" + std::string{chatId} + "/messages"
              : "/v2/users/" + std::string{chatId} + "/messages";
    co_return co_await request("POST", path, body);
}

asio::awaitable<QqResponse> QqApi::recallMessage(
    ChatType         type,
    std::string_view chatId,
    std::string_view messageId
) {
    const std::string path
        = type == ChatType::Group
              ? "/v2/groups/" + std::string{chatId} + "/messages/" + std::string{messageId}
              : "/v2/users/" + std::string{chatId} + "/messages/" + std::string{messageId};
    co_return co_await request("DELETE", path, utilxx_base::Json::object());
}

asio::awaitable<QqResponse> QqApi::putToPresignedUrl(
    std::string_view url,
    std::string_view binaryBody
) {
    // 预签名 URL 自带鉴权参数, 不再带 Authorization 头
    co_return co_await doRequest(
        "PUT",
        url,
        binaryBody,
        "application/octet-stream",
        {},
        /*binaryBody=*/true
    );
}

} // namespace chatbot
} // namespace agentxx
