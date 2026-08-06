#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/util/diff_util.h"
#include "agentxx/util/string_util.h"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include <markdown/dom_builder.hpp>
#include <markdown/parser.hpp>

using namespace ftxui;

namespace {

std::pair<Element, std::unique_ptr<markdown::DomBuilder>> renderMarkdown(
    std::string_view       content,
    Color                  color,
    markdown::Theme const& mdTheme,
    int                    maxWidth = 0
) {
    if (content.empty()) {
        return {ftxui::text(""), nullptr};
    }
    auto parser  = markdown::make_cmark_parser();
    auto ast     = parser->parse(content);
    auto builder = std::make_unique<markdown::DomBuilder>();
    if (maxWidth > 0) {
        builder->set_max_width(maxWidth);
    }
    auto el = builder->build(ast, -1, mdTheme);
    return {el | ftxui::color(color), std::move(builder)};
}

} // namespace

MessageListComponent::MessageListComponent(TUICtx& ctx) :
    ctx_(ctx) {
    scrollable_ = std::make_shared<Scrollable>([this]() {
        return buildItems();
    });
    Add(scrollable_);
}

void MessageListComponent::invalidateCache() {
    cache_.clear();
    prevMsgCount_ = 0;
}

Element MessageListComponent::OnRender() {
    // 由上一帧 viewport 可见区域反推可折叠消息的鼠标命中区域。
    // 必须在 scrollable_->Render() 之前计算: 此时 itemMeta_ 和 visibleBoxes()
    // 均为上一帧数据, 与用户当前看到的屏幕内容一致。
    collapsibleBoxes_.clear();
    collapsibleIndices_.clear();
    {
        const auto& vboxes = scrollable_->visibleBoxes();
        for (size_t i = 0; i < itemMeta_.size() && i < vboxes.size(); ++i) {
            const auto& meta = itemMeta_[i];
            if (!meta.collapsible || meta.messageIndex < 0) {
                continue;
            }
            if (vboxes[i].IsEmpty()) {
                continue;
            }
            collapsibleBoxes_.push_back(vboxes[i]);
            collapsibleIndices_.push_back(static_cast<size_t>(meta.messageIndex));
        }
    }

    return hbox({
               text("   "),
               scrollable_->Render() | bold | flex,
               text("   "),
           })
           | reflect(areaBox_);
}

bool MessageListComponent::OnEvent(Event event) {
    if (!event.is_mouse()) {
        return false;
    }
    const auto& mouse = event.mouse();
    if (handleCollapsibleClick(mouse)) {
        return true;
    }
    return scrollable_->OnEvent(event);
}

bool MessageListComponent::handleCollapsibleClick(const Mouse& mouse) {
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    if (areaBox_.x_max < areaBox_.x_min) {
        return false; // 尚未布局
    }
    if (mouse.x < areaBox_.x_min || mouse.x > areaBox_.x_max) {
        return false;
    }
    for (size_t k = 0; k < collapsibleBoxes_.size() && k < collapsibleIndices_.size(); ++k) {
        if (mouse.y < collapsibleBoxes_[k].y_min || mouse.y > collapsibleBoxes_[k].y_max) {
            continue;
        }
        const size_t mi      = collapsibleIndices_[k];
        bool         handled = false;
        ctx_.state->mutate([&](TUIRenderState& st) {
            if (mi < st.messages.size()) {
                auto& msg     = ctx_.state->mutableMessage(st, mi);
                msg.collapsed = !msg.collapsed;
                handled       = true;
            }
        });
        if (handled) {
            ctx_.postRedraw();
        }
        return handled;
    }
    return false;
}

int64_t MessageListComponent::messageSignature(const TUIMessage& msg) {
    auto combine = [](uint64_t seed, uint64_t v) -> uint64_t {
        return seed ^ (v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    };
    uint64_t h = 0;
    h          = combine(h, static_cast<uint64_t>(msg.role));
    h          = combine(h, msg.collapsed);
    h          = combine(h, msg.toolFinished);
    h          = combine(h, msg.durationMs);
    h          = combine(h, msg.startTimeMs);
    h          = combine(h, std::hash<std::string>{}(msg.toolName));
    h          = combine(h, std::hash<std::string>{}(msg.text));
    h          = combine(h, std::hash<std::string>{}(msg.toolResult));
    return static_cast<int64_t>(h);
}

std::vector<ScrollItem> MessageListComponent::buildItems() {
    itemMeta_.clear();
    const auto& st           = *ctx_.frameState;
    const auto& theme        = *ctx_.theme;
    const int   msgListWidth = scrollable_->contentWidth();

    const bool hasStreamingToken = st.isStreaming && st.currentToken && !st.currentToken->empty();
    if (st.messages.empty() && !hasStreamingToken) {
        auto banner = vbox({
            filler(),
            text(R"_(
    ___   _____________   ________
   /   | / ____/ ____/ | / /_  __/    __    __
  / /| |/ / __/ __/ /  |/ / / /    __/ /___/ /_
 / ___ / /_/ / /___/ /|  / / /    /_  __/_  __/
/_/  |_\____/_____/_/ |_/ /_/      /_/   /_/
)_") | bold | color(theme.accentColor)
                | center,
            text("Type a message to start. [F2] switch model, [Esc] cancel, "
                 "[Ctrl+C] quit.")
                | color(theme.hintColor) | center,
            filler(),
        });
        return {
            ScrollItem{std::move(banner), true}
        };
    }

    if (cache_.size() > st.messages.size()) {
        cache_.clear();
    }
    while (cache_.size() < st.messages.size()) {
        cache_.push_back(MessageCache{});
    }

    if (st.messages.size() > prevMsgCount_ && cache_.size() > kMaxCache) {
        const size_t evictEnd = cache_.size() - kMaxCache;
        for (size_t i = 0; i < evictEnd; ++i) {
            auto& c = cache_[i];
            if (c.element) {
                c.element = nullptr;
                c.mdBuilders.clear();
            }
        }
    }
    prevMsgCount_ = st.messages.size();

    std::vector<ScrollItem> items;
    items.reserve(st.messages.size() + 1);

    for (size_t i = 0; i < st.messages.size(); ++i) {
        const auto& msg   = *st.messages[i];
        auto&       cache = cache_[i];
        int64_t     sig   = messageSignature(msg);
        if (cache.sig != sig || cache.cachedWidth != msgListWidth || !cache.element) {
            cache.mdBuilders.clear();
            cache.element     = vbox({
                buildMessageBlock(msg, msgListWidth, cache.mdBuilders),
                text(""),
            });
            cache.sig         = sig;
            cache.cachedWidth = msgListWidth;
        }
        items.push_back(ScrollItem{cache.element, false});

        const bool collapsible
            = (msg.role == TUIMessage::Role::Thinking || msg.role == TUIMessage::Role::Tool);
        itemMeta_.push_back(ItemMeta{msg.role, collapsible, static_cast<int>(i)});
    }

    streamingMdBuilders_.clear();
    if (hasStreamingToken) {
        const TUIMessage* currentMsg = nullptr;
        for (size_t i = st.messages.size(); i > 0; --i) {
            if (st.messages[i - 1]->role == st.currentTokenRole) {
                currentMsg = st.messages[i - 1].get();
                break;
            }
        }

        Element block;
        if (st.currentTokenRole == TUIMessage::Role::Thinking) {
            Elements lines;
            Elements header;
            header.push_back(text("- [Thinking] ") | color(theme.thinkingColor));
            if (currentMsg && currentMsg->durationMs > 0) {
                header.push_back(
                    text(fmt::format("{} ", formatDurationMilliseconds(currentMsg->durationMs)))
                    | color(theme.thinkingColor)
                );
            }
            lines.push_back(hbox(std::move(header)));
            if (false == TUISettings::instance().isAnimationEnabled(AnimationLevel::Ultra)) {
                // 超长流式文本: 纯文本渲染, 避免每 token 全量 cmark 解析
                lines.push_back(paragraph(*st.currentToken) | color(theme.thinkingColor));
            } else {
                auto [el, builder] = renderMarkdown(
                    *st.currentToken,
                    theme.thinkingColor,
                    theme.markdownTheme,
                    msgListWidth
                );
                if (builder) {
                    streamingMdBuilders_.push_back(std::move(builder));
                }
                lines.push_back(std::move(el));
            }
            block = vbox(std::move(lines));
        } else {
            if (false == TUISettings::instance().isAnimationEnabled(AnimationLevel::Low)) {
                block = paragraph(*st.currentToken) | color(theme.normalColor);
            } else {
                auto [el, builder] = renderMarkdown(
                    *st.currentToken,
                    theme.normalColor,
                    theme.markdownTheme,
                    msgListWidth
                );
                if (builder) {
                    streamingMdBuilders_.push_back(std::move(builder));
                }
                block = std::move(el);
            }
        }
        items.push_back(ScrollItem{std::move(block), false});
        itemMeta_.push_back(ItemMeta{st.currentTokenRole, false, -1});
    }

    return items;
}

Element MessageListComponent::buildMessageBlock(
    const TUIMessage&                                   msg,
    int                                                 maxWidth,
    std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
) {
    const auto& theme = *ctx_.theme;

    switch (msg.role) {
        case TUIMessage::Role::User:
            return hbox({
                text("> ") | color(theme.userColor),
                paragraph(msg.text) | color(theme.userColor),
            });
        case TUIMessage::Role::Assistant: {
            auto [el, builder]
                = renderMarkdown(msg.text, theme.assistantColor, theme.markdownTheme, maxWidth);
            if (builder) {
                mdBuilders.push_back(std::move(builder));
            }
            return el;
        }
        case TUIMessage::Role::System:
            return hbox({
                text("# ") | color(theme.systemColor),
                paragraph(msg.text) | color(theme.systemColor),
            });
        case TUIMessage::Role::Thinking: {
            const bool expanded = !msg.collapsed;
            Elements   lines;
            Elements   header;
            header.push_back(text(expanded ? "- " : "+ ") | color(theme.thinkingColor));
            header.push_back(text("[Thinking] ") | color(theme.thinkingColor));
            if (msg.durationMs > 0) {
                header.push_back(
                    text(formatDurationMilliseconds(msg.durationMs)) | color(theme.thinkingColor)
                );
                header.push_back(text(" "));
            }
            if (!expanded) {
                header.push_back(text(oneLinePreview(msg.text)) | color(theme.thinkingColor) | dim);
            }
            lines.push_back(hbox(std::move(header)));
            if (expanded) {
                auto [el, builder]
                    = renderMarkdown(msg.text, theme.thinkingColor, theme.markdownTheme, maxWidth);
                if (builder) {
                    mdBuilders.push_back(std::move(builder));
                }
                lines.push_back(std::move(el));
            }
            return vbox(std::move(lines));
        }
        case TUIMessage::Role::Tool: {
            const bool expanded   = !msg.collapsed;
            const bool isEditTool = (msg.toolName == "filesystem_edit_text_file");
            Elements   lines;
            Elements   header;
            header.push_back(
                text(fmt::format("{} [Tool] {} ", expanded ? "-" : "+", msg.toolName))
                | color(theme.toolColor)
            );
            if (!expanded) {
                if (!msg.toolFinished) {
                    header.push_back(text("running...") | color(theme.toolColor));
                } else if (isEditTool) {
                    appendEditToolHeader(msg, header);
                } else {
                    header.push_back(
                        text(oneLinePreview(msg.toolResult)) | color(theme.toolColor) | dim
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
                            text("  args: ") | color(theme.toolColor),
                            paragraph(msg.text) | color(theme.toolColor),
                        }));
                    }
                    if (msg.toolFinished) {
                        lines.push_back(hbox({
                            text("  result: ") | color(theme.toolColor),
                            paragraph(msg.toolResult) | color(theme.toolColor),
                        }));
                    } else {
                        lines.push_back(text("  running...") | color(theme.toolColor));
                    }
                }
            }
            return vbox(std::move(lines));
        }
    }
    return text("");
}

// ---------------------------------------------------------------------------
// filesystem_edit_text_file 特化渲染
// ---------------------------------------------------------------------------

void MessageListComponent::appendEditToolHeader(const TUIMessage& msg, Elements& header) {
    const auto& theme = *ctx_.theme;
    std::string path;
    try {
        path = neograph::json::parse(msg.text).value("path", std::string{});
    } catch (...) {
    }
    if (!path.empty()) {
        header.push_back(text(path) | color(theme.toolColor) | dim);
    }
}

void MessageListComponent::appendEditToolBody(const TUIMessage& msg, Elements& lines) {
    const auto& theme = *ctx_.theme;
    std::string path, oldStr, newStr;
    try {
        auto args = neograph::json::parse(msg.text);
        path      = args.value("path", std::string{});
        oldStr    = args.value("old_str", std::string{});
        newStr    = args.value("new_str", std::string{});
    } catch (...) {
    }
    if (!path.empty()) {
        lines.push_back(hbox({
            text("  file: ") | color(theme.hintColor),
            text(path) | color(theme.toolColor),
        }));
    }
    lines.push_back(renderEditToolDiff(oldStr, newStr));
}

Element MessageListComponent::renderEditToolDiff(std::string_view oldStr, std::string_view newStr) {
    using agentxx::util::DiffLineType;
    const auto& theme = *ctx_.theme;
    auto        diff  = agentxx::util::computeLineDiff(oldStr, newStr);
    if (diff.empty()) {
        return text("  (no changes)") | color(theme.hintColor);
    }

    const int  screenW    = Terminal::Size().dimx;
    const bool sideBySide = screenW >= 100;

    auto trunc = [](std::string_view s, size_t maxChars) -> std::string {
        const auto idx = agentxx::util::findIndexByUtf8Length(s, maxChars);
        if (idx > 0 && idx < s.size()) {
            return fmt::format("{}...", s.substr(0, idx));
        }
        return std::string{s};
    };

    if (!sideBySide) {
        const size_t maxChars = static_cast<size_t>(std::max(20, screenW - 6));
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
        return vbox(std::move(lines));
    }

    const int colW  = std::max(20, (screenW - 3) / 2);
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
            text(trunc(txt, static_cast<size_t>(textW))) | color(c),
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

    return hbox({
        vbox(std::move(leftLines)) | flex,
        separator(),
        vbox(std::move(rightLines)) | flex,
    });
}
