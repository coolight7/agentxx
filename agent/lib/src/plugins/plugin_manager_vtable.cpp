#include "agentxx/plugin/plugin_manager.h"
#include "pluginxx/runtime/op_driver.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/agent/resource_applier.h"
#include "agentxx/plugin/plugin_framework.h"
#include "agentxx/plugin/plugin_interfaces.h"
#include "fmt/format.h"
#include "utilxx_base/log.h"

#include <cstring>

namespace agentxx {
namespace plugin {

// =====================================================================
// vtable 入口公共前置
// =====================================================================

using HostCall = PluginHostCall<PluginInstance, PluginManager>;

/// 解析宿主控制块，并持有实例/管理器强引用与 admission lease。
///
/// - 实例已卸载/已关闭，或传入的 host 指针不是本宿主发放的视图 -> 返回空上下文
///   （实例与管理器均为 nullptr），入口按失败返回，不访问已释放对象，也不会把
///   调用转交给后来加载的同名实例；
/// - `allowClosing=true` 用于只读查询：关闭过程中仍允许执行（lease 保证卸载会
///   等它返回）。注册、投递新工作等入口必须用默认值，Closing/Disabled 后拒绝；
/// - 返回的上下文按值捕获进投递闭包后，卸载的 idle 等待会覆盖"已排队但尚未在
///   IO 线程执行"的阶段，见 [ioCallSyncKeep]。
static HostCall enterHost(const AgentxxPluginHost* host, bool allowClosing = false) {
    return enterPluginHost<PluginInstance, PluginManager>(host, allowClosing);
}

/// 注册/写入类入口的公共骨架: 解析 host 上下文 → 把业务逻辑投递到 IO 线程执行。
///
/// - 参数视图只在本次调用内有效, 因此闭包必须按值捕获自己需要的拷贝;
/// - `fn` 的入参是实例与管理器 (投递期间由 `keep` 保活, 含 admission lease);
///   业务参数校验由 `fn` 自己完成, 失败返回非 0 状态码。
template<typename Fn>
static int32_t onInstanceIo(const AgentxxPluginHost* host, Fn&& fn) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
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
template<typename Fn>
static int32_t queryStringIo(const AgentxxPluginHost* host, AgentxxPluginString* out, Fn&& fn) {
    if (!out) {
        return -1;
    }
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host, /*allowClosing=*/true);
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

// C ABI 内存操作

static void* AGENTXX_PLUGIN_CALL xx_alloc(uint64_t size) {
    return agentxx::plugin::hostMemoryAlloc(size);
}

static void AGENTXX_PLUGIN_CALL xx_free(void* ptr) {
    agentxx::plugin::hostMemoryFree(ptr);
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_register_tool(const AgentxxPluginHost* host, const AgentxxPluginToolSpec* spec) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !spec || agentxx::plugin::PluginStringView::empty(&spec->name)) {
            return -1;
        }
        auto                  mgrPtr   = mgr;
        auto                  instPtr  = inst;
        AgentxxPluginToolSpec specCopy = *spec;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, specCopy]() {
            return mgrPtr->registerTool(instPtr, &specCopy);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_unregister_tool(const AgentxxPluginHost* host, const AgentxxPluginStringView* name) {
    if (agentxx::plugin::PluginStringView::empty(name)) {
        return -1;
    }
    auto nameValCopy = *name;
    return onInstanceIo(host, [nameValCopy](PluginInstance* inst, PluginManager* mgr) {
        return mgr->unregisterTool(inst, nameValCopy);
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_register_tool_permission(
    const AgentxxPluginHost*               host,
    const AgentxxPluginToolPermissionSpec* spec
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !spec || agentxx::plugin::PluginStringView::empty(&spec->tool_name)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        // 声明内容按值复制: 跨边界视图只在本次调用期间有效, 复制为自有字符串后
        // 再交给 IO 线程执行 (视图指向闭包持有的字符串)
        AgentxxPluginToolPermissionSpec specCopy = *spec;
        auto                            toolName = std::make_shared<std::string>(
            spec->tool_name.data ? spec->tool_name.data : "",
            static_cast<size_t>(spec->tool_name.size)
        );
        auto targetArg = std::make_shared<std::string>(
            spec->target_arg.data ? spec->target_arg.data : "",
            static_cast<size_t>(spec->target_arg.size)
        );
        auto category = std::make_shared<std::string>(
            spec->category.data ? spec->category.data : "",
            static_cast<size_t>(spec->category.size)
        );
        specCopy.tool_name  = agentxx::plugin::PluginStringView::from(*toolName);
        specCopy.target_arg = agentxx::plugin::PluginStringView::from(*targetArg);
        specCopy.category   = agentxx::plugin::PluginStringView::from(*category);
        return ioCallSyncKeep<int32_t>(
            call,
            mgrPtr,
            [mgrPtr, instPtr, specCopy, toolName, targetArg, category]() {
                return mgrPtr->registerToolPermission(instPtr, &specCopy);
            }
        );
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_unregister_tool_permission(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* tool_name
) {
    if (agentxx::plugin::PluginStringView::empty(tool_name)) {
        return -1;
    }
    auto nameValCopy = *tool_name;
    return onInstanceIo(host, [nameValCopy](PluginInstance* inst, PluginManager* mgr) {
        return mgr->unregisterToolPermission(inst, nameValCopy);
    });
}

/// check_paths 单次批量上限: 判定在宿主 io 线程执行, 限制单次规模避免长时间占用
/// (插件侧建议按 512 项分批; 上限仅作为异常调用的保护)
static constexpr int32_t kPermissionCheckPathsMax = 16384;

static int32_t AGENTXX_PLUGIN_CALL xx_check_paths(
    const AgentxxPluginHost*                host,
    const AgentxxPluginPermissionPathQuery* query,
    int32_t*                                out_decisions
) {
    if (!query || !out_decisions || !query->paths || query->path_count <= 0) {
        return -1;
    }
    if (query->path_count > kPermissionCheckPathsMax) {
        return -1;
    }
    // 旧布局保护: struct_size 非 0 时必须覆盖当前结构体
    if (query->struct_size != 0 && query->struct_size < sizeof(AgentxxPluginPermissionPathQuery)) {
        return -1;
    }
    // 入参按值复制后交给 io 线程执行 (跨边界视图只在本次调用期间有效)
    auto paths = std::make_shared<std::vector<std::string>>();
    paths->reserve(static_cast<size_t>(query->path_count));
    for (int32_t i = 0; i < query->path_count; ++i) {
        const auto& item = query->paths[i];
        paths->emplace_back(item.data ? item.data : "", static_cast<size_t>(item.size));
    }
    auto sessionId = std::make_shared<std::string>(
        query->session_id.data ? query->session_id.data : "",
        static_cast<size_t>(query->session_id.size)
    );
    const int32_t scope = query->scope;
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        // 只读查询: 允许关闭中调用 (实例收尾时其工具仍可能在执行)
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst) {
            return -1;
        }
        auto                 mgrPtr  = mgr;
        auto                 instPtr = inst;
        std::vector<int32_t> decisions;
        auto                 rc = ioCallSyncKeep<int32_t>(
            call,
            mgrPtr,
            [mgrPtr, instPtr, scope, sessionId, paths, &decisions]() {
                return mgrPtr->checkPermissionPaths(instPtr, scope, *sessionId, *paths, decisions);
            }
        );
        if (rc != 0 || decisions.size() != paths->size()) {
            return -1;
        }
        std::copy(decisions.begin(), decisions.end(), out_decisions);
        return 0;
    });
}

static void AGENTXX_PLUGIN_CALL xx_op_cancel(::AgentxxPluginOperatorHandle* op) {
    cancelPluginOperation(op);
}

static ::AgentxxPluginOperatorHandle* AGENTXX_PLUGIN_CALL xx_call_tool_async(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* name,
    const AgentxxPluginStringView* args_json,
    const AgentxxPluginStringView* session_id,
    AgentxxPluginOperatorCallback  cb,
    void*                          ud,
    AgentxxPluginString*           error_out
) {
    return guardVtableCall<::AgentxxPluginOperatorHandle*>(nullptr, [&]() {
        auto  call = enterHost(host);
        auto* inst = call.instance();
        auto* mgr  = call.manager();
        if (!mgr || !inst || !name) {
            hostMemorySetString(error_out, "call_tool_async: plugin runtime unavailable");
            return static_cast<::AgentxxPluginOperatorHandle*>(nullptr);
        }
        return mgr->callToolAsync(
            inst,
            *name,
            args_json ? *args_json : AgentxxPluginStringView{},
            session_id ? *session_id : AgentxxPluginStringView{},
            cb,
            ud,
            error_out
        );
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_register_hook(const AgentxxPluginHost* host, const AgentxxPluginHookSpec* spec) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !spec || spec->point < 0 || spec->point >= AGENTXX_PLUGIN_HOOK_COUNT
            || !spec->hook_start) {
            return -1;
        }
        auto                  mgrPtr   = mgr;
        auto                  instPtr  = inst;
        AgentxxPluginHookSpec specCopy = *spec;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, specCopy]() {
            return mgrPtr->registerHook(instPtr, &specCopy);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_unregister_hook(const AgentxxPluginHost* host, int32_t point) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || point < 0 || point >= AGENTXX_PLUGIN_HOOK_COUNT) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto pt      = static_cast<AgentxxPluginHookPoint>(point);
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, pt]() {
            return mgrPtr->unregisterHook(instPtr, pt);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_get_share_store(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* session_id,
    int64_t                        id,
    AgentxxPluginString*           out
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(session_id)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto sid     = *session_id;
        *out         = ioCallSyncKeep<AgentxxPluginString>(
            call,
            mgrPtr,
            [mgrPtr, instPtr, sid, id]() -> AgentxxPluginString {
                return mgrPtr->getShareStore(instPtr, sid, id);
            }
        );
        return (out->data != nullptr) ? 0 : -1;
    });
}

static int64_t AGENTXX_PLUGIN_CALL xx_add_share_store(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* session_id,
    const AgentxxPluginStringView* content
) {
    return agentxx::plugin::guardVtableCall<int64_t>(-1, [&]() -> int64_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !session_id || !content) {
            return -1;
        }
        auto mgrPtr     = mgr;
        auto instPtr    = inst;
        auto sid        = *session_id;
        auto contentVal = *content;
        return ioCallSyncKeep<int64_t>(
            call,
            mgrPtr,
            [mgrPtr, instPtr, sid, contentVal]() -> int64_t {
                return mgrPtr->addShareStore(instPtr, sid, contentVal);
            }
        );
    });
}

static void AGENTXX_PLUGIN_CALL xx_emit_message_tip(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* session_id,
    const AgentxxPluginStringView* text,
    int32_t                        level
) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !session_id || !text) {
            return;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto sid     = *session_id;
        auto txt     = *text;
        ioCallSyncVoidKeep(call, mgrPtr, [mgrPtr, instPtr, sid, txt, level]() {
            mgrPtr->emitMessageTip(instPtr, sid, txt, level);
        });
    });
}

// =====================================================================
// graph 接口表 (agentxx.agent.graph)
// =====================================================================

static int32_t AGENTXX_PLUGIN_CALL xx_register_node_type(
    const AgentxxPluginHost*              host,
    const AgentxxPluginGraphNodeTypeSpec* spec
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !spec || agentxx::plugin::PluginStringView::empty(&spec->type)
            || !spec->run_start) {
            return -1;
        }
        auto                           mgrPtr   = mgr;
        auto                           instPtr  = inst;
        AgentxxPluginGraphNodeTypeSpec specCopy = *spec;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, specCopy]() {
            return mgrPtr->registerGraphNodeType(instPtr, &specCopy);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_unregister_node_type(const AgentxxPluginHost* host, const AgentxxPluginStringView* type) {
    if (agentxx::plugin::PluginStringView::empty(type)) {
        return -1;
    }
    auto typeValCopy = *type;
    return onInstanceIo(host, [typeValCopy](PluginInstance* inst, PluginManager* mgr) {
        return mgr->unregisterGraphNodeType(inst, typeValCopy);
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_get_graph_json(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr]() -> std::string {
            return mgrPtr->getGraphJson();
        });
        if (json.empty()) {
            return -1;
        }
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_get_graph_name(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto name   = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr]() -> std::string {
            auto        json   = mgrPtr->getGraphJson();
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
        });
        hostMemorySetString(out, name);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_set_graph_json(const AgentxxPluginHost* host, const AgentxxPluginStringView* graph_json) {
    if (agentxx::plugin::PluginStringView::empty(graph_json)) {
        return -1;
    }
    auto jsonValCopy = *graph_json;
    return onInstanceIo(host, [jsonValCopy](PluginInstance* inst, PluginManager* mgr) {
        return mgr->setGraphJson(inst, jsonValCopy);
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_get_prompt(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return queryStringIo(host, out, [](PluginInstance* inst, PluginManager* mgr) {
        return mgr->getPromptJson();
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_set_prompt(const AgentxxPluginHost* host, const AgentxxPluginStringView* prompt_json) {
    if (agentxx::plugin::PluginStringView::empty(prompt_json)) {
        return -1;
    }
    auto pJsonCopy = *prompt_json;
    return onInstanceIo(host, [pJsonCopy](PluginInstance* inst, PluginManager* mgr) {
        return mgr->setPromptJson(inst, pJsonCopy);
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_model_get_config(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return queryStringIo(host, out, [](PluginInstance* inst, PluginManager* mgr) {
        return mgr->getModelConfigJson();
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_register_skill_dir(const AgentxxPluginHost* host, const AgentxxPluginStringView* path) {
    if (agentxx::plugin::PluginStringView::empty(path)) {
        return -1;
    }
    auto pathValCopy = *path;
    return onInstanceIo(host, [pathValCopy](PluginInstance* inst, PluginManager* mgr) {
        return mgr->registerSkillDir(inst, pathValCopy);
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_unregister_skill_dir(const AgentxxPluginHost* host, const AgentxxPluginStringView* path) {
    if (agentxx::plugin::PluginStringView::empty(path)) {
        return -1;
    }
    auto pathValCopy = *path;
    return onInstanceIo(host, [pathValCopy](PluginInstance* inst, PluginManager* mgr) {
        return mgr->unregisterSkillDir(inst, pathValCopy);
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_register_memory_file(const AgentxxPluginHost* host, const AgentxxPluginStringView* path) {
    if (agentxx::plugin::PluginStringView::empty(path)) {
        return -1;
    }
    auto pathValCopy = *path;
    return onInstanceIo(host, [pathValCopy](PluginInstance* inst, PluginManager* mgr) {
        return mgr->registerMemoryFile(inst, pathValCopy);
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_unregister_memory_file(const AgentxxPluginHost* host, const AgentxxPluginStringView* path) {
    if (agentxx::plugin::PluginStringView::empty(path)) {
        return -1;
    }
    auto pathValCopy = *path;
    return onInstanceIo(host, [pathValCopy](PluginInstance* inst, PluginManager* mgr) {
        return mgr->unregisterMemoryFile(inst, pathValCopy);
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_register_mcp_server(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* spec_json
) {
    if (agentxx::plugin::PluginStringView::empty(spec_json)) {
        return -1;
    }
    auto specValCopy = *spec_json;
    return onInstanceIo(host, [specValCopy](PluginInstance* inst, PluginManager* mgr) {
        return mgr->registerMcpServer(inst, specValCopy);
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_unregister_mcp_server(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* name_space
) {
    if (agentxx::plugin::PluginStringView::empty(name_space)) {
        return -1;
    }
    auto nsValCopy = *name_space;
    return onInstanceIo(host, [nsValCopy](PluginInstance* inst, PluginManager* mgr) {
        return mgr->unregisterMcpServer(inst, nsValCopy);
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_get_own_resources(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return queryStringIo(host, out, [](PluginInstance* inst, PluginManager* mgr) {
        return mgr->ownResourcesJson(inst);
    });
}

static const AgentxxPluginToolsIface g_ifaceTools = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION,
    /* struct_size */ sizeof(AgentxxPluginToolsIface),
    /* register_tool */ xx_register_tool,
    /* unregister_tool */ xx_unregister_tool,
    /* call_tool_async */ xx_call_tool_async,
    /* op_cancel */ xx_op_cancel,
};

static const AgentxxPluginPermissionIface g_ifacePermission = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_VERSION,
    /* struct_size */ sizeof(AgentxxPluginPermissionIface),
    /* register_tool_permission */ xx_register_tool_permission,
    /* unregister_tool_permission */ xx_unregister_tool_permission,
    /* check_paths */ xx_check_paths,
};

static const AgentxxPluginHooksIface g_ifaceHooks = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION,
    /* struct_size */ sizeof(AgentxxPluginHooksIface),
    /* register_hook */ xx_register_hook,
    /* unregister_hook */ xx_unregister_hook,
};

static const AgentxxPluginSessionIface g_ifaceSession = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION,
    /* struct_size */ sizeof(AgentxxPluginSessionIface),
    /* get_share_store */ xx_get_share_store,
    /* emit_message_tip */ xx_emit_message_tip,
    /* add_share_store */ xx_add_share_store,
};

static const AgentxxPluginPromptIface g_ifacePrompt = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION,
    /* struct_size */ sizeof(AgentxxPluginPromptIface),
    /* get_prompt */ xx_get_prompt,
    /* set_prompt */ xx_set_prompt,
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

const void* AGENTXX_PLUGIN_CALL
    xx_query_interface(const AgentxxPluginHost*, const AgentxxPluginStringView* iid);

static const AgentxxHostVtable g_hostVtable = {
    /* alloc */ xx_alloc,
    /* free */ xx_free,
    /* query_interface */ xx_query_interface,
};

const AgentxxHostVtable* PluginManager::hostVtable() {
    return &g_hostVtable;
}

const void* AGENTXX_PLUGIN_CALL
    xx_query_interface(const AgentxxPluginHost*, const AgentxxPluginStringView* iid) {
    if (!iid || !iid->data) {
        return nullptr;
    }
    const std::string_view n{iid->data, static_cast<size_t>(iid->size)};
    if (n == "__vtable") {
        return &g_hostVtable;
    }
    // 通用表 (log/json/config/plugins/events/scheduler/coroutine_runtime/tasks/
    // cancel/capabilities) 由 cxx_pluginxx 提供 (见 pluginxx/host/tables_impl.h),
    // 宿主侧的领域表在下面逐个分发。
    if (const void* generic = pluginxx::queryGenericPluginIface<PluginInstance, PluginManager>(n)) {
        return generic;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_TOOLS) {
        return &g_ifaceTools;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION) {
        return &g_ifacePermission;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_HOOKS) {
        return &g_ifaceHooks;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_SESSION) {
        return &g_ifaceSession;
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
    return nullptr;
}

// ==================== 宿主状态访问辅助实现 ====================

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

int PluginManager::registerSkillDir(PluginInstance* inst, AgentxxPluginStringView path) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&path)) {
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "Plugin `{}` registerSkillDir rejected: instance is closing or disabled",
            inst->name
        );
        return -1;
    }
    if (inst->resourcesFrozen) {
        XX_LOGW(
            "Plugin `{}` registerSkillDir rejected: resources frozen (immutable after init)",
            inst->name
        );
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "register_skill_dir");
    if (!ap) {
        return -1;
    }
    std::string pathStr = svToStr(path);
    std::string err;
    if (!ap->addSkillDir(inst->name, pathStr, err)) {
        XX_LOGW("Plugin `{}` register skill dir failed: {}", inst->name, err);
        return -1;
    }
    XX_LOGI("Plugin `{}` registered skill dir `{}`", inst->name, pathStr);
    return 0;
}

int PluginManager::unregisterSkillDir(PluginInstance* inst, AgentxxPluginStringView path) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&path)) {
        return -1;
    }
    if (inst->resourcesFrozen) {
        XX_LOGW(
            "Plugin `{}` unregisterSkillDir rejected: resources frozen (immutable after init)",
            inst->name
        );
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "unregister_skill_dir");
    if (!ap) {
        return -1;
    }
    std::string pathStr = svToStr(path);
    if (!ap->removeSkillDir(inst->name, pathStr)) {
        return -1;
    }
    XX_LOGI("Plugin `{}` unregistered skill dir `{}`", inst->name, pathStr);
    return 0;
}

int PluginManager::registerMemoryFile(PluginInstance* inst, AgentxxPluginStringView path) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&path)) {
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "Plugin `{}` registerMemoryFile rejected: instance is closing or disabled",
            inst->name
        );
        return -1;
    }
    if (inst->resourcesFrozen) {
        XX_LOGW(
            "Plugin `{}` registerMemoryFile rejected: resources frozen (immutable after init)",
            inst->name
        );
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "register_memory_file");
    if (!ap) {
        return -1;
    }
    std::string pathStr = svToStr(path);
    std::string err;
    if (!ap->addMemoryFile(inst->name, pathStr, err)) {
        XX_LOGW("Plugin `{}` register memory file failed: {}", inst->name, err);
        return -1;
    }
    XX_LOGI("Plugin `{}` registered memory file `{}`", inst->name, pathStr);
    return 0;
}

int PluginManager::unregisterMemoryFile(PluginInstance* inst, AgentxxPluginStringView path) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&path)) {
        return -1;
    }
    if (inst->resourcesFrozen) {
        XX_LOGW(
            "Plugin `{}` unregisterMemoryFile rejected: resources frozen (immutable after init)",
            inst->name
        );
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "unregister_memory_file");
    if (!ap) {
        return -1;
    }
    std::string pathStr = svToStr(path);
    if (!ap->removeMemoryFile(inst->name, pathStr)) {
        return -1;
    }
    XX_LOGI("Plugin `{}` unregistered memory file `{}`", inst->name, pathStr);
    return 0;
}

int PluginManager::registerMcpServer(PluginInstance* inst, AgentxxPluginStringView specJson) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&specJson)) {
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "Plugin `{}` registerMcpServer rejected: instance is closing or disabled",
            inst->name
        );
        return -1;
    }
    if (inst->resourcesFrozen) {
        XX_LOGW(
            "Plugin `{}` registerMcpServer rejected: resources frozen (immutable after init)",
            inst->name
        );
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "register_mcp_server");
    if (!ap) {
        return -1;
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
            return -1;
        }
        agentxx::agent::McpServerConfig cfg;
        cfg.url         = url;
        cfg.toolTimeout = std::chrono::seconds{std::max(timeoutSec, 0)};
        std::string err;
        if (!ap->addMcpServer(inst->name, ns, cfg, err)) {
            XX_LOGW("Plugin `{}` register mcp server failed: {}", inst->name, err);
            return -1;
        }
        XX_LOGI("Plugin `{}` registered mcp server `{}` ({})", inst->name, ns, url);
        return 0;
    } catch (const std::exception& e) {
        XX_LOGE("Plugin `{}` register_mcp_server invalid json: {}", inst->name, e.what());
        return -1;
    }
}

int PluginManager::unregisterMcpServer(PluginInstance* inst, AgentxxPluginStringView nameSpace) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&nameSpace)) {
        return -1;
    }
    if (inst->resourcesFrozen) {
        XX_LOGW(
            "Plugin `{}` unregisterMcpServer rejected: resources frozen (immutable after init)",
            inst->name
        );
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "unregister_mcp_server");
    if (!ap) {
        return -1;
    }
    std::string nsStr = svToStr(nameSpace);
    if (!ap->removeMcpServer(inst->name, nsStr)) {
        return -1;
    }
    XX_LOGI("Plugin `{}` unregistered mcp server `{}`", inst->name, nsStr);
    return 0;
}

std::string PluginManager::ownResourcesJson(const PluginInstance* inst) {
    if (!inst) {
        return {};
    }
    auto c = agentContext_.lock();
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
    // 初始化收尾即冻结资源声明: 之后固定不可变以防上下文变化
    // (仅 yaml 声明与初始化阶段追加生效; 见 PluginInstance::resourcesFrozen)
    inst.resourcesFrozen = true;
    if (resources.skillDirs.empty() && resources.memoryFiles.empty()
        && resources.mcpServers.empty()) {
        return;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->resourceApplier) {
        XX_LOGW("Plugin `{}` declared resources skipped (no resource applier)", inst.name);
        return;
    }
    agentxx::agent::PluginResourceDecls decls;
    decls.skillDirs   = resources.skillDirs;
    decls.memoryFiles = resources.memoryFiles;
    for (const auto& [ns, d] : resources.mcpServers) {
        agentxx::agent::McpServerConfig cfg;
        cfg.url              = d.url;
        cfg.toolTimeout      = std::chrono::milliseconds{d.timeoutMs};
        decls.mcpServers[ns] = cfg;
    }
    ctx->resourceApplier->applyDecls(inst.name, decls);
}

std::string PluginManager::getPromptJson() {
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return {};
    }
    return c->agentConfig->prompt.toJson().dump();
}

namespace {

/// prompt 贡献模型内部工具（见 [PluginManager::PromptKeyState]）。
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
std::optional<PluginManager::PromptValue>
    readPromptKey(const agentxx::agent::AgentPrompt& prompt, const std::string& key) {
    PluginManager::PromptValue value;
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
    const std::optional<PluginManager::PromptValue>& value
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

/// 两次取值是否相同（用于发现"宿主之外"的修改并 rebase 基础值）。
bool samePromptValue(
    const std::optional<PluginManager::PromptValue>& a,
    const std::optional<PluginManager::PromptValue>& b
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
PluginManager::PromptValue mergeToolPromptValue(
    const std::optional<PluginManager::PromptValue>& current,
    const utilxx_base::Json&                         spec
) {
    PluginManager::PromptValue value;
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
void PluginManager::recomposePromptKey(const std::string& key) {
    auto c = agentContext_.lock();
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
        // 宿主之外（用户或其他代码）改过：以当前值为新基础，贡献在其上重新应用
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

void PluginManager::removePromptContributions(std::string_view owner) {
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

int PluginManager::setPromptJson(PluginInstance* inst, AgentxxPluginStringView prompt_json) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&prompt_json)) {
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("Plugin `{}` setPromptJson rejected: instance is closing or disabled", inst->name);
        return -1;
    }
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return -1;
    }
    try {
        auto j = utilxx_base::Json::parse(
            std::string_view{prompt_json.data, static_cast<size_t>(prompt_json.size)}
        );
        if (!j.is_object()) {
            return -1;
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
        return 0;
    } catch (...) {
        return -1;
    }
}

/// 兼容入口：卸载/禁用时删除该 owner 的 prompt 贡献并重新合成。
void PluginManager::restorePromptBackup(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    removePromptContributions(inst->name);
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

AgentxxPluginString PluginManager::getShareStore(
    PluginInstance*         inst,
    AgentxxPluginStringView session_id,
    int64_t                 id
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&session_id) || id < 0) {
        return AgentxxPluginString{nullptr, 0};
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return AgentxxPluginString{nullptr, 0};
    }
    std::string sid = svToStr(session_id);
    auto val = ctx->middlewareHandleContext->getShareStoreItemValue(sid, static_cast<size_t>(id));
    if (!val.has_value()) {
        return AgentxxPluginString{nullptr, 0};
    }
    return agentxx::plugin::hostMemoryCreateString(*val);
}

int64_t PluginManager::addShareStore(
    PluginInstance*         inst,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView content
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&session_id)) {
        return -1;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return -1;
    }
    std::string sid  = svToStr(session_id);
    std::string text = svToStr(content);
    return static_cast<int64_t>(ctx->middlewareHandleContext->addShareStoreItemValue(sid, text));
}

void PluginManager::emitMessageTip(
    PluginInstance*         inst,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView text,
    int32_t                 level
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&session_id)
        || agentxx::plugin::PluginStringView::empty(&text)) {
        return;
    }
    auto ctx = agentContext_.lock();
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

} // namespace plugin
} // namespace agentxx
