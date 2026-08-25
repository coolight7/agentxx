// agentxx_codegraph 插件 —— 共享头
// - 插件不链接 libagentxx: 日志经宿主接口表 log 转发 (替代 XX_LOG)
//
// 多实例约定 (2026-08 API v1): 同一动态库可被同一进程内不同 agent 宿主
// 各自加载为独立实例, 本插件不持有任何可变全局; 功能状态一律存于每实例
// PluginCtx (create 创建经 *plugin_ctx 交付宿主, 回调经 spec.user_data /
// 订阅 ud 恢复); 日志经显式传入的 ctx 路由到宿主接口表。
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_tool_sync.h"
#include "codegraph/core/json.hpp"
#include "fmt/format.h"
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace agentxx_codegraph_plugin {

/// CodeGraphManager 前置声明 (完整定义见 codegraph_manager.h; 每实例
/// PluginCtx 以 shared_ptr 持有 —— 多实例契约下各实例独立索引)
class CodeGraphManager;

/// 实例日志转发到宿主 agentxx.agent.log 接口表 (host/logIf 取自本实例
/// 上下文; 任一为空时静默丢弃)。多实例契约: 不读任何进程级全局。
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

} // namespace agentxx_codegraph_plugin

// 注意: XX_LOG* 宏已移除 (多实例契约要求日志携带实例上下文); 本插件的
// 日志调用点统一改为 pluginLog(ctx, level, fmt::format(...)) 直调形式
