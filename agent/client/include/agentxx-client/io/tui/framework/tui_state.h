#pragma once

#include "agentxx/agent/context.h"
#include "neograph/api.h"
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/// TUI 消息模型 (从 TUIClientAgentIO 提取, 供各组件共享)
struct TUIMessage {
    enum class Role {
        User,
        Assistant,
        Thinking,
        System,
        Tool
    };
    Role        role;
    std::string text;
    std::string toolName;
    std::string toolCallId;
    std::string toolResult;
    bool        toolFinished = false;
    bool        collapsed    = false;
    int64_t     durationMs   = 0;
    int64_t     startTimeMs  = 0;
};

/// 排队等待发送的用户输入
struct TUIPendingInput {
    std::string text;
    bool        expanded = false;
};

/// 跨线程共享的渲染状态 (COW 语义)
///
/// 所有被 client 线程 (onDelta/onSync) 和 UI 线程 (渲染/事件) 并发访问的数据
/// 收敛于此结构, 经 shared_ptr + mutex 保护:
/// - 写方: 短锁 + COW (mutableState) 后修改
/// - 读方: 短锁拷贝 shared_ptr 快照, 之后无锁渲染
struct TUIRenderState {
    std::vector<std::shared_ptr<TUIMessage>> messages;
    std::string                              currentToken;
    TUIMessage::Role                         currentTokenRole = TUIMessage::Role::Assistant;
    bool                                     isStreaming      = false;

    int64_t pendingTokenDurationMs  = 0;
    int64_t pendingTokenStartTimeMs = 0;

    std::string currentNodeName;

    std::vector<std::string> modelNames;
    std::string              cachedModelName;

    std::deque<TUIPendingInput> pendingInputs;

    neograph::json contextMessages    = neograph::json::array();
    bool           showContextOverlay = false;

    std::vector<agentxx::agent::AppendComponentNotification> appendComponents;
};

/// COW 共享状态容器 (封装 mutex + shared_ptr + COW 辅助)
///
/// 线程模型:
/// - client 线程: 经 mutableState() 短锁 COW 写入
/// - UI 线程渲染: 经 snapshot() 短锁拷贝 shared_ptr, 之后无锁读取
/// - UI 线程事件: 经 mutableState() 短锁 COW 写入
class TUISharedState {
public:

    TUISharedState() :
        state_(std::make_shared<TUIRenderState>()) {}

    /// COW 写入: 获取可写引用; 若被快照共享 (use_count > 1) 则深拷贝结构
    /// 调用方须持有 lock()
    TUIRenderState& mutableState() {
        if (state_.use_count() > 1) {
            state_ = std::make_shared<TUIRenderState>(*state_);
        }
        return *state_;
    }

    /// COW 写入单条消息: 若被快照共享则拷贝该条
    /// 调用方须持有 lock()
    TUIMessage& mutableMessage(TUIRenderState& st, size_t idx) {
        if (st.messages[idx].use_count() > 1) {
            st.messages[idx] = std::make_shared<TUIMessage>(*st.messages[idx]);
        }
        return *st.messages[idx];
    }

    /// 快照: 拷贝 shared_ptr (纳秒级), 供 UI 线程本帧无锁渲染
    /// 调用方须持有 lock()
    std::shared_ptr<TUIRenderState> snapshot() {
        return state_;
    }

    std::mutex& mutex() {
        return mutex_;
    }

    /// 便捷: 加锁 + COW 写入 + 解锁
    void mutate(std::function<void(TUIRenderState&)> fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        fn(mutableState());
    }

    /// 便捷: 加锁 + 读取快照 + 解锁
    std::shared_ptr<TUIRenderState> readSnapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot();
    }

private:

    std::mutex                      mutex_;
    std::shared_ptr<TUIRenderState> state_;
};
