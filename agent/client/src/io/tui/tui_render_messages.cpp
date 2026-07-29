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

} // namespace

// 格式化毫秒到字符串 (秒。毫秒格式，如 "1.2s")
static std::string formatDurationMilliseconds(int32_t milliseconds) {
    const double seconds = static_cast<double>(milliseconds) / 1000.0;
    return fmt::format("{:.1f}s", seconds);
}

ftxui::Element AgentTUI::renderMessages() {
    collapsibleMsgIndices_.clear();
    for (size_t i = 0; i < messages_.size(); ++i) {
        if (messages_[i].role == Message::Role::Thinking
            || messages_[i].role == Message::Role::Tool) {
            collapsibleMsgIndices_.push_back(i);
        }
    }
    collapsibleBoxes_.assign(collapsibleMsgIndices_.size(), ftxui::Box{});

    Elements elements;
    int      collapsibleOrdinal = 0;
    auto     pushBlock          = [&](Element block, bool spacer) {
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
                header.push_back(text(expanded ? "- " : "+ ") | color(theme_.thinkingColor));
                header.push_back(text("[Thinking] ") | color(theme_.thinkingColor));

                // 如果设置了用时，显示格式为 [Thinking] {用时} {内容/缩略内容}
                std::string durationStr;
                if (msg.durationMs > 0 || msg.startTimeMs > 0) {
                    int32_t durationMs = msg.durationMs;
                    if (durationMs <= 0 && msg.startTimeMs > 0) {
                        // 使用 start time 计算当前时刻与开始时刻的差值
                        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch()
                        )
                                             .count();
                        durationMs = static_cast<int32_t>(now - msg.startTimeMs);
                    }
                    if (durationMs > 0) {
                        durationStr = fmt::format("({})", formatDurationMilliseconds(durationMs));
                    }
                }

                if (!expanded) {
                    header.push_back(text(oneLinePreview(msg.text)) | color(theme_.thinkingColor));
                    if (!durationStr.empty()) {
                        header.insert(header.begin(), text(durationStr) | color(theme_.hintColor));
                    }
                } else {
                    // 展开模式：在 header 后追加 duration
                    if (!durationStr.empty()) {
                        header.push_back(text(" ") | color(theme_.hintColor));
                        header.push_back(text(durationStr) | color(theme_.hintColor));
                    }
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
                pushBlock(
                    hbox({
                        text("# ") | color(theme_.systemColor),
                        paragraph(msg.text) | color(theme_.systemColor),
                    }),
                    true
                );
            } break;
            case Message::Role::Tool: {
                const bool expanded   = !msg.collapsed;
                const bool isEditTool = (msg.toolName == "filesystem_edit_text_file");
                Elements   lines;
                Elements   header;
                header.push_back(text(expanded ? "- " : "+ ") | color(theme_.toolColor));
                header.push_back(text("[Tool] ") | color(theme_.toolColor));
                header.push_back(text(msg.toolName) | color(theme_.toolColor));
                if (!expanded) {
                    if (!msg.toolFinished) {
                        header.push_back(text("  running...") | color(theme_.hintColor));
                    } else if (msg.toolHasError) {
                        header.push_back(text("  error: ") | color(theme_.systemColor));
                        header.push_back(
                            text(oneLinePreview(msg.toolResult)) | color(theme_.systemColor)
                        );
                    } else if (isEditTool) {
                        appendEditToolHeader(msg, header);
                    } else {
                        header.push_back(
                            text("  " + oneLinePreview(msg.toolResult)) | color(theme_.toolColor)
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
                            lines.push_back(text("  running...") | color(theme_.hintColor));
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
        // 查找最近的对应角色消息以获取时间信息
        Message* currentMsg = nullptr;
        for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
            if (it->role == currentTokenRole_) {
                currentMsg = &(*it);
                break;
            }
        }

        if (currentTokenRole_ == Message::Role::Thinking) {
            // 流式输出中的 Thinking：显示时间统计（如果存在）
            std::string durationStr;
            if (currentMsg && (currentMsg->durationMs > 0 || currentMsg->startTimeMs > 0)) {
                int32_t durationMs = currentMsg->durationMs;
                if (durationMs <= 0 && currentMsg->startTimeMs > 0) {
                    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch()
                    )
                                         .count();
                    durationMs = static_cast<int32_t>(now - currentMsg->startTimeMs);
                }
                if (durationMs > 0) {
                    durationStr = fmt::format("({})", formatDurationMilliseconds(durationMs));
                }
            }

            pushBlock(
                hbox({
                    text("[Thinking] ") | color(theme_.thinkingColor),
                    text(durationStr.empty() ? "" : (" " + durationStr)) | color(theme_.hintColor),
                    paragraph(oneLinePreview(currentToken_, 80)) | color(theme_.thinkingColor),
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
            text(R"_(
    ___   _____________   ________             
   /   | / ____/ ____/ | / /_  __/    __    __ 
  / /| |/ / __/ __/ /  |/ / / /    __/ /___/ /_
 / ___ / /_/ / /___/ /|  / / /    /_  __/_  __/
/_/  |_\____/_____/_/ |_/ /_/      /_/   /_/   
)_") | bold | color(theme_.accentColor)
                | center,
            text("Type a message to start. [F2] switch model, [Esc] cancel, "
                 "[Ctrl+C] quit.")
                | dim | center,
            filler(),
        });
    }
    return vbox(std::move(elements));
}
