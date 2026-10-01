#pragma once

#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "ftxui/component/animation.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/dom/elements.hpp"
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace agentxx::client {

/// 空消息列表 banner 的艺术字 "AGENT++" 动画帧序列
///
/// - 每帧是一整块 6 行文本 ("AGENT" 与右侧图案同高), 供外层一次性交给 `text()`
///   渲染: 各帧的行宽一致 (行尾空格已补齐), 居中块不会在动画中左右跳动
/// - 首帧: "AGENT" 之后是两个与它同风格的大字 "+"
/// - 末帧: 机器人图案 (即原静态艺术字, 见 [bannerArtFinalFrame])
/// - 中间帧: "+" 由外向内收回, 机器人图案由两只眼睛向外长出
///
/// 帧序列在首次调用时生成一次, 之后只读复用
/// (生成规则见 [banner_art.cpp](/agent/client/src/io/tui/components/banner_art.cpp))
const std::vector<std::string>& bannerArtFrames();

/// 动画末帧 (机器人图案): 动画未启用或播放结束时展示的静态艺术字
const std::string& bannerArtFinalFrame();

/// banner 艺术字动画组件 ("AGENT++" 中的 "++" 由大字 "+" 变形为机器人图案)
///
/// 基于 FTXUI 动画机制实现 (与 [SpinnerComponent] 同一套流程):
/// - 首次渲染时开始播放: 在渲染中经 `animation::RequestAnimationFrame()`
///   启动帧循环, 之后每次动画回调按累计时长推进一帧并续约下一帧
/// - 一次性播放: 播放结束停在末帧, 不再重播 (banner 再次出现时展示静态末帧)
/// - 动画等级低于 Config.requiredLevel 时不做动画, 直接渲染末帧
/// - 必须经 `Add()` 注册进组件树 (与 SpinnerComponent 相同: FTXUI 的 OnAnimation
///   由根组件沿组件树转发, 未入树的组件收不到动画回调)
class BannerArtComponent : public ftxui::ComponentBase {
public:

    struct Config {
        /// 每帧展示时长 (实际刷新率受 FTXUI 帧率上限约束)
        std::chrono::milliseconds frameInterval{70};
        /// 启用动画所需的最低动画等级 (低于该等级时直接展示末帧)
        AnimationLevel requiredLevel = AnimationLevel::High;
    };

    /// 默认配置构造 (等价于 `BannerArtComponent(Config{})`)
    /// - 不写成 `Config config = Config{}` 的默认实参: Config 的默认成员初始化器
    ///   要等外层类完整后才能求值, 类内默认实参里用它会编译不过
    BannerArtComponent();
    explicit BannerArtComponent(Config config);

    /// 渲染当前帧 (6 行艺术字; 颜色/居中由调用方装饰)
    ftxui::Element OnRender() override;

    /// FTXUI 动画步进: 按累计时长推进帧; 播放结束后停止请求下一帧
    void OnAnimation(ftxui::animation::Params& params) override;

    /// 是否正在播放
    /// - true 时 banner 元素不可跨帧缓存: 缓存的旧元素是不会推进的静止帧
    bool animating() const;

    /// 测试辅助: 当前帧序号 (0 = 首帧 "+")
    size_t frameIndex() const {
        return frame_;
    }

private:

    Config config_;

    /// 帧间隔 (duration<float> 便于与 animation::Duration 直接累加比较)
    std::chrono::duration<float> interval_{};
    /// 当前帧内已累计的动画时长 (动画回调步进推进)
    std::chrono::duration<float> elapsed_{};
    /// 当前帧序号
    size_t frame_ = 0;
    /// 播放已开始 (首次渲染时置位; 未开始时不推进也不续约)
    bool started_ = false;
    /// 播放已结束或动画被禁用 (此后恒展示末帧, 帧循环停止)
    bool finished_ = false;
};

} // namespace agentxx::client
