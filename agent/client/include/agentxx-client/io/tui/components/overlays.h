#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/ui_action_list.h"
#include "agentxx-client/io/tui/framework/ui_hit.h"
#include "agentxx-client/io/tui/scrollable.h"
#include "agentxx-client/io/tui/ui_components.h"
#include "agentxx/agent/conversation_types.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include "utilxx_base/json.h"
#include <functional>
#include <map>
#include <markdown/dom_builder.hpp>
#include <markdown/state_diagram.hpp>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx::client {

/// 模型选择器弹窗组件 (每帧按服务端模型列表重建条目)
///
/// 交互 (上下键移动/Enter 确认/鼠标点击命中/Esc 关闭) 与选中高亮由 [UiActionList]
/// 统一实现; 本组件只负责把模型列表映射为条目表, 以及确认后的模型切换。
class ModelSelectorOverlay : public ftxui::ComponentBase {
public:

    explicit ModelSelectorOverlay(TUICtx& ctx);

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    /// 确认回调 (参数: 选中的模型名)
    void onConfirm(std::function<void(std::string)> fn) {
        onConfirm_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

    /// 测试辅助: 选中项下标 (-1 = 无)
    int selectedIndex() const {
        return list_.selectedIndex();
    }

private:

    /// 重建条目表 (首次打开时把选中项对齐到当前使用的模型)
    void buildItems();
    /// 确认选中模型 (写入当前模型 + 通知外部; 关闭弹窗)
    void confirmItem(std::string_view model);
    /// 执行条目激活后请求的关闭 (见 [closeRequested_] 说明)
    void flushActivation();
    void close();

    TUICtx&       ctx_;
    UiActionList  list_;
    UiHitMap      hits_;
    UiActionStyle style_;

    /// 首次渲染时是否已把选中项对齐到当前使用的模型 (只对齐一次, 之后以用户选择为准)
    bool initialAligned_ = false;

    /// 条目激活动作里只记录"该关闭弹窗", 实际关闭 (onClose_ -> 移除模态) 在条目
    /// 列表事件处理返回后执行 —— 直接在闭包内关闭会在列表迭代/回调执行过程中
    /// 析构本对象 (闭包自身就在被销毁的容器里)
    bool closeRequested_ = false;

    std::function<void()>            onClose_;
    std::function<void(std::string)> onConfirm_;
};

/// 会话选择弹窗组件 (F3 / 状态栏 [F3] Sessions 按钮)
/// - 列表顶部固定一项 "新会话" (选中确认后创建全新会话, 不切换历史)
/// - 列表项两行: 第一行会话名称 (title, 空时回退 sessionId), 第二行最近活动日期
/// - Up/Down 选择, Enter/鼠标点击切换会话, Esc 关闭
/// - 列表分页加载: 打开弹窗先加载最新一页; 选择项下移接近已加载列表末尾时
///   经 ctx_.requestMoreSessions 自动预取下一页, 尾部显示加载进度提示行
/// - 首页未到达时显示 loading (sessionListLoaded == false)
class SessionSelectorOverlay : public ftxui::ComponentBase {
public:

    explicit SessionSelectorOverlay(TUICtx& ctx);

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    /// 切换会话回调 (参数: 目标 sessionId)
    void onSelect(std::function<void(std::string)> fn) {
        onSelect_ = std::move(fn);
    }

    /// 新建会话回调 (选中顶部 "新会话" 项时触发)
    void onNewSession(std::function<void()> fn) {
        onNewSession_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

    /// 测试辅助: 选中项下标 (-1 = 无)
    int selectedIndex() const {
        return list_.selectedIndex();
    }

private:

    /// 重建条目表 (0 = 新会话入口, 其后为持久化会话)
    void buildItems();
    /// 条目激活动作: 记录目标会话并请求关闭 (实际切换在列表事件处理返回后执行)
    void requestClose(std::string sessionId);
    /// 执行 [requestClose] 请求的关闭与切换 (空 sessionId = 新建会话)
    void flushActivation();

    TUICtx&       ctx_;
    UiActionList  list_;
    UiHitMap      hits_;
    UiActionStyle style_;

    /// 请求关闭并切换到的会话 id (空 = "新会话" 入口; 见 [requestClose])
    bool        closeRequested_ = false;
    std::string pendingSessionId_;

    std::function<void()>            onClose_;
    std::function<void(std::string)> onSelect_;
    std::function<void()>            onNewSession_;

    /// 条目 id: "新会话" 入口固定为 [kNewSessionId], 会话项为 kSessionIdPrefix + sessionId
    static constexpr const char* kNewSessionId    = "new-session";
    static constexpr const char* kSessionIdPrefix = "session/";
    /// 选择项接近已加载列表末尾时的预取提前量 (项)
    static constexpr int kSessionPrefetchAhead = 3;
};

/// 设置弹窗组件
///
/// 条目按分组显示 (分组标题行不可选中/点击, 见 [UiActionItem::group]):
/// - 界面: 主题切换 (Dark/Light) / 动画等级 (Disabled/Low/Medium/High/Ultra) /
///   界面语言 (简体中文 zh-cn / English en-us; 见 TuiI18n 翻译表)
/// - 显示: 日志等级 (Trace/Debug/Info/Warn/Error/Out) / 末尾思考展示模式
///   (Auto Expand / Single Line); 均见 TUISettings
/// - 更新: 启动时检查更新开关 + "检查更新"条目 (发起一次即时检查, 有新版本时
///   由外部打开 [UpdateNoticeOverlay])
/// - 其他: 快捷键 (显示插件已注册的全局快捷键条数; 打开 [KeybindListOverlay]
///   查看完整列表) / 信息 (打开关于弹窗; 显示版本/路径/插件等信息)
///
/// 交互: Up/Down 选择条目, Enter 应用/切换 (循环切换); 也支持鼠标点击;
/// 内容超出终端可用高度时内容区可滚动 (选中项自动滚入视口, 滚轮 = 上/下移动选中项)。
/// 所有条目切换后均保持弹窗打开, 便于连续调整; 由 [Esc] 关闭。
/// 条目 (分组/标签/当前值/切换动作) 由 [buildItems] 一处声明, 交互与高亮由
/// [UiActionList] 统一实现 (新增设置项只需往条目表加一行)。
class SettingsOverlay : public ftxui::ComponentBase {
public:

    explicit SettingsOverlay(TUICtx& ctx);

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    /// 主题变化回调 (供外部清理渲染缓存; 弹窗保持打开, 主题立即生效)
    void onThemeChange(std::function<void()> fn) {
        onThemeChange_ = std::move(fn);
    }

    /// 日志等级变化回调 (供外部清空已收集日志行, 重新按新等级收集)
    void onLogLevelChange(std::function<void()> fn) {
        onLogLevelChange_ = std::move(fn);
    }

    /// 动画等级变化回调 (供外部同步"是否允许动态效果"的门控: 插件定时器在
    /// Disabled 等级下不注册; 插件据此降级为静态展示)
    void onAnimationLevelChange(std::function<void()> fn) {
        onAnimationLevelChange_ = std::move(fn);
    }

    /// 界面语言变化回调 (供外部刷新静态文本/缓存: 侧边栏标签、输入框
    /// 占位符、消息列表缓存等; 语言立即生效并持久化)
    void onLanguageChange(std::function<void()> fn) {
        onLanguageChange_ = std::move(fn);
    }

    /// 关于弹窗回调 (供外部打开 AboutOverlay)
    void onAbout(std::function<void()> fn) {
        onAbout_ = std::move(fn);
    }

    /// 快捷键列表弹窗回调 (供外部打开 [KeybindListOverlay])
    void onKeybindList(std::function<void()> fn) {
        onKeybindList_ = std::move(fn);
    }

    /// "检查更新"回调 (供外部发起一次更新检查)
    void onCheckUpdate(std::function<void()> fn) {
        onCheckUpdate_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

    /// 测试辅助: 选中项下标 (-1 = 无)
    int selectedIndex() const {
        return list_.selectedIndex();
    }

private:

    /// 重建条目表 (每帧刷新各设置项的当前值文本)
    ///
    /// 条目顺序即弹窗显示顺序; `group` 字段相同的连续条目属同一分组,
    /// 分组标题变化处自动插一行标题 (见 [UiActionList::render])。
    void buildItems();

    /// 已注册的插件全局快捷键条数 (读 UI 注册表快照; 未装配插件管理器时为 0)
    size_t keybindCount() const;

    /// 循环切换主题: Dark -> Light -> Dark (需要访问 ctx_.theme, 非静态)
    void cycleTheme();
    /// 循环切换动画等级: Disabled -> Low -> Medium -> High -> Ultra -> Disabled
    /// (需要触发 onAnimationLevelChange_, 非静态)
    void cycleAnimationLevel();
    /// 循环切换日志等级: Trace -> Debug -> Info -> Warn -> Error -> Out -> Trace
    /// (需要访问 onLogLevelChange_, 非静态)
    void cycleLogLevel();
    /// 循环切换末尾思考展示模式: Auto Expand -> Single Line -> Auto Expand
    static void cycleTailThinkingMode();
    /// 循环切换界面语言: 简体中文 <-> English (需要访问 onLanguageChange_, 非静态)
    void cycleLanguage();
    /// 切换"启动时检查更新"开关: 开 <-> 关 (持久化; 仅影响下次启动)
    static void cycleCheckUpdateOnStartup();

    TUICtx&      ctx_;
    UiActionList list_;
    /// 条目命中表 (每帧重登记; 只有本帧渲染出来的条目可点击)
    UiHitMap     hits_;
    /// 条目配色 (每帧按当前主题刷新: 分组标题色与切换主题都取自这里)
    UiActionStyle style_;

    std::function<void()> onClose_;
    std::function<void()> onThemeChange_;
    std::function<void()> onLogLevelChange_;
    std::function<void()> onAnimationLevelChange_;
    std::function<void()> onLanguageChange_;
    std::function<void()> onAbout_;
    std::function<void()> onKeybindList_;
    std::function<void()> onCheckUpdate_;
};

/// 更新提示弹窗 (设置弹窗"检查更新"条目发现新版本时打开)
///
/// 内容为一小段固定版式 (内容短, 不需要滚动容器):
/// ```text
/// 新版本 {当前版本} -> {新版本}
/// · {发布页链接}
/// [ 前往下载 ]
/// ```
/// 版本行取编译期版本 `agentxx::kVersion` 与检查到的最新发布标签;
/// 链接行是 GitHub Release 发布页 (可直接拖选复制)。
///
/// 交互: 点击 [ 前往下载 ] / Enter 触发 [onDownload] (由外部用系统默认程序
/// 打开浏览器, 并决定是否关闭弹窗); Esc 关闭; 其余鼠标事件被吞掉 (模态)。
class UpdateNoticeOverlay : public ftxui::ComponentBase {
public:

    UpdateNoticeOverlay(
        TUICtx&     ctx,
        std::string currentVersion,
        std::string latestTag,
        std::string url
    );

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    /// [ 前往下载 ] 触发回调 (打开浏览器由外部实现, 便于测试替换)
    void onDownload(std::function<void()> fn) {
        onDownload_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

    /// 测试辅助: 上一帧 [ 前往下载 ] 按钮的屏幕区域 (未渲染时为空区域)
    ftxui::Box downloadButtonBox() const;

private:

    /// 触发"前往下载" (记下事件已消费; 打开动作由外部回调执行)
    void activateDownload();

    TUICtx&               ctx_;
    std::string           currentVersion_;
    std::string           latestTag_;
    std::string           url_;
    UiHitMap              hits_;
    std::function<void()> onClose_;
    std::function<void()> onDownload_;
    /// 按钮命中 id (单按钮弹窗; 命中后统一走 [activateDownload])
    static constexpr std::string_view kDownloadHitId = "download";
};

/// 插件全局快捷键列表弹窗 (只读; 由设置弹窗的"快捷键"条目打开)
///
/// 数据来源: [TUICtx::pluginManager] 的 UI 注册表快照 (UI 线程短锁读, 渲染期间
/// 不进入插件代码, 与其它接入点同一模型):
/// - 已注册快捷键 (`agentxx.client.keybind` 表): 键位 + 说明 + 归属插件
/// - 被占用键位的注册失败记录: 请求方 + 占用方 (见 `ClientKeybindConflict`)
///
/// 让用户在界面上确认"哪个插件占了哪个键位、哪个插件的快捷键没抢到",
/// 插件禁用/卸载时注册与记录一并从快照消失, 列表随之刷新。
///
/// 内容区可滚动 (条目数不受弹窗高度限制);
/// 交互: Up/Down/PageUp/PageDown/Home/End/滚轮 滚动, Esc 关闭 (只读, 没有编辑操作)。
class KeybindListOverlay : public ftxui::ComponentBase {
public:

    explicit KeybindListOverlay(TUICtx& ctx);

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

    /// 测试辅助: 最近一次渲染的键位条目数 (不含冲突段; 0 表示当前无注册)
    size_t keybindCount() const {
        return keybindCount_;
    }

private:

    std::vector<ScrollItem> buildItems();

    TUICtx&                     ctx_;
    std::shared_ptr<Scrollable> scrollable_;
    std::function<void()>       onClose_;
    /// 最近一次渲染的键位条目数 (诊断/测试)
    size_t keybindCount_ = 0;
};

/// 关于弹窗组件 (About)
///
/// 显示程序基本信息 (Scrollable 内容, Esc 关闭):
/// - 程序名称 + 版本号
/// - 当前可执行程序文件路径
/// - Server-IO 类型 (Inner Server / 远程 URL)
/// - 内嵌编译的插件列表 (builtin plugins)
/// - 当前加载的插件列表 (agent 侧 + client 侧)
/// - 数据文件夹绝对路径 (yaml data_dir)
/// - 当前会话工作目录绝对路径
class AboutOverlay : public ftxui::ComponentBase {
public:

    explicit AboutOverlay(TUICtx& ctx);

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    std::vector<ScrollItem> buildItems();

    TUICtx&                     ctx_;
    std::shared_ptr<Scrollable> scrollable_;
    std::function<void()>       onClose_;
};

/// 待发送消息队列弹窗组件 (内容区条目 + 每条一个删除按钮 + 标题栏清空按钮)
///
/// 命中区域经 [UiHitMap] 登记, 每帧由 OnRender 重建:
/// - 删除按钮以 `kDeleteHitPrefix + 条目 id` 登记 (登记在条目之前, 命中查询
///   按登记顺序返回第一个匹配项, 因此删除按钮优先于条目本体)
/// - 条目本体以条目 id 登记 (点击展开/折叠)
/// - "清空" 按钮以 [kClearHitId] 登记
/// - 弹窗关闭/条目消失时不登记 => 点击不会命中 (不再依赖"是否已清空成员 Box")
class PendingInputsOverlay : public ftxui::ComponentBase {
public:

    explicit PendingInputsOverlay(TUICtx& ctx) :
        ctx_(ctx) {}

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    void onClear(std::function<void()> fn) {
        onClear_ = std::move(fn);
    }

    void onDeleteItem(std::function<void(std::string itemId)> fn) {
        onDeleteItem_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

    /// 删除按钮命中 id 前缀 (id = kDeleteHitPrefix + 条目 id)
    static constexpr std::string_view kDeleteHitPrefix = "delete/";
    /// "清空" 按钮命中 id
    static constexpr std::string_view kClearHitId = "clear";

private:

    /// 命中载荷: 控件类型 + 条目 id (条目 id 用于定位待发送队列项)
    struct HitInfo {
        enum class Kind : uint8_t {
            Clear,  ///< 标题栏 "清空" 按钮
            Delete, ///< 条目右侧删除按钮
            Item,   ///< 条目本体 (点击展开/折叠)
        };

        Kind        kind = Kind::Item;
        std::string itemId;
    };

    /// 处理左键释放命中 (返回是否消费)
    bool handleClick(const ftxui::Mouse& mouse);

    /// 测试辅助: 取指定命中项的屏幕区域 (不存在返回空区域)
    ftxui::Box boxOf(HitInfo::Kind kind, std::string_view itemId = {}) const;

    TUICtx&                          ctx_;
    std::function<void()>            onClose_;
    std::function<void()>            onClear_;
    std::function<void(std::string)> onDeleteItem_;

    /// 命中区域登记表 (每帧由 OnRender 重建; 未渲染的按钮/条目不命中)
    agentxx::client::UiHitRegistry<HitInfo> hits_;
};

/// 上下文弹窗组件 (显示 llm messages)
///
/// 仿消息列表的可折叠展示 (默认折叠, 点击/Enter/Space 展开):
/// - 每条消息一个折叠单元: 折叠头 "+ [role] 预览", 展开后显示完整原始 JSON
///   (dump(2) 美化多行, 含 tool_calls/工具结果等全部字段, 便于调试查看)
/// - 展开内容可变高度, 经 Scrollable 惰性布局/绘制, 不裁剪截断
/// - 交互: 点击消息头行 / Enter / Space 切换折叠; 滚轮 / Up/Down / PgUp/PgDn 滚动
class ContextOverlay : public ftxui::ComponentBase {
public:

    explicit ContextOverlay(TUICtx& ctx) :
        ctx_(ctx) {
        scrollable_ = std::make_shared<Scrollable>([this]() -> std::vector<ScrollItem> {
            return buildItems();
        });
        // 上下文为静态快照: 打开时从顶部开始显示, 而非吸附到底部
        scrollable_->setStickToBottom(false);
        Add(scrollable_);
    }

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

    /// 测试辅助: 最近一次渲染各消息折叠头的可见命中区域
    /// (与 msgs 索引对应; 视口外为空 Box; 供测试模拟点击折叠/展开)
    std::vector<ftxui::Box> headerBoxes() const;

private:

    /// itemMessages_ 中"非折叠头子项"的取值 (展开体子项, 不参与点击命中)
    static constexpr size_t kNoMessage = static_cast<size_t>(-1);

    /// 弹窗内容项构建 (Scrollable 渲染回调; 每帧从本帧快照构建)
    /// 同时重建 itemMessages_ (子项下标 -> 消息下标 映射)
    std::vector<ScrollItem> buildItems();

    /// 构建单条消息的折叠头 (含 +/- 标记与单行预览)
    ftxui::Element buildMessageHeader(
        const utilxx_base::Json& m,
        bool                     expanded,
        const ftxui::Color&      roleColor
    );

    /// 构建单条消息的展开体: 完整原始 JSON (dump(2) 美化多行)
    ftxui::Element buildMessageBody(const utilxx_base::Json& m);

    /// 屏幕坐标命中的折叠头所属消息下标 (未命中返回 [kNoMessage])
    /// 命中区域取 [Scrollable::visibleBoxes] 的上一帧可见子项区域 ——
    /// 视口外的子项区域为空, 因此不会命中到看不见的消息
    size_t headerMessageAt(int x, int y) const;

    /// 自上而下第一个可见折叠头所属消息下标 (无可见折叠头返回 [kNoMessage])
    size_t firstVisibleHeaderMessage() const;

    /// 鼠标左键释放时切换命中的消息行折叠状态
    bool handleHeaderClick(const ftxui::Mouse& mouse);

    /// 切换指定消息的折叠状态 (UI 线程独占; 索引按当前快照消息数组)
    void toggleExpanded(size_t index);

    TUICtx&                     ctx_;
    std::shared_ptr<Scrollable> scrollable_;
    std::function<void()>       onClose_;

    /// 已展开的消息索引集合 (UI 线程独占; 默认全部折叠)
    std::set<size_t> expandedSet_;

    /// 本帧子项下标 -> 消息下标 映射 (由 buildItems 重建, 与 Scrollable
    /// 上一帧的 visibleBoxes 一一对应)
    /// - 折叠头子项: 值为消息下标; 展开体子项: [kNoMessage]
    /// - 不使用子项元素自带的 reflect 命中框: Scrollable 测量子项高度时
    ///   会用"测量用临时大框" (局部坐标) 调用 SetBox, 未被定位的视口外
    ///   子项残留该框, 点击时会先于真实子项命中 (点错消息)
    std::vector<size_t> itemMessages_;
};

/// Mermaid 状态图弹窗 (通用 open_overlay MERMAID 驱动; 标题可自定义)
///
/// 显示单条 mermaid (stateDiagram-v2) 的 ASCII 状态图:
/// - 全宽渲染, 内部 Scrollable 滚动 (滚轮 / Up/Down)
/// - 节点按 id 状态后缀着色 (_in_progress/_completed/_failed/_pending)
/// - 支持动态 mermaid 字符串 (构造时传入), 弹窗打开期间不变
/// 交互: 滚轮 / Up/Down 滚动, Esc 关闭
class MermaidDiagramOverlay : public ftxui::ComponentBase {
public:

    explicit MermaidDiagramOverlay(TUICtx& ctx, std::string mermaid, std::string title = {});

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    std::vector<ScrollItem> buildItems();

    TUICtx&                     ctx_;
    std::string                 mermaid_;
    std::string                 title_;
    std::shared_ptr<Scrollable> scrollable_;
    std::function<void()>       onClose_;

    /// 状态图渲染缓存: 仅当 mermaid/终端宽度/主题任一变化时重新解析重建 Element
    std::string                   cachedMermaid_;
    int                           cachedMaxW_ = 0;
    std::string                   cachedThemeName_;
    markdown::MermaidStateDiagram cachedDiagram_;
    ftxui::Element                cachedElement_;
};

/// 加载失败组件列表弹窗 (Info 侧边栏 Append "Failed" 组 [view] 按钮触发)
///
/// 列出启动阶段加载失败的组件 (appendComponents 中 success=false 项):
/// - 每项两行: "[类型] 名称" / 错误信息 (自动换行, 减淡色)
/// - 交互: 滚轮 / Up/Down 滚动, Esc 关闭
class FailedComponentsOverlay : public ftxui::ComponentBase {
public:

    explicit FailedComponentsOverlay(TUICtx& ctx);

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    std::vector<ScrollItem> buildItems();

    TUICtx&                     ctx_;
    std::shared_ptr<Scrollable> scrollable_;
    std::function<void()>       onClose_;
};

/// Logs 侧边栏 Menu 菜单弹窗组件
/// - 提供 LLM Context, Summy Context, Clear Logs 三个操作按钮
/// - 支持键盘 Up/Down 选择, Enter 确认, Esc 关闭, 以及鼠标点击
/// - 条目表与交互由 [UiActionList] 统一实现
class LogMenuOverlay : public ftxui::ComponentBase {
public:

    explicit LogMenuOverlay(TUICtx& ctx);

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    void onLlmContext(std::function<void()> fn) {
        onLlmContext_ = std::move(fn);
    }

    void onSummyContext(std::function<void()> fn) {
        onSummyContext_ = std::move(fn);
    }

    void onClearLogs(std::function<void()> fn) {
        onClearLogs_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

    /// 测试辅助: 选中项下标 (-1 = 无)
    int selectedIndex() const {
        return list_.selectedIndex();
    }

private:

    /// 重建条目表 (三项操作; 文案按当前界面语言)
    void buildItems();

    TUICtx&       ctx_;
    UiActionList  list_;
    UiHitMap      hits_;
    UiActionStyle style_;

    std::function<void()> onClose_;
    std::function<void()> onLlmContext_;
    std::function<void()> onSummyContext_;
    std::function<void()> onClearLogs_;
};

/// 通用 overlay 的尺寸与外观选项 (`AgentxxOverlaySpec.extra_json`, 数据层扩展)
///
/// 示例: `{"size":"large","height_frac":0.6,"footer":false,"stack":true}`
/// - `size`: auto / compact / normal (缺省) / large / full
/// - `width_frac` / `height_frac`: 显式比例 (0~1; 与 `size` 同时给出时以它为准)
/// - `footer`: 是否显示底栏提示 (缺省显示)
/// - `scroll`: 内容是否需要滚动; `false` 时隐藏滚动提示 (与 `footer:false` 等效)
/// - `stack`: 已有 overlay 打开时是否保留现有 overlay (缺省替换, 见 openOverlay)
/// - 未知键忽略 (老宿主同样忽略), 非法值回退缺省
struct OverlayOptions {
    std::string size       = "normal";
    double      widthFrac  = 0.0; ///< 0 = 用 `size` 预设
    double      heightFrac = 0.0; ///< 0 = 用 `size` 预设
    bool        footer     = true;
    bool        stack      = false;

    /// 解析 `extra_json` (空/非法输入返回缺省选项)
    static OverlayOptions fromJson(std::string_view extraJson);

    /// 合成宽高比例 (0~1), 供外框按终端尺寸换算像素宽高
    void resolveFractions(double& wFrac, double& hFrac) const;
};

/// 通用文本 overlay (open_overlay TEXT 驱动; payload=原文, extra={"markdown":bool})
///
/// - markdown=true (缺省): 按 markdown 主题渲染 (mermaid 围栏渲染为状态图)
/// - markdown=false: 纯段落渲染
/// - Scrollable + Esc 关 + overlay.scrollHint 底栏; 宽 3/5、高 4/5 双约束防塌缩
class TextOverlay : public ftxui::ComponentBase {
public:

    explicit TextOverlay(TUICtx& ctx, std::string title, std::string content, bool markdown = true);

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    /// 设置尺寸与外观选项 (open_overlay 的 extra_json; 见 [OverlayOptions])
    void setOptions(const OverlayOptions& options) {
        options_ = options;
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    std::vector<ScrollItem> buildItems();

    TUICtx&                     ctx_;
    std::string                 title_;
    std::string                 content_;
    bool                        markdown_;
    std::shared_ptr<Scrollable> scrollable_;
    std::function<void()>       onClose_;
    /// 尺寸与外观选项 (open_overlay 的 extra_json; 见 [OverlayOptions])
    OverlayOptions options_;

    /// markdown 渲染缓存 (DomBuilder 生命周期与 Element 绑定, 见 attachments)
    std::vector<std::shared_ptr<void>> cachedAttachments_;
    std::string                        cachedContent_;
    int                                cachedMaxW_ = 0;
    std::string                        cachedThemeName_;
    bool                               cachedMarkdown_ = true;
    ftxui::Element                     cachedElement_;
};

/// 通用 diff overlay (open_overlay DIFF 驱动; payload={path,old_str,new_str})
///
/// - 渲染复用 message diff 逻辑 (computeLineDiff + side-by-side/统一样式,
///   见 plugin_ui_items renderPluginDiff)
/// - Scrollable + Esc 关 + overlay.scrollHint 底栏; 宽 4/5、高 4/5 双约束
class DiffOverlay : public ftxui::ComponentBase {
public:

    explicit DiffOverlay(
        TUICtx&     ctx,
        std::string title,
        std::string path,
        std::string oldStr,
        std::string newStr
    );

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    /// 设置尺寸与外观选项 (open_overlay 的 extra_json; 见 [OverlayOptions])
    void setOptions(const OverlayOptions& options) {
        options_ = options;
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    std::vector<ScrollItem> buildItems();

    TUICtx&                     ctx_;
    std::string                 title_;
    std::string                 path_;
    std::string                 oldStr_;
    std::string                 newStr_;
    std::shared_ptr<Scrollable> scrollable_;
    std::function<void()>       onClose_;

    /// 尺寸与外观选项 (open_overlay 的 extra_json; 见 [OverlayOptions])
    OverlayOptions options_;
};

/// 通用自定义 overlay (open_overlay CUSTOM 驱动; payload={"items":[...]})
///
/// - items schema 同 panel/items (共享组件层全部 kind, 见 ui_components.h),
///   button 同样走 action_id + 通用派发 (owner 固定 "__overlay", 被 fallback 接住)
/// - overlay 内命中: 滚动容器把点击映射到子项 + 局部坐标, 再按子项登记的
///   可命中区域处理 (动作派发 / 折叠标题切换); 视口外内容不占点击区域
/// - Scrollable + Esc 关 + overlay.scrollHint 底栏; 宽 3/5、高 4/5 双约束
class CustomOverlay : public ftxui::ComponentBase {
public:

    explicit CustomOverlay(
        TUICtx&           ctx,
        std::string       title,
        utilxx_base::Json items,
        std::string       ownerPlugin
    );

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    /// 设置尺寸与外观选项 (open_overlay 的 extra_json; 见 [OverlayOptions])
    void setOptions(const OverlayOptions& options) {
        options_ = options;
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    TUICtx&           ctx_;
    std::string       title_;
    utilxx_base::Json items_;
    std::string       ownerPlugin_;
    /// ownerPlugin_ 在最近一次渲染快照中的实例代次 (点击复查; 见 dispatchAction)
    uint64_t                    ownerGeneration_ = 0;
    std::shared_ptr<Scrollable> scrollable_;
    std::function<void()>       onClose_;

    /// 折叠分组的展开状态 (键 = 组件 id; 宿主维护, 点击标题切换)
    std::map<std::string, bool, std::less<>> collapseStates_;

    /// 尺寸与外观选项 (open_overlay 的 extra_json; 见 [OverlayOptions])
    OverlayOptions options_;

    /// 表单状态与当前组件树 (控件值/勾选/选中/焦点; 由宿主维护, 见 ui_components.h)
    agentxx::client::UiFormState   form_;
    std::vector<agentxx::ui::Item> formItems_;

    /// 提交表单 (校验 → 组装 `{"values":{...}}` → 动作通道回传 __submit)
    void submitForm();

    /// markdown 渲染器生命周期 (Element 内部指向它; 随内容重建)
    std::vector<std::unique_ptr<markdown::DomBuilder>> mdBuilders_;
};

/// 创建通用覆盖层弹窗工厂函数 (支持 Mermaid / Text / Diff / Custom 弹窗)
std::shared_ptr<ftxui::ComponentBase> createUniversalOverlay(
    TUICtx&               ctx,
    int                   type,
    std::string_view      title,
    std::string_view      payload,
    std::string_view      extraJson,
    std::string_view      ownerPlugin,
    std::function<void()> onClose
);

/// 多模态文件选择弹窗 (输入框右侧 [ @ ︎] 按钮触发)
///
/// - 初始化时接收当前模型的 `ModelCapabilityInfo`, 按支持的媒体类型动态
///   过滤目录中的文件 (不支持的类型灰显且不可选, 非媒体文件不展示)
/// - 目录导航: ↑/↓ 选择, Enter 进入子目录或确认选中文件, Esc 关闭
/// - 确认选中文件后调用 onSelectFile 回调, 外部完成预检、Base64 编码并挂载到托盘
/// - 命中区域经 UiHitMap 登记: 仅当前 tab 真正渲染出来的按钮/条目才会命中
///   (跨设备标签页按钮仅跨设备时渲染, 因此同设备下不会被误点)
class FilePickerOverlay : public ftxui::ComponentBase {
public:

    FilePickerOverlay(
        TUICtx&                             ctx,
        agentxx::agent::ModelCapabilityInfo capability,
        std::string                         initialDir = ""
    );

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    /// 文件选中回调 (参数: 文件绝对路径)
    void onSelectFile(std::function<void(std::string)> fn) {
        onSelectFile_ = std::move(fn);
    }

    /// 附件选中回调 (参数: 构造完毕的 MediaAttachment 对象, 支持本地与服务端附件)
    void onSelectAttachment(std::function<void(agentxx::agent::MediaAttachment)> fn) {
        onSelectAttachment_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

    /// 测试辅助: 上一帧第 index 目录条目的命中区域 (未渲染时为空区域)
    ftxui::Box itemBox(size_t index) const;

    /// 测试辅助: 上一帧 "本地" / "服务端" 标签页按钮命中区域 (未渲染时为空区域)
    ftxui::Box tabButtonBox(bool serverTab) const;

private:

    enum class PickerTab : uint8_t {
        Local,
        Server,
    };

    /// 目录条目
    struct DirEntry {
        std::string               name;     ///< 显示名 (文件名, 不含图标)
        std::string               fullPath; ///< 绝对路径
        bool                      isDir     = false;
        bool                      supported = true; ///< 当前模型是否支持该文件类型
        uint64_t                  sizeBytes = 0;
        agentxx::agent::MediaType mediaType = agentxx::agent::MediaType::Image;
    };

    struct TabState {
        std::string           currentDir;
        std::vector<DirEntry> entries;
        int                   selectedIndex = 0;
        bool                  loading       = false;
        std::string           error;
    };

    void                      navigateToLocal(std::string dirPath);
    void                      navigateToServer(std::string dirPath);
    void                      switchTab(PickerTab tab);
    void                      confirmSelection();
    bool                      isMediaFile(const std::string& ext) const;
    bool                      isSupportedMedia(const std::string& ext) const;
    agentxx::agent::MediaType guessMediaType(const std::string& ext) const;

    /// 当前生效的 tab 状态
    TabState&       currentTab();
    const TabState& currentTab() const;

    /// 条目命中 id (含 tab 归属与条目下标, 避免两个 tab 的条目 id 冲突)
    static std::string itemHitId(PickerTab tab, size_t index);

    /// 从条目命中 id 解析条目下标 (不属于该 tab 或格式错误返回 -1)
    static int itemIndexOfHitId(PickerTab tab, std::string_view hitId);

    /// 取指定命中 id 的屏幕区域 (未渲染时为空区域)
    ftxui::Box hitBox(std::string_view id) const;

    TUICtx&                                              ctx_;
    agentxx::agent::ModelCapabilityInfo                  capability_;
    PickerTab                                            activeTab_ = PickerTab::Local;
    TabState                                             localTab_;
    TabState                                             serverTab_;
    std::function<void()>                                onClose_;
    std::function<void(std::string)>                     onSelectFile_;
    std::function<void(agentxx::agent::MediaAttachment)> onSelectAttachment_;
    std::set<std::string>                                allowedExtensions_;

    /// 命中区域登记表 (每帧重建; 未渲染的标签页按钮/条目不会命中)
    UiHitMap hits_;
    /// "本地" / "服务端" 标签页按钮命中 id
    static constexpr std::string_view kLocalTabHitId  = "tab/local";
    static constexpr std::string_view kServerTabHitId = "tab/server";
};

} // namespace agentxx::client
