#include "agentxx-client/io/tui/components/interrupt_view.h"

#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/markdown_block.h"
#include "fmt/format.h"
#include "utilxx_base/log.h"
#include <algorithm>
#include <utility>

using namespace ftxui;

namespace agentxx {
namespace client {

namespace {

using utilxx_base::Json;

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

std::vector<InterruptView::FormItem> InterruptView::formItems(const middleware::InterruptUi& ui) const {
    std::vector<FormItem> out;
    for (size_t bi = 0; bi < ui.blocks.size(); ++bi) {
        const auto& block = ui.blocks[bi];
        if (block.kind == "control") {
            // 标签/说明的 i18n 键在此解析 (共享渲染层只认字面文本)
            auto resolved  = block;
            resolved.id    = controlIdOf(block);
            resolved.label = resolveLabel(block.labelKey, block.label);
            resolved.help  = resolveLabel(block.helpKey, block.help);
            for (auto& opt : resolved.options) {
                opt.label = resolveLabel(opt.labelKey, opt.label);
            }
            if (auto item = itemFromInterruptBlock(resolved)) {
                out.push_back(FormItem{bi, std::move(*item)});
            }
            continue;
        }
        if (block.kind == "submit") {
            auto item = itemFromInterruptBlock(block);
            if (!item) {
                continue;
            }
            // 中断提交行的缺省文案与插件表单不同 (确认 / ✕)
            if (item->label.empty()) {
                item->label = std::string{tr("interrupt.confirm")};
            }
            if (item->cancelLabel.empty()) {
                item->cancelLabel = std::string{tr("interrupt.cancel")};
            }
            out.push_back(FormItem{bi, std::move(*item)});
            continue;
        }
    }
    return out;
}

std::vector<agentxx::ui::Item>
    InterruptView::plainItems(const std::vector<FormItem>& items) {
    std::vector<agentxx::ui::Item> out;
    out.reserve(items.size());
    for (const auto& item : items) {
        out.push_back(item.item);
    }
    return out;
}

std::string InterruptView::firstControlId(const std::vector<agentxx::ui::Item>& items) {
    for (const auto& item : items) {
        if (item.kind == "control" && !item.id.empty()) {
            return item.id;
        }
    }
    return {};
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
    // 惰性初始化: 按描述填充各控件状态 (key = 控件 id), 首个控件为键盘焦点
    auto&      state = it->second;
    const auto items = plainItems(formItems(resolveUi(msg)));
    initFormState(state, items);
    state.focusedId = firstControlId(items);
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

void InterruptView::registerBlockHits(
    size_t                msgIndex,
    size_t                blockIndex,
    const UiRenderResult& out,
    size_t                firstRow
) const {
    for (size_t r = firstRow; r < out.rows.size(); ++r) {
        const auto& row = out.rows[r];
        if (!row.box || row.regions.empty()) {
            continue;
        }
        for (const auto& region : row.regions) {
            // 只登记控件区域与提交行区域: 描述里的普通动作区域 (扩展组件内的按钮)
            // 在中断里没有结果去处, 不参与命中
            if (region.kind != UiHitRegionKind::Form
                && region.kind != UiHitRegionKind::FormSubmit) {
                continue;
            }
            const bool isSubmit = (region.kind == UiHitRegionKind::FormSubmit);
            const bool cancel   = isSubmit && (std::string_view{region.id} == kFormCancelActionId);
            hit(
                msgIndex,
                blockIndex,
                isSubmit ? std::string{kItemIdSubmit} : region.id,
                isSubmit ? (cancel ? kSubSubmitCancel : kSubSubmitConfirm) : region.sub,
                row.box,
                region
            );
        }
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

    // 控件块与提交行 → 共享组件项 (标签/说明的 i18n 键在此解析)
    const auto items = formItems(ui);

    UiRenderCtx rc;
    rc.theme  = ctx_.theme;
    rc.width  = width;
    rc.indent = 0;
    rc.form   = state; // 估算路径 (state == nullptr) 按描述缺省值渲染静态形态

    auto renderBlockItem = [&](const FormItem& formItem) {
        const size_t firstRow = out.rows.size();
        renderItem(formItem.item, rc, out);
        if (registerHits) {
            registerBlockHits(msgIndex, formItem.blockIndex, out, firstRow);
        }
    };

    size_t nextItem = 0;
    for (size_t bi = 0; bi < ui.blocks.size(); ++bi) {
        const auto& block = ui.blocks[bi];

        // ---- 控件块与提交行: 共享组件渲染 (控件行/标签/校验提示与插件表单同实现) ----
        if (block.kind == "control" || block.kind == "submit") {
            if (nextItem < items.size() && items[nextItem].blockIndex == bi) {
                renderBlockItem(items[nextItem]);
                ++nextItem;
            }
            continue;
        }

        // ---- 文本块: 解析 i18n 键后走共享组件渲染 ----
        if (block.kind == "text") {
            auto resolved = block; // (textKey 优先, 缺键回退 text)
            resolved.text = resolveLabel(block.textKey, block.text);
            if (auto item = itemFromInterruptBlock(resolved)) {
                renderItem(*item, rc, out);
            }
            continue;
        }

        // ---- 其余块: 走共享组件渲染 ----
        // 覆盖 markdown/diff/separator/gap 与扩展组件 (表格/树/横排/分组/趋势图等,
        // 按块描述直接使用 `agentxx.ui.item` schema), 以及 custom 块派发;
        // 未识别的块降级为 fallback 文本 (无 fallback 则跳过, 向前兼容)。
        if (auto item = itemFromInterruptBlock(block)) {
            if (item->kind != "control" && item->kind != "submit") {
                renderItem(*item, rc, out);
                continue;
            }
        }
        if (!block.fallback.empty()) {
            middleware::InterruptUiBlock tb;
            tb.kind   = "text";
            tb.text   = block.fallback;
            tb.color  = "hint";
            tb.dim    = true;
            tb.wrap   = true;
            tb.indent = std::max(0, block.indent);
            if (auto item = itemFromInterruptBlock(tb)) {
                renderItem(*item, rc, out);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

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
                el = el | theme.dim();
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
                text(confirmedText) | color(theme.accentColor) | theme.dim() | xflex_shrink,
            });
        }
        case TUIMessage::InterruptStatus::Cancelled:
            return hbox({
                text(tr("interrupt.header")) | color(theme.hintColor),
                text(tr("interrupt.cancelled")) | color(theme.errorColor) | theme.dim()
                    | xflex_shrink,
            });
        case TUIMessage::InterruptStatus::Expired:
            return hbox({
                text(tr("interrupt.header")) | color(theme.hintColor),
                text(tr("interrupt.expired")) | color(theme.errorColor) | theme.dim()
                    | xflex_shrink,
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
    const std::shared_ptr<ftxui::Box>& box,
    const UiHitRegion&                 region
) const {
    HitBox h;
    h.msgIndex   = msgIndex;
    h.blockIndex = blockIndex;
    h.controlId  = std::move(controlId);
    h.sub        = sub;
    h.box        = box;
    h.region     = region;
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
    it->second->async_send(utilxx_base::AsioErrorCode{}, submit, [](utilxx_base::AsioErrorCode) {});
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

    // 命中检测 (命中区域为上一帧布局结果): 行元素框 + 行内区域两级判定
    for (const auto& h : hits_) {
        if (!h.box || h.box->IsEmpty()) {
            continue;
        }
        if (!h.box->Contain(mouse.x, mouse.y)) {
            continue;
        }
        if (!h.region.contains(mouse.x - h.box->x_min, mouse.y - h.box->y_min)) {
            continue;
        }

        // 命中后的动作在锁外执行 (confirm/cancel 内部再次加锁, 避免同锁重入)
        enum class Act : uint8_t {
            None,
            Confirm,
            Cancel,
            StateChanged,
        };
        const size_t mi      = h.msgIndex;
        const size_t blockIx = h.blockIndex;
        Act          act     = Act::None;

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
            if (block.kind != "control" && block.kind != "submit") {
                return; // 内容块不可交互
            }

            // 提交行 (按块下标定位): 确认 / 取消
            if (block.kind == "submit") {
                act = (h.sub == kSubSubmitCancel) ? Act::Cancel : Act::Confirm;
                return;
            }

            // 控件命中: 语义 (选中/翻转/步进/聚焦) 由共享表单层处理
            auto&             state  = uiStateFor(msg);
            const auto        items  = plainItems(formItems(ui));
            const UiFormAction action = handleFormControlHit(items, state, h.controlId, h.sub);
            if (action == UiFormAction::None) {
                return;
            }
            ++state.version; // 状态变化 → 消息列表缓存失效与高度重估
            act = (action == UiFormAction::Submit) ? Act::Confirm : Act::StateChanged;
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
        Escape,
        Handled,
    };
    Act  act        = Act::None;
    bool stillValid = false;

    // 校验激活消息仍可交互; 按键作用于当前聚焦控件 (状态修改在锁内完成)
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (mi >= st.messages.size()) {
            return;
        }
        const auto& msg = *st.messages[mi];
        if (!isWaiting(msg)) {
            return;
        }
        stillValid = true;

        // 回车提交整份表单; Esc 释放激活状态 (中断表单的"失焦"语义)
        if (event == Event::Escape) {
            act = Act::Escape;
            return;
        }
        if (event == Event::Return) {
            act = Act::Confirm;
            return;
        }

        const auto items = plainItems(formItems(resolveUi(msg)));
        if (items.empty()) {
            return; // 无控件: 键盘不参与 (回车/Esc 已在上方处理)
        }
        auto& state = uiStateFor(msg);
        // 描述变化时补齐新声明的控件 (已初始化的控件保留用户编辑的值)
        initFormState(state, items);
        // 无焦点或焦点已失效 (控件被描述移除): 回到首个控件
        // (中断表单始终有一个焦点控件, 键盘输入总有去处)
        const bool focusValid
            = !state.focusedId.empty()
              && std::any_of(items.begin(), items.end(), [&](const agentxx::ui::Item& it) {
                     return it.kind == "control" && it.id == state.focusedId;
                 });
        if (!focusValid) {
            state.focusedId = firstControlId(items);
        }
        // 控件语义 (选中/翻转/步进/文本编辑/焦点移动) 由共享表单层处理
        if (handleFormKeyInput(items, state, event)) {
            ++state.version; // 状态变化 → 消息列表缓存失效与高度重估
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
        const auto        ui    = resolveUi(src);
        const auto        items = plainItems(formItems(ui));
        auto&             state = mutateUiState(src); // 校验提示/提交结果影响渲染 → 递增版本

        // 校验 (数值范围/整数约束/候选项缺失) 与取值都走共享表单层
        if (!validateForm(items, state)) {
            needRedraw = true;
            return; // 校验失败: 提示保留在各控件下方, 不提交
        }
        auto formState = formValues(items, state);
        utilxx_base::Json values
            = (formState.is_object() && formState.contains("values") && formState["values"].is_object())
                  ? formState["values"]
                  : utilxx_base::Json::object();

        // 提交结果展示文本 (状态行): 各控件结果值拼接 (标签: 值)
        for (const auto& block : ui.blocks) {
            if (block.kind != "control") {
                continue;
            }
            const auto id = controlIdOf(block);
            if (!values.contains(id)) {
                continue; // 未知控件形态不参与结果 (诊断行已提示)
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
    submit.values    = utilxx_base::Json::object();
    sendSubmit(wireId, submit);
    activeMsg_ = static_cast<size_t>(-1);
    ctx_.postRedraw();
}

} // namespace client
} // namespace agentxx
