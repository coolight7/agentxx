#include "agentxx-test/core/test_prompt_stability.h"
#include "agentxx-test/core/test_agent.h" // 本地 LLM 模拟器 startDaSimServer/g_da_sim_*
#include "agentxx-test/test_coro_util.h"

#include "agentxx/agent/assembly_snapshot.h"
#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/agent/prompt.h"
#include "agentxx/agent/prompt_sources.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/middlewares/skill.h"
#include "agentxx/util/observability.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_pr_passed = 0;
int g_pr_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_pr_passed
#define XX_TEST_FAILED g_pr_failed

namespace agentxx {
namespace test {

using namespace utilxx_base;
namespace fs = std::filesystem;

namespace {

/// 统计文本出现次数
size_t countOccurrences(const std::string& text, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_pr_test_{}",
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

asio::awaitable<bool> waitFor(std::function<bool()> cond, int timeoutMs = 8000, int pollMs = 15) {
    auto ex       = co_await asio::this_coro::executor;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        asio::steady_timer t(ex);
        t.expires_after(std::chrono::milliseconds{pollMs});
        utilxx_base::AsioErrorCode ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return cond();
}

/// 测试夹具: 模拟器 + BaseAgent + 会话端点 (与 session_sync 模块同构)
struct Fixture {
    DaSimServer                                              sim;
    std::shared_ptr<agentxx::agent::AgentConfig>             cfg;
    std::shared_ptr<agentxx::agent::CodeAgent>               agent;
    std::shared_ptr<agentxx::agent::SessionServerAgentIO>    endpoint;
    std::shared_ptr<agentxx::agent::ChannelAgentIOTransport> clientT;
    std::string                                              sessionId;
    std::atomic<int>                                         turnResults{0};

    ~Fixture() {
        if (clientT) {
            clientT->close();
        }
        if (endpoint) {
            endpoint->stop();
        }
        sim.stop();
    }

    void sendInput(std::string text) {
        agentxx::agent::WireUserInput input;
        input.sessionId = sessionId;
        input.text      = std::move(text);
        clientT->send(agentxx::agent::WireMessage{std::move(input)});
    }
};

asio::awaitable<std::shared_ptr<Fixture>>
    makeFixture(std::string sessionId, const agentxx::agent::AgentConfig& tweaks) {
    auto fx                   = std::make_shared<Fixture>();
    fx->sim                   = startDaSimServer();
    g_da_sim_response_content = "prompt stability response";
    g_da_sim_tool_calls       = utilxx_base::Json::array();
    g_da_sim_fail_count       = 0;
    g_da_sim_delay_ms         = 0;

    const auto baseUrl       = "http://127.0.0.1:" + std::to_string(fx->sim.port);
    fx->cfg                  = std::make_shared<agentxx::agent::AgentConfig>(tweaks);
    fx->cfg->model.baseUrl   = baseUrl;
    fx->cfg->model.apiKey    = "EMPTY";
    fx->cfg->model.modelName = "default-model";
    fx->cfg->llmMaxRetry     = 1;

    // CodeAgent: 记忆文件 / 技能中间件在它上面加载 (BaseAgent 不装这些)
    fx->agent = std::make_shared<agentxx::agent::CodeAgent>(fx->cfg);
    co_await fx->agent->init();

    auto ex = co_await asio::this_coro::executor;

    agentxx::agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId = sessionId;
    fx->sessionId   = sessionId;
    fx->endpoint    = std::make_shared<agentxx::agent::SessionServerAgentIO>(ex, fx->agent, scCfg);

    auto [clientOwn, serverT] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    fx->endpoint->setTransport(
        std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT))
    );
    fx->clientT = std::move(clientOwn);

    asio::co_spawn(
        ex,
        keepAliveRun(
            [clientT = fx->clientT, turnResults = &fx->turnResults]() -> asio::awaitable<void> {
                while (clientT->alive()) {
                    auto msg = co_await clientT->recv();
                    if (!msg) {
                        break;
                    }
                    std::visit(
                        [turnResults](auto&& m) {
                            using T = std::decay_t<decltype(m)>;
                            if constexpr (std::is_same_v<T, agentxx::agent::WireTurnResult>) {
                                turnResults->fetch_add(1);
                            }
                        },
                        std::move(*msg)
                    );
                }
            }
        ),
        asio::detached
    );
    asio::co_spawn(
        ex,
        keepAliveRun([ep = fx->endpoint]() -> asio::awaitable<void> {
            co_await ep->runTransportLoop();
        }),
        asio::detached
    );
    asio::co_spawn(
        ex,
        keepAliveRun([ep = fx->endpoint]() -> asio::awaitable<void> {
            co_await ep->run();
        }),
        asio::detached
    );
    co_return fx;
}

asio::awaitable<void> runOneTurn(const std::shared_ptr<Fixture>& fx, std::string text) {
    const int before = fx->turnResults.load();
    fx->sendInput(std::move(text));
    co_await waitFor([&] {
        return fx->turnResults.load() > before;
    });
}

/// 请求体里的最后一条消息 (无消息时返回空对象)
utilxx_base::Json lastMessageOf(const utilxx_base::Json& request) {
    if (!request.contains("messages") || !request["messages"].is_array()
        || request["messages"].empty()) {
        return utilxx_base::Json::object();
    }
    return request["messages"][request["messages"].size() - 1];
}

/// 请求体里第一条 system 消息的正文 (无则空串)
std::string systemContentOf(const utilxx_base::Json& request) {
    if (!request.contains("messages") || !request["messages"].is_array()) {
        return {};
    }
    for (const auto& m : request["messages"]) {
        if (m.value("role", std::string{}) == "system") {
            return m.value("content", std::string{});
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// 1. 段落排序与 JSON 往返
// ---------------------------------------------------------------------------

void test_prompt_section_order() {
    agentxx::agent::AgentPrompt prompt;
    // 清空出厂附加段 (planning/skill/codegraph 等), 只测本次写入的段落
    prompt.appendSystemPrompts.clear();
    prompt.appendSystemPromptMeta.clear();

    prompt.setAppendSection("later", "L", 20, "plugin-x");
    prompt.setAppendSection("first", "F", 10);
    prompt.setAppendSection("same-a", "A", 15, "builtin");
    prompt.setAppendSection("same-b", "B", 15, "plugin-y");
    prompt.setAppendSection("empty", "", 5);

    auto ordered = prompt.orderedAppendSections();
    XX_TEST_EXPECT_EQ(ordered.size(), size_t{4}); // 空段落不参与
    if (ordered.size() == 4) {
        XX_TEST_EXPECT_EQ(std::string{ordered[0].key}, std::string{"first"});
        XX_TEST_EXPECT_EQ(std::string{ordered[1].key}, std::string{"same-a"});
        XX_TEST_EXPECT_EQ(std::string{ordered[2].key}, std::string{"same-b"});
        XX_TEST_EXPECT_EQ(std::string{ordered[3].key}, std::string{"later"});
        XX_TEST_EXPECT_EQ(ordered[0].order, 10);
        XX_TEST_EXPECT_EQ(std::string{ordered[3].source}, std::string{"plugin-x"});
    }

    // 键列表按名字升序 (诊断用)
    auto keys = prompt.appendSectionKeys();
    XX_TEST_EXPECT_EQ(keys.size(), size_t{5});

    // JSON 往返: 新形态 {text, order, source} 与旧形态 (纯字符串) 都接受
    auto j = prompt.toJson();
    XX_TEST_EXPECT_TRUE(j["appendSystemPrompts"]["later"].is_object());
    XX_TEST_EXPECT_EQ(j["appendSystemPrompts"]["later"].value("order", 0), 20);

    agentxx::agent::AgentPrompt restored;
    restored.appendSystemPrompts.clear();
    restored.appendSystemPromptMeta.clear();
    restored.fromJson(j);
    auto restoredOrdered = restored.orderedAppendSections();
    XX_TEST_EXPECT_EQ(restoredOrdered.size(), size_t{4});
    if (restoredOrdered.size() == 4) {
        XX_TEST_EXPECT_EQ(std::string{restoredOrdered[0].key}, std::string{"first"});
        XX_TEST_EXPECT_EQ(std::string{restoredOrdered[3].source}, std::string{"plugin-x"});
    }

    // 旧形态兼容: 字符串 = 正文 (order 0) / null = 删除
    utilxx_base::Json legacy;
    legacy["appendSystemPrompts"] = utilxx_base::Json{
        {"legacy-key", "legacy text"},
        {"later",      nullptr      },
    };
    restored.mergeFromJson(legacy);
    XX_TEST_EXPECT_TRUE(restored.appendSystemPrompts.count("later") == 0);
    XX_TEST_EXPECT_EQ(restored.appendSystemPrompts["legacy-key"], std::string{"legacy text"});
    XX_TEST_EXPECT_TRUE(restored.removeAppendSection("legacy-key"));
    XX_TEST_EXPECT_TRUE(restored.appendSystemPrompts.count("legacy-key") == 0);
    XX_TEST_EXPECT_TRUE(!restored.removeAppendSection("legacy-key"));

    // 哈希随 order 变化 (顺序属于提示词内容的一部分)
    agentxx::agent::AgentPrompt hashA;
    hashA.setAppendSection("k", "text", 0);
    agentxx::agent::AgentPrompt hashB;
    hashB.setAppendSection("k", "text", 7);
    XX_TEST_EXPECT_TRUE(hashA.promptHash() != hashB.promptHash());
}

// ---------------------------------------------------------------------------
// 2. 请求体结构与稳定段哈希
// ---------------------------------------------------------------------------

asio::awaitable<void> test_request_structure_and_stable_prefix() {
    const auto root       = makeTempRoot();
    const auto memoryFile = (fs::path{root} / "memory.md").string();
    {
        std::ofstream ofs{utilxx_base::utf8ToPath(memoryFile)};
        ofs << "MEMORY-MARKER: remember the build script\n";
        ofs << "MEMORY-MARKER: remember the test command\n";
    }

    agentxx::agent::AgentConfig tweaks;
    tweaks.memoryFilePaths.push_back(memoryFile);

    auto fx = co_await makeFixture("prompt-stability-session", tweaks);

    co_await runOneTurn(fx, "first turn");
    XX_TEST_EXPECT_TRUE(g_da_sim_request_count.load() >= 1);

    const auto request1 = g_da_sim_last_request;
    const auto system1  = systemContentOf(request1);
    XX_TEST_EXPECT_TRUE(!system1.empty());
    // 记忆内容进稳定段 (system 消息末尾): 随前缀一起被上游缓存命中
    XX_TEST_EXPECT_TRUE(system1.find("MEMORY-MARKER") != std::string::npos);

    // 动态段当前没有写入方: 请求末尾不再附加 <dynamic_context> 消息
    const auto last1 = lastMessageOf(request1);
    XX_TEST_EXPECT_EQ(last1.value("role", std::string{}), std::string{"user"});
    const auto lastContent1 = last1.value("content", std::string{});
    XX_TEST_EXPECT_TRUE(lastContent1.find("<dynamic_context") == std::string::npos);
    // 整条请求里记忆内容只有一份且落在 system 消息里 (每轮不重复发送)
    XX_TEST_EXPECT_EQ(countOccurrences(request1.dump(), "MEMORY-MARKER"), size_t{2});
    // 工具 schema 随请求下发 (稳定段的一部分)
    XX_TEST_EXPECT_TRUE(request1.contains("tools") && request1["tools"].is_array());
    XX_TEST_EXPECT_TRUE(!request1["tools"].empty());

    // 第二轮: system 消息逐字节不变 (前缀稳定), 记忆内容仍只有一份
    co_await runOneTurn(fx, "second turn");
    XX_TEST_EXPECT_TRUE(g_da_sim_request_count.load() >= 2);
    const auto request2 = g_da_sim_last_request;
    XX_TEST_EXPECT_EQ(systemContentOf(request2), system1);
    XX_TEST_EXPECT_EQ(countOccurrences(request2.dump(), "MEMORY-MARKER"), size_t{2});

    // 稳定段哈希: 连续请求不变 (缓存前缀有效), 变化次数为 0
    auto session = fx->agent->agentContext->sessions->get(fx->sessionId);
    XX_TEST_EXPECT_TRUE(session != nullptr);
    if (session) {
        XX_TEST_EXPECT_TRUE(session->stablePrefixHash() != 0);
        XX_TEST_EXPECT_EQ(session->stablePrefixChanges(), uint64_t{0});
    }

    // 系统消息本身 (会话上下文第一条) 含稳定段: 记忆内容随 system 一起下发
    if (session) {
        const auto& msgs = session->messages();
        XX_TEST_EXPECT_TRUE(!msgs.empty());
        if (!msgs.empty()) {
            XX_TEST_EXPECT_EQ(msgs.front().role, std::string{"system"});
            XX_TEST_EXPECT_TRUE(msgs.front().content.find("MEMORY-MARKER") != std::string::npos);
        }
    }

    removeTempRoot(root);
}

// ---------------------------------------------------------------------------
// 3. 技能同名裁决: 优先级高者生效, 来源展示给模型
// ---------------------------------------------------------------------------

/// 取 agent 加载的技能中间件 (未加载返回 nullptr)
agentxx::middleware::SkillMiddlewareHandle* findSkillMiddleware(agentxx::agent::CodeAgent& agent) {
    auto ctx = agent.agentContext;
    if (!ctx || !ctx->middlewareHandleContext) {
        return nullptr;
    }
    for (auto& handle : ctx->middlewareHandleContext->handles) {
        if (auto* skill = dynamic_cast<agentxx::middleware::SkillMiddlewareHandle*>(handle.get())) {
            return skill;
        }
    }
    return nullptr;
}

/// 写一个技能目录 (name 目录 + SKILL.md, 含 metadata)
void writeSkillDir(
    const fs::path&    parent,
    const std::string& name,
    const std::string& description
) {
    const auto dir = parent / name;
    fs::create_directories(dir);
    std::ofstream ofs{utilxx_base::utf8ToPath((dir / "SKILL.md").string())};
    ofs << "---\n";
    ofs << "name: " << name << "\n";
    ofs << "description: " << description << "\n";
    ofs << "---\n\n";
    ofs << "# " << name << "\n\nBody of " << description << "\n";
}

asio::awaitable<void> test_skill_name_adjudication() {
    const auto root = makeTempRoot();
    // 项目级目录 (优先级 0) 与插件级目录 (优先级 100) 各放一个同名技能
    const auto projectDir = fs::path{root} / "project-skills";
    const auto pluginDir  = fs::path{root} / "plugin-skills";
    fs::create_directories(projectDir);
    fs::create_directories(pluginDir);
    writeSkillDir(projectDir, "shared-skill", "PROJECT-VERSION-DESC");
    writeSkillDir(pluginDir, "shared-skill", "PLUGIN-VERSION-DESC");
    writeSkillDir(pluginDir, "plugin-only", "PLUGIN-ONLY-DESC");

    agentxx::agent::AgentConfig tweaks;
    tweaks.skillDirPaths.push_back(projectDir.string());

    auto fx = co_await makeFixture("skill-priority-session", tweaks);

    // 插件 (或主程序) 再注册一个低优先级目录
    auto* skillMiddleware = findSkillMiddleware(*fx->agent);
    XX_TEST_EXPECT_TRUE(skillMiddleware != nullptr);
    if (!skillMiddleware) {
        removeTempRoot(root);
        co_return;
    }
    skillMiddleware->addSkillDirs({pluginDir.string()}, 100, "plugin");
    XX_TEST_EXPECT_EQ(skillMiddleware->skillDirPathList().size(), size_t{2});

    co_await runOneTurn(fx, "first turn");

    // 同名技能只保留高优先级的一份: 项目级正文出现, 插件级正文不出现;
    // 插件目录里只剩不同名的那一个 (清单里每个名字一条)
    const auto request = g_da_sim_last_request.dump();
    XX_TEST_EXPECT_EQ(countOccurrences(request, "PROJECT-VERSION-DESC"), size_t{1});
    XX_TEST_EXPECT_EQ(countOccurrences(request, "PLUGIN-VERSION-DESC"), size_t{0});
    XX_TEST_EXPECT_EQ(countOccurrences(request, "plugin-skills"), size_t{1});
    XX_TEST_EXPECT_EQ(countOccurrences(request, "project-skills"), size_t{1});
    // 不同名技能都保留 (插件级也能生效)
    XX_TEST_EXPECT_TRUE(request.find("plugin-only") != std::string::npos);
    // 来源展示给模型
    XX_TEST_EXPECT_TRUE(request.find("source: config") != std::string::npos);

    // 被遮蔽的技能记录在案 (诊断/UI 展示)
    const auto& shadowed = skillMiddleware->shadowedSkills();
    XX_TEST_EXPECT_EQ(shadowed.size(), size_t{1});
    if (shadowed.size() == 1) {
        XX_TEST_EXPECT_EQ(shadowed[0][0], std::string{"shared-skill"});
        XX_TEST_EXPECT_TRUE(shadowed[0][1].find("plugin-skills") != std::string::npos);
        XX_TEST_EXPECT_TRUE(shadowed[0][2].find("project-skills") != std::string::npos);
    }

    // 摘除高优先级目录后, 同名技能由插件级目录接管 (重扫后生效)
    skillMiddleware->removeSkillDirs({projectDir.string()});
    co_await runOneTurn(fx, "second turn");
    const auto request2 = g_da_sim_last_request.dump();
    XX_TEST_EXPECT_TRUE(request2.find("PLUGIN-VERSION-DESC") != std::string::npos);
    XX_TEST_EXPECT_TRUE(skillMiddleware->shadowedSkills().empty());

    removeTempRoot(root);
}

// ---------------------------------------------------------------------------
// 4. 动态段机制保留 (预留给插件/今后功能)
//
// slot = Dynamic 的提示词来源仍作为**请求末尾的独立消息**附加, 不进 system 消息:
// 它位于稳定前缀之外, 每次调用全额重发且命中不了前缀缓存, 会明显降低 KV 缓存
// 命中率 (modelcall 里有存在性检查与告警)
// ---------------------------------------------------------------------------

/// 测试用动态段来源 (模拟插件): 在构造时把自己登记为 slot = Dynamic 的提示词来源
/// - 段落由注册表持有 (不再经 graphData 通道), 文本随请求末尾下发
class TestDynamicSectionHandle
    : public agentxx::middleware::BaseMiddlewareHandle<agentxx::middleware::BaseMiddlewareState> {
public:

    TestDynamicSectionHandle(
        std::string                                 source,
        std::string                                 text,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    ) :
        BaseMiddlewareHandle<agentxx::middleware::BaseMiddlewareState>(
            "TestDynamicSectionHandle",
            in_agentContext
        ),
        source_(std::move(source)),
        text_(std::move(text)) {
        if (auto ctxPtr = in_agentContext.lock(); ctxPtr && ctxPtr->promptSources) {
            registry_ = ctxPtr->promptSources;
            ctxPtr->promptSources->registerSource(agentxx::agent::PromptSource{
                .key                 = source_,
                .owner               = "test:dynamic",
                .order               = 900,
                .slot                = agentxx::agent::PromptSectionSlot::Dynamic,
                .perSession          = false,
                .refreshOnCompaction = true,
                .timeout             = std::chrono::milliseconds{1000},
                .build               = [this](const agentxx::agent::PromptRefreshContext& /*rc*/)
                    -> asio::awaitable<std::optional<std::string>> {
                    co_return text_;
                },
            });
        }
    }

    ~TestDynamicSectionHandle() override {
        if (auto registry = registry_.lock()) {
            registry->removeSource(source_, "test:dynamic");
        }
    }

private:

    std::string                                         source_;
    std::string                                         text_;
    std::weak_ptr<agentxx::agent::PromptSourceRegistry> registry_;
};

asio::awaitable<void> test_dynamic_section_tail() {
    const auto root       = makeTempRoot();
    const auto memoryFile = (fs::path{root} / "memory.md").string();
    {
        std::ofstream ofs{utilxx_base::utf8ToPath(memoryFile)};
        ofs << "MEMORY-MARKER: stable section content\n";
    }

    agentxx::agent::AgentConfig tweaks;
    tweaks.memoryFilePaths.push_back(memoryFile);

    auto fx = co_await makeFixture("dynamic-section-session", tweaks);

    // 模拟插件写入动态段: 本轮请求末尾应出现带来源标签的独立消息
    fx->agent->agentContext->middlewareHandleContext->handles.push_back(
        std::make_shared<TestDynamicSectionHandle>(
            "plugin-extra",
            "DYNAMIC-MARKER",
            fx->agent->agentContext
        )
    );

    co_await runOneTurn(fx, "dynamic turn");

    const auto request = g_da_sim_last_request;
    // 记忆内容在 system 稳定段; 动态段内容不进 system 消息
    const auto system = systemContentOf(request);
    XX_TEST_EXPECT_TRUE(system.find("MEMORY-MARKER") != std::string::npos);
    XX_TEST_EXPECT_TRUE(system.find("DYNAMIC-MARKER") == std::string::npos);

    const auto last        = lastMessageOf(request);
    const auto lastContent = last.value("content", std::string{});
    XX_TEST_EXPECT_EQ(last.value("role", std::string{}), std::string{"user"});
    XX_TEST_EXPECT_TRUE(
        lastContent.find("<dynamic_context source=\"plugin-extra\">") != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(lastContent.find("DYNAMIC-MARKER") != std::string::npos);

    // 动态段只在本请求可见: 不写进会话上下文 (权威 transcript)
    auto session = fx->agent->agentContext->sessions->get(fx->sessionId);
    XX_TEST_EXPECT_TRUE(session != nullptr);
    if (session) {
        XX_TEST_EXPECT_TRUE(
            session->llmMessagesJson().dump().find("DYNAMIC-MARKER") == std::string::npos
        );
    }

    removeTempRoot(root);
}

// ---------------------------------------------------------------------------
// 5. 压缩完成后的刷新时机 (A2 / A3 / A4)
//
// - 文件在外部被改动但**没有**发生压缩/资源变更 → 系统提示词不变 (证明没有监听)
// - 压缩完成 (markDirty(AfterCompaction)) 且内容未变 → 系统提示词逐字节不变,
//   stablePrefixChanges 不增长 (A2)
// - 压缩完成且内容变了 → 下一次请求的 system 含新内容, stablePrefixChanges +1 (A3)
// ---------------------------------------------------------------------------

asio::awaitable<void> test_refresh_after_compaction() {
    const auto root       = makeTempRoot();
    const auto memoryFile = (fs::path{root} / "memory.md").string();
    {
        std::ofstream ofs{utilxx_base::utf8ToPath(memoryFile)};
        ofs << "MEMORY-V1: first version\n";
    }

    agentxx::agent::AgentConfig tweaks;
    tweaks.memoryFilePaths.push_back(memoryFile);

    auto fx = co_await makeFixture("refresh-after-compaction-session", tweaks);

    co_await runOneTurn(fx, "first turn");
    const auto system1 = systemContentOf(g_da_sim_last_request);
    XX_TEST_EXPECT_TRUE(system1.find("MEMORY-V1") != std::string::npos);

    auto session = fx->agent->agentContext->sessions->get(fx->sessionId);
    XX_TEST_EXPECT_TRUE(session != nullptr);
    if (!session) {
        removeTempRoot(root);
        co_return;
    }

    // ---- A4: 文件在外部被改动, 但没有压缩/资源变更 → 不刷新 ----
    {
        std::ofstream ofs{utilxx_base::utf8ToPath(memoryFile)};
        ofs << "MEMORY-V2: second version\n";
    }
    // 保证时间戳与上一版不同 (部分文件系统时间戳精度较粗)
    {
        std::error_code ec;
        fs::last_write_time(
            utilxx_base::utf8ToPath(memoryFile),
            fs::file_time_type::clock::now() + std::chrono::seconds{3},
            ec
        );
    }
    co_await runOneTurn(fx, "second turn");
    const auto system2 = systemContentOf(g_da_sim_last_request);
    XX_TEST_EXPECT_EQ(system2, system1); // 逐字节相同: 没有文件监听
    XX_TEST_EXPECT_TRUE(system2.find("MEMORY-V2") == std::string::npos);
    XX_TEST_EXPECT_EQ(session->stablePrefixChanges(), uint64_t{0});

    // ---- A2: 压缩完成但内容未变 → 系统提示词不变, 前缀变化次数不增长 ----
    // (把文件内容改回与已注入的文本一致: 读盘后内容与当前段落相同)
    {
        std::ofstream ofs{utilxx_base::toCurrentSystemAbsolutePath(memoryFile)};
        ofs << "MEMORY-V1: first version\n";
    }
    fx->agent->agentContext->promptSources->markDirty(
        fx->sessionId,
        agentxx::agent::PromptRefreshReason::AfterCompaction
    );
    co_await runOneTurn(fx, "third turn");
    const auto system3 = systemContentOf(g_da_sim_last_request);
    XX_TEST_EXPECT_EQ(system3, system1);
    XX_TEST_EXPECT_EQ(session->stablePrefixChanges(), uint64_t{0});

    // ---- A3: 压缩完成且内容变了 → 下一次请求生效, 前缀变化次数恰好 +1 ----
    {
        std::ofstream ofs{utilxx_base::toCurrentSystemAbsolutePath(memoryFile)};
        ofs << "MEMORY-V3: third version\n";
    }
    fx->agent->agentContext->promptSources->markDirty(
        fx->sessionId,
        agentxx::agent::PromptRefreshReason::AfterCompaction
    );
    co_await runOneTurn(fx, "fourth turn");
    const auto system4 = systemContentOf(g_da_sim_last_request);
    XX_TEST_EXPECT_TRUE(system4.find("MEMORY-V3") != std::string::npos);
    XX_TEST_EXPECT_TRUE(system4.find("MEMORY-V1") == std::string::npos);
    XX_TEST_EXPECT_EQ(session->stablePrefixChanges(), uint64_t{1});

    // 刷新指标可见 (metrics.prompt_sources.changed_sections 至少 1 次)
    if (fx->agent->agentContext->metrics) {
        XX_TEST_EXPECT_TRUE(fx->agent->agentContext->metrics->promptChangedSections() >= 1);
        XX_TEST_EXPECT_TRUE(fx->agent->agentContext->metrics->promptRefreshes() >= 1);
    }

    // 加载快照里能看到来源状态 (不含正文)
    const auto snapshot = agentxx::agent::buildRuntimeSnapshot(*fx->agent->agentContext);
    XX_TEST_EXPECT_TRUE(snapshot.contains("prompt_sources"));
    if (snapshot.contains("prompt_sources")) {
        const auto& ps = snapshot["prompt_sources"];
        XX_TEST_EXPECT_TRUE(ps.value("count", 0) >= 2);                        // memory + skills
        XX_TEST_EXPECT_TRUE(ps.dump().find("MEMORY-V3") == std::string::npos); // 不含正文
    }

    removeTempRoot(root);
}

} // namespace

TestResult testPromptSectionOrder() {
    test_prompt_section_order();
    return TestResult{g_pr_passed, g_pr_failed};
}

asio::awaitable<TestResult> run_prompt_stability_tests() {
    // 本模块与 testPromptSectionOrder 共用计数器: 只上报本函数内的增量
    const int passedBefore = g_pr_passed;
    const int failedBefore = g_pr_failed;
    co_await test_request_structure_and_stable_prefix();
    co_await test_skill_name_adjudication();
    co_await test_dynamic_section_tail();
    co_await test_refresh_after_compaction();
    co_return TestResult{g_pr_passed - passedBefore, g_pr_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
