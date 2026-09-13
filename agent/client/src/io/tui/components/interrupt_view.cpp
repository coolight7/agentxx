#include "agentxx-client/io/tui/components/interrupt_view.h"

#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/plugin_ui_items.h"
#include "agentxx-client/io/tui/text_layout.h"
#include "agentxx/util/diff_util.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

using namespace ftxui;

namespace agentxx {
namespace client {

namespace {

/// 布尔默认值规范化 (bool 输入的默认值可写 true/false/yes/no/y/n/1/0)
/// - 与 stdio 端输入解析口径一致: 非否定值一律视为 true
std::string normalizeBoolDefault(std::string_view s) {
    if (s.empty()) {
        return "false";
    }
    std::string v{s};
    agentxx::util::toLowerSelf(v);
    if (v == "false" || v == "no" || v == "n" || v == "0") {
        return "false";
    }
    return "true";
}

/// 整数/浮点步进后的显示格式 (整数值按 "1.0" 风格, 非整数保留有效精度)
std::string formatStepDouble(double v) {
    if (v == std::floor(v) && std::abs(v) < 1e15) {
        return fmt::format("{:.1f}", v);
    }
    return fmt::format("{:.10g}", v);
}

} // namespace

InterruptView::InterruptView(TUICtx& ctx) :
    ctx_(ctx) {}

// ---------------------------------------------------------------------------
// 通道与 UI 状态管理
// ---------------------------------------------------------------------------

void InterruptView::beginFrame() {
    hits_.clear();
}

void InterruptView::attachChannel(int64_t wireId, std::shared_ptr<InterruptResultChannel> ch) {
    channels_[wireId] = std::move(ch);
}

void InterruptView::releaseChannel(int64_t wireId) {
    channels_.erase(wireId);
    for (auto it = states_.begin(); it != states_.end();) {
        if (it->first.id == wireId) {
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
}

void InterruptView::clear() {
    states_.clear();
    channels_.clear();
    hits_.clear();
    activeMsg_ = static_cast<size_t>(-1);
}

bool InterruptView::isWaiting(const TUIMessage& msg) {
    return msg.role == TUIMessage::Role::Interrupt && msg.interrupt
           && msg.interrupt->interruptStatus == TUIMessage::InterruptStatus::Waiting;
}

size_t InterruptView::activeMsg() const {
    return activeMsg_;
}

void InterruptView::setActiveMsg(size_t msgIndex) {
    activeMsg_ = msgIndex;
    // 激活时确保表单状态已按描述/消息初始化 (首次编辑前 edited=false)
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (msgIndex < st.messages.size() && isWaiting(*st.messages[msgIndex])) {
            uiStateFor(*st.messages[msgIndex]);
        }
    });
    ctx_.postRedraw();
}

void InterruptView::clearActive() {
    activeMsg_ = static_cast<size_t>(-1);
}

bool InterruptView::keyOf(const TUIMessage& msg, Key& out) {
    if (msg.role != TUIMessage::Role::Interrupt || !msg.interrupt) {
        return false;
    }
    out.id    = msg.interrupt->interruptId;
    out.index = msg.interrupt->inputIndex;
    return true;
}

InterruptView::FormState& InterruptView::uiStateFor(const TUIMessage& msg) {
    Key key;
    if (!keyOf(msg, key)) {
        // 非中断消息不应请求表单状态 (调用方按角色分支, 不会走到)
        static FormState fallback;
        return fallback;
    }
    auto [it, inserted] = states_.try_emplace(key);
    if (!inserted) {
        return it->second;
    }
    // 惰性初始化: 勾选项默认值 + 输入项初值 (文本=默认值, 数值无默认时 "0"/"0.0",
    // 按钮/枚举=与默认值匹配的项, 无匹配取首项)
    auto&       state = it->second;
    const auto  ui    = resolveUi(msg);
    for (const auto& item : ui.items) {
        if (item.kind == "toggle" && !item.id.empty()) {
            state.toggles[item.id] = item.defaultToggle;
        }
    }
    Resolved input;
    if (resolveInputItem(msg, input)) {
        if (input.view == "buttons") {
            const std::string def = normalizeBoolDefault(input.defaultValue);
            state.selected        = 0;
            for (size_t i = 0; i < input.buttons.size(); ++i) {
                if (input.buttons[i].value == def) {
                    state.selected = static_cast<int>(i);
                    break;
                }
            }
        } else if (input.view == "list") {
            state.selected = 0;
            for (size_t i = 0; i < input.enums.size(); ++i) {
                if (input.enums[i] == input.defaultValue) {
                    state.selected = static_cast<int>(i);
                    break;
                }
            }
        } else if (input.view == "number") {
            state.editText = input.defaultValue;
            if (state.editText.empty()) {
                state.editText = (input.inputType == "double") ? "0.0" : "0";
            }
        } else {
            state.editText = input.defaultValue;
        }
    }
    return state;
}

InterruptView::FormState& InterruptView::mutateUiState(const TUIMessage& msg) {
    auto& state = uiStateFor(msg);
    ++state.version; // 驱动消息列表缓存失效 (高度/滚动重估)
    return state;
}

bool InterruptView::toggleValue(const TUIMessage& msg, std::string_view id, bool defaultValue) const {
    Key key;
    if (id.empty() || !keyOf(msg, key)) {
        return defaultValue;
    }
    auto it = states_.find(key);
    if (it == states_.end()) {
        return defaultValue;
    }
    auto t = it->second.toggles.find(std::string{id});
    return t == it->second.toggles.end() ? defaultValue : t->second;
}

uint64_t InterruptView::stateVersion(const TUIMessage& msg) const {
    Key key;
    if (!keyOf(msg, key)) {
        return 0;
    }
    auto it = states_.find(key);
    return it == states_.end() ? 0 : it->second.version;
}

InterruptView::FormState InterruptView::formState(size_t msgIndex) {
    auto st = ctx_.state ? ctx_.state->readSnapshot() : ctx_.frameState;
    if (!st || msgIndex >= st->messages.size()) {
        return {};
    }
    const auto& msg = *st->messages[msgIndex];
    Key         key;
    if (!keyOf(msg, key)) {
        return {};
    }
    return uiStateFor(msg); // 惰性初始化 (描述默认值/消息默认值)
}

// ---------------------------------------------------------------------------
// 描述解析 (含模板语义: 描述字段为空时取消息字段)
// ---------------------------------------------------------------------------

middleware::InterruptUi InterruptView::resolveUi(const TUIMessage& msg) const {
    if (msg.interrupt && msg.interrupt->ui.is_object()) {
        auto ui = middleware::InterruptUi::fromJson(msg.interrupt->ui);
        if (!ui.empty()) {
            return ui;
        }
    }
    // 无描述 (旧服务端/未声明): 通用默认模板 (与 agent 侧 defaultUi 同语义)
    return middleware::InterruptUi::defaultUi();
}

std::string InterruptView::defaultViewFor(std::string_view inputType) {
    if (inputType == "bool") {
        return "buttons";
    }
    if (inputType == "enum") {
        return "list";
    }
    if (inputType == "int" || inputType == "double") {
        return "number";
    }
    return "text";
}

std::vector<middleware::InterruptUiButton> InterruptView::builtinBoolButtons() {
    middleware::InterruptUiButton yes;
    yes.value    = "true";
    yes.labelKey = "interrupt.yes";
    yes.label    = "Yes";
    middleware::InterruptUiButton no;
    no.value    = "false";
    no.labelKey = "interrupt.no";
    no.label    = "No";
    return {std::move(yes), std::move(no)};
}

InterruptView::Resolved
    InterruptView::resolveItem(const middleware::InterruptUiItem& item, const TUIMessage& msg) const {
    Resolved r;
    r.kind     = item.kind;
    r.text     = item.text;
    r.labelKey = item.labelKey;
    r.color    = item.color;
    r.bold     = item.bold;
    r.dim      = item.dim;
    r.wrap     = item.wrap;
    r.indent   = std::max(0, item.indent);
    r.lines    = std::max(1, item.lines);

    r.id            = item.id;
    r.defaultToggle = item.defaultToggle;
    r.path          = item.path;
    r.oldStr        = item.oldStr;
    r.newStr        = item.newStr;

    if (r.kind != "input") {
        return r;
    }
    const auto& id = msg.interrupt;
    r.inputType    = !item.inputType.empty() ? item.inputType : (id ? id->inputType : std::string{});
    r.defaultValue = !item.defaultValue.empty() ? item.defaultValue
                                                : (id ? id->inputDefault : std::string{});
    r.enums        = !item.enumValues.empty() ? item.enumValues
                                              : (id ? id->inputEnums : std::vector<std::string>{});
    r.view         = !item.view.empty() ? item.view : defaultViewFor(r.inputType);
    r.buttons      = item.buttons;
    if (r.view == "buttons" && r.buttons.empty()) {
        // 描述未声明按钮: bool 输入用内置的是/否按钮 (通用形态, 非特化)
        r.buttons = builtinBoolButtons();
    }
    if (r.id.empty()) {
        r.id = "value";
    }
    return r;
}

const middleware::InterruptUiItem*
    InterruptView::findInputItem(const middleware::InterruptUi& ui, const TUIMessage& /*msg*/) const {
    // 描述声明的结果顺序优先 (result.values[0] 指定的项), 否则首个 input 项
    if (!ui.values.empty()) {
        for (const auto& item : ui.items) {
            if (item.kind == "input" && item.id == ui.values[0]) {
                return &item;
            }
        }
    }
    for (const auto& item : ui.items) {
        if (item.kind == "input") {
            return &item;
        }
    }
    return nullptr;
}

bool InterruptView::resolveInputItem(const TUIMessage& msg, Resolved& out) const {
    const auto ui    = resolveUi(msg);
    const auto* item = findInputItem(ui, msg);
    if (item == nullptr) {
        return false;
    }
    out = resolveItem(*item, msg);
    return true;
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

std::string InterruptView::resolveLabel(std::string_view labelKey, std::string_view text) const {
    if (!labelKey.empty()) {
        auto translated = tr(labelKey);
        // 词表缺键时 t() 回退返回 key 本身 (便于发现漏配): 此时用字面文本更合适
        if (!translated.empty() && translated != labelKey) {
            return std::string{translated};
        }
    }
    return std::string{text};
}

Element InterruptView::buildHeader(const TUIMessage& msg, const middleware::InterruptUi& ui) const {
    const auto& theme = *ctx_.theme;
    Elements    segs;
    if (ui.header.segments.empty()) {
        // 默认前缀 (语言随 TuiI18n): 进度形态 "! [Interrupt] Input i/n: "
        if (ui.header.progress) {
            segs.push_back(
                text(trf("interrupt.header", msg.interrupt->inputIndex, msg.interrupt->inputTotal))
                | color(theme.accentColor) | bold
            );
        } else {
            segs.push_back(
                text(tr("interrupt.headerNoProgress")) | color(theme.accentColor) | bold
            );
        }
    } else {
        // 自定义分段 (完全由描述决定; 描述未着色时用普通内容色)
        for (const auto& seg : ui.header.segments) {
            auto label = resolveLabel(seg.labelKey, seg.text);
            if (label.empty()) {
                continue;
            }
            Element el = text(label) | color(client::uiRoleColor(seg.color, theme));
            if (seg.bold) {
                el = el | bold;
            }
            if (seg.dim) {
                el = el | dim;
            }
            segs.push_back(std::move(el));
        }
    }
    if (ui.header.label && !msg.interrupt->inputLabel.empty()) {
        segs.push_back(text(msg.interrupt->inputLabel) | color(theme.accentColor) | xflex_shrink);
    }
    return hbox(std::move(segs));
}

Element InterruptView::renderValueButton(
    const middleware::InterruptUiButton& btn,
    bool                                 active
) const {
    const auto& theme = *ctx_.theme;
    auto        label = resolveLabel(btn.labelKey, btn.label);

    PluginButtonDesc desc;
    desc.label = std::move(label);
    // 选中项高亮 (强调样式); 未选中按描述色 (error = 高危操作, 其余普通按钮)
    if (active) {
        desc.role = PluginButtonRole::Accent;
    } else if (btn.color == "error" || btn.color == "danger") {
        desc.role = PluginButtonRole::Danger;
    } else {
        desc.role = PluginButtonRole::Normal;
    }
    return renderPluginButton(desc, theme);
}

void InterruptView::hit(
    size_t                      msgIndex,
    std::string                 itemId,
    int                         sub,
    const std::shared_ptr<Box>& box
) {
    HitBox hb;
    hb.msgIndex = msgIndex;
    hb.itemId   = std::move(itemId);
    hb.sub      = sub;
    hb.box      = box;
    hits_.push_back(std::move(hb));
}

void InterruptView::appendItemRows(
    const TUIMessage& msg,
    size_t            msgIndex,
    const Resolved&   item,
    const Resolved*   prev,
    int               maxWidth,
    Elements&         rows
) {
    const auto& theme = *ctx_.theme;
    const auto  indent = std::string(static_cast<size_t>(item.indent), ' ');

    if (item.kind == "text") {
        // 文本: labelKey/text 为空时取消息 inputDepict (模板语义); 全空则不渲染
        std::string content = resolveLabel(item.labelKey, item.text);
        if (content.empty() && msg.interrupt) {
            content = msg.interrupt->inputDepict;
        }
        if (content.empty()) {
            return;
        }
        const Color  c     = client::uiRoleColor(item.color, theme);
        const auto   width = std::max(1, maxWidth);
        if (item.wrap) {
            // 硬折行 (无空格长路径/URL 也能按宽度断行)
            const int avail = std::max(10, width - item.indent);
            for (auto& line : wrapTextToLines(content, avail)) {
                Elements els;
                if (!indent.empty()) {
                    els.push_back(text(indent));
                }
                els.push_back(text(std::move(line)) | color(c));
                rows.push_back(hbox(std::move(els)));
            }
            return;
        }
        Element el = text(content) | color(c) | xflex_shrink;
        if (item.bold) {
            el = el | bold;
        }
        if (item.dim) {
            el = el | dim;
        }
        Elements els;
        if (!indent.empty()) {
            els.push_back(text(indent));
        }
        els.push_back(std::move(el));
        rows.push_back(hbox(std::move(els)));
        return;
    }

    if (item.kind == "gap") {
        for (int i = 0; i < item.lines; ++i) {
            rows.push_back(text(" "));
        }
        return;
    }

    if (item.kind == "separator") {
        Elements els;
        if (!indent.empty()) {
            els.push_back(text(indent));
        }
        els.push_back(text("─") | color(theme.hintColor) | dim | xflex_shrink);
        rows.push_back(hbox(std::move(els)));
        return;
    }

    if (item.kind == "diff") {
        rows.push_back(client::renderPluginDiff(item.path, item.oldStr, item.newStr, theme, maxWidth));
        return;
    }

    if (item.kind == "toggle") {
        // 勾选项: 整行可点 (左指示器 + 标签)
        const bool on  = toggleValue(msg, item.id, item.defaultToggle);
        auto       box = std::make_shared<Box>();
        auto       indicator = on ? text("[ ✓ ] ") | color(theme.accentColor) | bold
                                  : text("[   ] ") | color(theme.hintColor);
        auto       label     = resolveLabel(item.labelKey, item.text);
        // 标签用普通内容色: 按钮文字色在深色主题下为纯黑, 无背景时会不可见
        Element row = hbox({
                          std::move(indicator),
                          text(label) | color(theme.normalColor),
                      })
                      | reflect(*box);
        hit(msgIndex, item.id, 0, box);
        rows.push_back(std::move(row));
        return;
    }

    if (item.kind == "input") {
        auto& state = uiStateFor(msg);
        if (item.view == "buttons") {
            Elements els;
            for (size_t i = 0; i < item.buttons.size(); ++i) {
                auto    box = std::make_shared<Box>();
                Element btn = renderValueButton(item.buttons[i], state.selected == static_cast<int>(i))
                              | reflect(*box);
                hit(msgIndex, item.id, static_cast<int>(i), box);
                if (i > 0) {
                    els.push_back(text(" "));
                }
                els.push_back(std::move(btn));
            }
            rows.push_back(hbox(std::move(els)));
            return;
        }
        if (item.view == "number") {
            auto minusBox = std::make_shared<Box>();
            auto plusBox  = std::make_shared<Box>();
            auto editBox  = std::make_shared<Box>();
            hit(msgIndex, item.id, kSubNumMinus, minusBox);
            hit(msgIndex, item.id, kSubNumPlus, plusBox);
            hit(msgIndex, item.id, kSubEdit, editBox);
            auto btnStyle = [&theme](std::string_view label) {
                return text(label) | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
            };
            rows.push_back(hbox({
                btnStyle("[ - ]") | reflect(*minusBox),
                text(" "),
                text(" " + state.editText + " ") | bgcolor(theme.inputBgColor)
                    | color(theme.inputTextColor) | reflect(*editBox) | xflex_shrink,
                text(" "),
                btnStyle("[ + ]") | reflect(*plusBox),
            }));
            return;
        }
        if (item.view == "list") {
            // 枚举项竖直列表 (选中项高亮); 逐项渲染, 不截断
            Elements els;
            for (size_t i = 0; i < item.enums.size(); ++i) {
                const bool active = (state.selected == static_cast<int>(i));
                auto       box    = std::make_shared<Box>();
                Element    entry  = text(fmt::format(" {} {}", active ? "▸" : " ", item.enums[i]));
                if (active) {
                    entry = entry | bgcolor(theme.buttonActiveBgColor)
                            | color(theme.buttonActiveTextColor) | bold;
                } else {
                    entry = entry | color(theme.buttonTextColor);
                }
                els.push_back(entry | reflect(*box));
                hit(msgIndex, item.id, static_cast<int>(i), box);
            }
            rows.push_back(vbox(std::move(els)));
            return;
        }
        // 文本输入框
        auto editBox = std::make_shared<Box>();
        hit(msgIndex, item.id, 0, editBox);
        rows.push_back(hbox({
            text(" " + state.editText + " ") | bgcolor(theme.inputBgColor)
                | color(theme.inputTextColor) | reflect(*editBox) | xflex_shrink,
        }));
        return;
    }

    if (item.kind == "submit") {
        auto confirmBox = std::make_shared<Box>();
        auto cancelBox  = std::make_shared<Box>();
        hit(msgIndex, std::string{kItemIdSubmit}, kSubSubmitConfirm, confirmBox);
        hit(msgIndex, std::string{kItemIdSubmit}, kSubSubmitCancel, cancelBox);

        auto        confirmLabel = resolveLabel(item.labelKey, item.text);
        if (confirmLabel.empty()) {
            confirmLabel = std::string{tr("interrupt.confirm")};
        }
        Element confirmEl = text(confirmLabel) | bgcolor(theme.buttonBgColor)
                            | color(theme.buttonTextColor) | reflect(*confirmBox);
        Element cancelEl = text(tr("interrupt.cancel")) | color(theme.errorColor)
                           | reflect(*cancelBox);
        // 列表形态的输入项 (多行) 独占一行: 前置空行后另起一行渲染确认/取消
        const bool prevIsListInput
            = prev != nullptr && prev->isInput() && prev->view == "list";
        const bool inlineSubmit
            = prev != nullptr && prev->isInput() && prev->view != "list" && !rows.empty();
        Element submitRow = hbox({std::move(confirmEl), text("  "), std::move(cancelEl)});
        if (inlineSubmit) {
            rows.back() = hbox({std::move(rows.back()), text("  "), std::move(submitRow)});
        } else {
            if (prevIsListInput) {
                rows.push_back(text(" "));
            }
            rows.push_back(std::move(submitRow));
        }
        return;
    }
    // 未知 kind: 忽略 (描述向前兼容; 后续版本新增项不会破坏旧客户端渲染)
}

size_t InterruptView::estimateItemLines(
    const TUIMessage& msg,
    const Resolved&   item,
    const Resolved*   prev,
    int               width
) const {
    if (item.kind == "text") {
        std::string content = resolveLabel(item.labelKey, item.text);
        if (content.empty() && msg.interrupt) {
            content = msg.interrupt->inputDepict;
        }
        if (content.empty()) {
            return 0;
        }
        if (item.wrap) {
            const int avail = std::max(10, std::max(1, width) - item.indent);
            return wrapTextToLines(content, avail).size();
        }
        // 非折行形态按单行渲染 (超宽由 xflex_shrink 在右缘裁剪), 估算恒 1 行
        return 1;
    }
    if (item.kind == "gap") {
        return static_cast<size_t>(item.lines);
    }
    if (item.kind == "separator") {
        return 1;
    }
    if (item.kind == "diff") {
        // 与 renderPluginDiff 语义一致: 路径行 (可选) + 差异行 (side-by-side 减半)
        const auto   diff       = agentxx::util::computeLineDiff(item.oldStr, item.newStr);
        const bool   sideBySide = std::max(1, width) >= 100;
        const size_t pathLines  = item.path.empty() ? 0 : 1;
        if (diff.empty()) {
            return pathLines + 1; // "(no changes)"
        }
        return pathLines + (sideBySide ? (diff.size() / 2 + 1) : diff.size());
    }
    if (item.kind == "toggle") {
        return 1;
    }
    if (item.kind == "input") {
        if (item.view == "list") {
            return std::max<size_t>(1, item.enums.size());
        }
        return 1;
    }
    if (item.kind == "submit") {
        if (prev != nullptr && prev->isInput() && prev->view != "list") {
            return 0; // 与控件同行渲染
        }
        // 列表形态: 前置空行 + 确认行; 其余: 单独一行
        return (prev != nullptr && prev->isInput() && prev->view == "list") ? 2 : 1;
    }
    return 0; // 未知 kind 忽略
}

Element InterruptView::buildStatusLine(const TUIMessage& msg) const {
    const auto& theme = *ctx_.theme;
    if (!msg.interrupt) {
        return text("");
    }
    const auto& it = *msg.interrupt;
    switch (it.interruptStatus) {
        case TUIMessage::InterruptStatus::Confirmed:
            return hbox({
                text(trf("interrupt.header", it.inputIndex, it.inputTotal))
                    | color(theme.hintColor),
                text(trf("interrupt.confirmed", it.inputLabel, it.interruptResult))
                    | color(theme.accentColor) | dim | xflex_shrink,
            });
        case TUIMessage::InterruptStatus::Cancelled:
            return hbox({
                text(trf("interrupt.header", it.inputIndex, it.inputTotal))
                    | color(theme.hintColor),
                text(trf("interrupt.cancelled", it.inputLabel)) | color(theme.errorColor) | dim
                    | xflex_shrink,
            });
        case TUIMessage::InterruptStatus::Expired:
            return hbox({
                text(trf("interrupt.header", it.inputIndex, it.inputTotal))
                    | color(theme.hintColor),
                text(trf("interrupt.expired", it.inputLabel)) | color(theme.errorColor) | dim
                    | xflex_shrink,
            });
        default:
            return text("");
    }
}

Element InterruptView::build(const TUIMessage& msg, size_t msgIndex, int maxWidth) {
    if (!msg.interrupt) {
        return text("");
    }
    if (!isWaiting(msg)) {
        return buildStatusLine(msg);
    }

    const auto& theme = *ctx_.theme;
    const auto  ui    = resolveUi(msg);

    Elements rows;
    rows.push_back(buildHeader(msg, ui));

    // 交互输入项: 一条消息对应一个输入项 (结果 values 的取值来源);
    // 描述中多余的 input 项忽略 (模板语义下同一描述服务于每个输入项消息)
    const auto*             inputItem = findInputItem(ui, msg);
    std::optional<Resolved> prev;
    for (const auto& raw : ui.items) {
        if (raw.kind == "input" && &raw != inputItem) {
            continue;
        }
        Resolved item = resolveItem(raw, msg);
        appendItemRows(msg, msgIndex, item, prev ? &*prev : nullptr, maxWidth, rows);
        prev = std::move(item);
    }

    // 校验失败提示 (表单状态, 非消息内容)
    Key key;
    if (keyOf(msg, key)) {
        auto it = states_.find(key);
        if (it != states_.end() && !it->second.tip.empty()) {
            rows.push_back(hbox({
                text("  "),
                text(it->second.tip) | color(theme.errorColor) | xflex_shrink,
            }));
        }
    }
    return vbox(std::move(rows));
}

// ---------------------------------------------------------------------------
// 高度估算 (与 build 同一套项判定; 供未进入视口的消息使用)
// ---------------------------------------------------------------------------

size_t InterruptView::estimate(const TUIMessage& msg, int width) const {
    if (!msg.interrupt) {
        return 1;
    }
    if (!isWaiting(msg)) {
        return 1; // 状态行
    }
    const auto ui    = resolveUi(msg);
    size_t     lines = 1; // 头行

    const auto*             inputItem = findInputItem(ui, msg);
    std::optional<Resolved> prev;
    for (const auto& raw : ui.items) {
        if (raw.kind == "input" && &raw != inputItem) {
            continue;
        }
        Resolved item = resolveItem(raw, msg);
        lines += estimateItemLines(msg, item, prev ? &*prev : nullptr, width);
        prev = std::move(item);
    }

    Key key;
    if (keyOf(msg, key)) {
        auto it = states_.find(key);
        if (it != states_.end() && !it->second.tip.empty()) {
            ++lines;
        }
    }
    return lines;
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

void InterruptView::sendResult(
    int64_t                    wireId,
    int                        inputIndex,
    std::optional<std::string> value,
    const agentxx::util::Json& options
) {
    auto it = channels_.find(wireId);
    if (it == channels_.end() || !it->second) {
        XX_LOGD("[tui] interrupt result dropped (no channel): id={}, index={}", wireId, inputIndex);
        return;
    }
    // 结果经通道回传到 client 线程 (线程安全): {输入项序号, 值, 勾选项}
    it->second->async_send(
        neograph_asio_error_code{},
        inputIndex,
        std::move(value),
        options,
        [](neograph_asio_error_code) {}
    );
}

bool InterruptView::handleClick(const Mouse& mouse, const Box& areaBox) {
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    if (areaBox.x_max < areaBox.x_min) {
        return false; // 尚未布局
    }
    if (mouse.x < areaBox.x_min || mouse.x > areaBox.x_max) {
        return false;
    }

    // 命中检测 (后渲染的控件优先: 顺序查找; 命中区域为上一帧布局结果)
    for (const auto& h : hits_) {
        if (!h.box) {
            continue;
        }
        const auto& box = *h.box;
        if (mouse.y < box.y_min || mouse.y > box.y_max || mouse.x < box.x_min
            || mouse.x > box.x_max) {
            continue;
        }

        // 命中后的动作在锁外执行 (confirm/step 内部再次加锁, 避免同锁重入)
        enum class Act : uint8_t {
            None,
            Confirm,
            Cancel,
            StepUp,
            StepDown,
            StateChanged,
        };
        const size_t mi  = h.msgIndex;
        Act          act = Act::None;

        ctx_.state->mutate([&](TUIRenderState& st) {
            if (mi >= st.messages.size()) {
                return;
            }
            const auto& msg = *st.messages[mi];
            if (!isWaiting(msg)) {
                return;
            }
            const auto ui = resolveUi(msg);
            Resolved   input;
            const bool hasInput = resolveInputItem(msg, input);

            if (h.itemId == std::string{kItemIdSubmit}) {
                act = (h.sub == kSubSubmitCancel) ? Act::Cancel : Act::Confirm;
                return;
            }
            // 勾选项 (描述项 id 匹配)
            for (const auto& item : ui.items) {
                if (item.kind != "toggle" || item.id != h.itemId) {
                    continue;
                }
                auto&      state = mutateUiState(msg);
                const bool cur   = toggleValue(msg, item.id, item.defaultToggle);
                state.toggles[item.id] = !cur;
                act                    = Act::StateChanged;
                return;
            }
            // 输入项控件
            if (hasInput && input.id == h.itemId) {
                if (input.view == "buttons") {
                    // 值按钮: 选中并确认 (一问一答形态)
                    auto&      state = mutateUiState(msg);
                    const int  n     = static_cast<int>(input.buttons.size());
                    state.selected   = std::clamp(h.sub, 0, std::max(0, n - 1));
                    state.tip.clear();
                    act              = Act::Confirm;
                } else if (input.view == "list") {
                    auto& state    = mutateUiState(msg);
                    state.selected = h.sub;
                    state.tip.clear();
                    act            = Act::StateChanged;
                } else if (input.view == "number") {
                    if (h.sub == kSubNumMinus) {
                        act = Act::StepDown;
                    } else if (h.sub == kSubNumPlus) {
                        act = Act::StepUp;
                    } else {
                        act = Act::StateChanged; // 输入框: 仅激活
                    }
                } else {
                    act = Act::StateChanged; // 文本输入框: 仅激活
                }
                return;
            }
        });

        if (act == Act::None) {
            return false; // 未处理 (消息已结束/非交互项)
        }
        setActiveMsg(mi);
        switch (act) {
            case Act::Confirm:
                confirm(mi);
                break;
            case Act::Cancel:
                cancel(mi);
                break;
            case Act::StepUp:
                step(mi, 1.0);
                break;
            case Act::StepDown:
                step(mi, -1.0);
                break;
            case Act::StateChanged:
            default:
                ctx_.postRedraw();
                break;
        }
        return true;
    }
    return false;
}

bool InterruptView::handleKey(Event event) {
    const size_t mi = activeMsg_;
    if (mi == static_cast<size_t>(-1)) {
        return false;
    }

    enum class Act : uint8_t {
        None,
        Confirm,
        StepUp,
        StepDown,
        Escape,
        Handled,
    };
    Act         act        = Act::None;
    bool        stillValid = false;

    // 校验激活消息仍可交互, 并按描述形态处理按键 (状态修改在锁内完成)
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (mi >= st.messages.size()) {
            return;
        }
        const auto& msg = *st.messages[mi];
        if (!isWaiting(msg)) {
            return;
        }
        Resolved input;
        if (!resolveInputItem(msg, input)) {
            return;
        }
        stillValid = true;

        if (event == Event::Escape) {
            act = Act::Escape;
            return;
        }
        if (event == Event::Return) {
            act = Act::Confirm;
            return;
        }
        if (input.view == "buttons") {
            // 左右切换选中项 (值按钮: 选中即高亮, 确认由 Enter/点击完成)
            if (event == Event::ArrowLeft || event == Event::ArrowRight) {
                auto&     state = mutateUiState(msg);
                const int n     = static_cast<int>(input.buttons.size());
                if (n > 0) {
                    const int stepDir = (event == Event::ArrowRight) ? 1 : n - 1;
                    state.selected    = (state.selected + stepDir) % n;
                }
                act = Act::Handled;
            }
            return;
        }
        if (input.view == "list") {
            const int delta = (event == Event::ArrowUp)     ? -1
                              : (event == Event::ArrowDown) ? 1
                                                            : 0;
            if (delta != 0) {
                auto&     state = mutateUiState(msg);
                const int n     = static_cast<int>(input.enums.size());
                state.selected  = std::clamp(state.selected + delta, 0, std::max(0, n - 1));
                state.tip.clear();
                act = Act::Handled;
            }
            return;
        }
        // number / text: 上下方向键步进 (number) 或文本编辑
        if (input.view == "number" && (event == Event::ArrowUp || event == Event::ArrowDown)) {
            act = (event == Event::ArrowUp) ? Act::StepUp : Act::StepDown;
            return;
        }
        if (event.is_character() || event == Event::Backspace || event == Event::Delete) {
            auto& state = mutateUiState(msg);
            if (event.is_character()) {
                if (!state.edited) {
                    // 首次输入替换默认值 (与输入框激活时保留默认值的语义一致)
                    state.editText.clear();
                    state.edited = true;
                }
                state.editText += event.character();
            } else if (!state.editText.empty()) {
                state.editText.pop_back();
                state.edited = true;
            }
            state.tip.clear();
            act = Act::Handled;
            return;
        }
        // 左右方向键: 单行输入无光标定位, 标记已处理避免落到其他组件
        if (event == Event::ArrowLeft || event == Event::ArrowRight) {
            act = Act::Handled;
        }
    });

    if (!stillValid) {
        clearActive();
        return false;
    }
    switch (act) {
        case Act::Confirm:
            confirm(mi);
            return true;
        case Act::StepUp:
            step(mi, 1.0);
            return true;
        case Act::StepDown:
            step(mi, -1.0);
            return true;
        case Act::Escape:
            clearActive();
            ctx_.postRedraw();
            return true;
        case Act::Handled:
            ctx_.postRedraw();
            return true;
        case Act::None:
        default:
            return false;
    }
}

void InterruptView::confirm(size_t msgIndex) {
    std::string         value;
    bool                confirmed = false;
    int64_t             wireId    = 0;
    int                 inputIdx  = 0;
    agentxx::util::Json options   = agentxx::util::Json::object();

    ctx_.state->mutate([&](TUIRenderState& st) {
        if (msgIndex >= st.messages.size()) {
            return;
        }
        const auto& src = *st.messages[msgIndex];
        if (!isWaiting(src)) {
            return;
        }
        const auto ui = resolveUi(src);
        Resolved   input;
        if (!resolveInputItem(src, input)) {
            return;
        }
        auto& state = mutateUiState(src); // 校验提示/确认结果影响渲染 → 递增版本

        if (input.view == "buttons") {
            const int n = static_cast<int>(input.buttons.size());
            if (n <= 0) {
                return;
            }
            const int sel = std::clamp(state.selected, 0, n - 1);
            value         = input.buttons[static_cast<size_t>(sel)].value;
            confirmed     = true;
        } else if (input.view == "list") {
            const int n = static_cast<int>(input.enums.size());
            if (n <= 0 || state.selected < 0 || state.selected >= n) {
                return;
            }
            value     = input.enums[static_cast<size_t>(state.selected)];
            confirmed = true;
        } else if (input.view == "number") {
            std::string errTip;
            if (input.inputType == "int") {
                int64_t num = 0;
                if (agentxx::util::parseNumberFromString(state.editText, num).ec != std::errc{}) {
                    errTip = std::string{tr("interrupt.tipInt")};
                }
            } else {
                double num = 0.0;
                if (agentxx::util::parseNumberFromString(state.editText, num).ec != std::errc{}) {
                    errTip = std::string{tr("interrupt.tipNum")};
                }
            }
            if (!errTip.empty()) {
                state.tip = std::move(errTip);
                ctx_.postRedraw();
                return;
            }
            value     = state.editText;
            confirmed = true;
        } else {
            value     = state.editText;
            confirmed = true;
        }
        if (!confirmed) {
            return;
        }
        state.tip.clear();

        // 勾选项结果 (描述声明的顺序优先, 否则全部 toggle 项)
        auto putOption = [&](const std::string& id) {
            if (id.empty()) {
                return;
            }
            for (const auto& raw : ui.items) {
                if (raw.kind == "toggle" && raw.id == id) {
                    options[id] = toggleValue(src, id, raw.defaultToggle);
                    return;
                }
            }
        };
        if (!ui.options.empty()) {
            for (const auto& id : ui.options) {
                putOption(id);
            }
        } else {
            for (const auto& raw : ui.items) {
                if (raw.kind == "toggle") {
                    putOption(raw.id);
                }
            }
        }

        wireId   = src.interrupt ? src.interrupt->interruptId : 0;
        inputIdx = src.interrupt ? src.interrupt->inputIndex : 0;
        // 确认结果写入消息 (跨线程共享的展示状态); 表单状态保留编辑残留
        auto& mm                      = ctx_.state->mutableMessage(st, msgIndex);
        mm.interrupt->interruptStatus = TUIMessage::InterruptStatus::Confirmed;
        mm.interrupt->interruptResult = value;
    });

    if (confirmed) {
        sendResult(wireId, inputIdx, std::optional<std::string>(value), options);
        ctx_.postRedraw();
    }
}

void InterruptView::cancel(size_t msgIndex) {
    int64_t wireId = 0;
    bool    valid  = false;

    ctx_.state->mutate([&](TUIRenderState& st) {
        if (msgIndex >= st.messages.size() || !st.messages[msgIndex]->interrupt) {
            return;
        }
        const auto& src = *st.messages[msgIndex];
        if (!isWaiting(src)) {
            return;
        }
        wireId = src.interrupt->interruptId;
        valid  = true;
        // 标记同请求所有未操作消息为 Cancelled
        for (size_t i = 0; i < st.messages.size(); ++i) {
            const auto& m = *st.messages[i];
            if (m.role == TUIMessage::Role::Interrupt && m.interrupt
                && m.interrupt->interruptId == wireId
                && m.interrupt->interruptStatus == TUIMessage::InterruptStatus::Waiting) {
                auto& mm                      = ctx_.state->mutableMessage(st, i);
                mm.interrupt->interruptStatus = TUIMessage::InterruptStatus::Cancelled;
            }
        }
    });
    if (!valid) {
        return;
    }
    // 清理同请求的表单状态 (消息已固定状态, 编辑残留不再需要)
    for (auto it = states_.begin(); it != states_.end();) {
        if (it->first.id == wireId) {
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
    // 整体取消: inputIndex = -1 + 空值 (client 线程据此终止等待)
    sendResult(wireId, -1, std::nullopt, agentxx::util::Json::object());
    activeMsg_ = static_cast<size_t>(-1);
    ctx_.postRedraw();
}

void InterruptView::step(size_t msgIndex, double delta) {
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (msgIndex >= st.messages.size()) {
            return;
        }
        const auto& src = *st.messages[msgIndex];
        if (!isWaiting(src)) {
            return;
        }
        Resolved input;
        if (!resolveInputItem(src, input) || input.view != "number") {
            return;
        }
        auto&  state = mutateUiState(src);
        double val   = 0.0;
        if (input.inputType == "int") {
            int64_t num = 0;
            if (agentxx::util::parseNumberFromString(state.editText, num).ec != std::errc{}) {
                return; // 编辑值非法时步进无效
            }
            val = static_cast<double>(num);
        } else {
            double num = 0.0;
            if (agentxx::util::parseNumberFromString(state.editText, num).ec != std::errc{}) {
                return;
            }
            val = num;
        }
        val += delta;
        if (input.inputType == "int") {
            state.editText = fmt::format("{}", static_cast<int64_t>(val));
        } else {
            state.editText = formatStepDouble(val);
        }
        state.edited = true;
        state.tip.clear();
    });
    ctx_.postRedraw();
}

} // namespace client
} // namespace agentxx
