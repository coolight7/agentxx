// markdown 块渲染测试 (agentxx::client::renderMarkdown / estimateMarkdownLines)
//
// 背景: 代码块 (``` 围栏) 的每行代码原先只生成一个 ftxui::text 元素, 而 FTXUI
// 对超出盒宽的元素是**裁剪**而不是折行 —— 长代码行只显示前半截, 尾部内容
// 完全看不到 (用户报告"代码段不会自动换行, 被裁剪不显示")。
//
// 覆盖点:
// - [折行不丢内容] 超宽代码行按可用宽度折成多行, 拼接后与源码完全一致
// - [尾部可见] 长行末尾的内容真的画到了屏幕上 (不被右缘裁剪)
// - [缩进扣除] 块引用前缀与代码块自身左右内边距都从折行宽度里扣除
// - [宽字符] CJK 等双宽字符按显示宽度折行, 不拆开多字节字符
// - [高度估算] estimateMarkdownLines 与真实布局高度一致 (低估会导致滚动偏移
//   偏小, 底部内容被推出视口)
// - [默认行为] 未限制宽度时不折行
#include "agentxx-test/client/test_markdown_block.h"

#include "agentxx-client/io/tui/markdown_block.h"
#include "agentxx-client/io/tui/scroll_common.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <algorithm>
#include <markdown/text_utils.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_markdown_block_passed = 0;
int g_markdown_block_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_markdown_block_passed
#define XX_TEST_FAILED g_markdown_block_failed

namespace agentxx {
namespace test {

using namespace agentxx::client;

namespace {

/// 消息正文/思考内容使用的 markdown 主题
markdown::Theme const& mdTheme() {
    static const TUITheme t = TUITheme::darkTheme();
    return t.markdownTheme;
}

/// 渲染 markdown 到屏幕并取回逐行文本 (行数 = h)
///
/// - [content] markdown 文本
/// - [wrapWidth] 传给 renderMarkdown 的宽度上限 (<=0 表示不限制)
/// - [w] / [h] 画布尺寸
std::vector<std::string> renderRows(std::string_view content, int wrapWidth, int w, int h) {
    std::vector<std::unique_ptr<markdown::DomBuilder>> keep;
    auto [el, builder] = renderMarkdown(content, ftxui::Color::White, mdTheme(), wrapWidth);
    if (builder) {
        keep.push_back(std::move(builder));
    }
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, el);

    std::vector<std::string> rows;
    rows.reserve(static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        std::string line;
        // 逐格拼成"显示宽度 = w"的文本: 未写入的格补空格 (保持列号对应),
        // 双宽字符的第二格跳过 (该字符自身已占 2 列)
        bool previousFullwidth = false;
        for (int x = 0; x < w; ++x) {
            const std::string& ch = screen.PixelAt(x, y).character;
            if (!previousFullwidth) {
                line += ch.empty() ? std::string{" "} : ch;
            }
            previousFullwidth = !ch.empty() && markdown::utf8_display_width(ch) == 2;
        }
        rows.push_back(std::move(line));
    }
    return rows;
}

/// 取一行的 [from, to) 显示列区间文本 (与屏幕列对应)
std::string sliceCols(const std::string& row, int from, int to) {
    // 行文本由 renderRows 跳过空串格拼接而来, 与屏幕列不总是一一对应
    // (宽字符占 2 列但只产生 1 个字符), 故这里按显示宽度重新切分
    std::string out;
    int         col = 0;
    size_t      i   = 0;
    while (i < row.size()) {
        const size_t len = std::min(markdown::utf8_byte_length(row[i]), row.size() - i);
        const int    cw
            = markdown::codepoint_width(markdown::utf8_codepoint(row.data() + i, len));
        if (col >= from && col < to) {
            out.append(row, i, len);
        }
        col += std::max(cw, 0);
        i   += len;
    }
    return out;
}

/// 去行尾空白 (折行后每行尾部由 filler 补的空格)
std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

/// 渲染 markdown (宽度上限 wrapWidth) 并测量该布局宽度下的真实高度 (行)
int renderHeight(std::string_view content, int wrapWidth, int boxWidth) {
    std::vector<std::unique_ptr<markdown::DomBuilder>> keep;
    auto [el, builder] = renderMarkdown(content, ftxui::Color::White, mdTheme(), wrapWidth);
    if (builder) {
        keep.push_back(std::move(builder));
    }
    return layoutAndMeasure(el, ftxui::Box{0, boxWidth - 1, 0, kTallHeight});
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

/// 拼接若干行的同一列区间 (代码块折行后的内容还原)
std::string joinCols(
    const std::vector<std::string>& rows,
    int                             firstRow,
    int                             lastRow,
    int                             from,
    int                             to
) {
    std::string out;
    for (int y = firstRow; y <= lastRow; ++y) {
        out += rtrim(sliceCols(rows[static_cast<size_t>(y)], from, to));
    }
    return out;
}

} // namespace

TestResult testMarkdownBlock() {
    // ---------------- 超宽代码行折行 (内容不丢失) ----------------
    {
        constexpr int     W    = 40;    // 总宽度
        constexpr int     KCol = W - 2; // 代码块内容可用列数 (左右各 1 列内边距)
        const std::string longLine
            = std::string(4, ' ') + std::string(90, 'x') + "_TAILMARK"; // 104 列
        const std::string content = "```\nshort_1\n" + longLine + "\nshort_2\n```\n";

        // 折行: 104 列 / 38 列 = 3 行
        XX_TEST_EXPECT_EQ(markdown::wrap_line_by_width(longLine, KCol).size(), size_t{3});
        // 元素高度 = 5 行代码内容 + 上下各 1 行内边距
        XX_TEST_EXPECT_EQ(renderHeight(content, W, W), 5 + 2);

        auto        rows = renderRows(content, W, W, 12);
        std::string code = joinCols(rows, 1, 5, 1, W - 1);
        XX_TEST_EXPECT_EQ(code, std::string{"short_1"} + longLine + "short_2");

        // 行尾标记真的画出来了 (修复前超宽行只剩前 38 列, 该标记不可见)
        bool tailVisible = false;
        for (const auto& row : rows) {
            tailVisible = tailVisible || contains(row, "_TAILMARK");
        }
        XX_TEST_EXPECT_TRUE(tailVisible);
        // 下内边距行之后没有内容
        XX_TEST_EXPECT_TRUE(rtrim(sliceCols(rows[6], 0, W)).empty());

        // 高度估算与实测一致 (低估会导致总高度偏小 -> 滚动偏移偏小,
        // 底部内容被推出视口)
        XX_TEST_EXPECT_EQ(
            estimateMarkdownLines(content, W),
            static_cast<size_t>(renderHeight(content, W, W))
        );
    }

    // ---------------- 不超宽的代码行保持原样 ----------------
    {
        const std::string content = "```\nline_1\nline_2\n```\n";
        XX_TEST_EXPECT_EQ(renderHeight(content, 40, 40), 2 + 2);
        XX_TEST_EXPECT_EQ(estimateMarkdownLines(content, 40), size_t{4});
        auto rows = renderRows(content, 40, 40, 8);
        XX_TEST_EXPECT_EQ(joinCols(rows, 1, 2, 1, 39), std::string{"line_1line_2"});
    }

    // ---------------- 未限制宽度时不折行 (默认行为不变) ----------------
    {
        const std::string longLine = std::string(90, 'y') + "_NOLIMIT";
        const std::string content  = "```\n" + longLine + "\n```\n";
        // wrapWidth <= 0: 代码行不折行 (高度 = 1 内容行 + 上下内边距)
        XX_TEST_EXPECT_EQ(renderHeight(content, 0, 200), 1 + 2);
    }

    // ---------------- 块引用内的代码块: 扣掉引用前缀列数 ----------------
    {
        constexpr int     W    = 40;
        constexpr int     KCol = W - 2 - 2; // 引用前缀 "│ " 占 2 列
        const std::string quotedLine = std::string(80, 'q') + "_QUOTETAIL"; // 90 列
        const std::string content    = "> ```\n> " + quotedLine + "\n> ```\n";

        // 折行: 90 列 / 36 列 = 3 行
        XX_TEST_EXPECT_EQ(markdown::wrap_line_by_width(quotedLine, KCol).size(), size_t{3});
        XX_TEST_EXPECT_EQ(renderHeight(content, W, W), 3 + 2);

        auto rows = renderRows(content, W, W, 10);
        // 内容从第 3 列起 (引用前缀 2 列 + 代码块左内边距 1 列), 到第 38 列止
        XX_TEST_EXPECT_EQ(joinCols(rows, 1, 3, 3, W - 1), quotedLine);

        bool tailVisible = false;
        for (const auto& row : rows) {
            tailVisible = tailVisible || contains(row, "_QUOTETAIL");
        }
        XX_TEST_EXPECT_TRUE(tailVisible);
    }

    // ---------------- 宽字符 (CJK) 折行: 不拆开多字节字符 ----------------
    {
        constexpr int W = 40;
        std::string   cjkLine;
        for (int i = 0; i < 30; ++i) {
            cjkLine += "汉"; // 每个字符 2 列, 共 60 列
        }
        const std::string content = "```\n" + cjkLine + "\n```\n";

        // 60 列 / 38 列 = 2 行 (19 字符 = 38 列, 恰好铺满一行)
        XX_TEST_EXPECT_EQ(markdown::wrap_line_by_width(cjkLine, W - 2).size(), size_t{2});
        XX_TEST_EXPECT_EQ(renderHeight(content, W, W), 2 + 2);

        auto        rows = renderRows(content, W, W, 8);
        std::string code = joinCols(rows, 1, 2, 1, W - 1);
        XX_TEST_EXPECT_EQ(code, cjkLine); // 拼接后与原文一致 (字符未被截断)
    }

    // ---------------- 表格同样扣除引用缩进 (不超出容器右缘) ----------------
    {
        constexpr int W = 40;
        // 单元格自然宽度合计超出可用宽度: 表格按可用列数压缩并折行;
        // 引用前缀 2 列必须从可用宽度里扣除, 否则右侧边框会被裁剪
        const std::string content
            = "> | h1 | h2 |\n"
              "> | --- | --- |\n"
              "> | 0123456789012345678901234567890123 | x |\n";
        auto rows      = renderRows(content, W, W, 12);
        int  topBorder = -1;
        for (size_t y = 0; y < rows.size(); ++y) {
            if (contains(rows[y], "\u250C")) { // ┌
                topBorder = static_cast<int>(y);
                break;
            }
        }
        XX_TEST_EXPECT_TRUE(topBorder >= 0);
        if (topBorder >= 0) {
            const auto& row = rows[static_cast<size_t>(topBorder)];
            // 右边框 "┐" 落在最后一列 (可见), 行首仍是引用前缀 "│"
            XX_TEST_EXPECT_EQ(sliceCols(row, W - 1, W), std::string{"\u2510"});
            XX_TEST_EXPECT_EQ(sliceCols(row, 0, 1), std::string{"\u2502"});
            XX_TEST_EXPECT_TRUE(contains(row, "\u252C")); // ┬ 列分隔符仍在
        }
    }

    return TestResult{g_markdown_block_passed, g_markdown_block_failed};
}

} // namespace test
} // namespace agentxx
