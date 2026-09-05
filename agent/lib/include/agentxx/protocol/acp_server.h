#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <neograph/json.h>

#include "agentxx/agent/base_agent.h"
#include "agentxx/util/http_server.h"

namespace agentxx {
namespace server {

using json = neograph::json;

// ---------------------------------------------------------------------------
// ACP 协议处理器 (HTTP 与 stdio 传输共用)
//
// 实现 Agent Client Protocol 的 JSON-RPC 2.0 子集:
//   - initialize        – 握手, 返回 capabilities 与 agent 信息
//   - session/new       – 创建会话
//   - session/prompt    – 提交提示 (异步, 结果经 notification 返回)
//   - session/cancel    – 取消进行中的 prompt
// ---------------------------------------------------------------------------

class AcpProtocolHandler {
public:

    struct Config {
        std::string serverName         = "agentxx-acp";
        std::string serverVersion      = "0.1.0";
        int         maxInflightPrompts = 32;
    };

    using NotificationSink = std::function<void(const json&)>;

    AcpProtocolHandler(
        std::shared_ptr<agentxx::agent::BaseAgent> agent,
        json                                       agentInfo,
        Config                                     config
    );

    AcpProtocolHandler(const AcpProtocolHandler&)            = delete;
    AcpProtocolHandler& operator=(const AcpProtocolHandler&) = delete;

    ~AcpProtocolHandler();

    /// 是否已收到 initialize 握手
    bool initialized() const;

    /// 本 server 上报的 agent 信息 JSON (能力声明等)
    const json& agentInfo() const;

    /// 设置通知接收器 (用于异步结果与流式更新); 处理消息前必须先设置
    void setNotificationSink(NotificationSink sink);

    /// 是否已请求停止 (stop 后为 true)
    bool stopRequested() const;

    /// 停止所有进行中的 prompt 并取消挂起请求
    void stop();

    /// 处理一条 JSON-RPC 信封。同步方法返回响应信封;
    /// 通知/异步派发返回空 json。异步响应经 notification sink 投递。
    json handleMessage(const json& env);

    /// 发出一个出站请求 (agent→client) 并等待响应。
    /// 调用前必须先设置 notification sink。
    json callClient(
        std::string_view          method,
        json                      params,
        std::chrono::milliseconds timeout = std::chrono::seconds{30}
    );

    // -- 会话查询 (供测试/内省) ---------------------------------------------

    bool        hasSession(std::string_view sessionId) const;
    std::string sessionCwd(std::string_view sessionId) const;
    bool        isInFlight(std::string_view sessionId) const;
    int         inflightCount() const;

    /// 等待进行中的 worker 排空 (传输层关闭前使用)
    void drainWorkers();

    // -----------------------------------------------------------------------
    // JSON-RPC 工具
    // -----------------------------------------------------------------------

    static json        jsonRpcResult(const json& id, json result);
    static json        jsonRpcError(const json& id, int code, std::string_view msg);
    static json        makeParseError(std::string_view detail);
    static json        makeInvalidRequest();
    static std::string extractUserText(const json& prompt);
    static std::string generateSessionId();

    // -----------------------------------------------------------------------
    // 各 JSON-RPC 方法处理器
    // -----------------------------------------------------------------------

    json handleInitialize(const json& params, const json& id);
    json handleSessionNew(const json& params, const json& id);
    void handleSessionPrompt(const json& env, const json& params, const json& id);
    void workerRunPrompt(
        std::string_view                   sessionId,
        const json&                        promptBlocks,
        const json&                        id,
        std::shared_ptr<std::atomic<bool>> cancelFlag
    );
    void workerCleanup(std::string_view sessionId);
    void handleSessionCancel(const json& params);

    // -----------------------------------------------------------------------
    // 通知 / 流式输出工具
    // -----------------------------------------------------------------------

    void emit(const json& env);
    void emitNotification(std::string_view method, const json& params);
    void emitAgentMessageChunk(std::string_view sessionId, std::string_view text);

private:

    // -----------------------------------------------------------------------
    // 成员
    // -----------------------------------------------------------------------

    Config                                     config_;
    std::shared_ptr<agentxx::agent::BaseAgent> agent_;
    json                                       agentInfo_;
    std::atomic<bool>                          initialized_{false};
    std::atomic<bool>                          stopFlag_{false};
    NotificationSink                           sink_;

    // -- 会话表 --
    mutable std::mutex                              sessionsMu_;
    std::map<std::string, std::string, std::less<>> sessions_; // sessionId → 工作目录
    std::map<std::string, std::shared_ptr<std::atomic<bool>>, std::less<>> cancelFlags_;

    // -- 进行中 prompt 追踪 --
    mutable std::mutex                 inflightMu_;
    std::set<std::string, std::less<>> inflightSessions_;
    std::atomic<int>                   inflightCount_{0};

    std::mutex              workersMu_;
    std::condition_variable workersCv_;

    // -- 出站请求追踪 (agent→client) --
    mutable std::mutex                                               pendingMu_;
    std::map<int64_t, std::shared_ptr<std::promise<neograph::json>>> pending_;
    std::atomic<int64_t>                                             nextOutboundId_{1};
};

// ===========================================================================
// HTTP ACP Server
//
// 以 HTTP 传输包装 AcpProtocolHandler。
//   POST /acp    – 主 JSON-RPC 端点
//   GET /acp/sse – SSE 端点 (流式通知)
// ===========================================================================

class HttpAcpServer {
public:

    struct Config {
        util::HttpServer::Config httpConfig;
        std::string              acpEndpoint   = "/acp";
        std::string              sseEndpoint   = "/acp/sse";
        std::string              serverName    = "agentxx-acp";
        std::string              serverVersion = "0.1.0";
        std::chrono::seconds     asyncTimeout{120};
    };

    HttpAcpServer(
        std::shared_ptr<agentxx::agent::BaseAgent> agent,
        neograph::json                             agentInfo,
        Config                                     config
    );

    HttpAcpServer(const HttpAcpServer&)            = delete;
    HttpAcpServer& operator=(const HttpAcpServer&) = delete;

    ~HttpAcpServer();

    /// 启动 HTTP 监听 (阻塞到 startAsync 模式调度)
    void start();
    /// 停止服务并回收
    void stop();

    /// 实际监听端口 (自动分配端口时用)
    uint16_t port() const;
    /// 是否已停止
    bool     isStopped() const;

    /// 底层协议处理器引用
    AcpProtocolHandler& handler();

private:

    // -----------------------------------------------------------------------
    // 将 handler 的 notification sink 接到 SSE 广播与 pending resolver
    // -----------------------------------------------------------------------

    void setupHandlerSink();

    // -----------------------------------------------------------------------
    // 路由注册
    // -----------------------------------------------------------------------

    void setupRoutes();

    // -----------------------------------------------------------------------
    // ACP 请求处理 (HTTP JSON-RPC)
    // -----------------------------------------------------------------------

    asio::awaitable<void>
        handleAcpRequest(util::HttpServer::Request& req, util::HttpServer::Response& resp);

    // -----------------------------------------------------------------------
    // SSE 端点
    // -----------------------------------------------------------------------

    asio::awaitable<void>
        handleSseRequest(util::HttpServer::Request& req, util::HttpServer::Response& resp);

    void broadcastSSE(std::string_view /*data*/);
    void stopSSE();

    // -----------------------------------------------------------------------
    // HTTP 响应工具
    // -----------------------------------------------------------------------

    void writeJsonResponse(
        util::HttpServer::Response& resp,
        boost::beast::http::status  status,
        const neograph::json&       body
    );

    neograph::json jsonRpcError(const neograph::json& id, int code, std::string_view message) const;

    // -----------------------------------------------------------------------
    // 成员
    // -----------------------------------------------------------------------

    Config                                     config_;
    std::shared_ptr<agentxx::agent::BaseAgent> agent_;
    AcpProtocolHandler                         handler_;
    std::unique_ptr<util::HttpServer>          httpServer_;

    // 挂起的异步响应追踪 (HTTP 传输用)
    std::mutex                                                       pendingMutex_;
    std::map<int64_t, std::shared_ptr<std::promise<neograph::json>>> pendingResponses_;
};

// ===========================================================================
// Stdio ACP Server
//
// 从输入流读取换行分隔的 JSON-RPC 并写入输出流 (默认 std::cin / std::cout)。
// ===========================================================================

class StdioAcpServer {
public:

    StdioAcpServer(std::shared_ptr<agentxx::agent::BaseAgent> agent, neograph::json agentInfo);

    StdioAcpServer(const StdioAcpServer&)            = delete;
    StdioAcpServer& operator=(const StdioAcpServer&) = delete;

    ~StdioAcpServer();

    void run();
    void run(std::istream& in, std::ostream& out);

    void stop();

    bool isRunning() const;

    AcpProtocolHandler& handler();

private:

    std::shared_ptr<agentxx::agent::BaseAgent> agent_;
    AcpProtocolHandler                         handler_;
    std::atomic<bool>                          running_{false};
};

} // namespace server
} // namespace agentxx
