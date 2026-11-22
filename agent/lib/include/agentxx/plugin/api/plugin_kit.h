/// 插件开发 SDK (C++ header-only) —— 两端共用部分
///
/// 三个头按"端"分工, 插件按自己需要的端包含 (双端插件两个都包含):
/// - **本头**: 两端共用 —— 插件框架通用名字的引入 (`pluginxx::kit`)、广播便捷入口
///   (`<应用名>.broadcasts`)、网络便捷入口 (`<应用名>.net`)、生命周期入口的公共
///   实现 (`createCtxEntry` / `destroyCtxEntry`) 与 `agentxx::ui` 描述层别名;
/// - **`agent_plugin_kit.h`**: agent 侧 —— `AgentIfaces`、`PluginBase`、工具注册与
///   模式构建、图节点、工具权限、功能埋点、会话运行时与后台任务便捷调用、
///   `AGENTXX_AGENT_PLUGIN_*_EXPORT` 导出宏;
/// - **`client_plugin_kit.h`**: client 侧 —— `ClientIfaces`、`ClientPluginBase`、
///   client 表调用、界面 / 定时器 / 快捷键便捷方法、`AGENTXX_CLIENT_PLUGIN_*_EXPORT`
///   导出宏。
///
/// 本头自带全部公共依赖的包含; 两端头只补各自的 C API 头。

#pragma once
#include "agentxx/plugin/api/agentxx_result.h"
#include "pluginxx/api/broadcast_handle.h"
#include "pluginxx/api/net.h"
#include "pluginxx/net/ops.h"
#include "asio/associated_executor.hpp"
#include "asio/async_result.hpp"
#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "pluginxx/kit/kit.h"
#include "pluginxx/ui.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/json.h"
#include "utilxx_base/json_view.h"
#include <type_traits>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

/// asio 命名空间别名 (与 pluginxx/kit/kit.h 中同名别名指向同一目标, 重复声明合法)
namespace asio = ::boost::asio;

namespace agentxx {
namespace plugin {

/* ==================== 通用 SDK 名的引入 (pluginxx/kit/kit.h) ====================
 *
 * 插件源码一直书写 `agentxx::plugin::Xxx`, 为保持**插件源码零改动**, 这里把插件框架通用
 * 部分的名字逐条引入本命名空间:
 * - 类型本体只有 `pluginxx` 一份 (using 声明不产生新类型, 无 ODR 风险);
 * - `agentxx::plugin::Task<T>` 与 `pluginxx::Task<T>` 是同一模板;
 * - 新增插件框架通用名字时按需在此追加一行, 引入面始终显式可读。
 *
 * 注意: 这不是"转发头"——没有 `agentxx/plugin/api/plugin_kit_core.h` 之类的路径兼容层,
 * 通用 SDK 只能经 `pluginxx/kit/kit.h` 包含。
 */

using pluginxx::ArgReader;
using pluginxx::CanceledException;
using pluginxx::CancelRegistry;
using pluginxx::capability;
using pluginxx::ctxGuardLogger;
using pluginxx::finishLater;
using pluginxx::ifaceHas;
using pluginxx::ifaceHasVersion;
using pluginxx::invoke_cap;
using pluginxx::Json;
using pluginxx::jsonEscape;
using pluginxx::JsonView;
using pluginxx::LifecycleFinisher;
using pluginxx::logCreateFailure;
using pluginxx::Logger;
using pluginxx::offload;
using pluginxx::OpCtl;
using pluginxx::PluginBaseT;
using pluginxx::PluginIfaceCore;
using pluginxx::pluginLog;
using pluginxx::pluginStrdup;
using pluginxx::PluginString;
using pluginxx::PluginStringView;
using pluginxx::queryInterface;
using pluginxx::RootRequest;
using pluginxx::ScopeRef;
using pluginxx::sleep;
using pluginxx::spawn;
using pluginxx::tableOp;
using pluginxx::tableOpCallback;
using pluginxx::tableOpSyncWait;
using pluginxx::TableOpResult;
using pluginxx::TableOpTicket;
using pluginxx::Task;
using pluginxx::validateInterface;
using pluginxx::whenAll;
using pluginxx::yield;

namespace detail {

// 通用表的阻塞调用形态 (定义在 pluginxx/kit/kit.h): 只取正文 / 只看成败

/// 取一个 host 分配的字符串出参 (用完释放)
template<typename Fn>
inline std::string takeHostString(const PluginxxHost* host, Fn&& fn) {
    PluginxxString out{nullptr, 0};
    const int32_t  rc = fn(&out);
    if (rc != AGENTXX_RESULT_OK || nullptr == out.data) {
        if (nullptr != out.data) {
            PluginString::free(host, &out);
        }
        return std::string{};
    }
    std::string text{out.data, static_cast<size_t>(out.size)};
    PluginString::free(host, &out);
    return text;
}

/// 取一段字符串的实际内容 (接受 `std::string_view` 与跨边界的 `PluginxxStringView`)
inline std::string_view textOfView(std::string_view v) noexcept {
    return v;
}

inline std::string_view textOfView(const PluginxxStringView& v) noexcept {
    return PluginStringView::str(v);
}

} // namespace detail

/* ==================== 广播便捷入口 (插件侧) ==================== *
 * 插件面的发布 / 订阅一律走框架的 `<应用名>.broadcasts` 表
 * (取代早期的 `pluginxx.events` 事件表):
 * - `publishPluginBroadcast`: 频道不存在时先声明 (幂等) 再发布;
 * - `subscribePluginBroadcast`: 订阅 (句柄由主程序随实例回收, 不需手动释放)。
 *
 * 频道 id 写全名: `plugin.<插件名>.*` 可用 (`publish: owner` 时归属就是本插件),
 * `agentxx.agent.*` / `agentxx.client.*` 是主程序频道, 插件只能订阅。
 */

/// 声明一条广播频道 (幂等; 已声明时保持原声明)
///
/// - 插件只能声明自己的频道: 短名 (`demo`) 由主程序补成 `plugin.<插件 id>.demo`,
///   写全名 (`plugin.<插件名>.demo`) 也可以;
/// - `agentxx.*` 是主程序频道, 插件声明会被拒 (需要时请主程序声明);
/// - `anyonePublish` = true 时两端都能发 (跨端共享频道用)。
/// - 订阅自己的频道前先声明它, 否则订阅会被拒 (声明是显式动作)。
/// 广播表调用要用的取消/调度表 (阻塞形态的"能不能阻塞"判据)
///
/// 进程级静态数据, 按 IID 查一次即可; 广播不在热路径上, 不做缓存。
inline std::pair<const PluginxxCancelIface*, const PluginxxSchedulerIface*>
    broadcastCallIfaces(const PluginxxHost* host) {
    if (!host) {
        return {nullptr, nullptr};
    }
    return {
        queryInterface<PluginxxCancelIface>(host, PLUGINXX_IFACE_CANCEL),
        queryInterface<PluginxxSchedulerIface>(host, PLUGINXX_IFACE_SCHEDULER),
    };
}

/// 声明一条广播频道 (**协程形态**: 调用即提交, `co_await` 拿 `TableOpResult`)
template<typename Ctx>
inline pluginxx::TableOpTicket declarePluginBroadcast(
    const Ctx&       ctx,
    std::string_view id,
    bool             anyonePublish = false,
    bool             cacheLatest   = true
) {
    const std::string decl = "{\"id\":\"" + std::string{id} + "\",\"cache\":\""
                             + (cacheLatest ? "latest" : "none") + "\",\"publish\":\""
                             + (anyonePublish ? "anyone" : "owner") + "\"}";
    return pluginxx::tableOp(
        ctx,
        [&ctx, decl, id](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (id.empty() || !ctx.host || !ctx.iface.broadcasts
                || !ctx.iface.broadcasts->declare_broadcast) {
                return nullptr;
            }
            auto declSv = PluginStringView::from(decl.data(), decl.size());
            return ctx.iface.broadcasts->declare_broadcast(ctx.host, &declSv, cb, ud, err);
        }
    );
}

inline bool declarePluginBroadcast(
    const PluginxxHost*            host,
    const PluginxxBroadcastsIface* broadcasts,
    std::string_view               id,
    bool                           anyonePublish = false,
    bool                           cacheLatest   = true
) {
    if (!host || !broadcasts || !broadcasts->declare_broadcast || id.empty()) {
        return false;
    }
    const std::string decl = "{\"id\":\"" + std::string{id} + "\",\"cache\":\""
                             + (cacheLatest ? "latest" : "none") + "\",\"publish\":\""
                             + (anyonePublish ? "anyone" : "owner") + "\"}";
    const auto declSv = PluginStringView::from(decl.data(), decl.size());
    const auto caps   = broadcastCallIfaces(host);
    // 阻塞形态: 只在 create 或不跑插件代码的线程上可用 (插件执行线程上会被拒绝,
    // 见 pluginxx/api/sync_bridge.h); 需要提交即返回时用 *NoWait 形态
    return pluginxx::tableOpSyncWait(
               host,
               caps.first,
               caps.second,
               [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                   return broadcasts->declare_broadcast(host, &declSv, cb, ud, err);
               }
    )
        .ok();
}

/// 发布一条广播 (频道不存在时自动声明; 失败只记日志并返回 false)
///
/// - `anyonePublish` 为 true 时声明成"任何人都能发" (跨端共享频道用)
inline bool publishPluginBroadcast(
    const PluginxxHost*            host,
    const PluginxxBroadcastsIface* broadcasts,
    std::string_view               id,
    std::string_view               payload,
    bool                           anyonePublish = false
) {
    if (!host || !broadcasts || !broadcasts->publish_broadcast || id.empty()) {
        return false;
    }
    const auto idSv = PluginStringView::from(id.data(), id.size());
    // 先声明 (已声明时主程序按幂等处理); 不关心结果 —— 应用频道不能改声明
    declarePluginBroadcast(host, broadcasts, id, anyonePublish);
    const auto payloadSv = PluginStringView::from(payload.data(), payload.size());
    const auto caps      = broadcastCallIfaces(host);
    return pluginxx::tableOpSyncWait(
               host,
               caps.first,
               caps.second,
               [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                   return broadcasts->publish_broadcast(host, &idSv, &payloadSv, cb, ud, err);
               }
    )
        .ok();
}

/// 撤销一条广播订阅 (异步表的阻塞形态; 幂等)
///
/// `sub` 来自 [subscribePluginBroadcast]; 主程序在实例卸载时也会一并回收。
inline bool unsubscribePluginBroadcast(
    const PluginxxHost*            host,
    const PluginxxBroadcastsIface* broadcasts,
    PluginxxSubscription*          sub
) {
    if (!host || !broadcasts || !broadcasts->unsubscribe_broadcast || sub == nullptr) {
        return false;
    }
    const auto caps = broadcastCallIfaces(host);
    return pluginxx::tableOpSyncWait(
               host,
               caps.first,
               caps.second,
               [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                   return broadcasts->unsubscribe_broadcast(host, sub, cb, ud, err);
               }
    )
        .ok();
}

/// 订阅一条广播 (返回句柄; 主程序在实例卸载时一并回收)
inline PluginxxSubscription* subscribePluginBroadcast(
    const PluginxxHost*            host,
    const PluginxxBroadcastsIface* broadcasts,
    const PluginxxScopeRef*        scope,
    std::string_view               id,
    void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*),
    void* ud
) {
    if (!host || !broadcasts || !broadcasts->subscribe_broadcast || id.empty() || !handler) {
        return nullptr;
    }
    const auto idSv = PluginStringView::from(id.data(), id.size());
    const auto caps = broadcastCallIfaces(host);
    auto       op   = pluginxx::tableOpSyncWait(
        host,
        caps.first,
        caps.second,
        [&](PluginxxOperatorCallback cb, void* opUd, PluginxxString* err) {
            return broadcasts->subscribe_broadcast(host, scope, &idSv, handler, ud, nullptr, cb,
                                                   opUd, err);
        }
    );
    // 表成员是异步契约: 订阅句柄以十进制文本放在 `payload` 里交回
    return op.ok() ? pluginxx::pluginxxSubscriptionFromText(op.text()) : nullptr;
}

/* ---- 提交即返回形态 ---- *
 *
 * 只发起、不等结果: 调用即返回, 任何线程都不等, 也不需要协程。适用于只有副作用、
 * 不需要句柄/成败的广播动作 (发一条状态、订阅一条频道、退订一条频道)。
 * 订阅与声明失败时主程序侧会记日志; 订阅句柄由主程序随实例卸载一并回收, 因此
 * 不保存句柄也不会泄漏。
 */

/// 声明一条广播频道 (**提交即返回**)
template<typename IdView>
inline bool declarePluginBroadcastNoWait(
    const PluginxxHost*            host,
    const PluginxxBroadcastsIface* broadcasts,
    const IdView&                  idView,
    bool                           anyonePublish = false,
    bool                           cacheLatest   = true
) {
    const std::string id   = std::string{detail::textOfView(idView)};
    if (!host || !broadcasts || !broadcasts->declare_broadcast || id.empty()) {
        return false;
    }
    const std::string decl = "{\"id\":\"" + id + "\",\"cache\":\""
                             + (cacheLatest ? "latest" : "none") + "\",\"publish\":\""
                             + (anyonePublish ? "anyone" : "owner") + "\"}";
    auto              declSv = PluginStringView::from(decl.data(), decl.size());
    PluginxxString    err{nullptr, 0};
    auto*             handle = broadcasts->declare_broadcast(host, &declSv, nullptr, nullptr, &err);
    if (err.data != nullptr) {
        PluginString::free(host, &err);
    }
    return handle != nullptr;
}

/// 发布一条广播 (**提交即返回**; 频道不存在时先声明)
template<typename IdView, typename PayloadView>
inline bool publishPluginBroadcastNoWait(
    const PluginxxHost*            host,
    const PluginxxBroadcastsIface* broadcasts,
    const IdView&                  idView,
    const PayloadView&             payloadView,
    bool                           anyonePublish = false
) {
    const std::string id       = std::string{detail::textOfView(idView)};
    const std::string payload  = std::string{detail::textOfView(payloadView)};
    if (!host || !broadcasts || !broadcasts->publish_broadcast || id.empty()) {
        return false;
    }
    declarePluginBroadcastNoWait(host, broadcasts, id, anyonePublish);
    auto           idSv      = PluginStringView::from(id.data(), id.size());
    auto           payloadSv = PluginStringView::from(payload.data(), payload.size());
    PluginxxString err{nullptr, 0};
    auto*          handle = broadcasts->publish_broadcast(host, &idSv, &payloadSv, nullptr, nullptr, &err);
    if (err.data != nullptr) {
        PluginString::free(host, &err);
    }
    return handle != nullptr;
}

/// 订阅一条广播 (**提交即返回**; 句柄由主程序随实例卸载回收)
template<typename IdView>
inline bool subscribePluginBroadcastNoWait(
    const PluginxxHost*            host,
    const PluginxxBroadcastsIface* broadcasts,
    const PluginxxScopeRef*        scope,
    const IdView&                  idView,
    void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*),
    void* ud
) {
    const std::string id = std::string{detail::textOfView(idView)};
    if (!host || !broadcasts || !broadcasts->subscribe_broadcast || id.empty() || !handler) {
        return false;
    }
    auto           idSv = PluginStringView::from(id.data(), id.size());
    PluginxxString err{nullptr, 0};
    auto*          handle
        = broadcasts->subscribe_broadcast(host, scope, &idSv, handler, ud, nullptr, nullptr, nullptr, &err);
    if (err.data != nullptr) {
        PluginString::free(host, &err);
    }
    return handle != nullptr;
}

/// 撤销一条广播订阅 (**提交即返回**; 幂等)
inline bool unsubscribePluginBroadcastNoWait(
    const PluginxxHost*            host,
    const PluginxxBroadcastsIface* broadcasts,
    PluginxxSubscription*          sub
) {
    if (!host || !broadcasts || !broadcasts->unsubscribe_broadcast || sub == nullptr) {
        return false;
    }
    PluginxxString err{nullptr, 0};
    auto*          handle = broadcasts->unsubscribe_broadcast(host, sub, nullptr, nullptr, &err);
    if (err.data != nullptr) {
        PluginString::free(host, &err);
    }
    return handle != nullptr;
}

/// 发布一条广播 (**协程形态**: 调用即提交, 发布前先声明频道)
///
/// 结果的 `.ok()` 才是"主程序受理并执行完成"; `publish` 的失败只对调用方有意义
/// (日志由插件自己打), 主程序不会因为发布失败而中断会话。
template<typename Ctx>
inline pluginxx::TableOpTicket publishPluginBroadcast(
    const Ctx&       ctx,
    std::string_view id,
    std::string_view payload,
    bool             anyonePublish = false
) {
    // 声明本身也是提交即返回, 不等结果 (已声明时主程序按幂等处理)
    declarePluginBroadcast(ctx, id, anyonePublish).forget();
    return pluginxx::tableOp(
        ctx,
        [&ctx, id, payload](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (id.empty() || !ctx.host || !ctx.iface.broadcasts
                || !ctx.iface.broadcasts->publish_broadcast) {
                return nullptr;
            }
            auto idSv      = PluginStringView::from(id.data(), id.size());
            auto payloadSv = PluginStringView::from(payload.data(), payload.size());
            return ctx.iface.broadcasts->publish_broadcast(
                ctx.host,
                &idSv,
                &payloadSv,
                cb,
                ud,
                err
            );
        }
    );
}

/// 撤销一条广播订阅 (**协程形态**; 幂等)
template<typename Ctx>
inline pluginxx::TableOpTicket unsubscribePluginBroadcast(
    const Ctx&            ctx,
    PluginxxSubscription* sub
) {
    return pluginxx::tableOp(
        ctx,
        [&ctx, sub](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (sub == nullptr || !ctx.host || !ctx.iface.broadcasts
                || !ctx.iface.broadcasts->unsubscribe_broadcast) {
                return nullptr;
            }
            return ctx.iface.broadcasts->unsubscribe_broadcast(ctx.host, sub, cb, ud, err);
        }
    );
}

/// 订阅一条广播 (**协程形态**: `co_await` 之后用 [subscriptionOf] 取句柄)
template<typename Ctx>
inline pluginxx::TableOpTicket subscribePluginBroadcast(
    const Ctx&             ctx,
    const PluginxxScopeRef* scope,
    std::string_view        id,
    void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*),
    void* ud
) {
    return pluginxx::tableOp(
        ctx,
        [&ctx, scope, id, handler, ud](PluginxxOperatorCallback cb,
                                       void*                    opUd,
                                       PluginxxString*          err)
            -> PluginxxOperatorHandle* {
            if (id.empty() || !handler || !ctx.host || !ctx.iface.broadcasts
                || !ctx.iface.broadcasts->subscribe_broadcast) {
                return nullptr;
            }
            auto idSv = PluginStringView::from(id.data(), id.size());
            return ctx.iface.broadcasts->subscribe_broadcast(
                ctx.host,
                scope,
                &idSv,
                handler,
                ud,
                nullptr,
                cb,
                opUd,
                err
            );
        }
    );
}

/// 订阅句柄从表结果里取出来 (失败返回 NULL; 配合 [subscribePluginBroadcast] 的协程形态)
inline PluginxxSubscription* subscriptionOf(const pluginxx::TableOpResult& result) {
    return result.ok() ? pluginxx::pluginxxSubscriptionFromText(result.text()) : nullptr;
}

/// 订阅一条广播 (扩展形态: 带频道结束回调与选项) — **协程形态**
///
/// - `optionsJson` 形如 `{"intervalMs":100,"mode":"coalesce"}`, 空串 = 全用默认值;
/// - `onEnd` 可选: 频道被结束 (或声明撤销) 时回调一次;
/// - `co_await` 之后用 [subscriptionOf] 取句柄。
template<typename Ctx>
inline pluginxx::TableOpTicket subscribePluginBroadcastEx(
    const Ctx&             ctx,
    const PluginxxScopeRef* scope,
    std::string_view        id,
    void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*),
    void(PLUGINXX_CALL* onEnd)(const PluginxxStringView*, void*),
    void*            ud,
    std::string_view optionsJson = {}
) {
    auto options = std::string{optionsJson};
    return pluginxx::tableOp(
        ctx,
        [&ctx, scope, id, handler, onEnd, ud, options](PluginxxOperatorCallback cb,
                                                      void*                    opUd,
                                                      PluginxxString*          err)
            -> PluginxxOperatorHandle* {
            if (id.empty() || !handler || !ctx.host || !ctx.iface.broadcasts
                || !ctx.iface.broadcasts->subscribe_broadcast_ex) {
                return nullptr;
            }
            auto idSv = PluginStringView::from(id.data(), id.size());
            auto optSv = options.empty()
                             ? PluginxxStringView{nullptr, 0}
                             : PluginStringView::from(options.data(), options.size());
            return ctx.iface.broadcasts->subscribe_broadcast_ex(
                ctx.host,
                scope,
                &idSv,
                handler,
                onEnd,
                ud,
                &optSv,
                cb,
                opUd,
                err
            );
        }
    );
}

/// 发布一条广播 (ABI 视图形态; 与 `string_view` 版同义)
///
/// `id` / `payload` 为跨边界视图 (插件里常见的是 `PluginStringView::fromCstr` 的产物),
/// 内部零拷贝转成 `string_view`。
inline bool publishPluginBroadcast(
    const PluginxxHost*            host,
    const PluginxxBroadcastsIface* broadcasts,
    const PluginxxStringView&      id,
    const PluginxxStringView&      payload,
    bool                           anyonePublish = false
) {
    return publishPluginBroadcast(
        host,
        broadcasts,
        PluginStringView::str(id),
        PluginStringView::str(payload),
        anyonePublish
    );
}

/// 订阅一条广播 (ABI 视图形态; 与 `string_view` 版同义)
inline PluginxxSubscription* subscribePluginBroadcast(
    const PluginxxHost*            host,
    const PluginxxBroadcastsIface* broadcasts,
    const PluginxxScopeRef*        scope,
    const PluginxxStringView&      id,
    void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*),
    void* ud
) {
    return subscribePluginBroadcast(host, broadcasts, scope, PluginStringView::str(id), handler, ud);
}

/* ==================== 网络便捷入口 (插件侧) ====================
 *
 * 插件不自己建连接: HTTP / TCP / WebSocket / UDP 一律经主程序的 `<应用名>.net` 表
 * (实现在 `utilxx::NetHub`, 按插件实例划分资源与生命周期)。本节的入口只做三件事:
 * 拼参数 JSON、调表、把结果包成 [NetResult]。
 *
 * - **一次操作一个入口**: [netCall] 提交后返回 awaiter, `co_await` 拿结果
 *   (表入口本身是异步操作契约, 没有同步版本);
 * - **提交完不用等** (关连接 / 心跳 / 应答这类动作): [netSubmit] —— 只提交, 不回收结果,
 *   所以非协程位置 (析构、回调) 也能用;
 * - 包装: [netFetch] / [netDownload] / [netWsConnect] / [netWsSend] / [netHttpServe] /
 *   [netHttpRespond] / [netInfo];
 * - **事件**: 频道 `plugin.<插件 id>.net` (见 [netEventTopic]), 由主程序声明并代发,
 *   插件订阅即可 —— 载荷里带 `kind` 字段 (`ws.open` / `ws.message` / `http.request` …)。
 *
 * 两层状态不要混: [NetResult::code] 是表入口的结果码 (参数 / 状态层面被拒, 取值见
 * `pluginxx/api/result.h` 的通用段), 结果 JSON 里的 `ok` 才是业务结论 —— 连不上、超时
 * 这类失败是 `code == PLUGINXX_RESULT_OK` 而 `ok == false`。
 */

/// 一次网络调用的结果
struct NetResult {
    int32_t     code = PLUGINXX_RESULT_ERR_STATE; ///< 表入口结果码 (`PLUGINXX_RESULT_*` 通用段)
    std::string json;                             ///< 结果 JSON (受理失败时可能为空)

    /// 操作执行完毕 (业务结论还要看 [ok])
    bool accepted() const noexcept {
        return code == PLUGINXX_RESULT_OK;
    }

    /// 业务成功 (= 受理成功且结果 JSON 里 `ok:true`)
    bool ok() const;

    /// 结果 JSON 里的字符串字段 (不存在 / 不是对象时返回空串)
    std::string str(std::string_view key) const;

    /// 结果 JSON 里的 `status` (HTTP 状态码; 没有就返回 0)
    int64_t status() const;

    /// 失败原因: 表入口失败给结果码名字, 业务失败给 `error: message`
    std::string error() const;
};

/* ==================== 插件框架 detail 名的引入 (pluginxx/kit/kit.h) ====================
 *
 * 业务 helper (tool 的协程体 / graph node / call_tool awaiter) 需要插件框架协程设施的
 * detail 类型与函数; 同样以 using 声明引入, 类型本体仍在 pluginxx::detail 一份。
 */
namespace detail {

using pluginxx::detail::advanceRootOnce;
using pluginxx::detail::autoStopSpawns;
using pluginxx::detail::AwaiterState;
using pluginxx::detail::BridgeRoot;
using pluginxx::detail::callLifecycleEntry;
using pluginxx::detail::callLifecycleEntryVoid;
using pluginxx::detail::CompletionGuard;
using pluginxx::detail::createInstanceEntry;
using pluginxx::detail::destroyInstanceEntry;
using pluginxx::detail::destroyBridgeFrame;
using pluginxx::detail::finishIfDone;
using pluginxx::detail::invokeCap;
using pluginxx::detail::InvokeCapAwaiter;
using pluginxx::detail::InvokeCapState;
using pluginxx::detail::jsonGet;
using pluginxx::detail::notifyLifecycleFailure;
using pluginxx::detail::OffloadState;
using pluginxx::detail::OffloadTicket;
using pluginxx::detail::PolledRoot;
using pluginxx::detail::PollOneBridge;
using pluginxx::detail::PromiseBase;
using pluginxx::detail::resumePluginCoroutine;
using pluginxx::detail::RootRequest;
using pluginxx::detail::SleepState;
using pluginxx::detail::SleepTicket;
using pluginxx::detail::spawnTaskImpl;
using pluginxx::detail::startBridgedRoot;
using pluginxx::detail::YieldAwaiter;

/// 手写 create 入口的公共实现: 构造上下文 (**可重入**) → 业务初始化 → 注册事务
///
/// 少数插件需要自己控制上下文构造 (按平台建运行时、装日志 sink、查必需接口),
/// 于是手写 `create` 入口。本函数把"可重入 / 异常兜底 / 失败通知"这三件重复事收在一处:
///
/// ```cpp
/// extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_create(
///     const PluginxxHost* host, void** plugin_ctx,
///     const PluginxxOperatorNotify* notify, PluginxxString* err) {
///     agentxx::plugin::detail::createCtxEntry<MyCtx>(
///         host, plugin_ctx, notify, err, "my_plugin",
///         [](MyCtx& ctx) -> std::string {
///             ctx.extra = ...;                  // 构造阶段的事（可失败）
///             return checkOk(ctx) ? "" : "missing required interface";
///         },
///         myStart);                              // void(MyCtx&, notify, err)
/// }
/// ```
///
/// - `*plugin_ctx` 非空 = 同一实例第二次 create (禁用后启用): 不重建上下文,
///   跳过 `check` 与业务初始化, 只跑注册事务;
/// - 首次构造的同步阶段只做"绑定 host/iface" (`attachHost`, `spawn` 依赖它)、写出
///   `*plugin_ctx` 与 `check`; `onHostReady` (要调表的业务初始化) 与注册事务进后台任务,
///   完成后经 `notify` 恰好一次交回 —— 插件执行线程上不能同步等表;
/// - `check` 返回非空串 = 构造失败; 上下文仍写回 `*plugin_ctx` (主程序随后会调
///   destroy 收尾), 完成通知已发 FAILED;
/// - `create` 为空 = 只构造 (没有注册事务);
/// - 主机没有 `pluginxx.tasks` 表时 `spawn` 不可用, 退回旧路径 (跳过 `onHostReady`, 记警告)。
template<typename CtxType, typename CheckFn, typename CreateFn>
inline void createCtxEntry(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err,
    std::string_view              name,
    CheckFn&&                     check,
    CreateFn&&                    create
) noexcept {
    if (!plugin_ctx) {
        notifyLifecycleFailure(host, notify, err, "plugin create: null plugin context");
        return;
    }
    if (!notify || !notify->done) {
        PluginString::set(host, err, "plugin create: null completion notify");
        return;
    }
    auto*      ctx   = static_cast<CtxType*>(*plugin_ctx);
    const bool fresh = (ctx == nullptr);
    if (fresh) {
        try {
            auto instance = std::make_unique<CtxType>();
            if constexpr (pluginxx::detail::KitInstanceCtx<CtxType>) {
                instance->attachHost(host);
            } else {
                // 自定义上下文: 自己绑定 host / 接口表 (旧形态)
                instance->init(host);
            }
            ctx         = instance.release();
            *plugin_ctx = ctx;
        } catch (const std::exception& e) {
            notifyLifecycleFailure(
                host,
                notify,
                err,
                fmt::format("plugin create {}: {}", name, e.what())
            );
            return;
        } catch (...) {
            notifyLifecycleFailure(host, notify, err, "plugin create: unknown exception");
            return;
        }
        // 构造阶段的插件自有检查 (同步; 只查接口表指针, 不调表)
        try {
            std::string reason = check(*ctx);
            if (!reason.empty()) {
                notifyLifecycleFailure(host, notify, err, reason);
                return;
            }
        } catch (const std::exception& e) {
            notifyLifecycleFailure(host, notify, err, e.what());
            return;
        } catch (...) {
            notifyLifecycleFailure(host, notify, err, "plugin create: unknown exception");
            return;
        }
    }
    // 注册事务的调用形态收进一个助手: CreateFn 可能是 nullptr / 函数指针 / 可调用对象
    using CreateType = std::decay_t<CreateFn>;
    auto runCreate
        = [](CreateType& fn, CtxType& instance, const PluginxxOperatorNotify* nf, PluginxxString* e) {
              if constexpr (std::is_same_v<CreateType, std::nullptr_t>) {
                  (void)fn;
                  (void)instance;
                  (void)e;
                  nf->done(nf->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
              } else {
                  if constexpr (std::is_pointer_v<CreateType>) {
                      if (!fn) {
                          (void)instance;
                          (void)e;
                          nf->done(nf->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
                          return;
                      }
                  }
                  fn(instance, nf, e);
              }
          };
    // 首次构造: 业务初始化是协程 (要调表), 与注册事务一起进后台任务。
    // 闭包里的 create 必须按值持有: 它原本是本函数的转发引用, 调用方 (C 入口) 返回即失效。
    if constexpr (pluginxx::detail::KitInstanceCtx<CtxType>) {
        if (fresh && ctx->iface.tasks && ctx->iface.tasks->register_task) {
            const PluginxxOperatorNotify done = *notify; // 借用指针: 按值持有到任务结束
            CreateType createCopy(std::forward<CreateFn>(create));
            ctx->spawn([host, done, createCopy = std::move(createCopy), runCreate](
                           CtxType& instance, OpCtl&) mutable -> Task<void> {
                try {
                    co_await instance.onHostReady();
                    // 注册事务可能在内部发起异步工作 (finishLater), 自己完成通知;
                    // 这里只负责把同步写出的错误串释放掉
                    PluginxxString localError{nullptr, 0};
                    runCreate(createCopy, instance, &done, &localError);
                    if (localError.data) {
                        PluginString::free(host, &localError);
                    }
                } catch (const std::exception& e) {
                    notifyLifecycleFailure(host, &done, nullptr, e.what());
                } catch (...) {
                    notifyLifecycleFailure(host, &done, nullptr, "plugin create: unknown exception");
                }
                co_return;
            });
            return;
        }
        if (fresh) {
            ctx->log.warn(
                "plugin create: host has no pluginxx.tasks; business init (onHostReady) skipped"
            );
        }
    }
    callLifecycleEntryVoid(host, ctx, notify, err, "plugin create", [&]() {
        CreateType createCopy(std::forward<CreateFn>(create));
        runCreate(createCopy, *ctx, notify, err);
    });
}

/// 手写 destroy 入口的公共实现: 插件自己的释放 + 完成通知
///
/// `cleanup` 只做本地释放 (不调主程序接口、不起异步工作); 抛异常按失败通知。
template<typename CtxType, typename CleanupFn>
inline void destroyCtxEntry(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err,
    CleanupFn&&                   cleanup
) noexcept {
    auto*       ctx  = static_cast<CtxType*>(plugin_ctx);
    const auto* host = ctx ? ctx->host : nullptr;
    pluginxx::detail::destroyInstanceEntry(host, ctx, notify, err, std::forward<CleanupFn>(cleanup));
}

} // namespace detail
} // namespace plugin
} // namespace agentxx

/// 插件侧的界面命名空间: 插件源码可以书写 `agentxx::ui::...` 拿到界面描述层
/// (`Item` / `build` / `parse` / `adapt` … 名字与 `pluginxx::ui` 是同一份),
/// 以及本客户端的扩展 kit `agentxx::ui::kit`(见生成的 `agentxx_ui_kit.g.h`;
/// 它自包含基础 kit 的全部组件, 并把终端留白规则改成本客户端习惯)。
///
/// 注意: `agentxx::ui::kit` 是**扩展 kit**(直接声明), 基础 kit 仍在
/// `pluginxx::ui::kit`; 扩展 kit 里同名组件的语义与基础 kit 一致, 只是留白更紧。
namespace agentxx {
namespace ui {

using namespace ::pluginxx::ui;

} // namespace ui
} // namespace agentxx

