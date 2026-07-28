#include "agentxx-client/io/tui/agent_tui.h"
#include <filesystem>

using namespace ftxui;

ftxui::Element AgentTUI::renderSidebar() {
    tabBoxes_.assign(sidebarTabs_.size(), ftxui::Box{});
    Elements tabs;
    for (size_t i = 0; i < sidebarTabs_.size(); ++i) {
        auto label = text(" " + sidebarTabs_[i].title + " ");
        if (static_cast<int>(i) == activeTabIndex_) {
            label = label | bgcolor(theme_.buttonActiveBgColor)
                    | color(theme_.buttonActiveTextColor) | bold;
        } else {
            label = label | color(theme_.hintColor);
        }
        tabs.push_back(label | reflect(tabBoxes_[i]));
    }
    auto tabBar = hbox(std::move(tabs)) | xframe;

    Element content = text(" ");
    if (activeTabIndex_ >= 0 && activeTabIndex_ < static_cast<int>(sidebarTabs_.size())) {
        content = sidebarTabs_[activeTabIndex_].render();
    }

    return vbox({
               tabBar,
               text(" "),
               hbox({text(" "), sidebarScrollable_->Render() | flex, text(" ")}) | flex,
           })
           | size(WIDTH, LESS_THAN, 56) | size(WIDTH, GREATER_THAN, 28)
           | bgcolor(theme_.blockColor);
}

ftxui::Element AgentTUI::renderLogWindow() {
    auto     lines = logSink_ ? logSink_->snapshot() : std::vector<TUILogSink::Line>{};
    Elements elements;
    for (const auto& line : lines) {
        ftxui::Color c = theme_.assistantColor;
        std::string  prefix;
        switch (line.level) {
            case agentxx::util::LogLevel::Debug:
                c      = theme_.hintColor;
                prefix = "[D] ";
                break;
            case agentxx::util::LogLevel::Info:
                c      = theme_.statusColor;
                prefix = "[I] ";
                break;
            case agentxx::util::LogLevel::Warn:
                c      = theme_.thinkingColor;
                prefix = "[W] ";
                break;
            case agentxx::util::LogLevel::Error:
                c      = theme_.systemColor;
                prefix = "[E] ";
                break;
            case agentxx::util::LogLevel::Out:
                c      = theme_.assistantColor;
                prefix = "";
                break;
        }
        elements.push_back(paragraph(prefix + line.text) | color(c));
    }
    if (elements.empty()) {
        return text(" (no logs) ") | dim;
    }
    return vbox(std::move(elements));
}

void AgentTUI::addSidebarTab(
    std::string_view                id,
    std::string_view                title,
    std::function<ftxui::Element()> render
) {
    for (auto& tab : sidebarTabs_) {
        if (tab.id == id) {
            tab.title  = title;
            tab.render = std::move(render);
            return;
        }
    }
    sidebarTabs_.push_back(SidebarTab{std::string{id}, std::string{title}, std::move(render)});
    activeTabIndex_ = static_cast<int>(sidebarTabs_.size()) - 1;
}

void AgentTUI::removeSidebarTab(std::string_view id) {
    for (size_t i = 0; i < sidebarTabs_.size(); ++i) {
        if (sidebarTabs_[i].id == id) {
            sidebarTabs_.erase(sidebarTabs_.begin() + i);
            if (activeTabIndex_ >= static_cast<int>(sidebarTabs_.size())) {
                activeTabIndex_ = static_cast<int>(sidebarTabs_.size()) - 1;
            }
            return;
        }
    }
}

bool AgentTUI::hasSidebarTab(std::string_view id) const {
    for (const auto& tab : sidebarTabs_) {
        if (tab.id == id) {
            return true;
        }
    }
    return false;
}

void AgentTUI::toggleLogWindow() {
    if (hasSidebarTab(kLogTabId)) {
        removeSidebarTab(kLogTabId);
    } else {
        addSidebarTab(kLogTabId, "Logs", [this]() {
            return renderLogWindow();
        });
    }
    if (sidebarScrollable_) {
        sidebarScrollable_->setStickToBottom(true);
    }
}

bool AgentTUI::handleSidebarMouse(const ftxui::Mouse& mouse) {
    for (size_t i = 0; i < sidebarTabs_.size() && i < tabBoxes_.size(); ++i) {
        if (false == tabBoxes_[i].Contain(mouse.x, mouse.y)) {
            continue;
        }
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Released) {
            activeTabIndex_ = static_cast<int>(i);
            if (sidebarScrollable_) {
                sidebarScrollable_->setStickToBottom(true);
            }
            return true;
        }
        if (mouse.button == Mouse::Right && mouse.motion == Mouse::Released) {
            removeSidebarTab(sidebarTabs_[i].id);
            return true;
        }
    }
    return false;
}

bool AgentTUI::handleCollapsibleMouse(const ftxui::Mouse& mouse) {
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    for (size_t k = 0; k < collapsibleBoxes_.size() && k < collapsibleMsgIndices_.size(); ++k) {
        if (false == collapsibleBoxes_[k].Contain(mouse.x, mouse.y)) {
            continue;
        }
        const size_t mi = collapsibleMsgIndices_[k];
        if (mi < messages_.size()) {
            messages_[mi].collapsed = !messages_[mi].collapsed;
            return true;
        }
    }
    return false;
}

std::optional<ftxui::Element> AgentTUI::renderPlanningInfo() {
    // 取最新的 planning_write toolcall
    const Message* plan = nullptr;
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
        if (it->role == Message::Role::Tool && it->toolName == "planning_write") {
            plan = &(*it);
            break;
        }
    }
    if (plan == nullptr) {
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
    title.push_back(text("规划") | color(theme_.accentColor));
    if (!plan->toolFinished) {
        title.push_back(text("  规划中...") | color(theme_.hintColor) | dim);
    }
    lines.push_back(hbox(std::move(title)));

    // todos 清单 (state: pending/in_progress/completed/failed)
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
                icon = "[x]";
                c    = theme_.promptColor;
            } else if (state == "failed") {
                icon = "[!]";
                c    = theme_.systemColor;
            }
            lines.push_back(hbox({
                text(icon + " ") | color(c),
                paragraph(content) | color(c),
            }));
        }
    }

    const auto notes = args.value("notes", std::string{});
    if (!notes.empty()) {
        lines.push_back(text(""));
        lines.push_back(text("笔记") | color(theme_.accentColor));
        lines.push_back(paragraph(notes) | color(theme_.hintColor));
    }

    return vbox(std::move(lines));
}

ftxui::Element AgentTUI::renderInfoSidebar() {
    Elements elements;

    // 顶部: planning 特化渲染 (存在 planning_write toolcall 时)
    if (auto planning = renderPlanningInfo()) {
        elements.push_back(std::move(*planning) | flex | vscroll_indicator | yframe);
        elements.push_back(text(" "));
    }

    elements.push_back(filler());

    // 底部: 当前工作目录
    std::string cwd;
    try {
        cwd = std::filesystem::current_path().string();
    } catch (...) {
        cwd = "(Unknown Work Dir)";
    }
    elements.push_back(text(cwd) | color(theme_.hintColor));

    // 底部: Agentxx 版本 + 运行模式 (内置 / 远程 http[s]://ip:port)
    std::string mode;
    if (remoteUrl_.empty()) {
        mode = "内置服务";
    } else {
        mode = "" + remoteUrl_;
    }
    elements.push_back(
        hbox({
            text(fmt::format("Agentxx {}", kAgentxxVersion, mode)),
            filler(),
            text(mode),
        })
        | color(theme_.hintColor)
    );
    elements.push_back(text(" "));

    return vbox(std::move(elements));
}
