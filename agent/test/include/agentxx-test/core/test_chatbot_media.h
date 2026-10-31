#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// `chatbot_media` —— QQ 富媒体上传的请求体与分片规划
/// (`platform/qq/qq_media_request.cpp`)
///
/// 这一层专门守住三类"按文档猜就会错"的地方 (都真实发生过):
/// - 平台把 64 位整数按**字符串**下发 (`"block_size": "10485760"`), 按 JSON 数字取会静默
///   拿到 0, 分片规划整体退化成"一片装下整个文件";
/// - `upload_part_finish` 必须带该片的 `block_size` 与 `md5`, 只发 `upload_id` +
///   `part_index` 会被服务端判成坏请求 (实测 500, 附件发不出去);
/// - 二进制直传不能与 `upload_id` 同时出现, 否则服务端走合并路径。
///
/// 直接 include 插件头 (`agent/plugins/` 已是测试 include 根), 不碰网络。
TestResult testChatbotMedia();

} // namespace test
} // namespace agentxx
