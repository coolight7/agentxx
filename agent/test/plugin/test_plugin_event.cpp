/// test_plugin_event —— 事件中枢的插件面 (接口表 `agentxx.agent.event`)
///
/// 背景: 事件中枢本身 (记录/等待/投递) 已在 `event_hub` / `event_delivery` 模块覆盖,
/// 本模块验证**插件经 C ABI 走一遍**时的事实一致 —— 插件声明的事件源、发布的值、替会话
/// 登记的等待都落在同一份中枢记录上, 模型侧 `watch` 拿到的就是插件发的那份值。
///
/// 覆盖:
/// 1. 声明与自查: 示例插件在 `mount` 里声明两条事件 (流式 / 单值) 并发布初始值,
///    单值事件自动结束, 注册清单计数与中枢记录一致
/// 2. 发布 / 结束 / 移除: `emit_value` 排队写入、`end_event` 幂等、结束后再到值重新打开、
///    `remove_event` 之后查不到 (挂在上面的等待按"已结束且无值"收尾)
/// 3. 等待与投递: `register_wait` 替会话登记等待, 值到达后成为该会话的待注入输入;
///    回放 (replay) 让注册前已经到达的值也能拿到
/// 4. 失败路径: 非法名称 / 同名不同种类 / 等待不存在 / 中枢未启用 / 引用无效
/// 5. 生命周期: 禁用删记录并取消等待、启用按 `mount` 重新声明、卸载后中枢回到基线
#include "agentxx-test/plugin/test_plugin_event.h"

#include "agentxx/agent/context.h"
#include "agentxx/agent/session_input_sink.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/hub.h"
#include "agentxx/event/naming.h"
#include "agentxx/plugin/api/plugin_kit.h"
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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_pe_passed = 0;
int g_pe_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_pe_passed
#define XX_TEST_FAILED g_pe_failed

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
        for (; it != end; it.increment(ec2)) {
            auto ext = it->path().extension().string();
            if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
                return true;
            }
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

/// 假驱动端: 只记录投递进来的输入 (不需要"轮次在跑"的场景用)
class RecordingSink : public agentxx::agent::SessionInputSink {
public:

    std::vector<std::string> texts;
    std::vector<std::string> sources;

    bool turnActive() const override {
        return false;
    }

    bool submitTurnInput(std::string_view text, std::string_view source, bool) override {
        texts.emplace_back(text);
        sources.emplace_back(source);
        return true;
    }
};

/// 测试夹具: 一个 agent 的上下文 (含事件中枢) + 插件管理器 + 若干会话
struct EventFixture {
    std::shared_ptr<agentxx::agent::AgentContext>   ctx;
    std::shared_ptr<PluginManager>                  mgr;
    std::shared_ptr<agentxx::events::EventHub>      hub;
    std::shared_ptr<agentxx::agent::Session>        session; ///< "s1": 示例插件替它登记等待
    /// 挂载的 agentId (要活到测试结束: 接口表的引用指向它内部的字符)
    std::string                                     agentId;
    uint64_t                                        mountSeq = 0;
};

/// 造一个 agent 上下文与挂载; `withHub=false` 时不建中枢 (模拟没有中枢的嵌入方)
asio::awaitable<EventFixture> makeFixture(bool withHub = true) {
    auto ex       = co_await asio::this_coro::executor;
    auto f        = EventFixture{};
    f.ctx         = std::make_shared<agentxx::agent::AgentContext>();
    f.ctx->agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
    f.ctx->middlewareHandleContext
        = std::make_shared<agentxx::middleware::MiddlewareContext>();
    f.ctx->bus                  = std::make_shared<agentxx::events::EventBus>(ex);
    f.ctx->toolRegistry         = std::make_shared<agentxx::plugin::ToolRegistry>();
    f.ctx->graphRegistry        = std::make_shared<neograph::graph::GraphRegistry>();
    f.session                   = f.ctx->sessions->getOrCreate("s1");
    if (withHub) {
        f.hub               = std::make_shared<agentxx::events::EventHub>(ex, f.ctx);
        f.ctx->eventHub     = f.hub;
        f.hub->attachBus(f.ctx->bus);
    }

    f.mgr                = std::make_shared<PluginManager>(f.ctx);
    f.ctx->pluginManager = f.mgr;
    // 与 BaseAgent::init 一致: 跨线程 vtable 调用经真实 post 回到本线程
    f.mgr->setIoExecutor(ex);
    f.ctx->pluginMount = co_await f.mgr->mountAgentAsync(f.ctx);
    f.agentId          = f.ctx->pluginMount->agentId();
    f.mountSeq         = f.ctx->pluginMount->mountSeq();
    co_return f;
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

/// 本挂载的 agent 引用 (接口表的第一个参数)
/// - `id` 必须指向活的字符: 这里指向夹具里那个字符串成员, 不再造临时副本
PluginxxScopeRef agentRefOf(const EventFixture& f) {
    PluginxxScopeRef ref{};
    ref.struct_size = sizeof(PluginxxScopeRef);
    ref.id          = agentxx::plugin::PluginStringView::from(f.agentId.data(), f.agentId.size());
    ref.seq         = f.mountSeq;
    return ref;
}

/// 取插件侧看到的事件中枢表
const AgentxxPluginEventIface* eventTableOf(PluginInstance* inst) {
    if (nullptr == inst) {
        return nullptr;
    }
    const auto* host = inst->hostView();
    if (nullptr == host || nullptr == host->vtable || nullptr == host->vtable->query_interface) {
        return nullptr;
    }
    auto iid = agentxx::plugin::PluginStringView::fromCstr(AGENTXX_PLUGIN_IFACE_AGENT_EVENT);
    return static_cast<const AgentxxPluginEventIface*>(
        host->vtable->query_interface(host, &iid)
    );
}

/// 表调用: 发布一个值
int32_t emitValue(
    PluginInstance*         inst,
    const PluginxxScopeRef& ref,
    std::string_view        name,
    std::string_view        valueJson
) {
    auto* table = eventTableOf(inst);
    if (nullptr == table || table->emit_value == nullptr) {
        return -1;
    }
    auto nameSv  = agentxx::plugin::PluginStringView::from(name.data(), name.size());
    auto valueSv = agentxx::plugin::PluginStringView::from(valueJson.data(), valueJson.size());
    return table->emit_value(inst->hostView(), &ref, &nameSv, &valueSv);
}

/// 表调用: 结束一条流
int32_t endEventOf(
    PluginInstance*         inst,
    const PluginxxScopeRef& ref,
    std::string_view        name,
    std::string_view        summaryJson = {}
) {
    auto* table = eventTableOf(inst);
    if (nullptr == table || table->end_event == nullptr) {
        return -1;
    }
    auto nameSv    = agentxx::plugin::PluginStringView::from(name.data(), name.size());
    auto summarySv = agentxx::plugin::PluginStringView::from(summaryJson.data(), summaryJson.size());
    return table->end_event(inst->hostView(), &ref, &nameSv, &summarySv);
}

/// 表调用: 移除一条记录
int32_t removeEventOf(
    PluginInstance*         inst,
    const PluginxxScopeRef& ref,
    std::string_view        name
) {
    auto* table = eventTableOf(inst);
    if (nullptr == table || table->remove_event == nullptr) {
        return -1;
    }
    auto nameSv = agentxx::plugin::PluginStringView::from(name.data(), name.size());
    return table->remove_event(inst->hostView(), &ref, &nameSv);
}

/// 表调用: 声明一个事件源
int32_t defineEventOf(
    PluginInstance*         inst,
    const PluginxxScopeRef& ref,
    std::string_view        name,
    bool                    single,
    std::string_view        description = {}
) {
    auto* table = eventTableOf(inst);
    if (nullptr == table || table->define_event == nullptr) {
        return -1;
    }
    AgentxxPluginEventSpec spec{};
    spec.struct_size = sizeof(AgentxxPluginEventSpec);
    spec.kind        = single ? AGENTXX_PLUGIN_EVENT_KIND_SINGLE : AGENTXX_PLUGIN_EVENT_KIND_STREAM;
    spec.name        = agentxx::plugin::PluginStringView::from(name.data(), name.size());
    spec.description = agentxx::plugin::PluginStringView::from(
        description.data(),
        description.size()
    );
    return table->define_event(inst->hostView(), &ref, &spec);
}

/// 表调用: 查一条事件 (返回 JSON 文本; 失败返回空串)
std::string queryEventOf(
    PluginInstance*         inst,
    const PluginxxScopeRef& ref,
    std::string_view        name,
    int32_t*                codeOut = nullptr
) {
    auto* table = eventTableOf(inst);
    if (nullptr == table || table->query_event == nullptr) {
        return {};
    }
    auto           nameSv = agentxx::plugin::PluginStringView::from(name.data(), name.size());
    PluginxxString out{nullptr, 0};
    const int32_t  rc = table->query_event(inst->hostView(), &ref, &nameSv, &out);
    if (codeOut != nullptr) {
        *codeOut = rc;
    }
    std::string text;
    if (nullptr != out.data) {
        text.assign(out.data, static_cast<size_t>(out.size));
        agentxx::plugin::PluginString::free(inst->hostView(), &out);
    }
    return text;
}

/// 表调用: 登记一个等待 (返回等待 id; 空串 = 失败)
std::string registerWaitOf(
    PluginInstance*         inst,
    const PluginxxScopeRef& ref,
    std::string_view        event,
    std::string_view        sessionId,
    std::string_view        mode      = "first",
    std::string_view        delivery  = "next-step",
    int32_t*                codeOut   = nullptr,
    int32_t*                stateOut  = nullptr
) {
    auto* table = eventTableOf(inst);
    if (nullptr == table || table->register_wait == nullptr) {
        return {};
    }
    AgentxxPluginEventWaitSpec spec{};
    spec.struct_size       = sizeof(AgentxxPluginEventWaitSpec);
    spec.mode              = mode == "each" ? AGENTXX_PLUGIN_EVENT_MODE_EACH
                             : mode == "all" ? AGENTXX_PLUGIN_EVENT_MODE_ALL
                                             : AGENTXX_PLUGIN_EVENT_MODE_FIRST;
    spec.replay            = 1;
    spec.notify_on_timeout = 0;
    spec.event             = agentxx::plugin::PluginStringView::from(event.data(), event.size());
    spec.session_id
        = agentxx::plugin::PluginStringView::from(sessionId.data(), sessionId.size());
    spec.delivery = agentxx::plugin::PluginStringView::from(delivery.data(), delivery.size());
    PluginxxString out{nullptr, 0};
    int32_t        state = AGENTXX_PLUGIN_EVENT_WAIT_ARMED;
    const int32_t  rc = table->register_wait(inst->hostView(), &ref, &spec, &out, &state);
    if (codeOut != nullptr) {
        *codeOut = rc;
    }
    if (stateOut != nullptr) {
        *stateOut = state;
    }
    std::string waitId;
    if (nullptr != out.data) {
        waitId.assign(out.data, static_cast<size_t>(out.size));
        agentxx::plugin::PluginString::free(inst->hostView(), &out);
    }
    return waitId;
}

/// 表调用: 取消一个等待
int32_t cancelWaitOf(PluginInstance* inst, const PluginxxScopeRef& ref, std::string_view waitId) {
    auto* table = eventTableOf(inst);
    if (nullptr == table || table->cancel_wait == nullptr) {
        return -1;
    }
    auto idSv = agentxx::plugin::PluginStringView::from(waitId.data(), waitId.size());
    return table->cancel_wait(inst->hostView(), &ref, &idSv);
}

/// 表调用: 事件清单 / 等待清单 (JSON 文本)
std::string listOf(PluginInstance* inst, const PluginxxScopeRef& ref, bool waits) {
    auto* table = eventTableOf(inst);
    if (nullptr == table) {
        return {};
    }
    PluginxxString out{nullptr, 0};
    const int32_t  rc = waits ? table->list_waits(inst->hostView(), &ref, nullptr, &out)
                              : table->list_events(inst->hostView(), &ref, nullptr, &out);
    if (rc != AGENTXX_PLUGIN_EVENT_OK || nullptr == out.data) {
        if (nullptr != out.data) {
            agentxx::plugin::PluginString::free(inst->hostView(), &out);
        }
        return {};
    }
    std::string text{out.data, static_cast<size_t>(out.size)};
    agentxx::plugin::PluginString::free(inst->hostView(), &out);
    return text;
}

/// 取走某会话的待注入事件输入正文
std::vector<std::string> takeEventInputs(const std::shared_ptr<agentxx::agent::Session>& session) {
    std::vector<std::string> out;
    if (!session) {
        return out;
    }
    for (auto& input : session->takePendingInputs(agentxx::agent::InputDelivery::NextStep)) {
        out.push_back(std::move(input.text));
    }
    return out;
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

/// 某个会话里还在等的等待数 (终态的记录会留到回收期, 因此不按条数断言)
size_t armedWaitsOf(const std::shared_ptr<agentxx::events::EventHub>& hub, std::string_view sessionId) {
    size_t count = 0;
    for (const auto& wait : hub->listWaits(sessionId)) {
        if (wait.state == agentxx::events::WaitState::Armed) {
            ++count;
        }
    }
    return count;
}

/// 加载示例插件 (挂载配置里给 `sessionId`, 让插件替该会话登记一个等待)
asio::awaitable<std::shared_ptr<PluginInstance>> loadExampleEvent(EventFixture& f) {
    agentxx::agent::PluginConfig cfg;
    cfg.path = findPluginDir("example_event");
    cfg.args = Json{{"sessionId", "s1"}};
    co_return co_await f.mgr->loadPluginAsync(cfg.path, &cfg);
}

/// 收尾: 卸载插件并拆掉本挂载, 让插件上下文与中枢回到基线 (未加载时是空操作)
asio::awaitable<void> unloadExampleEvent(EventFixture& f) {
    co_await f.mgr->unloadAsync("example_event", std::chrono::seconds{5});
    co_await f.mgr->unmountAgentAsync(f.agentId);
    // 挂载已从管理器拆掉: 上下文这边也放掉, 否则销毁时会报"挂载还挂着"
    f.ctx->pluginMount.reset();
}

} // namespace

// ===========================================================================
// 1. 声明与自查: 示例插件在 mount 里声明事件源并发布初始值
// ===========================================================================
namespace {

asio::awaitable<void> test_declaration_from_plugin() {
    auto f = co_await makeFixture();
    XX_TEST_EXPECT_TRUE(f.hub != nullptr);
    XX_TEST_EXPECT_EQ(f.hub->eventCount(), size_t{0});

    auto inst = co_await loadExampleEvent(f);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }

    // 事件记录: 两条, 归属 = 声明它的插件实例
    XX_TEST_EXPECT_EQ(f.hub->eventCount(), size_t{2});
    const auto build = f.hub->queryEvent("plugin.example_event.build");
    XX_TEST_EXPECT_TRUE(build.has_value());
    if (build.has_value()) {
        XX_TEST_EXPECT_EQ(build->owner, std::string{"plugin:example_event"});
        XX_TEST_EXPECT_EQ(build->kind, agentxx::events::EventKind::Stream);
        XX_TEST_EXPECT_EQ(build->state, agentxx::events::EventState::Open);
        XX_TEST_EXPECT_EQ(build->published, uint64_t{1});
        XX_TEST_EXPECT_EQ(build->historyLimit, size_t{4});
        XX_TEST_EXPECT_TRUE(contains(build->description, "构建进度"));
    }
    // 单值事件: 首个值到达后自动结束
    const auto ready = f.hub->queryEvent("plugin.example_event.ready");
    XX_TEST_EXPECT_TRUE(ready.has_value());
    if (ready.has_value()) {
        XX_TEST_EXPECT_EQ(ready->kind, agentxx::events::EventKind::Single);
        XX_TEST_EXPECT_EQ(ready->state, agentxx::events::EventState::Ended);
        XX_TEST_EXPECT_EQ(ready->published, uint64_t{1});
        XX_TEST_EXPECT_TRUE(ready->waitCount == 0);
    }

    // 事件名在模型可见的清单里 (带说明), 模型据此知道该等什么
    {
        const auto listJson = f.hub->listEventsJson("plugin.example_event.");
        const auto count    = listJson.value("count", int64_t{0});
        XX_TEST_EXPECT_EQ(count, int64_t{2});
        const auto text = listJson.dump();
        XX_TEST_EXPECT_TRUE(contains(text, "plugin.example_event.build"));
        XX_TEST_EXPECT_TRUE(contains(text, "构建进度"));
    }

    // 注册清单: 事件记录与等待都算在本实例名下
    {
        const auto inv = f.mgr->registrationInventory(*inst);
        XX_TEST_EXPECT_EQ(inv.eventRecords, size_t{2});
        XX_TEST_EXPECT_EQ(inv.eventWaits, size_t{1});
        XX_TEST_EXPECT_GE(inv.total(), size_t{3});
    }

    // 插件替会话登记的那个等待: 注册前已经发出的值按回放投递进会话
    {
        const auto waits = f.hub->listWaits("s1");
        XX_TEST_EXPECT_EQ(waits.size(), size_t{1});
        if (waits.size() == 1) {
            XX_TEST_EXPECT_EQ(waits[0].spec.event, std::string{"plugin.example_event.build"});
            XX_TEST_EXPECT_EQ(waits[0].spec.mode, std::string{"each"});
            XX_TEST_EXPECT_EQ(waits[0].spec.owner, std::string{"plugin:example_event"});
            XX_TEST_EXPECT_EQ(waits[0].deliveries, uint64_t{1}); ///< 回放了 mount 里那个值
        }
        const auto inputs = takeEventInputs(f.session);
        XX_TEST_EXPECT_EQ(inputs.size(), size_t{1});
        if (inputs.size() == 1) {
            XX_TEST_EXPECT_TRUE(contains(inputs[0], "[Event] plugin.example_event.build"));
            XX_TEST_EXPECT_TRUE(contains(inputs[0], "\"step\":1"));
        }
    }

    co_await unloadExampleEvent(f);
}

// ===========================================================================
// 2. 发布 / 结束 / 移除 / 查询: 插件侧的写入口
// ===========================================================================
asio::awaitable<void> test_emit_end_remove() {
    auto       f    = co_await makeFixture();
    auto       inst = co_await loadExampleEvent(f);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    const auto ref = agentRefOf(f);

    // 发布的第二个值经中枢线程写入 (`emit_value` 只排队不等待)
    XX_TEST_EXPECT_EQ(
        emitValue(inst.get(), ref, "plugin.example_event.build", R"({"step":2,"total":2})"),
        int32_t{AGENTXX_PLUGIN_EVENT_OK}
    );
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        const auto rec = f.hub->queryEvent("plugin.example_event.build");
        return rec.has_value() && rec->published == 2;
    }));

    // 查询: 字段与中枢记录一致
    {
        int32_t    code = 0;
        const auto text = queryEventOf(inst.get(), ref, "plugin.example_event.build", &code);
        XX_TEST_EXPECT_EQ(code, int32_t{AGENTXX_PLUGIN_EVENT_OK});
        auto json = Json::parse(text);
        XX_TEST_EXPECT_EQ(json.value("name", std::string{}), std::string{"plugin.example_event.build"});
        XX_TEST_EXPECT_EQ(json.value("kind", std::string{}), std::string{"stream"});
        XX_TEST_EXPECT_EQ(json.value("state", std::string{}), std::string{"open"});
        XX_TEST_EXPECT_EQ(json.value("owner", std::string{}), std::string{"plugin:example_event"});
        XX_TEST_EXPECT_EQ(json.value("published", int64_t{0}), int64_t{2});
        XX_TEST_EXPECT_EQ(json.value("history", int64_t{0}), int64_t{2});
        XX_TEST_EXPECT_EQ(json.value("waits", int64_t{0}), int64_t{1});
        XX_TEST_EXPECT_TRUE(json.contains("last"));
        XX_TEST_EXPECT_EQ(json["last"].value("step", 0), 2);
    }

    // 结束一条流: 幂等, 收尾信息进 summary
    XX_TEST_EXPECT_EQ(
        endEventOf(inst.get(), ref, "plugin.example_event.build", R"({"reason":"done"})"),
        int32_t{AGENTXX_PLUGIN_EVENT_OK}
    );
    {
        const auto rec = f.hub->queryEvent("plugin.example_event.build");
        XX_TEST_EXPECT_TRUE(rec.has_value());
        if (rec.has_value()) {
            XX_TEST_EXPECT_EQ(rec->state, agentxx::events::EventState::Ended);
            XX_TEST_EXPECT_EQ(rec->summary.value("reason", std::string{}), std::string{"done"});
        }
    }
    XX_TEST_EXPECT_EQ(
        endEventOf(inst.get(), ref, "plugin.example_event.build"),
        int32_t{AGENTXX_PLUGIN_EVENT_OK}
    );
    // 结束后再到值: 记录重新打开 (流式事件允许续上)
    XX_TEST_EXPECT_EQ(
        emitValue(inst.get(), ref, "plugin.example_event.build", R"({"step":3})"),
        int32_t{AGENTXX_PLUGIN_EVENT_OK}
    );
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        const auto rec = f.hub->queryEvent("plugin.example_event.build");
        return rec.has_value() && rec->state == agentxx::events::EventState::Open;
    }));

    // 移除记录: 之后查不到, 还挂在它上面的等待按"已结束且无值"收尾
    XX_TEST_EXPECT_EQ(
        removeEventOf(inst.get(), ref, "plugin.example_event.build"),
        int32_t{AGENTXX_PLUGIN_EVENT_OK}
    );
    XX_TEST_EXPECT_FALSE(f.hub->queryEvent("plugin.example_event.build").has_value());
    XX_TEST_EXPECT_EQ(f.hub->eventCount(), size_t{1}); ///< 只剩 example_event.ready
    {
        // 挂在上面的等待被收尾 (记录保留到回收期, 但不再等)
        for (const auto& wait : f.hub->listWaits("s1")) {
            XX_TEST_EXPECT_TRUE(wait.state != agentxx::events::WaitState::Armed);
        }
    }
    XX_TEST_EXPECT_EQ(
        removeEventOf(inst.get(), ref, "plugin.example_event.build"),
        int32_t{AGENTXX_PLUGIN_EVENT_ERR_NOT_FOUND}
    );

    // 插件发的值也能被模型侧等到 (回放): 单值事件的值已经结束, watch 直接拿到
    {
        agentxx::events::EventWaitSpec spec;
        spec.event          = "plugin.example_event.ready";
        spec.mode           = "first";
        spec.delivery       = "none";
        auto result         = f.hub->watch(std::move(spec));
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            XX_TEST_EXPECT_EQ(result->state, agentxx::events::WaitState::Satisfied);
            XX_TEST_EXPECT_EQ(result->values.size(), size_t{1});
            if (!result->values.empty()) {
                XX_TEST_EXPECT_EQ(result->values.back().value("plugin", std::string{}),
                                  std::string{"example_event"});
            }
        }
    }

    co_await unloadExampleEvent(f);
}

// ===========================================================================
// 3. 等待与投递: 插件替会话登记等待, 值到达后成为会话输入
// ===========================================================================
asio::awaitable<void> test_register_wait_delivery() {
    auto       f    = co_await makeFixture();
    auto       inst = co_await loadExampleEvent(f);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    const auto ref = agentRefOf(f);
    // 另开一个会话: 示例插件在 mount 里已经替 s1 登记过一个等待, 这里用干净的那个
    const auto other = f.ctx->sessions->getOrCreate("s2");
    takeEventInputs(f.session); // 清掉 mount 期间回放进 s1 的那条

    // 单值等待: 值到达后投递一次并结束等待
    int32_t    code  = 0;
    int32_t    state = -1;
    const auto waitId
        = registerWaitOf(inst.get(), ref, "plugin.example_event.ready", "s2", "first", "next-step", &code, &state);
    XX_TEST_EXPECT_EQ(code, int32_t{AGENTXX_PLUGIN_EVENT_OK});
    XX_TEST_EXPECT_TRUE(!waitId.empty());
    // 单值事件已经结束且已有值: 注册瞬间就满足
    XX_TEST_EXPECT_EQ(state, int32_t{AGENTXX_PLUGIN_EVENT_WAIT_SATISFIED});
    {
        const auto inputs = takeEventInputs(other);
        XX_TEST_EXPECT_GE(inputs.size(), size_t{1});
    }

    // 多值等待 (每条都要): 两次发布各成一条输入
    const auto eachWait
        = registerWaitOf(inst.get(), ref, "plugin.example_event.build", "s2", "each", "next-step", &code, &state);
    XX_TEST_EXPECT_EQ(code, int32_t{AGENTXX_PLUGIN_EVENT_OK});
    XX_TEST_EXPECT_EQ(state, int32_t{AGENTXX_PLUGIN_EVENT_WAIT_ARMED});
    XX_TEST_EXPECT_FALSE(eachWait.empty());
    takeEventInputs(other); // 回放的那条

    emitValue(inst.get(), ref, "plugin.example_event.build", R"({"step":2})");
    const bool delivered = co_await waitFor([&] {
        const auto rec = f.hub->queryEvent("plugin.example_event.build");
        return rec.has_value() && rec->published >= 2;
    });
    XX_TEST_EXPECT_TRUE(delivered);
    {
        const auto inputs = takeEventInputs(other);
        XX_TEST_EXPECT_EQ(inputs.size(), size_t{1});
        if (inputs.size() == 1) {
            XX_TEST_EXPECT_TRUE(contains(inputs[0], "\"step\":2"));
        }
    }

    // 取消: 第一次成功, 第二次已经收尾
    XX_TEST_EXPECT_EQ(
        cancelWaitOf(inst.get(), ref, eachWait),
        int32_t{AGENTXX_PLUGIN_EVENT_OK}
    );
    XX_TEST_EXPECT_EQ(
        cancelWaitOf(inst.get(), ref, eachWait),
        int32_t{AGENTXX_PLUGIN_EVENT_ERR_NOT_FOUND}
    );

    // 等待清单 (插件自查): 含事件名、模式、状态
    {
        const auto text = listOf(inst.get(), ref, /*waits=*/true);
        XX_TEST_EXPECT_TRUE(!text.empty());
        const auto json = Json::parse(text);
        XX_TEST_EXPECT_TRUE(json.contains("waits"));
        XX_TEST_EXPECT_TRUE(contains(text, "plugin.example_event.build"));
        XX_TEST_EXPECT_TRUE(contains(text, "each"));
    }
    {
        const auto text = listOf(inst.get(), ref, /*waits=*/false);
        const auto json = Json::parse(text);
        XX_TEST_EXPECT_EQ(json.value("count", int64_t{0}), int64_t{2});
    }

    co_await unloadExampleEvent(f);
}

// ===========================================================================
// 4. 失败路径: 参数 / 冲突 / 不存在 / 未启用 / 引用无效
// ===========================================================================
asio::awaitable<void> test_failure_paths() {
    auto       f    = co_await makeFixture();
    auto       inst = co_await loadExampleEvent(f);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    const auto ref = agentRefOf(f);

    // 名称只要在 plugin.* 里就行 (不限定插件名: 同一组织多插件可共用一段);
    // 裸名/空名称一律拒绝 (框架不补前缀)
    XX_TEST_EXPECT_EQ(
        defineEventOf(inst.get(), ref, "plugin.other_plugin.x", false),
        int32_t{AGENTXX_PLUGIN_EVENT_OK}
    );
    XX_TEST_EXPECT_EQ(
        removeEventOf(inst.get(), ref, "plugin.other_plugin.x"),
        int32_t{AGENTXX_PLUGIN_EVENT_OK}
    );
    XX_TEST_EXPECT_EQ(
        defineEventOf(inst.get(), ref, "build", false),
        int32_t{AGENTXX_PLUGIN_EVENT_ERR_ARG}
    );
    XX_TEST_EXPECT_EQ(
        defineEventOf(inst.get(), ref, "", false),
        int32_t{AGENTXX_PLUGIN_EVENT_ERR_ARG}
    );
    // 同名不同种类: 冲突
    XX_TEST_EXPECT_EQ(
        defineEventOf(inst.get(), ref, "plugin.example_event.build", /*single=*/true),
        int32_t{AGENTXX_PLUGIN_EVENT_ERR_CONFLICT}
    );
    // 同名同种类: 幂等成功 (只补充说明)
    XX_TEST_EXPECT_EQ(
        defineEventOf(inst.get(), ref, "plugin.example_event.build", /*single=*/false),
        int32_t{AGENTXX_PLUGIN_EVENT_OK}
    );
    // 查不存在的事件
    {
        int32_t code = 0;
        queryEventOf(inst.get(), ref, "plugin.example_event.missing", &code);
        XX_TEST_EXPECT_EQ(code, int32_t{AGENTXX_PLUGIN_EVENT_ERR_NOT_FOUND});
    }
    // 结束不存在的事件
    XX_TEST_EXPECT_EQ(
        endEventOf(inst.get(), ref, "plugin.example_event.missing"),
        int32_t{AGENTXX_PLUGIN_EVENT_ERR_NOT_FOUND}
    );
    // 值不是合法 JSON
    XX_TEST_EXPECT_EQ(
        emitValue(inst.get(), ref, "plugin.example_event.build", "{not json"),
        int32_t{AGENTXX_PLUGIN_EVENT_ERR_ARG}
    );
    // 等待参数非法: 投递模式必须是四种之一
    {
        int32_t code = 0;
        registerWaitOf(inst.get(), ref, "plugin.example_event.build", "s1", "first", "bogus", &code);
        XX_TEST_EXPECT_EQ(code, int32_t{AGENTXX_PLUGIN_EVENT_ERR_ARG});
        registerWaitOf(inst.get(), ref, "", "s1", "first", "next-step", &code);
        XX_TEST_EXPECT_EQ(code, int32_t{AGENTXX_PLUGIN_EVENT_ERR_ARG});
        // 引用无效: 注册类返回 AGENTXX_PLUGIN_ERR_NO_AGENT
        PluginxxScopeRef bad{};
        bad.struct_size = sizeof(PluginxxScopeRef);
        auto badId      = agentxx::plugin::PluginStringView::fromCstr("no-such-agent");
        bad.id          = badId;
        bad.seq         = 1;
        registerWaitOf(inst.get(), bad, "plugin.example_event.build", "s1", "first", "next-step", &code);
        XX_TEST_EXPECT_EQ(code, int32_t{AGENTXX_PLUGIN_ERR_NO_AGENT});
    }

    // 没有中枢的嵌入环境: 写入口一律返回不支持 (主程序恒常驻, 这条只是兼容老环境的降级路径)
    {
        auto dry     = co_await makeFixture(/*withHub=*/false);
        auto dryInst = co_await loadExampleEvent(dry);
        // 插件降级: 没有事件中枢时 mount 直接返回成功, 不声明任何事件
        XX_TEST_EXPECT_TRUE(dryInst != nullptr);
        if (dryInst) {
            const auto dryRef = agentRefOf(dry);
            XX_TEST_EXPECT_EQ(
                defineEventOf(dryInst.get(), dryRef, "plugin.example_event.dry", false),
                int32_t{AGENTXX_PLUGIN_EVENT_ERR_UNSUPPORTED}
            );
            XX_TEST_EXPECT_EQ(
                emitValue(dryInst.get(), dryRef, "plugin.example_event.dry", "{}"),
                int32_t{AGENTXX_PLUGIN_EVENT_ERR_UNSUPPORTED}
            );
        }
        XX_TEST_EXPECT_TRUE(dry.hub == nullptr);
        co_await unloadExampleEvent(dry);
    }

    co_await unloadExampleEvent(f);
}

// ===========================================================================
// 5. 生命周期: 禁用删记录并取消等待, 启用重新声明, 卸载回到基线
// ===========================================================================
asio::awaitable<void> test_lifecycle_reversible() {
    auto       f    = co_await makeFixture();
    auto       inst = co_await loadExampleEvent(f);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    XX_TEST_EXPECT_EQ(f.hub->eventCount(), size_t{2});
    XX_TEST_EXPECT_EQ(f.hub->listWaits("s1").size(), size_t{1});

    // 禁用: 事件记录删除、等待取消 (中枢里不留"没人发"的事件)
    f.mgr->disable("example_event");
    XX_TEST_EXPECT_FALSE(inst->enabled);
    XX_TEST_EXPECT_EQ(f.hub->eventCount(), size_t{0});
    XX_TEST_EXPECT_EQ(armedWaitsOf(f.hub, "s1"), size_t{0});
    {
        const auto inv = f.mgr->registrationInventory(*inst);
        XX_TEST_EXPECT_EQ(inv.eventRecords, size_t{0});
        XX_TEST_EXPECT_EQ(inv.eventWaits, size_t{0});
    }

    // 启用: `mount` 重新声明 (投递到 io 线程执行, 轮询等待)
    f.mgr->enable("example_event");
    const bool restored = co_await waitFor([&] {
        return f.hub->eventCount() == 2 && armedWaitsOf(f.hub, "s1") == 1;
    });
    XX_TEST_EXPECT_TRUE(restored);
    if (restored) {
        const auto rec = f.hub->queryEvent("plugin.example_event.build");
        XX_TEST_EXPECT_TRUE(rec.has_value());
        if (rec.has_value()) {
            XX_TEST_EXPECT_EQ(rec->owner, std::string{"plugin:example_event"});
        }
    }

    // 卸载: 中枢回到基线
    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_event", std::chrono::seconds{5}));
    XX_TEST_EXPECT_EQ(f.hub->eventCount(), size_t{0});
    XX_TEST_EXPECT_EQ(armedWaitsOf(f.hub, ""), size_t{0});
}

} // namespace

asio::awaitable<TestResult> run_plugin_event_tests() {
    const int passedBefore = g_pe_passed;
    const int failedBefore = g_pe_failed;

    try {
        co_await test_declaration_from_plugin();
        co_await test_emit_end_remove();
        co_await test_register_wait_delivery();
        co_await test_failure_paths();
        co_await test_lifecycle_reversible();
    } catch (const std::exception& e) {
        TEST_FAIL << "plugin_event suite exception: " << e.what() << std::endl;
        g_pe_failed++;
    }

    co_return TestResult{g_pe_passed - passedBefore, g_pe_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
