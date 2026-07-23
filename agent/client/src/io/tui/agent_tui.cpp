#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/util/string_util.h"
#include "asio/as_tuple.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/terminal.hpp"
#include "neograph/graph/cancel.h"
#include <algorithm>

using namespace ftxui;

namespace {
/// 取首行并按 UTF-8 字符数截断为单行预览 (仅用于折叠态显示)
std::string oneLinePreview(const std::string& s, size_t max = 60) {
    const auto  nl   = s.find('\n');
    std::string line = (nl == std::string::npos) ? s : s.substr(0, nl);
    const auto  idx  = agentxx::util::findIndexByUtf8Length(line, max);
    // findIndexByUtf8Length 在字符数不足 targetLen 时返回 0
    if (idx > 0 && idx < line.size()) {
        line.resize(idx);
        line += "...";
    }
    return line;
}
} // namespace

AgentTUI::AgentTUI(asio::any_io_executor                         ex,
                   std::shared_ptr<agentxx::agent::AgentContext> agentContext,
                   std::string                                   threadId,
                   TUITheme                                      theme) :
    agentContext_(std::move(agentContext)),
    theme_(theme),
    threadId_(std::move(threadId)),
    inputChannel_(std::make_shared<LineChannel>(ex, 64)),
    permissionChannel_(std::make_shared<BoolChannel>(ex, 4)),
    logSink_(std::make_shared<TUILogSink>()) {}

AgentTUI::~AgentTUI() {
    stop();
}

void AgentTUI::postRedraw() {
    if (screen_) {
        screen_->PostEvent(Event::Custom);
    }
}

void AgentTUI::start() {
    running_ = true;
    if (logSink_) {
        // 使用 weak_ptr 避免日志线程在 TUI 析构后回调到已销毁对象
        std::weak_ptr<AgentTUI> weakSelf = shared_from_this();
        logSink_->setOnNewLog([weakSelf]() {
            if (auto self = weakSelf.lock()) {
                self->postRedraw();
            }
        });
        agentxx::util::LogDispatcher::instance().addSink(logSink_);
    }

    uiThread_ = std::thread([this]() {
        auto screen = ScreenInteractive::Fullscreen();
        screen_     = &screen;

        auto input_option      = InputOption();
        input_option.multiline = true;
        // 覆盖默认 transform: 默认会在聚焦时加 inverted (反转前景/背景), 会把
        // 输入框背景反成白色; 这里仅保留占位符弱化, 颜色改由外部 bgcolor/color 控制
        input_option.transform = [](InputState state) {
            if (state.is_placeholder) {
                state.element |= dim;
            }
            return state.element;
        };
        // multiline 模式: Enter 插入换行 (不发送); 发送由事件处理器识别 Alt+Enter
        // 完成. 不用 on_enter (其会在每次 Enter 含粘贴 \n 时触发).
        input_option.on_enter = nullptr;

        auto input
            = Input(&inputText_, "Type a message... (Enter=newline, Alt+Enter=send)", input_option);

        auto layout = Renderer(input, [&]() -> Element {
            std::lock_guard<std::mutex> lock(mutex_);

            // yframe 仅纵向滚动并约束宽度, 使 paragraph 能按屏幕宽度自动换行;
            // 若用 frame (含横向) 会把内容撑到自然宽度导致不换行
            auto messages = renderMessages() | flex | vscroll_indicator | yframe;

            // 状态指示器 (固定 3 字符宽度)
            Element indicator;
            if (pendingPermission_) {
                indicator = text(" ! ") | bgcolor(Color::Red) | color(Color::White) | bold | blink;
            } else if (isStreaming_) {
                indicator = text("   ") | bgcolor(theme_.accentColor) | blink;
            } else {
                indicator = text(" > ") | color(theme_.promptColor) | bold;
            }

            const int maxInputTotalLines = std::max(3, ftxui::Terminal::Size().dimy / 2);
            auto      input_bar          = hbox({
                text(" "), // 外边距
                vbox({
                    text(" "), // 上边距
                    hbox({
                        text("  "), // 内左边距
                        indicator,
                        text("  "), // 间距
                        input->Render() | color(theme_.inputTextColor) | flex,
                        text("  "), // 内右边距
                    }),
                    text(" "), // 下边距
                }) | bgcolor(theme_.inputBgColor)
                    | xflex | size(HEIGHT, GREATER_THAN, 3)
                    | size(HEIGHT, LESS_THAN, maxInputTotalLines),
                text(" "), // 外边距
            });

            auto main = vbox({
                messages,
                separator(),
                input_bar,
                renderStatusBar(),
            });

            Element body = main;
            if (false == sidebarTabs_.empty()) {
                body = hbox({
                    main | flex,
                    renderSidebar(),
                });
            }

            Element result = body;
            if (pendingPermission_.has_value()) {
                result = renderPermissionOverlay() | center;
            } else if (showModelSelector_) {
                result = renderModelSelectorOverlay() | center;
            }
            return result | bgcolor(theme_.backgroundColor);
        });

        auto event_handler = CatchEvent(layout, [&](Event event) -> bool {
            if (event == Event::CtrlC) {
                running_ = false;
                screen.Exit();
                return true;
            }

            std::lock_guard<std::mutex> lock(mutex_);

            if (pendingPermission_.has_value()) {
                if (event == Event::Character('y') || event == Event::Character('Y')) {
                    pendingPermission_.reset();
                    permissionChannel_->async_send(neograph_asio_error_code{},
                                                   true,
                                                   [](neograph_asio_error_code) {});
                    postRedraw();
                    return true;
                }
                if (event == Event::Character('n') || event == Event::Character('N')
                    || event == Event::Escape) {
                    pendingPermission_.reset();
                    permissionChannel_->async_send(neograph_asio_error_code{},
                                                   false,
                                                   [](neograph_asio_error_code) {});
                    postRedraw();
                    return true;
                }
                return true;
            }

            if (showModelSelector_) {
                if (event == Event::ArrowUp) {
                    if (selectedModelIndex_ > 0) {
                        --selectedModelIndex_;
                    }
                    postRedraw();
                    return true;
                }
                if (event == Event::ArrowDown) {
                    if (selectedModelIndex_ + 1 < static_cast<int>(modelNames_.size())) {
                        ++selectedModelIndex_;
                    }
                    postRedraw();
                    return true;
                }
                if (event == Event::Return) {
                    confirmModelSelection();
                    postRedraw();
                    return true;
                }
                if (event == Event::Escape) {
                    showModelSelector_ = false;
                    postRedraw();
                    return true;
                }
                return true;
            }

            // Alt+Enter → 发送消息 (普通 Enter 不拦截, 交给 Input 插入换行).
            // Alt+Enter 的标准终端编码为 ESC+Enter (\x1B\n / \x1B\r), 全终端可用.
            {
                const std::string& in     = event.input();
                const bool         isSend = (in == "\x1B\n" || in == "\x1B\r"); // Alt+Enter
                if (isSend) {
                    std::string text = inputText_;
                    // 去掉首尾换行符 (保留内部换行与缩进)
                    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                        text.pop_back();
                    }
                    size_t start = 0;
                    while (start < text.size() && (text[start] == '\n' || text[start] == '\r')) {
                        ++start;
                    }
                    if (start > 0) {
                        text = text.substr(start);
                    }
                    if (!text.empty()) {
                        // 先冲刷上一轮未提交的流式 token, 保证 user 消息始终插入在末尾
                        if (!currentToken_.empty()) {
                            messages_.push_back({currentTokenRole_, currentToken_});
                            if (currentTokenRole_ == Message::Role::Thinking) {
                                messages_.back().collapsed = true;
                            }
                            currentToken_.clear();
                        }
                        messages_.push_back({Message::Role::User, text});
                        inputText_.clear();
                        isStreaming_   = true;
                        stickToBottom_ = true;
                        inputChannel_->async_send(neograph_asio_error_code{},
                                                  std::move(text),
                                                  [](neograph_asio_error_code) {});
                    }
                    postRedraw();
                    return true;
                }
            }

            if (event == Event::F2) {
                openModelSelector();
                postRedraw();
                return true;
            }
            if (event == Event::F12) {
                toggleLogWindow();
                postRedraw();
                return true;
            }
            if (event.is_mouse()) {
                const auto& mouse = event.mouse();
                if (mouse.button == Mouse::WheelUp || mouse.button == Mouse::WheelDown) {
                    // 鼠标滚轮: 滚动消息列表; 滚到底部时重新吸附底部
                    const int last = focusBlockCount() - 1;
                    if (last >= 0) {
                        int cur  = stickToBottom_ ? last : scrollAnchorIndex_;
                        cur     += (mouse.button == Mouse::WheelUp) ? -1 : +1;
                        if (cur >= last) {
                            stickToBottom_ = true;
                        } else {
                            stickToBottom_     = false;
                            scrollAnchorIndex_ = std::max(0, cur);
                        }
                    }
                    postRedraw();
                    return true;
                }
                if (handleCollapsibleMouse(mouse)) {
                    postRedraw();
                    return true;
                }
                if (handleSidebarMouse(mouse)) {
                    postRedraw();
                    return true;
                }
                return false;
            }
            if (event == Event::Escape && isStreaming_) {
                cancelCurrentRun();
                postRedraw();
                return true;
            }
            return false;
        });

        screen.Loop(event_handler);
        screen_ = nullptr;
    });
}

void AgentTUI::stop() {
    if (logSink_) {
        agentxx::util::LogDispatcher::instance().removeSink(logSink_);
        logSink_->setOnNewLog(nullptr);
    }
    running_ = false;
    if (screen_) {
        screen_->Exit();
    }
    if (uiThread_.joinable()) {
        uiThread_.join();
    }
}

int AgentTUI::focusBlockCount() const {
    int n = static_cast<int>(messages_.size());
    if (isStreaming_ && !currentToken_.empty()) {
        ++n;
    }
    return n;
}

ftxui::Element AgentTUI::renderMessages() {
    // 计算滚动锚点: 吸附底部时聚焦最后一块 (yframe 会滚动到底部),
    // 否则聚焦 scrollAnchorIndex_ 指向的块, 视图保持稳定不随新消息跳动
    const int count    = focusBlockCount();
    int       focusIdx = -1;
    if (count > 0) {
        focusIdx = stickToBottom_ ? (count - 1) : std::clamp(scrollAnchorIndex_, 0, count - 1);
    }

    // 收集可折叠消息 (Thinking/Tool) 索引并重置其点击区域
    collapsibleMsgIndices_.clear();
    for (size_t i = 0; i < messages_.size(); ++i) {
        if (messages_[i].role == Message::Role::Thinking
            || messages_[i].role == Message::Role::Tool) {
            collapsibleMsgIndices_.push_back(i);
        }
    }
    collapsibleBoxes_.assign(collapsibleMsgIndices_.size(), ftxui::Box{});

    Elements elements;
    int      idx                = 0;
    int      collapsibleOrdinal = 0;
    auto     pushBlock          = [&](Element block, bool spacer) {
        if (idx == focusIdx) {
            block = std::move(block) | focus;
        }
        ++idx;
        elements.push_back(std::move(block));
        if (spacer) {
            elements.push_back(text(""));
        }
    };

    for (const auto& msg : messages_) {
        switch (msg.role) {
        case Message::Role::User:
            pushBlock(hbox({
                          text("> ") | color(theme_.userColor) | bold,
                          paragraph(msg.text) | color(theme_.userColor),
                      }),
                      true);
            break;
        case Message::Role::Assistant:
            pushBlock(paragraph(msg.text) | color(theme_.assistantColor), true);
            break;
        case Message::Role::Thinking:
            {
                const bool expanded = !msg.collapsed;
                Elements   lines;
                Elements   header;
                header.push_back(text(expanded ? "\xe2\x96\xbe " : "\xe2\x96\xb8 ")
                                 | color(theme_.hintColor));
                header.push_back(text("[Thinking] ") | color(theme_.thinkingColor) | bold);
                if (!expanded) {
                    header.push_back(text(oneLinePreview(msg.text)) | color(theme_.thinkingColor));
                }
                lines.push_back(hbox(std::move(header)));
                if (expanded) {
                    lines.push_back(paragraph(msg.text) | color(theme_.thinkingColor));
                }
                Element block
                    = vbox(std::move(lines)) | reflect(collapsibleBoxes_[collapsibleOrdinal]);
                ++collapsibleOrdinal;
                pushBlock(std::move(block), true);
                break;
            }
        case Message::Role::System:
            pushBlock(paragraph(msg.text) | color(theme_.systemColor), true);
            break;
        case Message::Role::Tool:
            {
                const bool expanded = !msg.collapsed;
                Elements   lines;
                // 头部行: 折叠指示符 + [Tool] + 工具名 (+ 折叠态的单行预览)
                Elements header;
                header.push_back(text(expanded ? "\xe2\x96\xbe " : "\xe2\x96\xb8 ")
                                 | color(theme_.hintColor));
                header.push_back(text("[Tool] ") | color(theme_.accentColor) | bold);
                header.push_back(text(msg.toolName) | color(theme_.accentColor) | bold);
                if (!expanded) {
                    if (!msg.toolFinished) {
                        header.push_back(text("  running...") | color(theme_.hintColor) | dim);
                    } else if (msg.toolHasError) {
                        header.push_back(text("  error: ") | color(theme_.systemColor));
                        header.push_back(text(oneLinePreview(msg.toolResult))
                                         | color(theme_.systemColor) | dim);
                    } else {
                        header.push_back(text("  " + oneLinePreview(msg.toolResult))
                                         | color(theme_.statusColor) | dim);
                    }
                }
                lines.push_back(hbox(std::move(header)));

                if (expanded) {
                    if (!msg.text.empty()) {
                        lines.push_back(hbox({
                            text("  args: ") | color(theme_.hintColor),
                            paragraph(msg.text) | color(theme_.hintColor),
                        }));
                    }
                    if (msg.toolFinished) {
                        auto rc = msg.toolHasError ? theme_.systemColor : theme_.statusColor;
                        lines.push_back(hbox({
                            text(msg.toolHasError ? "  error: " : "  result: ") | color(rc),
                            paragraph(msg.toolResult) | color(rc),
                        }));
                    } else {
                        lines.push_back(text("  running...") | color(theme_.hintColor) | dim);
                    }
                }

                Element block
                    = vbox(std::move(lines)) | reflect(collapsibleBoxes_[collapsibleOrdinal]);
                ++collapsibleOrdinal;
                pushBlock(std::move(block), true);
                break;
            }
        }
    }

    if (isStreaming_ && !currentToken_.empty()) {
        if (currentTokenRole_ == Message::Role::Thinking) {
            pushBlock(hbox({
                          text("[Thinking] ") | color(theme_.thinkingColor) | bold,
                          paragraph(currentToken_) | color(theme_.thinkingColor),
                      }),
                      false);
        } else {
            pushBlock(paragraph(currentToken_) | color(theme_.assistantColor), false);
        }
    }

    if (elements.empty()) {
        return vbox({
            filler(),
            text("Agentxx TUI") | bold | color(theme_.accentColor) | center,
            text("Type a message to start. [F2] switch model, [Esc] cancel, "
                 "[Ctrl+C] quit.")
                | dim | center,
            filler(),
        });
    }
    return vbox(std::move(elements));
}

ftxui::Element AgentTUI::renderStatusBar() {
    std::string modelName = currentModelName();
    if (modelName.empty()) {
        modelName = "<none>";
    }
    auto modelInfo = hbox({
        text(" model: ") | color(theme_.hintColor),
        text(modelName) | color(theme_.accentColor) | bold,
        text(" [F2] ") | color(theme_.hintColor),
    });

    size_t ctx    = 0;
    size_t maxCtx = 0;
    if (auto session = currentSession()) {
        if (session->contextStats) {
            ctx    = session->contextStats->contextTokens.load();
            maxCtx = session->contextStats->maxContextTokens.load();
        }
    }
    const auto toK = [](size_t v) {
        return fmt::format("{:.1f}k", static_cast<double>(v) / 1000.0);
    };
    std::string ctxText;
    if (maxCtx > 0) {
        const double pct = 100.0 * static_cast<double>(ctx) / static_cast<double>(maxCtx);
        ctxText          = fmt::format(" {}/{} ({:.1f}%) ", toK(ctx), toK(maxCtx), pct);
    } else {
        ctxText = fmt::format(" {} ", toK(ctx));
    }
    auto ctxInfo = text(ctxText) | color(theme_.statusColor);

    return hbox({
        modelInfo,
        filler(),
        ctxInfo,
    });
}

ftxui::Element AgentTUI::renderPermissionOverlay() {
    const auto& req = pendingPermission_.value();
    return vbox({
               text(" Permission Request ") | bold | inverted,
               separator(),
               hbox({text(" Tool    : ") | bold, text(req.toolName)}),
               hbox({text(" Category: ") | bold, text(req.category)}),
               hbox({text(" Target  : ") | bold, text(req.target)}),
               separator(),
               text(" [y] Allow  [n/Esc] Deny ") | center,
           })
           | border | size(WIDTH, LESS_THAN, 60) | color(theme_.systemColor);
}

ftxui::Element AgentTUI::renderModelSelectorOverlay() {
    Elements items;
    items.push_back(text(" Select Model ") | bold | inverted);
    items.push_back(separator());
    if (modelNames_.empty()) {
        items.push_back(text(" (no models available) ") | dim);
    }
    for (size_t i = 0; i < modelNames_.size(); ++i) {
        auto entry = text(" " + modelNames_[i] + " ");
        if (static_cast<int>(i) == selectedModelIndex_) {
            entry = entry | bgcolor(theme_.buttonActiveBgColor)
                    | color(theme_.buttonActiveTextColor) | bold;
        } else {
            entry = entry | bgcolor(theme_.buttonBgColor) | color(theme_.buttonTextColor);
        }
        items.push_back(entry);
    }
    items.push_back(separator());
    items.push_back(text(" [Up/Down] Move  [Enter] Select  [Esc] Cancel ") | center | dim);
    return vbox(std::move(items)) | border | size(WIDTH, LESS_THAN, 50) | color(theme_.accentColor);
}

ftxui::Element AgentTUI::renderSidebar() {
    // tab 栏: 每个 tab 标题经 reflect 记录渲染区域, 供鼠标点击检测
    tabBoxes_.assign(sidebarTabs_.size(), ftxui::Box{});
    Elements tabs;
    for (size_t i = 0; i < sidebarTabs_.size(); ++i) {
        auto label = text(" " + sidebarTabs_[i].title + " ");
        if (static_cast<int>(i) == activeTabIndex_) {
            label = label | bgcolor(theme_.buttonActiveBgColor)
                    | color(theme_.buttonActiveTextColor) | bold;
        } else {
            label = label | bgcolor(theme_.buttonBgColor) | color(theme_.buttonTextColor);
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
               separator(),
               content | flex | vscroll_indicator | frame,
           })
           | size(WIDTH, LESS_THAN, 56) | size(WIDTH, GREATER_THAN, 28) | border;
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

void AgentTUI::addSidebarTab(const std::string&              id,
                             const std::string&              title,
                             std::function<ftxui::Element()> render) {
    for (auto& tab : sidebarTabs_) {
        if (tab.id == id) {
            tab.title  = title;
            tab.render = std::move(render);
            return;
        }
    }
    sidebarTabs_.push_back(SidebarTab{id, title, std::move(render)});
    activeTabIndex_ = static_cast<int>(sidebarTabs_.size()) - 1;
}

void AgentTUI::removeSidebarTab(const std::string& id) {
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

bool AgentTUI::hasSidebarTab(const std::string& id) const {
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
}

bool AgentTUI::handleSidebarMouse(const ftxui::Mouse& mouse) {
    for (size_t i = 0; i < sidebarTabs_.size() && i < tabBoxes_.size(); ++i) {
        if (false == tabBoxes_[i].Contain(mouse.x, mouse.y)) {
            continue;
        }
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Released) {
            // 左键点击: 切换到该 tab
            activeTabIndex_ = static_cast<int>(i);
            return true;
        }
        if (mouse.button == Mouse::Right && mouse.motion == Mouse::Released) {
            // 右键点击: 关闭该 tab
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

void AgentTUI::openModelSelector() {
    modelNames_.clear();
    selectedModelIndex_ = 0;
    if (agentContext_ && agentContext_->modelRegistry) {
        modelNames_        = agentContext_->modelRegistry->listModelNames();
        const auto current = currentModelName();
        for (size_t i = 0; i < modelNames_.size(); ++i) {
            if (modelNames_[i] == current) {
                selectedModelIndex_ = static_cast<int>(i);
                break;
            }
        }
    }
    showModelSelector_ = true;
}

void AgentTUI::confirmModelSelection() {
    if (selectedModelIndex_ >= 0 && selectedModelIndex_ < static_cast<int>(modelNames_.size())) {
        if (auto session = currentSession()) {
            session->setModelName(modelNames_[selectedModelIndex_]);
        }
    }
    showModelSelector_ = false;
}

void AgentTUI::cancelCurrentRun() {
    if (auto session = currentSession()) {
        auto token = session->getCancelToken();
        if (token) {
            token->cancel();
        }
    }
    if (!currentToken_.empty()) {
        messages_.push_back({currentTokenRole_, currentToken_});
        if (currentTokenRole_ == Message::Role::Thinking) {
            messages_.back().collapsed = true;
        }
        currentToken_.clear();
    }
    messages_.push_back({Message::Role::System, "[Cancelled by user]"});
    isStreaming_ = false;
}

std::shared_ptr<agentxx::agent::Session> AgentTUI::currentSession() {
    if (agentContext_ && agentContext_->sessions) {
        return agentContext_->sessions->getOrCreate(threadId_);
    }
    return nullptr;
}

std::string AgentTUI::currentModelName() {
    std::string selected;
    if (auto session = currentSession()) {
        selected = session->getModelName();
    }
    if (agentContext_ && agentContext_->modelRegistry) {
        return agentContext_->modelRegistry->resolveModelName(selected);
    }
    return selected;
}

void AgentTUI::onToken(const std::string& token, const std::string& kind) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto role = (kind == "thinking") ? Message::Role::Thinking : Message::Role::Assistant;
        if (currentTokenRole_ != role && !currentToken_.empty()) {
            messages_.push_back({currentTokenRole_, currentToken_});
            if (currentTokenRole_ == Message::Role::Thinking) {
                messages_.back().collapsed = true;
            }
            currentToken_.clear();
        }
        currentTokenRole_  = role;
        currentToken_     += token;
        isStreaming_       = true;
    }
    postRedraw();
}

void AgentTUI::onDisplay(const std::string& level, const std::string& content) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.push_back({Message::Role::System, content});
    }
    postRedraw();
}

asio::awaitable<std::optional<std::string>> AgentTUI::getInput() {
    auto [ec, line] = co_await inputChannel_->async_receive(asio::as_tuple(asio::use_awaitable));
    if (ec) {
        co_return std::nullopt;
    }
    co_return std::optional<std::string>(std::move(line));
}

asio::awaitable<bool> AgentTUI::promptPermission(const std::string& toolName,
                                                 const std::string& category,
                                                 const std::string& target) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingPermission_ = PermissionRequest{toolName, category, target};
    }
    postRedraw();

    auto [ec, allowed]
        = co_await permissionChannel_->async_receive(asio::as_tuple(asio::use_awaitable));
    if (ec) {
        co_return false;
    }
    co_return allowed;
}

void AgentTUI::onInterrupt(const std::string& node,
                           const std::string& value,
                           const std::string& handleName) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string                 msg = "Interrupted at: " + node + "\nValue: " + value;
        if (!handleName.empty()) {
            msg += "\nHandle: " + handleName;
        }
        messages_.push_back({Message::Role::System, msg});
    }
    postRedraw();
}

void AgentTUI::onToolStart(const std::string& toolName,
                           const std::string& toolCallId,
                           const std::string& arguments) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 先冲刷当前未提交的流式 token (thinking/content), 保证 toolcall 严格
        // 按时间顺序插入 (一轮内可能 thinking->toolcall->content 任意交替)
        if (!currentToken_.empty()) {
            messages_.push_back({currentTokenRole_, currentToken_});
            if (currentTokenRole_ == Message::Role::Thinking) {
                messages_.back().collapsed = true;
            }
            currentToken_.clear();
        }
        Message m;
        m.role         = Message::Role::Tool;
        m.toolName     = toolName;
        m.toolCallId   = toolCallId;
        m.text         = arguments;
        m.toolFinished = false;
        m.collapsed    = false; // 执行中保持展开
        messages_.push_back(std::move(m));
    }
    postRedraw();
}

void AgentTUI::onToolEnd(const std::string& toolName,
                         const std::string& toolCallId,
                         const std::string& result,
                         bool               hasError) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 按 toolCallId 从后往前找对应的开始消息并填充结果
        bool found = false;
        for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
            if (it->role == Message::Role::Tool && it->toolCallId == toolCallId
                && !it->toolFinished) {
                it->toolResult   = result;
                it->toolFinished = true;
                it->toolHasError = hasError;
                it->collapsed    = true; // 完成自动折叠
                found            = true;
                break;
            }
        }
        if (!found) {
            Message m;
            m.role         = Message::Role::Tool;
            m.toolName     = toolName;
            m.toolCallId   = toolCallId;
            m.toolResult   = result;
            m.toolFinished = true;
            m.toolHasError = hasError;
            m.collapsed    = true;
            messages_.push_back(std::move(m));
        }
    }
    postRedraw();
}

void AgentTUI::resetTokenState() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!currentToken_.empty()) {
            messages_.push_back({currentTokenRole_, currentToken_});
            if (currentTokenRole_ == Message::Role::Thinking) {
                messages_.back().collapsed = true;
            }
            currentToken_.clear();
        }
        isStreaming_ = false;
    }
    postRedraw();
}

void TUILogSink::onLog(agentxx::util::LogLevel level, const std::string& message) {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(Line{level, message});
        while (lines_.size() > maxLines_) {
            lines_.pop_front();
        }
        cb = onNewLog_;
    }
    if (cb) {
        cb();
    }
}

std::vector<TUILogSink::Line> TUILogSink::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {lines_.begin(), lines_.end()};
}

void TUILogSink::setOnNewLog(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    onNewLog_ = std::move(cb);
}

void TUILogSink::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
}
