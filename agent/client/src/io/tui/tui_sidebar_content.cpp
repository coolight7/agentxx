#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/components/sidebar.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/plugin_ui_items.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include <algorithm>
#include <filesystem>
#include <markdown/state_diagram.hpp>

using namespace ftxui;

namespace {

ftxui::Element buildLogLine(const TUILogSink::Line& line, const TUITheme& theme) {
    ftxui::Color c = theme.normalColor;
    std::string  prefix;
    switch (line.level) {
        case agentxx::util::LogLevel::Trace:
            c      = theme.hintColor;
            prefix = "[T] ";
            break;
        case agentxx::util::LogLevel::Debug:
            c      = theme.hintColor;
            prefix = "[D] ";
            break;
        case agentxx::util::LogLevel::Info:
            c      = theme.accentColor;
            prefix = "[I] ";
            break;
        case agentxx::util::LogLevel::Warn:
            c      = theme.thinkingColor;
            prefix = "[W] ";
            break;
        case agentxx::util::LogLevel::Error:
            c      = theme.errorColor;
            prefix = "[E] ";
            break;
        case agentxx::util::LogLevel::Out:
            c      = theme.accentColor;
            prefix = "";
            break;
    }
    return paragraph(prefix + line.text) | color(c);
}

/// 渲染插件段落/面板 items JSON 元素 (通用, 零特化; 共享 helper 收敛点)
/// - text/progress/badge/separator/button/diagram/diff 全走 plugin_ui_items
/// - button 有 action_id 且快照有该 plugin 绑定 → 挂 hitTargets_ 并 reflect
/// - text + button 隐式同行合并与 prefix 显式前缀统一在此实现
/// - diagram 保留静态内联渲染 (历史消息兼容), 不挂点击
static void appendPluginItems(
    const neograph::json&                       items,
    std::string_view                            plugin,
    std::string_view                            ownerId,
    const agentxx::plugin::ClientUiRegistry*    reg,
    const TUITheme&                             theme,
    ftxui::Elements&                            out,
    std::vector<TUIClientAgentIO::UiHitTarget>& hits
) {
    if (!items.is_array()) {
        return;
    }
    auto push = [&](ftxui::Element el) {
        out.push_back(std::move(el));
    };
    const size_t itemCount = items.size();
    for (size_t i = 0; i < itemCount; ++i) {
        const auto& it = items[i];
        if (!it.is_object()) {
            continue;
        }
        const auto kind = it.value("kind", std::string{"text"});
        if (kind == "text") {
            // 若下一个 item 为 button (如 "|- " 后面紧跟按钮), 则合并为单行展示
            if (i + 1 < itemCount && items[i + 1].is_object()
                && (items[i + 1].value("kind", std::string{}) == "button"
                    || items[i + 1].value("kind", std::string{}) == "action")) {
                agentxx::client::PluginButtonDesc desc;
                if (agentxx::client::parsePluginButton(items[i + 1], plugin, reg, desc)) {
                    Element btn = agentxx::client::renderPluginButton(desc, theme);
                    if (desc.clickable) {
                        TUIClientAgentIO::UiHitTarget t;
                        t.plugin   = std::string{plugin};
                        t.ownerId  = std::string{ownerId};
                        t.actionId = desc.actionId;
                        t.argsJson = desc.argsJson;
                        hits.push_back(std::move(t));
                        btn = btn | reflect(hits.back().box);
                    }
                    push(hbox({
                        agentxx::client::renderPluginTextItem(
                            it.value("text", std::string{}),
                            it.value("role", std::string{"normal"}),
                            theme
                        ),
                        std::move(btn),
                    }));
                    ++i;
                    continue;
                }
            }
            push(agentxx::client::renderPluginTextItem(
                it.value("text", std::string{}),
                it.value("role", std::string{"normal"}),
                theme
            ));
        } else if (kind == "progress") {
            const double v      = it.value("value", 0.0);
            const int    w      = 10;
            const int    filled = static_cast<int>(v * w);
            std::string  bar;
            bar.reserve(static_cast<size_t>(w));
            for (int j = 0; j < w; ++j) {
                bar += (j < filled) ? '#' : '-';
            }
            push(hbox({
                text("[" + bar + "]") | color(theme.accentColor),
                text(fmt::format(" {}%", static_cast<int>(v * 100))) | color(theme.hintColor),
            }));
        } else if (kind == "badge") {
            push(text("● " + it.value("text", std::string{})) | color(theme.accentColor));
        } else if (kind == "separator") {
            push(text("─") | color(theme.hintColor) | dim);
        } else if (kind == "button" || kind == "action") {
            agentxx::client::PluginButtonDesc desc;
            if (!agentxx::client::parsePluginButton(it, plugin, reg, desc)) {
                continue;
            }
            Element btn = agentxx::client::renderPluginButton(desc, theme);
            if (desc.clickable) {
                TUIClientAgentIO::UiHitTarget t;
                t.plugin   = std::string{plugin};
                t.ownerId  = std::string{ownerId};
                t.actionId = desc.actionId;
                t.argsJson = desc.argsJson;
                hits.push_back(std::move(t));
                btn = btn | reflect(hits.back().box);
            }
            if (!desc.prefix.empty()) {
                push(hbox({
                    text(desc.prefix) | color(theme.normalColor),
                    std::move(btn),
                }));
            } else {
                push(std::move(btn));
            }
        } else if (kind == "diagram") {
            // 静态内联渲染 (历史消息兼容), 不挂点击
            const auto mermaid = it.value("mermaid", std::string{});
            auto       diagram = markdown::parseMermaidStateDiagram(mermaid);
            if (!diagram.nodes.empty()) {
                const int diagW = std::max(20, Terminal::Size().dimx - 16);
                push(markdown::renderMermaidStateDiagram(
                    diagram,
                    diagW,
                    theme.normalColor,
                    markdown::diagramNodeColor(theme.markdownTheme)
                ));
            }
        } else if (kind == "diff") {
            push(agentxx::client::renderPluginDiff(
                it.value("path", std::string{}),
                it.value("old_str", std::string{}),
                it.value("new_str", std::string{}),
                theme
            ));
        }
    }
}

} // namespace

std::vector<ScrollItem> TUIClientAgentIO::renderLogWindow() {
    if (!logSink_) {
        return {
            ScrollItem{text(tr("info.empty")) | dim, false}
        };
    }
    // 仅当日志内容有变化时才重新 snapshot + 重建缓存:
    // 日志行可能高达 2000 行, 每帧全量拷贝字符串是浪费 (日志 tab 打开时每帧渲染都触发)
    const size_t   curCount  = logSink_->lineCount();
    const uint64_t curPopped = logSink_->poppedCount();
    if (curCount != logCacheLineCount_ || curPopped != logCachePoppedCount_) {
        if (curPopped != logCachePoppedCount_ || curCount < logCacheLineCount_) {
            // 淘汰发生 (popped 增加) 或行数减少 (clear): 缓存整体失效
            logLineCache_.clear();
        }
        logCachePoppedCount_ = curPopped;
        logCacheLineCount_   = curCount;
        auto lines           = logSink_->snapshot();
        // 增量构建新增行 (未发生淘汰时, 前段与缓存一一对应)
        while (logLineCache_.size() < lines.size()) {
            logLineCache_.push_back(buildLogLine(lines[logLineCache_.size()], theme_));
        }
    }
    if (logLineCache_.empty()) {
        return {
            ScrollItem{text(tr("info.empty")) | dim, false}
        };
    }
    std::vector<ScrollItem> items;
    items.reserve(logLineCache_.size());
    for (auto& el : logLineCache_) {
        items.push_back(ScrollItem{el, false});
    }
    return items;
}

std::vector<ScrollItem> TUIClientAgentIO::renderInfoSidebar() {
    const auto& st = *ctx_.frameState;

    Elements elements;

    // Plan 段落已拆分至 agentxx_planning 插件 client 侧 (经 register_info_section
    // 注入, 见 agent/plugins/agentxx_planning), TUI 不再硬编码渲染

    // 插件扩展的 Info 段落 (插件经 register_info_section 注入; UI 线程渲染,
    // 每帧从 client 插件注册表快照读取, 无需缓存):
    // - 段落在 Append 之后按注册顺序展示 (标题 + items, items schema 同面板)
    // - 若段落无内容项则跳过，避免仅显示孤立标题
    // - button 走通用 action_id 派发 (owner=section_id, 经 hitTargets_ 挂载)
    if (auto mgr = pluginManager_) {
        auto reg = mgr->uiRegistrySnapshot();
        if (reg && !reg->infoSections.empty()) {
            for (const auto& sec : reg->infoSections) {
                Elements secItems;
                appendPluginItems(
                    sec.items,
                    sec.plugin,
                    sec.id,
                    reg.get(),
                    theme_,
                    secItems,
                    hitTargets_
                );
                if (secItems.empty()) {
                    continue;
                }
                Elements secEls;
                if (!sec.title.empty()) {
                    secEls.push_back(text(sec.title) | color(theme_.accentColor));
                }
                secEls.insert(
                    secEls.end(),
                    std::make_move_iterator(secItems.begin()),
                    std::make_move_iterator(secItems.end())
                );
                elements.push_back(vbox(std::move(secEls)));
                elements.push_back(text(" "));
            }
        }
    }

    // 已加载组件 (Plugin/Memory/Skill/MCP) 展示:
    // - CodeGraph 索引状态与系统资源占用由对应插件经 register_info_section
    //   注入本 Info 栏 (见下方 "插件扩展 Info 段落"), TUI 不再单独渲染
    // - 本帧渲染前先清空 Failed 组 [view] 按钮命中区域 (Append 段未渲染/
    //   无失败项时防止残留旧区域误触); 按钮渲染时经 reflect 重新填充
    failedViewButtonBox_ = ftxui::Box{0, -1, 0, -1};
    if (!st.appendComponents.empty()) {
        Elements appendEls;
        appendEls.push_back(text(tr("info.append")) | color(theme_.accentColor));

        auto appendGroup = [&](std::string_view                                  label,
                               agentxx::agent::AppendComponentNotification::Type type,
                               bool                                              splitName) {
            size_t   count = 0;
            Elements elems;
            for (const auto& notif : st.appendComponents) {
                if (notif.type != type || !notif.success) {
                    continue;
                }
                ++count;
                elems.push_back(
                    (splitName ? hbox({text(fmt::format(
                                     "|  {}·{}",
                                     agentxx::util::getFileName(notif.name),
                                     notif.name
                                 ))})
                               : hbox({text("|  "), text(notif.name) | xflex_shrink}))
                    | color(theme_.hintColor)
                );
            }
            if (count > 0) {
                appendEls.push_back(
                    hbox({text("|- "), text(fmt::format("{}: {}", label, count))})
                    | color(theme_.normalColor)
                );
                appendEls.push_back(vbox(elems));
            }
        };
        appendGroup("Memory", agentxx::agent::AppendComponentNotification::Type::Memory, true);
        appendGroup("Skill", agentxx::agent::AppendComponentNotification::Type::Skill, true);
        appendGroup("MCP", agentxx::agent::AppendComponentNotification::Type::Mcp, false);
        appendGroup("Plugins", agentxx::agent::AppendComponentNotification::Type::Plugin, false);

        // 加载失败组件汇总组: 统计 success=false 的通知, 展示 "|- Failed: 数量"
        // 与 "| [view]" 按钮 (点击弹窗查看失败详情; 命中区域 reflect 到
        // failedViewButtonBox_, 点击处理见 agent_tui 主鼠标事件分支)
        size_t failedCount = 0;
        for (const auto& notif : st.appendComponents) {
            if (!notif.success) {
                ++failedCount;
            }
        }
        if (failedCount > 0) {
            appendEls.push_back(
                hbox({text("|- "), text(trf("info.appendFailed", failedCount))})
                | color(theme_.errorColor)
            );
            appendEls.push_back(hbox({
                text("|  ") | color(theme_.hintColor),
                text(tr("info.viewFailed")) | bgcolor(theme_.buttonBgColor)
                    | color(theme_.buttonTextColor) | reflect(failedViewButtonBox_),
            }));
        }

        elements.push_back(vbox(std::move(appendEls)));
    }

    if (elements.empty()) {
        elements.push_back(text(tr("info.empty")) | color(theme_.hintColor));
    }

    std::vector<ScrollItem> items;
    items.reserve(elements.size());
    for (auto& el : elements) {
        items.push_back(ScrollItem{std::move(el), false});
    }
    return items;
}

ftxui::Element TUIClientAgentIO::renderInfoSidebarFooter() {
    Elements elements;

    // 工作目录在进程运行期间固定: 首次调用时计算并缓存到静态变量,
    // 避免 Info tab 常驻时每帧执行 current_path() 系统调用
    static const std::string kCwd = agentxx::util::catchError<std::string>(
        []() -> std::string {
            return std::filesystem::current_path().string();
        },
        [](std::string) -> std::string {
            return std::string(tr("info.workDirUnknown"));
        }
    );
    elements.push_back(text(kCwd) | color(theme_.hintColor));

    std::string mode = remoteUrl_.empty() ? "Inner Server" : remoteUrl_;
    elements.push_back(
        hbox({
            text(fmt::format("Agentxx {} ", kAgentxxVersion)),
            filler(),
            text(mode) | xflex_shrink,
        })
        | xflex | color(theme_.hintColor)
    );

    return vbox(std::move(elements));
}

ftxui::Element TUIClientAgentIO::renderLogSidebarFooter() {
    const auto& st = *ctx_.frameState;

    Elements row;
    if (st.currentNodeName.empty()) {
        row.push_back(text(tr("info.idle")) | color(theme_.hintColor));
    } else {
        row.push_back(text(fmt::format("> {}", st.currentNodeName)) | color(theme_.accentColor));
    }
    row.push_back(filler());

    auto menuBtn = text(tr("footer.menu")) | bgcolor(theme_.buttonBgColor)
                   | color(theme_.buttonTextColor) | reflect(contextButtonBox_);
    row.push_back(menuBtn);

    return hbox(std::move(row));
}
