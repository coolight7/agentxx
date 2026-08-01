#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/string_util.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "ftxui/component/event.hpp"
#include "ftxui/component/loop.hpp"
#include "ftxui/screen/terminal.hpp"
#include "neograph/graph/cancel.h"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>

using namespace ftxui;

AgentTUI::AgentTUI(
    asio::any_io_executor                         ex,
    std::shared_ptr<agentxx::agent::AgentContext> agentContext,
    std::string                                   threadId,
    TUITheme                                      theme
) :
    agentContext_(std::move(agentContext)),
    theme_(theme),
    threadId_(std::move(threadId)),
    ex_(ex),
    inputChannel_(std::make_shared<LineChannel>(ex, 64)),
    logSink_(std::make_shared<TUILogSink>()) {
    if (agentContext_) {
        session_ = agentContext_->getSession(threadId_);
    }
    if (session_ && agentContext_ && agentContext_->modelRegistry) {
        cachedModelName_ = agentContext_->modelRegistry->resolveModelName(session_->getModelName());
    }
}

AgentTUI::~AgentTUI() {
    stop();
}

void AgentTUI::postRedraw() {
    std::shared_ptr<ftxui::ScreenInteractive> s;
    {
        std::lock_guard<std::mutex> lock(screenMutex_);
        s = screen_;
    }
    if (s) {
        // 必须经 App::Post (内部 task_runner 有互斥锁) 而非 PostEvent:
        // FTXUI v7.0.1 的 event_buffer (MultiReceiverBuffer) 非线程安全,
        // 多线程并发 Push 与 UI 线程 Pop/Prune 无锁竞争会破坏 std::deque,
        // 在 Get() 的 operator[] 越界断言处 SIGABRT
        s->Post(Event::Custom);
    }
}

void AgentTUI::start() {
    running_ = true;
    if (logSink_) {
        agentxx::util::LogDispatcher::instance().addSink(logSink_);
    }

    uiThread_ = std::thread([this]() {
        auto screen = std::make_shared<ScreenInteractive>(ScreenInteractive::Fullscreen());
        {
            std::lock_guard<std::mutex> lock(screenMutex_);
            screen_ = screen;
        }

        // 屏幕足够宽时默认显示信息侧边栏
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ftxui::Terminal::Size().dimx >= kInfoSidebarMinWidth
                && !hasSidebarTab(kInfoTabId)) {
                addSidebarTab(
                    kInfoTabId,
                    "Info",
                    [this]() {
                        return renderInfoSidebar();
                    },
                    [this]() {
                        return renderInfoSidebarFooter();
                    }
                );
            }
        }

        // 使用 Component 构建主 UI 树
        auto input_option            = InputOption();
        input_option.multiline       = true;
        input_option.insert          = true;
        input_option.cursor_position = 0;
        input_option.placeholder     = "Type a message... (Enter=send, Alt+Enter=newline)";
        input_option.on_enter        = nullptr;
        input_option.transform       = [](InputState state) {
            if (state.is_placeholder) {
                state.element |= dim;
            }
            return state.element;
        };
        auto input_component = Input(&inputText_, input_option);
        // 可滚动消息列表组件 (ListView 风格 viewport 局部绘制; 返回子项列表)
        messagesScrollable_ = std::make_shared<Scrollable>([this]() -> std::vector<ScrollItem> {
            return buildMessageItems();
        });

        // 侧边栏内容可滚动组件 (各 tab 共用; ListView 风格)
        sidebarScrollable_ = std::make_shared<Scrollable>([this]() -> std::vector<ScrollItem> {
            if (activeTabIndex_ >= 0 && activeTabIndex_ < static_cast<int>(sidebarTabs_.size())) {
                return sidebarTabs_[activeTabIndex_].render();
            }
            return {
                ScrollItem{text(" "), false}
            };
        });

        // 构建组件树: Stacked 将事件依次发给子组件, 不参与布局
        auto stacked
            = Container::Stacked({messagesScrollable_, sidebarScrollable_, input_component});

        // 主渲染器组件
        auto main_renderer = Renderer(stacked, [&]() -> Element {
            std::lock_guard<std::mutex> lock(mutex_);

            // 指示器状态显示
            Element indicator;
            if (awaitingInterruptInput_) {
                indicator
                    = text("!") | bgcolor(theme_.errorColor) | color(Color::White) | bold | blink;
            } else if (isStreaming_) {
                indicator = text("~") | color(theme_.accentColor) | bold;
            } else {
                indicator = text(">") | color(theme_.promptColor) | bold;
            }

            const int maxInputTotalLines = std::max(3, ftxui::Terminal::Size().dimy / 2);
            auto      input_bar          = hbox({
                text(" "),
                vbox({
                    text(" "),
                    hbox({
                        text("  "),
                        indicator,
                        text("  "),
                        input_component->Render() | color(theme_.inputTextColor) | flex,
                        text("  "),
                    }),
                    text(" "),
                }) | bgcolor(theme_.inputBgColor)
                    | xflex | size(HEIGHT, GREATER_THAN, 3)
                    | size(HEIGHT, LESS_THAN, maxInputTotalLines),
                text(" "),
            });

            Element pendingBar = text("");
            if (!pendingInputs_.empty()) {
                pendingBar = hbox({
                    text(" "),
                    text("待发送消息: " + std::to_string(pendingInputs_.size()))
                        | color(theme_.accentColor) | bold | reflect(pendingInputCounterBox_),
                    filler(),
                });
            }

            // 由 viewport 可见区域反推可折叠消息 (Thinking/Tool) 的鼠标命中区域。
            // 注意: 必须在 Render() 调用之前读取 visibleBoxes(), 因为 OnRender() 会重置
            // visibleBoxes_ 为空 (实际填充发生在后续 Element 树布局阶段)。
            // 此处使用上一帧的布局数据, 与用户当前看到的屏幕内容一致。
            collapsibleBoxes_.clear();
            collapsibleMsgIndices_.clear();
            {
                const auto& vboxes = messagesScrollable_->visibleBoxes();
                for (size_t i = 0; i < messageItemMeta_.size() && i < vboxes.size(); ++i) {
                    const auto& meta = messageItemMeta_[i];
                    if (!meta.collapsible || meta.messageIndex < 0) {
                        continue;
                    }
                    if (vboxes[i].IsEmpty()) {
                        continue; // 不可见 (空 Box)
                    }
                    collapsibleBoxes_.push_back(vboxes[i]);
                    collapsibleMsgIndices_.push_back(static_cast<size_t>(meta.messageIndex));
                }
            }

            auto messagesArea = hbox({
                                    text("   "),
                                    messagesScrollable_->Render() | bold | flex,
                                    text("   "),
                                })
                                | reflect(messagesAreaBox_);
            auto mainWidget = vbox({
                messagesArea | flex,
                pendingBar,
                input_bar,
                renderStatusBar(),
                text(" "),
            });

            // 带侧边栏的主体布局
            Element body = mainWidget;
            if (false == sidebarTabs_.empty()) {
                body = hbox({
                    mainWidget | flex,
                    renderSidebar(),
                });
            }

            Element result = body;
            if (showModelSelector_) {
                result = renderModelSelectorOverlay() | center;
            } else if (showSettings_) {
                result = renderSettingsOverlay() | center;
            } else if (showPendingInputs_) {
                result = renderPendingInputsOverlay() | center;
            } else if (showContextOverlay_) {
                result = renderContextOverlay() | center;
            }
            return result | bold | bgcolor(theme_.backgroundColor);
        });

        // 绑定事件处理
        auto event_handler = CatchEvent(main_renderer, [&](Event event) -> bool {
            if (event == Event::CtrlC) {
                if (!inputText_.empty()) {
                    inputText_.clear();
                    postRedraw();
                } else {
                    running_ = false;
                    screen->Exit();
                }
                return true;
            }

            std::lock_guard<std::mutex> lock(mutex_);

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

            if (showSettings_) {
                if (event == Event::ArrowUp) {
                    if (selectedSettingIndex_ > 0) {
                        --selectedSettingIndex_;
                    }
                    postRedraw();
                    return true;
                }
                if (event == Event::ArrowDown) {
                    if (selectedSettingIndex_ < 1) {
                        ++selectedSettingIndex_;
                    }
                    postRedraw();
                    return true;
                }
                if (event == Event::Return) {
                    applyThemeSelection();
                    postRedraw();
                    return true;
                }
                if (event == Event::Escape) {
                    showSettings_ = false;
                    postRedraw();
                    return true;
                }
                return true;
            }

            if (showPendingInputs_) {
                if (event == Event::Escape) {
                    showPendingInputs_ = false;
                    postRedraw();
                    return true;
                }
                if (event.is_mouse() && handlePendingInputsMouse(event.mouse())) {
                    postRedraw();
                    return true;
                }
                return true; // 弹窗打开时屏蔽其余输入
            }

            if (showContextOverlay_) {
                if (event == Event::Escape) {
                    showContextOverlay_ = false;
                    postRedraw();
                    return true;
                }
                // 键盘/鼠标滚动上下文列表
                const int totalItems
                    = contextMessages_.is_array() ? static_cast<int>(contextMessages_.size()) : 0;
                const int maxVisible = std::max(8, ftxui::Terminal::Size().dimy - 10);
                const int maxScroll  = std::max(0, totalItems - maxVisible);
                if (event == Event::ArrowUp) {
                    contextScrollOffset_ = std::max(0, contextScrollOffset_ - 1);
                    postRedraw();
                    return true;
                }
                if (event == Event::ArrowDown) {
                    contextScrollOffset_ = std::min(maxScroll, contextScrollOffset_ + 1);
                    postRedraw();
                    return true;
                }
                if (event == Event::PageUp) {
                    contextScrollOffset_ = std::max(0, contextScrollOffset_ - maxVisible);
                    postRedraw();
                    return true;
                }
                if (event == Event::PageDown) {
                    contextScrollOffset_ = std::min(maxScroll, contextScrollOffset_ + maxVisible);
                    postRedraw();
                    return true;
                }
                if (event.is_mouse()) {
                    const auto& mouse = event.mouse();
                    if (mouse.button == Mouse::WheelUp) {
                        contextScrollOffset_ = std::max(0, contextScrollOffset_ - 3);
                        postRedraw();
                        return true;
                    }
                    if (mouse.button == Mouse::WheelDown) {
                        contextScrollOffset_ = std::min(maxScroll, contextScrollOffset_ + 3);
                        postRedraw();
                        return true;
                    }
                }
                return true;
            }

            {
                std::string_view in = event.input();
                if (in == "\x1B\n" || in == "\x1B\r") {
                    inputText_ += '\n';
                    postRedraw();
                    return true;
                }
                const bool isSend = (event == Event::Return);
                if (isSend) {
                    std::string text = inputText_;
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
                        if (awaitingInterruptInput_.load(std::memory_order_acquire)) {
                            // 中断等待输入: 直接送入 inputChannel_ 供 handleInterrupt 的
                            // getInput() 接收; 不能走 isStreaming_ 待发送队列, 否则 getInput
                            // 永久阻塞导致死锁
                            pushCurrentTokenLocked();
                            messages_.push_back({Message::Role::User, text});
                            if (messagesScrollable_) {
                                messagesScrollable_->setStickToBottom(true);
                            }
                            inputChannel_->async_send(
                                neograph_asio_error_code{},
                                std::move(text),
                                [](neograph_asio_error_code) {}
                            );
                        } else if (isStreaming_) {
                            // BaseAgent 执行中 -> 加入待发送队列, 轮次结束后自动发送
                            pendingInputs_.push_back(PendingInput{std::move(text), false});
                        } else {
                            sendUserInputLocked(std::move(text));
                        }
                        inputText_.clear();
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
            if (event == Event::F3) {
                // 注意: 不能用 Ctrl+I (与 Event::Tab 同为 \x09, 会吞掉 Tab 键)
                selectedSettingIndex_ = 0;
                showSettings_         = true;
                postRedraw();
                return true;
            }
            if (event == Event::F12) {
                toggleLogWindow();
                postRedraw();
                return true;
            }
            if (event.is_mouse()) {
                // 滚轮事件由 Scrollable 子组件处理, 此处不拦截
                const auto& mouse = event.mouse();
                // 侧边栏拖拽调整宽度 (拖拽中优先消费所有鼠标事件)
                if (handleSidebarResizeMouse(mouse)) {
                    postRedraw();
                    return true;
                }
                if (mouse.button == Mouse::Left && mouse.motion == Mouse::Released
                    && !pendingInputs_.empty()
                    && pendingInputCounterBox_.Contain(mouse.x, mouse.y)) {
                    showPendingInputs_ = true;
                    postRedraw();
                    return true;
                }
                if (handleCollapsibleMouse(mouse)) {
                    postRedraw();
                    return true;
                }
                if (mouse.button == Mouse::Left && mouse.motion == Mouse::Released
                    && contextButtonBox_.Contain(mouse.x, mouse.y)) {
                    if (transport_) {
                        sendToPeer(agentxx::agent::WireGetContext{threadId_});
                    } else if (session_) {
                        contextMessages_    = session_->llmMessages;
                        showContextOverlay_ = true;
                    }
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
                cancelCurrentRunLocked();
                postRedraw();
                return true;
            }
            return false;
        });

        // 自定义 Loop: 每帧先 pump 日志队列 (UI 线程消费, 无需跨线程唤醒),
        // 有新日志时 Post(Event::Custom) 触发重绘
        {
            ftxui::Loop loop(screen.get(), event_handler);
            while (!loop.HasQuitted()) {
                if (logSink_ && logSink_->pump() > 0) {
                    screen->Post(Event::Custom);
                }
                loop.RunOnceBlocking();
            }
        }
        {
            std::lock_guard<std::mutex> lock(screenMutex_);
            screen_ = nullptr;
        }
    });
}

void AgentTUI::stop() {
    if (logSink_) {
        agentxx::util::LogDispatcher::instance().removeSink(logSink_);
    }
    running_ = false;
    std::shared_ptr<ftxui::ScreenInteractive> s;
    {
        std::lock_guard<std::mutex> lock(screenMutex_);
        s = std::move(screen_);
        screen_.reset();
    }
    if (s) {
        s->Exit();
    }
    if (uiThread_.joinable()) {
        uiThread_.join();
    }
}

void AgentTUI::requestCancel(std::string_view threadId) {
    if (transport_) {
        sendToPeer(agentxx::agent::WireCancel{std::string{threadId}});
    }
}

void AgentTUI::requestSelectModel(std::string_view threadId, std::string_view model) {
    if (transport_) {
        sendToPeer(agentxx::agent::WireSelectModel{std::string{threadId}, std::string{model}});
    }
}

void AgentTUI::onPeerMessage(agentxx::agent::WireMessage msg) {
    std::visit(
        [this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, agentxx::agent::Delta>) {
                onDelta(m);
            } else if constexpr (std::is_same_v<T, agentxx::agent::SyncPayload>) {
                onSync(m);
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireTurnResult>) {
                std::lock_guard<std::mutex> lock(mutex_);
                isStreaming_ = false;
                if (m.hasError && !m.errorMessage.empty()) {
                    messages_.push_back({Message::Role::System, "[Error] " + m.errorMessage});
                }
                dispatchNextPendingInput();
                postRedraw();
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireContextStats>) {
                if (session_ && session_->contextStats) {
                    session_->contextStats->contextTokens.store(
                        m.contextTokens,
                        std::memory_order_relaxed
                    );
                    session_->contextStats->maxContextTokens.store(
                        m.maxContextTokens,
                        std::memory_order_relaxed
                    );
                }
                postRedraw();
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireInterruptRequest>) {
                auto self = shared_from_this();
                asio::co_spawn(
                    ex_,
                    [self, req = std::move(m)]() mutable -> asio::awaitable<void> {
                        auto result
                            = co_await self
                                  ->handleInterrupt(req.threadId, req.node, req.value, req.argJson);
                        self->sendToPeer(agentxx::agent::WireInterruptResponse{req.id, result});
                    },
                    asio::detached
                );
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireLog>) {
                agentxx::util::LogDispatcher::instance().dispatch(
                    static_cast<agentxx::util::LogLevel>(m.level),
                    m.message
                );
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireModelInfo>) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!m.models.empty()) {
                    modelNames_ = m.models;
                }
                if (!m.currentModel.empty()) {
                    cachedModelName_ = m.currentModel;
                }
                postRedraw();
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireAppendComponentInfo>) {
                // 客户端拉取的启动信息: 整批更新统计与明细
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (const auto& notif : m.notifications) {
                        appendComponents_.push_back(notif);
                    }
                }
                postRedraw();
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireContextMessages>) {
                std::lock_guard<std::mutex> lock(mutex_);
                contextMessages_    = m.messages;
                showContextOverlay_ = true;
                postRedraw();
            }
        },
        std::move(msg)
    );
}

void AgentTUI::pushCurrentTokenLocked() {
    if (currentToken_.empty()) {
        return;
    }
    messages_.push_back(AgentTUI::Message{
        .role        = currentTokenRole_,
        .text        = currentToken_,
        .collapsed   = (currentTokenRole_ == Message::Role::Thinking),
        .durationMs  = pendingTokenDurationMs_,
        .startTimeMs = pendingTokenStartTimeMs_,
    });
    pendingTokenDurationMs_  = 0;
    pendingTokenStartTimeMs_ = 0;
    currentToken_.clear();
}

void AgentTUI::cancelCurrentRunLocked() {
    requestCancel(threadId_);
    pushCurrentTokenLocked();
    messages_.push_back({Message::Role::System, "[Cancel Request]"});
    isStreaming_ = false;
    dispatchNextPendingInput();
}

void AgentTUI::sendUserInputLocked(std::string text) {
    pushCurrentTokenLocked();
    messages_.push_back({Message::Role::User, text});
    isStreaming_ = true;
    if (messagesScrollable_) {
        messagesScrollable_->setStickToBottom(true);
    }
    if (transport_) {
        sendToPeer(agentxx::agent::WireUserInput{threadId_, text});
    } else {
        inputChannel_->async_send(
            neograph_asio_error_code{},
            std::move(text),
            [](neograph_asio_error_code) {}
        );
    }
}

void AgentTUI::dispatchNextPendingInput() {
    if (isStreaming_ || pendingInputs_.empty()) {
        return;
    }
    std::string next = std::move(pendingInputs_.front().text);
    pendingInputs_.pop_front();
    sendUserInputLocked(std::move(next));
}

std::shared_ptr<agentxx::agent::Session> AgentTUI::currentSession() {
    return session_;
}

void AgentTUI::onDelta(const agentxx::agent::Delta& delta) {
    using Type = agentxx::agent::Delta::Type;
    switch (delta.type) {
        case Type::TextToken:
        case Type::ThinkingToken: {
            std::lock_guard<std::mutex> lock(mutex_);
            auto role = (delta.type == Type::ThinkingToken) ? Message::Role::Thinking
                                                            : Message::Role::Assistant;
            if (currentTokenRole_ != role && !currentToken_.empty()) {
                pendingTokenStartTimeMs_ = delta.startTimeMs;
                pendingTokenDurationMs_  = delta.durationMs;
                pushCurrentTokenLocked();
            }
            currentTokenRole_  = role;
            currentToken_     += delta.text;
            isStreaming_       = true;
        } break;
        case Type::ToolStart: {
            std::lock_guard<std::mutex> lock(mutex_);
            pushCurrentTokenLocked();
            Message m;
            m.role         = Message::Role::Tool;
            m.toolName     = delta.toolName;
            m.toolCallId   = delta.toolCallId;
            m.text         = delta.arguments;
            m.toolFinished = false;
            m.collapsed    = false;
            m.startTimeMs  = delta.startTimeMs;
            messages_.push_back(std::move(m));
            isStreaming_ = true;
        } break;
        case Type::ToolEnd: {
            std::lock_guard<std::mutex> lock(mutex_);
            bool                        found = false;
            for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
                if (it->role == Message::Role::Tool && it->toolCallId == delta.toolCallId
                    && !it->toolFinished) {
                    it->toolResult   = delta.result;
                    it->toolFinished = true;
                    it->collapsed    = true;
                    it->startTimeMs  = delta.startTimeMs;
                    it->durationMs   = delta.durationMs;
                    found            = true;
                    break;
                }
            }
            if (!found) {
                Message m;
                m.role         = Message::Role::Tool;
                m.toolName     = delta.toolName;
                m.toolCallId   = delta.toolCallId;
                m.toolResult   = delta.result;
                m.toolFinished = true;
                m.startTimeMs  = delta.startTimeMs;
                m.durationMs   = delta.durationMs;
                m.collapsed    = true;
                messages_.push_back(std::move(m));
            }
        } break;
        case Type::NodeStart: {
            std::lock_guard<std::mutex> lock(mutex_);
            currentNodeName_ = delta.nodeName;
        } break;
        case Type::NodeEnd: {
            std::lock_guard<std::mutex> lock(mutex_);
            if (currentNodeName_ == delta.nodeName) {
                currentNodeName_.clear();
            }
            if (!currentToken_.empty()) {
                // 当前正在流式输出 (Thinking/Assistant): 暂存时间信息,
                // 待 pushCurrentTokenLocked() 时应用到对应消息
                pendingTokenStartTimeMs_ = delta.startTimeMs;
                pendingTokenDurationMs_  = delta.durationMs;
            } else if (!messages_.empty()) {
                // 当前无流式 token (如 Tool 已完成): 直接设到最近的消息
                messages_.back().startTimeMs = delta.startTimeMs;
                messages_.back().durationMs  = delta.durationMs;
            }
        } break;
        case Type::TurnStart: {
            std::lock_guard<std::mutex> lock(mutex_);
            isStreaming_ = true;
        } break;
        case Type::TurnEnd: {
            std::lock_guard<std::mutex> lock(mutex_);
            pushCurrentTokenLocked();
            isStreaming_ = false;

            // 创建一条系统消息记录本轮运行的统计信息
            if (delta.durationMs > 0 || delta.startTimeMs > 0) {
                Message statMsg;
                statMsg.role = Message::Role::System;
                statMsg.text = fmt::format(
                    "{} · {} · {}",
                    cachedModelName_,
                    formatDurationMilliseconds(delta.durationMs),
                    formatTimestampMilliseconds(delta.startTimeMs + delta.durationMs)
                );
                statMsg.durationMs  = delta.durationMs;
                statMsg.startTimeMs = delta.startTimeMs;
                messages_.push_back(std::move(statMsg));
            }

            // 轮次结束 -> 自动派发下一个排队输入
            dispatchNextPendingInput();
        } break;
    }

    // stickToBottom 由 Scrollable 布局阶段自动处理:
    // 靠近底部时内容增长自动吸附到底, 用户手动上滚 (stickToBottom=false) 则不跟随
    postRedraw();
}

void AgentTUI::onSync(const agentxx::agent::SyncPayload& payload) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
        currentToken_.clear();
        // 清空暂存的流式 token 时间信息, 避免残留值错误应用到同步后的首条消息
        pendingTokenDurationMs_  = 0;
        pendingTokenStartTimeMs_ = 0;
        isStreaming_             = false;
        for (const auto& hm : payload.messages) {
            const auto& d    = hm.data;
            auto        role = d.value("role", std::string{});
            Message     m;
            bool        skipPush = false;
            if (role == "user") {
                m.role = Message::Role::User;
                m.text = d.value("content", std::string{});
            } else if (role == "assistant") {
                if (d.contains("tool_calls")) {
                    for (const auto& tc : d["tool_calls"]) {
                        Message tm;
                        tm.role         = Message::Role::Tool;
                        tm.toolName     = tc.value("name", std::string{});
                        tm.toolCallId   = tc.value("id", std::string{});
                        tm.text         = tc.value("arguments", std::string{});
                        tm.toolFinished = false;
                        tm.collapsed    = true;
                        messages_.push_back(std::move(tm));
                    }
                    auto content = d.value("content", std::string{});
                    if (!content.empty()) {
                        m.role = Message::Role::Assistant;
                        m.text = content;
                    } else {
                        continue;
                    }
                } else {
                    m.role         = Message::Role::Assistant;
                    m.text         = d.value("content", std::string{});
                    auto reasoning = d.value("reasoning_content", std::string{});
                    if (!reasoning.empty()) {
                        Message thinkMsg;
                        thinkMsg.role      = Message::Role::Thinking;
                        thinkMsg.text      = reasoning;
                        thinkMsg.collapsed = true;
                        // 从原始数据中读取时间信息（如果有）
                        thinkMsg.startTimeMs = d.value("start_time_ms", int64_t{0});
                        thinkMsg.durationMs  = d.value("duration_ms", int64_t{0});
                        messages_.push_back(std::move(thinkMsg));
                    }
                }
            } else if (role == "tool") {
                m.role         = Message::Role::Tool;
                m.toolName     = d.value("tool_name", std::string{});
                m.toolCallId   = d.value("tool_call_id", std::string{});
                m.toolResult   = d.value("content", std::string{});
                m.toolFinished = true;
                m.collapsed    = true;
                // 从原始数据中读取时间信息（如果有）
                m.startTimeMs = d.value("start_time_ms", int64_t{0});
                m.durationMs  = d.value("duration_ms", int64_t{0});
                for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
                    if (it->role == Message::Role::Tool && it->toolCallId == m.toolCallId
                        && !it->toolFinished) {
                        it->toolResult   = m.toolResult;
                        it->toolFinished = true;
                        it->collapsed    = true;
                        skipPush         = true;
                        break;
                    }
                }
            } else {
                m.role = Message::Role::System;
                m.text = d.value("content", std::string{});
                // 从原始数据中读取时间信息（如果有）
                m.startTimeMs = d.value("start_time_ms", int64_t{0});
                m.durationMs  = d.value("duration_ms", int64_t{0});
            }

            if (false == skipPush) {
                messages_.push_back(std::move(m));
            }
        }
    }
    if (messagesScrollable_) {
        messagesScrollable_->onContentUpdate();
    }
    postRedraw();
}

asio::awaitable<neograph::json> AgentTUI::handleInterrupt(
    std::string_view threadId,
    std::string_view interruptNode,
    std::string_view interruptValue,
    std::string_view interruptArgJson
) {
    auto argOpt
        = agentxx::middleware::InterruptHandleArg::fromJson(neograph::json::parse(interruptArgJson)
        );
    if (!argOpt.has_value()) {
        co_return neograph::json::array();
    }
    const auto& handleArg = argOpt.value();

    // 标记进入中断输入等待: 使 Enter 把用户输入直接送入 inputChannel_ (避免死锁)
    awaitingInterruptInput_.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string                 msg
            = fmt::format("Interrupted at: {}\nValue: {}", interruptNode, interruptValue);
        if (!handleArg.name.empty()) {
            msg += "\nHandle: " + handleArg.name;
        }
        messages_.push_back({Message::Role::System, msg});
    }
    postRedraw();

    auto result = neograph::json::array();
    for (const auto& input : handleArg.inputs) {
        bool inputSuccess = false;
        do {
            std::string prompt = fmt::format(
                "[Input] {}: {}\n{}",
                input.label,
                input.depict,
                input.type.empty()
                    ? ""
                    : fmt::format("Type ({}), default: {}: ", input.type, input.defaultValue)
            );

            {
                std::lock_guard<std::mutex> lock(mutex_);
                messages_.push_back({Message::Role::System, prompt});
            }
            postRedraw();

            if (input.type.empty()) {
                inputSuccess = true;
            } else {
                auto        inputValueOpt = co_await getInput();
                std::string inputValue;
                if (inputValueOpt.has_value()) {
                    inputValue = inputValueOpt.value();
                }
                if (inputValue.empty()) {
                    inputValue = input.defaultValue;
                }

                if ("bool" == input.type) {
                    agentxx::util::toLowerSelf(inputValue);
                    if (inputValue == "yes" || inputValue == "y") {
                        inputValue   = "true";
                        inputSuccess = true;
                    } else if (inputValue == "no" || inputValue == "n") {
                        inputValue   = "false";
                        inputSuccess = true;
                    } else {
                        inputSuccess = false;
                    }
                } else if ("int" == input.type) {
                    int64_t num  = 0;
                    auto    r    = agentxx::util::parseNumberFromString(inputValue, num);
                    inputSuccess = (r.ec == std::errc{});
                } else if ("double" == input.type) {
                    double num;
                    auto   r     = agentxx::util::parseNumberFromString(inputValue, num);
                    inputSuccess = (r.ec == std::errc{});
                } else if ("string" == input.type) {
                    inputSuccess = true;
                } else if ("enum" == input.type) {
                    for (const auto& val : input.enumValues) {
                        if (val == inputValue) {
                            inputSuccess = true;
                            break;
                        }
                    }
                }

                if (inputSuccess) {
                    result.push_back(inputValue);
                } else {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        messages_.push_back(
                            {Message::Role::System, "Invalid input, please try again."}
                        );
                    }
                    postRedraw();
                }
            }
        } while (false == inputSuccess);
    }
    awaitingInterruptInput_.store(false, std::memory_order_release);
    co_return result;
}

asio::awaitable<std::optional<std::string>> AgentTUI::getInput() {
    auto [ec, line] = co_await inputChannel_->async_receive(asio::as_tuple(asio::use_awaitable));
    if (ec) {
        co_return std::nullopt;
    }
    co_return std::optional<std::string>(std::move(line));
}
