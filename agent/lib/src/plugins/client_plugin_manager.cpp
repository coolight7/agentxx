/*
 * client_plugin_manager.cpp —— client 侧插件管理器实现
 *
 * 线程模型 (与 agent 侧 PluginManager 一致的无锁单线程模型):
 * - 所有注册表/插件表/会话上下文状态仅 client io 线程读写
 * - UI 注册表 (uiRegistry_) 例外: io 线程写, UI 线程经快照读 (mutex + COW shared_ptr)
 * - dlopen/entry 卸载到内部 thread_pool; entry 的注册动作经 vtable ioCallSync
 *   投递回 io 线程串行执行 (插件无感)
 * - 插件回调 (事件 handler / 命令 execute) 在 io 线程同步调用, 快速返回约定
 * - UI 线程从不直接调用插件代码: 命令触发经 postCommandInvocation 投递
 */
#include "agentxx/plugin/client_plugin_manager.h"
#include "agentxx/plugin/plugin_manager.h" /* NativeLoader (平台 dlopen 封装) */

#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "asio/thread_pool.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>

#include <yaml-cpp/yaml.h>

/// 状态栏项宿主句柄实现 (全局作用域, 与 client_plugin_api.h 的 C 不透明类型
/// 对应 —— vtable 函数签名中的 AgentxxStatusItem 即此类型, 不能在命名空间内
/// 另行定义)
struct AgentxxStatusItem {
    agentxx::plugin::ClientPluginInstance* inst = nullptr;
    std::string                            id;
    std::string                            plugin;
};

/// 面板宿主句柄实现
struct AgentxxPanel {
    agentxx::plugin::ClientPluginInstance* inst = nullptr;
    std::string                            id;
    std::string                            plugin;
};

/// Info 栏段落宿主句柄实现
struct AgentxxInfoSection {
    agentxx::plugin::ClientPluginInstance* inst = nullptr;
    std::string                            id;
    std::string                            plugin;
};

namespace agentxx {
namespace plugin {

using agentxx::agent::PluginConfig;

namespace {

// ==================== 异常兜底宏 (同 plugin_manager.cpp) ====================

#define XX_PLUGIN_CATCH_BEGIN try {
#define XX_PLUGIN_CATCH_END(ret)                                 \
    }                                                            \
    catch (const std::exception& e) {                            \
        XX_LOGE("client plugin vtable exception: {}", e.what()); \
        return (ret);                                            \
    }                                                            \
    catch (...) {                                                \
        XX_LOGE("client plugin vtable unknown exception");       \
        return (ret);                                            \
    }
#define XX_PLUGIN_CATCH_END_VOID()                               \
    }                                                            \
    catch (const std::exception& e) {                            \
        XX_LOGE("client plugin vtable exception: {}", e.what()); \
        return;                                                  \
    }                                                            \
    catch (...) {                                                \
        XX_LOGE("client plugin vtable unknown exception");       \
        return;                                                  \
    }

// ==================== 工具 ====================

/// 从库文件名推断插件名 (libfoo.so → foo; 同 agent 侧 pluginNameFromPath)
std::string clientPluginNameFromPath(const std::string& path) {
    auto base = std::filesystem::path(path).filename().string();
    if (base.size() > 3 && base.compare(0, 3, "lib") == 0) {
        base.erase(0, 3);
    }
    for (const char* ext : {".dylib", ".so", ".dll"}) {
        auto pos = base.find(ext);
        if (pos != std::string::npos) {
            base.erase(pos);
            break;
        }
    }
    return base;
}

/// 从 JSON 提取字符串字段 (缺失/非字符串返回空)
std::string jsonStr(const neograph::json& j, std::string_view key) {
    if (!j.is_object()) {
        return {};
    }
    std::string k{key};
    if (!j.contains(k)) {
        return {};
    }
    try {
        const auto& v = j[k];
        if (v.is_string()) {
            return v.get<std::string>();
        }
    } catch (...) {
    }
    return {};
}

/// 从 JSON 提取 int 字段 (缺失/非数字返回默认)
int jsonInt(const neograph::json& j, std::string_view key, int def) {
    if (!j.is_object()) {
        return def;
    }
    std::string k{key};
    if (!j.contains(k)) {
        return def;
    }
    try {
        const auto& v = j[k];
        if (v.is_number()) {
            return static_cast<int>(v.get<double>());
        }
    } catch (...) {
    }
    return def;
}

/// 解析 plugin.yaml manifest (目录插件; 取 name/entry/depends; 失败返回 false)
bool parseClientManifest(
    const std::filesystem::path& dir,
    std::string&                 name,
    std::string&                 entry,
    std::vector<std::string>&    depends,
    std::vector<std::string>&    optionalDepends
) {
    auto            yamlPath = dir / "plugin.yaml";
    std::error_code ec;
    if (!std::filesystem::exists(yamlPath, ec)) {
        return false;
    }
    try {
        auto node = YAML::LoadFile(yamlPath.string());
        name      = node["name"] ? node["name"].as<std::string>() : "";
        entry     = node["entry"] ? node["entry"].as<std::string>() : "";
        if (auto d = node["depends"]; d && d.IsSequence()) {
            for (const auto& it : d) {
                depends.push_back(it.as<std::string>());
            }
        }
        if (auto d = node["optional_depends"]; d && d.IsSequence()) {
            for (const auto& it : d) {
                optionalDepends.push_back(it.as<std::string>());
            }
        }
        return !name.empty() && !entry.empty();
    } catch (const std::exception& e) {
        XX_LOGE("[client_plugin] parse manifest failed: {}: {}", yamlPath.string(), e.what());
        return false;
    }
}

/// 解析插件动态库路径 (目录插件: manifest entry + 平台扩展名修正 +
/// 配置子目录回退; 直接库路径原样返回)
/// - 返回 false 表示目录插件 manifest 缺失/非法 (调用方应跳过加载)
/// - 非目录路径恒返回 true (libPath = path)
/// - loadNativeAsync 正式加载与 loadConfiguredClientPlugins 入口探测共用,
///   保证两者解析一致 (探测直接 LoadLibrary 目录在 Windows 上恒失败
///   error 126)
bool resolveClientPluginLibPath(
    const std::string&        path,
    std::string&              libPath,
    std::vector<std::string>& depends,
    std::vector<std::string>& optionalDepends
) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
        libPath = path;
        return true;
    }
    std::string manifestName, manifestEntry;
    if (!parseClientManifest(
            std::filesystem::path(path),
            manifestName,
            manifestEntry,
            depends,
            optionalDepends
        )) {
        return false;
    }
    auto entryPath = (std::filesystem::path(path) / manifestEntry).lexically_normal().string();
#if defined(_WIN32)
    if (entryPath.ends_with(".so")) {
        entryPath.replace(entryPath.size() - 3, 3, ".dll");
    }
#elif defined(__APPLE__)
    if (entryPath.ends_with(".so")) {
        entryPath.replace(entryPath.size() - 3, 3, ".dylib");
    }
#endif
    if (!std::filesystem::exists(entryPath, ec)) {
        for (const char* cfg : {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"}) {
            auto candidate
                = (std::filesystem::path(path) / cfg / manifestEntry).lexically_normal().string();
#if defined(_WIN32)
            if (candidate.ends_with(".so")) {
                candidate.replace(candidate.size() - 3, 3, ".dll");
            }
#elif defined(__APPLE__)
            if (candidate.ends_with(".so")) {
                candidate.replace(candidate.size() - 3, 3, ".dylib");
            }
#endif
            if (std::filesystem::exists(candidate, ec)) {
                entryPath = std::move(candidate);
                break;
            }
        }
    }
    libPath = std::move(entryPath);
    return true;
}

/// 解析 action 动作 JSON: {"action": "send"|"toast"|"none", ...}
/// 返回 true 表示 action 字段可识别 (含 none); false 表示非法/空
bool parseCommandAction(const std::string& jsonText, std::string& action) {
    action.clear();
    if (jsonText.empty() || jsonText == "{}") {
        return false; // 空结果 = 已处理完毕, 无动作
    }
    try {
        auto j = neograph::json::parse(jsonText);
        if (!j.is_object()) {
            return false;
        }
        action = jsonStr(j, "action");
        if (action.empty()) {
            return false;
        }
        if (action != "send" && action != "toast" && action != "none") {
            XX_LOGW("[client_plugin] unknown command action `{}`", action);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        XX_LOGE("[client_plugin] invalid command action json: {}", e.what());
        return false;
    }
}

} // namespace (匿名: 工具函数)

// =====================================================================
// ClientPluginInstance
// =====================================================================

ClientPluginInstance::~ClientPluginInstance() {
    // dlHandle 的 dlclose 由 manager 在卸载流程完成后释放 (本析构仅清理状态)
    subscriptions.clear();
    statusItemHandles.clear();
    panelHandles.clear();
    infoSectionHandles.clear();
    subHandles.clear();
}

// =====================================================================
// ClientPluginManager
// =====================================================================

ClientPluginManager::ClientPluginManager(asio::any_io_executor ex) :
    pool_(std::make_unique<asio::thread_pool>(1)),
    uiRegistry_(std::make_shared<const ClientUiRegistry>()),
    ioExecutor_(std::move(ex)),
    ioThreadId_(std::this_thread::get_id()) {}

ClientPluginManager::~ClientPluginManager() {
    shutdownAll();
    if (pool_) {
        pool_->join();
    }
}

// ==================== 装配 ====================

void ClientPluginManager::setUiAdapter(std::shared_ptr<PluginUiAdapter> adapter) {
    uiAdapter_ = std::move(adapter);
}

void ClientPluginManager::setThreadId(std::string threadId) {
    threadId_ = std::move(threadId);
}

// ==================== 生命周期 ====================

asio::awaitable<std::shared_ptr<ClientPluginInstance>>
    ClientPluginManager::loadNativeAsync(std::string path) {
    // ---- 目录插件: 解析 plugin.yaml 取 entry 库路径 (与 agent 侧一致) ----
    // - manifest: name/entry/depends/optional_depends
    // - entry 为库文件名 (相对于插件目录)
    // - entry 平台化: manifest 按 Linux 书写 (libfoo.so), Windows/macOS 下
    //   修正扩展名 (.dll/.dylib); 多配置生成器 (MSVC Debug/Release) 产物位于
    //   配置子目录: entry 按 {dir}/{entry} 找不到时回退 {dir}/{Debug|Release|...}
    // - 解析逻辑与 loadConfiguredClientPlugins 入口探测共用
    //   (resolveClientPluginLibPath), 保证探测与正式加载路径一致
    std::string              libPath = path;
    std::vector<std::string> depends, optionalDepends;
    if (!resolveClientPluginLibPath(path, libPath, depends, optionalDepends)) {
        XX_LOGE("[client_plugin] `{}` missing/invalid plugin.yaml", path);
        co_return nullptr;
    }

    // dlopen 卸载到内部线程池 (避免阻塞 io 线程)
    std::string dlErr;
    void*       handle = co_await agentxx::util::offloadAsync<void*>(
        *pool_,
        [libPath, &dlErr]() -> asio::awaitable<void*> {
            co_return NativeLoader::open(libPath, dlErr);
        }
    );
    if (!handle) {
        XX_LOGE("[client_plugin] load failed: {}: {}", libPath, dlErr);
        co_return nullptr;
    }

    // 元信息 (可选符号)
    std::string name;
    std::string version;
    std::string desc;
    std::string err;
    uint32_t    minCaps = 0;
    if (auto getInfo = reinterpret_cast<AgentxxClientPluginGetInfoFn>(
            NativeLoader::sym(handle, AGENTXX_CLIENT_SYMBOL_GET_INFO, err)
        )) {
        auto info = getInfo();
        if (info) {
            if (info->api_version != AGENTXX_CLIENT_PLUGIN_API_VERSION) {
                XX_LOGE(
                    "[client_plugin] `{}` api_version {} mismatch (host expects {})",
                    path,
                    info->api_version,
                    AGENTXX_CLIENT_PLUGIN_API_VERSION
                );
                NativeLoader::close(handle);
                co_return nullptr;
            }
            name    = std::string{info->name.data ? info->name.data : "", info->name.size};
            version = std::string{info->version.data ? info->version.data : "", info->version.size};
            desc    = std::string{
                info->description.data ? info->description.data : "",
                info->description.size
            };
            minCaps = info->min_ui_caps;
        }
    }
    if (name.empty()) {
        name = clientPluginNameFromPath(libPath);
    }

    // 已存在同名插件: 拒绝 (防重复加载)
    if (plugins_.count(name) > 0) {
        XX_LOGE("[client_plugin] duplicate plugin name `{}`", name);
        NativeLoader::close(handle);
        co_return nullptr;
    }

    // 依赖检查: 必选缺失 → 失败; 可选缺失 → 警告
    for (const auto& d : depends) {
        if (plugins_.count(d) == 0) {
            XX_LOGE("[client_plugin] `{}` depends on missing plugin `{}`", name, d);
            NativeLoader::close(handle);
            co_return nullptr;
        }
    }
    for (const auto& d : optionalDepends) {
        if (plugins_.count(d) == 0) {
            XX_LOGW("[client_plugin] `{}` optional dependency `{}` not installed", name, d);
        }
    }

    auto inst             = std::make_shared<ClientPluginInstance>(name);
    inst->version         = version;
    inst->description     = desc;
    inst->path            = path;
    inst->args            = neograph::json::object();
    inst->dlHandle        = handle;
    inst->depends         = std::move(depends);
    inst->optionalDepends = std::move(optionalDepends);
    inst->manager         = weak_from_this();
    inst->host.opaque     = inst.get();
    inst->host.vtable     = hostVtable();

    // UI 能力最低要求检查
    if (uiAdapter_) {
        uint32_t caps = uiAdapter_->uiCaps();
        if ((caps & minCaps) != minCaps) {
            XX_LOGE(
                "[client_plugin] `{}` requires ui_caps 0x{:x}, host has 0x{:x}",
                name,
                minCaps,
                caps
            );
            NativeLoader::close(handle);
            co_return nullptr;
        }
    } else if (minCaps != 0) {
        XX_LOGE("[client_plugin] `{}` requires ui_caps 0x{:x}, no UI adapter", name, minCaps);
        NativeLoader::close(handle);
        co_return nullptr;
    }

    // entry 入口 (必需): 注册动作经 vtable ioCallSync 回 io 线程执行
    std::string entryErr;
    auto        entryFn = reinterpret_cast<AgentxxClientPluginEntryFn>(
        NativeLoader::sym(handle, AGENTXX_CLIENT_SYMBOL_ENTRY, entryErr)
    );
    if (!entryFn) {
        XX_LOGE("[client_plugin] `{}` missing {}: {}", path, AGENTXX_CLIENT_SYMBOL_ENTRY, entryErr);
        NativeLoader::close(handle);
        co_return nullptr;
    }

    int rc = entryFn(&inst->host, &inst->pluginCtx);
    if (rc != 0) {
        XX_LOGE("[client_plugin] `{}` entry failed (rc={})", name, rc);
        detachAll(inst.get(), false);
        NativeLoader::close(handle);
        co_return nullptr;
    }

    plugins_.emplace(name, inst);
    XX_LOGI("[client_plugin] loaded: {} ({})", name, version);
    co_return inst;
}

asio::awaitable<bool> ClientPluginManager::unloadAsync(std::string_view name) {
    auto inst = find(name);
    if (!inst) {
        XX_LOGW("[client_plugin] unload: not found `{}`", name);
        co_return false;
    }
    if (inst->unloadRequested) {
        co_return false;
    }
    inst->unloadRequested = true;

    // 级联: 必选依赖者先卸载 (先子后父)
    for (const auto& child : reverseRequiredDeps(std::string{name}, true)) {
        co_await unloadAsync(child);
    }

    // 摘除注册 (adapter 通知 UI 移除; 彻底清理)
    detachAll(inst.get(), false);
    plugins_.erase(std::string{name});

    // 等 in-flight 回调归零 (超时放弃, 保持已 detach 状态可稍后重试)
    if (!co_await waitInflightZero(inst, std::chrono::seconds{10})) {
        XX_LOGW("[client_plugin] `{}` inflight not zero, skip dlclose", name);
        co_return false;
    }

    // unload 回调 (业务清理)
    if (inst->dlHandle) {
        std::string err;
        auto        fn = reinterpret_cast<AgentxxClientPluginUnloadFn>(
            NativeLoader::sym(inst->dlHandle, AGENTXX_CLIENT_SYMBOL_UNLOAD, err)
        );
        if (fn) {
            fn(inst->pluginCtx);
        }
        NativeLoader::close(inst->dlHandle);
        inst->dlHandle = nullptr;
    }
    XX_LOGI("[client_plugin] unloaded: {}", inst->name);
    co_return true;
}

void ClientPluginManager::disableImpl(std::string_view name, bool userInitiated) {
    auto inst = find(name);
    if (!inst || !inst->enabled) {
        return;
    }
    if (userInitiated) {
        inst->userDisabled = true;
    }
    inst->enabled = false;

    // 级联禁用依赖者 (先子后父)
    for (const auto& child : reverseRequiredDeps(std::string{name}, true)) {
        disableImpl(child, false);
    }

    // 摘除 UI 注册 (adapter 通知); 注册信息保留, enable 可恢复
    detachAll(inst.get(), true);
    XX_LOGI("[client_plugin] disabled: {}", inst->name);
}

void ClientPluginManager::disable(std::string_view name) {
    disableImpl(name, true);
}

void ClientPluginManager::enableImpl(std::string_view name, bool userInitiated) {
    auto inst = find(name);
    if (!inst || inst->enabled) {
        return;
    }
    if (!userInitiated && inst->userDisabled) {
        return; // 被用户显式禁用: 级联不复活
    }
    inst->enabled = true;
    if (userInitiated) {
        inst->userDisabled = false;
    }

    // 级联启用依赖者 (先父后子)
    for (const auto& d : inst->depends) {
        enableImpl(d, false);
    }

    // 恢复 UI 注册 (注册信息在 disable 时保留): 重建句柄 + 写回注册表 + adapter 通知
    for (const auto& reg : inst->statusItemRegs) {
        auto h    = std::make_shared<AgentxxStatusItem>();
        h->inst   = inst.get();
        h->id     = reg.id;
        h->plugin = reg.plugin;
        {
            std::lock_guard<std::mutex> lock(uiMutex_);
            auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
            bool                        dup = false;
            for (const auto& s : cur->statusItems) {
                if (s.id == reg.id) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                cur->statusItems.push_back(reg);
            }
            uiRegistry_ = std::move(cur);
        }
        inst->statusItemHandles.push_back(h);
        if (uiAdapter_) {
            neograph::json props = neograph::json::object();
            props["text"]        = reg.text;
            uiAdapter_->onStatusItemRegistered(reg.id, props, reg.align, reg.order);
        }
    }
    for (const auto& reg : inst->panelRegs) {
        auto h    = std::make_shared<AgentxxPanel>();
        h->inst   = inst.get();
        h->id     = reg.id;
        h->plugin = reg.plugin;
        {
            std::lock_guard<std::mutex> lock(uiMutex_);
            auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
            bool                        dup = false;
            for (const auto& p : cur->panels) {
                if (p.id == reg.id) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                cur->panels.push_back(reg);
            }
            uiRegistry_ = std::move(cur);
        }
        inst->panelHandles.push_back(h);
        if (uiAdapter_) {
            neograph::json props = neograph::json::object();
            props["title"]       = reg.title;
            uiAdapter_->onPanelRegistered(reg.id, props);
        }
    }
    for (const auto& reg : inst->infoSectionRegs) {
        auto h    = std::make_shared<AgentxxInfoSection>();
        h->inst   = inst.get();
        h->id     = reg.id;
        h->plugin = reg.plugin;
        {
            std::lock_guard<std::mutex> lock(uiMutex_);
            auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
            bool                        dup = false;
            for (const auto& s : cur->infoSections) {
                if (s.id == reg.id) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                cur->infoSections.push_back(reg);
            }
            uiRegistry_ = std::move(cur);
        }
        inst->infoSectionHandles.push_back(h);
        if (uiAdapter_) {
            neograph::json props = neograph::json::object();
            props["title"]       = reg.title;
            uiAdapter_->onInfoSectionRegistered(reg.id, props);
        }
    }
    for (const auto& reg : inst->commandRegs) {
        {
            std::lock_guard<std::mutex> lock(uiMutex_);
            auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
            bool                        dup = false;
            for (const auto& c : cur->commands) {
                if (c.name == reg.name && c.plugin == reg.plugin) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                cur->commands.push_back(reg);
            }
            uiRegistry_ = std::move(cur);
        }
    }
    XX_LOGI("[client_plugin] enabled: {}", inst->name);
}

void ClientPluginManager::enable(std::string_view name) {
    enableImpl(name, true);
}

asio::awaitable<void>
    ClientPluginManager::loadConfiguredClientPlugins(const std::vector<PluginConfig>& plugins) {
    // 预解析各配置项 sides 过滤 + 依赖 (目录插件读 plugin.yaml depends)
    struct Item {
        std::string              path;
        std::string              name; ///< 空 = 无法推导 (不影响排序)
        std::vector<std::string> depends;
    };

    std::vector<Item> items;
    for (const auto& pc : plugins) {
        if (pc.sides == agentxx::agent::PluginSide::Agent) {
            continue; // 属于 agent 侧
        }
        Item it;
        it.path = pc.path;
        if (std::filesystem::is_directory(std::filesystem::path(pc.path))) {
            std::string              name, entry;
            std::vector<std::string> depends, optionalDepends;
            if (parseClientManifest(
                    std::filesystem::path(pc.path),
                    name,
                    entry,
                    depends,
                    optionalDepends
                )) {
                it.name    = name;
                it.depends = std::move(depends);
            }
        } else {
            it.name = clientPluginNameFromPath(pc.path);
        }
        items.push_back(std::move(it));
    }

    // 拓扑排序: 依赖者排在被依赖者之后 (贪心; 环由加载时依赖检查拒绝)
    std::vector<Item> ordered;
    std::vector<bool> done(items.size(), false);
    for (size_t round = 0; round < items.size(); ++round) {
        for (size_t i = 0; i < items.size(); ++i) {
            if (done[i]) {
                continue;
            }
            bool depsOk = true;
            for (const auto& d : items[i].depends) {
                bool found = false;
                for (size_t k = 0; k < items.size(); ++k) {
                    if (!done[k] && items[k].name == d) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    depsOk = false;
                    break;
                }
            }
            if (depsOk) {
                ordered.push_back(items[i]);
                done[i] = true;
            }
        }
    }
    for (size_t i = 0; i < items.size(); ++i) {
        if (!done[i]) {
            XX_LOGW(
                "[client_plugin] dependency cycle or missing dep for `{}`, skipped",
                items[i].path
            );
        }
    }

    // 依次加载 (Auto 无 client 入口时跳过, 不报错)
    for (const auto& it : ordered) {
        if (it.name.empty()) {
            continue;
        }
        if (plugins_.count(it.name) > 0) {
            continue; // 已加载
        }
        // 探测 client 入口: 无则视为纯 agent 插件跳过
        // - 目录插件先解析 manifest entry 库路径再探测 (直接 LoadLibrary
        //   目录在 Windows 上恒失败 error 126; 解析逻辑与 loadNativeAsync
        //   共用 resolveClientPluginLibPath, 保证探测/加载路径一致)
        std::string              probePath;
        std::vector<std::string> probeDeps, probeOptDeps;
        if (!resolveClientPluginLibPath(it.path, probePath, probeDeps, probeOptDeps)) {
            XX_LOGE("[client_plugin] `{}` missing/invalid plugin.yaml", it.path);
            continue;
        }
        std::string dlErr;
        void*       handle = NativeLoader::open(probePath, dlErr);
        if (!handle) {
            XX_LOGE("[client_plugin] load failed: {}: {}", probePath, dlErr);
            continue;
        }
        std::string symErr;
        void*       entry = NativeLoader::sym(handle, AGENTXX_CLIENT_SYMBOL_ENTRY, symErr);
        NativeLoader::close(handle); // 探测后立即关闭 (正式加载重新 dlopen)
        if (!entry) {
            XX_LOGI("[client_plugin] `{}` has no client entry, skipped (agent-only)", it.name);
            continue;
        }
        co_await loadNativeAsync(it.path);
    }
}

void ClientPluginManager::shutdownAll() {
    // 逆序卸载 (依赖者先; 加载时已拓扑排序)
    std::vector<std::shared_ptr<ClientPluginInstance>> insts;
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        insts.push_back(it->second);
    }
    plugins_.clear();
    for (auto& inst : insts) {
        detachAll(inst.get(), false);
        if (inst->dlHandle) {
            std::string err;
            auto        fn = reinterpret_cast<AgentxxClientPluginUnloadFn>(
                NativeLoader::sym(inst->dlHandle, AGENTXX_CLIENT_SYMBOL_UNLOAD, err)
            );
            if (fn) {
                fn(inst->pluginCtx);
            }
            NativeLoader::close(inst->dlHandle);
            inst->dlHandle = nullptr;
        }
        XX_LOGI("[client_plugin] shutdown: {}", inst->name);
    }
}

// ==================== 查询 ====================

std::vector<ClientPluginManager::PluginListView> ClientPluginManager::list() const {
    std::vector<PluginListView> out;
    for (const auto& [name, inst] : plugins_) {
        (void)name;
        PluginListView v;
        v.name            = inst->name;
        v.version         = inst->version;
        v.description     = inst->description;
        v.path            = inst->path;
        v.enabled         = inst->enabled;
        v.inflight        = inst->inflight.load(std::memory_order_relaxed);
        v.depends         = inst->depends;
        v.optionalDepends = inst->optionalDepends;
        for (const auto& s : inst->statusItemRegs) {
            v.statusItems.push_back(s.id);
        }
        for (const auto& p : inst->panelRegs) {
            v.panels.push_back(p.id);
        }
        for (const auto& s : inst->infoSectionRegs) {
            v.infoSections.push_back(s.id);
        }
        for (const auto& c : inst->commandRegs) {
            v.commands.push_back(c.name);
        }
        out.push_back(std::move(v));
    }
    return out;
}

std::shared_ptr<ClientPluginInstance> ClientPluginManager::find(std::string_view name) const {
    auto it = plugins_.find(std::string{name});
    return it == plugins_.end() ? nullptr : it->second;
}

// ==================== UI 注册表 ====================

std::shared_ptr<const ClientUiRegistry> ClientPluginManager::uiRegistrySnapshot() const {
    std::lock_guard<std::mutex> lock(uiMutex_);
    return uiRegistry_;
}

bool ClientPluginManager::hasCommand(std::string_view name) const {
    std::lock_guard<std::mutex> lock(uiMutex_);
    for (const auto& c : uiRegistry_->commands) {
        if (c.name == name) {
            return true;
        }
    }
    return false;
}

void ClientPluginManager::postCommandInvocation(std::string name, std::string argsJson) {
    auto self = shared_from_this();
    postToIo([self, name = std::move(name), args = std::move(argsJson)]() mutable {
        self->invokeCommand(name, args);
    });
}

void ClientPluginManager::invokeCommand(const std::string& name, const std::string& argsJson) {
    // 查表 (io 线程)
    const ClientCommand*                  cmd = nullptr;
    std::shared_ptr<ClientPluginInstance> inst;
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        for (const auto& c : uiRegistry_->commands) {
            if (c.name == name) {
                cmd  = &c;
                inst = find(c.plugin);
                break;
            }
        }
    }
    if (!cmd || !cmd->execute || !inst || !inst->enabled) {
        XX_LOGW("[client_plugin] command `{}` not found or plugin disabled", name);
        return;
    }

    ClientPluginInstance::InflightGuard guard(inst.get());
    char*                               err = nullptr;
    char* out = cmd->execute(cmd->ud, agentxx_plugin_sv(argsJson.data(), argsJson.size()), &err);
    std::string actionJson;
    if (!out) {
        XX_LOGW("[client_plugin] command `{}` failed: {}", name, err ? err : "(no error message)");
    } else {
        actionJson.assign(out);
    }
    if (err) {
        inst->host.vtable->free(err);
    }
    if (out) {
        inst->host.vtable->free(out);
    }
    dispatchCommandAction(actionJson);
}

void ClientPluginManager::dispatchCommandAction(const std::string& actionJson) {
    if (!uiAdapter_) {
        return;
    }
    std::string action;
    if (!parseCommandAction(actionJson, action)) {
        return; // 空/非法 = 已处理完毕
    }
    if (action == "none") {
        return;
    }
    if (action == "toast") {
        try {
            auto j = neograph::json::parse(actionJson);
            uiAdapter_->onToast(jsonStr(j, "text"), jsonInt(j, "level", 0));
        } catch (...) {
            uiAdapter_->onToast("(plugin toast)", 0);
        }
        return;
    }
    if (action == "send") {
        try {
            auto j = neograph::json::parse(actionJson);
            uiAdapter_->sendPluginMessage(jsonStr(j, "text"));
        } catch (const std::exception& e) {
            XX_LOGE("[client_plugin] invalid send action: {}", e.what());
        }
        return;
    }
}

// ==================== 会话上下文 ====================

std::string ClientPluginManager::clientStateJson() const {
    neograph::json j     = neograph::json::object();
    j["threadId"]        = threadId_;
    j["connState"]       = connState_;
    j["startupProgress"] = startupProgress_;
    j["uiCaps"]          = uiAdapter_ ? static_cast<int>(uiAdapter_->uiCaps()) : 0;
    return j.dump();
}

// ==================== io 线程投递 ====================

bool ClientPluginManager::isIoThread() const {
    return ioThreadId_ == std::this_thread::get_id();
}

void ClientPluginManager::postToIo(std::function<void()> fn) const {
    if (isIoThread()) {
        fn();
    } else {
        asio::post(ioExecutor_, std::move(fn));
    }
}

// ==================== ClientEventSink 实现 ====================

void ClientPluginManager::onReady() {
    neograph::json j = neograph::json::object();
    j["uiCaps"]      = uiAdapter_ ? static_cast<int>(uiAdapter_->uiCaps()) : 0;
    j["threadId"]    = threadId_;
    dispatchEvent(AGENTXX_CLIENT_EVT_READY, j.dump());
}

void ClientPluginManager::onConnStateChanged(std::string_view state, std::string_view progress) {
    connState_           = std::string{state};
    startupProgress_     = std::string{progress};
    neograph::json j     = neograph::json::object();
    j["connState"]       = connState_;
    j["startupProgress"] = startupProgress_;
    dispatchEvent(AGENTXX_CLIENT_EVT_CONN_STATE, j.dump());
}

void ClientPluginManager::onUserInput(std::string_view threadId, std::string_view text) {
    neograph::json j = neograph::json::object();
    j["threadId"]    = std::string{threadId};
    j["text"]        = std::string{text};
    dispatchEvent(AGENTXX_CLIENT_EVT_USER_INPUT, j.dump());
}

void ClientPluginManager::onDelta(const agentxx::agent::Delta& delta) {
    dispatchEvent(AGENTXX_CLIENT_EVT_DELTA, agentxx::agent::io::deltaToJson(delta).dump());
}

void ClientPluginManager::onTurnResult(const agentxx::agent::WireTurnResult& result) {
    neograph::json j = neograph::json::object();
    j["threadId"]    = result.threadId;
    j["hasError"]    = result.hasError;
    j["interrupted"] = result.interrupted;
    if (!result.errorMessage.empty()) {
        j["errorMessage"] = result.errorMessage;
    }
    j["startTimeMs"] = result.startTimeMs;
    j["durationMs"]  = result.durationMs;
    dispatchEvent(AGENTXX_CLIENT_EVT_TURN_END, j.dump());
}

void ClientPluginManager::onSessionSwitched(std::string_view threadId) {
    threadId_        = std::string{threadId};
    neograph::json j = neograph::json::object();
    j["threadId"]    = threadId_;
    dispatchEvent(AGENTXX_CLIENT_EVT_SESSION_SWITCH, j.dump());
}

void ClientPluginManager::onPluginData(const agentxx::agent::WirePluginData& data) {
    neograph::json j = neograph::json::object();
    j["plugin"]      = data.plugin;
    j["event"]       = data.event;
    j["data"]        = data.data;
    dispatchEvent(AGENTXX_CLIENT_EVT_PLUGIN_DATA, j.dump());
}

// ==================== 内部 ====================

asio::awaitable<bool> ClientPluginManager::waitInflightZero(
    const std::shared_ptr<ClientPluginInstance>& inst,
    std::chrono::milliseconds                    timeout
) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (inst->inflight.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return false;
        }
        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds{20});
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return true;
}

void ClientPluginManager::detachAll(ClientPluginInstance* inst, bool keepInfo) {
    if (!inst) {
        return;
    }
    // 从 UI 注册表摘除 (adapter 通知)
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        reg = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        auto&                       st  = reg->statusItems;
        for (auto it = st.begin(); it != st.end();) {
            if (it->plugin == inst->name) {
                it = st.erase(it);
            } else {
                ++it;
            }
        }
        auto& pn = reg->panels;
        for (auto it = pn.begin(); it != pn.end();) {
            if (it->plugin == inst->name) {
                it = pn.erase(it);
            } else {
                ++it;
            }
        }
        auto& inf = reg->infoSections;
        for (auto it = inf.begin(); it != inf.end();) {
            if (it->plugin == inst->name) {
                it = inf.erase(it);
            } else {
                ++it;
            }
        }
        auto& cm = reg->commands;
        for (auto it = cm.begin(); it != cm.end();) {
            if (it->plugin == inst->name) {
                it = cm.erase(it);
            } else {
                ++it;
            }
        }
        uiRegistry_ = std::move(reg);
    }
    // adapter 信号
    if (uiAdapter_) {
        for (const auto& s : inst->statusItemRegs) {
            uiAdapter_->onStatusItemRemoved(s.id);
        }
        for (const auto& p : inst->panelRegs) {
            uiAdapter_->onPanelRemoved(p.id);
        }
        for (const auto& is : inst->infoSectionRegs) {
            uiAdapter_->onInfoSectionRemoved(is.id);
        }
    }
    // 注意: 句柄 (statusItemHandles/panelHandles/infoSectionHandles/subHandles)
    // 不在此释放 —— 插件的 unload 回调 (detachAll 之后、实例析构之前调用)
    // 可能主动反注册 (unregister_status_item 等), 句柄必须存活到实例析构;
    // 由 ~ClientPluginInstance 统一释放 (实例在 plugins_.erase 后所有
    // shared_ptr 释放时析构, 晚于 unload 回调)
    if (!keepInfo) {
        // 彻底清理: 注册信息随实例释放 (unload/shutdown)
        inst->statusItemRegs.clear();
        inst->panelRegs.clear();
        inst->infoSectionRegs.clear();
        inst->commandRegs.clear();
        inst->subscriptions.clear();
    }
}

std::vector<std::string>
    ClientPluginManager::reverseRequiredDeps(const std::string& target, bool onlyEnabled) const {
    std::vector<std::string> out;
    for (const auto& [name, inst] : plugins_) {
        (void)name;
        if (onlyEnabled && !inst->enabled) {
            continue;
        }
        for (const auto& d : inst->depends) {
            if (d == target) {
                out.push_back(inst->name);
                break;
            }
        }
    }
    return out;
}

void ClientPluginManager::dispatchEvent(int event, const std::string& payloadJson) {
    // 快照订阅列表 (分发在 io 线程串行, 期间不会卸载/改 subscriptions)
    struct SubRef {
        ClientPluginInstance*               inst;
        ClientPluginInstance::Subscription* sub;
    };

    std::vector<SubRef> refs;
    for (const auto& [name, inst] : plugins_) {
        (void)name;
        if (!inst->enabled) {
            continue;
        }
        for (auto& s : inst->subscriptions) {
            if (s.alive && s.event == event) {
                refs.push_back(SubRef{inst.get(), &s});
            }
        }
    }
    for (const auto& ref : refs) {
        ClientPluginInstance::InflightGuard guard(ref.inst);
        ref.sub->handler(agentxx_plugin_sv(payloadJson.data(), payloadJson.size()), ref.sub->ud);
    }
}

// =====================================================================
// host vtable (C ABI)
// =====================================================================

namespace {

ClientPluginInstance* clientInstOf(const AgentxxClientHost* host) {
    return (host && host->opaque) ? static_cast<ClientPluginInstance*>(host->opaque) : nullptr;
}

ClientPluginManager* clientMgrOf(const AgentxxClientHost* host) {
    auto inst = clientInstOf(host);
    return inst ? inst->manager.lock().get() : nullptr;
}

/// 在 io 线程执行并同步等待结果 (调用方为 io 线程时直接执行)
template<typename T>
static T clientIoCallSync(ClientPluginManager* mgr, std::function<T()> fn) {
    if (!mgr) {
        throw std::runtime_error("client plugin manager released");
    }
    if (mgr->isIoThread()) {
        return fn();
    }
    std::promise<T> p;
    auto            fut = p.get_future();
    mgr->postToIo([&p, fn = std::move(fn)]() mutable {
        try {
            p.set_value(fn());
        } catch (...) {
            p.set_exception(std::current_exception());
        }
    });
    return fut.get();
}

static void clientIoCallSyncVoid(ClientPluginManager* mgr, std::function<void()> fn) {
    if (!mgr) {
        return;
    }
    if (mgr->isIoThread()) {
        fn();
        return;
    }
    std::promise<void> p;
    auto               fut = p.get_future();
    mgr->postToIo([&p, fn = std::move(fn)]() mutable {
        try {
            fn();
            p.set_value();
        } catch (...) {
            p.set_exception(std::current_exception());
        }
    });
    fut.get();
}

// ---- 内存 ----

void* xx_calloc(size_t size) {
    return ::malloc(size);
}

void xx_cfree(void* ptr) {
    ::free(ptr);
}

char* xx_cstrdup(const char* s) {
    if (!s) {
        return nullptr;
    }
    size_t n = std::strlen(s) + 1;
    char*  p = static_cast<char*>(::malloc(n));
    if (p) {
        std::memcpy(p, s, n);
    }
    return p;
}

// ---- 日志 / JSON ----

void xx_clog(const AgentxxClientHost* host, int level, AgentxxPluginStringView msg) {
    (void)host;
    std::string_view s{msg.data ? msg.data : "", msg.size};
    switch (level) {
        case 0:
            XX_LOGT("[client_plugin] {}", s);
            break;
        case 1:
            XX_LOGD("[client_plugin] {}", s);
            break;
        case 3:
            XX_LOGW("[client_plugin] {}", s);
            break;
        case 4:
            XX_LOGE("[client_plugin] {}", s);
            break;
        default:
            XX_LOGI("[client_plugin] {}", s);
            break;
    }
}

char* xx_cjson_get_string(
    const AgentxxClientHost* host,
    AgentxxPluginStringView  json,
    AgentxxPluginStringView  key
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = clientMgrOf(host);
    if (!mgr) {
        return nullptr;
    }
    std::string jsonText{json.data ? json.data : "", json.size};
    std::string keyStr{key.data ? key.data : "", key.size};
    return clientIoCallSync<char*>(mgr, [&]() -> char* {
        try {
            auto j = neograph::json::parse(jsonText);
            auto v = jsonStr(j, keyStr);
            if (v.empty() && !j.contains(keyStr)) {
                return nullptr;
            }
            return xx_cstrdup(v.c_str());
        } catch (...) {
            return nullptr;
        }
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

char* xx_cjson_escape(const AgentxxClientHost* host, AgentxxPluginStringView s) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = clientMgrOf(host);
    if (!mgr) {
        return nullptr;
    }
    std::string text{s.data ? s.data : "", s.size};
    return clientIoCallSync<char*>(mgr, [&]() -> char* {
        neograph::json j = std::string{text};
        return xx_cstrdup(j.dump().c_str());
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

// ---- 能力协商 ----

uint32_t xx_cui_caps(const AgentxxClientHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = clientMgrOf(host);
    if (!mgr || !mgr->uiAdapter()) {
        return 0;
    }
    return mgr->uiAdapter()->uiCaps();
    XX_PLUGIN_CATCH_END(0)
}

// ---- 状态栏项 ----

AgentxxStatusItem* xx_cregister_status_item(
    const AgentxxClientHost* host,
    AgentxxPluginStringView  id,
    AgentxxPluginStringView  initial_json,
    int                      align,
    int                      order
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    std::string idStr{id.data ? id.data : "", id.size};
    std::string jsonStr{initial_json.data ? initial_json.data : "", initial_json.size};
    if (idStr.empty()) {
        return nullptr;
    }
    return clientIoCallSync<AgentxxStatusItem*>(mgr, [&]() -> AgentxxStatusItem* {
        return static_cast<AgentxxStatusItem*>(
            mgr->registerStatusItem(inst, idStr.c_str(), jsonStr.c_str(), align, order)
        );
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

int xx_cupdate_status_item(
    const AgentxxClientHost* host,
    AgentxxStatusItem*       item,
    AgentxxPluginStringView  json
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst || !item) {
        return -1;
    }
    std::string jsonStr{json.data ? json.data : "", json.size};
    return clientIoCallSync<int>(mgr, [&]() -> int {
        return mgr->updateStatusItem(inst, item, jsonStr.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

void xx_cunregister_status_item(const AgentxxClientHost* host, AgentxxStatusItem* item) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst || !item) {
        return;
    }
    clientIoCallSyncVoid(mgr, [&]() {
        mgr->unregisterStatusItem(inst, item);
    });
    XX_PLUGIN_CATCH_END_VOID()
}

// ---- 面板 ----

AgentxxPanel* xx_cregister_panel(
    const AgentxxClientHost* host,
    AgentxxPluginStringView  id,
    AgentxxPluginStringView  props_json
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    std::string idStr{id.data ? id.data : "", id.size};
    std::string props{props_json.data ? props_json.data : "", props_json.size};
    if (idStr.empty()) {
        return nullptr;
    }
    return clientIoCallSync<AgentxxPanel*>(mgr, [&]() -> AgentxxPanel* {
        return static_cast<AgentxxPanel*>(mgr->registerPanel(inst, idStr.c_str(), props.c_str()));
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

int xx_cupdate_panel(
    const AgentxxClientHost* host,
    AgentxxPanel*            panel,
    AgentxxPluginStringView  items_json
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst || !panel) {
        return -1;
    }
    std::string items{items_json.data ? items_json.data : "", items_json.size};
    return clientIoCallSync<int>(mgr, [&]() -> int {
        return mgr->updatePanel(inst, panel, items.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

void xx_cunregister_panel(const AgentxxClientHost* host, AgentxxPanel* panel) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst || !panel) {
        return;
    }
    clientIoCallSyncVoid(mgr, [&]() {
        mgr->unregisterPanel(inst, panel);
    });
    XX_PLUGIN_CATCH_END_VOID()
}

// ---- Info 栏段落 ----

AgentxxInfoSection* xx_cregister_info_section(
    const AgentxxClientHost* host,
    AgentxxPluginStringView  id,
    AgentxxPluginStringView  props_json
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    std::string idStr{id.data ? id.data : "", id.size};
    std::string props{props_json.data ? props_json.data : "", props_json.size};
    if (idStr.empty()) {
        return nullptr;
    }
    return clientIoCallSync<AgentxxInfoSection*>(mgr, [&]() -> AgentxxInfoSection* {
        return static_cast<AgentxxInfoSection*>(
            mgr->registerInfoSection(inst, idStr.c_str(), props.c_str())
        );
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

int xx_cupdate_info_section(
    const AgentxxClientHost* host,
    AgentxxInfoSection*      section,
    AgentxxPluginStringView  items_json
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst || !section) {
        return -1;
    }
    std::string items{items_json.data ? items_json.data : "", items_json.size};
    return clientIoCallSync<int>(mgr, [&]() -> int {
        return mgr->updateInfoSection(inst, section, items.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

void xx_cunregister_info_section(const AgentxxClientHost* host, AgentxxInfoSection* section) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst || !section) {
        return;
    }
    clientIoCallSyncVoid(mgr, [&]() {
        mgr->unregisterInfoSection(inst, section);
    });
    XX_PLUGIN_CATCH_END_VOID()
}

// ---- 命令 ----

int xx_cregister_command(
    const AgentxxClientHost* host,
    AgentxxPluginStringView  name,
    AgentxxPluginStringView  description,
    char* (*execute)(void*, AgentxxPluginStringView, char**),
    void* ud
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst || !execute) {
        return -1;
    }
    std::string nameStr{name.data ? name.data : "", name.size};
    std::string descStr{description.data ? description.data : "", description.size};
    if (nameStr.empty()) {
        return -1;
    }
    return clientIoCallSync<int>(mgr, [&]() -> int {
        return mgr->registerCommand(inst, nameStr.c_str(), descStr.c_str(), execute, ud);
    });
    XX_PLUGIN_CATCH_END(-1)
}

int xx_cunregister_command(const AgentxxClientHost* host, AgentxxPluginStringView name) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst) {
        return -1;
    }
    std::string nameStr{name.data ? name.data : "", name.size};
    return clientIoCallSync<int>(mgr, [&]() -> int {
        return mgr->unregisterCommand(inst, nameStr.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

// ---- toast ----

void xx_cshow_toast(const AgentxxClientHost* host, AgentxxPluginStringView text, int level) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = clientMgrOf(host);
    if (!mgr || !mgr->uiAdapter()) {
        return;
    }
    std::string textStr{text.data ? text.data : "", text.size};
    clientIoCallSyncVoid(mgr, [&]() {
        mgr->uiAdapter()->onToast(textStr, level);
    });
    XX_PLUGIN_CATCH_END_VOID()
}

// ---- 事件订阅 ----

AgentxxSubscription* xx_csubscribe(
    const AgentxxClientHost* host,
    int                      event,
    void (*handler)(AgentxxPluginStringView, void*),
    void* ud
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst || !handler) {
        return nullptr;
    }
    if (event < 0 || event >= AGENTXX_CLIENT_EVT_COUNT) {
        return nullptr;
    }
    return clientIoCallSync<AgentxxSubscription*>(mgr, [&]() -> AgentxxSubscription* {
        return static_cast<AgentxxSubscription*>(mgr->subscribe(inst, event, handler, ud));
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

void xx_cunsubscribe(AgentxxSubscription* sub) {
    XX_PLUGIN_CATCH_BEGIN
    if (!sub) {
        return;
    }
    auto impl = reinterpret_cast<ClientSubscriptionImpl*>(sub);
    auto mgr  = impl->inst ? impl->inst->manager.lock().get() : nullptr;
    if (mgr) {
        clientIoCallSyncVoid(mgr, [&]() {
            mgr->unsubscribe(reinterpret_cast<AgentxxSubscription*>(impl));
        });
    }
    impl->inst = nullptr;
    impl->sub  = nullptr;
    XX_PLUGIN_CATCH_END_VOID()
}

// ---- 会话上下文 ----

char* xx_cget_client_state(const AgentxxClientHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = clientMgrOf(host);
    if (!mgr) {
        return nullptr;
    }
    return clientIoCallSync<char*>(mgr, [&]() -> char* {
        auto s = mgr->clientStateJson();
        return xx_cstrdup(s.c_str());
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

// ---- 会话操作 ----

int xx_csend_user_input(
    const AgentxxClientHost* host,
    AgentxxPluginStringView  thread_id,
    AgentxxPluginStringView  text
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(text)) {
        return -1;
    }
    std::string tid{thread_id.data ? thread_id.data : "", thread_id.size};
    std::string txt{text.data ? text.data : "", text.size};
    return clientIoCallSync<int>(mgr, [&]() -> int {
        mgr->sendUserInputToPeer(inst, tid.c_str(), txt.c_str());
        return 0;
    });
    XX_PLUGIN_CATCH_END(-1)
}

void xx_crequest_cancel(const AgentxxClientHost* host, AgentxxPluginStringView thread_id) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst) {
        return;
    }
    std::string tid{thread_id.data ? thread_id.data : "", thread_id.size};
    clientIoCallSyncVoid(mgr, [&]() {
        mgr->requestCancelToPeer(inst, tid.c_str());
    });
    XX_PLUGIN_CATCH_END_VOID()
}

// ---- 跨端数据 ----

int xx_csend_plugin_data(
    const AgentxxClientHost* host,
    AgentxxPluginStringView  event,
    AgentxxPluginStringView  json
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(event)) {
        return -1;
    }
    std::string ev{event.data ? event.data : "", event.size};
    std::string data{json.data ? json.data : "", json.size};
    return clientIoCallSync<int>(mgr, [&]() -> int {
        return mgr->sendPluginDataToPeer(inst, ev.c_str(), data.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

// ---- 自描述 ----

char* xx_cget_own_info(const AgentxxClientHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    return clientIoCallSync<char*>(mgr, [&]() -> char* {
        auto s = mgr->getOwnInfoJson(inst);
        return xx_cstrdup(s.c_str());
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

char* xx_cget_plugin_args(const AgentxxClientHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = clientMgrOf(host);
    auto inst = clientInstOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    return clientIoCallSync<char*>(mgr, [&]() -> char* {
        auto s = mgr->getPluginArgsJson(inst);
        return xx_cstrdup(s.c_str());
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

const AgentxxClientHostVtable g_clientHostVtable = {
    /* alloc */ xx_calloc,
    /* free */ xx_cfree,
    /* strdup */ xx_cstrdup,
    /* ui_caps */ xx_cui_caps,
    /* register_status_item */ xx_cregister_status_item,
    /* update_status_item */ xx_cupdate_status_item,
    /* unregister_status_item */ xx_cunregister_status_item,
    /* register_panel */ xx_cregister_panel,
    /* update_panel */ xx_cupdate_panel,
    /* unregister_panel */ xx_cunregister_panel,
    /* register_info_section */ xx_cregister_info_section,
    /* update_info_section */ xx_cupdate_info_section,
    /* unregister_info_section */ xx_cunregister_info_section,
    /* register_command */ xx_cregister_command,
    /* unregister_command */ xx_cunregister_command,
    /* show_toast */ xx_cshow_toast,
    /* subscribe */ xx_csubscribe,
    /* unsubscribe */ xx_cunsubscribe,
    /* get_client_state */ xx_cget_client_state,
    /* send_user_input */ xx_csend_user_input,
    /* request_cancel */ xx_crequest_cancel,
    /* send_plugin_data */ xx_csend_plugin_data,
    /* get_own_info */ xx_cget_own_info,
    /* get_plugin_args */ xx_cget_plugin_args,
    /* log */ xx_clog,
    /* json_get_string */ xx_cjson_get_string,
    /* json_escape */ xx_cjson_escape,
};

} // namespace

const AgentxxClientHostVtable* ClientPluginManager::hostVtable() {
    return &g_clientHostVtable;
}

// =====================================================================
// ClientPluginManager 内部实现 (vtable 强类型入口)
// =====================================================================

void* ClientPluginManager::registerStatusItem(
    ClientPluginInstance* inst,
    const char*           id,
    const char*           json,
    int                   align,
    int                   order
) {
    if (!inst) {
        return nullptr;
    }
    if (uiAdapter_ && !(uiAdapter_->uiCaps() & AGENTXX_UI_CAP_STATUS_ITEM)) {
        XX_LOGW("[client_plugin] status item `{}` rejected: UI has no STATUS_ITEM cap", id);
        return nullptr;
    }
    // id 冲突检查 (全局)
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        for (const auto& s : uiRegistry_->statusItems) {
            if (s.id == id) {
                XX_LOGW("[client_plugin] status item id `{}` already registered", id);
                return nullptr;
            }
        }
    }
    // 解析 initial_json → text
    std::string    text;
    neograph::json props;
    try {
        props = neograph::json::parse(json ? json : "{}");
        text  = jsonStr(props, "text");
    } catch (...) {
        text.clear();
    }
    if (text.empty()) {
        text = id;
    }

    auto handle    = std::make_shared<AgentxxStatusItem>();
    handle->inst   = inst;
    handle->id     = id;
    handle->plugin = inst->name;

    ClientStatusItem reg;
    reg.plugin = inst->name;
    reg.id     = handle->id;
    reg.text   = text;
    reg.align  = align;
    reg.order  = order;
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        cur->statusItems.push_back(reg);
        uiRegistry_ = std::move(cur);
    }
    inst->statusItemRegs.push_back(std::move(reg));
    inst->statusItemHandles.push_back(handle);
    if (uiAdapter_) {
        uiAdapter_->onStatusItemRegistered(handle->id, props, align, order);
    }
    return handle.get();
}

int ClientPluginManager::updateStatusItem(
    ClientPluginInstance* inst,
    void*                 item,
    const char*           json
) {
    auto h = static_cast<AgentxxStatusItem*>(item);
    if (!inst || !h) {
        return -1;
    }
    neograph::json props;
    std::string    text;
    try {
        props = neograph::json::parse(json ? json : "{}");
        text  = jsonStr(props, "text");
    } catch (...) {
        text.clear();
    }
    if (text.empty()) {
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        for (auto& s : cur->statusItems) {
            if (s.id == h->id) {
                s.text = text;
                break;
            }
        }
        uiRegistry_ = std::move(cur);
    }
    for (auto& s : inst->statusItemRegs) {
        if (s.id == h->id) {
            s.text = text;
            break;
        }
    }
    if (uiAdapter_) {
        uiAdapter_->onStatusItemUpdated(h->id, props);
    }
    return 0;
}

void ClientPluginManager::unregisterStatusItem(ClientPluginInstance* inst, void* item) {
    auto h = static_cast<AgentxxStatusItem*>(item);
    if (!inst || !h) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        auto&                       vec = cur->statusItems;
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if (it->id == h->id) {
                vec.erase(it);
                break;
            }
        }
        uiRegistry_ = std::move(cur);
    }
    auto& regs = inst->statusItemRegs;
    regs.erase(
        std::remove_if(
            regs.begin(),
            regs.end(),
            [&](const auto& s) {
                return s.id == h->id;
            }
        ),
        regs.end()
    );
    if (uiAdapter_) {
        uiAdapter_->onStatusItemRemoved(h->id);
    }
    h->inst = nullptr; // 句柄失效
}

void* ClientPluginManager::registerPanel(
    ClientPluginInstance* inst,
    const char*           id,
    const char*           props_json
) {
    if (!inst) {
        return nullptr;
    }
    if (uiAdapter_ && !(uiAdapter_->uiCaps() & AGENTXX_UI_CAP_PANEL)) {
        XX_LOGW("[client_plugin] panel `{}` rejected: UI has no PANEL cap", id);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        for (const auto& p : uiRegistry_->panels) {
            if (p.id == id) {
                XX_LOGW("[client_plugin] panel id `{}` already registered", id);
                return nullptr;
            }
        }
    }
    neograph::json props;
    std::string    title;
    try {
        props = neograph::json::parse(props_json ? props_json : "{}");
        title = jsonStr(props, "title");
    } catch (...) {
        title.clear();
    }
    if (title.empty()) {
        title = id;
    }

    auto handle    = std::make_shared<AgentxxPanel>();
    handle->inst   = inst;
    handle->id     = id;
    handle->plugin = inst->name;

    ClientPanel reg;
    reg.plugin = inst->name;
    reg.id     = handle->id;
    reg.title  = title;
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        cur->panels.push_back(reg);
        uiRegistry_ = std::move(cur);
    }
    inst->panelRegs.push_back(std::move(reg));
    inst->panelHandles.push_back(handle);
    if (uiAdapter_) {
        uiAdapter_->onPanelRegistered(handle->id, props);
    }
    return handle.get();
}

int ClientPluginManager::updatePanel(
    ClientPluginInstance* inst,
    void*                 panel,
    const char*           items_json
) {
    auto h = static_cast<AgentxxPanel*>(panel);
    if (!inst || !h) {
        return -1;
    }
    neograph::json items = neograph::json::array();
    try {
        auto j = neograph::json::parse(items_json ? items_json : "{}");
        if (j.contains("items") && j["items"].is_array()) {
            items = j["items"];
        }
    } catch (...) {
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        for (auto& p : cur->panels) {
            if (p.id == h->id) {
                p.items = items;
                break;
            }
        }
        uiRegistry_ = std::move(cur);
    }
    for (auto& p : inst->panelRegs) {
        if (p.id == h->id) {
            p.items = items;
            break;
        }
    }
    neograph::json payload = neograph::json::object();
    payload["items"]       = items;
    if (uiAdapter_) {
        uiAdapter_->onPanelUpdated(h->id, payload);
    }
    return 0;
}

void ClientPluginManager::unregisterPanel(ClientPluginInstance* inst, void* panel) {
    auto h = static_cast<AgentxxPanel*>(panel);
    if (!inst || !h) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        auto&                       vec = cur->panels;
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if (it->id == h->id) {
                vec.erase(it);
                break;
            }
        }
        uiRegistry_ = std::move(cur);
    }
    auto& regs = inst->panelRegs;
    regs.erase(
        std::remove_if(
            regs.begin(),
            regs.end(),
            [&](const auto& p) {
                return p.id == h->id;
            }
        ),
        regs.end()
    );
    if (uiAdapter_) {
        uiAdapter_->onPanelRemoved(h->id);
    }
    h->inst = nullptr;
}

void* ClientPluginManager::registerInfoSection(
    ClientPluginInstance* inst,
    const char*           id,
    const char*           props_json
) {
    if (!inst) {
        return nullptr;
    }
    if (uiAdapter_ && !(uiAdapter_->uiCaps() & AGENTXX_UI_CAP_INFO_SECTION)) {
        XX_LOGW("[client_plugin] info section `{}` rejected: UI has no INFO_SECTION cap", id);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        for (const auto& s : uiRegistry_->infoSections) {
            if (s.id == id) {
                XX_LOGW("[client_plugin] info section id `{}` already registered", id);
                return nullptr;
            }
        }
    }
    neograph::json props;
    std::string    title;
    try {
        props = neograph::json::parse(props_json ? props_json : "{}");
        title = jsonStr(props, "title");
    } catch (...) {
        title.clear();
    }

    auto handle    = std::make_shared<AgentxxInfoSection>();
    handle->inst   = inst;
    handle->id     = id;
    handle->plugin = inst->name;

    ClientInfoSection reg;
    reg.plugin = inst->name;
    reg.id     = handle->id;
    reg.title  = title;
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        cur->infoSections.push_back(reg);
        uiRegistry_ = std::move(cur);
    }
    inst->infoSectionRegs.push_back(std::move(reg));
    inst->infoSectionHandles.push_back(handle);
    if (uiAdapter_) {
        uiAdapter_->onInfoSectionRegistered(handle->id, props);
    }
    return handle.get();
}

int ClientPluginManager::updateInfoSection(
    ClientPluginInstance* inst,
    void*                 section,
    const char*           items_json
) {
    auto h = static_cast<AgentxxInfoSection*>(section);
    if (!inst || !h) {
        return -1;
    }
    neograph::json items = neograph::json::array();
    try {
        auto j = neograph::json::parse(items_json ? items_json : "{}");
        if (j.contains("items") && j["items"].is_array()) {
            items = j["items"];
        }
    } catch (...) {
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        for (auto& s : cur->infoSections) {
            if (s.id == h->id) {
                s.items = items;
                break;
            }
        }
        uiRegistry_ = std::move(cur);
    }
    for (auto& s : inst->infoSectionRegs) {
        if (s.id == h->id) {
            s.items = items;
            break;
        }
    }
    neograph::json payload = neograph::json::object();
    payload["items"]       = items;
    if (uiAdapter_) {
        uiAdapter_->onInfoSectionUpdated(h->id, payload);
    }
    return 0;
}

void ClientPluginManager::unregisterInfoSection(ClientPluginInstance* inst, void* section) {
    auto h = static_cast<AgentxxInfoSection*>(section);
    if (!inst || !h) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        auto&                       vec = cur->infoSections;
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if (it->id == h->id) {
                vec.erase(it);
                break;
            }
        }
        uiRegistry_ = std::move(cur);
    }
    auto& regs = inst->infoSectionRegs;
    regs.erase(
        std::remove_if(
            regs.begin(),
            regs.end(),
            [&](const auto& s) {
                return s.id == h->id;
            }
        ),
        regs.end()
    );
    if (uiAdapter_) {
        uiAdapter_->onInfoSectionRemoved(h->id);
    }
    h->inst = nullptr;
}

int ClientPluginManager::registerCommand(
    ClientPluginInstance* inst,
    const char*           name,
    const char*           description,
    char* (*exec)(void*, AgentxxPluginStringView, char**),
    void* ud
) {
    if (!inst || !exec) {
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        for (const auto& c : uiRegistry_->commands) {
            if (c.name == name) {
                XX_LOGW("[client_plugin] command `{}` already registered", name);
                return -1;
            }
        }
    }
    ClientCommand reg;
    reg.plugin      = inst->name;
    reg.name        = name;
    reg.description = description ? description : "";
    reg.execute     = exec;
    reg.ud          = ud;
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        cur->commands.push_back(reg);
        uiRegistry_ = std::move(cur);
    }
    inst->commandRegs.push_back(std::move(reg));
    return 0;
}

int ClientPluginManager::unregisterCommand(ClientPluginInstance* inst, const char* name) {
    if (!inst) {
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        auto&                       vec = cur->commands;
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if (it->name == name && it->plugin == inst->name) {
                vec.erase(it);
                break;
            }
        }
        uiRegistry_ = std::move(cur);
    }
    auto& regs = inst->commandRegs;
    regs.erase(
        std::remove_if(
            regs.begin(),
            regs.end(),
            [&](const auto& c) {
                return c.name == name && c.plugin == inst->name;
            }
        ),
        regs.end()
    );
    return 0;
}

AgentxxSubscription* ClientPluginManager::subscribe(
    ClientPluginInstance* inst,
    int                   event,
    void (*handler)(AgentxxPluginStringView, void*),
    void* ud
) {
    if (!inst || !handler) {
        return nullptr;
    }
    auto sub  = std::make_shared<ClientSubscriptionImpl>();
    sub->inst = inst;
    ClientPluginInstance::Subscription s;
    s.event   = event;
    s.handler = handler;
    s.ud      = ud;
    s.alive   = true;
    inst->subscriptions.push_back(s);
    sub->sub = &inst->subscriptions.back();
    inst->subHandles.push_back(sub);
    return reinterpret_cast<AgentxxSubscription*>(sub.get());
}

void ClientPluginManager::unsubscribe(AgentxxSubscription* sub) {
    auto impl = reinterpret_cast<ClientSubscriptionImpl*>(sub);
    if (!impl || !impl->inst || !impl->sub) {
        return;
    }
    impl->sub->alive = false;
    auto& subs       = impl->inst->subscriptions;
    subs.erase(
        std::remove_if(
            subs.begin(),
            subs.end(),
            [](const auto& s) {
                return !s.alive;
            }
        ),
        subs.end()
    );
    impl->inst = nullptr;
    impl->sub  = nullptr;
}

std::string ClientPluginManager::getOwnInfoJson(ClientPluginInstance* inst) {
    if (!inst) {
        return "{}";
    }
    neograph::json j = neograph::json::object();
    j["name"]        = inst->name;
    j["version"]     = inst->version;
    j["description"] = inst->description;
    j["path"]        = inst->path;
    return j.dump();
}

std::string ClientPluginManager::getPluginArgsJson(ClientPluginInstance* inst) {
    if (!inst) {
        return "{}";
    }
    return inst->args.dump();
}

void ClientPluginManager::sendUserInputToPeer(
    ClientPluginInstance* inst,
    const char*           threadId,
    const char*           text
) {
    (void)threadId; // 会话以当前绑定为准 (threadId 不符时由端点兜底)
    if (!inst || !uiAdapter_) {
        return;
    }
    // 实际发送由 UI 适配器完成 (与用户输入同排队语义)
    uiAdapter_->sendPluginMessage(text ? text : "");
}

void ClientPluginManager::requestCancelToPeer(ClientPluginInstance* inst, const char* threadId) {
    if (!inst || !uiAdapter_) {
        return;
    }
    uiAdapter_->requestCancel(threadId ? threadId : "");
}

int ClientPluginManager::sendPluginDataToPeer(
    ClientPluginInstance* inst,
    const char*           event,
    const char*           json
) {
    if (!inst || !uiAdapter_) {
        return -1;
    }
    return uiAdapter_->sendPluginData(inst->name, event ? event : "", json ? json : "{}") ? 0 : -1;
}

} // namespace plugin
} // namespace agentxx
