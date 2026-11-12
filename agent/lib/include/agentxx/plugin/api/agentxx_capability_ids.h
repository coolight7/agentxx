/// agentxx 应用能力 id 清单 (主程序声明的能力, 插件只提供实现行)
///
/// 能力分两类, 声明方不同:
/// - **应用声明的能力** (本头): id 落在 `<应用名>.` 段下, 由主程序在启动阶段声明
///   (几个实现、结果怎么合并、开放哪些阶段)。插件不能自己声明这类 id, 只能给它登记
///   实现行; 没声明就登记会被主程序拒绝 (`... is not declared by the application`)。
///   这样"有哪些官方能力"只在一处说了算, 插件之间的调用点不必各自记名字。
/// - **插件自己的能力**: id 写短名, 主程序补成 `plugin.<插件 id>.<短名>`,
///   声明与实现都由插件自己给。
///
/// 功能埋点 (`agentxx.agent.point.*`) 也是一批应用声明的能力, 但它们的清单在
/// `agentxx_point_ids.h` (那边同时还要声明同名广播频道), 不在这里。
#ifndef AGENTXX_PLUGIN_API_AGENTXX_CAPABILITY_IDS_H
#define AGENTXX_PLUGIN_API_AGENTXX_CAPABILITY_IDS_H

#include <string_view>

namespace agentxx {
namespace plugin {
namespace capabilities {

/// 当前系统负担的快照 (CPU / 内存 / GPU 占用与显存), 由 `agentxx_system_monitor` 提供
/// - 一个 id 允许多个实现行, 第一个给出结果的生效 (没有谁覆盖谁);
/// - 载荷字段见 docs/zh-cn/design/plugins.md 的"内置能力"段。
inline constexpr std::string_view SystemUsage = "agentxx.system_usage";

/// 全部应用声明的能力 (主程序声明表与测试按它清点)
inline constexpr std::string_view kAppCapabilities[] = {
    SystemUsage,
};

} // namespace capabilities
} // namespace plugin
} // namespace agentxx

#endif /* AGENTXX_PLUGIN_API_AGENTXX_CAPABILITY_IDS_H */
