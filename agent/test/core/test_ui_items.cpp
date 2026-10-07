// 中断描述 ↔ 组件项 桥接测试 (界面描述层的 agentxx 侧用法)
//
// 描述层本身 (解析/上限/序列化/纯文本/适配) 的用例在库仓库 (cxx_pluginxx_ui) 的
// 单测里; 本模块覆盖 agentxx 这一侧的用法:
// - 中断块 → 组件项 (itemOf): 具名块、扩展组件 (按 raw 解析)、控件、提交行、未知块
// - 组件项 → 中断块 (blockOf) 与 blocksOf 的往返 (内容字段不丢)
// - 预设模板生成的结构 (输入表单 / 确认卡片 / 权限卡片: 控件 id、候选项值、块种类)
// - 纯文本降级: 行式前端 (CLI/日志/FFI) 上内容与空行都不丢
// - 显示列宽辅助 (终端渲染与文本降级共用同一套算法)
#include "agentxx-test/core/test_ui_items.h"

#include "agentxx/middlewares/interrupt_presets.h"
#include "agentxx/middlewares/interrupt_ui.h"
#include "pluginxx/ui.h"
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ui_items_passed = 0;
int g_ui_items_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ui_items_passed
#define XX_TEST_FAILED g_ui_items_failed

namespace agentxx {
namespace test {

using utilxx_base::Json;
using pluginxx::ui::Item;
using pluginxx::ui::SizeValue;
using pluginxx::ui::TableCell;
using pluginxx::ui::TableColumn;
using pluginxx::ui::TextValue;

namespace {

/// 子串存在性 (纯文本降级断言用)
bool has(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

/// 一个两列一行的表格组件项
Item sampleTable() {
    Item table   = pluginxx::ui::build::node("Table");
    table.header = true;
    table.columns.push_back(TableColumn{TextValue::of("Path"), "start", SizeValue::autoValue(), {}});
    table.columns.push_back(TableColumn{TextValue::of("Scope"), "end", SizeValue::of(6), {}});
    table.rows.push_back({
        TableCell{TextValue::of("a.txt"), {}, {}},
        TableCell{TextValue::of("write"), {}, {}},
    });
    return table;
}

} // namespace

TestResult testUiItems() {
    using namespace agentxx::middleware;

    // ---------------- 块 → 组件项 (itemOf) ----------------
    {
        InterruptUiBlock b;
        b.kind = "text";
        b.text = "hello";
        b.color = "accent";
        b.bold  = true;
        b.wrap  = true;
        const auto item = itemOf(b);
        XX_TEST_EXPECT_TRUE(item.has_value());
        if (item) {
            XX_TEST_EXPECT_EQ(item->kind, std::string{"Text"});
            XX_TEST_EXPECT_EQ(item->text.fallback, std::string{"hello"});
            XX_TEST_EXPECT_EQ(item->tone, std::string{"accent"});
            XX_TEST_EXPECT_TRUE(item->bold);
            XX_TEST_EXPECT_TRUE(item->wrap);
        }
    }
    {
        // i18n 键: 文本块带 textKey 时用 TextValue 的 key 承载 (客户端词表解析)
        InterruptUiBlock b;
        b.kind    = "text";
        b.textKey = "interrupt.header";
        b.text    = "Header";
        const auto item = itemOf(b);
        XX_TEST_EXPECT_TRUE(item.has_value());
        if (item) {
            XX_TEST_EXPECT_EQ(item->text.key, std::string{"interrupt.header"});
            XX_TEST_EXPECT_EQ(item->text.fallback, std::string{"Header"});
        }
    }
    {
        // 缩进: 描述层没有 indent 字段, 用 Padding 容器表达 (u 由客户端换算)
        InterruptUiBlock b;
        b.kind   = "text";
        b.text   = "indented";
        b.indent = 2;
        const auto item = itemOf(b);
        XX_TEST_EXPECT_TRUE(item.has_value());
        if (item) {
            XX_TEST_EXPECT_EQ(item->kind, std::string{"Padding"});
            XX_TEST_EXPECT_EQ(item->children.size(), size_t{1});
            XX_TEST_EXPECT_TRUE(item->padding.left > 0.0);
        }
    }
    {
        InterruptUiBlock b;
        b.kind   = "gap";
        b.lines  = 2;
        const auto item = itemOf(b);
        XX_TEST_EXPECT_TRUE(item.has_value());
        if (item) {
            XX_TEST_EXPECT_EQ(item->kind, std::string{"Gap"});
            // 两行 → 两个格高 (默认 20u)
            XX_TEST_EXPECT_TRUE(item->size.value >= 2.0 * pluginxx::ui::gen::kDefaultCellHeight);
        }
    }
    {
        InterruptUiBlock b;
        b.kind = "separator";
        const auto item = itemOf(b);
        XX_TEST_EXPECT_TRUE(item.has_value() && item->kind == "Divider");
    }
    {
        InterruptUiBlock b;
        b.kind   = "diff";
        b.path   = "a.cpp";
        b.oldStr = "x";
        b.newStr = "y";
        const auto item = itemOf(b);
        XX_TEST_EXPECT_TRUE(item.has_value());
        if (item) {
            XX_TEST_EXPECT_EQ(item->kind, std::string{"Diff"});
            XX_TEST_EXPECT_EQ(item->path, std::string{"a.cpp"});
            XX_TEST_EXPECT_EQ(item->oldStr, std::string{"x"});
            XX_TEST_EXPECT_EQ(item->newStr, std::string{"y"});
        }
    }
    {
        // markdown 块
        InterruptUiBlock b;
        b.kind = "markdown";
        b.text = "# title";
        const auto item = itemOf(b);
        XX_TEST_EXPECT_TRUE(item.has_value());
        if (item) {
            XX_TEST_EXPECT_EQ(item->kind, std::string{"Markdown"});
            XX_TEST_EXPECT_EQ(item->markdown, std::string{"# title"});
        }
    }
    {
        // 控件: 形态/标签/说明/候选项原值/缺省值/数值范围都进描述层
        InterruptUiBlock b;
        b.kind       = "control";
        b.id         = "level";
        b.control    = "number";
        b.label      = "Level";
        b.help       = "1..5";
        b.defaultValue = 3;
        b.integer    = true;
        b.hasMin     = true;
        b.minValue   = 1.0;
        b.hasMax     = true;
        b.maxValue   = 5.0;
        b.step       = 2.0;
        const auto item = itemOf(b);
        XX_TEST_EXPECT_TRUE(item.has_value());
        if (item) {
            XX_TEST_EXPECT_EQ(item->kind, std::string{"Control"});
            XX_TEST_EXPECT_EQ(item->id, std::string{"level"});
            XX_TEST_EXPECT_EQ(item->control, std::string{"number"});
            XX_TEST_EXPECT_EQ(item->label.fallback, std::string{"Level"});
            XX_TEST_EXPECT_EQ(item->help.fallback, std::string{"1..5"});
            XX_TEST_EXPECT_EQ(item->valueJson, std::string{"3"});
            XX_TEST_EXPECT_TRUE(item->integer);
            XX_TEST_EXPECT_TRUE(item->hasMin && item->hasMax);
            XX_TEST_EXPECT_EQ(item->minValue, 1.0);
            XX_TEST_EXPECT_EQ(item->maxValue, 5.0);
            XX_TEST_EXPECT_EQ(item->step, 2.0);
        }
    }
    {
        // 候选项: 原值 (任意 JSON) 与标签 (i18n 键优先) 分别承载
        InterruptUiBlock b;
        b.kind    = "control";
        b.control = "buttons";
        b.id      = "pick";
        b.options.push_back(InterruptUiOption{Json("true"), {}, "interrupt.yes", {}});
        b.options.push_back(InterruptUiOption{Json(false), "No", {}, "error"});
        const auto item = itemOf(b);
        XX_TEST_EXPECT_TRUE(item.has_value());
        if (item) {
            XX_TEST_EXPECT_EQ(item->options.size(), size_t{2});
            // 候选项原值是"紧凑 JSON 文本": 存字符串就带引号, 存布尔就不带
            XX_TEST_EXPECT_EQ(item->options[0].valueJson, std::string{R"("true")"});
            XX_TEST_EXPECT_EQ(item->options[0].label.key, std::string{"interrupt.yes"});
            XX_TEST_EXPECT_EQ(item->options[1].valueJson, std::string{"false"});
            XX_TEST_EXPECT_EQ(item->options[1].label.fallback, std::string{"No"});
            XX_TEST_EXPECT_EQ(item->options[1].tone, std::string{"error"});
        }
    }
    {
        // 提交行: 确认/取消是域内两个按钮 (动作 id 为域内提交约定)
        InterruptUiBlock b;
        b.kind = "submit";
        const auto item = itemOf(b);
        XX_TEST_EXPECT_TRUE(item.has_value());
        if (item) {
            XX_TEST_EXPECT_EQ(item->kind, std::string{"Row"});
            XX_TEST_EXPECT_EQ(item->children.size(), size_t{2});
            XX_TEST_EXPECT_EQ(
                item->children[0].action.name,
                std::string{kInterruptSubmitActionId}
            );
            XX_TEST_EXPECT_EQ(
                item->children[1].action.name,
                std::string{kInterruptCancelActionId}
            );
            // 文案只给键 (字面文本由客户端词表提供)
            XX_TEST_EXPECT_EQ(item->children[0].label.key, std::string{"interrupt.confirm"});
        }
    }
    {
        // 扩展组件: 块描述即组件描述 (按 raw 解析)
        InterruptUiBlock b;
        b.kind = "Table";
        b.raw  = pluginxx::ui::dumpItem(sampleTable());
        const auto item = itemOf(b);
        XX_TEST_EXPECT_TRUE(item.has_value());
        if (item) {
            XX_TEST_EXPECT_EQ(item->kind, std::string{"Table"});
            XX_TEST_EXPECT_EQ(item->columns.size(), size_t{2});
            XX_TEST_EXPECT_EQ(item->rows.size(), size_t{1});
        }
    }
    {
        // 未知块: 无内容可映射 → 空 (调用方按 fallback 处理)
        InterruptUiBlock b;
        b.kind = "unknown_kind";
        XX_TEST_EXPECT_TRUE(!itemOf(b).has_value());
    }

    // ---------------- 组件项 ↔ 块 (blocksOf / blockOf) ----------------
    {
        auto blocks = preset::blocksOf({pluginxx::ui::build::title("标题"), sampleTable()});
        XX_TEST_EXPECT_EQ(blocks.size(), size_t{2});
        // 中断层有自己的块词汇 (text/markdown/diff/separator/gap/control; 见 blockOf)
        XX_TEST_EXPECT_EQ(blocks[0].kind, std::string{"text"});
        XX_TEST_EXPECT_EQ(blocks[0].text, std::string{"标题"});
        // 描述层的扩展组件按原始 JSON 带走 (kind 保持组件名)
        XX_TEST_EXPECT_EQ(blocks[1].kind, std::string{"Table"});

        // 往返: 块 → 项 → 块, 内容字段保持
        const auto item = itemOf(blocks[1]);
        XX_TEST_EXPECT_TRUE(item.has_value());
        if (item) {
            const auto back = blockOf(*item);
            XX_TEST_EXPECT_EQ(back.kind, std::string{"Table"});
            const auto again = itemOf(back);
            XX_TEST_EXPECT_TRUE(again.has_value());
            if (again) {
                XX_TEST_EXPECT_EQ(again->rows.size(), size_t{1});
                XX_TEST_EXPECT_EQ(again->rows[0][0].text.fallback, std::string{"a.txt"});
                XX_TEST_EXPECT_EQ(again->columns[1].align, std::string{"end"});
            }
        }
    }
    {
        // 单块内容 (contentBlock) 与空输入
        const auto block = preset::contentBlock(pluginxx::ui::build::badge("3", "accent"));
        XX_TEST_EXPECT_EQ(block.kind, std::string{"Badge"});
        const auto item = itemOf(block);
        XX_TEST_EXPECT_TRUE(item.has_value());
        if (item) {
            XX_TEST_EXPECT_EQ(item->text.fallback, std::string{"3"});
            XX_TEST_EXPECT_EQ(item->tone, std::string{"accent"});
        }
    }

    // ---------------- 预设模板结构 ----------------
    {
        // 单输入项: 控件 id = "value"; bool 类型是"点击即提交"的一问一答
        const auto ui = preset::inputForm({preset::InputSpec{.label = "继续?", .type = "bool"}});
        XX_TEST_EXPECT_TRUE(!ui.blocks.empty());
        const auto* control = [&]() -> const InterruptUiBlock* {
            for (const auto& b : ui.blocks) {
                if (b.kind == "control") {
                    return &b;
                }
            }
            return nullptr;
        }();
        XX_TEST_EXPECT_TRUE(control != nullptr);
        if (control) {
            XX_TEST_EXPECT_EQ(control->id, std::string{"value"});
            XX_TEST_EXPECT_EQ(control->control, std::string{"buttons"});
            XX_TEST_EXPECT_TRUE(control->commitOnPick);
            XX_TEST_EXPECT_EQ(control->options.size(), size_t{2});
            XX_TEST_EXPECT_EQ(control->options[0].value, Json("true"));
        }
    }
    {
        // 多输入项: 控件 id 追加序号; enum → select; int → number; string → text
        const auto ui = preset::inputForm({
            preset::InputSpec{.type = "enum", .enumValues = {"a", "b"}},
            preset::InputSpec{.type = "int", .defaultValue = "7"},
            preset::InputSpec{.type = "string", .defaultValue = "x"},
        });
        std::vector<const InterruptUiBlock*> controls;
        for (const auto& b : ui.blocks) {
            if (b.kind == "control") {
                controls.push_back(&b);
            }
        }
        XX_TEST_EXPECT_EQ(controls.size(), size_t{3});
        if (controls.size() == 3) {
            XX_TEST_EXPECT_EQ(controls[0]->id, std::string{"value1"});
            XX_TEST_EXPECT_EQ(controls[0]->control, std::string{"select"});
            XX_TEST_EXPECT_EQ(controls[0]->defaultValue, Json("a"));
            XX_TEST_EXPECT_EQ(controls[1]->id, std::string{"value2"});
            XX_TEST_EXPECT_EQ(controls[1]->control, std::string{"number"});
            XX_TEST_EXPECT_TRUE(controls[1]->integer);
            XX_TEST_EXPECT_EQ(controls[2]->id, std::string{"value3"});
            XX_TEST_EXPECT_EQ(controls[2]->control, std::string{"text"});
        }
    }
    {
        // 确认卡片: 控件 id 可指定, 默认选中"否"(安全语义), 记住项可选
        preset::ConfirmCardOptions opts;
        opts.title       = "继续?";
        opts.text        = "**说明**";
        opts.controlId   = "allow";
        opts.remember    = true;
        opts.defaultValue = false;
        const auto ui = preset::confirmCard(opts);
        bool hasRemember = false;
        bool hasAllow    = false;
        for (const auto& b : ui.blocks) {
            if (b.kind != "control") {
                continue;
            }
            if (b.id == "remember") {
                hasRemember = true;
            }
            if (b.id == "allow") {
                hasAllow = true;
                XX_TEST_EXPECT_TRUE(b.commitOnPick);
                XX_TEST_EXPECT_EQ(b.defaultValue, Json("false"));
                XX_TEST_EXPECT_EQ(b.options.size(), size_t{2});
            }
        }
        XX_TEST_EXPECT_TRUE(hasRemember);
        XX_TEST_EXPECT_TRUE(hasAllow);
    }
    {
        // 权限卡片: 头行分段 (文案只给键) + 目录目标提示 + 两个勾选项 + 一键取值
        const auto ui = preset::permissionCard("write_file", "write", "D:/dir/");
        XX_TEST_EXPECT_EQ(ui.header.segments.size(), size_t{3});
        XX_TEST_EXPECT_EQ(ui.header.segments[0].labelKey, std::string{"interrupt.permissionBadge"});
        bool hasRemember = false;
        bool hasFullAuth = false;
        bool hasDecision = false;
        bool hasDirTip   = false;
        for (const auto& b : ui.blocks) {
            if (b.kind == "control") {
                hasRemember = hasRemember || b.id == "remember";
                hasFullAuth = hasFullAuth || b.id == "fullAuth";
                if (b.id == "decision") {
                    hasDecision = true;
                    XX_TEST_EXPECT_EQ(b.control, std::string{"buttons"});
                    XX_TEST_EXPECT_EQ(b.defaultValue, Json("false"));
                }
            }
            if (b.textKey == "interrupt.rememberDir") {
                hasDirTip = true;
            }
        }
        XX_TEST_EXPECT_TRUE(hasRemember);
        XX_TEST_EXPECT_TRUE(hasFullAuth);
        XX_TEST_EXPECT_TRUE(hasDecision);
        XX_TEST_EXPECT_TRUE(hasDirTip);
    }

    // ---------------- 纯文本降级 (行式前端: CLI / 日志 / FFI) ----------------
    {
        InterruptUi desc;
        desc.blocks.push_back(preset::textBlock("第一行"));
        desc.blocks.push_back(preset::gapBlock(1));
        desc.blocks.push_back(preset::textBlock("第二行"));
        const auto text = interruptUiPlainText(desc, 0);
        // 空行 (Gap) 也占一行, 不能按"空内容"跳过
        XX_TEST_EXPECT_EQ(text, std::string{"第一行\n\n第二行"});
    }
    {
        InterruptUi desc;
        desc.blocks.push_back(preset::contentBlock(sampleTable()));
        desc.blocks.push_back(preset::meterBlock("CPU", 72, 100, {{90, "error"}}));
        desc.blocks.push_back(preset::submitBlock());
        const auto text = interruptUiPlainText(desc, 40);
        XX_TEST_EXPECT_TRUE(has(text, "Path"));
        XX_TEST_EXPECT_TRUE(has(text, "a.txt"));
        XX_TEST_EXPECT_TRUE(has(text, "CPU"));
        // 提交行只有交互语义: 纯文本里不出现确认/取消按钮
        XX_TEST_EXPECT_TRUE(!has(text, "__submit"));
    }
    {
        // 未知块有 fallback 时按文本输出 (不静默丢内容)
        InterruptUi desc;
        InterruptUiBlock b;
        b.kind     = "future_block";
        b.fallback = "n/a";
        desc.blocks.push_back(b);
        XX_TEST_EXPECT_EQ(interruptUiPlainText(desc, 0), std::string{"n/a"});
    }

    // ---------------- 显示列宽 (终端渲染与文本降级同一套规则) ----------------
    {
        XX_TEST_EXPECT_EQ(pluginxx::ui::displayWidth("abc"), 3);
        XX_TEST_EXPECT_EQ(pluginxx::ui::displayWidth("中文"), 4);
        XX_TEST_EXPECT_EQ(pluginxx::ui::displayWidth(""), 0);
    }
    {
        // 截断: 宽字符不会被切成半格; 省略号自身占宽
        XX_TEST_EXPECT_EQ(pluginxx::ui::truncateToWidth("abcdef", 4), std::string{"abc…"});
        XX_TEST_EXPECT_EQ(pluginxx::ui::truncateToWidth("中文中", 4), std::string{"中…"});
        XX_TEST_EXPECT_EQ(pluginxx::ui::truncateToWidth("abc", 5), std::string{"abc"});
        XX_TEST_EXPECT_EQ(pluginxx::ui::truncateToWidth("abc", 0), std::string{});
        XX_TEST_EXPECT_EQ(pluginxx::ui::truncateToWidth("abcdef", 3, ".."), std::string{"a.."});
    }
    {
        XX_TEST_EXPECT_EQ(pluginxx::ui::padRightToWidth("ab", 4), std::string{"ab  "});
        XX_TEST_EXPECT_EQ(pluginxx::ui::padRightToWidth("abcd", 2), std::string{"abcd"});
        XX_TEST_EXPECT_EQ(pluginxx::ui::padRightToWidth("中", 4), std::string{"中  "});
    }
    {
        int              used = 0;
        std::string_view head = pluginxx::ui::prefixByWidth("中abc", 3, used);
        XX_TEST_EXPECT_EQ(used, 3);
        XX_TEST_EXPECT_EQ(std::string{head}, std::string{"中a"});
    }

    return TestResult{g_ui_items_passed, g_ui_items_failed};
}

} // namespace test
} // namespace agentxx
