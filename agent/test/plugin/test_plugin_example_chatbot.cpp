/// test_plugin_example_chatbot —— 真实插件 `agentxx_chatbot` 全流程
///
/// 装置与 `plugin_example_runtime` 一致 (真实插件动态库 + 真实 `AgentHost` + 模拟模型
/// 服务), 平台侧换成插件自带的**内存假平台** (`accounts[].type = "fake"`), 因此这条
/// 用例不连 QQ 也不消耗真实账号。
///
/// 覆盖 (对应 `resource/history/chatbot/plan.md` §16.1 的 `plugin_example_chatbot`):
/// 1. `start` 成功 → 人设根建好, 根上有 5 个 `chatbot_*` 工具 (靠 `includeCreator`);
/// 2. 私聊注入一条 → 通道懒创建 → 模型收到带说话人前缀的输入 → 工具发送 → 假平台收到
///    (带被动回复 id);
/// 3. `mention` 模式: 3 条无 @ 只进缓冲, 第 4 条带 @ 时一次投递带上全部 4 条;
/// 4. 同 `msg_id` 注入两次只处理一次 (`duplicated` 计数 +1);
/// 5. 长文本按上限切成多条, 分段标记齐全;
/// 6. 卸载插件 → 通道关闭、根回收、工具撤销。
#include "agentxx-test/plugin/test_plugin_example_chatbot.h"

#include "agentxx-test/core/test_agent.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "asio/co_spawn.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/use_future.hpp"
#include "fmt/format.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
int g_cb_passed = 0;
int g_cb_failed = 0;
} // namespace

#define XX_TEST_PASSED g_cb_passed
#define XX_TEST_FAILED g_cb_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

using namespace std::chrono_literals;
using utilxx_base::Json;

/// 插件实例名 (= 它挂的根的 owner; 也是 `plugin.list[]` 里的名字)
constexpr const char* kOwner = "agentxx_chatbot";

/// 定位插件库目录 (与其他插件测试同一策略: exe 同目录优先, cwd 回退)
std::string findPluginDir(std::string_view pluginName) {
    std::error_code       ec;
    std::vector<fs::path> candidates;
    if (auto exeDir = executableDir()) {
        candidates.push_back(*exeDir / "plugins" / pluginName);
    }
    candidates.push_back(fs::current_path(ec) / "plugins" / pluginName);
    auto hasLibFile = [](const fs::path& dir) {
        std::error_code                     ec2;
        std::filesystem::directory_iterator it(dir, ec2);
        std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec2)) {
            auto ext = it->path().extension().string();
            if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
                return true;
            }
        }
        return false;
    };
    for (const auto& c : candidates) {
        if (fs::is_directory(c, ec) && hasLibFile(c)) {
            return c.string();
        }
    }
    return {};
}

/// 等待条件成立 (在容器 io 线程上轮转等待)
asio::awaitable<bool> waitUntil(
    asio::any_io_executor     ex,
    std::chrono::milliseconds timeout,
    std::function<bool()>     cond
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        asio::steady_timer timer(ex);
        timer.expires_after(10ms);
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return cond();
}

/// 固定等待一段时间 (验证"没有第二次投递"这类否定结论时用)
asio::awaitable<void> delay(asio::any_io_executor ex, std::chrono::milliseconds ms) {
    asio::steady_timer timer(ex);
    timer.expires_after(ms);
    utilxx_base::AsioErrorCode ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

/// 调用一个工具并返回结果文本 (工具不存在或抛错时返回 `[Error: ...]` 文本)
asio::awaitable<std::string> callTool(
    const std::shared_ptr<agent::AgentContext>& ctx,
    std::string_view                            name,
    std::string_view                            argsJson = "{}"
) {
    auto tool = ctx->toolRegistry ? ctx->toolRegistry->find(name) : nullptr;
    if (!tool) {
        co_return fmt::format("[Error: 工具 '{}' 不存在]", name);
    }
    Json args = Json::object();
    if (!argsJson.empty()) {
        args = Json::parse(std::string{argsJson});
    }
    try {
        co_return co_await tool->execute_async(args);
    } catch (const std::exception& e) {
        co_return fmt::format("[Error: {}]", e.what());
    }
}

/// 按归属者取插件挂的根
std::shared_ptr<agent::BaseAgent>
    rootOf(const std::shared_ptr<agent::AgentHost>& host, std::string_view owner) {
    for (const auto& root : host->roots()) {
        if (root.owner == owner && !root.primary) {
            return root.agent;
        }
    }
    return nullptr;
}

/// 按 sessionId 前缀取通道 (插件的会话 id 以 `chatbot_<人设>_` 开头)
std::shared_ptr<plugin::RuntimeChannel> channelOfPrefix(
    const std::shared_ptr<plugin::PluginManager>& mgr,
    std::string_view                              prefix
) {
    for (const auto& channel : mgr->runtimeChannels_) {
        if (channel && channel->sessionId.rfind(prefix, 0) == 0) {
            return channel;
        }
    }
    return nullptr;
}

/// 解析 `chatbot_debug_outbox` 的返回 (失败返回空数组)
Json outboxOf(const std::string& text) {
    try {
        auto j = Json::parse(text);
        return j.is_array() ? j : Json::array();
    } catch (const std::exception&) {
        return Json::array();
    }
}

/// 出站记录里的正文按顺序拼接 (查关键字用)
std::string concatSentText(const Json& outbox) {
    std::string all;
    for (const auto& item : outbox) {
        if (item.contains("text") && item["text"].is_string()) {
            all += item["text"].get<std::string>();
            all += "\n";
        }
    }
    return all;
}

/// 轮转等待「假平台的出站记录满足条件」
///
/// `waitUntil` 的检查体是普通函数 (不能 `co_await`), 而取出站要调工具, 因此这里
/// 单独写一个协程版本。
asio::awaitable<bool> waitOutbox(
    asio::any_io_executor                ex,
    std::chrono::milliseconds            timeout,
    std::shared_ptr<agent::AgentContext> rootCtx,
    std::function<bool(const Json&)>      cond
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond(outboxOf(co_await callTool(rootCtx, "chatbot_debug_outbox", R"({"limit":50})")))) {
            co_return true;
        }
        asio::steady_timer timer(ex);
        timer.expires_after(30ms);
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return cond(outboxOf(co_await callTool(rootCtx, "chatbot_debug_outbox", R"({"limit":50})")));
}

/// 最近一次模型请求的完整文本 (查输入里带了什么)
std::string lastRequestText() {
    if (g_da_sim_requests.empty()) {
        return {};
    }
    return g_da_sim_requests.back().dump();
}

// ===========================================================================
// 用例主体 (全部在容器 io 线程上跑)
// ===========================================================================

asio::awaitable<void> runBody(
    const std::shared_ptr<asio::io_context>& io,
    std::string_view                         baseUrl,
    std::string                              pluginDir
) {
    const auto        ex     = io->get_executor();
    const std::string prompt = "你是 chatbot 插件测试里的人设。";

    // ---- 主程序级上下文 ----
    auto primaryCtx         = std::make_shared<agent::AgentContext>();
    primaryCtx->agentConfig = std::make_shared<agent::AgentConfig>();
    {
        auto& cfg                       = *primaryCtx->agentConfig;
        cfg.model.baseUrl               = std::string{baseUrl};
        cfg.model.apiKey                = "EMPTY";
        cfg.model.modelName             = "sim-model";
        cfg.availableModels["test-sim"] = cfg.model;
        cfg.currentModelName            = "test-sim";
        cfg.prompt.systemPrompt         = "You are the built-in assistant.";
        cfg.permissionMode              = agent::PermissionMode::Pass;
        cfg.pluginMountPolicy           = agent::PluginMountPolicy::None;
        cfg.enableSessionStore          = false;
        cfg.enableSubagent              = false;
    }
    primaryCtx->bus                     = std::make_shared<events::EventBus>(ex);
    primaryCtx->middlewareHandleContext = std::make_shared<middleware::MiddlewareContext>();
    primaryCtx->toolRegistry            = std::make_shared<plugin::ToolRegistry>();

    // ---- 插件管理器 (在容器 io 线程上创建) ----
    auto mgr = std::make_shared<plugin::PluginManager>(primaryCtx);
    mgr->setIoExecutor(ex);
    primaryCtx->pluginManager      = mgr;
    primaryCtx->pluginManagerOwned = false;

    // ---- 容器 ----
    agent::AgentHost::Config hostCfg;
    hostCfg.ioCtx        = io;
    hostCfg.maxRoots     = 4;
    hostCfg.agentFactory = [mgr](std::shared_ptr<agent::AgentConfig> cfg)
        -> std::shared_ptr<agent::BaseAgent> {
        auto agent = std::make_shared<agent::CodeAgent>(std::move(cfg));
        if (auto ctx = agent->getContext()) {
            ctx->pluginManager      = mgr;
            ctx->pluginManagerOwned = false;
        }
        return agent;
    };
    auto host        = agent::AgentHost::create(hostCfg);
    primaryCtx->host = host;

    primaryCtx->pluginMount = co_await mgr->mountAgentAsync(primaryCtx);

    // ---- 插件配置: 一个假账号 + 一个人设 (一个群 + 一个私聊) ----
    const std::string argsJson = fmt::format(
        R"JSON({{
  "accounts": [
    {{"id": "fake_main", "type": "fake"}}
  ],
  "personas": [
    {{
      "name": "assistant",
      "prompt": {{"systemPrompt": "{}"}},
      "bind": [
        {{"account": "fake_main", "chats": [
          {{"type": "group", "id": "G1", "mode": "mention"}},
          {{"type": "private", "id": "U1", "mode": "always"}},
          {{"type": "private", "id": "U2", "mode": "always"}},
          {{"type": "private", "id": "U3", "mode": "always"}},
          {{"type": "private", "id": "U4", "mode": "always"}}
        ]}}
      ]
    }}
  ],
  "behavior": {{
    "aggregateWindowMs": 150,
    "sendIntervalMs": 0,
    "messageSplitLimit": 120,
    "maxSplitParts": 8,
    "maxRepliesPerTurn": 5,
    "debugTools": true
  }},
  "commands": [
    {{"name": "/axx-info", "mode": "local",
      "reply": "会话 {{chat}} 人设 {{persona}} 模式 {{mode}} 收到 {{received}} 条"}},
    {{"name": "/axx-hello", "mode": "model", "prompt": "请向 {{sender}} 打招呼: {{args}}"}},
    {{"name": "/axx-stop", "mode": "action", "action": "cancel"}}
  ]
}})JSON",
        prompt
    );

    agent::PluginConfig pluginCfg;
    pluginCfg.path = pluginDir;
    pluginCfg.args = Json::parse(argsJson);
    auto inst      = co_await mgr->loadPluginAsync(pluginDir, &pluginCfg);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }

    // ===================== 1. 人设根与工具 =====================
    XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
        auto root = rootOf(host, kOwner);
        if (!root) {
            return false;
        }
        auto ctx = root->getContext();
        return ctx && ctx->toolRegistry && ctx->toolRegistry->contains("chatbot_send_message");
    }));
    XX_TEST_EXPECT_EQ(host->rootCount(), size_t{1});
    auto personaRoot = rootOf(host, kOwner);
    XX_TEST_EXPECT_TRUE(personaRoot != nullptr);
    if (!personaRoot) {
        co_return;
    }
    auto rootCtx = personaRoot->getContext();
    XX_TEST_EXPECT_TRUE(rootCtx != nullptr);
    if (!rootCtx) {
        co_return;
    }
    // 5 个业务工具 + 2 个调试工具都在人设根上
    for (const auto* name : {"chatbot_send_message",
                             "chatbot_history",
                             "chatbot_chat_info",
                             "chatbot_status",
                             "chatbot_set_mode",
                             "chatbot_debug_inject",
                             "chatbot_debug_outbox"}) {
        XX_TEST_EXPECT_TRUE(rootCtx->toolRegistry->contains(name));
    }
    if (rootCtx->agentConfig) {
        // 根的提示词来自插件给的 config_json (内置会话那份一个字没动)
        XX_TEST_EXPECT_EQ(rootCtx->agentConfig->prompt.systemPrompt, prompt);
        XX_TEST_EXPECT_TRUE(primaryCtx->agentConfig->prompt.systemPrompt != prompt);
    }
    // 内置 root 上看不到这些工具 (`exposeToolsOnHost` 默认关)
    XX_TEST_EXPECT_FALSE(primaryCtx->toolRegistry->contains("chatbot_send_message"));

    // 注入一条消息 (经调试工具, 不碰平台网络)
    // 直接用 Json 构造参数 (文本里带引号/换行也不会破坏报文)
    auto injectJson = [&rootCtx](Json args) -> asio::awaitable<bool> {
        const auto result = co_await callTool(rootCtx, "chatbot_debug_inject", args.dump());
        try {
            co_return Json::parse(result).value("accepted", false);
        } catch (const std::exception&) {
            co_return false;
        }
    };
    auto inject = [&injectJson](
                      std::string_view type,
                      std::string_view chatId,
                      std::string_view text,
                      bool             mentioned,
                      std::string_view msgId
                  ) -> asio::awaitable<bool> {
        Json args = Json::object();
        args["account"]     = "fake_main";
        args["type"]        = std::string{type};
        args["chatId"]      = std::string{chatId};
        args["text"]        = std::string{text};
        args["sender"]      = "小张";
        args["mentionedMe"] = mentioned;
        args["msgId"]       = std::string{msgId};
        co_return co_await injectJson(std::move(args));
    };

    // 等所有通道都空闲、且没有人设会话还攒着待投消息
    //
    // 模拟模型是全局状态: 上一轮的后续请求会带上下一段用例设置的 tool_calls / 正文,
    // 缓冲里还有旧消息时下一轮也会被提前触发。因此每段用例开始前都要等干净。
    auto drain = [&]() -> asio::awaitable<bool> {
        const auto deadline = std::chrono::steady_clock::now() + 120s;
        while (std::chrono::steady_clock::now() < deadline) {
            bool busy = false;
            for (const auto& channel : mgr->runtimeChannels_) {
                if (channel && channel->turnBusy) {
                    busy = true;
                    break;
                }
            }
            if (!busy) {
                bool buffered = false;
                for (const auto* key : {"fake_main|group|G1",
                                       "fake_main|private|U1",
                                       "fake_main|private|U2",
                                       "fake_main|private|U3",
                                       "fake_main|private|U4"}) {
                    const auto info = Json::parse(fmt::format(
                        "{}",
                        co_await callTool(
                            rootCtx,
                            "chatbot_chat_info",
                            fmt::format(R"({{"chat":"{}"}})", key)
                        )
                    ));
                    if (info.value("buffered", int64_t{0}) > 0) {
                        buffered = true;
                        break;
                    }
                }
                if (!buffered) {
                    co_return true;
                }
            }
            co_await delay(ex, 30ms);
        }
        co_return false;
    };

    // ===================== 2. 私聊一条 → 跑一轮 → 工具发送 =====================
    {
        g_da_sim_delay_ms             = 0;
        g_da_sim_tool_calls_remaining = 1;
        g_da_sim_response_content     = "";
        // 第一轮模型就调 chatbot_send_message
        g_da_sim_tool_calls = Json::array(
            {Json{
                {"index", 0},
                {"id", "call_chatbot_1"},
                {"type", "function"},
                {"function",
                 Json{
                     {"name", "chatbot_send_message"},
                     {"arguments", R"({"text":"收到, 我在。"})"},
                 }},
            }}
        );
        const int requestsBefore = g_da_sim_request_count.load();

        XX_TEST_EXPECT_TRUE(co_await inject("private", "U1", "你好", false, "msg-p1"));

        // 一轮跑完: 模型请求 +1, 通道建好并回到空闲
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
            return g_da_sim_request_count.load() > requestsBefore;
        }));
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
            auto channel = channelOfPrefix(mgr, "chatbot_assistant_");
            return channel != nullptr && channel->ready && !channel->turnBusy;
        }));
        // 通道是懒创建的, 且会话 id 以人设名开头
        auto channel = channelOfPrefix(mgr, "chatbot_assistant_");
        XX_TEST_EXPECT_TRUE(channel != nullptr);
        XX_TEST_EXPECT_TRUE(channel != nullptr && channel->owner == "agentxx_chatbot");

        // 输入里有说话人前缀 (模型看到的不是裸正文)
        XX_TEST_EXPECT_TRUE(lastRequestText().find("小张") != std::string::npos);
        XX_TEST_EXPECT_TRUE(lastRequestText().find("你好") != std::string::npos);

        // 假平台收到了出站, 且带被动回复 id (来自被投递的那条消息)
        XX_TEST_EXPECT_TRUE(co_await waitOutbox(
            ex,
            30s,
            rootCtx,
            [](const Json& outbox) {
                return concatSentText(outbox).find("收到, 我在。") != std::string::npos;
            }
        ));
        // 被动回复: 同一条消息回的次数从 1 开始计
        {
            const auto outbox = outboxOf(
                co_await callTool(rootCtx, "chatbot_debug_outbox", R"({"limit":10})")
            );
            bool matched = false;
            for (const auto& item : outbox) {
                if (!item.contains("text") || !item["text"].is_string()) {
                    continue;
                }
                if (item["text"].get<std::string>().find("收到, 我在。") != std::string::npos) {
                    matched = true;
                    XX_TEST_EXPECT_EQ(item.value("passiveTo", std::string{}), std::string{"msg-p1"});
                    XX_TEST_EXPECT_EQ(item.value("passiveSeq", -1), 1);
                }
            }
            XX_TEST_EXPECT_TRUE(matched);
        }
    }

    // ===================== 3. mention 模式: 无 @ 只缓冲 =====================
    {
        XX_TEST_EXPECT_TRUE(co_await drain());
        g_da_sim_tool_calls_remaining = 0;
        g_da_sim_tool_calls           = Json::array();
        g_da_sim_response_content     = "";
        const int requestsBefore      = g_da_sim_request_count.load();

        XX_TEST_EXPECT_TRUE(co_await inject("group", "G1", "群里的第一句", false, "msg-g1"));
        XX_TEST_EXPECT_TRUE(co_await inject("group", "G1", "群里的第二句", false, "msg-g2"));
        XX_TEST_EXPECT_TRUE(co_await inject("group", "G1", "群里的第三句", false, "msg-g3"));
        // 等过聚合窗口, 确认没有被投递 (mention 模式下无 @ 不唤醒)
        co_await delay(ex, 400ms);
        XX_TEST_EXPECT_EQ(g_da_sim_request_count.load(), requestsBefore);

        // 第 4 条带 @: 一次投递里带上前面 3 条 + 这条
        XX_TEST_EXPECT_TRUE(co_await inject("group", "G1", "@机器人 你们在聊什么", true, "msg-g4"));
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
            return g_da_sim_request_count.load() > requestsBefore;
        }));
        const auto request = lastRequestText();
        XX_TEST_EXPECT_TRUE(request.find("群里的第一句") != std::string::npos);
        XX_TEST_EXPECT_TRUE(request.find("群里的第二句") != std::string::npos);
        XX_TEST_EXPECT_TRUE(request.find("群里的第三句") != std::string::npos);
        XX_TEST_EXPECT_TRUE(request.find("你们在聊什么") != std::string::npos);
        // 聚合计数: 这一批里 3 条是被合并进去的
        const auto status = co_await callTool(rootCtx, "chatbot_status");
        XX_TEST_EXPECT_TRUE(
            Json::parse(status).value("counters", Json::object()).value("aggregated", int64_t{0})
            >= 3
        );
    }

    // ===================== 4. 去重: 同 msg_id 只处理一次 =====================
    {
        g_da_sim_tool_calls_remaining = 0;
        g_da_sim_tool_calls           = Json::array();
        const auto statusBefore       = Json::parse(
            co_await callTool(rootCtx, "chatbot_status")
        );
        const int64_t dupBefore
            = statusBefore.value("counters", Json::object()).value("duplicated", int64_t{0});

        XX_TEST_EXPECT_TRUE(co_await inject("private", "U1", "重复的消息", false, "msg-dup"));
        XX_TEST_EXPECT_TRUE(co_await inject("private", "U1", "重复的消息", false, "msg-dup"));

        const auto statusAfter = Json::parse(co_await callTool(rootCtx, "chatbot_status"));
        XX_TEST_EXPECT_EQ(
            statusAfter.value("counters", Json::object()).value("duplicated", int64_t{0}),
            dupBefore + 1
        );
    }

    // ===================== 5. 长文本切成多条 + 分段标记 =====================
    {
        // 300 个「长」字、上限 120 码点 → 至少 3 段, 且带 `(i/n)` 分段标记
        std::string longText;
        longText.reserve(900);
        for (int i = 0; i < 300; ++i) {
            longText += "\u957f";
        }
        g_da_sim_tool_calls_remaining = 1;
        g_da_sim_tool_calls           = Json::array(
            {Json{
                {"index", 0},
                {"id", "call_chatbot_long"},
                {"type", "function"},
                {"function",
                 Json{
                     {"name", "chatbot_send_message"},
                     {"arguments", Json{{"text", longText}}.dump()},
                 }},
            }}
        );
        const int requestsBefore = g_da_sim_request_count.load();

        XX_TEST_EXPECT_TRUE(co_await inject("private", "U2", "讲个长故事", false, "msg-long"));
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
            return g_da_sim_request_count.load() > requestsBefore;
        }));
        // 300 码点、上限 120 → 至少 3 段
        XX_TEST_EXPECT_TRUE(co_await waitOutbox(ex, 30s, rootCtx, [](const Json& outbox) {
            size_t count = 0;
            for (const auto& item : outbox) {
                if (!item.contains("text") || !item["text"].is_string()) {
                    continue;
                }
                if (item["text"].get<std::string>().find("\u957f") != std::string::npos) {
                    ++count;
                }
            }
            return count >= 3;
        }));
        const auto outbox = outboxOf(
            co_await callTool(rootCtx, "chatbot_debug_outbox", R"({"limit":50})")
        );
        bool hasMarker = false;
        for (const auto& item : outbox) {
            if (!item.contains("text") || !item["text"].is_string()) {
                continue;
            }
            const auto& text = item["text"].get<std::string>();
            if (text.find("(1/") != std::string::npos || text.find("（1/") != std::string::npos) {
                hasMarker = true;
            }
        }
        XX_TEST_EXPECT_TRUE(hasMarker);
    }

    // ===================== 6. 命令: local 直回 / model 进模型 =====================
    {
        // 上一轮的第二次模型请求会吞掉本段设置的 tool_calls, 先等干净
        XX_TEST_EXPECT_TRUE(co_await drain());
        // local: 直接出站, 不产生模型调用
        g_da_sim_tool_calls_remaining = 0;
        g_da_sim_tool_calls           = Json::array();
        g_da_sim_response_content     = "";
        const int requestsBefore      = g_da_sim_request_count.load();

        XX_TEST_EXPECT_TRUE(co_await inject("private", "U1", "/axx-info", false, "msg-cmd1"));
        XX_TEST_EXPECT_TRUE(co_await waitOutbox(ex, 30s, rootCtx, [](const Json& outbox) {
            return concatSentText(outbox).find("人设 assistant") != std::string::npos;
        }));
        // 命令不进模型
        XX_TEST_EXPECT_EQ(g_da_sim_request_count.load(), requestsBefore);
        // 模板里的 {chat} 被渲染成会话 id
        {
            const auto outbox = outboxOf(
                co_await callTool(rootCtx, "chatbot_debug_outbox", R"({"limit":20})")
            );
            bool rendered = false;
            for (const auto& item : outbox) {
                if (!item.contains("text") || !item["text"].is_string()) {
                    continue;
                }
                const auto& text = item["text"].get<std::string>();
                if (text.find("U1") != std::string::npos
                    && text.find("{chat}") == std::string::npos) {
                    rendered = true;
                }
            }
            XX_TEST_EXPECT_TRUE(rendered);
        }

        // model: 产生一轮模型调用, 输入是 prompt 渲染结果
        g_da_sim_response_content = "你好呀";
        const int beforeModel     = g_da_sim_request_count.load();
        XX_TEST_EXPECT_TRUE(co_await inject("private", "U1", "/axx-hello 请简短", false, "msg-cmd2"));
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
            return g_da_sim_request_count.load() > beforeModel;
        }));
        const auto request = lastRequestText();
        XX_TEST_EXPECT_TRUE(request.find("请向") != std::string::npos);
        XX_TEST_EXPECT_TRUE(request.find("请简短") != std::string::npos);
    }

    // ===================== 7. 附件: 图片进输入 + 文本里有附件行 =====================
    {
        XX_TEST_EXPECT_TRUE(co_await drain());
        g_da_sim_tool_calls_remaining = 0;
        g_da_sim_tool_calls           = Json::array();
        g_da_sim_response_content     = "";
        const int requestsBefore      = g_da_sim_request_count.load();

        Json args = Json::object();
        args["account"]     = "fake_main";
        args["type"]        = "private";
        args["chatId"]      = "U1";
        args["text"]        = "看这张图";
        args["sender"]      = "小张";
        args["msgId"]       = "msg-img";
        args["attachments"] = Json::array(
            {Json{{"kind", "image"},
                  {"url", "https://example.invalid/a.png"},
                  {"fileName", "a.png"},
                  {"mimeType", "image/png"},
                  {"sizeBytes", 1024}}}
        );
        XX_TEST_EXPECT_TRUE(co_await injectJson(args));
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
            return g_da_sim_request_count.load() > requestsBefore;
        }));
        const auto request = lastRequestText();
        // 图片作为多模态附件投给模型 (主程序的 input.attachments)
        XX_TEST_EXPECT_TRUE(request.find("image/png") != std::string::npos
                            || request.find("a.png") != std::string::npos);
        XX_TEST_EXPECT_TRUE(request.find("看这张图") != std::string::npos);
    }

    // ===================== 8. 一条回复超长 → 按上限切成多条 =====================
    {
        XX_TEST_EXPECT_TRUE(co_await drain());
        // 上限 120 码点、maxRepliesPerTurn = 5, 一次给 700 字 → 至少切成 5 段
        std::string longText;
        longText.reserve(2100);
        for (int i = 0; i < 700; ++i) {
            longText += "\u5b57";
        }
        g_da_sim_tool_calls_remaining = 1;
        g_da_sim_tool_calls           = Json::array(
            {Json{
                {"index", 0},
                {"id", "call_chatbot_huge"},
                {"type", "function"},
                {"function",
                 Json{
                     {"name", "chatbot_send_message"},
                     {"arguments", Json{{"text", longText}}.dump()},
                 }},
            }}
        );
        const int requestsBefore = g_da_sim_request_count.load();
        XX_TEST_EXPECT_TRUE(co_await inject("private", "U3", "背课文", false, "msg-huge"));
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
            return g_da_sim_request_count.load() > requestsBefore;
        }));
        // 一轮最多 5 条: 队列里出现 5 条该会话的出站
        XX_TEST_EXPECT_TRUE(co_await waitOutbox(ex, 30s, rootCtx, [](const Json& outbox) {
            size_t count = 0;
            for (const auto& item : outbox) {
                if (item.value("chatId", std::string{}) != "U3") {
                    continue;
                }
                if (!item.contains("text") || !item["text"].is_string()) {
                    continue;
                }
                if (item["text"].get<std::string>().find("\u5b57") != std::string::npos) {
                    ++count;
                }
            }
            return count >= 5;
        }));
        // 每段都不超过上限 (含分段标记的少量余量, 这里只查明显超限)
        {
            const auto outbox = outboxOf(
                co_await callTool(rootCtx, "chatbot_debug_outbox", R"({"limit":50})")
            );
            bool tooLong = false;
            for (const auto& item : outbox) {
                if (item.value("chatId", std::string{}) != "U3") {
                    continue;
                }
                if (!item.contains("text") || !item["text"].is_string()) {
                    continue;
                }
                if (item["text"].get<std::string>().size() > 400) {
                    tooLong = true;
                }
            }
            XX_TEST_EXPECT_FALSE(tooLong);
        }
    }

    // ===================== 9. alreadySent 额度: maxRepliesPerTurn 生效 =====================
    {
        XX_TEST_EXPECT_TRUE(co_await drain());
        // 一轮里连调 7 次 → 第 6 次起报「已发满」
        Json calls = Json::array();
        for (int i = 0; i < 7; ++i) {
            calls.push_back(Json{
                {"index", i},
                {"id", fmt::format("call_quota_{}", i)},
                {"type", "function"},
                {"function",
                 Json{{"name", "chatbot_send_message"},
                      {"arguments", Json{{"text", fmt::format("第 {} 条", i + 1)}}.dump()}}},
            });
        }
        g_da_sim_tool_calls_remaining = 1;
        g_da_sim_tool_calls           = calls;
        const int requestsBefore      = g_da_sim_request_count.load();

        XX_TEST_EXPECT_TRUE(co_await inject("private", "U4", "说七件事", false, "msg-quota"));
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
            return g_da_sim_request_count.load() > requestsBefore;
        }));
        // 出站不超过 5 条
        XX_TEST_EXPECT_TRUE(co_await waitOutbox(ex, 30s, rootCtx, [](const Json& outbox) {
            size_t count = 0;
            for (const auto& item : outbox) {
                if (item.value("chatId", std::string{}) == "U4"
                    && !item.value("text", std::string{}).empty()) {
                    ++count;
                }
            }
            return count >= 5;
        }));
        const auto outbox = outboxOf(
            co_await callTool(rootCtx, "chatbot_debug_outbox", R"({"limit":50})")
        );
        size_t quotaCount = 0;
        for (const auto& item : outbox) {
            if (item.value("chatId", std::string{}) == "U4"
                && !item.value("text", std::string{}).empty()) {
                ++quotaCount;
            }
        }
        XX_TEST_EXPECT_TRUE(quotaCount <= 5);
    }

    // ===================== 10. mention 立即唤醒 + 被动回复 id =====================
    {
        XX_TEST_EXPECT_TRUE(co_await drain());
        g_da_sim_tool_calls_remaining = 1;
        g_da_sim_tool_calls           = Json::array(
            {Json{
                {"index", 0},
                {"id", "call_chatbot_group"},
                {"type", "function"},
                {"function",
                 Json{{"name", "chatbot_send_message"},
                      {"arguments", R"({"text":"群里收到, 回你一句。"})"}}},
            }}
        );
        const int requestsBefore = g_da_sim_request_count.load();
        XX_TEST_EXPECT_TRUE(co_await inject("group", "G1", "@机器人 在吗", true, "msg-g10"));
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
            return g_da_sim_request_count.load() > requestsBefore;
        }));
        XX_TEST_EXPECT_TRUE(co_await waitOutbox(ex, 30s, rootCtx, [](const Json& outbox) {
            return concatSentText(outbox).find("群里收到") != std::string::npos;
        }));
        // 被 @ 的那条 (最后一条) 就是被动回复 id
        const auto outbox = outboxOf(
            co_await callTool(rootCtx, "chatbot_debug_outbox", R"({"limit":50})")
        );
        bool matched = false;
        for (const auto& item : outbox) {
            const auto text = item.value("text", std::string{});
            if (text.find("群里收到") == std::string::npos) {
                continue;
            }
            matched = true;
            XX_TEST_EXPECT_EQ(item.value("passiveTo", std::string{}), std::string{"msg-g10"});
            XX_TEST_EXPECT_EQ(item.value("passiveSeq", -1), 1);
        }
        XX_TEST_EXPECT_TRUE(matched);
    }

    // ===================== 11. 工具查询面: history / chat_info / set_mode =====================
    {
        XX_TEST_EXPECT_TRUE(co_await drain());
        // history: 拿到之前投过的那些原文 (含被 mention 过滤掉的群消息)
        const auto history = co_await callTool(
            rootCtx,
            "chatbot_history",
            R"({"chat":"fake_main|group|G1","limit":20})"
        );
        XX_TEST_EXPECT_TRUE(history.find("群里的第一句") != std::string::npos);
        XX_TEST_EXPECT_TRUE(history.find("@机器人") != std::string::npos);

        // chat_info: 路由/模式/通道/计数都在
        const auto info = Json::parse(
            co_await callTool(rootCtx, "chatbot_chat_info", R"({"chat":"fake_main|group|G1"})")
        );
        XX_TEST_EXPECT_EQ(info.value("persona", std::string{}), std::string{"assistant"});
        XX_TEST_EXPECT_EQ(info.value("chatType", std::string{}), std::string{"group"});
        XX_TEST_EXPECT_EQ(info.value("mode", std::string{}), std::string{"mention"});
        XX_TEST_EXPECT_TRUE(info.value("received", int64_t{0}) >= 0);
        XX_TEST_EXPECT_TRUE(info.value("counters", Json::object()).is_object());

        // 不属于任何人设的会话被拒
        const auto rejected = co_await callTool(
            rootCtx,
            "chatbot_chat_info",
            R"({"chat":"fake_main|group|NOT_MINE"})"
        );
        XX_TEST_EXPECT_TRUE(rejected.rfind("[Error:", 0) == 0);

        // set_mode 改成 always → 后续无 @ 也投递 (运行期生效)
        const auto changed = Json::parse(
            co_await callTool(
                rootCtx,
                "chatbot_set_mode",
                R"({"chat":"fake_main|group|G1","mode":"always"})"
            )
        );
        XX_TEST_EXPECT_EQ(changed.value("mode", std::string{}), std::string{"always"});
        const auto infoAfter = Json::parse(
            co_await callTool(rootCtx, "chatbot_chat_info", R"({"chat":"fake_main|group|G1"})")
        );
        XX_TEST_EXPECT_EQ(infoAfter.value("mode", std::string{}), std::string{"always"});

        // always 生效: 无 @ 的新消息也会投递
        g_da_sim_tool_calls_remaining = 0;
        g_da_sim_tool_calls           = Json::array();
        g_da_sim_response_content     = "";
        const int requestsBefore      = g_da_sim_request_count.load();
        XX_TEST_EXPECT_TRUE(co_await inject("group", "G1", "改成 always 之后的一句", false, "msg-always"));
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
            return g_da_sim_request_count.load() > requestsBefore;
        }));

        // 改回 mention (不影响后面的用例)
        co_await callTool(
            rootCtx,
            "chatbot_set_mode",
            R"({"chat":"fake_main|group|G1","mode":"mention"})"
        );
    }

    // ===================== 12. 兜底发送: 模型只输出正文不调工具 =====================
    {
        XX_TEST_EXPECT_TRUE(co_await drain());
        g_da_sim_tool_calls_remaining = 0;
        g_da_sim_tool_calls           = Json::array();
        g_da_sim_response_content     = "这句是正文, 没有调工具。";
        const int requestsBefore      = g_da_sim_request_count.load();

        XX_TEST_EXPECT_TRUE(co_await inject("group", "G1", "@机器人 直接说一句", true, "msg-fb1"));
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
            return g_da_sim_request_count.load() > requestsBefore;
        }));
        // `fallbackReply` 默认 mention: 被 @ 时把最后一段正文兜底发出 (不必调工具)
        XX_TEST_EXPECT_TRUE(co_await waitOutbox(ex, 30s, rootCtx, [](const Json& outbox) {
            return concatSentText(outbox).find("这句是正文") != std::string::npos;
        }));
        // 兜底发送被计数 (outbound.fallback 或者 chat_info 的 sent 增长)
        const auto info = Json::parse(
            co_await callTool(rootCtx, "chatbot_chat_info", R"({"chat":"fake_main|group|G1"})")
        );
        XX_TEST_EXPECT_TRUE(
            info.value("counters", Json::object()).value("sent", int64_t{0}) >= 1
        );
    }

    // ===================== 13. 卸载插件 → 通道关闭、根回收 =====================
    {
        XX_TEST_EXPECT_TRUE(co_await mgr->unloadAsync("agentxx_chatbot", 20s));
        XX_TEST_EXPECT_TRUE(channelOfPrefix(mgr, "chatbot_assistant_") == nullptr);
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 20s, [&]() {
            return host->rootCount() == size_t{0};
        }));
        XX_TEST_EXPECT_TRUE(rootOf(host, kOwner) == nullptr);
        XX_TEST_EXPECT_FALSE(primaryCtx->toolRegistry->contains("chatbot_send_message"));
        XX_TEST_EXPECT_TRUE(mgr->list().empty());
    }

    co_await mgr->unmountAgentAsync(primaryCtx->agentId);
    XX_TEST_EXPECT_TRUE(co_await mgr->shutdownAsync(10s));
    primaryCtx->pluginMount.reset();
}

} // namespace

// ===========================================================================
// 入口
// ===========================================================================

asio::awaitable<TestResult> run_plugin_example_chatbot_tests() {
    g_cb_passed = 0;
    g_cb_failed = 0;

    const auto pluginDir = findPluginDir("agentxx_chatbot");
    XX_TEST_EXPECT_TRUE(!pluginDir.empty());
    if (pluginDir.empty()) {
        co_return TestResult{g_cb_passed, g_cb_failed};
    }

    auto sim                      = startDaSimServer();
    auto baseUrl                  = "http://127.0.0.1:" + std::to_string(sim.port);
    g_da_sim_response_content     = "";
    g_da_sim_tool_calls           = Json::array();
    g_da_sim_tool_calls_remaining = 0;
    g_da_sim_delay_ms             = 0;

    auto io   = std::make_shared<asio::io_context>();
    auto work = asio::make_work_guard(*io);
    auto fut  = asio::co_spawn(*io, runBody(io, baseUrl, pluginDir), asio::use_future);
    std::thread ioThread([io]() {
        io->run();
    });

    try {
        fut.get();
    } catch (const std::exception& e) {
        TEST_FAIL << "plugin_example_chatbot body threw: " << e.what() << std::endl;
        g_cb_failed++;
    }

    work.reset();
    io->stop();
    if (ioThread.joinable()) {
        ioThread.join();
    }
    std::cout << "[INFO] example_chatbot done: passed=" << g_cb_passed
              << " failed=" << g_cb_failed << std::endl;
    co_return TestResult{g_cb_passed, g_cb_failed};
}

} // namespace test
} // namespace agentxx
