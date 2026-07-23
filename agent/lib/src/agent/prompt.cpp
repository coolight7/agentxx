#include "agentxx/agent/prompt.h"

#include <cassert>

namespace agentxx {
namespace agent {

const std::string& ToolPrompt::getArg(std::string_view name) const {
    const auto it = args.find(name);
    assert(it != args.end());
    return it->second;
}

neograph::json AgentPrompt::toJson() const {
    neograph::json j;
    j["systemPrompt"]         = systemPrompt;
    j["systemPlanningPrompt"] = systemPlanningPrompt;
    j["systemSkillPrompt"]    = systemSkillPrompt;
    {
        neograph::json tools = neograph::json::object();
        for (const auto& kv : toolPrompt) {
            neograph::json tp;
            tp["depict"]        = kv.second.depict;
            neograph::json args = neograph::json::object();
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

void AgentPrompt::fromJson(const neograph::json& j) {
    mergeFromJson(j);
}

void AgentPrompt::mergeFromJson(const neograph::json& j) {
    if (j.contains("systemPrompt") && j["systemPrompt"].is_string()) {
        systemPrompt = j["systemPrompt"].get<std::string>();
    }
    if (j.contains("systemPlanningPrompt") && j["systemPlanningPrompt"].is_string()) {
        systemPlanningPrompt = j["systemPlanningPrompt"].get<std::string>();
    }
    if (j.contains("systemSkillPrompt") && j["systemSkillPrompt"].is_string()) {
        systemSkillPrompt = j["systemSkillPrompt"].get<std::string>();
    }
    if (j.contains("toolPrompt") && j["toolPrompt"].is_object()) {
        auto tools = j["toolPrompt"];
        for (const auto& item : tools.items()) {
            const auto& name   = item.first;
            const auto& tp     = item.second;
            auto&       target = toolPrompt[name]; // 不存在则默认构造插入
            if (tp.contains("depict") && tp["depict"].is_string()) {
                target.depict = tp["depict"].get<std::string>();
            }
            if (tp.contains("args") && tp["args"].is_object()) {
                auto args = tp["args"];
                for (const auto& a : args.items()) {
                    if (a.second.is_string()) {
                        target.args[a.first] = a.second.get<std::string>();
                    }
                }
            }
        }
    }
}

size_t AgentPrompt::promptHash() const {
    size_t h  = std::hash<std::string>{}(systemPrompt);
    h        ^= std::hash<std::string>{}(systemPlanningPrompt) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h        ^= std::hash<std::string>{}(systemSkillPrompt) + 0x9e3779b9 + (h << 6) + (h >> 2);
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
