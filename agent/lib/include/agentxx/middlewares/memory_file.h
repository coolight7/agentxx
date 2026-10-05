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

    /// 最近一次加载中超过 [kOversizeWarnChars] 的文件 <路径, 字符数>
    std::vector<std::pair<std::string, size_t>> oversizeFiles_;

public:

    /// 常驻记忆文件的体积警告阈值 (UTF-8 字符数)
    /// - 记忆文件每轮整份注入系统提示词, 过大会持续占用上下文预算
    /// - 超过阈值只告警提示用户精简, 不截断内容、不改变注入方式
    /// - 推荐的记忆文件形态与 skill 相同: 保持简略、索引式 (写清"有哪些信息、
    ///   放在哪里、怎么取"), 详细内容按需读取, 不要每轮整篇注入
    inline static constexpr size_t kOversizeWarnChars = 8000;

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

    /// 最近一次加载中体积超过 [kOversizeWarnChars] 的 <文件路径, 字符数>
    /// (测试/诊断用; 每次全量重读时重建)
    const std::vector<std::pair<std::string, size_t>>& oversizeMemoryFiles() const {
        return oversizeFiles_;
    }

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
