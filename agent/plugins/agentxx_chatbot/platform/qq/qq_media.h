#pragma once

/// QQ 富媒体: 格式判定 / 校验值 / 分片上传
///
/// 事实与流程见 `resource/history/chatbot/plan.md` §3.6 / §11.3:
/// - 发送前必须先上传拿 `file_info`, 再用 `msg_type=7` 发出;
/// - `file_type`: 1 图片 / 2 视频 / 3 语音 / 4 文件;
///   软限制 20/30/20/200 MB, 硬限制都是 200 MB; 超软限制降级成文件;
/// - 分片上传: `upload_prepare` (给 md5/sha1/md5_10m) → 逐片 `PUT` 到预签名地址
///   → `upload_part_finish` → `POST .../files {upload_id, file_name, srv_send_msg:false}`
///   拿 `file_info`; `file_info` 带 ttl, 过期内重发不重传;
/// - 群与单聊的上传结果**不互通** (缓存键里带 chatId 类型)。
///
/// 线程: 全部在插件 io 线程上跑。
#include "../../chatbot_types.h"

#include "asio/awaitable.hpp"
#include "utilxx_base/json.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asio = ::boost::asio;

namespace agentxx {
namespace chatbot {

class QqApi;

/// 一个文件的三段校验值 (分片上传 prepare 要求)
struct QqFileDigests {
    std::string md5;
    std::string sha1;
    /// 前 10 MB 的 md5 (文件不足 10 MB 时等于全文 md5)
    std::string md5_10m;
};

/// 上传结果
struct QqUploadResult {
    bool        ok = false;
    /// 失败原因 (可直接给模型看)
    std::string reason;
    /// 平台给的 `file_info` (msg_type=7 时用)
    std::string fileInfo;
    /// 实际使用的 file_type (超软限制时会降级成 4)
    int         fileType = 4;
};

/// 按扩展名 + mime 判定 `file_type` (1 图片 / 2 视频 / 3 语音 / 4 文件)
int qqFileTypeOf(std::string_view fileName, std::string_view mimeType) noexcept;

/// 该类型的软限制与硬限制 (字节; 用于降级与拒绝判断)
uint64_t qqSoftLimitOf(int fileType) noexcept;
uint64_t qqHardLimitOf(int fileType) noexcept;

/// 计算三段校验值 (纯内存, 调用方负责读文件)
QqFileDigests qqComputeDigests(std::string_view content);

/// 分片上传器 (每个账号一份; 内部缓存 file_info)
class QqMediaUploader {
public:

    QqMediaUploader(QqApi& api, std::string accountId);

    /// 上传一个本地文件, 拿 `file_info`
    ///
    /// - `chatType` / `chatId`: 群与单聊的上传结果不互通, 缓存键里带上它们
    /// - `textContent`: 直接给内容 (测试与"内存里的附件"用; 为空时读 `localPath`)
    asio::awaitable<QqUploadResult> upload(
        const MediaRef&  ref,
        ChatType         chatType,
        std::string_view chatId,
        std::string_view textContent = {}
    );

    /// 清空缓存 (账号重连时用)
    void clearCache();

private:

    struct CacheEntry {
        std::string fileInfo;
        int64_t     expireAtMs = 0;
        int         fileType   = 4;
    };

    /// 上传路径前缀: 群用 `/v2/groups/{id}` 单聊用 `/v2/users/{id}`
    static std::string basePathOf(ChatType chatType, std::string_view chatId);

    std::string cacheKeyOf(
        ChatType         chatType,
        std::string_view chatId,
        std::string_view localPath,
        int64_t          mtimeMs,
        int              fileType
    ) const;

    QqApi&                                 api_;
    std::string                            accountId_;
    std::map<std::string, CacheEntry, std::less<>> cache_;
};

} // namespace chatbot
} // namespace agentxx
