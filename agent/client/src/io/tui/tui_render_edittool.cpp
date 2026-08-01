#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/util/diff_util.h"
#include "agentxx/util/string_util.h"
#include "ftxui/screen/terminal.hpp"
#include <fmt/format.h>

using namespace ftxui;

// filesystem_edit_text_file 的特化渲染: 以 git diff 形式展示文件修改
// (折叠态显示文件路径, 展开态显示路径 + diff 对比 + 错误)

void AgentTUI::appendEditToolHeader(const Message& msg, Elements& header) {
    std::string path;
    try {
        path = neograph::json::parse(msg.text).value("path", std::string{});
    } catch (...) {
    }
    if (!path.empty()) {
        header.push_back(text("  " + path) | color(theme_.toolColor) | dim);
    }
}

void AgentTUI::appendEditToolBody(const Message& msg, Elements& lines) {
    std::string path;
    std::string oldStr;
    std::string newStr;
    try {
        auto args = neograph::json::parse(msg.text);
        path      = args.value("path", std::string{});
        oldStr    = args.value("old_str", std::string{});
        newStr    = args.value("new_str", std::string{});
    } catch (...) {
    }
    if (!path.empty()) {
        lines.push_back(hbox({
            text("  file: ") | color(theme_.hintColor),
            text(path) | color(theme_.toolColor),
        }));
    }
    lines.push_back(renderEditToolDiff(oldStr, newStr));
}

ftxui::Element AgentTUI::renderEditToolDiff(std::string_view oldStr, std::string_view newStr) {
    using agentxx::util::DiffLineType;
    auto diff = agentxx::util::computeLineDiff(oldStr, newStr);
    if (diff.empty()) {
        return text("  (no changes)") | color(theme_.hintColor);
    }

    const int  screenW    = ftxui::Terminal::Size().dimx;
    const bool sideBySide = screenW >= 100;

    auto trunc = [](std::string_view s, size_t maxChars) -> std::string {
        const auto idx = agentxx::util::findIndexByUtf8Length(s, maxChars);
        if (idx > 0 && idx < s.size()) {
            return fmt::format("{}...", s.substr(0, idx));
        }
        return std::string{s};
    };

    if (!sideBySide) {
        // 单块模式: 按可用宽度截断长行 (减去前缀 "x " 2 列与少量边距),
        // 避免 CJK 等宽字符被 stencil 从中间截断显示异常
        const size_t maxChars = static_cast<size_t>(std::max(20, screenW - 6));
        Elements     lines;
        for (const auto& l : diff) {
            ftxui::Color c      = theme_.toolColor;
            std::string  prefix = " ";
            if (l.type == DiffLineType::Add) {
                c      = theme_.promptColor;
                prefix = "+";
            } else if (l.type == DiffLineType::Delete) {
                c      = theme_.systemColor;
                prefix = "-";
            }
            lines.push_back(hbox({
                text(prefix) | color(c),
                text(" ") | color(theme_.hintColor),
                text(trunc(l.text, maxChars)) | color(c),
            }));
        }
        return vbox(std::move(lines));
    }

    const int colW  = std::max(20, (screenW - 3) / 2);
    const int textW = std::max(8, colW - 6);

    Elements leftLines;
    Elements rightLines;
    auto     emptyCell = [&]() {
        return text(" ") | color(theme_.hintColor);
    };
    auto makeCell = [&](std::string_view sign, int no, std::string_view txt, ftxui::Color c) {
        std::string noStr = (no > 0) ? std::to_string(no) : std::string{};
        return hbox({
            text(sign) | color(c) | bold,
            text(noStr) | color(theme_.hintColor) | size(WIDTH, EQUAL, 4),
            text(" ") | color(theme_.hintColor),
            text(trunc(txt, static_cast<size_t>(textW))) | color(c),
        });
    };

    size_t i = 0;
    while (i < diff.size()) {
        if (diff[i].type == DiffLineType::Context) {
            leftLines.push_back(makeCell(" ", diff[i].oldLineNo, diff[i].text, theme_.toolColor));
            rightLines.push_back(makeCell(" ", diff[i].newLineNo, diff[i].text, theme_.toolColor));
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
                    ? makeCell("-", dels[k]->oldLineNo, dels[k]->text, theme_.systemColor)
                    : emptyCell()
            );
            rightLines.push_back(
                (k < adds.size())
                    ? makeCell("+", adds[k]->newLineNo, adds[k]->text, theme_.promptColor)
                    : emptyCell()
            );
        }
    }

    return hbox({
        vbox(std::move(leftLines)) | flex,
        separator(),
        vbox(std::move(rightLines)) | flex,
    });
}
