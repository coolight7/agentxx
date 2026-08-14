#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/input_bar.h"
#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/components/sidebar.h"
#include "agentxx-client/io/tui/components/status_bar.h"
#include "agentxx-client/mode_runners.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/expand/get_cpu_gpu_use.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
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
#include <atomic>
#include <charconv>
#include <cstdint>
#include <format>
#include <iostream>
#include <memory>

using namespace ftxui;

// ---------------------------------------------------------------------------
// 系统剪贴板写入 (跨平台)
// ---------------------------------------------------------------------------
// 复制鼠标选中文本时调用; 仅写入, 不读取。
//
// 实现:
// - Windows: Win32 API (OpenClipboard + SetClipboardData(CF_UNICODETEXT)),
//   对任何图形终端/控制台均可靠
// - 其他平台 (Linux/macOS/WSL): OSC 52 转义序列写入终端主剪贴板,
//   依赖终端模拟器支持 (xterm/Windows Terminal/wezterm/kitty 等; tmux 需配置)

#if defined(_WIN32)
#include <windows.h>

/// Windows: UTF-8 文本写入系统剪贴板 (UTF-8 -> UTF-16)
static bool copyTextToSystemClipboard(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    // OpenClipboard(nullptr): 不关联具体窗口, 供无 GUI 窗口句柄的线程使用
    if (!OpenClipboard(nullptr)) {
        return false;
    }
    EmptyClipboard();
    bool      ok = false;
    const int wlen
        = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wlen > 0) {
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wlen + 1) * sizeof(wchar_t));
        if (hMem) {
            wchar_t* dst = static_cast<wchar_t*>(GlobalLock(hMem));
            if (dst) {
                MultiByteToWideChar(
                    CP_UTF8,
                    0,
                    text.data(),
                    static_cast<int>(text.size()),
                    dst,
                    wlen
                );
                dst[wlen] = L'\0';
                GlobalUnlock(hMem);
                // 成功时剪贴板拥有 hMem 所有权; 失败则释放, 避免泄漏
                ok = SetClipboardData(CF_UNICODETEXT, hMem) != nullptr;
                if (!ok) {
                    GlobalFree(hMem);
                }
            } else {
                GlobalFree(hMem);
            }
        }
    }
    CloseClipboard();
    return ok;
}
#else

/// 其他平台: OSC 52 序列写入终端主剪贴板 (c = CLIPBOARD)
static bool copyTextToSystemClipboard(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    // ESC ] 52 ; c ; <base64> BEL — 无可见输出, 与 FTXUI 屏幕刷新流交错安全
    std::cout << "\x1b]52;c;" << agentxx::util::base64Encode(text) << "\x07" << std::flush;
    return true;
}
#endif

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

TUIClientAgentIO::TUIClientAgentIO(
    asio::any_io_executor          ex,
    std::string                    threadId,
    TUITheme                       theme,
    agentxx::agent::PermissionMode permissionMode
) :
    theme_(theme),
    threadId_(std::move(threadId)),
    ex_(ex),
    permissionMode_(permissionMode),
    inputChannel_(std::make_shared<LineChannel>(ex, 64)),
    logSink_(std::make_shared<TUILogSink>()) {
    // 注意: TUI 是纯 client 端点, 不持有 AgentContext/Session (属于 agent-server
    // 线程); 模型名/上下文统计等所有 agent 侧信息均经 Wire 消息 (WireModelInfo /
    // WireContextStats) 由服务端推送获取, cachedModelName 初始为空,
    // 收到 WireModelInfo 后更新 (调用方在连接建立后发送 WireGetModel 请求)
}

TUIClientAgentIO::~TUIClientAgentIO() {
    stop();
}

// ---------------------------------------------------------------------------
// postRedraw / enqueueUiAction
// ---------------------------------------------------------------------------

void TUIClientAgentIO::enqueueUiAction(std::function<void()> fn) {
    if (!fn) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(uiActionsMutex_);
        uiActions_.push_back(std::move(fn));
    }
    postRedraw();
}

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

void TUIClientAgentIO::showToast(std::string text) {
    // UI 线程独占状态 (openSessionSelector 等 UI 事件处理中调用):
    // 设置文本与起始时刻, 由渲染帧检查超时并清除 (见 start() 主渲染器)
    toastText_    = std::move(text);
    toastShownAt_ = std::chrono::steady_clock::now();
    if (!toastTimer_) {
        toastTimer_ = std::make_shared<asio::steady_timer>(ex_);
    }
    // 取消上一次挂起的等待 (连续提示时以最后一次为准重新计时)
    toastTimer_->cancel();
    toastTimer_->expires_after(kToastDuration);
    // 回调仅触发重绘, 不写 UI 状态 (toastText_ 为 UI 线程独占, 无锁);
    // 超时清除由 UI 线程下一帧渲染时执行, 避免跨线程数据竞争
    toastTimer_->async_wait([this](neograph_asio_error_code ec) {
        if (!ec) {
            postRedraw();
        }
    });
}

// ---------------------------------------------------------------------------
// 复制鼠标选中的文本
// ---------------------------------------------------------------------------

bool TUIClientAgentIO::copySelectionToClipboard() {
    std::shared_ptr<ScreenInteractive> s;
    {
        std::lock_guard<std::mutex> lock(screenMutex_);
        s = screen_;
    }
    if (!s) {
        return false;
    }
    // GetSelection(): 返回上一次绘制帧中累积的选中文本 (FTXUI 在每帧
    // Render 时按当前 Selection 收集各文本节点选中的部分);
    // TUI 为全屏模式, selection 坐标为屏幕绝对坐标, 无需校正
    const std::string text = s->GetSelection();
    if (text.empty()) {
        return false;
    }
    const bool ok = copyTextToSystemClipboard(text);
    if (ok) {
        showToast(fmt::format("已复制 ({})", text.size()));
    } else {
        showToast("复制失败 (剪贴板不可用)");
    }
    postRedraw();
    return true;
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
        ctx_.threadId       = currentThreadId();
        ctx_.remoteUrl      = remoteUrl_;
        ctx_.showSystemInfo = &TUISettings::instance().showSystemInfoRef();
        // 注意: 不设置 ctx_.session —— TUI 不持有 Session (属于 agent-server 线程),
        // 上下文统计经 WireContextStats → onContextStats → sharedState_ 更新,
        // 状态栏等组件从 frameState 读取 (见 status_bar.cpp)

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
                    std::make_shared<TUIMessage>(TUIMessage::makeText(TUIMessage::Role::User, text))
                );
                messageList_->setStickToBottom(true);
                inputChannel_->async_send(
                    neograph_asio_error_code{},
                    std::move(text),
                    [](neograph_asio_error_code) {}
                );
            } else if (st.connState != ConnState::Connected) {
                // agent-server 未就绪 (启动中 Connecting / 连接失败 Failed):
                // 输入进入待发送队列 (与流式输出期间排队语义一致, 经底部队列
                // 提示展示), 不置 isStreaming; 连接建立后由 flushPendingInput
                // 统一发送 — 若在未连接时直接调 sendUserInputLocked, 远程模式下
                // transport 的写队列尚未创建 (connect 完成前 writeQueue_ 为空),
                // 消息会被静默丢弃
                st.pendingInputs.push_back(TUIPendingInput{std::move(text), false});
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
            // LLM 上下文消息在 agent-server 侧 (Session::llmMessages), TUI 不持有,
            // 经 WireGetContext 由服务端回推 (WireContextMessages → onPeerMessage
            // → sharedState_.contextMessages); 本地/远程模式均有 transport
            if (transport_) {
                sendToPeer(agentxx::agent::WireGetContext{currentThreadId()});
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
            // 屏幕上方 toast 提示 (如会话切换警告): 渲染时检查超时, 超过
            // kToastDuration 自动清除 (toastText_/toastShownAt_ 为 UI 线程独占,
            // 仅在本帧渲染中读写, 无跨线程竞争); 显示期间以 dbox 叠加在
            // 主界面之上, 水平居中、垂直靠顶 (vbox 顶部 + filler 撑满)
            if (!toastText_.empty()) {
                const auto elapsed = std::chrono::steady_clock::now() - toastShownAt_;
                if (elapsed >= kToastDuration) {
                    toastText_.clear();
                } else {
                    body = dbox({
                        body,
                        vbox({
                            text(" "),
                            hbox({
                                filler(),
                                hbox({
                                    text(toastText_) | bold | bgcolor(theme_.buttonActiveBgColor)
                                        | color(theme_.buttonActiveTextColor),
                                }) | border,
                                filler(),
                            }),
                            filler(),
                        }),
                    });
                }
            }
            return body | bold | bgcolor(theme_.backgroundColor);
        });

        // 模态容器: 主布局 + 弹窗层
        modal_ = ModalContainer::Create(mainRenderer);
        modal_->setBgColor(theme_.backgroundColor);

        // 全局快捷键 + 鼠标 (F2/F3/F12/Escape/点击): 组件未消费的事件到此处理
        auto handler = CatchEvent(modal_, [&](Event event) -> bool {
            // 全局退出快捷键: 优先处理, 弹窗打开时也放行 (否则模态期间 Ctrl+C 被
            // 弹窗 OnEvent 无条件消费, 无法退出程序)
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
            // 鼠标事件: 拖选跟踪在任何状态下生效 (弹窗内文本同样支持拖选复制)
            if (event.is_mouse()) {
                const auto& mouse = event.mouse();
                // ---- 拖选跟踪 (松开即复制) ----
                // Windows Terminal 等终端会拦截 Ctrl+Insert 作为终端复制 (不转发给
                // 应用), 故以"左键按下并拖动后松开"作为主要复制入口:
                // - Pressed:   记录按下 (单击判定起点)
                // - Moved:     SGR 拖动事件仅在按键按下并移动时上报, 出现即视为拖选
                // - Released:  发生过拖动 -> 自动复制选中文本 (GetSelection 取上一
                //               绘制帧累积的选择, 已含本次拖动终点) + toast 提示
                // - 单击 (无拖动): 不复制, 保持原有的点击交互 (按钮/折叠/拖拽条等)
                if (mouse.button == Mouse::Left) {
                    if (mouse.motion == Mouse::Pressed) {
                        mouseDown_    = true;
                        mouseDragged_ = false;
                    } else if (mouse.motion == Mouse::Moved) {
                        if (mouseDown_) {
                            mouseDragged_ = true;
                        }
                    } else if (mouse.motion == Mouse::Released) {
                        const bool wasDrag = mouseDragged_;
                        mouseDown_         = false;
                        mouseDragged_      = false;
                        if (wasDrag && copySelectionToClipboard()) {
                            // 清除选中高亮: 复制已完成; 懒加载列表跳过 FTXUI
                            // 每帧 ComputeRequirement, Text 节点的选中反色不会
                            // 自动复位, 需显式清除 (见 resetSelectionHighlight)
                            if (messageList_) {
                                messageList_->clearSelectionHighlight();
                            }
                            if (sidebar_) {
                                sidebar_->clearSelectionHighlight();
                            }
                            // 消费事件: 拖选释放不应触发下方按钮/折叠点击;
                            // (handleSelection(handled=true) 将清除选择高亮,
                            // 复制已完成, 符合"松开即复制"语义)
                            return true;
                        }
                    }
                }
                // 模态弹窗打开时: 主界面不可见 (命中 box 为残留值), 跳过主界面
                // 按钮点击检测, 事件交予弹窗组件处理
                if (modal_->hasModal()) {
                    return false;
                }
                if (mouse.button == Mouse::Left && mouse.motion == Mouse::Released) {
                    // 连接失败 banner 的"重试"按钮点击 → 重新发起连接
                    if (messageList_ && messageList_->retryButtonBox().Contain(mouse.x, mouse.y)) {
                        requestRetry();
                        return true;
                    }
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
                    // Info 侧边栏 Plan 状态图按钮点击 → 打开弹窗
                    if (planDiagramButtonBox_.Contain(mouse.x, mouse.y)) {
                        openPlanDiagram();
                        return true;
                    }
                    // 状态栏模型区域点击 → 打开模型选择弹窗
                    if (statusBar_ && statusBar_->modelBox().Contain(mouse.x, mouse.y)) {
                        openModelSelector();
                        return true;
                    }
                    // 状态栏 "Sessions" 按钮点击 → 打开会话选择弹窗 (同 F4)
                    if (statusBar_ && statusBar_->sessionBox().Contain(mouse.x, mouse.y)) {
                        openSessionSelector();
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
            // 模态弹窗打开时: 键盘优先让弹窗处理 (Escape 关闭弹窗等), 不拦截
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
            if (event == Event::F4) {
                openSessionSelector();
                return true;
            }
            if (event == Event::F12) {
                toggleLogWindow();
                return true;
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
            // 启用括号粘贴 (bracketed paste, \x1B[?2004h): 终端将粘贴内容包裹于
            // \x1B[200~ ... \x1B[201~ 之间, InputComponent 据此拦截多行粘贴,
            // 粘贴的换行不会触发发送。与 FTXUI 的 Install() 一致, 控制序列
            // 无条件发送 (其自身亦不判断 stdout 是否为 tty)。
            std::cout << "\x1B[?2004h" << std::flush;
            while (!loop.HasQuitted()) {
                // 本帧请求基线: 帧期间到达 (被合并进本帧渲染) 的重绘请求在帧结束后补帧,
                // 保证用最新 frameState 重绘。必须在帧开头记录 —— 若在 RunOnceBlocking
                // 之后才记录, 帧开头到记录点之间到达的请求会丢失。
                const uint64_t frameBaseline = redrawSeq_.load(std::memory_order_acquire);
                // 每帧开头获取状态快照: 事件处理 (CatchEvent/组件 OnEvent) 与渲染
                // 期间 frameState 始终有效
                ctx_.frameState = sharedState_.readSnapshot();
                // 消费 client 线程投递的 UI 动作 (弹窗开关/消息列表吸附等):
                // 必须在渲染之前执行, 使本帧渲染反映其效果;
                // 动作仅访问 UI 线程独占组件 (modal_/messageList_), 不依赖本帧快照
                {
                    std::vector<std::function<void()>> actions;
                    {
                        std::lock_guard<std::mutex> lock(uiActionsMutex_);
                        actions.swap(uiActions_);
                    }
                    for (auto& fn : actions) {
                        fn();
                    }
                }
                // 日志 sink 排空 (丢弃防护: 不消费则生产者可能丢弃新日志):
                // 仅当 Logs tab 存在且为当前激活 tab 时才触发重绘 —— agent 运行时
                // 日志量很大, 若 tab 未打开, 日志变化不影响任何可见 UI, 每批日志
                // 都触发整帧渲染 (布局 + 全屏 ToString + stdout 写) 是纯浪费
                if (logSink_ && logSink_->pump() > 0 && sidebar_
                    && sidebar_->isTabActive(kLogTabId)) {
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
        // Loop 析构已触发 PostMain/Uninstall 恢复终端 (termios);
        // 但 2004 括号粘贴模式是终端模拟器状态而非 termios, 需在此显式关闭,
        // 否则残留模式会使后续程序粘贴时收到 \x1B[200~ 标记。
        std::cout << "\x1B[?2004l" << std::flush;
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
    // 关闭输入通道: 打断 handleInterrupt 中挂起在 getInput() 上的
    // async_receive, 使其尽快返回并结束协程 —— 否则中断等待输入期间退出时,
    // detached 协程残留挂起操作会阻塞 client io_context 的 run(), 进程无法退出。
    // (close 幂等; 之后 async_send/async_receive 以 channel_closed 错误返回)
    inputChannel_->close();
    // 同上: 关闭所有进行中中断的结果回传通道, 打断 handleInterrupt 中挂起在
    // async_receive 上的等待, 避免退出时残留挂起操作阻塞 io_context
    for (auto& [id, ch] : activeInterrupts_) {
        ch->close();
    }
    activeInterrupts_.clear();
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
    // 取消 toast 超时定时器: 避免退出后残留挂起等待触发 use-after-free
    // (回调捕获 this; cancel 后回调以 operation_aborted 返回, 不访问 this)
    if (toastTimer_) {
        toastTimer_->cancel();
    }
}

// ---------------------------------------------------------------------------
// agent-server 连接状态管理
//
// 覆盖场景:
// - 本地一体模式: TUI 启动后 agent 线程仍在 init() (MCP 连接等可能耗时数秒),
//   期间 connState=Connecting, banner 显示"启动中", 用户输入进入待发送队列;
//   SessionServerAgentIO 会话驱动循环启动前经 onServerReady() 置 Connected 并刷新队列
// - 远程模式: WS 连接握手期间 connState=Connecting; 连接失败置 Failed,
//   banner 显示失败 + [重试] 按钮, 用户点击后 requestRetry() 唤醒连接协程重试
// ---------------------------------------------------------------------------

void TUIClientAgentIO::setConnState(ConnState state) {
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        st.connState                   = state;
    }
    postRedraw();
}

void TUIClientAgentIO::onServerReady() {
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        st.connState                   = ConnState::Connected;
        // 启动完成: 清空进行中的启动步骤 (banner 切换到"启动完成 + 按键提示")
        st.startupProgress.clear();
    }
    postRedraw();
    // 刷新连接前排队的用户输入 (发送首条并置 isStreaming, 后续由 TurnEnd 分发)
    flushPendingInput();
}

void TUIClientAgentIO::onServerProgress(std::string_view step) {
    // agent 线程同步调用 (startupNotifier → onServerProgress):
    // 只更新当前步骤文本, 不触碰其他字段; 经 sharedState 锁避免与 UI 快照竞争
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        st.startupProgress             = std::string(step);
    }
    postRedraw();
}

void TUIClientAgentIO::flushPendingInput() {
    std::lock_guard<std::mutex> lock(sharedState_.mutex());
    auto&                       st = sharedState_.mutableState();
    // 与 TurnEnd/TurnResult 后的分发一致: 仅当未处于流式输出且队列非空时
    // 发送首条 (sendUserInputLocked 内部置 isStreaming, 其余排队输入随后续
    // 轮次结束依次分发)
    dispatchNextPendingInput(st);
}

void TUIClientAgentIO::requestRetry() {
    retryRequested_.store(true, std::memory_order_release);
    // 立即回到"启动中"提示: 连接协程 waitRetry 返回后会重新发起连接
    setConnState(ConnState::Connecting);
}

asio::awaitable<void> TUIClientAgentIO::waitRetry() {
    // 轮询等待用户点击 banner 的"重试"按钮 (requestRetry 置 retryRequested_):
    // - 100ms 轮询间隔, 点击延迟不可感知
    // - 每次轮询检查 running_: 连接失败期间用户可能退出 TUI, 等待协程须尽快
    //   返回, 否则 runner 协程永久挂起会阻塞 client io_context 的 run() 使进程无法退出
    asio::steady_timer timer(ex_);
    for (;;) {
        if (retryRequested_.exchange(false, std::memory_order_acq_rel)) {
            co_return;
        }
        if (!running_.load(std::memory_order_acquire)) {
            co_return;
        }
        timer.expires_after(std::chrono::milliseconds(100));
        auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        (void)ec;
    }
}

// ---------------------------------------------------------------------------
// 系统资源监控 (每 kSystemInfoIntervalSec 秒经 Wire 请求 agent-server 采集)
//
// 采集已迁移到 agent-server: TUI 不再本地读取 CPU/内存 (TUI 与 agent-server
// 可分属不同进程/主机, 远端模式下展示的是 server 主机的资源), 仅周期发送
// WireGetSystemUsage 请求, 服务端读取后回传 WireSystemUsage (由 client 线程
// onPeerMessage 写入 sharedState_.systemUsage)。采集周期由 client io_context
// 上的协程 (steady_timer) 驱动, 不占用 UI 线程。
// ---------------------------------------------------------------------------

void TUIClientAgentIO::startSystemMonitor() {
    if (sysMonitorRunning_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    sysMonitorTimer_ = std::make_shared<asio::steady_timer>(ex_);
    asio::co_spawn(
        ex_,
        [this]() -> asio::awaitable<void> {
            auto timer = sysMonitorTimer_;
            for (;;) {
                // 显示关闭时跳过请求 (仍保持周期唤醒, 以便随时重新开启)
                if (TUISettings::instance().showSystemInfo() && transport_) {
                    // 请求 agent-server 读取系统资源占用 (CPU/内存/GPU):
                    // 服务端经 blockingPool 采集后回传 WireSystemUsage,
                    // 由 onPeerMessage 更新 sharedState_.systemUsage 并重绘
                    sendToPeer(agentxx::agent::WireGetSystemUsage{});
                }
                // 周期等待; stop() 时 cancel() 使本等待立即返回并退出循环
                timer->expires_after(std::chrono::seconds(kSystemInfoIntervalSec));
                auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
                if (ec || !sysMonitorRunning_.load(std::memory_order_acquire)) {
                    break;
                }
            }
        },
        asio::detached
    );
}

void TUIClientAgentIO::stopSystemMonitor() {
    sysMonitorRunning_.store(false, std::memory_order_release);
    // 取消挂起的周期定时器, 使监控协程尽快退出 (detached 协程无法 join,
    // 依赖 timer cancel + 运行标志结束; 残留定时器会阻塞 client io_context 的 run())
    if (sysMonitorTimer_) {
        sysMonitorTimer_->cancel();
    }
}

// ---------------------------------------------------------------------------
// 模态管理
// ---------------------------------------------------------------------------

void TUIClientAgentIO::openModelSelector() {
    // agent-server 未就绪时请求会被 transport 丢弃 (远程模式写队列未创建/
    // 本地模式服务尚未启动), 弹窗将永远显示 loading; 提示用户等待连接完成
    if (ctx_.frameState && ctx_.frameState->connState != ConnState::Connected) {
        showToast("agent-server 尚未就绪, 请稍后再试");
        postRedraw();
        return;
    }
    if (transport_) {
        sendToPeer(agentxx::agent::WireGetModel{currentThreadId()});
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
        requestSelectModel(currentThreadId(), model);
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
        modal_->popModal();
    });
    // 主题变化后清空消息缓存与日志行缓存 (设置弹窗保持打开, 主题立即生效)。
    // 注意: 必须在 popModal() 之前访问成员 —— popModal() 会释放 overlay,
    // 而当前闭包存储在该 overlay 内, pop 之后闭包已析构, 再访问捕获变量属于 use-after-free。
    overlay->onThemeChange([this] {
        if (messageList_) {
            messageList_->invalidateCache();
        }
        // 日志行缓存: 已按旧主题着色, 整体清空并重置计数,
        // 使 renderLogWindow 下次渲染时按新主题重建全部日志行
        // (仅 clear 不清计数会因行数未变跳过重建, 导致日志侧边栏显示 [Empty])
        logLineCache_.clear();
        logCacheLineCount_   = 0;
        logCachePoppedCount_ = 0;
        // 模态容器背景色: setBgColor 仅在 start() 时设置一次, 主题切换后
        // 若不更新, 设置弹窗背景仍是旧主题背景色 (弹窗内部元素每帧重建,
        // 已自动使用新主题; 背景由 ModalContainer 的 bgColor_ 提供)
        if (modal_) {
            modal_->setBgColor(theme_.backgroundColor);
        }
    });
    // 日志等级变化: 清空已收集日志行 (重新按新等级收集);
    // logLineCache_ 因行数骤减在下次渲染时自动整体失效
    overlay->onLogLevelChange([this] {
        if (logSink_) {
            logSink_->clear();
        }
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

void TUIClientAgentIO::openPlanDiagram() {
    if (modal_ && !modal_->hasModal()) {
        auto overlay = std::make_shared<PlanDiagramOverlay>(ctx_);
        overlay->onClose([this] {
            modal_->popModal();
        });
        modal_->pushModal(overlay);
    }
    postRedraw();
}

void TUIClientAgentIO::openSessionSelector() {
    if (!modal_ || modal_->hasModal()) {
        return;
    }
    // agent-server 未就绪时 WireListSessions/WireSwitchSession 无法送达,
    // 会话列表将永远显示 loading; 提示用户等待连接完成
    if (ctx_.frameState && ctx_.frameState->connState != ConnState::Connected) {
        showToast("agent-server 尚未就绪, 请稍后再试");
        postRedraw();
        return;
    }
    // 仅当前会话非运行状态时可切换: 轮次进行中切换会话会使 Delta/输入错投,
    // 请先停止当前会话 (Esc 或点击停止)
    // 提示以屏幕上方 toast 展示 (3 秒自动消失), 不插入消息列表
    const bool busy = (ctx_.frameState && ctx_.frameState->isStreaming)
                      || awaitingInterruptInput_.load(std::memory_order_acquire);
    if (busy) {
        showToast("请先停止当前会话, 再进行会话切换");
        postRedraw();
        return;
    }
    // 请求服务端持久化会话列表 (WireSessionList 异步回填 sharedState);
    // 重置 loaded 标志使弹窗先显示 loading
    sharedState_.mutate([](TUIRenderState& st) {
        st.sessionListLoaded = false;
    });
    sendToPeer(agentxx::agent::WireListSessions{});

    auto overlay = std::make_shared<SessionSelectorOverlay>(ctx_);
    overlay->onClose([this] {
        modal_->popModal();
    });
    overlay->onSelect([this](std::string threadId) {
        switchToSession(std::move(threadId));
    });
    overlay->onNewSession([this] {
        // 新建会话: 生成全新 threadId 并切换 (无历史, 服务端回推空 Sync)
        const auto newThreadId = agentxx::client::generateUniqueThreadId();
        XX_LOGI("[tui] new session: {}", newThreadId);
        switchToSession(newThreadId);
    });
    modal_->pushModal(overlay);
    postRedraw();
}

void TUIClientAgentIO::switchToSession(std::string newThreadId) {
    if (newThreadId.empty() || newThreadId == currentThreadId()) {
        return;
    }
    XX_LOGI("[tui] switching session: {} -> {}", currentThreadId(), newThreadId);
    setCurrentThreadId(newThreadId);
    // 更新组件共享上下文: 状态栏/会话弹窗据此标记 current 会话
    ctx_.threadId = newThreadId;
    // 注意: TUI 不持有 Session (属于 agent-server 线程), 切换后服务端回推
    // 新会话的全量 Sync + WireModelInfo + WireContextStats (WireSwitchSession
    // 处理路径), 客户端界面 (消息历史/模型名/上下文统计) 随之整体更新
    // 清理上一会话遗留的消息列表吸附/中断 UI 状态;
    // 消息历史由服务端回推的全量 Sync 整体替换 (onSync)
    if (messageList_) {
        messageList_->setStickToBottom(true);
        messageList_->clearInterruptUiState();
    }
    awaitingInterruptInput_.store(false, std::memory_order_release);
    // 清理上一会话遗留的排队输入与上下文弹窗数据 (正常路径下切换前
    // isStreaming == false 时排队输入已清空, 此处兜底防御), 避免串扰新会话
    sharedState_.mutate([](TUIRenderState& st) {
        st.pendingInputs.clear();
        st.contextMessages.reset();
        st.showContextOverlay = false;
    });
    // WS 模式: 更新重连握手 threadId 并复位增量重放状态 (新会话 delta seq 独立编号);
    // Channel/进程内模式为 no-op
    if (transport_) {
        transport_->updateReconnectThreadId(newThreadId);
        sendToPeer(agentxx::agent::WireSwitchSession{newThreadId});
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
                        // 记录 wire id 供 handleInterrupt 使用 (同线程顺序执行);
                        // 过期通知 (WireInterruptExpired) 按该 id 匹配并终止等待
                        self->interruptWireId_ = req.id;
                        auto result
                            = co_await self
                                  ->handleInterrupt(req.threadId, req.node, req.value, req.argJson);
                        self->sendToPeer(agentxx::agent::WireInterruptResponse{req.id, result});
                    },
                    asio::detached
                );
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireInterruptExpired>) {
                // server 通知中断已过期 (超时/会话取消): 将对应未操作的中断消息
                // 标记为过期, 并关闭结果回传通道终止 handleInterrupt 的等待
                // (其返回已收集的结果, 不再等待用户操作)
                const auto expiredId = m.id;
                {
                    std::lock_guard<std::mutex> lock(sharedState_.mutex());
                    auto&                       st = sharedState_.mutableState();
                    for (size_t i = 0; i < st.messages.size(); ++i) {
                        const auto& msg = *st.messages[i];
                        if (msg.role == TUIMessage::Role::Interrupt && msg.interrupt
                            && msg.interrupt->interruptId == expiredId
                            && msg.interrupt->interruptStatus
                                   == TUIMessage::InterruptStatus::Waiting) {
                            auto& mm                      = sharedState_.mutableMessage(st, i);
                            mm.interrupt->interruptStatus = TUIMessage::InterruptStatus::Expired;
                        }
                    }
                }
                auto it = activeInterrupts_.find(expiredId);
                if (it != activeInterrupts_.end()) {
                    it->second->close();
                }
                postRedraw();
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
                    st.modelInfoLoaded = true;
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
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireSystemUsage>) {
                // agent-server 侧采集的系统资源占用响应 (WireGetSystemUsage 的回复):
                // 写入 sharedState_ (Info 侧边栏从 frameState 读取, 见
                // renderInfoSidebar), 采集周期由 startSystemMonitor 的定时器驱动
                {
                    std::lock_guard<std::mutex> lock(sharedState_.mutex());
                    auto&                       st = sharedState_.mutableState();
                    st.systemUsage
                        = std::make_shared<agentxx::expand::CpuGpuUsage>(std::move(m.usage));
                }
                postRedraw();
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireContextMessages>) {
                {
                    std::lock_guard<std::mutex> lock(sharedState_.mutex());
                    auto&                       st = sharedState_.mutableState();
                    st.contextMessages    = std::make_shared<neograph::json>(std::move(m.messages));
                    st.showContextOverlay = true;
                }
                // 打开上下文弹窗: 组件树由 UI 线程独占, 须投递到 UI 线程执行
                enqueueUiAction([this]() {
                    if (modal_ && !modal_->hasModal()) {
                        auto overlay = std::make_shared<ContextOverlay>(ctx_);
                        overlay->onClose([this] {
                            modal_->popModal();
                        });
                        modal_->pushModal(overlay);
                    }
                });
                postRedraw();
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireSessionList>) {
                // 会话选择弹窗数据源: 回填持久化会话列表 (服务端已按最近活动降序)
                {
                    std::lock_guard<std::mutex> lock(sharedState_.mutex());
                    auto&                       st = sharedState_.mutableState();
                    st.sessionList                 = m.sessions;
                    st.sessionListLoaded           = true;
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
    requestCancel(currentThreadId());
    pushCurrentTokenLocked(st);
    // 取消提示由 agent 线程确认取消后经 MessageTip Delta 插入 (原在此处
    // 即时插入 "[Cancel Request]", 迁移后由 agent 端统一插入保证历史一致)
    st.isStreaming = false;
    dispatchNextPendingInput(st);
}

void TUIClientAgentIO::sendUserInputLocked(TUIRenderState& st, std::string text) {
    pushCurrentTokenLocked(st);
    // 注意: 不能 move text (后续 sendToPeer/inputChannel 仍需要使用);
    // makeText 按值参数, 此处 lvalue 拷贝一次, 与原聚合初始化拷贝次数一致
    st.messages.push_back(
        std::make_shared<TUIMessage>(TUIMessage::makeText(TUIMessage::Role::User, text))
    );
    st.isStreaming = true;
    // 消息列表吸附到底部: messageList_ 为 UI 线程独占组件, 本函数可能被
    // client 线程 (dispatchNextPendingInput) 调用, 须投递到 UI 线程执行
    enqueueUiAction([this]() {
        if (messageList_) {
            messageList_->setStickToBottom(true);
        }
    });
    if (transport_) {
        sendToPeer(agentxx::agent::WireUserInput{currentThreadId(), text});
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
                if (st.currentTokenRole != role && st.currentToken && !st.currentToken->empty()) {
                    // 先 push 再更新时间戳: pushCurrentTokenLocked 使用
                    // st.pendingToken* 的当前值构造消息, 若先覆盖成新角色的
                    // 时间戳, 旧 token 的时长/开始时间会丢失 (修复)
                    pushCurrentTokenLocked(st);
                    st.pendingTokenStartTimeMs = delta.startTimeMs;
                    st.pendingTokenDurationMs  = delta.durationMs;
                }
                st.currentTokenRole = role;
                // 按需 COW: 仅当字符串被 UI 快照共享 (渲染期间) 才复制本体,
                // 避免每 token 深拷贝整个已累积文本 (O(n²) -> O(n))
                if (!st.currentToken) {
                    st.currentToken = std::make_shared<std::string>();
                    // 新流开始: 递增流身份 (COW 复制不递增, 见 currentTokenEpoch 注释)
                    ++st.currentTokenEpoch;
                } else if (st.currentToken.use_count() > 1) {
                    st.currentToken = std::make_shared<std::string>(*st.currentToken);
                }
                st.currentToken->append(delta.text);
                st.isStreaming = true;
            } break;
            case Type::ToolStart: {
                pushCurrentTokenLocked(st);
                auto m                = std::make_shared<TUIMessage>();
                m->role               = TUIMessage::Role::Tool;
                m->tool               = TUIMessage::ToolData{};
                m->tool->toolName     = delta.toolName;
                m->tool->toolCallId   = delta.toolCallId;
                m->text               = delta.arguments;
                m->tool->toolFinished = false;
                m->collapsed          = false;
                m->startTimeMs        = delta.startTimeMs;
                st.messages.push_back(std::move(m));
                st.isStreaming = true;
            } break;
            case Type::ToolEnd: {
                bool found = false;
                for (size_t i = st.messages.size(); i > 0; --i) {
                    auto& msg = *st.messages[i - 1];
                    if (msg.role == TUIMessage::Role::Tool && msg.tool
                        && msg.tool->toolCallId == delta.toolCallId && !msg.tool->toolFinished) {
                        auto& m              = sharedState_.mutableMessage(st, i - 1);
                        m.tool->toolResult   = delta.result;
                        m.tool->toolFinished = true;
                        m.collapsed          = true;
                        m.startTimeMs        = delta.startTimeMs;
                        m.durationMs         = delta.durationMs;
                        found                = true;
                        break;
                    }
                }
                if (!found) {
                    auto m                = std::make_shared<TUIMessage>();
                    m->role               = TUIMessage::Role::Tool;
                    m->tool               = TUIMessage::ToolData{};
                    m->tool->toolName     = delta.toolName;
                    m->tool->toolCallId   = delta.toolCallId;
                    m->tool->toolResult   = delta.result;
                    m->tool->toolFinished = true;
                    m->startTimeMs        = delta.startTimeMs;
                    m->durationMs         = delta.durationMs;
                    m->collapsed          = true;
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
            case Type::MessageUITip: {
                // 通用提示消息: 插入提示消息 (按级别区分显示);
                // 默认折叠展示 (提示类消息, 与 makeText 的 System 默认折叠语义一致)
                pushCurrentTokenLocked(st);
                auto msg       = std::make_shared<TUIMessage>();
                msg->role      = TUIMessage::Role::Tip;
                msg->text      = delta.text;
                msg->tip       = TUIMessage::TipData{};
                msg->collapsed = true;
                switch (delta.tipType) {
                    case agentxx::agent::Delta::TipType::Info:
                        msg->tip->tipLevel = TUIMessage::TipLevel::Info;
                        break;
                    case agentxx::agent::Delta::TipType::Warning:
                        msg->tip->tipLevel = TUIMessage::TipLevel::Warning;
                        break;
                    case agentxx::agent::Delta::TipType::Error:
                        msg->tip->tipLevel = TUIMessage::TipLevel::Error;
                        break;
                }
                st.messages.push_back(std::move(msg));
                st.isStreaming = true;
            } break;
            case Type::MessageTip: {
                // 系统消息: 已由 agent 线程插入会话历史 (viewMessages),
                // 内容/时间戳/级别与历史完全一致, 直接追加即可;
                // 默认折叠展示 (服务端历史创建时即 collapsed=true, 此处显式同步)
                pushCurrentTokenLocked(st);
                auto msg       = std::make_shared<TUIMessage>();
                msg->role      = TUIMessage::Role::Tip;
                msg->id        = delta.msgId;
                msg->text      = delta.text;
                msg->tip       = TUIMessage::TipData{};
                msg->collapsed = true;
                switch (delta.tipType) {
                    case agentxx::agent::Delta::TipType::Warning:
                        msg->tip->tipLevel = TUIMessage::TipLevel::Warning;
                        break;
                    case agentxx::agent::Delta::TipType::Error:
                        msg->tip->tipLevel = TUIMessage::TipLevel::Error;
                        break;
                    default:
                        msg->tip->tipLevel = TUIMessage::TipLevel::Info;
                        break;
                }
                msg->startTimeMs = delta.startTimeMs;
                msg->durationMs  = delta.durationMs;
                st.messages.push_back(std::move(msg));
            } break;
            case Type::TurnStart: {
                st.isStreaming = true;
            } break;
            case Type::TurnEnd: {
                st.currentNodeName.clear();
                pushCurrentTokenLocked(st);
                st.isStreaming = false;
                // 轮次统计系统提示由 agent 端插入 (MessageTip Delta),
                // UI 端不再自行构造
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
        // 单次 mutate (内部加锁): 不得在持锁状态下再调 mutate() ——
        // 旧实现先 lock_guard 再调 mutate() 会对同一非递归 mutex 二次加锁,
        // 造成客户端线程死锁 (TUI 启动握手即触发, 界面冻结)
        sharedState_.mutate([&](TUIRenderState& cur) {
            auto st              = std::make_shared<TUIRenderState>();
            auto prev            = sharedState_.snapshot();
            st->cachedModelName  = prev->cachedModelName;
            st->modelNames       = prev->modelNames;
            st->appendComponents = prev->appendComponents;
            st->pendingInputs    = prev->pendingInputs;
            st->systemUsage      = prev->systemUsage;
            st->contextMessages  = prev->contextMessages;
            st->isStreaming      = false;
            // 连接状态不随 Sync 重置: 握手后服务端回推全量 Sync 时若被重置回
            // Connecting (默认值), banner 会错误地回到"启动中"
            st->connState = prev->connState;
            // 启动进度不随 Sync 重置: 本地模式下握手后 init 仍在进行,
            // 期间服务端回推的早期 Sync 不应清空正在展示的启动步骤
            st->startupProgress = prev->startupProgress;

            // 历史消息与 server viewMessages 同型 (ViewMessage), 直接拷贝;
            // 原 json→TUIMessage 拆解逻辑已下沉到 server (event_stream 展开)
            st->messages.reserve(payload.messages.size());
            for (const auto& vm : payload.messages) {
                st->messages.push_back(std::make_shared<TUIMessage>(vm));
            }
            // 直接替换 (旧快照由 UI 线程持有, 自然释放)
            cur = std::move(*st);
        });
    }
    // 消息列表吸附到底部 + 清理中断 UI 状态 (消息整体替换, 旧状态随之失效):
    // 组件由 UI 线程独占, 经动作队列投递
    enqueueUiAction([this]() {
        if (messageList_) {
            messageList_->setStickToBottom(true);
            messageList_->clearInterruptUiState();
        }
    });
    postRedraw();
}

// ---------------------------------------------------------------------------
// onTurnResult / onContextStats (client 线程)
// ---------------------------------------------------------------------------

void TUIClientAgentIO::onTurnResult(const agentxx::agent::WireTurnResult& /*result*/) {
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        st.isStreaming                 = false;
        // 错误提示已由 agent 线程插入会话历史并经 MessageTip Delta 送达,
        // 此处不再自行构造
        dispatchNextPendingInput(st);
    }
    postRedraw();
}

void TUIClientAgentIO::onContextStats(const agentxx::agent::WireContextStats& stats) {
    // 上下文统计写入 sharedState_ (而非 Session::contextStats —— TUI 不持有
    // Session, 属于 agent-server 线程); 状态栏等组件从 frameState 读取。
    // 会话切换后服务端推送新会话的 WireContextStats (#switchSession 路径),
    // 显示自动跟随新会话
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        st.contextTokens               = stats.contextTokens;
        st.maxContextTokens            = stats.maxContextTokens;
        st.tps                         = stats.tps;
    }
    postRedraw();
}

// ---------------------------------------------------------------------------
// handleInterrupt (client 线程, co_spawn)
//
// 中断输入直接渲染在消息列表中 (Role::Interrupt 消息内嵌交互控件), 不弹窗:
// - 每个输入项一条中断消息, 共享同一结果回传通道 (经 MessageListComponent
//   attachInterruptChannel 注入 UI 线程)
// - UI 线程 (MessageListComponent) 确认/取消后经通道发送 {inputIndex, value}
// - 本协程收集全部输入项结果后按序组装返回; 收到整体取消 (inputIndex=-1) 或
//   通道关闭 (server 过期通知 / TUI 退出) 时终止, 返回已收集结果
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

    // ---- 权限询问 (interruptNode == "permission") 的客户端侧处理 ----
    // 服务端权限处理器经 InterruptHandleArg.arg 透传权限上下文 {category, target}:
    // - category: 权限分类 ("filesystem_read" / "filesystem_write"), 决定规则作用域
    // - target:   受约束目标 (已标准化的绝对路径, 与中间件规则匹配口径一致)
    const bool  isPermission = (interruptNode == "permission");
    std::string permCategory;
    std::string permTarget;
    if (isPermission) {
        if (handleArg.arg.is_object()) {
            permCategory = handleArg.arg.value("category", std::string{});
            permTarget   = handleArg.arg.value("target", std::string{});
        }
        const std::string_view shownTarget
            = permTarget.empty() ? interruptValue : std::string_view{permTarget};
        // 客户端兜底处理 (模式来自 yaml 配置 `permission.mode`):
        // 中间件已注册的显式规则 (ALLOW/DENY) 在服务端先行判定, 能走到这里
        // 说明服务端策略为 INTERRUPT (如远程 server 与本地配置不一致时)。
        // - pass: 视为允许, 无需用户介入
        // - deny: 视为拒绝, 无需用户介入
        // - ask/all_ask: 询问用户 (ask 模式下工作目录内的路径由服务端规则
        //   直接放行, 到达客户端的均为需要询问的路径)
        if (permissionMode_ == agentxx::agent::PermissionMode::Pass) {
            {
                std::lock_guard<std::mutex> lock(sharedState_.mutex());
                auto&                       st = sharedState_.mutableState();
                st.messages.push_back(std::make_shared<TUIMessage>(TUIMessage::makeText(
                    TUIMessage::Role::Tip,
                    fmt::format("[Permission] Pass mode: allow {} ({})", shownTarget, permCategory)
                )));
            }
            postRedraw();
            co_return neograph::json::array({"true"});
        }
        if (permissionMode_ == agentxx::agent::PermissionMode::Deny) {
            {
                std::lock_guard<std::mutex> lock(sharedState_.mutex());
                auto&                       st = sharedState_.mutableState();
                st.messages.push_back(std::make_shared<TUIMessage>(TUIMessage::makeText(
                    TUIMessage::Role::Tip,
                    fmt::format("[Permission] Deny mode: reject {} ({})", shownTarget, permCategory)
                )));
            }
            postRedraw();
            co_return neograph::json::array({"false"});
        }
    }

    awaitingInterruptInput_.store(true, std::memory_order_release);

    // 中断头消息已由 agent 线程插入会话历史并经 MessageTip Delta 送达
    // (在发起中断请求前插入, 顺序先于本函数的输入项消息), 此处不再构造

    // 每个输入项一条中断消息 (共享结果通道)
    const int64_t wireId      = interruptWireId_;
    auto          ch          = std::make_shared<InterruptResultChannel>(ex_, 64);
    activeInterrupts_[wireId] = ch;

    // 结果回传通道注入 UI 线程 (MessageListComponent 中断 UI 状态表):
    // 通道由 client 线程创建, UI 线程交互 (确认/取消) 需经其发送结果;
    // 权限询问标记 rememberable: 渲染"记住"开关, 用户可勾选记住本次选择
    enqueueUiAction([this, wireId, ch, rememberable = isPermission]() {
        if (messageList_) {
            messageList_->attachInterruptChannel(wireId, ch, rememberable);
        }
    });

    const int total = static_cast<int>(handleArg.inputs.size());
    int       index = 0;
    for (const auto& input : handleArg.inputs) {
        ++index;
        if (input.type.empty()) {
            continue;
        }
        auto m                     = std::make_shared<TUIMessage>();
        m->role                    = TUIMessage::Role::Interrupt;
        m->interrupt               = TUIMessage::InterruptData{};
        m->interrupt->interruptId  = wireId;
        m->interrupt->inputLabel   = input.label;
        m->interrupt->inputDepict  = input.depict;
        m->interrupt->inputType    = input.type;
        m->interrupt->inputDefault = input.defaultValue;
        m->interrupt->inputEnums   = input.enumValues;
        m->interrupt->inputIndex   = index;
        m->interrupt->inputTotal   = total;
        // 编辑文本/选中项等纯 UI 状态由 MessageListComponent 按
        // interrupt->inputType/inputDefault/inputEnums 惰性初始化, 不存于消息
        {
            std::lock_guard<std::mutex> lock(sharedState_.mutex());
            auto&                       st = sharedState_.mutableState();
            st.messages.push_back(std::move(m));
        }
        postRedraw();
    }

    // 收集结果: 各输入项确认后按 inputIndex 回填 (支持任意顺序确认),
    // 整体取消 (inputIndex=-1) 或通道关闭 (过期/退出) 时终止
    auto                                    result = neograph::json::array();
    std::vector<std::optional<std::string>> values(total);
    size_t                                  confirmedCount = 0;
    bool                                    remember = false; // 权限询问: 记住本次选择
    while (confirmedCount < values.size()) {
        auto [ec, idx, val, rem] = co_await ch->async_receive(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            // 通道关闭: server 过期通知 / TUI 退出 → 终止, 返回已收集结果
            break;
        }
        if (idx < 0) {
            // 用户整体取消
            break;
        }
        if (idx < 1 || idx > total || values[static_cast<size_t>(idx - 1)].has_value()) {
            continue; // 防御: 非法/重复序号
        }
        values[static_cast<size_t>(idx - 1)]  = val;
        remember                             |= rem;
        ++confirmedCount;
    }
    for (auto& v : values) {
        if (v.has_value()) {
            result.push_back(std::move(*v));
        }
    }

    // 记住本次选择: 将路径规则注册到服务端权限中间件,
    // 后续访问该路径或其子目录时按本次允许/拒绝处理, 不再询问
    if (isPermission && remember && !permTarget.empty() && confirmedCount > 0
        && values[0].has_value()) {
        const auto&  v     = *values[0];
        const bool   allow = (v == "true" || v == "yes");
        const size_t index
            = (permCategory == "filesystem_write")
                  ? agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
                  : agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionREAD;
        if (transport_) {
            sendToPeer(agentxx::agent::WireSetPermission{
                .threadId = std::string{threadId},
                .path     = permTarget,
                .allow    = allow,
                .index    = index,
            });
        }
    }

    activeInterrupts_.erase(wireId);
    awaitingInterruptInput_.store(false, std::memory_order_release);
    // 中断流程结束 (全部确认/取消/过期): 清理 UI 线程的 channel 映射与
    // 该请求的 UI 状态 (消息已固定为 Confirmed/Cancelled/Expired, 状态行
    // 渲染不再需要编辑状态)
    enqueueUiAction([this, wireId]() {
        if (messageList_) {
            messageList_->releaseInterruptChannel(wireId);
        }
    });
    co_return result;
}

asio::awaitable<std::optional<std::string>> TUIClientAgentIO::getInput() {
    auto [ec, line] = co_await inputChannel_->async_receive(asio::as_tuple(asio::use_awaitable));
    if (ec) {
        co_return std::nullopt;
    }
    co_return std::optional<std::string>(std::move(line));
}
