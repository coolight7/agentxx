/// test_session_admin —— 会话列表检索与改名 (计划 RET-1a / STO-12b)
///
/// 覆盖存储层之上的两个入口 (存储层 API 已在 `session_schema` 覆盖):
/// - 重命名: `WireRenameSession` → 写 `meta.title` + `titleSource=user`,
///   回 `WireRenameSessionResult`; 空标题 / 会话不存在 / 无持久化各回可读原因,
///   且**不**因为改名把不存在的会话目录建出来
/// - 检索: `WireListSessions.keyword` 非空时按标题或正文子串检索, 命中片段随
///   `SessionInfo.snippet` 回传; 检索结果不参与 keyset 续取
#include "agentxx-test/core/test_session_admin.h"

#include "agentxx-test/core/test_agent.h" // 本地 LLM 模拟器 startDaSimServer
#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/agent/session_store.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/string_util.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_sa_passed = 0;
int g_sa_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_sa_passed
#define XX_TEST_FAILED g_sa_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

/// 客户端收到的会话相关消息
struct SaRecorder {
    std::mutex                                           mu;
    std::vector<agentxx::agent::WireSessionList>         lists;
    std::vector<agentxx::agent::WireRenameSessionResult> renames;

    void handle(agentxx::agent::WireMessage msg) {
        std::visit(
            [this](auto&& m) {
                using T = std::decay_t<decltype(m)>;
                std::lock_guard<std::mutex> lock(mu);
                if constexpr (std::is_same_v<T, agentxx::agent::WireSessionList>) {
                    lists.push_back(m);
                } else if constexpr (std::is_same_v<T, agentxx::agent::WireRenameSessionResult>) {
                    renames.push_back(m);
                }
            },
            std::move(msg)
        );
    }

    size_t listCount() {
        std::lock_guard<std::mutex> lock(mu);
        return lists.size();
    }

    /// 最近一次列表响应 (无则返回空)
    agentxx::agent::WireSessionList lastList() {
        std::lock_guard<std::mutex> lock(mu);
        return lists.empty() ? agentxx::agent::WireSessionList{} : lists.back();
    }

    /// 最近一次改名回执 (无则返回 nullopt)
    std::optional<agentxx::agent::WireRenameSessionResult> lastRename() {
        std::lock_guard<std::mutex> lock(mu);
        if (renames.empty()) {
            return std::nullopt;
        }
        return renames.back();
    }

    size_t renameCount() {
        std::lock_guard<std::mutex> lock(mu);
        return renames.size();
    }
};

/// 测试夹具: 数据目录 + agent + 会话端点 + 客户端传输
struct Fixture {
    DaSimServer                                              sim;
    std::shared_ptr<agentxx::agent::AgentConfig>             cfg;
    std::shared_ptr<agentxx::agent::BaseAgent>               agent;
    std::shared_ptr<agentxx::agent::SessionServerAgentIO>    endpoint;
    std::shared_ptr<SaRecorder>                              recorder;
    std::shared_ptr<agentxx::agent::ChannelAgentIOTransport> clientT;
    std::string                                              root;

    ~Fixture() {
        if (clientT) {
            clientT->close();
        }
        if (endpoint) {
            endpoint->stop();
        }
        sim.stop();
    }

    std::shared_ptr<agentxx::agent::SessionStore> store() {
        return agent->agentContext->sessions->sessionStore;
    }
};

std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_sa_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

void removeTempRoot(const std::string& root) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::error_code ec;
        fs::remove_all(utilxx_base::utf8ToPath(root), ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

asio::awaitable<std::shared_ptr<Fixture>> makeFixture(std::string sessionId, bool persist = true) {
    auto fx = std::make_shared<Fixture>();
    fx->root                  = makeTempRoot();
    fx->sim                   = startDaSimServer();
    g_da_sim_response_content = "ok response";
    g_da_sim_tool_calls       = utilxx_base::Json::array();
    g_da_sim_delay_ms         = 0;

    fx->cfg                     = std::make_shared<agentxx::agent::AgentConfig>();
    fx->cfg->model.baseUrl      = "http://127.0.0.1:" + std::to_string(fx->sim.port);
    fx->cfg->model.apiKey       = "EMPTY";
    fx->cfg->model.modelName    = "default-model";
    fx->cfg->dataDir            = fx->root;
    fx->cfg->enableSessionStore = persist; // 列表/改名用例要求落盘

    fx->agent = std::make_shared<agentxx::agent::BaseAgent>(fx->cfg);
    co_await fx->agent->init();

    auto ex = co_await asio::this_coro::executor;

    agentxx::agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId = sessionId;
    fx->endpoint    = std::make_shared<agentxx::agent::SessionServerAgentIO>(ex, fx->agent, scCfg);

    auto [clientOwn, serverT] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    fx->endpoint
        ->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));

    fx->recorder = std::make_shared<SaRecorder>();
    fx->clientT  = std::move(clientOwn);

    asio::co_spawn(
        ex,
        [clientT = fx->clientT, recorder = fx->recorder]() -> asio::awaitable<void> {
            while (clientT->alive()) {
                auto msg = co_await clientT->recv();
                if (!msg) {
                    break;
                }
                recorder->handle(std::move(*msg));
            }
        },
        asio::detached
    );
    // 端点接收循环: 没有它, 客户端发来的会话管理消息不会被处理
    asio::co_spawn(
        ex,
        [ep = fx->endpoint]() -> asio::awaitable<void> {
            co_await ep->runTransportLoop();
        },
        asio::detached
    );
    co_return fx;
}

/// 端口事件循环让出若干毫秒 (等异步回执/落盘完成)
asio::awaitable<void> spin(std::chrono::milliseconds total) {
    auto timer = asio::steady_timer{co_await asio::this_coro::executor};
    timer.expires_after(total);
    co_await timer.async_wait(asio::use_awaitable);
}

/// 轮询等待条件成立 (超时返回条件当时取值)
asio::awaitable<bool>
    waitFor(std::function<bool()> cond, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        co_await spin(std::chrono::milliseconds{5});
    }
    co_return cond();
}

/// 会话在库里是否已创建目录
bool sessionExists(const std::shared_ptr<Fixture>& fx, const std::string& sessionId) {
    return fx->store()->sessionDataDirExists(sessionId);
}

/// 列表里按 sessionId 找条目 (无则返回 nullopt)
std::optional<agentxx::agent::SessionInfo>
    findSession(const agentxx::agent::WireSessionList& list, std::string_view sessionId) {
    for (const auto& s : list.sessions) {
        if (s.sessionId == sessionId) {
            return s;
        }
    }
    return std::nullopt;
}

} // namespace

/// 用例: 改名 + 检索 (标题/正文) + 客户端 API 入口
asio::awaitable<void> test_rename_and_search() {
    auto fx = co_await makeFixture("sess-admin");
    XX_TEST_EXPECT_TRUE(fx->store() != nullptr);

    // 先跑两轮对话: 两个会话各自落盘 (正文不同, 便于验证正文检索与命中片段)
    // 注: 回复正文用 ASCII — 本地模拟器按字节切片推送 SSE 片段, 多字节字符会被
    // 切在中间产生非法 UTF-8 片段 (真实 provider 不会这样切), 与本次验证无关
    g_da_sim_response_content = "first session reply";
    (void)co_await fx->agent->runTurnAsync("alpha", "讨论 编译期 优化 方案", nullptr);
    g_da_sim_response_content = "second session reply";
    (void)co_await fx->agent->runTurnAsync("beta", "记录清单", nullptr);
    // 第二轮的正文里才出现检索词: 标题 (首条用户消息预览) 不含它, 因此命中来自正文
    (void)co_await fx->agent->runTurnAsync("beta", "部署 步骤 的细节补充", nullptr);
    // 落盘是轮末权威写 + 节流刷新的组合: 稍等一会儿再断言 (超时即失败)
    const bool persisted = co_await waitFor(
        [&] { return sessionExists(fx, "alpha") && sessionExists(fx, "beta"); },
        std::chrono::seconds{5}
    );
    if (!persisted) {
        TEST_FAIL << "session write error: " << fx->store()->lastWriteError() << std::endl;
    }
    XX_TEST_EXPECT_TRUE(persisted);
    XX_TEST_EXPECT_TRUE(sessionExists(fx, "alpha"));
    XX_TEST_EXPECT_TRUE(sessionExists(fx, "beta"));

    // ---- 1) 改名成功: 标题与来源都落库 ----
    fx->clientT->send(
        agentxx::agent::WireMessage{
            agentxx::agent::WireRenameSession{.sessionId = "alpha", .title = "编译优化会话"}
        }
    );
    co_await waitFor([&] { return fx->recorder->renameCount() >= 1; }, std::chrono::seconds{3});
    auto renameRes = fx->recorder->lastRename();
    XX_TEST_EXPECT_HAS_VALUE(renameRes);
    if (renameRes) {
        XX_TEST_EXPECT_TRUE(renameRes->ok);
        XX_TEST_EXPECT_EQ(renameRes->sessionId, std::string{"alpha"});
        XX_TEST_EXPECT_EQ(renameRes->title, std::string{"编译优化会话"});
        XX_TEST_EXPECT_TRUE(renameRes->error.empty());
    }
    XX_TEST_EXPECT_EQ(fx->store()->sessionTitle("alpha"), std::string{"编译优化会话"});
    XX_TEST_EXPECT_EQ(fx->store()->sessionTitleSource("alpha"), std::string{"user"});

    // ---- 2) 空标题被拒: 保留原标题 ----
    const size_t renameCountBefore = fx->recorder->renameCount();
    fx->clientT->send(
        agentxx::agent::WireMessage{
            agentxx::agent::WireRenameSession{.sessionId = "alpha", .title = "   "}
        }
    );
    co_await waitFor(
        [&] { return fx->recorder->renameCount() > renameCountBefore; },
        std::chrono::seconds{3}
    );
    auto emptyRes = fx->recorder->lastRename();
    XX_TEST_EXPECT_HAS_VALUE(emptyRes);
    if (emptyRes) {
        XX_TEST_EXPECT_FALSE(emptyRes->ok);
        XX_TEST_EXPECT_TRUE(!emptyRes->error.empty());
        XX_TEST_EXPECT_TRUE(emptyRes->title.empty());
    }
    XX_TEST_EXPECT_EQ(fx->store()->sessionTitle("alpha"), std::string{"编译优化会话"});

    // ---- 3) 不存在的会话被拒, 且不会因此建出目录 ----
    fx->clientT->send(
        agentxx::agent::WireMessage{
            agentxx::agent::WireRenameSession{.sessionId = "not-exist", .title = "随便"}
        }
    );
    co_await waitFor(
        [&] { return fx->recorder->renameCount() > renameCountBefore + 1; },
        std::chrono::seconds{3}
    );
    auto missingRes = fx->recorder->lastRename();
    XX_TEST_EXPECT_HAS_VALUE(missingRes);
    if (missingRes) {
        XX_TEST_EXPECT_FALSE(missingRes->ok);
        XX_TEST_EXPECT_TRUE(missingRes->error.find("not-exist") != std::string::npos);
    }
    XX_TEST_EXPECT_FALSE(sessionExists(fx, "not-exist"));

    // ---- 4) 标题检索: 只回命中项 ----
    fx->clientT->send(
        agentxx::agent::WireMessage{
            agentxx::agent::WireListSessions{.limit = 50, .keyword = "编译优化"}
        }
    );
    co_await waitFor([&] { return fx->recorder->listCount() >= 1; }, std::chrono::seconds{3});
    auto titleHit = fx->recorder->lastList();
    XX_TEST_EXPECT_EQ(titleHit.sessions.size(), size_t{1});
    if (titleHit.sessions.size() == 1) {
        XX_TEST_EXPECT_EQ(titleHit.sessions[0].sessionId, std::string{"alpha"});
        XX_TEST_EXPECT_EQ(titleHit.sessions[0].title, std::string{"编译优化会话"});
        // 标题命中不产生未使用片段
        XX_TEST_EXPECT_TRUE(titleHit.sessions[0].snippet.empty());
    }
    // 检索结果是一次性结果集: 不再续取
    XX_TEST_EXPECT_FALSE(titleHit.hasMore);

    // ---- 5) 正文检索: 命中片段随条目回传 ----
    const size_t listCountBefore = fx->recorder->listCount();
    fx->clientT->send(
        agentxx::agent::WireMessage{
            agentxx::agent::WireListSessions{.limit = 50, .keyword = "部署 步骤"}
        }
    );
    co_await waitFor(
        [&] { return fx->recorder->listCount() > listCountBefore; },
        std::chrono::seconds{3}
    );
    auto contentHit = fx->recorder->lastList();
    XX_TEST_EXPECT_EQ(contentHit.sessions.size(), size_t{1});
    if (contentHit.sessions.size() == 1) {
        XX_TEST_EXPECT_EQ(contentHit.sessions[0].sessionId, std::string{"beta"});
        XX_TEST_EXPECT_TRUE(!contentHit.sessions[0].snippet.empty());
        XX_TEST_EXPECT_TRUE(
            contentHit.sessions[0].snippet.find("部署") != std::string::npos
        );
    }

    // ---- 6) 协议入口: WireListSessions(..., keyword) 直接发送 ----
    const size_t apiListBefore = fx->recorder->listCount();
    fx->clientT->send(
        agentxx::agent::WireMessage{
            agentxx::agent::WireListSessions{.limit = 50, .keyword = "优化"}
        }
    );
    co_await waitFor(
        [&] { return fx->recorder->listCount() > apiListBefore; },
        std::chrono::seconds{3}
    );
    auto apiHit = fx->recorder->lastList();
    XX_TEST_EXPECT_EQ(apiHit.sessions.size(), size_t{1});
    if (apiHit.sessions.size() == 1) {
        XX_TEST_EXPECT_EQ(apiHit.sessions[0].sessionId, std::string{"alpha"});
    }

    // ---- 7) 空关键词 = 普通列表: 两个会话都在 (改名后的标题生效) ----
    const size_t plainListBefore = fx->recorder->listCount();
    fx->clientT->send(
        agentxx::agent::WireMessage{agentxx::agent::WireListSessions{.limit = 50}}
    );
    co_await waitFor(
        [&] { return fx->recorder->listCount() > plainListBefore; },
        std::chrono::seconds{3}
    );
    auto plain = fx->recorder->lastList();
    XX_TEST_EXPECT_EQ(plain.sessions.size(), size_t{2});
    auto alphaInfo = findSession(plain, "alpha");
    XX_TEST_EXPECT_HAS_VALUE(alphaInfo);
    if (alphaInfo) {
        XX_TEST_EXPECT_EQ(alphaInfo->title, std::string{"编译优化会话"});
    }

    removeTempRoot(fx->root);
}

/// 用例: 无持久化 (内存模式) 时改名明确失败, 而不是静默成功
asio::awaitable<void> test_rename_without_persistence() {
    auto fx = co_await makeFixture("sess-admin-mem", /*persist=*/false);
    XX_TEST_EXPECT_TRUE(fx->store() == nullptr);

    fx->clientT->send(
        agentxx::agent::WireMessage{
            agentxx::agent::WireRenameSession{.sessionId = "alpha", .title = "内存模式"}
        }
    );
    co_await waitFor([&] { return fx->recorder->renameCount() >= 1; }, std::chrono::seconds{3});
    auto res = fx->recorder->lastRename();
    XX_TEST_EXPECT_HAS_VALUE(res);
    if (res) {
        XX_TEST_EXPECT_FALSE(res->ok);
        XX_TEST_EXPECT_TRUE(!res->error.empty());
    }
    removeTempRoot(fx->root);
}

asio::awaitable<TestResult> run_session_admin_tests() {
    co_await test_rename_and_search();
    co_await test_rename_without_persistence();
    co_return TestResult{g_sa_passed, g_sa_failed};
}

} // namespace test
} // namespace agentxx
