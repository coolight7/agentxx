#include "qq_media.h"

#include "qq_api.h"
#include "utilxx/crypto.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include "asio/as_tuple.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

/// `file_info` 缓存时长 (ttl 也知道, 取 min(ttl, 5 分钟))
constexpr int64_t kFileInfoCacheMs = 5 * 60 * 1000;

/// `md5_10m` 的取样长度: 官方文档写的是"文件前 10002432 字节 (约 10MB)"
constexpr size_t kDigestHeadBytes = 10002432u;

/// 单片的尝试次数 (含首次)
constexpr int kPartAttempts = 3;
/// 合并请求的尝试次数 (含首次)
constexpr int kMergeAttempts = 3;

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

std::string lowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        out.push_back(uc >= 'A' && uc <= 'Z' ? static_cast<char>(uc + ('a' - 'A')) : c);
    }
    return out;
}

std::string extensionOf(std::string_view fileName) {
    const auto dot = fileName.find_last_of('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    return lowerAscii(fileName.substr(dot));
}

/// SHA1 (OpenSSL EVP; cxx_utilxx 已依赖 OpenSSL, 插件直接链)
std::string sha1Hex(std::string_view content) {
    unsigned char digest[20];
    unsigned int  len = 0;
    EVP_MD_CTX*   ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        return {};
    }
    std::string out;
    if (EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) == 1
        && EVP_DigestUpdate(ctx, content.data(), content.size()) == 1
        && EVP_DigestFinal_ex(ctx, digest, &len) == 1) {
        static const char* kHex = "0123456789abcdef";
        out.reserve(static_cast<size_t>(len) * 2);
        for (unsigned int i = 0; i < len; ++i) {
            out.push_back(kHex[digest[i] >> 4]);
            out.push_back(kHex[digest[i] & 0x0F]);
        }
    }
    EVP_MD_CTX_free(ctx);
    return out;
}

bool readWholeFileBytes(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

/// 5xx 与传输层失败可以重试 (官方错误码 40093001"文件上传失败，请重试"就是这类瞬时故障);
/// 4xx 是参数/权限/限额问题, 重试没有意义
bool retryable(const QqResponse& response) {
    if (!response.error.empty()) {
        return true;
    }
    return response.status >= 500 && response.status < 600;
}

/// 把一次失败拼成能直接给模型看的一句话 (状态码 + 传输层错误 + 响应片段)
std::string describeFailure(const std::string& what, const QqResponse& response) {
    std::string text = what;
    text += " (status ";
    text += std::to_string(response.status);
    if (!response.error.empty()) {
        text += ", ";
        text += response.error;
    }
    text += ")";
    if (!response.body.empty()) {
        text += ": ";
        text += response.body.substr(0, 200);
    }
    return text;
}

/// 在协程里等一会儿 (取当前协程的 executor, 不额外占线程)
asio::awaitable<void> sleepForMs(int64_t delayMs) {
    if (delayMs <= 0) {
        co_return;
    }
    auto               executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    timer.expires_after(std::chrono::milliseconds(delayMs));
    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
}

} // namespace

int qqFileTypeOf(std::string_view fileName, std::string_view mimeType) noexcept {
    const std::string ext  = extensionOf(fileName);
    const std::string mime = lowerAscii(mimeType);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".webp"
        || ext == ".bmp" || mime.rfind("image/", 0) == 0) {
        return 1;
    }
    if (ext == ".mp4" || ext == ".mov" || ext == ".webm" || ext == ".mkv"
        || mime.rfind("video/", 0) == 0) {
        return 2;
    }
    if (ext == ".silk" || ext == ".slk" || ext == ".mp3" || ext == ".wav" || ext == ".ogg"
        || ext == ".m4a" || ext == ".aac" || ext == ".flac" || mime.rfind("audio/", 0) == 0) {
        return 3;
    }
    return 4;
}

uint64_t qqSoftLimitOf(int fileType) noexcept {
    switch (fileType) {
        case 1:
            return 20ULL * 1024 * 1024;
        case 2:
            return 30ULL * 1024 * 1024;
        case 3:
            return 20ULL * 1024 * 1024;
        default:
            return 200ULL * 1024 * 1024;
    }
}

uint64_t qqHardLimitOf(int fileType) noexcept {
    (void)fileType;
    return 200ULL * 1024 * 1024;
}

QqFileDigests qqComputeDigests(std::string_view content) {
    QqFileDigests out;
    out.md5  = utilxx::md5Hex(content);
    out.sha1 = sha1Hex(content);
    const auto head = content.substr(0, std::min(content.size(), kDigestHeadBytes));
    out.md5_10m     = utilxx::md5Hex(head);
    return out;
}

QqMediaUploader::QqMediaUploader(QqApi& api, std::string accountId) :
    api_(api),
    accountId_(std::move(accountId)) {}

void QqMediaUploader::clearCache() {
    cache_.clear();
}

std::string QqMediaUploader::basePathOf(ChatType chatType, std::string_view chatId) {
    return chatType == ChatType::Group ? "/v2/groups/" + std::string{chatId}
                                       : "/v2/users/" + std::string{chatId};
}

std::string QqMediaUploader::cacheKeyOf(
    ChatType         chatType,
    std::string_view chatId,
    std::string_view localPath,
    int64_t          mtimeMs,
    int              fileType
) const {
    return std::string{chatTypeName(chatType)} + "|" + std::string{chatId} + "|"
           + std::string{localPath} + "|" + std::to_string(mtimeMs) + "|"
           + std::to_string(fileType);
}

asio::awaitable<QqUploadResult> QqMediaUploader::upload(
    const MediaRef&  ref,
    ChatType         chatType,
    std::string_view chatId,
    std::string_view textContent
) {
    QqUploadResult out;
    std::string    content;
    std::string    fileName = ref.fileName;
    if (!textContent.empty()) {
        content = std::string{textContent};
    } else if (!ref.localPath.empty()) {
        if (!readWholeFileBytes(ref.localPath, content)) {
            out.reason = "读不到本地文件: " + ref.localPath;
            co_return out;
        }
        if (fileName.empty()) {
            const auto slash = ref.localPath.find_last_of("/\\");
            fileName = slash == std::string::npos ? ref.localPath : ref.localPath.substr(slash + 1);
        }
    } else {
        out.reason = "附件既没有本地路径也没有内容, 无法上传 (远端 URL 请先下载)";
        co_return out;
    }
    if (fileName.empty()) {
        fileName = "file";
    }
    if (content.empty()) {
        out.reason = "附件内容为空, 无法上传";
        co_return out;
    }

    int fileType = qqFileTypeOf(fileName, ref.mimeType);
    // 大小以**真正要上传的字节数**为准 (调用方给的 sizeBytes 可能过期)
    const uint64_t size      = content.size();
    const uint64_t hardLimit = qqHardLimitOf(fileType);
    if (size > hardLimit) {
        out.reason = "附件超过平台硬限制 (" + std::to_string(size / (1024 * 1024)) + " MB > "
                     + std::to_string(hardLimit / (1024 * 1024)) + " MB)";
        co_return out;
    }
    // 超软限制降级成「文件」卡片 (设计文档 §3.6)
    if (size > qqSoftLimitOf(fileType)) {
        XX_LOGW(
            "[{}] 附件 '{}' 超过类型 {} 的软限制, 降级为文件类型发送",
            kLogTag,
            fileName,
            fileType
        );
        fileType = 4;
    }
    out.fileType = fileType;

    const int64_t mtimeMs = 0; // 缓存键只用路径 + 类型 (文件改动由 file_info ttl 兜住)
    const auto    key     = cacheKeyOf(
        chatType,
        chatId,
        ref.localPath.empty() ? fileName : ref.localPath,
        mtimeMs,
        fileType
    );
    if (auto it = cache_.find(key); it != cache_.end() && it->second.expireAtMs > steadyNowMs()) {
        out.ok       = true;
        out.fileInfo = it->second.fileInfo;
        XX_LOGD("[{}] 附件 '{}' 命中 file_info 缓存", kLogTag, fileName);
        co_return out;
    }

    const auto digests = qqComputeDigests(content);
    const auto base    = basePathOf(chatType, chatId);

    // ---- 1. upload_prepare ----
    auto prepare = co_await api_.request(
        "POST",
        base + "/upload_prepare",
        qqBuildPrepareBody(fileType, content.size(), fileName, digests)
    );
    if (!prepare.ok()) {
        out.reason = describeFailure("upload_prepare 失败", prepare);
        co_return out;
    }
    auto prepareJson = prepare.json();
    if (!prepareJson.has_value() || !prepareJson->is_object()) {
        out.reason = "upload_prepare 响应不是 JSON: " + prepare.body.substr(0, 200);
        co_return out;
    }
    const std::string uploadId = prepareJson->value("upload_id", std::string{});
    if (uploadId.empty()) {
        out.reason = "upload_prepare 没有返回 upload_id: " + prepare.body.substr(0, 200);
        co_return out;
    }

    // 平台下发的重试策略 (缺省: 延迟 1 秒 / 总时长 5 分钟), 只用于逐片与合并的重试
    int64_t retryDelayMs  = 1000;
    int64_t retryBudgetMs = 300 * 1000;
    if (prepareJson->contains("upload_config")) {
        const auto& config = (*prepareJson)["upload_config"];
        if (config.is_object()) {
            retryDelayMs
                = std::clamp<int64_t>(qqJsonInt64Of(config, "retry_delay", 1) * 1000, 200, 5000);
            retryBudgetMs
                = std::max<int64_t>(5000, qqJsonInt64Of(config, "retry_timeout", 300) * 1000);
        }
    }
    const int64_t deadlineMs = steadyNowMs() + retryBudgetMs;

    // ---- 2. 分片: 逐片 PUT + upload_part_finish ----
    // 平台把 `block_size` / `parts[].block_size` 当字符串下发, 由 qqPlanUploadParts 统一解析;
    // 返回空表示平台没给分片清单 (小文件走二进制直传)
    const auto parts = qqPlanUploadParts(*prepareJson, content.size());
    if (parts.empty()) {
        if (content.size() > qqDirectUploadLimit) {
            out.reason = "平台没有返回分片清单, 而附件 " + std::to_string(content.size())
                         + " 字节超过直传上限 (" + std::to_string(qqDirectUploadLimit)
                         + " 字节), 无法上传";
            co_return out;
        }
        auto direct = co_await api_.request(
            "POST",
            base + "/files",
            qqBuildDirectUploadBody(fileName, fileType, utilxx_base::base64Encode(content))
        );
        if (!direct.ok()) {
            out.reason = describeFailure("上传文件失败", direct);
            co_return out;
        }
        auto              directJson = direct.json();
        const std::string fileInfo   = directJson.has_value()
                                           ? directJson->value("file_info", std::string{})
                                           : std::string{};
        if (fileInfo.empty()) {
            out.reason = "上传成功但没有返回 file_info: " + direct.body.substr(0, 200);
            co_return out;
        }
        out.ok       = true;
        out.fileInfo = fileInfo;
        cache_[key]  = CacheEntry{fileInfo, steadyNowMs() + kFileInfoCacheMs, fileType};
        co_return out;
    }

    // 顺序上传: 单文件通常 1~few 片, 并发收益有限且更易触发限流
    for (const auto& part : parts) {
        const auto chunk   = std::string_view{content}.substr(part.offset, part.sizeBytes);
        const auto partMd5 = utilxx::md5Hex(chunk);
        const auto what    = "片 " + std::to_string(part.index) + " ("
                          + std::to_string(chunk.size()) + " 字节, 偏移 "
                          + std::to_string(part.offset) + ")";
        bool        uploaded   = false;
        bool        putDone    = false;
        std::string failReason;
        for (int attempt = 1; attempt <= kPartAttempts; ++attempt) {
            bool retry = false;
            if (!putDone) {
                auto put = co_await api_.putToPresignedUrl(part.url, chunk);
                if (!put.ok()) {
                    failReason = describeFailure("分片上传失败 (" + what + ")", put);
                    retry      = retryable(put);
                } else {
                    putDone = true;
                }
            }
            if (putDone) {
                // 这一片的真实字节数与 md5 都要回给服务端, 少给会被当成坏请求
                auto partFinish = co_await api_.request(
                    "POST",
                    base + "/upload_part_finish",
                    qqBuildPartFinishBody(uploadId, part.index, chunk.size(), partMd5)
                );
                if (partFinish.ok()) {
                    uploaded = true;
                    break;
                }
                failReason = describeFailure("upload_part_finish 失败 (" + what + ")", partFinish);
                retry      = retryable(partFinish);
            }
            const bool lastAttempt = attempt >= kPartAttempts;
            if (!retry || lastAttempt || steadyNowMs() >= deadlineMs) {
                break;
            }
            XX_LOGW(
                "[{}] 附件 '{}' 第 {} 片第 {} 次失败, {} 毫秒后重试: {}",
                kLogTag,
                fileName,
                part.index,
                attempt,
                retryDelayMs * attempt,
                failReason
            );
            co_await sleepForMs(retryDelayMs * attempt);
        }
        if (!uploaded) {
            out.reason = std::move(failReason);
            co_return out;
        }
    }

    // ---- 3. 合并 ----
    QqResponse merged;
    for (int attempt = 1; attempt <= kMergeAttempts; ++attempt) {
        merged = co_await api_.request(
            "POST",
            base + "/files",
            qqBuildMergeBody(uploadId, fileName, fileType)
        );
        if (merged.ok() || !retryable(merged) || attempt >= kMergeAttempts
            || steadyNowMs() >= deadlineMs) {
            break;
        }
        XX_LOGW(
            "[{}] 附件 '{}' 合并第 {} 次失败, {} 毫秒后重试: {}",
            kLogTag,
            fileName,
            attempt,
            retryDelayMs * attempt,
            describeFailure("合并文件失败", merged)
        );
        co_await sleepForMs(retryDelayMs * attempt);
    }
    if (!merged.ok()) {
        out.reason = describeFailure("合并文件失败", merged);
        co_return out;
    }
    auto              mergedJson = merged.json();
    const std::string fileInfo
        = mergedJson.has_value() ? mergedJson->value("file_info", std::string{}) : std::string{};
    if (fileInfo.empty()) {
        out.reason = "合并成功但没有返回 file_info: " + merged.body.substr(0, 200);
        co_return out;
    }
    int64_t ttlMs = kFileInfoCacheMs;
    if (mergedJson.has_value() && mergedJson->contains("ttl")) {
        const auto& value = (*mergedJson)["ttl"];
        if (value.is_number_integer() || value.is_number_unsigned()) {
            const auto ttlSec = value.get<long long>();
            if (ttlSec > 0) {
                ttlMs = std::min<int64_t>(kFileInfoCacheMs, ttlSec * 1000);
            }
        }
    }
    cache_[key]  = CacheEntry{fileInfo, steadyNowMs() + ttlMs, fileType};
    out.ok       = true;
    out.fileInfo = fileInfo;
    XX_LOGI(
        "[{}] 附件 '{}' 上传完成 ({} 字节, file_type={}, {} 片)",
        kLogTag,
        fileName,
        content.size(),
        fileType,
        parts.size()
    );
    co_return out;
}

} // namespace chatbot
} // namespace agentxx
