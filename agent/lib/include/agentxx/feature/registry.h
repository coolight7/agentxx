/// agentxx 功能埋点注册表 —— 点表 + 清单 + 插件面入口
///
/// 一个 [Registry] 随一个 `AgentContext` 一份 (多 agent 实例互不影响, 不使用全局
/// 状态)。点由加载代码声明 (中间件 / 节点注册), 插件的实现经能力表登记成
/// 实现行 ([PointBase::runPluginLayer]), 两处都落在同一份取值链上。
///
/// 注册表只提供:
/// - 声明点 ([provide] / [provideJson]);
/// - 库内实现登记与撤销 (主程序自己、或单测直接用的那条链);
/// - 清单 ([listPointsJson]) —— 加载快照与 `--dump-diagnostics` 共用同一份实现,
///   保证"各条出口看到的是同一份事实"。
#pragma once

#include "agentxx/feature/feature.h"
#include "utilxx_base/json.h"
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace feature {

/// 插件声明自己的功能埋点 -> 由能力表承担, 这里不再提供声明入口

/// 功能埋点 id 的命名检查 (核心点必须是 `agentxx.agent.*`)
/// - 不合格时只记一条警告日志, 不阻断声明 (第三方主程序自建点不至于因此跑不起来)
void warnIfCorePointMisnamed(std::string_view id);

/// 功能埋点注册表
class Registry {
public:

    Registry();
    ~Registry();

    Registry(const Registry&)            = delete;
    Registry& operator=(const Registry&) = delete;

    // ==================== 声明点 ====================

    /// 声明一个取值型功能埋点 (核心点在自己的加载代码里调用; 早于插件加载)
    /// - 同一 id 重复声明: 类型一致时返回已有点 (无害重声明), 否则记一条错误日志
    ///   并返回新点 (新点不注册进点表, 不会顶掉已有点)
    ///
    /// - `args`:
    ///     - [id] 功能埋点 id (核心点用 `agentxx.agent.<域>.<名字>`, 稳定清单见 points.h)
    ///     - [opts] 声明选项 (展示名 / 说明 / 可调性 / 值缓存策略 / 等实现超时)
    ///     - [identityOf] 身份规则: 由点声明"这次要算的是哪一份输入"
    ///
    /// `return`: 点对象的引用 (生命周期与注册表一致)
    template<class TReq, class TValue, class IdentityFn>
        requires JsonCodable<TReq> && JsonCodable<TValue>
    ProvidePoint<TReq, TValue>&
        provide(std::string_view id, PointOptions opts, IdentityFn identityOf) {
        warnIfCorePointMisnamed(id);
        if (auto* existing = find(id)) {
            if (auto* typed = dynamic_cast<ProvidePoint<TReq, TValue>*>(existing);
                typed != nullptr) {
                XX_LOGI("功能埋点 `{}` 重复声明, 复用已有点", id);
                return *typed;
            }
            XX_LOGE("功能埋点 `{}` 重复声明且类型不一致, 本次声明未注册进点表", id);
        }
        auto point = std::make_unique<ProvidePoint<TReq, TValue>>(
            std::string{id},
            std::move(opts),
            IdentityFn(std::move(identityOf))
        );
        point->setOrigin("core");
        return static_cast<ProvidePoint<TReq, TValue>&>(addPoint(std::move(point)));
    }

    /// 声明一个通用 JSON 取值点 (主程序声明的点用; 插件点走 [definePluginPoint])
    /// - id 必须在 `agentxx.agent.*` 段内 (与事件/主题同一套命名约定), 否则只记一条警告
    JsonProvidePoint& provideJson(std::string_view id, PointOptions opts, std::string origin);

    /// 设置插件层入口 (主程序侧: `PluginManager::emitPoint`)
    ///
    /// - 对已声明的点与之后声明的点都生效;
    /// - `co_await point->ask(...)` 时先发一次同名功能埋点 (实现行 = 能力行), 没人给值再问
    ///   本点自己登记的实现与核心实现。
    void setPluginEmitter(PointBase::PluginEmitFn fn);

    /// 按 id 取点 (不存在返回 nullptr)
    PointBase* find(std::string_view id) const;

    /// 点总数
    size_t pointsCount() const noexcept;

    // ==================== 实现登记 (库内与单测直接使用) ====================

    /// 登记实现
    /// - 点不存在时失败
    /// - 同一 `(点, owner)` 重复登记 = 覆盖; 数量不设上限
    ///
    /// `return`: 0 成功; 非 0 失败
    int32_t addImpl(std::string_view pointId, ImplSpec spec);

    /// 撤销某归属在某点上的实现
    ///
    /// `return`: 撤销的条数 (点不存在返回 0)
    size_t removeImpl(std::string_view pointId, std::string_view owner);

    /// 撤销某归属的全部实现 (跨越所有点; 插件禁用/卸载时调用)
    ///
    /// `return`: 撤销的条数
    size_t removeImplsByOwner(std::string_view owner);

    /// 撤销某归属声明的点 (连带撤掉这些点上的全部实现)
    ///
    /// `return`: 撤销的点数
    size_t removePointsOwnedBy(std::string_view owner);

    // ==================== 清单 ====================

    /// 功能埋点清单 (插件 `list_points`、加载快照与诊断共用这一份实现)
    utilxx_base::Json listPointsJson() const;

    /// 开发者模式是否开启 (启动期冻结; 清单的 `stat` 段按它出现)
    static bool devMode() noexcept;

private:

    /// 收进点表 (id 已被占用时返回已有点, 新点由调用方持有以便安全返回引用)
    PointBase& addPoint(std::unique_ptr<PointBase> point);

    std::vector<std::unique_ptr<PointBase>>        storage_;
    std::map<std::string, PointBase*, std::less<>> byId_;
    /// 插件层入口 (主程序设置; 空 = 没有插件层)
    PointBase::PluginEmitFn                        pluginEmit_;
};

} // namespace feature
} // namespace agentxx
