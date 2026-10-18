#include "agentxx-client/io/tui/plugin_ui_items.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include "utilxx_base/string_util.h"
#include <exception>
#include <optional>

using namespace ftxui;

namespace agentxx {
namespace client {

bool hasPluginBinding(std::string_view plugin, const agentxx::plugin::ClientUiRegistry* reg) {
    if (!reg || plugin.empty()) {
        return false;
    }
    for (const auto& b : reg->actionBindings) {
        if (b.plugin == plugin && b.cb) {
            return true;
        }
    }
    return false;
}

bool hasPluginBindingFor(
    std::string_view                         plugin,
    std::string_view                         ownerId,
    const agentxx::plugin::ClientUiRegistry* reg
) {
    if (!reg || plugin.empty()) {
        return false;
    }
    // 精确匹配优先
    for (const auto& b : reg->actionBindings) {
        if (b.plugin == plugin && b.targetId == ownerId && b.cb) {
            return true;
        }
    }
    // 回落实例级兜底 ("")
    for (const auto& b : reg->actionBindings) {
        if (b.plugin == plugin && b.targetId.empty() && b.cb) {
            return true;
        }
    }
    return false;
}

Element renderPluginButton(const PluginButtonDesc& desc, const TUITheme& theme) {
    std::string btnLabel = desc.label;
    if (!btnLabel.empty()) {
        btnLabel = fmt::format("[ {} ]", btnLabel);
    }
    Element btn;
    switch (desc.role) {
        case PluginButtonRole::Accent:
            btn = text(btnLabel) | bgcolor(theme.buttonActiveBgColor)
                  | color(theme.buttonActiveTextColor) | bold;
            break;
        case PluginButtonRole::Danger:
            btn = text(btnLabel) | bgcolor(theme.errorColor) | color(theme.buttonTextColor) | bold;
            break;
        case PluginButtonRole::Normal:
        default:
            btn = text(btnLabel) | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor)
                  | bold;
            break;
    }
    return btn;
}

ftxui::Color uiRoleColor(std::string_view role, const TUITheme& theme) {
    if (role == "title" || role == "accent") {
        return theme.accentColor;
    }
    if (role == "hint") {
        return theme.hintColor;
    }
    if (role == "error") {
        return theme.errorColor;
    }
    if (role == "thinking") {
        return theme.thinkingColor;
    }
    if (role == "tool") {
        return theme.toolColor;
    }
    if (role == "text" || role == "user") {
        return theme.userColor;
    }
    if (role == "assistant" || role == "content") {
        return theme.assistantColor;
    }
    return theme.normalColor;
}

Element renderPluginDiff(
    std::string_view path,
    std::string_view oldStr,
    std::string_view newStr,
    const TUITheme&  theme,
    int              screenW
) {
    using utilxx::DiffLineType;
    const int sw   = (screenW > 0) ? screenW : Terminal::Size().dimx;
    auto      diff = utilxx::computeLineDiff(oldStr, newStr);
    if (diff.empty()) {
        Elements els;
        if (!path.empty()) {
            els.push_back(hbox({
                text("  file: ") | color(theme.hintColor),
                text(std::string{path}) | color(theme.toolColor) | xflex_shrink,
            }));
        }
        els.push_back(text("  (no changes)") | color(theme.hintColor));
        return vbox(std::move(els));
    }

    const bool sideBySide = sw >= 100;

    auto trunc = [](std::string_view s, size_t maxChars) -> std::string {
        const auto idx = utilxx_base::findIndexByUtf8Length(s, maxChars);
        if (idx > 0 && idx < s.size()) {
            return fmt::format("{}...", s.substr(0, idx));
        }
        return std::string{s};
    };

    Elements els;
    if (!path.empty()) {
        els.push_back(hbox({
            text("  file: ") | color(theme.hintColor),
            text(std::string{path}) | color(theme.toolColor) | xflex_shrink,
        }));
    }

    if (!sideBySide) {
        const size_t maxChars = static_cast<size_t>(std::max(20, sw - 6));
        Elements     lines;
        for (const auto& l : diff) {
            Color       c      = theme.toolColor;
            std::string prefix = " ";
            if (l.type == DiffLineType::Add) {
                c      = theme.accentColor;
                prefix = "+";
            } else if (l.type == DiffLineType::Delete) {
                c      = theme.errorColor;
                prefix = "-";
            }
            lines.push_back(hbox({
                text(prefix) | color(c),
                text(" ") | color(theme.hintColor),
                text(trunc(l.text, maxChars)) | color(c),
            }));
        }
        els.push_back(vbox(std::move(lines)));
        return vbox(std::move(els));
    }

    const int colW  = std::max(20, (sw - 3) / 2);
    const int textW = std::max(8, colW - 6);

    Elements leftLines;
    Elements rightLines;
    auto     emptyCell = [&]() {
        return text(" ") | color(theme.hintColor);
    };
    auto makeCell = [&](std::string_view sign, int no, std::string_view txt, Color c) {
        std::string noStr = (no > 0) ? std::to_string(no) : std::string{};
        return hbox({
            text(sign) | color(c),
            text(noStr) | color(theme.hintColor) | size(WIDTH, EQUAL, 4),
            text(" "),
            text(trunc(txt, static_cast<size_t>(textW))) | color(c) | xflex_shrink,
        });
    };

    size_t i = 0;
    while (i < diff.size()) {
        if (diff[i].type == DiffLineType::Context) {
            leftLines.push_back(makeCell(" ", diff[i].oldLineNo, diff[i].text, theme.toolColor));
            rightLines.push_back(makeCell(" ", diff[i].newLineNo, diff[i].text, theme.toolColor));
            ++i;
            continue;
        }
        std::vector<const utilxx::DiffLine*> dels;
        std::vector<const utilxx::DiffLine*> adds;
        while (i < diff.size() && diff[i].type == DiffLineType::Delete) {
            dels.push_back(&diff[i]);
            ++i;
        }
        while (i < diff.size() && diff[i].type == DiffLineType::Add) {
            adds.push_back(&diff[i]);
            ++i;
        }
        const size_t maxk = std::max(dels.size(), adds.size());
        for (size_t k = 0; k < maxk; ++k) {
            leftLines.push_back(
                (k < dels.size())
                    ? makeCell("-", dels[k]->oldLineNo, dels[k]->text, theme.errorColor)
                    : emptyCell()
            );
            rightLines.push_back(
                (k < adds.size())
                    ? makeCell("+", adds[k]->newLineNo, adds[k]->text, theme.accentColor)
                    : emptyCell()
            );
        }
    }

    els.push_back(hbox({
        vbox(std::move(leftLines)) | flex,
        separator(),
        vbox(std::move(rightLines)) | flex,
    }));
    return vbox(std::move(els));
}

std::optional<PluginDiffPayload> parsePluginDiffPayload(std::string_view payloadJson) {
    utilxx_base::Json root;
    try {
        // 空 payload 按空对象处理 (与原实现一致: 打开一个"无变化"的 diff 弹窗)
        root = utilxx_base::Json::parse(payloadJson.empty() ? std::string_view{"{}"} : payloadJson);
    } catch (const std::exception&) {
        return std::nullopt;
    }

    PluginDiffPayload out;

    /// 收一个段落对象 (非对象元素跳过; 两侧都为空 = 没有内容的段, 跳过)
    auto takeSegment = [&out](const utilxx_base::Json& item) {
        if (!item.is_object()) {
            return;
        }
        PluginDiffSegment seg;
        seg.oldStr = item.value("old_str", std::string{});
        seg.newStr = item.value("new_str", std::string{});
        if (seg.oldStr.empty() && seg.newStr.empty()) {
            return;
        }
        out.segments.push_back(std::move(seg));
    };

    // 裸数组: 若干段落 (没有文件路径)
    if (root.is_array()) {
        for (const auto& item : root) {
            takeSegment(item);
        }
        return out;
    }
    if (!root.is_object()) {
        return std::nullopt;
    }

    out.path = root.value("path", std::string{});
    if (root.contains("edits")) {
        // `edits` 与 `old_str`/`new_str` 互斥 (与 edit 工具参数的规则一致):
        // 给了 `edits` 就只看它, 不再回退到单段写法
        const auto& edits = root["edits"];
        if (edits.is_array()) {
            for (const auto& item : edits) {
                takeSegment(item);
            }
        } else {
            takeSegment(edits);
        }
        return out;
    }
    // 单段旧写法: 顶层 old_str/new_str
    takeSegment(root);
    return out;
}

} // namespace client
} // namespace agentxx
