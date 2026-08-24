// agentxx_string 插件 —— 共享头
// - 从 libagentxx src/tools/string 拆分独立 (同名同行为):
//     agentxx_string_html_to_markdown / agentxx_string_regexp
// - 插件不链接 libagentxx: 日志经宿主 agentxx.agent.log 接口表转发 (替代 XX_LOG);
//   业务逻辑在 string_impl.h (纯函数, 亦供测试直接调用)
// - g_host 由入口 (agentxx_string.cpp) 在 entry 时装配
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include <fmt/format.h>
#include <neograph/json.h>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agentxx_string_plugin {

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

/// 宿主 toolPrompt 条目 {"depict": "...", "args": {...}} 的解析结果
struct ToolPromptText {
    std::string                     depict;
    std::map<std::string, std::string, std::less<>> args;
};

/// 读取宿主 toolPrompt 的完整条目 (depict + 各参数说明; io 线程约束操作,
/// 宿主内部自动投递); 未配置返回空结构
inline ToolPromptText readToolPrompt(const std::string& toolName) {
    ToolPromptText out;
    // 经宿主 get_prompt 整体读取后本地提取 (get_tool_prompt 仅返回 depict/args
    // 单工具条目亦可, 但一次 query 即可拿到全部所需字段, 这里用专用接口)
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
    // 条目 JSON 结构简单固定 ({"depict": str, "args": {k: str}}),
    // 用 neograph::json 解析 (插件经 agentxx_util 传递链接, 与宿主同构)
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

} // namespace agentxx_string_plugin

// 插件内日志统一经 vtable 转发到宿主 (见 pluginLog), 此处重定义 XX_LOG* 宏;
// agentxx/util/log.h (经 util 头间接引入) 也定义了同名宏, 先解除再定义,
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

#define XX_LOGT(...) ::agentxx_string_plugin::pluginLog(0, fmt::format(__VA_ARGS__))
#define XX_LOGD(...) ::agentxx_string_plugin::pluginLog(1, fmt::format(__VA_ARGS__))
#define XX_LOGI(...) ::agentxx_string_plugin::pluginLog(2, fmt::format(__VA_ARGS__))
#define XX_LOGW(...) ::agentxx_string_plugin::pluginLog(3, fmt::format(__VA_ARGS__))
#define XX_LOGE(...) ::agentxx_string_plugin::pluginLog(4, fmt::format(__VA_ARGS__))
