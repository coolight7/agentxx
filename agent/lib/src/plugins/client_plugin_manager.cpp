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
#include "agentxx/plugin/plugin_common.h"
#include "agentxx/plugin/plugin_manager.h" /* NativeLoader (平台 dlopen 封装) */
#include "agentxx/util/container_util.h"

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

// ==================== 工具 ====================

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

} // namespace

// =====================================================================
// ClientPluginInstance
// =====================================================================

ClientPluginInstance::~ClientPluginInstance() {
    // dlclose (与 agent 侧 PluginInstance 一致): 调用方保证无在途回调
    // (unloadAsync 等 inflight 归零后移除; shutdownAll 进程退出路径约定无在途)
    if (dlHandle) {
        NativeLoader::close(dlHandle);
        dlHandle = nullptr;
    }
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

void ClientPluginManager::setSessionId(std::string sessionId) {
    sessionId_ = std::move(sessionId);
}

// ==================== 生命周期 ====================

asio::awaitable<std::shared_ptr<ClientPluginInstance>> ClientPluginManager::loadNativeAsync(
    std::string                         path,
    const agentxx::agent::PluginConfig* cfg,
    bool                                allowMissingEntry
) {
    // ---- 目录插件: 解析 plugin.yaml 取 entry 库路径 (与 agent 侧一致) ----
    // - manifest: name/entry/depends/optional_depends
    // - entry 平台化 + 配置子目录回退见公共 resolvePluginEntryPath
    // - 依赖解析与正式加载合并 (B3): 不再先 dlopen 探测再 close 后重新
    //   dlopen —— 本函数一次 dlopen 完成 探测(entry 符号) + 装配
    std::error_code          ec;
    std::string              libPath = path;
    std::vector<std::string> depends, optionalDepends;
    if (std::filesystem::is_directory(path, ec)) {
        std::string manifestName, manifestEntry;
        if (!parsePluginManifest(
                std::filesystem::path(path),
                manifestName,
                manifestEntry,
                depends,
                optionalDepends
            )) {
            XX_LOGE("[client_plugin] `{}` missing/invalid plugin.yaml", path);
            co_return nullptr;
        }
        libPath = resolvePluginEntryPath(std::filesystem::path(path), manifestEntry);
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
        name = pluginNameFromPath(libPath);
    }

    // 已存在同名插件: 拒绝 (防重复加载)
    if (plugins_.count(name) > 0) {
        XX_LOGE("[client_plugin] duplicate plugin name `{}`", name);
        NativeLoader::close(handle);
        co_return nullptr;
    }

    // entry 入口 (必需): 探测与加载合并 (B3) —— 一次 dlopen 内查符号
    std::string entryErr;
    auto        entryFn = reinterpret_cast<AgentxxClientPluginEntryFn>(
        NativeLoader::sym(handle, AGENTXX_CLIENT_SYMBOL_ENTRY, entryErr)
    );
    if (!entryFn) {
        if (allowMissingEntry) {
            // sides==Auto: 无 client 入口视为纯 agent 插件, 静默跳过
            XX_LOGI("[client_plugin] `{}` has no client entry, skipped (agent-only)", name);
        } else {
            XX_LOGE(
                "[client_plugin] `{}` missing {}: {}",
                path,
                AGENTXX_CLIENT_SYMBOL_ENTRY,
                entryErr
            );
        }
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

    auto inst         = std::make_shared<ClientPluginInstance>(name);
    inst->version     = version;
    inst->description = desc;
    inst->path        = path;
    // 插件配置参数随加载直接传入 (C2, 与 agent 侧一致): 宿主不解析字段语义,
    // 插件经 vtable get_plugin_args 整体读取; 直连路径 cfg 为 nullptr → {}
    inst->args            = cfg ? cfg->args : neograph::json::object();
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

    // entry 卸载到内部线程池执行 (A2): 与 agent 侧一致 —— entry 内 vtable
    // 注册动作经 ioCallSync 回 io 线程同步执行; entry 在 io 线程执行会阻塞
    // client io 事件循环 (慢初始化/插件间调用时明显), 且违背契约声明的
    // "entry 运行在宿主线程池"
    int rc = co_await agentxx::util::offloadAsync<int>(
        *pool_,
        [inst, entryFn]() -> asio::awaitable<int> {
            co_return entryFn(&inst->host, &inst->pluginCtx);
        }
    );
    if (rc != 0) {
        XX_LOGE("[client_plugin] `{}` entry failed (rc={})", name, rc);
        detachAll(inst.get(), false);
        // inst 随局部释放析构 → ~ClientPluginInstance → dlclose
        co_return nullptr;
    }

    util::insertHeterogeneous(plugins_, std::string{name}, inst);
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

    // 级联: 必选依赖者先卸载 (先子后父) —— 与 agent 侧 unloadAsync 一致
    for (const auto& child : collectReverseRequiredDeps(plugins_, std::string{name}, true)) {
        XX_LOGI("[client_plugin] unload `{}` cascades unload of dependent `{}`", name, child);
        co_await unloadAsync(child);
    }

    // 摘除注册 (adapter 通知 UI 移除; 彻底清理) —— 先于等待, 插件卸载期间
    // 不再收到任何回调
    detachAll(inst.get(), false);

    // 等 in-flight 回调归零 (超时放弃: 保持已 detach 状态, 复位可稍后重试)
    if (!co_await waitInflightZero(inst, std::chrono::seconds{10})) {
        inst->unloadRequested = false;
        XX_LOGW("[client_plugin] `{}` inflight not zero, unload aborted (retry later)", name);
        co_return false;
    }

    // unload 回调 (业务清理; 宿主已自动反注册全部残留; 句柄仍存活:
    // statusItemHandles/subHandles 等随实例析构释放, 回调内主动反注册安全)
    if (inst->dlHandle) {
        std::string err;
        auto        fn = reinterpret_cast<AgentxxClientPluginUnloadFn>(
            NativeLoader::sym(inst->dlHandle, AGENTXX_CLIENT_SYMBOL_UNLOAD, err)
        );
        if (fn) {
            fn(inst->pluginCtx);
        }
    }
    // 从表移除 → 实例析构 → dlclose (~ClientPluginInstance)
    util::eraseHeterogeneous(plugins_, name); // 异构删除免拷贝
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
    for (const auto& child : collectReverseRequiredDeps(plugins_, std::string{name}, true)) {
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
    // - sides == Agent: 跳过 (属于 agent 侧); enabled == false: 跳过
    // - cfg 指针指向入参 vector 元素, 生命周期覆盖本函数
    struct Item {
        std::string              path;
        std::string              name; ///< 空 = 无法推导 (不影响排序)
        std::vector<std::string> depends;
        bool allowMissingEntry  = false; ///< sides==Auto: 无 client 入口静默跳过
        const PluginConfig* cfg = nullptr;
    };

    std::vector<Item> items;
    for (const auto& pc : plugins) {
        if (!pc.enabled) {
            continue;
        }
        if (pc.sides == agentxx::agent::PluginSide::Agent) {
            continue; // 属于 agent 侧
        }
        Item it;
        it.path              = pc.path;
        it.cfg               = &pc;
        it.allowMissingEntry = (pc.sides != agentxx::agent::PluginSide::Client);
        if (std::filesystem::is_directory(std::filesystem::path(pc.path))) {
            std::string              name, entry;
            std::vector<std::string> depends, optionalDepends;
            if (parsePluginManifest(
                    std::filesystem::path(pc.path),
                    name,
                    entry,
                    depends,
                    optionalDepends
                )) {
                it.name    = name;
                it.depends = std::move(depends);
            } else {
                // 与 agent 侧行为对齐 (agent 侧会走 dlopen 失败报错):
                // 目录存在但 plugin.yaml 缺失/非法时明确报错, 避免静默跳过
                // 造成"为什么 client 没加载该插件"无从排查
                XX_LOGE("[client_plugin] `{}` missing/invalid plugin.yaml, skipped", pc.path);
            }
        } else {
            it.name = pluginNameFromPath(pc.path);
        }
        items.push_back(std::move(it));
    }

    // 拓扑排序: 依赖者排在被依赖者之后 (公共 topoSortPlugins; 无进展项附后
    // 由 loadNativeAsync 的依赖检查报错)
    auto ordered = topoSortPlugins(std::move(items));

    // 依次加载 (Auto 无 client 入口时由 loadNativeAsync 静默跳过; B3:
    // 探测与正式加载合并为一次 dlopen)
    for (const auto& it : ordered) {
        if (it.name.empty()) {
            continue;
        }
        if (plugins_.count(it.name) > 0) {
            continue; // 已加载
        }
        co_await loadNativeAsync(it.path, it.cfg, it.allowMissingEntry);
    }
}

void ClientPluginManager::shutdownAll() {
    // 依赖图级联卸载 (先子后父) —— 与 agent 侧 shutdownAll 一致:
    // 脚本类插件 (depends 引擎) 先卸载, 引擎最后 dlclose
    // - 不等在途回调: 调用方 (进程退出) 须保证无在途插件回调
    std::vector<std::string> names;
    names.reserve(plugins_.size());
    for (const auto& [name, inst] : plugins_) {
        (void)inst;
        names.push_back(name);
    }
    for (const auto& name : names) {
        auto inst = find(name);
        if (inst && !inst->unloadRequested) {
            shutdownClientPlugin(inst);
        }
    }
    plugins_.clear();
}

void ClientPluginManager::shutdownClientPlugin(const std::shared_ptr<ClientPluginInstance>& inst) {
    if (!inst || inst->unloadRequested) {
        return;
    }
    inst->unloadRequested = true;
    // 先递归卸载必选依赖本插件的插件 (先子后父)
    for (const auto& dep : collectReverseRequiredDeps(plugins_, inst->name, false)) {
        auto depInst = find(dep);
        if (depInst && !depInst->unloadRequested) {
            shutdownClientPlugin(depInst);
        }
    }
    detachAll(inst.get(), false);
    if (inst->dlHandle) {
        std::string err;
        auto        fn = reinterpret_cast<AgentxxClientPluginUnloadFn>(
            NativeLoader::sym(inst->dlHandle, AGENTXX_CLIENT_SYMBOL_UNLOAD, err)
        );
        if (fn) {
            fn(inst->pluginCtx);
        }
    }
    // dlclose 由 ~ClientPluginInstance 完成 (plugins_.clear() 后实例释放)
    XX_LOGI("[client_plugin] shutdown: {}", inst->name);
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
    auto it = plugins_.find(name);
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
    j["sessionId"]       = sessionId_;
    j["connState"]       = connState_;
    j["startupProgress"] = startupProgress_;
    j["uiCaps"]          = uiAdapter_ ? static_cast<int>(uiAdapter_->uiCaps()) : 0;
    // 服务端已加载的 agent 侧插件名列表 (空数组 = 未知, 见成员注释)
    j["agentPlugins"]    = serverPlugins_;
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
    j["sessionId"]   = sessionId_;
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

void ClientPluginManager::onUserInput(std::string_view sessionId, std::string_view text) {
    neograph::json j = neograph::json::object();
    j["sessionId"]   = std::string{sessionId};
    j["text"]        = std::string{text};
    dispatchEvent(AGENTXX_CLIENT_EVT_USER_INPUT, j.dump());
}

void ClientPluginManager::onDelta(const agentxx::agent::Delta& delta) {
    dispatchEvent(AGENTXX_CLIENT_EVT_DELTA, agentxx::agent::io::deltaToJson(delta).dump());
}

void ClientPluginManager::onTurnResult(const agentxx::agent::WireTurnResult& result) {
    neograph::json j = neograph::json::object();
    j["sessionId"]   = result.sessionId;
    j["hasError"]    = result.hasError;
    j["interrupted"] = result.interrupted;
    if (!result.errorMessage.empty()) {
        j["errorMessage"] = result.errorMessage;
    }
    j["startTimeMs"] = result.startTimeMs;
    j["durationMs"]  = result.durationMs;
    dispatchEvent(AGENTXX_CLIENT_EVT_TURN_END, j.dump());
}

void ClientPluginManager::onSessionSwitched(std::string_view sessionId) {
    sessionId_       = std::string{sessionId};
    neograph::json j = neograph::json::object();
    j["sessionId"]   = sessionId_;
    dispatchEvent(AGENTXX_CLIENT_EVT_SESSION_SWITCH, j.dump());
}

void ClientPluginManager::onPluginData(const agentxx::agent::WirePluginData& data) {
    // 宿主约定事件 (server 端 SessionServerAgentIO 发布, 见该文件 kHostPluginName):
    // server_plugins → 记录服务端已加载插件名列表, 供 get_client_state
    // ("agentPlugins") 查询对端可用性; 其余约定事件照常向插件分发
    if (data.plugin == "agentxx_host" && data.event == "server_plugins") {
        try {
            auto          j = neograph::json::parse(data.data);
            std::vector<std::string> names;
            if (j.contains("plugins") && j["plugins"].is_array()) {
                for (const auto& p : j["plugins"]) {
                    if (p.is_string()) {
                        names.push_back(p.get<std::string>());
                    }
                }
            }
            serverPlugins_ = std::move(names);
        } catch (...) {
            // 载荷非法: 保留旧值 (不视为致命)
        }
    }

    // 对端缺失提示 (每插件名一次): 无任何 client 插件订阅 EVT_PLUGIN_DATA 时,
    // 该插件事件在本地无人消费 —— 多半是对应插件未在本端加载 (分进程/分设备
    // 部署时单侧缺失)。仅警告一次, 不随事件频率刷屏; 正常情况 (有插件订阅,
    // 各自按名过滤) 不受影响。
    bool hasSubscriber = false;
    for (const auto& [name, inst] : plugins_) {
        (void)name;
        for (const auto& sub : inst->subscriptions) {
            if (sub && sub->alive && sub->event == AGENTXX_CLIENT_EVT_PLUGIN_DATA) {
                hasSubscriber = true;
                break;
            }
        }
        if (hasSubscriber) {
            break;
        }
    }
    if (!hasSubscriber && pluginDataNoSubWarned_.insert(data.plugin).second) {
        XX_LOGW(
            "[client_plugin] plugin data `{}.{}` received but no client plugin subscribed "
            "(plugin missing on this side? server-side loaded: {})",
            data.plugin,
            data.event,
            serverPlugins_.empty() ? "unknown" : "yes"
        );
    }

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
    // 指数退避轮询 (20ms → 1s 上限): 慢回调等待期间减少 io 线程定时器唤醒
    auto backoff = std::chrono::milliseconds{20};
    while (inst->inflight.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return false;
        }
        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(backoff);
        co_await timer.async_wait(asio::use_awaitable);
        backoff = std::min(backoff * 2, std::chrono::milliseconds{1000});
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
        // 彻底清理 (unload/shutdown 路径):
        // - 订阅句柄断链 (B8): unload 回调内插件主动 unsubscribe 时,
        //   impl->inst 已置空 → xx_cunsubscribe 安全跳过 (句柄由 subHandles
        //   保活到实例析构, 不解引用已释放内存)
        for (const auto& h : inst->subHandles) {
            auto impl  = std::static_pointer_cast<ClientSubscriptionImpl>(h);
            impl->inst = nullptr;
            impl->sub.reset();
        }
        inst->statusItemRegs.clear();
        inst->panelRegs.clear();
        inst->infoSectionRegs.clear();
        inst->commandRegs.clear();
        inst->subscriptions.clear();
    }
}

/// 反向必选依赖收集 → 公共 collectReverseRequiredDeps (plugin_common.h)

void ClientPluginManager::dispatchEvent(int event, const std::string& payloadJson) {
    // 快照订阅列表 (shared_ptr 副本: 派发中退订/卸载不会使后续回调悬垂;
    // 订阅对象被 impl 句柄/派发副本保活, alive 位标记已退订)
    struct SubRef {
        ClientPluginInstance*                               inst;
        std::shared_ptr<ClientPluginInstance::Subscription> sub;
    };

    std::vector<SubRef> refs;
    for (const auto& [name, inst] : plugins_) {
        (void)name;
        if (!inst->enabled) {
            continue;
        }
        for (const auto& s : inst->subscriptions) {
            if (s->alive && s->event == event) {
                refs.push_back(SubRef{inst.get(), s});
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

/// JSON 辅助: 提取字符串字段 (线程安全, 纯函数; 供插件替代手写 JSON 解析)
/// - 与 agent 侧 xx_json_get_string 一致, 无需绕道 io 线程
char* xx_cjson_get_string(
    const AgentxxClientHost* host,
    AgentxxPluginStringView  json,
    AgentxxPluginStringView  key
) {
    XX_PLUGIN_CATCH_BEGIN
    auto inst = clientInstOf(host);
    if (!inst || agentxx_plugin_sv_empty(json) || agentxx_plugin_sv_empty(key)) {
        return nullptr;
    }
    try {
        auto j = neograph::json::parse(std::string{json.data, json.size});
        auto v = jsonStr(j, std::string_view{key.data, key.size});
        if (v.empty() && !j.contains(std::string{key.data, key.size})) {
            return nullptr;
        }
        return xx_cstrdup(v.c_str());
    } catch (...) {
        return nullptr;
    }
    XX_PLUGIN_CATCH_END(nullptr)
}

/// JSON 辅助: 字符串 → JSON 字符串字面量 (含引号与转义; 线程安全纯函数)
char* xx_cjson_escape(const AgentxxClientHost* host, AgentxxPluginStringView s) {
    XX_PLUGIN_CATCH_BEGIN
    auto inst = clientInstOf(host);
    if (!inst || agentxx_plugin_sv_empty(s)) {
        return nullptr;
    }
    try {
        neograph::json j = std::string{s.data, s.size};
        return xx_cstrdup(j.dump().c_str());
    } catch (...) {
        return nullptr;
    }
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
    return ioCallSync<AgentxxStatusItem*>(mgr, [&]() -> AgentxxStatusItem* {
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
    return ioCallSync<int>(mgr, [&]() -> int {
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
    ioCallSyncVoid(mgr, [&]() {
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
    return ioCallSync<AgentxxPanel*>(mgr, [&]() -> AgentxxPanel* {
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
    return ioCallSync<int>(mgr, [&]() -> int {
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
    ioCallSyncVoid(mgr, [&]() {
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
    return ioCallSync<AgentxxInfoSection*>(mgr, [&]() -> AgentxxInfoSection* {
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
    return ioCallSync<int>(mgr, [&]() -> int {
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
    ioCallSyncVoid(mgr, [&]() {
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
    return ioCallSync<int>(mgr, [&]() -> int {
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
    return ioCallSync<int>(mgr, [&]() -> int {
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
    ioCallSyncVoid(mgr, [&]() {
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
    return ioCallSync<AgentxxSubscription*>(mgr, [&]() -> AgentxxSubscription* {
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
    // impl 由 subHandles 保活到实例析构; 实例已断链 (detachAll) 时跳过
    auto mgr = impl->inst ? impl->inst->manager.lock().get() : nullptr;
    if (mgr) {
        ioCallSyncVoid(mgr, [&]() {
            mgr->unsubscribe(reinterpret_cast<AgentxxSubscription*>(impl));
        });
    }
    impl->inst = nullptr;
    impl->sub.reset();
    XX_PLUGIN_CATCH_END_VOID()
}

// ---- 会话上下文 ----

char* xx_cget_client_state(const AgentxxClientHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = clientMgrOf(host);
    if (!mgr) {
        return nullptr;
    }
    return ioCallSync<char*>(mgr, [&]() -> char* {
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
    return ioCallSync<int>(mgr, [&]() -> int {
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
    ioCallSyncVoid(mgr, [&]() {
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
    return ioCallSync<int>(mgr, [&]() -> int {
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
    return ioCallSync<char*>(mgr, [&]() -> char* {
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
    return ioCallSync<char*>(mgr, [&]() -> char* {
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
    auto sub   = std::make_shared<ClientSubscriptionImpl>();
    sub->inst  = inst;
    auto s     = std::make_shared<ClientPluginInstance::Subscription>();
    s->event   = event;
    s->handler = handler;
    s->ud      = ud;
    s->alive   = true;
    inst->subscriptions.push_back(s);
    sub->sub = s; // 强引用: 订阅对象从 vector 摘除后仍被句柄保活 (unload 回调内退订安全)
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
            [&](const std::shared_ptr<ClientPluginInstance::Subscription>& s) {
                return s == impl->sub;
            }
        ),
        subs.end()
    );
    impl->inst = nullptr;
    impl->sub.reset();
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
    const char*           sessionId,
    const char*           text
) {
    (void)sessionId; // 会话以当前绑定为准 (sessionId 不符时由端点兜底)
    if (!inst || !uiAdapter_) {
        return;
    }
    // 实际发送由 UI 适配器完成 (与用户输入同排队语义)
    uiAdapter_->sendPluginMessage(text ? text : "");
}

void ClientPluginManager::requestCancelToPeer(ClientPluginInstance* inst, const char* sessionId) {
    if (!inst || !uiAdapter_) {
        return;
    }
    uiAdapter_->requestCancel(sessionId ? sessionId : "");
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
