// TUI 主题 (TUITheme) 弱化文字 (dim) 测试
//
// 背景: `ftxui::dim` 输出的是终端 `dim` 属性 (SGR 2), 而终端普遍按"前景色亮度
// 减半"实现该属性 —— 深色背景上文字变暗 = 与背景接近 = 弱化; 浅色背景上深色
// 文字却更深、与白底对比更强, 反而比正文醒目 (弱化的反效果)。
//
// 覆盖点:
// - [深色主题] 仍走终端 `dim` 属性: 单元格 dim 标记置位, 前景色保持原值,
//   输出中含 SGR 2
// - [浅色主题] 不使用 SGR 2, 前景色向背景色混合: 深灰 50 → 180 (变淡),
//   且未设置前景色的文字以 normalColor 为基准色
// - [单元格背景色优先] 单元格自带背景色时向该背景色混合 (白字 + 黑底 → 194),
//   而不是向主题背景色变浅; 底色/前景色套在弱化元素之外 (整行着色) 时同样生效
// - [无颜色终端] 颜色被降级为终端默认色时不混合, 退回终端 `dim` 属性
// - [跨帧缓存安全] 装饰器产出的元素可在主题对象销毁后继续渲染 (颜色按值捕获)
#include "agentxx-test/client/test_tui_theme.h"

#include "agentxx-client/io/tui/tui_theme.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/terminal.hpp"
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_theme_passed = 0;
int g_tui_theme_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_theme_passed
#define XX_TEST_FAILED g_tui_theme_failed

namespace agentxx {
namespace test {

using namespace agentxx::client;

namespace {

using namespace ftxui;

/// 渲染期间强制真彩色: 用例按具体 RGB 值断言混合结果, 需要颜色不被降级为
/// 调色板色 (终端探测结果随运行环境变化); 析构时恢复原探测值
class ScopedTrueColor {
public:

    ScopedTrueColor() :
        old_(Terminal::ColorSupport()) {
        Terminal::SetColorSupport(Terminal::Color::TrueColor);
    }

    ~ScopedTrueColor() {
        Terminal::SetColorSupport(old_);
    }

private:

    Terminal::Color old_;
};

/// 离屏渲染单个元素 (尺寸足够放下用例文本/背景)
Screen renderOffscreen(Element el, int w = 6, int h = 2) {
    Screen screen(w, h);
    Render(screen, el);
    return screen;
}

/// 装饰器产出的元素可脱离主题对象继续渲染 (元素被跨帧缓存的场景):
/// 主题在渲染前销毁, 颜色须已按值捕获, 不应出现悬空访问
void testDimElementOutlivesTheme() {
    ScopedTrueColor colorSupport;
    Element         el;
    {
        const TUITheme theme = TUITheme::lightTheme();
        el                   = text("X") | color(Color::RGB(50, 50, 50)) | theme.dim();
    }
    auto screen = renderOffscreen(el);
    XX_TEST_EXPECT_EQ(screen.CellAt(0, 0).foreground_color, Color::RGB(180, 180, 180));
    XX_TEST_EXPECT_FALSE(screen.CellAt(0, 0).dim);
}

/// 深色主题: 弱化 = 终端 `dim` 属性 (前景色不动, 由终端变暗)
void testDimDarkTheme() {
    ScopedTrueColor colorSupport;
    const TUITheme  theme = TUITheme::darkTheme();
    const Color     base  = Color::RGB(50, 50, 50);

    auto screen = renderOffscreen(text("X") | color(base) | theme.dim());

    XX_TEST_EXPECT_FALSE(theme.lightBackground);
    XX_TEST_EXPECT_TRUE(screen.CellAt(0, 0).dim);
    XX_TEST_EXPECT_EQ(screen.CellAt(0, 0).foreground_color, base);
    // 终端输出中出现 dim 属性 (SGR 2)
    XX_TEST_EXPECT_TRUE(screen.ToString().find("\x1B[2m") != std::string::npos);
}

/// 浅色主题: 弱化 = 前景色向背景色混合 (变淡), 不再使用 SGR 2
void testDimLightTheme() {
    ScopedTrueColor colorSupport;
    const TUITheme  theme = TUITheme::lightTheme();

    // 深灰正文色 (50,50,50) 在白底上弱化后为 RGB(180,180,180):
    // 值由 sRGB gamma 2.2 插值 45% 独立算得 (与实现同公式但独立取数)
    {
        auto screen = renderOffscreen(text("X") | color(theme.normalColor) | theme.dim());
        XX_TEST_EXPECT_TRUE(theme.lightBackground);
        XX_TEST_EXPECT_FALSE(screen.CellAt(0, 0).dim);
        XX_TEST_EXPECT_TRUE(screen.ToString().find("\x1B[2m") == std::string::npos);
        XX_TEST_EXPECT_EQ(screen.CellAt(0, 0).foreground_color, Color::RGB(180, 180, 180));
        // 确实是"变淡"方向 (比正文色更接近白色背景), 而非变深
        XX_TEST_EXPECT_TRUE(screen.CellAt(0, 0).foreground_color != theme.normalColor);
    }

    // 当前提示色 (120,122,125) 向白底混合 45%，经 gamma 2.2 插值并截断为整数后
    // 得到 RGB(195,195,196)，期望值独立计算，不调用被测混色函数。
    {
        auto screen = renderOffscreen(text("─") | color(theme.hintColor) | theme.dim());
        XX_TEST_EXPECT_EQ(screen.CellAt(0, 0).foreground_color, Color::RGB(195, 195, 196));
    }

    // 未设置前景色的文字 (跟随终端默认色, 无法参与混合): 以正文色为基准色,
    // 渲染结果不应停留在终端默认色
    {
        auto screen = renderOffscreen(text("X") | theme.dim());
        XX_TEST_EXPECT_TRUE(screen.CellAt(0, 0).foreground_color != Color::Default);
        XX_TEST_EXPECT_EQ(screen.CellAt(0, 0).foreground_color, Color::RGB(180, 180, 180));
        XX_TEST_EXPECT_FALSE(screen.CellAt(0, 0).dim);
    }
}

/// 单元格自带背景色 (如选中行底色) 时向该底色混合, 而不是向主题背景色
void testDimUsesCellBackground() {
    ScopedTrueColor colorSupport;
    const TUITheme  theme = TUITheme::lightTheme();

    // 白字 + 黑底: 弱化后应偏暗 (RGB(194,194,194)), 而不是保持白色
    auto screen = renderOffscreen(
        text("X") | bgcolor(Color::RGB(0, 0, 0)) | color(Color::White) | theme.dim()
    );
    XX_TEST_EXPECT_EQ(screen.CellAt(0, 0).foreground_color, Color::RGB(194, 194, 194));
    XX_TEST_EXPECT_FALSE(screen.CellAt(0, 0).dim);

    // 底色套在弱化元素之外 (整行选中底色在条目元素之外设置): ftxui 的
    // 颜色装饰器先写入单元格再渲染子元素, 因此弱化时底色已就位
    auto outerBgScreen = renderOffscreen(
        (text("X") | color(Color::White) | theme.dim()) | bgcolor(Color::RGB(0, 0, 0))
    );
    XX_TEST_EXPECT_EQ(outerBgScreen.CellAt(0, 0).foreground_color, Color::RGB(194, 194, 194));

    // 前景色也套在弱化元素之外 (列表条目整行着色): 弱化元素渲染更晚, 结果不被外层抹掉
    auto outerFgScreen = renderOffscreen((text("X") | theme.dim()) | color(Color::RGB(50, 50, 50)));
    XX_TEST_EXPECT_EQ(outerFgScreen.CellAt(0, 0).foreground_color, Color::RGB(180, 180, 180));
}

/// 终端不报告颜色支持 (如 NO_COLOR): 主题各色被降级为终端默认色, 无法混合,
/// 退回终端 `dim` 属性
void testDimWithoutColorSupport() {
    const Terminal::Color old = Terminal::ColorSupport();
    Terminal::SetColorSupport(Terminal::Color::Palette1);

    const TUITheme theme = TUITheme::lightTheme();
    // 套用主题色后仍渲染为终端默认前景色 (ANSI 参数 39) : 没有可混合的颜色
    XX_TEST_EXPECT_EQ(theme.normalColor.Print(false), std::string{"39"});

    auto screen = renderOffscreen(text("X") | color(theme.normalColor) | theme.dim());
    XX_TEST_EXPECT_TRUE(screen.CellAt(0, 0).dim);
    // 退回终端 dim 属性时不改动前景色
    XX_TEST_EXPECT_EQ(screen.CellAt(0, 0).foreground_color, theme.normalColor);

    Terminal::SetColorSupport(old);
}

} // namespace

TestResult testTuiTheme() {
    testDimElementOutlivesTheme();
    testDimDarkTheme();
    testDimLightTheme();
    testDimUsesCellBackground();
    testDimWithoutColorSupport();

    return TestResult{g_tui_theme_passed, g_tui_theme_failed};
}

} // namespace test
} // namespace agentxx
