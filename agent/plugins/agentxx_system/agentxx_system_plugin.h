// agentxx_system 插件 —— 共享头
// - 从 libagentxx src/tools/system 拆分独立 (同名同行为):
//     agentxx_get_current_datetime
// - 插件不链接 libagentxx: 日志经宿主 agentxx.agent.log 接口表转发 (替代 XX_LOG)
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_tool_sync.h"
#include <fmt/format.h>
#include <neograph/json.h>
#include <cstring>
#include <string>

namespace agentxx_system_plugin {

// =====================================================================
// 多实例约定 (2026-08 API v1): 同一动态库可被同一进程内不同 agent 宿主
// 各自加载为独立实例, 本插件不持有任何可变全局; 功能状态一律存于每实例
// PluginCtx (create 创建经 *plugin_ctx 交付宿主, 工具回调经 spec.user_data
// 恢复); 日志经显式传入的 ctx 路由到宿主接口表。
// =====================================================================

/// 每实例上下文 (多实例安全的功能状态载体)
struct PluginCtx {
    const AgentxxHost*           host    = nullptr;
    agentxx::plugin::AgentIfaces iface   {};
    /// spec 字符串 (depict/schema) 稳定存储 (每实例独立, 卸载随 ctx 释放)
    std::vector<std::string>     storage {};
    /// 同步垫片适配器存储 (每注册工具一个, 随实例销毁释放; unique_ptr 目标
    /// 地址稳定 —— 注册后三件套回调引用其内容, 容器扩容不失效)
    std::vector<std::unique_ptr<AgentxxSyncToolShim>>   sync_tool_shims;
    std::vector<std::unique_ptr<AgentxxInlineToolShim>> inline_tool_shims;
};

/// 实例日志转发到宿主 agentxx.agent.log 接口表 (ctx 可空时静默丢弃)
inline void pluginLog(const PluginCtx* ctx, int level, const std::string& msg) {
    if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
        ctx->iface.log->log(ctx->host, level, agentxx_plugin_sv(msg.data(), msg.size()));
    }
}

/// 宿主分配字符串 (跨边界内存; host 取自回调恢复的 PluginCtx)
inline char* pluginStrdup(const AgentxxHost* host, const char* s) {
    if (!host || !s) {
        return nullptr;
    }
    return host->vtable->strdup(s);
}

/// C ABI 边界异常守卫日志闭包 (捕获本实例上下文; ctx 为空 = 尚未装配,
/// 静默丢弃 —— 仅 create 最前段可能发生)
inline auto ctxGuardLogger(PluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept { pluginLog(ctx, 4, msg ? msg : ""); };
}

/// 读取宿主 toolPrompt 的 depict; 未配置返回空 (host/iface 取自本实例 ctx)
inline std::string readToolDepict(
    const AgentxxHost*                  host,
    const agentxx::plugin::AgentIfaces& iface,
    const std::string&                  toolName
) {
    if (!host || !iface.config || !iface.config->get_tool_prompt) {
        return {};
    }
    char* json = iface.config->get_tool_prompt(
        host,
        agentxx_plugin_sv(toolName.data(), toolName.size())
    );
    if (!json) {
        return {};
    }
    std::string s{json};
    host->vtable->free(json);
    try {
        auto j = neograph::json::parse(s);
        return j.value("depict", std::string{});
    } catch (...) {
        return {};
    }
}

} // namespace agentxx_system_plugin

#ifdef XX_LOGT
#undef XX_LOGT
#endif
#ifdef XX_LOGD
#undef XX_LOGD
#endif
#ifdef XX_LOGI
#undef XX_LOGI
#endif
#ifdef XX_LOGW
#undef XX_LOGW
#endif
#ifdef XX_LOGE
#undef XX_LOGE
#endif

#define XX_LOGT(...) ::agentxx_system_plugin::pluginLog(0, fmt::format(__VA_ARGS__))
#define XX_LOGD(...) ::agentxx_system_plugin::pluginLog(1, fmt::format(__VA_ARGS__))
#define XX_LOGI(...) ::agentxx_system_plugin::pluginLog(2, fmt::format(__VA_ARGS__))
#define XX_LOGW(...) ::agentxx_system_plugin::pluginLog(3, fmt::format(__VA_ARGS__))
#define XX_LOGE(...) ::agentxx_system_plugin::pluginLog(4, fmt::format(__VA_ARGS__))
