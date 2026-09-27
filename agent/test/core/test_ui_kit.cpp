// agentxx 界面扩展 kit 与能力适配的用法测试
//
// 描述层本身 (解析/上限/序列化/纯文本/适配规则/基础 kit) 的用例在库仓库
// (cxx_pluginxx_ui) 的单测里; 本模块覆盖 agentxx 这一侧:
// - 扩展 kit 组件 (agentxx::ui::kit) 的装配结果与留白口径
// - 带 env (客户端能力摘要) 时的变体选择: 目标未知用第一个变体, 目标不支持时走兜底
// - 同一份描述在不同能力下 adapt 的收敛结果 (只含声明支持的组件, 最多降到 Text)
// - kit 产出 → dumpItem → parseBlock 的往返
#include "agentxx-test/core/test_ui_kit.h"

#include "agentxx/plugin/api/agentxx_ui_kit.g.h"
#include "pluginxx/ui.h"
#include "utilxx_base/json.h"
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ui_kit_passed = 0;
int g_ui_kit_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ui_kit_passed
#define XX_TEST_FAILED g_ui_kit_failed

namespace agentxx {
namespace test {

using pluginxx::ui::Action;
using pluginxx::ui::Capabilities;
using pluginxx::ui::Item;
using utilxx_base::Json;

namespace {

/// 文本字面值 (kit 参数里的字符串)
std::string textOf(const Item& item) {
    return item.text.fallback;
}

/// 深度优先查第一个指定种类的节点 (找不到返回 nullptr)
const Item* findItem(const Item& node, std::string_view kind) {
    if (node.kind == kind) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const Item* hit = findItem(child, kind)) {
            return hit;
        }
    }
    return nullptr;
}

/// 统计直接子节点里指定种类的个数
int countChildren(const Item& node, std::string_view kind) {
    int n = 0;
    for (const auto& child : node.children) {
        if (child.kind == kind) {
            ++n;
        }
    }
    return n;
}

/// 深度优先查第一个文本包含指定子串的节点 (找不到返回 nullptr)
const Item* findTextContaining(const Item& node, std::string_view needle) {
    if (node.kind == "Text" && node.text.fallback.find(needle) != std::string::npos) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const Item* hit = findTextContaining(child, needle)) {
            return hit;
        }
    }
    return nullptr;
}

/// 一组组件项 (含嵌套子节点) 是否全在当前能力里 (适配收敛的判据)
bool allSupported(const std::vector<Item>& items, const Capabilities& caps) {
    for (const auto& item : items) {
        if (!item.known) {
            continue; // 未知块保留 fallback, 由客户端决定显示或跳过
        }
        if (!caps.supportsBlock(item.kind)) {
            return false;
        }
        if (!allSupported(item.children, caps)) {
            return false;
        }
    }
    return true;
}

/// 终端常用集: 去掉 Stack / Image 与客户端专属块 (与 TUI 客户端上报的口径一致)
Capabilities tuiCaps() {
    Capabilities             caps = pluginxx::ui::fullCapabilities();
    caps.kind                     = "tui";
    caps.cell                     = pluginxx::ui::CellSize{8.0, 20.0};
    std::vector<std::string> keep;
    for (const auto& name : caps.blocks) {
        if (name == "Stack" || name == "Image" || name == "musicxx.Shader") {
            continue;
        }
        keep.push_back(name);
    }
    caps.blocks = std::move(keep);
    return caps;
}

/// 去掉某个组件的能力 (模拟"这个客户端没实现它")
Capabilities capsWithout(std::string_view drop) {
    Capabilities             caps = pluginxx::ui::fullCapabilities();
    std::vector<std::string> keep;
    for (const auto& name : caps.blocks) {
        if (name != drop) {
            keep.push_back(name);
        }
    }
    caps.blocks = std::move(keep);
    return caps;
}

} // namespace

TestResult testUiKit() {
    // ---------------- 基础 kit 的 agentxx 口径 ----------------
    {
        const Item title = agentxx::ui::kit::title({{"text", "会话统计"}});
        XX_TEST_EXPECT_EQ(title.kind, std::string{"Text"});
        XX_TEST_EXPECT_EQ(title.textType, std::string{"title"});
        XX_TEST_EXPECT_EQ(textOf(title), std::string{"会话统计"});

        const Item divider = agentxx::ui::kit::divider({});
        XX_TEST_EXPECT_EQ(divider.kind, std::string{"Divider"});

        // 缺省留白: kit 只表达"用客户端默认行距" (SizeValue 为 auto)
        const Item gap = agentxx::ui::kit::gap({});
        XX_TEST_EXPECT_EQ(gap.kind, std::string{"Gap"});
        XX_TEST_EXPECT_TRUE(gap.size.isAuto());
    }
    {
        // card 覆盖: 终端卡片留白更紧 (横向 8u / 纵向 4u)
        const Item card = agentxx::ui::kit::card(
            {{"title", "索引"},
             {"children", Json::array({Json::parse(R"({"kind":"Text","text":"inner"})")})}}
        );
        XX_TEST_EXPECT_EQ(card.kind, std::string{"Block"});
        XX_TEST_EXPECT_EQ(card.variant, std::string{"card"});
        XX_TEST_EXPECT_TRUE(card.hasPadding);
        XX_TEST_EXPECT_EQ(card.padding.left, 8.0);
        XX_TEST_EXPECT_EQ(card.padding.right, 8.0);
        XX_TEST_EXPECT_EQ(card.padding.top, 4.0);
        XX_TEST_EXPECT_EQ(card.padding.bottom, 4.0);
        XX_TEST_EXPECT_EQ(card.children.size(), size_t{1});
        XX_TEST_EXPECT_EQ(card.children.front().kind, std::string{"Text"});
    }
    {
        // listRow 覆盖: inset 行 + 8u 间距; 副标题 / 右侧文字缺省时不产出空节点
        const Item row = agentxx::ui::kit::listRow({{"title", "切歌次数"}, {"trailing", "3"}});
        XX_TEST_EXPECT_EQ(row.kind, std::string{"Block"});
        XX_TEST_EXPECT_EQ(row.variant, std::string{"inset"});
        XX_TEST_EXPECT_TRUE(row.hasPadding);
        XX_TEST_EXPECT_EQ(row.padding.left, 8.0);
        XX_TEST_EXPECT_EQ(row.padding.top, 4.0);
        XX_TEST_EXPECT_EQ(row.children.size(), size_t{1}); // Block 里只有一个 Row

        const Item& line = row.children.front();
        XX_TEST_EXPECT_EQ(line.kind, std::string{"Row"});
        XX_TEST_EXPECT_TRUE(line.hasGap);
        XX_TEST_EXPECT_EQ(line.gap.value, 8.0);
        XX_TEST_EXPECT_EQ(line.cross, std::string{"center"});
        XX_TEST_EXPECT_EQ(line.children.size(), size_t{2}); // 左侧内容 + 右侧文字
        XX_TEST_EXPECT_EQ(line.children.back().tone, std::string{"hint"});
        XX_TEST_EXPECT_EQ(textOf(line.children.back()), std::string{"3"});

        const Item* col = findItem(line, "Column");
        XX_TEST_EXPECT_TRUE(col != nullptr);
        if (col != nullptr) {
            XX_TEST_EXPECT_EQ(col->children.size(), size_t{1}); // 无副标题
            XX_TEST_EXPECT_EQ(textOf(col->children.front()), std::string{"切歌次数"});
        }

        // 不带右侧文字时 Row 只有左侧一份内容
        const Item plain = agentxx::ui::kit::listRow({{"title", "A"}});
        XX_TEST_EXPECT_EQ(plain.children.front().children.size(), size_t{1});

        // 带副标题: 左侧多一行 caption
        const Item row2
            = agentxx::ui::kit::listRow({{"title", "A"}, {"subtitle", "B"}, {"trailing", "C"}});
        const Item* col2 = findItem(row2, "Column");
        XX_TEST_EXPECT_TRUE(col2 != nullptr);
        if (col2 != nullptr) {
            XX_TEST_EXPECT_EQ(col2->children.size(), size_t{2});
            XX_TEST_EXPECT_EQ(col2->children[1].textType, std::string{"caption"});
            XX_TEST_EXPECT_EQ(textOf(col2->children[1]), std::string{"B"});
        }
    }
    {
        // progressRow 覆盖: agentxx 默认按百分比显示
        const Item row = agentxx::ui::kit::progressRow({{"label", "CPU"}, {"value", 42}});
        XX_TEST_EXPECT_EQ(row.kind, std::string{"Row"});
        const Item* bar = findItem(row, "Progress");
        XX_TEST_EXPECT_TRUE(bar != nullptr);
        if (bar != nullptr) {
            XX_TEST_EXPECT_EQ(bar->value, 42.0);
            XX_TEST_EXPECT_EQ(bar->total, 100.0);
            XX_TEST_EXPECT_EQ(bar->unit, std::string{"%"});
        }
        const Item custom
            = agentxx::ui::kit::progressRow({{"value", 3}, {"total", 10}, {"unit", "MB"}});
        const Item* bar2 = findItem(custom, "Progress");
        XX_TEST_EXPECT_TRUE(bar2 != nullptr);
        if (bar2 != nullptr) {
            XX_TEST_EXPECT_EQ(bar2->unit, std::string{"MB"});
            XX_TEST_EXPECT_EQ(bar2->total, 10.0);
        }
    }
    {
        // cols / rows: 按客户端格大小换算 (缺省用库默认 8u × 20u)
        XX_TEST_EXPECT_EQ(agentxx::ui::kit::cols(2), 16.0);
        XX_TEST_EXPECT_EQ(agentxx::ui::kit::rows(2), 40.0);
        const Capabilities caps = tuiCaps();
        XX_TEST_EXPECT_EQ(agentxx::ui::kit::cols(3, &caps), 24.0);
        XX_TEST_EXPECT_EQ(agentxx::ui::kit::rows(1, &caps), 20.0);
    }

    // ---------------- agentxx 常用组合 ----------------
    {
        // toolCallRow: 工具名 + 摘要 + 状态标签; 没给状态就不产标签
        const Item row = agentxx::ui::kit::toolCallRow(
            {{"name", "grep"},
             {"depict", "在 src 下搜索 TODO"},
             {"status", "完成"},
             {"statusTone", "success"},
             {"action", "open:grep"}}
        );
        XX_TEST_EXPECT_EQ(row.kind, std::string{"Block"});
        XX_TEST_EXPECT_EQ(row.variant, std::string{"inset"});
        XX_TEST_EXPECT_EQ(row.action.kind, Action::Kind::Dispatch);
        XX_TEST_EXPECT_EQ(row.action.name, std::string{"open:grep"});
        const Item* badge = findItem(row, "Badge");
        XX_TEST_EXPECT_TRUE(badge != nullptr);
        if (badge != nullptr) {
            XX_TEST_EXPECT_EQ(textOf(*badge), std::string{"完成"});
            XX_TEST_EXPECT_EQ(badge->tone, std::string{"success"});
        }
        const Item* col = findItem(row, "Column");
        XX_TEST_EXPECT_TRUE(col != nullptr);
        if (col != nullptr) {
            XX_TEST_EXPECT_EQ(col->children.size(), size_t{2}); // 名称 + 摘要
        }

        const Item plain = agentxx::ui::kit::toolCallRow({{"name", "grep"}});
        XX_TEST_EXPECT_TRUE(findItem(plain, "Badge") == nullptr);
        XX_TEST_EXPECT_EQ(findItem(plain, "Column")->children.size(), size_t{1});
    }
    {
        // thinkingBlock: 正文用 thinking 色调; label 可选
        const Item block = agentxx::ui::kit::thinkingBlock({{"content", "先看索引再取内容"}});
        XX_TEST_EXPECT_EQ(block.kind, std::string{"Block"});
        XX_TEST_EXPECT_EQ(block.variant, std::string{"inset"});
        XX_TEST_EXPECT_EQ(countChildren(block, "Column"), 1);
        const Item* col = findItem(block, "Column");
        XX_TEST_EXPECT_TRUE(col != nullptr);
        if (col != nullptr) {
            XX_TEST_EXPECT_EQ(col->children.size(), size_t{1});
            XX_TEST_EXPECT_EQ(col->children.front().tone, std::string{"thinking"});
        }
        const Item withLabel
            = agentxx::ui::kit::thinkingBlock({{"content", "正文"}, {"label", "思考"}});
        const Item* col2 = findItem(withLabel, "Column");
        XX_TEST_EXPECT_TRUE(col2 != nullptr);
        if (col2 != nullptr) {
            XX_TEST_EXPECT_EQ(col2->children.size(), size_t{2});
            XX_TEST_EXPECT_EQ(col2->children.front().textType, std::string{"caption"});
        }
    }
    {
        // sessionStats: 可选标题 + 表格 (列宽交给客户端自动分配)
        const Json columns
            = Json::parse(R"([{"title":"指标","width":"auto"},{"title":"数值","align":"end"}])");
        const Json rows = Json::parse(R"([["命中","12"],["未命中","3"]])");
        const Item stats
            = agentxx::ui::kit::sessionStats({{"columns", columns}, {"rows", rows}});
        const Item* table = findItem(stats, "Table");
        XX_TEST_EXPECT_TRUE(table != nullptr);
        if (table != nullptr) {
            XX_TEST_EXPECT_TRUE(table->header);
            XX_TEST_EXPECT_EQ(table->columns.size(), size_t{2});
            XX_TEST_EXPECT_EQ(table->rows.size(), size_t{2});
            XX_TEST_EXPECT_TRUE(table->columns.front().width.isAuto());
            XX_TEST_EXPECT_EQ(table->columns[1].align, std::string{"end"});
            XX_TEST_EXPECT_EQ(table->rows.front().front().text.fallback, std::string{"命中"});
        }
        // 没给标题时不产出标题行
        XX_TEST_EXPECT_TRUE(findItem(stats, "Text") == nullptr);
        const Item titled = agentxx::ui::kit::sessionStats(
            {{"title", "本轮统计"}, {"columns", columns}, {"rows", rows}}
        );
        XX_TEST_EXPECT_TRUE(findItem(titled, "Text") != nullptr);
    }
    {
        // pathDiffRow: 目标未知 → 第一个变体 (Diff); 目标不支持 Diff → 等宽文本兜底
        const Item diff = agentxx::ui::kit::pathDiffRow(
            {{"path", "src/main.cpp"}, {"oldStr", "old"}, {"newStr", "new"}}
        );
        const Item* diffNode = findItem(diff, "Diff");
        XX_TEST_EXPECT_TRUE(diffNode != nullptr);
        if (diffNode != nullptr) {
            XX_TEST_EXPECT_EQ(diffNode->path, std::string{"src/main.cpp"});
            XX_TEST_EXPECT_EQ(diffNode->oldStr, std::string{"old"});
            XX_TEST_EXPECT_EQ(diffNode->newStr, std::string{"new"});
        }

        const Capabilities noDiff   = capsWithout("Diff");
        const Item         degraded = agentxx::ui::kit::pathDiffRow(
            {{"path", "src/main.cpp"}, {"oldStr", "old"}, {"newStr", "new"}},
            &noDiff
        );
        XX_TEST_EXPECT_TRUE(findItem(degraded, "Diff") == nullptr);
        const Item* text = findTextContaining(degraded, "old");
        XX_TEST_EXPECT_TRUE(text != nullptr);
        if (text != nullptr) {
            XX_TEST_EXPECT_TRUE(text->mono);
            XX_TEST_EXPECT_TRUE(text->text.fallback.find("new") != std::string::npos);
        }
    }
    {
        // diagramBlock: 支持 Diagram 时给图; 不支持时退化成等宽源码
        const Item diagram
            = agentxx::ui::kit::diagramBlock({{"mermaid", "stateDiagram-v2\n  [*] --> A"}});
        XX_TEST_EXPECT_TRUE(findItem(diagram, "Diagram") != nullptr);

        const Capabilities noDiagram = capsWithout("Diagram");
        const Item         degraded   = agentxx::ui::kit::diagramBlock(
            {{"mermaid", "stateDiagram-v2\n  [*] --> A"}},
            &noDiagram
        );
        XX_TEST_EXPECT_TRUE(findItem(degraded, "Diagram") == nullptr);
        const Item* text = findItem(degraded, "Text");
        XX_TEST_EXPECT_TRUE(text != nullptr);
        if (text != nullptr) {
            XX_TEST_EXPECT_TRUE(text->mono);
            XX_TEST_EXPECT_TRUE(text->text.fallback.find("stateDiagram-v2") != std::string::npos);
        }
    }
    {
        // interruptRow: 确认 / 取消两个等份按钮; 动作名缺省用中断域内约定
        const Item row
            = agentxx::ui::kit::interruptRow({{"confirmLabel", "确认"}, {"cancelLabel", "取消"}});
        XX_TEST_EXPECT_EQ(row.kind, std::string{"Row"});
        XX_TEST_EXPECT_EQ(row.main, std::string{"end"});
        XX_TEST_EXPECT_EQ(row.children.size(), size_t{2});
        XX_TEST_EXPECT_EQ(countChildren(row, "Expanded"), 2);
        const Item* confirm = findItem(row, "Button");
        XX_TEST_EXPECT_TRUE(confirm != nullptr);
        if (confirm != nullptr) {
            XX_TEST_EXPECT_EQ(confirm->label.fallback, std::string{"确认"});
            XX_TEST_EXPECT_EQ(confirm->variant, std::string{"primary"});
            XX_TEST_EXPECT_EQ(confirm->action.kind, Action::Kind::Dispatch);
            XX_TEST_EXPECT_EQ(confirm->action.name, std::string{"__submit"});
        }

        // 自定义动作名: 两个按钮各用给定的动作
        const Item custom = agentxx::ui::kit::interruptRow(
            {{"confirmLabel", "是"},
             {"cancelLabel", "否"},
             {"confirmAction", "yes"},
             {"cancelAction", "no"}}
        );
        XX_TEST_EXPECT_EQ(custom.children.size(), size_t{2});
        const Item* btnYes = findItem(custom.children[0], "Button");
        XX_TEST_EXPECT_TRUE(btnYes != nullptr);
        if (btnYes != nullptr) {
            XX_TEST_EXPECT_EQ(btnYes->action.name, std::string{"yes"});
        }
        const Item* btnNo = findItem(custom.children[1], "Button");
        XX_TEST_EXPECT_TRUE(btnNo != nullptr);
        if (btnNo != nullptr) {
            XX_TEST_EXPECT_EQ(btnNo->label.fallback, std::string{"否"});
            XX_TEST_EXPECT_EQ(btnNo->variant, std::string{"ghost"});
            XX_TEST_EXPECT_EQ(btnNo->action.name, std::string{"no"});
        }
    }

    // ---------------- 同一份描述在不同能力下的收敛 ----------------
    {
        const Json description = Json::parse(R"([
            {"kind":"Text","text":"封面与图标"},
            {"kind":"Image","source":"cover","src":"a.png","alt":"无封面"},
            {"kind":"Stack","children":[{"kind":"Text","text":"底层"},{"kind":"Text","text":"顶层"}]},
            {"kind":"Diff","path":"p","oldStr":"a","newStr":"b"},
            {"kind":"Sparkline","data":[1,2,3]},
            {"kind":"Diagram","mermaid":"stateDiagram-v2\n  [*] --> A"},
            {"kind":"musicxx.Shader","bundle":"bg.bundle"}
        ])");
        const auto origin = pluginxx::ui::parseBlocks(description);
        XX_TEST_EXPECT_EQ(origin.size(), size_t{7});

        // 完整能力: 不做任何降级
        const Capabilities full    = pluginxx::ui::fullCapabilities();
        const auto         fullOut = pluginxx::ui::adaptBlocks(origin, full);
        XX_TEST_EXPECT_EQ(fullOut.size(), size_t{7});
        XX_TEST_EXPECT_TRUE(allSupported(fullOut, full));
        XX_TEST_EXPECT_EQ(fullOut[1].kind, std::string{"Image"});
        XX_TEST_EXPECT_EQ(fullOut[2].kind, std::string{"Stack"});
        XX_TEST_EXPECT_EQ(fullOut[6].kind, std::string{"musicxx.Shader"});

        // 终端常用集: Image → alt 文本, Stack → 最后一个子节点, 专属块跳过;
        // Diagram 本端支持, 保持原样
        const Capabilities tui    = tuiCaps();
        const auto         tuiOut = pluginxx::ui::adaptBlocks(origin, tui);
        XX_TEST_EXPECT_TRUE(allSupported(tuiOut, tui));
        bool sawAltText  = false;
        bool sawTopChild = false;
        bool sawDiagram  = false;
        for (const auto& item : tuiOut) {
            XX_TEST_EXPECT_FALSE(item.kind == "musicxx.Shader");
            XX_TEST_EXPECT_FALSE(item.kind == "Image");
            XX_TEST_EXPECT_FALSE(item.kind == "Stack");
            if (item.kind == "Text" && item.text.fallback == "无封面") {
                sawAltText = true; // Image 的降级形态
            }
            if (item.kind == "Text" && item.text.fallback == "顶层") {
                sawTopChild = true; // Stack 降级后保留最后一个子节点
            }
            if (item.kind == "Diagram") {
                sawDiagram = true;
            }
        }
        XX_TEST_EXPECT_TRUE(sawAltText);
        XX_TEST_EXPECT_TRUE(sawTopChild);
        XX_TEST_EXPECT_TRUE(sawDiagram);

        // 不支持 Diagram 的客户端: 退化成等宽文本 (源码仍可见)
        const Capabilities noDiagram = capsWithout("Diagram");
        const auto noDiagramOut      = pluginxx::ui::adaptBlocks(origin, noDiagram);
        XX_TEST_EXPECT_TRUE(allSupported(noDiagramOut, noDiagram));
        bool sawMermaid = false;
        for (const auto& item : noDiagramOut) {
            XX_TEST_EXPECT_FALSE(item.kind == "Diagram");
            if (item.kind == "Text" && item.mono
                && item.text.fallback.find("stateDiagram-v2") != std::string::npos) {
                sawMermaid = true;
            }
        }
        XX_TEST_EXPECT_TRUE(sawMermaid);

        // 最小能力: 只留 Text
        const Capabilities minimal = pluginxx::ui::minimalCapabilities();
        const auto         minOut  = pluginxx::ui::adaptBlocks(origin, minimal);
        XX_TEST_EXPECT_TRUE(allSupported(minOut, minimal));
        for (const auto& item : minOut) {
            XX_TEST_EXPECT_EQ(item.kind, std::string{"Text"});
        }

        // 适配是纯函数: 同一输入 + 同一能力 → 同一输出
        const auto again = pluginxx::ui::adaptBlocks(origin, tui);
        XX_TEST_EXPECT_EQ(
            pluginxx::ui::dumpBlocks(again).dump(),
            pluginxx::ui::dumpBlocks(tuiOut).dump()
        );
    }

    // ---------------- kit 产出 → dump → parse 往返 ----------------
    {
        const Item row = agentxx::ui::kit::toolCallRow(
            {{"name", "grep"}, {"depict", "搜索"}, {"status", "完成"}, {"statusTone", "success"}}
        );
        const Item back = pluginxx::ui::parseBlock(pluginxx::ui::dumpItem(row));
        XX_TEST_EXPECT_EQ(back.kind, row.kind);
        XX_TEST_EXPECT_EQ(back.variant, row.variant);
        XX_TEST_EXPECT_EQ(back.children.size(), row.children.size());
        XX_TEST_EXPECT_EQ(back.padding.left, row.padding.left);
        XX_TEST_EXPECT_EQ(back.padding.top, row.padding.top);
        XX_TEST_EXPECT_EQ(pluginxx::ui::dumpItem(back).dump(), pluginxx::ui::dumpItem(row).dump());

        // 每个扩展 kit 组件都能被解析回来 (kind 不丢)
        const std::vector<Item> samples = {
            agentxx::ui::kit::listRow({{"title", "A"}}),
            agentxx::ui::kit::thinkingBlock({{"content", "B"}}),
            agentxx::ui::kit::sessionStats(
                {{"columns", Json::parse(R"([{"title":"C"}])")},
                 {"rows", Json::parse(R"([["1"]])")}}
            ),
            agentxx::ui::kit::diagramBlock({{"mermaid", "stateDiagram-v2"}}),
            agentxx::ui::kit::interruptRow({{"confirmLabel", "好"}, {"cancelLabel", "算了"}}),
        };
        for (const auto& sample : samples) {
            const Item parsed = pluginxx::ui::parseBlock(pluginxx::ui::dumpItem(sample));
            XX_TEST_EXPECT_EQ(parsed.kind, sample.kind);
            XX_TEST_EXPECT_TRUE(parsed.known);
        }
    }

    return TestResult{g_ui_kit_passed, g_ui_kit_failed};
}

} // namespace test
} // namespace agentxx
