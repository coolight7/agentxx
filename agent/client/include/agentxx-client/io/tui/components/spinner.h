#pragma once

#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "ftxui/component/animation.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/dom/elements.hpp"
#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

/// 循环加载动画组件 (Spinner)
///
/// 基于 FTXUI 动画机制实现时间驱动的帧序列动画:
/// - FTXUI 动画流程: 组件调用 ftxui::animation::RequestAnimationFrame() 请求一帧,
///   ScreenInteractive 任务循环计算与上一动画帧的时间差后回调根组件的
///   OnAnimation(Params&) (ComponentBase 默认实现逐级转发给全部子组件),
///   随后标记帧失效触发重绘; 需要持续动画的组件在 OnAnimation 内再次
///   RequestAnimationFrame() 形成帧循环, 无请求时不绘制新帧 (空闲零开销)
/// - 本组件在 运行中 且 动画等级满足 时经渲染入口启动帧循环; OnAnimation 内按
///   累计时长推进帧索引并持续请求下一帧; 运行结束或等级不足时停止请求
///
/// 复用方式: 经 Config 配置 帧序列/帧间隔/最低动画等级/运行状态查询/元素装饰。
/// 注意: 必须经 Add() 注册进组件树 (作为某组件的子项) —— FTXUI 的
/// OnAnimation 由根组件沿组件树转发给已注册的子组件, 未入树的组件收不到
/// 动画回调; 此时仅手动调用 Render() 只会得到静止的首帧。
class SpinnerComponent : public ftxui::ComponentBase {
public:

    /// 默认动画帧序列 (braille 旋转点阵)
    static constexpr std::array<std::string_view, 10> kDefaultFrames = {
        "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
    };

    struct Config {
        /// 动画帧序列 (为空时使用 kDefaultFrames)
        std::vector<std::string> frames;
        /// 帧间隔 (推进一帧所需时长; 实际刷新率受 FTXUI ~30fps 帧率上限约束)
        std::chrono::milliseconds frameInterval{80};
        /// 启用动画所需的最低动画等级 (低于该等级时静态显示首帧)
        AnimationLevel requiredLevel = AnimationLevel::Low;
        /// 运行状态查询 (渲染/动画回调时调用); 为空时以 setRunning 设置的内部状态为准
        std::function<bool()> isActive;
        /// 当前帧元素装饰 (颜色/加粗等由调用方定制), 为空时不装饰
        std::function<ftxui::Element(ftxui::Element)> decorate;
    };

    explicit SpinnerComponent(Config config);

    /// 渲染当前帧 (运行中且动画启用时显示当前动画帧, 否则静态显示首帧);
    /// 运行中首次渲染会启动动画帧循环
    ftxui::Element OnRender() override;

    /// FTXUI 动画步进: 按累计时长推进帧索引, 仍处于运行态时请求下一帧;
    /// 运行结束/动画等级不足时停止请求 (帧循环自然终止)
    void OnAnimation(ftxui::animation::Params& params) override;

    /// 手动设置运行状态 (Config.isActive 为空时生效)
    /// - true : 重置到首帧 (下一次渲染时启动/续接动画帧循环)
    /// - false: 停止运行 (已入队的最后一次回调中发现非运行态即终止帧循环)
    void setRunning(bool running);

private:

    /// 是否处于运行态 (isActive 回调优先于内部状态)
    bool running() const;

    /// 是否允许动画 (动画等级满足要求)
    bool animationEnabled() const;

    Config config_;

    /// 实际使用的帧序列 (构造时回退默认值, 避免渲染期判空)
    std::vector<std::string> frames_;
    /// 帧间隔 (duration<float> 便于与 animation::Duration 直接累加比较)
    std::chrono::duration<float> interval_;

    /// 当前帧索引
    size_t frameIndex_ = 0;
    /// 当前帧内已累计的动画时长 (OnAnimation 步进推进)
    std::chrono::duration<float> elapsed_{};
    /// 期望运行状态 (手动模式; isActive 回调存在时不使用)
    bool running_ = false;
    /// 动画帧循环是否进行中 (true 时 OnAnimation 持续续约);
    /// 与 running_ 分离: 即使早期 RequestAnimationFrame 被丢弃
    /// (如 ScreenInteractive 尚未启动), 后续渲染仍可重新启动循环
    bool animating_ = false;
};
