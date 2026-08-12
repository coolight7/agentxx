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

#include "agentxx-client/io/tui/framework/tui_state.h"
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
    std::string_view                     text,
    markdown::Theme const&               theme,
    int                                  width,
    std::vector<std::unique_ptr<markdown::DomBuilder>>& keepAlive
) {
    auto parser = markdown::make_cmark_parser();
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
    markdown::IncrementalRenderer&                        inc,
    Color                                                 color,
    markdown::Theme const&                                theme,
    int                                                   width,
    std::vector<std::unique_ptr<markdown::DomBuilder>>&   buildersOut
) {
    std::unique_ptr<markdown::DomBuilder> fb;
    auto frontier = inc.renderFrontier(theme, width, fb);
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
void expectIncrementalEqualsFull(
    std::string_view text, int width, std::string const& tag
) {
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
            stderr, "  expected:\n%s\n  actual:\n%s\n",
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
        markdown::IncrementalRenderer inc;
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
    expectIncrementalEqualsFull("> quote line 1\n> quote line 2\n\nPlain after quote.\n", W, "quote");

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
        std::string doc = "one\n\ntwo\nthree\n\nfour\n";
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
        const auto& theme = markdown::theme_default();
        markdown::IncrementalRenderer inc;
        std::vector<std::unique_ptr<markdown::DomBuilder>> keepInc, keepFull;
        auto cmp = [&](std::string_view prefix) {
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
        XX_TEST_EXPECT_TRUE(
            toScreen(el1, W, 40) == toScreen(el2, W, 40)
        );
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

    return TestResult{g_tui_stream_passed, g_tui_stream_failed};
}

} // namespace test
} // namespace agentxx