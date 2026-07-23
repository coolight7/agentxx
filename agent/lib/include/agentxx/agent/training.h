#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/config_static.h"
#include "agentxx/agent/deepagent.h"
#include "neograph/json.h"
#include "neograph/llm/openai_provider.h"
#include "neograph/types.h"
#include <agentxx/util/log.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace agentxx {
namespace agent {

// ======================== 数据结构 ========================

/// 单个训练测试用例
struct TrainingTestCase {
    std::string name;
    std::string input;
    std::string expectedOutput; // 描述预期的结果、评分标准等（供评分器参考）
    std::string equalOutput;    // 若不为空，则判断 agent 输出是否与此完全相等
    neograph::json extra;
};

/// 从 JSON 文件中加载测试用例
inline std::vector<TrainingTestCase> loadTestCasesFromFile(const std::string& filePath) {
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
        for (const auto& item : j) {
            TrainingTestCase tc;
            tc.name           = item.value("name", "");
            tc.input          = item.value("input", "");
            tc.expectedOutput = item.value("expectedOutput", "");
            tc.equalOutput    = item.value("equalOutput", "");
            tc.extra          = item.value("extra", neograph::json::object());
            cases.push_back(std::move(tc));
        }
        XX_LOGD("[Training] Loaded {} test cases from {}", cases.size(), filePath);
    } catch (const std::exception& e) {
        XX_LOGE("[Training] Failed to parse test case file {}: {}", filePath, e.what());
    }
    return cases;
}

/// 从目录中加载所有 JSON 测试用例文件
inline std::vector<TrainingTestCase> loadTestCasesFromDirectory(const std::string& dirPath) {
    std::vector<TrainingTestCase> allCases;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                auto fileCases = loadTestCasesFromFile(entry.path().string());
                allCases.insert(allCases.end(),
                                std::make_move_iterator(fileCases.begin()),
                                std::make_move_iterator(fileCases.end()));
            }
        }
        XX_LOGD("[Training] Loaded {} total test cases from directory {}",
                allCases.size(),
                dirPath);
    } catch (const std::exception& e) {
        XX_LOGE("[Training] Failed to load test cases from directory {}: {}", dirPath, e.what());
    }
    return allCases;
}

/// 剥离 LLM 响应中可能存在的 Markdown 代码块标记
inline std::string stripMarkdownCodeBlock(const std::string& content) {
    std::string result = content;
    auto        start  = result.find_first_not_of(" \t\n\r");
    auto        end    = result.find_last_not_of(" \t\n\r");
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
inline neograph::json parseJsonFromResponse(const std::string& content) {
    auto stripped = stripMarkdownCodeBlock(content);
    try {
        return neograph::json::parse(stripped);
    } catch (...) {
        auto first = stripped.find('{');
        auto last  = stripped.rfind('}');
        if (first != std::string::npos && last != std::string::npos && last > first) {
            return neograph::json::parse(stripped.substr(first, last - first + 1));
        }
        throw;
    }
}

/// 评分结果
struct TrainingScore {
    double         score = 0.0;
    std::string    feedback;
    bool           passed    = false;
    int            iteration = 0;
    neograph::json extra;
};

/// 优化器/变异器输出的 prompt 修改 patch
/// - patch 是一个 JSON，结构同 AgentPrompt::toJson，仅包含要修改的字段
/// - 空 patch 表示无修改
struct OptimizedPrompts {
    neograph::json patch;
    std::string    analysis;
};

/// Prompt 变体：存储完整 AgentPrompt 及其评分
/// 训练目标是 AgentPrompt 类内定义的全部提示词（含 toolPrompt）
struct PromptVariant {
    std::string                   id;
    AgentPrompt                   prompt;
    double                        cumulativeScore = 0.0;
    int                           testCount       = 0;
    int                           generation      = 0;
    std::string                   parentId;
    std::map<std::string, double> perTestCaseScores;
    neograph::json                extra;

    double averageScore() const;

    /// 计算整个 prompt 的 hash，用于去重
    size_t promptHash() const;
};

// ======================== 回调类型 ========================

/// 自定义评分回调
using TrainingScoringFunc
    = std::function<asio::awaitable<TrainingScore>(const std::string&      agentOutput,
                                                   const TrainingTestCase& testCase,
                                                   int                     iteration)>;

/// 迭代观察回调
using TrainingIterationCallback
    = std::function<void(const TrainingScore& score, const std::string& agentOutput)>;

// ======================== 进化训练配置 ========================

struct EvolutionTrainingConfig {
    /// 测试用例列表
    std::vector<TrainingTestCase> testCases;

    /// 保存/加载 prompt 变体的文件路径
    std::string saveFilePath
        = agentxx::agent::AgentConfigStatic::getResultPath("/train/training_prompts.json");

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
    int    earlyTerminationCheckAfter = 2;
    double earlyTerminationScore      = 0.2;

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
    std::mt19937               rng;
    int                        generationCounter = 0;

    // ---- 通用 LLM 调用 ----

    /// 设置 agent 的 system prompt 后发起一次非流式对话
    /// 注意: ModelCallWrapNode 会用 agentConfig->prompt.systemPrompt 覆盖
    /// 输入中的 system 消息，因此必须写入 config 而非通过消息传入
    asio::awaitable<std::string> runLLMAgent(std::shared_ptr<agentxx::agent::DeepAgent> agent,
                                             const std::string& systemPrompt,
                                             const std::string& userContent);

    /// 将变体的完整 prompt 写入 trainAgent 的运行时配置
    /// 这是让变体真正生效的关键：ModelCallWrapNode / 各 middleware / 各 tool
    /// 均从 agentConfig->prompt 读取所有提示词
    void applyVariantToTrainAgent(const PromptVariant& variant);

    // ---- 文件 I/O ----

    neograph::json promptVariantToJson(const PromptVariant& v) const;

    PromptVariant promptVariantFromJson(const neograph::json& j) const;

    /// 轮转备份保存文件：file -> file.1 -> file.2 -> ... -> file.N
    void rotateSaveFile(const std::string& path, int keepCount);

    void savePopulationToFile(const std::string& filePath, int backupCount = 0);

    bool loadPopulationFromFile(const std::string& filePath);

    // ---- 评分 ----

    asio::awaitable<TrainingScore> defaultScoringWithSubAgent(std::string_view        agentOutput,
                                                              const TrainingTestCase& testCase,
                                                              int                     iteration,
                                                              const EvolutionTrainingConfig& cfg);

    // ---- Prompt 优化 ----

    /// 构建用于优化器/变异器的上下文消息：包含当前完整 prompt
    std::string buildPromptContextMessage(const PromptVariant& variant) const;

    asio::awaitable<OptimizedPrompts> optimizeVariantWithLLM(const PromptVariant&    variant,
                                                             const TrainingTestCase& testCase,
                                                             const std::string&      agentOutput,
                                                             const TrainingScore&    score,
                                                             const EvolutionTrainingConfig& cfg);

    // ---- 变异操作 ----

    std::string generateId();

    /// 对字符串进行随机变异：以 mutationRate 概率对每个字符进行插入/删除/替换
    /// 注意: 字符级变异会破坏 prompt 语义，仅作为 LLM 变异不可用时的降级手段
    std::string mutateString(const std::string& input, double mutationRate);

    PromptVariant createChildVariantCharMut(const PromptVariant& parent, double mutationRate);

    /// 使用 LLM 对 prompt 进行语义级变异，生成多样化的探索变体
    asio::awaitable<PromptVariant> createChildVariantLLMMut(const PromptVariant&           parent,
                                                            const EvolutionTrainingConfig& cfg);

    // ---- 评估单个变体 ----

    /// 评估结果附带最差用例信息，供后续优化使用
    struct EvaluationResult {
        const TrainingTestCase* worstCase = nullptr;
        std::string             worstCaseOutput;
        TrainingScore           worstCaseScore;
    };

    asio::awaitable<EvaluationResult> evaluateVariant(PromptVariant&                 variant,
                                                      const EvolutionTrainingConfig& cfg);

    // ---- 去重 ----

    void deduplicatePopulation();

public:

    EvolutionTrainingAgent(std::shared_ptr<agentxx::agent::DeepAgent> in_scoreAgent,
                           std::shared_ptr<agentxx::agent::DeepAgent> in_trainAgent,
                           std::shared_ptr<agentxx::agent::DeepAgent> in_optimizerAgent = nullptr);

    // ---- 初始化种子 prompt ----

    void seedInitialPopulation(const std::string& baseSystemPrompt);

    /// 从完整 AgentPrompt 初始化种子（包含 planning/skill/tool prompts）
    void seedInitialPopulation(const AgentPrompt& prompt);

    /// 从 trainAgent 当前完整 AgentPrompt 初始化种子
    void seedInitialPopulationFromAgent();

    // ---- 进化训练主循环 ----

    asio::awaitable<void> runEvolutionLoop(const EvolutionTrainingConfig& cfg);

    // ---- 获取当前 population ----

    const std::vector<PromptVariant>& getPopulation() const;

    const PromptVariant* getBestPrompt() const;

    // ---- 将最优 prompt 应用到 AgentConfig ----

    void applyBestPromptToConfig(std::shared_ptr<AgentConfig> config);
};

} // namespace agent
} // namespace agentxx
