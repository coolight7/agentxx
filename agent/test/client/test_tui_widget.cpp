// TUI 交互框架测试 (离屏渲染 + 直接驱动 OnEvent)
//
// 覆盖场景:
// - 命中登记表: 帧首清空 / 渲染时登记 / 未布局项不命中 / 登记顺序优先
// - 声明式条目列表: 选中项维护 (按 id 跨重建保持)、键盘导航 (跳过不可用项)、
//   Enter 激活、鼠标点击命中 (置选中并激活)
// - 隐藏按钮不占点击区域: [📎︎︎] 按钮隐藏后, 其在上一帧的屏幕位置不再可点
#include "test_tui_widget.h"

#include "agentxx-client/io/tui/components/input_bar.h"
#include "agentxx-client/io/tui/components/status_bar.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/framework/ui_action_list.h"
#include "agentxx-client/io/tui/framework/ui_hit.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
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
    XX_TEST_EXPECT_TRUE(hits.findClick(ftxui::Mouse{
                             .button = ftxui::Mouse::Right,
                             .motion = ftxui::Mouse::Released,
                             .x      = x,
                             .y      = y,
                         })
                        == nullptr);
    XX_TEST_EXPECT_TRUE(hits.findClick(ftxui::Mouse{
                             .button = ftxui::Mouse::Left,
                             .motion = ftxui::Mouse::Pressed,
                             .x      = x,
                             .y      = y,
                         })
                        == nullptr);
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
    UiActionList  list;
    UiHitMap      hits;
    UiActionStyle            style = testStyle();
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
    UiActionList  list;
    UiHitMap      hits;
    UiActionStyle            style = testStyle();
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
    UiActionList  list;
    UiHitMap      hits;
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

    bool opened = false;
    bool attachAllowed = true;

    InputComponent::Config cfg;
    cfg.canAttach = [&] {
        return attachAllowed;
    };
    cfg.onOpenAttachPicker = [&] {
        opened = true;
    };
    auto comp = std::make_shared<InputComponent>(ctx, std::move(cfg));

    // 1. 支持多模态: [📎︎︎] 按钮渲染并登记命中; 记录其屏幕位置
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
    opened       = false;
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
    comp->OnEvent(leftClickAt((restored.x_min + restored.x_max) / 2, (restored.y_min + restored.y_max) / 2));
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
            .onModelClick = [&] {
                ++modelClicks;
            },
            .onSessionsClick = [&] {
                ++sessionClicks;
            },
            .onSettingsClick = [&] {
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

    const auto click = [&](const ftxui::Box& box) {
        comp->OnEvent(leftClickAt((box.x_min + box.x_max) / 2, (box.y_min + box.y_max) / 2));
    };
    click(modelBox);
    click(sessionsBox);
    click(settingsBox);
    XX_TEST_EXPECT_EQ(modelClicks, 1);
    XX_TEST_EXPECT_EQ(sessionClicks, 1);
    XX_TEST_EXPECT_EQ(settingsClicks, 1);

    // 键盘事件不被状态栏消费 (由外层集中处理 F2/F3/F4)
    XX_TEST_EXPECT_FALSE(comp->OnEvent(ftxui::Event::F2));
    // 未命中任何区域的鼠标事件同样不消费 (交给其它组件)
    XX_TEST_EXPECT_FALSE(comp->OnEvent(leftClickAt(0, 0)));
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
    test_hidden_button_not_clickable();
    test_status_bar_click_actions();

    return TestResult{g_tui_widget_passed, g_tui_widget_failed};
}

} // namespace test
} // namespace agentxx
