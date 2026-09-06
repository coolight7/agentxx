#include "test_datetime_tool.h"
#include "agentxx/agent/context.h"
// 原 lib 内置工具已迁移至 agentxx_system 插件 (同名同行为); 测试直测插件
// 同一实现 (system_impl.h), 保证插件行为与测试覆盖一致
#include "agentxx_system/system_impl.h"
#include <asio/awaitable.hpp>
#include <iostream>
#include <neograph/types.h>
#include <regex>
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_dt_passed = 0;
int g_dt_failed = 0;
} // namespace

namespace agentxx {
namespace tools {

/// 测试适配: 原 GetCurrentDateTimeTool 的同名薄包装 (工具类已迁移至插件,
/// execute_async 直调插件实现, 保持既有用例结构不变)
struct GetCurrentDateTimeTool {
    explicit GetCurrentDateTimeTool(std::weak_ptr<agentxx::agent::AgentContext>) {}

    neograph::ChatTool get_definition() const {
        return {
            "agentxx_get_current_datetime",
            "Get the current date, time, and Unix timestamp.",
            {}
        };
    }

    asio::awaitable<std::string> execute_async(const neograph::json&) const {
        co_return agentxx_system_plugin::currentDatetimeExecute();
    }
};

} // namespace tools
} // namespace agentxx

namespace agentxx {
namespace test {

asio::awaitable<void>
    test_datetime_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::GetCurrentDateTimeTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_get_current_datetime") {
        g_dt_passed++;
        TEST_PASS << "GetCurrentDateTimeTool::get_definition() name correct" << std::endl;
    } else {
        g_dt_failed++;
        TEST_FAIL << "GetCurrentDateTimeTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void> test_datetime_execute(std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool   = agentxx::tools::GetCurrentDateTimeTool{agentContext};
    auto result = co_await tool.execute_async(neograph::json{});

    bool hasTimestamp = result.find("Timestamp:") != std::string::npos;
    bool hasLocalTime = result.find("Local Time (24Hour):") != std::string::npos;
    bool hasUtcTime   = result.find("UTC Time (24Hour):") != std::string::npos;

    if (hasTimestamp && hasLocalTime && hasUtcTime) {
        g_dt_passed++;
        TEST_PASS << "GetCurrentDateTimeTool returns timestamp, local time "
                     "and UTC time"
                  << std::endl;
    } else {
        g_dt_failed++;
        TEST_FAIL << "GetCurrentDateTimeTool missing fields, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_datetime_timestamp_format(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool   = agentxx::tools::GetCurrentDateTimeTool{agentContext};
    auto result = co_await tool.execute_async(neograph::json{});

    std::regex  timestampRegex(R"(Timestamp: (\d+) millisecond)");
    std::smatch match;
    if (std::regex_search(result, match, timestampRegex)) {
        auto timestamp = std::stoll(match[1].str());
        if (timestamp > 0) {
            g_dt_passed++;
            TEST_PASS << "GetCurrentDateTimeTool timestamp is a positive number" << std::endl;
        } else {
            g_dt_failed++;
            TEST_FAIL << "GetCurrentDateTimeTool timestamp should be positive" << std::endl;
        }
    } else {
        g_dt_failed++;
        TEST_FAIL << "GetCurrentDateTimeTool timestamp format incorrect, got: " << result
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_datetime_date_format(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool   = agentxx::tools::GetCurrentDateTimeTool{agentContext};
    auto result = co_await tool.execute_async(neograph::json{});

    std::regex dateRegex(R"(\d{4}-\d{2}-\d{2})");
    auto       count = size_t{0};
    auto       iter  = std::sregex_iterator(result.begin(), result.end(), dateRegex);
    auto       end   = std::sregex_iterator{};
    for (; iter != end; ++iter) {
        count++;
    }

    if (count >= 2) {
        g_dt_passed++;
        TEST_PASS << "GetCurrentDateTimeTool contains both Local and UTC "
                     "dates in YYYY-MM-DD format"
                  << std::endl;
    } else {
        g_dt_failed++;
        TEST_FAIL << "GetCurrentDateTimeTool date format incorrect, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_datetime_time_format(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool   = agentxx::tools::GetCurrentDateTimeTool{agentContext};
    auto result = co_await tool.execute_async(neograph::json{});

    std::regex timeRegex(R"(\d{2}:\d{2}:\d{2})");
    auto       count = size_t{0};
    auto       iter  = std::sregex_iterator(result.begin(), result.end(), timeRegex);
    auto       end   = std::sregex_iterator{};
    for (; iter != end; ++iter) {
        count++;
    }

    if (count >= 2) {
        g_dt_passed++;
        TEST_PASS << "GetCurrentDateTimeTool contains both Local and UTC "
                     "times in HH:MM:SS format"
                  << std::endl;
    } else {
        g_dt_failed++;
        TEST_FAIL << "GetCurrentDateTimeTool time format incorrect, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<TestResult>
    run_datetime_tool_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto run = [agentContext](auto testFn) -> asio::awaitable<void> {
        try {
            co_await testFn(agentContext);
        } catch (const std::exception& e) {
            g_dt_failed++;
            TEST_FAIL << "Exception in test: " << e.what() << std::endl;
        }
    };

    co_await run(test_datetime_get_definition);
    co_await run(test_datetime_execute);
    co_await run(test_datetime_timestamp_format);
    co_await run(test_datetime_date_format);
    co_await run(test_datetime_time_format);
    co_return TestResult{g_dt_passed, g_dt_failed};
}

} // namespace test
} // namespace agentxx
