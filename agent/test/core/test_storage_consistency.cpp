/// 存储一致性契约测试骨架 (计划 TST-6)
///
/// 目的: SessionStore(会话库 store 表)、share_store(会话共享存储: 内存替身与
/// "缓存 + 回库"两条路径)、settings_db(全局设置库) 在"存一条、读回来、覆盖、
/// 重开、写失败"这些**可观察语义**上不该有分歧 —— 一旦分歧, 上层会出现
/// "有的存储能返回失败、有的只能等日志"这类隐性差异。这里把语义写成一份共享
/// 用例 ([KvBackend] + [runKvContract]), 由各后端的薄适配层跑同一组断言。
///
/// 契约条目:
///   ① 未写入的键: 读回"无值", 不抛异常
///   ② 基本往返 + 覆盖写读到最新值 (ASCII / CJK / 多行 / 空串)
///   ③ 多条互不影响 (顺序读 + 逆序读, 覆盖缓存淘汰场景)
///   ④ 大值往返 (256 KB, 覆盖 spill 场景)
///   ⑤ 重开 (= 进程重启): 持久化后端保留数据; 纯内存替身明确丢弃
///   ⑥ 写失败可感知: 后端无法写入时, 要么 put 返回 false / 读回看不到该值,
///      要么提供非空失败原因; 恢复可写后继续正常工作
///
/// 已知语义差异 (写进断言, 不是漏测):
///   - 纯内存后端没有"跨重开"的持久性, 断言反过来确认"重开后确实为空";
///   - 注入持久化的 share store 可能先命中内存副本, 写失败要通过失败原因诊断
///     (见 [KvBackend::failureVisibleOnReadBack])。
#include "agentxx-test/core/test_storage_consistency.h"

#include "agentxx-test/core/test_writer_lease.h" // ForeignWriterLock

#include "agentxx/agent/session_store.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/settings_db.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_sc_passed = 0;
int g_sc_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_sc_passed
#define XX_TEST_FAILED g_sc_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

std::string makeTempRoot(std::string_view tag) {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_sc_test_{}_{}",
                   tag,
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

void removeTempRoot(const std::string& root) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::error_code ec;
        fs::remove_all(utilxx_base::utf8ToPath(root), ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

/// 一份"键值存储"后端要满足的语义接口 (薄适配层)
struct KvBackend {
    std::string name;

    /// 重开之后数据是否应当保留 (纯内存替身为 false)
    bool durable = true;

    /// 写入; 返回 false 表示后端明确拒绝了这次写入
    std::function<bool(std::string_view, std::string_view)> put;

    /// 读取; 键未写入过返回 nullopt
    std::function<std::optional<std::string>(std::string_view)> get;

    /// 关闭并重建句柄 = 模拟"进程重启后重新打开"
    std::function<void()> reopen;

    /// 写入失败时读回能否立刻看出失败
    /// - true: 失败后读回看不到本次写入的值 (或 put 直接返回 false)
    /// - false: 读回可能仍命中内存副本 (值不一定落盘), 此时必须靠 [lastError] 诊断
    bool failureVisibleOnReadBack = true;

    /// 人为制造写失败 (空 = 该后端无法制造, 契约跳过该步骤)
    std::function<void()> makeWritesFail;

    /// 恢复可写 (与 [makeWritesFail] 配对)
    std::function<void()> restoreWrites;

    /// 最近一次写失败原因 (空 = 后端不提供该诊断)
    std::function<std::string()> lastError;
};

/// 一份共享语义用例, 跑在任意 [KvBackend] 上
void runKvContract(const KvBackend& bk) {
    const std::string kAscii = "plain-ascii-value";
    const std::string kCjk   = "中文值：第一行\n第二行\t制表符";
    std::string       kHuge;
    kHuge.reserve(size_t{256} * 1024);
    while (kHuge.size() < size_t{256} * 1024) {
        kHuge += "0123456789abcdefghijklmnopqrstuvwxyz 存储一致性 大值往返\n";
    }

    TEST_INFO << "[storage-consistency] backend=" << bk.name
              << " durable=" << (bk.durable ? "true" : "false") << std::endl;

    // ① 未写入的键
    XX_TEST_EXPECT_NULLOPT(bk.get("never-written-key"));

    // ② 往返与覆盖
    XX_TEST_EXPECT_TRUE(bk.put("k1", kAscii));
    {
        auto v = bk.get("k1");
        XX_TEST_EXPECT_HAS_VALUE(v);
        if (v.has_value()) {
            XX_TEST_EXPECT_EQ(*v, kAscii);
        }
    }
    XX_TEST_EXPECT_TRUE(bk.put("k1", kCjk));
    {
        auto v = bk.get("k1");
        XX_TEST_EXPECT_HAS_VALUE(v);
        if (v.has_value()) {
            XX_TEST_EXPECT_EQ(*v, kCjk);
        }
    }

    // 空值 = "存在但内容为空", 不能与"键不存在"混为一谈
    XX_TEST_EXPECT_TRUE(bk.put("k-empty", ""));
    {
        auto v = bk.get("k-empty");
        XX_TEST_EXPECT_HAS_VALUE(v);
        if (v.has_value()) {
            XX_TEST_EXPECT_TRUE(v->empty());
        }
    }

    // ③ 多条互不影响: 顺序读 + 逆序读 (逆序读会穿过任何"最近使用"缓存的容量上限)
    constexpr int kMultiCount = 8;
    for (int i = 0; i < kMultiCount; ++i) {
        const auto key   = fmt::format("multi-{}", i);
        const auto value = fmt::format("value-{}", i);
        XX_TEST_EXPECT_TRUE(bk.put(key, value));
    }
    for (int i = 0; i < kMultiCount; ++i) {
        auto v = bk.get(fmt::format("multi-{}", i));
        XX_TEST_EXPECT_HAS_VALUE(v);
        if (v.has_value()) {
            XX_TEST_EXPECT_EQ(*v, fmt::format("value-{}", i));
        }
    }
    for (int i = kMultiCount - 1; i >= 0; --i) {
        auto v = bk.get(fmt::format("multi-{}", i));
        XX_TEST_EXPECT_HAS_VALUE(v);
        if (v.has_value()) {
            XX_TEST_EXPECT_EQ(*v, fmt::format("value-{}", i));
        }
    }

    // ④ 大值
    XX_TEST_EXPECT_TRUE(bk.put("k-huge", kHuge));
    {
        auto v = bk.get("k-huge");
        XX_TEST_EXPECT_HAS_VALUE(v);
        if (v.has_value()) {
            XX_TEST_EXPECT_EQ(v->size(), kHuge.size());
            XX_TEST_EXPECT_TRUE(*v == kHuge);
        }
    }

    // ⑤ 重开 = 进程重启
    bk.reopen();
    if (bk.durable) {
        auto v = bk.get("k1");
        XX_TEST_EXPECT_HAS_VALUE(v);
        if (v.has_value()) {
            XX_TEST_EXPECT_EQ(*v, kCjk);
        }
        auto m3 = bk.get("multi-3");
        XX_TEST_EXPECT_HAS_VALUE(m3);
        if (m3.has_value()) {
            XX_TEST_EXPECT_EQ(*m3, std::string{"value-3"});
        }
        auto huge = bk.get("k-huge");
        XX_TEST_EXPECT_HAS_VALUE(huge);
        if (huge.has_value()) {
            XX_TEST_EXPECT_EQ(huge->size(), kHuge.size());
        }
        // 重开后仍然能写新条目
        XX_TEST_EXPECT_TRUE(bk.put("k-after-reopen", "post-restart"));
        auto post = bk.get("k-after-reopen");
        XX_TEST_EXPECT_HAS_VALUE(post);
        if (post.has_value()) {
            XX_TEST_EXPECT_EQ(*post, std::string{"post-restart"});
        }
    } else {
        // 纯内存替身: 重启后一切皆无 (语义明确, 不是"偶发丢失")
        XX_TEST_EXPECT_NULLOPT(bk.get("k1"));
        XX_TEST_EXPECT_NULLOPT(bk.get("multi-3"));
    }

    // ⑥ 写失败可感知
    if (!bk.makeWritesFail) {
        TEST_INFO << "[storage-consistency] " << bk.name
                  << ": 写失败步骤不适用 (该后端无法人为制造写入失败)" << std::endl;
        return;
    }
    bk.makeWritesFail();
    const bool ok      = bk.put("k-fail", "fail-value");
    const auto after   = bk.get("k-fail");
    const bool visible = after.has_value() && *after == "fail-value";
    if (bk.failureVisibleOnReadBack) {
        // 不能"报告成功且读回还是新值"却其实没落盘
        XX_TEST_EXPECT_TRUE(!ok || !visible);
    } else {
        // 先写内存副本再落库: 缓存里能看到刚写的值 (既定语义, 不是漏写)
        XX_TEST_EXPECT_TRUE(visible);
    }
    if (bk.lastError) {
        XX_TEST_EXPECT_FALSE(bk.lastError().empty());
    }
    if (bk.restoreWrites) {
        bk.restoreWrites();
        if (!bk.failureVisibleOnReadBack && bk.durable) {
            // 恢复可写后重新打开: 写失败那一次的内容确实没有落盘
            bk.reopen();
            auto persisted = bk.get("k-fail");
            XX_TEST_EXPECT_TRUE(!persisted.has_value() || *persisted != "fail-value");
        }
        XX_TEST_EXPECT_TRUE(bk.put("k-recovered", "recovered-value"));
        auto v = bk.get("k-recovered");
        XX_TEST_EXPECT_HAS_VALUE(v);
        if (v.has_value()) {
            XX_TEST_EXPECT_EQ(*v, std::string{"recovered-value"});
        }
    }
}

// ---------------------------------------------------------------------------
// 后端 1: settings_db (全局设置库)
// ---------------------------------------------------------------------------

struct SettingsState {
    std::string                                goodPath;
    std::string                                blockedPath;
    std::unique_ptr<agentxx::util::SettingsDb> db;
};

/// settings_db 的写失败形态: 数据库路径的父目录被一个普通文件占住 (打开必失败),
/// 写接口因此返回 false、读接口返回 nullopt —— 失败对调用方直接可见
KvBackend makeSettingsBackend(const std::string& root) {
    auto st      = std::make_shared<SettingsState>();
    st->goodPath = (fs::path{root} / "global.db").string();
    const auto block = fs::path{root} / "blocker";
    {
        std::ofstream ofs(utilxx_base::utf8ToPath(block.string()), std::ios::binary);
        ofs << "not a directory";
    }
    st->blockedPath = (block / "global.db").string();
    st->db          = std::make_unique<agentxx::util::SettingsDb>(st->goodPath);

    KvBackend bk;
    bk.name      = "settings_db";
    bk.durable   = true;
    bk.put       = [st](std::string_view k, std::string_view v) { return st->db->set(k, v); };
    bk.get       = [st](std::string_view k) { return st->db->get(k); };
    bk.reopen    = [st] { st->db = std::make_unique<agentxx::util::SettingsDb>(st->goodPath); };
    bk.makeWritesFail = [st] {
        st->db = std::make_unique<agentxx::util::SettingsDb>(st->blockedPath);
    };
    bk.restoreWrites = [st] {
        st->db = std::make_unique<agentxx::util::SettingsDb>(st->goodPath);
    };
    // 失败原因只走日志, 端口本身只返回 false: 不提供原因字符串
    bk.lastError = nullptr;
    return bk;
}

// ---------------------------------------------------------------------------
// 后端 2: SessionStore 的 store 表 (会话共享存储的持久化后端)
// ---------------------------------------------------------------------------

struct StoreState {
    std::string                        root;
    std::string                        sessionId;
    std::unique_ptr<agentxx::agent::SessionStore> store;
    std::map<std::string, size_t>      ids;
    std::unique_ptr<ForeignWriterLock> foreign; ///< 模拟"另一个进程正在写"
};

KvBackend makeSessionStoreBackend(const std::string& root) {
    auto st        = std::make_shared<StoreState>();
    st->root       = (fs::path{root} / "sessions").string();
    st->sessionId  = "storage-contract";
    st->store      = std::make_unique<agentxx::agent::SessionStore>(st->root);

    KvBackend bk;
    bk.name    = "session_store.store";
    bk.durable = true;
    bk.put     = [st](std::string_view k, std::string_view v) {
        const auto key = std::string{k};
        auto       it  = st->ids.find(key);
        if (it == st->ids.end()) {
            // 新条目: 由库分配 id (与生产代码同一条路径)
            const auto id = st->store->addShareStoreItem(st->sessionId, v);
            if (id == 0) {
                return false; // 分配失败 (目录被占/库拒绝打开) = 后端明确拒绝写入
            }
            st->ids.emplace(key, id);
            return true;
        }
        st->store->setShareStoreItem(st->sessionId, it->second, v);
        return true;
    };
    bk.get = [st](std::string_view k) -> std::optional<std::string> {
        auto it = st->ids.find(std::string{k});
        if (it == st->ids.end()) {
            return std::nullopt;
        }
        return st->store->getShareStoreItem(st->sessionId, it->second);
    };
    bk.reopen = [st] {
        // 重开 = 换一个 SessionStore 实例 (旧连接与写租约随之释放)
        st->store = std::make_unique<agentxx::agent::SessionStore>(st->root);
    };
    bk.makeWritesFail = [st] {
        const auto dir = (fs::path{st->root} / st->sessionId).string();
        fs::create_directories(utilxx_base::utf8ToPath(dir));
        st->store.reset(); // 先释放本进程写租约, 保证外部独占锁能拿到
        st->foreign = std::make_unique<ForeignWriterLock>(dir);
        if (!st->foreign->held()) {
            throw std::runtime_error{"storage_consistency: 无法取得外部写锁 (测试夹具失效)"};
        }
        // 换一个新库实例: 它的写连接拿不到租约, 写入必失败且记录失败原因
        st->store = std::make_unique<agentxx::agent::SessionStore>(st->root);
    };
    bk.restoreWrites = [st] {
        st->foreign.reset();
        st->store = std::make_unique<agentxx::agent::SessionStore>(st->root);
    };
    bk.lastError = [st]() { return st->store ? st->store->lastWriteError() : std::string{}; };
    return bk;
}

// ---------------------------------------------------------------------------
// 后端 3/4: share store 内存替身 / 缓存 + 回库
// ---------------------------------------------------------------------------

struct ShareStoreState {
    std::string                                             sessionId;
    std::map<std::string, size_t>                           ids;
    std::shared_ptr<agentxx::middleware::MiddlewareContext> ctx;
    std::shared_ptr<agentxx::agent::SessionStore>           store; ///< 空 = 纯内存替身
    std::unique_ptr<ForeignWriterLock>                      foreign;
    std::string                                             root;
};

KvBackend makeShareStoreBackend(const std::string& root, bool withPersistence) {
    auto st       = std::make_shared<ShareStoreState>();
    st->root      = root;
    st->sessionId = withPersistence ? "share-cached" : "share-memory";
    st->store     = withPersistence ? std::make_shared<agentxx::agent::SessionStore>(root) : nullptr;
    st->ctx       = std::make_shared<agentxx::middleware::MiddlewareContext>(st->store);

    KvBackend bk;
    bk.name    = withPersistence ? "share_store.cached" : "share_store.memory";
    bk.durable = withPersistence;
    bk.put     = [st](std::string_view k, std::string_view v) {
        const auto key = std::string{k};
        auto       it  = st->ids.find(key);
        if (it == st->ids.end()) {
            const auto id = st->ctx->addShareStoreItemValue(st->sessionId, v);
            if (id == 0) {
                return false;
            }
            st->ids.emplace(key, id);
            return true;
        }
        st->ctx->setShareStoreItemValue(st->sessionId, it->second, v);
        return true;
    };
    bk.get = [st](std::string_view k) -> std::optional<std::string> {
        auto it = st->ids.find(std::string{k});
        if (it == st->ids.end()) {
            return std::nullopt;
        }
        try {
            return st->ctx->getShareStoreItemValue(st->sessionId, it->second);
        } catch (const std::exception&) {
            // 落盘失败时按内存计数分配的 id 在库里并不存在, 新上下文按"id 未分配"
            // 拒绝读取 (抛参数错误); 本契约按"读不到"处理, 与"没落盘"的结论一致
            return std::nullopt;
        }
    };
    if (!withPersistence) {
        // 纯内存替身: 重开 = 新上下文, 值没了, id 映射也没有意义
        bk.reopen = [st] {
            st->ids.clear();
            st->ctx = std::make_shared<agentxx::middleware::MiddlewareContext>(st->store);
        };
        return bk;
    }
    // 注入持久化: 重开只清内存缓存 (键 -> id 的映射是"应用侧"的知识, 重启后仍在)
    bk.reopen = [st] {
        st->ctx = std::make_shared<agentxx::middleware::MiddlewareContext>(st->store);
    };
    // 写入先落内存副本再落库: 落库失败时读回仍能命中内存副本, 必须靠失败原因诊断
    bk.failureVisibleOnReadBack = false;
    bk.makeWritesFail           = [st] {
        const auto dir = (fs::path{st->root} / st->sessionId).string();
        fs::create_directories(utilxx_base::utf8ToPath(dir));
        // 先丢掉持有旧库的上下文, 再丢库本身: 否则旧库的写租约不释放, 外部锁拿不到
        st->ctx = std::make_shared<agentxx::middleware::MiddlewareContext>(
            std::shared_ptr<agentxx::agent::SessionStore>{}
        );
        st->store.reset();
        st->foreign = std::make_unique<ForeignWriterLock>(dir);
        if (!st->foreign->held()) {
            throw std::runtime_error{"storage_consistency: 无法取得外部写锁 (测试夹具失效)"};
        }
        // 新库实例: 写连接拿不到租约 -> 分配 id/落库都失败并记录原因
        st->store = std::make_shared<agentxx::agent::SessionStore>(st->root);
        st->ctx   = std::make_shared<agentxx::middleware::MiddlewareContext>(st->store);
    };
    bk.restoreWrites = [st] {
        st->foreign.reset();
        st->store = std::make_shared<agentxx::agent::SessionStore>(st->root);
        st->ctx   = std::make_shared<agentxx::middleware::MiddlewareContext>(st->store);
    };
    bk.lastError = [st]() { return st->store ? st->store->lastWriteError() : std::string{}; };
    return bk;
}

} // namespace

TestResult testStorageConsistency() {
    const int passedBefore = g_sc_passed;
    const int failedBefore = g_sc_failed;

    // 后端 1: settings_db (写失败用"父目录被文件占住"制造)
    {
        const auto root = makeTempRoot("settings");
        runKvContract(makeSettingsBackend(root));
        removeTempRoot(root);
    }
    // 后端 2: SessionStore 的 store 表 (跨实例重开 + 外部持锁制造写失败)
    {
        const auto root = makeTempRoot("store");
        runKvContract(makeSessionStoreBackend(root));
        removeTempRoot(root);
    }
    // 后端 3: share store 纯内存替身 (重开即丢, 无法制造写失败)
    {
        const auto root = makeTempRoot("share-memory");
        runKvContract(makeShareStoreBackend(root, false));
        removeTempRoot(root);
    }
    // 后端 4: share store 缓存 + 回库 (重开后必须从库读回, 覆盖缓存淘汰与回读路径)
    {
        const auto root = makeTempRoot("share-cached");
        runKvContract(makeShareStoreBackend(root, true));
        removeTempRoot(root);
    }

    return TestResult{g_sc_passed - passedBefore, g_sc_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
