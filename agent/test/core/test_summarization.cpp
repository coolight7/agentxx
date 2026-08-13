// SummarizationMiddlewareHandle (上下文压缩中间件) 单元测试
//
// 设计要点 (见 agent/lib/include/agentxx/middlewares/summarization.h):
// - system prompt、最近的消息 不压缩
// - 可压缩的长消息内容用 agentxx_share_store 暂存, 留下 id + depict
// - 选择多条消息总结压缩合并为一条
// - 两级触发: >= 65% 上限 时去重 toolcall + 暂存长内容; >= 85% 上限 时 LLM 总结压缩
// - 每次 modelcall 前发布上下文统计 (contextTokens / maxContextTokens) 供 UI 显示

#include "test_summarization.h"

#include "agentxx/agent/model_registry.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/tools/sub_agent.h"
#include "fmt/format.h"
#include "neograph/graph/node.h"
#include "neograph/graph/run_context.h"
#include "neograph/graph/state.h"
#include "neograph/types.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agentxx {
namespace test {

int g_sum_passed = 0;
int g_sum_failed = 0;

namespace {

// ---------------------------------------------------------------------------
// 测试辅助: 伪造 subagent 管理器 (返回预设的总结文本, 不经过真实 LLM)
// ---------------------------------------------------------------------------

class FakeSubAgentManagerTool : public agentxx::tools::SubAgentManagerTool {
public:

    std::string summary;

    FakeSubAgentManagerTool(
        std::string_view                            in_nodeName,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    ) :
        SubAgentManagerTool(in_nodeName, std::move(in_agentContext)) {}

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override {
        co_return summary;
    }
};

// ---------------------------------------------------------------------------
// 测试环境: AgentContext + modelRegistry + 中间件实例 + 预创建会话
// ---------------------------------------------------------------------------

struct SummarizationTestEnv {
    std::shared_ptr<agentxx::agent::AgentContext>                       ctx      = nullptr;
    std::shared_ptr<FakeSubAgentManagerTool>                            subagent = nullptr;
    std::shared_ptr<agentxx::middleware::SummarizationMiddlewareHandle> handle   = nullptr;
    std::string threadId = "sum_test_thread";

    /// @param in_defaultMaxToken 中间件默认模型上限 (模型配置未指定时使用)
    SummarizationTestEnv(
        size_t in_defaultMaxToken       = 2048,
        double in_asciiCharsPerToken    = 4.0,
        double in_unicodeCharsPerToken  = 1.1,
        double in_tokensPerImage        = 400.0,
        double in_extraTokensPerMessage = 3.0
    ) {
        ctx                          = std::make_shared<agentxx::agent::AgentContext>();
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

        subagent = std::make_shared<FakeSubAgentManagerTool>("fake_subagent", ctx);
        ctx->subagentManagerToolPtr = subagent.get();

        handle = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(
            subagent.get(),
            ctx,
            in_defaultMaxToken,
            in_asciiCharsPerToken,
            in_unicodeCharsPerToken,
            in_tokensPerImage,
            in_extraTokensPerMessage
        );

        // 预创建会话, 用于校验上下文统计发布
        ctx->sessions->getOrCreate(threadId);
    }

    /// 当前会话使用的模型配置 (便于测试中切换模型)
    std::shared_ptr<agentxx::agent::Session> session() const {
        return ctx->sessions->get(threadId);
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

/// 长消息: 单条内容超过 longContentByteThreshold (2000) 字节
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
    std::string_view                                     threadId
) {
    auto session = ctx->sessions->get(threadId);
    return (nullptr != session && nullptr != session->contextStats)
               ? session->contextStats->contextTokens.load()
               : 0;
}

static size_t maxContextTokensOf(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    std::string_view                                     threadId
) {
    auto session = ctx->sessions->get(threadId);
    return (nullptr != session && nullptr != session->contextStats)
               ? session->contextStats->maxContextTokens.load()
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
    std::string_view                                                           threadId,
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
            threadId,
            agentxx::middleware::MiddlewareContext::graphDataKey_LLMTokenUsage,
            neograph::json(*apiTokenUsage)
        );
    } else {
        ctx->middlewareHandleContext->removeGraphDataItem(
            threadId,
            agentxx::middleware::MiddlewareContext::graphDataKey_LLMTokenUsage
        );
    }

    neograph::graph::RunContext runCtx;
    runCtx.thread_id = std::string{threadId};
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

    // ==================== doSummarizeWithLLM ====================

    {
        // subagentManager 为空 → 返回空串 (降级为不压缩)
        auto env        = std::make_shared<SummarizationTestEnv>();
        auto noSubagent = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(
            nullptr,
            env->ctx,
            2048
        );
        std::vector<neograph::ChatMessage> msgs{makeMsg("user", "hi")};
        auto                               r1 = co_await noSubagent->doSummarizeWithLLM(msgs);
        XX_TEST_EXPECT_EQ(r1, std::string{""});
        // 空消息 → 返回空串
        auto r2 = co_await env->handle->doSummarizeWithLLM({});
        XX_TEST_EXPECT_EQ(r2, std::string{""});
        // 伪造 subagent 返回预设总结
        env->subagent->summary = "fake summary";
        auto r3                = co_await env->handle->doSummarizeWithLLM(msgs);
        XX_TEST_EXPECT_EQ(r3, std::string{"fake summary"});
    }

    // ==================== offloadLongContentToTempStore ====================

    {
        auto  env = std::make_shared<SummarizationTestEnv>();
        auto& h   = env->handle;

        // 短内容 → 不处理, 不写入 share store
        auto shortMsg = makeMsg("user", "abc");
        h->offloadLongContentToTempStore(
            shortMsg,
            env->ctx->middlewareHandleContext,
            env->threadId
        );
        XX_TEST_EXPECT_EQ(shortMsg.content, std::string{"abc"});
        XX_TEST_EXPECT_FALSE(msgHasFlag(shortMsg, neograph::MessageFlag::ContentOffloaded));
        auto it = env->ctx->middlewareHandleContext->shareStore.find(env->threadId);
        XX_TEST_EXPECT_TRUE(it == env->ctx->middlewareHandleContext->shareStore.end());

        // 长内容 → 暂存 share store, content 替换为 id 引用
        auto longMsg = makeMsg("user", makeLongContent());
        h->offloadLongContentToTempStore(longMsg, env->ctx->middlewareHandleContext, env->threadId);
        XX_TEST_EXPECT_TRUE(msgHasFlag(longMsg, neograph::MessageFlag::ContentOffloaded));
        auto id = longMsg.extra.value<size_t>("offload_id", 0);
        XX_TEST_EXPECT_TRUE(id > 0);
        auto expected = fmt::format(
            "[Content offloaded. Use the `agentxx_share_store` tool to fetch the full content by "
            "ID {}]",
            id
        );
        XX_TEST_EXPECT_EQ(longMsg.content, expected);
        auto stored = env->ctx->middlewareHandleContext->getShareStoreItemValue(env->threadId, id);
        XX_TEST_EXPECT_TRUE(stored.has_value());
        XX_TEST_EXPECT_EQ(*stored, makeLongContent());

        // 已卸载过的消息 (内容已变短) → 不再重复卸载
        auto before = longMsg.content;
        h->offloadLongContentToTempStore(longMsg, env->ctx->middlewareHandleContext, env->threadId);
        XX_TEST_EXPECT_EQ(longMsg.content, before);

        // 多条长消息 → id 递增且互不覆盖
        auto a = makeMsg("user", makeLongContent(3000));
        auto b = makeMsg("user", makeLongContent(3000));
        h->offloadLongContentToTempStore(a, env->ctx->middlewareHandleContext, env->threadId);
        h->offloadLongContentToTempStore(b, env->ctx->middlewareHandleContext, env->threadId);
        auto idA = a.extra.value<size_t>("offload_id", 0);
        auto idB = b.extra.value<size_t>("offload_id", 0);
        XX_TEST_EXPECT_TRUE(idA != idB);
        XX_TEST_EXPECT_EQ(
            *env->ctx->middlewareHandleContext->getShareStoreItemValue(env->threadId, idA),
            makeLongContent(3000)
        );
        XX_TEST_EXPECT_EQ(
            *env->ctx->middlewareHandleContext->getShareStoreItemValue(env->threadId, idB),
            makeLongContent(3000)
        );
    }

    // ==================== doSummarizeToolcall ====================

    {
        auto  env = std::make_shared<SummarizationTestEnv>();
        auto& h   = env->handle;

        // --- A. response 去重 (filesystem 风格): 旧 response 截断, 新 response 保留,
        //       assistant request 不受影响 ---
        {
            h->summarizationToolHandles["read_file"] = makeReadFileHandle();
            std::vector<neograph::ChatMessage> msgs{
                makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
                makeToolResult("c1", "read_file", "r1"),
                makeAssistantToolcall("", {makeToolcall("c2", "read_file", R"({"path":"B"})")}),
                makeToolResult("c2", "read_file", "r2"),
                makeAssistantToolcall("", {makeToolcall("c3", "read_file", R"({"path":"A"})")}),
                makeToolResult("c3", "read_file", "r3"),
            };
            h->doSummarizeToolcall(msgs);
            XX_TEST_EXPECT_EQ(msgs.size(), size_t{6});
            // 旧 (path=A) 的 response 被截断
            XX_TEST_EXPECT_EQ(msgs[1].content, std::string{"[Truncated Response]"});
            // path=B 与新 path=A 的 response 保留
            XX_TEST_EXPECT_EQ(msgs[3].content, std::string{"r2"});
            XX_TEST_EXPECT_EQ(msgs[5].content, std::string{"r3"});
            // assistant request 不被截断 (truncateRequest == nullptr)
            XX_TEST_EXPECT_EQ(msgs[0].tool_calls[0].name, std::string{"read_file"});
            XX_TEST_EXPECT_EQ(msgs[4].tool_calls[0].arguments, std::string{R"({"path":"A"})"});
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
            // 索引 0 是 system 消息, 循环从 1 开始; 旧 request 全部截断
            XX_TEST_EXPECT_EQ(msgs[1].tool_calls[0].arguments, std::string{"[Truncated Request]"});
            XX_TEST_EXPECT_EQ(msgs[2].tool_calls[0].arguments, std::string{"[Truncated Request]"});
            // 最后一个 request 保留
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
            // 索引 0 的 request 已随 0f9f920 纳入去重循环 (i >= 0): 与后面的重复
            // request 一样被截断, 仅保留最后一组 (旧断言"索引 0 受循环边界保护"
            // 与新循环语义矛盾, 已同步更新)
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
    }

    // ==================== onModelcallRunFunc ====================

    // --- T1. 空消息 → 直接返回, 不更新上下文统计 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        auto res = co_await runModelcall(env->handle, env->ctx, env->threadId, {});
        XX_TEST_EXPECT_TRUE(res.empty());
        XX_TEST_EXPECT_EQ(contextTokensOf(env->ctx, env->threadId), size_t{0});
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->threadId), size_t{0});
    }

    // --- T2. token 用量低于 65% → 不压缩, 仅发布统计 (count 路径) ---
    {
        auto                               env = std::make_shared<SummarizationTestEnv>();
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("user", "hi"),
            makeMsg("assistant", "yo"),
        };
        // count = user(3+1+0) + assistant(3+2+0) = 9
        auto res = co_await runModelcall(env->handle, env->ctx, env->threadId, msgs);
        XX_TEST_EXPECT_EQ(res.size(), size_t{2});
        XX_TEST_EXPECT_EQ(res[0].content, std::string{"hi"});
        XX_TEST_EXPECT_EQ(res[1].content, std::string{"yo"});
        // 默认模型 "fallback" 未指定 maxToken → 使用中间件默认值 2048
        XX_TEST_EXPECT_EQ(contextTokensOf(env->ctx, env->threadId), size_t{9});
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->threadId), size_t{2048});
    }

    // --- T3. apiTokenUsage 存在时优先使用 (而非本地统计) ---
    {
        auto                               env = std::make_shared<SummarizationTestEnv>();
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("user", "hi"),
            makeMsg("assistant", "yo"),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->threadId, msgs, 100);
        XX_TEST_EXPECT_EQ(res.size(), size_t{2});
        XX_TEST_EXPECT_EQ(contextTokensOf(env->ctx, env->threadId), size_t{100});
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->threadId), size_t{2048});
    }

    // --- T4. 65% ~ 85% 之间: 只做 toolcall 去重 + 长内容暂存, 不做 LLM 总结 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small"); // max=1000, 阈值 650 / 850
        env->handle->summarizationToolHandles["read_file"] = makeReadFileHandle();
        auto                               longContent     = makeLongContent();
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("user", "u1"),
            makeAssistantToolcall("", {makeToolcall("c1", "read_file", R"({"path":"A"})")}),
            makeToolResult("c1", "read_file", "r1"),
            makeAssistantToolcall("", {makeToolcall("c2", "read_file", R"({"path":"A"})")}),
            makeToolResult("c2", "read_file", "r2"),
            makeMsg("user", longContent),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->threadId, msgs, 700);
        // 消息数量不变 (未做 LLM 总结)
        XX_TEST_EXPECT_EQ(res.size(), size_t{6});
        // toolcall 去重: 旧 response 截断, 新 response 保留
        XX_TEST_EXPECT_EQ(res[2].content, std::string{"[Truncated Response]"});
        XX_TEST_EXPECT_EQ(res[4].content, std::string{"r2"});
        // 长内容已暂存 share store
        XX_TEST_EXPECT_TRUE(msgHasFlag(res[5], neograph::MessageFlag::ContentOffloaded));
        auto offloadId = res[5].extra.value<size_t>("offload_id", 0);
        XX_TEST_EXPECT_TRUE(offloadId > 0);
        auto expected = fmt::format(
            "[Content offloaded. Use the `agentxx_share_store` tool to fetch the full content by "
            "ID {}]",
            offloadId
        );
        XX_TEST_EXPECT_EQ(res[5].content, expected);
        auto stored
            = env->ctx->middlewareHandleContext->getShareStoreItemValue(env->threadId, offloadId);
        XX_TEST_EXPECT_TRUE(stored.has_value());
        XX_TEST_EXPECT_EQ(*stored, longContent);
        // 没有出现总结消息
        bool hasSummary = false;
        for (const auto& m : res) {
            if (m.content.find("[Please compact context to save space]") != std::string::npos) {
                hasSummary = true;
            }
        }
        XX_TEST_EXPECT_FALSE(hasSummary);
        // 统计发布
        XX_TEST_EXPECT_EQ(contextTokensOf(env->ctx, env->threadId), size_t{700});
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->threadId), size_t{1000});
    }

    // --- T5. >= 85% 且 LLM 总结成功: system + 总结对 + 最近消息 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small"); // max=1000
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
        auto res = co_await runModelcall(env->handle, env->ctx, env->threadId, msgs, 900);
        // system(1) + 总结对(2) + 最近 4 条 = 7
        XX_TEST_EXPECT_EQ(res.size(), size_t{7});
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
        // 最近 4 条消息按原顺序保留
        XX_TEST_EXPECT_EQ(res[3].content, std::string{"u3"});
        XX_TEST_EXPECT_EQ(res[4].content, std::string{"a3"});
        XX_TEST_EXPECT_EQ(res[5].content, std::string{"u4"});
        XX_TEST_EXPECT_EQ(res[6].content, std::string{"a4"});
        // 被压缩的旧消息已消失
        bool hasOld = false;
        for (const auto& m : res) {
            if (m.content == "u1" || m.content == "a1" || m.content == "u2" || m.content == "a2") {
                hasOld = true;
            }
        }
        XX_TEST_EXPECT_FALSE(hasOld);
        XX_TEST_EXPECT_EQ(contextTokensOf(env->ctx, env->threadId), size_t{900});
    }

    // --- T6. >= 85% 但 LLM 总结失败 (空串) → 原样保留全部消息 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small");
        env->subagent->summary = ""; // 模拟压缩失败
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
        auto res = co_await runModelcall(env->handle, env->ctx, env->threadId, msgs, 900);
        XX_TEST_EXPECT_EQ(res.size(), size_t{9});
        for (size_t i = 0; i < msgs.size(); ++i) {
            XX_TEST_EXPECT_EQ(res[i].role, msgs[i].role);
            XX_TEST_EXPECT_EQ(res[i].content, msgs[i].content);
        }
    }

    // --- T7. 消息数 <= keepRecent + system → 不触发 LLM 总结 ---
    {
        auto env = std::make_shared<SummarizationTestEnv>();
        env->session()->setModelName("small");
        env->subagent->summary = "S1";
        std::vector<neograph::ChatMessage> msgs{
            makeMsg("system", "sys"),
            makeMsg("user", "u1"),
            makeMsg("assistant", "a1"),
            makeMsg("user", "u2"),
            makeMsg("assistant", "a2"),
        };
        auto res = co_await runModelcall(env->handle, env->ctx, env->threadId, msgs, 900);
        XX_TEST_EXPECT_EQ(res.size(), size_t{5});
        XX_TEST_EXPECT_EQ(res[1].content, std::string{"u1"});
        XX_TEST_EXPECT_EQ(res[4].content, std::string{"a2"});
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
        auto res = co_await runModelcall(env->handle, env->ctx, env->threadId, msgs, 900);
        // 总结对(2) + recent[assistant tc2, tool t2..t5](5) = 7
        XX_TEST_EXPECT_EQ(res.size(), size_t{7});
        XX_TEST_EXPECT_EQ(res[0].role, std::string{"user"});
        XX_TEST_EXPECT_EQ(res[0].content, std::string{"[Please compact context to save space]"});
        XX_TEST_EXPECT_EQ(res[1].role, std::string{"assistant"});
        // recent 以发起 tool 的 assistant 开头, 整组 tool 交换完整保留
        XX_TEST_EXPECT_EQ(res[2].role, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(res[2].content, std::string{"tc2"});
        XX_TEST_EXPECT_EQ(res[3].role, std::string{"tool"});
        XX_TEST_EXPECT_EQ(res[3].content, std::string{"t2"});
        XX_TEST_EXPECT_EQ(res[4].content, std::string{"t3"});
        XX_TEST_EXPECT_EQ(res[5].content, std::string{"t4"});
        XX_TEST_EXPECT_EQ(res[6].content, std::string{"t5"});
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
        auto res1 = co_await runModelcall(env->handle, env->ctx, env->threadId, msgs, 900);
        XX_TEST_EXPECT_EQ(res1.size(), size_t{8});
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->threadId), size_t{2048});

        // 切换 small (max=1000): 900 >= 850 → LLM 总结
        env->session()->setModelName("small");
        env->subagent->summary = "S";
        auto res2 = co_await runModelcall(env->handle, env->ctx, env->threadId, msgs, 900);
        XX_TEST_EXPECT_EQ(res2.size(), size_t{6}); // 总结对 + 最近 4 条
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->threadId), size_t{1000});

        // 切换 big (max=5000): 900 < 5000*0.65 → 不压缩
        env->session()->setModelName("big");
        auto res3 = co_await runModelcall(env->handle, env->ctx, env->threadId, msgs, 900);
        XX_TEST_EXPECT_EQ(res3.size(), size_t{8});
        XX_TEST_EXPECT_EQ(maxContextTokensOf(env->ctx, env->threadId), size_t{5000});
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
        auto res1 = co_await runModelcall(env->handle, env->ctx, env->threadId, msgs);
        XX_TEST_EXPECT_EQ(contextTokensOf(env->ctx, env->threadId), size_t{10});
        // 普通模型 (sendThinking=false): 9
        env->session()->setModelName("small");
        auto res2 = co_await runModelcall(env->handle, env->ctx, env->threadId, msgs);
        XX_TEST_EXPECT_EQ(contextTokensOf(env->ctx, env->threadId), size_t{9});
        // 消息均未被压缩
        XX_TEST_EXPECT_EQ(res1.size(), size_t{2});
        XX_TEST_EXPECT_EQ(res2.size(), size_t{2});
    }

    co_return TestResult{g_sum_passed, g_sum_failed};
}

} // namespace test
} // namespace agentxx
