#pragma once

#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/use_awaitable.hpp"
#include "boost/system/error_code.hpp"
#include <memory>
#include <utility>

namespace agentxx {
namespace util {

/// 协程感知的互斥锁 (二元信号量)
/// - 用于在单线程协程 executor 上序列化临界区, 替代 std::mutex
/// - 背景: 在单线程 executor 上持 std::mutex 跨越 co_await 会死锁 —— 阻塞线程会使
///   持锁协程的完成回调永远无法运行; 本锁以 co_await 等待令牌, 不阻塞线程
/// - 线程安全: 基于 asio concurrent_channel, 亦可用于跨线程 acquire/release
/// - 用法:
///     auto guard = co_await mtx.lock();   // 获得锁 (RAII 守卫)
///     co_await do_async_work();           // 持锁跨越 co_await 也安全
///     // guard 离开作用域自动释放
class AsyncMutex {
private:

    using ErrorCode = neograph_asio_error_code;
    using Channel   = asio::experimental::concurrent_channel<void(ErrorCode)>;

public:

    explicit AsyncMutex(asio::any_io_executor ex) :
        chan_(std::make_shared<Channel>(std::move(ex), 1)) {
        chan_->try_send(ErrorCode{}); // 初始令牌: 未锁定
    }

    AsyncMutex(const AsyncMutex&)            = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;

    /// RAII 守卫: 析构时归还令牌 (释放锁)
    class Guard {
    public:

        explicit Guard(std::shared_ptr<Channel> ch) :
            ch_(std::move(ch)) {}

        Guard(Guard&& o) noexcept :
            ch_(std::move(o.ch_)) {}

        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;

        Guard& operator=(Guard&& o) noexcept {
            if (this != &o) {
                if (ch_) {
                    ch_->try_send(ErrorCode{}); // 归还本对象已持有的令牌, 避免泄漏导致死锁
                }
                ch_ = std::move(o.ch_);
            }
            return *this;
        }

        ~Guard() {
            if (ch_) {
                ch_->try_send(ErrorCode{}); // 归还令牌 (此时通道为空, 必成功)
            }
        }

    private:

        std::shared_ptr<Channel> ch_;
    };

    /// 获取锁: 无令牌时 co_await 挂起 (不阻塞线程), 获得后返回 RAII 守卫
    asio::awaitable<Guard> lock() {
        co_await chan_->async_receive(asio::use_awaitable);
        co_return Guard{chan_};
    }

private:

    std::shared_ptr<Channel> chan_;
};

} // namespace util
} // namespace agentxx
