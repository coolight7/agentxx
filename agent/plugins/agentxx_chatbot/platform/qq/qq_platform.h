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
#include "qq_normalize.h"

#include <memory>
#include <mutex>
#include <string>
namespace agentxx {
namespace chatbot {

struct ChatbotNet;

/// QQ 平台连接 (一个账号一份)
class QqPlatform : public IPlatform {
public:

    QqPlatform(asio::any_io_executor ex, std::string dataDir, const ChatbotNet* net);
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

    /// 撤回自己发出的一条消息 (平台限制发送后 2 分钟内)
    RecallResult recallMessage(
        const std::string& accountId,
        ChatType           type,
        const std::string& chatId,
        const std::string& messageId
    ) override;

    /// 媒体根目录 (stop 之后仍可查; 附件下载目标)
    const std::string& mediaRoot() const noexcept {
        return mediaRoot_;
    }

private:

    /// 一条网关事件 → 归一化; 解析失败返回 false
    ///
    /// 真正的字段解析在 `qq_normalize.cpp` (纯函数, 不碰网络, 便于用真实事件做回归);
    /// 本方法负责提供/写回自己的身份表并统计 @ 判定依据 (因此不是 const)
    bool normalize(
        std::string_view           eventName,
        const utilxx_base::Json&   data,
        InboundEvent&              out
    );

    /// 按事件名计数 (判断平台到底有没有推“非 @ 消息”)
    void noteDispatchEvent(std::string_view eventName);

    /// 从机器人自己消息的回声里学习它在当前群的 openid
    void learnSelfIdentity(const utilxx_base::Json& data);

    /// 一次实际发送 (含附件上传; 协程)
    asio::awaitable<void> doSend(OutboundMessage msg, SendResultFn done);

    asio::any_io_executor  ex_;
    std::string            dataDir_;
    std::string            mediaRoot_;
    /// 插件网络入口 (插件实例持有; 本对象不比它活得久)
    const ChatbotNet*      net_ = nullptr;
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
    /// 按事件名统计的原始条数: 全量消息为 0 就说明平台没给“接收所有消息”权限
    uint64_t               fullModeCount_   = 0;
    uint64_t               atMessageCount_  = 0;
    uint64_t               c2cMessageCount_ = 0;
    /// 机器人自己的身份 (全量消息里判定“@ 的是不是我”; 随 READY / 回声逐步攒)
    QqSelfIdentity         selfIdentity_;
    /// @ 判定依据的计数器 (排查“@ 别人却回了一句”用)
    ///
    /// - `mentionByEventType_`: 平台直接给了「@机器人」事件
    /// - `mentionByOpenid_`: `mentions[]` / 正文标记命中已知自身 openid
    /// - `mentionByNickname_`: 比不到 openid, 但 `mentions[]` 里的昵称就是机器人昵称
    /// - `mentionSelfUnknown_`: 全量消息里 @ 了机器人但还没法认出来 (身份表空)
    uint64_t               mentionByEventType_  = 0;
    uint64_t               mentionByOpenid_     = 0;
    uint64_t               mentionByNickname_   = 0;
    uint64_t               mentionSelfUnknown_ = 0;
    bool                   stopped_ = false;
};

} // namespace chatbot
} // namespace agentxx
