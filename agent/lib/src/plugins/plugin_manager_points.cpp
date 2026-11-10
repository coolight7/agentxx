/// 功能扩展点 (point) 的声明与分发 —— 主程序侧入口
///
/// 一个扩展点 = 一个 id, 同时是能力 (实现面) 与同名广播频道 (通知面), 见
/// `agentxx/plugin/api/agentxx_point_ids.h`。这里只放"声明表"与"发一次"的薄封装,
/// 具体调用点 (轮次/模型/工具/压缩) 在后续迁移里逐个接上来。
#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/plugin/api/agentxx_point_ids.h"

#include "fmt/format.h"

namespace agentxx {
namespace plugin {

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

} // namespace plugin
} // namespace agentxx
