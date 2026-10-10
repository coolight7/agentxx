/// agentxx 功能点注册表 —— 点表 + 清单 + 插件面入口
///
/// 一个 [Registry] 随一个 `AgentContext` 一份 (多 agent 实例互不影响, 不使用全局
/// 状态)。核心点由装配代码声明 (中间件 / 节点注册), 插件点由插件经
/// [definePluginPoint] 在运行期声明; 两处的实现登记都落到点上。
///
/// 注册表只提供:
/// - 声明点 ([provide] / [provideJson] / [definePluginPoint]);
/// - 实现登记与撤销 (插件 / FFI 宿主, 按归属记账, 供禁用与卸载时整批摘除);
/// - 清单 ([listPointsJson]) —— 插件 `list_points`、装配快照与 `--dump-diagnostics`
///   共用同一份实现, 保证"各条出口看到的是同一份事实"。
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

/// 插件声明自己的功能点 (本轮只允许 `provide` 类型)
struct PluginPointDecl {
    /// 点 id, 必须为 `plugin.<本实例插件名>.<名字>`
    std::string id;
    /// 声明方归属 (`plugin:<插件名>`)
    std::string owner;
    /// 类型 (本轮只允许 [PointType::Provide]; 其它取值被拒绝并记日志)
    PointType type = PointType::Provide;
    /// 展示名 (缺省用 id)
    std::string title;
    /// 一句话说明
    std::string depict;
    /// 参数说明 (进清单与作者文档)
    std::string argsDoc;
    /// 结果说明
    std::string resultDoc;
    /// 等实现方的超时 (毫秒; 0 = 不限)
    int32_t implTimeoutMs = 0;
};

/// 功能点注册表
class Registry {
public:

    Registry();
    ~Registry();

    Registry(const Registry&)            = delete;
    Registry& operator=(const Registry&) = delete;

    // ==================== 声明点 ====================

    /// 声明一个取值型功能点 (核心点在自己的装配代码里调用; 早于插件装载)
    /// - 同一 id 重复声明: 类型一致时返回已有点 (无害重声明), 否则记一条错误日志
    ///   并返回新点 (新点不注册进点表, 不会顶掉已有点)
    ///
    /// - `args`:
    ///     - [id] 功能点 id (核心点用 `agentxx.<域>.<名字>`, 稳定清单见 points.h)
    ///     - [opts] 声明选项 (展示名 / 说明 / 可调性 / 值缓存策略 / 等实现超时)
    ///     - [identityOf] 身份规则: 由点声明"这次要算的是哪一份输入"
    ///
    /// `return`: 点对象的引用 (生命周期与注册表一致)
    template<class TReq, class TValue, class IdentityFn>
        requires JsonCodable<TReq> && JsonCodable<TValue>
    ProvidePoint<TReq, TValue>&
        provide(std::string_view id, PointOptions opts, IdentityFn identityOf) {
        if (auto* existing = find(id)) {
            if (auto* typed = dynamic_cast<ProvidePoint<TReq, TValue>*>(existing);
                typed != nullptr) {
                XX_LOGI("功能点 `{}` 重复声明, 复用已有点", id);
                return *typed;
            }
            XX_LOGE("功能点 `{}` 重复声明且类型不一致, 本次声明未注册进点表", id);
        }
        auto point = std::make_unique<ProvidePoint<TReq, TValue>>(
            std::string{id},
            std::move(opts),
            IdentityFn(std::move(identityOf))
        );
        point->setOrigin("core");
        return static_cast<ProvidePoint<TReq, TValue>&>(addPoint(std::move(point)));
    }

    /// 声明一个通用 JSON 取值点 (宿主声明的点用; 插件点走 [definePluginPoint])
    JsonProvidePoint& provideJson(std::string_view id, PointOptions opts, std::string origin);

    /// 插件声明自己的功能点
    /// - id 必须落在 `plugin.<本实例插件名>.*` 且不含空白; 不合法直接拒绝并记日志
    /// - 本轮只允许 `provide` 类型 (`decide` 被拒绝并记日志)
    /// - 声明的点在存活期间恒为可调; 值缓存固定不缓存 (调用方要缓存用结果里的 identity)
    /// - 同一 id 重复声明 = 覆盖 (记一条日志, 方便排查两个插件抢同一个点)
    ///
    /// `return`: 0 成功; 非 0 失败 (id 非法 / 类型不支持 / 与核心点冲突)
    int32_t definePluginPoint(const PluginPointDecl& decl);

    /// 撤销插件点 (含这些点上的全部实现); 只允许撤销 `plugin:` 来源的点
    ///
    /// `return`: 0 成功; 非 0 失败 (点不存在 / 不是插件点)
    int32_t undefinePluginPoint(std::string_view id);

    /// 按 id 取点 (不存在返回 nullptr)
    PointBase* find(std::string_view id) const;

    /// 点总数
    size_t pointsCount() const noexcept;

    // ==================== 实现登记 (插件 / FFI 宿主) ====================

    /// 登记实现
    /// - 点不存在时失败 (应用点未装配 / 插件点未 `define_point`)
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

    /// 撤销某归属声明的点 (插件禁用/卸载; 连带撤掉这些点上的全部实现)
    ///
    /// `return`: 撤销的点数
    size_t removePointsOwnedBy(std::string_view owner);

    // ==================== 清单 ====================

    /// 功能点清单 (插件 `list_points`、装配快照与诊断共用这一份实现)
    utilxx_base::Json listPointsJson() const;

    /// 开发者模式是否开启 (启动期冻结; 清单的 `stat` 段按它出现)
    static bool devMode() noexcept;

private:

    /// 收进点表 (id 已被占用时返回已有点, 新点由调用方持有以便安全返回引用)
    PointBase& addPoint(std::unique_ptr<PointBase> point);

    std::vector<std::unique_ptr<PointBase>>               storage_;
    std::map<std::string, PointBase*, std::less<>>        byId_;
};

} // namespace feature
} // namespace agentxx
