#include "agentxx/plugin/plugin_manager.h"
#include "pluginxx/runtime/op_driver.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/agent/resource_applier.h"
#include "agentxx/event/hub.h"
#include "agentxx/event/naming.h"
#include "agentxx/plugin/plugin_framework.h"
#include "agentxx/plugin/plugin_interfaces.h"
#include "agentxx/util/exception.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace agentxx {
namespace plugin {

// =====================================================================
// vtable 入口公共前置
// =====================================================================

using HostCall = PluginHostCall<PluginInstance, PluginManager>;

/// 解析主程序控制块，并持有实例/管理器强引用与 admission lease。
///
/// - 实例已卸载/已关闭，或传入的 host 指针不是本主程序发放的视图 -> 返回空上下文
///   （实例与管理器均为 nullptr），入口按失败返回，不访问已释放对象，也不会把
///   调用转交给后来加载的同名实例；
/// - `allowClosing=true` 用于只读查询：关闭过程中仍允许执行（lease 保证卸载会
///   等它返回）。注册、投递新工作等入口必须用默认值，Closing/Disabled 后拒绝；
/// - 返回的上下文按值捕获进投递闭包后，卸载的 idle 等待会覆盖"已排队但尚未在
///   IO 线程执行"的阶段，见 [ioCallSyncKeep]。
static HostCall enterHost(const PluginxxHost* host, bool allowClosing = false) {
    return enterPluginHost<PluginInstance, PluginManager>(host, allowClosing);
}

/// 校验作用域引用 (agent 级接口的公共门槛)
/// - 引用为空 / 长度不足 / id 不匹配 / seq 不匹配 → false
/// - 本主程序当前是一份管理器服务一个 agent, 所以只有一份可命中的引用
/// - 失败记一条警告: 带插件给的 `agent_id` / `seq` 与本程序期望的值, 便于定位
///   "在 start 里注册"或"旧引用"这类错误
static bool scopeOk(const PluginManager* mgr, const PluginxxScopeRef* agent) {
    if (mgr != nullptr && mgr->mountOf(agent) != nullptr) {
        return true;
    }
    std::string expected;
    if (mgr != nullptr) {
        for (const auto& mount : mgr->mounts()) {
            if (!mount) {
                continue;
            }
            if (!expected.empty()) {
                expected += ", ";
            }
            expected += mount->agentId() + "#" + std::to_string(mount->mountSeq());
        }
    }
    std::string gotId;
    uint64_t    gotSeq = 0;
    if (pluginxx_scope_ref_ok(agent) && agent != nullptr) {
        if (agent->id.data != nullptr && agent->id.size > 0) {
            gotId.assign(agent->id.data, static_cast<size_t>(agent->id.size));
        }
        gotSeq = agent->seq;
    }
    XX_LOGW(
        "agent 引用无效 (收到 agent_id=`{}` seq={}; 本程序的挂载: {}); "
        "agent 级接口请在插件的 mount 回调里调用, 并给它本挂载的引用",
        gotId.empty() ? std::string{"<empty>"} : gotId,
        gotSeq,
        expected.empty() ? std::string{"<none>"} : expected
    );
    return false;
}

/// 把作用域引用解析成挂载 (须在 IO 线程调用: 挂载表只在主程序 IO 线程变动)
static PluginMount* mountIn(const PluginManager* mgr, const PluginxxScopeRef* agent) {
    return mgr == nullptr ? nullptr : mgr->mountOf(agent);
}

/// 注册类入口解析失败时的统一返回值
static int32_t noAgent() {
    return AGENTXX_RESULT_ERR_NO_AGENT;
}

/// 解析挂载并执行; 解析失败时返回 `fail` 给出的失败值
/// - 注册类传 [noAgent] (`AGENTXX_RESULT_ERR_NO_AGENT`)
/// - 读取类传 `-1` / 空串 / 空句柄 (与"引用无效"的既有约定一致)
template<typename Fail, typename Fn>
static auto mountCall(const PluginManager* mgr, const PluginxxScopeRef* agent, Fail&& fail, Fn&& fn)
    -> decltype(fn(std::declval<PluginMount&>())) {
    auto* mount = mountIn(mgr, agent);
    if (mount == nullptr) {
        return fail();
    }
    return fn(*mount);
}

/// 异步表调用的公共骨架 (契约见 pluginxx/host/table_op.h 与 plugin_api.h 文件头)
///
/// - 入口只做"解析上下文 + 交给 [startTableOp]": 参数与作用域校验由 `work` 自己做
///   (失败抛 [TableOpError], 经回调按 FAILED 交回);
/// - `allowClosing=true` 用于只读查询 (关闭中仍可查), 写入口用默认值;
/// - `work` 在自己的 IO 线程执行, 调用方本来就在 IO 线程时内联执行 —— 所以
///   "注册完立刻可见"的顺序与同步调用一致。
template<bool AllowClosing = false, typename Fn>
static PluginxxOperatorHandle* asyncTableCall(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out,
    std::string              label,
    Fn&&                     work
) {
    auto call = enterHost(host, AllowClosing);
    if (!call.ok()) {
        setTableOpError(error_out, "plugin runtime is unavailable");
        return nullptr;
    }
    auto keep = call;
    return startTableOp(
        keep.manager(),
        cb,
        ud,
        error_out,
        std::move(label),
        [keep, work = std::forward<Fn>(work)]() mutable -> std::string {
            return work(keep.instance(), keep.manager());
        }
    );
}

/// 作用域引用的自持拷贝 (跨线程投递用)
///
/// 插件传来的 `PluginxxScopeRef` 中 `id` 是**借用视图**, 指向调用方的内存; 请求排到
/// 自己的 IO 线程之后那些内存可能已经不存在, 所以投递前先把 id 字节拷一份。
struct ScopeRefKeep {
    std::string id;
    uint64_t    seq   = 0;
    bool        valid = false;

    explicit ScopeRefKeep(const PluginxxScopeRef* ref) {
        if (!pluginxx_scope_ref_ok(ref)) {
            return;
        }
        valid = true;
        if (ref->id.data != nullptr && ref->id.size > 0) {
            id.assign(ref->id.data, static_cast<size_t>(ref->id.size));
        }
        seq = ref->seq;
    }

    /// 重建一份引用 (指向本对象内部, 只能在本次调用期间使用)
    PluginxxScopeRef ref() const {
        PluginxxScopeRef out{};
        out.struct_size = static_cast<uint32_t>(sizeof(PluginxxScopeRef));
        out.flags       = 0;
        out.id          = PluginxxStringView{id.data(), static_cast<uint64_t>(id.size())};
        out.seq         = seq;
        return out;
    }
};

/// 带作用域校验的异步表调用
///
/// - 引用的**形状**无效 (空指针 / `id` 为空 / `struct_size` 越界) 属于**未受理**:
///   只检查字段, 任意线程都能判, 所以同步返回 NULL + `error_out`;
/// - 引用指向的挂载是否存在、序号是否过期, 要读本管理器的挂载表, 只能在 IO 线程上判,
///   因此这两种情况按"受理后失败"交回 (`payload` 给原因), 调用方从 `status` 看结论。
template<bool AllowClosing = false, typename Fn>
static PluginxxOperatorHandle* asyncScopedTableCall(
    const PluginxxHost*      host,
    const PluginxxScopeRef*  agent,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out,
    std::string              label,
    Fn&&                     work
) {
    ScopeRefKeep scope{agent};
    if (!scope.valid) {
        setTableOpError(error_out, "agent reference is invalid");
        return nullptr;
    }
    return asyncTableCall<AllowClosing>(
        host,
        cb,
        ud,
        error_out,
        std::move(label),
        [scope, work = std::forward<Fn>(work)](
            PluginInstance* inst,
            PluginManager*  mgr
        ) -> std::string {
            auto ref = scope.ref();
            if (!scopeOk(mgr, &ref)) {
                throw TableOpError("agent reference is invalid");
            }
            return work(inst, mgr, static_cast<const PluginxxScopeRef&>(ref));
        }
    );
}

/// 只读查询类异步入口: 工作体返回文本, 作为 `payload` 交回
///
/// 旧版用"返回值 + 出参串"表达的两件事在这里统一: 空文本 = 没有结果,
/// 按失败交回 (与旧 `ERR_NOT_FOUND` 一致)。查询允许实例关闭中调用 (收尾时工具仍可能在跑)。
template<typename Fn>
static PluginxxOperatorHandle* asyncQueryCall(
    const PluginxxHost*      host,
    const PluginxxScopeRef*  agent,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out,
    std::string              label,
    Fn&&                     work
) {
    return asyncScopedTableCall</*AllowClosing=*/true>(
        host,
        agent,
        cb,
        ud,
        error_out,
        std::move(label),
        [work = std::forward<Fn>(work)](
            PluginInstance*         inst,
            PluginManager*          mgr,
            const PluginxxScopeRef& ref
        ) -> std::string {
            auto text = work(inst, mgr, ref);
            if (text.empty()) {
                throw TableOpError("no result");
            }
            return text;
        }
    );
}

/// 主程序堆字符串 → std::string (顺手释放)
static std::string takePluginString(PluginxxString& s) {
    std::string out;
    if (s.data != nullptr) {
        out.assign(s.data, static_cast<size_t>(s.size));
        hostMemoryFree(s.data);
        s = PluginxxString{};
    }
    return out;
}


/// 注册/写入类入口的公共骨架: 解析 host 上下文 → 把业务逻辑投递到 IO 线程执行。
/// - 参数视图只在本次调用内有效, 因此闭包必须按值捕获自己需要的拷贝;
/// - `fn` 的入参是实例与管理器 (投递期间由 `keep` 保活, 含 admission lease);
///   业务参数校验由 `fn` 自己完成, 失败返回非 0 状态码。
template<typename Fn>
static int32_t onInstanceIo(const PluginxxHost* host, Fn&& fn) {
    return agentxx::plugin::guardVtableCall(AGENTXX_RESULT_ERR_INTERNAL, [&]() -> int32_t {
        auto call = enterHost(host);
        if (!call.ok()) {
            return AGENTXX_RESULT_ERR_ARG;
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
template<typename Fn>
static int32_t queryStringIo(const PluginxxHost* host, PluginxxString* out, Fn&& fn) {
    if (!out) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    return agentxx::plugin::guardVtableCall(AGENTXX_RESULT_ERR_INTERNAL, [&]() -> int32_t {
        auto call = enterHost(host, /*allowClosing=*/true);
        if (!call.ok()) {
            return AGENTXX_RESULT_ERR_ARG;
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
            return AGENTXX_RESULT_ERR_NOT_FOUND;
        }
        hostMemorySetString(out, text);
        return AGENTXX_RESULT_OK;
    });
}

// C ABI 内存操作

/// 带作用域校验的注册入口: 引用无效时直接返回 `AGENTXX_RESULT_ERR_NO_AGENT`
/// (不产生任何副作用)
template<typename Fn>
static int32_t
    onScopedInstanceIo(const PluginxxHost* host, const PluginxxScopeRef* agent, Fn&& fn) {
    return onInstanceIo(
        host,
        [agent, fn = std::forward<Fn>(fn)](PluginInstance* inst, PluginManager* mgr) -> int32_t {
            if (!scopeOk(mgr, agent)) {
                XX_LOGW("agentxx 注册接口: agent 引用无效, 请在 mount 里注册");
                return AGENTXX_RESULT_ERR_NO_AGENT;
            }
            return fn(inst, mgr);
        }
    );
}

/// 带作用域校验的只读入口: 引用无效时返回 -1
template<typename Fn>
static int32_t queryScopedStringIo(
    const PluginxxHost*     host,
    const PluginxxScopeRef* agent,
    PluginxxString*         out,
    Fn&&                    fn
) {
    return queryStringIo(
        host,
        out,
        [agent, fn = std::forward<Fn>(fn)](PluginInstance* inst, PluginManager* mgr) -> std::string {
            if (!scopeOk(mgr, agent)) {
                return std::string{};
            }
            return fn(inst, mgr);
        }
    );
}
static void* PLUGINXX_CALL xx_alloc(uint64_t size) {
    return agentxx::plugin::hostMemoryAlloc(size);
}

static void PLUGINXX_CALL xx_free(void* ptr) {
    agentxx::plugin::hostMemoryFree(ptr);
}

/// 工具注册声明的自持拷贝 (跨线程投递用)
///
/// `spec` 里的三个字符串视图只在本次调用期间有效; 投递前把内容拷成自有字符串,
/// 再把视图指回这些字符串。闭包按 `shared_ptr` 持有本对象, 所以视图始终有效。
struct ToolSpecKeep {
    AgentxxPluginToolSpec spec{};
    std::string           name;
    std::string           description;
    std::string           parameters;

    explicit ToolSpecKeep(const AgentxxPluginToolSpec& src) :
        spec(src),
        name(svToStr(&src.name)),
        description(svToStr(&src.description)),
        parameters(svToStr(&src.parameters_json)) {
        spec.name            = agentxx::plugin::PluginStringView::from(name);
        spec.description     = agentxx::plugin::PluginStringView::from(description);
        spec.parameters_json = agentxx::plugin::PluginStringView::from(parameters);
    }
};

static PluginxxOperatorHandle* PLUGINXX_CALL xx_register_tool(
    const PluginxxHost*          host,
    const PluginxxScopeRef*      agent,
    const AgentxxPluginToolSpec* spec,
    PluginxxOperatorCallback     cb,
    void*                        ud,
    PluginxxString*              error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (spec == nullptr || agentxx::plugin::PluginStringView::empty(&spec->name)) {
            setTableOpError(error_out, "register_tool: spec and tool name are required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        // 长度门槛: 0 = 基线布局, 低于基线一律拒绝 (见 plugin_spec.h)
        if (!specSizeOk<AgentxxPluginToolSpec>(spec->struct_size)) {
            setTableOpError(error_out, "register_tool: spec struct_size is too small");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto keep = std::make_shared<ToolSpecKeep>(*spec);
        return asyncScopedTableCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "tools.register_tool",
            [keep](
                PluginInstance*         inst,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                auto rc = mountCall(mgr, &ref, noAgent, [inst, keep](PluginMount& mount) {
                    return mount.registerTool(inst, &keep->spec);
                });
                if (rc != AGENTXX_RESULT_OK) {
                    throw TableOpError(std::string(agentxx::plugin::resultName(rc)));
                }
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_unregister_tool(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* name,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (agentxx::plugin::PluginStringView::empty(name)) {
            setTableOpError(error_out, "unregister_tool: tool name is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto nameKeep = svToStr(name);
        return asyncScopedTableCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "tools.unregister_tool",
            [nameKeep](
                PluginInstance*         inst,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                const PluginxxStringView view{
                    nameKeep.data(), static_cast<uint64_t>(nameKeep.size())
                };
                auto rc = mountCall(mgr, &ref, noAgent, [inst, view](PluginMount& mount) {
                    return mount.unregisterTool(inst, view);
                });
                if (rc != AGENTXX_RESULT_OK) {
                    throw TableOpError(std::string(agentxx::plugin::resultName(rc)));
                }
                return std::string{};
            }
        );
    });
}

/// 工具权限声明的自持拷贝 (跨线程投递用)
///
/// 三个字符串视图都是借用视图: 投递前拷成自有字符串并把视图指回它们。
struct ToolPermissionSpecKeep {
    AgentxxPluginToolPermissionSpec spec{};
    std::string                     toolName;
    std::string                     targetArg;
    std::string                     category;

    explicit ToolPermissionSpecKeep(const AgentxxPluginToolPermissionSpec& src) :
        spec(src),
        toolName(svToStr(&src.tool_name)),
        targetArg(svToStr(&src.target_arg)),
        category(svToStr(&src.category)) {
        spec.tool_name  = agentxx::plugin::PluginStringView::from(toolName);
        spec.target_arg = agentxx::plugin::PluginStringView::from(targetArg);
        spec.category   = agentxx::plugin::PluginStringView::from(category);
    }
};

static PluginxxOperatorHandle* PLUGINXX_CALL xx_register_tool_permission(
    const PluginxxHost*                    host,
    const PluginxxScopeRef*                agent,
    const AgentxxPluginToolPermissionSpec* spec,
    PluginxxOperatorCallback               cb,
    void*                                  ud,
    PluginxxString*                        error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (spec == nullptr || agentxx::plugin::PluginStringView::empty(&spec->tool_name)) {
            setTableOpError(error_out, "register_tool_permission: spec and tool name are required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        if (!specSizeOk<AgentxxPluginToolPermissionSpec>(spec->struct_size)) {
            setTableOpError(
                error_out,
                "register_tool_permission: spec struct_size is too small"
            );
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto keep = std::make_shared<ToolPermissionSpecKeep>(*spec);
        return asyncScopedTableCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "permission.register_tool_permission",
            [keep](
                PluginInstance*         inst,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                auto rc = mountCall(mgr, &ref, noAgent, [inst, keep](PluginMount& mount) {
                    return mount.registerToolPermission(inst, &keep->spec);
                });
                if (rc != AGENTXX_RESULT_OK) {
                    throw TableOpError(std::string(agentxx::plugin::resultName(rc)));
                }
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_unregister_tool_permission(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* tool_name,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (agentxx::plugin::PluginStringView::empty(tool_name)) {
            setTableOpError(error_out, "unregister_tool_permission: tool name is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto nameKeep = svToStr(tool_name);
        return asyncScopedTableCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "permission.unregister_tool_permission",
            [nameKeep](
                PluginInstance*         inst,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                const PluginxxStringView view{
                    nameKeep.data(), static_cast<uint64_t>(nameKeep.size())
                };
                auto rc = mountCall(mgr, &ref, noAgent, [inst, view](PluginMount& mount) {
                    return mount.unregisterToolPermission(inst, view);
                });
                if (rc != AGENTXX_RESULT_OK) {
                    throw TableOpError(std::string(agentxx::plugin::resultName(rc)));
                }
                return std::string{};
            }
        );
    });
}

/// check_paths 单次批量上限: 判定在主程序 io 线程执行, 限制单次规模避免长时间占用
/// (插件侧建议按 512 项分批; 上限仅作为异常调用的保护)
static constexpr int32_t kPermissionCheckPathsMax = 16384;

static PluginxxOperatorHandle* PLUGINXX_CALL xx_check_paths(
    const PluginxxHost*                     host,
    const PluginxxScopeRef*                 agent,
    const AgentxxPluginPermissionPathQuery* query,
    PluginxxOperatorCallback                cb,
    void*                                   ud,
    PluginxxString*                         error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (query == nullptr || query->paths == nullptr || query->path_count <= 0) {
            setTableOpError(error_out, "check_paths: query and paths are required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        if (query->path_count > kPermissionCheckPathsMax) {
            setTableOpError(error_out, "check_paths: too many paths in one call");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        // 长度门槛: 0 = 基线布局, 低于基线一律拒绝 (见 plugin_spec.h)
        if (!agentxx::plugin::specSizeOk<AgentxxPluginPermissionPathQuery>(query->struct_size)) {
            setTableOpError(error_out, "check_paths: query struct_size is too small");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        // 入参按值复制后交给 io 线程执行 (跨边界视图只在本次调用期间有效)
        auto paths = std::make_shared<std::vector<std::string>>();
        paths->reserve(static_cast<size_t>(query->path_count));
        for (int32_t i = 0; i < query->path_count; ++i) {
            const auto& item = query->paths[i];
            paths->emplace_back(item.data ? item.data : "", static_cast<size_t>(item.size));
        }
        auto          sessionId = std::make_shared<std::string>(svToStr(&query->session_id));
        const int32_t scope     = query->scope;
        // 只读查询: 允许关闭中调用 (实例收尾时其工具仍可能在执行)
        return asyncScopedTableCall</*AllowClosing=*/true>(
            host,
            agent,
            cb,
            ud,
            error_out,
            "permission.check_paths",
            [paths, sessionId, scope](
                PluginInstance*         inst,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                std::vector<int32_t> decisions;
                auto                 rc = mountCall(
                    mgr,
                    &ref,
                    noAgent,
                    [&](PluginMount& mount) {
                        return mount.checkPermissionPaths(
                            inst,
                            scope,
                            *sessionId,
                            *paths,
                            decisions
                        );
                    }
                );
                if (rc != AGENTXX_RESULT_OK) {
                    throw TableOpError(std::string(agentxx::plugin::resultName(rc)));
                }
                if (decisions.size() != paths->size()) {
                    throw TableOpError("check_paths: decision count mismatch");
                }
                // 判定数组按 JSON 数组文本交回 (与入参 paths 等长、顺序一致)
                std::string out;
                out.reserve(decisions.size() * 2 + 2);
                out.push_back('[');
                for (size_t i = 0; i < decisions.size(); ++i) {
                    if (i > 0) {
                        out.push_back(',');
                    }
                    out += std::to_string(decisions[i]);
                }
                out.push_back(']');
                return out;
            }
        );
    });
}

/// 挂载表: 全部挂载的摘要数组 (`list`)
///
/// 定位在主程序业务表的开头, 因为 `query_interface` 在这个文件的后半段引用它。
static PluginxxOperatorHandle* PLUGINXX_CALL xx_mount_list(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        return asyncTableCall</*AllowClosing=*/true>(
            host,
            cb,
            ud,
            error_out,
            "mount.list",
            [](PluginInstance*, PluginManager* mgr) -> std::string {
                auto text = mgr->listMountsJson();
                if (text.empty()) {
                    throw TableOpError("no mount information");
                }
                return text;
            }
        );
    });
}

/// 挂载表: 按 agentId 查一个挂载的摘要 (`get`)
static PluginxxOperatorHandle* PLUGINXX_CALL xx_mount_get(
    const PluginxxHost*       host,
    const PluginxxStringView* agent_id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (agentxx::plugin::PluginStringView::empty(agent_id)) {
            setTableOpError(error_out, "mount.get: agent id is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto idStr = svToStr(agent_id);
        return asyncTableCall</*AllowClosing=*/true>(
            host,
            cb,
            ud,
            error_out,
            "mount.get",
            [idStr](PluginInstance*, PluginManager* mgr) -> std::string {
                auto text = mgr->mountInfoJson(idStr);
                if (text.empty()) {
                    throw TableOpError("no such mount");
                }
                return text;
            }
        );
    });
}

static const AgentxxPluginMountIface g_ifaceMount = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_MOUNT_VERSION,
    /* struct_size */ sizeof(AgentxxPluginMountIface),
    /* list */ xx_mount_list,
    /* get */ xx_mount_get,
};

static ::PluginxxOperatorHandle* PLUGINXX_CALL xx_call_tool(
    const PluginxxHost*        host,
    const PluginxxScopeRef*    agent,
    const PluginxxStringView*  name,
    const PluginxxStringView*  args_json,
    const PluginxxStringView*  session_id,
    AgentxxPluginToolCallback  cb,
    void*                      ud,
    PluginxxString*            error_out
) {
    return guardVtableCall<::PluginxxOperatorHandle*>(nullptr, [&]() {
        auto  call = enterHost(host);
        auto* inst = call.instance();
        auto* mgr  = call.manager();
        if (!mgr || !inst || !name) {
            hostMemorySetString(error_out, "call_tool: plugin runtime unavailable");
            return static_cast<::PluginxxOperatorHandle*>(nullptr);
        }
        if (!scopeOk(mgr, agent)) {
            hostMemorySetString(error_out, "call_tool: agent reference is invalid");
            return static_cast<::PluginxxOperatorHandle*>(nullptr);
        }
        auto* mount = mountIn(mgr, agent);
        if (mount == nullptr) {
            hostMemorySetString(error_out, "call_tool: agent reference is invalid");
            return static_cast<::PluginxxOperatorHandle*>(nullptr);
        }
        return mount->callToolAsync(
            inst,
            *name,
            args_json ? *args_json : PluginxxStringView{},
            session_id ? *session_id : PluginxxStringView{},
            cb,
            ud,
            error_out
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_get_share_store(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* session_id,
    int64_t                   id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (agentxx::plugin::PluginStringView::empty(session_id)) {
            setTableOpError(error_out, "get_share_store: session id is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        // 入参视图只在本次调用有效: 投递到 IO 线程前先拷成自己的字符串
        auto sid = svToStr(session_id);
        return asyncScopedTableCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "session.get_share_store",
            [sid, id](
                PluginInstance*         inst,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                auto text = mountCall(
                    mgr,
                    &ref,
                    [] { return PluginxxString{}; },
                    [inst, &sid, id](PluginMount& mount) {
                        return mount.getShareStore(
                            inst,
                            PluginxxStringView{sid.data(), sid.size()},
                            id
                        );
                    }
                );
                auto out = takePluginString(text);
                if (out.empty()) {
                    throw TableOpError(
                        fmt::format("share_store entry {} not found", id)
                    );
                }
                return out;
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_add_share_store(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* session_id,
    const PluginxxStringView* content,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (!content) {
            setTableOpError(error_out, "add_share_store: content is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto sid     = svToStr(session_id);
        auto payload = svToStr(content);
        return asyncScopedTableCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "session.add_share_store",
            [sid, payload](
                PluginInstance*         inst,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                const auto id = mountCall(
                    mgr,
                    &ref,
                    [] { return int64_t{-1}; },
                    [inst, &sid, &payload](PluginMount& mount) {
                        return mount.addShareStore(
                            inst,
                            PluginxxStringView{sid.data(), sid.size()},
                            PluginxxStringView{payload.data(), payload.size()}
                        );
                    }
                );
                if (id < 0) {
                    throw TableOpError("share_store entry could not be written");
                }
                return std::to_string(id);
            }
        );
    });
}

// =====================================================================
// 接口表: 会话访问的追加成员 (v3: 工作目录 / 取消查询)
// =====================================================================

/// `agentxx.agent.session.get_session_work_dir` (**异步**)
/// - `agent` 必须有效 (引用无效按 FAILED 交回并记一条警告)
/// - `session_id` 可为空: 空 = 取解析后的默认会话工作目录
static PluginxxOperatorHandle* PLUGINXX_CALL xx_session_work_dir(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* session_id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        auto sid = svToStr(session_id);
        return asyncScopedTableCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "session.get_session_work_dir",
            [sid](
                PluginInstance*,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                return mgr->sessionWorkDir(&ref, sid);
            }
        );
    });
}

/// `agentxx.agent.session.is_canceled`
/// - `agent` 必须有效: 引用无效返回 0 (未取消) 并记一条警告
/// - 只是主动查询: 权威的取消通知始终是取消回调
static int32_t PLUGINXX_CALL xx_session_Canceled(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* thread_id
) {
    return agentxx::plugin::guardVtableCall<int32_t>(0, [&]() -> int32_t {
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(thread_id)) {
            return 0;
        }
        if (!scopeOk(mgr, agent)) {
            return 0;
        }
        auto mgrPtr = mgr;
        auto tid    = svToStr(thread_id);
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, agent, tid]() -> int32_t {
            return mountCall(
                mgrPtr,
                agent,
                [] { return int32_t{0}; },
                [mgrPtr, agent, tid](PluginMount&) {
                    return mgrPtr->isSessionCanceled(agent, tid) ? 1 : 0;
                }
            );
        });
    });
}

// =====================================================================
// 接口表: 会话上下文查询 (agentxx.agent.context)
// =====================================================================

static PluginxxOperatorHandle* PLUGINXX_CALL xx_context_get_messages(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* session_id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (agentxx::plugin::PluginStringView::empty(session_id)) {
            setTableOpError(error_out, "get_messages: session id is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto sid = svToStr(session_id);
        return asyncQueryCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "context.get_messages",
            [sid](
                PluginInstance*,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                const PluginxxStringView view{
                    sid.data(), static_cast<uint64_t>(sid.size())
                };
                PluginxxString raw{};
                auto           rc = mountCall(mgr, &ref, noAgent, [&](PluginMount& mount) {
                    raw = mount.getSessionMessages(view);
                    return AGENTXX_RESULT_OK;
                });
                if (rc != AGENTXX_RESULT_OK) {
                    throw TableOpError(std::string(agentxx::plugin::resultName(rc)));
                }
                return takePluginString(raw);
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_context_messages_count(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* session_id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (agentxx::plugin::PluginStringView::empty(session_id)) {
            setTableOpError(error_out, "messages_count: session id is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto sid = svToStr(session_id);
        return asyncQueryCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "context.messages_count",
            [sid](
                PluginInstance*,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                const PluginxxStringView view{
                    sid.data(), static_cast<uint64_t>(sid.size())
                };
                auto count = mountCall(
                    mgr,
                    &ref,
                    [] { return int64_t{-1}; },
                    [&view](PluginMount& mount) { return mount.sessionMessagesCount(view); }
                );
                if (count < 0) {
                    throw TableOpError("session context is not available");
                }
                return std::to_string(count);
            }
        );
    });
}

static const AgentxxPluginContextIface g_ifaceContext = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT_VERSION,
    /* struct_size */ sizeof(AgentxxPluginContextIface),
    /* get_messages */ xx_context_get_messages,
    /* messages_count */ xx_context_messages_count,
};

static PluginxxOperatorHandle* PLUGINXX_CALL xx_emit_message_tip(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* session_id,
    const PluginxxStringView* text,
    int32_t                   level,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (agentxx::plugin::PluginStringView::empty(text)) {
            setTableOpError(error_out, "emit_message_tip: text is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto sid  = svToStr(session_id);
        auto body = svToStr(text);
        return asyncScopedTableCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "session.emit_message_tip",
            [sid, body, level](
                PluginInstance*         inst,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                mountCall(
                    mgr,
                    &ref,
                    [] { return 0; },
                    [inst, &sid, &body, level](PluginMount& mount) {
                        mount.emitMessageTip(
                            inst,
                            PluginxxStringView{sid.data(), sid.size()},
                            PluginxxStringView{body.data(), body.size()},
                            level
                        );
                        return 0;
                    }
                );
                return std::string{};
            }
        );
    });
}

// =====================================================================
// graph 接口表 (agentxx.agent.graph)
// =====================================================================

/// 节点类型声明的自持拷贝 (跨线程投递用)
struct GraphNodeTypeSpecKeep {
    AgentxxPluginGraphNodeTypeSpec spec{};
    std::string                    type;
    std::string                    configSchema;

    explicit GraphNodeTypeSpecKeep(const AgentxxPluginGraphNodeTypeSpec& src) :
        spec(src),
        type(svToStr(&src.type)),
        configSchema(svToStr(&src.config_schema_json)) {
        spec.type               = agentxx::plugin::PluginStringView::from(type);
        spec.config_schema_json = agentxx::plugin::PluginStringView::from(configSchema);
    }
};

static PluginxxOperatorHandle* PLUGINXX_CALL xx_register_node_type(
    const PluginxxHost*                   host,
    const PluginxxScopeRef*               agent,
    const AgentxxPluginGraphNodeTypeSpec* spec,
    PluginxxOperatorCallback              cb,
    void*                                 ud,
    PluginxxString*                       error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (spec == nullptr || agentxx::plugin::PluginStringView::empty(&spec->type)
            || !spec->run_start) {
            setTableOpError(error_out, "register_node_type: spec, type and run_start are required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        // 长度门槛: 0 = 基线布局, 低于基线一律拒绝 (见 plugin_spec.h)
        if (!specSizeOk<AgentxxPluginGraphNodeTypeSpec>(spec->struct_size)) {
            setTableOpError(error_out, "register_node_type: spec struct_size is too small");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto keep = std::make_shared<GraphNodeTypeSpecKeep>(*spec);
        return asyncScopedTableCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "graph.register_node_type",
            [keep](
                PluginInstance*         inst,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                auto rc = mountCall(mgr, &ref, noAgent, [inst, keep](PluginMount& mount) {
                    return mount.registerGraphNodeType(inst, &keep->spec);
                });
                if (rc != AGENTXX_RESULT_OK) {
                    throw TableOpError(std::string(agentxx::plugin::resultName(rc)));
                }
                return std::string{};
            }
        );
    });
}



static PluginxxOperatorHandle* PLUGINXX_CALL xx_get_graph_json(
    const PluginxxHost*      host,
    const PluginxxScopeRef*  agent,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        return asyncQueryCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "graph.get_graph_json",
            [](PluginInstance*, PluginManager* mgr, const PluginxxScopeRef& ref) -> std::string {
                auto* mount = mountIn(mgr, &ref);
                return mount != nullptr ? mount->getGraphJson() : std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_get_graph_name(
    const PluginxxHost*      host,
    const PluginxxScopeRef*  agent,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        return asyncQueryCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "graph.get_graph_name",
            [](PluginInstance*, PluginManager* mgr, const PluginxxScopeRef& ref) -> std::string {
                auto*       mount  = mountIn(mgr, &ref);
                auto        json   = mount != nullptr ? mount->getGraphJson() : std::string{};
                std::string result = "agentxx.default";
                if (!json.empty()) {
                    try {
                        auto j = utilxx_base::Json::parse(json);
                        if (j.is_object() && j.contains("name") && j["name"].is_string()) {
                            result = j["name"].get<std::string>();
                        }
                    } catch (...) {
                    }
                }
                return result;
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_unregister_node_type(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* type,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (agentxx::plugin::PluginStringView::empty(type)) {
            setTableOpError(error_out, "unregister_node_type: type is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto typeKeep = svToStr(type);
        return asyncScopedTableCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "graph.unregister_node_type",
            [typeKeep](
                PluginInstance*         inst,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                const PluginxxStringView view{
                    typeKeep.data(), static_cast<uint64_t>(typeKeep.size())
                };
                auto rc = mountCall(mgr, &ref, noAgent, [inst, view](PluginMount& mount) {
                    return mount.unregisterGraphNodeType(inst, view);
                });
                if (rc != AGENTXX_RESULT_OK) {
                    throw TableOpError(std::string(agentxx::plugin::resultName(rc)));
                }
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_set_graph_json(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* graph_json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (agentxx::plugin::PluginStringView::empty(graph_json)) {
            setTableOpError(error_out, "set_graph_json: graph_json is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto jsonKeep = svToStr(graph_json);
        return asyncScopedTableCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "graph.set_graph_json",
            [jsonKeep](
                PluginInstance*         inst,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                const PluginxxStringView view{
                    jsonKeep.data(), static_cast<uint64_t>(jsonKeep.size())
                };
                auto rc = mountCall(mgr, &ref, noAgent, [inst, view](PluginMount& mount) {
                    return mount.setGraphJson(inst, view);
                });
                if (rc != AGENTXX_RESULT_OK) {
                    throw TableOpError(std::string(agentxx::plugin::resultName(rc)));
                }
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL
    xx_get_prompt(const PluginxxHost*      host,
                  const PluginxxScopeRef*  agent,
                  PluginxxOperatorCallback cb,
                  void*                    ud,
                  PluginxxString*          error_out) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        return asyncQueryCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "prompt.get_prompt",
            [](PluginInstance*, PluginManager* mgr, const PluginxxScopeRef& ref) -> std::string {
                return mountCall(
                    mgr,
                    &ref,
                    [] { return std::string{}; },
                    [](PluginMount& mount) { return mount.getPromptJson(); }
                );
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_set_prompt(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* prompt_json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (agentxx::plugin::PluginStringView::empty(prompt_json)) {
            setTableOpError(error_out, "set_prompt: prompt_json is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto pJson = svToStr(prompt_json);
        return asyncScopedTableCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "prompt.set_prompt",
            [pJson](
                PluginInstance*         inst,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                const PluginxxStringView view{
                    pJson.data(), static_cast<uint64_t>(pJson.size())
                };
                auto rc = mountCall(mgr, &ref, noAgent, [inst, view](PluginMount& mount) {
                    return mount.setPromptJson(inst, view);
                });
                if (rc != AGENTXX_RESULT_OK) {
                    throw TableOpError(std::string(agentxx::plugin::resultName(rc)));
                }
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_prompt_tool(
    const PluginxxHost*       host,
    const PluginxxStringView* tool_name,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (agentxx::plugin::PluginStringView::empty(tool_name)) {
            setTableOpError(error_out, "get_tool_prompt: tool name is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        // 工具提示词是主程序级配置 (与具体 agent 无关), 不取作用域引用
        auto name = svToStr(tool_name);
        return asyncTableCall</*AllowClosing=*/true>(
            host,
            cb,
            ud,
            error_out,
            "prompt.get_tool_prompt",
            [name](PluginInstance*, PluginManager* mgr) -> std::string {
                auto text = mgr->toolPromptJson(name);
                if (text.empty()) {
                    throw TableOpError("tool prompt is not configured");
                }
                return text;
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL
    xx_model_get_config(const PluginxxHost*      host,
                        const PluginxxScopeRef*  agent,
                        PluginxxOperatorCallback cb,
                        void*                    ud,
                        PluginxxString*          error_out) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        return asyncScopedTableCall</*AllowClosing=*/true>(
            host,
            agent,
            cb,
            ud,
            error_out,
            "model.get_config",
            [](PluginInstance*, PluginManager* mgr, const PluginxxScopeRef&) -> std::string {
                // 未加载模型时旧行为是返回空串, 这里空结果是正常情况, 不算失败
                return mgr->getModelConfigJson();
            }
        );
    });
}

/// 资源登记类成员的公共实现 (单个字符串入参 + 一个挂载方法)
///
/// 六个成员只差"调哪个挂载方法", 复制六遍容易漏改一处, 因此收在这里:
/// 受理阶段先校验入参并拷成自己的字符串, 工作体在 IO 线程上执行。
template<typename Fn>
static PluginxxOperatorHandle* resourceOp(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* arg,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out,
    std::string               label,
    Fn&&                      call
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (agentxx::plugin::PluginStringView::empty(arg)) {
            setTableOpError(error_out, label + ": argument is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto keep = svToStr(arg);
        return asyncScopedTableCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            std::move(label),
            [keep, call = std::forward<Fn>(call)](
                PluginInstance*         inst,
                PluginManager*          mgr,
                const PluginxxScopeRef& ref
            ) -> std::string {
                const PluginxxStringView view{
                    keep.data(), static_cast<uint64_t>(keep.size())
                };
                auto rc = mountCall(mgr, &ref, noAgent, [inst, view, &call](PluginMount& mount) {
                    return call(mount, inst, view);
                });
                if (rc != AGENTXX_RESULT_OK) {
                    throw TableOpError(std::string(agentxx::plugin::resultName(rc)));
                }
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_register_skill_dir(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* path,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return resourceOp(
        host,
        agent,
        path,
        cb,
        ud,
        error_out,
        "resources.register_skill_dir",
        [](PluginMount& mount, PluginInstance* inst, PluginxxStringView view) {
            return mount.registerSkillDir(inst, view);
        }
    );
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_unregister_skill_dir(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* path,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return resourceOp(
        host,
        agent,
        path,
        cb,
        ud,
        error_out,
        "resources.unregister_skill_dir",
        [](PluginMount& mount, PluginInstance* inst, PluginxxStringView view) {
            return mount.unregisterSkillDir(inst, view);
        }
    );
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_register_memory_file(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* path,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return resourceOp(
        host,
        agent,
        path,
        cb,
        ud,
        error_out,
        "resources.register_memory_file",
        [](PluginMount& mount, PluginInstance* inst, PluginxxStringView view) {
            return mount.registerMemoryFile(inst, view);
        }
    );
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_unregister_memory_file(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* path,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return resourceOp(
        host,
        agent,
        path,
        cb,
        ud,
        error_out,
        "resources.unregister_memory_file",
        [](PluginMount& mount, PluginInstance* inst, PluginxxStringView view) {
            return mount.unregisterMemoryFile(inst, view);
        }
    );
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_register_mcp_server(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* spec_json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return resourceOp(
        host,
        agent,
        spec_json,
        cb,
        ud,
        error_out,
        "resources.register_mcp_server",
        [](PluginMount& mount, PluginInstance* inst, PluginxxStringView view) {
            return mount.registerMcpServer(inst, view);
        }
    );
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_unregister_mcp_server(
    const PluginxxHost*       host,
    const PluginxxScopeRef*   agent,
    const PluginxxStringView* name_space,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return resourceOp(
        host,
        agent,
        name_space,
        cb,
        ud,
        error_out,
        "resources.unregister_mcp_server",
        [](PluginMount& mount, PluginInstance* inst, PluginxxStringView view) {
            return mount.unregisterMcpServer(inst, view);
        }
    );
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_get_own_resources(
    const PluginxxHost*      host,
    const PluginxxScopeRef*  agent,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        return asyncQueryCall(
            host,
            agent,
            cb,
            ud,
            error_out,
            "resources.get_own_resources",
            [](PluginInstance* inst, PluginManager* mgr, const PluginxxScopeRef& ref) -> std::string {
                auto* mount = mountIn(mgr, &ref);
                return mount != nullptr ? mount->ownResourcesJson(inst) : std::string{};
            }
        );
    });
}

static const AgentxxPluginToolsIface g_ifaceTools = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION,
    /* struct_size */ sizeof(AgentxxPluginToolsIface),
    /* register_tool */ xx_register_tool,
    /* unregister_tool */ xx_unregister_tool,
    /* call_tool */ xx_call_tool,
};

static const AgentxxPluginPermissionIface g_ifacePermission = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_VERSION,
    /* struct_size */ sizeof(AgentxxPluginPermissionIface),
    /* register_tool_permission */ xx_register_tool_permission,
    /* unregister_tool_permission */ xx_unregister_tool_permission,
    /* check_paths */ xx_check_paths,
};

static const AgentxxPluginSessionIface g_ifaceSession = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION,
    /* struct_size */ sizeof(AgentxxPluginSessionIface),
    /* get_share_store */ xx_get_share_store,
    /* emit_message_tip */ xx_emit_message_tip,
    /* add_share_store */ xx_add_share_store,
    /* get_session_work_dir */ xx_session_work_dir,
    /* is_canceled */ xx_session_Canceled,
};

static const AgentxxPluginPromptIface g_ifacePrompt = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION,
    /* struct_size */ sizeof(AgentxxPluginPromptIface),
    /* get_prompt */ xx_get_prompt,
    /* set_prompt */ xx_set_prompt,
    /* get_tool_prompt */ xx_prompt_tool,
};

static const AgentxxPluginResourcesIface g_ifaceResources = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION,
    /* struct_size */ sizeof(AgentxxPluginResourcesIface),
    /* register_skill_dir */ xx_register_skill_dir,
    /* unregister_skill_dir */ xx_unregister_skill_dir,
    /* register_memory_file */ xx_register_memory_file,
    /* unregister_memory_file */ xx_unregister_memory_file,
    /* register_mcp_server */ xx_register_mcp_server,
    /* unregister_mcp_server */ xx_unregister_mcp_server,
    /* get_own_resources */ xx_get_own_resources,
};

static const AgentxxPluginModelIface g_ifaceModel = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_MODEL_VERSION,
    /* struct_size */ sizeof(AgentxxPluginModelIface),
    /* get_config */ xx_model_get_config,
};

static const AgentxxPluginGraphIface g_ifaceGraph = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_VERSION,
    /* struct_size */ sizeof(AgentxxPluginGraphIface),
    /* register_node_type */ xx_register_node_type,
    /* unregister_node_type */ xx_unregister_node_type,
    /* get_graph_json */ xx_get_graph_json,
    /* get_graph_name */ xx_get_graph_name,
    /* set_graph_json */ xx_set_graph_json,
};

// ==================== 运行时表 (agentxx.agent.runtime) ====================
//
// 21 个入口全部走异步操作契约 (pluginxx/host/table_op.h): 受理即返回句柄, 结果经
// 完成回调交回; 未受理 (参数形状错 / 实例不可用) 返回 NULL 并写 `error_out`。
// 工作体落在主程序 IO 线程 (经 `postToIo` 投递): 根/通道/触发器记录只在那一线程
// 读写, 因此不需要锁。

/// 取参数里的字符串 (空视图 = 空串)
static std::string svText(const PluginxxStringView* value) {
    return pluginxx::svToStr(value);
}

/// runtime 表统一构造: 入参按值拷进闭包, 业务在自己的 IO 线程执行
///
/// 表成员是容器级接口 (不带 agent 作用域): 句柄 id 与查询结果都经 `payload` 交回,
/// 纯控制类成员成功时 `payload` 为空。
template<typename Fn>
static PluginxxOperatorHandle* runtimeOp(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out,
    std::string              label,
    Fn&&                     work
) {
    return asyncTableCall(
        host,
        cb,
        ud,
        error_out,
        std::move(label),
        [work = std::forward<Fn>(work)](PluginInstance* inst, PluginManager* mgr) mutable -> std::string {
            return work(inst, mgr);
        }
    );
}

/// 业务结果码 → 失败 (payload 给"成员名: 结果码名字", 有细节时一并附上)
static void throwRuntimeFailure(
    const char*       what,
    int32_t           rc,
    std::string_view  detail = {}
) {
    if (rc == AGENTXX_RESULT_OK) {
        return;
    }
    const auto name = agentxx::plugin::resultName(rc);
    if (!detail.empty()) {
        throw TableOpError(fmt::format("{}: {} ({})", what, name, detail));
    }
    throw TableOpError(fmt::format("{}: {}", what, name));
}

/// 受理阶段的参数错: 写原因并返回 NULL (受理失败, 没有完成回调)
static PluginxxOperatorHandle* rejectTableOp(PluginxxString* error_out, std::string_view msg) {
    setTableOpError(error_out, msg);
    return nullptr;
}

// 下面四个 "spec + 自持字符串" 结构解决同一个问题: spec 里的字符串视图借用调用方内存,
// 而工作体可能排到 IO 线程才执行 (那时调用方内存已不在)。只拷 spec 本身不够 —— 视图仍指
// 原内存; 所以把字符串一起拷进本对象并重新指向, 由工作体捕获该对象保活。

/// 根配置 spec + 自持字符串
struct MountRootSpecKeep {
    AgentxxPluginMountRootSpec spec{};
    std::string                name;
    std::string                configJson;
    std::string                mountPlugins;

    explicit MountRootSpecKeep(const AgentxxPluginMountRootSpec& src) :
        spec(src),
        name(svText(&src.name)),
        configJson(svText(&src.config_json)),
        mountPlugins(svText(&src.mount_plugins)) {
        spec.name          = agentxx::plugin::PluginStringView::from(name);
        spec.config_json   = agentxx::plugin::PluginStringView::from(configJson);
        spec.mount_plugins = agentxx::plugin::PluginStringView::from(mountPlugins);
    }
};

/// 通道 spec + 自持字符串
struct ChannelSpecKeep {
    AgentxxPluginChannelSpec spec{};
    std::string              rootId;
    std::string              sessionId;
    std::string              title;
    std::string              model;

    explicit ChannelSpecKeep(const AgentxxPluginChannelSpec& src) :
        spec(src),
        rootId(svText(&src.root_id)),
        sessionId(svText(&src.session_id)),
        title(svText(&src.title)),
        model(svText(&src.model)) {
        spec.root_id    = agentxx::plugin::PluginStringView::from(rootId);
        spec.session_id = agentxx::plugin::PluginStringView::from(sessionId);
        spec.title      = agentxx::plugin::PluginStringView::from(title);
        spec.model      = agentxx::plugin::PluginStringView::from(model);
    }
};

/// 触发器 spec + 自持字符串
struct TriggerSpecKeep {
    AgentxxPluginTriggerSpec spec{};
    std::string              topic;
    std::string              delivery;

    explicit TriggerSpecKeep(const AgentxxPluginTriggerSpec& src) :
        spec(src),
        topic(svText(&src.topic)),
        delivery(svText(&src.delivery)) {
        spec.topic    = agentxx::plugin::PluginStringView::from(topic);
        spec.delivery = agentxx::plugin::PluginStringView::from(delivery);
    }
};

/// 定时器 spec + 自持字符串
struct TimerSpecKeep {
    AgentxxPluginTimerSpec spec{};
    std::string            inputJson;

    explicit TimerSpecKeep(const AgentxxPluginTimerSpec& src) :
        spec(src),
        inputJson(svText(&src.input_json)) {
        spec.input_json = agentxx::plugin::PluginStringView::from(inputJson);
    }
};

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_mount_root(
    const PluginxxHost*               host,
    const AgentxxPluginMountRootSpec* spec,
    PluginxxOperatorCallback          cb,
    void*                             ud,
    PluginxxString*                   error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (spec == nullptr
            || !agentxx::plugin::specSizeOk<AgentxxPluginMountRootSpec>(spec->struct_size)) {
            setTableOpError(error_out, "mount_root: spec is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto keep = std::make_shared<MountRootSpecKeep>(*spec);
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.mount_root",
            [keep](PluginInstance* inst, PluginManager* mgr) -> std::string {
                std::string err;
                std::string rootId;
                const auto  rc = mgr->runtimeMountRoot(inst, &keep->spec, rootId, err);
                if (rc != AGENTXX_RESULT_OK) {
                    XX_LOGW("[runtime] mount_root 被拒 ({}): {}", rc, err);
                }
                throwRuntimeFailure("mount_root", rc, err);
                return rootId;
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_unmount_root(
    const PluginxxHost*       host,
    const PluginxxStringView* root_id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (agentxx::plugin::PluginStringView::empty(root_id)) {
            setTableOpError(error_out, "unmount_root: root id is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto id = svText(root_id);
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.unmount_root",
            [id](PluginInstance* inst, PluginManager* mgr) -> std::string {
                throwRuntimeFailure("unmount_root", mgr->runtimeUnmountRoot(inst, id));
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_open_channel(
    const PluginxxHost*             host,
    const AgentxxPluginChannelSpec* spec,
    PluginxxOperatorCallback        cb,
    void*                           ud,
    PluginxxString*                 error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (spec == nullptr
            || !agentxx::plugin::specSizeOk<AgentxxPluginChannelSpec>(spec->struct_size)) {
            setTableOpError(error_out, "open_channel: spec is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto keep = std::make_shared<ChannelSpecKeep>(*spec);
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.open_channel",
            [keep](PluginInstance* inst, PluginManager* mgr) -> std::string {
                std::string err;
                int64_t     channelId = 0;
                const auto  rc        = mgr->runtimeOpenChannel(inst, &keep->spec, &channelId, err);
                if (rc != AGENTXX_RESULT_OK) {
                    XX_LOGW("[runtime] open_channel 被拒 ({}): {}", rc, err);
                }
                throwRuntimeFailure("open_channel", rc, err);
                return std::to_string(channelId);
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_close_channel(
    const PluginxxHost*      host,
    int64_t                  channel_id,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.close_channel",
            [channel_id](PluginInstance* inst, PluginManager* mgr) -> std::string {
                throwRuntimeFailure("close_channel", mgr->runtimeCloseChannel(inst, channel_id));
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_send_input(
    const PluginxxHost*       host,
    int64_t                   channel_id,
    const PluginxxStringView* input_json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        auto input = svText(input_json);
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.send_input",
            [channel_id, input = std::move(input)](PluginInstance* inst, PluginManager* mgr)
                -> std::string {
                std::string err;
                const auto  rc = mgr->runtimeSendInput(inst, channel_id, input, err);
                if (rc != AGENTXX_RESULT_OK) {
                    XX_LOGW("[runtime] send_input 被拒 ({}): {}", rc, err);
                }
                throwRuntimeFailure("send_input", rc, err);
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_cancel(
    const PluginxxHost*      host,
    int64_t                  channel_id,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.cancel",
            [channel_id](PluginInstance* inst, PluginManager* mgr) -> std::string {
                throwRuntimeFailure("cancel", mgr->runtimeCancel(inst, channel_id));
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_select_model(
    const PluginxxHost*       host,
    int64_t                   channel_id,
    const PluginxxStringView* model_name,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        auto model = svText(model_name);
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.select_model",
            [channel_id, model = std::move(model)](PluginInstance* inst, PluginManager* mgr)
                -> std::string {
                throwRuntimeFailure(
                    "select_model",
                    mgr->runtimeSelectModel(inst, channel_id, model)
                );
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_respond_interrupt(
    const PluginxxHost*       host,
    int64_t                   channel_id,
    int64_t                   interrupt_id,
    const PluginxxStringView* values_json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        auto values = svText(values_json);
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.respond_interrupt",
            [channel_id, interrupt_id, values = std::move(values)](
                PluginInstance* inst,
                PluginManager*  mgr
            ) -> std::string {
                std::string err;
                const auto  rc
                    = mgr->runtimeRespondInterrupt(inst, channel_id, interrupt_id, values, err);
                if (rc != AGENTXX_RESULT_OK) {
                    XX_LOGW("[runtime] respond_interrupt 被拒 ({}): {}", rc, err);
                }
                throwRuntimeFailure("respond_interrupt", rc, err);
                return std::string{};
            }
        );
    });
}

/// 只读查询: 通道状态 / 上下文 / 运行时清单
static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_get_channel_state(
    const PluginxxHost*      host,
    int64_t                  channel_id,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.get_channel_state",
            [channel_id](PluginInstance* inst, PluginManager* mgr) -> std::string {
                auto json = mgr->runtimeChannelStateJson(inst, channel_id);
                if (json.empty()) {
                    throw TableOpError("get_channel_state: channel not found");
                }
                return json;
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_get_context_messages(
    const PluginxxHost*      host,
    int64_t                  channel_id,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.get_context_messages",
            [channel_id](PluginInstance* inst, PluginManager* mgr) -> std::string {
                auto json = mgr->channelContextMessages(inst, channel_id);
                if (json.empty()) {
                    throw TableOpError("get_context_messages: channel not found");
                }
                return json;
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_list(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.list_runtime",
            [](PluginInstance* inst, PluginManager* mgr) -> std::string {
                auto json = mgr->listRuntimeJson(inst);
                if (json.empty()) {
                    throw TableOpError("list_runtime: no runtime information");
                }
                return json;
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_register_trigger(
    const PluginxxHost*             host,
    const AgentxxPluginTriggerSpec* spec,
    PluginxxOperatorCallback        cb,
    void*                           ud,
    PluginxxString*                 error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (spec == nullptr
            || !agentxx::plugin::specSizeOk<AgentxxPluginTriggerSpec>(spec->struct_size)) {
            setTableOpError(error_out, "register_trigger: spec is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto keep = std::make_shared<TriggerSpecKeep>(*spec);
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.register_trigger",
            [keep](PluginInstance* inst, PluginManager* mgr) -> std::string {
                std::string err;
                int64_t     triggerId = 0;
                const auto  rc
                    = mgr->runtimeRegisterTrigger(inst, &keep->spec, &triggerId, err);
                if (rc != AGENTXX_RESULT_OK) {
                    XX_LOGW("[runtime] register_trigger 被拒 ({}): {}", rc, err);
                }
                throwRuntimeFailure("register_trigger", rc);
                return std::to_string(triggerId);
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_unregister_trigger(
    const PluginxxHost*      host,
    int64_t                  trigger_id,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.unregister_trigger",
            [trigger_id](PluginInstance* inst, PluginManager* mgr) -> std::string {
                throwRuntimeFailure(
                    "unregister_trigger",
                    mgr->runtimeUnregisterTrigger(inst, trigger_id)
                );
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_emit_event(
    const PluginxxHost*       host,
    const PluginxxStringView* topic,
    const PluginxxStringView* payload_json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        auto topicText   = svText(topic);
        auto payloadText = svText(payload_json);
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.emit_event",
            [topicText = std::move(topicText), payloadText = std::move(payloadText)](
                PluginInstance* inst,
                PluginManager*  mgr
            ) -> std::string {
                throwRuntimeFailure(
                    "emit_event",
                    mgr->runtimeEmitEvent(inst, topicText, payloadText)
                );
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_register_timer(
    const PluginxxHost*           host,
    const AgentxxPluginTimerSpec* spec,
    PluginxxOperatorCallback      cb,
    void*                         ud,
    PluginxxString*               error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        if (spec == nullptr
            || !agentxx::plugin::specSizeOk<AgentxxPluginTimerSpec>(spec->struct_size)) {
            setTableOpError(error_out, "register_timer: spec is required");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        auto keep = std::make_shared<TimerSpecKeep>(*spec);
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.register_timer",
            [keep](PluginInstance* inst, PluginManager* mgr) -> std::string {
                std::string err;
                int64_t     timerId = 0;
                const auto  rc      = mgr->runtimeRegisterTimer(inst, &keep->spec, &timerId, err);
                if (rc != AGENTXX_RESULT_OK) {
                    XX_LOGW("[runtime] register_timer 被拒 ({}): {}", rc, err);
                }
                throwRuntimeFailure("register_timer", rc);
                return std::to_string(timerId);
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_unregister_timer(
    const PluginxxHost*      host,
    int64_t                  timer_id,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.unregister_timer",
            [timer_id](PluginInstance* inst, PluginManager* mgr) -> std::string {
                throwRuntimeFailure(
                    "unregister_timer",
                    mgr->runtimeUnregisterTimer(inst, timer_id)
                );
                return std::string{};
            }
        );
    });
}

// ---- 后台任务 (v2 尾部追加) ----

/// 解析 spec 里的一个 JSON 文本字段 (空 = 不设置)
/// - `return` false = 文本不是合法 JSON
static bool parseSpecJson(
    const PluginxxStringView& view,
    utilxx_base::Json&        out,
    std::string&              err,
    const char*               field
) {
    const auto raw = pluginxx::svToStr(&view);
    if (raw.empty()) {
        return true;
    }
    return agentxx::util::catchError<bool>(
        [&]() -> bool {
            out = utilxx_base::Json::parse(raw);
            return true;
        },
        [&](std::string info) -> bool {
            err = fmt::format("`{}` is not valid JSON: {}", field, info);
            return false;
        }
    );
}

/// 把 JSON 数组文本写成字符串列表 (空/`[]` = 不设置)
static bool specStringList(
    const utilxx_base::Json&    src,
    std::vector<std::string>&   out,
    std::string&                err,
    const char*                 field
) {
    if (!src.is_array()) {
        err = fmt::format("`{}` must be a JSON array of strings", field);
        return false;
    }
    for (const auto& item : src) {
        if (!item.is_string()) {
            err = fmt::format("`{}` must contain only strings", field);
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_submit_async_work(
    const PluginxxHost*               host,
    const AgentxxPluginAsyncWorkSpec* spec,
    PluginxxOperatorCallback          cb,
    void*                             ud,
    PluginxxString*                   error_out
) {
    if (spec == nullptr
        || !agentxx::plugin::specSizeOk<AgentxxPluginAsyncWorkSpec>(spec->struct_size)) {
        return rejectTableOp(error_out, "submit_async_work: spec is required");
    }
    const auto text = [](const PluginxxStringView& view) { return pluginxx::svToStr(&view); };

    agent::AsyncWorkSpec work;
    work.kind = (spec->kind == 1) ? agent::AsyncWorkKind::Tool : agent::AsyncWorkKind::Agent;
    work.name = text(spec->name);
    if (work.name.empty()) {
        work.name = "plugin-task";
    }
    work.systemPrompt     = text(spec->system_prompt);
    work.message          = text(spec->message);
    work.model            = text(spec->model);
    work.creatorSessionId = text(spec->session_id);
    work.queueKey         = text(spec->queue_key);
    work.workDir          = text(spec->work_dir);
    work.language         = text(spec->language);
    work.notifyDelivery   = text(spec->notify_delivery);
    // 追加字段 (旧插件填短结构体时读不到): kind=tool 的工名与参数
    if (specHas(&AgentxxPluginAsyncWorkSpec::tool_name, spec->struct_size)) {
        work.toolName = text(spec->tool_name);
    }
    if (work.kind == agent::AsyncWorkKind::Tool && work.toolName.empty()) {
        XX_LOGW("[runtime] submit_async_work 被拒: kind=tool 需要 tool_name");
        return rejectTableOp(error_out, "submit_async_work: tool name is required for kind=tool");
    }
    if (specHas(&AgentxxPluginAsyncWorkSpec::tool_args_json, spec->struct_size)) {
        utilxx_base::Json toolArgs;
        std::string       toolErr;
        if (!parseSpecJson(spec->tool_args_json, toolArgs, toolErr, "tool_args_json")) {
            XX_LOGW("[runtime] submit_async_work 被拒: {}", toolErr);
            return rejectTableOp(error_out, "submit_async_work: invalid arguments");
        }
        if (!toolArgs.is_null()) {
            if (!toolArgs.is_object()) {
                XX_LOGW("[runtime] submit_async_work 被拒: `tool_args_json` must be a JSON object");
                return rejectTableOp(error_out, "submit_async_work: invalid arguments");
            }
            work.toolArgs = std::move(toolArgs);
        }
    }
    if (spec->interrupt_policy >= 0) {
        work.interruptPolicy
            = static_cast<agent::AsyncInterruptPolicy>(spec->interrupt_policy);
    }
    if (spec->session_store >= 0) {
        work.sessionStore = spec->session_store != 0;
    }
    work.interruptTimeout = std::chrono::milliseconds{spec->interrupt_timeout_ms};
    // 插件侧的 `run_timeout_ms` 就是运行超时 (排队与延时不计入)
    work.runTimeout = std::chrono::milliseconds{spec->run_timeout_ms};
    // 追加字段: 起跑延时与排队时限 (旧插件填短结构体时读不到, 保持默认不限)
    if (specHas(&AgentxxPluginAsyncWorkSpec::delay_ms, spec->struct_size)) {
        work.delay = std::chrono::milliseconds{std::max<int64_t>(0, spec->delay_ms)};
    }
    if (specHas(&AgentxxPluginAsyncWorkSpec::queue_timeout_ms, spec->struct_size)) {
        work.queueTimeout
            = std::chrono::milliseconds{std::max<int64_t>(0, spec->queue_timeout_ms)};
    }

    std::string        err;
    utilxx_base::Json  json;
    std::vector<std::string> list;
    if (!parseSpecJson(spec->tools_json, json, err, "tools_json")) {
        XX_LOGW("[runtime] submit_async_work 被拒: {}", err);
        return rejectTableOp(error_out, "submit_async_work: invalid arguments");
    }
    if (!json.is_null()) {
        if (!json.is_array()) {
            XX_LOGW("[runtime] submit_async_work 被拒: `tools_json` must be a JSON array");
            return rejectTableOp(error_out, "submit_async_work: invalid arguments");
        }
        work.tools = json;
    }
    json = utilxx_base::Json{};
    if (!parseSpecJson(spec->plugins_json, json, err, "plugins_json")) {
        XX_LOGW("[runtime] submit_async_work 被拒: {}", err);
        return rejectTableOp(error_out, "submit_async_work: invalid arguments");
    }
    if (!json.is_null()) {
        if (!json.is_object()) {
            XX_LOGW("[runtime] submit_async_work 被拒: `plugins_json` must be a JSON object");
            return rejectTableOp(error_out, "submit_async_work: invalid arguments");
        }
        work.plugins = json;
    }

    struct ListField {
        const PluginxxStringView* view;
        const char*               name;
        std::vector<std::string>* target;
    };
    const ListField listFields[] = {
        {&spec->mcp_servers_json,   "mcp_servers_json",   &work.mcpServers  },
        {&spec->skills_json,        "skills_json",        &work.skills      },
        {&spec->memory_files_json,  "memory_files_json",  &work.memoryFiles },
        {&spec->rag_docs_json,      "rag_docs_json",      &work.ragDocs     },
        {&spec->permission_allow_json, "permission_allow_json", &work.permissionAllowPaths},
        {&spec->permission_deny_json,  "permission_deny_json",  &work.permissionDenyPaths },
    };
    for (const auto& field : listFields) {
        json = utilxx_base::Json{};
        if (!parseSpecJson(*field.view, json, err, field.name)) {
            XX_LOGW("[runtime] submit_async_work 被拒: {}", err);
            return rejectTableOp(error_out, "submit_async_work: invalid arguments");
        }
        if (json.is_null() || (json.is_array() && json.empty())) {
            continue;
        }
        if (!specStringList(json, *field.target, err, field.name)) {
            XX_LOGW("[runtime] submit_async_work 被拒: {}", err);
            return rejectTableOp(error_out, "submit_async_work: invalid arguments");
        }
    }
    (void)list;

    // 事件回调: 跨边界函数指针与 user_data 按值带进 C++ 闭包
    if (spec->on_event != nullptr) {
        auto fn = spec->on_event;
        auto ud = spec->user_data;
        work.onEvent
            = [fn, ud](agent::AsyncWorkEventPhase phase, const utilxx_base::Json& payload) {
                  const std::string        text = payload.dump();
                  const PluginxxStringView view{text.data(), text.size()};
                  fn(ud, static_cast<int32_t>(phase), &view);
              };
    }

    auto specPtr = std::make_shared<agent::AsyncWorkSpec>(std::move(work));
    return runtimeOp(
        host,
        cb,
        ud,
        error_out,
        "runtime.submit_async_work",
        [specPtr](PluginInstance* inst, PluginManager* mgr) -> std::string {
            auto outcome = mgr->runtimeSubmitAsyncWork(inst, std::move(*specPtr));
            if (outcome.code != AGENTXX_RESULT_OK) {
                XX_LOGW("[runtime] submit_async_work 被拒 ({}): {}", outcome.code, outcome.error);
                throw TableOpError(outcome.error.empty() ? "submit_async_work failed"
                                                         : outcome.error);
            }
            return outcome.asyncId;
        }
    );
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_cancel_async_work(
    const PluginxxHost*       host,
    const PluginxxStringView* async_id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        auto id = svText(async_id);
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.cancel_async_work",
            [id = std::move(id)](PluginInstance* inst, PluginManager* mgr) -> std::string {
                throwRuntimeFailure(
                    "cancel_async_work",
                    mgr->runtimeCancelAsyncWork(inst, id)
                );
                return std::string{};
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_get_async_work(
    const PluginxxHost*       host,
    const PluginxxStringView* async_id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        auto id = svText(async_id);
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.get_async_work",
            [id = std::move(id)](PluginInstance* inst, PluginManager* mgr) -> std::string {
                auto compact = mgr->runtimeGetAsyncWork(inst, id);
                if (compact.empty()) {
                    // 不存在 / 不是本实例提交的: 两者都不给内容, 按"找不到"交回
                    throw TableOpError("get_async_work: not_found");
                }
                return compact;
            }
        );
    });
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_runtime_list_async_work(
    const PluginxxHost*       host,
    const PluginxxStringView* session_id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        auto sessionId = svText(session_id);
        return runtimeOp(
            host,
            cb,
            ud,
            error_out,
            "runtime.list_async_work",
            [sessionId = std::move(sessionId)](PluginInstance* inst, PluginManager* mgr)
                -> std::string {
                return mgr->runtimeListAsyncWork(inst, sessionId);
            }
        );
    });
}

static const AgentxxPluginRuntimeIface g_ifaceRuntime = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME_VERSION,
    /* struct_size */ sizeof(AgentxxPluginRuntimeIface),
    /* mount_root */ xx_runtime_mount_root,
    /* unmount_root */ xx_runtime_unmount_root,
    /* open_channel */ xx_runtime_open_channel,
    /* close_channel */ xx_runtime_close_channel,
    /* send_input */ xx_runtime_send_input,
    /* cancel */ xx_runtime_cancel,
    /* select_model */ xx_runtime_select_model,
    /* respond_interrupt */ xx_runtime_respond_interrupt,
    /* get_channel_state */ xx_runtime_get_channel_state,
    /* get_context_messages */ xx_runtime_get_context_messages,
    /* list_runtime */ xx_runtime_list,
    /* register_trigger */ xx_runtime_register_trigger,
    /* unregister_trigger */ xx_runtime_unregister_trigger,
    /* emit_event */ xx_runtime_emit_event,
    /* register_timer */ xx_runtime_register_timer,
    /* unregister_timer */ xx_runtime_unregister_timer,
    /* submit_async_work */ xx_runtime_submit_async_work,
    /* cancel_async_work */ xx_runtime_cancel_async_work,
    /* get_async_work */ xx_runtime_get_async_work,
    /* list_async_work */ xx_runtime_list_async_work,
};

const void* PLUGINXX_CALL xx_query_interface(const PluginxxHost*, const PluginxxStringView* iid);

// ==================== 网络表 (`<应用名>.net`) ====================

/// 同步网络操作: op + 参数 JSON → 结果 JSON
///
/// 网络服务自有线程 (内部有界等待), 所以这里**不**占管理器的 io 线程。
static int32_t PLUGINXX_CALL xx_net_call(
    const PluginxxHost*       host,
    const PluginxxStringView* op,
    const PluginxxStringView* args_json,
    PluginxxString*           out_json
) {
    return agentxx::plugin::guardVtableCall(PLUGINXX_RESULT_ERR_INTERNAL, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(op)) {
            return PLUGINXX_RESULT_ERR_ARG;
        }
        const std::string opStr{op->data, static_cast<size_t>(op->size)};
        const std::string argsStr
            = args_json && args_json->data
                  ? std::string{args_json->data, static_cast<size_t>(args_json->size)}
                  : std::string{"{}"};
        std::string       result;
        const int32_t     rc = mgr->netCall(inst->name, opStr, argsStr, &result);
        if (rc == PLUGINXX_RESULT_OK && out_json != nullptr) {
            hostMemorySetString(out_json, result);
        } else if (rc != PLUGINXX_RESULT_OK && out_json != nullptr && !result.empty()) {
            // 失败但带了可读原因: 平台侧只看结果码, 这里顺手把原因写出来便于排查
            hostMemorySetString(out_json, result);
        }
        return rc;
    });
}

/// 异步网络操作: 受理成功写 requestId, 结果恰好一次经 `notify->done` 给出
static int32_t PLUGINXX_CALL xx_net_call_async(
    const PluginxxHost*           host,
    const PluginxxStringView*     op,
    const PluginxxStringView*     args_json,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               out_json,
    int64_t*                      out_request_id
) {
    return agentxx::plugin::guardVtableCall(PLUGINXX_RESULT_ERR_INTERNAL, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(op) || !notify
            || !notify->done) {
            return PLUGINXX_RESULT_ERR_ARG;
        }
        const std::string opStr{op->data, static_cast<size_t>(op->size)};
        const std::string argsStr
            = args_json && args_json->data
                  ? std::string{args_json->data, static_cast<size_t>(args_json->size)}
                  : std::string{"{}"};
        // 完成通知要跨线程保管: 拷贝成自有副本 (插件侧只要求恰好一次)
        PluginxxOperatorNotify notifyCopy = *notify;
        std::string             accepted;
        int64_t                 requestId = 0;
        const int32_t           rc        = mgr->netCallAsync(
            inst->name,
            opStr,
            argsStr,
            [](void* ud, int32_t status, const PluginxxStringView* payload) {
                auto* copy = static_cast<PluginxxOperatorNotify*>(ud);
                if (copy && copy->done) {
                    copy->done(copy->host_ud, status, payload);
                }
                delete copy;
            },
            new PluginxxOperatorNotify(notifyCopy),
            &accepted,
            &requestId,
            std::weak_ptr<PluginInstance>(inst->sharedSelf<PluginInstance>())
        );
        if (rc != PLUGINXX_RESULT_OK) {
            if (out_json != nullptr && !accepted.empty()) {
                hostMemorySetString(out_json, accepted);
            }
            return rc;
        }
        if (out_request_id != nullptr) {
            *out_request_id = requestId;
        }
        if (out_json != nullptr && !accepted.empty()) {
            hostMemorySetString(out_json, accepted);
        }
        return rc;
    });
}

/// 取消进行中的请求
static int32_t PLUGINXX_CALL xx_net_cancel(
    const PluginxxHost* host,
    int64_t             request_id,
    PluginxxString*     out_json
) {
    return agentxx::plugin::guardVtableCall(PLUGINXX_RESULT_ERR_INTERNAL, [&]() -> int32_t {
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || request_id <= 0) {
            return PLUGINXX_RESULT_ERR_ARG;
        }
        std::string   result;
        const int32_t rc = mgr->netCancel(inst->name, request_id, &result);
        if (out_json != nullptr && !result.empty()) {
            hostMemorySetString(out_json, result);
        }
        return rc;
    });
}

static const PluginxxNetIface g_ifaceNet = {
    /* version */ PLUGINXX_IFACE_NET_VERSION,
    /* struct_size */ sizeof(PluginxxNetIface),
    /* call */ xx_net_call,
    /* call_async */ xx_net_call_async,
    /* cancel */ xx_net_cancel,
};

static const PluginxxHostVtable g_hostVtable = {
    /* alloc */ xx_alloc,
    /* free */ xx_free,
    /* query_interface */ xx_query_interface,
};

const PluginxxHostVtable* PluginManager::hostVtable() {
    return &g_hostVtable;
}

const void* PLUGINXX_CALL xx_query_interface(const PluginxxHost*, const PluginxxStringView* iid) {
    if (!iid || !iid->data) {
        return nullptr;
    }
    const std::string_view n{iid->data, static_cast<size_t>(iid->size)};
    if (n == "__vtable") {
        return &g_hostVtable;
    }
    // 通用表 (log/json/config/plugins/events/scheduler/tasks/
    // cancel/capabilities) 由 cxx_pluginxx 提供 (见 pluginxx/host/tables_impl.h),
    // 主程序侧的业务表在下面逐个分发。
    if (const void* generic = pluginxx::queryGenericPluginIface<PluginInstance, PluginManager>(n)) {
        return generic;
    }
    if (n == PLUGINXX_IFACE_NET) {
        return &g_ifaceNet;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_TOOLS) {
        return &g_ifaceTools;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION) {
        return &g_ifacePermission;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_SESSION) {
        return &g_ifaceSession;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT) {
        return &g_ifaceContext;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_PROMPT) {
        return &g_ifacePrompt;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES) {
        return &g_ifaceResources;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_MODEL) {
        return &g_ifaceModel;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_GRAPH) {
        return &g_ifaceGraph;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_MOUNT) {
        return &g_ifaceMount;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME) {
        return &g_ifaceRuntime;
    }
    return nullptr;
}

// ==================== 主程序状态访问辅助实现 ====================

static std::shared_ptr<agentxx::agent::AgentResourceApplier> getResourceApplier(
    const std::weak_ptr<agentxx::agent::AgentContext>& ctxWeak,
    std::string_view                                   opLabel
) {
    auto ctx = ctxWeak.lock();
    if (!ctx) {
        return nullptr;
    }
    if (!ctx->resourceApplier) {
        XX_LOGW(
            "Plugin resource op `{}` ignored: no resource applier installed (BaseAgent?)",
            opLabel
        );
        return nullptr;
    }
    return ctx->resourceApplier;
}

int PluginMount::registerSkillDir(PluginInstance* inst, PluginxxStringView path) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&path)) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "Plugin `{}` registerSkillDir rejected: instance is closing or disabled",
            inst->name
        );
        return AGENTXX_RESULT_ERR_STATE;
    }
    if (resourcesFrozen(inst)) {
        XX_LOGW(
            "Plugin `{}` registerSkillDir rejected: resources frozen (immutable after init)",
            inst->name
        );
        return AGENTXX_RESULT_ERR_STATE;
    }
    auto ap = getResourceApplier(agent_, "register_skill_dir");
    if (!ap) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    std::string pathStr = svToStr(path);
    std::string err;
    if (!ap->addSkillDir(inst->name, pathStr, err)) {
        XX_LOGW("Plugin `{}` register skill dir failed: {}", inst->name, err);
        return AGENTXX_RESULT_ERR_FAILED;
    }
    XX_LOGI("Plugin `{}` registered skill dir `{}`", inst->name, pathStr);
    return AGENTXX_RESULT_OK;
}

int PluginMount::unregisterSkillDir(PluginInstance* inst, PluginxxStringView path) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&path)) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    if (resourcesFrozen(inst)) {
        XX_LOGW(
            "Plugin `{}` unregisterSkillDir rejected: resources frozen (immutable after init)",
            inst->name
        );
        return AGENTXX_RESULT_ERR_STATE;
    }
    auto ap = getResourceApplier(agent_, "unregister_skill_dir");
    if (!ap) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    std::string pathStr = svToStr(path);
    if (!ap->removeSkillDir(inst->name, pathStr)) {
        return AGENTXX_RESULT_ERR_NOT_FOUND;
    }
    XX_LOGI("Plugin `{}` unregistered skill dir `{}`", inst->name, pathStr);
    return AGENTXX_RESULT_OK;
}

int PluginMount::registerMemoryFile(PluginInstance* inst, PluginxxStringView path) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&path)) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "Plugin `{}` registerMemoryFile rejected: instance is closing or disabled",
            inst->name
        );
        return AGENTXX_RESULT_ERR_STATE;
    }
    if (resourcesFrozen(inst)) {
        XX_LOGW(
            "Plugin `{}` registerMemoryFile rejected: resources frozen (immutable after init)",
            inst->name
        );
        return AGENTXX_RESULT_ERR_STATE;
    }
    auto ap = getResourceApplier(agent_, "register_memory_file");
    if (!ap) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    std::string pathStr = svToStr(path);
    std::string err;
    if (!ap->addMemoryFile(inst->name, pathStr, err)) {
        XX_LOGW("Plugin `{}` register memory file failed: {}", inst->name, err);
        return AGENTXX_RESULT_ERR_FAILED;
    }
    XX_LOGI("Plugin `{}` registered memory file `{}`", inst->name, pathStr);
    return AGENTXX_RESULT_OK;
}

int PluginMount::unregisterMemoryFile(PluginInstance* inst, PluginxxStringView path) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&path)) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    if (resourcesFrozen(inst)) {
        XX_LOGW(
            "Plugin `{}` unregisterMemoryFile rejected: resources frozen (immutable after init)",
            inst->name
        );
        return AGENTXX_RESULT_ERR_STATE;
    }
    auto ap = getResourceApplier(agent_, "unregister_memory_file");
    if (!ap) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    std::string pathStr = svToStr(path);
    if (!ap->removeMemoryFile(inst->name, pathStr)) {
        return AGENTXX_RESULT_ERR_NOT_FOUND;
    }
    XX_LOGI("Plugin `{}` unregistered memory file `{}`", inst->name, pathStr);
    return AGENTXX_RESULT_OK;
}

int PluginMount::registerMcpServer(PluginInstance* inst, PluginxxStringView specJson) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&specJson)) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "Plugin `{}` registerMcpServer rejected: instance is closing or disabled",
            inst->name
        );
        return AGENTXX_RESULT_ERR_STATE;
    }
    if (resourcesFrozen(inst)) {
        XX_LOGW(
            "Plugin `{}` registerMcpServer rejected: resources frozen (immutable after init)",
            inst->name
        );
        return AGENTXX_RESULT_ERR_STATE;
    }
    auto ap = getResourceApplier(agent_, "register_mcp_server");
    if (!ap) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    try {
        auto j = utilxx_base::Json::parse(
            std::string_view{specJson.data, static_cast<size_t>(specJson.size)}
        );
        auto ns         = j.value("namespace", std::string{});
        auto url        = j.value("url", std::string{});
        int  timeoutSec = 120;
        if (j.contains("timeout")) {
            timeoutSec = j.value("timeout", 120);
        }
        if (ns.empty() || url.empty()) {
            XX_LOGW("Plugin `{}` register_mcp_server failed: namespace/url required", inst->name);
            return AGENTXX_RESULT_ERR_ARG;
        }
        agentxx::agent::McpServerConfig cfg;
        cfg.url         = url;
        cfg.toolTimeout = std::chrono::seconds{std::max(timeoutSec, 0)};
        std::string err;
        if (!ap->addMcpServer(inst->name, ns, cfg, err)) {
            XX_LOGW("Plugin `{}` register mcp server failed: {}", inst->name, err);
            return AGENTXX_RESULT_ERR_FAILED;
        }
        XX_LOGI("Plugin `{}` registered mcp server `{}` ({})", inst->name, ns, url);
        return AGENTXX_RESULT_OK;
    } catch (const std::exception& e) {
        XX_LOGE("Plugin `{}` register_mcp_server invalid json: {}", inst->name, e.what());
        return AGENTXX_RESULT_ERR_JSON;
    }
}

int PluginMount::unregisterMcpServer(PluginInstance* inst, PluginxxStringView nameSpace) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&nameSpace)) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    if (resourcesFrozen(inst)) {
        XX_LOGW(
            "Plugin `{}` unregisterMcpServer rejected: resources frozen (immutable after init)",
            inst->name
        );
        return AGENTXX_RESULT_ERR_STATE;
    }
    auto ap = getResourceApplier(agent_, "unregister_mcp_server");
    if (!ap) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    std::string nsStr = svToStr(nameSpace);
    if (!ap->removeMcpServer(inst->name, nsStr)) {
        return AGENTXX_RESULT_ERR_NOT_FOUND;
    }
    XX_LOGI("Plugin `{}` unregistered mcp server `{}`", inst->name, nsStr);
    return AGENTXX_RESULT_OK;
}

std::string PluginMount::ownResourcesJson(const PluginInstance* inst) {
    if (!inst) {
        return {};
    }
    auto c = agent_.lock();
    if (!c || !c->resourceApplier) {
        return {};
    }
    auto snap    = c->resourceApplier->ownedBy(inst->name);
    auto toArray = [](const std::vector<std::string>& v) {
        utilxx_base::Json a = utilxx_base::Json::array();
        for (const auto& s : v) {
            a.push_back(s);
        }
        return a;
    };
    utilxx_base::Json out;
    out["skills"] = toArray(snap.skillDirs);
    out["memory"] = toArray(snap.memoryFiles);
    out["mcp"]    = toArray(snap.mcpNamespaces);
    return out.dump();
}

void PluginManager::applyDeclaredResources(
    PluginInstance&                        inst,
    const plugin::PluginManifestResources& resources
) {
    // 加载阶段只记下来: 资源是 agent 级实体, 由 [mountOneAsync] 在每个挂载上应用一次
    // (MCP 每个挂载各一条连接), 拆挂载时由同一路径摘除
    inst.manifestResources = resources;
}

std::string PluginMount::getPromptJson() {
    auto c = agent_.lock();
    if (!c || !c->agentConfig) {
        return {};
    }
    return c->agentConfig->prompt.toJson().dump();
}

namespace {

/// prompt 贡献模型内部工具（见 [PluginMount::PromptKeyState]）。
///
/// 键名约定：
/// - `system`          -> agentConfig->prompt.systemPrompt
/// - `append:<key>`    -> appendSystemPrompts[key]
/// - `tool:<toolName>` -> toolPrompt[toolName]（结构化值：depict + args）
std::string promptSystemKey() {
    return "system";
}

std::string promptAppendKey(const std::string& key) {
    return "append:" + key;
}

std::string promptToolKey(const std::string& toolName) {
    return "tool:" + toolName;
}

/// 读取键当前值（`nullopt` = 键不存在）。
std::optional<PluginMount::PromptValue>
    readPromptKey(const agentxx::agent::AgentPrompt& prompt, const std::string& key) {
    PluginMount::PromptValue value;
    if (key == promptSystemKey()) {
        // systemPrompt 始终存在（空串表示没有系统提示词）。
        value.isTool = false;
        value.text   = prompt.systemPrompt;
        return value;
    }
    if (key.size() > 7 && key.compare(0, 7, "append:") == 0) {
        const std::string name = key.substr(7);
        auto              it   = prompt.appendSystemPrompts.find(name);
        if (it == prompt.appendSystemPrompts.end()) {
            return std::nullopt;
        }
        value.isTool = false;
        value.text   = it->second;
        return value;
    }
    if (key.size() > 5 && key.compare(0, 5, "tool:") == 0) {
        const std::string name = key.substr(5);
        auto              it   = prompt.toolPrompt.find(name);
        if (it == prompt.toolPrompt.end()) {
            return std::nullopt;
        }
        value.isTool = true;
        value.tool   = it->second;
        return value;
    }
    return std::nullopt;
}

/// 写入键值（`nullopt` = 删除该键；systemPrompt 视为写空串）。
void writePromptKey(
    agentxx::agent::AgentPrompt&                     prompt,
    const std::string&                               key,
    const std::optional<PluginMount::PromptValue>& value
) {
    if (key == promptSystemKey()) {
        prompt.systemPrompt = (value && !value->isTool) ? value->text : std::string{};
        return;
    }
    if (key.size() > 7 && key.compare(0, 7, "append:") == 0) {
        const std::string name = key.substr(7);
        if (value && !value->isTool) {
            prompt.appendSystemPrompts[name] = value->text;
        } else {
            prompt.appendSystemPrompts.erase(name);
        }
        return;
    }
    if (key.size() > 5 && key.compare(0, 5, "tool:") == 0) {
        const std::string name = key.substr(5);
        if (value && value->isTool && value->tool.has_value()) {
            prompt.toolPrompt[name] = *value->tool;
        } else {
            prompt.toolPrompt.erase(name);
        }
    }
}

/// 两次取值是否相同（用于发现"主程序之外"的修改并 rebase 基础值）。
bool samePromptValue(
    const std::optional<PluginMount::PromptValue>& a,
    const std::optional<PluginMount::PromptValue>& b
) {
    if (a.has_value() != b.has_value()) {
        return false;
    }
    if (!a) {
        return true;
    }
    if (a->isTool != b->isTool) {
        return false;
    }
    if (a->isTool) {
        if (a->tool.has_value() != b->tool.has_value()) {
            return false;
        }
        if (!a->tool) {
            return true;
        }
        return a->tool->depict == b->tool->depict && a->tool->args == b->tool->args;
    }
    return a->text == b->text;
}

/// 把 toolPrompt JSON 子对象合并到已有值上（与 AgentPrompt::mergeFromJson 的
/// 部分覆盖语义一致：只覆盖出现的 depict/args 子字段）。
PluginMount::PromptValue mergeToolPromptValue(
    const std::optional<PluginMount::PromptValue>& current,
    const utilxx_base::Json&                         spec
) {
    PluginMount::PromptValue value;
    value.isTool = true;
    agentxx::agent::ToolPrompt tool;
    if (current && current->isTool && current->tool.has_value()) {
        tool = *current->tool;
    }
    if (spec.is_object()) {
        if (spec.contains("depict") && spec["depict"].is_string()) {
            tool.depict = spec["depict"].get<std::string>();
        }
        if (spec.contains("args") && spec["args"].is_object()) {
            for (const auto& [argName, argValue] : spec["args"].items()) {
                if (argValue.is_string()) {
                    tool.args[std::string{argName}] = argValue.get<std::string>();
                }
            }
        }
    }
    value.tool = std::move(tool);
    return value;
}

} // namespace

/// 重新合成单个 prompt 键的有效值：
///   基础值（首次贡献前；被外部修改时 rebase）⊕ 按 sequence 顺序的全部存活贡献
void PluginMount::recomposePromptKey(const std::string& key) {
    auto c = agent_.lock();
    if (!c || !c->agentConfig) {
        return;
    }
    auto&      prompt  = c->agentConfig->prompt;
    auto&      state   = promptKeys_[key];
    const auto current = readPromptKey(prompt, key);

    if (!state.base.has_value() && state.contributions.empty() && !state.applied.has_value()) {
        // 首次接触该键：记录基础值
        state.base = current;
    } else if (!samePromptValue(current, state.applied)) {
        // 主程序之外（用户或其他代码）改过：以当前值为新基础，贡献在其上重新应用
        state.base = current;
    }

    std::vector<std::pair<uint64_t, const PromptValue*>> ordered;
    ordered.reserve(state.contributions.size());
    for (const auto& [owner, entry] : state.contributions) {
        (void)owner;
        ordered.emplace_back(entry.first, &entry.second);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    std::optional<PromptValue> effective = state.base;
    for (const auto& [sequence, value] : ordered) {
        (void)sequence;
        effective = *value;
    }
    writePromptKey(prompt, key, effective);
    state.applied = effective;
}

void PluginMount::removePromptContributions(std::string_view owner) {
    const std::string        ownerName{owner};
    std::vector<std::string> touched;
    for (auto& [key, state] : promptKeys_) {
        if (state.contributions.erase(ownerName) > 0) {
            touched.push_back(key);
        }
    }
    for (const auto& key : touched) {
        recomposePromptKey(key);
    }
}

int PluginMount::setPromptJson(PluginInstance* inst, PluginxxStringView prompt_json) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&prompt_json)) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("Plugin `{}` setPromptJson rejected: instance is closing or disabled", inst->name);
        return AGENTXX_RESULT_ERR_STATE;
    }
    auto c = agent_.lock();
    if (!c || !c->agentConfig) {
        return AGENTXX_RESULT_ERR_STATE;
    }
    try {
        auto j = utilxx_base::Json::parse(
            std::string_view{prompt_json.data, static_cast<size_t>(prompt_json.size)}
        );
        if (!j.is_object()) {
            return AGENTXX_RESULT_ERR_JSON;
        }

        std::vector<std::string> touched;
        /// 记录/更新本 owner 对某个键的贡献，并登记待重新合成。
        auto record = [&](const std::string& key, PromptValue value) {
            auto& state                     = promptKeys_[key];
            state.contributions[inst->name] = {++promptSequence_, std::move(value)};
            touched.push_back(key);
        };

        if (j.contains("systemPrompt") && j["systemPrompt"].is_string()) {
            PromptValue value;
            value.isTool = false;
            value.text   = j["systemPrompt"].get<std::string>();
            record(promptSystemKey(), std::move(value));
        }
        if (j.contains("appendSystemPrompts") && j["appendSystemPrompts"].is_object()) {
            for (const auto& [key, value] : j["appendSystemPrompts"].items()) {
                if (!value.is_string()) {
                    continue;
                }
                PromptValue contribution;
                contribution.isTool = false;
                contribution.text   = value.get<std::string>();
                record(promptAppendKey(std::string{key}), std::move(contribution));
            }
        }
        if (j.contains("toolPrompt") && j["toolPrompt"].is_object()) {
            for (const auto& [toolName, spec] : j["toolPrompt"].items()) {
                if (!spec.is_object()) {
                    continue;
                }
                const std::string key = promptToolKey(std::string{toolName});
                // 与 mergeFromJson 一致：在"当前有效值"上做部分覆盖
                record(key, mergeToolPromptValue(readPromptKey(c->agentConfig->prompt, key), spec));
            }
        }

        for (const auto& key : touched) {
            recomposePromptKey(key);
        }
        return AGENTXX_RESULT_OK;
    } catch (...) {
        return AGENTXX_RESULT_ERR_JSON;
    }
}

std::string PluginManager::getPluginArgsJson(PluginInstance* inst) {
    if (!inst) {
        return "{}";
    }
    return inst->args.is_object() ? inst->args.dump() : "{}";
}

std::string PluginManager::getPluginConfigPath(PluginInstance* inst) {
    if (!inst) {
        return {};
    }
    return inst->configPath;
}

PluginxxString
    PluginMount::getShareStore(PluginInstance* inst, PluginxxStringView session_id, int64_t id) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&session_id) || id < 0) {
        return PluginxxString{nullptr, 0};
    }
    auto ctx = agent_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return PluginxxString{nullptr, 0};
    }
    std::string sid = svToStr(session_id);
    // id 未分配 (大于自增 id) 时中间件按参数错误抛异常: 插件跨边界不抛异常,
    // 统一按"无此条目"返回空 (与条目不存在同一处理)
    auto val = agentxx::util::catchError<std::optional<std::string>>(
        [&]() -> std::optional<std::string> {
            return ctx->middlewareHandleContext->getShareStoreItemValue(
                sid,
                static_cast<size_t>(id)
            );
        },
        [&](std::string errmsg) -> std::optional<std::string> {
            XX_LOGD("PluginMount::getShareStore({}, {}) failed: {}", sid, id, errmsg);
            return std::nullopt;
        }
    );
    if (!val.has_value()) {
        return PluginxxString{nullptr, 0};
    }
    return agentxx::plugin::hostMemoryCreateString(*val);
}

int64_t PluginMount::addShareStore(
    PluginInstance*    inst,
    PluginxxStringView session_id,
    PluginxxStringView content
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&session_id)) {
        return -1;
    }
    auto ctx = agent_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return -1;
    }
    std::string sid  = svToStr(session_id);
    std::string text = svToStr(content);
    return static_cast<int64_t>(ctx->middlewareHandleContext->addShareStoreItemValue(sid, text));
}

void PluginMount::emitMessageTip(
    PluginInstance*    inst,
    PluginxxStringView session_id,
    PluginxxStringView text,
    int32_t            level
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&session_id)
        || agentxx::plugin::PluginStringView::empty(&text)) {
        return;
    }
    auto ctx = agent_.lock();
    if (!ctx) {
        return;
    }
    std::string sid     = svToStr(session_id);
    auto        session = ctx->getSession(sid);
    if (!session || !session->io) {
        return;
    }
    agentxx::agent::WireDelta delta;
    delta.type    = agentxx::agent::WireDelta::Type::MessageUITip;
    delta.text    = svToStr(text);
    delta.tipType = level >= 2 ? agentxx::agent::WireDelta::TipType::Error
                               : (level == 1 ? agentxx::agent::WireDelta::TipType::Warning
                                             : agentxx::agent::WireDelta::TipType::Info);
    delta.seq     = session->nextDeltaSeq();
    session->io->sendToPeer(delta);
}

PluginxxString PluginMount::getSessionMessages(PluginxxStringView session_id) {
    auto ctx = agent_.lock();
    if (!ctx || !ctx->sessions || agentxx::plugin::PluginStringView::empty(&session_id)) {
        return PluginxxString{nullptr, 0};
    }
    auto session = ctx->sessions->get(svToStr(session_id));
    if (!session) {
        return PluginxxString{nullptr, 0};
    }
    // 会话是上下文的唯一权威: 取 Json 形态 (惰性生成) 后按主程序 alloc 约定复制出去
    auto jsonText = session->llmMessagesJson().dump();
    return agentxx::plugin::hostMemoryCreateString(jsonText);
}

int64_t PluginMount::sessionMessagesCount(PluginxxStringView session_id) {
    auto ctx = agent_.lock();
    if (!ctx || !ctx->sessions || agentxx::plugin::PluginStringView::empty(&session_id)) {
        return -1;
    }
    auto session = ctx->sessions->get(svToStr(session_id));
    if (!session) {
        return -1;
    }
    return static_cast<int64_t>(session->messagesCount());
}

size_t PluginMount::writeSessionMessages(
    std::string_view         session_id,
    const utilxx_base::Json& messages,
    bool                     overwrite
) {
    auto ctx = agent_.lock();
    if (!ctx || !ctx->sessions || session_id.empty()) {
        return 0;
    }
    auto session = ctx->sessions->getOrCreate(session_id);
    if (!session) {
        return 0;
    }

    std::vector<neograph::ChatMessage> msgs;
    if (messages.is_array()) {
        msgs.reserve(messages.size());
        for (const auto& item : messages) {
            neograph::ChatMessage msg;
            neograph::json        neoItem = agentxx::util::toNeographJson(item);
            neograph::from_json(neoItem, msg);
            msgs.push_back(std::move(msg));
        }
    }

    if (overwrite) {
        // 空列表覆盖视为误写: 按旧契约读不到消息的插件可能以为上下文为空,
        // 直接覆盖会把整段上下文清空, 这里忽略并记日志
        if (msgs.empty() && session->messagesCount() > 0) {
            XX_LOGW(
                "PluginManager: overwrite session `{}` with an empty message list ignored "
                "(current count={})",
                session_id,
                session->messagesCount()
            );
            return 0;
        }
        session->replaceMessages(std::move(msgs));
        return session->messagesCount();
    }
    if (msgs.empty()) {
        return 0;
    }
    const size_t appended = msgs.size();
    session->appendMessages(std::move(msgs));
    return appended;
}

} // namespace plugin
} // namespace agentxx
