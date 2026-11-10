/// agentxx 广播频道 id 清单 (主程序声明的应用频道)
///
/// 频道的职责划分 (见 docs/zh-cn/design/events.md 与 plugins.md):
/// - **功能扩展点**频道 (`agentxx.agent.point.*`): 见 `agentxx_point_ids.h`;
/// - **生命周期通知** (本头): "某一刻发生了什么", 没有等待者、没有值语义;
/// - **事件中枢的值事件** (`agentxx.agent.event.*` / `agentxx.agent.async.done`):
///   由中枢自己发布, 名字规则在 `agentxx/event/naming.h`, 不在本头。
///
/// 插件自建频道用短名 (`declare_broadcast` 的 `id` 不给前缀), 主程序会补成
/// `plugin.<插件 id>.<短名>`; 本头的名字是主程序声明并发布的, 插件只能订阅。
#ifndef AGENTXX_PLUGIN_API_AGENTXX_BROADCAST_IDS_H
#define AGENTXX_PLUGIN_API_AGENTXX_BROADCAST_IDS_H

#include <string_view>

namespace agentxx {
namespace plugin {
namespace broadcasts {

/// 一轮会话开始 (主程序发布, 无缓存): SDK 的基础类订阅它清理会话级的取消标记
inline constexpr std::string_view AgentRoundStart = "agentxx.agent.round.start";
/// 后台任务结束 (主程序发布, `cache: latest`): 载荷里用 `asyncId` 区分是哪一条
inline constexpr std::string_view AgentAsyncDone = "agentxx.agent.async.done";
/// 客户端接入 (服务端端点发布, 无缓存): chatbot / planning 一类插件用它判断"有人在看"
inline constexpr std::string_view AgentClientAttached = "agentxx.agent.host.client_attached";

/// 客户端就绪 (client 侧发布, `cache: latest`)
inline constexpr std::string_view ClientReady = "agentxx.client.ready";
/// 客户端连接状态变化 (`cache: latest`)
inline constexpr std::string_view ClientConnectionState = "agentxx.client.connection_state";
/// 客户端切换会话 (`cache: latest`)
inline constexpr std::string_view ClientSessionSwitch = "agentxx.client.session_switch";
/// 客户端布局 / 区域尺寸 (`cache: latest`)
inline constexpr std::string_view ClientUiLayout = "agentxx.client.ui_layout";
/// 用户输入 (client 侧发布, 无缓存)
inline constexpr std::string_view ClientUserInput = "agentxx.client.user_input";
/// 流式增量 (client 侧发布, 无缓存)
inline constexpr std::string_view ClientDelta = "agentxx.client.delta";
/// 一轮结束 (client 侧发布, 无缓存)
inline constexpr std::string_view ClientTurnEnd = "agentxx.client.turn_end";

/// 跨端插件数据的频道前缀: 完整名字是 `agentxx.client.plugin.<插件名>.<事件>`
///
/// 两端都可发布 (`publish: anyone`), 首次发布前由主程序按需声明 (幂等)。
inline constexpr std::string_view ClientPluginPrefix = "agentxx.client.plugin.";

} // namespace broadcasts
} // namespace plugin
} // namespace agentxx

#endif /* AGENTXX_PLUGIN_API_AGENTXX_BROADCAST_IDS_H */
