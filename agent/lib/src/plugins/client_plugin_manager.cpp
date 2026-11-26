/// client_plugin_manager.cpp —— client 侧插件管理器实现
///
/// 线程模型 (与 agent 侧 PluginManager 一致的无锁单线程模型):
/// - 所有注册表/插件表/会话上下文状态仅 client io 线程读写
/// - UI 注册表 (uiRegistry_) 例外: io 线程写, UI 线程经快照读 (mutex + COW shared_ptr)
/// - dlopen/entry 卸载到内部 thread_pool; entry 的注册动作经 vtable 异步表操作
///   投递回 io 线程串行执行 (插件无感)
/// - 插件回调 (动作/定时器/快捷键/渲染器/命令/广播) 经 `dispatchToPlugin` 投到**插件
///   执行线程** (`agentxx-client-plugin`, 首次加载插件时自动起); 结果可立即交回或稍后交回
/// - UI 线程从不直接调用插件代码: 命令/点击经 postCommandInvocation / 派发投递
#include "agentxx/plugin/client_plugin_manager.h"
#include "agentxx/plugin/plugin_manager.h"
#include "pluginxx/host/kind_registry.h"
#include "pluginxx/host/table_op.h"
#include "pluginxx/runtime/driver.h"
#include "pluginxx/runtime/op_driver.h"
#include "utilxx_base/container_util.h"

#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/event/events.h"
#include "agentxx/event/naming.h"
#include "agentxx/plugin/api/agentxx_broadcast_ids.h"
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

/// 状态栏项主程序句柄实现 (全局作用域, 与
/// [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h)
/// 的 C 不透明类型对应 —— vtable 函数签名中的 AgentxxStatusItem 即此类型,
/// 不能在命名空间内另行定义)
struct AgentxxStatusItem {
    agentxx::plugin::ClientPluginInstance* inst = nullptr;
    std::string                            id;
    std::string                            plugin;
};

/// 面板主程序句柄实现
struct AgentxxPanel {
    agentxx::plugin::ClientPluginInstance* inst = nullptr;
    std::string                            id;
    std::string                            plugin;
};

/// Info 栏段落主程序句柄实现
struct AgentxxInfoSection {
    agentxx::plugin::ClientPluginInstance* inst = nullptr;
    std::string                            id;
    std::string                            plugin;
};

/// 全局快捷键主程序句柄实现
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
        "esc",
        "enter",
        "tab",
        "space",
        "backspace",
        "delete",
        "insert",
        "up",
        "down",
        "left",
        "right",
        "home",
        "end",
        "pageup",
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
    // F1~F24 功能键
    if (token.size() >= 2 && token[0] == 'f') {
        int  num = 0;
        bool ok  = true;
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
    bool        hasCtrl  = false;
    bool        hasAlt   = false;
    bool        hasShift = false;
    bool        hasSuper = false;
    bool        hasMod   = false;
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
        XX_LOGE(
            "[client_plugin] invalid command action json: {} (input {} bytes: {})",
            e.what(),
            jsonText.size(),
            jsonText.substr(0, 120)
        );
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
    if (!pluginDestroyed) {
        // destroy 从未完成 (或从未执行): 插件上下文还在, dlclose 会让残留代码跳进
        // 已卸载的内存。宁可泄漏 DSO, 也不在这里强行关闭。
        XX_LOGE(
            "[client_plugin] `{}` destroyed before its destroy entry finished; keeping plugin "
            "context and DSO loaded",
            name
        );
        return;
    }
    if (dlHandle) {
        NativeLoader::close(dlHandle);
        dlHandle = nullptr;
    }
    statusItemHandles.clear();
    panelHandles.clear();
    infoSectionHandles.clear();
}

// =====================================================================
// ClientPluginManager
// =====================================================================

namespace {

/// 本主程序的 client 侧预置形态 (与 agent 侧同一套规则)
///
/// - `native`: 动态库插件, 由框架加载 DSO 并调生命周期入口, 接口代次对是 client 侧的 C 接口那一对;
/// - `js`: 脚本形态的**预置位置**, 框架自己不实现 —— 引擎插件经 `agentxx.kind` 以
///   `overrideBuiltin: true` 接管后才能加载 (`requiresPluginAbi=false`, 代次对由登记它的运行器给)。
///   引擎没装 / 被禁用时, 加载 `kind: js` 的插件得到“缺运行器”而不是崩。
std::vector<pluginxx::PluginxxKindSpec> buildClientKindSpecs() {
    std::vector<pluginxx::PluginxxKindSpec> specs;
    pluginxx::PluginxxKindSpec              nativeSpec;
    nativeSpec.kind                = "native";
    nativeSpec.builtin             = true;
    nativeSpec.requiresPluginAbi   = true;
    nativeSpec.interfaceVersion    = AGENTXX_CLIENT_PLUGIN_API_VERSION;
    nativeSpec.minInterfaceVersion = AGENTXX_CLIENT_PLUGIN_API_VERSION;
    nativeSpec.describe            = "动态库插件 (框架加载)";
    specs.push_back(std::move(nativeSpec));

    pluginxx::PluginxxKindSpec scriptSpec;
    scriptSpec.kind              = "js";
    scriptSpec.builtin           = true;
    scriptSpec.requiresPluginAbi = false;
    scriptSpec.describe          = "脚本插件 (由运行器插件执行)";
    specs.push_back(std::move(scriptSpec));
    return specs;
}

} // namespace

ClientPluginManager::ClientPluginManager(asio::any_io_executor ex, size_t workerThreads) :
    pluginxx::PluginHostLifecycle<ClientPluginInstance>(std::move(ex)),
    uiRegistry_(std::make_shared<const ClientUiRegistry>()) {
    // 通用表 (log/json/config/plugins/broadcasts/...) 需要主程序数据时经这里取
    // (必须在任何 vtable 入口被调用之前注入)
    setDomainServices(this);
    // 插件形态表: 预置 `native` 与 `js` (后者只是位置, 由引擎插件接手)
    setKindRegistry(
        pluginxx::PluginxxKindRegistry(buildClientKindSpecs(), AGENTXX_CLIENT_PLUGIN_API_VERSION)
    );
    // 声明式界面: 槽位目录与初始快照 (界面线程随时可以读)
    uiSlotCatalog_ = buildClientUiSlotCatalog();
    publishUiContentSnapshot();
    // 客户端频道: 插件在 start 里就会订阅, 因此管理器构造时就声明好 (幂等)
    declareClientChannels();
    // 工作池与插件执行线程: 构造即起 (没有 client 插件也建有), 与 agent 侧同一模型
    const size_t workers = workerThreads == 0 ? kDefaultWorkerThreads : workerThreads;
    try {
        workerPool_ = std::make_shared<asio::thread_pool>(workers);
    } catch (const std::exception& e) {
        XX_LOGE("[client_plugin] worker pool could not be created: {}", e.what());
    }
    if (pluginThread_.start("agentxx-client-plugin")) {
        setPluginExecutor(pluginThread_.executor());
    } else {
        XX_LOGE(
            "[client_plugin] plugin executor thread could not start; plugin callbacks run on the "
            "client IO thread"
        );
    }
}

ClientPluginManager::~ClientPluginManager() {
    shutdownAll();
    netShutdownAll();
    warnPendingCloseOnDestroy("client");
    if (workerPool_) {
        workerPool_->join();
    }
}

// ==================== pluginxx::PluginHostLifecycle 的扩展点 (由本类覆写) ====================

/// 生成 client 侧实例对象
/// - 业务自引用 (self) 与所属管理器 (manager 弱引用);
/// - 其余公共部分 (生命周期入口 / 基类自引用 / 生命周期控制块 / 主程序控制块)
///   由 [attachInstance] 在加载路径中统一加载。
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

/// 停用收尾: 解释执行的形态先把脚本停掉
/// - 注册已由骨架摘除; 脚本自己的计时器 / 回调要随卸载一起回收,
///   否则插件已停用而脚本还在后台跑;
/// - 等待期间用户又启用它: 本次停用作废 (与 agent 侧同一处理), 交给启用事务
///   重新加载 (重复加载在运行器侧是幂等的)。
asio::awaitable<void> ClientPluginManager::onInstanceDisabledAsync(
    const std::shared_ptr<ClientPluginInstance>& inst
) {
    if (!inst || inst->enabled) {
        co_return;
    }
    co_await stopInterpretedInstance(inst);
    co_return;
}

/// 卸载开始后 (插件 stop/destroy 之前): 先把脚本停掉 —— 此刻实例还能被运行器代表
asio::awaitable<void> ClientPluginManager::onInstanceUnloadingAsync(
    const std::shared_ptr<ClientPluginInstance>& inst
) {
    co_await stopInterpretedInstance(inst);
    co_return;
}

/// 启用收尾: 解释执行的形态重新交给运行器加载 (脚本 start 里重新声明注册)
asio::awaitable<void> ClientPluginManager::onInstanceEnabledAsync(
    const std::shared_ptr<ClientPluginInstance>& inst
) {
    if (!inst || !inst->enabled || inst->kind.empty() || inst->kind == "native"
        || inst->kindLoaded) {
        co_return;
    }
    std::string reason;
    if (!co_await startInterpretedInstance(inst, reason)) {
        XX_LOGE("[client_plugin] `{}` could not be re-enabled: {}", inst->name, reason);
    }
    co_return;
}

// ==================== 加载 ====================

void ClientPluginManager::setUiAdapter(std::shared_ptr<PluginUiAdapter> adapter) {
    uiAdapter_ = std::move(adapter);
    // 界面能力段在加载时读取一次 (适配器加载后不可变, 见
    // [PluginUiAdapter::uiCapabilitiesJson]) —— get_client_state 是插件高频调用,
    // 每次重新解析 JSON 没有必要; 非法内容只记一条日志并按"未发布"处理
    uiCapabilitiesJson_ = utilxx_base::Json::object();
    if (uiAdapter_) {
        const std::string text = uiAdapter_->uiCapabilitiesJson();
        if (!text.empty()) {
            try {
                auto parsed = utilxx_base::Json::parse(text);
                if (parsed.is_object()) {
                    uiCapabilitiesJson_ = std::move(parsed);
                } else {
                    XX_LOGW("[client_plugin] ui capabilities is not a JSON object; ignored");
                }
            } catch (const std::exception& e) {
                XX_LOGW("[client_plugin] invalid ui capabilities JSON: {}", e.what());
            }
        }
    }
}

void ClientPluginManager::setSessionId(std::string sessionId) {
    sessionId_ = std::move(sessionId);
}

InterfaceSet ClientPluginManager::hostSupportedInterfaces() const {
    // 接口集唯一来源: UI 适配器声明 (位图方案已移除, 见
    // [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h)
    // "接口协商" 节)。无适配器 (纯测试/直连) 时空集 —— 仅命令输入管线等
    // 主程序固有能力也由适配器显式声明, 保持单一事实来源
    return uiAdapter_ ? uiAdapter_->supportedInterfaces() : InterfaceSet{};
}

// ==================== 生命周期 ====================

asio::awaitable<std::shared_ptr<ClientPluginInstance>> ClientPluginManager::loadNativeAsync(
    std::string                         path,
    const agentxx::agent::PluginConfig* cfg,
    bool                                allowMissingEntry
) {
    // 插件执行线程与工作池已在构造函数起好 (没有 client 插件也建有)
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
    //   dlopen —— 本函数一次 dlopen 完成 探测(entry 符号) + 加载
    std::error_code          ec;
    std::string              libPath = path;
    std::string              manifestKind;
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
                &interfaces,
                &manifestKind
            )) {
            XX_LOGE("[client_plugin] `{}` missing/invalid plugin.yaml", path);
            co_return nullptr;
        }
        // 脚本插件 (kind 不是 native) 没有动态库: 交给运行器插件解释执行
        // (没装运行器时报缺运行器, 不当错误)
        if (!manifestKind.empty() && manifestKind != "native") {
            co_return co_await loadInterpretedAsync(path, manifestKind, cfg);
        }
        libPath = resolvePluginEntryPath(std::filesystem::path(path), manifestEntry);
    }

    // dlopen 卸载到工作池 (避免阻塞 io 线程)
    std::string dlErr;
    void*       handle = co_await utilxx::offloadAsync<void*>(
        *workerPool_,
        [libPath, &dlErr]() -> asio::awaitable<void*> {
            co_return NativeLoader::open(libPath, dlErr);
        }
    );
    if (!handle) {
        XX_LOGE("[client_plugin] load failed: {}: {}", libPath, dlErr);
        co_return nullptr;
    }

    // 元信息 (可选符号; 框架给结构体, 插件填写 —— 见 pluginxx/api/abi.h)
    std::string name;
    std::string version;
    std::string desc;
    std::string err;
    if (auto getInfo = reinterpret_cast<PluginxxGetInfoFn>(
            NativeLoader::sym(handle, AGENTXX_CLIENT_PLUGIN_SYMBOL_GET_INFO, err)
        )) {
        // C ABI 回调异常保护: 插件违约按"未导出"处理 (名字从库名推导)
        PluginxxInfo info{};
        info.api_version = AGENTXX_CLIENT_PLUGIN_API_VERSION;
        info.struct_size = static_cast<uint32_t>(sizeof(PluginxxInfo));
        bool        infoReady = false;
        std::string infoError;
        try {
            const int32_t rc = getInfo(&info);
            if (rc == 0 && info.name.data && info.name.size > 0) {
                infoReady = true;
            } else {
                infoError = fmt::format("get_info returned {}", rc);
            }
        } catch (const std::exception& e) {
            infoError = fmt::format("get_info threw: {}", e.what());
        } catch (...) {
            infoError = "get_info threw unknown exception";
        }
        if (!infoError.empty()) {
            XX_LOGE("[client_plugin] `{}` metadata rejected: {}", path, infoError);
            NativeLoader::close(handle);
            co_return nullptr;
        }
        if (infoReady) {
            // 插件级闸门: 插件要求的最低主程序版本高于本端支持的版本时拒绝加载
            if (info.min_api_version > AGENTXX_CLIENT_PLUGIN_API_VERSION) {
                XX_LOGE(
                    "[client_plugin] `{}` requires API version {} but this host supports {}",
                    path,
                    info.min_api_version,
                    AGENTXX_CLIENT_PLUGIN_API_VERSION
                );
                NativeLoader::close(handle);
                co_return nullptr;
            }
            name    = std::string(info.name.data, static_cast<size_t>(info.name.size));
            version = std::string{
                info.version.data ? info.version.data : "",
                static_cast<size_t>(info.version.size)
            };
            desc = std::string{
                info.description.data ? info.description.data : "",
                static_cast<size_t>(info.description.size)
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
    // 同一插件目录服务 cli/tui/gui 多主程序, 本主程序缺某接口是预期情况);
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

    // 入口符号 (create / stop / destroy 三个都必需): 探测与加载合并 —— 一次 dlopen 内查符号
    std::string entryErr;
    auto        entryFn = reinterpret_cast<PluginxxCreateFn>(
        NativeLoader::sym(handle, AGENTXX_CLIENT_PLUGIN_SYMBOL_CREATE, entryErr)
    );
    std::string stopErr;
    auto        lifecycleStop = reinterpret_cast<PluginxxStopFn>(
        NativeLoader::sym(handle, AGENTXX_CLIENT_PLUGIN_SYMBOL_STOP, stopErr)
    );
    std::string destroyErr;
    auto        lifecycleDestroy = reinterpret_cast<PluginxxDestroyFn>(
        NativeLoader::sym(handle, AGENTXX_CLIENT_PLUGIN_SYMBOL_DESTROY, destroyErr)
    );
    // create / stop / destroy 都必需: 缺任一说明插件未按当前契约导出, 拒绝加载。
    if (entryFn && (!lifecycleStop || !lifecycleDestroy)) {
        XX_LOGE(
            "[client_plugin] `{}` missing lifecycle entry ({}: {}; {}: {}); plugins must export "
            "stop/destroy",
            path,
            AGENTXX_CLIENT_PLUGIN_SYMBOL_STOP,
            lifecycleStop ? "ok" : stopErr,
            AGENTXX_CLIENT_PLUGIN_SYMBOL_DESTROY,
            lifecycleDestroy ? "ok" : destroyErr
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
                AGENTXX_CLIENT_PLUGIN_SYMBOL_CREATE,
                entryErr
            );
        } else if (allowMissingEntry) {
            // sides==Auto: 无 client 入口视为纯 agent 插件, 静默跳过
            XX_LOGI("[client_plugin] `{}` has no client entry, skipped (agent-only)", name);
        } else {
            XX_LOGE(
                "[client_plugin] `{}` missing {}: {}",
                path,
                AGENTXX_CLIENT_PLUGIN_SYMBOL_CREATE,
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
    // 插件配置参数随加载直接传入 (与 agent 侧一致): 主程序不解析字段语义,
    // 插件经 vtable get_plugin_args 整体读取; 直连路径 cfg 为 nullptr → {}
    inst->args            = cfg ? cfg->args : utilxx_base::Json::object();
    inst->configPath      = cfg ? cfg->configPath : std::string{};
    inst->dlHandle        = handle;
    inst->depends         = std::move(depends);
    inst->optionalDepends = std::move(optionalDepends);
    inst->interfaces      = std::move(interfaces);
    // 生命周期入口 + 基类自引用 + 生命周期控制块 + 交给自己插件的主程序控制块
    // (host 视图放在进程级稳定的控制块里: 插件可能保存该指针并在卸载后继续调用,
    // 控制块 tombstone 保证这类迟到调用安全失败)
    attachInstance(inst, entryFn, lifecycleStop, lifecycleDestroy);

    // 创建事务 = 构造 + 注册, 一个完成包定成败 (在插件执行线程上跑, 不占 client io 线程)
    inst->pluginCtx = nullptr;
    std::string createError;
    if (!co_await pluginxx::awaitPluginLifecycleVoid(
            runtime(),
            inst,
            [&](const PluginxxOperatorNotify* notify, PluginxxString* errorOut) {
                entryFn(inst->hostView(), &inst->pluginCtx, notify, errorOut);
            },
            "client plugin create",
            createError
        )) {
        XX_LOGE("[client_plugin] `{}` create failed: {}", name, createError);
        inst->pluginCreated = (inst->pluginCtx != nullptr);
        detachAll(inst.get());
        co_await destroyInstanceAsync(inst);
        releasePluginName(name);
        if (inst->dlHandle) {
            NativeLoader::close(inst->dlHandle);
            inst->dlHandle = nullptr;
        }
        co_return nullptr;
    }
    inst->pluginCreated    = (inst->pluginCtx != nullptr);
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
// (见 pluginxx/host/lifecycle.h): 这些逻辑与主程序业务无关, client 与 agent 两侧
// 共用同一份实现 (含失败回滚、关闭超时、停止后重试、级联依赖)。
// 本类只保留两处 client 特有差异:
// - unloadAsync 的默认超时较短 (UI 交互路径, 见头文件);
// - 卸载级联只统计"启用中"的依赖者 (cascadeUnloadEnabledOnly)。
// 启停事务 (stop/start) 到达插件侧注册流程时, 业务动作经 detachDomainRegistrations 注入。

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

    // 主程序支持接口集 (加载前计算一次; 三层协商第 2 层的 require 限制数据源)
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
                // 与 agent 侧行为一致 (agent 侧会走 dlopen 失败报错):
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

std::string ClientToolRenderRequest::keyFor(
    std::string_view     toolCallId,
    std::string_view     toolName,
    ClientToolRenderPart part
) {
    std::string key;
    if (!toolCallId.empty()) {
        key.assign(toolCallId);
    } else {
        key = "#";
        key.append(toolName);
    }
    // 部分后缀: 折叠内容与展开内容各有缓存条目, 到达时间与输入特征互不影响
    key += (part == ClientToolRenderPart::Collapsed) ? "#c" : "#e";
    return key;
}

uint64_t ClientToolRenderRequest::inputHash() const {
    return hashInputs(toolName, argsJson, resultText, isFinished, isError, maxWidth);
}

uint64_t ClientToolRenderRequest::hashInputs(
    std::string_view toolName,
    std::string_view argsJson,
    std::string_view resultText,
    bool             isFinished,
    bool             isError,
    int              maxWidth
) {
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

std::shared_ptr<const ClientToolRenderEntry>
    ClientToolRenderCache::lookupLatest(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = entries_.find(key);
    return it == entries_.end() ? nullptr : it->second;
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

bool ClientToolRenderCache::inFlight(const std::string& key, uint64_t inputHash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = pending_.find(key);
    return it != pending_.end() && it->second == inputHash;
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

namespace {

/// 回答型回调 (工具折叠/展开内容、命令 execute) 的一次性完成通知
///
/// 这些回调不是表成员, 但同样走 `PluginxxOperatorNotify*` 契约: 插件返回 0 表示已受理,
/// 内容经 `done(status, payload)` 恰好一次交回。本对象负责
/// - **恰好一次**: 重复 `done` 只生效第一次;
/// - **快路径**: 插件在回调里立即 `done` 时 (常态) 就在本次调用栈上收尾,
///   与改造前同步出参一样当帧拿到内容;
/// - **晚到路径**: 插件从自己的线程 `done` 时投递回 client IO 线程再收尾。
///
/// 生命周期: 由第一次 `done` 自己释放 (每张票一对象, 不常驻)。插件既不 `done`
/// 也不返回就是违约, 对象会留下一个 (与框架对违约的处理一致: 只记日志, 不崩溃)。
class ReplySlot {
public:

    /// 收尾回调: `status` + 结果正文 (晚到路径下正文已拷成自有字符串)
    using Finalize = std::function<void(int32_t, std::string_view)>;

    ReplySlot(std::weak_ptr<ClientPluginManager> mgr, std::string label, Finalize fn) :
        mgr_(std::move(mgr)),
        label_(std::move(label)),
        finalize_(std::move(fn)) {}

    /// 交给插件的完成通知 (`done` 保证恰好一次)
    PluginxxOperatorNotify notify() {
        return PluginxxOperatorNotify{&ReplySlot::onDone, this};
    }

    static void PLUGINXX_CALL onDone(
        void*                     ud,
        int32_t                   status,
        const PluginxxStringView* payload
    ) {
        auto* slot = static_cast<ReplySlot*>(ud);
        if (slot == nullptr) {
            return;
        }
        if (slot->claimed_.exchange(true, std::memory_order_acq_rel)) {
            XX_LOGW("[client_plugin] `{}`: repeated completion ignored", slot->label_);
            return;
        }
        std::string text;
        if (payload != nullptr && payload->data != nullptr) {
            text.assign(payload->data, static_cast<size_t>(payload->size));
        }
        auto mgr = slot->mgr_.lock();
        if (!mgr) {
            delete slot;
            return;
        }
        if (mgr->isIoThread()) {
            slot->finish(status, text);
            delete slot;
            return;
        }
        // 插件从自己的线程完成: 收尾统一回到 client IO 线程 (界面状态归它)
        ReplySlot* raw = slot;
        try {
            mgr->postToIo([raw, status, text = std::move(text)]() {
                raw->finish(status, text);
                delete raw;
            });
        } catch (const std::exception& e) {
            XX_LOGW("[client_plugin] `{}` completion dropped: {}", raw->label_, e.what());
            delete raw;
        }
    }

private:

    void finish(int32_t status, std::string_view text) {
        try {
            finalize_(status, text);
        } catch (const std::exception& e) {
            XX_LOGW("[client_plugin] `{}` completion handler threw: {}", label_, e.what());
        } catch (...) {
            XX_LOGW("[client_plugin] `{}` completion handler threw", label_);
        }
    }

    std::weak_ptr<ClientPluginManager>   mgr_;
    std::string                          label_;
    Finalize                             finalize_;
    std::atomic<bool>                    claimed_{false};
};

} // namespace

bool ClientPluginManager::toolRenderInFlight(std::string_view key, uint64_t inputHash) const {
    return toolRenderCache_ && toolRenderCache_->inFlight(std::string{key}, inputHash);
}

std::shared_ptr<const ClientToolRenderEntry>
    ClientPluginManager::requestToolRender(const ClientToolRenderRequest& req) {
    if (req.toolName.empty()) {
        return nullptr;
    }
    const std::string key = ClientToolRenderRequest::keyFor(req.toolCallId, req.toolName, req.part);
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

/// 在 client io 线程执行自定义渲染器并把结果拷成主程序语义快照。
/// - 按请求的 `part` 只调对应部分的回调 (折叠内容不触发展开体渲染)
/// - 渲染器已摘除/失效/实例禁用 → 写入未命中条目 (UI 回退默认渲染)
/// - 插件回调没给内容 / 已摘除 / 实例禁用 → 写入未命中条目并清掉“进行中”标记,
///   避免每帧重复请求
/// - 插件回调可能当场完成 (常形), 也可能稍后才 `done`: 两种路径最终都走同一个
///   收尾函数 (写缓存 + 通知 UI 重绘)
void ClientPluginManager::performToolRender(
    ClientToolRenderRequest req,
    std::string             key,
    uint64_t                inputHash
) {
    ClientToolRenderEntry entry;
    entry.key       = key;
    entry.inputHash = inputHash;

    /// 本次没有自定义内容: 写一条未命中条目并清掉“进行中”标记
    /// (不清标记的话该键再也不会重新渲染)
    const auto storeUnmatched = [this, &entry, &key, inputHash]() {
        toolRenderCache_->store(std::move(entry));
        toolRenderCache_->endRequest(key, inputHash);
    };

    std::shared_ptr<const ClientUiRegistry> snapshot;
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        snapshot = uiRegistry_;
    }
    // 快照强引用保活: hit 指向快照内的注册记录, 后续插件回调期间不得失效
    // 查找顺序与 [renderClientToolCollapsed/Expanded] 一致: 插件注册项优先,
    // 未命中再查主程序内置项
    const bool                 wantCollapsedFlag = (req.part == ClientToolRenderPart::Collapsed);
    const ClientToolRenderReg* hit           = nullptr;
    const auto                 hasFn         = [wantCollapsedFlag](const ClientToolRenderReg& r) {
        return wantCollapsedFlag ? (r.collapsedFn != nullptr) : (r.expandedFn != nullptr);
    };
    for (const auto& r : snapshot->toolRenderers) {
        if (r.toolName == req.toolName && hasFn(r)) {
            hit = &r;
            break;
        }
    }
    if (!hit) {
        for (const auto& r : snapshot->builtinToolRenderers) {
            if (r.toolName == req.toolName && hasFn(r)) {
                hit = &r;
                break;
            }
        }
    }
    if (!hit) {
        storeUnmatched();
        return;
    }
    entry.plugin = hit->plugin;

    std::shared_ptr<ClientPluginInstance> owner;
    if (!hit->builtin) {
        if (!hit->lease || !hit->lease->alive.load(std::memory_order_acquire)) {
            storeUnmatched();
            return;
        }
        owner = hit->lease->instance.lock();
        if (!owner || !owner->enabled || !owner->lifetime || !owner->lifetime->acceptsOperations()) {
            storeUnmatched();
            return;
        }
        entry.generation = owner->lifetime ? owner->lifetime->generation() : 0;
    }

    AgentxxToolRenderInput input{};
    input.struct_size  = sizeof(AgentxxToolRenderInput);
    input.version      = 1;
    input.tool_call_id = agentxx::plugin::PluginStringView::from(req.toolCallId);
    input.tool_name    = agentxx::plugin::PluginStringView::from(req.toolName);
    input.args_json    = agentxx::plugin::PluginStringView::from(req.argsJson);
    input.result_text  = agentxx::plugin::PluginStringView::from(req.resultText);
    input.is_finished  = req.isFinished ? 1 : 0;
    input.is_error     = req.isError ? 1 : 0;
    input.max_width    = req.maxWidth;

    const std::string toolCallId    = req.toolCallId;
    const std::string toolName      = req.toolName;
    const bool        wantCollapsed = wantCollapsedFlag;
    const std::string pluginName    = hit->plugin;

    /// 实例仍可执行 (租约有效 + 启用中 + 生命周期允许)
    const auto ownerReady = [&hit, &owner]() {
        return hit->lease && hit->lease->alive.load(std::memory_order_acquire) && owner
               && owner->enabled && owner->lifetime && owner->lifetime->acceptsOperations();
    };
    if (!hit->builtin && !ownerReady()) {
        storeUnmatched();
        return;
    }

    /// 收尾: 把插件结果拷成主程序文本/JSON 写入缓存并通知 UI 重绘
    auto finish = [this,
                   entry = std::move(entry),
                   key,
                   inputHash,
                   toolCallId,
                   toolName,
                   wantCollapsed,
                   pluginName](int32_t status, std::string_view text) mutable {
        if (status == PLUGINXX_OPERATOR_OK && !text.empty()) {
            if (wantCollapsed) {
                entry.text    = std::string{text};
                entry.matched = !entry.text.empty();
            } else if (acceptUiJsonSize(text, "tool_renderer output", pluginName)) {
                try {
                    entry.items = utilxx_base::Json::parse(std::string{text});
                } catch (...) {
                }
                entry.matched = entry.items.is_array() && !entry.items.empty();
            }
        }
        toolRenderCache_->store(std::move(entry));
        toolRenderCache_->endRequest(key, inputHash);
        if (uiAdapter_) {
            uiAdapter_->onToolRenderUpdated(toolCallId, toolName);
        }
    };

    auto slot = std::make_unique<ReplySlot>(
        weak_from_this(),
        std::string{"client.ui.tool_render:"} + toolName,
        std::move(finish)
    );
    auto       notify = slot->notify();
    ReplySlot* raw    = slot.release();
    const auto cb     = wantCollapsed ? hit->collapsedFn : hit->expandedFn;

    if (hit->builtin) {
        // 主程序内置渲染器: 进程生命周期函数 (主程序自身实现), 无插件实例/租约,
        // 不经插件禁用/卸载路径, 直接在 io 线程执行
        PluginxxString err{nullptr, 0};
        int32_t        rc = AGENTXX_RESULT_ERR_INTERNAL;
        try {
            rc = cb(hit->userData, &input, &notify, &err);
        } catch (...) {
            rc = AGENTXX_RESULT_ERR_INTERNAL;
        }
        if (err.data) {
            XX_LOGW(
                "[client_plugin] tool renderer `{}` not accepted: {}",
                toolName,
                std::string{err.data, static_cast<size_t>(err.size)}
            );
            if (owner) {
                agentxx::plugin::PluginString::free(owner->hostView(), &err);
            }
        }
        if (rc != 0) {
            ReplySlot::onDone(raw, PLUGINXX_OPERATOR_FAILED, nullptr);
        }
        return;
    }

    // 插件渲染器: 回调投到插件执行器 (渲染回调是异步契约, 内容经 notify 交回)
    //
    // `hit` 指向当前注册表快照, 不能跨投递持有 (快照可能被替换释放):
    // 只捕回调与它的 userData。
    void*      renderUd = hit->userData;
    const bool queued   = pluginxx::dispatchToPlugin(
        owner,
        "client tool render " + toolName,
        [renderUd, cb, req, notify, owner, raw, toolCallId, toolName]() mutable {
            AgentxxToolRenderInput view{};
            view.struct_size  = sizeof(AgentxxToolRenderInput);
            view.version      = 1;
            view.tool_call_id = agentxx::plugin::PluginStringView::from(toolCallId);
            view.tool_name    = agentxx::plugin::PluginStringView::from(toolName);
            view.args_json    = agentxx::plugin::PluginStringView::from(req.argsJson);
            view.result_text  = agentxx::plugin::PluginStringView::from(req.resultText);
            view.is_finished  = req.isFinished ? 1 : 0;
            view.is_error     = req.isError ? 1 : 0;
            view.max_width    = req.maxWidth;
            PluginxxString err{nullptr, 0};
            int32_t        rc = AGENTXX_RESULT_ERR_INTERNAL;
            try {
                rc = cb(renderUd, &view, &notify, &err);
            } catch (const std::exception& e) {
                XX_LOGW("[client_plugin] tool renderer `{}` threw: {}", toolName, e.what());
            } catch (...) {
                XX_LOGW("[client_plugin] tool renderer `{}` threw", toolName);
            }
            if (err.data) {
                XX_LOGW(
                    "[client_plugin] tool renderer `{}` not accepted: {}",
                    toolName,
                    std::string{err.data, static_cast<size_t>(err.size)}
                );
                if (auto hostView = owner->hostView(); hostView != nullptr) {
                    agentxx::plugin::PluginString::free(hostView, &err);
                }
            }
            if (rc != 0) {
                // 未受理: 按“没有自定义内容”收尾
                ReplySlot::onDone(raw, PLUGINXX_OPERATOR_FAILED, nullptr);
            }
        },
        pluginxx::PluginDispatchOptions{.allowClosing = true, .measure = false}
    );
    if (!queued) {
        ReplySlot::onDone(raw, PLUGINXX_OPERATOR_FAILED, nullptr);
    }
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

    /// 收尾: 解析动作 JSON 并分发到 UI 适配器 (可能稍后才到)
    auto finish = [this](int32_t status, std::string_view text) {
        if (status != PLUGINXX_OPERATOR_OK) {
            if (!text.empty()) {
                XX_LOGW("[client_plugin] command failed: {}", text);
            }
            return;
        }
        dispatchCommandAction(std::string{text});
    };

    auto slot = std::make_unique<ReplySlot>(
        weak_from_this(),
        std::string{"client.ui.command:"} + name,
        std::move(finish)
    );
    auto                notify = slot->notify();
    auto                owner  = inst->self.lock();
    ReplySlot*          raw    = slot.release();
    if (!owner) {
        ReplySlot::onDone(raw, PLUGINXX_OPERATOR_FAILED, nullptr);
        return;
    }
    // 插件回调只能在它的执行器上跑: 受理结论 (rc/err) 也在那里得出
    //
    // `cmd` 指向当前注册表快照, 不能跨投递持有 (快照可能被替换释放): 只捕回调与它的
    // userData (与 [renderTool] 同一规则)。
    AgentxxClientCommandExecuteFn execute = cmd->execute;
    void*                          cmdUd   = cmd->ud;
    const bool                     queued  = pluginxx::dispatchToPlugin(
        owner,
        "client command " + name,
        [execute, cmdUd, name, argsJson, notify, owner, raw]() mutable {
            auto           argsSv = agentxx::plugin::PluginStringView::from(argsJson);
            PluginxxString err{nullptr, 0};
            int32_t        rc = AGENTXX_RESULT_ERR_INTERNAL;
            try {
                rc = execute(cmdUd, &argsSv, &notify, &err);
            } catch (const std::exception& e) {
                XX_LOGW("[client_plugin] command `{}` execute threw: {}", name, e.what());
            } catch (...) {
                XX_LOGW("[client_plugin] command `{}` execute threw unknown exception", name);
            }
            if (err.data) {
                XX_LOGW(
                    "[client_plugin] command `{}` not accepted: {}",
                    name,
                    std::string{err.data, static_cast<size_t>(err.size)}
                );
                if (auto hostView = owner->hostView(); hostView != nullptr) {
                    agentxx::plugin::PluginString::free(hostView, &err);
                }
            }
            if (rc != 0) {
                // 未受理: 本次命令什么也不做 (旧实现的“输出留空”)
                ReplySlot::onDone(raw, PLUGINXX_OPERATOR_FAILED, nullptr);
            }
        },
        pluginxx::PluginDispatchOptions{.allowClosing = true, .measure = false}
    );
    if (!queued) {
        ReplySlot::onDone(raw, PLUGINXX_OPERATOR_FAILED, nullptr);
    }
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
    // 主程序支持的接口名清单 (三层协商第 3 层 —— 插件据此自行决定启用哪些
    // 功能; 见 [plugin_interfaces.h](/agent/lib/include/agentxx/plugin/plugin_interfaces.h)
    // 接口协商节)。位图 uiCaps 字段已移除 (v4)
    j["interfaces"] = [&] {
        auto arr = utilxx_base::Json::array();
        for (const auto& n : hostSupportedInterfaces()) {
            arr.push_back(n);
        }
        return arr;
    }();
    // 客户端界面能力段 (界面描述层, 见
    // [PluginUiAdapter::uiCapabilitiesJson]): 内容由描述层库生成, 客户端如实上报
    // "能画哪些组件"。未发布的客户端不出现该字段, 插件按"描述层全支持"处理
    // (内容仍由客户端 adapt 降级)。
    if (uiCapabilitiesJson_.is_object() && !uiCapabilitiesJson_.empty()) {
        j["ui"] = uiCapabilitiesJson_;
    }
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
    // 展示区域尺寸快照 [{id,w,h},...]: 面板/Info 段落的可用宽高 (主程序布局后写入;
    // 插件据此按可用宽度自行重排; 频道 `agentxx.client.ui.layout` 会主动通知变化)
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
    // 主程序支持的接口名清单 (启动后最早可得的协商结果, 插件在 READY 回调内
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
    publishClientChannel(broadcasts::ClientReady, j.dump());
    // 三期6: 向服务端上报本 client 支持的接口集 (client 端主程序约定主题,
    // 镜像 server_plugins; 服务端存储并经事件总线发布, agent 侧插件订阅
    // `agentxx.client.host.client_interfaces` 据此自适应 —— 如 emit_message_tip
    // 在无 toast 接口的主程序上降级)
    if (uiAdapter_) {
        utilxx_base::Json up = utilxx_base::Json::object();
        up["sessionId"]      = sessionId_;
        up["interfaces"]     = j["interfaces"];
        uiAdapter_->sendPluginData(
            std::string{agentxx::events::Topic::ClientInterfaces},
            up.dump()
        );
    }
}

void ClientPluginManager::onConnStateChanged(std::string_view state, std::string_view progress) {
    connState_           = std::string{state};
    startupProgress_     = std::string{progress};
    utilxx_base::Json j  = utilxx_base::Json::object();
    j["connState"]       = connState_;
    j["startupProgress"] = startupProgress_;
    publishClientChannel(broadcasts::ClientConnectionState, j.dump());
}

void ClientPluginManager::onUserInput(std::string_view sessionId, std::string_view text) {
    utilxx_base::Json j = utilxx_base::Json::object();
    j["sessionId"]      = std::string{sessionId};
    j["text"]           = std::string{text};
    publishClientChannel(broadcasts::ClientUserInput, j.dump());
}

void ClientPluginManager::onDelta(const agentxx::agent::WireDelta& delta) {
    publishClientChannel(broadcasts::ClientDelta, agentxx::agent::io::deltaToJson(delta).dump());
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
    publishClientChannel(broadcasts::ClientTurnEnd, j.dump());
}

void ClientPluginManager::onSessionSwitched(std::string_view sessionId) {
    sessionId_ = std::string{sessionId};
    // 会话切换后旧的按 tool_call_id 语义结果不再有效, 整体失效 (UI 回退通用渲染)
    toolRenderCache_->clear();
    utilxx_base::Json j = utilxx_base::Json::object();
    j["sessionId"]      = sessionId_;
    publishClientChannel(broadcasts::ClientSessionSwitch, j.dump());
}

void ClientPluginManager::onPluginData(const agentxx::agent::WirePluginData& data) {
    // topic 为全名 (agent 端主程序约定主题在 `agentxx.agent.host.*`,
    // 插件事件在 `plugin.<插件名>.*`)
    // 主程序约定事件 `agentxx.agent.host.server_plugins` → 记录服务端已加载
    // 插件结构化信息 [{name,version,interfaces},...], 供 get_client_state
    // ("agentPlugins") 查询对端可用性与能力; 其余主题照常向插件分发
    if (data.topic == agentxx::events::Topic::HostServerPlugins) {
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

    // 对端缺失提示 (每插件名一次): 只看插件族主题 `plugin.<插件名>.*`。
    // 这条频道在本端没有订阅方时, 该插件事件在本地无人处理 —— 多半是对应
    // 插件未在本端加载 (分进程/分设备部署时单侧缺失)。仅警告一次, 不随事件
    // 频率刷屏; 正常情况 (有插件订阅, 各自按名过滤) 不受影响。
    // 主程序约定事件 (`agentxx.agent.host.client_attached` / `server_plugins`)
    // 由对端主程序发布, 本端没有任何插件订阅是正常情况 (client 插件关心的是
    // `agentxx.client.*` 自己的频道), 因此不按"插件缺失"提示。
    const auto pluginName = agentxx::events::naming::pluginNameOfTopic(data.topic);
    if (!pluginName.empty()) {
        bool hasSubscriber = false;
        if (broadcasts_) {
            try {
                const auto listed = utilxx_base::Json::parse(broadcasts_->listJson(data.topic));
                if (listed.contains("broadcasts") && listed["broadcasts"].is_array()) {
                    for (const auto& item : listed["broadcasts"]) {
                        if (item.value("subscribers", 0) > 0) {
                            hasSubscriber = true;
                            break;
                        }
                    }
                }
            } catch (...) {
                // 自省输出异常: 当作有订阅方, 只是不提示
                hasSubscriber = true;
            }
        }
        // 去重键 = 插件名 (同一插件的多条事件只提示一次)
        if (!hasSubscriber
            && pluginDataNoSubWarned_.insert(std::string{pluginName}).second) {
            XX_LOGW(
                "[client_plugin] plugin data `{}` received but no client plugin subscribed "
                "(plugin missing on this side? server-side loaded: {})",
                data.topic,
                serverPlugins_.empty() ? "unknown" : "yes"
            );
        }
    }

    // 跨端插件数据: 主题就是频道全名 (`plugin.<插件名>.<事件>`), 原样发给订阅方
    publishClientChannel(data.topic, data.data);
}

// ==================== 内部 ====================

/// 摘除插件在主程序侧的 UI 注册与订阅 (禁用与卸载共用; pluginxx 生命周期骨架的
/// 业务扩展点 [detachDomainRegistrations]):
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
    // 声明式界面: 声明项 / 内容 / 自建槽位跟着实例走 (重新启用时由 start 事务重新声明)
    detachUiContent(inst->name);
    // 网络资源跟着实例: 服务、连接与未完成请求一起收掉 (未完成按"已取消"结算)
    netShutdownPlugin(inst->name);
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
        // 快捷键冲突记录: 占用方被摘除 (键位空出) 或请求方被摘除 (记录作废) 都清掉
        std::erase_if(reg->keybindConflicts, [inst](const ClientKeybindConflict& c) {
            return c.plugin == inst->name || c.owner == inst->name;
        });
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

    // 订阅句柄 (广播订阅) 由插件框架的注册表按归属回收: 禁用/卸载时
    // `removeChannelsOwnedBy` 与订阅句柄一并清掉, 这里不再逐个断链。
    inst->statusItemRegs.clear();
    inst->panelRegs.clear();
    inst->infoSectionRegs.clear();
    inst->commandRegs.clear();
    inst->keybindRegs.clear();
    inst->toolDecorRegs.clear();
    inst->toolRenderRegs.clear();
    inst->actionRegs.clear();
    // 快捷键句柄与 panel/status 句柄同规则: 不在此释放 (插件的 stop 回调可能仍在
    // 注销它们), 由 ~ClientPluginInstance 统一回收
}

// =====================================================================
// host vtable (C ABI)
// =====================================================================

namespace {

// =====================================================================
// vtable 入口公共前置
// =====================================================================

using ClientHostCall = PluginHostCall<ClientPluginInstance, ClientPluginManager>;

/// 解析主程序控制块，并持有实例/管理器强引用与 admission lease。
///
/// - 实例已卸载/已关闭，或传入的 host 指针不是本主程序发放的视图 -> 返回空上下文
///   （实例与管理器均为 nullptr），入口按失败返回，不访问已释放对象；
/// - `allowClosing=true` 用于只读查询：关闭过程中仍允许执行（lease 保证卸载会
///   等它返回）。注册、投递新工作等入口必须用默认值，Closing/Disabled 后拒绝；
/// - 返回的上下文按值捕获进投递闭包后，卸载的 idle 等待会覆盖"已排队但尚未在
///   IO 线程执行"的阶段 (`OpCore` 租约 + 完成通道保证卸载等到它)。
static ClientHostCall enterClientHost(const PluginxxHost* host, bool allowClosing = false) {
    return enterPluginHost<ClientPluginInstance, ClientPluginManager>(host, allowClosing);
}

/// 异步表调用的公共骨架 (契约见 pluginxx/host/table_op.h 与 client_plugin_api.h 文件头)
///
/// - 入口只做“解析上下文 + 交给 [startTableOp]”: 业务校验在 `work` 里做
///   (失败抛 [TableOpError], 经完成回调按 FAILED 交回);
/// - `work` 返回结果正文 (句柄成员给 [clientHandleToText], 无结果类给空串);
/// - `work` 在 client IO 线程执行, 调用方本来就在 IO 线程时**当场执行** ——
///   “注册完立刻可见”的顺序与同步调用一致, 也省掉一次投递;
/// - `allowClosing=true` 用于只读查询 (关闭中仍可查), 写入口用默认值。
template<bool AllowClosing = false, typename Fn>
static PluginxxOperatorHandle* asyncClientTableCall(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out,
    std::string              label,
    Fn&&                     work
) {
    auto call = enterClientHost(host, AllowClosing);
    if (!call.ok()) {
        pluginxx::setTableOpError(error_out, "plugin runtime is unavailable");
        return nullptr;
    }
    auto keep = call;
    return pluginxx::startTableOp(
        keep.manager(),
        cb,
        ud,
        error_out,
        std::move(label),
        [keep, work = std::forward<Fn>(work)]() mutable -> std::string {
            return work(keep.instance(), keep.manager());
        }
    );
}

/// 只读查询类入口的公共骨架 (允许关闭中查询): `work` 返回结果正文
/// - 空结果照旧交回 (调用方按“没有值”处理), 不当作失败
template<typename Fn>
static PluginxxOperatorHandle* asyncClientQuery(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out,
    std::string              label,
    Fn&&                     work
) {
    return asyncClientTableCall<true>(
        host,
        cb,
        ud,
        error_out,
        std::move(label),
        std::forward<Fn>(work)
    );
}

/// 句柄结果: 拿不到句柄时按业务失败交回 (旧实现的 NULL 返回值)
/// - `what`: 成员名, 作为失败原因前缀
template<typename Handle>
static std::string clientHandleResult(Handle* handle, std::string_view what) {
    if (handle == nullptr) {
        throw TableOpError(fmt::format("{}: rejected", what));
    }
    return agentxx::plugin::clientHandleToText(handle);
}

/// 状态码结果: 非 0 按业务失败交回 (旧实现的返回码)
static std::string clientStatusResult(int rc, std::string_view what) {
    if (rc == 0) {
        return std::string{};
    }
    throw TableOpError(
        fmt::format("{}: {}", what, agentxx::plugin::resultName(static_cast<int32_t>(rc)))
    );
}

/// ABI 字符串视图 → std::string
///
/// 表入参都是**借用视图**: 工作体可能稍后在 IO 线程执行 (调用方在别的线程时),
/// 那时原内存已经失效 —— 捕获前必须先按值拷一份。
static std::string copyView(const PluginxxStringView& sv) {
    return sv.data ? std::string{sv.data, static_cast<size_t>(sv.size)} : std::string{};
}

/// "agentxx.client.ui" 展示接口表访问器 (定义于下方接口表加载区, 需在
/// xx_cquery_interface 处前向引用)
static const AgentxxClientUiIface* clientUiIface();

/// 其余标准接口表 (定义于下方加载区; 此处前向引用供 query_interface 分发)
extern const AgentxxClientSessionIface g_clientIfaceSession;
extern const AgentxxClientWireIface    g_clientIfaceWire;
extern const AgentxxClientSelfIface    g_clientIfaceSelf;
/// 定时器 / 全局快捷键接口表 (agentxx.client.timer / agentxx.client.keybind)
extern const AgentxxClientTimerIface   g_clientIfaceTimer;
extern const AgentxxClientKeybindIface g_clientIfaceKeybind;
/// 网络接口表 (`<应用名>.net`; 与 agent 侧同一个 `PluginNetService`)
extern const PluginxxNetIface g_clientIfaceNet;

// ---- 内存 ----

static void* PLUGINXX_CALL xx_calloc(uint64_t size) {
    return agentxx::plugin::hostMemoryAlloc(size);
}

static void PLUGINXX_CALL xx_cfree(void* ptr) {
    agentxx::plugin::hostMemoryFree(ptr);
}

// ---- COM 风格接口表查询 ----
const void* PLUGINXX_CALL
    xx_cquery_interface(const PluginxxHost* host, const PluginxxStringView* iid) {
    if (!iid || !iid->data) {
        return nullptr;
    }
    std::string_view n{iid->data, static_cast<size_t>(iid->size)};
    // 通用表 (log/json/config/plugins/broadcasts/scheduler/tasks/
    // cancel/capabilities) 由 cxx_pluginxx 提供 (见 pluginxx/host/tables_impl.h),
    // 主程序侧的业务表在下面逐个分发。
    if (const void* generic = pluginxx::queryGenericPluginIface<ClientPluginInstance, ClientPluginManager>(n)) {
        return generic;
    }
    if (n == AGENTXX_IFACE_CLIENT_UI) {
        return clientUiIface();
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
    if (n == AGENTXX_IFACE_CLIENT_TIMER) {
        return &g_clientIfaceTimer;
    }
    if (n == AGENTXX_IFACE_CLIENT_KEYBIND) {
        return &g_clientIfaceKeybind;
    }
    if (n == PLUGINXX_IFACE_NET) {
        return &g_clientIfaceNet;
    }
    return nullptr;
}

// ---- 状态栏项 ----

PluginxxOperatorHandle* PLUGINXX_CALL xx_cregister_status_item(
    const PluginxxHost*       host,
    const PluginxxStringView* id,
    const PluginxxStringView* initial_json,
    int32_t                   align,
    int32_t                   order,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!id || !id->data || id->size == 0) {
        pluginxx::setTableOpError(error_out, "register_status_item: id is required");
        return nullptr;
    }
    auto idCopy = copyView(*id);
    auto json   = initial_json && initial_json->data
                      ? copyView(*initial_json)
                      : std::string{"{}"};
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.register_status_item",
        [idCopy = std::move(idCopy), json = std::move(json), align, order](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            return clientHandleResult(
                static_cast<AgentxxStatusItem*>(
                    mgr->registerStatusItem(inst, idCopy, json, align, order)
                ),
                "register_status_item"
            );
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cupdate_status_item(
    const PluginxxHost*       host,
    AgentxxStatusItem*        item,
    const PluginxxStringView* json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!item || !json || !json->data) {
        pluginxx::setTableOpError(error_out, "update_status_item: item and json are required");
        return nullptr;
    }
    auto jsonCopy = copyView(*json);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.update_status_item",
        [item, jsonCopy = std::move(jsonCopy)](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            return clientStatusResult(
                mgr->updateStatusItem(inst, item, jsonCopy),
                "update_status_item"
            );
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cunregister_status_item(
    const PluginxxHost*      host,
    AgentxxStatusItem*       item,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    if (!item) {
        pluginxx::setTableOpError(error_out, "unregister_status_item: item is required");
        return nullptr;
    }
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.unregister_status_item",
        [item](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            mgr->unregisterStatusItem(inst, item);
            return {};
        }
    );
}

// ---- 侧边栏面板 ----

PluginxxOperatorHandle* PLUGINXX_CALL xx_cregister_panel(
    const PluginxxHost*       host,
    const PluginxxStringView* id,
    const PluginxxStringView* props_json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!id || !id->data || id->size == 0) {
        pluginxx::setTableOpError(error_out, "register_panel: id is required");
        return nullptr;
    }
    auto idCopy  = copyView(*id);
    auto props   = props_json && props_json->data
                       ? copyView(*props_json)
                       : std::string{"{}"};
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.register_panel",
        [idCopy = std::move(idCopy), props = std::move(props)](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            return clientHandleResult(
                static_cast<AgentxxPanel*>(mgr->registerPanel(inst, idCopy, props)),
                "register_panel"
            );
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cupdate_panel(
    const PluginxxHost*       host,
    AgentxxPanel*             panel,
    const PluginxxStringView* items_json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!panel || !items_json || !items_json->data) {
        pluginxx::setTableOpError(error_out, "update_panel: panel and items are required");
        return nullptr;
    }
    auto items = copyView(*items_json);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.update_panel",
        [panel, items = std::move(items)](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            return clientStatusResult(mgr->updatePanel(inst, panel, items), "update_panel");
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cunregister_panel(
    const PluginxxHost*      host,
    AgentxxPanel*            panel,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    if (!panel) {
        pluginxx::setTableOpError(error_out, "unregister_panel: panel is required");
        return nullptr;
    }
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.unregister_panel",
        [panel](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            mgr->unregisterPanel(inst, panel);
            return {};
        }
    );
}

// ---- Info 栏段落 ----

PluginxxOperatorHandle* PLUGINXX_CALL xx_cregister_info_section(
    const PluginxxHost*       host,
    const PluginxxStringView* id,
    const PluginxxStringView* props_json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!id || !id->data || id->size == 0) {
        pluginxx::setTableOpError(error_out, "register_info_section: id is required");
        return nullptr;
    }
    auto idCopy = copyView(*id);
    auto props  = props_json && props_json->data
                      ? copyView(*props_json)
                      : std::string{"{}"};
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.register_info_section",
        [idCopy = std::move(idCopy), props = std::move(props)](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            return clientHandleResult(
                static_cast<AgentxxInfoSection*>(mgr->registerInfoSection(inst, idCopy, props)),
                "register_info_section"
            );
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cupdate_info_section(
    const PluginxxHost*       host,
    AgentxxInfoSection*       section,
    const PluginxxStringView* items_json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!section || !items_json || !items_json->data) {
        pluginxx::setTableOpError(error_out, "update_info_section: section and items are required");
        return nullptr;
    }
    auto items = copyView(*items_json);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.update_info_section",
        [section, items = std::move(items)](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            return clientStatusResult(
                mgr->updateInfoSection(inst, section, items),
                "update_info_section"
            );
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cunregister_info_section(
    const PluginxxHost*      host,
    AgentxxInfoSection*      section,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    if (!section) {
        pluginxx::setTableOpError(error_out, "unregister_info_section: section is required");
        return nullptr;
    }
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.unregister_info_section",
        [section](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            mgr->unregisterInfoSection(inst, section);
            return {};
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cupdate_tool_decor(
    const PluginxxHost*       host,
    const PluginxxStringView* tool_call_id,
    const PluginxxStringView* decor_json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    auto tcid  = tool_call_id && tool_call_id->data
                    ? copyView(*tool_call_id)
                    : std::string{};
    auto decor = decor_json && decor_json->data
                     ? copyView(*decor_json)
                     : std::string{};
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.update_tool_decor",
        [tcid = std::move(tcid), decor = std::move(decor)](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            return clientStatusResult(
                mgr->updateToolDecor(inst, tcid, decor),
                "update_tool_decor"
            );
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cregister_tool_renderer(
    const PluginxxHost*          host,
    const AgentxxToolRenderSpec* spec,
    PluginxxOperatorCallback     cb,
    void*                        ud,
    PluginxxString*              error_out
) {
    if (!spec) {
        pluginxx::setTableOpError(error_out, "register_tool_renderer: spec is required");
        return nullptr;
    }
    // spec 里的字符串视图是借用视图: 投递前拷一份, 与 spec 一起交给工作体
    struct SpecKeep {
        AgentxxToolRenderSpec spec{};
        std::string           toolName;
        std::string           displayName;

        explicit SpecKeep(const AgentxxToolRenderSpec* in) {
            spec = *in;
            if (in->tool_name.data != nullptr && in->tool_name.size > 0) {
                toolName.assign(in->tool_name.data, static_cast<size_t>(in->tool_name.size));
            }
            if (in->display_name.data != nullptr && in->display_name.size > 0) {
                displayName.assign(
                    in->display_name.data,
                    static_cast<size_t>(in->display_name.size)
                );
            }
            spec.tool_name    = agentxx::plugin::PluginStringView::from(toolName);
            spec.display_name = agentxx::plugin::PluginStringView::from(displayName);
        }
    };
    auto keep = std::make_shared<SpecKeep>(spec);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.register_tool_renderer",
        [keep](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return clientStatusResult(
                mgr->registerToolRenderer(inst, &keep->spec),
                "register_tool_renderer"
            );
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cunregister_tool_renderer(
    const PluginxxHost*       host,
    const PluginxxStringView* tool_name,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!tool_name || !tool_name->data) {
        pluginxx::setTableOpError(error_out, "unregister_tool_renderer: tool_name is required");
        return nullptr;
    }
    auto tname = copyView(*tool_name);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.unregister_tool_renderer",
        [tname = std::move(tname)](ClientPluginInstance* inst, ClientPluginManager* mgr)
            -> std::string {
            return clientStatusResult(
                mgr->unregisterToolRenderer(inst, tname),
                "unregister_tool_renderer"
            );
        }
    );
}

// ---- 命令 ----

PluginxxOperatorHandle* PLUGINXX_CALL xx_cregister_command(
    const PluginxxHost*          host,
    const PluginxxStringView*    name,
    const PluginxxStringView*    description,
    AgentxxClientCommandExecuteFn execute,
    void*                        command_ud,
    PluginxxOperatorCallback     cb,
    void*                        ud,
    PluginxxString*              error_out
) {
    if (!execute || !name || !name->data || name->size == 0) {
        pluginxx::setTableOpError(
            error_out,
            "register_command: name and execute are required"
        );
        return nullptr;
    }
    auto nameCopy = copyView(*name);
    auto desc     = description && description->data
                        ? copyView(*description)
                        : std::string{};
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.register_command",
        [nameCopy = std::move(nameCopy),
         desc     = std::move(desc),
         execute,
         command_ud](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return clientStatusResult(
                mgr->registerCommand(inst, nameCopy, desc, execute, command_ud),
                "register_command"
            );
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cunregister_command(
    const PluginxxHost*       host,
    const PluginxxStringView* name,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!name || !name->data || name->size == 0) {
        pluginxx::setTableOpError(error_out, "unregister_command: name is required");
        return nullptr;
    }
    auto nameCopy = copyView(*name);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.unregister_command",
        [nameCopy = std::move(nameCopy)](ClientPluginInstance* inst, ClientPluginManager* mgr)
            -> std::string {
            return clientStatusResult(
                mgr->unregisterCommand(inst, nameCopy),
                "unregister_command"
            );
        }
    );
}

// ---- toast ----

PluginxxOperatorHandle* PLUGINXX_CALL xx_cshow_toast(
    const PluginxxHost*       host,
    const PluginxxStringView* text,
    int32_t                   level,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!text || !text->data) {
        pluginxx::setTableOpError(error_out, "show_toast: text is required");
        return nullptr;
    }
    auto textCopy = copyView(*text);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.show_toast",
        [textCopy = std::move(textCopy), level](
            ClientPluginInstance*,
            ClientPluginManager* mgr
        ) -> std::string {
            if (auto adapter = mgr->uiAdapter()) {
                adapter->onToast(std::string{textCopy}, level);
            }
            return {};
        }
    );
}

// ---- 会话上下文 ----

PluginxxOperatorHandle* PLUGINXX_CALL xx_cget_client_state(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return asyncClientQuery(
        host,
        cb,
        ud,
        error_out,
        "client.session.get_client_state",
        [](ClientPluginInstance*, ClientPluginManager* mgr) -> std::string {
            return mgr->clientStateJson();
        }
    );
}

// ---- 会话操作 ----

PluginxxOperatorHandle* PLUGINXX_CALL xx_csend_user_input(
    const PluginxxHost*       host,
    const PluginxxStringView* thread_id,
    const PluginxxStringView* text,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!text || !text->data || text->size == 0) {
        pluginxx::setTableOpError(error_out, "send_user_input: text is required");
        return nullptr;
    }
    auto tid  = thread_id && thread_id->data ? copyView(*thread_id)
                                             : std::string{};
    auto body = copyView(*text);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.session.send_user_input",
        [tid = std::move(tid), body = std::move(body)](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            mgr->sendUserInputToPeer(inst, tid, body);
            return {};
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_crequest_cancel(
    const PluginxxHost*       host,
    const PluginxxStringView* thread_id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    auto tid = thread_id && thread_id->data ? copyView(*thread_id)
                                            : std::string{};
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.session.request_cancel",
        [tid = std::move(tid)](ClientPluginInstance* inst, ClientPluginManager* mgr)
            -> std::string {
            mgr->requestCancelToPeer(inst, tid);
            return {};
        }
    );
}

// ---- 跨端数据 ----

PluginxxOperatorHandle* PLUGINXX_CALL xx_csend_plugin_data(
    const PluginxxHost*       host,
    const PluginxxStringView* topic,
    const PluginxxStringView* json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!topic || !topic->data || topic->size == 0) {
        pluginxx::setTableOpError(error_out, "send_plugin_data: topic is required");
        return nullptr;
    }
    auto topicCopy = copyView(*topic);
    auto payload   = json && json->data ? copyView(*json)
                                        : std::string{"{}"};
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.wire.send_plugin_data",
        [topicCopy = std::move(topicCopy), payload = std::move(payload)](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            return clientStatusResult(
                mgr->sendPluginDataToPeer(inst, topicCopy, payload),
                "send_plugin_data"
            );
        }
    );
}

// ---- 自描述 ----

PluginxxOperatorHandle* PLUGINXX_CALL xx_cget_own_info(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return asyncClientQuery(
        host,
        cb,
        ud,
        error_out,
        "client.self.get_own_info",
        [](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return mgr->getOwnInfoJson(inst);
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cget_plugin_args(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return asyncClientQuery(
        host,
        cb,
        ud,
        error_out,
        "client.self.get_plugin_args",
        [](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return mgr->getPluginArgsJson(inst);
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cget_plugin_config_path(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return asyncClientQuery(
        host,
        cb,
        ud,
        error_out,
        "client.self.get_plugin_config_path",
        [](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return mgr->getPluginConfigPath(inst);
        }
    );
}

static PluginxxOperatorHandle* PLUGINXX_CALL xx_cget_language(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return asyncClientQuery(
        host,
        cb,
        ud,
        error_out,
        "client.self.get_language",
        [](ClientPluginInstance*, ClientPluginManager* mgr) -> std::string {
            auto lang = mgr->getLanguage();
            return lang.empty() ? std::string{"en"} : lang;
        }
    );
}

static PluginxxOperatorHandle* PLUGINXX_CALL
    xx_cset_language(const PluginxxHost* host, const PluginxxStringView* language,
                     PluginxxOperatorCallback cb, void* ud, PluginxxString* error_out) {
    std::string lang = (language && language->data)
                           ? std::string(language->data, static_cast<size_t>(language->size))
                           : std::string{};
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.self.set_language",
        [lang = std::move(lang)](ClientPluginInstance*, ClientPluginManager* mgr) -> std::string {
            mgr->setLanguage(lang);
            return {};
        }
    );
}

// ---- 通用交互: 动作绑定 / overlay ----

PluginxxOperatorHandle* PLUGINXX_CALL xx_cbind_action_handler(
    const PluginxxHost*       host,
    const PluginxxStringView* target_id,
    AgentxxUiActionFn         on_action,
    void*                     user_data,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!on_action) {
        pluginxx::setTableOpError(error_out, "bind_action_handler: on_action is required");
        return nullptr;
    }
    auto target = target_id && target_id->data
                      ? copyView(*target_id)
                      : std::string{};
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.bind_action_handler",
        [target = std::move(target), on_action, user_data](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            return clientStatusResult(
                mgr->bindActionHandler(inst, target, on_action, user_data),
                "bind_action_handler"
            );
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cunbind_action_handler(
    const PluginxxHost*       host,
    const PluginxxStringView* target_id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    auto target = target_id && target_id->data
                      ? copyView(*target_id)
                      : std::string{};
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.unbind_action_handler",
        [target = std::move(target)](ClientPluginInstance* inst, ClientPluginManager* mgr)
            -> std::string {
            return clientStatusResult(
                mgr->unbindActionHandler(inst, target),
                "unbind_action_handler"
            );
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_copen_overlay(
    const PluginxxHost*       host,
    const AgentxxOverlaySpec* spec,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!spec) {
        pluginxx::setTableOpError(error_out, "open_overlay: spec is required");
        return nullptr;
    }
    // spec 里三个字符串视图是借用视图: 与 spec 一起放进同一个对象再捕获
    struct SpecKeep {
        AgentxxOverlaySpec spec{};
        std::string        title;
        std::string        payload;
        std::string        extra;

        explicit SpecKeep(const AgentxxOverlaySpec* in) {
            spec = *in;
            const auto copy = [](const PluginxxStringView& v, std::string& out) {
                if (v.data != nullptr && v.size > 0) {
                    out.assign(v.data, static_cast<size_t>(v.size));
                }
                return agentxx::plugin::PluginStringView::from(out);
            };
            spec.title      = copy(in->title, title);
            spec.payload    = copy(in->payload, payload);
            spec.extra_json = copy(in->extra_json, extra);
        }
    };
    auto keep = std::make_shared<SpecKeep>(spec);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.open_overlay",
        [keep](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return clientStatusResult(mgr->openOverlay(inst, &keep->spec), "open_overlay");
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cclose_overlay(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.close_overlay",
        [](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            mgr->closeOverlay(inst);
            return {};
        }
    );
}

// ---- 声明式界面 (声明项 / 内容 / 自建槽位) ----
//
// 与 agent 侧的 `musicxx.ui` 同一形状: 声明项只带数据, 内容是一个生成函数 (或直接 push)。
// 客户端的槽位只有 status / panel / info 三个分类, 具体是谁由内容 id 表达。

namespace {

/// 业务码 → 表操作结果 (OK 给空 payload; 其余抛 TableOpError)
std::string uiResultOrThrow(const char* what, int32_t rc) {
    if (rc == PLUGINXX_RESULT_OK) {
        return {};
    }
    const char* reason = "state";
    switch (rc) {
        case PLUGINXX_RESULT_ERR_ARG:
            reason = "arg";
            break;
        case PLUGINXX_RESULT_ERR_PERMISSION:
            reason = "namespace";
            break;
        case PLUGINXX_RESULT_ERR_NOT_DECLARED:
            reason = "unknown_slot";
            break;
        case PLUGINXX_RESULT_ERR_NOT_FOUND:
            reason = "not_found";
            break;
        default:
            reason = "state";
            break;
    }
    throw pluginxx::TableOpError(std::string{what} + ": " + reason);
}

/// 自有字符串 → ABI 视图 (借用, 只在本次调用的 lambda 生效)
PluginxxStringView uiViewOf(const std::string& text) {
    return PluginxxStringView{text.data(), static_cast<uint64_t>(text.size())};
}

} // namespace

PluginxxOperatorHandle* PLUGINXX_CALL xx_cregister_ui_item(
    const PluginxxHost*        host,
    const PluginxxUiEntrySpec* spec,
    PluginxxOperatorCallback   cb,
    void*                      ud,
    PluginxxString*            error_out
) {
    if (!spec) {
        pluginxx::setTableOpError(error_out, "register_ui_item: spec is required");
        return nullptr;
    }
    if (spec->version != 1) {
        pluginxx::setTableOpError(error_out, "register_ui_item: spec version must be 1");
        return nullptr;
    }
    auto itemId = copyView(spec->item_id);
    auto type   = copyView(spec->type);
    auto data   = copyView(spec->data_json);
    if (itemId.empty() || type.empty()) {
        pluginxx::setTableOpError(error_out, "register_ui_item: id and type are required");
        return nullptr;
    }
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.register_ui_item",
        [itemId = std::move(itemId), type = std::move(type), data = std::move(data), order =
                                                                                        spec->order](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            PluginxxUiEntrySpec owned{};
            owned.version     = 1;
            owned.struct_size = sizeof(PluginxxUiEntrySpec);
            owned.item_id     = uiViewOf(itemId);
            owned.type        = uiViewOf(type);
            owned.data_json   = uiViewOf(data);
            owned.order       = order;
            return uiResultOrThrow("register_ui_item", mgr->registerUiEntry(inst, &owned));
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cunregister_ui_item(
    const PluginxxHost*       host,
    const PluginxxStringView* item_id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!item_id || !item_id->data || item_id->size == 0) {
        pluginxx::setTableOpError(error_out, "unregister_ui_item: id is required");
        return nullptr;
    }
    auto id = copyView(*item_id);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.unregister_ui_item",
        [id = std::move(id)](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return uiResultOrThrow("unregister_ui_item", mgr->unregisterUiEntry(inst, id));
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_clist_ui_items(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.list_ui_items",
        [](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return mgr->listUiEntries(inst);
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cregister_ui_content(
    const PluginxxHost*          host,
    const PluginxxUiContentSpec* spec,
    PluginxxOperatorCallback     cb,
    void*                        ud,
    PluginxxString*              error_out
) {
    if (!spec || !spec->generate) {
        pluginxx::setTableOpError(error_out, "register_ui_content: spec with generate is required");
        return nullptr;
    }
    if (spec->version != 1) {
        pluginxx::setTableOpError(error_out, "register_ui_content: spec version must be 1");
        return nullptr;
    }
    auto id     = copyView(spec->id);
    auto slot   = copyView(spec->slot);
    auto title  = copyView(spec->title);
    auto depict = copyView(spec->depict);
    auto routes = copyView(spec->routes_json);
    auto deps   = copyView(spec->deps_json);
    if (id.empty() || slot.empty()) {
        pluginxx::setTableOpError(error_out, "register_ui_content: id and slot are required");
        return nullptr;
    }
    // 生成函数与 user_data 由插件提供: 属于这个实例, 实例卸载前一直有效
    auto generate = spec->generate;
    auto userData = spec->user_data;
    const int32_t order = spec->order;
    const int32_t minInterval = spec->min_interval_ms;
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.register_ui_content",
        [id = std::move(id),
         slot = std::move(slot),
         title = std::move(title),
         depict = std::move(depict),
         routes = std::move(routes),
         deps = std::move(deps),
         generate,
         userData,
         order,
         minInterval](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            PluginxxUiContentSpec owned{};
            owned.version         = 1;
            owned.struct_size     = sizeof(PluginxxUiContentSpec);
            owned.id              = uiViewOf(id);
            owned.slot            = uiViewOf(slot);
            owned.title           = uiViewOf(title);
            owned.depict          = uiViewOf(depict);
            owned.routes_json     = uiViewOf(routes);
            owned.deps_json       = uiViewOf(deps);
            owned.order           = order;
            owned.min_interval_ms = minInterval;
            owned.generate        = generate;
            owned.user_data       = userData;
            return uiResultOrThrow("register_ui_content", mgr->registerUiContent(inst, &owned));
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cunregister_ui_content(
    const PluginxxHost*       host,
    const PluginxxStringView* id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!id || !id->data || id->size == 0) {
        pluginxx::setTableOpError(error_out, "unregister_ui_content: id is required");
        return nullptr;
    }
    auto idCopy = copyView(*id);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.unregister_ui_content",
        [idCopy = std::move(idCopy)](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return uiResultOrThrow("unregister_ui_content", mgr->unregisterUiContent(inst, idCopy));
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cpush_ui_content(
    const PluginxxHost*       host,
    const PluginxxStringView* id,
    const PluginxxStringView* content_json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!id || !id->data || id->size == 0) {
        pluginxx::setTableOpError(error_out, "push_ui_content: id is required");
        return nullptr;
    }
    auto idCopy   = copyView(*id);
    auto jsonCopy = content_json && content_json->data ? copyView(*content_json) : std::string{"{}"};
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.push_ui_content",
        [idCopy = std::move(idCopy), jsonCopy = std::move(jsonCopy)](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            return uiResultOrThrow(
                "push_ui_content",
                mgr->pushUiContent(inst, idCopy, jsonCopy)
            );
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_clist_ui_contents(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.list_ui_contents",
        [](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return mgr->listUiContents(inst);
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cupdate_ui_slot(
    const PluginxxHost*       host,
    const PluginxxStringView* slot,
    const PluginxxStringView* ids_json,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!slot || !slot->data || slot->size == 0) {
        pluginxx::setTableOpError(error_out, "update_ui_slot: slot is required");
        return nullptr;
    }
    auto slotCopy = copyView(*slot);
    auto idsCopy  = ids_json && ids_json->data ? copyView(*ids_json) : std::string{};
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.update_ui_slot",
        [slotCopy = std::move(slotCopy), idsCopy = std::move(idsCopy)](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            return uiResultOrThrow("update_ui_slot", mgr->updateUiSlot(inst, slotCopy, idsCopy));
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cdefine_ui_slot(
    const PluginxxHost*       host,
    const PluginxxUiSlotSpec* spec,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!spec) {
        pluginxx::setTableOpError(error_out, "define_ui_slot: spec is required");
        return nullptr;
    }
    if (spec->version != 1) {
        pluginxx::setTableOpError(error_out, "define_ui_slot: spec version must be 1");
        return nullptr;
    }
    auto id   = copyView(spec->id);
    auto kind = copyView(spec->kind);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.define_ui_slot",
        [id = std::move(id), kind = std::move(kind)](
            ClientPluginInstance* inst,
            ClientPluginManager*  mgr
        ) -> std::string {
            PluginxxUiSlotSpec owned{};
            owned.version     = 1;
            owned.struct_size = sizeof(PluginxxUiSlotSpec);
            owned.id          = uiViewOf(id);
            owned.kind        = uiViewOf(kind);
            return uiResultOrThrow("define_ui_slot", mgr->defineUiSlot(inst, &owned));
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cundefine_ui_slot(
    const PluginxxHost*       host,
    const PluginxxStringView* slot,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!slot || !slot->data || slot->size == 0) {
        pluginxx::setTableOpError(error_out, "undefine_ui_slot: slot is required");
        return nullptr;
    }
    auto slotCopy = copyView(*slot);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.undefine_ui_slot",
        [slotCopy = std::move(slotCopy)](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return uiResultOrThrow("undefine_ui_slot", mgr->undefineUiSlot(inst, slotCopy));
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_clist_ui_slots(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.ui.list_ui_slots",
        [](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return mgr->listUiSlots(inst);
        }
    );
}

// ---- 定时器 (agentxx.client.timer) ----

PluginxxOperatorHandle* PLUGINXX_CALL xx_cset_timer(
    const PluginxxHost*       host,
    const AgentxxTimerSpec*   spec,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!spec) {
        pluginxx::setTableOpError(error_out, "set_timer: spec is required");
        return nullptr;
    }
    // spec 的 owner_id 是借用视图: 拷一份后重新指向
    struct SpecKeep {
        AgentxxTimerSpec spec{};
        std::string      ownerId;

        explicit SpecKeep(const AgentxxTimerSpec* in) {
            spec = *in;
            if (in->owner_id.data != nullptr && in->owner_id.size > 0) {
                ownerId.assign(in->owner_id.data, static_cast<size_t>(in->owner_id.size));
            }
            spec.owner_id = agentxx::plugin::PluginStringView::from(ownerId);
        }
    };
    auto keep = std::make_shared<SpecKeep>(spec);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.timer.set_timer",
        [keep](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return clientHandleResult(
                static_cast<AgentxxTimer*>(mgr->setTimer(inst, &keep->spec)),
                "set_timer"
            );
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_ccancel_timer(
    const PluginxxHost*      host,
    AgentxxTimer*            timer,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.timer.cancel_timer",
        [timer](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            mgr->cancelTimer(inst, timer);
            return {};
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cis_visible(
    const PluginxxHost*       host,
    const PluginxxStringView* owner_id,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    auto owner = owner_id && owner_id->data ? copyView(*owner_id)
                                            : std::string{};
    return asyncClientQuery(
        host,
        cb,
        ud,
        error_out,
        "client.timer.is_visible",
        [owner = std::move(owner)](ClientPluginInstance*, ClientPluginManager* mgr) -> std::string {
            if (owner.empty() || mgr == nullptr) {
                return std::string{"0"};
            }
            return mgr->isRegionVisible(owner) ? std::string{"1"} : std::string{"0"};
        }
    );
}

// ---- 全局快捷键 (agentxx.client.keybind) ----

PluginxxOperatorHandle* PLUGINXX_CALL xx_cregister_keybind(
    const PluginxxHost*       host,
    const AgentxxKeybindSpec* spec,
    PluginxxOperatorCallback  cb,
    void*                     ud,
    PluginxxString*           error_out
) {
    if (!spec) {
        pluginxx::setTableOpError(error_out, "register_keybind: spec is required");
        return nullptr;
    }
    // spec 两个字符串视图是借用视图: 拷一份后重新指向
    struct SpecKeep {
        AgentxxKeybindSpec spec{};
        std::string        keys;
        std::string        description;

        explicit SpecKeep(const AgentxxKeybindSpec* in) {
            spec = *in;
            if (in->keys.data != nullptr && in->keys.size > 0) {
                keys.assign(in->keys.data, static_cast<size_t>(in->keys.size));
            }
            if (in->description.data != nullptr && in->description.size > 0) {
                description.assign(
                    in->description.data,
                    static_cast<size_t>(in->description.size)
                );
            }
            spec.keys        = agentxx::plugin::PluginStringView::from(keys);
            spec.description = agentxx::plugin::PluginStringView::from(description);
        }
    };
    auto keep = std::make_shared<SpecKeep>(spec);
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.keybind.register_keybind",
        [keep](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            return clientHandleResult(
                static_cast<AgentxxKeybind*>(mgr->registerKeybind(inst, &keep->spec)),
                "register_keybind"
            );
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_cunregister_keybind(
    const PluginxxHost*      host,
    AgentxxKeybind*          bind,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return asyncClientTableCall(
        host,
        cb,
        ud,
        error_out,
        "client.keybind.unregister_keybind",
        [bind](ClientPluginInstance* inst, ClientPluginManager* mgr) -> std::string {
            mgr->unregisterKeybind(inst, bind);
            return {};
        }
    );
}

PluginxxOperatorHandle* PLUGINXX_CALL xx_clist_keybinds(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out
) {
    return asyncClientQuery(
        host,
        cb,
        ud,
        error_out,
        "client.keybind.list_keybinds",
        [](ClientPluginInstance*, ClientPluginManager* mgr) -> std::string {
            auto binds = mgr->keybinds();
            auto arr   = utilxx_base::Json::array();
            for (const auto& b : binds) {
                auto item           = utilxx_base::Json::object();
                item["keys"]        = b.keys;
                item["plugin"]      = b.plugin;
                item["description"] = b.description;
                arr.push_back(std::move(item));
            }
            return arr.dump();
        }
    );
}

/// "agentxx.client.ui" 展示接口表访问器: 表内成员恒非空 (函数实现存在), 子能力是否
/// 可用由各 register 入口的 hostSupportedInterfaces 限制决定 (拒绝时返回
/// NULL/非 0) —— 与接口表 "NULL = 不支持" 契约的分工: 表级 NULL 用于主程序
/// 整体缺失某子能力入口的场景 (当前主程序全部加载, 保留判空语义供第三方精简
/// 主程序使用)。以函数内静态表实现 (前向引用无需 extern 声明)
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
        /* register_ui_item */ xx_cregister_ui_item,
        /* unregister_ui_item */ xx_cunregister_ui_item,
        /* list_ui_items */ xx_clist_ui_items,
        /* register_ui_content */ xx_cregister_ui_content,
        /* unregister_ui_content */ xx_cunregister_ui_content,
        /* push_ui_content */ xx_cpush_ui_content,
        /* list_ui_contents */ xx_clist_ui_contents,
        /* update_ui_slot */ xx_cupdate_ui_slot,
        /* define_ui_slot */ xx_cdefine_ui_slot,
        /* undefine_ui_slot */ xx_cundefine_ui_slot,
        /* list_ui_slots */ xx_clist_ui_slots,
    };
    return &table;
}

// ---- 其余标准接口表 (进程级静态只读; 经 query_interface 分发) ----

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

/// 网络接口表 (`<应用名>.net`; 与 agent 侧同一个 `PluginNetService`)
///
/// 入口与 agent 侧同形: 一次网络操作 = 异步操作契约, 网络服务自有线程 (提交即返回),
/// 结果经 `OpCore` 的完成通道交回, 界面线程不会被一条请求拖住。
static PluginxxOperatorHandle* PLUGINXX_CALL xx_cnet_call(
    const PluginxxHost*       host,
    const PluginxxStringView* op,
    const PluginxxStringView* args_json,
    int64_t*                  request_id_out,
    PluginxxOperatorCallback  cb,
    void*                     cb_ud,
    PluginxxString*           error_out
) {
    return agentxx::plugin::guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
        auto call = enterClientHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(op)) {
            agentxx::plugin::hostMemorySetString(error_out, "net.call: host or op missing");
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        const std::string opStr{op->data, static_cast<size_t>(op->size)};
        const std::string argsStr
            = args_json && args_json->data
                  ? std::string{args_json->data, static_cast<size_t>(args_json->size)}
                  : std::string{"{}"};

        std::shared_ptr<pluginxx::OpCore> core;
        try {
            core = pluginxx::OpCore::createDetached(mgr->runtime(), "net." + opStr);
            core->setCallback(cb, cb_ud);
            core->accept();
        } catch (const std::exception& e) {
            agentxx::plugin::hostMemorySetString(error_out, e.what());
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }

        std::string   accepted;
        int64_t       requestId = 0;
        const int32_t rc        = mgr->netCallAsync(
            inst->name,
            opStr,
            argsStr,
            [](void* ud, int32_t status, const PluginxxStringView* payload) {
                // 完成端点的回调数据由本层保管 (网络服务只保证恰好一次)
                std::unique_ptr<std::shared_ptr<pluginxx::OpCore>> keep(
                    static_cast<std::shared_ptr<pluginxx::OpCore>*>(ud)
                );
                pluginxx::OpCore::onDoneCore(keep->get(), status, payload);
            },
            new std::shared_ptr<pluginxx::OpCore>(core),
            &accepted,
            &requestId,
            std::weak_ptr<ClientPluginInstance>(inst->sharedSelf<ClientPluginInstance>())
        );
        if (rc != PLUGINXX_RESULT_OK) {
            // 未受理: 回滚登记, 不回调
            core->reject();
            const char* name = pluginxx::result::nameOf(rc);
            XX_LOGW(
                "[client_plugin] net.call `{}` rejected: {} ({})",
                opStr,
                name != nullptr ? name : "unknown",
                accepted
            );
            agentxx::plugin::hostMemorySetString(
                error_out,
                accepted.empty()
                    ? fmt::format("net.call: {}", name != nullptr ? name : "rejected")
                    : accepted
            );
            return static_cast<PluginxxOperatorHandle*>(nullptr);
        }
        if (request_id_out != nullptr) {
            *request_id_out = requestId;
        }
        return core->handle();
    });
}

/// 取消进行中的网络请求
static int32_t PLUGINXX_CALL xx_cnet_cancel(
    const PluginxxHost* host,
    int64_t             request_id,
    PluginxxString*     out_json
) {
    return agentxx::plugin::guardVtableCall(PLUGINXX_RESULT_ERR_INTERNAL, [&]() -> int32_t {
        auto call = enterClientHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || request_id <= 0) {
            return PLUGINXX_RESULT_ERR_ARG;
        }
        std::string   result;
        const int32_t rc = mgr->netCancel(inst->name, request_id, &result);
        if (out_json != nullptr && !result.empty()) {
            hostMemorySetString(out_json, result);
        }
        return rc;
    });
}

const PluginxxNetIface g_clientIfaceNet = {
    /* version */ PLUGINXX_IFACE_NET_VERSION,
    /* struct_size */ sizeof(PluginxxNetIface),
    /* call */ xx_cnet_call,
    /* cancel */ xx_cnet_cancel,
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

namespace {

/// 旧状态栏项文本的可见长度 (与旧绘制一致: 只取第一行, 超长补 "...")
constexpr size_t kLegacyStatusTextMaxChars = 24;

/// 状态栏文本按旧绘制的方式裁剪 (第一行 + 最多 [kLegacyStatusTextMaxChars] 个字符)
std::string legacyStatusText(std::string_view text) {
    std::string out{text};
    const auto  nl = out.find('\n');
    if (nl != std::string::npos) {
        out.resize(nl);
    }
    if (const auto idx = utilxx_base::findIndexByUtf8Length(out, kLegacyStatusTextMaxChars);
        idx > 0 && idx < out.size()) {
        out.resize(idx);
        out += "...";
    }
    return out;
}

/// 旧状态栏项 → 界面描述层 JSON
///
/// - 有 `segments` / `sparkline` / `meter` 时给一行横排组件 (趋势图高度固定一行);
/// - 否则给一行文本; 富片段一项都用不上时也退回文本 (与旧绘制的回退一致)。
std::string legacyStatusContentJson(
    const utilxx_base::Json& props,
    bool                     hasRich,
    std::string_view         text
) {
    using utilxx_base::Json;
    Json blocks = Json::array();
    if (hasRich) {
        Json children = Json::array();
        if (const auto segs = props.find("segments"); segs != props.end() && segs->is_array()) {
            for (const auto& seg : *segs) {
                if (!seg.is_object()) {
                    continue;
                }
                const std::string segText = seg.value("text", std::string{});
                if (segText.empty()) {
                    continue;
                }
                Json cell;
                cell["kind"]  = "Text";
                cell["text"]  = segText;
                cell["color"] = seg.value("color", std::string{"hint"});
                children.push_back(std::move(cell));
            }
        }
        if (const auto sp = props.find("sparkline"); sp != props.end() && sp->is_object()) {
            Json item = *sp;
            item["kind"]   = "Sparkline";
            item["height"] = 1; // 状态栏只有一行高
            children.push_back(std::move(item));
        }
        if (const auto mt = props.find("meter"); mt != props.end() && mt->is_object()) {
            Json item = *mt;
            item["kind"] = "Progress";
            children.push_back(std::move(item));
        }
        if (!children.empty()) {
            Json row;
            row["kind"]     = "Row";
            row["gap"]      = 8;
            row["children"] = std::move(children);
            blocks.push_back(std::move(row));
        }
    }
    if (blocks.empty()) {
        Json line;
        line["kind"] = "Text";
        line["text"] = legacyStatusText(text);
        line["dim"]  = true; // 旧绘制: 纯文本项用 theme.dim()
        blocks.push_back(std::move(line));
    }
    Json doc;
    doc["blocks"] = std::move(blocks);
    return doc.dump();
}

/// 旧 items (面板 / Info 段落) → 界面描述层 JSON
/// - 旧 items 就是描述层的块数组, 原样交给渲染器
std::string legacyItemsContentJson(const utilxx_base::Json& items) {
    utilxx_base::Json doc = utilxx_base::Json::object();
    doc["items"]          = items;
    return doc.dump();
}

/// 旧注册项的全名 (注册 / 更新 / 注销三处必须算出同一个名字)
///
/// 名字带了自己的 `plugin.` 前缀就原样用; 带了别人的前缀时旧接口没有校验,
/// 仍然挂在当前插件名下 (旧接口不因此丢项)。
std::string legacyUiFullId(std::string_view instanceName, std::string_view rawId) {
    std::string id = clientUiFullName(instanceName, rawId);
    if (id.empty()) {
        id = "plugin." + std::string{instanceName} + "." + std::string{rawId};
    }
    return id;
}

} // namespace

std::string ClientPluginManager::upsertLegacyUi(
    ClientPluginInstance* inst,
    std::string_view      rawId,
    std::string_view      slot,
    std::string_view      entryDataJson,
    std::string_view      title,
    std::string_view      json
) {
    if (!inst || rawId.empty()) {
        return {};
    }
    const std::string fullId = legacyUiFullId(inst->name, rawId);
    // 声明项: 位置信息 (align / order / 标题) 进 data, 界面按它排序与定位
    auto entryIt = uiContent_.entries().find(fullId);
    if (entryIt != uiContent_.entries().end()) {
        entryIt->second.type     = std::string{slot};
        entryIt->second.dataJson = std::string{entryDataJson};
        entryIt->second.rev += 1;
    } else {
        pluginxx::host::UiEntry entry;
        entry.plugin   = inst->name;
        entry.pluginId = inst->name;
        entry.id       = fullId;
        entry.type     = std::string{slot};
        entry.dataJson = std::string{entryDataJson};
        entry.seq      = uiContent_.nextEntrySeq();
        uiContent_.entries().emplace(fullId, std::move(entry));
    }
    // 内容: 没有生成函数, 每次变化推一份 (注册时先给一份, 界面因此不会空着)
    auto contentIt = uiContent_.contents().find(fullId);
    if (contentIt != uiContent_.contents().end()) {
        contentIt->second.slot  = std::string{slot};
        contentIt->second.title = std::string{title};
    } else {
        pluginxx::host::UiContent content;
        content.plugin   = inst->name;
        content.pluginId = inst->name;
        content.id       = std::string{rawId};
        content.fullId   = fullId;
        content.slot     = std::string{slot};
        content.title    = std::string{title};
        content.depsJson = "[]";
        content.seq      = uiContent_.nextContentSeq();
        uiContent_.contents().emplace(fullId, std::move(content));
    }
    pushLegacyUi(fullId, json);
    return fullId;
}

void ClientPluginManager::pushLegacyUi(std::string_view fullId, std::string_view json) {
    if (fullId.empty()) {
        return;
    }
    const std::string payload = uiContent_.buildContentPayload(fullId, json, {});
    if (payload.empty()) {
        return;
    }
    publishUiContentSnapshot();
}

void ClientPluginManager::dropLegacyUi(std::string_view fullId) {
    if (fullId.empty()) {
        return;
    }
    bool changed = false;
    // 内容: 插件自己给它注册过生成函数时保留 (那一份由声明式接口管, 不随旧句柄失效);
    // 旧注册项建出来的内容 (没有生成函数) 跟着句柄一起摘
    bool contentKept = false;
    if (auto it = uiContent_.contents().find(fullId); it != uiContent_.contents().end()) {
        if (it->second.spec.generate != nullptr) {
            contentKept = true;
        } else {
            uiContent_.contents().erase(it);
            uiGenerating_.erase(std::string{fullId});
            changed = true;
        }
    }
    // 声明项与内容同名: 内容已由插件接手时声明项也留着 (它记的是位置信息)
    if (!contentKept && uiContent_.entries().erase(std::string{fullId}) > 0) {
        changed = true;
    }
    if (changed) {
        publishUiContentSnapshot();
    }
}

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
    const bool hasRich
        = props.is_object()
          && (props.contains("segments") || props.contains("sparkline") || props.contains("meter"));

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
    // 镜像到声明式注册表: 位置信息进声明项, 画什么由内容给出 (两边同一套存储)
    utilxx_base::Json entryData;
    entryData["align"] = align;
    entryData["order"] = order;
    (void)upsertLegacyUi(
        inst,
        idStr,
        kClientUiSlotStatus,
        entryData.dump(),
        std::string{},
        legacyStatusContentJson(props, hasRich, text)
    );
    return handle.get();
}

int ClientPluginManager::updateStatusItem(
    ClientPluginInstance* inst,
    void*                 item,
    PluginxxStringView    json
) {
    auto h = static_cast<AgentxxStatusItem*>(item);
    if (!inst || !h) {
        return AGENTXX_RESULT_ERR_STATE;
    }
    if (!acceptUiJsonSize(svToSv(json), "update_status_item", inst->name)) {
        return AGENTXX_RESULT_ERR_LIMIT;
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
    const bool hasRich
        = props.is_object()
          && (props.contains("segments") || props.contains("sparkline") || props.contains("meter"));
    // 富片段存在时 text 可省略 (状态栏以富内容为主, 文本仅作降级)
    if (!hasRich && text.empty()) {
        return AGENTXX_RESULT_ERR_ARG;
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
    pushLegacyUi(
        legacyUiFullId(inst->name, h->id),
        legacyStatusContentJson(props, hasRich, text)
    );
    return AGENTXX_RESULT_OK;
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
    dropLegacyUi(legacyUiFullId(inst->name, h->id));
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
    // 镜像到声明式注册表 (注册时还没有内容项, 先给一份空列表)
    utilxx_base::Json entryData;
    entryData["title"] = title;
    (void)upsertLegacyUi(
        inst,
        idStr,
        kClientUiSlotPanel,
        entryData.dump(),
        title,
        legacyItemsContentJson(utilxx_base::Json::array())
    );
    return handle.get();
}

int ClientPluginManager::updatePanel(
    ClientPluginInstance* inst,
    void*                 panel,
    PluginxxStringView    items_json
) {
    auto h = static_cast<AgentxxPanel*>(panel);
    if (!inst || !h) {
        return AGENTXX_RESULT_ERR_STATE;
    }
    if (!acceptUiJsonSize(svToSv(items_json), "update_panel", inst->name)) {
        return AGENTXX_RESULT_ERR_LIMIT;
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
        return AGENTXX_RESULT_ERR_JSON;
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
    pushLegacyUi(legacyUiFullId(inst->name, h->id), legacyItemsContentJson(items));
    return AGENTXX_RESULT_OK;
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
    dropLegacyUi(legacyUiFullId(inst->name, h->id));
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
    // 镜像到声明式注册表 (段落标题同时进声明项 data 与内容 title)
    utilxx_base::Json entryData;
    entryData["title"] = title;
    (void)upsertLegacyUi(
        inst,
        idStr,
        kClientUiSlotInfo,
        entryData.dump(),
        title,
        legacyItemsContentJson(utilxx_base::Json::array())
    );
    return handle.get();
}

int ClientPluginManager::updateInfoSection(
    ClientPluginInstance* inst,
    void*                 section,
    PluginxxStringView    items_json
) {
    auto h = static_cast<AgentxxInfoSection*>(section);
    if (!inst || !h) {
        return AGENTXX_RESULT_ERR_STATE;
    }
    if (!acceptUiJsonSize(svToSv(items_json), "update_info_section", inst->name)) {
        return AGENTXX_RESULT_ERR_LIMIT;
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
        return AGENTXX_RESULT_ERR_JSON;
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
    pushLegacyUi(legacyUiFullId(inst->name, h->id), legacyItemsContentJson(items));
    return AGENTXX_RESULT_OK;
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
    dropLegacyUi(legacyUiFullId(inst->name, h->id));
    h->inst = nullptr;
}

int ClientPluginManager::updateToolDecor(
    ClientPluginInstance* inst,
    PluginxxStringView    tool_call_id,
    PluginxxStringView    decor_json
) {
    if (!inst) {
        return AGENTXX_RESULT_ERR_STATE;
    }
    const std::string tid  = svToStr(tool_call_id);
    const std::string json = svToStr(decor_json);
    if (!acceptUiJsonSize(json, "update_tool_decor", inst->name)) {
        return AGENTXX_RESULT_ERR_LIMIT;
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
        return AGENTXX_RESULT_OK;
    }

    // 更新/插入
    ClientToolDecor decor;
    try {
        auto j = utilxx_base::Json::parse(json);
        if (!j.is_object()) {
            return AGENTXX_RESULT_ERR_JSON;
        }
        if (tid.empty()) {
            return AGENTXX_RESULT_ERR_ARG; ///< 更新必须指明 tool_call_id (删除才允许空 = 全部)
        }
        decor.plugin      = inst->name;
        decor.toolCallId  = tid;
        decor.displayName = j.value("displayName", std::string{});
        decor.summary     = j.value("summary", std::string{});
        if (j.contains("items") && j["items"].is_array()) {
            decor.items = j["items"];
        }
    } catch (...) {
        return AGENTXX_RESULT_ERR_JSON; ///< JSON 非法
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
    // 按正常帧率渲染 (工具消息本身的变化已驱动重绘)
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
    return AGENTXX_RESULT_OK;
}

int ClientPluginManager::registerCommand(
    ClientPluginInstance*        inst,
    PluginxxStringView           name,
    PluginxxStringView           description,
    AgentxxClientCommandExecuteFn exec,
    void*                        ud
) {
    if (!inst || !exec || agentxx::plugin::PluginStringView::empty(name)) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("[client_plugin] `{}` registerCommand rejected: closing or disabled", inst->name);
        return AGENTXX_RESULT_ERR_STATE;
    }
    std::string nameStr = svToStr(name);
    std::string descStr = svToStr(description);
    // 命令输入管线接口 (agentxx.client.command): 无命令输入面的主程序拒绝注册 ——
    // 与其他 register_* 的接口限制行为一致
    if (!hostSupportedInterfaces().contains(std::string{plugin_interfaces::ClientCommand})) {
        XX_LOGW(
            "[client_plugin] command `{}` rejected: interface agentxx.client.command unsupported",
            nameStr
        );
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        for (const auto& c : uiRegistry_->commands) {
            if (c.name == nameStr) {
                XX_LOGW("[client_plugin] command `{}` already registered", nameStr);
                return AGENTXX_RESULT_ERR_CONFLICT;
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
    return AGENTXX_RESULT_OK;
}

int ClientPluginManager::unregisterCommand(ClientPluginInstance* inst, PluginxxStringView name) {
    if (!inst || agentxx::plugin::PluginStringView::empty(name)) {
        return AGENTXX_RESULT_ERR_ARG;
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
    return AGENTXX_RESULT_OK;
}

// ==================== 定时器 (agentxx.client.timer) ====================

namespace {

/// 定时器间隔下限 (更小值按此限制, 避免插件把 io 线程打满)
constexpr int32_t kTimerMinIntervalMs = 50;
/// 单实例定时器数量上限
constexpr size_t kTimerMaxPerInstance = 8;
/// 单实例快捷键数量上限
constexpr size_t kKeybindMaxPerInstance = 16;

/// 等待一次到期 → 触发 (fireTimerTick) → 周期定时器续期
///
/// 说明: 定时器生命周期由 [ClientTimerImpl::alive] 控制; 取消后即使有尚未返回的等待
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
    const bool hidden
        = timer->pauseHidden && !timer->ownerId.empty() && !mgr->isRegionVisible(timer->ownerId);
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
    // 回调还在插件执行线程上跑 (耗时超过 interval): 本次到期直接跳过, 不排队追补 ——
    // 否则回调一返回就会连续补跑积压的那几次 (插件被自己的回调打满)
    const bool busy = timer->inFlight->load(std::memory_order_acquire);
    const bool usable = inst->enabled && inst->lifetime && inst->lifetime->acceptsOperations();
    if (stale || busy) {
        ++timer->dropped;
    }
    if (usable && !hidden && !stale && !busy) {
        // 插件回调在它的执行器上跑 (周期/次数记账仍在本线程, 与回调耗时无关)
        auto inFlight = timer->inFlight;
        inFlight->store(true, std::memory_order_release);
        if (!pluginxx::dispatchToPlugin(
                inst,
                "client timer",
                [cb = timer->cb, ud = timer->ud, inFlight] {
                    // 不论回调怎么结束都要放开标志, 否则这个定时器会一直停在"跳过"
                    struct Clear {
                        std::shared_ptr<std::atomic<bool>> flag;
                        ~Clear() {
                            flag->store(false, std::memory_order_release);
                        }
                    } clear{inFlight};
                    cb(ud);
                },
                pluginxx::PluginDispatchOptions{.allowClosing = true, .measure = false}
            )) {
            inFlight->store(false, std::memory_order_release);
            XX_LOGD("[client_plugin] timer callback dropped (instance closing)");
        }
        // 次数递减: 一次性定时器触发后结束, 周期定时器按 repeat 上限限制
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

AgentxxTimer*
    ClientPluginManager::setTimer(ClientPluginInstance* inst, const AgentxxTimerSpec* spec) {
    if (!inst || !spec || !spec->on_timer || spec->version != 1
        || !agentxx::plugin::specSizeOk<AgentxxTimerSpec>(spec->struct_size)) {
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
    // 能力门控: 与其它 register_* 入口一致 (主程序未声明定时器能力时拒绝)
    if (!hostSupportedInterfaces().contains(std::string{plugin_interfaces::ClientTimer})) {
        XX_LOGW(
            "[client_plugin] `{}` set_timer rejected: interface agentxx.client.timer unsupported",
            inst->name
        );
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
    auto impl         = std::make_shared<ClientTimerImpl>(executor);
    impl->inst        = inst->self;
    impl->ownerId     = svToStr(spec->owner_id);
    impl->intervalMs  = std::max<int32_t>(kTimerMinIntervalMs, spec->interval_ms);
    impl->repeatMode  = spec->repeat > 0;
    impl->repeat      = spec->repeat;
    impl->pauseHidden = spec->pause_when_hidden != 0;
    impl->cb          = spec->on_timer;
    impl->ud          = spec->user_data;
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
    auto it = std::find_if(inst->timers.begin(), inst->timers.end(), [&](const auto& h) {
        return h.get() == impl;
    });
    if (it == inst->timers.end()) {
        return;
    }
    impl->alive = false;
    impl->timer.cancel();
    // 不在此释放: 句柄地址需保持稳定 (见 fireTimerTick 说明)
}

bool acceptUiJsonSize(std::string_view json, std::string_view what, std::string_view plugin) {
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

AgentxxKeybind* ClientPluginManager::registerKeybind(
    ClientPluginInstance*     inst,
    const AgentxxKeybindSpec* spec
) {
    if (!inst || !spec || !spec->on_keybind || spec->version != 1
        || !agentxx::plugin::specSizeOk<AgentxxKeybindSpec>(spec->struct_size)) {
        return nullptr;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("[client_plugin] `{}` register_keybind rejected: closing or disabled", inst->name);
        return nullptr;
    }
    const std::string rawKeys = svToStr(spec->keys);
    const std::string keys    = normalizeKeybindSpec(rawKeys);
    if (keys.empty()) {
        XX_LOGW(
            "[client_plugin] `{}` register_keybind rejected: invalid keys `{}`",
            inst->name,
            rawKeys
        );
        return nullptr;
    }
    // 能力门控: 与其它 register_* 入口一致 (主程序未声明快捷键能力时拒绝)
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
                XX_LOGW("[client_plugin] keybind `{}` rejected: occupied by `{}`", keys, k.plugin);
                // 记录冲突 (供设置弹窗展示"为什么这个快捷键没生效"): 只针对跨插件
                // 占用, 同插件重复注册同一键位属插件自身行为, 不入表
                if (k.plugin != inst->name) {
                    const bool recorded = std::any_of(
                        uiRegistry_->keybindConflicts.begin(),
                        uiRegistry_->keybindConflicts.end(),
                        [&](const ClientKeybindConflict& c) {
                            return c.keys == keys && c.plugin == inst->name;
                        }
                    );
                    if (!recorded) {
                        auto cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
                        cur->keybindConflicts.push_back(
                            ClientKeybindConflict{keys, inst->name, k.plugin}
                        );
                        uiRegistry_ = std::move(cur);
                    }
                }
                return nullptr;
            }
        }
        auto cur = std::make_shared<ClientUiRegistry>(*uiRegistry_);
        cur->keybinds.push_back(reg);
        uiRegistry_ = std::move(cur);
    }
    inst->keybindRegs.push_back(std::move(reg));

    auto handle  = std::make_shared<AgentxxKeybind>();
    handle->inst = inst;
    handle->keys = keys;
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
        // 键位空出后相关冲突不再成立 (请求方下次注册可以成功);
        // 请求方自己注销时其记录一并作废
        std::erase_if(cur->keybindConflicts, [&](const ClientKeybindConflict& c) {
            return c.keys == keys || c.plugin == inst->name;
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
        ClientKeybind                         hit;
        bool                                  found = false;
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
        auto instKeep = inst;
        // 插件回调在它的执行器上跑 (按键事件本身已在这条 io 线程上完成查表与代次校验)
        if (!pluginxx::dispatchToPlugin(
                instKeep,
                "client keybind " + keys,
                [handler = hit.handler, ud = hit.ud] {
                    handler(ud);
                },
                pluginxx::PluginDispatchOptions{.allowClosing = true, .measure = false}
            )) {
            XX_LOGD("[client_plugin] keybind `{}` not dispatched (instance closing)", keys);
        }
    });
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
    (void)sessionId; // 会话以当前绑定为准 (sessionId 不符时由端点处理)
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
    PluginxxStringView    topic,
    PluginxxStringView    json
) {
    if (!inst || !uiAdapter_) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    const std::string topicStr = svToStr(topic);
    // 只能写 client 插件族 (`agentxx.client.plugin.<名字>.<事件>`); 不限定插件名,
    // 建议用自己的; 框架不补前缀
    std::string err;
    if (!agentxx::events::naming::validateClientPluginWriteTopic(topicStr, &err)) {
        XX_LOGW("[client_plugin] {} send_plugin_data rejected: {}", inst->name, err);
        return AGENTXX_RESULT_ERR_ARG;
    }
    const std::string j = agentxx::plugin::PluginStringView::empty(json) ? "{}" : svToStr(json);
    return uiAdapter_->sendPluginData(topicStr, j) ? AGENTXX_RESULT_OK
                                                   : AGENTXX_RESULT_ERR_FAILED;
}

namespace {

} // namespace

int ClientPluginManager::registerBuiltinToolRenderer(
    std::string_view             toolName,
    std::string_view             displayName,
    AgentxxToolCollapsedRenderFn collapsedFn,
    AgentxxToolExpandedRenderFn  expandedFn,
    void*                        userData
) {
    if (toolName.empty()) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    ClientToolRenderReg reg;
    reg.plugin      = std::string{kBuiltinRendererOwner};
    reg.toolName    = std::string{toolName};
    reg.displayName = displayName.empty() ? reg.toolName : std::string{displayName};
    reg.collapsedFn = collapsedFn;
    reg.expandedFn  = expandedFn;
    reg.userData    = userData;
    reg.builtin     = true;
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
    return AGENTXX_RESULT_OK;
}

int ClientPluginManager::registerToolRenderer(
    ClientPluginInstance*        inst,
    const AgentxxToolRenderSpec* spec
) {
    if (!inst || !spec || spec->version != 2
        || !agentxx::plugin::specSizeOk<AgentxxToolRenderSpec>(spec->struct_size)
        || agentxx::plugin::PluginStringView::empty(&spec->tool_name)) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "[client_plugin] `{}` registerToolRenderer rejected: closing or disabled",
            inst->name
        );
        return AGENTXX_RESULT_ERR_STATE;
    }
    const std::string   tname = svToStr(spec->tool_name);
    ClientToolRenderReg reg;
    reg.plugin      = inst->name;
    reg.toolName    = tname;
    // 显示名注册时定下 (空则用工具名), UI 线程同步读: 首帧就能显示名字
    reg.displayName = agentxx::plugin::PluginStringView::empty(&spec->display_name)
                          ? tname
                          : svToStr(spec->display_name);
    reg.collapsedFn = spec->collapsed_fn;
    reg.expandedFn  = spec->expanded_fn;
    reg.userData    = spec->user_data;
    if (reg.collapsedFn || reg.expandedFn) {
        reg.lease           = std::make_shared<ClientToolRendererLease>();
        reg.lease->instance = inst->self;
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
    return AGENTXX_RESULT_OK;
}

int ClientPluginManager::unregisterToolRenderer(
    ClientPluginInstance* inst,
    PluginxxStringView    tool_name
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&tool_name)) {
        return AGENTXX_RESULT_ERR_ARG;
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
    return AGENTXX_RESULT_OK;
}

int ClientPluginManager::bindActionHandler(
    ClientPluginInstance* inst,
    PluginxxStringView    target_id,
    AgentxxUiActionFn     on_action,
    void*                 user_data
) {
    if (!inst || !on_action) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("[client_plugin] `{}` bindActionHandler rejected: closing or disabled", inst->name);
        return AGENTXX_RESULT_ERR_STATE;
    }
    const std::string target = svToStr(target_id); // 空串 = 实例级 fallback
    if (!hostSupportedInterfaces().contains(std::string{plugin_interfaces::ClientAction})) {
        XX_LOGW(
            "[client_plugin] bind_action `{}` rejected: interface agentxx.client.action unsupported",
            target.empty() ? "(fallback)" : target
        );
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
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
    return AGENTXX_RESULT_OK;
}

int ClientPluginManager::unbindActionHandler(
    ClientPluginInstance* inst,
    PluginxxStringView    target_id
) {
    if (!inst) {
        return AGENTXX_RESULT_ERR_STATE;
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
    return AGENTXX_RESULT_OK;
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
        auto cb         = hit->cb;
        auto ud         = hit->ud;
        auto instKeep   = inst->self.lock();
        if (!instKeep) {
            return;
        }
        // 插件回调只能在它的执行器上跑: 视图数据拷进闭包, 异常由派发器记日志
        pluginxx::dispatchToPlugin(
            instKeep,
            "client action " + actionId,
            [cb, ud, ownerId, actionId, argsJson]() {
                AgentxxUiActionContext ctx{};
                ctx.struct_size = sizeof(AgentxxUiActionContext);
                ctx.version     = 1;
                ctx.owner_id
                    = agentxx::plugin::PluginStringView::from(ownerId.data(), ownerId.size());
                ctx.action_id
                    = agentxx::plugin::PluginStringView::from(actionId.data(), actionId.size());
                ctx.action_args
                    = agentxx::plugin::PluginStringView::from(argsJson.data(), argsJson.size());
                cb(&ctx, ud);
            },
            pluginxx::PluginDispatchOptions{.allowClosing = true, .measure = false}
        );
    });
}

int ClientPluginManager::openOverlay(ClientPluginInstance* inst, const AgentxxOverlaySpec* spec) {
    if (!inst || !spec || spec->version != 1
        || !agentxx::plugin::specSizeOk<AgentxxOverlaySpec>(spec->struct_size)) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("[client_plugin] `{}` openOverlay rejected: closing or disabled", inst->name);
        return AGENTXX_RESULT_ERR_ARG;
    }
    if (spec->type < AGENTXX_OVERLAY_MERMAID || spec->type > AGENTXX_OVERLAY_CUSTOM) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    if (!hostSupportedInterfaces().contains(std::string{plugin_interfaces::ClientOverlay})) {
        XX_LOGW(
            "[client_plugin] open_overlay rejected: interface agentxx.client.overlay unsupported"
        );
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    if (!uiAdapter_) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    const std::string title   = svToStr(spec->title);
    const std::string payload = svToStr(spec->payload);
    const std::string extra   = agentxx::plugin::PluginStringView::empty(&spec->extra_json)
                                    ? "{}"
                                    : svToStr(spec->extra_json);
    if (!acceptUiJsonSize(payload, "open_overlay payload", inst->name)
        || !acceptUiJsonSize(extra, "open_overlay extra_json", inst->name)) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    // 拷贝字符串后直调 adapter (TUI 实现内部 postToUi, 不阻塞插件)
    uiAdapter_->onOverlayOpen(inst->name, spec->type, title, payload, extra);
    return AGENTXX_RESULT_OK;
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
    // 区域尺寸快照先投回 io 线程 (注册表状态只在 io 线程上读写), 由 io 侧的发布路径
    // 再派发到插件执行线程通知订阅者
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
    payload["regions"]     = std::move(regions);
    const std::string text = payload.dump();
    auto              self = shared_from_this();
    asio::post(executor, [self, text]() {
        self->publishClientChannel(broadcasts::ClientUiLayout, text);
    });
}

std::vector<ClientRegionSize> ClientPluginManager::regionSizes() const {
    std::lock_guard<std::mutex>   lock(regionMutex_);
    std::vector<ClientRegionSize> out;
    out.reserve(regionSizes_.size());
    for (const auto& entry : regionSizes_) {
        out.push_back(entry.second);
    }
    return out;
}

namespace {

/// 按工具名查特化渲染注册项 (插件注册项优先, 未命中再查主程序内置项)
/// - `wantCollapsed` 决定"该注册项给了对应部分回调"的判据
const ClientToolRenderReg* findToolRenderReg(
    const ClientUiRegistry* reg,
    std::string_view        toolName,
    bool                    wantCollapsed
) {
    if (!reg || toolName.empty()) {
        return nullptr;
    }
    for (const auto* list : {&reg->toolRenderers, &reg->builtinToolRenderers}) {
        for (const auto& r : *list) {
            const bool hasFn
                = wantCollapsed ? (r.collapsedFn != nullptr) : (r.expandedFn != nullptr);
            if (r.toolName == toolName && hasFn) {
                return &r;
            }
        }
    }
    return nullptr;
}

/// 构造传给渲染回调的输入 (字段全部 string_view 借用, 只在本次回调内有效)
AgentxxToolRenderInput makeRenderInput(
    std::string_view toolCallId,
    std::string_view toolName,
    std::string_view argsJson,
    std::string_view resultText,
    bool             isFinished,
    bool             isError,
    int              maxWidth
) {
    AgentxxToolRenderInput input{};
    input.struct_size  = sizeof(AgentxxToolRenderInput);
    input.version      = 1;
    input.tool_call_id = agentxx::plugin::PluginStringView::from(
        toolCallId.data(),
        toolCallId.size()
    );
    input.tool_name = agentxx::plugin::PluginStringView::from(toolName.data(), toolName.size());
    input.args_json = agentxx::plugin::PluginStringView::from(argsJson.data(), argsJson.size());
    input.result_text
        = agentxx::plugin::PluginStringView::from(resultText.data(), resultText.size());
    input.is_finished = isFinished ? 1 : 0;
    input.is_error    = isError ? 1 : 0;
    input.max_width   = maxWidth;
    return input;
}

/// 没有渲染缓存时的“当场取结果”收尾 (单元测试 / 未挂载管理器)
///
/// 回调在本次调用栈上立即 `done` 时能拿到内容; 插件稍后才 `done` 时本次没有内容
/// (这种环境没有地方接晚到的结果, 与旧的同步形态行为一致)。
struct ImmediateReply {
    int32_t     status = PLUGINXX_OPERATOR_FAILED;
    std::string text;
    bool        done = false;
};

void PLUGINXX_CALL immediateReplyDone(
    void*                     ud,
    int32_t                   status,
    const PluginxxStringView* payload
) {
    auto* slot = static_cast<ImmediateReply*>(ud);
    if (slot == nullptr || slot->done) {
        return;
    }
    slot->done   = true;
    slot->status = status;
    if (payload != nullptr && payload->data != nullptr) {
        slot->text.assign(payload->data, static_cast<size_t>(payload->size));
    }
}

/// 直接调用一个渲染回调, 只收当场交回的内容 (`RendererFn` = 折叠或展开回调)
template<typename RendererFn>
std::string callRendererNow(RendererFn fn, void* userData, const AgentxxToolRenderInput* input) {
    if (fn == nullptr || input == nullptr) {
        return {};
    }
    ImmediateReply            slot;
    const PluginxxOperatorNotify notify{&immediateReplyDone, &slot};
    int32_t                   rc = AGENTXX_RESULT_ERR_INTERNAL;
    try {
        rc = fn(userData, input, &notify, nullptr);
    } catch (...) {
        rc = AGENTXX_RESULT_ERR_INTERNAL;
    }
    if (rc != 0 || !slot.done || slot.status != PLUGINXX_OPERATOR_OK) {
        return {};
    }
    return slot.text;
}

} // namespace

std::string_view toolDisplayNameFor(
    const ClientUiRegistry* reg,
    std::string_view        toolCallId,
    std::string_view        toolName
) {
    if (!reg) {
        return {};
    }
    // 1. 实例级装饰 (update_tool_decor) 的 displayName 优先 (按调用改名)
    if (!toolCallId.empty()) {
        for (const auto& d : reg->toolDecors) {
            if (d.toolCallId == toolCallId && !d.displayName.empty()) {
                return d.displayName;
            }
        }
    }
    // 2. 类型级注册项的名字 (插件项优先, 未命中再查内置项)
    if (!toolName.empty()) {
        for (const auto* list : {&reg->toolRenderers, &reg->builtinToolRenderers}) {
            for (const auto& r : *list) {
                if (r.toolName == toolName && !r.displayName.empty()) {
                    return r.displayName;
                }
            }
        }
    }
    return {};
}

ClientToolCollapsedResult renderClientToolCollapsed(
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
    ClientToolCollapsedResult res;
    if (!reg) {
        return res;
    }

    // 1. 实例级装饰: summary 非空时直接用; 装饰缺这一项时回落注册项 (不是清空)
    if (!toolCallId.empty()) {
        for (const auto& d : reg->toolDecors) {
            if (d.toolCallId == toolCallId && !d.summary.empty()) {
                res.text    = d.summary;
                res.isDecor = true;
                return res;
            }
        }
    }

    // 2. 按 tool_name 注册的折叠内容回调
    const auto* r = findToolRenderReg(reg, toolName, /*wantCollapsed=*/true);
    if (!r) {
        return res; // 没注册 → 默认渲染
    }
    if (!cache) {
        // 无语义缓存 (单元测试/未挂载管理器环境): 直接调回调, 只取当场交回的内容
        auto input = makeRenderInput(
            toolCallId,
            toolName,
            argsJson,
            resultText,
            isFinished,
            isError,
            maxWidth
        );
        res.text = callRendererNow(r->collapsedFn, r->userData, &input);
        return res;
    }

    // 零拷贝查缓存: 先用 string_view 算输入特征 + 查缓存, 命中即返回
    // (不拷贝 args/result 大文本); UI 侧只在 pending 时构造请求
    const std::string key = ClientToolRenderRequest::keyFor(
        toolCallId,
        toolName,
        ClientToolRenderPart::Collapsed
    );
    const uint64_t inputHash = ClientToolRenderRequest::hashInputs(
        toolName,
        argsJson,
        resultText,
        isFinished,
        isError,
        maxWidth
    );
    if (auto cached = cache->lookup(key, inputHash); cached && cached->plugin == r->plugin) {
        // 命中: 空内容 = 回调本次没给内容 (matched == false) → 默认渲染
        res.text = cached->text;
        return res;
    }
    res.pending       = true;
    res.pendingPlugin = r->plugin;
    // 过渡: 沿用同一调用同一部分的上一份内容 (结果到达/宽度变化使输入特征变化
    // 的那一帧继续显示屏幕上已有的内容)。没有 tool_call_id 时缓存键退化为
    // "#工具名#c", 可能是另一次调用的结果, 不沿用
    if (!toolCallId.empty()) {
        if (auto last = cache->lookupLatest(key); last && last->plugin == r->plugin) {
            res.text = last->text;
        }
    }
    return res;
}

ClientToolExpandedResult renderClientToolExpanded(
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
    ClientToolExpandedResult res;
    if (!reg) {
        return res;
    }

    // 1. 实例级装饰: items 非空时直接用; 装饰缺这一项时回落注册项 (不是清空)
    if (!toolCallId.empty()) {
        for (const auto& d : reg->toolDecors) {
            if (d.toolCallId == toolCallId && d.items.is_array() && !d.items.empty()) {
                res.items           = d.items;
                res.isDecor         = true;
                res.decorPlugin     = d.plugin;
                res.decorToolCallId = d.toolCallId;
                return res;
            }
        }
    }

    // 2. 按 tool_name 注册的展开内容回调
    const auto* r = findToolRenderReg(reg, toolName, /*wantCollapsed=*/false);
    if (!r) {
        return res; // 没注册 → 默认展开体
    }
    if (!cache) {
        // 无语义缓存 (单元测试/未挂载管理器环境): 直接调回调, 只取当场交回的内容
        auto       input = makeRenderInput(
            toolCallId,
            toolName,
            argsJson,
            resultText,
            isFinished,
            isError,
            maxWidth
        );
        const auto text  = callRendererNow(r->expandedFn, r->userData, &input);
        if (acceptUiJsonSize(text, "tool_renderer output", r->plugin)) {
            try {
                res.items = utilxx_base::Json::parse(text);
            } catch (...) {
            }
        }
        return res;
    }

    const std::string key
        = ClientToolRenderRequest::keyFor(toolCallId, toolName, ClientToolRenderPart::Expanded);
    const uint64_t inputHash = ClientToolRenderRequest::hashInputs(
        toolName,
        argsJson,
        resultText,
        isFinished,
        isError,
        maxWidth
    );
    if (auto cached = cache->lookup(key, inputHash); cached && cached->plugin == r->plugin) {
        res.items = cached->items;
        return res;
    }
    res.pending       = true;
    res.pendingPlugin = r->plugin;
    // 过渡: 上一份 items (没有 tool_call_id 时不沿用, 同折叠部分)
    if (!toolCallId.empty()) {
        if (auto last = cache->lookupLatest(key); last && last->plugin == r->plugin) {
            res.items = last->items;
        }
    }
    return res;
}

} // namespace plugin
} // namespace agentxx
