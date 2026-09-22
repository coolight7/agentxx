#include "agentxx-client/io/tui/ui_components.h"

#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/markdown_block.h"
#include "agentxx-client/io/tui/text_layout.h"
#include "fmt/format.h"
#include "ftxui/screen/terminal.hpp"
#include "markdown/state_diagram.hpp"
#include "utilxx/diff_util.h"
#include "utilxx_base/log.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <tuple>
#include <utility>

using namespace ftxui;

namespace agentxx {
namespace client {

namespace {

using utilxx_base::Json;

// ---------------------------------------------------------------------------
// 基础 helper
// ---------------------------------------------------------------------------

/// 缩进空格串
std::string spaces(int cols) {
    return (cols > 0) ? std::string(static_cast<size_t>(cols), ' ') : std::string{};
}

/// 内容可用宽度 (扣除缩进; <=0 表示不限宽)
int contentWidth(const UiRenderCtx& ctx, int itemIndent) {
    if (ctx.width <= 0) {
        return 0;
    }
    return std::max(1, ctx.width - ctx.indent - itemIndent);
}

/// 数值显示文本 (整数不带小数点)
std::string numText(double v) {
    if (!std::isfinite(v)) {
        return "0";
    }
    if (std::fabs(v - std::round(v)) < 1e-9) {
        return fmt::format("{:.0f}", v);
    }
    return fmt::format("{:.1f}", v);
}

/// JSON 值 → 文本
std::string jsonText(const Json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    return v.is_null() ? std::string{} : v.dump();
}

/// 候选值下标 → 选中下标 (按归一化文本比较, 生产者不必与候选项值严格同型)
int optionIndex(const std::vector<agentxx::ui::ControlOption>& options, const Json& value) {
    if (options.empty() || value.is_null()) {
        return 0;
    }
    const auto want = jsonText(value);
    for (size_t i = 0; i < options.size(); ++i) {
        if (jsonText(options[i].value) == want) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 行模型 (容器组合时的中间形态; 区域坐标相对该行元素的左上角)
// ---------------------------------------------------------------------------

struct Row {
    Element                  element;
    size_t                   lines = 1;
    std::vector<UiHitRegion> regions;
};

using Rows = std::vector<Row>;

/// 多行垂直堆叠为一个元素 (区域坐标自动按行偏移)
Row stackRows(const Rows& rows, int dx = 0, int dy = 0) {
    Row      out;
    Elements els;
    int      offsetY = dy;
    for (const auto& row : rows) {
        els.push_back(row.element);
        for (const auto& region : row.regions) {
            out.regions.push_back(region.offsetBy(dx, offsetY));
        }
        offsetY += static_cast<int>(std::max<size_t>(1, row.lines));
    }
    out.lines = static_cast<size_t>(std::max(0, offsetY - dy));
    if (els.size() == 1) {
        out.element = std::move(els[0]);
    } else if (!els.empty()) {
        out.element = vbox(std::move(els));
    }
    if (out.lines == 0 || !out.element) {
        out.lines   = 1;
        out.element = text(" ");
    }
    return out;
}

// ---------------------------------------------------------------------------
// 文本与富文本
// ---------------------------------------------------------------------------

/// 文本项折行结果 (与渲染同一口径: 空内容 = 空列表)
std::vector<std::string> textLines(const agentxx::ui::Item& item, const UiRenderCtx& ctx) {
    if (item.text.empty()) {
        return {};
    }
    if (!item.wrap) {
        return {item.text};
    }
    return wrapTextToLines(item.text, std::max(1, contentWidth(ctx, item.indent)));
}

/// diff 块渲染行数 (与 renderPluginDiff 的 side-by-side/统一两种形态一致)
size_t diffBlockLines(const agentxx::ui::Item& item, const UiRenderCtx& ctx) {
    const auto   diff       = utilxx::computeLineDiff(item.oldStr, item.newStr);
    const int    sw         = (ctx.width > 0) ? ctx.width : Terminal::Size().dimx;
    const bool   sideBySide = sw >= 100;
    const size_t pathLines  = item.path.empty() ? 0 : 1;
    if (diff.empty()) {
        return pathLines + 1; // "(no changes)"
    }
    return pathLines + (sideBySide ? (diff.size() / 2 + 1) : diff.size());
}

/// mermaid 状态图解析 + 渲染 (解析失败返回空元素与 0 行)
std::pair<Element, size_t> buildDiagram(const agentxx::ui::Item& item, const UiRenderCtx& ctx) {
    if (item.mermaid.empty()) {
        return {text(""), 0};
    }
    auto diagram = markdown::parseMermaidStateDiagram(item.mermaid);
    if (diagram.nodes.empty()) {
        return {text(""), 0};
    }
    size_t srcLines = 1;
    for (char ch : item.mermaid) {
        if (ch == '\n') {
            ++srcLines;
        }
    }
    // 图形宽度预算: 可用宽度扣除缩进与边界余量, 下限保底可读
    const int   avail = (ctx.width > 0) ? std::max(20, ctx.width - ctx.indent - item.indent - 6) : 0;
    const auto& theme = *ctx.theme;
    auto        diagEl = markdown::renderMermaidStateDiagram(
        diagram,
        avail,
        theme.normalColor,
        markdown::diagramNodeColor(theme.markdownTheme)
    );
    return {std::move(diagEl), srcLines * 3 + 3};
}

// ---------------------------------------------------------------------------
// 按钮
// ---------------------------------------------------------------------------

/// 按钮配色角色 (语义色名 → 按钮样式)
PluginButtonRole buttonRoleOf(std::string_view color) {
    if (color == "accent" || color == "title" || color == "tool") {
        return PluginButtonRole::Accent;
    }
    if (color == "error" || color == "danger") {
        return PluginButtonRole::Danger;
    }
    return PluginButtonRole::Normal;
}

/// 按钮显示宽度 ("[ 标签 ]")
int buttonWidth(std::string_view label) {
    return agentxx::ui::displayWidth(label) + 4;
}

/// 渲染按钮元素
Element buttonElement(
    std::string_view label,
    std::string_view colorName,
    std::string_view style,
    const TUITheme&  theme
) {
    const std::string caption = label.empty() ? std::string{"Button"} : std::string{label};
    if (style == "text") {
        Element el = text(caption) | bold;
        if (colorName == "error" || colorName == "danger") {
            return el | color(theme.errorColor);
        }
        if (colorName == "hint") {
            return el | color(theme.hintColor);
        }
        return el | color(theme.accentColor);
    }
    PluginButtonDesc desc;
    desc.label = caption;
    desc.role  = buttonRoleOf(colorName);
    return renderPluginButton(desc, theme);
}

// ---------------------------------------------------------------------------
// 列宽分配 (表格与横排共用; 纯计算)
// ---------------------------------------------------------------------------

/// 声明固定宽度的列按声明值, 其余列平分剩余宽度
/// - 总和超出可用宽度时先收缩固定列 (下限 4 列), 空间仍不足则全部等分
/// - `avail <= 0` 时按缺省宽度铺开 (不限宽场景)
/// - 无自适应列时, 剩余宽度默认给最后一列; `stretchAll = true` 时改为均分给
///   各列 (row 的 `align: "stretch"`: 横向铺满可用宽度)
std::vector<int> layoutColumnWidths(
    const std::vector<int>& fixed,
    int                     avail,
    int                     gap,
    bool                    stretchAll = false
) {
    constexpr int kMinColumn = 4;
    const size_t  n          = fixed.size();
    std::vector<int> widths(n, kMinColumn);
    if (n == 0) {
        return widths;
    }
    const int gapTotal = gap * static_cast<int>(n - 1);
    int       budget   = (avail > 0) ? (avail - gapTotal) : static_cast<int>(n) * 8;
    budget             = std::max(static_cast<int>(n) * kMinColumn, budget);

    int    fixedSum  = 0;
    size_t flexCount = 0;
    for (size_t i = 0; i < n; ++i) {
        if (fixed[i] > 0) {
            fixedSum += fixed[i];
        } else {
            ++flexCount;
        }
    }
    const int flexReserve = static_cast<int>(flexCount) * kMinColumn;
    if (fixedSum + flexReserve > budget) {
        const int room = budget - flexReserve;
        if (room < static_cast<int>(n) * kMinColumn) {
            const int each = std::max(1, budget / static_cast<int>(n));
            std::fill(widths.begin(), widths.end(), each);
            return widths;
        }
        const double scale = static_cast<double>(room) / static_cast<double>(fixedSum);
        int          used  = 0;
        for (size_t i = 0; i < n; ++i) {
            if (fixed[i] > 0) {
                widths[i]
                    = std::max(kMinColumn, static_cast<int>(static_cast<double>(fixed[i]) * scale));
                used += widths[i];
            }
        }
        // 收缩产生的舍入余量: 从后往前扣到下限为止
        int over = used - room;
        for (size_t i = n; i-- > 0 && over > 0;) {
            if (fixed[i] <= 0) {
                continue;
            }
            const int delta = std::min(over, widths[i] - kMinColumn);
            widths[i] -= delta;
            over -= delta;
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            widths[i] = (fixed[i] > 0) ? fixed[i] : kMinColumn;
        }
    }

    int assigned = 0;
    for (int w : widths) {
        assigned += w;
    }
    const int leftover = budget - assigned;
    if (leftover > 0) {
        if (flexCount > 0) {
            const int each = leftover / static_cast<int>(flexCount);
            int       rest = leftover - each * static_cast<int>(flexCount);
            for (size_t i = 0; i < n; ++i) {
                if (fixed[i] > 0) {
                    continue;
                }
                widths[i] += each;
                if (rest > 0) {
                    widths[i] += 1;
                    --rest;
                }
            }
        } else if (stretchAll) {
            // 横向铺满: 剩余宽度均分给所有列 (含声明了固定宽度的列)
            const int each = leftover / static_cast<int>(n);
            int       rest = leftover - each * static_cast<int>(n);
            for (size_t i = 0; i < n; ++i) {
                widths[i] += each;
                if (rest > 0) {
                    widths[i] += 1;
                    --rest;
                }
            }
        } else {
            widths[n - 1] += leftover;
        }
    }
    return widths;
}

/// 表格列描述 → 固定宽度数组 (0 = 自适应)
std::vector<int> tableFixedWidths(const std::vector<agentxx::ui::TableColumn>& columns) {
    std::vector<int> fixed(columns.size(), 0);
    for (size_t i = 0; i < columns.size(); ++i) {
        fixed[i] = columns[i].width;
    }
    return fixed;
}

/// 单元格文本按列宽补齐/截断 (显示列宽口径; 宽字符安全)
std::string cellPadded(std::string_view content, int width, std::string_view align) {
    const std::string trimmed = agentxx::ui::truncateToWidth(content, width);
    const int         used    = agentxx::ui::displayWidth(trimmed);
    const int         pad     = std::max(0, width - used);
    if (align == "right") {
        return spaces(pad) + trimmed;
    }
    if (align == "center") {
        const int left = pad / 2;
        return spaces(left) + trimmed + spaces(pad - left);
    }
    return trimmed + spaces(pad);
}

// ---------------------------------------------------------------------------
// 表格 / 键值对 / 层级列表
// ---------------------------------------------------------------------------

/// 渲染表格为多行元素 (表头 + 分隔线 + 数据行; 单元格可点则登记区域)
Row renderTable(const agentxx::ui::Item& item, const UiRenderCtx& ctx) {
    constexpr int kGap  = 2;
    const auto&   theme = *ctx.theme;
    const int     avail = std::max(1, contentWidth(ctx, item.indent));
    const auto    widths = layoutColumnWidths(tableFixedWidths(item.columns), avail, kGap);

    Elements                 lines;
    std::vector<UiHitRegion> regions;
    int                      y = 0;

    auto emit = [&](const std::vector<std::string>& cells,
                    const std::vector<Color>&       colors,
                    const std::vector<std::string>& actions,
                    const std::vector<Json>&        args) {
        Elements chunks;
        int      x = 0;
        for (size_t c = 0; c < item.columns.size(); ++c) {
            if (c > 0) {
                chunks.push_back(text(spaces(kGap)));
                x += kGap;
            }
            const std::string cell = (c < cells.size()) ? cells[c] : std::string{};
            chunks.push_back(text(cell) | color(colors[c]));
            if (c < actions.size() && !actions[c].empty()) {
                UiHitRegion region;
                region.x       = x;
                region.y       = y;
                region.w       = widths[c];
                region.h       = 1;
                region.id      = actions[c];
                region.arg     = (c < args.size() && !args[c].is_null()) ? args[c].dump() : "{}";
                region.plugin  = ctx.plugin;
                region.ownerId = ctx.ownerId;
                regions.push_back(std::move(region));
            }
            x += widths[c];
        }
        lines.push_back(hbox(std::move(chunks)));
        ++y;
    };

    const size_t columnCount = item.columns.size();
    if (item.header && columnCount > 0) {
        std::vector<std::string> cells;
        std::vector<Color>       colors;
        for (size_t c = 0; c < columnCount; ++c) {
            cells.push_back(cellPadded(item.columns[c].title, widths[c], item.columns[c].align));
            colors.push_back(theme.accentColor);
        }
        emit(
            cells,
            colors,
            std::vector<std::string>(columnCount),
            std::vector<Json>(columnCount)
        );
        std::string sepLine;
        for (size_t c = 0; c < columnCount; ++c) {
            if (c > 0) {
                sepLine += spaces(kGap);
            }
            sepLine += std::string(static_cast<size_t>(std::max(0, widths[c])), '-');
        }
        lines.push_back(text(sepLine) | color(theme.hintColor) | theme.dim());
        ++y;
    }

    for (const auto& rowCells : item.rows) {
        std::vector<std::string> cells;
        std::vector<Color>       colors;
        std::vector<std::string> actions;
        std::vector<Json>        args;
        for (size_t c = 0; c < columnCount; ++c) {
            const agentxx::ui::TableCell* cell = (c < rowCells.size()) ? &rowCells[c] : nullptr;
            const std::string             raw  = (cell != nullptr) ? cell->text : std::string{};
            cells.push_back(cellPadded(raw, widths[c], item.columns[c].align));
            const std::string colorName
                = (cell != nullptr && !cell->color.empty()) ? cell->color : item.columns[c].color;
            colors.push_back(itemColor(colorName, theme));
            actions.push_back((cell != nullptr && !cell->action.empty()) ? cell->action
                                                                        : std::string{});
            args.push_back((cell != nullptr) ? cell->args : Json{});
        }
        emit(cells, colors, actions, args);
    }

    Row row;
    if (lines.empty()) {
        row.element = text(" ");
        return row;
    }
    row.element = vbox(std::move(lines));
    row.lines   = static_cast<size_t>(std::max(1, y));
    row.regions = std::move(regions);
    return row;
}

/// 键值对 (键列按最长键补齐 + 值列截断)
Row renderKeyValue(const agentxx::ui::Item& item, const UiRenderCtx& ctx) {
    const auto& theme = *ctx.theme;
    const int   avail = std::max(1, contentWidth(ctx, item.indent));
    size_t      keyW  = static_cast<size_t>(std::max(0, item.keyWidth));
    for (const auto& p : item.pairs) {
        keyW = std::max(keyW, static_cast<size_t>(agentxx::ui::displayWidth(p.key)));
    }
    const int sepW   = agentxx::ui::displayWidth(item.sep);
    const int valueW = std::max(1, avail - static_cast<int>(keyW) - sepW);

    Elements lines;
    for (const auto& p : item.pairs) {
        lines.push_back(hbox({
            text(agentxx::ui::padRightToWidth(p.key, static_cast<int>(keyW)))
                | color(itemColor(p.keyColor, theme)),
            text(item.sep) | color(theme.hintColor),
            text(agentxx::ui::truncateToWidth(p.value, valueW))
                | color(itemColor(p.valueColor, theme)),
        }));
    }
    Row row;
    if (lines.empty()) {
        row.element = text(" ");
        return row;
    }
    row.lines   = lines.size(); // 注意: 须在 move 之前取行数
    row.element = vbox(std::move(lines));
    return row;
}

/// 层级列表 (连接线 + 缩进; 节点可点则登记整行区域)
Row renderTree(const agentxx::ui::Item& item, const UiRenderCtx& ctx) {
    const auto&              theme = *ctx.theme;
    Elements                 lines;
    std::vector<UiHitRegion> regions;
    int                      y = 0;

    std::function<void(const std::vector<agentxx::ui::TreeNode>&, const std::string&)> emit =
        [&](const std::vector<agentxx::ui::TreeNode>& nodes, const std::string& prefix) {
            for (size_t i = 0; i < nodes.size(); ++i) {
                const auto&       node = nodes[i];
                const bool        last = (i + 1 == nodes.size());
                const std::string line = item.connector
                                             ? prefix + (last ? "└─ " : "├─ ") + node.label
                                             : prefix + node.label;
                lines.push_back(text(line) | color(itemColor(node.color, theme)));
                if (!node.action.empty()) {
                    UiHitRegion region;
                    region.x       = 0;
                    region.y       = y;
                    region.w       = 0; // 整行可点
                    region.h       = 1;
                    region.id      = node.action;
                    region.arg     = node.args.is_null() ? "{}" : node.args.dump();
                    region.plugin  = ctx.plugin;
                    region.ownerId = ctx.ownerId;
                    regions.push_back(std::move(region));
                }
                ++y;
                if (!node.children.empty()) {
                    emit(node.children, item.connector ? prefix + (last ? "   " : "│  ") : prefix);
                }
            }
        };
    emit(item.nodes, "");

    Row row;
    if (lines.empty()) {
        row.element = text(" ");
        return row;
    }
    row.element = vbox(std::move(lines));
    row.lines   = static_cast<size_t>(std::max(1, y));
    row.regions = std::move(regions);
    return row;
}

// ---------------------------------------------------------------------------
// 图表
// ---------------------------------------------------------------------------

const char* const kSparkBlocks[8] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
const char* const kBarBlocks[8]   = {"▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};

/// 按目标宽度对数据做分桶平均 (数据点多于宽度时压缩)
std::vector<double> bucketize(const std::vector<double>& data, int buckets) {
    if (buckets <= 0 || data.size() <= static_cast<size_t>(buckets)) {
        return data;
    }
    std::vector<double> out;
    out.reserve(static_cast<size_t>(buckets));
    const double step = static_cast<double>(data.size()) / static_cast<double>(buckets);
    for (int b = 0; b < buckets; ++b) {
        const size_t from = static_cast<size_t>(static_cast<double>(b) * step);
        size_t       to   = static_cast<size_t>(static_cast<double>(b + 1) * step);
        if (to <= from) {
            to = from + 1;
        }
        to = std::min(to, data.size());
        double sum = 0.0;
        for (size_t i = from; i < to; ++i) {
            sum += data[i];
        }
        out.push_back(sum / static_cast<double>(std::max<size_t>(1, to - from)));
    }
    return out;
}

/// 迷你趋势图 (高度 1 = 单行块字符; 高度 > 1 = 多行纵向分辨率)
Row renderSparkline(const agentxx::ui::Item& item, const UiRenderCtx& ctx) {
    const auto& theme  = *ctx.theme;
    const int   avail  = std::max(1, contentWidth(ctx, item.indent));
    const int   labelW = item.label.empty() ? 0 : agentxx::ui::displayWidth(item.label) + 1;
    const int   height = std::clamp(item.height, 1, 8);

    Row row;
    if (item.data.empty()) {
        Elements els;
        if (!item.label.empty()) {
            els.push_back(text(item.label + " ") | color(theme.hintColor));
        }
        els.push_back(text("[sparkline]") | color(theme.hintColor) | theme.dim());
        row.element = hbox(std::move(els));
        return row;
    }

    const bool hasSuffix = item.showLast || !item.unit.empty();
    const int  suffixW   = hasSuffix
                               ? (agentxx::ui::displayWidth(numText(item.data.back()) + item.unit)
                                  + 1)
                               : 0;
    const int  plotW     = std::max(4, avail - labelW - suffixW);
    const auto data      = bucketize(item.data, plotW);
    const auto minIt     = std::min_element(data.begin(), data.end());
    const auto maxIt     = std::max_element(data.begin(), data.end());
    const double lo      = item.hasMin ? item.minValue : *minIt;
    const double hi      = item.hasMax ? item.maxValue : *maxIt;
    const double span    = (hi > lo) ? (hi - lo) : 0.0;
    const double last    = item.data.back();

    auto colorAt = [&](size_t i) -> Color {
        const double r = (span > 0) ? ((data[i] - lo) / span) : 0.5;
        if (!item.colors.empty()) {
            const size_t idx = std::min(
                item.colors.size() - 1,
                static_cast<size_t>(
                    std::clamp(r, 0.0, 0.9999) * static_cast<double>(item.colors.size())
                )
            );
            return itemColor(item.colors[idx], theme);
        }
        return itemColor(item.color, theme);
    };

    constexpr int kLevelsPerRow = 8;
    const int     totalLevels   = height * kLevelsPerRow;
    const bool    barStyle      = (item.sparkStyle == "bar");
    Elements      lines;
    for (int rowIdx = 0; rowIdx < height; ++rowIdx) {
        Elements chunks;
        if (rowIdx == 0) {
            if (!item.label.empty()) {
                chunks.push_back(text(item.label + " ") | color(theme.hintColor));
            }
        } else if (labelW > 0) {
            chunks.push_back(text(spaces(labelW)));
        }
        for (size_t i = 0; i < data.size(); ++i) {
            const double r = (span > 0) ? std::clamp((data[i] - lo) / span, 0.0, 1.0) : 0.5;
            const int    level = std::clamp(
                static_cast<int>(r * static_cast<double>(totalLevels - 1) + 0.5),
                0,
                totalLevels - 1
            );
            const int inRow = std::clamp(
                level - (height - 1 - rowIdx) * kLevelsPerRow + 1,
                0,
                kLevelsPerRow
            );
            if (inRow <= 0) {
                chunks.push_back(text(" "));
                continue;
            }
            const int idx = std::min(7, inRow - 1);
            chunks.push_back(
                text(barStyle ? kBarBlocks[idx] : kSparkBlocks[idx]) | color(colorAt(i))
            );
        }
        if (rowIdx == 0 && hasSuffix) {
            chunks.push_back(text(" " + numText(last) + item.unit) | color(theme.hintColor));
        }
        lines.push_back(hbox(std::move(chunks)));
    }
    row.lines   = static_cast<size_t>(height);
    row.element = (lines.size() == 1) ? std::move(lines[0]) : vbox(std::move(lines));
    return row;
}

/// 条形计量 (填充块 + 背景块 + 数值文本; 阈值配色)
Row renderMeter(const agentxx::ui::Item& item, const UiRenderCtx& ctx) {
    const auto&  theme  = *ctx.theme;
    const int    avail  = std::max(1, contentWidth(ctx, item.indent));
    const int    labelW = item.label.empty() ? 0 : agentxx::ui::displayWidth(item.label) + 1;
    const int    barW   = (item.width > 0) ? item.width
                                           : std::max(6, std::min(24, avail - labelW - 8));
    const double total  = (item.total > 0) ? item.total : 100.0;
    const double ratio  = std::clamp(item.value / total, 0.0, 1.0);
    const int    filled = static_cast<int>(ratio * static_cast<double>(barW) + 0.5);

    // 阈值配色: 由高到低匹配首个满足项 (解析时已按阈值降序)
    std::string fillColor = item.color.empty() ? "accent" : item.color;
    for (const auto& th : item.thresholds) {
        if (item.value >= th.at) {
            fillColor = th.color;
            break;
        }
    }

    Elements els;
    if (!item.label.empty()) {
        els.push_back(text(item.label + " ") | color(theme.hintColor));
    }
    Elements barChunks;
    barChunks.push_back(text("▕") | color(theme.hintColor));
    if (filled > 0) {
        barChunks.push_back(
            text(std::string(static_cast<size_t>(filled), ' '))
            | bgcolor(itemColor(fillColor, theme))
        );
    }
    if (filled < barW) {
        barChunks.push_back(
            text(std::string(static_cast<size_t>(barW - filled), ' '))
            | bgcolor(theme.blockColor) | theme.dim()
        );
    }
    barChunks.push_back(text("▏") | color(theme.hintColor));
    els.push_back(hbox(std::move(barChunks)));
    if (item.showValue) {
        std::string valueText = numText(ratio * 100.0);
        valueText += item.unit.empty() ? "%" : item.unit;
        els.push_back(text(" " + valueText) | color(theme.normalColor));
    }
    Row row;
    row.element = hbox(std::move(els));
    return row;
}

// ---------------------------------------------------------------------------
// 交互控件与表单
// ---------------------------------------------------------------------------

/// 控件编辑文本 (表单状态已初始化时优先, 否则取描述缺省值)
std::string controlEditText(const agentxx::ui::Item& item, const UiFormState* form) {
    if (form != nullptr) {
        if (const auto* state = form->find(item.id);
            state != nullptr && state->initialized && state->edited) {
            return state->editText;
        }
    }
    if (item.defaultValue.is_string()) {
        return item.defaultValue.get<std::string>();
    }
    if (item.defaultValue.is_number()) {
        return numText(item.defaultValue.get<double>());
    }
    if (item.defaultValue.is_boolean()) {
        return item.defaultValue.get<bool>() ? "true" : "false";
    }
    return {};
}

/// 控件选中下标
int controlSelected(const agentxx::ui::Item& item, const UiFormState* form) {
    if (form != nullptr) {
        if (const auto* state = form->find(item.id); state != nullptr && state->initialized) {
            return state->selected;
        }
    }
    return optionIndex(item.options, item.defaultValue);
}

/// 控件勾选状态
bool controlChecked(const agentxx::ui::Item& item, const UiFormState* form) {
    if (form != nullptr) {
        if (const auto* state = form->find(item.id); state != nullptr && state->initialized) {
            return state->checked;
        }
    }
    return item.defaultValue.is_boolean() && item.defaultValue.get<bool>();
}

/// 输入框显示宽度 (" " + 值 + 右侧补齐 + " "; 最小 4 列, 与命中区域宽度一致)
int inputFieldWidth(std::string_view value) {
    return std::max(4, agentxx::ui::displayWidth(value) + 2);
}

/// 输入框元素 (背景填充表示可编辑; 获得键盘焦点时加粗下划线)
Element inputField(std::string value, const agentxx::ui::Item& item, const UiRenderCtx& ctx) {
    const auto& theme = *ctx.theme;
    const int   width = inputFieldWidth(value);
    const int   pad   = std::max(0, width - 2 - agentxx::ui::displayWidth(value));
    Element     el    = text(" " + value + std::string(static_cast<size_t>(pad), ' ') + " ")
                    | bgcolor(theme.inputBgColor)
                    | color(value.empty() ? theme.hintColor : theme.inputTextColor);
    if (ctx.form != nullptr && ctx.form->focusedId == item.id) {
        el = el | bold | underlined;
    }
    return el | xflex_shrink;
}

/// 数值控件的步进按钮 ("[ - ]" / "[ + ]")
Element inputStepButton(std::string_view label, const TUITheme& theme) {
    return text(label) | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
}

/// 追加控件区域
void addControlRegion(
    std::vector<UiHitRegion>& regions,
    const UiRenderCtx&        ctx,
    const agentxx::ui::Item&  item,
    int                       x,
    int                       y,
    int                       w,
    int                       sub
) {
    UiHitRegion region;
    region.kind    = UiHitRegionKind::Form;
    region.x       = x;
    region.y       = y;
    region.w       = w;
    region.h       = 1;
    region.id      = item.id;
    region.sub     = sub;
    region.plugin  = ctx.plugin;
    region.ownerId = ctx.ownerId;
    regions.push_back(std::move(region));
}

/// 提交与取消区域 (id 固定为 __submit / __cancel)
void addSubmitRegions(
    std::vector<UiHitRegion>& regions,
    const UiRenderCtx&        ctx,
    int                       confirmW,
    int                       cancelX,
    int                       cancelW
) {
    auto add = [&](std::string_view id, int x, int w) {
        UiHitRegion region;
        region.kind    = UiHitRegionKind::FormSubmit;
        region.x       = x;
        region.y       = 0;
        region.w       = w;
        region.h       = 1;
        region.id      = std::string{id};
        region.arg     = "{}";
        region.plugin  = ctx.plugin;
        region.ownerId = ctx.ownerId;
        regions.push_back(std::move(region));
    };
    add(kFormSubmitActionId, 0, confirmW);
    add(kFormCancelActionId, cancelX, cancelW);
}

/// 提交行标签 (描述未声明时取界面语言默认文案)
std::string submitLabelOf(const agentxx::ui::Item& item) {
    return item.label.empty() ? std::string{tr("ui.submit")} : item.label;
}

std::string cancelLabelOf(const agentxx::ui::Item& item) {
    return item.cancelLabel.empty() ? std::string{tr("ui.cancel")} : item.cancelLabel;
}

// ---------------------------------------------------------------------------
// 主渲染分发
// ---------------------------------------------------------------------------

Rows renderItemRows(const agentxx::ui::Item& item, const UiRenderCtx& ctx, UiRenderResult& out);

/// 渲染子项列表 (返回中间行模型; 容器统一偏移区域坐标并附加命中框)
Rows renderChildren(
    const std::vector<agentxx::ui::Item>& items,
    const UiRenderCtx&                    ctx,
    UiRenderResult&                       out
) {
    Rows rows;
    for (const auto& child : items) {
        auto sub = renderItemRows(child, ctx, out);
        rows.insert(
            rows.end(),
            std::make_move_iterator(sub.begin()),
            std::make_move_iterator(sub.end())
        );
    }
    return rows;
}

/// 子项渲染上下文 (指定可用宽度与缩进层数)
UiRenderCtx childCtx(const UiRenderCtx& ctx, int width, int indent) {
    UiRenderCtx sub = ctx;
    sub.width       = width;
    sub.indent      = indent;
    return sub;
}

/// 文本类降级行 (fallback / canvas 无内容时的兜底)
Rows fallbackRows(
    const agentxx::ui::Item& item,
    const UiRenderCtx&       ctx,
    UiRenderResult&          out,
    std::string_view         text0 = {}
) {
    agentxx::ui::Item textItem;
    textItem.kind   = "text";
    textItem.text   = text0.empty() ? item.fallback : std::string{text0};
    textItem.color  = "hint";
    textItem.dim    = true;
    textItem.wrap   = true;
    textItem.indent = item.indent;
    if (textItem.text.empty()) {
        return {};
    }
    return renderItemRows(textItem, ctx, out);
}

/// 文本 + 按钮合并为一行 (返回 false 表示无法合并, 调用方按普通顺序渲染)
///
/// 触发条件: 相邻两项分别是 text 与 button, 且各自都只产出一行。
bool mergeTextButton(
    const agentxx::ui::Item& textItem,
    const agentxx::ui::Item& buttonItem,
    const UiRenderCtx&       ctx,
    UiRenderResult&          out
) {
    UiRenderResult left;
    renderItem(textItem, ctx, left);
    if (left.rows.size() != 1) {
        return false;
    }
    UiRenderCtx buttonCtx = ctx;
    buttonCtx.indent      = 0; // 按钮紧跟文本之后, 不再重复基础缩进
    UiRenderResult right;
    renderItem(buttonItem, buttonCtx, right);
    if (right.rows.size() != 1) {
        return false;
    }

    // 合并后各区域仍用合并行的局部坐标: 按钮侧区域按文本宽度右移
    const int prefixCols = ctx.indent + std::max(0, textItem.indent)
                           + agentxx::ui::displayWidth(textItem.text);
    UiRow row;
    row.lines   = 1;
    row.element = hbox({std::move(left.rows[0].element), std::move(right.rows[0].element)});
    row.regions = std::move(left.rows[0].regions);
    for (auto& region : right.rows[0].regions) {
        row.regions.push_back(region.offsetBy(prefixCols, 0));
    }
    for (auto& builder : left.builders) {
        out.builders.push_back(std::move(builder));
    }
    for (auto& builder : right.builders) {
        out.builders.push_back(std::move(builder));
    }
    if (!row.regions.empty()) {
        auto box    = std::make_shared<ftxui::Box>(kNoBox);
        row.box     = box;
        row.element = std::move(row.element) | reflect(*box);
    }
    out.rows.push_back(std::move(row));
    return true;
}

Rows renderItemRows(const agentxx::ui::Item& item, const UiRenderCtx& ctx, UiRenderResult& out) {
    Rows rows;
    if (ctx.theme == nullptr) {
        return rows;
    }
    const auto& theme = *ctx.theme;

    // 未知 kind: 降级为 fallback 文本 (无 fallback 则跳过)
    if (!item.known) {
        return fallbackRows(item, ctx, out);
    }

    const std::string pad     = spaces(ctx.indent + std::max(0, item.indent));
    const int         padCols = static_cast<int>(pad.size());
    auto              padRow  = [&](Element el) -> Element {
        return pad.empty() ? std::move(el) : hbox({text(pad), std::move(el)});
    };
    auto pushPlain = [&](Element el, size_t lines, std::vector<UiHitRegion> regions = {}) {
        Row row;
        row.element = padRow(std::move(el));
        row.lines   = std::max<size_t>(1, lines);
        for (auto& region : regions) {
            region.x += padCols;
            row.regions.push_back(std::move(region));
        }
        rows.push_back(std::move(row));
    };

    // ---------------- 文本 ----------------
    if (item.kind == "text") {
        const Color textColor  = itemColor(item.color, theme);
        const auto  lines      = textLines(item, ctx);
        const bool  actionable = !item.action.empty();
        for (size_t i = 0; i < lines.size(); ++i) {
            Element el = text(lines[i]) | color(textColor);
            if (item.bold) {
                el = el | bold;
            }
            if (item.dim) {
                el = el | theme.dim();
            }
            if (!item.wrap) {
                el = el | xflex_shrink;
            }
            std::vector<UiHitRegion> regions;
            if (actionable && i == 0) {
                UiHitRegion region;
                region.x       = 0;
                region.y       = 0;
                region.w       = 0;
                region.h       = static_cast<int>(lines.size());
                region.id      = item.action;
                region.arg     = item.args.is_null() ? "{}" : item.args.dump();
                region.plugin  = ctx.plugin;
                region.ownerId = ctx.ownerId;
                regions.push_back(std::move(region));
            }
            pushPlain(std::move(el), 1, std::move(regions));
        }
        return rows;
    }

    // ---------------- markdown ----------------
    if (item.kind == "markdown") {
        if (item.text.empty()) {
            return rows;
        }
        auto [el, builder] = renderMarkdown(
            item.text,
            itemColor(item.color, theme),
            theme.markdownTheme,
            contentWidth(ctx, item.indent)
        );
        if (builder) {
            out.builders.push_back(std::move(builder));
        }
        pushPlain(
            std::move(el) | xflex_shrink,
            estimateMarkdownLines(item.text, contentWidth(ctx, item.indent))
        );
        return rows;
    }

    // ---------------- diff ----------------
    if (item.kind == "diff") {
        pushPlain(
            renderPluginDiff(item.path, item.oldStr, item.newStr, theme, ctx.width),
            diffBlockLines(item, ctx)
        );
        return rows;
    }

    // ---------------- separator / gap ----------------
    if (item.kind == "separator") {
        if (ctx.separatorStyle == UiSeparatorStyle::Block) {
            // 面性风格: 分隔用整行浅色背景区块 (不画横线)
            // 背景色须施加在整行元素上 (元素被分配整行宽度, 装饰器按整框着色)
            Row row;
            row.element = padRow(text(" ")) | bgcolor(theme.surfaceFooterColor);
            rows.push_back(std::move(row));
        } else {
            pushPlain(text("─") | color(theme.hintColor) | theme.dim() | xflex_shrink, 1);
        }
        return rows;
    }
    if (item.kind == "gap") {
        for (int i = 0; i < item.lines; ++i) {
            pushPlain(text(" "), 1);
        }
        return rows;
    }

    // ---------------- badge ----------------
    if (item.kind == "badge") {
        pushPlain(text("● " + item.text) | color(itemColor(item.color, theme)), 1);
        return rows;
    }

    // ---------------- diagram ----------------
    if (item.kind == "diagram") {
        auto [el, lines] = buildDiagram(item, ctx);
        if (lines == 0) {
            return rows;
        }
        pushPlain(std::move(el) | flex, lines);
        return rows;
    }

    // ---------------- button ----------------
    if (item.kind == "button") {
        const std::string        label = item.label.empty() ? std::string{"Button"} : item.label;
        Element                  el    = buttonElement(label, item.color, item.style, theme);
        std::vector<UiHitRegion> regions;
        const bool               actionable
            = !item.action.empty()
              && (ctx.registry == nullptr || hasPluginBinding(ctx.plugin, ctx.registry));
        if (actionable) {
            UiHitRegion region;
            region.x       = 0;
            region.y       = 0;
            region.w       = buttonWidth(label);
            region.h       = 1;
            region.id      = item.action;
            region.arg     = item.args.is_null() ? "{}" : item.args.dump();
            region.plugin  = ctx.plugin;
            region.ownerId = ctx.ownerId;
            regions.push_back(std::move(region));
        }
        if (!item.prefix.empty()) {
            el = hbox({text(item.prefix) | color(theme.normalColor), std::move(el)});
        }
        pushPlain(std::move(el) | xflex_shrink, 1, std::move(regions));
        return rows;
    }

    // ---------------- 结构化与图表组件 ----------------
    if (item.kind == "meter") {
        pushPlain(renderMeter(item, ctx).element, 1);
        return rows;
    }
    if (item.kind == "sparkline") {
        auto row = renderSparkline(item, ctx);
        pushPlain(row.element, row.lines);
        return rows;
    }
    if (item.kind == "kv") {
        auto row = renderKeyValue(item, ctx);
        pushPlain(row.element, row.lines);
        return rows;
    }
    if (item.kind == "table") {
        auto row = renderTable(item, ctx);
        pushPlain(row.element, row.lines, std::move(row.regions));
        return rows;
    }
    if (item.kind == "tree") {
        auto row = renderTree(item, ctx);
        pushPlain(row.element, row.lines, std::move(row.regions));
        return rows;
    }

    // ---------------- row (横向组合) ----------------
    if (item.kind == "row") {
        const int n = static_cast<int>(item.items.size());
        if (n == 0) {
            return rows;
        }
        const int        avail = std::max(1, contentWidth(ctx, item.indent));
        std::vector<int> fixed(static_cast<size_t>(n), 0);
        for (int i = 0; i < n; ++i) {
            fixed[static_cast<size_t>(i)] = item.items[static_cast<size_t>(i)].columnWidth;
        }
        const auto widths = layoutColumnWidths(fixed, avail, item.gap, item.align == "stretch");

        Elements columnEls;
        Rows     columns;
        int      maxLines = 1;
        for (int i = 0; i < n; ++i) {
            const int w = std::max(1, widths[static_cast<size_t>(i)]);
            // 列内子项按列宽渲染 (缩进为 0: 本行的基础缩进由 pushPlain 叠加)
            auto subRows = renderItemRows(
                item.items[static_cast<size_t>(i)],
                childCtx(ctx, w, 0),
                out
            );
            Row     columnRow = stackRows(subRows);
            Element el        = std::move(columnRow.element);
            if (item.align == "right") {
                el = hbox({filler(), std::move(el)});
            } else if (item.align == "center") {
                el = hbox({filler(), std::move(el), filler()});
            }
            columnEls.push_back(std::move(el) | size(WIDTH, EQUAL, w));
            maxLines = std::max(maxLines, static_cast<int>(columnRow.lines));
            columns.push_back(std::move(columnRow));
            if (i + 1 < n && item.gap > 0) {
                columnEls.push_back(text(spaces(item.gap)));
            }
        }

        std::vector<UiHitRegion> regions;
        int                      offsetX = 0;
        for (int i = 0; i < n; ++i) {
            for (const auto& region : columns[static_cast<size_t>(i)].regions) {
                regions.push_back(region.offsetBy(offsetX, 0));
            }
            offsetX += widths[static_cast<size_t>(i)] + item.gap;
        }
        pushPlain(hbox(std::move(columnEls)), static_cast<size_t>(maxLines), std::move(regions));
        return rows;
    }

    // ---------------- box (分组框) ----------------
    if (item.kind == "box") {
        const bool border      = item.border != "none";
        const int  innerPadCols = item.pad * 2;
        const int  innerW
            = std::max(1, contentWidth(ctx, item.indent) - (border ? 2 : 0) - innerPadCols);
        auto inner = stackRows(renderChildren(item.items, childCtx(ctx, innerW, 0), out));
        if (item.pad > 0 && innerPadCols > 0) {
            inner.element = hbox({text(spaces(innerPadCols)), std::move(inner.element)});
        }
        Element boxEl = std::move(inner.element);
        if (item.pad > 0) {
            Elements withPad;
            for (int i = 0; i < item.pad; ++i) {
                withPad.push_back(text(" "));
            }
            withPad.push_back(std::move(boxEl));
            for (int i = 0; i < item.pad; ++i) {
                withPad.push_back(text(" "));
            }
            boxEl = vbox(std::move(withPad));
        }
        if (border) {
            BorderStyle style = BorderStyle::LIGHT;
            if (item.border == "round") {
                style = BorderStyle::ROUNDED;
            } else if (item.border == "square") {
                style = BorderStyle::DOUBLE;
            }
            if (item.title.empty()) {
                boxEl = std::move(boxEl) | borderStyled(style) | color(theme.hintColor);
            } else {
                const Color titleColor = item.titleColor.empty()
                                             ? theme.accentColor
                                             : itemColor(item.titleColor, theme);
                boxEl = window(text(item.title) | color(titleColor), std::move(boxEl), style)
                        | color(theme.hintColor);
            }
        }
        const int    dx    = (border ? 1 : 0) + innerPadCols;
        const int    dy    = (border ? 1 : 0) + item.pad;
        const size_t lines = inner.lines + static_cast<size_t>(item.pad * 2)
                             + (border ? 2u : 0u);
        std::vector<UiHitRegion> regions;
        for (const auto& region : inner.regions) {
            regions.push_back(region.offsetBy(dx, dy));
        }
        pushPlain(std::move(boxEl), lines, std::move(regions));
        return rows;
    }

    // ---------------- collapse (可折叠分组) ----------------
    if (item.kind == "collapse") {
        bool expanded = item.expanded;
        if (ctx.collapseExpanded) {
            expanded = ctx.collapseExpanded(item.id, item.expanded);
        }
        Element header = hbox({
            text(expanded ? "▾ " : "▸ ") | color(theme.accentColor),
            text(item.title) | color(theme.accentColor) | bold,
        });
        std::vector<UiHitRegion> regions;
        if (!item.id.empty()) {
            UiHitRegion region;
            region.kind    = UiHitRegionKind::Collapse;
            region.x       = 0;
            region.y       = 0;
            region.w       = 0; // 整行可点
            region.h       = 1;
            region.id      = item.id;
            region.plugin  = ctx.plugin;
            region.ownerId = ctx.ownerId;
            regions.push_back(std::move(region));
        }
        pushPlain(std::move(header), 1, std::move(regions));
        if (expanded) {
            // 子项自带缩进 (与标题右移 2 列对齐), 直接追加不额外补白
            auto sub = renderChildren(
                item.items,
                childCtx(ctx, ctx.width, ctx.indent + item.indent + 2),
                out
            );
            for (auto& row : sub) {
                rows.push_back(std::move(row));
            }
        }
        return rows;
    }

    // ---------------- control (交互控件) ----------------
    if (item.kind == "control") {
        const bool interactive = ctx.form != nullptr;
        const bool inlineLabel = (item.control == "checkbox");
        if (!item.controlLabel.empty() && !inlineLabel) {
            pushPlain(text(item.controlLabel) | color(theme.accentColor) | bold, 1);
        }
        if (!item.help.empty()) {
            const int helpW = std::max(1, contentWidth(ctx, item.indent));
            for (const auto& line : wrapTextToLines(item.help, helpW)) {
                pushPlain(text(line) | color(theme.hintColor) | theme.dim(), 1);
            }
        }
        const std::string caption = item.controlLabel.empty() ? item.id : item.controlLabel;
        if (item.control == "checkbox") {
            const bool checked = controlChecked(item, ctx.form);
            Element mark = text(checked ? "[ ✓ ] " : "[   ] ")
                           | color(checked ? theme.accentColor : theme.hintColor);
            if (checked) {
                mark = mark | bold;
            }
            Element el = hbox({std::move(mark), text(caption) | color(theme.normalColor)});
            std::vector<UiHitRegion> regions;
            if (interactive && !item.id.empty()) {
                addControlRegion(regions, ctx, item, 0, 0, 0, 0);
            }
            pushPlain(std::move(el), 1, std::move(regions));
        } else if (item.control == "text") {
            Element el = inputField(controlEditText(item, ctx.form), item, ctx);
            std::vector<UiHitRegion> regions;
            if (interactive && !item.id.empty()) {
                addControlRegion(regions, ctx, item, 0, 0, 0, 0);
            }
            pushPlain(hbox({std::move(el)}), 1, std::move(regions));
        } else if (item.control == "number") {
            const std::string value   = controlEditText(item, ctx.form);
            constexpr int     kMinusW = 5; // "[ - ]"
            const int         valueW  = inputFieldWidth(value);
            Element           el      = hbox({
                inputStepButton("[ - ]", theme),
                text(" "),
                inputField(value, item, ctx),
                text(" "),
                inputStepButton("[ + ]", theme),
            });
            std::vector<UiHitRegion> regions;
            if (interactive && !item.id.empty()) {
                addControlRegion(regions, ctx, item, 0, 0, kMinusW, 0);
                addControlRegion(regions, ctx, item, kMinusW + 1, 0, valueW, 2);
                addControlRegion(regions, ctx, item, kMinusW + 1 + valueW + 1, 0, kMinusW, 1);
            }
            pushPlain(std::move(el), 1, std::move(regions));
        } else if (item.control == "buttons") {
            const int                selected = controlSelected(item, ctx.form);
            Elements                 els;
            std::vector<UiHitRegion> regions;
            int                      x = 0;
            for (size_t i = 0; i < item.options.size(); ++i) {
                const auto&       opt      = item.options[i];
                const std::string optLabel = opt.label.empty() ? caption : opt.label;
                if (i > 0) {
                    els.push_back(text(" "));
                    ++x;
                }
                els.push_back(buttonElement(
                    optLabel,
                    (static_cast<int>(i) == selected) ? std::string{"accent"} : opt.color,
                    "solid",
                    theme
                ));
                if (interactive && !item.id.empty()) {
                    addControlRegion(
                        regions,
                        ctx,
                        item,
                        x,
                        0,
                        buttonWidth(optLabel),
                        static_cast<int>(i)
                    );
                }
                x += buttonWidth(optLabel);
            }
            if (els.empty()) {
                els.push_back(text(tr("ui.none")) | color(theme.hintColor));
            }
            pushPlain(hbox(std::move(els)), 1, std::move(regions));
        } else if (item.control == "select") {
            const int                selected = controlSelected(item, ctx.form);
            Elements                 els;
            std::vector<UiHitRegion> regions;
            int                      y = 0;
            for (size_t i = 0; i < item.options.size(); ++i) {
                const auto&       opt      = item.options[i];
                const std::string optLabel = opt.label.empty() ? caption : opt.label;
                const bool        active   = (static_cast<int>(i) == selected);
                // 选中项: 指示符 + 反色底 (整行); 未选中: 同宽占位 + 普通色
                Element entry = hbox({
                    text(active ? "▸ " : "  "),
                    text(optLabel),
                });
                entry = active ? entry | bgcolor(theme.buttonActiveBgColor)
                                     | color(theme.buttonActiveTextColor) | bold
                               : entry | color(theme.normalColor);
                els.push_back(std::move(entry));
                if (interactive && !item.id.empty()) {
                    addControlRegion(regions, ctx, item, 0, y, 0, static_cast<int>(i));
                }
                ++y;
            }
            if (els.empty()) {
                els.push_back(text(tr("ui.none")) | color(theme.hintColor));
            }
            pushPlain(
                vbox(std::move(els)),
                static_cast<size_t>(std::max(1, y)),
                std::move(regions)
            );
        } else {
            pushPlain(
                text(fmt::format("[control: {}]", item.control)) | color(theme.hintColor),
                1
            );
        }
        if (ctx.form != nullptr) {
            if (const auto* state = ctx.form->find(item.id);
                state != nullptr && state->initialized && !state->tip.empty()) {
                pushPlain(text(state->tip) | color(theme.errorColor), 1);
            }
        }
        return rows;
    }

    // ---------------- submit (表单提交行) ----------------
    if (item.kind == "submit") {
        const std::string confirm  = submitLabelOf(item);
        const std::string cancel   = cancelLabelOf(item);
        const int         confirmW = buttonWidth(confirm);
        const int         cancelW  = buttonWidth(cancel);
        Elements          els;
        els.push_back(buttonElement(confirm, "accent", "solid", theme));
        els.push_back(text(" "));
        els.push_back(buttonElement(cancel, "", "solid", theme));
        std::vector<UiHitRegion> regions;
        // 只有存在表单状态时才登记提交区域 (没有状态就没有值可提交,
        // 此时提交行是纯展示, 与控件渲染口径一致)
        if (ctx.form != nullptr) {
            addSubmitRegions(regions, ctx, confirmW, confirmW + 1, cancelW);
        }
        pushPlain(hbox(std::move(els)), 1, std::move(regions));
        return rows;
    }

    // ---------------- custom (派发到内置组件) ----------------
    if (item.kind == "custom") {
        if (!item.component.empty() && item.component != "components") {
            Json params = item.props.is_object() ? item.props : Json::object();
            params["kind"] = item.component;
            auto sub       = agentxx::ui::parseItem(params);
            if (sub.known) {
                sub.indent += item.indent;
                return renderItemRows(sub, ctx, out);
            }
        }
        if (!item.items.empty()) {
            return renderChildren(item.items, ctx, out);
        }
        if (!item.fallback.empty()) {
            return fallbackRows(item, ctx, out);
        }
        if (!item.component.empty()) {
            pushPlain(
                text(fmt::format("[custom component: {}]", item.component))
                    | color(theme.hintColor) | theme.dim(),
                1
            );
        }
        return rows;
    }

    // ---------------- canvas (本版不做自绘: 渲染 fallback 文本) ----------------
    if (item.kind == "canvas") {
        return fallbackRows(
            item,
            ctx,
            out,
            item.fallback.empty() ? std::string{"[canvas]"} : item.fallback
        );
    }

    return rows;
}

} // namespace

ftxui::Color itemColor(std::string_view color, const TUITheme& theme) {
    return uiRoleColor(color, theme);
}

void initFormState(UiFormState& form, const std::vector<agentxx::ui::Item>& items) {
    for (const auto& item : items) {
        if (item.kind == "control" && !item.id.empty()) {
            auto& state = form.ensure(item.id);
            if (!state.initialized) {
                state.initialized = true;
                state.selected    = optionIndex(item.options, item.defaultValue);
                state.checked = item.defaultValue.is_boolean() && item.defaultValue.get<bool>();
                state.editText = controlEditText(item, nullptr);
                state.edited   = false;
            }
        }
        if (!item.items.empty()) {
            initFormState(form, item.items);
        }
    }
}

std::vector<std::string> collectControlIds(const std::vector<agentxx::ui::Item>& items) {
    std::vector<std::string> out;
    for (const auto& item : items) {
        if (item.kind == "control" && !item.id.empty()) {
            out.push_back(item.id);
        }
        if (!item.items.empty()) {
            auto sub = collectControlIds(item.items);
            out.insert(out.end(), sub.begin(), sub.end());
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 表单交互
// ---------------------------------------------------------------------------

namespace {

/// 在组件树内按 id 找控件 (含容器内; 找不到返回 nullptr)
const agentxx::ui::Item* findControl(
    const std::vector<agentxx::ui::Item>& items,
    std::string_view                      id
) {
    for (const auto& item : items) {
        if (item.kind == "control" && item.id == id) {
            return &item;
        }
        if (!item.items.empty()) {
            if (const auto* hit = findControl(item.items, id); hit != nullptr) {
                return hit;
            }
        }
    }
    return nullptr;
}

/// 按描述取缺省编辑文本
std::string defaultEditText(const agentxx::ui::Item& item) {
    return controlEditText(item, nullptr);
}

/// 数值文本 → 数值 (解析失败返回缺省值)
double parseNumber(const std::string& text, bool integer, double fallback) {
    if (text.empty()) {
        return fallback;
    }
    char*  end = nullptr;
    double v   = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return fallback;
    }
    return integer ? std::round(v) : v;
}

/// 删除一个 UTF-8 码点 (末尾可能为多字节字符)
void popCodePoint(std::string& text) {
    if (text.empty()) {
        return;
    }
    size_t i = text.size();
    while (i > 0 && (static_cast<unsigned char>(text[i - 1]) & 0xC0) == 0x80) {
        --i;
    }
    if (i > 0) {
        --i;
    }
    text.erase(i);
}

/// 校验单个数值控件 (写 tip; 返回是否通过)
bool validateNumber(const agentxx::ui::Item& item, UiFormControlState& state) {
    const std::string text = state.edited ? state.editText : defaultEditText(item);
    if (text.empty()) {
        state.tip = tr("ui.invalidNumber");
        return false;
    }
    char*  end = nullptr;
    double v   = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        state.tip = item.integer ? std::string{tr("ui.invalidInteger")}
                                 : std::string{tr("ui.invalidNumber")};
        return false;
    }
    if (item.integer && std::fabs(v - std::round(v)) > 1e-9) {
        state.tip = std::string{tr("ui.invalidInteger")};
        return false;
    }
    if (item.hasNumMin && v < item.numMin) {
        state.tip = trf("ui.outOfRange", numText(item.numMin));
        return false;
    }
    if (item.hasNumMax && v > item.numMax) {
        state.tip = trf("ui.outOfRange", numText(item.numMax));
        return false;
    }
    state.tip.clear();
    return true;
}

} // namespace

UiFormAction handleFormControlHit(
    const std::vector<agentxx::ui::Item>& items,
    UiFormState&                          form,
    std::string_view                      controlId,
    int                                   sub
) {
    const agentxx::ui::Item* item = findControl(items, controlId);
    if (item == nullptr || item->id.empty()) {
        return UiFormAction::None;
    }
    auto& state = form.ensure(item->id);
    if (!state.initialized) {
        UiFormState tmp;
        initFormState(tmp, items);
        if (const auto* init = tmp.find(item->id); init != nullptr) {
            state = *init;
        } else {
            state.initialized = true;
        }
    }
    form.focusedId = item->id;
    state.tip.clear();

    if (item->control == "checkbox") {
        state.checked = !state.checked;
        ++form.version;
        return UiFormAction::Changed;
    }
    if (item->control == "buttons" || item->control == "select") {
        if (sub < 0 || sub >= static_cast<int>(item->options.size())) {
            return UiFormAction::None;
        }
        state.selected = sub;
        ++form.version;
        return item->commitOnPick ? UiFormAction::Submit : UiFormAction::Changed;
    }
    if (item->control == "number") {
        if (sub == 2) {
            // 聚焦输入框 (首次聚焦时把缺省值填入编辑文本)
            if (!state.edited) {
                state.editText = defaultEditText(*item);
            }
            ++form.version;
            return UiFormAction::Changed;
        }
        const double step  = (item->step > 0) ? item->step : 1.0;
        const double base  = parseNumber(
            state.edited ? state.editText : defaultEditText(*item),
            item->integer,
            0.0
        );
        double v = base + ((sub == 1) ? step : -step);
        if (item->hasNumMin) {
            v = std::max(v, item->numMin);
        }
        if (item->hasNumMax) {
            v = std::min(v, item->numMax);
        }
        state.editText = numText(v);
        state.edited   = true;
        ++form.version;
        return UiFormAction::Changed;
    }
    if (item->control == "text") {
        if (!state.edited) {
            state.editText = defaultEditText(*item);
        }
        ++form.version;
        return UiFormAction::Changed;
    }
    return UiFormAction::None;
}

UiFormAction handleFormSubmitHit(std::string_view actionId) {
    if (actionId == kFormSubmitActionId) {
        return UiFormAction::Submit;
    }
    if (actionId == kFormCancelActionId) {
        return UiFormAction::Cancel;
    }
    return UiFormAction::None;
}

bool handleFormKeyInput(
    const std::vector<agentxx::ui::Item>& items,
    UiFormState&                          form,
    const ftxui::Event&                   event
) {
    const auto ids = collectControlIds(items);
    if (ids.empty()) {
        return false;
    }
    // Escape: 释放焦点
    if (event == ftxui::Event::Escape) {
        if (form.focusedId.empty()) {
            return false;
        }
        form.focusedId.clear();
        ++form.version;
        return true;
    }
    // Tab / Shift+Tab: 在控件间移动焦点
    if (event == ftxui::Event::Tab || event == ftxui::Event::TabReverse) {
        const bool forward = (event == ftxui::Event::Tab);
        int        current = -1;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] == form.focusedId) {
                current = static_cast<int>(i);
                break;
            }
        }
        const int n   = static_cast<int>(ids.size());
        int       next = forward ? (current + 1) % n : ((current <= 0 ? n : current) - 1);
        form.focusedId = ids[static_cast<size_t>(std::max(0, next))];
        if (auto& state = form.ensure(form.focusedId); !state.initialized) {
            UiFormState tmp;
            initFormState(tmp, items);
            if (const auto* init = tmp.find(form.focusedId); init != nullptr) {
                state = *init;
            }
        }
        ++form.version;
        return true;
    }
    if (form.focusedId.empty()) {
        return false;
    }
    const agentxx::ui::Item* item = findControl(items, form.focusedId);
    if (item == nullptr) {
        return false;
    }
    auto& state = form.ensure(item->id);
    if (!state.initialized) {
        state.initialized = true;
        state.editText    = defaultEditText(*item);
    }

    // 非输入类控件: 方向键/空格改变选中状态 (与中断表单同一套键盘语义)
    if (item->control == "buttons") {
        if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::ArrowRight) {
            const int n = static_cast<int>(item->options.size());
            if (n > 0) {
                const int dir = (event == ftxui::Event::ArrowRight) ? 1 : n - 1;
                state.selected = (state.selected + dir) % n;
            }
            state.tip.clear();
            ++form.version;
            return true;
        }
        return false;
    }
    if (item->control == "select") {
        if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown) {
            const int n     = static_cast<int>(item->options.size());
            const int delta = (event == ftxui::Event::ArrowUp) ? -1 : 1;
            state.selected
                = std::clamp(state.selected + delta, 0, std::max(0, n - 1));
            state.tip.clear();
            ++form.version;
            return true;
        }
        return false;
    }
    if (item->control == "checkbox") {
        if (event.is_character() && event.character() == " ") {
            state.checked = !state.checked;
            state.tip.clear();
            ++form.version;
            return true;
        }
        return false;
    }
    // 输入类控件 (text / number)
    if (item->control != "text" && item->control != "number") {
        return false;
    }
    if (item->control == "number"
        && (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown)) {
        const double step = (item->step > 0) ? item->step : 1.0;
        const double base = parseNumber(
            state.edited ? state.editText : defaultEditText(*item),
            item->integer,
            0.0
        );
        double v = base + ((event == ftxui::Event::ArrowUp) ? step : -step);
        if (item->hasNumMin) {
            v = std::max(v, item->numMin);
        }
        if (item->hasNumMax) {
            v = std::min(v, item->numMax);
        }
        state.editText = numText(v);
        state.edited   = true;
        state.tip.clear();
        ++form.version;
        return true;
    }
    // 单行输入框无光标定位: 左右方向键消费掉, 避免落到其他组件 (如滚动)
    if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::ArrowRight) {
        return true;
    }
    if (event.is_character()) {
        const std::string ch = event.character();
        if (item->control == "number") {
            // 数值框只接受数字与一个小数点/负号
            for (char c : ch) {
                const bool ok = std::isdigit(static_cast<unsigned char>(c)) != 0
                                || c == '.' || c == '-' || c == '+';
                if (!ok) {
                    return true; // 消费但忽略
                }
            }
        }
        if (!state.edited) {
            // 首次输入: 替换缺省值 (与"点击输入框后直接输入"的语义一致)
            state.editText.clear();
            state.edited = true;
        }
        state.editText += ch;
        state.tip.clear();
        ++form.version;
        return true;
    }
    if (event == ftxui::Event::Backspace) {
        state.edited = true;
        popCodePoint(state.editText);
        state.tip.clear();
        ++form.version;
        return true;
    }
    if (event == ftxui::Event::Delete) {
        state.editText.clear();
        state.edited = true;
        state.tip.clear();
        ++form.version;
        return true;
    }
    return false;
}

bool validateForm(const std::vector<agentxx::ui::Item>& items, UiFormState& form) {
    bool ok = true;
    for (const auto& item : items) {
        if (item.kind == "control" && !item.id.empty()) {
            auto& state = form.ensure(item.id);
            if (!state.initialized) {
                state.initialized = true;
                state.editText    = defaultEditText(item);
                state.selected    = optionIndex(item.options, item.defaultValue);
                state.checked
                    = item.defaultValue.is_boolean() && item.defaultValue.get<bool>();
            }
            if (item.control == "number") {
                if (!validateNumber(item, state)) {
                    ok = false;
                }
            } else if ((item.control == "buttons" || item.control == "select")
                       && item.options.empty()) {
                // 无候选项: 该控件的值无法确定, 拒绝提交并提示 (与渲染诊断行一致)
                state.tip = std::string{tr("ui.noOptions")};
                ok        = false;
            }
        }
        if (!item.items.empty() && !validateForm(item.items, form)) {
            ok = false;
        }
    }
    ++form.version;
    return ok;
}

utilxx_base::Json formValues(const std::vector<agentxx::ui::Item>& items, UiFormState& form) {
    utilxx_base::Json values = utilxx_base::Json::object();
    /// 递归收集控件值到同一层对象 (容器内的控件与顶层控件平级)
    std::function<void(const std::vector<agentxx::ui::Item>&)> collect
        = [&](const std::vector<agentxx::ui::Item>& list) {
              for (const auto& item : list) {
                  if (item.kind == "control" && !item.id.empty()) {
                      auto& state = form.ensure(item.id);
                      if (!state.initialized) {
                          initFormState(form, {item});
                      }
                      if (item.control == "checkbox") {
                          values[item.id] = state.checked;
                      } else if (item.control == "buttons" || item.control == "select") {
                          const int idx = std::clamp(
                              state.selected,
                              0,
                              std::max(0, static_cast<int>(item.options.size()) - 1)
                          );
                          if (!item.options.empty()) {
                              values[item.id] = item.options[static_cast<size_t>(idx)].value;
                          }
                      } else if (item.control == "number") {
                          const std::string text
                              = state.edited ? state.editText : defaultEditText(item);
                          const double v = parseNumber(text, item.integer, 0.0);
                          // 整数控件写整数 (与描述声明的取值类型一致), 浮点控件保留小数
                          if (item.integer) {
                              values[item.id] = utilxx_base::Json(static_cast<int64_t>(v));
                          } else {
                              values[item.id] = utilxx_base::Json(v);
                          }
                      } else if (item.control == "text") {                          values[item.id] = state.edited ? state.editText : defaultEditText(item);
                      }
                      // 未知控件形态不参与结果 (渲染为不可交互的诊断行)
                  }
                  if (!item.items.empty()) {
                      collect(item.items);
                  }
              }
          };
    collect(items);
    utilxx_base::Json out = utilxx_base::Json::object();
    out["values"]         = std::move(values);
    return out;
}

std::optional<agentxx::ui::Item>
    itemFromInterruptBlock(const middleware::InterruptUiBlock& block) {
    // 唯一实现在 lib (`agentxx::middleware::itemOf`): 中断描述 → 组件项的映射
    // 由 TUI 渲染、纯文本降级与"构建器拼中断"共用, 避免各接入点各写一份
    return middleware::itemOf(block);
}

void renderItems(
    const std::vector<agentxx::ui::Item>& items,
    const UiRenderCtx&                    ctx,
    UiRenderResult&                       out
) {
    for (size_t i = 0; i < items.size(); ++i) {
        // 文本 + 按钮的隐式同行合并: 历史上面板/Info 段落用
        // `{"kind":"text","text":"|- "}, {"kind":"button",...}` 表达"前缀 + 按钮"
        // (按钮渲染在文本之后), 此处保持该外观, 各接入点行为一致
        if (items[i].kind == "text" && i + 1 < items.size() && items[i + 1].kind == "button") {
            if (mergeTextButton(items[i], items[i + 1], ctx, out)) {
                ++i;
                continue;
            }
        }
        renderItem(items[i], ctx, out);
    }
}

void renderItem(const agentxx::ui::Item& item, const UiRenderCtx& ctx, UiRenderResult& out) {
    auto rows = renderItemRows(item, ctx, out);
    for (auto& row : rows) {
        UiRow outRow;
        outRow.lines = std::max<size_t>(1, row.lines);
        if (!row.regions.empty()) {
            // 顶层行附加反射框: 命中时先定位到行, 再按局部坐标判定具体区域
            auto box       = std::make_shared<ftxui::Box>(kNoBox);
            outRow.box     = box;
            outRow.element = std::move(row.element) | reflect(*box);
            outRow.regions = std::move(row.regions);
        } else {
            outRow.element = std::move(row.element);
        }
        out.rows.push_back(std::move(outRow));
    }
}

void renderItemJson(const utilxx_base::Json& json, const UiRenderCtx& ctx, UiRenderResult& out) {
    renderItems(agentxx::ui::parseItemList(json), ctx, out);
}

size_t measureItem(const agentxx::ui::Item& item, const UiRenderCtx& ctx) {
    // 与渲染同源: 渲染一次并统计行数 (不做第二套判定, 避免估算与渲染漂移)
    UiRenderResult out;
    renderItem(item, ctx, out);
    size_t lines = 0;
    for (const auto& row : out.rows) {
        lines += std::max<size_t>(1, row.lines);
    }
    return lines;
}

size_t measureItems(const std::vector<agentxx::ui::Item>& items, const UiRenderCtx& ctx) {
    size_t lines = 0;
    for (const auto& item : items) {
        lines += measureItem(item, ctx);
    }
    return lines;
}

} // namespace client
} // namespace agentxx
