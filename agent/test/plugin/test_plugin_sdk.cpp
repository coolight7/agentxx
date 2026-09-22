/// 插件 C++ SDK 根操作适配测试（plugin.md 6.1/6.2/6.3）。
///
/// 覆盖：
/// - F13 输入所有权：tool 根操作的 args/session/tool_call_id 由 Request 拥有，
///   宿主借用缓冲区失效后协程继续读取仍然正确；
/// - 完成协议：同步钩子（正常/抛异常）与异步 Task 钩子都是 exactly-once 完成；
/// - F19 钩子返回类型分发：同步 void 立即完成，返回 Task<void> 时由协程真正结束
///   后才完成，并返回宿主可取消的 provider 句柄。
///
/// 用例不依赖真实管理器：用伪宿主接口表捕获 SDK 注册出来的 spec，再直接按 ABI
/// 调用 `execute_start`/`hook_start`，因此可以精确控制借用缓冲区的生命周期。
#include "agentxx-test/plugin/test_plugin_sdk.h"

#include "agentxx/plugin/api/plugin_kit.h"

#include <atomic>
#include <coroutine>
#include <cstdlib>
#include <string>
#include <string_view>

#define XX_TEST_PASSED result.passed
#define XX_TEST_FAILED result.failed

namespace agentxx::test {
namespace {
using namespace agentxx::plugin;

/// SDK helper 注册出来的 spec 捕获（替代真实宿主的注册表）。
struct CapturedRegistration {
    AgentxxPluginToolSpec           tool{};
    bool                            hasTool = false;
    AgentxxPluginHookSpec           hook{};
    bool                            hasHook       = false;
    PluginxxCapabilityStartFunction capStart      = nullptr;
    PluginxxOperatorCancelFunction  capCancel     = nullptr;
    void*                           capUd         = nullptr;
    bool                            hasCapability = false;
    AgentxxPluginGraphNodeTypeSpec  graphNode{};
    bool                            hasGraphNode = false;
};

CapturedRegistration g_captured;

int32_t PLUGINXX_CALL fakeRegisterTool(const PluginxxHost*, const AgentxxPluginToolSpec* spec) {
    if (!spec) {
        return -1;
    }
    g_captured.tool    = *spec;
    g_captured.hasTool = true;
    return 0;
}

int32_t PLUGINXX_CALL fakeRegisterHook(const PluginxxHost*, const AgentxxPluginHookSpec* spec) {
    if (!spec) {
        return -1;
    }
    g_captured.hook    = *spec;
    g_captured.hasHook = true;
    return 0;
}

int32_t PLUGINXX_CALL fakeRegisterCapabilityEx(
    const PluginxxHost*,
    const PluginxxStringView*,
    PluginxxCapabilityStartFunction start,
    PluginxxOperatorCancelFunction  cancel,
    void*                           ctx
) {
    g_captured.capStart      = start;
    g_captured.capCancel     = cancel;
    g_captured.capUd         = ctx;
    g_captured.hasCapability = true;
    return 0;
}

int32_t PLUGINXX_CALL
    fakeRegisterGraphNodeType(const PluginxxHost*, const AgentxxPluginGraphNodeTypeSpec* spec) {
    if (!spec) {
        return -1;
    }
    g_captured.graphNode    = *spec;
    g_captured.hasGraphNode = true;
    return 0;
}

const AgentxxPluginToolsIface g_fakeTools = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION,
    /* struct_size */ sizeof(AgentxxPluginToolsIface),
    /* register_tool */ fakeRegisterTool,
    /* unregister_tool */ nullptr,
    /* call_tool_async */ nullptr,
    /* op_cancel */ nullptr,
};

const AgentxxPluginHooksIface g_fakeHooks = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION,
    /* struct_size */ sizeof(AgentxxPluginHooksIface),
    /* register_hook */ fakeRegisterHook,
    /* unregister_hook */ nullptr,
};

const PluginxxCapabilitiesIface g_fakeCapabilities = {
    /* version */ PLUGINXX_IFACE_CAPABILITIES_VERSION,
    /* struct_size */ sizeof(PluginxxCapabilitiesIface),
    /* register_capability */ nullptr,
    /* register_capability_ex */ fakeRegisterCapabilityEx,
    /* unregister_capability */ nullptr,
    /* has_capability */ nullptr,
    /* invoke_capability_async */ nullptr,
    /* op_cancel */ nullptr,
};

const AgentxxPluginGraphIface g_fakeGraph = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_VERSION,
    /* struct_size */ sizeof(AgentxxPluginGraphIface),
    /* register_node_type */ fakeRegisterGraphNodeType,
    /* unregister_node_type */ nullptr,
    /* get_graph_json */ nullptr,
    /* get_graph_name */ nullptr,
    /* set_graph_json */ nullptr,
};

/// 伪宿主内存操作 (kit 会用 host->vtable->free 释放 request_driver 的 error_out)
void* PLUGINXX_CALL fakeAlloc(uint64_t size) {
    return std::malloc(static_cast<size_t>(size));
}

void PLUGINXX_CALL fakeFree(void* ptr) {
    std::free(ptr);
}

/// 伪宿主驱动票据: 记录待执行的驱动回调, 由用例经 [runDriver] 手动执行
/// (等价于宿主 IO 线程执行一次 `poll_one`)。
struct FakeDriverTicket {
    PluginxxDriveOnceFn drive = nullptr;
    void*               ud    = nullptr;
    bool                done  = false;
};

FakeDriverTicket* g_pendingDriver = nullptr;

PluginxxDriver* PLUGINXX_CALL
    fakeRequestDriver(const PluginxxHost*, PluginxxDriveOnceFn drive, void* ud, PluginxxString*) {
    auto* ticket    = new FakeDriverTicket{drive, ud, false};
    g_pendingDriver = ticket;
    return reinterpret_cast<PluginxxDriver*>(ticket);
}

void PLUGINXX_CALL fakeCancelDriver(PluginxxDriver* driver) {
    auto* ticket = reinterpret_cast<FakeDriverTicket*>(driver);
    if (ticket) {
        ticket->done = true;
    }
    if (g_pendingDriver == ticket) {
        g_pendingDriver = nullptr;
    }
}

int32_t PLUGINXX_CALL fakeIsIoThread(const PluginxxHost*) {
    return 1;
}

const PluginxxCoroutineRuntimeIface g_fakeRuntime = {
    /* version */ PLUGINXX_IFACE_COROUTINE_RUNTIME_VERSION,
    /* struct_size */ sizeof(PluginxxCoroutineRuntimeIface),
    /* request_driver */ fakeRequestDriver,
    /* cancel_driver */ fakeCancelDriver,
    /* is_io_thread */ fakeIsIoThread,
};

/// 伪 client UI 接口表: 捕获 SDK 便捷方法提交的 JSON (替代真实宿主注册表)
struct ClientUiCapture {
    int         panelUpdates = 0;
    std::string panelItems;
    int         statusUpdates = 0;
    std::string statusJson;
    int         decorUpdates = 0;
    std::string decorJson;
    std::string decorToolCallId;
    int         overlays = 0;
    std::string overlayPayload;
    std::string overlayExtra;
};

ClientUiCapture g_clientUiCapture;

int32_t PLUGINXX_CALL
    fakeUpdatePanel(const PluginxxHost*, AgentxxPanel*, const PluginxxStringView* items) {
    ++g_clientUiCapture.panelUpdates;
    g_clientUiCapture.panelItems
        = (items && items->data) ? std::string{items->data, static_cast<size_t>(items->size)} : "";
    return 0;
}

int32_t PLUGINXX_CALL fakeUpdateStatusItem(
    const PluginxxHost*,
    AgentxxStatusItem*,
    const PluginxxStringView* json
) {
    ++g_clientUiCapture.statusUpdates;
    g_clientUiCapture.statusJson
        = (json && json->data) ? std::string{json->data, static_cast<size_t>(json->size)} : "";
    return 0;
}

int32_t PLUGINXX_CALL fakeUpdateToolDecor(
    const PluginxxHost*,
    const PluginxxStringView* toolCallId,
    const PluginxxStringView* json
) {
    ++g_clientUiCapture.decorUpdates;
    g_clientUiCapture.decorToolCallId = (toolCallId && toolCallId->data)
                                            ? std::string{
                                                  toolCallId->data,
                                                  static_cast<size_t>(toolCallId->size)
    }
                                            : "";
    g_clientUiCapture.decorJson
        = (json && json->data) ? std::string{json->data, static_cast<size_t>(json->size)} : "";
    return 0;
}

int32_t PLUGINXX_CALL fakeOpenOverlay(
    const PluginxxHost*,
    const AgentxxOverlaySpec* spec
) {
    if (!spec) {
        return -1;
    }
    ++g_clientUiCapture.overlays;
    g_clientUiCapture.overlayPayload = (spec->payload.data)
                                           ? std::string{
                                                 spec->payload.data,
                                                 static_cast<size_t>(spec->payload.size)
    }
                                           : "";
    g_clientUiCapture.overlayExtra = (spec->extra_json.data)
                                         ? std::string{
                                               spec->extra_json.data,
                                               static_cast<size_t>(spec->extra_json.size)
    }
                                         : "";
    return 0;
}

/// 伪 client UI 表 (只填本模块用到的成员; 其余为 NULL = 该子能力不支持)
const AgentxxClientUiIface g_fakeClientUi = {
    /* version */ AGENTXX_IFACE_CLIENT_UI_VERSION,
    /* struct_size */ sizeof(AgentxxClientUiIface),
    /* register_status_item */ nullptr,
    /* update_status_item */ fakeUpdateStatusItem,
    /* unregister_status_item */ nullptr,
    /* register_panel */ nullptr,
    /* update_panel */ fakeUpdatePanel,
    /* unregister_panel */ nullptr,
    /* register_info_section */ nullptr,
    /* update_info_section */ nullptr,
    /* unregister_info_section */ nullptr,
    /* register_command */ nullptr,
    /* unregister_command */ nullptr,
    /* show_toast */ nullptr,
    /* update_tool_decor */ fakeUpdateToolDecor,
    /* register_tool_renderer */ nullptr,
    /* unregister_tool_renderer */ nullptr,
    /* bind_action_handler */ nullptr,
    /* unbind_action_handler */ nullptr,
    /* open_overlay */ fakeOpenOverlay,
    /* close_overlay */ nullptr,
};

/// 执行一次待处理的驱动请求 (推进桥的本地执行器一个有限步骤);
/// 反复调用直到 `false` 表示当前没有可推进的工作。
bool runDriver() {
    auto* ticket = g_pendingDriver;
    if (!ticket || ticket->done) {
        return false;
    }
    g_pendingDriver = nullptr;
    ticket->done    = true;
    if (ticket->drive) {
        ticket->drive(ticket->ud);
    }
    delete ticket;
    return true;
}

/// 接口表查询 (定义见下方: 需要先声明伪 runtime 表)
const void* PLUGINXX_CALL fakeQueryInterface(const PluginxxHost*, const PluginxxStringView* iid);

const PluginxxHostVtable g_fakeVtable = {
    /* alloc */ fakeAlloc,
    /* free */ fakeFree,
    /* query_interface */ fakeQueryInterface,
};

const void* PLUGINXX_CALL fakeQueryInterface(const PluginxxHost*, const PluginxxStringView* iid) {
    if (!iid || !iid->data) {
        return nullptr;
    }
    const std::string_view name{iid->data, static_cast<size_t>(iid->size)};
    if (name == AGENTXX_PLUGIN_IFACE_AGENT_TOOLS) {
        return &g_fakeTools;
    }
    if (name == AGENTXX_PLUGIN_IFACE_AGENT_HOOKS) {
        return &g_fakeHooks;
    }
    if (name == PLUGINXX_IFACE_CAPABILITIES) {
        return &g_fakeCapabilities;
    }
    if (name == AGENTXX_PLUGIN_IFACE_AGENT_GRAPH) {
        return &g_fakeGraph;
    }
    if (name == PLUGINXX_IFACE_COROUTINE_RUNTIME) {
        return &g_fakeRuntime;
    }
    if (name == AGENTXX_IFACE_CLIENT_UI) {
        return &g_fakeClientUi;
    }
    return nullptr;
}

/// 老宿主模拟: client 侧接口表一律不提供 (query_interface 返回 NULL)
///
/// 用途: 验证 SDK 便捷方法在"宿主不支持"时返回非 0 且不崩 (插件据此降级)
const void* PLUGINXX_CALL
    fakeQueryInterfaceLegacy(const PluginxxHost* host, const PluginxxStringView* iid) {
    if (!iid || !iid->data) {
        return nullptr;
    }
    const std::string_view name{iid->data, static_cast<size_t>(iid->size)};
    if (name == AGENTXX_IFACE_CLIENT_UI || name == AGENTXX_IFACE_CLIENT_EVENTS
        || name == AGENTXX_IFACE_CLIENT_SESSION || name == AGENTXX_IFACE_CLIENT_WIRE
        || name == AGENTXX_IFACE_CLIENT_SELF || name == AGENTXX_IFACE_CLIENT_JSON
        || name == AGENTXX_IFACE_CLIENT_LOG || name == AGENTXX_IFACE_CLIENT_TIMER
        || name == AGENTXX_IFACE_CLIENT_KEYBIND) {
        return nullptr;
    }
    return fakeQueryInterface(host, iid);
}

const PluginxxHostVtable g_legacyVtable = {
    /* alloc */ fakeAlloc,
    /* free */ fakeFree,
    /* query_interface */ fakeQueryInterfaceLegacy,
};

/// 完成通知探针：记录回调次数、状态与载荷。
struct NotifyProbe {
    int         calls  = 0;
    int32_t     status = -1;
    std::string payload;

    static void PLUGINXX_CALL done(void* ud, int32_t status, const PluginxxStringView* payload) {
        auto& probe = *static_cast<NotifyProbe*>(ud);
        ++probe.calls;
        probe.status = status;
        if (payload && payload->data && payload->size > 0) {
            probe.payload.assign(payload->data, static_cast<size_t>(payload->size));
        }
    }

    PluginxxOperatorNotify notify() {
        return PluginxxOperatorNotify{&NotifyProbe::done, this};
    }
};

/// 手动放行的挂起点：测试先让 start 返回，再改变外部状态并 resume。
struct Gate {
    bool*                    released = nullptr;
    std::coroutine_handle<>* handle   = nullptr;

    bool await_ready() const noexcept {
        return released && *released;
    }

    void await_suspend(std::coroutine_handle<> h) const noexcept {
        if (handle) {
            *handle = h;
        }
    }

    void await_resume() const noexcept {}
};

struct SdkCtx : PluginBase {};

/// 结束一个手动驱动的根协程：与 SDK resume 路径一致（destroy 帧 + 派发 notify +
/// 回收 opCleanup_）。
template<typename Promise>
void finishRoot(std::coroutine_handle<> h) {
    detail::finishIfDone(std::coroutine_handle<Promise>::from_address(h.address()));
}

} // namespace

TestResult testPluginSdk() {
    TestResult         result;
    const PluginxxHost host{&g_fakeVtable, nullptr};

    /// F13：tool 根操作的输入由 Request 拥有 —— 宿主借用缓冲区失效后，协程挂起
    /// 恢复继续读取 args 仍必须得到原值。
    {
        bool                    released = false;
        std::coroutine_handle<> handle{};
        std::string             seen;
        std::string             seenSession;

        // box 先声明：它必须比 ctx（持有 shim + 协程）活得更久。
        struct Box {
            bool*                    released = nullptr;
            std::coroutine_handle<>* handle   = nullptr;
            std::string*             seen     = nullptr;
            std::string*             session  = nullptr;
        } box{&released, &handle, &seen, &seenSession};

        SdkCtx ctx;
        ctx.init(&host);

        g_captured = CapturedRegistration{};
        agentxx::plugin::tool(
            ctx,
            "sdk_owns_args",
            "depict",
            "{}",
            [&box](SdkCtx&, std::string_view args, OpCtl& ctl) -> Task<std::string> {
                // 挂起前记录视图；恢复后（宿主借用缓冲区已失效）再读取。
                auto argsView = args;
                co_await Gate{box.released, box.handle};
                box.seen->assign(argsView.data(), argsView.size());
                box.session->assign(ctl.threadId.data(), ctl.threadId.size());
                co_return *box.seen;
            }
        );
        XX_TEST_EXPECT_TRUE(g_captured.hasTool);

        auto spec = g_captured.tool;
        XX_TEST_EXPECT_TRUE(spec.execute_start != nullptr);

        // 宿主借用缓冲区：start 返回后立即被覆写，插件不得再依赖它。
        std::string borrowedArgs = R"({"n":42})";
        std::string borrowedSid  = "sess-42";
        auto        argsSv       = PluginStringView::from(borrowedArgs.data(), borrowedArgs.size());
        auto        sidSv        = PluginStringView::from(borrowedSid.data(), borrowedSid.size());
        auto        cidSv        = PluginStringView::fromCstr("call-42");

        NotifyProbe probe;
        auto        notify = probe.notify();
        void* op = spec.execute_start(spec.user_data, &argsSv, &sidSv, &cidSv, &notify, nullptr);
        XX_TEST_EXPECT_TRUE(op != nullptr);
        XX_TEST_EXPECT_EQ(probe.calls, 0);
        // 首步由 host driver 推进: 跑一次驱动让协程挂起到 Gate
        XX_TEST_EXPECT_TRUE(runDriver());
        // 借用缓冲区失效
        borrowedArgs.assign(borrowedArgs.size(), 'x');
        borrowedSid.assign(borrowedSid.size(), 'y');

        XX_TEST_EXPECT_TRUE(handle != nullptr);
        released = true;
        handle.resume();
        XX_TEST_EXPECT_EQ(seen, std::string{R"({"n":42})"});
        XX_TEST_EXPECT_EQ(seenSession, std::string{"sess-42"});
        // 协程已到 final_suspend，按 SDK 根完成流程收束
        finishRoot<Task<std::string>::promise_type>(handle);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_OK);
        XX_TEST_EXPECT_EQ(probe.payload, std::string{R"({"n":42})"});
    }

    /// F19/完成协议：同步 void 钩子正常返回 -> OK；抛异常 -> FAILED；
    /// 两种情况都恰好一次完成，且不返回 provider 句柄。
    {
        SdkCtx okCtx;
        okCtx.init(&host);
        g_captured = CapturedRegistration{};
        agentxx::plugin::hook(
            okCtx,
            AGENTXX_PLUGIN_HOOK_AGENT_START,
            [](SdkCtx&, AgentxxPluginHookPoint, std::string_view) {}
        );
        XX_TEST_EXPECT_TRUE(g_captured.hasHook);
        {
            auto        spec = g_captured.hook;
            auto        in   = PluginStringView::fromCstr(R"({"k":1})");
            NotifyProbe probe;
            auto        notify = probe.notify();
            void*       op     = spec.hook_start(spec.user_data, spec.point, &in, &notify, nullptr);
            XX_TEST_EXPECT_TRUE(op == nullptr);
            XX_TEST_EXPECT_EQ(probe.calls, 1);
            XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_OK);
        }

        SdkCtx throwCtx;
        throwCtx.init(&host);
        g_captured = CapturedRegistration{};
        agentxx::plugin::hook(
            throwCtx,
            AGENTXX_PLUGIN_HOOK_AGENT_START,
            [](SdkCtx&, std::string_view) {
                throw std::runtime_error("hook boom");
            }
        );
        XX_TEST_EXPECT_TRUE(g_captured.hasHook);
        {
            auto        spec = g_captured.hook;
            auto        in   = PluginStringView::fromCstr("{}");
            NotifyProbe probe;
            auto        notify = probe.notify();
            void*       op     = spec.hook_start(spec.user_data, spec.point, &in, &notify, nullptr);
            XX_TEST_EXPECT_TRUE(op == nullptr);
            XX_TEST_EXPECT_EQ(probe.calls, 1);
            XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_FAILED);
            XX_TEST_EXPECT_TRUE(probe.payload.find("hook boom") != std::string::npos);
        }
    }

    /// F19：返回 Task<void> 的钩子是异步的 —— start 返回 provider 句柄且不完成，
    /// 协程真正结束后才 exactly-once 完成；hook_cancel 可用。
    {
        bool                    released = false;
        std::coroutine_handle<> handle{};
        std::string             seenInput;
        bool                    finished = false;

        struct AsyncBox {
            bool*                    released = nullptr;
            std::coroutine_handle<>* handle   = nullptr;
            std::string*             input    = nullptr;
            bool*                    finished = nullptr;
        } box{&released, &handle, &seenInput, &finished};

        SdkCtx ctx;
        ctx.init(&host);
        g_captured = CapturedRegistration{};
        agentxx::plugin::hook(
            ctx,
            AGENTXX_PLUGIN_HOOK_TOOL_START,
            [&box](SdkCtx&, AgentxxPluginHookPoint, std::string_view input) -> Task<void> {
                box.input->assign(input.data(), input.size());
                co_await Gate{box.released, box.handle};
                *box.finished = true;
                co_return;
            }
        );
        XX_TEST_EXPECT_TRUE(g_captured.hasHook);

        auto spec = g_captured.hook;
        XX_TEST_EXPECT_TRUE(spec.hook_cancel != nullptr);
        auto        in = PluginStringView::fromCstr(R"({"hook":true})");
        NotifyProbe probe;
        auto        notify = probe.notify();
        void*       op     = spec.hook_start(spec.user_data, spec.point, &in, &notify, nullptr);

        XX_TEST_EXPECT_TRUE(op != nullptr); ///< 未完成 -> 返回 provider 句柄
        XX_TEST_EXPECT_EQ(probe.calls, 0);  ///< 不得提前完成
        XX_TEST_EXPECT_TRUE(runDriver());   ///< 首步由 host driver 推进 (挂起到 Gate)
        XX_TEST_EXPECT_EQ(seenInput, std::string{R"({"hook":true})"});
        XX_TEST_EXPECT_FALSE(finished);

        // 借用缓冲区失效后协程仍能完成（输入由 Request 拥有）
        released = true;
        XX_TEST_EXPECT_TRUE(handle != nullptr);
        handle.resume();
        XX_TEST_EXPECT_TRUE(finished);
        finishRoot<Task<void>::promise_type>(handle);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_OK);
    }

    /// capability: 同步字符串能力 + 异步 Task<string> 能力 (统一 root adapter)。
    {
        SdkCtx ctx;
        ctx.init(&host);
        g_captured = CapturedRegistration{};
        agentxx::plugin::capability(
            ctx,
            "test.cap.sync",
            [](SdkCtx&, std::string_view method, std::string_view args) -> std::string {
                return fmt::format(R"({{"method":"{}","args":{}}})", method, args);
            }
        );
        XX_TEST_EXPECT_TRUE(g_captured.hasCapability);

        NotifyProbe probe;
        auto        notify = probe.notify();
        auto        method = PluginStringView::fromCstr("ping");
        auto        args   = PluginStringView::fromCstr(R"({"n":1})");
        void* op = g_captured.capStart(g_captured.capUd, nullptr, &method, &args, &notify, nullptr);
        XX_TEST_EXPECT_TRUE(op == nullptr); ///< 同步完成不返回句柄
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_OK);
        XX_TEST_EXPECT_TRUE(probe.payload.find("ping") != std::string::npos);
    }
    {
        bool                    released = false;
        std::coroutine_handle<> handle{};
        bool                    finished = false;

        struct AsyncBox {
            bool*                    released = nullptr;
            std::coroutine_handle<>* handle   = nullptr;
            bool*                    finished = nullptr;
        } box{&released, &handle, &finished};

        SdkCtx ctx;
        ctx.init(&host);
        g_captured = CapturedRegistration{};
        agentxx::plugin::capability(
            ctx,
            "test.cap.async",
            [&box](SdkCtx&, std::string_view method, std::string_view) -> Task<std::string> {
                co_await Gate{box.released, box.handle};
                *box.finished = true;
                co_return fmt::format("done:{}", method);
            }
        );
        XX_TEST_EXPECT_TRUE(g_captured.hasCapability);
        XX_TEST_EXPECT_TRUE(g_captured.capCancel != nullptr);

        NotifyProbe probe;
        auto        notify = probe.notify();
        auto        method = PluginStringView::fromCstr("async-ping");
        auto        args   = PluginStringView::fromCstr("{}");
        void* op = g_captured.capStart(g_captured.capUd, nullptr, &method, &args, &notify, nullptr);
        XX_TEST_EXPECT_TRUE(op != nullptr); ///< 未完成 -> provider 句柄
        XX_TEST_EXPECT_EQ(probe.calls, 0);  ///< 不得提前完成
        XX_TEST_EXPECT_TRUE(runDriver());   ///< 首步由 host driver 推进 (挂起到 Gate)
        XX_TEST_EXPECT_FALSE(finished);

        // 借用缓冲区 (method/args 视图) 失效后协程仍能完成 (Request 拥有输入)。
        released = true;
        XX_TEST_EXPECT_TRUE(handle != nullptr);
        handle.resume();
        XX_TEST_EXPECT_TRUE(finished);
        finishRoot<Task<std::string>::promise_type>(handle);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_OK);
        XX_TEST_EXPECT_TRUE(probe.payload.find("done:async-ping") != std::string::npos);
    }

    /// graph_node: 快同步节点（返回节点输出 JSON）+ 输入拥有化 + 取消句柄。
    {
        SdkCtx ctx;
        ctx.init(&host);
        g_captured       = CapturedRegistration{};
        const int32_t rc = agentxx::plugin::graph_node(
            ctx,
            "test.node.sync",
            R"({"type":"object"})",
            [](SdkCtx&, const RootRequest& req) -> std::string {
                return fmt::format(
                    R"({{"node":"{}","thread":"{}","state":{}}})",
                    req.node(),
                    req.session(),
                    req.state()
                );
            }
        );
        XX_TEST_EXPECT_EQ(rc, 0);
        XX_TEST_EXPECT_TRUE(g_captured.hasGraphNode);
        XX_TEST_EXPECT_TRUE(g_captured.graphNode.run_start != nullptr);
        XX_TEST_EXPECT_TRUE(g_captured.graphNode.run_cancel != nullptr);

        auto        spec    = g_captured.graphNode;
        auto        nameSv  = PluginStringView::fromCstr("n1");
        auto        cfgSv   = PluginStringView::fromCstr(R"({"intents":["a"]})");
        auto        stateSv = PluginStringView::fromCstr(R"({"channels":{"x":1}})");
        auto        tidSv   = PluginStringView::fromCstr("sess-1");
        NotifyProbe probe;
        auto        notify = probe.notify();
        void*       op
            = spec.run_start(spec.user_data, &nameSv, &cfgSv, &stateSv, &tidSv, &notify, nullptr);
        XX_TEST_EXPECT_TRUE(op == nullptr); ///< 快同步: 调用内完成, 不返回句柄
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_OK);
        XX_TEST_EXPECT_TRUE(probe.payload.find("n1") != std::string::npos);
        XX_TEST_EXPECT_TRUE(probe.payload.find("sess-1") != std::string::npos);
        XX_TEST_EXPECT_TRUE(probe.payload.find("channels") != std::string::npos);
    }

    /// graph_node: 快同步节点异常 → FAILED（不越界、不悬垂）。
    {
        SdkCtx ctx;
        ctx.init(&host);
        g_captured       = CapturedRegistration{};
        const int32_t rc = agentxx::plugin::graph_node(
            ctx,
            "test.node.throw",
            "{}",
            [](SdkCtx&, const RootRequest&) -> std::string {
                throw std::runtime_error("node boom");
            }
        );
        XX_TEST_EXPECT_EQ(rc, 0);
        auto        spec = g_captured.graphNode;
        NotifyProbe probe;
        auto        notify = probe.notify();
        void*       op
            = spec.run_start(spec.user_data, nullptr, nullptr, nullptr, nullptr, &notify, nullptr);
        XX_TEST_EXPECT_TRUE(op == nullptr);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_FAILED);
        XX_TEST_EXPECT_TRUE(probe.payload.find("node boom") != std::string::npos);
    }

    /// graph_node: Task<string> 节点 —— provider 句柄 + 借用输入失效后仍完成。
    {
        bool                    released = false;
        std::coroutine_handle<> handle{};
        bool                    finished = false;
        std::string             seenState;

        struct AsyncBox {
            bool*                    released  = nullptr;
            std::coroutine_handle<>* handle    = nullptr;
            bool*                    finished  = nullptr;
            std::string*             seenState = nullptr;
        } box{&released, &handle, &finished, &seenState};

        SdkCtx ctx;
        ctx.init(&host);
        g_captured       = CapturedRegistration{};
        const int32_t rc = agentxx::plugin::graph_node(
            ctx,
            "test.node.async",
            "{}",
            [&box](SdkCtx&, const RootRequest& req, OpCtl ctl) -> Task<std::string> {
                std::string node{req.node()};
                std::string state{req.state()};
                co_await Gate{box.released, box.handle};
                if (ctl.cancelled()) {
                    co_return std::string{"cancelled"};
                }
                *box.seenState = state;
                *box.finished  = true;
                co_return fmt::format(R"({{"async":"{}"}})", node);
            }
        );
        XX_TEST_EXPECT_EQ(rc, 0);
        XX_TEST_EXPECT_TRUE(g_captured.hasGraphNode);
        XX_TEST_EXPECT_TRUE(g_captured.graphNode.run_cancel != nullptr);

        auto        spec    = g_captured.graphNode;
        auto        nameSv  = PluginStringView::fromCstr("n2");
        auto        cfgSv   = PluginStringView::fromCstr("{}");
        auto        stateSv = PluginStringView::fromCstr(R"({"channels":{}})");
        auto        tidSv   = PluginStringView::fromCstr("sess-2");
        NotifyProbe probe;
        auto        notify = probe.notify();
        void*       op
            = spec.run_start(spec.user_data, &nameSv, &cfgSv, &stateSv, &tidSv, &notify, nullptr);
        XX_TEST_EXPECT_TRUE(op != nullptr); ///< 未完成 -> provider 句柄
        XX_TEST_EXPECT_EQ(probe.calls, 0);
        XX_TEST_EXPECT_TRUE(runDriver()); ///< 首步由 host driver 推进 (挂起到 Gate)
        XX_TEST_EXPECT_FALSE(finished);

        // 宿主借用缓冲区失效后 (置空视图) 协程仍能完成 (Request 拥有输入)
        released = true;
        XX_TEST_EXPECT_TRUE(handle != nullptr);
        handle.resume();
        XX_TEST_EXPECT_TRUE(finished);
        finishRoot<Task<std::string>::promise_type>(handle);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_OK);
        XX_TEST_EXPECT_TRUE(probe.payload.find("n2") != std::string::npos);
        XX_TEST_EXPECT_EQ(seenState, std::string{R"({"channels":{}})"});
    }
    // ---- client 侧 SDK 便捷方法: 组件构建器 → 面板/装饰/overlay 提交 ----
    //
    // 用伪 client UI 表捕获提交的 JSON: 覆盖"构建器 → JSON → 接口调用"这条
    // 端到端路径 (含老宿主缺失该表时的降级)。
    {
        ClientPluginBase client;
        client.host  = &host;
        client.iface = ClientIfaces::query(&host);
        XX_TEST_EXPECT_TRUE(client.iface.ui == &g_fakeClientUi);

        // 伪句柄: 伪 update_* 忽略句柄, 只要求非空
        AgentxxPanel*      panel  = reinterpret_cast<AgentxxPanel*>(uintptr_t{1});
        AgentxxStatusItem* status = reinterpret_cast<AgentxxStatusItem*>(uintptr_t{1});

        // 1) 构建器 → setPanelItems
        g_clientUiCapture = ClientUiCapture{};
        {
            agentxx::ui::Items ui;
            ui.table({
                .columns = {{"Path", "left", 0}, {"Scope", "right", 6}},
                .rows    = {{"a.txt", "write"}},
            });
            ui.meter(72, 100, {.width = 4, .label = "CPU"});
            XX_TEST_EXPECT_EQ(client.setPanelItems(panel, ui), 0);
        }
        XX_TEST_EXPECT_EQ(g_clientUiCapture.panelUpdates, 1);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("\"table\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("a.txt") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("\"meter\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("\"items\"") != std::string::npos);

        // 2) panelItems 就地构建器: 作用域结束自动提交
        {
            auto writer = client.panelItems(panel);
            writer->text("from-writer").bold(true);
            writer->checkbox("opt", "Opt", true);
        }
        XX_TEST_EXPECT_EQ(g_clientUiCapture.panelUpdates, 2);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("from-writer") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("\"bold\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("checkbox") != std::string::npos);

        // 3) form(...) → 分组框 + 控件 + 提交行
        {
            agentxx::ui::Items ui;
            ui.form({
                .title       = "Options",
                .fields      = {agentxx::ui::Items{}.checkbox("detail", "Detail", false)},
                .submitLabel = "APPLY",
            });
            XX_TEST_EXPECT_EQ(client.setPanelItems(panel, ui), 0);
        }
        XX_TEST_EXPECT_EQ(g_clientUiCapture.panelUpdates, 3);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("\"box\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("\"submit\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("APPLY") != std::string::npos);

        // 4) 状态栏 JSON
        XX_TEST_EXPECT_EQ(client.setStatusText(status, "turns: 3"), 0);
        XX_TEST_EXPECT_EQ(g_clientUiCapture.statusUpdates, 1);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.statusJson.find("turns: 3") != std::string::npos);

        // 5) 工具消息装饰: DecorSpec → update_tool_decor; 清除 = 空 JSON
        {
            ClientPluginBase::DecorSpec spec;
            spec.displayName = "Plan";
            spec.summary     = "[~] a; [ ] b";
            spec.items.table({
                .columns = {{"Step", "left", 0}},
                .rows    = {{"a"}, {"b"}},
            });
            XX_TEST_EXPECT_EQ(client.setToolDecor("call_1", spec), 0);
            XX_TEST_EXPECT_EQ(g_clientUiCapture.decorUpdates, 1);
            XX_TEST_EXPECT_EQ(g_clientUiCapture.decorToolCallId, std::string{"call_1"});
            XX_TEST_EXPECT_TRUE(g_clientUiCapture.decorJson.find("\"displayName\":\"Plan\"") != std::string::npos);
            XX_TEST_EXPECT_TRUE(g_clientUiCapture.decorJson.find("\"table\"") != std::string::npos);

            XX_TEST_EXPECT_EQ(client.clearToolDecor("call_1"), 0);
            XX_TEST_EXPECT_EQ(g_clientUiCapture.decorUpdates, 2);
            XX_TEST_EXPECT_TRUE(g_clientUiCapture.decorJson.empty());
        }

        // 6) overlay: 组件树作为 payload + 尺寸选项透传
        {
            g_clientUiCapture = ClientUiCapture{};
            agentxx::ui::Items ui;
            ui.table({.columns = {{"P", "left", 0}}, .rows = {{"x"}}});
            XX_TEST_EXPECT_EQ(
                client.showItemsOverlay("Files", ui, "{\"size\":\"large\"}"),
                0
            );
            XX_TEST_EXPECT_EQ(g_clientUiCapture.overlays, 1);
            XX_TEST_EXPECT_TRUE(g_clientUiCapture.overlayPayload.find("\"items\"") != std::string::npos);
            XX_TEST_EXPECT_EQ(g_clientUiCapture.overlayExtra, std::string{"{\"size\":\"large\"}"});
        }

        // 7) 未知 kind 原样透传 (老宿主忽略, 数据层向前兼容)
        {
            agentxx::ui::Items ui;
            ui.raw(utilxx_base::Json{{"kind", "future_widget"}, {"fallback", "n/a"}});
            XX_TEST_EXPECT_EQ(client.setPanelItems(panel, ui), 0);
            XX_TEST_EXPECT_TRUE(
                g_clientUiCapture.panelItems.find("future_widget") != std::string::npos
            );
            XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("n/a") != std::string::npos);
        }
    }

    // ---- 老宿主降级: client UI 表缺失 → 便捷方法返回非 0, 不崩 ----
    {
        const PluginxxHost legacyHost{&g_legacyVtable, nullptr};
        ClientPluginBase   client;
        client.host  = &legacyHost;
        client.iface = ClientIfaces::query(&legacyHost);
        XX_TEST_EXPECT_TRUE(client.iface.ui == nullptr);

        AgentxxPanel* panel = reinterpret_cast<AgentxxPanel*>(uintptr_t{1});
        agentxx::ui::Items ui;
        ui.text("x");
        XX_TEST_EXPECT_TRUE(client.setPanelItems(panel, ui) != 0);
        XX_TEST_EXPECT_TRUE(client.setToolDecor("call_1", {}) != 0);
        XX_TEST_EXPECT_TRUE(client.showItemsOverlay("t", ui) != 0);
        XX_TEST_EXPECT_FALSE(client.hostSupports("agentxx.client.components"));
        XX_TEST_EXPECT_TRUE(client.regionSize("p").width == 0);
    }

    return result;
}
} // namespace agentxx::test
