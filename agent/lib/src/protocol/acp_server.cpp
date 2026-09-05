#include "agentxx/protocol/acp_server.h"

#include "agentxx/util/container_util.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include <fmt/format.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace agentxx {
namespace server {

// ---------------------------------------------------------------------------
// AcpProtocolHandler
// ---------------------------------------------------------------------------

AcpProtocolHandler::AcpProtocolHandler(
    std::shared_ptr<agentxx::agent::BaseAgent> agent,
    json                                       agentInfo,
    Config                                     config
) :
    config_(std::move(config)),
    agent_(std::move(agent)),
    agentInfo_(std::move(agentInfo)) {}

AcpProtocolHandler::~AcpProtocolHandler() {
    stop();
}

bool AcpProtocolHandler::initialized() const {
    return initialized_.load(std::memory_order_acquire);
}

const json& AcpProtocolHandler::agentInfo() const {
    return agentInfo_;
}

void AcpProtocolHandler::setNotificationSink(NotificationSink sink) {
    sink_ = std::move(sink);
}

bool AcpProtocolHandler::stopRequested() const {
    return stopFlag_.load(std::memory_order_acquire);
}

void AcpProtocolHandler::stop() {
    stopFlag_.store(true, std::memory_order_release);
    {
        std::lock_guard lk(sessionsMu_);
        for (auto& [sid, flag] : cancelFlags_) {
            if (flag) {
                flag->store(true, std::memory_order_release);
            }
        }
    }
    workersCv_.notify_all();
    // 等待所有执行中 worker 结束: worker 为 detached 线程, 若不等待, 本对象析构后
    // 仍在运行的 worker 会访问已销毁成员 (agent_/sessions/sink) → UAF。
    // worker 检测到 cancel/stop 标志后退出 (callClient 亦自带超时, 不会永久阻塞)。
    drainWorkers();
}

json AcpProtocolHandler::handleMessage(const json& env) {
    bool hasMethod = env.contains("method") && !env["method"].is_null();

    if (!hasMethod) {
        if (env.contains("id")) {
            auto    idV = env["id"];
            int64_t id  = idV.is_number_integer() ? idV.get<int64_t>() : -1;
            std::shared_ptr<std::promise<json>> p = nullptr;
            {
                std::lock_guard lk(pendingMu_);
                auto            it = pending_.find(id);
                if (it != pending_.end()) {
                    p = it->second;
                    pending_.erase(it);
                }
            }
            if (p) {
                p->set_value(env);
            }
        }
        return {};
    }

    auto method         = env.value("method", std::string());
    auto params         = env.contains("params") ? env["params"] : json::object();
    auto id             = env.contains("id") ? env["id"] : json();
    bool isNotification = !env.contains("id");

    bool handled = false;
    auto result  = agentxx::util::catchError<json>(
        [&]() -> json {
            if (method == "initialize") {
                handled = true;
                return handleInitialize(params, id);
            }
            if (method == "session/new") {
                handled = true;
                return handleSessionNew(params, id);
            }
            if (method == "session/prompt") {
                handled = true;
                handleSessionPrompt(env, params, id);
                return {};
            }
            if (method == "session/cancel") {
                handled = true;
                handleSessionCancel(params);
                return {};
            }
            return {};
        },
        [&](std::string errmsg) -> json {
            XX_LOGE("[acp] error handling '{}': {}", method, errmsg);
            if (!isNotification) {
                return jsonRpcError(id, -32602, fmt::format("Invalid params: {}", errmsg));
            }
            return {};
        }
    );
    if (!handled) {
        if (isNotification) {
            return {};
        }
        return jsonRpcError(id, -32601, fmt::format("Method not found: {}", method));
    }
    return result;
}

json AcpProtocolHandler::callClient(
    std::string_view          method,
    json                      params,
    std::chrono::milliseconds timeout
) {
    if (!sink_) {
        throw std::runtime_error("AcpProtocolHandler::callClient: no notification sink set");
    }

    auto id = nextOutboundId_.fetch_add(1, std::memory_order_relaxed);
    json env;
    env["jsonrpc"] = "2.0";
    env["id"]      = id;
    env["method"]  = method;
    env["params"]  = std::move(params);

    auto promise = std::make_shared<std::promise<json>>();
    auto fut     = promise->get_future();
    {
        std::lock_guard lk(pendingMu_);
        pending_[id] = promise;
    }

    sink_(env);

    auto status = fut.wait_for(timeout);
    if (status != std::future_status::ready) {
        std::lock_guard lk(pendingMu_);
        pending_.erase(id);
        throw std::runtime_error(
            fmt::format("AcpProtocolHandler::callClient: timeout for '{}'", method)
        );
    }
    auto resp = fut.get();
    if (resp.contains("error")) {
        throw std::runtime_error(
            fmt::format("AcpProtocolHandler::callClient: error: {}", resp["error"].dump())
        );
    }
    return resp.contains("result") ? resp["result"] : json::object();
}

bool AcpProtocolHandler::hasSession(std::string_view sessionId) const {
    std::lock_guard lk(sessionsMu_);
    return sessions_.find(sessionId) != sessions_.end();
}

std::string AcpProtocolHandler::sessionCwd(std::string_view sessionId) const {
    std::lock_guard lk(sessionsMu_);
    auto            it = sessions_.find(sessionId);
    return it != sessions_.end() ? it->second : std::string{};
}

bool AcpProtocolHandler::isInFlight(std::string_view sessionId) const {
    std::lock_guard lk(inflightMu_);
    return inflightSessions_.count(sessionId) > 0;
}

int AcpProtocolHandler::inflightCount() const {
    return inflightCount_.load(std::memory_order_acquire);
}

void AcpProtocolHandler::drainWorkers() {
    std::unique_lock lk(workersMu_);
    workersCv_.wait(lk, [this] {
        return inflightCount_.load(std::memory_order_acquire) == 0;
    });
}

json AcpProtocolHandler::jsonRpcResult(const json& id, json result) {
    json j;
    j["jsonrpc"] = "2.0";
    j["id"]      = id;
    j["result"]  = std::move(result);
    return j;
}

json AcpProtocolHandler::jsonRpcError(const json& id, int code, std::string_view msg) {
    json j;
    j["jsonrpc"] = "2.0";
    j["id"]      = id;
    j["error"]   = {
        {"code",    code},
        {"message", msg }
    };
    return j;
}

json AcpProtocolHandler::makeParseError(std::string_view detail) {
    return jsonRpcError(json{}, -32700, fmt::format("Parse error: {}", detail));
}

json AcpProtocolHandler::makeInvalidRequest() {
    return jsonRpcError(json{}, -32600, "Invalid Request");
}

std::string AcpProtocolHandler::extractUserText(const json& prompt) {
    if (!prompt.is_array()) {
        return {};
    }
    std::string text;
    for (const auto& block : prompt) {
        if (block.value("type", "") == "text" && block.contains("text")) {
            if (!text.empty()) {
                text += ' ';
            }
            text += block["text"].get<std::string>();
        }
    }
    return text;
}

std::string AcpProtocolHandler::generateSessionId() {
    static std::atomic<uint64_t> counter{0};
    static uint64_t              seed = std::chrono::steady_clock::now().time_since_epoch().count();
    auto                         c    = counter.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream           oss;
    oss << "sess-" << std::hex << (seed & 0xFFFFFFFF) << "-" << std::setw(12) << std::setfill('0')
        << c;
    return oss.str();
}

json AcpProtocolHandler::handleInitialize(const json& params, const json& id) {
    initialized_.store(true, std::memory_order_release);

    auto caps = json{
        {"loadSession",         false                                                           },
        {"promptCapabilities",  {{"image", false}, {"audio", false}, {"embeddedContext", false}}},
        {"mcpCapabilities",     {{"http", false}, {"sse", false}}                               },
        {"sessionCapabilities", {{"close", false}, {"list", false}, {"resume", false}}          },
    };

    auto result = json{
        {"protocolVersion", params.value("protocolVersion", 1)},
        {"agentCapabilities", std::move(caps)},
        {"authMethods", json::array()},
        {"agentInfo", agentInfo_},
    };
    return jsonRpcResult(id, std::move(result));
}

json AcpProtocolHandler::handleSessionNew(const json& params, const json& id) {
    std::string cwd       = params.value("cwd", std::string{});
    auto        sessionId = generateSessionId();

    {
        std::lock_guard lk(sessionsMu_);
        util::insertOrAssignHeterogeneous(sessions_, sessionId, cwd);
        util::insertOrAssignHeterogeneous(
            cancelFlags_,
            sessionId,
            std::make_shared<std::atomic<bool>>(false)
        );
    }

    // 会话工作目录独立: 把客户端提供的 cwd 注入本会话 (AgentContext::
    // getSessionWorkDir 解析时优先于 agent 级配置), filesystem/命令执行等
    // 工具的相对路径基准与权限放行范围随之切换到该目录 —— 各 ACP 会话可绑定
    // 不同项目目录, 不再隐式依赖 agent 进程启动目录 (cwd 为空/上下文不可用
    // 时保持旧行为: 回退 AgentConfig::resolvedWorkDir / 进程 cwd)
    if (!cwd.empty()) {
        auto ctx = agent_ ? agent_->getContext() : nullptr;
        if (ctx) {
            // 归一为绝对路径 (~ 展开与相对路径按进程 cwd 解析; ACP 客户端
            // 通常直接发送绝对路径, 此处仅兜底非规范输入)
            auto absCwd = agentxx::util::toCurrentSystemAbsolutePath(cwd);
            // 词法规范化对以 '.'/'..' 结尾的路径保留尾部分隔符 (".../dir/"
            // 形式); 工作目录基准统一去除尾斜杠 (根目录 "/" 除外),
            // 便于各使用方拼接与比较
            while (absCwd.size() > 1 && absCwd.back() == '/') {
                absCwd.pop_back();
            }
            if (!absCwd.empty()) {
                ctx->setSessionWorkDir(sessionId, absCwd);
            }
        }
    }

    XX_LOGI("[acp] session/new: {} (cwd={})", sessionId, cwd);

    auto result = json{
        {"sessionId",     sessionId     },
        {"configOptions", json::object()},
        {"modes",         json::object()}
    };
    return jsonRpcResult(id, std::move(result));
}

void AcpProtocolHandler::handleSessionPrompt(const json& env, const json& params, const json& id) {
    std::string sessionId = params.value("sessionId", std::string{});

    if (inflightCount_.load(std::memory_order_acquire) >= config_.maxInflightPrompts) {
        auto err = jsonRpcError(
            id,
            -32000,
            fmt::format(
                "ACP server overloaded: {} concurrent prompts in flight; retry shortly",
                config_.maxInflightPrompts
            )
        );
        emit(err);
        return;
    }

    std::shared_ptr<std::atomic<bool>> cancelFlag;
    {
        std::lock_guard lk(sessionsMu_);
        util::getOrCreateHeterogeneous(sessions_, sessionId);
        auto it = cancelFlags_.find(sessionId);
        if (it == cancelFlags_.end()) {
            it = cancelFlags_.emplace(sessionId, std::make_shared<std::atomic<bool>>(false)).first;
        }
        cancelFlag = it->second;
    }

    {
        std::lock_guard lk(inflightMu_);
        if (!util::insertHeterogeneous(inflightSessions_, sessionId).second) {
            auto err = jsonRpcError(
                id,
                -32000,
                fmt::format(
                    "session_id {} already has a prompt in flight; "
                    "ACP requires single-flight per session",
                    sessionId
                )
            );
            emit(err);
            return;
        }
    }

    inflightCount_.fetch_add(1, std::memory_order_acq_rel);

    auto promptBlocks = params.contains("prompt") ? params["prompt"] : json::array();

    std::thread worker([this, sessionId, promptBlocks, id, cancelFlag]() {
        workerRunPrompt(sessionId, promptBlocks, id, cancelFlag);
    });
    worker.detach();
}

void AcpProtocolHandler::workerRunPrompt(
    std::string_view                   sessionId,
    const json&                        promptBlocks,
    const json&                        id,
    std::shared_ptr<std::atomic<bool>> cancelFlag
) {
    // catchError: 本函数运行于 detached worker 线程, 取消类异常也须就地转为
    // 错误消息 (onRethrow), 避免异常逃逸线程 → std::terminate
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto userText = extractUserText(promptBlocks);

            auto& engine = agent_->engine;
            if (!engine) {
                emitAgentMessageChunk(sessionId, "(graph error: engine is null)");
                emit(jsonRpcResult(
                    id,
                    {
                        {"stopReason", "end_turn"}
                }
                ));
                return true;
            }

            json state;
            state["prompt"]          = userText;
            state["_acp_session_id"] = sessionId;

            neograph::graph::RunConfig cfg;
            cfg.thread_id    = sessionId;
            cfg.input        = std::move(state);
            cfg.stream_mode  = neograph::graph::StreamMode::ALL;
            cfg.cancel_token = std::make_shared<neograph::graph::CancelToken>();

            auto result = engine->run(cfg);

            if (cancelFlag->exchange(false, std::memory_order_acq_rel)) {
                emit(jsonRpcResult(
                    id,
                    {
                        {"stopReason", "cancelled"}
                }
                ));
                return true;
            }

            std::string agentText;

            auto channels = result.channel_raw("messages");
            if (channels.is_array() && !channels.empty()) {
                auto last = channels[channels.size() - 1];
                if (last.contains("content")) {
                    agentText = last["content"].get<std::string>();
                }
            }
            if (agentText.empty()) {
                auto resp = result.channel_raw("response");
                if (resp.is_string()) {
                    agentText = resp.get<std::string>();
                }
            }

            if (!agentText.empty()) {
                json chunk                        = json::object();
                chunk["sessionId"]                = sessionId;
                chunk["update"]["session_update"] = "agent_message_chunk";
                chunk["update"]["content"]        = json{
                           {"type", "text"   },
                           {"text", agentText}
                };
                chunk["update"]["raw"] = chunk["update"];
                emitNotification("session/update", chunk);
            }

            emit(jsonRpcResult(
                id,
                {
                    {"stopReason", "end_turn"}
            }
            ));
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("[acp] worker error: {}", errmsg);
            emitAgentMessageChunk(sessionId, fmt::format("(graph error: {})", errmsg));
            emit(jsonRpcResult(
                id,
                {
                    {"stopReason", "end_turn"}
            }
            ));
            return false;
        },
        [&](std::string& errmsg) -> std::optional<bool> {
            XX_LOGE("[acp] worker error: {}", errmsg);
            emitAgentMessageChunk(sessionId, fmt::format("(graph error: {})", errmsg));
            emit(jsonRpcResult(
                id,
                {
                    {"stopReason", "end_turn"}
            }
            ));
            return false;
        }
    );

    workerCleanup(sessionId);
}

void AcpProtocolHandler::workerCleanup(std::string_view sessionId) {
    {
        std::lock_guard lk(inflightMu_);
        (void)util::eraseHeterogeneous(inflightSessions_, sessionId); // 异构删除免拷贝
    }
    auto prev = inflightCount_.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
        std::lock_guard lk(workersMu_);
        workersCv_.notify_all();
    }
}

void AcpProtocolHandler::handleSessionCancel(const json& params) {
    std::string sessionId = params.value("sessionId", std::string{});
    if (sessionId.empty()) {
        return;
    }

    std::lock_guard lk(sessionsMu_);
    auto            it = cancelFlags_.find(sessionId);
    if (it != cancelFlags_.end() && it->second) {
        it->second->store(true, std::memory_order_release);
        XX_LOGI("[acp] session/cancel: {}", sessionId);
    } else {
        util::insertOrAssignHeterogeneous(
            cancelFlags_,
            sessionId,
            std::make_shared<std::atomic<bool>>(true)
        );
    }
}

void AcpProtocolHandler::emit(const json& env) {
    if (sink_) {
        sink_(env);
    }
}

void AcpProtocolHandler::emitNotification(std::string_view method, const json& params) {
    json env;
    env["jsonrpc"] = "2.0";
    env["method"]  = method;
    env["params"]  = params;
    emit(env);
}

void AcpProtocolHandler::emitAgentMessageChunk(std::string_view sessionId, std::string_view text) {
    json chunk;
    chunk["sessionId"]                = sessionId;
    chunk["update"]["session_update"] = "agent_message_chunk";
    chunk["update"]["content"]        = json{
               {"type", "text"},
               {"text", text  }
    };
    chunk["update"]["raw"] = chunk["update"];
    emitNotification("session/update", chunk);
}

// ---------------------------------------------------------------------------
// HttpAcpServer
// ---------------------------------------------------------------------------

HttpAcpServer::HttpAcpServer(
    std::shared_ptr<agentxx::agent::BaseAgent> agent,
    neograph::json                             agentInfo,
    Config                                     config
) :
    config_(std::move(config)),
    agent_(std::move(agent)),
    handler_(
        agent_,
        std::move(agentInfo),
        {.serverName = config_.serverName, .serverVersion = config_.serverVersion}
    ) {
    setupHandlerSink();
    httpServer_ = std::make_unique<util::HttpServer>(config_.httpConfig);
    setupRoutes();
}

HttpAcpServer::~HttpAcpServer() {
    stop();
}

void HttpAcpServer::start() {
    httpServer_->start();
}

void HttpAcpServer::stop() {
    handler_.stop();
    stopSSE();
    httpServer_->stop();
}

uint16_t HttpAcpServer::port() const {
    return httpServer_->port();
}

bool HttpAcpServer::isStopped() const {
    return httpServer_->isStopped();
}

AcpProtocolHandler& HttpAcpServer::handler() {
    return handler_;
}

void HttpAcpServer::setupHandlerSink() {
    handler_.setNotificationSink([this](const neograph::json& envelope) {
        if (!envelope.contains("method") && envelope.contains("id") && !envelope["id"].is_null()) {
            neograph::json id    = envelope["id"];
            int64_t        idVal = id.is_number_integer() ? id.get<int64_t>() : -1;

            std::unique_lock lock(pendingMutex_);
            auto             it = pendingResponses_.find(idVal);
            if (it != pendingResponses_.end()) {
                it->second->set_value(envelope);
                pendingResponses_.erase(it);
            }
        }

        std::string sseData;
        bool        isResponse
            = !envelope.contains("method") && envelope.contains("id") && !envelope["id"].is_null();
        if (isResponse) {
            sseData = fmt::format("data: {}\n\n", envelope.dump());
        } else {
            std::string method    = envelope.value("method", "");
            std::string eventType = method;
            if (!eventType.empty()) {
                for (auto& c : eventType) {
                    if (c == '/') {
                        c = '_';
                    }
                }
                sseData = fmt::format("event: {}\ndata: {}\n\n", eventType, envelope.dump());
            } else {
                sseData = fmt::format("data: {}\n\n", envelope.dump());
            }
        }

        broadcastSSE(sseData);
    });
}

void HttpAcpServer::setupRoutes() {
    using Handler = util::HttpServer::Handler;

    auto acpHandler = std::make_shared<Handler>(Handler(
        [this](util::HttpServer::Request& req, util::HttpServer::Response& resp, std::string_view)
            -> asio::awaitable<void> {
            co_await handleAcpRequest(req, resp);
        }
    ));
    httpServer_->router().add(config_.acpEndpoint, 2, acpHandler);

    auto sseHandler = std::make_shared<Handler>(Handler(
        [this](util::HttpServer::Request& req, util::HttpServer::Response& resp, std::string_view)
            -> asio::awaitable<void> {
            co_await handleSseRequest(req, resp);
        }
    ));
    httpServer_->router().add(config_.sseEndpoint, 0, sseHandler);
}

asio::awaitable<void> HttpAcpServer::handleAcpRequest(
    util::HttpServer::Request&  req,
    util::HttpServer::Response& resp
) {
    namespace http = boost::beast::http;

    bool           isError     = false;
    neograph::json requestJson = agentxx::util::catchError<neograph::json>(
        [&req]() -> neograph::json {
            return neograph::json::parse(req.body());
        },
        [&](std::string errmsg) -> neograph::json {
            writeJsonResponse(
                resp,
                http::status::bad_request,
                AcpProtocolHandler::makeParseError(std::move(errmsg))
            );
            isError = true;
            return neograph::json{};
        }
    );
    if (isError) {
        co_return;
    }

    if (!requestJson.is_object() || requestJson.value("jsonrpc", "") != "2.0") {
        writeJsonResponse(
            resp,
            http::status::bad_request,
            AcpProtocolHandler::makeInvalidRequest()
        );
        isError = true;
    }
    if (isError) {
        co_return;
    }

    neograph::json id = requestJson.contains("id") ? requestJson["id"] : neograph::json{};

    neograph::json response = agentxx::util::catchError<neograph::json>(
        [&]() -> neograph::json {
            return handler_.handleMessage(requestJson);
        },
        [&](std::string errmsg) -> neograph::json {
            XX_LOGE("[acp] handleMessage error: {}", errmsg);
            writeJsonResponse(
                resp,
                http::status::internal_server_error,
                jsonRpcError(id, -32603, fmt::format("Internal error: {}", errmsg))
            );
            isError = true;
            return neograph::json{};
        }
    );
    if (isError) {
        co_return;
    }

    if (!response.is_null()) {
        writeJsonResponse(resp, http::status::ok, response);
        co_return;
    }

    if (id.is_null()) {
        writeJsonResponse(
            resp,
            http::status::accepted,
            neograph::json{
                {"jsonrpc", "2.0"                   },
                {"id",      neograph::json(nullptr) },
                {"result",  neograph::json::object()}
        }
        );
        co_return;
    }

    int64_t idVal = id.is_number_integer() ? id.get<int64_t>() : -1;
    if (idVal < 0 && id.is_string()) {
        std::string idStr = id.get<std::string>();
        idVal             = static_cast<int64_t>(std::hash<std::string>{}(idStr));
    }

    auto promise = std::make_shared<std::promise<neograph::json>>();
    auto future  = promise->get_future().share();

    {
        std::unique_lock lock(pendingMutex_);
        auto [it, inserted] = pendingResponses_.try_emplace(idVal, promise);
        if (!inserted) {
            writeJsonResponse(
                resp,
                http::status::internal_server_error,
                jsonRpcError(id, -32603, "Duplicate request ID")
            );
            co_return;
        }
    }

    // 协程感知的异步等待: 周期性挂起(让出线程)检查 future, 直到就绪或超时。
    // 直接 future.wait_for 会阻塞整个 executor 线程, 卡死同线程上的其他协程/请求。
    const auto         deadline = std::chrono::steady_clock::now() + config_.asyncTimeout;
    std::future_status status   = future.wait_for(std::chrono::milliseconds(0));
    while (std::future_status::timeout == status && std::chrono::steady_clock::now() < deadline) {
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(20));
        co_await timer.async_wait(asio::use_awaitable);
        status = future.wait_for(std::chrono::milliseconds(0));
    }
    if (status == std::future_status::timeout) {
        std::unique_lock lock(pendingMutex_);
        pendingResponses_.erase(idVal);
        writeJsonResponse(
            resp,
            http::status::gateway_timeout,
            jsonRpcError(id, -32000, "Async request timed out")
        );
        co_return;
    }

    neograph::json asyncResponse = future.get();
    {
        std::unique_lock lock(pendingMutex_);
        pendingResponses_.erase(idVal);
    }

    writeJsonResponse(resp, http::status::ok, asyncResponse);
}

asio::awaitable<void> HttpAcpServer::handleSseRequest(
    util::HttpServer::Request&  req,
    util::HttpServer::Response& resp
) {
    resp.version(req.version());
    resp.result(boost::beast::http::status::ok);
    resp.set(boost::beast::http::field::content_type, "text/event-stream");
    resp.set(boost::beast::http::field::cache_control, "no-cache");
    resp.set(boost::beast::http::field::connection, "keep-alive");
    resp.set("X-Accel-Buffering", "no");

    resp.body() = fmt::format("event: endpoint\ndata: {}\n\n", config_.acpEndpoint);
    resp.prepare_payload();
    co_return;
}

void HttpAcpServer::broadcastSSE(std::string_view /*data*/) {
    if (config_.httpConfig.accessLogEnabled) {
        XX_LOGI("[acp] SSE notification");
    }
}

void HttpAcpServer::stopSSE() {}

void HttpAcpServer::writeJsonResponse(
    util::HttpServer::Response& resp,
    boost::beast::http::status  status,
    const neograph::json&       body
) {
    resp.result(status);
    resp.set(boost::beast::http::field::content_type, "application/json");
    resp.body() = body.dump();
    resp.prepare_payload();
}

neograph::json
    HttpAcpServer::jsonRpcError(const neograph::json& id, int code, std::string_view message)
        const {
    neograph::json err;
    err["jsonrpc"] = "2.0";
    err["id"]      = id;
    err["error"]   = {
        {"code",    code   },
        {"message", message}
    };
    return err;
}

// ---------------------------------------------------------------------------
// StdioAcpServer
// ---------------------------------------------------------------------------

StdioAcpServer::StdioAcpServer(
    std::shared_ptr<agentxx::agent::BaseAgent> agent,
    neograph::json                             agentInfo
) :
    agent_(std::move(agent)),
    handler_(
        agent_,
        std::move(agentInfo),
        {.serverName = "agentxx-acp-stdio", .serverVersion = "0.1.0"}
    ) {}

StdioAcpServer::~StdioAcpServer() {
    stop();
}

void StdioAcpServer::run() {
    run(std::cin, std::cout);
}

void StdioAcpServer::run(std::istream& in, std::ostream& out) {
    running_.store(true, std::memory_order_release);

    struct RunningGuard {
        std::atomic<bool>* flag;

        ~RunningGuard() {
            if (flag) {
                flag->store(false, std::memory_order_release);
            }
        }
    };

    RunningGuard guard{&running_};

    auto outMu  = std::make_shared<std::mutex>();
    auto outPtr = &out;

    handler_.setNotificationSink([outPtr, outMu](const neograph::json& env) {
        auto            s = env.dump();
        std::lock_guard lk(*outMu);
        (*outPtr) << s << '\n';
        outPtr->flush();
    });

    std::string line;
    while (!handler_.stopRequested() && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        neograph::json env;
        bool           parsed = agentxx::util::catchError<bool>(
            [&]() -> bool {
                env = neograph::json::parse(line);
                return true;
            },
            [&](std::string) -> bool {
                std::lock_guard lk(*outMu);
                (*outPtr) << AcpProtocolHandler::makeParseError("invalid JSON").dump() << '\n';
                outPtr->flush();
                return false;
            }
        );
        if (!parsed) {
            continue;
        }

        auto resp = handler_.handleMessage(env);
        if (!resp.is_null()) {
            std::lock_guard lk(*outMu);
            (*outPtr) << resp.dump() << '\n';
            outPtr->flush();
        }
    }

    handler_.drainWorkers();

    handler_.setNotificationSink(nullptr);
}

void StdioAcpServer::stop() {
    handler_.stop();
}

bool StdioAcpServer::isRunning() const {
    return running_.load(std::memory_order_acquire);
}

AcpProtocolHandler& StdioAcpServer::handler() {
    return handler_;
}

} // namespace server
} // namespace agentxx
