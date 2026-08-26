// agentxx_websearch 插件 —— 共享头
// - 从 libagentxx src/tools/web_search 拆分独立 (同名同行为):
//     agentxx_web_search / agentxx_web_fetch / agentxx_web_fetch_markdown
// - 插件不链接 libagentxx: 日志经宿主 agentxx.agent.log 接口表转发 (替代 XX_LOG)
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_poll_loop.h"
#include "agentxx/plugin/plugin_tool_sync.h"
#include <fmt/format.h>
#include <neograph/json.h>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agentxx_websearch_plugin {

// =====================================================================
// 多实例约定 (2026-08 API v1): 同一动态库可被同一进程内不同 agent 宿主
// 各自加载为独立实例, 本插件不持有任何可变全局; 功能状态一律存于每实例
// PluginCtx (create 创建经 *plugin_ctx 交付宿主, 工具回调经 spec.user_data
// 恢复); 日志经显式传入的 ctx 路由到宿主接口表。
// =====================================================================

/// web_search 配置 (create 时从宿主 model 接口装配; 字段与
/// websearch_impl.h 的 ModelSearchConfig 一致, 回调内按值传递)
struct WebSearchConfig {
    std::string                        baseUrl;
    std::string                        apiKey                  = "EMPTY";
    std::string                        modelName               = "Agentxx";
    int                                readChunkTimeoutSeconds = 100;
};

/// 每实例上下文 (多实例安全的功能状态载体)
struct PluginCtx {
    const AgentxxHost*           host    = nullptr;
    agentxx::plugin::AgentIfaces iface   {};
    /// spec 字符串稳定存储 (每实例独立, 卸载随 ctx 释放)
    std::vector<std::string>     storage {};
    /// 同步垫片适配器存储 (每注册工具一个, 随实例销毁释放; unique_ptr 目标
    /// 地址稳定 —— 注册后三件套回调引用其内容, 容器扩容不失效)
    std::vector<std::unique_ptr<AgentxxSyncToolShim>> sync_tool_shims;
    /// poll 寄生驱动事件循环 (统一异步操作模型: 工具工作协程在其 io_context
    /// 上 spawn, 由宿主 io 线程经 pollOnce 非阻塞步进 —— 与内置工具同线程
    /// 交错执行, 零额外线程; HTTP 等待期不占任何宿主线池线程)
    agentxx::plugin::PollLoop pollLoop {};
    /// poll 寄生驱动垫片适配器存储 (随实例销毁释放; 目标地址稳定)
    std::vector<std::unique_ptr<agentxx::plugin::PolledToolShim>> polled_shims;
    /// agentxx_web_search 配置 (create 时装配; execute 回调只读;
    /// use_model=true 走模型搜索, 否则走 api_url 直连; 两者皆空 = 不注册)
    bool             use_model_search      = false;
    bool             convert_html2markdown = true;
    std::string      search_api_url;
    WebSearchConfig  model_cfg {};
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

/// 读取宿主 toolPrompt 的完整条目 {"depict": "...", "args": {...}}
struct ToolPromptText {
    std::string                                     depict;
    std::map<std::string, std::string, std::less<>> args;
};

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
        auto j      = neograph::json::parse(s);
        out.depict  = j.value("depict", std::string{});
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

} // namespace agentxx_websearch_plugin

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

#define XX_LOGT(...) ::agentxx_websearch_plugin::pluginLog(0, fmt::format(__VA_ARGS__))
#define XX_LOGD(...) ::agentxx_websearch_plugin::pluginLog(1, fmt::format(__VA_ARGS__))
#define XX_LOGI(...) ::agentxx_websearch_plugin::pluginLog(2, fmt::format(__VA_ARGS__))
#define XX_LOGW(...) ::agentxx_websearch_plugin::pluginLog(3, fmt::format(__VA_ARGS__))
#define XX_LOGE(...) ::agentxx_websearch_plugin::pluginLog(4, fmt::format(__VA_ARGS__))
