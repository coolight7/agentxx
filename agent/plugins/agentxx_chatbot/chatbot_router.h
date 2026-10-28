#pragma once

/// `ChatKey` → 人设 + 通道 (会话) 的路由与通道池
///
/// 职责 (见 `resource/history/chatbot/plan.md` §6):
/// - 由配置一次性建好路由表 (`RouterTable`, 在 `chatbot_config.h`), 运行期只读;
/// - 每个「平台账号 + 群或私聊」一个会话: `sessionId` 由 `ChatKey` 确定性算出,
///   跨重启稳定 (同一个群永远同一个会话);
/// - 通道**懒创建** (第一条要投递的消息才开), 活跃数达到 `maxChatsPerPersona`
///   时按「最久没有新消息」回收一条 (会话上下文在会话库里, 重开无损)。
///
/// 线程: 全部入口只在插件 io 线程调用 (编排层与平台层共用同一条), 因此本类不加锁。
#include "chatbot_config.h"
#include "chatbot_types.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace chatbot {

/// 一个会话的通道记录
struct ChannelEntry {
    int64_t     channelId = 0;
    /// 最近一次有消息的序号 (LRU 依据)
    uint64_t    lastActiveSeq = 0;
    ChatMode    mode          = ChatMode::Mention;
    /// 通道是否已就绪 (收到 READY)
    bool        ready = false;
    /// 是否正在跑一轮
    bool        busy = false;
    std::string sessionId;
};

/// 路由 + 通道池
class ChatbotRouter {
public:

    /// 运行时回调 (由编排层提供; 转发到 `agentxx.agent.runtime` 的 SDK 调用)
    struct Hooks {
        /// 打开一个会话通道; 失败返回 0
        std::function<int64_t(std::string_view personaName, std::string_view sessionId,
                              std::string_view title)>
            open;
        /// 关闭一个通道 (幂等)
        std::function<void(int64_t channelId)> close;
    };

    /// 建表 (配置在启动后不可变)
    void build(const ChatbotConfig& config, int32_t maxChatsPerPersona, Hooks hooks);

    /// 路由一条消息
    RouteResult route(const ChatKey& key) const;

    /// 取该会话的通道; 没有就懒创建 (必要时按 LRU 回收一条)
    ///
    /// - `seq`: 单调递增的活动序号 (LRU 依据)
    /// - `err`: 失败原因 (达上限且无法回收 / 开通道被拒)
    /// - `return` 通道 id; 0 = 失败
    int64_t acquire(
        const ChatKey&      key,
        const RouteResult&  route,
        std::string_view    chatTitle,
        uint64_t            seq,
        std::string&        err
    );

    /// 按会话键取已存在的通道 (0 = 没有)
    int64_t find(std::string_view chatKey) const;

    /// 某个人设对接的会话键 (已配置的精确项)
    std::vector<std::string> chatKeysOf(std::string_view personaName) const;

    /// 按通道 id 找到会话键 (找不到返回空串)
    std::string chatKeyOfChannel(int64_t channelId) const;

    /// 按会话 id 找到会话键 (工具用 `thread_id` 反查)
    std::string chatKeyOfSession(std::string_view sessionId) const;

    /// 通道状态更新 (READY / 忙碌)
    void markReady(int64_t channelId, bool ready);
    void markBusy(int64_t channelId, bool busy);

    /// 运行期改某个会话的触发模式 (工具 `chatbot_set_mode`, 不写回 yaml)
    void setModeOverride(std::string_view chatKey, ChatMode mode);

    bool isBusy(int64_t channelId) const;
    bool isReady(int64_t channelId) const;

    /// 触碰活动时间 (LRU)
    void touch(int64_t channelId, uint64_t seq);

    /// 关闭一个通道 (记录保留, 下次需要时重开)
    void release(int64_t channelId, std::string_view reason);

    /// 关闭全部通道 (stop 时用); 返回关闭的通道数
    size_t releaseAll(std::string_view reason);

    /// 该人设当前活跃通道数
    size_t activeCount(std::string_view personaName) const;

    /// 某会话键对应的 sessionId (未开通道也能算出来)
    std::string sessionIdOf(std::string_view personaName, const ChatKey& key) const;

    /// 会话标题 (群名 / 用户名; 空则用 id 前 8 位)
    static std::string titleOf(std::string_view chatName, const ChatKey& key);

    /// 已建通道总数 (诊断)
    size_t channelCount() const noexcept {
        return entries_.size();
    }

    const ChatbotConfig& config() const noexcept {
        return config_;
    }

    /// 配置里的全部人设名 (按配置顺序)
    std::vector<std::string> personaNames() const;

    /// 人设 → root id (未挂成功的人设不在表里)
    void setRootId(std::string_view personaName, std::string rootId);
    std::string rootIdOf(std::string_view personaName) const;

    /// 记录一份会话信息 (chatName 等, 供 `chatbot_chat_info`)
    void rememberChatName(std::string_view chatKey, std::string_view chatName);

    /// 取记录 (没有返回 nullptr)
    ChannelEntry*       entry(std::string_view chatKey);
    const ChannelEntry* entry(std::string_view chatKey) const;

private:

    /// 回收该人设最久未活动的一条通道; 成功返回 true
    bool evictLru(std::string_view personaName, std::string& err);

    ChatbotConfig                                          config_;
    RouterTable                                            table_;
    int32_t                                                maxChatsPerPersona_ = 64;
    Hooks                                                  hooks_;
    std::map<std::string, ChannelEntry, std::less<>>        entries_; ///< 键 = ChatKey::key()
    std::map<int64_t, std::string>                          channelToKey_;
    std::map<std::string, std::string, std::less<>>         rootIds_;
    std::map<std::string, std::string, std::less<>>         chatNames_;
};

} // namespace chatbot
} // namespace agentxx
