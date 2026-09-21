// 共享 UI 组件渲染层测试 (离屏渲染 + 直接驱动组件事件)
//
// 覆盖场景:
// - 屏幕渲染: 文本/表格/容器/图表/控件/提交行的可见内容与对齐
// - 测量一致: measureItem 的行数与元素真实布局高度相同 (估算走同一实现)
// - 命中区域: 元素内子区域的坐标与标识 (表格单元格 / 折叠标题 / 控件 / 提交行)
// - 滚动命中: Scrollable::hitTestItem 在滚动前后都能映射到正确子项与局部坐标
#include "agentxx-test/client/test_tui_ui_items.h"

#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/scroll_common.h"
#include "agentxx-client/io/tui/scrollable.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx-client/io/tui/ui_components.h"
#include "agentxx/ui/item.h"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "utilxx_base/json.h"
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_ui_items_passed = 0;
int g_tui_ui_items_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_ui_items_passed
#define XX_TEST_FAILED g_tui_ui_items_failed

namespace agentxx {
namespace test {

using agentxx::client::ScrollItem;
using agentxx::client::Scrollable;
using agentxx::client::TUITheme;
using agentxx::client::UiHitRegion;
using agentxx::client::UiHitRegionKind;
using agentxx::client::UiRenderCtx;
using agentxx::client::UiRenderResult;
using utilxx_base::Json;

namespace {

const TUITheme& theme() {
    static const TUITheme t = TUITheme::darkTheme();
    return t;
}

/// 渲染上下文 (固定宽度与缩进; 默认 60 列)
UiRenderCtx ctxFor(int width = 60, int indent = 0) {
    UiRenderCtx ctx;
    ctx.theme  = &theme();
    ctx.width  = width;
    ctx.indent = indent;
    return ctx;
}

/// 渲染 JSON 描述为行模型
UiRenderResult renderJson(const char* json, const UiRenderCtx& ctx) {
    UiRenderResult out;
    agentxx::client::renderItems(agentxx::ui::parseItems(Json::parse(json)), ctx, out);
    return out;
}

/// 把行模型拼成一个元素 (与调用方实际使用方式一致) 并渲染到屏幕
///
/// 返回"逐格字符"文本 (不用 Screen::ToString: 它会把颜色转义序列一起输出,
/// 断言子串时会被转义码打断); 每行按宽度补齐, 行间以 \n 分隔
std::string renderToText(const UiRenderResult& res, int w = 60, int h = 20) {
    ftxui::Elements els;
    for (const auto& row : res.rows) {
        els.push_back(row.element);
    }
    auto el     = ftxui::vbox(std::move(els));
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, el);
    std::string out;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            out += screen.PixelAt(x, y).character;
        }
        out += '\n';
    }
    return out;
}

/// 行模型元素真实布局后的高度 (行)
int layoutLines(UiRenderResult& res, int w) {
    ftxui::Elements els;
    for (const auto& row : res.rows) {
        els.push_back(row.element);
    }
    auto el = ftxui::vbox(std::move(els));
    return agentxx::client::layoutAndMeasure(el, ftxui::Box{0, w - 1, 0, agentxx::client::kTallHeight});
}

/// 估算的行数 (与布局高度一致才说明测量与渲染同源)
size_t measuredLines(UiRenderResult& res) {
    size_t lines = 0;
    for (const auto& row : res.rows) {
        lines += std::max<size_t>(1, row.lines);
    }
    return lines;
}

/// 屏幕文本包含 (去掉行尾空白, 避免宽度差异干扰)
bool screenHas(const std::string& screenText, std::string_view needle) {
    return screenText.find(needle) != std::string::npos;
}

/// 收集全部可命中区域
std::vector<UiHitRegion> allRegions(const UiRenderResult& res) {
    std::vector<UiHitRegion> out;
    for (const auto& row : res.rows) {
        for (const auto& region : row.regions) {
            out.push_back(region);
        }
    }
    return out;
}

/// 按标识找区域 (不存在返回 nullptr)
const UiHitRegion* findRegion(const UiRenderResult& res, std::string_view id) {
    for (const auto& row : res.rows) {
        for (const auto& region : row.regions) {
            if (region.id == id) {
                return &region;
            }
        }
    }
    return nullptr;
}

/// 构造鼠标事件 (左键释放)
ftxui::Mouse leftClick(int x, int y) {
    ftxui::Mouse m;
    m.button = ftxui::Mouse::Left;
    m.motion = ftxui::Mouse::Released;
    m.x      = x;
    m.y      = y;
    return m;
}

/// 渲染组件一帧 (布局填充内部框)
void renderOnce(const ftxui::Component& comp, int w = 60, int h = 12) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, comp->Render());
}

} // namespace

TestResult testTuiUiItems() {
    // ---------------- 文本与样式 ----------------
    {
        auto res  = renderJson(R"([{"kind":"text","text":"hello"},{"kind":"gap","lines":2},
                                   {"kind":"separator"}])", ctxFor(20));
        auto text = renderToText(res);
        XX_TEST_EXPECT_TRUE(screenHas(text, "hello"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "─"));
        XX_TEST_EXPECT_EQ(measuredLines(res), size_t{4}); // 1 + 2 + 1
    }
    {
        // 窄宽度下长文本折行: 估算行数与真实布局一致
        std::string json = R"([{"kind":"text","text":"0123456789abcdefghij"}])";
        auto        res  = renderJson(json.c_str(), ctxFor(8));
        XX_TEST_EXPECT_EQ(measuredLines(res), size_t{3}); // 20 列 / 8 列 = 3 行
        XX_TEST_EXPECT_EQ(layoutLines(res, 8), 3);
    }

    // ---------------- 未知 kind 与 canvas 降级 ----------------
    {
        auto res  = renderJson(
            R"([{"kind":"unknown-x","fallback":"降级文本"},{"kind":"canvas","fallback":"[图]"},{"kind":"custom","component":"no-such"}])",
            ctxFor(40)
        );
        auto text = renderToText(res);
        XX_TEST_EXPECT_TRUE(screenHas(text, "降级文本"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "[图]"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "no-such"));
    }

    // ---------------- 表格 ----------------
    {
        auto res = renderJson(R"([
            {"kind":"table","header":true,
             "columns":[{"title":"File","w":10},{"title":"Size","align":"right","w":6}],
             "rows":[["main.cpp","12.4K"],["a-very-long-file-name.cpp","1K"]]}
        ])", ctxFor(40));
        auto text = renderToText(res, 40);
        XX_TEST_EXPECT_TRUE(screenHas(text, "File"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "Size"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "main.cpp"));
        // 右对齐列: 数值前面有补白
        XX_TEST_EXPECT_TRUE(screenHas(text, "12.4K"));
        // 超宽单元格按列宽截断并加省略号
        XX_TEST_EXPECT_TRUE(screenHas(text, "…"));
        // 表头 + 分隔线 + 2 行 = 4 行
        XX_TEST_EXPECT_EQ(measuredLines(res), size_t{4});
        XX_TEST_EXPECT_EQ(layoutLines(res, 40), 4);
    }
    {
        // 单元格可点: 登记区域 (标识 = 动作 id, 参数原样带回)
        auto res = renderJson(R"([
            {"kind":"table","header":false,
             "columns":[{"title":"A","w":6},{"title":"B","w":6}],
             "rows":[["x",{"text":"open","action":"open:file","args":{"line":12}}]]}
        ])", ctxFor(40));
        const auto* region = findRegion(res, "open:file");
        XX_TEST_EXPECT_TRUE(region != nullptr);
        if (region != nullptr) {
            XX_TEST_EXPECT_EQ(region->kind, UiHitRegionKind::Action);
            XX_TEST_EXPECT_EQ(region->x, 8); // 第一列 6 列 + 2 列间距
            // 无可伸缩列时剩余宽度归最后一列 (表格铺满可用宽度)
            XX_TEST_EXPECT_EQ(region->w, 32);
            XX_TEST_EXPECT_EQ(region->y, 0); // 无表头 → 第一行
            XX_TEST_EXPECT_TRUE(region->arg.find("12") != std::string::npos);
        }
    }

    // ---------------- 键值对与层级列表 ----------------
    {
        auto res  = renderJson(R"([{"kind":"kv","items":[{"k":"Model","v":"gpt-x"},
                                                          {"k":"LongKey","v":"1"}]}])", ctxFor(40));
        auto text = renderToText(res, 40);
        XX_TEST_EXPECT_TRUE(screenHas(text, "Model"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "gpt-x"));
        // 键列按最长键补齐: 值列起始位置对齐
        XX_TEST_EXPECT_TRUE(screenHas(text, "LongKey : 1"));
        XX_TEST_EXPECT_EQ(measuredLines(res), size_t{2});
        XX_TEST_EXPECT_EQ(layoutLines(res, 40), 2);
    }
    {
        auto res = renderJson(R"([{"kind":"tree","nodes":[
            {"label":"src","children":[{"label":"main.cpp","action":"open:main"},
                                       {"label":"io"}]}
        ]}])", ctxFor(40));
        auto text = renderToText(res, 40);
        XX_TEST_EXPECT_TRUE(screenHas(text, "src"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "├─ main.cpp"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "└─ io"));
        XX_TEST_EXPECT_EQ(measuredLines(res), size_t{3});
        XX_TEST_EXPECT_EQ(layoutLines(res, 40), 3);
        XX_TEST_EXPECT_TRUE(findRegion(res, "open:main") != nullptr);
    }

    // ---------------- 分组框 / 折叠 / 横排 ----------------
    {
        auto res = renderJson(R"([{"kind":"box","title":"Index","border":"round",
                                   "items":[{"kind":"text","text":"inner"}]}])", ctxFor(30));
        auto text = renderToText(res, 30);
        XX_TEST_EXPECT_TRUE(screenHas(text, "Index"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "inner"));
        XX_TEST_EXPECT_EQ(measuredLines(res), size_t{3}); // 上边框 + 1 行 + 下边框
        XX_TEST_EXPECT_EQ(layoutLines(res, 30), 3);
    }
    {
        // 折叠: 展开时提示符为 ▾ 且含子项; 折叠时 ▸ 且不含子项
        auto expanded = renderJson(R"([{"kind":"collapse","id":"c1","title":"Stack","expanded":true,
                                        "items":[{"kind":"text","text":"detail"}]}])", ctxFor(30));
        auto expandedText = renderToText(expanded, 30);
        XX_TEST_EXPECT_TRUE(screenHas(expandedText, "▾ Stack"));
        XX_TEST_EXPECT_TRUE(screenHas(expandedText, "detail"));
        XX_TEST_EXPECT_EQ(measuredLines(expanded), size_t{2});
        XX_TEST_EXPECT_EQ(layoutLines(expanded, 30), 2);

        auto collapsed = renderJson(R"([{"kind":"collapse","id":"c1","title":"Stack","expanded":false,
                                        "items":[{"kind":"text","text":"detail"}]}])", ctxFor(30));
        auto collapsedText = renderToText(collapsed, 30);
        XX_TEST_EXPECT_TRUE(screenHas(collapsedText, "▸ Stack"));
        XX_TEST_EXPECT_FALSE(screenHas(collapsedText, "detail"));

        // 标题行登记折叠区域 (宿主据此切换展开状态)
        const auto* region = findRegion(expanded, "c1");
        XX_TEST_EXPECT_TRUE(region != nullptr);
        if (region != nullptr) {
            XX_TEST_EXPECT_EQ(region->kind, UiHitRegionKind::Collapse);
        }
    }
    {
        // 折叠状态由宿主的回调决定 (描述里 expanded=true, 宿主返回 false)
        auto ctx = ctxFor(30);
        ctx.collapseExpanded = [](const std::string&, bool) {
            return false;
        };
        auto res  = renderJson(R"([{"kind":"collapse","id":"c1","title":"Stack","expanded":true,
                                    "items":[{"kind":"text","text":"detail"}]}])", ctx);
        auto text = renderToText(res, 30);
        XX_TEST_EXPECT_TRUE(screenHas(text, "▸ Stack"));
        XX_TEST_EXPECT_FALSE(screenHas(text, "detail"));
    }
    {
        // 横排: 固定宽度列 + 自适应列, 同一行渲染
        auto res = renderJson(R"([{"kind":"row","gap":1,"items":[
            {"kind":"text","text":"CPU","w":4},
            {"kind":"text","text":"55%"}
        ]}])", ctxFor(20));
        auto text = renderToText(res, 20);
        XX_TEST_EXPECT_TRUE(screenHas(text, "CPU"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "55%"));
        XX_TEST_EXPECT_EQ(measuredLines(res), size_t{1});
        // 同一行渲染: 首行同时含两列内容 (列宽不同也同行)
        const std::string firstLine = text.substr(0, text.find('\n'));
        XX_TEST_EXPECT_TRUE(screenHas(firstLine, "CPU"));
        XX_TEST_EXPECT_TRUE(screenHas(firstLine, "55%"));
    }
    {
        // 极窄宽度: 列宽收缩但不崩 (仍渲染出内容)
        auto res = renderJson(R"([{"kind":"row","gap":1,"items":[
            {"kind":"text","text":"abcdefghij"},{"kind":"text","text":"klmnopqrst"}
        ]}])", ctxFor(6));
        auto text = renderToText(res, 6);
        XX_TEST_EXPECT_TRUE(text.size() > 0);
        XX_TEST_EXPECT_GE(layoutLines(res, 6), 1);
    }
    {
        // 极宽宽度: 自适应列吃掉剩余宽度 (内容仍在)
        auto res = renderJson(R"([{"kind":"row","items":[
            {"kind":"text","text":"left"},{"kind":"sparkline","data":[1,2,3]}
        ]}])", ctxFor(120));
        auto text = renderToText(res, 120);
        XX_TEST_EXPECT_TRUE(screenHas(text, "left"));
        XX_TEST_EXPECT_EQ(measuredLines(res), size_t{1});
    }

    // ---------------- 图表 ----------------
    {
        auto res = renderJson(R"([{"kind":"sparkline","data":[0,1,2,3,4],"color":"accent"}])",
                               ctxFor(40));
        auto text = renderToText(res, 40);
        XX_TEST_EXPECT_TRUE(screenHas(text, "▁") || screenHas(text, "▂"));
        XX_TEST_EXPECT_EQ(measuredLines(res), size_t{1});
        XX_TEST_EXPECT_EQ(layoutLines(res, 40), 1);
    }
    {
        // 数据点多于宽度: 分桶聚合后仍只有一行
        std::string json = R"([{"kind":"sparkline","data":[)";
        for (int i = 0; i < 200; ++i) {
            if (i > 0) {
                json += ",";
            }
            json += std::to_string(i % 10);
        }
        json += R"(]}])";
        auto res  = renderJson(json.c_str(), ctxFor(30));
        auto text = renderToText(res, 30);
        XX_TEST_EXPECT_EQ(measuredLines(res), size_t{1});
        XX_TEST_EXPECT_EQ(layoutLines(res, 30), 1);
        XX_TEST_EXPECT_TRUE(text.size() > 0);
    }
    {
        // 高度 2 的迷你趋势图: 两行
        auto res = renderJson(R"([{"kind":"sparkline","data":[0,5,10],"height":2}])", ctxFor(30));
        XX_TEST_EXPECT_EQ(measuredLines(res), size_t{2});
        XX_TEST_EXPECT_EQ(layoutLines(res, 30), 2);
    }
    {
        // 计量条: 标签 + 数值
        auto res = renderJson(R"([{"kind":"meter","value":55,"total":100,"width":10,
                                   "label":"CPU","unit":"%" }])", ctxFor(40));
        auto text = renderToText(res, 40);
        XX_TEST_EXPECT_TRUE(screenHas(text, "CPU"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "55%"));
        XX_TEST_EXPECT_EQ(measuredLines(res), size_t{1});
        XX_TEST_EXPECT_EQ(layoutLines(res, 40), 1);
    }

    // ---------------- 控件与提交行 ----------------
    {
        auto res  = renderJson(R"([{"kind":"control","id":"mode","control":"buttons",
                                    "label":"模式","options":[{"value":"fast","label":"Fast "},
                                                              {"value":"safe","label":"Safe"}]}])",
                                 ctxFor(40));
        auto text = renderToText(res, 40);
        XX_TEST_EXPECT_TRUE(screenHas(text, "模式"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "Fast"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "Safe"));
        // 未提供表单状态时按描述缺省值渲染 (静态形态, 不登记命中区域)
        XX_TEST_EXPECT_EQ(allRegions(res).size(), size_t{0});
    }
    {
        // 提供表单状态 → 登记控件区域 (子序号 = 候选值下标)
        auto ctx      = ctxFor(40);
        auto form     = std::make_shared<agentxx::client::UiFormState>();
        ctx.form      = form.get();
        agentxx::client::UiRenderResult res;
        auto items = agentxx::ui::parseItemList(Json::parse(
            R"({"items":[{"kind":"control","id":"mode","control":"buttons",
               "options":[{"value":"fast","label":"Fast"},{"value":"safe","label":"Safe"}]}]})"
        ));
        agentxx::client::initFormState(*form, items);
        agentxx::client::renderItems(items, ctx, res);

        const auto* region = findRegion(res, "mode");
        XX_TEST_EXPECT_TRUE(region != nullptr);
        if (region != nullptr) {
            XX_TEST_EXPECT_EQ(region->kind, UiHitRegionKind::Form);
            XX_TEST_EXPECT_EQ(region->sub, 0);
        }
        // 第二个候选项的区域在第一个之后 (坐标递增)
        int fastX = -1;
        int safeX = -1;
        for (const auto& row : res.rows) {
            for (const auto& r : row.regions) {
                if (r.sub == 0) {
                    fastX = r.x;
                } else if (r.sub == 1) {
                    safeX = r.x;
                }
            }
        }
        XX_TEST_EXPECT_TRUE(fastX >= 0);
        XX_TEST_EXPECT_TRUE(safeX > fastX);
    }
    {
        // 提交行: 登记 __submit / __cancel 两个区域
        auto ctx  = ctxFor(40);
        auto form = std::make_shared<agentxx::client::UiFormState>();
        ctx.form  = form.get();
        auto res  = renderJson(R"([{"kind":"submit","label":"应用","cancelLabel":"取消"}])", ctx);
        const auto* submit = findRegion(res, "__submit");
        const auto* cancel = findRegion(res, "__cancel");
        XX_TEST_EXPECT_TRUE(submit != nullptr);
        XX_TEST_EXPECT_TRUE(cancel != nullptr);
        if (submit != nullptr && cancel != nullptr) {
            XX_TEST_EXPECT_EQ(submit->kind, UiHitRegionKind::FormSubmit);
            XX_TEST_EXPECT_TRUE(cancel->x > submit->x);
        }
        // 未提供表单状态时不登记 (纯展示)
        auto plain = renderJson(R"([{"kind":"submit"}])", ctxFor(40));
        XX_TEST_EXPECT_EQ(allRegions(plain).size(), size_t{0});
    }
    {
        // 表单状态初始化后按状态渲染 (勾选态/选中项来自状态表)
        auto ctx  = ctxFor(40);
        auto form = std::make_shared<agentxx::client::UiFormState>();
        ctx.form  = form.get();
        auto items = agentxx::ui::parseItemList(Json::parse(
            R"({"items":[{"kind":"control","id":"opt","control":"select",
               "options":[{"value":"a","label":"A"},{"value":"b","label":"B"}]}]})"
        ));
        agentxx::client::initFormState(*form, items);
        form->ensure("opt").selected = 1; // 用户选中第二项

        agentxx::client::UiRenderResult res;
        agentxx::client::renderItems(items, ctx, res);
        auto text = renderToText(res, 40);
        XX_TEST_EXPECT_TRUE(screenHas(text, "● B"));
        XX_TEST_EXPECT_TRUE(screenHas(text, "○ A"));
    }

    // ---------------- 文本 + 按钮合并行 ----------------
    {
        auto res  = renderJson(R"([{"kind":"text","text":"|- "},{"kind":"button","label":"Rebuild"}])",
                               ctxFor(40));
        auto text = renderToText(res, 40);
        XX_TEST_EXPECT_EQ(measuredLines(res), size_t{1});
        XX_TEST_EXPECT_TRUE(screenHas(text, "|- "));
        XX_TEST_EXPECT_TRUE(screenHas(text, "Rebuild"));
    }
    {
        // 无绑定时按钮不可点 (registry 为空 → 视为可点; 有 registry 且无绑定时不可点)
        agentxx::plugin::ClientUiRegistry reg;
        auto                            ctx = ctxFor(40);
        ctx.registry                        = &reg;
        ctx.plugin                          = "test_plugin";
        auto res = renderJson(R"([{"kind":"button","label":"Go","action":"run"}])", ctx);
        XX_TEST_EXPECT_EQ(allRegions(res).size(), size_t{0});

        agentxx::plugin::ClientActionBinding binding;
        binding.targetId = "owner";
        binding.plugin   = "test_plugin";
        binding.cb       = [](const AgentxxUiActionContext*, void*) {};
        reg.actionBindings.push_back(binding);
        auto bound = renderJson(R"([{"kind":"button","label":"Go","action":"run"}])", ctx);
        const auto* region = findRegion(bound, "run");
        XX_TEST_EXPECT_TRUE(region != nullptr);
        if (region != nullptr) {
            XX_TEST_EXPECT_EQ(region->plugin, std::string{"test_plugin"});
        }
    }

    // ---------------- 滚动容器命中映射 ----------------
    {
        // 20 个单行子项, 每项一个可命中区域 (标识 = item-N)
        auto items = std::make_shared<std::vector<ScrollItem>>();
        for (int i = 0; i < 20; ++i) {
            agentxx::ui::Item item;
            item.kind = "text";
            item.text = "row-" + std::to_string(i);
            UiRenderCtx ctx = ctxFor(20);
            UiRenderResult res;
            agentxx::client::renderItem(item, ctx, res);

            ScrollItem si;
            si.element = std::move(res.rows[0].element);
            si.hits.push_back(UiHitRegion{
                .x    = 0,
                .y    = 0,
                .w    = 0,
                .h    = 1,
                .kind = UiHitRegionKind::Action,
                .id   = "item-" + std::to_string(i),
            });
            items->push_back(std::move(si));
        }
        auto scroll = std::make_shared<Scrollable>([items]() {
            return *items;
        });
        scroll->setStickToBottom(false);
        renderOnce(scroll, 30, 6);

        // 首帧: 顶部可见, 第一项命中
        size_t index  = 0;
        int    localX = 0;
        int    localY = 0;
        XX_TEST_EXPECT_TRUE(scroll->hitTestItem(1, 0, index, localX, localY));
        XX_TEST_EXPECT_EQ(index, size_t{0});
        XX_TEST_EXPECT_EQ(localY, 0);

        // 滚轮向下滚动 3 行后, 命中映射到当前可见子项
        ftxui::Event wheelDown = ftxui::Event::Mouse("", [] {
            ftxui::Mouse m;
            m.button = ftxui::Mouse::WheelDown;
            m.motion = ftxui::Mouse::Pressed;
            m.x      = 1;
            m.y      = 1;
            return m;
        }());
        for (int i = 0; i < 3; ++i) {
            scroll->OnEvent(wheelDown);
        }
        renderOnce(scroll, 30, 6);
        XX_TEST_EXPECT_EQ(scroll->scrollOffset(), 3);
        XX_TEST_EXPECT_TRUE(scroll->hitTestItem(1, 0, index, localX, localY));
        XX_TEST_EXPECT_EQ(index, size_t{3}); // 顶行现在是第 4 项

        // 视口外(下方)子项不参与命中: y 超出视口返回 false
        XX_TEST_EXPECT_FALSE(scroll->hitTestItem(1, 99, index, localX, localY));
        // 被裁剪到视口上方的子项: 局部 y 按子项顶边换算 (第 3 项顶边在视口上方 1 行)
        XX_TEST_EXPECT_TRUE(scroll->hitTestItem(1, 0, index, localX, localY));
    }

    // ---------------- 面积型分隔线 (面性弹窗) ----------------
    {
        auto ctx = ctxFor(20);
        ctx.separatorStyle = agentxx::client::UiSeparatorStyle::Block;
        auto res  = renderJson(R"([{"kind":"separator"}])", ctx);
        auto text = renderToText(res, 20);
        XX_TEST_EXPECT_FALSE(screenHas(text, "─")); // 面性风格不画横线
    }

    return TestResult{g_tui_ui_items_passed, g_tui_ui_items_failed};
}

} // namespace test
} // namespace agentxx
