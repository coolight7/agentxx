#include "agentxx/agent/prompt.h"
#include "agentxx/util/container_util.h"
#include "agentxx/util/log.h"
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

void AgentPrompt::refreshEnvDetectedPrompts() {
    // 触发 PowerShell 探测 (首次调用阻塞, 结果按进程缓存):
    // - 必须在 agent 线程 (BaseAgent::init) 执行, 避免阻塞 UI/主线程启动
    // - 探测结果同时供 execute_command tool 运行时使用 (见 buildWinProcLaunch),
    //   与提示词文本相互独立, 刷新前后执行均正确
    (void)agentxx::util::detectPowerShell();
    const auto& ps  = cachedPowerShellInfo();
    auto&       win = util::getOrCreateHeterogeneous(toolPrompt, "agentxx_execute_windows_command");
    win.depict      = winCommandToolDepict();
    util::insertOrAssignHeterogeneous(
        win.args,
        "command_process",
        ps.available ? winCommandProcessPwsh() : winCommandProcessCmd()
    );
    util::insertOrAssignHeterogeneous(
        win.args,
        "command_popen",
        ps.available ? winCommandPopenPwsh() : winCommandPopenCmd()
    );
}

neograph::json AgentPrompt::toJson() const {
    neograph::json j;
    j["systemPrompt"] = systemPrompt;
    {
        neograph::json append = neograph::json::object();
        for (const auto& kv : appendSystemPrompts) {
            append[kv.first] = kv.second;
        }
        j["appendSystemPrompts"] = std::move(append);
    }
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
    if (j.contains("appendSystemPrompts") && j["appendSystemPrompts"].is_object()) {
        auto append = j["appendSystemPrompts"];
        for (const auto& item : append.items()) {
            const auto& key = item.first;
            const auto& val = item.second;
            if (val.is_string()) {
                util::insertOrAssignHeterogeneous(appendSystemPrompts, key, val.get<std::string>());
            } else if (val.is_null()) {
                appendSystemPrompts.erase(key);
            }
        }
    }
    if (j.contains("toolPrompt") && j["toolPrompt"].is_object()) {
        auto tools = j["toolPrompt"];
        for (const auto& item : tools.items()) {
            const auto& name = item.first;
            const auto& tp   = item.second;
            auto& target = util::getOrCreateHeterogeneous(toolPrompt, name); // 不存在则默认构造插入
            if (tp.contains("depict") && tp["depict"].is_string()) {
                target.depict = tp["depict"].get<std::string>();
            }
            if (tp.contains("args") && tp["args"].is_object()) {
                auto args = tp["args"];
                for (const auto& a : args.items()) {
                    if (a.second.is_string()) {
                        util::insertOrAssignHeterogeneous(
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
