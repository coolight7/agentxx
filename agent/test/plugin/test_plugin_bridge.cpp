/// 插件协程驱动桥测试（设计文档第 10 节的 kit 侧完成条件）。
///
/// 测试形态: 用**伪宿主**直接实现 `agentxx.agent.coroutine_runtime` 表：
/// - `request_driver` 只把回调入队并返回请求 (永不内联), 由用例在"宿主 IO 线程"
///   上按需调用 [Harness::runOne], 从而精确观察"一次请求 = 一次 poll_one";
/// - `cancel_driver` 记录取消 (只对尚未执行的请求生效, 与宿主实现语义一致);
/// - `is_io_thread` 恒返回 1 (用例本身就在模拟宿主 IO 线程)。
///
/// 因此用例可以断言真实实现里很难观察的不变量: 不内联、恰好一次推进、空闲不新增
/// 请求、wake 三个窗口不丢失、宿主回调不重入插件协程、取消只产生一个终态。
#include "test_plugin_bridge.h"

#include "agentxx/plugin/api/plugin_kit.h"

#include "asio/awaitable.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define XX_TEST_PASSED result.passed
#define XX_TEST_FAILED result.failed

namespace agentxx::test {
namespace {
using namespace agentxx::plugin;

/// 请求句柄: 对插件侧只是不透明指针 (`AgentxxPluginDriver` 为不完整类型),
/// 伪宿主用自建 token 表示它, 并记录回调/取消/执行状态。
struct Harness;
struct FakeOp;

struct FakeTicket {
    AgentxxPluginDriveOnceFn drive     = nullptr;
    void*                    ud        = nullptr;
    Harness*                 harness   = nullptr;
    bool                     executed  = false;
    bool                     cancelled = false;
};

/// 伪宿主驱动 + 调度设施。所有调用都发生在用例线程上 (模拟宿主 IO 线程)。
struct Harness {
    /// 伪调度器 sleep 的挂起点 (用例手工触发到期, 模拟宿主 timer adapter)。
    struct PendingSleep {
        int64_t                       ms     = 0;
        AgentxxPluginOperatorCallback cb     = nullptr;
        void*                         ud     = nullptr;
        uintptr_t                     handle = 0;
        /// 已被 op_cancel 请求取消。真宿主会在取消后以 CANCELLED 触发完成回调,
        /// 伪宿主只做标记, 由用例决定何时触发 (保持对既有用例的兼容)。
        bool cancelled = false;
    };

    std::deque<FakeTicket*> pending; ///< 已申请、尚未执行的请求 (FIFO = 宿主任务队列)
    std::vector<FakeTicket*>             all;
    std::vector<PendingSleep>            sleeps;
    std::vector<std::unique_ptr<FakeOp>> ops;  ///< 已发放的伪句柄记录
    std::vector<std::string>             logs; ///< 经宿主日志接口表收到的诊断
    int                                  refuseCount = 0; ///< >0 时 request_driver 直接失败
    int                                  cancelCalls = 0;
    int                                  isIoThreadCalls = 0;
    int                                  opCancelCalls = 0; ///< scheduler op_cancel 被调用次数
    int                                  offloadCalls  = 0; ///< scheduler offload 被调用次数
    uintptr_t                            nextSleepHandle = 1;

    size_t queuedTicketCount() const {
        return pending.size();
    }

    /// 执行"下一次请求" (等价于宿主把一次投递排到 IO 线程并执行它)。
    /// `return`: 是否真的执行了一次请求
    bool runOne() {
        if (pending.empty()) {
            return false;
        }
        FakeTicket* ticket = pending.front();
        pending.pop_front();
        ticket->executed = true;
        ticket->drive(ticket->ud);
        return true;
    }

    /// 把所有当前已排队的请求跑完 (每张一次; 请求回调可能又申请新请求)。
    int runPending(int limit = 64) {
        int ran = 0;
        while (ran < limit && runOne()) {
            ++ran;
        }
        return ran;
    }

    void clear() {
        pending.clear();
        for (auto* ticket : all) {
            delete ticket;
        }
        all.clear();
        sleeps.clear();
        ops.clear();
        logs.clear();
        refuseCount     = 0;
        cancelCalls     = 0;
        isIoThreadCalls = 0;
        opCancelCalls   = 0;
        offloadCalls    = 0;
        nextSleepHandle = 1;
    }
};

/// 伪调度器句柄记录 (sleep/offload 返回的不透明句柄):
/// 句柄本身没有 host 参数, 因此记录里带上所属 harness 以便 op_cancel 找到挂起点。
struct FakeOp {
    Harness*  harness = nullptr;
    uintptr_t handle  = 0;
};

Harness* harnessOf(const AgentxxPluginHost* host) {
    return host ? static_cast<Harness*>(host->opaque) : nullptr;
}

/// 伪宿主的错误输出: 测试进程内用 malloc 即可 (真实宿主经 host->vtable->alloc)。
void setHostError(AgentxxPluginString* out, std::string_view message) {
    if (!out) {
        return;
    }
    out->data = static_cast<char*>(std::malloc(message.size() + 1));
    if (out->data) {
        if (!message.empty()) {
            std::memcpy(out->data, message.data(), message.size());
        }
        out->data[message.size()] = '\0';
        out->size                 = message.size();
    }
}

AgentxxPluginDriver* AGENTXX_PLUGIN_CALL fakeRequestDriver(
    const AgentxxPluginHost* host,
    AgentxxPluginDriveOnceFn drive,
    void*                    ud,
    AgentxxPluginString*     error
) {
    auto* harness = harnessOf(host);
    if (!harness || !drive) {
        setHostError(error, "fake host: no harness");
        return nullptr;
    }
    if (harness->refuseCount > 0) {
        setHostError(error, "fake host: driver refused");
        return nullptr;
    }
    auto* ticket    = new FakeTicket();
    ticket->drive   = drive;
    ticket->ud      = ud;
    ticket->harness = harness;
    harness->all.push_back(ticket);
    harness->pending.push_back(ticket);
    return reinterpret_cast<AgentxxPluginDriver*>(ticket);
}

void AGENTXX_PLUGIN_CALL fakeCancelDriver(AgentxxPluginDriver* driver) {
    auto* ticket = reinterpret_cast<FakeTicket*>(driver);
    if (!ticket) {
        return;
    }
    if (ticket->harness) {
        ++ticket->harness->cancelCalls;
    }
    // 与宿主实现一致: 取消只对尚未执行的请求生效, 执行中的不打断。
    if (!ticket->executed) {
        ticket->cancelled = true;
    }
}

int32_t AGENTXX_PLUGIN_CALL fakeIsIoThread(const AgentxxPluginHost* host) {
    if (auto* harness = harnessOf(host)) {
        ++harness->isIoThreadCalls;
    }
    return 1;
}

/// 伪 sleep: 记录回调, 由用例触发到期 (真宿主 timer adapter 的等价物)。
AgentxxPluginOperatorHandle* AGENTXX_PLUGIN_CALL fakeSleep(
    const AgentxxPluginHost*      host,
    int64_t                       ms,
    AgentxxPluginOperatorCallback cb,
    void*                         ud,
    AgentxxPluginString*          error
) {
    auto* harness = harnessOf(host);
    if (!harness || !cb) {
        setHostError(error, "fake scheduler: bad args");
        return nullptr;
    }
    Harness::PendingSleep entry;
    entry.ms     = ms;
    entry.cb     = cb;
    entry.ud     = ud;
    entry.handle = harness->nextSleepHandle++;
    harness->sleeps.push_back(entry);
    auto record     = std::make_unique<FakeOp>();
    record->harness = harness;
    record->handle  = entry.handle;
    auto* raw       = record.get();
    harness->ops.push_back(std::move(record));
    return reinterpret_cast<AgentxxPluginOperatorHandle*>(raw);
}

/// 伪 op_cancel: 只标记句柄对应的挂起点为"已取消" (真宿主随后会以 CANCELLED
/// 触发完成回调; 用例按需手工触发, 以免影响既有用例的手工时序)。
/// 句柄不含 host 参数, 因此伪宿主把 (harness, handle) 记录在堆对象里。
void AGENTXX_PLUGIN_CALL fakeOpCancel(AgentxxPluginOperatorHandle* op) {
    auto* record = reinterpret_cast<FakeOp*>(op);
    if (!record || !record->harness) {
        return;
    }
    auto* harness = record->harness;
    for (auto& entry : harness->sleeps) {
        if (entry.handle == record->handle) {
            ++harness->opCancelCalls;
            entry.cancelled = true;
            return;
        }
    }
}

int32_t AGENTXX_PLUGIN_CALL
    fakeIsCancelled(const AgentxxPluginHost*, const AgentxxPluginStringView*) {
    return 0;
}

int32_t AGENTXX_PLUGIN_CALL fakeOffloadIsCancelled(const AgentxxPluginCancelToken*) {
    return 0;
}

/// 伪 offload: 用例线程内联执行工作体后触发完成回调。
/// (真宿主在工作线程池执行; 这里只需验证"降级路径能跑完"的语义, 不引入线程。)
AgentxxPluginOperatorHandle* AGENTXX_PLUGIN_CALL fakeOffload(
    const AgentxxPluginHost* host,
    void*(AGENTXX_PLUGIN_CALL* work)(
        void*                           ud,
        const AgentxxPluginCancelToken* token,
        AgentxxPluginString*            error_out
    ),
    void(AGENTXX_PLUGIN_CALL*
             done)(void* ud, int32_t status, void* result, const AgentxxPluginStringView* error),
    void*                ud,
    AgentxxPluginString* error
) {
    auto* harness = harnessOf(host);
    if (!harness || !work || !done) {
        setHostError(error, "fake scheduler offload: bad args");
        return nullptr;
    }
    ++harness->offloadCalls;
    AgentxxPluginCancelToken token{&fakeOffloadIsCancelled, nullptr};
    AgentxxPluginString      workError{nullptr, 0};
    void*                    result = work(ud, &token, &workError);
    PluginString::free(host, &workError);
    AgentxxPluginStringView payload = PluginStringView::from(nullptr, 0);
    done(ud, AGENTXX_PLUGIN_OPERATOR_OK, result, &payload);
    auto record     = std::make_unique<FakeOp>();
    record->harness = harness;
    record->handle  = harness->nextSleepHandle++;
    auto* raw       = record.get();
    harness->ops.push_back(std::move(record));
    return reinterpret_cast<AgentxxPluginOperatorHandle*>(raw);
}

void AGENTXX_PLUGIN_CALL
    fakeLog(const AgentxxPluginHost* host, int32_t, const AgentxxPluginStringView* msg) {
    auto* harness = harnessOf(host);
    if (harness && msg && msg->data) {
        harness->logs.emplace_back(msg->data, static_cast<size_t>(msg->size));
    }
}

int32_t AGENTXX_PLUGIN_CALL
    fakeRegisterTool(const AgentxxPluginHost*, const AgentxxPluginToolSpec* spec);

const AgentxxPluginCoroutineRuntimeIface g_fakeRuntime = {
    /* version */ AGENTXX_PLUGIN_IFACE_COROUTINE_RUNTIME_VERSION,
    /* struct_size */ sizeof(AgentxxPluginCoroutineRuntimeIface),
    /* request_driver */ fakeRequestDriver,
    /* cancel_driver */ fakeCancelDriver,
    /* is_io_thread */ fakeIsIoThread,
};

const AgentxxPluginSchedulerIface g_fakeScheduler = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER_VERSION,
    /* struct_size */ sizeof(AgentxxPluginSchedulerIface),
    /* is_io_thread */ fakeIsIoThread,
    /* post_to_io */ nullptr,
    /* sleep */ fakeSleep,
    /* op_cancel */ fakeOpCancel,
    /* offload */ fakeOffload,
};

const AgentxxPluginCancelIface g_fakeCancel = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_CANCEL_VERSION,
    /* struct_size */ sizeof(AgentxxPluginCancelIface),
    /* is_cancelled */ fakeIsCancelled,
};

const AgentxxPluginLogIface g_fakeLog = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_LOG_VERSION,
    /* struct_size */ sizeof(AgentxxPluginLogIface),
    /* log */ fakeLog,
};

const AgentxxPluginToolsIface g_fakeTools = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION,
    /* struct_size */ sizeof(AgentxxPluginToolsIface),
    /* register_tool */ fakeRegisterTool,
    /* unregister_tool */ nullptr,
    /* call_tool_async */ nullptr,
    /* op_cancel */ nullptr,
};

/// 用例可临时置空来验证"宿主不提供驱动时"的回退路径。
const AgentxxPluginCoroutineRuntimeIface* g_runtimeForTest = &g_fakeRuntime;

const void* AGENTXX_PLUGIN_CALL
    fakeQueryInterface(const AgentxxPluginHost*, const AgentxxPluginStringView* iid) {
    if (!iid || !iid->data) {
        return nullptr;
    }
    const std::string_view name{iid->data, static_cast<size_t>(iid->size)};
    if (name == AGENTXX_PLUGIN_IFACE_COROUTINE_RUNTIME) {
        return g_runtimeForTest;
    }
    if (name == AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER) {
        return &g_fakeScheduler;
    }
    if (name == AGENTXX_PLUGIN_IFACE_AGENT_CANCEL) {
        return &g_fakeCancel;
    }
    if (name == AGENTXX_PLUGIN_IFACE_AGENT_LOG) {
        return &g_fakeLog;
    }
    if (name == AGENTXX_PLUGIN_IFACE_AGENT_TOOLS) {
        return &g_fakeTools;
    }
    return nullptr;
}

/// 伪宿主内存操作: kit 会用 host->vtable->free 释放 request_driver 的
/// error_out (缺失该函数会导致测试进程真的泄漏, 不是被测代码的问题)。
void* AGENTXX_PLUGIN_CALL fakeAlloc(uint64_t size) {
    return std::malloc(static_cast<size_t>(size));
}

void AGENTXX_PLUGIN_CALL fakeFree(void* ptr) {
    std::free(ptr);
}

const AgentxxHostVtable g_fakeVtable = {
    /* alloc */ fakeAlloc,
    /* free */ fakeFree,
    /* query_interface */ fakeQueryInterface,
};

/// 工具注册捕获 (与 test_plugin_sdk 一致的伪注册表)。
struct CapturedTool {
    AgentxxPluginToolSpec spec{};
    bool                  has = false;
};

CapturedTool g_capturedTool;

int32_t AGENTXX_PLUGIN_CALL
    fakeRegisterTool(const AgentxxPluginHost*, const AgentxxPluginToolSpec* spec) {
    if (!spec) {
        return -1;
    }
    g_capturedTool.spec = *spec;
    g_capturedTool.has  = true;
    return 0;
}

/// 完成通知探针。
struct NotifyProbe {
    int         calls  = 0;
    int32_t     status = -1;
    std::string payload;

    static void AGENTXX_PLUGIN_CALL
        done(void* ud, int32_t status, const AgentxxPluginStringView* payload) {
        auto& probe = *static_cast<NotifyProbe*>(ud);
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

struct BridgeCtx : PluginBase {};

/// 通过桥放行的挂起点: 模拟"外部完成回调到达"。
///
/// 它与 kit 内部 awaiter 的恢复方式完全一致: 调用 [detail::resumePluginCoroutine]
/// (先 post 到本地执行器再 wake), 因此 **绝不从回调栈内恢复协程**, continuation
/// 只由下一次 host driver 的 poll_one 执行。
template<typename Promise>
struct BridgeGate {
    detail::PollOneBridge* bridge   = nullptr;
    void*                  addr     = nullptr;
    bool                   released = false;

    bool await_ready() const noexcept {
        return released;
    }

    void await_suspend(std::coroutine_handle<Promise> h) noexcept {
        addr = h.address();
    }

    void await_resume() const noexcept {}

    /// 外部完成: 只投递 + 唤醒 (不恢复协程)。
    void release() {
        released = true;
        if (!bridge || !addr) {
            return;
        }
        auto handle = std::coroutine_handle<Promise>::from_address(addr);
        detail::resumePluginCoroutine(bridge, handle);
    }
};

/// 启动一个工具根操作并返回 provider 句柄 (用例统一入口)。
void* startTool(
    const AgentxxPluginToolSpec& spec,
    NotifyProbe&                 probe,
    const char*                  argsJson = "{}"
) {
    auto notify = probe.notify();
    auto args   = PluginStringView::fromCstr(argsJson);
    auto sid    = PluginStringView::fromCstr("session-1");
    auto cid    = PluginStringView::fromCstr("call-1");
    return spec.execute_start(spec.user_data, &args, &sid, &cid, &notify, nullptr);
}

} // namespace

TestResult testPluginBridge() {
    TestResult              result;
    Harness                 harness;
    const AgentxxPluginHost host{&g_fakeVtable, &harness};

    /// 1. request_driver 永不内联 + 一次请求恰好一次推进 + 空闲不自旋。
    {
        harness.clear();
        BridgeCtx ctx;
        ctx.init(&host);
        g_capturedTool = CapturedTool{};

        int steps = 0;
        agentxx::plugin::tool(
            ctx,
            "bridge_once",
            "depict",
            "{}",
            [&steps](BridgeCtx&, std::string_view, OpCtl&) -> Task<std::string> {
                ++steps;
                co_return "ok";
            }
        );
        XX_TEST_EXPECT_TRUE(g_capturedTool.has);

        auto        spec = g_capturedTool.spec;
        NotifyProbe probe;
        void*       op = startTool(spec, probe);
        XX_TEST_EXPECT_TRUE(op != nullptr);
        // 不内联: start 返回时插件协程尚未运行, 完成通知尚未发出
        XX_TEST_EXPECT_EQ(steps, 0);
        XX_TEST_EXPECT_EQ(probe.calls, 0);
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{1});

        auto* bridge = &ctx.bridge();
        XX_TEST_EXPECT_EQ(bridge->ticketsIssued(), uint64_t{1});
        XX_TEST_EXPECT_EQ(bridge->driverSteps(), uint64_t{0});
        // 请求已登记但尚未执行; 首步仍挂在本地执行器上 (待推进步骤数 = 1)
        XX_TEST_EXPECT_TRUE(bridge->isDriverQueued());
        XX_TEST_EXPECT_FALSE(bridge->isDriverRunning());
        XX_TEST_EXPECT_EQ(bridge->readySteps(), uint64_t{1});

        // 一次请求 = 一次 poll_one = 一次根推进
        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_EQ(steps, 1);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_OK);
        XX_TEST_EXPECT_EQ(probe.payload, std::string{"ok"});
        XX_TEST_EXPECT_EQ(bridge->driverSteps(), uint64_t{1});
        XX_TEST_EXPECT_FALSE(bridge->isDriverQueued()); // 请求已消费, 无新工作
        XX_TEST_EXPECT_EQ(bridge->readySteps(), uint64_t{0});

        // 空闲不自旋: 无新工作就不再申请请求
        const auto ticketsAfter = bridge->ticketsIssued();
        XX_TEST_EXPECT_EQ(harness.runPending(), 0);
        XX_TEST_EXPECT_EQ(bridge->ticketsIssued(), ticketsAfter);
        XX_TEST_EXPECT_EQ(bridge->activeRootCount(), size_t{0});
    }

    /// 2. 宿主回调不重入插件协程 (post + wake), continuation 由下一次请求恢复。
    {
        harness.clear();
        NotifyProbe probe; // 必须比 ctx 声明更早: ctx 析构时桥会终结残留根
        BridgeCtx   ctx;
        ctx.init(&host);
        g_capturedTool = CapturedTool{};

        bool                                        resumed = false;
        BridgeGate<Task<std::string>::promise_type> gate;

        agentxx::plugin::tool(
            ctx,
            "bridge_callbacks",
            "depict",
            "{}",
            [&](BridgeCtx& c, std::string_view, OpCtl&) -> Task<std::string> {
                gate.bridge = &c.bridge();
                co_await gate;
                resumed = true;
                co_return "woke";
            }
        );

        auto  spec = g_capturedTool.spec;
        void* op   = startTool(spec, probe);
        XX_TEST_EXPECT_TRUE(op != nullptr);

        auto* bridge = &ctx.bridge();
        XX_TEST_EXPECT_TRUE(harness.runOne()); // 首步: 协程挂到 gate 上
        XX_TEST_EXPECT_EQ(probe.calls, 0);
        XX_TEST_EXPECT_FALSE(bridge->hasPendingWake());
        XX_TEST_EXPECT_EQ(bridge->activeRootCount(), size_t{1});

        const auto ticketsBefore = bridge->ticketsIssued();
        gate.release();                // 外部完成到达 (driver 已返回: window 3)
        XX_TEST_EXPECT_FALSE(resumed); // 未在回调栈内重入插件协程
        XX_TEST_EXPECT_EQ(probe.calls, 0);
        XX_TEST_EXPECT_EQ(bridge->ticketsIssued(), ticketsBefore + 1);

        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_TRUE(resumed);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.payload, std::string{"woke"});
        XX_TEST_EXPECT_EQ(harness.runPending(), 0); // 完成后无新工作
    }

    /// 3. wake 在"driver 执行中"窗口: 本步执行期间出现新工作, 请求收尾必须补票。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.init(&host);
        g_capturedTool = CapturedTool{};

        bool wokeDuringDriver = false;
        agentxx::plugin::tool(
            ctx,
            "bridge_wake_during",
            "depict",
            "{}",
            [&wokeDuringDriver](BridgeCtx& c, std::string_view, OpCtl&) -> Task<std::string> {
                auto* bridge = &c.bridge();
                // 模拟"执行本步时外部完成到达": 投递新工作并唤醒。
                // 此刻请求仍在执行中 (driverRunning), wake 只能记为 wakePending_,
                // 由本轮回调收尾时补票 —— 这正是"窗口 2 不丢唤醒"的可观察形式。
                bridge->postToLocal([] {});
                bridge->wake();
                wokeDuringDriver = true;
                co_return "during";
            }
        );

        auto  spec = g_capturedTool.spec;
        void* op   = startTool(spec, probe);
        XX_TEST_EXPECT_TRUE(op != nullptr);

        auto* bridge = &ctx.bridge();
        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_TRUE(wokeDuringDriver);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(bridge->ticketsIssued(), uint64_t{2}); // 收尾补票
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{1});

        XX_TEST_EXPECT_TRUE(harness.runOne());      // 消化遗留的本地任务
        XX_TEST_EXPECT_EQ(harness.runPending(), 0); // 之后不再补票
        XX_TEST_EXPECT_EQ(bridge->driverSteps(), uint64_t{2});
    }

    /// 4. wake 在"driver 前"窗口: 请求已排队时到达的唤醒被合并, 且不会被丢掉
    ///    (driver 收尾补一次请求消化它)。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.init(&host);
        g_capturedTool = CapturedTool{};

        int steps = 0;
        agentxx::plugin::tool(
            ctx,
            "bridge_wake_before",
            "depict",
            "{}",
            [&steps](BridgeCtx&, std::string_view, OpCtl&) -> Task<std::string> {
                ++steps;
                co_return "before";
            }
        );

        auto  spec = g_capturedTool.spec;
        void* op   = startTool(spec, probe);
        XX_TEST_EXPECT_TRUE(op != nullptr);

        auto* bridge = &ctx.bridge();
        XX_TEST_EXPECT_EQ(bridge->ticketsIssued(), uint64_t{1});

        // 首步请求仍在排队时, 外部完成到达: wake 被合并 (不新增请求), 但不丢失
        bridge->postToLocal([] {});
        bridge->wake();
        XX_TEST_EXPECT_EQ(bridge->ticketsIssued(), uint64_t{1});
        XX_TEST_EXPECT_TRUE(bridge->hasPendingWake());

        // 首步执行 -> 完成; 收尾补票消化被合并的唤醒
        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_EQ(steps, 1);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.payload, std::string{"before"});
        XX_TEST_EXPECT_EQ(bridge->ticketsIssued(), uint64_t{2});
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{1});

        XX_TEST_EXPECT_TRUE(harness.runOne());      // 消化遗留唤醒
        XX_TEST_EXPECT_EQ(harness.runPending(), 0); // 之后无新工作 -> 不再补票
        XX_TEST_EXPECT_EQ(steps, 1);
    }

    /// 5. 宿主 sleep 适配 (timer adapter): 到期回调不重入插件协程, 由其后的请求恢复。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.init(&host);
        g_capturedTool = CapturedTool{};

        bool resumed = false;
        agentxx::plugin::tool(
            ctx,
            "bridge_sleep",
            "depict",
            "{}",
            [&resumed](BridgeCtx& c, std::string_view, OpCtl&) -> Task<std::string> {
                co_await sleep(c, 25);
                resumed = true;
                co_return "slept";
            }
        );

        auto  spec = g_capturedTool.spec;
        void* op   = startTool(spec, probe);
        XX_TEST_EXPECT_TRUE(op != nullptr);

        auto* bridge = &ctx.bridge();
        XX_TEST_EXPECT_TRUE(harness.runOne()); // 首步 -> 注册 sleep 并挂起
        XX_TEST_EXPECT_EQ(harness.sleeps.size(), size_t{1});
        XX_TEST_EXPECT_EQ(harness.sleeps.front().ms, int64_t{25});
        XX_TEST_EXPECT_EQ(probe.calls, 0);
        XX_TEST_EXPECT_FALSE(resumed);

        const auto ticketsBefore = bridge->ticketsIssued();
        auto&      sleepEntry    = harness.sleeps.front();
        sleepEntry.cb(sleepEntry.ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
        XX_TEST_EXPECT_FALSE(resumed); // 未在宿主回调栈内重入
        XX_TEST_EXPECT_EQ(probe.calls, 0);
        XX_TEST_EXPECT_EQ(bridge->ticketsIssued(), ticketsBefore + 1);

        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_TRUE(resumed);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.payload, std::string{"slept"});
        XX_TEST_EXPECT_EQ(harness.runPending(), 0);
    }

    /// 6. 取消: 只产生一个终态 (CANCELLED); 重复取消与迟到完成不产生第二个终态。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.init(&host);
        g_capturedTool = CapturedTool{};

        agentxx::plugin::tool(
            ctx,
            "bridge_cancel",
            "depict",
            "{}",
            [](BridgeCtx& c, std::string_view, OpCtl&) -> Task<std::string> {
                co_await sleep(c, 1000);
                co_return "never";
            }
        );

        auto  spec = g_capturedTool.spec;
        void* op   = startTool(spec, probe);
        XX_TEST_EXPECT_TRUE(op != nullptr);
        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_EQ(harness.sleeps.size(), size_t{1});

        // 宿主取消 Operation (execute_cancel 协作式取消)
        XX_TEST_EXPECT_TRUE(spec.execute_cancel != nullptr);
        spec.execute_cancel(spec.user_data, op);
        XX_TEST_EXPECT_EQ(probe.calls, 0); // 取消请求本身不完成操作
        harness.runPending();

        auto& sleepEntry = harness.sleeps.front();
        sleepEntry.cb(sleepEntry.ud, AGENTXX_PLUGIN_OPERATOR_CANCELLED, nullptr);
        harness.runPending();
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_CANCELLED);

        // 终态唯一: 宿主的 OpCore 在 done 提交后短路一切取消 (不会再用 op 句柄回调
        // 插件), 因此这里不再调用 execute_cancel, 只确认没有第二个终态。
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(harness.runPending(), 0);
    }

    /// 7. 宿主拒绝驱动: 根被终结为失败 (否则宿主 Operation 永远不完成)。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.init(&host);
        g_capturedTool = CapturedTool{};

        bool bodyRan = false;
        agentxx::plugin::tool(
            ctx,
            "bridge_refuse",
            "depict",
            "{}",
            [&bodyRan](BridgeCtx&, std::string_view, OpCtl&) -> Task<std::string> {
                bodyRan = true;
                co_return "nope";
            }
        );

        harness.refuseCount = 1;
        auto  spec          = g_capturedTool.spec;
        void* op            = startTool(spec, probe);
        // 操作已被接受 (返回 provider 句柄), 但宿主拒绝驱动 -> 立刻以 FAILED 终结;
        // 这是 ABI 内存操作契约允许的形态 (返回句柄就必须 exactly-once 通知, 而不是
        // 返回 NULL + error_out)。返回 NULL 只用于参数/实例状态非法的同步拒绝。
        XX_TEST_EXPECT_TRUE(op != nullptr);
        XX_TEST_EXPECT_FALSE(bodyRan);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_FAILED);
        XX_TEST_EXPECT_FALSE(probe.payload.empty());
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{0});
    }

    /// 8. stop: 取消尚未开始的请求 + 终结活跃根 (恰好一次失败上报) + 幂等 +
    ///    放弃后的迟到宿主回调安全跳过 (不访问已终结的根)。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.init(&host);
        g_capturedTool = CapturedTool{};

        bool sleepStarted = false;
        agentxx::plugin::tool(
            ctx,
            "bridge_stop",
            "depict",
            "{}",
            [&sleepStarted](BridgeCtx& c, std::string_view, OpCtl&) -> Task<std::string> {
                sleepStarted = true;
                co_await sleep(c, 1000);
                co_return "stopped";
            }
        );

        auto  spec = g_capturedTool.spec;
        void* op   = startTool(spec, probe);
        XX_TEST_EXPECT_TRUE(op != nullptr);
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{1});
        auto* bridge = &ctx.bridge();

        XX_TEST_EXPECT_TRUE(harness.runOne()); // 首步 -> 挂到 sleep 上
        XX_TEST_EXPECT_TRUE(sleepStarted);
        XX_TEST_EXPECT_EQ(harness.sleeps.size(), size_t{1});
        XX_TEST_EXPECT_EQ(probe.calls, 0);

        // 停止: 无排队请求可取消 (首步已执行), 但活跃根被终结为失败
        ctx.stopBridge();
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_FAILED);
        XX_TEST_EXPECT_TRUE(bridge->isStopping());

        // 停止后新的 wake 不再申请请求
        const auto ticketsAfterStop = bridge->ticketsIssued();
        bridge->wake();
        XX_TEST_EXPECT_EQ(bridge->ticketsIssued(), ticketsAfterStop);

        // 二次 stop 幂等 (不再重复上报)
        ctx.stopBridge();
        XX_TEST_EXPECT_EQ(probe.calls, 1);

        // 迟到的宿主完成回调: 根已被放弃, 必须安全跳过 (不恢复协程, 不二次上报)
        auto& sleepEntry = harness.sleeps.front();
        sleepEntry.cb(sleepEntry.ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
        XX_TEST_EXPECT_EQ(harness.runPending(), 0);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
    }

    /// 8b. stop 时请求仍排队: 尚未开始的请求被取消 (不再执行插件代码)。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.init(&host);
        g_capturedTool = CapturedTool{};

        bool bodyRan = false;
        agentxx::plugin::tool(
            ctx,
            "bridge_stop_queued",
            "depict",
            "{}",
            [&bodyRan](BridgeCtx&, std::string_view, OpCtl&) -> Task<std::string> {
                bodyRan = true;
                co_return "queued";
            }
        );

        auto  spec = g_capturedTool.spec;
        void* op   = startTool(spec, probe);
        XX_TEST_EXPECT_TRUE(op != nullptr);
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{1});

        ctx.stopBridge();
        XX_TEST_EXPECT_FALSE(bodyRan);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_FAILED);
        XX_TEST_EXPECT_EQ(harness.cancelCalls, 1);
        XX_TEST_EXPECT_TRUE(harness.pending.front()->cancelled);
        // 宿主按取消语义丢弃请求
        harness.pending.clear();
        XX_TEST_EXPECT_FALSE(bodyRan);
    }

    /// 9. 多实例隔离: 请求、唤醒、停止互不影响。
    {
        harness.clear();
        NotifyProbe probeA;
        NotifyProbe probeB;
        BridgeCtx   ctxA;
        ctxA.init(&host);
        g_capturedTool = CapturedTool{};
        agentxx::plugin::tool(
            ctxA,
            "bridge_multi_a",
            "depict",
            "{}",
            [](BridgeCtx&, std::string_view, OpCtl&) -> Task<std::string> {
                co_return "a";
            }
        );
        auto specA = g_capturedTool.spec;

        BridgeCtx ctxB;
        ctxB.init(&host);
        g_capturedTool = CapturedTool{};
        agentxx::plugin::tool(
            ctxB,
            "bridge_multi_b",
            "depict",
            "{}",
            [](BridgeCtx&, std::string_view, OpCtl&) -> Task<std::string> {
                co_return "b";
            }
        );
        auto specB = g_capturedTool.spec;

        void* opA = startTool(specA, probeA);
        void* opB = startTool(specB, probeB);
        XX_TEST_EXPECT_TRUE(opA != nullptr);
        XX_TEST_EXPECT_TRUE(opB != nullptr);
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{2});

        auto* bridgeA = &ctxA.bridge();
        auto* bridgeB = &ctxB.bridge();
        XX_TEST_EXPECT_TRUE(bridgeA != bridgeB);
        XX_TEST_EXPECT_EQ(bridgeA->ticketsIssued(), uint64_t{1});
        XX_TEST_EXPECT_EQ(bridgeB->ticketsIssued(), uint64_t{1});

        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_EQ(probeA.calls + probeB.calls, 1);
        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_EQ(probeA.calls, 1);
        XX_TEST_EXPECT_EQ(probeB.calls, 1);
        XX_TEST_EXPECT_TRUE(probeA.payload == "a" && probeB.payload == "b");

        ctxA.stopBridge();
        XX_TEST_EXPECT_TRUE(bridgeA->isStopping());
        XX_TEST_EXPECT_FALSE(bridgeB->isStopping());
        XX_TEST_EXPECT_EQ(harness.cancelCalls, 0); // 两者请求都已执行, 无需取消
    }

    /// 11. 受控轮询 (`polled_tool`): 插件本地 reactor 上的等待由 pump 推进。
    ///     - 首步不内联 (start 返回时业务体未运行, 只有桥的一次请求);
    ///     - 有进展 (本轮 poll_one 执行到 handler) 立即续票, 不等退避;
    ///     - 无进展时恰好安排一次 10ms 退避 (期间不新增请求);
    ///     - 根结束 -> 停止轮询 (取消在途退避), 之后请求数不再增长。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.init(&host);
        g_capturedTool = CapturedTool{};

        int  bodyRuns   = 0;
        bool sawPumping = false;
        agentxx::plugin::polled_tool(
            ctx,
            "polled_timer",
            "depict",
            "{}",
            [&](BridgeCtx& c,
                std::string_view,
                std::string_view,
                std::string_view,
                const AgentxxPluginCancelToken* cancel) -> asio::awaitable<std::string> {
                ++bodyRuns;
                auto* bridge = &c.bridge();
                sawPumping   = bridge != nullptr && bridge->isPumping();
                // 插件本地 reactor 上的等待 (没有宿主可见唤醒源)
                auto               ex = co_await asio::this_coro::executor;
                asio::steady_timer timer(ex, std::chrono::milliseconds(15));
                co_await timer.async_wait(asio::use_awaitable);
                if (agentxx_plugin_cancel_is_requested(cancel)) {
                    throw CancelledException("polled cancelled");
                }
                co_return "polled-ok";
            }
        );

        auto  spec = g_capturedTool.spec;
        void* op   = startTool(spec, probe);
        XX_TEST_EXPECT_TRUE(op != nullptr);
        XX_TEST_EXPECT_EQ(bodyRuns, 0); // 首步不内联
        XX_TEST_EXPECT_EQ(probe.calls, 0);
        auto* bridge = &ctx.bridge();
        XX_TEST_EXPECT_EQ(bridge->polledRootCount(), uint64_t{1});
        XX_TEST_EXPECT_TRUE(bridge->isPumping());
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{1});

        // 第一次请求: 业务体起步并挂到本地 timer 上 (有进展 -> 立即续票)
        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_EQ(bodyRuns, 1);
        XX_TEST_EXPECT_TRUE(sawPumping);
        XX_TEST_EXPECT_EQ(probe.calls, 0);
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{1});
        XX_TEST_EXPECT_TRUE(bridge->isPumping());

        // 第二张请求: timer 未到期 (无进展) -> 安排一次 10ms 退避, 不再申请请求
        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{0});
        XX_TEST_EXPECT_TRUE(bridge->isPumpWaitScheduled());
        XX_TEST_EXPECT_EQ(harness.sleeps.size(), size_t{1});
        XX_TEST_EXPECT_EQ(harness.sleeps.front().ms, int64_t{10});
        XX_TEST_EXPECT_EQ(bridge->idlePollCount(), uint64_t{1});

        // 退避到期 -> 再驱动; 反复直到本地 timer 真到期并把根跑完
        auto pumpUntilDone = [&](int maxRounds) {
            for (int i = 0; i < maxRounds && probe.calls == 0; ++i) {
                if (harness.queuedTicketCount() == 0) {
                    if (harness.sleeps.empty()) {
                        break;
                    }
                    auto entry = harness.sleeps.front();
                    harness.sleeps.erase(harness.sleeps.begin());
                    entry.cb(entry.ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
                    continue;
                }
                harness.runOne();
            }
        };
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        pumpUntilDone(200);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_OK);
        XX_TEST_EXPECT_EQ(probe.payload, std::string{"polled-ok"});
        XX_TEST_EXPECT_EQ(bodyRuns, 1);

        // 根结束 -> 停止轮询: 计数归零、在途退避取消、请求不再增长
        XX_TEST_EXPECT_EQ(bridge->polledRootCount(), uint64_t{0});
        XX_TEST_EXPECT_FALSE(bridge->isPumpWaitScheduled());
        XX_TEST_EXPECT_FALSE(bridge->isPumping());
        const auto ticketsAfter = bridge->ticketsIssued();
        XX_TEST_EXPECT_EQ(harness.runPending(), 0);
        XX_TEST_EXPECT_EQ(bridge->ticketsIssued(), ticketsAfter);
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{0});
    }

    /// 12. 受控轮询取消: execute_cancel 置取消标志 + 取消在途退避 (插件不必等满
    ///     一个退避量子), 根在下一个阶段边界收束, 只产生一个 CANCELLED 终态。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.init(&host);
        g_capturedTool = CapturedTool{};

        agentxx::plugin::polled_tool(
            ctx,
            "polled_cancel",
            "depict",
            "{}",
            [](BridgeCtx&,
               std::string_view,
               std::string_view,
               std::string_view,
               const AgentxxPluginCancelToken* cancel) -> asio::awaitable<std::string> {
                auto ex = co_await asio::this_coro::executor;
                for (int i = 0; i < 200; ++i) {
                    asio::steady_timer t(ex, std::chrono::milliseconds(5));
                    co_await t.async_wait(asio::use_awaitable);
                    if (agentxx_plugin_cancel_is_requested(cancel)) {
                        throw CancelledException("polled cancel");
                    }
                }
                co_return "never";
            }
        );

        auto  spec = g_capturedTool.spec;
        void* op   = startTool(spec, probe);
        XX_TEST_EXPECT_TRUE(op != nullptr);
        auto* bridge = &ctx.bridge();

        // 驱动到"退避在途"状态 (本地 timer 未到期, 本轮无进展)
        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_TRUE(bridge->isPumpWaitScheduled());
        XX_TEST_EXPECT_EQ(bridge->idlePollCount(), uint64_t{1});
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{0});

        // 取消: 在途退避被 op_cancel 取消 (真宿主随后会以 CANCELLED 触发完成回调)
        XX_TEST_EXPECT_TRUE(spec.execute_cancel != nullptr);
        spec.execute_cancel(spec.user_data, op);
        XX_TEST_EXPECT_EQ(harness.opCancelCalls, 1);
        XX_TEST_EXPECT_EQ(harness.sleeps.size(), size_t{1});
        XX_TEST_EXPECT_TRUE(harness.sleeps.front().cancelled);
        XX_TEST_EXPECT_FALSE(bridge->isPumpWaitScheduled()); // 插件侧状态已复位
        XX_TEST_EXPECT_EQ(probe.calls, 0);                   // 取消请求本身不完成操作

        // 退避回调以 CANCELLED 到达: 仍按"退避结束"处理 -> 立刻续票 (无需等 10ms)
        auto entry = harness.sleeps.front();
        harness.sleeps.erase(harness.sleeps.begin());
        entry.cb(entry.ud, AGENTXX_PLUGIN_OPERATOR_CANCELLED, nullptr);
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{1});

        // 继续驱动: 本地 5ms timer 到期后业务体看到取消 -> CANCELLED 终态
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        for (int i = 0; i < 200 && probe.calls == 0; ++i) {
            if (harness.queuedTicketCount() == 0) {
                if (harness.sleeps.empty()) {
                    break;
                }
                auto pending = harness.sleeps.front();
                harness.sleeps.erase(harness.sleeps.begin());
                pending.cb(pending.ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
                continue;
            }
            harness.runOne();
        }
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_CANCELLED);
        XX_TEST_EXPECT_EQ(probe.payload, std::string{"polled cancel"});
        XX_TEST_EXPECT_EQ(bridge->polledRootCount(), uint64_t{0});
        // 终态唯一: 再驱动不产生第二个终态
        XX_TEST_EXPECT_EQ(harness.runPending(), 0);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
    }

    /// 13. stop/关闭: 在途 polled 根按 FAILED 终结一次, pump 停止 (在途退避取消),
    ///     被放弃的 Job 由清理回调回收 (泄漏由 LSan 覆盖), 迟到请求不跑插件代码。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.init(&host);
        g_capturedTool = CapturedTool{};

        int bodyRuns = 0;
        agentxx::plugin::polled_tool(
            ctx,
            "polled_stop",
            "depict",
            "{}",
            [&bodyRuns](BridgeCtx&, std::string_view, std::string_view, std::string_view, const AgentxxPluginCancelToken*)
                -> asio::awaitable<std::string> {
                ++bodyRuns;
                auto               ex = co_await asio::this_coro::executor;
                asio::steady_timer t(ex, std::chrono::seconds(30));
                co_await t.async_wait(asio::use_awaitable);
                co_return "never";
            }
        );

        auto  spec = g_capturedTool.spec;
        void* op   = startTool(spec, probe);
        XX_TEST_EXPECT_TRUE(op != nullptr);
        auto* bridge = &ctx.bridge();
        XX_TEST_EXPECT_TRUE(harness.runOne()); // 业务体挂到 30s timer 上
        XX_TEST_EXPECT_EQ(bodyRuns, 1);
        XX_TEST_EXPECT_TRUE(harness.runOne()); // 无进展 -> 退避在途
        XX_TEST_EXPECT_TRUE(bridge->isPumpWaitScheduled());
        XX_TEST_EXPECT_EQ(probe.calls, 0);

        ctx.stopBridge();
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_FAILED);
        XX_TEST_EXPECT_EQ(bridge->polledRootCount(), uint64_t{0});
        XX_TEST_EXPECT_FALSE(bridge->isPumpWaitScheduled());
        XX_TEST_EXPECT_EQ(harness.opCancelCalls, 1); // 在途退避被取消
        XX_TEST_EXPECT_TRUE(bridge->isStopping());

        // 二次 stop 幂等 (不再重复上报)
        ctx.stopBridge();
        XX_TEST_EXPECT_EQ(probe.calls, 1);

        // 停止后 wake 不再申请请求, 也不执行插件代码
        const auto ticketsAfterStop = bridge->ticketsIssued();
        bridge->wake();
        XX_TEST_EXPECT_EQ(bridge->ticketsIssued(), ticketsAfterStop);
        XX_TEST_EXPECT_FALSE(harness.runOne());
        XX_TEST_EXPECT_EQ(bodyRuns, 1);
    }

    /// 14. 突发上限: 连续有进展 (业务体自己给自己 post) 达到 kPollBurstMax 后
    ///     强制让出 1ms, 避免同实例长期独占宿主 IO 线程。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.init(&host);
        g_capturedTool = CapturedTool{};

        constexpr int kSteps = agentxx::plugin::detail::PollOneBridge::kPollBurstMax + 40;
        agentxx::plugin::polled_tool(
            ctx,
            "polled_burst",
            "depict",
            "{}",
            [kSteps](BridgeCtx&, std::string_view, std::string_view, std::string_view, const AgentxxPluginCancelToken*)
                -> asio::awaitable<std::string> {
                auto ex = co_await asio::this_coro::executor;
                for (int i = 0; i < kSteps; ++i) {
                    co_await asio::post(ex, asio::use_awaitable);
                }
                co_return fmt::format("burst-{}", kSteps);
            }
        );

        auto  spec = g_capturedTool.spec;
        void* op   = startTool(spec, probe);
        XX_TEST_EXPECT_TRUE(op != nullptr);
        auto* bridge = &ctx.bridge();

        bool sawBurstYield = false;
        int  rounds        = 0;
        for (; rounds < 4000 && probe.calls == 0; ++rounds) {
            if (harness.queuedTicketCount() == 0) {
                if (harness.sleeps.empty()) {
                    break;
                }
                auto entry = harness.sleeps.front();
                harness.sleeps.erase(harness.sleeps.begin());
                // 突发让出是 1ms; 若这里是 10ms 说明策略退化成了空闲退避
                XX_TEST_EXPECT_EQ(entry.ms, int64_t{1});
                sawBurstYield = true;
                entry.cb(entry.ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
                continue;
            }
            harness.runOne();
        }
        XX_TEST_EXPECT_TRUE(sawBurstYield);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, AGENTXX_PLUGIN_OPERATOR_OK);
        XX_TEST_EXPECT_EQ(probe.payload, fmt::format("burst-{}", kSteps));
        XX_TEST_EXPECT_EQ(bridge->polledRootCount(), uint64_t{0});
        XX_TEST_EXPECT_GE(bridge->idlePollCount(), uint64_t{1});
        XX_TEST_EXPECT_EQ(rounds < 4000, true); // 未超轮次上限 (无自旋)
    }

    harness.clear();
    return result;
}

} // namespace agentxx::test
