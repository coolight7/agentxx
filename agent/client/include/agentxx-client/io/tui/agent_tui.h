#pragma once

#include "agentxx-client/config_loader.h"
#include "agentxx-client/io/tui/framework/modal_container.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/framework/ui_action_list.h"
#include "agentxx-client/io/tui/framework/ui_hit.h"
#include "agentxx-client/io/tui/scrollable.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx-client/io/tui/ui_components.h"
#include "agentxx-client/update_check.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/plugin/client_plugin_manager.h"
#include "agentxx/version.h"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "fmt/format.h"
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "markdown/text_utils.hpp"
#include "neograph/api.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace agentxx::client {

// ---------------------------------------------------------------------------
// TUI 共享工具函数
// (时长/时间戳格式化已迁移到
// [string_util.h](/agent/third_party/cxx_utilxx_base/include/utilxx_base/string_util.h), 供 agent
// 端构造
//  系统提示文本复用, 此处仅保留 UI 专用函数)
// ---------------------------------------------------------------------------

/// 单行预览: 取首个换行前的内容, 按终端显示列宽截断 (宽字符 CJK/emoji 按
/// 2 列计), 截断时以 "..." 收尾。max 为最大显示列数且含省略号占用;
/// 折叠消息头部应按实际剩余列宽传入以实现自适应 (而非固定字符数)。
inline std::string oneLinePreview(std::string_view s, size_t max = 60) {
    // 跳过前导空白/空行, 保证第一行有效内容能作为预览展示 (如前导换行的系统提示词)
    size_t start = 0;
    while (start < s.size()
           && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        ++start;
    }
    s.remove_prefix(start);
    const auto  nl = s.find('\n');
    std::string line{(nl == std::string_view::npos) ? s : s.substr(0, nl)};
    if (max == 0 || line.empty()) {
        return {};
    }
    // 内容预算: 预留省略号 3 列, 保证截断后总宽度不超过 max
    const auto idx = utilxx_base::findIndexByUtf8Length(line, max);
    if (idx > 0 && idx < line.size()) {
        line.resize(idx);
        line += "...";
    }
    return line;
}

inline std::string tailLinePreview(std::string_view s, size_t max = 60) {
    if (s.empty() || max == 0) {
        return "";
    }
    std::string line;
    line.reserve(std::min(s.size(), max * 4 + 4));
    bool prevSpace = false;
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!prevSpace && !line.empty()) {
                line.push_back(' ');
                prevSpace = true;
            }
        } else {
            line.push_back(c);
            prevSpace = false;
        }
    }
    while (!line.empty() && line.back() == ' ') {
        line.pop_back();
    }
    if (line.empty()) {
        return "";
    }
    // 按显示列宽自适应: 保留末尾不超过 budget 列的内容 (宽字符按 2 列计),
    // 前缀 "..." 占 3 列计入 max。从尾部反向逐码点累积列宽, 放不下即停,
    // 宽字符跨预算边界时整体舍弃, 不切断码点
    const size_t budget = (max > 3) ? max - 3 : 0;
    if (budget == 0) {
        return "";
    }
    size_t totalCol = 0;
    for (size_t i = 0; i < line.size();) {
        size_t len  = markdown::utf8_byte_length(line[i]);
        len         = std::min(len, line.size() - i);
        totalCol   += static_cast<size_t>(
            markdown::codepoint_width(markdown::utf8_codepoint(line.data() + i, len))
        );
        i += len;
    }
    if (totalCol <= budget) {
        return line;
    }
    // 反向找出保留区间的起始字节: 从尾部回溯逐码点累计列宽, 超出预算停止;
    // 至少强制保留最后一个码点 (极小预算下宁可溢出 1 列也不返回空 "...",
    // 溢出由渲染层 xflex_shrink 右缘裁剪兜底)
    size_t startByte = line.size();
    size_t col       = 0;
    size_t i         = line.size();
    bool   keptAny   = false;
    while (i > 0) {
        // 回溯找 i 之前最后一个码点的起点 (跳过 UTF-8 续字节)
        size_t len = 1;
        while (len < i && (static_cast<unsigned char>(line[i - len]) & 0xC0) == 0x80) {
            ++len;
        }
        const int w
            = markdown::codepoint_width(markdown::utf8_codepoint(line.data() + i - len, len));
        // 零宽字符随相邻内容保留; 已有内容且超出预算即停止
        if (keptAny && w > 0 && col + static_cast<size_t>(w) > budget) {
            break;
        }
        keptAny    = true;
        col       += static_cast<size_t>(w);
        i         -= len;
        startByte  = i;
    }
    return "..." + line.substr(startByte);
}

/// TUI 日志接收器
class TUILogSink : public utilxx_base::LogSink {
public:

    struct Line {
        utilxx_base::LogLevel level;
        std::string           text;
    };

    std::vector<Line> snapshot() const;
    void              clear();

    uint64_t poppedCount() const {
        return poppedCount_;
    }

    size_t lineCount() const {
        return lines_.size();
    }

protected:

    void onLog(const utilxx_base::LogEntry& entry) override;

private:

    std::deque<Line> lines_;
    size_t           maxLines_    = 2000;
    uint64_t         poppedCount_ = 0;
};

// 前向声明组件
class MessageListComponent;
class SidebarComponent;
class StatusBarComponent;
class InputComponent;

/// TUI 主类
///
/// 职责:
/// - 线程管理 (UI 线程 + client 线程)
/// - 协议处理 (onDelta/onSync/onPeerMessage → 更新 TUISharedState)
/// - 构建组件树 + 启动 FTXUI Loop
///
/// 渲染/事件逻辑已分解到各组件:
/// - MessageListComponent: 消息列表渲染 + 折叠交互
/// - InputComponent: 输入栏 + 发送逻辑
/// - StatusBarComponent: 状态栏
/// - SidebarComponent: 侧边栏 tab + 拖拽
/// - Overlay 组件: 模型选择/设置/待发送/上下文弹窗
///
/// 线程模型:
/// - client 线程: onDelta/onSync/onPeerMessage → sharedState_.mutate()
/// - UI 线程: FTXUI Loop 渲染 + 事件; 每帧 readSnapshot() 后无锁渲染
class TUIClientAgentIO : public agentxx::agent::AgentIOBase,
                         public std::enable_shared_from_this<TUIClientAgentIO> {
public:

    using LineChannel
        = asio::experimental::concurrent_channel<void(utilxx_base::AsioErrorCode, std::string)>;

    static constexpr std::string_view kAgentxxVersion   = agentxx::kVersion;
    static constexpr std::string_view kAgentxxBuildDate = agentxx::kBuildDate;

    /// 复制鼠标选中的文本到系统剪贴板 (鼠标左键拖选后松开时调用, UI 线程):
    /// - 从 FTXUI Screen 的当前 selection 提取文本 (GetSelection, 取上一绘制帧
    ///   累积的选中文本, 与屏幕显示一致, 已含本次拖动终点)
    /// - 写入系统剪贴板: Windows 用 Win32 API, 其他平台用 OSC 52 转义序列
    ///   (依赖终端模拟器支持, 如 Windows Terminal/wezterm/kitty/xterm)
    /// - 返回 true 表示已复制; 无选中文本或剪贴板不可用时返回 false
    /// - 复制成功/失败时均以 toast 提示 (无选中文本不提示)
    bool copySelectionToClipboard();

    explicit TUIClientAgentIO(
        asio::any_io_executor ex,
        std::string           sessionId = "session",
        TUITheme              theme     = TUITheme::darkTheme()
    );
    ~TUIClientAgentIO() override;

    void start();
    void stop();

    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /// 渲染帧统计 (供性能基准与现场诊断读取; 无额外线程/锁开销)
    /// - 仅在启用全局标记 [agentxx::agent::AgentConfigStatic::enableBenchmark] 时采集,
    ///   未启用时计数恒为 0 (正常使用不读时钟、不累加计数)
    struct FrameStats {
        uint64_t frames      = 0;   ///< 已渲染帧数
        double totalRenderMs = 0.0; ///< 渲染耗时累计 (仅组件树构建, 不含终端输出)
        double maxRenderMs   = 0.0; ///< 单帧最大渲染耗时
    };

    /// 读取帧统计 (任意线程可调用)
    FrameStats frameStats() const;
    /// 清零帧统计 (基准测试分段统计用; 任意线程可调用)
    void resetFrameStats();

    void setRemoteUrl(std::string url) {
        remoteUrl_ = std::move(url);
    }

    void setDataDir(std::string dir) {
        dataDir_ = std::move(dir);
    }

    void setWorkDir(std::string dir) {
        workDir_ = std::move(dir);
    }

    /// 装配 client 插件管理器 (mode_runners 在 start() 后调用):
    /// - 供命令管线 (onSend 拦截 "/" 命令) 与组件渲染 (状态栏/侧边栏) 读取
    /// - 同时也是事件接收器 (ClientEventSink), 经 setEventSink 注入
    void setPluginManager(std::shared_ptr<agentxx::plugin::ClientPluginManager> mgr) {
        pluginManager_ = std::move(mgr);
        if (pluginManager_) {
            pluginManager_->setSessionId(currentSessionId());
            // 动画等级门控: Disabled 时插件定时器不注册 (插件降级为静态展示);
            // 之后由设置弹窗的动画等级切换回调保持同步
            pluginManager_->setAnimationEnabled(
                TUISettings::instance().isAnimationEnabled(AnimationLevel::Low)
            );
        }
    }

    std::shared_ptr<agentxx::plugin::ClientPluginManager> pluginManager() const {
        return pluginManager_;
    }

    // -----------------------------------------------------------------------
    // 插件适配器接口 (由 TuiPluginAdapter 在 client io 线程调用; 内部自行
    // 投递到 UI 线程 / 加锁, 线程安全)
    // -----------------------------------------------------------------------

    /// 投递 UI 线程独占操作 (等价 enqueueUiAction; 任意线程可调用)
    void postToUi(std::function<void()> fn) {
        enqueueUiAction(std::move(fn));
    }

    /// 请求 UI 重绘 (任意线程可调用; 合并同帧多次请求)
    void requestRedraw() {
        postRedraw();
    }

    /// 侧边栏组件访问器 (UI 线程使用; 未启动时返回 nullptr)
    std::shared_ptr<SidebarComponent> sidebar() const {
        return sidebar_;
    }

    /// 插件面板挂载: 添加侧边栏 tab (UI 线程调用; 内容渲染经 renderPluginPanel)
    /// - 已存在同名 tab 时跳过 (幂等)
    void addPluginPanelTab(const std::string& id, const std::string& title);

    /// 插件面板摘除: 移除侧边栏 tab (UI 线程调用; 不存在时忽略)
    void removePluginPanelTab(const std::string& id);

    /// 渲染插件侧边栏面板内容 (UI 线程; 从 pluginManager UI 注册表快照读取)
    std::vector<ScrollItem> renderPluginPanel(const std::string& panelId);

    /// 渲染 Info 栏内容 (UI 线程; 内置段落 + 插件 Info 段落)
    ///
    /// - 与侧边栏实际渲染同一实现 (侧边栏 Info tab 的 render 回调即本方法)
    /// - 调用前需保证组件共享上下文的帧快照有效: 生产路径由帧循环刷新
    ///   ([refreshRenderContext]), 测试/诊断可手动调用
    std::vector<ScrollItem> renderInfoSidebar();

    /// 渲染 Info 栏底部区域 (工作目录行 + 授权按钮 + 版本/连接信息; UI 线程)
    ///
    /// - 侧边栏 Info tab 的 footer 回调即本方法
    /// - 授权按钮 (见 [toggleFullAuth]) 的可点区域登记到 shell 级命中表,
    ///   因此未渲染时不会占用点击区域
    /// - 同 [renderInfoSidebar]: 调用前需保证帧快照有效
    ftxui::Element renderInfoSidebarFooter();

    /// 切换"完全授权所有权限"状态 (UI 线程; 点击 Info 侧边栏授权按钮时调用)
    /// - 立即在本地界面反映 (乐观更新, 按钮即刻切换), 同时向服务端发送
    ///   WireSetFullAuth; 服务端应用后经 WirePermissionState 广播回真实状态
    /// - 状态本身由 agent 侧权限中间件持有, 客户端只展示与请求切换
    void toggleFullAuth();

    /// 发起启动更新检查 (GitHub Release; client io 线程调用, 由 [start] 触发)
    ///
    /// - 设置项 `启动时检查更新` (TUISettings::checkUpdateOnStartup) 关闭时不发起;
    ///   变更仅影响下次启动
    /// - 检查在 client io 线程的协程中执行 (启动后延迟若干秒再发请求, 不与首屏
    ///   渲染/agent 初始化争抢), 不阻塞 UI 线程
    /// - 失败 (无网络/超时/解析失败) 只记日志, 不打扰用户; 有更新时记录到共享状态
    ///   并在 Info 侧边栏底部显示提示行 (点击复制发布页链接)
    void startUpdateCheck();

    /// 应用更新检查结果 (UI 线程): 记录可用新版本"tag/url"并提示 (toast + 重绘)
    /// - `ok=false` (检查失败) 时只记日志
    /// - 无更新时不改动界面状态
    void applyUpdateCheckResult(agentxx::client::UpdateCheckResult result);

    // ---- 即时更新检查 (设置弹窗"检查更新"条目) ----

    /// 发起一次即时更新检查 (UI 线程; 设置弹窗"检查更新"条目激活时调用)
    ///
    /// - 与启动检查共用 [agentxx::client::checkLatestReleaseForCurrentVersion]
    ///   (client io 线程协程, 不阻塞 UI), 不受 `启动时检查更新` 开关影响
    /// - 立即 toast "正在检查更新" 作为点击反馈; 已有检查在跑时不重复发请求
    /// - 结果经 [applyManualUpdateCheckResult] 投递到 UI 线程分支处理:
    ///   有新版本 -> 打开 [UpdateNoticeOverlay]; 无更新/失败 -> toast 说明
    void requestUpdateCheckNow();

    /// 处理即时检查结果 (client io 线程): 复位进行中标志, 有新版本时同步写入
    /// 共享状态 (Info 侧边栏提示行), 再把结果投递到 UI 线程 (弹窗/toast 只能在
    /// UI 线程操作)
    void applyManualUpdateCheckResult(agentxx::client::UpdateCheckResult result);

    /// 展示即时检查结果 (UI 线程)
    /// - 有新版本: 打开更新提示弹窗 (见 [openUpdateNotice])
    /// - 无更新: toast "已经是最新版本"
    /// - 检查失败: toast 失败原因 (手动触发的动作必须有反馈, 不像启动检查那样静默)
    void showUpdateCheckResult(const agentxx::client::UpdateCheckResult& result);

    /// 打开更新提示弹窗 (UI 线程; 替换当前模态, 与"关于"弹窗一致)
    ///
    /// 弹窗显示 "新版本 {当前} -> {新}" + 发布页链接 + [ 前往下载 ] 按钮;
    /// 按钮经 [agentxx::client::openUrlInBrowser] 交给系统默认程序打开浏览器
    /// (成功则关闭弹窗, 失败保留弹窗并提示 —— 链接可直接拖选复制)
    void openUpdateNotice(std::string latestTag, std::string url);

    /// 测试辅助: 当前 toast 提示文本 (空 = 无提示)
    /// - toastText_ 为 UI 线程独占状态, 仅供单线程测试读取
    std::string_view toastText() const {
        return toastText_;
    }

    /// 刷新组件共享上下文的帧快照 (UI 线程; 帧循环每帧调用一次)
    ///
    /// - 取本帧状态快照并挂上 client 插件 UI 注册表快照
    /// - 抽成方法是因为组件渲染 (侧边栏/面板/Info) 都要求上下文里有有效快照;
    ///   测试与渲染诊断因此可以不经完整帧循环直接渲染
    void refreshRenderContext();

    /// 上报插件展示区域可见性 (UI 线程; 每帧渲染前调用一次)
    /// - 面板 = 是否为当前激活 tab; Info 段落 = Info tab 是否激活
    /// - 命中 `agentxx.client.timer` 的 `is_visible` 与 `pause_when_hidden` 门控
    ///   (管理器按值变化去重)
    void reportSidebarRegionVisibility();

    /// 上报通用 overlay 区域的可见性 (UI 线程; 打开/关闭时调用)
    ///
    /// 区域 id 固定为 `__overlay` ([AGENTXX_CLIENT_OVERLAY_OWNER]): 插件用该 id
    /// 注册 `pause_when_hidden` 定时器时可据此在弹窗关闭后暂停回调;
    /// 尺寸由弹窗自身在渲染时上报 (见 CustomOverlay::OnRender)。
    void reportOverlayVisible(bool visible);

    /// 通用 overlay 打开 (open_overlay 驱动; UI 线程; 单模态 last-wins):
    /// - type: AgentxxOverlayType (0=MERMAID 1=TEXT 2=DIFF 3=CUSTOM)
    /// - ownerPlugin: 发起插件 (CUSTOM 内按钮与 close 归因用)
    /// (由 TuiPluginAdapter 在 client io 线程经 postToUi 投递调用)
    void openOverlay(
        int         type,
        std::string title,
        std::string payload,
        std::string extraJson,
        std::string ownerPlugin
    );
    /// 通用 overlay 关闭 (UI 线程; 由 TuiPluginAdapter 经 postToUi 投递调用)
    void closeOverlay();

    /// 侧边栏内容区点击处理 (面板 / Info 段落; UI 线程)
    ///
    /// 经 [SidebarComponent::contentScrollable] 把屏幕坐标映射到"第几个子项 +
    /// 子项内局部坐标", 再按子项登记的可命中区域分类处理:
    /// - 折叠标题: 翻转宿主维护的展开状态并重绘
    /// - 动作区域: 经 [ClientPluginManager::dispatchAction] 投递到插件回调
    /// - 表单控件: 交由表单状态处理 (值编辑与提交; 见后续阶段)
    ///
    /// 返回 true 表示已处理 (调用方不再继续判定其他命中)
    bool handleSidebarRegionClick(const ftxui::Mouse& mouse);

    /// 面板/Info 段落内折叠分组的展开状态查询 (同时用于初始化与渲染)
    /// - 键 = 归属 id + ":" + 组件 id; 未记录时写入描述缺省值并返回它
    bool collapseExpanded(const std::string& ownerId, const std::string& id, bool defaultValue);

    /// 折叠分组展开状态表 (UI 线程独占; 键 = 归属 id + ":" + 组件 id)
    std::map<std::string, bool, std::less<>> collapseStates_;

    /// 侧边栏内容 (插件面板 / Info 段落) 的 markdown 渲染器生命周期
    ///
    /// Element 内部的容器/链接 Box 指向 markdown 的 DomBuilder, 必须随元素一同存活;
    /// 面板与 Info 段落每帧重建元素, 故按"帧"保存: 每帧轮换到新容器并保留上一代
    /// (上一帧元素可能仍被滚动容器的子项缓存持有)。
    std::vector<std::vector<std::unique_ptr<markdown::DomBuilder>>> sidebarMdBuilders_;

    /// 插件表单 (面板 / Info 段落 / overlay 的控件): 归属 id → 状态 + 最近一次描述
    ///
    /// 控件值、勾选态、选中项与校验提示都由宿主维护 (UI 线程独占), 插件只收到
    /// 提交结果 (`{"values":{控件 id: 值}}` 经动作通道回传), 不进入 UI 线程。
    struct PluginFormData {
        /// 归属插件名 (动作派发需要)
        std::string                    plugin;
        agentxx::client::UiFormState   state;
        std::vector<agentxx::ui::Item> items;
    };

    /// 归属 id → 表单数据 (键 = 面板 id / Info 段落 id)
    std::map<std::string, PluginFormData, std::less<>> pluginForms_;

    /// 当前拥有键盘焦点的表单归属 id (空 = 无焦点; 有焦点时字符键进入该表单)
    std::string formFocusedOwner_;

    /// 取/刷新某归属的表单数据 (按最新描述初始化控件, 保留用户已编辑的值)
    /// - 返回映射内引用 (调用方直接修改状态; 键不存在时创建)
    /// - 控件状态变化后调用方应 [`postRedraw`] 触发重绘
    PluginFormData&
        formFor(const std::string& ownerId, const std::string& plugin, std::vector<agentxx::ui::Item> items);

    /// 侧边栏表单键盘输入 (UI 线程; 有焦点且已消费返回 true)
    bool handleSidebarFormKey(const ftxui::Event& event);

    /// 提交表单 (校验 → 组装 `{"values":{...}}` → 动作通道回传 __submit)
    /// - 校验失败时写各控件提示并重绘 (不提交)
    bool submitSidebarForm(const std::string& ownerId, const std::string& plugin);

    /// 取消表单 (动作通道回传 __cancel)
    bool cancelSidebarForm(const std::string& ownerId, const std::string& plugin);

    /// 显示 toast (任意线程可调用; 内部投递到 UI 线程)
    void uiToast(std::string text, int level);

    /// 处理服务端握手确认 (记录服务端 deviceId 与 workDir)
    void onHelloAck(const agentxx::agent::WireHelloAck& ack);

    /// 代发用户消息 (client io 线程; 与用户输入同排队语义: 未连接/流式中
    /// 进 pendingInputs, 连接后按轮次分发; 发送后通知事件接收器)
    void sendPluginUserInput(std::string text);

    /// 跨端插件数据上行 (client io 线程): WirePluginDataUp → agent 侧插件
    /// 返回 true 表示已投递 (未连接等失败返回 false)
    bool sendPluginDataUp(
        const std::string& plugin,
        const std::string& event,
        const std::string& json
    );

    /// 设置连接状态 (跨线程安全: 更新 sharedState 并触发重绘)
    /// 状态枚举 ConnState 定义于
    /// [tui_state.h](/agent/client/include/agentxx-client/io/tui/framework/tui_state.h)
    /// (TUIRenderState::connState)
    void setConnState(ConnState state);

    /// AgentIOBase::onServerReady 覆写: 置 Connected 并刷新待发送队列
    /// (连接建立后由 mode_runners 调用)
    void onServerReady() override;

    /// AgentIOBase::onServerProgress 覆写: 更新 banner 当前启动步骤
    /// (agent 线程同步调用, 经 sharedState 锁 + postRedraw 安全更新)
    void onServerProgress(std::string_view step) override;

    /// 等待用户点击"重试" (连接失败后由连接协程 await; TUI 退出时尽快返回,
    /// 避免失败后用户退出导致协程永久挂起阻塞 io_context)
    asio::awaitable<void> waitRetry();

    /// 用户点击 banner 上的"重试"按钮 (UI 线程调用): 置 Connecting 并唤醒 waitRetry
    void requestRetry();

    /// 界面语言切换后的即时刷新 (UI 线程, 由设置弹窗语言变化回调调用):
    /// - 消息列表缓存失效 (banner 等静态文本按语言缓存)
    /// - 日志行缓存清空 (重新按当前语言重建 "[空]" 等标签)
    /// - 侧边栏 Info/Logs 常驻标签标题与已建 tab 标题按新语言更新
    /// - 输入框占位符即时刷新 (placeholder 以引用绑定, 见 InputComponent)
    void refreshLanguage();

    asio::awaitable<std::optional<std::string>> getInput() override;
    asio::awaitable<utilxx_base::Json>          handleInterrupt(
                 std::string_view sessionId,
                 std::string_view interruptNode,
                 std::string_view interruptValue,
                 std::string_view interruptArgJson
             ) override;
    void requestCancel(std::string sessionId) override;

    /// 记录待应用模型选择
    /// 随下一条发送的用户消息 (WireUserInput.model) 携带, BaseAgent 执行
    /// 新一轮会话时 (runTurnAsync 开头 selectModel) 自动切换。
    /// - 模型选择弹窗确认 (UI 线程) 与远程 TUI 启动 --model 参数 (client io
    ///   线程) 共用; 内部加锁, 任意线程可调用
    /// - 空模型名忽略; 重复选择以最后一次为准
    void setPendingModel(std::string model);

    /// 供组件访问共享状态 (UI 线程渲染/事件时使用)
    TUISharedState& sharedState() {
        return sharedState_;
    }

    /// 会话选择弹窗确认后的切换逻辑 (UI 线程):
    /// - 更新本地 sessionId 绑定与重连握手 sessionId (WS 模式)
    /// - 发送 WireSwitchSession, 服务端回推全量 Sync/模型/上下文统计 (WireModelInfo
    ///   / WireContextStats) 恢复界面; TUI 不持有 Session (属于 server-io 线程)
    void switchToSession(std::string newSessionId);

    /// 当前会话 sessionId 的跨线程安全读写:
    /// UI 线程切换会话时写入, client 线程发送用户输入时读取
    std::string currentSessionId() const {
        std::lock_guard<std::mutex> lock(sessionIdMutex_);
        return sessionId_;
    }

    void setCurrentSessionId(std::string newSessionId) {
        std::lock_guard<std::mutex> lock(sessionIdMutex_);
        sessionId_ = std::move(newSessionId);
    }

    void onPeerMessage(agentxx::agent::WireMessage msg) override;

protected:

    // ---- AgentIOBase 被动接收回调 (client 端点实现; 仅由 onPeerMessage 分发) ----
    void onDelta(const agentxx::agent::WireDelta& delta) override;
    void onSync(const agentxx::agent::WireSyncPayload& payload) override;
    void onTurnResult(const agentxx::agent::WireTurnResult& result) override;
    void onContextStats(const agentxx::agent::WireContextStats& stats) override;

private:

    // -----------------------------------------------------------------------
    // 协议处理辅助 (client 线程, 须持有 sharedState_.mutex())
    // -----------------------------------------------------------------------
    /// 重置末尾最近连续处于 running 状态的 tool 消息为非 running (!toolFinished -> toolFinished =
    /// true) (新消息到达 / 轮次开始 / 输入发送时调用, 避免会话恢复或异常中断后残留的 tool
    /// 一直显示正在运行)
    void resetTrailingRunningToolsLocked(TUIRenderState& st);

    void pushCurrentTokenLocked(TUIRenderState& st);
    void cancelCurrentRunLocked(TUIRenderState& st);
    void sendUserInputLocked(
        TUIRenderState&                              st,
        std::string                                  text,
        std::vector<agentxx::agent::MediaAttachment> attachments = {}
    );
    void onMessageQueueUpdate(const agentxx::agent::WireMessageQueueUpdate& update);

    // ---- 历史分页 (viewMessages 尾窗同步 + 向上滚动分页拉取) ----

    /// 服务端页响应处理 (client 线程): 校验会话/连续性后前插到已加载窗口
    /// 上方, 更新窗口元数据; 组件锚定经 UI 动作队列投递 (LazyScrollable
    /// ::notifyPrepended 把锚点随索引平移, 视口内容零跳变)
    void onViewMessagesPage(const agentxx::agent::WireViewMessagesPage& page);
    /// 请求更早历史 (ctx_.requestMoreHistory 入口; UI 线程触发):
    /// - 已有请求未返回时直接忽略 (historyLoading 去重); hasMoreHistory 边界判断
    /// - 页大小 kHistoryPageSize 与服务端默认兜底一致
    void requestOlderHistory();
    /// 历史分页每页条数 (与服务端 SessionServerAgentIO 的默认兜底一致)
    static constexpr uint32_t kHistoryPageSize = 100;

    // ---- 会话列表分页 (会话选择弹窗数据源, keyset 游标按最近活动降序) ----

    /// 请求服务端列举目录 (跨设备附件选择)
    void requestServerListDir(
        std::string                                                   path,
        std::vector<std::string>                                      allowedExtensions,
        std::function<void(const agentxx::agent::WireListDirResult&)> callback
    );

    /// 会话列表页响应处理 (client 线程): 首页/全量响应替换本地列表, 后续页追加,
    /// 更新 totalCount/hasMore 分页元数据 (旧版服务端全量响应按替换处理)
    void onSessionListPage(const agentxx::agent::WireSessionList& resp);
    /// 请求下一页会话列表 (ctx_.requestMoreSessions 入口; UI 线程触发):
    /// - 已有请求未返回时直接忽略 (sessionListLoadingMore 去重); sessionListHasMore 边界判断
    /// - 游标取已加载列表最后一条的 (lastActiveMs, sessionId)
    void requestNextSessionListPage();
    /// 会话列表分页每页条数 (首屏一页即可覆盖弹窗可视区域数倍, 减少请求次数)
    static constexpr uint32_t kSessionListPageSize = 50;

    /// 将 UI 线程独占的组件操作 (弹窗开关/消息列表状态等) 投递到 UI 线程执行。
    /// client 线程 (onDelta/onSync/onPeerMessage) 不得直接触碰组件树
    /// (modal_/messageList_ 等由 UI 线程独占), 必须经本接口排队,
    /// 由帧循环开头处理, 消除跨线程数据竞争。
    void enqueueUiAction(std::function<void()> fn);

    void postRedraw();

    /// 打开模型选择器模态
    void openModelSelector();
    /// 打开设置模态
    void openSettings();
    /// 打开关于模态
    void openAbout();
    /// 打开插件全局快捷键列表模态 (只读; 由设置模态的"快捷键"条目打开)
    void openKeybindList();
    /// 打开会话选择模态 (F3 / 状态栏 [F3] Sessions 按钮):
    /// - 仅当前会话非运行状态时可打开 (否则提示先停止当前会话)
    /// - 请求服务端会话列表并展示; 确认后经 WireSwitchSession 切换
    void openSessionSelector();

    /// 打开多模态文件选择弹窗 (FilePickerOverlay)
    /// - 按当前活动模型的多模态能力过滤可选文件类型
    /// - 选中文件后读取、预检大小、Base64 编码为 Data URL 并挂载到输入栏附件托盘
    void openFilePickerOverlay();

    /// 屏幕上方提示 (toast): 设置提示文本并安排 kToastDuration 后触发重绘,
    /// 由 UI 线程渲染时检查超时并清除 (toastText_/toastShownAt_ 为 UI 线程独占,
    /// 定时器回调仅触发重绘, 不直接写状态, 无跨线程竞争)
    void showToast(std::string text);

    /// 通知事件接收器: 用户输入已发送 (sendUserInputLocked 内部调用;
    /// 任意线程, 内部按需 post 到 client io 线程)
    void notifyUserInputSent(const std::string& sessionId, const std::string& text);

    /// client 插件管理器 (装配后不可变; uiRegistrySnapshot/hasCommand 线程安全)
    std::shared_ptr<agentxx::plugin::ClientPluginManager> pluginManager_;

    /// F12: 切换日志窗口 tab
    void toggleLogWindow();
    /// 确保 Info/Logs 侧边栏 tab 已创建 (不存在时 addTab 并激活;
    /// 供初始展开判断与 tabs 列表常驻标签点击回调复用)
    void ensureInfoSidebarTab();
    void ensureLogSidebarTab();
    /// 打开加载失败组件列表模态 (Info 侧边栏 Append "Failed" 组 [view] 按钮触发)
    void openFailedAppendComponents();

    /// 刷新界面语言后更新侧边栏常驻/已建 tab 标题 (UI 线程)
    void refreshSidebarTabTitles();

    /// 侧边栏渲染辅助 (renderInfoSidebarFooter/toggleFullAuth 为公开接口, 见类头部)
    std::vector<ScrollItem> renderLogWindow();
    ftxui::Element          renderLogSidebarFooter();

    /// 在整棵界面树 (主界面或弹窗模态) 之上叠加 toast 提示 (UI 线程; 每帧渲染调用)
    ///
    /// - 提示画在模态层之上: 弹窗打开时主界面整棵树不参与渲染 (见 ModalContainer),
    ///   提示若只画在主界面里会被弹窗盖住 (例如在设置弹窗里点"检查更新"的进行中
    ///   与结果提示)
    /// - 渲染时检查超时: 超过 kToastDuration 清除提示
    ///   (toastText_/toastShownAt_ 为 UI 线程独占, 仅在本帧渲染中读写)
    ftxui::Element applyToastOverlay(ftxui::Element content);

    // -----------------------------------------------------------------------
    // 状态
    // -----------------------------------------------------------------------
    TUISharedState sharedState_;

    TUITheme theme_;
    /// 当前会话 sessionId (切换会话时由 UI 线程写入, client 线程发送输入时读取;
    /// 经 sessionIdMutex_ 保护, 见 currentSessionId()/setCurrentSessionId())
    std::string           sessionId_;
    mutable std::mutex    sessionIdMutex_;
    asio::any_io_executor ex_;

    std::mutex                                screenMutex_;
    std::shared_ptr<ftxui::ScreenInteractive> screen_;
    std::thread                               uiThread_;
    std::atomic<bool>                         running_{false};
    std::atomic<bool>                         awaitingInterruptInput_{false};

    std::shared_ptr<LineChannel> inputChannel_;
    std::shared_ptr<TUILogSink>  logSink_;
    std::string                  remoteUrl_;
    std::string                  dataDir_;
    std::string                  workDir_;
    std::string                  clientDeviceId_;
    std::string                  serverDeviceId_;
    std::string                  serverWorkDir_;

    std::mutex listDirMutex_;
    uint64_t   nextListDirReqId_{0};
    std::unordered_map<uint64_t, std::function<void(const agentxx::agent::WireListDirResult&)>>
        pendingListDirCallbacks_;

    /// UI 线程组件 (start() 中创建, UI 线程独占)
    TUICtx                                ctx_;
    std::shared_ptr<MessageListComponent> messageList_;
    std::shared_ptr<SidebarComponent>     sidebar_;
    std::shared_ptr<StatusBarComponent>   statusBar_;
    std::shared_ptr<InputComponent>       inputBar_;
    std::shared_ptr<ModalContainer>       modal_;

    /// 日志行缓存 (UI 线程, 供 renderLogWindow 使用)
    std::vector<ftxui::Element> logLineCache_;
    uint64_t                    logCachePoppedCount_ = 0;
    /// 上次快照的日志行数 (用于判断日志是否新增, 避免每帧全量 snapshot 拷贝)
    size_t logCacheLineCount_ = 0;

    // ---- 渲染帧统计 (基准测试/诊断用; UI 线程写入, 任意线程读取) ----
    /// 记录一帧的组件树构建耗时 (仅在启用
    /// [agentxx::agent::AgentConfigStatic::enableBenchmark] 时调用)
    /// - `elapsed`: 本帧组件树构建耗时
    void recordFrameStats(std::chrono::steady_clock::duration elapsed) noexcept;

    /// 已渲染帧数
    std::atomic<uint64_t> frameCount_{0};
    /// 渲染耗时累计 (纳秒)
    std::atomic<uint64_t> frameTotalNs_{0};
    /// 单帧最大渲染耗时 (纳秒)
    std::atomic<uint64_t> frameMaxNs_{0};

    /// 重绘请求合并 (postRedraw 由 client/UI 线程并发调用):
    /// - redrawPosted_: 已投递尚未处理的 Custom 标记, 仅当无待处理事件时才 Post,
    /// 同帧内多次请求合并为一次
    /// - redrawSeq_:    请求计数, UI 线程在帧结束时据此判断帧期间是否有请求被合并
    ///                  (被合并进本帧渲染, 而本帧快照取的是帧开头, 可能未反映其状态变更),
    ///                  若有则补 Post 一帧, 保证以最新快照重绘, 避免请求丢失
    std::atomic<uint64_t> redrawSeq_{0};
    std::atomic<bool>     redrawPosted_{false};

    // ---- 屏幕上方提示 (toast, UI 线程独占) ----
    /// 当前 toast 文本 (空 = 无提示); 渲染时检查超时并清除
    std::string toastText_;
    /// toast 显示起始时刻 (渲染时据此判断是否超过 kToastDuration)
    std::chrono::steady_clock::time_point toastShownAt_;
    /// toast 超时定时器 (client io_context 上): 超时后仅触发重绘,
    /// 由 UI 线程渲染时清除状态; stop() 时 cancel 避免挂起等待
    std::shared_ptr<asio::steady_timer> toastTimer_;
    /// toast 显示时长
    static constexpr std::chrono::seconds kToastDuration{3};

    // ---- 即时更新检查 (设置弹窗"检查更新"; 跨线程原子标志) ----
    /// 是否已有即时检查在跑 (UI 线程置位, client io 线程的协程收尾时复位):
    /// 用户连点"检查更新"时不重复发起请求
    std::atomic<bool> updateChecking_{false};

    // ---- 鼠标拖选跟踪 (UI 线程独占, 用于"松开即复制") ----
    /// 左键是否处于按下状态 (Left Pressed 置位, Released 复位)
    bool mouseDown_ = false;
    /// 按下后是否发生过拖动 (Left Moved 置位); Released 且该标志为真时
    /// 判定为一次拖选完成 -> 自动复制选中文本并 toast 提示。
    /// 单击 (无拖动) 不复制, 保持原有点击交互 (按钮/折叠/拖拽条等)
    bool mouseDragged_ = false;

    // ---- UI 线程动作队列 ----
    /// client 线程投递、UI 线程 (帧循环开头) 消费的组件操作队列;
    /// 与 sharedState_ 无关, 独立加锁 (消费方仅短暂持有, 不嵌套 sharedState 锁)
    std::mutex                         uiActionsMutex_;
    std::vector<std::function<void()>> uiActions_;

    // ---- 中断请求 (client 线程独占) ----
    /// 当前进行中中断的 wire id (onPeerMessage 收到 WireInterruptRequest 时设置,
    /// 与 handleInterrupt 同线程顺序执行, 无需同步)
    int64_t interruptWireId_ = 0;
    /// 进行中中断的结果回传通道: wireId → 通道 (中断输入消息共享引用)。
    /// handleInterrupt 插入/移除; WireInterruptExpired / stop() 关闭通道以终止等待
    std::map<int64_t, std::shared_ptr<InterruptResultChannel>> activeInterrupts_;

    // ---- 连接失败重试 (跨线程原子标志) ----
    /// 用户点击 banner"重试"按钮的标志 (UI 线程 requestRetry 置位;
    /// 连接协程 waitRetry 轮询消费并据此返回重试连接)
    std::atomic<bool> retryRequested_{false};

    /// 主界面零散按钮的命中登记表 (跨组件、动作实现在 TUIClientAgentIO 内):
    /// - Info 侧边栏 Append "Failed" 组 [view] 按钮 → 打开加载失败组件列表
    /// - Logs 侧边栏底部 [Menu] 按钮 → 打开日志菜单
    /// 由对应渲染函数在渲染期登记, 每帧渲染入口清空 (见 mainRenderer):
    /// 按钮未渲染 (侧边栏 tab 未激活 / 无失败项) 时不会命中。
    agentxx::client::UiHitMap shellHits_;

    /// 命中 id (shell 级按钮)
    static constexpr std::string_view kFailedViewHitId = "shell/info-failed-view";
    static constexpr std::string_view kLogsMenuHitId   = "shell/logs-menu";
    /// Info 侧边栏底部"完全授权 / 询问授权"切换按钮 (点击切换授权模式)
    static constexpr std::string_view kAuthToggleHitId = "shell/info-auth-toggle";
    /// Info 侧边栏底部"发现新版本"提示行 (点击复制发布页链接)
    static constexpr std::string_view kUpdateNoticeHitId = "shell/info-update-notice";

    /// 处理 shell 级按钮命中 (UI 线程; 由全局鼠标事件经 shellHits_ 分发)
    void handleShellHit(std::string_view id);

    /// 打开 Logs 侧边栏底部 [Menu] 菜单弹窗 (LLM 上下文 / 总结上下文 / 清空日志)
    void openLogsMenu();

    /// 当前通用 overlay 的发起插件 (CUSTOM 内按钮与 close 归因用;
    /// 单模态 last-wins, 仅记日志/归因, 不做强互斥)
    std::string overlayOwnerPlugin_;

    static constexpr const char* kLogTabId            = "xx_logs";
    static constexpr const char* kInfoTabId           = "xx_info";
    static constexpr int         kInfoSidebarMinWidth = 120;
};

} // namespace agentxx::client
