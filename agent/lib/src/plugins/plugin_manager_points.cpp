/// 功能扩展点 (point) 的声明与分发 —— 主程序侧入口
///
/// 一个扩展点 = 一个 id, 同时是能力 (实现面) 与同名广播频道 (通知面), 见
/// `agentxx/plugin/api/agentxx_point_ids.h`。这里只放"声明表"与"发一次"的薄封装,
/// 具体调用点 (轮次/模型/工具/压缩) 在后续迁移里逐个接上来。
#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/plugin/api/agentxx_point_ids.h"

#include "fmt/format.h"

#include <asio/async_result.hpp>
#include <asio/post.hpp>

#include <memory>
#include <optional>

namespace agentxx {
namespace plugin {

namespace {

/// 一次 `emitPoint` 的完成状态: 持有 `use_awaitable` 的完成回调
///
/// 平台保证完成回调**恰好一次**, 所以这块内存由回调自己释放 (回调进来即接管所有权)。
/// 只有"受理失败、回调不会再来"的那条路径由发起方释放。
template <typename Handler>
struct PointEmitState {
    Handler handler;

    explicit PointEmitState(Handler h)
        : handler(std::move(h)) {}

    /// 把结果投递回等待方 (不在这里直接 resume, 交给 asio 的完成语义)
    void complete(std::optional<std::string> value) {
        auto exec = asio::get_associated_executor(handler);
        asio::post(
            exec,
            [h = std::move(handler), v = std::move(value)]() mutable {
                std::move(h)(std::move(v));
            }
        );
    }

    /// 完成回调: 只有 `OK` 且确实带了结果才算"有人给了值"
    static void PLUGINXX_CALL onDone(void* ud, int32_t status, const PluginxxStringView* payload) {
        std::unique_ptr<PointEmitState> self{static_cast<PointEmitState*>(ud)};
        if (!self) {
            return;
        }
        std::optional<std::string> value;
        if (status == PLUGINXX_OPERATOR_OK && payload != nullptr && payload->data != nullptr
            && payload->size > 0) {
            std::string text{payload->data, static_cast<size_t>(payload->size)};
            // 合并结果里的 `null` 等于"没人给值"
            if (!text.empty() && text != "null") {
                value = std::move(text);
            }
        }
        self->complete(std::move(value));
    }
};

} // namespace

void PluginManager::declareAppPoints() {
    // 通知型: 高频, 不缓存 (订阅方只关心"发生了"); 实现行同样开放, 结果没人看
    for (const auto id : points::kNotifyPoints) {
        pluginxx::PluginPointDecl decl;
        decl.id     = std::string{id};
        decl.impl   = true;
        decl.notify = true;
        decl.cache  = pluginxx::BroadcastCache_e::None;
        std::string error;
        if (!declarePoint(decl, &error)) {
            XX_LOGW("point `{}` could not be declared: {}", id, error);
        }
    }
    // 取值型: 订阅方需要当前值, 订阅时先推一次
    for (const auto id : points::kValuePoints) {
        pluginxx::PluginPointDecl decl;
        decl.id              = std::string{id};
        decl.impl            = true;
        decl.notify          = true;
        decl.cache           = pluginxx::BroadcastCache_e::Latest;
        decl.pushOnSubscribe = true;
        std::string error;
        if (!declarePoint(decl, &error)) {
            XX_LOGW("point `{}` could not be declared: {}", id, error);
        }
    }
}

PluginxxOperatorHandle* PluginManager::emitPointAsync(
    std::string_view         pointId,
    std::string_view         agentId,
    std::string_view         payloadJson,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error
) {
    // 作用域 = 挂载时的 agent_id: 实现行按它过滤 (空 = 不按作用域过滤, 只发通知面)
    PluginxxScopeRef scope{};
    scope.struct_size = sizeof(PluginxxScopeRef);
    scope.flags       = 0;
    scope.id          = PluginStringView::from(agentId.data(), agentId.size());
    scope.seq         = 0; ///< 0 = 不校验序号; 作用域隔离只按 id
    auto* handle      = pluginxx::PluginHostCore<PluginInstance>::emitPointAsync(
        agentId.empty() ? nullptr : &scope,
        pointId,
        payloadJson,
        /*timeoutMs=*/0,
        cb,
        ud,
        error
    );
    if (handle == nullptr && error != nullptr && error->data != nullptr) {
        XX_LOGW(
            "point `{}` could not be emitted: {}",
            pointId,
            std::string{error->data, static_cast<size_t>(error->size)}
        );
    }
    return handle;
}

void PluginManager::publishPoint(
    std::string_view pointId,
    std::string_view agentId,
    std::string_view payloadJson
) {
    // 通知型调用点只要"发出去": 不关心实现行的结果, 也不等完成通知
    // (错误串不要了: 拿不到 host 就没法释放, 这里只记一条日志)
    auto* handle = emitPointAsync(pointId, agentId, payloadJson, nullptr, nullptr, nullptr);
    if (handle == nullptr) {
        XX_LOGW("point `{}` could not be emitted", pointId);
    }
}

asio::awaitable<std::optional<std::string>> PluginManager::emitPoint(
    std::string_view agentId,
    std::string_view pointId,
    std::string_view payloadJson
) {
    // 三份入参都要活到回调, 拷贝进发起函数的捕获里
    std::string agent{agentId};
    std::string point{pointId};
    std::string payload{payloadJson};
    // `async_initiate` 的完成令牌按非 const 引用收: 单独拿出来
    asio::use_awaitable_t<> token;
    co_return co_await asio::async_initiate<
        asio::use_awaitable_t<>,
        void(std::optional<std::string>)>(
        [this, agent = std::move(agent), point = std::move(point), payload = std::move(payload)](
            auto handler
        ) {
            using Handler = decltype(handler);
            using State   = PointEmitState<Handler>;
            auto* state   = new State(std::move(handler));
            // 错误串不要了: 失败就是"没人给值", 原因已经在 emitPointAsync 内部记过日志
            auto* handle = this->emitPointAsync(
                point,
                agent,
                payload,
                &State::onDone,
                state,
                nullptr
            );
            if (handle == nullptr) {
                // 受理失败: 回调不会再来, 这里自己收尾并释放
                state->complete(std::nullopt);
                delete state;
            }
        },
        token
    );
}

} // namespace plugin
} // namespace agentxx
