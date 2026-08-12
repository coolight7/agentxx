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

    /// 模拟 onDelta 新建流式 token (指定流身份 epoch + 整体替换内容);
    /// 同流追加时保持相同 epoch (与 onDelta 的 COW/原地 append 语义一致)
    void setTokenEpoch(uint64_t epoch, const std::string& tok) {
        sharedState.mutate([&](TUIRenderState& st) {
            st.currentToken     = std::make_shared<std::string>(tok);
            st.currentTokenEpoch = epoch;
            st.isStreaming      = true;
        });
    }

    /// 模拟 onSync: 整体重建 TUIRenderState (currentTokenEpoch 归 0, 消息历史保留)
    /// 注意: 必须深拷贝消息 (每个消息新建 shared_ptr) —— 真实 onSync 从
    /// WireMessage 反序列化重建 ViewMessage, 消息指针全变, 驱动 itemKey 变化;
    /// 若浅拷贝 (共享指针), key 不变, 无法模拟缓存失效路径
    void rebuildState() {
        sharedState.mutate([&](TUIRenderState& st) {
            auto prev = sharedState.snapshot();
            auto ns   = std::make_shared<TUIRenderState>();
            ns->isStreaming = false;
            ns->messages.reserve(prev->messages.size());
            for (const auto& m : prev->messages) {
                ns->messages.push_back(std::make_shared<TUIMessage>(*m));
            }
            st = std::move(*ns);
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

    {
        // 场景 6: onSync 重建 state 后新一轮流式首 token 不丢失 (epoch 碰撞回归)
        // 回归链: 首轮流式 (UI 缓存 streamEpoch_=1) -> 流结束 -> onSync (epoch 归 0)
        //   -> 新流首 token (epoch 0->1, 恰与 UI 缓存的 streamEpoch_ 相等)
        //   -> 必须强制重建渲染器; 否则 syncStream 误判"同一流"走增量分支,
        //      而 fedLen 仍是旧流长度, 首 token 被整体跳过 (渲染缺字),
        //      直到 token 超过旧 fedLen 才从错误偏移开始显示
        ScrollFixture f;
        f.addHistory();
        f.render();

        // 第一轮流式: epoch 0 -> 1, UI 缓存 streamEpoch_ = 1
        f.setTokenEpoch(1, "first stream content marker F1ST");
        f.render();
        f.endStream(); // token 落为正式消息
        f.render();    // 无 token 帧 (OnRender 清理)

        // 模拟 onSync: 整体重建 state, epoch 归 0 (保留历史)
        f.rebuildState();
        f.render(); // 无 token 帧

        // 用户发送消息后新一轮首 token: epoch 0 -> 1, 与 UI 缓存的 streamEpoch_ 相等
        f.setTokenEpoch(1, "second stream marker S2ND");
        std::string frame = f.render();
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLine(frame).find("S2ND") != std::string::npos
        );

        // 同流后续 token 追加 (epoch 不变) 仍正常增量显示
        f.setTokenEpoch(1, "second stream marker S2ND with more tail T3ST");
        std::string frame2 = f.render();
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLine(frame2).find("T3ST") != std::string::npos
        );
    }

    {
        // 场景 7: 主题切换 (invalidateCache) 后同内容渲染必须与切换前一致
        // 回归: clearCache 后阶段 1 用新主题重建了 Element, 但阶段 2 的
        // "缓存命中且 box 相同则跳过布局" 误判 (lastBoxes_ 未清, 新元素
        // 从未 SetBox) -> 新元素以未初始化 box_ 渲染, 消息列表消失;
        // 且此后每帧 box 恒同持续跳过, 直到内容变化才恢复
        ScrollFixture f;
        f.addHistory();
        f.render(); // 建立缓存与布局
        std::string before = f.render();

        f.comp->invalidateCache(); // 模拟主题切换: 清空渲染缓存
        std::string afterClear = f.render(); // 缓存重建首帧: 必须重新布局
        XX_TEST_EXPECT_TRUE(before == afterClear);

        // 后续帧 (缓存命中 + box 相同路径) 仍一致 (跳过布局是安全的)
        std::string cachedFrame = f.render();
        XX_TEST_EXPECT_TRUE(before == cachedFrame);

        // 流式结束后再切一次主题 (双路径: 消息 + 流式残留缓存) 仍一致
        f.setToken("streaming text with marker THME");
        f.render();
        f.endStream();
        f.render();
        std::string before2 = f.render();
        f.comp->invalidateCache();
        XX_TEST_EXPECT_TRUE(before2 == f.render());
    }

    {
        // 场景 8: onSync 整体重建 state 后 (消息指针全变), 同内容渲染必须正常
        // 回归: onSync 重建后所有消息 key 变化 -> 阶段1 全部重建新元素,
        // 但 lastBoxes_ 保留旧值 (resize 不重置), 内容相同 + 滚动位置不变时
        // itemBox 与上帧相同 -> 阶段2 误判 "缓存命中且 box 相同" 跳过布局,
        // 新元素从未 SetBox (box_ = {0,0,0,0}) -> 渲染只画首字符/整行消失
        ScrollFixture f;
        f.addHistory();
        f.render(); // 建立缓存与布局

        // 模拟 onSync: 整体重建 state (消息内容不变, 指针全变)
        f.rebuildState();
        f.render(); // 全部重建帧

        // 同内容再渲染帧: 内容未变, box 相同 -> 缓存命中/跳过布局路径
        std::string frame = f.render();
        // 历史消息完整显示 (无整行消失)
        XX_TEST_EXPECT_TRUE(frame.find("history line 79") != std::string::npos);
    }

    {
        // 场景 8b: onSync 重建后发送 user 消息, 新消息必须完整显示
        ScrollFixture f;
        f.addHistory();
        f.render();
        f.rebuildState();
        f.render();

        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::User;
            m->text = "hello user message with marker USRM";
            st.messages.push_back(std::move(m));
            st.isStreaming = true;
        });
        f.comp->setStickToBottom(true);
        std::string frame = f.render();
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(frame, 3).find("USRM") != std::string::npos
        );
        XX_TEST_EXPECT_TRUE(frame == f.render());
    }

    {
        // 场景 8c: onSync 重建 + 估算==实测的短消息 (无折行) -> 跳过布局回归
        // 触发条件: 消息短/单行, estimateLines == 实测高度, corrected=false,
        // 滚动偏移不变 -> 阶段 2 sameBox=true 误判跳过布局 -> 新元素 box_ 未
        // 初始化 {0,0,0,0} -> Text 只画首字符到 (0,0) (用户报告"少开头两
        // 个字/整行不显示但有滚动高度"); 长文本场景因估算偏差触发 corrected
        // 重算偏移恰好避开此路径, 故此前测试未复现
        ScrollFixture f;
        // 短消息 (单行, 无折行): 估算 == 实测
        f.sharedState.mutate([&](TUIRenderState& st) {
            for (int i = 0; i < 30; ++i) {
                auto m  = std::make_shared<TUIMessage>();
                m->role = TUIMessage::Role::Assistant;
                m->text = "short line " + std::to_string(i);
                st.messages.push_back(std::move(m));
            }
        });
        f.render(); // 建立缓存与布局

        // 模拟 onSync: 整体重建 state (指针全变, 内容相同)
        f.rebuildState();
        std::string rebuilt = f.render(); // 全部重建帧
        if (rebuilt.find("short line 29") == std::string::npos) {
            fprintf(stderr, "[DBG8c] rebuilt missing short line 29, dump:\n");
            // 分行打印 (转义序列干扰行内查找, 但可看行分布)
            std::string s = rebuilt;
            size_t pos = 0, lineNo = 0;
            while ((pos = s.find('\n')) != std::string::npos) {
                fprintf(stderr, "  L%02zu: %s\n", lineNo++, s.substr(0, pos).c_str());
                s.erase(0, pos + 1);
            }
            fprintf(stderr, "  L%02zu: %s\n", lineNo, s.c_str());
        }
        // 历史消息完整显示 (无整行消失)
        XX_TEST_EXPECT_TRUE(rebuilt.find("short line 29") != std::string::npos);
        // 同内容再渲染帧一致
        XX_TEST_EXPECT_TRUE(rebuilt == f.render());

        // 发送 user 消息 (估算==实测的单行消息) 后完整显示
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::User;
            m->text = "user msg marker USRM";
            st.messages.push_back(std::move(m));
            st.isStreaming = true;
        });
        f.comp->setStickToBottom(true);
        std::string frame = f.render();
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(frame, 3).find("USRM") != std::string::npos
        );
        XX_TEST_EXPECT_TRUE(frame == f.render());
    }

    return TestResult{g_tui_scroll_passed, g_tui_scroll_failed};
}

} // namespace test
} // namespace agentxx
