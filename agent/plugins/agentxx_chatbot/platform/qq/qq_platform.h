#pragma once

/// QQ 平台实现: 网关事件 → `InboundEvent`; `OutboundMessage` → openapi
///
/// 归一化的全部平台字段集中在本文件 (见 `resource/history/chatbot/plan.md` §12.1
/// 的风险对策: 解析失败只丢该条并 warn, 不影响连接)。
#include "../../chatbot_config.h"
#include "../platform.h"
#include "qq_api.h"
#include "qq_gateway.h"
#include "qq_media.h"

#include <memory>
#include <mutex>
#include <string>
namespace agentxx {
namespace chatbot {

/// QQ 平台连接 (一个账号一份)
class QqPlatform : public IPlatform {
public:

    QqPlatform(asio::any_io_executor ex, std::string dataDir);
    ~QqPlatform() override;

    std::string type() const override {
        return "qq";
    }

    PlatformCaps caps() const override;

    bool start(const AccountConfig& cfg, PlatformEventSink& sink) override;

    void stop() override;

    void send(OutboundMessage msg, SendResultFn done) override;

    std::string statusJson() const override;

    /// 下载一个入站附件 (落盘; 失败时保留 URL 由模型自己拉)
    void downloadAttachment(const std::string& accountId, MediaRef& ref) override;

    /// 媒体根目录 (stop 之后仍可查; 附件下载目标)
    const std::string& mediaRoot() const noexcept {
        return mediaRoot_;
    }

private:

    /// 一条网关事件 → 归一化; 解析失败返回 false
    bool normalize(
        std::string_view           eventName,
        const utilxx_base::Json&   data,
        InboundEvent&              out
    ) const;

    /// 群成员角色 (`member_role`); 平台不提供返回空
    static std::string roleOf(const utilxx_base::Json& author);

    /// 一次实际发送 (含附件上传; 协程)
    asio::awaitable<void> doSend(OutboundMessage msg, SendResultFn done);

    asio::any_io_executor  ex_;
    std::string            dataDir_;
    std::string            mediaRoot_;
    AccountConfig          config_;
    PlatformEventSink*     sink_ = nullptr;
    mutable std::mutex     mutex_;
    std::unique_ptr<QqApi>           api_;
    std::unique_ptr<QqGateway>       gateway_;
    std::unique_ptr<QqMediaUploader> uploader_;
    PlatformState          state_ = PlatformState::Connecting;
    std::string            stateReason_;
    uint64_t               inboundCount_ = 0;
    uint64_t               outboundCount_ = 0;
    uint64_t               sendFailCount_ = 0;
    bool                   stopped_ = false;
};

} // namespace chatbot
} // namespace agentxx
