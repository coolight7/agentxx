#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/config_static.h"
#include "agentxx/agent/deepagent.h"
#include "neograph/json.h"
#include "neograph/llm/openai_provider.h"
#include "neograph/types.h"
#include <agentxx/util/log.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
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
  auto start = result.find_first_not_of(" \t\n\r");
  auto end = result.find_last_not_of(" \t\n\r");
  if (start == std::string::npos) {
    return result;
  }
  result = result.substr(start, end - start + 1);

  if (result.size() >= 3 && result.substr(0, 3) == "```") {
    auto newlinePos = result.find('\n');
    if (newlinePos != std::string::npos) {
      result = result.substr(newlinePos + 1);
    }
    if (result.size() >= 3 && result.substr(result.size() - 3) == "```") {
      result = result.substr(0, result.size() - 3);
    }
    start = result.find_last_not_of(" \t\n\r");
    if (start != std::string::npos) {
      result = result.substr(0, start + 1);
    }
  }
  return result;
}

/// 从 LLM 响应中解析 JSON：先剥离 markdown 代码块，失败则尝试提取首个 {...}
/// 子串
inline neograph::json parseJsonFromResponse(const std::string &content) {
  auto stripped = stripMarkdownCodeBlock(content);
  try {
    return neograph::json::parse(stripped);
  } catch (...) {
    auto first = stripped.find('{');
    auto last = stripped.rfind('}');
    if (first != std::string::npos && last != std::string::npos &&
        last > first) {
      return neograph::json::parse(stripped.substr(first, last - first + 1));
    }
    throw;
  }
}

/// 评分结果
struct TrainingScore {
  double score = 0.0;
  std::string feedback;
  bool passed = false;
  int iteration = 0;
  neograph::json extra;
};

/// 优化器/变异器输出的 prompt 修改 patch
/// - patch 是一个 JSON，结构同 AgentPrompt::toJson，仅包含要修改的字段
/// - 空 patch 表示无修改
struct OptimizedPrompts {
  neograph::json patch;
  std::string analysis;
};

/// Prompt 变体：存储完整 AgentPrompt 及其评分
/// 训练目标是 AgentPrompt 类内定义的全部提示词（含 toolPrompt）
struct PromptVariant {
  std::string id;
  AgentPrompt prompt;
  double cumulativeScore = 0.0;
  int testCount = 0;
  int generation = 0;
  std::string parentId;
  std::map<std::string, double> perTestCaseScores;
  neograph::json extra;

  double averageScore() const {
    return testCount > 0 ? cumulativeScore / testCount : 0.0;
  }

  /// 计算整个 prompt 的 hash，用于去重
  size_t promptHash() const { return prompt.promptHash(); }
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
  std::string saveFilePath = agentxx::agent::AgentConfigStatic::getResultPath(
      "/train/training_prompts.json");

  /// 保留的 top N 个 prompt 变体
  int topK = 100;

  /// 每轮从 top 中选取多少个进行变异生成新变体
  int mutateCount = 10;

  /// 每个选中的变体生成几个子代
  int childrenPerParent = 3;

  /// 最大迭代代数（0 表示不限制，仅靠收敛检测停止）
  int maxGenerations = 50;

  /// 是否使用 LLM 进行语义变异；false 则使用字符级随机变异
  bool useLLMMutation = true;

  /// 每代最多尝试多少次 prompt 优化（针对低分变体）
  int maxOptimizationsPerGen = 5;

  /// 早终：已测试用例数达到 [earlyTerminationCheckAfter] 后，
  /// 若平均分低于 [earlyTerminationScore] 则跳过剩余用例（剩余记 0 分）
  int earlyTerminationCheckAfter = 2;
  double earlyTerminationScore = 0.2;

  /// 保存文件时保留的历史备份数（0 表示不备份）
  int saveFileBackupCount = 3;

  /// 评分 subagent 的 system prompt
  std::string scoringPrompt = R"(
You are an expert evaluator for an AI agent. Score the agent's response against the test case criteria.

## Scoring Rubric (0.0 to 1.0)
- 1.0: Excellent — fully satisfies all requirements, no issues
- 0.8-0.9: Good — meets requirements with minor issues
- 0.6-0.7: Acceptable — mostly correct, some gaps
- 0.4-0.5: Weak — partially correct, significant issues
- 0.2-0.3: Poor — mostly incorrect or missing
- 0.0-0.1: Fail — incorrect, irrelevant, or no response

## Evaluation Dimensions
1. Correctness: Is the answer factually/technically correct?
2. Completeness: Does it address all parts of the request?
3. Clarity: Is the response clear and well-structured?
4. Format: Does it match any requested format (exact output, language, etc.)?

## Output
Output ONLY a JSON object (no markdown fences, no prose outside JSON):
{
  "score": <number 0.0-1.0>,
  "feedback": "<concise: what was good, what was missing, how to improve>",
  "passed": <true if score >= threshold given below, else false>,
  "dimensions": {
    "correctness": <0.0-1.0>,
    "completeness": <0.0-1.0>,
    "clarity": <0.0-1.0>,
    "format": <0.0-1.0>
  }
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
You are a prompt engineering expert. Improve the prompts for an AI agent based on observed performance.

## Inputs
You will receive:
1. The current prompts (system, planning, skill, and tool prompts) used by the agent
2. The test case and expected behavior
3. The agent's actual output
4. The score and feedback from evaluation

## Your Task
Write IMPROVED prompts that will help the agent perform better on similar tasks.

## Prompt Engineering Principles
- Be specific and direct about expected behavior
- Use structured sections (##) for clarity
- Prefer positive guidance (what to do) over prohibitions (what not to do)
- Add concrete examples where helpful
- Keep prompts concise — avoid redundancy
- Preserve working parts of the prompt; focus changes on weak areas

## Output
Output ONLY a JSON object (no markdown fences):
{
  "systemPrompt": "<improved main system prompt, or empty to keep current>",
  "systemPlanningPrompt": "<improved planning prompt, or empty to keep current>",
  "systemSkillPrompt": "<improved skill prompt, or empty to keep current>",
  "toolPrompt": {
    "<tool_name>": {
      "depict": "<improved tool description, or empty to keep>",
      "args": {
        "<arg_name>": "<improved arg description, or empty to keep>"
      }
    }
  },
  "analysis": "<what was wrong and how you fixed it>"
}

Leave any field empty ("") to keep it unchanged. Only modify prompts that need improvement.
Include the "toolPrompt" object only if you want to modify tool prompts.
)";

  /// 优化器模型名称
  std::string optimizerModelName;

  /// 优化器 API Key
  std::string optimizerModelApiKey;

  /// 优化器 Base URL
  std::string optimizerModelBaseUrl;

  /// LLM 变异 prompt：用于生成多样化的 prompt 变体（探索而非改进）
  std::string mutationPrompt = R"(
You are a prompt variation generator. Create a DIVERSE variation of the given AI agent prompts that explores different phrasings while preserving the core intent.

## Goal
Generate a meaningfully different version to explore the prompt space. The variation should:
- Keep the core instructions and intent
- Vary wording, structure, emphasis, or ordering
- Potentially add helpful guidance or examples
- NOT just paraphrase — make substantive structural changes

## Variation Strategies (use one or more)
- Reorganize sections for better logical flow
- Add concrete examples or analogies
- Change tone (formal / concise / explicit)
- Emphasize different aspects of the task
- Simplify verbose parts or expand terse parts

## Output
Output ONLY a JSON object (no markdown fences):
{
  "systemPrompt": "<varied main system prompt, or empty to keep current>",
  "systemPlanningPrompt": "<varied planning prompt, or empty to keep current>",
  "systemSkillPrompt": "<varied skill prompt, or empty to keep current>",
  "toolPrompt": {
    "<tool_name>": {
      "depict": "<varied tool description, or empty to keep>",
      "args": {
        "<arg_name>": "<varied arg description, or empty to keep>"
      }
    }
  },
  "strategy": "<which variation strategy you used>"
}

Leave any field empty ("") to keep it unchanged.
Include the "toolPrompt" object only if you want to vary tool prompts.
)";

  /// 收敛阈值（评分达到此值视为通过）
  double convergenceThreshold = 0.8;

  /// 连续 N 代最佳分数无提升则停止训练（0 表示不自动停止）
  int maxGenerationsWithoutImprovement = 5;

  /// 字符级变异率（仅当 useLLMMutation=false 时生效）
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

  // ---- 通用 LLM 调用 ----

  /// 设置 agent 的 system prompt 后发起一次非流式对话
  /// 注意: ModelCallWrapNode 会用 agentConfig->prompt.systemPrompt 覆盖
  /// 输入中的 system 消息，因此必须写入 config 而非通过消息传入
  asio::awaitable<std::string>
  runLLMAgent(std::shared_ptr<agentxx::agent::DeepAgent> agent,
              const std::string &systemPrompt, const std::string &userContent) {
    agent->getContext()->agentConfig->prompt.systemPrompt = systemPrompt;
    std::vector<neograph::ChatMessage> messages = {
        neograph::ChatMessage{.role = "user", .content = userContent},
    };
    auto result = co_await agent->runStreamAsync(messages);
    co_return result.content;
  }

  /// 将变体的完整 prompt 写入 trainAgent 的运行时配置
  /// 这是让变体真正生效的关键：ModelCallWrapNode / 各 middleware / 各 tool
  /// 均从 agentConfig->prompt 读取所有提示词
  void applyVariantToTrainAgent(const PromptVariant &variant) {
    auto cfg = trainAgent->getContext()->agentConfig;
    cfg->prompt = variant.prompt;
  }

  // ---- 文件 I/O ----

  neograph::json promptVariantToJson(const PromptVariant &v) const {
    neograph::json j;
    j["id"] = v.id;
    j["prompt"] = v.prompt.toJson();
    j["cumulativeScore"] = v.cumulativeScore;
    j["testCount"] = v.testCount;
    j["generation"] = v.generation;
    j["parentId"] = v.parentId;
    {
      neograph::json scores = neograph::json::object();
      for (const auto &kv : v.perTestCaseScores) {
        scores[kv.first] = kv.second;
      }
      j["perTestCaseScores"] = scores;
    }
    j["extra"] = v.extra;
    return j;
  }

  PromptVariant promptVariantFromJson(const neograph::json &j) const {
    PromptVariant v;
    v.id = j.value("id", std::string{});
    if (j.contains("prompt") && j["prompt"].is_object()) {
      v.prompt.mergeFromJson(j["prompt"]);
    } else {
      // 兼容旧版只存了 3 个 system prompt 字段的格式
      v.prompt.mergeFromJson(j);
    }
    v.cumulativeScore = j.value("cumulativeScore", 0.0);
    v.testCount = j.value("testCount", 0);
    v.generation = j.value("generation", 0);
    v.parentId = j.value("parentId", std::string{});
    if (j.contains("perTestCaseScores") && j["perTestCaseScores"].is_object()) {
      auto scores = j["perTestCaseScores"];
      for (const auto &item : scores.items()) {
        v.perTestCaseScores[item.first] = item.second.get<double>();
      }
    }
    v.extra = j.value("extra", neograph::json::object());
    return v;
  }

  /// 轮转备份保存文件：file -> file.1 -> file.2 -> ... -> file.N
  void rotateSaveFile(const std::string &path, int keepCount) {
    if (keepCount <= 0) {
      return;
    }
    namespace fs = std::filesystem;
    try {
      fs::path oldest(path + "." + std::to_string(keepCount));
      if (fs::exists(oldest)) {
        fs::remove(oldest);
      }
      for (int i = keepCount - 1; i >= 1; --i) {
        fs::path from(path + "." + std::to_string(i));
        fs::path to(path + "." + std::to_string(i + 1));
        if (fs::exists(from)) {
          fs::rename(from, to);
        }
      }
      fs::path cur(path);
      if (fs::exists(cur)) {
        fs::copy_file(cur, path + ".1", fs::copy_options::overwrite_existing);
      }
    } catch (const std::exception &e) {
      XX_LOGD("[EvolutionTraining] Backup rotation skipped: {}", e.what());
    }
  }

  void savePopulationToFile(const std::string &filePath, int backupCount = 0) {
    try {
      if (backupCount > 0) {
        rotateSaveFile(filePath, backupCount);
      }

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
    scoringMessage << "\nPass threshold: score >= " << cfg.convergenceThreshold
                   << "\n";
    if (testCase.extra.contains("language")) {
      scoringMessage << "Required language: "
                     << testCase.extra["language"].get<std::string>() << "\n";
    }

    try {
      auto content = co_await runLLMAgent(scoreAgent, cfg.scoringPrompt,
                                          scoringMessage.str());
      auto parsed = parseJsonFromResponse(content);
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

  /// 构建用于优化器/变异器的上下文消息：包含当前完整 prompt
  std::string buildPromptContextMessage(const PromptVariant &variant) const {
    std::ostringstream msg;
    msg << "Current System Prompt:\n```\n"
        << variant.prompt.systemPrompt << "\n```\n\n";
    if (!variant.prompt.systemPlanningPrompt.empty()) {
      msg << "Current Planning Prompt:\n```\n"
          << variant.prompt.systemPlanningPrompt << "\n```\n\n";
    }
    if (!variant.prompt.systemSkillPrompt.empty()) {
      msg << "Current Skill Prompt:\n```\n"
          << variant.prompt.systemSkillPrompt << "\n```\n\n";
    }
    if (!variant.prompt.toolPrompt.empty()) {
      msg << "Current Tool Prompts:\n";
      for (const auto &kv : variant.prompt.toolPrompt) {
        msg << "### " << kv.first << "\n";
        msg << "  depict: " << kv.second.depict << "\n";
        for (const auto &a : kv.second.args) {
          msg << "  arg[" << a.first << "]: " << a.second << "\n";
        }
        msg << "\n";
      }
    }
    return msg.str();
  }

  asio::awaitable<OptimizedPrompts> optimizeVariantWithLLM(
      const PromptVariant &variant, const TrainingTestCase &testCase,
      const std::string &agentOutput, const TrainingScore &score,
      const EvolutionTrainingConfig &cfg) {
    OptimizedPrompts result;
    if (!optimizerAgent) {
      co_return result;
    }

    std::ostringstream msg;
    msg << buildPromptContextMessage(variant);
    msg << "Test Case: " << testCase.name << "\n";
    msg << "User Input: " << testCase.input << "\n";
    if (!testCase.expectedOutput.empty()) {
      msg << "Expected Output / Scoring Criteria: " << testCase.expectedOutput
          << "\n";
    }
    if (!testCase.equalOutput.empty()) {
      msg << "Required Exact Output: " << testCase.equalOutput << "\n";
    }
    msg << "\nAgent Output:\n```\n" << agentOutput << "\n```\n\n";
    msg << "Score: " << score.score << "\n";
    msg << "Feedback: " << score.feedback << "\n";
    msg << "\nPlease provide improved prompts.";

    try {
      auto content =
          co_await runLLMAgent(optimizerAgent, cfg.optimizerPrompt, msg.str());
      auto parsed = parseJsonFromResponse(content);
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
        result.patch = patch;
        result.analysis = parsed.value("analysis", std::string{});
        if (!patch.empty()) {
          XX_LOGD("[EvolutionTraining] Optimizer produced prompt patch");
        }
      }
    } catch (const std::exception &e) {
      XX_LOGE("[EvolutionTraining] Optimizer error: {}", e.what());
    }
    co_return result;
  }

  // ---- 变异操作 ----

  std::string generateId() {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return fmt::format("gen{}_{}", generationCounter, now);
  }

  /// 对字符串进行随机变异：以 mutationRate 概率对每个字符进行插入/删除/替换
  /// 注意: 字符级变异会破坏 prompt 语义，仅作为 LLM 变异不可用时的降级手段
  std::string mutateString(const std::string &input, double mutationRate) {
    if (mutationRate <= 0.0 || input.empty()) {
      return input;
    }
    std::string result;
    result.reserve(input.size() * 2);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    static const char mutationChars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 "
        ".,;:!?-_()[]{}\n";
    static const int mutationCharsLen =
        static_cast<int>(sizeof(mutationChars) - 1);
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

  PromptVariant createChildVariantCharMut(const PromptVariant &parent,
                                          double mutationRate) {
    PromptVariant child;
    child.id = generateId();
    child.prompt = parent.prompt;
    // 字符级变异仅作用于 3 个 system prompt，toolPrompt 保持不变
    // （字符级变异会破坏语义，仅作为 LLM 变异不可用时的降级手段）
    child.prompt.systemPrompt =
        mutateString(parent.prompt.systemPrompt, mutationRate);
    child.prompt.systemPlanningPrompt =
        mutateString(parent.prompt.systemPlanningPrompt, mutationRate);
    child.prompt.systemSkillPrompt =
        mutateString(parent.prompt.systemSkillPrompt, mutationRate);
    child.generation = generationCounter;
    child.parentId = parent.id;
    return child;
  }

  /// 使用 LLM 对 prompt 进行语义级变异，生成多样化的探索变体
  asio::awaitable<PromptVariant>
  createChildVariantLLMMut(const PromptVariant &parent,
                           const EvolutionTrainingConfig &cfg) {
    PromptVariant child;
    child.id = generateId();
    child.prompt = parent.prompt;
    child.generation = generationCounter;
    child.parentId = parent.id;

    std::ostringstream msg;
    msg << buildPromptContextMessage(parent);
    msg << "\nGenerate a diverse variation of these prompts.";

    try {
      auto content =
          co_await runLLMAgent(optimizerAgent, cfg.mutationPrompt, msg.str());
      auto parsed = parseJsonFromResponse(content);
      if (parsed.is_object()) {
        // 以 patch 方式合并：只覆盖出现的字段
        child.prompt.mergeFromJson(parsed);
      } else {
        child = createChildVariantCharMut(parent, cfg.mutationRate);
      }
    } catch (const std::exception &e) {
      XX_LOGD("[EvolutionTraining] LLM mutation failed, fallback to char mut: "
              "{}",
              e.what());
      child = createChildVariantCharMut(parent, cfg.mutationRate);
    }
    co_return child;
  }

  // ---- 评估单个变体 ----

  /// 评估结果附带最差用例信息，供后续优化使用
  struct EvaluationResult {
    const TrainingTestCase *worstCase = nullptr;
    std::string worstCaseOutput;
    TrainingScore worstCaseScore;
  };

  asio::awaitable<EvaluationResult>
  evaluateVariant(PromptVariant &variant, const EvolutionTrainingConfig &cfg) {
    EvaluationResult evResult;
    variant.perTestCaseScores.clear();
    double totalScore = 0.0;
    int testCount = 0;
    bool earlyTerminated = false;

    for (size_t caseIdx = 0; caseIdx < cfg.testCases.size(); ++caseIdx) {
      const auto &testCase = cfg.testCases[caseIdx];
      const auto threadId =
          fmt::format("evotrain_{}_{}_{}", variant.id, caseIdx, testCase.name);

      applyVariantToTrainAgent(variant);

      if (cfg.verbose) {
        XX_LOGD(
            "[EvolutionTraining] [{}] Testing case '{}/{}' with variant '{}'",
            generationCounter, caseIdx + 1, cfg.testCases.size(), variant.id);
      }

      std::string agentOutput = co_await trainAgent->runSingleInputAsync(
          threadId, testCase.input, "");

      if (cfg.verbose) {
        XX_LOGD("[EvolutionTraining] [{}] Output (len={}): {}",
                generationCounter, agentOutput.size(),
                agentOutput.size() > 200 ? agentOutput.substr(0, 200) + "..."
                                         : agentOutput);
      }

      TrainingScore score;
      if (cfg.scoringFunc) {
        score =
            co_await cfg.scoringFunc(agentOutput, testCase, generationCounter);
      } else {
        score = co_await defaultScoringWithSubAgent(agentOutput, testCase,
                                                    generationCounter, cfg);
      }

      variant.perTestCaseScores[testCase.name] = score.score;
      totalScore += score.score;
      testCount++;

      if (evResult.worstCase == nullptr ||
          score.score < evResult.worstCaseScore.score) {
        evResult.worstCase = &testCase;
        evResult.worstCaseOutput = agentOutput;
        evResult.worstCaseScore = score;
      }

      if (cfg.verbose) {
        XX_LOGD(
            "[EvolutionTraining] [{}] Score: {:.3f}, Passed: {}, Feedback: {}",
            generationCounter, score.score, score.passed, score.feedback);
      }

      if (cfg.onIteration) {
        cfg.onIteration(score, agentOutput);
      }

      // 早终检查
      if (cfg.earlyTerminationCheckAfter > 0 &&
          testCount >= cfg.earlyTerminationCheckAfter) {
        double avg = totalScore / testCount;
        if (avg < cfg.earlyTerminationScore) {
          if (cfg.verbose) {
            XX_LOGD("[EvolutionTraining] [{}] Early-terminating variant '{}' "
                    "avg={:.3f} < {:.3f}",
                    generationCounter, variant.id, avg,
                    cfg.earlyTerminationScore);
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
    variant.testCount = static_cast<int>(cfg.testCases.size());

    if (cfg.verbose) {
      XX_LOGD("[EvolutionTraining] [{}] Variant '{}' avgScore={:.4f}{}",
              generationCounter, variant.id, variant.averageScore(),
              earlyTerminated ? " (early-terminated)" : "");
    }
    co_return evResult;
  }

  // ---- 去重 ----

  void deduplicatePopulation() {
    std::set<size_t> seen;
    std::vector<PromptVariant> unique;
    unique.reserve(population.size());
    for (auto &v : population) {
      auto h = v.promptHash();
      if (seen.insert(h).second) {
        unique.push_back(std::move(v));
      }
    }
    if (unique.size() != population.size()) {
      XX_LOGD("[EvolutionTraining] Deduplicated population: {} -> {}",
              population.size(), unique.size());
    }
    population = std::move(unique);
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
    if (trainAgent) {
      // 从 trainAgent 当前配置复制完整 prompt，再覆盖 systemPrompt
      seed.prompt = trainAgent->getContext()->agentConfig->prompt;
    }
    seed.prompt.systemPrompt = baseSystemPrompt;
    seed.generation = 0;
    seed.parentId = "seed";
    population.push_back(std::move(seed));

    XX_LOGD("[EvolutionTraining] Seeded initial population with 1 prompt");
  }

  /// 从完整 AgentPrompt 初始化种子（包含 planning/skill/tool prompts）
  void seedInitialPopulation(const AgentPrompt &prompt) {
    if (!population.empty()) {
      return;
    }

    PromptVariant seed;
    seed.id = generateId();
    seed.prompt = prompt;
    seed.generation = 0;
    seed.parentId = "seed";
    population.push_back(std::move(seed));

    XX_LOGD("[EvolutionTraining] Seeded initial population from AgentPrompt");
  }

  /// 从 trainAgent 当前完整 AgentPrompt 初始化种子
  void seedInitialPopulationFromAgent() {
    if (!population.empty() || !trainAgent) {
      return;
    }
    seedInitialPopulation(trainAgent->getContext()->agentConfig->prompt);
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

    deduplicatePopulation();
    std::sort(population.begin(), population.end(),
              [](const PromptVariant &a, const PromptVariant &b) {
                return a.averageScore() > b.averageScore();
              });

    double bestScoreEver =
        population.empty() ? 0.0 : population[0].averageScore();
    int generationsWithoutImprovement = 0;

    while (true) {
      generationCounter++;

      if (cfg.verbose) {
        XX_LOGD(
            "[EvolutionTraining] ====== Generation {} | Population: {} ======",
            generationCounter, population.size());
      }

      // 2a. 从当前 population 中选取 top 变体进行变异
      int mutateFrom =
          std::min(cfg.mutateCount, static_cast<int>(population.size()));

      std::vector<PromptVariant> newGeneration;

      for (int i = 0; i < mutateFrom; ++i) {
        const auto &parent = population[i];

        for (int c = 0; c < cfg.childrenPerParent; ++c) {
          if (cfg.useLLMMutation && optimizerAgent) {
            newGeneration.push_back(
                co_await createChildVariantLLMMut(parent, cfg));
          } else {
            newGeneration.push_back(
                createChildVariantCharMut(parent, cfg.mutationRate));
          }
        }
      }

      if (cfg.verbose) {
        XX_LOGD("[EvolutionTraining] [{}] Created {} new variants from top {} "
                "parents (mutation={})",
                generationCounter, newGeneration.size(), mutateFrom,
                cfg.useLLMMutation ? "LLM" : "char");
      }

      // 2b. 测试所有新变体，并收集最差用例信息
      std::vector<EvaluationResult> evalResults;
      evalResults.reserve(newGeneration.size());
      for (auto &variant : newGeneration) {
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
        std::sort(
            candidates.begin(), candidates.end(),
            [](const Cand &a, const Cand &b) { return a.score < b.score; });

        int optCount = std::min(cfg.maxOptimizationsPerGen,
                                static_cast<int>(candidates.size()));
        for (int i = 0; i < optCount; ++i) {
          auto &variant = newGeneration[candidates[i].index];
          const auto &ev = evalResults[candidates[i].index];
          if (!ev.worstCase) {
            continue;
          }

          if (cfg.verbose) {
            XX_LOGD("[EvolutionTraining] [{}] Optimizing variant '{}' on "
                    "worst case '{}'",
                    generationCounter, variant.id, ev.worstCase->name);
          }

          auto optimized = co_await optimizeVariantWithLLM(
              variant, *ev.worstCase, ev.worstCaseOutput, ev.worstCaseScore,
              cfg);

          // patch 为空对象表示无修改
          bool hasChange =
              optimized.patch.is_object() && !optimized.patch.empty();
          if (!hasChange) {
            continue;
          }

          PromptVariant improved = variant;
          improved.id = generateId();
          improved.parentId = variant.id;
          improved.generation = generationCounter;
          improved.cumulativeScore = 0.0;
          improved.testCount = 0;
          improved.perTestCaseScores.clear();
          // 以 patch 方式合并优化结果
          improved.prompt.mergeFromJson(optimized.patch);
          optimizedVariants.push_back(std::move(improved));
        }
      }

      // 2d. 测试优化后的变体（在全部用例上公平评估）
      for (auto &variant : optimizedVariants) {
        if (cfg.verbose) {
          XX_LOGD("[EvolutionTraining] [{}] Evaluating optimized variant '{}'",
                  generationCounter, variant.id);
        }
        co_await evaluateVariant(variant, cfg);
      }

      // 2e. 合并新旧 population
      population.insert(population.end(),
                        std::make_move_iterator(newGeneration.begin()),
                        std::make_move_iterator(newGeneration.end()));
      population.insert(population.end(),
                        std::make_move_iterator(optimizedVariants.begin()),
                        std::make_move_iterator(optimizedVariants.end()));

      // 2f. 去重、排序并保留 top K
      deduplicatePopulation();
      std::sort(population.begin(), population.end(),
                [](const PromptVariant &a, const PromptVariant &b) {
                  return a.averageScore() > b.averageScore();
                });

      if (population.size() > static_cast<size_t>(cfg.topK)) {
        if (cfg.verbose) {
          XX_LOGD("[EvolutionTraining] [{}] Trimming population from {} to {}",
                  generationCounter, population.size(), cfg.topK);
        }
        population.resize(cfg.topK);
      }

      // 2g. 检查收敛
      double currentBestScore =
          population.empty() ? 0.0 : population[0].averageScore();
      if (currentBestScore > bestScoreEver) {
        bestScoreEver = currentBestScore;
        generationsWithoutImprovement = 0;
      } else {
        generationsWithoutImprovement++;
      }

      // 2h. 打印 top 信息
      if (cfg.verbose && !population.empty()) {
        XX_LOGD("[EvolutionTraining] [{}] Top 5 prompts:", generationCounter);
        for (size_t i = 0;
             i < std::min(population.size(), static_cast<size_t>(5)); ++i) {
          const auto &v = population[i];
          XX_LOGD("  [{}/{}] id={} avgScore={:.4f} tests={} gen={} parent={}",
                  i + 1, population.size(), v.id, v.averageScore(), v.testCount,
                  v.generation, v.parentId);
        }

        const auto &best = population[0];
        const auto &sp = best.prompt.systemPrompt;
        XX_LOGD("[EvolutionTraining] [{}] Best prompt (score={:.4f}):\n{}",
                generationCounter, best.averageScore(),
                sp.size() > 300 ? sp.substr(0, 300) + "..." : sp);
      }

      // 2i. 收敛检查
      if (cfg.maxGenerationsWithoutImprovement > 0 &&
          generationsWithoutImprovement >=
              cfg.maxGenerationsWithoutImprovement) {
        XX_LOGD("[EvolutionTraining] Converged! Best score {:.4f} unchanged "
                "for {} generations. Stopping training.",
                bestScoreEver, generationsWithoutImprovement);
        savePopulationToFile(cfg.saveFilePath, cfg.saveFileBackupCount);
        break;
      }

      // 2j. 最大代数检查
      if (cfg.maxGenerations > 0 && generationCounter >= cfg.maxGenerations) {
        XX_LOGD("[EvolutionTraining] Reached maxGenerations {}. Stopping.",
                cfg.maxGenerations);
        savePopulationToFile(cfg.saveFilePath, cfg.saveFileBackupCount);
        break;
      }

      // 2k. 保存到文件（带备份轮转）
      savePopulationToFile(cfg.saveFilePath, cfg.saveFileBackupCount);
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
    config->prompt = best->prompt;
    XX_LOGD("[EvolutionTraining] Applied best prompt (id={}, score={:.4f}) to "
            "config",
            best->id, best->averageScore());
  }
};

} // namespace agent
} // namespace agentxx
