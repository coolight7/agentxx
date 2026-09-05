#include "agentxx-client/io/tui/components/status_bar.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "ftxui/dom/elements.hpp"
#include <algorithm>
#include <vector>

using namespace ftxui;

Element StatusBarComponent::OnRender() {
    const auto& st    = *ctx_.frameState;
    const auto& theme = *ctx_.theme;

    std::string modelName = st.cachedModelName;
    if (modelName.empty()) {
        modelName = std::string(tr("status.modelNone"));
    }

    size_t  ctx    = st.contextTokens;
    size_t  maxCtx = st.maxContextTokens;
    Element ctxText;
    if (maxCtx > 0) {
        ctxText = hbox({
            text(agentxx::util::formatSize(ctx, 1024, false)) | color(theme.hintColor),
            text("/") | color(theme.hintColor) | dim,
            text(agentxx::util::formatSize(maxCtx, 1024, false)) | color(theme.hintColor),
            text("·") | color(theme.hintColor) | dim,
            text(fmt::format(
                "{}%",
                static_cast<int>(100.0 * static_cast<double>(ctx) / static_cast<double>(maxCtx))
            )) | color(theme.hintColor)
                | xflex_shrink,
        });
    } else {
        ctxText = text(fmt::format("{}", agentxx::util::formatSize(ctx))) | color(theme.hintColor);
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
        modelChildren.push_back(text("·") | color(theme.hintColor) | dim);
        modelChildren.push_back(text(fmt::format("{}t/s", tps)) | color(theme.hintColor));
    }
    // 外层 hbox 也需感知 modelInfo 可收缩, 否则整块被按比例压缩 (见
    // ftxui box_helper::ComputeShrinkHard: 不可收缩元素同样被缩减)
    auto modelInfo = hbox(std::move(modelChildren)) | xflex_shrink | reflect(modelBox_);

    // ---- 插件状态栏项 (client 插件注册; 左侧 align=0 / 右侧 align=1) ----
    // 从插件 UI 注册表快照读取 (短锁拷贝 shared_ptr, 渲染无锁);
    // 按 order 排序, 文本过长时裁剪 (UTF-8 安全截断)
    std::vector<Element> leftPluginItems;
    std::vector<Element> rightPluginItems;
    if (auto mgr = ctx_.pluginManager) {
        auto reg = mgr->uiRegistrySnapshot();
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
                std::string textStr = item.text;
                const auto  nl      = textStr.find('\n');
                if (nl != std::string::npos) {
                    textStr.resize(nl);
                }
                if (auto idx = agentxx::util::findIndexByUtf8Length(textStr, 24);
                    idx > 0 && idx < textStr.size()) {
                    textStr.resize(idx);
                    textStr += "...";
                }
                auto el = text(textStr) | color(theme.hintColor) | dim | xflex_shrink;
                if (item.align == 0) {
                    leftPluginItems.push_back(std::move(el));
                } else {
                    rightPluginItems.push_back(std::move(el));
                }
            }
        }
    }

    // Sessions 按钮: 可点击打开会话选择弹窗 (F4), 位于 Settings 左侧
    auto sessionsText = text(tr("status.sessions")) | color(theme.hintColor) | reflect(sessionBox_);

    // Settings 按钮: 可点击打开设置弹窗, 鼠标悬浮时高亮背景
    auto settingsText
        = text(tr("status.settings")) | color(theme.hintColor) | reflect(settingsBox_);

    // 组装: 左段 = 模型信息 + 插件左项; 右段 = 插件右项 + Sessions + Settings
    std::vector<Element> leftChildren = {
        text(" "),
        modelInfo,
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
    rightChildren.push_back(sessionsText);
    rightChildren.push_back(text(" "));
    rightChildren.push_back(settingsText);
    rightChildren.push_back(text(" "));

    return hbox({
        hbox(std::move(leftChildren)),
        filler(),
        hbox(std::move(rightChildren)),
    });
}
