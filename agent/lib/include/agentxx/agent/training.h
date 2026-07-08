#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/deepagent.h"
#include "neograph/llm/openai_provider.h"
#include "neograph/neograph.h"
#include <agentxx/util/log.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>

namespace agentxx {
namespace agent {

// ======================== 数据结构 ========================

/// 单个训练测试用例
struct TrainingTestCase {
  std::string name;
  std::string input;
  std::string expectedOutput; // 描述预期的结果、评分标准等（供评分器参考）
  std::string equalOutput; // 若不为空，则判断 agent 输出是否与此完全相等
  neograph::json extra;
};

/// 从 JSON 文件中加载测试用例
inline std::vector<TrainingTestCase>
loadTestCasesFromFile(const std::string &filePath) {
  std::vector<TrainingTestCase> cases;
  try {
    std::ifstream ifs(filePath);
    if (!ifs.is_open()) {
      XX_LOGE("[Training] Failed to open test case file: {}", filePath);
      return cases;
    }
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    ifs.close();

    auto j = neograph::json::parse(content);
    if (!j.is_array()) {
      XX_LOGE("[Training] Test case file is not a JSON array: {}", filePath);
      return cases;
    }
    for (const auto &item : j) {
      TrainingTestCase tc;
      tc.name = item.value("name", "");
      tc.input = item.value("input", "");
      tc.expectedOutput = item.value("expectedOutput", "");
      tc.equalOutput = item.value("equalOutput", "");
      tc.extra = item.value("extra", neograph::json::object());
      cases.push_back(std::move(tc));
    }
    XX_LOGD("[Training] Loaded {} test cases from {}", cases.size(), filePath);
  } catch (const std::exception &e) {
    XX_LOGE("[Training] Failed to parse test case file {}: {}", filePath,
            e.what());
  }
  return cases;
}

/// 从目录中加载所有 JSON 测试用例文件
inline std::vector<TrainingTestCase>
loadTestCasesFromDirectory(const std::string &dirPath) {
  std::vector<TrainingTestCase> allCases;
  try {
    for (const auto &entry : std::filesystem::directory_iterator(dirPath)) {
      if (entry.is_regular_file() && entry.path().extension() == ".json") {
        auto fileCases = loadTestCasesFromFile(entry.path().string());
        allCases.insert(allCases.end(),
                        std::make_move_iterator(fileCases.begin()),
                        std::make_move_iterator(fileCases.end()));
      }
    }
    XX_LOGD("[Training] Loaded {} total test cases from directory {}",
            allCases.size(), dirPath);
  } catch (const std::exception &e) {
    XX_LOGE("[Training] Failed to load test cases from directory {}: {}",
            dirPath, e.what());
  }
  return allCases;
}

/// 剥离 LLM 响应中可能存在的 Markdown 代码块标记
inline std::string stripMarkdownCodeBlock(const std::string &content) {
  std::string result = content;
  // 去除首尾空白
  auto start = result.find_first_not_of(" \t\n\r");
  auto end = result.find_last_not_of(" \t\n\r");
  if (start == std::string::npos) {
    return result;
  }
  result = result.substr(start, end - start + 1);

  // 检查是否以 ```json 或 ``` 开头
  if (result.size() >= 3 && result.substr(0, 3) == "```") {
    auto newlinePos = result.find('\n');
    if (newlinePos != std::string::npos) {
      result = result.substr(newlinePos + 1);
    }
    // 去除结尾的 ```
    if (result.size() >= 3 && result.substr(result.size() - 3) == "```") {
      result = result.substr(0, result.size() - 3);
    }
    // 去除尾部空白
    start = result.find_last_not_of(" \t\n\r");
    if (start != std::string::npos) {
      result = result.substr(0, start + 1);
    }
  }
  return result;
}

/// 评分结果
struct TrainingScore {
  double score = 0.0;
  std::string feedback;
  bool passed = false;
  int iteration = 0;
  neograph::json extra;
};

/// Prompt 变体：存储一组 prompt 配置及其评分
struct PromptVariant {
  std::string id;
  std::string systemPrompt;
  std::string systemPlanningPrompt;
  std::string systemSkillPrompt;
  double cumulativeScore = 0.0;
  int testCount = 0;
  int generation = 0;
  std::string parentId;
  neograph::json extra;

  double averageScore() const {
    return testCount > 0 ? cumulativeScore / testCount : 0.0;
  }
};

// ======================== 回调类型 ========================

/// 自定义评分回调
using TrainingScoringFunc = std::function<asio::awaitable<TrainingScore>(
    const std::string &agentOutput, const TrainingTestCase &testCase,
    int iteration)>;

/// 迭代观察回调
using TrainingIterationCallback = std::function<void(
    const TrainingScore &score, const std::string &agentOutput)>;

// ======================== 进化训练配置 ========================

struct EvolutionTrainingConfig {
  /// 测试用例列表
  std::vector<TrainingTestCase> testCases;

  /// 保存/加载 prompt 变体的文件路径
  std::string saveFilePath = "./training_prompts.json";

  /// 保留的 top N 个 prompt 变体
  int topK = 100;

  /// 每轮从 top 中选取多少个进行变异生成新变体
  int mutateCount = 10;

  /// 每个选中的变体生成几个子代
  int childrenPerParent = 3;

  /// 评分 subagent 的 system prompt
  std::string scoringPrompt = R"(
You are an expert evaluator. Your task is to score the agent's response based on the test case.

Scoring criteria (0.0 to 1.0):
- 1.0: Perfect response, fully satisfies the test case requirements
- 0.8-0.9: Good response, minor issues
- 0.6-0.7: Acceptable response, some issues
- 0.4-0.5: Partially correct, significant issues
- 0.0-0.3: Poor or incorrect response

Output ONLY a JSON object with the following schema:
{
  "score": <0.0-1.0>,
  "feedback": "<detailed feedback explaining the score>",
  "passed": <true if score >= threshold, false otherwise>
}
)";

  /// 评分模型名称（为空则使用主 agent 的模型）
  std::string scoringModelName;

  /// 评分模型 API Key
  std::string scoringModelApiKey;

  /// 评分模型 Base URL
  std::string scoringModelBaseUrl;

  /// prompt 优化器（调整 prompt）的 system prompt
  std::string optimizerPrompt = R"(
You are a prompt engineering expert. Your task is to improve a system prompt for an AI agent.

Given:
1. The current system prompt that was used
2. The test case the agent was given
3. The agent's output
4. The score and feedback the agent received

Your job is to write an IMPROVED system prompt that will help the agent perform better on similar tasks.

Output ONLY a JSON object:
{
  "systemPrompt": "<the improved system prompt>",
  "analysis": "<brief analysis of what was wrong and how you fixed it>"
}
)";

  /// 优化器模型名称
  std::string optimizerModelName;

  /// 优化器 API Key
  std::string optimizerModelApiKey;

  /// 优化器 Base URL
  std::string optimizerModelBaseUrl;

  /// 收敛阈值（评分达到此值视为通过）
  double convergenceThreshold = 0.8;

  /// 连续 N 代最佳分数无提升则停止训练（0 表示不自动停止）
  int maxGenerationsWithoutImprovement = 5;

  /// 变异率：对 prompt 字符串中每个字符进行随机变异的概率（0.0-1.0）
  double mutationRate = 0.01;

  /// 自定义评分回调
  TrainingScoringFunc scoringFunc;

  /// 每轮迭代观察回调
  TrainingIterationCallback onIteration;

  /// 是否启用详细日志
  bool verbose = true;
};

// ======================== 进化训练 Agent ========================

class EvolutionTrainingAgent {
protected:
  std::shared_ptr<agentxx::agent::DeepAgent> scoreAgent;
  std::shared_ptr<agentxx::agent::DeepAgent> trainAgent;
  std::shared_ptr<agentxx::agent::DeepAgent> optimizerAgent;

  std::vector<PromptVariant> population;
  std::mt19937 rng;
  int generationCounter = 0;

  // ---- 文件 I/O ----

  neograph::json promptVariantToJson(const PromptVariant &v) const {
    return {
        {"id", v.id},
        {"systemPrompt", v.systemPrompt},
        {"systemPlanningPrompt", v.systemPlanningPrompt},
        {"systemSkillPrompt", v.systemSkillPrompt},
        {"cumulativeScore", v.cumulativeScore},
        {"testCount", v.testCount},
        {"generation", v.generation},
        {"parentId", v.parentId},
        {"extra", v.extra},
    };
  }

  PromptVariant promptVariantFromJson(const neograph::json &j) const {
    PromptVariant v;
    v.id = j.value("id", "");
    v.systemPrompt = j.value("systemPrompt", "");
    v.systemPlanningPrompt = j.value("systemPlanningPrompt", "");
    v.systemSkillPrompt = j.value("systemSkillPrompt", "");
    v.cumulativeScore = j.value("cumulativeScore", 0.0);
    v.testCount = j.value("testCount", 0);
    v.generation = j.value("generation", 0);
    v.parentId = j.value("parentId", "");
    v.extra = j.value("extra", neograph::json::object());
    return v;
  }

  void savePopulationToFile(const std::string &filePath) {
    try {
      neograph::json j = neograph::json::array();
      for (const auto &v : population) {
        j.push_back(promptVariantToJson(v));
      }

      neograph::json root;
      root["population"] = j;
      root["generationCounter"] = generationCounter;
      root["savedAt"] =
          std::chrono::system_clock::now().time_since_epoch().count();

      std::ofstream ofs(filePath, std::ios::out | std::ios::trunc);
      if (ofs.is_open()) {
        ofs << root.dump(2);
        ofs.close();
        XX_LOGD("[EvolutionTraining] Saved {} prompts to {}", population.size(),
                filePath);
      } else {
        XX_LOGE("[EvolutionTraining] Failed to open file for writing: {}",
                filePath);
      }
    } catch (const std::exception &e) {
      XX_LOGE("[EvolutionTraining] Failed to save population: {}", e.what());
    }
  }

  bool loadPopulationFromFile(const std::string &filePath) {
    try {
      std::ifstream ifs(filePath);
      if (!ifs.is_open()) {
        XX_LOGD("[EvolutionTraining] No existing save file found at {}, "
                "starting fresh",
                filePath);
        return false;
      }

      std::string content((std::istreambuf_iterator<char>(ifs)),
                          std::istreambuf_iterator<char>());
      ifs.close();

      if (content.empty()) {
        return false;
      }

      auto root = neograph::json::parse(content);
      if (root.contains("population") && root["population"].is_array()) {
        population.clear();
        for (const auto &j : root["population"]) {
          population.push_back(promptVariantFromJson(j));
        }
        generationCounter = root.value("generationCounter", 0);

        std::sort(population.begin(), population.end(),
                  [](const PromptVariant &a, const PromptVariant &b) {
                    return a.averageScore() > b.averageScore();
                  });

        XX_LOGD("[EvolutionTraining] Loaded {} prompts from {} (generation {})",
                population.size(), filePath, generationCounter);
        return true;
      }
    } catch (const std::exception &e) {
      XX_LOGE("[EvolutionTraining] Failed to load population: {}", e.what());
    }
    return false;
  }

  // ---- Agent 运行 ----

  asio::awaitable<std::string>
  runSingleInputAsync(const std::string &threadId, const std::string &userInput,
                      const std::string &systemPrompt) {
    co_return co_await trainAgent->runSingleInputAsync(threadId, userInput,
                                                       systemPrompt);
  }

  // ---- 评分 ----

  asio::awaitable<TrainingScore>
  defaultScoringWithSubAgent(std::string_view agentOutput,
                             const TrainingTestCase &testCase, int iteration,
                             const EvolutionTrainingConfig &cfg) {
    TrainingScore result;
    result.iteration = iteration;

    // 若设置了 equalOutput，则优先进行精确匹配判断
    if (!testCase.equalOutput.empty()) {
      if (agentOutput == testCase.equalOutput) {
        result.score = 1.0;
        result.feedback = "Output exactly matches equalOutput.";
        result.passed = true;
      } else {
        result.score = 0.0;
        result.feedback = "Output does not match equalOutput.";
        result.passed = false;
      }
      co_return result;
    }

    std::ostringstream scoringMessage;
    scoringMessage << "Test Case: " << testCase.name << "\n";
    scoringMessage << "User Input: " << testCase.input << "\n";
    if (!testCase.expectedOutput.empty()) {
      scoringMessage << "Expected Output / Scoring Criteria: "
                     << testCase.expectedOutput << "\n";
    }
    scoringMessage << "\nAgent Response:\n" << agentOutput << "\n";
    scoringMessage << "\nScore threshold (score >= " << cfg.convergenceThreshold
                   << " is passed)";

    neograph::ChatMessage scoringMsg{
        .role = "user",
        .content = scoringMessage.str(),
    };

    neograph::ChatMessage systemMsg{
        .role = "system",
        .content = cfg.scoringPrompt,
    };

    try {
      std::vector<neograph::ChatMessage> messages = {systemMsg, scoringMsg};
      auto response = co_await scoreAgent->runStreamAsync(messages);

      const auto &content = response.content;
      auto stripped = stripMarkdownCodeBlock(content);

      auto parsed = neograph::json::parse(stripped);
      if (parsed.is_object()) {
        result.score = parsed.value("score", 0.0);
        result.feedback = parsed.value("feedback", std::string{});
        result.passed = parsed.value("passed", false);
        if (parsed.contains("extra") && parsed["extra"].is_object()) {
          result.extra = parsed["extra"];
        }
      } else {
        result.score = 0.0;
        result.feedback = "Scorer returned non-JSON: " + content;
        result.passed = false;
      }
    } catch (const std::exception &e) {
      result.score = 0.0;
      result.feedback = std::string("Scoring subagent error: ") + e.what();
      result.passed = false;
    }

    co_return result;
  }

  // ---- Prompt 优化 ----

  asio::awaitable<std::string> optimizePromptWithLLM(
      const std::string &currentSystemPrompt, const TrainingTestCase &testCase,
      const std::string &agentOutput, const TrainingScore &score,
      const EvolutionTrainingConfig &cfg) {
    if (!optimizerAgent) {
      co_return currentSystemPrompt;
    }

    std::ostringstream optimizerMsg;
    optimizerMsg << "Current System Prompt:\n```\n"
                 << currentSystemPrompt << "\n```\n\n";
    optimizerMsg << "Test Case: " << testCase.name << "\n";
    optimizerMsg << "User Input: " << testCase.input << "\n";
    if (!testCase.expectedOutput.empty()) {
      optimizerMsg << "Expected Output / Scoring Criteria: "
                   << testCase.expectedOutput << "\n";
    }
    if (!testCase.equalOutput.empty()) {
      optimizerMsg << "Required Exact Output: " << testCase.equalOutput << "\n";
    }
    optimizerMsg << "\nAgent Output:\n```\n" << agentOutput << "\n```\n\n";
    optimizerMsg << "Score: " << score.score << "\n";
    optimizerMsg << "Feedback: " << score.feedback << "\n";
    optimizerMsg << "\nPlease provide an improved system prompt.";

    neograph::ChatMessage userMsg{
        .role = "user",
        .content = optimizerMsg.str(),
    };
    neograph::ChatMessage systemMsg{
        .role = "system",
        .content = cfg.optimizerPrompt,
    };

    try {
      std::vector<neograph::ChatMessage> messages = {systemMsg, userMsg};
      auto response = co_await optimizerAgent->runStreamAsync(messages);

      const auto &content = response.content;
      auto stripped = stripMarkdownCodeBlock(content);
      auto parsed = neograph::json::parse(stripped);
      if (parsed.is_object() && parsed.contains("systemPrompt") &&
          parsed["systemPrompt"].is_string()) {
        auto improved = parsed["systemPrompt"].get<std::string>();
        if (!improved.empty()) {
          XX_LOGD("[EvolutionTraining] Optimizer produced new prompt (len={})",
                  improved.size());
          co_return improved;
        }
      }

      XX_LOGD(
          "[EvolutionTraining] Optimizer returned non-JSON or empty prompt, "
          "keeping original");
      co_return currentSystemPrompt;
    } catch (const std::exception &e) {
      XX_LOGE("[EvolutionTraining] Optimizer error: {}", e.what());
      co_return currentSystemPrompt;
    }
  }

  // ---- 变异操作 ----

  std::string generateId() {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return fmt::format("gen{}_{}", generationCounter, now);
  }

  /// 对字符串进行随机变异：以 mutationRate 概率对每个字符进行插入/删除/替换
  std::string mutateString(const std::string &input, double mutationRate) {
    if (mutationRate <= 0.0 || input.empty()) {
      return input;
    }
    std::string result;
    result.reserve(input.size() * 2);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    // 可变异字符集
    static const char mutationChars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 "
        ".,;:!?-_()[]{}\n";
    static const int mutationCharsLen =
        static_cast<int>(sizeof(mutationChars) - 1);
    std::uniform_int_distribution<int> charDist(0, mutationCharsLen - 1);
    // 操作类型分布：0=替换, 1=插入, 2=删除
    std::uniform_int_distribution<int> opDist(0, 2);

    for (size_t i = 0; i < input.size(); ++i) {
      if (dist(rng) < mutationRate) {
        int op = opDist(rng);
        switch (op) {
        case 0:
          // 替换
          result.push_back(mutationChars[charDist(rng)]);
          break;
        case 1:
          // 插入一个随机字符，然后保留原字符
          result.push_back(mutationChars[charDist(rng)]);
          result.push_back(input[i]);
          break;
        case 2:
          // 删除（跳过当前字符）
          break;
        }
      } else {
        result.push_back(input[i]);
      }
    }
    // 如果全部被删除了，至少保留原字符串
    if (result.empty()) {
      result = input;
    }
    return result;
  }

  PromptVariant createChildVariant(const PromptVariant &parent,
                                   double mutationRate = 0.01) {
    PromptVariant child;
    child.id = generateId();
    child.systemPrompt = mutateString(parent.systemPrompt, mutationRate);
    child.systemPlanningPrompt =
        mutateString(parent.systemPlanningPrompt, mutationRate);
    child.systemSkillPrompt =
        mutateString(parent.systemSkillPrompt, mutationRate);
    child.generation = generationCounter;
    child.parentId = parent.id;
    child.cumulativeScore = 0.0;
    child.testCount = 0;
    return child;
  }

  // ---- 测试单个变体 ----

  asio::awaitable<double> evaluateVariant(PromptVariant &variant,
                                          const EvolutionTrainingConfig &cfg) {
    double totalScore = 0.0;
    int testCount = 0;

    for (size_t caseIdx = 0; caseIdx < cfg.testCases.size(); ++caseIdx) {
      const auto &testCase = cfg.testCases[caseIdx];
      const auto threadId =
          fmt::format("evotrain_{}_{}_{}", variant.id, caseIdx, testCase.name);

      if (cfg.verbose) {
        XX_LOGD(
            "[EvolutionTraining] [{}] Testing case '{}/{}' with variant '{}'",
            generationCounter, caseIdx + 1, cfg.testCases.size(), variant.id);
      }

      std::string agentOutput = co_await runSingleInputAsync(
          threadId, testCase.input, variant.systemPrompt);

      if (cfg.verbose) {
        XX_LOGD("[EvolutionTraining] [{}] Output (len={}): {}",
                generationCounter, agentOutput.size(),
                agentOutput.size() > 200 ? agentOutput.substr(0, 200) + "..."
                                         : agentOutput);
      }

      TrainingScore score;
      if (cfg.scoringFunc) {
        score = co_await cfg.scoringFunc(agentOutput, testCase, testCount);
      } else {
        score = co_await defaultScoringWithSubAgent(agentOutput, testCase,
                                                    testCount, cfg);
      }

      totalScore += score.score;
      testCount++;

      if (cfg.verbose) {
        XX_LOGD(
            "[EvolutionTraining] [{}] Score: {:.3f}, Passed: {}, Feedback: {}",
            generationCounter, score.score, score.passed, score.feedback);
      }

      if (cfg.onIteration) {
        cfg.onIteration(score, agentOutput);
      }

      // 如果评分较低，尝试用优化器改进 prompt
      if (score.score < cfg.convergenceThreshold && optimizerAgent) {
        std::string improvedPrompt = co_await optimizePromptWithLLM(
            variant.systemPrompt, testCase, agentOutput, score, cfg);

        if (improvedPrompt != variant.systemPrompt) {
          // 创建新变体用优化后的 prompt 再测一次
          PromptVariant improvedVariant = createChildVariant(variant);
          improvedVariant.systemPrompt = improvedPrompt;

          if (cfg.verbose) {
            XX_LOGD("[EvolutionTraining] [{}] Testing improved variant '{}'",
                    generationCounter, improvedVariant.id);
          }

          const auto improvedThreadId = fmt::format(
              "evotrain_improved_{}_{}", improvedVariant.id, caseIdx);

          std::string improvedOutput = co_await runSingleInputAsync(
              improvedThreadId, testCase.input, improvedVariant.systemPrompt);

          TrainingScore improvedScore;
          if (cfg.scoringFunc) {
            improvedScore =
                co_await cfg.scoringFunc(improvedOutput, testCase, testCount);
          } else {
            improvedScore = co_await defaultScoringWithSubAgent(
                improvedOutput, testCase, testCount, cfg);
          }

          if (improvedScore.score > score.score) {
            improvedVariant.cumulativeScore = improvedScore.score;
            improvedVariant.testCount = 1;

            if (cfg.verbose) {
              XX_LOGD("[EvolutionTraining] [{}] Improved variant '{}' score "
                      "{:.3f} > "
                      "original {:.3f}, keeping it",
                      generationCounter, improvedVariant.id,
                      improvedScore.score, score.score);
            }

            population.push_back(std::move(improvedVariant));
          }
        }
      }
    }

    variant.cumulativeScore += totalScore;
    variant.testCount += testCount;

    co_return variant.averageScore();
  }

public:
  EvolutionTrainingAgent(
      std::shared_ptr<agentxx::agent::DeepAgent> in_scoreAgent,
      std::shared_ptr<agentxx::agent::DeepAgent> in_trainAgent,
      std::shared_ptr<agentxx::agent::DeepAgent> in_optimizerAgent = nullptr)
      : scoreAgent(in_scoreAgent), trainAgent(in_trainAgent),
        optimizerAgent(in_optimizerAgent),
        rng(std::chrono::system_clock::now().time_since_epoch().count()) {}

  // ---- 初始化种子 prompt ----

  void seedInitialPopulation(const std::string &baseSystemPrompt) {
    if (!population.empty()) {
      return;
    }

    PromptVariant seed;
    seed.id = generateId();
    seed.systemPrompt = baseSystemPrompt;
    seed.generation = 0;
    seed.parentId = "seed";
    seed.cumulativeScore = 0.0;
    seed.testCount = 0;
    population.push_back(std::move(seed));

    XX_LOGD("[EvolutionTraining] Seeded initial population with 1 prompt");
  }

  // ---- 进化训练主循环 ----

  asio::awaitable<void> runEvolutionLoop(const EvolutionTrainingConfig &cfg) {
    // Step 1: 尝试从文件加载已有 population
    bool loaded = loadPopulationFromFile(cfg.saveFilePath);

    if (!loaded && population.empty()) {
      XX_LOGE("[EvolutionTraining] No population loaded and no seed provided. "
              "Call seedInitialPopulation() first or provide a save file.");
      co_return;
    }

    if (!loaded && cfg.verbose) {
      XX_LOGD("[EvolutionTraining] Starting fresh with {} seed prompts",
              population.size());
    }

    // 确保 population 按评分排序
    std::sort(population.begin(), population.end(),
              [](const PromptVariant &a, const PromptVariant &b) {
                return a.averageScore() > b.averageScore();
              });

    // Step 2: 进化训练主循环
    double bestScoreEver = 0.0;
    int generationsWithoutImprovement = 0;

    while (true) {
      generationCounter++;

      if (cfg.verbose) {
        XX_LOGD(
            "[EvolutionTraining] ====== Generation {} | Population: {} ======",
            generationCounter, population.size());
      }

      // 2a. 从当前 population 中选取 top 变体进行变异
      int mutateFrom = std::min(cfg.mutateCount, (int)population.size());

      std::vector<PromptVariant> newGeneration;

      for (int i = 0; i < mutateFrom; ++i) {
        const auto &parent = population[i];

        for (int c = 0; c < cfg.childrenPerParent; ++c) {
          PromptVariant child = createChildVariant(parent, cfg.mutationRate);
          newGeneration.push_back(std::move(child));
        }
      }

      if (cfg.verbose) {
        XX_LOGD("[EvolutionTraining] [{}] Created {} new variants from top {} "
                "parents (mutationRate={})",
                generationCounter, newGeneration.size(), mutateFrom,
                cfg.mutationRate);
      }

      // 2b. 测试所有新变体
      for (auto &variant : newGeneration) {
        co_await evaluateVariant(variant, cfg);
      }

      // 2c. 合并新旧 population
      population.insert(population.end(),
                        std::make_move_iterator(newGeneration.begin()),
                        std::make_move_iterator(newGeneration.end()));

      // 2d. 排序并保留 top K
      std::sort(population.begin(), population.end(),
                [](const PromptVariant &a, const PromptVariant &b) {
                  return a.averageScore() > b.averageScore();
                });

      if ((int)population.size() > cfg.topK) {
        if (cfg.verbose) {
          XX_LOGD("[EvolutionTraining] [{}] Trimming population from {} to {}",
                  generationCounter, population.size(), cfg.topK);
        }
        population.resize(cfg.topK);
      }

      // 2e. 检查收敛
      double currentBestScore =
          population.empty() ? 0.0 : population[0].averageScore();
      if (currentBestScore > bestScoreEver) {
        bestScoreEver = currentBestScore;
        generationsWithoutImprovement = 0;
      } else {
        generationsWithoutImprovement++;
      }

      // 2f. 打印 top 信息
      if (cfg.verbose && !population.empty()) {
        XX_LOGD("[EvolutionTraining] [{}] Top 5 prompts:", generationCounter);
        for (size_t i = 0; i < std::min(population.size(), (size_t)5); ++i) {
          const auto &v = population[i];
          XX_LOGD("  [{}/{}] id={} avgScore={:.4f} tests={} gen={} parent={}",
                  i + 1, population.size(), v.id, v.averageScore(), v.testCount,
                  v.generation, v.parentId);
        }

        const auto &best = population[0];
        XX_LOGD("[EvolutionTraining] [{}] Best prompt (score={:.4f}):\n{}",
                generationCounter, best.averageScore(),
                best.systemPrompt.size() > 300
                    ? best.systemPrompt.substr(0, 300) + "..."
                    : best.systemPrompt);
      }

      // 2g. 收敛检查
      if (cfg.maxGenerationsWithoutImprovement > 0 &&
          generationsWithoutImprovement >=
              cfg.maxGenerationsWithoutImprovement) {
        XX_LOGD("[EvolutionTraining] Converged! Best score {:.4f} unchanged "
                "for {} generations. Stopping training.",
                bestScoreEver, generationsWithoutImprovement);
        break;
      }

      // 2h. 保存到文件
      savePopulationToFile(cfg.saveFilePath);
    }

    co_return;
  }

  // ---- 获取当前 population ----

  const std::vector<PromptVariant> &getPopulation() const { return population; }

  const PromptVariant *getBestPrompt() const {
    if (population.empty()) {
      return nullptr;
    }
    return &population[0];
  }

  // ---- 将最优 prompt 应用到 AgentConfig ----

  void applyBestPromptToConfig(std::shared_ptr<AgentConfig> config) {
    const auto *best = getBestPrompt();
    if (!best) {
      return;
    }

    if (!best->systemPrompt.empty()) {
      config->prompt.systemPrompt = best->systemPrompt;
    }
    if (!best->systemPlanningPrompt.empty()) {
      config->prompt.systemPlanningPrompt = best->systemPlanningPrompt;
    }
    if (!best->systemSkillPrompt.empty()) {
      config->prompt.systemSkillPrompt = best->systemSkillPrompt;
    }

    XX_LOGD("[EvolutionTraining] Applied best prompt (id={}, score={:.4f}) to "
            "config",
            best->id, best->averageScore());
  }
};

} // namespace agent
} // namespace agentxx