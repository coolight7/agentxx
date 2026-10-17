#include "agentxx-test/core/test_graph_data.h"

#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "neograph/graph/state.h"
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_gd_passed = 0;
int g_gd_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_gd_passed
#define XX_TEST_FAILED g_gd_failed

namespace agentxx {
namespace test {

using agentxx::middleware::MiddlewareContext;

/// 段落表 (来源键 → 文本) 的值类型 (与 graphData 的 map 通道同形)
using SectionMap = std::map<std::string, std::string, std::less<>>;

namespace {

/// 取对象键的字符串值
/// - 键缺失或值非字符串时返回占位文本: 序列化退化成 null 时本模块按断言失败报告,
///   不能用会抛异常的 [Json::get] / [Json::at] (否则测试进程被异常终止, 后续模块不再运行)
/// - `args`: [obj] 对象; [key] 键
std::string stringAt(const utilxx_base::Json& obj, std::string_view key) {
    const auto& value = obj[key];
    if (false == value.is_string()) {
        return "<not-a-string>";
    }
    return value.get<std::string>();
}

/// 取段落表里的段文本, 键缺失时返回占位文本 (同上, 失败路径不抛异常)
std::string sectionAt(const SectionMap& sections, std::string_view key) {
    auto it = sections.find(key);
    if (sections.end() == it) {
        return "<missing>";
    }
    return it->second;
}

} // namespace

TestResult testGraphData() {
    g_gd_passed = 0;
    g_gd_failed = 0;

    // ---- anyToJson: 动态段表 (缺失分支时返回 null, 中断 checkpoint 会丢值) ----
    {
        std::any value = SectionMap{{"skills", "S"}, {"memory", "M"}};
        auto     saved = MiddlewareContext::anyToJson(value);
        XX_TEST_EXPECT_TRUE(saved.is_object());
        XX_TEST_EXPECT_EQ(saved.size(), (size_t)2);
        XX_TEST_EXPECT_EQ(stringAt(saved, "memory"), std::string("M"));
        XX_TEST_EXPECT_EQ(stringAt(saved, "skills"), std::string("S"));
    }

    // 空表: 序列化为空对象 (不是 null, 否则恢复侧会与"该键缺失"混为一谈)
    {
        std::any value = SectionMap{};
        auto     saved = MiddlewareContext::anyToJson(value);
        XX_TEST_EXPECT_TRUE(saved.is_object());
        XX_TEST_EXPECT_EQ(saved.size(), (size_t)0);
    }

    // ---- jsonToValue: 反向转换与 anyToJson 对称 ----
    {
        auto sectionsJson      = utilxx_base::Json::object();
        sectionsJson["memory"] = std::string{"M"};
        sectionsJson["skills"] = std::string{"S"};

        const auto sections = MiddlewareContext::jsonToValue<SectionMap>(sectionsJson);
        XX_TEST_EXPECT_EQ(sections.size(), (size_t)2);
        XX_TEST_EXPECT_EQ(sectionAt(sections, "memory"), std::string("M"));
        XX_TEST_EXPECT_EQ(sectionAt(sections, "skills"), std::string("S"));

        // 非对象形态 (null / 数组): 退回空表, 不抛异常
        XX_TEST_EXPECT_TRUE(
            MiddlewareContext::jsonToValue<SectionMap>(utilxx_base::Json{nullptr}).empty()
        );
        XX_TEST_EXPECT_TRUE(
            MiddlewareContext::jsonToValue<SectionMap>(utilxx_base::Json::array()).empty()
        );
    }

    // ---- graphData → 中断 checkpoint (state 通道) → graphData ----
    // 与 AgentRunner 中断处理循环的快照入口、程序重启后的恢复入口同一套调用
    {
        const std::string sessionId = "graph_data_test";
        auto              source    = std::make_shared<MiddlewareContext>();

        // 注意: 提示词段落已不再走 graphData 通道 (改由 PromptSourceRegistry 持有,
        // 见 agentxx/agent/prompt_sources.h), 这里用普通通道验证 map 值的往返
        const std::string probeMapKey{"xx_testSectionMap"};
        source->setGraphDataItemValue<SectionMap>(
            sessionId,
            probeMapKey,
            SectionMap{{"memory", "M"}, {"skills", "S"}}
        );
        // 两个字符串通道同形 (不同值), 一并覆盖往返
        const std::string probeMapKeyB{"xx_testSectionMapB"};
        source->setGraphDataItemValue<SectionMap>(
            sessionId,
            probeMapKeyB,
            SectionMap{{"memory", "SM"}, {"skills", "SS"}}
        );
        source->setGraphDataItemValue<std::string>(
            sessionId,
            MiddlewareContext::graphDataKey_interruptNode,
            "llm"
        );
        source->setGraphDataItemValue<size_t>(
            sessionId,
            MiddlewareContext::graphDataKey_summarizationFailCount,
            size_t{2}
        );
        auto interruptValue     = utilxx_base::Json::object();
        interruptValue["tasks"] = 1;
        source->setGraphDataItemValue<utilxx_base::Json>(
            sessionId,
            MiddlewareContext::graphDataKey_interruptValue,
            interruptValue
        );

        neograph::graph::GraphState state;
        // 快照是图引擎的 json, 先过桥为业务 Json 再做断言/恢复 (与重启恢复路径一致)
        const auto savedSnapshot
            = agentxx::util::fromNeographJson(source->getGraphDataToState(state, sessionId));

        // map 值必须落成对象: 落成 null 即 checkpoint 丢值 (未覆盖类型的退化行为)
        const auto& savedSections = savedSnapshot[probeMapKey];
        XX_TEST_EXPECT_TRUE(savedSections.is_object());
        XX_TEST_EXPECT_EQ(savedSections.size(), (size_t)2);
        XX_TEST_EXPECT_EQ(stringAt(savedSections, "memory"), std::string("M"));

        // 同形的第二个 map 通道同样完整落盘
        const auto& savedStable = savedSnapshot[probeMapKeyB];
        XX_TEST_EXPECT_TRUE(savedStable.is_object());
        XX_TEST_EXPECT_EQ(stringAt(savedStable, "memory"), std::string("SM"));

        // 恢复 (程序重启 resume 的入口)
        auto target = std::make_shared<MiddlewareContext>();
        target->setGraphDataFromState(savedSnapshot, sessionId);

        const auto& restoredSections
            = target->getGraphDataItemValue<SectionMap>(sessionId, probeMapKey);
        XX_TEST_EXPECT_EQ(restoredSections.size(), (size_t)2);
        XX_TEST_EXPECT_EQ(sectionAt(restoredSections, "memory"), std::string("M"));
        XX_TEST_EXPECT_EQ(sectionAt(restoredSections, "skills"), std::string("S"));

        const auto& restoredStable
            = target->getGraphDataItemValue<SectionMap>(sessionId, probeMapKeyB);
        XX_TEST_EXPECT_EQ(restoredStable.size(), (size_t)2);
        XX_TEST_EXPECT_EQ(sectionAt(restoredStable, "skills"), std::string("SS"));

        // 其余常用类型同样往返完整
        XX_TEST_EXPECT_EQ(
            target
                ->getGraphDataItemValue<std::string>(
                    sessionId,
                    MiddlewareContext::graphDataKey_interruptNode
                ),
            std::string("llm")
        );
        XX_TEST_EXPECT_EQ(
            target->getGraphDataItemValue<size_t>(
                sessionId,
                MiddlewareContext::graphDataKey_summarizationFailCount
            ),
            (size_t)2
        );
        XX_TEST_EXPECT_EQ(
            target
                ->getGraphDataItemValue<utilxx_base::Json>(
                    sessionId,
                    MiddlewareContext::graphDataKey_interruptValue
                )
                .value("tasks", 0),
            1
        );
    }

    return TestResult{g_gd_passed, g_gd_failed};
}

} // namespace test
} // namespace agentxx
