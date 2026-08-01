#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/util/string_util.h"
#include "ftxui/screen/terminal.hpp"
#include <algorithm>
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

    // 取当前激活 tab 的底部常驻内容 (渲染于滚动区之外, 不随主体滚动)
    std::function<ftxui::Element()> footerRender;
    if (activeTabIndex_ >= 0 && activeTabIndex_ < static_cast<int>(sidebarTabs_.size())) {
        footerRender = sidebarTabs_[activeTabIndex_].footer;
    }

    Elements layout;
    layout.push_back(tabBar);
    layout.push_back(text(" "));
    // 滚动主体: | flex 使其充满侧边栏可用宽度 (日志/信息 tab 统一占满)
    layout.push_back(hbox({text(" "), sidebarScrollable_->Render() | flex, text(" ")}) | flex);
    if (footerRender) {
        layout.push_back(hbox({text(" "), footerRender() | flex, text(" ")}) | xframe);
        layout.push_back(text(" "));
    }

    // 左侧拖拽手柄: 1 格宽的竖线 (取输入框背景色), 按住左键左右拖动可调整侧边栏宽度
    auto handle = separatorStyled(BorderStyle::LIGHT) | color(theme_.inputBgColor)
                  | reflect(sidebarHandleBox_);

    return hbox({
               handle,
               vbox(std::move(layout)) | flex,
           })
           | size(WIDTH, EQUAL, sidebarWidth_) | bgcolor(theme_.blockColor);
}

namespace {

// 构建单行日志元素 (按日志级别着色 + 前缀)
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

std::vector<ScrollItem> AgentTUI::renderLogWindow() {
    auto lines = logSink_ ? logSink_->snapshot() : std::vector<TUILogSink::Line>{};
    if (lines.empty()) {
        return {
            ScrollItem{text(" (no logs) ") | dim, false}
        };
    }
    // 日志仅追加: 按行索引缓存元素, 仅构建新增行 (避免每帧重建全部日志行)
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

void AgentTUI::addSidebarTab(
    std::string_view                         id,
    std::string_view                         title,
    std::function<std::vector<ScrollItem>()> render,
    std::function<ftxui::Element()>          footer
) {
    for (auto& tab : sidebarTabs_) {
        if (tab.id == id) {
            tab.title  = title;
            tab.render = std::move(render);
            tab.footer = std::move(footer);
            return;
        }
    }
    sidebarTabs_.push_back(SidebarTab{
        std::string{id},
        std::string{title},
        std::move(render),
        std::move(footer),
    });
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
        addSidebarTab(
            kLogTabId,
            "Logs",
            [this]() {
                return renderLogWindow();
            },
            [this]() {
                return renderLogSidebarFooter();
            }
        );
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

bool AgentTUI::handleSidebarResizeMouse(const ftxui::Mouse& mouse) {
    // 拖拽中: 消费所有鼠标事件, 依据相对位移调整宽度 (手柄左移 -> 变宽)
    if (sidebarResizing_) {
        if (mouse.motion == Mouse::Released) {
            sidebarResizing_ = false;
            return true;
        }
        const int newWidth = sidebarResizeStartWidth_ + (sidebarResizeStartX_ - mouse.x);
        // 上限同时受终端宽度约束, 避免侧边栏超出屏幕挤掉消息区
        const int screenW = ftxui::Terminal::Size().dimx;
        const int maxW    = std::min(kSidebarMaxWidth, std::max(kSidebarMinWidth, screenW - 10));
        sidebarWidth_     = std::clamp(newWidth, kSidebarMinWidth, maxW);
        return true;
    }
    // 在手柄上按下左键 -> 进入拖拽
    if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed
        && sidebarHandleBox_.Contain(mouse.x, mouse.y)) {
        sidebarResizing_         = true;
        sidebarResizeStartX_     = mouse.x;
        sidebarResizeStartWidth_ = sidebarWidth_;
        return true;
    }
    return false;
}

bool AgentTUI::handleCollapsibleMouse(const ftxui::Mouse& mouse) {
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    if (mouse.x < messagesAreaBox_.x_min || mouse.x > messagesAreaBox_.x_max) {
        return false;
    }
    for (size_t k = 0; k < collapsibleBoxes_.size() && k < collapsibleMsgIndices_.size(); ++k) {
        if (mouse.y < collapsibleBoxes_[k].y_min || mouse.y > collapsibleBoxes_[k].y_max) {
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
                icon = "[#]";
                c    = theme_.promptColor;
            } else if (state == "failed") {
                icon = "[!]";
                c    = theme_.errorColor;
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

std::vector<ScrollItem> AgentTUI::renderInfoSidebar() {
    Elements elements;

    // planning 特化渲染 (存在 planning_write toolcall 时)
    if (auto planning = renderPlanningInfo()) {
        elements.push_back(std::move(*planning));
        elements.push_back(text(" "));
    }

    // 附加加载模块 (MCP/Skill/Memory) 统计 + 明细
    if (false == appendComponents_.empty()) {
        Elements appendComponentsElements;

        appendComponentsElements.push_back(text("Append Components") | color(theme_.accentColor));

        // 输出一组: 先显示统计数量, 再逐行列出名称 (失败项用错误色标注)
        auto appendGroup = [&](std::string_view                                  label,
                               agentxx::agent::AppendComponentNotification::Type type,
                               bool                                              splitName) {
            size_t   count = 0;
            Elements elements;
            for (const auto& notif : appendComponents_) {
                if (notif.type != type) {
                    continue;
                }
                ++count;
                elements.push_back(
                    (splitName ? hbox({
                                     text(fmt::format(
                                         "│   {}·{}",
                                         agentxx::util::getFileName(notif.name),
                                         notif.name
                                     )),
                                 })
                               : hbox({
                                     text("│   "),
                                     text(notif.name),
                                 }))
                    | color(notif.success ? theme_.assistantColor : theme_.systemColor)
                );
            }
            if (count > 0) {
                appendComponentsElements.push_back(
                    hbox({
                        text("┣━ "),
                        text(fmt::format("{}: {}", label, count)),
                    })
                    | color(theme_.assistantColor)
                );
                appendComponentsElements.push_back(vbox(elements));
            }
        };
        appendGroup("Memory", agentxx::agent::AppendComponentNotification::Type::Memory, true);
        appendGroup("Skill", agentxx::agent::AppendComponentNotification::Type::Skill, true);
        appendGroup("MCP", agentxx::agent::AppendComponentNotification::Type::Mcp, false);

        elements.push_back(vbox(std::move(appendComponentsElements)));
    }

    if (elements.empty()) {
        elements.push_back(text("(暂无信息)") | color(theme_.hintColor));
    }

    // planning 与统计信息共同组成单一可滚动列表 (滚动由 sidebarScrollable_ 提供)
    std::vector<ScrollItem> items;
    items.reserve(elements.size());
    for (auto& el : elements) {
        items.push_back(ScrollItem{std::move(el), false});
    }
    return items;
}

ftxui::Element AgentTUI::renderInfoSidebarFooter() {
    Elements elements;

    // 当前工作目录
    std::string cwd;
    try {
        cwd = std::filesystem::current_path().string();
    } catch (...) {
        cwd = "(Unknown Work Dir)";
    }
    elements.push_back(text(cwd) | color(theme_.hintColor));

    // Agentxx 版本 + 运行模式 (内置 / 远程 http[s]://ip:port)
    std::string mode;
    if (remoteUrl_.empty()) {
        mode = "内置服务";
    } else {
        mode = remoteUrl_;
    }
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

ftxui::Element AgentTUI::renderLogSidebarFooter() {
    Elements row;

    if (currentNodeName_.empty()) {
        row.push_back(text(" idle") | color(theme_.hintColor));
    } else {
        row.push_back(text(" > " + currentNodeName_) | color(theme_.accentColor));
    }

    row.push_back(filler());

    auto ctxBtn = text(" 上下文 ") | bgcolor(theme_.buttonBgColor) | color(theme_.buttonTextColor)
                  | reflect(contextButtonBox_);
    row.push_back(ctxBtn);

    return hbox(std::move(row));
}
