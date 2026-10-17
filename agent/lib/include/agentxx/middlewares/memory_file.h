#pragma once

#include "agentxx/agent/prompt_sources.h"
#include "agentxx/middlewares/middleware.h"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agentxx {
namespace middleware {

/// 上下文文件中间件状态 (段落文本改由 PromptSourceRegistry 持有, 这里不再存内容)
class MemoryFileMiddlewareState : public BaseMiddlewareState {
public:

    MemoryFileMiddlewareState() {}
};

/// 上下文文件中间件
///
/// - 把配置/插件声明的记忆文件内容作为运行时提示词段落 (键 `memory`) 注入
///   system 消息末尾, 属于稳定前缀 (上游前缀缓存可命中)
/// - 文本由 [agentxx::agent::PromptSourceRegistry] 持有, 本中间件只登记来源并
///   负责取文本; 取文本的时机由 model call 的 start 钩子决定
///   (首次 / 压缩完成 / 资源变更), **不监听文件变化、不按轮次读盘**
/// - 文件路径支持绝对路径或相对路径 (相对路径按会话工作目录解析)
class MemoryFileMiddlewareHandle : public BaseMiddlewareHandle<MemoryFileMiddlewareState> {
protected:

    /// 上下文文件绝对路径列表 (可变: 支持插件运行期追加/摘除 —— 见
    /// addMemoryFiles/removeMemoryFiles; 仅 io 线程读写)
    std::vector<std::string> memoryFilePaths;

    /// 资源注册表 (登记/撤销来源用; 弱引用避免与 AgentContext 形成环)
    std::weak_ptr<agentxx::agent::PromptSourceRegistry> promptSources_;

    /// 文件时间戳快照 (只用于"被要求刷新时省去读盘", 不作为刷新触发条件)
    struct FileStamp {
        uint64_t mtime = 0;
        uint64_t size  = 0;

        bool operator==(const FileStamp& other) const {
            return mtime == other.mtime && size == other.size;
        }
    };

    /// 一个"路径集合"对应的缓存内容
    struct PathSetCache {
        /// 单文件缓存 (内容 + 时间戳; 读取失败时保留上次内容)
        struct FileEntry {
            FileStamp   stamp;
            std::string content;
            bool        ok = false;
        };

        /// 解析后的绝对路径 → 内容
        std::map<std::string, FileEntry, std::less<>> files;
        /// 上次生成的段落文本 (已含 `<MemoryFile>` 包裹)
        std::string text;
        bool        built = false;
    };

    /// 路径集合指纹 → 缓存内容
    /// - 按**解析后的路径集合**缓存: 不同工作目录的会话各取各的, 同一批路径
    ///   只存一份内容 (修复"后启动的会话拿到先启动会话读到的内容"的串台问题)
    std::map<uint64_t, PathSetCache> cacheByPathSet_;

    /// 最近一次加载中超过 [kOversizeWarnChars] 的文件 <路径, 字符数>
    std::vector<std::pair<std::string, size_t>> oversizeFiles_;

    /// 取段落文本 (来源的 build 实现; 见 PromptSource::build)
    asio::awaitable<std::optional<std::string>>
        buildSectionAsync(const agentxx::agent::PromptRefreshContext& ctx);

    /// 读取单个文件 (异步 I/O 可用时异步读, 否则同步兜底)
    /// - `return` 读到的内容 (已转 UTF-8); 失败返回 nullopt 并写入错误文本
    asio::awaitable<std::optional<std::string>>
        readFileAsync(const std::string& absolutePath, std::string& errorOut);

public:

    /// 常驻记忆文件的体积警告阈值 (UTF-8 字符数)
    /// - 记忆文件每轮整份注入系统提示词, 过大会持续占用上下文预算
    /// - 超过阈值只告警提示用户精简, 不截断内容、不改变注入方式
    /// - 推荐的记忆文件形态与 skill 相同: 保持简略、索引式 (写清"有哪些信息、
    ///   放在哪里、怎么取"), 详细内容按需读取, 不要每轮整篇注入
    inline static constexpr size_t kOversizeWarnChars = 8000;

    /// 段落键与归属 (注册表内唯一)
    inline static constexpr std::string_view kSectionKey   = "memory";
    inline static constexpr std::string_view kSectionOwner = "core:memory";

    MemoryFileMiddlewareHandle(
        const std::vector<std::string>&             in_memoryFilePaths,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    ~MemoryFileMiddlewareHandle() override;

    /// 最近一次加载中体积超过 [kOversizeWarnChars] 的 <文件路径, 字符数>
    /// (测试/诊断用; 每次重新读取时重建)
    const std::vector<std::pair<std::string, size_t>>& oversizeMemoryFiles() const {
        return oversizeFiles_;
    }

    // ---------------- 插件资源扩展: 动态增删上下文文件 (仅 io 线程调用) ----------------

    /// 动态追加上下文文件 (插件声明/运行时注册):
    /// - 只改文件列表并标记"资源变更"; 实际读盘在下一个请求装配前发生
    void addMemoryFiles(std::vector<std::string> paths);

    /// 摘除上下文文件 (io 线程; 同上, 只改列表 + 标记)
    void removeMemoryFiles(const std::vector<std::string>& paths);

    /// 当前上下文文件列表 (测试/调试用)
    const std::vector<std::string>& memoryFilePathList() const {
        return memoryFilePaths;
    }
};

} // namespace middleware
} // namespace agentxx
