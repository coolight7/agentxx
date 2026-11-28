/// test_plugin_broadcast —— 插件广播面 (通用表 `agentxx.broadcasts`)
///
/// 背景: 广播注册表本身 (声明 / 缓存 / 订阅 / 限频 / 结束) 由 `cxx_pluginxx` 提供, 本模块
/// 验证**插件经 C ABI 走一遍**时的事实一致: 插件声明的频道落在主程序的同一份注册表上,
/// 归主程序与本插件共用, 禁用/卸载后回到基线。
///
/// 覆盖:
/// 1. 声明: 示例插件在 `mount` 里声明两条自己的频道 (短名由主程序补全), 幂等,
///    应用频道 (`agentxx.*`) 不能被插件改写
/// 2. 发布: 属主可发, 非属主发应用频道被拒; 载荷原样进缓存
/// 3. 订阅: 收到载荷; `pushOnSubscribe` 先推当前值; 退订后不再收
/// 4. 读当前值: 有值 / 无缓存 / 未声明三种结果
/// 5. 结束流: 发布与读被拒、`on_end` 恰好一次、重复结束幂等、再次声明开新流
/// 6. 生命周期: 禁用/卸载后频道与订阅回到基线
#include "agentxx-test/plugin/test_plugin_broadcast.h"
#include "agentxx-test/plugin/plugin_thread_fixture.h"
#include "agentxx-test/plugin/test_table_op.h"

#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/hub.h"
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "neograph/graph/registry.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_pb_passed = 0;
int g_pb_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_pb_passed
#define XX_TEST_FAILED g_pb_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

using agentxx::plugin::PluginInstance;
using agentxx::plugin::PluginManager;

namespace {

using Json = utilxx_base::Json;

/// 定位插件库目录 (与其它插件测试同一策略)
std::string findPluginDir(const char* pluginName) {
    std::error_code       ec;
    std::vector<fs::path> candidates;
    if (auto exeDir = executableDir()) {
        candidates.push_back(*exeDir / "plugins" / pluginName);
    }
    candidates.push_back(fs::current_path(ec) / "plugins" / pluginName);
    auto hasLibFile = [](const fs::path& dir) {
        std::error_code                     ec2;
        std::filesystem::directory_iterator it(dir, ec2);
        std::filesystem::directory_iterator end;
        bool                                hasScript = false;
        for (; it != end; it.increment(ec2)) {
            auto ext = it->path().extension().string();
            if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
                return true;
            }
            if (it->path().filename() == "plugin.js") {
                hasScript = true;
            }
        }
        // 纯脚本插件 (如 example_js) 没有动态库: 清单 + plugin.js 也算插件目录;
        // 只有 plugin.yaml 的 C++ 插件源码目录不算, 否则 cwd 在仓库根时会误命中
        if (hasScript && std::filesystem::exists(dir / "plugin.yaml", ec2)) {
            return true;
        }
        return false;
    };
    for (const auto& c : candidates) {
        if (fs::is_directory(c, ec) && hasLibFile(c)) {
            return c.string();
        }
    }
    return std::string{"plugins/"} + pluginName;
}

/// 测试夹具: 一个 agent 的上下文 (含事件中枢) + 插件管理器
struct BroadcastFixture {
    /// 生产同款线程模型: 声明在最前面 —— 成员逆序析构, 管理器先拆、线程池后停
    std::shared_ptr<agentxx::test::PluginThreadFixture> threads;
    std::shared_ptr<agentxx::agent::AgentContext>      ctx;
    std::shared_ptr<PluginManager>                     mgr;
    /// 挂载的 agentId (要活到测试结束: 接口表的引用指向它内部的字符)
    std::string                                   agentId;
    uint64_t                                      mountSeq = 0;
};

/// 造一个 agent 上下文与挂载
asio::awaitable<BroadcastFixture> makeFixture() {
    auto ex             = co_await asio::this_coro::executor;
    auto f              = BroadcastFixture{};
    f.ctx               = std::make_shared<agentxx::agent::AgentContext>();
    f.ctx->agentConfig  = std::make_shared<agentxx::agent::AgentConfig>();
    f.ctx->middlewareHandleContext
        = std::make_shared<agentxx::middleware::MiddlewareContext>();
    f.ctx->toolRegistry  = std::make_shared<agentxx::plugin::ToolRegistry>();
    f.ctx->graphRegistry = std::make_shared<neograph::graph::GraphRegistry>();
    // 事件总线: 主程序发在总线上的应用话题要能镜像进广播 (管理器构造时自动挂上)
    f.ctx->bus = std::make_shared<agentxx::events::EventBus>(ex);

    f.mgr                = std::make_shared<PluginManager>(f.ctx);
    f.ctx->pluginManager = f.mgr;
    // 与 BaseAgent::init 一致: 跨线程 vtable 调用经真实 post 回到本线程
    f.mgr->setIoExecutor(ex);
    f.threads = std::make_shared<agentxx::test::PluginThreadFixture>();
    XX_TEST_EXPECT_TRUE(f.threads->start("agentxx-test-broadcast"));
    f.threads->attach(*f.mgr);
    f.ctx->pluginMount = co_await f.mgr->mountAgentAsync(f.ctx);
    f.agentId          = f.ctx->pluginMount->agentId();
    f.mountSeq         = f.ctx->pluginMount->mountSeq();
    co_return f;
}

/// 本挂载的 agent 引用 (接口表的第一个参数)
PluginxxScopeRef agentRefOf(const BroadcastFixture& f) {
    PluginxxScopeRef ref{};
    ref.struct_size = sizeof(PluginxxScopeRef);
    ref.id          = agentxx::plugin::PluginStringView::from(f.agentId.data(), f.agentId.size());
    ref.seq         = f.mountSeq;
    return ref;
}

/// 取插件侧看到的广播表
const PluginxxBroadcastsIface* broadcastTableOf(PluginInstance* inst) {
    if (nullptr == inst) {
        return nullptr;
    }
    const auto* host = inst->hostView();
    if (nullptr == host) {
        return nullptr;
    }
    return agentxx::plugin::queryInterface<PluginxxBroadcastsIface>(
        host,
        PLUGINXX_IFACE_BROADCASTS
    );
}

/// 一次广播表调用 (等结果)
///
/// 表成员是异步操作契约: 用例跑在主程序 IO 线程上, 这里用测试专用的
/// [agentxx::test::tableOpWait] 换取同步形状 (提交 + 完成回调 + 条件变量)。
/// 失败细节在结果正文里 (例如 `read_broadcast: no_value`), 因此下面的便捷入口把它
/// 记进 [gLastBroadcastReason], 供断言检查具体原因。
template<typename Fn>
pluginxx::detail::TableOpResult broadcastOp(PluginInstance* inst, Fn&& start) {
    const auto* host   = inst->hostView();
    const auto  ifaces = agentxx::plugin::AgentIfaces::query(host);
    return agentxx::test::tableOpWait(
        host,
        ifaces.cancel,
        ifaces.scheduler,
        std::forward<Fn>(start)
    );
}

/// 最近一次广播表调用的失败原因 (空 = 成功)
std::string gLastBroadcastReason;

/// 结果记进 [gLastBroadcastReason], 成功给 0、失败给 -1 (表成员不再有细分返回码)
int32_t broadcastCodeOf(pluginxx::detail::TableOpResult result) {
    if (result.ok()) {
        gLastBroadcastReason.clear();
        return 0;
    }
    gLastBroadcastReason.assign(result.text());
    if (gLastBroadcastReason.empty()) {
        gLastBroadcastReason = result.startError;
    }
    return -1;
}

/// 表调用: 声明一个频道
int32_t declareOf(PluginInstance* inst, std::string_view id, std::string_view extra = {}) {
    auto* table = broadcastTableOf(inst);
    if (nullptr == table || table->declare_broadcast == nullptr) {
        gLastBroadcastReason = "declare_broadcast unavailable";
        return -1;
    }
    std::string decl = R"({"id":")" + std::string{id} + R"(","cache":"latest","publish":"owner")";
    if (!extra.empty()) {
        decl += ",";
        decl += std::string{extra};
    }
    decl += "}";
    auto declSv = agentxx::plugin::PluginStringView::from(decl.data(), decl.size());
    return broadcastCodeOf(
        broadcastOp(inst, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return table->declare_broadcast(inst->hostView(), &declSv, cb, ud, err);
        })
    );
}

/// 表调用: 发布
int32_t publishOf(PluginInstance* inst, std::string_view id, std::string_view payload) {
    auto* table = broadcastTableOf(inst);
    if (nullptr == table || table->publish_broadcast == nullptr) {
        gLastBroadcastReason = "publish_broadcast unavailable";
        return -1;
    }
    auto idSv      = agentxx::plugin::PluginStringView::from(id.data(), id.size());
    auto payloadSv = agentxx::plugin::PluginStringView::from(payload.data(), payload.size());
    return broadcastCodeOf(
        broadcastOp(inst, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return table->publish_broadcast(inst->hostView(), &idSv, &payloadSv, cb, ud, err);
        })
    );
}

/// 表调用: 读当前值 (返回正文; `codeOut` 0 = 成功 / -1 = 失败)
std::string readOf(PluginInstance* inst, std::string_view id, int32_t* codeOut = nullptr) {
    auto* table = broadcastTableOf(inst);
    if (nullptr == table || table->read_broadcast == nullptr) {
        gLastBroadcastReason = "read_broadcast unavailable";
        if (codeOut != nullptr) {
            *codeOut = -1;
        }
        return {};
    }
    auto   idSv = agentxx::plugin::PluginStringView::from(id.data(), id.size());
    auto   op   = broadcastOp(
        inst,
        [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return table->read_broadcast(inst->hostView(), &idSv, cb, ud, err);
        }
    );
    const int32_t code = broadcastCodeOf(op);
    if (codeOut != nullptr) {
        *codeOut = code;
    }
    return op.ok() ? std::string{op.text()} : std::string{};
}

/// 表调用: 结束一条流
int32_t endOf(
    PluginInstance*  inst,
    std::string_view id,
    std::string_view reasonJson = R"({"reason":"test"})"
) {
    auto* table = broadcastTableOf(inst);
    if (nullptr == table || table->end_broadcast == nullptr) {
        gLastBroadcastReason = "end_broadcast unavailable";
        return -1;
    }
    auto idSv     = agentxx::plugin::PluginStringView::from(id.data(), id.size());
    auto reasonSv = agentxx::plugin::PluginStringView::from(
        reasonJson.data(),
        reasonJson.size()
    );
    return broadcastCodeOf(
        broadcastOp(inst, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return table->end_broadcast(inst->hostView(), &idSv, &reasonSv, cb, ud, err);
        })
    );
}

/// 表调用: 订阅 (返回句柄; 失败给 nullptr, 原因记进 [gLastBroadcastReason])
///
/// `onEnd` 非空且主程序支持时走 `subscribe_broadcast_ex` (频道被结束时回调一次)。
PluginxxSubscription* subscribeOf(
    PluginInstance*           inst,
    const PluginxxScopeRef*   scope,
    const PluginxxStringView* id,
    void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*),
    void*                     ud,
    void(PLUGINXX_CALL* onEnd)(const PluginxxStringView*, void*) = nullptr
) {
    auto* table = broadcastTableOf(inst);
    if (nullptr == table || table->subscribe_broadcast == nullptr) {
        gLastBroadcastReason = "subscribe_broadcast unavailable";
        return nullptr;
    }
    const bool wantEnd = onEnd != nullptr && table->subscribe_broadcast_ex != nullptr;
    auto       op      = broadcastOp(
        inst,
        [&](PluginxxOperatorCallback cb, void* opUd, PluginxxString* err) {
            if (wantEnd) {
                return table->subscribe_broadcast_ex(
                    inst->hostView(),
                    scope,
                    id,
                    handler,
                    onEnd,
                    ud,
                    nullptr,
                    cb,
                    opUd,
                    err
                );
            }
            return table->subscribe_broadcast(
                inst->hostView(),
                scope,
                id,
                handler,
                ud,
                nullptr,
                cb,
                opUd,
                err
            );
        }
    );
    (void)broadcastCodeOf(op);
    // 订阅句柄以十进制文本放在结果正文里交回
    return op.ok() ? pluginxx::pluginxxSubscriptionFromText(op.text()) : nullptr;
}

/// 表调用: 退订 (幂等)
bool unsubscribeOf(PluginInstance* inst, PluginxxSubscription* sub) {
    auto* table = broadcastTableOf(inst);
    if (nullptr == table || table->unsubscribe_broadcast == nullptr || nullptr == sub) {
        return false;
    }
    return broadcastCodeOf(
               broadcastOp(inst, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                   return table->unsubscribe_broadcast(
                       inst->hostView(),
                       sub,
                       cb,
                       ud,
                       err
                   );
               })
           )
           == 0;
}

/// 订阅回调记录 (测试自己持有, 用 `ud` 指回来)
///
/// **回调跑在插件执行线程上**, 与断言所在的线程不同: 计数与载荷都要加锁读写。
/// 另外表调用返回**不代表回调已经跑过** —— 发布/订阅类回调是排队到插件执行线程
/// 执行的, 断言前一律先 `co_await waitSubValues/waitSubEnds` 等它落地。
struct SubCounter {
    /// 收到一次发布载荷
    void onValue(const PluginxxStringView* payload) {
        std::lock_guard lock(mutex_);
        ++values_;
        if (payload != nullptr && payload->data != nullptr) {
            lastPayload_.assign(payload->data, static_cast<size_t>(payload->size));
        }
    }

    /// 收到一次流结束
    void onEnd() {
        std::lock_guard lock(mutex_);
        ++ends_;
    }

    int valueCount() const {
        std::lock_guard lock(mutex_);
        return values_;
    }

    int endCount() const {
        std::lock_guard lock(mutex_);
        return ends_;
    }

    std::string payloadText() const {
        std::lock_guard lock(mutex_);
        return lastPayload_;
    }

private:
    mutable std::mutex mutex_;
    int                values_ = 0;
    int                ends_   = 0;
    std::string        lastPayload_;
};

void PLUGINXX_CALL countValue(const PluginxxStringView* payload, void* ud) {
    auto* counter = static_cast<SubCounter*>(ud);
    if (nullptr == counter) {
        return;
    }
    counter->onValue(payload);
}

void PLUGINXX_CALL countEnd(const PluginxxStringView*, void* ud) {
    auto* counter = static_cast<SubCounter*>(ud);
    if (nullptr == counter) {
        return;
    }
    counter->onEnd();
}

/// 加载示例插件 (挂载配置可空)
asio::awaitable<std::shared_ptr<PluginInstance>> loadExampleEvent(
    BroadcastFixture&  f,
    std::string        argsJson = {}
) {
    agentxx::agent::PluginConfig cfg;
    cfg.path = findPluginDir("example_event");
    if (!argsJson.empty()) {
        cfg.args = Json::parse(argsJson);
    }
    co_return co_await f.mgr->loadPluginAsync(cfg.path, &cfg);
}

/// 收尾: 卸载插件并拆掉本挂载 (未加载时是空操作)
asio::awaitable<void> unloadExampleEvent(BroadcastFixture& f) {
    co_await f.mgr->unloadAsync("example_event", std::chrono::seconds{5});
    co_await f.mgr->unmountAgentAsync(f.agentId);
    f.ctx->pluginMount.reset();
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

/// 轮询等待条件成立 (插件侧调用与生命周期事务都投递到 io 线程)
asio::awaitable<bool> waitFor(std::function<bool()> cond, int timeoutMs = 5000) {
    auto       ex       = co_await asio::this_coro::executor;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds{10});
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return cond();
}

/// 等订阅回调收到 `expected` 次发布 (回包排队在插件执行线程上, 不能立即断言)
asio::awaitable<bool> waitSubValues(const SubCounter& counter, int expected) {
    co_return co_await waitFor([&counter, expected] {
        return counter.valueCount() >= expected;
    });
}

/// 等订阅回调收到 `expected` 次流结束
asio::awaitable<bool> waitSubEnds(const SubCounter& counter, int expected) {
    co_return co_await waitFor([&counter, expected] {
        return counter.endCount() >= expected;
    });
}

/// 给"不该发生"的回调留出时间再断言
///
/// 回调是排队到插件执行线程执行的, 发布返回后立即断言等于什么也没验: 晚到的回调
/// 会从下面跑过去。这里等一小段稳定时间, 让不该来的回包真有机会来。
asio::awaitable<void> settlePluginCallbacks(int ms = 100) {
    (void)co_await waitFor([] { return false; }, ms);
}

} // namespace

// ===========================================================================
// 1. 声明与自查: 示例插件在 mount 里声明频道并发布初始值
// ===========================================================================
namespace {

asio::awaitable<void> test_declare_and_publish() {
    auto f = co_await makeFixture();
    auto registry = f.mgr->broadcasts();
    XX_TEST_EXPECT_TRUE(registry != nullptr);

    auto inst = co_await loadExampleEvent(f);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }

    // 两条频道都由插件声明 (属主 = 插件名), 短名已补成 `plugin.<插件 id>.<名字>`
    const auto* build = registry->declOf("plugin.example_event.build");
    XX_TEST_EXPECT_TRUE(build != nullptr);
    if (build != nullptr) {
        XX_TEST_EXPECT_EQ(build->owner, std::string{"example_event"});
        XX_TEST_EXPECT_TRUE(build->cache == pluginxx::BroadcastCache_e::Latest);
        XX_TEST_EXPECT_TRUE(build->publish == pluginxx::BroadcastPublisher_e::Owner);
    }
    XX_TEST_EXPECT_TRUE(registry->declOf("plugin.example_event.ready") != nullptr);

    // 当前值: 插件在 mount 里发过一条 (进度 1/2 与就绪值)
    std::string value;
    XX_TEST_EXPECT_TRUE(registry->read("plugin.example_event.build", &value));
    const auto step = Json::parse(value);
    XX_TEST_EXPECT_EQ(step.value("step", 0), 1);
    XX_TEST_EXPECT_EQ(step.value("total", 0), 2);

    XX_TEST_EXPECT_TRUE(registry->read("plugin.example_event.ready", &value));
    const auto ready = Json::parse(value);
    XX_TEST_EXPECT_EQ(ready.value("ok", false), true);
    XX_TEST_EXPECT_EQ(ready.value("plugin", std::string{}), std::string{"example_event"});

    // 注册清单: 插件订阅了自己的进度频道 (+ 主程序轮次通知, 声明过才有)
    const auto inv = f.mgr->registrationInventory(*inst);
    XX_TEST_EXPECT_GE(inv.eventSubscriptions, size_t{1});

    // 自省: 频道清单里能看到这两条, 且订阅数是从真实订阅表里数的
    const auto listJson = registry->listJson("plugin.example_event.");
    XX_TEST_EXPECT_TRUE(contains(listJson, "plugin.example_event.build"));
    XX_TEST_EXPECT_TRUE(contains(listJson, "plugin.example_event.ready"));

    co_await unloadExampleEvent(f);
}

// ===========================================================================
// 2. 发布权限: 属主可发; 非属主发别人的频道被拒; 声明规则
// ===========================================================================
asio::awaitable<void> test_publish_permission() {
    auto       f    = co_await makeFixture();
    auto       inst = co_await loadExampleEvent(f);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    auto registry = f.mgr->broadcasts();

    // 属主发布自己的频道: 成功, 缓存更新
    XX_TEST_EXPECT_EQ(
        publishOf(inst.get(), "plugin.example_event.build", R"({"step":2,"total":2})"),
        int32_t{PLUGINXX_RESULT_OK}
    );
    std::string value;
    XX_TEST_EXPECT_TRUE(registry->read("plugin.example_event.build", &value));
    XX_TEST_EXPECT_EQ(Json::parse(value).value("step", 0), 2);

    // 非属主发布应用频道: 被拒 (属主是主程序), 缓存不变
    if (registry->declares("agentxx.agent.round.start")) {
        XX_TEST_EXPECT_TRUE(
            publishOf(inst.get(), "agentxx.agent.round.start", R"({"hijack":true})")
            != int32_t{PLUGINXX_RESULT_OK}
        );
        XX_TEST_EXPECT_FALSE(registry->read("agentxx.agent.round.start", &value)
                             && contains(value, "hijack"));
    }

    // 插件不能把应用频道改写成自己的: 声明被拒, 属主仍是主程序
    XX_TEST_EXPECT_TRUE(
        declareOf(inst.get(), "agentxx.agent.round.start", R"("cache":"none")")
        != int32_t{PLUGINXX_RESULT_OK}
    );
    if (const auto* decl = registry->declOf("agentxx.agent.round.start"); decl != nullptr) {
        XX_TEST_EXPECT_TRUE(decl->owner.empty()); ///< 属主仍是主程序
    }

    // 短名声明: 补成 `plugin.<插件 id>.<短名>`; 同一 id 重复声明幂等
    XX_TEST_EXPECT_EQ(declareOf(inst.get(), "third"), int32_t{PLUGINXX_RESULT_OK});
    XX_TEST_EXPECT_TRUE(registry->declares("plugin.example_event.third"));
    XX_TEST_EXPECT_EQ(declareOf(inst.get(), "third"), int32_t{PLUGINXX_RESULT_OK});
    XX_TEST_EXPECT_EQ(registry->declOf("plugin.example_event.third")->owner, std::string{"example_event"});
    // 完全相同的重复声明不改状态: 当前值保留 (插件每次发布前都会声明一次, 不能把值刷掉)
    XX_TEST_EXPECT_EQ(
        publishOf(inst.get(), "plugin.example_event.third", R"({"n":1})"),
        int32_t{PLUGINXX_RESULT_OK}
    );
    XX_TEST_EXPECT_EQ(declareOf(inst.get(), "third"), int32_t{PLUGINXX_RESULT_OK});
    {
        std::string kept;
        XX_TEST_EXPECT_TRUE(registry->read("plugin.example_event.third", &kept));
        XX_TEST_EXPECT_EQ(Json::parse(kept).value("n", 0), 1);
    }

    // 未声明的 id 发布: 被拒
    XX_TEST_EXPECT_TRUE(publishOf(inst.get(), "plugin.example_event.none", "{}") != PLUGINXX_RESULT_OK);

    co_await unloadExampleEvent(f);
}

// ===========================================================================
// 3. 订阅与读: 载荷原样、pushOnSubscribe 补当前值、退订后不再收、读的三种结果
// ===========================================================================
asio::awaitable<void> test_subscribe_and_read() {
    auto       f    = co_await makeFixture();
    auto       inst = co_await loadExampleEvent(f);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    auto* table = broadcastTableOf(inst.get());
    XX_TEST_EXPECT_TRUE(table != nullptr && table->subscribe_broadcast != nullptr);
    if (nullptr == table || table->subscribe_broadcast == nullptr) {
        co_await unloadExampleEvent(f);
        co_return;
    }
    const auto ref = agentRefOf(f);
    auto       id  = agentxx::plugin::PluginStringView::fromCstr("plugin.example_event.build");

    // A. 普通订阅: 只收后续发布
    SubCounter plain;
    auto* sub = subscribeOf(inst.get(), &ref, &id, &countValue, &plain);
    XX_TEST_EXPECT_TRUE(sub != nullptr);
    XX_TEST_EXPECT_EQ(plain.valueCount(), 0); ///< 没 pushOnSubscribe, 订阅时不补当前值
    XX_TEST_EXPECT_EQ(
        publishOf(inst.get(), "plugin.example_event.build", R"({"step":3})"),
        int32_t{PLUGINXX_RESULT_OK}
    );
    XX_TEST_EXPECT_TRUE(co_await waitSubValues(plain, 1));
    XX_TEST_EXPECT_EQ(plain.valueCount(), 1);
    XX_TEST_EXPECT_TRUE(contains(plain.payloadText(), "\"step\":3"));

    // 退订之后不再收到
    if (sub != nullptr) {
        XX_TEST_EXPECT_TRUE(unsubscribeOf(inst.get(), sub));
    }
    XX_TEST_EXPECT_EQ(
        publishOf(inst.get(), "plugin.example_event.build", R"({"step":4})"),
        int32_t{PLUGINXX_RESULT_OK}
    );
    co_await settlePluginCallbacks(); ///< 退订若没生效, 这条发布就会让计数变成 2
    XX_TEST_EXPECT_EQ(plain.valueCount(), 1);

    // B. pushOnSubscribe: 声明时就要求补当前值
    XX_TEST_EXPECT_EQ(
        declareOf(inst.get(), "pushy", R"("pushOnSubscribe":true)"),
        int32_t{PLUGINXX_RESULT_OK}
    );
    XX_TEST_EXPECT_EQ(publishOf(inst.get(), "plugin.example_event.pushy", R"({"v":1})"), int32_t{0});
    SubCounter pushy;
    auto       pushyId = agentxx::plugin::PluginStringView::fromCstr("plugin.example_event.pushy");
    auto*      pushySub = subscribeOf(inst.get(), &ref, &pushyId, &countValue, &pushy);
    XX_TEST_EXPECT_TRUE(pushySub != nullptr);
    XX_TEST_EXPECT_TRUE(co_await waitSubValues(pushy, 1));
    XX_TEST_EXPECT_EQ(pushy.valueCount(), 1); ///< 订阅前的那份当前值补了一次
    XX_TEST_EXPECT_TRUE(contains(pushy.payloadText(), "\"v\":1"));
    if (pushySub != nullptr) {
        XX_TEST_EXPECT_TRUE(unsubscribeOf(inst.get(), pushySub));
    }

    // C. 读当前值: 有缓存 / 无缓存 / 未声明
    {
        int32_t code = 0;
        auto    text = readOf(inst.get(), "plugin.example_event.build", &code);
        XX_TEST_EXPECT_EQ(code, int32_t{PLUGINXX_RESULT_OK});
        XX_TEST_EXPECT_TRUE(contains(text, "\"step\":4"));

        // 无缓存的频道: 没有当前值
        XX_TEST_EXPECT_EQ(
            declareOf(inst.get(), "nocache", R"("cache":"none")"),
            int32_t{PLUGINXX_RESULT_OK}
        );
        (void)readOf(inst.get(), "plugin.example_event.nocache", &code);
        XX_TEST_EXPECT_TRUE(code != int32_t{PLUGINXX_RESULT_OK});
        XX_TEST_EXPECT_TRUE(contains(gLastBroadcastReason, "no_value"));

        // 未声明: 没有这个频道
        (void)readOf(inst.get(), "plugin.example_event.missing", &code);
        XX_TEST_EXPECT_TRUE(code != int32_t{PLUGINXX_RESULT_OK});
        XX_TEST_EXPECT_TRUE(contains(gLastBroadcastReason, "not_declared"));
    }

    co_await unloadExampleEvent(f);
}

// ===========================================================================
// 4. 结束流: 发布与读被拒、on_end 恰好一次、重复幂等、再次声明开新流
// ===========================================================================
asio::awaitable<void> test_end_stream() {
    auto       f    = co_await makeFixture();
    auto       inst = co_await loadExampleEvent(f);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    auto  registry = f.mgr->broadcasts();
    auto* table    = broadcastTableOf(inst.get());
    XX_TEST_EXPECT_TRUE(table != nullptr && table->end_broadcast != nullptr
                        && table->subscribe_broadcast_ex != nullptr);
    if (nullptr == table || table->end_broadcast == nullptr) {
        co_await unloadExampleEvent(f);
        co_return;
    }
    const auto ref = agentRefOf(f);
    auto       id  = agentxx::plugin::PluginStringView::fromCstr("plugin.example_event.build");

    // 带结束回调的订阅
    SubCounter watch;
    auto*      sub = subscribeOf(inst.get(), &ref, &id, &countValue, &watch, &countEnd);
    XX_TEST_EXPECT_TRUE(sub != nullptr);

    XX_TEST_EXPECT_EQ(
        publishOf(inst.get(), "plugin.example_event.build", R"({"step":9})"),
        int32_t{PLUGINXX_RESULT_OK}
    );
    XX_TEST_EXPECT_TRUE(co_await waitSubValues(watch, 1));
    XX_TEST_EXPECT_EQ(watch.valueCount(), 1);

    // 结束: 订阅方收到一次 on_end, 频道标记为已结束
    XX_TEST_EXPECT_EQ(
        endOf(inst.get(), "plugin.example_event.build"),
        int32_t{PLUGINXX_RESULT_OK}
    );
    XX_TEST_EXPECT_TRUE(registry->isEnded("plugin.example_event.build"));
    XX_TEST_EXPECT_TRUE(co_await waitSubEnds(watch, 1));
    XX_TEST_EXPECT_EQ(watch.endCount(), 1);

    // 结束后: 发布与读都被拒 (失败原因经结果正文交回)
    {
        const auto rc = publishOf(inst.get(), "plugin.example_event.build", R"({"step":10})");
        XX_TEST_EXPECT_TRUE(rc != int32_t{PLUGINXX_RESULT_OK});
        XX_TEST_EXPECT_TRUE(contains(gLastBroadcastReason, "ended"));
    }
    {
        int32_t code = 0;
        (void)readOf(inst.get(), "plugin.example_event.build", &code);
        XX_TEST_EXPECT_TRUE(code != int32_t{PLUGINXX_RESULT_OK});
        XX_TEST_EXPECT_TRUE(contains(gLastBroadcastReason, "ended"));
    }
    // 重复结束: 幂等成功, 不再回调 on_end
    XX_TEST_EXPECT_EQ(
        endOf(inst.get(), "plugin.example_event.build"),
        int32_t{PLUGINXX_RESULT_OK}
    );
    co_await settlePluginCallbacks(); ///< 重复结束若多回调一次, 计数会变成 2
    XX_TEST_EXPECT_EQ(watch.endCount(), 1);

    // 再次声明同一个 id = 开一条新流 (清掉结束标记与缓存)
    XX_TEST_EXPECT_EQ(
        declareOf(inst.get(), "plugin.example_event.build"),
        int32_t{PLUGINXX_RESULT_OK}
    );
    XX_TEST_EXPECT_FALSE(registry->isEnded("plugin.example_event.build"));
    XX_TEST_EXPECT_EQ(
        publishOf(inst.get(), "plugin.example_event.build", R"({"step":11})"),
        int32_t{PLUGINXX_RESULT_OK}
    );

    co_await unloadExampleEvent(f);
}

// ===========================================================================
// 5. 生命周期: 禁用/启用/卸载后频道与订阅回到基线
// ===========================================================================
asio::awaitable<void> test_lifecycle_reversible() {
    auto       f    = co_await makeFixture();
    auto       inst = co_await loadExampleEvent(f);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    auto registry = f.mgr->broadcasts();
    XX_TEST_EXPECT_TRUE(registry->declares("plugin.example_event.build"));
    const auto before = f.mgr->registrationInventory(*inst).total();
    XX_TEST_EXPECT_GE(before, size_t{1});

    // 禁用: 频道与订阅由框架按属主回收 (同步入口, 事务投递到 io 线程执行)
    co_await f.mgr->disable("example_event");
    XX_TEST_EXPECT_TRUE(
        co_await waitFor([&] { return !registry->declares("plugin.example_event.build"); })
    );
    XX_TEST_EXPECT_EQ(f.mgr->registrationInventory(*inst).total(), size_t{0});

    // 启用: 插件在自己的 mount 里重新声明
    co_await f.mgr->enable("example_event");
    XX_TEST_EXPECT_TRUE(
        co_await waitFor([&] { return registry->declares("plugin.example_event.build"); })
    );
    std::string value;
    XX_TEST_EXPECT_TRUE(registry->read("plugin.example_event.build", &value));
    XX_TEST_EXPECT_EQ(Json::parse(value).value("step", 0), 1);

    co_await unloadExampleEvent(f);
    XX_TEST_EXPECT_FALSE(registry->declares("plugin.example_event.build"));
    XX_TEST_EXPECT_FALSE(registry->declares("plugin.example_event.ready"));
}

// ===========================================================================
// 6. 广播 ↔ 事件总线两向转发
// =========================================================================
// 插件只认广播表, 主程序与客户端上行的话题却在事件总线上; 两端都缺一跳时
// `agentxx.agent.host.client_attached` 送不到插件, 插件发布的广播也发不出 wire。
asio::awaitable<void> test_bus_bridge() {
    auto f = co_await makeFixture();
    XX_TEST_EXPECT_TRUE(f.ctx->bus != nullptr);
    auto registry = f.mgr->broadcasts();
    XX_TEST_EXPECT_TRUE(registry != nullptr);
    if (nullptr == f.ctx->bus || nullptr == registry) {
        co_return;
    }
    auto bus = f.ctx->bus;

    // ---- 6.1 总线 → 广播: 已声明的应用频道收到值 ----
    f.mgr->ensureBroadcast(
        "agentxx.agent.host.client_attached",
        pluginxx::BroadcastCache_e::Latest,
        /*anyonePublish=*/true
    );
    // 前缀回调在发布方栈上同步执行, co_await 返回时镜像已完成
    co_await bus->publish(
        std::string{"agentxx.agent.host.client_attached"},
        std::string{R"({"sessionId":"sess-bridge"})"}
    );
    std::string value;
    XX_TEST_EXPECT_TRUE(registry->read("agentxx.agent.host.client_attached", &value));
    XX_TEST_EXPECT_EQ(
        Json::parse(value).value("sessionId", std::string{}),
        std::string{"sess-bridge"}
    );

    // 客户端上行族同样落到广播 (agent 侧插件订阅 usage_enabled 一类开关)
    f.mgr->ensureBroadcast(
        "agentxx.client.plugin.agentxx_system_monitor.usage_enabled",
        pluginxx::BroadcastCache_e::Latest,
        /*anyonePublish=*/true
    );
    co_await bus->publish(
        std::string{"agentxx.client.plugin.agentxx_system_monitor.usage_enabled"},
        std::string{R"({"enabled":false})"}
    );
    XX_TEST_EXPECT_TRUE(
        registry->read("agentxx.client.plugin.agentxx_system_monitor.usage_enabled", &value)
    );
    XX_TEST_EXPECT_EQ(Json::parse(value).value("enabled", true), false);

    // 没声明过的名字不建频道 (高频强类型话题 / 没人订阅的事件不占缓存)
    co_await bus->publish(std::string{"agentxx.agent.io.delta"}, std::string{"{}"});
    XX_TEST_EXPECT_FALSE(registry->declares("agentxx.agent.io.delta"));

    // ---- 6.2 插件发布 → 总线: 端点据此发 WirePluginData 给客户端 ----
    std::map<std::string, std::string> onBus;
    const size_t                       subId = bus->listenPrefix(
        "plugin.example_event.",
        [&onBus](std::string_view topic, const std::any& data) {
            if (data.type() == typeid(std::string)) {
                onBus[std::string{topic}] = std::any_cast<const std::string&>(data);
            }
        }
    );
    auto inst = co_await loadExampleEvent(f);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    // 插件的发布在 mount 里, 转发经总线 executor 异步执行 → 等一拍
    const auto readyTopic = std::string{"plugin.example_event.ready"};
    const auto buildTopic = std::string{"plugin.example_event.build"};
    XX_TEST_EXPECT_TRUE(co_await waitFor([&onBus, &readyTopic, &buildTopic] {
        return onBus.contains(readyTopic) && onBus.contains(buildTopic);
    }));
    if (onBus.contains(readyTopic)) {
        const auto ready = Json::parse(onBus[readyTopic]);
        XX_TEST_EXPECT_EQ(ready.value("ok", false), true);
        XX_TEST_EXPECT_EQ(ready.value("plugin", std::string{}), std::string{"example_event"});
    }
    if (onBus.contains(buildTopic)) {
        const auto build = Json::parse(onBus[buildTopic]);
        XX_TEST_EXPECT_EQ(build.value("step", 0), 1);
        XX_TEST_EXPECT_EQ(build.value("total", 0), 2);
    }
    bus->unlistenPrefix(subId);

    co_await unloadExampleEvent(f);
}

} // namespace

asio::awaitable<TestResult> run_plugin_broadcast_tests() {
    g_pb_passed = 0;
    g_pb_failed = 0;

    co_await test_declare_and_publish();
    co_await test_publish_permission();
    co_await test_subscribe_and_read();
    co_await test_end_stream();
    co_await test_lifecycle_reversible();
    co_await test_bus_bridge();

    co_return TestResult{g_pb_passed, g_pb_failed};
}

} // namespace test
} // namespace agentxx
