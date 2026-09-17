#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/ui_hit.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include <functional>
#include <string_view>

namespace agentxx::client {

/// 底部状态栏组件: 左侧模型名 + 上下文占用, 右侧会话选择 + 设置快捷键提示
/// 展示组件 + 鼠标点击处理; 每帧重建渲染内容 (上下文 token 数实时变化)
///
/// 可点击区域与动作:
/// - 模型区域 → [Config::onModelClick] (打开模型选择弹窗, 同 F2)
/// - [F3] Sessions → [Config::onSessionsClick] (打开会话选择弹窗, 同 F3)
/// - [F4] Settings → [Config::onSettingsClick] (打开设置弹窗, 同 F4)
///
/// 命中检测经 [agentxx::client::UiHitMap]: 渲染时登记可点元素, 帧首清空 ——
/// 无需为每个按钮维护 Box 成员, 也不用在外部事件处理里按坐标逐个判断
/// (未渲染出来的按钮不会命中)。本组件需加入组件树 (事件路由) 才能收到事件。
class StatusBarComponent : public ftxui::ComponentBase {
public:

    /// 点击动作 (均为 UI 线程回调)
    struct Config {
        std::function<void()> onModelClick;    ///< 点击模型区域
        std::function<void()> onSessionsClick; ///< 点击 [F3] Sessions
        std::function<void()> onSettingsClick; ///< 点击 [F4] Settings
    };

    explicit StatusBarComponent(TUICtx& ctx, Config config = {}) :
        ctx_(ctx),
        config_(std::move(config)) {}

    ftxui::Element OnRender() override;
    bool           OnEvent(ftxui::Event event) override;

    /// 测试辅助: 指定命中项的屏幕区域 (未渲染时为空区域)
    ftxui::Box hitBox(std::string_view id) const;

    /// 命中 id (供测试与事件处理引用)
    static constexpr std::string_view kModelHitId    = "status/model";
    static constexpr std::string_view kSessionsHitId = "status/sessions";
    static constexpr std::string_view kSettingsHitId = "status/settings";

private:

    TUICtx&                   ctx_;
    Config                    config_;
    agentxx::client::UiHitMap hits_;
};

} // namespace agentxx::client
