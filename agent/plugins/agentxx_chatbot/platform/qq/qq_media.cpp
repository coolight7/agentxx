#include "qq_media.h"

#include "qq_api.h"
#include "utilxx/crypto.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

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

/// 分片校验值的前缀长度 (10 MB)
constexpr size_t kDigestHeadBytes = 10u * 1024u * 1024u;

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

    int fileType = qqFileTypeOf(fileName, ref.mimeType);
    const uint64_t size      = ref.sizeBytes > 0 ? ref.sizeBytes : content.size();
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
    const auto    key     = cacheKeyOf(chatType, chatId, ref.localPath.empty() ? fileName : ref.localPath, mtimeMs, fileType);
    if (auto it = cache_.find(key); it != cache_.end() && it->second.expireAtMs > steadyNowMs()) {
        out.ok       = true;
        out.fileInfo = it->second.fileInfo;
        XX_LOGD("[{}] 附件 '{}' 命中 file_info 缓存", kLogTag, fileName);
        co_return out;
    }

    const auto digests = qqComputeDigests(content);
    const auto base    = basePathOf(chatType, chatId);

    // ---- 1. upload_prepare ----
    utilxx_base::Json prepareBody = utilxx_base::Json::object();
    prepareBody["file_type"] = fileType;
    prepareBody["file_size"] = static_cast<int64_t>(size);
    prepareBody["file_name"] = fileName;
    prepareBody["md5"]       = digests.md5;
    prepareBody["sha1"]      = digests.sha1;
    prepareBody["md5_10m"]   = digests.md5_10m;

    auto prepare = co_await api_.request("POST", base + "/upload_prepare", prepareBody);
    if (!prepare.ok()) {
        out.reason = "upload_prepare 失败 (status " + std::to_string(prepare.status) + "): "
                     + prepare.body.substr(0, 200);
        co_return out;
    }
    auto prepareJson = prepare.json();
    if (!prepareJson.has_value() || !prepareJson->is_object()) {
        out.reason = "upload_prepare 响应不是 JSON";
        co_return out;
    }
    const std::string uploadId  = prepareJson->value("upload_id", std::string{});
    int64_t           blockSize = prepareJson->value("block_size", int64_t{0});
    if (uploadId.empty()) {
        out.reason = "upload_prepare 没有返回 upload_id";
        co_return out;
    }
    if (blockSize <= 0) {
        blockSize = static_cast<int64_t>(content.size()); // 单片
    }
    if (blockSize <= 0) {
        blockSize = 1;
    }

    // ---- 2. 逐片 PUT + upload_part_finish ----
    struct PartDesc {
        int         index = 0;
        std::string url;
        int64_t     size = 0;
    };
    std::vector<PartDesc> parts;
    if (prepareJson->contains("parts") && (*prepareJson)["parts"].is_array()) {
        for (const auto& item : (*prepareJson)["parts"]) {
            if (!item.is_object()) {
                continue;
            }
            PartDesc part;
            part.index = static_cast<int>(item.value("index", int64_t{0}));
            part.url   = item.value("presigned_url", std::string{});
            part.size  = item.value("block_size", blockSize);
            if (!part.url.empty()) {
                parts.push_back(std::move(part));
            }
        }
    }

    if (parts.empty()) {
        // 平台没给分片清单: 单片直传 (≤5MB 的二进制直传路径)
        utilxx_base::Json finish = utilxx_base::Json::object();
        finish["upload_id"]      = uploadId;
        finish["file_name"]      = fileName;
        finish["file_data"]      = utilxx_base::base64Encode(content);
        finish["srv_send_msg"]   = false;
        auto merged = co_await api_.request("POST", base + "/files", finish);
        if (!merged.ok()) {
            out.reason = "上传文件失败 (status " + std::to_string(merged.status) + "): "
                         + merged.body.substr(0, 200);
            co_return out;
        }
        auto mergedJson = merged.json();
        const std::string fileInfo
            = mergedJson.has_value() ? mergedJson->value("file_info", std::string{}) : std::string{};
        if (fileInfo.empty()) {
            out.reason = "上传成功但没有返回 file_info";
            co_return out;
        }
        out.ok       = true;
        out.fileInfo = fileInfo;
        cache_[key]  = CacheEntry{fileInfo, steadyNowMs() + kFileInfoCacheMs, fileType};
        co_return out;
    }

    // 按分片清单逐片上传 (顺序上传: 单文件通常 1~few 片, 并发收益有限且更易触发限流)
    size_t offset = 0;
    for (const auto& part : parts) {
        const size_t take = static_cast<size_t>(
            std::min<int64_t>(part.size > 0 ? part.size : blockSize,
                              static_cast<int64_t>(content.size() - std::min(offset, content.size())))
        );
        const auto chunk = content.substr(offset, take);
        offset += take;
        if (chunk.empty()) {
            continue;
        }
        auto put = co_await api_.putToPresignedUrl(part.url, chunk);
        if (!put.ok()) {
            out.reason = "分片上传失败 (片 " + std::to_string(part.index) + ", status "
                         + std::to_string(put.status) + "): " + put.body.substr(0, 160);
            co_return out;
        }
        utilxx_base::Json finish = utilxx_base::Json::object();
        finish["upload_id"]      = uploadId;
        finish["part_index"]     = part.index;
        auto partFinish = co_await api_.request(
            "POST",
            base + "/upload_part_finish",
            finish
        );
        if (!partFinish.ok()) {
            out.reason = "upload_part_finish 失败 (片 " + std::to_string(part.index)
                         + ", status " + std::to_string(partFinish.status) + ")";
            co_return out;
        }
    }

    // ---- 3. 合并 ----
    utilxx_base::Json mergeBody = utilxx_base::Json::object();
    mergeBody["upload_id"]      = uploadId;
    mergeBody["file_name"]      = fileName;
    mergeBody["srv_send_msg"]   = false;
    auto merged = co_await api_.request("POST", base + "/files", mergeBody);
    if (!merged.ok()) {
        out.reason = "合并文件失败 (status " + std::to_string(merged.status) + "): "
                     + merged.body.substr(0, 200);
        co_return out;
    }
    auto mergedJson = merged.json();
    const std::string fileInfo
        = mergedJson.has_value() ? mergedJson->value("file_info", std::string{}) : std::string{};
    if (fileInfo.empty()) {
        out.reason = "合并成功但没有返回 file_info";
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
        "[{}] 附件 '{}' 上传完成 ({} 字节, file_type={})",
        kLogTag,
        fileName,
        content.size(),
        fileType
    );
    co_return out;
}

} // namespace chatbot
} // namespace agentxx
