#include "agentxx-client/io/tui/ui_components.h"

#include "agentxx-client/io/tui/framework/owned_reflect.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/markdown_block.h"
#include "agentxx-client/io/tui/text_layout.h"
#include "fmt/format.h"
#include "ftxui/screen/terminal.hpp"
#include "markdown/state_diagram.hpp"
#include "utilxx/diff_util.h"
#include "utilxx_base/log.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>
#include <tuple>
#include <utility>

using namespace ftxui;

namespace agentxx {
namespace client {

namespace {

using utilxx_base::Json;
using pluginxx::ui::Edges;
using pluginxx::ui::Item;
using pluginxx::ui::SizeValue;
using pluginxx::ui::TextValue;

// ---------------------------------------------------------------------------
// 能力段 (如实上报; adapt 按它决定降级结果)
// ---------------------------------------------------------------------------

/// TUI 实际支持的组件 (未列出的由 adapt 降级: Stack → 最后一个子节点 /
/// Image → alt 文本 / musicxx.Shader → 跳过)
constexpr std::string_view kTuiBlockNames[] = {
    "Text",   "Divider", "Gap",    "Button",   "Block", "Row",     "Column", "Expanded",
    "Spacer", "SizedBox", "Padding", "Align",   "Collapse", "KV",  "Table",  "Tree",
    "Progress", "Badge", "Control", "Markdown", "Icon", "Diff",   "Sparkline", "Diagram",
};

// ---------------------------------------------------------------------------
// 基础 helper
// ---------------------------------------------------------------------------

/// 缩进空格串
std::string spaces(int cols) {
    return (cols > 0) ? std::string(static_cast<size_t>(cols), ' ') : std::string{};
}

/// 内容可用宽度 (扣除当前缩进; <=0 表示不限宽)
///
/// 注意: 组件描述里没有"缩进"字段 (终端专有概念), 缩进由容器表达
/// (`Padding` / `Block.padding`), 渲染时逐层并入 [UiRenderCtx::indent]。
int contentWidth(const UiRenderCtx& ctx) {
    if (ctx.width <= 0) {
        return 0;
    }
    return std::max(1, ctx.width - ctx.indent);
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

/// 紧凑 JSON 文本 → JSON (空/非法时返回 null; 描述里的 `valueJson` 用这个口径)
Json jsonOf(const std::string& text) {
    if (text.empty()) {
        return Json{};
    }
    try {
        return Json::parse(text);
    } catch (const std::exception&) {
        return Json{};
    }
}

/// 候选项原值下标 → 选中下标 (按归一化文本比较, 生产者不必与候选项值严格同型)
int optionIndex(const std::vector<pluginxx::ui::ControlOption>& options, const std::string& value) {
    if (options.empty()) {
        return 0;
    }
    const auto want = jsonText(jsonOf(value));
    for (size_t i = 0; i < options.size(); ++i) {
        if (jsonText(jsonOf(options[i].valueJson)) == want) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 尺寸换算 (u → 列/行)
// ---------------------------------------------------------------------------

/// 长度 u → 列数 (`SizeValue::Auto` 返回 0, 由调用方决定)
int colsOf(const SizeValue& size, int avail, bool atLeastOne = false) {
    if (size.isValue()) {
        return tuiUiCapabilities().colsOf(size.value, atLeastOne);
    }
    if (size.isPercent()) {
        if (avail <= 0) {
            return 0;
        }
        const int cols = static_cast<int>(static_cast<double>(avail) * size.value / 100.0 + 0.5);
        return (atLeastOne && cols < 1 && size.value > 0.0) ? 1 : std::max(0, cols);
    }
    return 0;
}

/// 长度 u → 行数 (`SizeValue::Auto` 返回 0, 由调用方决定)
int rowsOf(const SizeValue& size, bool atLeastOne = false) {
    if (size.isValue()) {
        return tuiUiCapabilities().rowsOf(size.value, atLeastOne);
    }
    // 终端的高度基准无界, percent 退化为内容尺寸 (0 行)
    return 0;
}

/// 间距尺寸 (缺省取能力段的默认行距)
int gapCols(const SizeValue& size) {
    if (size.isAuto()) {
        const int u = static_cast<int>(tuiUiCapabilities().gap);
        return tuiUiCapabilities().colsOf(u);
    }
    return colsOf(size, 0);
}

/// 内边距/外边距 → 列 (左右) 与行 (上下); 终端不做像素级留白, 小于半格即消失
struct Insets {
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;

    bool empty() const {
        return left == 0 && right == 0 && top == 0 && bottom == 0;
    }
};

Insets insetsOf(const Edges& edges) {
    Insets out;
    out.left   = tuiUiCapabilities().colsOf(edges.left);
    out.right  = tuiUiCapabilities().colsOf(edges.right);
    out.top    = tuiUiCapabilities().rowsOf(edges.top);
    out.bottom = tuiUiCapabilities().rowsOf(edges.bottom);
    return out;
}

/// 文案: 先按键查语言表 (缺键用 fallback), 最后替换 `args` 里的命名占位
std::string textOf(const TextValue& value, const UiRenderCtx& ctx) {
    const std::function<std::string(std::string_view)> lookup
        = ctx.translate != nullptr
              ? ctx.translate
              : std::function<std::string(std::string_view)>([](std::string_view key) {
                    if (key.empty()) {
                        return std::string{};
                    }
                    const std::string_view hit = tr(key);
                    // 语言表缺键时返回键本身 (见 tui_i18n): 视为缺键, 让 fallback 生效
                    if (hit.empty() || hit == key) {
                        return std::string{};
                    }
                    return std::string{hit};
                });
    return value.resolve(lookup);
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
std::vector<std::string> textLines(const Item& item, const UiRenderCtx& ctx) {
    const std::string value = textOf(item.text, ctx);
    if (value.empty()) {
        return {};
    }
    std::vector<std::string> lines
        = item.wrap ? wrapTextToLines(value, std::max(1, contentWidth(ctx)))
                    : std::vector<std::string>{value};
    if (item.maxLines > 0 && lines.size() > static_cast<size_t>(item.maxLines)) {
        lines.resize(static_cast<size_t>(item.maxLines));
    }
    return lines;
}

/// diff 块渲染行数 (与 renderPluginDiff 的 side-by-side/统一两种形态一致)
size_t diffBlockLines(const Item& item, const UiRenderCtx& ctx) {
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
std::pair<Element, size_t> buildDiagram(const Item& item, const UiRenderCtx& ctx) {
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
    const int avail = (ctx.width > 0) ? std::max(20, ctx.width - ctx.indent - 6) : 0;
    const auto& theme  = *ctx.theme;
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

/// 按钮语义色 → 按钮样式
PluginButtonRole buttonRoleOf(std::string_view tone) {
    if (tone == "accent" || tone == "title" || tone == "tool") {
        return PluginButtonRole::Accent;
    }
    if (tone == "error" || tone == "danger") {
        return PluginButtonRole::Danger;
    }
    return PluginButtonRole::Normal;
}

/// 按钮显示宽度 ("[ 标签 ]")
int buttonWidth(std::string_view label) {
    return pluginxx::ui::displayWidth(label) + 4;
}

/// 渲染按钮元素
/// - `variant`: primary (强调反色) / secondary (默认按钮) / ghost|link (纯文字)
Element buttonElement(
    std::string_view label,
    std::string_view tone,
    std::string_view variant,
    const TUITheme&  theme,
    bool             disabled
) {
    const std::string caption = label.empty() ? std::string{"Button"} : std::string{label};
    if (variant == "ghost" || variant == "link") {
        Element el = text(caption) | bold;
        if (tone == "error" || tone == "danger") {
            el = el | color(theme.errorColor);
        } else if (tone == "hint") {
            el = el | color(theme.hintColor);
        } else {
            el = el | color(theme.accentColor);
        }
        return disabled ? el | theme.dim() : el;
    }
    PluginButtonDesc desc;
    desc.label = caption;
    desc.role  = (variant == "primary" || variant.empty()) && variant != "secondary"
                     ? PluginButtonRole::Accent
                     : buttonRoleOf(tone);
    Element el = renderPluginButton(desc, theme);
    return disabled ? el | theme.dim() : el;
}

/// 行为动作 id (TUI 只把动作透传给动作通道: dispatch 用名字, route/command 用目标)
std::string actionIdOf(const pluginxx::ui::Action& action) {
    if (action.empty()) {
        return {};
    }
    return action.name.empty() ? action.route : action.name;
}

// ---------------------------------------------------------------------------
// 列宽分配 (表格/横排共用; 纯计算)
// ---------------------------------------------------------------------------

/// 声明固定宽度的列按声明值, 其余列按权重分剩余宽度
/// - 总和超出可用宽度时先收缩固定列 (下限 4 列), 空间仍不足则全部等分
/// - `avail <= 0` 时按缺省宽度铺开 (不限宽场景)
/// - `weights[i] > 0` 表示该列按权重分剩余空间 (Expanded/Spacer 的 flex);
///   权重全为 0 时剩余空间给最后一列, `stretchAll = true` 时均分给各列
std::vector<int> layoutColumnWidths(
    const std::vector<int>& fixed,
    const std::vector<int>& weights,
    int                     avail,
    int                     gap,
    bool                    stretchAll = false
) {
    constexpr int    kMinColumn = 4;
    const size_t     n          = fixed.size();
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
            const int delta  = std::min(over, widths[i] - kMinColumn);
            widths[i]       -= delta;
            over            -= delta;
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
            int weightSum = 0;
            for (size_t i = 0; i < n; ++i) {
                if (fixed[i] > 0) {
                    continue;
                }
                weightSum += (i < weights.size() && weights[i] > 0) ? weights[i] : 1;
            }
            int given = 0;
            for (size_t i = 0; i < n; ++i) {
                if (fixed[i] > 0) {
                    continue;
                }
                const int weight = (i < weights.size() && weights[i] > 0) ? weights[i] : 1;
                const int share  = (i + 1 == n) ? (leftover - given)
                                                : (leftover * weight / std::max(1, weightSum));
                widths[i] += share;
                given     += share;
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

/// 单元格文本按列宽补齐/截断 (显示列宽口径; 宽字符安全)
std::string cellPadded(std::string_view content, int width, std::string_view align) {
    const std::string trimmed = pluginxx::ui::truncateToWidth(content, width);
    const int         used    = pluginxx::ui::displayWidth(trimmed);
    const int         pad     = std::max(0, width - used);
    if (align == "end" || align == "right") {
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
///
/// 列宽三态 (见描述层文档 §4.6): 省略/auto = 按内容比例分剩余空间; 数值 = 固定;
/// `{percent:n}` = 占可用宽度的比例。
Row renderTable(const Item& item, const UiRenderCtx& ctx) {
    constexpr int kGap  = 2;
    const auto&   theme = *ctx.theme;
    const int     avail = std::max(1, contentWidth(ctx));
    const size_t  n     = item.columns.size();

    // 各列自然宽度 (表头与单元格里最宽的一个), 供 auto 列按比例分配
    std::vector<int> natural(n, 0);
    for (size_t c = 0; c < n; ++c) {
        natural[c] = pluginxx::ui::displayWidth(textOf(item.columns[c].title, ctx));
    }
    for (const auto& rowCells : item.rows) {
        for (size_t c = 0; c < n && c < rowCells.size(); ++c) {
            natural[c]
                = std::max(natural[c], pluginxx::ui::displayWidth(textOf(rowCells[c].text, ctx)));
        }
    }

    std::vector<int> fixed(n, 0);
    int              autoCount = 0;
    for (size_t c = 0; c < n; ++c) {
        const auto& width = item.columns[c].width;
        if (width.isValue()) {
            fixed[c] = std::max(1, colsOf(width, avail, true));
        } else if (width.isPercent()) {
            fixed[c] = std::max(1, colsOf(width, avail, true));
        } else {
            ++autoCount;
        }
    }
    // auto 列按自然宽度作权重 (内容越长分得越多), 都没有内容时等分
    std::vector<int> weights(n, 0);
    int              naturalSum = 0;
    for (size_t c = 0; c < n; ++c) {
        if (fixed[c] > 0) {
            continue;
        }
        weights[c]  = std::max(1, natural[c]);
        naturalSum += weights[c];
    }
    (void)naturalSum;
    const auto widths = layoutColumnWidths(fixed, weights, avail, kGap);

    Elements                 lines;
    std::vector<UiHitRegion> regions;
    int                      y = 0;

    auto emit = [&](const std::vector<std::string>& cells,
                    const std::vector<Color>&       colors,
                    const std::vector<std::string>& actions,
                    const std::vector<std::string>& args) {
        Elements chunks;
        int      x = 0;
        for (size_t c = 0; c < n; ++c) {
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
                region.arg     = (c < args.size() && !args[c].empty()) ? args[c] : "{}";
                region.plugin  = ctx.plugin;
                region.ownerId = ctx.ownerId;
                regions.push_back(std::move(region));
            }
            x += widths[c];
        }
        lines.push_back(hbox(std::move(chunks)));
        ++y;
    };

    if (item.header && n > 0) {
        std::vector<std::string> cells;
        std::vector<Color>       colors;
        for (size_t c = 0; c < n; ++c) {
            cells.push_back(
                cellPadded(textOf(item.columns[c].title, ctx), widths[c], item.columns[c].align)
            );
            colors.push_back(theme.accentColor);
        }
        emit(
            cells,
            colors,
            std::vector<std::string>(n),
            std::vector<std::string>(n)
        );
        std::string sepLine;
        for (size_t c = 0; c < n; ++c) {
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
        std::vector<std::string> args;
        for (size_t c = 0; c < n; ++c) {
            const pluginxx::ui::TableCell* cell = (c < rowCells.size()) ? &rowCells[c] : nullptr;
            const std::string raw = (cell != nullptr) ? textOf(cell->text, ctx) : std::string{};
            cells.push_back(cellPadded(raw, widths[c], item.columns[c].align));
            const std::string tone
                = (cell != nullptr && !cell->tone.empty()) ? cell->tone : item.columns[c].tone;
            colors.push_back(itemColor(tone, theme));
            actions.push_back((cell != nullptr) ? actionIdOf(cell->action) : std::string{});
            args.push_back((cell != nullptr) ? cell->action.argsJson : std::string{});
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

/// 键值对 (键列宽度: auto = 按最长键; 数值/percent 按声明; 值列按剩余宽度截断)
Row renderKeyValue(const Item& item, const UiRenderCtx& ctx) {
    const auto& theme = *ctx.theme;
    const int   avail = std::max(1, contentWidth(ctx));
    int         keyW  = item.keyWidth.isValue() ? colsOf(item.keyWidth, avail, true)
                     : item.keyWidth.isPercent()
                         ? colsOf(item.keyWidth, avail, true)
                         : 0;
    if (keyW <= 0) {
        for (const auto& p : item.pairs) {
            keyW = std::max(keyW, pluginxx::ui::displayWidth(textOf(p.key, ctx)));
        }
    }
    const int sepW   = pluginxx::ui::displayWidth(item.sep);
    const int valueW = std::max(1, avail - keyW - sepW);

    Elements lines;
    for (const auto& p : item.pairs) {
        lines.push_back(hbox({
            text(pluginxx::ui::padRightToWidth(textOf(p.key, ctx), keyW))
                | color(itemColor(p.kTone, theme)),
            text(item.sep) | color(theme.hintColor),
            text(pluginxx::ui::truncateToWidth(textOf(p.value, ctx), valueW))
                | color(itemColor(p.vTone, theme)),
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
///
/// 折叠态由**宿主**维护 (键 = 从根到该节点的路径, 如 `src/io/`): 提供
/// [UiRenderCtx::collapseExpanded] 时, 有子节点的行可点击展开/收起 (行首标记
/// `-`/`+`), 收起后子树不渲染且不占点击区域; 未提供时按全展开渲染 (行式前端与
/// 不关心折叠的调用方行为不变)。
Row renderTree(const Item& item, const UiRenderCtx& ctx) {
    const auto&              theme = *ctx.theme;
    Elements                 lines;
    std::vector<UiHitRegion> regions;
    int                      y = 0;

    std::function<void(const std::vector<pluginxx::ui::TreeNode>&, const std::string&, const std::string&)>
        emit = [&](const std::vector<pluginxx::ui::TreeNode>& nodes,
                   const std::string&                        prefix,
                   const std::string&                        path) {
            for (size_t i = 0; i < nodes.size(); ++i) {
                const auto& node  = nodes[i];
                const bool  last  = (i + 1 == nodes.size());
                const std::string label = textOf(node.label, ctx);
                // 节点路径 (折叠状态键; 结尾带 '/' 便于与 id 型键区分)
                const std::string nodePath = path + label + "/";
                const bool foldable = !node.children.empty() && ctx.collapseExpanded != nullptr;
                const bool expanded = !foldable || ctx.collapseExpanded(nodePath, true);

                std::string line = item.connector ? prefix + (last ? "└─ " : "├─ ") : prefix;
                if (foldable) {
                    line += expanded ? "- " : "+ ";
                }
                line += label;
                lines.push_back(text(line) | color(itemColor(node.tone, theme)));
                const std::string actionId = actionIdOf(node.action);
                if (!actionId.empty()) {
                    UiHitRegion region;
                    region.x       = 0;
                    region.y       = y;
                    region.w       = 0; // 整行可点
                    region.h       = 1;
                    region.id      = actionId;
                    region.arg     = node.action.argsJson.empty() ? "{}" : node.action.argsJson;
                    region.plugin  = ctx.plugin;
                    region.ownerId = ctx.ownerId;
                    regions.push_back(std::move(region));
                } else if (foldable) {
                    // 无动作节点: 整行登记为折叠区域 (点击切换展开状态)
                    UiHitRegion region;
                    region.kind    = UiHitRegionKind::Collapse;
                    region.x       = 0;
                    region.y       = y;
                    region.w       = 0; // 整行可点
                    region.h       = 1;
                    region.id      = nodePath;
                    region.plugin  = ctx.plugin;
                    region.ownerId = ctx.ownerId;
                    regions.push_back(std::move(region));
                }
                ++y;
                if (expanded && !node.children.empty()) {
                    emit(
                        node.children,
                        item.connector ? prefix + (last ? "   " : "│  ") : prefix,
                        nodePath
                    );
                }
            }
        };
    emit(item.nodes, "", "");

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
        to         = std::min(to, data.size());
        double sum = 0.0;
        for (size_t i = from; i < to; ++i) {
            sum += data[i];
        }
        out.push_back(sum / static_cast<double>(std::max<size_t>(1, to - from)));
    }
    return out;
}

/// 迷你趋势图 (高度 1 = 单行块字符; 高度 > 1 = 多行纵向分辨率)
Row renderSparkline(const Item& item, const UiRenderCtx& ctx) {
    const auto& theme  = *ctx.theme;
    const int   avail  = std::max(1, contentWidth(ctx));
    const std::string label = textOf(item.label, ctx);
    const int   labelW = label.empty() ? 0 : pluginxx::ui::displayWidth(label) + 1;
    const int   height = std::clamp(item.glyphHeight, 1, 8);

    Row row;
    if (item.data.empty()) {
        Elements els;
        if (!label.empty()) {
            els.push_back(text(label + " ") | color(theme.hintColor));
        }
        els.push_back(text("[sparkline]") | color(theme.hintColor) | theme.dim());
        row.element = hbox(std::move(els));
        return row;
    }

    const bool hasSuffix = item.showLast;
    const int  suffixW
        = hasSuffix ? (pluginxx::ui::displayWidth(numText(item.data.back())) + 1) : 0;
    const int    plotW = std::max(4, avail - labelW - suffixW);
    const auto   data  = bucketize(item.data, plotW);
    const auto   minIt = std::min_element(data.begin(), data.end());
    const auto   maxIt = std::max_element(data.begin(), data.end());
    const double lo    = item.hasMin ? item.minValue : *minIt;
    const double hi    = item.hasMax ? item.maxValue : *maxIt;
    const double span  = (hi > lo) ? (hi - lo) : 0.0;
    const double last  = item.data.back();

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
        return itemColor(item.tone, theme);
    };

    constexpr int kLevelsPerRow = 8;
    const int     totalLevels   = height * kLevelsPerRow;
    const bool    barStyle      = (item.glyphStyle == "bar");
    Elements      lines;
    for (int rowIdx = 0; rowIdx < height; ++rowIdx) {
        Elements chunks;
        if (rowIdx == 0) {
            if (!label.empty()) {
                chunks.push_back(text(label + " ") | color(theme.hintColor));
            }
        } else if (labelW > 0) {
            chunks.push_back(text(spaces(labelW)));
        }
        for (size_t i = 0; i < data.size(); ++i) {
            const double r     = (span > 0) ? std::clamp((data[i] - lo) / span, 0.0, 1.0) : 0.5;
            const int    level = std::clamp(
                static_cast<int>(r * static_cast<double>(totalLevels - 1) + 0.5),
                0,
                totalLevels - 1
            );
            const int inRow
                = std::clamp(level - (height - 1 - rowIdx) * kLevelsPerRow + 1, 0, kLevelsPerRow);
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
            chunks.push_back(text(" " + numText(last)) | color(theme.hintColor));
        }
        lines.push_back(hbox(std::move(chunks)));
    }
    row.lines   = static_cast<size_t>(height);
    row.element = (lines.size() == 1) ? std::move(lines[0]) : vbox(std::move(lines));
    return row;
}

/// 进度/计量条 (填充块 + 背景块 + 数值文本; 阈值配色)
Row renderProgress(const Item& item, const UiRenderCtx& ctx) {
    const auto& theme  = *ctx.theme;
    const int   avail  = std::max(1, contentWidth(ctx));
    const std::string label = textOf(item.label, ctx);
    const int   labelW = label.empty() ? 0 : pluginxx::ui::displayWidth(label) + 1;
    const int   declared = colsOf(item.width, avail, true);
    const int   barW     = (declared > 0)
                               ? declared
                               : std::max(6, std::min(24, avail - labelW - 8));
    const double total  = (item.total != 0.0) ? item.total : 100.0;
    const double ratio  = std::clamp(item.value / total, 0.0, 1.0);
    const int    filled = static_cast<int>(ratio * static_cast<double>(barW) + 0.5);

    // 阈值配色: 由高到低匹配首个满足项 (解析时已按阈值降序)
    std::string fillColor = item.tone.empty() ? "accent" : item.tone;
    for (const auto& th : item.thresholds) {
        if (item.value >= th.at) {
            fillColor = th.tone;
            break;
        }
    }

    Elements els;
    if (!label.empty()) {
        els.push_back(text(label + " ") | color(theme.hintColor));
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
            text(std::string(static_cast<size_t>(barW - filled), ' ')) | bgcolor(theme.blockColor)
            | theme.dim()
        );
    }
    barChunks.push_back(text("▏") | color(theme.hintColor));
    els.push_back(hbox(std::move(barChunks)));
    if (item.showValue) {
        std::string valueText  = numText(ratio * 100.0);
        valueText             += item.unit.empty() ? "%" : item.unit;
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
std::string controlEditText(const Item& item, const UiFormState* form) {
    if (form != nullptr) {
        if (const auto* state = form->find(item.id);
            state != nullptr && state->initialized && state->edited) {
            return state->editText;
        }
    }
    const Json value = jsonOf(item.valueJson);
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number()) {
        return numText(value.get<double>());
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    return {};
}

/// 控件选中下标
int controlSelected(const Item& item, const UiFormState* form) {
    if (form != nullptr) {
        if (const auto* state = form->find(item.id); state != nullptr && state->initialized) {
            return state->selected;
        }
    }
    return optionIndex(item.options, item.valueJson);
}

/// 控件勾选状态
bool controlChecked(const Item& item, const UiFormState* form) {
    if (form != nullptr) {
        if (const auto* state = form->find(item.id); state != nullptr && state->initialized) {
            return state->checked;
        }
    }
    const Json value = jsonOf(item.valueJson);
    return value.is_boolean() && value.get<bool>();
}

/// 输入框显示宽度 (" " + 值 + 右侧补齐 + " "; 最小 4 列; 仅用于宽度不可知时的兜底命中区域)
int inputFieldWidth(std::string_view value) {
    return std::max(4, pluginxx::ui::displayWidth(value) + 2);
}

/// 输入框行 (整行: 左右各 1 格内边距 + 输入框底色, 与主消息输入框同一形态)
///
/// - 占位文本取控件 help: 与主消息输入框一致 —— 值为空时显示, 一输入就隐藏
/// - 值为空时文字用弱化色 (占位), 有值时用输入文字色
/// - 获得键盘焦点时加粗加下划线 (与既有控件一致)
/// - 文字元素水平撑满 (`xflex`): 视觉上是一条整行输入框, 命中区域也按整行登记
Element inputFieldRow(
    const std::string& value,
    const std::string& placeholder,
    const Item&        item,
    const UiRenderCtx& ctx
) {
    const auto& theme = *ctx.theme;
    const bool  empty = value.empty();
    const std::string& shown = empty ? placeholder : value;
    Element el = text(" " + shown + " ")
                 | color(empty ? theme.hintColor : theme.inputTextColor) | xflex;
    if (ctx.form != nullptr && ctx.form->focusedId == item.id) {
        el = el | bold | underlined;
    }
    return hbox({std::move(el)}) | bgcolor(theme.inputBgColor);
}

/// 数值控件的步进按钮 ("[ - ]" / "[ + ]")
Element inputStepButton(std::string_view label, const TUITheme& theme) {
    return text(label) | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
}

/// 追加控件区域
void addControlRegion(
    std::vector<UiHitRegion>& regions,
    const UiRenderCtx&        ctx,
    const Item&               item,
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

/// 动作 id 是否为域内的提交/取消 (中断提交行的按钮由描述层表达为普通按钮,
/// 点击语义仍是"提交/取消整份表单", 命中类型要按提交行登记)
bool isFormSubmitAction(std::string_view id) {
    return id == kFormSubmitActionId || id == kFormCancelActionId;
}

// ---------------------------------------------------------------------------
// 主渲染分发
// ---------------------------------------------------------------------------

Rows renderItemRows(const Item& item, const UiRenderCtx& ctx, UiRenderResult& out);

/// 渲染子项列表 (返回中间行模型; 容器统一偏移区域坐标并附加命中框)
Rows renderChildren(const std::vector<Item>& items, const UiRenderCtx& ctx, UiRenderResult& out) {
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

/// 文本类降级行 (fallback / 描述层未支持的组件兜底)
Rows fallbackRows(
    const Item&        item,
    const UiRenderCtx& ctx,
    UiRenderResult&    out,
    std::string_view   text0 = {}
) {
    const std::string fallback = text0.empty() ? item.fallback : std::string{text0};
    if (fallback.empty()) {
        return {};
    }
    Item textItem = pluginxx::ui::build::caption(fallback);
    textItem.dim  = true;
    textItem.wrap = true;
    return renderItemRows(textItem, ctx, out);
}

/// 文本 + 按钮合并为一行 (返回 false 表示无法合并, 调用方按普通顺序渲染)
///
/// 触发条件: 相邻两项分别是 Text 与 Button, 且各自都只产出一行。
bool mergeTextButton(
    const Item&        textItem,
    const Item&        buttonItem,
    const UiRenderCtx& ctx,
    UiRenderResult&    out
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
    const int prefixCols
        = ctx.indent + pluginxx::ui::displayWidth(textOf(textItem.text, ctx));
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
        // 反射框必须由元素自持: 本函数直接把 UiRow 交给调用方, 调用方通常只搬走
        // Element (ScrollItem/面板缓存) —— UiRow::box 随局部 UiRenderResult 析构,
        // 用 FTXUI 的 reflect(Box&) (只存引用) 时元素后续布局会写已释放内存
        // (见 [OwnedReflect]; 该路径不过 renderItem, 需在此自行包装)
        auto box    = std::make_shared<ftxui::Box>(kNoBox);
        row.box     = box;
        row.element = std::make_shared<OwnedReflect>(std::move(row.element), std::move(box));
    }
    out.rows.push_back(std::move(row));
    return true;
}

Rows renderItemRows(const Item& item, const UiRenderCtx& ctx, UiRenderResult& out) {
    Rows rows;
    if (ctx.theme == nullptr) {
        return rows;
    }
    const auto& theme = *ctx.theme;

    // 未知 kind: 降级为 fallback 文本 (无 fallback 则跳过)
    if (!item.known) {
        return fallbackRows(item, ctx, out);
    }

    const std::string pad     = spaces(ctx.indent);
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
    /// 登记一个"整行可点"区域 (文本/容器类组件的 action)
    auto actionRegion = [&](const pluginxx::ui::Action& action, int lines) -> std::vector<UiHitRegion> {
        std::vector<UiHitRegion> regions;
        const std::string id = actionIdOf(action);
        if (id.empty()) {
            return regions;
        }
        UiHitRegion region;
        region.x       = 0;
        region.y       = 0;
        region.w       = 0; // 整行可点
        region.h       = std::max(1, lines);
        region.id      = id;
        region.arg     = action.argsJson.empty() ? "{}" : action.argsJson;
        region.plugin  = ctx.plugin;
        region.ownerId = ctx.ownerId;
        regions.push_back(std::move(region));
        return regions;
    };

    // ---------------- Text ----------------
    if (item.kind == "Text") {
        std::string tone = item.tone;
        if (item.textType == "title" && (tone.empty() || tone == "normal")) {
            tone = "accent";
        } else if (item.textType == "caption" && (tone.empty() || tone == "normal")) {
            tone = "hint";
        }
        Color      textColor = itemColor(tone, theme);
        const auto lines     = textLines(item, ctx);
        if (lines.empty()) {
            return rows;
        }
        const int avail = std::max(1, contentWidth(ctx));
        for (size_t i = 0; i < lines.size(); ++i) {
            // 行内对齐 (start/center/end): 用空格补齐到可用宽度, 保证命中区域可计算
            int         leftPad = 0;
            std::string line    = lines[i];
            const int   used    = pluginxx::ui::displayWidth(line);
            if (item.align == "center" && used < avail) {
                leftPad = (avail - used) / 2;
            } else if (item.align == "end" && used < avail) {
                leftPad = avail - used;
            }
            if (leftPad > 0) {
                line = spaces(leftPad) + line;
            }
            Element el = text(line) | color(textColor);
            if (item.bold || item.textType == "title") {
                el = el | bold;
            }
            if (item.dim || item.textType == "caption") {
                el = el | theme.dim();
            }
            if (!item.wrap) {
                el = el | xflex_shrink;
            }
            std::vector<UiHitRegion> regions;
            if (i == 0) {
                regions = actionRegion(item.action, static_cast<int>(lines.size()));
                for (auto& region : regions) {
                    region.x += leftPad;
                }
            }
            pushPlain(std::move(el), 1, std::move(regions));
        }
        return rows;
    }

    // ---------------- Markdown ----------------
    if (item.kind == "Markdown") {
        if (item.markdown.empty()) {
            return rows;
        }
        auto [el, builder] = renderMarkdown(
            item.markdown,
            itemColor(item.tone, theme),
            theme.markdownTheme,
            contentWidth(ctx)
        );
        if (builder) {
            out.builders.push_back(std::move(builder));
        }
        pushPlain(
            std::move(el) | xflex_shrink,
            estimateMarkdownLines(item.markdown, contentWidth(ctx))
        );
        return rows;
    }

    // ---------------- Diff ----------------
    if (item.kind == "Diff") {
        pushPlain(
            renderPluginDiff(item.path, item.oldStr, item.newStr, theme, ctx.width),
            diffBlockLines(item, ctx)
        );
        return rows;
    }

    // ---------------- Divider / Gap ----------------
    if (item.kind == "Divider") {
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
    if (item.kind == "Gap") {
        // 终端留白按格换算: 12u(默认) ≈ 1 行; 换算成 0 时该留白消失
        int rowsCount = rowsOf(item.size);
        if (rowsCount <= 0 && item.size.isAuto()) {
            rowsCount = tuiUiCapabilities().rowsOf(tuiUiCapabilities().gap);
        }
        for (int i = 0; i < std::max(0, rowsCount); ++i) {
            pushPlain(text(" "), 1);
        }
        return rows;
    }

    // ---------------- Badge ----------------
    if (item.kind == "Badge") {
        const std::string value = textOf(item.text, ctx);
        if (value.empty()) {
            return rows;
        }
        pushPlain(text("● " + value) | color(itemColor(item.tone, theme)), 1);
        return rows;
    }

    // ---------------- Diagram ----------------
    if (item.kind == "Diagram") {
        auto [el, lines] = buildDiagram(item, ctx);
        if (lines == 0) {
            return rows;
        }
        pushPlain(std::move(el) | flex, lines);
        return rows;
    }

    // ---------------- Icon (终端用 glyph; 没有 glyph 则跳过) ----------------
    if (item.kind == "Icon") {
        if (item.glyph.empty()) {
            return rows;
        }
        pushPlain(
            text(item.glyph) | color(itemColor(item.tone, theme)),
            1,
            actionRegion(item.action, 1)
        );
        return rows;
    }

    // ---------------- Button ----------------
    if (item.kind == "Button") {
        const std::string label = textOf(item.label, ctx);
        Element           el    = buttonElement(label, item.tone, item.variant, theme, item.disabled);
        const std::string actionId = actionIdOf(item.action);
        std::vector<UiHitRegion> regions;
        // 域内的表单提交/取消动作由宿主处理 (插件不需要绑定动作处理器), 其余动作要求
        // 快照里存在该插件的绑定; 未提供表单状态时提交行只是展示, 不登记命中区域
        const bool formSubmit = isFormSubmitAction(actionId);
        const bool actionable = !actionId.empty() && !item.disabled
                                && (formSubmit ? (ctx.form != nullptr)
                                               : (ctx.registry == nullptr
                                                  || hasPluginBinding(ctx.plugin, ctx.registry)));
        if (actionable) {
            UiHitRegion region;
            region.kind    = formSubmit ? UiHitRegionKind::FormSubmit : UiHitRegionKind::Action;
            region.x       = 0;
            region.y       = 0;
            region.w       = buttonWidth(label.empty() ? std::string_view{"Button"} : label);
            region.h       = 1;
            region.id      = actionId;
            region.arg     = item.action.argsJson.empty() ? "{}" : item.action.argsJson;
            region.plugin  = ctx.plugin;
            region.ownerId = ctx.ownerId;
            regions.push_back(std::move(region));
        }
        pushPlain(std::move(el) | xflex_shrink, 1, std::move(regions));
        return rows;
    }

    // ---------------- 结构化与图表组件 ----------------
    if (item.kind == "Progress") {
        pushPlain(renderProgress(item, ctx).element, 1);
        return rows;
    }
    if (item.kind == "Sparkline") {
        auto row = renderSparkline(item, ctx);
        pushPlain(row.element, row.lines);
        return rows;
    }
    if (item.kind == "KV") {
        auto row = renderKeyValue(item, ctx);
        pushPlain(row.element, row.lines);
        return rows;
    }
    if (item.kind == "Table") {
        auto row = renderTable(item, ctx);
        pushPlain(row.element, row.lines, std::move(row.regions));
        return rows;
    }
    if (item.kind == "Tree") {
        auto row = renderTree(item, ctx);
        pushPlain(row.element, row.lines, std::move(row.regions));
        return rows;
    }

    // ---------------- Row (横向组合) ----------------
    if (item.kind == "Row") {
        const int n = static_cast<int>(item.children.size());
        if (n == 0) {
            return rows;
        }
        const int        avail = std::max(1, contentWidth(ctx));
        const int        gap   = item.hasGap ? gapCols(item.gap) : 0;
        std::vector<int> fixed(static_cast<size_t>(n), 0);
        std::vector<int> weights(static_cast<size_t>(n), 0);
        for (int i = 0; i < n; ++i) {
            const auto& child = item.children[static_cast<size_t>(i)];
            if (child.kind == "SizedBox" && child.width.isValue()) {
                fixed[static_cast<size_t>(i)] = std::max(1, colsOf(child.width, avail, true));
            } else if (child.kind == "Expanded" || child.kind == "Spacer") {
                weights[static_cast<size_t>(i)] = std::max(1, child.flex);
            }
        }
        const auto widths
            = layoutColumnWidths(fixed, weights, avail, gap, item.align == "stretch");

        Elements columnEls;
        Rows     columns;
        int      maxLines = 1;
        for (int i = 0; i < n; ++i) {
            const int           w     = std::max(1, widths[static_cast<size_t>(i)]);
            const Item&         child = item.children[static_cast<size_t>(i)];
            const bool          last  = (i + 1 == n);
            // 列内子项按列宽渲染 (indent = 0: 本行的基础缩进由 pushPlain 叠加)
            Rows subRows;
            if (child.kind == "Spacer") {
                Row blank;
                blank.element = text(" ");
                subRows.push_back(std::move(blank));
            } else {
                subRows = renderItemRows(child, childCtx(ctx, w, 0), out);
            }
            Row     columnRow = stackRows(subRows);
            Element el        = std::move(columnRow.element);
            // 行内对齐: main = center/end 时整行留白; spaceBetween 时末列右对齐
            const bool rightAlign = (item.main == "spaceBetween" && last) || item.align == "end";
            const bool centerAlign = (item.align == "center" && !rightAlign);
            if (rightAlign) {
                el = hbox({filler(), std::move(el)});
            } else if (centerAlign) {
                el = hbox({filler(), std::move(el), filler()});
            }
            columnEls.push_back(std::move(el) | size(WIDTH, EQUAL, w));
            maxLines = std::max(maxLines, static_cast<int>(columnRow.lines));
            columns.push_back(std::move(columnRow));
            if (!last && gap > 0) {
                columnEls.push_back(text(spaces(gap)));
            }
        }

        std::vector<UiHitRegion> regions;
        int                      offsetX = 0;
        for (int i = 0; i < n; ++i) {
            for (const auto& region : columns[static_cast<size_t>(i)].regions) {
                regions.push_back(region.offsetBy(offsetX, 0));
            }
            offsetX += widths[static_cast<size_t>(i)] + gap;
        }
        pushPlain(hbox(std::move(columnEls)), static_cast<size_t>(maxLines), std::move(regions));
        return rows;
    }

    // ---------------- Column (纵向组合) ----------------
    if (item.kind == "Column") {
        const int gap = item.hasGap ? gapCols(item.gap) : 0;
        Rows      childrenRows;
        bool      first = true;
        for (const auto& child : item.children) {
            if (!first && gap > 0) {
                for (int i = 0; i < gap; ++i) {
                    Row blank;
                    blank.element = text(" ");
                    childrenRows.push_back(std::move(blank));
                }
            }
            first = false;
            auto sub = renderItemRows(child, ctx, out);
            childrenRows.insert(
                childrenRows.end(),
                std::make_move_iterator(sub.begin()),
                std::make_move_iterator(sub.end())
            );
        }
        // main 的纵向分布与 cross 的居中都要求"父容器有界高度", 终端没有该信息:
        // 纵向按内容顺序排列 (vertical 对齐尽力而为), 命中区域保持子项自身坐标
        for (auto& row : childrenRows) {
            rows.push_back(std::move(row));
        }
        return rows;
    }

    // ---------------- Expanded / Spacer (不在 flex 容器内时的退化形态) ----------------
    if (item.kind == "Expanded") {
        Rows out2;
        for (const auto& child : item.children) {
            auto sub = renderItemRows(child, ctx, out);
            out2.insert(
                out2.end(),
                std::make_move_iterator(sub.begin()),
                std::make_move_iterator(sub.end())
            );
        }
        return out2;
    }
    if (item.kind == "Spacer") {
        pushPlain(text(" "), 1);
        return rows;
    }

    // ---------------- SizedBox ----------------
    if (item.kind == "SizedBox") {
        const int avail  = std::max(1, contentWidth(ctx));
        const int width  = colsOf(item.width, avail, true);
        int       height = rowsOf(item.height, true);
        if (item.aspect > 0.0 && width > 0) {
            // 终端按字符格比例近似: rows = ceil(列数 × 格宽 / (比例 × 格高))
            const double cellW = tuiUiCapabilities().cell.width > 0.0
                                     ? tuiUiCapabilities().cell.width
                                     : 8.0;
            const double cellH = tuiUiCapabilities().cell.height > 0.0
                                     ? tuiUiCapabilities().cell.height
                                     : 20.0;
            height = std::max(
                1,
                static_cast<int>(std::ceil(static_cast<double>(width) * cellW
                                           / (item.aspect * cellH)))
            );
        }
        if (item.children.empty()) {
            const int lines = std::max(1, height);
            for (int i = 0; i < lines; ++i) {
                pushPlain(text(spaces(width > 0 ? width : 1)), 1);
            }
            return rows;
        }
        const int childWidth = (width > 0) ? width : avail;
        auto      sub        = renderChildren(item.children, childCtx(ctx, childWidth, 0), out);
        for (auto& row : sub) {
            Element el = std::move(row.element);
            if (width > 0) {
                el = std::move(el) | size(WIDTH, EQUAL, width);
            }
            pushPlain(std::move(el), row.lines, std::move(row.regions));
        }
        // 高度只保证下限 (超出时按内容渲染, 终端不做裁剪)
        int rendered = 0;
        for (const auto& row : sub) {
            rendered += static_cast<int>(std::max<size_t>(1, row.lines));
        }
        for (int i = rendered; i < height; ++i) {
            pushPlain(text(" "), 1);
        }
        return rows;
    }

    // ---------------- Padding ----------------
    if (item.kind == "Padding") {
        const Insets insets = insetsOf(item.padding);
        for (int i = 0; i < insets.top; ++i) {
            pushPlain(text(" "), 1);
        }
        const int innerW = std::max(1, contentWidth(ctx) - insets.left - insets.right);
        auto      sub
            = renderChildren(item.children, childCtx(ctx, innerW, ctx.indent + insets.left), out);
        for (auto& row : sub) {
            if (insets.right > 0 && row.element) {
                row.element = hbox({std::move(row.element), text(spaces(insets.right))});
            }
            rows.push_back(std::move(row));
        }
        for (int i = 0; i < insets.bottom; ++i) {
            pushPlain(text(" "), 1);
        }
        return rows;
    }

    // ---------------- Align ----------------
    if (item.kind == "Align") {
        const bool center = (item.align == "center");
        const bool end    = (item.align == "end");
        auto       sub    = renderChildren(item.children, ctx, out);
        for (auto& row : sub) {
            if ((center || end) && row.regions.empty() && row.element) {
                // 子项自身宽度未知: 交给 FTXUI 的 filler 居中/靠右 (有命中区域时
                // 位置无法预测, 保持左对齐以保证命中正确)
                row.element = center
                                  ? hbox({filler(), std::move(row.element), filler()})
                                  : hbox({filler(), std::move(row.element)});
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    // ---------------- Block (内容块) ----------------
    if (item.kind == "Block") {
        const std::string variant = item.variant.empty() ? "card" : item.variant;
        const bool        border  = (variant == "card");
        // inset: 左右各缩进 1 列 (终端没有背景块时用缩进表达"内容区")
        const int insetCols = (variant == "inset") ? 1 : 0;
        Insets    pad       = insetsOf(item.padding);
        if (pad.empty()) {
            pad.left  = border ? 1 : 0;
            pad.right = border ? 1 : 0;
            pad.top   = border ? 0 : 0;
        }
        const Insets margin = insetsOf(item.margin);
        const int    frame  = (border ? 2 : 0) + pad.left + pad.right + insetCols * 2;
        const int    innerW = std::max(1, contentWidth(ctx) - frame);

        for (int i = 0; i < margin.top; ++i) {
            pushPlain(text(" "), 1);
        }
        auto inner = stackRows(renderChildren(item.children, childCtx(ctx, innerW, 0), out));
        Element innerEl = std::move(inner.element);
        if (insetCols > 0) {
            innerEl = hbox({text(spaces(insetCols)), std::move(innerEl)});
        }
        if (pad.left > 0 || pad.right > 0) {
            innerEl = hbox({
                text(spaces(pad.left)),
                std::move(innerEl),
                text(spaces(std::max(0, pad.right))),
            });
        }
        Element boxEl = std::move(innerEl);
        for (int i = 0; i < pad.top; ++i) {
            boxEl = vbox({text(" "), std::move(boxEl)});
        }
        for (int i = 0; i < pad.bottom; ++i) {
            boxEl = vbox({std::move(boxEl), text(" ")});
        }
        if (border) {
            const std::string title = textOf(item.title, ctx);
            if (title.empty()) {
                boxEl = std::move(boxEl) | borderStyled(BorderStyle::LIGHT)
                        | color(theme.hintColor);
            } else {
                boxEl = window(
                            text(title) | color(theme.accentColor) | bold,
                            std::move(boxEl),
                            BorderStyle::LIGHT
                        )
                        | color(theme.hintColor);
            }
        }
        const int    dx     = (border ? 1 : 0) + pad.left + insetCols;
        const int    dy     = (border ? 1 : 0) + pad.top;
        const size_t lines  = inner.lines + static_cast<size_t>(pad.top + pad.bottom)
                             + (border ? 2u : 0u);
        std::vector<UiHitRegion> regions;
        for (const auto& region : inner.regions) {
            regions.push_back(region.offsetBy(dx, dy));
        }
        pushPlain(std::move(boxEl), lines, std::move(regions));
        for (int i = 0; i < margin.bottom; ++i) {
            pushPlain(text(" "), 1);
        }
        return rows;
    }

    // ---------------- Collapse (可折叠分组) ----------------
    if (item.kind == "Collapse") {
        bool expanded = item.expanded;
        if (ctx.collapseExpanded) {
            expanded = ctx.collapseExpanded(item.id, item.expanded);
        }
        const std::string title = textOf(item.title, ctx);
        Element           header = hbox({
            text(expanded ? "- " : "+ ") | color(theme.accentColor),
            text(title) | color(theme.accentColor) | bold,
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
                item.children,
                childCtx(ctx, ctx.width, ctx.indent + 2),
                out
            );
            for (auto& row : sub) {
                rows.push_back(std::move(row));
            }
        }
        return rows;
    }

    // ---------------- Control (交互控件) ----------------
    if (item.kind == "Control") {
        const bool interactive = ctx.form != nullptr;
        const std::string label = textOf(item.label, ctx);
        const std::string help  = textOf(item.help, ctx);
        const bool inlineLabel  = (item.control == "checkbox" || item.control == "switch");
        // 输入类控件 (text/number): help 作为输入框内的占位文本 (空值时显示),
        // 不再单起一行说明 (与主消息输入框的占位行为一致)
        const bool inputLike    = (item.control == "text" || item.control == "number");
        if (!label.empty() && !inlineLabel) {
            pushPlain(text(label) | color(theme.accentColor) | bold, 1);
        }
        if (!help.empty() && !inputLike) {
            const int helpW = std::max(1, contentWidth(ctx));
            for (const auto& line : wrapTextToLines(help, helpW)) {
                pushPlain(text(line) | color(theme.hintColor) | theme.dim(), 1);
            }
        }
        const std::string caption = label.empty() ? item.id : label;
        if (inlineLabel) {
            const bool checked = controlChecked(item, ctx.form);
            Element    mark    = text(checked ? "[ ✓ ] " : "[   ] ")
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
            // 整行输入框: 点击行内任意位置聚焦输入 (w = 0 表示延伸到行右边界)
            std::vector<UiHitRegion> regions;
            if (interactive && !item.id.empty()) {
                addControlRegion(regions, ctx, item, 0, 0, 0, 0);
            }
            pushPlain(
                inputFieldRow(controlEditText(item, ctx.form), help, item, ctx),
                1,
                std::move(regions)
            );
        } else if (item.control == "number") {
            // 整行输入框 + 两端步进按钮 "[ - ]" / "[ + ]" (值区域撑满其余宽度)
            const std::string value  = controlEditText(item, ctx.form);
            constexpr int     kStepW = 5; // "[ - ]" 与 "[ + ]" 的显示宽度
            const int         rowW   = contentWidth(ctx);
            Element           el     = hbox({
                inputStepButton("[ - ]", theme),
                inputFieldRow(value, help, item, ctx) | flex,
                inputStepButton("[ + ]", theme),
            });
            std::vector<UiHitRegion> regions;
            if (interactive && !item.id.empty()) {
                addControlRegion(regions, ctx, item, 0, 0, kStepW, 0);
                if (rowW > kStepW * 2) {
                    // 宽度可知: "+" 贴右边界, 值区域延伸到 "+" 之前
                    // (顺序在 "+" 之后登记: 命中查询取第一个匹配项)
                    addControlRegion(regions, ctx, item, rowW - kStepW, 0, kStepW, 1);
                    addControlRegion(regions, ctx, item, kStepW, 0, 0, 2);
                } else {
                    // 宽度未知 (不限宽渲染): 按内容宽度兜底
                    const int valueW = inputFieldWidth(value.empty() ? help : value);
                    addControlRegion(regions, ctx, item, kStepW, 0, valueW, 2);
                    addControlRegion(regions, ctx, item, kStepW + valueW, 0, kStepW, 1);
                }
            }
            pushPlain(std::move(el), 1, std::move(regions));
        } else if (item.control == "buttons") {
            const int                selected = controlSelected(item, ctx.form);
            Elements                 els;
            std::vector<UiHitRegion> regions;
            int                      x = 0;
            for (size_t i = 0; i < item.options.size(); ++i) {
                const auto&       opt      = item.options[i];
                const std::string optLabel = textOf(opt.label, ctx);
                const std::string shown    = optLabel.empty() ? caption : optLabel;
                if (i > 0) {
                    els.push_back(text(" "));
                    ++x;
                }
                els.push_back(buttonElement(
                    shown,
                    (static_cast<int>(i) == selected) ? std::string{"accent"} : opt.tone,
                    (static_cast<int>(i) == selected) ? std::string{"primary"}
                                                      : std::string{"secondary"},
                    theme,
                    false
                ));
                if (interactive && !item.id.empty()) {
                    addControlRegion(
                        regions,
                        ctx,
                        item,
                        x,
                        0,
                        buttonWidth(shown),
                        static_cast<int>(i)
                    );
                }
                x += buttonWidth(shown);
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
                const std::string optLabel = textOf(opt.label, ctx);
                const std::string shown    = optLabel.empty() ? caption : optLabel;
                const bool        active   = (static_cast<int>(i) == selected);
                // 选中项: 指示符 + 反色底 (整行); 未选中: 同宽占位 + 普通色
                Element entry = hbox({
                    text(active ? "+ " : "  "),
                    text(shown) | color(itemColor(opt.tone, theme)),
                });
                entry         = active ? entry | bgcolor(theme.buttonActiveBgColor)
                                     | color(theme.buttonActiveTextColor) | bold
                                       : entry;
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
            pushPlain(text(fmt::format("[control: {}]", item.control)) | color(theme.hintColor), 1);
        }
        if (ctx.form != nullptr) {
            if (const auto* state = ctx.form->find(item.id);
                state != nullptr && state->initialized && !state->tip.empty()) {
                pushPlain(text(state->tip) | color(theme.errorColor), 1);
            }
        }
        return rows;
    }

    // ---------------- 其他 (描述层未支持 / 未知组件): fallback 文本 ----------------
    return fallbackRows(item, ctx, out);
}

/// 在组件树内按 id 找控件 (含容器内; 找不到返回 nullptr)
const Item* findControl(const std::vector<Item>& items, std::string_view id) {
    for (const auto& item : items) {
        if (item.kind == "Control" && item.id == id) {
            return &item;
        }
        if (!item.children.empty()) {
            if (const auto* hit = findControl(item.children, id); hit != nullptr) {
                return hit;
            }
        }
    }
    return nullptr;
}

/// 按描述取缺省编辑文本
std::string defaultEditText(const Item& item) {
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
bool validateNumber(const Item& item, UiFormControlState& state) {
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
    if (item.hasMin && v < item.minValue) {
        state.tip = trf("ui.outOfRange", numText(item.minValue));
        return false;
    }
    if (item.hasMax && v > item.maxValue) {
        state.tip = trf("ui.outOfRange", numText(item.maxValue));
        return false;
    }
    state.tip.clear();
    return true;
}

/// 数值控件的步进量 (缺省 1)
double stepOf(const Item& item) {
    return (item.hasStep && item.step > 0.0) ? item.step : 1.0;
}

} // namespace

// ---------------------------------------------------------------------------
// 能力段与适配
// ---------------------------------------------------------------------------

const pluginxx::ui::Capabilities& tuiUiCapabilities() {
    static const pluginxx::ui::Capabilities caps = [] {
        pluginxx::ui::Capabilities out;
        out.kind = "tui";
        for (const std::string_view name : kTuiBlockNames) {
            out.blocks.emplace_back(name);
        }
        for (const std::string_view name : pluginxx::ui::gen::kControlKinds) {
            out.controls.emplace_back(name);
        }
        // 每个字符格相当于多少 u / 默认行距: 取描述层常量 (终端可覆盖, 此处用默认值)
        out.cell    = pluginxx::ui::CellSize{};
        out.gap     = pluginxx::ui::gen::kDefaultGap;
        out.percent = true; // 终端可按可用宽度算比例
        out.aspect  = true; // 按字符宽高比近似 (见 SizedBox)
        return out;
    }();
    return caps;
}

std::vector<pluginxx::ui::Item> adaptItems(const std::vector<pluginxx::ui::Item>& items) {
    pluginxx::ui::AdaptReport report;
    auto out = pluginxx::ui::adaptBlocks(items, tuiUiCapabilities(), &report);
    for (const auto& note : report.notes) {
        XX_LOGW("[tui] ui adapt: {}", note);
    }
    return out;
}

pluginxx::ui::Item adaptItem(const pluginxx::ui::Item& item) {
    pluginxx::ui::AdaptReport report;
    auto out = pluginxx::ui::adaptItem(item, tuiUiCapabilities(), &report);
    for (const auto& note : report.notes) {
        XX_LOGW("[tui] ui adapt: {}", note);
    }
    return out.empty() ? pluginxx::ui::build::text("") : out.front();
}

std::string resolveText(const pluginxx::ui::TextValue& text, const UiRenderCtx& ctx) {
    return textOf(text, ctx);
}

ftxui::Color itemColor(std::string_view tone, const TUITheme& theme) {
    return uiRoleColor(tone, theme);
}

// ---------------------------------------------------------------------------
// 表单状态
// ---------------------------------------------------------------------------

void initFormState(UiFormState& form, const std::vector<pluginxx::ui::Item>& items) {
    for (const auto& item : items) {
        if (item.kind == "Control" && !item.id.empty()) {
            auto& state = form.ensure(item.id);
            if (!state.initialized) {
                state.initialized = true;
                state.selected    = optionIndex(item.options, item.valueJson);
                const Json value  = jsonOf(item.valueJson);
                state.checked     = value.is_boolean() && value.get<bool>();
                state.editText    = controlEditText(item, nullptr);
                state.edited      = false;
            }
        }
        if (!item.children.empty()) {
            initFormState(form, item.children);
        }
    }
}

std::vector<std::string> collectControlIds(const std::vector<pluginxx::ui::Item>& items) {
    std::vector<std::string> out;
    for (const auto& item : items) {
        if (item.kind == "Control" && !item.id.empty()) {
            out.push_back(item.id);
        }
        if (!item.children.empty()) {
            auto sub = collectControlIds(item.children);
            out.insert(out.end(), sub.begin(), sub.end());
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 表单交互
// ---------------------------------------------------------------------------

UiFormAction handleFormControlHit(
    const std::vector<pluginxx::ui::Item>& items,
    UiFormState&                           form,
    std::string_view                       controlId,
    int                                    sub,
    const UiRenderCtx&                     ctx
) {
    const pluginxx::ui::Item* item = findControl(items, controlId);
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

    if (item->control == "checkbox" || item->control == "switch") {
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
        // "点击即提交"属中断表单的域内约定 (描述层只表达"值变化即派发")
        const bool commitOnPick = (ctx.commitOnPick != nullptr) && ctx.commitOnPick(item->id);
        return commitOnPick ? UiFormAction::Submit : UiFormAction::Changed;
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
        const double step = stepOf(*item);
        const double base = parseNumber(
            state.edited ? state.editText : defaultEditText(*item),
            item->integer,
            0.0
        );
        double v = base + ((sub == 1) ? step : -step);
        if (item->hasMin) {
            v = std::max(v, item->minValue);
        }
        if (item->hasMax) {
            v = std::min(v, item->maxValue);
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
    const std::vector<pluginxx::ui::Item>& items,
    UiFormState&                           form,
    const ftxui::Event&                    event
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
        const int n    = static_cast<int>(ids.size());
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
    const pluginxx::ui::Item* item = findControl(items, form.focusedId);
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
                const int dir  = (event == ftxui::Event::ArrowRight) ? 1 : n - 1;
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
            state.selected  = std::clamp(state.selected + delta, 0, std::max(0, n - 1));
            state.tip.clear();
            ++form.version;
            return true;
        }
        return false;
    }
    if (item->control == "checkbox" || item->control == "switch") {
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
        const double step = stepOf(*item);
        const double base = parseNumber(
            state.edited ? state.editText : defaultEditText(*item),
            item->integer,
            0.0
        );
        double v = base + ((event == ftxui::Event::ArrowUp) ? step : -step);
        if (item->hasMin) {
            v = std::max(v, item->minValue);
        }
        if (item->hasMax) {
            v = std::min(v, item->maxValue);
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
                const bool ok = std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.'
                                || c == '-' || c == '+';
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

bool validateForm(const std::vector<pluginxx::ui::Item>& items, UiFormState& form) {
    bool ok = true;
    for (const auto& item : items) {
        if (item.kind == "Control" && !item.id.empty()) {
            auto& state = form.ensure(item.id);
            if (!state.initialized) {
                state.initialized = true;
                state.editText    = defaultEditText(item);
                state.selected    = optionIndex(item.options, item.valueJson);
                const Json value  = jsonOf(item.valueJson);
                state.checked     = value.is_boolean() && value.get<bool>();
            }
            if (item.control == "number") {
                if (!validateNumber(item, state)) {
                    ok = false;
                }
            } else if ((item.control == "buttons" || item.control == "select") && item.options.empty()) {
                // 无候选项: 该控件的值无法确定, 拒绝提交并提示 (与渲染诊断行一致)
                state.tip = std::string{tr("ui.noOptions")};
                ok        = false;
            }
        }
        if (!item.children.empty() && !validateForm(item.children, form)) {
            ok = false;
        }
    }
    ++form.version;
    return ok;
}

utilxx_base::Json formValues(const std::vector<pluginxx::ui::Item>& items, UiFormState& form) {
    utilxx_base::Json values = utilxx_base::Json::object();
    /// 递归收集控件值到同一层对象 (容器内的控件与顶层控件平级)
    std::function<void(const std::vector<pluginxx::ui::Item>&)> collect
        = [&](const std::vector<pluginxx::ui::Item>& list) {
              for (const auto& item : list) {
                  if (item.kind == "Control" && !item.id.empty()) {
                      auto& state = form.ensure(item.id);
                      if (!state.initialized) {
                          initFormState(form, {item});
                      }
                      if (item.control == "checkbox" || item.control == "switch") {
                          values[item.id] = state.checked;
                      } else if (item.control == "buttons" || item.control == "select") {
                          const int idx = std::clamp(
                              state.selected,
                              0,
                              std::max(0, static_cast<int>(item.options.size()) - 1)
                          );
                          if (!item.options.empty()) {
                              values[item.id] = jsonOf(item.options[static_cast<size_t>(idx)].valueJson);
                          }
                      } else if (item.control == "number") {
                          const std::string text
                              = state.edited ? state.editText : defaultEditText(item);
                          const double v = parseNumber(text, item.integer, 0.0);
                          // 整数控件写整数 (与声明口径一致), 浮点控件保留小数
                          if (item.integer) {
                              values[item.id] = utilxx_base::Json(static_cast<int64_t>(v));
                          } else {
                              values[item.id] = utilxx_base::Json(v);
                          }
                      } else if (item.control == "text") {
                          values[item.id] = state.edited ? state.editText : defaultEditText(item);
                      }
                      // 未知控件形态不参与结果 (渲染为不可交互的诊断行)
                  }
                  if (!item.children.empty()) {
                      collect(item.children);
                  }
              }
          };
    collect(items);
    utilxx_base::Json out = utilxx_base::Json::object();
    out["values"]         = std::move(values);
    return out;
}

std::optional<pluginxx::ui::Item> itemFromInterruptBlock(const middleware::InterruptUiBlock& block) {
    // 唯一实现在 lib (`agentxx::middleware::itemOf`): 中断描述 → 组件项的映射
    // 由 TUI 渲染、纯文本降级与"构建器拼中断"共用, 避免各接入点各写一份
    return middleware::itemOf(block);
}

// ---------------------------------------------------------------------------
// 对外入口
// ---------------------------------------------------------------------------

void renderItems(
    const std::vector<pluginxx::ui::Item>& items,
    const UiRenderCtx&                     ctx,
    UiRenderResult&                        out
) {
    // 按本客户端能力适配 (适配结果只含本客户端支持的组件; 已经适配过的内容再过
    // 一次是幂等的)
    const auto adapted = adaptItems(items);
    for (size_t i = 0; i < adapted.size(); ++i) {
        // 文本 + 按钮的隐式同行合并: 面板/Info 段落常用 `Text` + `Button` 表达
        // "前缀 + 按钮" (按钮渲染在文本之后), 此处保持该外观, 各接入点行为一致
        if (adapted[i].kind == "Text" && i + 1 < adapted.size()
            && adapted[i + 1].kind == "Button") {
            if (mergeTextButton(adapted[i], adapted[i + 1], ctx, out)) {
                ++i;
                continue;
            }
        }
        renderItem(adapted[i], ctx, out);
    }
}

void renderItem(const pluginxx::ui::Item& item, const UiRenderCtx& ctx, UiRenderResult& out) {
    auto rows = renderItemRows(item, ctx, out);
    for (auto& row : rows) {
        UiRow outRow;
        outRow.lines = std::max<size_t>(1, row.lines);
        if (!row.regions.empty()) {
            // 顶层行附加反射框: 命中时先定位到行, 再按局部坐标判定具体区域
            // - 用 [OwnedReflect] 让元素自己持有 Box (元素可能被搬进滚动容器/
            //   消息块缓存并跨帧存活, 只存引用会悬空; 见该类说明)
            auto box       = std::make_shared<ftxui::Box>(kNoBox);
            outRow.box     = box;
            outRow.element = std::make_shared<OwnedReflect>(std::move(row.element), std::move(box));
            outRow.regions = std::move(row.regions);
        } else {
            outRow.element = std::move(row.element);
        }
        out.rows.push_back(std::move(outRow));
    }
}

void renderItemJson(const utilxx_base::Json& json, const UiRenderCtx& ctx, UiRenderResult& out) {
    pluginxx::ui::ParseReport report;
    renderItems(pluginxx::ui::parseBlocks(json, {}, &report), ctx, out);
    for (const auto& warning : report.warnings) {
        XX_LOGW("[tui] ui parse: {}", warning);
    }
}

size_t measureItem(const pluginxx::ui::Item& item, const UiRenderCtx& ctx) {
    // 与渲染同源: 渲染一次并统计行数 (不做第二套判定, 避免估算与渲染漂移)
    UiRenderResult out;
    renderItem(adaptItem(item), ctx, out);
    size_t lines = 0;
    for (const auto& row : out.rows) {
        lines += std::max<size_t>(1, row.lines);
    }
    return lines;
}

size_t measureItems(const std::vector<pluginxx::ui::Item>& items, const UiRenderCtx& ctx) {
    size_t lines = 0;
    for (const auto& item : items) {
        lines += measureItem(item, ctx);
    }
    return lines;
}

} // namespace client
} // namespace agentxx
