/// SDK 反例编译检查的正向对照（必须编译成功）。
/// 作用：确认检查脚本使用的编译环境有效，避免"全部片段都编译失败"的假阳性。
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"

using namespace agentxx::plugin;

struct Ctx : PluginBase {};

asio::awaitable<void> usePositive(Ctx& ctx) {
    // 1. 工具体 (**唯一形态**: asio 协程体), 体里可以 co_await 各种 `*Async` 入口
    co_await toolAsync(
        ctx,
        ScopeRef{},
        "pos.tool",
        "d",
        "{}",
        [](Ctx&, std::string_view args) -> asio::awaitable<std::string> {
            co_return std::string{args};
        }
    );

    // 2. 阻塞工作: onWorkerAsync 包一层, 工作体在主程序工作池线程上跑
    co_await toolAsync(
        ctx,
        ScopeRef{},
        "pos.blocking",
        "d",
        "{}",
        onWorkerAsync([](Ctx&, std::string_view args) -> std::string { return std::string{args}; })
    );

    // 3. 受控轮询体: 等插件本地 reactor 上的内核就绪事件
    co_await toolAsync(
        ctx,
        ScopeRef{},
        "pos.polled",
        "d",
        "{}",
        [](Ctx&, std::string_view, std::string_view, std::string_view, const PluginxxCancelToken*)
            -> asio::awaitable<std::string> { co_return std::string{}; }
    );

    // 功能埋点实现行 = 能力行 (实现体收载荷, 返回结果正文)
    co_await providePointAsync(
        ctx,
        ScopeRef{},
        "pos.point",
        [](Ctx&, std::string_view payload) -> std::string { return std::string{payload}; }
    );
    co_await capabilityAsync(
        ctx,
        "pos.cap",
        [](Ctx&, std::string_view, std::string_view) -> std::string { return {}; }
    );
    co_await graphNodeAsync(
        ctx,
        ScopeRef{},
        "pos.node",
        "{}",
        [](Ctx&, const RootRequest&) -> std::string { return "{}"; }
    );
    co_return;
}
