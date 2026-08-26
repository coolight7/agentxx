// agentxx_text_selection_monitor 插件 —— 共享头
// - 插件不链接 libagentxx: 日志经宿主 agentxx.agent.log 接口表转发 (替代 XX_LOG)
// - 平台宏: XX_IS_WIN_D / XX_IS_LINUX_D / XX_IS_MACOS_D 本地推导
//   (libagentxx 由 util.h 提供, 插件独立编译需自备)
// - g_host 由入口 (agentxx_text_selection_monitor.cpp) 在 entry 时装配
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_tool_sync.h"
#include "fmt/format.h"
#include "simdjson.h"
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#define XX_IS_WIN_D   1
#define XX_IS_LINUX_D 0
#define XX_IS_MACOS_D 0
#elif defined(__APPLE__)
#define XX_IS_WIN_D   0
#define XX_IS_LINUX_D 0
#define XX_IS_MACOS_D 1
#else
#define XX_IS_WIN_D   0
#define XX_IS_LINUX_D 1
#define XX_IS_MACOS_D 0
#endif

namespace agentxx_text_selection_monitor_plugin {

// =====================================================================
// 多实例约定 (2026-08 API v1): 同一动态库可被同一进程内不同宿主各自
// 加载为独立实例, 本插件不持有任何可变全局; 功能状态一律存于每实例
// PluginCtx (定义于主 cpp, create 经 *plugin_ctx 交付宿主, 回调经
// user_data 恢复); 日志经显式传入的 host/logIf 路由到宿主接口表。
// =====================================================================

/// 日志转发到宿主 agentxx.agent.log 接口表 (host/logIf 取自本实例上下文;
/// 任一为空时静默丢弃)
inline void pluginLog(
    const AgentxxHost*     host,
    const AgentxxLogIface* logIf,
    int                    level,
    const std::string&     msg
) {
    if (host && logIf && logIf->log) {
        logIf->log(host, level, agentxx_plugin_sv(msg.data(), msg.size()));
    }
}

// =====================================================================
// XX_LOG* 宏路由 Sink: 实现文件 (text_selection_monitor.cpp 等) 内的
// XX_LOG* 宏经此转发到宿主接口表。每实例 Sink 闭包存于入口 cpp 的
// PluginCtx::log_sink, create 时装配并发布到 g_log_sink。
// 多实例说明: 同库多实例共享 .data 段, g_log_sink 为进程级单点 —— 后装配
// 实例生效 (与 agentxx_codegraph 的 g_mgr_log_sink 同策略); 日志仅为观测
// 旁路, 不影响功能正确性。未装配/实例已销毁时静默丢弃。
// =====================================================================
using PluginLogSink = std::function<void(int level, const std::string& msg)>;

/// 当前生效的实例日志 Sink (指向某实例 PluginCtx 内的闭包存储)
inline std::atomic<const PluginLogSink*> g_log_sink{nullptr};

/// XX_LOG* 宏路由用重载 (level, msg): 经当前实例 Sink 转发到宿主接口表
inline void pluginLog(int level, const std::string& msg) {
    if (const auto* sink = g_log_sink.load(std::memory_order_acquire)) {
        (*sink)(level, msg);
    }
}

/// 宿主分配字符串 (跨边界内存; host 取自回调恢复的实例上下文)
inline char* pluginStrdup(const AgentxxHost* host, const char* s) {
    if (!host || !s) {
        return nullptr;
    }
    return host->vtable->strdup(s);
}

// =====================================================================
// 轻量 JSON 读取 (基于 simdjson ondemand; 只读不递归)
// =====================================================================

class SimpleJson {
public:

    explicit SimpleJson(const std::string& s) {
        padded_ = std::make_unique<simdjson::padded_string>(s);
        auto r  = parser_.iterate(*padded_);
        if (r.error()) {
            ok_ = false;
            return;
        }
        doc_ = std::move(r).value();
        ok_  = true;
    }

    bool ok() const {
        return ok_;
    }

    simdjson::ondemand::document& doc() {
        return doc_;
    }

private:

    simdjson::ondemand::parser               parser_;
    std::unique_ptr<simdjson::padded_string> padded_;
    simdjson::ondemand::document             doc_;
    bool                                     ok_ = false;
};

inline bool
    jsonGetString(simdjson::simdjson_result<simdjson::ondemand::value> v, std::string& out) {
    if (v.error()) {
        return false;
    }
    std::string_view sv;
    if (v.value().get_string().get(sv)) {
        return false;
    }
    out = std::string(sv);
    return true;
}

inline bool jsonGetInt(simdjson::simdjson_result<simdjson::ondemand::value> v, int64_t& out) {
    if (v.error()) {
        return false;
    }
    return !v.value().get_int64().get(out);
}

} // namespace agentxx_text_selection_monitor_plugin

#define XX_LOGT(...) ::agentxx_text_selection_monitor_plugin::pluginLog(0, fmt::format(__VA_ARGS__))
#define XX_LOGD(...) ::agentxx_text_selection_monitor_plugin::pluginLog(1, fmt::format(__VA_ARGS__))
#define XX_LOGI(...) ::agentxx_text_selection_monitor_plugin::pluginLog(2, fmt::format(__VA_ARGS__))
#define XX_LOGW(...) ::agentxx_text_selection_monitor_plugin::pluginLog(3, fmt::format(__VA_ARGS__))
#define XX_LOGE(...) ::agentxx_text_selection_monitor_plugin::pluginLog(4, fmt::format(__VA_ARGS__))
