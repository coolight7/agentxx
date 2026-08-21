#include "agentxx/agent/training.h"

#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <fmt/core.h>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace agentxx {
namespace agent {

namespace {

/// 精英复评的 EMA 平滑系数: 新一轮均分权重 0.5, 历史平滑分权重 0.5。
/// LLM-as-judge 单次评分噪声大, 复评取滑动平均可显著降低排序/收敛判定抖动
constexpr double kEliteSmoothingAlpha = 0.5;

/// UTF-8 首字节对应的序列长度 (非法首字节按单字节处理)
inline size_t utf8CharLen(unsigned char lead) {
    if (lead < 0x80) {
        return 1;
    }
    if ((lead >> 5) == 0x6) {
        return 2;
    }
    if ((lead >> 4) == 0xE) {
        return 3;
    }
    if ((lead >> 3) == 0x1E) {
        return 4;
    }
    return 1;
}

/// 完整比较两个 AgentPrompt 是否逐字段相等
/// 用于去重时对 hash 碰撞做二次确认, 避免 64 位哈希误删不同 prompt
bool agentPromptEquals(const AgentPrompt& a, const AgentPrompt& b) {
    if (a.systemPrompt != b.systemPrompt || a.systemPlanningPrompt != b.systemPlanningPrompt
        || a.systemSkillPrompt != b.systemSkillPrompt
        || a.summarizationPrompt != b.summarizationPrompt || a.toolPrompt.size() != b.toolPrompt.size()) {
        return false;
    }
    auto itA = a.toolPrompt.begin();
    auto itB = b.toolPrompt.begin();
    for (; itA != a.toolPrompt.end(); ++itA, ++itB) {
        if (itA->first != itB->first || itA->second.depict != itB->second.depict
            || itA->second.args != itB->second.args) {
            return false;
        }
    }
    return true;
}

} // namespace

// ======================== 数据结构实现 ========================

double PromptVariant::averageScore() const {
    // 启用精英复评后优先使用跨轮 EMA 平滑分, 降低单轮评分噪声影响
    if (smoothedScore >= 0.0) {
        return smoothedScore;
    }
    return testCount > 0 ? cumulativeScore / testCount : 0.0;
}

size_t PromptVariant::promptHash() const {
    return prompt.promptHash();
}

// ======================== 测试用例加载 ========================

std::vector<TrainingTestCase> testCasesFromJson(const neograph::json& j) {
    std::vector<TrainingTestCase> cases;
    if (!j.is_array()) {
        XX_LOGE("[Training] Test case JSON is not an array");
        return cases;
    }
    // 名称唯一化: 空名自动编号, 重名追加 #N 后缀,
    // 保证 evaluateVariant 中 perTestCaseScores 的键唯一不互相覆盖
    std::set<std::string> usedNames;
    int                   autoIdx = 0;
    for (const auto& item : j) {
        TrainingTestCase tc;
        tc.name           = item.value("name", "");
        tc.input          = item.value("input", "");
        tc.expectedOutput = item.value("expectedOutput", "");
        tc.equalOutput    = item.value("equalOutput", "");
        tc.extra          = item.value("extra", neograph::json::object());
        if (tc.name.empty()) {
            tc.name = fmt::format("case_{}", ++autoIdx);
        }
        int         suffix     = 1;
        std::string uniqueName = tc.name;
        while (!usedNames.insert(uniqueName).second) {
            uniqueName = fmt::format("{}#{}", tc.name, ++suffix);
        }
        tc.name = std::move(uniqueName);
        cases.push_back(std::move(tc));
    }
    return cases;
}

std::vector<TrainingTestCase> loadTestCasesFromFile(std::string_view filePath) {
    std::vector<TrainingTestCase> cases;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            std::ifstream ifs(std::string{filePath});
            if (!ifs.is_open()) {
                XX_LOGE("[Training] Failed to open test case file: {}", filePath);
                return true;
            }
            std::string content(
                (std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>()
            );
            ifs.close();

            auto j = neograph::json::parse(content);
            cases  = testCasesFromJson(j);
            XX_LOGD("[Training] Loaded {} test cases from {}", cases.size(), filePath);
            return true;
        },
        [filePath](std::string errmsg) -> bool {
            XX_LOGE("[Training] Failed to parse test case file {}: {}", filePath, errmsg);
            return false;
        }
    );
    return cases;
}

std::vector<TrainingTestCase>
    loadTestCasesFromDirectory(std::string_view dirPath, bool recursive) {
    std::vector<TrainingTestCase> allCases;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            namespace fs = std::filesystem;
            for (const auto& entry : fs::directory_iterator(dirPath)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    auto fileCases = loadTestCasesFromFile(entry.path().string());
                    allCases.insert(
                        allCases.end(),
                        std::make_move_iterator(fileCases.begin()),
                        std::make_move_iterator(fileCases.end())
                    );
                } else if (recursive && entry.is_directory()) {
                    auto subCases = loadTestCasesFromDirectory(entry.path().string(), true);
                    allCases.insert(
                        allCases.end(),
                        std::make_move_iterator(subCases.begin()),
                        std::make_move_iterator(subCases.end())
                    );
                }
            }
            XX_LOGD(
                "[Training] Loaded {} total test cases from directory {}{}",
                allCases.size(),
                dirPath,
                recursive ? " (recursive)" : ""
            );
            return true;
        },
        [dirPath](std::string errmsg) -> bool {
            XX_LOGE("[Training] Failed to load test cases from directory {}: {}", dirPath, errmsg);
            return false;
        }
    );
    return allCases;
}

std::string stripMarkdownCodeBlock(std::string_view content) {
    std::string result{content};
    auto        start = result.find_first_not_of(" \t\n\r");
    auto        end   = result.find_last_not_of(" \t\n\r");
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

neograph::json parseJsonFromResponse(std::string_view content) {
    auto stripped = stripMarkdownCodeBlock(content);
    return agentxx::util::catchError<neograph::json>(
        [&stripped]() -> neograph::json {
            return neograph::json::parse(stripped);
        },
        [&stripped](std::string errmsg) -> neograph::json {
            auto first = stripped.find('{');
            auto last  = stripped.rfind('}');
            if (first != std::string::npos && last != std::string::npos && last > first) {
                return neograph::json::parse(stripped.substr(first, last - first + 1));
            }
            // 两种解析均失败: 以原始错误信息抛出, 由调用方统一处理
            throw std::runtime_error(std::move(errmsg));
        }
    );
}

neograph::json normalizePromptPatch(const neograph::json& parsed) {
    neograph::json patch = neograph::json::object();

    // 顶层字符串字段: 空串视为"保持不变", 直接剔除 (约定见 optimizer/mutation prompt)
    auto addStringIfNonEmpty = [&](const char* key) {
        if (parsed.contains(key) && parsed[key].is_string()) {
            auto s = parsed[key].get<std::string>();
            if (!s.empty()) {
                patch[key] = std::move(s);
            }
        }
    };
    addStringIfNonEmpty("systemPrompt");
    addStringIfNonEmpty("systemPlanningPrompt");
    addStringIfNonEmpty("systemSkillPrompt");

    // toolPrompt: 同样剔除空 depict / 空 args 值; 整个工具无有效内容时不加入 patch
    if (parsed.contains("toolPrompt") && parsed["toolPrompt"].is_object()) {
        neograph::json tools = neograph::json::object();
        for (const auto& t : parsed["toolPrompt"].items()) {
            if (!t.second.is_object()) {
                continue;
            }
            neograph::json tp = neograph::json::object();
            if (t.second.contains("depict") && t.second["depict"].is_string()) {
                auto depict = t.second["depict"].get<std::string>();
                if (!depict.empty()) {
                    tp["depict"] = std::move(depict);
                }
            }
            if (t.second.contains("args") && t.second["args"].is_object()) {
                neograph::json args = neograph::json::object();
                for (const auto& a : t.second["args"].items()) {
                    if (a.second.is_string()) {
                        auto v = a.second.get<std::string>();
                        if (!v.empty()) {
                            args[a.first] = std::move(v);
                        }
                    }
                }
                if (!args.empty()) {
                    tp["args"] = std::move(args);
                }
            }
            if (!tp.empty()) {
                tools[t.first] = std::move(tp);
            }
        }
        if (!tools.empty()) {
            patch["toolPrompt"] = std::move(tools);
        }
    }
    return patch;
}

// ======================== 默认 prompt 定义 ========================

std::string EvolutionTrainingConfig::defaultScoringPrompt() {
    return R"(
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
}

std::string EvolutionTrainingConfig::defaultOptimizerPrompt() {
    return R"(
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
}

std::string EvolutionTrainingConfig::defaultMutationPrompt() {
    return R"(
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
}

// ======================== PromptVariant 序列化 ========================

// ======================== 进化训练 Agent 实现 ========================

asio::awaitable<std::string> EvolutionTrainingAgent::runLLMAgent(
    std::shared_ptr<agentxx::agent::BaseAgent> agent,
    std::string_view                           systemPrompt,
    std::string_view                           userContent
) {
    agent->getContext()->agentConfig->prompt.systemPrompt = std::string{systemPrompt};
    std::vector<neograph::ChatMessage> messages           = {
        neograph::ChatMessage{.role = "user", .content = std::string{userContent}},
    };
    auto result = co_await agent->runStreamTurnAsync(messages);
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
    // 平滑分与评估轮数: 跨代持久化精英复评的去噪结果
    j["smoothedScore"]   = v.smoothedScore;
    j["evalRounds"]      = v.evalRounds;
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
    v.smoothedScore   = j.value("smoothedScore", -1.0);
    v.evalRounds      = j.value("evalRounds", 0);
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
    namespace fs = std::filesystem;
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

            // 原子写: 先写临时文件再替换主文件,
            // 写入中途崩溃/断电不会损坏已有的完整数据 (备份 .1~.N 兜底更早版本)
            fs::path targetPath(std::string{filePath});
            fs::path tmpPath(std::string{filePath} + ".tmp");
            {
                std::ofstream ofs(tmpPath, std::ios::out | std::ios::trunc);
                if (!ofs.is_open()) {
                    XX_LOGE(
                        "[EvolutionTraining] Failed to open temp file for writing: {}",
                        tmpPath.string()
                    );
                    return false;
                }
                ofs << root.dump(2);
                ofs.flush();
                bool writeOk = !ofs.fail();
                ofs.close();
                if (!writeOk) {
                    XX_LOGE(
                        "[EvolutionTraining] Failed to write temp file: {}",
                        tmpPath.string()
                    );
                    return false;
                }
            }
            std::error_code ec;
            if (fs::exists(targetPath)) {
                fs::remove(targetPath, ec);
                if (ec) {
                    XX_LOGE(
                        "[EvolutionTraining] Failed to remove old save file {}: {}",
                        filePath,
                        ec.message()
                    );
                    return false;
                }
            }
            fs::rename(tmpPath, targetPath, ec);
            if (ec) {
                XX_LOGE(
                    "[EvolutionTraining] Failed to rename temp file to {}: {}",
                    filePath,
                    ec.message()
                );
                return false;
            }
            XX_LOGD("[EvolutionTraining] Saved {} prompts to {}", population.size(), filePath);
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
            if (!(root.contains("population") && root["population"].is_array())) {
                return false;
            }

            // 先填充局部向量, 全部解析成功且非空后再替换 population:
            // 避免半途失败或空文件清掉调用方已注入的种子
            std::vector<PromptVariant> loadedVec;
            for (const auto& j : root["population"]) {
                loadedVec.push_back(promptVariantFromJson(j));
            }
            if (loadedVec.empty()) {
                // 空 population 文件视为无效存档, 拒绝加载 (避免后续静默空转)
                XX_LOGE(
                    "[EvolutionTraining] Save file {} contains an empty "
                    "population, ignore it",
                    filePath
                );
                return false;
            }

            population        = std::move(loadedVec);
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
        },
        [filePath](std::string errmsg) -> bool {
            XX_LOGE("[EvolutionTraining] Failed to load population: {}", errmsg);
            return false;
        }
    );
    return loaded;
}

// ======================== 评分 ========================

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
    // 类型安全: extra.language 必须是字符串才拼接,
    // 避免配置写成数字/对象时 json 类型异常逃逸导致整个训练崩溃
    if (testCase.extra.contains("language") && testCase.extra["language"].is_string()) {
        scoringMessage << "Required language: " << testCase.extra["language"].get<std::string>()
                       << "\n";
    }

    // catchErrorAsync: 评分子代理异常 (含取消/中断类) 转为 0 分反馈, 训练循环继续;
    // 取消会在循环边界被再次检测到并优雅退出
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            auto content
                = co_await runLLMAgent(scoreAgent, cfg.scoringPrompt, scoringMessage.str());
            auto parsed = parseJsonFromResponse(content);
            if (parsed.is_object()) {
                result.score    = parsed.value("score", 0.0);
                result.feedback = parsed.value("feedback", std::string{});
                // passed 不信任评分模型自报 (阈值语义可能漂移),
                // 按收敛阈值本地判定, 保证与 score 数值始终一致
                result.passed   = result.score >= cfg.convergenceThreshold;
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
        },
        cfg.cancelToken
    );

    co_return result;
}

// ======================== Prompt 优化 ========================

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
                // 规范化 patch: 剔除空串字段 ("") 与 analysis 等非 prompt 字段。
                // 若不过滤, LLM 按约定返回 "" 表示保留时, mergeFromJson 会把
                // 对应 prompt 清空 (历史 bug)
                result.patch    = normalizePromptPatch(parsed);
                result.analysis = parsed.value("analysis", std::string{});
                if (!result.patch.empty()) {
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
        },
        cfg.cancelToken
    );
    co_return result;
}

// ======================== 变异操作 ========================

std::string EvolutionTrainingAgent::generateId() {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    // 附带原子递增序号: 同代内快速生成的变体 id 不依赖时间戳精度, 避免碰撞
    return fmt::format(
        "gen{}_{}_{}",
        generationCounter,
        idSeq.fetch_add(1, std::memory_order_relaxed),
        now
    );
}

// UTF-8 安全的字符级随机变异: 以码点为单位执行 插入(ASCII)/替换(ASCII)/删除,
// 多字节字符 (如中文) 整体参与变异, 不会被拆成非法字节序列
std::string mutateStringUtf8(std::string_view input, double mutationRate, std::mt19937& rng) {
    if (mutationRate <= 0.0 || input.empty()) {
        return std::string{input};
    }
    std::string result;
    result.reserve(input.size() * 2);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    static const char                      mutationChars[]
        = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 "
          ".,;:!?-_()[]{}\n";
    static const int                   mutationCharsLen = static_cast<int>(sizeof(mutationChars) - 1);
    std::uniform_int_distribution<int> charDist(0, mutationCharsLen - 1);
    std::uniform_int_distribution<int> opDist(0, 2);

    size_t i = 0;
    while (i < input.size()) {
        const size_t charLen = std::min(utf8CharLen(static_cast<unsigned char>(input[i])), input.size() - i);
        if (dist(rng) < mutationRate) {
            switch (opDist(rng)) {
                case 0:
                    // 在该码点前插入一个随机 ASCII 字符
                    result.push_back(mutationChars[charDist(rng)]);
                    result.append(input.substr(i, charLen));
                    break;
                case 1:
                    // 用随机 ASCII 字符替换整个码点
                    result.push_back(mutationChars[charDist(rng)]);
                    break;
                case 2:
                    // 删除整个码点
                    break;
            }
        } else {
            result.append(input.substr(i, charLen));
        }
        i += charLen;
    }
    if (result.empty()) {
        result = input;
    }
    return result;
}

std::string EvolutionTrainingAgent::mutateString(std::string_view input, double mutationRate) {
    return mutateStringUtf8(input, mutationRate, rng);
}

PromptVariant EvolutionTrainingAgent::createChildVariantCharMut(
    const PromptVariant& parent,
    double               mutationRate
) {
    PromptVariant child;
    child.id     = generateId();
    child.prompt = parent.prompt;
    // 字符级变异仅作用于 3 个 system prompt，toolPrompt 保持不变
    // （字符级变异会破坏语义，仅作为 LLM 变异不可用时的降级手段；
    //   变异按 UTF-8 码点进行, 中文等多字节字符不会被拆坏）
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

    // 防御性检查: 无优化器时直接降级为字符变异 (调用侧已有守卫, 这里保证方法自洽)
    if (!optimizerAgent) {
        child = createChildVariantCharMut(parent, cfg.mutationRate);
        co_return child;
    }

    std::ostringstream msg;
    msg << buildPromptContextMessage(parent);
    msg << "\nGenerate a diverse variation of these prompts.";

    // catchErrorAsync: LLM 变异失败 (含取消/中断类) 降级为字符变异, 训练循环继续
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            auto content = co_await runLLMAgent(optimizerAgent, cfg.mutationPrompt, msg.str());
            auto parsed  = parseJsonFromResponse(content);
            if (parsed.is_object()) {
                // 先规范化 patch 再合并: 只覆盖非空的 prompt 字段,
                // 避免空串 "" (约定=保持不变) 与 strategy/analysis 键造成误清空
                child.prompt.mergeFromJson(normalizePromptPatch(parsed));
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
        },
        cfg.cancelToken
    );
    co_return child;
}

// ======================== 评估单个变体 ========================

bool EvolutionTrainingAgent::cancelRequested(const EvolutionTrainingConfig& cfg) {
    return cfg.cancelToken != nullptr && cfg.cancelToken->is_cancelled();
}

void EvolutionTrainingAgent::removeTrainSession(std::string_view sessionId) const {
    auto ctx = trainAgent ? trainAgent->getContext() : nullptr;
    if (!ctx || !ctx->sessions) {
        return;
    }
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            // 与 runInternalAsync(cleanupAfter=true) 相同的清理路径:
            // 移除内存 Session + 中间件句柄缓存, 防止长期训练中
            // 会话消息与 SQLite 落盘数据无限累积
            ctx->sessions->remove(sessionId);
            if (ctx->middlewareHandleContext) {
                ctx->middlewareHandleContext->cleanupSession(std::string{sessionId});
            }
            return true;
        },
        [sessionId](std::string errmsg) -> bool {
            XX_LOGD("[EvolutionTraining] Cleanup session {} failed: {}", sessionId, errmsg);
            return false;
        }
    );
}

asio::awaitable<EvolutionTrainingAgent::EvaluationResult> EvolutionTrainingAgent::evaluateVariant(
    PromptVariant&                 variant,
    const EvolutionTrainingConfig& cfg,
    bool                           isEliteReevaluation
) {
    EvaluationResult evResult;
    variant.perTestCaseScores.clear();
    double totalScore = 0.0;

    // 变体 prompt 在评估开始前应用一次即可 (训练循环严格串行执行)
    applyVariantToTrainAgent(variant);

    // 随机打乱用例顺序: 早终检查基于前 k 例均分, 固定顺序会让排在后面的
    // 用例系统性影响早终判断 (排序偏置); 打乱后等价于随机抽样
    std::vector<size_t> order(cfg.testCases.size());
    std::iota(order.begin(), order.end(), 0);
    if (cfg.shuffleTestCases && order.size() > 1) {
        std::shuffle(order.begin(), order.end(), rng);
    }

    int                          evaluatedCount = 0;
    bool                         cancelled      = false;
    std::map<std::string, int>   scoreKeySeen; // 重名用例计数

    for (size_t pos = 0; pos < order.size(); ++pos) {
        const size_t caseIdx = order[pos];
        const auto&  testCase = cfg.testCases[caseIdx];
        const auto sessionId = fmt::format("evotrain_{}_{}_{}", variant.id, caseIdx, testCase.name);

        // 取消检查 (用例边界): 剩余用例不再评估, 外层循环保存现场后退出
        if (cancelRequested(cfg)) {
            cancelled = true;
            break;
        }

        if (cfg.verbose) {
            XX_LOGD(
                "[EvolutionTraining] [{}] Testing case '{}/{}' with variant '{}'",
                generationCounter,
                pos + 1,
                order.size(),
                variant.id
            );
        }

        // 单个用例的失败不应终止整个训练: agent 运行/评分异常计 0 分并继续。
        // (catchErrorAsync 把取消类异常也转到这里; 循环边界会再次检测取消)
        std::string agentOutput;
        TrainingScore score;
        score.iteration = generationCounter;
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                agentOutput
                    = co_await trainAgent->runSingleInputAsync(sessionId, testCase.input, "");

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
                co_return true;
            },
            [&](std::string errmsg) -> asio::awaitable<bool> {
                score.score    = 0.0;
                score.feedback = fmt::format("Evaluation error: {}", errmsg);
                score.passed   = false;
                co_return false;
            },
            [&](std::string& errmsg) -> std::optional<bool> {
                score.score    = 0.0;
                score.feedback = fmt::format("Evaluation error: {}", errmsg);
                score.passed   = false;
                return false;
            },
            cfg.cancelToken
        );

        // 评估完立即清理会话, 避免每个 变体x用例 都在 SessionStore/SQLite
        // 留下一份完整历史 (长期训练下内存与磁盘无限增长)
        removeTrainSession(sessionId);

        // perTestCaseScores 以用例名为 key; 配置重名时追加 #N 后缀避免覆盖
        ++scoreKeySeen[testCase.name];
        const std::string scoreKey = scoreKeySeen[testCase.name] == 1
                                       ? testCase.name
                                       : fmt::format("{}#{}", testCase.name, scoreKeySeen[testCase.name]);
        variant.perTestCaseScores[scoreKey] = score.score;
        totalScore += score.score;
        evaluatedCount++;

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
            // 观察回调异常同样不应终止训练
            agentxx::util::catchError<bool>(
                [&]() -> bool {
                    cfg.onIteration(score, agentOutput);
                    return true;
                },
                [](std::string errmsg) -> bool {
                    XX_LOGE("[EvolutionTraining] onIteration callback error: {}", errmsg);
                    return false;
                }
            );
        }

        // 早终检查 (基于随机顺序的前缀均值):
        // 未评估的剩余用例不计入分数统计 (不写 0 分占位, 保持最近一轮真实数据)
        if (cfg.earlyTerminationCheckAfter > 0 && evaluatedCount >= cfg.earlyTerminationCheckAfter) {
            double avg = totalScore / evaluatedCount;
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
                break;
            }
        }
    }

    // 分数统计说明:
    // - cumulativeScore/testCount/perTestCaseScores 始终保存最近一轮原始结果
    // - smoothedScore 仅在精英复评时跨轮 EMA 平滑, 排序与收敛判定用它降噪
    const size_t nCases     = cfg.testCases.size();
    variant.cumulativeScore = totalScore;
    variant.testCount       = static_cast<int>(nCases);
    const double roundAvg   = nCases > 0 ? totalScore / static_cast<double>(nCases) : 0.0;
    if (isEliteReevaluation && variant.evalRounds > 0 && variant.smoothedScore >= 0.0) {
        variant.smoothedScore
            = kEliteSmoothingAlpha * roundAvg + (1.0 - kEliteSmoothingAlpha) * variant.smoothedScore;
        variant.evalRounds++;
    } else {
        variant.smoothedScore = roundAvg;
        variant.evalRounds    = 1;
    }

    if (cfg.verbose) {
        XX_LOGD(
            "[EvolutionTraining] [{}] Variant '{}' avgScore={:.4f} rounds={}{}{}",
            generationCounter,
            variant.id,
            variant.averageScore(),
            variant.evalRounds,
            evaluatedCount < static_cast<int>(nCases) ? " (early-terminated)" : "",
            cancelled ? " (cancelled)" : ""
        );
    }
    co_return evResult;
}

// ======================== 去重 ========================

void EvolutionTrainingAgent::deduplicatePopulation() {
    std::vector<PromptVariant> unique;
    unique.reserve(population.size());
    for (auto& v : population) {
        auto h  = v.promptHash();
        bool dup = false;
        // hash 相同再做一次完整字段比对, 避免 64 位哈希碰撞误删不同 prompt
        for (const auto& u : unique) {
            if (u.promptHash() == h && agentPromptEquals(u.prompt, v.prompt)) {
                dup = true;
                break;
            }
        }
        if (!dup) {
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

std::vector<PromptVariant>
    EvolutionTrainingAgent::filterDuplicateCandidates(std::vector<PromptVariant>&& candidates
    ) const {
    // 评估前预去重: 与现有 population 以及本批已接受的候选比较,
    // 完全相同的候选 (如 LLM 返回全空字段导致子代==父代) 直接丢弃,
    // 不再浪费整套用例的评估算力
    std::vector<PromptVariant> unique;
    unique.reserve(candidates.size());
    size_t dupCount = 0;
    for (auto& v : candidates) {
        auto h   = v.promptHash();
        bool dup = false;
        for (const auto& p : population) {
            if (p.promptHash() == h && agentPromptEquals(p.prompt, v.prompt)) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            for (const auto& u : unique) {
                if (u.promptHash() == h && agentPromptEquals(u.prompt, v.prompt)) {
                    dup = true;
                    break;
                }
            }
        }
        if (dup) {
            dupCount++;
            continue;
        }
        unique.push_back(std::move(v));
    }
    if (dupCount > 0) {
        XX_LOGD(
            "[EvolutionTraining] Candidate pre-dedup: skipped {} duplicates "
            "({} -> {})",
            dupCount,
            dupCount + unique.size(),
            unique.size()
        );
    }
    return unique;
}

// ======================== 构造与种子 ========================

EvolutionTrainingAgent::EvolutionTrainingAgent(
    std::shared_ptr<agentxx::agent::BaseAgent> in_scoreAgent,
    std::shared_ptr<agentxx::agent::BaseAgent> in_trainAgent,
    std::shared_ptr<agentxx::agent::BaseAgent> in_optimizerAgent
) :
    scoreAgent(in_scoreAgent),
    trainAgent(in_trainAgent),
    optimizerAgent(in_optimizerAgent),
    // 显式转换到 mt19937::result_type, 消除时间戳 rep 截断告警
    rng(static_cast<std::mt19937::result_type>(
        std::chrono::system_clock::now().time_since_epoch().count()
    )) {}

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

// ======================== 进化训练主循环 ========================

asio::awaitable<void> EvolutionTrainingAgent::runEvolutionLoop(const EvolutionTrainingConfig& cfg) {
    auto sortByScore = [](std::vector<PromptVariant>& pop) {
        std::sort(
            pop.begin(),
            pop.end(),
            [](const PromptVariant& a, const PromptVariant& b) {
                return a.averageScore() > b.averageScore();
            }
        );
    };

    // Step 1: 尝试从文件加载已有 population
    bool loaded = loadPopulationFromFile(cfg.saveFilePath);

    // 无论文件加载失败还是存档为空, 空 population 一律拒绝运行
    // (避免不消耗任何评估的静默空转)
    if (population.empty()) {
        XX_LOGE(
            "[EvolutionTraining] No population available. "
            "Call seedInitialPopulation() first or provide a valid save file."
        );
        co_return;
    }

    if (!loaded && cfg.verbose) {
        XX_LOGD("[EvolutionTraining] Starting fresh with {} seed prompts", population.size());
    }

    // 用例重名提示: 评估时会自动追加 #N 后缀保证键唯一, 这里仅提示配置问题
    {
        std::set<std::string> names;
        for (const auto& tc : cfg.testCases) {
            if (!names.insert(tc.name).second) {
                XX_LOGW(
                    "[EvolutionTraining] Duplicate test case name '{}' in config",
                    tc.name
                );
            }
        }
    }

    deduplicatePopulation();
    sortByScore(population);

    double bestScoreEver                 = population.front().averageScore();
    int    generationsWithoutImprovement = 0;

    while (true) {
        // 取消检查 (代边界): 优雅退出并保存现场
        if (cancelRequested(cfg)) {
            XX_LOGW(
                "[EvolutionTraining] Cancelled at generation {} boundary. "
                "Saving and stopping.",
                generationCounter
            );
            savePopulationToFile(cfg.saveFilePath, cfg.saveFileBackupCount);
            co_return;
        }

        generationCounter++;

        if (cfg.verbose) {
            XX_LOGD(
                "[EvolutionTraining] ====== Generation {} | Population: {} ======",
                generationCounter,
                population.size()
            );
        }

        // 2a. 从当前 population 中选取 top 变体进行变异
        int                        mutateFrom = std::min(cfg.mutateCount, static_cast<int>(population.size()));
        std::vector<PromptVariant> newGeneration;
        newGeneration.reserve(static_cast<size_t>(mutateFrom) * cfg.childrenPerParent);
        for (int i = 0; i < mutateFrom; ++i) {
            const auto& parent = population[i];
            for (int c = 0; c < cfg.childrenPerParent; ++c) {
                if (cancelRequested(cfg)) {
                    break;
                }
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

        // 评估前预去重: 与父代及批内候选完全相同的子代直接丢弃,
        // 避免浪费整套用例的评估算力
        newGeneration = filterDuplicateCandidates(std::move(newGeneration));

        // 全部子代与现有变体重复: 本代没有新评估可做。
        // 必须推进停滞计数, 否则会在零消耗下无限空转
        if (newGeneration.empty()) {
            XX_LOGD(
                "[EvolutionTraining] [{}] Generation produced no unique variants",
                generationCounter
            );
            generationsWithoutImprovement++;
            if ((cfg.maxGenerationsWithoutImprovement > 0
                 && generationsWithoutImprovement >= cfg.maxGenerationsWithoutImprovement)
                || (cfg.maxGenerations > 0 && generationCounter >= cfg.maxGenerations)) {
                savePopulationToFile(cfg.saveFilePath, cfg.saveFileBackupCount);
                break;
            }
            continue;
        }

        // 2b. 测试所有新变体，并收集最差用例信息
        std::vector<EvaluationResult> evalResults;
        evalResults.reserve(newGeneration.size());
        for (auto& variant : newGeneration) {
            evalResults.push_back(co_await evaluateVariant(variant, cfg));
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
                if (cancelRequested(cfg)) {
                    break;
                }
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
                improved.smoothedScore   = -1.0;
                improved.evalRounds      = 0;
                improved.perTestCaseScores.clear();
                // 以 patch 方式合并优化结果
                improved.prompt.mergeFromJson(optimized.patch);
                optimizedVariants.push_back(std::move(improved));
            }
        }

        // 2d. 测试优化后的变体（在全部用例上公平评估）
        for (auto& variant : optimizedVariants) {
            if (cancelRequested(cfg)) {
                break;
            }
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
        sortByScore(population);

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

        // 2g. 精英复评 (EMA 去噪): 对排序后的前 N 个精英重新评估一轮并与历史平滑分合并,
        // 降低 LLM 评分噪声对排序/收敛判定的影响。本代新生成的变体刚评过, 跳过
        bool eliteReevaluated = false;
        if (cfg.eliteReevaluatePerGen > 0) {
            size_t eliteN
                = std::min<size_t>(static_cast<size_t>(cfg.eliteReevaluatePerGen), population.size());
            for (size_t i = 0; i < eliteN; ++i) {
                if (cancelRequested(cfg)) {
                    break;
                }
                auto&       elite = population[i];
                if (elite.generation >= generationCounter) {
                    continue; // 本代新产生, 分数新鲜无需复评
                }
                if (cfg.verbose) {
                    XX_LOGD(
                        "[EvolutionTraining] [{}] Re-evaluating elite '{}' "
                        "(rounds={})",
                        generationCounter,
                        elite.id,
                        elite.evalRounds
                    );
                }
                co_await evaluateVariant(elite, cfg, /*isEliteReevaluation=*/true);
                eliteReevaluated = true;
            }
            if (eliteReevaluated) {
                // 复评更新了平滑分, 需要重新排序
                sortByScore(population);
            }
        }

        // 2h. 打印 top 信息
        if (cfg.verbose && !population.empty()) {
            XX_LOGD("[EvolutionTraining] [{}] Top 5 prompts:", generationCounter);
            for (size_t i = 0; i < std::min(population.size(), static_cast<size_t>(5)); ++i) {
                [[maybe_unused]] const auto& v = population[i];
                XX_LOGD(
                    "  [{}/{}] id={} avg={:.4f} rounds={} tests={} gen={} parent={}",
                    i + 1,
                    population.size(),
                    v.id,
                    v.averageScore(),
                    v.evalRounds,
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

        // 取消检查 (轮末统一出口): 已合并/排序/裁剪完成, 保存后退出
        if (cancelRequested(cfg)) {
            XX_LOGW(
                "[EvolutionTraining] Cancelled during generation {}. "
                "Saving and stopping.",
                generationCounter
            );
            savePopulationToFile(cfg.saveFilePath, cfg.saveFileBackupCount);
            co_return;
        }

        // 更新停滞计数 (基于平滑分)
        {
            double currentBestScore = population.empty() ? 0.0 : population.front().averageScore();
            if (currentBestScore > bestScoreEver) {
                bestScoreEver                 = currentBestScore;
                generationsWithoutImprovement = 0;
            } else {
                generationsWithoutImprovement++;
            }
        }

        // 2k. 每代末保存到文件（原子写 + 备份轮转）
        savePopulationToFile(cfg.saveFilePath, cfg.saveFileBackupCount);
    }

    co_return;
}

// ======================== 公共访问接口 ========================

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
