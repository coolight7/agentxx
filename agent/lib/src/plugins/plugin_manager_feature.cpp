/// 功能点宿主实现 (接口表 `agentxx.agent.feature` 的落地)
///
/// 一个文件承载三件事:
/// 1. **实现登记**: 插件写的 `impl_start` / `impl_cancel` 经操作协议驱动
///    (`pluginxx::OpCore`, 与工具/钩子同一套), 包装成 `agentxx::feature::ImplSpec`
///    交给点;
/// 2. **插件点声明**: `define_point` / `undefine_point` 落到注册表 (命名空间校验与
///    生命周期记账都在注册表与实例记录里);
/// 3. **调用**: `call_point_async` 在宿主 IO 线程上跑点的实现链, 结果经回调返回。
///
/// 线程约定: 本文件的入口都由 vtable 层投递到 IO 线程后调用 (见
/// plugin_manager_vtable.cpp 的 ioCallSyncKeep); 只有 IO 线程访问注册表与实例记录。
#include "agentxx/feature/points.h"
#include "agentxx/feature/registry.h"
#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/agent/context.h"
#include "agentxx/plugin/plugin_framework.h"
#include "agentxx/util/exception.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "pluginxx/runtime/op_driver.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>

namespace agentxx {
namespace plugin {

namespace {

/// C ABI 视图 → std::string
std::string viewToStr(const PluginxxStringView& sv) {
    if (sv.data == nullptr || sv.size == 0) {
        return {};
    }
    return std::string{sv.data, static_cast<size_t>(sv.size)};
}

/// 归属标签: 本实例在功能点体系里的身份 (`plugin:<插件名>`)
std::string featureOwnerOf(const PluginInstance& inst) {
    return fmt::format("plugin:{}", inst.name);
}

/// 插件实现的回答规范化 (插件边界):
/// - 合法的 JSON 但不带回答关键字 (`value` / `disable` / `verdict`) 时, 按
///   "整份回答就是值" 包一层 `{"value": ...}` —— 插件写 `{"tokens":42}` 与写
///   `{"value":{"tokens":42}}` 等价, 同步实现 (SDK 的 wrapFeatureAnswer) 与
///   协程实现的写法因此保持一致;
/// - 空文本 / 非法 JSON / 已带回答关键字的文本原样返回, 交给 [parseImplAnswer]
///   判定 (空对象与非法 JSON 都等价于"没意见")
std::string normalizePluginAnswer(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const auto parsed = utilxx_base::catchError<std::optional<utilxx_base::Json>>(
        [text]() -> std::optional<utilxx_base::Json> {
            return utilxx_base::Json::parse(text);
        },
        [](std::string) -> std::optional<utilxx_base::Json> {
            return std::nullopt;
        }
    );
    if (!parsed.has_value()) {
        return std::string{text};
    }
    if (parsed->is_object()
        && (parsed->contains("value") || parsed->contains("disable")
            || parsed->contains("verdict"))) {
        return std::string{text};
    }
    utilxx_base::Json wrapped = utilxx_base::Json::object();
    wrapped["value"]          = *parsed;
    return wrapped.dump();
}

/// 写 C ABI 出参错误串 (经实例的宿主视图分配, 与插件侧释放路径一致)
void setFeatureErrOut(PluginInstance* inst, PluginxxString* error_out, const std::string& msg) {
    if (error_out == nullptr || error_out->data != nullptr) {
        return;
    }
    const PluginxxHost* host = inst != nullptr ? inst->hostView() : nullptr;
    *error_out               = PluginString::from(host, strToSv(msg));
    if (error_out->data == nullptr) {
        auto* p = static_cast<char*>(hostMemoryAlloc(msg.size() + 1));
        if (p != nullptr) {
            std::memcpy(p, msg.c_str(), msg.size() + 1);
            error_out->data = p;
            error_out->size = msg.size();
        }
    }
}

/// 一个注册下正在跑的插件实现调用集合
///
/// 超时/取消只能拿到"注册级"的取消函数 (ImplSpec::CancelFn 每注册一个), 因此这里
/// 记录该注册当前活跃的操作句柄; 取消时逐个请求取消。句柄用 weak_ptr, 操作自己
/// 完成即失效, 不需要额外清理。
struct ImplOps {
    std::mutex                                    mutex;
    std::vector<std::weak_ptr<pluginxx::OpCore>>  ops;

    void track(const std::shared_ptr<pluginxx::OpCore>& op) {
        std::vector<std::weak_ptr<pluginxx::OpCore>> alive;
        alive.reserve(ops.size() + 1);
        for (auto& item : ops) {
            if (!item.expired()) {
                alive.push_back(item);
            }
        }
        alive.push_back(op);
        std::lock_guard lock(mutex);
        ops = std::move(alive);
    }

    /// 请求取消全部活跃调用 (协作式; 真正的终态仍由插件 done 决定)
    void cancelAll() {
        std::vector<std::shared_ptr<pluginxx::OpCore>> targets;
        {
            std::lock_guard lock(mutex);
            for (auto& item : ops) {
                if (auto op = item.lock()) {
                    targets.push_back(std::move(op));
                }
            }
        }
        for (auto& op : targets) {
            op->cancel();
        }
    }
};

} // namespace

// =====================================================================
// 实现登记
// =====================================================================

int PluginManager::registerFeatureImpl(PluginInstance* inst, const AgentxxPluginFeatureImplSpec* spec) {
    if (inst == nullptr || spec == nullptr || !spec->impl_start) {
        return -1;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || ctx->features == nullptr) {
        XX_LOGW("插件 `{}` 登记功能点实现被拒绝: 功能点注册表未装配", inst->name);
        return -1;
    }
    const std::string pointId = viewToStr(spec->point_id);
    if (pointId.empty()) {
        XX_LOGW("插件 `{}` 登记功能点实现被拒绝: point_id 为空", inst->name);
        return -1;
    }

    // 实现体: 经操作协议驱动插件回调 (与工具 call_tool_async 同一套)
    auto self   = shared_from_this();
    auto ops    = std::make_shared<ImplOps>();
    auto weakInst = std::weak_ptr<PluginInstance>{inst->self};
    const AgentxxPluginFeatureImplSpec specCopy = *spec;
    const std::string                  owner    = featureOwnerOf(*inst);

    agentxx::feature::ImplSpec impl;
    impl.layer             = agentxx::feature::ImplLayer::Plugin;
    impl.owner             = owner;
    impl.priority          = spec->priority;
    impl.defaultTimeoutMs  = spec->default_timeout_ms;
    impl.load              = (inst->builtinUnload != nullptr) ? "builtin" : "dynamic";
    impl.fn                = [self, weakInst, specCopy, pointId, ops](std::string callJson)
        -> asio::awaitable<std::optional<std::string>> {
        auto instPtr = weakInst.lock();
        if (!instPtr) {
            co_return std::nullopt; // 实例已卸载: 按"没意见"处理
        }
        auto runtime = instPtr->runtime.lock();
        if (!runtime || !isRuntimeIoThread(runtime)) {
            XX_LOGW("功能点 `{}`: 实例 `{}` 的实现不在宿主 IO 线程上调用, 按没意见处理",
                    pointId, instPtr->name);
            co_return std::nullopt;
        }
        std::shared_ptr<pluginxx::OpCore> core;
        try {
            core = pluginxx::OpCore::create(runtime, instPtr, nullptr, fmt::format("feature_impl {}", pointId));
        } catch (const std::exception& e) {
            XX_LOGW("功能点 `{}`: 实现 `{}` 无法启动: {}", pointId, instPtr->name, e.what());
            co_return std::nullopt;
        }
        pluginxx::OpDrive drive;
        drive.start = [specCopy, pointId, callJson](const PluginxxOperatorNotify* notify,
                                                   PluginxxString*               errorOut) -> void* {
            const auto idSv   = PluginStringView::from(pointId);
            const auto jsonSv = PluginStringView::from(callJson);
            return specCopy.impl_start(specCopy.user_data, &idSv, &jsonSv, notify, errorOut);
        };
        drive.cancel = [specCopy](void* op) {
            if (specCopy.impl_cancel != nullptr) {
                specCopy.impl_cancel(specCopy.user_data, op);
            }
        };
        std::string startError;
        if (!core->start(std::move(drive), startError)) {
            XX_LOGW(
                "功能点 `{}`: 实现 `{}` 启动被拒绝: {}",
                pointId,
                instPtr->name,
                startError.empty() ? "插件未完成操作协议" : startError
            );
            co_return std::nullopt;
        }
        ops->track(core);
        co_await core->wait();
        if (core->status() != PLUGINXX_OPERATOR_OK) {
            // 取消 / 失败: 回包里是错误文本而不是回答, 按"没意见"继续问下一个实现
            // (与实现抛异常同一处理, 避免错误文本被当成值)
            co_return std::nullopt;
        }
        co_return normalizePluginAnswer(core->payload());
    };
    impl.cancel = [ops]() {
        ops->cancelAll();
    };

    if (ctx->features->addImpl(pointId, std::move(impl)) != 0) {
        return -1;
    }
    // 记账: 同一实例同一个点只保留一条记录 (重复登记 = 覆盖)
    std::erase(inst->featurePointImpls, pointId);
    inst->featurePointImpls.push_back(pointId);
    return 0;
}

int PluginManager::unregisterFeatureImpl(PluginInstance* inst, PluginxxStringView pointId) {
    if (inst == nullptr) {
        return -1;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || ctx->features == nullptr) {
        return -1;
    }
    const std::string id = viewToStr(pointId);
    if (id.empty()) {
        return -1;
    }
    const size_t removed
        = ctx->features->removeImpl(id, featureOwnerOf(*inst));
    std::erase(inst->featurePointImpls, id);
    return removed > 0 ? 0 : -1;
}

// =====================================================================
// 插件点声明
// =====================================================================

int PluginManager::defineFeaturePoint(
    PluginInstance*                     inst,
    const AgentxxPluginFeaturePointSpec* spec
) {
    if (inst == nullptr || spec == nullptr) {
        return -1;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || ctx->features == nullptr) {
        return -1;
    }
    agentxx::feature::PluginPointDecl decl;
    decl.id             = viewToStr(spec->id);
    decl.owner          = featureOwnerOf(*inst);
    decl.type           = (spec->type == AGENTXX_PLUGIN_FEATURE_TYPE_DECIDE)
                              ? agentxx::feature::PointType::Decide
                              : agentxx::feature::PointType::Provide;
    decl.title          = viewToStr(spec->title);
    decl.depict         = viewToStr(spec->depict);
    decl.argsDoc        = viewToStr(spec->args_doc);
    decl.resultDoc      = viewToStr(spec->result_doc);
    decl.implTimeoutMs  = spec->impl_timeout_ms;
    if (ctx->features->definePluginPoint(decl) != 0) {
        return -1;
    }
    std::erase(inst->featurePoints, decl.id);
    inst->featurePoints.push_back(decl.id);
    return 0;
}

int PluginManager::undefineFeaturePoint(PluginInstance* inst, PluginxxStringView pointId) {
    if (inst == nullptr) {
        return -1;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || ctx->features == nullptr) {
        return -1;
    }
    const std::string id = viewToStr(pointId);
    if (id.empty()) {
        return -1;
    }
    // 只允许撤销本实例声明的点 (注册表按来源再校验一次)
    auto* point = ctx->features->find(id);
    if (point == nullptr || point->origin() != featureOwnerOf(*inst)) {
        XX_LOGW("插件 `{}` 撤销功能点 `{}` 被拒绝: 不是本实例声明的点", inst->name, id);
        return -1;
    }
    if (ctx->features->undefinePluginPoint(id) != 0) {
        return -1;
    }
    std::erase(inst->featurePoints, id);
    // 点没了, 它上面的实现记录一并失效
    std::erase(inst->featurePointImpls, id);
    return 0;
}

std::string PluginManager::listFeaturePoints() {
    auto ctx = agentContext_.lock();
    if (!ctx || ctx->features == nullptr) {
        return utilxx_base::Json::object().dump();
    }
    return ctx->features->listPointsJson().dump();
}

// =====================================================================
// 调用
// =====================================================================

PluginxxOperatorHandle* PluginManager::callFeatureAsync(
    PluginInstance*          caller,
    PluginxxStringView       pointId,
    PluginxxStringView       argsJson,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    if (caller == nullptr || !cb || PluginStringView::empty(&pointId)) {
        setFeatureErrOut(caller, error_out, "call_point_async: invalid arguments");
        return nullptr;
    }
    if (!isIoThread()) {
        // 非 IO 线程: 投递到 IO 线程再受理 (与工具互调同一条约定)
        auto self  = shared_from_this();
        auto owner = caller->self.lock();
        auto point = viewToStr(pointId);
        auto args  = viewToStr(argsJson);
        return ioCallSync<PluginxxOperatorHandle*>(
            this,
            [self, owner, point, args, cb, ud, error_out] {
                return self->callFeatureAsync(
                    owner.get(),
                    strToSv(point),
                    strToSv(args),
                    cb,
                    ud,
                    error_out
                );
            }
        );
    }

    auto ctx = agentContext_.lock();
    if (!ctx || ctx->features == nullptr) {
        setFeatureErrOut(caller, error_out, "call_point_async: feature registry unavailable");
        return nullptr;
    }
    auto* point = ctx->features->find(viewToStr(pointId));
    if (point == nullptr) {
        setFeatureErrOut(
            caller,
            error_out,
            fmt::format("call_point_async: feature point not declared: {}", viewToStr(pointId))
        );
        return nullptr;
    }
    // 通用 JSON 点 (插件点与宿主点) 的调用入参就是调用方原样给的 JSON;
    // 强类型核心点需要类型化的请求对象, 插件没法直接构造 —— 这类点的调用在
    // 阶段 4/6 经命令行与 FFI 接入时按 JSON 请求解码 (见 plan §4.1)。
    auto* jsonPoint = dynamic_cast<agentxx::feature::JsonProvidePoint*>(point);
    if (jsonPoint == nullptr) {
        setFeatureErrOut(
            caller,
            error_out,
            fmt::format(
                "call_point_async: point `{}` 不接受按名调用 (需要类型化请求)",
                point->id()
            )
        );
        return nullptr;
    }

    // 参数必须是合法 JSON 对象; 不合法按 bad_args 结果回 (受理成功, 失败经回调)
    std::string argsText = viewToStr(argsJson);
    if (argsText.empty()) {
        argsText = "{}";
    }
    const auto parsed = utilxx_base::catchError<std::optional<utilxx_base::Json>>(
        [&]() -> std::optional<utilxx_base::Json> {
            return utilxx_base::Json::parse(argsText);
        },
        [](std::string) -> std::optional<utilxx_base::Json> {
            return std::nullopt;
        }
    );
    if (!parsed.has_value() || !parsed->is_object()) {
        argsText.clear(); // 交给点的 bad_args 判定 (JsonProvidePoint::call 里会按参数不合法回)
    }

    auto runtime = caller->sharedSelf<PluginInstance>();
    std::shared_ptr<pluginxx::OpCore> core;
    try {
        core = pluginxx::OpCore::create(
            caller->runtime.lock(),
            runtime,
            nullptr,
            fmt::format("feature_call {}", point->id())
        );
    } catch (const std::exception& e) {
        setFeatureErrOut(caller, error_out, fmt::format("call_point_async: {}", e.what()));
        return nullptr;
    }

    const std::string callerOwner = featureOwnerOf(*caller);
    core->setCallback(cb, ud);
    core->accept();

    // 跑实现链, 完成后经完成协议把结果 JSON 交给插件 (完成通知在 IO 线程发布)
    auto self = shared_from_this();
    asio::co_spawn(
        ioExecutor(),
        [jsonPoint, argsText, callerOwner, core]() -> asio::awaitable<void> {
            agentxx::feature::CallOptions opts;
            opts.caller = callerOwner;
            auto result = co_await jsonPoint->call(argsText, opts);

            utilxx_base::Json out = utilxx_base::Json::object();
            out["ok"] = result.ok();
            out["id"] = jsonPoint->id();
            if (result.ok()) {
                out["value"]     = result.value.has_value() ? *result.value : utilxx_base::Json{};
                out["by"]        = result.by;
                out["identity"]  = result.identity;
                out["fromCache"] = result.fromCache;
                out["ms"]        = result.ms;
            } else {
                out["error"]    = agentxx::feature::callErrorKey(result.error);
                out["message"]  = result.message;
                out["identity"] = result.identity;
                out["ms"]       = result.ms;
            }
            const std::string text = out.dump();
            const auto        sv   = PluginStringView::from(text);
            auto              ntf  = core->notify();
            ntf.done(
                ntf.host_ud,
                result.ok() ? PLUGINXX_OPERATOR_OK : PLUGINXX_OPERATOR_FAILED,
                &sv
            );
            co_return;
        },
        asio::detached
    );
    return core->handle();
}

} // namespace plugin
} // namespace agentxx
