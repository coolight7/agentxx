/// builtin_tool_renderers.cpp —— lib 内置工具的工具特化渲染器
///
/// 背景: 工具特化渲染 (见 docs/zh-cn/design/plugins.md "工具特化渲染架构") 通常由
/// 提供工具的插件注册; lib 内置工具 (agentxx_share_store / agentxx_subagent)
/// 没有对应插件, 因此由宿主自身注册内置渲染器
/// (见 ClientPluginManager::registerBuiltinToolRenderer)。
///
/// 渲染范围: 折叠头的显示名与一行摘要 (与插件渲染器的 "displayName + summary"
/// 形态一致)。不提供 items —— 展开体保持宿主通用展示 (参数 JSON + 结果文本),
/// 特化渲染不隐藏工具的原始信息。
#include "agentxx/plugin/builtin_tool_renderers.h"

#include "agentxx/plugin/client_plugin_manager.h"
#include "agentxx/plugin/plugin_common.h"
#include "agentxx/plugin/plugin_manager_base.h"
#include "agentxx/util/json.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
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

/// 折叠头显示名 (与插件渲染器口径一致: 简短英文名, 如 Read/Edit/Bash)
constexpr std::string_view kNameShareStore = "Store";
constexpr std::string_view kNameSubagent   = "Subagent";

/// 渲染中间结果 (宿主内部 C++ 形态; 最终经宿主堆字符串写入 C ABI 输出结构)
struct RenderText {
    std::string displayName;
    std::string summary;
};

/// 摘要预览的可见字符数上限 (与宿主预设模版渲染器同口径: 宽度预算不足时取 80)
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
    const auto idx = util::findIndexByUtf8Length(line, maxCols);
    if (idx > 0 && idx < line.size()) {
        line.resize(idx);
        line += "...";
    }
    return line;
}

/// 解析工具参数 JSON
/// - 空串/非法 JSON/非对象返回 nullopt (如参数仍在流式输出中):
///   调用方退化为仅提供显示名, 不做摘要
std::optional<util::Json> parseToolArgs(std::string_view argsJson) {
    if (argsJson.empty()) {
        return std::nullopt;
    }
    try {
        auto j = util::Json::parse(argsJson);
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

/// 文本行数摘要 ("152 lines"; 空文本返回空串)
std::string linesText(std::string_view text) {
    const size_t lines = util::countLines(text);
    return (lines > 0) ? fmt::format("{} lines", lines) : std::string{};
}

/// 解析 `agentxx_share_store` insert 结果中的新内容 id (结果形如 {"id":3})
/// - 非该形态/解析失败返回 0
uint64_t shareStoreResultId(std::string_view result) {
    if (result.empty() || result.front() != '{') {
        return 0;
    }
    try {
        auto          j  = util::Json::parse(result);
        const int64_t id = j.is_object() ? j.value<int64_t>("id", 0) : 0;
        return (id > 0) ? static_cast<uint64_t>(id) : uint64_t{0};
    } catch (...) {
        return 0;
    }
}

/// `agentxx_subagent` 任务文本: 优先取 `message`, 其次取 `messages` 最后一项的
/// `content` (两者都是工具接受的委派输入)
std::string subagentTaskText(const util::Json& args) {
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

/// `agentxx_share_store` 渲染: 显示名 "Store" + 操作摘要
/// - insert: " · insert 152 lines → #7" (id 取自结果 JSON, 完成后才有)
/// - set:    " · set #7 [0, 100] 152 lines"
/// - get:    " · get #7 [0, 100]"
/// - delete: " · delete #7"
/// - 未识别的 opt (协议扩展) 原样展示, 参数未就绪时仅提供显示名
int32_t renderShareStore(const AgentxxToolRenderInput& in, RenderText& out) {
    out.displayName = std::string{kNameShareStore};

    auto args = parseToolArgs(PluginStringView::str(in.args_json));
    if (!args) {
        return 0;
    }
    const std::string opt    = args->value("opt", std::string{});
    const int64_t     id     = args->value<int64_t>("id", 0);
    const int64_t     offset = args->value<int64_t>("line_offset", -1);
    const int64_t     limit  = args->value<int64_t>("line_limit", -1);
    const std::string text   = args->value("text", std::string{});

    std::string summary = " ·";
    auto        addPart = [&summary](std::string_view part) {
        if (!part.empty()) {
            summary += ' ';
            summary += part;
        }
    };

    if (opt == "insert") {
        addPart("insert");
        addPart(lineRangeText(offset, limit));
        addPart(linesText(text));
        // insert 的新增 id 只在结果里 (参数无 id), 完成后补 " → #7"
        if (in.is_finished != 0) {
            const uint64_t newId = shareStoreResultId(PluginStringView::str(in.result_text));
            if (newId > 0) {
                addPart(fmt::format("→ #{}", newId));
            }
        }
    } else if (opt == "get" || opt == "set" || opt == "delete") {
        addPart(opt);
        if (id > 0) {
            addPart(fmt::format("#{}", id));
        }
        if (opt != "delete") {
            addPart(lineRangeText(offset, limit));
        }
        if (opt == "set") {
            addPart(linesText(text));
        }
    } else if (!opt.empty()) {
        addPart(opt);
    }

    out.summary = std::move(summary);
    return 0;
}

/// `agentxx_subagent` 渲染: 显示名 "Subagent" + 委派摘要
/// - 批量 (tasks 非空): " · 3 tasks: explorer, coder" (子代理名最多列 3 个)
/// - 单发: " · explorer · 修复登录失败的问题" (任务文本取首行并按宽度截断);
///   无子代理名时为 " · <任务文本>"
int32_t renderSubagent(const AgentxxToolRenderInput& in, RenderText& out) {
    out.displayName = std::string{kNameSubagent};

    auto args = parseToolArgs(PluginStringView::str(in.args_json));
    if (!args) {
        return 0;
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
            out.summary = fmt::format(" · {} tasks", total);
            if (!names.empty()) {
                out.summary += ": " + names;
            }
            return 0;
        }
    }

    const std::string name = args->value("subagent", std::string{});
    const std::string task
        = oneLinePreview(subagentTaskText(*args), summaryPreviewCols(in.max_width));

    // 摘要组合: 有子代理名时 " · <名字>"; 任务文本用 " · " 与名字分隔,
    // 无名字时直接接在 " ·" 之后 (如 " · summarize the module")
    std::string summary = " ·";
    if (!name.empty() && !task.empty()) {
        summary += " " + name + " · " + task;
    } else if (!name.empty()) {
        summary += " " + name;
    } else if (!task.empty()) {
        summary += " " + task;
    }
    out.summary = std::move(summary);
    return 0;
}

/// 渲染回调公共外壳: 调用具体渲染逻辑 → 经宿主堆字符串写入 C ABI 输出
/// - 输出字符串仅在渲染成功时分配 (空串不分配, 调用方按 nullptr 跳过)
/// - items 不提供 (展开体保持宿主通用展示)
template<typename Fn>
int32_t runRenderer(
    void*                         userData,
    const AgentxxToolRenderInput* input,
    AgentxxToolRenderOutput*      output,
    Fn&&                          fn
) {
    if (!input || !output) {
        return -1;
    }
    (void)userData; // 内置渲染器无实例状态 (多实例契约: 不依赖可变全局)

    RenderText    result;
    const int32_t rc = fn(*input, result);
    if (rc != 0) {
        return rc;
    }
    hostMemorySetString(&output->displayName, result.displayName);
    hostMemorySetString(&output->summary, result.summary);
    return 0;
}

} // namespace

int32_t AGENTXX_PLUGIN_CALL builtinRenderShareStore(
    void*                         userData,
    const AgentxxToolRenderInput* input,
    AgentxxToolRenderOutput*      output
) {
    return runRenderer(userData, input, output, renderShareStore);
}

int32_t AGENTXX_PLUGIN_CALL builtinRenderSubagent(
    void*                         userData,
    const AgentxxToolRenderInput* input,
    AgentxxToolRenderOutput*      output
) {
    return runRenderer(userData, input, output, renderSubagent);
}

void registerBuiltinToolRenderers(ClientPluginManager& mgr) {
    mgr.registerBuiltinToolRenderer(kToolShareStore, &builtinRenderShareStore);
    mgr.registerBuiltinToolRenderer(kToolSubagent, &builtinRenderSubagent);
}

} // namespace plugin
} // namespace agentxx
