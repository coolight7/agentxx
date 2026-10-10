/// agentxx 功能点注册表实现 (点表 / 清单 / 插件面入口)
#include "agentxx/feature/registry.h"

#include "agentxx/agent/config_static.h"
#include "utilxx_base/log.h"
#include <algorithm>
#include <cctype>

namespace agentxx {
namespace feature {

namespace {

/// 归属是否属于插件声明 (`plugin:<插件名>`)
bool isPluginOwner(std::string_view owner) {
    return owner.starts_with("plugin:") && owner.size() > 7;
}

/// id 是否含空白字符 (含空白直接判为非法: 这类 id 用点分名字, 不允许空格)
bool hasWhitespace(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
}

} // namespace

Registry::Registry() = default;

Registry::~Registry() = default;

bool Registry::devMode() noexcept {
    return agentxx::agent::AgentConfigStatic::devMode;
}

PointBase& Registry::addPoint(std::unique_ptr<PointBase> point) {
    PointBase* raw = point.get();
    storage_.push_back(std::move(point));
    // id 已被占用时不顶掉已有点 (新点仍由 storage_ 持有, 保证返回的引用有效)
    byId_.try_emplace(raw->id(), raw);
    return *raw;
}

JsonProvidePoint& Registry::provideJson(std::string_view id, PointOptions opts, std::string origin) {
    if (auto* existing = find(id); existing != nullptr) {
        if (!existing->origin().starts_with("plugin:")) {
            XX_LOGE("功能点 `{}` 已存在 (来源 `{}`), 本次声明未注册进点表", id, existing->origin());
            return static_cast<JsonProvidePoint&>(addPoint(std::make_unique<JsonProvidePoint>(
                std::string{id},
                std::move(opts),
                std::move(origin)
            )));
        }
        // 插件点覆盖: 先撤掉旧点 (含其实现), 再声明新的
        undefinePluginPoint(id);
    }
    auto point = std::make_unique<JsonProvidePoint>(std::string{id}, std::move(opts), std::move(origin));
    return static_cast<JsonProvidePoint&>(addPoint(std::move(point)));
}

PointBase* Registry::find(std::string_view id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : it->second;
}

size_t Registry::pointsCount() const noexcept {
    return byId_.size();
}

int32_t Registry::definePluginPoint(const PluginPointDecl& decl) {
    if (decl.id.empty() || hasWhitespace(decl.id)) {
        XX_LOGW("插件点声明被拒绝: id 为空或含空白字符 (`{}`)", decl.id);
        return -1;
    }
    if (!isPluginOwner(decl.owner)) {
        XX_LOGW("插件点 `{}` 声明被拒绝: 归属 `{}` 不是插件", decl.id, decl.owner);
        return -1;
    }
    if (decl.type != PointType::Provide) {
        XX_LOGW(
            "插件点 `{}` 声明被拒绝: 本轮只允许 `provide` 类型 (收到 `{}`)",
            decl.id,
            pointTypeKey(decl.type)
        );
        return -1;
    }
    // 命名空间强校验: id 必须落在 `plugin.<本实例插件名>.*`
    const std::string_view pluginName = std::string_view{decl.owner}.substr(7);
    const std::string      prefix     = fmt::format("plugin.{}.", pluginName);
    if (!decl.id.starts_with(prefix) || decl.id.size() <= prefix.size()) {
        XX_LOGW(
            "插件点 `{}` 声明被拒绝: id 必须落在 `plugin.{}.` 命名空间内",
            decl.id,
            pluginName
        );
        return -1;
    }
    if (auto* existing = find(decl.id); existing != nullptr) {
        if (!existing->origin().starts_with("plugin:")) {
            XX_LOGE("插件点 `{}` 声明被拒绝: 与来源 `{}` 的点冲突", decl.id, existing->origin());
            return -1;
        }
        // 同一 id 重复声明 = 覆盖 (记一条日志, 方便排查两个插件抢同一个点)
        XX_LOGI("插件点 `{}` 重复声明 (原来源 `{}`), 覆盖", decl.id, existing->origin());
        undefinePluginPoint(decl.id);
    }

    PointOptions opts;
    opts.title         = decl.title.empty() ? decl.id : decl.title;
    opts.depict        = decl.depict;
    opts.callable      = true; // 插件声明的点在其声明期间恒为可调
    opts.cache         = CacheMode::None; // 插件点本轮固定不缓存
    opts.implTimeoutMs = decl.implTimeoutMs;
    if (!decl.argsDoc.empty() || !decl.resultDoc.empty()) {
        opts.callDoc = fmt::format("参数: {}; 结果: {}", decl.argsDoc, decl.resultDoc);
    }
    provideJson(decl.id, std::move(opts), decl.owner);
    XX_LOGI("插件点 `{}` 已声明 (来源 `{}`)", decl.id, decl.owner);
    return 0;
}

int32_t Registry::undefinePluginPoint(std::string_view id) {
    auto* point = find(id);
    if (point == nullptr) {
        return -1;
    }
    if (!point->origin().starts_with("plugin:")) {
        XX_LOGW("功能点 `{}` 撤销被拒绝: 来源 `{}` 不是插件点", id, point->origin());
        return -1;
    }
    byId_.erase(std::string{id});
    std::erase_if(storage_, [point](const std::unique_ptr<PointBase>& item) {
        return item.get() == point;
    });
    return 0;
}

int32_t Registry::addImpl(std::string_view pointId, ImplSpec spec) {
    auto* point = find(pointId);
    if (point == nullptr) {
        XX_LOGW(
            "功能点 `{}` 的实现登记被拒绝: 点不存在 (插件目录先 define_point, "
            "应用点需先由宿主装配声明)",
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
    std::vector<std::string> ids;
    for (const auto& [id, point] : byId_) {
        if (point->origin() == owner) {
            ids.push_back(id);
        }
    }
    size_t removed = 0;
    for (const auto& id : ids) {
        // 撤点同时撤掉这些点上的全部实现 (声明方走了, 实现没有意义)
        if (auto* point = find(id); point != nullptr) {
            point->clearImpls();
        }
        if (undefinePluginPoint(id) == 0) {
            ++removed;
        }
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
