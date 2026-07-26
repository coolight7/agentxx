#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <asio/awaitable.hpp>
#include <neograph/json.h>

#include "agentxx/util/http_client.h"

namespace agentxx {
namespace server {

using json = neograph::json;

// ---------------------------------------------------------------------------
// A2A Client
//
// Connects to a remote A2A server via the JSON-RPC binding:
//   - Fetches Agent Card for discovery
//   - Sends messages (sync)
//   - Gets / lists / cancels tasks
// ---------------------------------------------------------------------------

class A2aClient {
public:

    struct Config {
        std::string          baseUrl;
        std::string          a2aEndpoint   = "/a2a";
        std::string          agentCardPath = "/.well-known/agent-card.json";
        std::chrono::seconds requestTimeout{60};
        std::string          protocolVersion = "1.0";
    };

    explicit A2aClient(Config config);

    A2aClient(const A2aClient&)            = delete;
    A2aClient& operator=(const A2aClient&) = delete;

    // -----------------------------------------------------------------------
    // Agent discovery
    // -----------------------------------------------------------------------

    asio::awaitable<std::expected<json, std::string>> fetchAgentCard();

    // -----------------------------------------------------------------------
    // Core operations
    // -----------------------------------------------------------------------

    asio::awaitable<std::expected<json, std::string>> sendMessage(
        std::string_view text,
        std::string_view taskId    = "",
        std::string_view contextId = ""
    );

    asio::awaitable<std::expected<json, std::string>>
        getTask(std::string_view taskId, int historyLength = 0);

    asio::awaitable<std::expected<json, std::string>>
        listTasks(std::string_view contextId = "", std::string_view status = "", int pageSize = 50);

    asio::awaitable<std::expected<json, std::string>> cancelTask(std::string_view taskId);

    // -----------------------------------------------------------------------
    // Low-level JSON-RPC call (public for testing)
    // -----------------------------------------------------------------------

    asio::awaitable<std::expected<json, std::string>> rpcCall(std::string_view method, json params);

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    static json buildTextMessage(
        std::string_view text,
        std::string_view taskId    = "",
        std::string_view contextId = ""
    );

    static std::string extractTaskId(const json& sendMessageResult);
    static std::string extractTaskState(const json& task);
    static std::string extractArtifactText(const json& task);

private:

    Config  config_;
    int64_t nextId_ = 1;
};

} // namespace server
} // namespace agentxx
