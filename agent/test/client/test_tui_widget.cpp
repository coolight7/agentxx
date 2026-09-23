// TUI 交互框架测试 (离屏渲染 + 直接驱动 OnEvent)
//
// 覆盖场景:
// - 命中登记表: 帧首清空 / 渲染时登记 / 未布局项不命中 / 登记顺序优先
// - 声明式条目列表: 选中项维护 (按 id 跨重建保持)、键盘导航 (跳过不可用项)、
//   Enter 激活、鼠标点击命中 (置选中并激活)、分组标题行 (不可点击, 不占下标)
// - 隐藏按钮不占点击区域: [ @︎ ] 按钮隐藏后, 其在上一帧的屏幕位置不再可点
#include "agentxx-test/client/test_tui_widget.h"

#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/input_bar.h"
#include "agentxx-client/io/tui/components/status_bar.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/framework/ui_action_list.h"
#include "agentxx-client/io/tui/framework/ui_hit.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <memory>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_widget_passed = 0;
int g_tui_widget_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_widget_passed
#define XX_TEST_FAILED g_tui_widget_failed

namespace agentxx {
namespace test {

using namespace agentxx::client;

namespace {

using agentxx::client::kNoBox;
using agentxx::client::UiActionItem;
using agentxx::client::UiActionList;
using agentxx::client::UiActionStyle;
using agentxx::client::UiHitInfo;
using agentxx::client::UiHitMap;

/// 构造鼠标数据 (左键释放 = 一次点击完成)
ftxui::Mouse leftClickMouse(int x, int y) {
    ftxui::Mouse m;
    m.button = ftxui::Mouse::Left;
    m.motion = ftxui::Mouse::Released;
    m.x      = x;
    m.y      = y;
    return m;
}

/// 构造鼠标事件 (左键释放 = 一次点击完成)
ftxui::Event leftClickAt(int x, int y) {
    return ftxui::Event::Mouse("", leftClickMouse(x, y));
}

/// 渲染一帧: 返回最近一次布局后的命中表内容 (登记表按引用传入)
void renderOnce(const ftxui::Component& comp, int w = 60, int h = 12) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, comp->Render());
}

/// 取屏幕上某块矩形区域内的可见文字 (逐格拼接; 宽字符占位格为空串)
std::string screenTextIn(const ftxui::Screen& screen, const ftxui::Box& box) {
    std::string out;
    for (int y = box.y_min; y <= box.y_max; ++y) {
        for (int x = box.x_min; x <= box.x_max; ++x) {
            if (x < 0 || y < 0 || x >= screen.dimx() || y >= screen.dimy()) {
                continue;
            }
            out += screen.PixelAt(x, y).character;
        }
    }
    return out;
}

/// 渲染元素一帧 (无组件包装)
void renderElement(const ftxui::Element& el, int w = 60, int h = 12) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, el);
}

/// 渲染 [UiActionList] 一帧 (返回屏幕文本, 便于断言高亮/文本)
std::string renderActionList(
    const UiActionList&  list,
    UiHitMap&            hits,
    const UiActionStyle& style,
    int                  w = 40,
    int                  h = 12
) {
    hits.beginFrame();
    auto el     = list.render(hits, style);
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, el);
    return screen.ToString();
}

/// 命中表中指定 id 的屏幕区域 (不存在返回空区域)
ftxui::Box boxOf(const UiHitMap& hits, std::string_view id) {
    for (const auto& entry : hits.entries()) {
        if (entry.payload.id == id) {
            return *entry.box;
        }
    }
    return kNoBox;
}

/// 取命中表中指定 id 的屏幕中心点 (便于模拟点击)
bool centerOf(const UiHitMap& hits, std::string_view id, int& x, int& y) {
    const auto box = boxOf(hits, id);
    if (box.IsEmpty()) {
        return false;
    }
    x = (box.x_min + box.x_max) / 2;
    y = (box.y_min + box.y_max) / 2;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// UiHitRegistry
// ---------------------------------------------------------------------------

void test_hit_registry_frame_reset() {
    UiHitMap hits;

    // 帧首 (未登记): 任意坐标都不命中
    hits.beginFrame();
    XX_TEST_EXPECT_TRUE(hits.empty());
    XX_TEST_EXPECT_TRUE(hits.find(3, 3) == nullptr);
    XX_TEST_EXPECT_TRUE(hits.findClick(leftClickMouse(3, 3)) == nullptr);

    // 登记并布局: 命中框为元素区域
    hits.beginFrame();
    auto el = hits.add(ftxui::text("AAAA"), "a", std::string{});
    renderElement(el);
    XX_TEST_EXPECT_EQ(hits.size(), size_t{1});
    XX_TEST_EXPECT_TRUE(!boxOf(hits, "a").IsEmpty());

    // 下一帧帧首清空: 上一帧的登记不再命中 (未重新登记 = 不占区域)
    hits.beginFrame();
    XX_TEST_EXPECT_TRUE(hits.empty());
    XX_TEST_EXPECT_TRUE(hits.find(0, 0) == nullptr);
}

void test_hit_registry_unrendered_item_never_hits() {
    UiHitMap hits;
    hits.beginFrame();

    // 只登记不渲染 (元素被丢弃): Box 保持空区域, 不参与命中
    // (这是"未显示的按钮占那块区域"问题的根因 —— 登记表用帧首清空 + 空 Box 兜底)
    (void)hits.add(ftxui::text("hidden"), "hidden", std::string{});
    XX_TEST_EXPECT_TRUE(hits.size() == 1);
    XX_TEST_EXPECT_TRUE(hits.find(5, 0) == nullptr);
    XX_TEST_EXPECT_TRUE(boxOf(hits, "hidden").IsEmpty());
    XX_TEST_EXPECT_TRUE(hits.findClick(leftClickMouse(0, 0)) == nullptr);

    // 对照: 默认构造的 ftxui::Box 含 (0,0), 因此"无命中区域"必须用空区域表达,
    // 否则屏幕左上角会被当成命中 (回归防护)
    XX_TEST_EXPECT_TRUE(ftxui::Box{}.Contain(0, 0));
    XX_TEST_EXPECT_FALSE(kNoBox.Contain(0, 0));
}

void test_hit_registry_click_and_order() {
    UiHitMap hits;
    hits.beginFrame();

    // 内外两个区域: 内层先登记 => 重叠区域命中内层 (按登记顺序取首个匹配)
    auto inner = hits.add(ftxui::text("IN"), "inner", std::string{});
    auto outer = hits.add(ftxui::text("OUTER"), "outer", std::string{});
    renderElement(ftxui::vbox({std::move(inner), std::move(outer)}) | ftxui::border);

    int x = 0;
    int y = 0;
    XX_TEST_EXPECT_TRUE(centerOf(hits, "inner", x, y));
    const auto* hit = hits.findClick(leftClickMouse(x, y));
    XX_TEST_EXPECT_TRUE(hit != nullptr);
    if (hit != nullptr) {
        XX_TEST_EXPECT_EQ(hit->payload.id, std::string("inner"));
    }

    // 非左键 / 非释放动作不算点击
    XX_TEST_EXPECT_TRUE(
        hits.findClick(ftxui::Mouse{
            .button = ftxui::Mouse::Right,
            .motion = ftxui::Mouse::Released,
            .x      = x,
            .y      = y,
        })
        == nullptr
    );
    XX_TEST_EXPECT_TRUE(
        hits.findClick(ftxui::Mouse{
            .button = ftxui::Mouse::Left,
            .motion = ftxui::Mouse::Pressed,
            .x      = x,
            .y      = y,
        })
        == nullptr
    );
    XX_TEST_EXPECT_TRUE(hits.find(x, y) != nullptr); // 坐标查询仍可用 (拖拽等场景)
}

// ---------------------------------------------------------------------------
// UiActionList
// ---------------------------------------------------------------------------

namespace {

/// 条目样式 (测试统一取暗色主题按钮配色)
UiActionStyle testStyle() {
    return UiActionStyle::fromTheme(TUITheme::darkTheme());
}

/// 构造 3 项条目表 ("A"/"B"(禁用)/"C"), 激活记录到 activateList
std::vector<UiActionItem> makeActions(std::vector<std::string>& activateList) {
    auto record = [&activateList](const char* id) -> std::function<void()> {
        return [&activateList, id] {
            activateList.push_back(id);
        };
    };
    return {
        UiActionItem{.id = "a", .label = "A", .onActivate = record("a")},
        UiActionItem{.id = "b", .label = "B", .enabled = false, .onActivate = record("b")},
        UiActionItem{.id = "c", .label = "C", .onActivate = record("c")},
    };
}

} // namespace

void test_action_list_selection_keeps_by_id() {
    UiActionList             list;
    UiHitMap                 hits;
    std::vector<std::string> activated;

    list.setItems(makeActions(activated));
    XX_TEST_EXPECT_EQ(list.size(), size_t{3});
    XX_TEST_EXPECT_EQ(list.selectedIndex(), 0);

    // 键盘下移跳过不可用项: 0 -> 2 (第 1 项 disabled 被跳过)
    XX_TEST_EXPECT_TRUE(list.onKeyEvent(ftxui::Event::ArrowDown));
    XX_TEST_EXPECT_EQ(list.selectedIndex(), 2);
    // 已在末尾: 保持不动
    XX_TEST_EXPECT_TRUE(list.onKeyEvent(ftxui::Event::ArrowDown));
    XX_TEST_EXPECT_EQ(list.selectedIndex(), 2);
    // Home/End (落到首个/末个可用项)
    XX_TEST_EXPECT_TRUE(list.onKeyEvent(ftxui::Event::Home));
    XX_TEST_EXPECT_EQ(list.selectedIndex(), 0);
    XX_TEST_EXPECT_TRUE(list.onKeyEvent(ftxui::Event::End));
    XX_TEST_EXPECT_EQ(list.selectedIndex(), 2);

    // 条目表重建 (顺序变化): 选中项按 id 保持
    std::vector<UiActionItem> reordered;
    for (auto& item : makeActions(activated)) {
        reordered.push_back(item);
    }
    std::reverse(reordered.begin(), reordered.end()); // C, B, A
    list.setItems(std::move(reordered));
    XX_TEST_EXPECT_EQ(list.selectedIndex(), 0); // "c" 现在在第 0 项

    // 选中项消失: 下标收敛到有效范围
    list.setItems({
        UiActionItem{.id = "x", .label = "X"},
    });
    XX_TEST_EXPECT_EQ(list.selectedIndex(), 0);
}

void test_action_list_keyboard_activate() {
    UiActionList             list;
    UiHitMap                 hits;
    std::vector<std::string> activated;

    list.setItems(makeActions(activated));

    // Enter 激活选中项 (第 0 项)
    XX_TEST_EXPECT_TRUE(list.onKeyEvent(ftxui::Event::Return));
    XX_TEST_EXPECT_EQ(activated.size(), size_t{1});
    XX_TEST_EXPECT_EQ(activated[0], std::string("a"));

    // 移到末项后 Enter
    list.setSelectedIndex(2);
    XX_TEST_EXPECT_TRUE(list.onKeyEvent(ftxui::Event::Return));
    XX_TEST_EXPECT_EQ(activated.size(), size_t{2});
    XX_TEST_EXPECT_EQ(activated[1], std::string("c"));

    // 不可用项: activateIndex 直接调用不触发回调
    XX_TEST_EXPECT_FALSE(list.activateIndex(1));

    // 未消费的键 (Esc) 由调用方处理
    XX_TEST_EXPECT_FALSE(list.onKeyEvent(ftxui::Event::Escape));

    // 空列表: Enter 不崩溃且无可激活项
    UiActionList empty;
    empty.setItems({});
    XX_TEST_EXPECT_EQ(empty.selectedIndex(), -1);
    XX_TEST_EXPECT_TRUE(empty.onKeyEvent(ftxui::Event::Return));
    XX_TEST_EXPECT_FALSE(empty.activateIndex(0));
}

void test_action_list_mouse_click() {
    UiActionList             list;
    UiHitMap                 hits;
    UiActionStyle            style = testStyle();
    std::vector<std::string> activated;

    list.setItems(makeActions(activated));
    const std::string screen = renderActionList(list, hits, style);
    XX_TEST_EXPECT_TRUE(screen.find("A") != std::string::npos);
    XX_TEST_EXPECT_TRUE(screen.find("C") != std::string::npos);

    // 点击第 2 项 ("C"): 置选中 + 激活
    int x = 0;
    int y = 0;
    XX_TEST_EXPECT_TRUE(centerOf(hits, "c", x, y));
    XX_TEST_EXPECT_TRUE(list.onMouseEvent(leftClickMouse(x, y), hits));
    XX_TEST_EXPECT_EQ(list.selectedIndex(), 2);
    XX_TEST_EXPECT_EQ(activated.size(), size_t{1});
    XX_TEST_EXPECT_EQ(activated[0], std::string("c"));

    // 点击不可用项 ("B"): 不激活 (选中项也不改)
    XX_TEST_EXPECT_TRUE(centerOf(hits, "b", x, y));
    XX_TEST_EXPECT_FALSE(list.onMouseEvent(leftClickMouse(x, y), hits));
    XX_TEST_EXPECT_EQ(activated.size(), size_t{1});
    XX_TEST_EXPECT_EQ(list.selectedIndex(), 2);

    // 未命中任何条目: 不消费 (调用方仍可处理该点击)
    XX_TEST_EXPECT_FALSE(list.onMouseEvent(leftClickMouse(999, 999), hits));

    // 选中项整行高亮: 重新渲染后选中行 (C 在第 2 行) 覆盖整行背景色
    hits.beginFrame();
    auto el    = list.render(hits, style);
    auto probe = ftxui::Screen::Create(ftxui::Dimension::Fixed(30), ftxui::Dimension::Fixed(6));
    ftxui::Render(probe, el);
    XX_TEST_EXPECT_EQ(probe.CellAt(0, 2).background_color, style.selectedBg);
    XX_TEST_EXPECT_EQ(probe.CellAt(probe.dimx() - 1, 2).background_color, style.selectedBg);
    // 未选中行不着色 (保持父级背景而不是选中色)
    XX_TEST_EXPECT_TRUE(probe.CellAt(0, 0).background_color != style.selectedBg);
}

void test_action_list_selection_changed_callback() {
    UiActionList     list;
    std::vector<int> changes;

    list.setItems({
        UiActionItem{.id = "a", .label = "A"},
        UiActionItem{.id = "b", .label = "B"},
    });
    list.onSelectionChanged([&](int index) {
        changes.push_back(index);
    });

    list.onKeyEvent(ftxui::Event::ArrowDown);
    XX_TEST_EXPECT_EQ(changes.size(), size_t{1});
    XX_TEST_EXPECT_EQ(changes[0], 1);

    // 已在末尾 (无移动): 不重复通知
    list.onKeyEvent(ftxui::Event::ArrowDown);
    XX_TEST_EXPECT_EQ(changes.size(), size_t{1});

    // 点击命中即选中: 选中项变化同样通知
    UiHitMap hits;
    hits.beginFrame();
    auto el = list.render(hits, testStyle());
    renderElement(el, 30, 6);
    int x = 0;
    int y = 0;
    XX_TEST_EXPECT_TRUE(centerOf(hits, "a", x, y));
    list.onMouseEvent(leftClickMouse(x, y), hits);
    XX_TEST_EXPECT_EQ(changes.size(), size_t{2});
    XX_TEST_EXPECT_EQ(changes[1], 0);
}

/// 条目分组: 分组标题独占一行 (不占条目下标), 不可点击, 组间留白
void test_action_list_group_headers() {
    UiActionList             list;
    UiHitMap                 hits;
    std::vector<std::string> activated;

    auto record = [&activated](const char* id) -> std::function<void()> {
        return [&activated, id] {
            activated.push_back(id);
        };
    };
    list.setItems({
        UiActionItem{.id = "a", .label = "A", .group = "G1", .onActivate = record("a")},
        UiActionItem{.id = "b", .label = "B", .group = "G1", .onActivate = record("b")},
        UiActionItem{.id = "c", .label = "C", .group = "G2", .onActivate = record("c")},
        UiActionItem{.id = "d", .label = "D", .onActivate = record("d")},
    });

    const std::string screen = renderActionList(list, hits, testStyle(), 30, 10);
    // 每个分组标题只出现一次 (同组的多行条目共用一行标题)
    XX_TEST_EXPECT_TRUE(screen.find("G1") != std::string::npos);
    XX_TEST_EXPECT_TRUE(screen.find("G2") != std::string::npos);
    const size_t first = screen.find("G1");
    XX_TEST_EXPECT_EQ(screen.find("G1", first + 1), std::string::npos);

    // 标题行不登记命中: 点标题行不激活任何条目, 选中项也不变
    const ftxui::Box boxA = boxOf(hits, "a");
    XX_TEST_EXPECT_FALSE(boxA.IsEmpty());
    if (!boxA.IsEmpty()) {
        const int titleX = boxA.x_min;     // 标题行与条目同行起点
        const int titleY = boxA.y_min - 1; // 条目上一行即标题行
        XX_TEST_EXPECT_TRUE(hits.findClick(leftClickMouse(titleX, titleY)) == nullptr);
        XX_TEST_EXPECT_FALSE(list.onMouseEvent(leftClickMouse(titleX, titleY), hits)); // 未消费
        XX_TEST_EXPECT_EQ(list.selectedIndex(), 0);
        XX_TEST_EXPECT_TRUE(activated.empty());
    }

    // 键盘导航仍在条目之间移动: 标题行不占下标 (0 -> 1 -> 2 -> 3)
    XX_TEST_EXPECT_TRUE(list.onKeyEvent(ftxui::Event::ArrowDown));
    XX_TEST_EXPECT_EQ(list.selectedIndex(), 1);
    XX_TEST_EXPECT_TRUE(list.onKeyEvent(ftxui::Event::ArrowDown));
    XX_TEST_EXPECT_EQ(list.selectedIndex(), 2);
    XX_TEST_EXPECT_TRUE(list.onKeyEvent(ftxui::Event::ArrowDown));
    XX_TEST_EXPECT_EQ(list.selectedIndex(), 3);
    XX_TEST_EXPECT_TRUE(list.onKeyEvent(ftxui::Event::Return));
    XX_TEST_EXPECT_EQ(activated.size(), size_t{1});
    XX_TEST_EXPECT_EQ(activated[0], std::string("d"));
}

// ---------------------------------------------------------------------------
// 组件级: 隐藏的按钮不再占点击区域
// ---------------------------------------------------------------------------

void test_hidden_button_not_clickable() {
    TUISharedState sharedState;
    TUITheme       theme = TUITheme::darkTheme();
    TUICtx         ctx;
    ctx.state          = &sharedState;
    ctx.frameState     = sharedState.readSnapshot();
    ctx.postRedraw     = [] {};
    ctx.theme          = &theme;
    ctx.sessionId      = "s";
    ctx.viewportWidth  = 60;
    ctx.viewportHeight = 20;

    bool opened        = false;
    bool attachAllowed = true;

    InputComponent::Config cfg;
    cfg.canAttach = [&] {
        return attachAllowed;
    };
    cfg.onOpenAttachPicker = [&] {
        opened = true;
    };
    auto comp = std::make_shared<InputComponent>(ctx, std::move(cfg));

    // 1. 支持多模态: [ @︎ ] 按钮渲染并登记命中; 记录其屏幕位置
    renderOnce(comp);
    const ftxui::Box visibleBox = comp->attachButtonBox();
    XX_TEST_EXPECT_TRUE(!visibleBox.IsEmpty());
    int clickX = 0;
    int clickY = 0;
    XX_TEST_EXPECT_TRUE(!visibleBox.IsEmpty());
    clickX = (visibleBox.x_min + visibleBox.x_max) / 2;
    clickY = (visibleBox.y_min + visibleBox.y_max) / 2;

    comp->OnEvent(leftClickAt(clickX, clickY));
    XX_TEST_EXPECT_TRUE(opened);

    // 2. 模型不再支持多模态: 按钮不渲染 -> 命中框为空区域, 原位置点击无响应
    opened        = false;
    attachAllowed = false;
    renderOnce(comp);
    XX_TEST_EXPECT_TRUE(comp->attachButtonBox().IsEmpty());
    comp->OnEvent(leftClickAt(clickX, clickY));
    XX_TEST_EXPECT_FALSE(opened);

    // 3. 再次支持多模态: 按钮恢复可点 (帧首清空 + 重新登记)
    attachAllowed = true;
    renderOnce(comp);
    XX_TEST_EXPECT_TRUE(!comp->attachButtonBox().IsEmpty());
    const ftxui::Box restored = comp->attachButtonBox();
    comp->OnEvent(
        leftClickAt((restored.x_min + restored.x_max) / 2, (restored.y_min + restored.y_max) / 2)
    );
    XX_TEST_EXPECT_TRUE(opened);
}

void test_status_bar_click_actions() {
    TUISharedState sharedState;
    TUITheme       theme = TUITheme::darkTheme();
    TUICtx         ctx;
    ctx.state          = &sharedState;
    ctx.frameState     = sharedState.readSnapshot();
    ctx.postRedraw     = [] {};
    ctx.theme          = &theme;
    ctx.sessionId      = "s";
    ctx.viewportWidth  = 80;
    ctx.viewportHeight = 20;

    int  modelClicks    = 0;
    int  sessionClicks  = 0;
    int  settingsClicks = 0;
    auto comp           = std::make_shared<StatusBarComponent>(
        ctx,
        StatusBarComponent::Config{
                      .onModelClick =
                [&] {
                    ++modelClicks;
                },
                      .onSessionsClick =
                [&] {
                    ++sessionClicks;
                },
                      .onSettingsClick =
                [&] {
                    ++settingsClicks;
                },
        }
    );

    renderOnce(comp, 80, 3);

    // 三个可点区域均登记命中
    const ftxui::Box modelBox    = comp->hitBox(StatusBarComponent::kModelHitId);
    const ftxui::Box sessionsBox = comp->hitBox(StatusBarComponent::kSessionsHitId);
    const ftxui::Box settingsBox = comp->hitBox(StatusBarComponent::kSettingsHitId);
    XX_TEST_EXPECT_TRUE(!modelBox.IsEmpty());
    XX_TEST_EXPECT_TRUE(!sessionsBox.IsEmpty());
    XX_TEST_EXPECT_TRUE(!settingsBox.IsEmpty());

    // 快捷键提示须与实际按键绑定一致 (F3 = 会话选择弹窗, F4 = 设置弹窗,
    // 见 agent_tui.cpp 的全局快捷键处理), 且会话按钮排在设置左侧
    {
        auto screen
            = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(3));
        ftxui::Render(screen, comp->Render());
        XX_TEST_EXPECT_TRUE(screenTextIn(screen, sessionsBox).find("[F3]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(screenTextIn(screen, settingsBox).find("[F4]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(sessionsBox.x_min < settingsBox.x_min);
    }

    const auto click = [&](const ftxui::Box& box) {
        comp->OnEvent(leftClickAt((box.x_min + box.x_max) / 2, (box.y_min + box.y_max) / 2));
    };
    click(modelBox);
    click(sessionsBox);
    click(settingsBox);
    XX_TEST_EXPECT_EQ(modelClicks, 1);
    XX_TEST_EXPECT_EQ(sessionClicks, 1);
    XX_TEST_EXPECT_EQ(settingsClicks, 1);

    // ---- 插件状态栏项的富展示片段 (segments / sparkline / meter) ----
    {
        auto registry = std::make_shared<agentxx::plugin::ClientUiRegistry>();
        agentxx::plugin::ClientStatusItem rich;
        rich.plugin = "test_plugin";
        rich.id     = "test_plugin.rich";
        rich.text   = "降级文本";
        rich.align  = 0;
        rich.order  = 0;
        rich.rich   = utilxx_base::Json::parse(
            R"({"segments":[{"text":"CPU","color":"hint"},{"text":"55%","color":"accent"}],)"
            R"("sparkline":{"data":[1,3,2,5,4],"color":"accent"},)"
            R"("meter":{"value":55,"total":100,"width":8,"unit":"%"}})"
        );
        registry->statusItems.push_back(rich);

        agentxx::plugin::ClientStatusItem plain;
        plain.plugin = "test_plugin";
        plain.id     = "test_plugin.plain";
        plain.text   = "plain-item";
        plain.align  = 1;
        registry->statusItems.push_back(plain);

        auto bound = TUISharedState{};
        ctx.frameState = bound.readSnapshot();
        ctx.frameState->pluginRegistry = registry;

        auto screen
            = ftxui::Screen::Create(ftxui::Dimension::Fixed(100), ftxui::Dimension::Fixed(3));
        ftxui::Render(screen, comp->Render());
        const std::string text = screenTextIn(screen, ftxui::Box{0, 99, 0, 2});
        XX_TEST_EXPECT_TRUE(text.find("CPU") != std::string::npos);
        XX_TEST_EXPECT_TRUE(text.find("55%") != std::string::npos);
        XX_TEST_EXPECT_TRUE(text.find("plain-item") != std::string::npos);
        // 富片段渲染成功时不显示降级文本
        XX_TEST_EXPECT_TRUE(text.find("降级文本") == std::string::npos);
    }
    {
        // 无富片段时仍按纯文本渲染 (原行为不变)
        auto registry = std::make_shared<agentxx::plugin::ClientUiRegistry>();
        agentxx::plugin::ClientStatusItem plain;
        plain.plugin = "test_plugin";
        plain.id     = "test_plugin.text";
        plain.text   = "text-only";
        registry->statusItems.push_back(plain);
        auto bound = TUISharedState{};
        ctx.frameState = bound.readSnapshot();
        ctx.frameState->pluginRegistry = registry;

        auto screen
            = ftxui::Screen::Create(ftxui::Dimension::Fixed(100), ftxui::Dimension::Fixed(3));
        ftxui::Render(screen, comp->Render());
        XX_TEST_EXPECT_TRUE(screenTextIn(screen, ftxui::Box{0, 99, 0, 2}).find("text-only")
                            != std::string::npos);
    }

    // 键盘事件不被状态栏消费 (由外层集中处理 F2/F3/F4)
    XX_TEST_EXPECT_FALSE(comp->OnEvent(ftxui::Event::F2));
    // 未命中任何区域的鼠标事件同样不消费 (交给其它组件)
    XX_TEST_EXPECT_FALSE(comp->OnEvent(leftClickAt(0, 0)));
}

// ---------------------------------------------------------------------------
// 接入点: 插件面板 / Info 段落 (真实注册路径 → 共享组件层渲染)
// ---------------------------------------------------------------------------

/// 测试用 UI 适配器 (只声明必要能力)
class WidgetTestUiAdapter : public agentxx::plugin::PluginUiAdapter {
public:

    agentxx::plugin::InterfaceSet supportedInterfaces() const override {
        namespace pi = agentxx::plugin::plugin_interfaces;
        return {
            std::string{pi::ClientUi},
            std::string{pi::ClientPanel},
            std::string{pi::ClientInfoSection},
            std::string{pi::ClientComponents},
            std::string{pi::ClientLayout},
        };
    }
};

/// 测试用管理器 (暴露 createInstance 构造伪实例)
class WidgetTestManager : public agentxx::plugin::ClientPluginManager {
public:

    using ClientPluginManager::ClientPluginManager;
    using ClientPluginManager::createInstance;
};

/// 把滚动子项渲染到屏幕并逐格读取 (未写入的格补空格, 保证列位置可比对)
std::string renderScrollItems(const std::vector<ScrollItem>& rows, int w, int h) {
    ftxui::Elements els;
    for (const auto& row : rows) {
        if (row.element) {
            els.push_back(row.element);
        }
    }
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, ftxui::vbox(std::move(els)));
    std::string out;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::string& ch = screen.PixelAt(x, y).character;
            out += ch.empty() ? std::string{" "} : ch;
        }
        out += '\n';
    }
    return out;
}

/// 面板接入点: 新组件 (表格/计量条/横排/趋势图) 与控件经真实注册路径上屏,
/// 并上报可用尺寸 (插件据此重排; 见 reportRegionSize)
void test_panel_access_point_extended_components() {
    asio::io_context io;
    auto             mgr = std::make_shared<WidgetTestManager>(io.get_executor());
    mgr->setUiAdapter(std::make_shared<WidgetTestUiAdapter>());
    auto inst = mgr->createInstance("widget_probe");
    mgr->plugins_.emplace(inst->name, inst);

    auto* panel = static_cast<AgentxxPanel*>(mgr->registerPanel(
        inst.get(),
        std::string_view{"widget_probe.panel"},
        std::string_view{R"({"title":"Probe"})"}
    ));
    XX_TEST_EXPECT_TRUE(panel != nullptr);

    const std::string items = R"({"items":[
        {"kind":"table","header":true,
         "columns":[{"title":"Path","w":"flex"},{"title":"Scope","w":6}],
         "rows":[["a.txt","write"],["b.txt","read"]]},
        {"kind":"meter","value":72,"total":100,"width":4,"label":"CPU"},
        {"kind":"row","gap":1,"items":[{"kind":"text","text":"L"},
                                       {"kind":"sparkline","data":[1,5,9]}]}
    ]})";
    XX_TEST_EXPECT_EQ(mgr->updatePanel(inst.get(), panel, std::string_view{items}), 0);

    auto tui = std::make_shared<TUIClientAgentIO>(io.get_executor(), "session");
    tui->setPluginManager(mgr);
    tui->refreshRenderContext();

    const auto rows = tui->renderPluginPanel("widget_probe.panel");
    XX_TEST_EXPECT_EQ(rows.size(), size_t{3}); // 表格 1 行 + 计量条 + 横排
    const auto text = renderScrollItems(rows, 40, 6);
    XX_TEST_EXPECT_TRUE(text.find("Path") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("a.txt") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("write") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("CPU") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("72%") != std::string::npos); // 计量条数值文本
    XX_TEST_EXPECT_TRUE(text.find("L") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("▁") != std::string::npos);

    // 可用尺寸上报 (无侧边栏时按缺省宽度 40; 高度 = 内容行数)
    bool found = false;
    for (const auto& r : mgr->regionSizes()) {
        if (r.id == "widget_probe.panel") {
            found = true;
            XX_TEST_EXPECT_EQ(r.width, 40);
            XX_TEST_EXPECT_EQ(r.height, 6); // 内容行数 (表格 4 行 + 计量条 + 横排)
        }
    }
    XX_TEST_EXPECT_TRUE(found);

    // 内容更新: 新的描述立即反映到下一次渲染 (版本号递增, 供缓存 key 使用)
    const std::string updated = R"({"items":[{"kind":"text","text":"second"}]})";
    XX_TEST_EXPECT_EQ(mgr->updatePanel(inst.get(), panel, std::string_view{updated}), 0);
    const auto rows2 = tui->renderPluginPanel("widget_probe.panel");
    const auto text2 = renderScrollItems(rows2, 40, 3);
    XX_TEST_EXPECT_TRUE(text2.find("second") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text2.find("a.txt") == std::string::npos);
}

/// Info 接入点: 插件 Info 段落里的新组件同样上屏 (与面板共用渲染实现)
void test_info_access_point_extended_components() {
    asio::io_context io;
    auto             mgr = std::make_shared<WidgetTestManager>(io.get_executor());
    mgr->setUiAdapter(std::make_shared<WidgetTestUiAdapter>());
    auto inst = mgr->createInstance("info_probe");
    mgr->plugins_.emplace(inst->name, inst);

    auto* section = static_cast<AgentxxInfoSection*>(mgr->registerInfoSection(
        inst.get(),
        std::string_view{"info_probe.section"},
        std::string_view{R"({"title":"Probe Info"})"}
    ));
    XX_TEST_EXPECT_TRUE(section != nullptr);
    const std::string items = R"({"items":[
        {"kind":"kv","items":[{"k":"Model","v":"gpt-x"}]},
        {"kind":"box","title":"Limits","border":"round","items":[{"kind":"text","text":"inside box"}]}
    ]})";
    XX_TEST_EXPECT_EQ(mgr->updateInfoSection(inst.get(), section, std::string_view{items}), 0);

    auto tui = std::make_shared<TUIClientAgentIO>(io.get_executor(), "session");
    tui->setPluginManager(mgr);
    tui->refreshRenderContext();

    const auto rows = tui->renderInfoSidebar();
    XX_TEST_EXPECT_TRUE(!rows.empty());
    const auto text = renderScrollItems(rows, 40, static_cast<int>(rows.size()) + 2);
    XX_TEST_EXPECT_TRUE(text.find("Probe Info") != std::string::npos); // 段落标题
    XX_TEST_EXPECT_TRUE(text.find("Model") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("gpt-x") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("inside box") != std::string::npos);

    // 段落可用尺寸上报 (与面板同机制)
    bool found = false;
    for (const auto& r : mgr->regionSizes()) {
        if (r.id == "info_probe.section") {
            found = true;
            XX_TEST_EXPECT_EQ(r.width, 40);
            XX_TEST_EXPECT_EQ(r.height, 4); // 内容行数 (kv 1 行 + 分组框 3 行)
        }
    }
    XX_TEST_EXPECT_TRUE(found);
}

TestResult testTuiWidget() {
    g_tui_widget_passed = 0;
    g_tui_widget_failed = 0;

    // 恢复默认语言 (其余 TUI 测试模块会切换界面语言)
    TUISettings::instance().setLanguage(TuiLanguage::ZhCn);

    test_hit_registry_frame_reset();
    test_hit_registry_unrendered_item_never_hits();
    test_hit_registry_click_and_order();
    test_action_list_selection_keeps_by_id();
    test_action_list_keyboard_activate();
    test_action_list_mouse_click();
    test_action_list_selection_changed_callback();
    test_action_list_group_headers();
    test_hidden_button_not_clickable();
    test_status_bar_click_actions();
    test_panel_access_point_extended_components();
    test_info_access_point_extended_components();

    return TestResult{g_tui_widget_passed, g_tui_widget_failed};
}

} // namespace test
} // namespace agentxx
