#include "agentxx/util/env.h"
#include "agentxx/util/log.h"
#include "bench_aho_corasick.h"
#include "bench_code_agent.h"
#include "bench_regex.h"
#include "bench_resource.h"
#include "bench_router.h"
#include "bench_string_util.h"
#include "bench_util.h"

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
#include "agentxx/util/log.h"
#endif

namespace {

struct BenchModule {
    std::string           name;
    std::string           description;
    std::function<void()> run;
    bool                  isSubResource = false; ///< 是否为 resource 的子模块 (全量时由 resource 代替)
};

} // namespace

int main(int argn, char** argv) {
#if XX_IS_DEBUG_D && XX_IS_LINUX_D
    agentxx::util::signalError(argv[0]);
#endif

    // 解析参数
    bool                     failFast = false;
    std::vector<std::string> selectedModules;

    for (int i = 1; i < argn; ++i) {
        std::string arg = argv[i];
        if (arg == "--fail-fast" || arg == "-f") {
            failFast = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: agentxx_benchmark [module ...] [options]\n"
                      << "Options:\n"
                      << "  --list             List all available benchmark modules\n"
                      << "  --fail-fast, -f    Abort benchmark immediately on failure/exception\n"
                      << "  --help, -h         Show this help message\n"
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
            = agentxx::util::ApplicationEnv::instance().getOr("AGENTXX_BENCH_LLM_BASE_URL", "");
        config.openAIApiKey
            = agentxx::util::ApplicationEnv::instance().getOr("AGENTXX_BENCH_LLM_API_KEY", "EMPTY");
        config.openAIModelName = agentxx::util::ApplicationEnv::instance().getOr(
            "AGENTXX_BENCH_LLM_MODEL_NAME",
            "Agentxx"
        );
        config.systemPrompt = agentxx::util::ApplicationEnv::instance().getOr(
            "AGENTXX_BENCH_LLM_SYSTEM_PROMPT",
            "You are a helpful assistant."
        );
        config.userInput = agentxx::util::ApplicationEnv::instance().getOr(
            "AGENTXX_BENCH_LLM_USER_INPUT",
            "Hello, please respond briefly."
        );
        config.iterations = 5;
        return config;
    };

    // 模块注册表 (仿 test 单点维护)
    std::vector<BenchModule> modules = {
        {"string_util", "Benchmark string manipulation and path conversion", agentxx::bench::benchStringUtil},
        {"aho_corasick", "Benchmark Aho-Corasick multi-pattern search", agentxx::bench::benchAhoCorasick},
        {"regex", "Benchmark regex matching and pattern substitution", agentxx::bench::benchRegex},
        {"router", "Benchmark URL routing and dispatching", agentxx::bench::benchRouter},
        {"code_agent_init", "Benchmark CodeAgent cold initialization", agentxx::bench::benchCodeAgentInit},
        {"code_agent_init_warm", "Benchmark CodeAgent warm initialization", agentxx::bench::benchCodeAgentInitWarm},
        {"code_agent_turn", "Benchmark CodeAgent single conversation turn", [=]() {
            auto cfg = makeCodeAgentConfig();
            agentxx::bench::benchCodeAgentRunConversationTurnAsync(cfg);
        }},
        {"code_agent_simple", "Benchmark CodeAgent simple completion", [=]() {
            auto cfg = makeCodeAgentConfig();
            agentxx::bench::benchCodeAgentSimpleCompletion(cfg);
        }},
        {"code_agent_multi", "Benchmark CodeAgent multi-turn conversation", [=]() {
            auto cfg = makeCodeAgentConfig();
            agentxx::bench::benchCodeAgentMultiTurn(cfg);
        }},
        {"code_agent_large_history", "Benchmark CodeAgent with large conversation history", [=]() {
            auto cfg = makeCodeAgentConfig();
            agentxx::bench::benchCodeAgentLargeHistory(cfg);
        }},
        {"resource", "Benchmark memory and CPU across all 5 modes (startup/100K/200K)", agentxx::bench::benchResourceAll},
        {"resource_cli", "Benchmark memory and CPU for in-process CLI", agentxx::bench::benchResourceCli, true},
        {"resource_tui", "Benchmark memory and CPU for in-process TUI", agentxx::bench::benchResourceTui, true},
        {"resource_split_cli", "Benchmark memory and CPU for split CLI + Server", agentxx::bench::benchResourceSplitCli, true},
        {"resource_split_tui", "Benchmark memory and CPU for split TUI + Server", agentxx::bench::benchResourceSplitTui, true},
        {"resource_ffi", "Benchmark memory and CPU for libagentxx_shared control group", agentxx::bench::benchResourceFfi, true},
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

    bool runAll = selectedModules.empty();
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
    if (auto envDir = agentxx::util::ApplicationEnv::instance().get("AGENTXX_BENCH_OUTPUT_DIR")) {
        outputDir = *envDir;
    }
    if (!outputDir.empty()) {
        reporter.setOutputDir(outputDir);
        std::cout << "[BenchReporter] output dir: " << outputDir << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  agentxx Performance Benchmarks" << std::endl;
    std::cout << "========================================" << std::endl;

    for (const auto& mod : modules) {
        if (!shouldRun(mod)) {
            continue;
        }
        std::cout << "\n>>> Running module: " << mod.name << " (" << mod.description << ")" << std::endl;
        try {
            mod.run();
        } catch (const std::exception& e) {
            std::cerr << "[Error] Module " << mod.name << " threw exception: " << e.what() << std::endl;
            if (failFast) {
                std::cerr << "======= FAIL-FAST: aborting after " << mod.name << " =======" << std::endl;
                std::_Exit(1);
            }
        } catch (...) {
            std::cerr << "[Error] Module " << mod.name << " threw unknown exception" << std::endl;
            if (failFast) {
                std::cerr << "======= FAIL-FAST: aborting after " << mod.name << " =======" << std::endl;
                std::_Exit(1);
            }
        }
    }

    reporter.flushToFile();

    std::cout << "\n========================================" << std::endl;
    std::cout << "  All Benchmarks Complete" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
