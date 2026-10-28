#pragma once

/// QQ openapi 客户端: access token / 发消息 / 撤回 / 网关地址
///
/// 事实与结论见 `resource/history/chatbot/plan.md` §3.1 / §3.2 / §3.4:
/// - token: `POST /app/getAppAccessToken` `{appId, clientSecret}` →
///   `{access_token, expires_in}`; 有效期 7200 秒且不自动续, 剩余不足 90 秒时刷新;
/// - 之后的请求都带 `Authorization: QQBot <access_token>`;
/// - 发消息: `POST /v2/groups/{group_openid}/messages` 与
///   `POST /v2/users/{user_openid}/messages`;
/// - 被动回复: `msg_id` + `msg_seq` (同一个 msg_id+msg_seq 重复发送会失败)。
///
/// 线程: 全部协程在插件 io 线程上跑 (由 `QqPlatform` 的 io_context 驱动)。
#include "../../chatbot_types.h"

#include "asio/awaitable.hpp"
#include "utilxx_base/json.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace asio = ::boost::asio;

namespace agentxx {
namespace chatbot {

/// 一次接口调用的结果
struct QqResponse {
    /// HTTP 状态 (0 = 没走到 HTTP, 见 `error`)
    int         status = 0;
    std::string body;
    /// 传输层错误 (连接失败 / 超时 / TLS)
    std::string error;

    bool        transportOk() const noexcept {
        return error.empty() && status != 0;
    }
    bool        ok() const noexcept {
        return transportOk() && status >= 200 && status < 300;
    }
    /// 尝试把 body 当 JSON 解析 (失败返回 nullopt)
    std::optional<utilxx_base::Json> json() const;
};

/// QQ openapi 客户端
class QqApi {
public:

    QqApi(asio::any_io_executor ex, std::string appId, std::string appSecret, std::string apiBase);

    /// 取 access token (内部缓存; 剩余 < 90 秒时自动刷新)
    ///
    /// - `force`: 强制刷新 (遇到 401/403 时用)
    asio::awaitable<std::optional<std::string>> accessToken(bool force = false);

    /// 取网关地址 (`GET /gateway/bot`)
    asio::awaitable<QqResponse> gatewayUrl();

    /// 发一条消息
    ///
    /// - 群聊走 `/v2/groups/{group_openid}/messages`, 单聊走 `/v2/users/{uid}/messages`
    /// - `passiveMsgId` 非空 = 被动回复 (还要给 `passiveSeq`)
    /// - `msgType`: 0 文本 / 2 markdown / 7 富媒体 (`mediaFileInfo` 必填)
    asio::awaitable<QqResponse> sendMessage(
        ChatType         type,
        std::string_view chatId,
        std::string_view text,
        std::string_view passiveMsgId,
        int32_t          passiveSeq,
        std::string_view messageReference,
        int              msgType = 0,
        std::string_view mediaFileInfo = {},
        std::string_view eventId = {}
    );

    /// 撤回 (`DELETE .../messages/{message_id}`, 发送后 2 分钟内有效)
    asio::awaitable<QqResponse> recallMessage(
        ChatType         type,
        std::string_view chatId,
        std::string_view messageId
    );

    /// 通用 JSON 请求 (上传用)
    asio::awaitable<QqResponse> request(
        std::string_view                 method,
        std::string_view                 path,
        const utilxx_base::Json&         body,
        const std::vector<std::pair<std::string, std::string>>& extraHeaders = {}
    );

    /// 上传一个分片到预签名 URL (PUT, 不带 Authorization)
    asio::awaitable<QqResponse>
        putToPresignedUrl(std::string_view url, std::string_view binaryBody);

    /// 带查询参数的 GET (下载附件)
    asio::awaitable<QqResponse>
        getUrl(std::string_view url, std::string maxBytesNotice = {}, size_t maxBytes = 0);

    const std::string& apiBase() const noexcept {
        return apiBase_;
    }

    /// 立即作废缓存的 token (收到鉴权失败时用)
    void invalidateToken();

    /// 当前缓存的 token (可能为空; 不触发网络请求)
    std::string cachedToken() const {
        return token_;
    }

private:

    asio::awaitable<QqResponse> doRequest(
        std::string_view                                        method,
        std::string_view                                        url,
        std::string_view                                        body,
        std::string_view                                        contentType,
        const std::vector<std::pair<std::string, std::string>>& headers
    );

    asio::any_io_executor ex_;
    std::string           appId_;
    std::string           appSecret_;
    std::string           apiBase_;
    std::string           token_;
    int64_t               tokenExpireAtMs_ = 0;
};

} // namespace chatbot
} // namespace agentxx
