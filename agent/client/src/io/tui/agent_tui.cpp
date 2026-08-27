#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/input_bar.h"
#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/components/sidebar.h"
#include "agentxx-client/io/tui/components/status_bar.h"
#include "agentxx-client/mode_runners.h"
#include "agentxx/agent/model_registry.h"
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
    std::string                    sessionId,
    TUITheme                       theme,
    agentxx::agent::PermissionMode permissionMode
) :
    theme_(theme),
    sessionId_(std::move(sessionId)),
    ex_(ex),
    permissionMode_(permissionMode),
    inputChannel_(std::make_shared<LineChannel>(ex, 64)),
    logSink_(std::make_shared<TUILogSink>()) {
    // 注意: TUI 是纯 client 端点, 不持有 AgentContext/Session (属于 agent-io
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
// 插件适配器接口 (TuiPluginAdapter 调用; 任意线程安全)
// ---------------------------------------------------------------------------

void TUIClientAgentIO::uiToast(std::string text, int level) {
    (void)level; // 一期统一样式; 后续可按级别着色
    // toastText_/toastShownAt_ 为 UI 线程独占, 投递到 UI 线程执行
    enqueueUiAction([this, text = std::move(text)]() {
        showToast(std::move(text));
    });
}

void TUIClientAgentIO::sendPluginUserInput(std::string text) {
    if (text.empty()) {
        return;
    }
    // 与用户输入同排队语义 (见 start() 中 inputCfg.onSend 的分支逻辑):
    // - 中断等待输入: 直接投递到输入通道 (由中断流程消费)
    // - 未连接 (Connecting/Failed): 进待发送队列
    // - 流式中: 进待发送队列 (轮次结束后分发)
    // - 空闲: 立即发送
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        if (awaitingInterruptInput_.load(std::memory_order_acquire)) {
            pushCurrentTokenLocked(st);
            resetTrailingRunningToolsLocked(st);
            st.messages.push_back(
                std::make_shared<TUIMessage>(TUIMessage::makeText(TUIMessage::Role::User, text))
            );
            // messageList_ 为 UI 线程独占组件, 投递到 UI 线程执行
            enqueueUiAction([this]() {
                if (messageList_) {
                    messageList_->setStickToBottom(true);
                }
            });
            inputChannel_->async_send(
                neograph_asio_error_code{},
                std::move(text),
                [](neograph_asio_error_code) {}
            );
            return; // 中断输入不触发事件接收器 (属于中断响应, 非用户消息)
        }
        if (st.connState != ConnState::Connected) {
            XX_LOGW("[tui] sendPluginUserInput dropped: agent-io not connected");
            return;
        }
        sendUserInputLocked(st, std::move(text)); // 内部通知事件接收器
    }
    postRedraw();
}

bool TUIClientAgentIO::sendPluginDataUp(
    const std::string& plugin,
    const std::string& event,
    const std::string& json
) {
    // client io 线程调用 (adapter); 未连接时 sendToPeer 丢弃并记日志
    if (!transport_ || !transport_->alive()) {
        XX_LOGW("[tui] sendPluginDataUp dropped (no transport): {}.{}", plugin, event);
        return false;
    }
    agentxx::agent::WirePluginDataUp up;
    up.plugin = plugin;
    up.event  = event;
    up.data   = json;
    sendToPeer(std::move(up));
    return true;
}

void TUIClientAgentIO::notifyUserInputSent(const std::string& sessionId, const std::string& text) {
    // 事件接收器回调须在 client io 线程 (ClientEventSink 约定):
    // - 本函数可能被 client 线程 (sendUserInputLocked ← dispatchNextPendingInput)
    //   或 UI 线程 (inputCfg.onSend) 调用; 统一 post 到 io 线程执行
    //   (低频事件, post 延迟一个事件循环 tick 可接受)
    auto self = shared_from_this();
    asio::post(ex_, [self, tid = sessionId, t = text]() mutable {
        self->emitEventSink([&](agentxx::agent::ClientEventSink& sink) {
            sink.onUserInput(tid, t);
        });
    });
}

void TUIClientAgentIO::addPluginPanelTab(const std::string& id, const std::string& title) {
    // UI 线程调用 (适配器经 postToUi 投递); sidebar_ 为 UI 线程独占组件
    if (!sidebar_ || sidebar_->hasTab(id)) {
        return;
    }
    sidebar_->addTab(id, title, [this, id]() {
        return renderPluginPanel(id);
    });
    postRedraw();
}

void TUIClientAgentIO::removePluginPanelTab(const std::string& id) {
    // UI 线程调用 (适配器经 postToUi 投递); sidebar_ 为 UI 线程独占组件
    if (!sidebar_) {
        return;
    }
    sidebar_->removeTab(id);
    postRedraw();
}

std::vector<ScrollItem> TUIClientAgentIO::renderPluginPanel(const std::string& panelId) {
    // UI 线程调用 (侧边栏 tab render 回调); 读取注册表快照 (短锁拷贝 shared_ptr)
    std::vector<ScrollItem> out;
    auto                    mgr = pluginManager_;
    if (!mgr) {
        return out;
    }
    auto reg = mgr->uiRegistrySnapshot();
    if (!reg) {
        return out;
    }
    const agentxx::plugin::ClientPanel* panel = nullptr;
    for (const auto& p : reg->panels) {
        if (p.id == panelId) {
            panel = &p;
            break;
        }
    }
    if (!panel) {
        return out;
    }
    const auto& theme = theme_;
    // 面板内容: items JSON 数组 (kind: text/progress/action/badge/separator;
    // text 支持 role: title=高亮 / normal=普通(默认) / hint=减淡)
    if (panel->items.is_array()) {
        for (const auto& it : panel->items) {
            if (!it.is_object()) {
                continue;
            }
            const auto kind = it.value("kind", std::string{"text"});
            if (kind == "text") {
                const auto role = it.value("role", std::string{"normal"});
                const auto txt  = text(it.value("text", std::string{}));
                if (role == "title") {
                    out.push_back(ScrollItem{txt | color(theme.accentColor) | bold});
                } else if (role == "hint") {
                    out.push_back(ScrollItem{txt | color(theme.hintColor)});
                } else {
                    out.push_back(ScrollItem{txt | color(theme.normalColor)});
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
                out.push_back(ScrollItem{hbox({
                    text("[" + bar + "]") | color(theme.accentColor),
                    text(fmt::format(" {}%", static_cast<int>(v * 100))) | color(theme.hintColor),
                })});
            } else if (kind == "action") {
                out.push_back(ScrollItem{
                    text("◈ " + it.value("label", std::string{"(action)"}))
                    | color(theme.buttonActiveTextColor) | bold
                });
            } else if (kind == "badge") {
                out.push_back(ScrollItem{
                    text("● " + it.value("text", std::string{})) | color(theme.accentColor)
                });
            } else if (kind == "separator") {
                out.push_back(ScrollItem{text("─") | color(theme.hintColor) | dim});
            }
        }
    }
    return out;
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
        // 历史分页钩子: MessageListComponent 滚动接近窗口顶部时触发
        // (requestOlderHistory 内部做在途去重与边界判断; sendToPeer 经
        // transport 写队列投递, 线程安全)
        ctx_.requestMoreHistory = [this] {
            requestOlderHistory();
        };
        // 会话列表分页钩子: SessionSelectorOverlay 选择项接近已加载列表末尾时
        // 触发预取下一页 (requestNextSessionListPage 内部做在途去重与边界判断)
        ctx_.requestMoreSessions = [this] {
            requestNextSessionListPage();
        };
        ctx_.theme         = &theme_;
        ctx_.sessionId     = currentSessionId();
        ctx_.remoteUrl     = remoteUrl_;
        ctx_.pluginManager = pluginManager_;
        // 注意: 不设置 ctx_.session —— TUI 不持有 Session (属于 agent-io 线程),
        // 上下文统计经 WireContextStats → onContextStats → sharedState_ 更新,
        // 状态栏等组件从 frameState 读取 (见 status_bar.cpp)

        // 创建组件
        messageList_ = std::make_shared<MessageListComponent>(ctx_);
        statusBar_   = std::make_shared<StatusBarComponent>(ctx_);
        sidebar_     = std::make_shared<SidebarComponent>(ctx_);

        // tabs 竖向列表的常驻标签: Info/Logs 始终显示 (对应 tab 未创建时点击经
        // ensure 回调创建并激活; 已激活再点一次取消激活隐藏内容区)
        sidebar_->setPinnedTabs({
            {std::string(kInfoTabId),
             "Info", [this]() {
                 ensureInfoSidebarTab();
             }},
            {std::string(kLogTabId),
             "Logs", [this]() {
                 ensureLogSidebarTab();
             }},
        });

        InputComponent::Config inputCfg;
        inputCfg.onSend = [this](std::string text) -> bool {
            // ---- 插件命令拦截 (UI 线程) ----
            // 输入以 "/" 开头且匹配插件注册的命令时, 拦截并投递到 client io
            // 线程执行命令回调 (execute 返回动作 JSON, 由宿主解释执行);
            // 未命中命令照常作为普通消息发送
            if (pluginManager_ && !text.empty() && text[0] == '/') {
                const auto spacePos = text.find(' ');
                const auto cmdName  = text.substr(
                    1,
                    spacePos == std::string::npos ? std::string::npos : spacePos - 1
                );
                if (!cmdName.empty() && pluginManager_->hasCommand(cmdName)) {
                    // 参数: 剩余部分整体作为 {"text": "..."} 传入 (语义由插件定义)
                    std::string argsText
                        = spacePos == std::string::npos ? std::string{} : text.substr(spacePos + 1);
                    neograph::json args = neograph::json::object();
                    args["text"]        = argsText;
                    pluginManager_->postCommandInvocation(cmdName, args.dump());
                    return true;
                }
            }
            std::lock_guard<std::mutex> lock(sharedState_.mutex());
            auto&                       st = sharedState_.mutableState();
            if (awaitingInterruptInput_.load(std::memory_order_acquire)) {
                pushCurrentTokenLocked(st);
                resetTrailingRunningToolsLocked(st);
                st.messages.push_back(
                    std::make_shared<TUIMessage>(TUIMessage::makeText(TUIMessage::Role::User, text))
                );
                messageList_->setStickToBottom(true);
                inputChannel_->async_send(
                    neograph_asio_error_code{},
                    std::move(text),
                    [](neograph_asio_error_code) {}
                );
                return true;
            } else if (st.connState != ConnState::Connected) {
                // agent-io 未初始化完成前不允许发送消息, 且不清空输入框
                showToast("agent-io 尚未就绪, 请稍后再试");
                postRedraw();
                return false;
            } else {
                sendUserInputLocked(st, std::move(text));
                return true;
            }
        };
        inputCfg.isAwaitingInterrupt = [this] {
            return awaitingInterruptInput_.load(std::memory_order_acquire);
        };
        inputCfg.isStreaming = [this] {
            return ctx_.frameState && ctx_.frameState->isStreaming;
        };
        inputBar_ = std::make_shared<InputComponent>(ctx_, std::move(inputCfg));

        // 屏幕足够宽时默认展开信息侧边栏
        // (Info/Logs 标签无论如何都常驻显示于 tabs 竖向列表)
        if (Terminal::Size().dimx >= kInfoSidebarMinWidth) {
            ensureInfoSidebarTab();
        }

        // 侧边栏 footer 点击: 处理 "上下文" 按钮
        sidebar_->onFooterClick([this](const Mouse&) -> bool {
            // LLM 上下文消息在 agent-io 侧 (Session::llmMessages), TUI 不持有,
            // 经 WireGetContext 由服务端回推 (WireContextMessages → onPeerMessage
            // → sharedState_.contextMessages); 本地/远程模式均有 transport
            if (transport_) {
                sendToPeer(agentxx::agent::WireGetContext{currentSessionId()});
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
            // client 插件 UI 注册表快照 (工具消息装饰等; 每帧刷新, 渲染期无锁读)
            ctx_.frameState->pluginRegistry
                = pluginManager_ ? pluginManager_->uiRegistrySnapshot() : nullptr;
            const auto& st = *ctx_.frameState;

            Element pendingBar = text("");
            if (!st.pendingInputs.empty()) {
                pendingBar = hbox({
                    text(" "),
                    text(fmt::format("  · Message Queue: {}", st.pendingInputs.size()))
                        | color(theme_.accentColor) | bold | reflect(pendingCounterBox_),
                    text(" "),
                    text(" [insert] ") | bgcolor(theme_.buttonBgColor)
                        | color(theme_.buttonTextColor) | bold | reflect(pendingInsertButtonBox_),
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

            // 侧边栏 tabs 竖向列表常驻显示 (Info/Logs 固定标签),
            // 故始终参与布局; 无激活 tab 时其内容区自动隐藏、仅显示列表
            Element body = hbox({
                mainWidget | flex,
                sidebar_->Render(),
            });
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
                    // 可折叠消息点击 (Think/Tool 展开/折叠)
                    if (messageList_ && messageList_->handleCollapsibleClick(mouse)) {
                        return true;
                    }
                    // 待发送消息队列 insert 按钮点击 → 取消当前轮次并立即从队列弹出执行
                    if (!ctx_.frameState->pendingInputs.empty()
                        && pendingInsertButtonBox_.Contain(mouse.x, mouse.y)) {
                        if (transport_) {
                            sendToPeer(agentxx::agent::WireInterruptAndRunNext{currentSessionId()});
                        }
                        std::lock_guard<std::mutex> lock(sharedState_.mutex());
                        auto&                       st = sharedState_.mutableState();
                        if (st.isStreaming) {
                            pushCurrentTokenLocked(st);
                        }
                        postRedraw();
                        return true;
                    }
                    // 待发送消息计数点击
                    if (!ctx_.frameState->pendingInputs.empty()
                        && pendingCounterBox_.Contain(mouse.x, mouse.y)) {
                        auto overlay = std::make_shared<PendingInputsOverlay>(ctx_);
                        overlay->onClear([this] {
                            if (transport_) {
                                sendToPeer(agentxx::agent::WireClearMessageQueue{currentSessionId()}
                                );
                            }
                        });
                        overlay->onDeleteItem([this](std::string itemId) {
                            if (transport_) {
                                sendToPeer(agentxx::agent::WireRemoveQueueItem{
                                    currentSessionId(),
                                    std::move(itemId)
                                });
                            }
                        });
                        overlay->onClose([this] {
                            modal_->popModal();
                        });
                        modal_->pushModal(overlay);
                        postRedraw();
                        return true;
                    }
                    // Info 侧边栏 Append "Failed" 组 [view] 按钮点击 → 打开失败组件弹窗
                    if (failedViewButtonBox_.Contain(mouse.x, mouse.y)) {
                        openFailedAppendComponents();
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
                // client 插件 UI 注册表快照 (同 mainRenderer 帧首; 供事件处理
                // 路径读取装饰等插件注册数据)
                ctx_.frameState->pluginRegistry
                    = pluginManager_ ? pluginManager_->uiRegistrySnapshot() : nullptr;
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
    // 取消 toast 超时定时器: 避免退出后残留挂起等待触发 use-after-free
    // (回调捕获 this; cancel 后回调以 operation_aborted 返回, 不访问 this)
    if (toastTimer_) {
        toastTimer_->cancel();
    }
}

// ---------------------------------------------------------------------------
// agent-io 连接状态管理
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
    // 通知事件接收器 (client 插件系统订阅连接状态事件)
    const char* stateName = state == ConnState::Connected
                                ? "connected"
                                : (state == ConnState::Failed ? "failed" : "connecting");
    emitEventSink([&](agentxx::agent::ClientEventSink& sink) {
        sink.onConnStateChanged(stateName, {});
    });
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
    // 通知事件接收器: 服务端就绪 (基类默认实现)
    agentxx::agent::AgentIOBase::onServerReady();
}

void TUIClientAgentIO::onServerProgress(std::string_view step) {
    // agent 线程同步调用 (initNotifier → onServerProgress):
    // 只更新当前步骤文本, 不触碰其他字段; 经 sharedState 锁避免与 UI 快照竞争
    std::string stepCopy{step};
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        st.startupProgress             = stepCopy;
    }
    postRedraw();
    // 通知事件接收器 (agent 线程 → post 到 client io 线程)
    auto self = shared_from_this();
    asio::post(ex_, [self, step = std::move(stepCopy)]() mutable {
        self->emitEventSink([&](agentxx::agent::ClientEventSink& sink) {
            sink.onConnStateChanged("connecting", step);
        });
    });
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
// 模态管理
// ---------------------------------------------------------------------------

void TUIClientAgentIO::openModelSelector() {
    // agent-io 未就绪时请求会被 transport 丢弃 (远程模式写队列未创建/
    // 本地模式服务尚未启动), 弹窗将永远显示 loading; 提示用户等待连接完成
    if (ctx_.frameState && ctx_.frameState->connState != ConnState::Connected) {
        showToast("agent-io 尚未就绪, 请稍后再试");
        postRedraw();
        return;
    }
    if (transport_) {
        sendToPeer(agentxx::agent::WireGetModel{currentSessionId()});
    }
    auto overlay = std::make_shared<ModelSelectorOverlay>(ctx_);
    auto snap    = sharedState_.readSnapshot();
    for (size_t i = 0; i < snap->modelNames.size(); ++i) {
        if (snap->modelNames[i] == snap->cachedModelName) {
            overlay->setInitialIndex(static_cast<int>(i));
            break;
        }
    }
    // 确认选择: 不即时通知 agent-io 切换 (不发送 WireSelectModel), 仅记录为
    // 待应用选择 (setPendingModel), 随下一次发送的用户消息 (WireUserInput.model)
    // 携带, BaseAgent 执行新一轮会话时 (runTurnAsync 开头 selectModel) 自动切换。
    // 状态栏显示已由 confirmSelection 更新 cachedModelName, 此处仅登记待应用
    overlay->onConfirm([this](std::string model) {
        setPendingModel(std::move(model));
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

void TUIClientAgentIO::ensureInfoSidebarTab() {
    if (!sidebar_->hasTab(kInfoTabId)) {
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
}

void TUIClientAgentIO::ensureLogSidebarTab() {
    if (!sidebar_->hasTab(kLogTabId)) {
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
}

void TUIClientAgentIO::toggleLogWindow() {
    if (sidebar_->hasTab(kLogTabId)) {
        sidebar_->removeTab(kLogTabId);
    } else {
        ensureLogSidebarTab();
    }
    postRedraw();
}

void TUIClientAgentIO::openFailedAppendComponents() {
    if (modal_ && !modal_->hasModal()) {
        auto overlay = std::make_shared<FailedComponentsOverlay>(ctx_);
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
    // agent-io 未就绪时 WireListSessions/WireSwitchSession 无法送达,
    // 会话列表将永远显示 loading; 提示用户等待连接完成
    if (ctx_.frameState && ctx_.frameState->connState != ConnState::Connected) {
        showToast("agent-io 尚未就绪, 请稍后再试");
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
    // 请求服务端持久化会话列表首页 (WireSessionList 异步回填 sharedState);
    // 重置分页状态使弹窗先显示 loading; keyset 游标 beforeMs=0 表示从最新开始,
    // 仅取 kSessionListPageSize 条, 浏览到末尾时经 ctx.requestMoreSessions 续取
    sharedState_.mutate([](TUIRenderState& st) {
        st.sessionList.clear();
        st.sessionListLoaded      = false;
        st.sessionListHasMore     = false;
        st.sessionListLoadingMore = false;
        st.sessionListTotalCount  = 0;
    });
    requestSessionListPage(0, "", kSessionListPageSize);

    auto overlay = std::make_shared<SessionSelectorOverlay>(ctx_);
    overlay->onClose([this] {
        modal_->popModal();
    });
    overlay->onSelect([this](std::string sessionId) {
        switchToSession(std::move(sessionId));
    });
    overlay->onNewSession([this] {
        // 新建会话: 生成全新 sessionId 并切换 (无历史, 服务端回推空 Sync)
        const auto newThreadId = agentxx::client::generateUniqueSessionId();
        XX_LOGI("[tui] new session: {}", newThreadId);
        switchToSession(newThreadId);
    });
    modal_->pushModal(overlay);
    postRedraw();
}

void TUIClientAgentIO::switchToSession(std::string newThreadId) {
    if (newThreadId.empty() || newThreadId == currentSessionId()) {
        return;
    }
    XX_LOGI("[tui] switching session: {} -> {}", currentSessionId(), newThreadId);
    setCurrentSessionId(newThreadId);
    // 更新组件共享上下文: 状态栏/会话弹窗据此标记 current 会话
    ctx_.sessionId = newThreadId;
    // 注意: TUI 不持有 Session (属于 agent-io 线程), 切换后服务端回推
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
    // WS 模式: 更新重连握手 sessionId 并复位增量重放状态 (新会话 delta seq 独立编号);
    // Channel/进程内模式为 no-op
    if (transport_) {
        transport_->updateReconnectSessionId(newThreadId);
        sendToPeer(agentxx::agent::WireSwitchSession{newThreadId});
    }
    postRedraw();
    // 通知事件接收器: 会话切换 (post 到 client io 线程)
    {
        auto self = shared_from_this();
        asio::post(ex_, [self, tid = newThreadId]() mutable {
            self->emitEventSink([&](agentxx::agent::ClientEventSink& sink) {
                sink.onSessionSwitched(tid);
            });
        });
    }
}

// ---------------------------------------------------------------------------
// requestCancel / setPendingModel
// ---------------------------------------------------------------------------

void TUIClientAgentIO::requestCancel(std::string sessionId) {
    if (transport_) {
        sendToPeer(agentxx::agent::WireCancel{std::move(sessionId)});
    }
}

void TUIClientAgentIO::setPendingModel(std::string model) {
    if (model.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(sharedState_.mutex());
    sharedState_.mutableState().pendingModel = std::move(model);
}

// ---------------------------------------------------------------------------
// onPeerMessage (client 线程)
// ---------------------------------------------------------------------------

void TUIClientAgentIO::onPeerMessage(agentxx::agent::WireMessage msg) {
    std::visit(
        [this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, agentxx::agent::Delta>) {
                // 通知事件接收器 (client 插件系统订阅 delta 事件)
                emitEventSink([&](agentxx::agent::ClientEventSink& sink) {
                    sink.onDelta(m);
                });
                onDelta(m);
            } else if constexpr (std::is_same_v<T, agentxx::agent::SyncPayload>) {
                onSync(m);
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireTurnResult>) {
                // 通知事件接收器 (client 插件系统订阅轮次结束事件)
                emitEventSink([&](agentxx::agent::ClientEventSink& sink) {
                    sink.onTurnResult(m);
                });
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
                        auto result            = co_await self->handleInterrupt(
                            req.sessionId,
                            req.node,
                            req.value,
                            req.argJson
                        );
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
            } else if constexpr (std::is_same_v<T, agentxx::agent::WirePluginData>) {
                // 插件事件转发 (WirePluginData): 原样通知事件接收器 (client
                // 插件系统订阅跨端插件数据事件)。渲染由各插件的 client 侧
                // 入口完成 (如 agentxx_codegraph 经订阅更新侧边栏面板、
                // agentxx_system_monitor 周期采集的 usage 事件更新状态栏项),
                // TUI 不再解析/保存插件载荷
                emitEventSink([&](agentxx::agent::ClientEventSink& sink) {
                    sink.onPluginData(m);
                });
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
                // 会话选择弹窗数据源: 分页响应回填/追加到已加载会话列表
                onSessionListPage(m);
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireMessageQueueUpdate>) {
                onMessageQueueUpdate(m);
            } else if constexpr (std::is_same_v<T, agentxx::agent::WireViewMessagesPage>) {
                // 历史分页响应: 前插到已加载窗口上方 (向上滚动加载更早历史)
                onViewMessagesPage(m);
            }
        },
        std::move(msg)
    );
}

// ---------------------------------------------------------------------------
// 协议处理辅助 (调用方须持有 sharedState_.mutex())
// ---------------------------------------------------------------------------

void TUIClientAgentIO::resetTrailingRunningToolsLocked(TUIRenderState& st) {
    for (size_t i = st.messages.size(); i > 0; --i) {
        const auto& msg = *st.messages[i - 1];
        if (msg.role != TUIMessage::Role::Tool) {
            break;
        }
        if (msg.tool && !msg.tool->toolFinished) {
            auto& m              = sharedState_.mutableMessage(st, i - 1);
            m.tool->toolFinished = true;
        }
    }
}

void TUIClientAgentIO::pushCurrentTokenLocked(TUIRenderState& st) {
    if (!st.hasPendingToken()) {
        return;
    }
    resetTrailingRunningToolsLocked(st);
    auto msg  = std::make_shared<TUIMessage>();
    msg->role = st.currentTokenRole;
    if (st.currentToken && !st.currentToken->empty()) {
        msg->text = *st.currentToken;
    }
    if (st.currentTokenRole == TUIMessage::Role::Think) {
        msg->collapsed = true;
        if (st.pendingTokenThink.has_value()) {
            msg->think = *st.pendingTokenThink;
        }
    }
    msg->durationMs  = st.pendingTokenDurationMs;
    msg->startTimeMs = st.pendingTokenStartTimeMs;
    st.messages.push_back(std::move(msg));
    st.pendingTokenDurationMs  = 0;
    st.pendingTokenStartTimeMs = 0;
    st.pendingTokenThink.reset();
    st.currentToken.reset();
}

void TUIClientAgentIO::cancelCurrentRunLocked(TUIRenderState& st) {
    requestCancel(currentSessionId());
    pushCurrentTokenLocked(st);
    resetTrailingRunningToolsLocked(st);
    // 取消提示由 agent 线程确认取消后经 MessageTip Delta 插入 (原在此处
    // 即时插入 "[Cancel Request]", 迁移后由 agent 端统一插入保证历史一致)
    st.isStreaming = false;
}

void TUIClientAgentIO::sendUserInputLocked(TUIRenderState& st, std::string text) {
    resetTrailingRunningToolsLocked(st);
    // 事件接收器通知用原文 (inputChannel 分支会 move text, 提前拷贝)
    const std::string notifyText = text;
    // 待应用模型选择: 取走后随本条消息携带给 agent-io (WireUserInput.model),
    // BaseAgent 执行新一轮会话时自动切换; 清空使模型选择仅对"选择之后发送的
    // 下一条消息"生效
    std::string pendingModel = std::move(st.pendingModel);
    st.pendingModel.clear();
    if (transport_) {
        sendToPeer(agentxx::agent::WireUserInput{currentSessionId(), text, std::move(pendingModel)}
        );
    } else {
        // 无 transport (遗留直连模式): 输入经本地 channel 送达, 无法携带
        // 模型选择, 已取走的 pendingModel 直接丢弃 (该模式下不切换模型)
        inputChannel_->async_send(
            neograph_asio_error_code{},
            std::move(text),
            [](neograph_asio_error_code) {}
        );
    }
    // 通知事件接收器 (用户输入事件; 任意线程安全, 内部按需 post 到 io 线程)
    notifyUserInputSent(currentSessionId(), notifyText);
}

void TUIClientAgentIO::onMessageQueueUpdate(const agentxx::agent::WireMessageQueueUpdate& update) {
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        if (st.pendingInputs.empty() && update.items.empty()) {
            return;
        }
        std::map<std::string, bool> expandedMap;
        for (const auto& pi : st.pendingInputs) {
            auto key         = pi.id.empty() ? pi.text : pi.id;
            expandedMap[key] = pi.expanded;
        }
        st.pendingInputs.clear();
        for (const auto& item : update.items) {
            TUIPendingInput pi;
            pi.id          = item.id;
            pi.text        = item.text;
            pi.model       = item.model;
            pi.createdAtMs = item.createdAtMs;
            auto key       = pi.id.empty() ? pi.text : pi.id;
            if (expandedMap.count(key)) {
                pi.expanded = expandedMap[key];
            }
            st.pendingInputs.push_back(std::move(pi));
        }
    }
    postRedraw();
}

// ---------------------------------------------------------------------------
// 历史分页 (client 线程)
//
// 长会话恢复时服务端仅同步末尾窗口 (SyncPayload.fromIndex = 窗口起始绝对
// 下标), 用户向上滚动到已加载窗口顶部时经 WireGetViewMessages 分页拉取
// 更早历史; 页响应在此前插到本地窗口上方并做滚动锚定。
// viewMessages 为 append-only, 绝对下标恒定, 前插不影响既有下标。
// ---------------------------------------------------------------------------

void TUIClientAgentIO::onViewMessagesPage(const agentxx::agent::WireViewMessagesPage& page) {
    size_t prependedCount = 0;
    bool   anchored       = false;
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        // 在途标志复位 (无论本页是否可用, 请求生命周期已结束)
        st.historyLoading = false;
        // 会话不匹配: 切换会话后迟到的旧页响应, 丢弃
        if (!page.sessionId.empty() && page.sessionId != currentSessionId()) {
            XX_LOGW(
                "[tui] drop stale history page (session {} != {})",
                page.sessionId,
                currentSessionId()
            );
            return;
        }
        if (page.messages.empty()) {
            // 空页: 无更早历史 (或会话不存在), 窗口起点归零终止后续触发
            st.historyWindowStart = 0;
            if (page.totalCount > 0) {
                st.historyTotal = page.totalCount;
            }
            return;
        }
        // 连续性校验: 页尾必须紧贴当前窗口首条 (分页请求按序应答且单在途,
        // 不连续说明窗口已被 Sync 整体替换, 本页过期丢弃)
        const uint64_t pageEnd = page.startIndex + page.messages.size();
        if (st.historyWindowStart != 0 || !st.messages.empty()) {
            if (pageEnd != st.historyWindowStart) {
                XX_LOGW(
                    "[tui] drop non-contiguous history page ([{}, {}) vs window start {})",
                    page.startIndex,
                    pageEnd,
                    st.historyWindowStart
                );
                return;
            }
        }
        prependedCount = page.messages.size();
        anchored       = !st.messages.empty();
        // ViewMessage → TUIMessage (shared_ptr) 转换后按页内顺序整体前插
        std::vector<std::shared_ptr<TUIMessage>> converted;
        converted.reserve(page.messages.size());
        for (const auto& vm : page.messages) {
            converted.push_back(std::make_shared<TUIMessage>(vm));
        }
        st.messages.insert(st.messages.begin(), converted.begin(), converted.end());
        st.historyWindowStart = page.startIndex;
        if (page.totalCount > st.historyTotal) {
            st.historyTotal = page.totalCount;
        }
    }
    // 滚动锚定: LazyScrollable 为 UI 线程独占, 经动作队列在帧间执行。
    // anchored=false 表示首屏填充 (前插前无消息), 无需稳定旧视口内容
    enqueueUiAction([this, prependedCount, anchored]() {
        if (messageList_ && anchored) {
            messageList_->onHistoryPrepended(prependedCount);
        }
    });
    postRedraw();
}

void TUIClientAgentIO::requestOlderHistory() {
    uint64_t beforeIndex = 0;
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        // 边界判断 + 在途去重 (UI 线程滚动事件可能高频触发)
        if (st.historyLoading || !st.hasMoreHistory()) {
            return;
        }
        st.historyLoading = true;
        beforeIndex       = st.historyWindowStart;
    }
    requestViewMessagesPage(currentSessionId(), beforeIndex, kHistoryPageSize);
}

// ---------------------------------------------------------------------------
// 会话列表分页 (client 线程)
//
// 会话选择弹窗数据源按 keyset 游标分页加载: 打开弹窗时请求最新一页, 用户浏览
// 到已加载列表末尾时以上一页最后一条为游标续取 (SessionSelectorOverlay 经
// ctx_.requestMoreSessions 触发), 避免会话很多时一次性加载/渲染全量。
// ---------------------------------------------------------------------------

void TUIClientAgentIO::onSessionListPage(const agentxx::agent::WireSessionList& resp) {
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        // 在途标志复位 (无论本页是否可用, 请求生命周期已结束)
        st.sessionListLoadingMore = false;
        // 旧版服务端兼容: 响应无分页元数据 (totalCount==0 && !hasMore) 时视为
        // 全量列表, 直接替换本地列表
        const bool legacyFullList = (resp.totalCount == 0 && !resp.hasMore);
        if (!st.sessionListLoaded || legacyFullList || st.sessionList.empty()) {
            // 首页 / 全量响应: 替换
            st.sessionList = resp.sessions;
        } else {
            // 后续页: 追加 (服务端保证不与已收页重叠; 双重防御跳过重复项)
            for (const auto& s : resp.sessions) {
                bool dup = false;
                for (const auto& e : st.sessionList) {
                    if (e.sessionId == s.sessionId) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    st.sessionList.push_back(s);
                }
            }
        }
        st.sessionListLoaded = true;
        if (resp.totalCount > 0) {
            st.sessionListTotalCount = resp.totalCount;
        }
        // hasMore 边界: 服务端标志 + 空页防御 (keyset 边界处可能多给一页空响应,
        // 此时终止续取) + 已加载数达到总数时收敛
        st.sessionListHasMore = resp.hasMore && !resp.sessions.empty()
                                && (st.sessionListTotalCount == 0
                                    || st.sessionList.size() < st.sessionListTotalCount);
    }
    postRedraw();
}

void TUIClientAgentIO::requestNextSessionListPage() {
    int64_t     beforeMs = 0;
    std::string beforeId;
    {
        std::lock_guard<std::mutex> lock(sharedState_.mutex());
        auto&                       st = sharedState_.mutableState();
        // 边界判断 + 在途去重 (UI 线程滚动事件可能高频触发)
        if (!st.sessionListLoaded || !st.sessionListHasMore || st.sessionListLoadingMore
            || st.sessionList.empty()) {
            return;
        }
        st.sessionListLoadingMore = true;
        // 游标取已加载列表最后一条 (排序最旧), 服务端返回严格排在其后的至多一页
        beforeMs = st.sessionList.back().lastActiveMs;
        beforeId = st.sessionList.back().sessionId;
    }
    requestSessionListPage(beforeMs, std::move(beforeId), kSessionListPageSize);
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
            case Type::ThinkToken: {
                if (delta.type == Type::ThinkToken && delta.text.empty()) {
                    // 空文本 ThinkToken: 加密思考载体或思考元数据更新 (如
                    // reasoning_tokens/duration) 若当前正在累积明文 Think 流 (尚未收到正文
                    // TextToken / ToolStart), 先将已累积的思考文本落盘到 messages
                    if (st.currentTokenRole == TUIMessage::Role::Think && st.hasPendingToken()) {
                        pushCurrentTokenLocked(st);
                    }

                    bool updatedExisting = false;
                    for (size_t i = st.messages.size(); i > 0; --i) {
                        const auto r = st.messages[i - 1]->role;
                        if (r == TUIMessage::Role::User || r == TUIMessage::Role::Tool
                            || r == TUIMessage::Role::Assistant) {
                            break; // 遇到用户输入、工具执行或正文回答边界，不得跨越更新前序动作的
                                   // Think
                        }
                        if (r == TUIMessage::Role::Think) {
                            auto& m = sharedState_.mutableMessage(st, i - 1);
                            if (delta.think) {
                                if (!m.think) {
                                    m.think = *delta.think;
                                } else {
                                    if (delta.think->reasoningTokens > 0) {
                                        m.think->reasoningTokens = delta.think->reasoningTokens;
                                    }
                                    if (delta.think->isEncrypted) {
                                        m.think->isEncrypted = true;
                                    }
                                }
                            }
                            if (delta.durationMs > 0) {
                                m.durationMs = delta.durationMs;
                            }
                            if (delta.startTimeMs > 0 && m.startTimeMs == 0) {
                                m.startTimeMs = delta.startTimeMs;
                            }
                            updatedExisting = true;
                            break;
                        }
                    }
                    if (updatedExisting) {
                        st.isStreaming = true;
                        break;
                    }
                    // 当前轮次/步骤尚无 Think 消息 (如加密思考首包): 开始思考时立即在消息列表中创建
                    // Think 消息展示
                    pushCurrentTokenLocked(st);
                    resetTrailingRunningToolsLocked(st);
                    auto msg       = std::make_shared<TUIMessage>();
                    msg->role      = TUIMessage::Role::Think;
                    msg->collapsed = true;
                    if (delta.think) {
                        msg->think = *delta.think;
                    } else {
                        msg->think = TUIMessage::ThinkData{
                            .reasoningTokens = 0,
                            .isEncrypted     = true,
                        };
                    }
                    msg->startTimeMs
                        = delta.startTimeMs > 0 ? delta.startTimeMs : st.pendingTokenStartTimeMs;
                    msg->durationMs = delta.durationMs;
                    st.messages.push_back(std::move(msg));
                    st.currentTokenRole = TUIMessage::Role::Assistant;
                    st.isStreaming      = true;
                    break;
                }

                auto role = (delta.type == Type::ThinkToken) ? TUIMessage::Role::Think
                                                             : TUIMessage::Role::Assistant;
                if (st.currentTokenRole != role && st.hasPendingToken()) {
                    // 先 push 再更新时间戳: pushCurrentTokenLocked 使用
                    // st.pendingToken* 的当前值构造消息, 若先覆盖成新角色的
                    // 时间戳, 旧 token 的时长/开始时间会丢失 (修复)
                    pushCurrentTokenLocked(st);
                    st.pendingTokenStartTimeMs = delta.startTimeMs;
                    st.pendingTokenDurationMs  = delta.durationMs;
                }
                st.currentTokenRole = role;
                if (delta.type == Type::ThinkToken && delta.think.has_value()) {
                    st.pendingTokenThink = delta.think;
                }
                if (delta.startTimeMs > 0 && st.pendingTokenStartTimeMs == 0) {
                    st.pendingTokenStartTimeMs = delta.startTimeMs;
                }
                if (delta.durationMs > 0) {
                    st.pendingTokenDurationMs = delta.durationMs;
                }
                // 按需 COW: 仅当字符串被 UI 快照共享 (渲染期间) 才复制本体,
                // 避免每 token 深拷贝整个已累积文本 (O(n²) -> O(n))
                if (!st.currentToken) {
                    resetTrailingRunningToolsLocked(st);
                    st.currentToken = std::make_shared<std::string>();
                    // 新流开始: 递增流身份 (COW 复制不递增, 见 currentTokenEpoch 注释)
                    ++st.currentTokenEpoch;
                } else if (st.currentToken.use_count() > 1) {
                    st.currentToken = std::make_shared<std::string>(*st.currentToken);
                }
                if (!delta.text.empty()) {
                    st.currentToken->append(delta.text);
                }
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
                m->collapsed          = true;
                m->startTimeMs        = delta.startTimeMs > 0
                                            ? delta.startTimeMs
                                            : static_cast<int64_t>(
                                           std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::system_clock::now().time_since_epoch()
                                           )
                                               .count()
                                       );
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
                        if (delta.startTimeMs > 0) {
                            m.startTimeMs = delta.startTimeMs;
                        }
                        if (delta.durationMs > 0) {
                            m.durationMs = delta.durationMs;
                        } else if (m.startTimeMs > 0) {
                            const int64_t nowMs = static_cast<int64_t>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()
                                )
                                    .count()
                            );
                            m.durationMs = std::max(int64_t{0}, nowMs - m.startTimeMs);
                        }
                        found = true;
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
                // 节点级计时不回填 Think 流: 节点耗时聚合了思考+正文等多个消息,
                // 直接覆盖会破坏单条消息的准确时长; Think 的耗时由 agent 端在
                // 思考流完成时经空文本 ThinkToken 结算包 (durationMs) 回填
                if (st.hasPendingToken()) {
                    if (st.currentTokenRole != TUIMessage::Role::Think) {
                        st.pendingTokenStartTimeMs = delta.startTimeMs;
                        st.pendingTokenDurationMs  = delta.durationMs;
                    }
                } else if (!st.messages.empty()
                           && st.messages.back()->role != TUIMessage::Role::Think) {
                    auto& m       = sharedState_.mutableMessage(st, st.messages.size() - 1);
                    m.startTimeMs = delta.startTimeMs;
                    m.durationMs  = delta.durationMs;
                }
            } break;
            case Type::MessageUITip: {
                // 通用提示消息: 插入提示消息 (按级别区分显示);
                // 默认折叠展示 (提示类消息, 与 makeText 的 System 默认折叠语义一致)
                pushCurrentTokenLocked(st);
                resetTrailingRunningToolsLocked(st);
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
            case Type::InsertMessage: {
                // 完整 ViewMessage 消息插入 (轮次统计、系统提示、中断头等):
                // 服务端已完成 appendViewMessage 与持久化, 客户端直接装载展示
                pushCurrentTokenLocked(st);
                resetTrailingRunningToolsLocked(st);
                if (delta.message) {
                    st.messages.push_back(std::make_shared<TUIMessage>(*delta.message));
                }
            } break;
            case Type::TurnStart: {
                pushCurrentTokenLocked(st);
                resetTrailingRunningToolsLocked(st);
                if (!delta.text.empty()) {
                    auto msg = std::make_shared<TUIMessage>(
                        TUIMessage::makeText(TUIMessage::Role::User, delta.text, delta.startTimeMs)
                    );
                    msg->id = delta.msgId;
                    st.messages.push_back(std::move(msg));
                    enqueueUiAction([this]() {
                        if (messageList_) {
                            messageList_->setStickToBottom(true);
                        }
                    });
                }
                st.isStreaming = true;
            } break;
            case Type::TurnEnd: {
                st.currentNodeName.clear();
                pushCurrentTokenLocked(st);
                resetTrailingRunningToolsLocked(st);
                st.isStreaming = false;
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
            st->contextMessages  = prev->contextMessages;
            st->isStreaming      = false;
            // 连接状态不随 Sync 重置: 握手后服务端回推全量 Sync 时若被重置回
            // Connecting (默认值), banner 会错误地回到"启动中"
            st->connState = prev->connState;
            // 启动进度不随 Sync 重置: 本地模式下握手后 init 仍在进行,
            // 期间服务端回推的早期 Sync 不应清空正在展示的启动步骤
            st->startupProgress = prev->startupProgress;

            // 消息队列同步 (服务端排队消息镜像)
            st->pendingInputs.clear();
            for (const auto& item : payload.messageQueue) {
                TUIPendingInput pi;
                pi.id          = item.id;
                pi.text        = item.text;
                pi.model       = item.model;
                pi.createdAtMs = item.createdAtMs;
                st->pendingInputs.push_back(std::move(pi));
            }

            // 历史消息与 server viewMessages 同型 (ViewMessage), 直接拷贝;
            // 原 json→TUIMessage 拆解逻辑已下沉到 server (event_stream 展开)
            st->messages.reserve(payload.messages.size());
            for (const auto& vm : payload.messages) {
                st->messages.push_back(std::make_shared<TUIMessage>(vm));
            }
            // 历史分页窗口元数据: fromIndex = 本批消息的起始绝对下标
            // (尾窗同步时 > 0, 上方还有更早历史待分页拉取; 全量同步时为 0);
            // 在途页请求随整体替换作废, 复位加载标志
            st->historyWindowStart = payload.fromIndex;
            st->historyTotal
                = payload.totalMessages != 0 ? payload.totalMessages : payload.messages.size();
            st->historyLoading = false;
            // 直接替换 (旧快照由 UI 线程持有, 自然释放)
            cur = std::move(*st);
        });
    }
    // 消息列表吸附到底部 + 清理中断 UI 状态 + 重置历史分页锚定 (消息整体
    // 替换, 旧状态随之失效): 组件由 UI 线程独占, 经动作队列投递
    enqueueUiAction([this]() {
        if (messageList_) {
            messageList_->setStickToBottom(true);
            messageList_->clearInterruptUiState();
            messageList_->resetHistoryPagination();
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
        pushCurrentTokenLocked(st);
        resetTrailingRunningToolsLocked(st);
        st.isStreaming = false;
    }
    postRedraw();
}

void TUIClientAgentIO::onContextStats(const agentxx::agent::WireContextStats& stats) {
    // 上下文统计写入 sharedState_ (而非 Session::contextStats —— TUI 不持有
    // Session, 属于 agent-io 线程); 状态栏等组件从 frameState 读取。
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
    std::string_view sessionId,
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
                resetTrailingRunningToolsLocked(st);
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
                resetTrailingRunningToolsLocked(st);
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
                .sessionId = std::string{sessionId},
                .path      = permTarget,
                .allow     = allow,
                .index     = index,
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
