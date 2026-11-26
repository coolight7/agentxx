#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/plugin/api/plugin_api.h"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "pluginxx/api/kinds.h"
#include "pluginxx/host/kind_registry.h"
#include "pluginxx/host/manifest.h"
#include "pluginxx/host/table_op.h"
#include "pluginxx/runtime/instance_base.h"
#include "pluginxx/runtime/op_driver.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include <chrono>
#include <filesystem>
#include <utility>

namespace agentxx {
namespace plugin {

namespace {

/// 一次"问运行器"的等待状态
///
/// 运行器的回答经能力完成回调回来 (IO 线程), 这里用定时器把等待方唤醒: 回调只写状态 +
/// 取消定时器, 所以等待方与回调在同一个线程上也不会互相等待。
struct KindProviderCall {
    bool                                done = false;
    bool                                ok   = false;
    std::string                         text;
    std::unique_ptr<asio::steady_timer> wake;
};

/// 能力完成回调的持有者: 把框架回调转发给调用方给的闭包
struct KindCallBridge {
    std::function<void(int32_t, const std::string&)> done;
};

/// 能力完成回调 (恒在 IO 线程发布)
void PLUGINXX_CALL kindCallTrampoline(void* ud, int32_t status, const PluginxxStringView* payload) {
    std::unique_ptr<KindCallBridge> holder{static_cast<KindCallBridge*>(ud)};
    if (!holder) {
        return;
    }
    std::string text;
    if (payload && payload->data) {
        text.assign(payload->data, static_cast<size_t>(payload->size));
    }
    if (holder->done) {
        holder->done(status == PLUGINXX_OPERATOR_OK ? PLUGINXX_RESULT_OK : PLUGINXX_RESULT_ERR_STATE, text);
    }
}

/// 运行器回答的解析结果
struct KindAnswer {
    bool        ok = false;
    std::string reason;
};

/// 解析运行器的回答: `{"ok":true}` = 接手成功; `{"ok":false,"error":"..."}` = 拒绝
KindAnswer parseKindAnswer(const std::string& text) {
    KindAnswer answer;
    if (text.empty()) {
        answer.ok = true; ///< 没有正文 = 没话说, 按成功处理 (与能力完成语义一致)
        return answer;
    }
    utilxx_base::Json json;
    try {
        json = utilxx_base::Json::parse(text);
    } catch (...) {
        // 运行器回了非 JSON: 只要能力本身成功就按成功处理, 不猜它的意思
        answer.ok = true;
        return answer;
    }
    if (json.is_object() && json.contains("ok") && json["ok"].is_boolean()) {
        answer.ok = json["ok"].get<bool>();
        if (!answer.ok) {
            answer.reason = json.value("error", std::string{"the runner rejected this operation"});
        }
        return answer;
    }
    answer.ok = true;
    return answer;
}

/// 主程序插件配置 → 插件框架加载参数 (与生命周期骨架的适配层同一套字段)
void kindToLoadOptions(
    const agentxx::agent::PluginConfig* cfg,
    pluginxx::PluginLoadOptions&        options,
    bool&                               hasOptions
) {
    hasOptions = (cfg != nullptr);
    if (cfg) {
        options.args       = cfg->args;
        options.configPath = cfg->configPath;
    }
}

} // namespace

asio::awaitable<std::pair<bool, std::string>> PluginManager::invokeKindCapability(
    PluginInstance* caller,
    std::string     capability,
    std::string     argsJson,
    uint32_t        timeoutMs
) {
    if (caller == nullptr) {
        co_return std::make_pair(false, std::string{"plugin kind call has no target instance"});
    }
    if (!ioExecutor()) {
        co_return std::make_pair(false, std::string{"plugin host has no IO executor"});
    }
    auto state     = std::make_shared<KindProviderCall>();
    state->wake    = std::make_unique<asio::steady_timer>(ioExecutor());
    auto bridge    = new KindCallBridge{[state](int32_t rc, const std::string& text) {
        state->done = true;
        state->ok   = rc == PLUGINXX_RESULT_OK;
        state->text = text;
        // 唤醒等待方: 还没在等的时候取消失败只说明没有等待项
        try {
            state->wake->cancel();
        } catch (...) {
        }
    }};
    auto capView  = std::string_view{capability};
    auto argsView = std::string_view{argsJson};
    PluginxxString callErr{};
    // 调用方实例传目标实例本身: 运行器据此把脚本里的注册挂到目标插件上
    auto* handle = invokeCapabilityAsync(
        caller,
        nullptr, ///< 作用域: 主程序发起的调用不在任何作用域里
        capView,
        argsView,
        0,  ///< 阶段: 0 = 完整链 before → impl → after
        -1, ///< 超时: 用声明里的值 (运行器自己排队, 框架不再加一层)
        &kindCallTrampoline,
        bridge,
        &callErr
    );
    if (!handle) {
        // 受理失败: 完成回调不会被调用, 由这里回收 bridge 并给出原因
        std::unique_ptr<KindCallBridge> holder{bridge};
        std::string message = callErr.data
                                  ? std::string{callErr.data, static_cast<size_t>(callErr.size)}
                                  : std::string{"the capability call was not accepted"};
        if (callErr.data) {
            pluginxx::hostMemoryFree(callErr.data);
        }
        co_return std::make_pair(false, std::move(message));
    }
    if (callErr.data) {
        pluginxx::hostMemoryFree(callErr.data);
    }
    if (!state->done) {
        state->wake->expires_after(std::chrono::milliseconds{timeoutMs});
        co_await state->wake->async_wait(asio::as_tuple(asio::use_awaitable));
    }
    if (!state->done) {
        co_return std::make_pair(
            false,
            fmt::format(
                "the runner did not answer within {} ms ({})",
                timeoutMs,
                capability
            )
        );
    }
    if (!state->ok) {
        co_return std::make_pair(
            false,
            state->text.empty() ? fmt::format("the runner call `{}` failed", capability)
                                : state->text
        );
    }
    const KindAnswer answer = parseKindAnswer(state->text);
    co_return std::make_pair(answer.ok, answer.reason);
}

asio::awaitable<bool> PluginManager::startInterpretedInstance(
    const std::shared_ptr<PluginInstance>& inst,
    std::string&                           reason
) {
    reason.clear();
    if (!inst) {
        reason = "missing instance";
        co_return false;
    }
    const pluginxx::PluginxxKindSpec* spec = kindRegistry().find(inst->kind);
    if (spec == nullptr) {
        reason = fmt::format(
            "plugin kind `{}` is not registered (install a plugin that provides it)",
            inst->kind
        );
        co_return false;
    }
    if (spec->frameworkOwned() || spec->loadCapability.empty()) {
        reason = fmt::format(
            "plugin kind `{}` has no runner: install a plugin that provides it",
            inst->kind
        );
        co_return false;
    }
    // 运行器可能已经卸载: 没加载的运行器直接报清楚, 不要等一个永远不来的回答
    if (!find(spec->provider)) {
        reason = fmt::format(
            "plugin kind `{}` is provided by plugin `{}`, which is not loaded "
            "(enable that runner plugin first)",
            inst->kind,
            spec->provider
        );
        co_return false;
    }
    utilxx_base::Json payload;
    payload["action"]   = "load";
    payload["kind"]     = inst->kind;
    payload["id"]       = inst->name;
    payload["path"]     = inst->path;
    payload["instance"] = fmt::format("{}:{}", inst->kind, inst->name);
    payload["version"]  = inst->version;
    payload["side"]     = "agent";
    // 校验 (可选): 运行器比框架更清楚"这种插件长什么样"
    if (!spec->validateCapability.empty()) {
        utilxx_base::Json check;
        check["action"]  = "validate";
        check["kind"]    = inst->kind;
        check["id"]      = inst->name;
        check["path"]    = inst->path;
        check["version"] = inst->version;
        const auto [accepted, why] = co_await invokeKindCapability(
            inst.get(),
            spec->validateCapability,
            check.dump(),
            15000
        );
        if (!accepted) {
            reason = why.empty()
                         ? fmt::format(
                               "runner plugin `{}` refused to load `{}`",
                               spec->provider,
                               inst->name
                           )
                         : why;
            co_return false;
        }
    }
    const auto [ok, message]
        = co_await invokeKindCapability(inst.get(), spec->loadCapability, payload.dump(), 30000);
    if (!ok) {
        reason = message.empty()
                     ? fmt::format(
                           "runner plugin `{}` failed to load `{}`",
                           spec->provider,
                           inst->name
                       )
                     : message;
        co_return false;
    }
    inst->kindLoaded = true;
    co_return true;
}

asio::awaitable<bool>
    PluginManager::stopInterpretedInstance(const std::shared_ptr<PluginInstance>& inst) {
    if (!inst || !inst->kindLoaded) {
        co_return true;
    }
    inst->kindLoaded            = false;
    const pluginxx::PluginxxKindSpec* spec = kindRegistry().find(inst->kind);
    if (spec == nullptr || spec->frameworkOwned() || spec->unloadCapability.empty()) {
        // 没有卸载能力 / 形态已被撤销: 运行器自己收尾, 框架这里没有可做的
        co_return true;
    }
    utilxx_base::Json payload;
    payload["action"]   = "unload";
    payload["kind"]     = inst->kind;
    payload["id"]       = inst->name;
    payload["path"]     = inst->path;
    payload["instance"] = fmt::format("{}:{}", inst->kind, inst->name);
    const auto [ok, message]
        = co_await invokeKindCapability(inst.get(), spec->unloadCapability, payload.dump(), 15000);
    if (!ok) {
        XX_LOGW(
            "Plugin `{}`: runner `{}` failed to unload its script: {}",
            inst->name,
            spec->provider,
            message
        );
    }
    co_return ok;
}

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadInterpretedAsync(
    std::string                         path,
    std::string                         kind,
    const agentxx::agent::PluginConfig* cfg
) {
    namespace fs = std::filesystem;

    const fs::path dir{utilxx_base::utf8ToPath(path)};
    std::string    name, entry, manifestKind, version, description;
    int32_t        apiVersion = 0;
    std::vector<std::string>    depends, optionalDepends;
    PluginManifestResources     resources;
    PluginManifestInterfaces    interfaces;
    if (!parsePluginManifest(
            dir,
            name,
            entry,
            depends,
            optionalDepends,
            &resources,
            &interfaces,
            &manifestKind,
            &version,
            &description,
            &apiVersion
        )) {
        XX_LOGE("Plugin load failed: `{}` has no readable plugin.yaml", path);
        co_return nullptr;
    }
    if (name.empty()) {
        XX_LOGE("Plugin load failed: `{}` manifest has no name", path);
        co_return nullptr;
    }
    const pluginxx::PluginxxKindSpec* spec = kindRegistry().find(kind);
    if (spec == nullptr) {
        XX_LOGE(
            "Plugin `{}` load failed: kind `{}` is not registered (install a plugin that "
            "provides it)",
            name,
            kind
        );
        co_return nullptr;
    }
    if (spec->frameworkOwned() || spec->loadCapability.empty()) {
        XX_LOGE(
            "Plugin `{}` load failed: kind `{}` has no runner (install a plugin that provides it)",
            name,
            kind
        );
        co_return nullptr;
    }
    // 代次: 解释执行的形态用自己的接口代次对判 (清单 api_version >= 下限)
    if (apiVersion > 0 && spec->minInterfaceVersion > 0
        && apiVersion < spec->minInterfaceVersion) {
        XX_LOGE(
            "Plugin `{}` load failed: it declares api_version {} but kind `{}` requires >= {}",
            name,
            apiVersion,
            kind,
            spec->minInterfaceVersion
        );
        co_return nullptr;
    }

    if (!this->reservePluginName(name)) {
        XX_LOGE("Plugin load rejected: duplicate or currently loading name `{}`", name);
        co_return nullptr;
    }
    // 没有动态库: 生命周期入口留空, 加载 / 卸载全走运行器
    auto inst = makeInstance(
        name,
        version,
        description,
        path,
        /*createFn=*/nullptr,
        /*stopFn=*/nullptr,
        /*destroyFn=*/nullptr
    );
    if (!inst) {
        this->releasePluginName(name);
        co_return nullptr;
    }
    pluginxx::PluginLoadOptions options;
    bool                        hasOptions = false;
    kindToLoadOptions(cfg, options, hasOptions);
    if (hasOptions) {
        applyLoadOptions(*inst, &options);
    }
    inst->kind             = kind;
    inst->depends          = std::move(depends);
    inst->optionalDepends  = std::move(optionalDepends);
    inst->interfaces       = interfaces;
    this->plugins_[inst->name] = inst;

    std::string reason;
    if (!co_await startInterpretedInstance(inst, reason)) {
        XX_LOGE("Plugin `{}` load failed: {}", inst->name, reason);
        co_await this->rollbackLoadAsync(inst, /*closeHandle=*/false);
        co_return nullptr;
    }
    finishLoad(inst, resources);
    XX_LOGI("Plugin `{}` loaded successfully (kind `{}`, runner `{}`)", inst->name, kind, spec->provider);
    co_return inst;
}

} // namespace plugin
} // namespace agentxx
