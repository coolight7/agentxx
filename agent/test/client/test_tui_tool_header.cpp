// TUI 特化渲染: 工具调用头部摘要 (buildToolHeaderSummary)
//
// 覆盖已知工具头部渲染为 "动词 · 参数摘要" 的场景, 以及未知工具/参数
// 解析失败回退显示原始 toolName 的降级路径:
// - filesystem 系列: list / read_text_file (含 [offset, limit] 区间) /
//   write_file / edit_text_file / glob / grep
// - web_search 系列: web_search / web_fetch_url / web_fetch_url_markdown
// - 降级: 未知工具名 / 非 JSON 参数 -> 头部仍显示原始 toolName
#include "test_tui_tool_header.h"

#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "asio/io_context.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "test_framework.h"
#include <memory>
#include <string>
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

namespace {

static AgentxxPluginString makeTestString(std::string_view s) {
    if (s.empty()) {
        return AgentxxPluginString{nullptr, 0};
    }
    char* buf = static_cast<char*>(std::malloc(s.size() + 1));
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    return AgentxxPluginString{buf, static_cast<uint64_t>(s.size())};
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
    static auto readFn = [](void*, const AgentxxToolRenderInput* in, AgentxxToolRenderOutput* out) -> int32_t {
        std::string_view args(in->args_json.data ? in->args_json.data : "", in->args_json.size);
        neograph::json j;
        try { j = neograph::json::parse(args); } catch (...) { return -1; }
        if (!j.is_object()) return -1;
        std::string path = j.value("path", std::string{});
        int64_t off = j.value("line_offset", int64_t{-1});
        int64_t lim = j.value("line_limit", int64_t{-1});
        std::string range;
        if (off <= 0 && lim <= 0) {
            // empty
        } else if (off <= 0) {
            range = fmt::format("0, {}", lim);
        } else if (lim <= 0) {
            range = fmt::format("{}", off);
        } else {
            range = fmt::format("{}, {}", off, lim);
        }
        std::string summary = " ·";
        if (!range.empty()) summary += " [" + range + "]";
        if (!path.empty()) summary += " " + path;
        out->displayName = makeTestString("Read");
        out->summary     = makeTestString(summary);
        return 0;
    };
    reg->toolRenderers.push_back({
        .plugin   = "agentxx_filesystem",
        .toolName = "agentxx_filesystem_read",
        .renderFn = readFn,
    });

    // Glob (回调)
    static auto globFn = [](void*, const AgentxxToolRenderInput* in, AgentxxToolRenderOutput* out) -> int32_t {
        std::string_view args(in->args_json.data ? in->args_json.data : "", in->args_json.size);
        neograph::json j;
        try { j = neograph::json::parse(args); } catch (...) { return -1; }
        if (!j.is_object()) return -1;
        auto files = j.value("file_patterns", std::vector<std::string>{});
        std::string joined;
        const size_t n = std::min<size_t>(files.size(), 2);
        for (size_t i = 0; i < n; ++i) {
            if (i > 0) joined += ", ";
            joined += files[i];
        }
        if (files.size() > 2) joined += (n > 0 ? ", ..." : "...");
        out->displayName = makeTestString("Glob");
        out->summary     = makeTestString(" · " + joined);
        return 0;
    };
    reg->toolRenderers.push_back({
        .plugin   = "agentxx_filesystem",
        .toolName = "agentxx_filesystem_glob",
        .renderFn = globFn,
    });

    // Grep (回调)
    static auto grepFn = [](void*, const AgentxxToolRenderInput* in, AgentxxToolRenderOutput* out) -> int32_t {
        std::string_view args(in->args_json.data ? in->args_json.data : "", in->args_json.size);
        neograph::json j;
        try { j = neograph::json::parse(args); } catch (...) { return -1; }
        if (!j.is_object()) return -1;
        auto textPats  = j.value("text_patterns", std::vector<std::string>{});
        auto regexPats = j.value("regex_patterns", std::vector<std::string>{});
        auto files     = j.value("file_patterns", std::vector<std::string>{});
        std::vector<std::string> shown;
        for (const auto& p : textPats) { if (shown.size() >= 2) break; shown.push_back(p); }
        for (const auto& p : regexPats) { if (shown.size() >= 2) break; shown.push_back(p); }
        const size_t total = textPats.size() + regexPats.size();
        std::string quoted;
        for (size_t i = 0; i < shown.size(); ++i) {
            if (i > 0) quoted += ", ";
            quoted += '"' + shown[i] + '"';
        }
        if (total > shown.size()) quoted += ", ...";
        std::string joinedFiles;
        const size_t n = std::min<size_t>(files.size(), 2);
        for (size_t i = 0; i < n; ++i) {
            if (i > 0) joinedFiles += ", ";
            joinedFiles += files[i];
        }
        if (files.size() > 2) joinedFiles += (n > 0 ? ", ..." : "...");
        std::string summary = " ·";
        if (!quoted.empty()) summary += " [" + quoted + "]";
        if (!joinedFiles.empty()) summary += " " + joinedFiles;
        out->displayName = makeTestString("Grep");
        out->summary     = makeTestString(summary);
        return 0;
    };
    reg->toolRenderers.push_back({
        .plugin   = "agentxx_filesystem",
        .toolName = "agentxx_filesystem_grep",
        .renderFn = grepFn,
    });

    // Edit (回调)
    static auto editFn = [](void*, const AgentxxToolRenderInput* in, AgentxxToolRenderOutput* out) -> int32_t {
        std::string_view args(in->args_json.data ? in->args_json.data : "", in->args_json.size);
        neograph::json j;
        try { j = neograph::json::parse(args); } catch (...) { return -1; }
        if (!j.is_object()) return -1;
        std::string path   = j.value("path", std::string{});
        std::string oldStr = j.value("old_str", std::string{});
        std::string newStr = j.value("new_str", std::string{});
        out->displayName = makeTestString("Edit");
        out->summary     = makeTestString(" · " + path);
        if (!in->is_error) {
            neograph::json diffItem;
            diffItem["kind"]    = "diff";
            diffItem["path"]    = std::move(path);
            diffItem["old_str"] = std::move(oldStr);
            diffItem["new_str"] = std::move(newStr);
            neograph::json arr  = neograph::json::array();
            arr.push_back(std::move(diffItem));
            out->items_json = makeTestString(arr.dump());
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
        // 导致断言随环境漂移 —— 测试内强制声明 TrueColor 支持
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
    void pushTool(
        std::string name,
        std::string args,
        bool        finished   = true,
        bool        collapsed  = true,
        std::string result     = "",
        int64_t     durationMs = 0
    ) {
        sharedState.mutate([&](TUIRenderState& st) {
            auto m                = std::make_shared<TUIMessage>();
            m->role               = TUIMessage::Role::Tool;
            m->tool               = TUIMessage::ToolData{};
            m->tool->toolName     = std::move(name);
            m->tool->toolCallId   = "call_1";
            m->tool->toolFinished = finished;
            m->tool->toolResult   = std::move(result);
            m->durationMs         = durationMs;
            // Tool 消息默认折叠展示 (与真实 TUI 流一致, 见 agent_tui.cpp);
            // 折叠态头部才显示 "动词 · 参数摘要" 特化渲染
            m->collapsed = collapsed;
            m->text      = std::move(args);
            st.messages.push_back(std::move(m));
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
            auto  reg         = st.pluginRegistry ? std::make_shared<agentxx::plugin::ClientUiRegistry>(*st.pluginRegistry)
                                                  : std::make_shared<agentxx::plugin::ClientUiRegistry>();
            auto& d           = reg->toolDecors.emplace_back();
            d.plugin          = "agentxx_planning";
            d.toolCallId      = "call_1";
            d.displayName     = "Plan";
            d.summary         = "[~] reproduce issue; [ ] fix root cause; [#] write tests";
            d.items           = neograph::json::parse(R"([
                {"kind":"text","role":"title","text":"State Diagram:"},
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
    f.pushTool(
        "agentxx_filesystem_read",
        R"({"path":"/home/a.cpp","line_offset":0,"line_limit":100})"
    );
    XX_TEST_EXPECT_TRUE(f.render().find("Read · [0, 100] /home/a.cpp") != std::string::npos);

    // read_text_file: 无区间 (默认 -1) 不显示中括号
    f.pushTool("agentxx_filesystem_read", R"({"path":"/home/b.cpp"})");
    XX_TEST_EXPECT_TRUE(f.render().find("Read · /home/b.cpp") != std::string::npos);

    // read_text_file: 仅 limit
    f.pushTool("agentxx_filesystem_read", R"({"path":"/home/c.cpp","line_limit":50})");
    XX_TEST_EXPECT_TRUE(f.render().find("Read · [0, 50] /home/c.cpp") != std::string::npos);

    // read_text_file: 仅 offset
    f.pushTool("agentxx_filesystem_read", R"({"path":"/home/d.cpp","line_offset":10})");
    XX_TEST_EXPECT_TRUE(f.render().find("Read · [10] /home/d.cpp") != std::string::npos);

    // list
    f.pushTool("agentxx_filesystem_list", R"({"path":"/home"})");
    XX_TEST_EXPECT_TRUE(f.render().find("List · /home") != std::string::npos);

    // write_file
    f.pushTool("agentxx_filesystem_write", R"({"path":"/home/out.txt","overwrite":true})");
    XX_TEST_EXPECT_TRUE(f.render().find("Write · /home/out.txt") != std::string::npos);

    // edit_text_file: 头部摘要含路径 (diff 正文不受影响)
    f.pushTool("agentxx_filesystem_edit", R"({"path":"/home/e.cpp","old_str":"a","new_str":"b"})");
    XX_TEST_EXPECT_TRUE(f.render().find("Edit · /home/e.cpp") != std::string::npos);

    // glob: 单模式 / 多模式折叠为前两项 + "..."
    f.pushTool("agentxx_filesystem_glob", R"({"file_patterns":["agent/lib/**/*.cpp"]})");
    XX_TEST_EXPECT_TRUE(f.render().find("Glob · agent/lib/**/*.cpp") != std::string::npos);
    f.pushTool("agentxx_filesystem_glob", R"({"file_patterns":["a","b","c","d"]})");
    XX_TEST_EXPECT_TRUE(f.render().find("Glob · a, b, ...") != std::string::npos);

    // grep: 引号包裹的匹配模式 (参数区, 中括号) + 文件模式 (主参数)
    f.pushTool(
        "agentxx_filesystem_grep",
        R"({"text_patterns":["foo"],"file_patterns":["src/**/*.h"]})"
    );
    XX_TEST_EXPECT_TRUE(f.render().find(R"(Grep · ["foo"] src/**/*.h)") != std::string::npos);
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
        R"({"path":"/very/very/long/path/that/definitely/exceeds/the/narrow/viewport/width/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.cpp","line_offset":0,"line_limit":100})"
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
        R"({"path":"/home/running.cpp","line_offset":0,"line_limit":100})",
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
    XX_TEST_EXPECT_TRUE(expanded.find("State Diagram:") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("Todos:") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("[~] do task A") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("working on A") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("[#] done task B") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("Notes:") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("my notes content") != std::string::npos);

    // 装饰失败回退: 结果错误时展开体回退通用 result 错误展示
    ToolHeaderFixture f3(120, 16);
    f3.pushTool(
        "agentxx_planning",
        R"({"mode":"read"})",
        true,
        false,
        "[Error] No saved planning in this session."
    );
    f3.pushDecor();
    std::string errBody = f3.plainRender();
    XX_TEST_EXPECT_TRUE(
        errBody.find("[Error] No saved planning in this session.") != std::string::npos
    );
}

// 执行失败工具折叠与展开渲染:
// 1. 折叠时保持特化 toolName (如 "Read", "Write", "Bash" 等), 异常结果显示在后面并标红
// 2. 未知工具折叠时显示原始 toolName + 异常结果并标红
// 3. 展开时 result 显示为红色
void testTuiToolHeaderFailed() {
    // 已知工具 (Read) 执行失败折叠展示: 保持 "Read", 后面紧随异常结果
    ToolHeaderFixture fRead;
    fRead.pushTool(
        "agentxx_filesystem_read",
        R"({"path":"/nonexistent/file.txt"})",
        true,
        true,
        "[Error] Path not exist"
    );
    XX_TEST_EXPECT_TRUE(
        fRead.plainRender().find("Read · [Error] Path not exist") != std::string::npos
    );
    // 样式断言: 异常结果包含 errorColor 颜色代码 (255;85;85)
    XX_TEST_EXPECT_TRUE(fRead.render().find("255;85;85") != std::string::npos);

    // 已知工具 (Edit) 执行失败折叠展示
    ToolHeaderFixture fEdit;
    fEdit.pushTool(
        "agentxx_filesystem_edit",
        R"({"path":"/a.cpp","old_str":"","new_str":"b"})",
        true,
        true,
        "[Error] Arg old_str is empty"
    );
    XX_TEST_EXPECT_TRUE(
        fEdit.plainRender().find("Edit · [Error] Arg old_str is empty") != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(fEdit.render().find("255;85;85") != std::string::npos);

    // 已知工具 (Bash) 执行失败折叠展示
    ToolHeaderFixture fBash;
    fBash.pushTool(
        "agentxx_execute_bash_command",
        R"({"command":"invalid_cmd"})",
        true,
        true,
        "[Error] Command failed with code 127"
    );
    XX_TEST_EXPECT_TRUE(
        fBash.plainRender().find("Bash · [Error] Command failed with code 127") != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(fBash.render().find("255;85;85") != std::string::npos);

    // 异常中止 ([Exception aborted: ...])
    ToolHeaderFixture fExcept;
    fExcept.pushTool(
        "agentxx_web_fetch",
        R"({"url":"https://example.com"})",
        true,
        true,
        "[Exception aborted: connection timeout]"
    );
    XX_TEST_EXPECT_TRUE(
        fExcept.plainRender().find("Fetch · [Exception aborted: connection timeout]")
        != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(fExcept.render().find("255;85;85") != std::string::npos);

    // 未知工具执行失败折叠展示: 保持原始 toolName + 异常结果
    ToolHeaderFixture fUnknown;
    fUnknown
        .pushTool("custom_plugin_tool", R"({"foo":"bar"})", true, true, "[Error] Custom failure");
    XX_TEST_EXPECT_TRUE(
        fUnknown.plainRender().find("custom_plugin_tool · [Error] Custom failure")
        != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(fUnknown.render().find("255;85;85") != std::string::npos);

    // 展开状态下普通工具的 result: 同样使用 errorColor 标红
    ToolHeaderFixture fExpanded;
    fExpanded.pushTool(
        "agentxx_execute_bash_command",
        R"({"command":"ls"})",
        true,
        false, // 展开
        "[Error] exit code 1"
    );
    XX_TEST_EXPECT_TRUE(fExpanded.plainRender().find("result:") != std::string::npos);
    XX_TEST_EXPECT_TRUE(fExpanded.plainRender().find("[Error] exit code 1") != std::string::npos);
    XX_TEST_EXPECT_TRUE(fExpanded.render().find("255;85;85") != std::string::npos);
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

TestResult testTuiToolHeader() {
    testTuiToolHeaderFilesystem();
    testTuiToolHeaderWeb();
    testTuiToolHeaderFallback();
    testTuiToolHeaderOverflow();
    testTuiToolHeaderRunning();
    testTuiToolHeaderDecor();
    testTuiToolHeaderFailed();
    testTuiToolHeaderDuration();
    return {g_tui_tool_header_passed, g_tui_tool_header_failed};
}

} // namespace test
} // namespace agentxx
