#include "test_tui_input.h"

#include "agentxx-client/io/tui/components/input_bar.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "ftxui/component/animation.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/screen.hpp"
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_input_passed = 0;
int g_tui_input_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_input_passed
#define XX_TEST_FAILED g_tui_input_failed

namespace agentxx {
namespace test {

// ---------------------------------------------------------------------------
// 测试夹具: 构建最小 TUICtx + InputComponent, 直接驱动 OnEvent
// ---------------------------------------------------------------------------

namespace {

struct InputFixture {
    TUISharedState sharedState;
    TUITheme       theme       = TUITheme::darkTheme();
    int            redrawCount = 0;
    std::string    sentText;
    bool           sent = false;

    TUICtx ctx;

    InputFixture() {
        ctx.state      = &sharedState;
        ctx.frameState = sharedState.readSnapshot();
        ctx.postRedraw = [this] {
            ++redrawCount;
        };
        ctx.theme     = &theme;
        ctx.sessionId = "session";
        ctx.remoteUrl = "";
    }

    /// 创建组件; ComponentBase 不可移动, 使用 shared_ptr 持有
    std::shared_ptr<InputComponent> makeComponent() {
        InputComponent::Config cfg;
        cfg.onSend = [this](std::string text) -> bool {
            sentText = std::move(text);
            sent     = true;
            return true;
        };
        return std::make_shared<InputComponent>(ctx, std::move(cfg));
    }

    /// 依次输入字符串的每个字符
    static void type(InputComponent& comp, const std::string& s) {
        for (char c : s) {
            comp.OnEvent(ftxui::Event::Character(c));
        }
    }

    /// 模拟一次括号粘贴: 终端启用 \x1B[?2004h 后发送 \x1B[200~ ... \x1B[201~,
    /// 内容中的换行以 \r 传输 (解析后为 Event::Return)
    static void paste(InputComponent& comp, const std::string& text) {
        comp.OnEvent(ftxui::Event::Special("\x1B[200~"));
        for (char c : text) {
            if (c == '\n') {
                comp.OnEvent(ftxui::Event::Return);
            } else {
                comp.OnEvent(ftxui::Event::Character(c));
            }
        }
        comp.OnEvent(ftxui::Event::Special("\x1B[201~"));
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 测试用例
// ---------------------------------------------------------------------------

void test_multiline_paste_inserted_not_sent() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 括号粘贴 "line1\rline2" (终端以 \r 表示换行)
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    InputFixture::type(*comp, "line1");
    comp->OnEvent(ftxui::Event::Return); // 粘贴的换行
    InputFixture::type(*comp, "line2");
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));

    // 多行内容被整体插入, 换行不会触发发送
    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("line1\nline2"));
    XX_TEST_EXPECT_FALSE(f.sent);
}

void test_paste_crlf_dedup() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 粘贴内容中的空行 (\n\n) 必须原样保留。
    // 背景: FTXUI 解析层已把 \r 归一化为 \n, 事件层面无法区分
    // "CRLF 的第二个 \n" 与 "粘贴内容中真实的空行" —— 去重会吞掉空行
    // (如代码块中的空行), 代价大于个别 Windows 终端 (CRLF 粘贴) 多出的空行,
    // 故不做去重, 全部换行原样保留。
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    comp->OnEvent(ftxui::Event::Character('a'));
    comp->OnEvent(ftxui::Event::Return); // 行尾换行
    comp->OnEvent(ftxui::Event::Return); // 空行 (原 CRLF 去重场景, 现保留)
    comp->OnEvent(ftxui::Event::Character('b'));
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));

    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("a\n\nb"));
    XX_TEST_EXPECT_FALSE(f.sent);
}

void test_paste_preserves_tab() {
    InputFixture f;
    auto         comp = f.makeComponent();

    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    comp->OnEvent(ftxui::Event::Tab); // 粘贴中的 Tab 应原样保留
    InputFixture::type(*comp, "foo");
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));

    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("\tfoo"));
}

void test_paste_inserts_at_cursor() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // "ab", 光标左移一位, 粘贴 "XY" → 插入到光标处 → "aXYb"
    InputFixture::type(*comp, "ab");
    comp->OnEvent(ftxui::Event::ArrowLeft);
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    InputFixture::type(*comp, "XY");
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));

    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("aXYb"));
    // 粘贴后光标位于粘贴内容末尾 (即 'b' 之前), 后续输入接在粘贴内容之后
    comp->OnEvent(ftxui::Event::Character('Z'));
    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("aXYZb"));
}

void test_paste_trailing_newline_not_sent() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 粘贴以换行结尾: 该换行属于粘贴内容, 不应触发发送
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    InputFixture::type(*comp, "hello");
    comp->OnEvent(ftxui::Event::Return);
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));

    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("hello\n"));
    XX_TEST_EXPECT_FALSE(f.sent);
}

void test_paste_empty_content() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 空粘贴 (仅标记): 不崩溃, 内容为空
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));

    XX_TEST_EXPECT_TRUE(comp->inputText().empty());
    XX_TEST_EXPECT_FALSE(f.sent);
}

void test_real_enter_sends() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 正常输入 + 回车: 发送并清空输入框
    InputFixture::type(*comp, "hello");
    comp->OnEvent(ftxui::Event::Return);

    XX_TEST_EXPECT_TRUE(f.sent);
    XX_TEST_EXPECT_EQ(f.sentText, std::string("hello"));
    XX_TEST_EXPECT_TRUE(comp->inputText().empty());
}

void test_send_rejected_retains_input() {
    InputFixture           f;
    InputComponent::Config cfg;
    // 模拟 agent-io 未初始化就绪, 拒绝发送
    cfg.onSend = [](std::string) -> bool {
        return false;
    };
    auto comp = std::make_shared<InputComponent>(f.ctx, std::move(cfg));

    InputFixture::type(*comp, "pending message");
    comp->OnEvent(ftxui::Event::Return);

    // 未就绪拒绝发送时, 输入框内容保留不被清空
    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("pending message"));
}

void test_enter_after_paste_sends_all() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 粘贴多行后回车: 发送完整多行内容 (去除首尾换行)
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    InputFixture::type(*comp, "line1");
    comp->OnEvent(ftxui::Event::Return); // 粘贴换行
    InputFixture::type(*comp, "line2");
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));
    comp->OnEvent(ftxui::Event::Return); // 真实回车

    XX_TEST_EXPECT_TRUE(f.sent);
    XX_TEST_EXPECT_EQ(f.sentText, std::string("line1\nline2"));
}

void test_alt_enter_newline_cursor_at_end() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // "abc" + Alt+Enter + "def": 换行后光标应位于换行之后 (新行开头),
    // 后续输入接在换行之后 → "abc\ndef"
    InputFixture::type(*comp, "abc");
    comp->OnEvent(ftxui::Event::Special("\x1B\n")); // Alt+Enter
    InputFixture::type(*comp, "def");

    // 旧实现 (inputText_ += '\n' 不更新光标) 会得到 "abcdef\n"
    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("abc\ndef"));
}

void test_alt_enter_newline_mid_text() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // "abc", 光标移到 'c' 前, Alt+Enter → "ab\nc", 光标在换行后
    InputFixture::type(*comp, "abc");
    comp->OnEvent(ftxui::Event::ArrowLeft);         // 光标在 'c' 前
    comp->OnEvent(ftxui::Event::Special("\x1B\r")); // Alt+Enter (部分终端发送 \r)
    comp->OnEvent(ftxui::Event::Character('X'));

    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("ab\nXc"));
}

void test_paste_timeout_recovery() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 粘贴开始后结束标记丢失: 超过超时 (2s) 后自动退出粘贴模式,
    // 后续输入不再被吞入缓冲区
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    InputFixture::type(*comp, "ab");

    std::this_thread::sleep_for(std::chrono::milliseconds(2100));

    comp->OnEvent(ftxui::Event::Character('x'));
    // 未完成的粘贴缓冲区被丢弃, 仅 'x' 被正常插入
    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("x"));
}

void test_clear_resets_paste_state() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 粘贴进行中调用 clear(): 重置粘贴状态, 后续输入正常插入
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    comp->OnEvent(ftxui::Event::Character('a'));
    comp->clear();
    comp->OnEvent(ftxui::Event::Character('b'));

    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("b"));
}

void test_tui_state_message_queue_sync() {
    TUISharedState sharedState;

    // 模拟收到 WireMessageQueueUpdate 更新 pendingInputs
    agentxx::agent::WireMessageQueueUpdate update;
    update.sessionId = "test-session";
    agentxx::agent::MessageQueueItem it1;
    it1.id          = "q-1";
    it1.text        = "msg 1";
    it1.model       = "m1";
    it1.createdAtMs = 1000;

    agentxx::agent::MessageQueueItem it2;
    it2.id          = "q-2";
    it2.text        = "msg 2";
    it2.model       = "";
    it2.createdAtMs = 2000;

    update.items.push_back(it1);
    update.items.push_back(it2);

    sharedState.mutate([&](TUIRenderState& st) {
        st.pendingInputs.clear();
        for (const auto& item : update.items) {
            TUIPendingInput pi;
            pi.id          = item.id;
            pi.text        = item.text;
            pi.model       = item.model;
            pi.createdAtMs = item.createdAtMs;
            st.pendingInputs.push_back(std::move(pi));
        }
    });

    auto snap = sharedState.readSnapshot();
    XX_TEST_EXPECT_EQ(snap->pendingInputs.size(), size_t{2});
    if (snap->pendingInputs.size() == 2) {
        XX_TEST_EXPECT_EQ(snap->pendingInputs[0].id, std::string("q-1"));
        XX_TEST_EXPECT_EQ(snap->pendingInputs[0].text, std::string("msg 1"));
        XX_TEST_EXPECT_EQ(snap->pendingInputs[1].id, std::string("q-2"));
        XX_TEST_EXPECT_EQ(snap->pendingInputs[1].text, std::string("msg 2"));
    }
}

// ---------------------------------------------------------------------------
// 流式指示 spinner: 动画帧经组件树转发 OnAnimation 推进
// (回归: SpinnerComponent 若未 Add() 入组件树, 收不到动画回调, 恒显示首帧 ⠋)
// ---------------------------------------------------------------------------
void test_spinner_frame_advances_via_tree() {
    InputFixture           f;
    bool                   streaming = false;
    InputComponent::Config cfg;
    cfg.onSend = [](std::string) -> bool {
        return true;
    };
    cfg.isStreaming = [&streaming] {
        return streaming;
    };
    auto comp = std::make_shared<InputComponent>(f.ctx, std::move(cfg));

    // 渲染到字符串便于检查指示器字符 (braille 字符在输入栏中唯一)
    auto renderToString = [](InputComponent& c) {
        ftxui::Screen screen(80, 5);
        ftxui::Render(screen, c.OnRender());
        return screen.ToString();
    };

    // 非流式: 静态 ">" 提示符, 无 braille 动画字符
    const std::string idle = renderToString(*comp);
    XX_TEST_EXPECT_TRUE(idle.find(">") != std::string::npos);
    XX_TEST_EXPECT_TRUE(idle.find("⠋") == std::string::npos);

    // 流式: 首帧渲染为第 0 帧 ⠋
    streaming = true;
    XX_TEST_EXPECT_TRUE(renderToString(*comp).find("⠋") != std::string::npos);

    // 经组件树分发动画步进 (根组件 OnAnimation 默认逐级转发给已注册子组件):
    // 单次 90ms > 帧间隔 80ms → 推进到第 1 帧 ⠙
    ftxui::animation::Params params(std::chrono::milliseconds(90));
    comp->OnAnimation(params);
    XX_TEST_EXPECT_TRUE(renderToString(*comp).find("⠙") != std::string::npos);

    // 多次小步长累计跨帧: 再累计 10×20ms=200ms ≥ 2×80ms → 第 3 帧 ⠸
    for (int i = 0; i < 10; ++i) {
        ftxui::animation::Params small(std::chrono::milliseconds(20));
        comp->OnAnimation(small);
    }
    XX_TEST_EXPECT_TRUE(renderToString(*comp).find("⠸") != std::string::npos);

    // 停止流式: 指示器回到静态 ">"
    streaming = false;
    XX_TEST_EXPECT_TRUE(renderToString(*comp).find(">") != std::string::npos);
}

TestResult testTuiInput() {
    g_tui_input_passed = 0;
    g_tui_input_failed = 0;

    test_multiline_paste_inserted_not_sent();
    test_paste_crlf_dedup();
    test_paste_preserves_tab();
    test_paste_inserts_at_cursor();
    test_paste_trailing_newline_not_sent();
    test_paste_empty_content();
    test_real_enter_sends();
    test_send_rejected_retains_input();
    test_enter_after_paste_sends_all();
    test_alt_enter_newline_cursor_at_end();
    test_alt_enter_newline_mid_text();
    test_paste_timeout_recovery();
    test_clear_resets_paste_state();
    test_spinner_frame_advances_via_tree();
    test_tui_state_message_queue_sync();

    return TestResult{g_tui_input_passed, g_tui_input_failed};
}

} // namespace test
} // namespace agentxx
