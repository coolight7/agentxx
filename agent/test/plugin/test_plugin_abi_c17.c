/// agentxx 插件 ABI 的 C17 编译期检查（配合 test_plugin_runtime 的运行期对照）。
///
/// 目的（对应 plugin.md 第 11.1 节）：
/// - 用 **C17 编译器**（`-std=c17 -pedantic-errors`）包含两个 ABI 头，证明它们
///   不依赖任何 C++ 语法（`extern "C"` 之外）；
/// - 用 `_Static_assert` 固定跨边界结构体的对齐、首字段布局与关键 `offsetof`；
/// - 通过 `agentxx_test_abi_value()` 把 C 侧看到的 `sizeof`/`offsetof` 暴露给
///   C++ 测试，两边逐项比对，任何"只有 C++ 侧成立"的布局都会被检出。
///
/// 注意：本文件必须保持纯 C（不要引入 C++ 头或 STL）。新增 ABI 字段时同步更新
/// 这里的 `_Static_assert` 与 `agentxx_test_abi_value` 的编号表。

#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_api.h"

#include <stddef.h>
#include <stdint.h>

/* ==================== 全局版本 ==================== */

_Static_assert(AGENTXX_PLUGIN_API_VERSION == 1, "agent 侧 ABI 版本必须为 1");
_Static_assert(AGENTXX_CLIENT_PLUGIN_API_VERSION == 1, "client 侧 ABI 版本必须为 1");

/* ==================== 8 字节对齐（全部跨边界结构体） ==================== */

_Static_assert(sizeof(AgentxxPluginStringView) % 8 == 0, "StringView 必须 8 字节对齐");
_Static_assert(sizeof(AgentxxPluginString) % 8 == 0, "String 必须 8 字节对齐");
_Static_assert(sizeof(AgentxxPluginHost) % 8 == 0, "PluginHost 必须 8 字节对齐");
_Static_assert(sizeof(AgentxxHostVtable) % 8 == 0, "HostVtable 必须 8 字节对齐");
_Static_assert(sizeof(AgentxxPluginOperatorNotify) % 8 == 0, "Notify 必须 8 字节对齐");
_Static_assert(sizeof(AgentxxPluginToolSpec) % 8 == 0, "ToolSpec 必须 8 字节对齐");
_Static_assert(sizeof(AgentxxPluginHookSpec) % 8 == 0, "HookSpec 必须 8 字节对齐");
_Static_assert(sizeof(AgentxxPluginToolsIface) % 8 == 0, "ToolsIface 必须 8 字节对齐");
_Static_assert(sizeof(AgentxxPluginHooksIface) % 8 == 0, "HooksIface 必须 8 字节对齐");
_Static_assert(sizeof(AgentxxPluginEventsIface) % 8 == 0, "EventsIface 必须 8 字节对齐");
_Static_assert(
    sizeof(AgentxxPluginCapabilitiesIface) % 8 == 0,
    "CapabilitiesIface 必须 8 字节对齐"
);
_Static_assert(sizeof(AgentxxPluginSchedulerIface) % 8 == 0, "SchedulerIface 必须 8 字节对齐");
_Static_assert(sizeof(AgentxxPluginTasksIface) % 8 == 0, "TasksIface 必须 8 字节对齐");
_Static_assert(
    sizeof(AgentxxPluginCoroutineRuntimeIface) % 8 == 0,
    "CoroutineRuntimeIface 必须 8 字节对齐"
);
_Static_assert(sizeof(AgentxxPluginGraphIface) % 8 == 0, "GraphIface 必须 8 字节对齐");
_Static_assert(sizeof(AgentxxClientUiIface) % 8 == 0, "ClientUiIface 必须 8 字节对齐");
_Static_assert(sizeof(AgentxxClientEventsIface) % 8 == 0, "ClientEventsIface 必须 8 字节对齐");

/* ==================== 字符串视图 / 字符串 ==================== */

_Static_assert(sizeof(AgentxxPluginStringView) == 16, "StringView = data(8) + size(8)");
_Static_assert(offsetof(AgentxxPluginStringView, data) == 0, "StringView.data 必须是首字段");
_Static_assert(offsetof(AgentxxPluginStringView, size) == 8, "StringView.size 必须紧随 data");
_Static_assert(offsetof(AgentxxPluginString, data) == 0, "String.data 必须是首字段");
_Static_assert(offsetof(AgentxxPluginString, size) == 8, "String.size 必须紧随 data");

/* ==================== 宿主 vtable / 视图 ==================== */

_Static_assert(offsetof(AgentxxPluginHost, vtable) == 0, "PluginHost.vtable 必须是首字段");
_Static_assert(offsetof(AgentxxHostVtable, alloc) == 0, "HostVtable.alloc 必须是首字段");
_Static_assert(offsetof(AgentxxHostVtable, free) == 8, "HostVtable.free 紧随 alloc");
_Static_assert(
    offsetof(AgentxxHostVtable, query_interface) == 16,
    "HostVtable.query_interface 是第三项"
);

/* ==================== 接口表首两字段固定 ==================== */

_Static_assert(offsetof(AgentxxPluginToolsIface, version) == 0, "ToolsIface.version 首字段");
_Static_assert(
    offsetof(AgentxxPluginToolsIface, struct_size) == sizeof(int32_t),
    "ToolsIface.struct_size 紧随 version"
);
_Static_assert(
    offsetof(AgentxxPluginSchedulerIface, version) == 0,
    "SchedulerIface.version 首字段"
);
_Static_assert(
    offsetof(AgentxxPluginSchedulerIface, struct_size) == sizeof(int32_t),
    "SchedulerIface.struct_size 紧随 version"
);
_Static_assert(offsetof(AgentxxPluginTasksIface, version) == 0, "TasksIface.version 首字段");
_Static_assert(
    offsetof(AgentxxPluginCoroutineRuntimeIface, version) == 0,
    "CoroutineRuntimeIface.version 首字段"
);
_Static_assert(
    offsetof(AgentxxPluginCoroutineRuntimeIface, struct_size) == sizeof(int32_t),
    "CoroutineRuntimeIface.struct_size 紧随 version"
);
_Static_assert(
    offsetof(AgentxxPluginCoroutineRuntimeIface, request_driver)
        == sizeof(int32_t) + sizeof(uint32_t),
    "CoroutineRuntimeIface.request_driver 是第三项"
);
_Static_assert(offsetof(AgentxxClientUiIface, version) == 0, "ClientUiIface.version 首字段");
_Static_assert(
    offsetof(AgentxxClientUiIface, struct_size) == sizeof(int32_t),
    "ClientUiIface.struct_size 紧随 version"
);

/* ==================== 调用约定与入口函数类型可用 ==================== */

/* 这些声明只在 C 视角下检查"函数指针类型可声明、调用约定宏可用"，
   不引入任何实现；GCC/Clang 的 -Wunused 对文件级 static 变量不告警。 */
static AgentxxPluginCreateFn abi_probe_create = NULL;
/* 协程驱动: 不透明请求句柄与驱动回调类型必须在 C 视角下可声明 */
static AgentxxPluginDriver*     abi_probe_driver  = NULL;
static AgentxxPluginDriveOnceFn abi_probe_drive   = NULL;
static AgentxxPluginStartFn     abi_probe_start   = NULL;
static AgentxxPluginStopFn      abi_probe_stop    = NULL;
static AgentxxPluginDestroyFn   abi_probe_destroy = NULL;

/* ==================== 运行期对照（供 C++ 测试调用） ==================== */

/// 编号表（与 C++ 侧 `abiValueExpected` 一一对应；不得重排已发布编号）。
enum {
    AGENTXX_ABI_VALUE_STRING_VIEW_SIZE                    = 1,
    AGENTXX_ABI_VALUE_STRING_VIEW_SIZE_OFFSET             = 2,
    AGENTXX_ABI_VALUE_PLUGIN_HOST_SIZE                    = 3,
    AGENTXX_ABI_VALUE_PLUGIN_HOST_OPAQUE_OFFSET           = 4,
    AGENTXX_ABI_VALUE_HOST_VTABLE_SIZE                    = 5,
    AGENTXX_ABI_VALUE_TOOL_SPEC_SIZE                      = 6,
    AGENTXX_ABI_VALUE_TOOL_SPEC_START_OFFSET              = 7,
    AGENTXX_ABI_VALUE_HOOK_SPEC_SIZE                      = 8,
    AGENTXX_ABI_VALUE_NOTIFY_SIZE                         = 9,
    AGENTXX_ABI_VALUE_TOOLS_IFACE_SIZE                    = 10,
    AGENTXX_ABI_VALUE_TOOLS_IFACE_SIZE_OFFSET             = 11,
    AGENTXX_ABI_VALUE_SCHEDULER_IFACE_SIZE                = 12,
    AGENTXX_ABI_VALUE_TASKS_IFACE_SIZE                    = 13,
    AGENTXX_ABI_VALUE_CLIENT_UI_IFACE_SIZE                = 14,
    AGENTXX_ABI_VALUE_AGENT_API_VERSION                   = 15,
    AGENTXX_ABI_VALUE_CLIENT_API_VERSION                  = 16,
    AGENTXX_ABI_VALUE_TOOLS_IFACE_VERSION                 = 17,
    AGENTXX_ABI_VALUE_COROUTINE_RUNTIME_IFACE_SIZE        = 18,
    AGENTXX_ABI_VALUE_COROUTINE_RUNTIME_IFACE_SIZE_OFFSET = 19,
    AGENTXX_ABI_VALUE_COROUTINE_RUNTIME_IFACE_VERSION     = 20,
    AGENTXX_ABI_VALUE_VALUE_COUNT                         = 21
};

/// C 侧看到的 `sizeof`/`offsetof`/版本号取值。
/// - `id`: 见上方编号表；未知编号返回 `UINT64_MAX`
/// - 与 C++ 侧逐项比对，用于发现"C 与 C++ 对同一 ABI 布局理解不一致"
uint64_t agentxx_test_abi_value(int32_t id) {
    switch (id) {
        case AGENTXX_ABI_VALUE_STRING_VIEW_SIZE:
            return (uint64_t)sizeof(AgentxxPluginStringView);
        case AGENTXX_ABI_VALUE_STRING_VIEW_SIZE_OFFSET:
            return (uint64_t)offsetof(AgentxxPluginStringView, size);
        case AGENTXX_ABI_VALUE_PLUGIN_HOST_SIZE:
            return (uint64_t)sizeof(AgentxxPluginHost);
        case AGENTXX_ABI_VALUE_PLUGIN_HOST_OPAQUE_OFFSET:
            return (uint64_t)offsetof(AgentxxPluginHost, opaque);
        case AGENTXX_ABI_VALUE_HOST_VTABLE_SIZE:
            return (uint64_t)sizeof(AgentxxHostVtable);
        case AGENTXX_ABI_VALUE_TOOL_SPEC_SIZE:
            return (uint64_t)sizeof(AgentxxPluginToolSpec);
        case AGENTXX_ABI_VALUE_TOOL_SPEC_START_OFFSET:
            return (uint64_t)offsetof(AgentxxPluginToolSpec, execute_start);
        case AGENTXX_ABI_VALUE_HOOK_SPEC_SIZE:
            return (uint64_t)sizeof(AgentxxPluginHookSpec);
        case AGENTXX_ABI_VALUE_NOTIFY_SIZE:
            return (uint64_t)sizeof(AgentxxPluginOperatorNotify);
        case AGENTXX_ABI_VALUE_TOOLS_IFACE_SIZE:
            return (uint64_t)sizeof(AgentxxPluginToolsIface);
        case AGENTXX_ABI_VALUE_TOOLS_IFACE_SIZE_OFFSET:
            return (uint64_t)offsetof(AgentxxPluginToolsIface, struct_size);
        case AGENTXX_ABI_VALUE_SCHEDULER_IFACE_SIZE:
            return (uint64_t)sizeof(AgentxxPluginSchedulerIface);
        case AGENTXX_ABI_VALUE_TASKS_IFACE_SIZE:
            return (uint64_t)sizeof(AgentxxPluginTasksIface);
        case AGENTXX_ABI_VALUE_CLIENT_UI_IFACE_SIZE:
            return (uint64_t)sizeof(AgentxxClientUiIface);
        case AGENTXX_ABI_VALUE_AGENT_API_VERSION:
            return (uint64_t)AGENTXX_PLUGIN_API_VERSION;
        case AGENTXX_ABI_VALUE_CLIENT_API_VERSION:
            return (uint64_t)AGENTXX_CLIENT_PLUGIN_API_VERSION;
        case AGENTXX_ABI_VALUE_TOOLS_IFACE_VERSION:
            return (uint64_t)AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION;
        case AGENTXX_ABI_VALUE_COROUTINE_RUNTIME_IFACE_SIZE:
            return (uint64_t)sizeof(AgentxxPluginCoroutineRuntimeIface);
        case AGENTXX_ABI_VALUE_COROUTINE_RUNTIME_IFACE_SIZE_OFFSET:
            return (uint64_t)offsetof(AgentxxPluginCoroutineRuntimeIface, struct_size);
        case AGENTXX_ABI_VALUE_COROUTINE_RUNTIME_IFACE_VERSION:
            return (uint64_t)AGENTXX_PLUGIN_IFACE_COROUTINE_RUNTIME_VERSION;
        default:
            return UINT64_MAX;
    }
}

/// C 侧自检：ABI 头在 C17 下可编译、结构体对齐与首字段布局成立。
/// - `return` 0 表示全部通过，非 0 为失败编号（见上方断言）
int32_t agentxx_test_abi_c_probe(void) {
    if (abi_probe_create || abi_probe_start || abi_probe_stop || abi_probe_destroy
        || abi_probe_driver || abi_probe_drive) {
        return 1; /* 文件级函数指针必须保持未使用（仅类型检查） */
    }
    if (sizeof(AgentxxPluginStringView) != 16) {
        return 2;
    }
    if (sizeof(void*) == 8 && offsetof(AgentxxPluginString, size) != 8) {
        return 3;
    }
    if (agentxx_test_abi_value(AGENTXX_ABI_VALUE_VALUE_COUNT - 1) == UINT64_MAX) {
        return 4; /* 编号表最后一个有效项必须能取值 */
    }
    if (agentxx_test_abi_value(0) != UINT64_MAX || agentxx_test_abi_value(9999) != UINT64_MAX) {
        return 5; /* 未知编号必须返回哨兵值 */
    }
    /* 协程驱动表: 版本/大小/函数指针齐备 */
    if (AGENTXX_PLUGIN_IFACE_COROUTINE_RUNTIME_VERSION != 1) {
        return 90;
    }
    if (sizeof(AgentxxPluginCoroutineRuntimeIface) % 8 != 0) {
        return 91;
    }
    return 0;
}
