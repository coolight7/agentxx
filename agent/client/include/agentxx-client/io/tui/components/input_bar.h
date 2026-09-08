#pragma once

#include "agentxx-client/io/tui/components/spinner.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx/agent/conversation_types.h"
#include "ftxui/component/component.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/dom/elements.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/// 输入栏组件: 指示器 + 多行文本输入 + 附件挂载托盘 + 多模态选择按钮
///
/// 事件处理:
/// - Alt+Enter: 插入换行 (经 Input 组件插入逻辑, 光标随之后移)
/// - Enter: 发送 (经 onSend 回调, 携带输入文本与待发附件列表)
/// - Ctrl+L: 清空输入与附件
/// - 鼠标点击右侧 [+ 📎︎︎]: 触发打开模态文件选择弹窗 (不绑定键盘热键)
/// - 鼠标点击附件 [✕]: 从待发附件托盘移除对应附件
/// - 括号粘贴 (bracketed paste): 终端启用 \x1B[?2004h 后, 粘贴内容以
///   \x1B[200~ ... \x1B[201~ 包裹到达, 本组件拦截并整体插入光标处,
///   支持多行粘贴 (粘贴的换行不会触发发送)
///
/// 发送逻辑由外部 (TUIClientAgentIO) 通过 onSend 回调实现,
/// 本组件仅负责 UI 交互与文本、附件管理。
class InputComponent : public ftxui::ComponentBase {
public:

    struct Config {
        /// 发送回调: 参数为去除首尾换行后的文本和待发附件; 返回 true
        /// 表示发送成功并清空输入框与托盘, 返回 false 表示发送未成功保留输入框内容与托盘
        std::function<bool(std::string, std::vector<agentxx::agent::MediaAttachment>)> onSend;
        /// 是否处于中断等待输入模式 (影响指示器显示)
        std::function<bool()> isAwaitingInterrupt;
        /// 是否正在流式输出 (影响指示器显示)
        std::function<bool()> isStreaming;
        /// 当前活动模型是否支持多模态输入 (决定是否展示 [+ 📎︎︎] 按钮)
        std::function<bool()> canAttach;
        /// 点击 [+ 📎︎︎] 按钮触发打开文件选择弹窗
        std::function<void()> onOpenAttachPicker;
    };

    InputComponent(TUICtx& ctx, Config config);

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

    /// 刷新界面语言文本 (输入框占位符; 语言切换后由外部调用, 立即生效)
    void refreshLanguage() {
        placeholderText_ = std::string(TuiI18n::instance().t("input.placeholder"));
        ctx_.postRedraw();
    }

    /// 清空输入框与待发附件
    void clear() {
        inputText_.clear();
        attachments_.clear();
        // 同步重置粘贴状态, 避免残留粘贴缓冲区
        inPaste_ = false;
        pasteBuffer_.clear();
    }

    const std::string& inputText() const {
        return inputText_;
    }

    /// 添加附件到待发托盘
    void addAttachment(agentxx::agent::MediaAttachment att) {
        attachments_.push_back(std::move(att));
        ctx_.postRedraw();
    }

    /// 获取当前挂载的附件
    const std::vector<agentxx::agent::MediaAttachment>& attachments() const {
        return attachments_;
    }

    /// 清空待发附件
    void clearAttachments() {
        attachments_.clear();
        ctx_.postRedraw();
    }

private:

    /// 括号粘贴起始/结束标记 (终端启用 \x1B[?2004h 后包裹粘贴内容)
    static constexpr std::string_view kPasteStartSeq = "\x1B[200~";
    static constexpr std::string_view kPasteEndSeq   = "\x1B[201~";
    /// 粘贴中断安全阀: 粘贴期间事件间隔超过该时长则放弃本次粘贴,
    /// 防止结束标记丢失时后续输入被吞入粘贴缓冲区
    static constexpr auto kPasteTimeout = std::chrono::milliseconds(2000);

    TUICtx&                                      ctx_;
    Config                                       config_;
    std::string                                  inputText_;
    ftxui::Component                             input_;
    std::vector<agentxx::agent::MediaAttachment> attachments_;

    /// 输入框右侧 [+ 📎︎︎] 按钮点击命中区域
    ftxui::Box attachButtonBox_;
    /// 托盘各附件删除按钮 [✕] 点击命中区域
    std::vector<ftxui::Box> attachmentDeleteBoxes_;

    /// 输入框占位符 (绑定到 Input 的 placeholder 引用: Input 渲染时实时读取,
    /// 语言切换后刷新本成员即生效, 无需重建组件)
    std::string placeholderText_;

    /// 会话运行加载动画 (流式输出指示, 替代原先静态 "~" 标记; 可复用组件)
    /// - 注意必须经 Add() 注册为本组件子项: FTXUI 的 OnAnimation 由根组件
    ///   沿组件树逐级转发给已注册的子组件, 未入树的组件收不到动画回调,
    ///   帧循环无法推进 (渲染仅是手动调用 Render(), 不建立父子关系)
    std::shared_ptr<SpinnerComponent> spinner_;

    /// 括号粘贴状态: 是否处于粘贴内容接收中
    bool inPaste_ = false;
    /// 粘贴内容累积缓冲区 (结束标记处一次性插入)
    std::string pasteBuffer_;
    /// 上一次粘贴事件时间 (用于粘贴中断超时检测)
    std::chrono::steady_clock::time_point lastPasteEventTime_;
};
