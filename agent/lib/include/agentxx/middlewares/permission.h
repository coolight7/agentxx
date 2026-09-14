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
    /// worktree 根 (规范化目录路径, 尾斜杠): 该子树内读写不受隔离约束
    /// (即不会被下面的 denyWritePath 写拒绝命中; 读写本身仍按已注册规则处理)
    /// - 真实 worktree 位于主检出的 `.agentxx/agent/worktrees/` 下, 故本字段是
    ///   denyWritePath 之内的例外子树, 必须优先判定
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
        std::function<asio::awaitable<bool>(const neograph::Tool& item, agentxx::util::Json& args)>>
        handles{};

    /// 未命中任何已注册规则时 (router 返回 nullptr) 的默认处理操作。
    /// - 默认 ALLOW: 与历史行为一致 (无规则即放行)
    /// - CodeAgent 按配置的 permission.mode 设置:
    ///   ask/all_ask → INTERRUPT (询问), pass → ALLOW, deny → DENY,
    ///   使未注册规则覆盖的路径 (如白名单子树之外的兄弟路径) 也有明确语义
    PermissionOperator noRuleOperator = PermissionOperator::ALLOW;

    PermissionMiddlewareHandle(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    void setFilesystemPermission(std::string_view path, PermissionOperator op, size_t index);

    /// 添加配置文件显式拒绝的路径 (配置中的 permissionDenyPaths / blacklist;
    /// 无论后续是否完全授权, 始终保持拒绝且不询问)
    /// - 内部同时向 [configDenyPermission_] 以及常规 [filesystemPermission] 注册 READ/WRITE 的 DENY
    void addConfigDenyPath(std::string_view path);

    /// 查询指定路径是否命中配置文件显式拒绝的规则 (最长前缀匹配, 支持 * 通配符)
    bool isConfigDenied(std::string_view path, size_t index) const;

    /// 是否已完全授权所有权限 (用户在权限询问中勾选并确认 "完全授权所有权限")
    bool isFullAuthorized() const noexcept {
        return fullAuthorized_;
    }

    /// 设置完全授权状态
    void setFullAuthorized(bool authorized = true) noexcept {
        fullAuthorized_ = authorized;
    }

    // ---------------- worktree 会话隔离边界 (仅 io 线程调用) ----------------

    /// 设置/更新指定会话的隔离边界 (worktree 绑定时由 agentxx_git_worktree 调用)
    /// - 生效规则: 命中 denyWritePath 的写操作直接拒绝 —— 隔离优先于白名单
    ///   与模式默认规则 (与 Claude Code "绑定 worktree 后阻止针对主检出的
    ///   文件编辑" 同语义); 命中 allowPath 的路径是该约束的例外 (worktree
    ///   本身位于主检出内), 按已注册规则照常处理; 读操作与其他路径完全不受影响
    void setSessionIsolation(std::string_view sessionId, SessionFsIsolation isolation);

    /// 清除指定会话的隔离边界 (解绑/删除 worktree 时)
    void clearSessionIsolation(std::string_view sessionId);

    /// 查询会话隔离边界 (未设置返回 nullptr; 测试用)
    const SessionFsIsolation* sessionIsolation(std::string_view sessionId) const;

    /// 权限路径规范化: 绝对路径 (基准 = AgentConfig::resolvedWorkDir, workDir
    /// 未配置时回退进程 cwd) + Unix 分隔符 (+ Windows 转小写)
    /// - 目录路径 (在文件系统中实际存在为目录, 或原路径显式以 '/' 或 '\\' 结尾) 追加/保留尾斜杠
    /// - 文件路径 (在文件系统中实际存在为普通文件, 或实际不存在且原路径未以斜杠结尾) 确保不带尾斜杠,
    ///   避免对文件请求权限时被错误添加末尾 '/'
    std::string normalizePermissionPath(std::string_view path) const;

    /// 同上, 但相对路径解析基准为会话生效工作目录 (worktree 绑定 > 会话
    /// 工作目录覆写, 经 AgentContext::getSessionWorkDir; sessionId 为空时
    /// 等价单参版本)
    std::string normalizePermissionPath(std::string_view path, std::string_view sessionId) const;

    asio::awaitable<bool>
        defOnFilesystemHandle(const neograph::Tool& item, agentxx::util::Json& args, size_t index);

    /// 经会话总线发起权限询问, 并按应答处理"记住本次选择"
    /// - 无 prompter (无 IO 端点注册应答) 或被拒绝时返回 false
    /// - 应答携带 [events::RespPermission::remember] 时, 为本目标注册允许/拒绝
    ///   规则 (作用域由 [index] 决定), 后续同目标及其子路径不再询问
    ///
    /// - `args`:
    ///     - [item]  被检查的 tool (取工具名下发询问)
    ///     - [args]  tool 调用参数 (取 sessionId 定位会话总线; 原样下发)
    ///     - [index] 规则作用域: [FilesystemPermissionREAD] / [FilesystemPermissionWRITE]
    ///     - [target] 受约束目标 (已规范化的绝对路径, 与规则匹配口径一致)
    asio::awaitable<bool> requestPermission(
        const neograph::Tool& item,
        agentxx::util::Json&  args,
        size_t                index,
        std::string           target
    );

    void registerFilesystemHandles();

    void registerHandles();

    ~PermissionMiddlewareHandle() override;

    /// 在 EventBus 上注册权限检查服务与会话隔离订阅
    /// - 须传入 **agent 全局总线** (agentContext->bus): 工具权限检查服务
    ///   (service.permission.check) 与会话隔离事件 (worktree) 订阅都注册在此总线上
    /// - 权限询问 (service.permission) 不同: 由 [requestPermission] 经**会话总线**
    ///   (session->bus) 发起, 因为应答方是绑定到会话的 IO 端点
    /// - 重复调用会先注销上一次注册 (见 unregisterFromBus)
    void registerOnBus(const std::shared_ptr<agentxx::event::EventBus>& bus);

    /// 从 EventBus 注销
    void unregisterFromBus();

private:

    /// 配置文件显式拒绝的路径路由 (优先判定, 无论是否完全授权均保持拒绝)
    XXRouter<PermissionOperator, 2> configDenyPermission_{};

    /// 是否完全授权所有权限 (经由权限询问勾选 fullAuth 且确认允许激活)
    bool fullAuthorized_ = false;

    /// <sessionId, 隔离边界> (仅 io 线程读写, 与中间件链同线程模型, 无需锁)
    std::map<std::string, SessionFsIsolation, std::less<>> sessionIsolations_;

    std::weak_ptr<agentxx::event::EventBus> registeredBus_;
    size_t                                  checkServerId_       = 0;
    size_t                                  setIsolationSubId_   = 0;
    size_t                                  clearIsolationSubId_ = 0;
};

} // namespace middleware
} // namespace agentxx