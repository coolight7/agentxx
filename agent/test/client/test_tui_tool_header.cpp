// TUI 特化渲染: 工具调用头部摘要 (buildToolHeaderSummary)
//
// 覆盖已知工具头部渲染为 "动词 · 参数摘要" 的场景, 以及未知工具/参数
// 解析失败回退显示原始 toolName 的降级路径:
// - filesystem 系列: list / read_text_file (含 [offset, limit] 区间) /
//   write_file / edit_text_file (含 "[+行 -行] path" diff 提示) / glob / grep
// - web_search 系列: web_search / web_fetch_url / web_fetch_url_markdown
// - 降级: 未知工具名 / 非 JSON 参数 -> 头部仍显示原始 toolName
#include "agentxx-test/client/test_tui_tool_header.h"

#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx-test/test_framework.h"
#include "agentxx/plugin/builtin_tool_renderers.h"
#include "agentxx_filesystem/agentxx_fs_plugin.h"
#include "asio/io_context.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "pluginxx/runtime/runtime.h" // mgr->runtime()->ioThreadId (异步渲染中间帧夹具)
#include "utilxx_base/json.h"
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_tool_header_passed = 0;
int g_tui_tool_header_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_tool_header_passed
#define XX_TEST_FAILED g_tui_tool_header_failed

namespace agentxx {
namespace test {

using namespace agentxx::client;

namespace {

static PluginxxString makeTestString(std::string_view s) {
    if (s.empty()) {
        return PluginxxString{nullptr, 0};
    }
    char* buf = static_cast<char*>(std::malloc(s.size() + 1));
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    return PluginxxString{buf, static_cast<uint64_t>(s.size())};
}

} // namespace

std::shared_ptr<agentxx::plugin::ClientUiRegistry> makeTestToolRegistry() {
    auto reg = std::make_shared<agentxx::plugin::ClientUiRegistry>();

    // List (模板)
    reg->toolRenderers.push_back({
        .plugin              = "agentxx_filesystem",
        .toolName            = "agentxx_filesystem_list",
        .renderFn            = nullptr,
        .userData            = nullptr,
        .templateDisplayName = "List",
        .templateSummaryKey  = "path",
    });

    // Write (模板)
    reg->toolRenderers.push_back({
        .plugin              = "agentxx_filesystem",
        .toolName            = "agentxx_filesystem_write",
        .renderFn            = nullptr,
        .userData            = nullptr,
        .templateDisplayName = "Write",
        .templateSummaryKey  = "path",
    });

    // Read (回调)
    static auto readFn
        = [](void*, const AgentxxToolRenderInput* in, AgentxxToolRenderOutput* out) -> int32_t {
        std::string_view  args(in->args_json.data ? in->args_json.data : "", in->args_json.size);
        utilxx_base::Json j;
        try {
            j = utilxx_base::Json::parse(args);
        } catch (...) {
            return -1;
        }
        if (!j.is_object()) {
            return -1;
        }
        std::string path = j.value("path", std::string{});
        int64_t     off  = j.value("offset", int64_t{-1});
        int64_t     lim  = j.value("limit", int64_t{-1});
        std::string range;
        if (off <= 0 && lim <= 0) {
            // empty
        } else if (off <= 0) {
            range = fmt::format("0, {}", lim);
        } else if (lim <= 0) {
            range = fmt::format("{}, ~", off);
        } else {
            range = fmt::format("{}, {}", off, off + lim);
        }
        std::string summary = " ·";
        if (!range.empty()) {
            summary += " [" + range + "]";
        }
        if (!path.empty()) {
            summary += " " + path;
        }
        out->displayName = makeTestString("Read");
        out->summary     = makeTestString(summary);
        return 0;
    };
    reg->toolRenderers.push_back({
        .plugin   = "agentxx_filesystem",
        .toolName = "agentxx_filesystem_read",
        .renderFn = readFn,
    });

    // Glob (回调; 与真实插件回调同语义: 兼容单字符串/数组)
    // - 提取逻辑镜像 agentxx_fs_plugin::stringListArg (单字符串包装为单元素
    //   列表, 数组逐项提取字符串), 保证夹具摘要与真实渲染一致
    static auto globFilesOf = [](const utilxx_base::Json& j) -> std::vector<std::string> {
        std::vector<std::string> out;
        if (!j.is_object() || !j.contains("file_patterns")) {
            return out;
        }
        const auto& v = j["file_patterns"];
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
    };
    static auto globFn
        = [](void*, const AgentxxToolRenderInput* in, AgentxxToolRenderOutput* out) -> int32_t {
        std::string_view  args(in->args_json.data ? in->args_json.data : "", in->args_json.size);
        utilxx_base::Json j;
        try {
            j = utilxx_base::Json::parse(args);
        } catch (...) {
            return -1;
        }
        if (!j.is_object()) {
            return -1;
        }
        auto         files = globFilesOf(j);
        std::string  joined;
        const size_t n = std::min<size_t>(files.size(), 2);
        for (size_t i = 0; i < n; ++i) {
            if (i > 0) {
                joined += ", ";
            }
            joined += files[i];
        }
        if (files.size() > 2) {
            joined += (n > 0 ? ", ..." : "...");
        }
        out->displayName = makeTestString("Glob");
        out->summary     = makeTestString(" · " + joined);
        return 0;
    };
    reg->toolRenderers.push_back({
        .plugin   = "agentxx_filesystem",
        .toolName = "agentxx_filesystem_glob",
        .renderFn = globFn,
    });

    // Grep (回调; 与真实插件回调同语义: 兼容单字符串/数组)
    static auto grepListOf
        = [](const utilxx_base::Json& j, std::string_view key) -> std::vector<std::string> {
        std::vector<std::string> out;
        std::string              k{key};
        if (!j.is_object() || !j.contains(k)) {
            return out;
        }
        const auto& v = j[k];
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
    };
    static auto grepFn
        = [](void*, const AgentxxToolRenderInput* in, AgentxxToolRenderOutput* out) -> int32_t {
        std::string_view  args(in->args_json.data ? in->args_json.data : "", in->args_json.size);
        utilxx_base::Json j;
        try {
            j = utilxx_base::Json::parse(args);
        } catch (...) {
            return -1;
        }
        if (!j.is_object()) {
            return -1;
        }
        auto                     textPats  = grepListOf(j, "text_patterns");
        auto                     regexPats = grepListOf(j, "regex_patterns");
        auto                     files     = grepListOf(j, "file_patterns");
        std::vector<std::string> shown;
        for (const auto& p : textPats) {
            if (shown.size() >= 2) {
                break;
            }
            shown.push_back(p);
        }
        for (const auto& p : regexPats) {
            if (shown.size() >= 2) {
                break;
            }
            shown.push_back(p);
        }
        const size_t total = textPats.size() + regexPats.size();
        std::string  quoted;
        for (size_t i = 0; i < shown.size(); ++i) {
            if (i > 0) {
                quoted += ", ";
            }
            quoted += '"' + shown[i] + '"';
        }
        if (total > shown.size()) {
            quoted += ", ...";
        }
        std::string  joinedFiles;
        const size_t n = std::min<size_t>(files.size(), 2);
        for (size_t i = 0; i < n; ++i) {
            if (i > 0) {
                joinedFiles += ", ";
            }
            joinedFiles += files[i];
        }
        if (files.size() > 2) {
            joinedFiles += (n > 0 ? ", ..." : "...");
        }
        std::string summary = fmt::format(" · [{}] {}", quoted, joinedFiles);
        out->displayName    = makeTestString("Grep");
        out->summary        = makeTestString(summary);
        return 0;
    };
    reg->toolRenderers.push_back({
        .plugin   = "agentxx_filesystem",
        .toolName = "agentxx_filesystem_grep",
        .renderFn = grepFn,
    });

    // Edit (回调)
    static auto editFn
        = [](void*, const AgentxxToolRenderInput* in, AgentxxToolRenderOutput* out) -> int32_t {
        std::string_view  args(in->args_json.data ? in->args_json.data : "", in->args_json.size);
        utilxx_base::Json j;
        try {
            j = utilxx_base::Json::parse(args);
        } catch (...) {
            return -1;
        }
        if (!j.is_object()) {
            return -1;
        }
        std::string path     = j.value("path", std::string{});
        auto        segments = agentxx_fs_plugin::editSegmentsArg(j);
        out->displayName     = makeTestString("Edit");
        // 折叠头摘要与真实插件一致: " · [+行 -行] path"
        // (增删行数经共享 helper 计算 —— 与插件渲染器、展开体 diff 三者同源,
        // 见 agentxx_fs_plugin.h 的 diffStatTextOfSegments; 各段各替换一处, 按段求和)
        std::string       summary  = " ·";
        const std::string diffStat = agentxx_fs_plugin::diffStatTextOfSegments(segments);
        if (!diffStat.empty()) {
            summary += " " + diffStat;
        }
        if (!path.empty()) {
            summary += " " + path;
        }
        out->summary = makeTestString(summary);
        if (!in->is_error) {
            // 展开体与真实插件一致: 每段一个 Diff 片段 (路径只在首段), 段间分隔线
            utilxx_base::Json arr          = utilxx_base::Json::array();
            bool              firstSegment = true;
            for (const auto& seg : segments) {
                if (seg.oldStr.empty() && seg.newStr.empty()) {
                    continue;
                }
                if (!firstSegment) {
                    arr.push_back(utilxx_base::Json{
                        {"kind", "Divider"}
                    });
                }
                arr.push_back(utilxx_base::Json{
                    {"kind",   "Diff"                             },
                    {"path",   firstSegment ? path : std::string{}},
                    {"oldStr", seg.oldStr                         },
                    {"newStr", seg.newStr                         },
                });
                firstSegment = false;
            }
            if (!arr.empty()) {
                out->items_json = makeTestString(arr.dump());
            }
        }
        return 0;
    };
    reg->toolRenderers.push_back({
        .plugin   = "agentxx_filesystem",
        .toolName = "agentxx_filesystem_edit",
        .renderFn = editFn,
    });

    // Search (模板)
    reg->toolRenderers.push_back({
        .plugin              = "agentxx_websearch",
        .toolName            = "agentxx_web_search",
        .renderFn            = nullptr,
        .userData            = nullptr,
        .templateDisplayName = "Search",
        .templateSummaryKey  = "query",
    });

    // Fetch (模板)
    reg->toolRenderers.push_back({
        .plugin              = "agentxx_websearch",
        .toolName            = "agentxx_web_fetch",
        .renderFn            = nullptr,
        .userData            = nullptr,
        .templateDisplayName = "Fetch",
        .templateSummaryKey  = "url",
    });

    // FetchMD (模板)
    reg->toolRenderers.push_back({
        .plugin              = "agentxx_websearch",
        .toolName            = "agentxx_web_fetch_markdown",
        .renderFn            = nullptr,
        .userData            = nullptr,
        .templateDisplayName = "FetchMD",
        .templateSummaryKey  = "url",
    });

    // Bash (模板)
    reg->toolRenderers.push_back({
        .plugin              = "agentxx_execute_command",
        .toolName            = "agentxx_execute_bash_command",
        .renderFn            = nullptr,
        .userData            = nullptr,
        .templateDisplayName = "Bash",
        .templateSummaryKey  = "command",
    });

    // Windows (模板)
    reg->toolRenderers.push_back({
        .plugin              = "agentxx_execute_command",
        .toolName            = "agentxx_execute_windows_command",
        .renderFn            = nullptr,
        .userData            = nullptr,
        .templateDisplayName = "Bash",
        .templateSummaryKey  = "command",
    });

    // Planning (回调; 镜像 agentxx_planning 插件注册的类型级渲染器):
    // 由工具参数/结果推导折叠头与展开体, 覆盖"重启恢复会话后历史 Tool 消息
    // 没有运行时装饰"的渲染路径 (实时调用的装饰优先级更高, 见 renderClientTool)
    static auto planningFn
        = [](void*, const AgentxxToolRenderInput* in, AgentxxToolRenderOutput* out) -> int32_t {
        std::string_view  args(in->args_json.data ? in->args_json.data : "", in->args_json.size);
        utilxx_base::Json a;
        try {
            a = args.empty() ? utilxx_base::Json::object() : utilxx_base::Json::parse(args);
        } catch (...) {
            return -1; // 参数不可解析: 回退通用渲染
        }
        if (!a.is_object()) {
            return -1;
        }
        // write: 参数即规划内容; read: 结果即保存的规划 JSON
        utilxx_base::Json plan;
        if (a.value("mode", std::string{}) == "read") {
            const std::string_view result(
                in->result_text.data ? in->result_text.data : "",
                in->result_text.size
            );
            try {
                plan = result.empty() ? utilxx_base::Json::object()
                                      : utilxx_base::Json::parse(result);
            } catch (...) {
                return -1;
            }
        } else {
            plan = a;
        }

        std::string summary;
        if (plan.contains("todos") && plan["todos"].is_array()) {
            for (const auto& td : plan["todos"]) {
                if (!td.is_object()) {
                    continue;
                }
                const auto content = td.value("content", std::string{});
                if (content.empty()) {
                    continue;
                }
                const auto state = td.value("state", std::string{});
                if (!summary.empty()) {
                    summary += "; ";
                }
                summary += fmt::format(
                    "{} {}",
                    state == "completed" ? "[#]" : (state == "in_progress" ? "[~]" : "[ ]"),
                    content
                );
            }
        }

        utilxx_base::Json items   = utilxx_base::Json::array();
        const auto        roadmap = plan.value("roadmap", std::string{});
        if (!roadmap.empty()) {
            utilxx_base::Json diagram;
            diagram["kind"]    = "diagram";
            diagram["mermaid"] = roadmap;
            items.push_back(std::move(diagram));
        }
        if (plan.contains("todos") && plan["todos"].is_array() && !plan["todos"].empty()) {
            utilxx_base::Json head;
            head["kind"] = "text";
            head["role"] = "normal";
            head["text"] = "|- Todo";
            items.push_back(std::move(head));
            for (const auto& td : plan["todos"]) {
                if (!td.is_object() || td.value("content", std::string{}).empty()) {
                    continue;
                }
                utilxx_base::Json row;
                row["kind"] = "text";
                row["role"] = "normal";
                row["text"] = fmt::format(
                    "{} {}",
                    td.value("state", std::string{}) == "completed" ? "[#]" : "[~]",
                    td.value("content", std::string{})
                );
                items.push_back(std::move(row));
            }
        }
        if (plan.contains("notes") && plan["notes"].is_string()) {
            utilxx_base::Json row;
            row["kind"] = "text";
            row["role"] = "hint";
            row["text"] = "|- Note " + plan["notes"].get<std::string>();
            items.push_back(std::move(row));
        }

        out->displayName = makeTestString("Plan");
        out->summary     = makeTestString(summary);
        out->items_json  = makeTestString(items.dump());
        return 0;
    };
    reg->toolRenderers.push_back({
        .plugin   = "agentxx_planning",
        .toolName = "agentxx_planning",
        .renderFn = planningFn,
    });

    // 主程序内置工具 (lib 内置实现, 没有对应插件): 主程序自身注册的渲染器
    // - 见 agentxx/plugin/builtin_tool_renderers.h
    reg->builtinToolRenderers.push_back({
        .plugin   = std::string{agentxx::plugin::kBuiltinRendererOwner},
        .toolName = "agentxx_share_store",
        .renderFn = &agentxx::plugin::builtinRenderShareStore,
        .builtin  = true,
    });
    reg->builtinToolRenderers.push_back({
        .plugin   = std::string{agentxx::plugin::kBuiltinRendererOwner},
        .toolName = "agentxx_subagent",
        .renderFn = &agentxx::plugin::builtinRenderSubagent,
        .builtin  = true,
    });

    return reg;
}

namespace {

/// 测试夹具: 固定视口的消息列表 (单条 Tool 消息即可完整展示头部)
struct ToolHeaderFixture {
    asio::io_context io;
    TUISharedState   sharedState;
    TUITheme         theme = TUITheme::darkTheme();

    TUICtx                                ctx;
    std::shared_ptr<MessageListComponent> comp;

    int width  = 120;
    int height = 16;

    ToolHeaderFixture(int w = 120, int h = 16) :
        width(w),
        height(h) {
        // 颜色断言依赖真彩色 ANSI 序列 (如 "102;204;255"); FTXUI 会按运行
        // 环境检测色彩能力, 无 COLORTERM=truecolor 时降级 256 色 (38;5;N)
        // 导致断言随环境变化 —— 测试内强制声明 TrueColor 支持
        ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
        sharedState.mutate([&](TUIRenderState& st) {
            st.pluginRegistry = makeTestToolRegistry();
        });
        ctx.state      = &sharedState;
        ctx.frameState = sharedState.readSnapshot();
        ctx.postRedraw = [] {};
        ctx.theme      = &theme;
        ctx.sessionId  = "s";
        ctx.remoteUrl  = "";
        comp           = std::make_shared<MessageListComponent>(ctx);
    }

    /// 追加一条 Tool 消息 (text 为工具参数 JSON, 与 server 端约定一致)
    /// - `hasError`: 失败标记, 由执行层给出 (异常/工具不存在/参数非法/权限拒绝等);
    ///   界面只按它选错误色与"失败回退通用展示"分支, 不再看结果文本前缀
    void pushTool(
        std::string name,
        std::string args,
        bool        finished   = true,
        bool        collapsed  = true,
        std::string result     = "",
        int64_t     durationMs = 0,
        bool        hasError   = false
    ) {
        sharedState.mutate([&](TUIRenderState& st) {
            auto m                = std::make_shared<TUIMessage>();
            m->role               = TUIMessage::Role::Tool;
            m->tool               = TUIMessage::ToolData{};
            m->tool->toolName     = std::move(name);
            m->tool->toolCallId   = "call_1";
            m->tool->toolFinished = finished;
            m->tool->toolResult   = std::move(result);
            m->tool->hasError     = hasError;
            m->durationMs         = durationMs;
            // Tool 消息默认折叠展示 (与真实 TUI 流一致, 见
            // [agent_tui.cpp](/agent/client/src/io/tui/agent_tui.cpp));
            // 折叠态头部才显示 "动词 · 参数摘要" 特化渲染
            m->collapsed = collapsed;
            m->text      = std::move(args);
            st.messages.push_back(std::move(m));
        });
    }

    /// 追加一条执行失败的 Tool 消息 (失败标记见
    /// [agentxx::agent::ViewMessage::ToolData::hasError])
    void pushFailedTool(
        std::string name,
        std::string args,
        std::string result,
        bool        collapsed = true
    ) {
        pushTool(std::move(name), std::move(args), true, collapsed, std::move(result), 0, true);
    }

    /// 把最后一条 Tool 消息置为已完成 (模拟 ToolEnd: 结果文本到达)。
    /// 工具结束时消息内容变化, 特化渲染的输入特征 (结果文本) 随之变化, 语义
    /// 缓存因此不命中 —— 用于覆盖"结果到达后的一帧"
    void finishLastTool(std::string result) {
        sharedState.mutate([&](TUIRenderState& st) {
            for (auto it = st.messages.rbegin(); it != st.messages.rend(); ++it) {
                if ((*it)->role != TUIMessage::Role::Tool || !(*it)->tool) {
                    continue;
                }
                (*it)->tool->toolFinished = true;
                (*it)->tool->toolResult   = std::move(result);
                return;
            }
        });
    }

    /// 追加一条 Think 消息 (头部为 "-/+/[Think]/预览")
    void pushThinking(std::string text, bool collapsed = true, int64_t durationMs = 0) {
        sharedState.mutate([&](TUIRenderState& st) {
            auto m        = std::make_shared<TUIMessage>();
            m->role       = TUIMessage::Role::Think;
            m->collapsed  = collapsed;
            m->durationMs = durationMs;
            m->text       = std::move(text);
            st.messages.push_back(std::move(m));
        });
    }

    /// 注入工具消息装饰 (模拟 planning 插件经 update_tool_decor 推送的
    /// 语义层装饰; toolCallId 与 pushTool 的 "call_1" 对应)
    void pushDecor() {
        sharedState.mutate([&](TUIRenderState& st) {
            auto  reg         = st.pluginRegistry
                                    ? std::make_shared<agentxx::plugin::ClientUiRegistry>(*st.pluginRegistry)
                                    : std::make_shared<agentxx::plugin::ClientUiRegistry>();
            auto& d           = reg->toolDecors.emplace_back();
            d.plugin          = "agentxx_planning";
            d.toolCallId      = "call_1";
            d.displayName     = "Plan";
            d.summary         = "[~] reproduce issue; [ ] fix root cause; [#] write tests";
            d.items           = utilxx_base::Json::parse(R"([
                {"kind":"diagram","mermaid":"stateDiagram-v2\n[*] --> phase1\nphase1 --> [*]"},
                {"kind":"text","role":"title","text":"Todos:"},
                {"kind":"text","role":"normal","text":"[~] do task A"},
                {"kind":"text","role":"hint","text":"- working on A"},
                {"kind":"text","role":"title","text":"[#] done task B"},
                {"kind":"text","role":"title","text":"Notes:"},
                {"kind":"text","role":"hint","text":"my notes content"}
            ])");
            st.pluginRegistry = std::move(reg);
        });
    }

    /// 注入含可点击按钮的工具消息装饰 (真实 planning 插件经 update_tool_decor 推送)
    void pushDecorWithButton() {
        sharedState.mutate([&](TUIRenderState& st) {
            auto  reg     = st.pluginRegistry
                                ? std::make_shared<agentxx::plugin::ClientUiRegistry>(*st.pluginRegistry)
                                : std::make_shared<agentxx::plugin::ClientUiRegistry>();
            auto& d       = reg->toolDecors.emplace_back();
            d.plugin      = "agentxx_planning";
            d.toolCallId  = "call_1";
            d.displayName = "Plan";
            d.summary     = "[~] reproduce issue";
            d.items       = utilxx_base::Json::parse(R"([
                {"kind":"Button","label":" Graph ","tone":"accent",
                 "action":{"kind":"dispatch","name":"planning.open_graph"}},
                {"kind":"Text","type":"title","text":"Todos:"},
                {"kind":"Text","text":"[~] do task A"}
            ])");
            // 模拟插件注册的 action 绑定 (按钮因此可点)
            agentxx::plugin::ClientActionBinding binding;
            binding.targetId = "";
            binding.plugin   = "agentxx_planning";
            binding.cb       = [](const AgentxxUiActionContext*, void*) {};
            binding.ud       = nullptr;
            reg->actionBindings.push_back(std::move(binding));
            st.pluginRegistry = std::move(reg);
        });
    }

    /// 注入含新组件 (表格/键值/计量条) 的工具消息装饰
    ///
    /// 装饰 items 与面板/Info/overlay/中断共用同一套组件 schema 与渲染实现,
    /// 该用例保护"装饰接入点同样能上屏新组件"。
    void pushDecorExtended() {
        sharedState.mutate([&](TUIRenderState& st) {
            auto  reg         = st.pluginRegistry
                                    ? std::make_shared<agentxx::plugin::ClientUiRegistry>(*st.pluginRegistry)
                                    : std::make_shared<agentxx::plugin::ClientUiRegistry>();
            auto& d           = reg->toolDecors.emplace_back();
            d.plugin          = "agentxx_codegraph";
            d.toolCallId      = "call_1";
            d.displayName     = "Index";
            d.summary         = "indexed 2 files";
            d.items           = utilxx_base::Json::parse(R"([
                {"kind":"Table","header":true,
                 "columns":[{"title":"Path"},{"title":"Lines","align":"end","width":48}],
                 "rows":[["main.cpp","120"],["io.cpp","42"]]},
                {"kind":"KV","pairs":[{"k":"Nodes","v":"171"}]}
            ])");
            st.pluginRegistry = std::move(reg);
        });
    }

    // ---- 异步特化渲染 (client io 线程执行 renderer) 的加载与驱动 ----

    /// client 插件管理器的 io 上下文 (仅在加载管理器后使用)
    asio::io_context                                      mgrIo;
    std::shared_ptr<agentxx::plugin::ClientPluginManager> mgr;

    /// 加载 client 插件管理器: 特化渲染改走异步语义缓存路径 (自定义 renderer
    /// 只在 client io 线程执行), 用于覆盖"结果尚未就绪"的中间帧。
    ///
    /// 与默认夹具 (不加载管理器 -> renderClientTool 走"无语义缓存"分支,
    /// renderer 在本线程同步执行) 的区别是本次要测的那条路径。
    ///
    /// 关键处理: 插件运行时的 IO 线程标识默认取绑定 executor 的线程 (= 本测试
    /// 线程), 而真机上界面线程与 client io 线程是两个线程。不把它清掉, 本线程
    /// 发出的渲染请求会被 postToIo 当作"已在 io 线程"内联执行, 中间帧根本不存在。
    /// 清掉之后请求只入队 (夹具的 io 上下文默认不驱动, 见 [drainIo])。
    void usePendingPluginManager() {
        mgr = std::make_shared<agentxx::plugin::ClientPluginManager>(mgrIo.get_executor());
        // 内置工具渲染器注册到管理器的注册表: 渲染请求由管理器在 client io 线程
        // 执行时按管理器的注册表找 renderer (真机两侧同源, 见
        // setupClientPlugins -> registerBuiltinToolRenderers)
        agentxx::plugin::registerBuiltinToolRenderers(*mgr);
        mgr->runtime()->ioThreadId.store(std::thread::id{}, std::memory_order_release);
        ctx.pluginManager = mgr;
    }

    /// 驱动一次夹具 io: 执行已入队的渲染请求 (client io 线程的那一步),
    /// 结果写入语义渲染缓存
    void drainIo() {
        mgrIo.restart();
        mgrIo.poll();
        mgrIo.restart();
    }

    /// 写入一条特化渲染条目 (用于构造"未就绪帧"的缓存状态)
    /// - 不设 inputHash (保持 0): UI 查询用真实输入特征算哈希, 因此该条目只会被
    ///   "上一份结果"查询 (lookupLatest) 取到, 不会被当作精确命中
    void pushRenderEntry(
        std::string key,
        std::string plugin,
        std::string displayName,
        std::string summary = {}
    ) {
        agentxx::plugin::ClientToolRenderEntry entry;
        entry.key         = std::move(key);
        entry.plugin      = std::move(plugin);
        entry.matched     = true;
        entry.displayName = std::move(displayName);
        entry.summary     = std::move(summary);
        mgr->toolRenderCache()->store(std::move(entry));
    }

    /// 写入"名称结果"条目: 占位提示阶段按工具名解析出的特化显示名
    /// (键 = "#工具名", 输入只有工具名, 无参数/结果)
    void pushToolNameResult(std::string toolName, std::string plugin, std::string displayName) {
        pushRenderEntry(
            agentxx::plugin::ClientToolRenderRequest::keyFor(std::string_view{}, toolName),
            std::move(plugin),
            std::move(displayName)
        );
    }

    /// 剥离 ANSI 转义序列, 获取纯文本表示
    static std::string stripAnsi(std::string_view str) {
        std::string out;
        out.reserve(str.size());
        for (size_t i = 0; i < str.size(); ++i) {
            if (str[i] == '\033' && i + 1 < str.size() && str[i + 1] == '[') {
                i += 2;
                while (i < str.size() && !(str[i] >= '@' && str[i] <= '~')) {
                    ++i;
                }
                continue;
            }
            out += str[i];
        }
        return out;
    }

    /// 渲染一帧并返回屏幕文本 (含 ANSI 转义控制字符)
    std::string render() {
        ctx.frameState = sharedState.readSnapshot();
        auto el        = comp->Render();
        auto screen    = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(width),
            ftxui::Dimension::Fixed(height)
        );
        ftxui::Render(screen, el);
        return screen.ToString();
    }

    /// 渲染一帧并返回纯文本 (剥离 ANSI 颜色/样式代码, 方便断言纯文本)
    std::string plainRender() {
        return stripAnsi(render());
    }
};

} // namespace

// filesystem 系列
void testTuiToolHeaderFilesystem() {
    ToolHeaderFixture f;

    // read_text_file: 完整 [offset, limit] 区间
    f.pushTool("agentxx_filesystem_read", R"({"path":"/home/a.cpp","offset":0,"limit":100})");
    XX_TEST_EXPECT_TRUE(f.render().find("Read · [0, 100] /home/a.cpp") != std::string::npos);

    // read_text_file: 无区间 (默认 -1) 不显示中括号
    f.pushTool("agentxx_filesystem_read", R"({"path":"/home/b.cpp"})");
    XX_TEST_EXPECT_TRUE(f.render().find("Read · /home/b.cpp") != std::string::npos);

    // read_text_file: 仅 limit
    f.pushTool("agentxx_filesystem_read", R"({"path":"/home/c.cpp","limit":50})");
    XX_TEST_EXPECT_TRUE(f.render().find("Read · [0, 50] /home/c.cpp") != std::string::npos);

    // read_text_file: 仅 offset
    f.pushTool("agentxx_filesystem_read", R"({"path":"/home/d.cpp","offset":10})");
    XX_TEST_EXPECT_TRUE(f.render().find("Read · [10, ~] /home/d.cpp") != std::string::npos);

    // list
    f.pushTool("agentxx_filesystem_list", R"({"path":"/home"})");
    XX_TEST_EXPECT_TRUE(f.render().find("List · /home") != std::string::npos);

    // write_file
    f.pushTool("agentxx_filesystem_write", R"({"path":"/home/out.txt","overwrite":true})");
    XX_TEST_EXPECT_TRUE(f.render().find("Write · /home/out.txt") != std::string::npos);

    // edit_text_file: 头部摘要含 "git 风格 +行 -行" 与路径 (diff 正文不受影响)
    f.pushTool("agentxx_filesystem_edit", R"({"path":"/home/e.cpp","old_str":"a","new_str":"b"})");
    XX_TEST_EXPECT_TRUE(f.render().find("Edit · [+1 -1] /home/e.cpp") != std::string::npos);

    // glob: 单模式 / 多模式折叠为前两项 + "..."
    f.pushTool("agentxx_filesystem_glob", R"({"file_patterns":["agent/lib/**/*.cpp"]})");
    XX_TEST_EXPECT_TRUE(f.render().find("Glob · agent/lib/**/*.cpp") != std::string::npos);
    f.pushTool("agentxx_filesystem_glob", R"({"file_patterns":["a","b","c","d"]})");
    XX_TEST_EXPECT_TRUE(f.render().find("Glob · a, b, ...") != std::string::npos);
    // glob: LLM 实际下发的单字符串形态 (非数组) 同样展示检索路径
    f.pushTool("agentxx_filesystem_glob", R"({"file_patterns":"agent/test/*.cpp"})");
    XX_TEST_EXPECT_TRUE(f.render().find("Glob · agent/test/*.cpp") != std::string::npos);

    // grep: 引号包裹的匹配模式 (参数区, 中括号) + 文件模式 (主参数)
    f.pushTool(
        "agentxx_filesystem_grep",
        R"({"text_patterns":["foo"],"file_patterns":["src/**/*.h"]})"
    );
    XX_TEST_EXPECT_TRUE(f.render().find(R"(Grep · ["foo"] src/**/*.h)") != std::string::npos);
    // grep: LLM 实际下发的单字符串形态 (file/text 均为单字符串) 同样展示参数
    f.pushTool(
        "agentxx_filesystem_grep",
        R"({"text_patterns":"startDaSimServer","file_patterns":"agent/test/core/test_agent.cpp"})"
    );
    XX_TEST_EXPECT_TRUE(
        f.render().find(R"(Grep · ["startDaSimServer"] agent/test/core/test_agent.cpp)")
        != std::string::npos
    );
    f.pushTool(
        "agentxx_filesystem_grep",
        R"({"text_patterns":["a","b","c"],"file_patterns":["agent/**/*.cpp"]})"
    );
    XX_TEST_EXPECT_TRUE(
        f.render().find(R"(Grep · ["a", "b", ...] agent/**/*.cpp)") != std::string::npos
    );
    // grep: regex_patterns 单独 / 与 text_patterns 并用 (摘要合并展示)
    f.pushTool(
        "agentxx_filesystem_grep",
        R"({"regex_patterns":["line[0-9]+"],"file_patterns":["src/**/*.cpp"]})"
    );
    XX_TEST_EXPECT_TRUE(
        f.render().find(R"(Grep · ["line[0-9]+"] src/**/*.cpp)") != std::string::npos
    );
    f.pushTool(
        "agentxx_filesystem_grep",
        R"({"text_patterns":["hello"],"regex_patterns":["world"],"file_patterns":["src/**/*.txt"]})"
    );
    XX_TEST_EXPECT_TRUE(
        f.render().find(R"(Grep · ["hello", "world"] src/**/*.txt)") != std::string::npos
    );
}

// web_search 系列
void testTuiToolHeaderWeb() {
    ToolHeaderFixture f;

    f.pushTool("agentxx_web_search", R"({"query":"hello world"})");
    XX_TEST_EXPECT_TRUE(f.render().find("Search · hello world") != std::string::npos);

    f.pushTool("agentxx_web_fetch", R"({"url":"https://example.com/a"})");
    XX_TEST_EXPECT_TRUE(f.render().find("Fetch · https://example.com/a") != std::string::npos);

    f.pushTool("agentxx_web_fetch_markdown", R"({"url":"https://example.com/b"})");
    XX_TEST_EXPECT_TRUE(f.render().find("FetchMD · https://example.com/b") != std::string::npos);
}

// 降级路径: 未知工具 / 非法参数 -> 回退显示原始 toolName
void testTuiToolHeaderFallback() {
    ToolHeaderFixture f;

    f.pushTool("unknown_tool_xyz", R"({"path":"/home"})");
    XX_TEST_EXPECT_TRUE(f.render().find("unknown_tool_xyz") != std::string::npos);

    // 参数不是 JSON (普通文本) 时解析失败, 同样回退 toolName
    f.pushTool("agentxx_filesystem_read", "not-a-json");
    XX_TEST_EXPECT_TRUE(f.render().find("agentxx_filesystem_read") != std::string::npos);
}

// 回归: 折叠头部内容超宽时前缀不得被压缩 (向左覆盖压缩 bug)
//
// ftxui hbox 在内容总宽超出容器时对全部子元素按比例收缩
// (box_helper::ComputeShrinkHard), 长摘要会把 "+ [Tool] " 前缀压扁、
// 摘要字符向左挤占覆盖。修复: 长内容元素加 xflex_shrink, 使其吸收剩余
// 宽度并在右缘裁剪, 前缀保持完整。
void testTuiToolHeaderOverflow() {
    // 窄视口 (内容宽度 = 29 列, 扣除滚动条 gutter)
    ToolHeaderFixture f(30, 8);

    // 摘要含超长路径, 远超视口宽度
    f.pushTool(
        "agentxx_filesystem_read",
        R"({"path":"/very/very/long/path/that/definitely/exceeds/the/narrow/viewport/width/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.cpp","offset":0,"limit":100})"
    );
    std::string out = f.render();
    // 前缀必须完整保留 (修复前会被按比例压扁, 丢失 "Tool]" 等字符)
    XX_TEST_EXPECT_TRUE(out.find("+ [Tool] ") != std::string::npos);
    // 摘要开头仍可见 (右缘裁剪)
    XX_TEST_EXPECT_TRUE(out.find("Read · ") != std::string::npos);

    // Think 折叠头部: 超长预览同样不得压缩 "- / [Think] " 前缀
    ToolHeaderFixture g(30, 8);
    g.pushThinking("这是一个非常非常非常非常非常非常非常非常非常非常非常非常非常长的思考内容"
                   "用来验证折叠预览超宽时头部前缀不会被压缩覆盖");
    std::string out2 = g.render();
    XX_TEST_EXPECT_TRUE(out2.find("+ [Think] ") != std::string::npos);
}

/// 装饰接入点: 新组件 (表格/键值) 在工具消息展开体里渲染上屏
void testTuiToolHeaderDecorExtended() {
    ToolHeaderFixture f(80, 14);
    f.pushTool("agentxx_codegraph_index", R"({"path":"."})", true, false);
    f.pushDecorExtended();

    const std::string out = f.plainRender();
    // 折叠头: 装饰声明的显示名 (展开态头部只显示显示名, 摘要用于折叠态)
    XX_TEST_EXPECT_TRUE(out.find("Index") != std::string::npos);
    // 展开体: 表格表头/单元格 + 键值对
    XX_TEST_EXPECT_TRUE(out.find("Path") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("main.cpp") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("io.cpp") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("Nodes") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("171") != std::string::npos);
}

/// edit 多段替换 (edits 数组) 的展开体: 每段一个 diff 片段 (路径只在首段,
/// 段间分隔线), 每个片段按整段差异逐行展示
void testTuiToolHeaderEditMultiSegmentBody() {
    ToolHeaderFixture f(80, 24);
    f.pushTool(
        "agentxx_filesystem_edit",
        R"({"path":"/home/seg.cpp","edits":[{"old_str":"oldA();","new_str":"newA();"},{"old_str":"oldB();","new_str":"newB();"}]})",
        true,
        false // 展开: 渲染插件给的特化展开体
    );

    const std::string out = f.plainRender();
    // 两段的差异都在展开体里 (每段一个 Diff 组件)
    XX_TEST_EXPECT_TRUE(out.find("oldA();") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("newA();") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("oldB();") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("newB();") != std::string::npos);
    // 段间分隔线 (Divider 组件渲染的横线)
    XX_TEST_EXPECT_TRUE(out.find("─") != std::string::npos);
    // 文件路径只出现一次 (仅首段携带 path)
    size_t pathCount = 0;
    for (size_t pos = out.find("/home/seg.cpp"); pos != std::string::npos;
         pos        = out.find("/home/seg.cpp", pos + 1)) {
        ++pathCount;
    }
    XX_TEST_EXPECT_EQ(pathCount, size_t{1});

    // `edits` **单个对象** (少写一层数组的形态) 与"只含一项的数组"渲染一致:
    // 工具参数的类型自动修正会把两种形态互转, 展开体两种写法都得认
    ToolHeaderFixture single(80, 24);
    single.pushTool(
        "agentxx_filesystem_edit",
        R"({"path":"/home/one.cpp","edits":{"old_str":"oldA();","new_str":"newA();"}})",
        true,
        false
    );
    ToolHeaderFixture arrayOfOne(80, 24);
    arrayOfOne.pushTool(
        "agentxx_filesystem_edit",
        R"({"path":"/home/one.cpp","edits":[{"old_str":"oldA();","new_str":"newA();"}]})",
        true,
        false
    );
    const std::string singleOut = single.plainRender();
    XX_TEST_EXPECT_TRUE(singleOut.find("oldA();") != std::string::npos);
    XX_TEST_EXPECT_TRUE(singleOut.find("newA();") != std::string::npos);
    XX_TEST_EXPECT_TRUE(singleOut.find("/home/one.cpp") != std::string::npos);
    XX_TEST_EXPECT_EQ(singleOut, arrayOfOne.plainRender());
}

/// 装饰区域尺寸上报: owner = tool_call_id (插件据此查询 regionSize / 收 UI_LAYOUT)
void testTuiToolHeaderDecorRegionSize() {
    asio::io_context io;
    auto mgr = std::make_shared<agentxx::plugin::ClientPluginManager>(io.get_executor());

    ToolHeaderFixture f(80, 14);
    f.ctx.pluginManager = mgr; // 夹具持有 TUICtx 引用: 构造后注入同样生效
    f.pushTool("agentxx_codegraph_index", R"({"path":"."})", true, false);
    f.pushDecorExtended();
    f.render();
    // 第二帧: 可见性上报用上一帧的可见区域 (1 帧延迟), 故再渲染一帧
    f.render();

    bool found = false;
    for (const auto& r : mgr->regionSizes()) {
        if (r.id == "call_1") { // 装饰 owner = tool_call_id
            found = true;
            XX_TEST_EXPECT_TRUE(r.width > 0);
            XX_TEST_EXPECT_TRUE(r.height >= 2); // 表格 3 行 + 键值 1 行
        }
    }
    XX_TEST_EXPECT_TRUE(found);
    // 可见性上报同样以 tool_call_id 为键 (pause_when_hidden 门控)
    XX_TEST_EXPECT_TRUE(mgr->isRegionVisible("call_1"));
}

// 运行中工具消息折叠展示 (默认不自动展开, 头部展示参数 toolName 高亮)
void testTuiToolHeaderRunning() {
    ToolHeaderFixture f;

    // 已知工具在 running 状态下折叠展示 "Read · /path"
    f.pushTool("agentxx_filesystem_read", R"({"path":"/home/running.cpp"})", false);
    XX_TEST_EXPECT_TRUE(f.plainRender().find("Read · /home/running.cpp") != std::string::npos);
    // 样式断言: 包含 accentColor 高亮颜色代码 (102;204;255)
    XX_TEST_EXPECT_TRUE(f.render().find("102;204;255") != std::string::npos);

    // 已知带区间工具在 running 状态下折叠展示 "Read · [0, 100] /path"
    ToolHeaderFixture fReadRange;
    fReadRange.pushTool(
        "agentxx_filesystem_read",
        R"({"path":"/home/running.cpp","offset":0,"limit":100})",
        false
    );
    XX_TEST_EXPECT_TRUE(
        fReadRange.plainRender().find("Read · [0, 100] /home/running.cpp") != std::string::npos
    );

    // bash 工具在 running 状态下折叠展示 "Bash · command"
    ToolHeaderFixture fBash;
    fBash.pushTool("agentxx_execute_bash_command", R"({"command":"ls -la"})", false);
    XX_TEST_EXPECT_TRUE(fBash.plainRender().find("Bash · ls -la") != std::string::npos);

    // plan 工具在 running 状态下折叠展示装饰头 "Plan · [~] ..."
    // (插件在 tool_start 即推送装饰, 运行中即有摘要)
    ToolHeaderFixture fPlan;
    fPlan.pushTool(
        "agentxx_planning",
        R"({"mode":"write","roadmap":"[*] --> s1\ns1 --> [*]","todos":[{"state":"in_progress","content":"reproduce issue"}]})",
        false
    );
    fPlan.pushDecor();
    XX_TEST_EXPECT_TRUE(
        fPlan.plainRender().find("Plan · [~] reproduce issue") != std::string::npos
    );

    // 未知工具在 running 状态下折叠展示 "toolName · "
    ToolHeaderFixture f2;
    f2.pushTool("custom_tool_run", R"({})", false);
    XX_TEST_EXPECT_TRUE(f2.plainRender().find("custom_tool_run · ") != std::string::npos);

    // 未知工具带参数在 running 状态下折叠展示 "toolName · args..."
    ToolHeaderFixture f3;
    f3.pushTool("custom_tool_run", R"({"key":"val"})", false);
    XX_TEST_EXPECT_TRUE(
        f3.plainRender().find("custom_tool_run · {\"key\":\"val\"}") != std::string::npos
    );
}

// 特化渲染结果尚未就绪时的折叠头 / 占位行 (回归):
//
// 自定义 renderer 只在 client io 线程执行, 结果写入语义缓存前每帧都会走到
// renderClientTool 的 "pendingRender" 分支。修复前该分支直接回退通用展示
// (原始工具名 + 原始参数 JSON), 而占位提示刚展示过特化显示名, 于是工具消息
// 出现时会闪一下非特化内容 (如 "agentxx_filesystem_read · {\"path\":...}"),
// 下一帧才变成 "Read · [0, 100] /home/a.cpp"; 工具结果到达时 (finished 输入变了)
// 会因同样的原因再闪一次 (显示原始结果首行)。Bash 等用预设模版注册的工具在 UI
// 线程同步算出摘要, 没有该中间帧, 因此不闪。
//
// 未就绪帧同样不能"留空": 空一帧同样是跳变 (显示名或摘要消失再回来)。该分支
// 现在按"上一份已知内容"过渡 —— 本次调用上一份结果优先, 其次该工具的名称结果;
// 两者都没有时, 运行中沿用占位提示的同一句说明文字 (该帧与占位行逐字一致),
// 已完成则不显示说明文字 (见 renderClientTool 与 message_list 的 [pendingRender] 分支)。
void testTuiToolHeaderPendingRender() {
    // 1. 占位提示 (工具调用阶段): 名称结果已解析 -> 沿用特化显示名, 不显示原始工具名
    {
        ToolHeaderFixture f;
        f.usePendingPluginManager();
        f.pushToolNameResult("agentxx_filesystem_read", "agentxx_filesystem", "Read");
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.genPhase    = agentxx::agent::WireDelta::GenPhase::ToolCall;
            st.genToolName = "agentxx_filesystem_read";
            st.isStreaming = true;
        });
        const std::string out = f.plainRender();
        XX_TEST_EXPECT_TRUE(out.find("Read receiving tool call...") != std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("agentxx_filesystem_read") == std::string::npos);
    }

    // 2. 占位提示: 名称结果还没算出来 -> 不显示原始工具名 (等特化显示名上屏)
    {
        ToolHeaderFixture f;
        f.usePendingPluginManager();
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.genPhase    = agentxx::agent::WireDelta::GenPhase::ToolCall;
            st.genToolName = "agentxx_filesystem_read";
            st.isStreaming = true;
        });
        const std::string out = f.plainRender();
        XX_TEST_EXPECT_TRUE(out.find("receiving tool call...") != std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("agentxx_filesystem_read") == std::string::npos);
    }

    // 3. 运行中的工具消息: 沿用名称结果的显示名, 不展示原始工具名与原始参数 JSON,
    //    摘要未就绪时沿用占位提示的同一句说明文字 (该帧与占位行逐字一致, 不留空档)
    {
        ToolHeaderFixture f;
        f.usePendingPluginManager();
        f.pushToolNameResult("agentxx_filesystem_read", "agentxx_filesystem", "Read");
        f.pushTool(
            "agentxx_filesystem_read",
            R"({"path":"/home/a.cpp","offset":0,"limit":100})",
            false // 运行中
        );
        const std::string out = f.plainRender();
        // 运行中头部使用加载动画 (非 "+"), 故只断言角色标签之后的内容
        XX_TEST_EXPECT_TRUE(out.find("[Tool] Read") != std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("Read receiving tool call...") != std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("agentxx_filesystem_read") == std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("\"path\"") == std::string::npos);
    }

    // 4. 运行中且名称结果也没有 (某工具本会话首次调用): 只显示角色标签 + 占位提示
    //    的同一句说明文字, 同样不展示原始工具名与原始参数 JSON
    {
        ToolHeaderFixture f;
        f.usePendingPluginManager();
        f.pushTool("agentxx_filesystem_read", R"({"path":"/home/b.cpp"})", false);
        const std::string out = f.plainRender();
        XX_TEST_EXPECT_TRUE(out.find("[Tool]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("receiving tool call...") != std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("agentxx_filesystem_read") == std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("\"path\"") == std::string::npos);
    }

    // 5. 已完成但结果的特化渲染未就绪: 不展示原始结果预览
    {
        ToolHeaderFixture f;
        f.usePendingPluginManager();
        f.pushToolNameResult("agentxx_filesystem_read", "agentxx_filesystem", "Read");
        f.pushTool(
            "agentxx_filesystem_read",
            R"({"path":"/home/c.cpp"})",
            true, // 已完成
            true, // 折叠
            "     1|file content line one"
        );
        const std::string out = f.plainRender();
        XX_TEST_EXPECT_TRUE(out.find("+ [Tool] Read") != std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("file content line one") == std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("agentxx_filesystem_read") == std::string::npos);
    }

    // 6. 没有特化渲染器的工具不受影响 (原始工具名 + 参数预览照常展示)
    {
        ToolHeaderFixture f;
        f.usePendingPluginManager();
        f.pushTool("custom_tool_run", R"({"key":"val"})", false);
        XX_TEST_EXPECT_TRUE(
            f.plainRender().find("custom_tool_run · {\"key\":\"val\"}") != std::string::npos
        );
    }

    // 7. 全流程 (主程序内置渲染器): 未就绪帧不展示原始参数, 请求执行后切到特化摘要
    {
        ToolHeaderFixture f;
        f.usePendingPluginManager();
        f.pushTool("agentxx_share_store", R"({"opt":"new","text":"a\nb"})", false);
        const std::string pending = f.plainRender();
        XX_TEST_EXPECT_TRUE(pending.find("agentxx_share_store") == std::string::npos);
        XX_TEST_EXPECT_TRUE(pending.find("\"opt\"") == std::string::npos);
        // 次帧: 内容区宽度经实测稳定后重新查询一次 (输入特征含宽度, 首帧宽度尚未
        // 测量, 请求的输入特征与后续帧不同)
        XX_TEST_EXPECT_TRUE(f.plainRender().find("\"opt\"") == std::string::npos);

        // 布局 (内容宽度/滚动条列) 需要几帧才稳定, 而宽度属于渲染请求的输入特征:
        // 逐帧"渲染 -> 驱动 io 执行已入队请求"直到特化摘要上屏
        std::string ready;
        for (int i = 0; i < 8; ++i) {
            ready = f.plainRender();
            if (ready.find("Store · new") != std::string::npos) {
                break;
            }
            f.drainIo();
        }
        XX_TEST_EXPECT_TRUE(ready.find("Store · new") != std::string::npos);
        XX_TEST_EXPECT_TRUE(ready.find("agentxx_share_store") == std::string::npos);
    }

    // 8. 工具结束 (结果文本到达使输入特征变化) 的那一帧: 继续显示上一份摘要,
    //    不空白也不要原始结果预览 —— 摘要消失一帧再回来同样是可见的跳变。
    //    用主程序内置渲染器 (share_store) 走完整流程: 运行中的摘要由 client io
    //    线程算出并写入缓存, 结果到达后的完成态摘要补上结果里的新 id (可区分)
    {
        ToolHeaderFixture f;
        f.usePendingPluginManager();
        f.pushTool(
            "agentxx_share_store",
            R"({"opt":"new","text":"a\nb"})",
            false // 运行中
        );
        // 逐帧驱动 io 直到运行中的特化摘要上屏 (布局宽度实测稳定后才定稿)
        std::string running;
        for (int i = 0; i < 8; ++i) {
            running = f.plainRender();
            if (running.find("Store · new") != std::string::npos) {
                break;
            }
            f.drainIo();
        }
        XX_TEST_EXPECT_TRUE(running.find("Store · new") != std::string::npos);

        // 结果到达, 本帧特化结果尚未算出 (不驱动 io): 头部保持上一份摘要
        f.finishLastTool(R"({"id":7})");
        const std::string endFrame = f.plainRender();
        XX_TEST_EXPECT_TRUE(endFrame.find("Store · new") != std::string::npos);
        XX_TEST_EXPECT_TRUE(endFrame.find("\"id\"") == std::string::npos);
        XX_TEST_EXPECT_TRUE(endFrame.find("agentxx_share_store") == std::string::npos);

        // 完成态的特化结果上屏: 摘要补上结果里的新 id (正常的内容更新, 不是空白帧)
        f.drainIo();
        const std::string done = f.plainRender();
        XX_TEST_EXPECT_TRUE(done.find("Store · new #7") != std::string::npos);
        XX_TEST_EXPECT_TRUE(done.find("agentxx_share_store") == std::string::npos);
    }

    // 9. 已完成消息 (块可跨帧缓存, 只有这类块才走"版本号变化 -> 重建"判定) 在两个
    //    缓存条目都还没有时: 先显示空显示名, 名称结果到达后补上特化显示名 ——
    //    消息块缓存 key 必须计入名称结果的版本号, 否则折叠头会一直停在无显示名形态
    {
        ToolHeaderFixture f;
        f.usePendingPluginManager();
        f.pushTool(
            "agentxx_filesystem_read",
            R"({"path":"/home/e.cpp"})",
            true, // 已完成: 无加载动画, 块可缓存
            true,
            "     1|file content line one"
        );
        const std::string before = f.plainRender();
        XX_TEST_EXPECT_TRUE(before.find("+ [Tool]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(before.find("[Tool] Read") == std::string::npos);
        XX_TEST_EXPECT_TRUE(before.find("agentxx_filesystem_read") == std::string::npos);

        f.pushToolNameResult("agentxx_filesystem_read", "agentxx_filesystem", "Read");
        const std::string after = f.plainRender();
        XX_TEST_EXPECT_TRUE(after.find("+ [Tool] Read") != std::string::npos);
        XX_TEST_EXPECT_TRUE(after.find("agentxx_filesystem_read") == std::string::npos);
    }

    // 10. 该调用有"上一份结果"、但那份结果没给显示名 (renderer 返回空显示名) 时,
    //     名称结果到达同样要使块重建 —— 只要有显示名来自名称结果的可能, 它的版本号
    //     就要计入 key (否则末帧停在无显示名形态, 直到该调用自己的结果算出为止)
    {
        ToolHeaderFixture f;
        f.usePendingPluginManager();
        f.pushTool(
            "agentxx_filesystem_read",
            R"({"path":"/home/f.cpp"})",
            true,
            true,
            "     1|file content line one"
        );
        // 上一份结果: matched 但显示名为空, 输入特征与本次查询不同 (仍是未就绪帧)
        f.pushRenderEntry("call_1", "agentxx_filesystem", "");
        const std::string before = f.plainRender();
        XX_TEST_EXPECT_TRUE(before.find("[Tool] Read") == std::string::npos);
        XX_TEST_EXPECT_TRUE(before.find("agentxx_filesystem_read") == std::string::npos);

        f.pushToolNameResult("agentxx_filesystem_read", "agentxx_filesystem", "Read");
        const std::string after = f.plainRender();
        XX_TEST_EXPECT_TRUE(after.find("+ [Tool] Read") != std::string::npos);
        XX_TEST_EXPECT_TRUE(after.find("agentxx_filesystem_read") == std::string::npos);
    }
}

// 插件工具消息装饰渲染 (planning 插件经 update_tool_decor 推送装饰):
// 1. 折叠头显示装饰 displayName ("Plan") + 装饰 summary (todos 一行格式化)
// 2. 展开头显示装饰 displayName; 展开体按 items 通用渲染 (状态图/todos/notes)
void testTuiToolHeaderDecor() {
    // 折叠测试
    ToolHeaderFixture f(120, 16);
    f.pushTool(
        "agentxx_planning",
        R"({"mode":"write","roadmap":"stateDiagram-v2\n[*] --> s1\ns1 --> [*]","todos":[{"state":"in_progress","content":"reproduce issue","summary":"trying"},{"state":"pending","content":"fix root cause"},{"state":"completed","content":"write tests"}],"notes":"memo 123"})",
        true,
        true // 折叠
    );
    f.pushDecor();
    std::string collapsed = f.render();
    XX_TEST_EXPECT_TRUE(collapsed.find("+ [Tool] ") != std::string::npos);
    XX_TEST_EXPECT_TRUE(collapsed.find("Plan · ") != std::string::npos);
    XX_TEST_EXPECT_TRUE(
        collapsed.find("[~] reproduce issue; [ ] fix root cause; [#] write tests")
        != std::string::npos
    );

    // 展开测试 (展开头显示装饰名; 体为 items 通用渲染)
    ToolHeaderFixture f2(120, 24);
    f2.pushTool(
        "agentxx_planning",
        R"({"mode":"write","roadmap":"stateDiagram-v2\n[*] --> phase1\nphase1 --> [*]","todos":[{"state":"in_progress","content":"do task A","summary":"working on A"},{"state":"completed","content":"done task B"}],"notes":"my notes content"})",
        true,
        false // 展开
    );
    f2.pushDecor();
    std::string expanded = f2.render();
    XX_TEST_EXPECT_TRUE(expanded.find("- [Tool] Plan") != std::string::npos);
    XX_TEST_EXPECT_TRUE(
        expanded.find("agentxx_planning") == std::string::npos
    ); ///< 无原始工具名特化
    // 状态图以 inline diagram 回退渲染 (夹具 items 仅含 diagram 无 button;
    // 插件真实输出为可点击 Graph 按钮 + 兼容 diagram 双份)
    XX_TEST_EXPECT_TRUE(expanded.find("Graph") == std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("Todos:") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("[~] do task A") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("working on A") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("[#] done task B") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("Notes:") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("my notes content") != std::string::npos);

    // 装饰失败回退: 失败标记 (hasError) 到达时展开体回退通用 result 错误展示
    ToolHeaderFixture f3(120, 16);
    f3.pushFailedTool(
        "agentxx_planning",
        R"({"mode":"read"})",
        "[Error] No saved planning in this session.",
        false
    );
    f3.pushDecor();
    std::string errBody = f3.plainRender();
    XX_TEST_EXPECT_TRUE(
        errBody.find("[Error] No saved planning in this session.") != std::string::npos
    );
}

// 含可点击 decor 按钮的消息展开与多帧渲染回归测试:
// 1. 初始折叠, decorHitBoxes 为空
// 2. 展开后首帧渲染出按钮, 且 decorHitBoxes 记录按钮命中区
// 3. 展开后次帧再次渲染 (未修改状态):
//    - 修复前在 ASAN 下因缓存复用访问已释放 Box 产生 heap-use-after-free 崩溃
//    - 修复后不崩溃, decorHitBoxes 持续有效, box 指针非空
// 4. 再次折叠后 decorHitBoxes 清空
void testTuiToolHeaderDecorButtonMultiFrame() {
    ToolHeaderFixture f(120, 24);
    f.pushTool(
        "agentxx_planning",
        R"({"mode":"write","roadmap":"stateDiagram-v2\n[*] --> s1\ns1 --> [*]","todos":[{"state":"in_progress","content":"reproduce issue"}]})",
        true,
        true // 初始折叠
    );
    f.pushDecorWithButton();

    // 帧 1: 折叠渲染
    std::string collapsed = f.render();
    XX_TEST_EXPECT_TRUE(collapsed.find("+ [Tool] ") != std::string::npos);
    XX_TEST_EXPECT_TRUE(f.comp->decorHitBoxes().empty());

    // 点击展开消息 (模拟用户点击展开)
    f.sharedState.mutate([&](TUIRenderState& st) {
        st.messages[0]->collapsed = false;
    });

    // 帧 2: 展开渲染, 此时生成 decorHits 按钮
    std::string expanded1 = f.render();
    XX_TEST_EXPECT_TRUE(expanded1.find("- [Tool] Plan") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded1.find("Graph") != std::string::npos);
    XX_TEST_EXPECT_EQ(f.comp->decorHitBoxes().size(), size_t{1});
    XX_TEST_EXPECT_EQ(f.comp->decorHitBoxes()[0].actionId, "planning.open_graph");

    // 帧 3: 保持展开状态再次渲染 (未修改状态) —— 回归防 UAF 与命中持续有效
    std::string expanded2 = f.render();
    XX_TEST_EXPECT_TRUE(expanded2.find("- [Tool] Plan") != std::string::npos);
    XX_TEST_EXPECT_EQ(f.comp->decorHitBoxes().size(), size_t{1});
    XX_TEST_EXPECT_EQ(f.comp->decorHitBoxes()[0].actionId, "planning.open_graph");
    XX_TEST_EXPECT_TRUE(f.comp->decorHitBoxes()[0].box != nullptr);

    // 帧 4: 再次折叠
    f.sharedState.mutate([&](TUIRenderState& st) {
        st.messages[0]->collapsed = true;
    });
    std::string collapsed2 = f.render();
    XX_TEST_EXPECT_TRUE(collapsed2.find("+ [Tool] ") != std::string::npos);
    XX_TEST_EXPECT_TRUE(f.comp->decorHitBoxes().empty());
}

// 执行失败工具折叠与展开渲染:
// 1. 折叠时保持特化 toolName (如 "Read", "Write", "Bash" 等), 异常结果显示在后面并标红
// 2. 未知工具折叠时显示原始 toolName + 异常结果并标红
// 3. 展开时 result 显示为红色
// 失败一律由执行层给出的 hasError 标记驱动 (界面不看结果文本前缀, 反方向用例见函数末尾)
void testTuiToolHeaderFailed() {
    // 已知工具 (Read) 执行失败折叠展示: 保持 "Read", 后面紧随异常结果
    ToolHeaderFixture fRead;
    fRead.pushFailedTool(
        "agentxx_filesystem_read",
        R"({"path":"/nonexistent/file.txt"})",
        "[Error] Path not exist"
    );
    XX_TEST_EXPECT_TRUE(
        fRead.plainRender().find("Read · [Error] Path not exist") != std::string::npos
    );
    // 样式断言: 异常结果包含 errorColor 颜色代码 (255;85;85)
    XX_TEST_EXPECT_TRUE(fRead.render().find("255;85;85") != std::string::npos);

    // 已知工具 (Edit) 执行失败折叠展示
    ToolHeaderFixture fEdit;
    fEdit.pushFailedTool(
        "agentxx_filesystem_edit",
        R"({"path":"/a.cpp","old_str":"","new_str":"b"})",
        "[Error] Arg old_str is empty"
    );
    XX_TEST_EXPECT_TRUE(
        fEdit.plainRender().find("Edit · [Error] Arg old_str is empty") != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(fEdit.render().find("255;85;85") != std::string::npos);

    // 已知工具 (Bash) 执行失败折叠展示
    ToolHeaderFixture fBash;
    fBash.pushFailedTool(
        "agentxx_execute_bash_command",
        R"({"command":"invalid_cmd"})",
        "[Error] Command failed with code 127"
    );
    XX_TEST_EXPECT_TRUE(
        fBash.plainRender().find("Bash · [Error] Command failed with code 127") != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(fBash.render().find("255;85;85") != std::string::npos);

    // 异常中止 ([Exception aborted: ...])
    ToolHeaderFixture fExcept;
    fExcept.pushFailedTool(
        "agentxx_web_fetch",
        R"({"url":"https://example.com"})",
        "[Exception aborted: connection timeout]"
    );
    XX_TEST_EXPECT_TRUE(
        fExcept.plainRender().find("Fetch · [Exception aborted: connection timeout]")
        != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(fExcept.render().find("255;85;85") != std::string::npos);

    // 未知工具执行失败折叠展示: 保持原始 toolName + 异常结果
    ToolHeaderFixture fUnknown;
    fUnknown.pushFailedTool(
        "custom_plugin_tool",
        R"({"foo":"bar"})",
        "[Error] Custom failure"
    );
    XX_TEST_EXPECT_TRUE(
        fUnknown.plainRender().find("custom_plugin_tool · [Error] Custom failure")
        != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(fUnknown.render().find("255;85;85") != std::string::npos);

    // 展开状态下普通工具的 result: 同样使用 errorColor 标红
    ToolHeaderFixture fExpanded;
    fExpanded.pushFailedTool(
        "agentxx_execute_bash_command",
        R"({"command":"ls"})",
        "[Error] exit code 1",
        false // 展开
    );
    XX_TEST_EXPECT_TRUE(
        fExpanded.plainRender().find(TuiI18n::instance().t("tool.result")) != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(fExpanded.plainRender().find("[Error] exit code 1") != std::string::npos);
    XX_TEST_EXPECT_TRUE(fExpanded.render().find("255;85;85") != std::string::npos);

    // 反向用例: 同样的结果文本, 执行层未标失败 (hasError = false)
    // —— 界面不按文本判错: 不加 " · " 失败后缀, 也不用错误色
    ToolHeaderFixture fTextOnly;
    fTextOnly.pushTool(
        "custom_plugin_tool",
        R"({"foo":"bar"})",
        true,
        true,
        "[Error] this line is log content, not a failure"
    );
    XX_TEST_EXPECT_TRUE(
        fTextOnly.plainRender().find("custom_plugin_tool [Error] this line is log content")
        != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(
        fTextOnly.plainRender().find("custom_plugin_tool · [Error]") == std::string::npos
    );
    XX_TEST_EXPECT_TRUE(fTextOnly.render().find("255;85;85") == std::string::npos);
}

// 展开 Tool 消息时显示耗时
void testTuiToolHeaderDuration() {
    // 1. 普通工具展开, 耗时 1.2s
    ToolHeaderFixture f1;
    f1.pushTool(
        "agentxx_filesystem_read",
        R"({"path":"/home/a.cpp"})",
        true,
        false, // 展开
        "file content",
        1200
    );
    XX_TEST_EXPECT_TRUE(
        f1.plainRender().find("- [Tool] agentxx_filesystem_read 1.2s") != std::string::npos
    );

    // 1.1 短耗时 (< 100ms) 工具展开, 耗时显示为毫秒单位 (50ms)
    ToolHeaderFixture f1ms;
    f1ms.pushTool(
        "agentxx_filesystem_read",
        R"({"path":"/home/a.cpp"})",
        true,
        false, // 展开
        "file content",
        50
    );
    XX_TEST_EXPECT_TRUE(
        f1ms.plainRender().find("- [Tool] agentxx_filesystem_read 50ms") != std::string::npos
    );

    // 2. 长耗时工具展开, 耗时 1m5s
    ToolHeaderFixture f2;
    f2.pushTool(
        "agentxx_execute_bash_command",
        R"({"command":"make"})",
        true,
        false, // 展开
        "build ok",
        65000
    );
    XX_TEST_EXPECT_TRUE(
        f2.plainRender().find("- [Tool] agentxx_execute_bash_command 1m5s") != std::string::npos
    );

    // 3. 插件装饰工具展开, 耗时 0.3s
    ToolHeaderFixture f3(120, 24);
    f3.pushTool(
        "agentxx_planning",
        R"({"mode":"read"})",
        true,
        false, // 展开
        "ok",
        300
    );
    f3.pushDecor();
    XX_TEST_EXPECT_TRUE(f3.plainRender().find("- [Tool] Plan 0.3s") != std::string::npos);

    // 4. filesystem_edit 特化工具展开, 耗时 0.5s
    ToolHeaderFixture f4;
    f4.pushTool(
        "agentxx_filesystem_edit",
        R"({"path":"/a.cpp","old_str":"foo","new_str":"bar"})",
        true,
        false, // 展开
        "success",
        450
    );
    XX_TEST_EXPECT_TRUE(
        f4.plainRender().find("- [Tool] agentxx_filesystem_edit 0.5s") != std::string::npos
    );

    // 5. 运行中的工具展开, 无耗时后缀
    ToolHeaderFixture f5;
    f5.pushTool(
        "agentxx_filesystem_read",
        R"({"path":"/home/a.cpp"})",
        false,
        false, // 展开
        "",
        0
    );
    XX_TEST_EXPECT_TRUE(
        f5.plainRender().find("[Tool] agentxx_filesystem_read") != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(f5.plainRender().find("0.0s") == std::string::npos);

    // 6. 已完成但耗时为 0 的工具展开, 不显示耗时 (无 0.0s)
    ToolHeaderFixture f6;
    f6.pushTool(
        "agentxx_filesystem_read",
        R"({"path":"/home/a.cpp"})",
        true,
        false, // 展开
        "content",
        0
    );
    XX_TEST_EXPECT_TRUE(
        f6.plainRender().find("- [Tool] agentxx_filesystem_read") != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(f6.plainRender().find("0.0s") == std::string::npos);

    // 7. Think 消息: 耗时 > 0 时显示耗时, 耗时为 0 时不显示
    ToolHeaderFixture f7;
    f7.pushThinking("thinking content 1", false, 1500); // 展开, 1.5s
    XX_TEST_EXPECT_TRUE(f7.plainRender().find("- [Think] 1.5s") != std::string::npos);

    ToolHeaderFixture f8;
    f8.pushThinking("thinking content 2", false, 0); // 展开, 耗时 0
    XX_TEST_EXPECT_TRUE(f8.plainRender().find("- [Think] ") != std::string::npos);
    XX_TEST_EXPECT_TRUE(f8.plainRender().find("0.0s") == std::string::npos);

    ToolHeaderFixture f9;
    f9.pushThinking("thinking content 3", true, 0); // 折叠, 耗时 0
    XX_TEST_EXPECT_TRUE(f9.plainRender().find("+ [Think] ") != std::string::npos);
    XX_TEST_EXPECT_TRUE(f9.plainRender().find("0.0s") == std::string::npos);
}

// 主程序内置工具 (lib 内置实现: agentxx_share_store / agentxx_subagent) 头部特化渲染:
// 显示名 + 一行摘要 (插件渲染器之外的"主程序内置渲染器"路径, 见
// agentxx/plugin/builtin_tool_renderers.h)。展开体不受影响 (仍为参数/结果)。
void testTuiToolHeaderBuiltin() {
    ToolHeaderFixture f;

    // share_store new: 操作 + 新增 id (取自结果 JSON; 区间只有 get 才有)
    f.pushTool(
        "agentxx_share_store",
        R"({"opt":"new","text":"line1\nline2\nline3"})",
        true,
        true,
        R"({"id":7})"
    );
    XX_TEST_EXPECT_TRUE(f.render().find("Store · new #7") != std::string::npos);

    // share_store get: 行区间 + 操作 + id
    f.pushTool(
        "agentxx_share_store",
        R"({"opt":"get","id":12,"offset":0,"limit":100})",
        true,
        true,
        "content"
    );
    XX_TEST_EXPECT_TRUE(f.render().find("Store · [0, 100] get #12") != std::string::npos);

    // share_store update
    f.pushTool(
        "agentxx_share_store",
        R"({"opt":"update","id":3,"text":"x\ny"})",
        true,
        true,
        "success"
    );
    XX_TEST_EXPECT_TRUE(f.render().find("Store · update #3") != std::string::npos);
    // 已移除的 `delete` 与其它未识别的 opt 一样原样展示 (不带 id/行区间摘要)
    f.pushTool("agentxx_share_store", R"({"opt":"delete","id":3})", true, true, "success");
    XX_TEST_EXPECT_TRUE(f.render().find("Store · delete") != std::string::npos);

    // subagent 单发: 子代理名 + 任务首行
    f.pushTool(
        "agentxx_subagent",
        R"({"subagent":"explorer","message":"fix login bug\nmore details"})",
        true,
        true,
        "done"
    );
    XX_TEST_EXPECT_TRUE(
        f.render().find("Subagent · explorer · fix login bug") != std::string::npos
    );

    // subagent 批量: 任务数 + 子代理名
    f.pushTool(
        "agentxx_subagent",
        R"({"tasks":[{"subagent":"explorer","message":"a"},{"subagent":"coder","message":"b"}]})",
        true,
        true,
        "ok"
    );
    XX_TEST_EXPECT_TRUE(
        f.render().find("Subagent · 2 tasks: explorer, coder") != std::string::npos
    );

    // 运行中 (未完成) 的 share_store: 无新增 id 后缀, 且显示名保持运行态高亮
    ToolHeaderFixture g;
    g.pushTool("agentxx_share_store", R"({"opt":"new","text":"a\nb"})", false);
    XX_TEST_EXPECT_TRUE(g.plainRender().find("Store · new") != std::string::npos);
    XX_TEST_EXPECT_TRUE(g.plainRender().find('#') == std::string::npos);
    XX_TEST_EXPECT_TRUE(g.render().find("102;204;255") != std::string::npos);
}

// 重启恢复会话后的历史 Tool 消息特化渲染 (回归):
// 历史消息由服务端 Sync 回放, 没有运行时装饰 (update_tool_decor 只覆盖本进程
// 实时调用, 且装饰随会话切换清理), 特化渲染必须由按 tool_name 注册的类型级
// 渲染器从消息自带的参数/结果推导 —— 修复前这类消息走通用渲染 (折叠头显示
// 原始工具名 "agentxx_planning" + 参数/结果原文)
void testTuiToolHeaderPlanningRestored() {
    const std::string readResult
        = R"({"roadmap":"stateDiagram-v2\n[*] --> h2\nh2 --> [*]","todos":[{"state":"completed","content":"hist read task"}]})";

    // 折叠展示: 渲染器提供显示名 "Plan" + todos 摘要 (无任何装饰注入)
    ToolHeaderFixture f(120, 16);
    f.pushTool(
        "agentxx_planning",
        R"({"mode":"write","roadmap":"stateDiagram-v2\n[*] --> h1\nh1 --> [*]","todos":[{"state":"in_progress","content":"hist task"}],"notes":"hist note"})",
        true,
        true,
        "success"
    );
    std::string collapsed = f.plainRender();
    XX_TEST_EXPECT_TRUE(collapsed.find("agentxx_planning") == std::string::npos);
    XX_TEST_EXPECT_TRUE(collapsed.find("Plan · [~] hist task") != std::string::npos);

    // 展开展示: 展开体按 items 通用渲染 (状态图/todos/notes)
    ToolHeaderFixture f2(120, 24);
    f2.pushTool(
        "agentxx_planning",
        R"({"mode":"write","roadmap":"stateDiagram-v2\n[*] --> h1\nh1 --> [*]","todos":[{"state":"in_progress","content":"hist task"}],"notes":"hist note"})",
        true,
        false,
        "success"
    );
    std::string expanded = f2.plainRender();
    XX_TEST_EXPECT_TRUE(expanded.find("|- Todo") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("[~] hist task") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("hist note") != std::string::npos);

    // read 模式历史消息: 内容取自工具结果 (保存的规划 JSON)
    ToolHeaderFixture f3(120, 16);
    f3.pushTool("agentxx_planning", R"({"mode":"read"})", true, true, readResult);
    XX_TEST_EXPECT_TRUE(f3.plainRender().find("Plan · [#] hist read task") != std::string::npos);
}

// filesystem edit 折叠头摘要: 类似 git 的 "+行 -行" 提示 (`[+3 -1] path`)
//
// 行数统计方式与展开体 diff 同源 (utilxx::computeLineDiff 的逐行 LCS 结果),
// 不是 old_str/new_str 的行数差 —— 替换块里未变的上下文行不计入增删。
void testTuiToolHeaderEditDiffStat() {
    // 单行替换: 1 增 1 删
    {
        ToolHeaderFixture f(120, 16);
        f.pushTool(
            "agentxx_filesystem_edit",
            R"({"path":"/home/a.cpp","old_str":"foo","new_str":"bar"})"
        );
        XX_TEST_EXPECT_TRUE(f.render().find("Edit · [+1 -1] /home/a.cpp") != std::string::npos);
    }

    // 多行替换含未变上下文行: 上下文 (return/}) 不计入, 仍为 1 增 1 删
    {
        ToolHeaderFixture f(120, 16);
        f.pushTool(
            "agentxx_filesystem_edit",
            R"({"path":"/home/b.cpp","old_str":"int a = 1;\nreturn a;\n}","new_str":"int a = 2;\nreturn a;\n}"})"
        );
        XX_TEST_EXPECT_TRUE(f.render().find("Edit · [+1 -1] /home/b.cpp") != std::string::npos);
    }

    // 纯插入 / 纯删除: 无增删的一侧显示 0
    {
        ToolHeaderFixture f(120, 16);
        f.pushTool(
            "agentxx_filesystem_edit",
            R"({"path":"/home/c.cpp","old_str":"a","new_str":"a\nb"})"
        );
        XX_TEST_EXPECT_TRUE(f.render().find("Edit · [+1 -0] /home/c.cpp") != std::string::npos);
    }
    {
        ToolHeaderFixture f(120, 16);
        f.pushTool(
            "agentxx_filesystem_edit",
            R"({"path":"/home/d.cpp","old_str":"a\nb","new_str":"a"})"
        );
        XX_TEST_EXPECT_TRUE(f.render().find("Edit · [+0 -1] /home/d.cpp") != std::string::npos);
    }

    // 两行都替换: 2 增 2 删
    {
        ToolHeaderFixture f(120, 16);
        f.pushTool(
            "agentxx_filesystem_edit",
            R"({"path":"/home/e.cpp","old_str":"int x = 1;\nint y = 2;","new_str":"int x = 3;\nint y = 4;"})"
        );
        XX_TEST_EXPECT_TRUE(f.render().find("Edit · [+2 -2] /home/e.cpp") != std::string::npos);
    }

    // CRLF 形态参数: 行尾差异不当作整段重写 (与 edit 执行体同为 LF 归一化后匹配)
    {
        ToolHeaderFixture f(120, 16);
        f.pushTool(
            "agentxx_filesystem_edit",
            R"({"path":"/home/f.cpp","old_str":"int a;\r\nint b;","new_str":"int a;\r\nint c;"})"
        );
        XX_TEST_EXPECT_TRUE(f.render().find("Edit · [+1 -1] /home/f.cpp") != std::string::npos);
    }

    // 多段替换 (edits 数组): 各段增删行数求和
    {
        ToolHeaderFixture f(120, 16);
        f.pushTool(
            "agentxx_filesystem_edit",
            R"({"path":"/home/o.cpp","edits":[{"old_str":"oldA();","new_str":"newA();"},{"old_str":"x = 1;\ny = 2;","new_str":"x = 3;\ny = 2;"}]})",
            true,
            true,
            "success"
        );
        // 第 1 段 +1 -1; 第 2 段逐行 diff 只统计变化行 +1 -1 → 合计 [+2 -2]
        XX_TEST_EXPECT_TRUE(f.render().find("Edit · [+2 -2] /home/o.cpp") != std::string::npos);
    }

    // 多段替换运行中 (结果未到): 每段按一处展示
    {
        ToolHeaderFixture f(120, 16);
        f.pushTool(
            "agentxx_filesystem_edit",
            R"({"path":"/home/q.cpp","edits":[{"old_str":"a","new_str":"A"},{"old_str":"b","new_str":"B"}]})",
            false // 运行中
        );
        XX_TEST_EXPECT_TRUE(
            f.plainRender().find("Edit · [+2 -2] /home/q.cpp") != std::string::npos
        );
    }

    // 参数缺失 old_str/new_str: 无中括号提示, 仅显示路径 (保持旧观感)
    {
        ToolHeaderFixture f(120, 16);
        f.pushTool("agentxx_filesystem_edit", R"({"path":"/home/h.cpp"})");
        XX_TEST_EXPECT_TRUE(f.render().find("Edit · /home/h.cpp") != std::string::npos);
        XX_TEST_EXPECT_TRUE(f.render().find("Edit · [+0 -0]") == std::string::npos);
    }

    // 运行中 (参数已就绪, 尚未完成): 同样给出增删行数提示
    // (运行态显示名与摘要样式不同, 原文断言需剥离颜色转义, 见 plainRender)
    {
        ToolHeaderFixture f(120, 16);
        f.pushTool(
            "agentxx_filesystem_edit",
            R"({"path":"/home/i.cpp","old_str":"foo","new_str":"bar"})",
            false // 运行中
        );
        XX_TEST_EXPECT_TRUE(
            f.plainRender().find("Edit · [+1 -1] /home/i.cpp") != std::string::npos
        );
    }

    // 展开态: 头部保持原始工具名, 提示不进入展开头 (信息由展开体 diff 呈现)
    {
        ToolHeaderFixture f(120, 16);
        f.pushTool(
            "agentxx_filesystem_edit",
            R"({"path":"/home/j.cpp","old_str":"foo","new_str":"bar"})",
            true,
            false // 展开
        );
        std::string out = f.render();
        XX_TEST_EXPECT_TRUE(out.find("- [Tool] agentxx_filesystem_edit") != std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("[+1 -1]") == std::string::npos);
    }
}

TestResult testTuiToolHeader() {
    // 消息列表头部角色标签 ([Tool]/[Think] 等) 随界面语言切换 (见 TuiI18n):
    // 本模块断言英文标签, 固定界面语言为英文, 避免跟随系统语言
    auto&      tuiSettings = TUISettings::instance();
    const auto savedLang   = tuiSettings.language();
    tuiSettings.setLanguage(TuiLanguage::EnUs);

    testTuiToolHeaderFilesystem();
    testTuiToolHeaderEditDiffStat();
    testTuiToolHeaderWeb();
    testTuiToolHeaderFallback();
    testTuiToolHeaderOverflow();
    testTuiToolHeaderRunning();
    testTuiToolHeaderPendingRender();
    testTuiToolHeaderDecor();
    testTuiToolHeaderDecorExtended();
    testTuiToolHeaderEditMultiSegmentBody();
    testTuiToolHeaderDecorRegionSize();
    testTuiToolHeaderDecorButtonMultiFrame();
    testTuiToolHeaderFailed();
    testTuiToolHeaderDuration();
    testTuiToolHeaderBuiltin();
    testTuiToolHeaderPlanningRestored();

    // 恢复原始界面语言
    tuiSettings.setLanguage(savedLang);

    return {g_tui_tool_header_passed, g_tui_tool_header_failed};
}

} // namespace test
} // namespace agentxx
