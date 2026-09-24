// 自绘折行文本节点测试 (markdown::FlowText)
//
// 背景: markdown 正文原先按"每个词一个 ftxui::text 元素 + flexbox"组装, 一条
// 消息几百个节点, 折行结果每帧重算。FlowText 用单个节点承载整段文本, 折行结果
// 按宽度缓存 (宽度不变时重排几乎无成本)。
//
// 覆盖点:
// - [折行] 空格为词边界, 行首空格丢弃; 高度 = 折行行数 (与渲染一致)
// - [缓存] 同宽度重复布局行数/行宽稳定; 宽度变化后重新折行且高度更新
// - [硬拆] 超宽单词按列硬拆 (不丢字符); 宽字符 2 列; 组合字符并入前一格
// - [硬换行] '\n' 分段, 尾随换行保留一行空行
// - [行内样式] 粗体/暗色/斜体/下划线/颜色落到正确单元格; 未设置的通道继承
//   外层装饰器颜色
// - [选择] 行区间 + 列区间取文本 (含跨行、宽字符、组合字符)
// - [链接区段] 链接的可见区段写进登记目标 (供鼠标点击命中)
#include "agentxx-test/client/test_markdown_flow.h"

#include "agentxx-client/io/tui/scroll_common.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/selection.hpp"
#include "ftxui/screen/screen.hpp"
#include "markdown/flow.hpp"
#include "markdown/incremental.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_markdown_flow_passed = 0;
int g_markdown_flow_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_markdown_flow_passed
#define XX_TEST_FAILED g_markdown_flow_failed

namespace agentxx {
namespace test {

using namespace agentxx::client;

namespace {

/// 构造纯文本折行节点
std::shared_ptr<markdown::FlowText> plainNode(std::string_view content) {
    return markdown::FlowText::plain(content);
}

/// 按宽度布局一次并返回实测高度 (与消息列表的测量口径一致)
int layoutHeight(const ftxui::Element& el, int width) {
    return layoutAndMeasure(el, ftxui::Box{0, width - 1, 0, kTallHeight});
}

/// 渲染到 [w, h] 画布, 逐行拼成文本 (宽字符的占位格跳过, 行尾空白去掉)
std::vector<std::string> renderRows(const ftxui::Element& el, int w, int h) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, el);
    std::vector<std::string> rows;
    rows.reserve(static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        std::string row;
        bool        previous_fullwidth = false;
        for (int x = 0; x < w; ++x) {
            const std::string& ch = screen.PixelAt(x, y).character;
            if (!previous_fullwidth) {
                row += ch.empty() ? std::string{" "} : ch;
            }
            previous_fullwidth = !ch.empty() && markdown::utf8_display_width(ch) == 2;
        }
        while (!row.empty() && row.back() == ' ') {
            row.pop_back();
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

/// 按 FTXUI 拖选路径取回 [x0, y0] - [x1, y1] 区域内的文本
std::string selectText(const ftxui::Element& el, int w, int h, int x0, int y0, int x1, int y1) {
    el->ComputeRequirement();
    el->SetBox(ftxui::Box{0, w - 1, 0, h - 1});
    ftxui::Selection selection(x0, y0, x1, y1);
    el->Select(selection);
    return selection.GetParts();
}

} // namespace

TestResult testMarkdownFlow() {
    // ---------------- 折行与高度 ----------------
    {
        auto flow = plainNode("aaa bbb ccc");
        XX_TEST_EXPECT_EQ(layoutHeight(flow, 7), 2); // "aaa bbb" + "ccc"
        XX_TEST_EXPECT_EQ(flow->lineCount(), size_t{2});
        XX_TEST_EXPECT_EQ(flow->lineWidth(0), 7);
        XX_TEST_EXPECT_EQ(flow->lineWidth(1), 3);
        XX_TEST_EXPECT_EQ(flow->wrappedWidth(), 7);

        // 宽度够宽: 一行; 折行宽度记录随之更新
        XX_TEST_EXPECT_EQ(layoutHeight(flow, 100), 1);
        XX_TEST_EXPECT_EQ(flow->lineWidth(0), 11);
        XX_TEST_EXPECT_EQ(flow->wrappedWidth(), 100);

        // 收窄: 重新折行
        XX_TEST_EXPECT_EQ(layoutHeight(flow, 4), 3); // "aaa" "bbb" "ccc"
        XX_TEST_EXPECT_EQ(flow->lineCount(), size_t{3});
        XX_TEST_EXPECT_EQ(flow->lineWidth(0), 3);
    }

    // ---------------- 同宽度重复布局稳定 (折行缓存) ----------------
    {
        auto flow = plainNode("the quick brown fox jumps over the lazy dog");
        const int first = layoutHeight(flow, 17);
        XX_TEST_EXPECT_TRUE(first > 1);
        // 重复布局 (盒位置变化 / 多次测量) 结果不变, 且不重新折行
        const int wrapped = flow->wrappedWidth();
        auto      el      = std::static_pointer_cast<ftxui::Node>(flow);
        el->SetBox(ftxui::Box{0, 16, 5, 5 + kTallHeight});
        el->SetBox(ftxui::Box{0, 16, 9, 9 + kTallHeight});
        XX_TEST_EXPECT_EQ(flow->wrappedWidth(), wrapped);
        XX_TEST_EXPECT_EQ(static_cast<int>(flow->lineCount()), first);
    }

    // ---------------- 硬换行与空文本 ----------------
    {
        auto flow = plainNode("alpha\nbeta");
        XX_TEST_EXPECT_EQ(layoutHeight(flow, 40), 2);
        XX_TEST_EXPECT_EQ(flow->sourceLineCount(), size_t{2});
        // 尾随换行保留一行空行
        auto trailing = plainNode("alpha\n");
        XX_TEST_EXPECT_EQ(layoutHeight(trailing, 40), 2);
        XX_TEST_EXPECT_EQ(trailing->lineWidth(1), 0);
        // 空文本占一行
        auto empty = plainNode("");
        XX_TEST_EXPECT_EQ(layoutHeight(empty, 40), 1);
        XX_TEST_EXPECT_EQ(empty->lineWidth(0), 0);
    }

    // ---------------- 超宽单词按列硬拆 ----------------
    {
        auto flow = plainNode("xxxxxxxxxx");
        XX_TEST_EXPECT_EQ(layoutHeight(flow, 4), 3); // 4 + 4 + 2
        XX_TEST_EXPECT_EQ(flow->lineWidth(0), 4);
        XX_TEST_EXPECT_EQ(flow->lineWidth(2), 2);
        auto rows = renderRows(flow, 4, 3);
        XX_TEST_EXPECT_EQ(rows[0], std::string{"xxxx"});
        XX_TEST_EXPECT_EQ(rows[1], std::string{"xxxx"});
        XX_TEST_EXPECT_EQ(rows[2], std::string{"xx"});
    }

    // ---------------- 宽字符 (两列) 与组合字符 (并入前一格) ----------------
    {
        // 5 个汉字 = 10 列, 宽 4 -> 2 + 2 + 1 个汉字
        auto flow = plainNode("汉字宽字符");
        XX_TEST_EXPECT_EQ(layoutHeight(flow, 4), 3);
        XX_TEST_EXPECT_EQ(flow->lineWidth(0), 4);
        XX_TEST_EXPECT_EQ(flow->lineWidth(2), 2);
        auto rows = renderRows(flow, 4, 3);
        XX_TEST_EXPECT_EQ(rows[0], std::string{"汉字"});
        XX_TEST_EXPECT_EQ(rows[2], std::string{"符"});

        // 组合字符不占列: "e" + U+0301 (合成 é)
        auto combining = plainNode("e\xCC\x81");
        XX_TEST_EXPECT_EQ(layoutHeight(combining, 1), 1);
        XX_TEST_EXPECT_EQ(combining->lineWidth(0), 1);
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(1), ftxui::Dimension::Fixed(1));
        ftxui::Render(screen, combining);
        XX_TEST_EXPECT_EQ(screen.PixelAt(0, 0).character, std::string{"e\xCC\x81"});
    }

    // ---------------- 渲染与折行一致 ----------------
    {
        auto flow = plainNode("aaa bbb ccc");
        auto rows = renderRows(flow, 7, 4);
        XX_TEST_EXPECT_EQ(rows[0], std::string{"aaa bbb"});
        XX_TEST_EXPECT_EQ(rows[1], std::string{"ccc"});
        XX_TEST_EXPECT_EQ(rows[2], std::string{});
        // 行首空格丢弃
        auto lead = plainNode("aaaaaa bbb");
        auto rows2 = renderRows(lead, 6, 3);
        XX_TEST_EXPECT_EQ(rows2[0], std::string{"aaaaaa"});
        XX_TEST_EXPECT_EQ(rows2[1], std::string{"bbb"});
    }

    // ---------------- 行内样式落到正确单元格 ----------------
    {
        std::vector<markdown::Span> spans(3);
        spans[0].text             = "bold";
        spans[0].style.bold       = true;
        spans[1].text             = "-red";
        spans[1].style.fg         = ftxui::Color::Red;
        spans[2].text             = "-italic";
        spans[2].style.italic     = true;
        auto flow = std::make_shared<markdown::FlowText>(std::move(spans));
        auto el   = std::static_pointer_cast<ftxui::Node>(flow) | ftxui::color(ftxui::Color::Blue);

        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(14), ftxui::Dimension::Fixed(1));
        ftxui::Render(screen, el);
        // "bold-red-italic"
        for (int x = 0; x < 4; ++x) {
            XX_TEST_EXPECT_TRUE(screen.PixelAt(x, 0).bold);
            XX_TEST_EXPECT_TRUE(screen.PixelAt(x, 0).foreground_color == ftxui::Color::Blue);
        }
        XX_TEST_EXPECT_TRUE(screen.PixelAt(4, 0).foreground_color == ftxui::Color::Red);
        for (int x = 4; x < 8; ++x) {
            XX_TEST_EXPECT_TRUE(screen.PixelAt(x, 0).foreground_color == ftxui::Color::Red);
        }
        XX_TEST_EXPECT_TRUE(screen.PixelAt(8, 0).foreground_color == ftxui::Color::Blue);
        for (int x = 9; x < 14; ++x) {
            XX_TEST_EXPECT_TRUE(screen.PixelAt(x, 0).italic);
        }
    }

    // ---------------- 选择取文本 ----------------
    {
        auto flow = plainNode("hello world");
        XX_TEST_EXPECT_EQ(selectText(flow, 11, 1, 0, 0, 4, 0), std::string{"hello"});
        XX_TEST_EXPECT_EQ(selectText(flow, 11, 1, 6, 0, 10, 0), std::string{"world"});
        // 词间空格在选区内按空格取出
        XX_TEST_EXPECT_EQ(selectText(flow, 11, 1, 0, 0, 10, 0), std::string{"hello world"});

        // 跨行折行后的选择: 第 2 行只取 "bbb"
        auto wrapped = plainNode("aaa bbb");
        XX_TEST_EXPECT_EQ(selectText(wrapped, 3, 3, 0, 0, 2, 0), std::string{"aaa"});
        XX_TEST_EXPECT_EQ(selectText(wrapped, 3, 3, 0, 1, 2, 1), std::string{"bbb"});
        XX_TEST_EXPECT_EQ(selectText(wrapped, 3, 3, 0, 0, 2, 1), std::string{"aaa\nbbb"});

        // 宽字符: 保留列不产生字符
        auto wide = plainNode("汉字");
        XX_TEST_EXPECT_EQ(selectText(wide, 4, 1, 0, 0, 0, 0), std::string{"汉"});
        XX_TEST_EXPECT_EQ(selectText(wide, 4, 1, 2, 0, 3, 0), std::string{"字"});
        XX_TEST_EXPECT_EQ(selectText(wide, 4, 1, 0, 0, 3, 0), std::string{"汉字"});

        // 组合字符随其修饰的字符一起被选中
        auto combining = plainNode("e\xCC\x81x");
        XX_TEST_EXPECT_EQ(selectText(combining, 2, 1, 0, 0, 0, 0), std::string{"e\xCC\x81"});
        XX_TEST_EXPECT_EQ(selectText(combining, 2, 1, 1, 0, 1, 0), std::string{"x"});
    }

    // ---------------- 链接可见区段登记 ----------------
    {
        std::vector<markdown::Span> spans(3);
        spans[0].text = "go ";
        spans[0].link = 0;
        spans[1].text = "here now";
        spans[1].link = 0;
        spans[2].text = " tail";
        auto flow     = std::make_shared<markdown::FlowText>(std::move(spans));
        const auto el = std::static_pointer_cast<ftxui::Node>(flow);

        el->ComputeRequirement();
        el->SetBox(ftxui::Box{0, 12, 0, 0});
        // "go here now" 占 0..10 列 (链接 0 覆盖 0..10, 相邻片段合并为一个盒子),
        // " tail" 从第 11 列起且不属于链接
        {
            const auto& boxes = flow->linkBoxes();
            XX_TEST_EXPECT_EQ(boxes.size(), size_t{1});
            XX_TEST_EXPECT_EQ(boxes[0].first, 0);
            XX_TEST_EXPECT_EQ(boxes[0].second.x_min, 0);
            XX_TEST_EXPECT_EQ(boxes[0].second.x_max, 10);
            XX_TEST_EXPECT_EQ(boxes[0].second.y_min, 0);
            XX_TEST_EXPECT_EQ(boxes[0].second.y_max, 0);
        }
        // 盒子按到屏幕坐标的偏移写入 (盒起点不在 0 时同样成立)
        el->SetBox(ftxui::Box{3, 15, 4, 4});
        {
            const auto& boxes = flow->linkBoxes();
            XX_TEST_EXPECT_EQ(boxes[0].second.x_min, 3);
            XX_TEST_EXPECT_EQ(boxes[0].second.x_max, 13);
            XX_TEST_EXPECT_EQ(boxes[0].second.y_min, 4);
        }

        // 链接跨行时按行各登记一个盒子
        auto wrapped = std::make_shared<markdown::FlowText>(
            [] {
                std::vector<markdown::Span> s(1);
                s[0].text = "aaaa bbbb";
                s[0].link = 0;
                return s;
            }()
        );
        const auto wrappedEl = std::static_pointer_cast<ftxui::Node>(wrapped);
        wrappedEl->ComputeRequirement();
        wrappedEl->SetBox(ftxui::Box{0, 3, 0, 2});
        XX_TEST_EXPECT_EQ(wrapped->lineCount(), size_t{2});
        {
            const auto& boxes = wrapped->linkBoxes();
            XX_TEST_EXPECT_EQ(boxes.size(), size_t{2});
            XX_TEST_EXPECT_EQ(boxes[0].second.y_min, 0);
            XX_TEST_EXPECT_EQ(boxes[0].second.x_max, 3);
            XX_TEST_EXPECT_EQ(boxes[1].second.y_min, 1);
            XX_TEST_EXPECT_EQ(boxes[1].second.x_max, 3);
        }

        // 键盘焦点: 焦点链接的首个区段作为 requirement_.focused 上报
        flow->setFocusedLink(0);
        el->SetBox(ftxui::Box{0, 12, 0, 0});
        XX_TEST_EXPECT_TRUE(el->requirement().focused.enabled);
        XX_TEST_EXPECT_EQ(el->requirement().focused.box.x_max, 10);
        flow->setFocusedLink(-1);
        el->SetBox(ftxui::Box{0, 12, 0, 0});
        XX_TEST_EXPECT_FALSE(el->requirement().focused.enabled);
    }

    // ---------------- 代码块排版 (FlowCodeBlock) ----------------
    {
        using markdown::CellStyle;
        using markdown::FlowCodeBlock;

        // 无语言标签: 上内边距 + 代码行 + 下内边距; 代码文字从第 1 列起
        auto block = std::make_shared<FlowCodeBlock>("a\nbb", "", CellStyle{});
        const auto el = std::static_pointer_cast<ftxui::Node>(block);
        XX_TEST_EXPECT_EQ(layoutHeight(el, 10), 4);
        auto rows = renderRows(el, 10, 4);
        XX_TEST_EXPECT_EQ(rows[0], std::string{});
        XX_TEST_EXPECT_EQ(rows[1], std::string{" a"});
        XX_TEST_EXPECT_EQ(rows[2], std::string{" bb"});
        XX_TEST_EXPECT_EQ(rows[3], std::string{});

        // 进度标签行在最上方 (左右各 1 空格), 并带暗色样式
        auto labeled = std::make_shared<FlowCodeBlock>("x", "cpp", CellStyle{.dim = true});
        const auto labeledEl = std::static_pointer_cast<ftxui::Node>(labeled);
        XX_TEST_EXPECT_EQ(layoutHeight(labeledEl, 10), 4); // 标签 + 上边距 + 1 行 + 下边距
        {
            auto screen = ftxui::Screen::Create(
                ftxui::Dimension::Fixed(10),
                ftxui::Dimension::Fixed(4)
            );
            ftxui::Render(screen, labeledEl);
            XX_TEST_EXPECT_EQ(screen.PixelAt(0, 0).character, std::string{" "});
            XX_TEST_EXPECT_EQ(screen.PixelAt(1, 0).character, std::string{"c"});
            XX_TEST_EXPECT_EQ(screen.PixelAt(4, 0).character, std::string{" "});
            XX_TEST_EXPECT_TRUE(screen.PixelAt(1, 0).dim);
            XX_TEST_EXPECT_EQ(screen.PixelAt(1, 2).character, std::string{"x"});
        }

        // 超宽代码行按可用宽度 (盒宽 - 2 列内边距) 硬拆
        auto longLine = std::make_shared<FlowCodeBlock>("abcdefghij", "", CellStyle{});
        const auto longEl = std::static_pointer_cast<ftxui::Node>(longLine);
        XX_TEST_EXPECT_EQ(layoutHeight(longEl, 6), 5); // 4 + 4 + 2 列 -> 3 行 + 2 边距
        auto longRows = renderRows(longEl, 6, 5);
        XX_TEST_EXPECT_EQ(longRows[1], std::string{" abcd"});
        XX_TEST_EXPECT_EQ(longRows[2], std::string{" efgh"});
        XX_TEST_EXPECT_EQ(longRows[3], std::string{" ij"});

        // 空代码块: 1 行空内容 + 上下内边距
        auto empty = std::make_shared<FlowCodeBlock>("", "", CellStyle{});
        XX_TEST_EXPECT_EQ(
            layoutHeight(std::static_pointer_cast<ftxui::Node>(empty), 10),
            3
        );

        // 末尾换行不算内容行; 宽字符占两列
        auto trailing = std::make_shared<FlowCodeBlock>("line\n", "", CellStyle{});
        XX_TEST_EXPECT_EQ(
            layoutHeight(std::static_pointer_cast<ftxui::Node>(trailing), 10),
            3
        );
        auto wide = std::make_shared<FlowCodeBlock>("汉字", "", CellStyle{});
        const auto wideEl = std::static_pointer_cast<ftxui::Node>(wide);
        XX_TEST_EXPECT_EQ(layoutHeight(wideEl, 6), 3);
        auto wideRows = renderRows(wideEl, 6, 3);
        XX_TEST_EXPECT_EQ(wideRows[1], std::string{" 汉字"});
    }

    // ---------------- 流式稳定块预算 (已构建块 LRU 释放) ----------------
    {
        markdown::IncrementalRenderer renderer;
        renderer.setElementBudget(3, 1024 * 1024);
        std::string text;
        size_t      fed = 0;
        for (int i = 0; i < 10; ++i) {
            text += "paragraph number " + std::to_string(i) + " with some words\n\n";
            renderer.append(std::string_view(text).substr(fed));
            fed = text.size();
        }
        XX_TEST_EXPECT_GE(renderer.stableBlockCount(), size_t{8});

        // 逐个访问 (等价于每块进入视口上屏): 已构建 (常驻) 块数不超过预算
        for (size_t i = 0; i < renderer.stableBlockCount(); ++i) {
            auto el = renderer.stableBlockElement(i, markdown::theme_default(), 60);
            XX_TEST_EXPECT_TRUE(static_cast<bool>(el));
            XX_TEST_EXPECT_TRUE(renderer.builtBlockCount() <= 3);
        }
        // 被 LRU 释放的块可再次访问 (按块重新解析重建)
        auto first = renderer.stableBlockElement(0, markdown::theme_default(), 60);
        XX_TEST_EXPECT_TRUE(static_cast<bool>(first));
        XX_TEST_EXPECT_TRUE(renderer.builtBlockCount() <= 3);
        // 源码文本始终保留 (释放的只是渲染结果)
        XX_TEST_EXPECT_EQ(renderer.text().size(), text.size());
    }

    return TestResult{g_markdown_flow_passed, g_markdown_flow_failed};
}

} // namespace test
} // namespace agentxx
