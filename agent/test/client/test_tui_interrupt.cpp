#include "test_tui_interrupt.h"

#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/interrupt_view.h"
#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/middlewares/interrupt_ui.h"
#include "agentxx/util/string_util.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "fmt/format.h"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <memory>
#include <optional>
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_interrupt_passed = 0;
int g_tui_interrupt_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_interrupt_passed
#define XX_TEST_FAILED g_tui_interrupt_failed

namespace agentxx {
namespace test {

// ---------------------------------------------------------------------------
// 测试夹具: 消息列表内嵌中断输入 (Role::Interrupt) 的通用渲染与交互
//
// 中断形态完全由消息携带的 UI 描述 (`InterruptData::ui`, 见
// agentxx/middlewares/interrupt_ui.h) 决定: 本文件按描述数据构造消息, 断言
// 通用渲染/交互/结果组装 (客户端不含任何 permission 特化分支)。描述**必填**
// (服务端 InterruptHandleArg::toJson 恒下发; 通用默认描述 = 进度头行 + 描述
// + 按消息类型的输入控件 + 确认取消行), 缺失按契约违规输出诊断行。
//
// 命中区域: (msgIndex, 描述项 id, 子序号); 描述项 id 约定:
// - 输入项: "value" (值按钮下标 = sub / 枚举项下标 = sub /
//   数值控件 sub: 0=减, 1=加, 2=输入框; 文本输入框 sub = 0)
// - 勾选项: 描述中声明的 id (如 "remember")
// - 确认/取消行: "submit" (0=确认, 1=取消)
// ---------------------------------------------------------------------------

namespace {

struct InterruptFixture {
    // io_context 须先声明 (最后析构): 结果通道 (中断视图持有) 在 sharedState
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
        ctx.theme     = &theme;
        ctx.sessionId = "session";
        ctx.remoteUrl = "";
        comp          = std::make_shared<MessageListComponent>(ctx);
    }

    /// 创建中断结果回传通道
    std::shared_ptr<InterruptResultChannel> makeChannel() {
        return std::make_shared<InterruptResultChannel>(io.get_executor(), 16);
    }

    /// 追加一条中断输入消息, 返回其消息索引
    /// - ui: 中断 UI 描述 (缺省 = 服务端通用默认描述 InterruptUi::defaultUi(),
    ///   与 InterruptHandleArg::toJson 在生产者未声明时的下发内容一致;
    ///   客户端不再有"无描述"的渲染回退 —— 见 test_missing_descriptor_diagnostic)
    size_t addInterrupt(
        std::shared_ptr<InterruptResultChannel> ch,
        std::string                             type,
        std::string                             defaultValue = "",
        std::string                             label        = "label",
        std::vector<std::string>                enumValues   = {},
        int64_t                                 interruptId  = 1,
        int                                     inputIndex   = 1,
        int                                     inputTotal   = 1,
        agentxx::util::Json                     ui           = agentxx::util::Json{},
        std::string                             depict       = ""
    ) {
        auto m                     = std::make_shared<TUIMessage>();
        m->role                    = TUIMessage::Role::Interrupt;
        m->interrupt               = TUIMessage::InterruptData{};
        m->interrupt->interruptId  = interruptId;
        m->interrupt->inputLabel   = label;
        m->interrupt->inputDepict  = std::move(depict);
        m->interrupt->inputType    = type;
        m->interrupt->inputDefault = defaultValue;
        m->interrupt->inputIndex   = inputIndex;
        m->interrupt->inputTotal   = inputTotal;
        m->interrupt->inputEnums   = std::move(enumValues);
        // 描述必填: 未显式给出时用通用默认描述 (服务端 toJson 的兜底行为)
        m->interrupt->ui = ui.is_object() && !middleware::InterruptUi::fromJson(ui).empty()
                               ? std::move(ui)
                               : middleware::InterruptUi::defaultUi().toJson();
        // 表单状态 (编辑文本/选中项/勾选项) 不存于消息: 由中断视图惰性初始化
        sharedState.mutate([&](TUIRenderState& st) {
            st.messages.push_back(std::move(m));
        });
        // 结果通道注入 (与 handleInterrupt 经 enqueueUiAction 一致)
        comp->attachInterruptChannel(interruptId, ch);
        return sharedState.readSnapshot()->messages.size() - 1;
    }

    /// 渲染消息列表 (刷新中断控件命中区域) 并取回文本
    std::string render(int width = 120, int height = 60) {
        ctx.frameState = sharedState.readSnapshot();
        auto el        = comp->Render();
        auto screen
            = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
        ftxui::Render(screen, el);
        return screen.ToString();
    }

    /// 渲染并统计内容占据的行数 (首/末非空行之间的跨度, 含内部空行):
    /// 用于校验"估算 == 实测" (消息块 = 内容行 + 尾部空行)
    size_t renderedRows(int width = 120, int height = 60) {
        ctx.frameState = sharedState.readSnapshot();
        auto el        = comp->Render();
        auto screen
            = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
        ftxui::Render(screen, el);
        int first = -1;
        int last  = -1;
        for (int y = 0; y < height; ++y) {
            std::string line;
            for (int x = 0; x < width; ++x) {
                line += screen.PixelAt(x, y).character;
            }
            if (line.find_first_not_of(' ') != std::string::npos) {
                if (first < 0) {
                    first = y;
                }
                last = y;
            }
        }
        return (first < 0) ? 0 : static_cast<size_t>(last - first + 1);
    }

    /// 在指定消息的控件上模拟鼠标点击 (先渲染刷新命中区域)
    bool click(size_t msgIndex, std::string_view itemId, int sub = 0) {
        render();
        for (const auto& h : comp->interruptHitBoxes()) {
            if (h.msgIndex != msgIndex || h.itemId != itemId || h.sub != sub || !h.box) {
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
    /// options 为可选输出: 勾选项结果 (如权限询问的 "remember"; nullptr 不读取)
    bool recv(
        std::shared_ptr<InterruptResultChannel> ch,
        int&                                    inputIndex,
        std::optional<std::string>&             value,
        agentxx::util::Json*                    options = nullptr
    ) {
        bool got = ch->try_receive(
            [&](neograph_asio_error_code,
                int                        idx,
                std::optional<std::string> v,
                agentxx::util::Json        opts) {
                inputIndex = idx;
                value      = std::move(v);
                if (options) {
                    *options = std::move(opts);
                }
            }
        );
        io.run(); // 排空 async_send 投递的完成 handler (数据本身已入队)
        return got;
    }

    /// 读取消息的快照副本 (校验状态用)
    TUIMessage snapshotMsg(size_t mi) {
        auto snap = sharedState.readSnapshot();
        return (mi < snap->messages.size()) ? *snap->messages[mi] : TUIMessage{};
    }

    /// 权限询问描述 (agent 侧构造; 客户端仅按数据渲染)
    static agentxx::util::Json permissionUi() {
        return agentxx::middleware::InterruptUi::permissionUi(
                   "read_file",
                   "filesystem_read",
                   "/workspace/data/x.txt"
        )
            .toJson();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 通用默认描述 (InterruptUi::defaultUi): 进度头行 + 描述 + 类型控件 + 确认行
// ---------------------------------------------------------------------------

void test_default_descriptor_bool_render() {
    InterruptFixture  f;
    auto              ch = f.makeChannel();
    auto              mi = f.addInterrupt(
        ch,
        "bool",
        "true",
        "read_file",
        {},
        1,
        1,
        1,
        {},
        "/tmp/a.txt"
    );
    const std::string text = f.render();
    // 进度头行 (i18n) + 输入项标签 + 描述文本
    XX_TEST_EXPECT_TRUE(text.find("! [中断] 输入 1/1: ") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("read_file") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("/tmp/a.txt") != std::string::npos);
    // 内置的是/否值按钮 + 确认/取消行
    XX_TEST_EXPECT_TRUE(text.find("是") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("否") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("确认") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("✕") != std::string::npos);
    // 默认值 "true" → 选中"是"
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).selected, 0);
}

void test_default_descriptor_value_buttons_confirm() {
    InterruptFixture f;
    auto             ch  = f.makeChannel();
    auto             mi1 = f.addInterrupt(ch, "bool", "true");
    XX_TEST_EXPECT_TRUE(f.click(mi1, "value", 0)); // 是 → 立即确认
    auto msg1 = f.snapshotMsg(mi1);
    XX_TEST_EXPECT_TRUE(msg1.interrupt.has_value());
    if (msg1.interrupt) {
        XX_TEST_EXPECT_EQ(
            static_cast<int>(msg1.interrupt->interruptStatus),
            static_cast<int>(TUIMessage::InterruptStatus::Confirmed)
        );
        XX_TEST_EXPECT_EQ(msg1.interrupt->interruptResult, std::string("true"));
    }
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_EQ(idx, 1);
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("true"));

    // 否按钮 → 立即确认 false
    auto mi2 = f.addInterrupt(ch, "bool", "true", "label", {}, 2);
    XX_TEST_EXPECT_TRUE(f.click(mi2, "value", 1));
    auto msg2 = f.snapshotMsg(mi2);
    XX_TEST_EXPECT_TRUE(msg2.interrupt.has_value());
    if (msg2.interrupt) {
        XX_TEST_EXPECT_EQ(msg2.interrupt->interruptResult, std::string("false"));
    }
}

void test_default_descriptor_bool_default_no_selected() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "bool", "no");
    // 默认 "no" → 选中"否"; 点击确认行 → "false"
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).selected, 1);
    XX_TEST_EXPECT_TRUE(f.click(mi, "submit", 0));
    int                        idx = 0;
    std::optional<std::string> val;
    agentxx::util::Json        options;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val, &options));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("false"));
    // 缺省模板未声明勾选项: 结果选项为空对象 (client 侧回传形态)
    XX_TEST_EXPECT_TRUE(options.is_object());
    XX_TEST_EXPECT_TRUE(options.empty());
}

void test_confirm_renders_status_line() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "bool", "true");
    f.click(mi, "value", 0);
    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("已确认") != std::string::npos);
    // 确认后控件不再可交互
    XX_TEST_EXPECT_FALSE(f.click(mi, "value", 1));
}

void test_escape_blurs_active_message() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "string", "hi");
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 0));
    XX_TEST_EXPECT_EQ(f.comp->activeInterruptMsg(), mi);
    f.comp->OnEvent(ftxui::Event::Escape);
    XX_TEST_EXPECT_EQ(f.comp->activeInterruptMsg(), static_cast<size_t>(-1));
    // 失焦后输入不再作用于该消息
    f.type("x");
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).editText, std::string("hi"));
}

// ---------------------------------------------------------------------------
// int / double: 数值控件 (- [输入框] +) + 确认行
// ---------------------------------------------------------------------------

void test_int_step_plus_minus() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "int", "5");
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 1)); // +
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).editText, std::string("6"));
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 0)); // -
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 0)); // -
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).editText, std::string("4"));
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
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 2)); // 输入框: 激活
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
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 2));
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
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 2));
    f.type("abc");
    // 确认校验失败: 不确认, 显示错误提示
    f.comp->OnEvent(ftxui::Event::Return);
    auto msg = f.snapshotMsg(mi);
    XX_TEST_EXPECT_TRUE(msg.interrupt.has_value());
    if (msg.interrupt) {
        XX_TEST_EXPECT_EQ(
            static_cast<int>(msg.interrupt->interruptStatus),
            static_cast<int>(TUIMessage::InterruptStatus::Waiting)
        );
    }
    XX_TEST_EXPECT_TRUE(f.comp->interruptUiState(mi).tip.find("无效") != std::string::npos);
    // 提示行参与渲染
    XX_TEST_EXPECT_TRUE(f.render().find("无效") != std::string::npos);
    // 清空后输入合法值: 确认成功
    for (int i = 0; i < 3; ++i) {
        f.comp->OnEvent(ftxui::Event::Backspace);
    }
    f.type("7");
    XX_TEST_EXPECT_TRUE(f.comp->interruptUiState(mi).tip.empty());
    f.comp->OnEvent(ftxui::Event::Return);
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("7"));
}

void test_double_step_keeps_fraction() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "double", "2.5");
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 1));
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
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 2));
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
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 2));
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
    XX_TEST_EXPECT_TRUE(f.comp->interruptUiState(mi).tip.find("无效") != std::string::npos);
}

// ---------------------------------------------------------------------------
// enum: 竖直选择列表 (全部渲染, 不截断)
// ---------------------------------------------------------------------------

void test_enum_render_list_and_select() {
    InterruptFixture  f;
    auto              ch   = f.makeChannel();
    auto              mi   = f.addInterrupt(ch, "enum", "b", "mode", {"a", "b", "c"});
    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("a") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("c") != std::string::npos);
    // 默认选中 b → 确认行确认
    XX_TEST_EXPECT_TRUE(f.click(mi, "submit", 0));
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("b"));

    // 点击第 3 项 (c) → 选中, Enter 确认
    auto ch2 = f.makeChannel();
    auto mi2 = f.addInterrupt(ch2, "enum", "", "mode", {"a", "b", "c"}, {}, 2);
    XX_TEST_EXPECT_TRUE(f.click(mi2, "value", 2));
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi2).selected, 2);
    f.comp->OnEvent(ftxui::Event::Return);
    XX_TEST_EXPECT_TRUE(f.recv(ch2, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("c"));
}

void test_enum_many_items_all_rendered() {
    InterruptFixture         f;
    auto                     ch = f.makeChannel();
    std::vector<std::string> items;
    for (int i = 0; i < 8; ++i) {
        items.push_back(fmt::format("opt{}", i));
    }
    f.addInterrupt(ch, "enum", "", "mode", items);
    const std::string text = f.render();
    for (const auto& it : items) {
        XX_TEST_EXPECT_TRUE(text.find(it) != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// string: 文本输入框
// ---------------------------------------------------------------------------

void test_string_replace_default_and_confirm() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "string", "hi");
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 0));
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
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 0));
    f.comp->OnEvent(ftxui::Event::Return);
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("hi"));
}

// ---------------------------------------------------------------------------
// 权限询问卡片: 由描述数据驱动 (客户端无 permission 特化分支)
// ---------------------------------------------------------------------------

void test_permission_card_layout_from_descriptor() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    f.addInterrupt(
        ch,
        "bool",
        "no",
        "read_file filesystem_read",
        {},
        1,
        1,
        1,
        InterruptFixture::permissionUi(),
        "/workspace/data/x.txt"
    );
    const std::string text = f.render();
    // 头行分段: 权限标记 + 工具名 + 权限分类
    XX_TEST_EXPECT_TRUE(text.find("! [Permission]") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("read_file") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("filesystem_read") != std::string::npos);
    // 描述 (目标路径) + 设置项 + 一键按钮; 不展示通用进度头与确认行
    XX_TEST_EXPECT_TRUE(text.find("/workspace/data/x.txt") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("记住此选择") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("[   ]") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("允许") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("拒绝") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("! [中断] 输入") == std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("确认") == std::string::npos);
}

void test_permission_allow_with_remember_returns_options() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(
        ch,
        "bool",
        "no",
        "edit_file filesystem_write",
        {},
        1,
        1,
        1,
        InterruptFixture::permissionUi(),
        "/workspace/data/y.txt"
    );
    // 勾选 "记住此选择" → 指示器更新
    XX_TEST_EXPECT_FALSE(f.comp->interruptUiState(mi).toggles["remember"]);
    XX_TEST_EXPECT_TRUE(f.click(mi, "remember", 0));
    XX_TEST_EXPECT_TRUE(f.comp->interruptUiState(mi).toggles["remember"]);
    XX_TEST_EXPECT_TRUE(f.render().find("[ ✓ ]") != std::string::npos);
    // 点击 "允许" (值按钮下标 0) → 立即确认
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 0));
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
    agentxx::util::Json        options;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val, &options));
    XX_TEST_EXPECT_EQ(idx, 1);
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("true"));
    XX_TEST_EXPECT_TRUE(options.is_object());
    XX_TEST_EXPECT_TRUE(options.value("remember", false));
}

void test_permission_deny_without_remember_has_empty_options() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(
        ch,
        "bool",
        "no",
        "read_file filesystem_read",
        {},
        1,
        1,
        1,
        InterruptFixture::permissionUi(),
        "/workspace/data/z.txt"
    );
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 1)); // 拒绝
    int                        idx = 0;
    std::optional<std::string> val;
    agentxx::util::Json        options;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val, &options));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("false"));
    XX_TEST_EXPECT_TRUE(options.is_object());
    // 描述声明了 options=remember: 未勾选时回传 false (服务端据此不注册规则)
    XX_TEST_EXPECT_FALSE(options.value("remember", true));
}

void test_permission_wrapped_long_path() {
    InterruptFixture  f;
    auto              ch = f.makeChannel();
    const std::string longPath
        = "/home/coolight/program/agentxx/agent/lib/include/agentxx/agent/conversation_types.h";
    f.addInterrupt(
        ch,
        "bool",
        "no",
        "read_file filesystem_read",
        {},
        1,
        1,
        1,
        agentxx::middleware::InterruptUi::permissionUi("read_file", "filesystem_read", longPath)
            .toJson(),
        longPath
    );
    // 窄宽度下: 描述行按宽度硬折行, 首尾均可渲染 (不被压为 0 宽消失)
    const std::string text = f.render(40, 30);
    XX_TEST_EXPECT_TRUE(text.find("/home/coolight") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("conversation_types.h") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 描述扩展性: 自定义项 (text/gap/separator/未知项) 与模板语义
// ---------------------------------------------------------------------------

void test_custom_descriptor_renders_items() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    agentxx::middleware::InterruptUi ui;
    ui.header.progress = false;
    ui.header.label    = true;
    agentxx::middleware::InterruptUiSegment badge;
    badge.text  = "! [Custom] ";
    badge.color = "error";
    badge.bold  = true;
    ui.header.segments.push_back(badge);

    agentxx::middleware::InterruptUiItem title;
    title.kind = "text";
    title.text = "Custom question";
    ui.items.push_back(title);

    agentxx::middleware::InterruptUiItem gap;
    gap.kind = "gap";
    ui.items.push_back(gap);

    agentxx::middleware::InterruptUiItem sep;
    sep.kind = "separator";
    ui.items.push_back(sep);

    agentxx::middleware::InterruptUiItem unknown;
    unknown.kind = "future_kind"; // 未知项: 忽略 (向前兼容)
    unknown.text = "SHOULD_NOT_RENDER";
    ui.items.push_back(unknown);

    agentxx::middleware::InterruptUiItem input;
    input.kind = "input";
    input.id   = "value";
    ui.items.push_back(input);

    agentxx::middleware::InterruptUiItem submit;
    submit.kind = "submit";
    ui.items.push_back(submit);

    auto mi       = f.addInterrupt(ch, "string", "typed", "label", {}, 1, 1, 1, ui.toJson());
    auto rendered = f.render();
    XX_TEST_EXPECT_TRUE(rendered.find("! [Custom] ") != std::string::npos);
    XX_TEST_EXPECT_TRUE(rendered.find("Custom question") != std::string::npos);
    XX_TEST_EXPECT_TRUE(rendered.find("SHOULD_NOT_RENDER") == std::string::npos);
    // 输入框默认值来自消息字段 (描述未声明 defaultValue)
    XX_TEST_EXPECT_TRUE(rendered.find("typed") != std::string::npos);
    // 提交行可用
    XX_TEST_EXPECT_TRUE(f.click(mi, "submit", 0));
    int                        idx = 0;
    std::optional<std::string> val;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("typed"));
}

void test_custom_value_buttons_and_toggle_options() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    agentxx::middleware::InterruptUi ui;
    ui.header.label = false;
    agentxx::middleware::InterruptUiItem text;
    text.kind = "text";
    text.text = "choose one";
    ui.items.push_back(text);

    agentxx::middleware::InterruptUiItem toggle;
    toggle.kind          = "toggle";
    toggle.id            = "notify";
    toggle.text          = "Notify me";
    toggle.defaultToggle = true; // 默认勾选
    ui.items.push_back(toggle);

    agentxx::middleware::InterruptUiItem input;
    input.kind = "input";
    input.id   = "value";
    input.view = "buttons";
    agentxx::middleware::InterruptUiButton yes;
    yes.value = "yes-value";
    yes.label = "Proceed";
    agentxx::middleware::InterruptUiButton no;
    no.value = "no-value";
    no.label = "Abort";
    input.buttons.push_back(yes);
    input.buttons.push_back(no);
    ui.items.push_back(input);
    ui.values  = {"value"};
    ui.options = {"notify"};

    auto mi       = f.addInterrupt(ch, "bool", "no", "label", {}, 1, 1, 1, ui.toJson());
    auto rendered = f.render();
    XX_TEST_EXPECT_TRUE(rendered.find("Proceed") != std::string::npos);
    XX_TEST_EXPECT_TRUE(rendered.find("Notify me") != std::string::npos);
    XX_TEST_EXPECT_TRUE(rendered.find("[ ✓ ]") != std::string::npos); // 默认勾选
    XX_TEST_EXPECT_TRUE(f.comp->interruptUiState(mi).toggles["notify"]);

    // 不存在的勾选项 id: 不命中
    XX_TEST_EXPECT_FALSE(f.click(mi, "remember", 0));

    // 取消勾选后点击 Abort → 值 = 描述声明的 no-value
    XX_TEST_EXPECT_TRUE(f.click(mi, "notify", 0));
    XX_TEST_EXPECT_FALSE(f.comp->interruptUiState(mi).toggles["notify"]);
    XX_TEST_EXPECT_TRUE(f.click(mi, "value", 1));
    int                        idx = 0;
    std::optional<std::string> val;
    agentxx::util::Json        options;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val, &options));
    XX_TEST_EXPECT_TRUE(val.has_value());
    XX_TEST_EXPECT_EQ(*val, std::string("no-value"));
    XX_TEST_EXPECT_TRUE(options.is_object());
    XX_TEST_EXPECT_FALSE(options.value("notify", true));
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
    XX_TEST_EXPECT_TRUE(f.click(mi1, "submit", 1));
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
    agentxx::util::Json        options;
    XX_TEST_EXPECT_TRUE(f.recv(ch, idx, val, &options));
    XX_TEST_EXPECT_EQ(idx, -1);
    XX_TEST_EXPECT_FALSE(val.has_value());
    XX_TEST_EXPECT_TRUE(options.is_object() && options.empty());
}

void test_cancel_renders_status_line() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "bool", "true");
    f.click(mi, "submit", 1);
    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("已取消") != std::string::npos);
}

void test_expired_renders_and_not_interactive() {
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
    // 过期后控件不再可交互 (无命中区域)
    XX_TEST_EXPECT_FALSE(f.click(mi, "value", 0));
    auto msg = f.snapshotMsg(mi);
    XX_TEST_EXPECT_TRUE(msg.interrupt.has_value());
    if (msg.interrupt) {
        XX_TEST_EXPECT_EQ(
            static_cast<int>(msg.interrupt->interruptStatus),
            static_cast<int>(TUIMessage::InterruptStatus::Expired)
        );
    }
}

void test_release_channel_clears_form_state() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, "string", "hi");
    f.click(mi, "value", 0);
    f.type("changed");
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).editText, std::string("changed"));
    // 中断流程结束: 释放通道与表单状态 (消息进入终态, 不再需要编辑残留);
    // 再次查询会按消息默认值重新初始化 (不含此前的编辑内容)
    f.comp->releaseInterruptChannel(1);
    XX_TEST_EXPECT_EQ(f.comp->interruptUiState(mi).editText, std::string("hi"));
}

// ---------------------------------------------------------------------------
// 高度估算与实测一致 (渲染与估算同源: 同一份描述逐项判定)
// ---------------------------------------------------------------------------

void test_estimate_matches_rendered_rows() {
    // 缺省模板 (bool + 描述文本)
    {
        InterruptFixture f;
        auto             ch = f.makeChannel();
        f.addInterrupt(ch, "bool", "yes", "label", {}, 1, 1, 1, {}, "/tmp/some/path.txt");
        const size_t est = f.comp->interruptEstimate(0, 120);
        const size_t got = f.renderedRows();
        XX_TEST_EXPECT_EQ(got, est);
    }
    // 权限卡片 (分段头 + 折行描述 + 空行 + 勾选项 + 空行 + 按钮行)
    {
        InterruptFixture f;
        auto             ch = f.makeChannel();
        f.addInterrupt(
            ch,
            "bool",
            "no",
            "read_file filesystem_read",
            {},
            1,
            1,
            1,
            InterruptFixture::permissionUi(),
            "/workspace/data/x.txt"
        );
        const size_t est = f.comp->interruptEstimate(0, 120);
        const size_t got = f.renderedRows();
        XX_TEST_EXPECT_EQ(got, est);
    }
    // enum 列表 (项数参与估算)
    {
        InterruptFixture f;
        auto             ch = f.makeChannel();
        f.addInterrupt(ch, "enum", "", "mode", {"a", "b", "c", "d"});
        const size_t est = f.comp->interruptEstimate(0, 120);
        const size_t got = f.renderedRows();
        XX_TEST_EXPECT_EQ(got, est);
    }
    // 校验提示行参与估算
    {
        InterruptFixture f;
        auto             ch = f.makeChannel();
        auto             mi = f.addInterrupt(ch, "int", "0");
        f.click(mi, "value", 2);
        f.type("abc");
        f.comp->OnEvent(ftxui::Event::Return);
        const size_t est = f.comp->interruptEstimate(mi, 120);
        const size_t got = f.renderedRows();
        XX_TEST_EXPECT_EQ(got, est);
    }
}

// ---------------------------------------------------------------------------
// 表单状态版本 (驱动消息列表缓存失效)
// ---------------------------------------------------------------------------

void test_form_state_version_bumps_on_change() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    const auto       ui = InterruptFixture::permissionUi();
    auto             mi = f.addInterrupt(ch, "bool", "no", "perm", {}, 1, 1, 1, ui, "/tmp/p");
    const uint64_t   v0 = f.comp->interruptUiState(mi).version;
    f.click(mi, "remember", 0);
    const uint64_t v1 = f.comp->interruptUiState(mi).version;
    XX_TEST_EXPECT_TRUE(v1 > v0);
    f.click(mi, "value", 0);
    const uint64_t v2 = f.comp->interruptUiState(mi).version;
    XX_TEST_EXPECT_TRUE(v2 > v1);
}

void test_missing_descriptor_diagnostic() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    // 直接构造无描述的消息 (契约违规: 服务端必填; 客户端不再回退默认模板)
    auto m                     = std::make_shared<TUIMessage>();
    m->role                    = TUIMessage::Role::Interrupt;
    m->interrupt               = TUIMessage::InterruptData{};
    m->interrupt->interruptId  = 1;
    m->interrupt->inputLabel   = "label";
    m->interrupt->inputType    = "bool";
    m->interrupt->inputDefault = "no";
    m->interrupt->inputIndex   = 1;
    m->interrupt->inputTotal   = 1;
    f.sharedState.mutate([&](TUIRenderState& st) {
        st.messages.push_back(std::move(m));
    });
    f.comp->attachInterruptChannel(1, ch);

    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("缺少 UI 描述") != std::string::npos);
    // 不可交互: 无任何命中区域
    XX_TEST_EXPECT_TRUE(f.comp->interruptHitBoxes().empty());
    XX_TEST_EXPECT_FALSE(f.click(0, "value", 0));
    XX_TEST_EXPECT_FALSE(f.click(0, "submit", 0));
    // 估算与实际渲染一致 (诊断行 1 行 + 消息尾部空行)
    XX_TEST_EXPECT_EQ(f.comp->interruptEstimate(0, 120), size_t{1});
    XX_TEST_EXPECT_EQ(f.renderedRows(), size_t{1});
}

TestResult testTuiInterrupt() {
    g_tui_interrupt_passed = 0;
    g_tui_interrupt_failed = 0;

    auto savedLang = TUISettings::instance().language();
    TUISettings::instance().setLanguage(TuiLanguage::ZhCn);

    // 通用默认描述 (defaultUi)
    test_default_descriptor_bool_render();
    test_default_descriptor_value_buttons_confirm();
    test_default_descriptor_bool_default_no_selected();
    test_confirm_renders_status_line();
    test_escape_blurs_active_message();
    // 数值
    test_int_step_plus_minus();
    test_int_arrow_step_active();
    test_int_manual_edit_replace_default();
    test_int_invalid_rejected_and_recover();
    test_double_step_keeps_fraction();
    test_double_step_integer_style();
    test_double_invalid_rejected();
    // 枚举 / 文本
    test_enum_render_list_and_select();
    test_enum_many_items_all_rendered();
    test_string_replace_default_and_confirm();
    test_string_enter_uses_default();
    // 权限询问卡片 (描述驱动, 客户端无特化分支)
    test_permission_card_layout_from_descriptor();
    test_permission_allow_with_remember_returns_options();
    test_permission_deny_without_remember_has_empty_options();
    test_permission_wrapped_long_path();
    // 描述扩展性
    test_custom_descriptor_renders_items();
    test_custom_value_buttons_and_toggle_options();
    // 描述缺失 (契约违规): 诊断行 + 不可交互
    test_missing_descriptor_diagnostic();
    // 取消 / 过期 / 状态清理
    test_cancel_marks_all_and_notifies();
    test_cancel_renders_status_line();
    test_expired_renders_and_not_interactive();
    test_release_channel_clears_form_state();
    // 估算与实测一致 / 状态版本
    test_estimate_matches_rendered_rows();
    test_form_state_version_bumps_on_change();

    TUISettings::instance().setLanguage(savedLang);

    return TestResult{g_tui_interrupt_passed, g_tui_interrupt_failed};
}

} // namespace test
} // namespace agentxx
