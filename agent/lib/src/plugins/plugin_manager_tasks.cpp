/// 后台任务与工具/能力调用共用 Operation 生命周期和 exactly-once 完成协议。
#include "agentxx/plugin/op_driver.h"

namespace agentxx::plugin {

AgentxxPluginOperatorHandle* PluginManager::registerTask(
    PluginInstance*                     inst,
    AgentxxPluginOperatorCancelFunction cancel_fn,
    void*                               cancel_ud,
    AgentxxPluginOperatorNotify*        notify,
    AgentxxPluginString*                error_out
) {
    if (notify) {
        *notify = {};
    }
    std::shared_ptr<OpCore> core;
    try {
        if (!inst || !notify || !isIoThread() || !acceptsRegistration(inst)) {
            throw std::runtime_error("register_task: missing instance/notify or wrong IO thread");
        }
        core = OpCore::create(runtime(), inst->self.lock(), nullptr, "background task");
        core->accept([cancel_fn, cancel_ud] {
            if (cancel_fn) {
                cancel_fn(cancel_ud, nullptr);
            }
        });
        *notify = core->notify();
        return core->handle();
    } catch (const std::exception& e) {
        if (core) {
            core->reject();
        }
        hostMemorySetString(error_out, e.what());
        return nullptr;
    }
}

} // namespace agentxx::plugin
