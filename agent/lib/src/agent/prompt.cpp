#include "agentxx/agent/prompt.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/log.h"
#include <cassert>

namespace agentxx {
namespace agent {

const std::string& ToolPrompt::getArg(std::string_view name) const {
    const auto it = args.find(name);
    if (it == args.end()) {
        XX_LOGE("ToolPrompt::getArg 必须传入存在的 name: {}", name);
        assert(false);
        static const std::string empty;
        return empty;
    }
    return it->second;
}

utilxx_base::Json AgentPrompt::toJson() const {
    utilxx_base::Json j;
    j["systemPrompt"] = systemPrompt;
    {
        utilxx_base::Json append = utilxx_base::Json::object();
        for (const auto& kv : appendSystemPrompts) {
            append[kv.first] = kv.second;
        }
        j["appendSystemPrompts"] = std::move(append);
    }
    {
        utilxx_base::Json tools = utilxx_base::Json::object();
        for (const auto& kv : toolPrompt) {
            utilxx_base::Json tp;
            tp["depict"]           = kv.second.depict;
            utilxx_base::Json args = utilxx_base::Json::object();
            for (const auto& a : kv.second.args) {
                args[a.first] = a.second;
            }
            tp["args"]      = args;
            tools[kv.first] = tp;
        }
        j["toolPrompt"] = tools;
    }
    return j;
}

void AgentPrompt::fromJson(const utilxx_base::Json& j) {
    mergeFromJson(j);
}

void AgentPrompt::mergeFromJson(const utilxx_base::Json& j) {
    if (j.contains("systemPrompt") && j["systemPrompt"].is_string()) {
        systemPrompt = j["systemPrompt"].get<std::string>();
    }
    if (j.contains("appendSystemPrompts") && j["appendSystemPrompts"].is_object()) {
        auto append = j["appendSystemPrompts"];
        for (const auto& item : append.items()) {
            const auto& key = item.first;
            const auto& val = item.second;
            if (val.is_string()) {
                utilxx_base::insertOrAssignHeterogeneous(
                    appendSystemPrompts,
                    key,
                    val.get<std::string>()
                );
            } else if (val.is_null()) {
                // 异构删除复用 utilxx_base::eraseHeterogeneous (libc++ 无 C++23 异构 erase)
                utilxx_base::eraseHeterogeneous(appendSystemPrompts, key);
            }
        }
    }
    if (j.contains("toolPrompt") && j["toolPrompt"].is_object()) {
        auto tools = j["toolPrompt"];
        for (const auto& item : tools.items()) {
            const auto& name = item.first;
            const auto& tp   = item.second;
            auto&       target
                = utilxx_base::getOrCreateHeterogeneous(toolPrompt, name); // 不存在则默认构造插入
            if (tp.contains("depict") && tp["depict"].is_string()) {
                target.depict = tp["depict"].get<std::string>();
            }
            if (tp.contains("args") && tp["args"].is_object()) {
                auto args = tp["args"];
                for (const auto& a : args.items()) {
                    if (a.second.is_string()) {
                        utilxx_base::insertOrAssignHeterogeneous(
                            target.args,
                            a.first,
                            a.second.get<std::string>()
                        );
                    }
                }
            }
        }
    }
}

size_t AgentPrompt::promptHash() const {
    size_t h = std::hash<std::string>{}(systemPrompt);
    for (const auto& kv : appendSystemPrompts) {
        h ^= std::hash<std::string>{}(kv.first) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(kv.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    for (const auto& kv : toolPrompt) {
        h ^= std::hash<std::string>{}(kv.first) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(kv.second.depict) + 0x9e3779b9 + (h << 6) + (h >> 2);
        for (const auto& a : kv.second.args) {
            h ^= std::hash<std::string>{}(a.first) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<std::string>{}(a.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
    }
    return h;
}

} // namespace agent
} // namespace agentxx
