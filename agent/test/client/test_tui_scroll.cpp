// 消息列表 (LazyScrollable) 滚动行为测试
//
// 覆盖的回归场景:
// - [抖动回归] 流式输出时内容更新帧 (token 增长后首帧) 与同内容再渲染帧
//   (鼠标移动/任意事件触发) 的渲染结果必须完全一致, 且吸附底部时屏幕最底行
//   展示最新内容 —— 修复前内容帧按估算高度定位子项 (估算偏差约 ±1 行),
//   底部多出空行, 下一帧才回到正确位置, 帧间交替表现为列表上下抖动
// - [吸附跟随] stickToBottom 下内容增长自动跟随到底 (流式追加/消息入列)
// - [滚轮滚动] 滚轮上滚解除吸附并保持视图不动, 滚回底部恢复吸附
#include "test_tui_scroll.h"

#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "asio/io_context.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <memory>
#include <string>
#include <vector>

namespace agentxx {
namespace test {

int g_tui_scroll_passed = 0;
int g_tui_scroll_failed = 0;

namespace {

/// 测试夹具: 固定 100x20 视口的消息列表
struct ScrollFixture {
    asio::io_context io;
    TUISharedState   sharedState;
    TUITheme         theme = TUITheme::darkTheme();

    TUICtx                                ctx;
    std::shared_ptr<MessageListComponent> comp;

    static constexpr int kWidth  = 100;
    static constexpr int kHeight = 20;

    ScrollFixture() {
        ctx.state      = &sharedState;
        ctx.frameState = sharedState.readSnapshot();
        ctx.postRedraw = [] {};
        ctx.theme          = &theme;
        ctx.showSystemInfo = nullptr;
        ctx.session        = nullptr;
        ctx.threadId       = "s";
        ctx.remoteUrl      = "";
        comp               = std::make_shared<MessageListComponent>(ctx);
    }

    /// 追加一条历史消息 (长文本, 撑高列表使其溢出视口)
    void addHistory(int n = 8) {
        for (int i = 0; i < n; ++i) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::Assistant;
            for (int j = 0; j < 80; ++j) {
                m->text += "history line " + std::to_string(j) + " with some words to wrap ";
            }
            sharedState.mutate([&](TUIRenderState& st) {
                st.messages.push_back(std::move(m));
            });
        }
    }

    /// 设置流式增量文本 (模拟 onDelta 累积)
    void setToken(const std::string& tok) {
        sharedState.mutate([&](TUIRenderState& st) {
            if (!st.currentToken) {
                st.currentToken = std::make_shared<std::string>();
            }
            st.currentToken->assign(tok);
            st.isStreaming = true;
        });
    }

    /// 流式结束: token 落为正式消息 (模拟 TurnEnd pushCurrentTokenLocked)
    void endStream() {
        sharedState.mutate([&](TUIRenderState& st) {
            if (st.currentToken && !st.currentToken->empty()) {
                auto m      = std::make_shared<TUIMessage>();
                m->role     = TUIMessage::Role::Assistant;
                m->text     = *st.currentToken;
                st.messages.push_back(std::move(m));
            }
            st.currentToken.reset();
            st.isStreaming = false;
        });
    }

    /// 渲染一帧 (模拟 UI 循环: 取快照 -> 渲染)
    std::string render() {
        ctx.frameState = sharedState.readSnapshot();
        auto el        = comp->Render();
        auto screen
            = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
        ftxui::Render(screen, el);
        return screen.ToString();
    }

    /// 在消息列表区域中部发送滚轮事件 (返回是否被消费)
    bool wheel(ftxui::Mouse::Button button) {
        ftxui::Mouse m;
        m.button = button;
        m.motion = ftxui::Mouse::Pressed;
        m.x      = 40;
        m.y      = kHeight / 2;
        return comp->OnEvent(ftxui::Event::Mouse("", m));
    }

    /// 取渲染文本的最后一行 (去除 ANSI 控制序列干扰后判断内容)
    static std::string lastLine(const std::string& screen) {
        size_t pos = screen.rfind('\n');
        return (pos == std::string::npos) ? screen : screen.substr(pos + 1);
    }

    /// 取渲染文本的最后 n 行 (消息子项尾部带空行, 标记可能落在倒数第二行)
    static std::string lastLines(const std::string& screen, int n) {
        std::vector<std::string> lines;
        std::string              s = screen;
        size_t                   pos = 0;
        while ((pos = s.find('\n')) != std::string::npos) {
            lines.push_back(s.substr(0, pos));
            s.erase(0, pos + 1);
        }
        if (!s.empty()) {
            lines.push_back(s);
        }
        std::string out;
        for (size_t k = (lines.size() > static_cast<size_t>(n)) ? lines.size() - static_cast<size_t>(n)
                                                                : 0;
             k < lines.size();
             ++k) {
            out += lines[k];
            out += '\n';
        }
        return out;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 回归: 内容更新帧与同内容再渲染帧必须完全一致 (流式抖动)
// ---------------------------------------------------------------------------

TestResult testTuiScroll() {
    XX_TEST_EXPECT_TRUE(true);

    {
        // 场景 1: 流式输出中, 内容更新帧 vs 同内容再渲染帧 (鼠标移动帧)
        ScrollFixture f;
        f.addHistory();
        f.render(); // 首帧建立布局

        std::string tok;
        for (int step = 0; step < 25; ++step) {
            // 每步追加带唯一尾部标记的 token (标记落在流式项最后一行)
            tok += "new token content " + std::to_string(step) + " with some words "
                   + "M" + std::to_string(step) + "END ";
            f.setToken(tok);
            // 内容帧: token 增长后首帧渲染
            std::string contentFrame = f.render();
            // 同内容再渲染帧: 模拟鼠标移动触发的渲染 (内容未变)
            std::string mouseFrame = f.render();

            XX_TEST_EXPECT_TRUE(contentFrame == mouseFrame);
            // 吸附底部: 屏幕最底行必须包含最新 token 的尾部标记 (不能多出空行)
            XX_TEST_EXPECT_TRUE(
                ScrollFixture::lastLine(contentFrame).find("M" + std::to_string(step) + "END")
                    != std::string::npos
            );
        }
    }

    {
        // 场景 2: 流式结束, token 落为正式消息后仍吸附到底部
        ScrollFixture f;
        f.addHistory();
        f.render();
        f.setToken("final answer text with distinctive marker Z9Q7");
        f.render();
        f.endStream();
        f.render(); // 落消息后的首帧
        std::string frame = f.render();
        // 消息子项尾部带空行, 标记在倒数第二行
        XX_TEST_EXPECT_TRUE(ScrollFixture::lastLines(frame, 2).find("Z9Q7") != std::string::npos);
    }

    {
        // 场景 3: 滚轮上滚解除吸附, 内容增长时视图保持不动 (不跟随)
        ScrollFixture f;
        f.addHistory();
        f.render();
        for (int i = 0; i < 5; ++i) {
            XX_TEST_EXPECT_TRUE(f.wheel(ftxui::Mouse::WheelUp));
            f.render();
        }
        // 记录当前视图 (滚动位置已上移)
        std::string before = f.render();
        XX_TEST_EXPECT_FALSE(before.find("history line 79") != std::string::npos);
        // 内容在底部增长
        f.setToken("growing content below viewport with marker Q9ZZ");
        for (int i = 0; i < 10; ++i) {
            f.render();
        }
        // 未吸附: 视图不得被底部增长拉动 (两帧渲染一致)
        std::string view1 = f.render();
        std::string view2 = f.render();
        XX_TEST_EXPECT_TRUE(view1 == view2);
        XX_TEST_EXPECT_TRUE(view1 == before);
    }

    {
        // 场景 4: 滚回底部恢复吸附, 内容继续增长时跟随到底
        ScrollFixture f;
        f.addHistory();
        f.render();
        f.wheel(ftxui::Mouse::WheelUp);
        f.render();
        // 一直滚到底部 (视口高 20, 多滚几次必然到达)
        for (int i = 0; i < 200; ++i) {
            f.wheel(ftxui::Mouse::WheelDown);
            f.render();
        }
        // 底部内容增长 -> 跟随
        f.setToken("follow me to the bottom marker F9AA");
        f.render();
        XX_TEST_EXPECT_TRUE(ScrollFixture::lastLine(f.render()).find("F9AA") != std::string::npos);
    }

    {
        // 场景 5: 估算高度偏差大时 (含长串无空格文本) 内容帧也必须定位准确
        ScrollFixture f;
        f.addHistory(4);
        f.render();
        std::string tok;
        for (int step = 0; step < 15; ++step) {
            // 长串无空格文本 (估算/实测偏差大) + 短标记
            tok += "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" + std::to_string(step)
                   + " ";
            f.setToken(tok);
            std::string c1 = f.render();
            std::string c2 = f.render();
            XX_TEST_EXPECT_TRUE(c1 == c2);
            XX_TEST_EXPECT_TRUE(
                ScrollFixture::lastLine(c1).find(std::to_string(step)) != std::string::npos
            );
        }
    }

    return TestResult{g_tui_scroll_passed, g_tui_scroll_failed};
}

} // namespace test
} // namespace agentxx
