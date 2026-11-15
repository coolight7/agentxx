#pragma once

#include "utilxx_base/json.h"
#include "utilxx_base/string_util.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {

/// 多模态媒体类型
enum class MediaType : uint8_t {
    Image,
    Audio,
    Video
};

inline std::string_view mediaTypeToString(MediaType t) noexcept {
    switch (t) {
        case MediaType::Image:
            return "image";
        case MediaType::Audio:
            return "audio";
        case MediaType::Video:
            return "video";
    }
    return "image";
}

inline MediaType mediaTypeFromString(std::string_view s) noexcept {
    if (s == "audio") {
        return MediaType::Audio;
    }
    if (s == "video") {
        return MediaType::Video;
    }
    return MediaType::Image;
}

/// 多模态单文件/单次限额 (与 TUI 预检/服务端保持一致)
inline constexpr uint64_t kMaxImageBytes            = 10ULL * 1024 * 1024;
inline constexpr uint64_t kMaxAudioBytes            = 25ULL * 1024 * 1024;
inline constexpr uint64_t kMaxVideoBytes            = 50ULL * 1024 * 1024;
inline constexpr size_t   kMaxAttachmentsPerMessage = 5;

/// 小写后缀 (含 '.') -> MediaType, 非媒体返回 nullopt
inline std::optional<MediaType> mediaTypeFromExtension(std::string_view ext) noexcept {
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".gif"
        || ext == ".bmp") {
        return MediaType::Image;
    }
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".m4a" || ext == ".aac"
        || ext == ".flac") {
        return MediaType::Audio;
    }
    if (ext == ".mp4" || ext == ".mov" || ext == ".webm" || ext == ".mkv") {
        return MediaType::Video;
    }
    return std::nullopt;
}

/// 小写后缀 -> MIME, 未知返回空
inline std::string_view mimeTypeFromExtension(std::string_view ext) noexcept {
    if (ext == ".png") {
        return "image/png";
    }
    if (ext == ".jpg" || ext == ".jpeg") {
        return "image/jpeg";
    }
    if (ext == ".webp") {
        return "image/webp";
    }
    if (ext == ".gif") {
        return "image/gif";
    }
    if (ext == ".bmp") {
        return "image/bmp";
    }
    if (ext == ".wav") {
        return "audio/wav";
    }
    if (ext == ".mp3") {
        return "audio/mpeg";
    }
    if (ext == ".ogg") {
        return "audio/ogg";
    }
    if (ext == ".m4a") {
        return "audio/mp4";
    }
    if (ext == ".aac") {
        return "audio/aac";
    }
    if (ext == ".flac") {
        return "audio/flac";
    }
    if (ext == ".mp4") {
        return "video/mp4";
    }
    if (ext == ".mov") {
        return "video/quicktime";
    }
    if (ext == ".webm") {
        return "video/webm";
    }
    if (ext == ".mkv") {
        return "video/x-matroska";
    }
    return "";
}

inline uint64_t maxBytesForMediaType(MediaType t) noexcept {
    switch (t) {
        case MediaType::Image:
            return kMaxImageBytes;
        case MediaType::Audio:
            return kMaxAudioBytes;
        case MediaType::Video:
            return kMaxVideoBytes;
    }
    return kMaxImageBytes;
}

/// 模型声明的三类多模态输入开关 → 该媒体类型能不能投给模型
///
/// - 参数就是 `ModelConfig` 的 `imageInput` / `audioInput` / `videoInput`
///   (yaml `image_input` / `audio_input` / `video_input`)
/// - 服务端据它过滤附件: 没声明的类型上游通常整轮拒绝 (HTTP 400 一类),
///   而 base64 附件会永久留在会话上下文里、每轮重发
///
/// - 相关: 使用方见 [base_agent.cpp] 的轮次组装与 [modelcall.cpp] 的请求前兜底
inline bool
    mediaInputEnabled(MediaType type, bool imageInput, bool audioInput, bool videoInput) noexcept {
    switch (type) {
        case MediaType::Image:
            return imageInput;
        case MediaType::Audio:
            return audioInput;
        case MediaType::Video:
            return videoInput;
    }
    return true;
}

/// 因模型未声明输入能力而没有投递的附件计数 (按大类)
struct MediaSkipCounts {
    size_t image = 0;
    size_t audio = 0;
    size_t video = 0;

    size_t total() const noexcept {
        return image + audio + video;
    }
};

/// 计数 → 一句话说明 (写进日志与模型看到的正文)
///
/// 例: `1 个图片 (模型配置 image_input = false)、2 个视频 (模型配置 video_input = false)`
/// 写法带配置键名: 用户看到后能直接去 yaml 里把那项打开
inline std::string describeSkippedMedia(const MediaSkipCounts& counts) {
    std::string out;
    auto        add = [&out](size_t count, std::string_view label, std::string_view key) {
        if (count == 0) {
            return;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += fmt::format("{}/{} count(agentxx config {} = false)", count, label, key);
    };
    add(counts.image, "图片", "image_input");
    add(counts.audio, "音频", "audio_input");
    add(counts.video, "视频", "video_input");
    return out;
}

/// 通用附件对象
struct MediaAttachment {
    MediaType   type = MediaType::Image;
    std::string displayName; ///< 文件名 (展示用，如 "chart.png")
    std::string mimeType;    ///< MIME 类型 (如 "image/png", "audio/wav")
    std::string pathOrUrl;   ///< 本地绝对路径或 HTTP(S) URL
    std::string dataUrl;     ///< RFC 2397 格式: "data:<mime>;base64,<payload>"
    uint64_t    sizeBytes = 0;

    inline static std::string_view mediaTypeIcon(agentxx::agent::MediaType type) {
        switch (type) {
            case agentxx::agent::MediaType::Image:
                return "📷︎";
            case agentxx::agent::MediaType::Audio:
                return "🎵︎";
            case agentxx::agent::MediaType::Video:
                return "🎬︎";
        }
        return "📷︎";
    }

    utilxx_base::Json toJson() const {
        utilxx_base::Json j = utilxx_base::Json::object();
        j["type"]           = std::string(mediaTypeToString(type));
        if (!displayName.empty()) {
            j["display_name"] = displayName;
        }
        if (!mimeType.empty()) {
            j["mime_type"] = mimeType;
        }
        if (!pathOrUrl.empty()) {
            j["path_or_url"] = pathOrUrl;
        }
        if (!dataUrl.empty()) {
            j["data_url"] = dataUrl;
        }
        if (sizeBytes > 0) {
            j["size_bytes"] = sizeBytes;
        }
        return j;
    }

    static MediaAttachment fromJson(const utilxx_base::Json& j) {
        MediaAttachment att;
        att.type = mediaTypeFromString(j.value("type", j.value("kind", std::string{})));
        att.displayName
            = j.value("display_name", j.value("displayName", j.value("name", std::string{})));
        att.mimeType = j.value("mime_type", j.value("mimeType", std::string{}));
        att.pathOrUrl
            = j.value("path_or_url", j.value("pathOrUrl", j.value("path", std::string{})));
        att.dataUrl = j.value("data_url", j.value("dataUrl", std::string{}));
        if (j.contains("size_bytes") && j["size_bytes"].is_number()) {
            att.sizeBytes = j["size_bytes"].get<uint64_t>();
        } else if (j.contains("sizeBytes") && j["sizeBytes"].is_number()) {
            att.sizeBytes = j["sizeBytes"].get<uint64_t>();
        } else if (j.contains("size") && j["size"].is_number()) {
            att.sizeBytes = j["size"].get<uint64_t>();
        } else {
            att.sizeBytes = 0;
        }
        return att;
    }
};

/// 估算附件字节大小 (优先取 sizeBytes; 其次估算 dataUrl Base64 解码后大小; 再次检查本地文件大小)
inline uint64_t estimateAttachmentSizeBytes(const MediaAttachment& att) {
    if (att.sizeBytes > 0) {
        return att.sizeBytes;
    }
    if (!att.dataUrl.empty()) {
        auto comma = att.dataUrl.find(',');
        if (comma != std::string::npos) {
            size_t b64len = att.dataUrl.size() - comma - 1;
            return (b64len * 3) / 4;
        }
        return att.dataUrl.size();
    }
    if (!att.pathOrUrl.empty() && !att.pathOrUrl.starts_with("http://")
        && !att.pathOrUrl.starts_with("https://")) {
        std::error_code ec;
        auto            p  = utilxx_base::utf8ToPath(att.pathOrUrl);
        auto            sz = std::filesystem::file_size(p, ec);
        if (!ec) {
            return sz;
        }
    }
    return 0;
}

} // namespace agent
} // namespace agentxx
