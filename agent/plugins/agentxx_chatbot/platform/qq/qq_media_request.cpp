#include "qq_media_request.h"

#include <algorithm>

namespace agentxx {
namespace chatbot {

namespace {

/// 解析字符串里的十进制整数 (只认 [+-]?[0-9]+, 其它一律失败)
///
/// 平台出现过 `"block_size": " 5242880"` 这类带空格的值, 所以首尾空白先裁掉。
bool parseDecimal(std::string_view text, int64_t& out) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return false;
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    text           = text.substr(begin, end - begin + 1);
    bool negative  = false;
    size_t index   = 0;
    if (text[0] == '-' || text[0] == '+') {
        negative = text[0] == '-';
        index    = 1;
    }
    if (index >= text.size()) {
        return false;
    }
    int64_t value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = negative ? -value : value;
    return true;
}

/// 成功解析返回 true; 数字与"数字字符串"两种形态都接受
bool tryParseInt64(const utilxx_base::Json& value, int64_t& out) {
    if (value.is_number_integer()) {
        out = static_cast<int64_t>(value.get<long long>());
        return true;
    }
    if (value.is_number_unsigned()) {
        out = static_cast<int64_t>(value.get<unsigned long long>());
        return true;
    }
    if (value.is_number_float()) {
        out = static_cast<int64_t>(value.get<double>());
        return true;
    }
    if (value.is_string()) {
        return parseDecimal(value.get<std::string>(), out);
    }
    return false;
}

} // namespace

int64_t qqJsonInt64(const utilxx_base::Json& value) {
    int64_t out = 0;
    if (!tryParseInt64(value, out)) {
        return 0;
    }
    return out;
}

int64_t qqJsonInt64Of(const utilxx_base::Json& object, std::string_view key, int64_t fallback) {
    if (!object.is_object() || !object.contains(key)) {
        return fallback;
    }
    int64_t out = 0;
    if (!tryParseInt64(object[key], out)) {
        return fallback;
    }
    return out;
}

std::vector<QqUploadPartPlan>
    qqPlanUploadParts(const utilxx_base::Json& prepareJson, size_t contentSize) {
    std::vector<QqUploadPartPlan> out;
    if (!prepareJson.is_object() || contentSize == 0) {
        return out;
    }
    // 文件级分块大小: 平台按字符串下发, 解析不出来时退回到"整个文件一片"
    const int64_t defaultBlock = qqJsonInt64Of(prepareJson, "block_size", 0);

    struct RawPart {
        int         index = 0;
        std::string url;
        int64_t     blockSize = 0;
    };
    std::vector<RawPart> raw;
    if (prepareJson.contains("parts")) {
        const auto& partsJson = prepareJson["parts"];
        if (partsJson.is_array()) {
            for (const auto& item : partsJson) {
                if (!item.is_object()) {
                    continue;
                }
                RawPart part;
                // 序号缺失时按出现顺序补, 保证后面回传的 `part_index` 不会是同一个值
                part.index = static_cast<int>(
                    qqJsonInt64Of(item, "index", static_cast<int64_t>(raw.size()))
                );
                part.url       = item.value("presigned_url", std::string{});
                part.blockSize = qqJsonInt64Of(item, "block_size", 0);
                if (part.url.empty()) {
                    continue;
                }
                raw.push_back(std::move(part));
            }
        }
    }
    if (raw.empty()) {
        return out;
    }
    // 偏移按序号升序累加: 平台给的排列顺序不可全信, 乱序会让每片都传到错的位置
    std::stable_sort(raw.begin(), raw.end(), [](const RawPart& lhs, const RawPart& rhs) {
        return lhs.index < rhs.index;
    });

    const int64_t total = static_cast<int64_t>(contentSize);
    int64_t       offset = 0;
    for (const auto& part : raw) {
        if (offset >= total) {
            break;
        }
        int64_t take = part.blockSize > 0 ? part.blockSize : defaultBlock;
        if (take <= 0) {
            take = total - offset;
        }
        const int64_t size = std::min<int64_t>(take, total - offset);
        if (size <= 0) {
            continue;
        }
        QqUploadPartPlan plan;
        plan.index     = part.index;
        plan.url       = part.url;
        plan.offset    = static_cast<size_t>(offset);
        plan.sizeBytes = static_cast<size_t>(size);
        out.push_back(std::move(plan));
        offset += size;
    }
    return out;
}

utilxx_base::Json qqBuildPrepareBody(
    int                  fileType,
    size_t               fileSize,
    std::string_view     fileName,
    const QqFileDigests& digests
) {
    utilxx_base::Json body = utilxx_base::Json::object();
    body["file_type"]      = fileType;
    // 文档里 `file_size` 是 string: 64 位整数按字符串下发, 传数字时按 0 规划分片
    body["file_size"] = std::to_string(fileSize);
    body["file_name"] = std::string{fileName};
    body["md5"]       = digests.md5;
    body["sha1"]      = digests.sha1;
    body["md5_10m"]   = digests.md5_10m;
    return body;
}

utilxx_base::Json qqBuildPartFinishBody(
    std::string_view uploadId,
    int              partIndex,
    size_t           partSize,
    std::string_view partMd5
) {
    utilxx_base::Json body = utilxx_base::Json::object();
    body["upload_id"]      = std::string{uploadId};
    body["part_index"]     = partIndex;
    // 文档里 `block_size` 是 string, 且写的是"该分块的实际大小" —— 必须给这一片的真实字节数,
    // 少给这个字段服务端直接回 500 (实测)
    body["block_size"] = std::to_string(partSize);
    body["md5"]        = std::string{partMd5};
    return body;
}

utilxx_base::Json qqBuildMergeBody(
    std::string_view uploadId,
    std::string_view fileName,
    int              fileType
) {
    utilxx_base::Json body = utilxx_base::Json::object();
    body["upload_id"]      = std::string{uploadId};
    body["file_name"]      = std::string{fileName};
    body["file_type"]      = fileType;
    body["srv_send_msg"]   = false;
    return body;
}

utilxx_base::Json qqBuildDirectUploadBody(
    std::string_view fileName,
    int              fileType,
    std::string_view base64Data
) {
    utilxx_base::Json body = utilxx_base::Json::object();
    body["file_type"]      = fileType;
    body["file_name"]      = std::string{fileName};
    body["file_data"]      = std::string{base64Data};
    body["srv_send_msg"]   = false;
    // 故意不带 `upload_id`: 带上它服务端会走"分片合并"路径, 与 `file_data` 互相冲突
    return body;
}

} // namespace chatbot
} // namespace agentxx
