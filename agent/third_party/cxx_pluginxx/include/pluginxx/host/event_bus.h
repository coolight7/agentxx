/// pluginxx 通用事件表的事件后端与订阅句柄 (宿主侧, 与宿主领域无关)
///
/// 内容:
/// - [EventSource]: 宿主事件总线的最小抽象 (订阅 / 撤销 / 发布) —— 宿主用自己已有的
///   事件系统实现它 (agentxx 侧见 `agentxx/plugin/plugin_event_source.h`);
/// - [AgentxxPluginSubscription]: 事件订阅句柄的**实现体** (C ABI 中作为不透明指针
///   `AgentxxPluginSubscription*` 传递, 因此名字保持 ABI 冻结时的形态);
/// - [unsubscribePluginSubscription]: 幂等撤销 (任意线程; 宿主侧簿记在 IO 线程执行)。
///
/// 线程约定:
/// - 订阅登记 / 撤销簿记只在宿主 IO 线程执行 (与其它注册事务同一串行上下文);
/// - `alive` 为原子标志: 撤销返回后事件线程据此立即短路, 不再回调插件;
/// - 事件回调本身经实例执行 lease 保护 (见 [PluginHostCore::subscribe])。
#pragma once

#include "pluginxx/api/abi.h"
#include "pluginxx/host/abi_util.h"
#include "pluginxx/runtime/runtime.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace pluginxx {

class PluginInstanceBase;

/// 宿主事件源 (通用事件表的事件后端)
///
/// 主题的命名空间补齐由 [DomainHooks::qualifyEventTopic] 完成, 本接口收到的
/// 已经是最终主题, 实现方只负责"把主题映射到自己的事件系统"。
class EventSource {
public:

    virtual ~EventSource() = default;

    /// 订阅主题
    /// - handler 由宿主在其事件线程调用, **不得抛异常** (抛出的异常会被短路成日志)
    /// - `return` 订阅句柄 (0 表示失败; 0 不会交给 [unsubscribe])
    virtual size_t subscribe(std::string_view topic, std::function<void(std::string_view)> handler)
        = 0;

    /// 撤销订阅 (幂等); topic 与 subscriptionId 须与订阅时一致
    virtual void unsubscribe(std::string_view topic, size_t subscriptionId) = 0;

    /// 发布事件 (0 成功, 非 0 失败); 实现方自行决定同步还是异步发布
    virtual int publish(std::string_view topic, std::string_view eventJson) = 0;
};

} // namespace pluginxx

/// 事件订阅句柄的实现体 (C ABI 不透明句柄 `AgentxxPluginSubscription*` 的实际类型)。
///
/// 归属 cxx_pluginxx: 事件表是通用表, 句柄字段因此只依赖内核类型。
struct AgentxxPluginSubscription {
    /// 事件后端 (被本句柄持有, 保证撤销动作执行时后端仍然有效)
    std::shared_ptr<pluginxx::EventSource> source;
    /// 已补齐命名空间的最终主题
    std::string topic;
    /// 事件后端的订阅句柄 (0 = 未登记或已撤销)
    size_t subscriptionId = 0;
    /// 订阅者实例 (弱引用: 句柄不延长实例生命周期)
    std::weak_ptr<pluginxx::PluginInstanceBase> inst;
    /// 宿主运行时 (撤销时把簿记投递到 IO 线程; 弱引用不延长其生命周期)
    std::weak_ptr<pluginxx::PluginRuntime> runtime;
    /// 插件事件回调 (C ABI 函数指针)
    void(AGENTXX_PLUGIN_CALL* handler)(const AgentxxPluginStringView* event_json, void* ud)
        = nullptr;
    void* ud = nullptr;
    /// 是否仍然有效: 撤销时先置 false, 事件线程据此立即短路
    std::atomic<bool> alive{true};
};

namespace pluginxx {
namespace detail {

/// 撤销簿记的具体动作 (**须在宿主 IO 线程执行**): 撤销后端订阅并移出实例订阅表。
/// - 幂等: `subscriptionId` 置 0 后重复调用是空操作;
/// - 异常不外抛 (撤销发生在实例关闭路径, 不能因为后端报错而留下悬挂登记)。
///
/// 定义放在 `pluginxx/runtime/instance_base.h` (那里 [PluginInstanceBase] 已完整):
/// 本头只前置声明实例类型, 不能在这里访问其成员。
void revokeSubscription(AgentxxPluginSubscription* sub) noexcept;

} // namespace detail

/// 撤销事件订阅 (**幂等**, 任意线程可调用)
///
/// 步骤:
/// 1. 原子置 `alive=false`: 之后到达的事件立即不再回调插件, 不必等待簿记完成;
/// 2. 在宿主 IO 线程撤销后端订阅并移出实例的订阅表 (非 IO 线程时同步等待投递完成,
///    与其它 vtable 入口的投递语义一致);
/// 3. 运行时不可投递 (io executor 缺失/已停止) 时就地完成簿记 —— 此时实例正在
///    关闭, 不再有并发访问实例订阅表的执行体。
///
/// 句柄本体的保活由实例的 `subscriptionHandles` 负责 (随实例析构释放), 因此插件在
/// 撤销之后再传同一裸指针进来也只会命中 [detail::revokeSubscription] 的空操作分支。
inline void unsubscribePluginSubscription(AgentxxPluginSubscription* sub) {
    if (!sub) {
        return;
    }
    bool expected = true;
    if (!sub->alive.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return; // 已撤销: 幂等空操作
    }

    auto runtime = sub->runtime.lock();
    if (!runtime || isRuntimeIoThread(runtime) || runtimeExecutorStopped(runtime->executor)) {
        detail::revokeSubscription(sub);
        return;
    }

    auto       done   = std::make_shared<std::promise<void>>();
    auto       fut    = done->get_future();
    const bool queued = enqueueRuntimeAction(
        runtime,
        [runtime, sub, done]() {
            runtime->ioThreadId.store(std::this_thread::get_id(), std::memory_order_release);
            detail::revokeSubscription(sub);
            done->set_value();
        },
        false
    );
    if (!queued) {
        detail::revokeSubscription(sub);
        return;
    }
    fut.wait();
}

} // namespace pluginxx
