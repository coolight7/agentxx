#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/util/string_util.h"

#include <markdown/dom_builder.hpp>
#include <markdown/parser.hpp>

using namespace ftxui;

namespace {

/// 渲染 markdown 并返回元素 + DomBuilder (builder 内部 Box 被 reflect() 引用,
/// 必须与返回的 element 同生命周期, 否则 SetBox 写入悬空引用 → UAF)
std::pair<ftxui::Element, std::unique_ptr<markdown::DomBuilder>>
    renderMarkdown(std::string_view text, ftxui::Color color, markdown::Theme const& mdTheme) {
    if (text.empty()) {
        return {ftxui::text(""), nullptr};
    }
    auto parser  = markdown::make_cmark_parser();
    auto ast     = parser->parse(text);
    auto builder = std::make_unique<markdown::DomBuilder>();
    auto el      = builder->build(ast, -1, mdTheme);
    return {el | ftxui::color(color), std::move(builder)};
}

} // namespace

// 计算消息内容签名 (64 位哈希): 组合各影响渲染的字段 (标量 + 文本哈希)。
// 相比字符串拼接, 整数哈希比较开销更低。签名不变则复用缓存的 Element。
int64_t AgentTUI::messageSignature(const Message& msg) {
    // boost 风格 hash_combine (64 位黄金比例常数)
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

// 构建单条消息的渲染块 (不含末尾空行)。
// 由 buildMessageItems 在消息签名变化时调用并缓存。
ftxui::Element AgentTUI::buildMessageBlock(
    const Message&                                      msg,
    std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
) {
    switch (msg.role) {
        case Message::Role::User:
            return hbox({
                text("> ") | color(theme_.userColor),
                paragraph(msg.text) | color(theme_.userColor),
            });
        case Message::Role::Assistant: {
            auto [el, builder]
                = renderMarkdown(msg.text, theme_.assistantColor, theme_.markdownTheme);
            if (builder) {
                mdBuilders.push_back(std::move(builder));
            }
            return el;
        }
        case Message::Role::System:
            return hbox({
                text("# ") | color(theme_.systemColor),
                paragraph(msg.text) | color(theme_.systemColor),
            });
        case Message::Role::Thinking: {
            // - 如果这个 Thinking
            // 消息正在输出，默认为自动展开状态并跟随滚动，直到输出该消息完成时自动折叠
            const bool expanded = !msg.collapsed;
            Elements   lines;
            Elements   header;
            header.push_back(text(expanded ? "- " : "+ ") | color(theme_.thinkingColor));
            header.push_back(text("[Thinking] ") | color(theme_.thinkingColor));

            // 如果设置了用时，显示格式为 [Thinking] {用时} {内容/缩略内容}
            std::string durationStr;
            if (msg.durationMs > 0) {
                durationStr = formatDurationMilliseconds(msg.durationMs);
                // 在 header 后追加 duration
                header.push_back(text(durationStr) | color(theme_.thinkingColor));
                header.push_back(text(" "));
            }

            if (!expanded) {
                header.push_back(
                    text(oneLinePreview(msg.text)) | color(theme_.thinkingColor) | dim
                );
            }
            lines.push_back(hbox(std::move(header)));
            if (expanded) {
                auto [el, builder]
                    = renderMarkdown(msg.text, theme_.thinkingColor, theme_.markdownTheme);
                if (builder) {
                    mdBuilders.push_back(std::move(builder));
                }
                lines.push_back(std::move(el));
            }
            return vbox(std::move(lines));
        }
        case Message::Role::Tool: {
            const bool expanded   = !msg.collapsed;
            const bool isEditTool = (msg.toolName == "filesystem_edit_text_file");
            Elements   lines;
            Elements   header;
            header.push_back(
                text(fmt::format("{} [Tool] {} ", expanded ? "-" : "+", msg.toolName))
                | color(theme_.toolColor)
            );
            if (!expanded) {
                if (!msg.toolFinished) {
                    header.push_back(text("running...") | color(theme_.toolColor));
                } else if (isEditTool) {
                    appendEditToolHeader(msg, header);
                } else {
                    header.push_back(
                        text(oneLinePreview(msg.toolResult)) | color(theme_.toolColor) | dim
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
                            text("  args: ") | color(theme_.toolColor),
                            paragraph(msg.text) | color(theme_.toolColor),
                        }));
                    }
                    if (msg.toolFinished) {
                        auto rc = theme_.toolColor;
                        lines.push_back(hbox({
                            text("  result: ") | color(rc),
                            paragraph(msg.toolResult) | color(rc),
                        }));
                    } else {
                        lines.push_back(text("  running...") | color(theme_.toolColor));
                    }
                }
            }
            return vbox(std::move(lines));
        }
    }
    return text("");
}

std::vector<ScrollItem> AgentTUI::buildMessageItems() {
    messageItemMeta_.clear();

    // 空状态: 单个占满视口的欢迎横幅 (fillViewport 居中)
    const bool hasStreamingToken = isStreaming_ && !currentToken_.empty();
    if (messages_.empty() && !hasStreamingToken) {
        auto banner = vbox({
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
        return {
            ScrollItem{std::move(banner), true}
        };
    }

    // 同步缓存大小: 收缩 (如 onSync 清空) 时整体重建; 增长时仅新增条目
    if (messageCache_.size() > messages_.size()) {
        messageCache_.clear();
    }
    while (messageCache_.size() < messages_.size()) {
        messageCache_.push_back(MessageCache{});
    }

    std::vector<ScrollItem> items;
    items.reserve(messages_.size() + 1);

    for (size_t i = 0; i < messages_.size(); ++i) {
        const auto& msg   = messages_[i];
        auto&       cache = messageCache_[i];
        int64_t     sig   = messageSignature(msg);
        if (cache.sig != sig || !cache.element) {
            // 内容/状态变化 -> 重建块元素 (markdown 仅在此解析)
            cache.mdBuilders.clear();
            cache.element = vbox({
                buildMessageBlock(msg, cache.mdBuilders),
                text(""),
            });
            cache.sig     = sig;
        }
        items.push_back(ScrollItem{cache.element, false});

        const bool collapsible
            = (msg.role == Message::Role::Thinking || msg.role == Message::Role::Tool);
        messageItemMeta_.push_back(MessageItemMeta{msg.role, collapsible, static_cast<int>(i)});
    }

    // 流式输出中的当前 token (每帧重建, 不缓存)
    streamingMdBuilders_.clear();
    if (hasStreamingToken) {
        // 查找最近的对应角色消息以获取时间信息
        const Message* currentMsg = nullptr;
        for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
            if (it->role == currentTokenRole_) {
                currentMsg = &(*it);
                break;
            }
        }

        Element block;
        if (currentTokenRole_ == Message::Role::Thinking) {
            // 流式输出中的 Thinking：自动展开并显示完整内容，跟随滚动
            Elements lines;
            Elements header;
            header.push_back(text("- [Thinking] ") | color(theme_.thinkingColor));
            if (currentMsg && currentMsg->durationMs > 0) {
                header.push_back(
                    text(fmt::format("({}) ", formatDurationMilliseconds(currentMsg->durationMs)))
                    | color(theme_.thinkingColor)
                );
            }
            lines.push_back(hbox(std::move(header)));
            auto [el, builder]
                = renderMarkdown(currentToken_, theme_.thinkingColor, theme_.markdownTheme);
            if (builder) {
                streamingMdBuilders_.push_back(std::move(builder));
            }
            lines.push_back(std::move(el));
            block = vbox(std::move(lines));
        } else {
            auto [el, builder]
                = renderMarkdown(currentToken_, theme_.assistantColor, theme_.markdownTheme);
            if (builder) {
                streamingMdBuilders_.push_back(std::move(builder));
            }
            block = std::move(el);
        }
        items.push_back(ScrollItem{std::move(block), false});
        messageItemMeta_.push_back(MessageItemMeta{currentTokenRole_, false, -1});
    }

    return items;
}
