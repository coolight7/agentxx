#include "agentxx/agent/config_static.h"
#include "bench_aho_corasick.h"
#include "bench_code_agent.h"
#include "bench_regex.h"
#include "bench_render.h"
#include "bench_resource.h"
#include "bench_resource_util.h"
#include "bench_router.h"
#include "bench_string_util.h"
#include "bench_util.h"
#include "utilxx_base/env.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#ifndef AGENTXX_BENCH_OUTPUT_DIR
#define AGENTXX_BENCH_OUTPUT_DIR ""
#endif

#if XX_IS_DEBUG_D && XX_IS_LINUX_D
#include "utilxx_base/log.h"
#endif

namespace {

struct BenchModule {
    std::string           name;
    std::string           description;
    std::function<void()> run;
    bool isSubResource = false; ///< 是否为 resource 的子模块 (全量时由 resource 代替)
};

} // namespace

int main(int argn, char** argv) {
#if XX_IS_DEBUG_D && XX_IS_LINUX_D
    utilxx_base::signalError(argv[0]);
#endif

    // 开启性能统计采集 (TUI 帧耗时等): 基准程序全程打开;
    // 正常使用 (agentxx_cli / agentxx_test) 该标记默认关闭, 统计相关代码不执行,
    // 避免统计带来的额外开销 (见 agentxx::agent::AgentConfigStatic::enableBenchmark)
    agentxx::agent::AgentConfigStatic::setEnableBenchmark(true);

    // 解析参数
    bool                     failFast = false;
    std::string              baselinePath;
    std::vector<std::string> selectedModules;

    for (int i = 1; i < argn; ++i) {
        std::string arg = argv[i];
        if (arg == "--fail-fast" || arg == "-f") {
            failFast = true;
        } else if (arg == "--baseline" && i + 1 < argn) {
            baselinePath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: agentxx_benchmark [module ...] [options]\n"
                << "Options:\n"
                << "  --list             List all available benchmark modules\n"
                << "  --fail-fast, -f    Abort benchmark immediately on failure/exception\n"
                << "  --baseline <json>  Compare with a previous bench_*.json and print deltas\n"
                << "  --help, -h         Show this help message\n"
                << "\n"
                << "资源基准测试 (resource_*) 采集内容:\n"
                << "  RSS/PSS/私有脏页/匿名/峰值/线程/fd、glibc 堆在用与碎片、\n"
                << "  malloc_trim 可回收量、smaps 模块级分解 (插件/库/堆/匿名)、\n"
                << "  逻辑内存 (消息容器/TUI 状态/工具 schema)、分阶段增量、CPU 用户/内核时间;\n"
                << "  报告同时输出 JSON (机器对比) 与 Markdown (人工阅读)。\n"
                << "环境变量: AGENTXX_BENCH_OUTPUT_DIR (报告目录) / AGENTXX_BENCH_SCALE\n"
                << "          (真实两进程场景的负载缩放系数, 默认 1.0)\n"
                << "\n"
                << "When no module is specified, all benchmarks will be run.\n";
            return 0;
        } else if (arg[0] != '-') {
            selectedModules.push_back(arg);
        }
    }

    auto makeCodeAgentConfig = []() {
        agentxx::bench::CodeAgentBenchConfig config;
        config.openAIBaseUrl
            = utilxx_base::ApplicationEnv::instance().getOr("AGENTXX_BENCH_LLM_BASE_URL", "");
        config.openAIApiKey
            = utilxx_base::ApplicationEnv::instance().getOr("AGENTXX_BENCH_LLM_API_KEY", "EMPTY");
        config.openAIModelName = utilxx_base::ApplicationEnv::instance().getOr(
            "AGENTXX_BENCH_LLM_MODEL_NAME",
            "Agentxx"
        );
        config.systemPrompt = utilxx_base::ApplicationEnv::instance().getOr(
            "AGENTXX_BENCH_LLM_SYSTEM_PROMPT",
            "You are a helpful assistant."
        );
        config.userInput = utilxx_base::ApplicationEnv::instance().getOr(
            "AGENTXX_BENCH_LLM_USER_INPUT",
            "Hello, please respond briefly."
        );
        config.iterations = 5;
        return config;
    };

    // 模块注册表 (仿 test 单点维护)
    std::vector<BenchModule> modules = {
        {"string_util",
         "Benchmark string manipulation and path conversion", agentxx::bench::benchStringUtil},
        {"aho_corasick",
         "Benchmark Aho-Corasick multi-pattern search", agentxx::bench::benchAhoCorasick},
        {"regex", "Benchmark regex matching and pattern substitution", agentxx::bench::benchRegex},
        {"render",
         "Benchmark TUI render path (markdown build/layout/frame cost, lazy list per-frame cost)",
         agentxx::bench::benchRender},
        {"router", "Benchmark URL routing and dispatching", agentxx::bench::benchRouter},
        {"code_agent_init",
         "Benchmark CodeAgent cold initialization", agentxx::bench::benchCodeAgentInit},
        {"code_agent_init_warm",
         "Benchmark CodeAgent warm initialization", agentxx::bench::benchCodeAgentInitWarm},
        {"code_agent_turn",
         "Benchmark CodeAgent single conversation turn", [=]() {
             auto cfg = makeCodeAgentConfig();
             agentxx::bench::benchCodeAgentRunConversationTurnAsync(cfg);
         }},
        {"code_agent_simple",
         "Benchmark CodeAgent simple completion", [=]() {
             auto cfg = makeCodeAgentConfig();
             agentxx::bench::benchCodeAgentSimpleCompletion(cfg);
         }},
        {"code_agent_multi",
         "Benchmark CodeAgent multi-turn conversation", [=]() {
             auto cfg = makeCodeAgentConfig();
             agentxx::bench::benchCodeAgentMultiTurn(cfg);
         }},
        {"code_agent_large_history",
         "Benchmark CodeAgent with large conversation history", [=]() {
             auto cfg = makeCodeAgentConfig();
             agentxx::bench::benchCodeAgentLargeHistory(cfg);
         }},
        {"resource",
         "Benchmark memory and CPU across all 5 modes (startup/100K/200K)", agentxx::bench::benchResourceAll},
        {"resource_cli",
         "Benchmark memory and CPU for in-process CLI", agentxx::bench::benchResourceCli,
         true},
        {"resource_tui",
         "Benchmark memory and CPU for in-process TUI", agentxx::bench::benchResourceTui,
         true},
        {"resource_split_cli",
         "Benchmark memory and CPU for split CLI + Server", agentxx::bench::benchResourceSplitCli,
         true},
        {"resource_split_tui",
         "Benchmark memory and CPU for split TUI + Server", agentxx::bench::benchResourceSplitTui,
         true},
        {"resource_ffi",
         "Benchmark memory and CPU for libagentxx_shared control group", agentxx::bench::benchResourceFfi,
         true},
        {"resource_real_tui",
         "Real running TUI (FTXUI loop) + in-process server, with frame timing and render bytes", agentxx::bench::benchResourceRealTui,
         true},
        {"resource_server_only",
         "Real server process alone: idle drift, driven turns, disconnect reclaim", agentxx::bench::benchResourceServerOnly,
         true},
        {"resource_real_tui_child",
         "Real TUI child process driven via pty + real server child process", agentxx::bench::benchResourceRealTuiChild,
         true},
        {"resource_plugin_attrib",
         "Per-plugin marginal memory cost and unload reclaim", agentxx::bench::benchResourcePluginAttrib,
         true},
    };

    // 处理 --list
    for (int i = 1; i < argn; ++i) {
        if (strcmp(argv[i], "--list") == 0) {
            std::cout << "Available benchmark modules:\n";
            for (const auto& mod : modules) {
                std::cout << fmt::format("  {:<26} {}\n", mod.name, mod.description);
            }
            return 0;
        }
    }

    // 校验指定模块名是否合法
    for (const auto& sel : selectedModules) {
        bool found = false;
        for (const auto& mod : modules) {
            if (mod.name == sel) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "[Error] Unknown benchmark module: '" << sel << "'\n"
                      << "Use 'agentxx_benchmark --list' to see all valid module names.\n";
            return 1;
        }
    }

    bool runAll    = selectedModules.empty();
    auto shouldRun = [&](const BenchModule& mod) {
        if (runAll) {
            // 全量运行时: 运行非子模块 (resource 会覆盖所有 resource_* 细分)
            return !mod.isSubResource;
        }
        for (const auto& sel : selectedModules) {
            if (sel == mod.name) {
                return true;
            }
        }
        return false;
    };

    auto&       reporter  = agentxx::bench::BenchReporter::instance();
    std::string outputDir = AGENTXX_BENCH_OUTPUT_DIR;
    if (auto envDir = utilxx_base::ApplicationEnv::instance().get("AGENTXX_BENCH_OUTPUT_DIR")) {
        outputDir = *envDir;
    }
    if (!outputDir.empty()) {
        reporter.setOutputDir(outputDir);
        if (!agentxx::bench::benchChildMode()) {
            std::cout << "[BenchReporter] output dir: " << outputDir << std::endl;
        }
    }
    reporter.setHostInfo(agentxx::bench::collectHostInfo());
    if (!baselinePath.empty()) {
        reporter.loadBaselineFile(baselinePath);
    }

    if (!agentxx::bench::benchChildMode()) {
        std::cout << "========================================" << std::endl;
        std::cout << "  agentxx Performance Benchmarks" << std::endl;
        std::cout << "========================================" << std::endl;
    } else {
        std::cout << "[child] 场景子进程: "
                  << (selectedModules.empty() ? "(全量)" : selectedModules.front()) << std::endl;
    }

    for (const auto& mod : modules) {
        if (!shouldRun(mod)) {
            continue;
        }
        std::cout << "\n>>> Running module: " << mod.name << " (" << mod.description << ")"
                  << std::endl;
        try {
            mod.run();
        } catch (const std::exception& e) {
            std::cerr << "[Error] Module " << mod.name << " threw exception: " << e.what()
                      << std::endl;
            if (failFast) {
                std::cerr << "======= FAIL-FAST: aborting after " << mod.name
                          << " =======" << std::endl;
                std::_Exit(1);
            }
        } catch (...) {
            std::cerr << "[Error] Module " << mod.name << " threw unknown exception" << std::endl;
            if (failFast) {
                std::cerr << "======= FAIL-FAST: aborting after " << mod.name
                          << " =======" << std::endl;
                std::_Exit(1);
            }
        }
    }

    // 资源对比摘要 (本次各模式 + 与基线的差值/模块级差异)
    reporter.printComparisonSummary();

    reporter.flushToFile();

    if (!agentxx::bench::benchChildMode()) {
        std::cout << "\n========================================" << std::endl;
        std::cout << "  All Benchmarks Complete" << std::endl;
        std::cout << "========================================" << std::endl;
    }

    return 0;
}
