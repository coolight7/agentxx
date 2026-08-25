#pragma once

#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/router.h"
#include "asio/io_context.hpp"
#include <functional>
#include <map>
#include <memory>
#include <neograph/neograph.h>
#include <string>
#include <string_view>

namespace agentxx {
namespace middleware {

enum class PermissionOperator {
    /// 允许
    ALLOW,

    /// 拒绝
    DENY,

    /// 中断,询问用户是否同意
    INTERRUPT,
};

class PermissionMiddlewareState : public BaseMiddlewareState {
public:

    PermissionMiddlewareState() {}
};

/// 每会话文件系统隔离边界 (worktree 模式; 见 setSessionIsolation)
struct SessionFsIsolation {
    /// worktree 根 (规范化目录路径, 尾斜杠): 该子树内读写放行
    std::string allowPath;
    /// 主检出仓库根 (规范化目录路径, 尾斜杠): 该子树内写操作拒绝 (读不受限)
    std::string denyWritePath;
};

class PermissionMiddlewareHandle : public BaseMiddlewareHandle<PermissionMiddlewareState> {
protected:
public:

    inline static constexpr size_t FilesystemPermissionREAD  = 0;
    inline static constexpr size_t FilesystemPermissionWRITE = 1;

    /// 遵循最长路径匹配，支持 * 通配符
    XXRouter<PermissionOperator, 2> filesystemPermission{};
    /// <name, handle>
    std::map<
        std::string,
        std::function<asio::awaitable<bool>(const neograph::Tool& item, neograph::json& args)>>
        handles{};

    /// 未命中任何已注册规则时 (router 返回 nullptr) 的默认处理操作。
    /// - 默认 ALLOW: 与历史行为一致 (无规则即放行)
    /// - CodeAgent 按配置的 permission.mode 设置:
    ///   ask/all_ask → INTERRUPT (询问), pass → ALLOW, deny → DENY,
    ///   使未注册规则覆盖的路径 (如白名单子树之外的兄弟路径) 也有明确语义
    PermissionOperator noRuleOperator = PermissionOperator::ALLOW;

    PermissionMiddlewareHandle(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    void setFilesystemPermission(std::string_view path, PermissionOperator op, size_t index);

    // ---------------- worktree 会话隔离边界 (仅 io 线程调用) ----------------

    /// 设置/更新指定会话的隔离边界 (worktree 绑定时由 agentxx_git_worktree 调用)
    /// - 生效规则: 命中 denyWritePath 的写操作直接拒绝 —— 隔离优先于白名单
    ///   与模式默认规则 (与 Claude Code "绑定 worktree 后阻止针对主检出的
    ///   文件编辑" 同语义); 读操作与其他路径完全不受影响
    void setSessionIsolation(std::string_view sessionId, SessionFsIsolation isolation);

    /// 清除指定会话的隔离边界 (解绑/删除 worktree 时)
    void clearSessionIsolation(std::string_view sessionId);

    /// 查询会话隔离边界 (未设置返回 nullptr; 测试用)
    const SessionFsIsolation* sessionIsolation(std::string_view sessionId) const;

    /// 权限路径规范化: 绝对路径 (基准 = AgentConfig::resolvedWorkDir, workDir
    /// 未配置时回退进程 cwd) + Unix 分隔符 + 目录尾斜杠 (+ Windows 转小写)
    std::string normalizePermissionPath(std::string_view path) const;

    /// 同上, 但相对路径解析基准为会话生效工作目录 (worktree 绑定优先,
    /// 经 AgentContext::resolveSessionWorkDir; sessionId 为空时等价单参版本)
    std::string normalizePermissionPath(std::string_view path, std::string_view sessionId) const;

    asio::awaitable<bool>
        defOnFilesystemHandle(const neograph::Tool& item, neograph::json& args, size_t index);

    /// 经总线发起权限询问; 无 prompter 或被拒绝时返回 false
    asio::awaitable<bool> requestPermission(
        const neograph::Tool& item,
        neograph::json&       args,
        std::string           category,
        std::string           target
    );

    void registerFilesystemHandles();

    void registerHandles();

private:

    /// <sessionId, 隔离边界> (仅 io 线程读写, 与中间件链同线程模型, 无需锁)
    std::map<std::string, SessionFsIsolation, std::less<>> sessionIsolations_;
};

} // namespace middleware
} // namespace agentxx