#pragma once
#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 启动 banner 艺术字 ("AGENT++") 的 "++" 变形动画测试
/// - 帧序列: 每帧 6 行等宽 ("AGENT" 前缀不变, 末帧与原机器人图案一致, 逐帧收敛)
/// - 播放组件: 首次渲染开始播放 / 按累计时长推进 / 到末帧停止 / 等级不足直接展示末帧
/// - 消息列表空状态接入: 无事件也会从大字 "+" 推进到机器人图案
TestResult testBannerArt();

} // namespace test
} // namespace agentxx
