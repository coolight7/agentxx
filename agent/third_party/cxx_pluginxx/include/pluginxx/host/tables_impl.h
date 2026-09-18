/// pluginxx 通用表入口实现 (宿主侧 vtable 的通用部分, 与宿主领域无关)
///
/// 用法 (宿主侧 `query_interface` 实现):
/// ```cpp
/// const void* AGENTXX_PLUGIN_CALL xx_query_interface(
///     const AgentxxPluginHost*, const AgentxxPluginStringView* iid) {
///     ...
///     if (n == "__vtable") return &hostVtable();
///     if (const void* t = pluginxx::queryGenericPluginIface<MyInstance, MyManager>(n)) return t;
///     // 领域表 (tools / permission / hooks / session / model / prompt / resources / graph)
///     ...
/// }
/// ```
///
/// 覆盖 10 张通用表: log / json / config / plugins / events / scheduler /
/// coroutine_runtime / tasks / cancel / capabilities。入口体只做三件事:
/// 解析宿主控制块 (取实例/管理器 + admission lease) → 投递到 IO 线程 → 调用
/// [PluginHostCore] 的同名方法; 领域数据一律由 [DomainHooks] 提供。
///
/// 线程约定: 入口可在任意线程调用 (内部投递并同步等待); 表结构体首次查询时构造,
/// 之后只读, 因此可安全被多个插件线程并发查询。
#pragma once

#include "pluginxx/api/tables.h"
#include "pluginxx/host/abi_util.h"
#include "pluginxx/host/host_core.h"
#include "pluginxx/runtime/driver.h"
#include "pluginxx/runtime/instance_base.h"
#include "pluginxx/runtime/manager_base.h"
#include "pluginxx/runtime/op_driver.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "fmt/format.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace pluginxx {
namespace detail {

/// 注册/写入类入口的公共骨架: 解析 host 上下文 → 把业务逻辑投递到 IO 线程执行。
///
/// - 参数视图只在本次调用内有效, 因此闭包必须按值捕获自己需要的拷贝;
/// - `fn` 的入参是实例与管理器 (投递期间由 `keep` 保活, 含 admission lease);
///   业务参数校验由 `fn` 自己完成, 失败返回非 0 状态码。
template<typename InstanceT, typename ManagerT, typename Fn>
int32_t onInstanceIo(const AgentxxPluginHost* host, Fn&& fn) {
    return guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterPluginHost<InstanceT, ManagerT>(host);
        if (!call.ok()) {
            return -1;
        }
        auto keep = call; // 投递期间持实例/管理器强引用与 admission lease
        return ioCallSyncKeep<int32_t>(
            keep,
            keep.manager(),
            [keep, fn = std::forward<Fn>(fn)]() -> int32_t {
                return fn(keep.instance(), keep.manager());
            }
        );
    });
}

/// 只读查询类入口的公共骨架 (允许关闭中查询):
/// 在 IO 线程取字符串结果 → 经 host->alloc 写入 `out`; 结果为空按失败返回 -1。
template<typename InstanceT, typename ManagerT, typename Fn>
int32_t queryStringIo(const AgentxxPluginHost* host, AgentxxPluginString* out, Fn&& fn) {
    if (!out) {
        return -1;
    }
    return guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterPluginHost<InstanceT, ManagerT>(host, /*allowClosing=*/true);
        if (!call.ok()) {
            return -1;
        }
        auto keep = call;
        auto text = ioCallSyncKeep<std::string>(
            keep,
            keep.manager(),
            [keep, fn = std::forward<Fn>(fn)]() -> std::string {
                return fn(keep.instance(), keep.manager());
            }
        );
        if (text.empty()) {
            return -1;
        }
        hostMemorySetString(out, text);
        return 0;
    });
}

} // namespace detail

/// 通用表入口集合。
///
/// `InstanceT` 须继承 [PluginInstanceBase]; `ManagerT` 须继承
/// [PluginHostCore<InstanceT>] (入口按同名方法调用, 由编译期约束保证形态正确)。
template<typename InstanceT, typename ManagerT>
struct GenericTableEntries {
    using HostCall = PluginHostCall<InstanceT, ManagerT>;

    static HostCall enterHost(const AgentxxPluginHost* host, bool allowClosing = false) {
        return enterPluginHost<InstanceT, ManagerT>(host, allowClosing);
    }

    // =====================================================================
    // log 表
    // =====================================================================

    static void AGENTXX_PLUGIN_CALL
        logEntry(const AgentxxPluginHost* host, int32_t level, const AgentxxPluginStringView* msg) {
        (void)host;
        using utilxx_base::LogLevel;
        LogLevel lv = LogLevel::Info;
        switch (level) {
            case 0:
                lv = LogLevel::Trace;
                break;
            case 1:
                lv = LogLevel::Debug;
                break;
            case 2:
                lv = LogLevel::Info;
                break;
            case 3:
                lv = LogLevel::Warn;
                break;
            case 4:
                lv = LogLevel::Error;
                break;
            default:
                break;
        }
        utilxx_base::xxLogPrint(lv, svToStr(msg));
    }

    // =====================================================================
    // json 表
    // =====================================================================

    static int32_t AGENTXX_PLUGIN_CALL jsonGetStringEntry(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* json,
        const AgentxxPluginStringView* key,
        AgentxxPluginString*           out
    ) {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto inst = call.instance();
        if (!inst || detail::abiViewEmpty(json) || detail::abiViewEmpty(key)) {
            return -1;
        }
        try {
            auto j = utilxx_base::Json::parse(svToStr(*json));
            const auto keyStr = svToStr(*key);
            if (j.is_object() && j.contains(keyStr) && j[keyStr].is_string()) {
                hostMemorySetString(out, j[keyStr].get<std::string>());
                return 0;
            }
        } catch (...) {
        }
        return -1;
    }

    static int32_t AGENTXX_PLUGIN_CALL jsonEscapeEntry(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* s,
        AgentxxPluginString*           out
    ) {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto inst = call.instance();
        if (!inst || detail::abiViewEmpty(s)) {
            return -1;
        }
        const auto src = svToSv(*s);
        std::string strOut;
        strOut.reserve(src.size() + 2);
        strOut += '"';
        for (const char raw : src) {
            const auto c = static_cast<unsigned char>(raw);
            switch (c) {
                case '"':
                    strOut += "\\\"";
                    break;
                case '\\':
                    strOut += "\\\\";
                    break;
                case '\b':
                    strOut += "\\b";
                    break;
                case '\f':
                    strOut += "\\f";
                    break;
                case '\n':
                    strOut += "\\n";
                    break;
                case '\r':
                    strOut += "\\r";
                    break;
                case '\t':
                    strOut += "\\t";
                    break;
                default:
                    if (c < 0x20) {
                        strOut += fmt::format("\\u{:04x}", c);
                    } else {
                        strOut += raw;
                    }
                    break;
            }
        }
        strOut += '"';
        hostMemorySetString(out, strOut);
        return 0;
    }

    // =====================================================================
    // config 表
    // =====================================================================

    static int32_t AGENTXX_PLUGIN_CALL
        configGetEntry(const AgentxxPluginHost* host, AgentxxPluginString* out) {
        return detail::queryStringIo<InstanceT, ManagerT>(
            host,
            out,
            [](InstanceT* inst, ManagerT* mgr) -> std::string {
                auto* hooks = mgr->domainHooks();
                (void)inst;
                return hooks ? hooks->configJson() : std::string{};
            }
        );
    }

    static int32_t AGENTXX_PLUGIN_CALL
        pluginArgsEntry(const AgentxxPluginHost* host, AgentxxPluginString* out) {
        return guardVtableCall(-1, [&]() -> int32_t {
            if (!out) {
                return -1;
            }
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst) {
                return -1;
            }
            auto keep = call;
            auto json = ioCallSyncKeep<std::string>(keep, mgr, [keep]() -> std::string {
                return keep.instance()->args.is_object() ? keep.instance()->args.dump()
                                                         : std::string{"{}"};
            });
            hostMemorySetString(out, json);
            return 0;
        });
    }

    static int32_t AGENTXX_PLUGIN_CALL toolPromptEntry(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* tool_name,
        AgentxxPluginString*           out
    ) {
        return guardVtableCall(-1, [&]() -> int32_t {
            if (!out) {
                return -1;
            }
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            if (!mgr || detail::abiViewEmpty(tool_name)) {
                return -1;
            }
            auto              keep = call;
            const std::string name = svToStr(*tool_name);
            auto              json = ioCallSyncKeep<std::string>(keep, mgr, [keep, name]() {
                auto* hooks = keep.manager()->domainHooks();
                return hooks ? hooks->toolPromptJson(name) : std::string{};
            });
            if (json.empty()) {
                return -1;
            }
            hostMemorySetString(out, json);
            return 0;
        });
    }

    static int32_t AGENTXX_PLUGIN_CALL sessionWorkDirEntry(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* thread_id,
        AgentxxPluginString*           out
    ) {
        return guardVtableCall(-1, [&]() -> int32_t {
            if (!out) {
                return -1;
            }
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst) {
                return -1;
            }
            auto              keep = call;
            const std::string tid  = thread_id ? svToStr(*thread_id) : std::string{};
            auto              dir  = ioCallSyncKeep<std::string>(keep, mgr, [keep, tid]() {
                auto* hooks = keep.manager()->domainHooks();
                return hooks ? hooks->sessionWorkDir(tid) : std::string{};
            });
            if (dir.empty()) {
                return -1;
            }
            hostMemorySetString(out, dir);
            return 0;
        });
    }

    static int32_t AGENTXX_PLUGIN_CALL
        pluginConfigPathEntry(const AgentxxPluginHost* host, AgentxxPluginString* out) {
        return detail::queryStringIo<InstanceT, ManagerT>(
            host,
            out,
            [](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)mgr;
                return inst ? inst->configPath : std::string{};
            }
        );
    }

    static int32_t AGENTXX_PLUGIN_CALL
        languageGetEntry(const AgentxxPluginHost* host, AgentxxPluginString* out) {
        return guardVtableCall(-1, [&]() -> int32_t {
            if (!out) {
                return -1;
            }
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            if (!mgr) {
                return -1;
            }
            auto keep = call;
            auto lang = ioCallSyncKeep<std::string>(keep, mgr, [keep]() {
                auto* hooks = keep.manager()->domainHooks();
                return hooks ? hooks->language() : std::string{};
            });
            if (lang.empty()) {
                lang = "en";
            }
            hostMemorySetString(out, lang);
            return 0;
        });
    }

    static int32_t AGENTXX_PLUGIN_CALL languageSetEntry(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* language
    ) {
        return guardVtableCall(-1, [&]() -> int32_t {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            if (!mgr) {
                return -1;
            }
            auto              keep = call;
            const std::string lang = language ? svToStr(*language) : std::string{};
            ioCallSyncVoidKeep(keep, mgr, [keep, lang]() {
                if (auto* hooks = keep.manager()->domainHooks()) {
                    hooks->setLanguage(lang);
                }
            });
            return 0;
        });
    }

    // =====================================================================
    // plugins 表
    // =====================================================================

    static int32_t AGENTXX_PLUGIN_CALL
        listPluginsEntry(const AgentxxPluginHost* host, AgentxxPluginString* out) {
        return detail::queryStringIo<InstanceT, ManagerT>(
            host,
            out,
            [](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)inst;
                auto* hooks = mgr->domainHooks();
                return hooks ? hooks->pluginsJson() : std::string{};
            }
        );
    }

    static int32_t AGENTXX_PLUGIN_CALL getPluginEntry(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* name,
        AgentxxPluginString*           out
    ) {
        return guardVtableCall(-1, [&]() -> int32_t {
            if (!out) {
                return -1;
            }
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            if (!mgr || detail::abiViewEmpty(name)) {
                return -1;
            }
            auto              keep      = call;
            const std::string pluginName = svToStr(*name);
            auto              json      = ioCallSyncKeep<std::string>(keep, mgr, [keep, pluginName]() {
                auto* hooks = keep.manager()->domainHooks();
                return hooks ? hooks->pluginJson(pluginName) : std::string{};
            });
            if (json.empty()) {
                return -1;
            }
            hostMemorySetString(out, json);
            return 0;
        });
    }

    static int32_t AGENTXX_PLUGIN_CALL
        ownInfoEntry(const AgentxxPluginHost* host, AgentxxPluginString* out) {
        return guardVtableCall(-1, [&]() -> int32_t {
            if (!out) {
                return -1;
            }
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst) {
                return -1;
            }
            auto              keep     = call;
            const std::string ownName  = inst->name;
            auto              json     = ioCallSyncKeep<std::string>(keep, mgr, [keep, ownName]() {
                auto* hooks = keep.manager()->domainHooks();
                return hooks ? hooks->pluginJson(ownName) : std::string{};
            });
            if (json.empty()) {
                return -1;
            }
            hostMemorySetString(out, json);
            return 0;
        });
    }

    // =====================================================================
    // events 表
    // =====================================================================

    static AgentxxPluginSubscription* AGENTXX_PLUGIN_CALL subscribeEntry(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* topic,
        void(AGENTXX_PLUGIN_CALL* handler)(const AgentxxPluginStringView* event_json, void* ud),
        void* ud
    ) {
        return guardVtableCall(nullptr, [&]() -> AgentxxPluginSubscription* {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || detail::abiViewEmpty(topic) || !handler) {
                return static_cast<AgentxxPluginSubscription*>(nullptr);
            }
            auto              keep      = call;
            const std::string topicCopy = svToStr(*topic);
            return ioCallSyncKeep<AgentxxPluginSubscription*>(
                keep,
                mgr,
                [keep, topicCopy, handler, ud]() {
                    return keep.manager()->subscribe(keep.instance(), topicCopy, handler, ud);
                }
            );
        });
    }

    static void AGENTXX_PLUGIN_CALL unsubscribeEntry(AgentxxPluginSubscription* sub) {
        guardVtableCallVoid([&] {
            unsubscribePluginSubscription(sub);
        });
    }

    static int32_t AGENTXX_PLUGIN_CALL publishEntry(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* topic,
        const AgentxxPluginStringView* event_json
    ) {
        return guardVtableCall(-1, [&]() -> int32_t {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || detail::abiViewEmpty(topic) || !event_json) {
                return -1;
            }
            if (!inst->enabled) {
                XX_LOGW("Plugin `{}` publish ignored (disabled)", inst->name);
                return -1;
            }
            auto              keep      = call;
            const std::string topicCopy = svToStr(*topic);
            const std::string payload   = svToStr(*event_json);
            return ioCallSyncKeep<int32_t>(keep, mgr, [keep, topicCopy, payload]() -> int32_t {
                return keep.manager()->publish(topicCopy, payload);
            });
        });
    }

    // =====================================================================
    // capabilities 表
    // =====================================================================

    static int32_t AGENTXX_PLUGIN_CALL capabilityRegisterEntry(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* capability
    ) {
        if (detail::abiViewEmpty(capability)) {
            return -1;
        }
        const std::string capCopy = svToStr(*capability);
        return detail::onInstanceIo<InstanceT, ManagerT>(
            host,
            [capCopy](InstanceT* inst, ManagerT* mgr) {
                return mgr->registerCapability(inst, capCopy);
            }
        );
    }

    static int32_t AGENTXX_PLUGIN_CALL capabilityUnregisterEntry(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* capability
    ) {
        if (detail::abiViewEmpty(capability)) {
            return -1;
        }
        const std::string capCopy = svToStr(*capability);
        return detail::onInstanceIo<InstanceT, ManagerT>(
            host,
            [capCopy](InstanceT* inst, ManagerT* mgr) {
                return mgr->unregisterCapability(inst, capCopy);
            }
        );
    }

    static int32_t AGENTXX_PLUGIN_CALL
        capabilityHasEntry(const AgentxxPluginHost* host, const AgentxxPluginStringView* capability) {
        return guardVtableCall(0, [&]() -> int32_t {
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            if (!mgr || detail::abiViewEmpty(capability)) {
                return 0;
            }
            auto              keep = call;
            const std::string cap  = svToStr(*capability);
            return ioCallSyncKeep<bool>(keep, mgr, [keep, cap]() {
                return keep.manager()->hasCapability(cap) != 0;
            })
                       ? 1
                       : 0;
        });
    }

    static int32_t AGENTXX_PLUGIN_CALL capabilityRegisterExEntry(
        const AgentxxPluginHost*             host,
        const AgentxxPluginStringView*       capability,
        AgentxxPluginCapabilityStartFunction start,
        AgentxxPluginOperatorCancelFunction  cancel,
        void*                                ctx
    ) {
        return guardVtableCall(-1, [&]() -> int32_t {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || detail::abiViewEmpty(capability) || !start) {
                return -1;
            }
            auto              keep = call;
            const std::string cap  = svToStr(*capability);
            return ioCallSyncKeep<int32_t>(keep, mgr, [keep, cap, start, cancel, ctx]() {
                return keep.manager()->registerCapabilityEx(
                    keep.instance(),
                    cap,
                    start,
                    cancel,
                    ctx
                );
            });
        });
    }

    static AgentxxPluginOperatorHandle* AGENTXX_PLUGIN_CALL invokeCapabilityAsyncEntry(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* capability,
        const AgentxxPluginStringView* method,
        const AgentxxPluginStringView* args_json,
        AgentxxPluginOperatorCallback  cb,
        void*                          ud,
        AgentxxPluginString*           error_out
    ) {
        return guardVtableCall<AgentxxPluginOperatorHandle*>(nullptr, [&]() {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || detail::abiViewEmpty(capability) || detail::abiViewEmpty(method)) {
                return static_cast<AgentxxPluginOperatorHandle*>(nullptr);
            }
            // 跨边界视图只在本次调用内有效: 投递前复制为自有字符串
            auto              keep = call;
            const std::string cap  = svToStr(*capability);
            const std::string meth = svToStr(*method);
            const std::string args = args_json ? svToStr(*args_json) : std::string{"{}"};
            return ioCallSyncKeep<AgentxxPluginOperatorHandle*>(
                keep,
                mgr,
                [keep, cap, meth, args, cb, ud, error_out]() {
                    return keep.manager()->invokeCapabilityAsync(
                        keep.instance(),
                        cap,
                        meth,
                        args,
                        cb,
                        ud,
                        error_out
                    );
                }
            );
        });
    }

    // =====================================================================
    // scheduler 表
    // =====================================================================

    static int32_t AGENTXX_PLUGIN_CALL isIoThreadEntry(const AgentxxPluginHost* host) {
        // 只读查询：关闭过程中仍允许回答（返回 0 表示"不是 IO 线程"）
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        return (mgr && mgr->isIoThread()) ? 1 : 0;
    }

    static int32_t AGENTXX_PLUGIN_CALL
        postToIoEntry(const AgentxxPluginHost* host, void(AGENTXX_PLUGIN_CALL* fn)(void*), void* ud) {
        return guardVtableCall(-1, [&]() -> int32_t {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || !fn) {
                return -1;
            }
            auto keep = call;
            return ioCallSyncKeep<int32_t>(keep, mgr, [keep, fn, ud]() -> int32_t {
                return keep.manager()->postCallback(keep.instance(), fn, ud) ? 0 : -1;
            });
        });
    }

    static AgentxxPluginOperatorHandle* AGENTXX_PLUGIN_CALL sleepEntry(
        const AgentxxPluginHost*      host,
        int64_t                       ms,
        AgentxxPluginOperatorCallback cb,
        void*                         ud,
        AgentxxPluginString*          error_out
    ) {
        return guardVtableCall<AgentxxPluginOperatorHandle*>(nullptr, [&]() {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || !cb) {
                detail::setErrorString(error_out, "scheduler sleep: plugin runtime unavailable");
                return static_cast<AgentxxPluginOperatorHandle*>(nullptr);
            }
            // sleep 自身会经 OpCore 获取实例执行 lease, 这里只需投递到 IO 线程
            auto keep = call;
            return ioCallSyncKeep<AgentxxPluginOperatorHandle*>(
                keep,
                mgr,
                [keep, ms, cb, ud, error_out]() {
                    return keep.manager()->sleep(keep.instance(), ms, cb, ud, error_out);
                }
            );
        });
    }

    static AgentxxPluginOperatorHandle* AGENTXX_PLUGIN_CALL offloadEntry(
        const AgentxxPluginHost* host,
        void*(AGENTXX_PLUGIN_CALL* work)(
            void*                           ud,
            const AgentxxPluginCancelToken* token,
            AgentxxPluginString*            error_out
        ),
        void(AGENTXX_PLUGIN_CALL* done)(
            void*                          ud,
            int32_t                        status,
            void*                          result,
            const AgentxxPluginStringView* error
        ),
        void*                ud,
        AgentxxPluginString* error_out
    ) {
        return guardVtableCall<AgentxxPluginOperatorHandle*>(nullptr, [&]() {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || !work) {
                detail::setErrorString(error_out, "scheduler offload: plugin runtime unavailable");
                return static_cast<AgentxxPluginOperatorHandle*>(nullptr);
            }
            auto keep = call;
            return ioCallSyncKeep<AgentxxPluginOperatorHandle*>(
                keep,
                mgr,
                [keep, work, done, ud, error_out]() {
                    return keep.manager()->offload(keep.instance(), work, done, ud, error_out);
                }
            );
        });
    }

    /// Operation 取消 (scheduler / capabilities / tasks 三张表共用)
    static void AGENTXX_PLUGIN_CALL opCancelEntry(AgentxxPluginOperatorHandle* op) {
        cancelPluginOperation(op);
    }

    // =====================================================================
    // coroutine_runtime 表
    // =====================================================================

    /// 申请一次驱动请求 (任意线程可调用; 永不内联回调)
    ///
    /// 语义见 pluginxx/api/tables.h 的接口表声明与 runtime/driver.h 的实现说明:
    /// - admission 采用 `allowClosing=true` 的 lifecycle lease: 实例进入 Closing
    ///   后仍必须允许驱动, 否则"取消全部 Operation → 插件收束 root"会因为拿不到
    ///   驱动而永远无法跑完;
    /// - Disabled/Closed 拒绝 (lease 获取失败);
    /// - 入队失败 (IO executor 不可用/已停止) 返回 NULL, 调用方必须把受影响的操作
    ///   以失败终结, 并且请求此时已经收束 (不会泄漏 lease)。
    static AgentxxPluginDriver* AGENTXX_PLUGIN_CALL requestDriverEntry(
        const AgentxxPluginHost*   host,
        AgentxxPluginDriveOnceFn   drive_once,
        void*                      user_data,
        AgentxxPluginString*       error_out
    ) {
        return guardVtableCall<AgentxxPluginDriver*>(nullptr, [&]() {
            if (!drive_once) {
                detail::setErrorString(error_out, "coroutine runtime: null drive callback");
                return static_cast<AgentxxPluginDriver*>(nullptr);
            }
            auto call = enterHost(host, /*allowClosing=*/true);
            auto inst = call.instance();
            auto mgr  = call.manager();
            if (!mgr || !inst || !inst->lifetime) {
                detail::setErrorString(
                    error_out,
                    "coroutine runtime: plugin instance is closed or unavailable"
                );
                return static_cast<AgentxxPluginDriver*>(nullptr);
            }
            auto driver = AgentxxPluginDriver::create(
                mgr->runtime(),
                inst->lifetime,
                drive_once,
                user_data,
                inst->name + " driver"
            );
            if (!driver) {
                detail::setErrorString(
                    error_out,
                    "coroutine runtime: plugin instance is closing or closed"
                );
                return static_cast<AgentxxPluginDriver*>(nullptr);
            }
            // 句柄墓碑先登记再排队: 迟到 cancel_driver 只会观察终态, 不会解引用已释放对象。
            inst->retainDriverHandle(driver);
            if (!driver->schedule()) {
                detail::setErrorString(error_out, "coroutine runtime: host IO executor is unavailable");
                return static_cast<AgentxxPluginDriver*>(nullptr);
            }
            return driver.get();
        });
    }

    /// 取消尚未开始的请求 (幂等, 非阻塞, 任意线程可调用)
    ///
    /// `cancel_driver` 的 ABI 形态不含 host 参数, 因此句柄校验走请求自身的进程级
    /// 地址注册表: 命中才解引用 (weak_ptr 升级为强引用), 伪造/过期指针安全忽略。
    static void AGENTXX_PLUGIN_CALL cancelDriverEntry(AgentxxPluginDriver* driver) {
        if (!driver) {
            return;
        }
        guardVtableCallVoid([&] {
            if (!AgentxxPluginDriver::cancelByHandle(driver)) {
                XX_LOGW("Plugin driver cancellation ignored: handle is not an active ticket");
            }
        });
    }

    // =====================================================================
    // tasks 表
    // =====================================================================

    static AgentxxPluginOperatorHandle* AGENTXX_PLUGIN_CALL registerTaskEntry(
        const AgentxxPluginHost*            host,
        AgentxxPluginOperatorCancelFunction cancel_fn,
        void*                               cancel_ud,
        AgentxxPluginOperatorNotify*        notify,
        AgentxxPluginString*                error_out
    ) {
        return guardVtableCall<AgentxxPluginOperatorHandle*>(nullptr, [&]() {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || !notify) {
                return static_cast<AgentxxPluginOperatorHandle*>(nullptr);
            }
            auto keep = call;
            return ioCallSyncKeep<AgentxxPluginOperatorHandle*>(
                keep,
                mgr,
                [keep, cancel_fn, cancel_ud, notify, error_out]() {
                    return keep.manager()
                        ->registerTask(keep.instance(), cancel_fn, cancel_ud, notify, error_out);
                }
            );
        });
    }

    // =====================================================================
    // cancel 表
    // =====================================================================

    static int32_t AGENTXX_PLUGIN_CALL cancelIsCancelledEntry(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* thread_id
    ) {
        return guardVtableCall(0, [&]() -> int32_t {
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            if (!mgr || detail::abiViewEmpty(thread_id)) {
                return 0;
            }
            auto              keep = call;
            const std::string tid  = svToStr(*thread_id);
            return ioCallSyncKeep<bool>(keep, mgr, [keep, tid]() {
                auto* hooks = keep.manager()->domainHooks();
                return hooks ? hooks->isSessionCancelled(tid) : false;
            })
                       ? 1
                       : 0;
        });
    }

    // =====================================================================
    // 表结构体 (首次查询时构造; 线程安全的静态初始化)
    // =====================================================================

    static const AgentxxPluginLogIface& logIface() {
        static const AgentxxPluginLogIface iface{
            AGENTXX_PLUGIN_IFACE_AGENT_LOG_VERSION,
            sizeof(AgentxxPluginLogIface),
            &logEntry,
        };
        return iface;
    }

    static const AgentxxPluginJsonIface& jsonIface() {
        static const AgentxxPluginJsonIface iface{
            AGENTXX_PLUGIN_IFACE_AGENT_JSON_VERSION,
            sizeof(AgentxxPluginJsonIface),
            &jsonGetStringEntry,
            &jsonEscapeEntry,
        };
        return iface;
    }

    static const AgentxxPluginConfigIface& configIface() {
        static const AgentxxPluginConfigIface iface{
            AGENTXX_PLUGIN_IFACE_AGENT_CONFIG_VERSION,
            sizeof(AgentxxPluginConfigIface),
            &configGetEntry,
            &pluginArgsEntry,
            &toolPromptEntry,
            &sessionWorkDirEntry,
            &pluginConfigPathEntry,
            &languageGetEntry,
            &languageSetEntry,
        };
        return iface;
    }

    static const AgentxxPluginsIface& pluginsIface() {
        static const AgentxxPluginsIface iface{
            AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS_VERSION,
            sizeof(AgentxxPluginsIface),
            &listPluginsEntry,
            &getPluginEntry,
            &ownInfoEntry,
        };
        return iface;
    }

    static const AgentxxPluginEventsIface& eventsIface() {
        static const AgentxxPluginEventsIface iface{
            AGENTXX_PLUGIN_IFACE_AGENT_EVENTS_VERSION,
            sizeof(AgentxxPluginEventsIface),
            &subscribeEntry,
            &unsubscribeEntry,
            &publishEntry,
        };
        return iface;
    }

    static const AgentxxPluginCapabilitiesIface& capabilitiesIface() {
        static const AgentxxPluginCapabilitiesIface iface{
            AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES_VERSION,
            sizeof(AgentxxPluginCapabilitiesIface),
            &capabilityRegisterEntry,
            &capabilityRegisterExEntry,
            &capabilityUnregisterEntry,
            &capabilityHasEntry,
            &invokeCapabilityAsyncEntry,
            &opCancelEntry,
        };
        return iface;
    }

    static const AgentxxPluginSchedulerIface& schedulerIface() {
        static const AgentxxPluginSchedulerIface iface{
            AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER_VERSION,
            sizeof(AgentxxPluginSchedulerIface),
            &isIoThreadEntry,
            &postToIoEntry,
            &sleepEntry,
            &opCancelEntry,
            &offloadEntry,
        };
        return iface;
    }

    static const AgentxxPluginCoroutineRuntimeIface& coroutineRuntimeIface() {
        static const AgentxxPluginCoroutineRuntimeIface iface{
            AGENTXX_PLUGIN_IFACE_COROUTINE_RUNTIME_VERSION,
            sizeof(AgentxxPluginCoroutineRuntimeIface),
            &requestDriverEntry,
            &cancelDriverEntry,
            &isIoThreadEntry,
        };
        return iface;
    }

    static const AgentxxPluginTasksIface& tasksIface() {
        static const AgentxxPluginTasksIface iface{
            AGENTXX_PLUGIN_IFACE_AGENT_TASKS_VERSION,
            sizeof(AgentxxPluginTasksIface),
            &registerTaskEntry,
            &opCancelEntry,
        };
        return iface;
    }

    static const AgentxxPluginCancelIface& cancelIface() {
        static const AgentxxPluginCancelIface iface{
            AGENTXX_PLUGIN_IFACE_AGENT_CANCEL_VERSION,
            sizeof(AgentxxPluginCancelIface),
            &cancelIsCancelledEntry,
        };
        return iface;
    }
};

/// 按 IID 查询**通用表** (未命中返回 nullptr, 宿主继续查自己的领域表)
/// - 覆盖 10 张通用表; IID 为编译期常量字符串, 比较用 string_view (不构造临时串)
template<typename InstanceT, typename ManagerT>
const void* queryGenericPluginIface(std::string_view iid) {
    using Entries = GenericTableEntries<InstanceT, ManagerT>;
    if (iid == AGENTXX_PLUGIN_IFACE_AGENT_LOG) {
        return &Entries::logIface();
    }
    if (iid == AGENTXX_PLUGIN_IFACE_AGENT_JSON) {
        return &Entries::jsonIface();
    }
    if (iid == AGENTXX_PLUGIN_IFACE_AGENT_CONFIG) {
        return &Entries::configIface();
    }
    if (iid == AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS) {
        return &Entries::pluginsIface();
    }
    if (iid == AGENTXX_PLUGIN_IFACE_AGENT_EVENTS) {
        return &Entries::eventsIface();
    }
    if (iid == AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES) {
        return &Entries::capabilitiesIface();
    }
    if (iid == AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER) {
        return &Entries::schedulerIface();
    }
    if (iid == AGENTXX_PLUGIN_IFACE_COROUTINE_RUNTIME) {
        return &Entries::coroutineRuntimeIface();
    }
    if (iid == AGENTXX_PLUGIN_IFACE_AGENT_TASKS) {
        return &Entries::tasksIface();
    }
    if (iid == AGENTXX_PLUGIN_IFACE_AGENT_CANCEL) {
        return &Entries::cancelIface();
    }
    return nullptr;
}

} // namespace pluginxx
