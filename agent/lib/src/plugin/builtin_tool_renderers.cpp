/// builtin_tool_renderers.cpp —— lib 内置工具的工具行内容
///
/// 背景: 工具行内容 (`agentxx.client.tool` 上的声明项 + 内容) 通常由提供工具的
/// 插件注册; lib 内置工具 (agentxx_share_store / agentxx_subagent) 没有对应插件,
/// 因此由主程序自身注册 (见 ClientPluginManager::registerBuiltinToolContent)。
///
/// 与插件内容的区别只在“谁来算”: 内置工具行的折叠内容是一个普通函数, 在 client
/// io 线程同步跑完; 插件内容跑生成函数 (插件执行线程, 异步契约)。两者都进同一张
/// 渲染表与同一个按 (调用, 折叠/展开) 分键的缓存, 界面读取方式完全一致。
///
/// 渲染范围: 显示名注册时给定, 这里只产出折叠态的一行内容。不提供展开内容 ——
/// 展开体保持主程序默认展示 (参数 JSON + 结果文本), 特化渲染不隐藏工具的原始信息。
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
/// agentxx::tools::SubAgentManagerTool 以及 async_tools / async_emit 的工具族)
constexpr std::string_view kToolShareStore  = "agentxx_share_store";
constexpr std::string_view kToolSubagent    = "agentxx_subagent";
constexpr std::string_view kToolAsyncRun    = "agentxx_async_run";
constexpr std::string_view kToolAsyncQuery  = "agentxx_async_query";
constexpr std::string_view kToolAsyncAwait  = "agentxx_async_await";
constexpr std::string_view kToolAsyncMgr    = "agentxx_async_manager";
constexpr std::string_view kToolAsyncEmit   = "agentxx_async_emit";

/// 显示名 (与插件渲染器规则一致: 简短英文名, 如 Read/Edit/Bash)
/// - 后台任务族统一带 `Async` 前缀: 头行一眼能看出这是后台任务而不是前台调用
constexpr std::string_view kNameShareStore = "Store";
constexpr std::string_view kNameSubagent   = "Subagent";
constexpr std::string_view kNameAsyncRun   = "AsyncRun";
constexpr std::string_view kNameAsyncQuery = "AsyncQuery";
constexpr std::string_view kNameAsyncAwait = "AsyncAwait";
constexpr std::string_view kNameAsyncMgr   = "AsyncManager";
constexpr std::string_view kNameAsyncEmit  = "AsyncEmit";

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

/// 取 JSON 里的 id 文本 (数字与字符串都接受), 缺失/非数字返回空串
/// - 工具参数 schema 把 `id` 声明为字符串, `new` 的结果里却是数字 id, 两种形态都要认
std::string idTextOf(const utilxx_base::Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return std::to_string(value.get<int64_t>());
    }
    if (value.is_number_float()) {
        return fmt::format("{:g}", value.get<double>());
    }
    return {};
}

/// 解析 `agentxx_share_store` new 结果中的新内容 id (结果形如 {"id":3})
/// - 非该形态/解析失败返回空串
std::string shareStoreResultId(std::string_view result) {
    if (result.empty() || result.front() != '{') {
        return {};
    }
    try {
        const auto j = utilxx_base::Json::parse(result);
        if (!j.is_object()) {
            return {};
        }
        return idTextOf(j["id"]);
    } catch (...) {
        return {};
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
/// - id 在参数里是字符串 (schema 如此), 在 `new` 的结果里是数字: 两种都认
/// - 区间只由 get 展示 (offset/limit 只对 get 生效; new/update 整段原样存储, 不带区间)
/// - 未识别的 opt (协议扩展) 原样展示; 参数未就绪时返回空串
std::string renderShareStoreCollapsed(const ClientToolRenderInput& in) {
    auto args = parseToolArgs(in.argsJson);
    if (!args) {
        return {};
    }
    const auto&       argsObj = *args; // 只读访问: 缺失的键不会插进参数里
    const std::string opt     = args->value("opt", std::string{});
    const std::string argId   = idTextOf(argsObj["id"]);
    const int64_t     offset  = args->value<int64_t>("offset", -1);
    const int64_t     limit   = args->value<int64_t>("limit", -1);

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
        auto shownId = argId;
        if (opt == "new" && in.finished) {
            shownId = shareStoreResultId(in.resultText);
        }
        // 判空看实际要显示的那个 (new 运行中没有 id, 完成后才由结果补上)
        if (shownId.empty() == false) {
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
std::string renderSubagentCollapsed(const ClientToolRenderInput& in) {
    auto args = parseToolArgs(in.argsJson);
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
        = oneLinePreview(subagentTaskText(*args), summaryPreviewCols(in.maxWidth));

    if (!name.empty() && !task.empty()) {
        return name + " · " + task;
    }
    return !name.empty() ? name : task;
}

/// 毫秒时长的简短写法 (整秒给 `5s`, 否则给 `500ms`)
std::string shortDuration(int64_t ms) {
    if (ms <= 0) {
        return {};
    }
    if (ms % 1000 == 0) {
        return fmt::format("{}s", ms / 1000);
    }
    return fmt::format("{}ms", ms);
}

/// 取任务 id 列表的展示文本 (<= 3 个, 多余补 `...`)
/// - 声明是数组; 模型给单个字符串时由执行层按 schema 自愈成数组, 这里两种都认
std::string idListText(const utilxx_base::Json& args) {
    std::string out;
    size_t      shown = 0;
    size_t      total = 0;
    const auto  add   = [&out](std::string_view id) {
        if (id.empty()) {
            return false;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += id;
        return true;
    };
    if (!args.contains("id")) {
        return out;
    }
    const auto& ids = args["id"];
    if (ids.is_array()) {
        for (const auto& item : ids) {
            ++total;
            if (shown >= 3 || !item.is_string()) {
                continue;
            }
            if (add(item.get<std::string>())) {
                ++shown;
            }
        }
    } else if (ids.is_string()) {
        total = 1;
        add(ids.get<std::string>());
    }
    if (total > shown) {
        out += ", ...";
    }
    return out;
}

/// `agentxx_async_run` 的任务目标文本: 子代理给 "subagent <名字>", 其余给工具名
std::string asyncRunTarget(const utilxx_base::Json& args) {
    const std::string tool = args.value("tool", std::string{});
    if (tool.empty()) {
        return {};
    }
    if (tool != kToolSubagent) {
        return tool;
    }
    const auto& inner = args.contains("args") ? args["args"] : utilxx_base::Json{};
    const std::string name
        = inner.is_object() ? inner.value("subagent", std::string{}) : std::string{};
    return name.empty() ? tool : fmt::format("subagent {}", name);
}

/// 结果里的 `asyncId` (受理成功才有; 失败/未完成返回空串)
std::string asyncIdOfResult(std::string_view result) {
    if (result.empty() || result.front() != '{') {
        return {};
    }
    try {
        const auto j = utilxx_base::Json::parse(result);
        if (!j.is_object()) {
            return {};
        }
        return idTextOf(j["asyncId"]);
    } catch (...) {
        return {};
    }
}

/// `agentxx_async_run` 折叠内容: "<目标工具> +5s (async-3)"
/// - 起跑延时只在给了的时候画; 任务 id 只有受理后才知道 (运行中没有后缀)
std::string renderAsyncRunCollapsed(const ClientToolRenderInput& in) {
    auto args = parseToolArgs(in.argsJson);
    if (!args) {
        return {};
    }
    std::string text = asyncRunTarget(*args);
    const auto  delay = shortDuration(args->value<int64_t>("delayMs", 0));
    if (!delay.empty()) {
        if (text.empty()) {
            text = "+ " + delay;
        } else {
            text += " +" + delay;
        }
    }
    if (in.finished) {
        const auto id = asyncIdOfResult(in.resultText);
        if (!id.empty()) {
            text += text.empty() ? fmt::format("({})", id) : fmt::format(" ({})", id);
        }
    }
    return text;
}

/// `agentxx_async_query` 折叠内容: "id=async-3" / "group=running"
std::string renderAsyncQueryCollapsed(const ClientToolRenderInput& in) {
    auto args = parseToolArgs(in.argsJson);
    if (!args) {
        return {};
    }
    const std::string id = args->value("id", std::string{});
    if (!id.empty()) {
        return "id=" + id;
    }
    std::string group = args->value("group", std::string{});
    if (group.empty()) {
        group = "all";
    }
    return "group=" + group;
}

/// `agentxx_async_await` 折叠内容: "async-3 (30s)" (超时只在显式给了时画)
std::string renderAsyncAwaitCollapsed(const ClientToolRenderInput& in) {
    auto args = parseToolArgs(in.argsJson);
    if (!args) {
        return {};
    }
    std::string text = args->value("id", std::string{});
    if (text.empty()) {
        return {};
    }
    const std::string timeout = shortDuration(args->value<int64_t>("timeoutMs", 0));
    if (!timeout.empty()) {
        text += fmt::format(" ({})", timeout);
    }
    return text;
}

/// `agentxx_async_manager` 折叠内容: "cancel async-3, async-5" / "delay async-3 +5s"
std::string renderAsyncManagerCollapsed(const ClientToolRenderInput& in) {
    auto args = parseToolArgs(in.argsJson);
    if (!args) {
        return {};
    }
    const std::string opt = args->value("opt", std::string{});
    if (opt.empty()) {
        return {};
    }
    const std::string ids = idListText(*args);
    std::string       text = ids.empty() ? opt : fmt::format("{} {}", opt, ids);
    if (opt == "delay") {
        const auto delay = shortDuration(args->value<int64_t>("delayMs", 0));
        text += delay.empty() ? " +0s" : " +" + delay;
    }
    return text;
}

/// `agentxx_async_emit` 折叠内容: "<事件名> (end)"
std::string renderAsyncEmitCollapsed(const ClientToolRenderInput& in) {
    auto args = parseToolArgs(in.argsJson);
    if (!args) {
        return {};
    }
    const std::string event = args->value("event", std::string{});
    if (event.empty()) {
        return {};
    }
    const bool ending = args->value("end", false);
    return ending ? event + " (end)" : event;
}

} // namespace

std::string builtinShareStoreSummary(const ClientToolRenderInput& input) {
    return renderShareStoreCollapsed(input);
}

std::string builtinSubagentSummary(const ClientToolRenderInput& input) {
    return renderSubagentCollapsed(input);
}

std::string builtinAsyncRunSummary(const ClientToolRenderInput& input) {
    return renderAsyncRunCollapsed(input);
}

std::string builtinAsyncQuerySummary(const ClientToolRenderInput& input) {
    return renderAsyncQueryCollapsed(input);
}

std::string builtinAsyncAwaitSummary(const ClientToolRenderInput& input) {
    return renderAsyncAwaitCollapsed(input);
}

std::string builtinAsyncManagerSummary(const ClientToolRenderInput& input) {
    return renderAsyncManagerCollapsed(input);
}

std::string builtinAsyncEmitSummary(const ClientToolRenderInput& input) {
    return renderAsyncEmitCollapsed(input);
}

void registerBuiltinToolRows(ClientPluginManager& mgr) {
    ClientToolRowContent shareStore;
    shareStore.summary = &builtinShareStoreSummary;
    mgr.registerBuiltinToolContent(kToolShareStore, kNameShareStore, std::move(shareStore));

    ClientToolRowContent subagent;
    subagent.summary = &builtinSubagentSummary;
    mgr.registerBuiltinToolContent(kToolSubagent, kNameSubagent, std::move(subagent));

    // 后台任务族 (async_tools / async_emit): 运行中也要有内容 ——
    // 没内容的工具行只能显示 "正在调用工具...", 而后台任务的参数本身（定位哪个工具/
    // 哪个任务) 就是用户最关心的信息
    ClientToolRowContent asyncRun;
    asyncRun.summary = &builtinAsyncRunSummary;
    mgr.registerBuiltinToolContent(kToolAsyncRun, kNameAsyncRun, std::move(asyncRun));

    ClientToolRowContent asyncQuery;
    asyncQuery.summary = &builtinAsyncQuerySummary;
    mgr.registerBuiltinToolContent(kToolAsyncQuery, kNameAsyncQuery, std::move(asyncQuery));

    ClientToolRowContent asyncAwait;
    asyncAwait.summary = &builtinAsyncAwaitSummary;
    mgr.registerBuiltinToolContent(kToolAsyncAwait, kNameAsyncAwait, std::move(asyncAwait));

    ClientToolRowContent asyncManager;
    asyncManager.summary = &builtinAsyncManagerSummary;
    mgr.registerBuiltinToolContent(kToolAsyncMgr, kNameAsyncMgr, std::move(asyncManager));

    ClientToolRowContent asyncEmit;
    asyncEmit.summary = &builtinAsyncEmitSummary;
    mgr.registerBuiltinToolContent(kToolAsyncEmit, kNameAsyncEmit, std::move(asyncEmit));
}

} // namespace plugin
} // namespace agentxx
