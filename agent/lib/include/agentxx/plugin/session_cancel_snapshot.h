///
/// 内容:
/// - [SessionCancelSnapshot]: 会话取消状态的跨线程快照 (插件在自己的线程上查
///   `is_canceled` 时读它, 不碰会话对象)
///
#pragma once

#include "neograph/graph/cancel.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace agentxx {
namespace plugin {

/// 会话取消状态的跨线程快照
///
/// 为什么需要它: 会话与取消令牌归 IO 线程 (`Session::getCancelToken` 带 io 线程断言),
/// 而插件要在自己的线程上回答"这个会话是否已取消" —— 插件线程、JS 引擎线程都会问。
/// 让插件线程同步等一次 IO 会把插件卡住 (旧实现就是这么做的), 所以改成:
/// IO 线程在开轮时把「会话 id → 取消令牌」登记进来, 查询侧只锁一次这张小表,
/// 再去问令牌自己的原子标志 (`CancelToken::is_cancelled` 无锁)。
///
/// 线程约定:
/// - `bind` / `unbind` / `clear` 由 IO 线程调用 (与会话同一线程);
/// - `canceled` 任意线程可调用;
/// - 同一会话再次开轮会覆盖旧令牌 (令牌本身只对那一轮有效)。
class SessionCancelSnapshot {
public:
    /// 登记会话的取消令牌 (IO 线程)
    void bind(std::string_view sessionId, std::shared_ptr<neograph::graph::CancelToken> token) {
        if (sessionId.empty()) {
            return;
        }
        std::lock_guard lock(mutex_);
        tokens_[std::string{sessionId}] = std::move(token);
    }

    /// 摘除一个会话 (IO 线程; 会话关闭/删除时)
    void unbind(std::string_view sessionId) {
        std::lock_guard lock(mutex_);
        tokens_.erase(std::string{sessionId});
    }

    /// 是否已取消 (**任意线程**)
    /// - 未登记的会话按"未取消"回答: 没有开过轮, 也就没有取消这回事
    [[nodiscard]] bool canceled(std::string_view sessionId) const {
        if (sessionId.empty()) {
            return false;
        }
        std::shared_ptr<neograph::graph::CancelToken> token;
        {
            std::lock_guard lock(mutex_);
            auto            it = tokens_.find(std::string{sessionId});
            if (it == tokens_.end()) {
                return false;
            }
            token = it->second;
        }
        return token && token->is_cancelled();
    }

    /// 清空全部登记 (IO 线程; agent 收尾)
    void clear() {
        std::lock_guard lock(mutex_);
        tokens_.clear();
    }

    /// 已登记的会话数 (诊断/测试用)
    [[nodiscard]] size_t size() const {
        std::lock_guard lock(mutex_);
        return tokens_.size();
    }

private:
    mutable std::mutex                                                             mutex_;
    std::unordered_map<std::string, std::shared_ptr<neograph::graph::CancelToken>> tokens_;
};

} // namespace plugin
} // namespace agentxx
