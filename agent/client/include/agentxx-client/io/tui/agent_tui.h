#pragma once

#include "agentxx-client/io/tui/scrollable.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/context.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "fmt/format.h"
#include "ftxui/component/component.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include "neograph/api.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <format>
#include <functional>
#include <markdown/dom_builder.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// TUI 共享工具函数 (多个渲染 TU 共用, 定义为 inline 避免重复实现)
// ---------------------------------------------------------------------------

/// 格式化毫秒到可读字符串，自动选择最合适的单位
/// < 60s: "X.Xs" (如 "3.2s")
/// < 3600s (1h): "Xm Ys" (如 "2m 15s")
/// >= 3600s: "Xh Ym Zs" (如 "1h 2m 30s")
inline std::string formatDurationMilliseconds(int64_t milliseconds) {
    if (milliseconds < 0) {
        return "0.0s";
    }
    const int64_t totalSec = milliseconds / 1000;
    const int64_t hours    = totalSec / 3600;
    const int64_t minutes  = (totalSec % 3600) / 60;
    const int64_t seconds  = totalSec % 60;

    if (hours > 0) {
        return fmt::format("{}h{}m{}s", hours, minutes, seconds);
    }
    if (minutes > 0) {
        if (seconds > 0) {
            return fmt::format("{}m{}s", minutes, seconds);
        }
        return fmt::format("{}m0s", minutes);
    }
    // 不足一分钟：显示秒+毫秒
    const double sec = static_cast<double>(milliseconds) / 1000.0;
    return fmt::format("{:.1f}s", sec);
}

/// 格式化时间戳为本地时间 (HH:MM:SS)
inline std::string formatTimestampMilliseconds(int64_t timestamp_ms) {
    if (timestamp_ms <= 0) {
        return "00:00:00";
    }
    std::chrono::zoned_time time{
        std::chrono::current_zone(),
        std::chrono::sys_time{std::chrono::seconds(timestamp_ms / 1000)}
    };

    return std::format("{:%H:%M:%S}", time);
}

/// 取首行并按 utf8 长度截断 (用于消息/待发送项折叠为一行)
inline std::string oneLinePreview(std::string_view s, size_t max = 60) {
    const auto  nl = s.find('\n');
    std::string line{(nl == std::string_view::npos) ? s : s.substr(0, nl)};
    const auto  idx = agentxx::util::findIndexByUtf8Length(line, max);
    if (idx > 0 && idx < line.size()) {
        line.resize(idx);
        line += "...";
    }
    return line;
}

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

    /// 已弹出的行累计数 (单调递增); 用于检测缓存错位 (pop_front 后索引偏移)
    uint64_t poppedCount() const {
        return poppedCount_;
    }

protected:

    /// UI 线程串行调用 (经 pump), 无需加锁
    void onLog(const agentxx::util::LogEntry& entry) override;

private:

    std::deque<Line> lines_;
    size_t           maxLines_   = 2000;
    uint64_t         poppedCount_ = 0; ///< 因超限而被 pop_front 的行累计数
};

/// TUI
/// - [自动滚动] 靠近消息列表底部时，自动吸附到底部
/// - [自动折叠] Thinking/Tool
/// 的消息在末尾输出中时自动展开显示，输出完成后自动折叠
///
/// 线程模型与锁设计:
/// - client 线程 (asio executor): 运行 runTransportLoop, 调用 onDelta/onSync/onPeerMessage
///   写入共享状态 (经 mutex_ 短临界区 + COW)
/// - UI 线程 (uiThread_): FTXUI Loop 渲染 + 事件处理;
///   每帧开头短锁拷贝 state_ 快照到 frameState_, 之后无锁渲染;
///   事件处理短锁 COW 修改 state_
/// - 共享状态收敛于 RenderState (shared_ptr 持有), 消息体经 shared_ptr<Message> 结构共享:
///   追加/修改仅拷贝 vector 结构 (指针数组) 或被改的单条 Message, 未改消息零拷贝
class AgentTUI : public agentxx::agent::AgentIOBase,
                 public std::enable_shared_from_this<AgentTUI> {
public:

    using LineChannel
        = asio::experimental::concurrent_channel<void(neograph_asio_error_code, std::string)>;

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

    /// 排队等待发送的用户输入 (BaseAgent 执行中收到, 轮次结束后自动逐个发送)
    struct PendingInput {
        std::string text;
        bool        expanded = false; // 弹窗中是否展开为多行 (默认折叠为一行)
    };

private:

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
        int cachedWidth = -1; // 构建时的可用宽度 (变化时重建, 表格换行依赖宽度)
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

    // -----------------------------------------------------------------------
    // 跨线程共享状态 (RenderState)
    //
    // 所有被 client 线程 (onDelta/onSync/onPeerMessage) 和 UI 线程 (渲染/事件)
    // 并发访问的数据收敛于此结构, 经 shared_ptr + mutex_ 保护:
    // - 写方 (client/UI 事件): 短锁 + COW (mutableStateLocked) 后修改
    // - 读方 (UI 渲染): 短锁拷贝 shared_ptr 快照 (snapshotStateLocked), 之后无锁渲染
    //
    // 消息体经 shared_ptr<Message> 结构共享: 追加/修改仅拷贝 vector 结构 (指针数组)
    // 或被改的单条 Message, 未改消息零拷贝
    // -----------------------------------------------------------------------
    struct RenderState {
        std::vector<std::shared_ptr<Message>> messages;
        std::string                           currentToken;
        Message::Role                         currentTokenRole = Message::Role::Assistant;
        bool                                  isStreaming      = false;

        /// 暂存的当前流式 token 时间信息 (由 NodeEnd 设置, pushCurrentTokenLocked 时应用)
        int64_t pendingTokenDurationMs  = 0;
        int64_t pendingTokenStartTimeMs = 0;

        /// 当前正在执行的节点名称 (由 NodeStart/NodeEnd Delta 更新)
        std::string currentNodeName;

        /// 模型选择器: 可用模型列表 + 当前模型显示名
        std::vector<std::string> modelNames;
        std::string              cachedModelName;

        /// 用户输入队列: BaseAgent 执行中收到的用户输入排队, 轮次结束后自动逐个发送
        std::deque<PendingInput> pendingInputs;

        /// 从 server 收到的 llm messages 缓存 (上下文弹窗展示用)
        neograph::json contextMessages = neograph::json::array();
        /// 上下文弹窗是否打开 (client 线程收到 WireContextMessages 时置 true)
        bool showContextOverlay = false;

        /// 会话加载的组件明细 (MCP/Skill/Memory), 供信息侧边栏渲染
        std::vector<agentxx::agent::AppendComponentNotification> appendComponents;
    };

    /// 跨线程共享状态 (mutex_ 保护)
    std::mutex                            mutex_;
    std::shared_ptr<RenderState>          state_ = std::make_shared<RenderState>();
    /// UI 线程本帧快照 (每帧开头由 snapshotStateLocked 填充, 渲染期间无锁读取)
    std::shared_ptr<RenderState>          frameState_;

    /// COW: 获取 state_ 的可写引用; 若被 UI 线程快照共享 (use_count > 1) 则深拷贝结构
    /// - 调用方须持有 mutex_
    RenderState& mutableStateLocked();
    /// COW: 获取 messages[idx] 的可写引用; 若被快照共享则拷贝该条 Message
    /// - 调用方须持有 mutex_
    Message& mutableMessageLocked(RenderState& st, size_t idx);
    /// 快照: 拷贝 state_ 的 shared_ptr (纳秒级), 供 UI 线程本帧无锁渲染
    /// - 调用方须持有 mutex_
    std::shared_ptr<RenderState> snapshotStateLocked();

    /// 可滚动的消息列表组件 (ListView 风格 viewport 局部绘制)
    std::shared_ptr<Scrollable> messagesScrollable_;
    /// 侧边栏内容可滚动组件 (ListView 风格 viewport 局部绘制)
    std::shared_ptr<Scrollable> sidebarScrollable_;

    // -----------------------------------------------------------------------
    // UI 线程独占状态 (仅 uiThread_ 访问, 无需锁)
    // -----------------------------------------------------------------------

    /// 消息元素缓存 (按 messages 索引; 仅内容变化的消息重建 Element)
    /// 滑动窗口淘汰: 仅保留最近 kMaxMessageCache 条的完整缓存 (Element + mdBuilders),
    /// 更早的消息释放重量级对象 (markdown AST/Element 树), 滚动回时按需重建
    std::vector<MessageCache> messageCache_;
    /// 上一帧消息数量 (检测增长, 仅在增长时触发淘汰, 避免每帧重复清理)
    size_t prevMessageCount_ = 0;
    /// 流式 token 的 markdown DomBuilder (每帧重建, 须与帧内 element 同生命周期)
    std::vector<std::unique_ptr<markdown::DomBuilder>> streamingMdBuilders_;
    /// 消息列表子项元数据 (每帧由 buildMessageItems 填充)
    std::vector<MessageItemMeta> messageItemMeta_;
    /// 日志行元素缓存 (日志仅追加, 按行索引缓存避免每帧重建)
    std::vector<ftxui::Element> logLineCache_;
    /// 上次构建 logLineCache_ 时的 poppedCount (检测 pop_front 导致的缓存错位)
    uint64_t logCachePoppedCount_ = 0;

    std::string inputText_;

    /// 模型选择器弹窗状态 (UI-only; 模型数据在 RenderState 中)
    bool showModelSelector_  = false;
    int  selectedModelIndex_ = 0;

    /// 设置弹窗状态
    bool showSettings_         = false;
    int  selectedSettingIndex_ = 0;

    /// 上下文弹窗滚动偏移 (UI-only; 数据与开关在 RenderState 中)
    int contextScrollOffset_ = 0;

    /// 待发送消息队列弹窗开关 (UI-only; 队列数据在 RenderState 中)
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

    /// 各可折叠消息块 (Thinking/Tool) 的渲染区域与对应 messages 索引,
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
    /// 本 TUI 绑定的 executor (供 onPeerMessage 中 co_spawn 使用)
    asio::any_io_executor ex_;

    // screen_ 由 UI 线程创建/销毁, agent/日志线程经 postRedraw 读;
    // 经 screenMutex_ + shared_ptr 保护: 并发读取方持有引用计数,
    // 确保 UI 线程退出并销毁 App 期间 postRedraw 不会访问已销毁对象 (UAF)
    std::mutex                                screenMutex_;
    std::shared_ptr<ftxui::ScreenInteractive> screen_;
    std::thread                               uiThread_;
    std::atomic<bool>                         running_{false};

    /// handleInterrupt 正在等待用户输入时为 true;
    /// 此时 Enter 须把输入直接送入 inputChannel_ (而非 isStreaming 待发送队列), 否则死锁
    std::atomic<bool> awaitingInterruptInput_{false};

    std::shared_ptr<LineChannel> inputChannel_;
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
    /// 消息元素缓存滑动窗口大小: 仅保留最近 N 条的完整缓存 (Element + mdBuilders),
    /// 更早的消息释放重量级对象, 滚动回时按需重建 (避免长会话内存无限增长)
    static constexpr size_t kMaxMessageCache = 200;
    /// 程序版本号 (与 CMake project VERSION 保持一致)
    static constexpr const char* kAgentxxVersion = "0.1.0";

    void postRedraw();
    /// 构建消息列表子项 (ListView 风格): 返回各消息块 + 流式 token,
    /// 同时填充 messageItemMeta_ 与 messageCache_ (仅重建内容变化的消息)
    /// - 使用 frameState_ (本帧快照, 无锁)
    std::vector<ScrollItem> buildMessageItems();
    /// 构建单条消息的渲染块 (不含末尾空行); 仅在消息签名变化时调用并缓存
    /// maxWidth: 消息列表可用宽度 (终端列数), 用于限制表格宽度; <= 0 表示不限制
    /// mdBuilders 收集 markdown DomBuilder (其内部 Box 被 reflect() 引用, 须与 element 同生命周期)
    ftxui::Element buildMessageBlock(
        const Message&                                      msg,
        int                                                 maxWidth,
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
    /// - 使用 frameState_ (本帧快照, 无锁)
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
    void cancelCurrentRunLocked(RenderState& st);

    /// 发送一条用户输入到 BaseAgent (刷新 currentToken / 追加 User 消息 / 置 streaming / 入
    /// channel)
    /// - 调用方须持有 mutex_
    void sendUserInputLocked(RenderState& st, std::string text);
    /// 将当前流式 token 推入 messages (角色切换/轮次结束时调用);
    /// 同时应用暂存的时间信息 (pendingTokenDurationMs/pendingTokenStartTimeMs)
    /// - 调用方须持有 mutex_
    void pushCurrentTokenLocked(RenderState& st);
    /// 若空闲且输入队列非空, 取队首发送 (轮次结束自动派发); 调用方须持有 mutex_
    void dispatchNextPendingInput(RenderState& st);

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
