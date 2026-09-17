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

/// 工具权限目标来源 (工具权限声明的一部分; 见 [ToolPermissionSpec])
enum class ToolPermissionTargetKind {
    /// 无目标: 仅做工具级判定 (目标为空, 命中不到规则表, 由 noRuleOperator 兜底)
    None,

    /// 路径: 参数值按会话工作目录规范化为绝对路径后匹配规则 (最长前缀匹配)
    Path,

    /// 文本: 参数值原样作为目标 (如命令/网址), 规则按精确或前缀文本匹配
    Text,
};

/// 工具权限声明: 由插件在注册工具后声明自身工具的权限限制, 宿主据此解析目标
/// 并执行统一规则判定 (白/黑名单、permission.mode 默认规则、记住的选择、
/// 工作区隔离、完全授权)
/// - 权限规则本身不在此结构内: 结构只描述"哪些参数是受约束目标", 判定口径
///   与内置规则一致 (见 [PermissionMiddlewareHandle::checkTargetPermission])
/// - 未声明权限的工具不参与权限判定 (直接放行): 无权限需求的工具无需任何声明
struct ToolPermissionSpec {
    /// 权限作用域 (规则表索引): [PermissionMiddlewareHandle::FilesystemPermissionREAD] /
    /// [PermissionMiddlewareHandle::FilesystemPermissionWRITE]
    size_t scope = 0;

    /// 权限目标来源
    ToolPermissionTargetKind targetKind = ToolPermissionTargetKind::None;

    /// 目标参数名 (工具 args 中的字段名; 依次判定, 任一目标被拒绝即拒绝)
    /// - 目标值按**参数实际 JSON 类型**处理: 字符串视为单个目标, 数组逐项判定
    ///   (如 glob 的 `file_patterns`), 无需额外声明形态
    std::vector<std::string> targetArgs{};

    /// 权限分类文本 (权限询问卡片上显示; 空 = 按作用域生成)
    std::string category{};
};

/// 目标权限判定结果 (三态)
/// - 与"工具调用权限检查"完全同一口径, 区别仅在于 [Ask] 不在判定阶段发起询问:
///   需要询问的场合由调用方决定 (工具调用检查会经总线询问用户, 路径查询接口
///   则把它作为"未获批准"返回, 不弹任何界面)
enum class PathDecision {
    /// 已明确拒绝: 配置黑名单 / 记住的拒绝规则 / 工作区隔离的写边界
    Deny,

    /// 已明确允许: 白名单 / 工作目录规则 / 完全授权 / 模式默认放行 (pass)
    Allow,

    /// 未获批准: 命中 INTERRUPT 规则, 或未命中任何规则且模式默认为询问 (ask/all_ask)
    Ask,
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

    /// 未命中任何已注册规则时 (router 返回 nullptr) 的默认处理操作。
    /// - 默认 ALLOW: 与历史行为一致 (无规则即放行)
    /// - CodeAgent 按配置的 permission.mode 设置:
    ///   ask/all_ask → INTERRUPT (询问), pass → ALLOW, deny → DENY,
    ///   使未注册规则覆盖的路径 (如白名单子树之外的兄弟路径) 也有明确语义
    PermissionOperator noRuleOperator = PermissionOperator::ALLOW;

    PermissionMiddlewareHandle(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    /// 声明工具权限限制 (插件在注册工具后经 PluginManager 调用)
    /// - 同工具重复声明为覆盖 (插件重新 start 时按新声明生效)
    /// - 未声明的工具不参与权限判定 (权限检查直接放行), 与仅加载部分插件的
    ///   场景一致: 工具权限随工具来源 (插件) 走
    void registerToolPermission(std::string_view toolName, ToolPermissionSpec spec);

    /// 撤销工具权限声明 (工具注销/插件禁用卸载时由 PluginManager 调用)
    /// `return` 是否存在被撤销的声明
    bool unregisterToolPermission(std::string_view toolName);

    /// 查询工具权限声明 (未声明返回 nullptr)
    const ToolPermissionSpec* toolPermission(std::string_view toolName) const;

    /// 按已声明的权限判定工具调用是否允许 (未声明权限的工具直接放行)
    /// - 工具调用的统一权限入口: 权限检查服务 (service.permission.check) 与
    ///   直接判定的调用方都走这里
    asio::awaitable<bool> checkToolPermission(std::string_view toolName, agentxx::util::Json& args);

    /// 按声明判定工具调用是否允许 (目标按声明从 args 解析, 依次判定全部目标)
    /// - 声明无目标或目标参数缺省/为空: 退化为工具级判定 (见 [checkTargetPermission])
    asio::awaitable<bool> checkToolPermission(
        std::string_view          toolName,
        agentxx::util::Json&      args,
        const ToolPermissionSpec& spec
    );

    /// 判定单个目标是否允许 (权限规则统一入口)
    /// - 依次: 工作区隔离写拒绝 → 配置拒绝路径 → 完全授权 → 规则表命中
    ///   (ALLOW/DENY/INTERRUPT) → [noRuleOperator] 兜底
    /// - INTERRUPT 时经会话总线询问 (target 为空表示工具级询问)
    ///
    /// - `args`:
    ///     - [toolName] 被检查的工具名 (询问时下发给外部授权者)
    ///     - [args]     tool 调用参数 (取 sessionId 定位会话总线; 原样下发)
    ///     - [scope]    规则作用域: [FilesystemPermissionREAD] / [FilesystemPermissionWRITE]
    ///     - [target]   受约束目标 (已规范化的绝对路径或文本, 与规则匹配口径一致)
    ///     - [category] 权限分类文本 (询问卡片显示; 空 = 按作用域生成)
    asio::awaitable<bool> checkTargetPermission(
        std::string_view     toolName,
        agentxx::util::Json& args,
        size_t               scope,
        std::string_view     target,
        std::string_view     category = {}
    );

    /// 权限分类文本: 声明未指定 category 时按作用域生成
    static std::string_view defaultCategory(size_t scope);

    /// 判定单个路径目标 (纯只读判定: 不发起询问、不产生中断、不修改任何状态)
    /// - 依次: 工作区隔离写拒绝 → 配置拒绝路径 → 完全授权 → 规则表命中
    ///   (ALLOW/DENY/INTERRUPT) → [noRuleOperator] 兜底; INTERRUPT 场合返回
    ///   [PathDecision::Ask] 而不询问
    /// - `path` 须为已规范化的绝对路径 (见 [normalizePermissionPath])
    PathDecision
        decideTarget(std::string_view path, size_t scope, std::string_view sessionId) const;

    /// 批量判定路径 (相对路径按会话生效工作目录规范化; 空路径或规范化失败按
    /// [PathDecision::Ask] 返回: 无法判定时不按"已批准"处理)
    /// - 供插件路径查询接口 (agentxx.agent.permission 的 check_paths) 使用:
    ///   支持模式/前缀参数的插件工具在枚举出实际路径后逐项过滤
    std::vector<PathDecision>
        decidePaths(const std::vector<std::string>& paths, size_t scope, std::string_view sessionId)
            const;

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
    /// - 文件路径 (在文件系统中实际存在为普通文件, 或实际不存在且原路径未以斜杠结尾)
    /// 确保不带尾斜杠,
    ///   避免对文件请求权限时被错误添加末尾 '/'
    std::string normalizePermissionPath(std::string_view path) const;

    /// 同上, 但相对路径解析基准为会话生效工作目录 (worktree 绑定 > 会话
    /// 工作目录覆写, 经 AgentContext::getSessionWorkDir; sessionId 为空时
    /// 等价单参版本)
    std::string normalizePermissionPath(std::string_view path, std::string_view sessionId) const;

    /// 经会话总线发起权限询问, 并按应答处理"记住本次选择"
    /// - 无 prompter (无 IO 端点注册应答) 或被拒绝时返回 false
    /// - 应答携带 [events::RespPermission::remember] 时, 为本目标注册允许/拒绝
    ///   规则 (作用域由 [scope] 决定), 后续同目标及其子路径不再询问
    ///
    /// - `args`:
    ///     - [toolName] 被检查的 tool 名 (询问时下发给外部授权者)
    ///     - [args]  tool 调用参数 (取 sessionId 定位会话总线; 原样下发)
    ///     - [scope] 规则作用域: [FilesystemPermissionREAD] / [FilesystemPermissionWRITE]
    ///     - [target] 受约束目标 (已规范化的绝对路径, 与规则匹配口径一致)
    ///     - [category] 权限分类文本 (询问卡片显示; 空 = 按作用域生成)
    asio::awaitable<bool> requestPermission(
        std::string_view     toolName,
        agentxx::util::Json& args,
        size_t               scope,
        std::string          target,
        std::string_view     category = {}
    );

    ~PermissionMiddlewareHandle() override;

    /// 在 EventBus 上注册权限检查服务与会话隔离订阅
    /// - 须传入 **agent 全局总线** (agentContext->bus): 工具权限检查服务
    ///   (service.permission.check) 与会话隔离事件 (worktree) 订阅都注册在此总线上
    /// - 权限询问 (service.permission) 不同: 由 [requestPermission] 经**会话总线**
    ///   (session->bus) 发起, 因为应答方是绑定到会话的 IO 端点
    /// - 重复调用会先注销上一次注册 (见 unregisterFromBus)
    void registerOnBus(const std::shared_ptr<agentxx::events::EventBus>& bus);

    /// 从 EventBus 注销
    void unregisterFromBus();

private:

    /// 配置文件显式拒绝的路径路由 (优先判定, 无论是否完全授权均保持拒绝)
    XXRouter<PermissionOperator, 2> configDenyPermission_{};

    /// <工具名, 权限声明> (插件注册工具后声明; 仅 io 线程读写, 与中间件链同线程模型)
    std::map<std::string, ToolPermissionSpec, std::less<>> toolPermissions_{};

    /// 是否完全授权所有权限 (经由权限询问勾选 fullAuth 且确认允许激活)
    bool fullAuthorized_ = false;

    /// <sessionId, 隔离边界> (仅 io 线程读写, 与中间件链同线程模型, 无需锁)
    std::map<std::string, SessionFsIsolation, std::less<>> sessionIsolations_;

    std::weak_ptr<agentxx::events::EventBus> registeredBus_;
    size_t                                   checkServerId_       = 0;
    size_t                                   setIsolationSubId_   = 0;
    size_t                                   clearIsolationSubId_ = 0;
};

} // namespace middleware
} // namespace agentxx