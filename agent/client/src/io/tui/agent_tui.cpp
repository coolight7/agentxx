#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/input_bar.h"
#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/components/sidebar.h"
#include "agentxx-client/io/tui/components/status_bar.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/string_util.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "ftxui/component/event.hpp"
#include "ftxui/component/loop.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include "neograph/graph/cancel.h"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>

using namespace ftxui;

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

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
        sharedState_.mutate([&](TUIRenderState& st) {
            st.cachedModelName
                = agentContext_->modelRegistry->resolveModelName(session_->getModelName());
        });
    }
}

AgentTUI::~AgentTUI() {
    stop();
}

// ---------------------------------------------------------------------------
// postRedraw
// ---------------------------------------------------------------------------

void AgentTUI::postRedraw() {
    std::shared_ptr<ScreenInteractive> s;
    {
        std::lock_guard<std::mutex> lock(screenMutex_);
        s = screen_;
    }
    if (s) {
        s->Post(Event::Custom);
    }
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

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

        // 构建组件共享上下文
        ctx_.state      = &sharedState_;
        ctx_.frameState = sharedState_.readSnapshot();
        ctx_.postRedraw = [this] {
            postRedraw();
        };
        ctx_.theme     = &theme_;
        ctx_.session   = session_;
        ctx_.threadId  = threadId_;
        ctx_.remoteUrl = remoteUrl_;

        // 创建组件
        messageList_ = std::make_shared<MessageListComponent>(ctx_);
        statusBar_   = std::make_shared<StatusBarComponent>(ctx_);
        sidebar_     = std::make_shared<SidebarComponent>(ctx_);

        InputComponent::Config inputCfg;
        inputCfg.onSend = [this](std::string text) {
            std::lock_guard<std::mutex> lock(sharedState_.mutex());
            auto&                       st = sharedState_.mutableState();
            if (awaitingInterruptInput_.load(std::memory_order_acquire)) {
                pushCurrentTokenLocked(st);
                st.messages.push_back(
                    std::make_shared<TUIMessage>(TUIMessage{TUIMessage::Role::User, text})
                );
                messageList_->setStickToBottom(true);
                inputChannel_->async_send(
                    neograph_asio_error_code{},
                    std::move(text),
                    [](neograph_asio_error_code) {}
                );
            } else if (st.isStreaming) {
                st.pendingInputs.push_back(TUIPendingInput{std::move(text), false});
            } else {
                sendUserInputLocked(st, std::move(text));
            }
        };
        inputCfg.onCtrlC = [this, screen]() -> bool {
            running_ = false;
            screen->Exit();
            return true;
        };
        inputCfg.isAwaitingInterrupt = [this] {
            return awaitingInterruptInput_.load(std::memory_order_acquire);
        };
        inputCfg.isStreaming = [this] {
            return ctx_.frameState && ctx_.frameState->isStreaming;
        };
        inputBar_ = std::make_shared<InputComponent>(ctx_, std::move(inputCfg));

        // 屏幕足够宽时默认显示信息侧边栏
        if (Terminal::Size().dimx >= kInfoSidebarMinWidth && !sidebar_->hasTab(kInfoTabId)) {
            sidebar_->addTab(
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

        // 侧边栏 footer 点击: 处理 "上下文" 按钮
        sidebar_->onFooterClick([this](const Mouse&) -> bool {
            if (transport_) {
                sendToPeer(agentxx::agent::WireGetContext{threadId_});
            } else if (session_) {
                std::lock_guard<std::mutex> lock(sharedState_.mutex());
                auto&                       st = sharedState_.mutableState();
                st.contextMessages             = session_->llmMessages;
                st.showContextOverlay          = true;
            }
            if (modal_ && !modal_->hasModal()) {
                auto overlay = std::make_shared<ContextOverlay>(ctx_);
                overlay->onClose([this] {
                    modal_->popModal();
                });
                modal_->pushModal(overlay);
            }
            return true;
        });

        // 主布局: Stacked 让子组件接收事件, Renderer 组合渲染
        auto stacked      = Container::Stacked({messageList_, sidebar_, inputBar_});
        auto mainRenderer = Renderer(stacked, [&]() -> Element {
            ctx_.frameState = sharedState_.readSnapshot();
            const auto& st  = *ctx_.frameState;

            Element pendingBar = text("");
            if (!st.pendingInputs.empty()) {
                pendingBar = hbox({
                    text(" "),
                    text("待发送消息: " + std::to_string(st.pendingInputs.size()))
                        | color(theme_.accentColor) | bold | reflect(pendingCounterBox_),
                    filler(),
                });
            }

            auto mainWidget = vbox({
                messageList_->Render() | flex,
                pendingBar,
                inputBar_->Render(),
                statusBar_->Render(),
                text(" "),
            });

            Element body = mainWidget;
            if (!sidebar_->empty()) {
                body = hbox({
                    mainWidget | flex,
                    sidebar_->Render(),
                });
            }
            return body | bold | bgcolor(theme_.backgroundColor);
        });

        // 模态容器: 主布局 + 弹窗层
        modal_ = ModalContainer::Create(mainRenderer);
        modal_->setBgColor(theme_.backgroundColor);

        // 全局快捷键 + 鼠标 (F2/F3/F12/Escape/点击): 组件未消费的事件到此处理
        auto handler = CatchEvent(modal_, [&](Event event) -> bool {
            // 模态弹窗打开时: 优先让弹窗处理 (Escape 关闭弹窗等), 不拦截
            if (modal_->hasModal()) {
                return false;
            }
            if (event == Event::F2) {
                openModelSelector();
                return true;
            }
            if (event == Event::F3) {
                openSettings();
                return true;
            }
            if (event == Event::F12) {
                toggleLogWindow();
                return true;
            }
            if (event.is_mouse()) {
                const auto& mouse = event.mouse();
                if (mouse.button == Mouse::Left && mouse.motion == Mouse::Released) {
                    // 可折叠消息点击 (Thinking/Tool 展开/折叠)
                    if (messageList_ && messageList_->handleCollapsibleClick(mouse)) {
                        return true;
                    }
                    // 待发送消息计数点击
                    if (!ctx_.frameState->pendingInputs.empty()
                        && pendingCounterBox_.Contain(mouse.x, mouse.y)) {
                        auto overlay = std::make_shared<PendingInputsOverlay>(ctx_);
                        overlay->onClose([this] {
                            modal_->popModal();
                        });
                        modal_->pushModal(overlay);
                        postRedraw();
                        return true;
                    }
                }
                return false;
            }
            if (event == Event::Escape) {
                std::lock_guard<std::mutex> lock(sharedState_.mutex());
                auto                        snap = sharedState_.snapshot();
                if (snap->isStreaming) {
                    auto& st = sharedState_.mutableState();
                    cancelCurrentRunLocked(st);
                    postRedraw();
                    return true;
                }
            }
            return false;
        });

        // 自定义 Loop: 每帧 pump 日志
        {
            Loop loop(screen.get(), handler);
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
    std::shared_ptr<ScreenInteractive> s;
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

// ---------------------------------------------------------------------------
// 模态管理
// ---------------------------------------------------------------------------

void AgentTUI::openModelSelector() {
    if (transport_) {
        sendToPeer(agentxx::agent::WireGetModel{threadId_});
    }
    auto overlay = std::make_shared<ModelSelectorOverlay>(ctx_);
    auto snap    = sharedState_.readSnapshot();
    for (size_t i = 0; i < snap->modelNames.size(); ++i) {
        if (snap->modelNames[i] == snap->cachedModelName) {
            overlay->setInitialIndex(static_cast<int>(i));
            break;
        }
    }
    overlay->onConfirm([this](std::string model) {
        requestSelectModel(threadId_, model);
    });
    overlay->onClose([this] {
        modal_->popModal();
    });
    modal_->pushModal(overlay);
    postRedraw();
}

void AgentTUI::openSettings() {
    auto overlay = std::make_shared<SettingsOverlay>(ctx_);
    overlay->onClose([this] {
        modal_->popModal();
        // 主题变化后清空消息缓存
        if (messageList_) {
            messageList_->invalidateCache();
        }
        logLineCache_.clear();
    });
    modal_->pushModal(overlay);
    postRedraw();
}

void AgentTUI::toggleLogWindow() {
    if (sidebar_->hasTab(kLogTabId)) {
        sidebar_->removeTab(kLogTabId);
    } else {
        sidebar_->addTab(
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
    postRedraw();
}

// ---------------------------------------------------------------------------
// requestCancel / requestSelectModel
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// onPeerMessage (client 线程)
// ---------------------------------------------------------------------------

void AgentTUI::onPeerMessage(agentxx::agent::WireMessage msg) {
    std::visit(
        [this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, agentxx::agent::Delta>) {
                onDelta(m);
            } else if constexpr (std::is_same_v<T, agentxx::agent::SyncPayload>) {
                onSync(m);
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireTurnResult>) {
                onTurnResult(m);
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireContextStats>) {
                onContextStats(m);
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
                {
                    std::lock_guard<std::mutex> lock(sharedState_.mutex());
                    auto&                       st = sharedState_.mutableState();
                    if (!m.models.empty()) {
                        st.modelNames = m.models;
                    }
                    if (!m.currentModel.empty()) {
                        st.cachedModelName = m.currentModel;
                    }
                }
                postRedraw();
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireAppendComponentInfo>) {
                {
                    std::lock_guard<std::mutex> lock(sharedState_.mutex());
                    auto&                       st = sharedState_.mutableState();
                    for (const auto& notif : m.notifications) {
                        st.appendComponents.push_back(notif);
                    }
                }
                postRedraw();
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireContextMessages>) {
                {
                    std::lock_guard<std::mutex> lock(sharedState_.mutex());
                    auto&                       st = sharedState_.mutableState();
                    st.contextMessages             = m.messages;
                    st.showContextOverlay          = true;
                }
                // 打开上下文弹窗
                if (modal_ && !modal_->hasModal()) {
                    auto overlay = std::make_shared<ContextOverlay>(ctx_);
                    overlay->onClose([this] {
                        modal_->popModal();
                    });
                    modal_->pushModal(overlay);
                }
                postRedraw();
            }
        },
        std::move(msg)
    );
}

// ---------------------------------------------------------------------------
// 协议处理辅助 (调用方须持有 sharedState_.mutex())
// ---------------------------------------------------------------------------

void AgentTUI::pushCurrentTokenLocked(TUIRenderState& st) {
    if (st.currentToken.empty()) {
        return;
    }
    auto msg         = std::make_shared<TUIMessage>();
    msg->role        = st.currentTokenRole;
    msg->text        = st.currentToken;
    msg->collapsed   = (st.currentTokenRole == TUIMessage::Role::Thinking);
    msg->durationMs  = st.pendingTokenDurationMs;
    msg->startTimeMs = st.pendingTokenStartTimeMs;
    st.messages.push_back(std::move(msg));
    st.pendingTokenDurationMs  = 0;
    st.pendingTokenStartTimeMs = 0;
    st.currentToken.clear();
}

void AgentTUI::cancelCurrentRunLocked(TUIRenderState& st) {
    requestCancel(threadId_);
    pushCurrentTokenLocked(st);
    st.messages.push_back(
        std::make_shared<TUIMessage>(TUIMessage{TUIMessage::Role::System, "[Cancel Request]"})
    );
    st.isStreaming = false;
    dispatchNextPendingInput(st);
}

void AgentTUI::sendUserInputLocked(TUIRenderState& st, std::string text) {
    pushCurrentTokenLocked(st);
    st.messages.push_back(std::make_shared<TUIMessage>(TUIMessage{TUIMessage::Role::User, text}));
    st.isStreaming = true;
    if (messageList_) {
        messageList_->setStickToBottom(true);
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

void AgentTUI::dispatchNextPendingInput(TUIRenderState& st) {
    if (st.isStreaming || st.pendingInputs.empty()) {
        return;
    }
    std::string next = std::move(st.pendingInputs.front().text);
    st.pendingInputs.pop_front();
    sendUserInputLocked(st, std::move(next));
}

// ---------------------------------------------------------------------------
// onDelta (client 线程)
// ---------------------------------------------------------------------------

void AgentTUI::onDelta(const agentxx::agent::Delta& delta) {
    using Type = agentxx::agent::Delta::Type;
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        switch (delta.type) {
            case Type::TextToken:
            case Type::ThinkingToken: {
                auto role = (delta.type == Type::ThinkingToken) ? TUIMessage::Role::Thinking
                                                                : TUIMessage::Role::Assistant;
                if (st.currentTokenRole != role && !st.currentToken.empty()) {
                    st.pendingTokenStartTimeMs = delta.startTimeMs;
                    st.pendingTokenDurationMs  = delta.durationMs;
                    pushCurrentTokenLocked(st);
                }
                st.currentTokenRole  = role;
                st.currentToken     += delta.text;
                st.isStreaming       = true;
            } break;
            case Type::ToolStart: {
                pushCurrentTokenLocked(st);
                auto m          = std::make_shared<TUIMessage>();
                m->role         = TUIMessage::Role::Tool;
                m->toolName     = delta.toolName;
                m->toolCallId   = delta.toolCallId;
                m->text         = delta.arguments;
                m->toolFinished = false;
                m->collapsed    = false;
                m->startTimeMs  = delta.startTimeMs;
                st.messages.push_back(std::move(m));
                st.isStreaming = true;
            } break;
            case Type::ToolEnd: {
                bool found = false;
                for (size_t i = st.messages.size(); i > 0; --i) {
                    auto& msg = *st.messages[i - 1];
                    if (msg.role == TUIMessage::Role::Tool && msg.toolCallId == delta.toolCallId
                        && !msg.toolFinished) {
                        auto& m        = sharedState_.mutableMessage(st, i - 1);
                        m.toolResult   = delta.result;
                        m.toolFinished = true;
                        m.collapsed    = true;
                        m.startTimeMs  = delta.startTimeMs;
                        m.durationMs   = delta.durationMs;
                        found          = true;
                        break;
                    }
                }
                if (!found) {
                    auto m          = std::make_shared<TUIMessage>();
                    m->role         = TUIMessage::Role::Tool;
                    m->toolName     = delta.toolName;
                    m->toolCallId   = delta.toolCallId;
                    m->toolResult   = delta.result;
                    m->toolFinished = true;
                    m->startTimeMs  = delta.startTimeMs;
                    m->durationMs   = delta.durationMs;
                    m->collapsed    = true;
                    st.messages.push_back(std::move(m));
                }
            } break;
            case Type::NodeStart: {
                st.currentNodeName = delta.nodeName;
            } break;
            case Type::NodeEnd: {
                if (st.currentNodeName == delta.nodeName) {
                    st.currentNodeName.clear();
                }
                if (!st.currentToken.empty()) {
                    st.pendingTokenStartTimeMs = delta.startTimeMs;
                    st.pendingTokenDurationMs  = delta.durationMs;
                } else if (!st.messages.empty()) {
                    auto& m       = sharedState_.mutableMessage(st, st.messages.size() - 1);
                    m.startTimeMs = delta.startTimeMs;
                    m.durationMs  = delta.durationMs;
                }
            } break;
            case Type::TurnStart: {
                st.isStreaming = true;
            } break;
            case Type::TurnEnd: {
                pushCurrentTokenLocked(st);
                st.isStreaming = false;
                if (delta.durationMs > 0 || delta.startTimeMs > 0) {
                    auto statMsg  = std::make_shared<TUIMessage>();
                    statMsg->role = TUIMessage::Role::System;
                    statMsg->text = fmt::format(
                        "{} · {} · {}",
                        st.cachedModelName,
                        formatDurationMilliseconds(delta.durationMs),
                        formatTimestampMilliseconds(delta.startTimeMs + delta.durationMs)
                    );
                    statMsg->durationMs  = delta.durationMs;
                    statMsg->startTimeMs = delta.startTimeMs;
                    st.messages.push_back(std::move(statMsg));
                }
                dispatchNextPendingInput(st);
            } break;
        }
    }
    postRedraw();
}

// ---------------------------------------------------------------------------
// onSync (client 线程)
// ---------------------------------------------------------------------------

void AgentTUI::onSync(const agentxx::agent::SyncPayload& payload) {
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto                        st   = std::make_shared<TUIRenderState>();
        auto                        prev = sharedState_.snapshot();
        st->cachedModelName              = prev->cachedModelName;
        st->modelNames                   = prev->modelNames;
        st->appendComponents             = prev->appendComponents;
        st->pendingInputs                = prev->pendingInputs;
        st->isStreaming                  = false;

        for (const auto& hm : payload.messages) {
            const auto& d        = hm.data;
            auto        role     = d.value("role", std::string{});
            auto        m        = std::make_shared<TUIMessage>();
            bool        skipPush = false;
            if (role == "user") {
                m->role = TUIMessage::Role::User;
                m->text = d.value("content", std::string{});
            } else if (role == "assistant") {
                if (d.contains("tool_calls")) {
                    for (const auto& tc : d["tool_calls"]) {
                        auto tm          = std::make_shared<TUIMessage>();
                        tm->role         = TUIMessage::Role::Tool;
                        tm->toolName     = tc.value("name", std::string{});
                        tm->toolCallId   = tc.value("id", std::string{});
                        tm->text         = tc.value("arguments", std::string{});
                        tm->toolFinished = false;
                        tm->collapsed    = true;
                        st->messages.push_back(std::move(tm));
                    }
                    auto content = d.value("content", std::string{});
                    if (!content.empty()) {
                        m->role = TUIMessage::Role::Assistant;
                        m->text = content;
                    } else {
                        continue;
                    }
                } else {
                    m->role        = TUIMessage::Role::Assistant;
                    m->text        = d.value("content", std::string{});
                    auto reasoning = d.value("reasoning_content", std::string{});
                    if (!reasoning.empty()) {
                        auto thinkMsg         = std::make_shared<TUIMessage>();
                        thinkMsg->role        = TUIMessage::Role::Thinking;
                        thinkMsg->text        = reasoning;
                        thinkMsg->collapsed   = true;
                        thinkMsg->startTimeMs = d.value("start_time_ms", int64_t{0});
                        thinkMsg->durationMs  = d.value("duration_ms", int64_t{0});
                        st->messages.push_back(std::move(thinkMsg));
                    }
                }
            } else if (role == "tool") {
                m->role         = TUIMessage::Role::Tool;
                m->toolName     = d.value("tool_name", std::string{});
                m->toolCallId   = d.value("tool_call_id", std::string{});
                m->toolResult   = d.value("content", std::string{});
                m->toolFinished = true;
                m->collapsed    = true;
                m->startTimeMs  = d.value("start_time_ms", int64_t{0});
                m->durationMs   = d.value("duration_ms", int64_t{0});
                for (size_t i = st->messages.size(); i > 0; --i) {
                    auto& prevMsg = *st->messages[i - 1];
                    if (prevMsg.role == TUIMessage::Role::Tool
                        && prevMsg.toolCallId == m->toolCallId && !prevMsg.toolFinished) {
                        prevMsg.toolResult   = m->toolResult;
                        prevMsg.toolFinished = true;
                        prevMsg.collapsed    = true;
                        skipPush             = true;
                        break;
                    }
                }
            } else {
                m->role        = TUIMessage::Role::System;
                m->text        = d.value("content", std::string{});
                m->startTimeMs = d.value("start_time_ms", int64_t{0});
                m->durationMs  = d.value("duration_ms", int64_t{0});
            }
            if (!skipPush) {
                st->messages.push_back(std::move(m));
            }
        }
        // 直接替换 (旧快照由 UI 线程持有, 自然释放)
        sharedState_.mutate([&](TUIRenderState& cur) {
            cur = std::move(*st);
        });
    }
    if (messageList_) {
        messageList_->setStickToBottom(true);
    }
    postRedraw();
}

// ---------------------------------------------------------------------------
// onTurnResult / onContextStats (client 线程)
// ---------------------------------------------------------------------------

void AgentTUI::onTurnResult(const agentxx::agent::WireTurnResult& result) {
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        st.isStreaming                 = false;
        if (result.hasError && !result.errorMessage.empty()) {
            st.messages.push_back(std::make_shared<TUIMessage>(
                TUIMessage{TUIMessage::Role::System, "[Error] " + result.errorMessage}
            ));
        }
        dispatchNextPendingInput(st);
    }
    postRedraw();
}

void AgentTUI::onContextStats(const agentxx::agent::WireContextStats& stats) {
    if (session_ && session_->contextStats) {
        session_->contextStats->contextTokens.store(stats.contextTokens, std::memory_order_relaxed);
        session_->contextStats->maxContextTokens.store(
            stats.maxContextTokens,
            std::memory_order_relaxed
        );
    }
    postRedraw();
}

// ---------------------------------------------------------------------------
// handleInterrupt (client 线程, co_spawn)
// ---------------------------------------------------------------------------

asio::awaitable<neograph::json> AgentTUI::handleInterrupt(
    std::string_view threadId,
    std::string_view interruptNode,
    std::string_view interruptValue,
    std::string_view interruptArgJson
) {
    std::optional<agentxx::middleware::InterruptHandleArg> argOpt;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            argOpt = agentxx::middleware::InterruptHandleArg::fromJson(
                neograph::json::parse(interruptArgJson)
            );
            return true;
        },
        [](std::string errinfo) -> bool {
            XX_LOGE("AgentTUI::handleInterrupt json::parse failed: {}", errinfo);
            return true;
        }
    );
    if (!argOpt.has_value()) {
        co_return neograph::json::array();
    }
    const auto& handleArg = argOpt.value();

    awaitingInterruptInput_.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        std::string                 msg
            = fmt::format("Interrupted at: {}\nValue: {}", interruptNode, interruptValue);
        if (!handleArg.name.empty()) {
            msg += "\nHandle: " + handleArg.name;
        }
        st.messages.push_back(std::make_shared<TUIMessage>(TUIMessage{TUIMessage::Role::System, msg}
        ));
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
                std::lock_guard<std::mutex> lock(sharedState_.mutex());
                auto&                       st = sharedState_.mutableState();
                st.messages.push_back(
                    std::make_shared<TUIMessage>(TUIMessage{TUIMessage::Role::System, prompt})
                );
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
                    }
                } else if ("int" == input.type) {
                    int64_t num;
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
                    std::lock_guard<std::mutex> lock(sharedState_.mutex());
                    auto&                       st = sharedState_.mutableState();
                    st.messages.push_back(std::make_shared<TUIMessage>(
                        TUIMessage{TUIMessage::Role::System, "Invalid input, please try again."}
                    ));
                    postRedraw();
                }
            }
        } while (!inputSuccess);
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
