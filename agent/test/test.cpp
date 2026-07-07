#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/random_access_file.hpp"
#include "asio/read.hpp"
#include "asio/read_at.hpp"
#include "asio/read_until.hpp"
#include "asio/redirect_error.hpp"
#include "asio/registered_buffer.hpp"
#include "asio/stream_file.hpp"
#include "asio/use_awaitable.hpp"
#include "test_codegraph_tools.h"
#include "test_command_tools.h"
#include "test_cpu_gpu_use.h"
#include "test_datetime_tool.h"
#include "test_filesystem_tools.h"
#include "test_rag_search_tools.h"
#include "test_regex.h"
#include "test_screen_capture.h"
#include "test_string_tools.h"
#include "test_string_util.h"
#include "test_text_selection_monitor.h"
#include "test_web_search_tools.h"
#include <fstream>
#include <iostream>
#include <map>

#if XX_IS_WIN_D
#include <Windows.h>
#endif

bool waitForInput = false;
asio::io_context ioCtx;

asio::awaitable<void> test(asio::io_context &ioCtx) { co_return; }

/// 运行可执行 ../script/test_run.sh
int main(int argn, char **argv) {
#if XX_IS_WIN_D
  SetConsoleOutputCP(CP_UTF8);
#endif
#if XX_IS_DEBUG_D && XX_IS_LINUX_D
  agentxx::util::signalError(argv[0]);
#endif
  std::cout << "======= Test Start =======" << std::endl;

  asio::co_spawn(
      ioCtx,
      []() -> asio::awaitable<void> {
        co_await test(ioCtx);

        auto agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
        auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
        agentContext->agentConfig = agentConfig;

        auto run = [](auto testFn, auto... args) -> asio::awaitable<void> {
          try {
            co_await testFn(args...);
          } catch (const std::exception &e) {
            std::cout << "[FAIL] Test suite exception: " << e.what()
                      << std::endl;
          }
        };

        { agentxx::test::testStringUtil(); }
        { agentxx::test::testRegex(); }

        co_await run(agentxx::test::run_string_tools_tests, agentContext);
        co_await run(agentxx::test::run_rag_search_tools_tests, agentContext);
        co_await run(agentxx::test::run_datetime_tool_tests, agentContext);
        co_await run(agentxx::test::run_filesystem_tools_tests, agentContext);
        co_await run(agentxx::test::run_command_tools_tests, agentContext);
        co_await run(agentxx::test::run_web_search_tools_tests, agentContext);
        co_await run(agentxx::test::run_codegraph_tools_tests, agentContext);
        co_await agentxx::test::test_cpu_gpu_use();
      },
      asio::detached);
  ioCtx.run();

  test_screen_capture();

  auto monitor = test_text_selection_monitor();
  if (waitForInput) {
    std::cout << "Press any key to continue..." << std::endl;
    std::cin.get();
  }
  if (nullptr != monitor) {
    monitor->stop();
  }

  std::cout << "======= Test Done =======" << std::endl;
  return 0;
}