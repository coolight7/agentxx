#include "agentxx-client/io/stdio/stdin_reader.h"

#include "asio/as_tuple.hpp"
#include "asio/use_awaitable.hpp"
#include <iostream>
#include <mutex>
#include <utility>

StdinReader::StdinReader(asio::any_io_executor ex)
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

StdinReader &StdinReader::instance(asio::any_io_executor ex) {
  static std::shared_ptr<StdinReader> inst;
  static std::once_flag flag;
  std::call_once(flag, [&]() {
    inst = std::shared_ptr<StdinReader>(new StdinReader{ex});
  });
  return *inst;
}

asio::awaitable<std::optional<std::string>> StdinReader::readLine() {
  // 已 EOF 且 channel 空, 直接返回 (避免永久阻塞)
  if (eof_ && !channel_->ready()) {
    co_return std::nullopt;
  }
  auto [ec, line] =
      co_await channel_->async_receive(asio::as_tuple(asio::use_awaitable));
  if (ec) {
    co_return std::nullopt;
  }
  co_return std::optional<std::string>(std::move(line));
}

bool StdinReader::available() const { return std::cin.good(); }

StdinReader::~StdinReader() {
  running_ = false;
  // std::cin 的 getline 在 EOF 后会返回 false, 线程自然退出
  if (readThread_.joinable()) {
    readThread_.detach(); // 不阻塞析构 (进程退出时线程被回收)
  }
}
