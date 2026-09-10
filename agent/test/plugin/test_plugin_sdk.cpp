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
#include "test_plugin_sdk.h"

#include "agentxx/plugin/api/plugin_kit.h"

#include <atomic>
#include <coroutine>
#include <string>
#include <string_view>

#define XX_TEST_PASSED result.passed
#define XX_TEST_FAILED result.failed

namespace agentxx::test {
namespace {
using namespace agentxx::plugin;

/// SDK helper 注册出来的 spec 捕获（替代真实宿主的注册表）。
struct CapturedRegistration {
    AgentxxPluginToolSpec tool{};
    bool                  hasTool = false;
    AgentxxPluginHookSpec hook{};
    bool                  hasHook = false;
    AgentxxPluginCapabilityStartFunction capStart = nullptr;
    AgentxxPluginOperatorCancelFunction  capCancel = nullptr;
    void*                                capUd     = nullptr;
    bool                                 hasCapability = false;
};

CapturedRegistration g_captured;

int32_t AGENTXX_PLUGIN_CALL
    fakeRegisterTool(const AgentxxPluginHost*, const AgentxxPluginToolSpec* spec) {
    if (!spec) {
        return -1;
    }
    g_captured.tool    = *spec;
    g_captured.hasTool = true;
    return 0;
}

int32_t AGENTXX_PLUGIN_CALL
    fakeRegisterHook(const AgentxxPluginHost*, const AgentxxPluginHookSpec* spec) {
    if (!spec) {
        return -1;
    }
    g_captured.hook    = *spec;
    g_captured.hasHook = true;
    return 0;
}

int32_t AGENTXX_PLUGIN_CALL fakeRegisterCapabilityEx(
    const AgentxxPluginHost*,
    const AgentxxPluginStringView*,
    AgentxxPluginCapabilityStartFunction start,
    AgentxxPluginOperatorCancelFunction  cancel,
    void*                                ctx
) {
    g_captured.capStart      = start;
    g_captured.capCancel     = cancel;
    g_captured.capUd         = ctx;
    g_captured.hasCapability = true;
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

const AgentxxPluginCapabilitiesIface g_fakeCapabilities = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES_VERSION,
    /* struct_size */ sizeof(AgentxxPluginCapabilitiesIface),
    /* register_capability */ nullptr,
    /* register_capability_ex */ fakeRegisterCapabilityEx,
    /* unregister_capability */ nullptr,
    /* has_capability */ nullptr,
    /* invoke_capability_async */ nullptr,
    /* op_cancel */ nullptr,
};

const void* AGENTXX_PLUGIN_CALL
    fakeQueryInterface(const AgentxxPluginHost*, const AgentxxPluginStringView* iid) {
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
    if (name == AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES) {
        return &g_fakeCapabilities;
    }
    return nullptr;
}

const AgentxxHostVtable g_fakeVtable = {
    /* alloc */ nullptr,
    /* free */ nullptr,
    /* query_interface */ fakeQueryInterface,
};

/// 完成通知探针：记录回调次数、状态与载荷。
struct NotifyProbe {
    int         calls  = 0;
    int32_t     status = -1;
    std::string payload;

    static void AGENTXX_PLUGIN_CALL done(
        void* ud, int32_t status, const AgentxxPluginStringView* payload
    ) {
        auto& probe  = *static_cast<NotifyProbe*>(ud);
        ++probe.calls;
        probe.status = status;
        if (payload && payload->data && payload->size > 0) {
            probe.payload.assign(payload->data, static_cast<size_t>(payload->size));
        }
    }

    AgentxxPluginOperatorNotify notify() {
        return AgentxxPluginOperatorNotify{&NotifyProbe::done, this};
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
    TestResult result;
    const AgentxxPluginHost host{&g_fakeVtable, nullptr};

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
        void*       op = spec.execute_start(
            spec.user_data,
            &argsSv,
            &sidSv,
            &cidSv,
            &notify,
            nullptr
        );
        XX_TEST_EXPECT_TRUE(op != nullptr);
        XX_TEST_EXPECT_EQ(probe.calls, 0);
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
        XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_OK);
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
            auto                spec = g_captured.hook;
            auto                in   = PluginStringView::fromCstr(R"({"k":1})");
            NotifyProbe         probe;
            auto                notify = probe.notify();
            void*               op
                = spec.hook_start(spec.user_data, spec.point, &in, &notify, nullptr);
            XX_TEST_EXPECT_TRUE(op == nullptr);
            XX_TEST_EXPECT_EQ(probe.calls, 1);
            XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_OK);
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
            void*       op = spec.hook_start(spec.user_data, spec.point, &in, &notify, nullptr);
            XX_TEST_EXPECT_TRUE(op == nullptr);
            XX_TEST_EXPECT_EQ(probe.calls, 1);
            XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_FAILED);
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

        auto        spec = g_captured.hook;
        XX_TEST_EXPECT_TRUE(spec.hook_cancel != nullptr);
        auto        in = PluginStringView::fromCstr(R"({"hook":true})");
        NotifyProbe probe;
        auto        notify = probe.notify();
        void*       op = spec.hook_start(spec.user_data, spec.point, &in, &notify, nullptr);

        XX_TEST_EXPECT_TRUE(op != nullptr); ///< 未完成 -> 返回 provider 句柄
        XX_TEST_EXPECT_EQ(probe.calls, 0);  ///< 不得提前完成
        XX_TEST_EXPECT_EQ(seenInput, std::string{R"({"hook":true})"});
        XX_TEST_EXPECT_FALSE(finished);

        // 借用缓冲区失效后协程仍能完成（输入由 Request 拥有）
        released = true;
        XX_TEST_EXPECT_TRUE(handle != nullptr);
        handle.resume();
        XX_TEST_EXPECT_TRUE(finished);
        finishRoot<Task<void>::promise_type>(handle);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_OK);
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
        void*       op = g_captured.capStart(
            g_captured.capUd,
            nullptr,
            &method,
            &args,
            &notify,
            nullptr
        );
        XX_TEST_EXPECT_TRUE(op == nullptr); ///< 同步完成不返回句柄
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_OK);
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
        void*       op = g_captured.capStart(
            g_captured.capUd,
            nullptr,
            &method,
            &args,
            &notify,
            nullptr
        );
        XX_TEST_EXPECT_TRUE(op != nullptr); ///< 未完成 -> provider 句柄
        XX_TEST_EXPECT_EQ(probe.calls, 0);  ///< 不得提前完成
        XX_TEST_EXPECT_FALSE(finished);

        // 借用缓冲区 (method/args 视图) 失效后协程仍能完成 (Request 拥有输入)。
        released = true;
        XX_TEST_EXPECT_TRUE(handle != nullptr);
        handle.resume();
        XX_TEST_EXPECT_TRUE(finished);
        finishRoot<Task<std::string>::promise_type>(handle);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_OK);
        XX_TEST_EXPECT_TRUE(probe.payload.find("done:async-ping") != std::string::npos);
    }
    return result;
}
} // namespace agentxx::test
