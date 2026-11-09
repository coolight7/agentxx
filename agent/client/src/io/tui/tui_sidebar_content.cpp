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

/// 已完成条目的单字符标记
///
/// 运行/排队段的条目没有标记 —— 状态已经由分组头 `|- 运行 n` / `|- 排队 n` 说明;
/// 「完成」段里要区分是怎么结束的, 但只用**一个**字符 (ASCII: 侧边栏窄时多字节
/// 字符会被截成半个), 颜色统一由调用方给。
///
/// - `return` 完成 `#`、失败 `x`、取消 `-`; 未知终态按完成处理
std::string_view asyncWorkMark(std::string_view state) {
    if (state == "failed") {
        return "x";
    }
    if (state == "canceled") {
        return "-";
    }
    return "#";
}

/// 渲染插件段落/面板 items 为一组子项 (通用, 零特化; 共享组件层统一入口)
/// - 全部组件 kind 走 [ui_components.h] 的唯一实现 (与面板/装饰/overlay/中断一致)
/// - 含可点区域的子项在 [ScrollItem::hits] 内登记局部坐标区域, 点击经
///   [Scrollable::hitTestItem] 定位子项后按区域判定 (不使用子项内 reflect)
/// - `form` 为主程序维护的表单状态 (控件值/勾选/选中/焦点); 为空时控件只做展示
/// - markdown 渲染器的生命周期追加到 [mdBuilders] (调用方持有, 与元素同存活)
///
/// - `return` 内容行数 (各行行数之和; 供可用尺寸上报, 与面板计数方式一致)
static size_t appendPluginItems(
    const std::vector<pluginxx::ui::Item>&                           items,
    std::string_view                                                 plugin,
    std::string_view                                                 ownerId,
    const agentxx::plugin::ClientUiRegistry*                         reg,
    const TUITheme&                                                  theme,
    int                                                              avail,
    const std::function<bool(const std::string&, bool)>&             collapseExpanded,
    const agentxx::client::UiFormState*                              form,
    std::vector<ScrollItem>&                                         out,
    std::vector<std::vector<std::unique_ptr<markdown::DomBuilder>>>& mdBuilders
) {
    agentxx::client::UiRenderCtx rc;
    rc.theme            = &theme;
    rc.width            = avail;
    rc.indent           = 0;
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
    size_t lines = 0;
    for (auto& row : res.rows) {
        lines += std::max<size_t>(1, row.lines);
        ScrollItem item;
        item.element = std::move(row.element);
        item.hits    = std::move(row.regions);
        out.push_back(std::move(item));
    }
    return lines;
}

} // namespace

std::vector<ScrollItem> TUIClientAgentIO::renderLogWindow() {
    if (!logSink_) {
        return {
            ScrollItem{text(tr("info.empty")) | theme_.dim(), false}
        };
    }
    // 仅当日志内容有变化时才重新 snapshot + 重建缓存:
    // 日志行可能多达 512 行, 每帧整份拷贝字符串是浪费 (日志 tab 打开时每帧渲染都触发)
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

    // 当前会话标题 (信息侧边栏第一段): 标题来自会话库 meta.title
    // (首条用户消息预览或用户改名), 由服务端随 WireContextStats.sessionTitle 下发
    // (见 TUIClientAgentIO::onContextStats); 尚未取得标题时显式提示"未命名",
    // 避免空白首行看起来像渲染缺失
    {
        Elements sessionEls;
        sessionEls.push_back(text(tr("info.session")) | color(theme_.accentColor));
        if (st.sessionTitle.empty()) {
            sessionEls.push_back(
                hbox({text("|- "), text(tr("info.sessionUntitled"))}) | color(theme_.hintColor)
            );
        } else {
            // 长标题按可用宽度折行 (不用 text: 侧边栏较窄时标题会直接被裁掉)
            sessionEls.push_back(paragraph(fmt::format("|- {}", st.sessionTitle))
                                 | color(theme_.normalColor));
        }
        pushPlain(vbox(std::move(sessionEls)));
        pushPlain(text(""));
    }

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
                auto collapseCb
                    = [this, ownerId = sec.id](const std::string& id, bool defaultValue) {
                          return collapseExpanded(ownerId, id, defaultValue);
                      };
                // 表单状态: 按最新描述初始化并保留用户已编辑的值
                auto& form = formFor(sec.id, sec.plugin, pluginxx::ui::parseBlocks(sec.items));
                std::vector<ScrollItem> secItems;
                const size_t            secLines = appendPluginItems(
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
                // 上报可用尺寸 (值变化时主程序投递 UI_LAYOUT 事件; 高度按内容行数,
                // 与面板计数方式一致 —— 插件据此决定是否折叠/分页)
                if (mgr) {
                    mgr->reportRegionSize(sec.id, avail, static_cast<int>(secLines));
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

    // Context 段: 本会话已消耗的 token 用量 (累计值, 与上方"上下文占用"不同 ——
    // 这里只增不减)。显示在 Append 段之上, 数据来自服务端推送的 WireContextStats
    // (见 TUIClientAgentIO::onContextStats)。
    //
    // - 已消耗 = 输入 + 输出 合计; 输入含命中缓存与写入缓存的输入
    // - 缓存命中率 = 命中缓存的输入 / 输入 (输入为 0 时不显示该行)
    // - 服务端尚未推送过用量 (老服务端/新会话) 时只显示一行"( 无数据 )",
    //   不显示一串 0, 避免看起来像是统计归零
    {
        Elements ctxEls;
        ctxEls.push_back(text(tr("info.context")) | color(theme_.accentColor));
        if (!st.sessionUsageKnown) {
            ctxEls.push_back(
                hbox({text("|- "), text(tr("info.contextEmpty"))}) | color(theme_.hintColor)
            );
        } else {
            // 数字按 K/M 折算 (token 数普遍到万级, 完整数字会把侧边栏撑宽)
            auto usageLine = [&](std::string_view key, uint64_t value) {
                ctxEls.push_back(
                    hbox({
                        text("|- "),
                        text(trf(key, utilxx_base::formatSize(value))),
                    })
                    | color(theme_.normalColor)
                );
            };
            usageLine("info.usedTokens", st.sessionTotalTokens);
            usageLine("info.cacheTokens", st.sessionCachedPromptTokens);
            usageLine("info.inputTokens", st.sessionPromptTokens);
            usageLine("info.outputTokens", st.sessionCompletionTokens);
            // 输入为 0 时命中率没有意义 (0/0), 该行不显示
            if (st.sessionPromptTokens > 0) {
                const double hitRate = 100.0 * static_cast<double>(st.sessionCachedPromptTokens)
                                       / static_cast<double>(st.sessionPromptTokens);
                ctxEls.push_back(
                    hbox({
                        text("|- "),
                        text(trf("info.cacheHitRate", hitRate)),
                    })
                    | color(theme_.normalColor)
                );
            }
        }
        elements.push_back(vbox(std::move(ctxEls)));
        elements.push_back(text(""));
    }

    // 后台任务段: 本会话创建的后台任务, 按 运行 / 排队 / 完成 分三段
    // - 数据来自服务端推送的 WireContextStats.asyncWorks (见 onContextStats);
    //   服务端在任务状态变化后主动重推 (运行期间每秒一次, 已跑时长跟着走)
    // - 状态已由分组表达, 条目前不再带状态标记; 只有「完成」段的条目带一个单字符
    //   标记 (见 [asyncWorkMark])
    // - 只展示状态与已跑时长, 不提供取消入口 (取消走模型工具 `agentxx_async_cancel`)
    // - 空数组时显示"( 无 )": 老服务端不带该字段, 或本会话确实没有任务
    {
        Elements workEls;
        workEls.push_back(text(tr("info.asyncWorks")) | color(theme_.accentColor));
        if (st.asyncWorks.empty()) {
            workEls.push_back(
                hbox({text("|- "), text(tr("info.asyncWorksEmpty"))}) | color(theme_.hintColor)
            );
        } else {
            // 已跑时长按 mm:ss 显示 (超过一小时按 h:mm:ss)
            auto elapsedText = [](int64_t ms) -> std::string {
                const int64_t total = std::max<int64_t>(0, ms) / 1000;
                const int64_t hours = total / 3600;
                const int64_t mins  = (total % 3600) / 60;
                const int64_t secs  = total % 60;
                if (hours > 0) {
                    return fmt::format("{}:{:02d}:{:02d}", hours, mins, secs);
                }
                return fmt::format("{:02d}:{:02d}", mins, secs);
            };
            // 分组: 0 = 运行, 1 = 排队, 2 = 完成 (done/failed/canceled)
            auto groupOf = [](std::string_view state) -> int {
                if (state == "running") {
                    return 0;
                }
                if (state == "queued") {
                    return 1;
                }
                return 2;
            };
            size_t counts[3] = {0, 0, 0};
            for (const auto& work : st.asyncWorks) {
                ++counts[groupOf(work.state)];
            }
            static constexpr std::string_view kGroupTitleKeys[3] = {
                "info.asyncWorkRunning",
                "info.asyncWorkQueued",
                "info.asyncWorkDone"
            };
            for (int group = 0; group < 3; ++group) {
                // 分组头恒显示 (计数为 0 也是信息), 颜色与原汇总行一致
                workEls.push_back(
                    hbox({text("|- "), text(trf(kGroupTitleKeys[group], counts[group]))})
                    | color(theme_.normalColor)
                );
                for (const auto& work : st.asyncWorks) {
                    if (groupOf(work.state) != group) {
                        continue;
                    }
                    std::string line{"|- "};
                    if (group == 2) {
                        // 完成段: 单字符标记区分 完成/失败/取消
                        line += asyncWorkMark(work.state);
                        line += ' ';
                    }
                    line += fmt::format("{} {}", work.id, work.name);
                    // 排队中不显示时长 (还没开始跑), 也不显示排队位置 —— 排队段的
                    // 条目顺序就是先后顺序
                    if (work.elapsedMs > 0) {
                        line += " " + elapsedText(work.elapsedMs);
                    }
                    // 所有列表项统一弱化色 (分组头已区分状态, 条目不再用状态色)
                    workEls.push_back(text(std::move(line)) | color(theme_.hintColor));
                }
            }
        }
        elements.push_back(vbox(std::move(workEls)));
        elements.push_back(text(""));
    }

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
        elements.push_back(text(""));
    }

    // 补充提示: 连 Context 段都没有内容时提示 (Context 段恒渲染, 因此当前只在
    // 未来把该段改为条件渲染后才会走到这里)
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
            // UTF-8 路径 (Windows 下 path::string() 为本地代码页, 中文目录会乱码)
            return utilxx_base::pathToUtf8Generic(std::filesystem::current_path());
        },
        [](std::string) -> std::string {
            return std::string(tr("info.workDirUnknown"));
        }
    );
    // 授权按钮: 非完全授权显示 "[ 询问授权 ]" (点击切换为完全授权),
    // 完全授权显示 "[ 完全授权 ]" (点击恢复询问);
    // 命中登记到 shell 级命中表, 点击经 handleShellHit → toggleFullAuth 处理
    // (状态本身由 agent 侧权限中间件持有, 客户端只负责展示与请求切换)
    const bool fullAuth = (ctx_.frameState != nullptr) && ctx_.frameState->fullAuthorized;
    elements.push_back(
        hbox({
            text(fmt::format("{} ", utilxx_base::getFileName(kCwd))),
            shellHits_.add(
                fullAuth
                    ? (text(std::string(tr("info.authFull"))) | bgcolor(theme_.buttonActiveBgColor)
                       | color(theme_.buttonActiveTextColor))
                    : (text(std::string(tr("info.authAsk"))) | bgcolor(theme_.buttonBgColor)
                       | color(theme_.buttonTextColor)),
                std::string{kAuthToggleHitId}
            ),
            text(" "),
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

    // 发现新版本时的提示行 (启动更新检查结果; 点击复制发布页链接):
    // 未发现更新时不渲染, 因此不占用任何点击区域
    if (ctx_.frameState != nullptr && !ctx_.frameState->availableUpdateTag.empty()) {
        elements.push_back(hbox({
            shellHits_.add(
                text(trf("info.updateNotice", ctx_.frameState->availableUpdateTag))
                    | bgcolor(theme_.buttonBgColor) | color(theme_.buttonTextColor),
                std::string{kUpdateNoticeHitId}
            ),
            filler(),
            text(tr("info.updateHint")) | color(theme_.hintColor) | xflex_shrink,
        }));
    }

    return vbox(std::move(elements));
}

ftxui::Element TUIClientAgentIO::renderLogSidebarFooter() {
    const auto& st = *ctx_.frameState;

    Elements elements;

    // 第一行: 当前会话 session-id (排查问题时需要知道日志属于哪个会话);
    // 侧边栏宽度有限, 超宽时由 xflex_shrink 压缩
    const std::string sessionId = currentSessionId();
    if (!sessionId.empty()) {
        elements.push_back(
            shellHits_.add(
                text(trf("info.sessionId", sessionId)) | xflex_shrink,
                std::string{kLogsSessionIdHitId}
            )
            | color(theme_.hintColor)
        );
    }

    // 第二行: 当前执行节点 (空闲时显示 idle) + [Menu] 按钮
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

    elements.push_back(hbox(std::move(row)));
    return vbox(std::move(elements));
}

} // namespace agentxx::client
