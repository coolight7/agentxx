// agentxx_rag_search 插件 —— 共享头
// - 从 libagentxx src/tools/rag_search 拆分独立 (同名同行为): agentxx_rag_search
// - 插件不链接 libagentxx: 日志经宿主 agentxx.agent.log 接口表转发 (替代 XX_LOG);
//   业务逻辑在 rag_search_impl.h (纯函数, 亦供测试直接调用)
// - g_host 由入口 (agentxx_rag_search.cpp) 在 entry 时装配
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_tool_sync.h"
#include <fmt/format.h>
#include <neograph/json.h>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agentxx_rag_plugin {

// =====================================================================
// 多实例约定 (2026-08 API v1): 同一动态库可被同一进程内不同 agent 宿主
// 各自加载为独立实例, 本插件不持有任何可变全局; 功能状态一律存于每实例
// PluginCtx (create 创建经 *plugin_ctx 交付宿主, 工具回调经 spec.user_data
// 恢复); 日志经显式传入的 ctx 路由到宿主接口表。
// =====================================================================

/// 每实例上下文 (多实例安全的功能状态载体; VectorStore 等重状态由
/// create 按本实例配置构建 —— 原静态存储在多实例下会串索引)
struct PluginCtx {
    const AgentxxHost*           host    = nullptr;
    agentxx::plugin::AgentIfaces iface   {};
    /// spec 字符串稳定存储 (每实例独立, 卸载随 ctx 释放)
    std::vector<std::string>     storage {};
    /// 同步垫片适配器存储 (每注册工具一个, 随实例销毁释放)
    std::vector<std::unique_ptr<AgentxxSyncToolShim>> sync_tool_shims;
    /// 向量索引 (create 时按本实例配置构建, 实际类型为
    /// agentxx_rag_plugin::VectorStore —— 以不透明指针持有, 避免本头依赖
    /// impl 头引入的日志宏顺序问题; 生命周期随 ctx, create 内 new / destroy 内 delete)
    void*                        store_opaque = nullptr;
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

/// 宿主 toolPrompt 条目 {"depict": "...", "args": {...}} 的解析结果
struct ToolPromptText {
    std::string                                     depict;
    std::map<std::string, std::string, std::less<>> args;
};

/// 读取宿主 toolPrompt 的完整条目 (depict + 各参数说明; io 线程约束操作,
/// 宿主内部自动投递); 未配置返回空结构
inline ToolPromptText readToolPrompt(
    const AgentxxHost*                  host,
    const agentxx::plugin::AgentIfaces& iface,
    const std::string&                  toolName
) {
    ToolPromptText out;
    if (!host || !iface.config || !iface.config->get_tool_prompt) {
        return out;
    }
    char* json = iface.config->get_tool_prompt(
        host,
        agentxx_plugin_sv(toolName.data(), toolName.size())
    );
    if (!json) {
        return out;
    }
    std::string s{json};
    host->vtable->free(json);
    try {
        auto j = neograph::json::parse(s);
        out.depict = j.value("depict", std::string{});
        if (j.contains("args") && j["args"].is_object()) {
            for (const auto& [key, val] : j["args"].items()) {
                if (val.is_string()) {
                    out.args[key] = val.get<std::string>();
                }
            }
        }
    } catch (...) {
        // 解析失败按未配置处理 (回退插件默认描述)
    }
    return out;
}

} // namespace agentxx_rag_plugin

// 插件内日志统一经 vtable 转发到宿主 (见 pluginLog), 此处重定义 XX_LOG* 宏;
// agentxx/util/log.h (rag_search_impl.h 使用) 也定义了同名宏, 先解除再定义,
// 避免重定义警告 (保持本文件内插件日志宏始终生效)
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

#define XX_LOGT(...) ::agentxx_rag_plugin::pluginLog(0, fmt::format(__VA_ARGS__))
#define XX_LOGD(...) ::agentxx_rag_plugin::pluginLog(1, fmt::format(__VA_ARGS__))
#define XX_LOGI(...) ::agentxx_rag_plugin::pluginLog(2, fmt::format(__VA_ARGS__))
#define XX_LOGW(...) ::agentxx_rag_plugin::pluginLog(3, fmt::format(__VA_ARGS__))
#define XX_LOGE(...) ::agentxx_rag_plugin::pluginLog(4, fmt::format(__VA_ARGS__))
