#include "test_util_misc.h"

#include "agentxx/util/container_util.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/http_header.h"
#include "agentxx/util/stream.h"
#include "agentxx/util/util.h"
#include <chrono>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_um_passed = 0;
int g_um_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_um_passed
#define XX_TEST_FAILED g_um_failed

namespace agentxx {
namespace test {

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

// ---------------------------------------------------------------------------
// util/stream.h: Throttle (节流) / Debounce (防抖)
// ---------------------------------------------------------------------------
void test_stream_throttle_debounce() {
    using namespace std::chrono;

    // --- Throttle: 最小放行间隔 ---
    {
        agentxx::util::Throttle throttle(seconds{1});
        // 首次调用恒放行
        XX_TEST_EXPECT_TRUE(throttle.try_acquire());
        // 间隔内不放行
        XX_TEST_EXPECT_FALSE(throttle.try_acquire());
        // 剩余时间在 (0, interval] 内
        auto remaining = throttle.time_until_acquire();
        XX_TEST_EXPECT_TRUE(remaining > std::chrono::steady_clock::duration::zero());
        XX_TEST_EXPECT_TRUE(remaining <= seconds{1});
        // 等待满间隔后放行
        std::this_thread::sleep_for(milliseconds{1100});
        XX_TEST_EXPECT_TRUE(throttle.try_acquire());
    }
    {
        // force() 计为一次放行: 之后立即 try_acquire 不放行
        agentxx::util::Throttle throttle(seconds{1});
        throttle.force();
        XX_TEST_EXPECT_FALSE(throttle.try_acquire());
        // 从未放行时 time_until_acquire 为 0
        agentxx::util::Throttle fresh(seconds{1});
        XX_TEST_EXPECT_TRUE(
            fresh.time_until_acquire() == std::chrono::steady_clock::duration::zero()
        );
    }

    // --- Debounce: 静默满 wait 后才 ready, 期间触发重置计时 ---
    {
        agentxx::util::Debounce debounce(milliseconds{200});
        // 未触发过: 不 ready
        XX_TEST_EXPECT_FALSE(debounce.ready());
        debounce.trigger();
        std::this_thread::sleep_for(milliseconds{100});
        debounce.trigger(); // 重置计时
        std::this_thread::sleep_for(milliseconds{150});
        // 距最后一次触发 150ms < 200ms: 不 ready
        XX_TEST_EXPECT_FALSE(debounce.ready());
        std::this_thread::sleep_for(milliseconds{100});
        // 静默满 200ms: ready
        XX_TEST_EXPECT_TRUE(debounce.ready());
        debounce.reset();
        XX_TEST_EXPECT_FALSE(debounce.ready());
    }
}

// ---------------------------------------------------------------------------
// util/container_util.h 异构关联容器操作测试
// ---------------------------------------------------------------------------
void test_container_util_heterogeneous() {
    // 1. eraseHeterogeneous
    {
        std::map<std::string, int, std::less<>> m;
        m["hello"] = 42;
        m["world"] = 100;

        XX_TEST_EXPECT_TRUE(agentxx::util::eraseHeterogeneous(m, std::string_view("hello")));
        XX_TEST_EXPECT_EQ(m.size(), (size_t)1);
        XX_TEST_EXPECT_FALSE(agentxx::util::eraseHeterogeneous(m, std::string_view("non_exist")));
        XX_TEST_EXPECT_EQ(m.size(), (size_t)1);

        std::set<std::string, std::less<>> s{"apple", "banana"};
        XX_TEST_EXPECT_TRUE(agentxx::util::eraseHeterogeneous(s, std::string_view("apple")));
        XX_TEST_EXPECT_FALSE(agentxx::util::eraseHeterogeneous(s, std::string_view("orange")));
        XX_TEST_EXPECT_EQ(s.size(), (size_t)1);
    }

    // 2. insertHeterogeneous
    {
        std::map<std::string, std::string, std::less<>> m;
        // string_view 参数
        auto [it1, ok1] = agentxx::util::insertHeterogeneous(m, "k1", "v1");
        XX_TEST_EXPECT_TRUE(ok1);
        XX_TEST_EXPECT_EQ(it1->second, std::string("v1"));

        // 重复 key 不覆盖
        auto [it2, ok2] = agentxx::util::insertHeterogeneous(m, "k1", "v2_ignored");
        XX_TEST_EXPECT_FALSE(ok2);
        XX_TEST_EXPECT_EQ(it2->second, std::string("v1"));

        // 右值 string (move 复用)
        std::string moveKey = "k2";
        std::string moveVal = "v2";
        auto [it3, ok3]
            = agentxx::util::insertHeterogeneous(m, std::move(moveKey), std::move(moveVal));
        XX_TEST_EXPECT_TRUE(ok3);
        XX_TEST_EXPECT_EQ(it3->second, std::string("v2"));

        // set 测试
        std::set<std::string, std::less<>> s;
        auto [sit1, sok1] = agentxx::util::insertHeterogeneous(s, "item1");
        XX_TEST_EXPECT_TRUE(sok1);
        auto [sit2, sok2] = agentxx::util::insertHeterogeneous(s, "item1");
        XX_TEST_EXPECT_FALSE(sok2);
    }

    // 3. insertOrAssignHeterogeneous / overwriteHeterogeneous
    {
        std::map<std::string, int, std::less<>> m;
        // 新建插入
        auto [it1, inserted1]
            = agentxx::util::insertOrAssignHeterogeneous(m, std::string_view("score"), 100);
        XX_TEST_EXPECT_TRUE(inserted1);
        XX_TEST_EXPECT_EQ(it1->second, 100);

        // 已存在时覆盖
        auto [it2, inserted2]
            = agentxx::util::insertOrAssignHeterogeneous(m, std::string_view("score"), 200);
        XX_TEST_EXPECT_FALSE(inserted2);
        XX_TEST_EXPECT_EQ(it2->second, 200);
        XX_TEST_EXPECT_EQ(m["score"], 200);

        // overwriteHeterogeneous 别名测试
        auto [it3, inserted3] = agentxx::util::overwriteHeterogeneous(m, "score", 300);
        XX_TEST_EXPECT_FALSE(inserted3);
        XX_TEST_EXPECT_EQ(it3->second, 300);
        XX_TEST_EXPECT_EQ(m["score"], 300);

        // 右值 string 覆盖
        std::string rvalKey   = "rkey";
        auto [it4, inserted4] = agentxx::util::overwriteHeterogeneous(m, std::move(rvalKey), 400);
        XX_TEST_EXPECT_TRUE(inserted4);
        XX_TEST_EXPECT_EQ(it4->second, 400);
    }

    // 4. getOrCreateHeterogeneous
    {
        std::map<std::string, std::vector<int>, std::less<>> m;
        // 不存在则默认构造插入
        auto& vec = agentxx::util::getOrCreateHeterogeneous(m, std::string_view("numbers"));
        XX_TEST_EXPECT_TRUE(vec.empty());
        vec.push_back(1);
        vec.push_back(2);

        // 再次获取得到相同引用
        auto& vec2 = agentxx::util::getOrCreateHeterogeneous(m, "numbers");
        XX_TEST_EXPECT_EQ(vec2.size(), (size_t)2);
        XX_TEST_EXPECT_EQ(vec2[0], 1);
        XX_TEST_EXPECT_EQ(vec2[1], 2);
    }
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
    test_stream_throttle_debounce();
    test_container_util_heterogeneous();

    return TestResult{g_um_passed, g_um_failed};
}

} // namespace test
} // namespace agentxx
