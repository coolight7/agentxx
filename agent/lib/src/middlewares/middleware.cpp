#include "agentxx/middlewares/middleware.h"
#include "agentxx/tools/tool.h"
#include <charconv>

agentxx::middleware::BaseMiddlewareHandleInterface::
    BaseMiddlewareHandleInterface(
        std::string_view in_name,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
    : name(in_name), agentContext(in_agentContext) {}

asio::awaitable<std::optional<neograph::json>>
agentxx::middleware::MiddlewareContext::execInterruptHandle(
    std::string_view name, const agentxx::middleware::InterruptHandleArg &arg) {
  auto handleIt = interruptHandles.find(arg.name);
  if (handleIt != interruptHandles.end()) {
    co_return co_await handleIt->second(arg);
  }
  co_return std::nullopt;
}

void agentxx::middleware::MiddlewareContext::registerInterruptHandles() {
  interruptHandles[interruptHandleName_default] =
      [](const agentxx::middleware::InterruptHandleArg &handleArg)
      -> asio::awaitable<neograph::json> {
    auto result = neograph::json::array();
    std::cout << "\n  ┏━━━━━━ Input ━━━━━━┓" << std::endl;
    bool haveWaitInput = false;
    for (const auto &input : handleArg.inputs) {
      bool inputSuccess = false;
      do {
        std::cout << fmt::format("  ┣━ ## {} : {}", input.label, input.depict)
                  << std::endl;

        if (input.type.empty()) {
          // 不需要输入
          inputSuccess = true;
        } else {
          std::cout << "  ┣━ Type | " << input.type << " | ";
          if ("bool" == input.type) {
            std::cout << "`yes/y` or `no/n`";
          } else if ("int" == input.type) {
          } else if ("double" == input.type) {
          } else if ("string" == input.type) {
          } else if ("enum" == input.type) {
            std::cout << "value of [";
            for (const auto &val : input.enumValues) {
              std::cout << val << ", ";
            }
            std::cout << "]";
          }
          std::cout << std::endl;
          std::cout << "  ┣━ Default Value: " << input.defaultValue
                    << std::endl;
          std::cout << "  ┣━ >>> " << std::flush;

          haveWaitInput = true;
          std::string line;
          std::getline(std::cin, line);
          if (line.empty()) {
            line = input.defaultValue;
          }

          if ("bool" == input.type) {
            std::transform(line.begin(), line.end(), line.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (line == "yes" || line == "y") {
              line = "true";
              inputSuccess = true;
            } else if (line == "no" || line == "n") {
              line = "false";
              inputSuccess = true;
            } else {
              inputSuccess = false;
            }
          } else if ("int" == input.type) {
            int64_t num = 0;
            auto result =
                std::from_chars(line.c_str(), line.c_str() + line.size(), num);
            inputSuccess = (result.ec == std::errc{});
          } else if ("double" == input.type) {
            double num;
            auto result =
                std::from_chars(line.c_str(), line.c_str() + line.size(), num);
            inputSuccess = (result.ec == std::errc{});
          } else if ("string" == input.type) {
            inputSuccess = true;
          } else if ("enum" == input.type) {
            for (const auto &val : input.enumValues) {
              if (val == line) {
                inputSuccess = true;
                break;
              }
            }
          }

          if (inputSuccess) {
            result.push_back(line);
          } else {
            std::cout << "  ┣━ Invalid Input, please try again." << std::endl;
          }
        }
      } while (false == inputSuccess);
    }
    if (false == haveWaitInput) {
      // 没有输入，等待用户确认
      std::cout << "  ┣━ Wait user review, `Enter` to continue." << std::endl;
      std::cout << "  ┣━ >>> " << std::flush;
      std::string temp;
      std::getline(std::cin, temp);
    }
    std::cout << "  ┗━━━━━━ Input ━━━━━━┛\n" << std::endl;
    co_return result;
  };
}