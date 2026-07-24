#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/util/string_util.h"

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
                        appendEditToolHeader(msg, header);
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
                        appendEditToolBody(msg, lines);
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
