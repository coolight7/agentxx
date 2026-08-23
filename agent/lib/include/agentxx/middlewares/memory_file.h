#pragma once

#include "agentxx/middlewares/middleware.h"
#include <cstdint>
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

    /// 生成缓存时的资源纪元 (MemoryFileMiddlewareHandle::resourceEpoch;
    /// 插件运行期增删上下文文件后纪元递增, 缓存据此失效重建)
    uint64_t cachedResourceEpoch = 0;

    MemoryFileMiddlewareState() {}
};

class MemoryFileMiddlewareHandle : public BaseMiddlewareHandle<MemoryFileMiddlewareState> {
protected:

    /// 上下文文件绝对路径列表 (可变: 支持插件运行期追加/摘除 —— 见
    /// addMemoryFiles/removeMemoryFiles; 仅 io 线程读写)
    std::vector<std::string> memoryFilePaths;

    /// <path, content> 缓存
    std::vector<std::pair<std::string, std::string>> fileContents{};
    bool                                             haveLoaded = false;

    /// 是否需要重读 (插件运行期增删文件后置位; 下次 onAgentcallStartFunc 全量重读)
    bool needReloadMemoryFiles = false;
    /// 资源纪元 (文件列表变更时递增; 各线程状态缓存据此失效重建)
    /// - 初始为 1 (状态侧 cachedResourceEpoch 默认 0, 保证首轮必定生成缓存)
    uint64_t resourceEpoch = 1;

public:

    MemoryFileMiddlewareHandle(
        const std::vector<std::string>&             in_memoryFilePaths,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    ) :
        BaseMiddlewareHandle<MemoryFileMiddlewareState>(
            "MemoryFileMiddlewareHandle",
            in_agentContext
        ),
        memoryFilePaths(in_memoryFilePaths) {}

    asio::awaitable<void> onAgentcallStartFunc(neograph::graph::NodeInput& in) override;

    // ---------------- 插件资源扩展: 动态增删上下文文件 (仅 io 线程调用) ----------------

    /// 动态追加上下文文件 (插件声明/运行时注册):
    /// - 未加载: 直接并入列表, 首轮懒加载自然包含
    /// - 已加载: 置重载标记 + 递增纪元, 下次轮次重新读取全部文件
    void addMemoryFiles(std::vector<std::string> paths);

    /// 摘除上下文文件并置重载标记 (io 线程; 缓存随下次轮次重建)
    void removeMemoryFiles(const std::vector<std::string>& paths);

    /// 当前上下文文件列表 (测试/调试用)
    const std::vector<std::string>& memoryFilePathList() const {
        return memoryFilePaths;
    }
};

} // namespace middleware
} // namespace agentxx
