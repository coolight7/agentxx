#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/components/sidebar.h"
#include "agentxx/util/string_util.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include <algorithm>
#include <filesystem>

using namespace ftxui;

namespace {

ftxui::Element buildLogLine(const TUILogSink::Line& line, const TUITheme& theme) {
    ftxui::Color c = theme.assistantColor;
    std::string  prefix;
    switch (line.level) {
        case agentxx::util::LogLevel::Debug:
            c      = theme.hintColor;
            prefix = "[D] ";
            break;
        case agentxx::util::LogLevel::Info:
            c      = theme.statusColor;
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
            c      = theme.assistantColor;
            prefix = "";
            break;
    }
    return paragraph(prefix + line.text) | color(c);
}

} // namespace

std::vector<ScrollItem> TUIClientAgentIO::renderLogWindow() {
    auto lines = logSink_ ? logSink_->snapshot() : std::vector<TUILogSink::Line>{};
    if (lines.empty()) {
        return {
            ScrollItem{text("[Empty]") | dim, false}
        };
    }
    const uint64_t curPopped = logSink_ ? logSink_->poppedCount() : 0;
    if (curPopped != logCachePoppedCount_) {
        logLineCache_.clear();
        logCachePoppedCount_ = curPopped;
    }
    if (logLineCache_.size() > lines.size()) {
        logLineCache_.clear();
    }
    while (logLineCache_.size() < lines.size()) {
        logLineCache_.push_back(buildLogLine(lines[logLineCache_.size()], theme_));
    }
    std::vector<ScrollItem> items;
    items.reserve(logLineCache_.size());
    for (auto& el : logLineCache_) {
        items.push_back(ScrollItem{el, false});
    }
    return items;
}

std::optional<ftxui::Element> TUIClientAgentIO::renderPlanningInfo() {
    const auto& st = *ctx_.frameState;

    const TUIMessage* plan = nullptr;
    for (size_t i = st.messages.size(); i > 0; --i) {
        const auto& m = *st.messages[i - 1];
        if (m.role == TUIMessage::Role::Tool && m.toolName == "planning_write") {
            plan = &m;
            break;
        }
    }
    if (!plan) {
        return std::nullopt;
    }

    neograph::json args;
    try {
        args = neograph::json::parse(plan->text);
    } catch (...) {
        return std::nullopt;
    }

    Elements lines;
    Elements title;
    title.push_back(text("Plan") | color(theme_.accentColor));
    if (!plan->toolFinished) {
        title.push_back(text(" Planning...") | color(theme_.hintColor));
    }
    lines.push_back(hbox(std::move(title)));

    if (args.contains("todos") && args["todos"].is_array()) {
        for (const auto& td : args["todos"]) {
            const auto   state   = td.value("state", std::string{});
            const auto   content = td.value("content", std::string{});
            std::string  icon    = "[ ]";
            ftxui::Color c       = theme_.assistantColor;
            if (state == "in_progress") {
                icon = "[~]";
                c    = theme_.thinkingColor;
            } else if (state == "completed") {
                icon = "[#]";
                c    = theme_.promptColor;
            } else if (state == "failed") {
                icon = "[!]";
                c    = theme_.errorColor;
            }
            lines.push_back(hbox({
                text(fmt::format("{} ", icon)) | color(c),
                paragraph(content) | color(c),
            }));
        }
    }

    const auto notes = args.value("notes", std::string{});
    if (!notes.empty()) {
        lines.push_back(text(""));
        lines.push_back(text("Notes") | color(theme_.accentColor));
        lines.push_back(paragraph(notes) | color(theme_.assistantColor));
    }

    return vbox(std::move(lines));
}

std::vector<ScrollItem> TUIClientAgentIO::renderInfoSidebar() {
    const auto& st = *ctx_.frameState;

    Elements elements;

    // 系统资源占用 (CPU/内存), 由资源监控线程周期刷新
    if (systemInfoEnabled_.load(std::memory_order_relaxed)) {
        Elements sysEls;
        sysEls.push_back(text("System") | color(theme_.accentColor));
        if (st.systemUsage) {
            const auto& usage = *st.systemUsage;
            sysEls.push_back(
                hbox({
                    text("|- CPU: "),
                    text(fmt::format("{:.1f}%", usage.cpuUsagePercent)),
                })
                | color(theme_.assistantColor)
            );
            sysEls.push_back(
                hbox({
                    text("|- RAM: "),
                    text(fmt::format(
                        "{:.1f}% ({} / {})",
                        usage.memory.usagePercent,
                        agentxx::util::formatSize(usage.memory.usedPhysicalMB * 1024 * 1024),
                        agentxx::util::formatSize(usage.memory.totalPhysicalMB * 1024 * 1024)
                    )),
                })
                | color(theme_.assistantColor)
            );
        } else {
            sysEls.push_back(text("|- loading...") | color(theme_.hintColor));
        }
        elements.push_back(vbox(std::move(sysEls)));
        elements.push_back(text(" "));
    }

    if (auto planning = renderPlanningInfo()) {
        elements.push_back(std::move(*planning));
        elements.push_back(text(" "));
    }

    if (!st.appendComponents.empty()) {
        Elements appendEls;
        appendEls.push_back(text("Append") | color(theme_.accentColor));

        auto appendGroup = [&](std::string_view                                  label,
                               agentxx::agent::AppendComponentNotification::Type type,
                               bool                                              splitName) {
            size_t   count = 0;
            Elements elems;
            for (const auto& notif : st.appendComponents) {
                if (notif.type != type) {
                    continue;
                }
                ++count;
                elems.push_back(
                    (splitName ? hbox({text(fmt::format(
                                     "|  {}·{}",
                                     agentxx::util::getFileName(notif.name),
                                     notif.name
                                 ))})
                               : hbox({text("|  "), text(notif.name)}))
                    | color(notif.success ? theme_.assistantColor : theme_.systemColor)
                );
            }
            if (count > 0) {
                appendEls.push_back(
                    hbox({text("|- "), text(fmt::format("{}: {}", label, count))})
                    | color(theme_.assistantColor)
                );
                appendEls.push_back(vbox(elems));
            }
        };
        appendGroup("Memory", agentxx::agent::AppendComponentNotification::Type::Memory, true);
        appendGroup("Skill", agentxx::agent::AppendComponentNotification::Type::Skill, true);
        appendGroup("MCP", agentxx::agent::AppendComponentNotification::Type::Mcp, false);

        elements.push_back(vbox(std::move(appendEls)));
    }

    if (elements.empty()) {
        elements.push_back(text("[Empty]") | color(theme_.hintColor));
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

    std::string cwd;
    try {
        cwd = std::filesystem::current_path().string();
    } catch (...) {
        cwd = "(Unknown Work Dir)";
    }
    elements.push_back(text(cwd) | color(theme_.hintColor));

    std::string mode = remoteUrl_.empty() ? "Inner Server" : remoteUrl_;
    elements.push_back(
        hbox({
            text(fmt::format("Agentxx {} ", kAgentxxVersion)),
            filler(),
            text(mode),
        })
        | xflex | color(theme_.hintColor)
    );

    return vbox(std::move(elements));
}

ftxui::Element TUIClientAgentIO::renderLogSidebarFooter() {
    const auto& st = *ctx_.frameState;

    Elements row;
    if (st.currentNodeName.empty()) {
        row.push_back(text(" idle") | color(theme_.hintColor));
    } else {
        row.push_back(text(fmt::format(" > {}", st.currentNodeName)) | color(theme_.accentColor));
    }
    row.push_back(filler());

    auto ctxBtn = text(" LLM Context ") | bgcolor(theme_.buttonBgColor)
                  | color(theme_.buttonTextColor) | reflect(contextButtonBox_);
    row.push_back(ctxBtn);

    return hbox(std::move(row));
}
