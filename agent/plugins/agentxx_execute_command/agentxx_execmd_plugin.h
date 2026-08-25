// agentxx_execute_command 插件 —— 共享头
// - 从 libagentxx src/tools/execute_command 拆分独立 (同名同行为):
//     agentxx_execute_bash_command / agentxx_execute_windows_command
// - 插件不链接 libagentxx: 日志经宿主 agentxx.agent.log 接口表转发 (替代 XX_LOG)
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_tool_sync.h"
#include <fmt/format.h>
#include <neograph/json.h>
#include <cstring>
#include <string>

namespace agentxx_execmd_plugin {

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

/// 读取会话工作目录 (AgentConfig::resolvedWorkDir; 失败返回空串)
/// - execute 回调运行在宿主线程池, get_work_dir 为 io 线程约束操作,
///   宿主内部自动投递同步等待
inline std::string readWorkDir() {
    if (!g_if.config || !g_if.config->get_work_dir) {
        return {};
    }
    char* dir = g_if.config->get_work_dir(g_host);
    if (!dir) {
        return {};
    }
    std::string out{dir};
    g_host->vtable->free(dir);
    return out;
}

/// 会话取消查询回调 (供子进程取消 watcher 轮询; 会话不存在/未取消返回 false)
inline bool isSessionCancelled(AgentxxPluginStringView thread_id) {
    if (!g_if.cancel || !g_if.cancel->is_cancelled || agentxx_plugin_sv_empty(thread_id)) {
        return false;
    }
    return g_if.cancel->is_cancelled(g_host, thread_id) != 0;
}

/// 读取宿主 toolPrompt 的完整条目 {"depict": "...", "args": {...}}
struct ToolPromptText {
    std::string                                     depict;
    std::map<std::string, std::string, std::less<>> args;
};

inline ToolPromptText readToolPrompt(const std::string& toolName) {
    ToolPromptText out;
    if (!g_host || !g_if.config || !g_if.config->get_tool_prompt) {
        return out;
    }
    char* json = g_if.config->get_tool_prompt(
        g_host,
        agentxx_plugin_sv(toolName.data(), toolName.size())
    );
    if (!json) {
        return out;
    }
    std::string s{json};
    g_host->vtable->free(json);
    try {
        auto j     = neograph::json::parse(s);
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

} // namespace agentxx_execmd_plugin

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

#define XX_LOGT(...) ::agentxx_execmd_plugin::pluginLog(0, fmt::format(__VA_ARGS__))
#define XX_LOGD(...) ::agentxx_execmd_plugin::pluginLog(1, fmt::format(__VA_ARGS__))
#define XX_LOGI(...) ::agentxx_execmd_plugin::pluginLog(2, fmt::format(__VA_ARGS__))
#define XX_LOGW(...) ::agentxx_execmd_plugin::pluginLog(3, fmt::format(__VA_ARGS__))
#define XX_LOGE(...) ::agentxx_execmd_plugin::pluginLog(4, fmt::format(__VA_ARGS__))
