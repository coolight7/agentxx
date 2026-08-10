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
};

} // namespace middleware
} // namespace agentxx