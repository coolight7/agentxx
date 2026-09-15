#include "test_tui_interrupt.h"

#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/interrupt_view.h"
#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/middlewares/interrupt_presets.h"
#include "agentxx/middlewares/interrupt_ui.h"
#include "agentxx/middlewares/middleware.h"
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
#include <string_view>

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
// 测试夹具: 消息列表内嵌中断表单 (Role::Interrupt) 的通用渲染与交互
//
// 中断形态完全由消息携带的 UI 描述 (`InterruptData::ui`, schema 见
// agentxx/middlewares/interrupt_ui.h) 决定: 描述 = 有序块列表, 内容块
// (text/markdown/diff/separator/gap) 与控件块 (control: buttons/select/text/
// number/checkbox) 混排 + 提交行 (submit)。本文件按描述数据构造消息, 断言
// 通用渲染/交互/结果组装 (客户端不含任何 permission 特化分支)。
//
// **无历史版本兼容**: 描述缺失/非法按契约错误处理 (诊断行 + 不可交互)。
//
// 命中区域: (消息下标, 块下标, 控件 id, 子序号); 结果: {"values": {控件 id: 值}}
// ---------------------------------------------------------------------------

namespace {

/// 统计子串出现次数 (校验同一标签不重复渲染)
size_t countOccurrences(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }
    size_t count = 0;
    for (size_t pos = text.find(needle); pos != std::string_view::npos;
         pos        = text.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

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

    /// 追加一条中断表单消息 (描述必填), 返回其消息索引
    size_t addInterrupt(
        std::shared_ptr<InterruptResultChannel> ch,
        const agentxx::middleware::InterruptUi& ui,
        int64_t                                 interruptId = 1
    ) {
        return addInterruptJson(ch, ui.toJson(), interruptId);
    }

    /// 追加一条中断表单消息 (原始描述 JSON; 传空 JSON = 无描述, 契约错误用例)
    size_t addInterruptJson(
        std::shared_ptr<InterruptResultChannel> ch,
        const agentxx::util::Json&              uiJson,
        int64_t                                 interruptId = 1
    ) {
        auto m                    = std::make_shared<TUIMessage>();
        m->role                   = TUIMessage::Role::Interrupt;
        m->interrupt              = TUIMessage::InterruptData{};
        m->interrupt->interruptId = interruptId;
        m->interrupt->ui          = uiJson;
        sharedState.mutate([&](TUIRenderState& st) {
            st.messages.push_back(std::move(m));
        });
        comp->attachInterruptChannel(interruptId, ch);
        return sharedState.readSnapshot()->messages.size() - 1;
    }

    /// 渲染消息列表 (刷新中断控件命中区域) 并取回文本
    std::string render(int width = 120, int height = 60) {
        ctx.frameState = sharedState.readSnapshot();
        auto el        = comp->Render();
        auto screen    = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(width),
            ftxui::Dimension::Fixed(height)
        );
        ftxui::Render(screen, el);
        return screen.ToString();
    }

    /// 渲染并统计内容占据的行数 (首/末非空行之间的跨度, 含内部空行):
    /// 用于校验"估算 == 实测" (消息块 = 内容行 + 尾部空行)
    size_t renderedRows(int width = 120, int height = 60) {
        ctx.frameState = sharedState.readSnapshot();
        auto el        = comp->Render();
        auto screen    = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(width),
            ftxui::Dimension::Fixed(height)
        );
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

    /// 在指定消息的控件上模拟鼠标点击 (按控件 id + 子序号; 先渲染刷新命中区域)
    bool click(size_t msgIndex, const std::string& controlId, int sub = 0) {
        render();
        for (const auto& h : comp->interruptHitBoxes()) {
            if (h.msgIndex != msgIndex || h.controlId != controlId || h.sub != sub || !h.box) {
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

    /// 按描述块下标点击 (控件 id 重复或需精确定位块时使用)
    bool clickBlock(size_t msgIndex, size_t blockIndex, int sub = 0) {
        render();
        for (const auto& h : comp->interruptHitBoxes()) {
            if (h.msgIndex != msgIndex || h.blockIndex != blockIndex || h.sub != sub || !h.box) {
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

    /// 依次输入字符串的每个字符 (作用于激活的中断消息的聚焦控件)
    void type(const std::string& s) {
        for (char c : s) {
            comp->OnEvent(ftxui::Event::Character(c));
        }
    }

    /// 表单提交结果 (通道读取)
    struct Submit {
        bool                cancelled = false;
        agentxx::util::Json values    = agentxx::util::Json::object();

        /// 便捷: 指定控件 id 的字符串值 (不存在返回 nullopt)
        std::optional<std::string> get(const std::string& id) const {
            if (!values.is_object() || !values.contains(id)) {
                return std::nullopt;
            }
            const auto& v = values[id];
            return v.is_string() ? std::optional<std::string>{v.get<std::string>()}
                                 : std::optional<std::string>{v.dump()};
        }

        bool has(const std::string& id) const {
            return values.is_object() && values.contains(id);
        }
    };

    /// 从通道读取一次提交结果; 返回 false = 无可用消息
    bool recvForm(std::shared_ptr<InterruptResultChannel> ch, Submit& out) {
        bool got = ch->try_receive([&](neograph_asio_error_code, InterruptFormSubmit s) {
            out.cancelled = s.cancelled;
            out.values    = std::move(s.values);
        });
        io.run(); // 排空 async_send 投递的完成 handler (数据本身已入队)
        return got;
    }

    /// 读取消息的快照副本 (校验状态用)
    TUIMessage snapshotMsg(size_t mi) {
        auto snap = sharedState.readSnapshot();
        return (mi < snap->messages.size()) ? *snap->messages[mi] : TUIMessage{};
    }

    /// 指定消息指定控件的状态副本 (测试便捷访问; 空 id 返回默认值)
    agentxx::client::InterruptView::ControlState
        controlState(size_t mi, const std::string& controlId) {
        auto state = comp->interruptUiState(mi);
        return state.control(controlId);
    }

    /// 权限询问描述 (agent 侧预设模板构造; 客户端仅按数据渲染)
    static agentxx::middleware::InterruptUi permissionUi() {
        return agentxx::middleware::preset::permissionCard(
            "read_file",
            "filesystem_read",
            "/workspace/data/x.txt"
        );
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 头行 (默认前缀 / 自定义分段)
// ---------------------------------------------------------------------------

void test_header_default_and_custom() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    // 默认头行 (preset::inputForm 不声明头行分段)
    auto ui = agentxx::middleware::preset::inputForm({
        agentxx::middleware::preset::InputSpec{.label = "Q", .type = "string"},
    });
    f.addInterrupt(ch, ui);
    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("! [中断] ") != std::string::npos);

    // 自定义头行分段 (权限卡片预设)
    InterruptFixture f2;
    auto             ch2 = f2.makeChannel();
    f2.addInterrupt(ch2, InterruptFixture::permissionUi());
    const std::string text2 = f2.render();
    XX_TEST_EXPECT_TRUE(text2.find("! [权限] ") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text2.find("read_file") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text2.find("filesystem_read") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text2.find("! [中断] ") == std::string::npos);
}

// ---------------------------------------------------------------------------
// 内容块: text / markdown / diff / separator / gap / custom
// ---------------------------------------------------------------------------

void test_text_block_render_and_estimate() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    agentxx::middleware::InterruptUi ui;
    ui.blocks.push_back(agentxx::middleware::preset::textBlock("plain line"));
    ui.blocks.push_back(agentxx::middleware::preset::textBlock("wrapped", "hint", 2, true));
    ui.blocks.push_back(agentxx::middleware::preset::separatorBlock());
    ui.blocks.push_back(agentxx::middleware::preset::textBlock("after separator", "accent"));

    auto mi       = f.addInterrupt(ch, ui);
    auto rendered = f.render();
    XX_TEST_EXPECT_TRUE(rendered.find("plain line") != std::string::npos);
    XX_TEST_EXPECT_TRUE(rendered.find("wrapped") != std::string::npos);
    XX_TEST_EXPECT_TRUE(rendered.find("after separator") != std::string::npos);
    XX_TEST_EXPECT_TRUE(rendered.find("─") != std::string::npos); // 分隔线
    // 估算 == 实测 (渲染与估算同一布局过程)
    XX_TEST_EXPECT_EQ(f.comp->interruptEstimate(mi, 120), f.renderedRows());
}

void test_text_block_wrap_counts_lines() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    // 无空格长路径: wrap=true 时按宽度硬折行 (多行), wrap=false 时单行
    const std::string longPath
        = "/home/coolight/program/agentxx/agent/lib/include/agentxx/agent/conversation_types.h";
    agentxx::middleware::InterruptUi ui;
    ui.blocks.push_back(agentxx::middleware::preset::textBlock(longPath, "hint", 0, true));
    auto miWrap = f.addInterrupt(ch, ui, 1);
    XX_TEST_EXPECT_TRUE(f.comp->interruptEstimate(miWrap, 40) > 1);
    XX_TEST_EXPECT_EQ(f.comp->interruptEstimate(miWrap, 40), f.renderedRows(40, 30));
    const std::string text = f.render(40, 30);
    XX_TEST_EXPECT_TRUE(text.find("/home/coolight") != std::string::npos);
    // 硬折行按宽度切分 (长文件名可能被切开), 断言末段内容可见
    XX_TEST_EXPECT_TRUE(text.find("types.h") != std::string::npos);

    agentxx::middleware::InterruptUi uiNoWrap;
    uiNoWrap.blocks.push_back(agentxx::middleware::preset::textBlock(longPath, "hint", 0, false));
    auto miNoWrap = f.addInterruptJson(ch, uiNoWrap.toJson(), 2);
    XX_TEST_EXPECT_EQ(f.comp->interruptEstimate(miNoWrap, 40), size_t{2}); // 头行 + 1 行
}

void test_markdown_block_render() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    agentxx::middleware::InterruptUi ui;
    ui.blocks.push_back(agentxx::middleware::preset::markdownBlock(
        "# Title\n\nsome **bold** text\n\n- item one\n- item two"
    ));
    auto mi = f.addInterrupt(ch, ui);

    auto rendered = f.render();
    XX_TEST_EXPECT_TRUE(rendered.find("Title") != std::string::npos);
    XX_TEST_EXPECT_TRUE(rendered.find("bold") != std::string::npos);
    XX_TEST_EXPECT_TRUE(rendered.find("item one") != std::string::npos);
    // markdown 块参与估算 (多行; 估算与实测允许 1 行偏差:
    // markdown 渲染器的块间空行口径与估算近似, 进入视口后由懒列表实测修正)
    const size_t markdownEst = f.comp->interruptEstimate(mi, 120);
    const size_t markdownGot = f.renderedRows();
    XX_TEST_EXPECT_TRUE(markdownEst > 2);
    XX_TEST_EXPECT_TRUE(markdownEst + 1 >= markdownGot && markdownEst <= markdownGot + 1);
}

void test_diff_block_render() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    agentxx::middleware::InterruptUi ui;
    ui.blocks.push_back(agentxx::middleware::preset::diffBlock(
        "a.txt",
        "line1\nold line\nline3\n",
        "line1\nnew line\nline3\n"
    ));
    auto mi = f.addInterrupt(ch, ui);

    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("a.txt") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("old line") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("new line") != std::string::npos);
    XX_TEST_EXPECT_EQ(f.comp->interruptEstimate(mi, 120), f.renderedRows());

    // 无差异: 渲染 "(no changes)"
    InterruptFixture                 f2;
    auto                             ch2 = f2.makeChannel();
    agentxx::middleware::InterruptUi uiSame;
    uiSame.blocks.push_back(agentxx::middleware::preset::diffBlock("b.txt", "same\n", "same\n"));
    auto miSame = f2.addInterrupt(ch2, uiSame);
    XX_TEST_EXPECT_TRUE(f2.render().find("same") != std::string::npos);
    XX_TEST_EXPECT_EQ(f2.comp->interruptEstimate(miSame, 120), f2.renderedRows());
}

void test_gap_unknown_and_custom_blocks() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    agentxx::middleware::InterruptUi ui;
    ui.blocks.push_back(agentxx::middleware::preset::textBlock("first"));
    ui.blocks.push_back(agentxx::middleware::preset::gapBlock(2));
    ui.blocks.push_back(agentxx::middleware::preset::textBlock("second"));

    // 未知块 kind: 忽略 (向前兼容)
    agentxx::middleware::InterruptUiBlock unknown;
    unknown.kind = "future_kind";
    unknown.text = "SHOULD_NOT_RENDER";
    ui.blocks.push_back(unknown);

    // 自定义渲染块 (字段预留, 暂未实现): 渲染 fallback 文本
    agentxx::middleware::InterruptUiBlock custom;
    custom.kind      = "custom";
    custom.component = "my_component";
    custom.fallback  = "custom fallback text";
    ui.blocks.push_back(custom);

    // 无 fallback: 输出组件名占位
    agentxx::middleware::InterruptUiBlock custom2;
    custom2.kind      = "custom";
    custom2.component = "another_component";
    ui.blocks.push_back(custom2);

    auto mi       = f.addInterruptJson(ch, ui.toJson());
    auto rendered = f.render();
    XX_TEST_EXPECT_TRUE(rendered.find("first") != std::string::npos);
    XX_TEST_EXPECT_TRUE(rendered.find("second") != std::string::npos);
    XX_TEST_EXPECT_TRUE(rendered.find("SHOULD_NOT_RENDER") == std::string::npos);
    XX_TEST_EXPECT_TRUE(rendered.find("custom fallback text") != std::string::npos);
    XX_TEST_EXPECT_TRUE(rendered.find("another_component") != std::string::npos);
    XX_TEST_EXPECT_EQ(f.comp->interruptEstimate(mi, 120), f.renderedRows());
}

// ---------------------------------------------------------------------------
// 控件: buttons / select / checkbox / number / text
// ---------------------------------------------------------------------------

/// 单控件描述 (buttons, 点击即提交)
agentxx::middleware::InterruptUi makeBoolButtonsUi() {
    using namespace agentxx::middleware;
    InterruptUi ui;
    ui.blocks.push_back(preset::textBlock("run this tool?", "accent", 2, false, true));
    ui.blocks.push_back(preset::gapBlock(1));
    ui.blocks.push_back(preset::buttonControl(
        "allow",
        {preset::option("true", "Yes", "interrupt.yes"),
         preset::option("false", "No", "interrupt.no")},
        {},
        {},
        agentxx::util::Json(false),
        true
    ));
    return ui;
}

void test_buttons_commit_on_pick() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, makeBoolButtonsUi());

    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("run this tool?") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("是") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("否") != std::string::npos);

    // 点击 "是" (下标 0) → 立即提交, 结果 values["allow"] = "true"
    XX_TEST_EXPECT_TRUE(f.click(mi, "allow", 0));
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_TRUE(f.recvForm(ch, s));
    XX_TEST_EXPECT_FALSE(s.cancelled);
    XX_TEST_EXPECT_TRUE(s.values.is_object());
    XX_TEST_EXPECT_EQ(s.get("allow").value_or(""), std::string("true"));

    auto msg = f.snapshotMsg(mi);
    XX_TEST_EXPECT_TRUE(msg.interrupt.has_value());
    if (msg.interrupt) {
        XX_TEST_EXPECT_EQ(
            static_cast<int>(msg.interrupt->interruptStatus),
            static_cast<int>(TUIMessage::InterruptStatus::Confirmed)
        );
        XX_TEST_EXPECT_TRUE(
            msg.interrupt->interruptResult.find("allow: true") != std::string::npos
        );
    }
    // 提交后控件不再可交互
    XX_TEST_EXPECT_FALSE(f.click(mi, "allow", 1));
}

void test_buttons_without_commit_selects_then_enter() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    using namespace agentxx::middleware;
    InterruptUi ui;
    ui.blocks.push_back(preset::buttonControl(
        "decision",
        {preset::option("true", "Allow", "interrupt.allow"),
         preset::option("false", "Deny", "interrupt.deny", "error")},
        {},
        {},
        agentxx::util::Json("false"),
        false // 不点击即提交: 仅选中, 由提交行提交
    ));
    ui.blocks.push_back(preset::submitBlock());
    auto mi = f.addInterrupt(ch, ui);

    // 默认选中 "Deny" (下标 1)
    XX_TEST_EXPECT_EQ(f.controlState(mi, "decision").selected, 1);
    // 点击 "Allow": 仅选中, 不提交
    XX_TEST_EXPECT_TRUE(f.click(mi, "decision", 0));
    XX_TEST_EXPECT_EQ(f.controlState(mi, "decision").selected, 0);
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_FALSE(f.recvForm(ch, s));
    // 键盘 → 切回 "Deny"; Enter 提交
    f.comp->OnEvent(ftxui::Event::ArrowRight);
    f.comp->OnEvent(ftxui::Event::ArrowLeft);
    XX_TEST_EXPECT_EQ(f.controlState(mi, "decision").selected, 0);
    f.comp->OnEvent(ftxui::Event::Return);
    XX_TEST_EXPECT_TRUE(f.recvForm(ch, s));
    XX_TEST_EXPECT_EQ(s.get("decision").value_or(""), std::string("true"));
}

void test_select_click_and_keyboard() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    using namespace agentxx::middleware;
    InterruptUi ui;
    ui.blocks.push_back(preset::selectControl(
        "mode",
        {preset::option("fast", "fast"),
         preset::option("balanced", "balanced"),
         preset::option("slow", "slow")},
        {},
        {},
        agentxx::util::Json("balanced")
    ));
    ui.blocks.push_back(preset::submitBlock());
    auto mi = f.addInterrupt(ch, ui);

    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("balanced") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("slow") != std::string::npos);
    // 默认选中 "balanced" (下标 1)
    XX_TEST_EXPECT_EQ(f.controlState(mi, "mode").selected, 1);

    // 点击第 3 项 → 选中 "slow"
    XX_TEST_EXPECT_TRUE(f.click(mi, "mode", 2));
    XX_TEST_EXPECT_EQ(f.controlState(mi, "mode").selected, 2);
    // 键盘 ↑ 回到 balanced, Enter 提交
    f.comp->OnEvent(ftxui::Event::ArrowUp);
    XX_TEST_EXPECT_EQ(f.controlState(mi, "mode").selected, 1);
    f.comp->OnEvent(ftxui::Event::Return);
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_TRUE(f.recvForm(ch, s));
    XX_TEST_EXPECT_EQ(s.get("mode").value_or(""), std::string("balanced"));
}

void test_checkbox_click_and_space() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    using namespace agentxx::middleware;
    InterruptUi ui;
    ui.blocks.push_back(
        preset::checkboxControl("remember", "Remember this choice", "interrupt.remember")
    );
    ui.blocks.push_back(preset::submitBlock());
    auto mi = f.addInterrupt(ch, ui);

    XX_TEST_EXPECT_FALSE(f.controlState(mi, "remember").checked);
    XX_TEST_EXPECT_TRUE(f.click(mi, "remember", 0));
    XX_TEST_EXPECT_TRUE(f.controlState(mi, "remember").checked);
    XX_TEST_EXPECT_TRUE(f.render().find("[ ✓ ]") != std::string::npos);
    // 空格切换 (键盘作用于聚焦控件)
    f.comp->OnEvent(ftxui::Event::Character(' '));
    XX_TEST_EXPECT_FALSE(f.controlState(mi, "remember").checked);
    f.comp->OnEvent(ftxui::Event::Character(' '));
    XX_TEST_EXPECT_TRUE(f.controlState(mi, "remember").checked);

    f.comp->OnEvent(ftxui::Event::Return);
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_TRUE(f.recvForm(ch, s));
    XX_TEST_EXPECT_TRUE(s.values.is_object());
    XX_TEST_EXPECT_TRUE(s.values.value("remember", false));
}

void test_number_step_and_edit() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    using namespace agentxx::middleware;
    InterruptUi ui;
    ui.blocks.push_back(preset::numberControl("count", "Count", {}, 5.0, true, 1.0));
    ui.blocks.push_back(preset::submitBlock());
    auto mi = f.addInterrupt(ch, ui);

    // 默认值 5; 点击 "+" (子序号 1) → 6; 点击 "-" (子序号 0) 两次 → 4
    XX_TEST_EXPECT_EQ(f.controlState(mi, "count").editText, std::string("5"));
    XX_TEST_EXPECT_TRUE(f.click(mi, "count", 1));
    XX_TEST_EXPECT_EQ(f.controlState(mi, "count").editText, std::string("6"));
    XX_TEST_EXPECT_TRUE(f.click(mi, "count", 0));
    XX_TEST_EXPECT_TRUE(f.click(mi, "count", 0));
    XX_TEST_EXPECT_EQ(f.controlState(mi, "count").editText, std::string("4"));
    // 输入框 (子序号 2) 激活: 步进已置 edited=true, 手动编辑按追加处理
    // (与输入框语义一致); 先退格清空再输入 42
    XX_TEST_EXPECT_TRUE(f.click(mi, "count", 2));
    for (int i = 0; i < 2; ++i) {
        f.comp->OnEvent(ftxui::Event::Backspace);
    }
    f.type("42");
    XX_TEST_EXPECT_EQ(f.controlState(mi, "count").editText, std::string("42"));
    f.comp->OnEvent(ftxui::Event::Return);
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_TRUE(f.recvForm(ch, s));
    XX_TEST_EXPECT_TRUE(s.values["count"].is_number_integer());
    XX_TEST_EXPECT_EQ(s.values["count"].get<int64_t>(), int64_t{42});
}

void test_number_validation_tip_and_recover() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    using namespace agentxx::middleware;
    InterruptUi ui;
    ui.blocks.push_back(preset::numberControl("count", "Count", {}, 0.0, true, 1.0));
    ui.blocks.push_back(preset::submitBlock());
    auto mi = f.addInterrupt(ch, ui);

    // 非法整数: 提交被拒, 提示行出现在该控件下方, 状态仍为 Waiting
    XX_TEST_EXPECT_TRUE(f.click(mi, "count", 2));
    f.type("abc");
    f.comp->OnEvent(ftxui::Event::Return);
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_FALSE(f.recvForm(ch, s));
    XX_TEST_EXPECT_TRUE(f.controlState(mi, "count").tip.find("无效整数") != std::string::npos);
    XX_TEST_EXPECT_TRUE(f.render().find("无效整数") != std::string::npos);
    auto msg = f.snapshotMsg(mi);
    XX_TEST_EXPECT_TRUE(msg.interrupt.has_value());
    if (msg.interrupt) {
        XX_TEST_EXPECT_EQ(
            static_cast<int>(msg.interrupt->interruptStatus),
            static_cast<int>(TUIMessage::InterruptStatus::Waiting)
        );
    }
    // 提示行存在时估算与实测仍一致
    XX_TEST_EXPECT_EQ(f.comp->interruptEstimate(mi, 120), f.renderedRows());
    // 修正后提交成功
    for (int i = 0; i < 3; ++i) {
        f.comp->OnEvent(ftxui::Event::Backspace);
    }
    f.type("7");
    XX_TEST_EXPECT_TRUE(f.controlState(mi, "count").tip.empty());
    f.comp->OnEvent(ftxui::Event::Return);
    XX_TEST_EXPECT_TRUE(f.recvForm(ch, s));
    XX_TEST_EXPECT_EQ(s.values["count"].get<int64_t>(), int64_t{7});
}

void test_number_range_validation() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    using namespace agentxx::middleware;
    InterruptUi ui;
    auto        number = preset::numberControl("count", "Count", {}, 5.0, true, 1.0);
    number.hasMin      = true;
    number.minValue    = 1.0;
    number.hasMax      = true;
    number.maxValue    = 10.0;
    ui.blocks.push_back(number);
    ui.blocks.push_back(preset::submitBlock());
    auto mi = f.addInterrupt(ch, ui);

    XX_TEST_EXPECT_TRUE(f.click(mi, "count", 2));
    f.type("99");
    f.comp->OnEvent(ftxui::Event::Return);
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_FALSE(f.recvForm(ch, s));
    XX_TEST_EXPECT_TRUE(f.controlState(mi, "count").tip.find("超出范围") != std::string::npos);
}

void test_text_control_edit_and_submit() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    using namespace agentxx::middleware;
    InterruptUi ui;
    ui.blocks.push_back(preset::textControl("path", "Path", {}, "/tmp/a.txt"));
    ui.blocks.push_back(preset::submitBlock());
    auto mi = f.addInterrupt(ch, ui);

    XX_TEST_EXPECT_TRUE(f.render().find("/tmp/a.txt") != std::string::npos);
    // 点击输入框激活 → 输入替换默认值
    XX_TEST_EXPECT_TRUE(f.click(mi, "path", 0));
    f.type("/etc/hosts");
    XX_TEST_EXPECT_EQ(f.controlState(mi, "path").editText, std::string("/etc/hosts"));
    f.comp->OnEvent(ftxui::Event::Return);
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_TRUE(f.recvForm(ch, s));
    XX_TEST_EXPECT_EQ(s.get("path").value_or(""), std::string("/etc/hosts"));
}

void test_escape_blurs_active_message() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, makeBoolButtonsUi());

    auto number = agentxx::middleware::preset::textControl("path", "Path", {}, "hi");
    (void)number;

    // 点击控件激活该消息
    XX_TEST_EXPECT_TRUE(f.click(mi, "allow", 0)); // buttons commitOnPick: 会提交
    // 改为验证 Esc 失焦: 新消息 (不提交)
    InterruptFixture f2;
    auto             ch2 = f2.makeChannel();
    using namespace agentxx::middleware;
    InterruptUi ui;
    ui.blocks.push_back(preset::textControl("path", "Path", {}, "hi"));
    auto mi2 = f2.addInterrupt(ch2, ui);
    XX_TEST_EXPECT_TRUE(f2.click(mi2, "path", 0));
    XX_TEST_EXPECT_EQ(f2.comp->activeInterruptMsg(), mi2);
    f2.comp->OnEvent(ftxui::Event::Escape);
    XX_TEST_EXPECT_EQ(f2.comp->activeInterruptMsg(), static_cast<size_t>(-1));
    // 失焦后输入不再作用于该消息
    f2.type("x");
    XX_TEST_EXPECT_EQ(f2.controlState(mi2, "path").editText, std::string("hi"));
}

// ---------------------------------------------------------------------------
// 多控件表单与结果契约
// ---------------------------------------------------------------------------

/// 双控件 (text + number) + 勾选 + 提交行
agentxx::middleware::InterruptUi makeMultiControlUi() {
    using namespace agentxx::middleware;
    InterruptUi ui;
    ui.blocks.push_back(preset::textBlock("Edit request", "", 0, false, true));
    ui.blocks.push_back(preset::textControl("path", "Path", {}, "/tmp/a.txt"));
    ui.blocks.push_back(preset::gapBlock(1));
    ui.blocks.push_back(preset::numberControl("count", "Count", {}, 3.0, true, 1.0));
    ui.blocks.push_back(preset::gapBlock(1));
    ui.blocks.push_back(preset::checkboxControl("force", "Force write"));
    ui.blocks.push_back(preset::submitBlock());
    return ui;
}

void test_form_submits_all_control_values() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, makeMultiControlUi());

    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("Edit request") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("Path") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("/tmp/a.txt") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("Count") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("Force write") != std::string::npos);

    // 修改文本控件 + 数值控件 + 勾选项, 一次提交全部值
    XX_TEST_EXPECT_TRUE(f.click(mi, "path", 0));
    f.type("edited");
    XX_TEST_EXPECT_EQ(f.controlState(mi, "path").editText, std::string("edited"));
    XX_TEST_EXPECT_TRUE(f.click(mi, "count", 1)); // +
    XX_TEST_EXPECT_EQ(f.controlState(mi, "count").editText, std::string("4"));
    XX_TEST_EXPECT_TRUE(f.click(mi, "force", 0));

    XX_TEST_EXPECT_TRUE(f.click(mi, "submit", 0));
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_TRUE(f.recvForm(ch, s));
    XX_TEST_EXPECT_FALSE(s.cancelled);
    XX_TEST_EXPECT_TRUE(s.values.is_object());
    XX_TEST_EXPECT_EQ(s.values.size(), size_t{3});
    XX_TEST_EXPECT_EQ(s.get("path").value_or(""), std::string("edited"));
    XX_TEST_EXPECT_EQ(s.values["count"].get<int64_t>(), int64_t{4});
    XX_TEST_EXPECT_TRUE(s.values.value("force", false));
    // 状态行展示各控件结果 (标签: 值)
    XX_TEST_EXPECT_TRUE(f.render().find("已确认") != std::string::npos);
}

void test_form_without_controls_submits_empty_values() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    agentxx::middleware::InterruptUi ui;
    ui.blocks.push_back(agentxx::middleware::preset::textBlock("Please review the plan"));
    ui.blocks.push_back(agentxx::middleware::preset::submitBlock());
    auto mi = f.addInterrupt(ch, ui);

    XX_TEST_EXPECT_TRUE(f.click(mi, "submit", 0));
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_TRUE(f.recvForm(ch, s));
    XX_TEST_EXPECT_FALSE(s.cancelled);
    XX_TEST_EXPECT_TRUE(s.values.is_object());
    XX_TEST_EXPECT_TRUE(s.values.empty());
    // 无结果值: 状态行用无占位符文案
    const std::string statusText = f.render();
    XX_TEST_EXPECT_TRUE(statusText.find("已确认") != std::string::npos);
    XX_TEST_EXPECT_TRUE(statusText.find("已确认:") == std::string::npos);
}

void test_keyboard_acts_on_focused_control() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, makeMultiControlUi());

    // 点击第 2 个控件 (number) → 键盘作用于它
    XX_TEST_EXPECT_TRUE(f.click(mi, "count", 2));
    f.comp->OnEvent(ftxui::Event::ArrowUp);
    XX_TEST_EXPECT_EQ(f.controlState(mi, "count").editText, std::string("4"));
    XX_TEST_EXPECT_EQ(f.controlState(mi, "path").editText, std::string("/tmp/a.txt"));
    // 切回第 1 个控件: 输入作用于它
    XX_TEST_EXPECT_TRUE(f.click(mi, "path", 0));
    f.type("z");
    XX_TEST_EXPECT_EQ(f.controlState(mi, "path").editText, std::string("z"));
    XX_TEST_EXPECT_EQ(f.controlState(mi, "count").editText, std::string("4"));
}

// ---------------------------------------------------------------------------
// 预设模板: 权限卡片 (客户端无 permission 特化分支)
// ---------------------------------------------------------------------------

void test_permission_card_render_and_result() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, InterruptFixture::permissionUi());

    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("! [权限] ") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("read_file") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("/workspace/data/x.txt") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("记住此选择") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("完全授权所有权限") != std::string::npos);
    // 勾选控件标签即行内文本: 只渲染一次 (无额外标题行)
    XX_TEST_EXPECT_EQ(countOccurrences(text, "记住此选择"), size_t{1});
    XX_TEST_EXPECT_EQ(countOccurrences(text, "完全授权所有权限"), size_t{1});
    XX_TEST_EXPECT_TRUE(text.find("[   ]") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("允许") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("拒绝") != std::string::npos);
    // 权限卡片预设无提交行 (允许/拒绝点击即提交)
    XX_TEST_EXPECT_TRUE(text.find("确认") == std::string::npos);

    // 勾选"记住此选择" → 点击"允许"提交: values = {decision: "true", remember: true, fullAuth:
    // false}
    XX_TEST_EXPECT_TRUE(f.click(mi, "remember", 0));
    XX_TEST_EXPECT_TRUE(f.render().find("[ ✓ ]") != std::string::npos);
    XX_TEST_EXPECT_TRUE(f.click(mi, "decision", 0));
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_TRUE(f.recvForm(ch, s));
    XX_TEST_EXPECT_EQ(s.get("decision").value_or(""), std::string("true"));
    XX_TEST_EXPECT_TRUE(s.values.value("remember", false));
    XX_TEST_EXPECT_FALSE(s.values.value("fullAuth", false));

    // 勾选"完全授权所有权限" → 点击"允许"提交: values = {decision: "true", fullAuth: true}
    InterruptFixture fFull;
    auto             chFull = fFull.makeChannel();
    auto             miFull = fFull.addInterrupt(chFull, InterruptFixture::permissionUi());
    XX_TEST_EXPECT_TRUE(fFull.click(miFull, "fullAuth", 0));
    XX_TEST_EXPECT_TRUE(fFull.click(miFull, "decision", 0));
    InterruptFixture::Submit sFull;
    XX_TEST_EXPECT_TRUE(fFull.recvForm(chFull, sFull));
    XX_TEST_EXPECT_EQ(sFull.get("decision").value_or(""), std::string("true"));
    XX_TEST_EXPECT_TRUE(sFull.values.value("fullAuth", false));

    // 拒绝 (未勾选 remember): values.decision = "false"
    InterruptFixture f2;
    auto             ch2 = f2.makeChannel();
    auto             mi2 = f2.addInterrupt(ch2, InterruptFixture::permissionUi());
    XX_TEST_EXPECT_TRUE(f2.click(mi2, "decision", 1));
    InterruptFixture::Submit s2;
    XX_TEST_EXPECT_TRUE(f2.recvForm(ch2, s2));
    XX_TEST_EXPECT_EQ(s2.get("decision").value_or(""), std::string("false"));
    XX_TEST_EXPECT_FALSE(s2.values.value("remember", true));

    // 文件目标: 勾选项无生效范围提示; 目录目标 (尾斜杠): 提示记住的目录规则
    // 同时覆盖其子目录与文件 (与中间件最长前缀匹配语义一致)
    XX_TEST_EXPECT_TRUE(text.find("且授权子目录与文件") == std::string::npos);

    InterruptFixture f3;
    auto             ch3 = f3.makeChannel();
    f3.addInterrupt(
        ch3,
        agentxx::middleware::preset::permissionCard(
            "list_dir",
            "filesystem_read",
            "/workspace/data/"
        )
    );
    const std::string dirText = f3.render();
    XX_TEST_EXPECT_TRUE(dirText.find("且授权子目录与文件") != std::string::npos);
    XX_TEST_EXPECT_TRUE(dirText.find("/workspace/data/") != std::string::npos);
}

void test_permission_wrapped_long_path() {
    InterruptFixture  f;
    auto              ch = f.makeChannel();
    const std::string longPath
        = "/home/coolight/program/agentxx/agent/lib/include/agentxx/agent/conversation_types.h";
    f.addInterrupt(
        ch,
        agentxx::middleware::preset::permissionCard("read_file", "filesystem_read", longPath)
    );
    // 窄宽度下: 描述行按宽度硬折行, 首尾均可渲染 (不被压为 0 宽消失)
    const std::string text = f.render(40, 30);
    XX_TEST_EXPECT_TRUE(text.find("/home/coolight") != std::string::npos);
    // 硬折行按宽度切分 (长文件名可能被切开), 断言末段内容可见
    XX_TEST_EXPECT_TRUE(text.find("types.h") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 预设模板: 类型化输入表单 (inputForm)
// ---------------------------------------------------------------------------

void test_input_form_preset_types() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    using namespace agentxx::middleware;
    auto ui = preset::inputForm({
        preset::InputSpec{
                          .label        = "Mode",
                          .depict       = "pick one",
                          .type         = "enum",
                          .defaultValue = "fast",
                          .enumValues   = {"fast", "slow"}
        },
        preset::InputSpec{.label = "Retries", .type = "int", .defaultValue = "1"},
        preset::InputSpec{.label = "Enable", .type = "bool", .defaultValue = "yes"},
        preset::InputSpec{.label = "Note", .type = "string", .defaultValue = "text"},
    });
    auto mi = f.addInterrupt(ch, ui);

    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("Mode") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("pick one") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("Retries") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("Enable") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("Note") != std::string::npos);
    // 多输入项: 控件 id = value1..value4
    XX_TEST_EXPECT_EQ(f.controlState(mi, "value1").selected, 0);
    XX_TEST_EXPECT_EQ(f.controlState(mi, "value2").editText, std::string("1"));
    XX_TEST_EXPECT_EQ(f.controlState(mi, "value4").editText, std::string("text"));

    // 修改: 文本值 + 数值步进, 提交一次回传全部控件
    XX_TEST_EXPECT_TRUE(f.click(mi, "value4", 0));
    f.type("hello");
    XX_TEST_EXPECT_TRUE(f.click(mi, "value2", 1)); // +
    XX_TEST_EXPECT_EQ(f.controlState(mi, "value2").editText, std::string("2"));
    XX_TEST_EXPECT_TRUE(f.click(mi, "submit", 0));
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_TRUE(f.recvForm(ch, s));
    XX_TEST_EXPECT_EQ(s.get("value1").value_or(""), std::string("fast"));
    XX_TEST_EXPECT_EQ(s.values["value2"].get<int64_t>(), int64_t{2});
    XX_TEST_EXPECT_EQ(s.get("value4").value_or(""), std::string("hello"));
    // bool 控件 (buttons) 默认 "是": 点击即提交 — 单独一条消息验证
    InterruptFixture f2;
    auto             ch2 = f2.makeChannel();
    auto             ui2 = preset::inputForm({
        preset::InputSpec{.label = "Enable", .type = "bool", .defaultValue = "yes"},
    });
    auto             mi2 = f2.addInterrupt(ch2, ui2);
    XX_TEST_EXPECT_TRUE(f2.click(mi2, "value", 0));
    InterruptFixture::Submit s2;
    XX_TEST_EXPECT_TRUE(f2.recvForm(ch2, s2));
    XX_TEST_EXPECT_EQ(s2.get("value").value_or(""), std::string("true"));
}

// ---------------------------------------------------------------------------
// 契约错误 / 扩展性 / 取消与过期
// ---------------------------------------------------------------------------

void test_missing_descriptor_diagnostic() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    // 直接构造无描述的消息 (契约错误: HIL 中断必须携带描述)
    auto mi = f.addInterruptJson(ch, agentxx::util::Json{});

    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("缺少 UI 描述") != std::string::npos);
    // 不可交互: 无任何命中区域
    XX_TEST_EXPECT_TRUE(f.comp->interruptHitBoxes().empty());
    XX_TEST_EXPECT_FALSE(f.click(mi, "value", 0));
    // 估算与实际渲染一致 (诊断行 1 行 + 消息尾部空行)
    XX_TEST_EXPECT_EQ(f.comp->interruptEstimate(mi, 120), size_t{1});
    XX_TEST_EXPECT_EQ(f.renderedRows(), size_t{1});
}

void test_unknown_control_diagnostic_and_others_usable() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    using namespace agentxx::middleware;
    InterruptUi      ui;
    InterruptUiBlock future;
    future.kind    = "control";
    future.control = "future_widget";
    future.id      = "x";
    ui.blocks.push_back(future);
    ui.blocks.push_back(preset::textControl("path", "Path", {}, "p"));
    ui.blocks.push_back(preset::submitBlock());
    auto mi = f.addInterruptJson(ch, ui.toJson());

    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("unsupported control") != std::string::npos);
    // 其余控件仍可用
    XX_TEST_EXPECT_TRUE(f.click(mi, "path", 0));
    f.type("q");
    XX_TEST_EXPECT_EQ(f.controlState(mi, "path").editText, std::string("q"));
    XX_TEST_EXPECT_TRUE(f.click(mi, "submit", 0));
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_TRUE(f.recvForm(ch, s));
    XX_TEST_EXPECT_EQ(s.get("path").value_or(""), std::string("q"));
    XX_TEST_EXPECT_FALSE(s.has("x")); // 未知控件不参与结果
}

void test_cancel_marks_all_and_notifies() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    using namespace agentxx::middleware;
    InterruptUi ui;
    ui.blocks.push_back(preset::textControl("path", "Path"));
    ui.blocks.push_back(preset::submitBlock());
    // 同请求 (id=7) 两条消息 (防御性: 提交/取消按请求 id 归并)
    auto mi1 = f.addInterrupt(ch, ui, 7);
    auto mi2 = f.addInterrupt(ch, ui, 7);

    XX_TEST_EXPECT_TRUE(f.click(mi1, "submit", 1)); // 取消 (子序号 1)
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
    InterruptFixture::Submit s;
    XX_TEST_EXPECT_TRUE(f.recvForm(ch, s));
    XX_TEST_EXPECT_TRUE(s.cancelled);
    XX_TEST_EXPECT_TRUE(s.values.is_object() && s.values.empty());
    // 状态行
    XX_TEST_EXPECT_TRUE(f.render().find("已取消") != std::string::npos);
}

void test_expired_and_release_channel() {
    InterruptFixture f;
    auto             ch = f.makeChannel();

    using namespace agentxx::middleware;
    InterruptUi ui;
    ui.blocks.push_back(preset::textControl("path", "Path", {}, "hi"));
    ui.blocks.push_back(preset::submitBlock());
    auto mi = f.addInterrupt(ch, ui, 3);

    // 输入后释放通道: 表单状态清空并按描述默认值重新初始化
    XX_TEST_EXPECT_TRUE(f.click(mi, "path", 0));
    f.type("changed");
    XX_TEST_EXPECT_EQ(f.controlState(mi, "path").editText, std::string("changed"));
    f.comp->releaseInterruptChannel(3);
    XX_TEST_EXPECT_EQ(f.controlState(mi, "path").editText, std::string("hi"));

    // 过期状态: 不再可交互
    auto mi2 = f.addInterrupt(ch, ui, 4);
    f.sharedState.mutate([&](TUIRenderState& st) {
        if (mi2 < st.messages.size() && st.messages[mi2]->interrupt) {
            st.messages[mi2]->interrupt->interruptStatus = TUIMessage::InterruptStatus::Expired;
        }
    });
    const std::string text = f.render();
    XX_TEST_EXPECT_TRUE(text.find("已过期") != std::string::npos);
    XX_TEST_EXPECT_FALSE(f.click(mi2, "path", 0));
    XX_TEST_EXPECT_EQ(f.comp->interruptEstimate(mi2, 120), size_t{1}); // 状态行 1 行
}

// ---------------------------------------------------------------------------
// 估算与实测一致 (渲染/估算同源) 与状态版本
// ---------------------------------------------------------------------------

void test_estimate_matches_rendered_rows() {
    // 权限卡片 (自定义头行 + 折行描述 + 空行 + 勾选 + 空行 + 按钮)
    {
        InterruptFixture f;
        auto             ch = f.makeChannel();
        f.addInterrupt(ch, InterruptFixture::permissionUi());
        XX_TEST_EXPECT_EQ(f.comp->interruptEstimate(0, 120), f.renderedRows());
    }
    // 多控件表单
    {
        InterruptFixture f;
        auto             ch = f.makeChannel();
        f.addInterrupt(ch, makeMultiControlUi());
        XX_TEST_EXPECT_EQ(f.comp->interruptEstimate(0, 120), f.renderedRows());
    }
    // 类型化输入表单 (含 markdown 说明/多控件/提交行)
    {
        InterruptFixture f;
        auto             ch = f.makeChannel();
        f.addInterrupt(
            ch,
            agentxx::middleware::preset::inputForm({
                agentxx::middleware::preset::InputSpec{
                                                       .label      = "Mode",
                                                       .depict     = "pick one",
                                                       .type       = "enum",
                                                       .enumValues = {"a", "b", "c"}
                },
                agentxx::middleware::preset::InputSpec{.label = "Count", .type = "int"},
        })
        );
        XX_TEST_EXPECT_EQ(f.comp->interruptEstimate(0, 120), f.renderedRows());
    }
    // 校验提示行存在时
    {
        InterruptFixture f;
        auto             ch = f.makeChannel();
        using namespace agentxx::middleware;
        InterruptUi ui;
        ui.blocks.push_back(preset::numberControl("n", "N", {}, 0.0, true, 1.0));
        ui.blocks.push_back(preset::submitBlock());
        auto mi = f.addInterrupt(ch, ui);
        XX_TEST_EXPECT_TRUE(f.click(mi, "n", 2));
        f.type("abc");
        f.comp->OnEvent(ftxui::Event::Return);
        XX_TEST_EXPECT_EQ(f.comp->interruptEstimate(mi, 120), f.renderedRows());
    }
}

void test_state_version_bumps_on_change() {
    InterruptFixture f;
    auto             ch = f.makeChannel();
    auto             mi = f.addInterrupt(ch, InterruptFixture::permissionUi());

    const uint64_t v0 = f.comp->interruptUiState(mi).version;
    f.click(mi, "remember", 0);
    const uint64_t v1 = f.comp->interruptUiState(mi).version;
    XX_TEST_EXPECT_TRUE(v1 > v0);
    f.click(mi, "decision", 0);
    const uint64_t v2 = f.comp->interruptUiState(mi).version;
    XX_TEST_EXPECT_TRUE(v2 > v1);
}

TestResult testTuiInterrupt() {
    g_tui_interrupt_passed = 0;
    g_tui_interrupt_failed = 0;

    auto savedLang = TUISettings::instance().language();
    TUISettings::instance().setLanguage(TuiLanguage::ZhCn);

    // 头行
    test_header_default_and_custom();
    // 内容块
    test_text_block_render_and_estimate();
    test_text_block_wrap_counts_lines();
    test_markdown_block_render();
    test_diff_block_render();
    test_gap_unknown_and_custom_blocks();
    // 控件
    test_buttons_commit_on_pick();
    test_buttons_without_commit_selects_then_enter();
    test_select_click_and_keyboard();
    test_checkbox_click_and_space();
    test_number_step_and_edit();
    test_number_validation_tip_and_recover();
    test_number_range_validation();
    test_text_control_edit_and_submit();
    test_escape_blurs_active_message();
    // 多控件表单与结果契约
    test_form_submits_all_control_values();
    test_form_without_controls_submits_empty_values();
    test_keyboard_acts_on_focused_control();
    // 预设模板
    test_permission_card_render_and_result();
    test_permission_wrapped_long_path();
    test_input_form_preset_types();
    // 契约错误 / 扩展性 / 取消与过期
    test_missing_descriptor_diagnostic();
    test_unknown_control_diagnostic_and_others_usable();
    test_cancel_marks_all_and_notifies();
    test_expired_and_release_channel();
    // 估算与实测一致 / 状态版本
    test_estimate_matches_rendered_rows();
    test_state_version_bumps_on_change();

    TUISettings::instance().setLanguage(savedLang);

    return TestResult{g_tui_interrupt_passed, g_tui_interrupt_failed};
}

} // namespace test
} // namespace agentxx
