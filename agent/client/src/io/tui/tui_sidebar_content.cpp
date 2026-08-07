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

} // namespace

std::vector<ScrollItem> TUIClientAgentIO::renderLogWindow() {
    if (!logSink_) {
        return {
            ScrollItem{text("[Empty]") | dim, false}
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
        auto lines = logSink_->snapshot();
        // 增量构建新增行 (未发生淘汰时, 前段与缓存一一对应)
        while (logLineCache_.size() < lines.size()) {
            logLineCache_.push_back(buildLogLine(lines[logLineCache_.size()], theme_));
        }
    }
    if (logLineCache_.empty()) {
        return {
            ScrollItem{text("[Empty]") | dim, false}
        };
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
        if (m.role == TUIMessage::Role::Tool && m.toolName == "agentxx_planning_write") {
            plan = st.messages[i - 1].get();
            break;
        }
    }
    if (!plan) {
        return std::nullopt;
    }

    // 解析缓存: plan 消息被修改时 (mutableMessage 总是复制 → 指针变化)
    // 或文本长度变化时重新解析, 避免每帧重复解析 planning 参数 JSON
    if (planCacheMsgPtr_ != plan || planCacheTextLen_ != plan->text.size()
        || planCacheFinished_ != plan->toolFinished) {
        planCacheMsgPtr_   = plan;
        planCacheTextLen_  = plan->text.size();
        planCacheFinished_ = plan->toolFinished;
        planCacheArgs_     = neograph::json::array();
        try {
            planCacheArgs_  = neograph::json::parse(plan->text);
            planCacheValid_ = true;
        } catch (...) {
            planCacheValid_ = false;
        }
    }
    if (!planCacheValid_) {
        return std::nullopt;
    }
    const auto& args = planCacheArgs_;

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
            ftxui::Color c       = theme_.hintColor;
            if (state == "in_progress") {
                icon = "[~]";
                c    = theme_.thinkingColor;
            } else if (state == "completed") {
                icon = "[#]";
                c    = theme_.accentColor;
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
        lines.push_back(paragraph(notes) | color(theme_.hintColor));
    }

    return vbox(std::move(lines));
}

std::vector<ScrollItem> TUIClientAgentIO::renderInfoSidebar() {
    const auto& st = *ctx_.frameState;

    Elements elements;

    // 系统资源占用 (CPU/内存), 由资源监控线程周期刷新; 显示开关存储于全局设置单例
    if (TUISettings::instance().showSystemInfo()) {
        Elements sysEls;
        sysEls.push_back(text("System") | color(theme_.accentColor));
        if (st.systemUsage) {
            const auto& usage = *st.systemUsage;
            sysEls.push_back(
                text(fmt::format("|- CPU: {:.1f}%", usage.cpuUsagePercent))
                | color(theme_.normalColor)
            );
            sysEls.push_back(
                text(fmt::format(
                    "|- RAM: {:.1f}% {}/{}",
                    usage.memory.usagePercent,
                    agentxx::util::formatSize(usage.memory.usedPhysicalMB * 1024 * 1024),
                    agentxx::util::formatSize(usage.memory.totalPhysicalMB * 1024 * 1024)
                ))
                | color(theme_.normalColor)
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
                    | color(notif.success ? theme_.hintColor : theme_.errorColor)
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
