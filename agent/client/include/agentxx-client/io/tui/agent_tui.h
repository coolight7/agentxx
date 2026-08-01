#pragma once

#include "agentxx-client/io/tui/scrollable.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/agent/agent_io.h"
#include <markdown/dom_builder.hpp>
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
/// - 宿主为 UI 线程: 由 UI Loop 每帧调 pump() 消费, onLog/snapshot/clear 均在 UI 线程, 无需加锁
class TUILogSink : public agentxx::util::LogSink {
public:

    struct Line {
        agentxx::util::LogLevel level;
        std::string             text;
    };

    /// 拷贝当前缓存的日志行 (UI 线程调用)
    std::vector<Line> snapshot() const;

    void clear();

protected:

    /// UI 线程串行调用 (经 pump), 无需加锁
    void onLog(const agentxx::util::LogEntry& entry) override;

private:

    std::deque<Line> lines_;
    size_t           maxLines_ = 2000;
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

        bool collapsed = false; // 折叠/展开 (Thinking/Tool 默认折叠)

        // 运行时长统计 (毫秒)
        int64_t durationMs = 0;
        // 开始时间戳 (毫秒)
        int64_t startTimeMs = 0;
    };

    struct PermissionRequest {
        std::string toolName;
        std::string category;
        std::string target;
    };

    /// 排队等待发送的用户输入 (BaseAgent 执行中收到, 轮次结束后自动逐个发送)
    struct PendingInput {
        std::string text;
        bool        expanded = false; // 弹窗中是否展开为多行 (默认折叠为一行)
    };

    /// 右侧边栏 tab (类似浏览器 tab)
    struct SidebarTab {
        std::string id;
        std::string title;
        /// 可滚动主体内容 (经 sidebarScrollable_ 懒加载 viewport 渲染)
        std::function<std::vector<ScrollItem>()> render;
        /// 底部常驻内容 (渲染于滚动区之外, 不随内容滚动; 为空则无)
        std::function<ftxui::Element()> footer;
    };

    /// 消息元素缓存 (仿 Flutter Widget 重建: 仅内容/状态变化的消息才重建 Element,
    /// 避免每帧对全部消息重复解析 markdown)
    struct MessageCache {
        ftxui::Element element; // 缓存的渲染元素
        int64_t        sig = 0; // 内容签名 (64 位哈希, 变化时重建)
        /// markdown DomBuilder 持有 reflect() 引用的 Box (LinkTarget::boxes),
        /// 必须与 element 同生命周期, 否则 reflect 写入悬空引用 → UAF
        std::vector<std::unique_ptr<markdown::DomBuilder>> mdBuilders;
    };

    /// 消息列表子项元数据 (与 buildMessageItems 返回的 items 一一对应),
    /// 用于由 viewport 可见区域反推可折叠消息的鼠标命中区域
    struct MessageItemMeta {
        Message::Role role;
        bool          collapsible;  // 是否可点击折叠/展开 (Thinking/Tool)
        int           messageIndex; // 对应 messages_ 索引 (流式 token 为 -1)
    };

    std::mutex           mutex_;
    std::vector<Message> messages_;
    std::string          currentToken_;
    Message::Role        currentTokenRole_ = Message::Role::Assistant;
    bool                 isStreaming_      = false;

    /// 暂存的当前流式 token 时间信息 (由 NodeEnd 设置, pushCurrentTokenLocked 时应用)
    int64_t pendingTokenDurationMs_  = 0;
    int64_t pendingTokenStartTimeMs_ = 0;

    /// 可滚动的消息列表组件 (ListView 风格 viewport 局部绘制)
    std::shared_ptr<Scrollable> messagesScrollable_;
    /// 侧边栏内容可滚动组件 (ListView 风格 viewport 局部绘制)
    std::shared_ptr<Scrollable> sidebarScrollable_;

    /// 消息元素缓存 (按 messages_ 索引; 仅内容变化的消息重建 Element)
    std::vector<MessageCache> messageCache_;
    /// 流式 token 的 markdown DomBuilder (每帧重建, 须与帧内 element 同生命周期)
    std::vector<std::unique_ptr<markdown::DomBuilder>> streamingMdBuilders_;
    /// 消息列表子项元数据 (每帧由 buildMessageItems 填充)
    std::vector<MessageItemMeta> messageItemMeta_;
    /// 日志行元素缓存 (日志仅追加, 按行索引缓存避免每帧重建)
    std::vector<ftxui::Element> logLineCache_;

    std::string                      inputText_;
    std::optional<PermissionRequest> pendingPermission_;

    /// 模型选择器状态
    bool                     showModelSelector_  = false;
    int                      selectedModelIndex_ = 0;
    std::vector<std::string> modelNames_;

    /// 设置弹窗状态
    bool showSettings_         = false;
    int  selectedSettingIndex_ = 0;

    /// 上下文弹窗状态 (查看当前会话 llm messages)
    bool showContextOverlay_  = false;
    int  contextScrollOffset_ = 0;
    /// 从 server 收到的 llm messages 缓存 (弹窗展示用)
    neograph::json contextMessages_ = neograph::json::array();

    /// 当前正在执行的节点名称 (由 NodeStart/NodeEnd Delta 更新)
    std::string currentNodeName_;

    /// 用户输入队列: BaseAgent 执行中收到的用户输入排队, 轮次结束后自动逐个发送
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

    /// 侧边栏宽度 (拖拽左边框可调; 渲染时对整体应用 size(WIDTH, EQUAL, sidebarWidth_))
    int sidebarWidth_ = kSidebarDefaultWidth;
    /// 是否正在拖拽调整侧边栏宽度
    bool sidebarResizing_ = false;
    /// 拖拽起始鼠标 x 与起始宽度 (delta 计算, 避免依赖绝对屏幕几何)
    int sidebarResizeStartX_     = 0;
    int sidebarResizeStartWidth_ = 0;
    /// 侧边栏左侧拖拽手柄的渲染区域 (渲染时经 reflect 填充)
    ftxui::Box sidebarHandleBox_;

    /// 各可折叠消息块 (Thinking/Tool) 的渲染区域与对应 messages_ 索引,
    /// 用于鼠标点击展开/折叠 (渲染时经 reflect 填充)
    std::vector<ftxui::Box> collapsibleBoxes_;
    std::vector<size_t>     collapsibleMsgIndices_;
    /// 消息列表整体渲染区域, 用于限制折叠点击的 X 范围 (避免误触侧边栏)
    ftxui::Box messagesAreaBox_;
    /// 日志侧边栏底部 "上下文" 按钮的渲染区域 (鼠标点击检测)
    ftxui::Box contextButtonBox_;

    std::shared_ptr<agentxx::agent::AgentContext> agentContext_;
    TUITheme                                      theme_;
    /// 本 TUI 绑定的会话 thread_id (按 thread_id 取会话状态)
    std::string threadId_;
    /// 缓存的模型显示名称 (避免 render 热路径频繁加锁)
    std::string cachedModelName_;
    /// 本 TUI 绑定的 executor (供 onPeerMessage 中 co_spawn 使用)
    asio::any_io_executor ex_;

    // screen_ 由 UI 线程创建/销毁, agent/日志线程经 postRedraw 读;
    // 经 screenMutex_ + shared_ptr 保护: 并发读取方持有引用计数,
    // 确保 UI 线程退出并销毁 App 期间 postRedraw 不会访问已销毁对象 (UAF)
    std::mutex                                    screenMutex_;
    std::shared_ptr<ftxui::ScreenInteractive> screen_;
    std::thread                                  uiThread_;
    std::atomic<bool>                            running_{false};

    /// handleInterrupt 正在等待用户输入时为 true;
    /// 此时 Enter 须把输入直接送入 inputChannel_ (而非 isStreaming_ 待发送队列), 否则死锁
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
    /// 侧边栏宽度拖拽范围与默认值
    static constexpr int kSidebarMinWidth     = 24;
    static constexpr int kSidebarMaxWidth     = 120;
    static constexpr int kSidebarDefaultWidth = 40;
    /// 程序版本号 (与 CMake project VERSION 保持一致)
    static constexpr const char* kAgentxxVersion = "0.1.0";

    void postRedraw();
    /// 构建消息列表子项 (ListView 风格): 返回各消息块 + 流式 token,
    /// 同时填充 messageItemMeta_ 与 messageCache_ (仅重建内容变化的消息)
    std::vector<ScrollItem> buildMessageItems();
    /// 构建单条消息的渲染块 (不含末尾空行); 仅在消息签名变化时调用并缓存
    /// mdBuilders 收集 markdown DomBuilder (其内部 Box 被 reflect() 引用, 须与 element 同生命周期)
    ftxui::Element buildMessageBlock(
        const Message&                                   msg,
        std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
    );
    /// 计算消息内容签名 (64 位哈希; 签名不变则复用缓存元素)
    static int64_t messageSignature(const Message& msg);
    /// 特化渲染 filesystem_edit_text_file (git diff 对比), 实现见 tui_render_edittool.cpp
    /// - 折叠态: 在 header 追加文件路径预览
    void appendEditToolHeader(const Message& msg, ftxui::Elements& header);
    /// - 展开态: 渲染文件路径 + diff 对比 + 错误
    void appendEditToolBody(const Message& msg, ftxui::Elements& lines);
    /// - diff 对比块 (屏幕足够宽时左右对比, 不足时单块内对比)
    ftxui::Element renderEditToolDiff(std::string_view oldStr, std::string_view newStr);
    ftxui::Element renderPermissionOverlay();
    ftxui::Element renderModelSelectorOverlay();
    ftxui::Element renderSettingsOverlay();
    /// 待发送消息队列弹窗 (顶部清空按钮, 每条消息折叠为一行 + 删除按钮)
    ftxui::Element renderPendingInputsOverlay();
    /// 上下文弹窗: 显示当前会话的 llm messages 列表
    ftxui::Element renderContextOverlay();
    /// 输入框下方的状态栏: 左侧模型名, 右侧上下文占用
    ftxui::Element renderStatusBar();
    /// 右侧边栏: 顶部 tab 栏 + 当前 tab 内容; 无 tab 时不应调用
    ftxui::Element renderSidebar();
    /// 日志窗口内容 (ListView 子项; 按行缓存)
    std::vector<ScrollItem> renderLogWindow();
    /// 信息侧边栏可滚动主体: planning 特化渲染 + MCP/Skill/Memory 统计 (组成单一滚动列表)
    std::vector<ScrollItem> renderInfoSidebar();
    /// 信息侧边栏底部常驻内容: 工作目录 + 版本/运行模式 (不随主体滚动)
    ftxui::Element renderInfoSidebarFooter();
    /// 日志侧边栏底部常驻内容: 当前执行节点名 + "上下文" 按钮
    ftxui::Element renderLogSidebarFooter();
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

    /// 发送一条用户输入到 BaseAgent (刷新 currentToken / 追加 User 消息 / 置 streaming / 入
    /// channel)
    /// - 调用方须持有 mutex_
    void sendUserInputLocked(std::string text);
    /// 将当前流式 token 推入 messages_ (角色切换/轮次结束时调用);
    /// 同时应用暂存的时间信息 (pendingTokenDurationMs_/pendingTokenStartTimeMs_)
    /// - 调用方须持有 mutex_
    void pushCurrentTokenLocked();
    /// 若空闲且输入队列非空, 取队首发送 (轮次结束自动派发); 调用方须持有 mutex_
    void dispatchNextPendingInput();

    /// 侧边栏 tab 管理
    /// - render: 可滚动主体内容; footer: 底部常驻内容 (可选, 渲染于滚动区之外)
    void addSidebarTab(
        std::string_view                         id,
        std::string_view                         title,
        std::function<std::vector<ScrollItem>()> render,
        std::function<ftxui::Element()>          footer = nullptr
    );
    void removeSidebarTab(std::string_view id);
    bool hasSidebarTab(std::string_view id) const;
    /// F12: 插入/关闭日志窗口 tab
    void toggleLogWindow();
    /// 处理侧边栏 tab 的鼠标点击; 返回是否消费了该事件
    bool handleSidebarMouse(const ftxui::Mouse& mouse);
    /// 处理侧边栏左边框拖拽调整宽度; 返回是否消费了该事件
    /// - 拖拽中会消费所有鼠标事件, 应在其他鼠标处理之前调用
    bool handleSidebarResizeMouse(const ftxui::Mouse& mouse);
    /// 处理可折叠消息块 (Thinking/Tool) 的鼠标点击 (展开/折叠);
    /// 返回是否消费了该事件
    bool handleCollapsibleMouse(const ftxui::Mouse& mouse);
    /// 处理待发送消息队列弹窗的鼠标点击 (清空/删除/展开折叠); 返回是否消费了该事件
    bool handlePendingInputsMouse(const ftxui::Mouse& mouse);

    /// 缓存的本会话指针 (构造时初始化，避免反复查找 SessionStore)
    std::shared_ptr<agentxx::agent::Session> session_;

    // 会话加载的组件明细 (MCP/Skill/Memory), 供信息侧边栏渲染
    std::vector<agentxx::agent::AppendComponentNotification> appendComponents_;

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
