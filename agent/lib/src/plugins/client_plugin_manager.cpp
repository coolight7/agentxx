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
#include "agentxx/plugin/plugin_manager.h"
#include "pluginxx/runtime/driver.h"
#include "pluginxx/runtime/op_driver.h"
#include "utilxx_base/container_util.h"

#include "agentxx/agent/io/wire_protocol.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "asio/thread_pool.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "utilxx/async_offload.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
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

/// 全局快捷键宿主句柄实现
struct AgentxxKeybind {
    agentxx::plugin::ClientPluginInstance* inst = nullptr;
    std::string                            keys; ///< 规范化后的键位描述
};

namespace agentxx {
namespace plugin {

using agentxx::agent::PluginConfig;

namespace {

// ==================== 快捷键描述解析 ====================

/// 两端去空白 (含 tab/换行)
std::string_view trimAscii(std::string_view s) {
    size_t begin = 0;
    size_t end   = s.size();
    auto   space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    while (begin < end && space(s[begin])) {
        ++begin;
    }
    while (end > begin && space(s[end - 1])) {
        --end;
    }
    return s.substr(begin, end - begin);
}

/// 修饰键别名归一 (非修饰键返回空串)
std::string_view modifierOf(std::string_view token) {
    if (token == "ctrl" || token == "control") {
        return "ctrl";
    }
    if (token == "alt" || token == "option") {
        return "alt";
    }
    if (token == "shift") {
        return "shift";
    }
    if (token == "super" || token == "cmd" || token == "win" || token == "meta") {
        return "super";
    }
    return {};
}

/// 主键别名归一; 非法主键返回空串 (允许的主键见 client_plugin_api.h 的说明)
std::string keyOf(std::string_view token) {
    if (token.empty()) {
        return {};
    }
    if (token.size() == 1) {
        return std::string{token}; // 可打印单字符 (a-z/0-9/符号)
    }
    static const char* kNames[] = {
        "esc",    "enter", "tab",     "space",   "backspace", "delete",   "insert",
        "up",     "down",  "left",    "right",   "home",      "end",      "pageup",
        "pagedown",
    };
    for (const char* name : kNames) {
        if (token == name) {
            return std::string{token};
        }
    }
    // 别名
    if (token == "escape") {
        return "esc";
    }
    if (token == "return") {
        return "enter";
    }
    if (token == "spacebar") {
        return "space";
    }
    if (token == "del") {
        return "delete";
    }
    if (token == "ins") {
        return "insert";
    }
    if (token == "pgup" || token == "page_up") {
        return "pageup";
    }
    if (token == "pgdn" || token == "pagedn" || token == "page_down") {
        return "pagedown";
    }
    // F1~F24
    if (token.size() >= 2 && token[0] == 'f') {
        int         num  = 0;
        bool        ok   = true;
        for (size_t i = 1; i < token.size(); ++i) {
            const char c = token[i];
            if (c < '0' || c > '9') {
                ok = false;
                break;
            }
            num = num * 10 + (c - '0');
        }
        if (ok && num >= 1 && num <= 24) {
            return std::string{token};
        }
    }
    return {};
}

} // namespace

std::string normalizeKeybindSpec(std::string_view keys) {
    std::string lower = utilxx_base::toLower(std::string{trimAscii(keys)});
    if (lower.empty()) {
        return {};
    }
    bool hasCtrl  = false;
    bool hasAlt   = false;
    bool hasShift = false;
    bool hasSuper = false;
    bool hasMod   = false;
    std::string mainKey;
    size_t      begin = 0;
    while (begin <= lower.size()) {
        const size_t pos   = lower.find('+', begin);
        const size_t end   = (pos == std::string::npos) ? lower.size() : pos;
        auto         token = trimAscii(std::string_view{lower}.substr(begin, end - begin));
        if (token.empty()) {
            return {}; // 空段 ("ctrl++k" / 首尾多余 '+') 视为非法
        }
        if (!mainKey.empty()) {
            return {}; // 主键之后不允许再出现段 (主键必须在最后)
        }
        if (auto mod = modifierOf(token); !mod.empty()) {
            hasMod = true;
            if (mod == "ctrl") {
                hasCtrl = true;
            } else if (mod == "alt") {
                hasAlt = true;
            } else if (mod == "shift") {
                hasShift = true;
            } else {
                hasSuper = true;
            }
        } else {
            mainKey = keyOf(token);
            if (mainKey.empty()) {
                return {}; // 未知主键
            }
        }
        if (pos == std::string::npos) {
            break;
        }
        begin = pos + 1;
    }
    if (mainKey.empty()) {
        return {}; // 只有修饰键
    }
    // 无修饰键的可打印单字符不参与匹配 (会与输入框抢字符)
    if (!hasMod && mainKey.size() == 1) {
        return {};
    }
    std::string out;
    if (hasCtrl) {
        out += "ctrl+";
    }
    if (hasAlt) {
        out += "alt+";
    }
    if (hasShift) {
        out += "shift+";
    }
    if (hasSuper) {
        out += "super+";
    }
    out += mainKey;
    return out;
}

namespace {

// ==================== 工具 ====================
/// 解析 action 动作 JSON: {"action": "send"|"toast"|"none", ...}
/// 返回 true 表示 action 字段可识别 (含 none); false 表示非法/空
bool parseCommandAction(const std::string& jsonText, std::string& action) {
    action.clear();
    if (jsonText.empty() || jsonText == "{}") {
        return false; // 空结果 = 已处理完毕, 无动作
    }
    try {
        auto j = utilxx_base::Json::parse(jsonText);
        if (!j.is_object()) {
            return false;
        }
        action = j.value("action", "");
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
    if (lifecycleStopPending()) {
        // stop 从未执行：destroy 会看到不完整的插件状态。析构只能保留上下文
        // 与 DSO (宁可泄漏也不能 dlclose 后让残留回调跳入已卸载代码)。
        XX_LOGE(
            "[client_plugin] `{}` destroyed with lifecycle stop pending; keeping plugin "
            "context and DSO loaded (owner must call shutdownAsync before destruction)",
            name
        );
        return;
    }
    if (!destroyPlugin()) {
        XX_LOGE(
            "[client_plugin] `{}` destroyed while {} lease(s) remain; refusing to dlclose",
            name,
            lifetime ? lifetime->leaseCount() : 0
        );
        return;
    }
    if (dlHandle) {
        NativeLoader::close(dlHandle);
        dlHandle = nullptr;
    }
    clientSubscriptions.clear();
    statusItemHandles.clear();
    panelHandles.clear();
    infoSectionHandles.clear();
    subHandles.clear();
}

// =====================================================================
// ClientPluginManager
// =====================================================================

ClientPluginManager::ClientPluginManager(asio::any_io_executor ex) :
    pluginxx::PluginHostLifecycle<ClientPluginInstance>(std::move(ex)),
    pool_(std::make_unique<asio::thread_pool>(1)),
    uiRegistry_(std::make_shared<const ClientUiRegistry>()) {}

ClientPluginManager::~ClientPluginManager() {
    shutdownAll();
    warnPendingCloseOnDestroy("client");
    if (pool_) {
        pool_->join();
    }
}

// ==================== pluginxx::PluginHostLifecycle 宿主接缝 ====================

/// 生成 client 侧实例对象
/// - 领域自引用 (self) 与所属管理器 (manager 弱引用);
/// - 其余公共部分 (生命周期入口 / 基类自引用 / 生命周期控制块 / 宿主控制块)
///   由 [attachInstance] 在装载路径中统一装配。
std::shared_ptr<ClientPluginInstance> ClientPluginManager::createInstance(std::string name) {
    auto inst     = std::make_shared<ClientPluginInstance>(std::move(name));
    inst->manager = weak_from_this();
    inst->self    = inst;
    return inst;
}

/// 实例加载完成: 登记实例代次 (UI 点击携带代次, 派发时复查; 重载同名插件后
/// 旧代次的点击会被丢弃, 不会转交新实例)
void ClientPluginManager::onInstanceLoaded(ClientPluginInstance& inst) {
    setRegistryGeneration(inst.name, inst.lifetime ? inst.lifetime->generation() : 0, true);
}

/// 实例从插件表摘除: 清除实例代次登记
void ClientPluginManager::onInstanceUnloaded(ClientPluginInstance& inst) {
    setRegistryGeneration(inst.name, 0, false);
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
    // - 依赖解析与正式加载合并: 不再先 dlopen 探测再 close 后重新
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
    void*       handle = co_await utilxx::offloadAsync<void*>(
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

    // 预占名称覆盖 entry 在线程池运行期间的并发重复加载。
    if (!reservePluginName(name)) {
        XX_LOGE("[client_plugin] duplicate or currently loading plugin name `{}`", name);
        NativeLoader::close(handle);
        co_return nullptr;
    }

    // ---- 接口协商限制 (三层协商第 2 层; 见
    //      [plugin_interfaces.h](/agent/lib/include/agentxx/plugin/plugin_interfaces.h) 接口协商节)
    //      ----
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
            utilxx_base::insertOrAssignHeterogeneous(
                skippedPlugins_,
                name,
                "missing required interfaces: " + missing
            );
            releasePluginName(name);
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

    // entry 入口 (必需): 探测与加载合并 —— 一次 dlopen 内查符号
    std::string entryErr;
    auto        entryFn = reinterpret_cast<AgentxxClientPluginCreateFn>(
        NativeLoader::sym(handle, AGENTXX_PLUGIN_CLIENT_SYMBOL_CREATE, entryErr)
    );
    std::string lifecycleErr;
    auto        lifecycleStart = reinterpret_cast<PluginxxStartFn>(
        NativeLoader::sym(handle, AGENTXX_PLUGIN_CLIENT_SYMBOL_START, lifecycleErr)
    );
    lifecycleErr.clear();
    auto lifecycleStop = reinterpret_cast<PluginxxStopFn>(
        NativeLoader::sym(handle, AGENTXX_PLUGIN_CLIENT_SYMBOL_STOP, lifecycleErr)
    );
    // start/stop 是必备入口 (create 只构造, start 注册, stop 撤销):
    // 缺少任一符号说明插件未按当前契约导出, 直接拒绝加载。
    if (entryFn && (!lifecycleStart || !lifecycleStop)) {
        XX_LOGE(
            "[client_plugin] `{}` missing lifecycle entry ({}: {}; {}: {}); plugins must export "
            "start/stop",
            path,
            AGENTXX_PLUGIN_CLIENT_SYMBOL_START,
            lifecycleStart ? "ok" : lifecycleErr,
            AGENTXX_PLUGIN_CLIENT_SYMBOL_STOP,
            lifecycleStop ? "ok" : lifecycleErr
        );
        releasePluginName(name);
        NativeLoader::close(handle);
        co_return nullptr;
    }
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
        releasePluginName(name);
        NativeLoader::close(handle);
        co_return nullptr;
    }

    // 依赖检查: 必选缺失 → 失败; 可选缺失 → 警告
    for (const auto& d : depends) {
        if (plugins_.count(d) == 0) {
            XX_LOGE("[client_plugin] `{}` depends on missing plugin `{}`", name, d);
            releasePluginName(name);
            NativeLoader::close(handle);
            co_return nullptr;
        }
    }
    for (const auto& d : optionalDepends) {
        if (plugins_.count(d) == 0) {
            XX_LOGW("[client_plugin] `{}` optional dependency `{}` not installed", name, d);
        }
    }

    auto inst         = createInstance(name);
    inst->version     = version;
    inst->description = desc;
    inst->path        = path;
    // 插件配置参数随加载直接传入 (与 agent 侧一致): 宿主不解析字段语义,
    // 插件经 vtable get_plugin_args 整体读取; 直连路径 cfg 为 nullptr → {}
    inst->args            = cfg ? cfg->args : utilxx_base::Json::object();
    inst->configPath      = cfg ? cfg->configPath : std::string{};
    inst->dlHandle        = handle;
    inst->depends         = std::move(depends);
    inst->optionalDepends = std::move(optionalDepends);
    inst->interfaces      = std::move(interfaces);
    // 生命周期入口 + 基类自引用 + 生命周期控制块 + 交给自己插件的宿主控制块
    // (host 视图放在进程级稳定的控制块里: 插件可能保存该指针并在卸载后继续调用,
    // 控制块 tombstone 保证这类迟到调用安全失败)
    attachInstance(inst, lifecycleStart, lifecycleStop);

    // entry 卸载到内部线程池执行 (A2): 与 agent 侧一致 —— entry 内 vtable
    // 注册动作经 ioCallSync 回 io 线程同步执行; entry 在 io 线程执行会阻塞
    // client io 事件循环 (慢初始化/插件间调用时明显), 且违背契约声明的
    // "entry 运行在宿主线程池";
    // 插件违约抛异常按 rc=-1 处理 (加载失败清理路径)
    int rc = co_await utilxx::offloadAsync<int>(*pool_, [inst, entryFn]() -> asio::awaitable<int> {
        try {
            const auto rc       = entryFn(inst->hostView(), &inst->pluginCtx);
            inst->pluginCreated = (inst->pluginCtx != nullptr);
            co_return rc;
        } catch (const std::exception& e) {
            XX_LOGE("[client_plugin] `{}` entry threw: {}", inst->name, e.what());
        } catch (...) {
            XX_LOGE("[client_plugin] `{}` entry threw unknown exception", inst->name);
        }
        co_return -1;
    });
    if (rc != 0) {
        XX_LOGE("[client_plugin] `{}` entry failed (rc={})", name, rc);
        detachAll(inst.get());
        inst->destroyPlugin();
        releasePluginName(name);
        // inst 随局部释放析构 → ~ClientPluginInstance → dlclose
        co_return nullptr;
    }

    std::string startError;
    if (!co_await awaitPluginLifecycle(
            runtime(),
            inst,
            inst->pluginCtx,
            inst->lifecycleStart,
            "client plugin start",
            startError
        )) {
        XX_LOGE("[client_plugin] `{}` start failed: {}", name, startError);
        detachAll(inst.get());
        inst->destroyPlugin();
        releasePluginName(name);
        if (inst->dlHandle) {
            NativeLoader::close(inst->dlHandle);
            inst->dlHandle = nullptr;
        }
        co_return nullptr;
    }
    inst->lifecycleStarted = true;

    inst->lifetime->setState(pluginxx::PluginInstanceState::Ready);
    utilxx_base::insertHeterogeneous(plugins_, std::string{name}, inst);
    // 实例代次登记 (UI 点击携带代次, 派发时复查; 见 onInstanceLoaded)
    onInstanceLoaded(*inst);
    releasePluginName(name);
    XX_LOGI("[client_plugin] loaded: {} ({})", name, version);
    co_return inst;
}

// ==================== 生命周期 (继承自 pluginxx::PluginHostLifecycle) ====================
//
// 卸载 / 关闭等待 / 禁用启用 / 级联依赖 / destroy 与 dlclose 的骨架在 cxx_pluginxx
// (见 pluginxx/host/lifecycle.h): 这些逻辑与宿主领域无关, client 与 agent 两侧
// 共用同一份实现 (含失败回滚、关闭超时、停止后重试、级联依赖)。
// 本类只保留两处 client 特有差异:
// - unloadAsync 的默认超时较短 (UI 交互路径, 见头文件);
// - 卸载级联只统计"启用中"的依赖者 (cascadeUnloadEnabledOnly)。
// 启停事务 (stop/start) 触达插件侧注册时, 领域动作经 detachDomainRegistrations 注入。

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

    // 宿主支持接口集 (加载前计算一次; 三层协商第 2 层的 require 限制数据源)
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
                // 接口协商限制 (dlopen 前跳过): require 未满足 → 记录原因并
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
                    utilxx_base::insertOrAssignHeterogeneous(
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

    // 依次加载 (Auto 无 client 入口时由 loadNativeAsync 静默跳过:
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
        v.inflight           = inst->lifetime ? inst->lifetime->leaseCount() : 0;
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

// ==================== 工具语义渲染缓存 ====================

namespace {

/// FNV-1a: 输入特征哈希 (跨平台确定, 与 std::hash 实现无关)
void hashBytes(uint64_t& h, std::string_view bytes) {
    for (const unsigned char c : bytes) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    h ^= 0xFFu; // 字段分隔, 避免 ("ab","c") 与 ("a","bc") 同值
    h *= 1099511628211ULL;
}

} // namespace

std::string
    ClientToolRenderRequest::keyFor(std::string_view toolCallId, std::string_view toolName) {
    if (!toolCallId.empty()) {
        return std::string{toolCallId};
    }
    std::string key{"#"};
    key.append(toolName);
    return key;
}

uint64_t ClientToolRenderRequest::inputHash() const {
    uint64_t h = 1469598103934665603ULL;
    hashBytes(h, toolName);
    hashBytes(h, argsJson);
    hashBytes(h, resultText);
    hashBytes(h, isFinished ? "1" : "0");
    hashBytes(h, isError ? "1" : "0");
    hashBytes(h, std::to_string(maxWidth));
    return h;
}

std::shared_ptr<const ClientToolRenderEntry>
    ClientToolRenderCache::lookup(const std::string& key, uint64_t inputHash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = entries_.find(key);
    if (it == entries_.end() || it->second->inputHash != inputHash) {
        return nullptr;
    }
    return it->second;
}

uint64_t ClientToolRenderCache::version(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = versions_.find(key);
    return it == versions_.end() ? 0 : it->second;
}

std::shared_ptr<const ClientToolRenderEntry>
    ClientToolRenderCache::store(ClientToolRenderEntry entry) {
    auto snapshot = std::make_shared<const ClientToolRenderEntry>(std::move(entry));
    std::lock_guard<std::mutex> lock(mutex_);
    // 首次写入 (含淘汰后重写) 才登记顺序; 已有版本记录的键保留原顺序位置
    if (versions_.find(snapshot->key) == versions_.end()) {
        order_.push_back(snapshot->key);
    }
    ++versions_[snapshot->key];
    entries_[snapshot->key] = snapshot;
    evictLocked();
    return snapshot;
}

void ClientToolRenderCache::invalidatePlugin(std::string_view plugin) {
    const std::string           owner{plugin};
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second->plugin == owner) {
            ++versions_[it->first]; // 版本号变化驱动 UI 重建为通用回退
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    // 被失效的键保留版本记录 (旧快照据此重建), 由 evictLocked 按顺序回收,
    // 保证长时间会话中版本记录同样有界。
    evictLocked();
}

void ClientToolRenderCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& key : order_) {
        ++versions_[key];
    }
    entries_.clear();
    evictLocked();
}

size_t ClientToolRenderCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void ClientToolRenderCache::evictLocked() {
    while (order_.size() > maxEntries_) {
        std::string key = std::move(order_.front());
        order_.pop_front();
        entries_.erase(key);
        versions_.erase(key);
    }
}

bool ClientToolRenderCache::beginRequest(const std::string& key, uint64_t inputHash) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = pending_.find(key);
    if (it != pending_.end() && it->second == inputHash) {
        return false;
    }
    pending_[key] = inputHash;
    return true;
}

void ClientToolRenderCache::endRequest(const std::string& key, uint64_t inputHash) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = pending_.find(key);
    if (it != pending_.end() && it->second == inputHash) {
        pending_.erase(it);
    }
}

/// 登记/清除插件实例代次 (UI 点击携带代次, io 线程复查; 见
/// [ClientUiRegistry::instanceGenerations])
void ClientPluginManager::setRegistryGeneration(
    std::string_view plugin,
    uint64_t         generation,
    bool             present
) {
    if (plugin.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(uiMutex_);
    auto                        reg = std::make_shared<ClientUiRegistry>(*uiRegistry_);
    if (present) {
        reg->instanceGenerations[std::string{plugin}] = generation;
    } else {
        reg->instanceGenerations.erase(std::string{plugin});
    }
    uiRegistry_ = std::move(reg);
}

std::shared_ptr<const ClientToolRenderEntry>
    ClientPluginManager::requestToolRender(const ClientToolRenderRequest& req) {
    if (req.toolName.empty()) {
        return nullptr;
    }
    const std::string key   = ClientToolRenderRequest::keyFor(req.toolCallId, req.toolName);
    const uint64_t    input = req.inputHash();
    if (auto cached = toolRenderCache_->lookup(key, input)) {
        return cached;
    }
    if (!toolRenderCache_->beginRequest(key, input)) {
        // 已有同键同输入特征的请求在执行, 本次只回退通用渲染
        return nullptr;
    }
    // 未命中: 拷贝输入后投递到 client io 线程执行 renderer (UI 线程不进入插件代码)。
    // UI 线程与 io 线程是同一线程时 postToIo 会内联执行, 此时结果已在缓存里,
    // 直接再查一次即可返回, 不必等下一帧。
    auto self = shared_from_this();
    try {
        postToIo([self, req, key, input]() mutable {
            self->performToolRender(std::move(req), std::move(key), input);
        });
    } catch (const std::exception& e) {
        XX_LOGW("[client_plugin] tool render request dropped: {}", e.what());
        toolRenderCache_->endRequest(key, input);
        return nullptr;
    }
    return toolRenderCache_->lookup(key, input);
}

/// 在 client io 线程执行自定义 renderer 并把结果拷成宿主语义快照。
/// - 渲染器已摘除/失效/实例禁用 → 写入未命中条目 (UI 回退通用渲染)
/// - 插件回调返回失败 → 同样写未命中条目, 避免每帧重复请求
void ClientPluginManager::performToolRender(
    ClientToolRenderRequest req,
    std::string             key,
    uint64_t                inputHash
) {
    // 无论成功/失败/提前返回都要清掉在途标记, 否则该键再也不会重新渲染
    struct PendingGuard {
        ClientToolRenderCache* cache;
        std::string            key;
        uint64_t               inputHash;

        ~PendingGuard() {
            cache->endRequest(key, inputHash);
        }
    } pendingGuard{toolRenderCache_.get(), key, inputHash};

    ClientToolRenderEntry entry;
    entry.key       = key;
    entry.inputHash = inputHash;

    std::shared_ptr<const ClientUiRegistry> snapshot;
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        snapshot = uiRegistry_;
    }
    // 快照强引用保活: hit 指向快照内的注册记录, 后续插件回调期间不得失效
    // 查找顺序与 [renderClientTool] 一致: 插件注册项优先, 未命中再查宿主内置项
    const ClientToolRenderReg* hit = nullptr;
    for (const auto& r : snapshot->toolRenderers) {
        if (r.toolName == req.toolName && r.renderFn) {
            hit = &r;
            break;
        }
    }
    if (!hit) {
        for (const auto& r : snapshot->builtinToolRenderers) {
            if (r.toolName == req.toolName && r.renderFn) {
                hit = &r;
                break;
            }
        }
    }
    if (!hit) {
        toolRenderCache_->store(std::move(entry));
        return;
    }
    entry.plugin = hit->plugin;

    /// 调用渲染器回调 → 拷贝输出 → 写缓存并通知 UI 重绘。
    /// 定义成闭包: 插件渲染器分支的 InflightGuard 须存活到回调返回,
    /// 用内部作用域 + 闭包调用可避免其在回调前析构。
    auto runRenderer = [&]() {
        AgentxxToolRenderInput input{};
        input.version      = 1;
        input.tool_call_id = agentxx::plugin::PluginStringView::from(req.toolCallId);
        input.tool_name    = agentxx::plugin::PluginStringView::from(req.toolName);
        input.args_json    = agentxx::plugin::PluginStringView::from(req.argsJson);
        input.result_text  = agentxx::plugin::PluginStringView::from(req.resultText);
        input.is_finished  = req.isFinished ? 1 : 0;
        input.is_error     = req.isError ? 1 : 0;
        input.max_width    = req.maxWidth;

        AgentxxToolRenderOutput output{};
        int32_t                 rc = -1;
        try {
            rc = hit->renderFn(hit->userData, &input, &output);
        } catch (...) {
            rc = -1;
        }
        if (rc == 0) {
            entry.matched = true;
            if (output.displayName.data) {
                entry.displayName.assign(
                    output.displayName.data,
                    static_cast<size_t>(output.displayName.size)
                );
            }
            if (output.summary.data) {
                entry.summary.assign(output.summary.data, static_cast<size_t>(output.summary.size));
            }
            if (output.items_json.data) {
                if (acceptUiJsonSize(
                        std::string_view{
                            output.items_json.data,
                            static_cast<size_t>(output.items_json.size)
                        },
                        "tool_renderer output",
                        hit->plugin
                    )) {
                    try {
                        entry.items = utilxx_base::Json::parse(std::string_view{
                            output.items_json.data,
                            static_cast<size_t>(output.items_json.size)
                        });
                    } catch (...) {
                    }
                }
            }
        }
        // 输出字段无论成功/失败/异常路径都由宿主释放
        hostMemoryFree(output.displayName.data);
        hostMemoryFree(output.summary.data);
        hostMemoryFree(output.items_json.data);

        toolRenderCache_->store(std::move(entry));
        if (uiAdapter_) {
            uiAdapter_->onToolRenderUpdated(req.toolCallId, req.toolName);
        }
    };

    // 宿主内置渲染器: 进程生命周期函数 (宿主自身实现), 无插件实例/租约,
    // 不经插件禁用/卸载路径, 直接执行
    if (hit->builtin) {
        runRenderer();
        return;
    }

    if (auto rendererOwner = hit->lease ? hit->lease->instance.lock() : nullptr) {
        entry.generation = rendererOwner->lifetime ? rendererOwner->lifetime->generation() : 0;
    }

    std::shared_ptr<ClientPluginInstance> owner;
    if (!hit->lease || !hit->lease->alive.load(std::memory_order_acquire)) {
        toolRenderCache_->store(std::move(entry));
        return;
    }
    owner = hit->lease->instance.lock();
    if (!owner || !owner->enabled || !owner->lifetime || !owner->lifetime->acceptsOperations()) {
        toolRenderCache_->store(std::move(entry));
        return;
    }
    PluginInstanceBase::InflightGuard guard(owner);
    if (!guard || !hit->lease->alive.load(std::memory_order_acquire) || !owner->enabled
        || !owner->lifetime->acceptsOperations()) {
        toolRenderCache_->store(std::move(entry));
        return;
    }

    // 租约仍有效: 回调期间实例由 guard 保活 (禁止卸载 dlclose)
    runRenderer();
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

    PluginInstanceBase::InflightGuard guard(inst->self.lock());
    PluginxxString                    err{nullptr, 0};
    PluginxxString                    out{nullptr, 0};
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
        agentxx::plugin::PluginString::free(inst->hostView(), &err);
    }
    if (out.data) {
        agentxx::plugin::PluginString::free(inst->hostView(), &out);
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
            auto j = utilxx_base::Json::parse(actionJson);
            uiAdapter_->onToast(j.value("text", ""), j.value("level", 0));
        } catch (...) {
            uiAdapter_->onToast("(plugin toast)", 0);
        }
        return;
    }
    if (action == "send") {
        try {
            auto j = utilxx_base::Json::parse(actionJson);
            uiAdapter_->sendPluginMessage(j.value("text", ""));
        } catch (const std::exception& e) {
            XX_LOGE("[client_plugin] invalid send action: {}", e.what());
        }
        return;
    }
}

// ==================== 会话上下文 ====================

std::string ClientPluginManager::clientStateJson() const {
    utilxx_base::Json j  = utilxx_base::Json::object();
    j["sessionId"]       = sessionId_;
    j["connState"]       = connState_;
    j["startupProgress"] = startupProgress_;
    // 宿主支持的接口名清单 (三层协商第 3 层 —— 插件据此自行决定启用哪些
    // 功能; 见 [plugin_interfaces.h](/agent/lib/include/agentxx/plugin/plugin_interfaces.h)
    // 接口协商节)。位图 uiCaps 字段已移除 (v4)
    j["interfaces"] = [&] {
        auto arr = utilxx_base::Json::array();
        for (const auto& n : hostSupportedInterfaces()) {
            arr.push_back(n);
        }
        return arr;
    }();
    // 服务端已加载的 agent 侧插件结构化列表 [{name,version,interfaces},...]
    // (空数组 = 未知, 见成员注释)
    j["agentPlugins"] = [&] {
        auto arr = utilxx_base::Json::array();
        for (const auto& p : serverPlugins_) {
            arr.push_back({
                {"name",       p.name      },
                {"version",    p.version   },
                {"interfaces", p.interfaces}
            });
        }
        return arr;
    }();
    // 展示区域尺寸快照 [{id,w,h},...]: 面板/Info 段落的可用宽高 (宿主布局后写入;
    // 插件据此按可用宽度自行重排; 事件 AGENTXX_CLIENT_EVT_UI_LAYOUT 会主动通知变化)
    j["regions"] = [&] {
        auto arr = utilxx_base::Json::array();
        for (const auto& r : regionSizes()) {
            auto item  = utilxx_base::Json::object();
            item["id"] = r.id;
            item["w"]  = r.width;
            item["h"]  = r.height;
            arr.push_back(std::move(item));
        }
        return arr;
    }();
    return j.dump();
}

// ==================== ClientEventSink 实现 ====================

void ClientPluginManager::onReady() {
    utilxx_base::Json j = utilxx_base::Json::object();
    // 宿主支持的接口名清单 (启动后最早可得的协商结果, 插件在 READY 回调内
    // 即可完成功能启用决策; 位图 uiCaps 字段已移除, 见
    // [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h) v4)
    j["interfaces"] = [&] {
        auto arr = utilxx_base::Json::array();
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
        utilxx_base::Json up = utilxx_base::Json::object();
        up["sessionId"]      = sessionId_;
        up["interfaces"]     = j["interfaces"];
        uiAdapter_->sendPluginData("agentxx_host", "client_interfaces", up.dump());
    }
}

void ClientPluginManager::onConnStateChanged(std::string_view state, std::string_view progress) {
    connState_           = std::string{state};
    startupProgress_     = std::string{progress};
    utilxx_base::Json j  = utilxx_base::Json::object();
    j["connState"]       = connState_;
    j["startupProgress"] = startupProgress_;
    dispatchEvent(AGENTXX_CLIENT_EVT_CONN_STATE, j.dump());
}

void ClientPluginManager::onUserInput(std::string_view sessionId, std::string_view text) {
    utilxx_base::Json j = utilxx_base::Json::object();
    j["sessionId"]      = std::string{sessionId};
    j["text"]           = std::string{text};
    dispatchEvent(AGENTXX_CLIENT_EVT_USER_INPUT, j.dump());
}

void ClientPluginManager::onDelta(const agentxx::agent::WireDelta& delta) {
    dispatchEvent(AGENTXX_CLIENT_EVT_DELTA, agentxx::agent::io::deltaToJson(delta).dump());
}

void ClientPluginManager::onTurnResult(const agentxx::agent::WireTurnResult& result) {
    utilxx_base::Json j = utilxx_base::Json::object();
    j["sessionId"]      = result.sessionId;
    j["hasError"]       = result.hasError;
    j["interrupted"]    = result.interrupted;
    if (!result.errorMessage.empty()) {
        j["errorMessage"] = result.errorMessage;
    }
    j["startTimeMs"] = result.startTimeMs;
    j["durationMs"]  = result.durationMs;
    dispatchEvent(AGENTXX_CLIENT_EVT_TURN_END, j.dump());
}

void ClientPluginManager::onSessionSwitched(std::string_view sessionId) {
    sessionId_ = std::string{sessionId};
    // 会话切换后旧的按 tool_call_id 语义结果不再有效, 整体失效 (UI 回退通用渲染)
    toolRenderCache_->clear();
    utilxx_base::Json j = utilxx_base::Json::object();
    j["sessionId"]      = sessionId_;
    dispatchEvent(AGENTXX_CLIENT_EVT_SESSION_SWITCH, j.dump());
}

void ClientPluginManager::onPluginData(const agentxx::agent::WirePluginData& data) {
    // 宿主约定事件 (server 端 SessionServerAgentIO 发布, 见该文件 kHostPluginName):
    // server_plugins → 记录服务端已加载插件结构化信息 [{name,version,interfaces},...],
    // 供 get_client_state ("agentPlugins") 查询对端可用性与能力; 其余约定
    // 事件照常向插件分发
    if (data.plugin == "agentxx_host" && data.event == "server_plugins") {
        try {
            auto                          j = utilxx_base::Json::parse(data.data);
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
        for (const auto& sub : inst->clientSubscriptions) {
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

    utilxx_base::Json j = utilxx_base::Json::object();
    j["plugin"]         = data.plugin;
    j["event"]          = data.event;
    j["data"]           = data.data;
    dispatchEvent(AGENTXX_CLIENT_EVT_PLUGIN_DATA, j.dump());
}

// ==================== 内部 ====================

/// 摘除插件在宿主侧的 UI 注册与订阅 (禁用与卸载共用; pluginxx 生命周期骨架的
/// 领域接缝 [detachDomainRegistrations]):
/// - 旧 COW 快照中的自定义 renderer 先失效, UI 线程只能回退通用渲染;
/// - UI 注册表一次性重建 (COW), adapter 按注册记录逐项通知移除;
/// - 注册记录与订阅句柄一并清空: 重新启用时由插件 start 事务重新声明。
///
/// 句柄 (statusItemHandles/panelHandles/infoSectionHandles/subHandles) 不在此释放:
/// 插件的 stop 回调可能主动反注册, 句柄必须存活到实例析构, 由 ~ClientPluginInstance 统一释放。
void ClientPluginManager::detachDomainRegistrations(ClientPluginInstance* inst) {
    if (!inst) {
        return;
    }
    for (auto& renderer : inst->toolRenderRegs) {
        if (renderer.lease) {
            renderer.lease->alive.store(false, std::memory_order_release);
        }
    }
    // 定时器全部取消 (运行时资源; enable 后由插件 start 事务重新注册)
    for (auto& timer : inst->timers) {
        if (timer) {
            timer->alive = false;
            timer->timer.cancel();
        }
    }
    inst->timers.clear();
    // 语义渲染缓存同步失效: 禁用/卸载后 UI 只读缓存, 未重算即回退通用渲染
    toolRenderCache_->invalidatePlugin(inst->name);

    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        reg  = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        auto                        drop = [inst](auto& entries) {
            std::erase_if(entries, [inst](const auto& item) {
                return item.plugin == inst->name;
            });
        };
        drop(reg->statusItems);
        drop(reg->panels);
        drop(reg->infoSections);
        drop(reg->toolDecors);
        drop(reg->toolRenderers);
        drop(reg->actionBindings);
        drop(reg->commands);
        drop(reg->keybinds);
        uiRegistry_ = std::move(reg);
    }

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

    // 订阅句柄断链: stop 回调内插件主动 unsubscribe 时 impl->inst 已置空,
    // xx_cunsubscribe 安全跳过 (句柄由 subHandles 保活到实例析构, 不解引用已释放内存)
    for (const auto& h : inst->subHandles) {
        auto impl  = std::static_pointer_cast<ClientSubscriptionImpl>(h);
        impl->inst = nullptr;
        impl->sub.reset();
    }
    inst->statusItemRegs.clear();
    inst->panelRegs.clear();
    inst->infoSectionRegs.clear();
    inst->commandRegs.clear();
    inst->keybindRegs.clear();
    inst->toolDecorRegs.clear();
    inst->toolRenderRegs.clear();
    inst->actionRegs.clear();
    inst->clientSubscriptions.clear();
    inst->subHandles.clear();
    // 快捷键句柄与 panel/status 句柄同规则: 不在此释放 (插件的 stop 回调可能仍在
    // 注销它们), 由 ~ClientPluginInstance 统一回收
}

/// 反向必选依赖收集 → 公共 collectReverseRequiredDeps
/// (见 [plugin_interfaces.h](/agent/lib/include/agentxx/plugin/plugin_interfaces.h))

void ClientPluginManager::dispatchEvent(int event, const std::string& payloadJson) {
    ioThreadId_.store(std::this_thread::get_id(), std::memory_order_release);

    // 快照订阅列表 (shared_ptr 副本: 派发中退订/卸载不会使后续回调悬垂;
    // 订阅对象被 impl 句柄/派发副本保活, alive 位标记已退订)
    struct SubRef {
        std::weak_ptr<ClientPluginInstance>                 inst;
        std::shared_ptr<ClientPluginInstance::Subscription> sub;
    };

    std::vector<SubRef> refs;
    for (const auto& [name, inst] : plugins_) {
        (void)name;
        if (!inst->enabled) {
            continue;
        }
        for (const auto& s : inst->clientSubscriptions) {
            if (s->alive && s->event == event) {
                refs.push_back(SubRef{inst, s});
            }
        }
    }
    for (const auto& ref : refs) {
        // 前一个 handler 可能在同轮内退订此项、禁用或卸载所属插件。
        // 每次开始 callback 前都重新检查当前订阅和实例状态。
        auto inst = ref.inst.lock();
        if (!ref.sub || !ref.sub->alive || !inst || !inst->enabled || !inst->lifetime
            || !inst->lifetime->acceptsOperations()) {
            continue;
        }
        PluginInstanceBase::InflightGuard guard(inst);
        if (!guard || !ref.sub->alive || !inst->enabled || !inst->lifetime->acceptsOperations()) {
            continue;
        }
        // C ABI 回调异常兜底: 单个插件 handler 违约不得打断整轮派发
        // (影响其他订阅者与 client io 事件循环)
        try {
            auto payloadSv
                = agentxx::plugin::PluginStringView::from(payloadJson.data(), payloadJson.size());
            ref.sub->handler(&payloadSv, ref.sub->ud);
        } catch (const std::exception& e) {
            XX_LOGW("[client_plugin] `{}` event handler threw: {}", inst->name, e.what());
        } catch (...) {
            XX_LOGW("[client_plugin] `{}` event handler threw unknown exception", inst->name);
        }
    }
}

// =====================================================================
// host vtable (C ABI)
// =====================================================================

namespace {

// =====================================================================
// vtable 入口公共前置
// =====================================================================

using ClientHostCall = PluginHostCall<ClientPluginInstance, ClientPluginManager>;

/// 框架内核通用表入口 (client 侧复用的部分: 协程驱动等与宿主领域无关的入口)
using ClientGenericEntries
    = pluginxx::GenericTableEntries<ClientPluginInstance, ClientPluginManager>;

/// 解析宿主控制块，并持有实例/管理器强引用与 admission lease。
///
/// - 实例已卸载/已关闭，或传入的 host 指针不是本宿主发放的视图 -> 返回空上下文
///   （实例与管理器均为 nullptr），入口按失败返回，不访问已释放对象；
/// - `allowClosing=true` 用于只读查询：关闭过程中仍允许执行（lease 保证卸载会
///   等它返回）。注册、投递新工作等入口必须用默认值，Closing/Disabled 后拒绝；
/// - 返回的上下文按值捕获进投递闭包后，卸载的 idle 等待会覆盖"已排队但尚未在
///   IO 线程执行"的阶段，见 [ioCallSyncKeep]。
static ClientHostCall enterClientHost(const PluginxxHost* host, bool allowClosing = false) {
    return enterPluginHost<ClientPluginInstance, ClientPluginManager>(host, allowClosing);
}

/// 注册/写入类入口的公共骨架: 解析 host 上下文 → 把业务逻辑投递到 client IO 线程执行。
///
/// - 参数视图只在本次调用内有效, 闭包必须按值捕获自己需要的拷贝;
/// - `fallback` 是失败返回值 (句柄入口传 nullptr, 状态码入口传 -1);
/// - `fn` 拿到实例与管理器 (投递期间由 `keep` 保活, 含 admission lease),
///   业务参数校验由入口自己完成后传入。
template<typename Ret, typename Fn>
static Ret onClientIo(const PluginxxHost* host, Ret fallback, Fn&& fn) {
    return agentxx::plugin::guardVtableCall(fallback, [&]() -> Ret {
        auto call = enterClientHost(host);
        if (!call.ok()) {
            return fallback;
        }
        auto keep = call; // 投递期间持实例/管理器强引用与 admission lease
        return ioCallSyncKeep<Ret>(
            keep,
            keep.manager(),
            [keep, fn = std::forward<Fn>(fn)]() -> Ret {
                return fn(keep.instance(), keep.manager());
            }
        );
    });
}

/// 撤销类入口 (无返回值) 的公共骨架
template<typename Fn>
static void onClientIoVoid(const PluginxxHost* host, Fn&& fn) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto call = enterClientHost(host);
        if (!call.ok()) {
            return;
        }
        auto keep = call;
        ioCallSyncVoidKeep(keep, keep.manager(), [keep, fn = std::forward<Fn>(fn)]() {
            fn(keep.instance(), keep.manager());
        });
    });
}

/// 只读查询类入口的公共骨架 (允许关闭中查询):
/// 在 IO 线程取字符串结果 → 经 host->alloc 写入 `out`; 结果为空按失败返回 -1。
template<typename Fn>
static int32_t queryClientString(const PluginxxHost* host, PluginxxString* out, Fn&& fn) {
    if (!out) {
        return -1;
    }
    return onClientIo<int32_t>(
        host,
        -1,
        [out, fn = std::forward<Fn>(fn)](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            auto text = fn(inst, mgr);
            if (text.empty()) {
                return -1;
            }
            hostMemorySetString(out, text);
            return 0;
        }
    );
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
/// 定时器 / 全局快捷键接口表 (agentxx.client.timer / agentxx.client.keybind)
extern const AgentxxClientTimerIface   g_clientIfaceTimer;
extern const AgentxxClientKeybindIface g_clientIfaceKeybind;
/// 协程驱动接口表 (agentxx.agent.coroutine_runtime; 与 agent 侧同 IID)
extern const PluginxxCoroutineRuntimeIface g_clientIfaceCoroutineRuntime;

// ---- 内存 ----

static void* PLUGINXX_CALL xx_calloc(uint64_t size) {
    return agentxx::plugin::hostMemoryAlloc(size);
}

static void PLUGINXX_CALL xx_cfree(void* ptr) {
    agentxx::plugin::hostMemoryFree(ptr);
}

// ---- 日志 / JSON ----

void PLUGINXX_CALL xx_clog(const PluginxxHost* host, int32_t level, const PluginxxStringView* msg) {
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
int32_t PLUGINXX_CALL xx_cjson_get_string(
    const PluginxxHost*       host,
    const PluginxxStringView* json,
    const PluginxxStringView* key,
    PluginxxString*           out
) {
    if (!out) {
        return -1;
    }
    auto call = enterClientHost(host, /*allowClosing=*/true);
    auto inst = call.instance();
    if (!inst || agentxx::plugin::PluginStringView::empty(json)
        || agentxx::plugin::PluginStringView::empty(key)) {
        return -1;
    }
    try {
        auto j = utilxx_base::Json::parse(std::string{json->data, static_cast<size_t>(json->size)});
        auto v = j.value(std::string_view{key->data, static_cast<size_t>(key->size)}, "");
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
int32_t PLUGINXX_CALL
    xx_cjson_escape(const PluginxxHost* host, const PluginxxStringView* s, PluginxxString* out) {
    if (!out) {
        return -1;
    }
    auto call = enterClientHost(host, /*allowClosing=*/true);
    auto inst = call.instance();
    if (!inst || agentxx::plugin::PluginStringView::empty(s)) {
        return -1;
    }
    try {
        utilxx_base::Json j       = std::string{s->data, static_cast<size_t>(s->size)};
        auto              dumpStr = j.dump();
        hostMemorySetString(out, dumpStr);
        return 0;
    } catch (...) {
        return -1;
    }
}

// ---- 协程驱动 (agentxx.agent.coroutine_runtime; 与 agent 侧同 IID/同语义) ----
//
// 驱动请求/取消/线程判定的实现整体复用框架内核的通用表入口
// ([pluginxx::GenericTableEntries]): 这三项只依赖"宿主控制块 + 实例生命周期 +
// 宿主 runtime/io 线程判定", 与宿主领域无关, 因此 client / agent 两侧共用同一份
// 逻辑与语义 (见 pluginxx/host/tables_impl.h 的 requestDriverEntry 等)。
// 语义要点: 申请恒异步、每张请求至多执行一次、排队期间持有实例 lease、
// 关闭中仍允许驱动 (取消收束需要驱动继续流动)、已关闭拒绝。

// ---- COM 风格接口表查询 ----

const void* PLUGINXX_CALL
    xx_cquery_interface(const PluginxxHost* host, const PluginxxStringView* iid) {
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
    if (n == AGENTXX_IFACE_CLIENT_TIMER) {
        return &g_clientIfaceTimer;
    }
    if (n == AGENTXX_IFACE_CLIENT_KEYBIND) {
        return &g_clientIfaceKeybind;
    }
    if (n == PLUGINXX_IFACE_COROUTINE_RUNTIME) {
        return &g_clientIfaceCoroutineRuntime;
    }
    return nullptr;
}

// ---- 状态栏项 ----

AgentxxStatusItem* PLUGINXX_CALL xx_cregister_status_item(
    const PluginxxHost*       host,
    const PluginxxStringView* id,
    const PluginxxStringView* initial_json,
    int32_t                   align,
    int32_t                   order
) {
    if (agentxx::plugin::PluginStringView::empty(id)) {
        return nullptr;
    }
    auto idVal   = *id;
    auto jsonVal = initial_json ? *initial_json : agentxx::plugin::PluginStringView::from("{}", 2);
    return onClientIo<AgentxxStatusItem*>(
        host,
        nullptr,
        [idVal, jsonVal, align, order](ClientPluginInstance* inst, ClientPluginManager* mgr)
            -> AgentxxStatusItem* {
            return static_cast<AgentxxStatusItem*>(
                mgr->registerStatusItem(inst, idVal, jsonVal, align, order)
            );
        }
    );
}

int32_t PLUGINXX_CALL xx_cupdate_status_item(
    const PluginxxHost*       host,
    AgentxxStatusItem*        item,
    const PluginxxStringView* json
) {
    if (!item || !json) {
        return -1;
    }
    auto jsonVal = *json;
    return onClientIo<int32_t>(
        host,
        -1,
        [item, jsonVal](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->updateStatusItem(inst, item, jsonVal);
        }
    );
}

void PLUGINXX_CALL xx_cunregister_status_item(const PluginxxHost* host, AgentxxStatusItem* item) {
    if (!item) {
        return;
    }
    onClientIoVoid(host, [item](ClientPluginInstance* inst, ClientPluginManager* mgr) {
        mgr->unregisterStatusItem(inst, item);
    });
}

// ---- 侧边栏面板 ----

AgentxxPanel* PLUGINXX_CALL xx_cregister_panel(
    const PluginxxHost*       host,
    const PluginxxStringView* id,
    const PluginxxStringView* props_json
) {
    if (agentxx::plugin::PluginStringView::empty(id)) {
        return nullptr;
    }
    auto idVal    = *id;
    auto propsVal = props_json ? *props_json : agentxx::plugin::PluginStringView::from("{}", 2);
    return onClientIo<AgentxxPanel*>(
        host,
        nullptr,
        [idVal, propsVal](ClientPluginInstance* inst, ClientPluginManager* mgr) -> AgentxxPanel* {
            return static_cast<AgentxxPanel*>(mgr->registerPanel(inst, idVal, propsVal));
        }
    );
}

int32_t PLUGINXX_CALL xx_cupdate_panel(
    const PluginxxHost*       host,
    AgentxxPanel*             panel,
    const PluginxxStringView* items_json
) {
    if (!panel || !items_json) {
        return -1;
    }
    auto itemsVal = *items_json;
    return onClientIo<int32_t>(
        host,
        -1,
        [panel, itemsVal](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->updatePanel(inst, panel, itemsVal);
        }
    );
}

void PLUGINXX_CALL xx_cunregister_panel(const PluginxxHost* host, AgentxxPanel* panel) {
    if (!panel) {
        return;
    }
    onClientIoVoid(host, [panel](ClientPluginInstance* inst, ClientPluginManager* mgr) {
        mgr->unregisterPanel(inst, panel);
    });
}

// ---- Info 栏段落 ----

AgentxxInfoSection* PLUGINXX_CALL xx_cregister_info_section(
    const PluginxxHost*       host,
    const PluginxxStringView* id,
    const PluginxxStringView* props_json
) {
    if (agentxx::plugin::PluginStringView::empty(id)) {
        return nullptr;
    }
    auto idVal    = *id;
    auto propsVal = props_json ? *props_json : agentxx::plugin::PluginStringView::from("{}", 2);
    return onClientIo<AgentxxInfoSection*>(
        host,
        nullptr,
        [idVal,
         propsVal](ClientPluginInstance* inst, ClientPluginManager* mgr) -> AgentxxInfoSection* {
            return static_cast<AgentxxInfoSection*>(mgr->registerInfoSection(inst, idVal, propsVal)
            );
        }
    );
}

int32_t PLUGINXX_CALL xx_cupdate_info_section(
    const PluginxxHost*       host,
    AgentxxInfoSection*       section,
    const PluginxxStringView* items_json
) {
    if (!section || !items_json) {
        return -1;
    }
    auto itemsVal = *items_json;
    return onClientIo<int32_t>(
        host,
        -1,
        [section, itemsVal](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->updateInfoSection(inst, section, itemsVal);
        }
    );
}

void PLUGINXX_CALL
    xx_cunregister_info_section(const PluginxxHost* host, AgentxxInfoSection* section) {
    if (!section) {
        return;
    }
    onClientIoVoid(host, [section](ClientPluginInstance* inst, ClientPluginManager* mgr) {
        mgr->unregisterInfoSection(inst, section);
    });
}

int32_t PLUGINXX_CALL xx_cupdate_tool_decor(
    const PluginxxHost*       host,
    const PluginxxStringView* tool_call_id,
    const PluginxxStringView* decor_json
) {
    auto tcidVal  = tool_call_id ? *tool_call_id : agentxx::plugin::PluginStringView::from("", 0);
    auto decorVal = decor_json ? *decor_json : agentxx::plugin::PluginStringView::from("", 0);
    return onClientIo<int32_t>(
        host,
        -1,
        [tcidVal, decorVal](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->updateToolDecor(inst, tcidVal, decorVal);
        }
    );
}

int32_t PLUGINXX_CALL
    xx_cregister_tool_renderer(const PluginxxHost* host, const AgentxxToolRenderSpec* spec) {
    if (!spec) {
        return -1;
    }
    return onClientIo<int32_t>(
        host,
        -1,
        [spec](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->registerToolRenderer(inst, spec);
        }
    );
}

int32_t PLUGINXX_CALL
    xx_cunregister_tool_renderer(const PluginxxHost* host, const PluginxxStringView* tool_name) {
    if (!tool_name) {
        return -1;
    }
    auto tnameVal = *tool_name;
    return onClientIo<int32_t>(
        host,
        -1,
        [tnameVal](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->unregisterToolRenderer(inst, tnameVal);
        }
    );
}

// ---- 命令 ----

int32_t PLUGINXX_CALL xx_cregister_command(
    const PluginxxHost*       host,
    const PluginxxStringView* name,
    const PluginxxStringView* description,
    int32_t(PLUGINXX_CALL*
                execute)(void*, const PluginxxStringView*, PluginxxString*, PluginxxString*),
    void* ud
) {
    if (!execute || agentxx::plugin::PluginStringView::empty(name)) {
        return -1;
    }
    auto nameVal = *name;
    auto descVal = description ? *description : agentxx::plugin::PluginStringView::from("", 0);
    return onClientIo<int32_t>(
        host,
        -1,
        [nameVal, descVal, execute, ud](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->registerCommand(inst, nameVal, descVal, execute, ud);
        }
    );
}

int32_t PLUGINXX_CALL
    xx_cunregister_command(const PluginxxHost* host, const PluginxxStringView* name) {
    if (agentxx::plugin::PluginStringView::empty(name)) {
        return -1;
    }
    auto nameVal = *name;
    return onClientIo<int32_t>(
        host,
        -1,
        [nameVal](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->unregisterCommand(inst, nameVal);
        }
    );
}

// ---- toast ----

void PLUGINXX_CALL
    xx_cshow_toast(const PluginxxHost* host, const PluginxxStringView* text, int32_t level) {
    if (!text || !text->data) {
        return;
    }
    std::string textStr{text->data, static_cast<size_t>(text->size)};
    onClientIoVoid(host, [textStr, level](ClientPluginInstance*, ClientPluginManager* mgr) {
        if (auto adapter = mgr->uiAdapter()) {
            adapter->onToast(textStr, level);
        }
    });
}

// ---- 事件订阅 ----

PluginxxSubscription* PLUGINXX_CALL xx_csubscribe(
    const PluginxxHost* host,
    int32_t             event,
    void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*),
    void* ud
) {
    if (!handler || event < 0 || event >= AGENTXX_CLIENT_EVT_COUNT) {
        return nullptr;
    }
    return onClientIo<PluginxxSubscription*>(
        host,
        nullptr,
        [event,
         handler,
         ud](ClientPluginInstance* inst, ClientPluginManager* mgr) -> PluginxxSubscription* {
            return static_cast<PluginxxSubscription*>(mgr->subscribe(inst, event, handler, ud));
        }
    );
}

void PLUGINXX_CALL xx_cunsubscribe(PluginxxSubscription* sub) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        if (!sub) {
            return;
        }
        auto impl = reinterpret_cast<ClientSubscriptionImpl*>(sub);
        // 持有管理器强引用：ioCallSyncVoid 投递期间管理器必须存活。
        auto mgr = impl->inst ? impl->inst->manager.lock() : nullptr;
        if (mgr) {
            ioCallSyncVoid(mgr.get(), [mgr, impl]() {
                mgr->unsubscribe(reinterpret_cast<PluginxxSubscription*>(impl));
            });
        }
        impl->inst = nullptr;
        impl->sub.reset();
    });
}

// ---- 会话上下文 ----

int32_t PLUGINXX_CALL xx_cget_client_state(const PluginxxHost* host, PluginxxString* out) {
    return queryClientString(host, out, [](ClientPluginInstance*, ClientPluginManager* mgr) {
        return mgr->clientStateJson();
    });
}

// ---- 会话操作 ----

int32_t PLUGINXX_CALL xx_csend_user_input(
    const PluginxxHost*       host,
    const PluginxxStringView* thread_id,
    const PluginxxStringView* text
) {
    if (agentxx::plugin::PluginStringView::empty(text)) {
        return -1;
    }
    auto tidVal  = thread_id ? *thread_id : agentxx::plugin::PluginStringView::from("", 0);
    auto textVal = *text;
    return onClientIo<int32_t>(
        host,
        -1,
        [tidVal, textVal](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            mgr->sendUserInputToPeer(inst, tidVal, textVal);
            return 0;
        }
    );
}

void PLUGINXX_CALL
    xx_crequest_cancel(const PluginxxHost* host, const PluginxxStringView* thread_id) {
    auto tidVal = thread_id ? *thread_id : agentxx::plugin::PluginStringView::from("", 0);
    onClientIoVoid(host, [tidVal](ClientPluginInstance* inst, ClientPluginManager* mgr) {
        mgr->requestCancelToPeer(inst, tidVal);
    });
}

// ---- 跨端数据 ----

int32_t PLUGINXX_CALL xx_csend_plugin_data(
    const PluginxxHost*       host,
    const PluginxxStringView* event,
    const PluginxxStringView* json
) {
    if (agentxx::plugin::PluginStringView::empty(event)) {
        return -1;
    }
    auto evtVal  = *event;
    auto jsonVal = json ? *json : agentxx::plugin::PluginStringView::from("{}", 2);
    return onClientIo<int32_t>(
        host,
        -1,
        [evtVal, jsonVal](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->sendPluginDataToPeer(inst, evtVal, jsonVal);
        }
    );
}

// ---- 自描述 ----

int32_t PLUGINXX_CALL xx_cget_own_info(const PluginxxHost* host, PluginxxString* out) {
    return queryClientString(host, out, [](ClientPluginInstance* inst, ClientPluginManager* mgr) {
        return mgr->getOwnInfoJson(inst);
    });
}

int32_t PLUGINXX_CALL xx_cget_plugin_args(const PluginxxHost* host, PluginxxString* out) {
    return queryClientString(host, out, [](ClientPluginInstance* inst, ClientPluginManager* mgr) {
        return mgr->getPluginArgsJson(inst);
    });
}

int32_t PLUGINXX_CALL xx_cget_plugin_config_path(const PluginxxHost* host, PluginxxString* out) {
    return queryClientString(host, out, [](ClientPluginInstance* inst, ClientPluginManager* mgr) {
        return mgr->getPluginConfigPath(inst);
    });
}

static int32_t PLUGINXX_CALL xx_cget_language(const PluginxxHost* host, PluginxxString* out) {
    return queryClientString(host, out, [](ClientPluginInstance*, ClientPluginManager* mgr) {
        auto lang = mgr->getLanguage();
        return lang.empty() ? std::string{"en"} : lang;
    });
}

static int32_t PLUGINXX_CALL
    xx_cset_language(const PluginxxHost* host, const PluginxxStringView* language) {
    std::string lang = (language && language->data)
                           ? std::string(language->data, static_cast<size_t>(language->size))
                           : std::string{};
    return onClientIo<int32_t>(host, -1, [lang](ClientPluginInstance*, ClientPluginManager* mgr) {
        mgr->setLanguage(lang);
        return 0;
    });
}

// ---- 通用交互: 动作绑定 / overlay ----

int32_t PLUGINXX_CALL xx_cbind_action_handler(
    const PluginxxHost*       host,
    const PluginxxStringView* target_id,
    AgentxxUiActionFn         on_action,
    void*                     user_data
) {
    if (!on_action) {
        return -1;
    }
    auto targetVal = target_id ? *target_id : agentxx::plugin::PluginStringView::from("", 0);
    return onClientIo<int32_t>(
        host,
        -1,
        [targetVal, on_action, user_data](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->bindActionHandler(inst, targetVal, on_action, user_data);
        }
    );
}

int32_t PLUGINXX_CALL
    xx_cunbind_action_handler(const PluginxxHost* host, const PluginxxStringView* target_id) {
    auto targetVal = target_id ? *target_id : agentxx::plugin::PluginStringView::from("", 0);
    return onClientIo<int32_t>(
        host,
        -1,
        [targetVal](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->unbindActionHandler(inst, targetVal);
        }
    );
}

int32_t PLUGINXX_CALL xx_copen_overlay(const PluginxxHost* host, const AgentxxOverlaySpec* spec) {
    if (!spec) {
        return -1;
    }
    return onClientIo<int32_t>(
        host,
        -1,
        [spec](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->openOverlay(inst, spec);
        }
    );
}

void PLUGINXX_CALL xx_cclose_overlay(const PluginxxHost* host) {
    onClientIoVoid(host, [](ClientPluginInstance* inst, ClientPluginManager* mgr) {
        mgr->closeOverlay(inst);
    });
}

// ---- 定时器 (agentxx.client.timer) ----

AgentxxTimer* PLUGINXX_CALL
    xx_cset_timer(const PluginxxHost* host, const AgentxxTimerSpec* spec) {
    if (!spec) {
        return nullptr;
    }
    return onClientIo<AgentxxTimer*>(
        host,
        nullptr,
        [spec](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->setTimer(inst, spec);
        }
    );
}

void PLUGINXX_CALL xx_ccancel_timer(const PluginxxHost* host, AgentxxTimer* timer) {
    onClientIoVoid(host, [timer](ClientPluginInstance* inst, ClientPluginManager* mgr) {
        mgr->cancelTimer(inst, timer);
    });
}

int32_t PLUGINXX_CALL
    xx_cis_visible(const PluginxxHost* host, const PluginxxStringView* owner_id) {
    if (!owner_id || !owner_id->data) {
        return 0;
    }
    std::string_view owner{owner_id->data, static_cast<size_t>(owner_id->size)};
    // 只读查询 (允许关闭中查询), 与其它 get_* 入口同一形态
    auto call = enterClientHost(host, /*allowClosing=*/true);
    if (!call.ok()) {
        return 0;
    }
    auto mgr = call.manager();
    return (mgr && mgr->isRegionVisible(owner)) ? 1 : 0;
}

// ---- 全局快捷键 (agentxx.client.keybind) ----

AgentxxKeybind* PLUGINXX_CALL
    xx_cregister_keybind(const PluginxxHost* host, const AgentxxKeybindSpec* spec) {
    if (!spec) {
        return nullptr;
    }
    return onClientIo<AgentxxKeybind*>(
        host,
        nullptr,
        [spec](ClientPluginInstance* inst, ClientPluginManager* mgr) {
            return mgr->registerKeybind(inst, spec);
        }
    );
}

void PLUGINXX_CALL xx_cunregister_keybind(const PluginxxHost* host, AgentxxKeybind* bind) {
    onClientIoVoid(host, [bind](ClientPluginInstance* inst, ClientPluginManager* mgr) {
        mgr->unregisterKeybind(inst, bind);
    });
}

int32_t PLUGINXX_CALL xx_clist_keybinds(const PluginxxHost* host, PluginxxString* out) {
    return queryClientString(host, out, [](ClientPluginInstance*, ClientPluginManager* mgr) {
        auto binds = mgr->keybinds();
        auto arr   = utilxx_base::Json::array();
        for (const auto& b : binds) {
            auto item        = utilxx_base::Json::object();
            item["keys"]     = b.keys;
            item["plugin"]   = b.plugin;
            item["description"] = b.description;
            arr.push_back(std::move(item));
        }
        return arr.dump();
    });
}

/// "agentxx.client.ui" 展示接口表访问器: 表内成员恒非空 (函数实现存在), 子能力是否
/// 可用由各 register 入口的 hostSupportedInterfaces 限制决定 (拒绝时返回
/// NULL/非 0) —— 与接口表 "NULL = 不支持" 契约的分工: 表级 NULL 用于宿主
/// 整体缺失某子能力入口的场景 (当前宿主全量装配, 保留判空语义供第三方精简
/// 宿主使用)。以函数内静态表实现 (前向引用无需 extern 声明)
static const AgentxxClientUiIface* clientUiIface() {
    static const AgentxxClientUiIface table = {
        /* version */ AGENTXX_IFACE_CLIENT_UI_VERSION,
        /* struct_size */ sizeof(AgentxxClientUiIface),
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
        /* bind_action_handler */ xx_cbind_action_handler,
        /* unbind_action_handler */ xx_cunbind_action_handler,
        /* open_overlay */ xx_copen_overlay,
        /* close_overlay */ xx_cclose_overlay,
    };
    return &table;
}

// ---- 其余标准接口表 (进程级静态只读; 经 query_interface 分发) ----

const AgentxxClientEventsIface g_clientIfaceEvents = {
    /* version */ AGENTXX_IFACE_CLIENT_EVENTS_VERSION,
    /* struct_size */ sizeof(AgentxxClientEventsIface),
    /* subscribe */ xx_csubscribe,
    /* unsubscribe */ xx_cunsubscribe,
};

const AgentxxClientSessionIface g_clientIfaceSession = {
    /* version */ AGENTXX_IFACE_CLIENT_SESSION_VERSION,
    /* struct_size */ sizeof(AgentxxClientSessionIface),
    /* get_client_state */ xx_cget_client_state,
    /* send_user_input */ xx_csend_user_input,
    /* request_cancel */ xx_crequest_cancel,
};

const AgentxxClientWireIface g_clientIfaceWire = {
    /* version */ AGENTXX_IFACE_CLIENT_WIRE_VERSION,
    /* struct_size */ sizeof(AgentxxClientWireIface),
    /* send_plugin_data */ xx_csend_plugin_data,
};

const AgentxxClientSelfIface g_clientIfaceSelf = {
    /* version */ AGENTXX_IFACE_CLIENT_SELF_VERSION,
    /* struct_size */ sizeof(AgentxxClientSelfIface),
    /* get_own_info */ xx_cget_own_info,
    /* get_plugin_args */ xx_cget_plugin_args,
    /* get_plugin_config_path */ xx_cget_plugin_config_path,
    /* get_language */ xx_cget_language,
    /* set_language */ xx_cset_language,
};

const AgentxxClientJsonIface g_clientIfaceJson = {
    /* version */ AGENTXX_IFACE_CLIENT_JSON_VERSION,
    /* struct_size */ sizeof(AgentxxClientJsonIface),
    /* json_get_string */ xx_cjson_get_string,
    /* json_escape */ xx_cjson_escape,
};

const AgentxxClientLogIface g_clientIfaceLog = {
    /* version */ AGENTXX_IFACE_CLIENT_LOG_VERSION,
    /* struct_size */ sizeof(AgentxxClientLogIface),
    /* log */ xx_clog,
};

const AgentxxClientTimerIface g_clientIfaceTimer = {
    /* version */ AGENTXX_IFACE_CLIENT_TIMER_VERSION,
    /* struct_size */ sizeof(AgentxxClientTimerIface),
    /* set_timer */ xx_cset_timer,
    /* cancel_timer */ xx_ccancel_timer,
    /* is_visible */ xx_cis_visible,
};

const AgentxxClientKeybindIface g_clientIfaceKeybind = {
    /* version */ AGENTXX_IFACE_CLIENT_KEYBIND_VERSION,
    /* struct_size */ sizeof(AgentxxClientKeybindIface),
    /* register_keybind */ xx_cregister_keybind,
    /* unregister_keybind */ xx_cunregister_keybind,
    /* list_keybinds */ xx_clist_keybinds,
};

/// 协程驱动接口表 (与 agent 侧同 IID; client 插件用同一套 kit 桥接)
/// - 三个入口整体复用框架内核的通用表实现 (见文件上方"协程驱动"说明)
const PluginxxCoroutineRuntimeIface g_clientIfaceCoroutineRuntime = {
    /* version */ PLUGINXX_IFACE_COROUTINE_RUNTIME_VERSION,
    /* struct_size */ sizeof(PluginxxCoroutineRuntimeIface),
    /* request_driver */ &ClientGenericEntries::requestDriverEntry,
    /* cancel_driver */ &ClientGenericEntries::cancelDriverEntry,
    /* is_io_thread */ &ClientGenericEntries::isIoThreadEntry,
};

/// 核心 vtable (契约冻结: 仅内存操作 + query_interface)
const PluginxxHostVtable g_clientHostVtable = {
    /* alloc */ xx_calloc,
    /* free */ xx_cfree,
    /* query_interface */ xx_cquery_interface,
};

} // namespace

const PluginxxHostVtable* ClientPluginManager::hostVtable() {
    return &g_clientHostVtable;
}

// =====================================================================
// ClientPluginManager 内部实现 (vtable 强类型入口)
// =====================================================================

void* ClientPluginManager::registerStatusItem(
    ClientPluginInstance* inst,
    PluginxxStringView    id,
    PluginxxStringView    json,
    int                   align,
    int                   order
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(id)) {
        return nullptr;
    }
    // 执行期复查：请求可能排在 IO 队列里，等执行时实例已进入 Closing/Disabled。
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "[client_plugin] `{}` registerStatusItem rejected: closing or disabled",
            inst->name
        );
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
    // 解析 initial_json → text (与富片段)
    std::string       text;
    utilxx_base::Json props;
    try {
        props = utilxx_base::Json::parse(
            agentxx::plugin::PluginStringView::empty(json) ? "{}" : svToSv(json)
        );
        text = props.value("text", "");
    } catch (...) {
        text.clear();
    }
    if (text.empty()) {
        text = idStr;
    }
    // 富展示片段 (segments/sparkline/meter): 注册时即可给出, 更新时同 updateStatusItem
    const bool hasRich = props.is_object()
                         && (props.contains("segments") || props.contains("sparkline")
                             || props.contains("meter"));

    auto handle    = std::make_shared<AgentxxStatusItem>();
    handle->inst   = inst;
    handle->id     = idStr;
    handle->plugin = inst->name;

    ClientStatusItem reg;
    reg.plugin = inst->name;
    reg.id     = handle->id;
    reg.text   = text;
    reg.rich   = hasRich ? props : utilxx_base::Json::object();
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
    PluginxxStringView    json
) {
    auto h = static_cast<AgentxxStatusItem*>(item);
    if (!inst || !h) {
        return -1;
    }
    if (!acceptUiJsonSize(svToSv(json), "update_status_item", inst->name)) {
        return -1;
    }
    utilxx_base::Json props;
    std::string       text;
    try {
        props = utilxx_base::Json::parse(
            agentxx::plugin::PluginStringView::empty(json) ? "{}" : svToSv(json)
        );
        text = props.value("text", "");
    } catch (...) {
        text.clear();
    }
    // 富展示片段 (segments/sparkline/meter): 与 text 同为可选, 两者都为空才算非法更新
    const bool hasRich = props.is_object()
                         && (props.contains("segments") || props.contains("sparkline")
                             || props.contains("meter"));
    // 富片段存在时 text 可省略 (状态栏以富内容为主, 文本仅作降级)
    if (!hasRich && text.empty()) {
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        for (auto& s : cur->statusItems) {
            if (s.id == h->id) {
                s.text = text;
                s.rich = hasRich ? props : utilxx_base::Json::object();
                ++s.version;
                break;
            }
        }
        uiRegistry_ = std::move(cur);
    }
    for (auto& s : inst->statusItemRegs) {
        if (s.id == h->id) {
            s.text = text;
            s.rich = hasRich ? props : utilxx_base::Json::object();
            ++s.version;
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
    PluginxxStringView    id,
    PluginxxStringView    props_json
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(id)) {
        return nullptr;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("[client_plugin] `{}` registerPanel rejected: closing or disabled", inst->name);
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
    utilxx_base::Json props;
    std::string       title;
    try {
        props = utilxx_base::Json::parse(
            agentxx::plugin::PluginStringView::empty(props_json) ? "{}" : svToSv(props_json)
        );
        title = props.value("title", "");
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
    ClientPluginInstance* inst,
    void*                 panel,
    PluginxxStringView    items_json
) {
    auto h = static_cast<AgentxxPanel*>(panel);
    if (!inst || !h) {
        return -1;
    }
    if (!acceptUiJsonSize(svToSv(items_json), "update_panel", inst->name)) {
        return -1;
    }
    utilxx_base::Json items = utilxx_base::Json::array();
    try {
        auto j = utilxx_base::Json::parse(
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
                ++p.version;
                break;
            }
        }
        uiRegistry_ = std::move(cur);
    }
    for (auto& p : inst->panelRegs) {
        if (p.id == h->id) {
            p.items = items;
            ++p.version;
            break;
        }
    }
    utilxx_base::Json payload = utilxx_base::Json::object();
    payload["items"]          = items;
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
    PluginxxStringView    id,
    PluginxxStringView    props_json
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(id)) {
        return nullptr;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "[client_plugin] `{}` registerInfoSection rejected: closing or disabled",
            inst->name
        );
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
    utilxx_base::Json props;
    std::string       title;
    try {
        props = utilxx_base::Json::parse(
            agentxx::plugin::PluginStringView::empty(props_json) ? "{}" : svToSv(props_json)
        );
        title = props.value("title", "");
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
    ClientPluginInstance* inst,
    void*                 section,
    PluginxxStringView    items_json
) {
    auto h = static_cast<AgentxxInfoSection*>(section);
    if (!inst || !h) {
        return -1;
    }
    if (!acceptUiJsonSize(svToSv(items_json), "update_info_section", inst->name)) {
        return -1;
    }
    utilxx_base::Json items = utilxx_base::Json::array();
    try {
        auto j = utilxx_base::Json::parse(
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
                ++s.version;
                break;
            }
        }
        uiRegistry_ = std::move(cur);
    }
    for (auto& s : inst->infoSectionRegs) {
        if (s.id == h->id) {
            s.items = items;
            ++s.version;
            break;
        }
    }
    utilxx_base::Json payload = utilxx_base::Json::object();
    payload["items"]          = items;
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
    ClientPluginInstance* inst,
    PluginxxStringView    tool_call_id,
    PluginxxStringView    decor_json
) {
    if (!inst) {
        return -1;
    }
    const std::string tid  = svToStr(tool_call_id);
    const std::string json = svToStr(decor_json);
    if (!acceptUiJsonSize(json, "update_tool_decor", inst->name)) {
        return -1;
    }

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
        auto j = utilxx_base::Json::parse(json);
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
    ClientPluginInstance* inst,
    PluginxxStringView    name,
    PluginxxStringView    description,
    int32_t(PLUGINXX_CALL*
                exec)(void*, const PluginxxStringView*, PluginxxString*, PluginxxString*),
    void* ud
) {
    if (!inst || !exec || agentxx::plugin::PluginStringView::empty(name)) {
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("[client_plugin] `{}` registerCommand rejected: closing or disabled", inst->name);
        return -1;
    }
    std::string nameStr = svToStr(name);
    std::string descStr = svToStr(description);
    // 命令输入管线接口 (agentxx.client.command): 无命令输入面的宿主拒绝注册 ——
    // 与其他 register_* 的接口限制行为一致
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

int ClientPluginManager::unregisterCommand(ClientPluginInstance* inst, PluginxxStringView name) {
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

// ==================== 定时器 (agentxx.client.timer) ====================

namespace {

/// 定时器间隔下限 (更小值按此收敛, 避免插件把 io 线程打满)
constexpr int32_t kTimerMinIntervalMs = 50;
/// 单实例定时器数量上限
constexpr size_t kTimerMaxPerInstance = 8;
/// 单实例快捷键数量上限
constexpr size_t kKeybindMaxPerInstance = 16;

/// 等待一次到期 → 触发 (fireTimerTick) → 周期定时器续期
///
/// 说明: 定时器生命周期由 [ClientTimerImpl::alive] 控制; 取消后即使有在途等待
/// 也会在回调里立即返回 (asio 的 cancel 只是让等待以 operation_aborted 提前结束)。
void armTimer(
    const std::weak_ptr<ClientPluginManager>& mgr,
    const std::shared_ptr<ClientTimerImpl>&   timer
);

/// 触发一次定时器 (仅 io 线程)
void fireTimerTick(
    const std::shared_ptr<ClientPluginManager>& mgr,
    const std::shared_ptr<ClientTimerImpl>&     timer
) {
    if (!timer->alive || !timer->cb) {
        return;
    }
    auto inst = timer->inst.lock();
    if (!inst) {
        timer->alive = false;
        return;
    }
    // 门控: 关联区域不可见时跳过本次回调并顺延 (计时继续; 可见后自然恢复)
    // - 一次性定时器在暂停期间不消耗 (区域可见后的下一次到时触发)
    // - 周期定时器在暂停期间不减计数 (触发次数上限只统计真正回调的次数)
    const bool hidden = timer->pauseHidden && !timer->ownerId.empty()
                        && !mgr->isRegionVisible(timer->ownerId);
    // 同帧合并: 续期后已过去两个及以上周期说明 io 线程被占住 (回调积压), 本次
    // 到期与后续到期落在同一批事件里 —— 只回调一次, 丢弃已错过的周期, 避免插件
    // 被"追赶式"连续回调打满 (周期定时器才有意义; 一次性定时器照常触发)
    // 注意: 只用"续期时刻到现在"判定, 回调自身耗时导致的下一次到期不算迟到
    const bool stale = timer->repeatMode && timer->armedAt.time_since_epoch().count() != 0
                       && std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - timer->armedAt
                          )
                              .count()
                              >= 2 * static_cast<int64_t>(timer->intervalMs);
    const bool usable = inst->enabled && inst->lifetime && inst->lifetime->acceptsOperations();
    if (stale) {
        ++timer->dropped;
    }
    if (usable && !hidden && !stale) {
        // 回调期间实例由 guard 保活 (禁止卸载 dlclose)
        PluginInstanceBase::InflightGuard guard(inst);
        if (guard) {
            try {
                timer->cb(timer->ud);
            } catch (const std::exception& e) {
                XX_LOGW("[client_plugin] timer callback threw: {}", e.what());
            } catch (...) {
                XX_LOGW("[client_plugin] timer callback threw unknown exception");
            }
        }
        // 次数递减: 一次性定时器触发后结束, 周期定时器按 repeat 上限收敛
        if (timer->repeatMode) {
            if (timer->repeat > 0) {
                --timer->repeat;
            }
            if (timer->repeat <= 0) {
                timer->alive = false;
            }
        } else {
            timer->alive = false;
        }
    }
    if (!timer->alive) {
        // 已结束的定时器保留在实例表内 (不释放): 句柄地址稳定, 插件重复 cancel
        // 或持有旧句柄都不会命中"被复用地址的新定时器"; 实例禁用/卸载时统一清理
        return;
    }
    armTimer(mgr, timer);
}

void armTimer(
    const std::weak_ptr<ClientPluginManager>& mgr,
    const std::shared_ptr<ClientTimerImpl>&   timer
) {
    if (!timer || !timer->alive) {
        return;
    }
    timer->armedAt = std::chrono::steady_clock::now();
    timer->timer.expires_after(std::chrono::milliseconds{std::max<int32_t>(1, timer->intervalMs)});
    timer->timer.async_wait([mgr, timer](const utilxx_base::AsioErrorCode& ec) {
        if (ec || !timer->alive) {
            return; // 已取消 (operation_aborted) 或实例关闭: 静默结束
        }
        auto manager = mgr.lock();
        if (!manager) {
            return;
        }
        fireTimerTick(manager, timer);
    });
}

} // namespace

AgentxxTimer* ClientPluginManager::setTimer(ClientPluginInstance* inst, const AgentxxTimerSpec* spec) {
    if (!inst || !spec || !spec->on_timer || spec->version != 1) {
        return nullptr;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("[client_plugin] `{}` set_timer rejected: closing or disabled", inst->name);
        return nullptr;
    }
    if (!animationEnabled_.load(std::memory_order_relaxed)) {
        // 动画等级 Disabled: 拒绝注册 (插件据此降级为静态展示)
        XX_LOGD("[client_plugin] `{}` set_timer rejected: animation disabled", inst->name);
        return nullptr;
    }
    // 能力门控: 与其它 register_* 入口一致 (宿主未声明定时器能力时拒绝)
    if (!hostSupportedInterfaces().contains(std::string{plugin_interfaces::ClientTimer})) {
        XX_LOGW("[client_plugin] `{}` set_timer rejected: interface agentxx.client.timer unsupported", inst->name);
        return nullptr;
    }
    // 上限只统计"仍存活"的定时器 (已结束/已取消的保留在表内但不计数)
    const size_t liveTimers = std::count_if(
        inst->timers.begin(),
        inst->timers.end(),
        [](const std::shared_ptr<ClientTimerImpl>& h) {
            return h && h->alive;
        }
    );
    if (liveTimers >= kTimerMaxPerInstance) {
        XX_LOGW(
            "[client_plugin] `{}` set_timer rejected: too many timers ({})",
            inst->name,
            liveTimers
        );
        return nullptr;
    }
    const auto& executor = ioExecutor();
    if (!executor) {
        return nullptr;
    }
    auto impl          = std::make_shared<ClientTimerImpl>(executor);
    impl->inst         = inst->self;
    impl->ownerId      = svToStr(spec->owner_id);
    impl->intervalMs   = std::max<int32_t>(kTimerMinIntervalMs, spec->interval_ms);
    impl->repeatMode   = spec->repeat > 0;
    impl->repeat       = spec->repeat;
    impl->pauseHidden  = spec->pause_when_hidden != 0;
    impl->cb           = spec->on_timer;
    impl->ud           = spec->user_data;
    inst->timers.push_back(impl);
    armTimer(weak_from_this(), impl);
    return reinterpret_cast<AgentxxTimer*>(impl.get());
}

void ClientPluginManager::cancelTimer(ClientPluginInstance* inst, AgentxxTimer* timer) {
    if (!inst || !timer) {
        return;
    }
    auto* impl = reinterpret_cast<ClientTimerImpl*>(timer);
    // 只接受仍在本实例表内的句柄 (重复取消/跨实例句柄忽略);
    // 已结束的定时器仍留在表内, 因此旧句柄可安全命中并直接标记
    auto  it   = std::find_if(inst->timers.begin(), inst->timers.end(), [&](const auto& h) {
        return h.get() == impl;
    });
    if (it == inst->timers.end()) {
        return;
    }
    impl->alive = false;
    impl->timer.cancel();
    // 不在此释放: 句柄地址需保持稳定 (见 fireTimerTick 说明)
}

bool acceptUiJsonSize(
    std::string_view json,
    std::string_view what,
    std::string_view plugin
) {
    if (json.size() <= kUiJsonMaxBytes) {
        return true;
    }
    XX_LOGW(
        "[client_plugin] `{}` {} rejected: UI json too large ({} bytes > {} bytes)",
        plugin,
        what,
        json.size(),
        kUiJsonMaxBytes
    );
    return false;
}

bool ClientPluginManager::isRegionVisible(std::string_view ownerId) const {
    if (ownerId.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(regionMutex_);
    auto                        it = regionVisibility_.find(ownerId);
    return (it == regionVisibility_.end()) ? false : it->second;
}

void ClientPluginManager::reportRegionVisible(const std::string& id, bool visible) {
    if (id.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(regionMutex_);
        auto [it, inserted] = regionVisibility_.try_emplace(id, visible);
        if (!inserted && it->second == visible) {
            return; // 值未变化: 不产生任何开销
        }
        it->second = visible;
    }
}

std::map<std::string, bool, std::less<>> ClientPluginManager::regionVisibility() const {
    std::lock_guard<std::mutex> lock(regionMutex_);
    return regionVisibility_;
}

// ==================== 全局快捷键 (agentxx.client.keybind) ====================

AgentxxKeybind*
    ClientPluginManager::registerKeybind(ClientPluginInstance* inst, const AgentxxKeybindSpec* spec) {
    if (!inst || !spec || !spec->on_keybind || spec->version != 1) {
        return nullptr;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("[client_plugin] `{}` register_keybind rejected: closing or disabled", inst->name);
        return nullptr;
    }
    const std::string rawKeys = svToStr(spec->keys);
    const std::string keys    = normalizeKeybindSpec(rawKeys);
    if (keys.empty()) {
        XX_LOGW("[client_plugin] `{}` register_keybind rejected: invalid keys `{}`", inst->name, rawKeys);
        return nullptr;
    }
    // 能力门控: 与其它 register_* 入口一致 (宿主未声明快捷键能力时拒绝)
    if (!hostSupportedInterfaces().contains(std::string{plugin_interfaces::ClientKeybind})) {
        XX_LOGW(
            "[client_plugin] `{}` register_keybind rejected: interface agentxx.client.keybind unsupported",
            inst->name
        );
        return nullptr;
    }
    if (inst->keybindRegs.size() >= kKeybindMaxPerInstance) {
        XX_LOGW("[client_plugin] `{}` register_keybind rejected: too many keybinds", inst->name);
        return nullptr;
    }
    ClientKeybind reg;
    reg.plugin      = inst->name;
    reg.keys        = keys;
    reg.description = svToStr(spec->description);
    reg.handler     = spec->on_keybind;
    reg.ud          = spec->user_data;
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        for (const auto& k : uiRegistry_->keybinds) {
            if (k.keys == keys) {
                XX_LOGW(
                    "[client_plugin] keybind `{}` rejected: occupied by `{}`",
                    keys,
                    k.plugin
                );
                return nullptr;
            }
        }
        auto cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        cur->keybinds.push_back(reg);
        uiRegistry_ = std::move(cur);
    }
    inst->keybindRegs.push_back(std::move(reg));

    auto handle    = std::make_shared<AgentxxKeybind>();
    handle->inst   = inst;
    handle->keys   = keys;
    inst->keybindHandles.push_back(handle);
    return handle.get();
}

void ClientPluginManager::unregisterKeybind(ClientPluginInstance* inst, AgentxxKeybind* bind) {
    if (!inst || !bind) {
        return;
    }
    auto* handle = static_cast<AgentxxKeybind*>(bind);
    if (handle->inst != inst) {
        // 跨实例句柄或已注销的句柄 (unregisterPanel 同规则: 句柄保活到实例析构,
        // 注销只把 inst 置空; 句柄释放时机由 ~ClientPluginInstance 决定)
        return;
    }
    const std::string keys = handle->keys;
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        std::erase_if(cur->keybinds, [&](const ClientKeybind& k) {
            return k.plugin == inst->name && k.keys == keys;
        });
        uiRegistry_ = std::move(cur);
    }
    std::erase_if(inst->keybindRegs, [&](const ClientKeybind& k) {
        return k.plugin == inst->name && k.keys == keys;
    });
    handle->inst = nullptr; // 标记失效 (句柄本身保活到实例析构, 重复注销安全)
}

bool ClientPluginManager::hasKeybind(std::string_view keys) const {
    if (keys.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(uiMutex_);
    for (const auto& k : uiRegistry_->keybinds) {
        if (k.keys == keys) {
            return true;
        }
    }
    return false;
}

std::vector<ClientKeybind> ClientPluginManager::keybinds() const {
    std::lock_guard<std::mutex> lock(uiMutex_);
    auto                        out = uiRegistry_->keybinds;
    std::sort(out.begin(), out.end(), [](const ClientKeybind& a, const ClientKeybind& b) {
        if (a.keys != b.keys) {
            return a.keys < b.keys;
        }
        return a.plugin < b.plugin;
    });
    return out;
}

void ClientPluginManager::postKeybindInvocation(std::string keys) {
    auto self = shared_from_this();
    postToIo([self, keys = std::move(keys)]() mutable {
        ClientKeybind   hit;
        bool            found = false;
        std::shared_ptr<ClientPluginInstance> inst;
        {
            std::lock_guard<std::mutex> lock(self->uiMutex_);
            for (const auto& k : self->uiRegistry_->keybinds) {
                if (k.keys == keys) {
                    hit   = k;
                    inst  = self->find(k.plugin);
                    found = true;
                    break;
                }
            }
        }
        if (!found || !hit.handler || !inst || !inst->enabled) {
            XX_LOGD("[client_plugin] keybind `{}` not dispatched (unregistered or disabled)", keys);
            return;
        }
        PluginInstanceBase::InflightGuard guard(inst);
        if (!guard) {
            return;
        }
        try {
            hit.handler(hit.ud);
        } catch (const std::exception& e) {
            XX_LOGW("[client_plugin] keybind `{}` handler threw: {}", keys, e.what());
        } catch (...) {
            XX_LOGW("[client_plugin] keybind `{}` handler threw unknown exception", keys);
        }
    });
}

PluginxxSubscription* ClientPluginManager::subscribe(
    ClientPluginInstance* inst,
    int32_t               event,
    void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*),
    void* ud
) {
    if (!inst || !handler) {
        return nullptr;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("[client_plugin] `{}` subscribe rejected: closing or disabled", inst->name);
        return nullptr;
    }
    auto sub   = std::make_shared<ClientSubscriptionImpl>();
    sub->inst  = inst;
    auto s     = std::make_shared<ClientPluginInstance::Subscription>();
    s->event   = event;
    s->handler = handler;
    s->ud      = ud;
    s->alive   = true;
    inst->clientSubscriptions.push_back(s);
    sub->sub = s; // 强引用: 订阅对象从 vector 摘除后仍被句柄保活 (unload 回调内退订安全)
    inst->subHandles.push_back(sub);
    return reinterpret_cast<PluginxxSubscription*>(sub.get());
}

void ClientPluginManager::unsubscribe(PluginxxSubscription* sub) {
    auto impl = reinterpret_cast<ClientSubscriptionImpl*>(sub);
    if (!impl || !impl->inst || !impl->sub) {
        return;
    }
    impl->sub->alive = false;
    auto& subs       = impl->inst->clientSubscriptions;
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
    utilxx_base::Json j = utilxx_base::Json::object();
    j["name"]           = inst->name;
    j["version"]        = inst->version;
    j["description"]    = inst->description;
    j["path"]           = inst->path;
    j["config"]         = inst->configPath;
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
    ClientPluginInstance* inst,
    PluginxxStringView    sessionId,
    PluginxxStringView    text
) {
    (void)sessionId; // 会话以当前绑定为准 (sessionId 不符时由端点兜底)
    if (!inst || !uiAdapter_) {
        return;
    }
    // 实际发送由 UI 适配器完成 (与用户输入同排队语义)
    uiAdapter_->sendPluginMessage(svToStr(text));
}

void ClientPluginManager::requestCancelToPeer(
    ClientPluginInstance* inst,
    PluginxxStringView    sessionId
) {
    if (!inst || !uiAdapter_) {
        return;
    }
    uiAdapter_->requestCancel(svToStr(sessionId));
}

int ClientPluginManager::sendPluginDataToPeer(
    ClientPluginInstance* inst,
    PluginxxStringView    event,
    PluginxxStringView    json
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
    const auto idx = utilxx_base::findIndexByUtf8Length(line, maxCols);
    if (idx > 0 && idx < line.size()) {
        line.resize(idx);
        line += "...";
    }
    return line;
}

} // namespace

int ClientPluginManager::registerBuiltinToolRenderer(
    std::string_view    toolName,
    AgentxxToolRenderFn fn,
    void*               userData
) {
    if (toolName.empty() || !fn) {
        return -1;
    }
    ClientToolRenderReg reg;
    reg.plugin   = std::string{kBuiltinRendererOwner};
    reg.toolName = std::string{toolName};
    reg.renderFn = fn;
    reg.userData = userData;
    reg.builtin  = true;
    // 内置渲染器无插件实例与 lease: 不做租约登记, 也不进入实例的
    // toolRenderRegs (disable/enable 不涉及进程级内置渲染器)
    std::lock_guard<std::mutex> lock(uiMutex_);
    auto                        cur      = std::make_shared<ClientUiRegistry>(*uiRegistry_);
    bool                        replaced = false;
    for (auto& r : cur->builtinToolRenderers) {
        if (r.toolName == reg.toolName) {
            r        = reg;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        cur->builtinToolRenderers.push_back(std::move(reg));
    }
    uiRegistry_ = std::move(cur);
    return 0;
}

int ClientPluginManager::registerToolRenderer(
    ClientPluginInstance*        inst,
    const AgentxxToolRenderSpec* spec
) {
    if (!inst || !spec || spec->version != 1
        || agentxx::plugin::PluginStringView::empty(&spec->tool_name)) {
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "[client_plugin] `{}` registerToolRenderer rejected: closing or disabled",
            inst->name
        );
        return -1;
    }
    const std::string   tname = svToStr(spec->tool_name);
    ClientToolRenderReg reg;
    reg.plugin   = inst->name;
    reg.toolName = tname;
    reg.renderFn = spec->render_fn;
    reg.userData = spec->user_data;
    if (reg.renderFn) {
        reg.lease           = std::make_shared<ClientToolRendererLease>();
        reg.lease->instance = inst->self;
    }

    if (!spec->render_fn && !agentxx::plugin::PluginStringView::empty(&spec->template_json)) {
        reg.templateJson = svToStr(spec->template_json);
        if (!acceptUiJsonSize(reg.templateJson, "register_tool_renderer template", inst->name)) {
            return -1;
        }
        try {
            auto j = utilxx_base::Json::parse(reg.templateJson);
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
                if (r.lease) {
                    r.lease->alive.store(false, std::memory_order_release);
                }
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
            if (r.lease) {
                r.lease->alive.store(false, std::memory_order_release);
            }
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
    ClientPluginInstance* inst,
    PluginxxStringView    tool_name
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&tool_name)) {
        return -1;
    }
    const std::string tname = svToStr(tool_name);
    for (auto& r : inst->toolRenderRegs) {
        if (r.toolName == tname && r.lease) {
            r.lease->alive.store(false, std::memory_order_release);
        }
    }
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

int ClientPluginManager::bindActionHandler(
    ClientPluginInstance* inst,
    PluginxxStringView    target_id,
    AgentxxUiActionFn     on_action,
    void*                 user_data
) {
    if (!inst || !on_action) {
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("[client_plugin] `{}` bindActionHandler rejected: closing or disabled", inst->name);
        return -1;
    }
    const std::string target = svToStr(target_id); // 空串 = 实例级 fallback
    if (!hostSupportedInterfaces().contains(std::string{plugin_interfaces::ClientAction})) {
        XX_LOGW(
            "[client_plugin] bind_action `{}` rejected: interface agentxx.client.action unsupported",
            target.empty() ? "(fallback)" : target
        );
        return -1;
    }
    ClientActionBinding reg;
    reg.targetId = target;
    reg.plugin   = inst->name;
    reg.cb       = on_action;
    reg.ud       = user_data;
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur      = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        bool                        replaced = false;
        for (auto& b : cur->actionBindings) {
            if (b.plugin == inst->name && b.targetId == target) {
                b        = reg;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            cur->actionBindings.push_back(reg);
        }
        uiRegistry_ = std::move(cur);
    }
    bool replacedInst = false;
    for (auto& b : inst->actionRegs) {
        if (b.targetId == target) {
            b            = reg;
            replacedInst = true;
            break;
        }
    }
    if (!replacedInst) {
        inst->actionRegs.push_back(reg);
    }
    return 0;
}

int ClientPluginManager::unbindActionHandler(
    ClientPluginInstance* inst,
    PluginxxStringView    target_id
) {
    if (!inst) {
        return -1;
    }
    const std::string target = svToStr(target_id);
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        auto                        cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        cur->actionBindings.erase(
            std::remove_if(
                cur->actionBindings.begin(),
                cur->actionBindings.end(),
                [&](const auto& b) {
                    return b.plugin == inst->name && b.targetId == target;
                }
            ),
            cur->actionBindings.end()
        );
        uiRegistry_ = std::move(cur);
    }
    inst->actionRegs.erase(
        std::remove_if(
            inst->actionRegs.begin(),
            inst->actionRegs.end(),
            [&](const auto& b) {
                return b.targetId == target;
            }
        ),
        inst->actionRegs.end()
    );
    return 0;
}

void ClientPluginManager::dispatchAction(
    std::string plugin,
    std::string ownerId,
    std::string actionId,
    std::string argsJson,
    uint64_t    generation
) {
    if (plugin.empty() || actionId.empty()) {
        return;
    }
    // UI 线程只拷贝字符串快照, 派发回 io 线程二次校验 (防"点击瞬间 unbind/unload" UAF)
    auto self = shared_from_this();
    postToIo([self,
              plugin   = std::move(plugin),
              ownerId  = std::move(ownerId),
              actionId = std::move(actionId),
              argsJson = std::move(argsJson),
              generation]() mutable {
        auto inst = self->find(plugin);
        if (!inst || !inst->enabled) {
            XX_LOGW(
                "[client_plugin] dispatch action `{}` dropped: plugin `{}` missing/disabled",
                actionId,
                plugin
            );
            return;
        }
        // 实例代次复查: 同名插件重载后旧点击只能丢弃, 不得转交新实例
        if (generation != 0 && inst->lifetime && inst->lifetime->generation() != generation) {
            XX_LOGW(
                "[client_plugin] dispatch action `{}` dropped: plugin `{}` generation changed "
                "(click={}, now={})",
                actionId,
                plugin,
                generation,
                inst->lifetime->generation()
            );
            return;
        }
        // 精确匹配优先, 未命中回落实例级 fallback ("")
        const ClientActionBinding* hit = nullptr;
        for (const auto& b : inst->actionRegs) {
            if (b.targetId == ownerId && b.cb) {
                hit = &b;
                break;
            }
        }
        if (!hit) {
            for (const auto& b : inst->actionRegs) {
                if (b.targetId.empty() && b.cb) {
                    hit = &b;
                    break;
                }
            }
        }
        if (!hit) {
            XX_LOGW(
                "[client_plugin] dispatch action `{}` dropped: no binding (plugin=`{}`, owner=`{}`)",
                actionId,
                plugin,
                ownerId
            );
            return;
        }
        // 快照一致性: 快照中的 cb/ud 须与实例当前一致 (防点击瞬间 unbind 后重绑旧回调)
        {
            std::lock_guard<std::mutex> lock(self->uiMutex_);
            bool                        snapOk = false;
            for (const auto& b : self->uiRegistry_->actionBindings) {
                if (b.plugin == plugin && b.targetId == hit->targetId && b.cb == hit->cb
                    && b.ud == hit->ud) {
                    snapOk = true;
                    break;
                }
            }
            if (!snapOk) {
                XX_LOGW(
                    "[client_plugin] dispatch action `{}` dropped: binding changed mid-flight",
                    actionId
                );
                return;
            }
        }
        PluginInstanceBase::InflightGuard guard(inst->self.lock());
        AgentxxUiActionContext            ctx{};
        ctx.version     = 1;
        ctx.owner_id    = agentxx::plugin::PluginStringView::from(ownerId.data(), ownerId.size());
        ctx.action_id   = agentxx::plugin::PluginStringView::from(actionId.data(), actionId.size());
        ctx.action_args = agentxx::plugin::PluginStringView::from(argsJson.data(), argsJson.size());
        auto cb         = hit->cb;
        auto ud         = hit->ud;
        // C ABI 回调异常兜底: 单个插件违约不得打断 io 事件循环
        try {
            cb(&ctx, ud);
        } catch (const std::exception& e) {
            XX_LOGW("[client_plugin] `{}` action `{}` threw: {}", plugin, actionId, e.what());
        } catch (...) {
            XX_LOGW("[client_plugin] `{}` action `{}` threw unknown exception", plugin, actionId);
        }
    });
}

int ClientPluginManager::openOverlay(ClientPluginInstance* inst, const AgentxxOverlaySpec* spec) {
    if (!inst || !spec || spec->version != 1) {
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("[client_plugin] `{}` openOverlay rejected: closing or disabled", inst->name);
        return -1;
    }
    if (spec->type < AGENTXX_OVERLAY_MERMAID || spec->type > AGENTXX_OVERLAY_CUSTOM) {
        return -1;
    }
    if (!hostSupportedInterfaces().contains(std::string{plugin_interfaces::ClientOverlay})) {
        XX_LOGW(
            "[client_plugin] open_overlay rejected: interface agentxx.client.overlay unsupported"
        );
        return -1;
    }
    if (!uiAdapter_) {
        return -1;
    }
    const std::string title   = svToStr(spec->title);
    const std::string payload = svToStr(spec->payload);
    const std::string extra   = agentxx::plugin::PluginStringView::empty(&spec->extra_json)
                                    ? "{}"
                                    : svToStr(spec->extra_json);
    if (!acceptUiJsonSize(payload, "open_overlay payload", inst->name)
        || !acceptUiJsonSize(extra, "open_overlay extra_json", inst->name)) {
        return -1;
    }
    // 拷贝字符串后直调 adapter (TUI 实现内部 postToUi, 不阻塞插件)
    uiAdapter_->onOverlayOpen(inst->name, spec->type, title, payload, extra);
    return 0;
}

void ClientPluginManager::closeOverlay(ClientPluginInstance* inst) {
    if (!inst || !uiAdapter_) {
        return;
    }
    uiAdapter_->onOverlayClose(inst->name);
}

void ClientPluginManager::reportRegionSize(const std::string& id, int width, int height) {
    if (id.empty() || width <= 0) {
        return;
    }
    // 值未变化时直接返回 (布局每帧都会上报, 只有变化才通知插件)
    {
        std::lock_guard<std::mutex> lock(regionMutex_);
        auto&                       entry = regionSizes_[id];
        entry.id                          = id;
        if (entry.width == width && entry.height == height) {
            return;
        }
        entry.width  = width;
        entry.height = height;
    }
    // 事件投递到 io 线程 (插件回调的线程约定: 一律在 client io 线程执行)
    auto executor = ioExecutor();
    if (!executor) {
        return;
    }
    auto snapshot = regionSizes();
    auto payload  = utilxx_base::Json::object();
    auto regions  = utilxx_base::Json::array();
    for (const auto& r : snapshot) {
        auto item  = utilxx_base::Json::object();
        item["id"] = r.id;
        item["w"]  = r.width;
        item["h"]  = r.height;
        regions.push_back(std::move(item));
    }
    payload["regions"] = std::move(regions);
    const std::string text = payload.dump();
    auto              self = shared_from_this();
    asio::post(executor, [self, text]() {
        self->dispatchEvent(AGENTXX_CLIENT_EVT_UI_LAYOUT, text);
    });
}

std::vector<ClientRegionSize> ClientPluginManager::regionSizes() const {
    std::lock_guard<std::mutex> lock(regionMutex_);
    std::vector<ClientRegionSize> out;
    out.reserve(regionSizes_.size());
    for (const auto& entry : regionSizes_) {
        out.push_back(entry.second);
    }
    return out;
}

ClientToolRenderResult renderClientTool(
    const ClientUiRegistry*      reg,
    const ClientToolRenderCache* cache,
    std::string_view             toolCallId,
    std::string_view             toolName,
    std::string_view             argsJson,
    std::string_view             resultText,
    bool                         isFinished,
    bool                         isError,
    int                          maxWidth
) {
    ClientToolRenderResult res;
    if (!reg) {
        return res;
    }

    // 1. 优先匹配动态 per-call decor (由 update_tool_decor 针对 toolCallId 注册)
    if (!toolCallId.empty()) {
        for (const auto& d : reg->toolDecors) {
            if (d.toolCallId == toolCallId) {
                res.displayName     = d.displayName;
                res.summary         = d.summary;
                res.items           = d.items;
                res.matched         = true;
                res.isDecor         = true;
                res.decorPlugin     = d.plugin;
                res.decorToolCallId = d.toolCallId;
                return res;
            }
        }
    }

    // 2. 匹配按 toolName 注册的工具特化渲染器 (render_fn 或 预设模版):
    //    插件注册项优先, 未命中再查宿主内置项 (插件可覆盖内置渲染)
    if (!toolName.empty()) {
        for (const auto* rendererList : {&reg->toolRenderers, &reg->builtinToolRenderers}) {
            for (const auto& r : *rendererList) {
                if (r.toolName == toolName) {
                    if (r.renderFn) {
                        if (cache) {
                            ClientToolRenderRequest req;
                            req.toolCallId = std::string{toolCallId};
                            req.toolName   = std::string{toolName};
                            req.argsJson   = std::string{argsJson};
                            req.resultText = std::string{resultText};
                            req.isFinished = isFinished;
                            req.isError    = isError;
                            req.maxWidth   = maxWidth;
                            const std::string key
                                = ClientToolRenderRequest::keyFor(toolCallId, toolName);
                            auto cached = cache->lookup(key, req.inputHash());
                            if (!cached || cached->plugin != r.plugin) {
                                res.matched       = false;
                                res.pendingRender = true;
                                res.pendingPlugin = r.plugin;
                                return res;
                            }
                            res.matched     = cached->matched;
                            res.displayName = cached->displayName;
                            res.summary     = cached->summary;
                            res.items       = cached->items;
                            return res;
                        }

                        // 无语义缓存 (如单元测试/未装配 pluginManager 环境): 同步执行 renderer
                        AgentxxToolRenderInput input{};
                        input.version      = 1;
                        input.tool_call_id = agentxx::plugin::PluginStringView::from(
                            toolCallId.data(),
                            toolCallId.size()
                        );
                        input.tool_name = agentxx::plugin::PluginStringView::from(
                            toolName.data(),
                            toolName.size()
                        );
                        input.args_json = agentxx::plugin::PluginStringView::from(
                            argsJson.data(),
                            argsJson.size()
                        );
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
                            }
                            if (output.summary.data) {
                                res.summary.assign(
                                    output.summary.data,
                                    static_cast<size_t>(output.summary.size)
                                );
                            }
                            if (output.items_json.data) {
                                const std::string_view itemsJson{
                                    output.items_json.data,
                                    static_cast<size_t>(output.items_json.size)
                                };
                                if (acceptUiJsonSize(
                                        itemsJson,
                                        "tool_renderer output",
                                        r.plugin
                                    )) {
                                    try {
                                        res.items = utilxx_base::Json::parse(itemsJson);
                                    } catch (...) {
                                    }
                                }
                            }
                        }
                        hostMemoryFree(output.displayName.data);
                        hostMemoryFree(output.summary.data);
                        hostMemoryFree(output.items_json.data);
                        return res;
                    } else if (!r.templateDisplayName.empty() || !r.templateSummaryKey.empty()) {
                        res.displayName = r.templateDisplayName;
                        res.matched     = true;

                        if (!r.templateSummaryKey.empty() && !argsJson.empty()) {
                            try {
                                auto j = utilxx_base::Json::parse(argsJson);
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
    }

    return res;
}

} // namespace plugin
} // namespace agentxx
