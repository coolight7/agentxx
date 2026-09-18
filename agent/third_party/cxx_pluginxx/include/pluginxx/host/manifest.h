/// pluginxx 插件清单与名称推导 (宿主侧, 与宿主领域无关)
///
/// 内容:
/// - 插件名推导: [pluginNameFromPath] (`libfoo.so` → `foo`)、`builtin://` 简写解析;
/// - 清单解析: [parsePluginManifest] (目录 `plugin.yaml`) /
///   [parsePluginManifestFromString] / [parseBuiltinManifest] (内嵌清单);
/// - 资源声明: [PluginManifestResources] (skill/memory/mcp; 相对路径已解析为绝对);
/// - 接口声明: [PluginManifestInterfaces] (require/optional 接口名清单);
/// - 入口路径解析: [resolvePluginEntryPath] (平台扩展名修正 + 配置子目录回退);
/// - 依赖排序: [PluginSortItem] / [topoSortPlugins] (Kahn 拓扑排序)。
///
/// 接口声明的**协商** (前缀归属与宿主支持集比对) 属于宿主领域, 见宿主侧
/// `agentxx/plugin/plugin_interfaces.h`。
///
/// 线程约定: 本头内函数均为纯函数, 可在任意线程调用。
#ifndef PLUGINXX_HOST_MANIFEST_H
#define PLUGINXX_HOST_MANIFEST_H

#include "pluginxx/api/abi.h"
#include "utilxx_base/log.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pluginxx {

/// yaml 配置中内置插件路径简写 `builtin://<name>` 的判定
/// - 只认前缀, 不校验名称非空 (空名称由调用方按非法路径处理)
inline bool isBuiltinScheme(std::string_view p) noexcept {
    return p.size() > 10 && p.substr(0, 10) == "builtin://";
}

/// 提取 `builtin://<name>` 中的插件名
/// - 非 `builtin://` 前缀时按原样取第 10 字节起的子串 (调用方先经 [isBuiltinScheme] 判定)
inline std::string parseBuiltinName(std::string_view p) {
    return std::string(p.substr(10));
}

/// 内置插件清单提供者: 由"把插件合并编译进自身二进制"的宿主注册
///
/// 背景: 内置插件表由宿主经 `configure_file` 生成 (见 agentxx 的
/// `plugins/builtin_plugins.cpp.in`), 其符号不属于框架内核。框架内核因此不直接
/// 引用宿主符号, 改由宿主在启动早期把提供者注册进来; 未注册时视为"无内置插件"
/// (返回 0 项), 纯动态库宿主无需注册。
struct BuiltinPluginProvider {
    /// 返回内置插件描述静态数组, 并写出元素个数 (可为空)
    const AgentxxPluginBuiltinInfo* (*plugins)(uint64_t* count) = nullptr;
    /// 返回内置内嵌清单静态数组, 并写出元素个数 (可为空)
    const AgentxxPluginBuiltinManifest* (*manifests)(uint64_t* count) = nullptr;
};

/// 注册内置插件清单提供者 (宿主静态初始化期调用一次即可; 幂等覆盖)
void setBuiltinPluginProvider(BuiltinPluginProvider provider) noexcept;

/// 读取当前注册的内置插件清单提供者
BuiltinPluginProvider builtinPluginProvider() noexcept;

/// 按名查找内置插件描述 (未注册提供者/未命中返回 nullptr)
const AgentxxPluginBuiltinInfo* findBuiltinPlugin(std::string_view name);

/// 按名查找内置内嵌清单 (未注册提供者/未命中返回 nullptr)
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

/// 插件清单接口声明 (plugin.yaml 可选段 `interfaces`, 键名 require/optional):
///   interfaces:
///     require:  [agentxx.agent.core, agentxx.client.command]
///     optional: [agentxx.client.toast]
/// - 同一清单可同时声明两侧接口 (前缀决定归属), 服务 cli/tui/gui 多宿主
/// - 归属过滤与宿主支持集比对由宿主侧的 checkInterfacesForSide 完成
struct PluginManifestInterfaces {
    std::vector<std::string> require;
    std::vector<std::string> optional;

    bool empty() const {
        return require.empty() && optional.empty();
    }
};

/// 解析插件目录 plugin.yaml 清单 (name/entry/depends/optional_depends)
/// - 返回 false 表示解析失败 (目录无 plugin.yaml 或 yaml 非法/缺字段);
///   YAML 非法时记日志, 无 manifest 不记 (调用方决定日志策略)
/// - resources 非空时额外输出资源声明段 (skill/memory/mcp, 见上);
///   段缺失时保持为空 —— 资源声明不参与 manifest 合法性判定
/// - interfaces 非空时额外输出接口声明段 (require/optional, 见
///   PluginManifestInterfaces); 段缺失时保持为空 —— 接口声明不参与
///   manifest 合法性判定
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
    std::vector<T>    ordered;
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

} // namespace pluginxx

#endif /* PLUGINXX_HOST_MANIFEST_H */
