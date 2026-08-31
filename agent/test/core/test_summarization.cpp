// SummarizationMiddlewareHandle (上下文压缩中间件) 单元测试
//
// 设计要点 (见 agent/lib/include/agentxx/middlewares/summarization.h):
// - system prompt、最近的消息 不压缩
// - 两级触发: >= 65% 上限 时确定性压缩 (toolcall 去重/探索折叠 + 噪音清理,
//   不剥离 thinking, 不做 offload); >= 85% 上限 时 LLM 同上下文总结压缩
// - LLM 压缩通过 subagent 完成 (同上下文模式, FakeSubAgentManagerTool 模拟):
//   messages 结构化透传 (system + 压缩段 + 末尾 user 压缩指令), 指定父线程
//   thread_id, 仅提供 agentxx_share_store 工具 (模型自主外置长内容为
//   id + 极简摘要, 写入父会话 store), 禁用 enable_summarization (禁止二次压缩)
// - 压缩结果覆盖回: [system] | [user 压缩指令] | [assistant 摘要] | 最近消息
// - 压缩失败 >= 2 次 (同一轮内) 或 >= 95% 上限: 硬截断兜底
//
// 完整语义验证:
// - system 消息不能动: 不参与 tool 压缩 / 噪音清理 / LLM 总结, 原样保留
// - 按顺序先进行确定性压缩, 再由同上下文 subagent 压缩成一段总结
// - 消息角色顺序正确: 压缩后 = system | user(自动插入提示) |
//   assistant(压缩总结), 然后才是未压缩的最近消息 (保留原角色与顺序)

#include "test_summarization.h"

#include "agentxx/agent/model_registry.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/tools/subagent.h"
#include "fmt/format.h"
#include "neograph/graph/node.h"
#include "neograph/graph/run_context.h"
#include "neograph/graph/state.h"
#include "neograph/types.h"
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_sum_passed = 0;
int g_sum_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_sum_passed
#define XX_TEST_FAILED g_sum_failed

namespace agentxx {
namespace test {

namespace {

// ---------------------------------------------------------------------------
// 测试辅助: 伪造 subagent 管理器 (返回预设摘要, 不经过真实 LLM/中断)
// - 记录每次调用收到的参数 (用于断言同上下文压缩的请求构造)
// ---------------------------------------------------------------------------

class FakeSubAgentManagerTool : public agentxx::tools::SubAgentManagerTool {
public:

    /// 预设的摘要文本 (空串模拟压缩失败)
    std::string summary;
    /// 记录每次调用收到的参数
    std::vector<neograph::json> receivedArguments;
    /// 是否返回错误 (true 时返回 error json, 模拟 subagent 执行失败)
    bool failWithError = false;
    /// 是否抛出异常 (true 时 execute_async 抛 std::runtime_error,
    /// 模拟 subagent 执行链路异常, 经 catchErrorAsync 捕获降级)
    bool throwException = false;

    FakeSubAgentManagerTool(
        std::string_view                            in_nodeName,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    ) :
        SubAgentManagerTool(in_nodeName, std::move(in_agentContext)) {}

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override {
        receivedArguments.emplace_back(arguments);
        if (throwException) {
            throw std::runtime_error("fake subagent crashed");
        }
        if (failWithError) {
            co_return R"({"error":"fake subagent failed"})";
        }
        co_return summary;
    }
};

// ---------------------------------------------------------------------------
// 测试环境: AgentContext + 伪造 subagent + 中间件实例 + 预创建会话
// ---------------------------------------------------------------------------

struct SummarizationTestEnv {
    std::shared_ptr<agentxx::agent::AgentContext>                       ctx      = nullptr;
    std::shared_ptr<FakeSubAgentManagerTool>                            subagent = nullptr;
    std::shared_ptr<agentxx::middleware::SummarizationMiddlewareHandle> handle   = nullptr;
    std::string sessionId = "sum_test_thread";

    /// @param in_defaultMaxToken 中间件默认模型上限 (模型配置未指定时使用)
    /// @param in_recentRatio     最近消息 token 预算比例 (测试用小值使切分点可控)
    SummarizationTestEnv(
        size_t in_defaultMaxToken       = 2048,
        double in_asciiCharsPerToken    = 4.0,
        double in_unicodeCharsPerToken  = 1.1,
        double in_tokensPerImage        = 400.0,
        double in_extraTokensPerMessage = 3.0,
        double in_recentRatio           = 0.03
    ) {
        ctx                          = std::make_shared<agentxx::agent::AgentContext>();
        static asio::io_context s_ioCtx;
        ctx->bus                     = std::make_shared<agentxx::event::EventBus>(s_ioCtx.get_executor());
        ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
        ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
        ctx->modelRegistry           = std::make_shared<agentxx::agent::ModelProviderRegistry>();

        // 默认模型: 未指定 maxToken (0) → 使用中间件默认值 (回退逻辑)
        agentxx::agent::ModelConfig fallback;
        fallback.name      = "fallback";
        fallback.modelName = "fallback-model";
        ctx->modelRegistry->registerModel("fallback", fallback);

        agentxx::agent::ModelConfig small;
        small.name                  = "small";
        small.modelName             = "small-model";
        small.modelContenxtMaxToken = 1000;
        ctx->modelRegistry->registerModel("small", small);

        agentxx::agent::ModelConfig big;
        big.name                  = "big";
        big.modelName             = "big-model";
        big.modelContenxtMaxToken = 5000;
        ctx->modelRegistry->registerModel("big", big);

        agentxx::agent::ModelConfig thinking;
        thinking.name                  = "thinking";
        thinking.modelName             = "thinking-model";
        thinking.modelContenxtMaxToken = 1000;
        thinking.sendThinking          = true;
        ctx->modelRegistry->registerModel("thinking", thinking);

        // 默认模型名 = 首个注册的 "fallback"
        ctx->agentConfig->model.modelName = "fallback";

        // 伪造 subagent 管理器 (压缩走 subagent 路径)
        subagent = std::make_shared<FakeSubAgentManagerTool>("fake_subagent", ctx);
        subagent->registerOnBus(ctx->bus);

        handle = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(
            ctx,
            in_defaultMaxToken,
            in_asciiCharsPerToken,
            in_unicodeCharsPerToken,
            in_tokensPerImage,
            in_extraTokensPerMessage,
            in_recentRatio
        );

        // 预创建会话, 用于校验上下文统计发布
        ctx->sessions->getOrCreate(sessionId);
    }

    /// 当前会话使用的模型配置 (便于测试中切换模型)
    std::shared_ptr<agentxx::agent::Session> session() const {
        return ctx->sessions->get(sessionId);
    }
};

// ---------------------------------------------------------------------------
// 消息构造辅助
// ---------------------------------------------------------------------------

static neograph::ChatMessage makeMsg(std::string role, std::string content) {
    neograph::ChatMessage m;
    m.role    = std::move(role);
    m.content = std::move(content);
    return m;
}

static neograph::ToolCall makeToolcall(std::string id, std::string name, std::string arguments) {
    neograph::ToolCall tc;
    tc.id        = std::move(id);
    tc.name      = std::move(name);
    tc.arguments = std::move(arguments);
    return tc;
}

static neograph::ChatMessage
    makeAssistantToolcall(std::string content, std::vector<neograph::ToolCall> tcs) {
    auto m       = makeMsg("assistant", std::move(content));
    m.tool_calls = std::move(tcs);
    return m;
}

static neograph::ChatMessage
    makeToolResult(std::string callId, std::string toolName, std::string content) {
    auto m         = makeMsg("tool", std::move(content));
    m.tool_call_id = std::move(callId);
    m.tool_name    = std::move(toolName);
    return m;
}

/// 长消息: 用于载荷裁剪等场景
static std::string makeLongContent(size_t bytes = 2500) {
    return std::string(bytes, 'x');
}

/// 检查消息标记位
static bool msgHasFlag(const neograph::ChatMessage& m, neograph::MessageFlag flag) {
    return neograph::hasFlag(m.flags, flag);
}

/// 读取当前会话的上下文统计
static size_t contextTokensOf(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    std::string_view                                     sessionId
) {
    auto session = ctx->sessions->get(sessionId);
    return (nullptr != session && nullptr != session->contextStats)
               ? session->contextStats->contextTokens
               : 0;
}

static size_t maxContextTokensOf(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    std::string_view                                     sessionId
) {
    auto session = ctx->sessions->get(sessionId);
    return (nullptr != session && nullptr != session->contextStats)
               ? session->contextStats->maxContextTokens
               : 0;
}

// ---------------------------------------------------------------------------
// SummarizationToolHandle 构造辅助
// ---------------------------------------------------------------------------

/// filesystem 风格: 按 args["path"] 去重, 仅截断旧 response
static agentxx::middleware::SummarizationToolHandle makeReadFileHandle() {
    agentxx::middleware::SummarizationToolHandle th;
    th.generateDeduplicationKey = [](const neograph::json& args) -> std::optional<std::string> {
        if (!args.is_object()) {
            return std::nullopt;
        }
        auto it = args.find("path");
        if (it == args.end() || !it.value().is_string()) {
            return std::nullopt;
        }
        return it.value().get<std::string>();
    };
    th.truncateRequest  = nullptr;
    th.truncateResponse = [](neograph::ChatMessage& m) {
        m.content = "[Truncated Response]";
    };
    return th;
}

/// planning 风格: 恒定 key, 仅截断旧 request
static agentxx::middleware::SummarizationToolHandle makeConstantKeyHandle(std::string key) {
    agentxx::middleware::SummarizationToolHandle th;
    th.generateDeduplicationKey = [key = std::move(key)](const neograph::json&) {
        return std::optional<std::string>{key};
    };
    th.truncateRequest = [](neograph::ToolCall& tc) {
        tc.arguments = "[Truncated Request]";
    };
    th.truncateResponse = nullptr;
    return th;
}

/// share_store 风格: request/response 均截断
static agentxx::middleware::SummarizationToolHandle makeBothTruncateHandle() {
    auto th            = makeReadFileHandle();
    th.truncateRequest = [](neograph::ToolCall& tc) {
        tc.arguments = "[Truncated Request]";
    };
    return th;
}

// ---------------------------------------------------------------------------
// 运行 onModelcallRunFunc 的辅助: 构造 GraphState + RunContext + NodeInput
// ---------------------------------------------------------------------------

static asio::awaitable<std::vector<neograph::ChatMessage>> runModelcall(
    const std::shared_ptr<agentxx::middleware::SummarizationMiddlewareHandle>& handle,
    const std::shared_ptr<agentxx::agent::AgentContext>&                       ctx,
    std::string_view                                                           sessionId,
    std::vector<neograph::ChatMessage>                                         messages,
    std::optional<size_t> apiTokenUsage = std::nullopt
) {
    neograph::graph::GraphState state;
    state.init_channel(
        "messages",
        neograph::graph::ReducerType::OVERWRITE,
        nullptr,
        neograph::json::array()
    );
    neograph::json msgsJson;
    neograph::to_json(msgsJson, messages);
    state.overwrite("messages", std::move(msgsJson));

    // 注入/清除 api token usage (模拟上一次 LLM 调用返回的 usage)
    if (apiTokenUsage.has_value()) {
        ctx->middlewareHandleContext->setGraphDataItemValue(
            sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_LLMTokenUsage,
            neograph::json(*apiTokenUsage)
        );
    } else {
        ctx->middlewareHandleContext->removeGraphDataItem(
            sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_LLMTokenUsage
        );
    }

    neograph::graph::RunContext runCtx;
    runCtx.thread_id = std::string{sessionId};
    neograph::graph::NodeInput in{state, runCtx, nullptr};
    co_await handle->onModelcallRunFunc(in);
    co_return in.state.get_messages();
}

} // namespace

// ---------------------------------------------------------------------------
// 测试套件
// ---------------------------------------------------------------------------

asio::awaitable<TestResult> run_summarization_tests() {
    g_sum_passed = 0;
    g_sum_failed = 0;

    // ==================== countTokensForUtf8Str ====================

    {
        auto  env = std::make_shared<SummarizationTestEnv>();
        auto& h   = env->handle;

        // 空串 → 0
        XX_TEST_EXPECT_EQ(h->countTokensForUtf8Str(""), size_t{0});
        // ascii: 每 4 字符 1 token (整数除法)
        XX_TEST_EXPECT_EQ(h->countTokensForUtf8Str("abcd"), size_t{1});
        XX_TEST_EXPECT_EQ(h->countTokensForUtf8Str("abc"), size_t{0});
        XX_TEST_EXPECT_EQ(h->countTokensForUtf8Str(std::string(2000, 'a')), size_t{500});
        // unicode: 每 1.1 字符 1 token
        XX_TEST_EXPECT_EQ(h->countTokensForUtf8Str("你好世界"), size_t{3}); // 4/1.1 → 3
        XX_TEST_EXPECT_EQ(h->countTokensForUtf8Str("你好"), size_t{1});     // 2/1.1 → 1
        // 混合: ascii + unicode 分别折算
        XX_TEST_EXPECT_EQ(h->countTokensForUtf8Str("a你好b"), size_t{1}); // 0 + 1
        // 无效 utf-8 字节按 ascii 单字节处理
        XX_TEST_EXPECT_EQ(h->countTokensForUtf8Str("\x80\xff\x80\xff"), size_t{1});
        // 多字节前导字节: 整段按 1 个 unicode 字符 (续字节被 step 跳过)
        XX_TEST_EXPECT_EQ(h->countTokensForUtf8Str("\xf0\x80\x80\x80"), size_t{0}); // 1/1.1 → 0
        // 自定义 asciiCharsPerToken
        auto env2 = std::make_shared<SummarizationTestEnv>(2048, /*ascii*/ 2.0, 1.1, 400.0, 3.0);
        XX_TEST_EXPECT_EQ(env2->handle->countTokensForUtf8Str("abcdefgh"), size_t{4});
    }

    // ==================== countTokens ====================

    {
        auto  env = std::make_shared<SummarizationTestEnv>();
        auto& h   = env->handle;

        // 空输入 → 0
        XX_TEST_EXPECT_EQ(h->countTokens({}, {}), size_t{0});
        // system 消息: extra + 内容
        XX_TEST_EXPECT_EQ(h->countTokens({"hello"}, {}), size_t{4}); // 3 + 5/4
        // 普通消息: extra + role + content
        std::vector<neograph::ChatMessage> msgs{makeMsg("user", "test")};
        XX_TEST_EXPECT_EQ(h->countTokens({}, msgs), size_t{5}); // 3 + 4/4 + 4/4
        // tool_calls: id + name + arguments 都计入
        auto m = makeAssistantToolcall("", {makeToolcall("call_1", "read_file", R"({"a":1})")});
        XX_TEST_EXPECT_EQ(h->countTokens({}, {m}), size_t{9}); // 3 + 9/4 + 0 + (6+9+7)/4
        // 图片/音视频: 每个 tokensPerImage (400)
        auto mm       = makeMsg("user", "test");
        mm.image_urls = {"http://a/1.png", "http://a/2.png"};
        XX_TEST_EXPECT_EQ(h->countTokens({}, {mm}), size_t{805}); // 5 + 2*400
        auto mv       = makeMsg("user", "test");
        mv.audio_urls = {"http://a/1.mp3"};
        mv.video_urls = {"http://a/1.mp4"};
        XX_TEST_EXPECT_EQ(h->countTokens({}, {mv}), size_t{805}); // 5 + 400 + 400
        // reasoning: 仅 countThinking=true 时计入
        auto mr              = makeMsg("assistant", "hi");
        mr.reasoning_content = "think";
        XX_TEST_EXPECT_EQ(h->countTokens({}, {mr}, false), size_t{5}); // 3 + 9/4 + 0
        XX_TEST_EXPECT_EQ(h->countTokens({}, {mr}, true), size_t{6});  // + 5/4
    }

    // ==================== messagesToText ====================

    {
        auto  env = std::make_shared<SummarizationTestEnv>();
        auto& h   = env->handle;

        // 空输入 → 空串
        XX_TEST_EXPECT_EQ(h->messagesToText({}, false), std::string{""});
        // 默认跳过 system
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "hi"),
        };
        XX_TEST_EXPECT_EQ(h->messagesToText(msgs, false), std::string{"[user]: hi\n"});
        // includeSystem=true 时包含 system
        XX_TEST_EXPECT_EQ(
            h->messagesToText(msgs, true),
            std::string{"[system]: sys\n[user]: hi\n"}
        );
        // tool_calls 格式
        std::vector<neograph::ChatMessage> tcs{
            makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"a"})")})
        };
        XX_TEST_EXPECT_EQ(
            h->messagesToText(tcs, false),
            std::string{"[assistant]: \n  - [toolcall:read_file] {\"path\":\"a\"}\n"}
        );
    }

    // ==================== cleanNoiseMessages ====================

    {
        auto  env = std::make_shared<SummarizationTestEnv>();
        auto& h   = env->handle;

        // --- A. 空消息删除; thinking (reasoning_content) 保留 ---
        {
            std::vector<neograph::ChatMessage> msgs{
                makeMsg("user", ""), // 空内容
                makeMsg("assistant", ""),
                [&]() {
                    auto m              = makeMsg("assistant", "a1");
                    m.reasoning_content = "think-1";
                    return m;
                }(),
                makeMsg("user", "u2"),
            };
            h->cleanNoiseMessages(msgs);
            XX_TEST_EXPECT_EQ(msgs.size(), size_t{2});
            // thinking 保留 (不剥离)
            XX_TEST_EXPECT_EQ(msgs[0].content, std::string{"a1"});
            XX_TEST_EXPECT_EQ(msgs[0].reasoning_content, std::string{"think-1"});
            XX_TEST_EXPECT_EQ(msgs[1].content, std::string{"u2"});
        }

        // --- B. 相邻完全相同的消息只保留最后一条 ---
        {
            std::vector<neograph::ChatMessage> msgs{
                makeMsg("user", "dup"),
                makeMsg("user", "dup"),
                makeMsg("user", "other"),
            };
            h->cleanNoiseMessages(msgs);
            XX_TEST_EXPECT_EQ(msgs.size(), size_t{2});
            XX_TEST_EXPECT_EQ(msgs[0].content, std::string{"dup"});
            XX_TEST_EXPECT_EQ(msgs[1].content, std::string{"other"});
        }

        // --- C. 连续 AutoInserted 噪音只保留最后一条 ---
        {
            std::vector<neograph::ChatMessage> msgs{
                makeMsg("user", "u1"),
                [&]() {
                    auto m  = makeMsg("assistant", "[Please continue]");
                    m.flags = neograph::MessageFlag::AutoInserted;
                    return m;
                }(),
                [&]() {
                    auto m  = makeMsg("assistant", "[Please continue]");
                    m.flags = neograph::MessageFlag::AutoInserted;
                    return m;
                }(),
                [&]() {
                    auto m  = makeMsg("assistant", "[User cancelled]");
                    m.flags = neograph::MessageFlag::AutoInserted;
                    return m;
                }(),
                makeMsg("user", "u2"),
            };
            h->cleanNoiseMessages(msgs);
            XX_TEST_EXPECT_EQ(msgs.size(), size_t{3});
            XX_TEST_EXPECT_EQ(msgs[0].content, std::string{"u1"});
            // 连续噪音段折叠为最后一条
            XX_TEST_EXPECT_EQ(msgs[1].content, std::string{"[User cancelled]"});
            XX_TEST_EXPECT_EQ(msgs[2].content, std::string{"u2"});
        }

        // --- D. 非连续噪音 (被正常消息隔开) 各自保留 ---
        {
            std::vector<neograph::ChatMessage> msgs{
                [&]() {
                    auto m  = makeMsg("assistant", "[Please continue]");
                    m.flags = neograph::MessageFlag::AutoInserted;
                    return m;
                }(),
                makeMsg("user", "u1"),
                [&]() {
                    auto m  = makeMsg("assistant", "[Exception aborted]");
                    m.flags = neograph::MessageFlag::AutoInserted;
                    return m;
                }(),
            };
            h->cleanNoiseMessages(msgs);
            XX_TEST_EXPECT_EQ(msgs.size(), size_t{3});
        }

        // --- E. 非 AutoInserted 的相似内容不折叠; system 不受影响 ---
        {
            std::vector<neograph::ChatMessage> msgs{
                makeMsg("system", "sys"),
                makeMsg("user", "[Please continue]"), // 用户手动输入的同文案, 非 AutoInserted
                makeMsg("user", "[Please continue]"),
            };
            h->cleanNoiseMessages(msgs);
            // 相邻相同 user 消息仍按"重复保留最后一条"处理
            XX_TEST_EXPECT_EQ(msgs.size(), size_t{2});
            XX_TEST_EXPECT_EQ(msgs[0].content, std::string{"sys"});
            XX_TEST_EXPECT_EQ(msgs[1].content, std::string{"[Please continue]"});
        }
    }

    // ==================== doSummarizeWithLLM (同上下文压缩) ====================

    {
        // --- A. 无 subagentManager → 返回空串 (降级为不压缩) ---
        {
            auto env = std::make_shared<SummarizationTestEnv>();
            env->subagent->unregisterFromBus();
            std::vector<neograph::ChatMessage> msgs{makeMsg("user", "hi")};
            auto r = co_await env->handle->doSummarizeWithLLM(env->sessionId, msgs);
            XX_TEST_EXPECT_EQ(r, std::string{""});
        }

        // --- B. 空消息 → 返回空串 ---
        {
            auto env = std::make_shared<SummarizationTestEnv>();
            auto r   = co_await env->handle->doSummarizeWithLLM(env->sessionId, {});
            XX_TEST_EXPECT_EQ(r, std::string{""});
        }

        // --- C. 同上下文 subagent 请求构造 + 返回摘要 ---
        {
            auto env               = std::make_shared<SummarizationTestEnv>();
            env->subagent->summary = "fake summary";

            std::vector<neograph::ChatMessage> msgs{
                makeMsg("system", "sys"),
                makeMsg("user", "u1"),
                makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
                makeToolResult("c1", "read_file", "r1"),
            };
            auto r = co_await env->handle->doSummarizeWithLLM(env->sessionId, msgs);
            XX_TEST_EXPECT_EQ(r, std::string{"fake summary"});

            // 仅调用一次 subagent
            XX_TEST_EXPECT_EQ(env->subagent->receivedArguments.size(), size_t{1});
            const auto& args = env->subagent->receivedArguments[0];
            // 子代理名
            XX_TEST_EXPECT_EQ(args.value("subagent", std::string{}), std::string{"subagent_task"});
            // 同上下文: 指定父线程
            XX_TEST_EXPECT_EQ(args.value("sessionId", std::string{}), env->sessionId);
            // 工具策略: 仅 share_store (模型自主外置长内容)
            XX_TEST_EXPECT_TRUE(args["tools"].is_array());
            XX_TEST_EXPECT_EQ(args["tools"].size(), size_t{1});
            XX_TEST_EXPECT_EQ(
                args["tools"][0].get<std::string>(),
                std::string{"agentxx_share_store"}
            );
            // 禁止二次压缩
            XX_TEST_EXPECT_TRUE(args["enable_summarization"].is_boolean());
            XX_TEST_EXPECT_FALSE(args["enable_summarization"].get<bool>());

            // 结构化消息透传: system + 原消息 + 末尾追加 user 压缩指令 (无文本转录)
            const auto& reqMsgs = args["messages"];
            XX_TEST_EXPECT_TRUE(reqMsgs.is_array());
            XX_TEST_EXPECT_EQ(reqMsgs.size(), msgs.size() + 1);
            for (size_t i = 0; i < msgs.size(); ++i) {
                XX_TEST_EXPECT_EQ(reqMsgs[i].value("role", std::string{}), msgs[i].role);
                XX_TEST_EXPECT_EQ(reqMsgs[i].value("content", std::string{}), msgs[i].content);
            }
            // 压缩指令: 最后一条 user 消息, 含 share_store 提示
            const auto& promptMsg = reqMsgs.back();
            XX_TEST_EXPECT_EQ(promptMsg.value("role", std::string{}), std::string{"user"});
            XX_TEST_EXPECT_TRUE(
                promptMsg.value("content", std::string{}).find("Summarize") != std::string::npos
            );
            XX_TEST_EXPECT_TRUE(
                promptMsg.value("content", std::string{}).find("agentxx_share_store")
                != std::string::npos
            );
        }

        // --- D. 压缩失败 (subagent 返回空串, 与真实失败路径一致) → 空串 ---
        {
            auto env               = std::make_shared<SummarizationTestEnv>();
            env->subagent->summary = ""; // 模拟压缩失败 (子代理失败时 content 为空)
            std::vector<neograph::ChatMessage> msgs{makeMsg("user", "u1")};
            auto r = co_await env->handle->doSummarizeWithLLM(env->sessionId, msgs);
            XX_TEST_EXPECT_EQ(r, std::string{""});
            // 仍发起了一次 subagent 调用
            XX_TEST_EXPECT_EQ(env->subagent->receivedArguments.size(), size_t{1});
        }

        // --- E. 请求载荷裁剪: 压缩段超限时丢弃最旧消息 ---
        {
            auto env = std::make_shared<SummarizationTestEnv>();
            env->session()->setModelName("small"); // max=1000, 裁剪阈值 950
            env->subagent->summary = "S";
            auto big1              = makeMsg("user", makeLongContent(2500)); // ~625 token
            auto big2              = makeMsg("user", makeLongContent(2500)); // ~625 token
            std::vector<neograph::ChatMessage> msgs{
                makeMsg("system", "sys"),
                big1,
                big2,
            };
            auto r = co_await env->handle->doSummarizeWithLLM(env->sessionId, msgs);
            XX_TEST_EXPECT_EQ(r, std::string{"S"});
            XX_TEST_EXPECT_EQ(env->subagent->receivedArguments.size(), size_t{1});
            const auto& reqMsgs = env->subagent->receivedArguments[0]["messages"];
            // 最旧的 big1 被丢弃, 仅保留 system + big2 + 指令
            XX_TEST_EXPECT_EQ(reqMsgs.size(), size_t{3});
            XX_TEST_EXPECT_EQ(reqMsgs[0].value("role", std::string{}), std::string{"system"});
            XX_TEST_EXPECT_EQ(reqMsgs[1].value("content", std::string{}), big2.content);
            // 指令中提示丢弃了最旧消息
            XX_TEST_EXPECT_TRUE(
                reqMsgs[2].value("content", std::string{}).find("oldest 1 message")
                != std::string::npos
            );
        }

        // --- F. AgentPrompt 定制压缩提示词生效: 模板可经 prompt 覆盖 ---
        {
            auto env               = std::make_shared<SummarizationTestEnv>();
            env->subagent->summary = "S";

            // 默认模板非空且含关键内容
            const auto& p = env->ctx->agentConfig->prompt;
            XX_TEST_EXPECT_FALSE(p.summarizationPrompt.empty());
            XX_TEST_EXPECT_TRUE(p.summarizationPrompt.find("Summarize") != std::string::npos);
            XX_TEST_EXPECT_TRUE(p.summarizationPrompt.find("{omitted_note}") != std::string::npos);
            XX_TEST_EXPECT_TRUE(p.summarizationPrompt.find("{max_words}") != std::string::npos);

            // 定制模板
            env->ctx->agentConfig->prompt.summarizationPrompt
                = "CUSTOM SUMMARIZE PROMPT {omitted_note}max {max_words}";
            std::vector<neograph::ChatMessage> msgs{makeMsg("user", "u1")};
            auto r = co_await env->handle->doSummarizeWithLLM(env->sessionId, msgs);
            XX_TEST_EXPECT_EQ(r, std::string{"S"});
            XX_TEST_EXPECT_EQ(env->subagent->receivedArguments.size(), size_t{1});
            // 定制模板生效: 占位符被替换 (omitted_note 为空, max_words=2048/4=512)
            const auto& reqMsgs = env->subagent->receivedArguments[0]["messages"];
            XX_TEST_EXPECT_EQ(
                reqMsgs.back().value("content", std::string{}),
                std::string{"CUSTOM SUMMARIZE PROMPT max 512"}
            );
        }

        // --- G. 空压缩模板 → 降级为不压缩 (返回空串, 不发起 subagent) ---
        {
            auto env = std::make_shared<SummarizationTestEnv>();
            env->ctx->agentConfig->prompt.summarizationPrompt = "";
            std::vector<neograph::ChatMessage> msgs{makeMsg("user", "u1")};
            auto r = co_await env->handle->doSummarizeWithLLM(env->sessionId, msgs);
            XX_TEST_EXPECT_EQ(r, std::string{""});
            XX_TEST_EXPECT_EQ(env->subagent->receivedArguments.size(), size_t{0});
        }

        // --- H. AgentPrompt 序列化往返: toJson → mergeFromJson 保留新字段,
        //           缺失字段不影响既有字段 ---
        {
            agentxx::agent::AgentPrompt p;
            const auto&                 j = p.toJson();
            XX_TEST_EXPECT_EQ(j["summarizationPrompt"].get<std::string>(), p.summarizationPrompt);
            // 往返: 定制后序列化再合并, 字段一致
            agentxx::agent::AgentPrompt p2;
            p2.summarizationPrompt = "CUSTOM";
            agentxx::agent::AgentPrompt p3;
            p3.mergeFromJson(p2.toJson());
            XX_TEST_EXPECT_EQ(p3.summarizationPrompt, std::string{"CUSTOM"});
            // 缺失字段合并: 保持原值
            neograph::json partial = neograph::json{
                {"systemPrompt", "SYS"}
            };
            p3.mergeFromJson(partial);
            XX_TEST_EXPECT_EQ(p3.summarizationPrompt, std::string{"CUSTOM"});
            XX_TEST_EXPECT_EQ(p3.systemPrompt, std::string{"SYS"});
            // promptHash 覆盖新字段 (定制后哈希变化)
            agentxx::agent::AgentPrompt p4;
            agentxx::agent::AgentPrompt p5;
            p5.summarizationPrompt = "CUSTOM";
            XX_TEST_EXPECT_FALSE(p4.promptHash() == p5.promptHash());
        }
    }

    // ==================== doSummarizeToolcall / foldExploratoryToolcalls ====================

    {
        auto  env = std::make_shared<SummarizationTestEnv>();
        auto& h   = env->handle;

        // --- A. response 去重 (filesystem 风格): 旧 response 截断, 新 response 保留,
        //       assistant request 不受影响
        //       (中间插入 user 消息打断探索折叠, 避免 3 组连续同工具被折叠误伤) ---
        {
            h->summarizationToolHandles["read_file"] = makeReadFileHandle();
            std::vector<neograph::ChatMessage> msgs{
                makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
                makeToolResult("c1", "read_file", "r1"),
                makeMsg("user", "pause"),
                makeAssistantToolcall("", {makeToolcall("c2", "read_file", R"({"path":"B"})")}),
                makeToolResult("c2", "read_file", "r2"),
                makeAssistantToolcall("", {makeToolcall("c3", "read_file", R"({"path":"A"})")}),
                makeToolResult("c3", "read_file", "r3"),
            };
            h->doSummarizeToolcall(msgs);
            XX_TEST_EXPECT_EQ(msgs.size(), size_t{7});
            // 旧 (path=A) 的 response 被截断 (c1 的 r1 被 c3 的 path A 覆盖)
            XX_TEST_EXPECT_EQ(msgs[1].content, std::string{"[Truncated Response]"});
            // path=B 与新 path=A 的 response 保留
            XX_TEST_EXPECT_EQ(msgs[4].content, std::string{"r2"});
            XX_TEST_EXPECT_EQ(msgs[6].content, std::string{"r3"});
            // assistant request 不被截断 (truncateRequest == nullptr)
            XX_TEST_EXPECT_EQ(msgs[0].tool_calls[0].name, std::string{"read_file"});
            XX_TEST_EXPECT_EQ(msgs[5].tool_calls[0].arguments, std::string{R"({"path":"A"})"});
        }

        // --- B. request 去重 (planning 风格): 恒定 key, 仅保留最后一个 request ---
        {
            h->summarizationToolHandles["write_planning"] = makeConstantKeyHandle("planning:");
            std::vector<neograph::ChatMessage> msgs{
                makeMsg("system", "sys"),
                makeAssistantToolcall("", {makeToolcall("p1", "write_planning", R"({"a":1})")}),
                makeAssistantToolcall("", {makeToolcall("p2", "write_planning", R"({"a":2})")}),
                makeAssistantToolcall("", {makeToolcall("p3", "write_planning", R"({"a":3})")}),
            };
            h->doSummarizeToolcall(msgs);
            // 索引 0 是 system 消息; 旧 request 全部截断, 最后一个保留
            XX_TEST_EXPECT_EQ(msgs[1].tool_calls[0].arguments, std::string{"[Truncated Request]"});
            XX_TEST_EXPECT_EQ(msgs[2].tool_calls[0].arguments, std::string{"[Truncated Request]"});
            XX_TEST_EXPECT_EQ(msgs[3].tool_calls[0].arguments, std::string{R"({"a":3})"});
        }

        // --- C. request+response 均截断 (share_store 风格) ---
        {
            h->summarizationToolHandles["read_file"] = makeBothTruncateHandle();
            std::vector<neograph::ChatMessage> msgs{
                makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
                makeToolResult("c1", "read_file", "r1"),
                makeAssistantToolcall("", {makeToolcall("c2", "read_file", R"({"path":"A"})")}),
                makeToolResult("c2", "read_file", "r2"),
            };
            h->doSummarizeToolcall(msgs);
            // 旧 response 截断
            XX_TEST_EXPECT_EQ(msgs[1].content, std::string{"[Truncated Response]"});
            // 新 response 保留
            XX_TEST_EXPECT_EQ(msgs[3].content, std::string{"r2"});
            // 去重 key 已由新 response 占用, 新 request 也被截断 (共享 lastWriteIndex)
            XX_TEST_EXPECT_EQ(msgs[2].tool_calls[0].arguments, std::string{"[Truncated Request]"});
            // 索引 0 的 request 同样被截断 (循环含索引 0, 仅保留最后一组)
            XX_TEST_EXPECT_EQ(msgs[0].tool_calls[0].arguments, std::string{"[Truncated Request]"});
        }

        // --- D. 非法 JSON 参数: 跳过该条, 不崩溃 ---
        {
            h->summarizationToolHandles["read_file"] = makeReadFileHandle();
            std::vector<neograph::ChatMessage> msgs{
                makeAssistantToolcall("", {makeToolcall("c1", "read_file", "not-json{{")}),
                makeToolResult("c1", "read_file", "r1"),
            };
            h->doSummarizeToolcall(msgs);
            XX_TEST_EXPECT_EQ(msgs[1].content, std::string{"r1"});
        }

        // --- E. 生成 key 返回 nullopt (参数缺 path) → 不截断 ---
        {
            std::vector<neograph::ChatMessage> msgs{
                makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"file":"x"})")}),
                makeToolResult("c1", "read_file", "r1"),
                makeAssistantToolcall("", {makeToolcall("c2", "read_file", R"({"file":"x"})")}),
                makeToolResult("c2", "read_file", "r2"),
            };
            h->doSummarizeToolcall(msgs);
            XX_TEST_EXPECT_EQ(msgs[1].content, std::string{"r1"});
            XX_TEST_EXPECT_EQ(msgs[3].content, std::string{"r2"});
        }

        // --- F. tool 结果找不到对应的 assistant toolcall (悬空 tool_call_id) → 不崩溃 ---
        {
            std::vector<neograph::ChatMessage> msgs{
                makeMsg("user", "u"),
                makeToolResult("ghost", "read_file", "r1"),
            };
            h->doSummarizeToolcall(msgs);
            XX_TEST_EXPECT_EQ(msgs[1].content, std::string{"r1"});
        }

        // --- G. 未注册的 tool 名称 → 不处理 ---
        {
            std::vector<neograph::ChatMessage> msgs{
                makeAssistantToolcall("", {makeToolcall("c1", "unknown_tool", R"({"path":"A"})")}),
                makeToolResult("c1", "unknown_tool", "r1"),
                makeAssistantToolcall("", {makeToolcall("c2", "unknown_tool", R"({"path":"A"})")}),
                makeToolResult("c2", "unknown_tool", "r2"),
            };
            h->doSummarizeToolcall(msgs);
            XX_TEST_EXPECT_EQ(msgs[1].content, std::string{"r1"});
            XX_TEST_EXPECT_EQ(msgs[3].content, std::string{"r2"});
        }

        // --- H. 探索折叠: 连续 3 组同工具 (读类) → 只保留最后一组 ---
        {
            h->summarizationToolHandles["read_file"] = makeReadFileHandle();
            std::vector<neograph::ChatMessage> msgs{
                makeMsg("user", "u0"),
                makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
                makeToolResult("c1", "read_file", "rA"),
                makeAssistantToolcall("", {makeToolcall("c2", "read_file", R"({"path":"B"})")}),
                makeToolResult("c2", "read_file", "rB"),
                makeAssistantToolcall("", {makeToolcall("c3", "read_file", R"({"path":"C"})")}),
                makeToolResult("c3", "read_file", "rC"),
                makeMsg("user", "u1"),
            };
            h->doSummarizeToolcall(msgs);
            // u0 + 最后一组 (c3/rC) + u1 = 4 条
            XX_TEST_EXPECT_EQ(msgs.size(), size_t{4});
            XX_TEST_EXPECT_EQ(msgs[0].content, std::string{"u0"});
            XX_TEST_EXPECT_EQ(msgs[1].tool_calls[0].arguments, std::string{R"({"path":"C"})"});
            XX_TEST_EXPECT_EQ(msgs[2].content, std::string{"rC"});
            XX_TEST_EXPECT_EQ(msgs[3].content, std::string{"u1"});
        }

        // --- I. 探索折叠: 2 组不折叠 ---
        {
            h->summarizationToolHandles["read_file"] = makeReadFileHandle();
            std::vector<neograph::ChatMessage> msgs{
                makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
                makeToolResult("c1", "read_file", "rA"),
                makeAssistantToolcall("", {makeToolcall("c2", "read_file", R"({"path":"B"})")}),
                makeToolResult("c2", "read_file", "rB"),
            };
            h->doSummarizeToolcall(msgs);
            XX_TEST_EXPECT_EQ(msgs.size(), size_t{4});
        }

        // --- J. 探索折叠: user 消息打断连续段 → 不折叠 ---
        {
            h->summarizationToolHandles["read_file"] = makeReadFileHandle();
            std::vector<neograph::ChatMessage> msgs{
                makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
                makeToolResult("c1", "read_file", "rA"),
                makeMsg("user", "interrupt"),
                makeAssistantToolcall("", {makeToolcall("c2", "read_file", R"({"path":"B"})")}),
                makeToolResult("c2", "read_file", "rB"),
                makeAssistantToolcall("", {makeToolcall("c3", "read_file", R"({"path":"C"})")}),
                makeToolResult("c3", "read_file", "rC"),
            };
            h->doSummarizeToolcall(msgs);
            // user 打断: 前后各 2 组/3 组? 后面 c2,c3 连续 2 组 → 不折叠
            XX_TEST_EXPECT_EQ(msgs.size(), size_t{7});
        }

        // --- K. 探索折叠: 写类工具 (仅 truncateRequest) 不折叠 ---
        {
            h->summarizationToolHandles["write_planning"] = makeConstantKeyHandle("planning:");
            std::vector<neograph::ChatMessage> msgs{
                makeAssistantToolcall("", {makeToolcall("p1", "write_planning", R"({"a":1})")}),
                makeAssistantToolcall("", {makeToolcall("p2", "write_planning", R"({"a":2})")}),
                makeAssistantToolcall("", {makeToolcall("p3", "write_planning", R"({"a":3})")}),
            };
            h->doSummarizeToolcall(msgs);
            XX_TEST_EXPECT_EQ(msgs.size(), size_t{3});
        }

        // --- L. 探索折叠: 多工具调用打断连续段 ---
        {
            h->summarizationToolHandles["read_file"] = makeReadFileHandle();
            std::vector<neograph::ChatMessage> msgs{
                makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
                makeToolResult("c1", "read_file", "rA"),
                // 多工具调用 (read_file + glob) → 打断
                makeAssistantToolcall(
                    "",
                    {
                        makeToolcall("c2", "read_file", R"({"path":"B"})"),
                        makeToolcall("g2", "glob", R"({"pattern":"*.cpp"})"),
                    }
                ),
                makeToolResult("c2", "read_file", "rB"),
                makeToolResult("g2", "glob", "[a.cpp, b.cpp]"),
                makeAssistantToolcall("", {makeToolcall("c3", "read_file", R"({"path":"C"})")}),
                makeToolResult("c3", "read_file", "rC"),
            };
            h->doSummarizeToolcall(msgs);
            // 多工具调用后只有 c3 一组 → 不折叠
            XX_TEST_EXPECT_EQ(msgs.size(), size_t{7});
        }
    }

    // ==================== splitRecentByTokenBudget ====================

    {
        auto  env = std::make_shared<SummarizationTestEnv>();
        auto& h   = env->handle;

        // --- A. 预算充足 → 全部保留 (recent 起点 = system 之后) ---
        {
            std::vector<neograph::ChatMessage> msgs{
                makeMsg("system", "sys"),
                makeMsg("user", "u1"),
                makeMsg("assistant", "a1"),
            };
            auto end = h->splitRecentByTokenBudget(msgs, 1, 100000);
            XX_TEST_EXPECT_EQ(end, size_t{1});
        }

        // --- B. 预算有限 → 从后往前截断 (每条小消息 ~4-5 token) ---
        {
            std::vector<neograph::ChatMessage> msgs{
                makeMsg("system", "sys"),
                makeMsg("user", "u1"),
                makeMsg("assistant", "a1"),
                makeMsg("user", "u2"),
                makeMsg("assistant", "a2"),
                makeMsg("user", "u3"),
                makeMsg("assistant", "a3"),
            };
            // budget=10: 从后往前收 a3(5) u3(4) 后剩 1, a2(5) 超 → end=5
            auto end = h->splitRecentByTokenBudget(msgs, 1, 10);
            XX_TEST_EXPECT_EQ(end, size_t{5});
            // recent = [5,7) = u3, a3
            XX_TEST_EXPECT_EQ(msgs[end].content, std::string{"u3"});
        }

        // --- C. recent 开头为 tool → 回退到发起组 (整组纳入 recent) ---
        {
            std::vector<neograph::ChatMessage> msgs{
                makeMsg("system", "sys"),
                makeMsg("user", "u1"),
                makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
                makeToolResult("c1", "read_file", "rA"),
                makeAssistantToolcall("", {makeToolcall("c2", "read_file", R"({"path":"B"})")}),
                makeToolResult("c2", "read_file", "rB"),
            };
            // budget=6: 只收最后一条 rB(6)? rB: 3+1+0+0+2=6 → end=5; t? 再往前 tc2 超 → end=5
            // recent=[5,6)=rB 是 tool → 回退到 tc2 → end=4
            auto end = h->splitRecentByTokenBudget(msgs, 1, 6);
            XX_TEST_EXPECT_EQ(end, size_t{4});
            XX_TEST_EXPECT_EQ(msgs[end].role, std::string{"assistant"});
            XX_TEST_EXPECT_EQ(msgs[end].tool_calls[0].arguments, std::string{R"({"path":"B"})"});
        }

        // --- D. 压缩段末尾 assistant(tool_calls) 的 tool 结果在 recent → 整组划入 recent ---
        {
            std::vector<neograph::ChatMessage> msgs{
                makeMsg("system", "sys"),
                makeMsg("user", "u1"),
                makeMsg("assistant", "a1"),
                makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
                makeToolResult("c1", "read_file", "rA"),
                makeMsg("user", "u2"),
                makeMsg("assistant", "a2"),
            };
            // budget=15: 从后往前 a2(5)→end=6 b=10; u2(4)→end=5 b=6; rA(6)→end=4 b=0;
            // tc1(9) 超 0 → end=4 (recent=[4,7)=rA,u2,a2)
            // 对齐: recent 开头 rA 是 tool → 回退到 tc1 → end=3
            // 对齐 2: messages[2]=a1 无 tool_calls → 停
            auto end = h->splitRecentByTokenBudget(msgs, 1, 15);
            XX_TEST_EXPECT_EQ(end, size_t{3});
            XX_TEST_EXPECT_EQ(msgs[end].role, std::string{"assistant"});
            XX_TEST_EXPECT_EQ(msgs[end].tool_calls[0].name, std::string{"read_file"});
        }

        // --- E. 单条超预算 → recent 至少保留最后一条 ---
        {
            std::vector<neograph::ChatMessage> msgs{
                makeMsg("system", "sys"),
                makeMsg("user", "u1"),
                makeMsg("user", makeLongContent(2000)), // ~500 token
            };
            auto end = h->splitRecentByTokenBudget(msgs, 1, 10);
            // 最后一条超预算仍保留 (recent 至少 1 条)
            XX_TEST_EXPECT_EQ(end, size_t{2});
            XX_TEST_EXPECT_EQ(msgs[end].content.size(), size_t{2000});
        }
    }

    // ==================== hardTruncate ====================

    {
        auto  env = std::make_shared<SummarizationTestEnv>();
        auto& h   = env->handle;

        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
        };
        auto res = h->hardTruncate(msgs, 1, 1000);
        // system + 截断说明 + recent (budget=300 足够收全部)
        XX_TEST_EXPECT_EQ(res.size(), size_t{6});
        XX_TEST_EXPECT_EQ(res[0].role, std::string{"system"});
        XX_TEST_EXPECT_EQ(res[0].content, std::string{"sys"});
        XX_TEST_EXPECT_EQ(res[1].role, std::string{"user"});
        XX_TEST_EXPECT_TRUE(res[1].content.find("truncated") != std::string::npos);
        XX_TEST_EXPECT_TRUE(msgHasFlag(res[1], neograph::MessageFlag::AutoInserted));
        XX_TEST_EXPECT_TRUE(msgHasFlag(res[1], neograph::MessageFlag::Summarized));
        // recent 按原顺序
        XX_TEST_EXPECT_EQ(res[2].content, std::string{"u1"});
        XX_TEST_EXPECT_EQ(res[5].content, std::string{"a2"});
    }

    // ==================== onModelcallRunFunc ====================

    // --- T1. 空消息 → 直接返回, 不更新上下文统计 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, {});
        XX_TEST_EXPECT_TRUE(res.empty());
        XX_TEST_EXPECT_EQ(contextTokensOf(env->ctx, env->sessionId), size_t{0});
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->sessionId), size_t{0});
    }

    // --- T2. token 用量低于 65% → 不压缩, 仅发布统计 (count 路径) ---
    {
        auto                               env = std::make_shared<SummarizationTestEnv>();
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("user", "hi"),
            makeMsg("assistant", "yo"),
        };
        // count = user(3+1+0) + assistant(3+2+0) = 9
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs);
        XX_TEST_EXPECT_EQ(res.size(), size_t{2});
        XX_TEST_EXPECT_EQ(res[0].content, std::string{"hi"});
        XX_TEST_EXPECT_EQ(res[1].content, std::string{"yo"});
        // 默认模型 "fallback" 未指定 maxToken → 使用中间件默认值 2048
        XX_TEST_EXPECT_EQ(contextTokensOf(env->ctx, env->sessionId), size_t{9});
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->sessionId), size_t{2048});
    }

    // --- T3. apiTokenUsage 存在时优先使用 (而非本地统计) ---
    {
        auto                               env = std::make_shared<SummarizationTestEnv>();
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("user", "hi"),
            makeMsg("assistant", "yo"),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 100);
        XX_TEST_EXPECT_EQ(res.size(), size_t{2});
        XX_TEST_EXPECT_EQ(contextTokensOf(env->ctx, env->sessionId), size_t{100});
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->sessionId), size_t{2048});
    }

    // --- T4. < 85% 不压缩; >= 85% 时触发确定性去重 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small"); // max=1000, 阈值 850
        env->handle->summarizationToolHandles["read_file"] = makeReadFileHandle();
        auto                               longContent     = makeLongContent(3000);
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("user", "u1"),
            makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
            makeToolResult("c1", "read_file", "r1"),
            makeAssistantToolcall("", {makeToolcall("c2", "read_file", R"({"path":"A"})")}),
            makeToolResult("c2", "read_file", "r2"),
            makeMsg("user", longContent),
        };
        // 700 < 850: 不触发压缩
        auto resNo = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 700);
        XX_TEST_EXPECT_EQ(resNo.size(), size_t{6});
        XX_TEST_EXPECT_EQ(resNo[2].content, std::string{"r1"});
        XX_TEST_EXPECT_EQ(resNo[4].content, std::string{"r2"});

        // 900 >= 850: 触发压缩与去重
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        // 消息数量不变 (未做 LLM 总结)
        XX_TEST_EXPECT_EQ(res.size(), size_t{6});
        // toolcall 去重: 旧 response 截断, 新 response 保留
        XX_TEST_EXPECT_EQ(res[2].content, std::string{"[Truncated Response]"});
        XX_TEST_EXPECT_EQ(res[4].content, std::string{"r2"});
        // 长内容原样保留: 不 offload, 不标记
        XX_TEST_EXPECT_EQ(res[5].content, longContent);
        XX_TEST_EXPECT_FALSE(msgHasFlag(res[5], neograph::MessageFlag::ContentOffloaded));
        // share store 未被写入
        XX_TEST_EXPECT_TRUE(
            env->ctx->middlewareHandleContext->shareStore.find(env->sessionId)
            == env->ctx->middlewareHandleContext->shareStore.end()
        );
        // 没有出现总结消息
        bool hasSummary = false;
        for (const auto& m : res) {
            if (m.content.find("[Please compact context to save space]") != std::string::npos) {
                hasSummary = true;
            }
        }
        XX_TEST_EXPECT_FALSE(hasSummary);
        // 统计发布与 viewMessage
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->sessionId), size_t{1000});
        XX_TEST_EXPECT_TRUE(env->session()->viewMessages.size() >= 1);
        XX_TEST_EXPECT_TRUE(
            env->session()->viewMessages.back().text.find("压缩上下文") != std::string::npos
        );
    }

    // --- T5. >= 85% 且 LLM 总结成功: system + 总结对 + 最近消息 (token 预算切分) ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small"); // max=1000, budget=30
        env->subagent->summary = "S1";
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
            makeMsg("user", "u3"),
            makeMsg("assistant", "a3"),
            makeMsg("user", "u4"),
            makeMsg("assistant", "a4"),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        // system(1) + 总结对(2) + recent[u2,a2,u3,a3,u4,a4](6) = 9
        // (budget=30: 从后往前 a4(5)u4(4)a3(5)u3(4)a2(5)u2(4)=27, a1(5) 超 → end=3)
        XX_TEST_EXPECT_EQ(res.size(), size_t{9});
        // system 保留
        XX_TEST_EXPECT_EQ(res[0].role, std::string{"system"});
        XX_TEST_EXPECT_EQ(res[0].content, std::string{"sys"});
        // 总结 user 消息
        XX_TEST_EXPECT_EQ(res[1].role, std::string{"user"});
        XX_TEST_EXPECT_EQ(res[1].content, std::string{"[Please compact context to save space]"});
        XX_TEST_EXPECT_TRUE(msgHasFlag(res[1], neograph::MessageFlag::AutoInserted));
        XX_TEST_EXPECT_TRUE(msgHasFlag(res[1], neograph::MessageFlag::Summarized));
        // 总结 assistant 消息
        XX_TEST_EXPECT_EQ(res[2].role, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(res[2].content, std::string{"[Previous conversation summary]: \nS1"});
        XX_TEST_EXPECT_TRUE(msgHasFlag(res[2], neograph::MessageFlag::AutoInserted));
        XX_TEST_EXPECT_TRUE(msgHasFlag(res[2], neograph::MessageFlag::Summarized));
        // 最近消息按原顺序保留
        XX_TEST_EXPECT_EQ(res[3].content, std::string{"u2"});
        XX_TEST_EXPECT_EQ(res[4].content, std::string{"a2"});
        XX_TEST_EXPECT_EQ(res[5].content, std::string{"u3"});
        XX_TEST_EXPECT_EQ(res[6].content, std::string{"a3"});
        XX_TEST_EXPECT_EQ(res[7].content, std::string{"u4"});
        XX_TEST_EXPECT_EQ(res[8].content, std::string{"a4"});
        // 被压缩的旧消息已消失
        bool hasOld = false;
        for (const auto& m : res) {
            if (m.content == "u1" || m.content == "a1") {
                hasOld = true;
            }
        }
        XX_TEST_EXPECT_FALSE(hasOld);
        // 压缩请求: 同上下文 subagent (system + 压缩段 + 指令)
        XX_TEST_EXPECT_EQ(env->subagent->receivedArguments.size(), size_t{1});
        const auto& reqMsgs = env->subagent->receivedArguments[0]["messages"];
        XX_TEST_EXPECT_EQ(reqMsgs[0].value("role", std::string{}), std::string{"system"});
        XX_TEST_EXPECT_EQ(reqMsgs[0].value("content", std::string{}), std::string{"sys"});
        XX_TEST_EXPECT_EQ(reqMsgs[1].value("content", std::string{}), std::string{"u1"});
        XX_TEST_EXPECT_EQ(reqMsgs[2].value("content", std::string{}), std::string{"a1"});
        XX_TEST_EXPECT_EQ(reqMsgs.size(), size_t{4}); // sys + u1 + a1 + 指令
        XX_TEST_EXPECT_EQ(contextTokensOf(env->ctx, env->sessionId), size_t{58});
        XX_TEST_EXPECT_TRUE(env->session()->viewMessages.size() >= 1);
        XX_TEST_EXPECT_TRUE(
            env->session()->viewMessages.back().text.find("压缩上下文") != std::string::npos
        );
    }

    // --- T6. >= 85% 但 LLM 总结失败 (空响应) → 保留原消息, 失败计数 +1 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small");
        // responses 为空 → 压缩失败
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
            makeMsg("user", "u3"),
            makeMsg("assistant", "a3"),
            makeMsg("user", "u4"),
            makeMsg("assistant", "a4"),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        XX_TEST_EXPECT_EQ(res.size(), size_t{9});
        for (size_t i = 0; i < msgs.size(); ++i) {
            XX_TEST_EXPECT_EQ(res[i].role, msgs[i].role);
            XX_TEST_EXPECT_EQ(res[i].content, msgs[i].content);
        }
        // 失败计数 = 1 (未触发硬截断)
        auto failCount = env->ctx->middlewareHandleContext->getGraphDataItemValue<size_t>(
            env->sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_summarizationFailCount
        );
        XX_TEST_EXPECT_EQ(failCount, size_t{1});
    }

    // --- T7. 连续第 2 次失败 → 硬截断兜底 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small");
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
            makeMsg("user", "u3"),
            makeMsg("assistant", "a3"),
            makeMsg("user", "u4"),
            makeMsg("assistant", "a4"),
        };
        // 第一次失败
        auto res1 = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        XX_TEST_EXPECT_EQ(res1.size(), size_t{9});
        // 第二次失败 → 硬截断: system + 截断说明 + recent (budget=300 收全部 8 条)
        auto res2 = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        XX_TEST_EXPECT_EQ(res2.size(), size_t{10});
        XX_TEST_EXPECT_EQ(res2[0].role, std::string{"system"});
        XX_TEST_EXPECT_EQ(res2[0].content, std::string{"sys"});
        XX_TEST_EXPECT_EQ(res2[1].role, std::string{"user"});
        XX_TEST_EXPECT_TRUE(res2[1].content.find("truncated") != std::string::npos);
        XX_TEST_EXPECT_TRUE(msgHasFlag(res2[1], neograph::MessageFlag::AutoInserted));
        XX_TEST_EXPECT_TRUE(msgHasFlag(res2[1], neograph::MessageFlag::Summarized));
        // recent 按原顺序 (budget 充足收全部)
        XX_TEST_EXPECT_EQ(res2[2].content, std::string{"u1"});
        XX_TEST_EXPECT_EQ(res2[9].content, std::string{"a4"});
    }

    // --- T8. 切割点落在 tool 消息上 → 回退到发起 tool 的 assistant, 整组保留 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small");
        env->subagent->summary = "S";
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("user", "u1"),
            makeAssistantToolcall("tc1", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
            makeToolResult("c1", "read_file", "t1"),
            makeAssistantToolcall("tc2", {makeToolcall("c2", "read_file", R"({"path":"B"})")}),
            makeToolResult("c2", "read_file", "t2"),
            makeToolResult("c3", "read_file", "t3"),
            makeToolResult("c4", "read_file", "t4"),
            makeToolResult("c5", "read_file", "t5"),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        // 预算 30 (token: u1=4, tc1=10, t1=4, tc2=10, t2..t5=4):
        // 从后往前收 t5,t4,t3,t2,tc2,t1 (4+4+4+4+10+4=30) → end=2
        // recent 开头 t1 是 tool → 回退到发起组 tc1 → end=1
        // 总结对(2) + recent[tc1, t1, tc2, t2..t5](7) = 9
        XX_TEST_EXPECT_EQ(res.size(), size_t{9});
        XX_TEST_EXPECT_EQ(res[0].role, std::string{"user"});
        XX_TEST_EXPECT_EQ(res[0].content, std::string{"[Please compact context to save space]"});
        XX_TEST_EXPECT_EQ(res[1].role, std::string{"assistant"});
        // recent 以发起 tool 的 assistant 开头, 整组 tool 交换完整保留
        XX_TEST_EXPECT_EQ(res[2].role, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(res[2].content, std::string{"tc1"});
        XX_TEST_EXPECT_EQ(res[3].role, std::string{"tool"});
        XX_TEST_EXPECT_EQ(res[3].content, std::string{"t1"});
        XX_TEST_EXPECT_EQ(res[4].role, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(res[4].content, std::string{"tc2"});
        XX_TEST_EXPECT_EQ(res[5].role, std::string{"tool"});
        XX_TEST_EXPECT_EQ(res[5].content, std::string{"t2"});
        XX_TEST_EXPECT_EQ(res[6].content, std::string{"t3"});
        XX_TEST_EXPECT_EQ(res[7].content, std::string{"t4"});
        XX_TEST_EXPECT_EQ(res[8].content, std::string{"t5"});
    }

    // --- T9. 模型配置 maxToken 生效: 会话切换模型 → 阈值随之变化 ---
    {
        auto                               env = std::make_shared<SummarizationTestEnv>();
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
            makeMsg("user", "u3"),
            makeMsg("assistant", "a3"),
            makeMsg("user", "u4"),
            makeMsg("assistant", "a4"),
        };
        // 默认模型 "fallback" (max=0) → 中间件默认 2048; 900 < 2048*0.65 → 不压缩
        auto res1 = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        XX_TEST_EXPECT_EQ(res1.size(), size_t{8});
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->sessionId), size_t{2048});

        // 切换 small (max=1000): 900 >= 850 → LLM 总结
        env->session()->setModelName("small");
        env->subagent->summary = "S";
        auto res2 = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        // budget=30: recent=[u2,a2,u3,a3,u4,a4](6) → 总结对 + 6 = 8
        XX_TEST_EXPECT_EQ(res2.size(), size_t{8});
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->sessionId), size_t{1000});

        // 切换 big (max=5000): 900 < 5000*0.65 → 不压缩
        env->session()->setModelName("big");
        auto res3 = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        XX_TEST_EXPECT_EQ(res3.size(), size_t{8});
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->sessionId), size_t{5000});
    }

    // --- T10. sendThinking=true 时统计包含 reasoning_content ---
    {
        auto                               env = std::make_shared<SummarizationTestEnv>();
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("user", "hi"),
            [&]() {
                auto m              = makeMsg("assistant", "yo");
                m.reasoning_content = "think";
                return m;
            }(),
        };
        // thinking 模型 (sendThinking=true): user(3+1+0) + assistant(3+2+0+1) = 10
        env->session()->setModelName("thinking");
        auto res1 = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs);
        XX_TEST_EXPECT_EQ(contextTokensOf(env->ctx, env->sessionId), size_t{10});
        // 普通模型 (sendThinking=false): 9
        env->session()->setModelName("small");
        auto res2 = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs);
        XX_TEST_EXPECT_EQ(contextTokensOf(env->ctx, env->sessionId), size_t{9});
        // 消息均未被压缩
        XX_TEST_EXPECT_EQ(res1.size(), size_t{2});
        XX_TEST_EXPECT_EQ(res2.size(), size_t{2});
    }

    // ==================== 新增: 用户要求的完整语义验证 ====================
    // 预期效果:
    //   1. system 消息不能动 (不参与 tool 压缩 / 噪音清理 / LLM 总结)
    //   2. 按顺序先进行确定性压缩 (去重/折叠/噪音清理), 再由同上下文 LLM 将
    //      整体上下文压缩成一段总结
    //   3. 消息角色顺序正确: 压缩后 = system | user(自动插入提示) |
    //      assistant(压缩总结), 紧接着是未压缩的最近消息 (保留原角色与顺序)
    //   4. thinking 保留 (不剥离, 由 LLM 决定取舍)

    // --- T11. 完整链路: 65% 确定性压缩先行, 85% 同上下文压缩成一段总结;
    //            system 不能动; 角色顺序 = system | user(自动提示) | assistant(总结) | 最近消息;
    //            thinking 保留并传给压缩请求 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small"); // max=1000 → 阈值 650 / 850
        env->handle->summarizationToolHandles["read_file"] = makeReadFileHandle();
        env->subagent->summary                             = "S1";

        auto                               longContent = makeLongContent(3000);
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "u1"),
            makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
            makeToolResult("c1", "read_file", "r1"),
            makeMsg("user", longContent), // 长内容: 程序侧不 offload, 原样进入压缩请求
            makeAssistantToolcall("", {makeToolcall("c2", "read_file", R"({"path":"A"})")}),
            makeToolResult("c2", "read_file", "r2"),
            [&]() {
                auto m              = makeMsg("assistant", "a3");
                m.reasoning_content = "think-3"; // thinking 保留
                return m;
            }(),
            makeMsg("user", "u4"),
            makeMsg("assistant", "a4"),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);

        // ① system 消息不能动: 角色 / 内容 / flags 原样保留
        XX_TEST_EXPECT_EQ(res[0].role, std::string{"system"});
        XX_TEST_EXPECT_EQ(res[0].content, std::string{"sys"});
        XX_TEST_EXPECT_EQ(res[0].flags, neograph::MessageFlag::None);

        // ② 压缩成功 → (system | user 自动提示 | assistant 总结) + 未压缩的最近消息
        //    budget=30: 从后往前 a4(5)u4(4)a3(7: 含 think-3→3+2+0+1=6? 见下)...
        //    实际: a4=5, u4=4, a3(带 think, countThinking=false 不计 thinking)=5,
        //    u3? 无 u3; 消息序列: sys,u1,tc1,t1,longUser,tc2,t2,a3(think),u4,a4
        //    a4(5)→b=25; u4(4)→b=21; a3(5)→b=16; t2(6)→b=10; tc2(9)→b=1;
        //    longUser(750)→超 → end=6? 等等 longUser 是 messages[4]... 重新编号:
        //    0=sys 1=u1 2=tc1 3=t1 4=longUser 5=tc2 6=t2 7=a3 8=u4 9=a4
        //    a4(9)=5→end=9 b=25; u4(8)=4→end=8 b=21; a3(7)=5→end=7 b=16;
        //    t2(6)=6→end=6 b=10; tc2(5)=9→end=5 b=1; longUser(4) 超 1 → break. end=5
        //    对齐1: messages[5]=tc2 非 tool. 对齐2: messages[4]=longUser 非 assistant.
        //    oldSeg=[1,5)=u1,tc1,t1,longUser; recent=[5,10)=tc2,t2,a3,u4,a4 (5 条)
        XX_TEST_EXPECT_EQ(res.size(), size_t{8}); // sys + 总结对 + 5
        XX_TEST_EXPECT_EQ(res[1].role, std::string{"user"});
        XX_TEST_EXPECT_EQ(res[1].content, std::string{"[Please compact context to save space]"});
        XX_TEST_EXPECT_TRUE(msgHasFlag(res[1], neograph::MessageFlag::AutoInserted));
        XX_TEST_EXPECT_TRUE(msgHasFlag(res[1], neograph::MessageFlag::Summarized));
        XX_TEST_EXPECT_EQ(res[2].role, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(res[2].content, std::string{"[Previous conversation summary]: \nS1"});
        XX_TEST_EXPECT_TRUE(msgHasFlag(res[2], neograph::MessageFlag::AutoInserted));
        XX_TEST_EXPECT_TRUE(msgHasFlag(res[2], neograph::MessageFlag::Summarized));

        // ③ 未压缩的最近消息: 角色与顺序原样保留
        XX_TEST_EXPECT_EQ(res[3].role, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(res[3].tool_calls[0].arguments, std::string{R"({"path":"A"})"});
        XX_TEST_EXPECT_EQ(res[4].role, std::string{"tool"});
        XX_TEST_EXPECT_EQ(res[4].content, std::string{"r2"});
        XX_TEST_EXPECT_EQ(res[5].role, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(res[5].content, std::string{"a3"});
        XX_TEST_EXPECT_EQ(res[6].content, std::string{"u4"});
        XX_TEST_EXPECT_EQ(res[7].content, std::string{"a4"});

        // ④ 确定性压缩先于 LLM 压缩: 传给 subagent 的是去重后的旧消息
        XX_TEST_EXPECT_EQ(env->subagent->receivedArguments.size(), size_t{1});
        const auto& reqMsgs = env->subagent->receivedArguments[0]["messages"];
        // system + u1 + tc1 + [Truncated Response] + longUser + 指令
        XX_TEST_EXPECT_EQ(reqMsgs.size(), size_t{6});
        XX_TEST_EXPECT_EQ(reqMsgs[0].value("content", std::string{}), std::string{"sys"});
        XX_TEST_EXPECT_EQ(reqMsgs[1].value("content", std::string{}), std::string{"u1"});
        // 旧 tool 结果已被去重截断 (先于 LLM 压缩执行)
        XX_TEST_EXPECT_EQ(
            reqMsgs[3].value("content", std::string{}),
            std::string{"[Truncated Response]"}
        );
        // 长内容原样保留在压缩请求中 (不 offload, 由模型自主决定是否外置)
        XX_TEST_EXPECT_EQ(reqMsgs[4].value("content", std::string{}), longContent);

        // ⑤ thinking 保留: 压缩请求中含 thinking 的 assistant 消息不被剥离
        //    (本测试中 a3 在 recent 段; 另验证 cleanNoiseMessages 不剥离 thinking)
        XX_TEST_EXPECT_EQ(res[5].reasoning_content, std::string{"think-3"});
    }

    // --- T12. 无 system 时: 压缩后 = user(自动提示) | assistant(总结) | 最近消息 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small");
        env->subagent->summary = "S";
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
            makeMsg("user", "u3"),
            makeMsg("assistant", "a3"),
            makeMsg("user", "u4"),
            makeMsg("assistant", "a4"),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        // 总结对 + recent[u2,a2,u3,a3,u4,a4](6) = 8
        // (budget=30: a4(5)u4(4)a3(5)u3(4)a2(5)u2(4)=27, a1(5) 超 → end=2.
        //  recent=[2,8)=u2,a2,u3,a3,u4,a4)
        XX_TEST_EXPECT_EQ(res.size(), size_t{8});
        XX_TEST_EXPECT_EQ(res[0].role, std::string{"user"});
        XX_TEST_EXPECT_EQ(res[0].content, std::string{"[Please compact context to save space]"});
        XX_TEST_EXPECT_TRUE(msgHasFlag(res[0], neograph::MessageFlag::AutoInserted));
        XX_TEST_EXPECT_TRUE(msgHasFlag(res[0], neograph::MessageFlag::Summarized));
        XX_TEST_EXPECT_EQ(res[1].role, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(res[1].content, std::string{"[Previous conversation summary]: \nS"});
        // 最近消息保留原顺序
        XX_TEST_EXPECT_EQ(res[2].content, std::string{"u2"});
        XX_TEST_EXPECT_EQ(res[3].content, std::string{"a2"});
        XX_TEST_EXPECT_EQ(res[4].content, std::string{"u3"});
        XX_TEST_EXPECT_EQ(res[5].content, std::string{"a3"});
        XX_TEST_EXPECT_EQ(res[6].content, std::string{"u4"});
        XX_TEST_EXPECT_EQ(res[7].content, std::string{"a4"});
    }

    // --- T13. 整体上下文压缩为一段总结: 仅一条 assistant 总结消息, 仅 2 条
    //           Summarized 标记 (user 提示 + assistant 总结), 压缩调用只执行一次 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small");
        env->subagent->summary = "Key decision: X\nFile: /a/b/c.cpp\nAction: Y";
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
            makeMsg("user", "u3"),
            makeMsg("assistant", "a3"),
            makeMsg("user", "u4"),
            makeMsg("assistant", "a4"),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        XX_TEST_EXPECT_EQ(res.size(), size_t{8});
        size_t      summaryCount    = 0;
        size_t      summarizedFlags = 0;
        std::string summaryContent;
        for (const auto& m : res) {
            if (msgHasFlag(m, neograph::MessageFlag::Summarized)) {
                ++summarizedFlags;
            }
            if (m.role == "assistant"
                && m.content.rfind("[Previous conversation summary]:", 0) == 0) {
                ++summaryCount;
                summaryContent = m.content;
            }
        }
        XX_TEST_EXPECT_EQ(summaryCount, size_t{1});
        XX_TEST_EXPECT_EQ(summarizedFlags, size_t{2});
        // 多行总结整体保留在一条消息内 (一段总结)
        XX_TEST_EXPECT_EQ(
            summaryContent,
            std::string{
                "[Previous conversation summary]: \nKey decision: X\nFile: /a/b/c.cpp\nAction: Y"
            }
        );
        XX_TEST_EXPECT_EQ(env->subagent->receivedArguments.size(), size_t{1});
    }

    // --- T14. 超长 system 原样保留: 不参与压缩/清理/总结 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small");
        env->subagent->summary                        = "S";
        auto                               longSystem = makeLongContent(3000);
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", longSystem),
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
            makeMsg("user", "u3"),
            makeMsg("assistant", "a3"),
            makeMsg("user", "u4"),
            makeMsg("assistant", "a4"),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        // system 原样保留: 不参与噪音清理, 不参与压缩
        XX_TEST_EXPECT_EQ(res[0].role, std::string{"system"});
        XX_TEST_EXPECT_EQ(res[0].content, longSystem);
        XX_TEST_EXPECT_FALSE(msgHasFlag(res[0], neograph::MessageFlag::ContentOffloaded));
        // 角色顺序: system | user(自动提示) | assistant(总结) | 最近消息
        XX_TEST_EXPECT_EQ(res.size(), size_t{9});
        XX_TEST_EXPECT_EQ(res[1].role, std::string{"user"});
        XX_TEST_EXPECT_EQ(res[1].content, std::string{"[Please compact context to save space]"});
        XX_TEST_EXPECT_EQ(res[2].role, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(res[2].content, std::string{"[Previous conversation summary]: \nS"});
        XX_TEST_EXPECT_EQ(res[3].content, std::string{"u2"});
        XX_TEST_EXPECT_EQ(res[8].content, std::string{"a4"});
    }

    // --- T15. 最近消息含 tool 交换: system 保留, recent 整组 (assistant(tool_calls) + tool)
    //           完整保留, 角色顺序正确 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small");
        env->subagent->summary = "S";
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
            makeMsg("user", "u3"),
            makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
            makeToolResult("c1", "read_file", "t1"),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        // budget=30: t1(6)→b=24; tc1(9)→b=15; u3(4)→b=11; a2(5)→b=6; u2(4)→b=2;
        // a1(5) 超 → end=4? 循环: end=8→t1(7): 6<=30→end=7 b=24; tc1(6): 9<=24→end=6 b=15;
        // u3(5): 4<=15→end=5 b=11; a2(4): 5<=11→end=4 b=6; u2(3): 4<=6→end=3 b=2;
        // a1(2): 5>2 → break. end=3
        // 对齐1: messages[3]=u2 非 tool. 对齐2: messages[2]=a1 无 tool_calls.
        // system(1) + 总结对(2) + recent 5 条 (u2, a2, u3, tc1, t1) = 8
        XX_TEST_EXPECT_EQ(res.size(), size_t{8});
        XX_TEST_EXPECT_EQ(res[0].role, std::string{"system"});
        XX_TEST_EXPECT_EQ(res[0].content, std::string{"sys"});
        XX_TEST_EXPECT_EQ(res[1].role, std::string{"user"});
        XX_TEST_EXPECT_EQ(res[1].content, std::string{"[Please compact context to save space]"});
        XX_TEST_EXPECT_EQ(res[2].role, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(res[2].content, std::string{"[Previous conversation summary]: \nS"});
        // recent 保留原顺序与角色
        XX_TEST_EXPECT_EQ(res[3].role, std::string{"user"});
        XX_TEST_EXPECT_EQ(res[3].content, std::string{"u2"});
        XX_TEST_EXPECT_EQ(res[4].role, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(res[4].content, std::string{"a2"});
        XX_TEST_EXPECT_EQ(res[5].role, std::string{"user"});
        XX_TEST_EXPECT_EQ(res[5].content, std::string{"u3"});
        XX_TEST_EXPECT_EQ(res[6].role, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(res[6].tool_calls.size(), size_t{1});
        XX_TEST_EXPECT_EQ(res[7].role, std::string{"tool"});
        XX_TEST_EXPECT_EQ(res[7].content, std::string{"t1"});
        // 未压缩的最近消息不再包含旧段内容
        bool hasOld = false;
        for (const auto& m : res) {
            if (m.content == "u1" || m.content == "a1") {
                hasOld = true;
            }
        }
        XX_TEST_EXPECT_FALSE(hasOld);
    }

    // --- T16. 探索折叠在 onModelcallRunFunc 链路生效 (>= 85% 分支) ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small"); // max=1000
        env->handle->summarizationToolHandles["read_file"] = makeReadFileHandle();
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "u1"),
            makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
            makeToolResult("c1", "read_file", "rA"),
            makeAssistantToolcall("", {makeToolcall("c2", "read_file", R"({"path":"B"})")}),
            makeToolResult("c2", "read_file", "rB"),
            makeAssistantToolcall("", {makeToolcall("c3", "read_file", R"({"path":"C"})")}),
            makeToolResult("c3", "read_file", "rC"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        // 探索折叠: 3 组连续 read_file → 只保留最后一组 (C)
        XX_TEST_EXPECT_EQ(res.size(), size_t{6}); // sys,u1,tcC,tC,u2,a2
        XX_TEST_EXPECT_EQ(res[0].content, std::string{"sys"});
        XX_TEST_EXPECT_EQ(res[1].content, std::string{"u1"});
        XX_TEST_EXPECT_EQ(res[2].tool_calls[0].arguments, std::string{R"({"path":"C"})"});
        XX_TEST_EXPECT_EQ(res[3].content, std::string{"rC"});
        XX_TEST_EXPECT_EQ(res[4].content, std::string{"u2"});
        XX_TEST_EXPECT_EQ(res[5].content, std::string{"a2"});
    }

    // ==================== 错误兼容: 真实工具集成与异常路径 ====================

    // --- T17. 集成: 真实 SubAgentManagerTool (非 Fake) 的中断往返 ---
    // - doSummarizeWithLLM → 真实工具 execute_async → NodeInterrupt 放行传播
    //   到调用方 (由 Session 派生 subagent); 中断参数完整 (subagent_task /
    //   父线程 sessionId / 仅 share_store 工具 / 禁二次压缩)
    // - 预置 interruptResult 后再次调用 → 返回摘要文本
    {
        auto ctx                     = std::make_shared<agentxx::agent::AgentContext>();
        static asio::io_context s_ioCtx;
        ctx->bus                     = std::make_shared<agentxx::event::EventBus>(s_ioCtx.get_executor());
        ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
        ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
        ctx->modelRegistry           = std::make_shared<agentxx::agent::ModelProviderRegistry>();
        agentxx::agent::ModelConfig mcfg;
        mcfg.name                  = "m";
        mcfg.modelName             = "m-model";
        mcfg.modelContenxtMaxToken = 1000;
        ctx->modelRegistry->registerModel("m", mcfg);
        ctx->agentConfig->model.modelName = "m";

        // 真实 SubAgentManagerTool + 注册 CodeAgent 默认的 subagent_task
        auto realTool
            = std::make_shared<agentxx::tools::SubAgentManagerTool>("subagent_manager", ctx);
        realTool->registerOnBus(ctx->bus);
        realTool->subAgentList.insert(std::make_pair(
            "subagent_task",
            std::make_shared<agentxx::tools::SubAgentNormalTask>("subagent_task", "isolation")
        ));

        const std::string sid = "sum_real_tool_thread";
        ctx->sessions->getOrCreate(sid);

        auto handle = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(ctx);

        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
        };

        // ① 首次调用: NodeInterrupt 放行 (不捕获), 中断参数写入 graphData
        bool threwInterrupt = false;
        try {
            (void)co_await handle->doSummarizeWithLLM(sid, msgs);
        } catch (const neograph::graph::NodeInterrupt&) {
            threwInterrupt = true;
        }
        XX_TEST_EXPECT_TRUE(threwInterrupt);

        const auto& stored
            = ctx->middlewareHandleContext
                  ->getGraphDataItemValue<std::vector<agentxx::middleware::InterruptHandleArg>>(
                      sid,
                      agentxx::middleware::MiddlewareContext::graphDataKey_interruptArgs
                  );
        XX_TEST_EXPECT_EQ(stored.size(), size_t{1});
        XX_TEST_EXPECT_EQ(stored[0].name, std::string{"subagent"});
        // 压缩直接调用无 tool_call_id: resultId 为空 (读取端按序号兜底)
        XX_TEST_EXPECT_EQ(stored[0].resultId, std::string{""});
        const auto& tasksJson = stored[0].arg["tasks"];
        XX_TEST_EXPECT_TRUE(tasksJson.is_array());
        XX_TEST_EXPECT_EQ(tasksJson.size(), size_t{1});
        XX_TEST_EXPECT_EQ(
            tasksJson[0].value("subagent", std::string{}),
            std::string{"subagent_task"}
        );
        XX_TEST_EXPECT_EQ(tasksJson[0].value("sessionId", std::string{}), sid);
        XX_TEST_EXPECT_TRUE(tasksJson[0]["enable_summarization"].is_boolean());
        XX_TEST_EXPECT_FALSE(tasksJson[0]["enable_summarization"].get<bool>());
        XX_TEST_EXPECT_TRUE(tasksJson[0]["tools"].is_array());
        XX_TEST_EXPECT_EQ(tasksJson[0]["tools"].size(), size_t{1});
        XX_TEST_EXPECT_EQ(
            tasksJson[0]["tools"][0].get<std::string>(),
            std::string{"agentxx_share_store"}
        );
        // 结构化透传: system + 压缩段 + 末尾压缩指令
        const auto& reqMsgs = tasksJson[0]["messages"];
        XX_TEST_EXPECT_TRUE(reqMsgs.is_array());
        XX_TEST_EXPECT_EQ(reqMsgs.size(), msgs.size() + 1);
        XX_TEST_EXPECT_EQ(reqMsgs[0].value("role", std::string{}), std::string{"system"});
        XX_TEST_EXPECT_EQ(reqMsgs.back().value("role", std::string{}), std::string{"user"});

        // ② resume: 按 key 规则回填结果 ("1" = 无 resultId 时按序号兜底),
        //    再次调用返回摘要文本, 不再抛中断
        ctx->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
            sid,
            agentxx::middleware::MiddlewareContext::graphDataKey_interruptResult,
            neograph::json{
                {"1", "real-tool summary"}
        }
        );
        auto r = co_await handle->doSummarizeWithLLM(sid, msgs);
        XX_TEST_EXPECT_EQ(r, std::string{"real-tool summary"});
    }

    // --- T18. subagent 执行链路异常 → catchErrorAsync 捕获降级为空串,
    //           走失败计数路径 (保留原消息), 不向上传播崩溃 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small");
        env->subagent->throwException = true;

        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
            makeMsg("user", "u3"),
            makeMsg("assistant", "a3"),
            makeMsg("user", "u4"),
            makeMsg("assistant", "a4"),
        };
        auto r = co_await env->handle->doSummarizeWithLLM(env->sessionId, msgs);
        XX_TEST_EXPECT_EQ(r, std::string{""});

        // onModelcallRunFunc: 失败计数 +1, 未达阈值时保留原消息
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        XX_TEST_EXPECT_EQ(res.size(), size_t{9});
        for (size_t i = 0; i < msgs.size(); ++i) {
            XX_TEST_EXPECT_EQ(res[i].role, msgs[i].role);
            XX_TEST_EXPECT_EQ(res[i].content, msgs[i].content);
        }
        auto failCount = env->ctx->middlewareHandleContext->getGraphDataItemValue<size_t>(
            env->sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_summarizationFailCount
        );
        XX_TEST_EXPECT_EQ(failCount, size_t{1});
    }

    // --- T19. AgentContext 失效 (weak_ptr 过期/空) → 返回空串, 不崩溃 ---
    {
        auto handle = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(
            std::weak_ptr<agentxx::agent::AgentContext>{}
        );
        std::vector<neograph::ChatMessage> msgs{makeMsg("user", "hi")};
        auto r = co_await handle->doSummarizeWithLLM("any-thread", msgs);
        XX_TEST_EXPECT_EQ(r, std::string{""});
    }

    // --- T20. subagent 失败返回错误 JSON (非空串): 当前实现将其作为
    //           "摘要文本" 写回上下文 (行为记录: 错误信息透传给父 LLM,
    //           由其自行识别处理; 后续如改为失败判定需同步更新本用例) ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small");
        env->subagent->failWithError = true;
        env->subagent->summary       = "";

        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
            makeMsg("user", "u3"),
            makeMsg("assistant", "a3"),
            makeMsg("user", "u4"),
            makeMsg("assistant", "a4"),
        };
        auto r = co_await env->handle->doSummarizeWithLLM(env->sessionId, msgs);
        // 错误 JSON 非空 → doSummarizeWithLLM 原样返回 (未做失败判定)
        XX_TEST_EXPECT_EQ(r, std::string{R"({"error":"fake subagent failed"})"});

        // onModelcallRunFunc 视其为成功压缩 → Compact 写回, 摘要内容含错误 JSON
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        // 与 T5 同布局: system(1) + 总结对(2) + recent 6 条 = 9
        XX_TEST_EXPECT_EQ(res.size(), size_t{9});
        bool hasErrorSummary = false;
        for (const auto& m : res) {
            if (m.content.find(R"({"error":"fake subagent failed"})") != std::string::npos) {
                hasErrorSummary = true;
            }
        }
        XX_TEST_EXPECT_TRUE(hasErrorSummary);
    }

    // --- T20. 触发压缩时先发 viewMessage "正在压缩上下文", 压缩完成更新为 "压缩上下文 {old}->{new}/{max} · {耗时}" ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small"); // max=1000
        env->subagent->summary = "S_ViewMsg";

        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
            makeMsg("user", "u3"),
            makeMsg("assistant", "a3"),
            makeMsg("user", "u4"),
            makeMsg("assistant", "a4"),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->sessionId, msgs, 900);
        XX_TEST_EXPECT_EQ(res.size(), size_t{9});

        auto sess = env->session();
        XX_TEST_EXPECT_TRUE(sess->viewMessages.size() >= 1);
        const auto& vm = sess->viewMessages.back();
        XX_TEST_EXPECT_EQ(vm.role, agentxx::agent::ViewMessage::Role::Tip);
        // 验证格式: 包含 "压缩上下文 900->.../1000 · "
        XX_TEST_EXPECT_TRUE(vm.text.starts_with("压缩上下文 900->"));
        XX_TEST_EXPECT_TRUE(vm.text.find("/1000 · ") != std::string::npos);
    }

    // --- T21. 手动触发 compactSessionContext 压缩 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small");
        env->subagent->summary = "S_Manual";

        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
        };
        neograph::json msgsJson;
        neograph::to_json(msgsJson, msgs);
        env->session()->llmMessages = msgsJson;

        bool ok = co_await env->handle->compactSessionContext(env->sessionId);
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_TRUE(env->session()->llmMessages.is_array());
        XX_TEST_EXPECT_TRUE(env->session()->viewMessages.size() >= 1);
        const auto& vm = env->session()->viewMessages.back();
        XX_TEST_EXPECT_TRUE(vm.text.starts_with("压缩上下文 "));
    }

    co_return TestResult{g_sum_passed, g_sum_failed};
}

} // namespace test
} // namespace agentxx
