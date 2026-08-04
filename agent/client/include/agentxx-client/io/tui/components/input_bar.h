#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "ftxui/component/component.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/dom/elements.hpp"
#include <functional>
#include <string>

/// 输入栏组件: 指示器 + 多行文本输入
///
/// 事件处理:
/// - Alt+Enter: 插入换行
/// - Enter: 发送 (经 onSend 回调)
/// - Ctrl+C: 清空输入或退出 (经 onCtrlC 回调)
///
/// 发送逻辑由外部 (TUIClientAgentIO) 通过 onSend 回调实现,
/// 本组件仅负责 UI 交互与文本管理。
class InputComponent : public ftxui::ComponentBase {
public:

    struct Config {
        /// 发送回调: 参数为去除首尾换行后的文本
        std::function<void(std::string)> onSend;
        /// 是否处于中断等待输入模式 (影响指示器显示)
        std::function<bool()> isAwaitingInterrupt;
        /// 是否正在流式输出 (影响指示器显示)
        std::function<bool()> isStreaming;
    };

    InputComponent(TUICtx& ctx, Config config);

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

    /// 清空输入框
    void clear() {
        inputText_.clear();
    }

    const std::string& inputText() const {
        return inputText_;
    }

private:

    TUICtx&          ctx_;
    Config           config_;
    std::string      inputText_;
    ftxui::Component input_;
};
