/// 插件协程驱动桥测试（设计文档第 10 节的 kit 侧完成条件）。
///
/// 测试形态: 用**伪主程序**直接实现 `agentxx.agent.scheduler` 表 (含协程驱动)：
/// - `request_driver` 只把回调入队并返回请求 (永不内联), 由用例在"主程序 IO 线程"
///   上按需调用 [Harness::runOne], 从而精确观察"一次请求 = 一次 poll_one";
/// - `cancel_driver` 记录取消 (只对尚未执行的请求生效, 与主程序实现语义一致);
/// - `is_io_thread` 恒返回 1 (用例本身就在模拟主程序 IO 线程)。
///
/// 因此用例可以断言真实实现里很难观察的不变量: 不内联、恰好一次推进、空闲不新增
/// 请求、wake 三个窗口不丢失、主程序回调不重入插件协程、取消只产生一个终态。
#include "agentxx-test/plugin/test_plugin_bridge.h"

#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"

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

/// 请求句柄: 对插件侧只是不透明指针 (`PluginxxDriver` 为不完整类型),
/// 伪主程序用自建 token 表示它, 并记录回调/取消/执行状态。
struct Harness;

/// 伪调度器句柄记录 (sleep/offload 返回的不透明句柄):
/// 句柄本身没有 host 参数, 因此记录里带上所属 harness 以便 op_cancel 找到挂起点。
/// 注意: 必须定义在 Harness 之前 —— Harness 持有
/// `std::vector<std::unique_ptr<FakeOp>>`, 其 clear() 需要完整类型; libc++ 在类内
/// 实例化析构早于 libstdc++, 前置声明会让 libc++ 报
/// "invalid application of sizeof to incomplete type"。
struct FakeOp {
    Harness*  harness = nullptr;
    uintptr_t handle  = 0;
};

struct FakeTicket {
    PluginxxDriveOnceFn drive     = nullptr;
    void*               ud        = nullptr;
    Harness*            harness   = nullptr;
    bool                executed  = false;
    bool                Canceled = false;
};

/// 伪主程序驱动 + 调度设施。所有调用都发生在用例线程上 (模拟主程序 IO 线程)。
struct Harness {
    /// 伪调度器 sleep 的挂起点 (用例手工触发到期, 模拟主程序 timer adapter)。
    struct PendingSleep {
        int64_t                  ms     = 0;
        PluginxxOperatorCallback cb     = nullptr;
        void*                    ud     = nullptr;
        uintptr_t                handle = 0;
        /// 已被 op_cancel 请求取消。真主程序会在取消后以 Canceled 触发完成回调,
        /// 伪主程序只做标记, 由用例决定何时触发 (保持对既有用例的兼容)。
        bool Canceled = false;
    };

    std::deque<FakeTicket*> pending; ///< 已申请、尚未执行的请求 (FIFO = 主程序任务队列)
    std::vector<FakeTicket*>             all;
    std::vector<PendingSleep>            sleeps;
    std::vector<std::unique_ptr<FakeOp>> ops;  ///< 已发放的伪句柄记录
    std::vector<std::string>             logs; ///< 经主程序日志接口表收到的诊断
    int                                  refuseCount = 0; ///< >0 时 request_driver 直接失败
    int                                  cancelCalls = 0;
    int                                  isIoThreadCalls = 0;
    int                                  opCancelCalls = 0; ///< scheduler op_cancel 被调用次数
    int                                  offloadCalls  = 0; ///< scheduler offload 被调用次数
    uintptr_t                            nextSleepHandle = 1;
    /// 经 `cancel.op_cancel` 请求取消过的伪操作句柄 (按请求顺序)
    std::vector<uintptr_t> canceledHandles;

    size_t queuedTicketCount() const {
        return pending.size();
    }

    /// 造一个"已受理"的伪操作句柄 (登记在本 harness 里, 因此 `op_cancel` 能安全识别)
    PluginxxOperatorHandle* newOpHandle(uintptr_t id) {
        auto record     = std::make_unique<FakeOp>();
        record->harness = this;
        record->handle  = id;
        auto* raw       = record.get();
        ops.push_back(std::move(record));
        return reinterpret_cast<PluginxxOperatorHandle*>(raw);
    }

    /// 执行"下一次请求" (等价于主程序把一次投递排到 IO 线程并执行它)。
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
        canceledHandles.clear();
    }
};

Harness* harnessOf(const PluginxxHost* host) {
    return host ? static_cast<Harness*>(host->opaque) : nullptr;
}

/// 伪主程序的错误输出: 测试进程内用 malloc 即可 (真实主程序经 host->vtable->alloc)。
void setHostError(PluginxxString* out, std::string_view message) {
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

PluginxxDriver* PLUGINXX_CALL fakeRequestDriver(
    const PluginxxHost* host,
    PluginxxDriveOnceFn drive,
    void*               ud,
    PluginxxString*     error
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
    return reinterpret_cast<PluginxxDriver*>(ticket);
}

void PLUGINXX_CALL fakeCancelDriver(PluginxxDriver* driver) {
    auto* ticket = reinterpret_cast<FakeTicket*>(driver);
    if (!ticket) {
        return;
    }
    if (ticket->harness) {
        ++ticket->harness->cancelCalls;
    }
    // 与主程序实现一致: 取消只对尚未执行的请求生效, 执行中的不打断。
    if (!ticket->executed) {
        ticket->Canceled = true;
    }
}

int32_t PLUGINXX_CALL fakeIsIoThread(const PluginxxHost* host) {
    if (auto* harness = harnessOf(host)) {
        ++harness->isIoThreadCalls;
    }
    return 1;
}

/// 伪 sleep: 记录回调, 由用例触发到期 (真主程序 timer adapter 的等价物)。
PluginxxOperatorHandle* PLUGINXX_CALL fakeSleep(
    const PluginxxHost*      host,
    int64_t                  ms,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error
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
    return reinterpret_cast<PluginxxOperatorHandle*>(raw);
}

/// 伪 op_cancel: 只标记句柄对应的挂起点为"已取消" (真主程序随后会以 Canceled
/// 触发完成回调; 用例按需手工触发, 以免影响既有用例的手工时序)。
/// 句柄不含 host 参数, 因此伪主程序把 (harness, handle) 记录在堆对象里。
void PLUGINXX_CALL fakeOpCancel(PluginxxOperatorHandle* op) {
    auto* record = reinterpret_cast<FakeOp*>(op);
    if (!record || !record->harness) {
        return;
    }
    auto* harness = record->harness;
    harness->canceledHandles.push_back(record->handle);
    for (auto& entry : harness->sleeps) {
        if (entry.handle == record->handle) {
            ++harness->opCancelCalls;
            entry.Canceled = true;
            return;
        }
    }
}

int32_t PLUGINXX_CALL fakeOffloadIsCanceled(const PluginxxCancelToken*) {
    return 0;
}

/// 伪 offload: 用例线程内联执行工作体后触发完成回调。
/// (真主程序在工作线程池执行; 这里只需验证"降级路径能跑完"的语义, 不引入线程。)
PluginxxOperatorHandle* PLUGINXX_CALL fakeOffload(
    const PluginxxHost* host,
    void*(PLUGINXX_CALL*
              work)(void* ud, const PluginxxCancelToken* token, PluginxxString* error_out),
    void(PLUGINXX_CALL*
             done)(void* ud, int32_t status, void* result, const PluginxxStringView* error),
    void*           ud,
    PluginxxString* error
) {
    auto* harness = harnessOf(host);
    if (!harness || !work || !done) {
        setHostError(error, "fake scheduler offload: bad args");
        return nullptr;
    }
    ++harness->offloadCalls;
    PluginxxCancelToken token{&fakeOffloadIsCanceled, nullptr};
    PluginxxString      workError{nullptr, 0};
    void*               result = work(ud, &token, &workError);
    PluginString::free(host, &workError);
    PluginxxStringView payload = PluginStringView::from(nullptr, 0);
    done(ud, PLUGINXX_OPERATOR_OK, result, &payload);
    auto record     = std::make_unique<FakeOp>();
    record->harness = harness;
    record->handle  = harness->nextSleepHandle++;
    auto* raw       = record.get();
    harness->ops.push_back(std::move(record));
    return reinterpret_cast<PluginxxOperatorHandle*>(raw);
}

void PLUGINXX_CALL fakeLog(const PluginxxHost* host, int32_t, const PluginxxStringView* msg) {
    auto* harness = harnessOf(host);
    if (harness && msg && msg->data) {
        harness->logs.emplace_back(msg->data, static_cast<size_t>(msg->size));
    }
}

PluginxxOperatorHandle* PLUGINXX_CALL fakeRegisterTool(
    const PluginxxHost*,
    const PluginxxScopeRef*,
    const AgentxxPluginToolSpec* spec,
    PluginxxOperatorCallback cb,
    void* ud,
    PluginxxString* error_out
);

const PluginxxSchedulerIface g_fakeScheduler = {
    /* version */ PLUGINXX_IFACE_SCHEDULER_VERSION,
    /* struct_size */ sizeof(PluginxxSchedulerIface),
    /* is_io_thread */ fakeIsIoThread,
    /* sleep */ fakeSleep,
    /* offload */ fakeOffload,
    /* request_driver */ fakeRequestDriver,
    /* cancel_driver */ fakeCancelDriver,
};

const PluginxxCancelIface g_fakeCancel = {
    /* version */ PLUGINXX_IFACE_CANCEL_VERSION,
    /* struct_size */ sizeof(PluginxxCancelIface),
    /* op_cancel */ fakeOpCancel,
};

const PluginxxLogIface g_fakeLog = {
    /* version */ PLUGINXX_IFACE_LOG_VERSION,
    /* struct_size */ sizeof(PluginxxLogIface),
    /* log */ fakeLog,
};

const AgentxxPluginToolsIface g_fakeTools = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION,
    /* struct_size */ sizeof(AgentxxPluginToolsIface),
    /* register_tool */ fakeRegisterTool,
    /* unregister_tool */ nullptr,
    /* call_tool */ nullptr,
};

/// 伪 `pluginxx.tasks`: 只为 `spawn` 提供"已受理"句柄 (不跟踪任务, 也不取消)。
PluginxxOperatorHandle* PLUGINXX_CALL fakeRegisterTask(
    const PluginxxHost*,
    PluginxxOperatorCancelFunction,
    void*,
    PluginxxOperatorNotify*,
    PluginxxString*
) {
    return reinterpret_cast<PluginxxOperatorHandle*>(1);
}

void PLUGINXX_CALL fakeCancelTask(PluginxxOperatorHandle*) {}

const PluginxxTasksIface g_fakeTasks = {
    /* version */ PLUGINXX_IFACE_TASKS_VERSION,
    /* struct_size */ sizeof(PluginxxTasksIface),
    /* register_task */ fakeRegisterTask,
    /* cancel_task */ fakeCancelTask,
};

/// 用例可临时置空来验证"主程序不提供驱动时"的回退路径。
const PluginxxSchedulerIface* g_schedulerForTest = &g_fakeScheduler;

const void* PLUGINXX_CALL fakeQueryInterface(const PluginxxHost*, const PluginxxStringView* iid) {
    if (!iid || !iid->data) {
        return nullptr;
    }
    const std::string_view name{iid->data, static_cast<size_t>(iid->size)};
    if (name == PLUGINXX_IFACE_SCHEDULER) {
        return g_schedulerForTest;
    }
    if (name == PLUGINXX_IFACE_CANCEL) {
        return &g_fakeCancel;
    }
    if (name == PLUGINXX_IFACE_LOG) {
        return &g_fakeLog;
    }
    if (name == AGENTXX_PLUGIN_IFACE_AGENT_TOOLS) {
        return &g_fakeTools;
    }
    if (name == PLUGINXX_IFACE_TASKS) {
        return &g_fakeTasks;
    }
    return nullptr;
}

/// 伪主程序内存操作: kit 会用 host->vtable->free 释放 request_driver 的
/// error_out (缺失该函数会导致测试进程真的泄漏, 不是被测代码的问题)。
void* PLUGINXX_CALL fakeAlloc(uint64_t size) {
    return std::malloc(static_cast<size_t>(size));
}

void PLUGINXX_CALL fakeFree(void* ptr) {
    std::free(ptr);
}

const PluginxxHostVtable g_fakeVtable = {
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

/// 伪主程序的挂载引用 (`seq` = 0 = 不校验; 本模块不需要真实挂载)
const agentxx::plugin::ScopeRef g_agentRef{"root", 0};
const agentxx::plugin::ScopeRef g_agentRefA{"root", 0};
const agentxx::plugin::ScopeRef g_agentRefB{"root", 0};

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
    g_capturedTool.spec = *spec;
    g_capturedTool.has  = true;
    if (cb) {
        cb(ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return reinterpret_cast<PluginxxOperatorHandle*>(1);
}

/// 完成通知探针 (工具结果分两段: 正文 + 元数据)。
struct NotifyProbe {
    int         calls  = 0;
    int32_t     status = -1;
    std::string result;  ///< 正文
    std::string payload; ///< 元数据 JSON (无元数据为空)

    static void PLUGINXX_CALL done(
        void*                     ud,
        int32_t                   status,
        const PluginxxStringView* result,
        const PluginxxStringView* payload
    ) {
        auto& probe  = *static_cast<NotifyProbe*>(ud);
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
        auto& probe  = *static_cast<NotifyProbe*>(ud);
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
        auto& probe  = *static_cast<NotifyProbe*>(ud);
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
        return AgentxxPluginToolNotify{
            &NotifyProbe::done,
            &NotifyProbe::doneText,
            &NotifyProbe::doneFrame,
            this
        };
    }
};

struct BridgeCtx : PluginBase {};

/// 启动一个工具根操作并返回 provider 句柄 (用例统一入口)
///
/// 与主程序一样两种启动形态都支持: 有 `execute_start_invoke` 时优先用它
/// (带调用期上下文), 否则回落到旧签名。
void* startTool(
    const AgentxxPluginToolSpec& spec,
    NotifyProbe&                 probe,
    const char*                  argsJson = "{}"
) {
    auto notify = probe.notify();
    auto args   = PluginStringView::fromCstr(argsJson);
    auto sid    = PluginStringView::fromCstr("session-1");
    auto cid    = PluginStringView::fromCstr("call-1");
    if (spec.execute_start_invoke) {
        // 测试夹具没有会话工作目录与插件参数: 按"未提供"传空视图 / 空 JSON
        auto workDir = PluginStringView::from(nullptr, 0);
        auto pluginArgs = PluginStringView::fromCstr("{}");
        AgentxxPluginToolInvoke invoke{};
        invoke.struct_size  = sizeof(AgentxxPluginToolInvoke);
        invoke.args_json    = &args;
        invoke.session_id   = &sid;
        invoke.tool_call_id = &cid;
        invoke.work_dir     = &workDir;
        invoke.plugin_args  = &pluginArgs;
        invoke.notify       = &notify;
        return spec.execute_start_invoke(spec.user_data, &invoke, nullptr);
    }
    return spec.execute_start(spec.user_data, &args, &sid, &cid, &notify, nullptr);
}

/// 注册一个 asio 协程体工具并当场拿到 spec (用例是同步函数, 不能 `co_await`)
///
/// 与 `toolAsync` 同一份 spec 构造 (`buildToolSpecWithAwaitableBody`), 只是登记
/// 请求直接交给伪主程序: 这里不关心登记结果, 只关心注册出来的 spec。
template<typename Ctx, typename Body>
void registerAwaitableTool(
    Ctx&             ctx,
    const ScopeRef&  agent,
    std::string_view name,
    std::string_view depict,
    std::string_view schema,
    Body&&           body
) {
    auto spec = agentxx::plugin::detail::buildToolSpecWithAwaitableBody(
        ctx,
        agent,
        name,
        depict,
        schema,
        std::forward<Body>(body)
    );
    auto raw = agent.raw();
    (void)ctx.iface.tools->register_tool(ctx.host, &raw, &spec, nullptr, nullptr, nullptr);
}

} // namespace

TestResult testPluginBridge() {
    TestResult         result;
    Harness            harness;
    const PluginxxHost host{&g_fakeVtable, &harness};

    /// 11. 受控轮询 (`tool` 的 asio 协程体): 插件本地 reactor 上的等待由 pump 推进。
    ///     - 首步不内联 (start 返回时业务体未运行, 只有桥的一次请求);
    ///     - 有进展 (本轮 poll_one 执行到 handler) 立即续票, 不等退避;
    ///     - 无进展时恰好安排一次 10ms 退避 (期间不新增请求);
    ///     - 根结束 -> 停止轮询 (取消尚未触发的退避), 之后请求数不再增长。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.attachHost(&host);
        g_capturedTool = CapturedTool{};

        int  bodyRuns   = 0;
        bool sawPumping = false;
        registerAwaitableTool(
            ctx,
            g_agentRef,
            "polled_timer",
            "depict",
            "{}",
            [&](BridgeCtx& c,
                std::string_view,
                std::string_view,
                std::string_view,
                const PluginxxCancelToken* cancel) -> asio::awaitable<std::string> {
                ++bodyRuns;
                auto* bridge = &c.bridge();
                sawPumping   = bridge != nullptr && bridge->isPumping();
                // 插件本地 reactor 上的等待 (没有主程序可见唤醒源)
                auto               ex = co_await asio::this_coro::executor;
                asio::steady_timer timer(ex, std::chrono::milliseconds(15));
                co_await timer.async_wait(asio::use_awaitable);
                if (pluginxx_cancel_is_requested(cancel)) {
                    throw CanceledException("polled Canceled");
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
                    entry.cb(entry.ud, PLUGINXX_OPERATOR_OK, nullptr);
                    continue;
                }
                harness.runOne();
            }
        };
        // 本地 timer 到期后 root 才会跑完: 计时依赖机器负载, 固定一次等待在重负载下
        // 会偶发失败 (本地 timer 尚未到期就没法继续推进), 因此按总时限重试
        const auto pumpDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (probe.calls == 0 && std::chrono::steady_clock::now() < pumpDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            pumpUntilDone(200);
        }
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_OK);
        XX_TEST_EXPECT_EQ(probe.result, std::string{"polled-ok"});
        XX_TEST_EXPECT_EQ(bodyRuns, 1);

        // 根结束 -> 停止轮询: 计数归零、尚未触发的退避被取消、请求不再增长
        XX_TEST_EXPECT_EQ(bridge->polledRootCount(), uint64_t{0});
        XX_TEST_EXPECT_FALSE(bridge->isPumpWaitScheduled());
        XX_TEST_EXPECT_FALSE(bridge->isPumping());
        const auto ticketsAfter = bridge->ticketsIssued();
        XX_TEST_EXPECT_EQ(harness.runPending(), 0);
        XX_TEST_EXPECT_EQ(bridge->ticketsIssued(), ticketsAfter);
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{0});
    }

    /// 12. 受控轮询取消: execute_cancel 置取消标志 + 取消尚未触发的退避 (插件不必等满
    ///     一个退避间隔), 根在下一个阶段边界结束, 只产生一个 Canceled 终态。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.attachHost(&host);
        g_capturedTool = CapturedTool{};

        registerAwaitableTool(
            ctx,
            g_agentRef,
            "polled_cancel",
            "depict",
            "{}",
            [](BridgeCtx&,
               std::string_view,
               std::string_view,
               std::string_view,
               const PluginxxCancelToken* cancel) -> asio::awaitable<std::string> {
                auto ex = co_await asio::this_coro::executor;
                for (int i = 0; i < 200; ++i) {
                    asio::steady_timer t(ex, std::chrono::milliseconds(5));
                    try {
                        co_await t.async_wait(asio::use_awaitable);
                    } catch (const boost::system::system_error& e) {
                        // 取消会撤掉正在等的 asio 操作: 业务体想自己给原因就在这里转
                        if (e.code() == asio::error::operation_aborted) {
                            throw CanceledException("polled cancel");
                        }
                        throw;
                    }
                    if (pluginxx_cancel_is_requested(cancel)) {
                        throw CanceledException("polled cancel");
                    }
                }
                co_return "never";
            }
        );

        auto  spec = g_capturedTool.spec;
        void* op   = startTool(spec, probe);
        XX_TEST_EXPECT_TRUE(op != nullptr);
        auto* bridge = &ctx.bridge();

        // 驱动到"已安排退避"状态 (本地 timer 未到期, 本轮无进展)
        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_TRUE(harness.runOne());
        XX_TEST_EXPECT_TRUE(bridge->isPumpWaitScheduled());
        XX_TEST_EXPECT_EQ(bridge->idlePollCount(), uint64_t{1});
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{0});

        // 取消: 尚未触发的退避被 op_cancel 取消 (真主程序随后会以 Canceled 触发完成回调)
        XX_TEST_EXPECT_TRUE(spec.execute_cancel != nullptr);
        spec.execute_cancel(spec.user_data, op);
        XX_TEST_EXPECT_EQ(harness.opCancelCalls, 1);
        XX_TEST_EXPECT_EQ(harness.sleeps.size(), size_t{1});
        XX_TEST_EXPECT_TRUE(harness.sleeps.front().Canceled);
        XX_TEST_EXPECT_FALSE(bridge->isPumpWaitScheduled()); // 插件侧状态已复位
        XX_TEST_EXPECT_EQ(probe.calls, 0);                   // 取消请求本身不完成操作

        // 退避回调以 Canceled 到达: 仍按"退避结束"处理 -> 立刻续票 (无需等 10ms)
        auto entry = harness.sleeps.front();
        harness.sleeps.erase(harness.sleeps.begin());
        entry.cb(entry.ud, PLUGINXX_OPERATOR_CANCELED, nullptr);
        XX_TEST_EXPECT_EQ(harness.queuedTicketCount(), size_t{1});

        // 继续驱动: 本地 5ms timer 到期后业务体看到取消 -> Canceled 终态
        // (同样按总时限重试: 计时依赖机器负载, 只等一次在重负载下会偶发失败)
        const auto cancelDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (probe.calls == 0 && std::chrono::steady_clock::now() < cancelDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            for (int i = 0; i < 200 && probe.calls == 0; ++i) {
                if (harness.queuedTicketCount() == 0) {
                    if (harness.sleeps.empty()) {
                        break;
                    }
                    auto pending = harness.sleeps.front();
                    harness.sleeps.erase(harness.sleeps.begin());
                    pending.cb(pending.ud, PLUGINXX_OPERATOR_OK, nullptr);
                    continue;
                }
                harness.runOne();
            }
        }
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_CANCELED);
        XX_TEST_EXPECT_EQ(probe.result, std::string{"polled cancel"});
        XX_TEST_EXPECT_EQ(bridge->polledRootCount(), uint64_t{0});
        // 终态唯一: 再驱动不产生第二个终态
        XX_TEST_EXPECT_EQ(harness.runPending(), 0);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
    }

    /// 13. stop/关闭: 未完成的 polled 根按 FAILED 终结一次, pump 停止 (尚未触发的退避
    ///     被取消),
    ///     被放弃的 Job 由清理回调回收 (泄漏由 LSan 覆盖), 迟到请求不跑插件代码。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.attachHost(&host);
        g_capturedTool = CapturedTool{};

        int bodyRuns = 0;
        registerAwaitableTool(
            ctx,
            g_agentRef,
            "polled_stop",
            "depict",
            "{}",
            [&bodyRuns](BridgeCtx&, std::string_view, std::string_view, std::string_view, const PluginxxCancelToken*)
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
        XX_TEST_EXPECT_TRUE(harness.runOne()); // 无进展 -> 已安排退避
        XX_TEST_EXPECT_TRUE(bridge->isPumpWaitScheduled());
        XX_TEST_EXPECT_EQ(probe.calls, 0);

        ctx.stopBridge();
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_FAILED);
        XX_TEST_EXPECT_EQ(bridge->polledRootCount(), uint64_t{0});
        XX_TEST_EXPECT_FALSE(bridge->isPumpWaitScheduled());
        XX_TEST_EXPECT_EQ(harness.opCancelCalls, 1); // 尚未触发的退避被取消
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
    ///     强制让出 1ms, 避免同实例长期独占主程序 IO 线程。
    {
        harness.clear();
        NotifyProbe probe;
        BridgeCtx   ctx;
        ctx.attachHost(&host);
        g_capturedTool = CapturedTool{};

        constexpr int kSteps = agentxx::plugin::detail::PollOneBridge::kPollBurstMax + 40;
        registerAwaitableTool(
            ctx,
            g_agentRef,
            "polled_burst",
            "depict",
            "{}",
            [kSteps](BridgeCtx&, std::string_view, std::string_view, std::string_view, const PluginxxCancelToken*)
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
                entry.cb(entry.ud, PLUGINXX_OPERATOR_OK, nullptr);
                continue;
            }
            harness.runOne();
        }
        XX_TEST_EXPECT_TRUE(sawBurstYield);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.status, PLUGINXX_OPERATOR_OK);
        XX_TEST_EXPECT_EQ(probe.result, fmt::format("burst-{}", kSteps));
        XX_TEST_EXPECT_EQ(bridge->polledRootCount(), uint64_t{0});
        XX_TEST_EXPECT_GE(bridge->idlePollCount(), uint64_t{1});
        XX_TEST_EXPECT_EQ(rounds < 4000, true); // 未超轮次上限 (无自旋)
    }

    harness.clear();
    return result;
}

} // namespace agentxx::test
