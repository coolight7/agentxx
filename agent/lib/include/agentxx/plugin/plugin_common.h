/*
 * agentxx/plugin/plugin_common.h —— 插件系统公共设施 (agent/client 两侧共享)
 *
 * 背景: plugin_manager.cpp (agent 侧) 与 client_plugin_manager.cpp (client 侧)
 * 存在大量重复基建 (插件名推导 / manifest 解析 / entry 路径解析 / 拓扑排序 /
 * 反向依赖收集 / io 线程同步等待 / C ABI 异常兜底宏)。提取到本头避免两侧
 * 行为漂移 —— 历史上 client 侧多次"漏掉 agent 侧已修的问题" (见
 * plugins.md 13.x 记录), 公共化后修复只做一次。
 *
 * 内容:
 * - XX_PLUGIN_CATCH_*: C ABI 边界异常兜底宏 (宏定义, 无 ODR 问题)
 * - pluginNameFromPath / parsePluginManifest / resolvePluginEntryPath:
 *   插件名推导与目录 manifest 解析 (平台扩展名修正 + 配置子目录回退)
 * - topoSortPlugins<T>: Kahn 拓扑排序 (依赖者排在被依赖者之后)
 * - ioCallSync / ioCallSyncVoid: 跨线程投递到 io 线程同步等待 (duck typing,
 *   Mgr 须提供 isIoThread()/postToIo())
 * - collectReverseRequiredDeps: 反向必选依赖收集 (模板, Plugin 须含
 *   depends/enabled 成员)
 *
 * 线程约定: 本头内非模板函数均可在任意线程调用 (纯函数); 模板函数按
 * 各 manager 的线程模型工作。
 */
#pragma once

#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/util/log.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/// C ABI 边界异常兜底 (函数式): vtable 函数内部不得让 C++ 异常逃逸 (跨边界 UB),
/// 统一捕获转日志并按失败返回值返回。
/// 历史宏 XX_PLUGIN_CATCH_BEGIN/END 已移除，统一使用 guardVtableCall / guardVtableCallVoid
/// 函数式接口 (见下方)，调用示例:
///   return guardVtableCall(-1, [&]() { ... });
///   guardVtableCallVoid([&]() { ... });

namespace agentxx {
namespace plugin {

inline std::string_view svToSv(AgentxxPluginStringView sv) noexcept {
    return sv.data ? std::string_view{sv.data, static_cast<size_t>(sv.size)} : std::string_view{};
}

inline std::string_view svToSv(const AgentxxPluginStringView* sv) noexcept {
    return (sv && sv->data) ? std::string_view{sv->data, static_cast<size_t>(sv->size)}
                            : std::string_view{};
}

inline std::string svToStr(AgentxxPluginStringView sv) {
    return sv.data ? std::string{sv.data, static_cast<size_t>(sv.size)} : std::string{};
}

inline std::string svToStr(const AgentxxPluginStringView* sv) {
    return (sv && sv->data) ? std::string{sv->data, static_cast<size_t>(sv->size)} : std::string{};
}

inline AgentxxPluginStringView strToSv(std::string_view sv) noexcept {
    return agentxx::plugin::PluginStringView::from(sv.data(), static_cast<uint64_t>(sv.size()));
}

namespace detail {
template<typename Fn, typename Ret>
inline Ret guardVtableCallImpl(Ret fallback, Fn&& fn) noexcept {
    try {
        return fn();
    } catch (const std::exception& e) {
        XX_LOGE("plugin vtable exception: {}", e.what());
        return fallback;
    } catch (...) {
        XX_LOGE("plugin vtable unknown exception");
        return fallback;
    }
}

template<typename Fn>
inline void guardVtableCallVoidImpl(Fn&& fn) noexcept {
    try {
        fn();
    } catch (const std::exception& e) {
        XX_LOGE("plugin vtable exception: {}", e.what());
    } catch (...) {
        XX_LOGE("plugin vtable unknown exception");
    }
}
} // namespace detail
} // namespace plugin
} // namespace agentxx

namespace agentxx {
namespace plugin {
template<
    typename Ret,
    typename Fn,
    typename = std::enable_if_t<!std::is_same_v<std::decay_t<Ret>, std::nullptr_t>>>
inline Ret guardVtableCall(Ret fallback, Fn&& fn) noexcept {
    return ::agentxx::plugin::detail::guardVtableCallImpl<Fn, Ret>(
        std::move(fallback),
        std::forward<Fn>(fn)
    );
}

/// std::nullptr_t 重载: fallback 为 nullptr 时按 lambda 返回类型推导指针类型
template<typename Fn>
inline auto guardVtableCall(std::nullptr_t, Fn&& fn) noexcept -> std::invoke_result_t<Fn> {
    using Ret = std::invoke_result_t<Fn>;
    try {
        return fn();
    } catch (const std::exception& e) {
        XX_LOGE("plugin vtable exception: {}", e.what());
        if constexpr (std::is_pointer_v<Ret>) {
            return Ret(nullptr);
        } else {
            return Ret{};
        }
    } catch (...) {
        XX_LOGE("plugin vtable unknown exception");
        if constexpr (std::is_pointer_v<Ret>) {
            return Ret(nullptr);
        } else {
            return Ret{};
        }
    }
}

template<typename Fn>
inline void guardVtableCallVoid(Fn&& fn) noexcept {
    ::agentxx::plugin::detail::guardVtableCallVoidImpl(std::forward<Fn>(fn));
}
} // namespace plugin
} // namespace agentxx

namespace agentxx {
namespace plugin {

std::string_view pluginStringView2std(AgentxxPluginStringView str);

const AgentxxPluginBuiltinInfo* findBuiltinPlugin(std::string_view name);

const AgentxxPluginBuiltinManifest* findBuiltinManifest(std::string_view name);

/// 从库文件名推断插件名 (libfoo.so → foo; foo.dll → foo; libfoo.so.1.2 → foo;
/// my.plugin.so → my.plugin)
/// - 扩展名剥离用 rfind (兼容文件名内含扩展名片段, 如 my.plugin.so → my.plugin)
/// - 仅当剥离过扩展名后才去 lib 前缀: 无扩展名的库/目录 (如插件目录
///   `libanalysis`) 保持原名, 避免误剥
std::string pluginNameFromPath(const std::string& path);

/// 插件清单资源声明 (plugin.yaml 可选段; 相对插件目录的路径已解析为绝对路径)
/// - 键名与主配置 yaml 的 skill/memory/mcp 段一致 (降低理解成本):
///     skill:  [dir, ...]                  → skillDirs
///     memory: [file, ...]                 → memoryFiles
///     mcp:    [{namespace,url,timeout},…] → mcpServers
/// - 仅 agent 侧使用; client 侧插件宿主忽略资源声明
struct PluginManifestResources {
    /// 单个 MCP server 声明
    struct McpDecl {
        std::string url;
        long long   timeoutMs = 120000; ///< 工具调用超时; 0 = 不限制
    };

    std::vector<std::string> skillDirs;
    std::vector<std::string> memoryFiles;
    /// key = MCP 工具命名空间
    std::map<std::string, McpDecl> mcpServers;
};

/// 解析插件目录 plugin.yaml 清单 (name/entry/depends/optional_depends)
/// - 返回 false 表示解析失败 (目录无 plugin.yaml 或 yaml 非法/缺字段);
///   YAML 非法时记日志, 无 manifest 不记 (调用方决定日志策略)
/// - resources 非空时额外输出资源声明段 (skill/memory/mcp, 见上);
///   段缺失时保持为空 —— 资源声明不参与 manifest 合法性判定
/// - interfaces 非空时额外输出接口声明段 (require/optional, 见
///   PluginManifestInterfaces); 段缺失时保持为空 —— 接口声明不参与
///   manifest 合法性判定
struct PluginManifestInterfaces; // 完整定义见下方接口协商节
bool parsePluginManifest(
    const std::filesystem::path& dir,
    std::string&                 name,
    std::string&                 entry,
    std::vector<std::string>&    depends,
    std::vector<std::string>&    optionalDepends,
    PluginManifestResources*     resources  = nullptr,
    PluginManifestInterfaces*    interfaces = nullptr
);

/// 从 YAML 字符串解析插件清单 (内置内嵌 `plugin.yaml` 原文路径)
/// - 语义与 parsePluginManifest 完全一致, 仅输入为内存 YAML 字符串而非目录文件
/// - baseDir 为资源相对路径的解析基准 (为空则按当前工作目录/保持原样, 内置清单
///   通常为空 —— 内置插件的 skill/memory 声明较少, 且多为绝对路径)
bool parsePluginManifestFromString(
    const std::string&           yamlStr,
    const std::filesystem::path& baseDir,
    std::string&                 name,
    std::string&                 entry,
    std::vector<std::string>&    depends,
    std::vector<std::string>&    optionalDepends,
    PluginManifestResources*     resources  = nullptr,
    PluginManifestInterfaces*    interfaces = nullptr
);

/// 尝试从内置内嵌清单解析 (优先于文件系统)
/// - 内置编译时 plugin.yaml 已随二进制内嵌, 无需外部文件即可取 depends/resources/interfaces
/// - 返回 true 表示命中内置清单并解析成功; false 表示无内置清单 (回退文件系统)
bool parseBuiltinManifest(
    std::string_view          pluginName,
    std::string&              name,
    std::string&              entry,
    std::vector<std::string>& depends,
    std::vector<std::string>& optionalDepends,
    PluginManifestResources*  resources  = nullptr,
    PluginManifestInterfaces* interfaces = nullptr
);

/// 目录插件 entry 相对路径 → 平台化绝对库路径:
/// - entry 按 Linux 书写 (libfoo.so), Windows/macOS 下修正扩展名 (.dll/.dylib)
/// - 多配置生成器 (MSVC Debug/Release) 产物位于配置子目录: {dir}/{entry}
///   找不到时回退 {dir}/{Debug|Release|RelWithDebInfo|MinSizeRel}/{entry}
/// - 返回绝对路径 (可能不存在, 由调用方处理)
std::string resolvePluginEntryPath(const std::filesystem::path& dir, const std::string& entry);

/* ==================== 接口协商 (三层协商的声明/校验基础设施) ====================
 *
 * 背景: client 宿主形态多样 (cli/tui/gui/第三方 app), 各自支持的插件接口
 * 不同; server (server-io) 仅有 libagentxx 一个实现, agent 侧接口集 ≡ 核心
 * 契约 + 全部标准接口表 (版本匹配即全集)。协商机制因此是不对称的:
 * 机制通用, 实际起作用的门禁集中在 client 侧。
 *
 * 三层设计 (详见 docs/zh-cn/plugins.md 4.7):
 * 1. 声明层: 插件 plugin.yaml `interfaces.require/optional` 列出依赖的接口
 *    名 (稳定字符串: 本项目内置为 "agentxx.agent.*" / "agentxx.client.*",
 *    第三方私有接口用 "<vendor>.*"; agent 侧可用接口表 IID 精确声明,
 *    如 "agentxx.agent.tools"/"agentxx.agent.prompt");
 * 2. 校验层: 宿主加载前按前缀过滤出本侧声明, 与宿主支持集比对 ——
 *    require 未满足 → 跳过加载 (INFO + 原因记录, 非错误: 同一插件目录
 *    服务多种宿主是预期情况); 声明了本侧接口却缺对应入口符号 → 明确报错;
 * 3. 决策层: 插件 entry 内经 query_interface 判空 / EVT_READY 与
 *    get_client_state 的 "interfaces" 字符串数组自行决定启用哪些功能;
 *    展示类子能力经 "agentxx.client.ui" 接口表访问 (表内不支持项为 NULL
 *    函数指针)
 *
 * 命名规范: "agentxx." 为本项目内置接口的保留命名空间, 第三方插件不得使用
 * (第三方私有接口用 "<vendor>.<name>", 宿主不认识的名称一律视为不支持);
 * 前缀过滤规则见 sideCaresAboutInterface。
 *
 * api_version 精确匹配门禁保留且不被本机制替代: 核心结构体是 C 结构,
 * 老宿主+新插件按新偏移读字段是 UB —— 接口协商只解决"功能子集"维度。
 * COM 风格接口表机制使未来新增能力不再动全局版本号: 每个接口表自带
 * version 字段独立演进。
 */

/// 已知接口名常量 (稳定契约; 第三方私有接口用 "<vendor>.<name>" 自定义,
/// 宿主不认识的名称一律视为不支持 —— 安全失败)
namespace plugin_interfaces {
/// 元接口: 宿主实现完整核心契约 (plugin_api.h v1 核心 vtable) + 标准接口表
/// 全集 (libagentxx 即此类宿主); 精简第三方宿主可仅声明实际实现的下述子集
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

/* ---- client 侧: 接口表名 + 细粒度能力名 (映射到 agentxx.client.ui 表的非空成员) ---- */
inline constexpr std::string_view ClientUi = AGENTXX_IFACE_CLIENT_UI; ///< 展示扩展表整体
inline constexpr std::string_view ClientStatusItem  = "agentxx.client.status_item";
inline constexpr std::string_view ClientPanel       = "agentxx.client.panel";
inline constexpr std::string_view ClientToast       = "agentxx.client.toast";
inline constexpr std::string_view ClientKeybind     = "agentxx.client.keybind";      // 预留
inline constexpr std::string_view ClientPromptModal = "agentxx.client.prompt_modal"; // 预留
/// 工具消息装饰 (ui 表 v2 update_tool_decor; TUI 声明, CLI 无消息渲染面不声明)
inline constexpr std::string_view ClientMsgDecor    = "agentxx.client.msg_decor";
inline constexpr std::string_view ClientInfoSection = "agentxx.client.info_section";
inline constexpr std::string_view ClientCommand     = "agentxx.client.command";
} // namespace plugin_interfaces

/// 宿主支持的接口集合 (稳定名字符串; io 线程构建后只读)
using InterfaceSet = std::set<std::string, std::less<>>;

/// 插件清单接口声明 (plugin.yaml 可选段):
///   interfaces:
///     require:  [agentxx.agent.core, agentxx.client.command]  # 任一缺失(按前缀过滤后) →
///     该侧跳过加载 optional: [agentxx.client.toast]                # 缺失仅警告, 不影响加载
/// - 同一清单可同时声明两侧接口 (前缀决定归属), 服务 cli/tui/gui 多宿主
struct PluginManifestInterfaces {
    std::vector<std::string> require;
    std::vector<std::string> optional;

    bool empty() const {
        return require.empty() && optional.empty();
    }
};

/// 本侧是否关心此接口声明 (前缀过滤规则):
/// - "agentxx.agent.*": 仅 agent 侧检查 (client 侧忽略)
/// - "agentxx.client.*": 仅 client 侧检查 (agent 侧忽略)
/// - 其他 (无前缀 / "<vendor>.*"): 两侧都检查
///   (保守: 宿主不认识即不支持; "agentxx." 为本项目保留命名空间,
///   第三方不得使用, 故 agentxx. 开头但非 agent/client 子前缀的名称同样
///   两侧都检查)
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
    const PluginManifestInterfaces& decl,
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

/// 拓扑排序项 (调用方 Item 须含 path/name/depends 三个成员, 可附带其他字段)
struct PluginSortItem {
    std::string              path;
    std::string              name; ///< 空 = 无法推导 (不影响排序)
    std::vector<std::string> depends;
};

/// 拓扑排序 (Kahn): 依赖者排在被依赖者之后, 避免配置顺序导致必选依赖缺失
/// - 配置列表中且未放置的依赖 → 未满足; 不在配置列表的依赖视为已满足 (已加载)
/// - 无进展 (环/缺失) 时剩余项按原序附后, 由加载路径的依赖检查报错
template<typename T>
std::vector<T> topoSortPlugins(std::vector<T> items) {
    std::vector<T> ordered;
    ordered.reserve(items.size());
    std::vector<bool> placed(items.size(), false);
    size_t            placedCount = 0;
    while (placedCount < items.size()) {
        size_t progress = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            if (placed[i]) {
                continue;
            }
            bool depsOk = true;
            for (const auto& d : items[i].depends) {
                for (size_t j = 0; j < items.size(); ++j) {
                    if (!placed[j] && items[j].name == d) {
                        depsOk = false;
                        break;
                    }
                }
                if (!depsOk) {
                    break;
                }
            }
            if (depsOk) {
                ordered.push_back(std::move(items[i]));
                placed[i] = true;
                ++progress;
            }
        }
        if (progress == 0) {
            // 环或依赖缺失: 剩余项附后 (加载时依赖检查报错)
            for (size_t i = 0; i < items.size(); ++i) {
                if (!placed[i]) {
                    ordered.push_back(std::move(items[i]));
                    placed[i] = true;
                }
            }
            break;
        }
        placedCount += progress;
    }
    return ordered;
}

/// 在 io 线程执行并同步等待结果 (调用方为 io 线程时直接执行)
/// - 供 vtable 的 io 线程约束操作跨线程调用 (JS 线程/宿主线程池) 使用;
///   调用方线程阻塞等待, io 线程为事件循环 (挂起而非忙等), 无死锁风险
/// - Mgr 须提供 isIoThread()/postToIo(); T 为第一个显式模板参数, Mgr 推导
template<typename T, typename Mgr>
T ioCallSync(Mgr* mgr, std::function<T()> fn) {
    if (!mgr) {
        throw std::runtime_error("plugin manager released");
    }
    if (mgr->isIoThread()) {
        return fn();
    }
    std::promise<T> p;
    auto            fut = p.get_future();
    mgr->postToIo([&p, &fn]() {
        try {
            p.set_value(fn());
        } catch (...) {
            p.set_exception(std::current_exception());
        }
    });
    return fut.get();
}

/// ioCallSync 的 void 特化
template<typename Mgr>
void ioCallSyncVoid(Mgr* mgr, std::function<void()> fn) {
    if (!mgr) {
        return;
    }
    if (mgr->isIoThread()) {
        fn();
        return;
    }
    std::promise<void> p;
    auto               fut = p.get_future();
    mgr->postToIo([&p, &fn]() {
        try {
            fn();
            p.set_value();
        } catch (...) {
            p.set_exception(std::current_exception());
        }
    });
    fut.get();
}

/// 收集必选依赖 target 的插件名 (io 线程)
/// - onlyEnabled=true: 仅统计 enabled 的插件 (卸载/禁用级联)
/// - onlyEnabled=false: 全部统计 (启用级联: 需恢复被级联禁用的插件)
/// - PluginMap 元素须含 name(键)/depends/enabled 成员
template<typename PluginMap>
std::vector<std::string> collectReverseRequiredDeps(
    const PluginMap&   plugins,
    const std::string& target,
    bool               onlyEnabled
) {
    std::vector<std::string> out;
    for (const auto& [name, inst] : plugins) {
        if (name == target || (onlyEnabled && !inst->enabled)) {
            continue;
        }
        if (std::find(inst->depends.begin(), inst->depends.end(), target) != inst->depends.end()) {
            out.push_back(name);
        }
    }
    return out;
}

} // namespace plugin
} // namespace agentxx
