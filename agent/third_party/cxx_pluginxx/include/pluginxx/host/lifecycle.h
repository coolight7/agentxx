/// pluginxx 宿主生命周期骨架 (装载 / 启停 / 禁用启用 / 卸载 / 级联依赖)
///
/// 定位: 把"与宿主领域无关"的生命周期逻辑从各宿主的管理器里收敛到一处 ——
/// 动态库装载与入口符号校验、create/start 事务、启用与禁用级联、stop 事务补齐、
/// inflight 归零等待、destroy 与动态库卸载、失败回滚与关闭超时重试。
///
/// 三层结构:
/// - [PluginHostCore] (host_core.h): 通用表 (log/json/config/plugins/events/
///   scheduler/coroutine_runtime/tasks/cancel/capabilities) 的状态与方法实现;
/// - 本类的 [PluginHostLifecycle]: 在宿主核心之上补齐上面那套生命周期骨架;
/// - 宿主 (agentxx / musicxx): 领域表 + 领域注册 + 领域数据。
///
/// 宿主用法:
/// ```cpp
/// class MyManager : public pluginxx::PluginHostLifecycle<MyInstance>,
///                   public std::enable_shared_from_this<MyManager>,
///                   public pluginxx::DomainHooks {
///     // 1. 构造体内 setDomainHooks(this) (通用表取数);
///     // 2. 覆写下方"宿主接缝"里的纯虚函数与需要的可选项;
///     // 3. query_interface 里用 pluginxx::queryGenericPluginIface 装配通用表。
/// };
/// ```
///
/// 线程约定: 装载/卸载是协程 (在宿主 IO executor 上运行); 其余方法除标注外
/// 都只在宿主 IO 线程调用。`disable`/`enable`/`shutdownAll` 为同步入口, 其中的
/// stop/start 事务按需投递到 IO 线程执行 (调用方不必在 IO 线程)。
#pragma once

#include "pluginxx/host/abi_util.h"
#include "pluginxx/host/domain_hooks.h"
#include "pluginxx/host/host_core.h"
#include "pluginxx/host/loader.h"
#include "pluginxx/host/manifest.h"
#include "pluginxx/runtime/instance_base.h"
#include "pluginxx/runtime/manager_base.h"
#include "pluginxx/runtime/op_driver.h"
#include "pluginxx/runtime/runtime.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace pluginxx {

/// 插件加载参数 (与宿主配置类型解耦: 宿主把自己的配置字段拷进来)
///
/// 背景: 各宿主的配置类型不同 (agentxx 的 `agent::PluginConfig` 等), 框架内核
/// 不能引用它们, 因此装载入口只接收这两个真正会用到的字段。
struct PluginLoadOptions {
    /// 插件配置参数 (yaml `plugins` 条目 args; 宿主原样保存, 不解析字段语义)
    utilxx_base::Json args = utilxx_base::Json::object();
    /// 插件配置文件所在目录或文件路径 (yaml `plugins` 条目 config)
    std::string configPath;
};

/// 宿主生命周期骨架: 装载 / 启停 / 禁用启用 / 卸载 / 级联依赖
///
/// - `InstanceT` 须继承 [PluginInstanceBase], 且由宿主经 [createInstance] 构造;
/// - 宿主领域动作经"宿主接缝"(纯虚函数与可选覆写)注入, 见各类说明;
/// - 本类提供的行为与 agentxx 既有实现逐条一致 (含失败回滚、关闭超时、
///   停止后重试、级联依赖), 新宿主无需重复实现。
template<typename InstanceT>
class PluginHostLifecycle : public PluginHostCore<InstanceT> {
public:

    using Base        = PluginHostCore<InstanceT>;
    using InstancePtr = std::shared_ptr<InstanceT>;

    explicit PluginHostLifecycle(asio::any_io_executor ex = {}) :
        Base(std::move(ex)) {}

    // =====================================================================
    // 生命周期 (公开入口)
    // =====================================================================

    /// 加载目录/文件形式的插件 (协程; 在宿主 IO executor 上执行)
    ///
    /// - `path` 可为插件目录 (自动解析 `plugin.yaml` 取 entry), 也可直接是动态库路径;
    /// - `options` 为插件参数与配置路径 (可为 nullptr, 等价于空参数);
    /// - `allowClientOnlySkip=true` 时, 缺少本端入口符号按"只有另一端入口"静默跳过
    ///   (INFO 日志), 否则按错误处理;
    /// - `resources`/`interfaces` 为清单解析出的声明段, 由调用方 (通常是
    ///   [loadPluginAsync]) 传入; 加载收尾时经 [applyDeclaredResources] 交给宿主。
    ///
    /// - `return` 加载成功返回实例; 任一环节失败返回 nullptr (错误记日志, 已做回滚)
    asio::awaitable<InstancePtr> loadNativeAsync(
        std::string                  path,
        const PluginLoadOptions*     options             = nullptr,
        bool                         allowMissingEntry   = false,
        const PluginManifestResources&  resources        = {},
        const PluginManifestInterfaces& interfaces       = {}
    ) {
        std::string err;
        void*       dl = NativeLoader::open(path, err);
        if (!dl) {
            XX_LOGE("{}Plugin load failed: {}: {}", logTag(), path, err);
            co_return nullptr;
        }

        auto getInfoFn = reinterpret_cast<AgentxxPluginGetInfoFn>(
            NativeLoader::sym(dl, AGENTXX_PLUGIN_AGENT_SYMBOL_GET_INFO, err)
        );
        std::string createErr;
        auto        createFn = reinterpret_cast<AgentxxPluginCreateFn>(
            NativeLoader::sym(dl, AGENTXX_PLUGIN_AGENT_SYMBOL_CREATE, createErr)
        );
        std::string startErr;
        auto        startFn = reinterpret_cast<AgentxxPluginStartFn>(
            NativeLoader::sym(dl, AGENTXX_PLUGIN_AGENT_SYMBOL_START, startErr)
        );
        std::string stopErr;
        auto        stopFn = reinterpret_cast<AgentxxPluginStopFn>(
            NativeLoader::sym(dl, AGENTXX_PLUGIN_AGENT_SYMBOL_STOP, stopErr)
        );

        if (!createFn) {
            NativeLoader::close(dl);
            if (allowMissingEntry) {
                XX_LOGW("{}Plugin `{}` skipped: no agent entry (client only)", logTag(), path);
                co_return nullptr;
            }
            XX_LOGE(
                "{}Plugin `{}` missing {}: {}",
                logTag(),
                path,
                AGENTXX_PLUGIN_AGENT_SYMBOL_CREATE,
                createErr
            );
            co_return nullptr;
        }
        // start/stop 是必备入口 (create 只构造, start 注册, stop 撤销):
        // 缺少任一符号说明插件未按当前契约导出, 直接拒绝加载。
        if (!startFn || !stopFn) {
            NativeLoader::close(dl);
            XX_LOGE(
                "{}Plugin `{}` missing lifecycle entry ({}: {}; {}: {}); plugins must export "
                "start/stop",
                logTag(),
                path,
                AGENTXX_PLUGIN_AGENT_SYMBOL_START,
                startFn ? "ok" : startErr,
                AGENTXX_PLUGIN_AGENT_SYMBOL_STOP,
                stopFn ? "ok" : stopErr
            );
            co_return nullptr;
        }

        const AgentxxPluginInfo* info = nullptr;
        try {
            info = getInfoFn ? getInfoFn() : nullptr;
        } catch (const std::exception& e) {
            XX_LOGW("{}Plugin `{}` get_info threw: {}", logTag(), path, e.what());
        } catch (...) {
            XX_LOGW("{}Plugin `{}` get_info threw unknown exception", logTag(), path);
        }
        if (info && info->api_version != AGENTXX_PLUGIN_API_VERSION) {
            NativeLoader::close(dl);
            XX_LOGE(
                "{}Plugin `{}` API version mismatch (got {}, host requires {})",
                logTag(),
                path,
                info->api_version,
                AGENTXX_PLUGIN_API_VERSION
            );
            co_return nullptr;
        }

        std::string name = info && info->name.data
                               ? std::string(info->name.data, info->name.size)
                               : std::filesystem::path(path).stem().string();
        if (name.starts_with("lib")) {
            name = name.substr(3);
        }

        if (!this->reservePluginName(name)) {
            XX_LOGE(
                "{}Plugin load rejected: duplicate or currently loading name `{}`",
                logTag(),
                name
            );
            NativeLoader::close(dl);
            co_return nullptr;
        }

        auto inst        = makeInstance(std::move(name), info, path, startFn, stopFn);
        inst->dlHandle   = dl;
        inst->interfaces = interfaces;
        applyLoadOptions(*inst, options);

        this->plugins_[inst->name] = inst;
        int rc                     = -1;
        try {
            rc = createFn(inst->hostView(), &inst->pluginCtx);
            // 即使 create 返回失败，只要交付了上下文，destroy 仍是宿主的责任。
            inst->pluginCreated = (inst->pluginCtx != nullptr);
        } catch (const std::exception& e) {
            XX_LOGE("{}Plugin `{}` create threw: {}", logTag(), inst->name, e.what());
            rc = -1;
        } catch (...) {
            XX_LOGE("{}Plugin `{}` create threw unknown exception", logTag(), inst->name);
            rc = -1;
        }

        if (rc != 0) {
            XX_LOGE(
                "{}Plugin `{}` create failed (code={}), performing rollback",
                logTag(),
                inst->name,
                rc
            );
            rollbackLoad(inst, /*closeHandle=*/true);
            co_return nullptr;
        }

        std::string startError;
        if (!co_await awaitPluginLifecycle(
                this->runtime(),
                inst,
                inst->pluginCtx,
                inst->lifecycleStart,
                "plugin start",
                startError
            )) {
            XX_LOGE("{}Plugin `{}` start failed: {}", logTag(), inst->name, startError);
            rollbackLoad(inst, /*closeHandle=*/true);
            co_return nullptr;
        }
        inst->lifecycleStarted = true;

        finishLoad(inst, resources);
        XX_LOGI("{}Plugin `{}` loaded successfully", logTag(), inst->name);
        co_return inst;
    }

    /// 加载"合并编译进宿主二进制"的内置插件 (协程)
    ///
    /// - 内置插件表由宿主经 `setBuiltinPluginProvider` 注册 (见 manifest.h);
    /// - `name` 为内置注册名, `path` 只作为展示/资源相对路径基准;
    /// - 无动态库句柄, 因此回滚时不关闭句柄。
    asio::awaitable<InstancePtr> loadBuiltinAsync(
        std::string                     name,
        std::string                     path,
        std::vector<std::string>        depends,
        std::vector<std::string>        optionalDepends,
        const PluginLoadOptions*        options            = nullptr,
        const PluginManifestResources&  resources          = {},
        const PluginManifestInterfaces& interfaces         = {}
    ) {
        auto entry = findBuiltinPlugin(name);
        if (!entry) {
            XX_LOGE("{}Built-in plugin `{}` not found in registry", logTag(), name);
            co_return nullptr;
        }
        if (!entry->create || !entry->start || !entry->stop) {
            XX_LOGE(
                "{}Built-in plugin `{}` does not export create/start/stop",
                logTag(),
                name
            );
            co_return nullptr;
        }

        const AgentxxPluginInfo* info = entry->get_info ? entry->get_info() : nullptr;
        if (info && info->api_version != AGENTXX_PLUGIN_API_VERSION) {
            XX_LOGE(
                "{}Builtin plugin `{}` API version mismatch (got {}, host requires {})",
                logTag(),
                name,
                info->api_version,
                AGENTXX_PLUGIN_API_VERSION
            );
            co_return nullptr;
        }

        if (!this->reservePluginName(name)) {
            XX_LOGE(
                "{}Builtin plugin load rejected: duplicate or currently loading name `{}`",
                logTag(),
                name
            );
            co_return nullptr;
        }

        auto inst             = makeInstance(name, info, path, entry->start, entry->stop);
        inst->builtinUnload   = entry->destroy;
        inst->depends         = std::move(depends);
        inst->optionalDepends = std::move(optionalDepends);
        inst->interfaces      = interfaces;
        applyLoadOptions(*inst, options);

        this->plugins_[inst->name] = inst;
        int rc                     = -1;
        try {
            rc                  = entry->create(inst->hostView(), &inst->pluginCtx);
            inst->pluginCreated = (inst->pluginCtx != nullptr);
        } catch (const std::exception& e) {
            XX_LOGE("{}Builtin plugin `{}` create threw: {}", logTag(), inst->name, e.what());
            rc = -1;
        } catch (...) {
            XX_LOGE("{}Builtin plugin `{}` create threw unknown exception", logTag(), inst->name);
            rc = -1;
        }
        if (rc != 0) {
            XX_LOGE(
                "{}Builtin plugin `{}` create failed (code={}), performing rollback",
                logTag(),
                inst->name,
                rc
            );
            rollbackLoad(inst, /*closeHandle=*/false);
            co_return nullptr;
        }

        std::string startError;
        if (!co_await awaitPluginLifecycle(
                this->runtime(),
                inst,
                inst->pluginCtx,
                inst->lifecycleStart,
                "plugin start",
                startError
            )) {
            XX_LOGE("{}Builtin plugin `{}` start failed: {}", logTag(), inst->name, startError);
            rollbackLoad(inst, /*closeHandle=*/false);
            co_return nullptr;
        }
        inst->lifecycleStarted = true;

        finishLoad(inst, resources);
        XX_LOGI("{}Builtin plugin `{}` loaded successfully", logTag(), inst->name);
        co_return inst;
    }

    /// 按路径加载插件 (协程): 支持目录 (读 `plugin.yaml`, 取依赖与资源声明)、
    /// `builtin://<name>` 内置简写、以及直接指向动态库的路径。
    ///
    /// - 依赖检查先于装载: 必选依赖未加载直接失败, 可选依赖仅警告;
    /// - `builtin://` 且内置表未命中时回退为可执行目录/工作目录下的同名目录插件;
    /// - `allowMissingEntry=true` 时缺少本端入口符号按静默跳过处理 (另一端的插件)。
    asio::awaitable<InstancePtr> loadPluginAsync(
        std::string              path,
        const PluginLoadOptions* options           = nullptr,
        bool                     allowMissingEntry = false
    ) {
        // 内置简写: builtin://<name> 直接经内置注册表加载 (无需外部目录/文件)
        if (isBuiltinScheme(path)) {
            auto btName = parseBuiltinName(path);
            if (btName.empty()) {
                XX_LOGE("{}Plugin load failed: invalid builtin path `{}`", logTag(), path);
                co_return nullptr;
            }
            // 尝试从默认插件目录解析 manifest 以获取 depends/interfaces/resources
            // (可选: 失败则按无依赖/无资源处理, 不影响内置核心加载)
            std::vector<std::string> depends, optionalDepends;
            PluginManifestResources  resources;
            PluginManifestInterfaces interfaces;
            // 按可执行目录与当前工作目录探测 manifest (与内置合并模式资源拷贝布局一致)
            {
                std::string dummyName, dummyEntry;
                // 优先内嵌清单 (单文件分发, 无需外部 plugin.yaml)
                if (parseBuiltinManifest(
                        btName,
                        dummyName,
                        dummyEntry,
                        depends,
                        optionalDepends,
                        &resources,
                        &interfaces
                    )) {
                    // 命中内嵌清单
                } else {
                    // 跨平台探测: 优先 exe 目录 (安装布局) 其次 cwd (开发布局)
                    for (const auto& base : pluginProbeBases()) {
                        auto probe = base / "plugins" / btName / "plugin.yaml";
                        if (std::filesystem::exists(probe)) {
                            if (parsePluginManifest(
                                    probe.parent_path(),
                                    dummyName,
                                    dummyEntry,
                                    depends,
                                    optionalDepends,
                                    &resources,
                                    &interfaces
                                )) {
                                break;
                            }
                        }
                    }
                }
            }
            // 若内置注册表中不存在, 回退为普通目录插件加载 (非合并编译时
            // builtin:// 仍可指向外部目录插件, 保持兼容)
            if (!findBuiltinPlugin(btName)) {
                // 按目录插件路径重新进入常规加载分支 (跨平台: exe 目录优先)
                std::filesystem::path fallback;
                {
                    auto exeDir = getExecutableDirPath();
                    if (!exeDir.empty()) {
                        auto cand = exeDir / "plugins" / btName;
                        if (std::filesystem::is_directory(cand)) {
                            fallback = cand;
                        }
                    }
                }
                if (fallback.empty()) {
                    auto cand = std::filesystem::current_path() / "plugins" / btName;
                    if (std::filesystem::is_directory(cand)) {
                        fallback = cand;
                    }
                }
                if (!fallback.empty()) {
                    XX_LOGI(
                        "{}Builtin plugin `{}` not in registry, fallback to directory `{}`",
                        logTag(),
                        btName,
                        fallback.string()
                    );
                    co_return co_await loadPluginAsync(fallback.string(), options, allowMissingEntry);
                }
            }
            for (const auto& dep : depends) {
                if (!this->find(dep)) {
                    XX_LOGE(
                        "{}Builtin plugin `{}` load failed: required dependency `{}` not installed",
                        logTag(),
                        btName,
                        dep
                    );
                    co_return nullptr;
                }
            }
            for (const auto& dep : optionalDepends) {
                if (!this->find(dep)) {
                    XX_LOGW(
                        "{}Builtin plugin `{}` optional dependency `{}` not installed",
                        logTag(),
                        btName,
                        dep
                    );
                }
            }
            co_return co_await loadBuiltinAsync(
                btName,
                path,
                depends,
                optionalDepends,
                options,
                resources,
                interfaces
            );
        }

        namespace fs = std::filesystem;
        fs::path p(path);
        if (fs::is_directory(p)) {
            auto manifestPath = p / "plugin.yaml";
            if (fs::exists(manifestPath)) {
                std::string              manifestName, manifestEntry;
                std::vector<std::string> depends, optionalDepends;
                PluginManifestResources  resources;
                PluginManifestInterfaces interfaces;
                if (!parsePluginManifest(
                        p,
                        manifestName,
                        manifestEntry,
                        depends,
                        optionalDepends,
                        &resources,
                        &interfaces
                    )) {
                    XX_LOGE("{}Parse manifest `{}` failed", logTag(), manifestPath.string());
                    co_return nullptr;
                }

                for (const auto& dep : depends) {
                    if (!this->find(dep)) {
                        XX_LOGE(
                            "{}Plugin `{}` load failed: required dependency `{}` not installed "
                            "(load it first)",
                            logTag(),
                            manifestName,
                            dep
                        );
                        co_return nullptr;
                    }
                }
                for (const auto& dep : optionalDepends) {
                    if (!this->find(dep)) {
                        XX_LOGW(
                            "{}Plugin `{}` optional dependency `{}` not installed",
                            logTag(),
                            manifestName,
                            dep
                        );
                    }
                }

                if (findBuiltinPlugin(manifestName) != nullptr) {
                    co_return co_await loadBuiltinAsync(
                        manifestName,
                        manifestPath.string(),
                        depends,
                        optionalDepends,
                        options,
                        resources,
                        interfaces
                    );
                }

                std::string binPath = resolvePluginEntryPath(p, manifestEntry);
                if (!fs::exists(binPath)) {
                    binPath = defaultPluginLibraryPath(p, manifestName);
                }

                auto inst = co_await loadNativeAsync(
                    binPath,
                    options,
                    allowMissingEntry,
                    resources,
                    interfaces
                );
                if (inst) {
                    inst->depends         = std::move(depends);
                    inst->optionalDepends = std::move(optionalDepends);
                }
                co_return inst;
            }
        }

        co_return co_await loadNativeAsync(path, options, allowMissingEntry);
    }

    /// 卸载插件 (按名; 协程): 级联卸载依赖者 → 摘除注册 → 补齐 stop →
    /// 等 inflight 归零 → destroy/dlclose → 移出实例表。
    /// - `timeout` 是本次卸载的等待上限 (与级联的各实例共享同一截止时刻);
    /// - 超时返回 false 且实例保留 (状态 CloseFailed), 可再次调用重试。
    asio::awaitable<bool> unloadAsync(
        std::string_view          name,
        std::chrono::milliseconds timeout = std::chrono::seconds{30}
    ) {
        const auto deadline = std::chrono::steady_clock::now()
                              + std::max(timeout, std::chrono::milliseconds::zero());
        co_return co_await unloadAsyncUntil(std::string{name}, deadline);
    }

    /// 逐个卸载全部插件并等待安全关闭 (协程; 各实例共享同一超时时刻)
    asio::awaitable<bool> shutdownAsync(std::chrono::milliseconds timeout = std::chrono::seconds{30}
    ) {
        co_return co_await this->shutdownAllAsync(
            timeout,
            [this](const std::string& name, std::chrono::steady_clock::time_point deadline) {
                return unloadAsyncUntil(name, deadline);
            }
        );
    }

    /// 同步卸载全部插件 (进程退出/owner 析构路径)
    /// - 不等未返回的插件回调: 调用方须保证没有尚未返回的插件回调;
    /// - 仍有活动 lease 的实例保留上下文与动态库 (登记空闲收尾), 可稍后由
    ///   [shutdownAsync] 重试;
    /// - stop 事务未完成的实例标记 CloseFailed 并保留, 交由仍存活的 owner 收尾。
    void shutdownAll() {
        std::vector<std::string> names;
        names.reserve(this->plugins_.size());
        for (const auto& [name, inst] : this->plugins_) {
            (void)inst;
            names.push_back(name);
        }
        for (const auto& name : names) {
            auto inst = this->find(name);
            if (inst) {
                shutdownPlugin(inst);
            }
        }
        // shutdownPlugin 对仍有 lease 的实例会保留上下文和动态库；不能无条件
        // 清空实例表，否则最后一个 lease 释放后将失去可重试的关闭入口。
    }

    /// 禁用插件 (级联禁用依赖者; 摘除注册; 投递 stop 撤销插件自管资源)
    void disable(std::string_view name) {
        disableImpl(name, /*userInitiated=*/true);
    }

    /// 启用插件 (先启用必选依赖, 再投递 start 让插件重新声明注册)
    void enable(std::string_view name) {
        enableImpl(name, /*userInitiated=*/true);
    }

    /// 摘除实例在宿主侧的**全部**注册 (通用登记 + 领域注册 + 实例专属资源所有权),
    /// 但保留实例内的注册记录 (启用时由 start 事务重新声明)。
    void detachInstanceRegistrations(InstanceT* inst) {
        if (!inst) {
            return;
        }
        detachAll(inst);
        detachDomainOwnedResources(inst);
    }

    /// 摘除实例的注册 (通用部分) 并回调宿主摘除领域注册
    ///
    /// - 通用部分: 取消未终结的 Operation、撤销事件订阅、撤销能力声明;
    /// - 领域部分经 [detachDomainRegistrations] 交给宿主 (工具/权限/钩子/图/提示词等)。
    void detachAll(InstanceT* inst) {
        if (!inst) {
            return;
        }

        /// 只请求取消，不删除活跃记录；终态提交负责唯一一次清理。
        /// 回调可能登记其他操作，因此遍历当前快照，避免 vector 迭代器失效。
        const auto operations = inst->outstandingOps;
        for (const auto& op : operations) {
            if (op && !op->completed.load(std::memory_order_acquire) && op->cancelFn) {
                op->cancelFn();
            }
        }

        // 事件订阅登记与能力声明由宿主核心按通用表登记撤销 (内核侧实现)
        this->revokeInstanceSubscriptions(inst);
        this->unregisterInstanceCapabilities(inst);

        detachDomainRegistrations(inst);
    }

    /// 清空"由插件 start 事务重新声明"的注册记录 (工具/权限/钩子/图/能力/订阅)。
    /// stop 成功后调用，避免下次 start 在旧记录上重复累积。
    void clearPluginOwnedRegistrations(InstanceT* inst) {
        if (!inst) {
            return;
        }
        inst->subscriptions.clear();
        inst->subscriptionHandles.clear();
        inst->capabilityRegistrations.clear();
        clearDomainRegistrations(inst);
    }

    /// 禁用/启用事务的异步收尾 (仅 IO 线程)，供宿主在需要自行投递时调用:
    /// - [stopForDisable]: 调用插件 stop 导出，撤销插件自管资源 (订阅/线程/定时器);
    ///   失败只记录日志并保持 Disabled (可再次 disable/enable 重试)。
    /// - [startForEnable]: 先补齐欠着的 stop，再调用插件 start 重新注册；
    ///   成功后状态回到 Ready。
    asio::awaitable<void> stopForDisable(InstancePtr inst) {
        if (!inst || !inst->lifecycleStopPending()) {
            co_return;
        }
        // 实例已经开始关闭时, stop 由卸载路径负责补齐；这里再发一次会与
        // unload 的 stop/destroy 交错。
        if (inst->lifetime && inst->lifetime->closeRequested()) {
            co_return;
        }
        if (inst->enabled) {
            // 等待期间用户又启用了该插件：本次 stop 作废。
            co_return;
        }
        std::string error;
        if (!co_await awaitPluginLifecycle(
                this->runtime(),
                inst,
                inst->pluginCtx,
                inst->lifecycleStop,
                "plugin stop",
                error
            )) {
            XX_LOGE(
                "{}Plugin `{}` stop failed while disabling: {}",
                logTag(),
                inst->name,
                error
            );
            co_return;
        }
        inst->lifecycleStopped = true;
        // stop 成功: 插件侧注册已撤销，宿主侧记录同步清空，使下次 start 从干净状态
        // 重新声明，避免同一工具/能力在多次 enable/disable 后重复累积。
        clearPluginOwnedRegistrations(inst.get());
        XX_LOGI("{}Plugin `{}` stopped for disable", logTag(), inst->name);
        co_return;
    }

    asio::awaitable<void> startForEnable(InstancePtr inst) {
        if (!inst || !inst->lifecycleStart) {
            co_return;
        }
        // enable 事务可能落后于 unload: 关闭/已关闭的实例不得被重新置为 Ready
        // (InstanceLifetime::setState 明确禁止 Closed 重新打开)。
        if (!inst->lifetime || inst->lifetime->closeRequested()
            || inst->lifetime->state() == PluginInstanceState::Closed) {
            co_return;
        }
        if (!inst->enabled) {
            co_return;
        }
        // disable 的 stop 事务可能还在排队或正在执行：必须先停干净再 start，
        // 否则新注册会叠加在未撤销的旧状态之上。
        if (inst->lifecycleStopPending()) {
            std::string stopError;
            if (!co_await awaitPluginLifecycle(
                    this->runtime(),
                    inst,
                    inst->pluginCtx,
                    inst->lifecycleStop,
                    "plugin stop",
                    stopError
                )) {
                if (inst->lifetime) {
                    inst->lifetime->setState(PluginInstanceState::Disabled);
                }
                XX_LOGE(
                    "{}Plugin `{}` enable aborted, stop failed: {}",
                    logTag(),
                    inst->name,
                    stopError
                );
                co_return;
            }
            inst->lifecycleStopped = true;
            clearPluginOwnedRegistrations(inst.get());
        }
        if (!inst->enabled) {
            // 等待 stop 期间用户又禁用了该插件。
            co_return;
        }
        if (inst->lifetime->closeRequested()
            || inst->lifetime->state() == PluginInstanceState::Closed) {
            // 等待 stop 期间实例开始关闭: 不再 start。
            co_return;
        }

        std::string error;
        if (!co_await awaitPluginLifecycle(
                this->runtime(),
                inst,
                inst->pluginCtx,
                inst->lifecycleStart,
                "plugin start",
                error
            )) {
            // start 失败: 回到 Disabled 且不留部分注册；实例保持"stop 仍欠着"，
            // 卸载或下次启用时先 stop 清理, 保证回滚顺序为"先撤销注册, 再释放资源"。
            inst->enabled               = false;
            inst->blockedByDependencies = false;
            if (inst->lifetime && !inst->lifetime->closeRequested()) {
                inst->lifetime->setState(PluginInstanceState::Disabled);
            }
            detachInstanceRegistrations(inst.get());
            clearPluginOwnedRegistrations(inst.get());
            XX_LOGE(
                "{}Plugin `{}` start failed while enabling: {}",
                logTag(),
                inst->name,
                error
            );
            co_return;
        }
        inst->lifecycleStopped = false;
        onInstanceEnabledChanged(*inst, true);
        if (inst->lifetime && !inst->lifetime->closeRequested()) {
            inst->lifetime->setState(PluginInstanceState::Ready);
        }
        XX_LOGI("{}Plugin `{}` restarted after enable", logTag(), inst->name);
        co_return;
    }

    /// owner 析构前自检: 仍有未安全关闭的实例时给出明确日志
    /// (调用方应先 await shutdownAsync, 否则插件上下文与动态库会被保留到进程退出)
    void warnPendingCloseOnDestroy(std::string_view owner) const {
        if (this->hasPendingClose()) {
            XX_LOGW(
                "{}PluginManager (`{}`) destroyed with pending plugin shutdown; owner should "
                "await shutdownAsync() before stopping its IO executor",
                logTag(),
                owner
            );
        }
    }

protected:

    // =====================================================================
    // 宿主接缝 (宿主必须实现的纯虚函数)
    // =====================================================================

    /// 管理器自身的强引用 (派生类直接返回 `shared_from_this()`)
    ///
    /// 为什么需要它: 停用/启用事务与空闲收尾是异步的，需要在此期间保活管理器;
    /// 框架内核不能自己继承 `enable_shared_from_this` —— 与宿主管理器的
    /// `enable_shared_from_this<ManagerT>` 会形成多基类歧义 (`weak_this` 都不被初始化)。
    virtual std::shared_ptr<PluginHostLifecycle<InstanceT>> selfRef() = 0;

    /// 生成领域实例对象 (只做 `make_shared<MyInstance>(name)` 与领域自引用设置)
    ///
    /// 基类随后会补齐: 基类自引用 (`ownerSelf`)、版本/描述/路径、生命周期入口、
    /// 生命周期控制块 ([InstanceLifetime]) 与交给自己插件的 host 控制块。
    virtual InstancePtr createInstance(std::string name) = 0;

    /// 交给插件的宿主 vtable (进程内稳定静态表; 见 `AgentxxHostVtable`)
    virtual const AgentxxHostVtable* hostVtable() = 0;

    // =====================================================================
    // 宿主接缝 (可选覆写)
    // =====================================================================

    /// 同进程可能同时存在多个宿主 (如 client + agent), 日志前缀用于区分
    virtual std::string_view logTag() const noexcept {
        return {};
    }

    /// 摘除实例的**领域**注册 (工具/权限/钩子/图节点/提示词贡献 等)
    /// - 调用时机: 禁用与卸载; 只摘除宿主侧生效的注册, 保留实例内的注册记录
    virtual void detachDomainRegistrations(InstanceT* inst) {
        (void)inst;
    }

    /// 摘除实例专属资源的所有权 (中间件句柄/清单资源所有权 等)
    /// - 调用时机: 禁用与卸载 (在 [detachDomainRegistrations] 之后)
    virtual void detachDomainOwnedResources(InstanceT* inst) {
        (void)inst;
    }

    /// 清空"由插件 start 事务重新声明"的**领域**注册记录 (工具名/权限/hook/图)
    /// - 调用时机: stop 成功后; 使下次 start 从干净状态重新声明
    virtual void clearDomainRegistrations(InstanceT* inst) {
        (void)inst;
    }

    /// 应用清单声明的资源 (skill/memory/mcp) 并冻结资源声明
    /// - 调用时机: create + start 都成功之后、状态置 Ready 之前
    virtual void applyDeclaredResources(InstanceT& inst, const PluginManifestResources& resources) {
        (void)inst;
        (void)resources;
    }

    /// 卸载时释放实例级资源 (内置工具对象列表 / 清单资源所有权 等)
    virtual void releaseInstanceResources(InstanceT& inst) {
        (void)inst;
    }

    /// 实例启用状态变化通知 (禁用时 false, start 成功后 true)
    virtual void onInstanceEnabledChanged(InstanceT& inst, bool enabled) {
        (void)inst;
        (void)enabled;
    }

    /// 实例加载完成 (状态置 Ready 之前) 的宿主收尾
    virtual void onInstanceLoaded(InstanceT& inst) {
        (void)inst;
    }

    /// 实例即将从插件表摘除 (卸载收尾) 的宿主收尾
    virtual void onInstanceUnloaded(InstanceT& inst) {
        (void)inst;
    }

    /// 卸载级联时是否只统计"启用中"的依赖者
    /// - false (默认): 全部依赖者一起卸载 (与 agent 侧一致)
    /// - true: 已禁用的依赖者保持加载 (client 侧既有行为)
    virtual bool cascadeUnloadEnabledOnly() const noexcept {
        return false;
    }

    // =====================================================================
    // 内部实现
    // =====================================================================

    /// 装载选项落到实例字段 (args / configPath)
    void applyLoadOptions(InstanceT& inst, const PluginLoadOptions* options) {
        if (options) {
            inst.args       = options->args;
            inst.configPath = options->configPath;
        }
    }

    /// 装配实例的通用部分 (宿主的自定义装载路径也应调用它)
    ///
    /// - 生命周期入口 (start/stop);
    /// - 基类自引用: 通用表实现与完成回调只依赖 `ownerSelf` (不依赖管理器类型);
    /// - 生命周期控制块 (状态机 + 执行 lease); `makeLifetime` 同时写入实例的运行时弱引用;
    /// - 交给插件的宿主控制块: host 视图必须在进程级稳定地址上, 插件卸载后继续使用
    ///   旧指针时各 vtable 入口只会安全失败 (tombstone 语义, 见 instance_base.h)。
    void attachInstance(
        const InstancePtr&         inst,
        const AgentxxPluginStartFn startFn,
        const AgentxxPluginStopFn  stopFn
    ) {
        if (!inst) {
            return;
        }
        inst->lifecycleStart = startFn;
        inst->lifecycleStop  = stopFn;
        inst->ownerSelf      = inst;
        inst->lifetime       = this->makeLifetime(inst);
        inst->hostControl    = PluginHostControl::create(inst, hostVtable());
    }

    /// 探测 plugin.yaml 的基准目录 (可执行目录优先, 其次当前工作目录)
    static std::vector<std::filesystem::path> pluginProbeBases() {
        std::vector<std::filesystem::path> bases;
        auto                               exeDir = getExecutableDirPath();
        if (!exeDir.empty()) {
            bases.push_back(exeDir);
        }
        bases.push_back(std::filesystem::current_path());
        return bases;
    }

    /// manifest entry 找不到时的平台默认库文件名 (lib<name>.so / name.dll / lib<name>.dylib)
    static std::string defaultPluginLibraryPath(
        const std::filesystem::path& dir,
        const std::string&           name
    ) {
#if XX_IS_WIN_D
        auto p = dir / (name + ".dll");
        if (std::filesystem::exists(p)) {
            return p.string();
        }
        return (dir / ("lib" + name + ".dll")).string();
#elif XX_IS_MACOS_D
        return (dir / ("lib" + name + ".dylib")).string();
#else
        return (dir / ("lib" + name + ".so")).string();
#endif
    }

    /// 插件实例的公共装配 (两种加载路径共用): 元信息/生命周期入口/宿主控制块
    InstancePtr makeInstance(
        std::string                name,
        const AgentxxPluginInfo*   info,
        std::string                path,
        const AgentxxPluginStartFn startFn,
        const AgentxxPluginStopFn  stopFn
    ) {
        auto inst = createInstance(std::move(name));
        if (!inst) {
            return nullptr;
        }
        inst->version = info && info->version.data ? std::string(info->version.data, info->version.size)
                                                   : "1.0.0";
        inst->description = info && info->description.data
                                ? std::string(info->description.data, info->description.size)
                                : "";
        inst->path        = std::move(path);
        attachInstance(inst, startFn, stopFn);
        return inst;
    }

    /// create + start 都成功后的公共收尾: 应用声明式资源、宿主收尾、置 Ready。
    void finishLoad(const InstancePtr& inst, const PluginManifestResources& resources) {
        applyDeclaredResources(*inst, resources);
        onInstanceLoaded(*inst);
        inst->lifetime->setState(PluginInstanceState::Ready);
        this->releasePluginName(inst->name);
    }

    /// 加载失败 / start 失败的统一回滚: 摘除宿主侧注册 → 释放资源 → 销毁插件上下文
    /// → 移出插件表 → 释放名称预占 (动态库句柄由调用方决定是否关闭)。
    void rollbackLoad(const InstancePtr& inst, bool closeHandle) {
        if (!inst) {
            return;
        }
        detachInstanceRegistrations(inst.get());
        releaseInstanceResources(*inst);
        inst->destroyPlugin();
        this->plugins_.erase(inst->name);
        this->releasePluginName(inst->name);
        if (closeHandle && inst->dlHandle) {
            NativeLoader::close(inst->dlHandle);
            inst->dlHandle = nullptr;
        }
    }

    /// 同步关闭单个实例 (级联卸载依赖者)
    void shutdownPlugin(const InstancePtr& inst) {
        if (!inst || inst->unloadRequested) {
            return;
        }
        inst->unloadRequested = true;
        if (inst->lifetime) {
            inst->lifetime->requestClose();
        }
        for (const auto& dep :
             collectReverseRequiredDeps(this->plugins_, inst->name, /*onlyEnabled=*/false)) {
            auto depInst = this->find(dep);
            if (depInst && !depInst->unloadRequested) {
                shutdownPlugin(depInst);
            }
        }
        detachInstanceRegistrations(inst.get());
        releaseInstanceResources(*inst);
        // stop 事务尚未完成时不能 destroy/dlclose：同步路径无法等待该事务，
        // 只能保留实例并标记 CloseFailed，等待 shutdownAsync/unloadAsync 收尾。
        if (inst->lifecycleStopPending()) {
            if (inst->lifetime) {
                inst->lifetime->setState(PluginInstanceState::CloseFailed);
            }
            XX_LOGE(
                "{}Plugin `{}` shutdown deferred: lifecycle stop pending; call shutdownAsync "
                "before destroying the owner",
                logTag(),
                inst->name
            );
            return;
        }

        // 同步析构路径不能绕过运行中的 lease。注册已撤销，但插件上下文和动态库
        // 必须保留到所有已接受的 Operation/回调返回；完成回调随后可再次调用
        // unloadAsync 继续收尾。这里不强行 destroy，也不清空实例记录。
        if (inst->lifetime && inst->lifetime->leaseCount() != 0) {
            XX_LOGW(
                "{}Plugin shutdown deferred: `{}` still has {} active lease(s)",
                logTag(),
                inst->name,
                inst->lifetime->leaseCount()
            );
            deferInstanceShutdown(inst);
            return;
        }

        inst->destroyPlugin();
        if (!inst->pluginDestroyed) {
            XX_LOGW(
                "{}Plugin shutdown deferred after destroy attempt: `{}`",
                logTag(),
                inst->name
            );
            return;
        }
        if (inst->lifetime) {
            inst->lifetime->setState(PluginInstanceState::Closed);
        }
        onInstanceUnloaded(*inst);
        this->plugins_.erase(inst->name);
        XX_LOGI("{}Plugin shutdown: {}", logTag(), inst->name);
    }

    /// 同步 owner 即将析构时，保留实例和 DSO 到最后一个 lease 释放。
    /// cleanup 在 lifetime 所属 IO 线程执行；若 manager 已析构，弱引用为空，
    /// 仍会完成 plugin destroy 并由最后一个 shared_ptr 安全关闭 DSO。
    void deferInstanceShutdown(const InstancePtr& inst) {
        if (!inst || !inst->lifetime) {
            return;
        }
        std::weak_ptr<PluginManagerBase<InstanceT>> weakBase = selfRef();
        if (!inst->lifetime->setIdleCleanup([inst, weakBase] {
                if (inst->lifecycleStopPending()) {
                    // stop 事务只能在 IO 线程上协作完成；idle 回调是同步上下文，
                    // 不能在这里 begin/await。保留实例与动态库并保持 CloseFailed，
                    // 交给仍存活的 owner 经 shutdownAsync 收尾。
                    inst->lifetime->setState(PluginInstanceState::CloseFailed);
                    XX_LOGE(
                        "Plugin `{}` idle cleanup: lifecycle stop still pending; keeping "
                        "context and DSO",
                        inst->name
                    );
                    return;
                }
                if (!inst->destroyPlugin()) {
                    XX_LOGE("Plugin `{}` idle cleanup still has active leases", inst->name);
                    return;
                }
                inst->lifetime->setState(PluginInstanceState::Closed);
                // 管理器仍存活时释放同一实例的名称预占，使后续加载可以重试。
                // manager 已析构时 weak_ptr 为空，实例会在 cleanup 返回后自然释放。
                if (auto base = weakBase.lock()) {
                    auto it = base->plugins_.find(inst->name);
                    if (it != base->plugins_.end() && it->second == inst) {
                        base->plugins_.erase(it);
                    }
                }
            })) {
            XX_LOGW("{}Plugin `{}` already has an idle cleanup", logTag(), inst->name);
        }
    }

    /// 禁用/启用事务的内部实现（级联递归用）：
    /// - `userInitiated=true` 表示用户显式操作，会更新 `userDisabled`；
    /// - 级联（false）只维护 `blockedByDependencies`，不覆盖用户显式禁用标记。
    /// 依赖级联按直接依赖者递归，覆盖三级/菱形依赖。
    void disableImpl(std::string_view name, bool userInitiated) {
        auto inst = this->find(name);
        if (!inst || !inst->enabled) {
            return;
        }
        if (inst->lifetime && inst->lifetime->closeRequested()) {
            // 已进入关闭流程: 不再接受启用状态变化，避免与 stop/destroy 交错。
            return;
        }
        if (userInitiated) {
            inst->userDisabled          = true;
            inst->blockedByDependencies = false;
        } else {
            // 级联禁用: 只记录原因，不改写用户显式禁用标记。
            inst->blockedByDependencies = true;
        }
        inst->enabled = false;
        if (inst->lifetime) {
            inst->lifetime->setState(PluginInstanceState::Disabled);
        }
        detachInstanceRegistrations(inst.get());
        XX_LOGI(
            "{}Plugin `{}` disabled ({})",
            logTag(),
            inst->name,
            userInitiated ? "user" : "dependency"
        );

        // 级联禁用依赖者: 只收集直接依赖者，再逐层递归，覆盖三级/菱形依赖。
        for (const auto& child :
             collectReverseRequiredDeps(this->plugins_, inst->name, /*onlyEnabled=*/true)) {
            disableImpl(child, /*userInitiated=*/false);
        }

        // start/stop 事务（R5）：导出 stop 的插件必须收到 stop 才能撤销自管资源
        // （订阅/线程/定时器）；同步入口把事务投递到本管理器 IO executor，
        // 完成后状态保持 Disabled。
        requestStopForDisable(inst);
    }

    void enableImpl(std::string_view name, bool userInitiated) {
        auto inst = this->find(name);
        if (!inst || inst->enabled) {
            return;
        }
        if (inst->lifetime && inst->lifetime->closeRequested()) {
            return;
        }
        if (!userInitiated && inst->userDisabled) {
            // 用户显式禁用的插件不被级联恢复。
            return;
        }
        // 先置位再递归: 既让注册复查通过，也让循环依赖不会无限递归。
        inst->enabled = true;
        if (userInitiated) {
            inst->userDisabled = false;
        }
        inst->blockedByDependencies = false;
        if (inst->lifetime) {
            inst->lifetime->setState(PluginInstanceState::Ready);
        }

        // 先启用必选依赖: 子插件的 start 需要父插件的能力/工具已经可用。
        for (const auto& dep : inst->depends) {
            enableImpl(dep, /*userInitiated=*/false);
        }

        // 注册由插件 start 事务重新声明: 宿主只负责投递并处理结果 (失败则回到 Disabled)。
        requestStartForEnable(inst);
        XX_LOGI(
            "{}Plugin `{}` enabled ({})",
            logTag(),
            inst->name,
            userInitiated ? "user" : "dependency"
        );

        // 级联恢复因本插件被禁用的依赖者（不覆盖用户显式禁用）。
        for (const auto& child :
             collectReverseRequiredDeps(this->plugins_, inst->name, /*onlyEnabled=*/false)) {
            enableImpl(child, /*userInitiated=*/false);
        }
    }

    /// 按需投递禁用事务：只有"已 start 且尚未 stop"的实例需要 stop。
    void requestStopForDisable(const InstancePtr& inst) {
        if (!inst || !inst->lifecycleStopPending()) {
            return;
        }
        auto self = selfRef();
        try {
            asio::co_spawn(
                this->ioExecutor(),
                [self, inst]() -> asio::awaitable<void> {
                    co_await self->stopForDisable(inst);
                },
                [inst, tag = std::string{logTag()}](std::exception_ptr e) {
                    if (!e) {
                        return;
                    }
                    try {
                        std::rethrow_exception(e);
                    } catch (const std::exception& ex) {
                        XX_LOGE(
                            "{}Plugin `{}` disable stop threw: {}",
                            tag,
                            inst->name,
                            ex.what()
                        );
                    } catch (...) {
                        XX_LOGE(
                            "{}Plugin `{}` disable stop threw unknown",
                            tag,
                            inst->name
                        );
                    }
                }
            );
        } catch (const std::exception& e) {
            XX_LOGW(
                "{}Plugin `{}` disable stop could not be scheduled: {}",
                logTag(),
                inst->name,
                e.what()
            );
        }
    }

    /// 按需投递启用事务（导出 start 的插件）。
    void requestStartForEnable(const InstancePtr& inst) {
        if (!inst || !inst->lifecycleStart) {
            return;
        }
        auto self = selfRef();
        try {
            asio::co_spawn(
                this->ioExecutor(),
                [self, inst]() -> asio::awaitable<void> {
                    co_await self->startForEnable(inst);
                },
                [inst, tag = std::string{logTag()}](std::exception_ptr e) {
                    if (!e) {
                        return;
                    }
                    try {
                        std::rethrow_exception(e);
                    } catch (const std::exception& ex) {
                        XX_LOGE(
                            "{}Plugin `{}` enable start threw: {}",
                            tag,
                            inst->name,
                            ex.what()
                        );
                    } catch (...) {
                        XX_LOGE(
                            "{}Plugin `{}` enable start threw unknown",
                            tag,
                            inst->name
                        );
                    }
                }
            );
        } catch (const std::exception& e) {
            XX_LOGW(
                "{}Plugin `{}` enable start could not be scheduled: {}",
                logTag(),
                inst->name,
                e.what()
            );
        }
    }

    asio::awaitable<bool>
        unloadAsyncUntil(std::string name, std::chrono::steady_clock::time_point deadline) {
        auto inst = this->find(name);
        if (!inst) {
            XX_LOGW("{}Plugin unload: `{}` not loaded", logTag(), name);
            co_return false;
        }
        if (inst->unloadRequested) {
            // 同步 shutdownAll 可能已经摘除注册并登记了 idle cleanup；
            // 在 executor 仍运行时允许 shutdownAsync 接管这次关闭。
            if (!inst->lifetime || !inst->lifetime->closeRequested()) {
                co_return false;
            }
            inst->unloadRequested = false;
        }
        inst->unloadRequested = true;
        if (inst->lifetime) {
            inst->lifetime->requestClose();
        }

        for (const auto& dep : collectReverseRequiredDeps(
                 this->plugins_,
                 inst->name,
                 cascadeUnloadEnabledOnly()
             )) {
            auto depInst = this->find(dep);
            if (depInst) {
                if (!co_await unloadAsyncUntil(depInst->name, deadline)) {
                    co_return false;
                }
            }
        }

        detachInstanceRegistrations(inst.get());
        releaseInstanceResources(*inst);

        if (inst->lifecycleStopPending()) {
            std::string stopError;
            if (!co_await awaitPluginLifecycle(
                    this->runtime(),
                    inst,
                    inst->pluginCtx,
                    inst->lifecycleStop,
                    "plugin stop",
                    stopError
                )) {
                if (inst->lifetime) {
                    inst->lifetime->setState(PluginInstanceState::CloseFailed);
                }
                inst->unloadRequested = false;
                XX_LOGE("{}Plugin `{}` stop failed: {}", logTag(), inst->name, stopError);
                co_return false;
            }
            inst->lifecycleStopped = true;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(std::max(
            deadline - std::chrono::steady_clock::now(),
            std::chrono::steady_clock::duration::zero()
        ));
        bool       ok        = co_await this->waitInflightZero(inst, remaining);
        if (!ok) {
            if (inst->lifetime) {
                inst->lifetime->setState(PluginInstanceState::CloseFailed);
            }
            // 关闭已判定失败: 启动"驱动请求最后防线"。插件桥接正常会在自己的 stop
            // 事务里撤销排队请求 (见 pluginxx/runtime/driver.h 文件头), 这里撤销的是插件没能
            // 撤销的部分, 避免实例 lease 被永远占用 (lease 不归零就无法 destroy/dlclose)。
            const auto cancelled = inst->cancelPendingDrivers();
            if (cancelled > 0) {
                XX_LOGW(
                    "{}Plugin `{}` close rescue cancelled {} pending driver ticket(s)",
                    logTag(),
                    inst->name,
                    cancelled
                );
            }
            inst->unloadRequested = false;
            // 未终结 Operation 摘要：完成包可能已产生但没有投递到 IO 线程（executor
            // 停止时保留在待重放队列），这是 CloseFailed 的唯一可观察线索。
            const auto pending
                = this->runtime() ? this->runtime()->pendingOperationSummary() : std::string{};
            XX_LOGE(
                "{}Plugin `{}` unload timed out waiting for inflight callbacks (pending "
                "operations: {})",
                logTag(),
                inst->name,
                pending.empty() ? "none" : pending
            );
            co_return false;
        }

        inst->destroyPlugin();
        if (!inst->pluginDestroyed) {
            XX_LOGW(
                "{}Plugin `{}` unload deferred after destroy attempt",
                logTag(),
                inst->name
            );
            co_return false;
        }
        if (inst->lifetime) {
            inst->lifetime->setState(PluginInstanceState::Closed);
        }
        onInstanceUnloaded(*inst);
        this->plugins_.erase(name);
        XX_LOGI("{}Plugin `{}` unloaded", logTag(), name);
        co_return true;
    }
};

} // namespace pluginxx
