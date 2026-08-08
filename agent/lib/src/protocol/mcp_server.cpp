#include "agentxx/protocol/mcp_server.h"

#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include <algorithm>
#include <cctype>
#include <iostream>

namespace agentxx {
namespace server {

namespace {

/// Base64 sentinel 模式 (=?base64?...?=) 判定
bool isBase64SentinelValue(std::string_view s) {
    return s.size() >= 10 && s.starts_with("=?base64?") && s.ends_with("?=");
}

/// 解码 Mcp-Name / Mcp-Param-* 头值 (Base64 sentinel 规则)
std::string decodeMcpHeaderValueForServer(std::string_view value) {
    if (isBase64SentinelValue(value)) {
        auto inner = value.substr(9, value.size() - 11);
        auto dec   = agentxx::util::base64Decode(inner);
        if (dec.has_value()) {
            return std::move(*dec);
        }
    }
    return std::string{value};
}

/// 将 body 参数值转为字符串用于 header 比对 (数值按数值语义比较)
std::string mcpParamBodyValue(const json& args, const std::string& param) {
    if (!args.contains(param) || args[param].is_null()) {
        return {};
    }
    const auto& v = args[param];
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<int64_t>());
    }
    if (v.is_number_unsigned()) {
        return std::to_string(v.get<uint64_t>());
    }
    if (v.is_number_float()) {
        auto d = v.get<double>();
        // 整数浮点值输出不带小数点, 便于与整数字符串比较
        if (d == std::floor(d) && std::abs(d) < 1e15) {
            return std::to_string(static_cast<int64_t>(d));
        }
        return fmt::format("{}", d);
    }
    return v.dump();
}

/// 头值与 body 值比对: 先精确字符串比较, 再按数值比较 (42.0 == 42)
bool headerValuesMatch(const std::string& a, const std::string& b) {
    if (a == b) {
        return true;
    }
    char*  endA = nullptr;
    char*  endB = nullptr;
    double da   = std::strtod(a.c_str(), &endA);
    double db   = std::strtod(b.c_str(), &endB);
    if (endA != nullptr && *endA == '\0' && endB != nullptr && *endB == '\0' && da == db) {
        return true;
    }
    return false;
}

} // namespace

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
    // 2026-07-28: 结束所有活跃 subscriptions/listen 订阅 (优雅回发空 result);
    // endAllSubscriptions 内部会等待 SSE 协程写完终止 result 再返回,
    // 避免随后 httpServer_->stop() 的 ioCtx.stop() 取消未完成的写入
    endAllSubscriptions();
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
                    json(nullptr),
                    jsonRpcError(kJsonRpcParseError, std::string("Parse error: ") + errmsg)
                );
                writeStdioMessage(errorResp);
                return false;
            }
        );
        if (!parsed) {
            continue;
        }

        RequestContext ctx;
        json           response = processJsonRpc(requestJson, ctx);
        if (!response.is_null()) {
            writeStdioMessage(response);
        }
    }

    // stdin EOF: 结束所有活跃订阅 (优雅关闭)
    endAllSubscriptions();
}

void McpServer::writeStdioMessage(const json& msg) {
    std::cout << msg.dump() << "\n" << std::flush;
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
    lock.unlock();
    // 2026-07-28: 通知 subscriptions/listen 订阅者
    notifyToolsChanged();
}

void McpServer::removeTool(std::string_view name) {
    std::unique_lock lock(toolsMutex_);
    toolsByName_.erase(std::string{name});
    toolsListChanged_ = true;
    lock.unlock();
    notifyToolsChanged();
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
    lock.unlock();
    notifyResourcesChanged();
}

void McpServer::removeResource(std::string_view uri) {
    std::unique_lock lock(resourcesMutex_);
    resourcesByUri_.erase(std::string{uri});
    resourcesListChanged_ = true;
    lock.unlock();
    notifyResourcesChanged();
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
    lock.unlock();
    notifyPromptsChanged();
}

void McpServer::removePrompt(std::string_view name) {
    std::unique_lock lock(promptsMutex_);
    promptsByName_.erase(std::string{name});
    promptsListChanged_ = true;
    lock.unlock();
    notifyPromptsChanged();
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

    // 2026-07-28 subscriptions/listen: POST 长连接 SSE 流 (优先于普通 router handler)
    httpServer_->addSsePostRoute(
        config_.mcpEndpoint,
        [this](util::HttpServer::Request& req, std::shared_ptr<util::HttpServer::SseWriter> writer)
            -> asio::awaitable<void> {
            co_await handleMcpPostSse(req, writer);
        }
    );

    // legacy HTTP+SSE (2024-11-05) GET 流端点
    httpServer_->addSseRoute(
        config_.sseEndpoint,
        [this](util::HttpServer::Request& req, std::shared_ptr<util::HttpServer::SseWriter> writer)
            -> asio::awaitable<void> {
            co_await handleSseStream(req, writer);
        }
    );
}

std::string McpServer::extractProtocolVersion(const json& params, const RequestContext& ctx) {
    if (params.is_object() && params.contains("_meta") && params["_meta"].is_object()) {
        auto v = params["_meta"].find(std::string{kMetaProtocolVersion});
        if (v != params["_meta"].end() && (*v).is_string()) {
            return (*v).get<std::string>();
        }
    }
    if (ctx.isHttp && !ctx.httpProtocolVersionHeader.empty()) {
        return ctx.httpProtocolVersionHeader;
    }
    return {};
}

bool McpServer::isSupportedProtocolVersion(std::string_view version) {
    for (const auto& sv : kMcpSupportedProtocols) {
        if (sv == version) {
            return true;
        }
    }
    return false;
}

json McpServer::unsupportedVersionError(std::string_view requested) {
    json data;
    json supported = json::array();
    for (const auto& sv : kMcpSupportedProtocols) {
        supported.push_back(std::string{sv});
    }
    data["supported"] = std::move(supported);
    data["requested"] = std::string{requested};
    return jsonRpcError(
        kMcpUnsupportedProtocolVersion,
        "Unsupported protocol version",
        std::move(data)
    );
}

json McpServer::serverInfoMeta() const {
    json meta;
    json info;
    info["name"]    = config_.serverName;
    info["version"] = config_.serverVersion;
    meta[std::string{kMetaServerInfo}] = std::move(info);
    return meta;
}

json McpServer::decorateModernResult(json result, std::string_view method, const json& requestMeta) const {
    result["resultType"] = "complete";
    json meta = serverInfoMeta();
    // 保留请求 _meta 中的透传字段 (如 progressToken / OTel traceparent)
    if (requestMeta.is_object()) {
        for (const auto& item : requestMeta.items()) {
            if (!meta.contains(item.first)) {
                meta[item.first] = item.second;
            }
        }
    }
    result["_meta"] = std::move(meta);
    // CacheableResult: list 类结果携带缓存提示
    if (method == "tools/list" || method == "prompts/list" || method == "resources/list"
        || method == "resources/templates/list" || method == "resources/read"
        || method == "server/discover") {
        result["ttlMs"]     = config_.cacheTtlMs;
        result["cacheScope"] = (method == "resources/read") ? "private" : config_.cacheScope;
    }
    return result;
}

json McpServer::processJsonRpc(const json& requestJson, const RequestContext& ctx) {
    if (!requestJson.is_object()) {
        return jsonRpcErrorResponse(
            json(nullptr),
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
            json(nullptr),
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

    // ---- 2026-07-28 版本门控: 每个请求独立声明版本, 不支持则拒绝 ----
    std::string declaredVersion = extractProtocolVersion(params, ctx);
    if (!declaredVersion.empty() && !isSupportedProtocolVersion(declaredVersion)) {
        return jsonRpcErrorResponse(id, unsupportedVersionError(declaredVersion));
    }
    bool isModern = (declaredVersion == kMcpProtocol2026_07_28);

    // ---- 通知处理 ----
    if (method == "notifications/initialized") {
        handleInitialized(params);
        return hasId ? jsonRpcResponse(id, json::object()) : json{};
    }
    if (method == "notifications/cancelled") {
        // stdio 取消通知 (现代/legacy); HTTP 无此通知 (关闭流即取消)
        return hasId ? jsonRpcResponse(id, json::object()) : json{};
    }
    if (method == "notifications/message") {
        return json{};
    }
    if (method.empty()) {
        return jsonRpcErrorResponse(
            json(nullptr),
            jsonRpcError(kJsonRpcInvalidRequest, "Missing method")
        );
    }

    // ---- 现代模式必需字段校验 (2026-07-28) ----
    if (isModern && (!meta.is_object() || !meta.contains(std::string{kMetaProtocolVersion})
                     || !meta.contains(std::string{kMetaClientCapabilities}))) {
        return jsonRpcErrorResponse(
            id,
            jsonRpcError(
                kJsonRpcInvalidParams,
                "Missing required _meta fields: io.modelcontextprotocol/protocolVersion, "
                "io.modelcontextprotocol/clientCapabilities"
            )
        );
    }

    json response;
    // 分发异常统一转为 JSON-RPC 内部错误响应
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            if (method == "initialize") {
                // 2026-07-28 已移除 initialize 握手; 现代请求显式报 method not found
                response = isModern
                               ? jsonRpcErrorResponse(
                                     id,
                                     jsonRpcError(kJsonRpcMethodNotFound, "Method not found: initialize")
                                 )
                               : handleInitialize(id, params);
            } else if (method == "server/discover") {
                response = isModern ? handleDiscover(id, params)
                                    : jsonRpcErrorResponse(
                                          id,
                                          jsonRpcError(
                                              kJsonRpcMethodNotFound,
                                              std::string("Method not found: ") + method
                                          )
                                      );
            } else if (method == "subscriptions/listen") {
                if (isModern) {
                    if (ctx.isHttp) {
                        // HTTP 经 POST SSE 路由处理, 不应到达此处; 防御性报错
                        response = jsonRpcErrorResponse(
                            id,
                            jsonRpcError(
                                kJsonRpcMethodNotFound,
                                "subscriptions/listen requires a streaming transport"
                            )
                        );
                    } else {
                        // stdio: 注册订阅并立即下发 ack; 无响应体 (优雅结束时才回)
                        auto sub = handleSubscriptionsListenStdio(id, params);
                        if (sub.has_value()) {
                            response = std::move(*sub);
                        } else {
                            response = json{};
                        }
                    }
                } else {
                    response = jsonRpcErrorResponse(
                        id,
                        jsonRpcError(kJsonRpcMethodNotFound, std::string("Method not found: ") + method)
                    );
                }
            } else if (method == "ping") {
                // 2026-07-28 已从规范移除 ping, 但保留实现以兼容 legacy 客户端
                // (现代客户端 ping 我们的服务端仍可用)
                response = handlePing(id);
            } else if (method == "tools/list") {
                response = handleToolsList(id, params);
            } else if (method == "tools/call") {
                response = handleToolsCall(id, params);
            } else if (method == "resources/list") {
                response = handleResourcesList(id, params);
            } else if (method == "resources/read") {
                response = handleResourcesRead(id, params);
                // 2026-07-28: 资源不存在用 -32602 (Invalid Params);
                // legacy (2025-11-25 及更早) 保留 -32002
                if (!isModern && response.contains("error") && response["error"].is_object()) {
                    auto msg = response["error"].value("message", std::string{});
                    if (msg.starts_with("Resource not found")) {
                        response["error"]["code"] = kMcpResourceNotFound;
                    }
                }
            } else if (method == "resources/subscribe" || method == "resources/unsubscribe") {
                // 2026-07-28 由 subscriptions/listen 取代
                response = isModern
                               ? jsonRpcErrorResponse(
                                     id,
                                     jsonRpcError(kJsonRpcMethodNotFound, std::string("Method not found: ") + method)
                                 )
                               : (method == "resources/subscribe" ? handleResourcesSubscribe(id, params)
                                                                  : handleResourcesUnsubscribe(id, params));
            } else if (method == "resources/templates/list") {
                response = handleResourceTemplatesList(id, params);
            } else if (method == "prompts/list") {
                response = handlePromptsList(id, params);
            } else if (method == "prompts/get") {
                response = handlePromptsGet(id, params);
            } else if (method == "logging/setLevel") {
                // 2026-07-28 已移除; legacy 保留
                response = isModern
                               ? jsonRpcErrorResponse(
                                     id,
                                     jsonRpcError(kJsonRpcMethodNotFound, std::string("Method not found: ") + method)
                                 )
                               : handleLoggingSetLevel(id, params);
            } else if (method == "completion/complete") {
                response = handleComplete(id, params);
            } else {
                response = jsonRpcErrorResponse(
                    id,
                    jsonRpcError(kJsonRpcMethodNotFound, std::string("Method not found: ") + method)
                );
            }
            if (!response.is_null() && response.contains("result")) {
                if (isModern) {
                    // 2026-07-28: resultType + serverInfo + 缓存字段
                    response["result"] = decorateModernResult(response["result"], method, meta);
                } else if (!meta.is_null()) {
                    // legacy: _meta 透传 (2025-03-26+)
                    if (!response["result"].contains("_meta")) {
                        response["result"]["_meta"] = meta;
                    }
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
    co_await handleMcpPost(req, &resp, nullptr);
}

asio::awaitable<void> McpServer::handleMcpPostSse(
    util::HttpServer::Request&                   req,
    std::shared_ptr<util::HttpServer::SseWriter> writer
) {
    co_await handleMcpPost(req, nullptr, std::move(writer));
}

asio::awaitable<void> McpServer::handleMcpPost(
    util::HttpServer::Request&                   req,
    util::HttpServer::Response*                  resp,
    std::shared_ptr<util::HttpServer::SseWriter> writer
) {
    namespace http = boost::beast::http;

    // SSE 路由场景下错误响应直接写流 (writer), 普通路由场景写 resp
    auto respond = [&](http::status status, const json& body) -> asio::awaitable<void> {
        if (writer) {
            co_await writer->writeResponse(status, "application/json", body.dump());
        } else if (resp) {
            writeJsonResponse(*resp, status, body);
        }
        co_return;
    };
    auto respondEmpty = [&](http::status status) -> asio::awaitable<void> {
        if (writer) {
            co_await writer->writeResponse(status, "application/json", "");
        } else if (resp) {
            resp->result(status);
            resp->prepare_payload();
        }
        co_return;
    };

    // 2026-07-28 Origin 校验 (防 DNS rebinding): 非法 Origin → 403
    auto hostHeader = req[http::field::host];
    auto originIt   = req.find("Origin");
    if (originIt != req.end() && !isOriginAllowed(originIt->value(), hostHeader)) {
        json errorResp = jsonRpcErrorResponse(
            json(nullptr),
            jsonRpcError(-32000, "Forbidden: invalid Origin")
        );
        co_await respond(http::status::forbidden, errorResp);
        co_return;
    }

    auto accept = req[http::field::accept];
    if (!isAcceptValid(accept)) {
        json errorResp = jsonRpcErrorResponse(
            json(nullptr),
            jsonRpcError(
                -32000,
                "Not Acceptable: Client must accept both application/json "
                "and text/event-stream"
            )
        );
        co_await respond(http::status::not_acceptable, errorResp);
        co_return;
    }

    json requestJson;
    std::string parseErrMsg;
    bool parsed = agentxx::util::catchError<bool>(
        [&]() -> bool {
            requestJson = json::parse(req.body());
            return true;
        },
        [&](std::string errmsg) -> bool {
            parseErrMsg = std::move(errmsg);
            return false;
        }
    );
    if (!parsed) {
        auto errorResp = jsonRpcErrorResponse(
            json(nullptr),
            jsonRpcError(kJsonRpcParseError, fmt::format("Parse error: {}", parseErrMsg))
        );
        co_await respond(http::status::bad_request, errorResp);
        co_return;
    }

    // 构建请求上下文
    RequestContext ctx;
    ctx.isHttp                       = true;
    ctx.httpProtocolVersionHeader    = std::string{req["MCP-Protocol-Version"]};
    ctx.httpMcpMethodHeader          = std::string{req["Mcp-Method"]};
    ctx.httpMcpNameHeader            = std::string{req["Mcp-Name"]};
    std::cerr << "[DBG] mcp-server request headers:" << std::endl;
    for (auto const& h : req.base()) {
        std::cerr << "[DBG]   " << h.name_string() << ": " << h.value() << std::endl;
    }
    std::cerr << "[DBG]   Mcp-Name lookup: '" << ctx.httpMcpNameHeader << "'" << std::endl;
    json params                      = requestJson.contains("params") ? requestJson["params"] : json::object();
    json requestId                   = requestJson.contains("id") && !requestJson["id"].is_null()
                                          ? requestJson["id"]
                                          : json{};
    ctx.protocolVersion              = extractProtocolVersion(params, ctx);
    ctx.isModern                     = (ctx.protocolVersion == kMcpProtocol2026_07_28);

    std::string method = requestJson.value("method", "");

    // 版本门控: 未知版本 → 400 + UnsupportedProtocolVersionError
    if (!ctx.protocolVersion.empty() && !isSupportedProtocolVersion(ctx.protocolVersion)) {
        co_await respond(http::status::bad_request, jsonRpcErrorResponse(requestId, unsupportedVersionError(ctx.protocolVersion)));
        co_return;
    }

    // 现代模式: 标准请求头校验 → 400 + HeaderMismatch
    if (ctx.isModern) {
        std::optional<std::string> hdrErr = validateModernHeaders(req, requestJson, ctx);
        if (hdrErr.has_value()) {
            json errResp = jsonRpcErrorResponse(
                requestId,
                jsonRpcError(kMcpHeaderMismatch, std::move(*hdrErr))
            );
            co_await respond(http::status::bad_request, errResp);
            co_return;
        }
        // _meta 必需字段
        json meta = params.contains("_meta") ? params["_meta"] : json();
        if (!meta.is_object() || !meta.contains(std::string{kMetaProtocolVersion})
            || !meta.contains(std::string{kMetaClientCapabilities})) {
            json errResp = jsonRpcErrorResponse(
                requestId,
                jsonRpcError(
                    kJsonRpcInvalidParams,
                    "Missing required _meta fields: io.modelcontextprotocol/protocolVersion, "
                    "io.modelcontextprotocol/clientCapabilities"
                )
            );
            co_await respond(http::status::bad_request, errResp);
            co_return;
        }
    }

    // 2026-07-28 subscriptions/listen: 长连接 SSE 流
    if (ctx.isModern && method == "subscriptions/listen") {
        if (writer) {
            co_await handleSubscriptionsListenSse(requestId, params, writer);
            co_return;
        }
        // 无 writer (普通 handler 被 SSE 路由遮蔽, 实际不可达)
        co_await respond(
            http::status::bad_request,
            jsonRpcErrorResponse(requestId, jsonRpcError(kJsonRpcMethodNotFound, "subscriptions/listen requires a streaming transport"))
        );
        co_return;
    }

    json response = processJsonRpc(requestJson, ctx);
    if (response.is_null()) {
        // 通知: 202 Accepted, 无 body
        co_await respondEmpty(http::status::accepted);
        co_return;
    }

    // 现代模式错误 → 规范 HTTP 状态码
    http::status status = http::status::ok;
    if (ctx.isModern && response.contains("error") && response["error"].is_object()) {
        int code = response["error"].value("code", 0);
        if (code == kJsonRpcMethodNotFound) {
            status = http::status::not_found;
        } else if (code == kJsonRpcInvalidRequest || code == kJsonRpcInvalidParams
                   || code == kMcpHeaderMismatch || code == kMcpMissingRequiredClientCapability
                   || code == kMcpUnsupportedProtocolVersion) {
            status = http::status::bad_request;
        }
    }

    if (prefersSse(accept)) {
        if (writer) {
            co_await writer->writeEvent("message", response.dump());
            co_await writer->close();
        } else {
            resp->version(req.version());
            resp->result(http::status::ok);
            resp->set(http::field::content_type, "text/event-stream");
            resp->set(http::field::cache_control, "no-cache");
            resp->set("X-Accel-Buffering", "no");
            std::string sseBody = fmt::format("event: message\ndata: {}\n\n", response.dump());
            resp->body()        = std::move(sseBody);
            resp->prepare_payload();
        }
        co_return;
    }

    co_await respond(status, response);
}

bool McpServer::isOriginAllowed(std::string_view origin, std::string_view hostHeader) const {
    if (origin.empty()) {
        return true;
    }
    // 显式白名单
    if (!config_.allowedOrigins.empty()) {
        for (const auto& o : config_.allowedOrigins) {
            if (origin == o) {
                return true;
            }
            if (origin == "http://" + o || origin == "https://" + o) {
                return true;
            }
        }
        return false;
    }
    // 默认: 同源校验 (防 DNS rebinding)。Origin 形如 scheme://host[:port]
    auto schemePos = origin.find("://");
    if (schemePos == std::string_view::npos) {
        return false;
    }
    std::string_view rest = origin.substr(schemePos + 3);
    // Origin 不应含路径; 防御非法输入
    if (rest.find('/') != std::string_view::npos) {
        return false;
    }
    return rest == hostHeader;
}

std::optional<std::string> McpServer::validateModernHeaders(
    const util::HttpServer::Request& req,
    const json&                      requestJson,
    const RequestContext&            ctx
) const {
    std::string method = requestJson.value("method", "");
    json        params = requestJson.contains("params") ? requestJson["params"] : json::object();

    // MCP-Protocol-Version: 必须存在且与 body _meta 一致
    if (ctx.httpProtocolVersionHeader.empty()) {
        return "Missing required header MCP-Protocol-Version";
    }
    if (ctx.httpProtocolVersionHeader != ctx.protocolVersion) {
        return fmt::format(
            "MCP-Protocol-Version header value '{}' does not match body value '{}'",
            ctx.httpProtocolVersionHeader,
            ctx.protocolVersion
        );
    }

    // Mcp-Method: 必须存在且与 body method 一致
    if (ctx.httpMcpMethodHeader.empty()) {
        return "Missing required header Mcp-Method";
    }
    if (ctx.httpMcpMethodHeader != method) {
        return fmt::format(
            "Mcp-Method header value '{}' does not match body value '{}'",
            ctx.httpMcpMethodHeader,
            method
        );
    }

    // Mcp-Name: tools/call / resources/read / prompts/get 必需
    if (method == "tools/call" || method == "resources/read" || method == "prompts/get") {
        std::string bodyName;
        if (method == "resources/read") {
            bodyName = params.value("uri", std::string{});
        } else {
            bodyName = params.value("name", std::string{});
        }
        if (ctx.httpMcpNameHeader.empty()) {
            return "Missing required header Mcp-Name";
        }
        auto decoded = decodeMcpHeaderValueForServer(ctx.httpMcpNameHeader);
        if (decoded != bodyName) {
            return fmt::format(
                "Mcp-Name header value '{}' does not match body value '{}'",
                decoded,
                bodyName
            );
        }
    }

    // Mcp-Param-*: 仅校验工具 inputSchema 中声明了 x-mcp-header 的参数
    if (method == "tools/call") {
        auto toolName = params.value("name", std::string{});
        std::shared_lock lock(toolsMutex_);
        auto             it = toolsByName_.find(toolName);
        if (it != toolsByName_.end()) {
            const auto& schema = it->second.def.inputSchema;
            std::unordered_map<std::string, std::string> headerToParam; // 小写 header 名 -> 参数名
            std::unordered_map<std::string, std::string> paramToHeader; // 参数名 -> 原始 header 名
            if (schema.is_object() && schema.contains("properties") && schema["properties"].is_object()) {
                for (const auto& propItem : schema["properties"].items()) {
                    const auto& pname = propItem.first;
                    const auto& pdef  = propItem.second;
                    if (pdef.is_object() && pdef.contains("x-mcp-header") && pdef["x-mcp-header"].is_string()) {
                        auto hname = pdef["x-mcp-header"].get<std::string>();
                        std::string lower;
                        for (char c : hname) {
                            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                        }
                        headerToParam[lower] = pname;
                        paramToHeader[pname] = hname;
                    }
                }
            }
            if (!headerToParam.empty()) {
                json args = params.contains("arguments") && params["arguments"].is_object()
                                ? params["arguments"]
                                : json::object();
                // 1) 已声明的 header 必须与 body 一致
                for (auto& h : req.base()) {
                    std::string hname(h.name_string());
                    std::string lowerHname;
                    for (char c : hname) {
                        lowerHname.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                    }
                    constexpr std::string_view kPrefix = "mcp-param-";
                    if (lowerHname.size() <= kPrefix.size()
                        || lowerHname.substr(0, kPrefix.size()) != kPrefix) {
                        continue;
                    }
                    auto headerKey = lowerHname.substr(kPrefix.size());
                    auto pit       = headerToParam.find(std::string{headerKey});
                    if (pit == headerToParam.end()) {
                        continue; // 未声明注解 → 忽略
                    }
                    auto decoded = decodeMcpHeaderValueForServer(h.value());
                    std::string bodyVal = mcpParamBodyValue(args, pit->second);
                    if (!headerValuesMatch(decoded, bodyVal)) {
                        return fmt::format(
                            "Mcp-Param-{} header value '{}' does not match body value '{}'",
                            hname,
                            decoded,
                            bodyVal
                        );
                    }
                }
                // 2) 参数有值但缺 header → 拒绝 (非合规模客户端)
                for (const auto& [pname, hname] : paramToHeader) {
                    if (args.contains(pname) && !args[pname].is_null()) {
                        std::string lowerH;
                        for (char c : hname) {
                            lowerH.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                        }
                        if (req.find(fmt::format("Mcp-Param-{}", hname)) == req.end()
                            && req.find(fmt::format("mcp-param-{}", lowerH)) == req.end()) {
                            return fmt::format("Missing required header Mcp-Param-{}", hname);
                        }
                    }
                }
            }
        }
    }
    return std::nullopt;
}

json McpServer::buildCapabilities() const {
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
    capabilities["tasks"]        = json::object();
    capabilities["elicitation"]  = json::object();
    return capabilities;
}

json McpServer::handleDiscover(const json& id, const json& params) {
    json result;
    json versions = json::array();
    for (const auto& sv : kMcpSupportedProtocols) {
        versions.push_back(std::string{sv});
    }
    result["supportedVersions"] = std::move(versions);
    result["capabilities"]      = buildCapabilities();
    result["instructions"]      = "MCP server powered by agentxx";
    result["ttlMs"]             = config_.cacheTtlMs;
    result["cacheScope"]        = config_.cacheScope;
    return jsonRpcResponse(id, std::move(result));
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

    json capabilities = buildCapabilities();

    json serverInfo;
    serverInfo["name"]    = config_.serverName;
    serverInfo["version"] = config_.serverVersion;
    serverInfo["title"]   = config_.serverName;

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
            // 2026-07-28: 资源不存在 → -32602 (Invalid Params), 与 JSON-RPC 对齐
            return jsonRpcErrorResponse(
                id,
                jsonRpcError(kJsonRpcInvalidParams, std::string("Resource not found: ") + uri)
            );
        }
        reader = it->second.reader;
    }

    auto content = reader(uri);
    if (!content.has_value()) {
        return jsonRpcErrorResponse(
            id,
            jsonRpcError(kJsonRpcInvalidParams, std::string("Resource not found: ") + uri)
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

// ---------------------------------------------------------------------------
// 2026-07-28 subscriptions/listen
// ---------------------------------------------------------------------------

namespace {

/// 从 listen 请求解析通知过滤器 (未提供的字段默认不订阅)
McpServer::SubscriptionFilter parseSubscriptionFilter(const json& params) {
    McpServer::SubscriptionFilter filter;
    if (!params.is_object() || !params.contains("notifications") || !params["notifications"].is_object()) {
        return filter;
    }
    const auto& n = params["notifications"];
    filter.toolsListChanged     = n.value("toolsListChanged", false);
    filter.promptsListChanged   = n.value("promptsListChanged", false);
    filter.resourcesListChanged = n.value("resourcesListChanged", false);
    if (n.contains("resourceSubscriptions") && n["resourceSubscriptions"].is_array()) {
        for (const auto& u : n["resourceSubscriptions"]) {
            if (u.is_string()) {
                filter.resourceSubscriptions.push_back(u.get<std::string>());
            }
        }
    }
    return filter;
}

/// 生成过滤器对应的 ack notifications 子集 (服务端确认支持的)
json filterToJson(const McpServer::SubscriptionFilter& filter) {
    json n;
    if (filter.toolsListChanged) {
        n["toolsListChanged"] = true;
    }
    if (filter.promptsListChanged) {
        n["promptsListChanged"] = true;
    }
    if (filter.resourcesListChanged) {
        n["resourcesListChanged"] = true;
    }
    if (!filter.resourceSubscriptions.empty()) {
        json uris = json::array();
        for (const auto& u : filter.resourceSubscriptions) {
            uris.push_back(u);
        }
        n["resourceSubscriptions"] = std::move(uris);
    }
    return n;
}

/// 生成携带 subscriptionId 的通知
json makeSubscriptionNotification(std::string_view method, const json& subId, json extraParams = json::object()) {
    json notif;
    notif["jsonrpc"] = "2.0";
    notif["method"]  = std::string{method};
    json params;
    params["_meta"][std::string{kMetaSubscriptionId}] = subId;
    if (extraParams.is_object()) {
        for (const auto& item : extraParams.items()) {
            params[item.first] = item.second;
        }
    }
    notif["params"] = std::move(params);
    return notif;
}

} // namespace

std::optional<json> McpServer::handleSubscriptionsListenStdio(const json& id, const json& params) {
    if (id.is_null()) {
        return jsonRpcErrorResponse(
            json(nullptr),
            jsonRpcError(kJsonRpcInvalidRequest, "subscriptions/listen requires an id")
        );
    }
    auto entry        = std::make_shared<SubscriptionEntry>();
    entry->idKey      = id.dump();
    entry->id         = id;
    entry->filter     = parseSubscriptionFilter(params);
    entry->closed     = false;
    {
        std::lock_guard lock(subscriptionsMutex_);
        subscriptions_[entry->idKey] = entry;
    }

    // 立即下发 ack 通知 (stdio 直接写 stdout)
    auto ack = makeSubscriptionNotification(
        "notifications/subscriptions/acknowledged",
        id,
        {{"notifications", filterToJson(entry->filter)}}
    );
    writeStdioMessage(ack);
    XX_LOGI("[mcp] stdio subscription registered: id={}", id.dump());
    return std::nullopt;
}

asio::awaitable<void> McpServer::handleSubscriptionsListenSse(
    const json&                                  id,
    const json&                                  params,
    std::shared_ptr<util::HttpServer::SseWriter> writer
) {
    if (id.is_null()) {
        json err = jsonRpcErrorResponse(
            json(nullptr),
            jsonRpcError(kJsonRpcInvalidRequest, "subscriptions/listen requires an id")
        );
        co_await writer->writeResponse(boost::beast::http::status::bad_request, "application/json", err.dump());
        co_return;
    }

    auto entry    = std::make_shared<SubscriptionEntry>();
    entry->idKey  = id.dump();
    entry->id     = id;
    entry->filter = parseSubscriptionFilter(params);
    entry->writer = writer;
    entry->closed = false;
    {
        std::lock_guard lock(subscriptionsMutex_);
        subscriptions_[entry->idKey] = entry;
    }

    // 第一条消息: ack 通知
    auto ack = makeSubscriptionNotification(
        "notifications/subscriptions/acknowledged",
        id,
        {{"notifications", filterToJson(entry->filter)}}
    );
    if (!co_await writer->writeEvent("message", ack.dump())) {
        entry->closed = true;
        std::lock_guard lock(subscriptionsMutex_);
        subscriptions_.erase(entry->idKey);
        co_return;
    }

    // 长连接: 轮询通知队列 + keepalive 注释; 服务端停止/客户端断开时退出
    std::cerr << "[DBG] subs-listen-sse: entering loop id=" << id.dump()
              << " toolsChanged=" << entry->filter.toolsListChanged << std::endl;
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            auto executor = co_await asio::this_coro::executor;
            auto lastKeepalive = std::chrono::steady_clock::now();
            while (!entry->closed && !httpServer_->isStopped()) {
                // 排空通知队列 (notify* 可能从任意线程入队)
                std::vector<json> pending;
                {
                    std::lock_guard lock(subscriptionsMutex_);
                    pending.swap(entry->pending);
                }
                for (const auto& n : pending) {
                    std::cerr << "[DBG] subs-listen-sse: writing notification " << n.dump()
                              << std::endl;
                    if (!co_await writer->writeEvent("message", n.dump())) {
                        entry->closed = true;
                        std::cerr << "[DBG] subs-listen-sse: writeEvent failed" << std::endl;
                        break;
                    }
                }
                if (entry->closed) {
                    std::cerr << "[DBG] subs-listen-sse: closed, breaking" << std::endl;
                    break;
                }
                // keepalive 注释 (约 15s 一次)
                auto now = std::chrono::steady_clock::now();
                if (now - lastKeepalive >= std::chrono::seconds(15)) {
                    if (!co_await writer->writeChunk(": keepalive\n\n")) {
                        entry->closed = true;
                        break;
                    }
                    lastKeepalive = now;
                }
                asio::steady_timer timer(executor);
                timer.expires_after(std::chrono::milliseconds(250));
                neograph_asio_error_code ec;
                co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            }
            co_return true;
        },
        [](std::string) -> asio::awaitable<bool> {
            co_return false;
        },
        [](std::string&) -> std::optional<bool> {
            return false;
        }
    );

    {
        std::lock_guard lock(subscriptionsMutex_);
        subscriptions_.erase(entry->idKey);
    }
    // 排空剩余通知 (含 endSubscription 在服务端停止时入队的最终空 result)
    std::vector<json> pending;
    {
        std::lock_guard lock(subscriptionsMutex_);
        pending.swap(entry->pending);
    }
    for (const auto& n : pending) {
        co_await writer->writeEvent("message", n.dump());
    }
    co_await writer->close();
    // 优雅结束完成: stop() 等待此标志后再停止 ioCtx, 避免终止 result 被取消
    entry->done.store(true, std::memory_order_release);
}

/// 等待指定 HTTP 订阅 SSE 协程完成优雅结束 (最多 1s, 防止 stop 阻塞过久)
void McpServer::waitSubscriptionsDrained(
    const std::vector<std::shared_ptr<SubscriptionEntry>>& subs
) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    for (const auto& sub : subs) {
        if (!sub->writer) {
            continue; // stdio 订阅同步写完, 无需等待
        }
        while (!sub->done.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }
}

void McpServer::sendSubscriptionNotification(SubscriptionEntry& sub, const json& notification) {
    if (sub.writer) {
        // HTTP: 入队, 由 SSE 循环 (io 线程) 下发, 避免跨线程写 socket
        std::cerr << "[DBG] subs-notify: enqueue " << notification.dump() << std::endl;
        std::lock_guard lock(subscriptionsMutex_);
        sub.pending.push_back(notification);
    } else {
        // stdio: 直接写 stdout (同步, 线程安全)
        writeStdioMessage(notification);
    }
}

void McpServer::endSubscription(SubscriptionEntry& sub) {
    if (sub.closed.exchange(true)) {
        return;
    }
    json result;
    result["resultType"] = "complete";
    result["_meta"][std::string{kMetaSubscriptionId}] = sub.id;
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = sub.id;
    resp["result"]  = std::move(result);
    if (sub.writer) {
        std::lock_guard lock(subscriptionsMutex_);
        sub.pending.push_back(std::move(resp));
    } else {
        writeStdioMessage(resp);
    }
}

void McpServer::endAllSubscriptions() {
    std::vector<std::shared_ptr<SubscriptionEntry>> subs;
    {
        std::lock_guard lock(subscriptionsMutex_);
        for (auto& [key, sub] : subscriptions_) {
            subs.push_back(sub);
        }
        subscriptions_.clear();
    }
    for (auto& sub : subs) {
        endSubscription(*sub);
    }
    // 等待 SSE 协程把终止 result 写完 (stop() 随后会停止 ioCtx)
    waitSubscriptionsDrained(subs);
}

void McpServer::notifyToolsChanged() {
    std::vector<std::shared_ptr<SubscriptionEntry>> subs;
    {
        std::lock_guard lock(subscriptionsMutex_);
        for (auto& [key, sub] : subscriptions_) {
            if (!sub->closed && sub->filter.toolsListChanged) {
                subs.push_back(sub);
            }
        }
    }
    for (auto& sub : subs) {
        sendSubscriptionNotification(
            *sub,
            makeSubscriptionNotification("notifications/tools/list_changed", sub->id)
        );
    }
}

void McpServer::notifyResourcesChanged() {
    std::vector<std::shared_ptr<SubscriptionEntry>> subs;
    {
        std::lock_guard lock(subscriptionsMutex_);
        for (auto& [key, sub] : subscriptions_) {
            if (!sub->closed && sub->filter.resourcesListChanged) {
                subs.push_back(sub);
            }
        }
    }
    for (auto& sub : subs) {
        sendSubscriptionNotification(
            *sub,
            makeSubscriptionNotification("notifications/resources/list_changed", sub->id)
        );
    }
}

void McpServer::notifyPromptsChanged() {
    std::vector<std::shared_ptr<SubscriptionEntry>> subs;
    {
        std::lock_guard lock(subscriptionsMutex_);
        for (auto& [key, sub] : subscriptions_) {
            if (!sub->closed && sub->filter.promptsListChanged) {
                subs.push_back(sub);
            }
        }
    }
    for (auto& sub : subs) {
        sendSubscriptionNotification(
            *sub,
            makeSubscriptionNotification("notifications/prompts/list_changed", sub->id)
        );
    }
}

void McpServer::notifyResourceUpdated(std::string_view uri) {
    std::vector<std::shared_ptr<SubscriptionEntry>> subs;
    {
        std::lock_guard lock(subscriptionsMutex_);
        for (auto& [key, sub] : subscriptions_) {
            if (sub->closed) {
                continue;
            }
            for (const auto& u : sub->filter.resourceSubscriptions) {
                if (u == uri) {
                    subs.push_back(sub);
                    break;
                }
            }
        }
    }
    json extra;
    extra["uri"] = std::string{uri};
    for (auto& sub : subs) {
        sendSubscriptionNotification(
            *sub,
            makeSubscriptionNotification("notifications/resources/updated", sub->id, extra)
        );
    }
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
        [](std::string&) -> std::optional<bool> {
            return false;
        }
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
