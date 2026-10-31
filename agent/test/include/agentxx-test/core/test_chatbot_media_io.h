#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// `chatbot_media_io` —— QQ 富媒体上传的完整流程 (本地模拟服务端)
///
/// `chatbot_media` 只验证请求体与分片规划这些纯逻辑; 这里再把
/// `QqMediaUploader::upload()` 整条调用路径跑一遍: 预上传 → 逐片 PUT →
/// 分片完成 → 合并, 以及平台不给分片清单时的二进制直传。
///
/// 模拟服务端按官方文档**严格校验**分片完成请求: `upload_id` / `part_index` /
/// `block_size`(字符串, 等于该片字节数) / `md5`(等于该片内容 md5) 缺一项或对不上
/// 都回 500 —— 这正是"机器人主动发文件提示 upload_part_finish 失败 (status 500)"
/// 的那个错。另外覆盖 5xx 重试、4xx 不重试、`file_info` 缓存命中。
asio::awaitable<TestResult> testChatbotMediaIo();

} // namespace test
} // namespace agentxx
