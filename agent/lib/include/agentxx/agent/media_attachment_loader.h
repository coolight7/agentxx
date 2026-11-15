#pragma once

#include "agentxx/agent/media_attachment.h"
#include "asio/awaitable.hpp"
#include "asio/thread_pool.hpp"
#include "fmt/format.h"
#include "utilxx/async_offload.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace agentxx {
namespace agent {

/// 异步将本地媒体附件读取并转换为 Base64 Data URL (经线程池卸载, 避免阻塞 io 线程)
inline asio::awaitable<MediaAttachment> loadLocalMediaAttachmentAsync(
    MediaAttachment    att,
    asio::thread_pool* pool = nullptr
) {
    auto doLoad = [att = std::move(att)]() mutable -> MediaAttachment {
        std::error_code ec;
        auto            filePath = utilxx_base::utf8ToPath(att.pathOrUrl);
        auto            fileSize = std::filesystem::file_size(filePath, ec);
        if (ec) {
            XX_LOGW(
                "[media_loader] cannot get file size for media attachment: {}, err: {}",
                att.pathOrUrl,
                ec.message()
            );
            return att;
        }
        uint64_t maxSize = maxBytesForMediaType(att.type);
        if (fileSize > maxSize) {
            XX_LOGW(
                "[media_loader] media attachment too large: {} bytes > max {} bytes, path: {}",
                fileSize,
                maxSize,
                att.pathOrUrl
            );
            return att;
        }
        std::ifstream ifs(filePath, std::ios::binary);
        if (!ifs) {
            XX_LOGW("[media_loader] cannot open media attachment: {}", att.pathOrUrl);
            return att;
        }
        std::string fileData(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>()
        );
        ifs.close();
        if (att.mimeType.empty()) {
            auto ext = utilxx_base::toLower(utilxx_base::pathToUtf8Generic(filePath.extension()));
            att.mimeType = std::string(mimeTypeFromExtension(ext));
        }
        att.dataUrl = fmt::format(
            "data:{};base64,{}",
            att.mimeType,
            utilxx_base::base64Encode(fileData)
        );
        att.sizeBytes = fileSize;
        XX_LOGD(
            "[media_loader] media attachment loaded: {} ({} bytes, {})",
            att.pathOrUrl,
            fileSize,
            att.mimeType
        );
        return att;
    };

    if (nullptr == pool) {
        co_return doLoad();
    }
    co_return co_await utilxx::offloadAsync<MediaAttachment>(
        *pool,
        [doLoad = std::move(doLoad)]() mutable -> asio::awaitable<MediaAttachment> {
            co_return doLoad();
        }
    );
}

} // namespace agent
} // namespace agentxx
