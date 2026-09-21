#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/components/sidebar.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/plugin_ui_items.h"
#include "agentxx-client/io/tui/ui_components.h"
#include "agentxx/util/exception.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <filesystem>
#include <functional>
#include <markdown/state_diagram.hpp>
#include <memory>

namespace agentxx::client {

using namespace ftxui;

namespace {

ftxui::Element buildLogLine(const TUILogSink::Line& line, const TUITheme& theme) {
    ftxui::Color c = theme.normalColor;
    std::string  prefix;
    switch (line.level) {
        case utilxx_base::LogLevel::Trace:
            c      = theme.hintColor;
            prefix = "[T] ";
            break;
        case utilxx_base::LogLevel::Debug:
            c      = theme.hintColor;
            prefix = "[D] ";
            break;
        case utilxx_base::LogLevel::Info:
            c      = theme.accentColor;
            prefix = "[I] ";
            break;
        case utilxx_base::LogLevel::Warn:
            c      = theme.thinkingColor;
            prefix = "[W] ";
            break;
        case utilxx_base::LogLevel::Error:
            c      = theme.errorColor;
            prefix = "[E] ";
            break;
        case utilxx_base::LogLevel::Out:
            c      = theme.accentColor;
            prefix = "";
            break;
    }
    return paragraph(prefix + line.text) | color(c);
}

/// 渲染插件段落/面板 items 为一组子项 (通用, 零特化; 共享组件层收敛点)
/// - 全部组件 kind 走 [ui_components.h] 的唯一实现 (与面板/装饰/overlay/中断一致)
/// - 含可点区域的子项在 [ScrollItem::hits] 内登记局部坐标区域, 点击经
///   [Scrollable::hitTestItem] 定位子项后按区域判定 (不使用子项内 reflect)
/// - `form` 为宿主维护的表单状态 (控件值/勾选/选中/焦点); 为空时控件只做展示
/// - markdown 渲染器的生命周期追加到 [mdBuilders] (调用方持有, 与元素同存活)
static void appendPluginItems(
    const std::vector<agentxx::ui::Item>&                          items,
    std::string_view                                               plugin,
    std::string_view                                               ownerId,
    const agentxx::plugin::ClientUiRegistry*                       reg,
    const TUITheme&                                                theme,
    int                                                            avail,
    const std::function<bool(const std::string&, bool)>&           collapseExpanded,
    const agentxx::client::UiFormState*                            form,
    std::vector<ScrollItem>&                                       out,
    std::vector<std::vector<std::unique_ptr<markdown::DomBuilder>>>& mdBuilders
) {
    agentxx::client::UiRenderCtx rc;
    rc.theme            = &theme;
    rc.width            = avail;
    rc.indent           = 2;
    rc.plugin           = std::string{plugin};
    rc.ownerId          = std::string{ownerId};
    rc.registry         = reg;
    rc.form             = form;
    rc.collapseExpanded = collapseExpanded;

    agentxx::client::UiRenderResult res;
    agentxx::client::renderItems(items, rc, res);
    if (!res.builders.empty()) {
        mdBuilders.push_back(std::move(res.builders));
    }
    for (auto& row : res.rows) {
        ScrollItem item;
        item.element = std::move(row.element);
        item.hits    = std::move(row.regions);
        out.push_back(std::move(item));
    }
}

} // namespace

std::vector<ScrollItem> TUIClientAgentIO::renderLogWindow() {
    if (!logSink_) {
        return {
            ScrollItem{text(tr("info.empty")) | theme_.dim(), false}
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
            ScrollItem{text(tr("info.empty")) | theme_.dim(), false}
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

    std::vector<ScrollItem> items;
    auto                    pushPlain = [&items](ftxui::Element el) {
        items.push_back(ScrollItem{std::move(el), false});
    };

    // 插件 Info 段落内容区的可用宽度 (已扣除侧边栏内边距与滚动条占位)
    int avail = 0;
    if (sidebar_) {
        if (auto* scroll = sidebar_->contentScrollable(); scroll != nullptr) {
            avail = scroll->contentWidth();
        }
    }
    if (avail <= 0) {
        avail = sidebar_ ? sidebar_->width() - 4 : 40;
    }

    // Plan 段落已拆分至 agentxx_planning 插件 client 侧 (经 register_info_section
    // 注入, 见 agent/plugins/agentxx_planning), TUI 不再硬编码渲染

    // 插件扩展的 Info 段落 (插件经 register_info_section 注入; UI 线程渲染,
    // 每帧从 client 插件注册表快照读取, 无需缓存):
    // - 段落在 Append 之后按注册顺序展示 (标题 + items, items schema 同面板)
    // - 若段落无内容项则跳过，避免仅显示孤立标题
    // - 含可点内容的子项登记可命中区域 (owner=段落 id), 点击经
    //   [handleSidebarRegionClick] 定位后派发 (滚动容器内不用 reflect 判定)
    if (auto mgr = pluginManager_) {
        auto reg = mgr->uiRegistrySnapshot();
        if (reg && !reg->infoSections.empty()) {
            for (const auto& sec : reg->infoSections) {
                auto collapseCb = [this, ownerId = sec.id](const std::string& id, bool defaultValue) {
                    return collapseExpanded(ownerId, id, defaultValue);
                };
                // 表单状态: 按最新描述初始化并保留用户已编辑的值
                auto& form = formFor(
                    sec.id,
                    sec.plugin,
                    agentxx::ui::parseItemList(sec.items)
                );
                std::vector<ScrollItem> secItems;
                appendPluginItems(
                    form.items,
                    sec.plugin,
                    sec.id,
                    reg.get(),
                    theme_,
                    avail,
                    collapseCb,
                    &form.state,
                    secItems,
                    sidebarMdBuilders_
                );
                if (secItems.empty()) {
                    continue;
                }
                if (!sec.title.empty()) {
                    pushPlain(text(sec.title) | color(theme_.accentColor));
                }
                items.insert(
                    items.end(),
                    std::make_move_iterator(secItems.begin()),
                    std::make_move_iterator(secItems.end())
                );
                pushPlain(text(" "));
            }
        }
    }

    Elements elements;

    // 已加载组件 (Plugin/Memory/Skill/MCP) 展示:
    // - CodeGraph 索引状态与系统资源占用由对应插件经 register_info_section
    //   注入本 Info 栏 (见下方 "插件扩展 Info 段落"), TUI 不再单独渲染
    // - Failed 组 [view] 按钮命中经 shellHits_ 登记 (帧首由主渲染器清空):
    //   无失败项时该按钮不渲染, 因此不会占用任何点击区域
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
                                     utilxx_base::getFileName(notif.name),
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
        // 与 "| [view]" 按钮 (点击弹窗查看失败详情; 命中登记到 shellHits_,
        // 点击经 TUIClientAgentIO::handleShellHit 分发)
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
                shellHits_.add(
                    text(tr("info.viewFailed")) | bgcolor(theme_.buttonBgColor)
                        | color(theme_.buttonTextColor),
                    std::string{kFailedViewHitId}
                ),
            }));
        }

        elements.push_back(vbox(std::move(appendEls)));
    }

    if (elements.empty() && items.empty()) {
        pushPlain(text(tr("info.empty")) | color(theme_.hintColor));
    }

    for (auto& el : elements) {
        pushPlain(std::move(el));
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
    elements.push_back(
        hbox({
            text(fmt::format("{} ", utilxx_base::getFileName(kCwd))),
            filler(),
            text(kCwd) | xflex_shrink,
        })
        | color(theme_.hintColor)
    );

    // 程序名、版本、连接的服务端类型
    elements.push_back(
        hbox({
            text(fmt::format("Agentxx {} ", kAgentxxVersion)),
            filler(),
            text(
                ctx_.remoteUrl.empty() ? tr("info.innerServer") : trf("info.remote", ctx_.remoteUrl)
            ) | xflex_shrink,
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

    // [Menu] 按钮: 登记到 shell 级命中表 (未渲染时不占点击区域; 点击经
    // handleShellHit 打开日志菜单弹窗)
    row.push_back(shellHits_.add(
        text(tr("footer.menu")) | bgcolor(theme_.buttonBgColor) | color(theme_.buttonTextColor),
        std::string{kLogsMenuHitId}
    ));

    return hbox(std::move(row));
}

} // namespace agentxx::client
