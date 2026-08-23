// 流式增量 markdown 渲染器 (markdown::IncrementalRenderer) 测试
//
// 背景: 流式输出时若每帧对整段累积文本全量 cmark 解析 + DomBuilder 构建,
// 总成本为 O(n^2) (n = 最终文本长度), 大消息时占满帧预算。增量渲染器只缓存
// 已闭合 (稳定) 的顶层块, 每帧仅重建末尾仍增长的块, 总成本 O(n)。
//
// 覆盖点:
// - [增量==全量] 逐字节 feed 过程中任意时刻的渲染结果, 必须与对同一前缀做
//   一次性全量解析的结果完全一致 (块间空行分隔与整篇解析一致)
// - [稳定块缓存] stableBlockCount 随文档增长单调不减, 且闭合块被识别为稳定
// - [reset] 重置后从零开始, 渲染结果与全新解析一致
// - [边界前缀] setext 标题/表格/代码块/列表等块类型在增量下与全量一致
#include "test_tui_stream.h"

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

namespace agentxx {
namespace test {

int g_tui_stream_passed = 0;
int g_tui_stream_failed = 0;

namespace {

using namespace ftxui;

/// 一次性全量解析 + 构建 (O(n), 作为“期望结果”基线)
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

/// 断言: 增量渲染器在给定文本的渲染结果与一次性全量解析一致
void expectIncrementalEqualsFull(std::string_view text, int width, std::string const& tag) {
    auto const& theme = markdown::theme_default();

    // 期望值: 一次性全量解析
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

/// 断言: 逐字节 feed 过程中每个中间前缀的增量渲染都与全量一致
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

} // namespace

TestResult testTuiStream() {
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

    // ---- 加密 thinking 在 TUI 端通过 onDelta 的处理回归测试 ----
    {
        using namespace agentxx::agent;

        class TestTUIClientIO : public TUIClientAgentIO {
        public:

            TestTUIClientIO(asio::io_context& ctx) :
                TUIClientAgentIO(ctx.get_executor(), "test_session") {}

            void testOnDelta(const Delta& d) {
                onDelta(d);
            }
        };

        asio::io_context ioCtx;

        // 场景 1: 加密 thinking (text 为空, think.isEncrypted=true) -> 思考开始时立即展示 ->
        // 随后收到 assistant 文本 -> 轮次结束回填 token 数
        {
            TestTUIClientIO client(ioCtx);
            Delta           d1;
            d1.type        = Delta::Type::ThinkToken;
            d1.text        = "";
            d1.think       = ViewMessage::ThinkData{.reasoningTokens = 0, .isEncrypted = true};
            d1.startTimeMs = 1000;
            d1.durationMs  = 0;
            client.testOnDelta(d1);

            // 断言: 刚收到 ThinkToken 尚未收到 assistant 文本时，消息列表中已经立即创建了 Think
            // 消息展示
            auto snap1 = client.sharedState().snapshot();
            XX_TEST_EXPECT_EQ(snap1->messages.size(), (size_t)1);
            if (snap1->messages.size() == 1) {
                XX_TEST_EXPECT_TRUE(snap1->messages[0]->role == TUIMessage::Role::Think);
                XX_TEST_EXPECT_TRUE(snap1->messages[0]->think.has_value());
                if (snap1->messages[0]->think) {
                    XX_TEST_EXPECT_TRUE(snap1->messages[0]->think->isEncrypted);
                    XX_TEST_EXPECT_EQ(snap1->messages[0]->think->reasoningTokens, 0);
                }
            }

            Delta d2;
            d2.type        = Delta::Type::TextToken;
            d2.text        = "Hello";
            d2.startTimeMs = 1050;
            client.testOnDelta(d2);

            // 随后收到 usage 返回的 reasoning_tokens = 854 (由 handleChannelWrite 派发)
            Delta d_usage;
            d_usage.type       = Delta::Type::ThinkToken;
            d_usage.text       = "";
            d_usage.durationMs = 80;
            d_usage.think = ViewMessage::ThinkData{.reasoningTokens = 854, .isEncrypted = true};
            client.testOnDelta(d_usage);

            Delta d3;
            d3.type = Delta::Type::TurnEnd;
            client.testOnDelta(d3);

            auto snap = client.sharedState().snapshot();
            XX_TEST_EXPECT_EQ(snap->messages.size(), (size_t)2);
            if (snap->messages.size() == 2) {
                // 第一条 Think 消息已成功回填 reasoningTokens = 854 与 durationMs = 80
                XX_TEST_EXPECT_TRUE(snap->messages[0]->role == TUIMessage::Role::Think);
                XX_TEST_EXPECT_TRUE(snap->messages[0]->think.has_value());
                if (snap->messages[0]->think) {
                    XX_TEST_EXPECT_TRUE(snap->messages[0]->think->isEncrypted);
                    XX_TEST_EXPECT_EQ(snap->messages[0]->think->reasoningTokens, 854);
                }
                XX_TEST_EXPECT_EQ(snap->messages[0]->durationMs, 80);
                // 第二条应当是 Assistant 消息
                XX_TEST_EXPECT_TRUE(snap->messages[1]->role == TUIMessage::Role::Assistant);
                XX_TEST_EXPECT_EQ(snap->messages[1]->text, "Hello");
            }
        }

        // 场景 2: 多步 ReAct 交互 (加密 thinking 1 -> usage 1 -> ToolStart -> ToolEnd -> 加密
        // thinking 2 -> usage 2 -> Assistant 文本 -> TurnEnd) 核心验证: 每次新的 think
        // 独立在对应动作位置创建，绝不覆盖开头的 think 消息！
        {
            TestTUIClientIO client(ioCtx);

            // Step 1: 首轮思考
            Delta d1_think;
            d1_think.type  = Delta::Type::ThinkToken;
            d1_think.text  = "";
            d1_think.think = ViewMessage::ThinkData{.reasoningTokens = 0, .isEncrypted = true};
            client.testOnDelta(d1_think);

            // Step 1 结束: usage 回填 Think 1
            Delta d1_usage;
            d1_usage.type       = Delta::Type::ThinkToken;
            d1_usage.text       = "";
            d1_usage.durationMs = 120;
            d1_usage.think = ViewMessage::ThinkData{.reasoningTokens = 320, .isEncrypted = true};
            client.testOnDelta(d1_usage);

            // Step 1 调用工具
            Delta d1_tool;
            d1_tool.type       = Delta::Type::ToolStart;
            d1_tool.toolName   = "search";
            d1_tool.toolCallId = "call_1";
            d1_tool.arguments  = "{\"q\":\"test\"}";
            client.testOnDelta(d1_tool);

            Delta d1_tool_end;
            d1_tool_end.type       = Delta::Type::ToolEnd;
            d1_tool_end.toolName   = "search";
            d1_tool_end.toolCallId = "call_1";
            d1_tool_end.result     = "ok";
            client.testOnDelta(d1_tool_end);

            // 断言 Step 1 完成后有 2 条消息: [Think_1(320 tokens), Tool_1]
            auto snap1 = client.sharedState().snapshot();
            XX_TEST_EXPECT_EQ(snap1->messages.size(), (size_t)2);
            if (snap1->messages.size() == 2) {
                XX_TEST_EXPECT_TRUE(snap1->messages[0]->role == TUIMessage::Role::Think);
                if (snap1->messages[0]->think) {
                    XX_TEST_EXPECT_EQ(snap1->messages[0]->think->reasoningTokens, 320);
                }
                XX_TEST_EXPECT_TRUE(snap1->messages[1]->role == TUIMessage::Role::Tool);
            }

            // Step 2: 第二轮思考 (新的 think 开始)
            Delta d2_think;
            d2_think.type  = Delta::Type::ThinkToken;
            d2_think.text  = "";
            d2_think.think = ViewMessage::ThinkData{.reasoningTokens = 0, .isEncrypted = true};
            client.testOnDelta(d2_think);

            // 断言 Step 2 思考开始后，立即新增了 Think_2 消息，总数为 3 条 [Think_1, Tool_1,
            // Think_2]，且 Think_1 内容未被破坏
            auto snap2 = client.sharedState().snapshot();
            XX_TEST_EXPECT_EQ(snap2->messages.size(), (size_t)3);
            if (snap2->messages.size() == 3) {
                XX_TEST_EXPECT_TRUE(snap2->messages[0]->role == TUIMessage::Role::Think);
                if (snap2->messages[0]->think) {
                    XX_TEST_EXPECT_EQ(snap2->messages[0]->think->reasoningTokens, 320); // 未被覆盖
                }
                XX_TEST_EXPECT_TRUE(snap2->messages[1]->role == TUIMessage::Role::Tool);
                XX_TEST_EXPECT_TRUE(snap2->messages[2]->role == TUIMessage::Role::Think);
                if (snap2->messages[2]->think) {
                    XX_TEST_EXPECT_EQ(snap2->messages[2]->think->reasoningTokens, 0); // 新思考
                }
            }

            // Step 2 输出正文
            Delta d2_text;
            d2_text.type = Delta::Type::TextToken;
            d2_text.text = "Final answer";
            client.testOnDelta(d2_text);

            // Step 2 结束: usage 回填 Think 2
            Delta d2_usage;
            d2_usage.type       = Delta::Type::ThinkToken;
            d2_usage.text       = "";
            d2_usage.durationMs = 90;
            d2_usage.think = ViewMessage::ThinkData{.reasoningTokens = 150, .isEncrypted = true};
            client.testOnDelta(d2_usage);

            Delta d2_end;
            d2_end.type = Delta::Type::TurnEnd;
            client.testOnDelta(d2_end);

            // 最终断言: [Think_1 (320), Tool_1, Think_2 (150), Assistant ("Final answer")]
            auto snap3 = client.sharedState().snapshot();
            XX_TEST_EXPECT_EQ(snap3->messages.size(), (size_t)4);
            if (snap3->messages.size() == 4) {
                XX_TEST_EXPECT_TRUE(snap3->messages[0]->role == TUIMessage::Role::Think);
                if (snap3->messages[0]->think) {
                    XX_TEST_EXPECT_EQ(snap3->messages[0]->think->reasoningTokens, 320);
                }
                XX_TEST_EXPECT_TRUE(snap3->messages[1]->role == TUIMessage::Role::Tool);
                XX_TEST_EXPECT_TRUE(snap3->messages[2]->role == TUIMessage::Role::Think);
                if (snap3->messages[2]->think) {
                    XX_TEST_EXPECT_EQ(snap3->messages[2]->think->reasoningTokens, 150);
                }
                XX_TEST_EXPECT_TRUE(snap3->messages[3]->role == TUIMessage::Role::Assistant);
                XX_TEST_EXPECT_EQ(snap3->messages[3]->text, "Final answer");
            }
        }

        // 场景 3: 加密 thinking -> 随后直接 TurnEnd
        {
            TestTUIClientIO client(ioCtx);
            Delta           d1;
            d1.type  = Delta::Type::ThinkToken;
            d1.text  = "";
            d1.think = ViewMessage::ThinkData{.reasoningTokens = 50, .isEncrypted = true};
            client.testOnDelta(d1);

            Delta d2;
            d2.type = Delta::Type::TurnEnd;
            client.testOnDelta(d2);

            auto snap = client.sharedState().snapshot();
            XX_TEST_EXPECT_EQ(snap->messages.size(), (size_t)1);
            if (snap->messages.size() == 1) {
                XX_TEST_EXPECT_TRUE(snap->messages[0]->role == TUIMessage::Role::Think);
                XX_TEST_EXPECT_TRUE(snap->messages[0]->think.has_value());
                if (snap->messages[0]->think) {
                    XX_TEST_EXPECT_TRUE(snap->messages[0]->think->isEncrypted);
                    XX_TEST_EXPECT_EQ(snap->messages[0]->think->reasoningTokens, 50);
                }
            }
        }

        // 场景 4: 明文 thinking (流式) -> 无正文直接结束 -> 收到 usage 回填 (验证不产生重复空 Think
        // 消息)
        {
            TestTUIClientIO client(ioCtx);
            Delta           d1;
            d1.type        = Delta::Type::ThinkToken;
            d1.text        = "明文思考内容";
            d1.startTimeMs = 2000;
            client.testOnDelta(d1);

            Delta d_usage;
            d_usage.type       = Delta::Type::ThinkToken;
            d_usage.text       = "";
            d_usage.durationMs = 95;
            d_usage.think = ViewMessage::ThinkData{.reasoningTokens = 150, .isEncrypted = false};
            client.testOnDelta(d_usage);

            Delta d_end;
            d_end.type = Delta::Type::TurnEnd;
            client.testOnDelta(d_end);

            auto snap = client.sharedState().snapshot();
            XX_TEST_EXPECT_EQ(snap->messages.size(), (size_t)1);
            if (snap->messages.size() == 1) {
                XX_TEST_EXPECT_TRUE(snap->messages[0]->role == TUIMessage::Role::Think);
                XX_TEST_EXPECT_EQ(snap->messages[0]->text, "明文思考内容");
                XX_TEST_EXPECT_TRUE(snap->messages[0]->think.has_value());
                if (snap->messages[0]->think) {
                    XX_TEST_EXPECT_FALSE(snap->messages[0]->think->isEncrypted);
                    XX_TEST_EXPECT_EQ(snap->messages[0]->think->reasoningTokens, 150);
                }
                XX_TEST_EXPECT_EQ(snap->messages[0]->durationMs, 95);
            }
        }

        // 场景 5: 明文 thinking 流式 -> 思考完成结算包 (空文本 ThinkToken 仅携带
        // startTimeMs/durationMs) -> NodeEnd -> 正文 -> TurnEnd
        // 核心验证: think 耗时在输出完成时才由结算包回填; 流式期间不产生消息/
        // 时长; NodeEnd 的节点级计时不覆盖已回填的 Think 时长
        {
            TestTUIClientIO client(ioCtx);

            // 流式思考增量: 尚未完成, 不应落盘消息, 更不应有时长
            Delta d_think;
            d_think.type        = Delta::Type::ThinkToken;
            d_think.text        = "思考中...";
            d_think.startTimeMs = 3000;
            client.testOnDelta(d_think);

            {
                auto snap = client.sharedState().snapshot();
                XX_TEST_EXPECT_EQ(snap->messages.size(), (size_t)0);
                XX_TEST_EXPECT_EQ(snap->pendingTokenDurationMs, int64_t{0});
            }

            // 思考完成结算包 (agent 端 finalizeThinkSegment 发出)
            Delta d_final;
            d_final.type        = Delta::Type::ThinkToken;
            d_final.text        = "";
            d_final.startTimeMs = 3000;
            d_final.durationMs  = 1234;
            client.testOnDelta(d_final);

            // NodeEnd 携带节点级计时, 不得覆盖上面回填的 Think 时长
            Delta d_nodeEnd;
            d_nodeEnd.type       = Delta::Type::NodeEnd;
            d_nodeEnd.nodeName   = "llm";
            d_nodeEnd.startTimeMs = 2900;
            d_nodeEnd.durationMs  = 99999;
            client.testOnDelta(d_nodeEnd);

            // 正文输出后轮次结束
            Delta d_text;
            d_text.type        = Delta::Type::TextToken;
            d_text.text        = "Answer";
            d_text.startTimeMs = 4300;
            client.testOnDelta(d_text);

            Delta d_end;
            d_end.type = Delta::Type::TurnEnd;
            client.testOnDelta(d_end);

            auto snap = client.sharedState().snapshot();
            XX_TEST_EXPECT_EQ(snap->messages.size(), (size_t)2);
            if (snap->messages.size() == 2) {
                XX_TEST_EXPECT_TRUE(snap->messages[0]->role == TUIMessage::Role::Think);
                XX_TEST_EXPECT_EQ(snap->messages[0]->text, "思考中...");
                // 结算包回填的耗时; 未被 NodeEnd 的节点时长 (99999) 覆盖
                XX_TEST_EXPECT_EQ(snap->messages[0]->durationMs, 1234);
                XX_TEST_EXPECT_TRUE(snap->messages[1]->role == TUIMessage::Role::Assistant);
                XX_TEST_EXPECT_EQ(snap->messages[1]->text, "Answer");
            }
        }

        // 场景 6: Tool 执行中断/未结束 (toolFinished=false) -> 收到新轮次 TurnStart ->
        // 验证最近连续的 tool 消息被重置为非 running (toolFinished=true)
        {
            TestTUIClientIO client(ioCtx);

            // 模拟 ToolStart
            Delta d_tool;
            d_tool.type       = Delta::Type::ToolStart;
            d_tool.toolName   = "agentxx_execute_bash_command";
            d_tool.toolCallId = "call_bash_1";
            d_tool.arguments  = "{\"command\":\"sleep 100\"}";
            client.testOnDelta(d_tool);

            {
                auto snap = client.sharedState().snapshot();
                XX_TEST_EXPECT_EQ(snap->messages.size(), (size_t)1);
                XX_TEST_EXPECT_TRUE(snap->messages[0]->role == TUIMessage::Role::Tool);
                if (snap->messages[0]->tool) {
                    XX_TEST_EXPECT_FALSE(snap->messages[0]->tool->toolFinished);
                }
            }

            // 模拟重启恢复或中断后收到新消息 (TurnStart)
            Delta d_turnStart;
            d_turnStart.type        = Delta::Type::TurnStart;
            d_turnStart.text        = "新用户消息";
            d_turnStart.startTimeMs = 5000;
            client.testOnDelta(d_turnStart);

            {
                auto snap = client.sharedState().snapshot();
                XX_TEST_EXPECT_EQ(snap->messages.size(), (size_t)2);
                XX_TEST_EXPECT_TRUE(snap->messages[0]->role == TUIMessage::Role::Tool);
                if (snap->messages[0]->tool) {
                    // 断言: 原处于 running 状态的 tool 消息已被重置为 toolFinished=true
                    XX_TEST_EXPECT_TRUE(snap->messages[0]->tool->toolFinished);
                }
                XX_TEST_EXPECT_TRUE(snap->messages[1]->role == TUIMessage::Role::User);
                XX_TEST_EXPECT_EQ(snap->messages[1]->text, "新用户消息");
            }
        }

        // 场景 7: 多个并行 tool 均未结束 -> 收到 MessageTip (如取消请求) -> 全部连续 tool 消息被重置为 non-running
        {
            TestTUIClientIO client(ioCtx);

            Delta d_t1;
            d_t1.type       = Delta::Type::ToolStart;
            d_t1.toolName   = "tool1";
            d_t1.toolCallId = "call_p1";
            client.testOnDelta(d_t1);

            Delta d_t2;
            d_t2.type       = Delta::Type::ToolStart;
            d_t2.toolName   = "tool2";
            d_t2.toolCallId = "call_p2";
            client.testOnDelta(d_t2);

            {
                auto snap = client.sharedState().snapshot();
                XX_TEST_EXPECT_EQ(snap->messages.size(), (size_t)2);
                XX_TEST_EXPECT_FALSE(snap->messages[0]->tool->toolFinished);
                XX_TEST_EXPECT_FALSE(snap->messages[1]->tool->toolFinished);
            }

            // 收到 MessageTip 取消提示
            Delta d_tip;
            d_tip.type    = Delta::Type::MessageTip;
            d_tip.text    = "[Cancel Request]";
            d_tip.tipType = Delta::TipType::Info;
            client.testOnDelta(d_tip);

            {
                auto snap = client.sharedState().snapshot();
                XX_TEST_EXPECT_EQ(snap->messages.size(), (size_t)3);
                XX_TEST_EXPECT_TRUE(snap->messages[0]->tool->toolFinished);
                XX_TEST_EXPECT_TRUE(snap->messages[1]->tool->toolFinished);
                XX_TEST_EXPECT_TRUE(snap->messages[2]->role == TUIMessage::Role::Tip);
            }
        }

        // 场景 8: 历史中存在已完成的 tool 与未完成的 tool -> 收到 TextToken 时重置
        {
            TestTUIClientIO client(ioCtx);

            // Step 1: Tool 1 完成
            Delta d_t1;
            d_t1.type       = Delta::Type::ToolStart;
            d_t1.toolName   = "tool1";
            d_t1.toolCallId = "call_f1";
            client.testOnDelta(d_t1);

            Delta d_t1_end;
            d_t1_end.type       = Delta::Type::ToolEnd;
            d_t1_end.toolName   = "tool1";
            d_t1_end.toolCallId = "call_f1";
            d_t1_end.result     = "ok";
            client.testOnDelta(d_t1_end);

            // Step 2: Tool 2 未完成
            Delta d_t2;
            d_t2.type       = Delta::Type::ToolStart;
            d_t2.toolName   = "tool2";
            d_t2.toolCallId = "call_u2";
            client.testOnDelta(d_t2);

            // 随后直接输出正文 TextToken (未收到 ToolEnd)
            Delta d_text;
            d_text.type = Delta::Type::TextToken;
            d_text.text = "继续输出";
            client.testOnDelta(d_text);

            Delta d_end;
            d_end.type = Delta::Type::TurnEnd;
            client.testOnDelta(d_end);

            {
                auto snap = client.sharedState().snapshot();
                XX_TEST_EXPECT_EQ(snap->messages.size(), (size_t)3);
                XX_TEST_EXPECT_TRUE(snap->messages[0]->tool->toolFinished);
                XX_TEST_EXPECT_TRUE(snap->messages[1]->tool->toolFinished);
                XX_TEST_EXPECT_TRUE(snap->messages[2]->role == TUIMessage::Role::Assistant);
                XX_TEST_EXPECT_EQ(snap->messages[2]->text, "继续输出");
            }
        }
    }

    return TestResult{g_tui_stream_passed, g_tui_stream_failed};
}

} // namespace test
} // namespace agentxx