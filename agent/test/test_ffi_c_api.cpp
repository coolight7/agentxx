//
// 测试方式: 直接链接 libagentxx (静态副本也含 ffi_api 实现, 符号一致) 并以
// 纯 C 方式调用 agentxx_* 导出接口; mock OpenAI 兼容 HTTP 服务器模拟 LLM。
//
// 覆盖 (v4 提交 + 回执模型):
//  1. 版本/内存/错误串 (含新增的 canceled / rejected)
//  2. create 错误路径 (非法 JSON/缺模型/回调结构体字段) → NULL + AgentxxString log
//  3. 生命周期 + 会话对话: create→start→EVT_READY→input.send 回执→流式 DELTA→TURN_END
//  4. 查询类 op: model.list / context.get / context.view_page / session.list /
//     components.get / permission.get_state
//  5. call 的同步校验: 未知 op / 缺 op / args 非对象 / timeoutMs 非法 → 错误码, 无回执
//  6. 并发: 同一句柄 20 个调用同时在飞, 各自拿到自己的回执
//  7. 取消: 延迟应答的调用被 call_cancel 取消 → 恰好一条 canceled 回执; 迟到应答被丢弃;
//     再次取消已完成 reqId → OK (幂等)
//  8. 顺序: 先等 model.add / model.select 的回执, 再查 model.list 能看到新状态
//  9. 事件回调内调 call / call_cancel (旧实现里等满 10s 的死锁场景)
// 10. 生命周期: stop 受理即返回、EVT_STOPPED 是最后一个事件、运行中 destroy → ERR_STATE、
//     重复 stop 只发一次 EVT_STOPPED、收尾时未完成的调用以 canceled 收尾
// 11. 本地 op: language.get/set、delta.set_batch (不进 wire, 立即回执)
// 12. hosttool.register/unregister/respond 的回执数据
// 13. 模型管理: model.add → 落盘 → model.remove
// 14. 能力清单: ops / features
// 15. fs.list_dir / plugin.data_up (含 topic 校验失败)
// 16. HIL 中断: interrupt.respond op
#include "agentxx-test/test_ffi_c_api.h"

#include "agentxx/ffi_api.h"
#include "agentxx/version.h"
#include "utilxx/http_server.h"
#include "utilxx_base/json.h"

#if XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

static std::string findPluginDir(const char* pluginName) {
    namespace fs = std::filesystem;
    std::error_code       ec;
    std::vector<fs::path> candidates;
    if (auto exeDir = agentxx::test::executableDir()) {
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
    mutable std::mutex                                       m;
    std::vector<std::pair<AgentxxFFIEventType, std::string>> events;
    /// reqId → EVT_RESULT 的 payload (每个 reqId 只应有一条)
    std::map<uint64_t, std::string> results;
    /// 同一 reqId 收到回执的次数 (校验"恰好一条")
    std::map<uint64_t, int> resultCount;
    /// 每个事件到达时回调 (锁外调用, 用于"回调内调 API"的用例)
    std::function<void(AgentxxFFIEventType, std::string_view)> onAnyEvent;

    static void AGENTXX_FFI_CALL onEvent(int32_t type, AgentxxString* payload, void* ud) {
        auto* self = static_cast<FfiEventRecorder*>(ud);
        if (self == nullptr) {
            return;
        }
        std::string s;
        if (payload != nullptr && payload->data != nullptr && payload->size > 0) {
            s.assign(payload->data, static_cast<size_t>(payload->size));
        }
        // payload 归回调所有: 结构体与正文同在一块堆内存上, 看完立刻 agentxx_ffi_free
        // (ASan 下同时验证没有泄漏/没有 UAF)
        if (payload != nullptr) {
            agentxx_ffi_free(payload);
        }
        const auto t = static_cast<AgentxxFFIEventType>(type);
        {
            std::lock_guard<std::mutex> lock(self->m);
            if (t == AGENTXX_FFI_EVT_RESULT) {
                try {
                    auto j      = utilxx_base::Json::parse(s);
                    auto reqId  = j.value("reqId", uint64_t{0});
                    self->results[reqId] = s;
                    ++self->resultCount[reqId];
                } catch (...) {
                }
            }
            self->events.emplace_back(t, s);
        }
        if (self->onAnyEvent) {
            self->onAnyEvent(t, s);
        }
    }

    bool has(AgentxxFFIEventType t) const {
        std::lock_guard<std::mutex> lock(m);
        for (const auto& [type, payload] : events) {
            if (type == t) {
                return true;
            }
        }
        return false;
    }

    size_t count(AgentxxFFIEventType t) const {
        std::lock_guard<std::mutex> lock(m);
        size_t                      n = 0;
        for (const auto& [type, payload] : events) {
            if (type == t) {
                ++n;
            }
        }
        return n;
    }

    /// 等待某种事件出现 (轮询, 最多 timeoutMs)
    bool wait(AgentxxFFIEventType t, int timeoutMs) {
        const auto deadline
            = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (has(t)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return has(t);
    }

    /// 返回首个匹配事件的 payload
    std::string first(AgentxxFFIEventType t) const {
        std::lock_guard<std::mutex> lock(m);
        for (const auto& [type, payload] : events) {
            if (type == t) {
                return payload;
            }
        }
        return {};
    }

    /// 最后一个事件的种类 (无事件时返回 -1)
    int lastEventType() const {
        std::lock_guard<std::mutex> lock(m);
        if (events.empty()) {
            return -1;
        }
        return static_cast<int>(events.back().first);
    }

    /// 出现过 kind=xxx 的 delta
    bool hasDeltaKind(const char* kind) const {
        std::lock_guard<std::mutex> lock(m);
        for (const auto& [type, payload] : events) {
            if (type != AGENTXX_FFI_EVT_DELTA) {
                continue;
            }
            try {
                auto j = utilxx_base::Json::parse(payload);
                if (j.value("kind", std::string{}) == kind) {
                    return true;
                }
            } catch (...) {
            }
        }
        return false;
    }

    /// 等某个 reqId 的回执; 返回 payload (超时给空串)
    std::string waitResult(uint64_t reqId, int timeoutMs) {
        const auto deadline
            = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(m);
                auto                        it = results.find(reqId);
                if (it != results.end()) {
                    return it->second;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::lock_guard<std::mutex> lock(m);
        auto                        it = results.find(reqId);
        return it == results.end() ? std::string{} : it->second;
    }

    int resultCountOf(uint64_t reqId) const {
        std::lock_guard<std::mutex> lock(m);
        auto                        it = resultCount.find(reqId);
        return it == resultCount.end() ? 0 : it->second;
    }
};

// ---------------------------------------------------------------------------
// 调用封装 (提交 + 等回执)
// ---------------------------------------------------------------------------

struct CallReply {
    /// 是否受理成功 (false = 同步被拒, 不会有回执)
    bool              submitted = false;
    uint64_t          reqId     = 0;
    bool              ok        = false;
    int               code      = 0;
    std::string       name;
    std::string       message;
    utilxx_base::Json result;
};

struct FfiTestClient {
    AgentxxFFIAgent*  a = nullptr;
    FfiEventRecorder  rec;
    std::string       lastLog;

    /// 提交一次调用; 返回 C API 的返回码, 并把 log 收进 lastLog
    int32_t submit(const std::string& requestJson, uint64_t& reqId) {
        reqId = 0;
        lastLog.clear();
        AgentxxString log{};
        auto          sv = agentxx_string_view(requestJson.data(), requestJson.size());
        const int32_t rc = agentxx_ffi_call(a, &sv, &reqId, &log);
        if (log.data != nullptr) {
            lastLog.assign(log.data, static_cast<size_t>(log.size));
        }
        agentxx_ffi_string_free(&log);
        return rc;
    }

    /// 提交并等回执 (op 名 + args; timeoutMs < 0 表示不传)
    CallReply call(
        const char*              op,
        const utilxx_base::Json& args        = {},
        int64_t                  timeoutMs   = -1,
        int                      waitTimeout = 15000
    ) {
        utilxx_base::Json req = utilxx_base::Json::object();
        req["op"]             = op;
        req["args"]           = args.is_null() ? utilxx_base::Json::object() : args;
        if (timeoutMs >= 0) {
            req["timeoutMs"] = timeoutMs;
        }
        CallReply reply;
        uint64_t  reqId = 0;
        const int rc    = submit(req.dump(), reqId);
        if (rc != AGENTXX_FFI_OK) {
            return reply;
        }
        reply.submitted = true;
        reply.reqId     = reqId;
        auto payload    = rec.waitResult(reqId, waitTimeout);
        if (payload.empty()) {
            return reply;
        }
        try {
            auto j       = utilxx_base::Json::parse(payload);
            reply.ok     = j.value("ok", false);
            if (reply.ok) {
                reply.result = j.value("result", utilxx_base::Json::object());
            } else if (j.contains("error") && j["error"].is_object()) {
                reply.code    = j["error"].value("code", 0);
                reply.name    = j["error"].value("name", std::string{});
                reply.message = j["error"].value("message", std::string{});
            }
        } catch (...) {
        }
        return reply;
    }

    bool ready(int timeoutMs = 20000) {
        return rec.wait(AGENTXX_FFI_EVT_READY, timeoutMs);
    }

    /// 等 EVT_STOPPED (destroy 的前置条件)
    bool waitStopped(int timeoutMs = 30000) {
        return rec.wait(AGENTXX_FFI_EVT_STOPPED, timeoutMs);
    }

    int32_t start() {
        AgentxxString log{};
        const int32_t rc = agentxx_ffi_start(a, &log);
        lastLog.clear();
        if (log.data != nullptr) {
            lastLog.assign(log.data, static_cast<size_t>(log.size));
        }
        agentxx_ffi_string_free(&log);
        return rc;
    }

    int32_t stop() {
        AgentxxString log{};
        const int32_t rc = agentxx_ffi_stop(a, &log);
        lastLog.clear();
        if (log.data != nullptr) {
            lastLog.assign(log.data, static_cast<size_t>(log.size));
        }
        agentxx_ffi_string_free(&log);
        return rc;
    }

    int32_t destroy() {
        AgentxxString log{};
        const int32_t rc = agentxx_ffi_destroy(a, &log);
        lastLog.clear();
        if (log.data != nullptr) {
            lastLog.assign(log.data, static_cast<size_t>(log.size));
        }
        agentxx_ffi_string_free(&log);
        if (rc == AGENTXX_FFI_OK) {
            a = nullptr;
        }
        return rc;
    }

    int32_t cancel(uint64_t reqId) {
        AgentxxString log{};
        const int32_t rc = agentxx_ffi_call_cancel(a, reqId, &log);
        lastLog.clear();
        if (log.data != nullptr) {
            lastLog.assign(log.data, static_cast<size_t>(log.size));
        }
        agentxx_ffi_string_free(&log);
        return rc;
    }

    std::string capabilities() {
        AgentxxString out{};
        AgentxxString log{};
        std::string   text;
        if (agentxx_ffi_get_capabilities(a, &out, &log) == AGENTXX_FFI_OK && out.data != nullptr) {
            text.assign(out.data, static_cast<size_t>(out.size));
        }
        agentxx_ffi_string_free(&out);
        agentxx_ffi_string_free(&log);
        return text;
    }

    std::string drainLogs() {
        AgentxxString out{};
        AgentxxString log{};
        std::string   text;
        if (agentxx_ffi_drain_logs(a, &out, &log) == AGENTXX_FFI_OK && out.data != nullptr) {
            text.assign(out.data, static_cast<size_t>(out.size));
        }
        agentxx_ffi_string_free(&out);
        agentxx_ffi_string_free(&log);
        return text;
    }
};

/// 创建句柄 (事件回调指向 client.rec); 失败时 lastLog 给原因
bool ffiCreate(
    FfiTestClient&   client,
    const char*      configJson,
    const std::string& modelJson,
    bool             headless = false
) {
    AgentxxFFICallbacks cb{};
    cb.struct_size = sizeof(cb);
    if (!headless) {
        cb.on_event  = FfiEventRecorder::onEvent;
        cb.user_data = &client.rec;
    }
    AgentxxString log{};
    auto          modelSv = agentxx_string_view(modelJson.data(), modelJson.size());
    auto          cfgSv   = configJson != nullptr ? agentxx_string_view_cstr(configJson)
                                                  : AgentxxStringView{};
    client.a              = agentxx_ffi_create(
        configJson != nullptr ? &cfgSv : nullptr,
        &modelSv,
        &cb,
        &log
    );
    client.lastLog.clear();
    if (log.data != nullptr) {
        client.lastLog.assign(log.data, static_cast<size_t>(log.size));
    }
    agentxx_ffi_string_free(&log);
    return client.a != nullptr;
}

// ---------------------------------------------------------------------------
// mock OpenAI 兼容 LLM 服务器 (流式 /chat/completions)
// ---------------------------------------------------------------------------

struct FfiMockLLM {
    std::unique_ptr<utilxx::HttpServer> server;
    std::thread                         thread;
    std::atomic<int>                    requestCount{0};
    /// 首个请求返回工具调用 (模拟需要权限/输入的工具选择)
    bool firstIsToolCall = false;
    /// >0 时响应前延迟 (取消测试用)
    int slowMs = 0;

    /// 最近一次 /chat/completions 的请求体 (供测试断言上下文与附件组装)
    std::mutex  bodyMutex;
    std::string lastRequestBody;

    std::string capturedBody() {
        std::lock_guard<std::mutex> lock(bodyMutex);
        return lastRequestBody;
    }

    ~FfiMockLLM() {
        stop();
    }

    /// 文本流式 SSE chunks (openai chat completion 流式格式)
    static std::string textSse(std::string_view content) {
        const std::string id = "chatcmpl-ffi-mock";
        return std::string("data: ") + utilxx_base::Json{
            {"id", id},
            {"object", "chat.completion.chunk"},
            {"model", "ffi-mock"},
            {"choices", utilxx_base::Json::array({
                utilxx_base::Json{{"index", 0}, {"delta", {{"role", "assistant"}, {"content", ""}}}},
            })},
        }.dump() + "\n\n"
        + "data: " + utilxx_base::Json{
            {"id", id},
            {"object", "chat.completion.chunk"},
            {"model", "ffi-mock"},
            {"choices", utilxx_base::Json::array({
                utilxx_base::Json{{"index", 0}, {"delta", {{"content", content}}}},
            })},
        }.dump() + "\n\n"
        + "data: " + utilxx_base::Json{
            {"id", id},
            {"object", "chat.completion.chunk"},
            {"model", "ffi-mock"},
            {"choices", utilxx_base::Json::array({
                utilxx_base::Json{{"index", 0}, {"delta", utilxx_base::Json::object()}, {"finish_reason", "stop"}},
            })},
        }.dump() + "\n\n"
        + "data: [DONE]\n\n";
    }

    /// 工具调用流式 SSE chunks
    static std::string toolCallSse(const char* toolName, std::string_view argsJson) {
        const std::string id = "chatcmpl-ffi-tool";
        return std::string("data: ") + utilxx_base::Json{
            {"id", id},
            {"object", "chat.completion.chunk"},
            {"model", "ffi-mock"},
            {"choices", utilxx_base::Json::array({
                utilxx_base::Json{
                    {"index", 0},
                    {"delta", {
                        {"role", "assistant"},
                        {"tool_calls", utilxx_base::Json::array({
                            utilxx_base::Json{
                                {"index", 0},
                                {"id", "call-ffi-1"},
                                {"type", "function"},
                                {"function", {{"name", toolName}, {"arguments", argsJson}}},
                            },
                        })},
                    }},
                },
            })},
        }.dump() + "\n\n"
        + "data: " + utilxx_base::Json{
            {"id", id},
            {"object", "chat.completion.chunk"},
            {"model", "ffi-mock"},
            {"choices", utilxx_base::Json::array({
                utilxx_base::Json{{"index", 0}, {"delta", utilxx_base::Json::object()}, {"finish_reason", "tool_calls"}},
            })},
        }.dump() + "\n\n"
        + "data: [DONE]\n\n";
    }

    bool start(uint16_t& outPort) {
        server = std::make_unique<utilxx::HttpServer>(
            utilxx::HttpServer::Config{.address = "127.0.0.1", .port = 0, .ioThreads = 1}
        );
        auto handler = std::make_shared<utilxx::HttpServer::Handler>(
            [this](
                utilxx::HttpServer::Request&  req,
                utilxx::HttpServer::Response& resp,
                std::string_view
            ) -> asio::awaitable<void> {
                {
                    // 记录请求体: 附件/上下文组装断言的数据来源
                    std::lock_guard<std::mutex> lock(bodyMutex);
                    lastRequestBody = std::string{req.body()};
                }
                const int n = requestCount.fetch_add(1);
                if (slowMs > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(slowMs));
                }
                resp.result(boost::beast::http::status::ok);
                resp.set(boost::beast::http::field::content_type, "text/event-stream");
                resp.set(boost::beast::http::field::cache_control, "no-cache");
                resp.keep_alive(false);
                if (n == 0 && firstIsToolCall) {
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
    /// - `multimodal` 为 true 时声明多模态输入能力 (对齐 yaml 的
    ///   image_input/audio_input/video_input), 用于校验能力经 model.list 的
    ///   capabilities 下发
    std::string modelJson(bool multimodal = false) const {
        auto j = utilxx_base::Json{
            {"name",      "ffi-mock"                                          },
            {"type",      "openai"                                            },
            {"baseUrl",   "http://127.0.0.1:" + std::to_string(server->port())},
            {"apiKey",    "EMPTY"                                             },
            {"modelName", "ffi-mock"                                          },
        };
        if (multimodal) {
            j["imageInput"] = true;
            j["videoInput"] = true;
        }
        return j.dump();
    }
};

// ---------------------------------------------------------------------------
// 测试用例
// ---------------------------------------------------------------------------

/// 1) 版本 / 内存 / 错误串
void testVersionAndMemory() {
    XX_TEST_EXPECT_GE(agentxx_ffi_api_version(), AGENTXX_FFI_API_VERSION);
    XX_TEST_EXPECT_EQ(agentxx_ffi_api_version(), 4);

    AgentxxStringView libVer{};
    XX_TEST_EXPECT_EQ(agentxx_ffi_library_version(&libVer), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(libVer.data != nullptr && libVer.size > 0);
    XX_TEST_EXPECT_EQ(std::string_view(libVer.data, libVer.size), agentxx::kVersion);

    AgentxxStringView errOk{};
    XX_TEST_EXPECT_EQ(agentxx_ffi_strerror(AGENTXX_FFI_OK, &errOk), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(std::string_view(errOk.data, errOk.size) == "success");

    AgentxxStringView errInv{};
    XX_TEST_EXPECT_EQ(agentxx_ffi_strerror(AGENTXX_FFI_ERR_INVALID, &errInv), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(std::string_view(errInv.data, errInv.size) == "invalid argument");

    // v4 新增的两个错误码
    AgentxxStringView errCanceled{};
    XX_TEST_EXPECT_EQ(
        agentxx_ffi_strerror(AGENTXX_FFI_ERR_CANCELED, &errCanceled),
        AGENTXX_FFI_OK
    );
    XX_TEST_EXPECT_TRUE(
        std::string_view(errCanceled.data, errCanceled.size) == "call canceled"
    );
    AgentxxStringView errRejected{};
    XX_TEST_EXPECT_EQ(
        agentxx_ffi_strerror(AGENTXX_FFI_ERR_REJECTED, &errRejected),
        AGENTXX_FFI_OK
    );
    XX_TEST_EXPECT_TRUE(
        std::string_view(errRejected.data, errRejected.size) == "rejected by server"
    );

    // strdup_n / free 往返
    const char*   text   = "hello ffi";
    auto          textSv = agentxx_string_view_cstr(text);
    AgentxxString dup{};
    XX_TEST_EXPECT_EQ(agentxx_ffi_strdup_n(&textSv, &dup), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(dup.data != nullptr && dup.size == std::strlen(text));
    XX_TEST_EXPECT_TRUE(std::strcmp(dup.data, text) == 0);
    agentxx_ffi_string_free(&dup);

    // 带 NUL 的任意字节段
    const char    bytes[] = {'a', 'b', '\0', 'c', 'd'};
    auto          bytesSv = agentxx_string_view(bytes, sizeof(bytes));
    AgentxxString p{};
    XX_TEST_EXPECT_EQ(agentxx_ffi_strdup_n(&bytesSv, &p), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(p.data != nullptr && p.size == sizeof(bytes));
    XX_TEST_EXPECT_TRUE(std::memcmp(p.data, bytes, sizeof(bytes)) == 0);
    agentxx_ffi_string_free(&p);

    // free(NULL) 与 free 空串都安全
    agentxx_ffi_free(nullptr);
    AgentxxString empty{};
    agentxx_ffi_string_free(&empty);
    XX_TEST_EXPECT_TRUE(empty.data == nullptr);
}

/// 2) create 错误路径 → NULL + AgentxxString log
void testCreateInvalid() {
    AgentxxString log{};

    // 无模型配置
    AgentxxFFIAgent* a = agentxx_ffi_create(nullptr, nullptr, nullptr, &log);
    XX_TEST_EXPECT_TRUE(a == nullptr);
    XX_TEST_EXPECT_TRUE(log.data != nullptr && std::strstr(log.data, "模型配置") != nullptr);
    agentxx_ffi_string_free(&log);

    // config_json 非法 JSON
    auto badConfigSv  = agentxx_string_view_cstr("{not json");
    auto emptyModelSv = agentxx_string_view_cstr("{}");
    a                 = agentxx_ffi_create(&badConfigSv, &emptyModelSv, nullptr, &log);
    XX_TEST_EXPECT_TRUE(a == nullptr);
    XX_TEST_EXPECT_TRUE(log.data != nullptr && std::strstr(log.data, "非法 JSON") != nullptr);
    agentxx_ffi_string_free(&log);

    // model_json 非法 (无 baseUrl/apiKey → isValid() 失败)
    auto badModelSv = agentxx_string_view_cstr("{\"name\":\"x\"}");
    a               = agentxx_ffi_create(nullptr, &badModelSv, nullptr, &log);
    XX_TEST_EXPECT_TRUE(a == nullptr);
    XX_TEST_EXPECT_TRUE(log.data != nullptr && std::strstr(log.data, "模型配置非法") != nullptr);
    agentxx_ffi_string_free(&log);

    // 回调结构体: struct_size 不足 / reserved 非 0 都拒绝
    {
        AgentxxFFICallbacks cb{};
        cb.struct_size     = sizeof(cb) - 8;
        a                  = agentxx_ffi_create(nullptr, &badModelSv, &cb, &log);
        XX_TEST_EXPECT_TRUE(a == nullptr);
        XX_TEST_EXPECT_TRUE(log.data != nullptr && std::strstr(log.data, "struct_size") != nullptr);
        agentxx_ffi_string_free(&log);

        cb.struct_size = sizeof(cb);
        cb.reserved    = 1;
        a              = agentxx_ffi_create(nullptr, &badModelSv, &cb, &log);
        XX_TEST_EXPECT_TRUE(a == nullptr);
        XX_TEST_EXPECT_TRUE(log.data != nullptr && std::strstr(log.data, "reserved") != nullptr);
        agentxx_ffi_string_free(&log);
    }

    // NULL 句柄校验
    XX_TEST_EXPECT_EQ(agentxx_ffi_destroy(nullptr, &log), AGENTXX_FFI_ERR_INVALID);
    agentxx_ffi_string_free(&log);
    XX_TEST_EXPECT_EQ(agentxx_ffi_stop(nullptr, &log), AGENTXX_FFI_ERR_INVALID);
    agentxx_ffi_string_free(&log);

    uint64_t      reqId = 123;
    AgentxxString l2{};
    XX_TEST_EXPECT_EQ(
        agentxx_ffi_call(nullptr, nullptr, &reqId, &l2),
        AGENTXX_FFI_ERR_INVALID
    );
    XX_TEST_EXPECT_EQ(reqId, uint64_t{0});
    agentxx_ffi_string_free(&l2);
}

/// 3) 生命周期 + 会话对话 + 查询类 op
void testLifecycleAndConversation() {
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    FfiTestClient client;
    // 声明多模态能力: 校验能力清单经 model.list 的 capabilities 下发
    if (!ffiCreate(client, nullptr, mock.modelJson(/*multimodal=*/true))) {
        TEST_FAIL << "create failed: " << client.lastLog << std::endl;
        g_ffi_failed++;
        mock.stop();
        return;
    }

    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.ready());
    {
        auto ready = client.rec.first(AGENTXX_FFI_EVT_READY);
        XX_TEST_EXPECT_TRUE(ready.find("\"sessionId\"") != std::string::npos);
    }

    // 查询模型: 回执里带 currentModel / models / capabilities
    {
        auto reply = client.call("model.list");
        XX_TEST_EXPECT_TRUE(reply.submitted && reply.ok);
        XX_TEST_EXPECT_EQ(reply.reqId, uint64_t{1});
        XX_TEST_EXPECT_EQ(
            reply.result.value("currentModel", std::string{}),
            std::string{"ffi-mock"}
        );
        auto caps   = reply.result.value("capabilities", utilxx_base::Json::array());
        bool hasCap = false;
        for (const auto& c : caps) {
            if (c.value("name", std::string{}) != "ffi-mock") {
                continue;
            }
            hasCap = true;
            XX_TEST_EXPECT_TRUE(c.value("image_input", false));
            XX_TEST_EXPECT_TRUE(c.value("video_input", false));
            XX_TEST_EXPECT_FALSE(c.value("audio_input", true));
        }
        XX_TEST_EXPECT_TRUE(hasCap);
    }

    // 发送输入: 回执给出受理状态, 然后流式 delta → TURN_END
    {
        auto reply = client.call("input.send", utilxx_base::Json{{"text", "hello"}});
        XX_TEST_EXPECT_TRUE(reply.submitted && reply.ok);
        XX_TEST_EXPECT_EQ(
            reply.result.value("status", std::string{}),
            std::string{"started"}
        );
    }
    XX_TEST_EXPECT_TRUE(client.rec.wait(AGENTXX_FFI_EVT_TURN_END, 30000));
    XX_TEST_EXPECT_TRUE(client.rec.hasDeltaKind("text_token"));
    {
        auto turn = client.rec.first(AGENTXX_FFI_EVT_TURN_END);
        try {
            auto j = utilxx_base::Json::parse(turn);
            XX_TEST_EXPECT_TRUE(j.value("hasError", true) == false);
        } catch (...) {
            g_ffi_failed++;
            TEST_FAIL << "TURN_END payload not JSON: " << turn << std::endl;
        }
    }
    // 启动时自动拉过一次模型信息: 非回执 (reqId == 0) 的应答走广播事件
    XX_TEST_EXPECT_TRUE(client.rec.has(AGENTXX_FFI_EVT_MODEL_INFO));

    // 上下文: 整份 + 只取尾部
    {
        auto reply = client.call("context.get");
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_TRUE(reply.result.contains("messages"));
        XX_TEST_EXPECT_TRUE(reply.result["messages"].is_array());
        XX_TEST_EXPECT_TRUE(reply.result["messages"].size() > 0);

        auto tailReply = client.call("context.get", utilxx_base::Json{{"tail", 1}});
        XX_TEST_EXPECT_TRUE(tailReply.ok);
        XX_TEST_EXPECT_EQ(tailReply.result["messages"].size(), size_t{1});
    }

    // 展示历史分页 (首屏尾窗之外的更早消息)
    {
        auto reply = client.call(
            "context.view_page",
            utilxx_base::Json{{"beforeIndex", 0}, {"count", 10}}
        );
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_TRUE(reply.result.contains("startIndex"));
        XX_TEST_EXPECT_TRUE(reply.result.contains("totalCount"));
    }

    // 持久化会话列表 (未开启持久化 → 空数组, 但字段齐)
    {
        auto reply = client.call("session.list");
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_TRUE(reply.result.contains("sessions"));
        XX_TEST_EXPECT_TRUE(reply.result["sessions"].is_array());
    }

    // 启动组件清单
    {
        auto reply = client.call("components.get");
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_TRUE(reply.result.contains("notifications"));
    }

    // 权限状态
    {
        auto reply = client.call("permission.get_state");
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_TRUE(reply.result.contains("fullAuth"));
        XX_TEST_EXPECT_FALSE(reply.result.value("fullAuth", true));
    }

    // 日志 drain (至少返回合法 JSON 数组)
    {
        auto logs = client.drainLogs();
        XX_TEST_EXPECT_TRUE(!logs.empty());
        try {
            auto j = utilxx_base::Json::parse(logs);
            XX_TEST_EXPECT_TRUE(j.is_array());
        } catch (...) {
            g_ffi_failed++;
            TEST_FAIL << "drain_logs not JSON array: " << logs << std::endl;
        }
    }

    // 停止: 受理即返回, 收尾完成后 EVT_STOPPED 是最后一个事件
    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_FALSE(client.rec.has(AGENTXX_FFI_EVT_STOPPED));
    XX_TEST_EXPECT_TRUE(client.waitStopped(30000));
    XX_TEST_EXPECT_EQ(
        client.rec.lastEventType(),
        static_cast<int>(AGENTXX_FFI_EVT_STOPPED)
    );
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_OK);

    mock.stop();
}

/// 4) call 的同步校验: 一律不给回执
void testCallValidation() {
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    FfiTestClient client;
    XX_TEST_EXPECT_TRUE(ffiCreate(client, nullptr, mock.modelJson()));
    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.ready());

    uint64_t reqId = 0U;

    // 未知 op
    XX_TEST_EXPECT_EQ(client.submit(R"({"op":"no.such.op"})", reqId), AGENTXX_FFI_ERR_INVALID);
    XX_TEST_EXPECT_EQ(reqId, uint64_t{0});
    XX_TEST_EXPECT_TRUE(client.lastLog.find("no.such.op") != std::string::npos);

    // 缺 op
    XX_TEST_EXPECT_EQ(client.submit(R"({"args":{}})", reqId), AGENTXX_FFI_ERR_INVALID);
    XX_TEST_EXPECT_EQ(reqId, uint64_t{0});

    // op 不是字符串
    XX_TEST_EXPECT_EQ(client.submit(R"({"op":7})", reqId), AGENTXX_FFI_ERR_INVALID);
    XX_TEST_EXPECT_EQ(reqId, uint64_t{0});

    // 非法 JSON
    XX_TEST_EXPECT_EQ(client.submit("{not json", reqId), AGENTXX_FFI_ERR_JSON);
    XX_TEST_EXPECT_EQ(reqId, uint64_t{0});

    // args 不是对象
    XX_TEST_EXPECT_EQ(
        client.submit(R"({"op":"model.list","args":[]})", reqId),
        AGENTXX_FFI_ERR_INVALID
    );
    XX_TEST_EXPECT_EQ(reqId, uint64_t{0});

    // timeoutMs 非法
    XX_TEST_EXPECT_EQ(
        client.submit(R"({"op":"model.list","timeoutMs":"soon"})", reqId),
        AGENTXX_FFI_ERR_INVALID
    );
    XX_TEST_EXPECT_EQ(reqId, uint64_t{0});
    XX_TEST_EXPECT_EQ(
        client.submit(R"({"op":"model.list","timeoutMs":-1})", reqId),
        AGENTXX_FFI_ERR_INVALID
    );
    XX_TEST_EXPECT_EQ(reqId, uint64_t{0});

    // op 自己的参数校验 (缺 itemId / 空 name)
    XX_TEST_EXPECT_EQ(
        client.submit(R"({"op":"input.queue.remove","args":{}})", reqId),
        AGENTXX_FFI_ERR_INVALID
    );
    XX_TEST_EXPECT_EQ(reqId, uint64_t{0});
    XX_TEST_EXPECT_EQ(
        client.submit(R"({"op":"model.remove","args":{"name":""}})", reqId),
        AGENTXX_FFI_ERR_INVALID
    );
    XX_TEST_EXPECT_EQ(reqId, uint64_t{0});

    // 输入为空 / 附件超限: 同一条校验路径 (与 CLI/插件共用)
    XX_TEST_EXPECT_EQ(
        client.submit(R"({"op":"input.send","args":{}})", reqId),
        AGENTXX_FFI_ERR_INVALID
    );
    XX_TEST_EXPECT_EQ(reqId, uint64_t{0});

    // 句柄状态: 停止后调用直接 ERR_STATE (也没有回执)
    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.waitStopped(30000));
    XX_TEST_EXPECT_EQ(
        client.submit(R"({"op":"model.list"})", reqId),
        AGENTXX_FFI_ERR_STATE
    );
    XX_TEST_EXPECT_EQ(reqId, uint64_t{0});
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_OK);

    mock.stop();
}

/// 5) 并发: 20 个调用同时在飞, 各自拿到自己的回执
void testConcurrentCalls() {
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    FfiTestClient client;
    XX_TEST_EXPECT_TRUE(ffiCreate(client, nullptr, mock.modelJson()));
    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.ready());

    constexpr int          kCalls = 20;
    std::vector<uint64_t>  reqIds(kCalls, 0);
    std::vector<std::thread> threads;
    for (int i = 0; i < kCalls; ++i) {
        // 从多个线程同时提交: call 必须可重入 (任意线程可调)
        threads.emplace_back([&client, &reqIds, i] {
            reqIds[static_cast<size_t>(i)] = 0;
            AgentxxString log{};
            auto          sv = agentxx_string_view_cstr(R"({"op":"model.list"})");
            const int32_t rc = agentxx_ffi_call(client.a, &sv, &reqIds[static_cast<size_t>(i)], &log);
            agentxx_ffi_string_free(&log);
            XX_TEST_EXPECT_EQ(rc, AGENTXX_FFI_OK);
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    // 每个 reqId 各不相同, 且都收到自己的回执
    std::vector<uint64_t> sorted = reqIds;
    std::sort(sorted.begin(), sorted.end());
    XX_TEST_EXPECT_TRUE(std::unique(sorted.begin(), sorted.end()) == sorted.end());
    for (uint64_t id : reqIds) {
        XX_TEST_EXPECT_TRUE(id != 0);
        auto payload = client.rec.waitResult(id, 15000);
        XX_TEST_EXPECT_TRUE(!payload.empty());
        XX_TEST_EXPECT_EQ(client.rec.resultCountOf(id), 1);
        try {
            auto j = utilxx_base::Json::parse(payload);
            XX_TEST_EXPECT_EQ(j.value("reqId", uint64_t{0}), id);
            XX_TEST_EXPECT_EQ(j.value("op", std::string{}), std::string{"model.list"});
            XX_TEST_EXPECT_TRUE(j.value("ok", false));
        } catch (...) {
            g_ffi_failed++;
        }
    }

    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.waitStopped(30000));
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_OK);
    mock.stop();
}

/// 6) 取消: 延迟应答的调用被取消 → 恰好一条 canceled 回执, 迟到应答被丢弃
///
/// `input.send` 的 `collect` 投递模式会进 400ms 静默合并窗口, 受理回执在窗口
/// 结束后才发 —— 这给了"回执尚未到达时取消"的确定窗口。
void testCancelSemantics() {
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    FfiTestClient client;
    XX_TEST_EXPECT_TRUE(ffiCreate(client, nullptr, mock.modelJson()));
    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.ready());

    uint64_t reqId = 0;
    XX_TEST_EXPECT_EQ(
        client.submit(
            R"({"op":"input.send","args":{"text":"collected","delivery":"collect"}})",
            reqId
        ),
        AGENTXX_FFI_OK
    );
    XX_TEST_EXPECT_TRUE(reqId != 0);
    // 立刻取消 (回执还在合并窗口里)
    XX_TEST_EXPECT_EQ(client.cancel(reqId), AGENTXX_FFI_OK);

    auto payload = client.rec.waitResult(reqId, 10000);
    XX_TEST_EXPECT_TRUE(!payload.empty());
    try {
        auto j    = utilxx_base::Json::parse(payload);
        bool ok   = j.value("ok", true);
        XX_TEST_EXPECT_FALSE(ok);
        auto err  = j.value("error", utilxx_base::Json::object());
        XX_TEST_EXPECT_EQ(err.value("code", 0), static_cast<int>(AGENTXX_FFI_ERR_CANCELED));
        XX_TEST_EXPECT_EQ(err.value("name", std::string{}), std::string{"canceled"});
    } catch (...) {
        g_ffi_failed++;
        TEST_FAIL << "canceled payload not JSON: " << payload << std::endl;
    }

    // 合并窗口结束后服务端仍会回 input_ack (迟到应答): 不得产生第二条回执
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    XX_TEST_EXPECT_EQ(client.rec.resultCountOf(reqId), 1);

    // 幂等: 再次取消已完成的调用 → OK + log 说明
    XX_TEST_EXPECT_EQ(client.cancel(reqId), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.lastLog.find("already completed") != std::string::npos);

    // req_id == 0 非法
    XX_TEST_EXPECT_EQ(client.cancel(0), AGENTXX_FFI_ERR_INVALID);

    // 超时: 给 1ms 的 timeoutMs, 回执应是 timeout (合并窗口 400ms 远大于它)
    uint64_t timedReq = 0;
    XX_TEST_EXPECT_EQ(
        client.submit(
            R"({"op":"input.send","args":{"text":"timeout case","delivery":"collect"},"timeoutMs":1})",
            timedReq
        ),
        AGENTXX_FFI_OK
    );
    auto timedPayload = client.rec.waitResult(timedReq, 10000);
    XX_TEST_EXPECT_TRUE(!timedPayload.empty());
    try {
        auto j      = utilxx_base::Json::parse(timedPayload);
        auto err    = j.value("error", utilxx_base::Json::object());
        XX_TEST_EXPECT_EQ(err.value("code", 0), static_cast<int>(AGENTXX_FFI_ERR_TIMEOUT));
        XX_TEST_EXPECT_EQ(err.value("name", std::string{}), std::string{"timeout"});
    } catch (...) {
        g_ffi_failed++;
    }

    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.waitStopped(30000));
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_OK);
    mock.stop();
}

/// 7) 顺序: 先等前一步回执, 再提交下一步 (前一步一定已生效)
void testOrdering() {
    namespace fs = std::filesystem;
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    const auto tmpDataDir
        = fs::temp_directory_path()
          / ("ffi_order_"
             + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(tmpDataDir, ec);
    fs::create_directories(tmpDataDir, ec);
    const std::string cfgJson = utilxx_base::Json{
        {"dataDir", tmpDataDir.generic_string()}
    }.dump();

    FfiTestClient client;
    XX_TEST_EXPECT_TRUE(ffiCreate(client, cfgJson.c_str(), mock.modelJson()));
    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.ready());

    // 1. 添加模型 → 回执 ok
    const std::string newModel = fmt::format(
        R"({{"name":"ordered-model","type":"openai","baseUrl":"http://127.0.0.1:{}/v1","apiKey":"EMPTY"}})",
        port
    );
    {
        auto args  = utilxx_base::Json::parse(newModel);
        auto reply = client.call("model.add", args);
        XX_TEST_EXPECT_TRUE(reply.ok);
    }
    // 2. 切到新模型 → 回执 data 给出生效后的模型名
    {
        auto reply = client.call("model.select", utilxx_base::Json{{"name", "ordered-model"}});
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_EQ(
            reply.result.value("model", std::string{}),
            std::string{"ordered-model"}
        );
    }
    // 3. 现在查一定看到新模型是当前模型 (前两步都等到了回执)
    {
        auto reply = client.call("model.list");
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_EQ(
            reply.result.value("currentModel", std::string{}),
            std::string{"ordered-model"}
        );
        auto models  = reply.result.value("models", utilxx_base::Json::array());
        bool present = false;
        for (const auto& m : models) {
            if (m.is_string() && m.get<std::string>() == "ordered-model") {
                present = true;
            }
        }
        XX_TEST_EXPECT_TRUE(present);
    }
    // 4. 选一个不存在的模型 → ok:false + model_not_found (以前是静默忽略)
    {
        auto reply = client.call("model.select", utilxx_base::Json{{"name", "no-such-model"}});
        XX_TEST_EXPECT_FALSE(reply.ok);
        XX_TEST_EXPECT_EQ(reply.name, std::string{"model_not_found"});
        XX_TEST_EXPECT_EQ(reply.code, static_cast<int>(AGENTXX_FFI_ERR_REJECTED));
    }
    // 5. 删除当前使用的模型 → 被拒 (TypedResponse 的 ok:false)
    {
        auto reply = client.call("model.remove", utilxx_base::Json{{"name", "ordered-model"}});
        XX_TEST_EXPECT_FALSE(reply.ok);
    }
    // 6. 切回主模型后可以删掉新模型
    {
        XX_TEST_EXPECT_TRUE(
            client.call("model.select", utilxx_base::Json{{"name", "ffi-mock"}}).ok
        );
        XX_TEST_EXPECT_TRUE(
            client.call("model.remove", utilxx_base::Json{{"name", "ordered-model"}}).ok
        );
    }
    // 7. 重名添加 (已删除则不会命中) —— 用主模型名验证 bad_config 路径
    {
        auto args  = utilxx_base::Json::parse(mock.modelJson());
        auto reply = client.call("model.add", args);
        XX_TEST_EXPECT_FALSE(reply.ok);
        XX_TEST_EXPECT_EQ(reply.name, std::string{"bad_config"});
        XX_TEST_EXPECT_EQ(reply.code, static_cast<int>(AGENTXX_FFI_ERR_CONFIG));
    }

    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.waitStopped(30000));
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_OK);
    mock.stop();

    std::error_code cleanupEc;
    fs::remove_all(tmpDataDir, cleanupEc);
}

/// 8) 事件回调内调 call / call_cancel (旧实现里会等满 10s 的死锁场景)
void testCallFromCallback() {
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    FfiTestClient client;
    XX_TEST_EXPECT_TRUE(ffiCreate(client, nullptr, mock.modelJson()));

    std::atomic<int>  innerRc{-999};
    std::atomic<int>  cancelRc{-999};
    std::atomic<bool> insideCallback{false};
    const auto        t0 = std::chrono::steady_clock::now();
    client.rec.onAnyEvent = [&client, &innerRc, &cancelRc, &insideCallback](
                                AgentxxFFIEventType t,
                                std::string_view
                            ) {
        if (t != AGENTXX_FFI_EVT_READY || insideCallback.exchange(true)) {
            return;
        }
        // 直接在事件回调线程上提交调用 + 取消: 两者都必须立即返回
        AgentxxString log{};
        uint64_t      reqId = 0;
        auto          sv    = agentxx_string_view_cstr(R"({"op":"model.list"})");
        innerRc             = agentxx_ffi_call(client.a, &sv, &reqId, &log);
        agentxx_ffi_string_free(&log);
        // 取消刚提交的那条 (可能已完成, 两种情况都返回 OK)
        cancelRc = agentxx_ffi_call_cancel(client.a, reqId, &log);
        agentxx_ffi_string_free(&log);
    };

    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.ready(20000));
    XX_TEST_EXPECT_EQ(innerRc.load(), static_cast<int>(AGENTXX_FFI_OK));
    XX_TEST_EXPECT_EQ(cancelRc.load(), static_cast<int>(AGENTXX_FFI_OK));
    // 回调内的调用不该把回调卡住: 整个启动远小于旧的 10s 超时
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0
    )
                               .count();
    XX_TEST_EXPECT_TRUE(elapsedMs < 10000);
    client.rec.onAnyEvent = nullptr;

    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.waitStopped(30000));
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_OK);
    mock.stop();
}

/// 9) 生命周期: destroy 的状态约束、重复 stop 只发一次 EVT_STOPPED、
///    收尾时未完成的调用以 canceled 收尾
void testLifecycleAsync() {
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    FfiTestClient client;
    XX_TEST_EXPECT_TRUE(ffiCreate(client, nullptr, mock.modelJson()));

    // ① 未 start 就 stop: Created 状态直接收尾, 也会发一条 EVT_STOPPED
    //    (SDK 因此可以无脑 "stop → 等 EVT_STOPPED → destroy")
    {
        FfiTestClient created;
        XX_TEST_EXPECT_TRUE(ffiCreate(created, nullptr, mock.modelJson()));
        XX_TEST_EXPECT_EQ(created.stop(), AGENTXX_FFI_OK);
        XX_TEST_EXPECT_TRUE(created.waitStopped(5000));
        XX_TEST_EXPECT_EQ(
            created.rec.count(AGENTXX_FFI_EVT_STOPPED),
            size_t{1}
        );
        XX_TEST_EXPECT_EQ(created.destroy(), AGENTXX_FFI_OK);
    }

    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.ready());

    // ② 运行中 destroy → ERR_STATE (先 stop 并等 EVT_STOPPED)
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_ERR_STATE);
    XX_TEST_EXPECT_TRUE(client.lastLog.find("EVT_STOPPED") != std::string::npos);

    // ③ 收尾时还有未完成的调用: 提交一条延迟应答的调用, 然后立刻 stop
    uint64_t pending = 0;
    XX_TEST_EXPECT_EQ(
        client.submit(
            R"({"op":"input.send","args":{"text":"pending at stop","delivery":"collect"}})",
            pending
        ),
        AGENTXX_FFI_OK
    );
    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    // 重复 stop 幂等: 不再发事件
    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.waitStopped(30000));
    XX_TEST_EXPECT_EQ(client.rec.count(AGENTXX_FFI_EVT_STOPPED), size_t{1});
    XX_TEST_EXPECT_EQ(
        client.rec.lastEventType(),
        static_cast<int>(AGENTXX_FFI_EVT_STOPPED)
    );

    // 未完成的调用以 canceled 收尾, 且排在 EVT_STOPPED 之前 (恰好一条)
    XX_TEST_EXPECT_EQ(client.rec.resultCountOf(pending), 1);
    {
        auto payload = client.rec.waitResult(pending, 1000);
        try {
            auto j   = utilxx_base::Json::parse(payload);
            auto err = j.value("error", utilxx_base::Json::object());
            XX_TEST_EXPECT_EQ(
                err.value("code", 0),
                static_cast<int>(AGENTXX_FFI_ERR_CANCELED)
            );
        } catch (...) {
            g_ffi_failed++;
        }
    }
    // stop 期间的 call_cancel 仍可用 (幂等)
    XX_TEST_EXPECT_EQ(client.cancel(pending), AGENTXX_FFI_OK);

    // ④ 已停止: destroy 成功; 之后 stop 仍返回 OK (幂等)
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_OK);

    mock.stop();
}

/// 10) 本地 op: language.get/set、delta.set_batch (不进 wire, 立即回执)
void testLocalOps() {
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    FfiTestClient client;
    XX_TEST_EXPECT_TRUE(ffiCreate(client, R"({"language":"en"})", mock.modelJson()));
    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.ready());

    {
        auto reply = client.call("language.get");
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_EQ(reply.result.value("language", std::string{}), std::string{"en"});
    }
    {
        auto reply = client.call("language.set", utilxx_base::Json{{"language", "zh-cn"}});
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_EQ(reply.result.value("language", std::string{}), std::string{"zh-cn"});
    }
    {
        auto reply = client.call("language.get");
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_EQ(reply.result.value("language", std::string{}), std::string{"zh-cn"});
    }
    // 不支持的 auto 回退成 en (与 normalizeLanguage 一致)
    {
        auto reply = client.call("language.set", utilxx_base::Json{{"language", "auto"}});
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_EQ(reply.result.value("language", std::string{}), std::string{"en"});
    }
    // 合批开关: 打开后仍能正常收到增量 (合批只改变到达的粒度与时机)
    {
        auto reply = client.call("delta.set_batch", utilxx_base::Json{{"maxDelayMs", 24}});
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_EQ(reply.result.value("maxDelayMs", 0), 24);
    }
    XX_TEST_EXPECT_TRUE(
        client.call("input.send", utilxx_base::Json{{"text", "batched"}}).ok
    );
    XX_TEST_EXPECT_TRUE(client.rec.wait(AGENTXX_FFI_EVT_TURN_END, 30000));
    {
        // 关闭合批: 之后逐条收到 delta (不再等窗口)
        auto reply = client.call("delta.set_batch", utilxx_base::Json{{"maxDelayMs", 0}});
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_EQ(reply.result.value("maxDelayMs", -1), 0);
    }
    // maxDelayMs 负数被拒
    {
        uint64_t reqId = 0;
        XX_TEST_EXPECT_EQ(
            client.submit(R"({"op":"delta.set_batch","args":{"maxDelayMs":-5}})", reqId),
            AGENTXX_FFI_ERR_INVALID
        );
        XX_TEST_EXPECT_EQ(reqId, uint64_t{0});
    }

    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.waitStopped(30000));
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_OK);
    mock.stop();
}

/// 11) hosttool.* 的回执数据
void testHostToolOps() {
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    FfiTestClient client;
    XX_TEST_EXPECT_TRUE(ffiCreate(client, nullptr, mock.modelJson()));
    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.ready());

    // 注册: 回执里给出成功名单
    {
        auto args  = utilxx_base::Json::parse(R"({
            "tools": [{
                "name": "my_host_tool",
                "description": "test tool",
                "inputSchema": {"type":"object","properties":{"p":{"type":"string"}}},
                "timeoutSec": 30
            }]
        })");
        auto reply = client.call("hosttool.register", args);
        XX_TEST_EXPECT_TRUE(reply.ok);
        auto registered = reply.result.value("registered", utilxx_base::Json::array());
        XX_TEST_EXPECT_EQ(registered.size(), size_t{1});
        if (!registered.empty()) {
            XX_TEST_EXPECT_EQ(registered[0].get<std::string>(), std::string{"my_host_tool"});
        }
        XX_TEST_EXPECT_TRUE(reply.result.contains("failed"));
    }
    // 应答未知 callId: 服务端明确回 call_not_found (以前只打日志)
    {
        auto reply = client.call(
            "hosttool.respond",
            utilxx_base::Json{{"callId", 999}, {"ok", true}, {"result", "ok"}}
        );
        XX_TEST_EXPECT_FALSE(reply.ok);
        XX_TEST_EXPECT_EQ(reply.name, std::string{"call_not_found"});
    }
    // 注销: 回执给出移除名单
    {
        auto reply = client.call(
            "hosttool.unregister",
            utilxx_base::Json{{"names", {"my_host_tool"}}}
        );
        XX_TEST_EXPECT_TRUE(reply.ok);
        auto removed = reply.result.value("removed", utilxx_base::Json::array());
        XX_TEST_EXPECT_EQ(removed.size(), size_t{1});
    }
    // 参数校验: 空 tools
    {
        uint64_t reqId = 0;
        XX_TEST_EXPECT_EQ(
            client.submit(R"({"op":"hosttool.register","args":{"tools":[]}})", reqId),
            AGENTXX_FFI_ERR_INVALID
        );
        XX_TEST_EXPECT_EQ(reqId, uint64_t{0});
    }

    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.waitStopped(30000));
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_OK);
    mock.stop();
}

/// 12) 能力清单: ops / features
void testCapabilities() {
    FfiTestClient client;
    {
        // 无句柄也能取 (本库编译进来的那一套)
        AgentxxString out{};
        AgentxxString log{};
        XX_TEST_EXPECT_EQ(
            agentxx_ffi_get_capabilities(nullptr, &out, &log),
            AGENTXX_FFI_OK
        );
        XX_TEST_EXPECT_TRUE(out.data != nullptr);
        auto j = utilxx_base::Json::parse(std::string_view(out.data, out.size));
        XX_TEST_EXPECT_EQ(j.value("apiVersion", 0), 4);
        auto ops = j.value("ops", utilxx_base::Json::array());
        XX_TEST_EXPECT_TRUE(ops.size() > 20);
        auto features = j.value("features", utilxx_base::Json::array());
        XX_TEST_EXPECT_TRUE(features.size() >= 5);
        agentxx_ffi_string_free(&out);
        agentxx_ffi_string_free(&log);

        // null out
        XX_TEST_EXPECT_EQ(
            agentxx_ffi_get_capabilities(nullptr, nullptr, &log),
            AGENTXX_FFI_ERR_INVALID
        );
        agentxx_ffi_string_free(&log);
    }

    // 句柄上的清单与 op 表一致 (抽查关键 op)
    {
        auto caps = client.capabilities();
        XX_TEST_EXPECT_TRUE(!caps.empty());
        for (const char* op : {
                 "input.send",
                 "model.list",
                 "session.compact",
                 "context.view_page",
                 "hosttool.register",
                 "language.get",
             }) {
            XX_TEST_EXPECT_TRUE(caps.find(std::string("\"") + op + "\"") != std::string::npos);
        }
    }
}

/// 13) fs.list_dir / plugin.data_up (含 topic 校验失败)
void testFsAndPluginData() {
    namespace fs = std::filesystem;
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    const auto tmpDir
        = fs::temp_directory_path()
          / ("ffi_listdir_"
             + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(tmpDir, ec);

    FfiTestClient client;
    XX_TEST_EXPECT_TRUE(ffiCreate(client, nullptr, mock.modelJson()));
    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.ready());

    {
        auto reply = client.call(
            "fs.list_dir",
            utilxx_base::Json{{"path", tmpDir.generic_string()}}
        );
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_TRUE(reply.result.contains("entries"));
    }
    // 不存在的目录: 服务端回 ok:false + 原因
    {
        auto reply = client.call(
            "fs.list_dir",
            utilxx_base::Json{{"path", (tmpDir / "no-such-subdir").generic_string()}}
        );
        XX_TEST_EXPECT_FALSE(reply.ok);
    }
    // plugin.data_up: 主题必须落在 client 族, 否则服务端明确拒绝
    {
        auto reply = client.call(
            "plugin.data_up",
            utilxx_base::Json{{"topic", "not.a.client.topic"}, {"data", "{}"}}
        );
        XX_TEST_EXPECT_FALSE(reply.ok);
        XX_TEST_EXPECT_EQ(reply.name, std::string{"bad_topic"});
    }
    // 合法主题 (无订阅者也算已发布到总线)
    {
        auto reply = client.call(
            "plugin.data_up",
            utilxx_base::Json{
                {"topic", "agentxx.client.host.test_event"},
                {"data",  R"({"n":1})"                   }
        }
        );
        XX_TEST_EXPECT_TRUE(reply.ok);
    }

    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.waitStopped(30000));
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_OK);
    mock.stop();

    std::error_code cleanupEc;
    fs::remove_all(tmpDir, cleanupEc);
}

/// 14) HIL 权限中断: 首轮工具调用 → 权限询问 → interrupt.respond op → 轮次恢复
void testHilInterrupt() {
    FfiMockLLM mock;
    mock.firstIsToolCall = true;
    uint16_t port        = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    std::string pluginDir  = findPluginDir("agentxx_filesystem");
    std::string configJson = R"({"permissionMode": "all_ask"})";
    if (!pluginDir.empty()) {
        try {
            auto cfg       = utilxx_base::Json::parse(configJson);
            cfg["plugins"] = utilxx_base::Json::array({utilxx_base::Json{{"path", pluginDir}}});
            configJson     = cfg.dump();
        } catch (...) {
            TEST_FAIL << "inject plugins config failed" << std::endl;
            g_ffi_failed++;
            mock.stop();
            return;
        }
    }

    FfiTestClient client;
    XX_TEST_EXPECT_TRUE(ffiCreate(client, configJson.c_str(), mock.modelJson()));
    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.ready());

    XX_TEST_EXPECT_TRUE(client.call("input.send", utilxx_base::Json{{"text", "read hostname"}}).ok);

    // 等待权限中断请求
    XX_TEST_EXPECT_TRUE(client.rec.wait(AGENTXX_FFI_EVT_INTERRUPT_REQ, 30000));
    XX_TEST_EXPECT_FALSE(client.rec.has(AGENTXX_FFI_EVT_INTERRUPT_EXPIRED));

    int64_t interruptId = -1;
    {
        auto payload = client.rec.first(AGENTXX_FFI_EVT_INTERRUPT_REQ);
        try {
            auto j      = utilxx_base::Json::parse(payload);
            interruptId = j.value("interruptId", int64_t{-1});
            XX_TEST_EXPECT_TRUE(interruptId > 0);
            std::string argJson = j.value("argJson", std::string{});
            XX_TEST_EXPECT_TRUE(argJson.find("permission") != std::string::npos);
            // UI 描述必填: 权限询问下发权限卡片描述 (文案只带 i18n 键)
            XX_TEST_EXPECT_TRUE(argJson.find("\"ui\"") != std::string::npos);
            XX_TEST_EXPECT_TRUE(argJson.find("interrupt.permissionBadge") != std::string::npos);
        } catch (...) {
            g_ffi_failed++;
            TEST_FAIL << "interrupt payload not JSON: " << payload << std::endl;
        }
    }

    // 在独立线程延迟应答 (模拟嵌入方程序 UI 异步操作)
    CallReply   respondReply;
    std::thread responder([&client, interruptId, &respondReply]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        utilxx_base::Json args = utilxx_base::Json::object();
        args["interruptId"]    = interruptId;
        utilxx_base::Json values = utilxx_base::Json::object();
        values["decision"]       = "true";
        values["remember"]       = false;
        args["values"]           = std::move(values);
        respondReply            = client.call("interrupt.respond", args);
    });

    // 轮次应恢复并结束
    XX_TEST_EXPECT_TRUE(client.rec.wait(AGENTXX_FFI_EVT_TURN_END, 30000));
    responder.join();
    XX_TEST_EXPECT_TRUE(respondReply.ok);
    {
        auto turn = client.rec.first(AGENTXX_FFI_EVT_TURN_END);
        try {
            auto j = utilxx_base::Json::parse(turn);
            XX_TEST_EXPECT_TRUE(j.value("hasError", true) == false);
        } catch (...) {
            g_ffi_failed++;
            TEST_FAIL << "TURN_END payload not JSON: " << turn << std::endl;
        }
    }

    // 已应答后再次应答同一 id: 本地就拦下 (ERR_INTERRUPT, 无回执)
    {
        uint64_t reqId = 0;
        auto     req   = utilxx_base::Json{
            {"op",   "interrupt.respond"                                                },
            {"args", {{ "interruptId", interruptId}, {"values", {{ "decision", "true" }}}}},
        };
        XX_TEST_EXPECT_EQ(client.submit(req.dump(), reqId), AGENTXX_FFI_ERR_INTERRUPT);
        XX_TEST_EXPECT_EQ(reqId, uint64_t{0});
    }
    // 契约外载荷 (values 非对象) 被拒绝
    {
        uint64_t reqId = 0;
        utilxx_base::Json values = utilxx_base::Json::array({"true"});
        auto     req             = utilxx_base::Json::object();
        req["op"]                = "interrupt.respond";
        req["args"]             = utilxx_base::Json::object();
        req["args"]["interruptId"] = interruptId;
        req["args"]["values"]      = std::move(values);
        XX_TEST_EXPECT_EQ(client.submit(req.dump(), reqId), AGENTXX_FFI_ERR_INVALID);
        XX_TEST_EXPECT_EQ(reqId, uint64_t{0});
    }

    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.waitStopped(30000));
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_OK);
    mock.stop();
}

/// 15) 取消轮次: input.cancel 的回执表示"取消请求已处理", 轮次以中断收尾
void testTurnCancel() {
    FfiMockLLM mock;
    mock.slowMs   = 1500;
    uint16_t port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    FfiTestClient client;
    XX_TEST_EXPECT_TRUE(ffiCreate(client, nullptr, mock.modelJson()));
    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.ready());

    XX_TEST_EXPECT_TRUE(client.call("input.send", utilxx_base::Json{{"text", "run slow"}}).ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    XX_TEST_EXPECT_TRUE(client.call("input.cancel").ok);

    XX_TEST_EXPECT_TRUE(client.rec.wait(AGENTXX_FFI_EVT_TURN_END, 20000));
    {
        auto turn = client.rec.first(AGENTXX_FFI_EVT_TURN_END);
        try {
            auto j = utilxx_base::Json::parse(turn);
            XX_TEST_EXPECT_TRUE(j.value("hasError", false) == true);
            XX_TEST_EXPECT_TRUE(
                j.value("errorMessage", std::string{}).find("Canceled") != std::string::npos
            );
        } catch (...) {
            g_ffi_failed++;
            TEST_FAIL << "TURN_END payload not JSON: " << turn << std::endl;
        }
    }

    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.waitStopped(30000));
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_OK);
    mock.stop();
}

/// 16) 多 runtime 并发独立运行 (独立线程模型 + 独立 reqId 空间)
void testMultipleRuntimesConcurrent() {
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    constexpr size_t kRuntimeCount = 3;

    std::vector<std::unique_ptr<FfiTestClient>> slots;
    slots.reserve(kRuntimeCount);
    for (size_t i = 0; i < kRuntimeCount; ++i) {
        slots.push_back(std::make_unique<FfiTestClient>());
        XX_TEST_EXPECT_TRUE(ffiCreate(*slots.back(), nullptr, mock.modelJson()));
        XX_TEST_EXPECT_EQ(slots.back()->start(), AGENTXX_FFI_OK);
    }
    for (size_t i = 0; i < kRuntimeCount; ++i) {
        XX_TEST_EXPECT_TRUE(slots[i]->ready());
    }
    // 每个句柄的 reqId 从 1 开始独立编号
    for (size_t i = 0; i < kRuntimeCount; ++i) {
        auto reply = slots[i]->call("model.list");
        XX_TEST_EXPECT_TRUE(reply.ok);
        XX_TEST_EXPECT_EQ(reply.reqId, uint64_t{1});
    }
    // 并发发送输入并各自收到 TURN_END
    std::vector<std::thread> threads;
    for (size_t i = 0; i < kRuntimeCount; ++i) {
        threads.emplace_back([&slots, i] {
            XX_TEST_EXPECT_TRUE(
                slots[i]->call("input.send", utilxx_base::Json{{"text", "hi from slot"}}).ok
            );
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    for (size_t i = 0; i < kRuntimeCount; ++i) {
        XX_TEST_EXPECT_TRUE(slots[i]->rec.wait(AGENTXX_FFI_EVT_TURN_END, 30000));
    }
    for (size_t i = 0; i < kRuntimeCount; ++i) {
        XX_TEST_EXPECT_EQ(slots[i]->stop(), AGENTXX_FFI_OK);
        XX_TEST_EXPECT_TRUE(slots[i]->waitStopped(30000));
        XX_TEST_EXPECT_EQ(slots[i]->destroy(), AGENTXX_FFI_OK);
    }
    mock.stop();
}

/// 17) headless (on_event 为空): 不序列化事件也不崩, 调用照常工作
void testHeadless() {
    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    FfiTestClient client;
    XX_TEST_EXPECT_TRUE(ffiCreate(client, nullptr, mock.modelJson(), /*headless=*/true));
    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);

    // 没有 EVT_READY 可等: 起跑后直接调 op (服务端就绪前提交也受理)
    bool sawOk = false;
    for (int i = 0; i < 100 && !sawOk; ++i) {
        uint64_t      reqId = 0;
        AgentxxString log{};
        auto          sv = agentxx_string_view_cstr(R"({"op":"model.list"})");
        const int32_t rc = agentxx_ffi_call(client.a, &sv, &reqId, &log);
        agentxx_ffi_string_free(&log);
        if (rc == AGENTXX_FFI_OK && reqId != 0) {
            sawOk = true;
            // headless 也照常结算 (看不到回执, 但不会卡住调用表)
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            XX_TEST_EXPECT_EQ(client.rec.resultCountOf(reqId), 0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    XX_TEST_EXPECT_TRUE(sawOk);

    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    // headless 也能通过状态判断收尾完成 (destroy 只认 Stopped)
    int32_t rc = AGENTXX_FFI_ERR_STATE;
    for (int i = 0; i < 300 && rc != AGENTXX_FFI_OK; ++i) {
        rc = client.destroy();
        if (rc != AGENTXX_FFI_OK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    XX_TEST_EXPECT_EQ(rc, AGENTXX_FFI_OK);
    mock.stop();
}

/// 18) 会话落盘: session.list / session.rename (列表走线程池卸载后回填)
void testSessionStoreOps() {
    namespace fs = std::filesystem;

    FfiMockLLM mock;
    uint16_t   port = 0;
    if (!mock.start(port)) {
        TEST_FAIL << "mock LLM server start failed" << std::endl;
        g_ffi_failed++;
        return;
    }

    const auto tmpDir
        = fs::temp_directory_path()
          / ("ffi_session_store_"
             + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(tmpDir / "data", ec);

    // 会话落盘打开: 会话列表要扫目录 + 读 SQLite, 服务端会卸载到线程池后回填
    const std::string configJson
        = utilxx_base::Json{
              {"dataDir",            (tmpDir / "data").generic_string()},
              {"workDir",            tmpDir.generic_string()            },
              {"enableSessionStore", true                               },
              {"permissionMode",     "pass"                             },
    }
              .dump();

    FfiTestClient client;
    XX_TEST_EXPECT_TRUE(ffiCreate(client, configJson.c_str(), mock.modelJson()));
    XX_TEST_EXPECT_EQ(client.start(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.ready());

    // 一轮对话: 会话随轮次落盘
    XX_TEST_EXPECT_TRUE(client.call("input.send", utilxx_base::Json{{"text", "hello store"}}).ok);
    XX_TEST_EXPECT_TRUE(client.rec.wait(AGENTXX_FFI_EVT_TURN_END, 30000));

    // 会话列表: 落盘打开后至少一条, 且带 sessionId
    std::string sessionId;
    {
        auto reply = client.call("session.list", utilxx_base::Json::object(), -1, 30000);
        XX_TEST_EXPECT_TRUE(reply.ok);
        if (!reply.ok) {
            std::cout << "[ffi_c_api] session.list failed: name=" << reply.name
                      << " message=" << reply.message << std::endl;
        }
        XX_TEST_EXPECT_TRUE(reply.result.contains("sessions"));
        if (reply.result.contains("sessions") && reply.result["sessions"].is_array()
            && reply.result["sessions"].size() > 0) {
            sessionId = reply.result["sessions"][0].value("sessionId", std::string{});
        }
        XX_TEST_EXPECT_TRUE(!sessionId.empty());
    }

    // 改名 (写会话库): 改的是句柄当前绑定的会话
    if (!sessionId.empty()) {
        auto reply = client.call(
            "session.rename",
            utilxx_base::Json{{"title", "store-renamed"}},
            -1,
            30000
        );
        XX_TEST_EXPECT_TRUE(reply.ok);
    }

    XX_TEST_EXPECT_EQ(client.stop(), AGENTXX_FFI_OK);
    XX_TEST_EXPECT_TRUE(client.waitStopped(30000));
    XX_TEST_EXPECT_EQ(client.destroy(), AGENTXX_FFI_OK);
    mock.stop();

    std::error_code cleanupEc;
    fs::remove_all(tmpDir, cleanupEc);
}

} // namespace

namespace agentxx {
namespace test {

agentxx::test::TestResult testFfiCApi() {
    testVersionAndMemory();
    testCreateInvalid();
    testLifecycleAndConversation();
    testCallValidation();
    testConcurrentCalls();
    testCancelSemantics();
    testOrdering();
    testCallFromCallback();
    testLifecycleAsync();
    testLocalOps();
    testHostToolOps();
    testCapabilities();
    testFsAndPluginData();
    testHilInterrupt();
    testTurnCancel();
    testMultipleRuntimesConcurrent();
    testHeadless();
    testSessionStoreOps();
    return agentxx::test::TestResult(g_ffi_passed, g_ffi_failed);
}

} // namespace test
} // namespace agentxx
