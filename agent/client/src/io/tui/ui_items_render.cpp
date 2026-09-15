#include "agentxx-client/io/tui/ui_items_render.h"

#include "agentxx-client/io/tui/markdown_block.h"
#include "agentxx-client/io/tui/text_layout.h"
#include "agentxx/util/diff_util.h"
#include "agentxx/util/log.h"
#include "fmt/format.h"
#include "ftxui/screen/terminal.hpp"
#include "markdown/state_diagram.hpp"
#include <algorithm>

using namespace ftxui;

namespace agentxx {
namespace client {

namespace {

/// 缩进前缀文本 (空缩进返回空串)
std::string indentText(int cols) {
    return (cols > 0) ? std::string(static_cast<size_t>(cols), ' ') : std::string{};
}

/// 内容可用宽度 (扣除缩进; <=0 表示不限宽)
int contentWidth(const UiRenderCtx& ctx, int itemIndent) {
    if (ctx.width <= 0) {
        return 0;
    }
    return std::max(1, ctx.width - ctx.indent - itemIndent);
}

/// diff 块渲染行数 (与 renderPluginDiff 的 side-by-side/统一两种形态一致)
size_t diffBlockLines(const UiItem& item, const UiRenderCtx& ctx) {
    const auto   diff       = agentxx::util::computeLineDiff(item.oldStr, item.newStr);
    const int    sw         = (ctx.width > 0) ? ctx.width : Terminal::Size().dimx;
    const bool   sideBySide = sw >= 100;
    const size_t pathLines  = item.path.empty() ? 0 : 1;
    if (diff.empty()) {
        return pathLines + 1; // "(no changes)"
    }
    return pathLines + (sideBySide ? (diff.size() / 2 + 1) : diff.size());
}

/// mermaid 状态图解析 + 行数估算 (源行数 × 3 + 3; 与历史估算口径一致)
/// - `return` {图形元素(解析失败为空), 行数(解析失败为 0)}
std::pair<Element, size_t> buildDiagram(const UiItem& item, const UiRenderCtx& ctx) {
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
    const int avail = (ctx.width > 0) ? std::max(20, ctx.width - ctx.indent - item.indent - 6) : 0;
    const auto& theme  = *ctx.theme;
    auto        diagEl = markdown::renderMermaidStateDiagram(
        diagram,
        avail,
        theme.normalColor,
        markdown::diagramNodeColor(theme.markdownTheme)
    );
    return {std::move(diagEl), srcLines * 3 + 3};
}

/// 单个文本项折行结果 (与渲染同一口径: 空内容 = 空列表)
std::vector<std::string> textLines(const UiItem& item, const UiRenderCtx& ctx) {
    const auto content = uiItemText(item);
    if (content.empty()) {
        return {};
    }
    if (!item.wrap) {
        return {content};
    }
    return wrapTextToLines(content, std::max(1, contentWidth(ctx, item.indent)));
}

/// 追加一行元素 (基础缩进 + 项缩进)
void pushRow(
    UiRenderResult&      out,
    const UiRenderCtx&   ctx,
    const UiItem&        item,
    Element              el,
    size_t               lines  = 1,
    std::shared_ptr<Box> box    = nullptr,
    std::string          hitId  = {},
    int                  hitSub = 0
) {
    UiRow row;
    row.lines      = std::max<size_t>(1, lines);
    row.hitId      = std::move(hitId);
    row.hitSub     = hitSub;
    row.hitOwner   = ctx.ownerId;
    row.box        = std::move(box);
    const auto pad = indentText(ctx.indent + item.indent);
    if (pad.empty()) {
        row.element = std::move(el);
    } else {
        row.element = hbox({text(pad), std::move(el)});
    }
    out.rows.push_back(std::move(row));
}

} // namespace

std::optional<UiItem> uiItemFromPluginJson(const agentxx::util::Json& item) {
    if (!item.is_object()) {
        return std::nullopt;
    }
    const auto kind = item.value("kind", std::string{"text"});
    UiItem     ui;
    ui.kind = kind;
    if (kind == "text") {
        ui.text = item.value("text", std::string{});
        // 插件 items 的 role 与中断块的 color 是同一套主题色名
        ui.color = item.value("role", std::string{"normal"});
        // 历史渲染语义: title 加粗 / hint 减淡; 文本按宽度硬折行 (行数估算同源)
        ui.bold = (ui.color == "title");
        ui.dim  = (ui.color == "hint");
        ui.wrap = true;
        return ui;
    }
    if (kind == "markdown") {
        ui.text = item.value("text", std::string{});
        return ui;
    }
    if (kind == "button" || kind == "action") {
        // 可点性由渲染时按 ctx.plugin + registry 判定 (解析处不感知绑定)
        if (!parsePluginButton(item, std::string_view{}, nullptr, ui.button)) {
            return std::nullopt;
        }
        ui.hasButton = true;
        return ui;
    }
    if (kind == "separator") {
        return ui;
    }
    if (kind == "diagram") {
        ui.mermaid = item.value("mermaid", std::string{});
        return ui;
    }
    if (kind == "diff") {
        ui.path   = item.value("path", std::string{});
        ui.oldStr = item.value("old_str", item.value("oldStr", std::string{}));
        ui.newStr = item.value("new_str", item.value("newStr", std::string{}));
        return ui;
    }
    // 未知 kind: 忽略 (向前兼容)
    return std::nullopt;
}

std::optional<UiItem> uiItemFromInterruptBlock(const middleware::InterruptUiBlock& block) {
    UiItem ui;
    ui.kind   = block.kind;
    ui.indent = std::max(0, block.indent);
    if (block.kind == "text") {
        ui.text  = block.text;
        ui.color = block.color;
        ui.bold  = block.bold;
        ui.dim   = block.dim;
        ui.wrap  = block.wrap;
        return ui;
    }
    if (block.kind == "markdown") {
        ui.text = block.text;
        return ui;
    }
    if (block.kind == "diff") {
        ui.path   = block.path;
        ui.oldStr = block.oldStr;
        ui.newStr = block.newStr;
        return ui;
    }
    if (block.kind == "separator") {
        return ui;
    }
    if (block.kind == "gap") {
        ui.lines = std::max(0, block.lines);
        return ui;
    }
    // control / submit / custom / 未知: 由中断视图处理
    return std::nullopt;
}

std::string uiItemText(const UiItem& item) {
    return item.text;
}

size_t measureUiItem(const UiItem& item, const UiRenderCtx& ctx) {
    if (item.kind == "text") {
        return textLines(item, ctx).size();
    }
    if (item.kind == "markdown") {
        if (item.text.empty()) {
            return 0;
        }
        return estimateMarkdownLines(item.text, contentWidth(ctx, item.indent));
    }
    if (item.kind == "diff") {
        return diffBlockLines(item, ctx);
    }
    if (item.kind == "separator") {
        return 1;
    }
    if (item.kind == "gap") {
        return static_cast<size_t>(std::max(0, item.lines));
    }
    if (item.kind == "button") {
        return item.hasButton ? 1 : 0;
    }
    if (item.kind == "diagram") {
        return buildDiagram(item, ctx).second;
    }
    return 0;
}

void renderUiItem(const UiItem& item, const UiRenderCtx& ctx, UiRenderResult& out) {
    if (nullptr == ctx.theme) {
        return;
    }
    const auto& theme = *ctx.theme;

    if (item.kind == "text") {
        const Color c = uiRoleColor(item.color, theme);
        for (auto& line : textLines(item, ctx)) {
            Element el = text(line) | color(c);
            if (item.bold) {
                el = el | bold;
            }
            if (item.dim) {
                el = el | dim;
            }
            if (!item.wrap) {
                // 非折行形态: 超宽在右缘裁剪 (不挤压相邻元素)
                el = el | xflex_shrink;
            }
            pushRow(out, ctx, item, std::move(el));
        }
        return;
    }

    if (item.kind == "markdown") {
        if (item.text.empty()) {
            return;
        }
        auto [el, builder] = renderMarkdown(
            item.text,
            uiRoleColor(item.color, theme),
            theme.markdownTheme,
            contentWidth(ctx, item.indent)
        );
        if (builder) {
            out.builders.push_back(std::move(builder));
        }
        pushRow(
            out,
            ctx,
            item,
            std::move(el) | xflex_shrink,
            estimateMarkdownLines(item.text, contentWidth(ctx, item.indent))
        );
        return;
    }

    if (item.kind == "diff") {
        pushRow(
            out,
            ctx,
            item,
            renderPluginDiff(item.path, item.oldStr, item.newStr, theme, ctx.width),
            diffBlockLines(item, ctx)
        );
        return;
    }

    if (item.kind == "separator") {
        pushRow(out, ctx, item, text("─") | color(theme.hintColor) | dim | xflex_shrink);
        return;
    }

    if (item.kind == "gap") {
        for (int i = 0; i < item.lines; ++i) {
            pushRow(out, ctx, item, text(" "));
        }
        return;
    }

    if (item.kind == "button") {
        if (!item.hasButton) {
            return;
        }
        PluginButtonDesc desc = item.button;
        // 可点性: 有 action_id 且插件在 UI 注册表内存在绑定
        desc.clickable = !desc.actionId.empty() && hasPluginBinding(ctx.plugin, ctx.registry);
        auto    box    = desc.clickable ? std::make_shared<Box>() : nullptr;
        Element btn    = renderPluginButton(desc, theme);
        if (box) {
            btn = btn | reflect(*box);
        }
        Element rowEl = std::move(btn);
        if (!desc.prefix.empty()) {
            // 带前导前缀的按钮 (如 "|- "): 前缀与按钮同行
            rowEl = hbox({text(desc.prefix) | color(theme.normalColor), std::move(rowEl)});
        }
        pushRow(out, ctx, item, std::move(rowEl) | xflex_shrink, 1, box, desc.actionId);
        if (box) {
            // 命中参数随行携带 (派发时透传)
            out.rows.back().hitArgs = desc.argsJson;
        }
        return;
    }

    if (item.kind == "diagram") {
        auto [el, lines] = buildDiagram(item, ctx);
        if (lines == 0) {
            return;
        }
        pushRow(out, ctx, item, std::move(el) | flex, lines);
        return;
    }
    // 未知 kind: 忽略
}

} // namespace client
} // namespace agentxx
