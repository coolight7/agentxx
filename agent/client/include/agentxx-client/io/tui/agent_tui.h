#pragma once

#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/context.h"
#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include "neograph/api.h"
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/// TUI 日志接收器
/// - 注册到 agentxx::util::LogDispatcher 以接收 XX_LOG 系列日志
/// - 线程安全缓存日志行, 供右侧边栏日志窗口渲染
class TUILogSink : public agentxx::util::LogSink {
public:

    struct Line {
        agentxx::util::LogLevel level;
        std::string             text;
    };

    void onLog(agentxx::util::LogLevel level, std::string_view message) override;

    /// 拷贝当前缓存的日志行 (供渲染线程读取)
    std::vector<Line> snapshot() const;

    void setOnNewLog(std::function<void()> cb);

    void clear();

private:

    mutable std::mutex    mutex_;
    std::deque<Line>      lines_;
    size_t                maxLines_ = 2000;
    std::function<void()> onNewLog_;
};

/// TUI
/// - [自动滚动] 靠近消息列表底部时，自动吸附到底部
/// - [自动折叠] Thinking/Tool
/// 的消息在末尾输出中时自动展开显示，输出完成后自动折叠
class AgentTUI : public agentxx::agent::AgentIOBase,
                 public std::enable_shared_from_this<AgentTUI> {
public:

    using LineChannel
        = asio::experimental::concurrent_channel<void(neograph_asio_error_code, std::string)>;
    using BoolChannel
        = asio::experimental::concurrent_channel<void(neograph_asio_error_code, bool)>;

private:

    struct Message {
        enum class Role {
            User,
            Assistant,
            Thinking,
            System,
            Tool
        };
        Role        role;
        std::string text; // 文本内容; Tool 角色时为 arguments
        // 以下仅 Tool 角色使用
        std::string toolName;
        std::string toolCallId;
        std::string toolResult;
        bool        toolFinished = false;
        bool        toolHasError = false;
        bool        collapsed    = false; // 折叠/展开 (Thinking/Tool 默认折叠)
    };

    struct PermissionRequest {
        std::string toolName;
        std::string category;
        std::string target;
    };

    /// 排队等待发送的用户输入 (DeepAgent 执行中收到, 轮次结束后自动逐个发送)
    struct PendingInput {
        std::string text;
        bool        expanded = false; // 弹窗中是否展开为多行 (默认折叠为一行)
    };

    /// 右侧边栏 tab (类似浏览器 tab)
    struct SidebarTab {
        std::string                     id;
        std::string                     title;
        std::function<ftxui::Element()> render;
    };

    std::mutex           mutex_;
    std::vector<Message> messages_;
    std::string          currentToken_;
    Message::Role        currentTokenRole_ = Message::Role::Assistant;
    bool                 isStreaming_      = false;

    /// 消息列表滚动: 是否吸附在底部 (吸附时新增消息自动滚动到底部)
    bool stickToBottom_ = true;
    /// 未吸附底部时的滚动锚点: 聚焦的消息块绝对索引 (消息仅追加, 索引稳定)
    int scrollAnchorIndex_ = 0;
    /// 滚轮累加器: 用于降低滚动速度, 每次滚轮事件累加 0.5f, 达到 ±1.0f 时移动一个块
    float                  scrollAccum_ = 0.0f;
    static constexpr float kScrollStep  = 0.5f;

    std::string                      inputText_;
    std::optional<PermissionRequest> pendingPermission_;

    /// 模型选择器状态
    bool                     showModelSelector_  = false;
    int                      selectedModelIndex_ = 0;
    std::vector<std::string> modelNames_;

    /// 设置弹窗状态
    bool showSettings_         = false;
    int  selectedSettingIndex_ = 0;

    /// 用户输入队列: DeepAgent 执行中收到的用户输入排队, 轮次结束后自动逐个发送
    std::deque<PendingInput> pendingInputs_;
    /// 待发送消息队列弹窗开关
    bool showPendingInputs_ = false;
    /// 弹窗内各消息行 / 删除按钮 / 清空按钮 及输入框上方计数行的渲染区域 (鼠标点击检测)
    std::vector<ftxui::Box> pendingInputBoxes_;
    std::vector<ftxui::Box> pendingInputDelBoxes_;
    ftxui::Box              pendingInputClearBox_;
    ftxui::Box              pendingInputCounterBox_;

    /// 右侧边栏状态
    std::vector<SidebarTab> sidebarTabs_;
    int                     activeTabIndex_ = 0;
    /// 各 tab 标题的渲染区域, 用于鼠标点击检测 (渲染时经 reflect 填充)
    std::vector<ftxui::Box> tabBoxes_;

    /// 各可折叠消息块 (Thinking/Tool) 的渲染区域与对应 messages_ 索引,
    /// 用于鼠标点击展开/折叠 (渲染时经 reflect 填充)
    std::vector<ftxui::Box> collapsibleBoxes_;
    std::vector<size_t>     collapsibleMsgIndices_;

    std::shared_ptr<agentxx::agent::AgentContext> agentContext_;
    TUITheme                                      theme_;
    /// 本 TUI 绑定的会话 thread_id (按 thread_id 取会话状态)
    std::string threadId_;
    /// 缓存的模型显示名称 (避免 render 热路径频繁加锁)
    std::string cachedModelName_;
    /// 本 TUI 绑定的 executor (供 onPeerMessage 中 co_spawn 使用)
    asio::any_io_executor ex_;

    ftxui::ScreenInteractive* screen() const {
        return screen_.load(std::memory_order_acquire);
    }

    // screen_ 由 UI 线程写、agent/日志线程经 postRedraw 读, 必须原子化避免数据竞争与 UAF
    std::atomic<ftxui::ScreenInteractive*> screen_{nullptr};
    std::thread                            uiThread_;
    std::atomic<bool>                      running_{false};

    /// handleInterrupt 正在等待用户输入时为 true;
    /// 此时 Alt+Enter 须把输入直接送入 inputChannel_ (而非 isStreaming_ 待发送队列), 否则死锁
    std::atomic<bool> awaitingInterruptInput_{false};

    std::shared_ptr<LineChannel> inputChannel_;
    std::shared_ptr<BoolChannel> permissionChannel_;
    std::shared_ptr<TUILogSink>  logSink_;

    /// 远程 Agentxx 地址 (空表示内置 Agentxx; 非空为远程 http[s]://ip:port)
    std::string remoteUrl_;

    /// 日志窗口 tab 的固定 id
    static constexpr const char* kLogTabId = "xx_logs";
    /// 信息侧边栏 tab 的固定 id
    static constexpr const char* kInfoTabId = "xx_info";
    /// 屏幕宽度达到此值时默认显示信息侧边栏
    static constexpr int kInfoSidebarMinWidth = 120;
    /// 程序版本号 (与 CMake project VERSION 保持一致)
    static constexpr const char* kAgentxxVersion = "0.1.0";

    /// 日志窗口滚动状态 (同消息区的 stickToBottom_ 模式)
    bool logStickToBottom_ = true;
    int  logFocusIndex_    = -1;

    void           postRedraw();
    ftxui::Element renderMessages();
    /// 特化渲染 filesystem_edit_text_file (git diff 对比), 实现见 tui_render_edittool.cpp
    /// - 折叠态: 在 header 追加文件路径预览
    void appendEditToolHeader(const Message& msg, ftxui::Elements& header);
    /// - 展开态: 渲染文件路径 + diff 对比 + 错误
    void appendEditToolBody(const Message& msg, ftxui::Elements& lines);
    /// - diff 对比块 (屏幕足够宽时左右对比, 不足时单块内对比)
    ftxui::Element renderEditToolDiff(std::string_view oldStr, std::string_view newStr);
    /// 当前可聚焦的消息块数量 (需在持有 mutex_ 时调用)
    int            focusBlockCount() const;
    ftxui::Element renderPermissionOverlay();
    ftxui::Element renderModelSelectorOverlay();
    ftxui::Element renderSettingsOverlay();
    /// 待发送消息队列弹窗 (顶部清空按钮, 每条消息折叠为一行 + 删除按钮)
    ftxui::Element renderPendingInputsOverlay();
    /// 输入框下方的状态栏: 左侧模型名, 右侧上下文占用
    ftxui::Element renderStatusBar();
    /// 右侧边栏: 顶部 tab 栏 + 当前 tab 内容; 无 tab 时不应调用
    ftxui::Element renderSidebar();
    /// 日志窗口内容
    ftxui::Element renderLogWindow();
    /// 信息侧边栏内容: 顶部 planning 特化渲染 + 底部工作目录/版本/运行模式
    ftxui::Element renderInfoSidebar();
    /// planning 特化渲染 (取最新 planning_write toolcall 的 todos/notes); 无规划时返回空
    std::optional<ftxui::Element> renderPlanningInfo();

    /// 获取本 TUI 绑定的会话
    std::shared_ptr<agentxx::agent::Session> currentSession();

    /// 打开模型选择器 (刷新可用模型列表)
    void openModelSelector();
    /// 确认选择当前高亮的模型
    void confirmModelSelection();
    /// 应用设置中的主题选择
    void applyThemeSelection();
    /// 取消当前正在执行的轮次; 调用方须持有 mutex_
    void cancelCurrentRunLocked();

    /// 发送一条用户输入到 DeepAgent (刷新 currentToken / 追加 User 消息 / 置 streaming / 入
    /// channel)
    /// - 调用方须持有 mutex_
    void sendUserInputLocked(std::string text);
    /// 若空闲且输入队列非空, 取队首发送 (轮次结束自动派发); 调用方须持有 mutex_
    void dispatchNextPendingInput();

    /// 侧边栏 tab 管理
    void addSidebarTab(
        std::string_view                id,
        std::string_view                title,
        std::function<ftxui::Element()> render
    );
    void removeSidebarTab(std::string_view id);
    bool hasSidebarTab(std::string_view id) const;
    /// F12: 插入/关闭日志窗口 tab
    void toggleLogWindow();
    /// 处理侧边栏 tab 的鼠标点击; 返回是否消费了该事件
    bool handleSidebarMouse(const ftxui::Mouse& mouse);
    /// 处理可折叠消息块 (Thinking/Tool) 的鼠标点击 (展开/折叠);
    /// 返回是否消费了该事件
    bool handleCollapsibleMouse(const ftxui::Mouse& mouse);
    /// 处理待发送消息队列弹窗的鼠标点击 (清空/删除/展开折叠); 返回是否消费了该事件
    bool handlePendingInputsMouse(const ftxui::Mouse& mouse);

    /// 缓存的本会话指针 (构造时初始化, 避免反复查找 SessionStore)
    std::shared_ptr<agentxx::agent::Session> session_;

public:

    explicit AgentTUI(
        asio::any_io_executor                         ex,
        std::shared_ptr<agentxx::agent::AgentContext> agentContext,
        std::string                                   threadId = "session",
        TUITheme                                      theme    = TUITheme::darkTheme()
    );
    ~AgentTUI() override;

    void start();
    void stop();

    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /// 设置远程 Agentxx 地址 (供信息侧边栏显示运行模式; 不调用则为内置 Agentxx)
    void setRemoteUrl(std::string url) {
        remoteUrl_ = std::move(url);
    }

    void onDelta(const agentxx::agent::Delta& delta) override;
    void onSync(const agentxx::agent::SyncPayload& payload) override;
    asio::awaitable<std::optional<std::string>> getInput() override;
    asio::awaitable<neograph::json>             handleInterrupt(
                    std::string_view threadId,
                    std::string_view interruptNode,
                    std::string_view interruptValue,
                    std::string_view interruptArgJson
                ) override;
    void requestCancel(std::string_view threadId) override;
    void requestSelectModel(std::string_view threadId, std::string_view model) override;

protected:

    /// 处理从 transport 收到的对端消息 (Delta/Sync/InterruptRequest/TurnResult/ContextStats)
    void onPeerMessage(agentxx::agent::WireMessage msg) override;
};
