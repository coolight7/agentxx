/// client_plugin_manager.cpp —— client 侧插件管理器实现
///
/// 线程模型 (与 agent 侧 PluginManager 一致的无锁单线程模型):
/// - 所有注册表/插件表/会话上下文状态仅 client io 线程读写
/// - UI 注册表 (uiRegistry_) 例外: io 线程写, UI 线程经快照读 (mutex + COW shared_ptr)
/// - dlopen/entry 卸载到内部 thread_pool; entry 的注册动作经 vtable ioCallSync
///   投递回 io 线程串行执行 (插件无感)
/// - 插件回调 (事件 handler / 命令 execute) 在 io 线程同步调用, 快速返回约定
/// - UI 线程从不直接调用插件代码: 命令触发经 postCommandInvocation 投递
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
#include "fmt/ranges.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>

#if XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/// 可执行目录 helper
using agentxx::plugin::getExecutableDirPath;

/// 状态栏项宿主句柄实现 (全局作用域, 与
/// [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h)
/// 的 C 不透明类型对应 —— vtable 函数签名中的 AgentxxStatusItem 即此类型,
/// 不能在命名空间内另行定义)
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
static inline bool isBuiltinScheme(std::string_view p) noexcept {
    return p.size() > 10 && p.substr(0, 10) == "builtin://";
}

static inline std::string parseBuiltinName(std::string_view p) {
    return std::string(p.substr(10));
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
    // dlclose (与 agent 侧 PluginInstance 一致): 调用方保证无未返回的回调
    // (unloadAsync 等 inflight 归零后移除; shutdownAll 进程退出路径约定
    // 没有尚未返回的插件回调)
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
    PluginManagerBase<ClientPluginInstance>(std::move(ex)),
    pool_(std::make_unique<asio::thread_pool>(1)),
    uiRegistry_(std::make_shared<const ClientUiRegistry>()) {}

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

InterfaceSet ClientPluginManager::hostSupportedInterfaces() const {
    // 接口集唯一来源: UI 适配器声明 (位图方案已移除, 见
    // [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h)
    // "接口协商" 节)。无适配器 (纯测试/直连) 时空集 —— 仅命令输入管线等
    // 宿主固有能力也由适配器显式声明, 保持单一事实来源
    return uiAdapter_ ? uiAdapter_->supportedInterfaces() : InterfaceSet{};
}

// ==================== 生命周期 ====================

asio::awaitable<std::shared_ptr<ClientPluginInstance>> ClientPluginManager::loadNativeAsync(
    std::string                         path,
    const agentxx::agent::PluginConfig* cfg,
    bool                                allowMissingEntry
) {
    // 内置简写: builtin://<name> 在 client 侧暂无内置 registry, 按目录探测
    // 回退 (若插件以目录形式存在于默认 plugins/<name> 则按目录加载,
    // 否则视为仅 agent 侧内置, 静默跳过)
    if (isBuiltinScheme(path)) {
        auto btName = parseBuiltinName(path);
        XX_LOGI(
            "[client_plugin] builtin `{}` skipped on client side (no client builtin registry)",
            btName
        );
        // 尝试目录回退: 按 exe 目录优先 + cwd (与 agent 侧一致)
        {
            auto exeDir = getExecutableDirPath();
            if (!exeDir.empty()) {
                auto dir = exeDir / "plugins" / btName;
                if (std::filesystem::is_directory(dir)) {
                    co_return co_await loadNativeAsync(dir.string(), cfg, allowMissingEntry);
                }
            }
        }
        {
            auto dir = std::filesystem::current_path() / "plugins" / btName;
            if (std::filesystem::is_directory(dir)) {
                co_return co_await loadNativeAsync(dir.string(), cfg, allowMissingEntry);
            }
        }
        co_return nullptr;
    }
    // ---- 目录插件: 解析 plugin.yaml 取 entry 库路径 (与 agent 侧一致) ----
    // - manifest: name/entry/depends/optional_depends/interfaces(接口声明)
    // - entry 平台化 + 配置子目录回退见公共 resolvePluginEntryPath
    // - 依赖解析与正式加载合并 (B3): 不再先 dlopen 探测再 close 后重新
    //   dlopen —— 本函数一次 dlopen 完成 探测(entry 符号) + 装配
    std::error_code          ec;
    std::string              libPath = path;
    std::vector<std::string> depends, optionalDepends;
    PluginManifestInterfaces interfaces;
    if (std::filesystem::is_directory(path, ec)) {
        std::string manifestName, manifestEntry;
        if (!parsePluginManifest(
                std::filesystem::path(path),
                manifestName,
                manifestEntry,
                depends,
                optionalDepends,
                nullptr,
                &interfaces
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
    if (auto getInfo = reinterpret_cast<AgentxxClientPluginGetInfoFn>(
            NativeLoader::sym(handle, AGENTXX_PLUGIN_CLIENT_SYMBOL_GET_INFO, err)
        )) {
        // C ABI 回调异常兜底: 插件违约按"未导出"处理 (名字从库名推导)
        const AgentxxClientPluginInfo* info = nullptr;
        try {
            info = getInfo();
        } catch (const std::exception& e) {
            XX_LOGW("[client_plugin] `{}` get_info threw: {}", path, e.what());
        } catch (...) {
            XX_LOGW("[client_plugin] `{}` get_info threw unknown exception", path);
        }
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

    // ---- 接口协商门禁 (三层协商第 2 层; 见
    //      [plugin_common.h](/agent/lib/include/agentxx/plugin/plugin_common.h) 接口协商节) ----
    // require 中本侧相关项未满足 → 跳过加载 (INFO + 记录原因, 非错误:
    // 同一插件目录服务 cli/tui/gui 多宿主, 本宿主缺某接口是预期情况);
    // optional 缺失仅警告 (插件 entry 内应按 ui_caps()/interfaces 自降级)
    {
        auto check = checkInterfacesForSide(interfaces, hostSupportedInterfaces(), false);
        if (!check.satisfied) {
            auto missing = fmt::format("{}", fmt::join(check.missingRequired, ", "));
            XX_LOGI(
                "[client_plugin] `{}` skipped: host lacks required interface(s) [{}]",
                name,
                missing
            );
            util::insertOrAssignHeterogeneous(
                skippedPlugins_,
                name,
                "missing required interfaces: " + missing
            );
            NativeLoader::close(handle);
            co_return nullptr;
        }
        for (const auto& m : check.missingOptional) {
            XX_LOGW(
                "[client_plugin] `{}` optional interface `{}` not supported by host, "
                "related features disabled",
                name,
                m
            );
        }
    }

    // entry 入口 (必需): 探测与加载合并 (B3) —— 一次 dlopen 内查符号
    std::string entryErr;
    auto        entryFn = reinterpret_cast<AgentxxClientPluginCreateFn>(
        NativeLoader::sym(handle, AGENTXX_PLUGIN_CLIENT_SYMBOL_CREATE, entryErr)
    );
    if (!entryFn) {
        // 接口声明意图预检: manifest 声明依赖 client 侧接口却未导出
        // client 入口 → 明确报错 (声明的期望优先于 sides==Auto 的静默容忍)
        if (requiredEntrySides(interfaces.require).clientEntry) {
            XX_LOGE(
                "[client_plugin] `{}` requires client-side interfaces but missing {}: {}",
                path,
                AGENTXX_PLUGIN_CLIENT_SYMBOL_CREATE,
                entryErr
            );
        } else if (allowMissingEntry) {
            // sides==Auto: 无 client 入口视为纯 agent 插件, 静默跳过
            XX_LOGI("[client_plugin] `{}` has no client entry, skipped (agent-only)", name);
        } else {
            XX_LOGE(
                "[client_plugin] `{}` missing {}: {}",
                path,
                AGENTXX_PLUGIN_CLIENT_SYMBOL_CREATE,
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
    inst->configPath      = cfg ? cfg->configPath : std::string{};
    inst->dlHandle        = handle;
    inst->depends         = std::move(depends);
    inst->optionalDepends = std::move(optionalDepends);
    inst->interfaces      = std::move(interfaces);
    inst->manager         = weak_from_this();
    inst->host.opaque     = inst.get();
    inst->host.vtable     = hostVtable();

    // (v4) min_ui_caps 位图门禁已移除: 接口要求统一由上方清单 interfaces
    // require 门禁承担 (字符串集, 见
    // [plugin_common.h](/agent/lib/include/agentxx/plugin/plugin_common.h) 接口协商节)

    // entry 卸载到内部线程池执行 (A2): 与 agent 侧一致 —— entry 内 vtable
    // 注册动作经 ioCallSync 回 io 线程同步执行; entry 在 io 线程执行会阻塞
    // client io 事件循环 (慢初始化/插件间调用时明显), 且违背契约声明的
    // "entry 运行在宿主线程池";
    // 插件违约抛异常按 rc=-1 处理 (加载失败清理路径)
    int rc = co_await agentxx::util::offloadAsync<int>(
        *pool_,
        [inst, entryFn]() -> asio::awaitable<int> {
            try {
                co_return entryFn(&inst->host, &inst->pluginCtx);
            } catch (const std::exception& e) {
                XX_LOGE("[client_plugin] `{}` entry threw: {}", inst->name, e.what());
            } catch (...) {
                XX_LOGE("[client_plugin] `{}` entry threw unknown exception", inst->name);
            }
            co_return -1;
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

    // 等未返回的回调归零 (超时放弃: 保持已 detach 状态, 复位可稍后重试)
    if (!co_await waitInflightZero(inst, std::chrono::seconds{10})) {
        inst->unloadRequested = false;
        XX_LOGW("[client_plugin] `{}` inflight not zero, unload aborted (retry later)", name);
        co_return false;
    }

    // unload 回调 (业务清理; 宿主已自动反注册全部残留; 句柄仍存活:
    // statusItemHandles/subHandles 等随实例析构释放, 回调内主动反注册安全)
    // C ABI 回调异常兜底: 插件违约不得打断卸载流程
    if (inst->dlHandle) {
        std::string err;
        auto        fn = reinterpret_cast<AgentxxClientPluginDestroyFn>(
            NativeLoader::sym(inst->dlHandle, AGENTXX_PLUGIN_CLIENT_SYMBOL_DESTROY, err)
        );
        if (fn) {
            try {
                fn(inst->pluginCtx);
            } catch (const std::exception& e) {
                XX_LOGW("[client_plugin] `{}` unload callback threw: {}", inst->name, e.what());
            } catch (...) {
                XX_LOGW("[client_plugin] `{}` unload callback threw unknown exception", inst->name);
            }
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
    // 工具消息装饰恢复 (无句柄, 直接写回注册表)
    for (const auto& reg : inst->toolDecorRegs) {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        bool                        dup = false;
        for (const auto& d : cur->toolDecors) {
            if (d.plugin == inst->name && d.toolCallId == reg.toolCallId) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            cur->toolDecors.push_back(reg);
        }
        uiRegistry_ = std::move(cur);
    }
    // 工具特化渲染器恢复 (直接写回注册表)
    for (const auto& reg : inst->toolRenderRegs) {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        bool                        dup = false;
        for (const auto& r : cur->toolRenderers) {
            if (r.plugin == inst->name && r.toolName == reg.toolName) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            cur->toolRenderers.push_back(reg);
        }
        uiRegistry_ = std::move(cur);
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

    // 宿主支持接口集 (加载前计算一次; 三层协商第 2 层的 require 门禁数据源)
    const auto hostIfaces = hostSupportedInterfaces();

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
        if (isBuiltinScheme(pc.path)) {
            it.name = parseBuiltinName(pc.path);
            // client 侧内置无依赖清单, 保持空依赖
        } else if (std::filesystem::is_directory(std::filesystem::path(pc.path))) {
            std::string              name, entry;
            std::vector<std::string> depends, optionalDepends;
            PluginManifestInterfaces ifaces;
            if (parsePluginManifest(
                    std::filesystem::path(pc.path),
                    name,
                    entry,
                    depends,
                    optionalDepends,
                    nullptr,
                    &ifaces
                )) {
                it.name    = name;
                it.depends = std::move(depends);
                // 接口协商门禁 (dlopen 前跳过): require 未满足 → 记录原因并
                // 跳过 (INFO 非错误; loadNativeAsync 内对直连调用有同款检查)
                auto check = checkInterfacesForSide(ifaces, hostIfaces, false);
                if (!check.satisfied) {
                    auto missing = fmt::format("{}", fmt::join(check.missingRequired, ", "));
                    XX_LOGI(
                        "[client_plugin] `{}` ({}) skipped: host lacks required "
                        "interface(s) [{}]",
                        name,
                        pc.path,
                        missing
                    );
                    util::insertOrAssignHeterogeneous(
                        skippedPlugins_,
                        name,
                        "missing required interfaces: " + missing
                    );
                    continue;
                }
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
    // - 不等未返回的插件回调: 调用方 (进程退出) 须保证没有尚未返回的插件回调
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
        auto        fn = reinterpret_cast<AgentxxClientPluginDestroyFn>(
            NativeLoader::sym(inst->dlHandle, AGENTXX_PLUGIN_CLIENT_SYMBOL_DESTROY, err)
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
        v.name               = inst->name;
        v.version            = inst->version;
        v.description        = inst->description;
        v.path               = inst->path;
        v.configPath         = inst->configPath;
        v.enabled            = inst->enabled;
        v.inflight           = inst->inflight.load(std::memory_order_relaxed);
        v.depends            = inst->depends;
        v.optionalDepends    = inst->optionalDepends;
        v.requiredInterfaces = inst->interfaces.require;
        v.optionalInterfaces = inst->interfaces.optional;
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

    PluginInstanceBase::InflightGuard guard(inst.get());
    AgentxxPluginString               err{nullptr, 0};
    AgentxxPluginString               out{nullptr, 0};
    try {
        auto argsSv = agentxx::plugin::PluginStringView::from(argsJson.data(), argsJson.size());
        cmd->execute(cmd->ud, &argsSv, &out, &err);
    } catch (const std::exception& e) {
        XX_LOGW("[client_plugin] command `{}` execute threw: {}", name, e.what());
    } catch (...) {
        XX_LOGW("[client_plugin] command `{}` execute threw unknown exception", name);
    }
    std::string actionJson;
    if (!out.data) {
        XX_LOGW(
            "[client_plugin] command `{}` failed: {}",
            name,
            err.data ? std::string(err.data, err.size) : "(no error message)"
        );
    } else {
        actionJson.assign(out.data, out.size);
    }
    if (err.data) {
        agentxx::plugin::PluginString::free(&inst->host, &err);
    }
    if (out.data) {
        agentxx::plugin::PluginString::free(&inst->host, &out);
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
    // 宿主支持的接口名清单 (三层协商第 3 层 —— 插件据此自行决定启用哪些
    // 功能; 见 [plugin_common.h](/agent/lib/include/agentxx/plugin/plugin_common.h)
    // 接口协商节)。位图 uiCaps 字段已移除 (v4)
    j["interfaces"] = [&] {
        auto arr = neograph::json::array();
        for (const auto& n : hostSupportedInterfaces()) {
            arr.push_back(n);
        }
        return arr;
    }();
    // 服务端已加载的 agent 侧插件结构化列表 [{name,version,interfaces},...]
    // (空数组 = 未知, 见成员注释)
    j["agentPlugins"] = [&] {
        auto arr = neograph::json::array();
        for (const auto& p : serverPlugins_) {
            arr.push_back({
                {"name",       p.name      },
                {"version",    p.version   },
                {"interfaces", p.interfaces}
            });
        }
        return arr;
    }();
    return j.dump();
}

// ==================== ClientEventSink 实现 ====================

void ClientPluginManager::onReady() {
    neograph::json j = neograph::json::object();
    // 宿主支持的接口名清单 (启动后最早可得的协商结果, 插件在 READY 回调内
    // 即可完成功能启用决策; 位图 uiCaps 字段已移除, 见
    // [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h) v4)
    j["interfaces"] = [&] {
        auto arr = neograph::json::array();
        for (const auto& n : hostSupportedInterfaces()) {
            arr.push_back(n);
        }
        return arr;
    }();
    j["sessionId"] = sessionId_;
    dispatchEvent(AGENTXX_CLIENT_EVT_READY, j.dump());
    // 三期6: 向服务端上报本 client 支持的接口集 (约定事件, 镜像 server_plugins;
    // 服务端存储并经事件总线发布, agent 侧插件订阅 "agentxx_host.client_interfaces"
    // 据此自适应 —— 如 emit_message_tip 在无 toast 接口的宿主上降级)
    if (uiAdapter_) {
        neograph::json up = neograph::json::object();
        up["sessionId"]   = sessionId_;
        up["interfaces"]  = j["interfaces"];
        uiAdapter_->sendPluginData("agentxx_host", "client_interfaces", up.dump());
    }
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

void ClientPluginManager::onDelta(const agentxx::agent::WireDelta& delta) {
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
    // server_plugins → 记录服务端已加载插件结构化信息 [{name,version,interfaces},...],
    // 供 get_client_state ("agentPlugins") 查询对端可用性与能力; 其余约定
    // 事件照常向插件分发
    if (data.plugin == "agentxx_host" && data.event == "server_plugins") {
        try {
            auto                          j = neograph::json::parse(data.data);
            std::vector<ServerPluginInfo> infos;
            if (j.contains("plugins") && j["plugins"].is_array()) {
                for (const auto& p : j["plugins"]) {
                    if (!p.is_object() || !p.contains("name") || !p["name"].is_string()) {
                        continue;
                    }
                    ServerPluginInfo info{.name = p["name"].get<std::string>()};
                    if (p.contains("version") && p["version"].is_string()) {
                        info.version = p["version"].get<std::string>();
                    }
                    if (p.contains("interfaces") && p["interfaces"].is_array()) {
                        for (const auto& n : p["interfaces"]) {
                            if (n.is_string()) {
                                info.interfaces.push_back(n.get<std::string>());
                            }
                        }
                    }
                    infos.push_back(std::move(info));
                }
            }
            serverPlugins_ = std::move(infos);
        } catch (...) {
            // 载荷非法: 保留旧值, 不崩溃
        }
    }

    // 对端缺失提示 (每插件名一次): 无任何 client 插件订阅 EVT_PLUGIN_DATA 时,
    // 该插件事件在本地无人处理 —— 多半是对应插件未在本端加载 (分进程/分设备
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
        auto& td = reg->toolDecors;
        for (auto it = td.begin(); it != td.end();) {
            if (it->plugin == inst->name) {
                it = td.erase(it);
            } else {
                ++it;
            }
        }
        auto& tr = reg->toolRenderers;
        for (auto it = tr.begin(); it != tr.end();) {
            if (it->plugin == inst->name) {
                it = tr.erase(it);
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
        inst->toolDecorRegs.clear();
        inst->toolRenderRegs.clear();
        inst->subscriptions.clear();
    }
}

/// 反向必选依赖收集 → 公共 collectReverseRequiredDeps
/// (见 [plugin_common.h](/agent/lib/include/agentxx/plugin/plugin_common.h))

void ClientPluginManager::dispatchEvent(int event, const std::string& payloadJson) {
    ioThreadId_.store(std::this_thread::get_id(), std::memory_order_release);

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
        PluginInstanceBase::InflightGuard guard(ref.inst);
        // C ABI 回调异常兜底: 单个插件 handler 违约不得打断整轮派发
        // (影响其他订阅者与 client io 事件循环)
        try {
            auto payloadSv
                = agentxx::plugin::PluginStringView::from(payloadJson.data(), payloadJson.size());
            ref.sub->handler(&payloadSv, ref.sub->ud);
        } catch (const std::exception& e) {
            XX_LOGW("[client_plugin] `{}` event handler threw: {}", ref.inst->name, e.what());
        } catch (...) {
            XX_LOGW("[client_plugin] `{}` event handler threw unknown exception", ref.inst->name);
        }
    }
}

// =====================================================================
// host vtable (C ABI)
// =====================================================================

namespace {

ClientPluginInstance* clientInstOf(const AgentxxPluginHost* host) {
    return (host && host->opaque) ? static_cast<ClientPluginInstance*>(host->opaque) : nullptr;
}

/// "agentxx.client.ui" 展示接口表访问器 (定义于下方接口表装配区, 需在
/// xx_cquery_interface 处前向引用)
static const AgentxxClientUiIface* clientUiIface();

/// 其余标准接口表 (定义于下方装配区; 此处前向引用供 query_interface 分发)
extern const AgentxxClientEventsIface  g_clientIfaceEvents;
extern const AgentxxClientSessionIface g_clientIfaceSession;
extern const AgentxxClientWireIface    g_clientIfaceWire;
extern const AgentxxClientSelfIface    g_clientIfaceSelf;
extern const AgentxxClientJsonIface    g_clientIfaceJson;
extern const AgentxxClientLogIface     g_clientIfaceLog;

ClientPluginManager* clientMgrOf(const AgentxxPluginHost* host) {
    auto inst = clientInstOf(host);
    return inst ? inst->manager.lock().get() : nullptr;
}

// ---- 内存 ----

static void* AGENTXX_PLUGIN_CALL xx_calloc(uint64_t size) {
    return agentxx::plugin::hostMemoryAlloc(size);
}

static void AGENTXX_PLUGIN_CALL xx_cfree(void* ptr) {
    agentxx::plugin::hostMemoryFree(ptr);
}

// ---- 日志 / JSON ----

void AGENTXX_PLUGIN_CALL
    xx_clog(const AgentxxPluginHost* host, int32_t level, const AgentxxPluginStringView* msg) {
    (void)host;
    std::string_view s = (msg && msg->data)
                             ? std::string_view{msg->data, static_cast<size_t>(msg->size)}
                             : std::string_view{};
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
int32_t AGENTXX_PLUGIN_CALL xx_cjson_get_string(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* json,
    const AgentxxPluginStringView* key,
    AgentxxPluginString*           out
) {
    if (!out) {
        return -1;
    }
    auto inst = clientInstOf(host);
    if (!inst || agentxx::plugin::PluginStringView::empty(json)
        || agentxx::plugin::PluginStringView::empty(key)) {
        return -1;
    }
    try {
        auto j = neograph::json::parse(std::string{json->data, static_cast<size_t>(json->size)});
        auto v = jsonStr(j, std::string_view{key->data, static_cast<size_t>(key->size)});
        if (v.empty() && !j.contains(std::string{key->data, static_cast<size_t>(key->size)})) {
            return -1;
        }
        hostMemorySetString(out, v);
        return 0;
    } catch (...) {
        return -1;
    }
}

/// JSON 辅助: 字符串 → JSON 字符串字面量 (含引号与转义; 线程安全纯函数)
int32_t AGENTXX_PLUGIN_CALL xx_cjson_escape(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* s,
    AgentxxPluginString*           out
) {
    if (!out) {
        return -1;
    }
    auto inst = clientInstOf(host);
    if (!inst || agentxx::plugin::PluginStringView::empty(s)) {
        return -1;
    }
    try {
        neograph::json j       = std::string{s->data, static_cast<size_t>(s->size)};
        auto           dumpStr = j.dump();
        hostMemorySetString(out, dumpStr);
        return 0;
    } catch (...) {
        return -1;
    }
}

// ---- COM 风格接口表查询 ----

const void* AGENTXX_PLUGIN_CALL
    xx_cquery_interface(const AgentxxPluginHost* host, const AgentxxPluginStringView* iid) {
    if (!iid || !iid->data) {
        return nullptr;
    }
    std::string_view n{iid->data, static_cast<size_t>(iid->size)};
    if (n == AGENTXX_IFACE_CLIENT_UI) {
        return clientUiIface();
    }
    if (n == AGENTXX_IFACE_CLIENT_EVENTS) {
        return &g_clientIfaceEvents;
    }
    if (n == AGENTXX_IFACE_CLIENT_SESSION) {
        return &g_clientIfaceSession;
    }
    if (n == AGENTXX_IFACE_CLIENT_WIRE) {
        return &g_clientIfaceWire;
    }
    if (n == AGENTXX_IFACE_CLIENT_SELF) {
        return &g_clientIfaceSelf;
    }
    if (n == AGENTXX_IFACE_CLIENT_JSON) {
        return &g_clientIfaceJson;
    }
    if (n == AGENTXX_IFACE_CLIENT_LOG) {
        return &g_clientIfaceLog;
    }
    return nullptr;
}

// ---- 状态栏项 ----

AgentxxStatusItem* AGENTXX_PLUGIN_CALL xx_cregister_status_item(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* id,
    const AgentxxPluginStringView* initial_json,
    int32_t                        align,
    int32_t                        order
) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> AgentxxStatusItem* {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(id)) {
            return static_cast<AgentxxStatusItem*>(nullptr);
        }
        auto idVal = *id;
        auto jsonVal
            = initial_json ? *initial_json : agentxx::plugin::PluginStringView::from("{}", 2);
        return ioCallSync<AgentxxStatusItem*>(mgr, [&]() -> AgentxxStatusItem* {
            return static_cast<AgentxxStatusItem*>(
                mgr->registerStatusItem(inst, idVal, jsonVal, align, order)
            );
        });
    });
}

int32_t AGENTXX_PLUGIN_CALL xx_cupdate_status_item(
    const AgentxxPluginHost*       host,
    AgentxxStatusItem*             item,
    const AgentxxPluginStringView* json
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || !item || !json) {
            return -1;
        }
        auto jsonVal = *json;
        return ioCallSync<int32_t>(mgr, [&]() -> int32_t {
            return mgr->updateStatusItem(inst, item, jsonVal);
        });
    });
}

void AGENTXX_PLUGIN_CALL
    xx_cunregister_status_item(const AgentxxPluginHost* host, AgentxxStatusItem* item) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || !item) {
            return;
        }
        ioCallSyncVoid(mgr, [&]() {
            mgr->unregisterStatusItem(inst, item);
        });
    });
}

// ---- 侧边栏面板 ----

AgentxxPanel* AGENTXX_PLUGIN_CALL xx_cregister_panel(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* id,
    const AgentxxPluginStringView* props_json
) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> AgentxxPanel* {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(id)) {
            return static_cast<AgentxxPanel*>(nullptr);
        }
        auto idVal    = *id;
        auto propsVal = props_json ? *props_json : agentxx::plugin::PluginStringView::from("{}", 2);
        return ioCallSync<AgentxxPanel*>(mgr, [&]() -> AgentxxPanel* {
            return static_cast<AgentxxPanel*>(mgr->registerPanel(inst, idVal, propsVal));
        });
    });
}

int32_t AGENTXX_PLUGIN_CALL xx_cupdate_panel(
    const AgentxxPluginHost*       host,
    AgentxxPanel*                  panel,
    const AgentxxPluginStringView* items_json
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || !panel || !items_json) {
            return -1;
        }
        auto itemsVal = *items_json;
        return ioCallSync<int32_t>(mgr, [&]() -> int32_t {
            return mgr->updatePanel(inst, panel, itemsVal);
        });
    });
}

void AGENTXX_PLUGIN_CALL xx_cunregister_panel(const AgentxxPluginHost* host, AgentxxPanel* panel) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || !panel) {
            return;
        }
        ioCallSyncVoid(mgr, [&]() {
            mgr->unregisterPanel(inst, panel);
        });
    });
}

// ---- Info 栏段落 ----

AgentxxInfoSection* AGENTXX_PLUGIN_CALL xx_cregister_info_section(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* id,
    const AgentxxPluginStringView* props_json
) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> AgentxxInfoSection* {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(id)) {
            return static_cast<AgentxxInfoSection*>(nullptr);
        }
        auto idVal    = *id;
        auto propsVal = props_json ? *props_json : agentxx::plugin::PluginStringView::from("{}", 2);
        return ioCallSync<AgentxxInfoSection*>(mgr, [&]() -> AgentxxInfoSection* {
            return static_cast<AgentxxInfoSection*>(mgr->registerInfoSection(inst, idVal, propsVal)
            );
        });
    });
}

int32_t AGENTXX_PLUGIN_CALL xx_cupdate_info_section(
    const AgentxxPluginHost*       host,
    AgentxxInfoSection*            section,
    const AgentxxPluginStringView* items_json
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || !section || !items_json) {
            return -1;
        }
        auto itemsVal = *items_json;
        return ioCallSync<int32_t>(mgr, [&]() -> int32_t {
            return mgr->updateInfoSection(inst, section, itemsVal);
        });
    });
}

void AGENTXX_PLUGIN_CALL
    xx_cunregister_info_section(const AgentxxPluginHost* host, AgentxxInfoSection* section) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || !section) {
            return;
        }
        ioCallSyncVoid(mgr, [&]() {
            mgr->unregisterInfoSection(inst, section);
        });
    });
}

int32_t AGENTXX_PLUGIN_CALL xx_cupdate_tool_decor(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* tool_call_id,
    const AgentxxPluginStringView* decor_json
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst) {
            return -1;
        }
        auto tcidVal
            = tool_call_id ? *tool_call_id : agentxx::plugin::PluginStringView::from("", 0);
        auto decorVal = decor_json ? *decor_json : agentxx::plugin::PluginStringView::from("", 0);
        return ioCallSync<int32_t>(mgr, [&]() -> int32_t {
            return mgr->updateToolDecor(inst, tcidVal, decorVal);
        });
    });
}

int32_t AGENTXX_PLUGIN_CALL
    xx_cregister_tool_renderer(const AgentxxPluginHost* host, const AgentxxToolRenderSpec* spec) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || !spec) {
            return -1;
        }
        return ioCallSync<int32_t>(mgr, [&]() -> int32_t {
            return mgr->registerToolRenderer(inst, spec);
        });
    });
}

int32_t AGENTXX_PLUGIN_CALL xx_cunregister_tool_renderer(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* tool_name
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || !tool_name) {
            return -1;
        }
        auto tnameVal = *tool_name;
        return ioCallSync<int32_t>(mgr, [&]() -> int32_t {
            return mgr->unregisterToolRenderer(inst, tnameVal);
        });
    });
}

// ---- 命令 ----

int32_t AGENTXX_PLUGIN_CALL xx_cregister_command(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* name,
    const AgentxxPluginStringView* description,
    int32_t(AGENTXX_PLUGIN_CALL*
                execute)(void*, const AgentxxPluginStringView*, AgentxxPluginString*, AgentxxPluginString*),
    void* ud
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || !execute || agentxx::plugin::PluginStringView::empty(name)) {
            return -1;
        }
        auto nameVal = *name;
        auto descVal = description ? *description : agentxx::plugin::PluginStringView::from("", 0);
        return ioCallSync<int32_t>(mgr, [&]() -> int32_t {
            return mgr->registerCommand(inst, nameVal, descVal, execute, ud);
        });
    });
}

int32_t AGENTXX_PLUGIN_CALL
    xx_cunregister_command(const AgentxxPluginHost* host, const AgentxxPluginStringView* name) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(name)) {
            return -1;
        }
        auto nameVal = *name;
        return ioCallSync<int32_t>(mgr, [&]() -> int32_t {
            return mgr->unregisterCommand(inst, nameVal);
        });
    });
}

// ---- toast ----

void AGENTXX_PLUGIN_CALL xx_cshow_toast(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* text,
    int32_t                        level
) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto mgr = clientMgrOf(host);
        if (!mgr || !mgr->uiAdapter() || !text) {
            return;
        }
        std::string textStr{text->data ? text->data : "", static_cast<size_t>(text->size)};
        ioCallSyncVoid(mgr, [&]() {
            mgr->uiAdapter()->onToast(textStr, level);
        });
    });
}

// ---- 事件订阅 ----

AgentxxPluginSubscription* AGENTXX_PLUGIN_CALL xx_csubscribe(
    const AgentxxPluginHost* host,
    int32_t                  event,
    void(AGENTXX_PLUGIN_CALL* handler)(const AgentxxPluginStringView*, void*),
    void* ud
) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> AgentxxPluginSubscription* {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || !handler) {
            return static_cast<AgentxxPluginSubscription*>(nullptr);
        }
        if (event < 0 || event >= AGENTXX_CLIENT_EVT_COUNT) {
            return static_cast<AgentxxPluginSubscription*>(nullptr);
        }
        return ioCallSync<AgentxxPluginSubscription*>(mgr, [&]() -> AgentxxPluginSubscription* {
            return static_cast<AgentxxPluginSubscription*>(mgr->subscribe(inst, event, handler, ud)
            );
        });
    });
}

void AGENTXX_PLUGIN_CALL xx_cunsubscribe(AgentxxPluginSubscription* sub) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        if (!sub) {
            return;
        }
        auto impl = reinterpret_cast<ClientSubscriptionImpl*>(sub);
        auto mgr  = impl->inst ? impl->inst->manager.lock().get() : nullptr;
        if (mgr) {
            ioCallSyncVoid(mgr, [&]() {
                mgr->unsubscribe(reinterpret_cast<AgentxxPluginSubscription*>(impl));
            });
        }
        impl->inst = nullptr;
        impl->sub.reset();
    });
}

// ---- 会话上下文 ----

int32_t AGENTXX_PLUGIN_CALL
    xx_cget_client_state(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr = clientMgrOf(host);
        if (!mgr) {
            return -1;
        }
        auto s = ioCallSync<std::string>(mgr, [&]() -> std::string {
            return mgr->clientStateJson();
        });
        hostMemorySetString(out, s);
        return 0;
    });
}

// ---- 会话操作 ----

int32_t AGENTXX_PLUGIN_CALL xx_csend_user_input(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* thread_id,
    const AgentxxPluginStringView* text
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(text)) {
            return -1;
        }
        auto tidVal  = thread_id ? *thread_id : agentxx::plugin::PluginStringView::from("", 0);
        auto textVal = *text;
        return ioCallSync<int32_t>(mgr, [&]() -> int32_t {
            mgr->sendUserInputToPeer(inst, tidVal, textVal);
            return 0;
        });
    });
}

void AGENTXX_PLUGIN_CALL
    xx_crequest_cancel(const AgentxxPluginHost* host, const AgentxxPluginStringView* thread_id) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst) {
            return;
        }
        auto tidVal = thread_id ? *thread_id : agentxx::plugin::PluginStringView::from("", 0);
        ioCallSyncVoid(mgr, [&]() {
            mgr->requestCancelToPeer(inst, tidVal);
        });
    });
}

// ---- 跨端数据 ----

int32_t AGENTXX_PLUGIN_CALL xx_csend_plugin_data(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* event,
    const AgentxxPluginStringView* json
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(event)) {
            return -1;
        }
        auto evtVal  = *event;
        auto jsonVal = json ? *json : agentxx::plugin::PluginStringView::from("{}", 2);
        return ioCallSync<int32_t>(mgr, [&]() -> int32_t {
            return mgr->sendPluginDataToPeer(inst, evtVal, jsonVal);
        });
    });
}

// ---- 自描述 ----

int32_t AGENTXX_PLUGIN_CALL
    xx_cget_own_info(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst) {
            return -1;
        }
        auto s = ioCallSync<std::string>(mgr, [&]() -> std::string {
            return mgr->getOwnInfoJson(inst);
        });
        hostMemorySetString(out, s);
        return 0;
    });
}

int32_t AGENTXX_PLUGIN_CALL
    xx_cget_plugin_args(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst) {
            return -1;
        }
        auto s = ioCallSync<std::string>(mgr, [&]() -> std::string {
            return mgr->getPluginArgsJson(inst);
        });
        hostMemorySetString(out, s);
        return 0;
    });
}

int32_t AGENTXX_PLUGIN_CALL
    xx_cget_plugin_config_path(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr  = clientMgrOf(host);
        auto inst = clientInstOf(host);
        if (!mgr || !inst || inst->configPath.empty()) {
            return -1;
        }
        auto s = ioCallSync<std::string>(mgr, [&]() -> std::string {
            return mgr->getPluginConfigPath(inst);
        });
        if (s.empty()) {
            return -1;
        }
        hostMemorySetString(out, s);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_cget_language(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr = clientMgrOf(host);
        if (!mgr) {
            return -1;
        }
        auto s = ioCallSync<std::string>(mgr, [&]() -> std::string {
            return mgr->getLanguage();
        });
        if (s.empty()) {
            s = "en";
        }
        hostMemorySetString(out, s);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_cset_language(const AgentxxPluginHost* host, const AgentxxPluginStringView* language) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr = clientMgrOf(host);
        if (!mgr) {
            return -1;
        }
        std::string lang = (language && language->data)
                               ? std::string(language->data, static_cast<size_t>(language->size))
                               : std::string{};
        ioCallSyncVoid(mgr, [&]() {
            mgr->setLanguage(lang);
        });
        return 0;
    });
}

/// "agentxx.client.ui" 展示接口表访问器: 表内成员恒非空 (函数实现存在), 子能力是否
/// 可用由各 register 入口的 hostSupportedInterfaces 门禁决定 (拒绝时返回
/// NULL/非 0) —— 与接口表 "NULL = 不支持" 契约的分工: 表级 NULL 用于宿主
/// 整体缺失某子能力入口的场景 (当前宿主全量装配, 保留判空语义供第三方精简
/// 宿主使用)。以函数内静态表实现 (前向引用无需 extern 声明)
static const AgentxxClientUiIface* clientUiIface() {
    static const AgentxxClientUiIface table = {
        /* version */ AGENTXX_IFACE_CLIENT_UI_VERSION,
        /* _reserved */ 0,
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
        /* update_tool_decor */ xx_cupdate_tool_decor,
        /* register_tool_renderer */ xx_cregister_tool_renderer,
        /* unregister_tool_renderer */ xx_cunregister_tool_renderer,
    };
    return &table;
}

// ---- 其余标准接口表 (进程级静态只读; 经 query_interface 分发) ----

const AgentxxClientEventsIface g_clientIfaceEvents = {
    /* version */ AGENTXX_IFACE_CLIENT_EVENTS_VERSION,
    /* _reserved */ 0,
    /* subscribe */ xx_csubscribe,
    /* unsubscribe */ xx_cunsubscribe,
};

const AgentxxClientSessionIface g_clientIfaceSession = {
    /* version */ AGENTXX_IFACE_CLIENT_SESSION_VERSION,
    /* _reserved */ 0,
    /* get_client_state */ xx_cget_client_state,
    /* send_user_input */ xx_csend_user_input,
    /* request_cancel */ xx_crequest_cancel,
};

const AgentxxClientWireIface g_clientIfaceWire = {
    /* version */ AGENTXX_IFACE_CLIENT_WIRE_VERSION,
    /* _reserved */ 0,
    /* send_plugin_data */ xx_csend_plugin_data,
};

const AgentxxClientSelfIface g_clientIfaceSelf = {
    /* version */ AGENTXX_IFACE_CLIENT_SELF_VERSION,
    /* _reserved */ 0,
    /* get_own_info */ xx_cget_own_info,
    /* get_plugin_args */ xx_cget_plugin_args,
    /* get_plugin_config_path */ xx_cget_plugin_config_path,
    /* get_language */ xx_cget_language,
    /* set_language */ xx_cset_language,
};

const AgentxxClientJsonIface g_clientIfaceJson = {
    /* version */ AGENTXX_IFACE_CLIENT_JSON_VERSION,
    /* _reserved */ 0,
    /* json_get_string */ xx_cjson_get_string,
    /* json_escape */ xx_cjson_escape,
};

const AgentxxClientLogIface g_clientIfaceLog = {
    /* version */ AGENTXX_IFACE_CLIENT_LOG_VERSION,
    /* _reserved */ 0,
    /* log */ xx_clog,
};

/// 核心 vtable (契约冻结: 仅内存两件套 + query_interface)
const AgentxxHostVtable g_clientHostVtable = {
    /* alloc */ xx_calloc,
    /* free */ xx_cfree,
    /* query_interface */ xx_cquery_interface,
};

} // namespace

const AgentxxHostVtable* ClientPluginManager::hostVtable() {
    return &g_clientHostVtable;
}

// =====================================================================
// ClientPluginManager 内部实现 (vtable 强类型入口)
// =====================================================================

void* ClientPluginManager::registerStatusItem(
    ClientPluginInstance*   inst,
    AgentxxPluginStringView id,
    AgentxxPluginStringView json,
    int                     align,
    int                     order
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(id)) {
        return nullptr;
    }
    std::string idStr = svToStr(id);
    if (!hostSupportedInterfaces().contains(std::string{plugin_interfaces::ClientStatusItem})) {
        XX_LOGW(
            "[client_plugin] status item `{}` rejected: interface agentxx.client.status_item unsupported",
            idStr
        );
        return nullptr;
    }
    // id 冲突检查 (全局)
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        for (const auto& s : uiRegistry_->statusItems) {
            if (s.id == idStr) {
                XX_LOGW("[client_plugin] status item id `{}` already registered", idStr);
                return nullptr;
            }
        }
    }
    // 解析 initial_json → text
    std::string    text;
    neograph::json props;
    try {
        props = neograph::json::parse(
            agentxx::plugin::PluginStringView::empty(json) ? "{}" : svToSv(json)
        );
        text = jsonStr(props, "text");
    } catch (...) {
        text.clear();
    }
    if (text.empty()) {
        text = idStr;
    }

    auto handle    = std::make_shared<AgentxxStatusItem>();
    handle->inst   = inst;
    handle->id     = idStr;
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
    ClientPluginInstance*   inst,
    void*                   item,
    AgentxxPluginStringView json
) {
    auto h = static_cast<AgentxxStatusItem*>(item);
    if (!inst || !h) {
        return -1;
    }
    neograph::json props;
    std::string    text;
    try {
        props = neograph::json::parse(
            agentxx::plugin::PluginStringView::empty(json) ? "{}" : svToSv(json)
        );
        text = jsonStr(props, "text");
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
    ClientPluginInstance*   inst,
    AgentxxPluginStringView id,
    AgentxxPluginStringView props_json
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(id)) {
        return nullptr;
    }
    std::string idStr = svToStr(id);
    if (!hostSupportedInterfaces().contains(std::string{plugin_interfaces::ClientPanel})) {
        XX_LOGW(
            "[client_plugin] panel `{}` rejected: interface agentxx.client.panel unsupported",
            idStr
        );
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        for (const auto& p : uiRegistry_->panels) {
            if (p.id == idStr) {
                XX_LOGW("[client_plugin] panel id `{}` already registered", idStr);
                return nullptr;
            }
        }
    }
    neograph::json props;
    std::string    title;
    try {
        props = neograph::json::parse(
            agentxx::plugin::PluginStringView::empty(props_json) ? "{}" : svToSv(props_json)
        );
        title = jsonStr(props, "title");
    } catch (...) {
        title.clear();
    }
    if (title.empty()) {
        title = idStr;
    }

    auto handle    = std::make_shared<AgentxxPanel>();
    handle->inst   = inst;
    handle->id     = idStr;
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
    ClientPluginInstance*   inst,
    void*                   panel,
    AgentxxPluginStringView items_json
) {
    auto h = static_cast<AgentxxPanel*>(panel);
    if (!inst || !h) {
        return -1;
    }
    neograph::json items = neograph::json::array();
    try {
        auto j = neograph::json::parse(
            agentxx::plugin::PluginStringView::empty(items_json) ? "{}" : svToSv(items_json)
        );
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
    ClientPluginInstance*   inst,
    AgentxxPluginStringView id,
    AgentxxPluginStringView props_json
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(id)) {
        return nullptr;
    }
    std::string idStr = svToStr(id);
    if (!hostSupportedInterfaces().contains(std::string{plugin_interfaces::ClientInfoSection})) {
        XX_LOGW(
            "[client_plugin] info section `{}` rejected: interface agentxx.client.info_section unsupported",
            idStr
        );
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        for (const auto& s : uiRegistry_->infoSections) {
            if (s.id == idStr) {
                XX_LOGW("[client_plugin] info section id `{}` already registered", idStr);
                return nullptr;
            }
        }
    }
    neograph::json props;
    std::string    title;
    try {
        props = neograph::json::parse(
            agentxx::plugin::PluginStringView::empty(props_json) ? "{}" : svToSv(props_json)
        );
        title = jsonStr(props, "title");
    } catch (...) {
        title.clear();
    }

    auto handle    = std::make_shared<AgentxxInfoSection>();
    handle->inst   = inst;
    handle->id     = idStr;
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
    ClientPluginInstance*   inst,
    void*                   section,
    AgentxxPluginStringView items_json
) {
    auto h = static_cast<AgentxxInfoSection*>(section);
    if (!inst || !h) {
        return -1;
    }
    neograph::json items = neograph::json::array();
    try {
        auto j = neograph::json::parse(
            agentxx::plugin::PluginStringView::empty(items_json) ? "{}" : svToSv(items_json)
        );
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

int ClientPluginManager::updateToolDecor(
    ClientPluginInstance*   inst,
    AgentxxPluginStringView tool_call_id,
    AgentxxPluginStringView decor_json
) {
    if (!inst) {
        return -1;
    }
    const std::string tid  = svToStr(tool_call_id);
    const std::string json = svToStr(decor_json);

    // 删除语义: decor_json 空串 (tid 空 = 本插件全部)
    if (json.empty()) {
        {
            std::lock_guard<std::mutex> lock(uiMutex_);
            auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
            auto&                       vec = cur->toolDecors;
            vec.erase(
                std::remove_if(
                    vec.begin(),
                    vec.end(),
                    [&](const auto& d) {
                        return d.plugin == inst->name && (tid.empty() || d.toolCallId == tid);
                    }
                ),
                vec.end()
            );
            uiRegistry_ = std::move(cur);
        }
        auto& regs = inst->toolDecorRegs;
        regs.erase(
            std::remove_if(
                regs.begin(),
                regs.end(),
                [&](const auto& d) {
                    return tid.empty() || d.toolCallId == tid;
                }
            ),
            regs.end()
        );
        return 0;
    }

    // 更新/插入
    ClientToolDecor decor;
    try {
        auto j = neograph::json::parse(json);
        if (!j.is_object()) {
            return -1;
        }
        if (tid.empty()) {
            return -1; ///< 更新必须指明 tool_call_id (删除才允许空 = 全部)
        }
        decor.plugin      = inst->name;
        decor.toolCallId  = tid;
        decor.displayName = j.value("displayName", std::string{});
        decor.summary     = j.value("summary", std::string{});
        if (j.contains("items") && j["items"].is_array()) {
            decor.items = j["items"];
        }
    } catch (...) {
        return -1; ///< JSON 非法
    }

    bool replaced = false;
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        decor.version                   = toolDecorVersionSeq_++;
        for (auto& d : cur->toolDecors) {
            if (d.plugin == inst->name && d.toolCallId == tid) {
                d.displayName = decor.displayName;
                d.summary     = decor.summary;
                d.items       = decor.items;
                d.version     = decor.version;
                replaced      = true;
                break;
            }
        }
        if (!replaced) {
            cur->toolDecors.push_back(decor);
        }
        uiRegistry_ = std::move(cur);
    }
    // 实例注册信息同步 (disable/enable 恢复用); 无 adapter 信号 —— 装饰随
    // 正常帧节奏渲染 (工具消息本身的变化已驱动重绘)
    auto& regs = inst->toolDecorRegs;
    for (auto& d : regs) {
        if (d.toolCallId == tid) {
            d.displayName = decor.displayName;
            d.summary     = decor.summary;
            d.items       = decor.items;
            d.version     = decor.version;
            replaced      = true;
            break;
        }
    }
    if (!replaced) {
        regs.push_back(decor);
    }
    return 0;
}

int ClientPluginManager::registerCommand(
    ClientPluginInstance*   inst,
    AgentxxPluginStringView name,
    AgentxxPluginStringView description,
    int32_t(AGENTXX_PLUGIN_CALL*
                exec)(void*, const AgentxxPluginStringView*, AgentxxPluginString*, AgentxxPluginString*),
    void* ud
) {
    if (!inst || !exec || agentxx::plugin::PluginStringView::empty(name)) {
        return -1;
    }
    std::string nameStr = svToStr(name);
    std::string descStr = svToStr(description);
    // 命令输入管线接口 (agentxx.client.command): 无命令输入面的宿主拒绝注册 ——
    // 与其他 register_* 的接口门禁行为一致
    if (!hostSupportedInterfaces().contains(std::string{plugin_interfaces::ClientCommand})) {
        XX_LOGW(
            "[client_plugin] command `{}` rejected: interface agentxx.client.command unsupported",
            nameStr
        );
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        for (const auto& c : uiRegistry_->commands) {
            if (c.name == nameStr) {
                XX_LOGW("[client_plugin] command `{}` already registered", nameStr);
                return -1;
            }
        }
    }
    ClientCommand reg;
    reg.plugin      = inst->name;
    reg.name        = nameStr;
    reg.description = descStr;
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

int ClientPluginManager::unregisterCommand(
    ClientPluginInstance*   inst,
    AgentxxPluginStringView name
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(name)) {
        return -1;
    }
    std::string nameStr = svToStr(name);
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        auto&                       vec = cur->commands;
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if (it->name == nameStr && it->plugin == inst->name) {
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
                return c.name == nameStr && c.plugin == inst->name;
            }
        ),
        regs.end()
    );
    return 0;
}

AgentxxPluginSubscription* ClientPluginManager::subscribe(
    ClientPluginInstance* inst,
    int32_t               event,
    void(AGENTXX_PLUGIN_CALL* handler)(const AgentxxPluginStringView*, void*),
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
    return reinterpret_cast<AgentxxPluginSubscription*>(sub.get());
}

void ClientPluginManager::unsubscribe(AgentxxPluginSubscription* sub) {
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
    j["config"]      = inst->configPath;
    return j.dump();
}

std::string ClientPluginManager::getPluginArgsJson(ClientPluginInstance* inst) {
    if (!inst) {
        return "{}";
    }
    return inst->args.dump();
}

std::string ClientPluginManager::getPluginConfigPath(ClientPluginInstance* inst) {
    if (!inst) {
        return {};
    }
    return inst->configPath;
}

void ClientPluginManager::sendUserInputToPeer(
    ClientPluginInstance*   inst,
    AgentxxPluginStringView sessionId,
    AgentxxPluginStringView text
) {
    (void)sessionId; // 会话以当前绑定为准 (sessionId 不符时由端点兜底)
    if (!inst || !uiAdapter_) {
        return;
    }
    // 实际发送由 UI 适配器完成 (与用户输入同排队语义)
    uiAdapter_->sendPluginMessage(svToStr(text));
}

void ClientPluginManager::requestCancelToPeer(
    ClientPluginInstance*   inst,
    AgentxxPluginStringView sessionId
) {
    if (!inst || !uiAdapter_) {
        return;
    }
    uiAdapter_->requestCancel(svToStr(sessionId));
}

int ClientPluginManager::sendPluginDataToPeer(
    ClientPluginInstance*   inst,
    AgentxxPluginStringView event,
    AgentxxPluginStringView json
) {
    if (!inst || !uiAdapter_) {
        return -1;
    }
    std::string ev = svToStr(event);
    std::string j  = agentxx::plugin::PluginStringView::empty(json) ? "{}" : svToStr(json);
    return uiAdapter_->sendPluginData(inst->name, ev, j) ? 0 : -1;
}

namespace {

std::string truncateToolSummary(std::string_view s, size_t maxCols = 80) {
    const auto  nl = s.find('\n');
    std::string line{(nl == std::string_view::npos) ? s : s.substr(0, nl)};
    if (maxCols == 0 || line.empty()) {
        return {};
    }
    const auto idx = agentxx::util::findIndexByUtf8Length(line, maxCols);
    if (idx > 0 && idx < line.size()) {
        line.resize(idx);
        line += "...";
    }
    return line;
}

} // namespace

int ClientPluginManager::registerToolRenderer(
    ClientPluginInstance*        inst,
    const AgentxxToolRenderSpec* spec
) {
    if (!inst || !spec || spec->version != 1
        || agentxx::plugin::PluginStringView::empty(&spec->tool_name)) {
        return -1;
    }
    const std::string   tname = svToStr(spec->tool_name);
    ClientToolRenderReg reg;
    reg.plugin   = inst->name;
    reg.toolName = tname;
    reg.renderFn = spec->render_fn;
    reg.userData = spec->user_data;

    if (!spec->render_fn && !agentxx::plugin::PluginStringView::empty(&spec->template_json)) {
        reg.templateJson = svToStr(spec->template_json);
        try {
            auto j = neograph::json::parse(reg.templateJson);
            if (j.is_object()) {
                reg.templateDisplayName     = j.value("displayName", std::string{});
                reg.templateSummaryKey      = j.value("summaryKey", std::string{});
                reg.templateSummaryTemplate = j.value("summaryTemplate", std::string{});
            }
        } catch (...) {
            return -1;
        }
    }

    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur      = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        bool                        replaced = false;
        for (auto& r : cur->toolRenderers) {
            if (r.plugin == inst->name && r.toolName == tname) {
                r        = reg;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            cur->toolRenderers.push_back(reg);
        }
        uiRegistry_ = std::move(cur);
    }

    bool replacedInst = false;
    for (auto& r : inst->toolRenderRegs) {
        if (r.toolName == tname) {
            r            = reg;
            replacedInst = true;
            break;
        }
    }
    if (!replacedInst) {
        inst->toolRenderRegs.push_back(reg);
    }
    return 0;
}

int ClientPluginManager::unregisterToolRenderer(
    ClientPluginInstance*   inst,
    AgentxxPluginStringView tool_name
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&tool_name)) {
        return -1;
    }
    const std::string tname = svToStr(tool_name);
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        cur->toolRenderers.erase(
            std::remove_if(
                cur->toolRenderers.begin(),
                cur->toolRenderers.end(),
                [&](const auto& r) {
                    return r.plugin == inst->name && r.toolName == tname;
                }
            ),
            cur->toolRenderers.end()
        );
        uiRegistry_ = std::move(cur);
    }
    inst->toolRenderRegs.erase(
        std::remove_if(
            inst->toolRenderRegs.begin(),
            inst->toolRenderRegs.end(),
            [&](const auto& r) {
                return r.toolName == tname;
            }
        ),
        inst->toolRenderRegs.end()
    );
    return 0;
}

ClientToolRenderResult renderClientTool(
    const ClientUiRegistry* reg,
    std::string_view        toolCallId,
    std::string_view        toolName,
    std::string_view        argsJson,
    std::string_view        resultText,
    bool                    isFinished,
    bool                    isError,
    int                     maxWidth
) {
    ClientToolRenderResult res;
    if (!reg) {
        return res;
    }

    // 1. 优先匹配动态 per-call decor (由 update_tool_decor 针对 toolCallId 注册)
    if (!toolCallId.empty()) {
        for (const auto& d : reg->toolDecors) {
            if (d.toolCallId == toolCallId) {
                res.displayName = d.displayName;
                res.summary     = d.summary;
                res.items       = d.items;
                res.matched     = true;
                res.isDecor     = true;
                return res;
            }
        }
    }

    // 2. 匹配按 toolName 注册的工具特化渲染器 (render_fn 或 预设模版)
    if (!toolName.empty()) {
        for (const auto& r : reg->toolRenderers) {
            if (r.toolName == toolName) {
                if (r.renderFn) {
                    AgentxxToolRenderInput input{};
                    input.version      = 1;
                    input.tool_call_id = agentxx::plugin::PluginStringView::from(
                        toolCallId.data(),
                        toolCallId.size()
                    );
                    input.tool_name
                        = agentxx::plugin::PluginStringView::from(toolName.data(), toolName.size());
                    input.args_json
                        = agentxx::plugin::PluginStringView::from(argsJson.data(), argsJson.size());
                    input.result_text = agentxx::plugin::PluginStringView::from(
                        resultText.data(),
                        resultText.size()
                    );
                    input.is_finished = isFinished ? 1 : 0;
                    input.is_error    = isError ? 1 : 0;
                    input.max_width   = maxWidth;

                    AgentxxToolRenderOutput output{};
                    int32_t                 rc = -1;
                    try {
                        rc = r.renderFn(r.userData, &input, &output);
                    } catch (...) {
                        rc = -1;
                    }
                    if (rc == 0) {
                        res.matched = true;
                        if (output.displayName.data) {
                            res.displayName.assign(
                                output.displayName.data,
                                static_cast<size_t>(output.displayName.size)
                            );
                            agentxx::plugin::hostMemoryFree(output.displayName.data);
                        }
                        if (output.summary.data) {
                            res.summary.assign(
                                output.summary.data,
                                static_cast<size_t>(output.summary.size)
                            );
                            agentxx::plugin::hostMemoryFree(output.summary.data);
                        }
                        if (output.items_json.data) {
                            try {
                                res.items = neograph::json::parse(std::string_view{
                                    output.items_json.data,
                                    static_cast<size_t>(output.items_json.size)
                                });
                            } catch (...) {
                            }
                            agentxx::plugin::hostMemoryFree(output.items_json.data);
                        }
                        return res;
                    }
                } else if (!r.templateDisplayName.empty() || !r.templateSummaryKey.empty()) {
                    res.displayName = r.templateDisplayName;
                    res.matched     = true;

                    if (!r.templateSummaryKey.empty() && !argsJson.empty()) {
                        try {
                            auto j = neograph::json::parse(argsJson);
                            if (j.is_object() && j.contains(r.templateSummaryKey)) {
                                const auto& val = j[r.templateSummaryKey];
                                std::string rawVal;
                                if (val.is_string()) {
                                    rawVal = val.get<std::string>();
                                } else if (val.is_array()) {
                                    std::string joined;
                                    size_t      count = 0;
                                    for (const auto& item : val) {
                                        if (count > 0) {
                                            joined += ", ";
                                        }
                                        if (item.is_string()) {
                                            joined += item.get<std::string>();
                                        } else {
                                            joined += item.dump();
                                        }
                                        if (++count >= 2 && val.size() > 2) {
                                            joined += ", ...";
                                            break;
                                        }
                                    }
                                    rawVal = std::move(joined);
                                } else {
                                    rawVal = val.dump();
                                }
                                const size_t limit
                                    = (maxWidth > 20) ? static_cast<size_t>(maxWidth - 15) : 80;
                                res.summary = truncateToolSummary(rawVal, limit);
                            }
                        } catch (...) {
                            res.matched = false;
                            res.displayName.clear();
                            return res;
                        }
                    }
                    return res;
                }
            }
        }
    }

    return res;
}

} // namespace plugin
} // namespace agentxx
