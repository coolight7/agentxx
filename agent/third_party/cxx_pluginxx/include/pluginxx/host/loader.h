/// pluginxx 动态库装载封装 (宿主侧, 与宿主领域无关)
///
/// 背景: 插件以动态库形式分发, 装载需要跨平台的 dlopen/LoadLibrary 封装。
/// 提取到框架内核后 agentxx / musicxx 等宿主共用同一实现, 避免平台细节漂移。
///
/// 使用约定:
/// - [NativeLoader::open] 一律使用 **RTLD_NOW | RTLD_LOCAL / LoadLibraryW**:
///   立即解析符号 (缺符号立刻失败, 而不是在首次调用时崩溃), 且不把插件符号
///   放进全局命名空间 (避免不同插件之间的符号互相遮蔽);
/// - 卸载必须由宿主在"实例 lease 归零"之后执行: dlclose/FreeLibrary 越过仍可能
///   执行的插件代码即未定义行为 (见 pluginxx/runtime/runtime.h 的 lease 说明)。
#ifndef PLUGINXX_HOST_LOADER_H
#define PLUGINXX_HOST_LOADER_H

#include <string>

namespace pluginxx {

/// 动态库装载/符号查找/卸载 (跨平台: dlopen / LoadLibraryW)
struct NativeLoader {
    /// 打开动态库; 失败返回 nullptr 并在 err 输出原因
    static void* open(const std::string& path, std::string& err);

    /// 按名查找符号; 失败返回 nullptr 并在 err 输出原因
    static void* sym(void* handle, const char* name, std::string& err);

    /// 卸载动态库 (空句柄安全忽略)
    static void close(void* handle);
};

} // namespace pluginxx

#endif /* PLUGINXX_HOST_LOADER_H */
