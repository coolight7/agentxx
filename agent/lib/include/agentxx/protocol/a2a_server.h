#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <asio/awaitable.hpp>
#include <neograph/json.h>

#include "agentxx/agent/base_agent.h"
#include "agentxx/util/http_server.h"

namespace agentxx {
namespace server {

using json = neograph::json;

// ---------------------------------------------------------------------------
// A2A Protocol Data Model (v1.0)
// ---------------------------------------------------------------------------

inline constexpr const char* kA2aVersion = "1.0";

enum class A2aTaskState {
    Unspecified,
    Submitted,
    Working,
    Completed,
    Failed,
    Canceled,
    InputRequired,
    Rejected,
    AuthRequired,
};

inline std::string taskStateToString(A2aTaskState s) {
    switch (s) {
        case A2aTaskState::Submitted:
            return "TASK_STATE_SUBMITTED";
        case A2aTaskState::Working:
            return "TASK_STATE_WORKING";
        case A2aTaskState::Completed:
            return "TASK_STATE_COMPLETED";
        case A2aTaskState::Failed:
            return "TASK_STATE_FAILED";
        case A2aTaskState::Canceled:
            return "TASK_STATE_CANCELED";
        case A2aTaskState::InputRequired:
            return "TASK_STATE_INPUT_REQUIRED";
        case A2aTaskState::Rejected:
            return "TASK_STATE_REJECTED";
        case A2aTaskState::AuthRequired:
            return "TASK_STATE_AUTH_REQUIRED";
        default:
            return "TASK_STATE_UNSPECIFIED";
    }
}

inline A2aTaskState taskStateFromString(std::string_view s) {
    if (s == "TASK_STATE_SUBMITTED") {
        return A2aTaskState::Submitted;
    }
    if (s == "TASK_STATE_WORKING") {
        return A2aTaskState::Working;
    }
    if (s == "TASK_STATE_COMPLETED") {
        return A2aTaskState::Completed;
    }
    if (s == "TASK_STATE_FAILED") {
        return A2aTaskState::Failed;
    }
    if (s == "TASK_STATE_CANCELED") {
        return A2aTaskState::Canceled;
    }
    if (s == "TASK_STATE_INPUT_REQUIRED") {
        return A2aTaskState::InputRequired;
    }
    if (s == "TASK_STATE_REJECTED") {
        return A2aTaskState::Rejected;
    }
    if (s == "TASK_STATE_AUTH_REQUIRED") {
        return A2aTaskState::AuthRequired;
    }
    return A2aTaskState::Unspecified;
}

inline bool isTerminalState(A2aTaskState s) {
    return s == A2aTaskState::Completed || s == A2aTaskState::Failed || s == A2aTaskState::Canceled
           || s == A2aTaskState::Rejected;
}

// A2A-specific JSON-RPC error codes
inline constexpr int kA2aTaskNotFound                 = -32001;
inline constexpr int kA2aTaskNotCancelable            = -32002;
inline constexpr int kA2aPushNotificationNotSupported = -32003;
inline constexpr int kA2aUnsupportedOperation         = -32004;
inline constexpr int kA2aContentTypeNotSupported      = -32005;
inline constexpr int kA2aInvalidAgentResponse         = -32006;
inline constexpr int kA2aVersionNotSupported          = -32009;

// ---------------------------------------------------------------------------
// A2A Server
//
// Implements the A2A protocol (v1.0) JSON-RPC binding over HTTP:
//   GET  /.well-known/agent-card.json  – Agent Card discovery
//   POST /a2a                          – JSON-RPC endpoint
//   GET  /a2a/sse                      – SSE streaming endpoint
//
// Core methods:
//   SendMessage, SendStreamingMessage, GetTask, ListTasks, CancelTask
// ---------------------------------------------------------------------------

class A2aServer {
public:

    struct SkillDef {
        std::string              id;
        std::string              name;
        std::string              description;
        std::vector<std::string> tags;
        std::vector<std::string> examples;
    };

    struct Config {
        util::HttpServer::Config httpConfig;
        std::string              a2aEndpoint   = "/a2a";
        std::string              sseEndpoint   = "/a2a/sse";
        std::string              agentCardPath = "/.well-known/agent-card.json";
        std::string              serverName    = "agentxx-a2a";
        std::string              serverVersion = "1.0.0";
        std::string              description   = "Agentxx A2A Server";
        std::vector<std::string> inputModes    = {"text/plain"};
        std::vector<std::string> outputModes   = {"text/plain", "application/json"};
        std::vector<SkillDef>    skills;
        bool                     supportStreaming = true;
        size_t                   maxTasks         = 10000;
        std::chrono::seconds     taskTimeout{300};
    };

    explicit A2aServer(std::shared_ptr<agentxx::agent::BaseAgent> agent, Config config);

    A2aServer(const A2aServer&)            = delete;
    A2aServer& operator=(const A2aServer&) = delete;

    ~A2aServer();

    void                  start();
    asio::awaitable<void> stop();

    uint16_t port() const;
    bool     isStopped() const;

    json agentCard() const;

    // -----------------------------------------------------------------------
    // JSON-RPC helpers (public for testing)
    // -----------------------------------------------------------------------

    static json jsonRpcResult(const json& id, json result);
    static json jsonRpcError(const json& id, int code, std::string_view msg);
    static json makeTaskNotFound(const json& id, std::string_view taskId);
    static json makeUnsupportedOperation(const json& id, std::string_view detail);
    static json makeVersionNotSupported(const json& id, std::string_view version);

    static std::string generateId();
    static std::string currentTimestamp();

    static json makeTextPart(std::string_view text);
    static json makeMessage(std::string_view role, std::string_view text);
    static json makeTask(
        std::string_view id,
        std::string_view contextId,
        A2aTaskState     state,
        const json&      statusMessage = json()
    );

    static std::string extractTextFromParts(const json& parts);

private:

    struct TaskRecord {
        std::string                        id;
        std::string                        contextId;
        A2aTaskState                       state     = A2aTaskState::Submitted;
        json                               history   = json::array();
        json                               artifacts = json::array();
        json                               metadata  = json::object();
        std::string                        createdAt;
        std::string                        updatedAt;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
    };

    // -----------------------------------------------------------------------
    // Route setup
    // -----------------------------------------------------------------------

    void setupRoutes();

    // -----------------------------------------------------------------------
    // HTTP handlers
    // -----------------------------------------------------------------------

    asio::awaitable<void>
        handleAgentCard(util::HttpServer::Request& req, util::HttpServer::Response& resp);

    asio::awaitable<void>
        handleA2aRequest(util::HttpServer::Request& req, util::HttpServer::Response& resp);

    asio::awaitable<void> handleSseRequest(
        util::HttpServer::Request&                   req,
        std::shared_ptr<util::HttpServer::SseWriter> writer
    );

    // -----------------------------------------------------------------------
    // JSON-RPC dispatch
    // -----------------------------------------------------------------------

    json processJsonRpc(const json& request);

    json handleSendMessage(const json& id, const json& params);
    json handleGetTask(const json& id, const json& params);
    json handleListTasks(const json& id, const json& params);
    json handleCancelTask(const json& id, const json& params);

    // -----------------------------------------------------------------------
    // Task execution (async worker)
    // -----------------------------------------------------------------------

    void executeTask(std::string_view taskId, std::string_view userInput);

    // -----------------------------------------------------------------------
    // SSE broadcast
    // -----------------------------------------------------------------------

    asio::awaitable<void> broadcastSSE(std::string data);
    asio::awaitable<void> stopSSE();

    // -----------------------------------------------------------------------
    // Task store helpers
    // -----------------------------------------------------------------------

    std::shared_ptr<TaskRecord> findTask(std::string_view taskId);
    void                        updateTaskState(
                               std::string_view taskId,
                               A2aTaskState     state,
                               const json&      statusMsg = json()
                           );
    void pruneOldTasks();

    // -----------------------------------------------------------------------
    // Response helpers
    // -----------------------------------------------------------------------

    void writeJsonResponse(
        util::HttpServer::Response& resp,
        boost::beast::http::status  status,
        const json&                 body
    );

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    Config                                     config_;
    std::shared_ptr<agentxx::agent::BaseAgent> agent_;
    std::unique_ptr<util::HttpServer>          httpServer_;

    mutable std::mutex                                              tasksMutex_;
    std::map<std::string, std::shared_ptr<TaskRecord>, std::less<>> tasks_;

    struct SSEClient {
        std::shared_ptr<util::HttpServer::SseWriter> writer;
        std::atomic<bool>                            closed{false};
    };

    std::mutex                              sseClientsMutex_;
    std::vector<std::shared_ptr<SSEClient>> sseClients_;

    /// worker 线程句柄: done 标志用于在运行期回收已结束的线程, 避免 workers_ 无限增长
    struct WorkerHandle {
        std::thread                        thread;
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    };

    std::mutex                workersMutex_;
    std::vector<WorkerHandle> workers_;

    /// 串行化对共享 BaseAgent 的访问: BaseAgent 设计为单线程/多协程交错执行,
    /// 多个 worker 线程并发驱动同一 engine 会产生数据竞争, 故以互斥锁序列化任务执行
    std::mutex agentRunMutex_;

    std::atomic<bool> stopped_{false};
};

} // namespace server
} // namespace agentxx
