/// test_plugin_agent_runtime —— 会话运行时接口表 (`agentxx.agent.runtime`) 全流程
///
/// 这一层验证的是"插件能挂自己的根, 在根上开关会话通道"这件事本身:
/// - **根**: 一份独立 `AgentConfig` (自定义提示词 / 工具白名单 / 插件挂载策略),
///   与内置会话互不影响;
/// - **通道**: 一个 `sessionId` 的驱动壳 (输入 / 事件 / 查询);
/// - **触发器 / 定时器**: 把事件或时间转成通道输入;
/// - **归属**: 插件禁用后根与通道整批回收。
///
/// 装置: 真实容器 (`AgentHost`) + 真实 IO 线程 + 真实 `AgentSessionRuntime`。
/// 全部运行时动作都跑在**容器那条 io 线程**上 (与生产一致: 表入口经
/// `ioCallSyncKeep` 落到管理器绑定的 io 线程, 插件在自己的 `start` 里就在那条线程)。
/// "插件实例"是手工构造的伪实例 (与 `plugin_runtime` 模块同一手法): 运行时表只
/// 要求交给它一个有效的主程序视图, 不需要动态库。
///
/// 覆盖 (编号对应 docs/zh-cn/design/plugins.md §19.8):
/// 1 表头与版本门槛 / 2 无容器时不可用 / 3 根配置非法 / 4 挂根 → 开通道 → READY /
/// 5 自定义提示词与工具白名单生效 / 6 同 sessionId 冲突 / 7 输入与轮次事件 /
/// 8 清单与状态查询 / 9 两根两通道互不串话 / 10 根数预算 / 11 触发器三策略 /
/// 12 定时器 / 13 关通道与连带注销 / 14 禁用回收 / 15 卸载根
#include "agentxx-test/plugin/test_plugin_agent_runtime.h"

#include "agentxx-test/core/test_agent.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/plugin/api/plugin_iface_meta.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/api/plugin_spec.h"
#include "agentxx/plugin/plugin_manager.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/use_future.hpp"
#include "fmt/format.h"
#include "pluginxx/host/abi_util.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
int g_rt_passed = 0;
int g_rt_failed = 0;
} // namespace

#define XX_TEST_PASSED g_rt_passed
#define XX_TEST_FAILED g_rt_failed

namespace agentxx::plugin {
/// 主程序 vtable 加载入口 (定义在 plugin_manager_vtable.cpp; `__vtable` 是它认识的
/// 特殊 IID)。伪实例据此拿到真实主程序视图, 从而直接驱动 C ABI 入口。
const void* PLUGINXX_CALL xx_query_interface(const PluginxxHost*, const PluginxxStringView* iid);
} // namespace agentxx::plugin

namespace agentxx {
namespace test {

namespace {

using namespace std::chrono_literals;
using agentxx::plugin::PluginString;
using agentxx::plugin::PluginStringView;

/// 取"临时字符串视图"的地址
///
/// 表入口的字符串参数是 `const PluginxxStringView*`; 直接写 `&PluginStringView::from(x)`
/// 是取临时量的地址 (不合法)。本包装把视图存进一个临时对象, `&arg` 给出它的地址,
/// 对象活到所在语句结束 —— 与入口"视图只在调用期间有效"的约定一致。
struct SvArg {
    PluginxxStringView view;

    SvArg(const char* s) noexcept : view(PluginStringView::fromCstr(s)) {}
    SvArg(std::string_view s) noexcept : view(PluginStringView::from(s)) {}

    const PluginxxStringView* operator&() const noexcept {
        return &view;
    }
};

// ===========================================================================
// 事件收集
// ===========================================================================

/// 一条收集到的通道事件
struct CollectedEvent {
    int32_t     kind        = 0;
    int64_t     interruptId = 0;
    std::string text;
    std::string payload;
};

/// 通道事件收集器 (回调在容器 IO 线程; 用例协程读时加锁)
struct EventSink {
    mutable std::mutex          mutex;
    std::vector<CollectedEvent> events;

    void push(int32_t kind, int64_t interruptId, std::string_view text, std::string_view payload) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(CollectedEvent{
            kind,
            interruptId,
            std::string{text},
            std::string{payload},
        });
    }

    size_t count(int32_t kind) const {
        std::lock_guard<std::mutex> lock(mutex);
        size_t                      hits = 0;
        for (const auto& item : events) {
            if (item.kind == kind) {
                ++hits;
            }
        }
        return hits;
    }

    /// 某一类事件的正文拼接 (增量类事件用)
    std::string textOf(int32_t kind) const {
        std::lock_guard<std::mutex> lock(mutex);
        std::string                 out;
        for (const auto& item : events) {
            if (item.kind == kind) {
                out += item.text;
            }
        }
        return out;
    }

    /// 某一类事件最后一条的载荷
    std::string payloadOf(int32_t kind) const {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->kind == kind) {
                return it->payload;
            }
        }
        return {};
    }

    /// 某一类事件的载荷 JSON (缺失或解析失败返回空对象)
    utilxx_base::Json jsonOf(int32_t kind) const {
        const auto text = payloadOf(kind);
        if (text.empty()) {
            return utilxx_base::Json::object();
        }
        try {
            return utilxx_base::Json::parse(text);
        } catch (const std::exception&) {
            return utilxx_base::Json::object();
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        events.clear();
    }
};

/// 通道事件回调 (C ABI): 转发到 EventSink
void PLUGINXX_CALL onChannelEvent(void* user_data, const AgentxxPluginChannelEvent* event) {
    auto* sink = static_cast<EventSink*>(user_data);
    if (!sink || !event) {
        return;
    }
    sink->push(
        event->kind,
        event->interrupt_id,
        pluginxx::svToSv(event->text),
        pluginxx::svToSv(event->payload_json)
    );
}

// ===========================================================================
// 装置
// ===========================================================================

/// 测试用管理器: 把受保护的 `hostVtable` 暴露出来 (与 plugin_host_mount 同一手法)
struct TestHost : public agentxx::plugin::PluginManager {
    using agentxx::plugin::PluginManager::PluginManager;
    using agentxx::plugin::PluginManager::hostVtable;
};

/// 取插件侧的一张接口表
const void* queryTable(const PluginxxHost* host, const char* iid) {
    if (!host || !host->vtable || !host->vtable->query_interface) {
        return nullptr;
    }
    auto view = PluginStringView::fromCstr(iid);
    return host->vtable->query_interface(host, &view);
}

/// 伪插件实例: 只是为主程序视图提供一个实例对象 (与 `plugin_runtime` 的夹具同一做法)
std::shared_ptr<agentxx::plugin::PluginInstance> makeInstance(
    const std::shared_ptr<TestHost>& mgr,
    asio::any_io_executor            ex,
    std::string                      name
) {
    auto inst       = std::make_shared<agentxx::plugin::PluginInstance>(std::move(name));
    inst->self      = inst;
    inst->ownerSelf = inst;
    inst->manager   = mgr;

    // 主程序控制块: 交给插件的 host 视图必须有进程级稳定地址
    auto vtableIid    = PluginStringView::fromCstr("__vtable");
    inst->hostControl = pluginxx::PluginHostControl::create(
        inst,
        static_cast<const PluginxxHostVtable*>(
            agentxx::plugin::xx_query_interface(nullptr, &vtableIid)
        )
    );
    // 与 PluginManagerBase::makeLifetime 的加载语义一致: 实例登记所属运行时,
    // Operation 驱动器据此取 io executor 与线程标识
    const std::weak_ptr<pluginxx::PluginRuntime> runtime = mgr->runtime();
    inst->runtime                                       = mgr->runtime();
    inst->lifetime                                      = std::make_shared<pluginxx::InstanceLifetime>(
        ex,
        inst->name,
        1,
        [runtime](std::function<void()> fn) {
            if (auto state = runtime.lock()) {
                return pluginxx::enqueueRuntimeAction(state, std::move(fn), true);
            }
            return false;
        }
    );
    inst->lifetime->setState(pluginxx::PluginInstanceState::Ready);
    inst->enabled = true;
    // 登记进实例表: 禁用/卸载路径按名字找它
    mgr->plugins_[inst->name] = inst;
    return inst;
}

/// 造一份会话配置: 模型走本进程的模拟服务, 不落盘
std::shared_ptr<agentxx::agent::AgentConfig>
    makeConfig(std::string_view baseUrl, std::string_view prompt) {
    auto cfg = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = std::string{baseUrl};
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "sim-model";
    cfg->availableModels["test-sim"] = cfg->model;
    cfg->currentModelName            = "test-sim";
    cfg->prompt.systemPrompt         = std::string{prompt};
    cfg->permissionMode              = agentxx::agent::PermissionMode::Pass;
    cfg->pluginMountPolicy           = agentxx::agent::PluginMountPolicy::None;
    cfg->enableSessionStore          = false;
    cfg->enableSubagent              = false;
    return cfg;
}

/// 根配置 JSON (`config_json` 的常见写法: 提示词 + 模型 + 工具白名单)
std::string rootConfigJson(std::string_view systemPrompt) {
    utilxx_base::Json j      = utilxx_base::Json::object();
    j["prompt"]              = utilxx_base::Json{{"systemPrompt", std::string{systemPrompt}}};
    j["model"]               = "test-sim";
    j["toolWhitelist"]       = std::vector<std::string>{"agentxx_share_store"};
    j["enableToolFiltering"] = true;
    j["enableSessionStore"]  = false;
    return j.dump();
}

/// 等待条件成立 (容器 io 线程上轮转等待; 不用阻塞 sleep)
asio::awaitable<bool> waitUntil(
    asio::any_io_executor     ex,
    std::chrono::milliseconds timeout,
    std::function<bool()>     cond
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
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

/// 取一个字符串结果 (失败返回空串)
std::string takeString(const PluginxxHost* host, PluginxxString& out) {
    if (out.data == nullptr) {
        return {};
    }
    std::string text{out.data, static_cast<size_t>(out.size)};
    PluginString::free(host, &out);
    return text;
}

/// 从运行时清单里取某个根
utilxx_base::Json findRootJson(const utilxx_base::Json& runtime, std::string_view rootId) {
    if (!runtime.contains("roots") || !runtime["roots"].is_array()) {
        return utilxx_base::Json::object();
    }
    for (const auto& item : runtime["roots"]) {
        if (item.value("rootId", std::string{}) == rootId) {
            return item;
        }
    }
    return utilxx_base::Json::object();
}

// ===========================================================================
// 用例主体 (全部在容器 io 线程上跑)
// ===========================================================================

asio::awaitable<void> runBody(
    const std::shared_ptr<asio::io_context>& io,
    std::string_view                         baseUrl
) {
    const auto ex = io->get_executor();

    // ---- 主程序级上下文: 会话配置的来源 (语言/模型/工作目录) ----
    // 只作为"配置来源"使用, 不需要真起一个 agent: 管理器与容器读它的配置与总线
    auto primaryCtx             = std::make_shared<agentxx::agent::AgentContext>();
    primaryCtx->agentConfig     = makeConfig(baseUrl, "You are a helpful assistant.");
    primaryCtx->bus             = std::make_shared<agentxx::events::EventBus>(ex);

    // ---- 管理器 (在容器 io 线程上创建: setIoExecutor 记录的线程即这一条) ----
    auto mgr = std::make_shared<TestHost>(primaryCtx);
    mgr->setIoExecutor(ex);
    primaryCtx->pluginManager      = mgr;
    primaryCtx->pluginManagerOwned = false;

    // ---- 容器: 每根一个 agent 实例, 共用这条 io 线程 ----
    // 根的插件管理器由工厂注入容器这一份 (否则 BaseAgent::init 会自建一份)
    agentxx::agent::AgentHost::Config hostCfg;
    hostCfg.ioCtx    = io;
    hostCfg.maxRoots = 2; ///< 本模块只给插件根留两个位置 (预算用例要够用)
    hostCfg.agentFactory = [mgr](std::shared_ptr<agentxx::agent::AgentConfig> cfg)
        -> std::shared_ptr<agentxx::agent::BaseAgent> {
        auto agent = std::make_shared<agentxx::agent::CodeAgent>(std::move(cfg));
        auto ctx   = agent->getContext();
        if (ctx) {
            ctx->pluginManager      = mgr;
            ctx->pluginManagerOwned = false;
        }
        return agent;
    };
    auto host        = agentxx::agent::AgentHost::create(hostCfg);
    primaryCtx->host = host;

    auto                       inst = makeInstance(mgr, ex, "runtime_probe");
    const PluginxxHost*        hostView = inst->hostView();
    const AgentxxPluginRuntimeIface* runtime
        = static_cast<const AgentxxPluginRuntimeIface*>(
            queryTable(hostView, AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME)
        );

    // ===================== 1. 表头与版本门槛 =====================
    {
        XX_TEST_EXPECT_TRUE(hostView != nullptr);
        XX_TEST_EXPECT_TRUE(runtime != nullptr);
        if (runtime == nullptr) {
            co_return;
        }
        XX_TEST_EXPECT_EQ(runtime->version, AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME_VERSION);
        XX_TEST_EXPECT_EQ(runtime->struct_size, uint32_t{sizeof(AgentxxPluginRuntimeIface)});
        XX_TEST_EXPECT_TRUE(pluginxx::validateInterface<AgentxxPluginRuntimeIface>(runtime) != nullptr);
        // 门槛: 版本不足或基线字节数不足都判空
        AgentxxPluginRuntimeIface probe = *runtime;
        probe.version                   = AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME_MIN_VERSION - 1;
        XX_TEST_EXPECT_TRUE(pluginxx::validateInterface<AgentxxPluginRuntimeIface>(&probe) == nullptr);
        probe            = *runtime;
        probe.struct_size = AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME_BASELINE_SIZE - 1;
        XX_TEST_EXPECT_TRUE(pluginxx::validateInterface<AgentxxPluginRuntimeIface>(&probe) == nullptr);
        // 全部入口都在
        XX_TEST_EXPECT_TRUE(runtime->mount_root != nullptr);
        XX_TEST_EXPECT_TRUE(runtime->open_channel != nullptr);
        XX_TEST_EXPECT_TRUE(runtime->send_input != nullptr);
        XX_TEST_EXPECT_TRUE(runtime->register_trigger != nullptr);
        XX_TEST_EXPECT_TRUE(runtime->register_timer != nullptr);
    }

    // ===================== 2. 无容器时不可用 =====================
    {
        // 另一个管理器: 主程序上下文没有 host 引用 → 拿不到容器
        auto lonelyCtx         = std::make_shared<agentxx::agent::AgentContext>();
        lonelyCtx->agentConfig = makeConfig(baseUrl, "lonely");
        lonelyCtx->bus         = std::make_shared<agentxx::events::EventBus>(ex);
        auto lonelyMgr         = std::make_shared<TestHost>(lonelyCtx);
        lonelyMgr->setIoExecutor(ex);
        auto lonelyInst = makeInstance(lonelyMgr, ex, "lonely_probe");
        auto lonelyView = lonelyInst->hostView();
        const auto* lonelyRuntime = static_cast<const AgentxxPluginRuntimeIface*>(
            queryTable(lonelyView, AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME)
        );
        XX_TEST_EXPECT_TRUE(lonelyRuntime != nullptr);
        if (lonelyRuntime != nullptr) {
            AgentxxPluginMountRootSpec spec{};
            spec.struct_size = sizeof(AgentxxPluginMountRootSpec);
            spec.name        = PluginStringView::fromCstr("lonely");
            PluginxxString out{nullptr, 0};
            XX_TEST_EXPECT_EQ(
                lonelyRuntime->mount_root(lonelyView, &spec, &out),
                int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_UNSUPPORTED}
            );
        }
        lonelyMgr->plugins_.erase(lonelyInst->name);
    }

    // ===================== 3. 根配置非法 =====================
    std::string rootA;
    {
        AgentxxPluginMountRootSpec spec{};
        spec.struct_size = sizeof(AgentxxPluginMountRootSpec);
        spec.name        = PluginStringView::fromCstr("probe");

        // (a) config_json 不是合法 JSON
        spec.config_json = PluginStringView::fromCstr("{ not json");
        PluginxxString out{nullptr, 0};
        XX_TEST_EXPECT_EQ(
            runtime->mount_root(hostView, &spec, &out),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );

        // (b) 合法 JSON 但模型不在可用列表里
        spec.config_json = PluginStringView::fromCstr(R"({"model":"no-such-model"})");
        XX_TEST_EXPECT_EQ(
            runtime->mount_root(hostView, &spec, &out),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );

        // (c) 挂载策略混进 config_json: 只有一个来源, 两处都给按参数错误处理
        spec.config_json = PluginStringView::fromCstr(R"({"pluginMount":"all"})");
        XX_TEST_EXPECT_EQ(
            runtime->mount_root(hostView, &spec, &out),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );

        // (d) mount_plugins 的 mode 非法
        spec.config_json  = PluginStringView::fromCstr("{}");
        spec.mount_plugins = PluginStringView::fromCstr(R"({"mode":"every"})");
        XX_TEST_EXPECT_EQ(
            runtime->mount_root(hostView, &spec, &out),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );

        // 全部被拒: 容器里没有留下任何根
        XX_TEST_EXPECT_EQ(host->rootCount(), size_t{0});
    }

    // ===================== 4. 挂根 → 开通道 → READY =====================
    const std::string promptA = "你是运行时测试助手, 只回答 OK。";
    const std::string cfgA    = rootConfigJson(promptA);
    {
        AgentxxPluginMountRootSpec spec{};
        spec.struct_size = sizeof(AgentxxPluginMountRootSpec);
        spec.name        = PluginStringView::fromCstr("probe");
        spec.config_json = PluginStringView::from(std::string_view{cfgA});
        // 根不挂任何插件 (只有内置工具)
        spec.mount_plugins = PluginStringView::fromCstr(R"({"mode":"none"})");
        PluginxxString out{nullptr, 0};
        XX_TEST_EXPECT_EQ(
            runtime->mount_root(hostView, &spec, &out),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        rootA = takeString(hostView, out);
        XX_TEST_EXPECT_TRUE(!rootA.empty());
        // 根 id 由运行时自己算: `ext:<插件名>:<n>` (容器登记用同一个值)
        XX_TEST_EXPECT_TRUE(rootA.starts_with("ext:runtime_probe:"));
    }
    if (rootA.empty()) {
        co_return;
    }

    EventSink sinkA;
    int64_t   channelA = 0;
    {
        AgentxxPluginChannelSpec spec{};
        spec.struct_size = sizeof(AgentxxPluginChannelSpec);
        spec.root_id     = PluginStringView::from(std::string_view{rootA});
        spec.session_id  = PluginStringView::fromCstr("rt_sess_a");
        spec.title       = PluginStringView::fromCstr("测试通道 A");
        spec.on_event    = &onChannelEvent;
        spec.user_data   = &sinkA;
        XX_TEST_EXPECT_EQ(
            runtime->open_channel(hostView, &spec, &channelA),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        XX_TEST_EXPECT_TRUE(channelA > 0);
    }
    XX_TEST_EXPECT_TRUE(co_await waitUntil(
        ex,
        30s,
        [&]() {
            return host->findRoot(rootA) != nullptr
                   && sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_READY) > 0;
        }
    ));
    XX_TEST_EXPECT_EQ(host->rootCount(), size_t{1});

    // ===================== 5. 根的配置生效, 内置会话不受影响 =====================
    {
        auto root = host->findRoot(rootA);
        XX_TEST_EXPECT_TRUE(root != nullptr);
        if (root) {
            auto ctx = root->getContext();
            XX_TEST_EXPECT_TRUE(ctx != nullptr);
            if (ctx && ctx->agentConfig) {
                XX_TEST_EXPECT_EQ(ctx->agentConfig->prompt.systemPrompt, promptA);
                XX_TEST_EXPECT_TRUE(ctx->agentConfig->enableToolFiltering);
                XX_TEST_EXPECT_EQ(ctx->agentConfig->toolWhitelist.size(), size_t{1});
                // 内置会话的配置一个字都没动
                XX_TEST_EXPECT_TRUE(primaryCtx->agentConfig->prompt.systemPrompt != promptA);
            }
            // 工具白名单真的生效: 只有 share_store, 没有文件工具
            if (ctx) {
                const auto hasTool = [&ctx](std::string_view name) {
                    return std::find(ctx->toolNames.begin(), ctx->toolNames.end(), name)
                           != ctx->toolNames.end();
                };
                XX_TEST_EXPECT_TRUE(hasTool("agentxx_share_store"));
                XX_TEST_EXPECT_FALSE(hasTool("agentxx_filesystem_read"));
            }
        }
        const auto ready = sinkA.jsonOf(AGENTXX_PLUGIN_CHANNEL_EVT_READY);
        XX_TEST_EXPECT_EQ(ready.value("rootId", std::string{}), rootA);
        XX_TEST_EXPECT_EQ(ready.value("sessionId", std::string{}), std::string{"rt_sess_a"});
    }

    // ===================== 6. 同 sessionId 两个通道 → 冲突 =====================
    {
        AgentxxPluginChannelSpec spec{};
        spec.struct_size = sizeof(AgentxxPluginChannelSpec);
        spec.root_id     = PluginStringView::from(std::string_view{rootA});
        spec.session_id  = PluginStringView::fromCstr("rt_sess_a");
        spec.on_event    = &onChannelEvent;
        spec.user_data   = &sinkA;
        int64_t dupId    = 0;
        XX_TEST_EXPECT_EQ(
            runtime->open_channel(hostView, &spec, &dupId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_CONFLICT}
        );
        // 换个 sessionId 就能开; 用完就关 (不占会话占用)
        spec.session_id = PluginStringView::fromCstr("rt_sess_dup_free");
        XX_TEST_EXPECT_EQ(
            runtime->open_channel(hostView, &spec, &dupId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        if (dupId > 0) {
            XX_TEST_EXPECT_EQ(
                runtime->close_channel(hostView, dupId),
                int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
            );
        }
        // 缺 on_event 回调: 参数错误
        AgentxxPluginChannelSpec bad{};
        bad.struct_size = sizeof(AgentxxPluginChannelSpec);
        bad.root_id     = PluginStringView::from(std::string_view{rootA});
        bad.session_id  = PluginStringView::fromCstr("rt_sess_no_cb");
        int64_t badId   = 0;
        XX_TEST_EXPECT_EQ(
            runtime->open_channel(hostView, &bad, &badId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );
        // 未知根: 不存在
        bad.on_event = &onChannelEvent;
        bad.user_data = &sinkA;
        bad.root_id   = PluginStringView::fromCstr("ext:nobody:1");
        XX_TEST_EXPECT_EQ(
            runtime->open_channel(hostView, &bad, &badId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND}
        );
    }

    // ===================== 7. 输入 → 轮次事件 =====================
    {
        sinkA.clear();
        g_da_sim_response_content     = "runtime answer";
        g_da_sim_tool_calls           = utilxx_base::Json::array();
        g_da_sim_tool_calls_remaining = 0;
        g_da_sim_delay_ms             = 0;

        const std::string input = R"({"text":"你好"})";
        XX_TEST_EXPECT_EQ(
            runtime->send_input(hostView, channelA, &SvArg(std::string_view{input})),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        XX_TEST_EXPECT_TRUE(co_await waitUntil(
            ex,
            30s,
            [&]() { return sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END) > 0; }
        ));
        XX_TEST_EXPECT_GE(sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START), size_t{1});
        XX_TEST_EXPECT_GE(sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_TEXT), size_t{1});
        XX_TEST_EXPECT_TRUE(
            sinkA.textOf(AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_TEXT).find("runtime answer")
            != std::string::npos
        );
        // 本轮结果: 无错误 + 整轮正文
        const auto turnEnd = sinkA.jsonOf(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END);
        XX_TEST_EXPECT_FALSE(turnEnd.value("hasError", true));
        XX_TEST_EXPECT_TRUE(
            turnEnd.value("fullText", std::string{}).find("runtime answer") != std::string::npos
        );
        // 输入受理回执
        XX_TEST_EXPECT_GE(sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_INPUT_ACK), size_t{1});

        // 参数校验: 空文本 / 非法 JSON / 非法 delivery / 不存在的通道
        XX_TEST_EXPECT_EQ(
            runtime->send_input(
                hostView,
                channelA,
                &SvArg(R"({"text":""})")
            ),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );
        XX_TEST_EXPECT_EQ(
            runtime->send_input(hostView, channelA, &SvArg("{")),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );
        XX_TEST_EXPECT_EQ(
            runtime->send_input(
                hostView,
                channelA,
                &SvArg(R"({"text":"x","delivery":"no-such"})")
            ),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );
        XX_TEST_EXPECT_EQ(
            runtime->send_input(
                hostView,
                999999,
                &SvArg(std::string_view{input})
            ),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND}
        );
    }

    // ===================== 8. 清单 / 通道状态 / 上下文查询 =====================
    {
        PluginxxString out{nullptr, 0};
        XX_TEST_EXPECT_EQ(runtime->list_runtime(hostView, &out), int32_t{0});
        const auto text = takeString(hostView, out);
        XX_TEST_EXPECT_FALSE(text.empty());
        const auto state = utilxx_base::Json::parse(text);
        XX_TEST_EXPECT_TRUE(state.contains("roots"));
        XX_TEST_EXPECT_EQ(state["roots"].size(), size_t{1});
        const auto rootJson = findRootJson(state, rootA);
        XX_TEST_EXPECT_FALSE(rootJson.empty());
        XX_TEST_EXPECT_TRUE(rootJson.value("ready", false));
        XX_TEST_EXPECT_EQ(
            rootJson["configSummary"].value("pluginMount", std::string{}),
            std::string{"none"}
        );
        XX_TEST_EXPECT_EQ(rootJson["configSummary"].value("model", std::string{}), std::string{"test-sim"});
        XX_TEST_EXPECT_EQ(rootJson["channels"].size(), size_t{1});
        XX_TEST_EXPECT_TRUE(state["triggers"].is_array());
        XX_TEST_EXPECT_TRUE(state["timers"].is_array());

        // 通道状态
        PluginxxString chOut{nullptr, 0};
        XX_TEST_EXPECT_EQ(runtime->get_channel_state(hostView, channelA, &chOut), int32_t{0});
        const auto chText = takeString(hostView, chOut);
        XX_TEST_EXPECT_FALSE(chText.empty());
        const auto chState = utilxx_base::Json::parse(chText);
        XX_TEST_EXPECT_EQ(chState.value("channelId", int64_t{0}), channelA);
        XX_TEST_EXPECT_EQ(chState.value("rootId", std::string{}), rootA);
        XX_TEST_EXPECT_EQ(chState.value("sessionId", std::string{}), std::string{"rt_sess_a"});
        XX_TEST_EXPECT_TRUE(chState.value("ready", false));
        XX_TEST_EXPECT_FALSE(chState.value("busy", true));
        XX_TEST_EXPECT_TRUE(chState.contains("workDir"));
        // 不存在的通道: 查询失败
        PluginxxString missing{nullptr, 0};
        XX_TEST_EXPECT_TRUE(runtime->get_channel_state(hostView, 999999, &missing) != 0);

        // 上下文: 刚跑过一轮, 至少 system + user + assistant
        PluginxxString ctxOut{nullptr, 0};
        XX_TEST_EXPECT_EQ(runtime->get_context_messages(hostView, channelA, &ctxOut), int32_t{0});
        const auto ctxText = takeString(hostView, ctxOut);
        XX_TEST_EXPECT_FALSE(ctxText.empty());
        const auto msgs = utilxx_base::Json::parse(ctxText);
        XX_TEST_EXPECT_TRUE(msgs.is_array());
        XX_TEST_EXPECT_GE(msgs.size(), size_t{3});
        if (msgs.is_array() && !msgs.empty()) {
            // 系统提示词就是我们给的那一份
            XX_TEST_EXPECT_TRUE(
                msgs[0].value("content", std::string{}).find(promptA) != std::string::npos
            );
        }
    }

    // ===================== 9. 第二个根 + 第二个通道: 互不串话 =====================
    const std::string promptB = "你是第二个根, 只回答 SECOND。";
    std::string       rootB;
    EventSink         sinkB;
    int64_t           channelB = 0;
    {
        const std::string cfgB = rootConfigJson(promptB);
        AgentxxPluginMountRootSpec spec{};
        spec.struct_size = sizeof(AgentxxPluginMountRootSpec);
        spec.name        = PluginStringView::fromCstr("probe2");
        spec.config_json = PluginStringView::from(std::string_view{cfgB});
        PluginxxString out{nullptr, 0};
        XX_TEST_EXPECT_EQ(
            runtime->mount_root(hostView, &spec, &out),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        rootB = takeString(hostView, out);
        XX_TEST_EXPECT_TRUE(!rootB.empty());
        XX_TEST_EXPECT_TRUE(rootB != rootA);

        if (!rootB.empty()) {
            AgentxxPluginChannelSpec chSpec{};
            chSpec.struct_size = sizeof(AgentxxPluginChannelSpec);
            // 与 A 相同的 sessionId, 但不同根: 会话按 (存储根, sessionId) 定位,
            // 两个根的会话目录不同, 因此不冲突
            chSpec.root_id    = PluginStringView::from(std::string_view{rootB});
            chSpec.session_id = PluginStringView::fromCstr("rt_sess_a");
            chSpec.on_event   = &onChannelEvent;
            chSpec.user_data  = &sinkB;
            XX_TEST_EXPECT_EQ(
                runtime->open_channel(hostView, &chSpec, &channelB),
                int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
            );
        }
        XX_TEST_EXPECT_TRUE(co_await waitUntil(
            ex,
            30s,
            [&]() {
                return sinkB.count(AGENTXX_PLUGIN_CHANNEL_EVT_READY) > 0
                       && host->findRoot(rootB) != nullptr;
            }
        ));
        XX_TEST_EXPECT_EQ(host->rootCount(), size_t{2});

        // 各发一条: 事件各进各的收集器
        sinkA.clear();
        sinkB.clear();
        if (channelB > 0) {
            XX_TEST_EXPECT_EQ(
                runtime->send_input(
                    hostView,
                    channelA,
                    &SvArg(R"({"text":"first"})")
                ),
                int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
            );
            XX_TEST_EXPECT_EQ(
                runtime->send_input(
                    hostView,
                    channelB,
                    &SvArg(R"({"text":"second"})")
                ),
                int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
            );
            XX_TEST_EXPECT_TRUE(co_await waitUntil(
                ex,
                30s,
                [&]() {
                    return sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END) > 0
                           && sinkB.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END) > 0;
                }
            ));
            XX_TEST_EXPECT_EQ(sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END), size_t{1});
            XX_TEST_EXPECT_EQ(sinkB.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END), size_t{1});
            // 两个根的提示词各自独立
            auto agentA = host->findRoot(rootA);
            auto agentB = host->findRoot(rootB);
            if (agentA && agentB) {
                XX_TEST_EXPECT_EQ(agentA->getContext()->agentConfig->prompt.systemPrompt, promptA);
                XX_TEST_EXPECT_EQ(agentB->getContext()->agentConfig->prompt.systemPrompt, promptB);
            }
        }
    }

    // ===================== 10. 根数预算 =====================
    {
        // 容器上限 2, A 与 B 已占满
        AgentxxPluginMountRootSpec spec{};
        spec.struct_size = sizeof(AgentxxPluginMountRootSpec);
        spec.name        = PluginStringView::fromCstr("third");
        spec.config_json = PluginStringView::from(std::string_view{cfgA});
        PluginxxString out{nullptr, 0};
        XX_TEST_EXPECT_EQ(
            runtime->mount_root(hostView, &spec, &out),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_BUDGET}
        );
        XX_TEST_EXPECT_EQ(host->rootCount(), size_t{2});

        // 另一个实例的根与本实例无关: 关掉通道 B 后把 B 卸掉, 再用同一个 id 装回来
        if (channelB > 0) {
            XX_TEST_EXPECT_EQ(
                runtime->close_channel(hostView, channelB),
                int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
            );
            channelB = 0;
        }
    }

    // ===================== 11. 触发器: 三种策略 =====================
    {
        XX_TEST_EXPECT_EQ(runtime->list_runtime(hostView, nullptr), int32_t{-1});

        // (a) QUEUE: 每条事件立即投递
        const std::string topic = "runtime.tick";
        AgentxxPluginTriggerSpec spec{};
        spec.struct_size = sizeof(AgentxxPluginTriggerSpec);
        spec.policy      = AGENTXX_PLUGIN_TRIGGER_POLICY_QUEUE;
        spec.topic       = PluginStringView::from(std::string_view{topic});
        spec.channel_id  = channelA;
        int64_t queueTrigger = 0;
        XX_TEST_EXPECT_EQ(
            runtime->register_trigger(hostView, &spec, &queueTrigger),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        XX_TEST_EXPECT_TRUE(queueTrigger > 0);

        // 参数校验: topic 必填 / policy 越界 / 通道不存在 / delivery 非法
        AgentxxPluginTriggerSpec bad = spec;
        bad.topic                    = PluginStringView::fromCstr("");
        int64_t tmpId                = 0;
        XX_TEST_EXPECT_EQ(
            runtime->register_trigger(hostView, &bad, &tmpId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );
        bad       = spec;
        bad.policy = 99;
        XX_TEST_EXPECT_EQ(
            runtime->register_trigger(hostView, &bad, &tmpId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );
        bad            = spec;
        bad.channel_id = 999999;
        XX_TEST_EXPECT_EQ(
            runtime->register_trigger(hostView, &bad, &tmpId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND}
        );
        bad            = spec;
        bad.delivery   = PluginStringView::fromCstr("no-such");
        XX_TEST_EXPECT_EQ(
            runtime->register_trigger(hostView, &bad, &tmpId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );

        // 发一条事件: 载荷里的 text 直接成为输入
        sinkA.clear();
        XX_TEST_EXPECT_EQ(
            runtime->emit_event(
                hostView,
                &SvArg(std::string_view{topic}),
                &SvArg(R"({"text":"来自触发器的消息"})")
            ),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        XX_TEST_EXPECT_TRUE(co_await waitUntil(
            ex,
            30s,
            [&]() { return sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END) > 0; }
        ));
        // 回显就是触发器投递的文本
        XX_TEST_EXPECT_TRUE(
            sinkA.textOf(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START).find("来自触发器的消息")
            != std::string::npos
        );

        // (b) COALESCE: 窗口内的多条合并成一条投递
        AgentxxPluginTriggerSpec merge = spec;
        merge.policy                   = AGENTXX_PLUGIN_TRIGGER_POLICY_COALESCE;
        merge.debounce_ms              = 120;
        merge.topic                    = PluginStringView::fromCstr("runtime.batch");
        int64_t mergeTrigger           = 0;
        XX_TEST_EXPECT_EQ(
            runtime->register_trigger(hostView, &merge, &mergeTrigger),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        sinkA.clear();
        for (int i = 0; i < 3; ++i) {
            XX_TEST_EXPECT_EQ(
                runtime->emit_event(
                    hostView,
                    &SvArg("runtime.batch"),
                    &SvArg(std::string_view{fmt::format(R"({{"text":"batch-{}"}})", i)})
                ),
                int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
            );
        }
        XX_TEST_EXPECT_TRUE(co_await waitUntil(
            ex,
            30s,
            [&]() { return sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END) > 0; }
        ));
        // 三条合并成一条输入: 只跑了一轮, 回显里三条都在
        XX_TEST_EXPECT_EQ(sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START), size_t{1});
        const auto echo = sinkA.textOf(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START);
        XX_TEST_EXPECT_TRUE(echo.find("batch-0") != std::string::npos);
        XX_TEST_EXPECT_TRUE(echo.find("batch-1") != std::string::npos);
        XX_TEST_EXPECT_TRUE(echo.find("batch-2") != std::string::npos);

        // (c) DROP_IF_BUSY: 通道正在跑一轮时丢弃
        AgentxxPluginTriggerSpec drop = spec;
        drop.policy                   = AGENTXX_PLUGIN_TRIGGER_POLICY_DROP_IF_BUSY;
        drop.topic                    = PluginStringView::fromCstr("runtime.drop");
        int64_t dropTrigger           = 0;
        XX_TEST_EXPECT_EQ(
            runtime->register_trigger(hostView, &drop, &dropTrigger),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        // 让这一轮慢下来: 先起一轮, 等它变忙, 再发事件 (应被丢弃)
        g_da_sim_delay_ms = 300;
        sinkA.clear();
        const int requestBefore = g_da_sim_request_count.load();
        XX_TEST_EXPECT_EQ(
            runtime->send_input(
                hostView,
                channelA,
                &SvArg(R"({"text":"slow turn"})")
            ),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        XX_TEST_EXPECT_TRUE(co_await waitUntil(
            ex,
            10s,
            [&]() { return sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START) > 0; }
        ));
        XX_TEST_EXPECT_EQ(
            runtime->emit_event(
                hostView,
                &SvArg("runtime.drop"),
                &SvArg(R"({"text":"应当被丢弃"})")
            ),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        XX_TEST_EXPECT_TRUE(co_await waitUntil(
            ex,
            30s,
            [&]() { return sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END) > 0; }
        ));
        g_da_sim_delay_ms = 0;
        // 只多了一次模型调用 (被丢弃的那条没有起轮次)
        XX_TEST_EXPECT_EQ(g_da_sim_request_count.load(), requestBefore + 1);

        // 清单里能看到丢弃计数
        PluginxxString listOut{nullptr, 0};
        XX_TEST_EXPECT_EQ(runtime->list_runtime(hostView, &listOut), int32_t{0});
        const auto listed = utilxx_base::Json::parse(takeString(hostView, listOut));
        int        dropped = 0;
        for (const auto& item : listed["triggers"]) {
            if (item.value("triggerId", int64_t{0}) == dropTrigger) {
                dropped = item.value("dropped", 0);
            }
        }
        XX_TEST_EXPECT_GE(dropped, 1);

        // 注销: 三个都撤掉; 再注销一次报不存在
        XX_TEST_EXPECT_EQ(runtime->unregister_trigger(hostView, mergeTrigger), int32_t{0});
        XX_TEST_EXPECT_EQ(runtime->unregister_trigger(hostView, dropTrigger), int32_t{0});
        XX_TEST_EXPECT_EQ(runtime->unregister_trigger(hostView, queueTrigger), int32_t{0});
        XX_TEST_EXPECT_EQ(
            runtime->unregister_trigger(hostView, queueTrigger),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND}
        );
        // 注销之后事件不再投递
        sinkA.clear();
        XX_TEST_EXPECT_EQ(
            runtime->emit_event(
                hostView,
                &SvArg(std::string_view{topic}),
                &SvArg(R"({"text":"已经没有触发器了"})")
            ),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        co_await waitUntil(ex, 300ms, [&]() {
            return sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START) > 0;
        });
        XX_TEST_EXPECT_EQ(sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START), size_t{0});
    }

    // ===================== 12. 定时器 =====================
    {
        // (a) 一次性: 触发一次后自动注销
        AgentxxPluginTimerSpec once{};
        once.struct_size    = sizeof(AgentxxPluginTimerSpec);
        once.channel_id     = channelA;
        once.first_delay_ms = 20;
        once.interval_ms    = 0;
        once.input_json     = PluginStringView::fromCstr(R"({"text":"一次性提醒"})");
        int64_t onceId      = 0;
        XX_TEST_EXPECT_EQ(
            runtime->register_timer(hostView, &once, &onceId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        XX_TEST_EXPECT_TRUE(onceId > 0);
        sinkA.clear();
        XX_TEST_EXPECT_TRUE(co_await waitUntil(
            ex,
            30s,
            [&]() { return sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END) > 0; }
        ));
        XX_TEST_EXPECT_TRUE(
            sinkA.textOf(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START).find("一次性提醒")
            != std::string::npos
        );
        // 一次性定时器触发后不在了
        XX_TEST_EXPECT_EQ(
            runtime->unregister_timer(hostView, onceId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND}
        );

        // (b) 周期: 触发多次, 注销后停止
        AgentxxPluginTimerSpec periodic = once;
        periodic.first_delay_ms         = 10;
        periodic.interval_ms            = 40;
        periodic.input_json             = PluginStringView::fromCstr(R"({"text":"周期提醒"})");
        int64_t periodicId              = 0;
        XX_TEST_EXPECT_EQ(
            runtime->register_timer(hostView, &periodic, &periodicId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        XX_TEST_EXPECT_TRUE(co_await waitUntil(
            ex,
            30s,
            [&]() {
                PluginxxString out{nullptr, 0};
                if (runtime->list_runtime(hostView, &out) != 0) {
                    return false;
                }
                const auto listed = utilxx_base::Json::parse(takeString(hostView, out));
                for (const auto& item : listed["timers"]) {
                    if (item.value("timerId", int64_t{0}) == periodicId) {
                        return item.value("fired", 0) >= 2;
                    }
                }
                return false;
            }
        ));
        XX_TEST_EXPECT_EQ(runtime->unregister_timer(hostView, periodicId), int32_t{0});
        XX_TEST_EXPECT_EQ(
            runtime->unregister_timer(hostView, periodicId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND}
        );

        // 参数校验: input_json 必填 / 非法 JSON / 通道不存在 / 负数延迟
        AgentxxPluginTimerSpec bad = once;
        bad.input_json             = PluginStringView::fromCstr("");
        int64_t tmpId              = 0;
        XX_TEST_EXPECT_EQ(
            runtime->register_timer(hostView, &bad, &tmpId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );
        bad            = once;
        bad.input_json = PluginStringView::fromCstr("{");
        XX_TEST_EXPECT_EQ(
            runtime->register_timer(hostView, &bad, &tmpId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );
        bad            = once;
        bad.channel_id = 999999;
        XX_TEST_EXPECT_EQ(
            runtime->register_timer(hostView, &bad, &tmpId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND}
        );
        bad                 = once;
        bad.first_delay_ms  = -5;
        XX_TEST_EXPECT_EQ(
            runtime->register_timer(hostView, &bad, &tmpId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );
    }

    // ===================== 13. 关通道: 连带注销 + 会话占用释放 =====================
    {
        // 先给通道挂一个触发器与一个定时器: 关通道时这两个必须一起消失
        AgentxxPluginTriggerSpec spec{};
        spec.struct_size = sizeof(AgentxxPluginTriggerSpec);
        spec.policy      = AGENTXX_PLUGIN_TRIGGER_POLICY_QUEUE;
        spec.topic       = PluginStringView::fromCstr("runtime.will-vanish");
        spec.channel_id  = channelA;
        int64_t triggerId = 0;
        XX_TEST_EXPECT_EQ(
            runtime->register_trigger(hostView, &spec, &triggerId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        AgentxxPluginTimerSpec timerSpec{};
        timerSpec.struct_size    = sizeof(AgentxxPluginTimerSpec);
        timerSpec.channel_id     = channelA;
        timerSpec.first_delay_ms = 60000; ///< 远到不会触发
        timerSpec.input_json     = PluginStringView::fromCstr(R"({"text":"不会触发"})");
        int64_t timerId          = 0;
        XX_TEST_EXPECT_EQ(
            runtime->register_timer(hostView, &timerSpec, &timerId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );

        sinkA.clear();
        XX_TEST_EXPECT_EQ(
            runtime->close_channel(hostView, channelA),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        // 关闭事件恰好一条, 且之后不再有事件
        XX_TEST_EXPECT_EQ(sinkA.count(AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED), size_t{1});
        const auto closedPayload = sinkA.jsonOf(AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED);
        XX_TEST_EXPECT_EQ(closedPayload.value("reason", std::string{}), std::string{"closed"});

        // 关闭之后通道不可用
        XX_TEST_EXPECT_EQ(
            runtime->send_input(
                hostView,
                channelA,
                &SvArg(R"({"text":"after close"})")
            ),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND}
        );
        XX_TEST_EXPECT_EQ(
            runtime->close_channel(hostView, channelA),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND}
        );
        // 连带注销: 触发器与定时器都随通道消失
        XX_TEST_EXPECT_EQ(
            runtime->unregister_trigger(hostView, triggerId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND}
        );
        XX_TEST_EXPECT_EQ(
            runtime->unregister_timer(hostView, timerId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND}
        );
        // 根还在, 但下面已经没有通道
        PluginxxString out{nullptr, 0};
        XX_TEST_EXPECT_EQ(runtime->list_runtime(hostView, &out), int32_t{0});
        const auto listed = utilxx_base::Json::parse(takeString(hostView, out));
        const auto rootJson = findRootJson(listed, rootA);
        XX_TEST_EXPECT_EQ(rootJson["channels"].size(), size_t{0});

        // 会话占用已释放: 同 sessionId 可以重新开一个通道
        EventSink again;
        AgentxxPluginChannelSpec chSpec{};
        chSpec.struct_size = sizeof(AgentxxPluginChannelSpec);
        chSpec.root_id     = PluginStringView::from(std::string_view{rootA});
        chSpec.session_id  = PluginStringView::fromCstr("rt_sess_a");
        chSpec.on_event    = &onChannelEvent;
        chSpec.user_data   = &again;
        int64_t reopened   = 0;
        XX_TEST_EXPECT_EQ(
            runtime->open_channel(hostView, &chSpec, &reopened),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        XX_TEST_EXPECT_TRUE(co_await waitUntil(
            ex,
            30s,
            [&]() { return again.count(AGENTXX_PLUGIN_CHANNEL_EVT_READY) > 0; }
        ));
        // 重开时可以换模型 / 取消 (本轮没在跑, 取消只是空操作)
        XX_TEST_EXPECT_EQ(
            runtime->select_model(hostView, reopened, &SvArg("test-sim")),
            int32_t{0}
        );
        XX_TEST_EXPECT_EQ(
            runtime->select_model(hostView, reopened, &SvArg("no-such")),
            int32_t{0}
        );
        XX_TEST_EXPECT_EQ(runtime->cancel(hostView, reopened), int32_t{0});
        XX_TEST_EXPECT_EQ(
            runtime->select_model(hostView, reopened, &SvArg("")),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_ARG}
        );
        XX_TEST_EXPECT_EQ(
            runtime->cancel(hostView, 999999),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND}
        );
        // 谁也不能应答一个不存在的中断
        XX_TEST_EXPECT_EQ(
            runtime->respond_interrupt(hostView, reopened, 1, &SvArg("{}")),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_STATE}
        );
    }

    // ===================== 14. 卸载根 =====================
    {
        // 先给根 A 开一个通道 (卸载时它必须跟着关)
        EventSink victim;
        AgentxxPluginChannelSpec chSpec{};
        chSpec.struct_size = sizeof(AgentxxPluginChannelSpec);
        chSpec.root_id     = PluginStringView::from(std::string_view{rootA});
        chSpec.session_id  = PluginStringView::fromCstr("rt_sess_victim");
        chSpec.on_event    = &onChannelEvent;
        chSpec.user_data   = &victim;
        int64_t victimId   = 0;
        XX_TEST_EXPECT_EQ(
            runtime->open_channel(hostView, &chSpec, &victimId),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        XX_TEST_EXPECT_TRUE(co_await waitUntil(
            ex,
            30s,
            [&]() { return victim.count(AGENTXX_PLUGIN_CHANNEL_EVT_READY) > 0; }
        ));

        XX_TEST_EXPECT_EQ(
            runtime->unmount_root(hostView, &SvArg(std::string_view{rootA})),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        XX_TEST_EXPECT_TRUE(victim.count(AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED) >= 1);
        XX_TEST_EXPECT_EQ(host->rootCount(), size_t{1});
        XX_TEST_EXPECT_TRUE(host->findRoot(rootA) == nullptr);
        // 已卸掉的根: 再卸一次报不存在
        XX_TEST_EXPECT_EQ(
            runtime->unmount_root(hostView, &SvArg(std::string_view{rootA})),
            int32_t{AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND}
        );
        // 别的实例的根同样按不存在处理
        XX_TEST_EXPECT_EQ(
            runtime->unmount_root(hostView, &SvArg(std::string_view{rootB})),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        XX_TEST_EXPECT_EQ(host->rootCount(), size_t{0});
    }

    // ===================== 15. 禁用回收 =====================
    {
        // 重新挂一个根 + 一个通道, 然后禁用插件: 两者都要消失
        AgentxxPluginMountRootSpec spec{};
        spec.struct_size = sizeof(AgentxxPluginMountRootSpec);
        spec.name        = PluginStringView::fromCstr("probe_disabled");
        spec.config_json = PluginStringView::from(std::string_view{cfgA});
        PluginxxString out{nullptr, 0};
        std::string    rootC;
        XX_TEST_EXPECT_EQ(
            runtime->mount_root(hostView, &spec, &out),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        rootC = takeString(hostView, out);
        XX_TEST_EXPECT_FALSE(rootC.empty());

        EventSink sinkC;
        AgentxxPluginChannelSpec chSpec{};
        chSpec.struct_size = sizeof(AgentxxPluginChannelSpec);
        chSpec.root_id     = PluginStringView::from(std::string_view{rootC});
        chSpec.session_id  = PluginStringView::fromCstr("rt_sess_disabled");
        chSpec.on_event    = &onChannelEvent;
        chSpec.user_data   = &sinkC;
        int64_t channelC   = 0;
        XX_TEST_EXPECT_EQ(
            runtime->open_channel(hostView, &chSpec, &channelC),
            int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        XX_TEST_EXPECT_TRUE(co_await waitUntil(
            ex,
            30s,
            [&]() { return sinkC.count(AGENTXX_PLUGIN_CHANNEL_EVT_READY) > 0; }
        ));

        // 禁用: 主程序侧注册整批摘除 → 运行时对象按归属回收
        mgr->disable(inst->name);
        XX_TEST_EXPECT_TRUE(co_await waitUntil(
            ex,
            30s,
            [&]() {
                return sinkC.count(AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED) > 0
                       && host->rootCount() == 0;
            }
        ));
        XX_TEST_EXPECT_TRUE(host->findRoot(rootC) == nullptr);
        // 禁用后本实例的一切入口都先被框架挡下 (admission lease 不再发放), 因此这里
        // 拿到的是"被拒"而不是"通道不存在"; 两种情况都只要求它不再受理
        XX_TEST_EXPECT_TRUE(
            runtime->send_input(hostView, channelC, &SvArg(R"({"text":"x"})"))
            != int32_t{AGENTXX_PLUGIN_RUNTIME_OK}
        );
        PluginxxString listOut{nullptr, 0};
        XX_TEST_EXPECT_EQ(runtime->list_runtime(hostView, &listOut), int32_t{0});
        const auto listed = utilxx_base::Json::parse(takeString(hostView, listOut));
        XX_TEST_EXPECT_EQ(listed["roots"].size(), size_t{0});
    }

    // ---- 收尾: 放开实例记录, 让容器关根时不再等插件关闭 ----
    mgr->plugins_.erase(inst->name);
    co_return;
}

} // namespace

// ===========================================================================
// 入口
// ===========================================================================

asio::awaitable<TestResult> run_plugin_agent_runtime_tests() {
    g_rt_passed = 0;
    g_rt_failed = 0;

    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);
    g_da_sim_response_content     = "runtime answer";
    g_da_sim_tool_calls           = utilxx_base::Json::array();
    g_da_sim_tool_calls_remaining = 0;
    g_da_sim_delay_ms             = 0;

    // 容器自己的 io 线程: 根 init、会话驱动循环、通道收发、事件回调都在这一条上
    // (生产里这条线程由主程序提供; 用例自己给它一条, 否则 run() 永远不会开始跑)
    auto io   = std::make_shared<asio::io_context>();
    auto work = asio::make_work_guard(*io);
    auto fut  = asio::co_spawn(*io, runBody(io, baseUrl), asio::use_future);
    std::thread ioThread([io]() {
        io->run();
    });

    // 用例线程等容器线程跑完 (容器线程不会反过来等这条线程)
    try {
        fut.get();
    } catch (const std::exception& e) {
        TEST_FAIL << "plugin_agent_runtime body threw: " << e.what() << std::endl;
        g_rt_failed++;
    }

    work.reset();
    io->stop();
    if (ioThread.joinable()) {
        ioThread.join();
    }
    // 模拟服务可能在一条轮次刚结束时还有连接在收尾, 交由它的析构处理
    co_return TestResult{g_rt_passed, g_rt_failed};
}

} // namespace test
} // namespace agentxx
