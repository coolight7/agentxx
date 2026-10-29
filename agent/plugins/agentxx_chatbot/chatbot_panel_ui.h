#pragma once

/// 侧边栏「机器人」栏的渲染 (快照 → 界面描述层组件)
///
/// 只做组件拼装, 不碰主程序接口: client 插件入口 (chatbot_client.cpp) 解析出
/// [PanelSnapshot] 后调 [panelItemsOf], 再把结果提交给 `update_panel`。
/// 因此本文件可以编进测试直接断言组件树 (见 `chatbot_core` 模块)。
#include "chatbot_panel.h"

#include <pluginxx/ui/item.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace agentxx {
namespace chatbot {

/// 加载中动画的帧序列
///
/// 与 TUI 消息列表的"加载中"点阵**同一套字符** (braille 旋转点,
/// 见 `agent/client/.../components/spinner.h` 的 `kDefaultFrames`), 这样面板里的
/// 加载态和消息里的加载态看起来是同一个东西。
const std::array<std::string_view, 10>& panelSpinnerFrames();

/// 按计数取当前帧 (`tick` 由 agent 侧的刷新节拍递增)
std::string_view panelSpinnerFrame(uint32_t tick);

/// 面板组件树
///
/// 内容顺序: 会话列表 (主体) → 人设信息 → 账号状态 / 收发计数。
/// 没有收到过任何快照 (agent 侧插件没加载 / 还没连上) 时给一行提示。
std::vector<pluginxx::ui::Item> panelItemsOf(const PanelSnapshot& snapshot);

} // namespace chatbot
} // namespace agentxx
