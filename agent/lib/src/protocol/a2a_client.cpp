#include "agentxx/protocol/a2a_client.h"

#include "agentxx/util/log.h"

namespace agentxx {
namespace server {

A2aClient::A2aClient(Config config) :
    config_(std::move(config)) {}

// ---------------------------------------------------------------------------
// Agent discovery
// ---------------------------------------------------------------------------

asio::awaitable<std::expected<json, std::string>> A2aClient::fetchAgentCard() {
    auto url  = config_.baseUrl + config_.agentCardPath;
    auto resp = co_await util::HttpClient::getAsync(
        url,
        util::HeaderMap{},
        util::RequestConfig{
            .readTimeout = config_.requestTimeout,
        }
    );
    if (!resp.has_value()) {
        co_return std::unexpected("Failed to fetch agent card: " + resp.error());
    }
    if (!resp->isSuccess()) {
        co_return std::unexpected(
            "Failed to fetch agent card: HTTP " + std::to_string(resp->status)
        );
    }
    auto parsed = resp->bodyJson();
    if (!parsed.has_value()) {
        co_return std::unexpected("Failed to parse agent card JSON");
    }
    co_return std::move(*parsed);
}

// ---------------------------------------------------------------------------
// Core operations
// ---------------------------------------------------------------------------

asio::awaitable<std::expected<json, std::string>> A2aClient::sendMessage(
    std::string_view text,
    std::string_view taskId,
    std::string_view contextId
) {
    json params;
    params["message"] = buildTextMessage(text, taskId, contextId);
    co_return co_await rpcCall("SendMessage", std::move(params));
}

asio::awaitable<std::expected<json, std::string>>
    A2aClient::getTask(std::string_view taskId, int historyLength) {
    json params;
    params["id"] = taskId;
    if (historyLength > 0) {
        params["historyLength"] = historyLength;
    }
    co_return co_await rpcCall("GetTask", std::move(params));
}

asio::awaitable<std::expected<json, std::string>>
    A2aClient::listTasks(std::string_view contextId, std::string_view status, int pageSize) {
    json params;
    if (!contextId.empty()) {
        params["contextId"] = contextId;
    }
    if (!status.empty()) {
        params["status"] = status;
    }
    params["pageSize"] = pageSize;
    co_return co_await rpcCall("ListTasks", std::move(params));
}

asio::awaitable<std::expected<json, std::string>> A2aClient::cancelTask(std::string_view taskId) {
    json params;
    params["id"] = taskId;
    co_return co_await rpcCall("CancelTask", std::move(params));
}

// ---------------------------------------------------------------------------
// Low-level JSON-RPC
// ---------------------------------------------------------------------------

asio::awaitable<std::expected<json, std::string>>
    A2aClient::rpcCall(std::string_view method, json params) {
    json request;
    request["jsonrpc"] = "2.0";
    request["id"]      = nextId_++;
    request["method"]  = method;
    request["params"]  = std::move(params);

    auto url = config_.baseUrl + config_.a2aEndpoint;

    util::HeaderMap headers;
    headers.set("A2A-Version", config_.protocolVersion);

    auto resp = co_await util::HttpClient::postAsync(
        url,
        request.dump(),
        "application/json",
        headers,
        util::RequestConfig{
            .readTimeout = config_.requestTimeout,
        }
    );

    if (!resp.has_value()) {
        co_return std::unexpected("HTTP request failed: " + resp.error());
    }
    if (!resp->isSuccess()) {
        co_return std::unexpected("HTTP error " + std::to_string(resp->status) + ": " + resp->body);
    }

    auto parsed = resp->bodyJson();
    if (!parsed.has_value()) {
        co_return std::unexpected("Failed to parse response JSON");
    }
    auto& response = *parsed;

    if (response.contains("error") && !response["error"].is_null()) {
        auto code = response["error"].value("code", 0);
        auto msg  = response["error"].value("message", std::string{"Unknown error"});
        co_return std::unexpected("JSON-RPC error " + std::to_string(code) + ": " + msg);
    }

    if (!response.contains("result")) {
        co_return std::unexpected("Response missing result field");
    }

    co_return response["result"];
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

json A2aClient::buildTextMessage(
    std::string_view text,
    std::string_view taskId,
    std::string_view contextId
) {
    json msg;
    msg["role"]  = "ROLE_USER";
    msg["parts"] = json::array({json{{"text", text}}});
    if (!taskId.empty()) {
        msg["taskId"] = taskId;
    }
    if (!contextId.empty()) {
        msg["contextId"] = contextId;
    }
    return msg;
}

std::string A2aClient::extractTaskId(const json& sendMessageResult) {
    if (sendMessageResult.contains("task") && sendMessageResult["task"].contains("id")) {
        return sendMessageResult["task"]["id"].get<std::string>();
    }
    return "";
}

std::string A2aClient::extractTaskState(const json& task) {
    if (task.contains("status") && task["status"].contains("state")) {
        return task["status"]["state"].get<std::string>();
    }
    return "TASK_STATE_UNSPECIFIED";
}

std::string A2aClient::extractArtifactText(const json& task) {
    std::string result;
    if (!task.contains("artifacts") || !task["artifacts"].is_array()) {
        return result;
    }
    for (const auto& artifact : task["artifacts"]) {
        if (!artifact.contains("parts") || !artifact["parts"].is_array()) {
            continue;
        }
        for (const auto& part : artifact["parts"]) {
            if (part.contains("text") && part["text"].is_string()) {
                if (!result.empty()) {
                    result += "\n";
                }
                result += part["text"].get<std::string>();
            }
        }
    }
    return result;
}

} // namespace server
} // namespace agentxx
