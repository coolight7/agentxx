#include "agentxx/agent/training.h"

#include "agentxx/util/exception.h"
#include <algorithm>
#include <chrono>
#include <fmt/core.h>
#include <iostream>
#include <set>
#include <sstream>

namespace agentxx {
namespace agent {

double PromptVariant::averageScore() const {
    return testCount > 0 ? cumulativeScore / testCount : 0.0;
}

size_t PromptVariant::promptHash() const {
    return prompt.promptHash();
}

asio::awaitable<std::string> EvolutionTrainingAgent::runLLMAgent(
    std::shared_ptr<agentxx::agent::BaseAgent> agent,
    std::string_view                           systemPrompt,
    std::string_view                           userContent
) {
    agent->getContext()->agentConfig->prompt.systemPrompt = std::string{systemPrompt};
    std::vector<neograph::ChatMessage> messages           = {
        neograph::ChatMessage{.role = "user", .content = std::string{userContent}},
    };
    auto result = co_await agent->runStreamAsync(messages);
    co_return result.content;
}

void EvolutionTrainingAgent::applyVariantToTrainAgent(const PromptVariant& variant) {
    auto cfg    = trainAgent->getContext()->agentConfig;
    cfg->prompt = variant.prompt;
}

neograph::json EvolutionTrainingAgent::promptVariantToJson(const PromptVariant& v) const {
    neograph::json j;
    j["id"]              = v.id;
    j["prompt"]          = v.prompt.toJson();
    j["cumulativeScore"] = v.cumulativeScore;
    j["testCount"]       = v.testCount;
    j["generation"]      = v.generation;
    j["parentId"]        = v.parentId;
    {
        neograph::json scores = neograph::json::object();
        for (const auto& kv : v.perTestCaseScores) {
            scores[kv.first] = kv.second;
        }
        j["perTestCaseScores"] = scores;
    }
    j["extra"] = v.extra;
    return j;
}

PromptVariant EvolutionTrainingAgent::promptVariantFromJson(const neograph::json& j) const {
    PromptVariant v;
    v.id = j.value("id", std::string{});
    if (j.contains("prompt") && j["prompt"].is_object()) {
        v.prompt.mergeFromJson(j["prompt"]);
    } else {
        // 兼容旧版只存了 3 个 system prompt 字段的格式
        v.prompt.mergeFromJson(j);
    }
    v.cumulativeScore = j.value("cumulativeScore", 0.0);
    v.testCount       = j.value("testCount", 0);
    v.generation      = j.value("generation", 0);
    v.parentId        = j.value("parentId", std::string{});
    if (j.contains("perTestCaseScores") && j["perTestCaseScores"].is_object()) {
        auto scores = j["perTestCaseScores"];
        for (const auto& item : scores.items()) {
            v.perTestCaseScores[item.first] = item.second.get<double>();
        }
    }
    v.extra = j.value("extra", neograph::json::object());
    return v;
}

void EvolutionTrainingAgent::rotateSaveFile(std::string_view path, int keepCount) {
    if (keepCount <= 0) {
        return;
    }
    namespace fs = std::filesystem;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            fs::path oldest(fmt::format("{}.{}", path, keepCount));
            if (fs::exists(oldest)) {
                fs::remove(oldest);
            }
            for (int i = keepCount - 1; i >= 1; --i) {
                fs::path from(fmt::format("{}.{}", path, i));
                fs::path to(fmt::format("{}.{}", path, i + 1));
                if (fs::exists(from)) {
                    fs::rename(from, to);
                }
            }
            fs::path cur(path);
            if (fs::exists(cur)) {
                fs::copy_file(cur, fmt::format("{}.1", path), fs::copy_options::overwrite_existing);
            }
            return true;
        },
        [](std::string errmsg) -> bool {
            XX_LOGD("[EvolutionTraining] Backup rotation skipped: {}", errmsg);
            return false;
        }
    );
}

void EvolutionTrainingAgent::savePopulationToFile(std::string_view filePath, int backupCount) {
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            if (backupCount > 0) {
                rotateSaveFile(filePath, backupCount);
            }

            neograph::json j = neograph::json::array();
            for (const auto& v : population) {
                j.push_back(promptVariantToJson(v));
            }

            neograph::json root;
            root["population"]        = j;
            root["generationCounter"] = generationCounter;
            root["savedAt"]           = std::chrono::system_clock::now().time_since_epoch().count();

            std::ofstream ofs(std::string{filePath}, std::ios::out | std::ios::trunc);
            if (ofs.is_open()) {
                ofs << root.dump(2);
                ofs.close();
                XX_LOGD("[EvolutionTraining] Saved {} prompts to {}", population.size(), filePath);
            } else {
                XX_LOGE("[EvolutionTraining] Failed to open file for writing: {}", filePath);
            }
            return true;
        },
        [](std::string errmsg) -> bool {
            XX_LOGE("[EvolutionTraining] Failed to save population: {}", errmsg);
            return false;
        }
    );
}

bool EvolutionTrainingAgent::loadPopulationFromFile(std::string_view filePath) {
    bool loaded = agentxx::util::catchError<bool>(
        [&]() -> bool {
            std::ifstream ifs(std::string{filePath});
            if (!ifs.is_open()) {
                XX_LOGD(
                    "[EvolutionTraining] No existing save file found at {}, "
                    "starting fresh",
                    filePath
                );
                return false;
            }

            std::string content(
                (std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>()
            );
            ifs.close();

            if (content.empty()) {
                return false;
            }

            auto root = neograph::json::parse(content);
            if (root.contains("population") && root["population"].is_array()) {
                population.clear();
                for (const auto& j : root["population"]) {
                    population.push_back(promptVariantFromJson(j));
                }
                generationCounter = root.value("generationCounter", 0);

                std::sort(
                    population.begin(),
                    population.end(),
                    [](const PromptVariant& a, const PromptVariant& b) {
                        return a.averageScore() > b.averageScore();
                    }
                );

                XX_LOGD(
                    "[EvolutionTraining] Loaded {} prompts from {} (generation {})",
                    population.size(),
                    filePath,
                    generationCounter
                );
                return true;
            }
            return false;
        },
        [filePath](std::string errmsg) -> bool {
            XX_LOGE("[EvolutionTraining] Failed to load population: {}", errmsg);
            return false;
        }
    );
    return loaded;
}

asio::awaitable<TrainingScore> EvolutionTrainingAgent::defaultScoringWithSubAgent(
    std::string_view               agentOutput,
    const TrainingTestCase&        testCase,
    int                            iteration,
    const EvolutionTrainingConfig& cfg
) {
    TrainingScore result;
    result.iteration = iteration;

    // 若设置了 equalOutput，则优先进行精确匹配判断
    if (!testCase.equalOutput.empty()) {
        if (agentOutput == testCase.equalOutput) {
            result.score    = 1.0;
            result.feedback = "Output exactly matches equalOutput.";
            result.passed   = true;
        } else {
            result.score    = 0.0;
            result.feedback = "Output does not match equalOutput.";
            result.passed   = false;
        }
        co_return result;
    }

    std::ostringstream scoringMessage;
    scoringMessage << "Test Case: " << testCase.name << "\n";
    scoringMessage << "User Input: " << testCase.input << "\n";
    if (!testCase.expectedOutput.empty()) {
        scoringMessage << "Expected Output / Scoring Criteria: " << testCase.expectedOutput << "\n";
    }
    scoringMessage << "\nAgent Response:\n" << agentOutput << "\n";
    scoringMessage << "\nPass threshold: score >= " << cfg.convergenceThreshold << "\n";
    if (testCase.extra.contains("language")) {
        scoringMessage << "Required language: " << testCase.extra["language"].get<std::string>()
                       << "\n";
    }

    // catchErrorAsync: 评分子代理异常 (含取消/中断类) 转为 0 分反馈, 训练循环继续
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            auto content
                = co_await runLLMAgent(scoreAgent, cfg.scoringPrompt, scoringMessage.str());
            auto parsed = parseJsonFromResponse(content);
            if (parsed.is_object()) {
                result.score    = parsed.value("score", 0.0);
                result.feedback = parsed.value("feedback", std::string{});
                result.passed   = parsed.value("passed", false);
                if (parsed.contains("extra") && parsed["extra"].is_object()) {
                    result.extra = parsed["extra"];
                }
            } else {
                result.score    = 0.0;
                result.feedback = fmt::format("Scorer returned non-JSON: {}", content);
                result.passed   = false;
            }
            co_return true;
        },
        [&](std::string errmsg) -> asio::awaitable<bool> {
            result.score    = 0.0;
            result.feedback = std::string("Scoring subagent error: ") + std::move(errmsg);
            result.passed   = false;
            co_return false;
        },
        [&](std::string& errmsg) -> std::optional<bool> {
            result.score    = 0.0;
            result.feedback = std::string("Scoring subagent error: ") + std::move(errmsg);
            result.passed   = false;
            return false;
        }
    );

    co_return result;
}

std::string EvolutionTrainingAgent::buildPromptContextMessage(const PromptVariant& variant) const {
    std::ostringstream msg;
    msg << "Current System Prompt:\n```\n" << variant.prompt.systemPrompt << "\n```\n\n";
    if (!variant.prompt.systemPlanningPrompt.empty()) {
        msg << "Current Planning Prompt:\n```\n"
            << variant.prompt.systemPlanningPrompt << "\n```\n\n";
    }
    if (!variant.prompt.systemSkillPrompt.empty()) {
        msg << "Current Skill Prompt:\n```\n" << variant.prompt.systemSkillPrompt << "\n```\n\n";
    }
    if (!variant.prompt.toolPrompt.empty()) {
        msg << "Current Tool Prompts:\n";
        for (const auto& kv : variant.prompt.toolPrompt) {
            msg << "### " << kv.first << "\n";
            msg << "  depict: " << kv.second.depict << "\n";
            for (const auto& a : kv.second.args) {
                msg << "  arg[" << a.first << "]: " << a.second << "\n";
            }
            msg << "\n";
        }
    }
    return msg.str();
}

asio::awaitable<OptimizedPrompts> EvolutionTrainingAgent::optimizeVariantWithLLM(
    const PromptVariant&           variant,
    const TrainingTestCase&        testCase,
    std::string_view               agentOutput,
    const TrainingScore&           score,
    const EvolutionTrainingConfig& cfg
) {
    OptimizedPrompts result;
    if (!optimizerAgent) {
        co_return result;
    }

    std::ostringstream msg;
    msg << buildPromptContextMessage(variant);
    msg << "Test Case: " << testCase.name << "\n";
    msg << "User Input: " << testCase.input << "\n";
    if (!testCase.expectedOutput.empty()) {
        msg << "Expected Output / Scoring Criteria: " << testCase.expectedOutput << "\n";
    }
    if (!testCase.equalOutput.empty()) {
        msg << "Required Exact Output: " << testCase.equalOutput << "\n";
    }
    msg << "\nAgent Output:\n```\n" << agentOutput << "\n```\n\n";
    msg << "Score: " << score.score << "\n";
    msg << "Feedback: " << score.feedback << "\n";
    msg << "\nPlease provide improved prompts.";

    // catchErrorAsync: 优化器异常 (含取消/中断类) 仅记录日志, 训练循环继续
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            auto content = co_await runLLMAgent(optimizerAgent, cfg.optimizerPrompt, msg.str());
            auto parsed  = parseJsonFromResponse(content);
            if (parsed.is_object()) {
                // 提取 patch（移除非 prompt 字段）
                neograph::json patch = neograph::json::object();
                if (parsed.contains("systemPrompt")) {
                    patch["systemPrompt"] = parsed["systemPrompt"];
                }
                if (parsed.contains("systemPlanningPrompt")) {
                    patch["systemPlanningPrompt"] = parsed["systemPlanningPrompt"];
                }
                if (parsed.contains("systemSkillPrompt")) {
                    patch["systemSkillPrompt"] = parsed["systemSkillPrompt"];
                }
                if (parsed.contains("toolPrompt")) {
                    patch["toolPrompt"] = parsed["toolPrompt"];
                }
                result.patch    = patch;
                result.analysis = parsed.value("analysis", std::string{});
                if (!patch.empty()) {
                    XX_LOGD("[EvolutionTraining] Optimizer produced prompt patch");
                }
            }
            co_return true;
        },
        [](std::string errmsg) -> asio::awaitable<bool> {
            XX_LOGE("[EvolutionTraining] Optimizer error: {}", errmsg);
            co_return false;
        },
        [](std::string& errmsg) -> std::optional<bool> {
            XX_LOGE("[EvolutionTraining] Optimizer error: {}", errmsg);
            return false;
        }
    );
    co_return result;
}

std::string EvolutionTrainingAgent::generateId() {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return fmt::format("gen{}_{}", generationCounter, now);
}

std::string EvolutionTrainingAgent::mutateString(std::string_view input, double mutationRate) {
    if (mutationRate <= 0.0 || input.empty()) {
        return std::string{input};
    }
    std::string result;
    result.reserve(input.size() * 2);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    static const char                      mutationChars[]
        = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 "
          ".,;:!?-_()[]{}\n";
    static const int mutationCharsLen = static_cast<int>(sizeof(mutationChars) - 1);
    std::uniform_int_distribution<int> charDist(0, mutationCharsLen - 1);
    std::uniform_int_distribution<int> opDist(0, 2);

    for (size_t i = 0; i < input.size(); ++i) {
        if (dist(rng) < mutationRate) {
            int op = opDist(rng);
            switch (op) {
                case 0:
                    result.push_back(mutationChars[charDist(rng)]);
                    break;
                case 1:
                    result.push_back(mutationChars[charDist(rng)]);
                    result.push_back(input[i]);
                    break;
                case 2:
                    break;
            }
        } else {
            result.push_back(input[i]);
        }
    }
    if (result.empty()) {
        result = input;
    }
    return result;
}

PromptVariant EvolutionTrainingAgent::createChildVariantCharMut(
    const PromptVariant& parent,
    double               mutationRate
) {
    PromptVariant child;
    child.id     = generateId();
    child.prompt = parent.prompt;
    // 字符级变异仅作用于 3 个 system prompt，toolPrompt 保持不变
    // （字符级变异会破坏语义，仅作为 LLM 变异不可用时的降级手段）
    child.prompt.systemPrompt = mutateString(parent.prompt.systemPrompt, mutationRate);
    child.prompt.systemPlanningPrompt
        = mutateString(parent.prompt.systemPlanningPrompt, mutationRate);
    child.prompt.systemSkillPrompt = mutateString(parent.prompt.systemSkillPrompt, mutationRate);
    child.generation               = generationCounter;
    child.parentId                 = parent.id;
    return child;
}

asio::awaitable<PromptVariant> EvolutionTrainingAgent::createChildVariantLLMMut(
    const PromptVariant&           parent,
    const EvolutionTrainingConfig& cfg
) {
    PromptVariant child;
    child.id         = generateId();
    child.prompt     = parent.prompt;
    child.generation = generationCounter;
    child.parentId   = parent.id;

    std::ostringstream msg;
    msg << buildPromptContextMessage(parent);
    msg << "\nGenerate a diverse variation of these prompts.";

    // catchErrorAsync: LLM 变异失败 (含取消/中断类) 降级为字符变异, 训练循环继续
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            auto content = co_await runLLMAgent(optimizerAgent, cfg.mutationPrompt, msg.str());
            auto parsed  = parseJsonFromResponse(content);
            if (parsed.is_object()) {
                // 以 patch 方式合并：只覆盖出现的字段
                child.prompt.mergeFromJson(parsed);
            } else {
                child = createChildVariantCharMut(parent, cfg.mutationRate);
            }
            co_return true;
        },
        [&](std::string errmsg) -> asio::awaitable<bool> {
            XX_LOGD(
                "[EvolutionTraining] LLM mutation failed, fallback to char mut: "
                "{}",
                errmsg
            );
            child = createChildVariantCharMut(parent, cfg.mutationRate);
            co_return false;
        },
        [&](std::string& errmsg) -> std::optional<bool> {
            XX_LOGD(
                "[EvolutionTraining] LLM mutation failed, fallback to char mut: "
                "{}",
                errmsg
            );
            child = createChildVariantCharMut(parent, cfg.mutationRate);
            return false;
        }
    );
    co_return child;
}

asio::awaitable<EvolutionTrainingAgent::EvaluationResult> EvolutionTrainingAgent::evaluateVariant(
    PromptVariant&                 variant,
    const EvolutionTrainingConfig& cfg
) {
    EvaluationResult evResult;
    variant.perTestCaseScores.clear();
    double                totalScore      = 0.0;
    int                   testCount       = 0;
    [[maybe_unused]] bool earlyTerminated = false;

    for (size_t caseIdx = 0; caseIdx < cfg.testCases.size(); ++caseIdx) {
        const auto& testCase = cfg.testCases[caseIdx];
        const auto  threadId = fmt::format("evotrain_{}_{}_{}", variant.id, caseIdx, testCase.name);

        applyVariantToTrainAgent(variant);

        if (cfg.verbose) {
            XX_LOGD(
                "[EvolutionTraining] [{}] Testing case '{}/{}' with variant '{}'",
                generationCounter,
                caseIdx + 1,
                cfg.testCases.size(),
                variant.id
            );
        }

        std::string agentOutput
            = co_await trainAgent->runSingleInputAsync(threadId, testCase.input, "");

        if (cfg.verbose) {
            XX_LOGD(
                "[EvolutionTraining] [{}] Output (len={}): {}",
                generationCounter,
                agentOutput.size(),
                agentOutput.size() > 200
                    ? fmt::format("{}...", std::string_view{agentOutput}.substr(0, 200))
                    : agentOutput
            );
        }

        TrainingScore score;
        if (cfg.scoringFunc) {
            score = co_await cfg.scoringFunc(agentOutput, testCase, generationCounter);
        } else {
            score = co_await defaultScoringWithSubAgent(
                agentOutput,
                testCase,
                generationCounter,
                cfg
            );
        }

        variant.perTestCaseScores[testCase.name]  = score.score;
        totalScore                               += score.score;
        testCount++;

        if (evResult.worstCase == nullptr || score.score < evResult.worstCaseScore.score) {
            evResult.worstCase       = &testCase;
            evResult.worstCaseOutput = agentOutput;
            evResult.worstCaseScore  = score;
        }

        if (cfg.verbose) {
            XX_LOGD(
                "[EvolutionTraining] [{}] Score: {:.3f}, Passed: {}, Feedback: {}",
                generationCounter,
                score.score,
                score.passed,
                score.feedback
            );
        }

        if (cfg.onIteration) {
            cfg.onIteration(score, agentOutput);
        }

        // 早终检查
        if (cfg.earlyTerminationCheckAfter > 0 && testCount >= cfg.earlyTerminationCheckAfter) {
            double avg = totalScore / testCount;
            if (avg < cfg.earlyTerminationScore) {
                if (cfg.verbose) {
                    XX_LOGD(
                        "[EvolutionTraining] [{}] Early-terminating variant '{}' "
                        "avg={:.3f} < {:.3f}",
                        generationCounter,
                        variant.id,
                        avg,
                        cfg.earlyTerminationScore
                    );
                }
                earlyTerminated = true;
                for (size_t j = caseIdx + 1; j < cfg.testCases.size(); ++j) {
                    variant.perTestCaseScores[cfg.testCases[j].name] = 0.0;
                }
                break;
            }
        }
    }

    // 跳过的用例记 0 分，testCount 取总用例数以保证 averageScore 可比
    variant.cumulativeScore = totalScore;
    variant.testCount       = static_cast<int>(cfg.testCases.size());

    if (cfg.verbose) {
        XX_LOGD(
            "[EvolutionTraining] [{}] Variant '{}' avgScore={:.4f}{}",
            generationCounter,
            variant.id,
            variant.averageScore(),
            earlyTerminated ? " (early-terminated)" : ""
        );
    }
    co_return evResult;
}

void EvolutionTrainingAgent::deduplicatePopulation() {
    std::set<size_t>           seen;
    std::vector<PromptVariant> unique;
    unique.reserve(population.size());
    for (auto& v : population) {
        auto h = v.promptHash();
        if (seen.insert(h).second) {
            unique.push_back(std::move(v));
        }
    }
    if (unique.size() != population.size()) {
        XX_LOGD(
            "[EvolutionTraining] Deduplicated population: {} -> {}",
            population.size(),
            unique.size()
        );
    }
    population = std::move(unique);
}

EvolutionTrainingAgent::EvolutionTrainingAgent(
    std::shared_ptr<agentxx::agent::BaseAgent> in_scoreAgent,
    std::shared_ptr<agentxx::agent::BaseAgent> in_trainAgent,
    std::shared_ptr<agentxx::agent::BaseAgent> in_optimizerAgent
) :
    scoreAgent(in_scoreAgent),
    trainAgent(in_trainAgent),
    optimizerAgent(in_optimizerAgent),
    rng(std::chrono::system_clock::now().time_since_epoch().count()) {}

void EvolutionTrainingAgent::seedInitialPopulation(std::string_view baseSystemPrompt) {
    if (!population.empty()) {
        return;
    }

    PromptVariant seed;
    seed.id = generateId();
    if (trainAgent) {
        // 从 trainAgent 当前配置复制完整 prompt，再覆盖 systemPrompt
        seed.prompt = trainAgent->getContext()->agentConfig->prompt;
    }
    seed.prompt.systemPrompt = baseSystemPrompt;
    seed.generation          = 0;
    seed.parentId            = "seed";
    population.push_back(std::move(seed));

    XX_LOGD("[EvolutionTraining] Seeded initial population with 1 prompt");
}

void EvolutionTrainingAgent::seedInitialPopulation(const AgentPrompt& prompt) {
    if (!population.empty()) {
        return;
    }

    PromptVariant seed;
    seed.id         = generateId();
    seed.prompt     = prompt;
    seed.generation = 0;
    seed.parentId   = "seed";
    population.push_back(std::move(seed));

    XX_LOGD("[EvolutionTraining] Seeded initial population from AgentPrompt");
}

void EvolutionTrainingAgent::seedInitialPopulationFromAgent() {
    if (!population.empty() || !trainAgent) {
        return;
    }
    seedInitialPopulation(trainAgent->getContext()->agentConfig->prompt);
}

asio::awaitable<void> EvolutionTrainingAgent::runEvolutionLoop(const EvolutionTrainingConfig& cfg) {
    // Step 1: 尝试从文件加载已有 population
    bool loaded = loadPopulationFromFile(cfg.saveFilePath);

    if (!loaded && population.empty()) {
        XX_LOGE("[EvolutionTraining] No population loaded and no seed provided. "
                "Call seedInitialPopulation() first or provide a save file.");
        co_return;
    }

    if (!loaded && cfg.verbose) {
        XX_LOGD("[EvolutionTraining] Starting fresh with {} seed prompts", population.size());
    }

    deduplicatePopulation();
    std::sort(
        population.begin(),
        population.end(),
        [](const PromptVariant& a, const PromptVariant& b) {
            return a.averageScore() > b.averageScore();
        }
    );

    double bestScoreEver                 = population.empty() ? 0.0 : population[0].averageScore();
    int    generationsWithoutImprovement = 0;

    while (true) {
        generationCounter++;

        if (cfg.verbose) {
            XX_LOGD(
                "[EvolutionTraining] ====== Generation {} | Population: {} ======",
                generationCounter,
                population.size()
            );
        }

        // 2a. 从当前 population 中选取 top 变体进行变异
        int mutateFrom = std::min(cfg.mutateCount, static_cast<int>(population.size()));

        std::vector<PromptVariant> newGeneration;

        for (int i = 0; i < mutateFrom; ++i) {
            const auto& parent = population[i];

            for (int c = 0; c < cfg.childrenPerParent; ++c) {
                if (cfg.useLLMMutation && optimizerAgent) {
                    newGeneration.push_back(co_await createChildVariantLLMMut(parent, cfg));
                } else {
                    newGeneration.push_back(createChildVariantCharMut(parent, cfg.mutationRate));
                }
            }
        }

        if (cfg.verbose) {
            XX_LOGD(
                "[EvolutionTraining] [{}] Created {} new variants from top {} "
                "parents (mutation={})",
                generationCounter,
                newGeneration.size(),
                mutateFrom,
                cfg.useLLMMutation ? "LLM" : "char"
            );
        }

        // 2b. 测试所有新变体，并收集最差用例信息
        std::vector<EvaluationResult> evalResults;
        evalResults.reserve(newGeneration.size());
        for (auto& variant : newGeneration) {
            auto ev = co_await evaluateVariant(variant, cfg);
            evalResults.push_back(std::move(ev));
        }

        // 2c. 对低分变体使用优化器改进 prompt
        std::vector<PromptVariant> optimizedVariants;
        if (optimizerAgent && cfg.maxOptimizationsPerGen > 0) {
            struct Cand {
                size_t index;
                double score;
            };

            std::vector<Cand> candidates;
            for (size_t i = 0; i < newGeneration.size(); ++i) {
                if (newGeneration[i].averageScore() < cfg.convergenceThreshold) {
                    candidates.push_back({i, newGeneration[i].averageScore()});
                }
            }
            std::sort(candidates.begin(), candidates.end(), [](const Cand& a, const Cand& b) {
                return a.score < b.score;
            });

            int optCount
                = std::min(cfg.maxOptimizationsPerGen, static_cast<int>(candidates.size()));
            for (int i = 0; i < optCount; ++i) {
                auto&       variant = newGeneration[candidates[i].index];
                const auto& ev      = evalResults[candidates[i].index];
                if (!ev.worstCase) {
                    continue;
                }

                if (cfg.verbose) {
                    XX_LOGD(
                        "[EvolutionTraining] [{}] Optimizing variant '{}' on "
                        "worst case '{}'",
                        generationCounter,
                        variant.id,
                        ev.worstCase->name
                    );
                }

                auto optimized = co_await optimizeVariantWithLLM(
                    variant,
                    *ev.worstCase,
                    ev.worstCaseOutput,
                    ev.worstCaseScore,
                    cfg
                );

                // patch 为空对象表示无修改
                bool hasChange = optimized.patch.is_object() && !optimized.patch.empty();
                if (!hasChange) {
                    continue;
                }

                PromptVariant improved   = variant;
                improved.id              = generateId();
                improved.parentId        = variant.id;
                improved.generation      = generationCounter;
                improved.cumulativeScore = 0.0;
                improved.testCount       = 0;
                improved.perTestCaseScores.clear();
                // 以 patch 方式合并优化结果
                improved.prompt.mergeFromJson(optimized.patch);
                optimizedVariants.push_back(std::move(improved));
            }
        }

        // 2d. 测试优化后的变体（在全部用例上公平评估）
        for (auto& variant : optimizedVariants) {
            if (cfg.verbose) {
                XX_LOGD(
                    "[EvolutionTraining] [{}] Evaluating optimized variant '{}'",
                    generationCounter,
                    variant.id
                );
            }
            co_await evaluateVariant(variant, cfg);
        }

        // 2e. 合并新旧 population
        population.insert(
            population.end(),
            std::make_move_iterator(newGeneration.begin()),
            std::make_move_iterator(newGeneration.end())
        );
        population.insert(
            population.end(),
            std::make_move_iterator(optimizedVariants.begin()),
            std::make_move_iterator(optimizedVariants.end())
        );

        // 2f. 去重、排序并保留 top K
        deduplicatePopulation();
        std::sort(
            population.begin(),
            population.end(),
            [](const PromptVariant& a, const PromptVariant& b) {
                return a.averageScore() > b.averageScore();
            }
        );

        if (population.size() > static_cast<size_t>(cfg.topK)) {
            if (cfg.verbose) {
                XX_LOGD(
                    "[EvolutionTraining] [{}] Trimming population from {} to {}",
                    generationCounter,
                    population.size(),
                    cfg.topK
                );
            }
            population.resize(cfg.topK);
        }

        // 2g. 检查收敛
        double currentBestScore = population.empty() ? 0.0 : population[0].averageScore();
        if (currentBestScore > bestScoreEver) {
            bestScoreEver                 = currentBestScore;
            generationsWithoutImprovement = 0;
        } else {
            generationsWithoutImprovement++;
        }

        // 2h. 打印 top 信息
        if (cfg.verbose && !population.empty()) {
            XX_LOGD("[EvolutionTraining] [{}] Top 5 prompts:", generationCounter);
            for (size_t i = 0; i < std::min(population.size(), static_cast<size_t>(5)); ++i) {
                [[maybe_unused]] const auto& v = population[i];
                XX_LOGD(
                    "  [{}/{}] id={} avgScore={:.4f} tests={} gen={} parent={}",
                    i + 1,
                    population.size(),
                    v.id,
                    v.averageScore(),
                    v.testCount,
                    v.generation,
                    v.parentId
                );
            }

            const auto&                  best = population[0];
            [[maybe_unused]] const auto& sp   = best.prompt.systemPrompt;
            XX_LOGD(
                "[EvolutionTraining] [{}] Best prompt (score={:.4f}):\n{}",
                generationCounter,
                best.averageScore(),
                sp.size() > 300 ? fmt::format("{}...", sp.substr(0, 300)) : sp
            );
        }

        // 2i. 收敛检查
        if (cfg.maxGenerationsWithoutImprovement > 0
            && generationsWithoutImprovement >= cfg.maxGenerationsWithoutImprovement) {
            XX_LOGD(
                "[EvolutionTraining] Converged! Best score {:.4f} unchanged "
                "for {} generations. Stopping training.",
                bestScoreEver,
                generationsWithoutImprovement
            );
            savePopulationToFile(cfg.saveFilePath, cfg.saveFileBackupCount);
            break;
        }

        // 2j. 最大代数检查
        if (cfg.maxGenerations > 0 && generationCounter >= cfg.maxGenerations) {
            XX_LOGD("[EvolutionTraining] Reached maxGenerations {}. Stopping.", cfg.maxGenerations);
            savePopulationToFile(cfg.saveFilePath, cfg.saveFileBackupCount);
            break;
        }

        // 2k. 保存到文件（带备份轮转）
        savePopulationToFile(cfg.saveFilePath, cfg.saveFileBackupCount);
    }

    co_return;
}

const std::vector<PromptVariant>& EvolutionTrainingAgent::getPopulation() const {
    return population;
}

const PromptVariant* EvolutionTrainingAgent::getBestPrompt() const {
    if (population.empty()) {
        return nullptr;
    }
    return &population[0];
}

void EvolutionTrainingAgent::applyBestPromptToConfig(std::shared_ptr<AgentConfig> config) {
    const auto* best = getBestPrompt();
    if (!best) {
        return;
    }
    config->prompt = best->prompt;
    XX_LOGD(
        "[EvolutionTraining] Applied best prompt (id={}, score={:.4f}) to "
        "config",
        best->id,
        best->averageScore()
    );
}

} // namespace agent
} // namespace agentxx
