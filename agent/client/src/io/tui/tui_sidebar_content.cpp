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
        } else if (kind == "button") {
            // 通用按钮 (如 Plan Graph 弹窗触发): 渲染为带背景的标签
            const auto label = it.value("label", it.value("text", std::string{"Button"}));
            // 普通按钮样式 (无点击捕获时仅视觉区分)
            push(
                text(" " + label + " ") | bgcolor(theme.buttonBgColor)
                | color(theme.buttonTextColor) | bold
            );
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

std::vector<ScrollItem> TUIClientAgentIO::renderInfoSidebar() {
    const auto& st = *ctx_.frameState;

    Elements elements;

    // Plan 段落已拆分至 agentxx_planning 插件 client 侧 (经 register_info_section
    // 注入, 见 agent/plugins/agentxx_planning), TUI 不再硬编码渲染

    // 插件扩展的 Info 段落 (插件经 register_info_section 注入; UI 线程渲染,
    // 每帧从 client 插件注册表快照读取, 无需缓存):
    // - 段落在 Append 之后按注册顺序展示 (标题 + items, items schema 同面板)
    // - 若段落无内容项则跳过，避免仅显示孤立标题
    // - Plan Graph 按钮特殊处理: 渲染为可点击按钮，点击弹窗状态图
    planGraphButtonBox_ = ftxui::Box{0, -1, 0, -1};
    planGraphMermaid_.clear();
    if (auto mgr = pluginManager_) {
        auto reg = mgr->uiRegistrySnapshot();
        if (reg && !reg->infoSections.empty()) {
            for (const auto& sec : reg->infoSections) {
                if (sec.id == "agentxx_planning.plan") {
                    // Plan 定制渲染: Graph(按钮弹窗) / Todo / Note 三段式
                    // (参考剥离前的 renderPlanningInfo: Plan 标题 + Graph 按钮 + todos + notes)
                    Elements secEls;
                    if (!sec.title.empty()) {
                        // 标题行: Plan + Graph 按钮 (若有 roadmap)
                        Elements titleRow;
                        titleRow.push_back(text(sec.title) | color(theme_.accentColor));
                        // 从 items 中提取 Graph 的 mermaid (button kind)
                        std::string graphMermaid;
                        for (const auto& it : sec.items) {
                            if (it.is_object() && it.value("kind", std::string{}) == "button") {
                                graphMermaid = it.value("mermaid", std::string{});
                                break;
                            }
                        }
                        if (!graphMermaid.empty()) {
                            titleRow.push_back(text(" "));
                            titleRow.push_back(
                                text(" Graph ") | bgcolor(theme_.buttonBgColor)
                                | color(theme_.buttonTextColor) | reflect(planGraphButtonBox_)
                            );
                            planGraphMermaid_ = graphMermaid;
                        }
                        secEls.push_back(hbox(std::move(titleRow)));
                    }
                    // 剩余 items: 按通用渲染 but 跳过已处理的 button (已在标题行渲染)
                    bool isFirstGraphButtonSkipped = false;
                    for (const auto& it : sec.items) {
                        if (!it.is_object()) {
                            continue;
                        }
                        const auto kind = it.value("kind", std::string{"text"});
                        if (kind == "button" && !isFirstGraphButtonSkipped) {
                            // 首个 Graph 按钮已在标题行渲染，跳过避免重复
                            isFirstGraphButtonSkipped = true;
                            continue;
                        }
                        // Graph 段的 "Graph:" title 已在按钮行体现, 跳过重复标题?
                        // 保留 Todo/Note 的 title 文本
                        Elements       tmp;
                        neograph::json arr = neograph::json::array();
                        arr.push_back(it);
                        appendPluginItems(arr, theme_, tmp);
                        for (auto& el : tmp) {
                            secEls.push_back(std::move(el));
                        }
                    }
                    if (secEls.empty()) {
                        continue;
                    }
                    elements.push_back(vbox(std::move(secEls)));
                    elements.push_back(text(" "));
                } else {
                    Elements secItems;
                    // Info 栏段落列表项按 Append 段样式 ("|  xxx") 展示
                    appendPluginItems(sec.items, theme_, secItems);
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
    }

    // 已加载组件 (Plugin/Memory/Skill/MCP) 展示:
    // - CodeGraph 索引状态与系统资源占用由对应插件经 register_info_section
    //   注入本 Info 栏 (见下方 "插件扩展 Info 段落"), TUI 不再单独渲染
    // - 本帧渲染前先清空 Failed 组 [view] 按钮命中区域 (Append 段未渲染/
    //   无失败项时防止残留旧区域误触); 按钮渲染时经 reflect 重新填充
    failedViewButtonBox_ = ftxui::Box{0, -1, 0, -1};
    if (!st.appendComponents.empty()) {
        Elements appendEls;
        appendEls.push_back(text("Append") | color(theme_.accentColor));

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
                hbox({text("|- "), text(fmt::format("Failed: {}", failedCount))})
                | color(theme_.errorColor)
            );
            appendEls.push_back(hbox({
                text("|  ") | color(theme_.hintColor),
                text(" [view] ") | bgcolor(theme_.buttonBgColor) | color(theme_.buttonTextColor)
                    | reflect(failedViewButtonBox_),
            }));
        }

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

    auto menuBtn = text(" Menu ") | bgcolor(theme_.buttonBgColor) | color(theme_.buttonTextColor)
                   | reflect(contextButtonBox_);
    row.push_back(menuBtn);

    return hbox(std::move(row));
}
