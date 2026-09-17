/// agentxx_filesystem 插件 —— 共享头
#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "utilxx_base/json.h"
#include <fmt/format.h>
#include <memory>
#include <string>
#include <vector>

namespace agentxx_fs_plugin {

/// 公共助手统一由插件 SDK 提供 (实现见 plugin_kit.h)
using agentxx::plugin::ctxGuardLogger;
using agentxx::plugin::pluginLog;
using agentxx::plugin::pluginStrdup;

struct PluginCtx : public agentxx::plugin::PluginBase {};

/// 提取"字符串列表"参数 (client 渲染摘要用):
/// - 值为数组时逐项提取其中的字符串元素 (跳过非字符串元素)
/// - 值不是数组时 (如 LLM 下发的单字符串) 直接按单个字符串渲染
///   (如 file_patterns 写成 "agent/test/*.cpp")
/// - 缺失/其他类型: 返回空列表
inline std::vector<std::string>
    stringListArg(const utilxx_base::Json& args, std::string_view key) {
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

} // namespace agentxx_fs_plugin
