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
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "asio/io_context.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <memory>
#include <set>
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

    /// 渲染尺寸 (成员使高视口夹具可覆写; 默认 100x20)
    int width  = kWidth;
    int height = kHeight;

    ScrollFixture() {
        ctx.state      = &sharedState;
        ctx.frameState = sharedState.readSnapshot();
        ctx.postRedraw = [] {};
        ctx.theme      = &theme;
        ctx.sessionId  = "s";
        ctx.remoteUrl  = "";
        comp           = std::make_shared<MessageListComponent>(ctx);
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
            st.currentToken      = std::make_shared<std::string>(tok);
            st.currentTokenEpoch = epoch;
            st.isStreaming       = true;
        });
    }

    /// 模拟 onSync: 整体重建 TUIRenderState (currentTokenEpoch 归 0, 消息历史保留)
    /// 注意: 必须深拷贝消息 (每个消息新建 shared_ptr) —— 真实 onSync 从
    /// WireMessage 反序列化重建 ViewMessage, 消息指针全变, 驱动 itemKey 变化;
    /// 若浅拷贝 (共享指针), key 不变, 无法模拟缓存失效路径
    void rebuildState() {
        sharedState.mutate([&](TUIRenderState& st) {
            auto prev       = sharedState.snapshot();
            auto ns         = std::make_shared<TUIRenderState>();
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
                auto m  = std::make_shared<TUIMessage>();
                m->role = TUIMessage::Role::Assistant;
                m->text = *st.currentToken;
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
        auto screen    = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(width),
            ftxui::Dimension::Fixed(height)
        );
        ftxui::Render(screen, el);
        return screen.ToString();
    }

    /// 在消息列表区域中部发送滚轮事件 (返回是否被消费)
    bool wheel(ftxui::Mouse::Button button) {
        ftxui::Mouse m;
        m.button = button;
        m.motion = ftxui::Mouse::Pressed;
        m.x      = 40;
        m.y      = height / 2;
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
        std::string              s   = screen;
        size_t                   pos = 0;
        while ((pos = s.find('\n')) != std::string::npos) {
            lines.push_back(s.substr(0, pos));
            s.erase(0, pos + 1);
        }
        if (!s.empty()) {
            lines.push_back(s);
        }
        std::string out;
        for (size_t k
             = (lines.size() > static_cast<size_t>(n)) ? lines.size() - static_cast<size_t>(n) : 0;
             k < lines.size();
             ++k) {
            out += lines[k];
            out += '\n';
        }
        return out;
    }
};

/// 高视口夹具 (可见条数 > 消息列表 LazyScrollable 预算 maxItems=64):
/// 用于复现"可见集自身超预算 -> 预算淘汰误伤可见子项" (见场景 17b)
struct TallScrollFixture : ScrollFixture {
    TallScrollFixture() {
        width  = kWidth;
        height = 150; // 折叠消息 2 行/条 -> 可见 ~75 条 > 64
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
            tok += "new token content " + std::to_string(step) + " with some words " + "M"
                   + std::to_string(step) + "END ";
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
            tok += "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                   + std::to_string(step) + " ";
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
        XX_TEST_EXPECT_TRUE(ScrollFixture::lastLine(frame).find("S2ND") != std::string::npos);

        // 同流后续 token 追加 (epoch 不变) 仍正常增量显示
        f.setTokenEpoch(1, "second stream marker S2ND with more tail T3ST");
        std::string frame2 = f.render();
        XX_TEST_EXPECT_TRUE(ScrollFixture::lastLine(frame2).find("T3ST") != std::string::npos);
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

        f.comp->invalidateCache();           // 模拟主题切换: 清空渲染缓存
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
        XX_TEST_EXPECT_TRUE(ScrollFixture::lastLines(frame, 3).find("USRM") != std::string::npos);
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
            std::string s   = rebuilt;
            size_t      pos = 0, lineNo = 0;
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
        XX_TEST_EXPECT_TRUE(ScrollFixture::lastLines(frame, 3).find("USRM") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame == f.render());
    }

    {
        // 场景 9: System 消息支持折叠且默认折叠显示; 点击 header 折叠/展开
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m       = std::make_shared<TUIMessage>();
            m->role      = TUIMessage::Role::Tip;
            m->tip       = TUIMessage::TipData{};
            m->collapsed = true; // 默认折叠 (与 makeText(Role::Tip) 语义一致)
            m->text      = "tip tip body line that is long enough to be truncated "
                           "in the collapsed preview with unique tail marker "
                           "SYSM_TAIL_9XYZ";
            st.messages.push_back(std::move(m));
        });

        // 点击命中区域由上一帧 visibleBoxes 反推: 渲染两帧建立布局与命中盒
        f.render();
        f.render();

        // 折叠态: header 显示 "+ " 折叠标记 + "# " 前缀 + 单行预览,
        // 正文尾部标记 (超出 preview 截断) 不显示
        std::string collapsed1 = f.render();
        XX_TEST_EXPECT_TRUE(collapsed1.find("+ # ") != std::string::npos);
        XX_TEST_EXPECT_TRUE(collapsed1.find("SYSM_TAIL_9XYZ") == std::string::npos);

        // 模拟点击 header → 展开 (消费事件, 且消息折叠状态翻转)
        bool clicked = false;
        for (const auto& box : f.comp->collapsibleBoxes()) {
            if (box.IsEmpty()) {
                continue;
            }
            ftxui::Mouse m;
            m.button = ftxui::Mouse::Left;
            m.motion = ftxui::Mouse::Released;
            m.x      = (box.x_min + box.x_max) / 2;
            m.y      = (box.y_min + box.y_max) / 2;
            clicked  = f.comp->OnEvent(ftxui::Event::Mouse("", m));
            break;
        }
        XX_TEST_EXPECT_TRUE(clicked);

        // 展开态: "- " 展开标记 + 正文完整显示 (尾部标记可见)
        std::string expanded = f.render();
        XX_TEST_EXPECT_TRUE(expanded.find("- # ") != std::string::npos);
        XX_TEST_EXPECT_TRUE(expanded.find("SYSM_TAIL_9XYZ") != std::string::npos);

        // 状态确实更新为展开
        auto snap = f.sharedState.readSnapshot();
        XX_TEST_EXPECT_TRUE(!snap->messages.empty() && !snap->messages[0]->collapsed);

        // 再次点击 → 重新折叠 (先渲染刷新命中区域: 展开后消息更高, box 更大)
        f.render();
        clicked = false;
        for (const auto& box : f.comp->collapsibleBoxes()) {
            if (box.IsEmpty()) {
                continue;
            }
            ftxui::Mouse m;
            m.button = ftxui::Mouse::Left;
            m.motion = ftxui::Mouse::Released;
            m.x      = (box.x_min + box.x_max) / 2;
            m.y      = (box.y_min + box.y_max) / 2;
            clicked  = f.comp->OnEvent(ftxui::Event::Mouse("", m));
            break;
        }
        XX_TEST_EXPECT_TRUE(clicked);
        std::string collapsed2 = f.render();
        XX_TEST_EXPECT_TRUE(collapsed2.find("+ # ") != std::string::npos);
        XX_TEST_EXPECT_TRUE(collapsed2.find("SYSM_TAIL_9XYZ") == std::string::npos);
        snap = f.sharedState.readSnapshot();
        XX_TEST_EXPECT_TRUE(!snap->messages.empty() && snap->messages[0]->collapsed);
    }

    {
        // 场景 9b: System 消息折叠态按提示级别显示对应前缀 (Warning/Error)
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m           = std::make_shared<TUIMessage>();
            m->role          = TUIMessage::Role::Tip;
            m->tip           = TUIMessage::TipData{};
            m->tip->tipLevel = TUIMessage::TipLevel::Warning;
            m->collapsed     = true;
            m->text          = "warning tip body with marker WRN_TAIL_7K";
            st.messages.push_back(std::move(m));

            auto m2           = std::make_shared<TUIMessage>();
            m2->role          = TUIMessage::Role::Tip;
            m2->tip           = TUIMessage::TipData{};
            m2->tip->tipLevel = TUIMessage::TipLevel::Error;
            m2->collapsed     = true;
            m2->text          = "error tip body with marker ERR_TAIL_8M";
            st.messages.push_back(std::move(m2));

            auto m3       = std::make_shared<TUIMessage>();
            m3->role      = TUIMessage::Role::Tip;
            m3->tip       = TUIMessage::TipData{};
            m3->collapsed = true;
            m3->text      = "info tip body with marker INF_TAIL_6N";
            st.messages.push_back(std::move(m3));
        });
        f.render();
        f.render();
        std::string frame = f.render();
        XX_TEST_EXPECT_TRUE(frame.find("# [Warn]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("# [Error]") != std::string::npos);
        // 三条 System 消息均渲染 (折叠态预览含完整短文本 marker)
        XX_TEST_EXPECT_TRUE(frame.find("WRN_TAIL_7K") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("ERR_TAIL_8M") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("INF_TAIL_6N") != std::string::npos);
    }

    {
        // 场景 10: 多行单换行 (cmark softbreak) 的 markdown 消息 —— 高度估算
        // 必须与渲染语义一致 (段内换行合并为空格), 不得按 \n 计数。
        //
        // 回归: estimateLines 按 \n 硬换行计数, 而 DomBuilder 把段内单个换行
        // (softbreak) 合并为空格 (仅空行分隔的段落间插入 1 行空行) —— 对
        // "多行短句无空行"文本 (LLM 输出常见), 每条估算 >> 实测 (40 行 x
        // 25 字符在 97 列下估算 41 行/条, 实际合并折行 ~13 行/条)。总高度
        // 虚高 -> stickToBottom 滚动偏移过大, 顶部消息被推离视口, 视口内
        // 可见内容减少 (用户报告 "上半几条消息不渲染/像可用高度变小"),
        // 且被推出视口的项永不进入视口实测 -> 高估持续存在
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            for (int i = 0; i < 10; ++i) {
                auto m  = std::make_shared<TUIMessage>();
                m->role = TUIMessage::Role::Assistant;
                for (int j = 0; j < 40; ++j) {
                    m->text += "softbreak line " + std::to_string(j) + " msg"
                               + std::to_string(i) + "\n";
                }
                st.messages.push_back(std::move(m));
            }
        });
        // 多渲染几帧: stickToBottom 下底部可见项被实测修正, 总高度收敛到
        // 真实值附近 ((40 行 x ~27 字符 / 97 列 ≈ 12 行 + 1 空行) x 10 条
        // ≈ 130 行); 修复前按 \n 计数 ≈ 40*10 = 400+ 行
        for (int i = 0; i < 5; ++i) {
            f.render();
        }
        const int totalH = f.comp->totalHeight();
        if (totalH >= 250 || totalH <= 60) {
            fprintf(stderr, "[DBG10] softbreak totalHeight=%d (expect ~130)\n", totalH);
        }
        XX_TEST_EXPECT_TRUE(totalH < 250);
        XX_TEST_EXPECT_TRUE(totalH > 60);
        // 吸附底部: 最后一条消息完整可见 (底部贴底, 消息尾部带空行标记在倒数第二行)
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(f.render(), 2).find("msg9") != std::string::npos
        );
    }

    {
        // 场景 10b: softbreak 消息与折叠系统消息混合 —— 估算修正后滚动
        // 定位正常, 底部最新内容可见; 修复前折叠消息之后的多条 softbreak
        // 消息 (视口外高估) 会把最新消息推出视口底部
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            // 折叠的 System/Tip 消息 (单行 header)
            auto tip       = std::make_shared<TUIMessage>();
            tip->role      = TUIMessage::Role::Tip;
            tip->tip       = TUIMessage::TipData{};
            tip->collapsed = true;
            tip->text      = "collapsed tip header";
            st.messages.push_back(std::move(tip));
            // 8 条 softbreak assistant 消息
            for (int i = 0; i < 8; ++i) {
                auto m  = std::make_shared<TUIMessage>();
                m->role = TUIMessage::Role::Assistant;
                for (int j = 0; j < 30; ++j) {
                    m->text += "soft line " + std::to_string(j) + " mixed" + std::to_string(i)
                               + "\n";
                }
                st.messages.push_back(std::move(m));
            }
        });
        f.render();
        f.render();
        f.render();
        // 底部最新消息可见
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(f.render(), 2).find("mixed7") != std::string::npos
        );
    }

    {
        // 场景 11 (诊断): 混合消息类型长列表 —— 从顶部逐行滚到底部,
        // 每条消息的 marker 必须至少被渲染到屏幕一次; 若某消息在
        // 滚动过程中从未出现, 说明该消息被虚拟列表误判为不可见
        // (高度估算与实际渲染偏差导致 continue/break 误判), 即用户
        // 报告的"某些消息显示为空白, 滑动到某些位置又正常"
        ScrollFixture f;
        std::vector<std::string> markers;
        auto mk = [&](int i, const char* tag) {
            return fmt::format("MMK{}_{}_", i, tag);
        };

        f.sharedState.mutate([&](TUIRenderState& st) {
            for (int i = 0; i < 24; ++i) {
                auto m  = std::make_shared<TUIMessage>();
                const std::string marker = mk(i, "A");
                markers.push_back(marker);
                switch (i % 6) {
                    case 0: { // 普通 markdown 段落
                        m->role = TUIMessage::Role::Assistant;
                        m->text = "plain paragraph with marker " + marker + "\n\n"
                                  "second paragraph with some more text to wrap around.\n";
                        break;
                    }
                    case 1: { // mermaid 状态图 (planning 常用)
                        m->role = TUIMessage::Role::Assistant;
                        m->text = "```mermaid\nstateDiagram-v2\n"
                                  "    [*] --> phase_a\n"
                                  "    phase_a --> phase_b\n"
                                  "    phase_b --> phase_c\n"
                                  "    phase_c --> [*]\n"
                                  "```\n"
                                  "after mermaid marker " + marker + "\n";
                        break;
                    }
                    case 2: { // 表格 (单元格长文本 -> 受限列宽自动换行)
                        m->role = TUIMessage::Role::Assistant;
                        m->text = "| col1 | col2 |\n|---|---|\n"
                                  "| " + marker + " | " + marker + " |\n";
                        break;
                    }
                    case 3: { // 长串无空格文本 (flexbox 不折行, 按列估算会高估)
                        m->role = TUIMessage::Role::Assistant;
                        m->text = std::string(200, 'x') + " " + marker + "\n";
                        break;
                    }
                    case 4: { // User 多行
                        m->role = TUIMessage::Role::User;
                        m->text = "user line 1\nuser line 2 marker " + marker + "\nuser line 3\n";
                        break;
                    }
                    case 5: { // Tool 展开 (多行 JSON 参数 + 长 result)
                        m->role = TUIMessage::Role::Tool;
                        m->tool = TUIMessage::ToolData{};
                        m->tool->toolName      = "agentxx_filesystem_read";
                        m->tool->toolFinished  = true;
                        m->tool->toolResult    = "line1 result marker " + marker + "\n"
                                                 "line2 with some words to wrap if narrow\n"
                                                 "line3\n";
                        m->text                = "{\n  \"path\": \"/a/b/c\",\n  \"line_offset\": 0,\n"
                                                 "  \"line_limit\": 100\n}";
                        m->collapsed           = false;
                        break;
                    }
                }
                st.messages.push_back(std::move(m));
            }
        });

        f.render(); // 建立缓存与布局 (stickToBottom 默认)

        // 滚到顶部 (每次滚 1 行, 直到不再变化)
        f.comp->setStickToBottom(false);
        {
            int lastOffset = -1;
            for (int i = 0; i < 2000; ++i) {
                f.render();
                f.wheel(ftxui::Mouse::WheelUp);
                const int off = f.comp->scrollOffset();
                if (off == lastOffset && off == 0) {
                    break; // 已到顶部
                }
                lastOffset = off;
            }
        }

        // 从顶部逐行向下滚动, 收集每条消息 marker 是否出现过
        std::set<std::string> unseen(markers.begin(), markers.end());
        for (int off = 0; off < 4000; ++off) {
            std::string frame = f.render();
            for (auto it = unseen.begin(); it != unseen.end();) {
                if (frame.find(*it) != std::string::npos) {
                    it = unseen.erase(it);
                } else {
                    ++it;
                }
            }
            if (unseen.empty()) {
                break;
            }
            if (!f.wheel(ftxui::Mouse::WheelDown)) {
                break; // 滚不动 (已到底)
            }
        }
        // 最后一帧 (底部)
        {
            std::string frame = f.render();
            for (auto it = unseen.begin(); it != unseen.end();) {
                if (frame.find(*it) != std::string::npos) {
                    it = unseen.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (!unseen.empty()) {
            fprintf(stderr, "[DBG11] %zu messages never rendered:\n", unseen.size());
            for (const auto& m : unseen) {
                fprintf(stderr, "  missing: %s\n", m.c_str());
            }
            fprintf(stderr, "[DBG11] totalHeight=%d\n", f.comp->totalHeight());
        }
        XX_TEST_EXPECT_TRUE(unseen.empty());
    }

    {
        // 场景 12 (诊断): mermaid 状态图消息 (估算按代码块行数, 实际图形
        // 高度远大于源行数 -> 严重低估) + 长对话 + stickToBottom ——
        // 底部最新消息必须完整可见; 若 mermaid 低估, totalHeight 偏小,
        // offset 偏小, 底部最新消息被推出视口 (用户报告的"某些消息
        // 显示为空白, 滑动到某些位置又正常")
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            // 前面 300 条普通段落消息 + mermaid 在中间 + 后面 5 条普通消息:
            // mermaid 估算位置 (源 8 行) 落在视口上方被 continue 跳过 (不实测),
            // 但其实际高度 24 行 -> 总高度低估 -> 底部内容被推出视口,
            // 且 mermaid 区域 (视口内) 显示为空白
            auto addHistory = [&](int n, int base) {
                for (int i = 0; i < n; ++i) {
                    auto m  = std::make_shared<TUIMessage>();
                    m->role = TUIMessage::Role::Assistant;
                    m->text = "history paragraph " + std::to_string(base + i)
                              + " with enough words to wrap around the width nicely "
                                "and fill several lines of the screen area.\n";
                    st.messages.push_back(std::move(m));
                }
            };
            addHistory(300, 0);
            auto mm = std::make_shared<TUIMessage>();
            mm->role = TUIMessage::Role::Assistant;
            mm->text = "```mermaid\nstateDiagram-v2\n"
                       "    [*] --> phase_a\n"
                       "    phase_a --> phase_b\n"
                       "    phase_b --> phase_c\n"
                       "    phase_c --> [*]\n"
                       "```\n";
            st.messages.push_back(std::move(mm));
            addHistory(5, 1000);
            // 最后一条普通消息 (marker 必须在底部可见)
            auto last  = std::make_shared<TUIMessage>();
            last->role = TUIMessage::Role::Assistant;
            last->text = "final message with unique marker LAST_MK_9ZQ\n";
            st.messages.push_back(std::move(last));
        });

        for (int i = 0; i < 8; ++i) {
            f.render(); // stickToBottom 收敛
        }
        std::string frame = f.render();
        // mermaid 状态图必须渲染在视口中 (修复前整图缺失 -> 视口内一段空白;
        // 图中任一节点/箭头可见即说明被布局渲染; 视口只显示图的一部分)
        XX_TEST_EXPECT_TRUE(frame.find("[*]") != std::string::npos);
        // mermaid 之后的普通消息必须正常显示 (不被 mermaid 低估的高度错位)
        XX_TEST_EXPECT_TRUE(frame.find("history paragraph 1000") != std::string::npos);
        // 底部最新消息必须完整可见
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(frame, 4).find("LAST_MK_9ZQ") != std::string::npos
        );
        if (frame.find("LAST_MK_9ZQ") == std::string::npos) {
            fprintf(
                stderr,
                "[DBG12] last message NOT visible! totalHeight=%d\n",
                f.comp->totalHeight()
            );
            fprintf(stderr, "[DBG12] frame:\n%s\n", frame.c_str());
        }
        XX_TEST_EXPECT_TRUE(ScrollFixture::lastLines(frame, 4).find("LAST_MK_9ZQ") != std::string::npos);
    }

    {
        // 场景 13: 单条消息的估算 == 实测高度 (估算算法与渲染语义一致性回归)
        // - 普通段落消息: 内容折行 + 尾部空行 = 3 行
        // - mermaid 状态图: 源 7 行 -> 图形 24 行 (估算公式 源行数×3+3 = 24)
        // - softbreak 多行: 段内换行合并为空格, 1 行 + 尾部空行 = 2 行
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::Assistant;
            m->text = "history paragraph 0 with enough words to wrap around the width nicely "
                      "and fill several lines of the screen area.\n";
            st.messages.push_back(std::move(m));
        });
        f.render();
        XX_TEST_EXPECT_TRUE(f.comp->totalHeight() == 3);

        ScrollFixture f2;
        f2.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::Assistant;
            m->text = "```mermaid\nstateDiagram-v2\n"
                      "    [*] --> phase_a\n"
                      "    phase_a --> phase_b\n"
                      "    phase_b --> phase_c\n"
                      "    phase_c --> [*]\n"
                      "```\n";
            st.messages.push_back(std::move(m));
        });
        f2.render();
        XX_TEST_EXPECT_TRUE(f2.comp->totalHeight() == 24);

        ScrollFixture f3;
        f3.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::Assistant;
            m->text = "line one\nline two\nline three\nline four\nline five\n";
            st.messages.push_back(std::move(m));
        });
        f3.render();
        XX_TEST_EXPECT_TRUE(f3.comp->totalHeight() == 2);
    }

    {
        // 场景 14 (诊断): filesystem_edit Tool 消息展开 —— 实际渲染走
        // renderEditToolDiff (按 oldStr/newStr 差异逐行渲染), 而估算按
        // args JSON 行数 + toolResult 行数 —— 大 diff 时严重低估。
        // 与场景 12 相同机制: 低估 -> totalHeight 偏低 -> stickToBottom
        // 偏移偏小 -> 底部最新消息被推出视口 / edit Tool 被 continue 跳过
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            // 普通历史消息 (估算准确, 撑高列表)
            for (int i = 0; i < 300; ++i) {
                auto m  = std::make_shared<TUIMessage>();
                m->role = TUIMessage::Role::Assistant;
                m->text = "history paragraph " + std::to_string(i)
                          + " with enough words to wrap around the width nicely "
                            "and fill several lines of the screen area.\n";
                st.messages.push_back(std::move(m));
            }
            // 中间一条 filesystem_edit Tool 消息 (展开, 大 diff)
            auto et  = std::make_shared<TUIMessage>();
            et->role = TUIMessage::Role::Tool;
            et->tool = TUIMessage::ToolData{};
            et->tool->toolName     = "agentxx_filesystem_edit";
            et->tool->toolFinished = true;
            et->tool->toolResult   = "Success, Replace 1 hits"; // 成功 (非错误前缀)
            et->collapsed          = false;
            std::string oldStr, newStr;
            for (int j = 0; j < 14; ++j) {
                oldStr += "old line " + std::to_string(j) + "\n";
                newStr += "new line " + std::to_string(j) + "\n";
            }
            newStr += "diff marker DL_MK_7ZZ\n";
            // 构造合法 JSON args (换行必须转义为 \\n, 否则解析失败回退)
            std::string argsJson = "{\"path\":\"/a/b.txt\",\"old_str\":\"";
            for (char c : oldStr) {
                if (c == '\n') {
                    argsJson += "\\n";
                } else {
                    argsJson += c;
                }
            }
            argsJson += "\",\"new_str\":\"";
            for (char c : newStr) {
                if (c == '\n') {
                    argsJson += "\\n";
                } else {
                    argsJson += c;
                }
            }
            argsJson += "\"}";
            et->text = std::move(argsJson);
            st.messages.push_back(std::move(et));
            // 后面 5 条历史 + last
            for (int i = 0; i < 5; ++i) {
                auto m  = std::make_shared<TUIMessage>();
                m->role = TUIMessage::Role::Assistant;
                m->text = "tail paragraph " + std::to_string(i)
                          + " with enough words to wrap around the width nicely.\n";
                st.messages.push_back(std::move(m));
            }
            auto last  = std::make_shared<TUIMessage>();
            last->role = TUIMessage::Role::Assistant;
            last->text = "final message with unique marker LAST_MK_8XY\n";
            st.messages.push_back(std::move(last));
        });

        for (int i = 0; i < 8; ++i) {
            f.render(); // stickToBottom 收敛
        }
        std::string frame = f.render();
        // filesystem_edit 的 diff 内容必须渲染在视口中 (修复前整块丢失 -> 空白)
        XX_TEST_EXPECT_TRUE(frame.find("DL_MK_7ZZ") != std::string::npos);
        // 底部最新消息必须完整可见
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(frame, 4).find("LAST_MK_8XY") != std::string::npos
        );
    }

    {
        // 场景 15a (诊断): 单条 Tool 折叠消息的渲染结果
        ScrollFixture g;
        g.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->collapsed = true;
            m->role = TUIMessage::Role::Tool;
            m->tool = TUIMessage::ToolData{};
            m->tool->toolName     = "agentxx_filesystem_read";
            m->tool->toolFinished = true;
            m->tool->toolResult   = "some result with marker COLX_ZZ";
            m->text               = "{\"path\":\"/a/b/c\"}";
            st.messages.push_back(std::move(m));
        });
        g.render();
        std::string gframe = g.render();
        fprintf(
            stderr,
            "[DBG15a] single collapsed Tool: totalH=%d frame:\n%s\n",
            g.comp->totalHeight(),
            gframe.c_str()
        );
    }

    {
        // 场景 15 (诊断): 全折叠消息列表 (Think/Tool/System 折叠, 仅 header) +
        // 正常上下滚动 —— 所有折叠 header 必须渲染; 复现用户报告
        // "消息都是折叠的, 上下滚动到某些位置时连续几条不显示, 但可以
        // 点击展开, 滚动超过一段距离又好了"。折叠消息估算 2 行 == 实测,
        // 若仍出现缺失, 说明问题不在高度估算, 而在布局/绘制环节
        ScrollFixture f;
        std::vector<std::string> markers;
        f.sharedState.mutate([&](TUIRenderState& st) {
            for (int i = 0; i < 60; ++i) {
                const std::string mk = "COL_" + std::to_string(i) + "_ZQ";
                markers.push_back(mk);
                auto m  = std::make_shared<TUIMessage>();
                m->collapsed = true; // 全部折叠
                if (i % 3 == 0) {
                    // Think 折叠: header "[Think] <时长>" + 折叠预览(正文单行截断)
                    m->role       = TUIMessage::Role::Think;
                    m->durationMs = 1000 + i;
                    m->text       = "think body content with marker " + mk + " and lots of "
                                    "text to make preview truncate at the right edge ...";
                } else if (i % 3 == 1) {
                    // Tool 折叠: header 显示摘要 (buildToolHeaderSummary);
                    // marker 放入 path, 摘要 "Read · /a/COL_x_ZQ" 会显示出来
                    m->role                = TUIMessage::Role::Tool;
                    m->tool                = TUIMessage::ToolData{};
                    m->tool->toolName      = "agentxx_filesystem_read";
                    m->tool->toolFinished  = true;
                    m->tool->toolResult    = "some result text";
                    m->text                = "{\"path\":\"/a/" + mk + "\"}";
                } else {
                    // System/Tip 折叠
                    m->role = TUIMessage::Role::Tip;
                    m->tip  = TUIMessage::TipData{};
                    m->text = "system tip message with marker " + mk;
                }
                st.messages.push_back(std::move(m));
            }
        });

        f.render();
        // 从顶部逐行滚动到底部, 收集每个折叠 header 的 marker
        std::set<std::string> unseen(markers.begin(), markers.end());
        f.comp->setStickToBottom(false);
        {
            int lastOffset = -1;
            for (int i = 0; i < 2000; ++i) {
                f.render();
                f.wheel(ftxui::Mouse::WheelUp);
                const int off = f.comp->scrollOffset();
                if (off == lastOffset && off == 0) {
                    break;
                }
                lastOffset = off;
            }
        }
        for (int off = 0; off < 4000; ++off) {
            std::string frame = f.render();
            for (auto it = unseen.begin(); it != unseen.end();) {
                if (frame.find(*it) != std::string::npos) {
                    it = unseen.erase(it);
                } else {
                    ++it;
                }
            }
            if (unseen.empty()) {
                break;
            }
            if (!f.wheel(ftxui::Mouse::WheelDown)) {
                break;
            }
        }
        {
            std::string frame = f.render();
            for (auto it = unseen.begin(); it != unseen.end();) {
                if (frame.find(*it) != std::string::npos) {
                    it = unseen.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!unseen.empty()) {
            fprintf(stderr, "[DBG15] %zu folded messages never rendered:\n", unseen.size());
            for (const auto& m : unseen) {
                fprintf(stderr, "  missing: %s\n", m.c_str());
            }
            fprintf(stderr, "[DBG15] totalHeight=%d scrollOffset=%d\n",
                    f.comp->totalHeight(), f.comp->scrollOffset());
        }
        XX_TEST_EXPECT_TRUE(unseen.empty());
    }

    {
        // 场景 16 (诊断): 大量折叠消息 + 间隔插入的展开高消息 (mermaid/edit
        // diff) —— 模拟真实长对话; 逐行滚动遍历, 所有折叠 header 必须渲染
        // (折叠消息估算准确, 若高消息低估导致位置错位, 后续连续折叠消息
        //  会在某些滚动位置显示为空白, 滚动大段距离后恢复 —— 用户报告)
        ScrollFixture f;
        std::vector<std::string> markers;
        f.sharedState.mutate([&](TUIRenderState& st) {
            int msgNo = 0;
            for (int i = 0; i < 96; ++i) {
                // 每 16 条插入一条展开的高消息 (低估源)
                if (i > 0 && i % 16 == 0) {
                    auto hm = std::make_shared<TUIMessage>();
                    hm->role = TUIMessage::Role::Assistant;
                    if (i % 32 == 0) {
                        // mermaid 状态图 (展开)
                        hm->text = "```mermaid\nstateDiagram-v2\n"
                                   "    [*] --> p_a\n    p_a --> p_b\n    p_b --> p_c\n"
                                   "    p_c --> [*]\n```\n";
                    } else {
                        // filesystem_edit diff (展开)
                        hm->role = TUIMessage::Role::Tool;
                        hm->tool = TUIMessage::ToolData{};
                        hm->tool->toolName     = "agentxx_filesystem_edit";
                        hm->tool->toolFinished = true;
                        hm->tool->toolResult   = "Success, Replace 1 hits";
                        hm->collapsed          = false;
                        std::string os, ns;
                        for (int j = 0; j < 10; ++j) {
                            os += "old line " + std::to_string(j) + "\n";
                            ns += "new line " + std::to_string(j) + "\n";
                        }
                        std::string aj = "{\"path\":\"/a/f.txt\",\"old_str\":\"";
                        for (char c : os) {
                            if (c == '\n') {
                                aj += "\\n";
                            } else {
                                aj += c;
                            }
                        }
                        aj += "\",\"new_str\":\"";
                        for (char c : ns) {
                            if (c == '\n') {
                                aj += "\\n";
                            } else {
                                aj += c;
                            }
                        }
                        aj += "\"}";
                        hm->text = std::move(aj);
                    }
                    st.messages.push_back(std::move(hm));
                    continue;
                }
                // 折叠消息
                const std::string mk = "MX" + std::to_string(msgNo++) + "_KQ";
                markers.push_back(mk);
                auto m       = std::make_shared<TUIMessage>();
                m->collapsed = true;
                if (msgNo % 3 == 0) {
                    m->role       = TUIMessage::Role::Think;
                    m->durationMs = 1000 + msgNo;
                    m->text       = "think body with marker " + mk + " ...";
                } else if (msgNo % 3 == 1) {
                    m->role               = TUIMessage::Role::Tool;
                    m->tool               = TUIMessage::ToolData{};
                    m->tool->toolName     = "agentxx_filesystem_read";
                    m->tool->toolFinished = true;
                    m->tool->toolResult   = "ok";
                    m->text               = "{\"path\":\"/a/" + mk + "\"}";
                } else {
                    m->role = TUIMessage::Role::Tip;
                    m->tip  = TUIMessage::TipData{};
                    m->text = "tip with marker " + mk;
                }
                st.messages.push_back(std::move(m));
            }
        });

        f.render();
        std::set<std::string> unseen(markers.begin(), markers.end());
        f.comp->setStickToBottom(false);
        {
            int lastOffset = -1;
            for (int i = 0; i < 3000; ++i) {
                f.render();
                f.wheel(ftxui::Mouse::WheelUp);
                const int off = f.comp->scrollOffset();
                if (off == lastOffset && off == 0) {
                    break;
                }
                lastOffset = off;
            }
        }
        for (int off = 0; off < 6000; ++off) {
            std::string frame = f.render();
            for (auto it = unseen.begin(); it != unseen.end();) {
                if (frame.find(*it) != std::string::npos) {
                    it = unseen.erase(it);
                } else {
                    ++it;
                }
            }
            if (unseen.empty()) {
                break;
            }
            if (!f.wheel(ftxui::Mouse::WheelDown)) {
                break;
            }
        }
        {
            std::string frame = f.render();
            for (auto it = unseen.begin(); it != unseen.end();) {
                if (frame.find(*it) != std::string::npos) {
                    it = unseen.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!unseen.empty()) {
            fprintf(stderr, "[DBG16] %zu folded messages never rendered:\n", unseen.size());
            for (const auto& m : unseen) {
                fprintf(stderr, "  missing: %s\n", m.c_str());
            }
            fprintf(stderr, "[DBG16] totalHeight=%d\n", f.comp->totalHeight());
        }
        XX_TEST_EXPECT_TRUE(unseen.empty());
    }

    {
        // 场景 17 (回归): 预算淘汰不得移除"本帧已确保/待渲染"的可见子项
        //
        // 机制: buildItem 对内容较大的子项按源字节 ×64 上报 sourceBytes
        // (消息列表: 见 buildMessageItem), 当视口内可见子项的总估算字节
        // 超过 maxBytes (长代码 / JSON / 工具结果 / diff) —— 或可见条数
        // 超过 maxItems (高终端) —— 时, ensureElement 插入新子项触发的
        // evictIfNeeded 会持续从 LRU 尾部淘汰直至预算达标。尾部先消耗
        // 视口外的旧缓存, 耗尽后即命中"本帧阶段 1/2 已处理过、仍待渲染"
        // 的可见子项: hasCache_ 被置 false 但索引仍在 visibleIndices_
        // (visibleBoxes_ 有盒 -> 点击折叠/展开仍有效), 渲染时 elementAt
        // 回退空 text -> 连续多条消息显示为空白; 预算由可见集自身超限,
        // 每帧稳定复现, 滚动改变可见集后消失/恢复 (用户报告: "滚动到
        // 一定位置时连续几条消息不显示, 但可以点击展开和折叠, 再滚动
        // 就恢复")。修复前本场景的视口顶部连续标记缺失。
        //
        // 直接构造 LazyScrollable: 预算 (maxItems=8, maxBytes=2000,
        // byteExemptThreshold=0) 远小于可见集 (30 行视口, 单行子项 x ~42
        // 项 x sourceBytes=200 = ~8400), key 恒定无内容变更干扰, 使淘汰
        // 必然触及可见子项
        auto scrollable = std::make_shared<LazyScrollable>(
            [] { return static_cast<size_t>(60); }, // 60 个子项 (列表 60 行)
            [](size_t i) { return 0x1000ULL + i; }, // key 恒定
            [](size_t, int) { return 1; },          // 单行子项 (估算==实测)
            [](size_t i) {
                LazyBuiltItem b;
                b.element     = ftxui::text(
                    "MT17 item " + std::to_string(i) + " marker=" + std::to_string(i)
                );
                b.sourceBytes = 200; // 模拟 x64 折算后的较大渲染树
                return b;
            },
            LazyScrollable::CacheBudget{8, 2000, 0}, // maxItems, maxBytes, exempt=0
            nullptr
        );
        scrollable->setStickToBottom(false); // 固定视口在顶部 (offset=0)
        auto   el     = scrollable->Render() | ftxui::flex;
        auto   screen = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(80),
            ftxui::Dimension::Fixed(30)
        );
        ftxui::Render(screen, el);
        std::string out = screen.ToString();
        // 视口 (offset 0) 内 0..29 条全部必须渲染 (顶部连续标记缺失即回归)
        std::string missing;
        for (int i = 0; i < 30; ++i) {
            const std::string marker = "marker=" + std::to_string(i);
            if (out.find(marker) == std::string::npos) {
                missing += (missing.empty() ? "" : ",") + std::to_string(i);
            }
        }
        if (!missing.empty()) {
            fprintf(stderr, "[DBG17] missing visible markers: %s\n", missing.c_str());
        }
        XX_TEST_EXPECT_TRUE(missing.empty());
    }

    {
        // 场景 17b (回归): 高视口 (可见条数 > maxItems=64) 全折叠消息列表,
        // 顶部连续折叠消息必须渲染 —— 消息列表级复现场景 17 的同一机制
        // (count 预算淘汰误伤可见子项), 用户报告的正是折叠消息场景
        TallScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            for (int i = 0; i < 200; ++i) {
                const std::string mk = "T17B_" + std::to_string(i) + "_END";
                auto              m  = std::make_shared<TUIMessage>();
                m->collapsed        = true;
                m->role             = TUIMessage::Role::Tip;
                m->tip              = TUIMessage::TipData{};
                m->text             = "tip " + mk;
                st.messages.push_back(std::move(m));
            }
        });
        f.comp->setStickToBottom(false); // 固定视口在顶部 (offset=0)
        f.render();                      // 建立缓存与布局
        f.render();                      // 再渲染 (全部缓存命中路径)
        std::string frame = f.render();
        // 折叠消息 2 行/条, 150 行视口可见 0..74; 顶部消息被预算淘汰 ->
        // 空白 (修复前 T17B_0..T17B_10 丢失)
        std::string missing;
        for (int i = 0; i < 75; ++i) {
            const std::string mk = "T17B_" + std::to_string(i) + "_END";
            if (frame.find(mk) == std::string::npos) {
                missing += (missing.empty() ? "" : ",") + std::to_string(i);
            }
        }
        if (!missing.empty()) {
            fprintf(stderr, "[DBG17b] missing visible collapsed markers: %s\n", missing.c_str());
        }
        XX_TEST_EXPECT_TRUE(missing.empty());
    }

    {
        // 场景 18: TailThinkingMode 流式与末尾折叠预览测试
        auto& settings = TUISettings::instance();
        const auto origMode = settings.tailThinkingMode();

        // 18a: SingleLine 模式下流式 Think 显示单行折叠并截取末尾字符
        settings.setTailThinkingMode(TailThinkingMode::SingleLine);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                st.currentTokenRole = TUIMessage::Role::Think;
                st.currentToken     = std::make_shared<std::string>(
                    "Thinking step one\nThinking step two\nThinking step three final tail marker THK_TAIL_18A"
                );
                st.isStreaming = true;
            });
            std::string frame = f.render();
            // 单行折叠标志 "+ [Think]"
            XX_TEST_EXPECT_TRUE(frame.find("+ [Think]") != std::string::npos);
            // 包含末尾字符 marker
            XX_TEST_EXPECT_TRUE(frame.find("THK_TAIL_18A") != std::string::npos);
            // 不应为展开标志 "- [Think]"
            XX_TEST_EXPECT_TRUE(frame.find("- [Think]") == std::string::npos);
        }

        // 18b: AutoExpand 模式下流式 Think 自动展开显示
        settings.setTailThinkingMode(TailThinkingMode::AutoExpand);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                st.currentTokenRole = TUIMessage::Role::Think;
                st.currentToken     = std::make_shared<std::string>(
                    "Thinking step one auto expand header\nThinking step two\nThinking step three THK_TAIL_18B"
                );
                st.isStreaming = true;
            });
            std::string frame = f.render();
            // 自动展开标志 "- [Think]"
            XX_TEST_EXPECT_TRUE(frame.find("- [Think]") != std::string::npos);
            // 包含正文内容
            XX_TEST_EXPECT_TRUE(frame.find("Thinking step one auto expand header") != std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("THK_TAIL_18B") != std::string::npos);
        }

        // 18c: 历史末尾 Think 消息在 SingleLine 模式下折叠预览显示末尾截取
        settings.setTailThinkingMode(TailThinkingMode::SingleLine);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                auto m = std::make_shared<TUIMessage>();
                m->role = TUIMessage::Role::Think;
                m->collapsed = true;
                m->text = "First line head of thinking that is very long\nSecond line\nTail line with marker THK_TAIL_18C";
                st.messages.push_back(std::move(m));
            });
            std::string frame = f.render();
            XX_TEST_EXPECT_TRUE(frame.find("+ [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("THK_TAIL_18C") != std::string::npos);
        }

        // 点击流式末尾 Think 的辅助: 点击第一个非空命中区中心 (返回是否消费)
        auto clickFirstCollapsible = [](ScrollFixture& fx) -> bool {
            for (const auto& box : fx.comp->collapsibleBoxes()) {
                if (box.IsEmpty()) {
                    continue;
                }
                ftxui::Mouse m;
                m.button = ftxui::Mouse::Left;
                m.motion = ftxui::Mouse::Released;
                m.x      = (box.x_min + box.x_max) / 2;
                m.y      = (box.y_min + box.y_max) / 2;
                return fx.comp->OnEvent(ftxui::Event::Mouse("", m));
            }
            return false;
        };

        // 18d: SingleLine 模式下点击流式末尾 Think → 展开, 再点击 → 折叠
        settings.setTailThinkingMode(TailThinkingMode::SingleLine);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                st.currentTokenRole  = TUIMessage::Role::Think;
                st.currentTokenEpoch = 7;
                st.currentToken      = std::make_shared<std::string>(
                    "Thinking head line hidden when collapsed THK18D_HEAD\n"
                    "Thinking step two\nThinking step three tail THK_TAIL_18D"
                );
                st.isStreaming = true;
            });
            // 两帧建立布局与命中区域
            std::string frame = f.render();
            frame             = f.render();
            XX_TEST_EXPECT_TRUE(frame.find("+ [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("THK18D_HEAD") == std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("THK_TAIL_18D") != std::string::npos);
            // 流式区子项登记为可点击命中区 (isStream 标记)
            XX_TEST_EXPECT_TRUE(f.comp->collapsibleBoxes().size() == 1);
            XX_TEST_EXPECT_TRUE(f.comp->collapsibleIsStream(0));

            // 点击 → 展开 (显示 "- [Think]" 与全文头部)
            XX_TEST_EXPECT_TRUE(clickFirstCollapsible(f));
            std::string expanded = f.render();
            XX_TEST_EXPECT_TRUE(expanded.find("- [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(expanded.find("THK18D_HEAD") != std::string::npos);

            // 刷新命中区后再点击 → 折叠回单行预览
            f.render();
            XX_TEST_EXPECT_TRUE(clickFirstCollapsible(f));
            std::string collapsedAgain = f.render();
            XX_TEST_EXPECT_TRUE(collapsedAgain.find("+ [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(collapsedAgain.find("THK18D_HEAD") == std::string::npos);
            XX_TEST_EXPECT_TRUE(collapsedAgain.find("THK_TAIL_18D") != std::string::npos);
        }

        // 18e: AutoExpand 模式下点击流式末尾 Think → 折叠, 再点击 → 展开
        settings.setTailThinkingMode(TailThinkingMode::AutoExpand);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                st.currentTokenRole  = TUIMessage::Role::Think;
                st.currentTokenEpoch = 8;
                st.currentToken      = std::make_shared<std::string>(
                    "Auto expand head line THK18E_HEAD\n"
                    "Thinking step two\nThinking step three tail THK_TAIL_18E"
                );
                st.isStreaming = true;
            });
            std::string frame = f.render();
            frame             = f.render();
            XX_TEST_EXPECT_TRUE(frame.find("- [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("THK18E_HEAD") != std::string::npos);

            // 点击 → 折叠为单行末尾截取预览 (头部隐藏, 尾部可见)
            XX_TEST_EXPECT_TRUE(clickFirstCollapsible(f));
            std::string collapsed = f.render();
            XX_TEST_EXPECT_TRUE(collapsed.find("+ [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(collapsed.find("THK18E_HEAD") == std::string::npos);
            XX_TEST_EXPECT_TRUE(collapsed.find("THK_TAIL_18E") != std::string::npos);

            // 刷新命中区后再点击 → 恢复展开
            f.render();
            XX_TEST_EXPECT_TRUE(clickFirstCollapsible(f));
            std::string expandedAgain = f.render();
            XX_TEST_EXPECT_TRUE(expandedAgain.find("- [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(expandedAgain.find("THK18E_HEAD") != std::string::npos);
        }

        // 18f: 流结束提交后覆盖态重置 —— 新一轮思考回到设置模式默认展示
        settings.setTailThinkingMode(TailThinkingMode::AutoExpand);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                st.currentTokenRole  = TUIMessage::Role::Think;
                st.currentTokenEpoch = 3;
                st.currentToken      = std::make_shared<std::string>(
                    "Stream one head THK18F_HEAD\nStream one tail THK_TAIL_18F"
                );
                st.isStreaming = true;
            });
            f.render();
            f.render();
            // 点击折叠流式 think
            XX_TEST_EXPECT_TRUE(clickFirstCollapsible(f));
            XX_TEST_EXPECT_TRUE(f.render().find("+ [Think]") != std::string::npos);

            // 流结束: token 提交为正式 Think 消息 (pushCurrentTokenLocked 语义,
            // 默认 collapsed=true), 覆盖态应随流结束重置
            f.sharedState.mutate([&](TUIRenderState& st) {
                auto m       = std::make_shared<TUIMessage>();
                m->role      = TUIMessage::Role::Think;
                m->collapsed = true;
                m->text      = st.currentToken ? *st.currentToken : "";
                st.messages.push_back(std::move(m));
                st.currentToken.reset();
                st.isStreaming = false;
            });
            std::string committedFrame = f.render();
            XX_TEST_EXPECT_TRUE(committedFrame.find("+ [Think]") != std::string::npos);

            // 新一轮思考开始 (新 epoch): 不沿用上一流的折叠覆盖态, 自动展开
            f.sharedState.mutate([&](TUIRenderState& st) {
                st.currentTokenRole  = TUIMessage::Role::Think;
                st.currentTokenEpoch = 9;
                st.currentToken      = std::make_shared<std::string>(
                    "Stream two head THK18F_HEAD2\nStream two tail THK_TAIL_18F2"
                );
                st.isStreaming = true;
            });
            std::string nextStreamFrame = f.render();
            nextStreamFrame             = f.render();
            XX_TEST_EXPECT_TRUE(nextStreamFrame.find("- [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(nextStreamFrame.find("THK18F_HEAD2") != std::string::npos);
        }

        // 恢复原始设置
        settings.setTailThinkingMode(origMode);
    }

    return TestResult{g_tui_scroll_passed, g_tui_scroll_failed};
}

} // namespace test
} // namespace agentxx
