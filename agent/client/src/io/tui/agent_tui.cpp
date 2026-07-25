#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/string_util.h"
#include "asio/as_tuple.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/terminal.hpp"
#include "neograph/graph/cancel.h"
#include <algorithm>
#include <charconv>
#include <cstdint>

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
    inputChannel_(std::make_shared<LineChannel>(ex, 64)),
    permissionChannel_(std::make_shared<BoolChannel>(ex, 4)),
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
    if (auto* s = screen_.load(std::memory_order_acquire)) {
        s->PostEvent(Event::Custom);
    }
}

void AgentTUI::start() {
    running_ = true;
    if (logSink_) {
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
        screen_.store(&screen, std::memory_order_release);

        // 屏幕足够宽时默认显示信息侧边栏
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ftxui::Terminal::Size().dimx >= kInfoSidebarMinWidth
                && !hasSidebarTab(kInfoTabId)) {
                addSidebarTab(kInfoTabId, "信息", [this]() {
                    return renderInfoSidebar();
                });
            }
        }

        auto input_option      = InputOption();
        input_option.multiline = true;
        input_option.transform = [](InputState state) {
            if (state.is_placeholder) {
                state.element |= dim;
            }
            return state.element;
        };
        input_option.on_enter = nullptr;

        auto input
            = Input(&inputText_, "Type a message... (Enter=newline, Alt+Enter=send)", input_option);

        auto layout = Renderer(input, [&]() -> Element {
            std::lock_guard<std::mutex> lock(mutex_);

            auto messages = hbox({
                                text("   "),
                                renderMessages() | flex | vscroll_indicator | yframe,
                                text("   "),
                            })
                            | flex | yframe;

            Element indicator;
            if (pendingPermission_) {
                indicator = text("!") | bgcolor(Color::Red) | color(Color::White) | bold | blink;
            } else if (isStreaming_) {
                indicator = text("⏹") | color(theme_.accentColor) | bold;
            } else {
                indicator = text(">") | color(theme_.promptColor) | bold;
            }

            const int maxInputTotalLines = std::max(3, ftxui::Terminal::Size().dimy / 2);
            auto      input_bar          = hbox({
                text(" "),
                vbox({
                    text(" "),
                    hbox({
                        text(" "),
                        indicator,
                        text(" "),
                        input->Render() | color(theme_.inputTextColor) | flex,
                        text(" "),
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

            auto main = vbox({
                messages,
                pendingBar,
                input_bar,
                renderStatusBar(),
                text(" "),
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
            } else if (showSettings_) {
                result = renderSettingsOverlay() | center;
            } else if (showPendingInputs_) {
                result = renderPendingInputsOverlay() | center;
            }
            return result | bold | bgcolor(theme_.backgroundColor);
        });

        auto event_handler = CatchEvent(layout, [&](Event event) -> bool {
            if (event == Event::CtrlC) {
                if (!inputText_.empty()) {
                    inputText_.clear();
                    postRedraw();
                } else {
                    running_ = false;
                    screen.Exit();
                }
                return true;
            }

            std::lock_guard<std::mutex> lock(mutex_);

            if (pendingPermission_.has_value()) {
                if (event == Event::Character('y') || event == Event::Character('Y')) {
                    pendingPermission_.reset();
                    permissionChannel_->async_send(
                        neograph_asio_error_code{},
                        true,
                        [](neograph_asio_error_code) {}
                    );
                    postRedraw();
                    return true;
                }
                if (event == Event::Character('n') || event == Event::Character('N')
                    || event == Event::Escape) {
                    pendingPermission_.reset();
                    permissionChannel_->async_send(
                        neograph_asio_error_code{},
                        false,
                        [](neograph_asio_error_code) {}
                    );
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

            {
                const std::string& in     = event.input();
                const bool         isSend = (in == "\x1B\n" || in == "\x1B\r");
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
                            if (!currentToken_.empty()) {
                                messages_.push_back({currentTokenRole_, currentToken_});
                                if (currentTokenRole_ == Message::Role::Thinking) {
                                    messages_.back().collapsed = true;
                                }
                                currentToken_.clear();
                            }
                            messages_.push_back({Message::Role::User, text});
                            stickToBottom_ = true;
                            inputChannel_->async_send(
                                neograph_asio_error_code{},
                                std::move(text),
                                [](neograph_asio_error_code) {}
                            );
                        } else if (isStreaming_) {
                            // DeepAgent 执行中 -> 加入待发送队列, 轮次结束后自动发送
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
            if (event == Event::CtrlI) {
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
                const auto& mouse = event.mouse();
                if (mouse.button == Mouse::WheelUp || mouse.button == Mouse::WheelDown) {
                    if (!sidebarTabs_.empty() && mouse.x >= ftxui::Terminal::Size().dimx - 56) {
                        const int last
                            = static_cast<int>(logSink_ ? logSink_->snapshot().size() : 0) - 1;
                        if (last >= 0) {
                            int cur  = logStickToBottom_ ? last : logFocusIndex_;
                            cur     += (mouse.button == Mouse::WheelUp) ? -1 : +1;
                            if (cur >= last) {
                                logStickToBottom_ = true;
                                logFocusIndex_    = -1;
                            } else {
                                logStickToBottom_ = false;
                                logFocusIndex_    = std::max(0, cur);
                            }
                        }
                        postRedraw();
                        return true;
                    }
                    const int last = focusBlockCount() - 1;
                    if (last >= 0) {
                        int cur = stickToBottom_ ? last : scrollAnchorIndex_;
                        scrollAccum_
                            += (mouse.button == Mouse::WheelUp) ? -kScrollStep : +kScrollStep;
                        int move = static_cast<int>(scrollAccum_);
                        if (move != 0) {
                            scrollAccum_ -= static_cast<float>(move);
                            cur          += move;
                            if (cur >= last) {
                                stickToBottom_ = true;
                                scrollAccum_   = 0.0f;
                            } else {
                                stickToBottom_     = false;
                                scrollAnchorIndex_ = std::max(0, cur);
                            }
                        }
                    }
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
        screen_.store(nullptr, std::memory_order_release);
    });
}

void AgentTUI::stop() {
    if (logSink_) {
        agentxx::util::LogDispatcher::instance().removeSink(logSink_);
        logSink_->setOnNewLog(nullptr);
    }
    running_ = false;
    if (auto* s = screen_.load(std::memory_order_acquire)) {
        s->Exit();
    }
    if (uiThread_.joinable()) {
        uiThread_.join();
    }
}

void AgentTUI::cancelCurrentRun() {
    if (cancelCallback_) {
        cancelCallback_();
    } else if (session_) {
        auto token = session_->getCancelToken();
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
    dispatchNextPendingInput();
}

void AgentTUI::sendUserInputLocked(std::string text) {
    if (!currentToken_.empty()) {
        messages_.push_back({currentTokenRole_, currentToken_});
        if (currentTokenRole_ == Message::Role::Thinking) {
            messages_.back().collapsed = true;
        }
        currentToken_.clear();
    }
    messages_.push_back({Message::Role::User, text});
    isStreaming_   = true;
    stickToBottom_ = true;
    inputChannel_
        ->async_send(neograph_asio_error_code{}, std::move(text), [](neograph_asio_error_code) {});
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
                messages_.push_back({currentTokenRole_, currentToken_});
                if (currentTokenRole_ == Message::Role::Thinking) {
                    messages_.back().collapsed = true;
                }
                currentToken_.clear();
            }
            currentTokenRole_  = role;
            currentToken_     += delta.text;
            isStreaming_       = true;
        } break;
        case Type::ToolStart: {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!currentToken_.empty()) {
                messages_.push_back({currentTokenRole_, currentToken_});
                if (currentTokenRole_ == Message::Role::Thinking) {
                    messages_.back().collapsed = true;
                }
                currentToken_.clear();
            }
            Message m;
            m.role         = Message::Role::Tool;
            m.toolName     = delta.toolName;
            m.toolCallId   = delta.toolCallId;
            m.text         = delta.arguments;
            m.toolFinished = false;
            m.collapsed    = false;
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
                    it->toolHasError = delta.hasError;
                    it->collapsed    = true;
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
                m.toolHasError = delta.hasError;
                m.collapsed    = true;
                messages_.push_back(std::move(m));
            }
        } break;
        case Type::TurnStart: {
            std::lock_guard<std::mutex> lock(mutex_);
            isStreaming_ = true;
        } break;
        case Type::TurnEnd: {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!currentToken_.empty()) {
                messages_.push_back({currentTokenRole_, currentToken_});
                if (currentTokenRole_ == Message::Role::Thinking) {
                    messages_.back().collapsed = true;
                }
                currentToken_.clear();
            }
            isStreaming_ = false;
            // 轮次结束 -> 自动派发下一个排队输入
            dispatchNextPendingInput();
        } break;
    }
    postRedraw();
}

void AgentTUI::onSync(const agentxx::agent::SyncPayload& payload) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
        currentToken_.clear();
        isStreaming_ = false;
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
                try {
                    auto parsed    = neograph::json::parse(m.toolResult);
                    m.toolHasError = parsed.is_object() && parsed.contains("error");
                } catch (...) {
                }
                for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
                    if (it->role == Message::Role::Tool && it->toolCallId == m.toolCallId
                        && !it->toolFinished) {
                        it->toolResult   = m.toolResult;
                        it->toolFinished = true;
                        it->toolHasError = m.toolHasError;
                        it->collapsed    = true;
                        skipPush         = true;
                        break;
                    }
                }
            } else {
                m.role = Message::Role::System;
                m.text = d.value("content", std::string{});
            }
            if (false == skipPush) {
                messages_.push_back(std::move(m));
            }
        }
        stickToBottom_ = true;
    }
    postRedraw();
}

asio::awaitable<neograph::json> AgentTUI::handleInterrupt(
    const std::string& threadId,
    const std::string& interruptNode,
    const std::string& interruptValue,
    const std::string& interruptArgJson
) {
    auto argOpt
        = agentxx::middleware::InterruptHandleArg::fromJson(neograph::json::parse(interruptArgJson)
        );
    if (!argOpt.has_value()) {
        co_return neograph::json::array();
    }
    const auto& handleArg = argOpt.value();

    // 标记进入中断输入等待: 使 Alt+Enter 把用户输入直接送入 inputChannel_ (避免死锁)
    awaitingInterruptInput_.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string msg = "Interrupted at: " + interruptNode + "\nValue: " + interruptValue;
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
