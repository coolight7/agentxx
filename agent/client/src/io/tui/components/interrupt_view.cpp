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

/// 空表单状态的默认输入控件状态 (越界访问时返回)
const InterruptView::InputState& emptyInputState() {
    static const InterruptView::InputState empty{};
    return empty;
}

} // namespace

const InterruptView::InputState& InterruptView::FormState::input(size_t index) const {
    return index < inputs.size() ? inputs[index] : emptyInputState();
}

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
    states_.erase(wireId);
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
    // 激活时确保表单状态已按描述初始化 (首次编辑前 edited=false)
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

bool InterruptView::requestIdOf(const TUIMessage& msg, int64_t& out) {
    if (msg.role != TUIMessage::Role::Interrupt || !msg.interrupt) {
        return false;
    }
    out = msg.interrupt->interruptId;
    return true;
}

InterruptView::FormState& InterruptView::uiStateFor(const TUIMessage& msg) {
    int64_t wireId = 0;
    if (!requestIdOf(msg, wireId)) {
        // 非中断消息不应请求表单状态 (调用方按角色分支, 不会走到)
        static FormState fallback;
        return fallback;
    }
    auto [it, inserted] = states_.try_emplace(wireId);
    if (!inserted) {
        return it->second;
    }
    // 惰性初始化: 每个输入控件一份状态 + 勾选项默认值
    auto&      state  = it->second;
    const auto inputs = resolveInputs(msg);
    state.inputs.resize(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto& in     = inputs[i];
        auto&       target = state.inputs[i];
        if (in.view == "buttons") {
            const std::string def = normalizeBoolDefault(in.defaultValue);
            target.selected       = 0;
            for (size_t b = 0; b < in.buttons.size(); ++b) {
                if (in.buttons[b].value == def) {
                    target.selected = static_cast<int>(b);
                    break;
                }
            }
        } else if (in.view == "list") {
            target.selected = 0;
            for (size_t e = 0; e < in.enums.size(); ++e) {
                if (in.enums[e] == in.defaultValue) {
                    target.selected = static_cast<int>(e);
                    break;
                }
            }
        } else if (in.view == "number") {
            target.editText = in.defaultValue;
            if (target.editText.empty()) {
                target.editText = (in.inputType == "double") ? "0.0" : "0";
            }
        } else {
            target.editText = in.defaultValue;
        }
    }
    const auto ui = resolveUi(msg);
    for (const auto& item : ui.items) {
        if (item.kind == "toggle" && !item.id.empty()) {
            state.toggles[item.id] = item.defaultToggle;
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
    int64_t wireId = 0;
    if (id.empty() || !requestIdOf(msg, wireId)) {
        return defaultValue;
    }
    auto it = states_.find(wireId);
    if (it == states_.end()) {
        return defaultValue;
    }
    auto t = it->second.toggles.find(std::string{id});
    return t == it->second.toggles.end() ? defaultValue : t->second;
}

uint64_t InterruptView::stateVersion(const TUIMessage& msg) const {
    int64_t wireId = 0;
    if (!requestIdOf(msg, wireId)) {
        return 0;
    }
    auto it = states_.find(wireId);
    return it == states_.end() ? 0 : it->second.version;
}

InterruptView::FormState InterruptView::formState(size_t msgIndex) {
    auto st = ctx_.state ? ctx_.state->readSnapshot() : ctx_.frameState;
    if (!st || msgIndex >= st->messages.size()) {
        return {};
    }
    const auto& msg    = *st->messages[msgIndex];
    int64_t     wireId = 0;
    if (!requestIdOf(msg, wireId)) {
        return {};
    }
    return uiStateFor(msg); // 惰性初始化 (描述声明的默认值)
}

// ---------------------------------------------------------------------------
// 描述解析 (自包含: 全部字段来自描述本身)
// ---------------------------------------------------------------------------

middleware::InterruptUi InterruptView::resolveUi(const TUIMessage& msg) const {
    // 描述必填 (服务端构造中断请求时总是下发, 见 InterruptHandleArg::toJson);
    // 缺失属于契约违规 (如版本不匹配), 由 build/estimate 输出诊断行, 不再静默
    // 回退到默认模板 (避免用户在"看起来正常但语义已变"的控件上误操作)
    if (!msg.interrupt || !msg.interrupt->ui.is_object()) {
        return {};
    }
    return middleware::InterruptUi::fromJson(msg.interrupt->ui);
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
    InterruptView::resolveItem(const middleware::InterruptUiItem& item, size_t itemIndex) const {
    Resolved r;
    r.itemIndex = itemIndex;
    r.kind      = item.kind;
    r.text      = item.text;
    r.labelKey  = item.labelKey;
    r.color     = item.color;
    r.bold      = item.bold;
    r.dim       = item.dim;
    r.wrap      = item.wrap;
    r.indent    = std::max(0, item.indent);
    r.lines     = std::max(1, item.lines);

    r.id            = item.id;
    r.defaultToggle = item.defaultToggle;
    r.path          = item.path;
    r.oldStr        = item.oldStr;
    r.newStr        = item.newStr;

    if (r.kind != "input") {
        return r;
    }
    // 输入控件字段自包含 (描述即契约, 不再回退消息字段)
    r.inputType    = item.inputType;
    r.defaultValue = item.defaultValue;
    r.enums        = item.enumValues;
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

std::vector<InterruptView::Resolved> InterruptView::resolveInputs(const TUIMessage& msg) const {
    const auto                                      ui = resolveUi(msg);
    std::vector<const middleware::InterruptUiItem*> picked;
    std::vector<bool>                               used(ui.items.size(), false);
    // 描述声明的结果顺序优先 (result.values 的 id 列表), 未命中的 id 记日志跳过
    for (const auto& id : ui.values) {
        bool found = false;
        for (size_t i = 0; i < ui.items.size(); ++i) {
            if (used[i] || ui.items[i].kind != "input" || ui.items[i].id != id) {
                continue;
            }
            used[i] = true;
            found   = true;
            picked.push_back(&ui.items[i]);
            break;
        }
        if (!found) {
            XX_LOGW("[tui] interrupt ui declares unknown input id '{}', ignored", id);
        }
    }
    // 声明中未列出的输入控件按 items 顺序追加 (描述未声明 values 时即全部)
    for (size_t i = 0; i < ui.items.size(); ++i) {
        if (ui.items[i].kind == "input" && !used[i]) {
            picked.push_back(&ui.items[i]);
        }
    }
    std::vector<Resolved> out;
    out.reserve(picked.size());
    for (const auto* item : picked) {
        out.push_back(resolveItem(*item, static_cast<size_t>(item - ui.items.data())));
    }
    return out;
}

int InterruptView::inputIndexOf(const std::vector<Resolved>& inputs, size_t itemIndex) {
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i].itemIndex == itemIndex) {
            return static_cast<int>(i);
        }
    }
    return -1;
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

Element InterruptView::buildHeader(const middleware::InterruptUi& ui) const {
    const auto& theme = *ctx_.theme;
    Elements    segs;
    if (ui.header.segments.empty()) {
        // 默认前缀 (语言随 TuiI18n); 表单形态下不再有输入项进度
        segs.push_back(text(tr("interrupt.header")) | color(theme.accentColor) | bold);
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
    size_t                             msgIndex,
    size_t                             itemIndex,
    std::string                        itemId,
    int                                sub,
    const std::shared_ptr<ftxui::Box>& box
) {
    HitBox h;
    h.msgIndex  = msgIndex;
    h.itemIndex = itemIndex;
    h.itemId    = std::move(itemId);
    h.sub       = sub;
    h.box       = box;
    hits_.push_back(std::move(h));
}

void InterruptView::appendItemRows(
    const TUIMessage& msg,
    size_t            msgIndex,
    const Resolved&   item,
    int               inputStateIndex,
    const Prev&       prev,
    int               maxWidth,
    Elements&         rows
) {
    const auto& theme  = *ctx_.theme;
    const auto  indent = std::string(static_cast<size_t>(item.indent), ' ');

    if (item.kind == "text") {
        // 文本: labelKey/text 为空则不渲染 (描述自包含, 无消息字段回退)
        std::string content = resolveLabel(item.labelKey, item.text);
        if (content.empty()) {
            return;
        }
        const Color c     = client::uiRoleColor(item.color, theme);
        const auto  width = std::max(1, maxWidth);
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
        rows.push_back(
            client::renderPluginDiff(item.path, item.oldStr, item.newStr, theme, maxWidth)
        );
        return;
    }

    if (item.kind == "toggle") {
        // 勾选项: 整行可点 (左指示器 + 标签)
        const bool on        = toggleValue(msg, item.id, item.defaultToggle);
        auto       box       = std::make_shared<Box>();
        auto       indicator = on ? text("[ ✓ ] ") | color(theme.accentColor) | bold
                                  : text("[   ] ") | color(theme.hintColor);
        auto       label     = resolveLabel(item.labelKey, item.text);
        // 标签用普通内容色: 按钮文字色在深色主题下为纯黑, 无背景时会不可见
        Element row = hbox({
                          std::move(indicator),
                          text(label) | color(theme.normalColor),
                      })
                      | reflect(*box);
        hit(msgIndex, item.itemIndex, item.id, 0, box);
        rows.push_back(std::move(row));
        return;
    }

    if (item.kind == "input") {
        // 控件状态 (inputStateIndex 由 build 按描述解析给出; 越界时用临时默认状态)
        InterruptView::InputState  fallbackState;
        InterruptView::InputState* ist = &fallbackState;
        if (inputStateIndex >= 0) {
            auto& state = uiStateFor(msg);
            if (static_cast<size_t>(inputStateIndex) < state.inputs.size()) {
                ist = &state.inputs[static_cast<size_t>(inputStateIndex)];
            }
        }
        // 控件标签 (描述在 input 项上声明 text/labelKey 时; 多控件表单用于区分控件)
        const std::string label = resolveLabel(item.labelKey, item.text);
        if (!label.empty()) {
            Element el = text(label) | color(client::uiRoleColor(item.color, theme));
            if (item.bold) {
                el = el | bold;
            }
            Elements els;
            if (!indent.empty()) {
                els.push_back(text(indent));
            }
            els.push_back(std::move(el) | xflex_shrink);
            rows.push_back(hbox(std::move(els)));
        }
        if (item.view == "buttons") {
            Elements els;
            for (size_t i = 0; i < item.buttons.size(); ++i) {
                auto    box = std::make_shared<Box>();
                Element btn = renderValueButton(item.buttons[i], ist->selected == static_cast<int>(i))
                              | reflect(*box);
                hit(msgIndex, item.itemIndex, item.id, static_cast<int>(i), box);
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
            hit(msgIndex, item.itemIndex, item.id, kSubNumMinus, minusBox);
            hit(msgIndex, item.itemIndex, item.id, kSubNumPlus, plusBox);
            hit(msgIndex, item.itemIndex, item.id, kSubNumEdit, editBox);
            auto btnStyle = [&theme](std::string_view label) {
                return text(label) | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
            };
            rows.push_back(hbox({
                btnStyle("[ - ]") | reflect(*minusBox),
                text(" "),
                text(" " + ist->editText + " ") | bgcolor(theme.inputBgColor)
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
                const bool active = (ist->selected == static_cast<int>(i));
                auto       box    = std::make_shared<Box>();
                Element    entry  = text(fmt::format(" {} {}", active ? "▸" : " ", item.enums[i]));
                if (active) {
                    entry = entry | bgcolor(theme.buttonActiveBgColor)
                            | color(theme.buttonActiveTextColor) | bold;
                } else {
                    entry = entry | color(theme.buttonTextColor);
                }
                els.push_back(entry | reflect(*box));
                hit(msgIndex, item.itemIndex, item.id, static_cast<int>(i), box);
            }
            rows.push_back(vbox(std::move(els)));
            return;
        }
        // 文本输入框
        auto editBox = std::make_shared<Box>();
        hit(msgIndex, item.itemIndex, item.id, 0, editBox);
        rows.push_back(hbox({
            text(" " + ist->editText + " ") | bgcolor(theme.inputBgColor)
                | color(theme.inputTextColor) | reflect(*editBox) | xflex_shrink,
        }));
        return;
    }

    if (item.kind == "submit") {
        auto confirmBox = std::make_shared<Box>();
        auto cancelBox  = std::make_shared<Box>();
        hit(msgIndex, item.itemIndex, std::string{kItemIdSubmit}, kSubSubmitConfirm, confirmBox);
        hit(msgIndex, item.itemIndex, std::string{kItemIdSubmit}, kSubSubmitCancel, cancelBox);

        auto confirmLabel = resolveLabel(item.labelKey, item.text);
        if (confirmLabel.empty()) {
            confirmLabel = std::string{tr("interrupt.confirm")};
        }
        Element confirmEl = text(confirmLabel) | bgcolor(theme.buttonBgColor)
                            | color(theme.buttonTextColor) | reflect(*confirmBox);
        Element cancelEl
            = text(tr("interrupt.cancel")) | color(theme.errorColor) | reflect(*cancelBox);
        Element submitRow = hbox({std::move(confirmEl), text("  "), std::move(cancelEl)});
        if (prev.inlineSubmit() && !rows.empty()) {
            // 值按钮/数值/文本控件: 与控件同行 (紧凑表单)
            rows.back() = hbox({std::move(rows.back()), text("  "), std::move(submitRow)});
        } else {
            // 枚举列表控件 (多行) 后置空行分隔; 其余情况直接另起一行
            if (prev.isInput && prev.isList) {
                rows.push_back(text(" "));
            }
            rows.push_back(std::move(submitRow));
        }
        return;
    }
    // 未知 kind: 忽略 (描述向前兼容: 服务端新增项类型不会破坏已发布的客户端渲染)
}

Element InterruptView::buildStatusLine(const TUIMessage& msg) const {
    const auto& theme = *ctx_.theme;
    if (!msg.interrupt) {
        return text("");
    }
    const auto& it = *msg.interrupt;
    switch (it.interruptStatus) {
        case TUIMessage::InterruptStatus::Confirmed: {
            // 无输入控件的表单 (仅确认行) 提交后无结果值: 用无占位符文案
            const auto confirmedText = it.interruptResult.empty()
                                           ? std::string{tr("interrupt.confirmedEmpty")}
                                           : trf("interrupt.confirmed", it.interruptResult);
            return hbox({
                text(tr("interrupt.header")) | color(theme.hintColor),
                text(confirmedText) | color(theme.accentColor) | dim | xflex_shrink,
            });
        }
        case TUIMessage::InterruptStatus::Cancelled:
            return hbox({
                text(tr("interrupt.header")) | color(theme.hintColor),
                text(tr("interrupt.cancelled")) | color(theme.errorColor) | dim | xflex_shrink,
            });
        case TUIMessage::InterruptStatus::Expired:
            return hbox({
                text(tr("interrupt.header")) | color(theme.hintColor),
                text(tr("interrupt.expired")) | color(theme.errorColor) | dim | xflex_shrink,
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
    if (ui.empty()) {
        // 描述缺失 (契约违规): 仅输出诊断行, 不渲染任何控件 (无命中区域)
        XX_LOGE(
            "[tui] interrupt #{} has no UI descriptor, prompt not interactive",
            msg.interrupt->interruptId
        );
        return hbox({text(tr("interrupt.noDescriptor")) | color(theme.errorColor) | bold});
    }

    // 表单控件解析 (顺序 = 结果 values 顺序)
    const auto inputs = resolveInputs(msg);

    Elements rows;
    rows.push_back(buildHeader(ui));

    Prev prev;
    for (const auto& raw : ui.items) {
        Resolved  item     = resolveItem(raw, static_cast<size_t>(&raw - ui.items.data()));
        const int stateIdx = inputIndexOf(inputs, item.itemIndex);
        appendItemRows(msg, msgIndex, item, stateIdx, prev, maxWidth, rows);
        // 前一项形态 → 提交行同行判定; 控件校验提示 (表单状态, 非消息内容)
        // 紧随该控件之后输出, 并阻止提交行与该提示行合并
        prev = Prev{};
        if (stateIdx >= 0) {
            auto& state = uiStateFor(msg);
            const bool hasTip
                = static_cast<size_t>(stateIdx) < state.inputs.size()
                  && !state.inputs[static_cast<size_t>(stateIdx)].tip.empty();
            prev.isInput  = true;
            prev.isList   = (item.view == "list");
            prev.rowIsTip = hasTip;
            if (hasTip) {
                rows.push_back(hbox({
                    text("  "),
                    text(state.inputs[static_cast<size_t>(stateIdx)].tip) | color(theme.errorColor)
                        | xflex_shrink,
                }));
            }
        }
    }
    return vbox(std::move(rows));
}

// ---------------------------------------------------------------------------
// 高度估算 (与 build 同一套项判定; 供未进入视口的消息使用)
// ---------------------------------------------------------------------------

size_t InterruptView::estimateItemLines(
    const TUIMessage& msg,
    const Resolved&   item,
    int               inputStateIndex,
    const Prev&       prev,
    int               width
) const {
    if (item.kind == "text") {
        const std::string content = resolveLabel(item.labelKey, item.text);
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
        // 控件标签行 (描述在 input 项上声明 text/labelKey 时)
        size_t lines = resolveLabel(item.labelKey, item.text).empty() ? 0 : 1;
        lines += (item.view == "list") ? std::max<size_t>(1, item.enums.size()) : 1;
        // 校验提示行 (表单状态)
        int64_t wireId = 0;
        if (inputStateIndex >= 0 && requestIdOf(msg, wireId)) {
            auto it = states_.find(wireId);
            if (it != states_.end()
                && static_cast<size_t>(inputStateIndex) < it->second.inputs.size()
                && !it->second.inputs[static_cast<size_t>(inputStateIndex)].tip.empty()) {
                ++lines;
            }
        }
        return lines;
    }
    if (item.kind == "submit") {
        if (prev.inlineSubmit()) {
            return 0; // 与控件同行渲染
        }
        // 枚举列表控件: 前置空行 + 提交行; 其余: 单独一行
        return (prev.isInput && prev.isList) ? 2 : 1;
    }
    return 0; // 未知 kind 忽略
}

size_t InterruptView::estimate(const TUIMessage& msg, int width) const {
    if (!msg.interrupt) {
        return 1;
    }
    if (!isWaiting(msg)) {
        return 1; // 状态行
    }
    const auto ui = resolveUi(msg);
    if (ui.empty()) {
        return 1; // 描述缺失: 诊断行
    }
    size_t lines = 1; // 头行

    const auto inputs = resolveInputs(msg);

    Prev prev;
    for (const auto& raw : ui.items) {
        Resolved  item     = resolveItem(raw, static_cast<size_t>(&raw - ui.items.data()));
        const int stateIdx = inputIndexOf(inputs, item.itemIndex);
        lines += estimateItemLines(msg, item, stateIdx, prev, width);
        prev = Prev{};
        if (stateIdx >= 0) {
            int64_t wireId = 0;
            bool    hasTip = false;
            if (requestIdOf(msg, wireId)) {
                auto it = states_.find(wireId);
                hasTip  = it != states_.end()
                         && static_cast<size_t>(stateIdx) < it->second.inputs.size()
                         && !it->second.inputs[static_cast<size_t>(stateIdx)].tip.empty();
            }
            prev.isInput  = true;
            prev.isList   = (item.view == "list");
            prev.rowIsTip = hasTip;
        }
    }
    return lines;
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

void InterruptView::sendSubmit(int64_t wireId, const InterruptFormSubmit& submit) {
    auto it = channels_.find(wireId);
    if (it == channels_.end() || !it->second) {
        XX_LOGD("[tui] interrupt submit dropped (no channel): id={}", wireId);
        return;
    }
    // 结果经通道回传到 client 线程 (线程安全): 一次提交/取消整份表单
    it->second->async_send(neograph_asio_error_code{}, submit, [](neograph_asio_error_code) {});
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
        const size_t mi     = h.msgIndex;
        const size_t itemIx = h.itemIndex;
        Act          act    = Act::None;

        ctx_.state->mutate([&](TUIRenderState& st) {
            if (mi >= st.messages.size()) {
                return;
            }
            const auto& msg = *st.messages[mi];
            if (!isWaiting(msg)) {
                return;
            }
            const auto ui = resolveUi(msg);
            if (itemIx >= ui.items.size()) {
                return;
            }
            const auto& item = ui.items[itemIx];

            if (item.kind == "submit") {
                act = (h.sub == kSubSubmitCancel) ? Act::Cancel : Act::Confirm;
                return;
            }
            // 勾选项 (按项下标定位, 与 id 无关)
            if (item.kind == "toggle") {
                auto&      state       = mutateUiState(msg);
                const bool cur         = toggleValue(msg, item.id, item.defaultToggle);
                state.toggles[item.id] = !cur;
                act                    = Act::StateChanged;
                return;
            }
            // 输入控件
            if (item.kind == "input") {
                const auto inputs  = resolveInputs(msg);
                const int  stateIx = inputIndexOf(inputs, itemIx);
                if (stateIx < 0) {
                    return;
                }
                const auto& input = inputs[static_cast<size_t>(stateIx)];
                auto&       state = mutateUiState(msg);
                state.focused     = stateIx;
                if (static_cast<size_t>(stateIx) >= state.inputs.size()) {
                    return;
                }
                auto& ist = state.inputs[static_cast<size_t>(stateIx)];
                if (input.view == "buttons") {
                    // 值按钮: 选中并提交整份表单 (一问一答形态)
                    const int n  = static_cast<int>(input.buttons.size());
                    ist.selected = std::clamp(h.sub, 0, std::max(0, n - 1));
                    ist.tip.clear();
                    act = Act::Confirm;
                } else if (input.view == "list") {
                    ist.selected = h.sub;
                    ist.tip.clear();
                    act = Act::StateChanged;
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
            case Act::StepDown: {
                // 步进作用于本次命中的控件 (上面已更新 focused)
                const auto state = formState(mi);
                step(
                    mi,
                    static_cast<size_t>(std::max(0, state.focused)),
                    act == Act::StepUp ? 1.0 : -1.0
                );
                break;
            }
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
    Act  act        = Act::None;
    bool stillValid = false;

    // 校验激活消息仍可交互, 按键作用于当前聚焦控件 (状态修改在锁内完成)
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (mi >= st.messages.size()) {
            return;
        }
        const auto& msg = *st.messages[mi];
        if (!isWaiting(msg)) {
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
        const auto inputs = resolveInputs(msg);
        if (inputs.empty()) {
            return;
        }
        auto&     state = mutateUiState(msg);
        const int focus
            = std::clamp(state.focused, 0, static_cast<int>(inputs.size()) - 1);
        state.focused = focus;
        if (static_cast<size_t>(focus) >= state.inputs.size()) {
            return;
        }
        const auto& input = inputs[static_cast<size_t>(focus)];
        auto&       ist   = state.inputs[static_cast<size_t>(focus)];

        if (input.view == "buttons") {
            // 左右切换选中项 (值按钮: 选中即高亮, 提交由 Enter/点击完成)
            if (event == Event::ArrowLeft || event == Event::ArrowRight) {
                const int n = static_cast<int>(input.buttons.size());
                if (n > 0) {
                    const int stepDir = (event == Event::ArrowRight) ? 1 : n - 1;
                    ist.selected      = (ist.selected + stepDir) % n;
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
                const int n  = static_cast<int>(input.enums.size());
                ist.selected = std::clamp(ist.selected + delta, 0, std::max(0, n - 1));
                ist.tip.clear();
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
            if (event.is_character()) {
                if (!ist.edited) {
                    // 首次输入替换默认值 (与输入框激活时保留默认值的语义一致)
                    ist.editText.clear();
                    ist.edited = true;
                }
                ist.editText += event.character();
            } else if (!ist.editText.empty()) {
                ist.editText.pop_back();
                ist.edited = true;
            }
            ist.tip.clear();
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
        case Act::StepDown: {
            const auto state = formState(mi);
            step(
                mi,
                static_cast<size_t>(std::max(0, state.focused)),
                act == Act::StepUp ? 1.0 : -1.0
            );
            return true;
        }
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
    InterruptFormSubmit submit;
    bool                confirmed = false;
    int64_t             wireId    = 0;
    std::string         display;

    ctx_.state->mutate([&](TUIRenderState& st) {
        if (msgIndex >= st.messages.size()) {
            return;
        }
        const auto& src = *st.messages[msgIndex];
        if (!isWaiting(src)) {
            return;
        }
        const auto ui     = resolveUi(src);
        const auto inputs = resolveInputs(src);
        auto&      state  = mutateUiState(src); // 校验提示/提交结果影响渲染 → 递增版本

        agentxx::util::Json values = agentxx::util::Json::array();
        for (size_t k = 0; k < inputs.size(); ++k) {
            const auto& input = inputs[k];
            if (k >= state.inputs.size()) {
                return;
            }
            auto&       ist = state.inputs[k];
            std::string value;
            if (input.view == "buttons") {
                const int n = static_cast<int>(input.buttons.size());
                if (n <= 0) {
                    return;
                }
                const int sel = std::clamp(ist.selected, 0, n - 1);
                value         = input.buttons[static_cast<size_t>(sel)].value;
            } else if (input.view == "list") {
                const int n = static_cast<int>(input.enums.size());
                if (n <= 0 || ist.selected < 0 || ist.selected >= n) {
                    return;
                }
                value = input.enums[static_cast<size_t>(ist.selected)];
            } else if (input.view == "number") {
                std::string errTip;
                if (input.inputType == "int") {
                    int64_t num = 0;
                    if (agentxx::util::parseNumberFromString(ist.editText, num).ec != std::errc{}) {
                        errTip = std::string{tr("interrupt.tipInt")};
                    }
                } else {
                    double num = 0.0;
                    if (agentxx::util::parseNumberFromString(ist.editText, num).ec != std::errc{}) {
                        errTip = std::string{tr("interrupt.tipNum")};
                    }
                }
                if (!errTip.empty()) {
                    ist.tip = std::move(errTip);
                    ctx_.postRedraw();
                    return; // 校验失败: 不提交 (提示保留在该控件下方)
                }
                value = ist.editText;
            } else {
                value = ist.editText;
            }
            ist.tip.clear();
            values.push_back(value);
        }

        // 勾选项结果 (描述声明的顺序优先, 否则全部 toggle 项)
        auto putOption = [&](const std::string& id) {
            if (id.empty()) {
                return;
            }
            for (const auto& raw : ui.items) {
                if (raw.kind == "toggle" && raw.id == id) {
                    submit.options[id] = toggleValue(src, id, raw.defaultToggle);
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

        // 提交结果展示文本 (状态行): 各控件结果值拼接
        for (const auto& v : values) {
            if (!display.empty()) {
                display += ", ";
            }
            display += v.is_string() ? v.get<std::string>() : v.dump();
        }

        requestIdOf(src, wireId);
        submit.values                 = std::move(values);
        submit.cancelled              = false;
        confirmed                     = true;
        auto& mm                      = ctx_.state->mutableMessage(st, msgIndex);
        mm.interrupt->interruptStatus = TUIMessage::InterruptStatus::Confirmed;
        mm.interrupt->interruptResult = display;
    });

    if (confirmed) {
        sendSubmit(wireId, submit);
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
        // 标记同请求全部未操作消息为 Cancelled (一条请求一份表单; 防御性遍历)
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
    // 清理该请求的表单状态 (消息已固定状态, 编辑残留不再需要)
    states_.erase(wireId);
    InterruptFormSubmit submit;
    submit.cancelled = true;
    sendSubmit(wireId, submit);
    activeMsg_ = static_cast<size_t>(-1);
    ctx_.postRedraw();
}

void InterruptView::step(size_t msgIndex, size_t inputStateIndex, double delta) {
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (msgIndex >= st.messages.size()) {
            return;
        }
        const auto& src = *st.messages[msgIndex];
        if (!isWaiting(src)) {
            return;
        }
        const auto inputs = resolveInputs(src);
        if (inputStateIndex >= inputs.size()) {
            return;
        }
        const auto& input = inputs[inputStateIndex];
        if (input.view != "number") {
            return;
        }
        auto& state = mutateUiState(src);
        if (inputStateIndex >= state.inputs.size()) {
            return;
        }
        auto&  ist = state.inputs[inputStateIndex];
        double val = 0.0;
        if (input.inputType == "int") {
            int64_t num = 0;
            if (agentxx::util::parseNumberFromString(ist.editText, num).ec != std::errc{}) {
                return; // 编辑值非法时步进无效
            }
            val = static_cast<double>(num);
        } else {
            double num = 0.0;
            if (agentxx::util::parseNumberFromString(ist.editText, num).ec != std::errc{}) {
                return;
            }
            val = num;
        }
        val += delta;
        if (input.inputType == "int") {
            ist.editText = fmt::format("{}", static_cast<int64_t>(val));
        } else {
            ist.editText = formatStepDouble(val);
        }
        ist.edited = true;
        ist.tip.clear();
    });
    ctx_.postRedraw();
}

} // namespace client
} // namespace agentxx
