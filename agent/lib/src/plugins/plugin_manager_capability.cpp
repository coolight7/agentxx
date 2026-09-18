/// 插件工具调用 (agentxx.agent.tools 表的 call_tool_async 实现)
///
/// 通用表 (能力/事件/调度/任务/协程驱动/日志/JSON/配置/插件互查/取消) 的实现已
/// 下沉到 cxx_pluginxx (见 pluginxx/host/host_core.h 与 tables_impl.h); 本文件只保留
/// 领域部分: 工具调用与工具注册表/实例状态的耦合 (工具表是 agent 领域表)。
#include "agentxx/plugin/plugin_manager.h"

#include "asio/post.hpp"
#include "fmt/format.h"
#include "pluginxx/runtime/op_driver.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <cstring>

namespace agentxx {
namespace plugin {

namespace {

/// 写 C ABI 出参错误串: 优先经实例的宿主视图分配 (与插件侧释放路径一致),
/// 失败时回退宿主堆内存
void setErrOut(PluginInstance* caller, AgentxxPluginString* error_out, const std::string& msg) {
    if (!error_out || error_out->data) {
        return;
    }
    const AgentxxPluginHost* host = caller ? caller->hostView() : nullptr;
    *error_out                    = agentxx::plugin::PluginString::from(host, strToSv(msg));
    if (!error_out->data) {
        auto* p = static_cast<char*>(hostMemoryAlloc(msg.size() + 1));
        if (p) {
            std::memcpy(p, msg.c_str(), msg.size() + 1);
            error_out->data = p;
            error_out->size = msg.size();
        }
    }
}

} // namespace

AgentxxPluginOperatorHandle* PluginManager::callToolAsync(
    PluginInstance*               caller,
    AgentxxPluginStringView       name,
    AgentxxPluginStringView       args_json,
    AgentxxPluginStringView       thread_id,
    AgentxxPluginOperatorCallback cb,
    void*                         ud,
    AgentxxPluginString*          error_out
) {
    // 入参在当前调用内复制；查询、登记和完整 start 都在所属 IO 线程执行。
    if (!isIoThread()) {
        auto self  = shared_from_this();
        auto owner = caller ? caller->self.lock() : nullptr;
        /// 排队阶段也保护 caller；只有持有实例对象不能阻止 ctx/dlclose。
        auto admission = std::make_shared<PluginInstanceBase::InflightGuard>(owner);
        if (!*admission) {
            hostMemorySetString(error_out, "plugin caller is closing");
            return nullptr;
        }
        const std::string toolName = svToStr(name), args = svToStr(args_json),
                          sid = svToStr(thread_id);
        return ioCallSync<AgentxxPluginOperatorHandle*>(
            this,
            [self, owner, admission, toolName, args, sid, cb, ud, error_out] {
                return self->callToolAsync(owner.get(), toolName, args, sid, cb, ud, error_out);
            }
        );
    }
    std::shared_ptr<OpCore> core;
    try {
        auto owner = caller ? caller->self.lock() : nullptr;
        if (!owner || !ioExecutor()) {
            throw std::runtime_error("call_tool_async: missing caller or IO executor");
        }
        const auto toolName = svToStr(name);
        auto       tool     = std::dynamic_pointer_cast<PluginTool>(registry_->find(toolName));
        auto       provider = tool ? tool->instance() : nullptr;
        if (!provider || !provider->enabled
            || (provider->lifetime && !provider->lifetime->acceptsOperations())) {
            throw std::runtime_error("call_tool_async: plugin tool not available: " + toolName);
        }
        const auto spec = tool->spec();
        if (!spec.execute_start) {
            throw std::runtime_error("call_tool_async: tool has no start callback");
        }
        auto parsed = PluginStringView::empty(args_json)
                          ? utilxx_base::Json::object()
                          : utilxx_base::Json::parse(PluginStringView::str(args_json));
        if (!parsed.is_object()) {
            throw std::runtime_error("call_tool_async: arguments must be a JSON object");
        }
        const auto session     = svToStr(thread_id);
        core                   = OpCore::create(runtime(), provider, owner, toolName);
        const auto callId      = fmt::format("plugin_call_{}", core->id());
        parsed["sessionId"]    = session;
        parsed["tool_call_id"] = callId;
        OpDrive drive;
        drive.start = [spec,
                       args = parsed.dump(),
                       session,
                       callId](const auto* notify, auto* error) -> void* {
            const auto a = PluginStringView::from(args), s = PluginStringView::from(session),
                       id = PluginStringView::from(callId);
            return spec.execute_start(spec.user_data, &a, &s, &id, notify, error);
        };
        drive.cancel = [spec](void* op) {
            if (spec.execute_cancel) {
                spec.execute_cancel(spec.user_data, op);
            }
        };
        std::string error;
        if (!core->start(std::move(drive), error)) {
            setErrOut(caller, error_out, error);
            return nullptr; // 真正拒绝：不调用 cb。
        }
        core->setCallback(cb, ud);
        return core->handle(); // 同步 done 也返回受管句柄，callback 恒经 IO 发布。
    } catch (const std::exception& e) {
        if (core && !core->submitted()) {
            core->reject();
        }
        setErrOut(caller, error_out, e.what());
        return nullptr;
    }
}

} // namespace plugin
} // namespace agentxx
