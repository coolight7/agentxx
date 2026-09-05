#pragma once

#include "agentxx/protocol/mcp_server.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/http_client.h"
#include "agentxx/version.h"
#include "asio/awaitable.hpp"
#include <atomic>
#include <chrono>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <neograph/api.h>
#include <neograph/json.h>
#include <neograph/types.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace asio = ::boost::asio;

namespace agentxx {
namespace util {
class AsyncMutex;
} // namespace util

namespace server {

// ---------------------------------------------------------------------------
// McpClient —— 异步 MCP 客户端 (HTTP + stdio 传输)
// 规范: https://modelcontextprotocol.io/docs/2026-07-28/getting-started/intro
// ---------------------------------------------------------------------------

class McpClientTool;

class McpClient : public std::enable_shared_from_this<McpClient> {
public:

    inline static constexpr std::string_view kProtocol2024_11_05   = "2024-11-05";
    inline static constexpr std::string_view kProtocol2025_03_26   = "2025-03-26";
    inline static constexpr std::string_view kProtocol2025_06_18   = "2025-06-18";
    inline static constexpr std::string_view kProtocol2025_11_25   = "2025-11-25";
    inline static constexpr std::string_view kProtocol2026_07_28   = "2026-07-28";
    inline static constexpr std::string_view kSupportedProtocols[] = {
        kProtocol2026_07_28,
        kProtocol2025_11_25,
        kProtocol2025_06_18,
        kProtocol2025_03_26,
        kProtocol2024_11_05,
    };

    struct Config {
        std::string              serverUrl;
        std::vector<std::string> serverCommand;
        std::string              clientName    = "agentxx-mcp-client";
        std::string              clientVersion = std::string{agentxx::kVersion};
        std::string              protocolVersion{kProtocol2026_07_28};
        /// MCP tool 命名空间
        /// - 非空时作为该 client 所有 tool 对外名称的前缀 (格式: "namespace_toolName")
        /// - 远程调用时仍使用 tool 的原始名称
        std::string               toolNamespace;
        std::chrono::milliseconds requestTimeout{60000};
        std::chrono::milliseconds initTimeout{10000};
        /// MCP 工具调用超时限制 (McpClientTool::execute_async 整体超时, 毫秒)
        /// - 0 表示不限制 (无整体超时, 仅受 requestTimeout 等内部超时约束)
        /// - 默认 120 秒; 可在 yaml 中对每个 mcp 单独配置 (单位秒, 0=不限制)
        std::chrono::milliseconds toolCallTimeout{120000};
        util::HeaderMap           extraHeaders;

        bool isHttp() const {
            return !serverUrl.empty();
        }

        bool isStdio() const {
            return !serverCommand.empty();
        }

        bool isValid() const {
            return isHttp() || isStdio();
        }
    };

    struct InitializeResult {
        std::string protocolVersion;
        json        capabilities;
        std::string serverName;
        std::string serverVersion;
    };

    /// 2026-07-28 server/discover 结果
    struct DiscoverResult {
        std::vector<std::string> supportedVersions;
        json                     capabilities;
        std::string              serverName;
        std::string              serverVersion;
        std::string              instructions;
    };

    /// 2026-07-28 subscriptions/listen 通知过滤器
    struct SubscriptionFilter {
        bool                     toolsListChanged     = false;
        bool                     promptsListChanged   = false;
        bool                     resourcesListChanged = false;
        std::vector<std::string> resourceSubscriptions;
    };

    /// 协议时代: Modern = 2026-07-28 及更新 (per-request _meta); Legacy = 2025-11-25 及更早
    enum class ProtocolEra {
        Unknown,
        Modern,
        Legacy,
    };

    explicit McpClient(Config config);
    ~McpClient();

    McpClient(const McpClient&)            = delete;
    McpClient& operator=(const McpClient&) = delete;

    // -----------------------------------------------------------------------
    // 生命周期
    // -----------------------------------------------------------------------

    /// 初始化连接。2026-07-28 (默认) 时先以 server/discover 探测服务端时代:
    /// - 现代服务端: 直接完成初始化 (无 initialize 握手)
    /// - 旧版服务端: 自动回退到 initialize 握手
    asio::awaitable<std::expected<InitializeResult, std::string>> initialize();
    asio::awaitable<void>                                         close();

    bool                    isInitialized() const;
    bool                    isClosed() const;
    const InitializeResult& serverInfo() const;

    /// 当前协商使用的协议版本 (初始化后有效)
    const std::string& protocolVersion() const;

    /// 2026-07-28: server/discover — 查询服务端支持的版本/能力/身份 (可选调用)
    asio::awaitable<std::expected<DiscoverResult, std::string>> discover();

    // -----------------------------------------------------------------------
    // MCP 方法 (工具/资源/提示词)
    // -----------------------------------------------------------------------

    asio::awaitable<std::expected<bool, std::string>> ping();

    asio::awaitable<std::expected<std::vector<McpToolDefinition>, std::string>> listTools();

    asio::awaitable<std::expected<json, std::string>>
        callTool(std::string_view name, const json& arguments = json::object());

    asio::awaitable<std::expected<std::vector<McpResourceDefinition>, std::string>> listResources();

    asio::awaitable<std::expected<McpResourceContent, std::string>>
        readResource(std::string_view uri);

    asio::awaitable<std::expected<std::vector<McpPromptDefinition>, std::string>> listPrompts();

    asio::awaitable<std::expected<McpPromptResult, std::string>>
        getPrompt(std::string_view name, const json& arguments = json::object());

    // -----------------------------------------------------------------------
    // 2026-07-28 协议: subscriptions/listen (变更订阅)
    // -----------------------------------------------------------------------

    /// 打开长连接变更通知流 (仅现代协议)。通知回调:
    /// - stdio: 在子进程读取线程上调用
    /// - HTTP: 在本协程的 io 线程上调用
    /// onEnded 在服务端优雅结束订阅 (回发空 result) 时调用。
    /// HTTP 下该协程保持运行直到订阅结束或外部取消; stdio 下写入请求后立即返回。
    asio::awaitable<std::expected<void, std::string>> listen(
        const SubscriptionFilter&                     filter,
        std::function<void(const json& notification)> onNotification,
        std::function<void()>                         onEnded = {}
    );

    // -----------------------------------------------------------------------
    // 工具适配器工厂
    // -----------------------------------------------------------------------

    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>
        createTools(std::weak_ptr<agentxx::agent::AgentContext> ctx);

    /// 由单个工具定义创建工具适配器
    std::unique_ptr<McpClientTool>
        createTool(McpToolDefinition def, std::weak_ptr<agentxx::agent::AgentContext> ctx);

    // -----------------------------------------------------------------------
    // 内部: JSON-RPC 请求/响应
    // -----------------------------------------------------------------------

private:

    friend class McpClientTool;

    struct PendingRequest {
        std::promise<json> promise;
    };

    static json makeRequest(int64_t id, std::string_view method, const json& params);

    static std::optional<std::string> getErrorFromResponse(const json& response);

    static std::string
        negotiateProtocolVersion(std::string_view requested, const json& serverResult);

    asio::awaitable<std::expected<json, std::string>>
        sendRequest(std::string_view method, const json& params);

    // -----------------------------------------------------------------------
    // 2026-07-28 现代协议辅助 (server/discover + 请求头编码)
    // -----------------------------------------------------------------------

    /// 当前生效的协议版本 (初始化后为协商结果, 否则为配置值)
    std::string effectiveProtocolVersion() const;

    /// 构造现代请求的 _meta (protocolVersion/clientInfo/clientCapabilities)
    json buildModernMeta() const;

    /// 在现代模式下为 params 注入 _meta
    json withModernMeta(const json& params) const;

    /// 探测服务端时代 (Modern/Legacy) 并获取 DiscoverResult
    asio::awaitable<std::expected<DiscoverResult, std::string>> probeModern();

    /// 从服务端支持的版本列表挑选双方共同支持的版本 (优先 requested)
    static std::string
        pickMutualVersion(std::string_view requested, const json& serverSupportedVersions);

    /// 现代 HTTP 请求头 (MCP-Protocol-Version / Mcp-Method / Mcp-Name / Mcp-Param-*)
    util::HeaderMap buildModernHttpHeaders(std::string_view method, const json& params) const;

    /// x-mcp-header 值编码 (Base64 sentinel 规则)
    static std::string encodeMcpHeaderValue(const json& value);

    /// Mcp-Name / Mcp-Param-* 值解码 (Base64 sentinel)
    static std::string decodeMcpHeaderValue(std::string_view value);

    /// 校验工具定义中 x-mcp-header 注解合法性 (非法则拒绝该工具)
    static bool isToolXMcpHeaderValid(const McpToolDefinition& def);

    /// 从工具 inputSchema 提取 x-mcp-header 注解: headerName(小写) -> 信息
    struct XMcpHeaderInfo {
        std::string headerName; // 原始大小写 (发送时使用)
        std::string param;      // 参数名
    };

    static std::unordered_map<std::string, XMcpHeaderInfo>
        extractXMcpHeaders(const McpToolDefinition& def);

    /// 缓存工具定义 (listTools 时填充; callTool 的 x-mcp-header 需要)
    void cacheToolDefinition(const McpToolDefinition& def);

    std::optional<McpToolDefinition> cachedTool(std::string_view name) const;

    // -----------------------------------------------------------------------
    // SSE 端点发现与事件解析 (旧版 HTTP+SSE 传输)
    // -----------------------------------------------------------------------

    /// 由服务器 URL 构造 SSE 端点 URL (末尾追加 /sse)
    static std::string buildSseUrl(std::string_view serverUrl);

    struct SseEvent {
        std::string event;
        std::string data;
    };

    /// 将 SSE 文本解析为 (事件类型, 数据) 列表
    static std::vector<SseEvent> parseSseEvents(std::string_view body);

    /// 从 URL query 中按 key 取值
    static std::string getQueryParam(std::string_view query, std::string_view key);

    /// 连接 SSE URL 发现消息端点 (旧版 HTTP+SSE)
    asio::awaitable<void> discoverSseEndpoint();

    /// 构造 MCP HTTP 请求公共头 (旧版)
    util::HeaderMap buildHttpHeaders() const;

    asio::awaitable<std::expected<json, std::string>>
        sendHttpRequest(int64_t id, std::string_view method, const json& params);

    asio::awaitable<std::expected<json, std::string>>
        sendStdioRequest(int64_t id, std::string_view method, const json& params);

    /// 重新建立 HTTP 会话: session 过期/失效 (如 401 SessionExpired) 后调用。
    /// 清空旧 session → 重新发送 initialize 获取新 Mcp-Session-Id →
    /// 补发 notifications/initialized。成功返回 true。
    asio::awaitable<bool> rebuildHttpSession();

    /// 2026-07-28: 现代 HTTP 请求 (直接 POST serverUrl, 无 SSE discovery/会话)
    asio::awaitable<std::expected<json, std::string>>
        sendModernHttpRequest(int64_t id, std::string_view method, const json& params);

    asio::awaitable<void> sendRawNotification(std::string_view method, const json& params);

    /// stdio 单行写入 (串行化), 返回是否成功
    asio::awaitable<bool> writeStdioLine(const std::string& line);

    // -----------------------------------------------------------------------
    // stdio 子进程管理
    // -----------------------------------------------------------------------

    bool startStdioSubprocess(asio::any_io_executor executor);

    void closeInternal();

    void deliverResponse(const json& response);

    // -----------------------------------------------------------------------
    // 成员
    // -----------------------------------------------------------------------

    Config               config_;
    std::atomic<bool>    initialized_{false};
    std::atomic<bool>    closed_{false};
    InitializeResult     serverInfo_;
    std::atomic<int64_t> nextId_{1};

    // 2026-07-28 协议时代/版本状态
    ProtocolEra era_{ProtocolEra::Unknown};
    std::string negotiatedVersion_;

    // HTTP SSE 传输状态 (旧版 HTTP+SSE)
    std::atomic<bool> sseDiscovered_{false};
    std::string       httpMessageUrl_;
    std::string       mcpSessionId_;

    // stdio 传输状态 (平台差异细节隐藏在 .cpp)
    struct StdioTransport;
    std::unique_ptr<StdioTransport> stdio_;
    /// stdio 写序列化: 协程感知锁, 持锁跨越 co_await async_write 也不死锁 (见 AsyncMutex)
    std::unique_ptr<util::AsyncMutex>                            stdioWriteMutex_;
    std::mutex                                                   pendingMutex_;
    std::unordered_map<int64_t, std::shared_ptr<PendingRequest>> pending_;
    neograph_asio_error_code                                     ignoreEc_;

    // 工具定义缓存 (x-mcp-header 提取)
    mutable std::mutex                                 toolsCacheMutex_;
    std::unordered_map<std::string, McpToolDefinition> toolsCache_;

    // subscriptions/listen 通知分发
    std::mutex                       notifyMutex_;
    std::function<void(const json&)> notificationHandler_;
    std::function<void()>            subscriptionEnded_;
    int64_t                          listenRequestId_ = -1;
};

// ---------------------------------------------------------------------------
// McpClientTool —— 将远端 MCP 工具包装为 XXToolBase (供 agent 工具链调用)
// ---------------------------------------------------------------------------

class McpClientTool : public agentxx::tools::XXToolBase {
public:

    McpClientTool(
        std::shared_ptr<McpClient>                  client,
        McpToolDefinition                           def,
        std::weak_ptr<agentxx::agent::AgentContext> ctx,
        std::string                                 toolNamespace = {}
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;

    std::string get_name() const override;

    /// 命名空间前缀后的对外 tool 名称 (namespace 非空时为 "namespace_toolName")
    std::string namespacedName() const;

private:

    std::shared_ptr<McpClient> client_;
    McpToolDefinition          def_;
    std::string                toolNamespace_;
};

} // namespace server
} // namespace agentxx
