#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"

/// 底部状态栏组件: 左侧模型名 + 上下文占用, 右侧会话选择 + 设置快捷键提示
/// 展示组件 + 鼠标悬浮状态维护; 每帧重建渲染内容 (上下文 token 数实时变化)
/// 暴露可点击区域 (modelBox/sessionBox/settingsBox), 供全局事件处理时做命中检测:
/// - 点击模型区域 → 打开模型选择弹窗
/// - 点击 Sessions → 打开会话选择弹窗 (F4)
/// - 点击 Settings → 打开设置弹窗
class StatusBarComponent : public ftxui::ComponentBase {
public:

    explicit StatusBarComponent(TUICtx& ctx) :
        ctx_(ctx) {}

    ftxui::Element OnRender() override;

    /// 模型按钮的屏幕命中区域 (渲染时由 reflect 填充)
    const ftxui::Box& modelBox() const {
        return modelBox_;
    }

    /// Sessions 按钮的屏幕命中区域 (渲染时由 reflect 填充)
    const ftxui::Box& sessionBox() const {
        return sessionBox_;
    }

    /// Settings 按钮的屏幕命中区域 (渲染时由 reflect 填充)
    const ftxui::Box& settingsBox() const {
        return settingsBox_;
    }

private:

    TUICtx&    ctx_;
    ftxui::Box modelBox_;
    ftxui::Box sessionBox_;
    ftxui::Box settingsBox_;
};
