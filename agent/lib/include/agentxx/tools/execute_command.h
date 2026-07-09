#pragma once

#include "agentxx/tools/tool.h"
#include "agentxx/util/string_util.h"
#include "agentxx/util/util.h"
#include "asio/dispatch.hpp"
#include "asio/io_context.hpp"
#include "asio/read.hpp"
#include "asio/readable_pipe.hpp"
#include "asio/redirect_error.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include <cstdlib>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <sstream>
#include <string>
#include <vector>

#if AGENTXX_ENABLE_BOOST_PROCESS
#include "boost/process.hpp"
#endif

namespace asio = ::boost::asio;

namespace agentxx {
namespace tools {

class ExecuteLinuxCommandTool : public XXToolBase {
public:
  ExecuteLinuxCommandTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("execute_linux_command", in_agentContext, true, false) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
                        "command",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("command")},
                        },
                    },
                    {
                        "all_output",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("all_output")},
                        },
                    },
                },
            },
            {"required", neograph::json::array({"command"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto command = arguments.value("command", std::string{});
    if (command.empty()) {
      co_return R"({"error":"Arg `command` is empty"})";
    }
    auto all_output = arguments.value("all_output", true);

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
    {
      auto ctx = co_await asio::this_coro::executor;
      asio::readable_pipe outpip{ctx}, errpip{ctx};
      // 2. 创建管道，用于接收子进程的输出
      std::unordered_map<boost::process::environment::key,
                         boost::process::environment::value>
          procEnv;
      for (const auto &kv : boost::process::environment::current()) {
        if (kv.key().string() != "SECRET") {
          procEnv[kv.key()] = kv.value();
        }
      }

      auto proc = boost::process::process{
          ctx,
          boost::process::environment::find_executable("bash"),
          {"-c", command},
          boost::process::process_environment(procEnv),
          boost::process::process_stdio {
            .out = outpip,
            .err = errpip
          }};

      std::string strout, strerr;
      neograph_asio_error_code errCode;
      auto readStdOutFuture = asio::async_read(
          outpip, asio::dynamic_buffer(strout), asio::transfer_all(),
          asio::redirect_error(asio::use_awaitable, errCode));
      auto readStdErrFuture = asio::async_read(
          errpip, asio::dynamic_buffer(strerr), asio::transfer_all(),
          asio::redirect_error(asio::use_awaitable, errCode));
      // assert(!ec || (ec == asio::error::eof));
      co_await proc.async_wait();
      co_await std::move(readStdOutFuture);
      co_await std::move(readStdErrFuture);

      const auto exitCode = proc.exit_code();
      std::ostringstream result;
      result << "## ExitCode: " << exitCode << std::endl;
      if (all_output || 0 != exitCode) {
        // failed
        if (strout.empty() || agentxx::util::autoConvertToUtf8(strout)) {
          result << "## StdOut:\n" << strout << std::endl;
        } else {
          result << "## StdOut convert to utf8 faild, truncated" << std::endl;
        }
        if (strerr.empty() || agentxx::util::autoConvertToUtf8(strerr)) {
          result << "## StdErr:\n" << strerr << std::endl;
        } else {
          result << "## StdErr convert to utf8 faild, truncated" << std::endl;
        }
      }
      co_return result.str();
    }
#endif

#if XX_IS_WIN_D
    auto pipe = std::unique_ptr<FILE, decltype(&_pclose)>{
        _popen(command.c_str(), "r"), _pclose};
#else
    auto pipe = std::unique_ptr<FILE, decltype(&pclose)>{
        popen(command.c_str(), "r"), pclose};
#endif
    if (!pipe) {
      auto ec = std::error_code{errno, std::system_category()};
      co_return fmt::format(R"({{"error":"Exec command failed. Error: {}"}})",
                            ec.message());
    }

    // TODO: 压缩
    // TODO: 太长则暂存到 share_store
    // TODO: 超长时存入文件，不在内存逗留
    std::array<char, 1024> buffer{};
    std::ostringstream result;
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
      result << buffer.data();
    }
    co_return result.str();
  }
};

/// windows cmd
/// 支持 WSL
class ExecuteWindowsCommandTool : public XXToolBase {
public:
  ExecuteWindowsCommandTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("execute_windows_command", in_agentContext, true, false) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
                        "command",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("command_process")},
                        },
                    },
#else
                        "command",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("command_popen")},
                        },
                    },
#endif
                    {
                        "all_output",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("all_output")},
                        },
                    },
                },
            },
            {"required", neograph::json::array({"command"})},
        },
    };
  } // namespace tools

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto command = arguments.value("command", std::string{});
    if (command.empty()) {
      co_return R"({"error":"Arg `command` is empty"})";
    }
    auto all_output = arguments.value("all_output", true);
// TODO: UNC 路径不受支持。默认值设为 Windows 目录
#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
    {
      auto ctx = co_await asio::this_coro::executor;
      asio::readable_pipe outpip{ctx}, errpip{ctx};
      // 2. 创建管道，用于接收子进程的输出
      std::unordered_map<boost::process::environment::key,
                         boost::process::environment::value>
          procEnv;
      for (const auto &kv : boost::process::environment::current()) {
        if (kv.key().string() != "SECRET") {
          procEnv[kv.key()] = kv.value();
        }
      }

      auto proc = boost::process::process{
          ctx,
          boost::process::environment::find_executable("cmd.exe"),
          {"/c", command},
          boost::process::process_environment(procEnv),
          boost::process::process_stdio{.out = outpip, .err = errpip}};

      std::string strout, strerr;
      neograph_asio_error_code errCode;
      auto readStdOutFuture = asio::async_read(
          outpip, asio::dynamic_buffer(strout), asio::transfer_all(),
          asio::redirect_error(asio::use_awaitable, errCode));
      auto readStdErrFuture = asio::async_read(
          errpip, asio::dynamic_buffer(strerr), asio::transfer_all(),
          asio::redirect_error(asio::use_awaitable, errCode));
      // assert(!ec || (ec == asio::error::eof));
      co_await proc.async_wait();
      co_await std::move(readStdOutFuture);
      co_await std::move(readStdErrFuture);

      const auto exitCode = proc.exit_code();
      std::ostringstream result;
      result << "## ExitCode: " << exitCode << std::endl;
      if (all_output || 0 != exitCode) {
        // failed
        if (strout.empty() || agentxx::util::autoConvertToUtf8(strout)) {
          result << "## StdOut:\n" << strout << std::endl;
        } else {
          result << "## StdOut convert to utf8 faild, truncated" << std::endl;
        }
        if (strerr.empty() || agentxx::util::autoConvertToUtf8(strerr)) {
          result << "## StdErr:\n" << strerr << std::endl;
        } else {
          result << "## StdErr convert to utf8 faild, truncated" << std::endl;
        }
      }
      co_return result.str();
    }
#endif

#if XX_IS_WIN_D
    auto pipe = std::unique_ptr<FILE, decltype(&_pclose)>{
        _popen(command.c_str(), "r"), _pclose};
#else
    auto pipe = std::unique_ptr<FILE, decltype(&pclose)>{
        popen(command.c_str(), "r"), pclose};
#endif
    if (!pipe) {
      auto ec = std::error_code{errno, std::system_category()};
      co_return fmt::format(R"({{"error":"Exec command failed. Error: {}"}})",
                            ec.message());
    }

    std::array<char, 1024> buffer{};
    std::ostringstream out{};
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
      out << buffer.data();
    }
    std::string result = out.str();
    if (agentxx::util::autoConvertToUtf8(result)) {
      // 转 utf8
      co_return result;
    }
    co_return result;
  }
}; // namespace agentxx

class ExecutePythonTool : public XXToolBase {
public:
  ExecutePythonTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("execute_python_command", in_agentContext, true, false) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
                        "command",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("command")},
                        },
                    },
                },
            },
            {"required", neograph::json::array({"command"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto command = arguments.value("command", std::string{});
    if (command.empty()) {
      co_return R"({"error":"Arg `command` is empty"})";
    }

    co_return "";
  }
};

class ExecuteJavaScriptTool : public XXToolBase {
public:
  ExecuteJavaScriptTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("execute_javascript_command", in_agentContext, true, false) {
  }

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
                        "command",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("command")},
                        },
                    },
                },
            },
            {"required", neograph::json::array({"command"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto command = arguments.value("command", std::string{});
    if (command.empty()) {
      co_return R"({"error":"Arg `command` is empty"})";
    }

    co_return "";
  }
};

} // namespace tools
} // namespace agentxx