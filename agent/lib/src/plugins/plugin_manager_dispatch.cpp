/// 通知型扩展点 (原钩子点) 的派发 —— 7 处调用点各发一次广播
///
/// 插件参与方式只有一种: **订阅同名广播频道** (`agentxx.agent.point.*`, 见
/// `agentxx/plugin/api/agentxx_point_ids.h`)。插件侧的处理器注册表 (含优先级、
/// 句柄、core 层) 已随 `agentxx.agent.hooks` 表一起删除 —— 广播本身按订阅顺序投递,
/// 派发方也不必再排序、等待或处理失败。
///
/// 本文件三件事:
/// 1. 一个中间件句柄 [PluginHookDispatchHandle]: 挂在中间件链上的 7 个回调,
///    每个回调把对应扩展点发一次 (`PluginManager::publishPoint`);
/// 2. 点 id 与载荷 (载荷固定 `{sessionId, agentId, point}`, 不进会话内容);
/// 3. 清单 [PluginMount::hooksJson]: 点 id / 派发次数 (派发记录只在开发者模式下收集)。
///
/// 线程约定: 派发在主程序 io 线程执行 (中间件链本来就在这条线程)。
#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/context.h"
#include "agentxx/plugin/api/agentxx_point_ids.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <array>
#include <chrono>

namespace agentxx {
namespace plugin {

namespace {

/// 通知型扩展点 → id 与展示名 (顺序与 PluginMount::HookPoint 一致)
struct HookPointMeta {
    std::string_view id;
    std::string_view name;
};

constexpr std::array<HookPointMeta, PluginMount::kHookPointCount> kHookPointMeta{{
    {points::TurnStart, "TURN_START"},
    {points::TurnEnd, "TURN_END"},
    {points::ModelStart, "MODEL_START"},
    {points::ModelRun, "MODEL_RUN"},
    {points::ModelEnd, "MODEL_END"},
    {points::ToolStart, "TOOL_START"},
    {points::ToolEnd, "TOOL_END"},
}};

constexpr size_t hookPointIndex(PluginMount::HookPoint point) {
    return static_cast<size_t>(point);
}

/// 交到订阅方手里的载荷
/// - 只给"发生了什么"与在哪, 不带会话内容 (不随上下文增长)
utilxx_base::Json summarizeNodeInput(
    PluginMount::HookPoint            point,
    std::string_view                  agentId,
    const neograph::graph::NodeInput& in
) {
    utilxx_base::Json j;
    j["agentId"]   = std::string{agentId};
    j["sessionId"] = in.ctx.thread_id;
    j["point"]     = std::string{kHookPointMeta[hookPointIndex(point)].name};
    return j;
}

} // namespace

// =====================================================================
// 派发
// =====================================================================

asio::awaitable<void> PluginMount::dispatchHook(
    HookPoint                         point,
    const neograph::graph::NodeInput& in
) {
    if (hookPointIndex(point) >= kHookPointCount) {
        co_return;
    }
    const auto begin    = std::chrono::steady_clock::now();
    const auto pointId  = kHookPointMeta[hookPointIndex(point)].id;
    const auto agentId  = agentId_;
    auto       payload  = summarizeNodeInput(point, agentId, in).dump();
    // 订阅同名的插件在这里收到; 没有订阅方时就是一次哈希查找
    auto host = host_.lock();
    if (host != nullptr) {
        host->publishPoint(pointId, agentId, payload);
    }
    if (agentxx::agent::AgentConfigStatic::devMode) {
        auto&      stat = hookStats_[hookPointIndex(point)];
        const auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - begin
        )
                            .count();
        ++stat.dispatches;
        stat.lastMs = static_cast<uint64_t>(ms < 0 ? 0 : ms);
    }
}

// =====================================================================
// 单派发器 (中间件链)
// =====================================================================

void PluginMount::ensureHookDispatch() {
    auto ctx = agent_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        // 中间件链还没加载 (理论上不会: 插件在 initMiddleware 之后才加载)
        XX_LOGW("扩展点派发器无法挂载: 中间件上下文尚未加载");
        return;
    }
    if (hookDispatch_ == nullptr) {
        hookDispatch_ = std::make_shared<PluginHookDispatchHandle>(
            "plugin_points",
            agent_,
            shared_from_this()
        );
    }
    auto&      handles  = ctx->middlewareHandleContext->handles;
    const bool attached = std::any_of(
        handles.begin(),
        handles.end(),
        [this](const std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>& h) {
            return h.get() == hookDispatch_.get();
        }
    );
    if (!attached) {
        handles.push_back(hookDispatch_);
    }
}

/// 把派发器从中间件链上摸掉 (拆挂载时调; 重复调用无害)
void PluginMount::detachHookDispatch() {
    if (hookDispatch_ == nullptr) {
        return;
    }
    if (auto ctx = agent_.lock(); ctx && ctx->middlewareHandleContext) {
        auto& handles = ctx->middlewareHandleContext->handles;
        handles.erase(
            std::remove_if(
                handles.begin(),
                handles.end(),
                [this](const std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>& h
                ) { return h.get() == hookDispatch_.get(); }
            ),
            handles.end()
        );
    }
    hookDispatch_.reset();
}

// =====================================================================
// 清单
// =====================================================================

std::string PluginMount::hooksJson() {
    const bool        devMode = agentxx::agent::AgentConfigStatic::devMode;
    utilxx_base::Json points  = utilxx_base::Json::array();
    for (size_t i = 0; i < kHookPointCount; ++i) {
        utilxx_base::Json pointJson = utilxx_base::Json::object();
        pointJson["id"]             = std::string{kHookPointMeta[i].id};
        pointJson["name"]           = std::string{kHookPointMeta[i].name};
        if (devMode) {
            // 派发记录只在开发者模式下出现 (关闭时不是"全 0", 而是没有该段)
            const auto&       stat = hookStats_[i];
            utilxx_base::Json st   = utilxx_base::Json::object();
            st["dispatches"]       = static_cast<int64_t>(stat.dispatches);
            st["lastMs"]           = static_cast<int64_t>(stat.lastMs);
            pointJson["stat"]      = std::move(st);
        }
        points.push_back(std::move(pointJson));
    }

    utilxx_base::Json out = utilxx_base::Json::object();
    out["devMode"]        = devMode;
    out["count"]          = static_cast<int64_t>(kHookPointCount);
    out["points"]         = std::move(points);
    return out.dump();
}

// =====================================================================
// 派发器 (中间件句柄)
// =====================================================================

asio::awaitable<void> PluginHookDispatchHandle::onAgentcallStartFunc(neograph::graph::NodeInput& in
) {
    if (auto mount = mount_.lock()) {
        co_await mount->dispatchHook(PluginMount::HookPoint::TurnStart, in);
    }
}

asio::awaitable<void> PluginHookDispatchHandle::
    onAgentcallEndFunc(const neograph::graph::NodeInput& in, neograph::graph::NodeOutput&) {
    if (auto mount = mount_.lock()) {
        co_await mount->dispatchHook(PluginMount::HookPoint::TurnEnd, in);
    }
}

asio::awaitable<void> PluginHookDispatchHandle::onModelcallStartFunc(neograph::graph::NodeInput& in
) {
    if (auto mount = mount_.lock()) {
        co_await mount->dispatchHook(PluginMount::HookPoint::ModelStart, in);
    }
}

asio::awaitable<void> PluginHookDispatchHandle::onModelcallRunFunc(neograph::graph::NodeInput& in) {
    if (auto mount = mount_.lock()) {
        co_await mount->dispatchHook(PluginMount::HookPoint::ModelRun, in);
    }
}

asio::awaitable<void> PluginHookDispatchHandle::
    onModelcallEndFunc(const neograph::graph::NodeInput& in, neograph::graph::NodeOutput&) {
    if (auto mount = mount_.lock()) {
        co_await mount->dispatchHook(PluginMount::HookPoint::ModelEnd, in);
    }
}

asio::awaitable<void> PluginHookDispatchHandle::onToolcallStartFunc(neograph::graph::NodeInput& in
) {
    if (auto mount = mount_.lock()) {
        co_await mount->dispatchHook(PluginMount::HookPoint::ToolStart, in);
    }
}

asio::awaitable<void> PluginHookDispatchHandle::
    onToolcallEndFunc(const neograph::graph::NodeInput& in, neograph::graph::NodeOutput&) {
    if (auto mount = mount_.lock()) {
        co_await mount->dispatchHook(PluginMount::HookPoint::ToolEnd, in);
    }
}

} // namespace plugin
} // namespace agentxx
