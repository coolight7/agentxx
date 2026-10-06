#pragma once
/// 单次 provider 调用的取消域 (计划 LLM-7: 消费端退出时取消 provider)
///
/// 为什么需要独立一层取消令牌:
/// - 运行级令牌 (`NodeInput::ctx.cancel_token`) 是**整个轮次**的取消域: 用户点"取消"
///   或宿主关停时会取消它。若直接把运行令牌交给 provider, 消费方 (本节点) 中途放弃
///   一次调用时就没有办法只中止这一次在途请求 —— 要么继续读完整段响应 (白花额度),
///   要么把整轮都取消。
/// - 本类用 `CancelToken::fork()` 建立**子令牌**: 父令牌取消时照常级联到子令牌
///   (运行取消语义不变), 而子令牌单独取消不会影响父令牌 (只中止本次调用)。
///
/// 生命周期契约 (RAII):
/// - 调用正常返回后必须调用 [markDone]; 未标记完成就析构时, 析构函数会取消子令牌,
///   让 provider 侧的挂起等待/在途请求尽快结束 (见 [ProviderCallScope::markDone])。
/// - 子令牌是 `shared_ptr` 且由 fork 自带弱自引用, `cancel()` 投递的信号发射会保活
///   到执行完, 因此析构即取消是安全的。
///
/// 边界: 本类只负责"取消域", 不做超时竞速 (TOOL-4 分发层硬超时经人工核定不做)。
#include "neograph/graph/cancel.h"
#include <memory>
#include <utility>

namespace agentxx {
namespace nodes {

/// 一次 provider 调用的取消域 (见文件头说明)
class ProviderCallScope {
public:

    /// 以父令牌建立调用域; 父令牌为空时使用独立令牌 (不会被外部取消)
    explicit ProviderCallScope(std::shared_ptr<neograph::graph::CancelToken> parent = nullptr) {
        token_ = parent ? parent->fork() : std::make_shared<neograph::graph::CancelToken>();
    }

    ProviderCallScope(const ProviderCallScope&)            = delete;
    ProviderCallScope& operator=(const ProviderCallScope&) = delete;

    ~ProviderCallScope() {
        // 消费方放弃 (未 markDone): 只取消本次调用, 不影响运行级令牌
        if (!done_ && token_) {
            token_->cancel();
        }
    }

    /// 标记本次调用已正常结束 (成功或已按错误路径收尾)
    /// - 调用方应在 `co_await` 返回后立即调用; 未调用即析构会被视为"消费方放弃"
    void markDone() noexcept {
        done_ = true;
    }

    /// 交给 provider 的取消令牌 (即 `CompletionParams::cancel_token`)
    const std::shared_ptr<neograph::graph::CancelToken>& token() const noexcept {
        return token_;
    }

    /// 本次调用是否已处于取消状态 (子令牌被取消) —— 诊断/测试用
    bool cancelled() const noexcept {
        return token_ && token_->is_cancelled();
    }

private:

    std::shared_ptr<neograph::graph::CancelToken> token_;
    bool                                          done_ = false;
};

} // namespace nodes
} // namespace agentxx
