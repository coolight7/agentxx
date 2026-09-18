/// agentxx 插件接口协商 (三层协商的声明/校验基础设施)
///
/// 背景: client 宿主形态多样 (cli/tui/gui/第三方 app), 各自支持的插件接口
/// 不同; server (server-io) 仅有 libagentxx 一个实现, agent 侧接口集 ≡ 核心
/// 契约 + 全部标准接口表 (版本匹配即全集)。协商机制因此是不对称的:
/// 机制通用, 实际起作用的限制集中在 client 侧。
///
/// 三层设计 (详见 [plugins.md](/docs/zh-cn/design/plugins.md) 4.7):
/// 1. 声明层: 插件 plugin.yaml `interfaces.require/optional` 列出依赖的接口
///    名 (稳定字符串: 本项目内置为 "agentxx.agent.*" / "agentxx.client.*",
///    第三方私有接口用 "<vendor>.*"; agent 侧可用接口表 IID 精确声明,
///    如 "agentxx.agent.tools"/"agentxx.agent.prompt");
/// 2. 校验层: 宿主加载前按前缀过滤出本侧声明, 与宿主支持集比对 ——
///    require 未满足 → 跳过加载 (INFO + 原因记录, 非错误: 同一插件目录
///    服务多种宿主是预期情况); 声明了本侧接口却缺对应入口符号 → 明确报错;
/// 3. 决策层: 插件 entry 内经 query_interface 判空 / EVT_READY 与
///    get_client_state 的 "interfaces" 字符串数组自行决定启用哪些功能;
///    展示类子能力经 "agentxx.client.ui" 接口表访问 (表内不支持项为 NULL
///    函数指针)
///
/// 命名规范: "agentxx." 为本项目内置接口的保留命名空间, 第三方插件不得使用
/// (第三方私有接口用 "<vendor>.<name>", 宿主不认识的名称一律视为不支持);
/// 前缀过滤规则见 [sideCaresAboutInterface]。
///
/// api_version 限制 (要求 >= 当前基准版本) 保留且不被本机制替代: 核心结构体
/// 是 C 结构, 老宿主+新插件按新偏移读字段是 UB —— 接口协商只解决"功能子集"维度。
/// COM 风格接口表机制使未来新增能力不再动全局版本号: 每个接口表自带 version
/// 字段独立演进。
///
/// 归属: 接口**清单解析** (plugin.yaml interfaces 段) 属框架内核, 见
/// `pluginxx/host/manifest.h` 的 [pluginxx::PluginManifestInterfaces];
/// **清单目录与协商规则**属宿主领域 (需要知道本宿主实现了哪些表), 因此留在 agentxx。
#ifndef AGENTXX_PLUGIN_INTERFACES_H
#define AGENTXX_PLUGIN_INTERFACES_H

#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "pluginxx/host/manifest.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace plugin {
/// ==================== 接口协商 (三层协商的声明/校验基础设施) ====================
///
/// 背景: client 宿主形态多样 (cli/tui/gui/第三方 app), 各自支持的插件接口
/// 不同; server (server-io) 仅有 libagentxx 一个实现, agent 侧接口集 ≡ 核心
/// 契约 + 全部标准接口表 (版本匹配即全集)。协商机制因此是不对称的:
/// 机制通用, 实际起作用的限制集中在 client 侧。
///
/// 三层设计 (详见 [plugins.md](/docs/zh-cn/design/plugins.md) 4.7):
/// 1. 声明层: 插件 plugin.yaml `interfaces.require/optional` 列出依赖的接口
///    名 (稳定字符串: 本项目内置为 "agentxx.agent.*" / "agentxx.client.*",
///    第三方私有接口用 "<vendor>.*"; agent 侧可用接口表 IID 精确声明,
///    如 "agentxx.agent.tools"/"agentxx.agent.prompt");
/// 2. 校验层: 宿主加载前按前缀过滤出本侧声明, 与宿主支持集比对 ——
///    require 未满足 → 跳过加载 (INFO + 原因记录, 非错误: 同一插件目录
///    服务多种宿主是预期情况); 声明了本侧接口却缺对应入口符号 → 明确报错;
/// 3. 决策层: 插件 entry 内经 query_interface 判空 / EVT_READY 与
///    get_client_state 的 "interfaces" 字符串数组自行决定启用哪些功能;
///    展示类子能力经 "agentxx.client.ui" 接口表访问 (表内不支持项为 NULL
///    函数指针)
///
/// 命名规范: "agentxx." 为本项目内置接口的保留命名空间, 第三方插件不得使用
/// (第三方私有接口用 "<vendor>.<name>", 宿主不认识的名称一律视为不支持);
/// 前缀过滤规则见 sideCaresAboutInterface。
///
/// api_version 限制 (要求 >= 当前基准版本) 保留且不被本机制替代: 核心结构体是 C 结构,
/// 老宿主+新插件按新偏移读字段是 UB —— 接口协商只解决"功能子集"维度。
/// COM 风格接口表机制使未来新增能力不再动全局版本号: 每个接口表自带
/// version 字段独立演进。

/// 已知接口名常量 (稳定契约; 第三方私有接口用 "<vendor>.<name>" 自定义,
/// 宿主不认识的名称一律视为不支持 —— 安全失败)
namespace plugin_interfaces {
/// 元接口: 宿主实现完整核心契约
/// ([plugin_api.h](/agent/lib/include/agentxx/plugin/api/plugin_api.h) v1
/// 核心 vtable) + 标准接口表全集 (libagentxx 即此类宿主); 精简第三方宿主
/// 可仅声明实际实现的下述子集
inline constexpr std::string_view AgentCore = "agentxx.agent.core";
/// COM 风格接口表 IID (与 plugin_api.h 的 AGENTXX_IFACE_AGENT_* 宏一一对应;
/// 清单 interfaces.require 可按插件实际查询的表精确声明)
inline constexpr std::string_view AgentTools        = AGENTXX_PLUGIN_IFACE_AGENT_TOOLS;
inline constexpr std::string_view AgentHooks        = AGENTXX_PLUGIN_IFACE_AGENT_HOOKS;
inline constexpr std::string_view AgentEvents       = AGENTXX_PLUGIN_IFACE_AGENT_EVENTS;
inline constexpr std::string_view AgentCapabilities = AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES;
inline constexpr std::string_view AgentScheduler    = AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER;
inline constexpr std::string_view AgentSession      = AGENTXX_PLUGIN_IFACE_AGENT_SESSION;
inline constexpr std::string_view AgentPlugins      = AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS;
inline constexpr std::string_view AgentConfig       = AGENTXX_PLUGIN_IFACE_AGENT_CONFIG;
inline constexpr std::string_view AgentPrompt       = AGENTXX_PLUGIN_IFACE_AGENT_PROMPT;
inline constexpr std::string_view AgentJson         = AGENTXX_PLUGIN_IFACE_AGENT_JSON;
inline constexpr std::string_view AgentLog          = AGENTXX_PLUGIN_IFACE_AGENT_LOG;
inline constexpr std::string_view AgentResources    = AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES;
inline constexpr std::string_view AgentModel        = AGENTXX_PLUGIN_IFACE_AGENT_MODEL;
inline constexpr std::string_view AgentCancel       = AGENTXX_PLUGIN_IFACE_AGENT_CANCEL;
inline constexpr std::string_view AgentTasks        = AGENTXX_PLUGIN_IFACE_AGENT_TASKS;
/// 通用协程驱动表 (driver ticket / wake 协议; kit 的 PollOneBridge 依赖它)
inline constexpr std::string_view AgentCoroutineRuntime = AGENTXX_PLUGIN_IFACE_COROUTINE_RUNTIME;

/// ---- client 侧: 接口表名 + 细粒度能力名 (映射到 agentxx.client.ui 表的非空成员) ----
/// 展示扩展表整体: 声明它等价于"需要 ui 表内全部子能力", 只有覆盖全部子能力的
/// 宿主才会声明本项 (tui; cli 只有 toast/command 故不声明)。插件应优先按下方的
/// 细粒度能力名精确声明自己实际使用的子能力 —— 声明表整体会让"宿主缺某子能力"
/// 变成"整个插件不可加载/告警"
inline constexpr std::string_view ClientUi          = AGENTXX_IFACE_CLIENT_UI;
inline constexpr std::string_view ClientStatusItem  = "agentxx.client.status_item";
inline constexpr std::string_view ClientPanel       = "agentxx.client.panel";
inline constexpr std::string_view ClientToast       = "agentxx.client.toast";
inline constexpr std::string_view ClientKeybind     = "agentxx.client.keybind";      // 预留
inline constexpr std::string_view ClientPromptModal = "agentxx.client.prompt_modal"; // 预留
/// 工具消息装饰与特化渲染 (ui 表 v2 update_tool_decor / register_tool_renderer;
/// TUI 声明, CLI 无消息渲染面不声明)
inline constexpr std::string_view ClientMsgDecor    = "agentxx.client.msg_decor";
inline constexpr std::string_view ClientInfoSection = "agentxx.client.info_section";
inline constexpr std::string_view ClientCommand     = "agentxx.client.command";
/// 通用交互 (ui 表 v3 bind/unbind + button 拾取派发; TUI 声明, CLI 不声明)
inline constexpr std::string_view ClientAction = "agentxx.client.action";
/// 通用 overlay (ui 表 v3 open/close; TUI 声明, CLI 不声明)
inline constexpr std::string_view ClientOverlay = "agentxx.client.overlay";
} // namespace plugin_interfaces

/// 宿主支持的接口集合 (稳定名字符串; io 线程构建后只读)
using InterfaceSet = std::set<std::string, std::less<>>;
bool sideCaresAboutInterface(std::string_view name, bool agentSide);

/// 单侧接口要求检查结果
struct InterfaceCheckResult {
    bool                     satisfied = true; ///< require 是否全部满足
    std::vector<std::string> missingRequired;  ///< 本侧缺失的必选接口
    std::vector<std::string> missingOptional;  ///< 本侧缺失的可选接口 (仅警告)
};

/// 检查插件声明的接口是否被宿主支持集满足 (按前缀过滤出本侧相关项;
/// agentSide=true 表示以 agent 宿主视角检查, false 为 client 宿主视角)
InterfaceCheckResult checkInterfacesForSide(
    const pluginxx::PluginManifestInterfaces& decl,
    const InterfaceSet&             hostSupported,
    bool                            agentSide
);

/// require 列表隐含的入口符号需求:
/// - 含任何 agentxx.agent.* (或无前缀/vendor 前缀) → 需要 agent 入口符号
/// - 含任何 agentxx.client.* (或无前缀/vendor 前缀) → 需要 client 入口符号
/// 用于加载路径的 dlsym 意图预检: 声明了某侧接口却未导出该侧入口 → 明确报错
struct RequiredEntrySides {
    bool agentEntry  = false;
    bool clientEntry = false;
};

RequiredEntrySides requiredEntrySides(const std::vector<std::string>& interfaces);

} // namespace plugin
} // namespace agentxx

#endif /* AGENTXX_PLUGIN_INTERFACES_H */
