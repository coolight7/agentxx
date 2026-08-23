#pragma once

#include "agentxx/agent/config.h"
#include <asio/any_io_executor.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace middleware {
class SkillMiddlewareHandle;
class MemoryFileMiddlewareHandle;
} // namespace middleware

namespace server {
class McpClient;
}

namespace agent {

/// 完整定义见 context.h (此处前向声明, 成员仅持弱引用)
class AgentContext;

/// 插件资源声明 (plugin.yaml 声明式段; 相对插件目录的路径已解析为绝对路径)
/// - 与主配置 yaml 的 skill/memory/mcp 段同构, 但来源是插件清单,
///   在插件 entry 成功后应用
struct PluginResourceDecls {
    /// skill 扫描目录列表 (目录含 SKILL.md, 或为其父目录 —— 中间件递归扫描)
    std::vector<std::string> skillDirs;

    /// memory 上下文文件列表 (内容注入系统提示词)
    std::vector<std::string> memoryFiles;

    /// MCP server 声明 (key = 工具命名空间; 与主 yaml `mcp` 列表项同构)
    std::map<std::string, McpServerConfig> mcpServers;
};

/// owner 当前生效资源快照 (查询/调试/测试用)
struct AgentResourceSnapshot {
    std::vector<std::string> skillDirs;     ///< 生效的 skill 扫描目录
    std::vector<std::string> memoryFiles;   ///< 生效的 memory 上下文文件
    std::vector<std::string> mcpNamespaces; ///< 已注册 MCP 命名空间 (含连接中)
};

/// 宿主会话组件资源应用器 (Skill / Memory / MCP)
///
/// 职责: 把插件贡献的资源接入宿主管线 (由 agent-io 驱动加载并经
/// appendComponentInfo 上报客户端):
/// - 声明式: PluginManager 在插件 entry 成功后调用 applyDecls
///   (加载失败不会到达此处 → "加载失败则声明资源不生效"天然满足)
/// - 运行时: 插件经 vtable register_skill_dir / register_memory_file /
///   register_mcp_server 实时注册
///
/// 冲突规则: 主程序 yaml 配置优先 —— 声明的 skill 目录 / memory 文件路径
/// 或 MCP 命名空间与主配置已有项重复时跳过并 WARN; 插件之间先到先得。
///
/// 所有权: 每条生效资源记录归属插件名; 卸载时全部摘除 (removeAllOwned),
/// 禁用时摘除但保留记录、启用时恢复 (setOwnerEnabled), 与工具行为一致。
///
/// 线程模型: 全部方法仅可在 agent io 线程调用 (PluginManager vtable 内部
/// 已保证投递); MCP 连接为异步网络 IO —— addMcpServer 仅做查重与登记后立即返回,
/// 连接完成后工具动态进入 ToolRegistry (下一轮 modelcall 对模型可见)。
///
/// 装配: 由 CodeAgent::initMiddleware 构造并注入 AgentContext::resourceApplier
/// (构造参数即该处创建的 Skill/Memory 中间件); BaseAgent 场景无中间件 →
/// 保持 nullptr, 此时插件的资源注册 vtable API 返回非 0 (不支持)。
class AgentResourceApplier : public std::enable_shared_from_this<AgentResourceApplier> {
public:

    AgentResourceApplier(
        std::weak_ptr<AgentContext>                             in_agentContext,
        asio::any_io_executor                                   in_ioExecutor,
        std::shared_ptr<middleware::SkillMiddlewareHandle>      in_skillMiddleware,
        std::shared_ptr<middleware::MemoryFileMiddlewareHandle> in_memoryMiddleware
    );

    /// 应用声明式资源 (entry 成功后调用一次; 逐项冲突检查, 冲突项 WARN 跳过)
    void applyDecls(const std::string& owner, const PluginResourceDecls& decls);

    // ---- 运行时单条注册 (vtable; 返回 false 时 errOut 输出原因) ----

    /// 追加 skill 扫描目录 (绝对路径; 相对路径按程序工作目录解析)
    bool addSkillDir(const std::string& owner, const std::string& absPath, std::string& errOut);

    /// 摘除 skill 扫描目录 (仅可移除本 owner 注册的目录)
    bool removeSkillDir(const std::string& owner, const std::string& absPath);

    /// 追加 memory 上下文文件 (绝对路径)
    bool addMemoryFile(const std::string& owner, const std::string& absPath, std::string& errOut);

    /// 摘除 memory 上下文文件 (仅可移除本 owner 注册的文件)
    bool removeMemoryFile(const std::string& owner, const std::string& absPath);

    /// 注册 MCP server (异步连接; 命名空间查重通过即返回 true)
    bool addMcpServer(
        const std::string&     owner,
        std::string_view       nameSpace,
        const McpServerConfig& cfg,
        std::string&           errOut
    );

    /// 注销 MCP server (断开连接 + 摘除其全部动态工具;
    /// 连接失败后的幂等注销同样成功; 仅限本 owner 注册的命名空间)
    bool removeMcpServer(const std::string& owner, std::string_view nameSpace);

    /// 摘除该 owner 的全部生效资源并清除所有权记录 (卸载 / entry 失败清理)
    void removeAllOwned(const std::string& owner);

    /// 禁用: 摘除全部生效资源但保留记录 (enable 可恢复);
    /// 启用: 按记录重新应用 (重新连接 MCP 等; 与主配置的新冲突项跳过)
    void setOwnerEnabled(const std::string& owner, bool enabled);

    /// owner 当前生效资源快照 (含禁用保留的记录; 查询/调试/测试用)
    AgentResourceSnapshot ownedBy(std::string_view owner) const;

private:

    /// 动态 MCP 条目 (io 线程独占访问)
    struct McpEntry {
        std::string                        owner;
        McpServerConfig                    cfg;
        std::shared_ptr<server::McpClient> client; ///< add 时即创建 (连接身份标识);
                                                   ///< 注销后旧协程经指针比对识别 stale
        enum class Status {
            Connecting,
            Ready
        } status{Status::Connecting};
        std::vector<std::string> toolNames; ///< 已注册进 ToolRegistry 的工具名
        bool abortRequested = false; ///< 连接期间被注销 → 协程各阶段检查后自行退出
    };

    /// owner 的生效资源记录 (disable 摘生效时保留, enable 恢复用)
    struct OwnedRecord {
        PluginResourceDecls applied;
    };

    /// 路径归一化 (weakly_canonical; 失败回退 lexically_normal / 原值)
    static std::string normalizePath(std::string_view p);

    bool addSkillDirImpl(const std::string& owner, std::string normPath, std::string& errOut);
    bool addMemoryFileImpl(const std::string& owner, std::string normPath, std::string& errOut);

    /// 摘除 skill 目录的"生效"部分; keepOwned=true 时保留 owned_ 记录 (disable 用)
    void deactivateSkill(const std::string& owner, const std::string& normPath, bool keepOwned);

    /// 摘除 memory 文件的"生效"部分; keepOwned=true 时保留 owned_ 记录 (disable 用)
    void deactivateMemory(const std::string& owner, const std::string& normPath, bool keepOwned);

    /// 注销动态 MCP: 断连 + 摘除其全部动态工具 (仅 live 条目;
    /// 所有权记录由调用方管理); 返回 false = 命名空间无 live 条目
    bool deactivateMcp(std::string_view nameSpace);

    /// MCP 条目失败清理 (erase + 关闭连接); stale (条目已不存在/已被替换) 时 no-op
    void failMcp(const std::string& nameSpace, const std::shared_ptr<server::McpClient>& client);

    /// 派发异步连接协程 (io executor; 各阶段检查 abort/stale)
    void spawnMcpConnect(std::string nameSpace, std::shared_ptr<server::McpClient> client);

    McpEntry* findMcp(const std::string& nameSpace) {
        auto it = mcpEntries_.find(nameSpace);
        return it == mcpEntries_.end() ? nullptr : &it->second;
    }

    std::weak_ptr<AgentContext>                             agentContext_;
    asio::any_io_executor                                   ioExecutor_;
    std::shared_ptr<middleware::SkillMiddlewareHandle>      skillMiddleware_;
    std::shared_ptr<middleware::MemoryFileMiddlewareHandle> memoryMiddleware_;

    /// owner → 生效资源记录 (io 线程)
    std::map<std::string, OwnedRecord, std::less<>> owned_{};
    /// 动态 MCP 条目 ns → entry (io 线程; 含 Connecting)
    std::map<std::string, McpEntry, std::less<>> mcpEntries_{};
};

} // namespace agent
} // namespace agentxx
