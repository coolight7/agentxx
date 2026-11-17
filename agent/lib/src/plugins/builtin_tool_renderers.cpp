/// builtin_tool_renderers.cpp —— lib 内置工具的工具特化渲染器
///
/// 背景: 工具特化渲染 (见 docs/zh-cn/design/plugins.md "工具特化渲染架构") 通常由
/// 提供工具的插件注册; lib 内置工具 (agentxx_share_store / agentxx_subagent)
/// 没有对应插件, 因此由主程序自身注册内置渲染器
/// (见 ClientPluginManager::registerBuiltinToolRenderer)。
///
/// 渲染范围: 显示名注册时给定, 这里只产出折叠态的一行内容 (与插件渲染器的
/// 折叠内容回调同一形态)。不提供展开内容 —— 展开体保持主程序默认展示
/// (参数 JSON + 结果文本), 特化渲染不隐藏工具的原始信息。
///
/// 折叠内容**不带**分隔符 `" · "`: 头行格式由界面统一拼装。
///
/// 参数形态: 工具展示行的参数文本在写入展示历史前已按工具 schema 修正过
/// (见 agentxx::util::autoFixedToolArgsJson, 与执行前同一份实现), 所以这里按
/// schema 声明的类型直接读, 不需要再做 string/array、单对象/对象数组一类的兼容判断。
#include "agentxx/plugin/builtin_tool_renderers.h"

#include "agentxx/plugin/client_plugin_manager.h"
#include "agentxx/plugin/plugin_framework.h"
#include "agentxx/plugin/plugin_interfaces.h"

#include "fmt/format.h"
#include "utilxx_base/json.h"
#include "utilxx_base/string_util.h"
#include <optional>
#include <string>
#include <string_view>

namespace agentxx {
namespace plugin {

namespace {

/// 工具名 (与 lib 内置工具实现一致: agentxx::tools::SessionShareStoreTool /
/// agentxx::tools::SubAgentManagerTool)
constexpr std::string_view kToolShareStore = "agentxx_share_store";
constexpr std::string_view kToolSubagent   = "agentxx_subagent";

/// 显示名 (与插件渲染器规则一致: 简短英文名, 如 Read/Edit/Bash)
constexpr std::string_view kNameShareStore = "Store";
constexpr std::string_view kNameSubagent   = "Subagent";

/// 折叠内容预览的可见字符数上限 (宽度预算不足时取 80)
size_t summaryPreviewCols(int maxWidth) {
    return (maxWidth > 20) ? static_cast<size_t>(maxWidth - 15) : size_t{80};
}

/// 取文本首行并按可见字符数截断 (超长补 "..."; 空文本返回空串)
std::string oneLinePreview(std::string_view text, size_t maxCols) {
    if (text.empty() || maxCols == 0) {
        return {};
    }
    const auto  nl = text.find('\n');
    std::string line{(nl == std::string_view::npos) ? text : text.substr(0, nl)};
    if (!line.empty() && line.back() == '\r') {
        line.pop_back(); // CRLF 文本去掉行尾 '\r'
    }
    const auto idx = utilxx_base::findIndexByUtf8Length(line, maxCols);
    if (idx > 0 && idx < line.size()) {
        line.resize(idx);
        line += "...";
    }
    return line;
}

/// 解析工具参数 JSON
/// - 空串/非法 JSON/非对象返回 nullopt (如参数仍在流式输出中):
///   调用方退化为空内容 (界面走默认渲染)
std::optional<utilxx_base::Json> parseToolArgs(std::string_view argsJson) {
    if (argsJson.empty()) {
        return std::nullopt;
    }
    try {
        auto j = utilxx_base::Json::parse(argsJson);
        if (!j.is_object()) {
            return std::nullopt;
        }
        return j;
    } catch (...) {
        return std::nullopt;
    }
}

/// 行区间文本 (与工具参数语义一致: <=0 视为未指定, 仅指定一侧时另一侧开放)
/// - 例: offset=0/limit=100 → "[0, 100]"; 仅 offset=10 → "[10, ~]"
/// - 两侧都未指定返回空串
std::string lineRangeText(int64_t offset, int64_t limit) {
    if (offset <= 0 && limit <= 0) {
        return {};
    }
    if (offset <= 0) {
        return fmt::format("[0, {}]", limit);
    }
    if (limit <= 0) {
        return fmt::format("[{}, ~]", offset);
    }
    return fmt::format("[{}, {}]", offset, offset + limit);
}

/// 解析 `agentxx_share_store` new 结果中的新内容 id (结果形如 {"id":3})
/// - 非该形态/解析失败返回 0
uint64_t shareStoreResultId(std::string_view result) {
    if (result.empty() || result.front() != '{') {
        return 0;
    }
    try {
        auto          j  = utilxx_base::Json::parse(result);
        const int64_t id = j.is_object() ? j.value<int64_t>("id", 0) : 0;
        return (id > 0) ? static_cast<uint64_t>(id) : uint64_t{0};
    } catch (...) {
        return 0;
    }
}

/// `agentxx_subagent` 任务文本: 优先取 `message`, 其次取 `messages` 最后一项的
/// `content` (两者都是工具接受的委派输入)
std::string subagentTaskText(const utilxx_base::Json& args) {
    const std::string message = args.value("message", std::string{});
    if (!message.empty()) {
        return message;
    }
    if (!args.contains("messages")) {
        return {};
    }
    const auto& messages = args["messages"];
    if (!messages.is_array() || messages.empty()) {
        return {};
    }
    return messages[messages.size() - 1].value("content", std::string{});
}

/// `agentxx_share_store` 折叠内容, 形态 "[start, end] opt #id"
/// - get:    "[0, 100] get #12" (仅给 offset 时 "[40, ~] get #12";
///           未给分页参数时 "get #12")
/// - new:    "new" (运行中); 完成后补结果里的新 id → "new #7"
/// - update: "update #3"
/// - 区间只由 get 展示 (offset/limit 只对 get 生效; new/update 整段原样存储, 不带区间)
/// - 未识别的 opt (协议扩展) 原样展示; 参数未就绪时返回空串
std::string renderShareStoreCollapsed(const AgentxxToolRenderInput& in) {
    auto args = parseToolArgs(PluginStringView::str(in.args_json));
    if (!args) {
        return {};
    }
    const std::string opt    = args->value("opt", std::string{});
    const int64_t     id     = args->value<int64_t>("id", 0);
    const int64_t     offset = args->value<int64_t>("offset", -1);
    const int64_t     limit  = args->value<int64_t>("limit", -1);

    std::string text;
    auto        addPart = [&text](std::string_view part) {
        if (part.empty()) {
            return;
        }
        if (!text.empty()) {
            text += ' ';
        }
        text += part;
    };

    if (opt == "new" || opt == "get" || opt == "update") {
        if (opt == "get") {
            addPart(lineRangeText(offset, limit));
        }
        addPart(opt);

        // id: get/update 来自参数; new 的新增 id 只在结果里 (参数无 id), 完成后才有
        uint64_t shownId = (id > 0) ? static_cast<uint64_t>(id) : uint64_t{0};
        if (opt == "new" && in.is_finished != 0) {
            shownId = shareStoreResultId(PluginStringView::str(in.result_text));
        }
        if (shownId > 0) {
            addPart(fmt::format("#{}", shownId));
        }
    } else if (!opt.empty()) {
        addPart(opt);
    }
    return text;
}

/// `agentxx_subagent` 折叠内容
/// - 批量 (tasks 非空): "3 tasks: explorer, coder" (子代理名最多列 3 个)
/// - 单发: "explorer · 修复登录失败的问题" (任务文本取首行并按宽度截断);
///   无子代理名时只有任务文本
std::string renderSubagentCollapsed(const AgentxxToolRenderInput& in) {
    auto args = parseToolArgs(PluginStringView::str(in.args_json));
    if (!args) {
        return {};
    }

    // 批量模式判定与工具执行逻辑一致: `tasks` 数组非空时忽略顶层单任务字段
    if (args->contains("tasks")) {
        const auto& tasks = (*args)["tasks"];
        if (tasks.is_array() && !tasks.empty()) {
            std::string names;
            size_t      named = 0; // 已列出的子代理名数量
            size_t      total = 0; // 任务总数 (与 tasks 元素数一致)
            for (const auto& task : tasks) {
                ++total;
                if (!task.is_object()) {
                    continue;
                }
                const std::string name = task.value("subagent", std::string{});
                if (name.empty() || named >= 3) {
                    continue;
                }
                if (named > 0) {
                    names += ", ";
                }
                names += name;
                ++named;
            }
            if (named >= 3 && total > named) {
                names += ", ...";
            }
            std::string text = fmt::format("{} tasks", total);
            if (!names.empty()) {
                text += ": " + names;
            }
            return text;
        }
    }

    const std::string name = args->value("subagent", std::string{});
    const std::string task
        = oneLinePreview(subagentTaskText(*args), summaryPreviewCols(in.max_width));

    if (!name.empty() && !task.empty()) {
        return name + " · " + task;
    }
    return !name.empty() ? name : task;
}

/// 渲染回调公共外壳: 调用具体折叠内容逻辑 → 经完成回调交回结果
/// - 空内容 = 本次无自定义内容 (界面走默认渲染)
/// - 回调内部立即 `done` 就是同步完成, 与改造前一样当帧拿到内容
template<typename Fn>
int32_t runCollapsedRenderer(
    void*                         userData,
    const AgentxxToolRenderInput* input,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out,
    Fn&&                          fn
) {
    (void)error_out;
    if (!input || !notify || !notify->done) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    (void)userData; // 内置渲染器无实例状态 (多实例契约: 不依赖可变全局)

    const std::string text = fn(*input);
    const PluginxxStringView view{text.data(), static_cast<uint64_t>(text.size())};
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &view);
    return 0;
}

} // namespace

int32_t PLUGINXX_CALL builtinRenderShareStoreCollapsed(
    void*                         userData,
    const AgentxxToolRenderInput* input,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    return runCollapsedRenderer(userData, input, notify, error_out, renderShareStoreCollapsed);
}

int32_t PLUGINXX_CALL builtinRenderSubagentCollapsed(
    void*                         userData,
    const AgentxxToolRenderInput* input,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    return runCollapsedRenderer(userData, input, notify, error_out, renderSubagentCollapsed);
}

void registerBuiltinToolRenderers(ClientPluginManager& mgr) {
    mgr.registerBuiltinToolRenderer(
        kToolShareStore,
        kNameShareStore,
        &builtinRenderShareStoreCollapsed
    );
    mgr.registerBuiltinToolRenderer(kToolSubagent, kNameSubagent, &builtinRenderSubagentCollapsed);
}

} // namespace plugin
} // namespace agentxx
