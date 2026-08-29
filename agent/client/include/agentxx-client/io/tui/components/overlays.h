#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/scrollable.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include <functional>
#include <markdown/state_diagram.hpp>
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

    TUICtx& ctx_;
    /// 条目索引: 0 = 主题, 1 = 动画等级, 2 = 日志等级, 3 = 末尾思考模式
    /// Enter/鼠标点击索引时循环切换对应设置
    static constexpr int  kItemCount     = 4;
    int                   selectedIndex_ = 0;
    std::function<void()> onClose_;
    std::function<void()> onThemeChange_;
    std::function<void()> onLogLevelChange_;

    ftxui::Box themeBox_;        // 主题点击区域
    ftxui::Box animLevelBox_;    // 动画等级点击区域
    ftxui::Box logLevelBox_;     // 日志等级点击区域
    ftxui::Box tailThinkingBox_; // 末尾思考展示模式点击区域
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
class ContextOverlay : public ftxui::ComponentBase {
public:

    explicit ContextOverlay(TUICtx& ctx) :
        ctx_(ctx) {}

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    TUICtx&               ctx_;
    int                   scrollOffset_ = 0;
    std::function<void()> onClose_;
};

/// Mermaid 状态图弹窗 (Plan Graph 按钮触发)
///
/// 显示单条 roadmap (Mermaid stateDiagram-v2) 的 ASCII 状态图:
/// - 全宽渲染, 内部 Scrollable 滚动 (滚轮 / Up/Down)
/// - 节点按 id 状态后缀着色 (_in_progress/_completed/_failed/_pending)
/// - 支持动态 mermaid 字符串 (构造时传入), 弹窗打开期间不变
/// 交互: 滚轮 / Up/Down 滚动, Esc 关闭
class MermaidDiagramOverlay : public ftxui::ComponentBase {
public:

    explicit MermaidDiagramOverlay(TUICtx& ctx, std::string mermaid);

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    std::vector<ScrollItem> buildItems();

    TUICtx&                     ctx_;
    std::string                 mermaid_;
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
