#include "test_misc_fixes.h"

#include "agentxx/agent/conversation_types.h"
#include "agentxx/agent/prompt.h"
#include "agentxx/util/lru_cache.h"
#include "agentxx/util/router.h"
#include <memory>
#include <string>

namespace agentxx {
namespace test {

int g_mf_passed = 0;
int g_mf_failed = 0;

static void test_lru_cache() {
    // capacity 0 不应崩溃 (修复: 运行时强制最小为 1)
    {
        agentxx::util::LruCache<int, int> c0(0);
        XX_TEST_EXPECT_EQ(c0.capacity(), 1u);
        c0.put(1, 100);
        auto v = c0.get(1);
        XX_TEST_EXPECT_TRUE(v.has_value());
        if (v.has_value()) {
            XX_TEST_EXPECT_EQ(v.value(), 100);
        }
    }

    // 基本 LRU 淘汰
    agentxx::util::LruCache<int, int> c(2);
    c.put(1, 10);
    c.put(2, 20);
    XX_TEST_EXPECT_EQ(c.size(), 2u);
    c.put(3, 30); // 容量满, 淘汰最久未用的 1
    XX_TEST_EXPECT_EQ(c.size(), 2u);
    XX_TEST_EXPECT_FALSE(c.get(1).has_value());
    XX_TEST_EXPECT_TRUE(c.get(2).has_value());
    XX_TEST_EXPECT_TRUE(c.get(3).has_value());

    // get 提升新鲜度: 访问 2 后, put 4 应淘汰 3 而非 2
    { auto _ = c.get(2); }
    c.put(4, 40);
    XX_TEST_EXPECT_TRUE(c.get(2).has_value());
    XX_TEST_EXPECT_FALSE(c.get(3).has_value());

    // erase
    XX_TEST_EXPECT_TRUE(c.erase(2));
    XX_TEST_EXPECT_FALSE(c.get(2).has_value());
    XX_TEST_EXPECT_FALSE(c.erase(999));

    // clear
    c.clear();
    XX_TEST_EXPECT_TRUE(c.empty());
    XX_TEST_EXPECT_EQ(c.size(), 0u);
}

static void test_router() {
    XXRouter<int, 8> router;
    router.add("/foo/bar", 0, std::make_shared<int>(42));
    router.add("/foo/*", 1, std::make_shared<int>(7));

    std::string re_path;
    auto        h = router.get("/foo/bar", 0, re_path);
    XX_TEST_EXPECT_TRUE(h != nullptr);
    if (h) {
        XX_TEST_EXPECT_EQ(*h, 42);
    }

    // 命中缓存再取一次
    auto hCached = router.get("/foo/bar", 0, re_path);
    XX_TEST_EXPECT_TRUE(hCached != nullptr);

    // 修复 #clear-UAF: clear 释放节点后必须同步清缓存, 否则 get 命中悬空指针 -> UAF
    router.clear();
    auto hAfterClear = router.get("/foo/bar", 0, re_path);
    XX_TEST_EXPECT_TRUE(hAfterClear == nullptr);

    // clear 后重新 add 仍可用
    router.add("/baz", 2, std::make_shared<int>(99));
    auto hBaz = router.get("/baz", 2, re_path);
    XX_TEST_EXPECT_TRUE(hBaz != nullptr);
    if (hBaz) {
        XX_TEST_EXPECT_EQ(*hBaz, 99);
    }

    // remove
    auto removed = router.remove("/baz", 2);
    XX_TEST_EXPECT_TRUE(removed != nullptr);
    auto hRemoved = router.get("/baz", 2, re_path);
    XX_TEST_EXPECT_TRUE(hRemoved == nullptr);
}

static void test_chain_hash() {
    agentxx::agent::ChainHash ch;
    XX_TEST_EXPECT_EQ(ch.count(), 0u);
    XX_TEST_EXPECT_EQ(ch.tail(), 0u);

    ch.append("hello");
    XX_TEST_EXPECT_EQ(ch.count(), 1u);
    auto t1 = ch.tail();
    XX_TEST_EXPECT_TRUE(t1 != 0u);

    ch.append("world");
    XX_TEST_EXPECT_EQ(ch.count(), 2u);
    auto t2 = ch.tail();
    XX_TEST_EXPECT_TRUE(t2 != t1); // 链式哈希应随追加变化

    // tailHex 为 16 位十六进制
    XX_TEST_EXPECT_EQ(ch.tailHex().size(), 16u);

    // 确定性: 相同追加序列得到相同哈希
    agentxx::agent::ChainHash ch2;
    ch2.append("hello");
    ch2.append("world");
    XX_TEST_EXPECT_EQ(ch2.tail(), t2);

    // reset
    ch.reset();
    XX_TEST_EXPECT_EQ(ch.count(), 0u);
    XX_TEST_EXPECT_EQ(ch.tail(), 0u);
}

TestResult testMiscFixes() {
    g_mf_passed = 0;
    g_mf_failed = 0;

    test_lru_cache();
    test_router();
    test_chain_hash();

    return TestResult{g_mf_passed, g_mf_failed};
}

} // namespace test
} // namespace agentxx
