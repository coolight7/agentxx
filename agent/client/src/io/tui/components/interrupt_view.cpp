#include "agentxx-client/io/tui/components/interrupt_view.h"

#include "agentxx-client/io/tui/framework/owned_reflect.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/framework/ui_hit.h"
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

/// 摘要行分隔符 (分段 / 补充说明 / 处理结果三段之间的间隔符号)
constexpr std::string_view kSummarySeparator = " · ";
/// 分隔符占用的终端列数
constexpr int kSeparatorColumns = 3;
/// 补充说明的最少列数 (不足时分段与结果之外的地方不画)
constexpr int kMinDetailColumns = 8;

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

/// 文本终端显示列宽 (宽字符按 2 列)
int columnsOf(std::string_view s) {
    return static_cast<int>(markdown::utf8_display_width(s));
}

bool endsWithSpace(std::string_view s) {
    return !s.empty() && (s.back() == ' ' || s.back() == '\t');
}

bool startsWithSpace(std::string_view s) {
    return !s.empty() && (s.front() == ' ' || s.front() == '\t');
}

/// 单行截断: 只取首个换行前的内容, 按终端显示列宽截断 (宽字符按 2 列, 不切码点)
/// - 超出 [maxCols] 时以 "..." 收尾, 结果总宽不超过 [maxCols]
/// - 放不下省略号时返回空串 (调用方自行省略该段)
std::string oneLineWithin(std::string_view s, int maxCols) {
    if (maxCols <= 0 || s.empty()) {
        return {};
    }
    const auto        nl = s.find('\n');
    std::string_view  line = (nl == std::string_view::npos) ? s : s.substr(0, nl);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    if (line.empty()) {
        return {};
    }
    if (columnsOf(line) <= maxCols) {
        return std::string{line};
    }
    // 预留省略号 3 列后逐码点累积, 超过预算即停 (不切断码点)
    const int budget = maxCols - 3;
    if (budget <= 0) {
        return {};
    }
    std::string out;
    int         cols = 0;
    for (size_t i = 0; i < line.size();) {
        size_t len = markdown::utf8_byte_length(line[i]);
        len        = std::min(len, line.size() - i);
        const int w
            = markdown::codepoint_width(markdown::utf8_codepoint(line.data() + i, len));
        if (cols + w > budget) {
            break;
        }
        out.append(line.substr(i, len));
        cols += w;
        i    += len;
    }
    if (out.empty()) {
        return {}; // 首个字符就放不下: 宁可不画也不画半截
    }
    out += "...";
    return out;
}

} // namespace

InterruptView::InterruptView(TUICtx& ctx) :
    ctx_(ctx) {}

void InterruptView::setToolDisplayNameResolver(ToolDisplayNameFn fn) {
    toolDisplayName_ = std::move(fn);
}

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
        if (msgIndex < st.messages().size() && isWaiting(*st.messages()[msgIndex])) {
            uiStateFor(*st.messages()[msgIndex]);
        }
    });
    ctx_.postRedraw();
}

void InterruptView::clearActive() {
    activeMsg_ = static_cast<size_t>(-1);
}

void InterruptView::onMessagesTrimmedHead(size_t count) {
    if (count == 0) {
        return;
    }
    // 激活消息 (键盘作用对象) 下标前移; 落在被删区时清除激活
    if (activeMsg_ != static_cast<size_t>(-1)) {
        activeMsg_ = (activeMsg_ >= count) ? (activeMsg_ - count) : static_cast<size_t>(-1);
    }
    // 上一帧登记的命中区下标同批前移: 命中框由下一次渲染重建, 在这之前收到的
    // 点击仍应按新下标作用到同一条消息; 指向被删消息的命中区直接丢弃
    std::vector<HitBox> kept;
    kept.reserve(hits_.size());
    for (auto& hit : hits_) {
        if (hit.msgIndex < count) {
            continue;
        }
        hit.msgIndex -= count;
        kept.push_back(std::move(hit));
    }
    hits_.swap(kept);
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

std::vector<InterruptView::FormItem> InterruptView::formItems(const middleware::InterruptUi& ui
) const {
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
            // 提交行: 描述层把它映射成"确认 / 取消"两个按钮 (动作 id 是域内提交/取消
            // 约定 __submit / __cancel), 文案键由客户端词表解析, 提交动作由中断表单执行
            if (auto item = itemFromInterruptBlock(block)) {
                out.push_back(FormItem{bi, std::move(*item)});
            }
            continue;
        }
    }
    return out;
}

std::vector<pluginxx::ui::Item> InterruptView::plainItems(const std::vector<FormItem>& items) {
    std::vector<pluginxx::ui::Item> out;
    out.reserve(items.size());
    for (const auto& item : items) {
        out.push_back(item.item);
    }
    return out;
}

std::string InterruptView::firstControlId(const std::vector<pluginxx::ui::Item>& items) {
    // 注意: 这里比的是**组件项**的 kind ("Control"), 域内块词汇的 "control" 只用于
    // InterruptUiBlock (见 formItems)
    for (const auto& item : items) {
        if (item.kind == "Control" && !item.id.empty()) {
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
    if (!st || msgIndex >= st->messages().size()) {
        return {};
    }
    const auto& msg    = *st->messages()[msgIndex];
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
            hit(msgIndex,
                blockIndex,
                isSubmit ? std::string{kItemIdSubmit} : region.id,
                isSubmit ? (cancel ? kSubSubmitCancel : kSubSubmitConfirm) : region.sub,
                row.box,
                region);
        }
    }
}

void InterruptView::layoutForm(
    const TUIMessage& msg,
    size_t            msgIndex,
    const FormState*  state,
    int               width,
    bool              registerHits,
    UiRenderResult&   out,
    bool              contentOnly
) const {
    const auto ui = resolveUi(msg);
    if (ui.empty()) {
        return;
    }

    // 控件块与提交行 → 共享组件项 (标签/说明的 i18n 键在此解析)
    // - 已处理展开态只看内容块: 控件值已体现在结果文本里, 不再画可点控件
    const auto items = contentOnly ? std::vector<FormItem>{} : formItems(ui);

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
            if (contentOnly) {
                continue;
            }
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
            if (item->kind != "Control") {
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
// 形态与分段
// ---------------------------------------------------------------------------

InterruptView::Mode InterruptView::modeOf(const TUIMessage& msg) {
    if (isWaiting(msg)) {
        // 未处理恒展开: 不读 msg.collapsed, 从根上保证"待办条目不可能被折叠"
        return Mode::Waiting;
    }
    return msg.collapsed ? Mode::Collapsed : Mode::Expanded;
}

std::vector<InterruptView::ResolvedSegment> InterruptView::resolveSegments(
    const std::vector<middleware::InterruptUiSegment>& segments
) const {
    std::vector<ResolvedSegment> out;
    out.reserve(segments.size());
    for (const auto& seg : segments) {
        std::string label = resolveLabel(seg.labelKey, seg.text);
        // 工具名分段: 换成该工具的特化显示名 (如 agentxx_filesystem_write -> Write)。
        // 显示名未就绪时本帧**不画该段** —— 先画原始工具名会在下一帧跳变成显示名
        if (seg.kind == "tool" && toolDisplayName_) {
            label = toolDisplayName_(label);
        }
        if (label.empty()) {
            continue;
        }
        // 相邻两段之间补一个空格: 生产方只给各自的值, 不预埋前导/尾随空格
        // (键解析会丢掉字面文本里的空格, 两端都预留又会与英文段落重复)
        if (!out.empty() && !endsWithSpace(out.back().text) && !startsWithSpace(label)) {
            label.insert(label.begin(), ' ');
        }
        out.push_back(ResolvedSegment{std::move(label), seg.color, seg.bold, seg.dim});
    }
    return out;
}

std::string InterruptView::joinSegments(const std::vector<ResolvedSegment>& segments) {
    std::string out;
    for (const auto& seg : segments) {
        out += seg.text;
    }
    return out;
}

Elements InterruptView::segElements(const std::vector<ResolvedSegment>& segments) const {
    const auto& theme = *ctx_.theme;
    Elements    out;
    out.reserve(segments.size());
    for (const auto& seg : segments) {
        Element el = text(seg.text) | color(client::uiRoleColor(seg.color, theme));
        if (seg.bold) {
            el = el | bold;
        }
        if (seg.dim) {
            el = el | theme.dim();
        }
        out.push_back(std::move(el));
    }
    return out;
}

Element InterruptView::buildMark(Mode mode) const {
    const auto& theme = *ctx_.theme;
    switch (mode) {
        case Mode::Waiting:
            // 未处理: 提醒色 + 加粗 (与其它角色的折叠标记同列同时长)
            return text("! ") | color(theme.errorColor) | bold;
        case Mode::Collapsed:
            return text("+ ") | color(theme.hintColor);
        case Mode::Expanded:
        default:
            return text("- ") | color(theme.hintColor);
    }
}

std::vector<InterruptView::ResolvedSegment> InterruptView::segmentsOrDefault(
    const std::vector<middleware::InterruptUiSegment>& segments
) const {
    auto segs = resolveSegments(segments);
    if (segments.empty()) {
        // 描述未声明分段: 用客户端默认前缀 (语言随 TuiI18n)
        segs.push_back(ResolvedSegment{std::string{tr("interrupt.header")}, "accent", true, false});
    }
    return segs;
}

Elements InterruptView::buildHeaderSegments(const middleware::InterruptUi& ui) const {
    return segElements(segmentsOrDefault(ui.header.segments));
}

std::string InterruptView::statusTextOf(const TUIMessage& msg) const {
    if (!msg.interrupt) {
        return {};
    }
    const auto& it = *msg.interrupt;
    switch (it.interruptStatus) {
        case TUIMessage::InterruptStatus::Confirmed:
            // 无控件值的表单 (仅确认行) 提交后无结果值: 用无占位符文案
            return it.interruptResult.empty() ? std::string{tr("interrupt.confirmedEmpty")}
                                              : trf("interrupt.confirmed", it.interruptResult);
        case TUIMessage::InterruptStatus::Canceled:
            return std::string{tr("interrupt.Canceled")};
        case TUIMessage::InterruptStatus::Expired:
            return std::string{tr("interrupt.expired")};
        case TUIMessage::InterruptStatus::Waiting:
        default:
            return {};
    }
}

Color InterruptView::statusColorOf(const TUIMessage& msg) const {
    const auto& theme = *ctx_.theme;
    if (msg.interrupt
        && msg.interrupt->interruptStatus == TUIMessage::InterruptStatus::Confirmed) {
        return theme.accentColor;
    }
    return theme.errorColor;
}

std::string InterruptView::firstContentLine(const middleware::InterruptUi& ui) const {
    // 通用回退: 描述里首个内容块的首行 (去掉行首列表符号/标题符号与空白)
    for (const auto& block : ui.blocks) {
        std::string body;
        if (block.kind == "text") {
            body = resolveLabel(block.textKey, block.text);
        } else if (block.kind == "markdown") {
            body = block.text;
        } else {
            continue;
        }
        const auto        nl   = body.find('\n');
        std::string_view  line = (nl == std::string::npos) ? std::string_view{body}
                                                          : std::string_view{body}.substr(0, nl);
        while (!line.empty()
               && (line.front() == ' ' || line.front() == '\t' || line.front() == '-'
                   || line.front() == '*' || line.front() == '#' || line.front() == '•')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            return std::string{line};
        }
    }
    return {};
}

Element InterruptView::registerFoldHit(size_t msgIndex, Element element) const {
    // 整行判定: 区域不在行内细分 (点击头行/摘要行任意位置都算)
    auto box = std::make_shared<Box>(kNoBox);
    hit(msgIndex,
        kFoldBlockIndex,
        std::string{kItemIdFold},
        0,
        box,
        UiHitRegion{},
        HitBox::Kind::Fold);
    return std::make_shared<OwnedReflect>(std::move(element), std::move(box));
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

Element InterruptView::buildDiagnosticLine() const {
    const auto& theme = *ctx_.theme;
    return hbox({
        buildMark(Mode::Waiting),
        text(tr("interrupt.noDescriptor")) | color(theme.errorColor) | bold | xflex_shrink,
    });
}

Element InterruptView::buildSummaryLine(
    const TUIMessage&              msg,
    size_t                         msgIndex,
    const middleware::InterruptUi& ui,
    int                            maxWidth
) {
    const auto& theme = *ctx_.theme;

    // 1) 摘要分段: summary.segments 优先, 其次复用头行分段, 都没有时默认前缀
    const auto& rawSegs = ui.summary.segments.empty() ? ui.header.segments : ui.summary.segments;
    const auto  segs    = segmentsOrDefault(rawSegs);
    const std::string segText = joinSegments(segs);

    // 2) 补充说明 (目标/首行摘要): detailKey 优先, 空时回退首个内容块的首行
    std::string detail = resolveLabel(ui.summary.detailKey, ui.summary.detail);
    if (detail.empty()) {
        detail = firstContentLine(ui);
    }

    // 3) 处理结果: 折叠行就是要能直接看出结论, 与展开态同一份文案
    const std::string result = statusTextOf(msg);

    // 4) 单行拼装: 列宽预算按优先级分配
    //    - 摘要分段不裁 (它们说明"这是什么询问")
    //    - 结果保留: 先保证状态词 (已确认/已取消/已过期), 正文按剩余空间截断
    //    - detail 最后压缩, 没空间就整段省略
    const int avail     = std::max(1, maxWidth) - kMarkColumns;
    int       remaining = avail - columnsOf(segText);

    std::string resultText;
    if (!result.empty()) {
        std::string statusWord = result;
        if (const auto pos = result.find(": "); pos != std::string::npos) {
            statusWord.resize(pos);
        }
        if (remaining >= kSeparatorColumns + columnsOf(statusWord)) {
            remaining -= kSeparatorColumns;
            resultText = oneLineWithin(result, remaining);
            remaining -= columnsOf(resultText);
        }
    }

    std::string detailText;
    if (!detail.empty() && remaining >= kSeparatorColumns + kMinDetailColumns) {
        detailText = oneLineWithin(detail, remaining - kSeparatorColumns);
    }

    Elements row;
    row.push_back(buildMark(Mode::Collapsed));
    for (auto& el : segElements(segs)) {
        row.push_back(std::move(el));
    }
    if (!detailText.empty()) {
        row.push_back(text(kSummarySeparator) | color(theme.hintColor) | theme.dim());
        row.push_back(text(detailText) | color(theme.hintColor) | theme.dim());
    }
    if (!resultText.empty()) {
        row.push_back(text(kSummarySeparator) | color(theme.hintColor) | theme.dim());
        row.push_back(
            text(resultText) | color(statusColorOf(msg)) | theme.dim() | xflex_shrink
        );
    }
    return registerFoldHit(msgIndex, hbox(std::move(row)) | xflex_shrink);
}

Element InterruptView::buildProcessed(
    const TUIMessage&                                   msg,
    size_t                                              msgIndex,
    const middleware::InterruptUi&                      ui,
    int                                                 maxWidth,
    std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
) {
    const auto& theme = *ctx_.theme;

    // 头行: 标记 + 分段 + 处理结果 (同一行; 点它收起)
    const auto        segs    = segmentsOrDefault(ui.header.segments);
    const std::string segText = joinSegments(segs);
    Elements header;
    header.push_back(buildMark(Mode::Expanded));
    for (auto& el : segElements(segs)) {
        header.push_back(std::move(el));
    }
    const std::string result = statusTextOf(msg);
    // 结果文本占剩余列宽 (两个空格分隔); 剩余不足时整段省略, 不在窄终端抢头行的位置
    const int resultBudget
        = std::max(1, maxWidth) - kMarkColumns - columnsOf(segText) - 2;
    if (!result.empty() && resultBudget > 3) {
        if (const auto resultText = oneLineWithin(result, resultBudget); !resultText.empty()) {
            header.push_back(text("  ") | color(theme.hintColor) | theme.dim());
            header.push_back(
                text(resultText) | color(statusColorOf(msg)) | theme.dim() | xflex_shrink
            );
        }
    }

    // 内容块: 跳过控件与提交行 (控件值已体现在结果文本里, 不再画可点控件)
    UiRenderResult layout;
    layoutForm(msg, msgIndex, stateOf(msg), maxWidth, false, layout, /*contentOnly=*/true);
    for (auto& builder : layout.builders) {
        mdBuilders.push_back(std::move(builder));
    }

    Elements rows;
    rows.push_back(registerFoldHit(msgIndex, hbox(std::move(header))));
    for (auto& row : layout.rows) {
        rows.push_back(std::move(row.element));
    }
    return vbox(std::move(rows));
}

void InterruptView::hit(
    size_t                             msgIndex,
    size_t                             blockIndex,
    std::string                        controlId,
    int                                sub,
    const std::shared_ptr<ftxui::Box>& box,
    const UiHitRegion&                 region,
    HitBox::Kind                       kind
) const {
    HitBox h;
    h.kind       = kind;
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

    const auto ui = resolveUi(msg);
    if (ui.empty()) {
        // 描述缺失/非法 (契约错误): 仅输出诊断行, 不渲染任何控件 (无命中区域),
        // 也不给它折叠能力 (诊断行不是可处理的询问)
        if (isWaiting(msg)) {
            XX_LOGE(
                "[tui] interrupt #{} has no UI descriptor, prompt not interactive",
                msg.interrupt->interruptId
            );
        }
        return buildDiagnosticLine();
    }

    switch (modeOf(msg)) {
        case Mode::Collapsed:
            return buildSummaryLine(msg, msgIndex, ui, maxWidth);
        case Mode::Expanded:
            return buildProcessed(msg, msgIndex, ui, maxWidth, mdBuilders);
        case Mode::Waiting:
        default:
            break;
    }

    // 等待用户操作: 头行 + 描述全部内容块 + 控件 + 提交行 (与旧实现一致, 多了标记列)
    auto&          state = uiStateFor(msg);
    UiRenderResult layout;
    layoutForm(msg, msgIndex, &state, maxWidth, true, layout);
    // markdown 渲染器生命周期交回调用方 (Element 内部容器/链接 Box 指向它)
    for (auto& builder : layout.builders) {
        mdBuilders.push_back(std::move(builder));
    }

    Elements headerRow;
    headerRow.push_back(buildMark(Mode::Waiting));
    for (auto& el : buildHeaderSegments(ui)) {
        headerRow.push_back(std::move(el));
    }

    Elements rows;
    rows.push_back(hbox(std::move(headerRow)));
    for (auto& row : layout.rows) {
        rows.push_back(std::move(row.element));
    }
    return vbox(std::move(rows));
}

size_t InterruptView::estimate(const TUIMessage& msg, int width) const {
    if (!msg.interrupt) {
        return 1;
    }
    const auto ui = resolveUi(msg);
    if (ui.empty()) {
        return 1; // 描述缺失: 诊断行
    }
    const Mode mode = modeOf(msg);
    if (mode == Mode::Collapsed) {
        return 1; // 摘要行恒为一行
    }

    // 与 build 同一套布局过程 (registerHits=false; 不创建命中区域)
    UiRenderResult layout;
    layoutForm(
        msg,
        0,
        stateOf(msg),
        width,
        false,
        layout,
        /*contentOnly=*/mode == Mode::Expanded
    );
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

    // 折叠/展开**先**判定: 命中区为已处理消息的整行头行/摘要行 (不看行内区域)。
    // 未处理的消息不登记这种区域, 所以"待处理条目到底能不能被折叠"只由渲染决定
    for (const auto& h : hits_) {
        if (h.kind != HitBox::Kind::Fold) {
            continue;
        }
        if (!h.box || h.box->IsEmpty() || !h.box->Contain(mouse.x, mouse.y)) {
            continue;
        }
        const size_t mi      = h.msgIndex;
        bool         handled = false;
        ctx_.state->mutate([&](TUIRenderState& st) {
            if (mi >= st.messages().size()) {
                return;
            }
            const auto& src = *st.messages()[mi];
            if (src.role != TUIMessage::Role::Interrupt || !src.interrupt) {
                return;
            }
            if (isWaiting(src)) {
                return; // 未处理不可折叠 (渲染侧同一判定, 这里再防一次)
            }
            auto& msg     = ctx_.state->mutableMessage(st, mi);
            msg.collapsed = !msg.collapsed;
            handled       = true;
        });
        if (!handled) {
            return false;
        }
        // 折叠后 (或展开已处理消息) 没有可交互控件: 顺带释放键盘焦点
        if (activeMsg_ == mi) {
            clearActive();
        }
        ctx_.postRedraw();
        return true;
    }

    // 命中检测 (命中区域为上一帧布局结果): 行元素框 + 行内区域两级判定
    for (const auto& h : hits_) {
        if (h.kind != HitBox::Kind::Form) {
            continue;
        }
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
            if (mi >= st.messages().size()) {
                return;
            }
            const auto& msg = *st.messages()[mi];
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
            auto&      state = uiStateFor(msg);
            const auto items = plainItems(formItems(ui));
            // "点击候选项即提交整份表单"属中断层的域内约定 (描述层只表达"值变化即派发"):
            // 共享表单层按上下文回调询问该控件是否点击即提交
            UiRenderCtx pickCtx;
            pickCtx.commitOnPick = [&ui](const std::string& id) {
                for (const auto& b : ui.blocks) {
                    if (b.kind == "control" && controlIdOf(b) == id) {
                        return b.commitOnPick;
                    }
                }
                return false;
            };
            const UiFormAction action
                = handleFormControlHit(items, state, h.controlId, h.sub, pickCtx);
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
        if (mi >= st.messages().size()) {
            return;
        }
        const auto& msg = *st.messages()[mi];
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
              && std::any_of(items.begin(), items.end(), [&](const pluginxx::ui::Item& it) {
                     return it.kind == "Control" && it.id == state.focusedId;
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
        if (msgIndex >= st.messages().size()) {
            return;
        }
        const auto& src = *st.messages()[msgIndex];
        if (!isWaiting(src)) {
            return;
        }
        const auto ui    = resolveUi(src);
        const auto items = plainItems(formItems(ui));
        auto& state = mutateUiState(src); // 校验提示/提交结果影响渲染 → 递增版本

        // 校验 (数值范围/整数约束/候选项缺失) 与取值都走共享表单层
        if (!validateForm(items, state)) {
            needRedraw = true;
            return; // 校验失败: 提示保留在各控件下方, 不提交
        }
        auto              formState = formValues(items, state);
        utilxx_base::Json values    = (formState.is_object() && formState.contains("values")
                                    && formState["values"].is_object())
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
        submit.Canceled              = false;
        confirmed                     = true;
        auto& mm                      = ctx_.state->mutableMessage(st, msgIndex);
        mm.interrupt->interruptStatus = TUIMessage::InterruptStatus::Confirmed;
        mm.interrupt->interruptResult = display;
        // 处理完成即自动折叠成一行摘要 (之后用户可自行展开/收起)
        mm.collapsed = true;
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
        if (msgIndex >= st.messages().size() || !st.messages()[msgIndex]->interrupt) {
            return;
        }
        const auto& src = *st.messages()[msgIndex];
        if (!isWaiting(src)) {
            return;
        }
        wireId = src.interrupt->interruptId;
        valid  = true;
        // 标记同请求全部未操作消息为 Canceled (一条请求一份表单; 防御性遍历)
        for (size_t i = 0; i < st.messages().size(); ++i) {
            const auto& m = *st.messages()[i];
            if (m.role == TUIMessage::Role::Interrupt && m.interrupt
                && m.interrupt->interruptId == wireId
                && m.interrupt->interruptStatus == TUIMessage::InterruptStatus::Waiting) {
                auto& mm                      = ctx_.state->mutableMessage(st, i);
                mm.interrupt->interruptStatus = TUIMessage::InterruptStatus::Canceled;
                // 取消即自动折叠成一行摘要 (与提交同一个形态规则)
                mm.collapsed = true;
            }
        }
    });
    if (!valid) {
        return;
    }
    // 清理该请求的表单状态 (消息已固定状态, 编辑残留不再需要)
    states_.erase(wireId);
    InterruptFormSubmit submit;
    submit.Canceled = true;
    submit.values    = utilxx_base::Json::object();
    sendSubmit(wireId, submit);
    activeMsg_ = static_cast<size_t>(-1);
    ctx_.postRedraw();
}

} // namespace client
} // namespace agentxx
