/// 插件能力注册表 (宿主侧, 与宿主领域无关)
///
/// 归属: cxx_pluginxx (插件框架内核)。
///
/// 定位: 插件能力的**声明册** —— 记录"能力名 → 提供者插件 + 启动/取消回调 + 回调上下文"。
/// 能力本身是"带 JSON 载荷的 RPC"(见 pluginxx/api/tables.h 的 capabilities 表), 不携带
/// 宿主领域语义, 因此注册表放在内核, agentxx / musicxx 等宿主共用。
///
/// 线程约定: 仅宿主 IO 线程读写 (与插件注册事务同一串行上下文)。
///
/// - 相关: 能力调用与插件归属校验见宿主侧 `PluginManager::invokeCapabilityAsync`
#pragma once

#include "pluginxx/api/tables.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/log.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pluginxx {

/// 实例侧的能力声明记录 (提供者插件自己的登记表, 卸载/禁用时据此逐条撤销)
struct PluginCapabilityRegistration {
    std::string                          name;   ///< 能力名
    AgentxxPluginCapabilityStartFunction start  = nullptr;
    AgentxxPluginOperatorCancelFunction  cancel = nullptr;
    void*                                ctx    = nullptr;
};

/// 能力注册表: 名称 → (提供者, 启动回调, 取消回调, 上下文)
class CapabilityRegistry {
public:

    struct Entry {
        std::string                          provider;
        AgentxxPluginCapabilityStartFunction start  = nullptr;
        AgentxxPluginOperatorCancelFunction  cancel = nullptr;
        void*                                ctx    = nullptr;
    };

    /// 注册能力; 名称重复 (已被任何插件注册) 返回 false 并记日志
    bool registerCapability(
        std::string_view                     name,
        std::string_view                     provider,
        AgentxxPluginCapabilityStartFunction start  = nullptr,
        AgentxxPluginOperatorCancelFunction  cancel = nullptr,
        void*                                ctx    = nullptr
    );

    /// 注销能力; 仅提供者本人可注销 (他人调用返回 false 并记日志)
    bool unregisterCapability(std::string_view name, std::string_view provider);

    bool has(std::string_view name) const;

    /// 查询能力条目; 不存在返回 nullptr
    const Entry* get(std::string_view name) const;

    /// 提供者插件名 (不存在返回空串)
    std::string providerOf(std::string_view name) const;

    /// 全部能力名 (字典序)
    std::vector<std::string> names() const;

private:

    std::map<std::string, Entry, std::less<>> caps_;
};

} // namespace pluginxx
