#pragma once

#include "agentxx/events.h"
#include "test_framework.h"
#include <cassert>

namespace agentxx {
namespace test {

inline static int g_ev_passed = 0;
inline static int g_ev_failed = 0;

#define EV_EXPECT_TRUE(expr)                                                   \
  do {                                                                         \
    if (expr) {                                                                \
      g_ev_passed++;                                                           \
    } else {                                                                   \
      g_ev_failed++;                                                           \
      TEST_FAIL << "line " << __LINE__ << ": expected true" << std::endl;      \
    }                                                                          \
  } while (0)

inline void test_events_topics() {
  using namespace agentxx::events;
  EV_EXPECT_TRUE(!Topic::AgentTurnStart.empty());
  EV_EXPECT_TRUE(!Topic::AgentTurnEnd.empty());
  EV_EXPECT_TRUE(!Topic::ModelCallStart.empty());
  EV_EXPECT_TRUE(!Topic::ModelToken.empty());
  EV_EXPECT_TRUE(!Topic::ModelCallEnd.empty());
  EV_EXPECT_TRUE(!Topic::ToolCallStart.empty());
  EV_EXPECT_TRUE(!Topic::ToolCallEnd.empty());
  EV_EXPECT_TRUE(!Topic::SubagentProgress.empty());
  EV_EXPECT_TRUE(!Topic::Display.empty());
  EV_EXPECT_TRUE(!Topic::UserInput.empty());
  EV_EXPECT_TRUE(!Topic::Cancel.empty());
  EV_EXPECT_TRUE(!Topic::Error.empty());
  EV_EXPECT_TRUE(!Topic::Interrupt.empty());
  EV_EXPECT_TRUE(!Topic::Permission.empty());
  EV_EXPECT_TRUE(!Topic::Subagent.empty());
}

inline void test_events_structs_defaultconstruct() {
  using namespace agentxx::events;
  {
    EventAgentTurnStart e{};
    e.agentName = "a";
    e.threadId = "t1";
    e.userInput = "hi";
    EV_EXPECT_TRUE(e.agentName == "a" && e.userInput == "hi");
  }
  {
    EventModelToken e{};
    e.token = "tok";
    EV_EXPECT_TRUE(e.token == "tok");
  }
  {
    EventToolCallStart e{};
    e.toolName = "fs_read";
    e.toolCallId = "call_1";
    e.arguments = R"({"path":"/x"})";
    EV_EXPECT_TRUE(e.toolName == "fs_read" && e.toolCallId == "call_1");
  }
  {
    ReqPermission r{};
    r.category = "filesystem_read";
    r.target = "/etc";
    RespPermission resp{};
    resp.decision = RespPermission::Decision::Allow;
    EV_EXPECT_TRUE(resp.decision == RespPermission::Decision::Allow);
  }
  {
    ReqSubagentStart r{};
    r.subagentName = "research";
    r.message = "find foo";
    r.resultId = "call_2";
    RespSubagentResult resp{};
    resp.content = "done";
    resp.hasError = false;
    EV_EXPECT_TRUE(resp.content == "done" && !resp.hasError);
  }
  {
    RespInterrupt resp{};
    resp.handled = true;
    resp.resultJson = R"({"ok":true})";
    EV_EXPECT_TRUE(resp.handled && resp.resultJson == R"({"ok":true})");
  }
}

inline TestResult test_events() {
  test_events_topics();
  test_events_structs_defaultconstruct();
  return TestResult{g_ev_passed, g_ev_failed};
}

} // namespace test
} // namespace agentxx
