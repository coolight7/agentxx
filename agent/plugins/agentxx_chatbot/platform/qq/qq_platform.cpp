#include "qq_platform.h"

#include "../../chatbot_core.h"
#include "qq_normalize.h"
#include "utilxx/http_client.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include "asio/co_spawn.hpp"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"

#include <chrono>
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

} // namespace

QqPlatform::QqPlatform(asio::any_io_executor ex, std::string dataDir) :
    ex_(std::move(ex)),
    dataDir_(std::move(dataDir)) {}

QqPlatform::~QqPlatform() {
    stop();
}

PlatformCaps QqPlatform::caps() const {
    PlatformCaps caps;
    // 群支持 markdown 发送, 单聊「可发不收」: 调度层按 chatType 再收一次
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
    // 锁只护住本对象的成员读写: 下面调网关 / 回调调度层时**不能**持锁 ——
    // 网关 start 会同步回调 `onState`, 那个回调要在本对象上再拿一次锁;
    // 非递归 mutex 在同一线程上重入会直接抛 std::system_error (实测进程 abort)。
    {
        std::lock_guard lock(mutex_);
        config_  = cfg;
        sink_    = &sink;
        stopped_ = false;
        state_   = PlatformState::Connecting;
        stateReason_.clear();
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
        gateway_  = std::make_unique<QqGateway>(ex_, *api_);
    }

    QqGatewayHooks hooks;
    hooks.onState = [this](PlatformState state, std::string reason) {
        PlatformEventSink* sinkPtr = nullptr;
        {
            std::lock_guard guard(mutex_);
            state_       = state;
            stateReason_ = reason;
            sinkPtr      = sink_;
        }
        if (sinkPtr != nullptr && sinkPtr->onStateChanged) {
            // 回调调度层必须在锁外: 那边可能同步回查本对象 (如 statusJson)
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
    if (auto* gateway = gateway_.get(); gateway != nullptr) {
        gateway->start(cfg, std::move(hooks));
    }
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
    std::unique_ptr<QqGateway> gateway;
    {
        std::lock_guard lock(mutex_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
        sink_    = nullptr;
        gateway  = std::move(gateway_);
        state_       = PlatformState::Disabled;
        stateReason_ = "stopped";
    }
    // 网关 stop 会在本对象上回调状态 (onState), 因此同样放在锁外
    if (gateway) {
        gateway->stop();
        // 网关按引用持有 api_, 必须先销毁网关再释放 api_
        gateway.reset();
    }
    std::lock_guard lock(mutex_);
    api_.reset();
    uploader_.reset();
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
    const std::string fileName
        = ref.fileName.empty() ? qqFileNameOfUrl(ref.url) : ref.fileName;
    // 落盘目录: {data_dir}/chatbot/media/{accountId}/ (按附件大类分子目录, 便于排查)
    const fs::path dir = utilxx_base::utf8ToPath(mediaRoot_) / mediaKindName(ref.kind);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path    target     = dir / (std::to_string(unixNowMs()) + "-" + fileName);
    const std::string targetUtf8 = utilxx_base::pathToUtf8Generic(target);

    const auto& url = ref.url;
    // 下载必须**同步**完成后再回填 `localPath`:
    // - 调用方 (prepareAttachments) 紧接着就会把这条消息投给模型, 附件路径必须是
    //   已经落盘的文件; 之前提前填路径, 主程序读文件时报
    //   `cannot get file size ... The system cannot find the file specified`,
    //   模型请求直接 400 (media url 不是 http/https), 用户看到"加载一下就没了"
    // - 这里不开在 plugin io 线程上: 用一次性的 io_context 在本线程跑到完成,
    //   不阻塞插件 io 线程的其它协程 (附件通常几百 KB, 单次等待可接受)
    asio::io_context dlCtx;
    // work guard: 协程在被调度起来之前 run() 不能"因为没有工作"就返回
    auto             dlWork = asio::make_work_guard(dlCtx);
    asio::co_spawn(
        dlCtx,
        [&]() -> asio::awaitable<void> {
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
            ref.localPath = targetUtf8;
            XX_LOGD(
                "[{}] 附件 '{}' 已下载到 {} ({} 字节)",
                kLogTag,
                fileName,
                targetUtf8,
                response->body.size()
            );
        },
        [&dlWork](const std::exception_ptr& ep) {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    XX_LOGW("[{}] 附件下载协程异常: {}", kLogTag, e.what());
                } catch (...) {
                    XX_LOGW("[{}] 附件下载协程异常 (未知)", kLogTag);
                }
            }
            dlWork.reset(); // 协程结束: 让 run() 可以返回
        }
    );
    dlCtx.run();
}

RecallResult QqPlatform::recallMessage(
    const std::string& accountId,
    ChatType           type,
    const std::string& chatId,
    const std::string& messageId
) {
    (void)accountId;
    RecallResult result;
    if (chatId.empty() || messageId.empty()) {
        result.reason = "撤回需要会话 id 与消息 id";
        return result;
    }
    QqApi* api = nullptr;
    {
        std::lock_guard lock(mutex_);
        api = api_.get();
    }
    if (api == nullptr) {
        result.reason = "账号未连接, 无法撤回";
        return result;
    }

    // 与附件下载同一套做法: 用一次性 io_context 在本线程跑到完成。
    // 撤销接口要拿到结果才能回给模型, 不能像发送那样"提交后就不管"。
    asio::io_context rlCtx;
    auto             rlWork = asio::make_work_guard(rlCtx);
    asio::co_spawn(
        rlCtx,
        [&]() -> asio::awaitable<void> {
            auto response = co_await api->recallMessage(type, chatId, messageId);
            if (response.ok()) {
                result.ok = true;
                XX_LOGI("[{}] 已撤回消息 {}", kLogTag, messageId);
                co_return;
            }
            if (response.status == 0) {
                result.reason = "撤回请求失败: " + response.error;
            } else {
                // 平台对超时/无权限都返回 4xx, 正文里通常带可读原因
                result.reason = "撤回失败 (HTTP " + std::to_string(response.status) + "): "
                                + (response.body.empty() ? response.error : response.body.substr(0, 200));
            }
            XX_LOGW("[{}] 撤回消息 {} 失败: {}", kLogTag, messageId, result.reason);
        },
        [&rlWork](const std::exception_ptr& ep) {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    XX_LOGW("[{}] 撤回协程异常: {}", kLogTag, e.what());
                } catch (...) {
                    XX_LOGW("[{}] 撤回协程异常 (未知)", kLogTag);
                }
            }
            rlWork.reset();
        }
    );
    rlCtx.run();
    return result;
}

bool QqPlatform::normalize(
    std::string_view         eventName,
    const utilxx_base::Json& data,
    InboundEvent&            out
) const {
    // 字段解析全部在 qq_normalize.cpp (纯函数, 可以用真实事件做回归)
    return normalizeQqEvent(eventName, data, out);
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
