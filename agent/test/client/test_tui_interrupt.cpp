#include "test_tui_interrupt.h"

#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/util/string_util.h"
#include "asio/io_context.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <memory>
#include <optional>
#include <string>

namespace agentxx {
namespace test {

int g_tui_interrupt_passed = 0;
int g_tui_interrupt_failed = 0;

// ---------------------------------------------------------------------------
// 测试夹具: 消息列表内嵌中断输入 (Role::Interrupt) 的渲染与交互
//
// 注意: 中断输入项的纯 UI 状态 (编辑文本/选中项/校验提示/结果通道) 从消息
// 结构迁出, 由 MessageListComponent::interruptUi_ 状态表维护 (key =
// interruptId+inputIndex), 经 attachInterruptChannel 注入结果通道, 渲染/
// 交互时惰性初始化。测试经 comp->interruptUiState(msgIndex) 读取断言。
// ---------------------------------------------------------------------------

namespace {

struct InterruptFixture {
    // io_context 须先声明 (最后析构): 结果通道 (UI 状态表持有) 在 sharedState
    // 析构时释放, 其析构需访问 io_context 的 channel service
    asio::io_context io;
    TUISharedState   sharedState;
    TUITheme         theme       = TUITheme::darkTheme();
    int              redrawCount = 0;

    TUICtx                                ctx;
    std::shared_ptr<MessageListComponent> comp;

    InterruptFixture() {
        ctx.state      = &sharedState;
        ctx.frameState = sharedState.readSnapshot();
        ctx.postRedraw = [this] {
            ++redrawCount;
        };
        ctx.theme          = &theme;
        ctx.showSystemInfo = nullptr;
        ctx.session        = nullptr;
        ctx.threadId       = "session";
        ctx.remoteUrl      = "";
        comp               = std::make_shared<MessageListComponent>(ctx);
    }

    /// 创建中断结果回传通道
    std::shared_ptr<InterruptResultChannel> makeChannel() {
        return std::make_shared<InterruptResultChannel>(io.get_executor(), 16);
    }

    /// 追加一条中断输入消息, 返回其消息索引
    size_t addInterrupt(
        std::shared_ptr<InterruptResultChannel> ch,
        std::string                             type,
        std::string                             defaultValue = "",
        std::string                             label        = "label",
        std::vector<std::string>                enumValues   = {},
        int64_t                                 interruptId  = 1,
        int                                     inputIndex   = 1,
        int                                     inputTotal   = 1
    ) {
        auto m                     = std::make_shared<TUIMessage>();
        m->role                    = TUIMessage::Role::Interrupt;
        m->interrupt               = TUIMessage::InterruptData{};
        m->interrupt->interruptId  = interruptId;
        m->interrupt->inputLabel   = label;
        m->interrupt->inputType    = type;
        m->interrupt->inputDefault = defaultValue;
        m->interrupt->inputIndex   = inputIndex;
        m->interrupt->inputTotal   = inputTotal;
        m->interrupt->inputEnums   = std::move(enumValues);
        // UI 状态 (编辑文本/选中项) 不存于消息: 由组件状态表惰性初始化
        sharedState.mutate([&](TUIRenderState& st) {
            st.messages.push_back(std::move(m));
        });
        // 结果通道注入组件状态表 (与 handleInterrupt 经 enqueueUiAction 一致)
        comp->attachInterruptChannel(interruptId, ch);
        return sharedState.readSnapshot()->messages.size() - 1;
    }

    /// 渲染消息列表 (刷新中断控件命中区域) 并取回文本
    std::string render() {
        ctx.frameState = sharedState.readSnapshot();
        auto el        = comp->Render();
        auto screen
            = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(60));
        ftxui::Render(screen, el);
        return screen.ToString();
    }

    /// 在指定消息的控件上模拟鼠标点击 (先渲染刷新命中区域)
    bool click(size_t msgIndex, uint8_t kind, int sub = 0) {
        render();
        for (const auto& h : comp->interruptHitBoxes()) {
            if (h.msgIndex != msgIndex || h.kind != kind || h.sub != sub || !h.box) {
                continue;
            }
            ftxui::Mouse m;
            m.button = ftxui::Mouse::Left;
            m.motion = ftxui::Mouse::Released;
            m.x      = (h.box->x_min + h.box->x_max) / 2;
            m.y      = (h.box->y_min + h.box->y_max) / 2;
            comp->OnEvent(ftxui::Event::Mouse("", m));
            return true;
        }
        return false;
    }

    /// 依次输入字符串的每个字符 (作用于激活的中断消息)
    void type(const std::string& s) {
        for (char c : s) {
            comp->OnEvent(ftxui::Event::Character(c));
        }
    }

    /// 从通道读取一条结果; 返回 false = 无可用消息
    bool recv(
        std::shared_ptr<InterruptResultChannel> ch,
        int&                                    inputIndex,
        std::optional<std::string>&             value
    ) {
        bool got
            = ch->try_receive([&](neograph_asio_error_code ec, int idx, std::optional<std::string> v
                              ) {
                  inputIndex = idx;
                  value      = std::move(v);
              });
        io.run(); // 排空 async_send 投递的完成 handler (数据本身已入队)
        return got;
    }

    /// 读取消息的快照引用 (校验状态用)
    TUIMessage snapshotMsg(size_t mi) {
        auto snap = sharedState.readSnapshot();
        return (mi < snap->messages.size()) ? *snap->messages[mi] : TUIMessage{};
    }
};

} // namespace

// ---------------------------------------------------------------------------
// bool: 是/否 按钮
// ---------------------------------------------------------------------------

void test_bool_render_yes_no() {
    InterruptFixture  f;
    auto              ch   = f.makeChannel();
    auto              mi   = f.addInterrupt(ch, "bool", "true");
    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("是") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("否") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("label") != std::string::npos);
    // 默认值是 "true" → 选中"是"
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).selected, 0);
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitBoolYes));
}

void test_bool_click_yes_confirms_true() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "bool", "true");
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitBoolYes));
    auto msg = f.snapshotMsg(mi);
    XX_TEST_EXPECT_TRUE(msg.interrupt.has_value());
    if (msg.interrupt) {
        XX_TEST_EXPECT_EQ(
            static_cast<int>(msg.interrupt->interruptStatus),
            static_cast<int>(TUIMessage::InterruptStatus::Confirmed)
        );
        XX_TEST_EXPECT_EQ(msg.interrupt->interruptResult, std::string("true"));
    }

    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_EQ(idx, 1);
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("true"));
}

void test_bool_click_no_confirms_false() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "bool", "true");
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitBoolNo));
    auto msg = f.snapshotMsg(mi);
    XX_TEST_EXPECT_TRUE(msg.interrupt.has_value());
    if (msg.interrupt) {
        XX_TEST_EXPECT_EQ(
            static_cast<int>(msg.interrupt->interruptStatus),
            static_cast<int>(TUIMessage::InterruptStatus::Confirmed)
        );
        XX_TEST_EXPECT_EQ(msg.interrupt->interruptResult, std::string("false"));
    }

    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_EQ(idx, 1);
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("false"));
}

void test_bool_confirm_rendered() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "bool", "true");
    f.click(mi, MessageListComponent::kHitBoolYes);
    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("已确认") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("true") != std::string::npos);
}

void test_bool_default_no_selected() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "bool", "no");
    // 默认 "no" → 选中"否", 点击确认按钮 → "false"
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitConfirm));
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("false"));
}

// ---------------------------------------------------------------------------
// int: - [输入框] + 步进 1
// ---------------------------------------------------------------------------

void test_int_step_plus_minus() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "int", "5");
    // 点击 + → 6
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitNumPlus));
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).editText, std::string("6"));
    // 点击 - ×2 → 4
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitNumMinus));
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitNumMinus));
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).editText, std::string("4"));
    // 键盘确认
    f.comp->OnEvent(ftxui::Event::Return);
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("4"));
}

void test_int_arrow_step_active() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "int", "0");
    // 点击输入框激活, ↑ 步进 +1 → 1
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitEdit));
    f.comp->OnEvent(ftxui::Event::ArrowUp);
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).editText, std::string("1"));
    f.comp->OnEvent(ftxui::Event::ArrowDown);
    f.comp->OnEvent(ftxui::Event::ArrowDown);
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).editText, std::string("-1"));
    f.comp->OnEvent(ftxui::Event::Return);
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("-1"));
}

void test_int_manual_edit_replace_default() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "int", "0");
    // 点击输入框激活 (默认 "0"), 首次输入替换默认值
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitEdit));
    f.type("42");
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).editText, std::string("42"));
    f.comp->OnEvent(ftxui::Event::Return);
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("42"));
}

void test_int_invalid_rejected_and_recover() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "int", "0");
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitEdit));
    f.type("abc");
    // Enter 校验失败: 不确认, 显示错误提示
    f.comp->OnEvent(ftxui::Event::Return);
    auto msg = f.snapshotMsg(mi);
    XX_TEST_EXPECT_TRUE(msg.interrupt.has_value());
    if (msg.interrupt) {
        XX_TEST_EXPECT_EQ(
            static_cast<int>(msg.interrupt->interruptStatus),
            static_cast<int>(TUIMessage::InterruptStatus::Waiting)
        );
    }
    XX_TEST_EXPECT_TRUE(f.comp->interruptUiState(mi).tip.find("Invalid") != std::string::npos);
    // ↑ 步进无效 (编辑值非法)
    f.comp->OnEvent(ftxui::Event::ArrowUp);
    f.comp->OnEvent(ftxui::Event::Return);
    msg = f.snapshotMsg(mi);
    XX_TEST_EXPECT_TRUE(msg.interrupt.has_value());
    if (msg.interrupt) {
        XX_TEST_EXPECT_EQ(
            static_cast<int>(msg.interrupt->interruptStatus),
            static_cast<int>(TUIMessage::InterruptStatus::Waiting)
        );
    }
    // 清空后输入合法值: 确认成功
    for (int i = 0; i < 3; ++i) {
        f.comp->OnEvent(ftxui::Event::Backspace);
    }
    f.type("7");
    f.comp->OnEvent(ftxui::Event::Return);
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("7"));
}

// ---------------------------------------------------------------------------
// double: - [输入框] + 步进 1.0
// ---------------------------------------------------------------------------

void test_double_step_keeps_fraction() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "double", "2.5");
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitNumPlus));
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).editText, std::string("3.5"));
    f.comp->OnEvent(ftxui::Event::Return);
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("3.5"));
}

void test_double_step_integer_style() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "double", "");
    // 默认 0.0, ↑↑ → 2.0 (整数按 "1.0" 风格显示)
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitEdit));
    f.comp->OnEvent(ftxui::Event::ArrowUp);
    f.comp->OnEvent(ftxui::Event::ArrowUp);
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).editText, std::string("2.0"));
    f.comp->OnEvent(ftxui::Event::Return);
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("2.0"));
}

void test_double_invalid_rejected() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "double", "");
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitEdit));
    f.type("abc");
    f.comp->OnEvent(ftxui::Event::Return);
    auto msg = f.snapshotMsg(mi);
    XX_TEST_EXPECT_TRUE(msg.interrupt.has_value());
    if (msg.interrupt) {
        XX_TEST_EXPECT_EQ(
            static_cast<int>(msg.interrupt->interruptStatus),
            static_cast<int>(TUIMessage::InterruptStatus::Waiting)
        );
    }
    XX_TEST_EXPECT_TRUE(f.comp->interruptUiState(mi).tip.find("Invalid") != std::string::npos);
}

// ---------------------------------------------------------------------------
// enum: 竖直选择列表
// ---------------------------------------------------------------------------

void test_enum_render_list() {
    InterruptFixture  f;
    auto              ch   = f.makeChannel();
    auto              mi   = f.addInterrupt(ch, "enum", "", "mode", {"alpha", "beta", "gamma"});
    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("alpha") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("beta") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("gamma") != std::string::npos);
}

void test_enum_default_selected_and_move() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "enum", "b", "mode", {"a", "b", "c"});
    // 默认选中 b → 点击确认按钮
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitConfirm));
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("b"));

    // 点击第 3 项 (c) → 选中, Enter 确认 (不同中断请求 → 不同 interruptId)
    auto ch2 = f.makeChannel();
    auto mi2 = f.addInterrupt(ch2, "enum", "", "mode", {"a", "b", "c"}, {}, 2);
    XX_TEST_EXPECT_TRUE(f.click(mi2, MessageListComponent::kHitEnumItem, 2));
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi2).selected, 2);
    f.comp->OnEvent(ftxui::Event::Return);
    XX_TEST_EXPECT_TRUE(f.recv(ch2, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("c"));
}

// ---------------------------------------------------------------------------
// string: 文本输入框
// ---------------------------------------------------------------------------

void test_string_replace_default_and_confirm() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "string", "hi");
    // 点击输入框激活 (默认 "hi"), 首次输入替换默认值
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitEdit));
    f.type("hello");
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).editText, std::string("hello"));
    f.comp->OnEvent(ftxui::Event::Return);
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("hello"));
}

void test_string_enter_uses_default() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "string", "hi");
    // 激活后不编辑直接 Enter: 确认默认值
    XX_TEST_EXPECT_TRUE(f.click(mi, MessageListComponent::kHitEdit));
    f.comp->OnEvent(ftxui::Event::Return);
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("hi"));
}

// ---------------------------------------------------------------------------
// 取消 / 过期
// ---------------------------------------------------------------------------

void test_cancel_marks_all_and_notifies() {
    InterruptFixture f;
    auto             ch  = f.makeChannel();
    auto             mi1 = f.addInterrupt(ch, "bool", "true", "a", {}, 7, 1, 2);
    auto             mi2 = f.addInterrupt(ch, "bool", "false", "b", {}, 7, 2, 2);
    // 取消任意一条 → 同请求所有未操作消息 Cancelled, 通道收到整体取消
    XX_TEST_EXPECT_TRUE(f.click(mi1, MessageListComponent::kHitCancel));
    auto m1 = f.snapshotMsg(mi1);
    auto m2 = f.snapshotMsg(mi2);
    XX_TEST_EXPECT_TRUE(m1.interrupt.has_value() && m2.interrupt.has_value());
    if (m1.interrupt) {
        XX_TEST_EXPECT_EQ(
            static_cast<int>(m1.interrupt->interruptStatus),
            static_cast<int>(TUIMessage::InterruptStatus::Cancelled)
        );
    }
    if (m2.interrupt) {
        XX_TEST_EXPECT_EQ(
            static_cast<int>(m2.interrupt->interruptStatus),
            static_cast<int>(TUIMessage::InterruptStatus::Cancelled)
        );
    }
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_EQ(idx, -1);
    XX_TEST_EXPECT_FALSE(val.has_value());
}

void test_cancel_rendered() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "bool", "true");
    f.click(mi, MessageListComponent::kHitCancel);
    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("已取消") != std::string::npos);
}

void test_expired_rendered() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "bool", "true");
    // 模拟 server 过期通知: 消息标记为 Expired
    f.sharedState.mutate([&](TUIRenderState& st) {
        if (mi < st.messages.size() && st.messages[mi]->interrupt) {
            st.messages[mi]->interrupt->interruptStatus = TUIMessage::InterruptStatus::Expired;
        }
    });
    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("已过期") != std::string::npos);
    // 过期后控件不再可交互: 点击无效 (无命中区域)
    XX_TEST_EXPECT_FALSE(f.click(mi, MessageListComponent::kHitBoolYes));
    auto msg = f.snapshotMsg(mi);
    XX_TEST_EXPECT_TRUE(msg.interrupt.has_value());
    if (msg.interrupt) {
        XX_TEST_EXPECT_EQ(
            static_cast<int>(msg.interrupt->interruptStatus),
            static_cast<int>(TUIMessage::InterruptStatus::Expired)
        );
    }
}

TestResult testTuiInterrupt() {
    g_tui_interrupt_passed = 0;
    g_tui_interrupt_failed = 0;

    // bool
    test_bool_render_yes_no();
    test_bool_click_yes_confirms_true();
    test_bool_click_no_confirms_false();
    test_bool_confirm_rendered();
    test_bool_default_no_selected();
    // int
    test_int_step_plus_minus();
    test_int_arrow_step_active();
    test_int_manual_edit_replace_default();
    test_int_invalid_rejected_and_recover();
    // double
    test_double_step_keeps_fraction();
    test_double_step_integer_style();
    test_double_invalid_rejected();
    // enum
    test_enum_render_list();
    test_enum_default_selected_and_move();
    // string
    test_string_replace_default_and_confirm();
    test_string_enter_uses_default();
    // 取消 / 过期
    test_cancel_marks_all_and_notifies();
    test_cancel_rendered();
    test_expired_rendered();

    return TestResult{g_tui_interrupt_passed, g_tui_interrupt_failed};
}

} // namespace test
} // namespace agentxx
