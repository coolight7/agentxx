#include "agentxx-test/core/test_usage_ledger.h"

#include "agentxx-test/core/test_agent.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/session_store.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <string>
#include <thread>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ul_passed = 0;
int g_ul_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ul_passed
#define XX_TEST_FAILED g_ul_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

/// 创建唯一临时目录 (测试根目录)
std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_ul_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

/// 删除测试临时目录 (Windows 上文件句柄可能稍晚释放, 短暂重试)
void removeTempRoot(const std::string& root) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::error_code ec;
        fs::remove_all(utilxx_base::utf8ToPath(root), ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    std::error_code ec;
    fs::remove_all(utilxx_base::utf8ToPath(root), ec);
    if (ec) {
        XX_LOGW("清理测试临时目录失败: {} ({})", root, ec.message());
    }
}

} // namespace

asio::awaitable<TestResult> test_usage_ledger() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);
    auto root    = makeTempRoot();

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->dataDir             = root;
    cfg->enableSessionStore  = true;
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";
    // 不重试: 失败用例立即以错误结束, 避免测试等待退避重试
    cfg->llmMaxRetry         = 0;

    g_da_sim_response_content  = "ledger response";
    g_da_sim_prompt_tokens     = 137;
    g_da_sim_completion_tokens = 29;
    g_da_sim_tool_calls        = utilxx_base::Json::array();
    g_da_sim_fail_count        = 0;

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    // ---- 成功轮次: 用量记录里记一次调用与用量 ----
    auto result = co_await agent.runTurnAsync("ledger_sess", "hello ledger", nullptr);
    XX_TEST_EXPECT_FALSE(result.hasError);

    auto store = agent.agentContext->sessions->sessionStore;
    XX_TEST_EXPECT_TRUE(store != nullptr);
    if (store) {
        auto sum = store->usageSummary("ledger_sess");
        XX_TEST_EXPECT_EQ(sum.calls, int64_t{1});
        XX_TEST_EXPECT_EQ(sum.failedCalls, int64_t{0});
        XX_TEST_EXPECT_EQ(sum.promptTokens, int64_t{137});
        XX_TEST_EXPECT_EQ(sum.completionTokens, int64_t{29});
        XX_TEST_EXPECT_EQ(sum.totalTokens, int64_t{166});

        auto recent = store->recentUsage("ledger_sess", 1);
        XX_TEST_EXPECT_EQ(recent.size(), size_t{1});
        if (recent.size() == 1) {
            XX_TEST_EXPECT_TRUE(recent[0].ok);
            XX_TEST_EXPECT_EQ(recent[0].model, std::string{"test-sim"});
            XX_TEST_EXPECT_TRUE(recent[0].timeMs > 0);
            XX_TEST_EXPECT_TRUE(recent[0].errorKind.empty());
        }
    }

    // ---- 失败轮次: API 持续失败也要留痕 (失败原因进用量记录) ----
    g_da_sim_fail_count = 5;
    auto failed         = co_await agent.runTurnAsync("ledger_fail", "make it fail", nullptr);
    XX_TEST_EXPECT_TRUE(failed.hasError);
    g_da_sim_fail_count = 0;

    if (store) {
        auto sum = store->usageSummary("ledger_fail");
        XX_TEST_EXPECT_GE(sum.calls, int64_t{1});
        XX_TEST_EXPECT_GE(sum.failedCalls, int64_t{1});
        auto recent = store->recentUsage("ledger_fail", 1);
        XX_TEST_EXPECT_EQ(recent.size(), size_t{1});
        if (recent.size() == 1) {
            XX_TEST_EXPECT_FALSE(recent[0].ok);
            XX_TEST_EXPECT_FALSE(recent[0].errorKind.empty());
            XX_TEST_EXPECT_EQ(recent[0].model, std::string{"test-sim"});
        }
        // 成功会话的用量记录不受失败会话影响
        auto okSum = store->usageSummary("ledger_sess");
        XX_TEST_EXPECT_EQ(okSum.calls, int64_t{1});
        XX_TEST_EXPECT_EQ(okSum.failedCalls, int64_t{0});
    }

    removeTempRoot(root);
    co_return TestResult{g_ul_passed, g_ul_failed};
}

} // namespace test
} // namespace agentxx
