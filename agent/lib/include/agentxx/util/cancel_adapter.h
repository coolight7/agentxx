/// 取消抽象互适配 (宿主图引擎 <-> 统一取消抽象 utilxx::CancelToken)
///
/// 背景: 插件框架内核 (`cxx_pluginxx`) 与卸载工具 (`utilxx/async_offload.h`) 使用
/// `utilxx::CancelToken` 抽象 (不依赖图引擎), 而 agentxx 的会话取消令牌是
/// `neograph::graph::CancelToken`。本头提供两个方向的衔接:
/// - **令牌适配**: [adaptCancelToken] 把图引擎令牌包装成 `utilxx::CancelToken`,
///   使同一份取消意图在两条链路上表现一致 (轮询标志 / asio 信号 / 执行器绑定 /
///   fork 级联);
/// - **异常适配**: [awaitHostPluginOp] 把内核抛出的 `utilxx::CancelledException`
///   转换为宿主的 `neograph::graph::CancelledException`, 保持宿主侧 (tool / 图节点 /
///   中间件) 的取消判定口径与拆分前一致。
///
/// 用法:
/// ```c++
/// auto token = agentxx::util::adaptCancelToken(sessionCancelToken); // 可空
/// co_await agentxx::util::awaitHostPluginOp(args);                  // 宿主侧等待
/// ```
///
/// 注意: 令牌适配只做转发, 不改变原令牌的语义; 原令牌生命周期由调用方保证
/// (适配器持有 shared_ptr, 与原令牌共享所有权)。
#pragma once

#include "utilxx/cancel.h"
#include "pluginxx/runtime/op_driver.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/cancellation_signal.hpp"
#include "neograph/graph/cancel.h"
#include <memory>
#include <string>
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

/// 以宿主 (图引擎) 取消语义等待一次插件 Operation
///
/// 背景: 插件框架内核 (`cxx_pluginxx`) 按统一取消抽象抛 `utilxx::CancelledException`,
/// 而 agentxx 的 tool / 图节点 / 中间件层全部按 `neograph::graph::CancelledException`
/// 判定取消 (例如 toolcall 节点显式 catch 该类型以避免取消被当成普通工具错误吞掉)。
/// 因此在宿主边界统一转换, 使拆分前后的取消判定口径完全一致。
///
/// - 其余语义 (完成协议 / lease / 取消传播) 与 [pluginxx::awaitPluginOp] 完全一致;
/// - 无取消时异常原样传播。
template<typename ArgsT>
inline asio::awaitable<std::string> awaitHostPluginOp(ArgsT args) {
    try {
        co_return co_await pluginxx::awaitPluginOp(std::move(args));
    } catch (const utilxx::CancelledException& e) {
        throw neograph::graph::CancelledException(e.what());
    }
}

} // namespace util
} // namespace agentxx
