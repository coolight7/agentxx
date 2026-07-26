#include "agentxx-client/train/train.h"

#include "agentxx/agent/training.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "fmt/format.h"
#include "neograph/api.h"
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

std::vector<agentxx::agent::TrainingTestCase> loadTestCasesRecursive(std::string_view dirPath) {
    std::vector<agentxx::agent::TrainingTestCase> allCases;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                auto fileCases = agentxx::agent::loadTestCasesFromFile(entry.path().string());
                allCases.insert(
                    allCases.end(),
                    std::make_move_iterator(fileCases.begin()),
                    std::make_move_iterator(fileCases.end())
                );
            } else if (entry.is_directory()) {
                auto subCases = loadTestCasesRecursive(entry.path().string());
                allCases.insert(
                    allCases.end(),
                    std::make_move_iterator(subCases.begin()),
                    std::make_move_iterator(subCases.end())
                );
            }
        }
    } catch (const std::exception& e) {
        XX_LOGE("[Training] Failed to load from directory {}:{}", dirPath, e.what());
    }
    return allCases;
}

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

    auto trainAgent                   = std::make_shared<agentxx::agent::DeepAgent>(baseConfig);
    scorerConfig->prompt.systemPrompt = agentxx::agent::EvolutionTrainingConfig{}.scoringPrompt;
    auto scorerAgent                  = std::make_shared<agentxx::agent::DeepAgent>(scorerConfig);
    optimizerConfig->prompt.systemPrompt
        = agentxx::agent::EvolutionTrainingConfig{}.optimizerPrompt;
    auto optimizerAgent = std::make_shared<agentxx::agent::DeepAgent>(optimizerConfig);

    std::string projectRoot = findProjectRoot();
    std::string dataDir     = projectRoot + "/resource/train/data";
    std::string resultsDir  = projectRoot + "/resource/train/results";

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
            trainCfg.testCases = loadTestCasesRecursive(dataDir);
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
            try {
                std::string content(
                    (std::istreambuf_iterator<char>(tcFile)),
                    std::istreambuf_iterator<char>()
                );
                auto j = neograph::json::parse(content);
                if (j.is_array()) {
                    trainCfg.testCases.clear();
                    for (const auto& item : j) {
                        agentxx::agent::TrainingTestCase tc;
                        tc.name           = item.value("name", "");
                        tc.input          = item.value("input", "");
                        tc.expectedOutput = item.value("expectedOutput", "");
                        tc.equalOutput    = item.value("equalOutput", "");
                        tc.extra          = item.value("extra", neograph::json::object());
                        if (!tc.name.empty() && !tc.input.empty()) {
                            trainCfg.testCases.push_back(std::move(tc));
                        }
                    }
                    XX_OUT(
                        "[Training] Loaded {} test cases from training_testcases.json",
                        trainCfg.testCases.size()
                    );
                }
            } catch (const std::exception& e) {
                XX_LOGE("[Training] Failed to parse training_testcases.json: {}", e.what());
            }
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

    asio::co_spawn(
        trainIoCtx,
        [trainAgent, scorerAgent, optimizerAgent, trainCfg]() -> asio::awaitable<void> {
            XX_OUT("[Training] Initializing agents...");
            co_await trainAgent->init();
            co_await scorerAgent->init();
            co_await optimizerAgent->init();
            XX_OUT("[Training] All agents initialized.");

            agentxx::agent::EvolutionTrainingAgent trainer(scorerAgent, trainAgent, optimizerAgent);
            trainer.seedInitialPopulation(trainAgent->getContext()->agentConfig->prompt.systemPrompt
            );

            XX_OUT("[Training] Entering evolution loop...");
            co_await trainer.runEvolutionLoop(trainCfg);
        },
        asio::detached
    );

    trainIoCtx.run();
}
