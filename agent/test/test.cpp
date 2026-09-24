#include "agentxx-test/core/test_a2a.h"
#include "agentxx-test/core/test_acp.h"
#include "agentxx-test/core/test_agent.h"
#include "agentxx-test/core/test_agent_host.h"
#include "agentxx-test/core/test_aho_corasick.h"
#include "agentxx-test/core/test_anthropic_provider.h"
#include "agentxx-test/core/test_cancel.h"
#include "agentxx-test/core/test_checkpoint_store.h"
#include "agentxx-test/core/test_command_tools.h"
#include "agentxx-test/core/test_concurrency.h"
#include "agentxx-test/core/test_datetime_tool.h"
#include "agentxx-test/core/test_diff_util.h"
#include "agentxx-test/core/test_event_bridge.h"
#include "agentxx-test/core/test_event_stream.h"
#include "agentxx-test/core/test_events.h"
#include "agentxx-test/core/test_filesystem_tools.h"
#include "agentxx-test/core/test_http.h"
#include "agentxx-test/core/test_interrupt_bus.h"
#include "agentxx-test/core/test_interrupt_ui.h"
#include "agentxx-test/core/test_json.h"
#include "agentxx-test/core/test_json_reflection.h"
#include "agentxx-test/core/test_json_view.h"
#include "agentxx-test/core/test_math_tools.h"
#include "agentxx-test/core/test_mcp.h"
#include "agentxx-test/core/test_memgrowth.h"
#include "agentxx-test/core/test_message_supplement.h"
#include "agentxx-test/core/test_misc_fixes.h"
#include "agentxx-test/core/test_network_timeout.h"
#include "agentxx-test/core/test_openai_provider.h"
#include "agentxx-test/core/test_rag_search_tools.h"
#include "agentxx-test/core/test_regex.h"
#include "agentxx-test/core/test_remote_agent.h"
#include "agentxx-test/core/test_session_persistence.h"
#include "agentxx-test/core/test_settings_db.h"
#include "agentxx-test/core/test_share_store.h"
#include "agentxx-test/core/test_string_tools.h"
#include "agentxx-test/core/test_string_util.h"
#include "agentxx-test/core/test_subagent_bus.h"
#include "agentxx-test/core/test_subagent_tool.h"
#include "agentxx-test/core/test_summarization.h"
#include "agentxx-test/core/test_training.h"
#include "agentxx-test/core/test_ui_items.h"
#include "agentxx-test/core/test_util_misc.h"
#include "agentxx-test/core/test_worktree.h"
#include "agentxx-test/plugin/test_client_plugins.h"
#include "agentxx-test/plugin/test_codegraph_tools.h"
#include "agentxx-test/plugin/test_cpu_gpu_use.h"
#include "agentxx-test/plugin/test_plugin_bridge.h"
#include "agentxx-test/plugin/test_plugin_multi_instance.h"
#include "agentxx-test/plugin/test_plugin_resources.h"
#include "agentxx-test/plugin/test_plugin_runtime.h"
#include "agentxx-test/plugin/test_plugin_sdk.h"
#include "agentxx-test/plugin/test_plugins.h"
#include "agentxx-test/plugin/test_screen_capture.h"
#include "agentxx-test/plugin/test_text_selection_monitor.h"
#include "agentxx-test/test_ffi_c_api.h"
#include "agentxx-test/test_toolcall_args.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"
#include "utilxx_base/log.h"
#ifdef AGENTXX_BUILD_CLIENT
#include "agentxx-test/client/test_config_loader.h"
#include "agentxx-test/client/test_ftxui_text.h"
#include "agentxx-test/client/test_markdown_block.h"
#include "agentxx-test/client/test_markdown_flow.h"
#include "agentxx-test/client/test_mermaid_state.h"
#include "agentxx-test/client/test_thread_id.h"
#include "agentxx-test/client/test_tui_context_overlay.h"
#include "agentxx-test/client/test_tui_form.h"
#include "agentxx-test/client/test_tui_input.h"
#include "agentxx-test/client/test_tui_interrupt.h"
#include "agentxx-test/client/test_tui_scroll.h"
#include "agentxx-test/client/test_tui_settings.h"
#include "agentxx-test/client/test_tui_sidebar.h"
#include "agentxx-test/client/test_tui_stream.h"
#include "agentxx-test/client/test_tui_surface.h"
#include "agentxx-test/client/test_tui_theme.h"
#include "agentxx-test/client/test_tui_tool_header.h"
#include "agentxx-test/client/test_tui_ui_items.h"
#include "agentxx-test/client/test_tui_widget.h"
#include "agentxx-test/client/test_update_check.h"
#endif
#include "agentxx-test/core/test_util_misc.h"
#include "agentxx-test/core/test_web_search_tools.h"
#include "agentxx-test/core/test_websocket.h"
#include <cstring>
#include <iostream>
#include <map>

namespace {

/// 测试期日志 sink: 仅把 Warn/Error 输出到 stderr
/// - 库内错误 (如插件 LoadLibrary 失败 / entry 返回非零) 经 XX_LOGE 上报,
///   测试进程默认无 sink 时会被静默丢弃, 失败原因不可见; 此处透出便于诊断
class TestWarnErrorLogSink : public utilxx_base::ThreadedLogSink {
public:

    ~TestWarnErrorLogSink() override {
        // 在虚表仍为本类时停止日志线程 (避免延迟到基类析构触发 purecall)
        shutdownThread();
    }

    void onLog(const utilxx_base::LogEntry& entry) override {
        if (entry.level == utilxx_base::LogLevel::Warn
            || entry.level == utilxx_base::LogLevel::Error) {
            std::cerr << "[lib:" << (entry.level == utilxx_base::LogLevel::Error ? "E" : "W")
                      << "] " << entry.message << std::endl;
        }
    }
};

} // namespace

asio::io_context ioCtx;

// ASan 默认选项 (仅链接了 AddressSanitizer 时生效)。
//
// macOS/iOS 的 ASan 运行库默认开启 ODR 检查与全局变量红区。本项目架构下每个
// 插件动态库都会**静态**链接一份 cxx_utilxx(_base) (见 AGENTS.md 插件设计),
// 于是 simdjson 等库的全局符号会在 libagentxx 与各插件 dylib 中重复出现;
// Apple 平台 ASan 对跨镜像重复全局的处理会误报 (odr-violation 中止 /
// 全局红区误判为全局越界), 导致插件相关测试无法运行。
// 这里在 Apple 平台关闭 ODR 检查与全局变量登记 (堆/栈/越界检查不受影响),
// 其它平台保持 ASan 默认行为。
extern "C" const char* __asan_default_options() {
#if XX_IS_MACOS_D || XX_IS_IOS_D
    return "detect_odr_violation=0:report_globals=0";
#else
    return nullptr;
#endif
}

int main(int argn, char** argv) {
#if XX_IS_WIN_D
    SetConsoleOutputCP(CP_UTF8);
#endif
#if XX_IS_DEBUG_D && (XX_IS_LINUX_D || XX_IS_WIN_D)
    utilxx_base::signalError(argv[0]);
#endif

    // 注册库日志 sink (Warn/Error → stderr): 插件加载失败等原因不再被静默丢弃
    auto testLogSink = std::make_shared<TestWarnErrorLogSink>();
    utilxx_base::LogDispatcher::instance().addSink(testLogSink);

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
    bool                      failFastTriggered = false;

    struct FailFastException : public std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    std::cout << "======= Test Start =======" << std::endl;

    // ---- 同步测试模块 ----
    auto runSync = [&](const std::string& name, auto fn) {
        if (failFastTriggered) {
            return;
        }
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
            failFastTriggered = true;
            throw FailFastException("fail-fast aborted");
        }
    };

    try {
        runSync("string_util", agentxx::test::testStringUtil);
        runSync("regex", agentxx::test::testRegex);
        runSync("json", agentxx::test::testJson);
        runSync("json_view", agentxx::test::testJsonView);
        runSync("json_reflection", agentxx::test::testJsonReflection);
        runSync("diff_util", agentxx::test::testDiffUtil);
        runSync("events", agentxx::test::test_events);
        runSync("concurrency", agentxx::test::testConcurrency);
        runSync("misc_fixes", agentxx::test::testMiscFixes);
        runSync("aho_corasick", agentxx::test::testAhoCorasick);
        runSync("util_misc", agentxx::test::testUtilMisc);
        runSync("training", agentxx::test::testTraining);
        runSync("settings_db", agentxx::test::testSettingsDb);
        runSync("toolcall_args", agentxx::test::testToolcallArgs);
        runSync("interrupt_ui", agentxx::test::testInterruptUi);
        runSync("ui_items", agentxx::test::testUiItems);
        runSync("ffi_c_api", agentxx::test::testFfiCApi);
        runSync("plugin_runtime", agentxx::test::testPluginRuntime);
        runSync("plugin_sdk", agentxx::test::testPluginSdk);
        runSync("plugin_bridge", agentxx::test::testPluginBridge);
#ifdef AGENTXX_BUILD_CLIENT
        runSync("config_loader", agentxx::test::testConfigLoader);
        runSync("tui_settings", agentxx::test::testTuiSettings);
        runSync("update_check", agentxx::test::testUpdateCheck);
        runSync("tui_input", agentxx::test::testTuiInput);
        runSync("tui_interrupt", agentxx::test::testTuiInterrupt);
        runSync("tui_scroll", agentxx::test::testTuiScroll);
        runSync("tui_sidebar", agentxx::test::testTuiSidebar);
        runSync("tui_context_overlay", agentxx::test::testTuiContextOverlay);
        runSync("tui_form", agentxx::test::testTuiForm);
        runSync("tui_stream", agentxx::test::testTuiStream);
        runSync("tui_surface", agentxx::test::testTuiSurface);
        runSync("tui_theme", agentxx::test::testTuiTheme);
        runSync("tui_tool_header", agentxx::test::testTuiToolHeader);
        runSync("tui_ui_items", agentxx::test::testTuiUiItems);
        runSync("tui_widget", agentxx::test::testTuiWidget);
        runSync("sessionId", agentxx::test::testSessionId);
        runSync("mermaid_state", agentxx::test::testMermaidState);
        runSync("markdown_block", agentxx::test::testMarkdownBlock);
        runSync("ftxui_text", agentxx::test::testFtxuiText);
        runSync("markdown_flow", agentxx::test::testMarkdownFlow);
#endif
    } catch (const FailFastException&) {
        // fail-fast: 同步模块失败, 已标记 failFastTriggered 并跳过后续测试
    }

    // ---- 异步测试模块 ----
    if (!failFastTriggered) {
        asio::co_spawn(
            ioCtx,
            [&]() -> asio::awaitable<void> {
                auto agentConfig          = std::make_shared<agentxx::agent::AgentConfig>();
                auto agentContext         = std::make_shared<agentxx::agent::AgentContext>();
                agentContext->agentConfig = agentConfig;

                auto run = [&](const std::string& name, auto testFn) -> asio::awaitable<void> {
                    if (failFastTriggered) {
                        co_return;
                    }
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
                            failFastTriggered = true;
                            ioCtx.stop();
                            co_return;
                        }
                    } catch (const std::exception& e) {
                        TEST_FAIL << name << " suite exception: " << e.what() << std::endl;
                        total.failed++;
                        if (agentxx::test::g_failFast) {
                            std::cout << "======= FAIL-FAST: aborting after " << name
                                      << " =======" << std::endl;
                            failFastTriggered = true;
                            ioCtx.stop();
                            co_return;
                        }
                    }
                };

                auto runCtx
                    = [&](const std::string& name, auto testFn, auto ctx) -> asio::awaitable<void> {
                    if (failFastTriggered) {
                        co_return;
                    }
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
                            failFastTriggered = true;
                            ioCtx.stop();
                            co_return;
                        }
                    } catch (const std::exception& e) {
                        TEST_FAIL << name << " suite exception: " << e.what() << std::endl;
                        total.failed++;
                        if (agentxx::test::g_failFast) {
                            std::cout << "======= FAIL-FAST: aborting after " << name
                                      << " =======" << std::endl;
                            failFastTriggered = true;
                            ioCtx.stop();
                            co_return;
                        }
                    }
                };

                co_await run("event_stream", agentxx::test::run_event_stream_tests);
                co_await run("event_bridge", agentxx::test::run_event_bridge_tests);
                co_await run("interrupt_bus", agentxx::test::run_interrupt_bus_tests);
                co_await run("subagent_bus", agentxx::test::run_subagent_bus_tests);
                co_await run("subagent_tool", agentxx::test::run_subagent_tool_tests);
                co_await run("agent_host", agentxx::test::run_agent_host_tests);
                co_await runCtx(
                    "string_tools",
                    agentxx::test::run_string_tools_tests,
                    agentContext
                );
                co_await runCtx("math_tools", agentxx::test::run_math_tools_tests, agentContext);
                co_await run("share_store", agentxx::test::run_share_store_tests);
                co_await run("session_persistence", agentxx::test::run_session_persistence_tests);
                co_await runCtx(
                    "rag_search",
                    agentxx::test::run_rag_search_tools_tests,
                    agentContext
                );
                co_await runCtx("datetime", agentxx::test::run_datetime_tool_tests, agentContext);
                co_await runCtx(
                    "filesystem",
                    agentxx::test::run_filesystem_tools_tests,
                    agentContext
                );
                co_await runCtx("command", agentxx::test::run_command_tools_tests, agentContext);
                co_await run("worktree", agentxx::test::run_worktree_tests);
                co_await runCtx(
                    "web_search",
                    agentxx::test::run_web_search_tools_tests,
                    agentContext
                );
                co_await runCtx(
                    "codegraph",
                    agentxx::test::run_codegraph_tools_tests,
                    agentContext
                );
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
                co_await run(
                    "plugin_multi_instance",
                    agentxx::test::run_plugin_multi_instance_tests
                );
                co_await run("client_plugins", agentxx::test::run_client_plugin_tests);
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
    }

    if (failFastTriggered) {
        std::cout << "======= FAIL-FAST: Tests aborted =======" << std::endl;
    } else {
        std::cout << "======= Test Done =======" << std::endl;
    }
    std::cout << "Total: passed=" << total.passed << " failed=" << total.failed << std::endl;
    std::cout.flush();

    // 正常退出: 从 main 返回以刷新 stdout 并运行析构 (避免 _Exit 丢失末尾输出/掩盖资源泄漏)
    return total.failed > 0 ? 1 : 0;
}
