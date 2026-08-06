#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/input_bar.h"
#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/components/sidebar.h"
#include "agentxx-client/io/tui/components/status_bar.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/expand/get_cpu_gpu_use.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/string_util.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
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
#include <memory>

using namespace ftxui;

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

TUIClientAgentIO::TUIClientAgentIO(
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

TUIClientAgentIO::~TUIClientAgentIO() {
    stop();
}

// ---------------------------------------------------------------------------
// postRedraw
// ---------------------------------------------------------------------------

void TUIClientAgentIO::postRedraw() {
    // 合并同一时刻的多次重绘请求: 流式输出时 client 线程每 token 调用一次,
    // 若每次 Post 都触发完整渲染, 渲染成本被 token 到达频率放大。
    // - redrawPosted_: 合并标记, 仅当无在途 Custom 事件时才 Post
    // - redrawSeq_: 请求计数, UI 线程在帧结束时据此补帧 (见 start() 帧循环):
    //   帧期间到达的请求可能被合并进本帧渲染, 而本帧 frameState 快照取自帧开头,
    //   渲染结果可能未反映其状态变更, 需要补一帧以最新快照重绘
    redrawSeq_.fetch_add(1, std::memory_order_acq_rel);
    if (redrawPosted_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
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

void TUIClientAgentIO::start() {
    running_ = true;
    if (logSink_) {
        agentxx::util::LogDispatcher::instance().addSink(logSink_);
    }
    startSystemMonitor();

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
        ctx_.theme          = &theme_;
        ctx_.session        = session_;
        ctx_.threadId       = threadId_;
        ctx_.remoteUrl      = remoteUrl_;
        ctx_.showSystemInfo = &TUISettings::instance().showSystemInfoRef();

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
                // json 拷贝是深拷贝 (yyjson 全树复制), 仅此低频场景一次, 可接受
                st.contextMessages = std::make_shared<neograph::json>(session_->llmMessages);
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
                    text(fmt::format("  · Message Queue: {}", st.pendingInputs.size()))
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
            if (event == Event::CtrlC) {
                running_ = false;
                // 关闭 transport 以打断可能阻塞中的 connect/重连/recv 循环,
                // 使主协程能尽快走到退出分支 (尤其远程模式 server 不可达时 connect 会无限重连)
                if (transport_) {
                    transport_->close();
                }
                screen->Exit();
                return true;
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
                    // 状态栏模型区域点击 → 打开模型选择弹窗
                    if (statusBar_ && statusBar_->modelBox().Contain(mouse.x, mouse.y)) {
                        openModelSelector();
                        return true;
                    }
                    // 状态栏 "Settings" 按钮点击 → 打开设置弹窗
                    if (statusBar_ && statusBar_->settingsBox().Contain(mouse.x, mouse.y)) {
                        openSettings();
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
                // 本帧请求基线: 帧期间到达 (被合并进本帧渲染) 的重绘请求在帧结束后补帧,
                // 保证用最新 frameState 重绘。必须在帧开头记录 —— 若在 RunOnceBlocking
                // 之后才记录, 帧开头到记录点之间到达的请求会丢失。
                const uint64_t frameBaseline = redrawSeq_.load(std::memory_order_acquire);
                // 每帧开头获取状态快照: 事件处理 (CatchEvent/组件 OnEvent) 与渲染
                // 期间 frameState 始终有效
                ctx_.frameState = sharedState_.readSnapshot();
                if (logSink_ && logSink_->pump() > 0) {
                    screen->Post(Event::Custom);
                }
                loop.RunOnceBlocking();
                // 整帧渲染/事件处理完成: 释放本帧状态快照。
                // 关键性能点: 若帧间隙持续持有快照, client 线程 (onDelta 流式追加)
                // 每次 mutableState() 都会 COW 深拷贝整个 TUIRenderState (use_count>1),
                // 每 token 一次全量拷贝; 释放后渲染间隙 use_count==1,
                // client 线程可原地修改 state, 拷贝成本降为零。
                // Element 树在 OnRender 中已自包含 (文本已复制), 布局/绘制不依赖快照。
                ctx_.frameState.reset();
                // 帧完成: 本帧消费的在途 Custom 已对应一次渲染 (frameState 为本帧开头快照)。
                // 清除在途标记, 使新的 postRedraw 能重新 Post;
                // 若帧期间有新请求被合并 (计数 != 帧基线), 说明本帧渲染可能未反映其
                // 状态变更 (快照取于帧开头), 补 Post 一帧, 保证以最新快照重绘。
                // 注意: 不能在帧开头 reset —— FTXUI 的 RunUntilIdle 会在同一帧内消费
                // 事件处理期间 Post 的 Custom, 帧开头 reset 后帧中到达的请求会因合并
                // 标记为真而被丢弃且无在途事件, 导致重绘丢失 (如模型弹窗列表不刷新)。
                redrawPosted_.store(false, std::memory_order_release);
                if (redrawSeq_.load(std::memory_order_acquire) != frameBaseline
                    && !redrawPosted_.exchange(true, std::memory_order_acq_rel)) {
                    screen->Post(Event::Custom);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(screenMutex_);
            screen_ = nullptr;
        }
    });
}

void TUIClientAgentIO::stop() {
    if (logSink_) {
        agentxx::util::LogDispatcher::instance().removeSink(logSink_);
    }
    running_ = false;
    // 关闭 transport, 使 runTransportLoop/connect/重连循环退出, io_context 得以排空
    if (transport_) {
        transport_->close();
    }
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
    stopSystemMonitor();
}

// ---------------------------------------------------------------------------
// 系统资源监控 (每 kSystemInfoIntervalSec 秒采集一次 CPU/内存占用)
// ---------------------------------------------------------------------------

void TUIClientAgentIO::startSystemMonitor() {
    if (sysMonitorThread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sysMonitorMutex_);
        sysMonitorStop_ = false;
    }
    sysMonitorThread_ = std::thread([this]() {
        // CpuGpuMonitor 内部缓存上次 CPU 采样值, 同一实例连续查询才能得到准确的 CPU 占用率,
        // 因此在循环外构造一次, 跨采集周期复用
        agentxx::expand::CpuGpuMonitor monitor;
        for (;;) {
            // 显示关闭时跳过采集 (仍保持周期唤醒, 以便随时重新开启)
            if (TUISettings::instance().showSystemInfo()) {
                // query() 为协程, 需 io_context 驱动; 每次采集使用临时 io_context 同步等待完成
                asio::io_context io;
                auto             usage = std::make_shared<agentxx::expand::CpuGpuUsage>();
                asio::co_spawn(
                    io,
                    [&]() -> asio::awaitable<void> {
                        *usage = co_await monitor.query();
                    },
                    asio::detached
                );
                io.run();
                {
                    std::lock_guard<std::mutex> lock(sharedState_.mutex());
                    auto&                       st = sharedState_.mutableState();
                    st.systemUsage                 = std::move(usage);
                }
                postRedraw();
            }
            // 周期睡眠; stop 时被 cv 唤醒立即退出
            std::unique_lock<std::mutex> lock(sysMonitorMutex_);
            sysMonitorCv_.wait_for(lock, std::chrono::seconds(kSystemInfoIntervalSec), [this] {
                return sysMonitorStop_;
            });
            if (sysMonitorStop_) {
                break;
            }
        }
    });
}

void TUIClientAgentIO::stopSystemMonitor() {
    {
        std::lock_guard<std::mutex> lock(sysMonitorMutex_);
        sysMonitorStop_ = true;
    }
    sysMonitorCv_.notify_all();
    if (sysMonitorThread_.joinable()) {
        sysMonitorThread_.join();
    }
}

// ---------------------------------------------------------------------------
// 模态管理
// ---------------------------------------------------------------------------

void TUIClientAgentIO::openModelSelector() {
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

void TUIClientAgentIO::openSettings() {
    auto overlay = std::make_shared<SettingsOverlay>(ctx_);
    overlay->onClose([this] {
        // 主题变化后清空消息缓存。
        // 注意: 必须在 popModal() 之前访问成员 —— popModal() 会释放 overlay,
        // 而当前闭包存储在该 overlay 内, pop 之后闭包已析构, 再访问捕获变量属于 use-after-free。
        if (messageList_) {
            messageList_->invalidateCache();
        }
        logLineCache_.clear();
        modal_->popModal();
    });
    modal_->pushModal(overlay);
    postRedraw();
}

void TUIClientAgentIO::toggleLogWindow() {
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

void TUIClientAgentIO::requestCancel(std::string threadId) {
    if (transport_) {
        sendToPeer(agentxx::agent::WireCancel{std::move(threadId)});
    }
}

void TUIClientAgentIO::requestSelectModel(std::string threadId, std::string model) {
    if (transport_) {
        sendToPeer(agentxx::agent::WireSelectModel{std::move(threadId), std::move(model)});
    }
}

// ---------------------------------------------------------------------------
// onPeerMessage (client 线程)
// ---------------------------------------------------------------------------

void TUIClientAgentIO::onPeerMessage(agentxx::agent::WireMessage msg) {
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
                    st.contextMessages = std::make_shared<neograph::json>(std::move(m.messages));
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

void TUIClientAgentIO::pushCurrentTokenLocked(TUIRenderState& st) {
    if (!st.currentToken || st.currentToken->empty()) {
        return;
    }
    auto msg         = std::make_shared<TUIMessage>();
    msg->role        = st.currentTokenRole;
    msg->text        = *st.currentToken;
    msg->collapsed   = (st.currentTokenRole == TUIMessage::Role::Thinking);
    msg->durationMs  = st.pendingTokenDurationMs;
    msg->startTimeMs = st.pendingTokenStartTimeMs;
    st.messages.push_back(std::move(msg));
    st.pendingTokenDurationMs  = 0;
    st.pendingTokenStartTimeMs = 0;
    st.currentToken.reset();
}

void TUIClientAgentIO::cancelCurrentRunLocked(TUIRenderState& st) {
    requestCancel(threadId_);
    pushCurrentTokenLocked(st);
    st.messages.push_back(
        std::make_shared<TUIMessage>(TUIMessage{TUIMessage::Role::System, "[Cancel Request]"})
    );
    st.isStreaming = false;
    dispatchNextPendingInput(st);
}

void TUIClientAgentIO::sendUserInputLocked(TUIRenderState& st, std::string text) {
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

void TUIClientAgentIO::dispatchNextPendingInput(TUIRenderState& st) {
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

void TUIClientAgentIO::onDelta(const agentxx::agent::Delta& delta) {
    using Type = agentxx::agent::Delta::Type;
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        switch (delta.type) {
            case Type::TextToken:
            case Type::ThinkingToken: {
                auto role = (delta.type == Type::ThinkingToken) ? TUIMessage::Role::Thinking
                                                                : TUIMessage::Role::Assistant;
                if (st.currentTokenRole != role && st.currentToken
                    && !st.currentToken->empty()) {
                    st.pendingTokenStartTimeMs = delta.startTimeMs;
                    st.pendingTokenDurationMs  = delta.durationMs;
                    pushCurrentTokenLocked(st);
                }
                st.currentTokenRole = role;
                // 按需 COW: 仅当字符串被 UI 快照共享 (渲染期间) 才复制本体,
                // 避免每 token 深拷贝整个已累积文本 (O(n²) -> O(n))
                if (!st.currentToken) {
                    st.currentToken = std::make_shared<std::string>();
                } else if (st.currentToken.use_count() > 1) {
                    st.currentToken = std::make_shared<std::string>(*st.currentToken);
                }
                st.currentToken->append(delta.text);
                st.isStreaming = true;
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
                if (st.currentToken && !st.currentToken->empty()) {
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
                st.currentNodeName.clear();
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

void TUIClientAgentIO::onSync(const agentxx::agent::SyncPayload& payload) {
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto                        st   = std::make_shared<TUIRenderState>();
        auto                        prev = sharedState_.snapshot();
        st->cachedModelName              = prev->cachedModelName;
        st->modelNames                   = prev->modelNames;
        st->appendComponents             = prev->appendComponents;
        st->pendingInputs                = prev->pendingInputs;
        st->systemUsage                  = prev->systemUsage;
        st->contextMessages              = prev->contextMessages;
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

void TUIClientAgentIO::onTurnResult(const agentxx::agent::WireTurnResult& result) {
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        st.isStreaming                 = false;
        if (result.hasError && !result.errorMessage.empty()) {
            st.messages.push_back(std::make_shared<TUIMessage>(
                TUIMessage{TUIMessage::Role::System, fmt::format("[Error] {}", result.errorMessage)}
            ));
        }
        dispatchNextPendingInput(st);
    }
    postRedraw();
}

void TUIClientAgentIO::onContextStats(const agentxx::agent::WireContextStats& stats) {
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

asio::awaitable<neograph::json> TUIClientAgentIO::handleInterrupt(
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
            XX_LOGE("TUIClientAgentIO::handleInterrupt json::parse failed: {}", errinfo);
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
            msg += fmt::format("\nHandle: {}", handleArg.name);
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

asio::awaitable<std::optional<std::string>> TUIClientAgentIO::getInput() {
    auto [ec, line] = co_await inputChannel_->async_receive(asio::as_tuple(asio::use_awaitable));
    if (ec) {
        co_return std::nullopt;
    }
    co_return std::optional<std::string>(std::move(line));
}
