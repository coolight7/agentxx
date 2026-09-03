// plugin_manager_tasks.cpp —— 插件后台任务 (agentxx.agent.tasks 接口表) 宿主侧实现
//
// 背景: kit (plugin_kit.h) 的 spawn 让插件启动后台协作任务 (如周期采集:
// while(!cancelled()) { offload; sleep; })。旧设计中 spawn 游离在宿主管理
// 之外 (状态全在 DSO 内 spawns_/cancelFlag, 宿主只有不透明 plugin_ctx) ——
// 卸载时无人取消 → 协程帧悬挂泄漏; 若协程挂起时直接 delete ctx/dlclose,
// 恢复执行还会访问已销毁上下文 → UAF。
//
// 本文件把 spawn 纳入与工具/能力 op 同构的宿主任务管理:
// - 宿主登记: registerTask 把句柄推入实例 outstandingOps (与工具 op 同列表,
//   detachAll 统一取消)
// - 存活标记: InflightGuard 挂在 OpCore 上 (notify 到达才 reset) ——
//   waitInflightZero 会精确等到 spawn 协程退出, 不再需要猜测式 drain
// - 完成通知: 插件协程结束 (帧销毁后) 经 notify.done 恰好一次上报 →
//   OpCore::onDone: 原子 CAS + guard.reset + doneSignal.emit → 回收协程
//   (spawnHandleReaper) 把句柄从 outstandingOps 移除
//
// 线程约束 (与既有 ABI 契约一致):
// - register_task / cancel_task 仅 io 线程调用 (vtable 适配层对非 io 线程经
//   ioCallSync 投递, 插件无感)
// - notify.done 可从插件任意线程回调 (OpCore::onDone 无锁 CAS + chan +
//   post 回 io 派发, 为任意线程而设计); kit 协程完成路径 (finishIfDone 调用
//   点) 恒在 io 线程 —— 两者是不同路径, 不构成竞态前提
#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/plugin/op_driver.h" // spawnHandleReaper / OpCore (op_driver.h 自带所需 asio 头)
#include "agentxx/util/log.h"

#include <cstring>
#include <memory>

namespace agentxx {
namespace plugin {

// 本地错误串写入助手 (与 plugin_manager_capability.cpp 的 setErrOut 同构;
// 跨 TU 静态函数不可见, 故各自持一份)
static void setTaskErrOut(PluginInstance* inst, char** error_out, const std::string& msg) {
    if (!error_out || *error_out) {
        return;
    }
    if (inst && inst->host.vtable && inst->host.vtable->strdup) {
        *error_out = inst->host.vtable->strdup(strToSv(msg));
        return;
    }
    auto* p = static_cast<char*>(::malloc(msg.size() + 1));
    if (p) {
        std::memcpy(p, msg.c_str(), msg.size() + 1);
    }
    *error_out = p;
}

AgentxxPluginOperatorHandle* PluginManager::registerTask(
    PluginInstance*                     inst,
    AgentxxPluginOperatorCancelFunction cancel_fn,
    void*                               cancel_ud,
    AgentxxPluginOperatorNotify*        notify,
    char**                              error_out
) {
    auto setErr = [&](const std::string& msg) {
        setTaskErrOut(inst, error_out, msg);
    };
    if (!inst) {
        setErr("register_task: plugin instance released");
        return nullptr;
    }
    if (!ioExecutor_) {
        setErr("register_task: io executor not ready");
        return nullptr;
    }
    if (!notify) {
        setErr("register_task: notify out param is null");
        return nullptr;
    }
    // 宿主约定: 本函数 io 线程调用 (vtable 适配层已投递); 防御性校验
    if (!isIoThread()) {
        setErr("register_task: must be called on io thread (or via ioCallSync)");
        return nullptr;
    }

    // 1. 句柄登记 (与工具 op 同列表, detachAll 统一取消; 生命周期由清理协程
    //    托管 —— 清理协程持 handle shared_ptr, detachAll 只丢列表不销毁)
    auto handle = std::make_shared<AgentxxPluginOperatorHandle>();
    handle->caller = inst;
    inst->outstandingOps.push_back(handle);

    // 2. 任务存活期间持 inflight —— waitInflightZero 会等到任务结束
    //    (guard 以 shared_ptr 挂在 core 上, notify 到达才 reset —— 见 OpCore::onDone)
    auto guard = std::make_shared<PluginInstance::InflightGuard>(inst);
    auto core  = std::make_shared<OpCore>(ioExecutor_, guard);

    // 3. 宿主侧等待 done 的清理协程: done 到达 → 从调用方 outstandingOps erase
    detail::spawnHandleReaper(ioExecutor_, core, inst->self, handle);

    // 4. 宿主侧取消回调 → 转发给插件的 cancel_fn (协作式; 幂等由
    //    AgentxxPluginOperatorHandle::cancelled CAS + OpCore::cancelSent 保证)
    handle->cancelFn = [inst, cancel_fn, cancel_ud]() {
        if (cancel_fn) {
            try {
                cancel_fn(cancel_ud, /*op=*/nullptr);
            } catch (const std::exception& e) {
                XX_LOGW("Plugin `{}` task cancel_fn threw: {}", inst->name, e.what());
            } catch (...) {
                XX_LOGW("Plugin `{}` task cancel_fn threw unknown exception", inst->name);
            }
        }
    };

    // notify 为出参: 插件协程结束 (帧销毁后) 经 notify.done 恰好一次上报 →
    // OpCore::onDone (notified 原子 CAS) → guard.reset (inflight-1) +
    // doneSignal.emit → 清理协程把 handle 从 outstandingOps 移除
    *notify = core->notify();
    return handle.get();
}

} // namespace plugin
} // namespace agentxx
