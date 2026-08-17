/*
 * agentxx/plugin/builtin_plugin.h —— 内置插件注册表 (可选合并编译到 libagentxx)
 *
 * 背景:
 * - 默认构建中, 插件编译为独立动态库 (MODULE), 运行期由 PluginManager 经
 *   dlopen/LoadLibrary 加载 (见 plugin_manager.h NativeLoader)
 * - 内置模式 (AGENTXX_ENABLE_PLUGIN_BUILTIN=ON): 启用的插件源文件直接
 *   编译进 libagentxx, 运行期无需任何插件动态库文件 (适合嵌入式/单文件
 *   分发等不便 dlopen 的场景); 插件入口经编译期改名避免多插件符号冲突
 *   (agentxx_plugin_entry → agentxx_plugin_builtin_entry_<插件名>), 改名
 *   后的符号由 CMake 生成的清单 (plugins/builtin_plugins.cpp.in) 汇总,
 *   经本头声明的 agentxx_get_builtin_plugins() 暴露给 PluginManager
 * - 本头为纯 C ABI: 清单实现仅依赖 plugin_api.h 的类型 (宿主与插件共用)
 * - 兼容性: 未启用内置模式时, agentxx_get_builtin_plugins() 返回空表,
 *   运行期行为与纯动态加载完全一致 (PluginManager 先查动态库, 缺失时
 *   回退内置注册表, 见 plugin_manager.cpp loadPluginAsync)
 *
 * 版本策略: 结构体字段仅在 api 主版本升级时调整; 宿主按 name 匹配,
 * 未知条目忽略 (向前兼容)
 */
#ifndef AGENTXX_BUILTIN_PLUGIN_H
#define AGENTXX_BUILTIN_PLUGIN_H

#include "agentxx/plugin/plugin_api.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 内置插件描述 (编译进 libagentxx 的插件; 静态数组, 进程生命周期有效)
typedef struct AgentxxBuiltinPluginInfo {
    const char*            name;     ///< 插件唯一名 (如 "example_plugin"); NULL = 空表占位
    AgentxxPluginGetInfoFn get_info; ///< 可空 (加载前元信息校验, 与 dlsym 可选符号同语义)
    AgentxxPluginEntryFn   entry;    ///< 必需 (插件入口, 与 agentxx_plugin_entry 同契约)
    AgentxxPluginUnloadFn  unload;   ///< 可空 (卸载回调, 与 agentxx_plugin_unload 同契约)
} AgentxxBuiltinPluginInfo;

/// 查询全部内置插件 (libagentxx 实现; 返回静态数组, count 输出条目数)
/// - 调用方须跳过 name == NULL 的占位条目 (空表时 count 为 1)
/// - 任意线程可调用 (静态只读数据)
const AgentxxBuiltinPluginInfo* agentxx_get_builtin_plugins(size_t* count);

#ifdef __cplusplus
}
#endif

#endif /* AGENTXX_BUILTIN_PLUGIN_H */
