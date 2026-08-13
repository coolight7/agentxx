// 注意顺序: test_session_persistence.h 会把 XX_TEST_PASSED/FAILED 重定义为
// g_sp_*, 必须包含在 test_agent.h 之后 (test_agent.h 会重定义为 g_da_*),
// 使本模块测试计数回落到 g_sp_* 而非计入 agent 模块
#include "test_session_persistence.h"
#include "test_agent.h"

#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/session_persistence.h"
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

namespace agentxx {
namespace test {

int g_sp_passed = 0;
int g_sp_failed = 0;

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
        case V::Role::System: {
            V::SystemData s;
            s.tipLevel = V::TipLevel::Warning;
            msg.system = std::move(s);
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
// SessionPersistence 单测: viewMessages / llmMessages / meta
// ---------------------------------------------------------------------------

static TestResult testViewMessagesRoundtrip() {
    using agentxx::agent::SessionPersistence;
    using V = agentxx::agent::ViewMessage;

    auto root = makeTempRoot();
    {
        // 写入: user/assistant/tool/system/thinking/interrupt 各一条
        auto p = std::make_shared<SessionPersistence>(root);
        // 只读访问不创建目录/空文件
        auto fresh = p->loadSession("not-exist-yet");
        XX_TEST_EXPECT_TRUE(fresh.viewMessages.empty());
        XX_TEST_EXPECT_TRUE(!fs::exists(fs::path(root) / "not-exist-yet"));

        std::vector<V> msgs{
            makeMsg(V::Role::User, "hello"),
            makeMsg(V::Role::Assistant, "hi there"),
            makeMsg(V::Role::Tool, R"({"tool":"x"})"),
            makeMsg(V::Role::System, "tip"),
            makeMsg(V::Role::Thinking, "think"),
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
        XX_TEST_EXPECT_TRUE(sysMsg.system.has_value());
        if (sysMsg.system) {
            XX_TEST_EXPECT_EQ(sysMsg.system->tipLevel, V::TipLevel::Warning);
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
        auto dir = fs::path(root) / SessionPersistence::sanitizeThreadId("t1");
        XX_TEST_EXPECT_TRUE(fs::exists(dir / "session.db"));
        XX_TEST_EXPECT_TRUE(fs::exists(dir / "share_store.db"));

        // 模拟重启: 新实例读同一目录
        auto p2 = std::make_shared<SessionPersistence>(root);
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
    using agentxx::agent::SessionPersistence;

    auto root = makeTempRoot();
    {
        auto p = std::make_shared<SessionPersistence>(root);

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
        auto p2 = std::make_shared<SessionPersistence>(root);
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
// SessionPersistence 单测: share store
// ---------------------------------------------------------------------------

static TestResult testShareStoreRoundtrip() {
    using agentxx::agent::SessionPersistence;

    auto root = makeTempRoot();
    {
        auto p = std::make_shared<SessionPersistence>(root);

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
        auto p2     = std::make_shared<SessionPersistence>(root);
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
// Session::updateHistory + 持久化: tool 结果回填后重启恢复仍为已完成
// ---------------------------------------------------------------------------

static TestResult testUpdateHistoryPersistence() {
    using agentxx::agent::SessionPersistence;
    using agentxx::agent::SessionStore;
    using agentxx::agent::ViewMessage;
    using V = ViewMessage;

    auto root = makeTempRoot();
    {
        auto persistence   = std::make_shared<SessionPersistence>(root);
        auto store         = std::make_shared<SessionStore>();
        store->persistence = persistence;

        auto s1 = store->getOrCreate("thread-upd");
        s1->appendHistory(ViewMessage::makeText(V::Role::User, "u1"));
        // 追加一条未完成的 Tool 消息 (模拟 assistant tool_calls 展开)
        V toolMsg;
        toolMsg.role        = V::Role::Tool;
        toolMsg.text        = R"({"path":"/x"})";
        toolMsg.startTimeMs = 1700000000000LL;
        toolMsg.collapsed   = true;
        V::ToolData t;
        t.toolName     = "agentxx_filesystem_read_text_file";
        t.toolCallId   = "call_x";
        t.toolFinished = false; // 尚未收到结果
        toolMsg.tool   = std::move(t);
        auto toolId    = s1->appendHistory(std::move(toolMsg));

        // 模拟 tool 结果回填: 走 Session::updateHistory (触发 onUpdateMessage 落库)
        auto& stored              = s1->viewMessages.back();
        stored.tool->toolResult   = "file content";
        stored.tool->toolFinished = true;
        stored.collapsed          = true;
        s1->updateHistory(stored);

        // 内存中已完成
        XX_TEST_EXPECT_TRUE(s1->viewMessages.back().tool->toolFinished);

        // ---- 模拟重启: 新实例恢复, tool 消息应保持已完成 ----
        auto p2             = std::make_shared<SessionPersistence>(root);
        auto store2         = std::make_shared<SessionStore>();
        store2->persistence = p2;
        auto s2             = store2->getOrCreate("thread-upd");
        XX_TEST_EXPECT_EQ(s2->viewMessages.size(), size_t{2});
        const auto& toolMsg2 = s2->viewMessages[1];
        XX_TEST_EXPECT_EQ(toolMsg2.id, toolId);
        XX_TEST_EXPECT_TRUE(toolMsg2.tool.has_value());
        if (toolMsg2.tool) {
            XX_TEST_EXPECT_TRUE(toolMsg2.tool->toolFinished);
            XX_TEST_EXPECT_EQ(toolMsg2.tool->toolResult, std::string{"file content"});
        }

        // 链式哈希不受 updateHistory 影响 (历史内容语义不变)
        XX_TEST_EXPECT_EQ(s2->getHashInfo().count, size_t{2});

        // 不存在的 id: 仅记日志, 不崩溃
        V bogus  = ViewMessage::makeText(V::Role::User, "x");
        bogus.id = "msg_999999";
        s2->updateHistory(bogus);
    }
    fs::remove_all(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// Session + SessionStore 集成: 重启恢复语义
// ---------------------------------------------------------------------------

static TestResult testSessionStoreIntegration() {
    using agentxx::agent::SessionPersistence;
    using agentxx::agent::SessionStore;
    using agentxx::agent::ViewMessage;
    using V = ViewMessage;

    auto root = makeTempRoot();
    {
        // ---- 第一次"运行": 写入历史 + LLM 上下文 ----
        auto persistence   = std::make_shared<SessionPersistence>(root);
        auto store         = std::make_shared<SessionStore>();
        store->persistence = persistence;

        auto s1 = store->getOrCreate("thread-a");
        s1->appendHistory(ViewMessage::makeText(V::Role::User, "u1"));
        s1->appendHistory(ViewMessage::makeText(V::Role::Assistant, "a1"));
        s1->appendHistory(makeMsg(V::Role::Tool, R"({"tool":"x"})"));
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
        auto hash1 = s1->getHashInfo();
        XX_TEST_EXPECT_EQ(hash1.count, size_t{3});

        // ---- 第二次"运行" (模拟重启): 新建 store + persistence ----
        auto p2             = std::make_shared<SessionPersistence>(root);
        auto store2         = std::make_shared<SessionStore>();
        store2->persistence = p2;

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
        auto newId = s2->appendHistory(ViewMessage::makeText(V::Role::Assistant, "a2"));
        XX_TEST_EXPECT_EQ(newId, std::string{"msg_000004"});

        // 追加后再"重启", 历史含新消息
        auto p3             = std::make_shared<SessionPersistence>(root);
        auto store3         = std::make_shared<SessionStore>();
        store3->persistence = p3;
        auto s3             = store3->getOrCreate("thread-a");
        XX_TEST_EXPECT_EQ(s3->viewMessages.size(), size_t{4});
        XX_TEST_EXPECT_EQ(s3->viewMessages[3].text, std::string{"a2"});
        XX_TEST_EXPECT_EQ(s3->getHashInfo().count, size_t{4});
        // 计数延续
        auto newId2 = s3->appendHistory(ViewMessage::makeText(V::Role::User, "u3"));
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
// MiddlewareContext + share store 写穿持久化
// ---------------------------------------------------------------------------

static TestResult testMiddlewareShareStorePersistence() {
    using agentxx::agent::SessionPersistence;
    using agentxx::middleware::MiddlewareContext;

    auto root = makeTempRoot();
    {
        auto persistence = std::make_shared<SessionPersistence>(root);

        // ---- 第一次运行: 写穿落库 ----
        auto ctx = std::make_shared<MiddlewareContext>(persistence);
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
        auto ctx2 = std::make_shared<MiddlewareContext>(persistence);
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
        auto ctx3 = std::make_shared<MiddlewareContext>(persistence);
        XX_TEST_EXPECT_NULLOPT(ctx3->getShareStoreItemValue("m1", id2));
        XX_TEST_EXPECT_EQ(ctx3->getShareStoreItemValue("m1", id3).value_or(""), std::string{"v3"});
    }
    fs::remove_all(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// threadId 清洗
// ---------------------------------------------------------------------------

static TestResult testSanitizeThreadId() {
    using agentxx::agent::SessionPersistence;

    auto dir = [](std::string_view tid) {
        return SessionPersistence::sanitizeThreadId(tid);
    };

    // 常规 threadId 保持不变 (目录可读)
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
        // 发生过改写的 threadId 附带 8 位 hex 尾缀 (与原始 threadId 区分)
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

    // 不同 threadId 绝不映射到同一目录 (含清洗碰撞规避)
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

    // 超长 threadId 截断 (总长受控)
    {
        std::string longTid(300, 'x');
        auto        d = dir(longTid);
        XX_TEST_EXPECT_TRUE(d.size() <= 96 + 9);
        XX_TEST_EXPECT_TRUE(d.rfind("xxxx", d.size() - 10) != std::string::npos);
    }
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
            auto cfg                      = std::make_shared<agentxx::agent::AgentConfig>();
            cfg->model.baseUrl            = baseUrl;
            cfg->model.apiKey             = "EMPTY";
            cfg->model.modelName          = "test-sim";
            cfg->prompt.systemPrompt      = "You are a helpful assistant.";
            cfg->enableSessionPersistence = true;
            cfg->sessionPersistenceRoot   = root;
            return cfg;
        };

        g_da_sim_response_content = "";
        g_da_sim_tool_calls       = neograph::json::array({
            neograph::json{
                           {"index", 0},
                           {"id", "call_e2e_1"},
                           {"type", "function"},
                           {"function",
                       neograph::json{
                           {"name", "agentxx_filesystem_list"},
                           {"arguments", "{}"},
                 }},
                           },
        });
        // 模拟 thinking 模型: 首个请求携带 reasoning_content + tool_calls,
        // 验证展开出的 Thinking 历史消息可持久化并在重启后恢复
        g_da_sim_reasoning_content = "E2E reasoning before tool call";

        // ---- 第一次运行: 一轮对话 + share store 写入 ----
        {
            CodeAgent agent(makeCfg());
            co_await agent.init();

            auto result
                = co_await agent.runConversationTurnAsync("e2e-thread", "Hello", true, nullptr);
            XX_TEST_EXPECT_FALSE(result.hasError);
            XX_TEST_EXPECT_FALSE(result.interrupted);

            // 内存 viewMessages: user + Thinking(tool_calls 展开) + Tool
            auto sess = agent.agentContext->getSession("e2e-thread");
            XX_TEST_EXPECT_TRUE(sess->viewMessages.size() >= size_t{3});
            bool thinkingInMemory = false;
            for (const auto& vm : sess->viewMessages) {
                if (vm.role == agentxx::agent::ViewMessage::Role::Thinking
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

            // 落盘检查: 目录布局 {root}/{threadId}/{session.db, share_store.db}
            auto dir = fs::path(root)
                       / agentxx::agent::SessionPersistence::sanitizeThreadId("e2e-thread");
            XX_TEST_EXPECT_TRUE(fs::exists(dir / "session.db"));
            XX_TEST_EXPECT_TRUE(fs::exists(dir / "share_store.db"));
        }

        // ---- 模拟重启: 新 Agent 实例恢复会话 ----
        {
            CodeAgent agent(makeCfg());
            co_await agent.init();

            auto sess = agent.agentContext->getSession("e2e-thread");
            // 展示历史恢复: user + Thinking + Tool; Thinking 是本次修复的核心断言,
            // 修复前 tool_calls 分支不展开 Thinking, 重启后 Thinking 丢失
            XX_TEST_EXPECT_EQ(sess->viewMessages.size(), size_t{3});
            XX_TEST_EXPECT_EQ(sess->viewMessages[0].id, std::string{"msg_000001"});
            XX_TEST_EXPECT_EQ(sess->viewMessages[0].text, std::string{"Hello"});
            XX_TEST_EXPECT_TRUE(
                sess->viewMessages[1].role == agentxx::agent::ViewMessage::Role::Thinking
            );
            XX_TEST_EXPECT_EQ(
                sess->viewMessages[1].text,
                std::string{"E2E reasoning before tool call"}
            );
            XX_TEST_EXPECT_TRUE(sess->viewMessages[1].collapsed);
            XX_TEST_EXPECT_TRUE(
                sess->viewMessages[2].role == agentxx::agent::ViewMessage::Role::Tool
            );
            // LLM 上下文恢复 (system + user + assistant(tool_calls) + tool + assistant)
            XX_TEST_EXPECT_TRUE(sess->llmMessages.is_array());
            XX_TEST_EXPECT_TRUE(sess->llmMessages.size() >= size_t{2});
            // msg id 延续
            auto newId = sess->appendHistory(agentxx::agent::ViewMessage::makeText(
                agentxx::agent::ViewMessage::Role::Assistant,
                "extra"
            ));
            XX_TEST_EXPECT_EQ(newId, std::string{"msg_000004"});

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

asio::awaitable<TestResult> run_session_persistence_tests() {
    g_sp_passed = 0;
    g_sp_failed = 0;

    testViewMessagesRoundtrip();
    testLlmMessagesRoundtrip();
    testShareStoreRoundtrip();
    testUpdateHistoryPersistence();
    testSessionStoreIntegration();
    testMiddlewareShareStorePersistence();
    testSanitizeThreadId();

    // E2E 需要独立 io_context (BaseAgent 内部有自身的 io 循环)
    asio::io_context io;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            co_await testSessionPersistenceE2E();
        },
        asio::detached
    );
    io.run();

    co_return TestResult{g_sp_passed, g_sp_failed};
}

} // namespace test
} // namespace agentxx
