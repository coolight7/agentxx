#include "agentxx-client/train/train.h"

#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/signal_set.hpp"
#include "fmt/format.h"
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

std::string findProjectRoot() {
    auto exeDir    = std::filesystem::current_path();
    auto candidate = exeDir;
    for (int i = 0; i < 6; ++i) {
        if (std::filesystem::exists(candidate / "agent")
            && std::filesystem::exists(candidate / "resource")) {
            return candidate.string();
        }
        candidate = candidate.parent_path();
    }
    return exeDir.string();
}

void replacePlaceholders(
    std::vector<agentxx::agent::TrainingTestCase>& cases,
    std::string_view                               projectRoot
) {
    const std::string placeholder = "{agentxx_root}";
    for (auto& tc : cases) {
        auto pos = tc.input.find(placeholder);
        if (pos != std::string::npos) {
            tc.input.replace(pos, placeholder.size(), projectRoot);
        }
        pos = tc.expectedOutput.find(placeholder);
        if (pos != std::string::npos) {
            tc.expectedOutput.replace(pos, placeholder.size(), projectRoot);
        }
    }
}

void runTrainingMode(
    std::shared_ptr<agentxx::agent::AgentConfig> baseConfig,
    std::shared_ptr<agentxx::agent::AgentConfig> scorerConfig,
    std::shared_ptr<agentxx::agent::AgentConfig> optimizerConfig
) {
    XX_OUT("======= Agentxx Training Mode =======");

    // 训练主代理需要完整工具链, 使用 CodeAgent;
    // 评分器/优化器只需一次纯文本补全, 使用轻量 BaseAgent:
    // 不注册编程工具与中间件栈, 避免评分模型误调工具导致输出为空/被污染,
    // 也省去权限 HIL / summarization 等无谓开销。
    // (system prompt 由 EvolutionTrainingAgent.runLLMAgent 每次调用时写入 config)
    auto trainAgent     = std::make_shared<agentxx::agent::CodeAgent>(baseConfig);
    auto scorerAgent    = std::make_shared<agentxx::agent::BaseAgent>(scorerConfig);
    auto optimizerAgent = std::make_shared<agentxx::agent::BaseAgent>(optimizerConfig);

    std::string projectRoot = findProjectRoot();
    std::string dataDir     = fmt::format("{}/resource/train/data", projectRoot);
    std::string resultsDir  = fmt::format("{}/resource/train/results", projectRoot);

    agentxx::agent::EvolutionTrainingConfig trainCfg;

    trainCfg.topK                 = 100;
    trainCfg.mutateCount          = 5;
    trainCfg.childrenPerParent    = 2;
    trainCfg.convergenceThreshold = 0.7;
    trainCfg.verbose              = true;
    {
        auto                    now = std::chrono::system_clock::now();
        std::chrono::zoned_time local_time{std::chrono::current_zone(), now};
        std::string             timestamp = std::format("{:%Y%m%d_%H%M%S}", local_time);

        trainCfg.saveFilePath = (std::filesystem::path(resultsDir)
                                 / fmt::format("training_prompts_{}.json", timestamp))
                                    .generic_string();
    }

    {
        XX_OUT("[Training] Project root: {}", projectRoot);
        XX_OUT("[Training] Loading test cases from: {}", dataDir);

        if (std::filesystem::exists(dataDir)) {
            // 库内递归加载器 (含子目录), 并做用例名唯一化
            trainCfg.testCases = agentxx::agent::loadTestCasesFromDirectory(dataDir, true);
            replacePlaceholders(trainCfg.testCases, projectRoot);
            XX_OUT(
                "[Training] Loaded {} test cases from resource/train/data",
                trainCfg.testCases.size()
            );
        } else {
            XX_LOGE("[Training] Data directory not found: {}", dataDir);
        }
    }

    {
        std::ifstream tcFile("./training_testcases.json");
        if (tcFile.is_open()) {
            agentxx::util::catchError<bool>(
                [&]() -> bool {
                    std::string content(
                        (std::istreambuf_iterator<char>(tcFile)),
                        std::istreambuf_iterator<char>()
                    );
                    auto j = neograph::json::parse(content);
                    if (j.is_array()) {
                        // 复用库内解析: 字段语义与文件加载完全一致, 含重名唯一化
                        trainCfg.testCases = agentxx::agent::testCasesFromJson(j);
                        XX_OUT(
                            "[Training] Loaded {} test cases from training_testcases.json",
                            trainCfg.testCases.size()
                        );
                    }
                    return true;
                },
                [](std::string errmsg) -> bool {
                    XX_LOGE("[Training] Failed to parse training_testcases.json: {}", errmsg);
                    return false;
                }
            );
        }
    }

    if (trainCfg.testCases.empty()) {
        XX_LOGE("[Training] No test cases available. Aborting.");
        return;
    }

    XX_OUT("[Training] Test cases: {}", trainCfg.testCases.size());
    XX_OUT("[Training] Save path: {}", trainCfg.saveFilePath);
    XX_OUT("[Training] Top K: {}", trainCfg.topK);
    XX_OUT("[Training] Starting evolution loop...");

    asio::io_context trainIoCtx;

    // 取消令牌: Ctrl+C(SIGINT)/SIGTERM 时优雅停止训练 (保存当前进度后退出)
    auto cancelToken     = std::make_shared<neograph::graph::CancelToken>();
    trainCfg.cancelToken = cancelToken;

    asio::signal_set signals(trainIoCtx, SIGINT, SIGTERM);
    signals.async_wait([&cancelToken, &signals](const std::error_code& ec, int sig) {
        if (ec) {
            return; // 被主动 cancel, 正常退出路径
        }
        XX_LOGW(
            "[Training] Received signal {}, cancelling training gracefully "
            "(progress will be saved)...",
            sig
        );
        if (cancelToken) {
            cancelToken->cancel();
        }
        // 注销信号监听: 之后再次 Ctrl+C 按默认行为立即终止进程
        signals.clear();
    });

    asio::co_spawn(
        trainIoCtx,
        [&]() -> asio::awaitable<void> {
            // catchErrorAsync: 初始化/训练中的普通错误记录日志后结束;
            // 取消与中断类异常默认放行 (本项目异常处理约定)
            co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {
                    XX_OUT("[Training] Initializing agents...");
                    co_await trainAgent->init();
                    co_await scorerAgent->init();
                    co_await optimizerAgent->init();
                    XX_OUT("[Training] All agents initialized.");

                    agentxx::agent::EvolutionTrainingAgent trainer(
                        scorerAgent,
                        trainAgent,
                        optimizerAgent
                    );
                    trainer.seedInitialPopulationFromAgent();

                    XX_OUT("[Training] Entering evolution loop...");
                    co_await trainer.runEvolutionLoop(trainCfg);
                    co_return true;
                },
                [](std::string errmsg) -> asio::awaitable<bool> {
                    XX_LOGE("[Training] Training stopped by error: {}", errmsg);
                    co_return false;
                }
            );
            XX_OUT("[Training] Finished.");
            // 结束信号监听, 让 io_context::run() 可以返回
            signals.cancel();
            co_return;
        },
        asio::detached
    );

    trainIoCtx.run();
}
