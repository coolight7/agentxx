#pragma once

/// 内存假平台: 脚本化事件注入 + 记录出站
///
/// 用途 (见 `resource/history/chatbot/plan.md` §15 阶段 2):
/// - 端到端测试里替代真 QQ: 把事件直接投给编排层, 并记录插件发出来的东西;
/// - 本地手工演示 (不连平台, 不消耗真实账号)。
///
/// 行为全部可配 (`behavior.fake` / `accounts[].args`):
/// - `delayMs`: 每次发送延迟多久才回调 (模拟网络耗时);
/// - `failSend`: 前 N 次发送直接失败;
/// - 账号 id 以 `fake` 开头时也自动用本实现 (见 `createPlatform`)。
#include "../platform.h"

#include "asio/any_io_executor.hpp"
#include "asio/steady_timer.hpp"

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agentxx {
namespace chatbot {

/// 假平台记录的一条出站
struct FakeSentRecord {
    OutboundMessage message;
    /// 平台分配的 id (`fake-<序号>`)
    std::string platformMessageId;
    /// 平台分配的引用句柄 (`REFIDX_fake-<序号>`)
    std::string refIdx;
};

/// 假平台
class FakePlatform : public IPlatform {
public:

    explicit FakePlatform(asio::any_io_executor ex);

    std::string type() const override {
        return "fake";
    }

    PlatformCaps caps() const override;

    bool start(const AccountConfig& cfg, PlatformEventSink& sink) override;

    void stop() override;

    void send(OutboundMessage msg, SendResultFn done) override;

    std::string statusJson() const override;

    /* ---------- 测试/演示用 ---------- */

    /// 注入一条入站消息 (归一化后直接交给 sink)
    void inject(InboundEvent event);

    /// 注入一条文本消息 (常用形态)
    void injectText(
        ChatType    type,
        std::string chatId,
        std::string senderName,
        std::string text,
        bool        mentionedMe  = false,
        std::string msgId        = {},
        int64_t     timestampMs  = 0
    );

    /// 已发出的消息 (按顺序)
    std::vector<FakeSentRecord> sent() const;

    /// 清空记录
    void clearSent();

    /// 配置发送延迟 (毫秒)
    void setSendDelayMs(int32_t delayMs);

    /// 让接下来的 N 次发送失败
    void setFailCount(int32_t count);

    /// 连接状态是否已置为 online
    bool online() const;

    /// 手动改连接状态 (测试状态机用)
    void setOnline(bool online);

private:

    asio::any_io_executor  ex_;
    PlatformEventSink*     sink_ = nullptr;
    AccountConfig          config_;
    mutable std::mutex     mutex_;
    std::vector<FakeSentRecord> sent_;
    int64_t                nextId_       = 0;
    int32_t                sendDelayMs_  = 0;
    int32_t                failCount_    = 0;
    bool                   online_       = false;
    bool                   stopped_      = false;
};

/// 通过类型名造一个假平台 (测试里直接调)
std::shared_ptr<FakePlatform>
    makeFakePlatform(asio::any_io_executor ex);

} // namespace chatbot
} // namespace agentxx
