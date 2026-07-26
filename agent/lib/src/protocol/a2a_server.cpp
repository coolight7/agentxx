#include "agentxx/protocol/a2a_server.h"

#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include <fmt/format.h>
#include <random>
#include <sstream>
#include <thread>

namespace agentxx {
namespace server {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

A2aServer::A2aServer(std::shared_ptr<agentxx::agent::DeepAgent> agent, Config config) :
    config_(std::move(config)),
    deepAgent_(std::move(agent)),
    httpServer_(std::make_unique<util::HttpServer>(config_.httpConfig)) {
    setupRoutes();
}

A2aServer::~A2aServer() {
    stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void A2aServer::start() {
    httpServer_->start();
}

void A2aServer::stop() {
    if (stopped_.exchange(true)) {
        return;
    }
    stopSSE();
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        for (auto& [id, task] : tasks_) {
            if (task->cancelFlag) {
                task->cancelFlag->store(true, std::memory_order_release);
            }
        }
    }
    httpServer_->stop();
    {
        std::lock_guard<std::mutex> lock(workersMutex_);
        for (auto& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
        workers_.clear();
    }
}

uint16_t A2aServer::port() const {
    return httpServer_->port();
}

bool A2aServer::isStopped() const {
    return stopped_.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Agent Card
// ---------------------------------------------------------------------------

json A2aServer::agentCard() const {
    json skills = json::array();
    for (const auto& s : config_.skills) {
        json sk;
        sk["id"]          = s.id;
        sk["name"]        = s.name;
        sk["description"] = s.description;
        sk["tags"]        = s.tags;
        if (!s.examples.empty()) {
            sk["examples"] = s.examples;
        }
        skills.push_back(std::move(sk));
    }

    json interfaces = json::array();
    {
        json iface;
        iface["url"] = "http://localhost:" + std::to_string(port()) + config_.a2aEndpoint;
        iface["protocolBinding"] = "JSONRPC";
        iface["protocolVersion"] = kA2aVersion;
        interfaces.push_back(std::move(iface));
    }

    json card;
    card["name"]                = config_.serverName;
    card["description"]         = config_.description;
    card["version"]             = config_.serverVersion;
    card["supportedInterfaces"] = std::move(interfaces);
    card["defaultInputModes"]   = config_.inputModes;
    card["defaultOutputModes"]  = config_.outputModes;
    card["skills"]              = std::move(skills);

    json caps;
    caps["streaming"]         = config_.supportStreaming;
    caps["pushNotifications"] = false;
    card["capabilities"]      = std::move(caps);

    return card;
}

// ---------------------------------------------------------------------------
// Route setup
// ---------------------------------------------------------------------------

void A2aServer::setupRoutes() {
    using Handler = util::HttpServer::Handler;

    auto cardHandler = std::make_shared<Handler>(Handler(
        [this](util::HttpServer::Request& req, util::HttpServer::Response& resp, std::string_view)
            -> asio::awaitable<void> {
            co_await handleAgentCard(req, resp);
        }
    ));
    httpServer_->router().add(config_.agentCardPath, 0, cardHandler);

    auto a2aHandler = std::make_shared<Handler>(Handler(
        [this](util::HttpServer::Request& req, util::HttpServer::Response& resp, std::string_view)
            -> asio::awaitable<void> {
            co_await handleA2aRequest(req, resp);
        }
    ));
    httpServer_->router().add(config_.a2aEndpoint, 2, a2aHandler);

    httpServer_->addSseRoute(
        config_.sseEndpoint,
        [this](util::HttpServer::Request& req, std::shared_ptr<util::HttpServer::SseWriter> writer)
            -> asio::awaitable<void> {
            co_await handleSseRequest(req, writer);
        }
    );
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------

asio::awaitable<void> A2aServer::handleAgentCard(
    util::HttpServer::Request& /*req*/,
    util::HttpServer::Response& resp
) {
    writeJsonResponse(resp, boost::beast::http::status::ok, agentCard());
    co_return;
}

asio::awaitable<void>
    A2aServer::handleA2aRequest(util::HttpServer::Request& req, util::HttpServer::Response& resp) {
    auto versionIt = req.find("A2A-Version");
    if (versionIt != req.end()) {
        auto ver = versionIt->value();
        if (ver != kA2aVersion && ver != "0.3") {
            writeJsonResponse(
                resp,
                boost::beast::http::status::bad_request,
                makeVersionNotSupported(json(1), std::string(ver))
            );
            co_return;
        }
    }

    json request;
    try {
        request = json::parse(req.body());
    } catch (...) {
        writeJsonResponse(
            resp,
            boost::beast::http::status::ok,
            jsonRpcError(json(nullptr), -32700, "Parse error")
        );
        co_return;
    }

    auto result = processJsonRpc(request);
    writeJsonResponse(resp, boost::beast::http::status::ok, result);
    co_return;
}

asio::awaitable<void> A2aServer::handleSseRequest(
    util::HttpServer::Request& /*req*/,
    std::shared_ptr<util::HttpServer::SseWriter> writer
) {
    auto client    = std::make_shared<SSEClient>();
    client->writer = writer;
    {
        std::lock_guard<std::mutex> lock(sseClientsMutex_);
        sseClients_.push_back(client);
    }

    while (!client->closed && !stopped_.load(std::memory_order_acquire)) {
        co_await asio::steady_timer(
            co_await asio::this_coro::executor,
            std::chrono::milliseconds(100)
        )
            .async_wait(asio::use_awaitable);
    }

    {
        std::lock_guard<std::mutex> lock(sseClientsMutex_);
        sseClients_.erase(
            std::remove_if(
                sseClients_.begin(),
                sseClients_.end(),
                [&](const auto& c) {
                    return c.get() == client.get();
                }
            ),
            sseClients_.end()
        );
    }
    co_return;
}

// ---------------------------------------------------------------------------
// JSON-RPC dispatch
// ---------------------------------------------------------------------------

json A2aServer::processJsonRpc(const json& request) {
    if (!request.is_object()) {
        return jsonRpcError(json(nullptr), -32600, "Invalid Request");
    }

    auto jsonrpc = request.value("jsonrpc", std::string{});
    if (jsonrpc != "2.0") {
        return jsonRpcError(
            request.contains("id") ? request["id"] : json(nullptr),
            -32600,
            "Invalid Request: jsonrpc must be \"2.0\""
        );
    }

    auto method = request.value("method", std::string{});
    auto id     = request.contains("id") ? request["id"] : json(nullptr);
    auto params = request.value("params", json::object());

    if (method == "SendMessage") {
        return handleSendMessage(id, params);
    }
    if (method == "SendStreamingMessage") {
        return handleSendMessage(id, params);
    }
    if (method == "GetTask") {
        return handleGetTask(id, params);
    }
    if (method == "ListTasks") {
        return handleListTasks(id, params);
    }
    if (method == "CancelTask") {
        return handleCancelTask(id, params);
    }

    return jsonRpcError(id, -32601, "Method not found: " + method);
}

// ---------------------------------------------------------------------------
// Method handlers
// ---------------------------------------------------------------------------

json A2aServer::handleSendMessage(const json& id, const json& params) {
    if (!params.is_object() || !params.contains("message")) {
        return jsonRpcError(id, -32602, "Invalid params: message is required");
    }

    auto message = params["message"];
    if (!message.is_object() || !message.contains("parts")) {
        return jsonRpcError(id, -32602, "Invalid params: message.parts is required");
    }

    auto role = message.value("role", std::string{"ROLE_USER"});
    if (role != "ROLE_USER") {
        return jsonRpcError(id, -32602, "Invalid params: role must be ROLE_USER");
    }

    auto text = extractTextFromParts(message["parts"]);
    if (text.empty()) {
        return jsonRpcError(id, -32602, "Invalid params: message contains no text");
    }

    auto taskId    = message.value("taskId", std::string{});
    auto contextId = message.value("contextId", std::string{});

    std::shared_ptr<TaskRecord> task;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);

        if (!taskId.empty()) {
            auto it = tasks_.find(taskId);
            if (it == tasks_.end()) {
                return makeTaskNotFound(id, taskId);
            }
            task = it->second;
            if (isTerminalState(task->state)) {
                return makeUnsupportedOperation(
                    id,
                    "Cannot send message to task in terminal state"
                );
            }
        } else {
            taskId = generateId();
            if (contextId.empty()) {
                contextId = generateId();
            }
            task             = std::make_shared<TaskRecord>();
            task->id         = taskId;
            task->contextId  = contextId;
            task->state      = A2aTaskState::Submitted;
            task->createdAt  = currentTimestamp();
            task->updatedAt  = task->createdAt;
            task->cancelFlag = std::make_shared<std::atomic<bool>>(false);
            tasks_[taskId]   = task;
            pruneOldTasks();
        }

        json userMsg;
        userMsg["messageId"] = generateId();
        userMsg["role"]      = "ROLE_USER";
        userMsg["parts"]     = json::array({makeTextPart(text)});
        task->history.push_back(std::move(userMsg));
    }

    updateTaskState(taskId, A2aTaskState::Working);

    executeTask(taskId, text);

    auto result = json::object();
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        auto                        it = tasks_.find(taskId);
        if (it != tasks_.end()) {
            result["task"] = makeTask(it->second->id, it->second->contextId, it->second->state);
        }
    }
    return jsonRpcResult(id, std::move(result));
}

json A2aServer::handleGetTask(const json& id, const json& params) {
    auto taskId = params.value("id", std::string{});
    if (taskId.empty()) {
        return jsonRpcError(id, -32602, "Invalid params: id is required");
    }

    auto task = findTask(taskId);
    if (!task) {
        return makeTaskNotFound(id, taskId);
    }

    auto historyLength = params.value("historyLength", 0);

    json result;
    result["id"]        = task->id;
    result["contextId"] = task->contextId;
    result["status"]    = json{
           {"state",     taskStateToString(task->state)},
           {"timestamp", task->updatedAt               }
    };
    result["artifacts"] = task->artifacts;
    result["metadata"]  = task->metadata;

    if (historyLength > 0 && !task->history.empty()) {
        json hist  = json::array();
        auto start = task->history.size() > static_cast<size_t>(historyLength)
                         ? task->history.size() - historyLength
                         : 0;
        for (size_t i = start; i < task->history.size(); ++i) {
            hist.push_back(task->history[i]);
        }
        result["history"] = std::move(hist);
    }

    return jsonRpcResult(id, std::move(result));
}

json A2aServer::handleListTasks(const json& id, const json& params) {
    auto contextId    = params.value("contextId", std::string{});
    auto statusFilter = params.value("status", std::string{});
    auto pageSize     = params.value("pageSize", 50);
    if (pageSize < 1) {
        pageSize = 1;
    }
    if (pageSize > 100) {
        pageSize = 100;
    }

    std::vector<json> taskList;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        for (const auto& [tid, task] : tasks_) {
            if (!contextId.empty() && task->contextId != contextId) {
                continue;
            }
            if (!statusFilter.empty() && taskStateToString(task->state) != statusFilter) {
                continue;
            }
            json t;
            t["id"]        = task->id;
            t["contextId"] = task->contextId;
            t["status"]    = json{
                   {"state",     taskStateToString(task->state)},
                   {"timestamp", task->updatedAt               }
            };
            taskList.push_back(std::move(t));
        }
    }

    int  totalSize = static_cast<int>(taskList.size());
    json tasksArr  = json::array();
    for (auto& t : taskList) {
        tasksArr.push_back(std::move(t));
    }
    json result;
    result["tasks"]         = std::move(tasksArr);
    result["nextPageToken"] = "";
    result["pageSize"]      = pageSize;
    result["totalSize"]     = totalSize;

    return jsonRpcResult(id, std::move(result));
}

json A2aServer::handleCancelTask(const json& id, const json& params) {
    auto taskId = params.value("id", std::string{});
    if (taskId.empty()) {
        return jsonRpcError(id, -32602, "Invalid params: id is required");
    }

    std::shared_ptr<TaskRecord> task;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        auto                        it = tasks_.find(taskId);
        if (it == tasks_.end()) {
            return makeTaskNotFound(id, taskId);
        }
        task = it->second;
    }

    if (isTerminalState(task->state)) {
        return jsonRpcError(
            id,
            kA2aTaskNotCancelable,
            "Task is in terminal state and cannot be canceled"
        );
    }

    if (task->cancelFlag) {
        task->cancelFlag->store(true, std::memory_order_release);
    }
    updateTaskState(taskId, A2aTaskState::Canceled);

    return jsonRpcResult(id, makeTask(task->id, task->contextId, A2aTaskState::Canceled));
}

// ---------------------------------------------------------------------------
// Task execution
// ---------------------------------------------------------------------------

void A2aServer::executeTask(std::string_view taskId, std::string_view userInput) {
    std::thread worker([this, taskId = std::string(taskId), userInput = std::string(userInput)]() {
        auto task = findTask(taskId);
        if (!task) {
            return;
        }

        auto cancelFlag = task->cancelFlag;

        if (!deepAgent_) {
            updateTaskState(
                taskId,
                A2aTaskState::Failed,
                makeMessage("ROLE_AGENT", "Error: no agent configured")
            );
            return;
        }

        std::string threadId = fmt::format("a2a_{}", taskId);
        std::string collected;

        try {
            auto ioCtx = std::make_shared<asio::io_context>();
            asio::co_spawn(
                *ioCtx,
                [this, &threadId, &userInput, &collected, &cancelFlag]() -> asio::awaitable<void> {
                    std::vector<neograph::ChatMessage> msgs{
                        neograph::ChatMessage{"user", std::string{userInput}}
                    };
                    auto result = co_await deepAgent_->runNonStreamAsync(
                        threadId,
                        msgs,
                        [&collected, &cancelFlag](const neograph::graph::GraphEvent& event) {
                            if (event.type == neograph::graph::GraphEvent::Type::LLM_TOKEN) {
                                if (event.data.is_string()) {
                                    collected += event.data.get<std::string>();
                                } else if (event.data.is_object()) {
                                    neograph::ChatStreamChunk chunk;
                                    neograph::from_json(event.data, chunk);
                                    if (chunk.type != neograph::ChatStreamChunk::TYPE_THINKING) {
                                        collected += chunk.data;
                                    }
                                }
                            }
                        },
                        ""
                    );
                    collected = result;
                    co_return;
                },
                asio::detached
            );
            ioCtx->run();
        } catch (const std::exception& e) {
            if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                updateTaskState(taskId, A2aTaskState::Canceled);
            } else {
                updateTaskState(
                    taskId,
                    A2aTaskState::Failed,
                    makeMessage("ROLE_AGENT", std::string("Error: ") + e.what())
                );
            }
            return;
        }

        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            updateTaskState(taskId, A2aTaskState::Canceled);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(tasksMutex_);
            auto                        it = tasks_.find(taskId);
            if (it != tasks_.end()) {
                json agentMsg;
                agentMsg["messageId"] = generateId();
                agentMsg["role"]      = "ROLE_AGENT";
                agentMsg["parts"]     = json::array({makeTextPart(collected)});
                it->second->history.push_back(std::move(agentMsg));

                json artifact;
                artifact["artifactId"] = generateId();
                artifact["name"]       = "response";
                artifact["parts"]      = json::array({makeTextPart(collected)});
                it->second->artifacts.push_back(std::move(artifact));
            }
        }

        updateTaskState(taskId, A2aTaskState::Completed);

        json event;
        event["taskId"]    = taskId;
        event["contextId"] = task->contextId;
        event["status"]    = json{
               {"state",     taskStateToString(A2aTaskState::Completed)},
               {"timestamp", currentTimestamp()                        }
        };
        json sseMsg;
        sseMsg["jsonrpc"] = "2.0";
        sseMsg["id"]      = json(nullptr);
        sseMsg["result"]  = json{
             {"statusUpdate", std::move(event)}
        };
        broadcastSSE(sseMsg.dump());
    });
    {
        std::lock_guard<std::mutex> lock(workersMutex_);
        // 清理已完成的 worker
        workers_.erase(
            std::remove_if(
                workers_.begin(),
                workers_.end(),
                [](std::thread& t) {
                    if (t.joinable()) {
                        return false;
                    }
                    return true;
                }
            ),
            workers_.end()
        );
        workers_.push_back(std::move(worker));
    }
}

// ---------------------------------------------------------------------------
// SSE
// ---------------------------------------------------------------------------

void A2aServer::broadcastSSE(std::string_view data) {
    std::lock_guard<std::mutex> lock(sseClientsMutex_);
    for (auto& client : sseClients_) {
        if (!client->closed && client->writer) {
            client->writer->writeEvent("message", data);
        }
    }
}

void A2aServer::stopSSE() {
    std::lock_guard<std::mutex> lock(sseClientsMutex_);
    for (auto& client : sseClients_) {
        client->closed = true;
        if (client->writer) {
            client->writer->close();
        }
    }
    sseClients_.clear();
}

// ---------------------------------------------------------------------------
// Task store helpers
// ---------------------------------------------------------------------------

std::shared_ptr<A2aServer::TaskRecord> A2aServer::findTask(std::string_view taskId) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto                        it = tasks_.find(taskId);
    return it != tasks_.end() ? it->second : nullptr;
}

void A2aServer::updateTaskState(
    std::string_view taskId,
    A2aTaskState     state,
    const json&      statusMsg
) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto                        it = tasks_.find(taskId);
    if (it == tasks_.end()) {
        return;
    }
    it->second->state     = state;
    it->second->updatedAt = currentTimestamp();
    if (!statusMsg.is_null()) {
        json status;
        status["state"]     = taskStateToString(state);
        status["message"]   = statusMsg;
        status["timestamp"] = it->second->updatedAt;
    }
}

void A2aServer::pruneOldTasks() {
    if (tasks_.size() <= config_.maxTasks) {
        return;
    }
    auto excess = tasks_.size() - config_.maxTasks;
    for (auto it = tasks_.begin(); it != tasks_.end() && excess > 0;) {
        if (isTerminalState(it->second->state)) {
            it = tasks_.erase(it);
            --excess;
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------

void A2aServer::writeJsonResponse(
    util::HttpServer::Response& resp,
    boost::beast::http::status  status,
    const json&                 body
) {
    resp.result(status);
    resp.set(boost::beast::http::field::content_type, "application/json");
    resp.body() = body.dump();
    resp.prepare_payload();
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

json A2aServer::jsonRpcResult(const json& id, json result) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    resp["result"]  = std::move(result);
    return resp;
}

json A2aServer::jsonRpcError(const json& id, int code, std::string_view msg) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    resp["error"]   = json{
          {"code",    code},
          {"message", msg }
    };
    return resp;
}

json A2aServer::makeTaskNotFound(const json& id, std::string_view taskId) {
    return jsonRpcError(id, kA2aTaskNotFound, fmt::format("Task not found: {}", taskId));
}

json A2aServer::makeUnsupportedOperation(const json& id, std::string_view detail) {
    return jsonRpcError(id, kA2aUnsupportedOperation, detail);
}

json A2aServer::makeVersionNotSupported(const json& id, std::string_view version) {
    return jsonRpcError(id, kA2aVersionNotSupported, fmt::format("A2A version not supported: {}", version));
}

std::string A2aServer::generateId() {
    static std::atomic<uint64_t>                   counter{0};
    static std::random_device                      rd;
    static std::mt19937_64                         gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;

    auto ts  = std::chrono::steady_clock::now().time_since_epoch().count();
    auto rnd = dist(gen);
    auto cnt = counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream oss;
    oss << std::hex << ts << "-" << rnd << "-" << cnt;
    return oss.str();
}

std::string A2aServer::currentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
    gmtime_r(&tt, &tm);

    char buf[32];
    std::snprintf(
        buf,
        sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        static_cast<int>(ms.count())
    );
    return buf;
}

json A2aServer::makeTextPart(std::string_view text) {
    return json{
        {"text", text}
    };
}

json A2aServer::makeMessage(std::string_view role, std::string_view text) {
    json msg;
    msg["messageId"] = generateId();
    msg["role"]      = role;
    msg["parts"]     = json::array({makeTextPart(text)});
    return msg;
}

json A2aServer::makeTask(
    std::string_view id,
    std::string_view contextId,
    A2aTaskState     state,
    const json&      statusMessage
) {
    json task;
    task["id"]        = id;
    task["contextId"] = contextId;

    json status;
    status["state"]     = taskStateToString(state);
    status["timestamp"] = currentTimestamp();
    if (!statusMessage.is_null()) {
        status["message"] = statusMessage;
    }
    task["status"] = std::move(status);

    return task;
}

std::string A2aServer::extractTextFromParts(const json& parts) {
    std::string result;
    if (!parts.is_array()) {
        return result;
    }
    for (const auto& part : parts) {
        if (part.is_object() && part.contains("text") && part["text"].is_string()) {
            if (!result.empty()) {
                result += "\n";
            }
            result += part["text"].get<std::string>();
        }
    }
    return result;
}

} // namespace server
} // namespace agentxx
