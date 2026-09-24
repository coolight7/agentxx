// ftxui 文本节点测试 (third_party/ftxui 的 text() 节点)
//
// 背景: 渲染树中的 text() 节点原先为每个字素存一个 std::string, 消息列表
// 一帧要建上千个节点, 渲染树内存是源文本的 30~60 倍。改为"原文 + 各行起始
// 字节偏移"后, 对外行为必须与旧实现一致 —— 行数/列宽/换行/宽字符/组合字符/
// 控制字符/超出盒宽的裁剪/选择取文本都按同一口径。
//
// 覆盖点:
// - [行数与列宽] 多行/尾随换行/空文本/宽字符(两列)/组合字符(零列)/控制字符(零列)
// - [绘制] 逐格内容与旧实现一致 (宽字符占两格, 第二格为占位空串)
// - [裁剪] 超出盒宽的部分不绘制; 超出盒高的行不绘制
// - [选择] 行区间 + 列区间取文本与旧实现一致 (含宽字符与组合字符)
// - [内存] 构造 100 条 1KB 文本的分配字节数上界 (旧实现每字素一个 std::string)
#include "agentxx-test/client/test_ftxui_text.h"

#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/selection.hpp"
#include "ftxui/screen/screen.hpp"
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ftxui_text_passed = 0;
int g_ftxui_text_failed = 0;

/// 累计的 operator new 申请字节数 (仅 g_countAllocations 打开时累加)
std::atomic<size_t> g_allocatedBytes{0};
bool                g_countAllocations = false;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ftxui_text_passed
#define XX_TEST_FAILED g_ftxui_text_failed

// 计量分配: 转发到 malloc, 语义与默认实现一致; 只在本模块的内存断言期间
// 打开计数 (g_countAllocations), 不影响其他测试
void* operator new(std::size_t size) {
    if (g_countAllocations) {
        g_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (g_countAllocations) {
        g_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}

namespace agentxx {
namespace test {

using namespace ftxui;

namespace {

/// 逐格取回屏幕内容 (按行主序; 每格一个字符, 宽字符占位格为空串)
std::vector<std::string> renderCells(const Element& el, int w, int h) {
    auto screen = Screen::Create(Dimension::Fixed(w), Dimension::Fixed(h));
    Render(screen, el);
    std::vector<std::string> cells;
    cells.reserve(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            cells.push_back(screen.PixelAt(x, y).character);
        }
    }
    return cells;
}

/// 屏幕一行拼成的文本 (未写入的格补空格, 宽字符的占位格跳过)
std::string rowText(const std::vector<std::string>& cells, int w, int y) {
    std::string out;
    bool        previous_fullwidth = false;
    for (int x = 0; x < w; ++x) {
        const std::string& ch = cells[static_cast<size_t>(y) * static_cast<size_t>(w)
                                      + static_cast<size_t>(x)];
        if (!previous_fullwidth) {
            out += ch.empty() ? std::string{" "} : ch;
        }
        previous_fullwidth = ch.size() > 1;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

/// 按 FTXUI 拖选路径取回 [x0, y0] - [x1, y1] 区域内的文本
std::string selectText(
    const Element& el,
    int            w,
    int            h,
    int            x0,
    int            y0,
    int            x1,
    int            y1
) {
    el->ComputeRequirement();
    el->SetBox(Box{0, w - 1, 0, h - 1});
    Selection selection(x0, y0, x1, y1);
    el->Select(selection);
    return selection.GetParts();
}

} // namespace

TestResult testFtxuiText() {
    // ---------------- 行数与列宽 ----------------
    {
        auto req = [](std::string_view s) {
            auto el = text(s);
            el->ComputeRequirement();
            return std::pair<int, int>{el->requirement().min_x, el->requirement().min_y};
        };
        XX_TEST_EXPECT_TRUE(req("hello") == std::make_pair(5, 1));
        XX_TEST_EXPECT_TRUE(req("") == std::make_pair(0, 1));
        XX_TEST_EXPECT_TRUE(req("a\nbb\nccc") == std::make_pair(3, 3));
        // 尾随换行仍算一行 (旧实现: '\n' 之后还有一行, 内容为空)
        XX_TEST_EXPECT_TRUE(req("a\n") == std::make_pair(1, 2));
        XX_TEST_EXPECT_TRUE(req("\n\n") == std::make_pair(0, 3));
        // 宽字符占两列
        XX_TEST_EXPECT_TRUE(req("汉字") == std::make_pair(4, 1));
        XX_TEST_EXPECT_TRUE(req("汉\n字") == std::make_pair(2, 2));
        // 组合字符不占列 (与前一字符合并)
        XX_TEST_EXPECT_TRUE(req("e\xCC\x81x") == std::make_pair(2, 1));
        // 控制字符不占列 ('\r' 与 '\x01' 都丢弃; 字面量拆开写, 避免
        // "\x01b" 被当作单个十六进制转义 0x1B)
        XX_TEST_EXPECT_TRUE(req("a\x01" "b") == std::make_pair(2, 1));
        XX_TEST_EXPECT_TRUE(req("a\r\nb") == std::make_pair(1, 2));
    }

    // ---------------- 绘制 (逐格内容) ----------------
    {
        auto cells = renderCells(text("hello"), 5, 1);
        XX_TEST_EXPECT_EQ(rowText(cells, 5, 0), std::string{"hello"});

        cells = renderCells(text("a\nbb"), 3, 2);
        XX_TEST_EXPECT_EQ(rowText(cells, 3, 0), std::string{"a"});
        XX_TEST_EXPECT_EQ(rowText(cells, 3, 1), std::string{"bb"});

        // 宽字符: 写在自身所在格, 第二格为占位空串 (Screen::ToString 会跳过它)
        cells = renderCells(text("汉字"), 4, 1);
        XX_TEST_EXPECT_EQ(cells[0], std::string{"汉"});
        XX_TEST_EXPECT_EQ(cells[1], std::string{});
        XX_TEST_EXPECT_EQ(cells[2], std::string{"字"});
        XX_TEST_EXPECT_EQ(cells[3], std::string{});

        // 组合字符并入前一格
        cells = renderCells(text("e\xCC\x81x"), 2, 1);
        XX_TEST_EXPECT_EQ(cells[0], std::string{"e\xCC\x81"});
        XX_TEST_EXPECT_EQ(cells[1], std::string{"x"});
    }

    // ---------------- 超出盒宽的裁剪 ----------------
    {
        // 文本比盒宽长: 只画前 width 列, 其余不绘制
        auto cells = renderCells(text("abcdef"), 3, 1);
        XX_TEST_EXPECT_EQ(rowText(cells, 3, 0), std::string{"abc"});

        // 宽字符被盒宽截断时不越界写 (盒宽 2: 第二字起于第 3 列, 不绘制)
        cells = renderCells(text("汉字"), 2, 1);
        XX_TEST_EXPECT_EQ(cells[0], std::string{"汉"});
        XX_TEST_EXPECT_EQ(cells[1], std::string{});

        // 行数超过盒高: 只画前 height 行
        cells = renderCells(text("a\nb\nc"), 1, 2);
        XX_TEST_EXPECT_EQ(rowText(cells, 1, 0), std::string{"a"});
        XX_TEST_EXPECT_EQ(rowText(cells, 1, 1), std::string{"b"});
    }

    // ---------------- 选择取文本 ----------------
    {
        XX_TEST_EXPECT_EQ(selectText(text("hello world"), 11, 1, 0, 0, 4, 0), std::string{"hello"});
        XX_TEST_EXPECT_EQ(selectText(text("hello world"), 11, 1, 6, 0, 10, 0), std::string{"world"});
        // 选择区间超出盒宽时按盒宽收敛 (SaturateHorizontal)
        XX_TEST_EXPECT_EQ(selectText(text("hello world"), 11, 1, 0, 0, 99, 0), std::string{"hello world"});
        // 跨行选择: 行间以换行分隔。注意 FTXUI 的选择是"按行横向收敛"
        // (SaturateHorizontal): 起始行从起点列取到行尾, 结束行从行首取到终点列
        XX_TEST_EXPECT_EQ(selectText(text("abc\ndef"), 3, 2, 0, 0, 2, 1), std::string{"abc\ndef"});
        XX_TEST_EXPECT_EQ(selectText(text("abc\ndef"), 3, 2, 1, 0, 1, 1), std::string{"bc\nde"});
        // 完全在盒外的选择取不到内容
        XX_TEST_EXPECT_EQ(selectText(text("abc"), 3, 1, 0, 5, 2, 6), std::string{});
        // 宽字符: 只选它的第一格才取到该字符 (第二格是保留列, 无字符)
        XX_TEST_EXPECT_EQ(selectText(text("汉字"), 4, 1, 0, 0, 0, 0), std::string{"汉"});
        XX_TEST_EXPECT_EQ(selectText(text("汉字"), 4, 1, 2, 0, 3, 0), std::string{"字"});
        XX_TEST_EXPECT_EQ(selectText(text("汉字"), 4, 1, 0, 0, 3, 0), std::string{"汉字"});
        // 组合字符随其所修饰的字符一起被选中
        XX_TEST_EXPECT_EQ(selectText(text("e\xCC\x81x"), 2, 1, 0, 0, 0, 0), std::string{"e\xCC\x81"});
        XX_TEST_EXPECT_EQ(selectText(text("e\xCC\x81x"), 2, 1, 1, 0, 1, 0), std::string{"x"});
    }

    // ---------------- 渲染树内存 ----------------
    {
        // 100 条 1KB 纯文本: 源文本合计 100 KB。每个节点只允许少量固定开销,
        // 旧实现 (每字素一个 std::string) 在同一场景下为源文本的 30 倍以上
        constexpr int kCount = 100;
        constexpr int kSize  = 1024;

        std::vector<std::string> texts;
        texts.reserve(kCount);
        for (int i = 0; i < kCount; ++i) {
            texts.emplace_back(static_cast<size_t>(kSize), 'x');
        }

        std::vector<Element> elements;
        g_allocatedBytes.store(0);
        g_countAllocations = true;
        for (const auto& t : texts) {
            elements.push_back(text(t));
        }
        g_countAllocations = false;
        const size_t allocated = g_allocatedBytes.load();

        TEST_INFO << "text nodes: " << kCount << " x " << kSize << "B -> " << allocated
                  << " bytes allocated" << std::endl;
        XX_TEST_EXPECT_GE(allocated, static_cast<size_t>(kCount * kSize)); // 至少等于源文本
        XX_TEST_EXPECT_TRUE(allocated < static_cast<size_t>(kCount * kSize * 2));
    }

    return TestResult{g_ftxui_text_passed, g_ftxui_text_failed};
}

} // namespace test
} // namespace agentxx
