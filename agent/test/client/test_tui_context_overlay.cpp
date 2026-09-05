// 上下文弹窗 (ContextOverlay) 行为测试 (离屏渲染)
//
// 覆盖场景:
// - [默认折叠] 每条消息仅显示一行折叠头 (+ 标记 + role + 单行预览)
// - [点击展开] 点击折叠头行后展开: 显示 "-" 标记 + 摘要行 + 完整 JSON 内容
// - [再次点击折叠] 再点一次回到折叠态
// - [键盘 Enter/Space] 切换最近可见消息的折叠状态
// - [tool_calls] 折叠头预览显示工具名列表
// - [完整原始内容] 展开后显示 dump(2) 的完整 JSON (含 tool_calls/工具结果等)
#include "test_tui_context_overlay.h"

#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "neograph/json.h"
#include <memory>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_context_overlay_passed = 0;
int g_tui_context_overlay_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_context_overlay_passed
#define XX_TEST_FAILED g_tui_context_overlay_failed

namespace agentxx {
namespace test {

namespace {

/// 测试夹具: 最小 TUICtx + ContextOverlay
struct ContextOverlayFixture {
    TUISharedState sharedState;
    TUITheme       theme = TUITheme::darkTheme();

    TUICtx ctx;

    int width  = 100;
    int height = 24;

    std::shared_ptr<ContextOverlay> comp;

    ContextOverlayFixture() {
        ctx.state      = &sharedState;
        ctx.frameState = sharedState.readSnapshot();
        ctx.postRedraw = [] {};
        ctx.theme      = &theme;
        ctx.sessionId  = "s";
        ctx.remoteUrl  = "";
        comp           = std::make_shared<ContextOverlay>(ctx);
    }

    /// 写入 contextMessages (模拟服务端 WireContextMessages 推送)
    void setMessages(neograph::json msgs) {
        sharedState.mutate([&](TUIRenderState& st) {
            st.contextMessages = std::make_shared<neograph::json>(std::move(msgs));
        });
    }

    /// 渲染一帧并返回屏幕纯文本 (剥离 ANSI 样式转义 + 统一换行为 \n)
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

/// 构造典型上下文消息数组: 系统 + 用户 + 助手(带 tool_calls) + 工具结果
neograph::json makeContextMessages() {
    return neograph::json::parse(R"([
        {"role":"system","content":"You are a helpful assistant."},
        {"role":"user","content":"Hello, please check the weather."},
        {"role":"assistant","content":"","tool_calls":[
            {"id":"call_1","type":"function","name":"agentxx_web_fetch","arguments":"{\"url\":\"https://example.com\"}"}
        ]},
        {"role":"tool","tool_call_id":"call_1","name":"agentxx_web_fetch","content":"{\"status\":200,\"body\":\"<html>ok</html>\"}"}
    ])");
}

} // namespace

TestResult testTuiContextOverlay() {
    // 测试环境无真实终端: 设置大 fallback 尺寸, 使弹窗 OnRender 的
    // Terminal::Size() 计算出的弹窗高度与测试视口一致 (否则固定 24 行,
    // 展开的长 JSON 被滚动裁剪, 断言失败)
    ftxui::Terminal::SetFallbackSize({200, 60});

    // ---- 场景 1: 默认全部折叠, 每行显示 + 标记与单行预览 ----
    {
        ContextOverlayFixture fx;
        fx.setMessages(makeContextMessages());
        auto screen = fx.render();

        // 每条消息一个折叠头, 以 "+ " 开头
        int x = -1, y = -1;
        XX_TEST_EXPECT_TRUE(ContextOverlayFixture::findText(screen, "+ [system]", x, y));
        XX_TEST_EXPECT_TRUE(ContextOverlayFixture::findText(screen, "+ [user]", x, y));
        XX_TEST_EXPECT_TRUE(ContextOverlayFixture::findText(screen, "+ [assistant]", x, y));
        XX_TEST_EXPECT_TRUE(ContextOverlayFixture::findText(screen, "+ [tool]", x, y));

        // 折叠态不显示展开体 (JSON 内容 / 摘要行)
        XX_TEST_EXPECT_TRUE(screen.find("\"role\"") == std::string::npos);
        // tool_calls 消息预览显示工具名
        XX_TEST_EXPECT_TRUE(screen.find("agentxx_web_fetch") != std::string::npos);
        // 普通消息预览为 content 首行 (自适应宽度截断, 含省略号)
        XX_TEST_EXPECT_TRUE(screen.find("You are") != std::string::npos);
        XX_TEST_EXPECT_TRUE(
            screen.find("helpful assistant") == std::string::npos
            || screen.find("...") != std::string::npos
        );
    }

    // ---- 场景 2: 点击折叠头 -> 展开显示 - 标记 + 摘要 + 完整 JSON ----
    {
        ContextOverlayFixture fx;
        fx.setMessages(makeContextMessages());
        auto screen = fx.render();

        // 点击第一条消息 (system) 折叠头
        int x = -1, y = -1;
        XX_TEST_EXPECT_TRUE(ContextOverlayFixture::findText(screen, "+ [system]", x, y));
        XX_TEST_EXPECT_TRUE(fx.clickAt(x, y));

        auto screen2 = fx.render();
        // 该消息变为展开态: "- [system]"
        XX_TEST_EXPECT_TRUE(ContextOverlayFixture::findText(screen2, "- [system]", x, y));
        // 完整 JSON 内容出现 (dump(2) 美化)
        XX_TEST_EXPECT_TRUE(screen2.find("\"role\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(
            screen2.find("\"content\": \"You are a helpful assistant.\"") != std::string::npos
        );
        // 摘要行出现 (content 长度 / 字段清单)
        XX_TEST_EXPECT_TRUE(screen2.find("content[") != std::string::npos);
        // 其余消息仍折叠
        XX_TEST_EXPECT_TRUE(screen2.find("+ [user]") != std::string::npos);
    }

    // ---- 场景 3: 再点一次展开头 -> 折叠回去 ----
    {
        ContextOverlayFixture fx;
        fx.setMessages(makeContextMessages());
        auto screen = fx.render();
        int  x = -1, y = -1;
        XX_TEST_EXPECT_TRUE(ContextOverlayFixture::findText(screen, "+ [user]", x, y));
        XX_TEST_EXPECT_TRUE(fx.clickAt(x, y));
        auto screen2 = fx.render();
        XX_TEST_EXPECT_TRUE(screen2.find("- [user]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(fx.clickText(screen2, "- [user]"));
        auto screen3 = fx.render();
        XX_TEST_EXPECT_TRUE(screen3.find("+ [user]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(screen3.find("- [user]") == std::string::npos);
        // 展开体消失
        XX_TEST_EXPECT_TRUE(screen3.find("\"role\": \"user\"") == std::string::npos);
    }

    // ---- 场景 4: 键盘 Enter / Space 切换最近可见消息 ----
    {
        ContextOverlayFixture fx;
        fx.setMessages(makeContextMessages());
        auto screen = fx.render();
        // Enter 展开第一条可见 (system)
        XX_TEST_EXPECT_TRUE(fx.comp->OnEvent(ftxui::Event::Return));
        auto screen2 = fx.render();
        XX_TEST_EXPECT_TRUE(screen2.find("- [system]") != std::string::npos);
        // Space 折叠回去
        XX_TEST_EXPECT_TRUE(fx.comp->OnEvent(ftxui::Event::Character(" ")));
        auto screen3 = fx.render();
        XX_TEST_EXPECT_TRUE(screen3.find("+ [system]") != std::string::npos);
    }

    // ---- 场景 5: tool_calls 消息展开显示完整 tool_calls 详情 ----
    {
        ContextOverlayFixture fx;
        // 大视口容纳展开后的完整 JSON (避免滚动裁剪影响断言)
        fx.height = 40;
        fx.setMessages(makeContextMessages());
        auto screen = fx.render();
        int  x = -1, y = -1;
        // 点击 assistant (tool_calls) 消息
        XX_TEST_EXPECT_TRUE(ContextOverlayFixture::findText(screen, "+ [assistant]", x, y));
        XX_TEST_EXPECT_TRUE(fx.clickAt(x, y));
        auto screen2 = fx.render();
        // 完整 tool_calls JSON (id/type/name/arguments)
        XX_TEST_EXPECT_TRUE(screen2.find("\"tool_calls\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(screen2.find("\"id\": \"call_1\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(screen2.find("\"name\": \"agentxx_web_fetch\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(screen2.find("\"arguments\"") != std::string::npos);
        // 工具结果消息展开显示原始 content
        int tx = -1, ty = -1;
        XX_TEST_EXPECT_TRUE(ContextOverlayFixture::findText(screen2, "+ [tool]", tx, ty));
        XX_TEST_EXPECT_TRUE(fx.clickAt(tx, ty));
        auto screen3 = fx.render();
        XX_TEST_EXPECT_TRUE(screen3.find("\"tool_call_id\": \"call_1\"") != std::string::npos);
        // tool 消息 content 是 JSON 字符串, dump(2) 后转义为 \"status\":200
        XX_TEST_EXPECT_TRUE(screen3.find("\\\"status\\\":200") != std::string::npos);
    }

    // ---- 场景 6: 空消息数组显示 (empty) 占位 ----
    {
        ContextOverlayFixture fx;
        fx.setMessages(neograph::json::array());
        auto screen = fx.render();
        XX_TEST_EXPECT_TRUE(screen.find("(empty)") != std::string::npos);
    }

    // ---- 场景 7: 滚轮滚动不崩溃且可滚动 (多消息展开后) ----
    {
        ContextOverlayFixture fx;
        fx.setMessages(makeContextMessages());
        auto screen = fx.render();
        // 逐个点击展开前 3 条 (system/user/assistant); tool 保持折叠
        for (const char* needle : {"+ [system]", "+ [user]", "+ [assistant]"}) {
            int x = -1, y = -1;
            XX_TEST_EXPECT_TRUE(ContextOverlayFixture::findText(screen, needle, x, y));
            XX_TEST_EXPECT_TRUE(fx.clickAt(x, y));
            screen = fx.render();
        }
        XX_TEST_EXPECT_TRUE(screen.find("\"tool_calls\"") != std::string::npos);
        // 滚轮事件: 向下滚动
        ftxui::Mouse m;
        m.button = ftxui::Mouse::WheelDown;
        m.motion = ftxui::Mouse::Pressed;
        m.x      = 50;
        m.y      = 12;
        // 事件被处理 (Scrollable 内部处理), 不崩溃
        fx.comp->OnEvent(ftxui::Event::Mouse("", m));
        auto screen3 = fx.render();
        XX_TEST_EXPECT_TRUE(screen3.find("LLM Context") != std::string::npos);
        // 滚动偏移已下移 (内容超高时) 或保持 (内容未超高)
        XX_TEST_EXPECT_TRUE(fx.comp->headerBoxes().size() == 4);
    }

    return {g_tui_context_overlay_passed, g_tui_context_overlay_failed};
}

} // namespace test
} // namespace agentxx
