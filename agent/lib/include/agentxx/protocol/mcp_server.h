#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <asio/awaitable.hpp>
#include <neograph/json.h>

#include "agentxx/util/http_server.h"

namespace agentxx {
namespace server {

using json = neograph::json;

// ---------------------------------------------------------------------------
// JSON-RPC 工具函数
// ---------------------------------------------------------------------------

inline json jsonRpcError(int code, std::string_view message, std::optional<json> data = {}) {
    json err;
    err["code"]    = code;
    err["message"] = std::string(message);
    if (data.has_value()) {
        err["data"] = std::move(*data);
    }
    return err;
}

inline json jsonRpcResponse(json id, json result) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = std::move(id);
    resp["result"]  = std::move(result);
    return resp;
}

inline json jsonRpcErrorResponse(json id, json error) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = std::move(id);
    resp["error"]   = std::move(error);
    return resp;
}

// 标准 JSON-RPC 错误码
inline constexpr int kJsonRpcParseError     = -32700;
inline constexpr int kJsonRpcInvalidRequest = -32600;
inline constexpr int kJsonRpcMethodNotFound = -32601;
inline constexpr int kJsonRpcInvalidParams  = -32602;
inline constexpr int kJsonRpcInternalError  = -32603;

// MCP 自定义错误码
// -32000..-32019: 实现自定义 (legacy, 兼容既有 SDK 用法)
// -32020..-32099: MCP 规范保留 (2026-07-28 起)
inline constexpr int kMcpToolNotFound       = -32000;
inline constexpr int kMcpToolExecutionError = -32001;
inline constexpr int kMcpResourceNotFound = -32002; // 2025-11-25 及更早; 2026-07-28 起改用 -32602
inline constexpr int kMcpPromptNotFound = -32003;
// 2026-07-28: 规范定义错误码
inline constexpr int kMcpHeaderMismatch                  = -32020;
inline constexpr int kMcpMissingRequiredClientCapability = -32021;
inline constexpr int kMcpUnsupportedProtocolVersion      = -32022;

// ---------------------------------------------------------------------------
// 数据类型 (与 MCP 规范字段一一对应)
// ---------------------------------------------------------------------------

struct McpToolDefinition {
    std::string name;
    std::string description;
    std::string title; // 2025-11-25: display name
    json        inputSchema  = json::object();
    json        outputSchema = json::object(); // 2025-11-25: structured output schema
    json        annotations  = json::object(); // 2025-11-25: tool behavior metadata
    json        execution    = json::object(); // 2025-11-25: execution config
};

struct McpResourceDefinition {
    std::string uri;
    std::string name;
    std::string description;
    std::string mimeType;
};

struct McpResourceContent {
    std::string uri;
    std::string mimeType;
    std::string text;
};

struct McpPromptArgument {
    std::string name;
    std::string description;
    bool        required = false;
};

struct McpPromptDefinition {
    std::string                    name;
    std::string                    description;
    std::vector<McpPromptArgument> arguments;
};

struct McpPromptMessage {
    std::string role; // "user" | "assistant"
    json        content;
};

struct McpPromptResult {
    std::string                   description;
    std::vector<McpPromptMessage> messages;
};

// 支持的 MCP 协议版本 (协商时最新优先)
inline constexpr std::string_view kMcpProtocol2024_11_05   = "2024-11-05";
inline constexpr std::string_view kMcpProtocol2025_03_26   = "2025-03-26";
inline constexpr std::string_view kMcpProtocol2025_06_18   = "2025-06-18";
inline constexpr std::string_view kMcpProtocol2025_11_25   = "2025-11-25";
inline constexpr std::string_view kMcpProtocol2026_07_28   = "2026-07-28";
inline constexpr std::string_view kMcpSupportedProtocols[] = {
    kMcpProtocol2026_07_28,
    kMcpProtocol2025_11_25,
    kMcpProtocol2025_06_18,
    kMcpProtocol2025_03_26,
    kMcpProtocol2024_11_05,
};

// 2026-07-28 `_meta` 保留键
inline constexpr std::string_view kMetaProtocolVersion = "io.modelcontextprotocol/protocolVersion";
inline constexpr std::string_view kMetaClientInfo      = "io.modelcontextprotocol/clientInfo";
inline constexpr std::string_view kMetaClientCapabilities
    = "io.modelcontextprotocol/clientCapabilities";
inline constexpr std::string_view kMetaServerInfo     = "io.modelcontextprotocol/serverInfo";
inline constexpr std::string_view kMetaSubscriptionId = "io.modelcontextprotocol/subscriptionId";
inline constexpr std::string_view kMetaLogLevel       = "io.modelcontextprotocol/logLevel";

// ---------------------------------------------------------------------------
// McpServer —— MCP 服务器 (HTTP + SSE 传输)
// 规范: https://modelcontextprotocol.io/docs/2026-07-28/getting-started/intro
// ---------------------------------------------------------------------------

class McpServer {
public:

    using ToolHandler    = std::function<json(const json& arguments)>;
    using ResourceReader = std::function<std::optional<McpResourceContent>(std::string_view uri)>;
    using PromptHandler  = std::function<
         std::optional<McpPromptResult>(std::string_view name, const json& arguments)>;

    struct Config {
        util::HttpServer::Config httpConfig;
        std::string              mcpEndpoint   = "/mcp";
        std::string              sseEndpoint   = "/mcp/sse";
        std::string              serverName    = "agentxx-mcp";
        std::string              serverVersion = "0.1.0";
        std::chrono::seconds     toolTimeout{60};
        size_t                   maxMessageSize = 4 * 1024 * 1024; // 4 MB

        /// 2026-07-28 Origin 校验: 非空时仅允许列出的 Origin (host[:port] 或完整 URL);
        /// 为空时默认策略: Origin 与请求 Host 同源则放行, 否则 403 (防 DNS rebinding)。
        std::vector<std::string> allowedOrigins;

        /// 2026-07-28 CacheableResult: list 类结果 (tools/list 等) 的缓存提示
        uint64_t    cacheTtlMs = 300000; // 5 分钟
        std::string cacheScope = "public";
    };

    /// 2026-07-28 subscriptions/listen 通知过滤器
    struct SubscriptionFilter {
        bool                     toolsListChanged     = false;
        bool                     promptsListChanged   = false;
        bool                     resourcesListChanged = false;
        std::vector<std::string> resourceSubscriptions;
    };

    explicit McpServer();
    explicit McpServer(Config config);

    McpServer(const McpServer&)            = delete;
    McpServer& operator=(const McpServer&) = delete;

    ~McpServer();

    // -----------------------------------------------------------------------
    // 生命周期
    // -----------------------------------------------------------------------

    void start();
    void stop();

    /// 以 stdin/stdout 运行 (换行分隔 JSON); 阻塞到 stdin 关闭 (EOF / Ctrl-D)
    void runStdio();

    uint16_t port() const;
    size_t   activeConnections() const;
    bool     isStopped() const;

    // -----------------------------------------------------------------------
    // 工具注册
    // -----------------------------------------------------------------------

    void                           addTool(McpToolDefinition def, ToolHandler handler);
    void                           removeTool(std::string_view name);
    std::vector<McpToolDefinition> listTools() const;

    void addResource(McpResourceDefinition def, ResourceReader reader);
    void removeResource(std::string_view uri);
    std::vector<McpResourceDefinition> listResources() const;

    /// 2026-07-28: 资源内容更新时调用, 通知订阅了该 uri 的 subscriptions/listen 订阅者
    void notifyResourceUpdated(std::string_view uri);

    // -----------------------------------------------------------------------
    // 提示词 (Prompt) 注册
    // -----------------------------------------------------------------------

    void                             addPrompt(McpPromptDefinition def, PromptHandler handler);
    void                             removePrompt(std::string_view name);
    std::vector<McpPromptDefinition> listPrompts() const;

    // -----------------------------------------------------------------------
    // 能力 (Capabilities) 声明
    // -----------------------------------------------------------------------

    struct Capabilities {
        bool tools     = true;
        bool resources = false;
        bool prompts   = false;
        bool logging   = false;
    };

    void                setCapabilities(Capabilities caps);
    const Capabilities& capabilities() const;

private:

    struct ToolEntry {
        McpToolDefinition def;
        ToolHandler       handler;
    };

    struct ResourceEntry {
        McpResourceDefinition def;
        ResourceReader        reader;
    };

    struct PromptEntry {
        McpPromptDefinition def;
        PromptHandler       handler;
    };

    struct SSEClient {
        std::shared_ptr<util::HttpServer::SseWriter> writer;
        bool                                         closed = false;
    };

    /// 2026-07-28 subscriptions/listen 活跃订阅
    struct SubscriptionEntry {
        std::string        idKey; // JSON-RPC id 的规范化 key (数字/字符串)
        json               id;    // 原始 id (回显到通知 _meta)
        SubscriptionFilter filter;
        std::shared_ptr<util::HttpServer::SseWriter> writer; // HTTP: SSE 流; stdio 为空
        std::atomic<bool>                            closed{false};
        /// HTTP: SSE 协程完成优雅结束 (排空 pending + close) 后置位,
        /// 供 stop() 等待, 避免 ioCtx 停止取消未完成的终止 result 写入
        std::atomic<bool> done{false};
        /// HTTP 通知队列: notify* 可从任意线程入队, SSE 循环 (io 线程) 排空下发
        std::vector<json> pending;
    };

    // -----------------------------------------------------------------------
    // 请求上下文 (transport 无关的版本/元信息)
    // -----------------------------------------------------------------------

    struct RequestContext {
        std::string protocolVersion;  // 本次请求声明的协议版本 (空 = 未声明/legacy)
        bool        isModern = false; // 2026-07-28 及更新
        // HTTP 特有 (供 header 校验)
        std::string httpProtocolVersionHeader;
        std::string httpMcpMethodHeader;
        std::string httpMcpNameHeader;
        bool        isHttp = false;
    };

    // -----------------------------------------------------------------------
    // Accept 头校验辅助
    // -----------------------------------------------------------------------

    static bool isAcceptValid(std::string_view accept);
    static bool prefersSse(std::string_view accept);

    /// 2026-07-28 Origin 校验: 返回 false 时应以 403 拒绝
    bool isOriginAllowed(std::string_view origin, std::string_view hostHeader) const;

    /// 2026-07-28 HTTP 标准请求头校验 (MCP-Protocol-Version/Mcp-Method/Mcp-Name/
    /// Mcp-Param-*); 返回错误描述 (nullopt = 通过)
    std::optional<std::string> validateModernHeaders(
        const util::HttpServer::Request& req,
        const json&                      requestJson,
        const RequestContext&            ctx
    ) const;

    /// 构造 2026-07-28 服务端能力对象
    json buildCapabilities() const;

    // -----------------------------------------------------------------------
    // 路由设置
    // -----------------------------------------------------------------------

    void setupRoutes();

    // -----------------------------------------------------------------------
    // JSON-RPC 请求处理 (传输无关)
    // -----------------------------------------------------------------------

    json processJsonRpc(const json& requestJson, const RequestContext& ctx);

    /// 从请求 params 中提取协议版本: _meta 优先, 其次 HTTP MCP-Protocol-Version 头
    static std::string extractProtocolVersion(const json& params, const RequestContext& ctx);

    /// 校验协议版本是否受支持; 不支持时返回 -32022 错误响应 (HTTP 400)
    static bool isSupportedProtocolVersion(std::string_view version);

    /// 构造 UnsupportedProtocolVersionError (-32022)
    static json unsupportedVersionError(std::string_view requested);

    /// 构造带 serverInfo 的 result._meta (2026-07-28)
    json serverInfoMeta() const;

    /// 为现代 (2026-07-28) 结果补充 resultType/_meta/缓存字段
    json decorateModernResult(json result, std::string_view method, const json& requestMeta) const;

    // -----------------------------------------------------------------------
    // 主 MCP 请求处理 (HTTP)
    // -----------------------------------------------------------------------

    asio::awaitable<void>
        handleMcpRequest(util::HttpServer::Request& req, util::HttpServer::Response& resp);

    /// POST 统一入口: 普通 handler (resp 非空) 与 POST SSE 路由 (writer 非空) 共用
    asio::awaitable<void> handleMcpPost(
        util::HttpServer::Request&                   req,
        util::HttpServer::Response*                  resp,
        std::shared_ptr<util::HttpServer::SseWriter> writer
    );

    /// POST SSE 路由 (subscriptions/listen 长连接流)
    asio::awaitable<void> handleMcpPostSse(
        util::HttpServer::Request&                   req,
        std::shared_ptr<util::HttpServer::SseWriter> writer
    );

    // -----------------------------------------------------------------------
    // JSON-RPC 方法处理器
    // -----------------------------------------------------------------------

    json handleInitialize(const json& id, const json& params);
    json handlePing(const json& id);
    json handleDiscover(const json& id, const json& params);
    json handleToolsList(const json& id, const json&);
    json handleToolsCall(const json& id, const json& params);
    json handleResourcesList(const json& id, const json&);
    json handleResourcesRead(const json& id, const json& params);
    json handleResourcesSubscribe(const json& id, const json& params);
    json handleResourcesUnsubscribe(const json& id, const json& params);
    json handlePromptsList(const json& id, const json&);
    json handlePromptsGet(const json& id, const json& params);
    json handleLoggingSetLevel(const json& id, const json& params);
    json handleResourceTemplatesList(const json& id, const json&);
    json handleComplete(const json& id, const json&);
    void handleInitialized(const json&);

    /// 2026-07-28 subscriptions/listen — stdio: 注册订阅并立即返回 (无响应体,
    /// ack 经 stdout 下发; 优雅结束时回发空 result)
    std::optional<json> handleSubscriptionsListenStdio(const json& id, const json& params);

    /// 2026-07-28 subscriptions/listen — HTTP: 注册订阅并驱动 SSE 流
    asio::awaitable<void> handleSubscriptionsListenSse(
        const json&                                  id,
        const json&                                  params,
        std::shared_ptr<util::HttpServer::SseWriter> writer
    );

    /// 下发订阅通知 (HTTP 走 SSE 流, stdio 走 stdout)
    void sendSubscriptionNotification(SubscriptionEntry& sub, const json& notification);

    /// 结束订阅 (HTTP: 发空 result 后关闭流; stdio: 发空 result 行)
    void endSubscription(SubscriptionEntry& sub);

    // -----------------------------------------------------------------------
    // SSE 流处理 (旧版 HTTP+SSE 传输, 2024-11-05)
    // -----------------------------------------------------------------------

    asio::awaitable<void> handleSseStream(
        util::HttpServer::Request&                   req,
        std::shared_ptr<util::HttpServer::SseWriter> writer
    );

    // -----------------------------------------------------------------------
    // SSE 通知广播
    // -----------------------------------------------------------------------

    void broadcastSSE(std::string_view event, std::string_view data);
    void stopSSE();

    // -----------------------------------------------------------------------
    // 变更通知事件
    // -----------------------------------------------------------------------

    void notifyToolsChanged();
    void notifyResourcesChanged();
    void notifyPromptsChanged();
    /// 结束所有活跃订阅 (服务端停止/stdio EOF 时调用)
    void endAllSubscriptions();
    /// 等待指定 HTTP 订阅 SSE 协程完成优雅结束 (最多 1s, 防止 stop 阻塞过久)
    void waitSubscriptionsDrained(const std::vector<std::shared_ptr<SubscriptionEntry>>& subs);

    // -----------------------------------------------------------------------
    // 响应输出辅助
    // -----------------------------------------------------------------------

    void writeJsonResponse(
        util::HttpServer::Response& resp,
        boost::beast::http::status  status,
        const json&                 body
    );

    // -----------------------------------------------------------------------
    // 成员
    // -----------------------------------------------------------------------

    Config                            config_;
    std::unique_ptr<util::HttpServer> httpServer_;
    Capabilities                      capabilities_;

    mutable std::shared_mutex                  toolsMutex_;
    std::unordered_map<std::string, ToolEntry> toolsByName_;
    std::atomic<bool>                          toolsListChanged_ = false;

    mutable std::shared_mutex                      resourcesMutex_;
    std::unordered_map<std::string, ResourceEntry> resourcesByUri_;
    std::atomic<bool>                              resourcesListChanged_ = false;

    mutable std::shared_mutex                    promptsMutex_;
    std::unordered_map<std::string, PromptEntry> promptsByName_;
    std::atomic<bool>                            promptsListChanged_ = false;

    std::mutex                      subscribedResourcesMutex_;
    std::unordered_set<std::string> subscribedResources_;

    std::mutex                              sseClientsMutex_;
    std::vector<std::shared_ptr<SSEClient>> sseClients_;

    // 2026-07-28 subscriptions/listen 活跃订阅 (key = idKey)
    std::mutex                                                          subscriptionsMutex_;
    std::unordered_map<std::string, std::shared_ptr<SubscriptionEntry>> subscriptions_;
    std::atomic<uint64_t>                                               subscriptionSeq_{1};

    /// stdio 输出 (runStdio 与订阅通知共用)
    void writeStdioMessage(const json& msg);
};

} // namespace server
} // namespace agentxx
