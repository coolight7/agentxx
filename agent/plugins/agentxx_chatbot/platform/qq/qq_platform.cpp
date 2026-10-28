#include "qq_platform.h"

#include "../../chatbot_core.h"
#include "utilxx/http_client.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace agentxx {
namespace chatbot {

namespace {

namespace fs = std::filesystem;

constexpr const char* kLogTag = "chatbot";

/// 附件落盘上限 (超过不下载; 只是不给模型, 文本里仍写路径与大小)
constexpr uint64_t kMaxDownloadBytes = 200ULL * 1024 * 1024;

int64_t unixNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()
    )
        .count();
}

/// RFC3339 时间 → 毫秒 (解析失败返回 0)
int64_t parseRfc3339Ms(std::string_view text) {
    if (text.size() < 19) {
        return 0;
    }
    std::tm tmBuf{};
    int     year = 0;
    int     month = 0;
    int     day = 0;
    int     hour = 0;
    int     minute = 0;
    int     second = 0;
    if (std::sscanf(
            std::string{text}.c_str(),
            "%d-%d-%dT%d:%d:%d",
            &year,
            &month,
            &day,
            &hour,
            &minute,
            &second
        )
        != 6) {
        return 0;
    }
    tmBuf.tm_year = year - 1900;
    tmBuf.tm_mon  = month - 1;
    tmBuf.tm_mday = day;
    tmBuf.tm_hour = hour;
    tmBuf.tm_min  = minute;
    tmBuf.tm_sec  = second;
#if XX_IS_WIN_D
    const std::time_t seconds = _mkgmtime(&tmBuf);
#else
    const std::time_t seconds = timegm(&tmBuf);
#endif
    if (seconds <= 0) {
        return 0;
    }
    return static_cast<int64_t>(seconds) * 1000;
}

/// `message_scene.ext` 里的 `key=value` 串解析 (`msg_idx=REFIDX_...\tref_msg_idx=...`)
std::string sceneExtValue(std::string_view ext, std::string_view key) {
    size_t pos = 0;
    while (pos < ext.size()) {
        auto       end = ext.find_first_of("\t;&", pos);
        const auto item = ext.substr(pos, end == std::string_view::npos ? std::string_view::npos
                                                                        : end - pos);
        const auto eq   = item.find('=');
        if (eq != std::string_view::npos && item.substr(0, eq) == key) {
            return std::string{item.substr(eq + 1)};
        }
        if (end == std::string_view::npos) {
            break;
        }
        pos = end + 1;
    }
    return {};
}

std::string fileNameOfUrl(std::string_view url) {
    auto path = url;
    const auto query = path.find('?');
    if (query != std::string_view::npos) {
        path = path.substr(0, query);
    }
    const auto slash = path.find_last_of('/');
    return slash == std::string_view::npos ? std::string{"attachment"}
                                           : std::string{path.substr(slash + 1)};
}

} // namespace

QqPlatform::QqPlatform(asio::any_io_executor ex, std::string dataDir) :
    ex_(std::move(ex)),
    dataDir_(std::move(dataDir)) {}

QqPlatform::~QqPlatform() {
    stop();
}

PlatformCaps QqPlatform::caps() const {
    PlatformCaps caps;
    // 群支持 markdown 发送, 单聊「可发不收」: 编排层按 chatType 再收一次
    caps.markdown           = true;
    caps.passiveReply       = true;
    caps.quoteReply         = true;
    caps.multiReply         = true;
    caps.textLimit          = 0; ///< 平台没给出官方上限, 用 behavior.messageSplitLimit
    caps.maxAttachments     = 1; ///< 一条消息一个富媒体 (msg_type=7)
    caps.mentionByEventType = true;
    return caps;
}

bool QqPlatform::start(const AccountConfig& cfg, PlatformEventSink& sink) {
    std::lock_guard lock(mutex_);
    config_  = cfg;
    sink_    = &sink;
    stopped_ = false;
    state_   = PlatformState::Connecting;
    if (cfg.appId.empty() || cfg.appSecret.empty()) {
        state_       = PlatformState::Disabled;
        stateReason_ = "缺少 appId / appSecret";
        XX_LOGE("[{}] 账号 '{}' 启动失败: {}", kLogTag, cfg.id, stateReason_);
        return false;
    }
    api_ = std::make_unique<QqApi>(ex_, cfg.appId, cfg.appSecret, cfg.apiBase);

    // 媒体目录: {data_dir}/chatbot/media/{accountId}
    if (!dataDir_.empty()) {
        const fs::path root = utilxx_base::utf8ToPath(dataDir_) / "chatbot" / "media" / cfg.id;
        std::error_code ec;
        fs::create_directories(root, ec);
        if (!ec) {
            mediaRoot_ = utilxx_base::pathToUtf8Generic(root);
        }
    }
    uploader_ = std::make_unique<QqMediaUploader>(*api_, cfg.id);

    gateway_ = std::make_unique<QqGateway>(ex_, *api_);
    QqGatewayHooks hooks;
    hooks.onState = [this](PlatformState state, std::string reason) {
        {
            std::lock_guard guard(mutex_);
            state_       = state;
            stateReason_ = reason;
        }
        PlatformEventSink* sinkPtr = nullptr;
        {
            std::lock_guard guard(mutex_);
            sinkPtr = sink_;
        }
        if (sinkPtr != nullptr && sinkPtr->onStateChanged) {
            sinkPtr->onStateChanged(state, std::move(reason));
        }
    };
    hooks.onReady = [this](std::string_view sessionId, int64_t sequence) {
        XX_LOGI(
            "[{}] 账号 '{}' 网关就绪 (session '{}', seq {})",
            kLogTag,
            config_.id,
            std::string{sessionId},
            sequence
        );
    };
    hooks.onDispatch = [this](std::string_view eventName, const utilxx_base::Json& data) {
        InboundEvent event;
        if (!normalize(eventName, data, event)) {
            return;
        }
        event.accountId = config_.id;
        {
            std::lock_guard guard(mutex_);
            ++inboundCount_;
        }
        PlatformEventSink* sinkPtr = nullptr;
        {
            std::lock_guard guard(mutex_);
            sinkPtr = sink_;
        }
        if (sinkPtr != nullptr && sinkPtr->onInbound) {
            sinkPtr->onInbound(std::move(event));
        }
    };
    gateway_->start(cfg, std::move(hooks));
    XX_LOGI(
        "[{}] 账号 '{}' 已启动 (appId {}, sandbox={})",
        kLogTag,
        cfg.id,
        cfg.appId.size() > 4 ? "***" + cfg.appId.substr(cfg.appId.size() - 4) : "***",
        cfg.sandbox
    );
    return true;
}

void QqPlatform::stop() {
    std::lock_guard lock(mutex_);
    if (stopped_) {
        return;
    }
    stopped_ = true;
    sink_    = nullptr;
    if (gateway_) {
        gateway_->stop();
    }
    api_.reset();
    gateway_.reset();
    uploader_.reset();
    state_       = PlatformState::Disabled;
    stateReason_ = "stopped";
}

void QqPlatform::send(OutboundMessage msg, SendResultFn done) {
    if (stopped_) {
        if (done) {
            SendResult result;
            result.ok     = false;
            result.reason = "平台已停止";
            done(result);
        }
        return;
    }
    asio::co_spawn(
        ex_,
        doSend(std::move(msg), std::move(done)),
        asio::detached
    );
}

asio::awaitable<void> QqPlatform::doSend(OutboundMessage msg, SendResultFn done) {
    SendResult result;
    if (!api_) {
        result.reason = "平台未启动";
        if (done) {
            done(result);
        }
        co_return;
    }
    {
        std::lock_guard guard(mutex_);
        ++outboundCount_;
    }

    // ---- 附件: 一条消息一个富媒体 (平台限制), 其余在文本里给路径 ----
    std::string extraText;
    std::string fileInfo;
    int         msgType = msg.markdown ? 2 : 0;
    if (!msg.attachments.empty()) {
        const auto& first = msg.attachments.front();
        const auto  upload = co_await uploader_->upload(first, msg.type, msg.chatId);
        if (upload.ok) {
            fileInfo = upload.fileInfo;
            msgType  = 7;
        } else {
            extraText += "\n(附件 '" + (first.fileName.empty() ? std::string{"附件"} : first.fileName)
                         + "' 上传失败: " + upload.reason + ")";
        }
        for (size_t i = 1; i < msg.attachments.size(); ++i) {
            const auto& rest = msg.attachments[i];
            extraText += "\n(另有附件 '" + rest.fileName + "' 未随本条发送: "
                         + (rest.localPath.empty() ? rest.url : rest.localPath) + ")";
        }
    }

    auto response = co_await api_->sendMessage(
        msg.type,
        msg.chatId,
        msg.text + extraText,
        msg.passiveReplyTo,
        msg.passiveSeq,
        msg.replyToRef,
        msgType,
        fileInfo
    );
    if (!response.ok()) {
        result.ok     = false;
        result.reason = "发送失败 (status " + std::to_string(response.status) + "): "
                        + (response.error.empty() ? response.body.substr(0, 200) : response.error);
        {
            std::lock_guard guard(mutex_);
            ++sendFailCount_;
        }
        XX_LOGW("[{}] 账号 '{}' 发送失败: {}", kLogTag, config_.id, result.reason);
        if (done) {
            done(result);
        }
        co_return;
    }
    auto json = response.json();
    if (json.has_value() && json->is_object()) {
        result.platformMessageId = json->value("id", std::string{});
        if (json->contains("ext_info") && (*json)["ext_info"].is_object()) {
            result.refIdx = (*json)["ext_info"].value("ref_idx", std::string{});
        }
    }
    result.ok = true;
    if (done) {
        done(result);
    }
    co_return;
}

void QqPlatform::downloadAttachment(const std::string& accountId, MediaRef& ref) {
    (void)accountId;
    if (ref.url.empty() || !ref.localPath.empty()) {
        return;
    }
    if (ref.sizeBytes > kMaxDownloadBytes) {
        XX_LOGW(
            "[{}] 附件 '{}' 超过 {} MB, 不下载 (只在文本里给地址)",
            kLogTag,
            ref.fileName,
            kMaxDownloadBytes / (1024 * 1024)
        );
        return;
    }
    if (mediaRoot_.empty()) {
        return; // 没有数据目录: 直给 URL
    }
    const std::string fileName = ref.fileName.empty() ? fileNameOfUrl(ref.url) : ref.fileName;
    // 落盘目录: {data_dir}/chatbot/media/{accountId}/ (按附件大类分子目录, 便于排查)
    const fs::path dir = utilxx_base::utf8ToPath(mediaRoot_) / mediaKindName(ref.kind);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path    target     = dir / (std::to_string(unixNowMs()) + "-" + fileName);
    const std::string targetUtf8 = utilxx_base::pathToUtf8Generic(target);

    const auto& url = ref.url;
    asio::co_spawn(
        ex_,
        [this, url, target, targetUtf8, fileName]() -> asio::awaitable<void> {
            utilxx::RequestConfig config;
            config.connectTimeout   = std::chrono::seconds{20};
            config.readChunkTimeout = std::chrono::seconds{60};
            config.maxResponseBody  = kMaxDownloadBytes;
            auto response = co_await utilxx::HttpClient::getAsync(url, {}, config);
            if (!response.has_value() || response->status < 200 || response->status >= 300) {
                XX_LOGW(
                    "[{}] 附件 '{}' 下载失败 ({}), 保留平台地址",
                    kLogTag,
                    fileName,
                    response.has_value() ? std::to_string(response->status) : response.error()
                );
                co_return;
            }
            std::ofstream out(target, std::ios::binary);
            if (!out) {
                XX_LOGW("[{}] 附件 '{}' 无法写入本地文件", kLogTag, fileName);
                co_return;
            }
            out.write(response->body.data(), static_cast<std::streamsize>(response->body.size()));
            out.close();
            XX_LOGD(
                "[{}] 附件 '{}' 已下载到 {} ({} 字节)",
                kLogTag,
                fileName,
                targetUtf8,
                response->body.size()
            );
        },
        asio::detached
    );
    // 发起下载时就把目标路径写上: 模型读文件时若还没落盘, 会读到"文件不存在",
    // 但比"给一个需要鉴权的平台 URL"更可用 (平台 URL 带有效期内签名)
    ref.localPath = targetUtf8;
}

std::string QqPlatform::roleOf(const utilxx_base::Json& author) {
    if (!author.is_object()) {
        return {};
    }
    const auto role = author.value("member_role", std::string{});
    if (role == "owner" || role == "admin" || role == "member") {
        return role;
    }
    return {};
}

bool QqPlatform::normalize(
    std::string_view         eventName,
    const utilxx_base::Json& data,
    InboundEvent&            out
) const {
    if (eventName != "GROUP_AT_MESSAGE_CREATE" && eventName != "GROUP_MESSAGE_CREATE"
        && eventName != "C2C_MESSAGE_CREATE") {
        // 其它事件 (成员增删/消息接收回执等) 只记 debug
        XX_LOGD("[{}] 忽略事件 {}", kLogTag, std::string{eventName});
        return false;
    }
    if (!data.is_object()) {
        return false;
    }
    out = InboundEvent{};
    // @ 判定只看事件类型 (设计文档 §7.2): 全量消息事件不带 @ 语义
    out.mentionedMe  = (eventName == "GROUP_AT_MESSAGE_CREATE" || eventName == "C2C_MESSAGE_CREATE");
    out.fromFullMode = (eventName == "GROUP_MESSAGE_CREATE");
    out.type = (eventName == "C2C_MESSAGE_CREATE") ? ChatType::Private : ChatType::Group;

    out.platformMessageId = data.value("id", std::string{});
    const auto& author    = data.contains("author") ? data["author"] : utilxx_base::Json::object();
    if (author.is_object()) {
        out.senderId = author.value("id", std::string{});
        if (out.senderId.empty()) {
            out.senderId = author.value("member_openid", std::string{});
        }
        if (out.senderId.empty()) {
            out.senderId = author.value("user_openid", std::string{});
        }
        out.senderName = author.value("username", std::string{});
        out.senderRole = roleOf(author);
        if (author.value("bot", false)) {
            // 机器人自己发的消息不再进流程 (平台也可能推回来)
            return false;
        }
    }
    if (out.type == ChatType::Group) {
        out.chatId = data.value("group_openid", std::string{});
    } else {
        out.chatId = out.senderId;
    }
    if (out.chatId.empty()) {
        XX_LOGW("[{}] 事件 {} 缺少会话 id, 已丢弃", kLogTag, std::string{eventName});
        return false;
    }

    out.text = data.value("content", std::string{});

    // 引用消息 (message_type=103): 把被引用内容拼到正文前面
    const int messageType = static_cast<int>(data.value("message_type", int64_t{0}));
    if (data.contains("msg_elements") && data["msg_elements"].is_array()
        && !data["msg_elements"].empty()) {
        const auto& quoted = data["msg_elements"][0];
        if (quoted.is_object()) {
            if (quoted.contains("author") && quoted["author"].is_object()) {
                out.quotedSender = quoted["author"].value("username", std::string{});
            }
            out.quotedText = quoted.value("content", std::string{});
        }
    } else if (messageType == 3) {
        // 卡片消息: 用 ark_data 拼一行文本
        const auto& ark = data.contains("ark_data") ? data["ark_data"] : utilxx_base::Json{};
        if (ark.is_object()) {
            std::string joined = ark.value("prompt", std::string{});
            if (ark.contains("fields") && ark["fields"].is_array()) {
                for (const auto& field : ark["fields"]) {
                    if (!field.is_object()) {
                        continue;
                    }
                    const auto title = field.value("title", std::string{});
                    const auto desc  = field.value("desc", std::string{});
                    if (!title.empty()) {
                        joined += (joined.empty() ? "" : " | ") + title + ": " + desc;
                    }
                }
            }
            if (!joined.empty()) {
                out.text = joined;
            }
        }
    }

    // `message_scene.ext`: msg_idx (引用回复用) / ref_msg_idx
    if (data.contains("message_scene") && data["message_scene"].is_object()) {
        const auto& scene = data["message_scene"];
        const auto  ext   = scene.value("ext", std::string{});
        if (!ext.empty()) {
            out.replyRef = sceneExtValue(ext, "msg_idx");
        }
    }

    // 附件
    if (data.contains("attachments") && data["attachments"].is_array()) {
        for (const auto& item : data["attachments"]) {
            if (!item.is_object()) {
                continue;
            }
            MediaRef ref;
            ref.url       = item.value("url", std::string{});
            ref.fileName  = item.value("filename", std::string{});
            ref.mimeType  = item.value("content_type", std::string{});
            ref.asrText   = item.value("asr_refer_text", std::string{});
            ref.sizeBytes = static_cast<uint64_t>(
                item.value("size", static_cast<int64_t>(0))
            );
            const auto contentType = ref.mimeType;
            if (contentType.rfind("image/", 0) == 0) {
                ref.kind = MediaKind::Image;
            } else if (contentType.rfind("video/", 0) == 0) {
                ref.kind = MediaKind::Video;
            } else if (contentType == "voice" || contentType.rfind("audio/", 0) == 0) {
                ref.kind = MediaKind::Audio;
                if (!item.value("voice_wav_url", std::string{}).empty()) {
                    ref.url = item.value("voice_wav_url", std::string{});
                }
                // 语音优先用平台识别文本 (不必让模型听音频)
                if (!ref.asrText.empty()) {
                    if (!out.text.empty()) {
                        out.text += " ";
                    }
                    out.text += ref.asrText;
                }
            } else {
                ref.kind = MediaKind::File;
            }
            if (ref.fileName.empty()) {
                ref.fileName = fileNameOfUrl(ref.url);
            }
            if (!ref.url.empty()) {
                out.attachments.push_back(std::move(ref));
            }
        }
    }

    out.timestampMs = parseRfc3339Ms(data.value("timestamp", std::string{}));
    if (out.timestampMs <= 0) {
        out.timestampMs = unixNowMs();
    }
    if (out.text.empty() && out.attachments.empty()) {
        // 纯空消息 (表情/系统提示): 不值得叫醒模型
        return false;
    }
    return true;
}

std::string QqPlatform::statusJson() const {
    std::lock_guard guard(mutex_);
    utilxx_base::Json out = utilxx_base::Json::object();
    out["type"]         = "qq";
    out["account"]      = config_.id;
    out["state"]        = platformStateName(state_);
    out["reason"]       = stateReason_;
    out["online"]       = gateway_ != nullptr && gateway_->online();
    out["inbound"]      = static_cast<int64_t>(inboundCount_);
    out["outbound"]     = static_cast<int64_t>(outboundCount_);
    out["sendFail"]     = static_cast<int64_t>(sendFailCount_);
    out["reconnects"]   = gateway_ != nullptr ? gateway_->reconnectCount() : 0;
    out["sequence"]     = gateway_ != nullptr ? gateway_->sequence() : 0;
    if (gateway_ != nullptr && !gateway_->lastError().empty()) {
        out["lastError"] = gateway_->lastError();
    }
    return out.dump();
}

} // namespace chatbot
} // namespace agentxx
