#include "agentxx-client/io/tui/components/interrupt_view.h"

#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/markdown_block.h"
#include "agentxx-client/io/tui/plugin_ui_items.h"
#include "agentxx-client/io/tui/text_layout.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include <algorithm>
#include <cmath>
#include <utility>

using namespace ftxui;

namespace agentxx {
namespace client {

namespace {

using agentxx::util::Json;

/// 数值步进后的显示格式 (整数值按 "1.0" 风格, 非整数保留有效精度)
std::string formatStepDouble(double v) {
    if (v == std::floor(v) && std::abs(v) < 1e15) {
        return fmt::format("{:.1f}", v);
    }
    return fmt::format("{:.10g}", v);
}

/// 数值 → 显示文本 (integer 用整数写法, 浮点按上款)
std::string formatNumber(double v, bool integer) {
    if (integer) {
        return fmt::format("{}", static_cast<int64_t>(v));
    }
    return formatStepDouble(v);
}

/// JSON → 布尔 (布尔直取; 字符串 "true"/"yes"/"y"/"1" 为 true, "false"/"no"/"n"/"0" 为 false)
bool jsonBoolValue(const Json& v, bool defaultValue) {
    if (v.is_boolean()) {
        return v.get<bool>();
    }
    if (v.is_number()) {
        return v.get<double>() != 0.0;
    }
    if (v.is_string()) {
        auto s = agentxx::util::toLower(agentxx::util::removeBetweenSpace(v.get<std::string>()));
        if (s == "true" || s == "yes" || s == "y" || s == "1") {
            return true;
        }
        if (s == "false" || s == "no" || s == "n" || s == "0") {
            return false;
        }
    }
    return defaultValue;
}

/// 数值控件的初始编辑文本 (数值/字符串默认值均支持; 缺失按 "0"/"0.0")
std::string numberText(const Json& v, bool integer) {
    if (v.is_number()) {
        return formatNumber(v.get<double>(), integer);
    }
    if (v.is_string() && !v.get<std::string>().empty()) {
        return v.get<std::string>();
    }
    return integer ? "0" : "0.0";
}

/// 值 → 展示文本 (状态行 "标签: 值" 使用)
std::string valueText(const Json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    if (v.is_null()) {
        return {};
    }
    return v.dump();
}

/// 候选项选中下标 (缺失/未命中返回 0)
///
/// 比较口径为**归一化文本**: 布尔 true 与字符串 "true"、数值 1 与字符串 "1"
/// 视为同一候选项 (生产者写默认值时不必与候选项 value 严格同型)
int optionIndex(const std::vector<middleware::InterruptUiOption>& options, const Json& value) {
    if (options.empty() || value.is_null()) {
        return 0;
    }
    const auto want = valueText(value);
    for (size_t i = 0; i < options.size(); ++i) {
        if (valueText(options[i].value) == want) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

const InterruptView::ControlState& emptyControlState() {
    static const InterruptView::ControlState empty{};
    return empty;
}

/// 行追加 helper: 统一控件块的缩进口径 (块 indent + 额外缩进)
struct RowPusher {
    UiRenderResult& out;
    int             indent = 0;

    /// 追加单行元素 (缩进非空时前置空格)
    void push(Element el, size_t lines = 1) const {
        UiRow row;
        row.lines = std::max<size_t>(1, lines);
        if (indent > 0) {
            row.element = hbox({
                text(std::string(static_cast<size_t>(indent), ' ')),
                std::move(el),
            });
        } else {
            row.element = std::move(el);
        }
        out.rows.push_back(std::move(row));
    }
};

} // namespace

const InterruptView::ControlState& InterruptView::FormState::control(std::string_view id) const {
    if (id.empty()) {
        return emptyControlState();
    }
    auto it = controls.find(id);
    return (it == controls.end()) ? emptyControlState() : it->second;
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

std::string InterruptView::controlIdOf(const middleware::InterruptUiBlock& block) {
    return block.id.empty() ? std::string{"value"} : block.id;
}

void InterruptView::initControlState(
    const middleware::InterruptUiBlock& block,
    ControlState&                       state
) {
    if (block.control == "buttons" || block.control == "select") {
        state.selected = optionIndex(block.options, block.defaultValue);
    } else if (block.control == "checkbox") {
        state.checked = jsonBoolValue(block.defaultValue, false);
    } else if (block.control == "number") {
        state.editText = numberText(block.defaultValue, block.integer);
    } else {
        // text / 未知形态: 默认值为字符串 (其他类型按展示文本取值)
        if (block.defaultValue.is_string()) {
            state.editText = block.defaultValue.get<std::string>();
        } else if (!block.defaultValue.is_null()) {
            state.editText = valueText(block.defaultValue);
        }
    }
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
    // 惰性初始化: 每个 control 块一份状态 (key = 控件 id) + 首个控件为键盘焦点
    auto&      state = it->second;
    const auto ui    = resolveUi(msg);
    for (const auto& block : ui.blocks) {
        if (block.kind != "control") {
            continue;
        }
        auto         id = controlIdOf(block);
        ControlState cs;
        initControlState(block, cs);
        auto [cit, cinserted] = state.controls.emplace(id, std::move(cs));
        if (cinserted && state.focusedId.empty()) {
            state.focusedId = std::move(id);
        }
    }
    return state;
}

InterruptView::FormState& InterruptView::mutateUiState(const TUIMessage& msg) {
    auto& state = uiStateFor(msg);
    ++state.version; // 驱动消息列表缓存失效 (高度/滚动重估)
    return state;
}

const InterruptView::FormState* InterruptView::stateOf(const TUIMessage& msg) const {
    int64_t wireId = 0;
    if (!requestIdOf(msg, wireId)) {
        return nullptr;
    }
    auto it = states_.find(wireId);
    return (it == states_.end()) ? nullptr : &it->second;
}

uint64_t InterruptView::stateVersion(const TUIMessage& msg) const {
    const auto* state = stateOf(msg);
    return state ? state->version : 0;
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
// 描述解析与标签
// ---------------------------------------------------------------------------

middleware::InterruptUi InterruptView::resolveUi(const TUIMessage& msg) const {
    // 描述必填 (服务端构造 HIL 中断时总是下发, 见 InterruptHandleArg::ui);
    // 缺失属于契约错误 (生产者未填描述), 由 build/estimate 输出诊断行,
    // 不做任何默认回退 (避免用户在"看起来正常但语义已变"的控件上误操作)
    if (!msg.interrupt || !msg.interrupt->ui.is_object()) {
        return {};
    }
    return middleware::InterruptUi::fromJson(msg.interrupt->ui);
}

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

// ---------------------------------------------------------------------------
// 单一布局过程 (渲染与高度估算同源)
// ---------------------------------------------------------------------------

void InterruptView::layoutSubmit(
    size_t                              msgIndex,
    size_t                              blockIndex,
    const middleware::InterruptUiBlock& block,
    bool                                registerHits,
    UiRenderResult&                     out
) const {
    const auto& theme = *ctx_.theme;
    // 提交行: 确认按钮 (描述可覆盖标签) + 取消按钮
    auto confirmLabel = resolveLabel(block.labelKey, block.label);
    if (confirmLabel.empty()) {
        confirmLabel = std::string{tr("interrupt.confirm")};
    }
    auto cancelLabel = resolveLabel(block.cancelLabelKey, block.cancelLabel);
    if (cancelLabel.empty()) {
        cancelLabel = std::string{tr("interrupt.cancel")};
    }

    auto    confirmBox = registerHits ? std::make_shared<Box>() : nullptr;
    auto    cancelBox  = registerHits ? std::make_shared<Box>() : nullptr;
    Element confirmEl
        = text(confirmLabel) | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
    Element cancelEl = text(cancelLabel) | color(theme.errorColor);
    if (confirmBox) {
        confirmEl = confirmEl | reflect(*confirmBox);
        hit(msgIndex, blockIndex, std::string{kItemIdSubmit}, kSubSubmitConfirm, confirmBox);
    }
    if (cancelBox) {
        cancelEl = cancelEl | reflect(*cancelBox);
        hit(msgIndex, blockIndex, std::string{kItemIdSubmit}, kSubSubmitCancel, cancelBox);
    }
    RowPusher rows{out, std::max(0, block.indent)};
    rows.push(hbox({std::move(confirmEl), text("  "), std::move(cancelEl)}));
}

void InterruptView::layoutControl(
    size_t                              msgIndex,
    size_t                              blockIndex,
    const middleware::InterruptUiBlock& block,
    const ControlState&                 state,
    int                                 width,
    bool                                registerHits,
    UiRenderResult&                     out
) const {
    const auto& theme  = *ctx_.theme;
    const auto  id     = controlIdOf(block);
    const int   indent = std::max(0, block.indent);

    // 渲染上下文: 内容块与控件行同一口径 (基础缩进 0, 缩进由块自身 indent 表达)
    UiRenderCtx rc;
    rc.theme  = ctx_.theme;
    rc.width  = width;
    rc.indent = 0;

    // 控件标签与说明复用内容块的文本渲染 (与描述内其他文本同款样式)
    auto pushText
        = [&](std::string text, const char* color, bool bold, bool wrap, int extraIndent) {
              if (text.empty()) {
                  return;
              }
              middleware::InterruptUiBlock tb;
              tb.kind   = "text";
              tb.text   = std::move(text);
              tb.color  = color;
              tb.bold   = bold;
              tb.wrap   = wrap;
              tb.indent = indent + extraIndent;
              if (auto item = uiItemFromInterruptBlock(tb)) {
                  renderUiItem(*item, rc, out);
              }
          };
    // 标签行: checkbox 的标签即勾选行的行内文本 (不再单独渲染标题行, 避免重复)
    if (block.control != "checkbox") {
        pushText(resolveLabel(block.labelKey, block.label), "accent", true, false, 0);
    }
    pushText(resolveLabel(block.helpKey, block.help), "hint", false, true, 0);

    RowPusher rows{out, indent};

    if (block.control == "buttons") {
        // 横排按钮: 点击选中 (commitOnPick 时点击即提交整份表单)
        Elements els;
        for (size_t i = 0; i < block.options.size(); ++i) {
            auto    box = registerHits ? std::make_shared<Box>() : nullptr;
            Element btn
                = renderValueButton(block.options[i], state.selected == static_cast<int>(i));
            if (box) {
                btn = btn | reflect(*box);
                hit(msgIndex, blockIndex, id, static_cast<int>(i), box);
            }
            if (i > 0) {
                els.push_back(text(" "));
            }
            els.push_back(std::move(btn));
        }
        if (els.empty()) {
            rows.push(
                text(fmt::format("[control `{}` has no options]", id)) | color(theme.errorColor)
                | dim
            );
        } else {
            rows.push(hbox(std::move(els)));
        }
    } else if (block.control == "select") {
        // 竖排单选列表: 逐项一行 (超宽由右缘裁剪)
        if (block.options.empty()) {
            rows.push(
                text(fmt::format("[control `{}` has no options]", id)) | color(theme.errorColor)
                | dim
            );
        } else {
            for (size_t i = 0; i < block.options.size(); ++i) {
                const bool active = state.selected == static_cast<int>(i);
                auto       label  = resolveLabel(block.options[i].labelKey, block.options[i].label);
                if (label.empty()) {
                    label = valueText(block.options[i].value);
                }
                Element entry = text(fmt::format(" {} {}", active ? "▸" : " ", label));
                if (active) {
                    entry = entry | bgcolor(theme.buttonActiveBgColor)
                            | color(theme.buttonActiveTextColor) | bold;
                } else {
                    entry = entry | color(theme.buttonTextColor);
                }
                auto box = registerHits ? std::make_shared<Box>() : nullptr;
                if (box) {
                    entry = entry | reflect(*box);
                    hit(msgIndex, blockIndex, id, static_cast<int>(i), box);
                }
                rows.push(std::move(entry) | xflex_shrink);
            }
        }
    } else if (block.control == "checkbox") {
        // 勾选项: 整行可点 (左指示器 + 标签)
        auto       box       = registerHits ? std::make_shared<Box>() : nullptr;
        const auto indicator = state.checked ? text("[ ✓ ] ") | color(theme.accentColor) | bold
                                             : text("[   ] ") | color(theme.hintColor);
        auto       label     = resolveLabel(block.labelKey, block.label);
        if (label.empty()) {
            label = id;
        }
        // 标签用普通内容色: 按钮文字色在深色主题下为纯黑, 无背景时会不可见
        Element row = hbox({std::move(indicator), text(label) | color(theme.normalColor)});
        if (box) {
            row = row | reflect(*box);
            hit(msgIndex, blockIndex, id, 0, box);
        }
        rows.push(std::move(row));
    } else if (block.control == "number") {
        // 数值控件: [ - ] 输入 [ + ] (三个命中: 减/输入框/加)
        auto minusBox = registerHits ? std::make_shared<Box>() : nullptr;
        auto plusBox  = registerHits ? std::make_shared<Box>() : nullptr;
        auto editBox  = registerHits ? std::make_shared<Box>() : nullptr;
        if (registerHits) {
            hit(msgIndex, blockIndex, id, kSubNumMinus, minusBox);
            hit(msgIndex, blockIndex, id, kSubNumPlus, plusBox);
            hit(msgIndex, blockIndex, id, kSubNumEdit, editBox);
        }
        auto btnStyle = [&theme](std::string_view label) {
            return text(label) | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
        };
        Element minusEl = btnStyle("[ - ]");
        Element plusEl  = btnStyle("[ + ]");
        Element editEl  = text(" " + state.editText + " ") | bgcolor(theme.inputBgColor)
                         | color(theme.inputTextColor) | xflex_shrink;
        if (minusBox) {
            minusEl = minusEl | reflect(*minusBox);
        }
        if (plusBox) {
            plusEl = plusEl | reflect(*plusBox);
        }
        if (editBox) {
            editEl = editEl | reflect(*editBox);
        }
        rows.push(hbox({
            std::move(minusEl),
            text(" "),
            std::move(editEl),
            text(" "),
            std::move(plusEl),
        }));
    } else if (block.control == "text") {
        // 文本输入框 (multiline 预留: 当前按单行渲染)
        auto    box  = registerHits ? std::make_shared<Box>() : nullptr;
        Element edit = text(" " + state.editText + " ") | bgcolor(theme.inputBgColor)
                       | color(theme.inputTextColor) | xflex_shrink;
        if (box) {
            edit = edit | reflect(*box);
            hit(msgIndex, blockIndex, id, 0, box);
        }
        rows.push(hbox({std::move(edit)}));
    } else {
        // 未知控件形态: 诊断行 (不可交互; 不使整份描述失效)
        rows.push(
            text(fmt::format("[unsupported control: {}]", block.control)) | color(theme.errorColor)
            | dim
        );
    }

    // 校验提示行 (紧贴控件下方; 独立一行, 不与控件行/提交行合并)
    if (!state.tip.empty()) {
        rows.push(hbox({
            text("  "),
            text(state.tip) | color(theme.errorColor) | xflex_shrink,
        }));
    }
}

void InterruptView::layoutForm(
    const TUIMessage& msg,
    size_t            msgIndex,
    const FormState*  state,
    int               width,
    bool              registerHits,
    UiRenderResult&   out
) const {
    const auto ui = resolveUi(msg);
    if (ui.empty()) {
        return;
    }

    UiRenderCtx rc;
    rc.theme  = ctx_.theme;
    rc.width  = width;
    rc.indent = 0;

    for (size_t bi = 0; bi < ui.blocks.size(); ++bi) {
        const auto& block = ui.blocks[bi];

        // ---- 内容块 (text/markdown/diff/separator/gap): 共享块渲染层 ----
        if (block.kind == "text") {
            auto resolved = block; // 解析 i18n 键 (textKey 优先, 缺键回退 text)
            resolved.text = resolveLabel(block.textKey, block.text);
            if (auto item = uiItemFromInterruptBlock(resolved)) {
                renderUiItem(*item, rc, out);
            }
            continue;
        }
        if (block.kind == "markdown" || block.kind == "diff" || block.kind == "separator"
            || block.kind == "gap") {
            if (auto item = uiItemFromInterruptBlock(block)) {
                renderUiItem(*item, rc, out);
            }
            continue;
        }

        // ---- 控件块 ----
        if (block.kind == "control") {
            const auto id = controlIdOf(block);
            // 估算路径 (state* == nullptr) 使用描述默认值构造的临时状态
            ControlState        fallback;
            const ControlState* control = nullptr;
            if (state) {
                auto it = state->controls.find(id);
                if (it != state->controls.end()) {
                    control = &it->second;
                }
            }
            if (!control) {
                initControlState(block, fallback);
                control = &fallback;
            }
            layoutControl(msgIndex, bi, block, *control, width, registerHits, out);
            continue;
        }

        // ---- 提交行 ----
        if (block.kind == "submit") {
            layoutSubmit(msgIndex, bi, block, registerHits, out);
            continue;
        }

        // ---- 自定义渲染块 (字段预留, 暂未实现) ----
        if (block.kind == "custom") {
            // TODO(自定义渲染): 客户端组件渲染器 (component + props) 注册与派发尚未
            // 实现 —— 当前仅渲染 fallback 文本 (无 fallback 时输出组件名占位诊断行);
            // 字段已随描述往返保留, 后续接入插件渲染器后在此派发。
            std::string text = block.fallback;
            if (text.empty() && !block.component.empty()) {
                text = fmt::format("[custom component: {}]", block.component);
            }
            if (!text.empty()) {
                middleware::InterruptUiBlock tb;
                tb.kind   = "text";
                tb.text   = std::move(text);
                tb.color  = "hint";
                tb.dim    = true;
                tb.wrap   = true;
                tb.indent = std::max(0, block.indent);
                if (auto item = uiItemFromInterruptBlock(tb)) {
                    renderUiItem(*item, rc, out);
                }
            }
            continue;
        }
        // 未知 kind: 忽略 (向前兼容)
    }
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

Element
    InterruptView::renderValueButton(const middleware::InterruptUiOption& opt, bool active) const {
    const auto& theme = *ctx_.theme;
    auto        label = resolveLabel(opt.labelKey, opt.label);
    if (label.empty()) {
        label = valueText(opt.value);
    }

    PluginButtonDesc desc;
    desc.label = std::move(label);
    // 选中项高亮 (强调样式); 未选中按描述色 (error = 高危操作, 其余普通按钮)
    if (active) {
        desc.role = PluginButtonRole::Accent;
    } else if (opt.color == "error" || opt.color == "danger") {
        desc.role = PluginButtonRole::Danger;
    } else {
        desc.role = PluginButtonRole::Normal;
    }
    return renderPluginButton(desc, theme);
}

Element InterruptView::buildHeader(const middleware::InterruptUi& ui) const {
    const auto& theme = *ctx_.theme;
    Elements    segs;
    if (ui.header.segments.empty()) {
        // 默认前缀 (语言随 TuiI18n)
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

Element InterruptView::buildStatusLine(const TUIMessage& msg) const {
    const auto& theme = *ctx_.theme;
    if (!msg.interrupt) {
        return text("");
    }
    const auto& it = *msg.interrupt;
    switch (it.interruptStatus) {
        case TUIMessage::InterruptStatus::Confirmed: {
            // 无控件值的表单 (仅确认行) 提交后无结果值: 用无占位符文案
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

void InterruptView::hit(
    size_t                             msgIndex,
    size_t                             blockIndex,
    std::string                        controlId,
    int                                sub,
    const std::shared_ptr<ftxui::Box>& box
) const {
    HitBox h;
    h.msgIndex   = msgIndex;
    h.blockIndex = blockIndex;
    h.controlId  = std::move(controlId);
    h.sub        = sub;
    h.box        = box;
    hits_.push_back(std::move(h));
}

Element InterruptView::build(
    const TUIMessage&                                   msg,
    size_t                                              msgIndex,
    int                                                 maxWidth,
    std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
) {
    if (!msg.interrupt) {
        return text("");
    }
    if (!isWaiting(msg)) {
        return buildStatusLine(msg);
    }

    const auto& theme = *ctx_.theme;
    const auto  ui    = resolveUi(msg);
    if (ui.empty()) {
        // 描述缺失/非法 (契约错误): 仅输出诊断行, 不渲染任何控件 (无命中区域)
        XX_LOGE(
            "[tui] interrupt #{} has no UI descriptor, prompt not interactive",
            msg.interrupt->interruptId
        );
        return hbox({text(tr("interrupt.noDescriptor")) | color(theme.errorColor) | bold});
    }

    // 表单状态 (惰性初始化: 按描述默认值填充各控件)
    auto&          state = uiStateFor(msg);
    UiRenderResult layout;
    layoutForm(msg, msgIndex, &state, maxWidth, true, layout);
    // markdown 渲染器生命周期交回调用方 (Element 内部容器/链接 Box 指向它)
    for (auto& builder : layout.builders) {
        mdBuilders.push_back(std::move(builder));
    }

    Elements rows;
    rows.push_back(buildHeader(ui));
    for (auto& row : layout.rows) {
        rows.push_back(std::move(row.element));
    }
    return vbox(std::move(rows));
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

    // 与 build 同一套布局过程 (registerHits=false; 不创建命中区域)
    UiRenderResult layout;
    layoutForm(msg, 0, stateOf(msg), width, false, layout);
    size_t lines = 1; // 头行
    for (const auto& row : layout.rows) {
        lines += std::max<size_t>(1, row.lines);
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

    // 命中检测 (命中区域为上一帧布局结果)
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
        const size_t mi      = h.msgIndex;
        const size_t blockIx = h.blockIndex;
        Act          act     = Act::None;
        std::string  stepId;
        double       stepAmount = 1.0;

        ctx_.state->mutate([&](TUIRenderState& st) {
            if (mi >= st.messages.size()) {
                return;
            }
            const auto& msg = *st.messages[mi];
            if (!isWaiting(msg)) {
                return;
            }
            const auto ui = resolveUi(msg);
            if (blockIx >= ui.blocks.size()) {
                return;
            }
            const auto& block = ui.blocks[blockIx];

            // 提交行 (按块下标定位)
            if (block.kind == "submit") {
                act = (h.sub == kSubSubmitCancel) ? Act::Cancel : Act::Confirm;
                return;
            }
            if (block.kind != "control") {
                return; // 内容块不可交互
            }

            const auto id        = controlIdOf(block);
            auto&      state     = mutateUiState(msg);
            state.focusedId      = id;
            auto [cit, inserted] = state.controls.try_emplace(id);
            auto& cs             = cit->second;
            if (inserted) {
                initControlState(block, cs);
            }

            if (block.control == "buttons") {
                const int n = static_cast<int>(block.options.size());
                if (n <= 0) {
                    return;
                }
                // 值按钮: 选中 (commitOnPick 时立即提交整份表单, 一问一答形态)
                cs.selected = std::clamp(h.sub, 0, n - 1);
                cs.tip.clear();
                act = block.commitOnPick ? Act::Confirm : Act::StateChanged;
                return;
            }
            if (block.control == "select") {
                cs.selected
                    = std::clamp(h.sub, 0, std::max(0, static_cast<int>(block.options.size()) - 1));
                cs.tip.clear();
                act = Act::StateChanged;
                return;
            }
            if (block.control == "checkbox") {
                cs.checked = !cs.checked;
                cs.tip.clear();
                act = Act::StateChanged;
                return;
            }
            if (block.control == "number") {
                stepId     = id;
                stepAmount = (block.step > 0) ? block.step : 1.0;
                if (h.sub == kSubNumMinus) {
                    act = Act::StepDown;
                } else if (h.sub == kSubNumPlus) {
                    act = Act::StepUp;
                } else {
                    act = Act::StateChanged; // 输入框: 仅激活
                }
                return;
            }
            // text: 仅激活 (键盘输入作用于聚焦控件)
            act = Act::StateChanged;
        });

        if (act == Act::None) {
            return false; // 未处理 (消息已结束/非交互块)
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
                // 步进量按描述声明 (默认 1; 上面命中分支已解析)
                const double delta = stepAmount * ((act == Act::StepUp) ? 1.0 : -1.0);
                step(mi, stepId, delta);
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
    Act         act        = Act::None;
    bool        stillValid = false;
    std::string stepId;
    double      stepAmount = 1.0;

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

        const auto ui = resolveUi(msg);
        // 表单状态 (惰性初始化) + 聚焦控件定位
        auto&                               state = mutateUiState(msg);
        const middleware::InterruptUiBlock* block = nullptr;
        if (!state.focusedId.empty()) {
            for (const auto& b : ui.blocks) {
                if (b.kind == "control" && controlIdOf(b) == state.focusedId) {
                    block = &b;
                    break;
                }
            }
        }
        if (!block) {
            // 聚焦控件缺失 (空表单/描述变化): 回退到第一个控件并更新焦点
            for (const auto& b : ui.blocks) {
                if (b.kind == "control") {
                    block           = &b;
                    state.focusedId = controlIdOf(b);
                    break;
                }
            }
        }
        if (!block) {
            return;
        }

        const auto id        = controlIdOf(*block);
        state.focusedId      = id;
        auto [cit, inserted] = state.controls.try_emplace(id);
        auto& cs             = cit->second;
        if (inserted) {
            initControlState(*block, cs);
        }

        if (block->control == "buttons") {
            // 左右切换选中项 (提交由 Enter/点击完成)
            if (event == Event::ArrowLeft || event == Event::ArrowRight) {
                const int n = static_cast<int>(block->options.size());
                if (n > 0) {
                    const int dir = (event == Event::ArrowRight) ? 1 : n - 1;
                    cs.selected   = (cs.selected + dir) % n;
                }
                act = Act::Handled;
            }
            return;
        }
        if (block->control == "select") {
            const int delta = (event == Event::ArrowUp) ? -1 : (event == Event::ArrowDown) ? 1 : 0;
            if (delta != 0) {
                const int n = static_cast<int>(block->options.size());
                cs.selected = std::clamp(cs.selected + delta, 0, std::max(0, n - 1));
                cs.tip.clear();
                act = Act::Handled;
            }
            return;
        }
        if (block->control == "checkbox") {
            if (event.is_character() && event.character() == " ") {
                cs.checked = !cs.checked;
                act        = Act::Handled;
            }
            return;
        }
        if (block->control == "number" && (event == Event::ArrowUp || event == Event::ArrowDown)) {
            stepId     = id;
            stepAmount = (block->step > 0) ? block->step : 1.0;
            act        = (event == Event::ArrowUp) ? Act::StepUp : Act::StepDown;
            return;
        }
        // text / number: 字符编辑
        if (event.is_character() || event == Event::Backspace || event == Event::Delete) {
            if (event.is_character()) {
                if (!cs.edited) {
                    // 首次输入替换默认值 (与输入框激活时保留默认值的语义一致)
                    cs.editText.clear();
                    cs.edited = true;
                }
                cs.editText += event.character();
            } else if (!cs.editText.empty()) {
                cs.editText.pop_back();
                cs.edited = true;
            }
            cs.tip.clear();
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
            const double delta = stepAmount * ((act == Act::StepUp) ? 1.0 : -1.0);
            const auto   state = formState(mi);
            step(mi, state.focusedId.empty() ? stepId : state.focusedId, delta);
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
    bool                confirmed  = false;
    bool                needRedraw = false;
    int64_t             wireId     = 0;
    std::string         display;

    ctx_.state->mutate([&](TUIRenderState& st) {
        if (msgIndex >= st.messages.size()) {
            return;
        }
        const auto& src = *st.messages[msgIndex];
        if (!isWaiting(src)) {
            return;
        }
        const auto ui = resolveUi(src);
        auto& state = mutateUiState(src); // 校验提示/提交结果影响渲染 → 递增版本

        agentxx::util::Json values = agentxx::util::Json::object();
        for (const auto& block : ui.blocks) {
            if (block.kind != "control") {
                continue;
            }
            const auto id        = controlIdOf(block);
            auto [cit, inserted] = state.controls.try_emplace(id);
            auto& cs             = cit->second;
            if (inserted) {
                initControlState(block, cs);
            }

            agentxx::util::Json value;
            if (block.control == "buttons" || block.control == "select") {
                const int n = static_cast<int>(block.options.size());
                if (n <= 0) {
                    cs.tip     = std::string{tr("interrupt.tipNoOptions")};
                    needRedraw = true;
                    return; // 校验失败: 不提交
                }
                cs.selected = std::clamp(cs.selected, 0, n - 1);
                value       = block.options[static_cast<size_t>(cs.selected)].value;
            } else if (block.control == "checkbox") {
                value = cs.checked;
            } else if (block.control == "number") {
                // 数值校验: 可解析 + integer 约束 + min/max 范围
                std::string errTip;
                double      num     = 0.0;
                auto        trimmed = agentxx::util::removeBetweenSpace(cs.editText);
                if (trimmed.empty()
                    || agentxx::util::parseNumberFromString(trimmed, num).ec != std::errc{}) {
                    errTip
                        = std::string{tr(block.integer ? "interrupt.tipInt" : "interrupt.tipNum")};
                } else if (block.integer && num != std::trunc(num)) {
                    errTip = std::string{tr("interrupt.tipInt")};
                } else if (block.hasMin && num < block.minValue) {
                    errTip = trf("interrupt.tipRange", formatNumber(block.minValue, block.integer));
                } else if (block.hasMax && num > block.maxValue) {
                    errTip = trf("interrupt.tipRange", formatNumber(block.maxValue, block.integer));
                }
                if (!errTip.empty()) {
                    cs.tip     = std::move(errTip);
                    needRedraw = true;
                    return; // 校验失败: 提示保留在该控件下方
                }
                // 注意: 必须用括号构造 —— 单元素花括号会命中 initializer_list
                // 构造, 生成单元素数组而非数值
                value = block.integer ? agentxx::util::Json(static_cast<int64_t>(num))
                                      : agentxx::util::Json(num);
            } else if (block.control == "text") {
                value = cs.editText;
            } else {
                // 未知控件形态: 不参与结果 (诊断行已提示)
                continue;
            }

            cs.tip     = {};
            values[id] = std::move(value);
        }

        // 提交结果展示文本 (状态行): 各控件结果值拼接 (标签: 值)
        for (const auto& block : ui.blocks) {
            if (block.kind != "control") {
                continue;
            }
            const auto id = controlIdOf(block);
            if (!values.contains(id)) {
                continue;
            }
            auto label = resolveLabel(block.labelKey, block.label);
            if (label.empty()) {
                label = id;
            }
            if (!display.empty()) {
                display += ", ";
            }
            display += fmt::format("{}: {}", label, valueText(values[id]));
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
    } else if (needRedraw) {
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
    submit.values    = agentxx::util::Json::object();
    sendSubmit(wireId, submit);
    activeMsg_ = static_cast<size_t>(-1);
    ctx_.postRedraw();
}

void InterruptView::step(size_t msgIndex, std::string_view controlId, double delta) {
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (msgIndex >= st.messages.size()) {
            return;
        }
        const auto& src = *st.messages[msgIndex];
        if (!isWaiting(src)) {
            return;
        }
        const auto                          ui    = resolveUi(src);
        const middleware::InterruptUiBlock* block = nullptr;
        for (const auto& b : ui.blocks) {
            if (b.kind == "control" && controlIdOf(b) == controlId) {
                block = &b;
                break;
            }
        }
        if (!block || block->control != "number") {
            return;
        }
        auto& state          = mutateUiState(src);
        auto [cit, inserted] = state.controls.try_emplace(std::string{controlId});
        auto& cs             = cit->second;
        if (inserted) {
            initControlState(*block, cs);
        }
        double val     = 0.0;
        auto   trimmed = agentxx::util::removeBetweenSpace(cs.editText);
        if (trimmed.empty()
            || agentxx::util::parseNumberFromString(trimmed, val).ec != std::errc{}) {
            return; // 编辑值非法时步进无效
        }
        val += delta;
        // 整数控件按整数步进; 浮点控件保留有效精度
        cs.editText = formatNumber(val, block->integer);
        cs.edited   = true;
        cs.tip.clear();
    });
    ctx_.postRedraw();
}

} // namespace client
} // namespace agentxx
