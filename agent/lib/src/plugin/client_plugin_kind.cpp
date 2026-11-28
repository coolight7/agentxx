#include "agentxx/plugin/client_plugin_manager.h"

#include "agentxx/plugin/api/plugin_api.h"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "pluginxx/api/kinds.h"
#include "pluginxx/host/kind_registry.h"
#include "pluginxx/host/manifest.h"
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

/// 一次"问运行器"的等待状态 (与 agent 侧同一做法: 回调写状态 + 取消定时器)
struct KindProviderCall {
    bool                                done = false;
    bool                                ok   = false;
    std::string                         text;
    std::unique_ptr<asio::steady_timer> wake;
};

/// 能力完成回调的持有者
struct KindCallBridge {
    std::function<void(int32_t, const std::string&)> done;
};

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
        holder->done(
            status == PLUGINXX_OPERATOR_OK ? PLUGINXX_RESULT_OK : PLUGINXX_RESULT_ERR_STATE,
            text
        );
    }
}

/// 运行器回答的结论
struct KindAnswer {
    bool        ok = false;
    std::string reason;
};

KindAnswer parseKindAnswer(const std::string& text) {
    KindAnswer answer;
    if (text.empty()) {
        answer.ok = true; ///< 没有正文 = 没话说, 按成功处理
        return answer;
    }
    utilxx_base::Json json;
    try {
        json = utilxx_base::Json::parse(text);
    } catch (...) {
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

} // namespace

asio::awaitable<std::pair<bool, std::string>> ClientPluginManager::invokeKindCapability(
    ClientPluginInstance* caller,
    std::string           capability,
    std::string           argsJson,
    uint32_t              timeoutMs
) {
    if (caller == nullptr) {
        co_return std::make_pair(false, std::string{"plugin kind call has no target instance"});
    }
    if (!ioExecutor()) {
        co_return std::make_pair(false, std::string{"plugin host has no IO executor"});
    }
    auto state   = std::make_shared<KindProviderCall>();
    state->wake  = std::make_unique<asio::steady_timer>(ioExecutor());
    auto* bridge = new KindCallBridge{[state](int32_t rc, const std::string& text) {
        state->done = true;
        state->ok   = rc == PLUGINXX_RESULT_OK;
        state->text = text;
        try {
            state->wake->cancel();
        } catch (...) {
        }
    }};
    auto           capView  = std::string_view{capability};
    auto           argsView = std::string_view{argsJson};
    PluginxxString callErr{};
    auto*          handle   = invokeCapabilityAsync(
        caller,
        nullptr,
        capView,
        argsView,
        0,
        -1,
        &kindCallTrampoline,
        bridge,
        &callErr
    );
    if (!handle) {
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
            fmt::format("the runner did not answer within {} ms ({})", timeoutMs, capability)
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

asio::awaitable<bool> ClientPluginManager::startInterpretedInstance(
    const std::shared_ptr<ClientPluginInstance>& inst,
    std::string&                                 reason
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
    payload["side"]     = "client";
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

asio::awaitable<bool> ClientPluginManager::stopInterpretedInstance(
    const std::shared_ptr<ClientPluginInstance>& inst
) {
    if (!inst || !inst->kindLoaded) {
        co_return true;
    }
    inst->kindLoaded                  = false;
    const pluginxx::PluginxxKindSpec* spec = kindRegistry().find(inst->kind);
    if (spec == nullptr || spec->frameworkOwned() || spec->unloadCapability.empty()) {
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
            "[client_plugin] runner `{}` failed to unload the script of `{}`: {}",
            spec->provider,
            inst->name,
            message
        );
    }
    co_return ok;
}

asio::awaitable<std::shared_ptr<ClientPluginInstance>> ClientPluginManager::loadInterpretedAsync(
    std::string                         path,
    std::string                         kind,
    const agentxx::agent::PluginConfig* cfg
) {
    namespace fs = std::filesystem;

    const fs::path dir{utilxx_base::utf8ToPath(path)};
    std::string    name, entry, manifestKind, version, description;
    int32_t        apiVersion = 0;
    std::vector<std::string> depends, optionalDepends;
    PluginManifestResources  resources;
    PluginManifestInterfaces interfaces;
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
        XX_LOGE("[client_plugin] `{}` has no readable plugin.yaml", path);
        co_return nullptr;
    }
    if (name.empty()) {
        XX_LOGE("[client_plugin] `{}` manifest has no name", path);
        co_return nullptr;
    }
    const pluginxx::PluginxxKindSpec* spec = kindRegistry().find(kind);
    if (spec == nullptr || spec->frameworkOwned() || spec->loadCapability.empty()) {
        XX_LOGE(
            "[client_plugin] `{}` load failed: kind `{}` has no runner (install a plugin that "
            "provides it)",
            name,
            kind
        );
        co_return nullptr;
    }
    if (apiVersion > 0 && spec->minInterfaceVersion > 0
        && apiVersion < spec->minInterfaceVersion) {
        XX_LOGE(
            "[client_plugin] `{}` declares api_version {} but kind `{}` requires >= {}",
            name,
            apiVersion,
            kind,
            spec->minInterfaceVersion
        );
        co_return nullptr;
    }
    // 必选依赖已在调用方检查过一次; 这里再查一遍 (直连调用不经过它)
    for (const auto& d : depends) {
        if (plugins_.count(d) == 0) {
            XX_LOGE("[client_plugin] `{}` depends on missing plugin `{}`", name, d);
            co_return nullptr;
        }
    }
    if (!reservePluginName(name)) {
        XX_LOGE("[client_plugin] duplicate or currently loading plugin name `{}`", name);
        co_return nullptr;
    }
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
        releasePluginName(name);
        co_return nullptr;
    }
    inst->args            = cfg ? cfg->args : utilxx_base::Json::object();
    inst->configPath      = cfg ? cfg->configPath : std::string{};
    inst->depends         = std::move(depends);
    inst->optionalDepends = std::move(optionalDepends);
    inst->interfaces      = std::move(interfaces);
    inst->kind            = kind;
    plugins_[std::string{name}] = inst;

    std::string reason;
    if (!co_await startInterpretedInstance(inst, reason)) {
        XX_LOGE("[client_plugin] `{}` load failed: {}", name, reason);
        co_await rollbackLoadAsync(inst, /*closeHandle=*/false);
        co_return nullptr;
    }
    finishLoad(inst, resources);
    XX_LOGI("[client_plugin] loaded: {} ({}) as kind `{}`", name, version, kind);
    co_return inst;
}

} // namespace plugin
} // namespace agentxx
