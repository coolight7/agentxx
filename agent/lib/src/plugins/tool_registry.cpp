#include "agentxx/plugin/tool_registry.h"

#include "agentxx/util/log.h"
#include <algorithm>

namespace agentxx {
namespace plugin {

bool ToolRegistry::registerTool(
    std::string                                          name,
    std::shared_ptr<agentxx::tools::XXToolBase>          tool
) {
    if (name.empty() || !tool) {
        XX_LOGW("ToolRegistry: register with empty name or null tool");
        return false;
    }
    // 名称冲突: 表内 或 静态工具
    if (tools_.contains(name)) {
        XX_LOGW("ToolRegistry: tool `{}` already registered", name);
        return false;
    }
    if (std::find(staticToolNames_.begin(), staticToolNames_.end(), name)
        != staticToolNames_.end()) {
        XX_LOGW("ToolRegistry: tool `{}` conflicts with built-in tool", name);
        return false;
    }
    tools_[std::move(name)] = std::move(tool);
    return true;
}

std::shared_ptr<agentxx::tools::XXToolBase> ToolRegistry::unregisterTool(const std::string& name) {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return nullptr;
    }
    auto tool = std::move(it->second);
    tools_.erase(it);
    return tool;
}

std::shared_ptr<agentxx::tools::XXToolBase> ToolRegistry::find(const std::string& name) const {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return nullptr;
    }
    return it->second;
}

bool ToolRegistry::contains(const std::string& name) const {
    return tools_.contains(name);
}

void ToolRegistry::appendDefinitions(std::vector<neograph::ChatTool>& defs) const {
    defs.reserve(defs.size() + tools_.size());
    for (const auto& [name, tool] : tools_) {
        (void)name;
        defs.push_back(tool->get_definition());
    }
}

size_t ToolRegistry::size() const {
    return tools_.size();
}

std::vector<std::string> ToolRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(tools_.size());
    for (const auto& [name, tool] : tools_) {
        (void)tool;
        out.push_back(name);
    }
    return out;
}

void ToolRegistry::setStaticToolNames(std::vector<std::string> names) {
    staticToolNames_ = std::move(names);
}

} // namespace plugin
} // namespace agentxx
