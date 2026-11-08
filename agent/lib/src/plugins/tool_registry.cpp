#include "agentxx/plugin/tool_registry.h"

#include "agentxx/util/neograph_json_bridge.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/log.h"
#include <algorithm>

namespace agentxx {
namespace plugin {

bool ToolRegistry::registerTool(
    std::string                                 name,
    std::shared_ptr<agentxx::tools::XXToolBase> tool
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
    // 同名旧缓存 (仅在先摘除后重注册时有值) 必须清掉: 定义可能已变
    dynamicSchemaCache_.erase(name);
    utilxx_base::insertHeterogeneous(tools_, std::move(name), std::move(tool));
    return true;
}

std::shared_ptr<agentxx::tools::XXToolBase> ToolRegistry::unregisterTool(std::string_view name) {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return nullptr;
    }
    auto tool = std::move(it->second);
    tools_.erase(it);
    dynamicSchemaCache_.erase(std::string{name});
    return tool;
}

std::shared_ptr<agentxx::tools::XXToolBase> ToolRegistry::find(std::string_view name) const {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return nullptr;
    }
    return it->second;
}

bool ToolRegistry::contains(std::string_view name) const {
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

void ToolRegistry::setStaticToolSchemas(
    std::vector<std::pair<std::string, utilxx_base::Json>> schemas
) {
    staticToolSchemas_.clear();
    for (auto& [name, parameters] : schemas) {
        if (name.empty()) {
            continue;
        }
        utilxx_base::insertHeterogeneous(
            staticToolSchemas_,
            std::move(name),
            std::move(parameters)
        );
    }
}

bool ToolRegistry::parametersOf(std::string_view name, utilxx_base::Json& out) const {
    if (name.empty()) {
        return false;
    }
    if (auto it = staticToolSchemas_.find(name); it != staticToolSchemas_.end()) {
        out = it->second;
        return true;
    }
    auto tool = find(name);
    if (!tool) {
        return false;
    }
    if (auto it = dynamicSchemaCache_.find(name); it != dynamicSchemaCache_.end()) {
        out = it->second;
        return true;
    }
    // 插件工具: 定义里的 parameters 是图边界类型 (neograph::json), 转成业务 Json 后缓存
    auto params = agentxx::util::fromNeographJson(tool->get_definition().parameters);
    if (!params.is_object()) {
        params = utilxx_base::Json::object();
    }
    out = params;
    utilxx_base::insertHeterogeneous(dynamicSchemaCache_, std::string{name}, std::move(params));
    return true;
}

} // namespace plugin
} // namespace agentxx
