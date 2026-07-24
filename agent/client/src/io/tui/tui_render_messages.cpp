#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/util/string_util.h"
#include "ftxui/screen/terminal.hpp"

using namespace ftxui;

namespace {
std::string oneLinePreview(const std::string& s, size_t max = 60) {
    const auto  nl   = s.find('\n');
    std::string line = (nl == std::string::npos) ? s : s.substr(0, nl);
    const auto  idx  = agentxx::util::findIndexByUtf8Length(line, max);
    if (idx > 0 && idx < line.size()) {
        line.resize(idx);
        line += "...";
    }
    return line;
}
} // namespace

int AgentTUI::focusBlockCount() const {
    int n = static_cast<int>(messages_.size());
    if (isStreaming_ && !currentToken_.empty()) {
        ++n;
    }
    return n;
}

ftxui::Element AgentTUI::renderMessages() {
    const int count    = focusBlockCount();
    int       focusIdx = -1;
    if (count > 0) {
        focusIdx = stickToBottom_ ? (count - 1) : std::clamp(scrollAnchorIndex_, 0, count - 1);
    }

    collapsibleMsgIndices_.clear();
    for (size_t i = 0; i < messages_.size(); ++i) {
        if (messages_[i].role == Message::Role::Thinking
            || messages_[i].role == Message::Role::Tool) {
            collapsibleMsgIndices_.push_back(i);
        }
    }
    collapsibleBoxes_.assign(collapsibleMsgIndices_.size(), ftxui::Box{});

    Elements elements;
    int      idx                = 0;
    int      collapsibleOrdinal = 0;
    auto     pushBlock          = [&](Element block, bool spacer) {
        if (idx == focusIdx) {
            block = std::move(block) | focus;
        }
        ++idx;
        elements.push_back(std::move(block));
        if (spacer) {
            elements.push_back(text(""));
        }
    };

    for (const auto& msg : messages_) {
        switch (msg.role) {
            case Message::Role::User: {
                pushBlock(
                    hbox({
                        text("> ") | color(theme_.userColor),
                        paragraph(msg.text) | color(theme_.userColor),
                    }),
                    true
                );
            } break;
            case Message::Role::Assistant: {
                pushBlock(paragraph(msg.text) | color(theme_.assistantColor), true);
            } break;
            case Message::Role::Thinking: {
                const bool expanded = !msg.collapsed;
                Elements   lines;
                Elements   header;
                header.push_back(text(expanded ? "- " : "+ ") | color(theme_.hintColor));
                header.push_back(text("[Thinking] ") | color(theme_.thinkingColor));
                if (!expanded) {
                    header.push_back(text(oneLinePreview(msg.text)) | color(theme_.thinkingColor));
                }
                lines.push_back(hbox(std::move(header)));
                if (expanded) {
                    lines.push_back(paragraph(msg.text) | color(theme_.thinkingColor));
                }
                Element block
                    = vbox(std::move(lines)) | reflect(collapsibleBoxes_[collapsibleOrdinal]);
                ++collapsibleOrdinal;
                pushBlock(std::move(block), true);
                break;
            }
            case Message::Role::System: {
                pushBlock(paragraph(msg.text) | color(theme_.systemColor), true);
            } break;
            case Message::Role::Tool: {
                const bool expanded   = !msg.collapsed;
                const bool isEditTool = (msg.toolName == "filesystem_edit_text_file");
                Elements   lines;
                Elements   header;
                header.push_back(text(expanded ? "- " : "+ ") | color(theme_.hintColor));
                header.push_back(text("[Tool] ") | color(theme_.toolColor));
                header.push_back(text(msg.toolName) | color(theme_.toolColor));
                if (!expanded) {
                    if (!msg.toolFinished) {
                        header.push_back(text("  running...") | color(theme_.hintColor) | dim);
                    } else if (msg.toolHasError) {
                        header.push_back(text("  error: ") | color(theme_.systemColor));
                        header.push_back(
                            text(oneLinePreview(msg.toolResult)) | color(theme_.systemColor) | dim
                        );
                    } else if (isEditTool) {
                        std::string path;
                        try {
                            path = neograph::json::parse(msg.text).value("path", std::string{});
                        } catch (...) {
                        }
                        if (!path.empty()) {
                            header.push_back(text("  " + path) | color(theme_.toolColor) | dim);
                        }
                    } else {
                        header.push_back(
                            text("  " + oneLinePreview(msg.toolResult)) | color(theme_.toolColor)
                            | dim
                        );
                    }
                }
                lines.push_back(hbox(std::move(header)));

                if (expanded) {
                    if (isEditTool) {
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
                        if (msg.toolFinished && msg.toolHasError) {
                            lines.push_back(hbox({
                                text("  error: ") | color(theme_.systemColor),
                                paragraph(msg.toolResult) | color(theme_.systemColor),
                            }));
                        }
                    } else {
                        if (!msg.text.empty()) {
                            lines.push_back(hbox({
                                text("  args: ") | color(theme_.hintColor),
                                paragraph(msg.text) | color(theme_.toolColor),
                            }));
                        }
                        if (msg.toolFinished) {
                            auto rc = msg.toolHasError ? theme_.systemColor : theme_.toolColor;
                            lines.push_back(hbox({
                                text(msg.toolHasError ? "  error: " : "  result: ") | color(rc),
                                paragraph(msg.toolResult) | color(rc),
                            }));
                        } else {
                            lines.push_back(text("  running...") | color(theme_.hintColor) | dim);
                        }
                    }
                }

                Element block
                    = vbox(std::move(lines)) | reflect(collapsibleBoxes_[collapsibleOrdinal]);
                ++collapsibleOrdinal;
                pushBlock(std::move(block), true);
            } break;
        }
    }

    if (isStreaming_ && !currentToken_.empty()) {
        if (currentTokenRole_ == Message::Role::Thinking) {
            pushBlock(
                hbox({
                    text("[Thinking] ") | color(theme_.thinkingColor),
                    paragraph(currentToken_) | color(theme_.thinkingColor),
                }),
                false
            );
        } else {
            pushBlock(paragraph(currentToken_) | color(theme_.assistantColor), false);
        }
    }

    if (elements.empty()) {
        return vbox({
            filler(),
            text("Agentxx TUI") | bold | color(theme_.accentColor) | center,
            text("Type a message to start. [F2] switch model, [Esc] cancel, "
                 "[Ctrl+C] quit.")
                | dim | center,
            filler(),
        });
    }
    return vbox(std::move(elements));
}

ftxui::Element AgentTUI::renderEditToolDiff(const std::string& oldStr, const std::string& newStr) {
    using agentxx::util::DiffLineType;
    auto diff = agentxx::util::computeLineDiff(oldStr, newStr);
    if (diff.empty()) {
        return text("  (no changes)") | color(theme_.hintColor);
    }

    const int  screenW    = ftxui::Terminal::Size().dimx;
    const bool sideBySide = screenW >= 100;

    auto trunc = [](const std::string& s, size_t maxChars) {
        const auto idx = agentxx::util::findIndexByUtf8Length(s, maxChars);
        if (idx > 0 && idx < s.size()) {
            return s.substr(0, idx) + "...";
        }
        return s;
    };

    if (!sideBySide) {
        Elements lines;
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
                text(l.text) | color(c),
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
    auto makeCell = [&](const std::string& sign, int no, const std::string& txt, ftxui::Color c) {
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
