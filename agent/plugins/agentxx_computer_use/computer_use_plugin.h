// agentxx_computer_use 插件 —— 共享头
// - 插件不链接 libagentxx: 日志经宿主 agentxx.agent.log 接口表转发 (替代 XX_LOG)
// - 平台宏: XX_IS_WIN_D / XX_IS_LINUX_D / XX_IS_MACOS_D 本地推导
//   (libagentxx 由 util.h 提供, 插件独立编译需自备)
// - g_pluginHost 由入口 (agentxx_computer_use.cpp) 在 entry 时装配
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "codegraph/core/json.hpp"
#include "fmt/format.h"
#include <cstring>
#include <string>

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

namespace agentxx_computer_use_plugin {

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
// 轻量 JSON 读取 (基于 simdjson ondemand; 只读不递归)
// - codegraph::Json::parse 的 from_simdjson_value 在 ondemand 惰性迭代下
//   解析嵌套对象会错乱 (第三方库缺陷), 插件侧改用本读取器
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

/// 从值 (顶层字段或嵌套对象字段) 提取字符串
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

inline bool jsonGetBool(simdjson::simdjson_result<simdjson::ondemand::value> v, bool& out) {
    if (v.error()) {
        return false;
    }
    return !v.value().get_bool().get(out);
}

inline bool jsonGetInt(simdjson::simdjson_result<simdjson::ondemand::value> v, int64_t& out) {
    if (v.error()) {
        return false;
    }
    return !v.value().get_int64().get(out);
}

inline bool jsonGetDouble(simdjson::simdjson_result<simdjson::ondemand::value> v, double& out) {
    if (v.error()) {
        return false;
    }
    return !v.value().get_double().get(out);
}

/// 从值提取字符串数组
inline bool jsonGetStringArray(
    simdjson::simdjson_result<simdjson::ondemand::value> v,
    std::vector<std::string>&                            out
) {
    if (v.error()) {
        return false;
    }
    simdjson::ondemand::array arr;
    if (v.value().get_array().get(arr)) {
        return false;
    }
    for (auto elem : arr) {
        std::string_view sv;
        if (elem.get_string(sv)) {
            return false;
        }
        out.emplace_back(sv);
    }
    return true;
}

/// 宿主分配字符串 (跨边界内存; host 取自回调恢复的实例上下文)
inline char* pluginStrdup(const AgentxxHost* host, const char* s) {
    if (!host || !s) {
        return nullptr;
    }
    return host->vtable->strdup(s);
}
} // namespace agentxx_computer_use_plugin

#define XX_LOGT(...) ::agentxx_computer_use_plugin::pluginLog(0, fmt::format(__VA_ARGS__))
#define XX_LOGD(...) ::agentxx_computer_use_plugin::pluginLog(1, fmt::format(__VA_ARGS__))
#define XX_LOGI(...) ::agentxx_computer_use_plugin::pluginLog(2, fmt::format(__VA_ARGS__))
#define XX_LOGW(...) ::agentxx_computer_use_plugin::pluginLog(3, fmt::format(__VA_ARGS__))
#define XX_LOGE(...) ::agentxx_computer_use_plugin::pluginLog(4, fmt::format(__VA_ARGS__))
