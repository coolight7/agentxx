/// test_plugin_hooks —— 钩子处理器清单与优先级
/// (接口表 `agentxx.agent.hooks` / `agentxx.agent.hooks_ex`)
///
/// 背景: 钩子过去只有"一个实例一个点一个处理器、按装载顺序执行"这一种形态,
/// 也读不到"这个点上有谁"。本轮补上注册表与清单, 且**默认行为不变** —— 不声明
/// 优先级时顺序仍是登记顺序 (插件装载顺序)。本模块验证这两面:
///
/// 覆盖:
/// 1. 顺序: `(层, priority 升序, 登记序号)`; 不声明优先级时 = 登记顺序;
///    `core` 层排在 `plugin` 层之后
/// 2. 一个点多个处理器: 同一实例同一个点可以登记多个 (各自句柄), 按句柄精确撤销;
///    别的实例的句柄被拒绝
/// 3. 基础登记 (`register_hook`) 与扩展登记 (`register_hook_ex`) 互相独立:
///    基础登记覆盖式 (一个点一个), 撤销基础登记不影响扩展登记的处理器
/// 4. 优先级越界裁剪到上下限 (不拒绝登记)
/// 5. 宿主级单派发器: 首次登记挂到中间件链上, 没有处理器时摘除
///    (轮次执行中先停用, 轮末 flush 摘除)
/// 6. 实际派发: 按注册表顺序执行 (含 `core` 层), 单个处理器失败不影响后续;
///    core 层处理器必须是同步的 (返回操作句柄会被请求取消并记警告)
/// 7. 清单: `hooksJson()` 的字段与顺序、装配快照同源, 开发者模式控制 `stat` 段
/// 8. 生命周期: 禁用摘除处理器 (记录保留)、启用按 start 重新登记、卸载后回到基线
/// 9. 插件面: kit 的 `hookEx` / `listHooks` / `unhookEx` 经真实宿主走一遍
#include "agentxx-test/plugin/test_plugin_hooks.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/feature/feature.h"
#include "agentxx/feature/points.h"
#include "agentxx/feature/registry.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "neograph/graph/cancel.h"
#include "neograph/graph/node.h"
#include "neograph/graph/registry.h"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ph_passed = 0;
int g_ph_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ph_passed
#define XX_TEST_FAILED g_ph_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

/// 定位插件库目录 (与其他插件测试同一策略: exe 同目录优先, cwd 回退)
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

/// 最小宿主夹具: AgentContext + 中间件上下文 + 总线 + 工具注册表 + 功能点注册表
/// + 插件管理器
///
/// 功能点注册表按 `BaseAgent::init` 的顺序装配 (核心点先声明), 否则示例插件
/// `example_feature` 的 start 事务会因"点未声明"失败 —— 本模块要用它做
/// "后装载但优先级更高"的对照插件。
struct Fixture {
    std::shared_ptr<agentxx::agent::AgentContext>    ctx;
    std::shared_ptr<agentxx::plugin::PluginManager>  mgr;
    agentxx::feature::ContextPoints                  points;
};

asio::awaitable<Fixture> makeFixture() {
    Fixture f;
    f.ctx                          = std::make_shared<agentxx::agent::AgentContext>();
    f.ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    f.ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    f.ctx->bus = std::make_shared<agentxx::events::EventBus>(co_await asio::this_coro::executor);
    f.ctx->toolRegistry  = std::make_shared<agentxx::plugin::ToolRegistry>();
    f.ctx->graphRegistry = std::make_shared<neograph::graph::GraphRegistry>();

    f.ctx->features = std::make_shared<agentxx::feature::Registry>();
    agentxx::feature::ContextPointOptions pointOptions;
    f.points = agentxx::feature::registerContextPoints(*f.ctx->features, pointOptions);

    f.mgr                = std::make_shared<agentxx::plugin::PluginManager>(f.ctx);
    f.ctx->pluginManager = f.mgr;
    // 装配 io executor (与 BaseAgent::init 一致): 跨线程 vtable 调用经真实 post
    f.mgr->setIoExecutor(co_await asio::this_coro::executor);
    co_return f;
}

/// 轮询等待条件成立 (插件侧登记与生命周期事务都投递到 io 线程)
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

/// 清单里某个点的处理器顺序 (与 `handlersOf` 同源, 这里走 JSON 路径)
std::vector<std::string> jsonOrder(const utilxx_base::Json& hooksJson, std::string_view pointName) {
    std::vector<std::string> out;
    if (!hooksJson.contains("points") || !hooksJson["points"].is_array()) {
        return out;
    }
    for (const auto& point : hooksJson["points"]) {
        if (point.value("name", std::string{}) != pointName) {
            continue;
        }
        if (!point.contains("handlers") || !point["handlers"].is_array()) {
            break;
        }
        for (const auto& h : point["handlers"]) {
            out.push_back(fmt::format(
                "{}#{}",
                h.value("owner", std::string{}),
                h.value("handle", 0)
            ));
        }
        break;
    }
    return out;
}

/// 一次派发用的最小图输入 (与中间件派发器收到的形状一致)
void fillNodeInput(neograph::graph::RunContext& runCtx) {
    runCtx.thread_id = "hook_test_session";
}

/// core 层处理器的调用记录 (顺序验证用)
struct CoreHookTrace {
    std::vector<std::string> calls;
};

void* PLUGINXX_CALL coreHookStart(
    void*                         user_data,
    int32_t                       point,
    const PluginxxStringView*     node_input_json,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    (void)notify;
    (void)error_out;
    auto* trace = static_cast<CoreHookTrace*>(user_data);
    if (trace != nullptr) {
        trace->calls.push_back(fmt::format(
            "point={} input={}",
            point,
            (node_input_json != nullptr && node_input_json->data != nullptr)
                ? std::string{node_input_json->data, static_cast<size_t>(node_input_json->size)}
                : std::string{"-"}
        ));
    }
    return nullptr;
}

/// 出错的 core 层处理器 (派发必须只记日志继续)
void* PLUGINXX_CALL coreHookThrows(
    void*,
    int32_t,
    const PluginxxStringView*,
    const PluginxxOperatorNotify*,
    PluginxxString*
) {
    throw std::runtime_error("core hook boom");
}

/// "想异步"的 core 层处理器 (返回操作句柄): 派发应当请求取消并继续
struct AsyncCoreHookState {
    int   runs    = 0;
    int   cancels = 0;
    void* fakeOp  = nullptr;
};

void PLUGINXX_CALL coreHookCancelAsync(void* user_data, void*) {
    auto* state = static_cast<AsyncCoreHookState*>(user_data);
    if (state != nullptr) {
        ++state->cancels;
    }
}

void* PLUGINXX_CALL coreHookAsync(
    void*,
    int32_t,
    const PluginxxStringView*,
    const PluginxxOperatorNotify*,
    PluginxxString*
) {
    // core 层不支持异步: 返回非空句柄会被宿主记警告并请求取消
    return reinterpret_cast<void*>(static_cast<intptr_t>(0x1));
}

/// 登记一个 core 层处理器
int64_t addCoreHook(
    Fixture&        f,
    bool            throwsInstead,
    int32_t         priority,
    std::string     module,
    CoreHookTrace*  trace
) {
    AgentxxPluginHookSpec spec{};
    spec.point      = AGENTXX_PLUGIN_HOOK_AGENT_START;
    spec._reserved  = 0;
    spec.hook_start = throwsInstead ? &coreHookThrows : &coreHookStart;
    spec.hook_cancel = nullptr;
    spec.user_data   = trace;
    return f.mgr->addCoreHookHandler(
        AGENTXX_PLUGIN_HOOK_AGENT_START,
        priority,
        std::move(module),
        spec
    );
}

/// 登记一个插件层处理器 (绕过 kit, 直接走宿主入口)
int64_t addPluginHook(
    Fixture&                        f,
    agentxx::plugin::PluginInstance* inst,
    int32_t                         priority,
    std::string_view                ownerTag,
    std::string_view                depict
) {
    AgentxxPluginHookSpecEx spec{};
    spec.struct_size = sizeof(AgentxxPluginHookSpecEx);
    spec.point       = AGENTXX_PLUGIN_HOOK_AGENT_START;
    spec.priority    = priority;
    spec.flags       = 0;
    spec.owner_tag   = agentxx::plugin::PluginStringView::from(ownerTag);
    spec.depict      = agentxx::plugin::PluginStringView::from(depict);
    spec.user_data   = nullptr;
    spec.hook_start  = [](void*, int32_t, const PluginxxStringView*, const PluginxxOperatorNotify* notify, PluginxxString*) -> void* {
        // 快同步处理器: 立即完成 (钩子结果被丢弃)
        if (notify != nullptr && notify->done != nullptr) {
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        }
        return nullptr;
    };
    spec.hook_cancel = nullptr;
    int64_t handle   = 0;
    const int rc     = f.mgr->registerHookEx(inst, &spec, &handle);
    return rc == 0 ? handle : 0;
}

} // namespace

// ===========================================================================
// 1. 顺序: 层 -> 优先级 -> 登记序号 (默认顺序与登记顺序一致)
// ===========================================================================
namespace {

asio::awaitable<void> test_order_by_layer_priority_seq() {
    auto      f          = co_await makeFixture();
    const auto pluginA    = findPluginDir("example_plugin");   // 基础登记, 优先级 0
    const auto pluginB    = findPluginDir("example_feature");  // 扩展登记, 优先级 -10

    auto instA = co_await f.mgr->loadPluginAsync(pluginA);
    XX_TEST_EXPECT_TRUE(instA != nullptr);
    auto instB = co_await f.mgr->loadPluginAsync(pluginB);
    XX_TEST_EXPECT_TRUE(instB != nullptr);
    if (!instA || !instB) {
        co_return;
    }

    // example_feature 用优先级 -10 登记 (装载在 example_plugin 之后), 应排在前面
    auto handlers = f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START);
    XX_TEST_EXPECT_EQ(handlers.size(), size_t{2});
    if (handlers.size() == 2) {
        XX_TEST_EXPECT_EQ(handlers[0].owner, std::string{"plugin:example_feature"});
        XX_TEST_EXPECT_EQ(handlers[0].priority, int32_t{-10});
        XX_TEST_EXPECT_EQ(handlers[0].layer, std::string{"plugin"});
        XX_TEST_EXPECT_EQ(handlers[0].ownerTag, std::string{"example_feature"});
        XX_TEST_EXPECT_TRUE(handlers[0].depict.find("有序登记") != std::string::npos);
        XX_TEST_EXPECT_EQ(handlers[1].owner, std::string{"plugin:example_plugin"});
        XX_TEST_EXPECT_EQ(handlers[1].priority, int32_t{0});
        XX_TEST_EXPECT_TRUE(handlers[0].seq > handlers[1].seq); ///< 优先级压过登记序号
    }

    // 同优先级 → 按登记序号: 再给 example_plugin 登记一个优先级 0 的处理器 (句柄更大)
    int64_t       baseHandleA = 0;
    for (const auto& record : instA->hookRegistrations) {
        if (record.base) {
            baseHandleA = record.handle;
        }
    }
    XX_TEST_EXPECT_GE(baseHandleA, int64_t{1});
    const int64_t extra = addPluginHook(f, instA.get(), 0, "test", "第二个处理器 (同优先级)");
    XX_TEST_EXPECT_GE(extra, int64_t{1});
    handlers = f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START);
    XX_TEST_EXPECT_EQ(handlers.size(), size_t{3});
    if (handlers.size() == 3) {
        XX_TEST_EXPECT_EQ(handlers[0].owner, std::string{"plugin:example_feature"});
        XX_TEST_EXPECT_EQ(handlers[1].owner, std::string{"plugin:example_plugin"});
        XX_TEST_EXPECT_EQ(handlers[1].handle, baseHandleA); ///< 先登记的 (基础登记) 在前
        XX_TEST_EXPECT_EQ(handlers[2].handle, extra);
    }

    // core 层排在 plugin 层之后 (优先级再小也越不过层)
    CoreHookTrace trace;
    const int64_t coreHandle = addCoreHook(f, /*throwsInstead=*/false, -100, "test", &trace);
    XX_TEST_EXPECT_GE(coreHandle, int64_t{1});
    handlers = f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START);
    XX_TEST_EXPECT_EQ(handlers.size(), size_t{4});
    if (handlers.size() == 4) {
        XX_TEST_EXPECT_EQ(handlers[3].layer, std::string{"core"});
        XX_TEST_EXPECT_EQ(handlers[3].owner, std::string{"core:test"});
        XX_TEST_EXPECT_EQ(handlers[3].priority, int32_t{-100});
    }

    // 撤销 core 处理器 (按句柄; core 层没有实例归属)
    XX_TEST_EXPECT_TRUE(f.mgr->removeCoreHookHandler(coreHandle));
    XX_TEST_EXPECT_FALSE(f.mgr->removeCoreHookHandler(coreHandle));
    XX_TEST_EXPECT_EQ(
        f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).size(),
        size_t{3}
    );

    // 卸载插件: 处理器随实例一起走
    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_plugin", std::chrono::seconds{5}));
    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_feature", std::chrono::seconds{5}));
    XX_TEST_EXPECT_TRUE(f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).empty());
}

} // namespace

// ===========================================================================
// 2. 一个点多个处理器 + 按句柄撤销 + 归属校验
// ===========================================================================
namespace {

asio::awaitable<void> test_multiple_handlers_and_owner_check() {
    auto      f  = co_await makeFixture();
    auto instA  = co_await f.mgr->loadPluginAsync(findPluginDir("example_plugin"));
    auto instB  = co_await f.mgr->loadPluginAsync(findPluginDir("example_feature"));
    XX_TEST_EXPECT_TRUE(instA != nullptr && instB != nullptr);
    if (!instA || !instB) {
        co_return;
    }
    const size_t base = f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).size();
    XX_TEST_EXPECT_EQ(base, size_t{2});

    // 同一个实例在同一个点登记两个扩展处理器
    const int64_t h1 = addPluginHook(f, instB.get(), 5, "test", "处理器 1");
    const int64_t h2 = addPluginHook(f, instB.get(), 5, "test", "处理器 2");
    XX_TEST_EXPECT_GE(h1, int64_t{1});
    XX_TEST_EXPECT_GE(h2, int64_t{1});
    XX_TEST_EXPECT_FALSE(h1 == h2);
    auto handlers = f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START);
    XX_TEST_EXPECT_EQ(handlers.size(), base + 2);
    // 同一实例在注册表里出现三次 (插件自己 1 + 本用例 2), 实例记录同数
    XX_TEST_EXPECT_EQ(f.mgr->registrationInventory(*instB).hooks, size_t{3});
    XX_TEST_EXPECT_EQ(instB->hookRegistrations.size(), size_t{3});
    // example_feature 只走扩展入口登记 (没有基础登记记录)
    XX_TEST_EXPECT_EQ(
        std::count_if(
            instB->hookRegistrations.begin(),
            instB->hookRegistrations.end(),
            [](const agentxx::plugin::PluginInstance::HookRegistration& r) {
                return r.base;
            }
        ),
        ptrdiff_t{0}
    );
    XX_TEST_EXPECT_EQ(
        std::count_if(
            instA->hookRegistrations.begin(),
            instA->hookRegistrations.end(),
            [](const agentxx::plugin::PluginInstance::HookRegistration& r) {
                return r.base;
            }
        ),
        ptrdiff_t{1}
    );

    // 别人的句柄: 拒绝撤销, 处理器仍在
    XX_TEST_EXPECT_EQ(f.mgr->unregisterHookEx(instA.get(), h1), -1);
    XX_TEST_EXPECT_EQ(f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).size(), base + 2);
    // 不存在的句柄
    XX_TEST_EXPECT_EQ(f.mgr->unregisterHookEx(instB.get(), 99999), -1);

    // 自己的句柄: 精确撤销其中一个, 另一个不受影响
    XX_TEST_EXPECT_EQ(f.mgr->unregisterHookEx(instB.get(), h1), 0);
    handlers = f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START);
    XX_TEST_EXPECT_EQ(handlers.size(), base + 1);
    XX_TEST_EXPECT_TRUE(
        std::any_of(handlers.begin(), handlers.end(), [h2](const auto& h) {
            return h.handle == h2;
        })
    );
    XX_TEST_EXPECT_EQ(f.mgr->unregisterHookEx(instB.get(), h2), 0);
    XX_TEST_EXPECT_EQ(f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).size(), base);
    XX_TEST_EXPECT_EQ(instB->hookRegistrations.size(), size_t{1}); ///< 只剩 example_feature 自己的登记

    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_plugin", std::chrono::seconds{5}));
    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_feature", std::chrono::seconds{5}));
}

} // namespace

// ===========================================================================
// 3. 基础登记与扩展登记互相独立 + 优先级越界裁剪
// ===========================================================================
namespace {

asio::awaitable<void> test_base_and_ex_independent_and_clamp() {
    auto      f     = co_await makeFixture();
    auto      inst  = co_await f.mgr->loadPluginAsync(findPluginDir("example_plugin"));
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    XX_TEST_EXPECT_EQ(f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).size(), size_t{1});

    // 扩展登记: 优先级越界 -> 裁剪到上限 (不拒绝)
    const int64_t high = addPluginHook(f, inst.get(), 1000000, "test", "越界优先级 (上限)");
    XX_TEST_EXPECT_GE(high, int64_t{1});
    auto handlers = f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START);
    XX_TEST_EXPECT_EQ(handlers.size(), size_t{2});
    if (handlers.size() == 2) {
        XX_TEST_EXPECT_EQ(handlers[1].priority, agentxx::feature::kPriorityMax);
    }
    // 下限
    const int64_t low = addPluginHook(f, inst.get(), -1000000, "test", "越界优先级 (下限)");
    XX_TEST_EXPECT_GE(low, int64_t{1});
    handlers = f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START);
    XX_TEST_EXPECT_EQ(handlers.size(), size_t{3});
    if (handlers.size() == 3) {
        XX_TEST_EXPECT_EQ(handlers[0].priority, agentxx::feature::kPriorityMin);
        XX_TEST_EXPECT_EQ(handlers[0].handle, low);
    }

    // 基础登记重复 (覆盖式): 仍是 1 条基础记录, 句柄换新
    const int64_t baseHandleBefore = [&]() -> int64_t {
        for (const auto& r : inst->hookRegistrations) {
            if (r.base) {
                return r.handle;
            }
        }
        return 0;
    }();
    const int64_t extra = addPluginHook(f, inst.get(), 1, "test", "先登记一个扩展处理器");
    AgentxxPluginHookSpec baseSpec{};
    baseSpec.point       = AGENTXX_PLUGIN_HOOK_AGENT_START;
    baseSpec._reserved   = 0;
    baseSpec.user_data   = nullptr;
    baseSpec.hook_start  = [](void*, int32_t, const PluginxxStringView*, const PluginxxOperatorNotify* notify, PluginxxString*) -> void* {
        if (notify != nullptr && notify->done != nullptr) {
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        }
        return nullptr;
    };
    baseSpec.hook_cancel = nullptr;
    XX_TEST_EXPECT_EQ(f.mgr->registerHook(inst.get(), &baseSpec), 0);
    // 记录: 基础登记仍是 1 条 (覆盖), 扩展登记各自 1 条 (high / low / extra)
    size_t baseRecords = 0;
    size_t exRecords   = 0;
    for (const auto& r : inst->hookRegistrations) {
        if (r.base) {
            ++baseRecords;
        } else {
            ++exRecords;
        }
    }
    XX_TEST_EXPECT_EQ(baseRecords, size_t{1});
    XX_TEST_EXPECT_EQ(exRecords, size_t{3});
    const int64_t baseHandleAfter = [&]() -> int64_t {
        for (const auto& r : inst->hookRegistrations) {
            if (r.base) {
                return r.handle;
            }
        }
        return 0;
    }();
    XX_TEST_EXPECT_TRUE(baseHandleAfter != baseHandleBefore);
    XX_TEST_EXPECT_EQ(f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).size(), size_t{4});

    // 撤销基础登记: 只摘基础那条, 扩展登记的处理器还在
    XX_TEST_EXPECT_EQ(
        f.mgr->unregisterHook(inst.get(), AGENTXX_PLUGIN_HOOK_AGENT_START),
        0
    );
    handlers = f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START);
    XX_TEST_EXPECT_EQ(handlers.size(), size_t{3}); ///< high / low / extra 三个扩展处理器
    XX_TEST_EXPECT_TRUE(
        std::any_of(handlers.begin(), handlers.end(), [extra](const auto& h) {
            return h.handle == extra;
        })
    );
    XX_TEST_EXPECT_EQ(
        f.mgr->unregisterHook(inst.get(), AGENTXX_PLUGIN_HOOK_AGENT_START),
        -1
    ); ///< 基础登记已没有

    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_plugin", std::chrono::seconds{5}));
}

} // namespace

// ===========================================================================
// 4. 单派发器: 首次登记挂载、没有处理器时摘除 (轮次中先停用)
// ===========================================================================
namespace {

/// 中间件链上是否挂着派发器 (`found` 为是否存在, `disabled` 为是否被停用)
std::pair<bool, bool> dispatchHandleState(const Fixture& f) {
    const auto& handles = f.ctx->middlewareHandleContext->handles;
    for (const auto& h : handles) {
        if (h->name == "plugin_hooks") {
            return {true, h->disabled};
        }
    }
    return {false, false};
}

asio::awaitable<void> test_single_dispatcher_lifecycle() {
    auto f = co_await makeFixture();
    // 基线: 没有任何处理器时不该有派发器
    XX_TEST_EXPECT_FALSE(dispatchHandleState(f).first);

    CoreHookTrace trace;
    const int64_t h1 = addCoreHook(f, false, 0, "test", &trace);
    XX_TEST_EXPECT_GE(h1, int64_t{1});
    // 首次登记后挂上 (core 层处理器也算处理器)
    XX_TEST_EXPECT_TRUE(dispatchHandleState(f).first);
    XX_TEST_EXPECT_FALSE(dispatchHandleState(f).second);

    // 没有处理器时 (无轮次) 立即摘除
    XX_TEST_EXPECT_TRUE(f.mgr->removeCoreHookHandler(h1));
    XX_TEST_EXPECT_FALSE(dispatchHandleState(f).first);

    // 轮次执行中: 先停用 (链上还留着), 轮末 flush 摘除
    const int64_t h2 = addCoreHook(f, false, 0, "test", &trace);
    XX_TEST_EXPECT_TRUE(dispatchHandleState(f).first);
    f.mgr->onTurnBegin();
    XX_TEST_EXPECT_TRUE(f.mgr->removeCoreHookHandler(h2));
    {
        const auto state = dispatchHandleState(f);
        XX_TEST_EXPECT_TRUE(state.first);   ///< 链上还在
        XX_TEST_EXPECT_TRUE(state.second);  ///< 已停用 (链遍历跳过)
    }
    f.mgr->flushPendingCleanup();
    f.mgr->onTurnEnd();
    XX_TEST_EXPECT_FALSE(dispatchHandleState(f).first);

    // 停用后重新登记: 复用同一句柄并重新生效 (不再有排队摘除)
    const int64_t h3 = addCoreHook(f, false, 0, "test", &trace);
    XX_TEST_EXPECT_TRUE(dispatchHandleState(f).first);
    XX_TEST_EXPECT_FALSE(dispatchHandleState(f).second);
    XX_TEST_EXPECT_TRUE(f.mgr->removeCoreHookHandler(h3));
    XX_TEST_EXPECT_FALSE(dispatchHandleState(f).first);
}

} // namespace

// ===========================================================================
// 5. 实际派发: 顺序 + 单个处理器失败不影响后续
// ===========================================================================
namespace {

asio::awaitable<void> test_dispatch_order_and_failure_isolation() {
    auto      f    = co_await makeFixture();
    auto      inst = co_await f.mgr->loadPluginAsync(findPluginDir("example_plugin"));
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    CoreHookTrace trace;
    // 先登记一个会抛异常的 core 处理器, 再登记一个正常的 (两者都要跑)
    const int64_t throwing = addCoreHook(f, /*throwsInstead=*/true, -10, "test_throw", &trace);
    const int64_t ok       = addCoreHook(f, /*throwsInstead=*/false, 10, "test_ok", &trace);
    XX_TEST_EXPECT_GE(throwing, int64_t{1});
    XX_TEST_EXPECT_GE(ok, int64_t{1});

    neograph::graph::GraphState  state;
    neograph::graph::RunContext  runCtx;
    fillNodeInput(runCtx);
    neograph::graph::NodeInput in{state, runCtx, nullptr};

    co_await f.mgr->dispatchHook(AGENTXX_PLUGIN_HOOK_AGENT_START, in);
    // 插件层处理器先跑 (example_plugin 的基础登记), 之后是 core 层;
    // 抛异常的 core 处理器只记日志, 不影响后面的处理器
    XX_TEST_EXPECT_EQ(trace.calls.size(), size_t{1});
    if (!trace.calls.empty()) {
        XX_TEST_EXPECT_TRUE(trace.calls[0].find("point=0") != std::string::npos);
        XX_TEST_EXPECT_TRUE(trace.calls[0].find("hook_test_session") != std::string::npos);
    }

    // 空点派发: 没有处理器时什么都不做 (也不应崩)
    co_await f.mgr->dispatchHook(AGENTXX_PLUGIN_HOOK_TOOL_END, in);

    // core 层只支持同步处理器: 返回操作句柄的会被请求取消, 但派发要继续
    {
        AsyncCoreHookState asyncState;
        AgentxxPluginHookSpec spec{};
        spec.point       = AGENTXX_PLUGIN_HOOK_AGENT_START;
        spec._reserved   = 0;
        spec.hook_start  = &coreHookAsync;
        spec.hook_cancel = &coreHookCancelAsync;
        spec.user_data   = &asyncState;
        const int64_t handle = f.mgr->addCoreHookHandler(
            AGENTXX_PLUGIN_HOOK_AGENT_START,
            -5,
            "test_async",
            spec
        );
        XX_TEST_EXPECT_GE(handle, int64_t{1});
        co_await f.mgr->dispatchHook(AGENTXX_PLUGIN_HOOK_AGENT_START, in);
        XX_TEST_EXPECT_EQ(asyncState.cancels, 1); ///< 已请求取消 (core 层不驱动异步)
        XX_TEST_EXPECT_EQ(trace.calls.size(), size_t{2}); ///< 正常 core 处理器照常跑
        XX_TEST_EXPECT_TRUE(f.mgr->removeCoreHookHandler(handle));
    }

    // 禁用中的实例: 它的处理器不生效 (这里禁用后 core 处理器照常跑)
    f.mgr->disable("example_plugin");
    co_await f.mgr->dispatchHook(AGENTXX_PLUGIN_HOOK_AGENT_START, in);
    XX_TEST_EXPECT_EQ(trace.calls.size(), size_t{3});
    f.mgr->enable("example_plugin");
    XX_TEST_EXPECT_TRUE(co_await waitFor([&]() {
        return !f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).empty();
    }));

    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_plugin", std::chrono::seconds{5}));
}

} // namespace

// ===========================================================================
// 6. 清单字段 + 开发者模式对派发记录的开关
// ===========================================================================
namespace {

asio::awaitable<void> test_manifest_and_dev_mode() {
    const bool devModeBefore = agentxx::agent::AgentConfigStatic::devMode;
    auto       f             = co_await makeFixture();
    auto       inst          = co_await f.mgr->loadPluginAsync(findPluginDir("example_feature"));
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    const int64_t extra = addPluginHook(f, inst.get(), -20, "test", "清单字段检查");
    XX_TEST_EXPECT_GE(extra, int64_t{1});

    // 清单形状: 7 个点都在, 处理器带全部说明字段
    utilxx_base::Json hooksJson;
    try {
        hooksJson = utilxx_base::Json::parse(f.mgr->hooksJson());
    } catch (const std::exception& e) {
        TEST_FAIL << "[plugin_hooks] hooksJson parse failed: " << e.what() << std::endl;
    }
    XX_TEST_EXPECT_TRUE(hooksJson.is_object());
    XX_TEST_EXPECT_EQ(hooksJson.value("count", 0), 7);
    XX_TEST_EXPECT_GE(hooksJson.value("handlers", 0), 2);
    XX_TEST_EXPECT_FALSE(hooksJson.value("devMode", true));
    const auto order = jsonOrder(hooksJson, "AGENT_START");
    XX_TEST_EXPECT_EQ(order.size(), size_t{2});
    if (order.size() == 2) {
        XX_TEST_EXPECT_EQ(order[0], fmt::format("plugin:example_feature#{}", extra));
        XX_TEST_EXPECT_EQ(order[1], fmt::format("plugin:example_feature#{}", extra - 1));
    }
    if (hooksJson.contains("points") && hooksJson["points"].is_array()) {
        for (const auto& point : hooksJson["points"]) {
            if (point.value("name", std::string{}) != "AGENT_START") {
                continue;
            }
            XX_TEST_EXPECT_FALSE(point.contains("stat")); ///< 非开发者模式: 没有派发记录
            const auto& handlers = point["handlers"];
            XX_TEST_EXPECT_TRUE(handlers.is_array());
            XX_TEST_EXPECT_EQ(point.value("count", 0), 2);
            const auto& first = handlers[0];
            XX_TEST_EXPECT_EQ(first.value("layer", std::string{}), std::string{"plugin"});
            XX_TEST_EXPECT_EQ(first.value("ownerTag", std::string{}), std::string{"test"});
            XX_TEST_EXPECT_EQ(first.value("depict", std::string{}), std::string{"清单字段检查"});
            XX_TEST_EXPECT_EQ(first.value("priority", 0), -20);
            XX_TEST_EXPECT_EQ(first.value("load", std::string{}), std::string{"dynamic"});
            XX_TEST_EXPECT_TRUE(first.value("enabled", false));
            // 第二个是插件自己在 start 里经 kit 登记的 (归属标签与说明来自插件代码)
            const auto& second = handlers[1];
            XX_TEST_EXPECT_EQ(second.value("ownerTag", std::string{}), std::string{"example_feature"});
            XX_TEST_EXPECT_EQ(second.value("priority", 0), -10);
            XX_TEST_EXPECT_TRUE(
                second.value("depict", std::string{}).find("有序登记") != std::string::npos
            );
            break;
        }
    }

    // 开发者模式: 派发后出现 stat 段 (次数 / 上次顺序与耗时)
    agentxx::agent::AgentConfigStatic::devMode = true;
    neograph::graph::GraphState  state;
    neograph::graph::RunContext  runCtx;
    fillNodeInput(runCtx);
    neograph::graph::NodeInput in{state, runCtx, nullptr};
    co_await f.mgr->dispatchHook(AGENTXX_PLUGIN_HOOK_AGENT_START, in);

    hooksJson = utilxx_base::Json::parse(f.mgr->hooksJson());
    XX_TEST_EXPECT_TRUE(hooksJson.value("devMode", false));
    if (hooksJson.contains("points") && hooksJson["points"].is_array()) {
        for (const auto& point : hooksJson["points"]) {
            if (point.value("name", std::string{}) != "AGENT_START") {
                continue;
            }
            XX_TEST_EXPECT_TRUE(point.contains("stat"));
            const auto& stat = point["stat"];
            XX_TEST_EXPECT_EQ(stat.value("dispatches", 0), 1);
            XX_TEST_EXPECT_EQ(stat.value("lastHandlers", 0), 2);
            const auto lastOrder = stat.value("lastOrder", std::string{});
            XX_TEST_EXPECT_TRUE(lastOrder.find("plugin:example_feature") != std::string::npos);
            break;
        }
    }
    agentxx::agent::AgentConfigStatic::devMode = devModeBefore;

    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_feature", std::chrono::seconds{5}));
}

} // namespace

// ===========================================================================
// 7. 生命周期: 禁用摘除 / 启用重新登记 / 卸载回基线
// ===========================================================================
namespace {

asio::awaitable<void> test_lifecycle_disable_enable_unload() {
    auto f    = co_await makeFixture();
    auto inst = co_await f.mgr->loadPluginAsync(findPluginDir("example_plugin"));
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    const size_t loaded = f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).size();
    XX_TEST_EXPECT_EQ(loaded, size_t{1});
    XX_TEST_EXPECT_EQ(f.mgr->registrationInventory(*inst).hooks, size_t{1});

    // 禁用: 处理器摘除 (记录保留, 句柄置 0), 清单回到空
    f.mgr->disable("example_plugin");
    XX_TEST_EXPECT_TRUE(f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).empty());
    XX_TEST_EXPECT_EQ(f.mgr->registrationInventory(*inst).hooks, size_t{0});
    XX_TEST_EXPECT_EQ(inst->hookRegistrations.size(), size_t{1});
    XX_TEST_EXPECT_EQ(inst->hookRegistrations.front().handle, int64_t{0});
    XX_TEST_EXPECT_FALSE(dispatchHandleState(f).first);
    XX_TEST_EXPECT_TRUE(jsonOrder(
                            utilxx_base::Json::parse(f.mgr->hooksJson()),
                            "AGENT_START"
                        )
                            .empty());

    // 启用: start 事务重新登记
    f.mgr->enable("example_plugin");
    XX_TEST_EXPECT_TRUE(co_await waitFor([&]() {
        return !f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).empty();
    }));
    XX_TEST_EXPECT_EQ(f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).size(), size_t{1});
    XX_TEST_EXPECT_EQ(f.mgr->registrationInventory(*inst).hooks, size_t{1});
    XX_TEST_EXPECT_TRUE(dispatchHandleState(f).first);

    // 卸载: 实例记录与处理器全部消失, 派发器摘除
    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_plugin", std::chrono::seconds{5}));
    XX_TEST_EXPECT_TRUE(f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).empty());
    f.mgr->flushPendingCleanup();
    XX_TEST_EXPECT_FALSE(dispatchHandleState(f).first);
    auto views = f.mgr->list();
    XX_TEST_EXPECT_TRUE(views.empty());
}

} // namespace

// ===========================================================================
// 8. 插件面: kit 的 hookEx / listHooks / unhookEx
// ===========================================================================
namespace {

/// 插件侧上下文 (真实宿主视图; 只用来调 kit 的便捷函数)
struct HookSdkCtx : public agentxx::plugin::PluginBase {};

asio::awaitable<void> test_plugin_side_kit() {
    auto f    = co_await makeFixture();
    auto inst = co_await f.mgr->loadPluginAsync(findPluginDir("example_plugin"));
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    const auto* host = inst->hostView();
    XX_TEST_EXPECT_TRUE(host != nullptr);
    if (host == nullptr) {
        co_return;
    }

    HookSdkCtx sdk;
    sdk.init(host);
    XX_TEST_EXPECT_TRUE(sdk.iface.hooks != nullptr);
    XX_TEST_EXPECT_TRUE(sdk.iface.hooksEx != nullptr); ///< 宿主提供有序登记表

    // 经 kit 登记 (优先级 -5 -> 排在基础登记之前)
    agentxx::plugin::HookOptions opts;
    opts.priority = -5;
    opts.ownerTag = "kit";
    opts.depict   = "kit 登记的处理器";
    const int64_t handle = agentxx::plugin::hookEx(
        sdk,
        AGENTXX_PLUGIN_HOOK_AGENT_START,
        [](HookSdkCtx&, AgentxxPluginHookPoint, std::string_view) {},
        opts
    );
    XX_TEST_EXPECT_GE(handle, int64_t{1});

    {
        const auto handlers = f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START);
        XX_TEST_EXPECT_EQ(handlers.size(), size_t{2});
        if (handlers.size() == 2) {
            XX_TEST_EXPECT_EQ(handlers[0].handle, handle);
            XX_TEST_EXPECT_EQ(handlers[0].ownerTag, std::string{"kit"});
        }
    }

    // 清单 (kit 读宿主 list_hooks)
    const auto text = agentxx::plugin::listHooks(sdk);
    XX_TEST_EXPECT_TRUE(text.find("\"count\":7") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("\"ownerTag\":\"kit\"") != std::string::npos);

    // 按句柄撤销
    XX_TEST_EXPECT_EQ(agentxx::plugin::unhookEx(sdk, handle), 0);
    XX_TEST_EXPECT_EQ(f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).size(), size_t{1});
    XX_TEST_EXPECT_EQ(agentxx::plugin::unhookEx(sdk, handle), -1);

    // 卸载: 无残留
    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_plugin", std::chrono::seconds{5}));
    XX_TEST_EXPECT_TRUE(f.mgr->handlersOf(AGENTXX_PLUGIN_HOOK_AGENT_START).empty());
}

} // namespace

// ===========================================================================
// 模块入口
// ===========================================================================

asio::awaitable<TestResult> run_plugin_hooks_tests() {
    TEST_INFO << "[plugin_hooks] 钩子处理器清单与优先级 (agentxx.agent.hooks / hooks_ex)"
              << std::endl;
    try {
        co_await test_order_by_layer_priority_seq();
        co_await test_multiple_handlers_and_owner_check();
        co_await test_base_and_ex_independent_and_clamp();
        co_await test_single_dispatcher_lifecycle();
        co_await test_dispatch_order_and_failure_isolation();
        co_await test_manifest_and_dev_mode();
        co_await test_lifecycle_disable_enable_unload();
        co_await test_plugin_side_kit();
    } catch (const std::exception& e) {
        TEST_FAIL << "[plugin_hooks] 用例异常: " << e.what() << std::endl;
        ++g_ph_failed;
    } catch (...) {
        TEST_FAIL << "[plugin_hooks] 用例未知异常" << std::endl;
        ++g_ph_failed;
    }
    co_return TestResult{g_ph_passed, g_ph_failed};
}

} // namespace test
} // namespace agentxx
