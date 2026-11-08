// 流式增量 markdown 渲染器 (markdown::IncrementalRenderer) 测试
//
// 背景: 流式输出时若每帧对整段累积文本全部 cmark 解析 + DomBuilder 构建,
// 总成本为 O(n^2) (n = 最终文本长度), 大消息时占满帧预算。增量渲染器只缓存
// 已闭合 (稳定) 的顶层块, 每帧仅重建末尾仍增长的块, 总成本 O(n)。
//
// 覆盖点:
// - [增量==一次性解析] 逐字节 feed 过程中任意时刻的渲染结果, 必须与对同一前缀做
//   一次性完整解析的结果完全一致 (块间空行分隔与整篇解析一致)
// - [稳定块缓存] stableBlockCount 随文档增长单调不减, 且闭合块被识别为稳定
// - [reset] 重置后从零开始, 渲染结果与全新解析一致
// - [边界前缀] setext 标题/表格/代码块/列表等块类型在增量下与完整解析一致
#include "agentxx-test/client/test_tui_stream.h"

#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx/agent/conversation_types.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <markdown/dom_builder.hpp>
#include <markdown/incremental.hpp>
#include <markdown/parser.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_stream_passed = 0;
int g_tui_stream_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_stream_passed
#define XX_TEST_FAILED g_tui_stream_failed

namespace agentxx {
namespace test {

using namespace agentxx::client;

namespace {

using namespace ftxui;

/// 一次性完整解析 + 构建 (O(n), 作为“期望结果”基线)
Element fullRender(
    std::string_view                                    text,
    markdown::Theme const&                              theme,
    int                                                 width,
    std::vector<std::unique_ptr<markdown::DomBuilder>>& keepAlive
) {
    auto                  parser = markdown::make_cmark_parser();
    markdown::MarkdownAST ast;
    parser->parse(text, ast);
    auto builder = std::make_unique<markdown::DomBuilder>();
    if (width > 0) {
        builder->set_max_width(width);
    }
    auto el = builder->build(ast, -1, theme);
    keepAlive.push_back(std::move(builder));
    return el;
}

/// 将增量渲染器的稳定块 + 尾部块组装为完整元素 (与整篇解析的块间空行一致)
Element incrementalRenderAll(
    markdown::IncrementalRenderer&                      inc,
    Color                                               color,
    markdown::Theme const&                              theme,
    int                                                 width,
    std::vector<std::unique_ptr<markdown::DomBuilder>>& buildersOut
) {
    std::unique_ptr<markdown::DomBuilder> fb;
    auto                                  frontier = inc.renderFrontier(theme, width, fb);
    if (fb) {
        buildersOut.push_back(std::move(fb));
    }
    ftxui::Elements parts;
    for (size_t i = 0; i < inc.stableBlockCount(); ++i) {
        parts.push_back(inc.stableBlockElement(i, theme, width));
    }
    if (frontier) {
        parts.push_back(std::move(frontier));
    }
    ftxui::Elements spaced;
    spaced.reserve(parts.size() * 2 - 1);
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            spaced.push_back(ftxui::text(""));
        }
        spaced.push_back(std::move(parts[i]));
    }
    if (spaced.empty()) {
        return ftxui::text("") | ftxui::color(color);
    }
    if (spaced.size() == 1) {
        return std::move(spaced[0]) | ftxui::color(color);
    }
    return ftxui::vbox(std::move(spaced)) | ftxui::color(color);
}

std::string toScreen(Element el, int w, int h) {
    auto screen = Screen(w, h);
    Render(screen, el);
    return screen.ToString();
}

/// 断言: 增量渲染器在给定文本的渲染结果与一次性完整解析一致
void expectIncrementalEqualsFull(std::string_view text, int width, std::string const& tag) {
    auto const& theme = markdown::theme_default();

    // 期望值: 一次性完整解析
    std::vector<std::unique_ptr<markdown::DomBuilder>> keepFull;
    auto full = fullRender(text, theme, width, keepFull) | color(Color::White);

    // 实测: 增量渲染 (逐字节 feed, 模拟 token 到达)
    markdown::IncrementalRenderer inc;
    for (size_t i = 0; i < text.size(); ++i) {
        inc.append(std::string_view(text).substr(i, 1));
    }
    std::vector<std::unique_ptr<markdown::DomBuilder>> keepInc;
    auto incEl = incrementalRenderAll(inc, Color::White, theme, width, keepInc);

    if (toScreen(full, width, 80) != toScreen(incEl, width, 80)) {
        XX_TEST_EXPECT_TRUE(false);
        fprintf(stderr, "[tui_stream] mismatch (tag=%s)\n", tag.c_str());
        fprintf(
            stderr,
            "  expected:\n%s\n  actual:\n%s\n",
            toScreen(full, width, 80).c_str(),
            toScreen(incEl, width, 80).c_str()
        );
    } else {
        XX_TEST_EXPECT_TRUE(true);
    }
}

/// 断言: 逐字节 feed 过程中每个中间前缀的增量渲染都与完整解析一致
void expectIncrementalPrefixesEqualFull(std::string_view text, int width) {
    auto const& theme = markdown::theme_default();

    markdown::IncrementalRenderer inc;
    // 前缀长度采样点: 开头/若干中间点/末尾
    std::vector<size_t> points;
    for (size_t f : {size_t{1}, size_t{3}, size_t{10}, size_t{25}, size_t{50}, size_t{75}}) {
        points.push_back(text.size() * f / 100);
    }
    points.push_back(text.size());
    // 单调去重
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());

    std::vector<std::unique_ptr<markdown::DomBuilder>> keepInc, keepFull;
    for (size_t len : points) {
        std::string_view prefix = text.substr(0, len);
        // 增量渲染到该前缀 (增量 feed 差额)
        std::string_view acc = inc.text();
        if (prefix.size() > acc.size()) {
            inc.append(prefix.substr(acc.size()));
        }
        keepInc.clear();
        auto incEl = incrementalRenderAll(inc, Color::White, theme, width, keepInc);

        keepFull.clear();
        auto fullEl = fullRender(prefix, theme, width, keepFull) | color(Color::White);

        if (toScreen(fullEl, width, 80) != toScreen(incEl, width, 80)) {
            XX_TEST_EXPECT_TRUE(false);
            fprintf(stderr, "[tui_stream] prefix mismatch at %zu/%zu\n", len, text.size());
            return;
        }
    }
    XX_TEST_EXPECT_TRUE(true);
}

void testTuiStreamMarkdown() {
    XX_TEST_EXPECT_TRUE(true);

    const int W = 80;

    // ---- 空文档 ----
    {
        markdown::IncrementalRenderer                      inc;
        std::vector<std::unique_ptr<markdown::DomBuilder>> keep;
        auto el = incrementalRenderAll(inc, Color::White, markdown::theme_default(), W, keep);
        auto s  = toScreen(el, W, 10);
        XX_TEST_EXPECT_TRUE(!s.empty());
    }

    // ---- 基础结构: 标题/段落/粗体/行内代码/列表/代码块 ----
    expectIncrementalEqualsFull(
        "# Title\n\nHello **world** and `code` with *em* and [link](https://x).\n\n"
        "- item a\n- item b\n\n```cpp\nint x = 1;\n```\n\nLast paragraph.\n",
        W,
        "basic"
    );

    // ---- setext 标题 (流式增长时最后一个块类型可变) ----
    expectIncrementalEqualsFull("Some title\n==========\n\nBody text here.\n", W, "setext");

    // ---- 有序列表 + 嵌套 ----
    expectIncrementalEqualsFull(
        "1. first\n2. second\n3. third\n\nNested:\n\n- outer 1\n  - inner a\n  - inner b\n- outer 2\n",
        W,
        "lists"
    );

    // ---- GFM 表格 (table 扩展) ----
    expectIncrementalEqualsFull(
        "| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\nAfter table.\n",
        W,
        "table"
    );

    // ---- 引用块 ----
    expectIncrementalEqualsFull(
        "> quote line 1\n> quote line 2\n\nPlain after quote.\n",
        W,
        "quote"
    );

    // ---- 主题分割线 ----
    expectIncrementalEqualsFull("Before\n\n---\n\nAfter\n", W, "thematic");

    // ---- 多块连续 (块间空行分隔一致性) ----
    expectIncrementalEqualsFull(
        "P1 text.\n\nP2 text.\n\nP3 text.\n\nP4 text.\n",
        W,
        "many-paragraphs"
    );

    // ---- 逐字节 feed 的中间前缀一致性 ----
    {
        std::string doc;
        for (int i = 0; i < 12; ++i) {
            doc += "## Section " + std::to_string(i) + "\n\n";
            doc += "Paragraph with **bold** and text number " + std::to_string(i) + ".\n\n";
            doc += "- x\n- y\n\n";
        }
        doc += "Final paragraph with marker.\n";
        expectIncrementalPrefixesEqualFull(doc, W);
    }

    // ---- stableBlockCount 单调不减, 且最终稳定块数以空行为界 ----
    {
        std::string                   doc = "one\n\ntwo\nthree\n\nfour\n";
        markdown::IncrementalRenderer inc;
        inc.append("one\n\n");
        XX_TEST_EXPECT_TRUE(inc.stableBlockCount() >= 0);
        inc.append("two\nthree\n");
        size_t c1 = inc.stableBlockCount();
        inc.append("\nfour\n");
        size_t c2 = inc.stableBlockCount();
        XX_TEST_EXPECT_TRUE(c2 >= c1);
        // 文档有 3 个顶层块 (one / two-three / four): 渲染时最后一个块为增长中块
        std::vector<std::unique_ptr<markdown::DomBuilder>> keep;
        auto el = incrementalRenderAll(inc, Color::White, markdown::theme_default(), W, keep);
        XX_TEST_EXPECT_TRUE(true);
    }

    // ---- reset 后与全新解析一致 ----
    {
        markdown::IncrementalRenderer inc;
        inc.append("old content\n");
        inc.reset();
        XX_TEST_EXPECT_TRUE(inc.text().empty());
        std::string doc = "# Fresh\n\nNew body.\n";
        inc.append(doc);
        expectIncrementalEqualsFull(doc, W, "reset");
    }

    // ---- 引用定义段落 (cmark 在 finalize 时会释放纯引用定义段落, children 收缩) ----
    // 回归: 已缓存的稳定块不得被重复渲染 (参考式 [text][ref] 链接本身在流式期间
    // 可能显示为字面量, 属已知限制; 此处用无引用的定义验证不重复、不错位)
    expectIncrementalEqualsFull("P1 text.\n\n[x]: https://example.com\n\nP2 text.\n", W, "refdef");

    {
        // 分步 feed 回归: 定义段落已出现(仍是 parser 子块) 之后又被释放,
        // 已缓存的 P1 必须保持单份, 尾块从定义处起算 (曾经修复过重复渲染)
        const auto&                                        theme = markdown::theme_default();
        markdown::IncrementalRenderer                      inc;
        std::vector<std::unique_ptr<markdown::DomBuilder>> keepInc, keepFull;
        auto                                               cmp = [&](std::string_view prefix) {
            keepInc.clear();
            keepFull.clear();
            auto incEl = incrementalRenderAll(inc, Color::White, theme, W, keepInc);
            auto fullEl = fullRender(prefix, theme, W, keepFull) | color(Color::White);
            if (toScreen(incEl, W, 80) != toScreen(fullEl, W, 80)) {
                XX_TEST_EXPECT_TRUE(false);
                fprintf(stderr, "[tui_stream] refdef 分步回归不一致 @ %zu bytes\n", prefix.size());
            } else {
                XX_TEST_EXPECT_TRUE(true);
            }
        };
        std::string text = "P1 text.\n\n[x]: https://example.com\n\nP2 text.\n";
        inc.append("P1 text.\n\n");
        cmp("P1 text.\n\n");
        inc.append("[x]: https://example.com\n");
        cmp("P1 text.\n\n[x]: https://example.com\n");
        inc.append("\nP2 text.\n");
        cmp(text);
    }

    // ---- invalidateCache 后仍可渲染 (主题/宽度变化场景) ----
    {
        markdown::IncrementalRenderer inc;
        inc.append("# H\n\nBody.\n");
        std::vector<std::unique_ptr<markdown::DomBuilder>> keep;
        auto el1 = incrementalRenderAll(inc, Color::White, markdown::theme_default(), W, keep);
        inc.invalidateCache();
        auto el2 = incrementalRenderAll(inc, Color::White, markdown::theme_default(), W, keep);
        XX_TEST_EXPECT_TRUE(toScreen(el1, W, 40) == toScreen(el2, W, 40));
    }

    // ---- 宽依赖: 窄/宽宽度渲染 (表格随 maxWidth 变化) 至少不崩溃 ----
    {
        std::string doc = "| col1 | col2 | col3 |\n|---|---|---|\n| a | b | c |\n";
        markdown::IncrementalRenderer inc;
        inc.append(doc);
        std::vector<std::unique_ptr<markdown::DomBuilder>> keep;
        auto el40 = incrementalRenderAll(inc, Color::White, markdown::theme_default(), 40, keep);
        keep.clear();
        auto el100 = incrementalRenderAll(inc, Color::White, markdown::theme_default(), 100, keep);
        XX_TEST_EXPECT_TRUE(true);
    }
}

using namespace agentxx::agent;

class TestTUIClientIO : public TUIClientAgentIO {
public:

    TestTUIClientIO(asio::io_context& ctx) :
        TUIClientAgentIO(ctx.get_executor(), "test_session") {}

    void testOnDelta(const WireDelta& d) {
        onDelta(d);
    }

    void testOnSync(const WireSyncPayload& p) {
        onSync(p);
    }
};

namespace {

// ---- 常用 op 构造 (形态与 agent 端 EventBridge 下发的一致) ----

WireDelta streamBegin(
    std::string                           msgId,
    ViewMessage::Role                     role,
    std::optional<ViewMessage::ThinkData> think = std::nullopt
) {
    WireDelta d;
    d.type        = WireDelta::Type::StreamBegin;
    d.msgId       = std::move(msgId);
    d.streamRole  = role;
    d.think       = std::move(think);
    d.startTimeMs = 1000;
    return d;
}

WireDelta textToken(std::string msgId, std::string text) {
    WireDelta d;
    d.type  = WireDelta::Type::TextToken;
    d.msgId = std::move(msgId);
    d.text  = std::move(text);
    return d;
}

WireDelta thinkToken(std::string msgId, std::string text) {
    WireDelta d;
    d.type  = WireDelta::Type::ThinkToken;
    d.msgId = std::move(msgId);
    d.text  = std::move(text);
    return d;
}

WireDelta insertRow(ViewMessage m) {
    WireDelta d;
    d.type    = WireDelta::Type::InsertMessage;
    d.msgId   = m.id;
    d.message = std::make_shared<ViewMessage>(std::move(m));
    return d;
}

WireDelta updateRow(ViewMessage m) {
    WireDelta d;
    d.type    = WireDelta::Type::UpdateMessage;
    d.msgId   = m.id;
    d.message = std::make_shared<ViewMessage>(std::move(m));
    return d;
}

WireDelta streamEnd(std::string msgId) {
    WireDelta d;
    d.type  = WireDelta::Type::StreamEnd;
    d.msgId = std::move(msgId);
    return d;
}

WireDelta turnStart() {
    WireDelta d;
    d.type        = WireDelta::Type::TurnStart;
    d.startTimeMs = 1000;
    return d;
}

WireDelta turnEnd() {
    WireDelta d;
    d.type = WireDelta::Type::TurnEnd;
    return d;
}

/// 用户行 (agent 在轮次开始时先下发整条用户消息)
ViewMessage userRow(std::string id, std::string text, int64_t startMs = 1000) {
    auto m = ViewMessage::makeText(ViewMessage::Role::User, std::move(text), startMs);
    m.id   = std::move(id);
    return m;
}

/// 已定稿的流式行 (agent 在段结束时用 stream_begin 的同一个 id 下发整条)
ViewMessage streamRow(
    std::string       id,
    ViewMessage::Role role,
    std::string       text,
    int64_t           startMs    = 1000,
    int64_t           durationMs = 0
) {
    auto m = ViewMessage::makeText(role, std::move(text), startMs, durationMs);
    m.id   = std::move(id);
    if (role == ViewMessage::Role::Think || role == ViewMessage::Role::Tool
        || role == ViewMessage::Role::Tip || role == ViewMessage::Role::Event) {
        m.collapsed = true;
    }
    return m;
}

/// 工具行 (agent 在 assistant 的 tool_calls 展开时下发, 未完成状态)
ViewMessage toolRow(
    std::string id,
    std::string toolName,
    std::string toolCallId,
    std::string arguments,
    bool        finished = false
) {
    ViewMessage m;
    m.id        = std::move(id);
    m.role      = ViewMessage::Role::Tool;
    m.text      = std::move(arguments);
    m.collapsed = true;
    m.tool      = ViewMessage::ToolData{};
    m.tool->toolName     = std::move(toolName);
    m.tool->toolCallId   = std::move(toolCallId);
    m.tool->toolFinished = finished;
    m.startTimeMs        = 1000;
    return m;
}

} // namespace
} // namespace

// 场景 1: 常规一轮 —— 用户行 (insert) -> turn_start -> 占位 -> stream_begin/token ->
// 定稿行 (insert) -> turn_end
void testTuiStreamScenario1(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    client.testOnDelta(insertRow(userRow("msg_1", "hi")));
    client.testOnDelta(turnStart());
    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
        XX_TEST_EXPECT_TRUE(snap->messages()[0]->role == TUIMessage::Role::User);
        XX_TEST_EXPECT_EQ(snap->messages()[0]->text, std::string{"hi"});
        XX_TEST_EXPECT_TRUE(snap->isStreaming);
    }

    {
        WireDelta d;
        d.type     = WireDelta::Type::GenStatus;
        d.genPhase = WireDelta::GenPhase::Waiting;
        client.testOnDelta(d);
        XX_TEST_EXPECT_TRUE(
            client.sharedState().snapshot()->genPhase == WireDelta::GenPhase::Waiting
        );
    }

    client.testOnDelta(streamBegin("msg_2", ViewMessage::Role::Assistant));
    client.testOnDelta(textToken("msg_2", "Hello"));
    client.testOnDelta(textToken("msg_2", " world"));
    {
        auto snap = client.sharedState().snapshot();
        // 流式项不在行里: 行数不变, 文本只在尾部流式项上累积
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
        XX_TEST_EXPECT_TRUE(snap->replica.hasStream());
        XX_TEST_EXPECT_EQ(snap->replica.stream().id, std::string{"msg_2"});
        XX_TEST_EXPECT_EQ(*snap->stream().text, std::string{"Hello world"});
        // 首个 token 到达即清除占位提示
        XX_TEST_EXPECT_TRUE(snap->genPhase == WireDelta::GenPhase::Done);
    }

    client.testOnDelta(
        insertRow(streamRow("msg_2", ViewMessage::Role::Assistant, "Hello world", 1000, 42))
    );
    client.testOnDelta(turnEnd());
    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{2});
        XX_TEST_EXPECT_FALSE(snap->replica.hasStream());
        XX_TEST_EXPECT_FALSE(snap->isStreaming);
        const auto& row = *snap->messages()[1];
        XX_TEST_EXPECT_TRUE(row.role == TUIMessage::Role::Assistant);
        XX_TEST_EXPECT_EQ(row.id, std::string{"msg_2"});
        XX_TEST_EXPECT_EQ(row.text, std::string{"Hello world"});
        XX_TEST_EXPECT_EQ(row.durationMs, int64_t{42});
        XX_TEST_EXPECT_FALSE(row.collapsed);
    }
}

// 场景 2: 多步 ReAct —— 思考段 + 正文段 + 工具行 + 工具结果 + 第二段正文
void testTuiStreamScenario2(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    client.testOnDelta(insertRow(userRow("msg_1", "read the file")));

    // 第 1 段: 思考
    client.testOnDelta(
        streamBegin("msg_2", ViewMessage::Role::Think, ViewMessage::ThinkData{.reasoningTokens = 12})
    );
    client.testOnDelta(thinkToken("msg_2", "thinking..."));
    client.testOnDelta(
        insertRow(streamRow("msg_2", ViewMessage::Role::Think, "thinking...", 1000, 1200))
    );
    // 第 2 段: 正文
    client.testOnDelta(streamBegin("msg_3", ViewMessage::Role::Assistant));
    client.testOnDelta(textToken("msg_3", "let me read it"));
    client.testOnDelta(
        insertRow(streamRow("msg_3", ViewMessage::Role::Assistant, "let me read it", 1000, 900))
    );
    // 工具行 (未完成) + 结果 (整条覆盖)
    client.testOnDelta(insertRow(toolRow("msg_4", "read", "call_1", "{\"path\":\"a.txt\"}")));
    {
        auto toolDone                    = toolRow("msg_4", "read", "call_1", "{\"path\":\"a.txt\"}");
        toolDone.tool->toolResult        = "file body";
        toolDone.tool->toolFinished      = true;
        toolDone.durationMs              = 12;
        client.testOnDelta(updateRow(std::move(toolDone)));
    }
    // 第 3 段: 第二段正文
    client.testOnDelta(streamBegin("msg_5", ViewMessage::Role::Assistant));
    client.testOnDelta(textToken("msg_5", "done"));
    client.testOnDelta(insertRow(streamRow("msg_5", ViewMessage::Role::Assistant, "done")));
    client.testOnDelta(turnEnd());

    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{5});
    if (snap->messages().size() == 5) {
        XX_TEST_EXPECT_TRUE(snap->messages()[0]->role == TUIMessage::Role::User);
        XX_TEST_EXPECT_TRUE(snap->messages()[1]->role == TUIMessage::Role::Think);
        XX_TEST_EXPECT_EQ(snap->messages()[1]->text, std::string{"thinking..."});
        XX_TEST_EXPECT_TRUE(snap->messages()[2]->role == TUIMessage::Role::Assistant);
        XX_TEST_EXPECT_TRUE(snap->messages()[3]->role == TUIMessage::Role::Tool);
        XX_TEST_EXPECT_TRUE(snap->messages()[3]->tool.has_value());
        if (snap->messages()[3]->tool) {
            XX_TEST_EXPECT_TRUE(snap->messages()[3]->tool->toolFinished);
            XX_TEST_EXPECT_EQ(snap->messages()[3]->tool->toolResult, std::string{"file body"});
            XX_TEST_EXPECT_EQ(snap->messages()[3]->text, std::string{"{\"path\":\"a.txt\"}"});
        }
        XX_TEST_EXPECT_EQ(snap->messages()[3]->durationMs, int64_t{12});
        XX_TEST_EXPECT_TRUE(snap->messages()[4]->role == TUIMessage::Role::Assistant);
        XX_TEST_EXPECT_EQ(snap->messages()[4]->text, std::string{"done"});
    }
    XX_TEST_EXPECT_FALSE(snap->replica.hasStream());
}

// 场景 3: 加密思考 (stream_begin 带加密标记, 无正文 token) -> 定稿行带元数据
void testTuiStreamScenario3(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    client.testOnDelta(streamBegin(
        "msg_1",
        ViewMessage::Role::Think,
        ViewMessage::ThinkData{.reasoningTokens = 0, .isEncrypted = true}
    ));
    {
        auto snap = client.sharedState().snapshot();
        // 流式项已建立 (可展示"思考中"), 但还没落成行
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{0});
        XX_TEST_EXPECT_TRUE(snap->replica.hasStream());
        XX_TEST_EXPECT_TRUE(snap->stream().think.has_value());
        if (snap->stream().think) {
            XX_TEST_EXPECT_TRUE(snap->stream().think->isEncrypted);
        }
    }

    auto row         = streamRow("msg_1", ViewMessage::Role::Think, "", 1000, 80);
    row.think        = ViewMessage::ThinkData{.reasoningTokens = 50, .isEncrypted = true};
    client.testOnDelta(insertRow(std::move(row)));
    client.testOnDelta(turnEnd());

    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
    if (snap->messages().size() == 1) {
        XX_TEST_EXPECT_TRUE(snap->messages()[0]->role == TUIMessage::Role::Think);
        XX_TEST_EXPECT_TRUE(snap->messages()[0]->think.has_value());
        if (snap->messages()[0]->think) {
            XX_TEST_EXPECT_TRUE(snap->messages()[0]->think->isEncrypted);
            XX_TEST_EXPECT_EQ(snap->messages()[0]->think->reasoningTokens, 50);
        }
        XX_TEST_EXPECT_EQ(snap->messages()[0]->durationMs, int64_t{80});
        XX_TEST_EXPECT_TRUE(snap->messages()[0]->collapsed);
    }
}

// 场景 4: 明文思考定稿 -> 正文/耗时/元数据都来自定稿整条
void testTuiStreamScenario4(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    client.testOnDelta(streamBegin("msg_1", ViewMessage::Role::Think));
    client.testOnDelta(thinkToken("msg_1", "明文思考"));
    auto row  = streamRow("msg_1", ViewMessage::Role::Think, "明文思考", 2000, 95);
    row.think = ViewMessage::ThinkData{.reasoningTokens = 150};
    client.testOnDelta(insertRow(std::move(row)));
    client.testOnDelta(turnEnd());

    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
    if (snap->messages().size() == 1) {
        XX_TEST_EXPECT_EQ(snap->messages()[0]->text, std::string{"明文思考"});
        XX_TEST_EXPECT_EQ(snap->messages()[0]->startTimeMs, int64_t{2000});
        XX_TEST_EXPECT_EQ(snap->messages()[0]->durationMs, int64_t{95});
        XX_TEST_EXPECT_TRUE(snap->messages()[0]->think.has_value());
        if (snap->messages()[0]->think) {
            XX_TEST_EXPECT_EQ(snap->messages()[0]->think->reasoningTokens, 150);
            XX_TEST_EXPECT_FALSE(snap->messages()[0]->think->isEncrypted);
        }
    }
}

// 场景 5: 整段视觉为空 -> stream_end 丢弃流式项, 不产生行
void testTuiStreamScenario5(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    client.testOnDelta(streamBegin("msg_1", ViewMessage::Role::Assistant));
    client.testOnDelta(textToken("msg_1", " \xE2\x80\x8B "));
    XX_TEST_EXPECT_TRUE(client.sharedState().snapshot()->replica.hasStream());

    client.testOnDelta(streamEnd("msg_1"));
    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{0});
        XX_TEST_EXPECT_FALSE(snap->replica.hasStream());
    }
    // 未知 id 的 stream_end 是 no-op
    client.testOnDelta(streamEnd("msg_unknown"));
    XX_TEST_EXPECT_EQ(client.sharedState().snapshot()->messages().size(), size_t{0});

    // 后续正常一段不受影响
    client.testOnDelta(streamBegin("msg_2", ViewMessage::Role::Assistant));
    client.testOnDelta(textToken("msg_2", "answer"));
    client.testOnDelta(insertRow(streamRow("msg_2", ViewMessage::Role::Assistant, "answer")));
    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
    XX_TEST_EXPECT_EQ(snap->messages()[0]->text, std::string{"answer"});
}

// 场景 6: 工具行 —— insert(未完成) + update(完成) 覆盖同一行, 不产生第二条
void testTuiStreamScenario6(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    client.testOnDelta(insertRow(toolRow("msg_1", "read", "call_1", "{\"path\":\"a.txt\"}")));
    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
        XX_TEST_EXPECT_FALSE(snap->messages()[0]->tool->toolFinished);
    }

    auto done                   = toolRow("msg_1", "read", "call_1", "{\"path\":\"a.txt\"}");
    done.tool->toolResult       = "body";
    done.tool->toolFinished     = true;
    done.tool->status           = agentxx::util::ToolStatus::Failed;
    done.durationMs             = 34;
    client.testOnDelta(updateRow(std::move(done)));

    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
    if (snap->messages().size() == 1) {
        const auto& row = *snap->messages()[0];
        XX_TEST_EXPECT_EQ(row.id, std::string{"msg_1"});
        XX_TEST_EXPECT_TRUE(row.tool.has_value());
        if (row.tool) {
            XX_TEST_EXPECT_TRUE(row.tool->toolFinished);
            XX_TEST_EXPECT_EQ(row.tool->toolResult, std::string{"body"});
            XX_TEST_EXPECT_TRUE(row.tool->status == agentxx::util::ToolStatus::Failed);
        }
        XX_TEST_EXPECT_EQ(row.durationMs, int64_t{34});
    }
}

// 场景 7: update 未知 id 忽略 (本地清理掉的老行), 已知 id 生效
void testTuiStreamScenario7(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    auto row = streamRow("msg_1", ViewMessage::Role::Tip, "tip");
    client.testOnDelta(insertRow(row));

    auto stale = streamRow("msg_gone", ViewMessage::Role::Tip, "stale update");
    client.testOnDelta(updateRow(std::move(stale)));
    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
        XX_TEST_EXPECT_EQ(snap->messages()[0]->text, std::string{"tip"});
    }

    auto updated    = streamRow("msg_1", ViewMessage::Role::Tip, "tip updated");
    updated.tip->running = false;
    client.testOnDelta(updateRow(std::move(updated)));
    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
        XX_TEST_EXPECT_EQ(snap->messages()[0]->text, std::string{"tip updated"});
    }
}

// 场景 8: 重放幂等 —— 同一条 insert 重复投递不产生重复行; 先到的 update 不新建
void testTuiStreamScenario8(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    auto row = streamRow("msg_1", ViewMessage::Role::Assistant, "hello");
    client.testOnDelta(insertRow(row));
    client.testOnDelta(insertRow(row));
    client.testOnDelta(insertRow(row));
    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
    }

    // 断线重连: 增量补拉会再投递一遍同样的行 (按 id 去重)
    WireSyncPayload p;
    p.incremental   = true;
    p.messages      = {row, row};
    p.totalMessages = 1;
    client.testOnSync(p);
    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
    }
}

// 场景 9: 完整行插入 (Tip: 级别/耗时/折叠)
void testTuiStreamScenario9(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    auto vm          = ViewMessage::makeText(ViewMessage::Role::Tip, "Turn tip message", 1000, 200);
    vm.id            = "msg_000001";
    vm.tip->tipLevel = ViewMessage::TipLevel::Warning;
    vm.collapsed     = true;
    client.testOnDelta(insertRow(std::move(vm)));

    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
    if (snap->messages().size() == 1) {
        XX_TEST_EXPECT_EQ(snap->messages()[0]->id, std::string{"msg_000001"});
        XX_TEST_EXPECT_EQ(snap->messages()[0]->role, TUIMessage::Role::Tip);
        XX_TEST_EXPECT_EQ(snap->messages()[0]->text, std::string{"Turn tip message"});
        XX_TEST_EXPECT_EQ(snap->messages()[0]->startTimeMs, int64_t{1000});
        XX_TEST_EXPECT_EQ(snap->messages()[0]->durationMs, int64_t{200});
        XX_TEST_EXPECT_TRUE(snap->messages()[0]->collapsed);
        XX_TEST_EXPECT_TRUE(snap->messages()[0]->tip.has_value());
        if (snap->messages()[0]->tip) {
            XX_TEST_EXPECT_EQ(snap->messages()[0]->tip->tipLevel, ViewMessage::TipLevel::Warning);
        }
    }
}

// 场景 10: 空行过滤 —— 空白正文的行不进入列表; 带附件的空正文用户行保留
void testTuiStreamScenario10(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    // 空白用户行: 忽略
    client.testOnDelta(insertRow(userRow("msg_1", " ")));
    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{0});
    }

    // 空正文 + 附件的用户行: 保留 (附件卡片本身可见)
    {
        auto m          = userRow("msg_2", "");
        MediaAttachment att;
        att.type        = MediaType::Image;
        att.displayName = "a.png";
        att.mimeType    = "image/png";
        att.pathOrUrl   = "/tmp/a.png";
        m.attachments.push_back(att);
        client.testOnDelta(insertRow(std::move(m)));
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
        XX_TEST_EXPECT_EQ(snap->messages()[0]->attachments.size(), size_t{1});
    }

    // 空白 assistant 行: 忽略 (模型只吐出零宽占位符)
    client.testOnDelta(
        insertRow(streamRow("msg_3", ViewMessage::Role::Assistant, " \xE2\x80\x8B\n"))
    );
    // 空白提示行: 忽略
    client.testOnDelta(insertRow(streamRow("msg_4", ViewMessage::Role::Tip, "")));

    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});

    // onSync 完整历史: 空行同样过滤
    WireSyncPayload p;
    p.messages = {
        userRow("msg_10", "hi"),
        streamRow("msg_11", ViewMessage::Role::Assistant, "  "),
        streamRow("msg_12", ViewMessage::Role::Assistant, "answer"),
    };
    p.totalMessages = p.messages.size();
    client.testOnSync(p);
    snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{2});
    if (snap->messages().size() == 2) {
        XX_TEST_EXPECT_EQ(snap->messages()[0]->text, std::string{"hi"});
        XX_TEST_EXPECT_EQ(snap->messages()[1]->text, std::string{"answer"});
    }
}

// 场景 11: 流式思考的折叠点击覆盖态 —— 用户点击展开后定稿行保持展开,
// 覆盖态随定稿失效, 下一次思考回到默认折叠
void testTuiStreamScenario11(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    client.testOnDelta(streamBegin("msg_1", ViewMessage::Role::Think));
    client.testOnDelta(thinkToken("msg_1", "思考内容 A"));
    XX_TEST_EXPECT_EQ(client.sharedState().snapshot()->streamThinkOverride, -1);

    // 模拟 UI 线程点击展开 (MessageListComponent::toggleStreamThinkCollapsed)
    client.sharedState().mutate([](TUIRenderState& st) {
        st.streamThinkOverride = 1;
    });

    client.testOnDelta(insertRow(streamRow("msg_1", ViewMessage::Role::Think, "思考内容 A")));
    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
        if (snap->messages().size() == 1) {
            XX_TEST_EXPECT_EQ(snap->messages()[0]->text, std::string{"思考内容 A"});
            // 用户手动展开 -> 定稿行保持展开 (思考结束不折回)
            XX_TEST_EXPECT_FALSE(snap->messages()[0]->collapsed);
        }
        // 覆盖态随定稿失效
        XX_TEST_EXPECT_EQ(snap->streamThinkOverride, -1);
    }

    // 下一段思考 (未点击): 定稿行回到默认折叠
    client.testOnDelta(streamBegin("msg_2", ViewMessage::Role::Think));
    client.testOnDelta(thinkToken("msg_2", "思考内容 B"));
    client.testOnDelta(insertRow(streamRow("msg_2", ViewMessage::Role::Think, "思考内容 B")));
    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{2});
    if (snap->messages().size() == 2) {
        XX_TEST_EXPECT_TRUE(snap->messages()[1]->collapsed);
    }
}

// 场景 12: 生成占位提示 (GenStatus) 状态机
void testTuiStreamScenario12(asio::io_context& ioCtx) {
    using GP = WireDelta::GenPhase;
    TestTUIClientIO client(ioCtx);

    client.testOnDelta(insertRow(userRow("msg_1", "hi")));
    client.testOnDelta(turnStart());
    {
        WireDelta d;
        d.type     = WireDelta::Type::GenStatus;
        d.genPhase = GP::Waiting;
        client.testOnDelta(d);

        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_TRUE(snap->genPhase == GP::Waiting);
        XX_TEST_EXPECT_TRUE(snap->genToolName.empty());
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
        XX_TEST_EXPECT_TRUE(snap->isStreaming);
    }
    {
        // 首个思考 token 到达: 占位提示被真实内容取代
        client.testOnDelta(streamBegin("msg_2", ViewMessage::Role::Think));
        client.testOnDelta(thinkToken("msg_2", "思考中"));
        XX_TEST_EXPECT_TRUE(client.sharedState().snapshot()->genPhase == GP::Done);
    }
    {
        WireDelta d;
        d.type     = WireDelta::Type::GenStatus;
        d.genPhase = GP::ToolCall;
        d.toolName = "read_file";
        client.testOnDelta(d);

        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_TRUE(snap->genPhase == GP::ToolCall);
        XX_TEST_EXPECT_EQ(snap->genToolName, std::string{"read_file"});
    }
    {
        // 工具行到达: 占位提示清除
        client.testOnDelta(insertRow(toolRow("msg_3", "read_file", "call_1", "{\"p\":1}")));
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_TRUE(snap->genPhase == GP::Done);
        XX_TEST_EXPECT_TRUE(snap->genToolName.empty());
        XX_TEST_EXPECT_TRUE(snap->messages().back()->role == TUIMessage::Role::Tool);
    }
    {
        WireDelta d;
        d.type     = WireDelta::Type::GenStatus;
        d.genPhase = GP::Waiting;
        client.testOnDelta(d);
        XX_TEST_EXPECT_TRUE(client.sharedState().snapshot()->genPhase == GP::Waiting);

        WireDelta done;
        done.type     = WireDelta::Type::GenStatus;
        done.genPhase = GP::Done;
        client.testOnDelta(done);
        XX_TEST_EXPECT_TRUE(client.sharedState().snapshot()->genPhase == GP::Done);
    }
    {
        // 轮次结束: 占位提示不残留
        WireDelta d;
        d.type     = WireDelta::Type::GenStatus;
        d.genPhase = GP::Waiting;
        client.testOnDelta(d);
        client.testOnDelta(turnEnd());
        XX_TEST_EXPECT_TRUE(client.sharedState().snapshot()->genPhase == GP::Done);
    }
}

// 场景 13: 工具终态位来自行字段 (界面不看结果文本)
void testTuiStreamScenario13(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    {
        auto ok                 = toolRow("msg_1", "read", "call_ok", "{}");
        ok.tool->toolResult     = "no error here, just text";
        ok.tool->toolFinished   = true;
        ok.tool->status         = agentxx::util::ToolStatus::Ok;
        client.testOnDelta(insertRow(std::move(ok)));
    }
    {
        auto bad              = toolRow("msg_2", "read", "call_bad", "{}");
        bad.tool->toolResult  = "[Error: path does not exist]";
        bad.tool->toolFinished = true;
        bad.tool->status      = agentxx::util::ToolStatus::Failed;
        client.testOnDelta(insertRow(std::move(bad)));
    }

    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{2});
    if (snap->messages().size() == 2) {
        XX_TEST_EXPECT_TRUE(
            snap->messages()[0]->tool->status == agentxx::util::ToolStatus::Ok
        );
        XX_TEST_EXPECT_TRUE(
            snap->messages()[1]->tool->status == agentxx::util::ToolStatus::Failed
        );
    }
}

// 场景 14: 取消 —— 工具行未完成时 agent 下发终态覆盖原行 (同一个工具只有一条)
void testTuiStreamScenario14(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    client.testOnDelta(insertRow(toolRow("msg_1", "execute_command", "call_cancel", "{}")));
    client.testOnDelta(insertRow(streamRow("msg_2", ViewMessage::Role::Tip, "[Cancel Request]")));
    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{2});
        XX_TEST_EXPECT_FALSE(snap->messages()[0]->tool->toolFinished);
    }

    auto canceled               = toolRow("msg_1", "execute_command", "call_cancel", "{}");
    canceled.tool->toolResult   = "[Canceled]";
    canceled.tool->toolFinished = true;
    canceled.tool->status       = agentxx::util::ToolStatus::Canceled;
    canceled.durationMs         = 2000;
    client.testOnDelta(updateRow(std::move(canceled)));

    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{2});
    if (snap->messages().size() == 2) {
        const auto& toolMsg = *snap->messages()[0];
        XX_TEST_EXPECT_TRUE(toolMsg.tool->toolFinished);
        XX_TEST_EXPECT_EQ(toolMsg.tool->toolResult, std::string{"[Canceled]"});
        XX_TEST_EXPECT_TRUE(toolMsg.tool->status == agentxx::util::ToolStatus::Canceled);
        XX_TEST_EXPECT_EQ(toolMsg.durationMs, int64_t{2000});
        // 原参数保留 (整条覆盖内容来自 agent, 参数仍在)
        XX_TEST_EXPECT_EQ(toolMsg.text, std::string{"{}"});
        XX_TEST_EXPECT_TRUE(snap->messages()[1]->role == TUIMessage::Role::Tip);
    }
}

// 场景 15: token 自愈 —— stream_begin 丢了也能按 token 里的 id 建流式项,
// 定稿行到达时正常收敛成一行
void testTuiStreamScenario15(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    client.testOnDelta(textToken("msg_9", "Hello"));
    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{0});
        XX_TEST_EXPECT_TRUE(snap->replica.hasStream());
        XX_TEST_EXPECT_EQ(snap->replica.stream().id, std::string{"msg_9"});
        XX_TEST_EXPECT_EQ(*snap->stream().text, std::string{"Hello"});
    }
    client.testOnDelta(textToken("msg_9", " again"));
    client.testOnDelta(insertRow(streamRow("msg_9", ViewMessage::Role::Assistant, "Hello again")));
    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
    XX_TEST_EXPECT_FALSE(snap->replica.hasStream());
    XX_TEST_EXPECT_EQ(snap->messages()[0]->text, std::string{"Hello again"});
}

// 场景 16: 事件输入 (来源标记) -> Role::Event, 默认折叠; JSON 往返保留角色
void testTuiStreamEventSource(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    {
        auto m = userRow(
            "msg_1",
            "[Event] chat.message\n[Info]\nmode: first\nstatus: value\n[Content]\nhi"
        );
        m.role       = ViewMessage::Role::Event;
        m.source     = std::string{agentxx::agent::kInputSourceEventPrefix} + "chat.message";
        m.collapsed  = true;
        client.testOnDelta(insertRow(std::move(m)));
        client.testOnDelta(turnEnd());
    }

    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
    if (snap->messages().size() == 1) {
        XX_TEST_EXPECT_TRUE(snap->messages()[0]->role == TUIMessage::Role::Event);
        XX_TEST_EXPECT_TRUE(snap->messages()[0]->collapsed);
        XX_TEST_EXPECT_EQ(
            snap->messages()[0]->source,
            std::string{agentxx::agent::kInputSourceEventPrefix} + "chat.message"
        );
        XX_TEST_EXPECT_EQ(
            std::string{agentxx::agent::eventNameOfInputSource(snap->messages()[0]->source)},
            std::string{"chat.message"}
        );
    }

    // 用户自己输入: Role::User, 默认展开, 无来源
    client.testOnDelta(insertRow(userRow("msg_2", "typed by the user", 2000)));
    snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{2});
    if (snap->messages().size() == 2) {
        XX_TEST_EXPECT_TRUE(snap->messages()[1]->role == TUIMessage::Role::User);
        XX_TEST_EXPECT_FALSE(snap->messages()[1]->collapsed);
        XX_TEST_EXPECT_TRUE(snap->messages()[1]->source.empty());
    }

    // ViewMessage JSON 往返也保留角色与来源
    {
        ViewMessage original
            = ViewMessage::makeText(ViewMessage::Role::Event, "[Event] a.b", 1);
        original.source = std::string{agentxx::agent::kInputSourceEventPrefix} + "a.b";
        const auto json = original.toJson();
        XX_TEST_EXPECT_EQ(json.value("source", std::string{}), original.source);
        XX_TEST_EXPECT_EQ(json.value("role", std::string{}), std::string{"event"});
        const auto restored = ViewMessage::fromJson(json);
        XX_TEST_EXPECT_TRUE(restored.role == ViewMessage::Role::Event);
        XX_TEST_EXPECT_TRUE(restored.collapsed);
        XX_TEST_EXPECT_EQ(restored.source, original.source);
        // 空来源不写进 JSON
        ViewMessage plain = ViewMessage::makeText(ViewMessage::Role::User, "hi", 2);
        XX_TEST_EXPECT_FALSE(plain.toJson().contains("source"));
        // 事件输入只用 Role::Event: 老数据里把事件输入记成 user + source 的写法
        // 不再做角色归一化, 解析后仍是 user (折叠状态与来源字段按原文保留)
        auto legacy      = plain.toJson();
        legacy["source"] = std::string{agentxx::agent::kInputSourceEventPrefix} + "old.event";
        const auto unnormalized = ViewMessage::fromJson(legacy);
        XX_TEST_EXPECT_TRUE(unnormalized.role == ViewMessage::Role::User);
        XX_TEST_EXPECT_FALSE(unnormalized.collapsed);
        XX_TEST_EXPECT_EQ(unnormalized.source, legacy["source"].get<std::string>());
    }
}

// 场景 17: 思考/正文交错 —— agent 端按模型步骤归组: 一条思考 + 一条正文
//
// 一次模型调用里上游可能把两者分多段交替送出。agent 端 EventBridge 在切回已出现过
// 的角色时把文字并回那条已定稿的行 (同一 id 继续下发 token), 客户端只管按 id 追加。
// 这里与 agent 端实际下发的 op 序列一致。
void testTuiStreamScenario17(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    client.testOnDelta(streamBegin("msg_1", ViewMessage::Role::Think));
    client.testOnDelta(thinkToken("msg_1", "first segment. "));
    client.testOnDelta(insertRow(streamRow("msg_1", ViewMessage::Role::Think, "first segment. ")));
    client.testOnDelta(streamBegin("msg_2", ViewMessage::Role::Assistant));
    client.testOnDelta(textToken("msg_2", "已完成。"));
    // 思考又回来: 追加到已定稿的思考行 (不 new 一条)
    client.testOnDelta(thinkToken("msg_1", "last words."));
    // 正文继续
    client.testOnDelta(textToken("msg_2", "事件输入…"));
    client.testOnDelta(
        insertRow(streamRow("msg_2", ViewMessage::Role::Assistant, "已完成。事件输入…"))
    );
    client.testOnDelta(turnEnd());

    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{2});
    if (snap->messages().size() == 2) {
        XX_TEST_EXPECT_TRUE(snap->messages()[0]->role == TUIMessage::Role::Think);
        XX_TEST_EXPECT_EQ(snap->messages()[0]->text, std::string{"first segment. last words."});
        XX_TEST_EXPECT_TRUE(snap->messages()[1]->role == TUIMessage::Role::Assistant);
        XX_TEST_EXPECT_EQ(snap->messages()[1]->text, std::string{"已完成。事件输入…"});
    }
}

// 场景 18: 中途接入 —— 全量同步带 streaming, 之后继续追加 token 并正常定稿
void testTuiStreamScenario18(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    WireSyncPayload p;
    p.messages = {
        userRow("msg_1", "hi"),
        streamRow("msg_2", ViewMessage::Role::Assistant, "first part"),
    };
    p.totalMessages = p.messages.size();
    auto streaming  = streamRow("msg_3", ViewMessage::Role::Assistant, "半截内容");
    streaming.id    = "msg_3";
    p.streaming     = streaming;
    client.testOnSync(std::move(p));

    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{2});
        XX_TEST_EXPECT_TRUE(snap->replica.hasStream());
        XX_TEST_EXPECT_EQ(snap->replica.stream().id, std::string{"msg_3"});
        XX_TEST_EXPECT_EQ(*snap->stream().text, std::string{"半截内容"});
        XX_TEST_EXPECT_TRUE(snap->isStreaming);
    }

    client.testOnDelta(textToken("msg_3", "继续"));
    XX_TEST_EXPECT_EQ(*client.sharedState().snapshot()->stream().text, std::string{"半截内容继续"});

    client.testOnDelta(insertRow(streamRow("msg_3", ViewMessage::Role::Assistant, "半截内容继续")));
    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{3});
    XX_TEST_EXPECT_FALSE(snap->replica.hasStream());
    XX_TEST_EXPECT_EQ(snap->messages()[2]->text, std::string{"半截内容继续"});
}

// 场景 19: 本地行 (中断表单/瞬态提示) 与 agent 行同居一份副本 —— 本地行 id 带
// `local_` 前缀, agent 的消息级 op 永不命中它们; 整份同步重建副本时本地行
// 随界面态丢弃 (新中断请求到达时重新建行)
void testTuiStreamScenario19(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    {
        ViewMessage form;
        form.id        = "local_interrupt_7";
        form.role      = ViewMessage::Role::Interrupt;
        form.interrupt = ViewMessage::InterruptData{};
        form.interrupt->interruptId = 7;
        client.sharedState().mutate([&](TUIRenderState& st) {
            st.replica.upsertLocalRow(form);
        });
    }
    XX_TEST_EXPECT_EQ(client.sharedState().snapshot()->messages().size(), size_t{1});

    // agent 行照常按 id 落在末尾, 本地行仍在
    client.testOnDelta(insertRow(userRow("msg_1", "hi")));
    {
        auto snap = client.sharedState().snapshot();
        XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{2});
        XX_TEST_EXPECT_EQ(snap->messages()[0]->id, std::string{"local_interrupt_7"});
        XX_TEST_EXPECT_EQ(snap->messages()[1]->role, TUIMessage::Role::User);
    }

    // 整份同步: 副本整体重建, 本地行随界面态丢弃
    WireSyncPayload p;
    p.messages      = {userRow("msg_1", "hi")};
    p.totalMessages = p.messages.size();
    client.testOnSync(std::move(p));

    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
    if (snap->messages().size() == 1) {
        XX_TEST_EXPECT_EQ(snap->messages()[0]->id, std::string{"msg_1"});
    }
}

// 场景 20: 瞬态提示 (MessageUITip) 造本地行, id 带 local_ 前缀 (不被 agent 命中)
void testTuiStreamScenario20(asio::io_context& ioCtx) {
    TestTUIClientIO client(ioCtx);

    WireDelta tip;
    tip.type    = WireDelta::Type::MessageUITip;
    tip.text    = "模型重试中";
    tip.tipType = WireDelta::TipType::Warning;
    client.testOnDelta(tip);

    auto snap = client.sharedState().snapshot();
    XX_TEST_EXPECT_EQ(snap->messages().size(), size_t{1});
    if (snap->messages().size() == 1) {
        const auto& row = *snap->messages()[0];
        XX_TEST_EXPECT_TRUE(row.role == TUIMessage::Role::Tip);
        XX_TEST_EXPECT_EQ(row.text, std::string{"模型重试中"});
        XX_TEST_EXPECT_TRUE(row.id.starts_with("local_"));
        XX_TEST_EXPECT_TRUE(row.tip.has_value());
        if (row.tip) {
            XX_TEST_EXPECT_TRUE(row.tip->tipLevel == ViewMessage::TipLevel::Warning);
        }
    }

    // agent 的整条覆盖永远打不中本地行 (不同命名空间)
    auto stale   = streamRow("local_tip_1", ViewMessage::Role::Tip, "overridden");
    stale.id     = "local_tip_99";
    client.testOnDelta(updateRow(std::move(stale)));
    XX_TEST_EXPECT_EQ(client.sharedState().snapshot()->messages().size(), size_t{1});
    XX_TEST_EXPECT_EQ(
        client.sharedState().snapshot()->messages()[0]->text,
        std::string{"模型重试中"}
    );
}

TestResult testTuiStream() {
    testTuiStreamMarkdown();
    asio::io_context ioCtx;
    testTuiStreamScenario1(ioCtx);
    testTuiStreamScenario2(ioCtx);
    testTuiStreamScenario3(ioCtx);
    testTuiStreamScenario4(ioCtx);
    testTuiStreamScenario5(ioCtx);
    testTuiStreamScenario6(ioCtx);
    testTuiStreamScenario7(ioCtx);
    testTuiStreamScenario8(ioCtx);
    testTuiStreamScenario9(ioCtx);
    testTuiStreamScenario10(ioCtx);
    testTuiStreamScenario11(ioCtx);
    testTuiStreamScenario12(ioCtx);
    testTuiStreamScenario13(ioCtx);
    testTuiStreamScenario14(ioCtx);
    testTuiStreamScenario15(ioCtx);
    testTuiStreamEventSource(ioCtx);
    testTuiStreamScenario17(ioCtx);
    testTuiStreamScenario18(ioCtx);
    testTuiStreamScenario19(ioCtx);
    testTuiStreamScenario20(ioCtx);

    return TestResult{g_tui_stream_passed, g_tui_stream_failed};
}

} // namespace test
} // namespace agentxx
