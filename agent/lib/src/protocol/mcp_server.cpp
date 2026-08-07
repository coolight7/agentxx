#include "agentxx/protocol/mcp_server.h"

#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include <algorithm>
#include <iostream>

namespace agentxx {
namespace server {

McpServer::McpServer() :
    config_(),
    httpServer_(std::make_unique<util::HttpServer>(util::HttpServer::Config{})) {
    setupRoutes();
}

McpServer::McpServer(Config config) :
    config_(std::move(config)) {
    httpServer_ = std::make_unique<util::HttpServer>(config_.httpConfig);
    setupRoutes();
}

McpServer::~McpServer() {
    stop();
}

void McpServer::start() {
    httpServer_->start();
}

void McpServer::stop() {
    stopSSE();
    httpServer_->stop();
}

void McpServer::runStdio() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        json requestJson;
        bool parsed = agentxx::util::catchError<bool>(
            [&]() -> bool {
                requestJson = json::parse(line);
                return true;
            },
            [&](std::string errmsg) -> bool {
                json errorResp = jsonRpcErrorResponse(
                    json{nullptr},
                    jsonRpcError(kJsonRpcParseError, std::string("Parse error: ") + errmsg)
                );
                std::cout << errorResp.dump() << "\n" << std::flush;
                return false;
            }
        );
        if (!parsed) {
            continue;
        }

        json response = processJsonRpc(requestJson);
        if (!response.is_null()) {
            std::cout << response.dump() << "\n" << std::flush;
        }
    }
}

uint16_t McpServer::port() const {
    return httpServer_->port();
}

size_t McpServer::activeConnections() const {
    return httpServer_->activeConnections();
}

bool McpServer::isStopped() const {
    return httpServer_->isStopped();
}

void McpServer::addTool(McpToolDefinition def, ToolHandler handler) {
    const auto       name = def.name;
    std::unique_lock lock(toolsMutex_);
    toolsByName_[name] = ToolEntry{std::move(def), std::move(handler)};
    toolsListChanged_  = true;
}

void McpServer::removeTool(std::string_view name) {
    std::unique_lock lock(toolsMutex_);
    toolsByName_.erase(std::string{name});
    toolsListChanged_ = true;
}

std::vector<McpToolDefinition> McpServer::listTools() const {
    std::shared_lock               lock(toolsMutex_);
    std::vector<McpToolDefinition> result;
    result.reserve(toolsByName_.size());
    for (const auto& [key, entry] : toolsByName_) {
        result.push_back(entry.def);
    }
    return result;
}

void McpServer::addResource(McpResourceDefinition def, ResourceReader reader) {
    const auto       uri = def.uri;
    std::unique_lock lock(resourcesMutex_);
    resourcesByUri_[uri]  = ResourceEntry{std::move(def), std::move(reader)};
    resourcesListChanged_ = true;
}

void McpServer::removeResource(std::string_view uri) {
    std::unique_lock lock(resourcesMutex_);
    resourcesByUri_.erase(std::string{uri});
    resourcesListChanged_ = true;
}

std::vector<McpResourceDefinition> McpServer::listResources() const {
    std::shared_lock                   lock(resourcesMutex_);
    std::vector<McpResourceDefinition> result;
    result.reserve(resourcesByUri_.size());
    for (const auto& [key, entry] : resourcesByUri_) {
        result.push_back(entry.def);
    }
    return result;
}

void McpServer::addPrompt(McpPromptDefinition def, PromptHandler handler) {
    const auto       name = def.name;
    std::unique_lock lock(promptsMutex_);
    promptsByName_[name] = PromptEntry{std::move(def), std::move(handler)};
    promptsListChanged_  = true;
}

void McpServer::removePrompt(std::string_view name) {
    std::unique_lock lock(promptsMutex_);
    promptsByName_.erase(std::string{name});
    promptsListChanged_ = true;
}

std::vector<McpPromptDefinition> McpServer::listPrompts() const {
    std::shared_lock                 lock(promptsMutex_);
    std::vector<McpPromptDefinition> result;
    result.reserve(promptsByName_.size());
    for (const auto& [key, entry] : promptsByName_) {
        result.push_back(entry.def);
    }
    return result;
}

void McpServer::setCapabilities(Capabilities caps) {
    capabilities_ = caps;
}

const McpServer::Capabilities& McpServer::capabilities() const {
    return capabilities_;
}

bool McpServer::isAcceptValid(std::string_view accept) {
    if (accept.empty() || accept == "*/*") {
        return true;
    }
    bool hasJson = accept.find("application/json") != std::string_view::npos;
    bool hasSse  = accept.find("text/event-stream") != std::string_view::npos;
    return hasJson || hasSse;
}

bool McpServer::prefersSse(std::string_view accept) {
    if (accept.empty()) {
        return false;
    }
    bool hasSse = accept.find("text/event-stream") != std::string_view::npos;
    if (!hasSse) {
        return false;
    }
    bool hasJson = accept.find("application/json") != std::string_view::npos;
    bool isStar  = accept == "*/*";
    return hasSse && !hasJson && !isStar;
}

void McpServer::setupRoutes() {
    using Handler = util::HttpServer::Handler;

    auto mcpHandler = std::make_shared<Handler>(Handler(
        [this](util::HttpServer::Request& req, util::HttpServer::Response& resp, std::string_view)
            -> asio::awaitable<void> {
            co_await handleMcpRequest(req, resp);
        }
    ));
    httpServer_->router().add(config_.mcpEndpoint, 2, mcpHandler);

    httpServer_->addSseRoute(
        config_.sseEndpoint,
        [this](util::HttpServer::Request& req, std::shared_ptr<util::HttpServer::SseWriter> writer)
            -> asio::awaitable<void> {
            co_await handleSseStream(req, writer);
        }
    );
}

json McpServer::processJsonRpc(const json& requestJson) {
    if (!requestJson.is_object()) {
        return jsonRpcErrorResponse(
            json{nullptr},
            jsonRpcError(kJsonRpcInvalidRequest, "Request must be a JSON object")
        );
    }
    auto jrpc         = requestJson.value("jsonrpc", json{});
    bool validJsonRpc = false;
    if (jrpc.is_string() && jrpc.get<std::string>() == "2.0") {
        validJsonRpc = true;
    } else if (jrpc.is_number() && jrpc.get<double>() == 2.0) {
        validJsonRpc = true;
    }
    if (requestJson.contains("jsonrpc") && !validJsonRpc) {
        return jsonRpcErrorResponse(
            json{nullptr},
            jsonRpcError(kJsonRpcInvalidRequest, "Unsupported JSON-RPC version")
        );
    }

    std::string method = requestJson.value("method", "");
    bool        hasId  = requestJson.contains("id") && !requestJson["id"].is_null();
    json        id     = hasId ? requestJson["id"] : json{};
    json        params = requestJson.contains("params") ? requestJson["params"] : json::object();

    json meta;
    if (params.contains("_meta") && params["_meta"].is_object()) {
        meta = params["_meta"];
    }

    if (method == "notifications/initialized") {
        handleInitialized(params);
        return hasId ? jsonRpcResponse(id, json::object()) : json{};
    }
    if (method == "notifications/cancelled") {
        return hasId ? jsonRpcResponse(id, json::object()) : json{};
    }
    if (method == "notifications/message") {
        return json{};
    }
    if (method.empty()) {
        return jsonRpcErrorResponse(
            json{nullptr},
            jsonRpcError(kJsonRpcInvalidRequest, "Missing method")
        );
    }

    json response;
    // 分发异常统一转为 JSON-RPC 内部错误响应
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            if (method == "initialize") {
                response = handleInitialize(id, params);
            } else if (method == "ping") {
                response = handlePing(id);
            } else if (method == "tools/list") {
                response = handleToolsList(id, params);
            } else if (method == "tools/call") {
                response = handleToolsCall(id, params);
            } else if (method == "resources/list") {
                response = handleResourcesList(id, params);
            } else if (method == "resources/read") {
                response = handleResourcesRead(id, params);
            } else if (method == "resources/subscribe") {
                response = handleResourcesSubscribe(id, params);
            } else if (method == "resources/unsubscribe") {
                response = handleResourcesUnsubscribe(id, params);
            } else if (method == "resources/templates/list") {
                response = handleResourceTemplatesList(id, params);
            } else if (method == "prompts/list") {
                response = handlePromptsList(id, params);
            } else if (method == "prompts/get") {
                response = handlePromptsGet(id, params);
            } else if (method == "logging/setLevel") {
                response = handleLoggingSetLevel(id, params);
            } else if (method == "completion/complete") {
                response = handleComplete(id, params);
            } else {
                response = jsonRpcErrorResponse(
                    id,
                    jsonRpcError(kJsonRpcMethodNotFound, std::string("Method not found: ") + method)
                );
            }
            if (!response.is_null() && !meta.is_null() && response.contains("result")) {
                if (!response["result"].contains("_meta")) {
                    response["result"]["_meta"] = meta;
                }
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("[mcp] Handler error [{}]: {}", method, errmsg);
            response = jsonRpcErrorResponse(
                id,
                jsonRpcError(kJsonRpcInternalError, std::string("Internal error: ") + errmsg)
            );
            return false;
        }
    );

    if (!hasId) {
        return json{};
    }
    return response;
}

asio::awaitable<void>
    McpServer::handleMcpRequest(util::HttpServer::Request& req, util::HttpServer::Response& resp) {
    namespace http = boost::beast::http;

    auto accept = req[http::field::accept];
    if (!isAcceptValid(accept)) {
        json errorResp = jsonRpcErrorResponse(
            json{nullptr},
            jsonRpcError(
                -32000,
                "Not Acceptable: Client must accept both application/json "
                "and text/event-stream"
            )
        );
        resp.version(req.version());
        resp.result(http::status::not_acceptable);
        resp.set(http::field::content_type, "application/json");
        resp.body() = errorResp.dump();
        resp.prepare_payload();
        co_return;
    }

    json requestJson;
    bool parsed = agentxx::util::catchError<bool>(
        [&]() -> bool {
            requestJson = json::parse(req.body());
            return true;
        },
        [&](std::string errmsg) -> bool {
            auto errorResp = jsonRpcErrorResponse(
                json{nullptr},
                jsonRpcError(kJsonRpcParseError, std::string("Parse error: ") + errmsg)
            );
            writeJsonResponse(resp, http::status::bad_request, errorResp);
            return false;
        }
    );
    if (!parsed) {
        co_return;
    }

    json response = processJsonRpc(requestJson);
    if (response.is_null()) {
        resp.result(http::status::accepted);
        co_return;
    }

    if (prefersSse(accept)) {
        resp.version(req.version());
        resp.result(http::status::ok);
        resp.set(http::field::content_type, "text/event-stream");
        resp.set(http::field::cache_control, "no-cache");
        resp.set("X-Accel-Buffering", "no");
        std::string sseBody = fmt::format("event: message\ndata: {}\n\n", response.dump());
        resp.body()         = std::move(sseBody);
        resp.prepare_payload();
        co_return;
    }

    writeJsonResponse(resp, http::status::ok, response);
}

json McpServer::handleInitialize(const json& id, const json& params) {
    std::string clientVersion
        = params.value("protocolVersion", std::string{kMcpProtocol2024_11_05});

    std::string negotiatedVersion{kMcpProtocol2024_11_05};
    bool        foundExact = false;
    for (const auto& sv : kMcpSupportedProtocols) {
        if (sv == clientVersion) {
            negotiatedVersion = sv;
            foundExact        = true;
            break;
        }
    }
    if (!foundExact && !clientVersion.empty()) {
        auto clientYear = clientVersion.substr(0, 4);
        for (const auto& sv : kMcpSupportedProtocols) {
            auto svYear = sv.substr(0, 4);
            if (svYear <= clientYear) {
                negotiatedVersion = sv;
                break;
            }
        }
    }

    json capabilities;
    capabilities["experimental"] = json::object();
    if (capabilities_.tools) {
        capabilities["tools"] = json::object();
    }
    if (capabilities_.resources) {
        capabilities["resources"] = json::object();
    }
    if (capabilities_.prompts) {
        capabilities["prompts"] = json::object();
    }
    if (capabilities_.logging) {
        capabilities["logging"] = json::object();
    }
    capabilities["tasks"] = json::object();

    json serverInfo;
    serverInfo["name"]          = config_.serverName;
    serverInfo["version"]       = config_.serverVersion;
    serverInfo["title"]         = config_.serverName;
    capabilities["elicitation"] = json::object();

    json result;
    result["protocolVersion"] = negotiatedVersion;
    result["capabilities"]    = std::move(capabilities);
    result["serverInfo"]      = std::move(serverInfo);
    result["instructions"]    = "MCP server powered by agentxx";

    return jsonRpcResponse(id, std::move(result));
}

json McpServer::handlePing(const json& id) {
    return jsonRpcResponse(id, json::object());
}

json McpServer::handleToolsList(const json& id, const json&) {
    auto tools = listTools();
    json result;
    json toolsJson = json::array();
    for (const auto& t : tools) {
        json tj;
        tj["name"]        = t.name;
        tj["description"] = t.description;
        tj["inputSchema"] = t.inputSchema;
        if (!t.title.empty()) {
            tj["title"] = t.title;
        }
        if (!t.outputSchema.is_null() && t.outputSchema.is_object() && !t.outputSchema.empty()) {
            tj["outputSchema"] = t.outputSchema;
        }
        if (!t.annotations.is_null() && t.annotations.is_object() && !t.annotations.empty()) {
            tj["annotations"] = t.annotations;
        }
        if (!t.execution.is_null() && t.execution.is_object() && !t.execution.empty()) {
            tj["execution"] = t.execution;
        }
        toolsJson.push_back(std::move(tj));
    }
    result["tools"] = std::move(toolsJson);
    return jsonRpcResponse(id, std::move(result));
}

json McpServer::handleToolsCall(const json& id, const json& params) {
    std::string name      = params.value("name", "");
    json        arguments = params.contains("arguments") ? params["arguments"] : json::object();

    if (name.empty()) {
        return jsonRpcErrorResponse(id, jsonRpcError(kJsonRpcInvalidParams, "Missing tool name"));
    }

    ToolHandler handler;
    {
        std::shared_lock lock(toolsMutex_);
        auto             it = toolsByName_.find(name);
        if (it == toolsByName_.end()) {
            return jsonRpcErrorResponse(
                id,
                jsonRpcError(kMcpToolNotFound, std::string("Tool not found: ") + name)
            );
        }
        handler = it->second.handler;
    }

    json result;
    // 工具执行异常转为 isError 结果返回给客户端
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            json content      = handler(arguments);
            result["content"] = json::array();
            result["content"].push_back(std::move(content));
            result["isError"] = false;
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("[mcp] Tool execution error [{}]: {}", name, errmsg);
            json content;
            content["type"]   = "text";
            content["text"]   = std::string("Error: ") + errmsg;
            result["content"] = json::array();
            result["content"].push_back(std::move(content));
            result["isError"] = true;
            return false;
        }
    );

    return jsonRpcResponse(id, std::move(result));
}

json McpServer::handleResourcesList(const json& id, const json&) {
    auto resources = listResources();
    json result;
    json resourcesJson = json::array();
    for (const auto& r : resources) {
        json rj;
        rj["uri"]         = r.uri;
        rj["name"]        = r.name;
        rj["description"] = r.description;
        rj["mimeType"]    = r.mimeType;
        resourcesJson.push_back(std::move(rj));
    }
    result["resources"] = std::move(resourcesJson);
    return jsonRpcResponse(id, std::move(result));
}

json McpServer::handleResourcesRead(const json& id, const json& params) {
    std::string uri = params.value("uri", "");
    if (uri.empty()) {
        return jsonRpcErrorResponse(
            id,
            jsonRpcError(kJsonRpcInvalidParams, "Missing resource URI")
        );
    }

    ResourceReader reader;
    {
        std::shared_lock lock(resourcesMutex_);
        auto             it = resourcesByUri_.find(uri);
        if (it == resourcesByUri_.end()) {
            return jsonRpcErrorResponse(
                id,
                jsonRpcError(kMcpResourceNotFound, std::string("Resource not found: ") + uri)
            );
        }
        reader = it->second.reader;
    }

    auto content = reader(uri);
    if (!content.has_value()) {
        return jsonRpcErrorResponse(
            id,
            jsonRpcError(kMcpResourceNotFound, std::string("Failed to read resource: ") + uri)
        );
    }

    json result;
    json contents = json::array();
    json cj;
    cj["uri"]      = content->uri;
    cj["mimeType"] = content->mimeType;
    cj["text"]     = content->text;
    contents.push_back(std::move(cj));
    result["contents"] = std::move(contents);
    return jsonRpcResponse(id, std::move(result));
}

json McpServer::handleResourcesSubscribe(const json& id, const json& params) {
    std::string uri = params.value("uri", "");
    if (uri.empty()) {
        return jsonRpcErrorResponse(
            id,
            jsonRpcError(kJsonRpcInvalidParams, "Missing resource URI")
        );
    }
    {
        std::unique_lock lock(subscribedResourcesMutex_);
        subscribedResources_.insert(uri);
    }
    return jsonRpcResponse(id, json::object());
}

json McpServer::handleResourcesUnsubscribe(const json& id, const json& params) {
    std::string uri = params.value("uri", "");
    {
        std::unique_lock lock(subscribedResourcesMutex_);
        subscribedResources_.erase(uri);
    }
    return jsonRpcResponse(id, json::object());
}

json McpServer::handlePromptsList(const json& id, const json&) {
    auto prompts = listPrompts();
    json result;
    json promptsJson = json::array();
    for (const auto& p : prompts) {
        json pj;
        pj["name"]        = p.name;
        pj["description"] = p.description;
        json args         = json::array();
        for (const auto& a : p.arguments) {
            json aj;
            aj["name"]        = a.name;
            aj["description"] = a.description;
            aj["required"]    = a.required;
            args.push_back(std::move(aj));
        }
        pj["arguments"] = std::move(args);
        promptsJson.push_back(std::move(pj));
    }
    result["prompts"] = std::move(promptsJson);
    return jsonRpcResponse(id, std::move(result));
}

json McpServer::handlePromptsGet(const json& id, const json& params) {
    std::string name      = params.value("name", "");
    json        arguments = params.contains("arguments") ? params["arguments"] : json::object();

    if (name.empty()) {
        return jsonRpcErrorResponse(id, jsonRpcError(kJsonRpcInvalidParams, "Missing prompt name"));
    }

    PromptHandler handler;
    {
        std::shared_lock lock(promptsMutex_);
        auto             it = promptsByName_.find(name);
        if (it == promptsByName_.end()) {
            return jsonRpcErrorResponse(
                id,
                jsonRpcError(kMcpPromptNotFound, std::string("Prompt not found: ") + name)
            );
        }
        handler = it->second.handler;
    }

    auto result = handler(name, arguments);
    if (!result.has_value()) {
        return jsonRpcErrorResponse(
            id,
            jsonRpcError(kMcpPromptNotFound, std::string("Failed to get prompt: ") + name)
        );
    }

    json rj;
    rj["description"] = result->description;
    json messages     = json::array();
    for (const auto& m : result->messages) {
        json mj;
        mj["role"]    = m.role;
        mj["content"] = m.content;
        messages.push_back(std::move(mj));
    }
    rj["messages"] = std::move(messages);
    return jsonRpcResponse(id, std::move(rj));
}

json McpServer::handleLoggingSetLevel(const json& id, const json& params) {
    return jsonRpcResponse(id, json::object());
}

json McpServer::handleResourceTemplatesList(const json& id, const json&) {
    json result;
    result["resourceTemplates"] = json::array();
    return jsonRpcResponse(id, std::move(result));
}

json McpServer::handleComplete(const json& id, const json&) {
    json result;
    result["completion"]            = json::object();
    result["completion"]["values"]  = json::array();
    result["completion"]["hasMore"] = false;
    result["completion"]["total"]   = 0;
    return jsonRpcResponse(id, std::move(result));
}

void McpServer::handleInitialized(const json&) {
    XX_LOGI("[mcp] Client initialized");
}

asio::awaitable<void> McpServer::handleSseStream(
    util::HttpServer::Request&                   req,
    std::shared_ptr<util::HttpServer::SseWriter> writer
) {
    namespace http = boost::beast::http;

    auto accept = req[http::field::accept];
    if (!accept.empty() && accept != "*/*"
        && accept.find("text/event-stream") == std::string_view::npos) {
        co_await writer->close();
        co_return;
    }

    auto client    = std::make_shared<SSEClient>();
    client->writer = writer;
    {
        std::unique_lock lock(sseClientsMutex_);
        sseClients_.push_back(client);
    }

    if (!co_await writer->writeEvent("endpoint", config_.mcpEndpoint)) {
        std::unique_lock lock(sseClientsMutex_);
        sseClients_.erase(
            std::remove(sseClients_.begin(), sseClients_.end(), client),
            sseClients_.end()
        );
        co_return;
    }

    // keepalive 写失败/中断时静默退出循环 (onRethrow 也吞掉取消类异常),
    // 确保下方 sseClients_ 清理逻辑必定执行, 客户端不会被残留
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            while (!client->closed && !httpServer_->isStopped()) {
                co_await writer->writeChunk(": keepalive\n\n");
                auto               executor = co_await asio::this_coro::executor;
                asio::steady_timer timer(executor);
                timer.expires_after(std::chrono::seconds(15));
                neograph_asio_error_code ec;
                co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            }
            co_return true;
        },
        [](std::string) -> asio::awaitable<bool> {
            co_return false;
        },
        [](std::string&) -> std::optional<bool> { return false; }
    );

    {
        std::unique_lock lock(sseClientsMutex_);
        sseClients_.erase(
            std::remove(sseClients_.begin(), sseClients_.end(), client),
            sseClients_.end()
        );
    }
}

void McpServer::broadcastSSE(std::string_view event, std::string_view data) {
    std::unique_lock lock(sseClientsMutex_);
    for (auto& client : sseClients_) {
        if (!client->closed && client->writer) {
            XX_LOGI("[mcp] SSE broadcast: {} {}", event, data);
        }
    }
}

void McpServer::stopSSE() {
    std::unique_lock lock(sseClientsMutex_);
    for (auto& client : sseClients_) {
        client->closed = true;
    }
    sseClients_.clear();
}

void McpServer::notifyToolsChanged() {}

void McpServer::notifyResourcesChanged() {}

void McpServer::notifyPromptsChanged() {}

void McpServer::writeJsonResponse(
    util::HttpServer::Response& resp,
    boost::beast::http::status  status,
    const json&                 body
) {
    resp.result(status);
    resp.set(boost::beast::http::field::content_type, "application/json");
    resp.body() = body.dump();
    resp.prepare_payload();
}

} // namespace server
} // namespace agentxx
