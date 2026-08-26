#include "test_session_persistence.h"
#include "test_agent.h"

#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/log.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <neograph/json.h>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_sp_passed = 0;
int g_sp_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_sp_passed
#define XX_TEST_FAILED g_sp_failed
namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

/// 创建唯一临时目录 (测试根目录), 返回路径; 由调用方在测试结束 remove_all
std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_sp_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

/// 构造一条带角色专属字段的测试消息
agentxx::agent::ViewMessage makeMsg(agentxx::agent::ViewMessage::Role role, std::string text) {
    using V = agentxx::agent::ViewMessage;
    V msg;
    msg.role        = role;
    msg.text        = std::move(text);
    msg.startTimeMs = 1700000000000LL;
    msg.durationMs  = 123;
    switch (role) {
        case V::Role::Tool: {
            V::ToolData t;
            t.toolName     = "agentxx_share_store";
            t.toolCallId   = "call_001";
            t.toolResult   = "ok";
            t.diff         = "--- a\n+++ b";
            t.toolFinished = true;
            msg.tool       = std::move(t);
            break;
        }
        case V::Role::Tip: {
            V::TipData s;
            s.tipLevel = V::TipLevel::Warning;
            msg.tip    = std::move(s);
            break;
        }
        case V::Role::Interrupt: {
            V::InterruptData it;
            it.interruptId     = 7;
            it.inputLabel      = "choice";
            it.inputDepict     = "pick one";
            it.inputType       = "enum";
            it.inputDefault    = "b";
            it.inputEnums      = {"a", "b", "c"};
            it.inputIndex      = 1;
            it.inputTotal      = 2;
            it.interruptStatus = V::InterruptStatus::Confirmed;
            it.interruptResult = "b";
            msg.interrupt      = std::move(it);
            break;
        }
        default:
            break;
    }
    return msg;
}

} // namespace

// ---------------------------------------------------------------------------
// SessionStore 单测: viewMessages / llmMessages / meta
// ---------------------------------------------------------------------------

static TestResult testViewMessagesRoundtrip() {
    using agentxx::agent::SessionStore;
    using V = agentxx::agent::ViewMessage;

    auto root = makeTempRoot();
    {
        // 写入: user/assistant/tool/system/thinking/interrupt 各一条
        auto p = std::make_shared<SessionStore>(root);
        // 只读访问不创建目录/空文件
        auto fresh = p->loadSession("not-exist-yet");
        XX_TEST_EXPECT_TRUE(fresh.viewMessages.empty());
        XX_TEST_EXPECT_TRUE(!fs::exists(fs::path(root) / "not-exist-yet"));

        std::vector<V> msgs{
            makeMsg(V::Role::User, "hello"),
            makeMsg(V::Role::Assistant, "hi there"),
            makeMsg(V::Role::Tool, R"({"tool":"x"})"),
            makeMsg(V::Role::Tip, "tip"),
            makeMsg(V::Role::Think, "think"),
            makeMsg(V::Role::Interrupt, "interrupt-payload"),
        };
        for (size_t i = 0; i < msgs.size(); ++i) {
            p->appendViewMessage("t1", msgs[i], i + 1);
        }
        // 同一实例读回
        auto loaded = p->loadSession("t1");
        XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), msgs.size());
        if (loaded.viewMessages.size() == msgs.size()) {
            for (size_t i = 0; i < msgs.size(); ++i) {
                XX_TEST_EXPECT_EQ(loaded.viewMessages[i].role, msgs[i].role);
                XX_TEST_EXPECT_EQ(loaded.viewMessages[i].text, msgs[i].text);
                XX_TEST_EXPECT_EQ(loaded.viewMessages[i].startTimeMs, msgs[i].startTimeMs);
                XX_TEST_EXPECT_EQ(loaded.viewMessages[i].durationMs, msgs[i].durationMs);
            }
        }
        // 角色专属字段往返
        const auto& toolMsg = loaded.viewMessages[2];
        XX_TEST_EXPECT_TRUE(toolMsg.tool.has_value());
        if (toolMsg.tool) {
            XX_TEST_EXPECT_EQ(toolMsg.tool->toolName, "agentxx_share_store");
            XX_TEST_EXPECT_EQ(toolMsg.tool->toolCallId, "call_001");
            XX_TEST_EXPECT_EQ(toolMsg.tool->toolResult, "ok");
            XX_TEST_EXPECT_EQ(toolMsg.tool->diff, "--- a\n+++ b");
            XX_TEST_EXPECT_TRUE(toolMsg.tool->toolFinished);
        }
        const auto& sysMsg = loaded.viewMessages[3];
        XX_TEST_EXPECT_TRUE(sysMsg.tip.has_value());
        if (sysMsg.tip) {
            XX_TEST_EXPECT_EQ(sysMsg.tip->tipLevel, V::TipLevel::Warning);
        }
        const auto& intMsg = loaded.viewMessages[5];
        XX_TEST_EXPECT_TRUE(intMsg.interrupt.has_value());
        if (intMsg.interrupt) {
            XX_TEST_EXPECT_EQ(intMsg.interrupt->interruptId, int64_t{7});
            XX_TEST_EXPECT_EQ(intMsg.interrupt->inputType, "enum");
            XX_TEST_EXPECT_EQ(intMsg.interrupt->inputEnums.size(), size_t{3});
            XX_TEST_EXPECT_EQ(intMsg.interrupt->interruptStatus, V::InterruptStatus::Confirmed);
            XX_TEST_EXPECT_EQ(intMsg.interrupt->interruptResult, "b");
        }
        // msgIdCounter 恢复
        XX_TEST_EXPECT_EQ(loaded.msgIdCounter, uint64_t{6});

        // 目录布局: {root}/{sanitizedThreadId}/{session.db,share_store.db}
        auto dir = fs::path(root) / SessionStore::sanitizeSessionId("t1");
        XX_TEST_EXPECT_TRUE(fs::exists(dir / "session.db"));
        XX_TEST_EXPECT_TRUE(fs::exists(dir / "share_store.db"));

        // 模拟重启: 新实例读同一目录
        auto p2 = std::make_shared<SessionStore>(root);
        auto l2 = p2->loadSession("t1");
        XX_TEST_EXPECT_EQ(l2.viewMessages.size(), msgs.size());
        XX_TEST_EXPECT_EQ(l2.msgIdCounter, uint64_t{6});
        XX_TEST_EXPECT_EQ(l2.viewMessages[0].text, "hello");
    }
    // 清理
    fs::remove_all(root);
    return TestResult{};
}

static TestResult testLlmMessagesRoundtrip() {
    using agentxx::agent::SessionStore;

    auto root = makeTempRoot();
    {
        auto p = std::make_shared<SessionStore>(root);

        // 空上下文
        p->saveLlmMessages("t2", neograph::json::array());
        auto l1 = p->loadSession("t2");
        XX_TEST_EXPECT_TRUE(l1.llmMessages.is_array());
        XX_TEST_EXPECT_EQ(l1.llmMessages.size(), size_t{0});

        // 带 system/user/assistant/tool 的上下文
        neograph::json ctx = neograph::json::array();
        ctx.push_back(neograph::json{
            {"role",    "system"},
            {"content", "sys"   },
        });
        ctx.push_back(neograph::json{
            {"role",    "user"},
            {"content", "u1"  },
        });
        ctx.push_back(neograph::json{
            {"role",    "assistant"},
            {"content", "a1"       },
        });
        ctx.push_back(neograph::json{
            {"role",         "tool"      },
            {"tool_call_id", "call_1"    },
            {"content",      R"({"r":1})"},
        });
        p->saveLlmMessages("t2", ctx);

        // 重启后恢复
        auto p2 = std::make_shared<SessionStore>(root);
        auto l2 = p2->loadSession("t2");
        XX_TEST_EXPECT_TRUE(l2.llmMessages.is_array());
        XX_TEST_EXPECT_EQ(l2.llmMessages.size(), ctx.size());
        if (l2.llmMessages.size() == ctx.size()) {
            XX_TEST_EXPECT_EQ(
                l2.llmMessages[0].value("role", std::string{}),
                std::string{"system"}
            );
            XX_TEST_EXPECT_EQ(
                l2.llmMessages[3].value("tool_call_id", std::string{}),
                std::string{"call_1"}
            );
        }
    }
    fs::remove_all(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// SessionStore 单测: share store
// ---------------------------------------------------------------------------

static TestResult testShareStoreRoundtrip() {
    using agentxx::agent::SessionStore;

    auto root = makeTempRoot();
    {
        auto p = std::make_shared<SessionStore>(root);

        // 空存储
        auto empty = p->loadShareStore("s1");
        XX_TEST_EXPECT_TRUE(empty.items.empty());
        XX_TEST_EXPECT_EQ(empty.nextId, size_t{1});
        XX_TEST_EXPECT_NULLOPT(p->getShareStoreItem("s1", 1));

        // add: id 从 1 递增
        auto id1 = p->addShareStoreItem("s1", "first");
        auto id2 = p->addShareStoreItem("s1", "second");
        XX_TEST_EXPECT_EQ(id1, size_t{1});
        XX_TEST_EXPECT_EQ(id2, size_t{2});
        auto v1 = p->getShareStoreItem("s1", id1);
        XX_TEST_EXPECT_HAS_VALUE(v1);
        if (v1) {
            XX_TEST_EXPECT_EQ(*v1, std::string{"first"});
        }

        // set 覆盖 + 显式新 id
        p->setShareStoreItem("s1", id1, "first-updated");
        p->setShareStoreItem("s1", 100, "far-id");
        auto v1b = p->getShareStoreItem("s1", id1);
        XX_TEST_EXPECT_HAS_VALUE(v1b);
        if (v1b) {
            XX_TEST_EXPECT_EQ(*v1b, std::string{"first-updated"});
        }
        auto v100 = p->getShareStoreItem("s1", 100);
        XX_TEST_EXPECT_HAS_VALUE(v100);
        if (v100) {
            XX_TEST_EXPECT_EQ(*v100, std::string{"far-id"});
        }
        // set 显式 id 不影响自增: 下一个 id 取现有最大 id + 1 (101),
        // 避免与显式 set 的高位 id 冲突 (内存版计数器语义的稳健化)
        auto id3 = p->addShareStoreItem("s1", "third");
        XX_TEST_EXPECT_EQ(id3, size_t{101});

        // delete
        p->removeShareStoreItem("s1", id2);
        XX_TEST_EXPECT_NULLOPT(p->getShareStoreItem("s1", id2));
        p->removeShareStoreItem("s1", 9999); // 删除不存在: 无副作用
        XX_TEST_EXPECT_NULLOPT(p->getShareStoreItem("s1", 9999));

        // 模拟重启: 数据与 id 计数器延续 (剩余条目: id1/id3(101)/id100)
        auto p2     = std::make_shared<SessionStore>(root);
        auto loaded = p2->loadShareStore("s1");
        XX_TEST_EXPECT_EQ(loaded.items.size(), size_t{3});
        XX_TEST_EXPECT_EQ(loaded.nextId, size_t{102}); // max(1,100,101)+1
        XX_TEST_EXPECT_EQ(p2->getShareStoreItem("s1", 100).value_or(""), std::string{"far-id"});
        auto id4 = p2->addShareStoreItem("s1", "fourth");
        XX_TEST_EXPECT_EQ(id4, size_t{102});

        // 多 thread 隔离
        p->addShareStoreItem("s2", "other");
        auto loaded2 = p2->loadShareStore("s2");
        XX_TEST_EXPECT_EQ(loaded2.items.size(), size_t{1});
        XX_TEST_EXPECT_EQ(loaded2.items.at(1), std::string{"other"});
    }
    fs::remove_all(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// Session::updateViewMessage + 持久化: tool 结果回填后重启恢复仍为已完成
// ---------------------------------------------------------------------------

static TestResult testUpdateHistoryPersistence() {
    using agentxx::agent::SessionStore;
    using agentxx::agent::ViewMessage;
    using V = ViewMessage;

    auto root = makeTempRoot();
    {
        auto sessionStore   = std::make_shared<SessionStore>(root);
        auto store          = std::make_shared<agentxx::agent::SessionsManager>();
        store->sessionStore = sessionStore;

        auto s1 = store->getOrCreate("thread-upd");
        s1->appendViewMessage(ViewMessage::makeText(V::Role::User, "u1"));
        // 追加一条未完成的 Tool 消息 (模拟 assistant tool_calls 展开)
        V toolMsg;
        toolMsg.role        = V::Role::Tool;
        toolMsg.text        = R"({"path":"/x"})";
        toolMsg.startTimeMs = 1700000000000LL;
        toolMsg.collapsed   = true;
        V::ToolData t;
        t.toolName     = "agentxx_filesystem_read";
        t.toolCallId   = "call_x";
        t.toolFinished = false; // 尚未收到结果
        toolMsg.tool   = std::move(t);
        auto toolId    = s1->appendViewMessage(std::move(toolMsg));

        // 模拟 tool 结果回填: 走 Session::updateViewMessage (触发 onUpdateViewMessage 落库)
        auto& stored              = s1->viewMessages.back();
        stored.tool->toolResult   = "file content";
        stored.tool->toolFinished = true;
        stored.collapsed          = true;
        s1->updateViewMessage(stored);

        // 内存中已完成
        XX_TEST_EXPECT_TRUE(s1->viewMessages.back().tool->toolFinished);
        // 模拟轮末统一补存 (节流窗口内的 append/update 落库)
        s1->flushViewMessages();

        // ---- 模拟重启: 新实例恢复, tool 消息应保持已完成 ----
        auto p2              = std::make_shared<SessionStore>(root);
        auto store2          = std::make_shared<agentxx::agent::SessionsManager>();
        store2->sessionStore = p2;
        auto s2              = store2->getOrCreate("thread-upd");
        XX_TEST_EXPECT_EQ(s2->viewMessages.size(), size_t{2});
        const auto& toolMsg2 = s2->viewMessages[1];
        XX_TEST_EXPECT_EQ(toolMsg2.id, toolId);
        XX_TEST_EXPECT_TRUE(toolMsg2.tool.has_value());
        if (toolMsg2.tool) {
            XX_TEST_EXPECT_TRUE(toolMsg2.tool->toolFinished);
            XX_TEST_EXPECT_EQ(toolMsg2.tool->toolResult, std::string{"file content"});
        }

        // 链式哈希不受 updateViewMessage 影响 (历史内容语义不变)
        XX_TEST_EXPECT_EQ(s2->getHashInfo().count, size_t{2});

        // 不存在的 id: 仅记日志, 不崩溃
        V bogus  = ViewMessage::makeText(V::Role::User, "x");
        bogus.id = "msg_999999";
        s2->updateViewMessage(bogus);
    }
    fs::remove_all(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// Session + SessionStore 集成: 重启恢复语义
// ---------------------------------------------------------------------------

static TestResult testSessionStoreIntegration() {
    using agentxx::agent::SessionStore;
    using agentxx::agent::ViewMessage;
    using V = ViewMessage;

    auto root = makeTempRoot();
    {
        // ---- 第一次"运行": 写入历史 + LLM 上下文 ----
        auto sessionStore   = std::make_shared<SessionStore>(root);
        auto store          = std::make_shared<agentxx::agent::SessionsManager>();
        store->sessionStore = sessionStore;

        auto s1 = store->getOrCreate("thread-a");
        s1->appendViewMessage(ViewMessage::makeText(V::Role::User, "u1"));
        s1->appendViewMessage(ViewMessage::makeText(V::Role::Assistant, "a1"));
        s1->appendViewMessage(makeMsg(V::Role::Tool, R"({"tool":"x"})"));
        s1->llmMessages = neograph::json::array();
        s1->llmMessages.push_back(neograph::json{
            {"role",    "system"},
            {"content", "sys"   }
        });
        s1->llmMessages.push_back(neograph::json{
            {"role",    "user"},
            {"content", "u1"  }
        });
        s1->saveLlmMessages();
        // 模拟轮末统一补存 (节流窗口内的 append 落库)
        s1->flushViewMessages();
        auto hash1 = s1->getHashInfo();
        XX_TEST_EXPECT_EQ(hash1.count, size_t{3});

        // ---- 第二次"运行" (模拟重启): 新建 store + sessionStore ----
        auto p2              = std::make_shared<SessionStore>(root);
        auto store2          = std::make_shared<agentxx::agent::SessionsManager>();
        store2->sessionStore = p2;

        auto s2 = store2->getOrCreate("thread-a");
        XX_TEST_EXPECT_EQ(s2->viewMessages.size(), size_t{3});
        XX_TEST_EXPECT_EQ(s2->viewMessages[0].id, std::string{"msg_000001"});
        XX_TEST_EXPECT_EQ(s2->viewMessages[1].id, std::string{"msg_000002"});
        XX_TEST_EXPECT_EQ(s2->viewMessages[2].tool.has_value(), true);
        // 链式哈希一致 (恢复重建 == 追加计算)
        auto hash2 = s2->getHashInfo();
        XX_TEST_EXPECT_EQ(hash2.count, hash1.count);
        XX_TEST_EXPECT_EQ(hash2.tailHex, hash1.tailHex);
        // LLM 上下文恢复
        XX_TEST_EXPECT_TRUE(s2->llmMessages.is_array());
        XX_TEST_EXPECT_EQ(s2->llmMessages.size(), size_t{2});
        // msg id 延续: 新消息不冲突
        auto newId = s2->appendViewMessage(ViewMessage::makeText(V::Role::Assistant, "a2"));
        XX_TEST_EXPECT_EQ(newId, std::string{"msg_000004"});

        // 追加后再"重启", 历史含新消息
        auto p3              = std::make_shared<SessionStore>(root);
        auto store3          = std::make_shared<agentxx::agent::SessionsManager>();
        store3->sessionStore = p3;
        auto s3              = store3->getOrCreate("thread-a");
        XX_TEST_EXPECT_EQ(s3->viewMessages.size(), size_t{4});
        XX_TEST_EXPECT_EQ(s3->viewMessages[3].text, std::string{"a2"});
        XX_TEST_EXPECT_EQ(s3->getHashInfo().count, size_t{4});
        // 计数延续
        auto newId2 = s3->appendViewMessage(ViewMessage::makeText(V::Role::User, "u3"));
        XX_TEST_EXPECT_EQ(newId2, std::string{"msg_000005"});

        // ---- 不同 thread 互不影响 ----
        auto sOther = store3->getOrCreate("thread-b");
        XX_TEST_EXPECT_EQ(sOther->viewMessages.size(), size_t{0});
        XX_TEST_EXPECT_EQ(sOther->getHashInfo().count, size_t{0});
    }
    fs::remove_all(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// 持久化节流: 首次触发立即落盘, 窗口内合并, 轮末强制补存收敛
// ---------------------------------------------------------------------------

static TestResult testPersistThrottle() {
    using agentxx::agent::SessionStore;
    using agentxx::agent::SessionsManager;
    using V = agentxx::agent::ViewMessage;

    auto root = makeTempRoot();
    {
        auto sessionStore   = std::make_shared<SessionStore>(root);
        auto store          = std::make_shared<SessionsManager>();
        store->sessionStore = sessionStore;
        auto s1             = store->getOrCreate("throttle");

        // ---- view 节流: 第 1 条窗口未开启 → 立即落盘 ----
        s1->appendViewMessage(V::makeText(V::Role::User, "u1"));
        {
            SessionStore probe(root);
            XX_TEST_EXPECT_EQ(probe.loadSession("throttle").viewMessages.size(), size_t{1});
        }
        // ---- 第 2/3 条: 节流窗口内 (连续触发) → 合并未落盘 ----
        s1->appendViewMessage(V::makeText(V::Role::Assistant, "a1"));
        s1->appendViewMessage(V::makeText(V::Role::User, "u2"));
        {
            SessionStore probe(root);
            auto loaded = probe.loadSession("throttle");
            XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{1});
            // msgIdCounter 与已落库条数一致 (兜底恢复语义)
            XX_TEST_EXPECT_EQ(loaded.msgIdCounter, uint64_t{1});
        }
        // ---- llm 节流: 首次结算立即落盘 ----
        neograph::json ctx = neograph::json::array();
        ctx.push_back(neograph::json{
            {"role",    "user"},
            {"content", "u1"  },
        });
        s1->llmMessages = std::move(ctx);
        s1->requestSaveLlmMessages();
        {
            SessionStore probe(root);
            XX_TEST_EXPECT_EQ(probe.loadSession("throttle").llmMessages.size(), size_t{1});
        }
        // ---- 第二次结算 (窗口内): 内存增长, 未落盘 ----
        s1->appendSettledLlmMessages(neograph::json::array({
            neograph::json{
                {"role",    "assistant"},
                {"content", "a1"       },
            },
        }));
        XX_TEST_EXPECT_EQ(s1->llmMessages.size(), size_t{2});
        {
            SessionStore probe(root);
            XX_TEST_EXPECT_EQ(probe.loadSession("throttle").llmMessages.size(), size_t{1});
        }
        // ---- 强制补存后全部收敛 (轮末 saveLlmMessages + flushViewMessages 语义) ----
        s1->flushViewMessages();
        s1->saveLlmMessages();
        {
            SessionStore probe(root);
            auto loaded = probe.loadSession("throttle");
            XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{3});
            XX_TEST_EXPECT_EQ(loaded.viewMessages[1].text, std::string{"a1"});
            XX_TEST_EXPECT_EQ(loaded.viewMessages[2].text, std::string{"u2"});
            XX_TEST_EXPECT_EQ(loaded.msgIdCounter, uint64_t{3});
            XX_TEST_EXPECT_TRUE(loaded.llmMessages.is_array());
            XX_TEST_EXPECT_EQ(loaded.llmMessages.size(), size_t{2});
        }
    }
    fs::remove_all(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// MiddlewareContext + share store 写穿持久化
// ---------------------------------------------------------------------------

static TestResult testMiddlewareShareStorePersistence() {
    using agentxx::agent::SessionStore;
    using agentxx::middleware::MiddlewareContext;

    auto root = makeTempRoot();
    {
        auto sessionStore = std::make_shared<SessionStore>(root);

        // ---- 第一次运行: 写穿落库 ----
        auto ctx = std::make_shared<MiddlewareContext>(sessionStore);
        auto id1 = ctx->addShareStoreItemValue("m1", "v1");
        auto id2 = ctx->addShareStoreItemValue("m1", "v2");
        XX_TEST_EXPECT_EQ(id1, size_t{1});
        XX_TEST_EXPECT_EQ(id2, size_t{2});
        XX_TEST_EXPECT_EQ(ctx->getShareStoreItemValue("m1", id1).value_or(""), std::string{"v1"});
        ctx->setShareStoreItemValue("m1", id1, "v1-updated");
        XX_TEST_EXPECT_EQ(
            ctx->getShareStoreItemValue("m1", id1).value_or(""),
            std::string{"v1-updated"}
        );

        // ---- 模拟重启: 新 MiddlewareContext 从 DB 恢复 ----
        auto ctx2 = std::make_shared<MiddlewareContext>(sessionStore);
        XX_TEST_EXPECT_EQ(
            ctx2->getShareStoreItemValue("m1", id1).value_or(""),
            std::string{"v1-updated"}
        );
        XX_TEST_EXPECT_EQ(ctx2->getShareStoreItemValue("m1", id2).value_or(""), std::string{"v2"});
        // 空 thread 返回 nullopt (不误报)
        XX_TEST_EXPECT_NULLOPT(ctx2->getShareStoreItemValue("m1", 999));
        // id 延续
        auto id3 = ctx2->addShareStoreItemValue("m1", "v3");
        XX_TEST_EXPECT_EQ(id3, size_t{3});
        // 删除
        ctx2->removeShareStoreItemValue("m1", id2);
        XX_TEST_EXPECT_NULLOPT(ctx2->getShareStoreItemValue("m1", id2));
        // 重启后删除生效
        auto ctx3 = std::make_shared<MiddlewareContext>(sessionStore);
        XX_TEST_EXPECT_NULLOPT(ctx3->getShareStoreItemValue("m1", id2));
        XX_TEST_EXPECT_EQ(ctx3->getShareStoreItemValue("m1", id3).value_or(""), std::string{"v3"});
    }
    fs::remove_all(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// sessionId 清洗
// ---------------------------------------------------------------------------

static TestResult testSanitizeThreadId() {
    using agentxx::agent::SessionStore;

    auto dir = [](std::string_view tid) {
        return SessionStore::sanitizeSessionId(tid);
    };

    // 常规 sessionId 保持不变 (目录可读)
    XX_TEST_EXPECT_EQ(dir("session"), std::string{"session"});
    XX_TEST_EXPECT_EQ(dir("test-thread_1"), std::string{"test-thread_1"});

    // 空串 → default
    XX_TEST_EXPECT_EQ(dir(""), std::string{"default"});

    // 清洗结果不含路径分隔符 / 非法字符
    for (const auto& tid : {"a/b", "a\\b", "../evil", "x:y", "x?y", "x*y", "..", "."}) {
        auto d = dir(tid);
        XX_TEST_EXPECT_TRUE(d.find('/') == std::string::npos);
        XX_TEST_EXPECT_TRUE(d.find('\\') == std::string::npos);
        XX_TEST_EXPECT_TRUE(d.find(':') == std::string::npos);
        XX_TEST_EXPECT_TRUE(!d.empty());
        // 发生过改写的 sessionId 附带 8 位 hex 尾缀 (与原始 sessionId 区分)
        if (tid != d) {
            XX_TEST_EXPECT_TRUE(d.size() >= 9);
            auto suffix = d.substr(d.size() - 9);
            XX_TEST_EXPECT_EQ(suffix[0], '_');
            for (size_t i = 1; i < suffix.size(); ++i) {
                XX_TEST_EXPECT_TRUE(
                    (suffix[i] >= '0' && suffix[i] <= '9') || (suffix[i] >= 'a' && suffix[i] <= 'f')
                );
            }
        }
    }

    // 不同 sessionId 绝不映射到同一目录 (含清洗碰撞规避)
    {
        std::vector<std::string> ids{"a/b", "a_b", "../x", "..", "x:y", "x*y", "x?y", "CON"};
        for (size_t i = 0; i < ids.size(); ++i) {
            for (size_t j = i + 1; j < ids.size(); ++j) {
                XX_TEST_EXPECT_TRUE(dir(ids[i]) != dir(ids[j]));
            }
        }
        // ".." 与 "." 不能映射为父级/自身目录
        XX_TEST_EXPECT_TRUE(dir("..") != std::string{".."});
        XX_TEST_EXPECT_TRUE(dir(".") != std::string{"."});
    }

    // 确定性: 同输入同输出
    XX_TEST_EXPECT_EQ(dir("a/b"), dir("a/b"));

    // 超长 sessionId 截断 (总长受控)
    {
        std::string longTid(300, 'x');
        auto        d = dir(longTid);
        XX_TEST_EXPECT_TRUE(d.size() <= 96 + 9);
        XX_TEST_EXPECT_TRUE(d.rfind("xxxx", d.size() - 10) != std::string::npos);
    }
    return TestResult{};
}

// ---------------------------------------------------------------------------
// 会话列表 keyset 分页列举 (listSessionsPage: 按最近活动降序, 游标续取)
// ---------------------------------------------------------------------------

static TestResult testSessionListPagination() {
    using agentxx::agent::SessionStore;
    using V = agentxx::agent::ViewMessage;

    auto root = makeTempRoot();
    {
        SessionStore store(root);

        // 写入 7 个会话, lastActiveMs 交错; 目录创建顺序与时间顺序不同,
        // 保证排序/分页不依赖目录迭代序。"t5"/"t6" 同为 400ms 平局对,
        // 按 sessionId 升序稳定排列 (t5 在前)
        auto addSession = [&store](std::string_view sid, int64_t tsMs) {
            auto msg        = V::makeText(V::Role::User, fmt::format("msg-{}", sid));
            msg.startTimeMs = tsMs;
            store.appendViewMessage(sid, msg, 1);
        };
        addSession("t1", 700);
        addSession("t2", 600);
        addSession("t3", 500);
        addSession("t5", 400);
        addSession("t4", 300);
        addSession("t6", 400);
        addSession("t7", 200);

        // ---- 全量基准: 降序 + 同毫秒按 id 升序 ----
        const auto all = store.listSessions();
        XX_TEST_EXPECT_EQ(all.size(), size_t{7});
        if (all.size() == 7) {
            XX_TEST_EXPECT_EQ(all[0].sessionId, std::string{"t1"});
            XX_TEST_EXPECT_EQ(all[1].sessionId, std::string{"t2"});
            XX_TEST_EXPECT_EQ(all[2].sessionId, std::string{"t3"});
            XX_TEST_EXPECT_EQ(all[3].sessionId, std::string{"t5"});
            XX_TEST_EXPECT_EQ(all[4].sessionId, std::string{"t6"});
            XX_TEST_EXPECT_EQ(all[5].sessionId, std::string{"t4"});
            XX_TEST_EXPECT_EQ(all[6].sessionId, std::string{"t7"});
        }

        // ---- 首页 limit=3: 最新三条 + hasMore + 总数 ----
        auto p1 = store.listSessionsPage(0, "", 3);
        XX_TEST_EXPECT_EQ(p1.totalCount, uint64_t{7});
        XX_TEST_EXPECT_TRUE(p1.hasMore);
        XX_TEST_EXPECT_EQ(p1.sessions.size(), size_t{3});
        if (p1.sessions.size() == 3) {
            XX_TEST_EXPECT_EQ(p1.sessions[0].sessionId, std::string{"t1"});
            XX_TEST_EXPECT_EQ(p1.sessions[2].sessionId, std::string{"t3"});
        }
        // title 来自首条用户消息预览
        if (!p1.sessions.empty()) {
            XX_TEST_EXPECT_EQ(p1.sessions[0].title, std::string{"msg-t1"});
        }

        // ---- 游标续取至尽: 全序无重复无遗漏 ----
        std::vector<std::string> seen;
        auto                     cursor = p1;
        while (!cursor.sessions.empty()) {
            for (const auto& s : cursor.sessions) {
                seen.push_back(s.sessionId);
            }
            if (!cursor.hasMore) {
                break;
            }
            const auto& last = cursor.sessions.back();
            cursor            = store.listSessionsPage(last.lastActiveMs, last.sessionId, 3);
        }
        XX_TEST_EXPECT_EQ(seen.size(), size_t{7});
        if (seen.size() == 7) {
            XX_TEST_EXPECT_EQ(seen[0], std::string{"t1"});
            XX_TEST_EXPECT_EQ(seen[1], std::string{"t2"});
            XX_TEST_EXPECT_EQ(seen[2], std::string{"t3"});
            XX_TEST_EXPECT_EQ(seen[3], std::string{"t5"});
            XX_TEST_EXPECT_EQ(seen[4], std::string{"t6"});
            XX_TEST_EXPECT_EQ(seen[5], std::string{"t4"});
            XX_TEST_EXPECT_EQ(seen[6], std::string{"t7"});
        }

        // ---- 游标平局边界: beforeMs 恰为某会话时间戳时排除该会话本身,
        //      但包含同毫秒、sessionId 更大的会话 ----
        // 游标 = (400, "t5") → 之后为 t6(400, id 更大)、t4(300)、t7(200)
        auto ptie = store.listSessionsPage(400, "t5", 10);
        XX_TEST_EXPECT_FALSE(ptie.hasMore);
        XX_TEST_EXPECT_EQ(ptie.totalCount, uint64_t{7});
        XX_TEST_EXPECT_EQ(ptie.sessions.size(), size_t{3});
        if (ptie.sessions.size() == 3) {
            XX_TEST_EXPECT_EQ(ptie.sessions[0].sessionId, std::string{"t6"});
            XX_TEST_EXPECT_EQ(ptie.sessions[1].sessionId, std::string{"t4"});
            XX_TEST_EXPECT_EQ(ptie.sessions[2].sessionId, std::string{"t7"});
        }
        // 页大小恰好在平局对中间截断: 首页收满后同毫秒的 t6 排在游标之后的
        // 后续页, 游标携带 sessionId 保证不重不漏
        auto phalf = store.listSessionsPage(0, "", 4);
        if (phalf.sessions.size() == 4) {
            XX_TEST_EXPECT_EQ(phalf.sessions[3].sessionId, std::string{"t5"});
            XX_TEST_EXPECT_TRUE(phalf.hasMore);
            auto ptail = store.listSessionsPage(
                phalf.sessions.back().lastActiveMs, phalf.sessions.back().sessionId, 4
            );
            XX_TEST_EXPECT_FALSE(ptail.hasMore);
            if (ptail.sessions.size() == 3) {
                XX_TEST_EXPECT_EQ(ptail.sessions[0].sessionId, std::string{"t6"});
                XX_TEST_EXPECT_EQ(ptail.sessions[1].sessionId, std::string{"t4"});
                XX_TEST_EXPECT_EQ(ptail.sessions[2].sessionId, std::string{"t7"});
            }
        }

        // ---- limit=0 全量路径等价 listSessions ----
        auto pall = store.listSessionsPage(0, "", 0);
        XX_TEST_EXPECT_FALSE(pall.hasMore);
        XX_TEST_EXPECT_EQ(pall.totalCount, uint64_t{7});
        XX_TEST_EXPECT_EQ(pall.sessions.size(), all.size());

        // ---- 空根目录 (从未持久化): 空页无更多 ----
        auto emptyRoot = makeTempRoot();
        {
            SessionStore emptyStore(emptyRoot);
            auto         pe = emptyStore.listSessionsPage(0, "", 5);
            XX_TEST_EXPECT_TRUE(pe.sessions.empty());
            XX_TEST_EXPECT_FALSE(pe.hasMore);
            XX_TEST_EXPECT_EQ(pe.totalCount, uint64_t{0});
        }
        fs::remove_all(emptyRoot);

        // ---- 追加新会话后排最前, 已有游标序列不受影响 ----
        addSession("t0-new", 800);
        auto pn = store.listSessionsPage(0, "", 2);
        XX_TEST_EXPECT_EQ(pn.totalCount, uint64_t{8});
        if (pn.sessions.size() == 2) {
            XX_TEST_EXPECT_EQ(pn.sessions[0].sessionId, std::string{"t0-new"});
            XX_TEST_EXPECT_EQ(pn.sessions[1].sessionId, std::string{"t1"});
        }
        XX_TEST_EXPECT_TRUE(pn.hasMore);
    }
    fs::remove_all(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// 端到端: 真实 BaseAgent (模拟 LLM Server) + 持久化, 重启后恢复会话
// ---------------------------------------------------------------------------

static asio::awaitable<void> testSessionPersistenceE2E() {
    using agentxx::agent::CodeAgent;

    auto root = makeTempRoot();
    {
        auto sim     = startDaSimServer();
        auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

        auto makeCfg = [&]() {
            auto cfg                   = std::make_shared<agentxx::agent::AgentConfig>();
            cfg->model.baseUrl         = baseUrl;
            cfg->model.apiKey          = "EMPTY";
            cfg->model.modelName       = "test-sim";
            cfg->prompt.systemPrompt   = "You are a helpful assistant.";
            cfg->enableSessionStore    = true;
            cfg->sessionStoreDirectory = root;
            return cfg;
        };

        // 收尾请求返回真实文本 (provider 层已把完全空响应当作生成失败,
        // 模拟器/用例遵循同一契约: 回合以带内容的 assistant 消息结束)
        g_da_sim_response_content = "E2E final answer";
        g_da_sim_tool_calls       = neograph::json::array({
            neograph::json{
                           {"index", 0},
                           {"id", "call_e2e_1"},
                           {"type", "function"},
                           {
                    "function",
                    neograph::json{
                              {"name", "agentxx_filesystem_list"},
                              {"arguments", "{}"},
                    },
                }, },
        });
        // 模拟 thinking 模型: 首个请求携带 reasoning_content + tool_calls,
        // 验证展开出的 Think 历史消息可持久化并在重启后恢复
        g_da_sim_reasoning_content = "E2E reasoning before tool call";

        // ---- 第一次运行: 一轮对话 + share store 写入 ----
        {
            CodeAgent agent(makeCfg());
            co_await agent.init();

            auto result = co_await agent.runTurnAsync("e2e-thread", "Hello", true, nullptr);
            XX_TEST_EXPECT_FALSE(result.hasError);
            XX_TEST_EXPECT_FALSE(result.interrupted);

            // 内存 viewMessages: user + Think(tool_calls 展开) + Tool
            auto sess = agent.agentContext->getSession("e2e-thread");
            XX_TEST_EXPECT_TRUE(sess->viewMessages.size() >= size_t{3});
            bool thinkingInMemory = false;
            for (const auto& vm : sess->viewMessages) {
                if (vm.role == agentxx::agent::ViewMessage::Role::Think
                    && vm.text == "E2E reasoning before tool call") {
                    thinkingInMemory = true;
                }
            }
            XX_TEST_EXPECT_TRUE(thinkingInMemory);

            // share store 写穿
            auto id = agent.agentContext->middlewareHandleContext->addShareStoreItemValue(
                "e2e-thread",
                "stored-value"
            );
            XX_TEST_EXPECT_EQ(id, size_t{1});

            // 落盘检查: 目录布局 {root}/{sessionId}/{session.db, share_store.db}
            auto dir
                = fs::path(root) / agentxx::agent::SessionStore::sanitizeSessionId("e2e-thread");
            XX_TEST_EXPECT_TRUE(fs::exists(dir / "session.db"));
            XX_TEST_EXPECT_TRUE(fs::exists(dir / "share_store.db"));
        }

        // ---- 模拟重启: 新 Agent 实例恢复会话 ----
        {
            CodeAgent agent(makeCfg());
            co_await agent.init();

            auto sess = agent.agentContext->getSession("e2e-thread");
            // 展示历史恢复: user + Think + Tool + Assistant(收尾文本);
            // Think 是本次修复的核心断言, 修复前 tool_calls 分支不展开 Think,
            // 重启后 Think 丢失
            XX_TEST_EXPECT_EQ(sess->viewMessages.size(), size_t{4});
            XX_TEST_EXPECT_EQ(sess->viewMessages[0].id, std::string{"msg_000001"});
            XX_TEST_EXPECT_EQ(sess->viewMessages[0].text, std::string{"Hello"});
            XX_TEST_EXPECT_TRUE(
                sess->viewMessages[1].role == agentxx::agent::ViewMessage::Role::Think
            );
            XX_TEST_EXPECT_EQ(
                sess->viewMessages[1].text,
                std::string{"E2E reasoning before tool call"}
            );
            XX_TEST_EXPECT_TRUE(sess->viewMessages[1].collapsed);
            XX_TEST_EXPECT_TRUE(
                sess->viewMessages[2].role == agentxx::agent::ViewMessage::Role::Tool
            );
            // 收尾 assistant 消息 (带内容) 同样可持久化恢复
            XX_TEST_EXPECT_TRUE(
                sess->viewMessages[3].role == agentxx::agent::ViewMessage::Role::Assistant
            );
            XX_TEST_EXPECT_EQ(sess->viewMessages[3].text, std::string{"E2E final answer"});
            // LLM 上下文恢复 (system + user + assistant(tool_calls) + tool + assistant)
            XX_TEST_EXPECT_TRUE(sess->llmMessages.is_array());
            XX_TEST_EXPECT_TRUE(sess->llmMessages.size() >= size_t{2});
            // msg id 延续
            auto newId = sess->appendViewMessage(agentxx::agent::ViewMessage::makeText(
                agentxx::agent::ViewMessage::Role::Assistant,
                "extra"
            ));
            XX_TEST_EXPECT_EQ(newId, std::string{"msg_000005"});

            // share store 恢复 (懒加载自 DB)
            auto v = agent.agentContext->middlewareHandleContext->getShareStoreItemValue(
                "e2e-thread",
                1
            );
            XX_TEST_EXPECT_HAS_VALUE(v);
            if (v) {
                XX_TEST_EXPECT_EQ(*v, std::string{"stored-value"});
            }
            // 新条目 id 延续
            auto id2 = agent.agentContext->middlewareHandleContext->addShareStoreItemValue(
                "e2e-thread",
                "second"
            );
            XX_TEST_EXPECT_EQ(id2, size_t{2});
        }

        sim.stop();
    }
    fs::remove_all(root);
    co_return;
}

static asio::awaitable<void> testTurnEndTipPersistenceRoundtrip() {
    using agentxx::agent::CodeAgent;
    using agentxx::agent::SessionServerAgentIO;

    auto root = makeTempRoot();
    {
        auto sim     = startDaSimServer();
        auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

        auto makeCfg = [&]() {
            auto cfg                   = std::make_shared<agentxx::agent::AgentConfig>();
            cfg->model.baseUrl         = baseUrl;
            cfg->model.apiKey          = "EMPTY";
            cfg->model.modelName       = "test-model-xyz";
            cfg->prompt.systemPrompt   = "You are a helpful assistant.";
            cfg->enableSessionStore    = true;
            cfg->sessionStoreDirectory = root;
            return cfg;
        };

        g_da_sim_response_content  = "Turn result text";
        g_da_sim_tool_calls        = neograph::json::array();
        g_da_sim_reasoning_content = "";

        // 第一次运行: 传入 server-io 驱动会话, 验证轮次结束时插入 Tip 消息并落盘
        {
            auto agent = std::make_shared<CodeAgent>(makeCfg());
            co_await agent->init();

            SessionServerAgentIO::Config ioCfg;
            ioCfg.sessionId = "turn-tip-thread";
            auto serverIO   = std::make_shared<SessionServerAgentIO>(
                agent->ioCtx->get_executor(),
                agent,
                std::move(ioCfg)
            );

            auto result = co_await agent->runTurnAsync("turn-tip-thread", "Hello", true, serverIO);
            XX_TEST_EXPECT_FALSE(result.hasError);

            auto sess = agent->agentContext->getSession("turn-tip-thread");
            XX_TEST_EXPECT_TRUE(sess != nullptr);
            if (sess) {
                // 内存中消息: User + Assistant + Tip (轮次统计: 模型名 · 耗时 · 时间点)
                XX_TEST_EXPECT_EQ(sess->viewMessages.size(), size_t{3});
                if (sess->viewMessages.size() >= 3) {
                    const auto& tipMsg = sess->viewMessages.back();
                    XX_TEST_EXPECT_EQ(tipMsg.role, agentxx::agent::ViewMessage::Role::Tip);
                    XX_TEST_EXPECT_TRUE(tipMsg.text.find("test-model-xyz") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(tipMsg.tip.has_value());
                    if (tipMsg.tip) {
                        XX_TEST_EXPECT_EQ(
                            tipMsg.tip->tipLevel,
                            agentxx::agent::ViewMessage::TipLevel::Info
                        );
                    }
                }
            }
        }

        // 模拟重启: 新 Agent 实例从 SQLite 恢复会话, 验证轮次完成提示消息成功落盘且完整恢复
        {
            CodeAgent agent(makeCfg());
            co_await agent.init();

            auto sess = agent.agentContext->getSession("turn-tip-thread");
            XX_TEST_EXPECT_TRUE(sess != nullptr);
            if (sess) {
                XX_TEST_EXPECT_EQ(sess->viewMessages.size(), size_t{3});
                if (sess->viewMessages.size() >= 3) {
                    XX_TEST_EXPECT_EQ(
                        sess->viewMessages[0].role,
                        agentxx::agent::ViewMessage::Role::User
                    );
                    XX_TEST_EXPECT_EQ(
                        sess->viewMessages[1].role,
                        agentxx::agent::ViewMessage::Role::Assistant
                    );
                    const auto& tipMsg = sess->viewMessages[2];
                    XX_TEST_EXPECT_EQ(tipMsg.role, agentxx::agent::ViewMessage::Role::Tip);
                    XX_TEST_EXPECT_TRUE(tipMsg.text.find("test-model-xyz") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(tipMsg.tip.has_value());
                    if (tipMsg.tip) {
                        XX_TEST_EXPECT_EQ(
                            tipMsg.tip->tipLevel,
                            agentxx::agent::ViewMessage::TipLevel::Info
                        );
                    }
                }
            }
        }

        sim.stop();
    }
    fs::remove_all(root);
    co_return;
}

asio::awaitable<TestResult> run_session_persistence_tests() {
    g_sp_passed = 0;
    g_sp_failed = 0;

    testViewMessagesRoundtrip();
    testLlmMessagesRoundtrip();
    testShareStoreRoundtrip();
    testUpdateHistoryPersistence();
    testSessionStoreIntegration();
    testPersistThrottle();
    testMiddlewareShareStorePersistence();
    testSanitizeThreadId();
    testSessionListPagination();

    // E2E 需要独立 io_context (BaseAgent 内部有自身的 io 循环)
    asio::io_context io;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            co_await testSessionPersistenceE2E();
            co_await testTurnEndTipPersistenceRoundtrip();
        },
        asio::detached
    );
    io.run();

    co_return TestResult{g_sp_passed, g_sp_failed};
}

} // namespace test
} // namespace agentxx
