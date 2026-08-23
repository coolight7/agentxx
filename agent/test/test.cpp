#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"
#include "test_a2a.h"
#include "test_acp.h"
#include "test_agent.h"
#include "test_agent_host.h"
#include "test_aho_corasick.h"
#include "test_anthropic_provider.h"
#include "test_cancel.h"
#include "test_checkpoint_store.h"
#include "test_client_plugins.h"
#include "test_codegraph_tools.h"
#include "test_command_tools.h"
#include "test_concurrency.h"
#include "test_cpu_gpu_use.h"
#include "test_datetime_tool.h"
#include "test_diff_util.h"
#include "test_event_bridge.h"
#include "test_event_stream.h"
#include "test_events.h"
#include "test_ffi_c_api.h"
#include "test_filesystem_tools.h"
#include "test_http.h"
#include "test_interrupt_bus.h"
#include "test_mcp.h"
#include "test_memgrowth.h"
#include "test_message_supplement.h"
#include "test_misc_fixes.h"
#include "test_network_timeout.h"
#include "test_openai_provider.h"
#include "test_plugins.h"
#include "test_plugin_resources.h"
#include "test_rag_search_tools.h"
#include "test_regex.h"
#include "test_remote_agent.h"
#include "test_screen_capture.h"
#include "test_session_persistence.h"
#include "test_settings_db.h"
#include "test_share_store.h"
#include "test_string_tools.h"
#include "test_string_util.h"
#include "test_subagent_bus.h"
#include "test_summarization.h"
#include "test_text_selection_monitor.h"
#include "test_toolcall_args.h"
#include "test_training.h"
#ifdef AGENTXX_BUILD_CLIENT
#include "test_config_loader.h"
#include "test_mermaid_state.h"
#include "test_thread_id.h"
#include "test_tui_input.h"
#include "test_tui_interrupt.h"
#include "test_tui_scroll.h"
#include "test_tui_settings.h"
#include "test_tui_stream.h"
#include "test_tui_tool_header.h"
#endif
#include "test_util_misc.h"
#include "test_web_search_tools.h"
#include "test_websocket.h"
#include <cstring>
#include <iostream>
#include <map>

namespace {

/// 测试期日志 sink: 仅把 Warn/Error 输出到 stderr
/// - 库内错误 (如插件 LoadLibrary 失败 / entry 返回非零) 经 XX_LOGE 上报,
///   测试进程默认无 sink 时会被静默丢弃, 失败原因不可见; 此处透出便于诊断
class TestWarnErrorLogSink : public agentxx::util::ThreadedLogSink {
public:

    ~TestWarnErrorLogSink() override {
        // 在虚表仍为本类时停止日志线程 (避免延迟到基类析构触发 purecall)
        shutdownThread();
    }

    void onLog(const agentxx::util::LogEntry& entry) override {
        if (entry.level == agentxx::util::LogLevel::Warn
            || entry.level == agentxx::util::LogLevel::Error) {
            std::cerr << "[lib:" << (entry.level == agentxx::util::LogLevel::Error ? "E" : "W")
                      << "] " << entry.message << std::endl;
        }
    }
};

} // namespace

asio::io_context ioCtx;

/// 运行可执行 ../script/test_run.sh
int main(int argn, char** argv) {
#if XX_IS_WIN_D
    SetConsoleOutputCP(CP_UTF8);
#endif
#if XX_IS_DEBUG_D && (XX_IS_LINUX_D || XX_IS_WIN_D)
    agentxx::util::signalError(argv[0]);
#endif

    // 注册库日志 sink (Warn/Error → stderr): 插件加载失败等原因不再被静默丢弃
    auto testLogSink = std::make_shared<TestWarnErrorLogSink>();
    agentxx::util::LogDispatcher::instance().addSink(testLogSink);

    // 解析参数
    std::vector<std::string> selectedModules;
    for (int i = 1; i < argn; ++i) {
        if (strcmp(argv[i], "--fail-fast") == 0 || strcmp(argv[i], "-f") == 0) {
            agentxx::test::g_failFast = true;
        } else if (argv[i][0] != '-') {
            selectedModules.emplace_back(argv[i]);
        }
    }

    bool runAll    = selectedModules.empty();
    auto shouldRun = [&](const std::string& name) {
        if (runAll) {
            return true;
        }
        for (const auto& m : selectedModules) {
            if (m == name) {
                return true;
            }
        }
        return false;
    };

    agentxx::test::TestResult total;

    std::cout << "======= Test Start =======" << std::endl;

    // ---- 同步测试模块 ----
    auto runSync = [&](const std::string& name, auto fn) {
        if (!shouldRun(name)) {
            TEST_INFO << name << ": skipped" << std::endl;
            return;
        }
        std::cout << "--- " << name << " ---" << std::endl;
        auto r  = fn();
        total  += r;
        std::cout << "--- " << name << " done: passed=" << r.passed << " failed=" << r.failed
                  << " ---" << std::endl;
        if (r.failed > 0 && agentxx::test::g_failFast) {
            std::cout << "======= FAIL-FAST: aborting after " << name << " =======" << std::endl;
            std::_Exit(1);
        }
    };

    runSync("string_util", agentxx::test::testStringUtil);
    runSync("regex", agentxx::test::testRegex);
    runSync("diff_util", agentxx::test::testDiffUtil);
    runSync("events", agentxx::test::test_events);
    runSync("concurrency", agentxx::test::testConcurrency);
    runSync("misc_fixes", agentxx::test::testMiscFixes);
    runSync("aho_corasick", agentxx::test::testAhoCorasick);
    runSync("util_misc", agentxx::test::testUtilMisc);
    runSync("training", agentxx::test::testTraining);
    runSync("settings_db", agentxx::test::testSettingsDb);
    runSync("toolcall_args", agentxx::test::testToolcallArgs);
    runSync("ffi_c_api", agentxx::test::testFfiCApi);
#ifdef AGENTXX_BUILD_CLIENT
    runSync("config_loader", agentxx::test::testConfigLoader);
    runSync("tui_settings", agentxx::test::testTuiSettings);
    runSync("tui_input", agentxx::test::testTuiInput);
    runSync("tui_interrupt", agentxx::test::testTuiInterrupt);
    runSync("tui_scroll", agentxx::test::testTuiScroll);
    runSync("tui_stream", agentxx::test::testTuiStream);
    runSync("tui_tool_header", agentxx::test::testTuiToolHeader);
    runSync("sessionId", agentxx::test::testSessionId);
    runSync("mermaid_state", agentxx::test::testMermaidState);
#endif

    // ---- 异步测试模块 ----
    asio::co_spawn(
        ioCtx,
        [&]() -> asio::awaitable<void> {
            auto agentConfig          = std::make_shared<agentxx::agent::AgentConfig>();
            auto agentContext         = std::make_shared<agentxx::agent::AgentContext>();
            agentContext->agentConfig = agentConfig;

            auto run = [&](const std::string& name, auto testFn) -> asio::awaitable<void> {
                if (!shouldRun(name)) {
                    TEST_INFO << name << ": skipped" << std::endl;
                    co_return;
                }
                std::cout << "--- " << name << " ---" << std::endl;
                try {
                    auto r  = co_await testFn();
                    total  += r;
                    std::cout << "--- " << name << " done: passed=" << r.passed
                              << " failed=" << r.failed << " ---" << std::endl;
                    if (r.failed > 0 && agentxx::test::g_failFast) {
                        std::cout << "======= FAIL-FAST: aborting after " << name
                                  << " =======" << std::endl;
                        std::_Exit(1);
                    }
                } catch (const std::exception& e) {
                    TEST_FAIL << name << " suite exception: " << e.what() << std::endl;
                    total.failed++;
                    if (agentxx::test::g_failFast) {
                        std::_Exit(1);
                    }
                }
            };

            auto runCtx
                = [&](const std::string& name, auto testFn, auto ctx) -> asio::awaitable<void> {
                if (!shouldRun(name)) {
                    TEST_INFO << name << ": skipped" << std::endl;
                    co_return;
                }
                std::cout << "--- " << name << " ---" << std::endl;
                try {
                    auto r  = co_await testFn(ctx);
                    total  += r;
                    std::cout << "--- " << name << " done: passed=" << r.passed
                              << " failed=" << r.failed << " ---" << std::endl;
                    if (r.failed > 0 && agentxx::test::g_failFast) {
                        std::cout << "======= FAIL-FAST: aborting after " << name
                                  << " =======" << std::endl;
                        std::_Exit(1);
                    }
                } catch (const std::exception& e) {
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
            co_await run("agent_host", agentxx::test::run_agent_host_tests);
            co_await runCtx("string_tools", agentxx::test::run_string_tools_tests, agentContext);
            co_await run("share_store", agentxx::test::run_share_store_tests);
            co_await run("session_persistence", agentxx::test::run_session_persistence_tests);
            co_await runCtx("rag_search", agentxx::test::run_rag_search_tools_tests, agentContext);
            co_await runCtx("datetime", agentxx::test::run_datetime_tool_tests, agentContext);
            co_await runCtx("filesystem", agentxx::test::run_filesystem_tools_tests, agentContext);
            co_await runCtx("command", agentxx::test::run_command_tools_tests, agentContext);
            co_await runCtx("web_search", agentxx::test::run_web_search_tools_tests, agentContext);
            co_await runCtx("codegraph", agentxx::test::run_codegraph_tools_tests, agentContext);
            co_await runCtx(
                "screen_capture",
                agentxx::test::run_screen_capture_tests,
                agentContext
            );
            co_await runCtx("cpu_gpu", agentxx::test::run_cpu_gpu_use_tests, agentContext);
            co_await runCtx(
                "text_selection",
                agentxx::test::run_text_selection_monitor_tests,
                agentContext
            );
            co_await run("http", agentxx::test::run_http_client_tests);
            co_await run("network_timeout", agentxx::test::run_network_timeout_tests);
            co_await run("websocket", agentxx::test::run_websocket_tests);
            co_await run("remote_agent", agentxx::test::run_remote_agent_tests);
            co_await run("mcp", agentxx::test::run_mcp_tests);
            co_await run("acp", agentxx::test::run_acp_tests);
            co_await run("a2a", agentxx::test::run_a2a_tests);
            co_await run("openai_provider", agentxx::test::run_openai_provider_tests);
            co_await run("anthropic_provider", agentxx::test::run_anthropic_provider_tests);
            co_await run("plugins", agentxx::test::run_plugin_tests);
            co_await run("plugin_resources", agentxx::test::run_plugin_resource_tests);
#ifdef AGENTXX_ENABLE_PLUGIN_BUILTIN
            // 内置合并编译模式不产出 client 侧插件动态库 (client 插件仍走独立
            // 构建, 见 plugins.md 11.7.5), client_plugins 测试跳过
            TEST_INFO
                << "client_plugins: skipped (AGENTXX_ENABLE_PLUGIN_BUILTIN, no client plugin dlls)"
                << std::endl;
#else
            co_await run("client_plugins", agentxx::test::run_client_plugin_tests);
#endif
            co_await run("cancel", agentxx::test::run_cancel_tests);
            co_await run("message_supplement", agentxx::test::run_message_supplement_tests);
            co_await run("summarization", agentxx::test::run_summarization_tests);
            co_await run("checkpoint_store", agentxx::test::run_checkpoint_store_tests);
            co_await run("agent", agentxx::test::run_agent_tests);
            co_await run("memgrowth", agentxx::test::run_memgrowth_tests);

            ioCtx.stop();
        },
        asio::detached
    );
    ioCtx.run();

    std::cout << "======= Test Done =======" << std::endl;
    std::cout << "Total: passed=" << total.passed << " failed=" << total.failed << std::endl;

    // 正常退出: 从 main 返回以刷新 stdout 并运行析构 (避免 _Exit 丢失末尾输出/掩盖资源泄漏)
    return total.failed > 0 ? 1 : 0;
}
