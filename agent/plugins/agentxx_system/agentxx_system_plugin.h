// agentxx_system 插件 —— 共享头
// - 从 libagentxx src/tools/system 拆分独立 (同名同行为):
//     agentxx_get_current_datetime
// - 插件不链接 libagentxx: 日志经宿主 agentxx.agent.log 接口表转发 (替代 XX_LOG)
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include <fmt/format.h>
#include <neograph/json.h>
#include <cstring>
#include <string>

namespace agentxx_system_plugin {

/// 当前插件宿主句柄 (entry 装配; 线程安全: 只读)
inline const AgentxxHost* g_host = nullptr;

/// 宿主接口表缓存 (entry 时 AgentIfaces::query 一次查询; 表为进程级静态数据)
inline agentxx::plugin::AgentIfaces g_if{};

/// 日志转发到宿主 agentxx.agent.log 接口表 (线程安全)
inline void pluginLog(int level, const std::string& msg) {
    if (g_host && g_if.log && g_if.log->log) {
        g_if.log->log(g_host, level, agentxx_plugin_sv(msg.data(), msg.size()));
    }
}

/// 宿主分配字符串 (跨边界内存)
inline char* pluginStrdup(const char* s) {
    if (!s) {
        return nullptr;
    }
    return g_host->vtable->strdup(s);
}

/// 读取宿主 toolPrompt 的 depict; 未配置返回空
inline std::string readToolDepict(const std::string& toolName) {
    if (!g_host || !g_if.config || !g_if.config->get_tool_prompt) {
        return {};
    }
    char* json = g_if.config->get_tool_prompt(
        g_host,
        agentxx_plugin_sv(toolName.data(), toolName.size())
    );
    if (!json) {
        return {};
    }
    std::string s{json};
    g_host->vtable->free(json);
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
