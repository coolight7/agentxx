#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/components/sidebar.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
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

/// 渲染插件段落/面板 items JSON 元素 (kind: text/progress/badge/separator;
/// schema 见 client_plugin_api.h register_panel/register_info_section)
/// - text 项支持 role 指定样式: "title"=高亮强调 / "normal"=普通文本(默认) /
///   "hint"=减淡提示
static void
    appendPluginItems(const neograph::json& items, const TUITheme& theme, ftxui::Elements& out) {
    if (!items.is_array()) {
        return;
    }
    /// 行内元素 → 按列表样式加 "|  " 前缀后入列
    auto push = [&](ftxui::Element el) {
        out.push_back(std::move(el));
    };
    for (const auto& it : items) {
        if (!it.is_object()) {
            continue;
        }
        const auto kind = it.value("kind", std::string{"text"});
        if (kind == "text") {
            const auto role = it.value("role", std::string{"normal"});
            const auto txt  = paragraph(it.value("text", std::string{}));
            if (role == "title") {
                push(txt | color(theme.accentColor) | bold);
            } else if (role == "hint") {
                push(txt | color(theme.hintColor));
            } else {
                push(txt | color(theme.normalColor));
            }
        } else if (kind == "progress") {
            const double v      = it.value("value", 0.0);
            const int    w      = 10;
            const int    filled = static_cast<int>(v * w);
            std::string  bar;
            bar.reserve(w);
            for (int i = 0; i < w; ++i) {
                bar += (i < filled) ? '#' : '-';
            }
            push(hbox({
                text("[" + bar + "]") | color(theme.accentColor),
                text(fmt::format(" {}%", static_cast<int>(v * 100))) | color(theme.hintColor),
            }));
        } else if (kind == "badge") {
            push(text("● " + it.value("text", std::string{})) | color(theme.accentColor));
        } else if (kind == "separator") {
            push(text("─") | color(theme.hintColor) | dim);
        }
    }
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
        auto lines           = logSink_->snapshot();
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
        if (m.role == TUIMessage::Role::Tool && m.tool
            && m.tool->toolName == "agentxx_planning_write") {
            plan = st.messages[i - 1].get();
            break;
        }
    }
    if (!plan) {
        // 无 plan: 清空状态图按钮命中区域, 避免残留旧区域导致误触
        planDiagramButtonBox_ = ftxui::Box{0, -1, 0, -1};
        return std::nullopt;
    }

    // 解析缓存: plan 消息被修改时 (mutableMessage 总是复制 → 指针变化)
    // 或文本长度变化时重新解析, 避免每帧重复解析 planning 参数 JSON
    const bool planFinished = plan->tool && plan->tool->toolFinished;
    if (planCacheMsgPtr_ != plan || planCacheTextLen_ != plan->text.size()
        || planCacheFinished_ != planFinished) {
        planCacheMsgPtr_   = plan;
        planCacheTextLen_  = plan->text.size();
        planCacheFinished_ = planFinished;
        planCacheArgs_     = neograph::json::array();
        // 解析失败保持 planCacheValid_ = false, 界面显示占位内容而非异常中断渲染
        planCacheValid_ = agentxx::util::catchError<bool>(
            [&]() -> bool {
                planCacheArgs_ = neograph::json::parse(plan->text);
                return true;
            },
            [](std::string) -> bool {
                return false;
            }
        );
    }
    if (!planCacheValid_) {
        planDiagramButtonBox_ = ftxui::Box{0, -1, 0, -1};
        return std::nullopt;
    }
    const auto& args = planCacheArgs_;

    Elements lines;
    Elements title;
    title.push_back(text("Plan") | color(theme_.accentColor));
    // Roadmap 状态图按钮: 点击弹窗查看完整状态图 (PlanDiagramOverlay)
    // 状态图渲染成本高 (解析 + 分层布局), 侧边栏常驻显示仅保留按钮,
    // 仅在用户点击时才在弹窗中渲染
    const auto roadmap = args.value("roadmap", std::string{});
    if (!planFinished) {
        title.push_back(text(" Planning...") | color(theme_.hintColor));
    } else if (!roadmap.empty()) {
        title.push_back(text(" "));
        title.push_back(
            text(" Graph ") | bgcolor(theme_.buttonBgColor) | color(theme_.buttonTextColor)
            | reflect(planDiagramButtonBox_)
        );
    } else {
        // 无 roadmap: 清空按钮命中区域
        planDiagramButtonBox_ = ftxui::Box{0, -1, 0, -1};
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
                paragraph(content) | color(c) | xflex_shrink,
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

    if (auto planning = renderPlanningInfo()) {
        elements.push_back(std::move(*planning));
        elements.push_back(text(" "));
    }

    // 插件扩展的 Info 段落 (插件经 register_info_section 注入; UI 线程渲染,
    // 每帧从 client 插件注册表快照读取, 无需缓存):
    // - 段落在 Append 之后按注册顺序展示 (标题 + items, items schema 同面板)
    if (auto mgr = pluginManager_) {
        auto reg = mgr->uiRegistrySnapshot();
        if (reg && !reg->infoSections.empty()) {
            for (const auto& sec : reg->infoSections) {
                Elements secEls;
                if (!sec.title.empty()) {
                    secEls.push_back(text(sec.title) | color(theme_.accentColor));
                }
                // Info 栏段落列表项按 Append 段样式 ("|  xxx") 展示
                appendPluginItems(sec.items, theme_, secEls);
                if (!secEls.empty()) {
                    elements.push_back(vbox(std::move(secEls)));
                    elements.push_back(text(" "));
                }
            }
        }
    }

    // 已加载组件 (Plugin/Memory/Skill/MCP) 展示:
    // - CodeGraph 索引状态与系统资源占用由对应插件经 register_info_section
    //   注入本 Info 栏 (见下方 "插件扩展 Info 段落"), TUI 不再单独渲染
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
                    (splitName ? hbox({text(
                         fmt::format("|  {}·{}", agentxx::util::getFileName(notif.name), notif.name)
                     )})
                               : hbox({text("|  "), text(notif.name) | xflex_shrink}))
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
        appendGroup("Plugins", agentxx::agent::AppendComponentNotification::Type::Plugin, false);

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

    // 工作目录在进程运行期间固定: 首次调用时计算并缓存到静态变量,
    // 避免 Info tab 常驻时每帧执行 current_path() 系统调用
    static const std::string kCwd = agentxx::util::catchError<std::string>(
        []() -> std::string {
            return std::filesystem::current_path().string();
        },
        [](std::string) -> std::string {
            return "[Unknown Work Dir]";
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
        row.push_back(text("idle") | color(theme_.hintColor));
    } else {
        row.push_back(text(fmt::format("> {}", st.currentNodeName)) | color(theme_.accentColor));
    }
    row.push_back(filler());

    auto ctxBtn = text(" LLM Context ") | bgcolor(theme_.buttonBgColor)
                  | color(theme_.buttonTextColor) | reflect(contextButtonBox_);
    row.push_back(ctxBtn);

    return hbox(std::move(row));
}
