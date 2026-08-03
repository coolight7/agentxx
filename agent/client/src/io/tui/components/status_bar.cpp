#include "agentxx-client/io/tui/components/status_bar.h"
#include "ftxui/dom/elements.hpp"

using namespace ftxui;

Element StatusBarComponent::OnRender() {
    const auto& st    = *ctx_.frameState;
    const auto& theme = *ctx_.theme;

    std::string modelName = st.cachedModelName;
    if (modelName.empty()) {
        modelName = "<none>";
    }

    size_t ctx    = 0;
    size_t maxCtx = 0;
    if (ctx_.session && ctx_.session->contextStats) {
        ctx    = ctx_.session->contextStats->contextTokens.load();
        maxCtx = ctx_.session->contextStats->maxContextTokens.load();
    }
    Element ctxText;
    if (maxCtx > 0) {
        ctxText = hbox({
            text(agentxx::util::formatSize(ctx)) | color(theme.hintColor),
            text("/") | color(theme.hintColor) | dim,
            text(agentxx::util::formatSize(maxCtx)) | color(theme.hintColor),
            text("·") | color(theme.hintColor) | dim,
            text(fmt::format(
                "{}%",
                static_cast<int>(100.0 * static_cast<double>(ctx) / static_cast<double>(maxCtx))
            )) | color(theme.hintColor),
        });
    } else {
        ctxText = text(fmt::format("{}", agentxx::util::formatSize(ctx))) | color(theme.hintColor);
    }

    auto modelInfo = hbox({
        text("[F2] ") | color(theme.hintColor),
        text(modelName) | color(theme.accentColor),
        text(" · ") | color(theme.hintColor),
        ctxText | color(theme.hintColor),
    });
    return hbox({
        text(" "),
        modelInfo,
        text(" "),
        filler(),
        text(" "),
        text("[F3] Settings") | color(theme.hintColor) | reflect(settingsBox_),
        text(" "),
    });
}
