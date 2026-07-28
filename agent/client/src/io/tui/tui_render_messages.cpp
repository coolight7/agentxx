#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/util/string_util.h"

using namespace ftxui;

namespace {

std::string oneLinePreview(std::string_view s, size_t max = 60) {
    const auto  nl = s.find('\n');
    std::string line{(nl == std::string_view::npos) ? s : s.substr(0, nl)};
    const auto  idx = agentxx::util::findIndexByUtf8Length(line, max);
    if (idx > 0 && idx < line.size()) {
        line.resize(idx);
        line += "...";
    }
    return line;
}

/// 解码一个 UTF-8 字符, 返回 codepoint 和字节长度
inline std::pair<char32_t, size_t> decodeUtf8(std::string_view s, size_t pos) {
    unsigned char b = static_cast<unsigned char>(s[pos]);
    if (b < 0x80) {
        return {b, 1};
    }
    if (b >= 0xC0 && b < 0xE0 && pos + 1 < s.size()) {
        return {((b & 0x1Fu) << 6) | (static_cast<unsigned char>(s[pos + 1]) & 0x3Fu), 2};
    }
    if (b >= 0xE0 && b < 0xF0 && pos + 2 < s.size()) {
        return {((b & 0x0Fu) << 12)
                    | ((static_cast<unsigned char>(s[pos + 1]) & 0x3Fu) << 6)
                    | (static_cast<unsigned char>(s[pos + 2]) & 0x3Fu),
                3};
    }
    if (b >= 0xF0 && pos + 3 < s.size()) {
        return {((b & 0x07u) << 18)
                    | ((static_cast<unsigned char>(s[pos + 1]) & 0x3Fu) << 12)
                    | ((static_cast<unsigned char>(s[pos + 2]) & 0x3Fu) << 6)
                    | (static_cast<unsigned char>(s[pos + 3]) & 0x3Fu),
                4};
    }
    return {b, 1};
}

/// 判断 codepoint 是否为宽字符 (CJK 等, 终端显示宽度为 2)
inline bool isWideChar(char32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303F)
           || (cp >= 0x3040 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF)
           || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF)
           || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF)
           || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60)
           || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

/// 将单行文本按显示宽度换行
std::vector<std::string> wrapLine(std::string_view line, int maxWidth) {
    std::vector<std::string> result;
    if (maxWidth <= 0) {
        result.emplace_back(line);
        return result;
    }

    size_t i = 0;
    while (i < line.size()) {
        int         curWidth  = 0;
        size_t      lastBreak = std::string::npos;
        std::string curLine;

        while (i < line.size()) {
            auto [cp, len] = decodeUtf8(line, i);
            int charW = isWideChar(cp) ? 2 : 1;

            if (curWidth + charW > maxWidth) {
                break;
            }

            curWidth += charW;
            curLine.append(line.data() + i, len);
            i += len;

            if (cp == ' ' || isWideChar(cp)) {
                lastBreak = curLine.size();
            }
        }

        if (i < line.size() && lastBreak != std::string::npos
            && lastBreak < curLine.size()) {
            i = i - (curLine.size() - lastBreak);
            curLine.resize(lastBreak);
            while (!curLine.empty() && curLine.back() == ' ') {
                curLine.pop_back();
            }
        }

        if (curLine.empty() && i < line.size()) {
            auto [cp, len] = decodeUtf8(line, i);
            curLine.append(line.data() + i, len);
            i += len;
        }

        result.push_back(std::move(curLine));

        while (i < line.size() && line[i] == ' ') {
            ++i;
        }
    }

    if (result.empty()) {
        result.emplace_back();
    }
    return result;
}

/// 将文本按 '\n' 拆行后再按 maxWidth 自动换行, 生成 vbox 元素
/// 使用 reflect 测量的实际宽度, 自适应消息框真实宽度
ftxui::Element wrappedText(std::string_view text, int maxWidth) {
    ftxui::Elements lines;
    size_t          start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        std::string_view line
            = (end == std::string_view::npos) ? text.substr(start)
                                              : text.substr(start, end - start);
        auto wrapped = wrapLine(line, maxWidth);
        for (auto& wl : wrapped) {
            lines.push_back(ftxui::text(std::move(wl)));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
        if (start == text.size()) {
            lines.push_back(ftxui::text(""));
            break;
        }
    }
    if (lines.empty()) {
        lines.push_back(ftxui::text(""));
    }
    return ftxui::vbox(std::move(lines));
}

} // namespace

ftxui::Element AgentTUI::renderMessages() {
    /// 从 reflect 测量的消息区域 box 计算实际可用换行宽度
    int wrapWidth = messageAreaBox_.x_max - messageAreaBox_.x_min;
    if (wrapWidth <= 0) {
        wrapWidth = std::max(20, ftxui::Terminal::Size().dimx - 10);
    }

    collapsibleMsgIndices_.clear();
    for (size_t i = 0; i < messages_.size(); ++i) {
        if (messages_[i].role == Message::Role::Thinking
            || messages_[i].role == Message::Role::Tool) {
            collapsibleMsgIndices_.push_back(i);
        }
    }
    collapsibleBoxes_.assign(collapsibleMsgIndices_.size(), ftxui::Box{});

    /// 计算总块数 (消息 + 流式输出 + 底部锚点)
    int blockCount = static_cast<int>(messages_.size());
    if (isStreaming_ && !currentToken_.empty()) {
        ++blockCount;
    }
    if (blockCount > 0) {
        ++blockCount; // 底部锚点
    }
    messagesBlockCount_ = blockCount;

    /// 确定聚焦块索引
    int focusIdx = -1;
    if (blockCount > 0) {
        focusIdx = stickToBottom_ ? (blockCount - 1)
                                  : std::clamp(messagesSelector_, 0, blockCount - 1);
    }

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
                        wrappedText(msg.text, wrapWidth - 2) | color(theme_.userColor),
                    }),
                    true
                );
            } break;
            case Message::Role::Assistant: {
                pushBlock(wrappedText(msg.text, wrapWidth) | color(theme_.assistantColor), true);
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
                    lines.push_back(wrappedText(msg.text, wrapWidth) | color(theme_.thinkingColor));
                }
                Element block
                    = vbox(std::move(lines)) | reflect(collapsibleBoxes_[collapsibleOrdinal]);
                ++collapsibleOrdinal;
                pushBlock(std::move(block), true);
                break;
            }
            case Message::Role::System: {
                pushBlock(wrappedText(msg.text, wrapWidth) | color(theme_.systemColor), true);
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
                                wrappedText(msg.text, wrapWidth - 8) | color(theme_.toolColor),
                            }));
                        }
                        if (msg.toolFinished) {
                            auto rc = msg.toolHasError ? theme_.systemColor : theme_.toolColor;
                            lines.push_back(hbox({
                                text(msg.toolHasError ? "  error: " : "  result: ") | color(rc),
                                wrappedText(msg.toolResult, wrapWidth - 10) | color(rc),
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
                    wrappedText(currentToken_, wrapWidth - 11) | color(theme_.thinkingColor),
                }),
                false
            );
        } else {
            pushBlock(wrappedText(currentToken_, wrapWidth) | color(theme_.assistantColor), false);
        }
    }

    /// 底部锚点: stickToBottom_ 时聚焦此元素确保滚动到真正底部
    if (!elements.empty()) {
        Element anchor = text("");
        if (idx == focusIdx) {
            anchor = anchor | focus;
        }
        ++idx;
        elements.push_back(std::move(anchor));
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
