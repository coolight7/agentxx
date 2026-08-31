/*
 * agentxx/src/plugins/builtin_plugin_registry.cpp —— 内置插件注册表默认实现
 *
 * - 默认构建 (AGENTXX_ENABLE_PLUGIN_BUILTIN=OFF): 本文件提供空注册表,
 *   agentxx_get_builtin_plugins 返回占位条目 (name == NULL), PluginManager
 *   的 findBuiltinPlugin 按 name 匹配自然跳过 → 运行期行为与纯动态加载一致
 * - 内置合并编译模式 (AGENTXX_ENABLE_PLUGIN_BUILTIN=ON): 由 plugins/
 *   CMakeLists.txt 生成的清单 builtin_plugins.cpp 提供同名符号实现, 本文件
 *   经编译定义 AGENTXX_USE_BUILTIN_PLUGIN_MANIFEST=1 排除 (见
 *   lib/CMakeLists.txt 内置分支), 避免重复定义
 */
#include "agentxx/plugin/builtin_plugin.h"

#ifndef AGENTXX_USE_BUILTIN_PLUGIN_MANIFEST

const AgentxxBuiltinPluginInfo* agentxx_get_builtin_plugins(size_t* count) {
    // 空表占位 (静态, 进程生命周期有效): 宿主按 name == NULL 跳过
    static const AgentxxBuiltinPluginInfo kEmpty[] = {
        {nullptr, nullptr, nullptr, nullptr},
    };
    if (count) {
        *count = sizeof(kEmpty) / sizeof(kEmpty[0]);
    }
    return kEmpty;
}

const AgentxxBuiltinManifest* agentxx_get_builtin_manifests(size_t* count) {
    static const AgentxxBuiltinManifest kEmptyM[] = {
        {nullptr, nullptr},
    };
    if (count) {
        *count = sizeof(kEmptyM) / sizeof(kEmptyM[0]);
    }
    return kEmptyM;
}

#endif /* AGENTXX_USE_BUILTIN_PLUGIN_MANIFEST */
