#pragma once

/// QQ 富媒体上传的请求体与分片规划 (纯逻辑, 不碰网络)
///
/// 依据官方文档 (§ 富媒体消息, 2026-10 版; 群聊与单聊两套接口的字段一致):
/// - 预上传 `POST /v2/{groups|users}/{id}/upload_prepare`
///   请求: `file_type`(整数) / `file_size`(**字符串**数字) / `file_name` /
///   `md5` / `sha1` / `md5_10m`
///   响应: `upload_id` / `block_size`(**字符串**数字) /
///   `parts[]{index(整数), presigned_url, block_size(字符串)}` / `upload_config`
/// - 分片完成 `POST .../upload_part_finish`
///   请求: `upload_id` / `part_index`(整数) / `block_size`(字符串, **该片实际字节数**) /
///   `md5`(该片 md5); 四项都要给, 只发前两项会被服务端当成坏请求 (实测 500)
/// - 合并 `POST .../files`
///   请求: `upload_id` / `file_name` / `file_type` / `srv_send_msg`
/// - 二进制直传 (≤ 5 MB) `POST .../files`
///   请求: `file_type` / `file_name` / `file_data` / `srv_send_msg`;
///   不能带 `upload_id` (带了服务端会走合并路径, 与 `file_data` 冲突)
///
/// 这些函数单独放在这里的原因: 平台把 64 位整数按**字符串**下发
/// (`"block_size": "5242880"`), 而 `utilxx_base::Json::value<int64_t>` 遇到字符串会
/// 返回默认值 0 —— 分片规划会静默退化成"一片装下整个文件"。逻辑放在不碰网络的函数里,
/// 才能用测试模块 `chatbot_media` 直接回归请求体与切片。
#include "utilxx_base/json.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace chatbot {

/// 一个文件的三段校验值 (预上传要求)
struct QqFileDigests {
    std::string md5;
    std::string sha1;
    /// 文件前 10002432 字节的 md5 (文件不足该长度时等于全文 md5)
    std::string md5_10m;
};

/// 取 JSON 里的 64 位整数
/// - 数字与字符串两种形态都接受 (字符串必须是纯十进制数字), 解析不出返回 0
/// - 相关: [qqJsonInt64Of]
int64_t qqJsonInt64(const utilxx_base::Json& value);

/// 取对象里某个键的 64 位整数
/// - `object` 不是对象 / 键不存在 / 解析不出时返回 `fallback`
/// - 相关: [qqJsonInt64]
int64_t
    qqJsonInt64Of(const utilxx_base::Json& object, std::string_view key, int64_t fallback);

/// 一片要上传的数据: 平台给的序号 + 预签名地址 + 该片在文件里的字节区间
struct QqUploadPartPlan {
    /// 平台给的分片序号 (原样回给 `upload_part_finish`; 官方文档写"从 0 开始",
    /// 但有的灰度环境从 1 开始, 所以这里只用平台给的值, 不自己推算)
    int         index = 0;
    std::string url;
    /// 该片在文件内容里的起始偏移
    size_t offset = 0;
    /// 该片要上传的字节数 (已按文件长度裁剪, 一定 > 0)
    size_t sizeBytes = 0;
};

/// 按预上传响应算出每一片的上传区间
/// - `block_size` / `parts[].block_size` 是字符串数字也照常解析; 都取不到时把整个文件当一片
/// - 分片按 `index` 升序处理 (偏移按该顺序累加, 平台给的排列顺序不可全信)
/// - 区间不会越过 `contentSize`; 越界部分裁掉, 裁成 0 字节的分片直接丢掉
/// - 返回空 = 平台没给分片清单 (调用方走二进制直传)
std::vector<QqUploadPartPlan>
    qqPlanUploadParts(const utilxx_base::Json& prepareJson, size_t contentSize);

/// 预上传请求体 (`fileSize` 会写成字符串, 与文档一致)
utilxx_base::Json qqBuildPrepareBody(
    int                 fileType,
    size_t              fileSize,
    std::string_view    fileName,
    const QqFileDigests& digests
);

/// 分片完成请求体
/// - `partSize` / `partMd5` 必须是该片**实际**上传的字节数与 md5 (不是平台预告的分块大小)
utilxx_base::Json qqBuildPartFinishBody(
    std::string_view uploadId,
    int              partIndex,
    size_t           partSize,
    std::string_view partMd5
);

/// 分片合并请求体
utilxx_base::Json qqBuildMergeBody(
    std::string_view uploadId,
    std::string_view fileName,
    int              fileType
);

/// 二进制直传请求体 (`base64Data` 为文件内容的 base64)
utilxx_base::Json qqBuildDirectUploadBody(
    std::string_view fileName,
    int              fileType,
    std::string_view base64Data
);

/// 二进制直传的大小上限 (官方文档: 超过 5 MB 必须走 URL 上传或分片上传)
constexpr size_t qqDirectUploadLimit = 5u * 1024u * 1024u;

} // namespace chatbot
} // namespace agentxx
