#include "test_training.h"

#include "agentxx/agent/training.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/string_util.h"
#include <filesystem>
#include <fstream>
#include <random>
#include <set>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_training_passed = 0;
int g_training_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_training_passed
#define XX_TEST_FAILED g_training_failed

namespace agentxx {
namespace test {

// 访问器: 暴露 EvolutionTrainingAgent 的 protected 成员供测试
class TrainingTestAccessor : public agentxx::agent::EvolutionTrainingAgent {
public:

    TrainingTestAccessor() :
        agentxx::agent::EvolutionTrainingAgent(nullptr, nullptr, nullptr) {}

    // clang-format off
    using EvolutionTrainingAgent::population;                 // 数据成员公开
    using EvolutionTrainingAgent::promptVariantToJson;        // 序列化往返
    using EvolutionTrainingAgent::promptVariantFromJson;
    using EvolutionTrainingAgent::filterDuplicateCandidates;  // 预去重
    using EvolutionTrainingAgent::deduplicatePopulation;      // 全量去重
    using EvolutionTrainingAgent::cancelRequested;            // 取消轮询
    // clang-format on
};

namespace {

using namespace agentxx::agent;

/// 构造一个指定 systemPrompt 的变体
PromptVariant makeVariant(const std::string& id, const std::string& systemPrompt) {
    PromptVariant v;
    v.id                  = id;
    v.prompt.systemPrompt = systemPrompt;
    return v;
}

void testStripMarkdownCodeBlock() {
    // 标准 ```json 围栏剥离
    XX_TEST_EXPECT_EQ(
        stripMarkdownCodeBlock("```json\n{\"a\": 1}\n```"),
        std::string("{\"a\": 1}")
    );
    // 无语言标注围栏
    XX_TEST_EXPECT_EQ(stripMarkdownCodeBlock("```\nplain\n```"), std::string("plain"));
    // 无围栏原样返回 (仅去首尾空白)
    XX_TEST_EXPECT_EQ(stripMarkdownCodeBlock("  hello \n"), std::string("hello"));
    // 纯空白输入保持原样 (npos 分支)
    XX_TEST_EXPECT_EQ(stripMarkdownCodeBlock("   "), std::string("   "));
}

void testParseJsonFromResponse() {
    // 纯 JSON
    auto j1 = parseJsonFromResponse(R"({"score": 0.9})");
    XX_TEST_EXPECT_TRUE(j1.is_object());
    // 围栏包裹
    auto j2 = parseJsonFromResponse("```json\n{\"score\": 0.5}\n```");
    XX_TEST_EXPECT_TRUE(j2.is_object());
    // 前后夹杂说明文字时回退到大括号提取
    auto j3 = parseJsonFromResponse("Here is the result:\n{\"ok\": true}\nThanks!");
    XX_TEST_EXPECT_TRUE(j3.is_object());
    XX_TEST_EXPECT_EQ(j3.value("ok", false), true);
    // 完全无 JSON 时抛异常 (经 catchError 捕获转为错误结果, 不逃逸)
    bool errored = !agentxx::util::catchError<bool>(
        []() -> bool {
            (void)parseJsonFromResponse("no json at all");
            return true;
        },
        [](std::string) -> bool {
            return false;
        }
    );
    XX_TEST_EXPECT_TRUE(errored);
}

void testTestCasesFromJson() {
    auto j = neograph::json::parse(
        R"([
            {"name": "caseA", "input": "i1"},
            {"name": "caseA", "input": "i2"},
            {"input": "i3"},
            {"name": "caseA#2", "input": "i4"}
        ])"
    );
    auto cases = testCasesFromJson(j);
    XX_TEST_EXPECT_EQ(cases.size(), size_t{4});
    // 重名自动追加 #N 后缀, 空名自动编号, 保证键唯一不互相覆盖
    XX_TEST_EXPECT_EQ(cases[0].name, std::string("caseA"));
    XX_TEST_EXPECT_EQ(cases[1].name, std::string("caseA#2"));
    XX_TEST_EXPECT_EQ(cases[2].name, std::string("case_1"));
    std::set<std::string> names;
    for (const auto& c : cases) {
        names.insert(c.name);
    }
    XX_TEST_EXPECT_EQ(names.size(), size_t{4});

    // 非数组输入返回空且不抛异常
    XX_TEST_EXPECT_TRUE(testCasesFromJson(neograph::json::object()).empty());
}

void testLoadTestCasesFromFile() {
    namespace fs           = std::filesystem;
    const fs::path tmpPath = fs::temp_directory_path() / "agentxx_training_test_cases.json";
    {
        std::ofstream ofs(tmpPath, std::ios::out | std::ios::trunc);
        ofs << R"([{"name": "t1", "input": "a"}, {"name": "t2", "input": "b"}])";
    }
    auto cases = loadTestCasesFromFile(tmpPath.string());
    XX_TEST_EXPECT_EQ(cases.size(), size_t{2});
    XX_TEST_EXPECT_EQ(cases[0].name, std::string("t1"));
    fs::remove(tmpPath);

    // 损坏的 JSON 文件: 返回空且不抛异常
    {
        std::ofstream ofs(tmpPath, std::ios::out | std::ios::trunc);
        ofs << "{broken json";
    }
    XX_TEST_EXPECT_TRUE(loadTestCasesFromFile(tmpPath.string()).empty());
    fs::remove(tmpPath);

    // 不存在的文件: 返回空且不抛异常
    XX_TEST_EXPECT_TRUE(loadTestCasesFromFile("no/such/file_training.json").empty());
}

void testNormalizePromptPatch() {
    auto parsed = neograph::json::parse(
        R"({
            "systemPrompt": "",
            "systemPlanningPrompt": "plan v2",
            "systemSkillPrompt": "",
            "analysis": "some analysis",
            "strategy": "reorganize",
            "toolPrompt": {
                "tool_empty": {"depict": "", "args": {"arg_empty": ""}},
                "tool_ok": {"depict": "new depict", "args": {"good": "value", "bad": 3}},
                "tool_notobj": 5
            }
        })"
    );
    auto patch = normalizePromptPatch(parsed);
    XX_TEST_EXPECT_TRUE(patch.is_object());
    // 空串字段被剔除 ("保持不变"约定), 非空字段保留
    XX_TEST_EXPECT_FALSE(patch.contains("systemPrompt"));
    XX_TEST_EXPECT_FALSE(patch.contains("systemSkillPrompt"));
    XX_TEST_EXPECT_EQ(patch.value("systemPlanningPrompt", ""), std::string("plan v2"));
    // 非 prompt 字段被剔除
    XX_TEST_EXPECT_FALSE(patch.contains("analysis"));
    XX_TEST_EXPECT_FALSE(patch.contains("strategy"));
    // toolPrompt: 空 depict/args 被剔除, 非法结构被忽略
    XX_TEST_EXPECT_TRUE(patch.contains("toolPrompt"));
    const auto& tools = patch["toolPrompt"];
    XX_TEST_EXPECT_FALSE(tools.contains("tool_empty"));
    XX_TEST_EXPECT_FALSE(tools.contains("tool_notobj"));
    XX_TEST_EXPECT_TRUE(tools.contains("tool_ok"));
    XX_TEST_EXPECT_EQ(tools["tool_ok"].value("depict", ""), std::string("new depict"));
    XX_TEST_EXPECT_TRUE(tools["tool_ok"]["args"].contains("good"));
    XX_TEST_EXPECT_FALSE(tools["tool_ok"]["args"].contains("bad"));

    // 全部为空串时 patch 为空对象 (表示无修改)
    auto allEmpty = normalizePromptPatch(
        neograph::json::parse(R"({"systemPrompt": "", "systemSkillPrompt": ""})")
    );
    XX_TEST_EXPECT_TRUE(allEmpty.is_object());
    XX_TEST_EXPECT_TRUE(allEmpty.empty());
}

void testMutateStringUtf8() {
    std::mt19937      rng(42);
    const std::string chinese = "你好世界，提示词测试。English tail.";
    // rate=0 原样返回
    XX_TEST_EXPECT_EQ(mutateStringUtf8(chinese, 0.0, rng), chinese);
    // 高变异率下输出仍是合法 UTF-8 (多字节字符不被拆坏)
    for (int round = 0; round < 20; ++round) {
        auto out = mutateStringUtf8(chinese, 0.8, rng);
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8IsAvail(out));
        XX_TEST_EXPECT_FALSE(out.empty());
    }
    // 空串输入原样返回
    XX_TEST_EXPECT_EQ(mutateStringUtf8(std::string{}, 0.5, rng), std::string{});
}

void testAverageScoreSmoothing() {
    PromptVariant v;
    // 未评估: 0 分
    XX_TEST_EXPECT_EQ(v.averageScore(), 0.0);
    // 仅原始分: cumulative/testCount
    v.cumulativeScore = 4.0;
    v.testCount       = 5;
    XX_TEST_EXPECT_EQ(v.averageScore(), 0.8);
    // 平滑分启用后优先使用
    v.smoothedScore = 0.6;
    XX_TEST_EXPECT_EQ(v.averageScore(), 0.6);
    // 平滑分为负值视为未启用
    v.smoothedScore = -1.0;
    XX_TEST_EXPECT_EQ(v.averageScore(), 0.8);
}

void testVariantSerializationRoundtrip() {
    TrainingTestAccessor acc;
    PromptVariant        v = makeVariant("gen1_0_100", "sys prompt");
    v.generation           = 3;
    v.parentId             = "parent";
    v.cumulativeScore      = 3.5;
    v.testCount            = 5;
    v.smoothedScore        = 0.77;
    v.evalRounds           = 2;
    v.perTestCaseScores    = {
        {"c1", 0.9},
        {"c2", 0.5}
    };
    agentxx::agent::ToolPrompt tp{.depict = "dd", .args = {{"k", "v"}}};
    v.prompt.toolPrompt.emplace("my_tool", std::move(tp));

    auto j    = acc.promptVariantToJson(v);
    auto back = acc.promptVariantFromJson(j);
    XX_TEST_EXPECT_EQ(back.id, v.id);
    XX_TEST_EXPECT_EQ(back.parentId, v.parentId);
    XX_TEST_EXPECT_EQ(back.generation, v.generation);
    XX_TEST_EXPECT_EQ(back.testCount, v.testCount);
    XX_TEST_EXPECT_EQ(back.smoothedScore, v.smoothedScore);
    XX_TEST_EXPECT_EQ(back.evalRounds, v.evalRounds);
    XX_TEST_EXPECT_EQ(back.perTestCaseScores.size(), size_t{2});
    // prompt 内容逐字节一致 (hash 相同)
    XX_TEST_EXPECT_EQ(back.promptHash(), v.promptHash());

    // 兼容旧格式: 无 smoothedScore/evalRounds 字段时取默认值
    neograph::json old     = neograph::json::object();
    old["id"]              = "legacy";
    old["cumulativeScore"] = 1.0;
    old["testCount"]       = 2;
    auto legacy            = acc.promptVariantFromJson(old);
    XX_TEST_EXPECT_EQ(legacy.smoothedScore, -1.0);
    XX_TEST_EXPECT_EQ(legacy.evalRounds, 0);
}

void testDedupAndPreFilter() {
    TrainingTestAccessor acc;
    acc.population.clear();

    // deduplicatePopulation: 相同 prompt 的重复项被移除
    auto a      = makeVariant("a", "prompt A");
    auto aClone = makeVariant("a-clone-diff-id", "prompt A"); // id 不同但内容相同
    auto b      = makeVariant("b", "prompt B");
    acc.population.push_back(a);
    acc.population.push_back(aClone);
    acc.population.push_back(b);
    acc.deduplicatePopulation();
    XX_TEST_EXPECT_EQ(acc.population.size(), size_t{2});

    // filterDuplicateCandidates:
    // c1 与 population 中现有变体相同 → 剔除;
    // c2 是新 prompt → 保留;
    // c3 与批内已接受的 c2 相同 → 剔除
    acc.population.clear();
    acc.population.push_back(makeVariant("p", "P"));
    std::vector<PromptVariant> candidates;
    candidates.push_back(makeVariant("c1", "P"));  // 与父代完全相同
    candidates.push_back(makeVariant("c2", "P2")); // 新变体
    candidates.push_back(makeVariant("c3", "P2")); // 与批内已接受项重复
    auto kept = acc.filterDuplicateCandidates(std::move(candidates));
    XX_TEST_EXPECT_EQ(kept.size(), size_t{1});
    if (!kept.empty()) {
        XX_TEST_EXPECT_EQ(kept[0].id, std::string("c2"));
    }

    // 全部为重复候选时返回空
    std::vector<PromptVariant> dupOnly;
    dupOnly.push_back(makeVariant("d1", "P"));
    XX_TEST_EXPECT_TRUE(acc.filterDuplicateCandidates(std::move(dupOnly)).empty());
}

void testCancelRequested() {
    TrainingTestAccessor    acc;
    EvolutionTrainingConfig cfgNoToken;
    XX_TEST_EXPECT_FALSE(acc.cancelRequested(cfgNoToken));

    EvolutionTrainingConfig cfgWithToken;
    auto                    token = std::make_shared<neograph::graph::CancelToken>();
    cfgWithToken.cancelToken      = token;
    XX_TEST_EXPECT_FALSE(acc.cancelRequested(cfgWithToken));
    token->cancel();
    XX_TEST_EXPECT_TRUE(acc.cancelRequested(cfgWithToken));
}

} // namespace

TestResult testTraining() {
    TestResult result(g_training_passed, g_training_failed);

    testStripMarkdownCodeBlock();
    testParseJsonFromResponse();
    testTestCasesFromJson();
    testLoadTestCasesFromFile();
    testNormalizePromptPatch();
    testMutateStringUtf8();
    testAverageScoreSmoothing();
    testVariantSerializationRoundtrip();
    testDedupAndPreFilter();
    testCancelRequested();

    result.passed = g_training_passed;
    result.failed = g_training_failed;
    return result;
}

} // namespace test
} // namespace agentxx
