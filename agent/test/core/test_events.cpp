#include "test_events.h"
#include "agentxx/event/events.h"

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ev_passed = 0;
int g_ev_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ev_passed
#define XX_TEST_FAILED g_ev_failed

namespace agentxx {
namespace test {

void test_events_topics() {
    using namespace agentxx::events;
    XX_TEST_EXPECT_TRUE(!Topic::AgentTurnStart.empty());
    XX_TEST_EXPECT_TRUE(!Topic::AgentTurnEnd.empty());
    XX_TEST_EXPECT_TRUE(!Topic::ModelCallStart.empty());
    XX_TEST_EXPECT_TRUE(!Topic::ModelToken.empty());
    XX_TEST_EXPECT_TRUE(!Topic::ModelCallEnd.empty());
    XX_TEST_EXPECT_TRUE(!Topic::ToolCallStart.empty());
    XX_TEST_EXPECT_TRUE(!Topic::ToolCallEnd.empty());
    XX_TEST_EXPECT_TRUE(!Topic::SubagentProgress.empty());
    XX_TEST_EXPECT_TRUE(!Topic::Display.empty());
    XX_TEST_EXPECT_TRUE(!Topic::UserInput.empty());
    XX_TEST_EXPECT_TRUE(!Topic::Cancel.empty());
    XX_TEST_EXPECT_TRUE(!Topic::Error.empty());
    XX_TEST_EXPECT_TRUE(!Topic::Interrupt.empty());
    XX_TEST_EXPECT_TRUE(!Topic::Permission.empty());
    XX_TEST_EXPECT_TRUE(!Topic::Subagent.empty());
}

void test_events_structs_defaultconstruct() {
    using namespace agentxx::events;
    {
        EventAgentTurnStart e{};
        e.agentName = "a";
        e.sessionId = "t1";
        e.userInput = "hi";
        XX_TEST_EXPECT_TRUE(e.agentName == "a" && e.userInput == "hi");
    }
    {
        EventModelToken e{};
        e.token = "tok";
        XX_TEST_EXPECT_TRUE(e.token == "tok");
    }
    {
        EventToolCallStart e{};
        e.toolName   = "fs_read";
        e.toolCallId = "call_1";
        e.arguments  = R"({"path":"/x"})";
        XX_TEST_EXPECT_TRUE(e.toolName == "fs_read" && e.toolCallId == "call_1");
    }
    {
        ReqPermission r{};
        r.category = "filesystem_read";
        r.target   = "/etc";
        RespPermission resp{};
        resp.decision = RespPermission::Decision::Allow;
        XX_TEST_EXPECT_TRUE(resp.decision == RespPermission::Decision::Allow);
    }
    {
        ReqSubagentBatch r{};
        r.tasks.push_back(SubagentBatchItem{});
        r.tasks[0].subagentName = "research";
        r.tasks[0].message      = "find foo";
        r.tasks[0].resultId     = "call_2";
        RespSubagentBatch resp{};
        resp.results.push_back(RespSubagentBatchItem{});
        resp.results[0].content  = "done";
        resp.results[0].hasError = false;
        XX_TEST_EXPECT_TRUE(resp.results[0].content == "done" && !resp.results[0].hasError);
    }
    {
        RespInterrupt resp{};
        resp.handled    = true;
        resp.resultJson = R"({"ok":true})";
        XX_TEST_EXPECT_TRUE(resp.handled && resp.resultJson == R"({"ok":true})");
    }
}

TestResult test_events() {
    test_events_topics();
    test_events_structs_defaultconstruct();
    return TestResult{g_ev_passed, g_ev_failed};
}

} // namespace test
} // namespace agentxx
