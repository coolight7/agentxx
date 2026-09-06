#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/scrollable.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include <functional>
#include <markdown/state_diagram.hpp>
#include <neograph/json.h>
#include <set>
#include <string>
#include <vector>

/// 模型选择器弹窗组件 (独立处理键盘导航事件; 每帧重建以反映最新状态)
class ModelSelectorOverlay : public ftxui::ComponentBase {
public:

    explicit ModelSelectorOverlay(TUICtx& ctx) :
        ctx_(ctx) {}

    void setInitialIndex(int idx) {
        selectedIndex_ = idx;
    }

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    void onConfirm(std::function<void(std::string)> fn) {
        onConfirm_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    void confirmSelection();

    TUICtx&                          ctx_;
    int                              selectedIndex_ = 0;
    std::function<void()>            onClose_;
    std::function<void(std::string)> onConfirm_;
};

/// 会话选择弹窗组件 (F4 / 状态栏 [F4] Sessions 按钮)
/// - 列表顶部固定一项 "新会话" (选中确认后创建全新会话, 不切换历史)
/// - 列表项两行: 第一行会话名称 (title, 空时回退 sessionId), 第二行最近活动日期
/// - Up/Down 选择, Enter/鼠标点击切换会话, Esc 关闭
/// - 列表分页加载: 打开弹窗先加载最新一页; 选择项下移接近已加载列表末尾时
///   经 ctx_.requestMoreSessions 自动预取下一页, 尾部显示加载进度提示行
/// - 首页未到达时显示 loading (sessionListLoaded == false)
class SessionSelectorOverlay : public ftxui::ComponentBase {
public:

    explicit SessionSelectorOverlay(TUICtx& ctx) :
        ctx_(ctx) {}

    void onClose(std::function<void()> fn) {
        onClose_ = fn;
    }

    /// 切换会话回调 (参数: 目标 sessionId)
    void onSelect(std::function<void(std::string)> fn) {
        onSelect_ = fn;
    }

    /// 新建会话回调 (选中顶部 "新会话" 项时触发)
    void onNewSession(std::function<void()> fn) {
        onNewSession_ = fn;
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    void confirmSelection();

    TUICtx&                          ctx_;
    int                              selectedIndex_ = 0;
    std::function<void()>            onClose_;
    std::function<void(std::string)> onSelect_;
    std::function<void()>            onNewSession_;
    std::vector<ftxui::Box>          itemBoxes_;
};

/// 设置弹窗组件
/// - 主题切换 (Dark/Light, 单行显示当前值, 点击/Enter 循环切换)
/// - 动画等级 (Disabled/Low/Medium/High/Ultra; 见 TUISettings)
/// - 日志等级 (Trace/Debug/Info/Warn/Error/Out; 见 TUISettings)
/// - 末尾思考展示模式 (Auto Expand/Single Line)
/// - 界面语言 (简体中文 zh-cn / English en-us; 见 TuiI18n 翻译表)
/// - About (打开关于弹窗; 显示版本/路径/插件等信息)
///
/// 交互: Up/Down 选择条目, Enter 应用/切换 (循环切换); 也支持鼠标点击。
/// 所有条目切换后均保持弹窗打开, 便于连续调整; 由 [Esc] 关闭。
class SettingsOverlay : public ftxui::ComponentBase {
public:

    explicit SettingsOverlay(TUICtx& ctx) :
        ctx_(ctx) {}

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

    /// 界面语言变化回调 (供外部刷新静态文本/缓存: 侧边栏标签、输入框
    /// 占位符、消息列表缓存等; 语言立即生效并持久化)
    void onLanguageChange(std::function<void()> fn) {
        onLanguageChange_ = std::move(fn);
    }

    /// 关于弹窗回调 (供外部打开 AboutOverlay)
    void onAbout(std::function<void()> fn) {
        onAbout_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    bool handleMouse(const ftxui::Mouse& mouse);

    /// 循环切换主题: Dark -> Light -> Dark (需要访问 ctx_.theme, 非静态)
    void cycleTheme();
    /// 循环切换动画等级: Disabled -> Low -> Medium -> High -> Ultra -> Disabled
    static void cycleAnimationLevel();
    /// 循环切换日志等级: Trace -> Debug -> Info -> Warn -> Error -> Out -> Trace
    /// (需要访问 onLogLevelChange_, 非静态)
    void cycleLogLevel();
    /// 循环切换末尾思考展示模式: Auto Expand -> Single Line -> Auto Expand
    static void cycleTailThinkingMode();
    /// 循环切换界面语言: 简体中文 <-> English (需要访问 onLanguageChange_, 非静态)
    void cycleLanguage();

    TUICtx& ctx_;
    /// 条目索引: 0 = 主题, 1 = 动画等级, 2 = 日志等级, 3 = 末尾思考模式,
    ///         4 = 界面语言, 5 = About
    /// Enter/鼠标点击索引时循环切换对应设置 (About 打开弹窗)
    static constexpr int  kItemCount     = 6;
    int                   selectedIndex_ = 0;
    std::function<void()> onClose_;
    std::function<void()> onThemeChange_;
    std::function<void()> onLogLevelChange_;
    std::function<void()> onLanguageChange_;
    std::function<void()> onAbout_;

    ftxui::Box themeBox_;        // 主题点击区域
    ftxui::Box animLevelBox_;    // 动画等级点击区域
    ftxui::Box logLevelBox_;     // 日志等级点击区域
    ftxui::Box tailThinkingBox_; // 末尾思考展示模式点击区域
    ftxui::Box langBox_;         // 界面语言点击区域
    ftxui::Box aboutBox_;        // About 点击区域
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

/// 待发送消息队列弹窗组件
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

private:

    bool handleMouse(const ftxui::Mouse& mouse);

    TUICtx&                                 ctx_;
    std::function<void()>                   onClose_;
    std::function<void()>                   onClear_;
    std::function<void(std::string itemId)> onDeleteItem_;

    std::vector<ftxui::Box> itemBoxes_;
    std::vector<ftxui::Box> delBoxes_;
    ftxui::Box              clearBox_;
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

    /// 弹窗内容项构建 (Scrollable 渲染回调; 每帧从本帧快照构建)
    std::vector<ScrollItem> buildItems();

    /// 构建单条消息的折叠头 (含 +/- 标记与单行预览)
    ftxui::Element
        buildMessageHeader(const neograph::json& m, bool expanded, const ftxui::Color& roleColor);

    /// 构建单条消息的展开体: 完整原始 JSON (dump(2) 美化多行)
    ftxui::Element buildMessageBody(const neograph::json& m);

    /// 鼠标左键释放时切换命中的消息行折叠状态
    bool handleHeaderClick(const ftxui::Mouse& mouse);

    /// 切换指定消息的折叠状态 (UI 线程独占; 索引按当前快照消息数组)
    void toggleExpanded(size_t index);

    TUICtx&                     ctx_;
    std::shared_ptr<Scrollable> scrollable_;
    std::function<void()>       onClose_;

    /// 已展开的消息索引集合 (UI 线程独占; 默认全部折叠)
    std::set<size_t> expandedSet_;

    /// 消息 i 的折叠头在 items 中的子项索引 (由 buildItems 每帧重建;
    /// 与 visibleBoxes 对应, 供鼠标命中检测)
    std::vector<size_t> headerItemIndex_;
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
class LogMenuOverlay : public ftxui::ComponentBase {
public:

    explicit LogMenuOverlay(TUICtx& ctx) :
        ctx_(ctx) {}

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

private:

    bool handleMouse(const ftxui::Mouse& mouse);
    void confirmSelection();

    TUICtx&              ctx_;
    int                  selectedIndex_ = 0;
    static constexpr int kItemCount     = 3;

    std::function<void()> onClose_;
    std::function<void()> onLlmContext_;
    std::function<void()> onSummyContext_;
    std::function<void()> onClearLogs_;

    ftxui::Box llmContextBox_;
    ftxui::Box summyContextBox_;
    ftxui::Box clearLogsBox_;
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
};

/// 通用自定义 overlay (open_overlay CUSTOM 驱动; payload={"items":[...]})
///
/// - items schema 同 panel/items (text/progress/badge/separator/button),
///   button 同样走 action_id + 通用派发 (owner 固定 "__overlay", 被 fallback 接住)
/// - overlay 内局部命中: OnRender 收集按钮盒, OnEvent 命中后经
///   ctx_.pluginManager->dispatchAction(ownerPlugin, "__overlay", actionId, args)
/// - Scrollable + Esc 关 + overlay.scrollHint 底栏; 宽 3/5、高 4/5 双约束
class CustomOverlay : public ftxui::ComponentBase {
public:

    explicit CustomOverlay(
        TUICtx&          ctx,
        std::string      title,
        neograph::json   items,
        std::string      ownerPlugin
    );

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    TUICtx&                     ctx_;
    std::string                 title_;
    neograph::json              items_;
    std::string                 ownerPlugin_;
    std::shared_ptr<Scrollable> scrollable_;
    std::function<void()>       onClose_;

    /// overlay 内按钮局部命中盒 (OnRender 收集, OnEvent 命中检测;
    /// 与 CustomOverlayItem 对应, 视口外为空 Box)
    struct OverlayHit {
        ftxui::Box  box;
        std::string actionId;
        std::string argsJson;
    };

    std::vector<OverlayHit> hits_;
};
