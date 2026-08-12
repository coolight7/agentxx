#include "agentxx-client/io/tui/components/status_bar.h"
#include "ftxui/dom/elements.hpp"
#include <vector>

using namespace ftxui;

Element StatusBarComponent::OnRender() {
    const auto& st    = *ctx_.frameState;
    const auto& theme = *ctx_.theme;

    std::string modelName = st.cachedModelName;
    if (modelName.empty()) {
        modelName = "<none>";
    }

    size_t ctx    = st.contextTokens;
    size_t maxCtx = st.maxContextTokens;
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
            )) | color(theme.hintColor),
        });
    } else {
        ctxText = text(fmt::format("{}", agentxx::util::formatSize(ctx))) | color(theme.hintColor);
    }

    // 模型区域: 整体作为可点击区域 (点击打开模型选择弹窗)
    // - 流式期间 (ModelCall) 在上下文占比之后追加显示平均生成速度 (token/s)
    std::vector<Element> modelChildren = {
        text("[F2] ") | color(theme.hintColor),
        text(modelName) | color(theme.accentColor),
        text(" · ") | color(theme.hintColor),
        ctxText | color(theme.hintColor),
    };
    const int tps = static_cast<int>(st.tps);
    if (st.isStreaming && tps > 0) {
        modelChildren.push_back(text("·") | color(theme.hintColor) | dim);
        modelChildren.push_back(text(fmt::format("{}t/s", tps)) | color(theme.hintColor));
    }
    auto modelInfo = hbox(std::move(modelChildren)) | reflect(modelBox_);

    // Sessions 按钮: 可点击打开会话选择弹窗 (F4), 位于 Settings 左侧
    auto sessionsText = text("[F4] Sessions") | color(theme.hintColor) | reflect(sessionBox_);

    // Settings 按钮: 可点击打开设置弹窗, 鼠标悬浮时高亮背景
    auto settingsText = text("[F3] Settings") | color(theme.hintColor) | reflect(settingsBox_);

    return hbox({
        text(" "),
        modelInfo,
        text(" "),
        filler(),
        text(" "),
        sessionsText,
        text(" "),
        settingsText,
        text(" "),
    });
}
