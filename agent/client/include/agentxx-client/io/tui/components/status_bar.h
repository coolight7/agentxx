#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/dom/elements.hpp"

/// 底部状态栏组件: 左侧模型名 + 上下文占用, 右侧设置快捷键提示
/// 纯展示组件, 不处理事件; 每帧重建 (上下文 token 数实时变化)
class StatusBarComponent : public ftxui::ComponentBase {
public:

    explicit StatusBarComponent(TUICtx& ctx) :
        ctx_(ctx) {}

    ftxui::Element OnRender() override;

private:

    TUICtx& ctx_;
};
