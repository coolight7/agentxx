#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"

/// 底部状态栏组件: 左侧模型名 + 上下文占用, 右侧设置快捷键提示
/// 纯展示组件, 不处理事件; 每帧重建 (上下文 token 数实时变化)
/// 暴露 Settings 按钮命中区域 (settingsBox), 供全局事件处理时做点击检测
class StatusBarComponent : public ftxui::ComponentBase {
public:

    explicit StatusBarComponent(TUICtx& ctx) :
        ctx_(ctx) {}

    ftxui::Element OnRender() override;

    /// Settings 按钮的屏幕命中区域 (渲染时由 reflect 填充)
    const ftxui::Box& settingsBox() const {
        return settingsBox_;
    }

private:

    TUICtx&    ctx_;
    ftxui::Box settingsBox_;
};
