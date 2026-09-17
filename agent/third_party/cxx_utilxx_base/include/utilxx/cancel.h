#pragma once

/// 协作式取消抽象 (跨库共享契约)
///
/// 本头是 `cxx_utilxx_base` 里少数的跨库共享契约之一 (命名空间 `utilxx`):
/// 插件框架内核 (`cxx_pluginxx`)、工具库 (`cxx_utilxx`) 与宿主项目都基于同一个
/// 取消令牌抽象工作, 因此它必须落在**无重依赖**的库内 (pluginxx 不允许引入
/// OpenSSL/SQLite 等重依赖)。
///
/// 两条传播路径 (与图引擎的取消语义一致):
/// 1. **轮询**: `isCancelled()` —— 循环/步骤边界自行检查, 只阻止后续工作;
/// 2. **asio 取消信号**: `slot()` —— 经 `asio::bind_cancellation_slot` 绑到协程上,
///    在途的 `co_await` (含网络 IO) 收到 `operation_aborted` 并干净展开。
///
/// 信号必须在持有它的执行器上 `emit` (asio 规则): `cancel()` 可从任意线程调用,
/// 它先置位轮询标志, 再把 emit `post` 到已绑定的执行器上。
///
/// 宿主可自行实现 [CancelToken] (适配任意取消源, 例如图引擎的取消令牌经
/// 适配器包装, 见 agentxx 的 `agentxx/util/cancel_adapter.h`), 也可直接使用
/// 默认实现 [SignalCancelToken]。

#include "utilxx_base/asio_error.h"
#include "asio/any_io_executor.hpp"
#include "asio/cancellation_signal.hpp"
#include "asio/post.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace utilxx {

/// 取消发生时抛出的异常
/// - 属于**控制流**而非错误: 上层异常分类器应把它识别为取消并原样向上传播
///   (见 utilxx_base/exception.h 的 classifyCurrentException)
class CancelledException : public std::runtime_error {
public:

    CancelledException() : std::runtime_error("run cancelled") {}

    explicit CancelledException(const std::string& detail) :
        std::runtime_error(detail.empty() ? std::string{"run cancelled"} : "run cancelled — " + detail) {
    }
};

/// 取消令牌抽象 (宿主可实现/适配任意取消源)
///
/// 约定:
/// - 除特别说明外, 各方法可从任意线程调用; `cancel()` 幂等 (`isCancelled()` 为 true
///   后再次调用不再触发信号);
/// - `fork()` 产生**子令牌**: 父令牌取消时级联取消全部存活子令牌, 每个子令牌有
///   自己的取消信号 (并发消费者之间互不覆盖对方的 slot);
/// - 令牌生命周期由 `std::shared_ptr` 管理; 直接构造的令牌由调用方负责存活到
///   已绑定的执行器处理完 post 的工作为止。
class CancelToken {
public:

    virtual ~CancelToken() = default;

    /// 轮询读取取消标志 (无锁, 可高频调用)
    [[nodiscard]] virtual bool isCancelled() const noexcept = 0;

    /// 请求取消 (线程安全, 幂等)
    virtual void cancel() noexcept = 0;

    /// asio 取消槽 (用于 `asio::bind_cancellation_slot` 绑定到协程)
    virtual asio::cancellation_slot slot() noexcept = 0;

    /// 绑定派发取消信号的执行器 (在协程首个 co_await 点调用; 可重复调用)
    virtual void bindExecutor(asio::any_io_executor ex) = 0;

    /// 创建级联子令牌 (父令牌取消时子令牌一并取消)
    [[nodiscard]] virtual std::shared_ptr<CancelToken> fork() = 0;

    /// 已取消则抛出 [CancelledException] (步骤边界上的便捷检查)
    void throwIfCancelled(std::string_view detail = {}) const {
        if (isCancelled()) {
            throw CancelledException{std::string{detail}};
        }
    }
};

/// 取消令牌的共享所有权别名 (跨边界传递统一使用该类型)
using CancelTokenPtr = std::shared_ptr<CancelToken>;

/// 默认取消令牌实现
///
/// 行为: 原子标志 + `asio::cancellation_signal`
/// - `cancel()` 幂等且线程安全: 立刻置位轮询标志, 再把信号 emit 投递到已绑定
///   的执行器 (未绑定执行器时仅置位标志, 之后 `bindExecutor` 会补发一次);
/// - `fork()` 出的子令牌带自己的信号与已取消状态继承 (父已取消 → 子立即处于
///   已取消状态), 父令牌经弱引用持有子令牌以便级联 (子令牌析构后自动剪除)。
class SignalCancelToken : public CancelToken {
public:

    SignalCancelToken() = default;

    SignalCancelToken(const SignalCancelToken&)            = delete;
    SignalCancelToken& operator=(const SignalCancelToken&) = delete;

    void cancel() noexcept override {
        if (cancelled_.exchange(true, std::memory_order_acq_rel)) {
            return; // 已取消, 幂等返回
        }

        // 快照执行器 (bindExecutor 与 cancel 可能跨线程竞争)
        asio::any_io_executor exSnapshot;
        {
            std::lock_guard<std::mutex> lk(mu_);
            exSnapshot = ex_;
        }
        if (exSnapshot) {
            // 投递信号: fork 出的子令牌记录了一个弱自引用, 可借它把自身保留到
            // emit 执行完 (直接构造的令牌返回空, 生命周期由调用方保证)
            auto keepAlive = selfKeepAliveForPost();
            asio::post(exSnapshot, [this, keepAlive = std::move(keepAlive)]() {
                sig_.emit(asio::cancellation_type::all);
            });
        }

        // 级联取消存活子令牌: 先加锁快照 shared_ptr, 再在锁外递归调用
        std::vector<std::shared_ptr<CancelToken>> liveChildren;
        {
            std::lock_guard<std::mutex> lk(childrenMu_);
            liveChildren.reserve(children_.size());
            for (auto& w : children_) {
                if (auto sp = w.lock(); sp && sp.get() != this) {
                    liveChildren.push_back(std::move(sp));
                }
            }
        }
        for (auto& child : liveChildren) {
            child->cancel();
        }
    }

    [[nodiscard]] bool isCancelled() const noexcept override {
        return cancelled_.load(std::memory_order_acquire);
    }

    asio::cancellation_slot slot() noexcept override {
        return sig_.slot();
    }

    void bindExecutor(asio::any_io_executor ex) override {
        bool fireImmediately = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ex_              = std::move(ex);
            fireImmediately  = cancelled_.load(std::memory_order_acquire);
        }
        if (!fireImmediately) {
            return;
        }
        // 绑定前已被取消 (或 fork 时继承的已取消状态): 补发一次信号,
        // 使刚 spawn 的协程在首个 co_await 点即可展开
        asio::any_io_executor exSnapshot;
        {
            std::lock_guard<std::mutex> lk(mu_);
            exSnapshot = ex_;
        }
        if (exSnapshot) {
            auto keepAlive = selfKeepAliveForPost();
            asio::post(exSnapshot, [this, keepAlive = std::move(keepAlive)]() {
                sig_.emit(asio::cancellation_type::all);
            });
        }
    }

    [[nodiscard]] std::shared_ptr<CancelToken> fork() override {
        auto child = std::shared_ptr<SignalCancelToken>(new SignalCancelToken());

        // 子令牌记录一个弱自引用: 投递信号时借它把自身保留到 emit 执行完
        // (cancel() 级联时跳过该自引用项)
        {
            std::lock_guard<std::mutex> lk(child->childrenMu_);
            child->children_.push_back(child);
        }
        {
            std::lock_guard<std::mutex> lk(childrenMu_);
            // 顺手剪除已失效的子令牌 (长期存活的父令牌不会无限增长)
            children_.erase(
                std::remove_if(
                    children_.begin(),
                    children_.end(),
                    [](const std::weak_ptr<CancelToken>& w) { return w.expired(); }
                ),
                children_.end()
            );
            children_.push_back(child);
        }

        // 父已取消 → 子立即处于已取消状态 (bindExecutor 时补发信号)
        if (cancelled_.load(std::memory_order_acquire)) {
            child->cancel();
        }
        return child;
    }

private:

    /// 取回自身 shared_ptr (仅 fork 出的令牌存有弱自引用项; 直接构造的返回空)
    std::shared_ptr<CancelToken> selfKeepAliveForPost() {
        std::lock_guard<std::mutex> lk(childrenMu_);
        for (auto& candidate : children_) {
            if (auto self = candidate.lock(); self.get() == this) {
                return self;
            }
        }
        return {};
    }

    std::atomic<bool>         cancelled_{false};
    mutable std::mutex        mu_;    // 保护 ex_ 与 cancel() 的竞争
    asio::any_io_executor     ex_;    // 由调用方在执行前绑定
    asio::cancellation_signal sig_;   // asio 操作取消信号

    /// 子令牌弱引用表 (fork 产生; 子令牌同时保留一个弱自引用项)
    mutable std::mutex                      childrenMu_;
    std::vector<std::weak_ptr<CancelToken>> children_;
};

} // namespace utilxx
