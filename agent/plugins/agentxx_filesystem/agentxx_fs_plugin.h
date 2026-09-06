/// agentxx_filesystem 插件 —— 共享头
#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include <fmt/format.h>
#include <memory>
#include <neograph/json.h>
#include <string>
#include <vector>

namespace agentxx_fs_plugin {

struct PluginCtx : public agentxx::plugin::PluginBase {};

inline void pluginLog(const PluginCtx* ctx, int level, const std::string& msg) {
    if (ctx) {
        ctx->log.log(level, msg);
    }
}

/// 提取"字符串列表"参数 (client 渲染摘要用):
/// - 值为数组时逐项提取其中的字符串元素 (跳过非字符串元素)
/// - 值不是数组时 (如 LLM 下发的单字符串) 直接按单个字符串渲染
///   (如 file_patterns 写成 "agent/test/*.cpp")
/// - 缺失/其他类型: 返回空列表
inline std::vector<std::string> stringListArg(const neograph::json& args, std::string_view key) {
    std::vector<std::string> out;
    const std::string        k{key};
    if (!args.is_object() || !args.contains(k)) {
        return out;
    }
    const auto& v = args[k];
    if (v.is_string()) {
        out.push_back(v.get<std::string>());
    } else if (v.is_array()) {
        for (const auto& item : v) {
            if (item.is_string()) {
                out.push_back(item.get<std::string>());
            }
        }
    }
    return out;
}

inline char* pluginStrdup(const AgentxxPluginHost* host, const char* s) {
    if (!host || !s) {
        return nullptr;
    }
    auto sv = agentxx::plugin::PluginStringView::fromCstr(s);
    return agentxx::plugin::PluginString::strdup(host, &sv);
}

inline auto ctxGuardLogger(PluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            ctx->log.error(msg ? msg : "");
        }
    };
}

} // namespace agentxx_fs_plugin
