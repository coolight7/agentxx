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

static void test_lru_cache_more() {
    // capacity=1: 每次 put 都淘汰旧值
    {
        agentxx::util::LruCache<int, int> c(1);
        c.put(1, 10);
        XX_TEST_EXPECT_EQ(c.size(), 1u);
        c.put(2, 20);
        XX_TEST_EXPECT_EQ(c.size(), 1u);
        XX_TEST_EXPECT_FALSE(c.get(1).has_value());
        XX_TEST_EXPECT_TRUE(c.get(2).has_value());
        XX_TEST_EXPECT_EQ(c.get(2).value(), 20);
    }

    // 重复 put 已存在 key: 更新值, 不增加大小, 且提升到最新
    {
        agentxx::util::LruCache<int, int> c(2);
        c.put(1, 10);
        c.put(2, 20);
        c.put(1, 100); // 更新 1
        XX_TEST_EXPECT_EQ(c.size(), 2u);
        XX_TEST_EXPECT_EQ(c.get(1).value(), 100);
        // 1 是最新, 再 put 3 应淘汰 2
        c.put(3, 30);
        XX_TEST_EXPECT_TRUE(c.get(1).has_value());
        XX_TEST_EXPECT_FALSE(c.get(2).has_value());
        XX_TEST_EXPECT_TRUE(c.get(3).has_value());
    }

    // evict: 显式驱逐最久未用
    {
        agentxx::util::LruCache<int, int> c(3);
        XX_TEST_EXPECT_FALSE(c.evict()); // 空缓存 evict 失败
        c.put(1, 10);
        c.put(2, 20);
        c.put(3, 30);
        XX_TEST_EXPECT_TRUE(c.evict()); // 驱逐 1
        XX_TEST_EXPECT_FALSE(c.get(1).has_value());
        XX_TEST_EXPECT_TRUE(c.get(2).has_value());
        XX_TEST_EXPECT_EQ(c.size(), 2u);
    }

    // 大量元素循环 + 淘汰 (压力)
    {
        agentxx::util::LruCache<int, int> c(64);
        for (int i = 0; i < 1000; ++i) {
            c.put(i, i);
        }
        XX_TEST_EXPECT_EQ(c.size(), 64u);
        // 最近 64 个保留
        XX_TEST_EXPECT_TRUE(c.get(999).has_value());
        XX_TEST_EXPECT_FALSE(c.get(0).has_value());
        XX_TEST_EXPECT_FALSE(c.get(935).has_value()); // 999-64=935 之前被淘汰
    }

    // 字符串 key
    {
        agentxx::util::LruCache<std::string, int> c(2);
        c.put("key1", 1);
        c.put("key2", 2);
        XX_TEST_EXPECT_EQ(c.get("key1").value(), 1);
        c.put("key3", 3);
        XX_TEST_EXPECT_FALSE(c.get("key2").has_value()); // key1 被访问过, 淘汰 key2
        XX_TEST_EXPECT_TRUE(c.get("key1").has_value());
        XX_TEST_EXPECT_TRUE(c.get("key3").has_value());
    }

    // 空键 / 空值
    {
        agentxx::util::LruCache<std::string, std::string> c(2);
        c.put("", "");
        XX_TEST_EXPECT_TRUE(c.get("").has_value());
        XX_TEST_EXPECT_EQ(c.get("").value(), std::string(""));
        XX_TEST_EXPECT_TRUE(c.exists(""));
    }

    // 重复 put 空串提升新鲜度
    {
        agentxx::util::LruCache<std::string, int> c(2);
        c.put("a", 1);
        c.put("b", 2);
        c.put("a", 3); // 更新
        c.put("c", 4); // 应淘汰 b
        XX_TEST_EXPECT_TRUE(c.get("a").has_value());
        XX_TEST_EXPECT_FALSE(c.get("b").has_value());
    }
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

static void test_router_more() {
    XXRouter<int, 8> router;

    // 多级精确路径
    router.add("/a/b/c", 0, std::make_shared<int>(1));
    std::string re_path;
    auto        h = router.get("/a/b/c", 0, re_path);
    XX_TEST_EXPECT_TRUE(h != nullptr);
    if (h) {
        XX_TEST_EXPECT_EQ(*h, 1);
        XX_TEST_EXPECT_EQ(re_path, std::string("/a/b/c"));
    }

    // 末尾通配符: /api/* 匹配任意子路径 (支持多级)
    router.add("/api/*", 0, std::make_shared<int>(2));
    auto hWild = router.get("/api/v1", 0, re_path);
    XX_TEST_EXPECT_TRUE(hWild != nullptr);
    if (hWild) {
        XX_TEST_EXPECT_EQ(*hWild, 2);
        XX_TEST_EXPECT_EQ(re_path, std::string("/api/*"));
    }
    // 通配符匹配多级子路径
    auto hWildMulti = router.get("/api/v1/users", 0, re_path);
    XX_TEST_EXPECT_TRUE(hWildMulti != nullptr);
    if (hWildMulti) {
        XX_TEST_EXPECT_EQ(*hWildMulti, 2);
    }
    // 不匹配无通配的路径段
    auto hNoMatch = router.get("/other/x", 0, re_path);
    XX_TEST_EXPECT_TRUE(hNoMatch == nullptr);

    // 精确路径优先于通配符
    router.add("/api/v1", 0, std::make_shared<int>(5));
    auto hExact = router.get("/api/v1", 0, re_path);
    XX_TEST_EXPECT_TRUE(hExact != nullptr);
    if (hExact) {
        XX_TEST_EXPECT_EQ(*hExact, 5);
        XX_TEST_EXPECT_EQ(re_path, std::string("/api/v1"));
    }

    // 尾部通配符: /files/* 匹配任意子路径
    router.add("/files/*", 0, std::make_shared<int>(3));
    auto hFiles = router.get("/files/a/b/c.txt", 0, re_path);
    XX_TEST_EXPECT_TRUE(hFiles != nullptr);
    if (hFiles) {
        XX_TEST_EXPECT_EQ(*hFiles, 3);
    }

    // 连续斜杠等效: add "/x//y" 后 get "/x/y" 命中
    router.add("/x//y", 0, std::make_shared<int>(4));
    auto hXY = router.get("/x/y", 0, re_path);
    XX_TEST_EXPECT_TRUE(hXY != nullptr);

    // getNocache 不走缓存, 结果一致
    auto hNC = router.getNocache("/a/b/c", 0, re_path);
    XX_TEST_EXPECT_TRUE(hNC != nullptr);
    if (hNC) {
        XX_TEST_EXPECT_EQ(*hNC, 1);
    }

    // clearCache 后 get 仍正确 (重建查找)
    router.clearCache();
    auto hAfterClearCache = router.get("/a/b/c", 0, re_path);
    XX_TEST_EXPECT_TRUE(hAfterClearCache != nullptr);

    // 指定方法索引无处理函数: 返回 nullptr, re_path 仍应有值
    auto hNoHandle = router.get("/a/b/c", 7, re_path);
    XX_TEST_EXPECT_TRUE(hNoHandle == nullptr);
    XX_TEST_EXPECT_EQ(re_path, std::string("/a/b/c"));

    // 不存在的路径: nullptr 且 re_path 清空
    auto hMissing = router.get("/not/exist", 0, re_path);
    XX_TEST_EXPECT_TRUE(hMissing == nullptr);
    XX_TEST_EXPECT_TRUE(re_path.empty());

    // 通配符路径 remove 后不再命中 (未精确注册的路径回退到通配符)
    auto removedWild = router.remove("/api/*", 0);
    XX_TEST_EXPECT_TRUE(removedWild != nullptr);
    auto hWildAfterRemove = router.get("/api/v2", 0, re_path);
    XX_TEST_EXPECT_TRUE(hWildAfterRemove == nullptr);
    // 精确注册的路径不受通配符 remove 影响
    auto hExactAfter = router.get("/api/v1", 0, re_path);
    XX_TEST_EXPECT_TRUE(hExactAfter != nullptr);

    // clear 后缓存同步失效 (UAF 回归)
    auto hBefore = router.get("/files/a.txt", 0, re_path);
    XX_TEST_EXPECT_TRUE(hBefore != nullptr);
    router.clear();
    auto hAfter = router.get("/files/a.txt", 0, re_path);
    XX_TEST_EXPECT_TRUE(hAfter == nullptr);
}

/// 最长前缀匹配: 文件夹规则对其下任意子路径生效
static void test_router_prefix_fallback() {
    XXRouter<int, 8> router;

    // 注册文件夹规则 (带结尾斜杠, 应等价于不带)
    router.add("/home/user/projects/", 0, std::make_shared<int>(10));
    // 更深的精确规则
    router.add("/home/user/projects/secret", 0, std::make_shared<int>(11));
    // 通配符规则
    router.add("/tmp/*", 1, std::make_shared<int>(12));

    std::string re_path;

    // 1. 默认模式 (无 prefix_fallback): 子路径不命中文件夹规则
    auto hNone = router.get("/home/user/projects/file.txt", 0, re_path);
    XX_TEST_EXPECT_TRUE(hNone == nullptr);

    // 2. 最长前缀匹配: 子路径回退到最深的已注册父节点
    auto hFolder = router.get("/home/user/projects/file.txt", 0, re_path, true);
    XX_TEST_EXPECT_TRUE(hFolder != nullptr);
    if (hFolder) {
        XX_TEST_EXPECT_EQ(*hFolder, 10);
        XX_TEST_EXPECT_EQ(re_path, std::string("/home/user/projects"));
    }

    // 3. 多级子路径同样回退
    auto hDeep = router.get("/home/user/projects/a/b/c.txt", 0, re_path, true);
    XX_TEST_EXPECT_TRUE(hDeep != nullptr);
    if (hDeep) {
        XX_TEST_EXPECT_EQ(*hDeep, 10);
        XX_TEST_EXPECT_EQ(re_path, std::string("/home/user/projects"));
    }

    // 4. 精确注册的更深节点优先
    auto hSecret = router.get("/home/user/projects/secret/x.txt", 0, re_path, true);
    XX_TEST_EXPECT_TRUE(hSecret != nullptr);
    if (hSecret) {
        XX_TEST_EXPECT_EQ(*hSecret, 11);
        XX_TEST_EXPECT_EQ(re_path, std::string("/home/user/projects/secret"));
    }

    // 5. 通配符优先于父级回退
    auto hTmp = router.get("/tmp/x/y.txt", 1, re_path, true);
    XX_TEST_EXPECT_TRUE(hTmp != nullptr);
    if (hTmp) {
        XX_TEST_EXPECT_EQ(*hTmp, 12);
        XX_TEST_EXPECT_EQ(re_path, std::string("/tmp/*"));
    }

    // 6. 父链回退: 中间节点无对应 index 处理函数时沿父链查找
    router.add("/home", 2, std::make_shared<int>(13));
    // /home/user/projects 仅注册了 index0, index2 需回退到 /home
    auto hChain = router.get("/home/user/projects/x.txt", 2, re_path, true);
    XX_TEST_EXPECT_TRUE(hChain != nullptr);
    if (hChain) {
        XX_TEST_EXPECT_EQ(*hChain, 13);
    }
    // 但 index0 命中更深的 /home/user/projects
    auto hChainDeep = router.get("/home/user/projects/x.txt", 0, re_path, true);
    XX_TEST_EXPECT_TRUE(hChainDeep != nullptr);
    if (hChainDeep) {
        XX_TEST_EXPECT_EQ(*hChainDeep, 10);
    }

    // 7. getNocache 同样支持前缀回退
    auto hNC = router.getNocache("/home/user/projects/file.txt", 0, re_path, true);
    XX_TEST_EXPECT_TRUE(hNC != nullptr);
    if (hNC) {
        XX_TEST_EXPECT_EQ(*hNC, 10);
    }

    // 8. 完全未注册的路径 (无任何父节点规则) 回退后仍无结果
    auto hNothing = router.get("/var/log/syslog", 0, re_path, true);
    XX_TEST_EXPECT_TRUE(hNothing == nullptr);

    // 9. 缓存一致性: 重复查询结果一致 (缓存命中路径)
    auto hCached = router.get("/home/user/projects/file.txt", 0, re_path, true);
    XX_TEST_EXPECT_TRUE(hCached != nullptr);
    if (hCached) {
        XX_TEST_EXPECT_EQ(*hCached, 10);
        XX_TEST_EXPECT_EQ(re_path, std::string("/home/user/projects"));
    }

    // 10. 父链回退命中的结果不写缓存, 再次查询仍正确
    auto hChain2 = router.get("/home/user/projects/x.txt", 2, re_path, true);
    XX_TEST_EXPECT_TRUE(hChain2 != nullptr);
    if (hChain2) {
        XX_TEST_EXPECT_EQ(*hChain2, 13);
    }

    // 11. 结尾斜杠等价: "/home/user/projects/" 命中同一节点
    auto hSlash = router.get("/home/user/projects/", 0, re_path, true);
    XX_TEST_EXPECT_TRUE(hSlash != nullptr);
    if (hSlash) {
        XX_TEST_EXPECT_EQ(*hSlash, 10);
        XX_TEST_EXPECT_EQ(re_path, std::string("/home/user/projects"));
    }

    // 12. remove 使用精确匹配, 不影响父节点规则
    auto removed = router.remove("/home/user/projects/secret", 0);
    XX_TEST_EXPECT_TRUE(removed != nullptr);
    auto hAfterRemove = router.get("/home/user/projects/secret/x.txt", 0, re_path, true);
    XX_TEST_EXPECT_TRUE(hAfterRemove != nullptr);
    if (hAfterRemove) {
        XX_TEST_EXPECT_EQ(*hAfterRemove, 10); // 回退到文件夹规则
    }

    // 13. 精确路径自身无处理函数时也可沿父链回退
    // (/home/user/projects 仅注册 index0, 查询 index2 回退到 /home)
    router.add("/home/user", 0, std::make_shared<int>(14));
    auto hExactNoHandle = router.get("/home/user/projects", 2, re_path, true);
    XX_TEST_EXPECT_TRUE(hExactNoHandle != nullptr);
    if (hExactNoHandle) {
        XX_TEST_EXPECT_EQ(*hExactNoHandle, 13);
    }
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
    test_lru_cache_more();
    test_router();
    test_router_more();
    test_router_prefix_fallback();
    test_chain_hash();

    return TestResult{g_mf_passed, g_mf_failed};
}

} // namespace test
} // namespace agentxx
