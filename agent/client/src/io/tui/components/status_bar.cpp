#include "agentxx-client/io/tui/components/status_bar.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/ui_components.h"
#include "ftxui/dom/elements.hpp"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <vector>

namespace agentxx::client {

using namespace ftxui;

namespace {

/// 命中项查找 (命中表按 id 线性查找; 命中项数量为个位数)
const agentxx::client::UiHitMap::Entry*
    findEntry(const agentxx::client::UiHitMap& hits, std::string_view id) {
    for (const auto& entry : hits.entries()) {
        if (entry.payload.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

namespace {

/// 状态栏富展示片段 → 单行元素 ([ui_components] 的共享实现渲染)
///
/// 输入形态 (与 `update_status_item` 的 JSON 一致):
/// - `segments`: `[{text,color}]` 文本片段 (状态栏常见的"分色多段"写法)
/// - `sparkline`: 迷你趋势图参数 (`data` 必需; 高度强制 1 行)
/// - `meter`: 计量条参数 (`value`/`total`/`width`/`label`/`unit`/`thresholds`)
///
/// 三者可任意组合, 按 `segments → sparkline → meter` 顺序以单个空格拼接为一行;
/// 状态栏高度固定一行, 因此 sparkline 的 `height` 被忽略 (恒为 1)。
/// 无可用片段时返回 nullptr (调用方回退纯文本)。
ftxui::Element
    statusRichElement(const utilxx_base::Json& rich, const TUITheme& theme, int maxWidth) {
    agentxx::ui::Items cells;
    bool               any = false;
    if (const auto segs = rich.find("segments"); segs != rich.end() && segs->is_array()) {
        for (const auto& seg : *segs) {
            if (!seg.is_object()) {
                continue;
            }
            const std::string text = seg.value("text", std::string{});
            if (text.empty()) {
                continue;
            }
            cells.text(text, seg.value("color", std::string{"hint"}));
            any = true;
        }
    }
    if (const auto sp = rich.find("sparkline"); sp != rich.end() && sp->is_object()) {
        utilxx_base::Json params = *sp;
        params["height"]         = 1; // 状态栏只有一行
        cells.raw(std::move(params));
        any = true;
    }
    if (const auto mt = rich.find("meter"); mt != rich.end() && mt->is_object()) {
        cells.raw(*mt);
        any = true;
    }
    if (!any || cells.empty()) {
        return nullptr;
    }

    // 片段按横排组装 (共享实现负责间距与宽度分配)
    agentxx::ui::Items row;
    row.row(cells, {.gap = 1});

    client::UiRenderCtx rc;
    rc.theme  = &theme;
    rc.width  = (maxWidth > 0) ? maxWidth : 0;
    rc.indent = 0;
    UiRenderResult res;
    renderItems(agentxx::ui::parseItemList(row.json()), rc, res);
    if (res.rows.empty()) {
        return nullptr;
    }
    ftxui::Elements els;
    for (auto& rendered : res.rows) {
        els.push_back(std::move(rendered.element));
    }
    return ftxui::vbox(std::move(els)) | ftxui::xflex_shrink;
}

} // namespace

ftxui::Box StatusBarComponent::hitBox(std::string_view id) const {
    const auto* entry = findEntry(hits_, id);
    return entry ? *entry->box : agentxx::client::kNoBox;
}

Element StatusBarComponent::OnRender() {
    const auto& st    = *ctx_.frameState;
    const auto& theme = *ctx_.theme;

    // 帧首清空命中表: 本帧未渲染的区域不参与命中检测
    hits_.beginFrame();

    std::string modelName = st.cachedModelName;
    if (modelName.empty()) {
        modelName = std::string(tr("status.modelNone"));
    }

    size_t  ctx    = st.contextTokens;
    size_t  maxCtx = st.maxContextTokens;
    Element ctxText;
    if (maxCtx > 0) {
        ctxText = hbox({
            text(utilxx_base::formatSize(ctx, 1024, false)) | color(theme.hintColor),
            text("/") | color(theme.hintColor) | theme.dim(),
            text(utilxx_base::formatSize(maxCtx, 1024, false)) | color(theme.hintColor),
            text("·") | color(theme.hintColor) | theme.dim(),
            text(fmt::format(
                "{}%",
                static_cast<int>(100.0 * static_cast<double>(ctx) / static_cast<double>(maxCtx))
            )) | color(theme.hintColor)
                | xflex_shrink,
        });
    } else {
        ctxText = text(fmt::format("{}", utilxx_base::formatSize(ctx))) | color(theme.hintColor);
    }

    // 模型区域: 整体作为可点击区域 (点击打开模型选择弹窗)
    // - 流式期间 (ModelCall) 在上下文占比之后追加显示平均生成速度 (token/s)
    // - 模型名/上下文可能超宽: xflex_shrink 使长内容吸收剩余宽度并在右缘裁剪,
    //   避免 hbox 按比例压缩 "[F2] " 等前缀及右侧按钮 (向左覆盖压缩)
    std::vector<Element> modelChildren = {
        text("[F2] ") | color(theme.hintColor),
        text(modelName) | color(theme.accentColor) | xflex_shrink,
        text(" · ") | color(theme.hintColor),
        ctxText | color(theme.hintColor) | xflex_shrink,
    };
    const int tps = static_cast<int>(st.tps);
    if (st.isStreaming && tps > 0) {
        modelChildren.push_back(text("·") | color(theme.hintColor) | theme.dim());
        modelChildren.push_back(text(fmt::format("{}t/s", tps)) | color(theme.hintColor));
    }
    // 外层 hbox 也需感知 modelInfo 可收缩, 否则整块被按比例压缩 (见
    // ftxui box_helper::ComputeShrinkHard: 不可收缩元素同样被缩减)
    auto modelInfo
        = hits_.add(hbox(std::move(modelChildren)) | xflex_shrink, std::string{kModelHitId});

    // ---- 插件状态栏项 (client 插件注册; 左侧 align=0 / 右侧 align=1) ----
    // 从插件 UI 注册表快照读取 (短锁拷贝 shared_ptr, 渲染无锁);
    // 按 order 排序, 文本过长时裁剪 (UTF-8 安全截断)
    std::vector<Element> leftPluginItems;
    std::vector<Element> rightPluginItems;
    {
        // 注册表来源: 优先用本帧快照 (主渲染器帧首写入), 否则短锁取管理器快照
        std::shared_ptr<const agentxx::plugin::ClientUiRegistry> reg
            = ctx_.frameState ? ctx_.frameState->pluginRegistry : nullptr;
        if (!reg) {
            if (auto mgr = ctx_.pluginManager) {
                reg = mgr->uiRegistrySnapshot();
            }
        }
        if (reg) {
            // 按 (align, order) 排序: 同侧 order 小在前
            auto items = reg->statusItems;
            std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
                if (a.align != b.align) {
                    return a.align < b.align;
                }
                return a.order < b.order;
            });
            for (const auto& item : items) {
                // 富展示片段 (segments/sparkline/meter): 交共享组件层渲染为单行;
                // 无富片段或渲染为空时回退纯文本 (含 24 字截断)
                Element el;
                if (item.rich.is_object() && !item.rich.empty()) {
                    // 单侧最多占屏幕宽度的 1/3, 避免插件项挤掉状态栏核心信息
                    const int avail = std::max(12, ctx_.terminalSize().dimx / 3);
                    el              = statusRichElement(item.rich, theme, avail);
                }
                if (!el) {
                    std::string textStr = item.text;
                    const auto  nl      = textStr.find('\n');
                    if (nl != std::string::npos) {
                        textStr.resize(nl);
                    }
                    if (auto idx = utilxx_base::findIndexByUtf8Length(textStr, 24);
                        idx > 0 && idx < textStr.size()) {
                        textStr.resize(idx);
                        textStr += "...";
                    }
                    el = text(textStr) | color(theme.hintColor) | theme.dim() | xflex_shrink;
                }
                if (item.align == 0) {
                    leftPluginItems.push_back(std::move(el));
                } else {
                    rightPluginItems.push_back(std::move(el));
                }
            }
        }
    }

    // Sessions 按钮: 可点击打开会话选择弹窗 (F3), 位于 Settings 左侧
    auto sessionsText = hits_.add(
        text(tr("status.sessions")) | color(theme.hintColor),
        std::string{kSessionsHitId}
    );

    // Settings 按钮: 可点击打开设置弹窗
    auto settingsText = hits_.add(
        text(tr("status.settings")) | color(theme.hintColor),
        std::string{kSettingsHitId}
    );

    // 组装: 左段 = 模型信息 + 插件左项; 右段 = 插件右项 + Sessions + Settings
    std::vector<Element> leftChildren = {
        text(" "),
        std::move(modelInfo),
        text(" "),
    };
    for (auto& el : leftPluginItems) {
        leftChildren.push_back(text(" "));
        leftChildren.push_back(std::move(el));
    }
    std::vector<Element> rightChildren = {
        text(" "),
    };
    for (auto& el : rightPluginItems) {
        rightChildren.push_back(std::move(el));
        rightChildren.push_back(text(" "));
    }
    rightChildren.push_back(std::move(sessionsText));
    rightChildren.push_back(text(" "));
    rightChildren.push_back(std::move(settingsText));
    rightChildren.push_back(text(" "));

    return hbox({
        hbox(std::move(leftChildren)),
        filler(),
        hbox(std::move(rightChildren)),
    });
}

bool StatusBarComponent::OnEvent(Event event) {
    if (!event.is_mouse()) {
        return false;
    }
    // 左键释放命中 -> 执行对应动作 (命中表由本帧渲染建立; 未渲染区域不命中)
    const auto* hit = hits_.findClick(event.mouse());
    if (hit == nullptr) {
        return false;
    }
    const std::function<void()>* action = nullptr;
    if (hit->payload.id == kModelHitId) {
        action = &config_.onModelClick;
    } else if (hit->payload.id == kSessionsHitId) {
        action = &config_.onSessionsClick;
    } else if (hit->payload.id == kSettingsHitId) {
        action = &config_.onSettingsClick;
    }
    if (action == nullptr || !*action) {
        return false;
    }
    ctx_.postRedraw();
    (*action)();
    return true;
}

} // namespace agentxx::client
