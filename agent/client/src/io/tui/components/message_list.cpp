#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/agent_tui.h" // formatDurationMilliseconds / oneLinePreview
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/mermaid_state.h"
#include "agentxx/util/diff_util.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/string_util.h"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include <markdown/dom_builder.hpp>
#include <markdown/parser.hpp>

using namespace ftxui;

namespace tui_mermaid = agentxx::client::tui;

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

/// markdown 文本片段: 普通文本或 mermaid 代码块
struct MarkdownSegment {
    std::string text;
    bool        isMermaid = false;
};

/// 将 markdown 按 ```mermaid 代码块切分 (非 mermaid 代码块整体保留给 markdown 渲染)
std::vector<MarkdownSegment> splitMermaidBlocks(std::string_view content) {
    std::vector<MarkdownSegment> segs;
    size_t                       pos = 0; // 当前待刷出普通段起点
    while (true) {
        auto open = content.find("```", pos);
        if (open == std::string_view::npos) {
            segs.push_back(MarkdownSegment{std::string(content.substr(pos)), false});
            break;
        }
        auto eol = content.find('\n', open);
        // 围栏语言名
        std::string_view lang = (eol == std::string_view::npos)
                                    ? content.substr(open + 3)
                                    : content.substr(open + 3, eol - open - 3);
        while (!lang.empty() && (lang.front() == ' ' || lang.front() == '\t')) {
            lang.remove_prefix(1);
        }
        while (!lang.empty() && (lang.back() == ' ' || lang.back() == '\t' || lang.back() == '\r')
        ) {
            lang.remove_suffix(1);
        }
        if (lang != "mermaid") {
            // 普通代码块: 整个块仍属当前普通段, 跳过到闭合围栏
            auto close = content.find("```", open + 3);
            if (close == std::string_view::npos) {
                segs.push_back(MarkdownSegment{std::string(content.substr(pos)), false});
                break;
            }
            pos = close + 3;
            continue;
        }
        // mermaid 块
        if (pos < open) {
            segs.push_back(MarkdownSegment{std::string(content.substr(pos, open - pos)), false});
        }
        if (eol == std::string_view::npos) {
            // 围栏在最后一行且无换行: 空内容
            segs.push_back(MarkdownSegment{"", true});
            break;
        }
        auto close = content.find("```", eol + 1);
        if (close == std::string_view::npos) {
            // 未闭合: 剩余整段作 mermaid
            segs.push_back(MarkdownSegment{std::string(content.substr(eol + 1)), true});
            break;
        }
        segs.push_back(MarkdownSegment{
            std::string(content.substr(eol + 1, close - eol - 1)),
            true,
        });
        pos = close + 3;
    }
    return segs;
}

/// 渲染 markdown; 其中的 ```mermaid 代码块渲染为状态图
/// mermaid 片段不着色 (继承外层消息颜色装饰)
std::pair<Element, std::vector<std::unique_ptr<markdown::DomBuilder>>> renderMarkdownWithMermaid(
    std::string_view       content,
    Color                  color,
    markdown::Theme const& mdTheme,
    int                    maxWidth = 0
) {
    auto segs = splitMermaidBlocks(content);
    if (segs.size() == 1 && !segs[0].isMermaid) {
        // 快速路径: 无 mermaid 块, 保持原有渲染
        auto [el, builder] = renderMarkdown(content, color, mdTheme, maxWidth);
        std::vector<std::unique_ptr<markdown::DomBuilder>> builders;
        if (builder) {
            builders.push_back(std::move(builder));
        }
        return {std::move(el), std::move(builders)};
    }
    std::vector<std::unique_ptr<markdown::DomBuilder>> builders;
    Elements                                           parts;
    for (const auto& seg : segs) {
        if (seg.isMermaid) {
            if (seg.text.empty()) {
                continue;
            }
            auto dg = tui_mermaid::parseMermaidStateDiagram(seg.text);
            parts.push_back(tui_mermaid::renderMermaidStateDiagram(dg, maxWidth));
        } else if (!seg.text.empty()) {
            auto [el, builder] = renderMarkdown(seg.text, color, mdTheme, maxWidth);
            if (builder) {
                builders.push_back(std::move(builder));
            }
            parts.push_back(std::move(el));
        }
    }
    if (parts.empty()) {
        return {ftxui::text(""), std::move(builders)};
    }
    return {vbox(std::move(parts)) | ftxui::color(color), std::move(builders)};
}

/// 估算文本显示行数 (换行符计数 + 按宽度折行估算)。
/// 仅用于不可见子项的高度估算 (影响滚动条/滚动定位), 子项进入视口后实测修正。
int estimateLines(std::string_view s, int width) {
    if (width <= 0) {
        width = 80;
    }
    if (s.empty()) {
        return 1;
    }
    int    lines    = 1;
    size_t segChars = 0; // 当前行内已计字符数 (按 UTF-8 码点)
    for (unsigned char c : s) {
        if ((c & 0xC0) == 0x80) {
            continue; // UTF-8 续字节, 不独立计数
        }
        if (c == '\n') {
            ++lines;
            segChars = 0;
            continue;
        }
        if (++segChars >= static_cast<size_t>(width)) {
            ++lines;
            segChars = 0;
        }
    }
    return lines;
}

} // namespace

MessageListComponent::MessageListComponent(TUICtx& ctx) :
    ctx_(ctx) {
    LazyScrollable::CacheBudget budget;
    budget.maxItems            = 256; // 条数预算: 最近 ~256 条消息的渲染缓存
    budget.maxBytes            = 16 * 1024 * 1024; // 字节预算: 缓存源文本累计 16MiB
    budget.byteExemptThreshold = 1024;             // 短消息不计入字节预算
    scrollable_                = std::make_shared<LazyScrollable>(
        [this] {
            return itemCount();
        },
        [this](size_t index) {
            return itemKey(index);
        },
        [this](size_t index, int width) {
            return estimateHeight(index, width);
        },
        [this](size_t index) {
            return buildItem(index);
        },
        budget,
        [this](size_t index) {
            return fillViewport(index);
        }
    );
    Add(scrollable_);
}

void MessageListComponent::invalidateCache() {
    scrollable_->clearCache();
}

Element MessageListComponent::OnRender() {
    // 由上一帧 viewport 可见区域反推可折叠消息的鼠标命中区域。
    // 必须在 scrollable_->Render() 之前计算: 此时 visibleBoxes() 为上一帧数据,
    // 与用户当前看到的屏幕内容一致。
    // (注意: 本组件采用懒构建, OnRender 阶段不构建任何子项, 仅产出布局节点)
    collapsibleBoxes_.clear();
    collapsibleIndices_.clear();
    if (ctx_.frameState) {
        const auto& vboxes = scrollable_->visibleBoxes();
        const auto& msgs   = ctx_.frameState->messages;
        for (size_t i = 0; i < vboxes.size() && i < msgs.size(); ++i) {
            const auto& msg = *msgs[i];
            const bool  collapsible
                = (msg.role == TUIMessage::Role::Thinking || msg.role == TUIMessage::Role::Tool);
            if (!collapsible || vboxes[i].IsEmpty()) {
                continue;
            }
            collapsibleBoxes_.push_back(vboxes[i]);
            collapsibleIndices_.push_back(i);
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

// ---------------------------------------------------------------------------
// LazyScrollable 回调
// ---------------------------------------------------------------------------

bool MessageListComponent::hasStreamingToken(const TUIRenderState& st) const {
    return st.isStreaming && st.currentToken && !st.currentToken->empty();
}

size_t MessageListComponent::itemCount() {
    const auto& st = *ctx_.frameState;
    if (st.messages.empty() && !hasStreamingToken(st)) {
        return 1; // 空状态 banner (fillViewport)
    }
    return st.messages.size() + (hasStreamingToken(st) ? 1 : 0);
}

uint64_t MessageListComponent::itemKey(size_t index) {
    const auto& st      = *ctx_.frameState;
    auto        combine = [](uint64_t seed, uint64_t v) -> uint64_t {
        return seed ^ (v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    };
    if (st.messages.empty() && !hasStreamingToken(st)) {
        return 1; // banner
    }
    if (index < st.messages.size()) {
        // 消息内容变化必然伴随消息指针变化 (见 TUISharedState::mutableMessage /
        // onSync 整体替换), 故以指针为主 key; 附加廉价 O(1) 特征 (长度/标志)
        // 防止地址重用 (ABA) —— 内容变则指针变, 长度几乎必然同步变化
        uint64_t    h = reinterpret_cast<uint64_t>(st.messages[index].get());
        const auto& m = *st.messages[index];
        h             = combine(h, static_cast<uint64_t>(m.role));
        h             = combine(h, m.collapsed);
        h             = combine(h, m.toolFinished);
        h             = combine(h, static_cast<uint64_t>(m.durationMs));
        h             = combine(h, static_cast<uint64_t>(m.startTimeMs));
        h             = combine(h, m.text.size());
        h             = combine(h, m.toolResult.size());
        h             = combine(h, m.toolName.size());
        return h;
    }
    // 流式增量项: token 累积中, 以 (指针, 长度, role) 作为 key 触发高度重估
    uint64_t h = reinterpret_cast<uint64_t>(st.currentToken.get());
    h          = combine(h, st.currentToken ? st.currentToken->size() : 0);
    h          = combine(h, static_cast<uint64_t>(st.currentTokenRole));
    return h;
}

int MessageListComponent::estimateHeight(size_t index, int width) {
    const auto& st = *ctx_.frameState;
    if (st.messages.empty() && !hasStreamingToken(st)) {
        return 1; // banner 为 fillViewport, 高度由 LazyScrollable 置为视口高度
    }
    if (index < st.messages.size()) {
        const auto& msg = *st.messages[index];
        switch (msg.role) {
            case TUIMessage::Role::User:
                return estimateLines(msg.text, width);
            case TUIMessage::Role::Assistant:
                return estimateLines(msg.text, width);
            case TUIMessage::Role::System:
                return estimateLines(msg.text, width);
            case TUIMessage::Role::Thinking:
                return msg.collapsed ? 1 : 1 + estimateLines(msg.text, width);
            case TUIMessage::Role::Tool: {
                if (msg.collapsed) {
                    return 1;
                }
                int lines = 1; // header
                if (!msg.text.empty()) {
                    lines += estimateLines(msg.text, width);
                }
                lines += msg.toolFinished ? estimateLines(msg.toolResult, width) : 1;
                return lines;
            }
        }
        return 1;
    }
    // 流式增量项
    return 1 + estimateLines(*st.currentToken, width);
}

bool MessageListComponent::fillViewport(size_t index) {
    const auto& st = *ctx_.frameState;
    return index == 0 && st.messages.empty() && !hasStreamingToken(st);
}

LazyBuiltItem MessageListComponent::buildItem(size_t index) {
    const auto& st = *ctx_.frameState;
    if (st.messages.empty() && !hasStreamingToken(st)) {
        LazyBuiltItem out;
        out.element   = buildBanner();
        out.cacheable = true;
        return out;
    }
    if (index < st.messages.size()) {
        return buildMessageItem(*st.messages[index]);
    }
    return buildStreamingItem(st);
}

Element MessageListComponent::buildBanner() {
    const auto& theme = *ctx_.theme;
    return vbox({
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
}

LazyBuiltItem MessageListComponent::buildMessageItem(const TUIMessage& msg) {
    const int maxWidth = std::max(1, scrollable_->contentWidth());

    std::vector<std::unique_ptr<markdown::DomBuilder>> builders;
    auto block = buildMessageBlock(msg, maxWidth, builders);

    LazyBuiltItem out;
    out.element     = vbox({std::move(block), text("")});
    out.sourceBytes = msg.text.size() + msg.toolResult.size() + msg.toolName.size();
    out.cacheable   = true;
    // markdown DomBuilder 生命周期与 Element 绑定
    // (Element 内 reflect 的链接 Box 指向 builder 内部容器)
    for (auto& b : builders) {
        out.attachments.push_back(std::move(b));
    }
    return out;
}

LazyBuiltItem MessageListComponent::buildStreamingItem(const TUIRenderState& st) {
    const auto& theme    = *ctx_.theme;
    const int   maxWidth = std::max(1, scrollable_->contentWidth());

    LazyBuiltItem out;
    out.cacheable   = false; // 流式增量每帧都变, 不缓存 (每帧重建后即释放)
    out.sourceBytes = st.currentToken ? st.currentToken->size() : 0;

    // 与流式角色相同的最近一条消息 (thinking 头部显示其时长)
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
                maxWidth
            );
            if (builder) {
                out.attachments.push_back(std::move(builder));
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
                maxWidth
            );
            if (builder) {
                out.attachments.push_back(std::move(builder));
            }
            block = std::move(el);
        }
    }
    out.element = std::move(block);
    return out;
}

// ---------------------------------------------------------------------------
// 消息块构建 (与旧实现一致的视觉呈现)
// ---------------------------------------------------------------------------

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
            auto [el, builders] = renderMarkdownWithMermaid(
                msg.text,
                theme.assistantColor,
                theme.markdownTheme,
                maxWidth
            );
            for (auto& b : builders) {
                mdBuilders.push_back(std::move(b));
            }
            return el;
        }
        case TUIMessage::Role::System: {
            // 按提示级别区分前缀与颜色 (Info/Warning/Error)
            std::string  prefix   = "# ";
            ftxui::Color tipColor = theme.systemColor;
            switch (msg.tipLevel) {
                case TUIMessage::TipLevel::Warning:
                    prefix   = "[Warn] ";
                    tipColor = theme.thinkingColor;
                    break;
                case TUIMessage::TipLevel::Error:
                    prefix   = "[Error] ";
                    tipColor = theme.errorColor;
                    break;
                case TUIMessage::TipLevel::Info:
                    break;
            }
            return hbox({
                text(prefix) | color(tipColor),
                paragraph(msg.text) | color(tipColor),
            });
        }
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
                auto [el, builders] = renderMarkdownWithMermaid(
                    msg.text,
                    theme.thinkingColor,
                    theme.markdownTheme,
                    maxWidth
                );
                for (auto& b : builders) {
                    mdBuilders.push_back(std::move(b));
                }
                lines.push_back(std::move(el));
            }
            return vbox(std::move(lines));
        }
        case TUIMessage::Role::Tool: {
            const bool expanded   = !msg.collapsed;
            const bool isEditTool = (msg.toolName == "agentxx_filesystem_edit_text_file");
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
// agentxx_filesystem_edit_text_file 特化渲染
// ---------------------------------------------------------------------------

void MessageListComponent::appendEditToolHeader(const TUIMessage& msg, Elements& header) {
    const auto& theme = *ctx_.theme;
    std::string path  = agentxx::util::catchError<std::string>(
        [&msg]() -> std::string {
            return neograph::json::parse(msg.text).value("path", std::string{});
        },
        [](std::string) -> std::string {
            return {};
        }
    );
    if (!path.empty()) {
        header.push_back(text(path) | color(theme.toolColor) | dim);
    }
}

void MessageListComponent::appendEditToolBody(const TUIMessage& msg, Elements& lines) {
    const auto& theme = *ctx_.theme;
    std::string path, oldStr, newStr;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto args = neograph::json::parse(msg.text);
            path      = args.value("path", std::string{});
            oldStr    = args.value("old_str", std::string{});
            newStr    = args.value("new_str", std::string{});
            return true;
        },
        [](std::string) -> bool {
            return false;
        }
    );
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
