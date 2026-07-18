#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"
#include "test_acp.h"
#include "test_codegraph_tools.h"
#include "test_command_tools.h"
#include "test_cpu_gpu_use.h"
#include "test_crossagent.h"
#include "test_datetime_tool.h"
#include "test_deepagent.h"
#include "test_event_bridge.h"
#include "test_event_stream.h"
#include "test_events.h"
#include "test_filesystem_tools.h"
#include "test_framework.h"
#include "test_http.h"
#include "test_interrupt_bus.h"
#include "test_mcp.h"
#include "test_rag_search_tools.h"
#include "test_regex.h"
#include "test_screen_capture.h"
#include "test_string_tools.h"
#include "test_string_util.h"
#include "test_subagent_bus.h"
#include "test_text_selection_monitor.h"
#include "test_web_search_tools.h"
#include <cstring>
#include <iostream>
#include <map>

#if XX_IS_WIN_D
#include <Windows.h>
#endif

bool waitForInput = false;
asio::io_context ioCtx;

/// 运行可执行 ../script/test_run.sh
int main(int argn, char **argv) {
#if XX_IS_WIN_D
  SetConsoleOutputCP(CP_UTF8);
#endif
#if XX_IS_DEBUG_D && XX_IS_LINUX_D
  agentxx::util::signalError(argv[0]);
#endif

  // 解析参数
  std::vector<std::string> selectedModules;
  for (int i = 1; i < argn; ++i) {
    if (strcmp(argv[i], "--fail-fast") == 0 || strcmp(argv[i], "-f") == 0) {
      agentxx::test::g_failFast = true;
    } else if (argv[i][0] != '-') {
      selectedModules.emplace_back(argv[i]);
    }
  }

  bool runAll = selectedModules.empty();
  auto shouldRun = [&](const std::string &name) {
    if (runAll)
      return true;
    for (const auto &m : selectedModules) {
      if (m == name)
        return true;
    }
    return false;
  };

  agentxx::test::TestResult total;

  std::cout << "======= Test Start =======" << std::endl;

  // ---- 同步测试模块 ----
  auto runSync = [&](const std::string &name, auto fn) {
    if (!shouldRun(name)) {
      TEST_INFO << name << ": skipped" << std::endl;
      return;
    }
    std::cout << "--- " << name << " ---" << std::endl;
    auto r = fn();
    total += r;
    std::cout << "--- " << name << " done: passed=" << r.passed
              << " failed=" << r.failed << " ---" << std::endl;
    if (r.failed > 0 && agentxx::test::g_failFast) {
      std::cout << "======= FAIL-FAST: aborting after " << name
                << " =======" << std::endl;
      std::_Exit(1);
    }
  };

  runSync("string_util", agentxx::test::testStringUtil);
  runSync("regex", agentxx::test::testRegex);
  runSync("events", agentxx::test::test_events);

  // ---- 异步测试模块 ----
  asio::co_spawn(
      ioCtx,
      [&]() -> asio::awaitable<void> {
        auto agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
        auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
        agentContext->agentConfig = agentConfig;

        auto run = [&](const std::string &name,
                       auto testFn) -> asio::awaitable<void> {
          if (!shouldRun(name)) {
            TEST_INFO << name << ": skipped" << std::endl;
            co_return;
          }
          std::cout << "--- " << name << " ---" << std::endl;
          try {
            auto r = co_await testFn();
            total += r;
            std::cout << "--- " << name << " done: passed=" << r.passed
                      << " failed=" << r.failed << " ---" << std::endl;
            if (r.failed > 0 && agentxx::test::g_failFast) {
              std::cout << "======= FAIL-FAST: aborting after " << name
                        << " =======" << std::endl;
              std::_Exit(1);
            }
          } catch (const std::exception &e) {
            TEST_FAIL << name << " suite exception: " << e.what() << std::endl;
            total.failed++;
            if (agentxx::test::g_failFast) {
              std::_Exit(1);
            }
          }
        };

        auto runCtx = [&](const std::string &name, auto testFn,
                          auto ctx) -> asio::awaitable<void> {
          if (!shouldRun(name)) {
            TEST_INFO << name << ": skipped" << std::endl;
            co_return;
          }
          std::cout << "--- " << name << " ---" << std::endl;
          try {
            auto r = co_await testFn(ctx);
            total += r;
            std::cout << "--- " << name << " done: passed=" << r.passed
                      << " failed=" << r.failed << " ---" << std::endl;
            if (r.failed > 0 && agentxx::test::g_failFast) {
              std::cout << "======= FAIL-FAST: aborting after " << name
                        << " =======" << std::endl;
              std::_Exit(1);
            }
          } catch (const std::exception &e) {
            TEST_FAIL << name << " suite exception: " << e.what() << std::endl;
            total.failed++;
            if (agentxx::test::g_failFast) {
              std::_Exit(1);
            }
          }
        };

        co_await run("event_stream", agentxx::test::run_event_stream_tests);
        co_await run("event_bridge", agentxx::test::run_event_bridge_tests);
        co_await run("interrupt_bus", agentxx::test::run_interrupt_bus_tests);
        co_await run("subagent_bus", agentxx::test::run_subagent_bus_tests);
        co_await run("crossagent", agentxx::test::run_crossagent_tests);
        co_await runCtx("string_tools", agentxx::test::run_string_tools_tests,
                        agentContext);
        co_await runCtx("rag_search", agentxx::test::run_rag_search_tools_tests,
                        agentContext);
        co_await runCtx("datetime", agentxx::test::run_datetime_tool_tests,
                        agentContext);
        co_await runCtx("filesystem", agentxx::test::run_filesystem_tools_tests,
                        agentContext);
        co_await runCtx("command", agentxx::test::run_command_tools_tests,
                        agentContext);
        co_await runCtx("web_search", agentxx::test::run_web_search_tools_tests,
                        agentContext);
        co_await runCtx("codegraph", agentxx::test::run_codegraph_tools_tests,
                        agentContext);
        co_await run("cpu_gpu", agentxx::test::run_cpu_gpu_use_tests);
        co_await run("http", agentxx::test::run_http_client_tests);
        co_await run("mcp", agentxx::test::run_mcp_tests);
        co_await run("acp", agentxx::test::run_acp_tests);
        co_await run("deepagent", agentxx::test::run_deepagent_tests);
        ioCtx.stop();
      },
      asio::detached);
  ioCtx.run();

  // ---- 同步平台相关测试 ----
  runSync("screen_capture", test_screen_capture);

  if (shouldRun("text_selection")) {
    auto monitor = test_text_selection_monitor();
    if (waitForInput) {
      std::cout << "Press any key to continue..." << std::endl;
      std::cin.get();
    }
    if (nullptr != monitor) {
      monitor->stop();
    }
  }

  std::cout << "======= Test Done =======" << std::endl;
  std::cout << "Total: passed=" << total.passed << " failed=" << total.failed
            << std::endl;

  if (total.failed > 0) {
    std::_Exit(1);
  }
  std::_Exit(0);
  return 0;
}
