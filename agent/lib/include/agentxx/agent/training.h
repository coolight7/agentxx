#pragma once

#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/config_static.h"
#include "neograph/graph/cancel.h"
#include "neograph/json.h"
#include <atomic>
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

/// 从 JSON 数组解析测试用例
/// - 空名称自动生成 `case_N`；重名自动追加 `#N` 后缀，
///   保证 perTestCaseScores 的键唯一不互相覆盖
std::vector<TrainingTestCase> testCasesFromJson(const neograph::json& j);

/// 从 JSON 文件中加载测试用例 (实现见 training.cpp)
std::vector<TrainingTestCase> loadTestCasesFromFile(std::string_view filePath);

/// 从目录中加载所有 JSON 测试用例文件 (recursive=true 时递归子目录)
std::vector<TrainingTestCase>
    loadTestCasesFromDirectory(std::string_view dirPath, bool recursive = false);

/// 剥离 LLM 响应中可能存在的 Markdown 代码块标记 (实现见 training.cpp)
std::string stripMarkdownCodeBlock(std::string_view content);

/// 从 LLM 响应中解析 JSON：先剥离 markdown 代码块，失败则尝试提取首个 {...}
/// 子串 (实现见 training.cpp)
neograph::json parseJsonFromResponse(std::string_view content);

/// 规范化优化器/变异器输出的 prompt patch：
/// - 剔除空串字段：optimizerPrompt/mutationPrompt 均约定 ""=保持不变，
///   而 AgentPrompt::mergeFromJson 会用 JSON 中存在的字符串字段（含空串）
///   覆盖现值，必须先经此过滤，否则"想保留字段"会被误清空
/// - 剔除非 prompt 字段（analysis/strategy 等）与 toolPrompt 中的空 depict/args
neograph::json normalizePromptPatch(const neograph::json& parsed);

/// 对字符串进行 UTF-8 安全的字符级随机变异：
/// 以 mutationRate 概率对每个码点执行 插入(ASCII)/替换(ASCII)/删除，
/// 中文等多字节字符整体参与变异，不会被拆成非法字节序列。
/// 注意: 字符级变异会破坏 prompt 语义，仅作为 LLM 变异不可用时的降级手段
std::string mutateStringUtf8(std::string_view input, double mutationRate, std::mt19937& rng);

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
    std::string id;
    AgentPrompt prompt;
    double cumulativeScore = 0.0; // 最近一轮评估的原始总分（覆盖语义，非跨轮累计）
    int testCount = 0;            // 最近一轮的用例数
    /// 跨轮 EMA 平滑分（精英复评时更新；<0 表示未启用）。
    /// LLM-as-judge 单次评分噪声大, 排序/收敛判定优先使用平滑分以降噪
    double smoothedScore = -1.0;
    /// 评估轮数（含精英复评），用于判断平滑分是否已初始化
    int                           evalRounds = 0;
    int                           generation = 0;
    std::string                   parentId;
    std::map<std::string, double> perTestCaseScores;
    neograph::json                extra;

    double averageScore() const;

    /// 计算整个 prompt 的 hash，用于去重
    size_t promptHash() const;
};

// ======================== 回调类型 ========================

/// 自定义评分回调
using TrainingScoringFunc = std::function<asio::awaitable<
    TrainingScore>(std::string_view agentOutput, const TrainingTestCase& testCase, int iteration)>;

/// 迭代观察回调
using TrainingIterationCallback
    = std::function<void(const TrainingScore& score, std::string_view agentOutput)>;

// ======================== 进化训练配置 ========================

struct EvolutionTrainingConfig {
    /// 默认评分器 system prompt（大段文本实现于 training.cpp，
    /// 避免内联进所有包含本头的编译单元）
    static std::string defaultScoringPrompt();

    /// 默认优化器 system prompt
    static std::string defaultOptimizerPrompt();

    /// 默认变异器 system prompt
    static std::string defaultMutationPrompt();

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

    /// 每代对排序后前 N 个精英复评一次并做 EMA 平滑，降低 LLM 评分噪声
    /// 对排序与收敛判定的干扰；0 表示关闭精英复评
    int eliteReevaluatePerGen = 2;

    /// 评估前随机打乱用例顺序：早终检查基于前 k 例均分，
    /// 固定顺序会让排在后面的用例系统性影响早终判断（排序偏置）
    bool shuffleTestCases = true;

    /// 取消令牌：在代/用例边界轮询，取消后保存当前 population 并优雅退出。
    /// 为空则不支持取消。注意: 训练循环严格串行执行（变体写入共享 config），
    /// 不要在多个线程同时运行同一 trainer 的循环
    std::shared_ptr<neograph::graph::CancelToken> cancelToken;

    /// 保存文件时保留的历史备份数（0 表示不备份）
    int saveFileBackupCount = 3;

    /// 评分 subagent 的 system prompt
    std::string scoringPrompt = defaultScoringPrompt();

    /// 评分模型名称（为空则使用主 agent 的模型）
    std::string scoringModelName;

    /// 评分模型 API Key
    std::string scoringModelApiKey;

    /// 评分模型 Base URL
    std::string scoringModelBaseUrl;

    /// prompt 优化器（调整 prompt）的 system prompt
    std::string optimizerPrompt = defaultOptimizerPrompt();

    /// 优化器模型名称
    std::string optimizerModelName;

    /// 优化器 API Key
    std::string optimizerModelApiKey;

    /// 优化器 Base URL
    std::string optimizerModelBaseUrl;

    /// LLM 变异 prompt：用于生成多样化的 prompt 变体（探索而非改进）
    std::string mutationPrompt = defaultMutationPrompt();

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

    std::shared_ptr<agentxx::agent::BaseAgent> scoreAgent;
    std::shared_ptr<agentxx::agent::BaseAgent> trainAgent;
    std::shared_ptr<agentxx::agent::BaseAgent> optimizerAgent;

    std::vector<PromptVariant> population;
    std::mt19937               rng;
    int                        generationCounter = 0;
    /// id 序号: 同代内快速生成的变体 id 不依赖时间戳精度, 彻底避免碰撞
    std::atomic<uint64_t> idSeq{0};

    // ---- 通用 LLM 调用 ----

    /// 设置 agent 的 system prompt 后发起一次非流式对话
    /// 注意: ModelCallWrapNode 会用 agentConfig->prompt.systemPrompt 覆盖
    /// 输入中的 system 消息，因此必须写入 config 而非通过消息传入
    asio::awaitable<std::string> runLLMAgent(
        std::shared_ptr<agentxx::agent::BaseAgent> agent,
        std::string_view                           systemPrompt,
        std::string_view                           userContent
    );

    /// 将变体的完整 prompt 写入 trainAgent 的运行时配置
    /// 这是让变体真正生效的关键：ModelCallWrapNode / 各 middleware / 各 tool
    /// 均从 agentConfig->prompt 读取所有提示词
    void applyVariantToTrainAgent(const PromptVariant& variant);

    // ---- 文件 I/O ----

    neograph::json promptVariantToJson(const PromptVariant& v) const;

    PromptVariant promptVariantFromJson(const neograph::json& j) const;

    /// 轮转备份保存文件：file -> file.1 -> file.2 -> ... -> file.N
    void rotateSaveFile(std::string_view path, int keepCount);

    /// 原子保存: 先写 {filePath}.tmp 再替换主文件, 避免写入中途崩溃损坏数据
    void savePopulationToFile(std::string_view filePath, int backupCount = 0);

    bool loadPopulationFromFile(std::string_view filePath);

    // ---- 评分 ----

    asio::awaitable<TrainingScore> defaultScoringWithSubAgent(
        std::string_view               agentOutput,
        const TrainingTestCase&        testCase,
        int                            iteration,
        const EvolutionTrainingConfig& cfg
    );

    // ---- Prompt 优化 ----

    /// 构建用于优化器/变异器的上下文消息：包含当前完整 prompt
    std::string buildPromptContextMessage(const PromptVariant& variant) const;

    asio::awaitable<OptimizedPrompts> optimizeVariantWithLLM(
        const PromptVariant&           variant,
        const TrainingTestCase&        testCase,
        std::string_view               agentOutput,
        const TrainingScore&           score,
        const EvolutionTrainingConfig& cfg
    );

    // ---- 变异操作 ----

    std::string generateId();

    /// 对字符串进行随机变异：以 mutationRate 概率对每个字符进行插入/删除/替换
    /// 注意: 字符级变异会破坏 prompt 语义，仅作为 LLM 变异不可用时的降级手段
    std::string mutateString(std::string_view input, double mutationRate);

    PromptVariant createChildVariantCharMut(const PromptVariant& parent, double mutationRate);

    /// 使用 LLM 对 prompt 进行语义级变异，生成多样化的探索变体
    asio::awaitable<PromptVariant>
        createChildVariantLLMMut(const PromptVariant& parent, const EvolutionTrainingConfig& cfg);

    // ---- 评估单个变体 ----

    /// 评估结果附带最差用例信息，供后续优化使用
    struct EvaluationResult {
        const TrainingTestCase* worstCase = nullptr;
        std::string             worstCaseOutput;
        TrainingScore           worstCaseScore;
    };

    /// 评估一个变体: 逐用例运行 trainAgent 并评分
    /// - isEliteReevaluation=false: 首轮评估, 初始化平滑分
    /// - isEliteReevaluation=true: 精英复评, 与历史平滑分做 EMA 合并降噪
    asio::awaitable<EvaluationResult> evaluateVariant(
        PromptVariant&                 variant,
        const EvolutionTrainingConfig& cfg,
        bool                           isEliteReevaluation = false
    );

    // ---- 去重 / 会话清理 / 取消 ----

    void deduplicatePopulation();

    /// 评估候选预去重: 相对现有 population 与批内已接受项按 promptHash 过滤，
    /// 完全相同的候选直接丢弃，避免浪费评估算力
    [[nodiscard]] std::vector<PromptVariant>
        filterDuplicateCandidates(std::vector<PromptVariant>&& candidates) const;

    /// 清理一次评估产生的 trainAgent 会话（内存 SessionStore + 中间件句柄缓存），
    /// 防止长期训练中会话与 SQLite 数据无限累积
    void removeTrainSession(std::string_view sessionId) const;

    /// 取消轮询: cancelToken 非空且已取消时返回 true
    static bool cancelRequested(const EvolutionTrainingConfig& cfg);

public:

    EvolutionTrainingAgent(
        std::shared_ptr<agentxx::agent::BaseAgent> in_scoreAgent,
        std::shared_ptr<agentxx::agent::BaseAgent> in_trainAgent,
        std::shared_ptr<agentxx::agent::BaseAgent> in_optimizerAgent = nullptr
    );

    // ---- 初始化种子 prompt ----

    void seedInitialPopulation(std::string_view baseSystemPrompt);

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
