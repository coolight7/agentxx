#pragma once

#include "asio/experimental/concurrent_channel.hpp"
#include "asio/use_awaitable.hpp"
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace agentxx {
namespace agent {

/// 异步 stdin 读取器
/// - 在独立线程上阻塞读取 std::cin, 通过 channel 提供行给 io_context 协程
/// - 避免在单线程 io_context 上直接 std::getline 阻塞整个事件循环
/// - 进程内单例, 多个 handler 共享同一读取线程与 channel
class StdinReader {
public:
  using LineChannel = asio::experimental::concurrent_channel<void(
      neograph_asio_error_code, std::string)>;

private:
  std::shared_ptr<LineChannel> channel_;
  std::thread readThread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> eof_{false};

  StdinReader(asio::any_io_executor ex)
      : channel_(std::make_shared<LineChannel>(ex, 64)) {
    running_ = true;
    readThread_ = std::thread([this]() {
      std::string line;
      while (running_) {
        if (std::getline(std::cin, line)) {
          channel_->async_send(neograph_asio_error_code{}, std::move(line),
                               [](neograph_asio_error_code) {});
        } else {
          // EOF 或错误: 标记 eof, 并发一个 cancel 消息唤醒等待中的 readLine
          eof_ = true;
          channel_->async_send(
              asio::experimental::channel_errc::channel_cancelled,
              std::string{}, [](neograph_asio_error_code) {});
          break;
        }
      }
    });
  }

public:
  static StdinReader &instance(asio::any_io_executor ex) {
    static std::shared_ptr<StdinReader> inst;
    static std::once_flag flag;
    std::call_once(flag,
                   [&]() { inst = std::shared_ptr<StdinReader>(new StdinReader(ex)); });
    return *inst;
  }

  /// 异步读取一行; EOF 时返回 nullopt
  asio::awaitable<std::optional<std::string>> readLine() {
    // 已 EOF 且 channel 空, 直接返回 (避免永久阻塞)
    if (eof_ && !channel_->ready()) {
      co_return std::nullopt;
    }
    auto [ec, line] = co_await channel_->async_receive(
        asio::as_tuple(asio::use_awaitable));
    if (ec) {
      co_return std::nullopt;
    }
    co_return std::optional<std::string>{std::move(line)};
  }

  ~StdinReader() {
    running_ = false;
    // std::cin 的 getline 在 EOF 后会返回 false, 线程自然退出
    if (readThread_.joinable()) {
      readThread_.detach(); // 不阻塞析构 (进程退出时线程被回收)
    }
  }
};

} // namespace agent
} // namespace agentxx
