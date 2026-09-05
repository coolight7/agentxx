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

/// A2A (Agent-to-Agent) 客户端
///
/// 通过 JSON-RPC 协议连接远程 A2A 服务器:
///   - 获取 Agent Card (服务发现)
///   - 发送消息 (同步)
///   - 获取 / 列表 / 取消任务
class A2aClient {
public:

    struct Config {
        std::string          baseUrl;                                   ///< A2A 服务器基础地址
        std::string          a2aEndpoint   = "/a2a";                    ///< A2A JSON-RPC 端点路径
        std::string          agentCardPath = "/.well-known/agent-card.json"; ///< Agent Card 路径
        std::chrono::seconds requestTimeout{60};                        ///< 请求超时
        std::string          protocolVersion = "1.0";                   ///< 协议版本
    };

    explicit A2aClient(Config config);

    A2aClient(const A2aClient&)            = delete;
    A2aClient& operator=(const A2aClient&) = delete;

    // -----------------------------------------------------------------------
    // Agent 发现
    // -----------------------------------------------------------------------

    /// 获取远端 Agent Card (描述其能力/技能/提供商信息)
    asio::awaitable<std::expected<json, std::string>> fetchAgentCard();

    // -----------------------------------------------------------------------
    // 核心操作
    // -----------------------------------------------------------------------

    /// 发送一条文本消息; [taskId] 非空时续接已有任务, [contextId] 用于
    /// 保持会话上下文 (A2A context)
    asio::awaitable<std::expected<json, std::string>> sendMessage(
        std::string_view text,
        std::string_view taskId    = "",
        std::string_view contextId = ""
    );

    /// 查询任务状态; [historyLength] 指定返回多少条历史消息 (0 = 不带历史)
    asio::awaitable<std::expected<json, std::string>>
        getTask(std::string_view taskId, int historyLength = 0);

    /// 列出任务 ([contextId]/[status] 过滤, [pageSize] 分页)
    asio::awaitable<std::expected<json, std::string>>
        listTasks(std::string_view contextId = "", std::string_view status = "", int pageSize = 50);

    /// 取消任务
    asio::awaitable<std::expected<json, std::string>> cancelTask(std::string_view taskId);

    // -----------------------------------------------------------------------
    // 底层 JSON-RPC 调用 (公开供测试)
    // -----------------------------------------------------------------------

    /// 直接发起 JSON-RPC 请求 ([method] 方法名, [params] 参数对象)
    asio::awaitable<std::expected<json, std::string>> rpcCall(std::string_view method, json params);

    // -----------------------------------------------------------------------
    // 工具函数
    // -----------------------------------------------------------------------

    /// 构造文本消息 JSON (taskId/contextId 非空时附带)
    static json buildTextMessage(
        std::string_view text,
        std::string_view taskId    = "",
        std::string_view contextId = ""
    );

    /// 从 sendMessage 结果中提取 taskId
    static std::string extractTaskId(const json& sendMessageResult);
    /// 从任务 JSON 中提取状态字符串
    static std::string extractTaskState(const json& task);
    /// 从任务 JSON 中提取最终的文本产物
    static std::string extractArtifactText(const json& task);

private:

    Config  config_;
    int64_t nextId_ = 1;
};

} // namespace server
} // namespace agentxx
