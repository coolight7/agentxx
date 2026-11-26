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
    // 外层 hbox 也需知道 modelInfo 可收缩, 否则整块被按比例压缩 (见
    // ftxui box_helper::ComputeShrinkHard: 不可收缩元素同样被缩减)
    auto modelInfo
        = hits_.add(hbox(std::move(modelChildren)) | xflex_shrink, std::string{kModelHitId});

    // ---- 插件状态栏项 (左侧 align=0 / 右侧 align=1) ----
    // 只有一条绘制路径: 位置与画什么都在声明式注册表里 (`register_ui_entry` +
    // `register_ui_content`; 旧 `register_status_item` 由管理器写进同一套注册表)
    std::vector<Element> leftPluginItems;
    std::vector<Element> rightPluginItems;
    {
        // 快照来源: 优先用本帧快照 (主渲染器帧首写入), 否则短锁取管理器快照
        std::shared_ptr<const agentxx::plugin::ClientUiSnapshot> uiSnap
            = ctx_.frameState ? ctx_.frameState->uiContent : nullptr;
        if (!uiSnap) {
            if (auto mgr = ctx_.pluginManager) {
                uiSnap = mgr->uiContentSnapshot();
            }
        }
        if (uiSnap) {
            // 按钮动作绑定与可点区域要用它 (与本函数上文同一来源)
            std::shared_ptr<const agentxx::plugin::ClientUiRegistry> uiReg
                = ctx_.frameState ? ctx_.frameState->pluginRegistry : nullptr;
            if (!uiReg) {
                if (auto mgr = ctx_.pluginManager) {
                    uiReg = mgr->uiRegistrySnapshot();
                }
            }
            struct DeclItem {
                int32_t align = 0;
                int32_t order = 0;
                uint64_t seq  = 0;
                Element el;
            };
            std::vector<DeclItem> declItems;
            for (const auto& entry : uiSnap->entries) {
                if (entry.type != agentxx::plugin::kClientUiSlotStatus) {
                    continue;
                }
                const auto* content = uiSnap->contentOf(
                    agentxx::plugin::kClientUiSlotStatus,
                    entry.shortId
                );
                if (content == nullptr || !content->hasResult) {
                    continue;
                }
                utilxx_base::Json data = utilxx_base::Json::object();
                try {
                    data = utilxx_base::Json::parse(entry.dataJson);
                } catch (...) {
                    data = utilxx_base::Json::object();
                }
                if (!data.is_object()) {
                    data = utilxx_base::Json::object();
                }
                DeclItem item;
                item.align = data.value("align", 0) == 0 ? 0 : 1;
                item.order = data.value("order", 0);
                item.seq   = entry.seq;

                auto doc = pluginxx::ui::parseBlocks(content->parsed);

                agentxx::client::UiRenderCtx rc;
                rc.theme   = &theme;
                rc.width   = std::max(12, ctx_.terminalSize().dimx / 3);
                rc.plugin  = entry.plugin;
                rc.ownerId = entry.shortId;
                rc.registry = uiReg.get();
                agentxx::client::UiRenderResult res;
                agentxx::client::renderItems(doc, rc, res);
                Elements parts;
                for (auto& row : res.rows) {
                    parts.push_back(std::move(row.element));
                }
                if (parts.empty()) {
                    continue;
                }
                item.el = hbox(std::move(parts)) | xflex_shrink;
                declItems.push_back(std::move(item));
            }
            std::sort(
                declItems.begin(),
                declItems.end(),
                [](const DeclItem& a, const DeclItem& b) {
                    if (a.align != b.align) {
                        return a.align < b.align;
                    }
                    if (a.order != b.order) {
                        return a.order < b.order;
                    }
                    return a.seq < b.seq;
                }
            );
            for (auto& item : declItems) {
                if (item.align == 0) {
                    leftPluginItems.push_back(std::move(item.el));
                } else {
                    rightPluginItems.push_back(std::move(item.el));
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
