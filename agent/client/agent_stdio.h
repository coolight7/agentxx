#pragma once

#include "agentxx/agent/agent_io.h"
#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/this_coro.hpp"
#include "fmt/format.h"
#include "neograph/api.h"
#include "stdin_reader.h"
#include <iostream>
#include <memory>
#include <optional>
#include <string>

class AgentStdIO : public agentxx::agent::AgentIOBase {
private:
  bool isThinking_ = false;

public:
  AgentStdIO() = default;

  void onToken(const std::string &token, const std::string &kind) override {
    if (kind == "thinking") {
      if (!isThinking_) {
        std::cout << std::endl << "[Thinking] ";
      }
      isThinking_ = true;
    } else {
      if (isThinking_) {
        std::cout << std::endl << "[Content] ";
      }
      isThinking_ = false;
    }
    std::cout << token << std::flush;
  }

  void onDisplay(const std::string &level,
                 const std::string &content) override {
    std::cout << content << std::flush;
  }

  asio::awaitable<std::optional<std::string>> getInput() override {
    auto &stdinReader =
        StdinReader::instance(co_await asio::this_coro::executor);
    co_return co_await stdinReader.readLine();
  }

  asio::awaitable<bool> promptPermission(const std::string &toolName,
                                         const std::string &category,
                                         const std::string &target) override {
    std::cout << fmt::format(R"(
┏━━━━━━ Permission Request ━━━━━━┓
┣━ Tool    : {}
┣━ Category: {}
┣━ Target  : {}
┣━ Allow? [y/N]: )",
                             toolName, category, target)
              << std::flush;

    auto lineOpt = co_await getInput();

    std::cout << "┗━━━━━━ Permission Request ━━━━━━┛\n" << std::endl;

    if (!lineOpt.has_value()) {
      co_return false;
    }
    auto line = std::move(lineOpt.value());
    for (auto &c : line) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    co_return (line == "y" || line == "yes");
  }

  void onInterrupt(const std::string &node, const std::string &value,
                   const std::string &handleName) override {
    std::cout << fmt::format(
                     R"(
┏━━━━━━ Interrupted ━━━━━━┓
┣━ Interrupted at: {}
┣━ Value: {}
{}
┗━━━━━━ Interrupted ━━━━━━┛
)",
                     node, value,
                     (!handleName.empty())
                         ? fmt::format("┣━ Interrupt Handle: {}", handleName)
                         : "┣━ Unknown InterruptHandleArg")
              << std::endl;
  }

  void resetTokenState() { isThinking_ = false; }
};
