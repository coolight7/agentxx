#pragma once

#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "neograph/api.h"
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>

/// 异步 stdin 读取器
/// - 在独立线程上阻塞读取 std::cin, 通过 channel 提供行给 io_context 协程
/// - 避免在单线程 io_context 上直接 std::getline 阻塞整个事件循环
/// - 进程内单例, 多个 handler 共享同一读取线程与 channel
class StdinReader {
public:

    using LineChannel
        = asio::experimental::concurrent_channel<void(neograph_asio_error_code, std::string)>;

private:

    std::shared_ptr<LineChannel> channel_;
    std::thread                  readThread_;
    std::atomic<bool>            running_{false};
    std::atomic<bool>            eof_{false};

    StdinReader(asio::any_io_executor ex);

public:

    static StdinReader& instance(asio::any_io_executor ex);

    /// 异步读取一行; EOF 时返回 nullopt
    asio::awaitable<std::optional<std::string>> readLine();

    bool available() const;

    ~StdinReader();
};
