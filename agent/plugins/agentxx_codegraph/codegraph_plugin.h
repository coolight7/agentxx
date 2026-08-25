// agentxx_codegraph 插件 —— 共享头
// - 插件不链接 libagentxx: 日志经宿主接口表 log 转发 (替代 XX_LOG)
// - g_pluginHost/g_ifaces 由入口 (agentxx_codegraph.cpp) 在 entry 时装配
//   (COM 风格接口表一次性查询缓存, 见 plugin_iface_helper.h)
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_tool_sync.h"
#include "codegraph/core/json.hpp"
#include "fmt/format.h"
#include <cstring>
#include <string>

namespace agentxx_codegraph_plugin {

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

/// 宿主分配字符串 (跨边界内存)
inline char* pluginStrdup(const char* s) {
    if (!s) {
        return nullptr;
    }
    return g_host->vtable->strdup(s);
}

} // namespace agentxx_codegraph_plugin

#define XX_LOGT(...) ::agentxx_codegraph_plugin::pluginLog(0, fmt::format(__VA_ARGS__))
#define XX_LOGD(...) ::agentxx_codegraph_plugin::pluginLog(1, fmt::format(__VA_ARGS__))
#define XX_LOGI(...) ::agentxx_codegraph_plugin::pluginLog(2, fmt::format(__VA_ARGS__))
#define XX_LOGW(...) ::agentxx_codegraph_plugin::pluginLog(3, fmt::format(__VA_ARGS__))
#define XX_LOGE(...) ::agentxx_codegraph_plugin::pluginLog(4, fmt::format(__VA_ARGS__))
