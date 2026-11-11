/// 取消抽象互适配 (主程序图引擎 <-> 统一取消抽象 utilxx::CancelToken)
///
/// 背景: 插件框架 (`cxx_pluginxx`) 与卸载工具 (`utilxx/async_offload.h`) 使用
/// `utilxx::CancelToken` 抽象 (不依赖图引擎), 而 agentxx 的会话取消令牌是
/// `neograph::graph::CancelToken`。本头提供两个方向的衔接:
/// - **令牌适配**: [adaptCancelToken] 把图引擎令牌包装成 `utilxx::CancelToken`,
///   使同一份取消意图在两条路径上表现一致 (轮询标志 / asio 信号 / 执行器绑定 /
///   fork 级联);
/// - **取消换算**: [awaitHostPluginOpResult] 把插件框架的等待取消换算成状态
///   `PLUGINXX_OPERATOR_CANCELED` (业务终态原样返回, 统一经 [requirePluginOpOk] 判定)。
///
/// 用法:
/// ```c++
/// auto token = agentxx::util::adaptCancelToken(sessionCancelToken); // 可空
/// co_await agentxx::util::awaitHostPluginOpResult(args);           // 主程序侧等待
/// ```
///
/// 注意: 令牌适配只做转发, 不改变原令牌的语义; 原令牌生命周期由调用方保证
/// (适配器持有 shared_ptr, 与原令牌共享所有权)。
#pragma once

#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/cancellation_signal.hpp"
#include "neograph/graph/cancel.h"
#include "pluginxx/runtime/op_driver.h"
#include "utilxx/cancel.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace agentxx {
namespace util {

/// 包装图引擎取消令牌, 对外提供 utilxx::CancelToken 接口
class NeographCancelTokenAdapter final : public utilxx::CancelToken {
public:

    explicit NeographCancelTokenAdapter(std::shared_ptr<neograph::graph::CancelToken> token) :
        token_(std::move(token)) {}

    [[nodiscard]] bool isCanceled() const noexcept override {
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
            // 只做转发: 绑定后的 strand 由被包装令牌内部保存, 这里不需要在它上面
            // 启动协程, 因此显式丢弃返回值
            static_cast<void>(token_->bind_executor(std::move(ex)));
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
inline utilxx::CancelTokenPtr adaptCancelToken(std::shared_ptr<neograph::graph::CancelToken> token
) {
    if (!token) {
        return nullptr;
    }
    return std::make_shared<NeographCancelTokenAdapter>(std::move(token));
}

/// 以主程序 (图引擎) 取消语义等待一次插件 Operation
///
/// 背景: 插件框架 (`cxx_pluginxx`) 的等待被取消时抛 `utilxx::CanceledException`,
/// 而 agentxx 的 tool / 图节点 / 中间件层全部按 `neograph::graph::CancelledException`
/// 判定取消 (例如 toolcall 节点显式 catch 该类型以避免取消被当成普通工具错误吞掉)。
/// 因此在主程序边界统一换算, 使拆分前后的取消判定规则完全一致。
///
/// - 业务终态 (OK / FAILED / CANCELED) **原样返回**, 不转成异常;
/// - 等待本身被取消 (协程取消槽生效) 换算为状态 `PLUGINXX_OPERATOR_CANCELED`
///   (工具结果因此得到 `ToolStatus::Canceled`), 不向上抛取消异常;
/// - 其余异常 (实例已关闭 / 已禁用等一致性问题) 照旧传播 —— 那不是工具执行结果。
///
/// 语义与 [pluginxx::awaitPluginOpResult] 一致, 只多一层取消换算。
template<typename ArgsT>
inline asio::awaitable<pluginxx::PluginOpResult> awaitHostPluginOpResult(ArgsT args) {
    try {
        co_return co_await pluginxx::awaitPluginOpResult(std::move(args));
    } catch (const utilxx::CanceledException& e) {
        co_return pluginxx::PluginOpResult{PLUGINXX_OPERATOR_CANCELED, e.what()};
    }
}

/// "非 OK 就抛" 的统一入口 (图节点 / 扩展点派发)
/// - `PLUGINXX_OPERATOR_OK` -> 返回 payload
/// - `PLUGINXX_OPERATOR_CANCELED` -> 抛 `neograph::graph::CancelledException` (取消是控制流)
/// - 其余 -> 抛 `std::runtime_error(payload)`
inline std::string requirePluginOpOk(pluginxx::PluginOpResult result) {
    if (result.status == PLUGINXX_OPERATOR_OK) {
        return std::move(result.payload);
    }
    if (result.status == PLUGINXX_OPERATOR_CANCELED) {
        throw neograph::graph::CancelledException(
            result.payload.empty() ? std::string{"plugin operation canceled"}
                                   : std::move(result.payload)
        );
    }
    throw std::runtime_error(
        result.payload.empty() ? std::string{"plugin operation failed"} : std::move(result.payload)
    );
}

} // namespace util
} // namespace agentxx
