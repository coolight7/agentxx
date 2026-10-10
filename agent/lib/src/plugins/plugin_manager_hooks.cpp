/// 钩子处理器注册表与派发 (接口表 `agentxx.agent.hooks` / `agentxx.agent.hooks_ex` 的落地)
///
/// 一个文件承载三件事:
/// 1. **注册表**: 一个钩子点 -> 处理器列表, 条目为
///    `{句柄, 层, 归属, 优先级, 登记序号, 归属标签, 说明, 执行体}`;
///    顺序 = `plugin` 层 (插件 / FFI 宿主登记的) 按 `(priority 升序, 登记序号)`,
///    其后是 `core` 层 (库自己登记的)。不声明 `priority` (= 0) 时顺序就是登记
///    顺序 = 插件装载顺序 (与旧行为一致);
/// 2. **单派发器**: 中间件链上只挂一个 [PluginHookDispatchHandle] —— 首次登记时
///    插入、没有处理器时摘除; 派发时按注册表顺序串行执行处理器, 都在宿主 io 线程,
///    单个处理器失败只记一条警告日志并继续 (与旧实现一致);
/// 3. **清单**: [PluginManager::hooksJson] / [PluginManager::handlersOf] —— 插件
///    `list_hooks`、装配快照 `hooks` 段与 `--dump-diagnostics` 共用同一份实现;
///    派发记录只在开发者模式下收集 (见 plan §11.4)。
///
/// 线程约定: 注册表只在宿主 io 线程上读写 (注册入口由 vtable 层投递, 与工具 /
/// 功能点同一约定); 派发也在 io 线程执行。
#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/context.h"
#include "agentxx/feature/feature.h"
#include "agentxx/util/cancel_adapter.h"
#include "asio/this_coro.hpp"
#include "fmt/format.h"
#include "pluginxx/runtime/op_driver.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <chrono>

namespace agentxx {
namespace plugin {

// =====================================================================
// 点名 / 输入摘要 (清单与日志用)
// =====================================================================

namespace {

/// 钩子点名 (清单与日志用; 与 C ABI 的枚举一一对应)
const char* hookPointName(int32_t point) {
    switch (point) {
        case AGENTXX_PLUGIN_HOOK_AGENT_START:
            return "AGENT_START";
        case AGENTXX_PLUGIN_HOOK_AGENT_END:
            return "AGENT_END";
        case AGENTXX_PLUGIN_HOOK_MODEL_START:
            return "MODEL_START";
        case AGENTXX_PLUGIN_HOOK_MODEL_RUN:
            return "MODEL_RUN";
        case AGENTXX_PLUGIN_HOOK_MODEL_END:
            return "MODEL_END";
        case AGENTXX_PLUGIN_HOOK_TOOL_START:
            return "TOOL_START";
        case AGENTXX_PLUGIN_HOOK_TOOL_END:
            return "TOOL_END";
        default:
            return "UNKNOWN";
    }
}

/// 交到插件手里的输入摘要
/// - 钩子是"通知", 载荷固定为 `{sessionId, point}` (不进会话内容, 不随上下文增长)
/// - 需要更多信息的插件应由调用方另建功能点 (钩子不参与功能点实现链)
utilxx_base::Json
    summarizeNodeInput(AgentxxPluginHookPoint point, const neograph::graph::NodeInput& in) {
    utilxx_base::Json j;
    j["sessionId"] = in.ctx.thread_id;
    j["point"]     = static_cast<int>(point);
    return j;
}

/// 插件层处理器此刻是否可用 (实例还在且启用)
bool handlerEnabled(const std::weak_ptr<PluginInstance>& inst) {
    auto ptr = inst.lock();
    return ptr != nullptr && ptr->enabled;
}

/// 优先级越界裁剪到上下限并记一条警告 (不拒绝登记; 与功能点同一口径)
int32_t clampHookPriority(int32_t point, std::string_view owner, int32_t priority) {
    if (priority < agentxx::feature::kPriorityMin || priority > agentxx::feature::kPriorityMax) {
        XX_LOGW(
            "钩子点 `{}`: 处理器 `{}` 的 priority {} 越界, 已裁剪到 [{}, {}]",
            hookPointName(point),
            owner,
            priority,
            agentxx::feature::kPriorityMin,
            agentxx::feature::kPriorityMax
        );
        return std::clamp(
            priority,
            agentxx::feature::kPriorityMin,
            agentxx::feature::kPriorityMax
        );
    }
    return priority;
}

/// C ABI 视图 → std::string
std::string viewToStr(const PluginxxStringView& sv) {
    if (sv.data == nullptr || sv.size == 0) {
        return {};
    }
    return std::string{sv.data, static_cast<size_t>(sv.size)};
}

/// 交给 core 层处理器的完成通知器: 什么都不做
///
/// 钩子的结果本来就被丢弃, core 层处理器不需要经它回传值; 给一个非空实现
/// (而不是 NULL 函数指针) 是为了让直接调用 `notify->done(...)` 的处理器也安全。
void PLUGINXX_CALL ignoreHookCompletion(void*, int32_t, const PluginxxStringView*) {}

} // namespace

// =====================================================================
// 注册表
// =====================================================================

std::vector<PluginManager::HookHandlerEntry>
    PluginManager::orderedHandlers(AgentxxPluginHookPoint point) const {
    std::vector<HookHandlerEntry> out;
    out.reserve(hookHandlers_.size());
    for (const auto& entry : hookHandlers_) {
        if (entry.point == static_cast<int32_t>(point)) {
            out.push_back(entry);
        }
    }
    // 层 -> 优先级 -> 登记序号; 相等键不会出现 (同一实例同一点只覆盖/各持句柄,
    // 序号唯一), stable_sort 只是让顺序在极端情况下也确定
    std::stable_sort(
        out.begin(),
        out.end(),
        [](const HookHandlerEntry& a, const HookHandlerEntry& b) {
            if (a.coreLayer != b.coreLayer) {
                return !a.coreLayer; // plugin 层在前, core 层在后
            }
            if (a.priority != b.priority) {
                return a.priority < b.priority;
            }
            return a.seq < b.seq;
        }
    );
    return out;
}

PluginManager::HookHandlerView PluginManager::viewOf(const HookHandlerEntry& entry) const {
    HookHandlerView view;
    view.handle   = entry.handle;
    view.point    = entry.point;
    view.layer    = entry.coreLayer ? "core" : "plugin";
    view.owner    = entry.owner;
    view.ownerTag = entry.ownerTag;
    view.depict   = entry.depict;
    view.load     = entry.load;
    view.priority = entry.priority;
    view.seq      = entry.seq;
    view.enabled  = entry.coreLayer ? true : handlerEnabled(entry.inst);
    return view;
}

std::vector<PluginManager::HookHandlerView>
    PluginManager::handlersOf(AgentxxPluginHookPoint point) const {
    std::vector<HookHandlerView> out;
    for (const auto& entry : orderedHandlers(point)) {
        out.push_back(viewOf(entry));
    }
    return out;
}

size_t PluginManager::liveHookHandlersOf(const PluginInstance& inst) const {
    const std::string owner = fmt::format("plugin:{}", inst.name);
    size_t            count = 0;
    for (const auto& entry : hookHandlers_) {
        if (entry.owner == owner) {
            ++count;
        }
    }
    return count;
}

std::vector<PluginManager::HookHandlerView>
    PluginManager::allHookHandlersOf(const PluginInstance& inst) const {
    std::vector<HookHandlerView> out;
    const std::string            owner = fmt::format("plugin:{}", inst.name);
    for (int32_t p = 0; p < AGENTXX_PLUGIN_HOOK_COUNT; ++p) {
        for (const auto& entry : orderedHandlers(static_cast<AgentxxPluginHookPoint>(p))) {
            if (entry.owner == owner) {
                out.push_back(viewOf(entry));
            }
        }
    }
    return out;
}

// =====================================================================
// 登记 / 撤销
// =====================================================================

int64_t PluginManager::insertHookHandler(HookHandlerEntry entry) {
    entry.handle = nextHookHandle_++;
    entry.seq    = hookSeq_++;
    hookHandlers_.push_back(std::move(entry));
    ensureHookDispatch();
    return hookHandlers_.back().handle;
}

bool PluginManager::removeHookHandler(int64_t handle, PluginInstance* ownerInst, bool checkOwner) {
    if (handle <= 0) {
        return false;
    }
    auto it = std::find_if(
        hookHandlers_.begin(),
        hookHandlers_.end(),
        [handle](const HookHandlerEntry& entry) {
            return entry.handle == handle;
        }
    );
    if (it == hookHandlers_.end()) {
        return false;
    }
    if (checkOwner && ownerInst != nullptr) {
        if (it->inst.lock().get() != ownerInst) {
            XX_LOGW(
                "插件 `{}` 撤销钩子处理器 #{} 被拒绝: 句柄不属于本实例",
                ownerInst->name,
                handle
            );
            return false;
        }
    }
    hookHandlers_.erase(it);
    retireHookDispatchIfIdle();
    return true;
}

void PluginManager::detachHookHandlers(PluginInstance* inst) {
    if (inst == nullptr) {
        return;
    }
    size_t removed = 0;
    for (auto it = hookHandlers_.begin(); it != hookHandlers_.end();) {
        if (it->inst.lock().get() == inst) {
            it = hookHandlers_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    // 实例记录保留 (诊断与"本实例登记过什么"用), 句柄置 0 表示已从注册表摘除
    for (auto& record : inst->hookRegistrations) {
        record.handle = 0;
    }
    if (removed > 0) {
        retireHookDispatchIfIdle();
    }
}

int PluginManager::registerHook(PluginInstance* inst, const AgentxxPluginHookSpec* spec) {
    if (inst == nullptr || spec == nullptr || spec->point < 0
        || spec->point >= AGENTXX_PLUGIN_HOOK_COUNT || !spec->hook_start) {
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("插件 `{}` registerHook 被拒绝: 实例正在关闭或已禁用", inst->name);
        return -1;
    }
    // 基础入口: 同一实例同一个点只保留一个处理器 (重复登记 = 覆盖)
    for (auto& record : inst->hookRegistrations) {
        if (record.base && record.point == spec->point) {
            if (record.handle > 0) {
                removeHookHandler(record.handle, inst, /*checkOwner=*/true);
            }
            record.handle = 0;
        }
    }
    std::erase_if(inst->hookRegistrations, [spec](const PluginInstance::HookRegistration& record) {
        return record.base && record.point == spec->point;
    });

    HookHandlerEntry entry;
    entry.point    = spec->point;
    entry.priority = agentxx::feature::kPluginDefaultPriority;
    entry.owner    = fmt::format("plugin:{}", inst->name);
    entry.load     = (inst->builtinUnload != nullptr) ? "builtin" : "dynamic";
    entry.inst     = inst->self;
    entry.spec     = *spec;

    const int64_t handle = insertHookHandler(std::move(entry));
    inst->hookRegistrations.push_back(PluginInstance::HookRegistration{spec->point, handle, true});
    return 0;
}

int PluginManager::unregisterHook(PluginInstance* inst, AgentxxPluginHookPoint point) {
    if (inst == nullptr || point < 0 || point >= AGENTXX_PLUGIN_HOOK_COUNT) {
        return -1;
    }
    auto it = std::find_if(
        inst->hookRegistrations.begin(),
        inst->hookRegistrations.end(),
        [point](const PluginInstance::HookRegistration& record) {
            return record.base && record.point == static_cast<int32_t>(point);
        }
    );
    if (it == inst->hookRegistrations.end()) {
        return -1;
    }
    const int64_t handle = it->handle;
    it->handle           = 0;
    if (handle > 0) {
        removeHookHandler(handle, inst, /*checkOwner=*/true);
    }
    std::erase_if(inst->hookRegistrations, [point](const PluginInstance::HookRegistration& record) {
        return record.base && record.point == static_cast<int32_t>(point);
    });
    return 0;
}

int PluginManager::registerHookEx(
    PluginInstance*                inst,
    const AgentxxPluginHookSpecEx* spec,
    int64_t*                       outHandle
) {
    if (inst == nullptr || spec == nullptr || outHandle == nullptr) {
        return -1;
    }
    *outHandle = 0;
    if (spec->point < 0 || spec->point >= AGENTXX_PLUGIN_HOOK_COUNT || !spec->hook_start) {
        return -1;
    }
    // struct_size 守卫: 非 0 时必须覆盖当前结构体 (新宿主的字段不会被老插件误读)
    if (spec->struct_size != 0 && spec->struct_size < sizeof(AgentxxPluginHookSpecEx)) {
        XX_LOGW("插件 `{}` registerHookEx 被拒绝: struct_size 过小", inst->name);
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("插件 `{}` registerHookEx 被拒绝: 实例正在关闭或已禁用", inst->name);
        return -1;
    }

    HookHandlerEntry entry;
    entry.point    = spec->point;
    entry.owner    = fmt::format("plugin:{}", inst->name);
    entry.priority = clampHookPriority(spec->point, entry.owner, spec->priority);
    entry.ownerTag = viewToStr(spec->owner_tag);
    entry.depict   = viewToStr(spec->depict);
    entry.load     = (inst->builtinUnload != nullptr) ? "builtin" : "dynamic";
    entry.inst     = inst->self;
    entry.spec.point       = spec->point;
    entry.spec._reserved   = 0;
    entry.spec.hook_start  = spec->hook_start;
    entry.spec.hook_cancel = spec->hook_cancel;
    entry.spec.user_data   = spec->user_data;

    const int64_t handle = insertHookHandler(std::move(entry));
    inst->hookRegistrations.push_back(PluginInstance::HookRegistration{spec->point, handle, false});
    *outHandle = handle;
    return 0;
}

int PluginManager::unregisterHookEx(PluginInstance* inst, int64_t handle) {
    if (inst == nullptr || handle <= 0) {
        return -1;
    }
    auto it = std::find_if(
        inst->hookRegistrations.begin(),
        inst->hookRegistrations.end(),
        [handle](const PluginInstance::HookRegistration& record) {
            return record.handle == handle;
        }
    );
    if (it == inst->hookRegistrations.end()) {
        XX_LOGW("插件 `{}` 撤销钩子处理器 #{} 被拒绝: 句柄不属于本实例", inst->name, handle);
        return -1;
    }
    const bool removed = removeHookHandler(handle, inst, /*checkOwner=*/true);
    inst->hookRegistrations.erase(it);
    return removed ? 0 : -1;
}

int64_t PluginManager::addCoreHookHandler(
    AgentxxPluginHookPoint point,
    int32_t                priority,
    std::string            module,
    AgentxxPluginHookSpec  spec
) {
    if (point < 0 || point >= AGENTXX_PLUGIN_HOOK_COUNT || !spec.hook_start) {
        return 0;
    }
    if (module.empty()) {
        module = "unknown";
    }
    HookHandlerEntry entry;
    entry.point     = point;
    entry.coreLayer = true;
    entry.owner     = fmt::format("core:{}", module);
    entry.priority  = clampHookPriority(point, entry.owner, priority);
    entry.load      = "builtin";
    entry.spec      = spec;
    return insertHookHandler(std::move(entry));
}

bool PluginManager::removeCoreHookHandler(int64_t handle) {
    return removeHookHandler(handle, nullptr, /*checkOwner=*/false);
}

// =====================================================================
// 单派发器 (中间件链)
// =====================================================================

void PluginManager::ensureHookDispatch() {
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        // 中间件链还没装配 (理论上不会: 插件在 initMiddleware 之后才装载)
        XX_LOGW("钩子派发器无法挂载: 中间件上下文尚未装配");
        return;
    }
    if (hookDispatch_ == nullptr) {
        hookDispatch_ = std::make_shared<PluginHookDispatchHandle>(
            "plugin_hooks",
            agentContext_,
            shared_from_this()
        );
    }
    // 曾被停用 (没有处理器时停用待摘除): 重新生效并取消排队中的摘除
    hookDispatch_->disabled = false;
    std::erase_if(pendingCleanups_, [this](const PendingMiddlewareCleanup& item) {
        return item.handle.lock().get() == hookDispatch_.get();
    });
    auto&      handles  = ctx->middlewareHandleContext->handles;
    const bool attached = std::any_of(
        handles.begin(),
        handles.end(),
        [this](const std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>& h) {
            return h.get() == hookDispatch_.get();
        }
    );
    if (!attached) {
        handles.push_back(hookDispatch_);
    }
}

void PluginManager::retireHookDispatchIfIdle() {
    if (hookDispatch_ == nullptr || !hookHandlers_.empty()) {
        return;
    }
    // 先停用: 链遍历对 disabled 句柄直接跳过, 立刻不再派发
    hookDispatch_->disabled = true;
    if (runningTurns_ > 0) {
        // 轮次执行中不修改中间件链 (与旧实现同一约定): 轮末由 flushPendingCleanup 摘除
        pendingCleanups_.push_back(PendingMiddlewareCleanup{"plugin_hooks", hookDispatch_});
        return;
    }
    eraseHookDispatch(hookDispatch_.get());
    hookDispatch_ = nullptr;
}

void PluginManager::eraseHookDispatch(PluginHookDispatchHandle* handle) {
    if (handle == nullptr) {
        return;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return;
    }
    auto& handles = ctx->middlewareHandleContext->handles;
    handles.erase(
        std::remove_if(
            handles.begin(),
            handles.end(),
            [handle](const std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>& h) {
                return h.get() == handle;
            }
        ),
        handles.end()
    );
}

// =====================================================================
// 派发
// =====================================================================

namespace {

/// 驱动一个插件层处理器 (与工具 / 功能点实现同一套操作协议), 失败只记日志
asio::awaitable<void> runPluginHookHandler(
    const std::shared_ptr<PluginInstance>& inst,
    const AgentxxPluginHookSpec&           spec,
    AgentxxPluginHookPoint                 point,
    int64_t                                handle,
    std::string                            inputJson
) {
    auto       ex   = co_await asio::this_coro::executor;
    const auto specCopy = spec;

    plugin::OpDrive drive;
    drive.start = [specCopy, inputJson, point](
                      const PluginxxOperatorNotify* notify,
                      PluginxxString*               err
                  ) -> void* {
        auto inSv = agentxx::plugin::PluginStringView::from(inputJson.data(), inputJson.size());
        return specCopy.hook_start(specCopy.user_data, point, &inSv, notify, err);
    };
    drive.cancel = [specCopy](void* op) {
        if (specCopy.hook_cancel != nullptr) {
            specCopy.hook_cancel(specCopy.user_data, op);
        }
    };

    try {
        co_await agentxx::util::awaitHostPluginOp(plugin::PluginOpAwaitArgs{
            .inst        = inst,
            .label       = fmt::format("hook#{}", static_cast<int>(point)),
            .ex          = ex,
            .cancelToken = nullptr,
            .drive       = std::move(drive),
        });
    } catch (const std::exception& e) {
        XX_LOGW(
            "插件 `{}` 的钩子处理器 #{} (point={}) 失败: {}",
            inst->name,
            handle,
            hookPointName(static_cast<int32_t>(point)),
            e.what()
        );
    } catch (...) {
        XX_LOGW(
            "插件 `{}` 的钩子处理器 #{} (point={}) 未知失败",
            inst->name,
            handle,
            hookPointName(static_cast<int32_t>(point))
        );
    }
}

} // namespace

asio::awaitable<void>
    PluginManager::dispatchHook(AgentxxPluginHookPoint point, const neograph::graph::NodeInput& in) {
    if (point < 0 || point >= AGENTXX_PLUGIN_HOOK_COUNT) {
        co_return;
    }
    // 派发开始时取快照: 派发过程中登记/撤销不影响本次 (每个处理器执行前仍检查实例可用)
    const auto handlers = orderedHandlers(point);
    const bool devMode  = agentxx::agent::AgentConfigStatic::devMode;
    std::string order;
    const auto  startedAt = std::chrono::steady_clock::now();
    size_t      executed  = 0;

    for (const auto& entry : handlers) {
        if (!entry.coreLayer) {
            auto inst = entry.inst.lock();
            if (inst == nullptr || !inst->enabled) {
                continue; // 实例已卸载或已禁用: 该处理器不生效
            }
            if (devMode) {
                order += fmt::format("{}{}#{}", order.empty() ? "" : ", ", entry.owner, entry.handle);
            }
            co_await runPluginHookHandler(
                inst,
                entry.spec,
                point,
                entry.handle,
                summarizeNodeInput(point, in).dump()
            );
            ++executed;
            continue;
        }
        // core 层处理器: 宿主自己的实现已在 io 线程, 直接同步调用 (不走操作协议)
        // - 完成通知器是"什么都不做"的实现: 处理器里报的终态被丢弃 (钩子结果本就不用)
        // - core 层只支持同步处理器: 返回操作句柄的按"不支持"记一条警告并请求取消
        if (devMode) {
            order += fmt::format("{}{}#{}", order.empty() ? "" : ", ", entry.owner, entry.handle);
        }
        try {
            const auto inputJson = summarizeNodeInput(point, in).dump();
            auto       inSv      = agentxx::plugin::PluginStringView::from(inputJson);
            PluginxxString err{nullptr, 0};
            PluginxxOperatorNotify notify{&ignoreHookCompletion, nullptr};
            void* const op = entry.spec.hook_start(entry.spec.user_data, point, &inSv, &notify, &err);
            if (err.data != nullptr) {
                agentxx::plugin::hostMemoryFree(err.data);
            }
            if (op != nullptr) {
                XX_LOGW(
                    "core 钩子处理器 #{} (point={}): core 层只支持同步处理器, 已请求取消"
                    " (需要异步请登记插件层处理器)",
                    entry.handle,
                    hookPointName(static_cast<int32_t>(point))
                );
                if (entry.spec.hook_cancel != nullptr) {
                    entry.spec.hook_cancel(entry.spec.user_data, op);
                }
            }
        } catch (const std::exception& e) {
            XX_LOGW(
                "core 钩子处理器 #{} (point={}) 失败: {}",
                entry.handle,
                hookPointName(static_cast<int32_t>(point)),
                e.what()
            );
        } catch (...) {
            XX_LOGW(
                "core 钩子处理器 #{} (point={}) 未知失败",
                entry.handle,
                hookPointName(static_cast<int32_t>(point))
            );
        }
        ++executed;
    }

    if (devMode) {
        auto&      stat = hookStats_[static_cast<size_t>(point)];
        const auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - startedAt
        )
                            .count();
        ++stat.dispatches;
        stat.handlers  = executed;
        stat.lastMs    = static_cast<uint64_t>(ms < 0 ? 0 : ms);
        stat.lastOrder = std::move(order);
    }
}

// =====================================================================
// 清单
// =====================================================================

std::string PluginManager::hooksJson() {
    const bool       devMode = agentxx::agent::AgentConfigStatic::devMode;
    utilxx_base::Json points = utilxx_base::Json::array();
    int64_t           total  = 0;

    for (int32_t p = 0; p < AGENTXX_PLUGIN_HOOK_COUNT; ++p) {
        const auto point = static_cast<AgentxxPluginHookPoint>(p);
        utilxx_base::Json handlersJson = utilxx_base::Json::array();
        for (const auto& entry : orderedHandlers(point)) {
            const auto        view = viewOf(entry);
            utilxx_base::Json item = utilxx_base::Json::object();
            item["handle"]         = view.handle;
            item["layer"]          = view.layer;
            item["owner"]          = view.owner;
            if (!view.ownerTag.empty()) {
                item["ownerTag"] = view.ownerTag;
            }
            if (!view.depict.empty()) {
                item["depict"] = view.depict;
            }
            item["priority"] = view.priority;
            item["seq"]      = static_cast<int64_t>(view.seq);
            item["enabled"]  = view.enabled;
            item["load"]     = view.load;
            handlersJson.push_back(std::move(item));
            ++total;
        }

        utilxx_base::Json pointJson = utilxx_base::Json::object();
        pointJson["point"]          = p;
        pointJson["name"]           = hookPointName(p);
        pointJson["count"]          = static_cast<int64_t>(handlersJson.size());
        pointJson["handlers"]       = std::move(handlersJson);
        if (devMode) {
            // 派发记录只在开发者模式下出现 (关闭时不是"全 0", 而是没有该段)
            const auto&       stat = hookStats_[static_cast<size_t>(p)];
            utilxx_base::Json st   = utilxx_base::Json::object();
            st["dispatches"]       = static_cast<int64_t>(stat.dispatches);
            st["lastHandlers"]     = static_cast<int64_t>(stat.handlers);
            st["lastMs"]           = static_cast<int64_t>(stat.lastMs);
            st["lastOrder"]        = stat.lastOrder;
            pointJson["stat"]      = std::move(st);
        }
        points.push_back(std::move(pointJson));
    }

    utilxx_base::Json out = utilxx_base::Json::object();
    out["devMode"]        = devMode;
    out["count"]          = static_cast<int64_t>(AGENTXX_PLUGIN_HOOK_COUNT);
    out["handlers"]       = total;
    out["points"]         = std::move(points);
    return out.dump();
}

// =====================================================================
// 派发器 (中间件句柄)
// =====================================================================

asio::awaitable<void>
    PluginHookDispatchHandle::onAgentcallStartFunc(neograph::graph::NodeInput& in) {
    if (auto mgr = manager_.lock()) {
        co_await mgr->dispatchHook(AGENTXX_PLUGIN_HOOK_AGENT_START, in);
    }
}

asio::awaitable<void> PluginHookDispatchHandle::onAgentcallEndFunc(
    const neograph::graph::NodeInput& in,
    neograph::graph::NodeOutput&
) {
    if (auto mgr = manager_.lock()) {
        co_await mgr->dispatchHook(AGENTXX_PLUGIN_HOOK_AGENT_END, in);
    }
}

asio::awaitable<void>
    PluginHookDispatchHandle::onModelcallStartFunc(neograph::graph::NodeInput& in) {
    if (auto mgr = manager_.lock()) {
        co_await mgr->dispatchHook(AGENTXX_PLUGIN_HOOK_MODEL_START, in);
    }
}

asio::awaitable<void> PluginHookDispatchHandle::onModelcallRunFunc(neograph::graph::NodeInput& in) {
    if (auto mgr = manager_.lock()) {
        co_await mgr->dispatchHook(AGENTXX_PLUGIN_HOOK_MODEL_RUN, in);
    }
}

asio::awaitable<void> PluginHookDispatchHandle::onModelcallEndFunc(
    const neograph::graph::NodeInput& in,
    neograph::graph::NodeOutput&
) {
    if (auto mgr = manager_.lock()) {
        co_await mgr->dispatchHook(AGENTXX_PLUGIN_HOOK_MODEL_END, in);
    }
}

asio::awaitable<void>
    PluginHookDispatchHandle::onToolcallStartFunc(neograph::graph::NodeInput& in) {
    if (auto mgr = manager_.lock()) {
        co_await mgr->dispatchHook(AGENTXX_PLUGIN_HOOK_TOOL_START, in);
    }
}

asio::awaitable<void> PluginHookDispatchHandle::onToolcallEndFunc(
    const neograph::graph::NodeInput& in,
    neograph::graph::NodeOutput&
) {
    if (auto mgr = manager_.lock()) {
        co_await mgr->dispatchHook(AGENTXX_PLUGIN_HOOK_TOOL_END, in);
    }
}

} // namespace plugin
} // namespace agentxx
