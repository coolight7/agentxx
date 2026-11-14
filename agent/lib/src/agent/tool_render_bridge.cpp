/// 工具特化渲染 agent 侧实现 (详见头文件说明)
///
/// 这里只做三件事: 读登记表快照、取出一次调用所需的凭据、执行回调并把输出拷成
/// 主程序字符串。所有跨边界输出都由主程序释放 (与客户端侧插件调用同一规矩)。
#include "agentxx/agent/tool_render_bridge.h"
#include "agentxx/agent/context.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_framework.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/plugin_mount.h"
#include "utilxx_base/log.h"
#include <cstdint>
#include <exception>
#include <utility>

namespace agentxx {
namespace agent {
namespace {

/// 取插件输出字符串并释放主程序堆内存 (无论成功/失败/异常路径都要释放)
std::string takeHostString(PluginxxString& value) {
    std::string out;
    if (value.data != nullptr && value.size > 0) {
        out.assign(value.data, static_cast<size_t>(value.size));
    }
    agentxx::plugin::hostMemoryFree(value.data);
    value.data = nullptr;
    value.size = 0;
    return out;
}

/// 折叠内容归一到"一行": 插件可能返回多行文本, 而折叠行只占一行
std::string toSingleLine(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool lastSpace = false;
    for (const char c : text) {
        const bool isSpace = (c == '\n' || c == '\r' || c == '\t');
        if (isSpace) {
            if (!out.empty() && !lastSpace) {
                out.push_back(' ');
            }
            lastSpace = true;
            continue;
        }
        out.push_back(c);
        lastSpace = false;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

} // namespace

std::vector<WireToolRenderItem> collectToolRenderItems(const AgentContext& ctx) {
    std::vector<WireToolRenderItem> items;
    if (!ctx.pluginManager) {
        return items;
    }
    auto mount = ctx.pluginManager->findMountById(ctx.agentId);
    if (!mount) {
        return items;
    }
    items.reserve(mount->toolRenderers().size());
    for (const auto& [toolName, entry] : mount->toolRenderers()) {
        WireToolRenderItem item;
        item.toolName    = toolName;
        item.displayName = entry.displayName;
        item.collapsed   = entry.collapsed != nullptr;
        item.expanded    = entry.expanded != nullptr;
        item.owner       = entry.owner;
        items.push_back(std::move(item));
    }
    return items;
}

std::optional<ToolRenderCallable>
    findToolRenderCallable(const AgentContext& ctx, std::string_view toolName, bool collapsed) {
    if (toolName.empty() || !ctx.pluginManager) {
        return std::nullopt;
    }
    auto mount = ctx.pluginManager->findMountById(ctx.agentId);
    if (!mount) {
        return std::nullopt;
    }
    const auto* entry = mount->findToolRenderer(toolName);
    if (entry == nullptr) {
        return std::nullopt;
    }
    ToolRenderCallable call;
    call.fn        = collapsed ? entry->collapsed : entry->expanded;
    call.userData  = entry->userData;
    call.collapsed = collapsed;
    call.toolName  = entry->toolName;
    if (call.fn == nullptr) {
        return std::nullopt; // 只改了显示名: 内容走默认渲染
    }
    call.instance = entry->inst.lock();
    return call;
}

WireToolRenderResult
    invokeToolRender(const ToolRenderCallable& call, const WireToolRenderRequest& req) {
    WireToolRenderResult res;
    res.requestId = req.requestId;
    if (call.fn == nullptr) {
        return res;
    }
    if (req.argsJson.size() > kToolRenderValueMaxBytes
        || req.resultText.size() > kToolRenderValueMaxBytes) {
        XX_LOGW(
            "[tool_render] request {} for '{}' is too large (args={} bytes, result={} bytes), "
            "falling back to the default rendering",
            req.requestId,
            req.toolName,
            req.argsJson.size(),
            req.resultText.size()
        );
        return res;
    }

    // 执行租约: 持有实例强引用, 同时登记一次"正在执行插件代码"。卸载路径会等它结束,
    // 因此回调期间插件不会被 destroy/dlclose (实例已进入关闭流程时获取租约失败)。
    pluginxx::PluginInstanceBase::InflightGuard guard{
        std::static_pointer_cast<pluginxx::PluginInstanceBase>(call.instance)
    };
    if (call.instance && !static_cast<bool>(guard)) {
        XX_LOGI(
            "[tool_render] renderer for '{}' skipped: plugin instance is closing",
            call.toolName
        );
        return res;
    }

    AgentxxPluginToolRenderInput input{};
    input.struct_size  = sizeof(AgentxxPluginToolRenderInput);
    input.tool_call_id = agentxx::plugin::PluginStringView::from(req.toolCallId);
    input.tool_name    = agentxx::plugin::PluginStringView::from(req.toolName);
    input.args_json    = agentxx::plugin::PluginStringView::from(req.argsJson);
    input.result_text  = agentxx::plugin::PluginStringView::from(req.resultText);
    input.is_finished  = req.isFinished ? 1 : 0;
    input.is_error     = req.isError ? 1 : 0;
    input.max_width    = req.maxWidth;

    AgentxxPluginToolRenderOutput output{};
    int32_t                       rc = AGENTXX_RESULT_ERR_INTERNAL;
    try {
        rc = call.fn(call.userData, &input, &output);
    } catch (const std::exception& e) {
        rc = AGENTXX_RESULT_ERR_INTERNAL;
        XX_LOGW("[tool_render] renderer for '{}' threw: {}", call.toolName, e.what());
    } catch (...) {
        rc = AGENTXX_RESULT_ERR_INTERNAL;
        XX_LOGW("[tool_render] renderer for '{}' threw an unknown exception", call.toolName);
    }

    if (call.collapsed) {
        const std::string text = toSingleLine(takeHostString(output.text));
        if (rc != AGENTXX_RESULT_OK) {
            res.errorMessage = fmt::format("renderer failed (code {})", rc);
            return res;
        }
        if (text.size() > kToolRenderValueMaxBytes) {
            XX_LOGW(
                "[tool_render] collapsed text for '{}' is too large ({} bytes), dropped",
                call.toolName,
                text.size()
            );
            return res;
        }
        res.text = text;
        res.ok   = !res.text.empty();
        return res;
    }

    const std::string itemsJson = takeHostString(output.items_json);
    if (rc != AGENTXX_RESULT_OK) {
        res.errorMessage = fmt::format("renderer failed (code {})", rc);
        return res;
    }
    if (itemsJson.size() > kToolRenderValueMaxBytes) {
        XX_LOGW(
            "[tool_render] expanded items for '{}' are too large ({} bytes), dropped",
            call.toolName,
            itemsJson.size()
        );
        return res;
    }
    if (!itemsJson.empty()) {
        // 只接受非空数组: 其它形态在客户端解析不出来, 先在这里挡掉并留下日志
        bool valid = false;
        try {
            const auto parsed = utilxx_base::Json::parse(itemsJson);
            valid             = parsed.is_array() && !parsed.empty();
        } catch (const std::exception& e) {
            XX_LOGW(
                "[tool_render] expanded items for '{}' are not valid JSON: {}",
                call.toolName,
                e.what()
            );
        }
        if (!valid) {
            return res;
        }
        res.itemsJson = itemsJson;
        res.ok        = true;
    }
    return res;
}

} // namespace agent
} // namespace agentxx
