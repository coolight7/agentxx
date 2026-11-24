#include "agentxx-test/plugin/test_plugin_runtime.h"
#include "agentxx-test/plugin/test_table_op.h"

#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/plugin/plugin_graph_node.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugins/session_cancel_snapshot.h"
#include "asio/co_spawn.hpp"
#include "asio/io_context.hpp"
#include "asio/thread_pool.hpp"
#include "asio/use_future.hpp"
#include "neograph/graph/cancel.h"
#include "neograph/graph/registry.h"
#include "neograph/graph/run_context.h"
#include "neograph/graph/state.h"
#include "neograph/graph/types.h"
#include "pluginxx/host/table_op.h"
#include "pluginxx/runtime/op_driver.h"
#include "utilxx_base/asio_error.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <thread>

#define XX_TEST_PASSED result.passed
#define XX_TEST_FAILED result.failed

#include "agentxx-test/plugin/coroutine_runner.h"

namespace agentxx::plugin {
/// 主程序 vtable 加载入口（定义在 plugin_manager_vtable.cpp；测试用它给伪实例
/// 拿到真实接口表，从而直接驱动 C ABI 入口）。
const void* PLUGINXX_CALL xx_query_interface(const PluginxxHost*, const PluginxxStringView* iid);
} // namespace agentxx::plugin

/// C17 ABI 检查翻译单元（test_plugin_abi_c17.c）导出的对照入口。
/// - `agentxx_test_abi_value`：C 侧看到的 sizeof/offsetof/版本号（编号见表）
/// - `agentxx_test_abi_c_probe`：C 侧自检，0 表示通过
extern "C" uint64_t agentxx_test_abi_value(int32_t id);
extern "C" int32_t  agentxx_test_abi_c_probe(void);

namespace agentxx::test {
namespace {
using namespace agentxx::plugin;
using namespace std::chrono_literals;

/// 每个用例自行推进 IO；worker 用 promise/barrier 控制，无定时轮询。
struct RuntimeFixture {
    asio::io_context               io;
    std::shared_ptr<PluginManager> manager
        = std::make_shared<PluginManager>(std::weak_ptr<agentxx::agent::AgentContext>{});
    /// 本夹具的 agent 上下文与挂载 (登记记录落在挂载上)
    std::shared_ptr<agentxx::agent::AgentContext> ctx;
    std::shared_ptr<PluginMount>                 mount;
    std::shared_ptr<PluginInstance> provider;
    std::shared_ptr<PluginInstance> caller;

    /// 注意: 参数不能与成员 `ctx` 同名 —— 同名时成员恒为空, 临时上下文在构造
    /// 结束时释放, 其析构会关掉管理器上的全部实例。
    explicit RuntimeFixture(std::shared_ptr<agentxx::agent::AgentContext> existingCtx = {}) {
        if (existingCtx) {
            manager = std::make_shared<PluginManager>(existingCtx);
        }
        manager->setIoExecutor(io.get_executor());
        ctx = existingCtx ? std::move(existingCtx)
                          : std::make_shared<agentxx::agent::AgentContext>();
        // 挂载记录: 单元测试只走主程序侧接口, 不需要插件的 mount 回调
        ctx->toolRegistry  = std::make_shared<agentxx::plugin::ToolRegistry>();
        ctx->pluginManager = manager;
        mount              = manager->attachMountSync(ctx);
        ctx->pluginMount   = mount;
        provider = instance("provider", 1);
        caller   = instance("caller", 2);
    }

    std::shared_ptr<PluginInstance> instance(std::string name, uint64_t generation) {
        auto inst  = std::make_shared<PluginInstance>(std::move(name));
        inst->self = inst;
        // 基类自引用: 通用表实现 (sleep/offload/postCallback/registerTask/能力调用)
        // 只依赖 PluginInstanceBase::ownerSelf, 因此夹具必须与 makeInstance 一样设置它
        inst->ownerSelf = inst;
        inst->manager   = manager;
        // 主程序控制块：交给插件的 host 视图必须有进程级稳定地址；这里加载真实
        // 主程序 vtable，便于用例直接驱动 C ABI 入口。
        auto vtableSv     = PluginStringView::fromCstr("__vtable");
        inst->hostControl = PluginHostControl::create(
            inst,
            (const PluginxxHostVtable*)xx_query_interface(nullptr, &vtableSv)
        );
        const std::weak_ptr<pluginxx::PluginRuntime> runtime = manager->runtime();
        // 与 PluginManagerBase::makeLifetime 的加载语义一致: 实例登记所属运行时,
        // Operation 驱动器据此取 io executor 与线程标识
        inst->runtime  = manager->runtime();
        inst->lifetime = std::make_shared<pluginxx::InstanceLifetime>(
            io.get_executor(),
            inst->name,
            generation,
            [runtime](std::function<void()> fn) {
                if (auto state = runtime.lock()) {
                    return pluginxx::enqueueRuntimeAction(state, std::move(fn), true);
                }
                return false;
            }
        );
        inst->lifetime->setState(pluginxx::PluginInstanceState::Ready);
        manager->plugins_.emplace(inst->name, inst);
        return inst;
    }

    std::shared_ptr<OpCore> operation() {
        return OpCore::create(manager->runtime(), provider, caller, "runtime regression");
    }

    /// 把已排队的插件回调跑完 (等到插件线程空下来)
    ///
    /// 管理器现在**构造即起**插件执行线程, 插件代码 (accept/完成回调/驱动票) 都在那条
    /// 线上跑; 只 poll io 会把“已经投进插件队列但还没执行”的步骤当成已完成。
    /// 这里往插件执行器 post 一个哨兵并等它执行 —— 队列 FIFO, 哨兵跑完就说明前面的活
    /// 都跑完了。未配独立插件线程时它等于 io 执行器, 直接返回。
    void drainPluginThread() {
        if (!manager || !manager->hasPluginExecutor()) {
            return;
        }
        auto ex = manager->pluginExecutor();
        if (!ex) {
            return;
        }
        auto reached = std::make_shared<std::atomic<bool>>(false);
        asio::post(ex, [reached] { reached->store(true, std::memory_order_release); });
        for (int i = 0; i < 2000 && !reached->load(std::memory_order_acquire); ++i) {
            // 插件线程可能正在同步等 io (同步桥查表): 等待期间顺手推进 io,
            // 否则两边互等 (插件线程等 io 的完成, 而 io 没人跑)
            io.restart();
            io.poll();
            io.restart();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    void drain() {
        // 插件线程与 io 线程来回投递 (完成包 → 回调 → 再投回 io), 一轮 poll 不够;
        // 而且 worker / 独立定时器线程投的完成包在高负载下会晚到 —— 固定轮数会把
        // “还没入队”当成“已经跑完”。这里推进到连续几轮都没有就绪处理为止,
        // 总预算 500ms: 超预算只会让后面的断言失败, 不会挂住。
        const auto deadline = std::chrono::steady_clock::now() + 500ms;
        int        idle     = 0;
        for (;;) {
            drainPluginThread();
            io.restart();
            const auto ran = io.poll();
            // io_context becomes stopped after an empty poll. Keep the fixture's
            // normal state distinct from an explicit stop used by fault tests.
            io.restart();
            if (ran != 0) {
                idle = 0;
            } else if (++idle >= 5) {
                break;
            } else {
                // 让 worker / 定时器线程 / 插件线程有机会把完成包投进来;
                // 退避到 1~5ms, 总共约 15ms 的静默窗口足够盖住投递抖动
                std::this_thread::sleep_for(std::chrono::milliseconds{std::min(idle, 5)});
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
        }
        drainPluginThread();
    }

    /// 推进 IO 直到没有新的就绪处理器：生命周期事务包含多级投递
    /// (start/stop 完成包 → 状态提交 → 后续步骤)，单次 poll 不足以跑完。
    ///
    /// 注意"跨线程/定时器完成包"的投递时刻：Windows(IOCP) 下由其它线程
    /// (worker 提交的完成包、IO 线程之外的 post、**独立定时器线程的到期投递**)
    /// 产生的处理器可能在本次 `poll()` 返回之后才入队，Linux(epoll) 则在同一次
    /// poll 内就会处理已到期 timer。因此无就绪处理器时先短暂让出 CPU 再确认一次，
    /// 避免把"尚未入队"误判成"事务已跑完"（生产环境有常驻 `run()` 线程，不存在
    /// 该问题；这里只是让轮询驱动的测试不依赖平台投递时序）。
    void drainAll(int rounds = 8) {
        for (int i = 0; i < rounds; ++i) {
            io.restart();
            if (io.poll() != 0) {
                continue;
            }
            std::this_thread::sleep_for(1ms);
            io.restart();
            io.poll();
        }
        io.restart();
    }

    /// 持续推进 IO 直到谓词为真（有界，超时返回 false）。
    /// 用于"效果来自其它线程/定时器"的等待：不能靠固定轮数假设投递时刻。
    template<typename Pred>
    bool drainUntil(Pred pred, std::chrono::milliseconds timeout = 10s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (pred()) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            drain();
            std::this_thread::sleep_for(1ms);
        }
    }

    /// 有界等待 `use_future` 结果，同时持续推进 IO。
    ///
    /// 为什么需要它（而不是 `drain()` 之后直接 `future.get()`）：
    /// - 测试里主线程**同时**充当"IO 线程"与等待者：一旦阻塞在 `get()` 上，
    ///   就没有人再推进 io_context，最后一步投递将永远不会被处理 → 永久挂起；
    /// - Windows(win_iocp) 的定时器到期由**独立定时器线程**投递完成包，单次
    ///   `poll()` 返回后该完成包可能尚未入队（Linux epoll reactor 在 poll 内
    ///   直接处理已到期 timer，因此不会暴露）；生产环境有常驻 `run()` 线程，
    ///   该差异只影响这种"轮询驱动"的测试写法。
    /// 因此这里在等待期间持续 `poll()` 并让出 CPU，直到结果就绪或超时。
    ///
    /// - `timeout` 超时即返回 false：测试宁可失败也不允许永久挂起
    /// - 不处理 future；调用方在返回 true 后再 `get()`（异常语义不变）
    template<typename T>
    bool waitFutureReady(std::future<T>& fut, std::chrono::milliseconds timeout = 10s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (fut.wait_for(0ms) != std::future_status::ready) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            drain();
            // 让第三方线程(如 IOCP 定时器线程)有机会投递完成包
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    /// 有界等待并取结果；超时返回 `std::nullopt`（不阻塞、不永久挂起）。
    /// 结果本身是异常时照常抛出（取消/超时导致的 `AsioSystemError` 等）。
    /// - 按值接收 `future`（`std::future` 只可移动）: 调用方传右值或 `std::move`
    template<typename T>
    std::optional<T> waitFutureValue(std::future<T> fut, std::chrono::milliseconds timeout = 10s) {
        if (!waitFutureReady(fut, timeout)) {
            return std::nullopt;
        }
        return fut.get();
    }
};

struct CallbackState {
    int             calls                   = 0;
    int             status                  = -1;
    bool            protectedDuringCallback = false;
    bool            onIo                    = false;
    /// 结果正文
    std::string     result;
    /// 结果元数据 JSON (无元数据为空)
    std::string     payload;
    RuntimeFixture* fixture = nullptr;

    static void PLUGINXX_CALL done(
        void*                     ud,
        int32_t                   status,
        const PluginxxStringView* result,
        const PluginxxStringView* payload
    ) {
        auto& state   = *static_cast<CallbackState*>(ud);
        ++state.calls;
        state.status  = status;
        state.result  = svToStr(result);
        state.payload = svToStr(payload);
        state.noteCallbackContext();
    }

    /// 框架层的单段完成回调 (直接驱动 OpCore 的用例用): 结果只有一段
    static void PLUGINXX_CALL onOpDone(
        void*                     ud,
        int32_t                   status,
        const PluginxxStringView* payload
    ) {
        auto& state  = *static_cast<CallbackState*>(ud);
        ++state.calls;
        state.status = status;
        state.result = svToStr(payload);
        state.noteCallbackContext();
    }

    /// 记下回调现场 (是否在主程序的回调线程 / 双方 lease 是否仍在)
    ///
    /// 回调线程 = 插件代码的执行线程 (配了插件执行器时是那条线程, 否则是 io 线程)。
    /// 它不是工作池线程也不是任意线程 —— 旧断言写的 `onIo` 就是这个意思。
    void noteCallbackContext() {
        if (fixture) {
            onIo                    = fixture->manager->isIoThread()
                                      || fixture->manager->isPluginThread();
            protectedDuringCallback = fixture->provider->lifetime->leaseCount() != 0
                                      && fixture->caller->lifetime->leaseCount() != 0;
        }
    }
};

/// 顺序记录（取消 → 恢复 → 完成 → 回调 → 销毁）。
///
/// 追加来自后台任务线程（resumed/doneSubmitted）、主程序 IO 线程（cancel/callback/
/// completion）；主线程在 join 之前也要读它（断言"卸载停在 lease 等待上"时
/// order[0] 已是 cancel）。所有访问因此必须经互斥量 —— 裸 vector 在这里就是
/// 一条真实的数据竞争（TSan 定向回归会命中）。
struct OrderLog {
    mutable std::mutex       mu;
    std::vector<std::string> items;

    void push(std::string item) {
        std::lock_guard<std::mutex> lock(mu);
        items.push_back(std::move(item));
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mu);
        return items.size();
    }

    std::string at(size_t index) const {
        std::lock_guard<std::mutex> lock(mu);
        return index < items.size() ? items[index] : std::string{};
    }
};

/// plugin.md 11.2-8/9 探针：记录取消 → 恢复 → 完成 → 回调 → 销毁的实际顺序，
/// 并证明回调返回前不会调用插件 destroy。
struct ShutdownOrderProbe {
    OrderLog*       order                 = nullptr;
    PluginInstance* provider              = nullptr;
    PluginManager*  manager               = nullptr;
    bool            destroyDuringCallback = false;
    bool            onIo                  = false;

    static void PLUGINXX_CALL record(void* ud, int32_t, const PluginxxStringView*) {
        auto& probe = *static_cast<ShutdownOrderProbe*>(ud);
        if (probe.order) {
            probe.order->push("callback");
        }
        probe.destroyDuringCallback = probe.provider && probe.provider->pluginDestroyed;
        // 回调线程 = 插件代码的执行线程 (插件执行器 > io)
        probe.onIo = probe.manager
                     && (probe.manager->isIoThread() || probe.manager->isPluginThread());
    }
};

AgentxxPluginToolSpec fakeTool(void* ud, bool reject) {
    AgentxxPluginToolSpec spec{};
    spec.name            = strToSv("runtime_tool");
    spec.parameters_json = strToSv("{}");
    spec.user_data       = ud;
    if (reject) {
        spec.execute_start = +[](void*,
                                 const PluginxxStringView*,
                                 const PluginxxStringView*,
                                 const PluginxxStringView*,
                                 const AgentxxPluginToolNotify*,
                                 PluginxxString* error) -> void* {
            hostMemorySetString(error, "rejected by fake provider");
            return nullptr;
        };
    } else {
        spec.execute_start = +[](void*                          ud,
                                 const PluginxxStringView*      args,
                                 const PluginxxStringView*,
                                 const PluginxxStringView*,
                                 const AgentxxPluginToolNotify* notify,
                                 PluginxxString*) -> void* {
            *static_cast<bool*>(ud) = true;
            auto emptySv            = agentxx::plugin::PluginStringView::from(nullptr, 0);
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, args, &emptySv);
            return nullptr;
        };
    }
    return spec;
}

/// 生命周期 hook 探针: 记录 stop/destroy 实际调用次数。
int gLifecycleStops    = 0;
int gLifecycleDestroys = 0;

void PLUGINXX_CALL fakeStopHook(void*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    ++gLifecycleStops;
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
}

void PLUGINXX_CALL fakeDestroyHook(void* ud, const PluginxxOperatorNotify* notify, PluginxxString*) {
    ++*static_cast<int*>(ud);
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
}

/// 接口表严格协商探针：伪装主程序只返回一次可控接口表，用于验证 SDK 对
/// version / struct_size / NULL 表的拒绝行为（plugin.md 第 11.1 节）。
struct FakeIfaceHost {
    PluginxxHost                   host{};
    const AgentxxPluginToolsIface* table = nullptr;
};

const void* PLUGINXX_CALL fakeIfaceQuery(const PluginxxHost* host, const PluginxxStringView*) {
    auto* self = static_cast<FakeIfaceHost*>(host ? host->opaque : nullptr);
    return self ? static_cast<const void*>(self->table) : nullptr;
}

const PluginxxHostVtable g_fakeIfaceVtable = {
    /* alloc */ nullptr,
    /* free */ nullptr,
    /* query_interface */ fakeIfaceQuery,
};

/// 加载一个“激活且导出 stop”的伪实例: destroy 计数挂在 pluginCtx 上。
void installLifecycleHooks(PluginInstance& inst, int* destroys) {
    inst.lifecycleStop    = &fakeStopHook;
    inst.lifecycleStarted = true;
    inst.pluginCreated    = true;
    inst.lifecycleDestroy = &fakeDestroyHook;
    inst.pluginCtx        = destroys;
}

/// 生命周期事务探针: 记录 start/stop 实际调用次数，并让 start 可以按需失败。
/// 探针本身挂在实例的 pluginCtx 上，不使用任何可变全局状态。
void* PLUGINXX_CALL
    probeGraphRunStart(void* ud, const PluginxxStringView*, const PluginxxStringView*, const PluginxxStringView*, const PluginxxStringView*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    (void)ud;
    if (notify && notify->done) {
        PluginxxStringView empty{nullptr, 0};
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &empty);
    }
    return nullptr;
}

struct LifecycleProbe {
    int  starts    = 0;
    int  stops     = 0;
    bool failStart = false;
    /// start 失败前是否真的登记过工具（证明失败回滚清掉了"部分注册"）。
    bool                           partialRegistration = false;
    const PluginxxHost*            host                = nullptr;
    const AgentxxPluginToolsIface* tools               = nullptr;
    const AgentxxPluginGraphIface* graph               = nullptr;
    /// 表调用阻塞形态需要 (表成员是异步契约, 生命周期入口要用阻塞形态等完成)
    const PluginxxCancelIface*    cancel    = nullptr;
    const PluginxxSchedulerIface* scheduler = nullptr;
    /// start 失败前是否真的登记过图节点类型（证明图注册同样回滚）。
    bool                  partialGraphRegistration = false;
    AgentxxPluginToolSpec spec{};
    /// 接口调用用的挂载引用（`id` 借用 [agentId] 的内存）
    std::string       agentId;
    PluginxxScopeRef  ref{};
};

/// create = 构造 + 注册 (可重入): 上下文由主程序经 `*plugin_ctx` 交回
void PLUGINXX_CALL lifecycleCreateHook(
    const PluginxxHost*,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error
) {
    // 失败原因走 notify 的 payload (ABI 约定), 这个出参只在同步失败时才用
    (void)error;
    auto& probe = *static_cast<LifecycleProbe*>(*plugin_ctx);
    ++probe.starts;

    // 注册一律“提交 + 完成回调里收口”: 这个入口跑在插件执行线程上, 在那里同步等表结果
    // 就是自己等自己 (完成回调要投回这条线程)。与真实插件的 `finishLater` 同一个意思 ——
    // notify 可以从任意线程、在所有表调用结束后回答。
    struct StartPending {
        /// 按值存: 回答时可能已经在另一个线程, 借用指针不再可靠
        PluginxxOperatorNotify notify{};
        int                     remaining = 0;
        bool                    failStart = false;
    };
    auto pending = std::make_shared<StartPending>();
    pending->notify    = *notify;
    pending->failStart = probe.failStart;

    const bool wantTool = probe.tools != nullptr && probe.host != nullptr;
    const bool wantGraph
        = probe.graph != nullptr && probe.host != nullptr
          && probe.graph->register_node_type != nullptr;
    pending->remaining = (wantTool ? 1 : 0) + (wantGraph ? 1 : 0);

    auto finish = [pending]() {
        if (--pending->remaining > 0) {
            return;
        }
        auto& out = pending->notify;
        if (pending->failStart) {
            // 事务中途失败: 主程序必须撤销这次已生效的注册（部分注册回滚）。
            // 失败原因按 ABI 约定放在 payload 里。
            static constexpr std::string_view kRefused = "start refused by probe";
            PluginxxStringView                sv{kRefused.data(), kRefused.size()};
            out.done(out.host_ud, PLUGINXX_OPERATOR_FAILED, &sv);
            return;
        }
        out.done(out.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    };

    if (pending->remaining == 0) {
        // 没有要登记的东西: 直接收口
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        return;
    }
    if (wantTool) {
        agentxx::test::tableOpSubmit(
            probe.host,
            probe.cancel,
            probe.scheduler,
            [&probe](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return probe.tools->register_tool(
                    probe.host,
                    &probe.ref,
                    &probe.spec,
                    cb,
                    ud,
                    err
                );
            },
            [&probe, finish](pluginxx::detail::TableOpResult r) {
                probe.partialRegistration = r.ok();
                finish();
            }
        );
    }
    if (wantGraph) {
        AgentxxPluginGraphNodeTypeSpec gspec{};
        gspec.type      = agentxx::plugin::PluginStringView::fromCstr("probe_graph_type");
        gspec.run_start = &probeGraphRunStart;
        gspec.user_data = &probe;
        agentxx::test::tableOpSubmit(
            probe.host,
            probe.cancel,
            probe.scheduler,
            [&probe, gspec](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return probe.graph->register_node_type(
                    probe.host,
                    &probe.ref,
                    &gspec,
                    cb,
                    ud,
                    err
                );
            },
            [&probe, finish](pluginxx::detail::TableOpResult r) {
                probe.partialGraphRegistration = r.ok();
                finish();
            }
        );
    }
}

void PLUGINXX_CALL
    lifecycleStopHook(void* ud, const PluginxxOperatorNotify* notify, PluginxxString*) {
    auto& probe = *static_cast<LifecycleProbe*>(ud);
    ++probe.stops;
    // 同样不在这里等: 注销完成后回答 notify (notify 按值存一份, 回调可能在另一条线程)
    const PluginxxOperatorNotify notifyCopy = *notify;
    if (!probe.tools || !probe.host) {
        notifyCopy.done(notifyCopy.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        return;
    }
    agentxx::test::tableOpSubmit(
        probe.host,
        probe.cancel,
        probe.scheduler,
        [&probe](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return probe.tools->unregister_tool(
                probe.host,
                &probe.ref,
                &probe.spec.name,
                cb,
                ud,
                err
            );
        },
        [notifyCopy](pluginxx::detail::TableOpResult) {
            notifyCopy.done(notifyCopy.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        }
    );
}
} // namespace

TestResult testPluginRuntime() {
    TestResult result;

    /// 会话取消快照: 登记/查询可跨线程进行 (插件线程问 `is_canceled` 不等 IO)
    {
        SessionCancelSnapshot snapshot;
        auto                 token = std::make_shared<neograph::graph::CancelToken>();
        snapshot.bind("sess-a", token);
        XX_TEST_EXPECT_EQ(snapshot.size(), size_t{1});
        XX_TEST_EXPECT_FALSE(snapshot.canceled("sess-a"));
        XX_TEST_EXPECT_FALSE(snapshot.canceled("sess-b"));
        XX_TEST_EXPECT_FALSE(snapshot.canceled(""));
        std::atomic<int> seen{-1};
        std::thread      reader([&snapshot, &seen] {
            seen.store(snapshot.canceled("sess-a") ? 1 : 0, std::memory_order_release);
        });
        reader.join();
        XX_TEST_EXPECT_EQ(seen.load(), 0);
        token->cancel();
        XX_TEST_EXPECT_TRUE(snapshot.canceled("sess-a"));
        // 同一会话再次开轮: 新令牌顶掉旧令牌
        auto next = std::make_shared<neograph::graph::CancelToken>();
        snapshot.bind("sess-a", next);
        XX_TEST_EXPECT_FALSE(snapshot.canceled("sess-a"));
        snapshot.unbind("sess-a");
        XX_TEST_EXPECT_FALSE(snapshot.canceled("sess-a"));
        XX_TEST_EXPECT_EQ(snapshot.size(), size_t{0});
    }

    /// 拒绝不回调，双方登记和 lease 完整回滚；同步 done 仍为接受。
    {
        RuntimeFixture f;
        CallbackState  cb{.fixture = &f};
        auto           spec = fakeTool(nullptr, true);
        XX_TEST_EXPECT_EQ(f.mount->registerTool(f.provider.get(), &spec), 0);
        PluginxxString error{};
        auto*          rejected = f.mount->callToolAsync(
            f.caller.get(),
            "runtime_tool",
            "{}",
            "s",
            CallbackState::done,
            &cb,
            &error
        );
        XX_TEST_EXPECT_TRUE(rejected == nullptr);
        XX_TEST_EXPECT_TRUE(error.data != nullptr);
        hostMemoryFree(error.data);
        f.drain();
        XX_TEST_EXPECT_EQ(cb.calls, 0);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        XX_TEST_EXPECT_TRUE(f.provider->outstandingOps.empty());
        XX_TEST_EXPECT_TRUE(f.caller->outstandingOps.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
        XX_TEST_EXPECT_EQ(f.caller->lifetime->leaseCount(), size_t{0});
        f.mount->unregisterTool(f.provider.get(), "runtime_tool");
        bool started = false;
        spec         = fakeTool(&started, false);
        XX_TEST_EXPECT_EQ(f.mount->registerTool(f.provider.get(), &spec), 0);
        error          = {};
        auto* accepted = f.mount->callToolAsync(
            f.caller.get(),
            "runtime_tool",
            R"({"hello":"world"})",
            "s",
            CallbackState::done,
            &cb,
            &error
        );
        XX_TEST_EXPECT_TRUE(accepted != nullptr);
        XX_TEST_EXPECT_TRUE(started);
        XX_TEST_EXPECT_TRUE(error.data == nullptr);
        XX_TEST_EXPECT_EQ(cb.calls, 0);
        f.drain();
        XX_TEST_EXPECT_EQ(cb.calls, 1);
        XX_TEST_EXPECT_TRUE(cb.protectedDuringCallback && cb.onIo);
        XX_TEST_EXPECT_TRUE(cb.result.find("world") != std::string::npos);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        /// callback 可选，不允许因未提供 callback 将已接受请求改为拒绝。
        accepted = f.mount->callToolAsync(
            f.caller.get(),
            "runtime_tool",
            "{}",
            "s",
            nullptr,
            nullptr,
            &error
        );
        XX_TEST_EXPECT_TRUE(accepted != nullptr);
        f.drain();
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
    }

    /// 任意线程复制 payload，提交后取消无效；caller Closing
    /// 仍保护本次回调直到返回，不依赖 provider 启用位延长代码生命周期。
    {
        RuntimeFixture f;
        CallbackState  cb{.fixture = &f};
        auto           op      = f.operation();
        int            cancels = 0;
        op->accept([&] {
            ++cancels;
        });
        op->setCallback(CallbackState::onOpDone, &cb);
        const auto        notify = op->notify();
        const std::string expected(65536, 'x');
        std::thread       worker([notify, expected] {
            auto text = expected;
            auto view = strToSv(text);
            notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, &view);
            text.assign(text.size(), 'z');
        });
        worker.join();
        XX_TEST_EXPECT_TRUE(op->submitted());
        XX_TEST_EXPECT_FALSE(op->completed());
        XX_TEST_EXPECT_EQ(cb.calls, 0);
        f.caller->lifetime->requestClose();
        f.provider->lifetime->requestClose();
        op->cancel();
        XX_TEST_EXPECT_EQ(cancels, 0);
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_FAILED, nullptr);
        f.drain();
        XX_TEST_EXPECT_TRUE(op->completed());
        XX_TEST_EXPECT_EQ(cb.calls, 1);
        XX_TEST_EXPECT_EQ(cb.status, PLUGINXX_OPERATOR_OK);
        XX_TEST_EXPECT_EQ(cb.result, expected);
        XX_TEST_EXPECT_TRUE(cb.protectedDuringCallback);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
        XX_TEST_EXPECT_EQ(f.caller->lifetime->leaseCount(), size_t{0});
        cancelPluginOperation(op->handle());
        f.drain();
        XX_TEST_EXPECT_EQ(cancels, 0);
    }

    /// 取消中同步 done 可重入，终态清理不能遗漏；异常 callback 不阻止内部清理。
    {
        RuntimeFixture f;
        auto           op      = f.operation();
        auto           notify  = op->notify();
        int            cancels = 0, cleanup = 0;
        op->accept([&] {
            ++cancels;
            notify.done(notify.host_ud, PLUGINXX_OPERATOR_CANCELED, nullptr);
        });
        op->setCallback(
            +[](void*, int32_t, const PluginxxStringView*) {
                throw std::runtime_error("fake callback failure");
            },
            nullptr
        );
        op->setCompletionHandler([&](int32_t, std::string_view) {
            ++cleanup;
        });
        op->cancel();
        op->cancel();
        f.drain();
        XX_TEST_EXPECT_EQ(cancels, 1);
        XX_TEST_EXPECT_EQ(cleanup, 1);
        XX_TEST_EXPECT_TRUE(op->completed());
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// 关闭 deadline 与一次性 idle 事件；超时保留操作，后续释放可重试并广播。
    {
        RuntimeFixture f;
        auto           op = f.operation();
        op->accept();
        f.provider->lifetime->requestClose();
        XX_TEST_EXPECT_FALSE(static_cast<bool>(pluginxx::InstanceLease::acquire(f.provider->lifetime
        )));
        auto expired = asio::co_spawn(
            f.io,
            f.provider->lifetime->waitIdleUntil(std::chrono::steady_clock::now()),
            asio::use_future
        );
        // 超时即断言失败 (不允许测试永久挂起): 见 waitFutureValue 注释
        auto expiredValue = f.waitFutureValue(std::move(expired));
        XX_TEST_EXPECT_TRUE(expiredValue.has_value());
        XX_TEST_EXPECT_FALSE(expiredValue.value_or(true));
        f.provider->lifetime->setState(pluginxx::PluginInstanceState::CloseFailed);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1});
        auto first = asio::co_spawn(
            f.io,
            f.provider->lifetime->waitIdleUntil(std::chrono::steady_clock::now() + 5s),
            asio::use_future
        );
        auto second = asio::co_spawn(
            f.io,
            f.provider->lifetime->waitIdleUntil(std::chrono::steady_clock::now() + 5s),
            asio::use_future
        );
        f.drain();
        XX_TEST_EXPECT_TRUE(first.wait_for(0s) != std::future_status::ready);
        auto notify = op->notify();
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        f.drain();
        XX_TEST_EXPECT_TRUE(f.waitFutureValue(std::move(first)).value_or(false));
        XX_TEST_EXPECT_TRUE(f.waitFutureValue(std::move(second)).value_or(false));
        f.provider->lifetime->setState(pluginxx::PluginInstanceState::Closed);
        f.manager->plugins_.erase(f.provider->name);
    }

    /// worker 发起互调，start/登记/完成完整地在 IO 线程执行。
    {
        RuntimeFixture f;
        CallbackState  cb{.fixture = &f};
        bool           started = false;
        auto           spec    = fakeTool(&started, false);
        XX_TEST_EXPECT_EQ(f.mount->registerTool(f.provider.get(), &spec), 0);
        auto               keepIo = asio::make_work_guard(f.io);
        std::promise<bool> returned;
        std::thread        worker([&] {
            PluginxxString error{};
            auto*          handle = f.mount->callToolAsync(
                f.caller.get(),
                "runtime_tool",
                "{}",
                "worker",
                CallbackState::done,
                &cb,
                &error
            );
            returned.set_value(handle != nullptr && error.data == nullptr);
            hostMemoryFree(error.data);
            keepIo.reset();
        });
        f.io.run();
        worker.join();
        XX_TEST_EXPECT_TRUE(returned.get_future().get());
        XX_TEST_EXPECT_TRUE(started && cb.onIo);
        XX_TEST_EXPECT_EQ(cb.calls, 1);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
    }

    /// 1000 个跨线程完成：每个包的独立输入、状态和登记最终全部回收。
    {
        RuntimeFixture                       f;
        std::vector<std::shared_ptr<OpCore>> ops;
        std::vector<CallbackState>           callbacks(1000);
        for (auto& cb : callbacks) {
            auto op = f.operation();
            op->accept();
            op->setCallback(CallbackState::onOpDone, &cb);
            ops.push_back(std::move(op));
        }
        std::barrier             ready(5);
        std::vector<std::thread> workers;
        for (size_t worker = 0; worker < 4; ++worker) {
            workers.emplace_back([&, worker] {
                ready.arrive_and_wait();
                for (size_t i = worker; i < ops.size(); i += 4) {
                    auto text   = std::to_string(i) + std::string(128, 'p');
                    auto view   = strToSv(text);
                    auto notify = ops[i]->notify();
                    notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, &view);
                }
            });
        }
        ready.arrive_and_wait();
        for (auto& worker : workers) {
            worker.join();
        }
        // 完成包由 4 条 worker 线程投递, 高负载下会晚到: 先有界等到全部回调到齐再判定
        XX_TEST_EXPECT_TRUE(f.drainUntil([&] {
            for (const auto& cb : callbacks) {
                if (cb.calls != 1) {
                    return false;
                }
            }
            return true;
        }));
        bool correct = true;
        for (size_t i = 0; i < callbacks.size(); ++i) {
            correct = correct && callbacks[i].calls == 1
                      && callbacks[i].status == PLUGINXX_OPERATOR_OK
                      && callbacks[i].result == std::to_string(i) + std::string(128, 'p');
        }
        XX_TEST_EXPECT_TRUE(correct);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        XX_TEST_EXPECT_TRUE(f.provider->outstandingOps.empty() && f.caller->outstandingOps.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// worker 完成与 IO 取消并发。cancel 只请求取消，worker 仍唯一地提交终态；
    /// IO 在 worker 继续运行时处理完成包，覆盖并发投递而非仅 join 后批量读取。
    {
        RuntimeFixture                       f;
        constexpr size_t                     count = 128;
        std::vector<std::shared_ptr<OpCore>> ops;
        std::vector<CallbackState>           callbacks(count);
        std::vector<int>                     cancels(count, 0);
        for (size_t i = 0; i < count; ++i) {
            auto op = f.operation();
            op->accept([&, i] {
                ++cancels[i];
            });
            op->setCallback(CallbackState::onOpDone, &callbacks[i]);
            ops.push_back(std::move(op));
        }
        auto         keepIo = asio::make_work_guard(f.io);
        std::barrier ready(2);
        std::thread  worker([&] {
            ready.arrive_and_wait();
            for (const auto& op : ops) {
                auto notify = op->notify();
                notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
            }
            keepIo.reset();
        });
        ready.arrive_and_wait();
        for (const auto& op : ops) {
            cancelPluginOperation(op->handle());
        }
        f.io.run();
        worker.join();
        // 取消与完成并发: 每个已受理的操作必须恰好提交一次终态。完成包可能还没
        // 入队, 先有界等到全部完成再判定 (超时只让断言失败, 不会挂住)。
        XX_TEST_EXPECT_TRUE(f.drainUntil([&] {
            for (size_t i = 0; i < count; ++i) {
                if (callbacks[i].calls != 1 || !ops[i]->completed()) {
                    return false;
                }
            }
            return true;
        }));
        bool correct = true;
        for (size_t i = 0; i < count; ++i) {
            const bool ok = callbacks[i].calls == 1 && cancels[i] <= 1 && ops[i]->completed();
            if (!ok) {
                XX_LOGW(
                    "runtime regression: op {} calls={} cancels={} state={} operations={}",
                    i,
                    callbacks[i].calls,
                    cancels[i],
                    static_cast<int>(ops[i]->state()),
                    f.manager->runtime()->operations.size()
                );
            }
            correct = correct && ok;
        }
        XX_TEST_EXPECT_TRUE(correct);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// 等待者取消与 provider 完成分开：取消等待不能回收尚未完成的执行 lease。
    {
        RuntimeFixture f;
        auto           op = f.operation();
        op->accept();
        asio::cancellation_signal cancel;
        // 协程体是 op->wait() (成员协程): op 由夹具持有, 且下面在作用域内等待 future
        // → 不需要 keepAliveRun
        auto waited = asio::co_spawn(
            f.io,
            op->wait(),
            asio::bind_cancellation_slot(cancel.slot(), asio::use_future)
        );
        f.drain();
        cancel.emit(asio::cancellation_type::all);
        f.drain();
        bool aborted = false;
        // 有界等待: 取消投递在 Windows 下可能晚于单次 poll (见 waitFutureReady 注释)
        if (f.waitFutureReady(waited)) {
            try {
                waited.get();
            } catch (const utilxx_base::AsioSystemError&) {
                aborted = true;
            }
        }
        XX_TEST_EXPECT_TRUE(aborted);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1});
        XX_TEST_EXPECT_FALSE(op->completed());
        auto notify = op->notify();
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        f.drain();
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// 后台 task 提交 done 后，尚未 commit 就卸载，不调用失效 cancel_ud。
    {
        RuntimeFixture         f;
        PluginxxOperatorNotify notify{};
        PluginxxString         error{};
        int                    cancels = 0;
        auto*                  handle  = f.manager->registerTask(
            f.provider.get(),
            +[](void* ud, void*) {
                ++*static_cast<int*>(ud);
            },
            &cancels,
            &notify,
            &error
        );
        XX_TEST_EXPECT_TRUE(handle != nullptr && error.data == nullptr && notify.done != nullptr);
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        f.provider->lifetime->requestClose();
        f.manager->detachAll(f.provider.get());
        XX_TEST_EXPECT_EQ(cancels, 0);
        XX_TEST_EXPECT_FALSE(f.provider->outstandingOps.empty());
        f.drain();
        XX_TEST_EXPECT_EQ(cancels, 0);
        XX_TEST_EXPECT_TRUE(f.provider->outstandingOps.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// 关闭期 (Closing) 的后台清理仍要被受理: `register_task` / `sleep` / `offload`
    /// 是 stop 里“入口返回、任务继续跑”的基础 (`ctx.spawn` + `finishLater`);
    /// 这些 lease 计入 inflight, 卸载等待因此能覆盖到它们; 进入 Closed 后仍拒绝。
    {
        RuntimeFixture         f;
        PluginxxOperatorNotify notify{};
        PluginxxString         error{};
        f.provider->lifetime->requestClose();

        // 1) 关闭中的后台任务: 受理 + 记 lease (改动前这里会抛 "provider is closing")
        auto* handle = f.manager->registerTask(
            f.provider.get(),
            +[](void*, void*) {},
            nullptr,
            &notify,
            &error
        );
        XX_TEST_EXPECT_TRUE(handle != nullptr && notify.done != nullptr);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1});

        // 2) 关闭中的完成回调照常投递一次, lease 归零 (卸载得以继续)
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        XX_TEST_EXPECT_TRUE(f.drainUntil([&] { return f.provider->lifetime->leaseCount() == 0; }));

        // 3) 关闭中的 sleep: 受理且回调到达 (清理里的延时重试)
        int   calls = 0;
        auto* timer = f.manager->sleep(
            f.provider.get(),
            0,
            +[](void* ud, int32_t, const PluginxxStringView*) {
                ++*static_cast<int*>(ud);
            },
            &calls,
            &error
        );
        XX_TEST_EXPECT_TRUE(timer != nullptr);
        XX_TEST_EXPECT_TRUE(f.drainUntil([&] { return calls == 1; }));
        hostMemoryFree(error.data);
        error = {};

        // 4) 关闭中的 offload: 受理且工作体与完成回调都执行 (阻塞收尾)
        struct OffloadState {
            std::atomic<int> work{0};
            std::atomic<int> done{0};
        } offState;
        f.manager->offload(
            f.provider.get(),
            +[](void* ud, const PluginxxCancelToken*, PluginxxString*) -> void* {
                ++static_cast<OffloadState*>(ud)->work;
                return nullptr;
            },
            +[](void* ud, int32_t, void*, const PluginxxStringView*) {
                ++static_cast<OffloadState*>(ud)->done;
            },
            &offState,
            &error
        );
        XX_TEST_EXPECT_TRUE(f.drainUntil([&] {
            return offState.work.load() == 1 && offState.done.load() == 1;
        }));
        hostMemoryFree(error.data);
        error = {};

        // 5) 真实的 C ABI 入口也要放行关闭中: 插件侧的 `ctx.spawn` / `sleep` / `offload`
        //    经 tasks/scheduler 表进主程序, 入口处的状态门比管理器方法更靠前
        //    (曾因入口用严格判定而让 stop 里的清理协程根本起不来)。
        const PluginxxHost* hostView = f.provider->hostControl->host();
        auto                ifaceOf  = [](const PluginxxHost* h, const char* iid) -> const void* {
            if (!h || !h->vtable || !h->vtable->query_interface) {
                return nullptr;
            }
            auto sv = PluginStringView::fromCstr(iid);
            return h->vtable->query_interface(h, &sv);
        };
        const auto* tasks
            = static_cast<const PluginxxTasksIface*>(ifaceOf(hostView, PLUGINXX_IFACE_TASKS));
        const auto* scheduler = static_cast<const PluginxxSchedulerIface*>(
            ifaceOf(hostView, PLUGINXX_IFACE_SCHEDULER)
        );
        XX_TEST_EXPECT_TRUE(tasks != nullptr && tasks->register_task != nullptr);
        XX_TEST_EXPECT_TRUE(scheduler != nullptr && scheduler->sleep != nullptr);
        if (tasks && tasks->register_task && scheduler && scheduler->sleep) {
            PluginxxOperatorNotify entryNotify{};
            PluginxxString         entryError{};
            auto*                  entryHandle = tasks->register_task(
                hostView,
                +[](void*, void*) {},
                nullptr,
                &entryNotify,
                &entryError
            );
            XX_TEST_EXPECT_TRUE(entryHandle != nullptr && entryNotify.done != nullptr);
            hostMemoryFree(entryError.data);
            if (entryHandle) {
                entryNotify.done(entryNotify.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
            }
            int   entryCalls = 0;
            auto* entryTimer = scheduler->sleep(
                hostView,
                0,
                +[](void* ud, int32_t, const PluginxxStringView*) {
                    ++*static_cast<int*>(ud);
                },
                &entryCalls,
                &entryError
            );
            XX_TEST_EXPECT_TRUE(entryTimer != nullptr);
            hostMemoryFree(entryError.data);
            entryError = {};
            XX_TEST_EXPECT_TRUE(f.drainUntil([&] { return entryCalls == 1; }));
            XX_TEST_EXPECT_TRUE(f.drainUntil([&] {
                return f.provider->lifetime->leaseCount() == 0;
            }));
        }

        // 6) 进入 Closed 之后一律拒绝: dlclose 之后不能再进插件代码
        // (用单独的探针实例并先从管理器表里摘掉: 已 Closed 的实例不能再走收尾请求)
        auto probe = f.instance("closed_probe", 3);
        f.manager->plugins_.erase("closed_probe");
        probe->lifetime->setState(pluginxx::PluginInstanceState::Closed);
        PluginxxOperatorNotify closedNotify{};
        PluginxxString         closedError{};
        auto*                  rejected = f.manager->registerTask(
            probe.get(),
            +[](void*, void*) {},
            nullptr,
            &closedNotify,
            &closedError
        );
        XX_TEST_EXPECT_TRUE(rejected == nullptr);
        hostMemoryFree(closedError.data);
        // 入口路径同样拒: dlclose 之后的真插件回调也会走这条表
        if (tasks && tasks->register_task) {
            PluginxxOperatorNotify closedEntryNotify{};
            PluginxxString         closedEntryError{};
            auto*                  closedHandle = tasks->register_task(
                probe->hostControl->host(),
                +[](void*, void*) {},
                nullptr,
                &closedEntryNotify,
                &closedEntryError
            );
            XX_TEST_EXPECT_TRUE(closedHandle == nullptr);
            hostMemoryFree(closedEntryError.data);
        }
        probe.reset();
    }

    /// sleep 接受即计入 lease；取消和正常完成都回收记录，并且只回调一次。
    {
        RuntimeFixture f;
        int            calls    = 0;
        auto           callback = +[](void* ud, int32_t, const PluginxxStringView*) {
            ++*static_cast<int*>(ud);
        };
        PluginxxString error{};
        bool           allAccepted = true;
        for (int i = 0; i < 1000; ++i) {
            allAccepted
                = (f.manager->sleep(f.provider.get(), 0, callback, &calls, &error) != nullptr)
                  && allAccepted;
            hostMemoryFree(error.data);
            error = {};
        }
        XX_TEST_EXPECT_TRUE(allAccepted);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1000});
        XX_TEST_EXPECT_EQ(calls, 0);
        // 1000 个 0ms 定时器由独立定时器线程到期投递: 高负载下会晚到, 有界等到全到齐
        XX_TEST_EXPECT_TRUE(f.drainUntil([&] { return calls == 1000; }));
        XX_TEST_EXPECT_EQ(calls, 1000);
        XX_TEST_EXPECT_TRUE(f.provider->sleepTimers.empty());
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        auto* timer = f.manager->sleep(f.provider.get(), 60000, callback, &calls, &error);
        cancelPluginOperation(timer);
        cancelPluginOperation(timer);
        f.provider->lifetime->requestClose();
        // 取消后的完成包同经定时器线程, 有界等
        XX_TEST_EXPECT_TRUE(f.drainUntil([&] { return calls == 1001; }));
        XX_TEST_EXPECT_EQ(calls, 1001);
        XX_TEST_EXPECT_TRUE(f.provider->sleepTimers.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// 管理器先于完成包释放: 已受理的回调仍旧执行, lease 正常回收。
    /// (原用例走 `postCallback`; 该入口已删 —— 插件不再有“往 io 线程投递任意
    /// 代码”的能力, 这里改用 `sleep` 验证同一套投递协议。)
    {
        RuntimeFixture f;
        int            calls = 0;
        auto           callback = +[](void* ud, int32_t, const PluginxxStringView*) {
            ++*static_cast<int*>(ud);
        };
        PluginxxString error{};
        auto*          handle = f.manager->sleep(f.provider.get(), 0, callback, &calls, &error);
        XX_TEST_EXPECT_TRUE(handle != nullptr);
        XX_TEST_EXPECT_EQ(calls, 0);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1});
        /// 先移出实例，避免旧同步 shutdown 路径干扰这里独立验证的投递协议。
        f.manager->plugins_.clear();
        f.manager.reset();
        f.provider->lifetime->requestClose();
        // 管理器已释放, 完成包仍会执行: 有界等它到达
        XX_TEST_EXPECT_TRUE(f.drainUntil([&] { return calls == 1; }));
        XX_TEST_EXPECT_EQ(calls, 1);
        XX_TEST_EXPECT_TRUE(f.provider->sleepTimers.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// 未注入工作线程池时管理器自建一个: 主 agent 上下文缺失/未挂载也不能让
    /// 插件阻塞工作全部失败 (否则阻塞工具全部报 offload 失败)。工作体仍执行
    /// 一次, 经 IO 投递完成且无错误。
    {
        RuntimeFixture f;

        struct State {
            std::atomic<int> work{0};
            std::atomic<int> done{0};
            std::string      error;
        } state;

        PluginxxString error{};
        f.manager->offload(
            f.provider.get(),
            +[](void* ud, const PluginxxCancelToken*, PluginxxString*) -> void* {
                ++static_cast<State*>(ud)->work;
                return nullptr;
            },
            +[](void* ud, int32_t, void*, const PluginxxStringView* error) {
                auto& state = *static_cast<State*>(ud);
                state.error = svToStr(error);
                ++state.done;
            },
            &state,
            &error
        );
        // 工作体在线程池执行、完成后回投到本夹具 IO: 推进 IO 直到上报完成
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (state.done.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            f.drain();
            std::this_thread::sleep_for(1ms);
        }
        XX_TEST_EXPECT_EQ(state.work.load(), 1);
        XX_TEST_EXPECT_EQ(state.done.load(), 1);
        XX_TEST_EXPECT_TRUE(state.error.empty());
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
    }

    /// 主程序注入的工作线程池 (AgentHost / BaseAgent 的做法) 优先于上下文回查:
    /// 没有主 agent 上下文时 offload 也走注入的池, 工作体照常执行。
    {
        RuntimeFixture f;
        auto            pool = std::make_shared<asio::thread_pool>(2);
        f.manager->setWorkerThreadPool(pool);

        struct State {
            std::atomic<int> work{0};
            std::atomic<int> done{0};
            std::string      error;
        } state;

        PluginxxString error{};
        f.manager->offload(
            f.provider.get(),
            +[](void* ud, const PluginxxCancelToken*, PluginxxString*) -> void* {
                ++static_cast<State*>(ud)->work;
                return nullptr;
            },
            +[](void* ud, int32_t, void*, const PluginxxStringView* error) {
                auto& state = *static_cast<State*>(ud);
                state.error = svToStr(error);
                ++state.done;
            },
            &state,
            &error
        );
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (state.done.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            f.drain();
            std::this_thread::sleep_for(1ms);
        }
        XX_TEST_EXPECT_EQ(state.work.load(), 1);
        XX_TEST_EXPECT_EQ(state.done.load(), 1);
        XX_TEST_EXPECT_TRUE(state.error.empty());
        pool->join();
    }

    /// executor 停止期间完成包必须保留，恢复并重新绑定 executor 后只提交一次。
    {
        RuntimeFixture f;
        CallbackState  cb{.fixture = &f};
        auto           op = f.operation();
        op->accept();
        op->setCallback(CallbackState::onOpDone, &cb);
        auto notify = op->notify();

        f.io.stop();
        std::thread worker([notify] {
            auto payload = std::string{"completion after restart"};
            auto view    = strToSv(payload);
            notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, &view);
        });
        worker.join();
        XX_TEST_EXPECT_FALSE(op->completed());
        XX_TEST_EXPECT_EQ(cb.calls, 0);
        // 完成包已产生但未提交：Operation 未终结、lease 仍被持有（关闭只能等到
        // 截止时间进入 CloseFailed），并且这个状态可观察。
        XX_TEST_EXPECT_TRUE(op->completionPending());
        // provider 持有自己的 lease，caller（互调方）另有 1 个：完成包未提交前
        // 两侧都不能归零，关闭因此只能等到截止时间。
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1});
        XX_TEST_EXPECT_EQ(f.caller->lifetime->leaseCount(), size_t{1});
        XX_TEST_EXPECT_TRUE(
            f.manager->runtime()->pendingOperationSummary().find("runtime regression")
            != std::string::npos
        );
        // 摘要标记"完成包待提交"形态, 关闭超时可据此区分"插件未 done"
        XX_TEST_EXPECT_TRUE(
            f.manager->runtime()->pendingOperationSummary().find("completion-pending")
            != std::string::npos
        );

        f.io.restart();
        f.manager->setIoExecutor(f.io.get_executor());
        f.drain();
        XX_TEST_EXPECT_TRUE(op->completed());
        XX_TEST_EXPECT_FALSE(op->completionPending());
        XX_TEST_EXPECT_EQ(cb.calls, 1);
        XX_TEST_EXPECT_EQ(cb.status, PLUGINXX_OPERATOR_OK);
        XX_TEST_EXPECT_EQ(cb.result, "completion after restart");
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->pendingOperationSummary().empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
        XX_TEST_EXPECT_EQ(f.caller->lifetime->leaseCount(), size_t{0});
    }

    /// cancel 与 done 并发竞速 —— 只产生一个终态、回调恰好一次；终态之后
    /// 到达的 cancel 不再进入插件（`OpCore` 用普通 mutex 保证锁内不调用插件）。
    {
        for (int round = 0; round < 32; ++round) {
            RuntimeFixture   f;
            CallbackState    cb{.fixture = &f};
            auto             op     = f.operation();
            auto             notify = op->notify();
            std::atomic<int> cancels{0};
            op->accept([&] {
                ++cancels;
            });
            op->setCallback(CallbackState::onOpDone, &cb);

            std::barrier start{3};
            std::thread  doneThread([&] {
                start.arrive_and_wait();
                notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
            });
            std::thread  cancelThread([&] {
                start.arrive_and_wait();
                cancelPluginOperation(op->handle());
            });
            start.arrive_and_wait();
            doneThread.join();
            cancelThread.join();
            f.drain();

            XX_TEST_EXPECT_TRUE(op->completed());
            XX_TEST_EXPECT_EQ(cb.calls, 1);
            XX_TEST_EXPECT_EQ(cb.status, PLUGINXX_OPERATOR_OK);
            const int cancelsAtTerminal = cancels.load();
            XX_TEST_EXPECT_TRUE(cancelsAtTerminal == 0 || cancelsAtTerminal == 1);

            /// 终态之后的 cancel 是空操作：不进入插件、不改变终态。
            cancelPluginOperation(op->handle());
            f.drain();
            XX_TEST_EXPECT_EQ(cancels.load(), cancelsAtTerminal);
            XX_TEST_EXPECT_TRUE(op->completed());
            XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
            XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
        }
    }

    /// executor 停止期间的取消请求不能丢失；恢复后取消和同步 done 仍 exactly-once。
    {
        RuntimeFixture f;
        CallbackState  cb{.fixture = &f};
        auto           op      = f.operation();
        auto           notify  = op->notify();
        int            cancels = 0;
        op->accept([&] {
            ++cancels;
            notify.done(notify.host_ud, PLUGINXX_OPERATOR_CANCELED, nullptr);
        });
        op->setCallback(CallbackState::onOpDone, &cb);

        f.io.stop();
        cancelPluginOperation(op->handle());
        XX_TEST_EXPECT_EQ(cancels, 0);
        XX_TEST_EXPECT_FALSE(op->completed());

        f.io.restart();
        f.manager->setIoExecutor(f.io.get_executor());
        f.drain();
        XX_TEST_EXPECT_EQ(cancels, 1);
        XX_TEST_EXPECT_TRUE(op->completed());
        XX_TEST_EXPECT_EQ(cb.calls, 1);
        XX_TEST_EXPECT_EQ(cb.status, PLUGINXX_OPERATOR_CANCELED);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
    }

    /// 最后一个 lease 在 executor 停止期间释放时，idle cleanup 也必须在恢复后执行一次。
    {
        RuntimeFixture f;
        auto           lease = pluginxx::InstanceLease::acquire(f.provider->lifetime);
        XX_TEST_EXPECT_TRUE(static_cast<bool>(lease));
        f.provider->lifetime->requestClose();
        bool cleaned = false;
        XX_TEST_EXPECT_TRUE(f.provider->lifetime->setIdleCleanup([&] {
            cleaned = true;
        }));

        f.io.stop();
        lease.reset();
        XX_TEST_EXPECT_FALSE(cleaned);
        f.io.restart();
        f.manager->setIoExecutor(f.io.get_executor());
        f.drain();
        XX_TEST_EXPECT_TRUE(cleaned);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// executor 已停止时，跨线程同步 ABI 调用必须快速失败，不能永久等待 future。
    {
        RuntimeFixture f;
        f.io.stop();
        std::promise<bool> returned;
        std::thread        worker([&] {
            try {
                (void)ioCallSync<int>(&*f.manager, [] {
                    return 7;
                });
                returned.set_value(false);
            } catch (const std::exception&) {
                returned.set_value(true);
            } catch (...) {
                returned.set_value(true);
            }
        });
        worker.join();
        XX_TEST_EXPECT_TRUE(returned.get_future().get());
    }

    /// 真实 worker 由事件释放；Closing 期间不能提前 idle，done 派发给插件
    /// (本夹具没有插件执行器 ⇒ 仍在 io 执行; 有执行器时在那条线程上)
    {
        auto           ctx = std::make_shared<agentxx::agent::AgentContext>();
        RuntimeFixture f(ctx);
        PluginxxString error{};

        struct State {
            std::promise<void> started, release;
            bool               done = false, protectedDuringCallback = false, onIo = false;
            RuntimeFixture*    fixture;
        } state{.fixture = &f};

        f.manager->offload(
            f.provider.get(),
            +[](void* ud, const PluginxxCancelToken*, PluginxxString*) -> void* {
                auto& state = *static_cast<State*>(ud);
                state.started.set_value();
                state.release.get_future().wait();
                return ud;
            },
            +[](void* ud, int32_t status, void* value, const PluginxxStringView*) {
                auto& state = *static_cast<State*>(ud);
                state.done  = status == PLUGINXX_OPERATOR_OK && value == ud;
                state.onIo  = state.fixture->manager->isIoThread()
                              || state.fixture->manager->isPluginThread();
                // 派发本身也持一份 lease (op 一份 + 派发项一份), 因此这里只判 "非零"
                state.protectedDuringCallback
                    = state.fixture->provider->lifetime->leaseCount() != 0;
            },
            &state,
            &error
        );
        state.started.get_future().wait();
        f.provider->lifetime->requestClose();
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1});
        state.release.set_value();
        ctx->threadPool->join();
        f.drain();
        XX_TEST_EXPECT_TRUE(state.done && state.onIo && state.protectedDuringCallback);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// stop 事务未完成时, 同步 shutdownAll 不得 destroy/dlclose: 实例、上下文与
    /// 动态库全部保留, 状态标记 CloseFailed, 交由仍在运行的 owner 收尾。
    {
        RuntimeFixture f;
        gLifecycleStops    = 0;
        gLifecycleDestroys = 0;
        installLifecycleHooks(*f.provider, &gLifecycleDestroys);
        auto lease = pluginxx::InstanceLease::acquire(f.provider->lifetime);
        XX_TEST_EXPECT_TRUE(static_cast<bool>(lease));

        f.manager->shutdownAll();
        XX_TEST_EXPECT_EQ(gLifecycleStops, 0);
        XX_TEST_EXPECT_EQ(gLifecycleDestroys, 0);
        XX_TEST_EXPECT_EQ(
            f.provider->lifetime->state(),
            pluginxx::PluginInstanceState::CloseFailed
        );
        XX_TEST_EXPECT_TRUE(f.manager->find("provider") != nullptr);
        XX_TEST_EXPECT_TRUE(f.manager->hasPendingClose());

        // 最后一个 lease 释放 (含 idle 通知) 之后仍然不许 destroy。
        lease.reset();
        f.drain();
        XX_TEST_EXPECT_EQ(gLifecycleDestroys, 0);
        XX_TEST_EXPECT_TRUE(f.manager->find("provider") != nullptr);
    }

    /// shutdownAsync 是唯一能推进 stop 的 owner 路径: stop 恰好一次, 随后 destroy
    /// 恰好一次, 实例与动态库收尾后从表中移除。
    {
        RuntimeFixture f;
        gLifecycleStops    = 0;
        gLifecycleDestroys = 0;
        installLifecycleHooks(*f.provider, &gLifecycleDestroys);

        auto closed = asio::co_spawn(
            f.io,
            f.manager->shutdownAsync(std::chrono::seconds{5}),
            asio::use_future
        );
        f.io.run();
        XX_TEST_EXPECT_TRUE(closed.get());
        XX_TEST_EXPECT_EQ(gLifecycleStops, 1);
        XX_TEST_EXPECT_EQ(gLifecycleDestroys, 1);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->state(), pluginxx::PluginInstanceState::Closed);
        XX_TEST_EXPECT_TRUE(f.manager->find("provider") == nullptr);
        XX_TEST_EXPECT_FALSE(f.manager->hasPendingClose());
    }

    /// 直接构造的伪实例 (无 stop 入口) 仍走同步关闭: 加载路径已要求真实插件
    /// 必须导出 start/stop, 但关闭守卫不能因此变成无条件泄漏。
    {
        RuntimeFixture f;
        gLifecycleDestroys        = 0;
        f.provider->pluginCreated = true;
        f.provider->lifecycleDestroy = &fakeDestroyHook;
        f.provider->pluginCtx     = &gLifecycleDestroys;

        f.manager->shutdownAll();
        // destroy 在插件执行线程 / 工作池上跑: 有界等待, 不假设当步完成
        XX_TEST_EXPECT_TRUE(f.drainUntil([&] { return gLifecycleDestroys == 1; }));
        XX_TEST_EXPECT_EQ(gLifecycleDestroys, 1);
        XX_TEST_EXPECT_TRUE(f.manager->find("provider") == nullptr);
        XX_TEST_EXPECT_FALSE(f.manager->hasPendingClose());
    }

    /// 析构保护: stop 从未执行时绝不调用插件 destroy。
    {
        gLifecycleDestroys     = 0;
        auto inst              = std::make_shared<PluginInstance>("dtor_pending_stop");
        inst->lifecycleStop    = &fakeStopHook;
        inst->lifecycleStarted = true;
        inst->pluginCreated    = true;
        inst->lifecycleDestroy = &fakeDestroyHook;
        inst->pluginCtx        = &gLifecycleDestroys;
        XX_TEST_EXPECT_TRUE(inst->lifecycleStopPending());
        inst.reset();
        XX_TEST_EXPECT_EQ(gLifecycleDestroys, 0);
    }

    /// C17 ABI 编译期检查的 C++ 侧对照 —— C 翻译单元（-std=c17
    /// -pedantic-errors）与 C++ 看到的布局和版本号必须逐项一致；编号表见
    /// test_plugin_abi_c17.c。
    {
        XX_TEST_EXPECT_EQ(agentxx_test_abi_c_probe(), 0);

        struct AbiExpectation {
            int32_t  id;
            uint64_t expected;
        };

        const AbiExpectation expectations[] = {
            {1, sizeof(PluginxxStringView)},
            {2, offsetof(PluginxxStringView, size)},
            {3, sizeof(PluginxxHost)},
            {4, offsetof(PluginxxHost, opaque)},
            {5, sizeof(PluginxxHostVtable)},
            {6, sizeof(AgentxxPluginToolSpec)},
            {7, offsetof(AgentxxPluginToolSpec, execute_start)},
            {8, sizeof(PluginxxCapabilitySpec)},
            {9, sizeof(PluginxxOperatorNotify)},
            {10, sizeof(AgentxxPluginToolsIface)},
            {11, offsetof(AgentxxPluginToolsIface, struct_size)},
            {12, sizeof(PluginxxSchedulerIface)},
            {13, sizeof(PluginxxTasksIface)},
            {14, sizeof(AgentxxClientUiIface)},
            {15, PLUGINXX_API_VERSION},
            {16, AGENTXX_CLIENT_PLUGIN_API_VERSION},
            {17, AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION},
            {18, offsetof(PluginxxSchedulerIface, request_driver)},
            {19, offsetof(PluginxxSchedulerIface, cancel_driver)},
            {20, PLUGINXX_IFACE_CAPABILITIES_VERSION},
            {21, sizeof(AgentxxPluginToolPermissionSpec)},
            {22, offsetof(AgentxxPluginToolPermissionSpec, target_arg)},
            {23, sizeof(AgentxxPluginPermissionIface)},
            {24, AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_VERSION},
            {25, sizeof(AgentxxPluginPermissionPathQuery)},
            {26, offsetof(AgentxxPluginPermissionPathQuery, paths)},
            {27, AGENTXX_PLUGIN_PERMISSION_DECISION_ASK},
            {28, offsetof(AgentxxPluginToolPermissionSpec, struct_size)},
        };
        for (const auto& item : expectations) {
            XX_TEST_EXPECT_EQ(agentxx_test_abi_value(item.id), item.expected);
        }
        // 未登记的编号必须返回哨兵值（C 侧同样断言）。
        XX_TEST_EXPECT_EQ(agentxx_test_abi_value(9999), UINT64_MAX);
    }

    /// 版本对 + 基线判定 (plugin-version §3.2/§3.3) 与跨边界结构体长度门槛 (§3.5):
    /// - `validateInterface`: 版本低于最低 / 表长低于基线 → 拒绝; 更高版本 + 完整长度 → 接受;
    /// - `ifaceHas`: 表长覆盖某成员为真 (追加成员段的唯一判据), NULL 表为假;
    /// - `ifaceHasVersion`: 语义档位两态;
    /// - `specSizeOk`: `0` (基线布局) 与 `>= 基线` 接受, 低于基线拒绝;
    /// - `specHas`: 追加字段可读判定两态。
    {
        struct FakeTable {
            int32_t  version;
            uint32_t struct_size;
            void*    base_member;
            void*    added_member;
        };

        constexpr uint32_t kPtrBytes = static_cast<uint32_t>(sizeof(void*));
        const uint32_t     baseEnd
            = static_cast<uint32_t>(offsetof(FakeTable, base_member)) + kPtrBytes;
        const uint32_t addedEnd
            = static_cast<uint32_t>(offsetof(FakeTable, added_member)) + kPtrBytes;

        FakeTable table{};
        table.version = 1;
        // 只覆盖基线成员: 追加成员判为不可用
        table.struct_size = baseEnd;
        XX_TEST_EXPECT_TRUE(pluginxx::ifaceHas<&FakeTable::base_member>(&table));
        XX_TEST_EXPECT_TRUE(!pluginxx::ifaceHas<&FakeTable::added_member>(&table));
        // 覆盖到追加成员: 可用
        table.struct_size = addedEnd;
        XX_TEST_EXPECT_TRUE(pluginxx::ifaceHas<&FakeTable::added_member>(&table));
        XX_TEST_EXPECT_TRUE(
            !pluginxx::ifaceHas<&FakeTable::base_member>(static_cast<const FakeTable*>(nullptr))
        );
        // 语义档位
        XX_TEST_EXPECT_TRUE(pluginxx::ifaceHasVersion(&table, 1));
        XX_TEST_EXPECT_TRUE(!pluginxx::ifaceHasVersion(&table, 2));
        XX_TEST_EXPECT_TRUE(!pluginxx::ifaceHasVersion(static_cast<const FakeTable*>(nullptr), 1));

        // 结构体长度门槛: 0 = 基线布局 / 基线 / 低于基线
        XX_TEST_EXPECT_TRUE(agentxx::plugin::specSizeOk<FakeTable>(0));
        XX_TEST_EXPECT_TRUE(
            agentxx::plugin::specSizeOk<FakeTable>(static_cast<uint32_t>(sizeof(FakeTable)))
        );
        XX_TEST_EXPECT_TRUE(
            !agentxx::plugin::specSizeOk<FakeTable>(static_cast<uint32_t>(sizeof(FakeTable) - 1))
        );
        // 追加字段可读判定 (与 ifaceHas 同一思路, 长度由对方给)
        XX_TEST_EXPECT_TRUE(!agentxx::plugin::specHas(&FakeTable::added_member, baseEnd));
        XX_TEST_EXPECT_TRUE(agentxx::plugin::specHas(&FakeTable::added_member, addedEnd));
    }

    /// 跨边界结构体布局 (plugin-version §5.3):
    /// - 6 个"零成本"结构体: 用原 `_reserved` 的位置放 `struct_size`, 大小与关键偏移不变;
    /// - 3 个"+8"结构体: 插入 `struct_size` (首位或 `version` 之后);
    /// - 全部: `struct_size` 都是 8 字节对齐下的首字段 (或 `version` 之后)。
    {
        constexpr size_t kPtr = sizeof(void*);
        XX_TEST_EXPECT_EQ(offsetof(AgentxxPluginToolSpec, struct_size), size_t{0});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxPluginToolSpec, name), size_t{8});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxPluginToolSpec, execute_start), 8u + 3 * 16u);
        XX_TEST_EXPECT_EQ(
            offsetof(AgentxxPluginToolSpec, user_data),
            offsetof(AgentxxPluginToolSpec, execute_start) + 2 * kPtr
        );
        XX_TEST_EXPECT_EQ(
            offsetof(AgentxxPluginToolSpec, default_timeout_ms),
            offsetof(AgentxxPluginToolSpec, user_data) + kPtr
        );
        XX_TEST_EXPECT_EQ(
            offsetof(AgentxxPluginToolSpec, flags),
            offsetof(AgentxxPluginToolSpec, default_timeout_ms) + 8u
        );

        XX_TEST_EXPECT_EQ(offsetof(AgentxxPluginGraphNodeTypeSpec, struct_size), size_t{0});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxPluginGraphNodeTypeSpec, type), size_t{8});
        XX_TEST_EXPECT_EQ(
            offsetof(AgentxxPluginGraphNodeTypeSpec, run_start),
            offsetof(AgentxxPluginGraphNodeTypeSpec, type) + 16u
        );

        XX_TEST_EXPECT_EQ(offsetof(AgentxxOverlaySpec, version), size_t{0});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxOverlaySpec, struct_size), size_t{4});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxOverlaySpec, type), size_t{8});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxOverlaySpec, title), size_t{16});

        XX_TEST_EXPECT_EQ(offsetof(AgentxxToolRenderSpec, struct_size), size_t{0});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxToolRenderSpec, version), size_t{4});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxToolRenderSpec, tool_name), size_t{8});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxToolRenderSpec, display_name), size_t{24});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxToolRenderSpec, collapsed_fn), size_t{40});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxToolRenderSpec, expanded_fn), size_t{48});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxToolRenderSpec, user_data), size_t{56});
        XX_TEST_EXPECT_EQ(sizeof(AgentxxToolRenderSpec), size_t{64});

        XX_TEST_EXPECT_EQ(offsetof(AgentxxToolRenderInput, struct_size), size_t{0});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxToolRenderInput, version), size_t{4});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxToolRenderInput, tool_call_id), size_t{8});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxToolRenderInput, is_finished), 8u + 4 * 16u);
        XX_TEST_EXPECT_EQ(sizeof(AgentxxToolRenderInput), 8u + 4 * 16u + 16u);

        XX_TEST_EXPECT_EQ(offsetof(AgentxxUiActionContext, struct_size), size_t{0});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxUiActionContext, version), size_t{4});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxUiActionContext, owner_id), size_t{8});
        XX_TEST_EXPECT_EQ(sizeof(AgentxxUiActionContext), 8u + 3 * 16u);

        XX_TEST_EXPECT_EQ(offsetof(AgentxxTimerSpec, struct_size), size_t{0});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxTimerSpec, version), size_t{4});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxTimerSpec, interval_ms), size_t{8});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxTimerSpec, owner_id), size_t{24});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxTimerSpec, on_timer), 24u + 16u);

        XX_TEST_EXPECT_EQ(offsetof(AgentxxKeybindSpec, struct_size), size_t{0});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxKeybindSpec, version), size_t{4});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxKeybindSpec, keys), size_t{8});
        XX_TEST_EXPECT_EQ(offsetof(AgentxxKeybindSpec, on_keybind), 8u + 32u);

        // 绝对值只在 64 位环境断言 (32 位下指针宽度不同)
        if (kPtr == 8) {
            XX_TEST_EXPECT_EQ(sizeof(AgentxxPluginToolSpec), size_t{104});
            XX_TEST_EXPECT_EQ(sizeof(AgentxxPluginGraphNodeTypeSpec), size_t{64});
            XX_TEST_EXPECT_EQ(sizeof(AgentxxOverlaySpec), size_t{64});
            // 显示名 + 折叠/展开内容两个回调: 8 字节头 + 两个字符串视图 (32) +
            // 两个函数指针 + user_data = 64
            XX_TEST_EXPECT_EQ(sizeof(AgentxxToolRenderSpec), size_t{64});
            XX_TEST_EXPECT_EQ(sizeof(AgentxxTimerSpec), size_t{56});
            XX_TEST_EXPECT_EQ(sizeof(AgentxxKeybindSpec), size_t{56});
        }
    }

    /// 接口表协商 —— 判定规则为
    /// `version >= MIN_VERSION` 且 `struct_size >= BASELINE_SIZE` (见
    /// `pluginxx/api/iface_meta.h`): 低于最低版本、表长不足基线、NULL 表都必须被 SDK
    /// 拒绝; 声明了更高版本的完整表必须接受 (不再写死字面量 1)。
    {
        PluginxxStringView toolsIid  = PluginStringView::fromCstr(AGENTXX_PLUGIN_IFACE_AGENT_TOOLS);
        const auto*        realTools = static_cast<const AgentxxPluginToolsIface*>(
            agentxx::plugin::xx_query_interface(nullptr, &toolsIid)
        );
        XX_TEST_EXPECT_TRUE(realTools != nullptr);

        FakeIfaceHost fake;
        fake.host.vtable = &g_fakeIfaceVtable;
        fake.host.opaque = &fake;

        fake.table = realTools;
        XX_TEST_EXPECT_TRUE(AgentIfaces::query(&fake.host).tools != nullptr);

        // 更高版本的完整表: 接受 (修掉"一张表升版连累所有表"的写法)
        AgentxxPluginToolsIface newerVersion = *realTools;
        newerVersion.version                 = AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION + 1;
        fake.table                           = &newerVersion;
        XX_TEST_EXPECT_TRUE(AgentIfaces::query(&fake.host).tools != nullptr);

        // 低于最低版本: 拒绝
        AgentxxPluginToolsIface oldVersion = *realTools;
        oldVersion.version                 = AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_MIN_VERSION - 1;
        fake.table                         = &oldVersion;
        XX_TEST_EXPECT_TRUE(AgentIfaces::query(&fake.host).tools == nullptr);

        // 表长不足基线 (模拟老主程序): 拒绝
        AgentxxPluginToolsIface shortTable = *realTools;
        shortTable.struct_size = static_cast<uint32_t>(sizeof(AgentxxPluginToolsIface) - 8);
        fake.table             = &shortTable;
        XX_TEST_EXPECT_TRUE(AgentIfaces::query(&fake.host).tools == nullptr);

        // 表长等于基线: 接受
        AgentxxPluginToolsIface baselineTable = *realTools;
        baselineTable.struct_size
            = static_cast<uint32_t>(AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_BASELINE_SIZE);
        fake.table = &baselineTable;
        XX_TEST_EXPECT_TRUE(AgentIfaces::query(&fake.host).tools != nullptr);

        fake.table = nullptr;
        XX_TEST_EXPECT_TRUE(AgentIfaces::query(&fake.host).tools == nullptr);
    }

    /// 主程序控制块 —— 实例关闭后，插件保存的旧 host 指针仍然可读，但所有
    /// vtable 入口安全失败；同名新实例使用新令牌，旧指针绝不转交到新实例。
    {
        RuntimeFixture f;
        const auto*    host = f.provider->hostView();
        XX_TEST_EXPECT_TRUE(host != nullptr);
        XX_TEST_EXPECT_TRUE(host->opaque != nullptr);
        const auto ifaces = AgentIfaces::query(host);
        XX_TEST_EXPECT_TRUE(ifaces.config != nullptr);

        // config.get_plugin_args 是异步操作契约: 受理 + 完成回调, 这里阻塞等结果
        auto argsOf = [&](const PluginxxHost* target) {
            return agentxx::test::tableOpWait(
                target,
                ifaces.cancel,
                ifaces.scheduler,
                [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                    return ifaces.config->get_plugin_args(target, cb, ud, err);
                }
            );
        };
        XX_TEST_EXPECT_TRUE(argsOf(host).ok());

        // 真实路径里由 destroyPlugin 退休控制块；这里直接触发同一动作。
        f.provider->retireHostControl();
        XX_TEST_EXPECT_FALSE(argsOf(host).ok());

        // 卸载后重新加载同名实例：旧 host 指针保持失效，不会命中新实例。
        auto fresh = f.instance("provider", 77);
        XX_TEST_EXPECT_TRUE(fresh->hostView() != host);
        XX_TEST_EXPECT_TRUE(fresh->hostView()->opaque != host->opaque);
        XX_TEST_EXPECT_FALSE(argsOf(host).ok());
        XX_TEST_EXPECT_TRUE(argsOf(fresh->hostView()).ok());
    }

    /// prompt 贡献模型 —— 多 owner 叠加、卸载只移除自己的贡献、不把
    /// 已卸载 owner 的旧值写回，用户外部修改后卸载保留用户值。
    {
        auto ctx         = std::make_shared<agentxx::agent::AgentContext>();
        ctx->agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
        RuntimeFixture    f(ctx);
        auto&             prompt  = ctx->agentConfig->prompt;
        const std::string baseSys = prompt.systemPrompt;

        // 1) 两个 owner 依次写同一个 append 键：后者生效
        XX_TEST_EXPECT_EQ(
            f.mount->setPromptJson(f.provider.get(), R"({"appendSystemPrompts":{"demo":"A"}})"),
            0
        );
        XX_TEST_EXPECT_EQ(prompt.appendSystemPrompts["demo"], std::string{"A"});
        XX_TEST_EXPECT_EQ(
            f.mount->setPromptJson(f.caller.get(), R"({"appendSystemPrompts":{"demo":"B"}})"),
            0
        );
        XX_TEST_EXPECT_EQ(prompt.appendSystemPrompts["demo"], std::string{"B"});

        // 2) 先卸载写 "A" 的 owner：只剩 caller 的贡献
        f.mount->removePromptContributions(f.provider->name);
        XX_TEST_EXPECT_EQ(prompt.appendSystemPrompts["demo"], std::string{"B"});

        // 3) 再卸载 caller：回到基础值（原本不存在 -> 键删除），不写回已离开的 "A"
        f.mount->removePromptContributions(f.caller->name);
        XX_TEST_EXPECT_TRUE(
            prompt.appendSystemPrompts.find("demo") == prompt.appendSystemPrompts.end()
        );

        // 4) 外部（用户）修改后卸载：保留用户值
        XX_TEST_EXPECT_EQ(
            f.mount->setPromptJson(f.provider.get(), R"({"appendSystemPrompts":{"u":"plugin"}})"),
            0
        );
        prompt.appendSystemPrompts["u"] = "user";
        f.mount->removePromptContributions(f.provider->name);
        XX_TEST_EXPECT_EQ(prompt.appendSystemPrompts["u"], std::string{"user"});
        prompt.appendSystemPrompts.erase("u");

        // 5) systemPrompt 卸载回到基础值
        XX_TEST_EXPECT_EQ(
            f.mount->setPromptJson(f.provider.get(), R"({"systemPrompt":"plugin system"})"),
            0
        );
        XX_TEST_EXPECT_EQ(prompt.systemPrompt, std::string{"plugin system"});
        f.mount->removePromptContributions(f.provider->name);
        XX_TEST_EXPECT_EQ(prompt.systemPrompt, baseSys);

        // 6) toolPrompt：部分覆盖语义 + 卸载删除新增键
        XX_TEST_EXPECT_EQ(
            f.mount->setPromptJson(
                f.provider.get(),
                R"({"toolPrompt":{"demo_tool":{"depict":"D","args":{"a":"1"}}}})"
            ),
            0
        );
        XX_TEST_EXPECT_EQ(prompt.toolPrompt["demo_tool"].depict, std::string{"D"});
        XX_TEST_EXPECT_EQ(prompt.toolPrompt["demo_tool"].args["a"], std::string{"1"});
        XX_TEST_EXPECT_EQ(
            f.mount->setPromptJson(
                f.provider.get(),
                R"({"toolPrompt":{"demo_tool":{"args":{"b":"2"}}}})"
            ),
            0
        );
        XX_TEST_EXPECT_EQ(prompt.toolPrompt["demo_tool"].depict, std::string{"D"});
        XX_TEST_EXPECT_EQ(prompt.toolPrompt["demo_tool"].args["b"], std::string{"2"});
        f.mount->removePromptContributions(f.provider->name);
        XX_TEST_EXPECT_TRUE(prompt.toolPrompt.find("demo_tool") == prompt.toolPrompt.end());
    }

    /// 禁用/启用事务（导出 start/stop 的插件）—— 主程序侧立即摘除注册，
    /// stop 在 IO 线程撤销插件自管注册，start 重新声明；stop 成功后清空旧记录，
    /// 因此重复 enable/disable 不会累积重复项。
    {
        // 图类型注册需要主程序 graphRegistry (AgentContext 加载)。
        auto           ctx = std::make_shared<agentxx::agent::AgentContext>();
        RuntimeFixture f(ctx);
        ctx->graphRegistry = std::make_shared<neograph::graph::GraphRegistry>();
        auto inst          = f.instance("lifecycle_plugin", 7);
        auto probe         = std::make_shared<LifecycleProbe>();
        probe->host        = inst->hostView();
        probe->tools       = AgentIfaces::query(probe->host).tools;
        probe->graph       = AgentIfaces::query(probe->host).graph;
        probe->cancel      = AgentIfaces::query(probe->host).cancel;
        probe->scheduler   = AgentIfaces::query(probe->host).scheduler;
        probe->spec        = fakeTool(nullptr, false);
        probe->agentId     = f.manager->agentId();
        probe->ref.struct_size = sizeof(PluginxxScopeRef);
        probe->ref.id          = PluginStringView::from(probe->agentId);
        probe->ref.seq         = f.manager->mountSequence();
        XX_TEST_EXPECT_TRUE(probe->tools != nullptr);
        if (!probe->tools) {
            return result;
        }
        XX_TEST_EXPECT_TRUE(probe->graph != nullptr);

        inst->lifecycleCreate  = &lifecycleCreateHook;
        inst->lifecycleStop    = &lifecycleStopHook;
        inst->pluginCtx        = probe.get();
        inst->lifecycleStarted = true;
        const int baseStarts   = probe->starts;
        const int baseStops    = probe->stops;

        // 加载时的初始注册（模拟 create/start 已完成）
        XX_TEST_EXPECT_EQ(f.mount->registerTool(inst.get(), &probe->spec), 0);
        XX_TEST_EXPECT_TRUE(f.manager->registry()->contains("runtime_tool"));

        // ---- disable: 主程序侧摘除 + stop 事务已走完 (disable 返回即已落定) ----
        const bool disabledOk
            = agentxx::test::runPluginCoroutine(
                f.manager->ioExecutor(),
                f.manager->disable("lifecycle_plugin")
            );
        XX_TEST_EXPECT_TRUE(disabledOk);
        XX_TEST_EXPECT_FALSE(inst->enabled);
        XX_TEST_EXPECT_TRUE(inst->userDisabled);
        XX_TEST_EXPECT_EQ(
            static_cast<int>(inst->lifetime->state()),
            static_cast<int>(pluginxx::PluginInstanceState::Disabled)
        );
        XX_TEST_EXPECT_FALSE(f.manager->registry()->contains("runtime_tool"));
        // 启停已是可等待的协程: 返回时 stop 事务已完成 (不再“投递了就不管”)
        XX_TEST_EXPECT_TRUE(inst->lifecycleStopped);
        XX_TEST_EXPECT_EQ(probe->stops, baseStops + 1);
        f.drainAll();
        XX_TEST_EXPECT_TRUE(inst->allToolNames().empty()); // 记录已清空，等待 start 重新声明

        // ---- enable: create 事务重新注册 ----
        const bool enabledOk
            = agentxx::test::runPluginCoroutine(
                f.manager->ioExecutor(),
                f.manager->enable("lifecycle_plugin")
            );
        XX_TEST_EXPECT_TRUE(enabledOk);
        XX_TEST_EXPECT_TRUE(inst->enabled);
        XX_TEST_EXPECT_FALSE(inst->userDisabled);
        XX_TEST_EXPECT_EQ(probe->starts, baseStarts + 1);
        f.drainAll();
        XX_TEST_EXPECT_FALSE(inst->lifecycleStopped);
        XX_TEST_EXPECT_EQ(
            static_cast<int>(inst->lifetime->state()),
            static_cast<int>(pluginxx::PluginInstanceState::Ready)
        );
        XX_TEST_EXPECT_TRUE(f.manager->registry()->contains("runtime_tool"));
        XX_TEST_EXPECT_EQ(inst->allToolNames().size(), size_t{1});
        XX_TEST_EXPECT_EQ(inst->allGraphSlots().size(), size_t{1});
        auto graphSlot = inst->allGraphSlots().empty() ? std::shared_ptr<GraphTypeSlot>{}
                                                      : inst->allGraphSlots().front();
        XX_TEST_EXPECT_TRUE(graphSlot != nullptr);
        XX_TEST_EXPECT_TRUE(graphSlot && graphSlot->snapshot().active);

        // ---- 重复 enable/disable 不累积重复项 ----
        for (int i = 0; i < 3; ++i) {
            agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->disable("lifecycle_plugin"));
            f.drainAll();
            agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->enable("lifecycle_plugin"));
            f.drainAll();
        }
        XX_TEST_EXPECT_EQ(probe->starts, baseStarts + 4);
        XX_TEST_EXPECT_EQ(probe->stops, baseStops + 4);
        XX_TEST_EXPECT_EQ(inst->allToolNames().size(), size_t{1});
        XX_TEST_EXPECT_EQ(inst->allToolObjects().size(), size_t{1});

        // ---- 禁用后立刻启用（stop 事务仍在队列）: 先 stop 再 start ----
        agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->disable("lifecycle_plugin"));
        agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->enable("lifecycle_plugin"));
        f.drainAll();
        XX_TEST_EXPECT_TRUE(inst->enabled);
        XX_TEST_EXPECT_FALSE(inst->lifecycleStopped);
        XX_TEST_EXPECT_TRUE(f.manager->registry()->contains("runtime_tool"));
        XX_TEST_EXPECT_EQ(inst->allToolNames().size(), size_t{1});
        XX_TEST_EXPECT_EQ(probe->starts, baseStarts + 5);

        // ---- start 失败: 回到 Disabled、不留部分注册；再次启用可恢复 ----
        probe->failStart = true;
        agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->disable("lifecycle_plugin"));
        f.drainAll();
        const int startsBeforeFail      = probe->starts;
        probe->partialRegistration      = false;
        probe->partialGraphRegistration = false;
        agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->enable("lifecycle_plugin"));
        f.drainAll();
        XX_TEST_EXPECT_EQ(probe->starts, startsBeforeFail + 1);
        XX_TEST_EXPECT_TRUE(probe->partialRegistration);      // start 确实留下过部分注册
        XX_TEST_EXPECT_TRUE(probe->partialGraphRegistration); // 图类型也登记过
        XX_TEST_EXPECT_FALSE(inst->enabled);
        XX_TEST_EXPECT_EQ(
            static_cast<int>(inst->lifetime->state()),
            static_cast<int>(pluginxx::PluginInstanceState::Disabled)
        );
        XX_TEST_EXPECT_FALSE(f.manager->registry()->contains("runtime_tool")); // 已回滚
        XX_TEST_EXPECT_TRUE(inst->allToolNames().empty());
        XX_TEST_EXPECT_TRUE(inst->allGraphSlots().empty()); // 图注册记录同样回滚
        XX_TEST_EXPECT_TRUE(graphSlot && !graphSlot->snapshot().active); // 槽位失效

        probe->failStart = false;
        agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->enable("lifecycle_plugin"));
        f.drainAll();
        XX_TEST_EXPECT_TRUE(inst->enabled);
        XX_TEST_EXPECT_EQ(
            static_cast<int>(inst->lifetime->state()),
            static_cast<int>(pluginxx::PluginInstanceState::Ready)
        );
        XX_TEST_EXPECT_TRUE(f.manager->registry()->contains("runtime_tool"));
        XX_TEST_EXPECT_EQ(inst->allGraphSlots().size(), size_t{1});
        XX_TEST_EXPECT_TRUE(graphSlot && graphSlot->snapshot().active); // 重新激活

        // ---- 关闭流程中的实例不接受启用状态变化 ----
        inst->lifetime->requestClose();
        agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->disable("lifecycle_plugin"));
        XX_TEST_EXPECT_TRUE(inst->enabled);
        XX_TEST_EXPECT_EQ(
            static_cast<int>(inst->lifetime->state()),
            static_cast<int>(pluginxx::PluginInstanceState::Closing)
        );

        // ---- 关闭收尾: 欠着的 stop 由卸载路径补齐, 之后才能 destroy ----
        const int stopsBeforeUnload = probe->stops;
        bool      unloaded          = false;
        {
            // use_future + 有界推进 IO: 不假设固定轮数 poll 已跑完整轮事务
            auto rc = f.waitFutureValue(asio::co_spawn(
                f.io,
                f.manager->unloadAsync("lifecycle_plugin", 500ms),
                asio::use_future
            ));
            XX_TEST_EXPECT_TRUE(rc.has_value());
            unloaded = rc.value_or(false);
        }
        XX_TEST_EXPECT_TRUE(unloaded);
        XX_TEST_EXPECT_EQ(probe->stops, stopsBeforeUnload + 1);
        XX_TEST_EXPECT_TRUE(inst->pluginDestroyed);
        XX_TEST_EXPECT_EQ(
            static_cast<int>(inst->lifetime->state()),
            static_cast<int>(pluginxx::PluginInstanceState::Closed)
        );
        XX_TEST_EXPECT_TRUE(f.manager->find("lifecycle_plugin") == nullptr);
    }

    /// 依赖级联 —— 三级与菱形依赖的禁用/恢复；用户显式禁用不被级联恢复。
    {
        RuntimeFixture f;
        auto           leaf = f.instance("leaf", 20);
        auto           mid  = f.instance("mid", 21);
        auto           side = f.instance("side", 22);
        auto           top  = f.instance("top", 23);
        mid->depends        = {"leaf"};
        side->depends       = {"leaf"};
        top->depends        = {"mid", "side"}; // 菱形: top 经 mid/side 两级依赖 leaf

        // ---- 三级 + 菱形级联禁用 ----
        agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->disable("leaf"));
        XX_TEST_EXPECT_FALSE(leaf->enabled);
        XX_TEST_EXPECT_TRUE(leaf->userDisabled);
        XX_TEST_EXPECT_FALSE(mid->enabled);
        XX_TEST_EXPECT_TRUE(mid->blockedByDependencies);
        XX_TEST_EXPECT_FALSE(side->enabled);
        XX_TEST_EXPECT_TRUE(side->blockedByDependencies);
        XX_TEST_EXPECT_FALSE(top->enabled); // 三级依赖也被级联
        XX_TEST_EXPECT_TRUE(top->blockedByDependencies);
        XX_TEST_EXPECT_FALSE(top->userDisabled);

        // ---- 启用按依赖顺序恢复全部被级联禁用的插件 ----
        agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->enable("leaf"));
        XX_TEST_EXPECT_TRUE(leaf->enabled);
        XX_TEST_EXPECT_TRUE(mid->enabled);
        XX_TEST_EXPECT_TRUE(side->enabled);
        XX_TEST_EXPECT_TRUE(top->enabled);
        XX_TEST_EXPECT_FALSE(mid->blockedByDependencies);
        XX_TEST_EXPECT_FALSE(top->blockedByDependencies);

        // ---- 用户显式禁用的插件不被级联恢复 ----
        agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->disable("top"));
        XX_TEST_EXPECT_TRUE(top->userDisabled);
        agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->disable("leaf"));
        XX_TEST_EXPECT_FALSE(mid->enabled);
        XX_TEST_EXPECT_TRUE(mid->blockedByDependencies);
        XX_TEST_EXPECT_TRUE(top->userDisabled); // 用户禁用标记不被级联改写
        agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->enable("leaf"));
        XX_TEST_EXPECT_TRUE(mid->enabled);
        XX_TEST_EXPECT_TRUE(side->enabled);
        XX_TEST_EXPECT_FALSE(top->enabled); // 用户显式禁用: 级联不恢复
        XX_TEST_EXPECT_TRUE(top->userDisabled);
        agentxx::test::runPluginCoroutine(f.manager->ioExecutor(), f.manager->enable("top")); // 用户显式启用
        XX_TEST_EXPECT_TRUE(top->enabled);
        XX_TEST_EXPECT_FALSE(top->userDisabled);
    }

    /// 注册类入口的执行期复查 —— 排队期间实例进入 Closing 时，注册必须被
    /// 拒绝，注册表与实例记录都不留下残留。
    {
        RuntimeFixture f;
        auto           spec = fakeTool(nullptr, false);
        XX_TEST_EXPECT_EQ(f.mount->registerTool(f.provider.get(), &spec), 0);
        XX_TEST_EXPECT_EQ(f.mount->unregisterTool(f.provider.get(), "runtime_tool"), 0);
        XX_TEST_EXPECT_TRUE(f.provider->allToolNames().empty());

        f.provider->lifetime->requestClose();
        XX_TEST_EXPECT_TRUE(f.mount->registerTool(f.provider.get(), &spec) != 0);
        XX_TEST_EXPECT_FALSE(f.manager->registry()->contains("runtime_tool"));
        XX_TEST_EXPECT_TRUE(f.provider->allToolNames().empty());
    }

    /// 完整 vtable 路径 —— 工作线程发起注册，随后实例开始关闭。请求要么在
    /// 入口被拒绝，要么排队到 IO 线程后由执行期复查拒绝；两种顺序都必须无残留、
    /// 不阻塞调用线程、lease 归零。
    {
        RuntimeFixture f;
        const auto*    host   = f.provider->hostView();
        const auto     ifaces = AgentIfaces::query(host);
        XX_TEST_EXPECT_TRUE(ifaces.tools != nullptr);

        // promise/spec 由 shared_ptr 持有：请求若始终未被执行，线程可在不访问
        // 悬垂栈对象的前提下结束（正常情况下会走 join）。
        auto        spec      = std::make_shared<AgentxxPluginToolSpec>(fakeTool(nullptr, false));
        auto        entered   = std::make_shared<std::promise<void>>();
        auto        rcPromise = std::make_shared<std::promise<int>>();
        auto        rcFuture  = rcPromise->get_future();
        // 挂载引用由值捕获带入工作线程 (raw() 指向自身, 因此不能捕指针)
        const agentxx::plugin::ScopeRef ref{"root", 0};
        std::thread pluginThread([host, ifaces, spec, entered, rcPromise, ref] {
            entered->set_value();
            // 表成员是异步契约: 工作线程上用阻塞形态等结果 (入口拒绝或受理后失败)
            auto res = agentxx::test::tableOpWait(
                host,
                ifaces.cancel,
                ifaces.scheduler,
                [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                    return ifaces.tools->register_tool(
                        host,
                        ref.ptr(),
                        spec.get(),
                        cb,
                        ud,
                        err
                    );
                }
            );
            rcPromise->set_value(res.ok() ? 0 : -1);
        });
        entered->get_future().wait();
        f.provider->lifetime->requestClose();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (rcFuture.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready
               && std::chrono::steady_clock::now() < deadline) {
            f.io.restart();
            f.io.run_for(std::chrono::milliseconds{2});
        }
        const bool finished
            = rcFuture.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready;
        XX_TEST_EXPECT_TRUE(finished);
        const int rc = finished ? rcFuture.get() : 0;
        if (finished) {
            pluginThread.join();
        } else {
            // 请求未终结说明实现违约（挂起）；分离线程避免测试进程被永久阻塞。
            pluginThread.detach();
        }

        XX_TEST_EXPECT_TRUE(rc != 0);
        XX_TEST_EXPECT_FALSE(f.manager->registry()->contains("runtime_tool"));
        XX_TEST_EXPECT_TRUE(f.provider->allToolNames().empty());
        f.drain();
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// plugin.md 11.2-5: caller 卸载与 provider 未完成互调 —— 正在关闭的 caller
    /// 不能提前 destroy/dlclose：卸载等待其 lease 超时进入 CloseFailed 并保留
    /// 上下文；provider 完成后 callback 仍持有 caller lease，callback 返回后
    /// 才允许收尾（重试成功）。
    {
        RuntimeFixture f;
        CallbackState  cb{.fixture = &f};
        int            callerDestroys = 0;
        installLifecycleHooks(*f.caller, &callerDestroys);

        auto op = f.operation();
        op->accept();
        op->setCallback(CallbackState::onOpDone, &cb);
        XX_TEST_EXPECT_FALSE(op->completed());
        XX_TEST_EXPECT_EQ(f.caller->lifetime->leaseCount(), size_t{1});

        // provider 未完成时 caller 开始关闭: 等待 lease 超时 -> CloseFailed,
        // 插件上下文与动态库保留 (destroy 未调用)。
        bool firstUnload = true;
        {
            auto rc = f.waitFutureValue(
                asio::co_spawn(f.io, f.manager->unloadAsync("caller", 0ms), asio::use_future)
            );
            XX_TEST_EXPECT_TRUE(rc.has_value());
            firstUnload = rc.value_or(true);
        }
        XX_TEST_EXPECT_FALSE(firstUnload);
        XX_TEST_EXPECT_EQ(f.caller->lifetime->state(), pluginxx::PluginInstanceState::CloseFailed);
        XX_TEST_EXPECT_EQ(callerDestroys, 0);
        XX_TEST_EXPECT_FALSE(f.caller->pluginDestroyed);
        XX_TEST_EXPECT_EQ(f.caller->lifetime->leaseCount(), size_t{1});

        // provider 完成: callback 必须在 caller lease 保护下于 IO 线程执行。
        auto notify = op->notify();
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        f.drainAll();
        XX_TEST_EXPECT_EQ(cb.calls, 1);
        XX_TEST_EXPECT_TRUE(cb.protectedDuringCallback && cb.onIo);
        XX_TEST_EXPECT_EQ(f.caller->lifetime->leaseCount(), size_t{0});
        XX_TEST_EXPECT_EQ(callerDestroys, 0);

        // 现在才能完成 caller 的关闭 (可重试), destroy 恰好一次。
        bool secondUnload = false;
        {
            auto rc = f.waitFutureValue(
                asio::co_spawn(f.io, f.manager->unloadAsync("caller", 500ms), asio::use_future)
            );
            XX_TEST_EXPECT_TRUE(rc.has_value());
            secondUnload = rc.value_or(false);
        }
        XX_TEST_EXPECT_TRUE(secondUnload);
        XX_TEST_EXPECT_EQ(callerDestroys, 1);
        XX_TEST_EXPECT_TRUE(f.caller->pluginDestroyed);
        XX_TEST_EXPECT_EQ(f.caller->lifetime->state(), pluginxx::PluginInstanceState::Closed);
        XX_TEST_EXPECT_TRUE(f.manager->find("caller") == nullptr);
    }

    /// plugin.md 11.2-8: shutdown 中后台 Task 挂起 —— 取消唤醒协程、协程返回前
    /// 提交 exactly-once done、回调返回且 lease 归零后才执行 destroy。
    /// 顺序: cancel → resume/done → callback → completion → destroy。
    {
        RuntimeFixture f;
        int            destroys = 0;
        installLifecycleHooks(*f.provider, &destroys);
        OrderLog           order;
        ShutdownOrderProbe probe{
            .order    = &order,
            .provider = f.provider.get(),
            .manager  = f.manager.get()
        };
        auto op     = f.operation();
        auto notify = op->notify();
        // 模拟挂起的后台任务: 取消只负责唤醒, 任务在自己的线程上恢复并提交 done。
        std::promise<void> cancelSignal;
        auto               cancelFuture = cancelSignal.get_future();
        // 测试确认"卸载仍停在 lease 等待上"之前, 任务不提交 done。
        std::promise<void> allowDone;
        auto               allowDoneFuture = allowDone.get_future();
        std::thread        task([&] {
            cancelFuture.wait();
            order.push("resumed");
            allowDoneFuture.wait();
            notify.done(notify.host_ud, PLUGINXX_OPERATOR_CANCELED, nullptr);
            order.push("doneSubmitted");
        });
        op->accept([&] {
            order.push("cancel");
            cancelSignal.set_value();
        });
        op->setCallback(ShutdownOrderProbe::record, &probe);
        op->setCompletionHandler([&](int32_t, std::string_view) {
            order.push("completion");
        });
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1});

        bool unloaded = false;
        auto unloadFuture
            = asio::co_spawn(f.io, f.manager->unloadAsync("provider", 5s), asio::use_future);
        f.drainAll();
        // 关闭先取消后台任务 (detachAll → cancel), 任务尚未退出: 卸载停在
        // lease 等待上 (协程未结束, unloaded 保持初值), destroy 未发生。
        XX_TEST_EXPECT_FALSE(unloaded);
        XX_TEST_EXPECT_FALSE(f.provider->pluginDestroyed);
        XX_TEST_EXPECT_EQ(destroys, 0);
        XX_TEST_EXPECT_EQ(op->state(), PluginOperationState::Cancelling);
        XX_TEST_EXPECT_TRUE(order.size() >= 1);
        XX_TEST_EXPECT_EQ(order.at(0), std::string{"cancel"});

        // 任务在自有线程恢复并提交 done; 主程序回收 lease 与句柄, 卸载随后继续。
        allowDone.set_value();
        task.join();
        XX_TEST_EXPECT_TRUE(f.drainUntil([&] {
            return op->completed();
        }));
        XX_TEST_EXPECT_TRUE(op->completed());
        XX_TEST_EXPECT_EQ(op->status(), PLUGINXX_OPERATOR_CANCELED);
        XX_TEST_EXPECT_TRUE(probe.onIo);
        XX_TEST_EXPECT_FALSE(probe.destroyDuringCallback);
        XX_TEST_EXPECT_EQ(order.size(), size_t{5});
        XX_TEST_EXPECT_EQ(order.at(0), std::string{"cancel"});
        XX_TEST_EXPECT_EQ(order.at(1), std::string{"resumed"});
        XX_TEST_EXPECT_EQ(order.at(2), std::string{"doneSubmitted"});
        // 配了插件执行器时完成回调是**投递**到那条线程的, 主程序的完成处理器
        // (清索引/记账) 在 io 上就地跑 —— 因此 completion 可能先于 callback。
        // 两者都不应在 destroy 之前或之后错位 (见下一行与上面的 destroy 断言)。
        const bool callbackThenCompletion
            = order.at(3) == std::string{"callback"} && order.at(4) == std::string{"completion"};
        const bool completionThenCallback
            = order.at(3) == std::string{"completion"} && order.at(4) == std::string{"callback"};
        XX_TEST_EXPECT_TRUE(callbackThenCompletion || completionThenCallback);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});

        // lease 归零后销毁才发生 (destroy 不能早于 callback 与 done)。
        // 卸载协程在 lease 归零后才继续: 这里显式等待它结束再取结果。
        XX_TEST_EXPECT_TRUE(f.waitFutureReady(unloadFuture));
        unloaded = unloadFuture.get();
        XX_TEST_EXPECT_TRUE(unloaded);
        XX_TEST_EXPECT_EQ(destroys, 1);
        XX_TEST_EXPECT_TRUE(f.provider->pluginDestroyed);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->state(), pluginxx::PluginInstanceState::Closed);
        XX_TEST_EXPECT_TRUE(f.manager->find("provider") == nullptr);
    }

    /// plugin.md 11.2-9: 卸载超时后立即重试 —— 未完成的插件执行不能被跳过;
    /// 重试继续等待同一 lease, 只有执行真正退出 (done 提交) 后才 destroy/dlclose。
    {
        RuntimeFixture f;
        int            destroys = 0;
        installLifecycleHooks(*f.provider, &destroys);
        auto op = f.operation();
        op->accept();
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1});

        bool first = true;
        {
            // 用 use_future 明确等待本轮卸载协程结束 (而不是假设固定轮数 poll 已跑完)
            auto rc = f.waitFutureValue(
                asio::co_spawn(f.io, f.manager->unloadAsync("provider", 0ms), asio::use_future)
            );
            XX_TEST_EXPECT_TRUE(rc.has_value());
            first = rc.value_or(true);
        }
        XX_TEST_EXPECT_FALSE(first);
        XX_TEST_EXPECT_EQ(
            f.provider->lifetime->state(),
            pluginxx::PluginInstanceState::CloseFailed
        );
        XX_TEST_EXPECT_EQ(destroys, 0);
        XX_TEST_EXPECT_FALSE(f.provider->pluginDestroyed);
        XX_TEST_EXPECT_EQ(f.manager->runtime()->operations.size(), size_t{1});

        // 超时后立即重试: 执行仍在运行, 第二次卸载依然只能拒绝收尾。
        bool second = true;
        {
            auto rc = f.waitFutureValue(
                asio::co_spawn(f.io, f.manager->unloadAsync("provider", 0ms), asio::use_future)
            );
            XX_TEST_EXPECT_TRUE(rc.has_value());
            second = rc.value_or(true);
        }
        XX_TEST_EXPECT_FALSE(second);
        XX_TEST_EXPECT_EQ(destroys, 0);
        XX_TEST_EXPECT_FALSE(f.provider->pluginDestroyed);
        XX_TEST_EXPECT_FALSE(op->completed());

        // 插件执行最终退出 (worker 线程提交完成包)。
        auto        notify = op->notify();
        std::thread worker([notify] {
            notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        });
        worker.join();
        XX_TEST_EXPECT_TRUE(f.drainUntil([&] {
            return op->completed();
        }));
        XX_TEST_EXPECT_TRUE(op->completed());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
        XX_TEST_EXPECT_EQ(destroys, 0);

        // 执行完全退出后重试成功; destroy 只在此时发生。
        bool third = false;
        {
            auto rc = f.waitFutureValue(
                asio::co_spawn(f.io, f.manager->unloadAsync("provider", 500ms), asio::use_future)
            );
            XX_TEST_EXPECT_TRUE(rc.has_value());
            third = rc.value_or(false);
        }
        XX_TEST_EXPECT_TRUE(third);
        XX_TEST_EXPECT_EQ(destroys, 1);
        XX_TEST_EXPECT_TRUE(f.provider->pluginDestroyed);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->state(), pluginxx::PluginInstanceState::Closed);
        XX_TEST_EXPECT_TRUE(f.manager->find("provider") == nullptr);
    }

    /// GraphTypeSlot 代次 —— 编译后的旧节点在卸载/重载后只返回
    /// "插件已关闭/代次失效", 不调用任何插件回调; 同一实例重新注册类型也只会
    /// 使旧节点失效, 新编译的节点才使用新回调, 旧节点不得转交新实例。
    {
        RuntimeFixture f;
        auto           slot     = std::make_shared<GraphTypeSlot>();
        std::string    typeName = "slot_graph_type";
        int            callsOld = 0, callsNew = 0;
        auto           makeSpec = [&](int* counter) {
            AgentxxPluginGraphNodeTypeSpec spec{};
            spec.type      = strToSv(typeName);
            spec.user_data = counter;
            spec.run_start = +[](void* ud,
                                 const PluginxxStringView*,
                                 const PluginxxStringView*,
                                 const PluginxxStringView*,
                                 const PluginxxStringView*,
                                 const PluginxxOperatorNotify* notify,
                                 PluginxxString*) -> void* {
                ++*static_cast<int*>(ud);
                auto payload = PluginStringView::fromCstr("{}");
                notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &payload);
                return nullptr;
            };
            return spec;
        };
        neograph::graph::GraphState state;
        neograph::graph::RunContext runCtx;
        auto runNode = [&](const std::shared_ptr<PluginGraphNode>& node) -> std::string {
            auto fut = asio::co_spawn(
                f.io,
                [&]() -> asio::awaitable<std::string> {
                    try {
                        co_await node->run(neograph::graph::NodeInput{state, runCtx});
                        co_return std::string{};
                    } catch (const std::exception& e) {
                        co_return std::string{e.what()};
                    }
                },
                asio::use_future
            );
            // 节点回调经插件执行线程完成: 单次 poll 不足, 由夹具持续推进 IO 并让出 CPU
            return f.waitFutureValue(std::move(fut)).value_or("<timeout>");
        };

        // 注册有效时节点正常执行插件回调。
        auto specOld = makeSpec(&callsOld);
        slot->activate(f.provider, specOld, f.provider->lifetime->generation());
        auto oldNode = std::make_shared<PluginGraphNode>(
            "slot_old",
            "{}",
            f.provider,
            specOld,
            slot,
            slot->snapshot().generation
        );
        XX_TEST_EXPECT_EQ(callsOld, 0);
        XX_TEST_EXPECT_TRUE(runNode(oldNode).empty());
        XX_TEST_EXPECT_EQ(callsOld, 1);

        // 同一实例重新注册类型: 旧节点代次失效, 不调用旧/新回调。
        auto specNew = makeSpec(&callsNew);
        slot->activate(f.provider, specNew, f.provider->lifetime->generation());
        auto err = runNode(oldNode);
        XX_TEST_EXPECT_TRUE(err.find("generation") != std::string::npos);
        XX_TEST_EXPECT_EQ(callsOld, 1);
        XX_TEST_EXPECT_EQ(callsNew, 0);

        // 新代次编译的节点正常执行新回调。
        auto newNode = std::make_shared<PluginGraphNode>(
            "slot_new",
            "{}",
            f.provider,
            specNew,
            slot,
            slot->snapshot().generation
        );
        XX_TEST_EXPECT_TRUE(runNode(newNode).empty());
        XX_TEST_EXPECT_EQ(callsNew, 1);

        // 卸载 (invalidate + Closing): 新旧节点都只返回插件已关闭, 不再进入插件。
        slot->invalidate(f.provider.get());
        f.provider->lifetime->requestClose();
        err = runNode(newNode);
        XX_TEST_EXPECT_TRUE(
            err.find("closing") != std::string::npos || err.find("generation") != std::string::npos
        );
        XX_TEST_EXPECT_EQ(callsNew, 1);

        // 重载同名 type (新实例 + 新代次): 旧节点不得转交新实例的回调。
        auto provider2   = f.instance("provider2", 42);
        int  callsReload = 0;
        auto specReload  = makeSpec(&callsReload);
        slot->activate(provider2, specReload, provider2->lifetime->generation());
        err = runNode(oldNode);
        XX_TEST_EXPECT_TRUE(
            err.find("closing") != std::string::npos || err.find("generation") != std::string::npos
        );
        err = runNode(newNode);
        XX_TEST_EXPECT_TRUE(
            err.find("closing") != std::string::npos || err.find("generation") != std::string::npos
        );
        XX_TEST_EXPECT_EQ(callsReload, 0);
        XX_TEST_EXPECT_EQ(callsOld, 1);
        XX_TEST_EXPECT_EQ(callsNew, 1);

        // 新实例上的新节点正常工作。
        auto reloadNode = std::make_shared<PluginGraphNode>(
            "slot_reload",
            "{}",
            provider2,
            specReload,
            slot,
            slot->snapshot().generation
        );
        XX_TEST_EXPECT_TRUE(runNode(reloadNode).empty());
        XX_TEST_EXPECT_EQ(callsReload, 1);
    }

    /// 订阅句柄独立于实例生命周期 —— 重复 unsubscribe 为空操作；
    /// 卸载 (detachAll 退订) 之后对旧句柄再次 unsubscribe 不得访问已释放实例。
    {
        auto           ctx = std::make_shared<agentxx::agent::AgentContext>();
        RuntimeFixture f(ctx);
        ctx->bus = std::make_shared<agentxx::events::EventBus>(f.io.get_executor());

        const auto* host   = f.provider->hostView();
        const auto  ifaces = AgentIfaces::query(host);
        XX_TEST_EXPECT_TRUE(ifaces.broadcasts != nullptr);
        if (!ifaces.broadcasts) {
            return result;
        }
        const auto topicSv      = PluginStringView::fromCstr("plugin.provider.topic");
        int        handlerCalls = 0;
        // 广播频道必须先声明 (测试直接建管理器, 没有任何插件声明过它)
        f.manager->ensureBroadcast("plugin.provider.topic");
        // EventBus 派发为协程异步执行; 循环推进后必须 restart, 保持上下文
        // 处于非 stopped 状态 (否则 vtable 入口按"IO 不可用"拒绝退订/订阅)。
        auto flushEvents = [&] {
            for (int i = 0; i < 50; ++i) {
                f.io.restart();
                f.io.run_for(std::chrono::milliseconds{2});
            }
            f.io.restart();
        };
        // 表成员是异步操作契约: 用例在主程序 IO 线程上, 主程序内联完成, 不产生等待
        auto broadcastOp = [&](auto&& start) {
            return agentxx::test::tableOpWait(
                host,
                ifaces.cancel,
                ifaces.scheduler,
                std::forward<decltype(start)>(start)
            );
        };
        auto unsubscribeOf = [&](PluginxxSubscription* target) {
            if (target == nullptr) {
                return false;
            }
            return broadcastOp(
                       [&](PluginxxOperatorCallback cb, void* cbUd, PluginxxString* err) {
                           return ifaces.broadcasts->unsubscribe_broadcast(
                               host,
                               target,
                               cb,
                               cbUd,
                               err
                           );
                       }
                   )
                .ok();
        };
        auto subscribeOf = [&](const PluginxxStringView* topic) {
            auto op = broadcastOp(
                [&](PluginxxOperatorCallback cb, void* cbUd, PluginxxString* err) {
                    return ifaces.broadcasts->subscribe_broadcast(
                        host,
                        nullptr, ///< 主程序总线 (与 agent 无关的进程级事件)
                        topic,
                        +[](const PluginxxStringView*, void* ud2) {
                            ++*static_cast<int*>(ud2);
                        },
                        &handlerCalls,
                        nullptr,
                        cb,
                        cbUd,
                        err
                    );
                }
            );
            // 订阅句柄以十进制文本放在结果正文里交回
            return op.ok() ? pluginxx::pluginxxSubscriptionFromText(op.text()) : nullptr;
        };
        auto* sub = subscribeOf(&topicSv);
        XX_TEST_EXPECT_TRUE(sub != nullptr);
        XX_TEST_EXPECT_TRUE(sub && sub->alive.load());

        f.manager->publishBroadcastValue("plugin.provider.topic", "{}");
        flushEvents();
        XX_TEST_EXPECT_EQ(handlerCalls, 1);

        // 重复退订: 第二次必须是空操作 (alive 先失效再移除主程序引用)。
        XX_TEST_EXPECT_TRUE(unsubscribeOf(sub));
        XX_TEST_EXPECT_TRUE(unsubscribeOf(sub));
        XX_TEST_EXPECT_FALSE(sub->alive.load());
        f.manager->publishBroadcastValue("plugin.provider.topic", "{}");
        flushEvents();
        XX_TEST_EXPECT_EQ(handlerCalls, 1); ///< 退订后不再回调

        // 卸载实例 (detachAll 统一退订) 后, 对插件保存的旧句柄再次退订必须安全。
        auto* late = subscribeOf(&topicSv);
        XX_TEST_EXPECT_TRUE(late != nullptr);
        bool unloaded = false;
        {
            auto rc = f.waitFutureValue(
                asio::co_spawn(f.io, f.manager->unloadAsync("provider", 500ms), asio::use_future)
            );
            XX_TEST_EXPECT_TRUE(rc.has_value());
            unloaded = rc.value_or(false);
        }
        XX_TEST_EXPECT_TRUE(unloaded);
        if (late) {
            XX_TEST_EXPECT_FALSE(late->alive.load()); ///< detachAll 已失效句柄
            // 实例已卸载: 入口按“不可用”拒绍, 退订本身由卸载流程完成 (不得崩溃)
            (void)unsubscribeOf(late);
            (void)unsubscribeOf(late);
        }
        // 主题只要求落在 plugin.*: 实例卸载后写同一主题仍允许
        // (同组织多插件可共用一个段, 主程序不把主题绑定到具体实例)
        f.manager->publishBroadcastValue("plugin.provider.topic", "{}");
        flushEvents();
        XX_TEST_EXPECT_EQ(handlerCalls, 1);
    }

    /// 协程驱动请求 (`agentxx.agent.scheduler` 的 request_driver/cancel_driver)
    /// 的主程序语义：恒异步、每张至多一次、取消后不再执行、排队期间持有实例 lease、
    /// 关闭后拒绝。
    {
        RuntimeFixture f;
        auto*          host = f.provider->hostView();
        XX_TEST_EXPECT_TRUE(host != nullptr);

        PluginxxStringView iid = PluginStringView::fromCstr(PLUGINXX_IFACE_SCHEDULER);
        const auto*        runtime
            = static_cast<const PluginxxSchedulerIface*>(xx_query_interface(host, &iid));
        XX_TEST_EXPECT_TRUE(runtime != nullptr);
        if (runtime) {
            XX_TEST_EXPECT_EQ(runtime->version, PLUGINXX_IFACE_SCHEDULER_VERSION);
            XX_TEST_EXPECT_EQ(runtime->struct_size, uint32_t{sizeof(PluginxxSchedulerIface)});
            XX_TEST_EXPECT_TRUE(runtime->request_driver != nullptr);
            XX_TEST_EXPECT_TRUE(runtime->cancel_driver != nullptr);
            XX_TEST_EXPECT_TRUE(runtime->is_io_thread != nullptr);
            // is_io_thread: 有独立插件执行器时它回答的是"当前线程是不是该实例的
            // 插件执行线程" (没有插件线程时才是 io 线程)。本线程跑 io → 0;
            // 插件线程上 → 1。
            XX_TEST_EXPECT_EQ(runtime->is_io_thread(host), 0);
            {
                std::atomic<int> seen{-1};
                auto             ex = f.manager->pluginExecutor();
                XX_TEST_EXPECT_TRUE(static_cast<bool>(ex));
                if (ex) {
                    asio::post(ex, [&seen, host, runtime] {
                        seen.store(runtime->is_io_thread(host), std::memory_order_release);
                    });
                    for (int i = 0; i < 2000 && seen.load(std::memory_order_acquire) < 0; ++i) {
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                    }
                }
                XX_TEST_EXPECT_EQ(seen.load(), 1);
            }

            struct DriveProbe {
                std::atomic<int>  calls{0};
                /// 回调实际跑在申请线程上 (只在"内联执行"时才是 true)
                std::atomic<bool> onCaller{false};
                /// 闸门驱动用: 进入点 / 放行
                std::atomic<bool> entered{false};
                std::atomic<bool> release{false};
                std::thread::id   caller{};
            } probe;
            probe.caller = std::this_thread::get_id();

            // 驱动回调必须是无捕获函数 (C ABI 函数指针), 经 user_data 取上下文。
            auto driveFn = [](void* ud) {
                auto* p = static_cast<DriveProbe*>(ud);
                if (std::this_thread::get_id() == p->caller) {
                    p->onCaller.store(true, std::memory_order_release);
                }
                p->calls.fetch_add(1, std::memory_order_acq_rel);
            };
            // 闸门驱动: 占住插件线程直到测试放行 (用来拿到"尚未开始"的确定时序)
            auto gatedDriveFn = [](void* ud) {
                auto* p = static_cast<DriveProbe*>(ud);
                if (std::this_thread::get_id() == p->caller) {
                    p->onCaller.store(true, std::memory_order_release);
                }
                p->calls.fetch_add(1, std::memory_order_acq_rel);
                p->entered.store(true, std::memory_order_release);
                while (!p->release.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            };

            // 1) 申请恒异步: 回调绝不在申请线程上内联执行 (有插件执行器时它在
            //    插件线程上跑, 可能比本线程走下一条断言还早)
            PluginxxString  err{nullptr, 0};
            PluginxxDriver* ticket = runtime->request_driver(host, driveFn, &probe, &err);
            XX_TEST_EXPECT_TRUE(ticket != nullptr);
            XX_TEST_EXPECT_FALSE(probe.onCaller.load());
            // 还没轮到执行时必然持有实例 lease (卸载等待必然覆盖它);
            // 插件线程已经跑完时这两项归零也是正常结果 (上一条注释已说明"可能更早").
            // 只有"驱动票还没被执行"时 lease 必然还在: 两次读数之间票可能已经完结,
            // 所以 activeDriverCount 只读一次, 不能拿它两次比对。
            const auto activeTickets = f.provider->activeDriverCount();
            if (probe.calls.load() == 0 && activeTickets > 0) {
                XX_TEST_EXPECT_GE(f.provider->lifetime->leaseCount(), size_t{1});
            }

            // 2) 插件线程与 io 都推进后: 恰好执行一次
            f.drainAll();
            XX_TEST_EXPECT_EQ(probe.calls.load(), 1);
            XX_TEST_EXPECT_EQ(f.provider->activeDriverCount(), size_t{0});
            XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});

            // 3) 重复推进不得再次执行 (每张请求至多一次)
            f.drainAll();
            XX_TEST_EXPECT_EQ(probe.calls.load(), 1);

            // 4) 取消尚未开始的请求: 不再执行, lease 立刻释放
            //    时序要确定: 先让插件线程卡在一张闸门驱动上, 取消的这张就必定还没
            //    轮到 (有插件线程时"申请完立刻取消"是竞态, 不能当作未开始)
            DriveProbe blocker;
            blocker.caller = std::this_thread::get_id();
            PluginxxDriver* hold = runtime->request_driver(host, gatedDriveFn, &blocker, &err);
            XX_TEST_EXPECT_TRUE(hold != nullptr);
            for (int i = 0; i < 5000 && !blocker.entered.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            XX_TEST_EXPECT_TRUE(blocker.entered.load());
            const int callsBeforeCancel = probe.calls.load();
            PluginxxDriver* Canceled = runtime->request_driver(host, driveFn, &probe, &err);
            XX_TEST_EXPECT_TRUE(Canceled != nullptr);
            runtime->cancel_driver(Canceled);
            XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1}); // 只剩 hold
            blocker.release.store(true, std::memory_order_release);
            f.drainAll();
            XX_TEST_EXPECT_EQ(probe.calls.load(), callsBeforeCancel); // 取消后不再执行
            XX_TEST_EXPECT_EQ(f.provider->activeDriverCount(), size_t{0});
            XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
            runtime->cancel_driver(Canceled); // 幂等
            runtime->cancel_driver(nullptr);   // 空指针安全

            // 5) 迟到/无效句柄取消: 按地址查表未命中, 安全忽略 (不解引用)
            XX_TEST_EXPECT_FALSE(
                f.provider->cancelDriver(reinterpret_cast<const PluginxxDriver*>(&probe))
            );
            runtime->cancel_driver(reinterpret_cast<PluginxxDriver*>(&probe));

            // 6) 参数缺失: 空回调返回 NULL + error_out
            PluginxxString err2{nullptr, 0};
            XX_TEST_EXPECT_TRUE(runtime->request_driver(host, nullptr, nullptr, &err2) == nullptr);
            XX_TEST_EXPECT_TRUE(err2.data != nullptr);
            if (err2.data) {
                hostMemoryFree(err2.data);
            }
            if (err.data) {
                hostMemoryFree(err.data);
            }

            // 7) 实例进入 Closing: 仍允许驱动 (关闭要先取消 Operation, 插件的取消
            //    收尾需要驱动继续流动), 但 Closed 后拒绝。
            f.provider->lifetime->requestClose();
            PluginxxDriver* closingTicket = runtime->request_driver(host, driveFn, &probe, &err);
            XX_TEST_EXPECT_TRUE(closingTicket != nullptr);
            runtime->cancel_driver(closingTicket);
            f.provider->lifetime->setState(pluginxx::PluginInstanceState::Closed);
            PluginxxString err3{nullptr, 0};
            XX_TEST_EXPECT_TRUE(runtime->request_driver(host, driveFn, &probe, &err3) == nullptr);
            XX_TEST_EXPECT_TRUE(err3.data != nullptr);
            if (err3.data) {
                hostMemoryFree(err3.data);
            }
            // 本用例已把实例手动推到 Closed: 从管理器实例表摘除, 避免夹具析构时
            // 再由 shutdownAll 走一次关闭状态迁移 (Closed 不允许回到 Closing)。
            f.manager->plugins_.erase(f.provider->name);
        }
    }

    /// 表调用的顺序契约 ([startTableOp]):
    /// - 调用方就是 IO 线程 (夹具里 `setIoExecutor` 把当前线程登记为 IO 线程):
    ///   内联执行, 函数返回时工作体已经跑完, 顺序就是提交顺序;
    /// - 其它线程: 按受理顺序排队, 由 IO 线程依次执行 (FIFO)。
    {
        RuntimeFixture   f;
        std::vector<int> order;

        // 1) IO 线程上提交: 内联执行
        for (int i = 0; i < 3; ++i) {
            auto* op = pluginxx::startTableOp(
                f.manager.get(),
                nullptr,
                nullptr,
                nullptr,
                "test.inline",
                [&order, i] {
                    order.push_back(i);
                    return std::string{"ok"};
                }
            );
            XX_TEST_EXPECT_TRUE(op != nullptr);
        }
        XX_TEST_EXPECT_EQ(order.size(), size_t{3});

        // 2) 别的线程提交: 排队, 未驱动前一条都不跑
        order.clear();
        std::atomic<int> accepted{0};
        std::thread      submitter([&] {
            for (int i = 0; i < 5; ++i) {
                auto* op = pluginxx::startTableOp(
                    f.manager.get(),
                    nullptr,
                    nullptr,
                    nullptr,
                    "test.fifo",
                    [&order, i] {
                        order.push_back(i);
                        return std::string{"ok"};
                    }
                );
                if (op) {
                    accepted.fetch_add(1);
                }
            }
        });
        submitter.join();
        XX_TEST_EXPECT_EQ(accepted.load(), 5);
        XX_TEST_EXPECT_TRUE(order.empty());

        f.io.run();
        XX_TEST_EXPECT_EQ(order.size(), size_t{5});
        if (order.size() == 5) {
            for (int i = 0; i < 5; ++i) {
                XX_TEST_EXPECT_EQ(order[static_cast<size_t>(i)], i);
            }
        }
    }

    return result;
}
} // namespace agentxx::test
