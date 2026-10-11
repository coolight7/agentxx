// ftxui 文本节点测试 (third_party/ftxui 的 text() 节点)
//
// 背景: 渲染树中的 text() 节点原先为每个字素存一个 std::string, 消息列表
// 一帧要建上千个节点, 渲染树内存是源文本的 30~60 倍。改为"原文 + 各行起始
// 字节偏移"后, 对外行为必须与旧实现一致 —— 行数/列宽/换行/宽字符/组合字符/
// 控制字符/超出盒宽的裁剪/选择取文本都按同一套算法。
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
#include "markdown/dom_builder.hpp"
#include "markdown/parser.hpp"
#include "markdown/theme.hpp"
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if XX_IS_MACOS_D || XX_IS_IOS_D
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif XX_IS_WIN_D
#include <malloc.h>
#if defined(_DEBUG)
#include <crtdbg.h>
#endif
#else
#include <malloc.h>
#endif

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ftxui_text_passed = 0;
int g_ftxui_text_failed = 0;

// ---------------------------------------------------------------------------
// 堆内"在用"字节数 (渲染树内存上界断言用)
//
// 这里**不替换全局 operator new/delete**: 测试进程里同时链着 libagentxx 与
// cxx_* 等动态库 (各自带一份 operator new/delete 实例), 而 macOS/Windows 上
// 动态库里的分配/释放绑定的是它自己那份实现 —— 全局替换只能记录"自己这份"
// 分配的块, 一旦"别的实现分配、本份释放"就会多走一次头部偏移, 实测 libmalloc
// 直接 abort ("pointer being freed was not allocated")。改为读取系统堆统计的
// 差值, 不改变任何分配行为。
//
// - macOS/iOS: 汇总所有 malloc zone 的 size_in_use
// - Linux/Android: mallinfo2 (旧版 glibc 与 bionic 退回 mallinfo) 的 uordblks
// - Windows: 调试 CRT 的存活分配字节 (与 memgrowth 同法; Release 无该统计)
// - 取不到统计时返回 nullopt, 调用方跳过内存断言并打印说明
// ---------------------------------------------------------------------------

/// 当前进程堆内在用字节数; nullopt = 本平台/本次构建无可用统计
std::optional<size_t> heapInUseBytes() {
#if XX_IS_MACOS_D || XX_IS_IOS_D
    vm_address_t* zones = nullptr;
    unsigned      count = 0;
    malloc_get_all_zones(mach_task_self(), nullptr, &zones, &count);
    size_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        auto* zone = reinterpret_cast<malloc_zone_t*>(zones[i]);
        if (zone == nullptr) {
            continue;
        }
        malloc_statistics_t stats{};
        malloc_zone_statistics(zone, &stats);
        total += static_cast<size_t>(stats.size_in_use);
    }
    return total;
#elif XX_IS_WIN_D
#if defined(_DEBUG)
    _CrtMemState state{};
    _CrtMemCheckpoint(&state);
    size_t total = 0;
    for (int b = 0; b < _MAX_BLOCKS; ++b) {
        total += static_cast<size_t>(state.lSizes[b]);
    }
    return total;
#else
    return std::nullopt; // Release CRT 无存活分配统计
#endif
#elif defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
    struct mallinfo2 mi = ::mallinfo2();
    return static_cast<size_t>(mi.uordblks);
#else
    struct mallinfo mi = ::mallinfo();
    return static_cast<size_t>(mi.uordblks);
#endif
#elif defined(__ANDROID__) || defined(__BIONIC__)
    struct mallinfo mi = ::mallinfo();
    return static_cast<size_t>(mi.uordblks);
#else
    return std::nullopt; // musl 等无 mallinfo
#endif
}

/// 在 fn 执行前后各取一次堆在用字节数, 返回增量
/// - 窗口内只观察"在用"字节变化, 已释放的块不计入; 负增量 (其他线程回收) 按 0 记
template<typename Fn>
std::optional<size_t> measureHeapDelta(Fn&& fn) {
    const auto before = heapInUseBytes();
    std::forward<Fn>(fn)();
    const auto after = heapInUseBytes();
    if (!before.has_value() || !after.has_value()) {
        return std::nullopt;
    }
    return *after > *before ? (*after - *before) : size_t{0};
}
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ftxui_text_passed
#define XX_TEST_FAILED g_ftxui_text_failed

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
        // 跨行选择: 行间以换行分隔 —— 起点行从起点列取到行尾, 结束行从行首取到
        // 终点列, 中间行整行 (见 ftxui::Selection::RowRange)
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

        // 端点落在同一行的空白列 (没有任何节点绘制的列, 如列表项续行的悬挂缩进、
        // 面板留白) 上时, 该行只取"起点列到行尾" (反向拖选反之), 不会把整行都算进
        // 选择; 行内其它节点的整段文本也不会因此被取出
        {
            const auto row = hbox({text("  "), text("abc\ndef")});
            // 选区 [1..5]x[0..1]: 起点行只取第 5 列之后 (无), 终点行取到第 1 列 (无)
            XX_TEST_EXPECT_EQ(selectText(row, 6, 2, 5, 0, 1, 1), std::string{});
            // 普通跨行选区不受影响 (起点行取到行尾, 终点行从行首取)
            XX_TEST_EXPECT_EQ(selectText(row, 6, 2, 4, 0, 3, 1), std::string{"c\nde"});
        }
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
        elements.reserve(kCount);
        const auto allocated = measureHeapDelta([&] {
            for (const auto& t : texts) {
                elements.push_back(text(t));
            }
        });

        if (!allocated.has_value()) {
            TEST_INFO << "text nodes: 本平台无堆统计, 跳过内存上界断言" << std::endl;
        } else {
            TEST_INFO << "text nodes: " << kCount << " x " << kSize << "B -> " << *allocated
                      << " bytes allocated" << std::endl;
            // 下界: 节点保留原文, 至少等于源文本; 上界: 2 倍源文本 (实测约 1.5 倍,
            // 含分配器取整; 旧实现的每字素一个 std::string 为 30 倍以上)
            XX_TEST_EXPECT_GE(*allocated, static_cast<size_t>(kCount * kSize));
            XX_TEST_EXPECT_TRUE(*allocated < static_cast<size_t>(kCount * kSize * 2));
        }
    }

    // ---------------- markdown 渲染树内存放大 (sourceBytes 标定依据) ----------------
    {
        // 与方案基线同量级的文档 (1.4KB 富 markdown): 基线微基准里旧实现的
        // 渲染树为源文本的 ×112 (每词一个元素 + 每字素一个 std::string)
        std::string md
            = "# Heading\n\n"
              "A paragraph with **bold**, *italic*, `code` and a [link](https://example.com) "
              "that wraps over a couple of lines when the available width is limited.\n\n"
              "- item one\n"
              "- item two\n\n"
              "```cpp\nint main() { return 0; }\n```\n\n";
        {
            const std::string para
                = "Another paragraph of ordinary prose used to grow the document to the size "
                  "of a typical message body so the measurement is comparable with the "
                  "baseline micro benchmark numbers.\n\n";
            for (int i = 0; i < 4; ++i) {
                md += para;
            }
        }
        auto parser  = markdown::make_cmark_parser();
        auto ast     = parser->parse(md);
        auto builder = std::make_unique<markdown::DomBuilder>();
        builder->set_max_width(97);

        Element     element;
        const auto  allocated = measureHeapDelta([&] {
            element = builder->build(ast, -1, markdown::theme_default());
        });
        if (!allocated.has_value()) {
            TEST_INFO << "markdown render tree: 本平台无堆统计, 跳过内存放大断言" << std::endl;
        } else {
            const double ratio = static_cast<double>(*allocated) / static_cast<double>(md.size());
            TEST_INFO << "markdown render tree: " << md.size() << "B source -> " << *allocated
                      << " bytes (x" << ratio << ")" << std::endl;
            // 自绘折行节点 + 紧凑文本节点后应明显低于基线 (×112); 这里按 ×60 把关
            // (堆统计法实测约 ×37, 含分配器取整), 同时保证绝对量级合理
            // (sourceBytes 标定依据, 见 buildMessageItem)
            XX_TEST_EXPECT_TRUE(*allocated < md.size() * 60);
        }
        XX_TEST_EXPECT_TRUE(static_cast<bool>(element));
    }

    return TestResult{g_ftxui_text_passed, g_ftxui_text_failed};
}

} // namespace test
} // namespace agentxx
