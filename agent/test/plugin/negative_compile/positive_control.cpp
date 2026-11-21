/// SDK 反例编译检查的正向对照（必须编译成功）。
/// 作用：确认检查脚本使用的编译环境有效，避免"全部片段都编译失败"的假阳性。
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"

using namespace agentxx::plugin;

struct Ctx : PluginBase {};

void usePositive(Ctx& ctx) {
    // 1. kit 协程体 (Task 体): 可以在体内 co_await kit 原语
    tool(
        ctx,
        ScopeRef{},
        "pos.tool",
        "d",
        "{}",
        [](Ctx&, std::string_view args, OpCtl) -> Task<std::string> {
            co_return std::string{args};
        }
    )
        .forget();

    // 2. 同步体: 在工作池线程上跑, 可以直接阻塞
    tool(
        ctx,
        ScopeRef{},
        "pos.sync",
        "d",
        "{}",
        [](Ctx&, std::string_view args) -> std::string {
            return std::string{args};
        }
    )
        .forget();

    // 3. asio 协程体: 等待插件本地 reactor 上的内核就绪事件
    tool(
        ctx,
        ScopeRef{},
        "pos.polled",
        "d",
        "{}",
        [](Ctx&, std::string_view, std::string_view, std::string_view, const PluginxxCancelToken*)
            -> asio::awaitable<std::string> {
            co_return std::string{};
        }
    )
        .forget();

    // 功能埋点实现行 = 能力行 (实现体收载荷, 返回结果正文)
    providePoint(ctx, ScopeRef{}, "pos.point", [](Ctx&, std::string_view payload) -> std::string {
        return std::string{payload};
    })
        .forget();
    capability(ctx, "pos.cap", [](Ctx&, std::string_view, std::string_view) -> std::string {
        return {};
    })
        .forget();
    graph_node(ctx, ScopeRef{}, "pos.node", "{}", [](Ctx&, const RootRequest&) -> std::string {
        return "{}";
    })
        .forget();
}
