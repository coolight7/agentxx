#pragma once

/// 渲染路径性能基准 (不依赖 LLM, 只测渲染)
///
/// 覆盖 (对应方案 §5.2):
/// 1. 单条消息: markdown 解析 + 建树 (用户/助手/工具/思考 四类文本)
/// 2. 布局口径: 首次测量 / 同宽度重复布局 (折行缓存命中) / 流式盒位置上移
/// 3. 整屏: N 条消息的构建 + 每帧重排 + 绘制 + Screen::ToString
/// 4. 消息列表组件: 100/1000/5000 条消息下渲染一帧 (验证每帧成本与条数解耦)
///    以及滚动一屏的成本
///
/// 说明: 内存放大 (渲染树字节) 由 test_ftxui_text 的 "markdown render tree"
/// 用例断言 (929B 源 -> 32KB 存活, ×35; 基线旧实现 ×112), 这里只测时间。
#include "agentxx-client/io/tui/lazy_scrollable.h"
#include "agentxx-client/io/tui/markdown_block.h"
#include "agentxx-client/io/tui/scroll_common.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "bench_util.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "markdown/flow.hpp"
#include "markdown/parser.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace bench {

namespace bench_render_detail {

using namespace agentxx::client;

inline markdown::Theme const& renderTheme() {
    static const TUITheme theme = TUITheme::darkTheme();
    return theme.markdownTheme;
}

/// 典型的助手回复 (标题/段落/行内样式/列表/代码块/链接)
inline std::string assistantText(size_t repeats) {
    std::string out =
        "# Result\n\n"
        "The change is applied with **bold** and *italic* parts, `inline code` and a "
        "[link](https://example.com/docs) that wraps over a couple of lines when the "
        "available width is limited.\n\n"
        "- first item with some words\n"
        "- second item\n\n"
        "```cpp\nint main() { return 0; }\n```\n\n";
    for (size_t i = 0; i < repeats; ++i) {
        out += "Paragraph number " + std::to_string(i)
               + " repeating ordinary prose so the body reaches the size of a real "
                 "assistant message, with enough words to wrap several lines.\n\n";
    }
    return out;
}

/// 典型用户消息 (纯文本, 含换行)
inline std::string userText(size_t lines) {
    std::string out;
    for (size_t i = 0; i < lines; ++i) {
        out += "user line " + std::to_string(i) + " with some words to wrap\n";
    }
    return out;
}

/// 典型工具结果 (长行 JSON/日志, 会被硬拆)
inline std::string toolText(size_t bytes) {
    std::string out;
    out.reserve(bytes);
    while (out.size() < bytes) {
        out += "{\"path\":\"/a/very/long/path/to/some/file.cpp\",\"line\":123,"
               "\"message\":\"some diagnostic text\"}\n";
    }
    return out;
}

/// 测量某元素在该宽度下的高度 (完整迭代布局)
inline int measureHeight(const ftxui::Element& el, int width) {
    return layoutAndMeasure(el, ftxui::Box{0, width - 1, 0, kTallHeight});
}

/// 直接构造懒构建列表组件 (固定条目高度, 与消息列表的用法一致)
inline std::shared_ptr<LazyScrollable> makeList(size_t count, size_t height) {
    return std::make_shared<LazyScrollable>(
        [count] {
            return count;
        },
        [](size_t i) {
            return 0x1000ULL + i;
        },
        [height](size_t, int) {
            return height;
        },
        [height](size_t i) {
            ftxui::Elements rows;
            for (size_t r = 0; r < height; ++r) {
                rows.push_back(ftxui::text(
                    r == 0 ? ("message " + std::to_string(i) + " body text") : std::string{}
                ));
            }
            LazyBuiltItem out;
            out.element     = ftxui::vbox(std::move(rows));
            out.sourceBytes = 64;
            return out;
        },
        LazyScrollable::CacheBudget{64, 2 * 1024 * 1024, 32 * 1024},
        nullptr
    );
}

constexpr int kScreenW = 100;
constexpr int kScreenH = 40;

/// 渲染组件一帧 (含屏幕创建与 ToString, 与真实 UI 循环口径一致)
inline size_t renderFrame(const std::shared_ptr<LazyScrollable>& list) {
    auto el     = list->Render() | ftxui::flex;
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(kScreenW),
        ftxui::Dimension::Fixed(kScreenH)
    );
    ftxui::Render(screen, el);
    return screen.ToString().size();
}

/// 在组件内按一次滚轮 (固定 1 行; 行数在下一帧布局时落实到锚点)
inline void wheelOnce(const std::shared_ptr<LazyScrollable>& list, ftxui::Mouse::Button button) {
    ftxui::Mouse m;
    m.button = button;
    m.motion = ftxui::Mouse::Pressed;
    m.x      = 2;
    m.y      = 2;
    (void)list->OnEvent(ftxui::Event::Mouse("", m));
}

} // namespace bench_render_detail

inline void benchRender() {
    using namespace bench_render_detail;
    std::cout << "\n=== TUI Render Benchmarks ===" << std::endl;

    const std::string assistantMd = assistantText(6);
    const std::string userPlain   = userText(20);
    const std::string toolResult  = toolText(4096);
    {
        std::cout << "  [sample sizes] assistant=" << assistantMd.size() << "B user="
                  << userPlain.size() << "B tool=" << toolResult.size() << "B" << std::endl;
    }

    // ---- 1. 单条消息: 解析 + 建树 ----
    {
        auto r = runBench("render: markdown parse+build [assistant sample]", 200, [&] {
            auto [el, builder]
                = renderMarkdown(assistantMd, ftxui::Color::White, renderTheme(), kScreenW);
            (void)el;
            (void)builder;
        });
        printResult(r);
    }
    {
        auto r = runBench("render: plain text node build [user sample]", 200, [&] {
            auto el = renderPlainText(userPlain, ftxui::Color::White);
            (void)el;
        });
        printResult(r);
    }
    {
        auto r = runBench("render: markdown parse+build [tool result 4KB]", 200, [&] {
            auto [el, builder]
                = renderMarkdown("```\n" + toolResult + "```\n", ftxui::Color::White, renderTheme(), kScreenW);
            (void)el;
            (void)builder;
        });
        printResult(r);
    }

    // ---- 2. 布局口径 ----
    {
        auto [el, builder]
            = renderMarkdown(assistantMd, ftxui::Color::White, renderTheme(), kScreenW);
        auto element = el;

        auto r = runBench("render: layout first measure [assistant sample]", 200, [&] {
            auto fresh = std::make_shared<markdown::FlowText>(
                [] {
                    std::vector<markdown::Span> spans(1);
                    spans[0].text = bench_render_detail::assistantText(0);
                    return spans;
                }()
            );
            (void)measureHeight(fresh, kScreenW);
        });
        printResult(r);

        // 同宽度重复布局: 折行结果按宽度缓存 -> 只做盒更新
        auto r2 = runBench("render: layout same width (wrap cached)", 2000, [&] {
            element->SetBox(ftxui::Box{0, kScreenW - 1, 0, kTallHeight});
        });
        printResult(r2);

        // 流式: 盒位置每帧上移 (宽度不变)
        int y = 0;
        auto r3 = runBench("render: layout box moves each frame (streaming)", 2000, [&] {
            element->SetBox(ftxui::Box{0, kScreenW - 1, y, y + kTallHeight});
            y = (y + 1) % 64;
        });
        printResult(r3);
    }

    // ---- 3. 整屏 N 条消息 ----
    for (size_t count : {30u, 120u}) {
        std::vector<ftxui::Element> elements;
        std::vector<std::unique_ptr<markdown::DomBuilder>> keep;
        for (size_t i = 0; i < count; ++i) {
            auto [el, builder]
                = renderMarkdown(assistantMd, ftxui::Color::White, renderTheme(), kScreenW);
            if (builder) {
                keep.push_back(std::move(builder));
            }
            elements.push_back(el | ftxui::color(ftxui::Color::White));
        }
        auto screen = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(kScreenW),
            ftxui::Dimension::Fixed(kScreenH)
        );

        // 整屏布局后绘制 (盒位置每帧上移, 与流式滚动一致)
        int y = 0;
        auto r = runBench(
            "render: full screen " + std::to_string(count) + " messages (draw)",
            100,
            [&] {
                ftxui::Element body = ftxui::vbox(std::move(elements));
                ftxui::Render(screen, body | ftxui::flex);
                (void)y;
            }
        );
        printResult(r);

        auto r2 = runBench(
            "render: Screen::ToString " + std::to_string(count) + " messages",
            100,
            [&] {
                (void)screen.ToString();
            }
        );
        printResult(r2);
    }

    // ---- 4. 消息列表组件: 每帧成本 vs 条数 ----
    for (size_t count : {100u, 1000u, 5000u}) {
        auto list = makeList(count, 2);
        (void)renderFrame(list); // 预热 (构建窗口内条目)
        auto r = runBench(
            "render: lazy list frame [" + std::to_string(count) + " items, no scroll]",
            200,
            [&] {
                (void)renderFrame(list);
            }
        );
        printResult(r);

        // 滚动一屏: 解除吸附后逐行上滚 (每行都触发窗口推进)
        list->setStickToBottom(false);
        auto r2 = runBench(
            "render: lazy list scroll one screen [" + std::to_string(count) + " items]",
            20,
            [&] {
                for (int i = 0; i < kScreenH; ++i) {
                    wheelOnce(list, ftxui::Mouse::WheelUp);
                    (void)renderFrame(list);
                }
            }
        );
        printResult(r2);

        // 视口停在列表中部 (非吸附, 上方仍有大量条目): 每帧只处理"锚点到视口
        // 底部"这一段 -> 帧耗时与"视口上方条数"无关 (锚点模型的直接结论)
        list->setStickToBottom(false);
        for (int i = 0; i < 200; ++i) {
            wheelOnce(list, ftxui::Mouse::WheelUp);
            (void)renderFrame(list);
        }
        auto r3 = runBench(
            "render: lazy list frame [" + std::to_string(count) + " items, viewport in middle]",
            200,
            [&] {
                (void)renderFrame(list);
            }
        );
        printResult(r3);
    }
}

} // namespace bench
} // namespace agentxx
