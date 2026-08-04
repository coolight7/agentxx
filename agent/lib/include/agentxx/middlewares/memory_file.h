#pragma once

#include "agentxx/middlewares/middleware.h"
#include <string>
#include <vector>

namespace agentxx {
namespace middleware {

/// 上下文文件中间件
/// - 在首次 agent 调用时读取配置的上下文文件并缓存内容
/// - 在每次模型调用时将文件内容注入系统提示词
/// - 文件路径支持绝对路径或相对路径（由调用方在传入前解析为绝对路径）
class MemoryFileMiddlewareState : public BaseMiddlewareState {
public:

    /// 缓存的上下文文件内容拼接结果
    std::string cacheContextContent;

    MemoryFileMiddlewareState() {}
};

class MemoryFileMiddlewareHandle : public BaseMiddlewareHandle<MemoryFileMiddlewareState> {
protected:

    /// 初始化后固定，上下文文件绝对路径列表
    const std::vector<std::string> initMemoryFilePaths;

    /// <path, content> 缓存
    std::vector<std::pair<std::string, std::string>> fileContents{};
    bool                                             haveLoaded = false;

public:

    MemoryFileMiddlewareHandle(
        const std::vector<std::string>&             in_memoryFilePaths,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    ) :
        BaseMiddlewareHandle<MemoryFileMiddlewareState>(
            "MemoryFileMiddlewareHandle",
            in_agentContext
        ),
        initMemoryFilePaths(in_memoryFilePaths) {}

    asio::awaitable<void> onAgentcallStartFunc(neograph::graph::NodeInput& in) override;
};

} // namespace middleware
} // namespace agentxx
