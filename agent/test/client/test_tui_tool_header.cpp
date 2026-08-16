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
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace agentxx {
namespace test {

int g_tui_tool_header_passed = 0;
int g_tui_tool_header_failed = 0;

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
        ctx.state          = &sharedState;
        ctx.frameState     = sharedState.readSnapshot();
        ctx.postRedraw     = [] {};
        ctx.theme          = &theme;
        ctx.showSystemInfo = nullptr;
        ctx.threadId       = "s";
        ctx.remoteUrl      = "";
        comp               = std::make_shared<MessageListComponent>(ctx);
    }

    /// 追加一条 Tool 消息 (text 为工具参数 JSON, 与 server 端约定一致)
    void pushTool(std::string name, std::string args) {
        sharedState.mutate([&](TUIRenderState& st) {
            auto m                = std::make_shared<TUIMessage>();
            m->role               = TUIMessage::Role::Tool;
            m->tool               = TUIMessage::ToolData{};
            m->tool->toolName     = std::move(name);
            m->tool->toolCallId   = "call_1";
            m->tool->toolFinished = true;
            // 与实际流水线一致: 已完成的 Tool 消息默认折叠展示 (event_stream 历史
            // 重连时 collapsed=true), 折叠头部即 "动词 · 参数摘要" 特化渲染
            m->collapsed = true;
            m->text      = std::move(args);
            // Tool 消息默认折叠展示 (与真实 TUI 流一致, 见 agent_tui.cpp);
            // 折叠态头部才显示 "动词 · 参数摘要" 特化渲染
            m->collapsed = true;
            st.messages.push_back(std::move(m));
        });
    }

    /// 追加一条 Thinking 消息 (折叠状态, 头部为 "-/+/[Thinking]/预览")
    void pushThinking(std::string text) {
        sharedState.mutate([&](TUIRenderState& st) {
            auto m       = std::make_shared<TUIMessage>();
            m->role      = TUIMessage::Role::Thinking;
            m->collapsed = true;
            m->text      = std::move(text);
            st.messages.push_back(std::move(m));
        });
    }

    /// 渲染一帧并返回屏幕文本
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
        "agentxx_filesystem_read_text_file",
        R"({"path":"/very/very/long/path/that/definitely/exceeds/the/narrow/viewport/width/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.cpp","line_offset":0,"line_limit":100})"
    );
    std::string out = f.render();
    // 前缀必须完整保留 (修复前会被按比例压扁, 丢失 "Tool]" 等字符)
    XX_TEST_EXPECT_TRUE(out.find("+ [Tool] ") != std::string::npos);
    // 摘要开头仍可见 (右缘裁剪)
    XX_TEST_EXPECT_TRUE(out.find("Read · ") != std::string::npos);

    // Thinking 折叠头部: 超长预览同样不得压缩 "- / [Thinking] " 前缀
    ToolHeaderFixture g(30, 8);
    g.pushThinking("这是一个非常非常非常非常非常非常非常非常非常非常非常非常非常长的思考内容"
                   "用来验证折叠预览超宽时头部前缀不会被压缩覆盖");
    std::string out2 = g.render();
    XX_TEST_EXPECT_TRUE(out2.find("+ [Thinking] ") != std::string::npos);
}

TestResult testTuiToolHeader() {
    testTuiToolHeaderFilesystem();
    testTuiToolHeaderWeb();
    testTuiToolHeaderFallback();
    testTuiToolHeaderOverflow();
    return {g_tui_tool_header_passed, g_tui_tool_header_failed};
}

} // namespace test
} // namespace agentxx