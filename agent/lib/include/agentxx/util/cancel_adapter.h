/// neograph 取消令牌 -> utilxx::CancelToken 适配器
///
/// 背景: 插件框架内核 (`cxx_pluginxx`) 与卸载工具 (utilxx/async_offload.h) 使用
/// `utilxx::CancelToken` 抽象 (不依赖图引擎), 而 agentxx 的会话取消令牌是
/// `neograph::graph::CancelToken`。本适配器把后者包装成前者, 使同一份取消意图
/// 在两条链路上表现一致 (轮询标志 / asio 信号 / 执行器绑定 / fork 级联)。
///
/// 用法:
/// ```c++
/// auto token = agentxx::util::adaptCancelToken(sessionCancelToken); // 可空
/// co_await pluginxx::runtime...::run(token, ...);
/// ```
///
/// 注意: 适配器只做转发, 不改变原令牌的语义; 原令牌生命周期由调用方保证
/// (适配器持有 shared_ptr, 与原令牌共享所有权)。
#pragma once

#include "utilxx/cancel.h"
#include "asio/any_io_executor.hpp"
#include "asio/cancellation_signal.hpp"
#include "neograph/graph/cancel.h"
#include <memory>
#include <utility>

namespace agentxx {
namespace util {

/// 包装图引擎取消令牌, 对外提供 utilxx::CancelToken 接口
class NeographCancelTokenAdapter final : public utilxx::CancelToken {
public:

    explicit NeographCancelTokenAdapter(std::shared_ptr<neograph::graph::CancelToken> token) :
        token_(std::move(token)) {}

    [[nodiscard]] bool isCancelled() const noexcept override {
        return token_ && token_->is_cancelled();
    }

    void cancel() noexcept override {
        if (token_) {
            token_->cancel();
        }
    }

    asio::cancellation_slot slot() noexcept override {
        return token_ ? token_->slot() : asio::cancellation_slot{};
    }

    void bindExecutor(asio::any_io_executor ex) override {
        if (token_) {
            token_->bind_executor(std::move(ex));
        }
    }

    /// 子令牌同样包装 (图引擎的 fork 已实现级联取消)
    [[nodiscard]] std::shared_ptr<utilxx::CancelToken> fork() override {
        if (!token_) {
            return nullptr;
        }
        return std::make_shared<NeographCancelTokenAdapter>(token_->fork());
    }

    /// 取出被包装的图引擎令牌 (需要原始类型时使用)
    const std::shared_ptr<neograph::graph::CancelToken>& neographToken() const noexcept {
        return token_;
    }

private:

    std::shared_ptr<neograph::graph::CancelToken> token_;
};

/// 把图引擎取消令牌适配为 utilxx::CancelTokenPtr
/// - token 为空时返回 nullptr (调用方按"无取消"处理, 与拆分前一致)
inline utilxx::CancelTokenPtr adaptCancelToken(std::shared_ptr<neograph::graph::CancelToken> token) {
    if (!token) {
        return nullptr;
    }
    return std::make_shared<NeographCancelTokenAdapter>(std::move(token));
}

} // namespace util
} // namespace agentxx
