/// 插件 C++ SDK 根操作适配测试（plugin.md 6.1/6.2/6.3）。
///
/// 覆盖：
/// - 输入所有权：tool 根操作的 args/session/tool_call_id 由 Request 拥有，
///   主程序借用缓冲区失效后协程继续读取仍然正确；
/// - 完成协议：同步能力（正常/抛异常）与异步 asio 能力都是 exactly-once 完成；
/// - 功能埋点实现行：`providePoint` 两种业务体形态（收上下文+载荷 / 只收载荷）都能登记；
/// - 返回类型分发：返回字符串立即完成，返回 `asio::awaitable<T>` 时由协程真正
///   结束后才完成，并返回主程序可取消的 provider 句柄。
///
/// 用例不依赖真实管理器：用伪主程序接口表捕获 SDK 注册出来的 spec，再直接按 ABI
/// 调用 `execute_start`/`run_start`，因此可以精确控制借用缓冲区的生命周期。
#include "agentxx-test/plugin/test_plugin_sdk.h"

#include "agentxx/plugin/api/agentxx_point_ids.h"
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"

#include "asio/co_spawn.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

#define XX_TEST_PASSED result.passed
#define XX_TEST_FAILED result.failed

namespace agentxx::test {
namespace {
using namespace agentxx::plugin;

/// 伪主程序的挂载引用 (本模块不需要真实挂载: `seq` = 0 表示不校验)
const ScopeRef g_agentRef{"root", 0};

/// SDK helper 注册出来的 spec 捕获（替代真实主程序的注册表）。
struct CapturedRegistration {
    AgentxxPluginToolSpec           tool{};
    bool                            hasTool = false;
    PluginxxCapabilityStartFunction capStart      = nullptr;
    PluginxxOperatorCancelFunction  capCancel     = nullptr;
    void*                           capUd         = nullptr;
    bool                            hasCapability = false;
    AgentxxPluginGraphNodeTypeSpec  graphNode{};
    bool                            hasGraphNode = false;
};

CapturedRegistration g_captured;

PluginxxOperatorHandle* PLUGINXX_CALL fakeRegisterTool(
    const PluginxxHost*,
    const PluginxxScopeRef* agent,
    const AgentxxPluginToolSpec* spec,
    PluginxxOperatorCallback cb,
    void* ud,
    PluginxxString* error_out
) {
    (void)error_out;
    if (!agent || !spec) {
        return nullptr;
    }
    g_captured.tool    = *spec;
    g_captured.hasTool = true;
    if (cb) {
        cb(ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return reinterpret_cast<PluginxxOperatorHandle*>(1);
}

/// 能力登记 (spec 里带开始/取消处理器与上下文)
///
/// 表成员是异步操作契约: 记下 spec 后立刻用完成回调交回成功 (假主程序)。
PluginxxOperatorHandle* PLUGINXX_CALL fakeRegisterCapability(
    const PluginxxHost*,
    const PluginxxScopeRef*,
    const PluginxxCapabilitySpec* spec,
    PluginxxOperatorCallback      cb,
    void*                         cb_ud,
    PluginxxString*               error_out
) {
    (void)error_out;
    if (!spec) {
        return nullptr;
    }
    g_captured.capStart      = spec->run_start;
    g_captured.capCancel     = spec->cancel;
    g_captured.capUd         = spec->user_data;
    g_captured.hasCapability = true;
    if (cb) {
        cb(cb_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return reinterpret_cast<PluginxxOperatorHandle*>(1);
}

PluginxxOperatorHandle* PLUGINXX_CALL fakeRegisterGraphNodeType(
    const PluginxxHost*,
    const PluginxxScopeRef* agent,
    const AgentxxPluginGraphNodeTypeSpec* spec,
    PluginxxOperatorCallback cb,
    void* ud,
    PluginxxString* error_out
) {
    (void)error_out;
    if (!agent || !spec) {
        return nullptr;
    }
    g_captured.graphNode    = *spec;
    g_captured.hasGraphNode = true;
    if (cb) {
        cb(ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return reinterpret_cast<PluginxxOperatorHandle*>(1);
}

const AgentxxPluginToolsIface g_fakeTools = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION,
    /* struct_size */ sizeof(AgentxxPluginToolsIface),
    /* register_tool */ fakeRegisterTool,
    /* unregister_tool */ nullptr,
    /* call_tool */ nullptr,
};

const PluginxxCapabilitiesIface g_fakeCapabilities = {
    /* version */ PLUGINXX_IFACE_CAPABILITIES_VERSION,
    /* struct_size */ sizeof(PluginxxCapabilitiesIface),
    /* register_capability */ fakeRegisterCapability,
    /* unregister_capability */ nullptr,
    /* invoke_capability */ nullptr,
    /* capability_info */ nullptr,
    /* list_capabilities */ nullptr,
    /* invoke_capability_ex */ nullptr,
    /* unregister_capability_ex */ nullptr,
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

/// 伪主程序内存操作 (kit 会用 host->vtable->free 释放 request_driver 的 error_out)
void* PLUGINXX_CALL fakeAlloc(uint64_t size) {
    return std::malloc(static_cast<size_t>(size));
}

void PLUGINXX_CALL fakeFree(void* ptr) {
    std::free(ptr);
}

/// 伪主程序的驱动登记: 记录待执行的驱动回调, 由用例经 [runDriver] 手动执行
/// (等价于主程序 IO 线程执行一次 `poll_one`)。
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

/// 伪定时器 (定义见下方: 需要先声明, 供 [g_fakeScheduler] 引用)
PluginxxOperatorHandle* PLUGINXX_CALL fakeSleep(
    const PluginxxHost*      host,
    int64_t                  ms,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
);

/// 伪调度表: 驱动两成员走同一份 fake 实现 (v2 起 coroutine_runtime 已并入 scheduler)
const PluginxxSchedulerIface g_fakeScheduler = {
    /* version */ PLUGINXX_IFACE_SCHEDULER_VERSION,
    /* struct_size */ sizeof(PluginxxSchedulerIface),
    /* is_io_thread */ fakeIsIoThread,
    /* sleep */ fakeSleep,
    /* offload */ nullptr,
    /* request_driver */ fakeRequestDriver,
    /* cancel_driver */ fakeCancelDriver,
};

/// 伪 client UI 接口表: 捕获 SDK 便捷方法提交的 JSON (替代真实主程序注册表)
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

/// 伪主程序: 表入口必须返回非 NULL 句柄 (NULL = 未受理; 本模块只关心回调内容)
PluginxxOperatorHandle* fakeAcceptedHandle() {
    static char dummy = 0;
    return reinterpret_cast<PluginxxOperatorHandle*>(&dummy);
}

PluginxxOperatorHandle* PLUGINXX_CALL fakeUpdatePanel(
    const PluginxxHost*,
    AgentxxPanel*,
    const PluginxxStringView* items,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*
) {
    ++g_clientUiCapture.panelUpdates;
    g_clientUiCapture.panelItems
        = (items && items->data) ? std::string{items->data, static_cast<size_t>(items->size)} : "";
    if (cb) {
        cb(ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return fakeAcceptedHandle();
}

PluginxxOperatorHandle* PLUGINXX_CALL fakeUpdateStatusItem(
    const PluginxxHost*,
    AgentxxStatusItem*,
    const PluginxxStringView* json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*
) {
    ++g_clientUiCapture.statusUpdates;
    g_clientUiCapture.statusJson
        = (json && json->data) ? std::string{json->data, static_cast<size_t>(json->size)} : "";
    if (cb) {
        cb(ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return fakeAcceptedHandle();
}

PluginxxOperatorHandle* PLUGINXX_CALL fakeUpdateToolDecor(
    const PluginxxHost*,
    const PluginxxStringView* toolCallId,
    const PluginxxStringView* json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*
) {
    ++g_clientUiCapture.decorUpdates;
    g_clientUiCapture.decorToolCallId
        = (toolCallId && toolCallId->data)
              ? std::string{toolCallId->data, static_cast<size_t>(toolCallId->size)}
              : "";
    g_clientUiCapture.decorJson
        = (json && json->data) ? std::string{json->data, static_cast<size_t>(json->size)} : "";
    if (cb) {
        cb(ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return fakeAcceptedHandle();
}

PluginxxOperatorHandle* PLUGINXX_CALL fakeOpenOverlay(
    const PluginxxHost*,
    const AgentxxOverlaySpec* spec,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*
) {
    if (!spec) {
        return nullptr;
    }
    ++g_clientUiCapture.overlays;
    g_clientUiCapture.overlayPayload
        = (spec->payload.data)
              ? std::string{spec->payload.data, static_cast<size_t>(spec->payload.size)}
              : "";
    g_clientUiCapture.overlayExtra
        = (spec->extra_json.data)
              ? std::string{spec->extra_json.data, static_cast<size_t>(spec->extra_json.size)}
              : "";
    if (cb) {
        cb(ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return fakeAcceptedHandle();
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
    /* bind_action_handler */ nullptr,
    /* unbind_action_handler */ nullptr,
    /* open_overlay */ fakeOpenOverlay,
    /* close_overlay */ nullptr,
};

/// 伪主程序的定时器登记: 记录到期回调, 由 [runDriver] 在到期后触发
///
/// 桥的轮询退避 (`PollOneBridge` 的 10ms) 需要 `scheduler.sleep` 才能安排下一次
/// 驱动请求, 所以伪造的调度器必须提供它。
struct FakeTimer {
    std::chrono::steady_clock::time_point due;
    PluginxxOperatorCallback              cb = nullptr;
    void*                                 ud = nullptr;
};

std::vector<FakeTimer> g_fakeTimers;

PluginxxOperatorHandle* PLUGINXX_CALL fakeSleep(
    const PluginxxHost*,
    int64_t                  ms,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*
) {
    FakeTimer timer;
    timer.due = std::chrono::steady_clock::now()
              + std::chrono::milliseconds(ms < 0 ? 0 : ms);
    timer.cb = cb;
    timer.ud = ud;
    g_fakeTimers.push_back(timer);
    return reinterpret_cast<PluginxxOperatorHandle*>(uintptr_t{1});
}

/// 执行一次待处理的驱动请求 (推进桥的本地执行器一个有限步骤);
/// 反复调用直到 `false` 表示当前没有可推进的工作。
bool runDriver() {
    bool progressed = false;
    // 先触发已到期的计时器 (主程序 IO 线程上它们先于驱动回调执行)
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < g_fakeTimers.size();) {
        if (g_fakeTimers[i].due <= now) {
            auto entry = g_fakeTimers[i];
            g_fakeTimers.erase(g_fakeTimers.begin() + static_cast<ptrdiff_t>(i));
            if (entry.cb) {
                entry.cb(entry.ud, PLUGINXX_OPERATOR_OK, nullptr);
            }
            progressed = true;
            continue;
        }
        ++i;
    }

    auto* ticket = g_pendingDriver;
    if (!ticket || ticket->done) {
        return progressed;
    }
    g_pendingDriver = nullptr;
    ticket->done    = true;
    if (ticket->drive) {
        ticket->drive(ticket->ud);
    }
    delete ticket;
    return true;
}

/// 在一个私有 io_context 上跑完一个 asio 协程并取结果 (同步用例用)
///
/// 只用于“提交 + 等结果”这类简单场景: 假 host 的登记是内联完成的, 所以
/// `io.run()` 只会把 `asio::post` 回来的续体跑掉, 不会真的阻塞。
template<typename T>
    requires(!std::is_void_v<T>)
T runAwaitable(asio::awaitable<T> awaitable) {
    asio::io_context io;
    T                out{};
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> { out = co_await std::move(awaitable); },
        asio::detached
    );
    io.run();
    return out;
}

/// 挂起一小会儿 (asio steady_timer): 老用例里的手动闸门 [Gate] 的替代品
///
/// 桥的本地执行器由主程序驱动轮次 `poll_one`, timer 到期后下一次驱动就会恢复。
inline asio::awaitable<void>
    awaitTick(std::chrono::milliseconds ms = std::chrono::milliseconds(30)) {
    auto              ex = co_await asio::this_coro::executor;
    asio::steady_timer timer(ex, ms);
    co_await timer.async_wait(asio::use_awaitable);
}

/// 反复推进主程序驱动直到条件成立 (或超时); 返回最终条件
template<typename Pred>
bool pumpUntil(Pred pred, int timeoutMs = 5000) {
    const auto deadline
        = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        (void)runDriver();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
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
    if (name == PLUGINXX_IFACE_CAPABILITIES) {
        return &g_fakeCapabilities;
    }
    if (name == AGENTXX_PLUGIN_IFACE_AGENT_GRAPH) {
        return &g_fakeGraph;
    }
    if (name == PLUGINXX_IFACE_SCHEDULER) {
        return &g_fakeScheduler;
    }
    if (name == AGENTXX_IFACE_CLIENT_UI) {
        return &g_fakeClientUi;
    }
    return nullptr;
}

/// 老主程序模拟: client 侧接口表一律不提供 (query_interface 返回 NULL)
///
/// 用途: 验证 SDK 便捷方法在"主程序不支持"时返回非 0 且不崩 (插件据此降级)
const void* PLUGINXX_CALL
    fakeQueryInterfaceLegacy(const PluginxxHost* host, const PluginxxStringView* iid) {
    if (!iid || !iid->data) {
        return nullptr;
    }
    const std::string_view name{iid->data, static_cast<size_t>(iid->size)};
    if (name == AGENTXX_IFACE_CLIENT_UI || name == AGENTXX_IFACE_CLIENT_SESSION
        || name == AGENTXX_IFACE_CLIENT_WIRE || name == AGENTXX_IFACE_CLIENT_SELF
        || name == AGENTXX_IFACE_CLIENT_TIMER || name == AGENTXX_IFACE_CLIENT_KEYBIND
        || name == PLUGINXX_IFACE_LOG || name == PLUGINXX_IFACE_JSON
        || name == PLUGINXX_IFACE_BROADCASTS) {
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

/// 工具完成通知探针 (工具结果分两段: 正文 + 元数据)
struct ToolNotifyProbe {
    int         calls  = 0;
    int32_t     status = -1;
    std::string result;  ///< 正文
    std::string payload; ///< 元数据 JSON

    static void PLUGINXX_CALL done(
        void*                     ud,
        int32_t                   status,
        const PluginxxStringView* result,
        const PluginxxStringView* payload
    ) {
        auto& probe  = *static_cast<ToolNotifyProbe*>(ud);
        ++probe.calls;
        probe.status = status;
        if (result && result->data && result->size > 0) {
            probe.result.assign(result->data, static_cast<size_t>(result->size));
        }
        if (payload && payload->data && payload->size > 0) {
            probe.payload.assign(payload->data, static_cast<size_t>(payload->size));
        }
    }

    static void PLUGINXX_CALL doneText(
        void*                     ud,
        int32_t                   status,
        const PluginxxStringView* text
    ) {
        auto& probe  = *static_cast<ToolNotifyProbe*>(ud);
        ++probe.calls;
        probe.status = status;
        if (text && text->data && text->size > 0) {
            probe.result.assign(text->data, static_cast<size_t>(text->size));
        }
    }

    static void PLUGINXX_CALL doneFrame(
        void*                     ud,
        int32_t                   status,
        const PluginxxStringView* frame
    ) {
        auto& probe  = *static_cast<ToolNotifyProbe*>(ud);
        ++probe.calls;
        probe.status = status;
        if (frame && frame->data) {
            const auto parts = agentxx::plugin::decodeToolResultFrame(
                std::string_view{frame->data, static_cast<size_t>(frame->size)}
            );
            probe.result.assign(parts.result);
            probe.payload.assign(parts.metadata);
        }
    }

    AgentxxPluginToolNotify notify() {
        // 受控轮询工具用整帧通知器 (两段结果在收尾时拼成帧)
        return AgentxxPluginToolNotify{
            &ToolNotifyProbe::done,
            &ToolNotifyProbe::doneText,
            &ToolNotifyProbe::doneFrame,
            this
        };
    }
};

struct SdkCtx : PluginBase {};

/// 编译期检查: asio 形态的便捷入口能在 `asio::awaitable` 里实例化
///
/// 不执行 (只取地址让模板实例化): 这些入口的返回类型、参数形状、`co_await` 兼容性
/// 都在编译期确定, 跑起来需要真实的主程序表, 所以这里只做类型层面的检查。
asio::awaitable<void> compileCheckAsyncTableHelpers(SdkCtx& ctx) {
    auto send = co_await sendInputAsync(ctx, 1, "{}");
    (void)send.ok();
    auto channelId = co_await openChannelAsync(ctx, ChannelOptions{}, [](const ChannelEvent&) {});
    auto closed    = co_await closeChannelAsync(ctx, channelId);
    (void)closed.accepted();
    auto canceled = co_await cancelChannelAsync(ctx, channelId);
    (void)canceled.ok();
    auto responded = co_await respondInterruptAsync(ctx, channelId, 1, "{}");
    (void)responded.ok();
    auto state = co_await channelStateJsonAsync(ctx, channelId);
    (void)state.text();
    auto list = co_await listRuntimeJsonAsync(ctx);
    (void)list.text();
    AsyncWorkOptions opt;
    auto work = co_await submitAsyncWorkAsync(ctx, opt);
    (void)work.text();
    auto workCanceled = co_await cancelAsyncWorkAsync(ctx, std::string{work.text()});
    (void)workCanceled.ok();
    auto workJson = co_await getAsyncWorkJsonAsync(ctx, "async-1");
    (void)workJson.text();
    auto generic = co_await tableOpAsync(ctx, [](auto, void*, PluginxxString*) {
        return static_cast<PluginxxOperatorHandle*>(nullptr);
    });
    (void)generic.accepted();
    co_return;
}

/// 伪 runtime 表: 只实现 `send_input` (异步完成, 用来验证 asio 形态的恢复路径)
///
/// 完成回调故意在**另一条线程**上、延迟一小会儿再调 —— 与真实主程序一致
/// (表成员受理后在主程序/插件执行线程上回调), 借此验证:
/// - 结果不会在回调线程上继续协程 (必须经 `asio::post` 回到调用方执行器);
/// - 提交发生在调用处, 不等 `co_await`。
struct AsyncSendCapture {
    std::atomic<int>        calls{0};
    std::atomic<bool>       completed{false};
    std::string             lastInput;
    std::thread::id         completedOn{};
};

AsyncSendCapture g_asyncSend;
bool             g_asyncSendDelay = true;

PluginxxOperatorHandle* PLUGINXX_CALL fakeMountRootAccepted(
    const PluginxxHost*,
    const AgentxxPluginMountRootSpec*,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*
) {
    // 本用例不挂根: 只要求"表里存在" (hasRuntime 的判据)
    if (cb) {
        cb(ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return fakeAcceptedHandle();
}

PluginxxOperatorHandle* PLUGINXX_CALL fakeSendInputDelayed(
    const PluginxxHost*,
    int64_t,
    const PluginxxStringView* inputJson,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*
) {
    if (inputJson && inputJson->data) {
        g_asyncSend.lastInput = std::string{inputJson->data, static_cast<size_t>(inputJson->size)};
    }
    ++g_asyncSend.calls;
    // 回调在另一条线程上、稍后触发
    std::thread([cb, ud]() {
        if (g_asyncSendDelay) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        static const char kPayload[] = "sent-ok";
        const PluginxxStringView payload{kPayload, sizeof(kPayload) - 1};
        g_asyncSend.completedOn = std::this_thread::get_id();
        g_asyncSend.completed.store(true, std::memory_order_release);
        if (cb) {
            cb(ud, PLUGINXX_OPERATOR_OK, &payload);
        }
    }).detach();
    return fakeAcceptedHandle();
}

const AgentxxPluginRuntimeIface g_fakeRuntime = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME_VERSION,
    /* struct_size */ sizeof(AgentxxPluginRuntimeIface),
    /* mount_root */ fakeMountRootAccepted,
    /* unmount_root */ nullptr,
    /* open_channel */ nullptr,
    /* close_channel */ nullptr,
    /* send_input */ fakeSendInputDelayed,
    /* cancel */ nullptr,
    /* select_model */ nullptr,
    /* respond_interrupt */ nullptr,
    /* get_channel_state */ nullptr,
    /* get_context_messages */ nullptr,
    /* list_runtime */ nullptr,
    /* register_trigger */ nullptr,
    /* unregister_trigger */ nullptr,
    /* emit_event */ nullptr,
    /* register_timer */ nullptr,
    /* unregister_timer */ nullptr,
    /* submit_async_work */ nullptr,
    /* cancel_async_work */ nullptr,
    /* get_async_work */ nullptr,
    /* list_async_work */ nullptr,
};

const void* PLUGINXX_CALL
    fakeQueryInterfaceWithRuntime(const PluginxxHost*, const PluginxxStringView* iid) {
    if (!iid || !iid->data) {
        return nullptr;
    }
    const std::string_view name{iid->data, static_cast<size_t>(iid->size)};
    if (name == AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME) {
        return &g_fakeRuntime;
    }
    return fakeQueryInterface(nullptr, iid);
}

const PluginxxHostVtable g_runtimeVtable = {
    /* alloc */ fakeAlloc,
    /* free */ fakeFree,
    /* query_interface */ fakeQueryInterfaceWithRuntime,
};

/// asio 形态的表调用: 在插件自有 io 线程上 `co_await`, 完成经 post 回到同一条线程
void runAsyncTableOpCase(TestResult& result) {
    const PluginxxHost host{&g_runtimeVtable, nullptr};
    SdkCtx             ctx;
    ctx.host  = &host;
    ctx.iface = AgentIfaces::query(&host);
    XX_TEST_EXPECT_TRUE(ctx.iface.runtime != nullptr);

    g_asyncSend.calls.store(0);
    g_asyncSend.completed.store(false, std::memory_order_release);
    g_asyncSend.lastInput.clear();
    g_asyncSend.completedOn = std::thread::id{};

    asio::io_context io;
    std::thread::id  ioThread;
    bool             resumedOnIoThread = false;
    std::string      seen;
    bool             unsupportedAccepted = true;
    std::string      unsupportedError;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            ioThread = std::this_thread::get_id();
            auto r   = co_await sendInputAsync(ctx, 42, R"({"text":"hi"})");
            seen                 = std::string{r.text()};
            resumedOnIoThread    = std::this_thread::get_id() == ioThread;
            // 表中没有 `get_channel_state`: 未受理 (不给结果正文)
            auto missing        = co_await channelStateJsonAsync(ctx, 42);
            unsupportedAccepted = missing.accepted();
            unsupportedError    = missing.startError;
        },
        asio::detached
    );
    io.run();

    XX_TEST_EXPECT_TRUE(g_asyncSend.completed.load(std::memory_order_acquire));
    XX_TEST_EXPECT_TRUE(g_asyncSend.calls.load() == 1);
    XX_TEST_EXPECT_TRUE(seen == "sent-ok");
    XX_TEST_EXPECT_TRUE(resumedOnIoThread);
    XX_TEST_EXPECT_TRUE(g_asyncSend.completedOn != ioThread);
    XX_TEST_EXPECT_TRUE(g_asyncSend.lastInput == R"({"text":"hi"})");
    XX_TEST_EXPECT_TRUE(!unsupportedAccepted);
    XX_TEST_EXPECT_TRUE(!unsupportedError.empty());
}

} // namespace

TestResult testPluginSdk() {
    TestResult         result;
    const PluginxxHost host{&g_fakeVtable, nullptr};
    // asio 形态入口的实例化检查 (只取地址, 不执行)
    (void)&compileCheckAsyncTableHelpers;
    // asio 形态表调用: 自有 io 线程上的协程 + 跨线程完成
    runAsyncTableOpCase(result);

    /// tool 根操作的输入由 Request 拥有 —— 主程序借用缓冲区失效后，协程挂起
    /// 恢复继续读取 args 仍必须得到原值。
    {
        bool        finished = false;
        std::string seen;
        std::string seenSession;

        // box 先声明：它必须比 ctx（持有 shim + 协程）活得更久。
        struct Box {
            bool*        finished = nullptr;
            std::string* seen     = nullptr;
            std::string* session  = nullptr;
        } box{&finished, &seen, &seenSession};

        SdkCtx ctx;
        ctx.attachHost(&host);

        g_captured = CapturedRegistration{};
        auto reg   = runAwaitable(toolAsync(
            ctx,
            g_agentRef,
            "sdk_owns_args",
            "depict",
            "{}",
            [&box](SdkCtx&, std::string_view args, OpCtl& ctl) -> asio::awaitable<std::string> {
                // 挂起前记录视图；恢复后（主程序借用缓冲区已失效）再读取。
                auto argsView = args;
                co_await awaitTick(std::chrono::milliseconds(40));
                box.seen->assign(argsView.data(), argsView.size());
                box.session->assign(ctl.scopeKey.data(), ctl.scopeKey.size());
                *box.finished = true;
                co_return *box.seen;
            }
        ));
        XX_TEST_EXPECT_TRUE(reg.ok());
        XX_TEST_EXPECT_TRUE(g_captured.hasTool);

        auto spec = g_captured.tool;
        // 协程体工具走 `execute_start_invoke` (随调用带入会话工作目录)
        XX_TEST_EXPECT_TRUE(spec.execute_start_invoke != nullptr);

        // 主程序借用缓冲区：start 返回后立即被覆写，插件不得再依赖它。
        std::string borrowedArgs = R"({"n":42})";
        std::string borrowedSid  = "sess-42";
        std::string borrowedWork = "/tmp/work";
        auto        argsSv       = PluginStringView::from(borrowedArgs.data(), borrowedArgs.size());
        auto        sidSv        = PluginStringView::from(borrowedSid.data(), borrowedSid.size());
        auto        cidSv        = PluginStringView::fromCstr("call-42");
        auto        workSv       = PluginStringView::from(borrowedWork.data(), borrowedWork.size());

        ToolNotifyProbe probe;
        auto             notify = probe.notify();
        AgentxxPluginToolInvoke invoke{};
        invoke.struct_size   = sizeof(AgentxxPluginToolInvoke);
        invoke.args_json     = &argsSv;
        invoke.session_id    = &sidSv;
        invoke.tool_call_id  = &cidSv;
        invoke.work_dir      = &workSv;
        invoke.notify        = &notify;
        void* op = spec.execute_start_invoke(spec.user_data, &invoke, nullptr);
        XX_TEST_EXPECT_TRUE(op != nullptr);
        XX_TEST_EXPECT_EQ(probe.calls, 0);
        // 首步由 host driver 推进: 跑一次驱动让协程挂到 timer 上
        XX_TEST_EXPECT_TRUE(runDriver());
        // 借用缓冲区失效
        borrowedArgs.assign(borrowedArgs.size(), 'x');
        borrowedSid.assign(borrowedSid.size(), 'y');

        // 计时器到期后由主程序驱动恢复协程, 直到根完成
        XX_TEST_EXPECT_TRUE(pumpUntil([&] { return probe.calls > 0; }));
        XX_TEST_EXPECT_TRUE(finished);
        XX_TEST_EXPECT_EQ(seen, std::string{R"({"n":42})"});
        XX_TEST_EXPECT_EQ(seenSession, std::string{"sess-42"});
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_OK);
        XX_TEST_EXPECT_EQ(probe.result, std::string{R"({"n":42})"});
        // 工具只给正文, 元数据为空
        XX_TEST_EXPECT_TRUE(probe.payload.empty());
    }

    /// providePoint: 功能埋点实现行 = 能力行, 两种业务体形态都能登记
    /// (收上下文+载荷 / 只收载荷), 结果就是实现行给出的回答。
    {
        SdkCtx ctx;
        ctx.attachHost(&host);
        g_captured = CapturedRegistration{};
        auto pt    = runAwaitable(providePointAsync(
            ctx,
            g_agentRef,
            points::ContextCountTokens,
            [](std::string_view payload) -> std::string {
                // 只收载荷的形态; pointAnswer 把裸值包成 `{"value": ...}`
                return pointAnswer(fmt::format(R"({{"echo":{},"priority":5}})", payload));
            },
            PointImplOptions{.priority = 5}
        ));
        XX_TEST_EXPECT_TRUE(pt.ok());
        XX_TEST_EXPECT_TRUE(g_captured.hasCapability);
        {
            NotifyProbe probe;
            auto        notify = probe.notify();
            auto        method = PluginStringView::fromCstr(points::ContextCountTokens.data());
            auto        args   = PluginStringView::fromCstr(R"({"n":2})");
            void* op = g_captured.capStart(g_captured.capUd, nullptr, &method, &args, &notify, nullptr);
            XX_TEST_EXPECT_TRUE(op == nullptr);
            XX_TEST_EXPECT_EQ(probe.calls, 1);
            XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_OK);
            XX_TEST_EXPECT_TRUE(probe.payload.find("\"value\"") != std::string::npos);
            XX_TEST_EXPECT_TRUE(probe.payload.find("\"priority\":5") != std::string::npos);
        }

        // 收上下文+载荷的形态
        g_captured = CapturedRegistration{};
        auto pt2   = runAwaitable(providePointAsync(
            ctx,
            g_agentRef,
            points::ToolSummarizeOutput,
            [](SdkCtx&, std::string_view payload) -> std::string {
                return std::string{payload};
            }
        ));
        XX_TEST_EXPECT_TRUE(pt2.ok());
        XX_TEST_EXPECT_TRUE(g_captured.hasCapability);
        {
            NotifyProbe probe;
            auto        notify = probe.notify();
            auto        method = PluginStringView::fromCstr(points::ToolSummarizeOutput.data());
            auto        args   = PluginStringView::fromCstr(R"({"raw":"x"})");
            void* op = g_captured.capStart(g_captured.capUd, nullptr, &method, &args, &notify, nullptr);
            XX_TEST_EXPECT_TRUE(op == nullptr);
            XX_TEST_EXPECT_EQ(probe.calls, 1);
            XX_TEST_EXPECT_EQ(probe.payload, std::string{R"({"raw":"x"})"});
        }
    }

    /// capability: 同步字符串能力 + 异步 asio 能力 (统一 root adapter)。
    {
        SdkCtx ctx;
        ctx.attachHost(&host);
        g_captured = CapturedRegistration{};
        XX_TEST_EXPECT_TRUE(runAwaitable(capabilityAsync(
            ctx,
            "test.cap.sync",
            [](SdkCtx&, std::string_view method, std::string_view args) -> std::string {
                return fmt::format(R"({{"method":"{}","args":{}}})", method, args);
            }
        )).ok());
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
        bool        finished = false;

        struct AsyncBox {
            bool* finished = nullptr;
        } box{&finished};

        SdkCtx ctx;
        ctx.attachHost(&host);
        g_captured = CapturedRegistration{};
        XX_TEST_EXPECT_TRUE(runAwaitable(capabilityAsync(
            ctx,
            "test.cap.async",
            [&box](SdkCtx&, std::string_view method, std::string_view) -> asio::awaitable<std::string> {
                co_await awaitTick(std::chrono::milliseconds(40));
                *box.finished = true;
                co_return fmt::format("done:{}", method);
            }
        )).ok());
        XX_TEST_EXPECT_TRUE(g_captured.hasCapability);
        XX_TEST_EXPECT_TRUE(g_captured.capCancel != nullptr);

        NotifyProbe probe;
        auto        notify = probe.notify();
        auto        method = PluginStringView::fromCstr("async-ping");
        auto        args   = PluginStringView::fromCstr("{}");
        void* op = g_captured.capStart(g_captured.capUd, nullptr, &method, &args, &notify, nullptr);
        XX_TEST_EXPECT_TRUE(op != nullptr); ///< 未完成 -> provider 句柄
        XX_TEST_EXPECT_EQ(probe.calls, 0);  ///< 不得提前完成
        XX_TEST_EXPECT_TRUE(runDriver());   ///< 首步由 host driver 推进 (挂到 timer 上)
        XX_TEST_EXPECT_FALSE(finished);

        // 借用缓冲区 (method/args 视图) 失效后协程仍能完成 (Request 拥有输入)。
        XX_TEST_EXPECT_TRUE(pumpUntil([&] { return probe.calls > 0; }));
        XX_TEST_EXPECT_TRUE(finished);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_OK);
        XX_TEST_EXPECT_TRUE(probe.payload.find("done:async-ping") != std::string::npos);
    }

    /// graph_node: 快同步节点（返回节点输出 JSON）+ 输入拥有化 + 取消句柄。
    {
        SdkCtx ctx;
        ctx.attachHost(&host);
        g_captured       = CapturedRegistration{};
        auto nodeTicket   = runAwaitable(graphNodeAsync(
            ctx,
            g_agentRef,
            "test.node.sync",
            R"({"type":"object"})",
            [](SdkCtx&, const RootRequest& req) -> std::string {
                return fmt::format(
                    R"({{"node":"{}","thread":"{}","state":{}}})",
                    agentxx::plugin::nodeNameOf(req),
                    req.scope(),
                    agentxx::plugin::nodeStateOf(req)
                );
            }
        ));
        // 登记提交即生效; 假 host 的登记是内联完成的
        XX_TEST_EXPECT_TRUE(nodeTicket.ok());
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
        ctx.attachHost(&host);
        g_captured       = CapturedRegistration{};
        auto nodeTicket   = runAwaitable(graphNodeAsync(
            ctx,
            g_agentRef,
            "test.node.throw",
            "{}",
            [](SdkCtx&, const RootRequest&) -> std::string {
                throw std::runtime_error("node boom");
            }
        ));
        // 登记提交即生效; 假 host 的登记是内联完成的
        XX_TEST_EXPECT_TRUE(nodeTicket.ok());
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

    /// graph_node: 异步 asio 节点 —— provider 句柄 + 借用输入失效后仍完成。
    {
        bool        finished = false;
        std::string seenState;

        struct AsyncBox {
            bool*        finished  = nullptr;
            std::string* seenState = nullptr;
        } box{&finished, &seenState};

        SdkCtx ctx;
        ctx.attachHost(&host);
        g_captured       = CapturedRegistration{};
        auto nodeTicket   = runAwaitable(graphNodeAsync(
            ctx,
            g_agentRef,
            "test.node.async",
            "{}",
            [&box](SdkCtx&, const RootRequest& req, OpCtl ctl) -> asio::awaitable<std::string> {
                std::string node{agentxx::plugin::nodeNameOf(req)};
                std::string state{agentxx::plugin::nodeStateOf(req)};
                co_await awaitTick(std::chrono::milliseconds(40));
                if (ctl.canceled()) {
                    co_return std::string{"Canceled"};
                }
                *box.seenState = state;
                *box.finished  = true;
                co_return fmt::format(R"({{"async":"{}"}})", node);
            }
        ));
        // 登记提交即生效; 假 host 的登记是内联完成的
        XX_TEST_EXPECT_TRUE(nodeTicket.ok());
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
        XX_TEST_EXPECT_TRUE(runDriver()); ///< 首步由 host driver 推进 (挂到 timer 上)
        XX_TEST_EXPECT_FALSE(finished);

        // 主程序借用缓冲区失效后 (置空视图) 协程仍能完成 (Request 拥有输入)
        XX_TEST_EXPECT_TRUE(pumpUntil([&] { return probe.calls > 0; }));
        XX_TEST_EXPECT_TRUE(finished);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_OK);
        XX_TEST_EXPECT_TRUE(probe.payload.find("n2") != std::string::npos);
        XX_TEST_EXPECT_EQ(seenState, std::string{R"({"channels":{}})"});
    }
    // ---- client 侧 SDK 便捷方法: 组件构建器 → 面板/装饰/overlay 提交 ----
    //
    // 用伪 client UI 表捕获提交的 JSON: 覆盖"构建器 → JSON → 接口调用"这条
    // 全流程路径 (含老主程序缺失该表时的降级)。
    {
        ClientPluginBase client;
        client.host  = &host;
        client.iface = ClientIfaces::query(&host);
        XX_TEST_EXPECT_TRUE(client.iface.ui == &g_fakeClientUi);

        // 伪句柄: 伪 update_* 忽略句柄, 只要求非空
        AgentxxPanel*      panel  = reinterpret_cast<AgentxxPanel*>(uintptr_t{1});
        AgentxxStatusItem* status = reinterpret_cast<AgentxxStatusItem*>(uintptr_t{1});

        // 1) 组件项 → setPanelItems
        g_clientUiCapture = ClientUiCapture{};
        {
            std::vector<pluginxx::ui::Item> items;
            pluginxx::ui::Item              table = pluginxx::ui::build::node("Table");
            table.header                          = true;
            table.columns.push_back(pluginxx::ui::TableColumn{
                pluginxx::ui::TextValue::of("Path"),
                "start",
                pluginxx::ui::SizeValue::autoValue(),
                {},
            });
            table.columns.push_back(pluginxx::ui::TableColumn{
                pluginxx::ui::TextValue::of("Scope"),
                "end",
                pluginxx::ui::SizeValue::of(6),
                {},
            });
            table.rows.push_back({
                pluginxx::ui::TableCell{pluginxx::ui::TextValue::of("a.txt"), {}, {}},
                pluginxx::ui::TableCell{pluginxx::ui::TextValue::of("write"), {}, {}},
            });
            items.push_back(std::move(table));
            items.push_back(pluginxx::ui::build::progress(72, 100, "%"));
            XX_TEST_EXPECT_EQ(client.setPanelItems(panel, items), 0);
        }
        XX_TEST_EXPECT_EQ(g_clientUiCapture.panelUpdates, 1);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("\"Table\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("a.txt") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("\"Progress\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("\"items\"") != std::string::npos);

        // 2) panelItems 就地构建器: 作用域结束自动提交
        {
            auto               writer = client.panelItems(panel);
            pluginxx::ui::Item title  = pluginxx::ui::build::text("from-writer");
            title.bold                = true;
            writer.add(std::move(title));
            writer.add(pluginxx::ui::build::control(
                "checkbox",
                "opt",
                pluginxx::ui::TextValue::of("Opt"),
                {},
                "true"
            ));
        }
        XX_TEST_EXPECT_EQ(g_clientUiCapture.panelUpdates, 2);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("from-writer") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("\"bold\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("checkbox") != std::string::npos);

        // 3) 内容块 + 控件 + 提交按钮 (描述层没有 form() 组合器: 由插件自己组合)
        {
            pluginxx::ui::Item block = pluginxx::ui::build::card(
                pluginxx::ui::build::control(
                    "checkbox",
                    "detail",
                    pluginxx::ui::TextValue::of("Detail"),
                    {},
                    "false"
                ),
                "Options"
            );
            pluginxx::ui::Item apply = pluginxx::ui::build::button(
                "APPLY",
                pluginxx::ui::build::call("apply"),
                "normal"
            );
            XX_TEST_EXPECT_EQ(client.setPanelItems(panel, {block, apply}), 0);
        }
        XX_TEST_EXPECT_EQ(g_clientUiCapture.panelUpdates, 3);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("\"Block\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("\"Button\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("APPLY") != std::string::npos);

        // 4) 状态栏 JSON
        XX_TEST_EXPECT_EQ(client.setStatusText(status, "turns: 3"), 0);
        XX_TEST_EXPECT_EQ(g_clientUiCapture.statusUpdates, 1);
        XX_TEST_EXPECT_TRUE(g_clientUiCapture.statusJson.find("turns: 3") != std::string::npos);

        // 5) 工具消息装饰: DecorSpec → update_tool_decor; 清除 = 空 JSON
        {
            ClientPluginBase::DecorSpec spec;
            spec.displayName             = "Plan";
            spec.summary                 = "[~] a; [ ] b";
            pluginxx::ui::Item stepTable = pluginxx::ui::build::node("Table");
            stepTable.header             = true;
            stepTable.columns.push_back(pluginxx::ui::TableColumn{
                pluginxx::ui::TextValue::of("Step"),
                "start",
                pluginxx::ui::SizeValue::autoValue(),
                {},
            });
            for (const auto& step : {"a", "b"}) {
                stepTable.rows.push_back({
                    pluginxx::ui::TableCell{pluginxx::ui::TextValue::of(step), {}, {}}
                });
            }
            spec.items.push_back(std::move(stepTable));
            XX_TEST_EXPECT_EQ(client.setToolDecor("call_1", spec), 0);
            XX_TEST_EXPECT_EQ(g_clientUiCapture.decorUpdates, 1);
            XX_TEST_EXPECT_EQ(g_clientUiCapture.decorToolCallId, std::string{"call_1"});
            XX_TEST_EXPECT_TRUE(
                g_clientUiCapture.decorJson.find("\"displayName\":\"Plan\"") != std::string::npos
            );
            XX_TEST_EXPECT_TRUE(g_clientUiCapture.decorJson.find("\"Table\"") != std::string::npos);

            XX_TEST_EXPECT_EQ(client.clearToolDecor("call_1"), 0);
            XX_TEST_EXPECT_EQ(g_clientUiCapture.decorUpdates, 2);
            XX_TEST_EXPECT_TRUE(g_clientUiCapture.decorJson.empty());
        }

        // 6) overlay: 组件树作为 payload + 尺寸选项原样转发
        {
            g_clientUiCapture               = ClientUiCapture{};
            pluginxx::ui::Item overlayTable = pluginxx::ui::build::node("Table");
            overlayTable.header             = true;
            overlayTable.columns.push_back(pluginxx::ui::TableColumn{
                pluginxx::ui::TextValue::of("P"),
                "start",
                pluginxx::ui::SizeValue::autoValue(),
                {},
            });
            overlayTable.rows.push_back({
                pluginxx::ui::TableCell{pluginxx::ui::TextValue::of("x"), {}, {}}
            });
            XX_TEST_EXPECT_EQ(
                client.showItemsOverlay("Files", {std::move(overlayTable)}, "{\"size\":\"large\"}"),
                0
            );
            XX_TEST_EXPECT_EQ(g_clientUiCapture.overlays, 1);
            XX_TEST_EXPECT_TRUE(
                g_clientUiCapture.overlayPayload.find("\"items\"") != std::string::npos
            );
            XX_TEST_EXPECT_EQ(g_clientUiCapture.overlayExtra, std::string{"{\"size\":\"large\"}"});
        }

        // 7) 未知 kind 原样转发 (老主程序忽略, 数据层向前兼容)
        {
            pluginxx::ui::Item future = pluginxx::ui::build::node("future_widget");
            future.fallback           = "n/a";
            XX_TEST_EXPECT_EQ(client.setPanelItems(panel, {std::move(future)}), 0);
            XX_TEST_EXPECT_TRUE(
                g_clientUiCapture.panelItems.find("future_widget") != std::string::npos
            );
            XX_TEST_EXPECT_TRUE(g_clientUiCapture.panelItems.find("n/a") != std::string::npos);
        }
    }

    // ---- 老主程序降级: client UI 表缺失 → 便捷方法返回非 0, 不崩 ----
    {
        const PluginxxHost legacyHost{&g_legacyVtable, nullptr};
        ClientPluginBase   client;
        client.host  = &legacyHost;
        client.iface = ClientIfaces::query(&legacyHost);
        XX_TEST_EXPECT_TRUE(client.iface.ui == nullptr);

        AgentxxPanel*                   panel = reinterpret_cast<AgentxxPanel*>(uintptr_t{1});
        std::vector<pluginxx::ui::Item> items{pluginxx::ui::build::text("x")};
        XX_TEST_EXPECT_TRUE(client.setPanelItems(panel, items) != 0);
        XX_TEST_EXPECT_TRUE(client.setToolDecor("call_1", {}) != 0);
        XX_TEST_EXPECT_TRUE(client.showItemsOverlay("t", items) != 0);
        XX_TEST_EXPECT_FALSE(client.hostSupports("agentxx.client.components"));
        // 未刷新过快照时快照是空对象, 区域尺寸查询给 0 (改成协程形态后这里只验证降级路径)
        XX_TEST_EXPECT_TRUE(client.clientStateSnapshot().is_object());
        XX_TEST_EXPECT_TRUE(client.uiCapabilities().supportsBlock("Text"));
    }

    // ---- ArgReader: 字符串转数值/布尔的严格解析 ----
    // - 整数/浮点: **整个**字符串都必须是数值, 不再取前缀 (`"100 lines"` 读不到)
    // - 布尔: 忽略大小写, 认 true/yes/y/on/enable/enabled/1 与
    //   false/no/n/off/disable/disabled/0, 其它文本读不到(而不是静默 false)
    // - 整数还要落在目标类型范围内, 越界不做回绕转换
    // - JSON 里的浮点数转整数取“相近整数” (与向上取整差不到 1e-6 就向上, 否则向下)
    {
        ArgReader args(R"({
            "n": " 42 ",
            "plus": "+7",
            "big": "999999999999999999999",
            "prefix": "100 lines",
            "fracText": "3.7",
            "hex": "0x10",
            "f": "1.5 kg",
            "fv": "1.5",
            "flagTrue": "True",
            "flagZero": "0",
            "flagYes": "Yes",
            "flagOn": " ON ",
            "flagNo": "no",
            "flagOff": "disabled",
            "flagWord": "ok",
            "small": "9999999999",
            "roundUp": 4.999999999,
            "roundDown": 3.7,
            "roundNeg": -3.5,
            "wideInt": 3000000000
        })");
        XX_TEST_EXPECT_FALSE(args.hasParseError());

        // 整数: 首尾空白/前导 '+' 可忽略, 其余必须完整
        XX_TEST_EXPECT_EQ(args.get<long long>("n").value_or(0), 42LL);
        XX_TEST_EXPECT_EQ(args.get<long long>("plus").value_or(0), 7LL);
        XX_TEST_EXPECT_FALSE(args.get<long long>("prefix").has_value());
        XX_TEST_EXPECT_FALSE(args.get<long long>("fracText").has_value());
        XX_TEST_EXPECT_FALSE(args.get<long long>("big").has_value());
        XX_TEST_EXPECT_FALSE(args.get<int>("small").has_value()); // 超出 int 范围

        // 浮点: 同样要求完整, 且拒绝十六进制浮点写法
        XX_TEST_EXPECT_TRUE(args.get<double>("fv").value_or(0.0) == 1.5);
        XX_TEST_EXPECT_FALSE(args.get<double>("f").has_value());
        XX_TEST_EXPECT_FALSE(args.get<double>("hex").has_value());

        // 布尔: 忽略大小写, 常见肯定/否定词都认; 无法判断的词读不到
        XX_TEST_EXPECT_TRUE(args.get<bool>("flagTrue").value_or(false));
        XX_TEST_EXPECT_FALSE(args.get<bool>("flagZero").value_or(true));
        XX_TEST_EXPECT_TRUE(args.get<bool>("flagYes").value_or(false));
        XX_TEST_EXPECT_TRUE(args.get<bool>("flagOn").value_or(false));
        XX_TEST_EXPECT_FALSE(args.get<bool>("flagNo").value_or(true));
        XX_TEST_EXPECT_FALSE(args.get<bool>("flagOff").value_or(true));
        XX_TEST_EXPECT_FALSE(args.get<bool>("flagWord").has_value());

        // JSON 浮点数转整数: 取“相近整数”, 越 int 范围读不到
        XX_TEST_EXPECT_EQ(args.get<long long>("roundUp").value_or(0), 5LL);
        XX_TEST_EXPECT_EQ(args.get<long long>("roundDown").value_or(0), 3LL);
        XX_TEST_EXPECT_EQ(args.get<long long>("roundNeg").value_or(0), -4LL);
        XX_TEST_EXPECT_FALSE(args.get<int>("wideInt").has_value());

        // 缺省值/错误汇报语义不变
        XX_TEST_EXPECT_EQ(args.value<long long>("prefix", -1), -1LL);
        XX_TEST_EXPECT_TRUE(args.ok()); // 只是取用, 没调 require 不产生错误

        ArgReader required(R"({"n":"100 lines"})");
        XX_TEST_EXPECT_EQ(required.require<long long>("n"), 0LL);
        XX_TEST_EXPECT_FALSE(required.ok());
        XX_TEST_EXPECT_TRUE(required.errorMessage().find("'n'") != std::string::npos);
    }

    return result;
}
} // namespace agentxx::test
