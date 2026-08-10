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
#include <markdown/text_utils.hpp>

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

/// 估算文本显示行数 (换行符计数 + 按显示宽度折行估算)。
/// 仅用于不可见子项的高度估算 (影响滚动条/滚动定位), 子项进入视口后实测修正。
/// 宽字符 (CJK/emoji 等) 按 2 列计, 与 markdown::utf8_display_width 一致,
/// 修复旧实现按 UTF-8 码点计宽 (宽字符算 1 列) 导致 CJK 文本高度低估、
/// 滚动定位抖动的问题。线性扫描, 不整串调用 utf8_display_width (避免 O(n²))。
int estimateLines(std::string_view s, int width) {
    if (width <= 0) {
        width = 80;
    }
    if (s.empty()) {
        return 1;
    }
    int    lines = 1;
    int    col   = 0;
    size_t i     = 0;
    while (i < s.size()) {
        if (s[i] == '\n') {
            ++lines;
            col = 0;
            ++i;
            continue;
        }
        size_t len  = markdown::utf8_byte_length(s[i]);
        len         = std::min(len, s.size() - i);
        const int w = markdown::codepoint_width(markdown::utf8_codepoint(s.data() + i, len));
        if (w > 0) { // 组合字符/零宽字符不占列, 不触发折行
            col += w;
            if (col >= width) {
                ++lines;
                col = 0;
            }
        }
        i += len;
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

    // 中断控件命中区域: 清空后由本帧 scrollable_->Render() 中构建可见
    // Interrupt 消息时填充 (buildInterruptControl), 供下一帧点击命中检测
    interruptHits_.clear();

    return hbox({
               text("   "),
               scrollable_->Render() | bold | flex,
               text("   "),
           })
           | reflect(areaBox_);
}

bool MessageListComponent::OnEvent(Event event) {
    if (event.is_mouse()) {
        const auto& mouse = event.mouse();
        if (handleCollapsibleClick(mouse)) {
            return true;
        }
        if (handleInterruptClick(mouse)) {
            return true;
        }
        return scrollable_->OnEvent(event);
    }
    // 键盘: 优先作用于当前激活的中断消息 (输入框编辑/选中切换/确认/取消)
    if (activeInterruptMsg_ != static_cast<size_t>(-1)) {
        if (handleInterruptKey(event)) {
            return true;
        }
    }
    return false;
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
        h             = combine(h, m.tool ? m.tool->toolFinished : false);
        h             = combine(h, static_cast<uint64_t>(m.durationMs));
        h             = combine(h, static_cast<uint64_t>(m.startTimeMs));
        h             = combine(h, m.text.size());
        h             = combine(h, m.tool ? m.tool->toolResult.size() : size_t{0});
        h             = combine(h, m.tool ? m.tool->toolName.size() : size_t{0});
        // 中断消息: 状态变化经 mutableMessage 复制 (指针已变), 此处附加
        // 结构特征进一步防 ABA
        h = combine(
            h,
            static_cast<uint64_t>(
                m.interrupt ? m.interrupt->interruptStatus : TUIMessage::InterruptStatus::Waiting
            )
        );
        h = combine(h, m.interrupt ? static_cast<uint64_t>(m.interrupt->inputIndex) : uint64_t{0});
        h = combine(h, m.interrupt ? m.interrupt->interruptResult.size() : size_t{0});
        // 中断 UI 状态 (编辑文本/选中项/提示) 变化经 version 递增反映到 key,
        // 触发高度重估 (tip 增删影响估算行数)
        if (m.role == TUIMessage::Role::Interrupt) {
            InterruptKey key;
            if (interruptKeyOf(m, key)) {
                auto it = interruptUi_.find(key);
                h       = combine(h, it != interruptUi_.end() ? it->second.version : 0);
            }
        }
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
                const bool finished  = msg.tool && msg.tool->toolFinished;
                lines               += finished ? estimateLines(msg.tool->toolResult, width) : 1;
                return lines;
            }
            case TUIMessage::Role::Interrupt: {
                // 粗略估算 (进入视口后实测修正): 头行 + label + depict +
                // 控件区 + 提示/状态行 + 尾部空行
                // (编辑文本/选中项高度恒定, 不参与估算)
                const bool waiting
                    = msg.interrupt
                      && msg.interrupt->interruptStatus == TUIMessage::InterruptStatus::Waiting;
                if (waiting) {
                    size_t lines = 4;
                    auto   type  = msg.interrupt->inputType;
                    if (type == "enum") {
                        lines += std::min(msg.interrupt->inputEnums.size(), size_t{5});
                    }
                    InterruptKey key;
                    if (interruptKeyOf(msg, key)) {
                        auto it = interruptUi_.find(key);
                        if (it != interruptUi_.end() && !it->second.tip.empty()) {
                            ++lines;
                        }
                    }
                    return lines;
                }
                return 2;
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
        return buildMessageItem(*st.messages[index], index);
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

LazyBuiltItem MessageListComponent::buildMessageItem(const TUIMessage& msg, size_t index) {
    const int maxWidth = std::max(1, scrollable_->contentWidth());

    std::vector<std::unique_ptr<markdown::DomBuilder>> builders;
    auto block = buildMessageBlock(msg, index, maxWidth, builders);

    LazyBuiltItem out;
    out.element     = vbox({std::move(block), text("")});
    out.sourceBytes = msg.text.size() + (msg.tool ? msg.tool->toolResult.size() : 0)
                      + (msg.tool ? msg.tool->toolName.size() : 0);
    // 中断消息不缓存: 每帧重建以刷新控件 reflect 命中区域 (interruptHits_),
    // 否则缓存命中时控件 Box 丢失, 点击无法命中; 中断消息数量少, 成本可忽略
    out.cacheable = (msg.role != TUIMessage::Role::Interrupt);
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
    size_t                                              msgIndex,
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
            const auto   tipLevel = msg.system ? msg.system->tipLevel : TUIMessage::TipLevel::Info;
            switch (tipLevel) {
                case TUIMessage::TipLevel::Warning:
                    prefix   = "# [Warn] ";
                    tipColor = theme.thinkingColor;
                    break;
                case TUIMessage::TipLevel::Error:
                    prefix   = "# [Error] ";
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
            // 防御: 类型不变量下 tool 应非空 (fromJson/构造均保证), 缺失时跳过渲染
            if (!msg.tool) {
                return text("");
            }
            const bool expanded = !msg.collapsed;
            const bool isEditTool
                = msg.tool && msg.tool->toolName == "agentxx_filesystem_edit_text_file";
            const bool finished = msg.tool && msg.tool->toolFinished;
            Elements   lines;
            Elements   header;
            header.push_back(
                text(fmt::format("{} [Tool] {} ", expanded ? "-" : "+", msg.tool->toolName))
                | color(theme.toolColor)
            );
            if (!expanded) {
                if (!finished) {
                    header.push_back(text("running...") | color(theme.toolColor));
                } else if (isEditTool) {
                    appendEditToolHeader(msg, header);
                } else {
                    header.push_back(
                        text(oneLinePreview(msg.tool->toolResult)) | color(theme.toolColor) | dim
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
                    if (finished) {
                        lines.push_back(hbox({
                            text("  result: ") | color(theme.toolColor),
                            paragraph(msg.tool->toolResult) | color(theme.toolColor),
                        }));
                    } else {
                        lines.push_back(text("  running...") | color(theme.toolColor));
                    }
                }
            }
            return vbox(std::move(lines));
        }
        case TUIMessage::Role::Interrupt: {
            // 中断输入消息: 内嵌交互控件, 直接渲染在消息列表中
            Elements lines;

            const bool waiting
                = msg.interrupt
                  && msg.interrupt->interruptStatus == TUIMessage::InterruptStatus::Waiting;
            if (waiting) {
                // 头行: 类型 + 进度
                Elements header;
                header.push_back(
                    text(fmt::format(
                        "[Interrupt] Input {}/{}: ",
                        msg.interrupt->inputIndex,
                        msg.interrupt->inputTotal
                    ))
                    | color(theme.accentColor) | bold
                );
                header.push_back(text(msg.interrupt->inputLabel) | color(theme.accentColor));
                lines.push_back(hbox(std::move(header)));

                if (!msg.interrupt->inputDepict.empty()) {
                    lines.push_back(hbox({
                        text("  ") | color(theme.hintColor),
                        text(msg.interrupt->inputDepict) | color(theme.hintColor),
                    }));
                }

                lines.push_back(text(" "));
                lines.push_back(buildInterruptControl(msg, msgIndex));

                // 校验失败提示: 从 UI 状态表读取 (消息结构不承载 UI 状态)
                InterruptKey key;
                if (interruptKeyOf(msg, key)) {
                    auto it = interruptUi_.find(key);
                    if (it != interruptUi_.end() && !it->second.tip.empty()) {
                        lines.push_back(hbox({
                            text("  ") | color(theme.hintColor),
                            text(it->second.tip) | color(theme.errorColor),
                        }));
                    }
                }
            } else {
                lines.push_back(buildInterruptStatusLine(msg));
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

// ---------------------------------------------------------------------------
// 中断输入消息: 控件渲染 + 交互
// ---------------------------------------------------------------------------

namespace {

/// double 步进后的显示格式: 整数值按 "1.0" 风格, 非整数保留有效精度
std::string formatStepDouble(double v) {
    if (v == std::floor(v) && std::abs(v) < 1e15) {
        return fmt::format("{:.1f}", v);
    }
    return fmt::format("{:.10g}", v);
}

} // namespace

Element MessageListComponent::buildInterruptStatusLine(const TUIMessage& msg) {
    const auto& theme = *ctx_.theme;
    if (!msg.interrupt) {
        return text("");
    }
    const auto& it = *msg.interrupt;
    switch (it.interruptStatus) {
        case TUIMessage::InterruptStatus::Confirmed:
            return hbox({
                text(fmt::format("[Interrupt] Input {}/{}: ", it.inputIndex, it.inputTotal))
                    | color(theme.hintColor) | dim,
                text("✓ 已确认 ") | color(theme.accentColor) | bold,
                text(fmt::format("{}: {}", it.inputLabel, it.interruptResult))
                    | color(theme.accentColor),
            });
        case TUIMessage::InterruptStatus::Cancelled:
            return hbox({
                text(fmt::format("[Interrupt] Input {}/{}: ", it.inputIndex, it.inputTotal))
                    | color(theme.hintColor) | dim,
                text(fmt::format("✕ {}: 已取消", it.inputLabel)) | color(theme.hintColor) | dim,
            });
        case TUIMessage::InterruptStatus::Expired:
            return hbox({
                text(fmt::format("[Interrupt] Input {}/{}: ", it.inputIndex, it.inputTotal))
                    | color(theme.hintColor) | dim,
                text(fmt::format("⌛ {}: 已过期", it.inputLabel)) | color(theme.hintColor) | dim,
            });
        default:
            return text("");
    }
}

Element MessageListComponent::buildInterruptControl(const TUIMessage& msg, size_t msgIndex) {
    const auto& theme = *ctx_.theme;
    if (!msg.interrupt) {
        return text("");
    }
    // UI 状态 (编辑文本/选中项): 惰性创建并读取 (渲染是 UI 线程独占路径)
    const auto& ui = uiStateFor(msg);
    const auto& id = *msg.interrupt;

    // 记录命中区域: box 经 shared_ptr 持有, reflect 在布局 (SetBox) 时写回,
    // 点击检测读到的是最新布局位置 (构建阶段记录值会拿到空 Box)
    auto hit = [this, msgIndex](uint8_t kind, int sub, const std::shared_ptr<Box>& box) {
        InterruptHitBox hb;
        hb.msgIndex = msgIndex;
        hb.kind     = kind;
        hb.sub      = sub;
        hb.box      = box;
        interruptHits_.push_back(std::move(hb));
    };
    auto mkBox = []() {
        return std::make_shared<Box>();
    };

    // 按钮样式
    auto btn = [&theme](std::string label, bool active) {
        if (active) {
            return text(label) | bgcolor(theme.buttonActiveBgColor)
                   | color(theme.buttonActiveTextColor) | bold;
        }
        return text(label) | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
    };

    // 确认 / 取消按钮 (共用)
    auto confirmBox = mkBox();
    auto cancelBox  = mkBox();
    auto confirmBtn = btn(" [确认] ", false) | reflect(*confirmBox);
    auto cancelBtn  = text(" ✕ ") | color(theme.errorColor) | reflect(*cancelBox);
    hit(kHitConfirm, 0, confirmBox);
    hit(kHitCancel, 0, cancelBox);

    Element control;
    if (id.inputType == "bool") {
        auto yesBox = mkBox();
        auto noBox  = mkBox();
        auto yes    = btn(" 是 ", ui.selected == 0) | reflect(*yesBox);
        auto no     = btn(" 否 ", ui.selected == 1) | reflect(*noBox);
        hit(kHitBoolYes, 0, yesBox);
        hit(kHitBoolNo, 0, noBox);
        control = hbox({
            yes,
            text(" "),
            no,
            text("  "),
            confirmBtn,
            text("  "),
            cancelBtn,
        });
    } else if (id.inputType == "int" || id.inputType == "double") {
        auto minusBox = mkBox();
        auto plusBox  = mkBox();
        auto editBox  = mkBox();
        auto minus    = btn(" - ", false) | reflect(*minusBox);
        auto plus     = btn(" + ", false) | reflect(*plusBox);
        hit(kHitNumMinus, 0, minusBox);
        hit(kHitNumPlus, 0, plusBox);
        hit(kHitEdit, 0, editBox);
        control = hbox({
            minus,
            text(" "),
            text(" " + ui.editText + " ") | bgcolor(theme.inputBgColor)
                | color(theme.inputTextColor) | reflect(*editBox),
            text(" "),
            plus,
            text("  "),
            confirmBtn,
            text("  "),
            cancelBtn,
        });
    } else if (id.inputType == "enum") {
        // 枚举项竖直列表 (选中项高亮)
        enumBoxes_.resize(id.inputEnums.size());
        Elements items;
        for (size_t i = 0; i < id.inputEnums.size(); ++i) {
            if (!enumBoxes_[i]) {
                enumBoxes_[i] = mkBox();
            }
            auto entry = text(fmt::format(
                " {} {}",
                (static_cast<int>(i) == ui.selected) ? "▸" : " ",
                id.inputEnums[i]
            ));
            if (static_cast<int>(i) == ui.selected) {
                entry = entry | bgcolor(theme.buttonActiveBgColor)
                        | color(theme.buttonActiveTextColor) | bold;
            } else {
                entry = entry | color(theme.buttonTextColor);
            }
            entry = entry | reflect(*enumBoxes_[i]);
            hit(kHitEnumItem, static_cast<int>(i), enumBoxes_[i]);
            items.push_back(entry);
        }
        // 底部操作行: 确认 + 取消
        control = vbox({
            vbox(std::move(items)),
            text(" "),
            hbox({
                confirmBtn,
                text("  "),
                cancelBtn,
            }),
        });
    } else { // string
        auto editBox = mkBox();
        hit(kHitEdit, 0, editBox);
        control = hbox({
            text(" " + ui.editText + " ") | bgcolor(theme.inputBgColor)
                | color(theme.inputTextColor) | reflect(*editBox),
            text("  "),
            confirmBtn,
            text("  "),
            cancelBtn,
        });
    }
    return control;
}

// ---------------------------------------------------------------------------
// 中断 UI 状态表 (UI 线程独占; key = (interruptId, inputIndex))
// ---------------------------------------------------------------------------

void MessageListComponent::attachInterruptChannel(
    int64_t                                 wireId,
    std::shared_ptr<InterruptResultChannel> ch
) {
    interruptChannels_[wireId] = std::move(ch);
}

void MessageListComponent::releaseInterruptChannel(int64_t wireId) {
    interruptChannels_.erase(wireId);
    for (auto it = interruptUi_.begin(); it != interruptUi_.end();) {
        if (it->first.id == wireId) {
            it = interruptUi_.erase(it);
        } else {
            ++it;
        }
    }
}

void MessageListComponent::clearInterruptUiState() {
    interruptUi_.clear();
    interruptChannels_.clear();
}

bool MessageListComponent::interruptKeyOf(const TUIMessage& msg, InterruptKey& out) {
    if (msg.role != TUIMessage::Role::Interrupt || !msg.interrupt) {
        return false;
    }
    out.id    = msg.interrupt->interruptId;
    out.index = msg.interrupt->inputIndex;
    return true;
}

MessageListComponent::InterruptUIState& MessageListComponent::uiStateFor(const TUIMessage& msg) {
    InterruptKey key;
    if (!interruptKeyOf(msg, key)) {
        // 非中断消息不应请求 UI 状态; 返回静态兜底条目 (调用方按角色分支, 不会走到)
        static InterruptUIState fallback;
        return fallback;
    }
    auto [it, inserted] = interruptUi_.try_emplace(key);
    if (inserted) {
        // 惰性初始化: 编辑文本 = 默认值 (数值无默认时 "0"/"0.0"), string 无默认为空;
        // 选中项 = bool 按默认值 (false/no/n → "否"), enum 按默认值匹配 (无匹配首项)
        const auto& id = *msg.interrupt;
        auto&       ui = it->second;
        ui.editText    = id.inputDefault;
        if ((id.inputType == "int" || id.inputType == "double") && id.inputDefault.empty()) {
            ui.editText = (id.inputType == "double") ? "0.0" : "0";
        }
        if (id.inputType == "bool") {
            std::string def = id.inputDefault;
            agentxx::util::toLowerSelf(def);
            ui.selected = (def == "false" || def == "no" || def == "n") ? 1 : 0;
        } else if (id.inputType == "enum") {
            for (size_t i = 0; i < id.inputEnums.size(); ++i) {
                if (id.inputEnums[i] == id.inputDefault) {
                    ui.selected = static_cast<int>(i);
                    break;
                }
            }
        }
    }
    return it->second;
}

MessageListComponent::InterruptUIState&
    MessageListComponent::mutateInterruptUiState(const TUIMessage& msg) {
    auto& ui = uiStateFor(msg);
    ++ui.version; // 驱动 itemKey 变化 → 懒列表缓存失效 (高度/滚动重估)
    return ui;
}

MessageListComponent::InterruptUIState MessageListComponent::interruptUiState(size_t msgIndex
) const {
    const auto& st = *ctx_.frameState;
    if (msgIndex >= st.messages.size()) {
        return {};
    }
    const auto&  msg = *st.messages[msgIndex];
    InterruptKey key;
    if (!interruptKeyOf(msg, key)) {
        return {};
    }
    auto it = interruptUi_.find(key);
    return it == interruptUi_.end() ? InterruptUIState{} : it->second;
}

// ---------------------------------------------------------------------------
// 中断输入消息交互
// ---------------------------------------------------------------------------

bool MessageListComponent::handleInterruptClick(const Mouse& mouse) {
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    if (areaBox_.x_max < areaBox_.x_min) {
        return false; // 尚未布局
    }
    if (mouse.x < areaBox_.x_min || mouse.x > areaBox_.x_max) {
        return false;
    }
    // 命中检测: 后渲染的控件优先 (enum 项与底部按钮同帧注册, 无重叠; 直接顺序查找)
    for (const auto& h : interruptHits_) {
        if (!h.box) {
            continue;
        }
        const auto& box = *h.box;
        if (mouse.y < box.y_min || mouse.y > box.y_max || mouse.x < box.x_min
            || mouse.x > box.x_max) {
            continue;
        }
        switch (h.kind) {
            case kHitBoolYes:
                // 点击是/否直接确认 (先设置选中, 再确认)
                ctx_.state->mutate([&](TUIRenderState& st) {
                    if (h.msgIndex < st.messages.size()) {
                        auto& ui    = mutateInterruptUiState(*st.messages[h.msgIndex]);
                        ui.selected = 0;
                        ui.tip.clear();
                    }
                });
                setInterruptActive(h.msgIndex);
                confirmInterrupt(h.msgIndex);
                return true;
            case kHitBoolNo:
                ctx_.state->mutate([&](TUIRenderState& st) {
                    if (h.msgIndex < st.messages.size()) {
                        auto& ui    = mutateInterruptUiState(*st.messages[h.msgIndex]);
                        ui.selected = 1;
                        ui.tip.clear();
                    }
                });
                setInterruptActive(h.msgIndex);
                confirmInterrupt(h.msgIndex);
                return true;
            case kHitNumMinus:
                setInterruptActive(h.msgIndex);
                stepInterrupt(h.msgIndex, -1.0);
                return true;
            case kHitNumPlus:
                setInterruptActive(h.msgIndex);
                stepInterrupt(h.msgIndex, 1.0);
                return true;
            case kHitEnumItem: {
                // 选中枚举项
                ctx_.state->mutate([&](TUIRenderState& st) {
                    if (h.msgIndex >= st.messages.size()) {
                        return;
                    }
                    auto& ui    = mutateInterruptUiState(*st.messages[h.msgIndex]);
                    ui.selected = h.sub;
                    ui.tip.clear();
                });
                setInterruptActive(h.msgIndex);
                ctx_.postRedraw();
                return true;
            }
            case kHitEdit:
                setInterruptActive(h.msgIndex);
                return true;
            case kHitConfirm:
                setInterruptActive(h.msgIndex);
                confirmInterrupt(h.msgIndex);
                return true;
            case kHitCancel:
                cancelInterrupt(h.msgIndex);
                return true;
            default:
                return false;
        }
    }
    return false;
}

bool MessageListComponent::handleInterruptKey(Event event) {
    const size_t mi = activeInterruptMsg_;

    // 校验 active 消息仍存在且可交互
    std::string type;
    bool        valid = false;
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (mi < st.messages.size()) {
            const auto& m = *st.messages[mi];
            if (m.role == TUIMessage::Role::Interrupt && m.interrupt
                && m.interrupt->interruptStatus == TUIMessage::InterruptStatus::Waiting) {
                valid = true;
                type  = m.interrupt->inputType;
            }
        }
    });
    if (!valid) {
        activeInterruptMsg_ = static_cast<size_t>(-1);
        return false;
    }

    if (event == Event::Escape) {
        // 退出编辑态 (不取消中断)
        activeInterruptMsg_ = static_cast<size_t>(-1);
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::Return) {
        confirmInterrupt(mi);
        return true;
    }
    if (type == "bool") {
        if (event == Event::ArrowLeft || event == Event::ArrowRight) {
            ctx_.state->mutate([&](TUIRenderState& st) {
                if (mi < st.messages.size()) {
                    auto& ui    = mutateInterruptUiState(*st.messages[mi]);
                    ui.selected = 1 - ui.selected;
                }
            });
            ctx_.postRedraw();
            return true;
        }
        return false;
    }
    if (type == "enum") {
        int delta = 0;
        if (event == Event::ArrowUp) {
            delta = -1;
        } else if (event == Event::ArrowDown) {
            delta = 1;
        }
        if (delta != 0) {
            ctx_.state->mutate([&](TUIRenderState& st) {
                if (mi >= st.messages.size() || !st.messages[mi]->interrupt) {
                    return;
                }
                auto& ui    = mutateInterruptUiState(*st.messages[mi]);
                int   size  = static_cast<int>(st.messages[mi]->interrupt->inputEnums.size());
                ui.selected = std::clamp(ui.selected + delta, 0, size - 1);
            });
            ctx_.postRedraw();
            return true;
        }
        return false;
    }
    if (type == "int" || type == "double") {
        if (event == Event::ArrowUp) {
            stepInterrupt(mi, 1.0);
            return true;
        }
        if (event == Event::ArrowDown) {
            stepInterrupt(mi, -1.0);
            return true;
        }
    }
    // 数值/string: 文本编辑
    if (event.is_character() || event == Event::Backspace || event == Event::Delete
        || event == Event::ArrowLeft || event == Event::ArrowRight) {
        ctx_.state->mutate([&](TUIRenderState& st) {
            if (mi >= st.messages.size()) {
                return;
            }
            auto& ui = mutateInterruptUiState(*st.messages[mi]);
            if (event.is_character()) {
                if (!ui.edited) {
                    // 首次输入替换默认值 (与输入框激活时保留默认值的语义一致)
                    ui.editText.clear();
                    ui.edited = true;
                }
                ui.editText += event.character();
            } else if (event == Event::Backspace && !ui.editText.empty()) {
                ui.editText.pop_back();
                ui.edited = true;
            } else if (event == Event::Delete && !ui.editText.empty()) {
                // 简化: 与 Backspace 同义 (单行输入无光标定位)
                ui.editText.pop_back();
                ui.edited = true;
            }
            ui.tip.clear();
        });
        ctx_.postRedraw();
        return true;
    }
    return false;
}

void MessageListComponent::setInterruptActive(size_t mi) {
    activeInterruptMsg_ = mi;
    // 数值/string: 激活时确保 UI 状态已初始化 (编辑文本 = 默认值);
    // 首次编辑前 edited=false, 编辑后不再覆盖
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (mi >= st.messages.size()) {
            return;
        }
        uiStateFor(*st.messages[mi]); // 惰性创建 (初始化默认编辑文本/选中项)
    });
    ctx_.postRedraw();
}

void MessageListComponent::confirmInterrupt(size_t mi) {
    std::string  value;
    bool         confirmed = false;
    InterruptKey key; // 确认成功时记录 (mutate 内收集, 锁外发送结果)
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (mi >= st.messages.size() || !st.messages[mi]->interrupt) {
            return;
        }
        const auto& src = *st.messages[mi];
        if (src.role != TUIMessage::Role::Interrupt
            || src.interrupt->interruptStatus != TUIMessage::InterruptStatus::Waiting) {
            return;
        }
        const std::string& type = src.interrupt->inputType;
        auto&              ui   = uiStateFor(src);
        if (type == "bool") {
            value     = (ui.selected == 0) ? "true" : "false";
            confirmed = true;
        } else if (type == "enum") {
            if (ui.selected >= 0
                && ui.selected < static_cast<int>(src.interrupt->inputEnums.size())) {
                value     = src.interrupt->inputEnums[static_cast<size_t>(ui.selected)];
                confirmed = true;
            }
        } else if (type == "int" || type == "double") {
            std::string errTip;
            if (type == "int") {
                int64_t num = 0;
                auto    r   = agentxx::util::parseNumberFromString(ui.editText, num);
                if (r.ec != std::errc{}) {
                    errTip = "Invalid integer, please input again.";
                }
            } else {
                double num = 0.0;
                auto   r   = agentxx::util::parseNumberFromString(ui.editText, num);
                if (r.ec != std::errc{}) {
                    errTip = "Invalid number, please input again.";
                }
            }
            if (!errTip.empty()) {
                ui.tip = std::move(errTip);
                ctx_.postRedraw();
                return;
            }
            value     = ui.editText;
            confirmed = true;
        } else { // string
            value     = ui.editText;
            confirmed = true;
        }
        if (confirmed) {
            // 先取 key (src 引用随后被 mutableMessage 替换失效)
            interruptKeyOf(src, key);
            // 确认结果写入消息 (跨线程共享的展示状态); UI 状态表保留编辑残留
            auto& mm                      = ctx_.state->mutableMessage(st, mi);
            mm.interrupt->interruptStatus = TUIMessage::InterruptStatus::Confirmed;
            mm.interrupt->interruptResult = value;
        }
    });
    if (confirmed) {
        // 发送结果到 client 线程 (channel 线程安全):
        // 通道从 interruptChannels_ 取最新 (同请求共享, 经 attachInterruptChannel 注入)
        auto chIt = interruptChannels_.find(key.id);
        if (chIt != interruptChannels_.end() && chIt->second) {
            chIt->second->async_send(
                neograph_asio_error_code{},
                key.index,
                std::optional<std::string>(value),
                [](neograph_asio_error_code) {}
            );
        }
        ctx_.postRedraw();
    }
}

void MessageListComponent::cancelInterrupt(size_t mi) {
    int64_t      id = 0;
    InterruptKey key; // mutate 内收集, 锁外发送整体取消
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (mi >= st.messages.size() || !st.messages[mi]->interrupt) {
            return;
        }
        id = st.messages[mi]->interrupt->interruptId;
        interruptKeyOf(*st.messages[mi], key);
        // 标记同请求所有未操作消息为 Cancelled
        for (size_t i = 0; i < st.messages.size(); ++i) {
            const auto& m = *st.messages[i];
            if (m.role == TUIMessage::Role::Interrupt && m.interrupt
                && m.interrupt->interruptId == id
                && m.interrupt->interruptStatus == TUIMessage::InterruptStatus::Waiting) {
                auto& mm                      = ctx_.state->mutableMessage(st, i);
                mm.interrupt->interruptStatus = TUIMessage::InterruptStatus::Cancelled;
            }
        }
        // 清理同请求的 UI 状态 (消息已固定状态, 编辑残留不再需要)
        for (auto it = interruptUi_.begin(); it != interruptUi_.end();) {
            if (it->first.id == id) {
                it = interruptUi_.erase(it);
            } else {
                ++it;
            }
        }
    });
    // 整体取消: inputIndex = -1, value = nullopt (经同请求共享通道发送)
    auto chIt = interruptChannels_.find(id);
    if (chIt != interruptChannels_.end() && chIt->second) {
        chIt->second->async_send(
            neograph_asio_error_code{},
            -1,
            std::optional<std::string>(),
            [](neograph_asio_error_code) {}
        );
    }
    activeInterruptMsg_ = static_cast<size_t>(-1);
    ctx_.postRedraw();
}

void MessageListComponent::stepInterrupt(size_t mi, double delta) {
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (mi >= st.messages.size() || !st.messages[mi]->interrupt) {
            return;
        }
        const auto& src = *st.messages[mi];
        if (src.role != TUIMessage::Role::Interrupt
            || src.interrupt->interruptStatus != TUIMessage::InterruptStatus::Waiting) {
            return;
        }
        const std::string& type = src.interrupt->inputType;
        if (type != "int" && type != "double") {
            return;
        }
        auto&  ui  = uiStateFor(src);
        double val = 0.0;
        if (type == "int") {
            int64_t num = 0;
            auto    r   = agentxx::util::parseNumberFromString(ui.editText, num);
            if (r.ec != std::errc{}) {
                return; // 编辑值非法时步进无效
            }
            val = static_cast<double>(num);
        } else {
            double num = 0.0;
            auto   r   = agentxx::util::parseNumberFromString(ui.editText, num);
            if (r.ec != std::errc{}) {
                return;
            }
            val = num;
        }
        val += delta;
        // 注意: src 为快照引用, 修改 UI 状态表不改变消息, 引用保持有效
        if (type == "int") {
            ui.editText = fmt::format("{}", static_cast<int64_t>(val));
        } else {
            ui.editText = formatStepDouble(val);
        }
        ui.edited = true;
        ui.tip.clear();
    });
    ctx_.postRedraw();
}
