/// agentxx 功能埋点注册表实现 (点表 / 清单 / 插件面入口)
#include "agentxx/feature/registry.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/event/naming.h"
#include "utilxx_base/log.h"
#include <algorithm>

namespace agentxx {
namespace feature {

namespace {

} // namespace

void warnIfCorePointMisnamed(std::string_view id) {
    std::string err;
    if (agentxx::events::naming::validateFeaturePointId(id, {}, &err)) {
        return;
    }
    XX_LOGW("功能埋点 `{}` 的 id 不合命名约定: {}", id, err);
}

Registry::Registry() = default;

Registry::~Registry() = default;

bool Registry::devMode() noexcept {
    return agentxx::agent::AgentConfigStatic::devMode;
}

PointBase& Registry::addPoint(std::unique_ptr<PointBase> point) {
    PointBase* raw = point.get();
    point->setPluginEmitter(pluginEmit_);
    storage_.push_back(std::move(point));
    // id 已被占用时不顶掉已有点 (新点仍由 storage_ 持有, 保证返回的引用有效)
    byId_.try_emplace(raw->id(), raw);
    return *raw;
}

JsonProvidePoint&
    Registry::provideJson(std::string_view id, PointOptions opts, std::string origin) {
    warnIfCorePointMisnamed(id);
    if (auto* existing = find(id); existing != nullptr) {
        XX_LOGE("功能埋点 `{}` 已存在 (来源 `{}`), 本次声明未注册进点表", id, existing->origin());
        return static_cast<JsonProvidePoint&>(addPoint(std::make_unique<JsonProvidePoint>(
            std::string{id},
            std::move(opts),
            std::move(origin)
        )));
    }
    auto point
        = std::make_unique<JsonProvidePoint>(std::string{id}, std::move(opts), std::move(origin));
    return static_cast<JsonProvidePoint&>(addPoint(std::move(point)));
}

PointBase* Registry::find(std::string_view id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : it->second;
}

size_t Registry::pointsCount() const noexcept {
    return byId_.size();
}

void Registry::setPluginEmitter(PointBase::PluginEmitFn fn) {
    pluginEmit_ = fn;
    for (auto& point : storage_) {
        if (point) {
            point->setPluginEmitter(pluginEmit_);
        }
    }
}

int32_t Registry::addImpl(std::string_view pointId, ImplSpec spec) {
    auto* point = find(pointId);
    if (point == nullptr) {
        XX_LOGW(
            "功能埋点 `{}` 的实现登记被拒绝: 点不存在 (插件目录先 define_point, "
            "应用点需先由主程序加载声明)",
            pointId
        );
        return -1;
    }
    return point->addImpl(std::move(spec));
}

size_t Registry::removeImpl(std::string_view pointId, std::string_view owner) {
    auto* point = find(pointId);
    if (point == nullptr) {
        return 0;
    }
    return point->removeImpls(owner);
}

size_t Registry::removeImplsByOwner(std::string_view owner) {
    if (owner.empty()) {
        return 0;
    }
    size_t removed = 0;
    for (auto& [id, point] : byId_) {
        (void)id;
        removed += point->removeImpls(owner);
    }
    return removed;
}

size_t Registry::removePointsOwnedBy(std::string_view owner) {
    if (owner.empty()) {
        return 0;
    }
    size_t removed = 0;
    for (auto it = byId_.begin(); it != byId_.end();) {
        if (it->second && it->second->origin() == owner) {
            it = byId_.erase(it);
            ++removed;
            continue;
        }
        ++it;
    }
    return removed;
}

utilxx_base::Json Registry::listPointsJson() const {
    utilxx_base::Json out    = utilxx_base::Json::object();
    utilxx_base::Json points = utilxx_base::Json::array();
    for (const auto& [id, point] : byId_) {
        (void)id;
        points.push_back(point->listJson());
    }
    out["devMode"] = devMode();
    out["count"]   = static_cast<int64_t>(byId_.size());
    out["points"]  = std::move(points);
    return out;
}

} // namespace feature
} // namespace agentxx
