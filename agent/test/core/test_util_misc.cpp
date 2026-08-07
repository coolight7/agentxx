#include "test_util_misc.h"

#include "agentxx/util/exception.h"
#include "agentxx/util/http_header.h"
#include "agentxx/util/util.h"
#include <stdexcept>

namespace agentxx {
namespace test {

int g_um_passed = 0;
int g_um_failed = 0;

// ---------------------------------------------------------------------------
// HeaderMap
// ---------------------------------------------------------------------------

void test_header_map_basic() {
    agentxx::util::HeaderMap hm;

    XX_TEST_EXPECT_TRUE(hm.empty());
    XX_TEST_EXPECT_FALSE(hm.contains("Content-Type"));

    // set 单值
    hm.set("Content-Type", "application/json");
    XX_TEST_EXPECT_FALSE(hm.empty());
    XX_TEST_EXPECT_TRUE(hm.contains("content-type")); // 忽略大小写
    XX_TEST_EXPECT_TRUE(hm.contains("CONTENT-TYPE"));
    XX_TEST_EXPECT_EQ(hm.getSingle("Content-Type"), std::string_view("application/json"));
    XX_TEST_EXPECT_EQ(hm.getSingle("CONTENT-TYPE"), std::string_view("application/json"));

    // 覆盖
    hm.set("content-type", "text/plain");
    XX_TEST_EXPECT_EQ(hm.getSingle("Content-Type"), std::string_view("text/plain"));

    // set 多值
    hm.set("Accept", std::vector<std::string>{"application/json", "text/html"});
    auto it = hm.get("Accept");
    XX_TEST_EXPECT_EQ(it->second.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(it->second[0], std::string("application/json"));
    XX_TEST_EXPECT_EQ(it->second[1], std::string("text/html"));
    // getSingle 返回首值
    XX_TEST_EXPECT_EQ(hm.getSingle("accept"), std::string_view("application/json"));
}

void test_header_map_get_creates() {
    agentxx::util::HeaderMap hm;
    // get 不存在的 name: 插入空 vector 并返回迭代器
    auto it = hm.get("X-New-Header");
    XX_TEST_EXPECT_TRUE(it->second.empty());
    XX_TEST_EXPECT_TRUE(hm.contains("x-new-header"));

    // 获取不存在的单值: 空串
    XX_TEST_EXPECT_EQ(hm.getSingle("X-Missing"), std::string_view(""));
    // 已存在但值为空: 空串
    XX_TEST_EXPECT_EQ(hm.getSingle("X-New-Header"), std::string_view(""));
}

void test_header_map_ctor_with_data() {
    agentxx::util::IgnoreCaseMap<std::vector<std::string>> data;
    data["Content-Type"] = {"application/json"};
    agentxx::util::HeaderMap hm{data};
    XX_TEST_EXPECT_TRUE(hm.contains("content-type"));
    XX_TEST_EXPECT_EQ(hm.getSingle("CONTENT-TYPE"), std::string_view("application/json"));
}

// ---------------------------------------------------------------------------
// catchError
// ---------------------------------------------------------------------------

void test_catch_error_success() {
    auto result = agentxx::util::catchError<int>(
        []() -> int {
            return 42;
        },
        [](std::string errmsg) -> int {
            XX_TEST_EXPECT_TRUE(false); // 不应调用
            return -1;
        }
    );
    XX_TEST_EXPECT_EQ(result, 42);
}

void test_catch_error_std_exception() {
    std::string gotErr;
    auto        result = agentxx::util::catchError<int>(
        []() -> int {
            throw std::runtime_error("boom");
        },
        [&](std::string errmsg) -> int {
            gotErr = std::move(errmsg);
            return -1;
        }
    );
    XX_TEST_EXPECT_EQ(result, -1);
    XX_TEST_EXPECT_TRUE(gotErr.find("boom") != std::string::npos);
}

void test_catch_error_unknown() {
    std::string gotErr;
    auto        result = agentxx::util::catchError<int>(
        []() -> int {
            throw 123; // 非 std::exception
        },
        [&](std::string errmsg) -> int {
            gotErr = std::move(errmsg);
            return -1;
        }
    );
    XX_TEST_EXPECT_EQ(result, -1);
    XX_TEST_EXPECT_EQ(gotErr, std::string("unknown exception"));
}

// ---------------------------------------------------------------------------
// 系统工具
// ---------------------------------------------------------------------------

void test_system_utils() {
    // getSystemName 不应为空
    auto name = agentxx::util::getSystemName();
    XX_TEST_EXPECT_FALSE(name.empty());

    // isRunningInWSL 应返回 bool (不崩溃)
    (void)agentxx::util::isRunningInWSL();
}

TestResult testUtilMisc() {
    g_um_passed = 0;
    g_um_failed = 0;

    test_header_map_basic();
    test_header_map_get_creates();
    test_header_map_ctor_with_data();
    test_catch_error_success();
    test_catch_error_std_exception();
    test_catch_error_unknown();
    test_system_utils();

    return TestResult{g_um_passed, g_um_failed};
}

} // namespace test
} // namespace agentxx
