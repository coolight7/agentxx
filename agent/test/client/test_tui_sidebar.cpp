// 侧边栏 (SidebarComponent) 行为测试 (离屏渲染)
//
// 覆盖场景:
// - [常驻标签按钮] 未创建任何 tab 时, tabs 竖向列表必须显示常驻按钮 (Info/Logs)
// - [点击创建] 点击常驻按钮经 ensure 回调创建对应 tab 并自动激活 (内容区出现)
// - [footer 按钮] 激活带 footer 的 tab 时 footer 内容 (如 "LLM Context" 按钮) 可见
// - [再点取消] 已激活的常驻 tab 再点一次取消激活, 内容区隐藏
// - [动态 tab] addTab 的动态 tab 按钮显示于常驻按钮之后, 点击可切换激活
#include "test_tui_sidebar.h"

#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/components/sidebar.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <memory>
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_sidebar_passed = 0;
int g_tui_sidebar_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_sidebar_passed
#define XX_TEST_FAILED g_tui_sidebar_failed

namespace agentxx {
namespace test {

namespace {

/// 测试夹具: 最小 TUICtx + SidebarComponent (常驻 Info/Logs 标签)
struct SidebarFixture {
    TUISharedState sharedState;
    TUITheme       theme       = TUITheme::darkTheme();
    int            redrawCount = 0;

    TUICtx ctx;

    /// 与真实 agent_tui 一致的 tab id
    static constexpr const char* kInfoTabId = "info";
    static constexpr const char* kLogTabId  = "logs";

    int width  = 100;
    int height = 24;

    std::shared_ptr<SidebarComponent> comp;

    SidebarFixture() {
        ctx.state      = &sharedState;
        ctx.frameState = sharedState.readSnapshot();
        ctx.postRedraw = [this] {
            ++redrawCount;
        };
        ctx.theme     = &theme;
        ctx.sessionId = "s";
        ctx.remoteUrl = "";
        comp          = std::make_shared<SidebarComponent>(ctx);
        comp->setPinnedTabs({
            {kInfoTabId,
             "Info", [this] {
                 ensureInfo();
             }},
            {kLogTabId,
             "Logs", [this] {
                 ensureLogs();
             }},
        });
    }

    /// 模拟 agent_tui::ensureInfoSidebarTab (内容含可辨识文本)
    void ensureInfo() {
        if (!comp->hasTab(kInfoTabId)) {
            comp->addTab(
                kInfoTabId,
                "Info",
                []() -> std::vector<ScrollItem> {
                    return {
                        ScrollItem{ftxui::text("INFO_CONTENT_MARK"), false}
                    };
                },
                []() -> ftxui::Element {
                    return ftxui::hbox({ftxui::text("INFO_FOOTER_MARK")});
                }
            );
        }
    }

    /// 模拟 agent_tui::ensureLogSidebarTab (footer 含 "Menu" 按钮)
    void ensureLogs() {
        if (!comp->hasTab(kLogTabId)) {
            comp->addTab(
                kLogTabId,
                "Logs",
                []() -> std::vector<ScrollItem> {
                    return {
                        ScrollItem{ftxui::text("[Empty]") | ftxui::dim, false}
                    };
                },
                []() -> ftxui::Element {
                    return ftxui::hbox({
                        ftxui::text("> node"),
                        ftxui::filler(),
                        ftxui::text(" Menu ") | ftxui::bgcolor(ftxui::Color::Blue)
                            | ftxui::color(ftxui::Color::White),
                    });
                }
            );
        }
    }

    /// 渲染一帧并返回屏幕纯文本 (剥离 ANSI 样式转义 + 统一换行为 \n,
    /// 行号/列号即可直接用作鼠标坐标)
    std::string render() {
        ctx.frameState = sharedState.readSnapshot();
        auto el        = comp->Render();
        auto screen    = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(width),
            ftxui::Dimension::Fixed(height)
        );
        ftxui::Render(screen, el);
        return normalizeScreenText(screen.ToString());
    }

    /// 剥离 ANSI CSI 转义序列并把 "\r\n" 归一为 "\n"
    static std::string normalizeScreenText(const std::string& raw) {
        std::string noAnsi;
        noAnsi.reserve(raw.size());
        for (size_t i = 0; i < raw.size();) {
            if (raw[i] == '\x1B') {
                size_t j = i + 1;
                if (j < raw.size() && raw[j] == '[') {
                    ++j;
                    while (j < raw.size() && !(isalpha(static_cast<unsigned char>(raw[j])))) {
                        ++j;
                    }
                    if (j < raw.size()) {
                        ++j;
                    }
                } else if (j < raw.size()) {
                    ++j;
                }
                i = j;
            } else {
                noAnsi += raw[i++];
            }
        }
        std::string out;
        out.reserve(noAnsi.size());
        for (size_t i = 0; i < noAnsi.size(); ++i) {
            if (noAnsi[i] == '\r' && i + 1 < noAnsi.size() && noAnsi[i + 1] == '\n') {
                continue;
            }
            out += noAnsi[i];
        }
        return out;
    }

    /// 在渲染文本中定位 needle 首次出现的 (x, y) 屏幕坐标
    static bool
        findText(const std::string& screen, const std::string& needle, int& outX, int& outY) {
        int    y     = 0;
        size_t start = 0;
        while (start <= screen.size()) {
            size_t      end = screen.find('\n', start);
            std::string line
                = screen.substr(start, end == std::string::npos ? std::string::npos : end - start);
            size_t pos = line.find(needle);
            if (pos != std::string::npos) {
                outX = static_cast<int>(pos);
                outY = y;
                return true;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
            ++y;
        }
        return false;
    }

    /// 在 (x, y) 模拟一次左键点击 (Pressed + Released)
    /// - 组件仅在 Released 时处理点击 (Pressed 由拖选跟踪处理, 不改变状态),
    ///   故以 Released 的处理结果为准
    bool clickAt(int x, int y) {
        ftxui::Mouse m;
        m.button = ftxui::Mouse::Left;
        m.x      = x;
        m.y      = y;
        m.motion = ftxui::Mouse::Pressed;
        comp->OnEvent(ftxui::Event::Mouse("", m));
        m.motion = ftxui::Mouse::Released;
        return comp->OnEvent(ftxui::Event::Mouse("", m));
    }

    /// 定位 needle 并点击其所在单元格
    bool clickText(const std::string& screen, const std::string& needle) {
        int x = -1, y = -1;
        if (!findText(screen, needle, x, y)) {
            return false;
        }
        return clickAt(x, y);
    }
};

} // namespace

TestResult testTuiSidebar() {
    // ---- 场景 0 (诊断): 裸 Scrollable 渲染基线 ----
    {
        auto scroll = Scrollable::Create([]() -> std::vector<ScrollItem> {
            return {
                ScrollItem{ftxui::text(" AAA "), false},
                ScrollItem{ftxui::text(" BBB "), false},
            };
        });
        auto el     = scroll->Render();
        auto screen
            = ftxui::Screen::Create(ftxui::Dimension::Fixed(20), ftxui::Dimension::Fixed(6));
        ftxui::Render(screen, el);
        auto out = screen.ToString();
        XX_TEST_EXPECT_TRUE(out.find("AAA") != std::string::npos);
    }

    // ---- 场景 1: 初始帧常驻按钮可见 ----
    {
        SidebarFixture fx;
        auto           screen = fx.render();
        int            x = -1, y = -1;
        XX_TEST_EXPECT_TRUE(SidebarFixture::findText(screen, " Info ", x, y));
        int x2 = -1, y2 = -1;
        XX_TEST_EXPECT_TRUE(SidebarFixture::findText(screen, " Logs ", x2, y2));
        // 无激活 tab: 内容区不渲染 (无 INFO_CONTENT_MARK)
        XX_TEST_EXPECT_TRUE(screen.find("INFO_CONTENT_MARK") == std::string::npos);
    }

    // ---- 场景 2: 点击 Info 常驻按钮 -> 创建并激活, 内容/footer 出现 ----
    {
        SidebarFixture fx;
        auto           screen = fx.render();
        XX_TEST_EXPECT_TRUE(fx.clickText(screen, " Info "));
        XX_TEST_EXPECT_TRUE(fx.comp->hasTab(SidebarFixture::kInfoTabId));
        XX_TEST_EXPECT_TRUE(fx.comp->isTabActive(SidebarFixture::kInfoTabId));
        auto screen2 = fx.render();
        XX_TEST_EXPECT_TRUE(screen2.find("INFO_CONTENT_MARK") != std::string::npos);
        XX_TEST_EXPECT_TRUE(screen2.find("INFO_FOOTER_MARK") != std::string::npos);
    }

    // ---- 场景 3: 点击 Logs 常驻按钮 -> footer "Menu" 按钮可见 ----
    {
        SidebarFixture fx;
        auto           screen = fx.render();
        XX_TEST_EXPECT_TRUE(fx.clickText(screen, " Logs "));
        XX_TEST_EXPECT_TRUE(fx.comp->isTabActive(SidebarFixture::kLogTabId));
        auto screen2 = fx.render();
        XX_TEST_EXPECT_TRUE(screen2.find("Menu") != std::string::npos);
    }

    // ---- 场景 4: 已激活的常驻 tab 再点一次 -> 取消激活 ----
    {
        SidebarFixture fx;
        auto           screen = fx.render();
        XX_TEST_EXPECT_TRUE(fx.clickText(screen, " Info "));
        XX_TEST_EXPECT_TRUE(fx.comp->isTabActive(SidebarFixture::kInfoTabId));
        auto screen2 = fx.render();
        XX_TEST_EXPECT_TRUE(fx.clickText(screen2, " Info "));
        XX_TEST_EXPECT_FALSE(fx.comp->isTabActive(SidebarFixture::kInfoTabId));
        auto screen3 = fx.render();
        XX_TEST_EXPECT_TRUE(screen3.find("INFO_CONTENT_MARK") == std::string::npos);
        // 取消激活后常驻按钮仍在列表中
        int x = -1, y = -1;
        XX_TEST_EXPECT_TRUE(SidebarFixture::findText(screen3, " Info ", x, y));
    }

    // ---- 场景 5: 动态 tab 显示于常驻按钮之后, 点击切换 ----
    {
        SidebarFixture fx;
        fx.ensureLogs();
        // addTab 自动激活 logs; 再直接添加 info 动态 tab (模拟外部状态注入)
        fx.comp->addTab("dyn", "Dyn", []() -> std::vector<ScrollItem> {
            return {
                ScrollItem{ftxui::text("DYN_CONTENT_MARK"), false}
            };
        });
        XX_TEST_EXPECT_TRUE(fx.comp->isTabActive("dyn"));
        auto screen = fx.render();
        int  xi = -1, yi = -1, xd = -1, yd = -1;
        XX_TEST_EXPECT_TRUE(SidebarFixture::findText(screen, " Info ", xi, yi));
        XX_TEST_EXPECT_TRUE(SidebarFixture::findText(screen, " Dyn ", xd, yd));
        // 动态 tab 在常驻按钮下方
        XX_TEST_EXPECT_TRUE(yd > yi);
        // 切回 logs
        XX_TEST_EXPECT_TRUE(fx.clickText(screen, " Logs "));
        XX_TEST_EXPECT_TRUE(fx.comp->isTabActive(SidebarFixture::kLogTabId));
    }

    // ---- 场景 6: LogMenuOverlay 弹窗交互 (LLM Context / Summy Context / Clear Logs) ----
    {
        SidebarFixture fx;
        auto           overlay      = std::make_shared<LogMenuOverlay>(fx.ctx);
        bool           llmClicked   = false;
        bool           summyClicked = false;
        bool           clearClicked = false;
        bool           closed       = false;

        overlay->onLlmContext([&] {
            llmClicked = true;
        });
        overlay->onSummyContext([&] {
            summyClicked = true;
        });
        overlay->onClearLogs([&] {
            clearClicked = true;
        });
        overlay->onClose([&] {
            closed = true;
        });

        // 渲染测试
        auto el = overlay->Render();
        auto screen
            = ftxui::Screen::Create(ftxui::Dimension::Fixed(40), ftxui::Dimension::Fixed(16));
        ftxui::Render(screen, el);
        auto out = screen.ToString();
        XX_TEST_EXPECT_TRUE(out.find("Menu") != std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("LLM Context") != std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("Summy Context") != std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("Clear Logs") != std::string::npos);

        // 键盘 Enter: 默认第 0 项 LLM Context
        overlay->OnEvent(ftxui::Event::Return);
        XX_TEST_EXPECT_TRUE(llmClicked);

        // 键盘 Down + Enter: 第 1 项 Summy Context
        overlay->OnEvent(ftxui::Event::ArrowDown);
        overlay->OnEvent(ftxui::Event::Return);
        XX_TEST_EXPECT_TRUE(summyClicked);

        // 键盘 Down + Enter: 第 2 项 Clear Logs
        overlay->OnEvent(ftxui::Event::ArrowDown);
        overlay->OnEvent(ftxui::Event::Return);
        XX_TEST_EXPECT_TRUE(clearClicked);

        // 键盘 Escape: 关闭
        overlay->OnEvent(ftxui::Event::Escape);
        XX_TEST_EXPECT_TRUE(closed);
    }

    return TestResult{g_tui_sidebar_passed, g_tui_sidebar_failed};
}

} // namespace test
} // namespace agentxx
