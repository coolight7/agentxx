// agentxx_filesystem 插件 —— 共享头
// - 从 libagentxx src/tools/filesystem 拆分独立 (同名同行为):
//     agentxx_filesystem_list / read / write / edit / glob / grep
// - 插件不链接 libagentxx: 日志经宿主 agentxx.agent.log 接口表转发 (替代 XX_LOG);
//   业务逻辑在 filesystem_impl.h (纯函数, 亦供测试直接调用)
// - g_host 由入口 (agentxx_filesystem.cpp) 在 entry 时装配
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

namespace agentxx_fs_plugin {

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

/// C ABI 边界异常守卫日志 (由守卫函数调用处显式传入; noexcept —— catch 路径
/// 禁止任何可能再抛异常的操作, 栈缓冲 + 宿主 log 接口表, 缺失时静默丢弃)
inline void pluginCatchLog(const char* msg) noexcept {
    agentxx::plugin_guard::defaultLogTo(g_host, g_if.log, 4, "agentxx_filesystem", msg);
}

/// 宿主 toolPrompt 条目 {"depict": "...", "args": {...}} 的解析结果
struct ToolPromptText {
    std::string                                     depict;
    std::map<std::string, std::string, std::less<>> args;
};

/// 读取宿主 toolPrompt 的完整条目 (depict + 各参数说明; io 线程约束操作,
/// 宿主内部自动投递); 未配置返回空结构
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

/// 会话工作目录 (agentxx.agent.config v2 get_work_dir); entry 时装配一次,
/// execute 回调只读 (嵌入多实例场景下各宿主的解析值在各自进程/实例内有效)
inline const std::string& workDir() {
    static const std::string kWorkDir = []() -> std::string {
        if (!g_host || !g_if.config || !g_if.config->get_work_dir) {
            return {};
        }
        char* dir = g_if.config->get_work_dir(g_host);
        if (!dir) {
            return {};
        }
        std::string s{dir};
        g_host->vtable->free(dir);
        return s;
    }();
    return kWorkDir;
}

} // namespace agentxx_fs_plugin

// 插件内日志统一经 vtable 转发到宿主 (见 pluginLog), 此处重定义 XX_LOG* 宏;
// agentxx/util/log.h (经 util 头间接引入, filesystem_impl.h 使用) 也定义了
// 同名宏, 先解除再定义, 避免重定义警告 (保持本文件内插件日志宏始终生效)
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

#define XX_LOGT(...) ::agentxx_fs_plugin::pluginLog(0, fmt::format(__VA_ARGS__))
#define XX_LOGD(...) ::agentxx_fs_plugin::pluginLog(1, fmt::format(__VA_ARGS__))
#define XX_LOGI(...) ::agentxx_fs_plugin::pluginLog(2, fmt::format(__VA_ARGS__))
#define XX_LOGW(...) ::agentxx_fs_plugin::pluginLog(3, fmt::format(__VA_ARGS__))
#define XX_LOGE(...) ::agentxx_fs_plugin::pluginLog(4, fmt::format(__VA_ARGS__))
