#include "agentxx-client/io/tui/components/spinner.h"
#include "ftxui/component/animation.hpp"

using namespace ftxui;

SpinnerComponent::SpinnerComponent(Config config) :
    config_(std::move(config)),
    frames_(
        config_.frames.empty()
            ? std::vector<std::string>(kDefaultFrames.begin(), kDefaultFrames.end())
            : std::move(config_.frames)
    ) {
    // 帧间隔下限保护: 非正值视为每次动画回调推进一帧 (实际仍受 FTXUI 帧率上限约束)
    interval_
        = config_.frameInterval > std::chrono::milliseconds(0)
              ? std::chrono::duration_cast<std::chrono::duration<float>>(config_.frameInterval)
              : std::chrono::duration<float>(0.f);
}

bool SpinnerComponent::running() const {
    return config_.isActive ? config_.isActive() : running_;
}

bool SpinnerComponent::animationEnabled() const {
    return TUISettings::instance().isAnimationEnabled(config_.requiredLevel);
}

Element SpinnerComponent::OnRender() {
    const bool active = running();

    if (active && animationEnabled()) {
        // 运行中且允许动画: 首次渲染 (或循环终止后的重启) 时启动帧循环;
        // 即使此前 RequestAnimationFrame 被丢弃 (如屏幕尚未启动), 渲染仍会重新发起
        if (!animating_) {
            elapsed_   = {};
            animating_ = true;
            animation::RequestAnimationFrame();
        }
    } else {
        // 非运行态或动画等级不足: 终止帧循环, 静态显示
        // - 等级不足时冻结在当前帧 (避免跳回首帧造成视觉跳动)
        // - 非运行态时归零, 下次启动从首帧开始
        animating_ = false;
        if (!active) {
            elapsed_ = {};
        }
    }

    const size_t idx     = frameIndex_ % frames_.size();
    Element      element = text(frames_[idx]);
    if (config_.decorate) {
        element = config_.decorate(std::move(element));
    }
    return element;
}

void SpinnerComponent::OnAnimation(animation::Params& params) {
    // 运行结束/动画被禁用: 不再续约, 帧循环自然终止
    if (!running() || !animationEnabled()) {
        animating_ = false;
        return;
    }

    // 按累计时长推进帧序列; 单次回调间隔可能跨越多帧, 循环消化。
    // 上限保护: 极端滞后的回调最多追平 ~1s 对应的帧数后归零剩余时长,
    // 避免异常时间差导致长时间空转。
    elapsed_ += params.duration();
    if (interval_.count() > 0.f) {
        int guard = static_cast<int>(1.f / interval_.count()) + 1;
        while (elapsed_ >= interval_ && guard-- > 0) {
            elapsed_    -= interval_;
            frameIndex_  = (frameIndex_ + 1) % frames_.size();
        }
        if (guard <= 0) {
            elapsed_ = {};
        }
    } else {
        frameIndex_ = (frameIndex_ + 1) % frames_.size();
    }

    // 续约下一帧 (FTXUI 收到请求后才安排下一次 OnAnimation + 重绘)
    animation::RequestAnimationFrame();
}

void SpinnerComponent::setRunning(bool runningState) {
    if (config_.isActive) {
        return; // 外部状态查询模式, 手动设置无效
    }
    if (runningState == running_) {
        return;
    }
    running_    = runningState;
    frameIndex_ = 0;
    elapsed_    = {};
}
