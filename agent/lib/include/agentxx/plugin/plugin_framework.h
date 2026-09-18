/// agentxx 插件宿主侧框架内核的命名空间入口
///
/// 背景: 插件框架内核 (宿主运行时 / 装载 / 清单解析 / Operation 驱动器 / SDK 基座)
/// 已拆为独立库 `cxx_pluginxx` (命名空间 `pluginxx`), 供 agentxx / musicxx 等宿主
/// 复用。宿主侧的插件管理器与 vtable 实现仍写在 `agentxx::plugin` 内, 若每处都
/// 书写 `pluginxx::` 双命名空间会显著增加噪声且容易漏改。
///
/// 本头把宿主侧实际用到的内核类型/函数以**逐条 using 声明**的方式引入
/// `agentxx::plugin`, 因此:
/// - 类型本体只有 `pluginxx` 一份 (不存在两份实现或 ODR 风险);
/// - `agentxx::plugin::PluginInstanceBase` 与 `pluginxx::PluginInstanceBase`
///   是**同一个类型**, 两侧代码可自由混用;
/// - 新增内核类型时按需在此追加一行, 导出面始终显式可读。
///
/// 注意: 这里不是"转发头"——没有 `agentxx/plugin/plugin_runtime.h` 之类的文件路径
/// 兼容层; 内核头文件只能经 `pluginxx/...` 路径包含。
#ifndef AGENTXX_PLUGIN_FRAMEWORK_H
#define AGENTXX_PLUGIN_FRAMEWORK_H

#include "pluginxx/host/abi_util.h"
#include "pluginxx/host/capability_registry.h"
#include "pluginxx/host/domain_hooks.h"
#include "pluginxx/host/event_bus.h"
#include "pluginxx/host/host_core.h"
#include "pluginxx/host/lifecycle.h"
#include "pluginxx/host/loader.h"
#include "pluginxx/host/manifest.h"
#include "pluginxx/host/tables_impl.h"
#include "pluginxx/runtime/driver.h"
#include "pluginxx/runtime/instance_base.h"
#include "pluginxx/runtime/manager_base.h"
#include "pluginxx/runtime/op_driver.h"
#include "pluginxx/runtime/runtime.h"

#include <memory>

namespace agentxx {
namespace plugin {

// ==================== 实例与生命周期 (pluginxx/runtime/instance_base.h) ====================
using pluginxx::getExecutableDirPath;
using pluginxx::hostMemoryAlloc;
using pluginxx::hostMemoryCreateString;
using pluginxx::hostMemoryFree;
using pluginxx::hostMemorySetString;
using pluginxx::PluginHostControl;
using pluginxx::PluginInstanceBase;
using pluginxx::resolvePluginHostControl;

// ==================== 运行时状态机与执行 lease (pluginxx/runtime/runtime.h) ====================
using pluginxx::enqueueRuntimeAction;
using pluginxx::InstanceLease;
using pluginxx::InstanceLifetime;
using pluginxx::isRuntimeIoThread;
using pluginxx::PluginInstanceState;
using pluginxx::PluginRuntime;
using pluginxx::replayRuntimeActions;
using pluginxx::runtimeExecutorStopped;

// ==================== 管理器公共基类与 vtable 入口上下文 ====================
// (pluginxx/runtime/manager_base.h)
using pluginxx::collectReverseRequiredDeps;
using pluginxx::enterPluginHost;
using pluginxx::PluginHostCall;
using pluginxx::PluginManagerBase;

// ==================== Operation 驱动器 (pluginxx/runtime/op_driver.h) ====================
using pluginxx::awaitPluginLifecycle;
using pluginxx::awaitPluginOp;
using pluginxx::cancelPluginOperation;
using pluginxx::OpCore;
using pluginxx::OpDrive;
using pluginxx::OpErrorCode;
using pluginxx::OpGuardPtr;
using pluginxx::PluginOpAwaitArgs;
using pluginxx::PluginOperationState;

// ==================== 动态库装载 (pluginxx/host/loader.h) ====================
using pluginxx::NativeLoader;

// ==================== 清单解析与名称推导 (pluginxx/host/manifest.h) ====================
using pluginxx::findBuiltinManifest;
using pluginxx::findBuiltinPlugin;
using pluginxx::isBuiltinScheme;
using pluginxx::parseBuiltinManifest;
using pluginxx::parseBuiltinName;
using pluginxx::parsePluginManifest;
using pluginxx::parsePluginManifestFromString;
using pluginxx::PluginManifestInterfaces;
using pluginxx::PluginManifestResources;
using pluginxx::pluginNameFromPath;
using pluginxx::PluginSortItem;
using pluginxx::resolvePluginEntryPath;
using pluginxx::topoSortPlugins;

// ==================== 能力注册表 (pluginxx/host/capability_registry.h) ====================
using pluginxx::CapabilityRegistry;
using pluginxx::PluginCapabilityRegistration;

// ==================== 宿主核心与领域钩子 (pluginxx/host/host_core.h) ====================
using pluginxx::DomainHooks;
using pluginxx::EventSource;
using pluginxx::PluginHostCore;
using pluginxx::unsubscribePluginSubscription;

// ==================== 宿主生命周期骨架 (pluginxx/host/lifecycle.h) ====================
using pluginxx::PluginHostLifecycle;
using pluginxx::PluginLoadOptions;

// ==================== 通用表入口装配 (pluginxx/host/tables_impl.h) ====================
using pluginxx::GenericTableEntries;
using pluginxx::queryGenericPluginIface;

// ==================== C ABI 辅助 (pluginxx/host/abi_util.h) ====================
using pluginxx::guardVtableCall;
using pluginxx::guardVtableCallVoid;
using pluginxx::ioCallSync;
using pluginxx::ioCallSyncKeep;
using pluginxx::ioCallSyncVoid;
using pluginxx::ioCallSyncVoidKeep;
using pluginxx::pluginStringView2std;
using pluginxx::strToSv;
using pluginxx::svToStr;
using pluginxx::svToSv;

} // namespace plugin
} // namespace agentxx

#endif /* AGENTXX_PLUGIN_FRAMEWORK_H */
