#include "agentxx-test/core/test_persist_semantics.h"
#include "agentxx-test/core/test_writer_lease.h" // ForeignWriterLock

#include "agentxx/agent/context.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <asio/awaitable.hpp>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ps_passed = 0;
int g_ps_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ps_passed
#define XX_TEST_FAILED g_ps_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

using agentxx::agent::Session;
using agentxx::agent::SessionStore;
using agentxx::agent::SessionsManager;
using agentxx::agent::ViewMessage;

/// 创建唯一临时目录
std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_ps_test_{}",
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

ViewMessage makeMsg(std::string id, std::string text) {
    ViewMessage msg;
    msg.id          = std::move(id);
    msg.role        = ViewMessage::Role::User;
    msg.text        = std::move(text);
    msg.startTimeMs = 1700000000000LL;
    return msg;
}

/// 记录收到的增量 (验证降级提示只推一条 MessageUITip)
class RecordingIO : public agentxx::agent::AgentIOBase {
public:

    std::vector<agentxx::agent::WireDelta> deltas;

    /// 提示经 [sendToPeer] 发出 (WireDelta::MessageUITip): 直接记录, 不做传输
    void sendToPeer(agentxx::agent::WireMessage msg) override {
        std::visit(
            [this](auto&& m) {
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, agentxx::agent::WireDelta>) {
                    deltas.push_back(m);
                }
            },
            std::move(msg)
        );
    }

    void onDelta(const agentxx::agent::WireDelta& delta) override {
        deltas.push_back(delta);
    }

    void onSync(const agentxx::agent::WireSyncPayload&) override {}

    asio::awaitable<std::optional<std::string>> getInput() override {
        co_return std::nullopt;
    }

    asio::awaitable<utilxx_base::Json>
        handleInterrupt(std::string_view, std::string_view, std::string_view, std::string_view)
            override {
        co_return utilxx_base::Json{};
    }

    /// 统计 MessageUITip 条数
    size_t tipCount() const {
        size_t n = 0;
        for (const auto& d : deltas) {
            if (d.type == agentxx::agent::WireDelta::Type::MessageUITip) {
                ++n;
            }
        }
        return n;
    }

    std::string lastTipText() const {
        for (auto it = deltas.rbegin(); it != deltas.rend(); ++it) {
            if (it->type == agentxx::agent::WireDelta::Type::MessageUITip) {
                return it->text;
            }
        }
        return {};
    }
};

/// 计数用的假持久化回调 (只统计调用次数, 不落盘)
struct HookCounters {
    int append = 0;
    int update = 0;
    int llm    = 0;
};

agentxx::agent::SessionStoreHooks countingHooks(HookCounters* c) {
    return agentxx::agent::SessionStoreHooks{
        .onAppendViewMessage =
            [c](const ViewMessage&, uint64_t, uint64_t) {
                ++c->append;
            },
        .onUpdateViewMessage =
            [c](const ViewMessage&) {
                ++c->update;
            },
        .onSaveLlmMessages =
            [c](const utilxx_base::Json&) {
                ++c->llm;
            },
    };
}

// ---------------------------------------------------------------------------
// 1. 落盘分级 (计划 STO-5): persistNow 立即写, persistThrottled 走节流窗口
// ---------------------------------------------------------------------------

void test_persist_levels() {
    HookCounters counters;

    Session session;
    session.setStoreHooks(countingHooks(&counters));

    // 首条展示消息: 首次触发节流窗口 → 立即落盘
    session.appendViewMessage(makeMsg("m1", "hello"));
    XX_TEST_EXPECT_EQ(counters.append, 1);

    // 窗口内追加: 只入待落盘队列, 不立即写
    session.appendViewMessage(makeMsg("m2", "world"));
    XX_TEST_EXPECT_EQ(counters.append, 1);

    // 上下文变更 (不落盘) + persistNow: 上下文与展示历史一起立即落盘
    neograph::ChatMessage userMsg;
    {
        utilxx_base::Json j{{"role", "user"}, {"content", "hi"}};
        neograph::from_json(agentxx::util::toNeographJson(j), userMsg);
    }
    session.appendMessages({userMsg}, false);
    XX_TEST_EXPECT_EQ(counters.llm, 0);
    session.persistNow("user-input");
    XX_TEST_EXPECT_EQ(counters.llm, 1);   // 用户输入立即写上下文
    XX_TEST_EXPECT_EQ(counters.append, 2); // 待落盘展示消息一并刷出

    // 紧接着 persistThrottled: 上下文仍在节流窗口内 → 不重复写
    session.appendMessages({userMsg}, false);
    session.persistThrottled("llm-output");
    XX_TEST_EXPECT_EQ(counters.llm, 1);

    // 展示历史追加 + persistThrottled: 窗口内不写
    session.appendViewMessage(makeMsg("m3", "third"));
    session.persistThrottled("llm-output");
    XX_TEST_EXPECT_EQ(counters.append, 2);

    // persistNow 不受节流窗口限制 (轮次终态 / 工具结算 / 压缩完成)
    session.persistNow("turn-end");
    XX_TEST_EXPECT_EQ(counters.llm, 2);
    XX_TEST_EXPECT_EQ(counters.append, 3);

    // 内存模式 (无 hooks): persistNow/persistThrottled 均为 no-op (不崩)
    Session plain;
    plain.appendViewMessage(makeMsg("p1", "no persistence"));
    plain.persistNow("turn-end");
    plain.persistThrottled("llm-output");
    XX_TEST_EXPECT_TRUE(plain.lastViewSeq() == 1);
}

// ---------------------------------------------------------------------------
// 2. 展示历史序号 (计划 STO-4): 会话分配的序号与库内一致, 恢复后继续编号
// ---------------------------------------------------------------------------

void test_view_seq_recovery() {
    const auto        root = makeTempRoot();
    const std::string sid  = "ps-seq";

    {
        SessionsManager mgr;
        mgr.sessionStore = std::make_shared<SessionStore>(root);
        auto session     = mgr.getOrCreate(sid);
        XX_TEST_EXPECT_EQ(session->lastViewSeq(), uint64_t{0});

        session->appendViewMessage(makeMsg("m1", "one"));
        session->appendViewMessage(makeMsg("m2", "two"));
        XX_TEST_EXPECT_EQ(session->lastViewSeq(), uint64_t{2});
        session->persistNow("test");
    }
    // 同一进程内重建会话 (等价重启恢复): 序号从库内最大值继续
    {
        SessionsManager mgr;
        mgr.sessionStore = std::make_shared<SessionStore>(root);
        auto session     = mgr.getOrCreate(sid);
        XX_TEST_EXPECT_EQ(session->viewMessageCount(), size_t{2});
        XX_TEST_EXPECT_EQ(session->lastViewSeq(), uint64_t{2});

        session->appendViewMessage(makeMsg("m3", "three"));
        XX_TEST_EXPECT_EQ(session->lastViewSeq(), uint64_t{3});
        session->persistNow("test");

        // 增量补拉: 只回补序号大于 2 的消息
        auto rows = mgr.sessionStore->loadViewMessagesAfter(sid, 2);
        XX_TEST_EXPECT_EQ(rows.size(), size_t{1});
        if (rows.size() == 1) {
            XX_TEST_EXPECT_EQ(rows[0].seq, uint64_t{3});
            XX_TEST_EXPECT_EQ(rows[0].message.text, std::string{"three"});
        }
    }

    removeTempRoot(root);
}

// ---------------------------------------------------------------------------
// 3. 持久化降级提示 (计划 STO-9): 写失败提示一次, 同原因不重复, 恢复后不提示
// ---------------------------------------------------------------------------

void test_persist_degradation_tip() {
    const auto        root = makeTempRoot();
    const std::string sid  = "ps-degraded";
    const auto        dir  = (fs::path{root} / sid).string();

    SessionsManager mgr;
    mgr.sessionStore = std::make_shared<SessionStore>(root);
    auto session     = mgr.getOrCreate(sid);
    auto io          = std::make_shared<RecordingIO>();
    session->io      = io;

    {
        // 目录被"其他进程"占用 → 写连接失败 → 提示一次
        ForeignWriterLock foreign{dir};
        XX_TEST_EXPECT_TRUE(foreign.held());

        session->appendViewMessage(makeMsg("d1", "not saved"));
        session->persistNow("user-input");
        XX_TEST_EXPECT_EQ(io->tipCount(), 1);
        XX_TEST_EXPECT_TRUE(io->lastTipText().find("Persistence degraded") != std::string::npos);

        // 同一原因不再重复提示
        session->appendViewMessage(makeMsg("d2", "still not saved"));
        session->persistNow("turn-end");
        XX_TEST_EXPECT_EQ(io->tipCount(), 1);
    }

    // 外部锁释放后恢复正常写入: 不再提示
    session->appendViewMessage(makeMsg("d3", "saved"));
    session->persistNow("turn-end");
    XX_TEST_EXPECT_EQ(io->tipCount(), 1);

    // 库内确实落盘了锁释放后的消息 (前两条因写失败丢失, 不静默假装成功)
    auto loaded = mgr.sessionStore->loadSession(sid);
    XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{1});
    if (!loaded.viewMessages.empty()) {
        XX_TEST_EXPECT_EQ(loaded.viewMessages[0].text, std::string{"saved"});
    }

    removeTempRoot(root);
}

} // namespace

TestResult testPersistSemantics() {
    test_persist_levels();
    test_view_seq_recovery();
    test_persist_degradation_tip();
    return TestResult{g_ps_passed, g_ps_failed};
}

} // namespace test
} // namespace agentxx
