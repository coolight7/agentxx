// test_ffi_c_api.cpp —— libagentxx FFI C API 集成测试
//
// 测试方式: 直接链接 libagentxx (静态副本也含 ffi_api 实现, 符号一致) 并以
// 纯 C 方式调用 agentxx_* 导出接口; mock OpenAI 兼容 HTTP 服务器模拟 LLM。
// 覆盖:
//  1. 版本/内存/错误串
//  2. create 错误路径 (非法 JSON/缺模型) → NULL + char** log 详情
//  3. 生命周期 + 会话对话: create→start→EVT_READY→send_input→流式 DELTA→TURN_END
//  4. 同步查询: get_model_info / get_context_messages / list_sessions
//  5. HIL 权限中断: mock 返回工具调用 → EVT_INTERRUPT_REQ → 后台线程应答 → 轮次恢复
//  6. 取消: 慢 LLM 响应中 agentxx_cancel → TURN_END interrupted
//  7. 状态错误: stop 后 send_input → AGENTXX_ERR_STATE; drain_logs
#include "test_ffi_c_api.h"

#include "agentxx/ffi_api.h"
#include "agentxx/util/http_server.h"
#include "neograph/json.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

static std::string findPluginDir(const char* pluginName) {
    namespace fs = std::filesystem;
    std::error_code       ec;
    std::vector<fs::path> candidates;
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
        candidates.push_back(fs::path(buf).parent_path() / "plugins" / pluginName);
    }
#else
    if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec) {
        candidates.push_back(p.parent_path() / "plugins" / pluginName);
    }
#endif
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
    return std::string{"plugins/"} + pluginName;
}

// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ffi_passed = 0;
int g_ffi_failed = 0;

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ffi_passed
#define XX_TEST_FAILED g_ffi_failed

// ---------------------------------------------------------------------------
// 事件记录器 (C 回调 → 线程安全事件列表)
// ---------------------------------------------------------------------------

struct FfiEventRecorder {
    mutable std::mutex                                    m;
    std::vector<std::pair<AgentxxEventType, std::string>> events;

    static void onEvent(AgentxxEventType type, const char* payload, void* ud) {
        auto* self = static_cast<FfiEventRecorder*>(ud);
        if (self == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(self->m);
        self->events.emplace_back(type, payload == nullptr ? "" : std::string(payload));
    }

    bool has(AgentxxEventType t) const {
        std::lock_guard<std::mutex> lock(m);
        for (const auto& [type, payload] : events) {
            if (type == t) {
                return true;
            }
        }
        return false;
    }

    /// 等待某种事件出现 (轮询, 最多 timeoutMs)
    bool wait(AgentxxEventType t, int timeoutMs) {
        const auto deadline
            = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (has(t)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return has(t);
    }

    /// 返回首个匹配事件的 payload
    std::string first(AgentxxEventType t) const {
        std::lock_guard<std::mutex> lock(m);
        for (const auto& [type, payload] : events) {
            if (type == t) {
                return payload;
            }
        }
        return {};
    }

    /// 出现过 kind=xxx 的 delta
    bool hasDeltaKind(const char* kind) const {
        std::lock_guard<std::mutex> lock(m);
        for (const auto& [type, payload] : events) {
            if (type != AGENTXX_EVT_DELTA) {
                continue;
            }
            try {
                auto j = neograph::json::parse(payload);
                if (j.value("kind", std::string{}) == kind) {
                    return true;
                }
            } catch (...) {
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// mock OpenAI 兼容 LLM 服务器 (非流式 /chat/completions)
// ---------------------------------------------------------------------------

struct FfiMockLLM {
    std::unique_ptr<agentxx::util::HttpServer> server;
    std::thread                                thread;
    std::atomic<int>                           requestCount{0};
    /// 首个请求返回工具调用 (模拟需要权限/输入的工具选择)
    bool firstIsToolCall = false;
    /// >0 时响应前延迟 (取消测试用)
    int slowMs = 0;

    ~FfiMockLLM() {
        stop();
    }

    /// 文本流式 SSE chunks (openai chat completion 流式格式)
    static std::string textSse(std::string_view content) {
        const std::string id = "chatcmpl-ffi-mock";
        return std::string("data: ") + neograph::json{
            {"id", id},
            {"object", "chat.completion.chunk"},
            {"model", "ffi-mock"},
            {"choices", neograph::json::array({
                neograph::json{{"index", 0}, {"delta", {{"role", "assistant"}, {"content", ""}}}},
            })},
        }.dump() + "\n\n"
        + "data: " + neograph::json{
            {"id", id},
            {"object", "chat.completion.chunk"},
            {"model", "ffi-mock"},
            {"choices", neograph::json::array({
                neograph::json{{"index", 0}, {"delta", {{"content", content}}}},
            })},
        }.dump() + "\n\n"
        + "data: " + neograph::json{
            {"id", id},
            {"object", "chat.completion.chunk"},
            {"model", "ffi-mock"},
            {"choices", neograph::json::array({
                neograph::json{{"index", 0}, {"delta", neograph::json::object()}, {"finish_reason", "stop"}},
            })},
        }.dump() + "\n\n"
        + "data: [DONE]\n\n";
    }

    /// 工具调用流式 SSE chunks
    static std::string toolCallSse(const char* toolName, std::string_view argsJson) {
        const std::string id = "chatcmpl-ffi-tool";
        return std::string("data: ") + neograph::json{
            {"id", id},
            {"object", "chat.completion.chunk"},
            {"model", "ffi-mock"},
            {"choices", neograph::json::array({
                neograph::json{
                    {"index", 0},
                    {"delta", {{"role", "assistant"}, {"tool_calls", neograph::json::array({
                        neograph::json{{"index", 0}, {"id", "call_ffi_1"}, {"type", "function"}, {"function", {{"name", toolName}, {"arguments", ""}}}},
                    })}}},
                },
            })},
        }.dump() + "\n\n"
        + "data: " + neograph::json{
            {"id", id},
            {"object", "chat.completion.chunk"},
            {"model", "ffi-mock"},
            {"choices", neograph::json::array({
                neograph::json{
                    {"index", 0},
                    {"delta", {{"tool_calls", neograph::json::array({
                        neograph::json{{"index", 0}, {"function", {{"arguments", argsJson}}}},
                    })}}},
                },
            })},
        }.dump() + "\n\n"
        + "data: " + neograph::json{
            {"id", id},
            {"object", "chat.completion.chunk"},
            {"model", "ffi-mock"},
            {"choices", neograph::json::array({
                neograph::json{{"index", 0}, {"delta", neograph::json::object()}, {"finish_reason", "tool_calls"}},
            })},
        }.dump() + "\n\n"
        + "data: [DONE]\n\n";
    }

    bool start(uint16_t& outPort) {
        server    = std::make_unique<agentxx::util::HttpServer>(agentxx::util::HttpServer::Config{
               .address   = "127.0.0.1",
               .port      = 0,
               .ioThreads = 1,
        });
        auto mock = this;
        auto handler = std::make_shared<agentxx::util::HttpServer::Handler>(
            [mock](
                agentxx::util::HttpServer::Request& /*req*/,
                agentxx::util::HttpServer::Response& resp,
                std::string_view /*matched*/
            ) -> asio::awaitable<void> {
                const int n = mock->requestCount.fetch_add(1);
                if (mock->slowMs > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(mock->slowMs));
                }
                resp.result(boost::beast::http::status::ok);
                resp.set(boost::beast::http::field::content_type, "text/event-stream");
                resp.set(boost::beast::http::field::cache_control, "no-cache");
                resp.keep_alive(false);
                if (n == 0 && mock->firstIsToolCall) {
                    resp.body()
                        = toolCallSse("agentxx_filesystem_read", R"({"path": "/etc/hostname"})");
                } else {
                    resp.body() = textSse("hello from ffi mock");
                }
                resp.prepare_payload();
                co_return;
            }
        );
        server->router().add("/chat/completions", 2, handler);
        server->router().add("/v1/chat/completions", 2, handler);
        thread = std::thread([s = server.get()]() {
            s->start();
        });
        for (int i = 0; i < 100; ++i) {
            outPort = server->port();
            if (outPort != 0) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stop();
        return false;
    }

    void stop() {
        if (server) {
            server->stop();
        }
        if (thread.joinable()) {
            thread.join();
            thread = std::thread{};
        }
        server.reset();
    }

    /// 构造 model_json 指向本 mock
    std::string modelJson() const {
        return neograph::json{
            {"name",      "ffi-mock"                                          },
            {"type",      "openai"                                            },
            {"baseUrl",   "http://127.0.0.1:" + std::to_string(server->port())},
            {"apiKey",    "EMPTY"                                             },
            {"modelName", "ffi-mock"                                          },
        }
            .dump();
    }
};

// ---------------------------------------------------------------------------
// 测试用例
// ---------------------------------------------------------------------------

/// 1) 版本 / 内存 / 错误串
void testVersionAndMemory() {
    XX_TEST_EXPECT_EQ(agentxx_ffi_api_version(), AGENTXX_FFI_API_VERSION);
    XX_TEST_EXPECT_TRUE(agentxx_ffi_library_version() != nullptr);
    XX_TEST_EXPECT_TRUE(std::string(agentxx_ffi_strerror(AGENTXX_OK)) == "success");
    XX_TEST_EXPECT_TRUE(
        std::string(agentxx_ffi_strerror(AGENTXX_ERR_INVALID)) == "invalid argument"
    );

    // strdup_n / free 往返
    const char* text = "hello ffi";
    char*       dup  = agentxx_strdup_n(text, std::strlen(text));
    XX_TEST_EXPECT_TRUE(dup != nullptr);
    XX_TEST_EXPECT_TRUE(std::strcmp(dup, text) == 0);
    agentxx_free(dup);

    // 带 NUL 的任意字节段
    const char bytes[] = {'a', 'b', '\0', 'c', 'd'};
    char*      p       = agentxx_strdup_n(bytes, sizeof(bytes));
    XX_TEST_EXPECT_TRUE(p != nullptr);
    XX_TEST_EXPECT_TRUE(std::memcmp(p, bytes, sizeof(bytes)) == 0);
    agentxx_free(p);
}

/// 2) create 错误路径 → NULL + log
void testCreateInvalid() {
    char* log = nullptr;

    // 无模型配置
    AgentxxAgent* a = agentxx_create(nullptr, nullptr, nullptr, &log);
    XX_TEST_EXPECT_TRUE(a == nullptr);
    XX_TEST_EXPECT_TRUE(log != nullptr && std::strstr(log, "模型配置") != nullptr);
    agentxx_free(log);
    log = nullptr;

    // config_json 非法 JSON
    a = agentxx_create("{not json", "{}", nullptr, &log);
    XX_TEST_EXPECT_TRUE(a == nullptr);
    XX_TEST_EXPECT_TRUE(log != nullptr && std::strstr(log, "非法 JSON") != nullptr);
    agentxx_free(log);
    log = nullptr;

    // model_json 非法 (无 baseUrl/apiKey → isValid() 失败)
    a = agentxx_create(nullptr, "{\"name\":\"x\"}", nullptr, &log);
    XX_TEST_EXPECT_TRUE(a == nullptr);
    XX_TEST_EXPECT_TRUE(log != nullptr && std::strstr(log, "模型配置非法") != nullptr);
    agentxx_free(log);
    log = nullptr;

    // NULL 句柄校验
    XX_TEST_EXPECT_EQ(agentxx_destroy(nullptr, &log), AGENTXX_ERR_INVALID);
    agentxx_free(log);
    log = nullptr;
}

/// 3) 生命周期 + 会话对话 + 同步查询 (mock LLM)
void testLifecycleAndConversation() {
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    FfiEventRecorder rec;
    AgentxxCallbacks cb;
    std::memset(&cb, 0, sizeof(cb));
    cb.on_event  = FfiEventRecorder::onEvent;
    cb.user_data = &rec;

    char*         log = nullptr;
    AgentxxAgent* a   = agentxx_create(nullptr, mock.modelJson().c_str(), &cb, &log);
    if (a == nullptr) {
        TEST_FAIL << "create failed: " << (log ? log : "?") << std::endl;
        agentxx_free(log);
        g_ffi_failed++;
        mock.stop();
        return;
    }

    // start → 等待 EVT_READY
    XX_TEST_EXPECT_EQ(agentxx_start(a, &log), AGENTXX_OK);
    XX_TEST_EXPECT_TRUE(rec.wait(AGENTXX_EVT_READY, 20000));
    // READY payload 含 sessionId
    {
        auto ready = rec.first(AGENTXX_EVT_READY);
        XX_TEST_EXPECT_TRUE(ready.find("\"sessionId\"") != std::string::npos);
    }

    // 同步查询: 模型信息
    char* mi = agentxx_get_model_info(a, &log);
    XX_TEST_EXPECT_TRUE(mi != nullptr && std::strstr(mi, "currentModel") != nullptr);
    agentxx_free(mi);

    // 发送输入 → 流式 delta → 轮次结束
    XX_TEST_EXPECT_EQ(agentxx_send_input(a, "hello", &log), AGENTXX_OK);
    XX_TEST_EXPECT_TRUE(rec.wait(AGENTXX_EVT_TURN_END, 30000));
    XX_TEST_EXPECT_TRUE(rec.hasDeltaKind("text_token"));
    // TURN_END 无错误
    {
        auto turn = rec.first(AGENTXX_EVT_TURN_END);
        try {
            auto j = neograph::json::parse(turn);
            XX_TEST_EXPECT_TRUE(j.value("hasError", true) == false);
        } catch (...) {
            g_ffi_failed++;
            TEST_FAIL << "TURN_END payload not JSON: " << turn << std::endl;
        }
    }
    // 应收到过 EVT_MODEL_INFO (启动时自动请求)
    XX_TEST_EXPECT_TRUE(rec.has(AGENTXX_EVT_MODEL_INFO));

    // 同步查询: LLM 上下文 (一轮后应有 user/assistant 消息)
    char* ctx = agentxx_get_context_messages(a, &log);
    XX_TEST_EXPECT_TRUE(ctx != nullptr);
    if (ctx != nullptr) {
        try {
            auto j = neograph::json::parse(ctx);
            XX_TEST_EXPECT_TRUE(j.contains("messages") && j["messages"].is_array());
            XX_TEST_EXPECT_TRUE(j["messages"].size() > 0);
        } catch (...) {
            g_ffi_failed++;
            TEST_FAIL << "context payload not JSON: " << ctx << std::endl;
        }
        agentxx_free(ctx);
    }

    // 同步查询: 持久化会话列表 (未开启持久化 → 空数组)
    char* sess = agentxx_list_sessions(a, &log);
    XX_TEST_EXPECT_TRUE(sess != nullptr);
    if (sess != nullptr) {
        try {
            auto j = neograph::json::parse(sess);
            XX_TEST_EXPECT_TRUE(j.contains("sessions") && j["sessions"].is_array());
        } catch (...) {
            g_ffi_failed++;
            TEST_FAIL << "sessions payload not JSON: " << sess << std::endl;
        }
        agentxx_free(sess);
    }

    // 日志 drain (至少返回合法 JSON 数组)
    char* logs = agentxx_drain_logs(a, &log);
    XX_TEST_EXPECT_TRUE(logs != nullptr);
    if (logs != nullptr) {
        try {
            auto j = neograph::json::parse(logs);
            XX_TEST_EXPECT_TRUE(j.is_array());
        } catch (...) {
            g_ffi_failed++;
            TEST_FAIL << "drain_logs not JSON array: " << logs << std::endl;
        }
        agentxx_free(logs);
    }

    // 停止 → 销毁 (幂等 stop)
    XX_TEST_EXPECT_EQ(agentxx_stop(a, &log), AGENTXX_OK);
    XX_TEST_EXPECT_EQ(agentxx_stop(a, &log), AGENTXX_OK);
    XX_TEST_EXPECT_EQ(agentxx_destroy(a, &log), AGENTXX_OK);

    // 停止后: 状态错误
    XX_TEST_EXPECT_EQ(agentxx_send_input(nullptr, "x", &log), AGENTXX_ERR_INVALID);
    agentxx_free(log);
    log = nullptr;
    mock.stop();
}

/// 4) HIL 权限中断: mock 首轮返回工具调用 → 权限询问 → 后台应答 → 轮次恢复
void testHilInterrupt() {
    FfiMockLLM mock;
    mock.firstIsToolCall = true;
    uint16_t port        = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    FfiEventRecorder rec;
    AgentxxCallbacks cb;
    std::memset(&cb, 0, sizeof(cb));
    cb.on_event  = FfiEventRecorder::onEvent;
    cb.user_data = &rec;

    // 权限模式 all_ask: 全部路径读写均询问 (确保 /etc/hostname 触发权限中断)
    // 插件装配: mock 调用的 agentxx_filesystem_read 已从 lib 内置工具迁移为
    // filesystem 插件 (2026-08 内置插件化), FFI agent 不会自动加载插件,
    // 须显式配置; 路径按测试可执行同目录 plugins/ 推导
    std::string pluginDir  = findPluginDir("agentxx_filesystem");
    std::string configJson = R"({"permissionMode": "all_ask"})";
    if (!pluginDir.empty()) {
        // 经 json 库注入 plugins 段 (手拼 raw string 会踩 ")" 定界提前终止坑)
        try {
            auto cfg       = neograph::json::parse(configJson);
            cfg["plugins"] = neograph::json::array({neograph::json{{"path", pluginDir}}});
            configJson     = cfg.dump();
        } catch (...) {
            TEST_FAIL << "inject plugins config failed" << std::endl;
            g_ffi_failed++;
            mock.stop();
            return;
        }
    }

    char*         log = nullptr;
    AgentxxAgent* a   = agentxx_create(configJson.c_str(), mock.modelJson().c_str(), &cb, &log);
    if (a == nullptr) {
        TEST_FAIL << "create failed: " << (log ? log : "?") << std::endl;
        agentxx_free(log);
        mock.stop();
        g_ffi_failed++;
        return;
    }
    XX_TEST_EXPECT_EQ(agentxx_start(a, &log), AGENTXX_OK);
    XX_TEST_EXPECT_TRUE(rec.wait(AGENTXX_EVT_READY, 20000));

    XX_TEST_EXPECT_EQ(agentxx_send_input(a, "read /etc/hostname", &log), AGENTXX_OK);

    // 等待权限中断请求
    XX_TEST_EXPECT_TRUE(rec.wait(AGENTXX_EVT_INTERRUPT_REQ, 30000));
    XX_TEST_EXPECT_FALSE(rec.has(AGENTXX_EVT_INTERRUPT_EXPIRED));

    // 校验中断 payload 并取 interruptId
    int64_t interruptId = -1;
    {
        auto payload = rec.first(AGENTXX_EVT_INTERRUPT_REQ);
        try {
            auto j      = neograph::json::parse(payload);
            interruptId = j.value("interruptId", int64_t{-1});
            XX_TEST_EXPECT_TRUE(interruptId > 0);
            // argJson 应含 permission 上下文
            std::string argJson = j.value("argJson", std::string{});
            XX_TEST_EXPECT_TRUE(argJson.find("permission") != std::string::npos);
        } catch (...) {
            g_ffi_failed++;
            TEST_FAIL << "interrupt payload not JSON: " << payload << std::endl;
        }
    }

    // 在独立线程延迟应答 allow (模拟宿主 UI 异步操作)
    int         respondRc = -999;
    std::thread responder([a, interruptId, &respondRc]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        char* lg = nullptr;
        // bool 类型输入: "true" = 允许
        respondRc = agentxx_interrupt_respond(a, interruptId, R"(["true"])", &lg);
        agentxx_free(lg);
    });

    // 轮次应恢复并结束 (工具执行 → 第二次 LLM → 文本)
    XX_TEST_EXPECT_TRUE(rec.wait(AGENTXX_EVT_TURN_END, 30000));
    XX_TEST_EXPECT_TRUE(respondRc == 0);
    responder.join();
    {
        auto turn = rec.first(AGENTXX_EVT_TURN_END);
        try {
            auto j = neograph::json::parse(turn);
            XX_TEST_EXPECT_TRUE(j.value("hasError", true) == false);
        } catch (...) {
            g_ffi_failed++;
            TEST_FAIL << "TURN_END payload not JSON: " << turn << std::endl;
        }
    }

    // 已应答后, 再次应答同一 id 应报无效
    char* lg = nullptr;
    XX_TEST_EXPECT_EQ(
        agentxx_interrupt_respond(a, interruptId, R"(["true"])", &lg),
        AGENTXX_ERR_INTERRUPT
    );
    agentxx_free(lg);

    XX_TEST_EXPECT_EQ(agentxx_stop(a, &log), AGENTXX_OK);
    XX_TEST_EXPECT_EQ(agentxx_destroy(a, &log), AGENTXX_OK);
    mock.stop();
}

/// 5) 取消: 慢 LLM 响应中取消 → TURN_END interrupted
void testCancel() {
    FfiMockLLM mock;
    mock.slowMs   = 1500;
    uint16_t port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    FfiEventRecorder rec;
    AgentxxCallbacks cb;
    std::memset(&cb, 0, sizeof(cb));
    cb.on_event  = FfiEventRecorder::onEvent;
    cb.user_data = &rec;

    char*         log = nullptr;
    AgentxxAgent* a   = agentxx_create(nullptr, mock.modelJson().c_str(), &cb, &log);
    if (a == nullptr) {
        TEST_FAIL << "create failed" << std::endl;
        agentxx_free(log);
        mock.stop();
        g_ffi_failed++;
        return;
    }
    XX_TEST_EXPECT_EQ(agentxx_start(a, &log), AGENTXX_OK);
    XX_TEST_EXPECT_TRUE(rec.wait(AGENTXX_EVT_READY, 20000));

    XX_TEST_EXPECT_EQ(agentxx_send_input(a, "run slow", &log), AGENTXX_OK);
    // 模拟用户提前取消
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    XX_TEST_EXPECT_EQ(agentxx_cancel(a, &log), AGENTXX_OK);

    XX_TEST_EXPECT_TRUE(rec.wait(AGENTXX_EVT_TURN_END, 20000));
    {
        auto turn = rec.first(AGENTXX_EVT_TURN_END);
        try {
            auto j = neograph::json::parse(turn);
            // 用户取消语义: has_error=true + errorMessage="Cancelled by user"
            // (见 BaseAgent::runTurnAsync 取消分支)
            XX_TEST_EXPECT_TRUE(j.value("hasError", false) == true);
            XX_TEST_EXPECT_TRUE(
                j.value("errorMessage", std::string{}).find("Cancelled") != std::string::npos
            );
        } catch (...) {
            g_ffi_failed++;
            TEST_FAIL << "TURN_END payload not JSON: " << turn << std::endl;
        }
    }

    XX_TEST_EXPECT_EQ(agentxx_stop(a, &log), AGENTXX_OK);
    XX_TEST_EXPECT_EQ(agentxx_destroy(a, &log), AGENTXX_OK);
    mock.stop();
}

/// 6) 多 Runtime 并发独立运行测试 (方案 A 独立线程模型)
void testMultipleRuntimesConcurrent() {
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    constexpr size_t kRuntimeCount = 3;

    struct RuntimeSlot {
        FfiEventRecorder rec;
        AgentxxCallbacks cb{};
        AgentxxAgent*    agent = nullptr;
    };

    std::vector<RuntimeSlot> slots(kRuntimeCount);

    for (size_t i = 0; i < kRuntimeCount; ++i) {
        slots[i].cb.on_event  = FfiEventRecorder::onEvent;
        slots[i].cb.user_data = &slots[i].rec;
        char* log             = nullptr;
        slots[i].agent = agentxx_create(nullptr, mock.modelJson().c_str(), &slots[i].cb, &log);
        XX_TEST_EXPECT_TRUE(slots[i].agent != nullptr);
        if (log != nullptr) {
            agentxx_free(log);
        }
    }

    // 并发启动
    for (size_t i = 0; i < kRuntimeCount; ++i) {
        char* log = nullptr;
        XX_TEST_EXPECT_EQ(agentxx_start(slots[i].agent, &log), AGENTXX_OK);
        if (log != nullptr) {
            agentxx_free(log);
        }
    }

    // 等待所有 runtime 就绪
    for (size_t i = 0; i < kRuntimeCount; ++i) {
        XX_TEST_EXPECT_TRUE(slots[i].rec.wait(AGENTXX_EVT_READY, 20000));
    }

    // 并发发送输入
    for (size_t i = 0; i < kRuntimeCount; ++i) {
        char* log = nullptr;
        XX_TEST_EXPECT_EQ(agentxx_send_input(slots[i].agent, "Hello from slot", &log), AGENTXX_OK);
        if (log != nullptr) {
            agentxx_free(log);
        }
    }

    // 等待各 slot 独立收到 TURN_END
    for (size_t i = 0; i < kRuntimeCount; ++i) {
        XX_TEST_EXPECT_TRUE(slots[i].rec.wait(AGENTXX_EVT_TURN_END, 20000));
    }

    // 并发停止与销毁
    for (size_t i = 0; i < kRuntimeCount; ++i) {
        char* log = nullptr;
        XX_TEST_EXPECT_EQ(agentxx_stop(slots[i].agent, &log), AGENTXX_OK);
        if (log != nullptr) {
            agentxx_free(log);
            log = nullptr;
        }
        XX_TEST_EXPECT_EQ(agentxx_destroy(slots[i].agent, &log), AGENTXX_OK);
        if (log != nullptr) {
            agentxx_free(log);
        }
    }

    mock.stop();
}

} // namespace

namespace agentxx {
namespace test {

agentxx::test::TestResult testFfiCApi() {
    testVersionAndMemory();
    testCreateInvalid();
    testLifecycleAndConversation();
    testHilInterrupt();
    testCancel();
    testMultipleRuntimesConcurrent();
    return TestResult(g_ffi_passed, g_ffi_failed);
}

} // namespace test
} // namespace agentxx