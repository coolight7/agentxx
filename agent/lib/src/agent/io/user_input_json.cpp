#include "agentxx/agent/io/user_input_json.h"

#include "fmt/format.h"
#include "utilxx_base/json.h"

namespace agentxx {
namespace agent {

namespace {

/// 检查附件是否超出单文件限额; 超限时返回可读原因 (哪一项、多大、上限多少)
std::string attachmentLimitError(const MediaAttachment& att) {
    const uint64_t maxSize = maxBytesForMediaType(att.type);
    const uint64_t estSize = estimateAttachmentSizeBytes(att);
    if (estSize <= maxSize) {
        return {};
    }
    std::string name = !att.displayName.empty()
                           ? att.displayName
                           : (!att.pathOrUrl.empty() ? att.pathOrUrl : "attachment");
    return fmt::format(
        "附件 '{}' 超出 {} 类型的大小上限 ({} 字节 > {} 字节)",
        name,
        mediaTypeToString(att.type),
        estSize,
        maxSize
    );
}

} // namespace

std::expected<WireUserInput, UserInputError> parseUserInputJson(std::string_view inputJson) {
    utilxx_base::Json j;
    try {
        j = utilxx_base::Json::parse(inputJson);
    } catch (const std::exception& e) {
        return std::unexpected(
            UserInputError{true, fmt::format("input_json JSON 解析失败: {}", e.what())}
        );
    }
    if (!j.is_object()) {
        return std::unexpected(UserInputError{
            false,
            std::string{
                "input_json 必须是 JSON 对象, 形如 {\"text\":\"...\",\"attachments\":[...]}"
            }
        });
    }

    WireUserInput input;
    input.text     = j.value("text", std::string{});
    input.model    = j.value("model", std::string{});
    input.delivery = j.value("delivery", std::string{});

    if (j.contains("attachments") && j["attachments"].is_array()) {
        for (const auto& item : j["attachments"]) {
            // fromJson 同时认下划线与驼峰/别名写法 (与 wire user_input.attachments 一致)
            input.attachments.push_back(MediaAttachment::fromJson(item));
        }
    } else if (j.contains("attachments") && !j["attachments"].is_null()) {
        return std::unexpected(
            UserInputError{false, std::string{"input_json.attachments 必须是数组"}}
        );
    }

    if (input.text.empty() && input.attachments.empty()) {
        return std::unexpected(
            UserInputError{false, std::string{"输入为空: text 与 attachments 至少需要一个非空"}}
        );
    }
    // 条数与大小先在本地拦一次 (与 wire 服务端同一套约定): 参数写错立刻同步报错,
    // 不让调用方等到 input_ack(rejected) 才发现
    if (input.attachments.size() > kMaxAttachmentsPerMessage) {
        return std::unexpected(UserInputError{
            false,
            fmt::format(
                "附件条数超出上限: {} > {} (单条消息)",
                input.attachments.size(),
                kMaxAttachmentsPerMessage
            )
        });
    }
    for (const auto& att : input.attachments) {
        auto limitErr = attachmentLimitError(att);
        if (!limitErr.empty()) {
            return std::unexpected(UserInputError{false, std::move(limitErr)});
        }
    }
    return input;
}

} // namespace agent
} // namespace agentxx
