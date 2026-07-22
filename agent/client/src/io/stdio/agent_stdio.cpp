#include "agentxx-client/io/stdio/agent_stdio.h"

#include "agentxx-client/io/stdio/stdin_reader.h"
#include "asio/this_coro.hpp"
#include "fmt/format.h"
#include <cctype>
#include <iostream>
#include <utility>

void AgentStdIO::onToken(const std::string &token, const std::string &kind) {
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

void AgentStdIO::onDisplay(const std::string &level,
                           const std::string &content) {
  std::cout << content << std::flush;
}

asio::awaitable<std::optional<std::string>> AgentStdIO::getInput() {
  auto &stdinReader =
      StdinReader::instance(co_await asio::this_coro::executor);
  co_return co_await stdinReader.readLine();
}

asio::awaitable<bool>
AgentStdIO::promptPermission(const std::string &toolName,
                             const std::string &category,
                             const std::string &target) {
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

void AgentStdIO::onInterrupt(const std::string &node, const std::string &value,
                             const std::string &handleName) {
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

void AgentStdIO::resetTokenState() { isThinking_ = false; }
