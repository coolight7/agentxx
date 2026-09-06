#include "agentxx-client/io/tui/plugin_ui_items.h"
#include "agentxx/util/string_util.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"

using namespace ftxui;

namespace agentxx {
namespace client {

PluginButtonRole parseButtonRole(std::string_view role) {
    if (role == "accent") {
        return PluginButtonRole::Accent;
    }
    if (role == "danger") {
        return PluginButtonRole::Danger;
    }
    return PluginButtonRole::Normal;
}

bool hasPluginBinding(
    std::string_view                         plugin,
    const agentxx::plugin::ClientUiRegistry* reg
) {
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

bool parsePluginButton(
    const neograph::json&                    it,
    std::string_view                         plugin,
    const agentxx::plugin::ClientUiRegistry* reg,
    PluginButtonDesc&                        out
) {
    out = PluginButtonDesc{};
    if (!it.is_object()) {
        return false;
    }
    const auto kind = it.value("kind", std::string{"text"});
    std::string label, prefix, actionId, roleStr = "normal";
    std::string argsJson = "{}";
    if (kind == "button") {
        label    = it.value("label", it.value("text", std::string{"Button"}));
        prefix   = it.value("prefix", std::string{});
        actionId = it.value("action_id", std::string{});
        roleStr  = it.value("role", std::string{"normal"});
        if (it.contains("args") && it["args"].is_object()) {
            try {
                argsJson = it["args"].dump();
            } catch (...) {
                argsJson = "{}";
            }
        }
    } else if (kind == "action") {
        // 遗留死 schema: Panel 旧 {"kind":"action","id":...,"label":...}
        // 统一到 button + action_id (action_id = id)
        label    = it.value("label", std::string{"(action)"});
        actionId = it.value("id", std::string{});
        roleStr  = "accent";
    } else {
        return false;
    }
    // label 为空时回退默认 (避免渲染空按钮)
    if (label.empty()) {
        label = "Button";
    }
    out.label      = std::move(label);
    out.prefix     = std::move(prefix);
    out.actionId   = std::move(actionId);
    out.argsJson   = std::move(argsJson);
    out.role       = parseButtonRole(roleStr);
    out.clickable  = !out.actionId.empty() && hasPluginBinding(plugin, reg);
    return true;
}

Element renderPluginButton(const PluginButtonDesc& desc, const TUITheme& theme) {
    std::string btnLabel = desc.label;
    if (!btnLabel.empty() && btnLabel.front() != ' ' && btnLabel.back() != ' ') {
        btnLabel = " " + btnLabel + " ";
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

Element renderPluginTextItem(
    const std::string& textStr,
    const std::string& role,
    const TUITheme&    theme
) {
    Element el = paragraph(textStr);
    if (role == "title") {
        el = el | color(theme.accentColor) | bold;
    } else if (role == "hint") {
        el = el | color(theme.hintColor);
    } else {
        el = el | color(theme.normalColor);
    }
    return el;
}

Element renderPluginDiff(
    std::string_view path,
    std::string_view oldStr,
    std::string_view newStr,
    const TUITheme&  theme,
    int              screenW
) {
    using agentxx::util::DiffLineType;
    const int sw = (screenW > 0) ? screenW : Terminal::Size().dimx;
    auto      diff = agentxx::util::computeLineDiff(oldStr, newStr);
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
        const auto idx = agentxx::util::findIndexByUtf8Length(s, maxChars);
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
        std::vector<const agentxx::util::DiffLine*> dels;
        std::vector<const agentxx::util::DiffLine*> adds;
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

} // namespace client
} // namespace agentxx
