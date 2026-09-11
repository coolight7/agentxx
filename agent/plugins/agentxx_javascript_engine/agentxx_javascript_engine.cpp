/// agentxx_javascript_engine —— JS 解释器插件 (二期)
///
/// 功能: 注册 "interpreter.js" 脚本引擎, 承载加载/卸载 type: js 的脚本插件
///
/// 线程模型 (关键设计, 统一异步操作模型):
/// - 专用 JS 线程: 所有 QuickJS 操作集中于该线程 (QuickJS 非线程安全)
/// - 任务队列 (互斥锁 + 条件变量): post 非阻塞投递
/// - 工具 execute / 能力 load: 宿主 io 线程 start 入队 JS 线程后立即返回,
///   JS 线程执行完毕经 AgentxxPluginOperatorNotify 上报完成 (线程安全) —— 全程无阻塞
///   等待, 宿主 io 协程与 JS 任务交错执行; 旧版 postSync 阻塞桥已移除
/// - JS 线程 → io 线程: host vtable 内部经 post_to_io + 同步等待 (宿主实现)
/// - 钩子/事件回调: io 线程 → post 到 JS 线程 (fire-and-forget, 不等待)
/// - JS 内 callTool 命中本引擎工具: 同线程内联执行 (防自锁)
/// - 卸载安全: JsPluginCtx 由 shared_ptr 管理; 跨线程经 mirror 表 (互斥锁) 查
///   强引用; 插件卸载 (deleted) 后已入队任务检查标志跳过; JSContext 释放由
///   JsPluginCtx 析构完成 (全部进行中的任务结束后)
///
/// 沙箱: 内存限制 (JS_SetMemoryLimit) + 栈限制 + 指令中断超时; 不引入
/// quickjs-libc (无 os/std 模块); 全局仅注入标准 ECMA 内置 + agentxx 桥
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "fmt/format.h"
#include "quickjs.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class JsEngine;

namespace {

// =====================================================================
// C ABI 边界异常守卫 (由守卫函数调用处显式传入; entry 装配缓存)
// =====================================================================

} // namespace

namespace {

constexpr size_t kMemoryLimit   = 64 * 1024 * 1024; ///< JS 内存上限 64MB
constexpr size_t kStackLimit    = 512 * 1024;       ///< JS 栈上限 512KB
constexpr size_t kTaskTimeoutMs = 60000;            ///< 单任务 (工具执行等) 超时
constexpr int64_t kPromiseWaitLimitMs = 120000;    ///< Promise 等待上限 (绝对截止时间)

/// agentxx 桥方法 magic
enum BridgeMagic {
    B_REGISTER_TOOL = 1,
    B_UNREGISTER_TOOL,
    B_CALL_TOOL,
    B_GET_SHARE_STORE,
    B_EMIT_MESSAGE_TIP,
    B_LOG,
    B_REGISTER_HOOK,
    B_UNREGISTER_HOOK,
    B_SUBSCRIBE,
    B_UNSUBSCRIBE,
    B_PUBLISH,
    B_SET_TIMEOUT,
    B_CLEAR_TIMEOUT,
    B_LIST_PLUGINS,
    B_GET_PLUGIN,
    // 会话资源扩展 (宿主 plugin_api v8)
    B_ADD_SKILL_DIR,
    B_REMOVE_SKILL_DIR,
    B_ADD_MEMORY_FILE,
    B_REMOVE_MEMORY_FILE,
    B_ADD_MCP_SERVER,
    B_REMOVE_MCP_SERVER,
};

/// 工具执行请求/结果 (execute 桥跨线程传递)
struct ToolExecReq {
    std::string args, tid, tcid;
    std::string result, error;
    /// Promise 被拒绝或超时: 工具调用必须映射为 FAILED (不再当成成功文本)
    bool        failed    = false;
    /// 引擎停止/取消: 工具调用映射为 CANCELLED
    bool        cancelled = false;
    bool        done      = false;
};

/// 工具绑定 (注册工具时创建, 由 JsPluginCtx 持有; execute 回调期间存活
/// 由宿主 inflight 计数保证)
struct JsToolBinding {
    JsEngine*   engine = nullptr;
    std::string plugin; ///< 所属脚本插件名
    std::string name;
};

/// 钩子/事件绑定 (注册时创建, 由 JsPluginCtx 持有; 回调期间存活由宿主
/// inflight 计数保证)
struct JsHookBinding {
    JsEngine*   engine = nullptr;
    std::string plugin;
    int         point = -1; ///< 事件订阅时为 -1
};

/// JS 内 callTool 的跨线程完成桥:
/// - 宿主工具完成回调运行在宿主 io 线程, 只复制结果并向 JS 线程投递 settle 任务
/// - resolve/reject 均为 JS 线程所属的 JSValue, 只在 JS 线程 Free
/// - settled 原子标志保证 exactly-once (宿主协议已保证, 这里兜底)
struct JsCallBridge {
    JsEngine*         engine  = nullptr;
    JSContext*        ctx     = nullptr;
    JSValue           resolve = JS_UNDEFINED;
    JSValue           reject  = JS_UNDEFINED;
    std::atomic<bool> settled{false};
};

/// 脚本插件上下文 (生命周期: plugins_ / mirror / 进行中任务共享持有)
/// - 数据成员仅 JS 线程访问 (deleted/inflight 亦仅 JS 线程, 任务串行)
/// - JSContext 释放: 析构函数 (全部进行中的任务结束后由最后一个持有者析构)
struct JsPluginCtx {
    std::string              name;
    JSContext*               ctx      = nullptr;
    const AgentxxPluginHost* host     = nullptr; ///< 脚本插件宿主句柄
    JsEngine*                engine   = nullptr;
    bool                     deleted  = false; ///< 已卸载 (入队任务检查后跳过)
    size_t                   inflight = 0;     ///< 进行中的任务数 (execute/hook/event)
    /// 工具表: 普通对象 name -> {execute, name, description}
    JSValue tools = JS_UNDEFINED;
    /// 钩子表: Array(7) 元素为 fn 或 null
    JSValue hooks = JS_UNDEFINED;
    /// 订阅表: Array of {topic, handler, token}
    JSValue                                     agents = JS_UNDEFINED;
    std::vector<std::unique_ptr<JsToolBinding>> toolBindings;
    std::vector<std::unique_ptr<JsHookBinding>> hookBindings;
    /// 脚本初始化事务的撤销动作 (注册工具/钩子/订阅/资源成功时按序追加)。
    /// 顶层执行失败或脚本卸载时逆序执行, 宿主侧不留任何残留注册。
    std::vector<std::function<void()>> rollbackActions;

    /// 逆序执行并清空撤销动作; 单个动作失败只忽略 (注销本身幂等)。
    void runRollback() {
        for (auto it = rollbackActions.rbegin(); it != rollbackActions.rend(); ++it) {
            try {
                (*it)();
            } catch (...) {
            }
        }
        rollbackActions.clear();
    }

    ~JsPluginCtx();
};

} // namespace

// =====================================================================
// JsEngine
// =====================================================================

class JsEngine {
public:

    /// 引擎生命周期状态 (Reset-v1: create 只构造, runtime 与 JS 线程属于 start)
    enum class State {
        Stopped,  ///< 未启动或已完全停止 (无 JS 线程、无 runtime、无收尾线程)
        Running,  ///< 运行中: 接受 post
        Stopping, ///< 停止中: 拒绝新 post, 收尾线程正在 join JS 线程/释放 runtime
    };

    JsEngine() = default;

    /// 析构: 只处理已经安全停止 (或可以同步收尾) 的对象。
    /// 宿主协议要求 destroy 前 stop 已完成, 正常路径此处是空操作。
    ~JsEngine() { stopAndWait(); }

    // ==================== 生命周期 (start / stop) ====================

    /// 引擎是否运行中 (JS 线程存活, 接受新任务)
    bool running() const {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);
        return state_ == State::Running;
    }

    /// 引擎是否正在停止或已停止: 队列任务不再执行插件 JS 代码, 只按终态终结
    bool stopping() const {
        return !running();
    }

    /// 启动引擎: 创建 JSRuntime 与专用 JS 线程 (start 事务)
    /// - 幂等: 已运行时返回 true;
    /// - 上一次停止的收尾线程必须先结束 (宿主协议: stop 完成才会再次 start),
    ///   否则等待其退出后重建, 不共享已释放的 runtime/线程;
    /// - 失败 (runtime 创建失败) 返回 false 并保持停止状态, 可再次尝试。
    bool start() {
        joinStopper(); // 无锁等待: 收尾线程退出前不得重建 runtime/线程
        std::lock_guard<std::mutex> lk(lifecycleMtx_);
        if (state_ == State::Running) {
            return true;
        }
        rt_ = JS_NewRuntime();
        if (!rt_) {
            return false;
        }
        JS_SetMemoryLimit(rt_, kMemoryLimit);
        JS_SetMaxStackSize(rt_, kStackLimit);
        JS_SetInterruptHandler(rt_, &JsEngine::interruptHandler, this);
        taskStart_ = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> qlk(mtx_);
            queue_.clear(); ///< 停止时已全部终结, 启动前不应有残留
            busy_ = false;  ///< 上一次线程退出时已复位, 这里防御性归零
            stop_.store(false, std::memory_order_release);
        }
        state_  = State::Running;
        thread_ = std::thread(&JsEngine::jsThreadMain, this);
        return true;
    }

    /// 请求停止并异步等待收尾完成; `onStopped` 在停止真正完成后调用一次
    /// (可能来自收尾线程, 任意线程安全)。
    ///
    /// - 立即生效: 这是最后一次接受新任务之前的状态, 之后 post() 一律失败;
    /// - 队列中尚未开始的 JS 任务按取消/失败终结, 不再执行插件 JS 代码
    ///   (见各任务的 `stopping()` 前置检查);
    /// - 不在调用线程 join JS 线程: JS 线程可能正阻塞在宿主 vtable 调用
    ///   (ioCallSync 等待 IO 线程) 上, 而 stop 由 IO 线程调用, 直接 join 会
    ///   自锁; 因此 join 与 JS_FreeRuntime 交给独立收尾线程;
    /// - 幂等: 停止中重复调用只追加完成回调。
    void requestStop(std::function<void()> onStopped = nullptr) {
        std::vector<std::function<void()>> completed;
        bool                               spawnStopper = false;
        bool                               finishInline = false;
        {
            std::lock_guard<std::mutex> lk(lifecycleMtx_);
            if (onStopped) {
                stopCompletions_.push_back(std::move(onStopped));
            }
            if (state_ == State::Stopped) {
                // 已完全停止: 立即完成 (重复 stop 的快速路径)
                completed.swap(stopCompletions_);
            } else if (state_ == State::Running) {
                state_ = State::Stopping;
                {
                    std::lock_guard<std::mutex> qlk(mtx_);
                    stop_.store(true, std::memory_order_release); ///< post() 从这里起拒绝新任务
                    // JS 线程空闲 (没有在手中的任务、队列为空) 时, 停止标志生效后
                    // 它只会走线程退出清理路径 (不调用宿主 vtable), 因此可以在
                    // 调用线程直接收尾: 省一次线程切换, 也让 stop→start 紧邻的
                    // 生命周期事务保持确定顺序。
                    finishInline = !busy_ && queue_.empty();
                }
                cv_.notify_all();
                if (!finishInline) {
                    spawnStopper = true;
                }
            }
            // state_ == Stopping: 由收尾线程在下一轮完成回调中取走
        }
        if (finishInline) {
            finishStop(); ///< 空队列 join 立即返回; 完成后派发全部完成回调
            return;
        }
        if (spawnStopper) {
            try {
                std::lock_guard<std::mutex> lk(lifecycleMtx_);
                stopper_ = std::thread([this]() noexcept {
                    finishStop();
                });
            } catch (...) {
                // 辅助线程创建失败 (资源耗尽): 退化为调用线程同步收尾,
                // 保证已接受的 stop 仍然终结, 不静默悬挂。
                guardLog("js engine: stop helper thread creation failed, stopping inline");
                finishStop();
            }
        }
        for (auto& fn : completed) {
            try {
                fn();
            } catch (...) {
            }
        }
    }

    /// 同步停止 (destroy/析构安全网): 触发停止并等待收尾线程完成
    void stopAndWait() {
        requestStop(nullptr);
        joinStopper();
    }

    /// 能力 "interpreter.js" 是否已由本实例注册 (start 事务幂等判定)。
    /// 宿主在禁用/卸载时统一撤销注册, 引擎 stop 时同步清位。
    bool capabilityRegistered() const {
        return capabilityRegistered_;
    }

    void setCapabilityRegistered(bool registered) {
        capabilityRegistered_ = registered;
    }

    void setEngineHost(const AgentxxPluginHost* host) {
        engineHost_ = host;
        if (host && host->vtable && host->vtable->query_interface) {
            auto iidSv
                = agentxx::plugin::PluginStringView::fromCstr(AGENTXX_PLUGIN_IFACE_AGENT_LOG);
            logIface_ = static_cast<const AgentxxPluginLogIface*>(
                host->vtable->query_interface(host, &iidSv)
            );
        }
    }

    /// 守卫异常日志 (noexcept; 经本实例宿主接口表输出 —— 多实例契约:
    /// 不读任何全局, 日志归属精确到本引擎实例)
    void guardLog(const char* msg) const noexcept {
        agentxx::plugin::logTo(
            engineHost_,
            logIface_,
            4,
            "agentxx_javascript_engine",
            msg ? msg : ""
        );
    }

    /// 守卫日志闭包工厂 (捕获 this; 供静态回调内 guardCall 使用)
    auto guardLogger() const noexcept {
        return [this](const char* msg) noexcept {
            guardLog(msg);
        };
    }

    /// 引擎插件宿主句柄 (供静态桥回调读取; 可空)
    const AgentxxPluginHost* host() const {
        return engineHost_;
    }

    /// 能力 "load" 的活动 op 占位句柄 (每实例独立; 仅作不透明非空 token)
    void* capOpToken() { return &capOpToken_; }

private:

    /// 宿主 log 接口表缓存 (setEngineHost 装配; 随实例生死)
    const AgentxxPluginLogIface* logIface_ = nullptr;

public:

    /// JS 线程内加载脚本 (公共转发; 供能力方法异步任务在 JS 线程内直调)
    int loadScriptOnJsThread(
        const AgentxxPluginHost* host,
        const std::string&       name,
        const std::string&       path,
        const std::string&       code,
        std::string&             err
    ) {
        return doLoadScript(host, name, path, code, err);
    }

    // ==================== C 回调入口 (跨线程) ====================
    // (loadScriptInEngine 已移除: 能力 load 方法改为 JS 线程任务内直接调
    //  doLoadScript, 不再经 postSync 阻塞等待, 见 jsCapStart)

    /// 投递式卸载脚本 (不等待); 引擎停止时静默忽略
    void unloadScript(const char* script_name) {
        std::string name{script_name};
        post([this, name]() {
            doUnloadScript(name);
        });
    }

    /// 已加载脚本的工具名 JSON 数组 (【必须在 JS 线程调用】;
    /// 异步能力方法完成回调内直接使用)
    std::string loadedToolsJsonOnJsThread(const std::string& name) {
        auto pctx = findPlugin(name);
        if (!pctx || !pctx->ctx) {
            return "[]";
        }
        std::string out   = "[";
        bool        first = true;
        for (auto& k : jsToolsSnapshot(pctx.get())) {
            if (!first) {
                out += ",";
            }
            first  = false;
            out   += fmt::format("\"{}\"", k);
        }
        out += "]";
        return out;
    }

    /// 已加载脚本的工具名 JSON 数组 (任意线程; postSync 到 JS 线程)
    std::string loadedToolsJson(const std::string& name) {
        std::string out;
        if (!postSync([&]() {
                out = loadedToolsJsonOnJsThread(name);
            })) {
            return "[]"; // 引擎已停止
        }
        return out;
    }

    /// 【JS 线程】卸载脚本直调 (能力 unload 的 JS 线程任务内使用)
    void unloadScriptOnJsThread(const std::string& name) {
        doUnloadScript(name);
    }

    /// 宿主工具完成 (任意线程): 复制结果并投递回 JS 线程 settle Promise。
    /// - 完成回调由宿主在调用方 IO 线程派发, 调用期间 caller/provider lease
    ///   均被持有, 因此 engine 与其 JS 线程必然存活。
    /// - 引擎已停止时 post 失败: 不触碰 JSValue, 只释放桥 (JSContext 即将整体释放)。
    static void AGENTXX_PLUGIN_CALL onCallToolDone(
        void* ud, int32_t status, const AgentxxPluginStringView* payload
    ) noexcept {
        auto* bridge = static_cast<JsCallBridge*>(ud);
        if (!bridge || !bridge->engine) {
            return;
        }
        bool expected = false;
        if (!bridge->settled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        std::string text;
        try {
            if (payload && payload->data) {
                text.assign(payload->data, static_cast<size_t>(payload->size));
            }
        } catch (...) {
        }
        if (!bridge->engine->post([bridge, status, text = std::move(text)]() mutable {
                bridge->engine->settleCall(bridge, status, std::move(text));
            })) {
            delete bridge;
        }
    }

    /// 【JS 线程】settle callTool Promise: OK → resolve(JSON/字符串),
    /// CANCELLED/FAILED → reject(Error(message))
    void settleCall(JsCallBridge* bridge, int32_t status, std::string text) {
        if (!bridge) {
            return;
        }
        JSContext* ctx = bridge->ctx;
        if (status == AGENTXX_PLUGIN_OPERATOR_OK) {
            JSValue out = JS_ParseJSON(ctx, text.c_str(), text.size(), "<call_tool>");
            if (JS_IsException(out)) {
                JS_FreeValue(ctx, out);
                out = JS_NewStringLen(ctx, text.data(), text.size());
            }
            JSValue argv[1] = {out};
            JSValue rc      = JS_Call(ctx, bridge->resolve, JS_UNDEFINED, 1, argv);
            JS_FreeValue(ctx, rc);
            JS_FreeValue(ctx, out);
        } else {
            std::string msg = text;
            if (msg.empty()) {
                msg = status == AGENTXX_PLUGIN_OPERATOR_CANCELLED ? "call_tool cancelled"
                                                                  : "call_tool failed";
            }
            JSValue err = JS_NewError(ctx);
            JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg.c_str()));
            JSValue argv[1] = {err};
            JSValue rc      = JS_Call(ctx, bridge->reject, JS_UNDEFINED, 1, argv);
            JS_FreeValue(ctx, rc);
            JS_FreeValue(ctx, err);
        }
        JS_FreeValue(ctx, bridge->resolve);
        JS_FreeValue(ctx, bridge->reject);
        delete bridge;
    }

    /// 【JS 线程】立即拒绝 (宿主同步拒绝 call_tool_async 的路径)
    static void rejectCallNow(
        JSContext* ctx, JSValue resolve, JSValue reject, const std::string& msg
    ) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg.c_str()));
        JSValue argv[1] = {err};
        JSValue rc      = JS_Call(ctx, reject, JS_UNDEFINED, 1, argv);
        JS_FreeValue(ctx, rc);
        JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, resolve);
        JS_FreeValue(ctx, reject);
    }

    /// 【JS 线程】把 JS 调用结果 (值/Promise/异常) 链接到 resolve/reject。
    /// then() 调用后本函数释放调用方持有的 resolve/reject 引用。
    static void chainCallResult(JSContext* ctx, JSValue ret, JSValue resolve, JSValue reject) {
        if (JS_IsException(ret)) {
            JSValue exc     = JS_GetException(ctx);
            JSValue argv[1] = {exc};
            JSValue rc      = JS_Call(ctx, reject, JS_UNDEFINED, 1, argv);
            JS_FreeValue(ctx, rc);
            JS_FreeValue(ctx, exc);
        } else {
            JSValue thenFn = JS_GetPropertyStr(ctx, ret, "then");
            if (JS_IsFunction(ctx, thenFn)) {
                JSValue argv[2] = {resolve, reject};
                JSValue rc      = JS_Call(ctx, thenFn, ret, 2, argv);
                JS_FreeValue(ctx, rc);
            } else {
                JSValue argv[1] = {ret};
                JSValue rc      = JS_Call(ctx, resolve, JS_UNDEFINED, 1, argv);
                JS_FreeValue(ctx, rc);
            }
            JS_FreeValue(ctx, thenFn);
        }
        JS_FreeValue(ctx, resolve);
        JS_FreeValue(ctx, reject);
    }

    /// 工具 execute 桥 —— 异步启动 (统一异步操作模型):
    /// 【宿主 io 线程】调用; 任务入队 JS 线程后立即返回句柄,
    /// 完成结果由 JS 线程经 notifier 上报 (线程安全), io 线程零阻塞
    static void* AGENTXX_PLUGIN_CALL toolExecuteStart(
        void*                              ud,
        const AgentxxPluginStringView*     args_json,
        const AgentxxPluginStringView*     thread_id,
        const AgentxxPluginStringView*     tool_call_id,
        const AgentxxPluginOperatorNotify* notify,
        AgentxxPluginString*               error_out
    );

    /// 钩子回调桥 —— 异步启动 (统一异步操作模型):
    /// 【宿主 io 线程】调用; post 到 JS 线程 (fire-and-forget, 与旧语义一致)
    /// 后立即内联完成通知
    static void* AGENTXX_PLUGIN_CALL hookStart(
        void*                              ud,
        int32_t                            point,
        const AgentxxPluginStringView*     node_input_json,
        const AgentxxPluginOperatorNotify* notify,
        AgentxxPluginString*               error_out
    );

    /// 事件回调桥: io 线程调用; post 到 JS 线程 (fire-and-forget) (类外定义)
    static void AGENTXX_PLUGIN_CALL eventFire(const AgentxxPluginStringView* event_json, void* ud);

    // ==================== 任务队列 ====================

    /// 投递任务到 JS 线程 (非阻塞); 返回 false = 引擎已停止 (未入队)
    bool post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stop_.load(std::memory_order_acquire)) {
                return false;
            }
            queue_.push_back(std::move(fn));
        }
        cv_.notify_one();
        return true;
    }

    /// 投递任务到 JS 线程并阻塞等待执行完成; 返回 false = 引擎已停止
    /// (未执行, 输出参数保持不变, 由调用方置失败状态)
    bool postSync(std::function<void()> fn) {
        std::mutex              m;
        std::condition_variable cv;
        bool                    done = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stop_.load(std::memory_order_acquire)) {
                return false;
            }
            queue_.push_back([&]() {
                fn();
                {
                    std::lock_guard<std::mutex> lk2(m);
                    done = true;
                }
                cv.notify_one();
            });
        }
        cv_.notify_one();
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&]() {
            return done;
        });
        return true;
    }

    // ==================== 跨线程镜像 ====================

    /// 查找脚本插件上下文 (跨线程; 返回强引用保证存活)
    std::shared_ptr<JsPluginCtx> findPlugin(const std::string& name) {
        std::lock_guard<std::mutex> lk(mirrorMtx_);
        auto                        it = mirror_.find(name);
        if (it == mirror_.end()) {
            return nullptr;
        }
        return it->second.lock();
    }

private:

    friend struct JsPluginCtx;

    // ==================== 生命周期收尾 ====================

    /// 等待收尾线程退出 (先取出线程句柄再 join: 收尾线程结束前会短暂持
    /// lifecycleMtx_, 不能持锁等待, 否则自锁)
    void joinStopper() {
        std::thread t;
        {
            std::lock_guard<std::mutex> lk(lifecycleMtx_);
            if (stopper_.joinable()) {
                t = std::move(stopper_);
            }
        }
        if (t.joinable()) {
            t.join();
        }
    }

    /// 停止收尾 (由收尾线程调用; 极端情况下由 requestStop 的退化路径调用):
    /// 1) join JS 线程 —— 队列中未开始的任务在此执行 "按终态终结" 的收尾,
    ///    但不执行插件 JS 代码 (见各任务的 stopping() 检查);
    /// 2) 释放 JSRuntime (残余 JSContext 已由 JS 线程退出前清理);
    /// 3) 派发全部完成回调; 期间新追加的回调在下一轮取走。
    void finishStop() noexcept {
        if (thread_.joinable()) {
            thread_.join(); // 处理完已入队任务 (含进行中的 execute) 后退出
        }
        if (rt_) {
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
        }
        for (;;) {
            std::vector<std::function<void()>> completed;
            {
                std::lock_guard<std::mutex> lk(lifecycleMtx_);
                completed.swap(stopCompletions_);
                if (completed.empty()) {
                    // 状态切换与"取空"在同一临界区: 之后到达的 stop 请求走
                    // requestStop 的 Stopped 快速路径, 不会被漏掉。
                    state_ = State::Stopped;
                    return;
                }
            }
            for (auto& fn : completed) {
                try {
                    fn();
                } catch (...) {
                }
            }
        }
    }

    static void setErr(char** err_out, const AgentxxPluginHost* host, const char* msg) {
        if (err_out && host && msg) {
            auto sv  = agentxx::plugin::PluginStringView::fromCstr(msg);
            *err_out = agentxx::plugin::PluginString::strdup(host, &sv);
        }
    }

    // ==================== JS 线程主循环 ====================

    /// 最近定时器到期时间 (JS 线程成员, 需持锁调用)
    std::chrono::steady_clock::time_point nextTimerLocked() {
        auto next = std::chrono::steady_clock::time_point::max();
        for (const auto& [id, t] : timers_) {
            (void)id;
            if (t.due < next) {
                next = t.due;
            }
        }
        return next;
    }

    /// 执行到期定时器回调 (JS 线程直接执行, 不排队; 供任务循环与
    /// drivePromise 等待期间调用)
    /// 执行全部到期定时器; 返回实际执行的定时器数量 (0 = 没有到期项)。
    /// 调用方据此决定"立即回到主循环跑新产生的 job", 而不是等下一个定时器。
    size_t fireDueTimersInline() {
        size_t fired = 0;
        auto   now   = std::chrono::steady_clock::now();
        for (auto it = timers_.begin(); it != timers_.end();) {
            if (it->second.due > now) {
                ++it;
                continue;
            }
            ++fired;
            ++timerEpoch_;
            auto timer = std::move(it->second);
            it         = timers_.erase(it);
            auto pctx  = findPlugin(timer.plugin);
            if (!pctx || pctx->deleted || !pctx->ctx) {
                continue; // 上下文已销毁, fn 引用随 JsPluginCtx 释放
            }
            JSValue ret = JS_Call(pctx->ctx, timer.fn, JS_UNDEFINED, 0, nullptr);
            auto    outcome = drivePromise(pctx->ctx, ret);
            if (outcome.kind == PromiseOutcome::Kind::Rejected) {
                guardLog(
                    fmt::format(
                        "[interpreter.js] timer callback rejected: {}",
                        rejectedText(pctx->ctx, outcome)
                    ).c_str()
                );
            }
            JS_FreeValue(pctx->ctx, outcome.value);
            JS_FreeValue(pctx->ctx, ret);
            JS_FreeValue(pctx->ctx, timer.fn);
        }
        return fired;
    }

    void jsThreadMain() {
        // runtime 在 io 线程创建时 stack_top 记录的是 io 线程栈指针;
        // JS 线程栈地址不同, 必须在本线程更新 stack_top, 否则栈溢出检测误判
        JS_UpdateStackTop(rt_);
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                auto                         next = nextTimerLocked();
                if (next == std::chrono::steady_clock::time_point::max()) {
                    cv_.wait(lk, [&]() {
                        return stop_.load(std::memory_order_acquire) || !queue_.empty();
                    });
                } else {
                    cv_.wait_until(lk, next, [&]() {
                        return stop_.load(std::memory_order_acquire) || !queue_.empty();
                    });
                }
                // 退出条件仅看 stop_ + 队列: 未到期长定时器不再导致退出前忙循环
                // (定时器引用在下方 break 前统一释放)
                if (stop_.load(std::memory_order_acquire) && queue_.empty()) {
                    break;
                }
                if (queue_.empty()) {
                    // 仅定时器到期: 直接执行后继续等待。
                    // 执行定时器回调期间视为"忙": 期间可能调用宿主 vtable,
                    // 停止请求必须交给收尾线程 (见 requestStop)。
                    busy_ = true;
                    lk.unlock();
                    fireDueTimersInline();
                    lk.lock();
                    busy_ = false;
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
                // 任务在手中 (取任务与置忙在同一临界区内完成): 本线程此后可能
                // 调用宿主 vtable, 停止请求不得在调用线程直接 join。
                busy_ = true;
            }
            taskStart_ = std::chrono::steady_clock::now();
            try {
                task();
            } catch (...) {
                // 任务异常不得终止 JS 线程
            }
            {
                std::lock_guard<std::mutex> lk(mtx_);
                busy_ = false;
            }
        }
        // ---- 线程退出前清理 (JS 线程, 安全释放 JSValue) ----
        // 1. 残余定时器 (正常卸载路径 doUnloadScript 已清各插件定时器, 此处兜底)
        for (auto& [id, t] : timers_) {
            (void)id;
            auto pctx = findPlugin(t.plugin);
            if (pctx && pctx->ctx) {
                JS_FreeValue(pctx->ctx, t.fn);
            }
        }
        timers_.clear();
        // 2. 残余插件上下文 (正常路径引擎卸载前脚本插件已级联卸载; 兜底释放,
        //    保证 JS_FreeRuntime 前所有 JSContext 已 Free, 无 gc 断言)
        for (auto& [name, pctx] : plugins_) {
            (void)name;
            pctx->deleted = true;
            pctx->host    = nullptr;
        }
        plugins_.clear();
        {
            std::lock_guard<std::mutex> lk(mirrorMtx_);
            mirror_.clear();
        }
    }

    /// 中断处理: 任务超时打断 (返回 1 → 解释器抛 "interrupted")
    static int interruptHandler(JSRuntime* rt, void* opaque) {
        auto* engine = static_cast<JsEngine*>(opaque);
        (void)rt;
        if (std::chrono::steady_clock::now() - engine->taskStart_
            > std::chrono::milliseconds(kTaskTimeoutMs)) {
            return 1;
        }
        return 0;
    }

    // ==================== 插件加载/卸载 (JS 线程) ====================

    int doLoadScript(
        const AgentxxPluginHost* host,
        const std::string&       name,
        const std::string&       path,
        const std::string&       code,
        std::string&             err
    ) {
        if (plugins_.find(name) != plugins_.end()) {
            err = fmt::format("plugin already loaded: {}", name);
            return -1;
        }
        auto pctx    = std::make_shared<JsPluginCtx>();
        pctx->name   = name;
        pctx->host   = host;
        pctx->engine = this;
        pctx->ctx    = JS_NewContext(rt_);
        if (!pctx->ctx) {
            err = "JS_NewContext failed";
            return -1;
        }
        JS_SetContextOpaque(pctx->ctx, pctx.get());
        pctx->tools  = JS_NewObject(pctx->ctx);
        pctx->hooks  = JS_NewArray(pctx->ctx);
        pctx->agents = JS_NewArray(pctx->ctx);
        // 钩子数组预填 null
        for (int i = 0; i < 7; ++i) {
            JS_SetPropertyUint32(pctx->ctx, pctx->hooks, static_cast<uint32_t>(i), JS_NULL);
        }

        if (!injectBridge(pctx.get())) {
            err = "failed to inject agentxx bridge";
            rollbackScriptEffects(pctx);
            return -1;
        }

        // 执行插件脚本 (顶层可调用 agentxx.registerTool 等)
        JSValue ret = JS_Eval(pctx->ctx, code.c_str(), code.size(), path.c_str(), 0);
        if (JS_IsException(ret)) {
            err = extractException(pctx->ctx, ret);
            JS_FreeValue(pctx->ctx, ret);
            // F16: 脚本初始化是注册事务 —— 顶层异常时先撤销已产生的全部副作用
            // (工具/钩子/订阅/资源/定时器), 再释放 JSContext。
            rollbackScriptEffects(pctx);
            return -1;
        }
        JS_FreeValue(pctx->ctx, ret);

        plugins_[name] = pctx;
        {
            std::lock_guard<std::mutex> lk(mirrorMtx_);
            mirror_[name] = pctx;
        }
        return 0;
    }

    /// 撤销脚本产生的宿主注册与脚本定时器 (失败回滚与脚本卸载共用)。
    void rollbackScriptEffects(const std::shared_ptr<JsPluginCtx>& pctx) {
        if (!pctx) {
            return;
        }
        pctx->runRollback();
        if (!pctx->ctx) {
            return;
        }
        for (auto tit = timers_.begin(); tit != timers_.end();) {
            if (tit->second.plugin == pctx->name) {
                JS_FreeValue(pctx->ctx, tit->second.fn);
                tit = timers_.erase(tit);
            } else {
                ++tit;
            }
        }
    }

    void doUnloadScript(const std::string& name) {
        auto it = plugins_.find(name);
        if (it == plugins_.end()) {
            return;
        }
        auto pctx     = it->second;
        pctx->deleted = true;
        pctx->host    = nullptr; // 后续任务不得再调用宿主
        // 脚本卸载同样是注册事务的收尾: 注销该脚本注册的工具/钩子/订阅/资源,
        // 不留悬垂 user_data (宿主 detachAll 只覆盖引擎实例整体卸载的场景)。
        rollbackScriptEffects(pctx);
        plugins_.erase(it);
        {
            std::lock_guard<std::mutex> lk(mirrorMtx_);
            mirror_.erase(name);
        }
        // 全部进行中的任务结束后 shared_ptr 归零 → JsPluginCtx 析构 → JS_FreeContext
    }

    // ==================== 工具执行 (JS 线程) ====================

    void doToolExecute(JsToolBinding* binding, ToolExecReq& req) {
        auto pctx = findPlugin(binding->plugin);
        if (!pctx || pctx->deleted || !pctx->ctx) {
            req.error = "plugin unloaded";
            return;
        }
        pctx->inflight++;

        struct InflightGuard {
            JsPluginCtx* p;

            explicit InflightGuard(JsPluginCtx* pctx) :
                p(pctx) {}

            ~InflightGuard() {
                p->inflight--;
            }
        } guard(pctx.get());

        JSValue entry = JS_GetPropertyStr(pctx->ctx, pctx->tools, binding->name.c_str());
        if (!JS_IsObject(entry)) {
            req.error = fmt::format("tool not found in plugin: {}", binding->name);
            JS_FreeValue(pctx->ctx, entry);
            return;
        }
        JSValue execFn = JS_GetPropertyStr(pctx->ctx, entry, "execute");
        JS_FreeValue(pctx->ctx, entry);
        if (!JS_IsFunction(pctx->ctx, execFn)) {
            req.error = fmt::format("tool execute not a function: {}", binding->name);
            JS_FreeValue(pctx->ctx, execFn);
            return;
        }

        // 参数: args 对象 + ctx 对象 {session_id, tool_call_id}
        JSValue argsObj = JS_ParseJSON(pctx->ctx, req.args.c_str(), req.args.size(), "<args>");
        if (JS_IsException(argsObj)) {
            JS_FreeValue(pctx->ctx, argsObj);
            argsObj = JS_NewObject(pctx->ctx);
        }
        JSValue ctxObj = JS_NewObject(pctx->ctx);
        JS_SetPropertyStr(pctx->ctx, ctxObj, "sessionId", JS_NewString(pctx->ctx, req.tid.c_str()));
        JS_SetPropertyStr(
            pctx->ctx,
            ctxObj,
            "toolCallId",
            JS_NewString(pctx->ctx, req.tcid.c_str())
        );
        JSValue argv[2] = {argsObj, ctxObj};
        JSValue ret     = JS_Call(pctx->ctx, execFn, JS_UNDEFINED, 2, argv);
        JS_FreeValue(pctx->ctx, argsObj);
        JS_FreeValue(pctx->ctx, ctxObj);

        auto outcome = drivePromise(pctx->ctx, ret);
        JS_FreeValue(pctx->ctx, ret);
        switch (outcome.kind) {
            case PromiseOutcome::Kind::Value:
                req.result = valueToJsonString(pctx->ctx, outcome.value);
                break;
            case PromiseOutcome::Kind::Rejected:
                req.error = rejectedText(pctx->ctx, outcome);
                req.failed = true;
                break;
            case PromiseOutcome::Kind::Timeout:
                req.error = rejectedText(pctx->ctx, outcome);
                req.failed = true;
                break;
            case PromiseOutcome::Kind::Cancelled:
                req.error   = rejectedText(pctx->ctx, outcome);
                req.cancelled = true;
                break;
        }
        JS_FreeValue(pctx->ctx, outcome.value);
        JS_FreeValue(pctx->ctx, execFn);
    }

    // ==================== 钩子/事件 (JS 线程) ====================

    void doHookFire(
        JsHookBinding*                    binding,
        int                               point,
        const std::string&                payload,
        const AgentxxPluginOperatorNotify notify
    ) {
        bool        ok      = true;
        std::string errText;
        auto pctx = findPlugin(binding->plugin);
        if (!pctx || pctx->deleted || !pctx->ctx) {
            ok      = false;
            errText = "js plugin unloaded";
        } else {
            pctx->inflight++;

            struct InflightGuard {
                JsPluginCtx* p;

                explicit InflightGuard(JsPluginCtx* pctx) :
                    p(pctx) {}

                ~InflightGuard() {
                    p->inflight--;
                }
            } guard(pctx.get());

            JSValue fn = JS_GetPropertyUint32(
                pctx->ctx,
                pctx->hooks,
                static_cast<uint32_t>(point)
            );
            if (JS_IsFunction(pctx->ctx, fn)) {
                JSValue arg = JS_ParseJSON(pctx->ctx, payload.c_str(), payload.size(), "<hook>");
                if (JS_IsException(arg)) {
                    JS_FreeValue(pctx->ctx, arg);
                    arg = JS_NewString(pctx->ctx, payload.c_str());
                }
                JSValue ret     = JS_Call(pctx->ctx, fn, JS_UNDEFINED, 1, &arg);
                auto    outcome = drivePromise(pctx->ctx, ret);
                if (outcome.kind != PromiseOutcome::Kind::Value) {
                    ok      = false;
                    errText = rejectedText(pctx->ctx, outcome);
                    guardLog(fmt::format("[interpreter.js] hook callback failed: {}", errText)
                                 .c_str());
                }
                JS_FreeValue(pctx->ctx, outcome.value);
                JS_FreeValue(pctx->ctx, ret);
                JS_FreeValue(pctx->ctx, arg);
            }
            JS_FreeValue(pctx->ctx, fn);
        }
        // done 只在 JS 回调真正结束 (或确认插件已卸载) 之后触发:
        // 宿主 middleware 会等待该 op 的完成通知。
        if (notify.done) {
            auto errSv = agentxx::plugin::PluginStringView::from(errText.data(), errText.size());
            notify.done(
                notify.host_ud,
                ok ? AGENTXX_PLUGIN_OPERATOR_OK : AGENTXX_PLUGIN_OPERATOR_FAILED,
                ok ? nullptr : &errSv
            );
        }
    }

    void doEventFire(JsHookBinding* binding, const std::string& payload) {
        auto pctx = findPlugin(binding->plugin);
        if (!pctx || pctx->deleted || !pctx->ctx) {
            return;
        }
        pctx->inflight++;

        struct InflightGuard {
            JsPluginCtx* p;

            explicit InflightGuard(JsPluginCtx* pctx) :
                p(pctx) {}

            ~InflightGuard() {
                p->inflight--;
            }
        } guard(pctx.get());

        // 遍历订阅表, 触发全部 handler (payload 为事件 JSON 字符串)
        JSValue  lenVal = JS_GetPropertyStr(pctx->ctx, pctx->agents, "length");
        uint32_t len    = 0;
        JS_ToUint32(pctx->ctx, &len, lenVal);
        JS_FreeValue(pctx->ctx, lenVal);
        for (uint32_t i = 0; i < len; ++i) {
            JSValue entry = JS_GetPropertyUint32(pctx->ctx, pctx->agents, i);
            if (JS_IsObject(entry)) {
                JSValue handler = JS_GetPropertyStr(pctx->ctx, entry, "handler");
                if (JS_IsFunction(pctx->ctx, handler)) {
                    JSValue arg
                        = JS_ParseJSON(pctx->ctx, payload.c_str(), payload.size(), "<event>");
                    if (JS_IsException(arg)) {
                        JS_FreeValue(pctx->ctx, arg);
                        arg = JS_NewString(pctx->ctx, payload.c_str());
                    }
                    JSValue ret = JS_Call(pctx->ctx, handler, JS_UNDEFINED, 1, &arg);
                    auto    outcome = drivePromise(pctx->ctx, ret);
                    if (outcome.kind == PromiseOutcome::Kind::Rejected) {
                        guardLog(
                            fmt::format(
                                "[interpreter.js] event handler rejected: {}",
                                rejectedText(pctx->ctx, outcome)
                            ).c_str()
                        );
                    }
                    JS_FreeValue(pctx->ctx, outcome.value);
                    JS_FreeValue(pctx->ctx, ret);
                    JS_FreeValue(pctx->ctx, arg);
                }
                JS_FreeValue(pctx->ctx, handler);
            }
            JS_FreeValue(pctx->ctx, entry);
        }
    }

    // ==================== Promise 驱动 ====================

    /// Promise 驱动结果: 调用方必须据此映射 Operation 终态 ——
    /// 拒绝/超时是失败, 引擎停止是取消, 不能当成成功文本 (plugin.md 第 8.4 节)。
    struct PromiseOutcome {
        enum class Kind { Value, Rejected, Timeout, Cancelled };

        Kind        kind  = Kind::Value;
        JSValue     value = JS_UNDEFINED; ///< Value=结果值; Rejected=拒绝原因 (均拥有)
        std::string text;                 ///< Timeout/Cancelled 的说明文本
    };

    /// 驱动 Promise 直至 settle (JS 线程内调用)
    /// - 非 Promise/普通值原样 Dup 为 Value;
    /// - JS 异常值统一归一为 Rejected (拒绝原因已取出, 不再留在 context 上);
    /// - 用 steady_clock 绝对截止时间兜底 (interrupt handler 负责打断 CPU 长任务),
    ///   等待点是"队列任务/下一个定时器/截止时间"三者中最近的一个, 不忙轮询。
    PromiseOutcome drivePromise(JSContext* ctx, JSValue value) {
        PromiseOutcome out;
        if (JS_IsException(value)) {
            out.kind  = PromiseOutcome::Kind::Rejected;
            out.value = JS_GetException(ctx);
            return out;
        }
        if (!JS_IsPromise(value)) {
            out.kind  = PromiseOutcome::Kind::Value;
            out.value = JS_DupValue(ctx, value);
            return out;
        }
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(kPromiseWaitLimitMs);
        while (JS_PromiseState(ctx, value) == JS_PROMISE_PENDING) {
            if (stop_.load(std::memory_order_acquire)) {
                out.kind = PromiseOutcome::Kind::Cancelled;
                out.text = "interpreter.js engine stopped";
                return out;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                out.kind = PromiseOutcome::Kind::Timeout;
                out.text = fmt::format("promise not settled within {}ms", kPromiseWaitLimitMs);
                return out;
            }
            // 1) 先泵已入队的主队列任务 (工具并发调用等), 避免饥饿
            {
                std::function<void()> queued;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (!queue_.empty()) {
                        queued = std::move(queue_.front());
                        queue_.pop_front();
                    }
                }
                if (queued) {
                    try {
                        queued();
                    } catch (...) {
                    }
                    continue;
                }
            }
            // 2) 执行 QuickJS job (Promise continuation)
            JSContext* jobCtx = nullptr;
            int        rc     = JS_ExecutePendingJob(rt_, &jobCtx);
            if (rc < 0) {
                // job 抛出的异常同样按"拒绝"处理, 不再吞成普通字符串
                JSValue exc = jobCtx ? JS_GetException(jobCtx) : JS_UNDEFINED;
                out.kind    = PromiseOutcome::Kind::Rejected;
                out.value   = JS_IsUndefined(exc) ? JS_NewString(ctx, "pending job exception")
                                                  : exc;
                return out;
            }
            if (rc > 0) {
                continue;
            }
            // 3) 无 job: 执行到期定时器。执行过就先回到循环头重跑 job
            //    (定时器回调解决 Promise 后会产生新的 continuation job),
            //    否则等到"下一个定时器到期/截止时间/任务队列/定时器集合变化"。
            if (fireDueTimersInline() > 0) {
                continue;
            }
            auto                         next = nextTimerLocked();
            std::unique_lock<std::mutex> lk(mtx_);
            const uint64_t               epoch = timerEpoch_;
            auto                         wake = (next == std::chrono::steady_clock::time_point::max())
                                                   ? deadline
                                                   : std::min(next, deadline);
            cv_.wait_until(lk, wake, [&]() {
                return stop_.load(std::memory_order_acquire) || !queue_.empty()
                       || timerEpoch_ != epoch;
            });
        }
        if (JS_PromiseState(ctx, value) == JS_PROMISE_FULFILLED) {
            out.kind  = PromiseOutcome::Kind::Value;
            out.value = JS_PromiseResult(ctx, value); // 新引用, 调用方 Free
            return out;
        }
        out.kind  = PromiseOutcome::Kind::Rejected;
        out.value = JS_PromiseResult(ctx, value);
        return out;
    }

    /// 把 PromiseOutcome 的拒绝原因转成宿主错误文本 (调用方随后 Free outcome.value)
    std::string rejectedText(JSContext* ctx, PromiseOutcome& out) {
        if (out.kind == PromiseOutcome::Kind::Timeout
            || out.kind == PromiseOutcome::Kind::Cancelled) {
            return out.text;
        }
        if (JS_IsUndefined(out.value) || JS_IsNull(out.value)) {
            return "promise rejected";
        }
        // Error 对象的 message/name 是非枚举属性, JSON 序列化得到 "{}";
        // 常见拒绝原因就是 Error, 这里优先取 message。
        if (JS_IsObject(out.value)) {
            JSValue msgVal = JS_GetPropertyStr(ctx, out.value, "message");
            if (JS_IsString(msgVal)) {
                std::string msg = jsToCppString(ctx, msgVal);
                JS_FreeValue(ctx, msgVal);
                if (!msg.empty()) {
                    return fmt::format("promise rejected: {}", msg);
                }
            } else {
                JS_FreeValue(ctx, msgVal);
            }
        }
        return fmt::format("promise rejected: {}", valueToJsonString(ctx, out.value));
    }

    // ==================== 工具函数 ====================

    std::string extractException(JSContext* ctx, JSValue exc) {
        (void)exc;
        JSValue     e = JS_GetException(ctx);
        std::string s = valueToJsonString(ctx, e);
        JS_FreeValue(ctx, e);
        return s;
    }

    /// 值 → 字符串 (对象 JSON 序列化; 字符串原样; 其他 String() 转换)
    std::string valueToJsonString(JSContext* ctx, JSValueConst v) {
        if (JS_IsString(v) || JS_IsUndefined(v) || JS_IsNull(v) || JS_IsBool(v) || JS_IsNumber(v)) {
            const char* s   = JS_ToCString(ctx, v);
            std::string out = s ? s : "";
            JS_FreeCString(ctx, s);
            return out;
        }
        JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        if (JS_IsException(json)) {
            JS_FreeValue(ctx, json);
            const char* s   = JS_ToCString(ctx, v);
            std::string out = s ? s : "";
            JS_FreeCString(ctx, s);
            return out;
        }
        const char* s   = JS_ToCString(ctx, json);
        std::string out = s ? s : "";
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, json);
        return out;
    }

    static std::string jsToCppString(JSContext* ctx, JSValueConst v) {
        const char* s   = JS_ToCString(ctx, v);
        std::string out = s ? s : "";
        JS_FreeCString(ctx, s);
        return out;
    }

    // ==================== 桥注入 ====================

    bool injectBridge(JsPluginCtx* pctx) {
        JSContext* ctx    = pctx->ctx;
        JSValue    global = JS_GetGlobalObject(ctx);
        JSValue    bridge = JS_NewObject(ctx);

        auto def = [&](const char* name, int magic, int nargs) {
            // 注意: JS_SetProperty* 为 move 语义 (处理传入值), 不得再 Free
            JSValue fn = JS_NewCFunction2(
                ctx,
                reinterpret_cast<JSCFunction*>(&JsEngine::bridgeCall),
                name,
                nargs,
                JS_CFUNC_generic_magic,
                magic
            );
            JS_SetPropertyStr(ctx, bridge, name, fn);
        };
        def("registerTool", B_REGISTER_TOOL, 1);
        def("unregisterTool", B_UNREGISTER_TOOL, 1);
        def("callTool", B_CALL_TOOL, 3);
        def("getShareStore", B_GET_SHARE_STORE, 2);
        def("emitMessageTip", B_EMIT_MESSAGE_TIP, 3);
        def("log", B_LOG, 2);
        def("onHook", B_REGISTER_HOOK, 2);
        def("offHook", B_UNREGISTER_HOOK, 2);
        def("subscribe", B_SUBSCRIBE, 2);
        def("unsubscribe", B_UNSUBSCRIBE, 1);
        def("publish", B_PUBLISH, 2);
        def("setTimeout", B_SET_TIMEOUT, 2);
        def("clearTimeout", B_CLEAR_TIMEOUT, 1);
        def("listPlugins", B_LIST_PLUGINS, 0);
        def("getPlugin", B_GET_PLUGIN, 1);
        // 会话资源扩展 (宿主 plugin_api v8): Skill/Memory/MCP 贡献
        def("addSkillDir", B_ADD_SKILL_DIR, 1);
        def("removeSkillDir", B_REMOVE_SKILL_DIR, 1);
        def("addMemoryFile", B_ADD_MEMORY_FILE, 1);
        def("removeMemoryFile", B_REMOVE_MEMORY_FILE, 1);
        def("addMcpServer", B_ADD_MCP_SERVER, 1);
        def("removeMcpServer", B_REMOVE_MCP_SERVER, 1);

        int ok = JS_SetPropertyStr(ctx, global, "agentxx", bridge) >= 0;
        // 常用工具函数注入全局 (沙箱内可用): 裸 setTimeout/clearTimeout
        if (ok) {
            JSValue fn = JS_NewCFunction2(
                ctx,
                reinterpret_cast<JSCFunction*>(&JsEngine::bridgeCall),
                "setTimeout",
                2,
                JS_CFUNC_generic_magic,
                B_SET_TIMEOUT
            );
            JS_SetPropertyStr(ctx, global, "setTimeout", fn);
            fn = JS_NewCFunction2(
                ctx,
                reinterpret_cast<JSCFunction*>(&JsEngine::bridgeCall),
                "clearTimeout",
                1,
                JS_CFUNC_generic_magic,
                B_CLEAR_TIMEOUT
            );
            JS_SetPropertyStr(ctx, global, "clearTimeout", fn);
        }
        JS_FreeValue(ctx, global);
        return ok != 0;
    }

    /// agentxx.* 桥 C 函数 (magic 分派; ctx opaque = JsPluginCtx*)
    static JSValue
        bridgeCall(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

    // ==================== 成员 ====================

    JSRuntime*                        rt_         = nullptr;
    const AgentxxPluginHost*          engineHost_ = nullptr;
    std::thread                       thread_;
    std::mutex                        mtx_;
    std::condition_variable           cv_;
    std::deque<std::function<void()>> queue_;
    /// true = 拒绝新任务 (mtx_ 保护写入; JS 线程经原子读取观察停止标志)
    std::atomic<bool> stop_{false};
    /// JS 线程"忙"标记 (mtx_ 保护): 取任务与置忙在同一临界区内完成。
    /// 停止请求据此判断能否在调用线程直接 join JS 线程 (空闲时可).
    bool busy_ = false;

    /// 能力 "interpreter.js" 注册标记 (start/stop 事务幂等; 仅在所属 IO 线程访问)
    bool capabilityRegistered_ = false;

    /// 生命周期互斥量: 只保护引擎状态与停止完成回调 (不与 mtx_ 组成嵌套
    /// 死锁: 需要同时持有时一律先 lifecycleMtx_ 再 mtx_)。
    mutable std::mutex                 lifecycleMtx_;
    State                              state_ = State::Stopped;
    std::thread                        stopper_;  ///< 停止收尾线程 (join JS 线程 + 释放 runtime)
    std::vector<std::function<void()>> stopCompletions_; ///< 停止完成回调 (可能多个: 重复 stop)

    std::chrono::steady_clock::time_point taskStart_;

    /// 能力 "load" 的活动 op 占位 token (宿主只当不透明非空句柄; 每实例独立)
    int capOpToken_ = 0;

    /// 定时器集合版本号: 注册/清除/执行都会递增, 作为等待条件的唤醒依据
    /// (只做"是否变化"判断, 不承载业务语义)
    uint64_t timerEpoch_ = 0;

    /// 定时器 (setTimeout; JS 线程访问; fn 为 Dup 引用, 到期执行后 Free)
    struct JsTimer {
        std::chrono::steady_clock::time_point due;
        std::string                           plugin;
        JSValue                               fn;
    };

    std::map<uint64_t, JsTimer> timers_;
    uint64_t                    timerSeq_ = 0;

    /// 工具表属性名快照 (JS 线程; tools 为普通对象 name -> entry)
    std::vector<std::string> jsToolsSnapshot(JsPluginCtx* pctx) {
        std::vector<std::string> out;
        JSPropertyEnum*          props = nullptr;
        uint32_t                 n     = 0;
        if (JS_GetOwnPropertyNames(
                pctx->ctx,
                &props,
                &n,
                pctx->tools,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY
            )
            == 0) {
            for (uint32_t i = 0; i < n; ++i) {
                const char* k = JS_AtomToCString(pctx->ctx, props[i].atom);
                if (k) {
                    out.emplace_back(k);
                    JS_FreeCString(pctx->ctx, k);
                }
            }
            js_free_rt(JS_GetRuntime(pctx->ctx), props);
        }
        return out;
    }

    /// 脚本插件表 (JS 线程访问)
    std::map<std::string, std::shared_ptr<JsPluginCtx>, std::less<>> plugins_;
    /// 跨线程镜像 <name, weak_ptr> (互斥锁保护)
    std::mutex                                                     mirrorMtx_;
    std::map<std::string, std::weak_ptr<JsPluginCtx>, std::less<>> mirror_;
};

/// 工具 execute 异步桥 (统一异步操作模型):
/// - start: io 线程调用 —— 打包请求入队 JS 线程, 立即返回 op 句柄
///   (execute_poll 留 NULL, 宿主只等完成通知)
/// - JS 线程执行完毕后经 notifier 上报结果 (线程安全; payload host->alloc)
void* JsEngine::toolExecuteStart(
    void*                              ud,
    const AgentxxPluginStringView*     args_json,
    const AgentxxPluginStringView*     thread_id,
    const AgentxxPluginStringView*     tool_call_id,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
) {
    // C ABI 回调异常守卫: start 由宿主 io 线程调用 (请求打包/入队含分配),
    // 异常经通知器上报 OP_FAILED, 不外泄
    auto* binding = static_cast<JsToolBinding*>(ud);
    auto* engine = binding ? binding->engine : nullptr; ///< 提升至 try 外 (catch 日志闭包使用)
    try {
        if (!engine || !notify) {
            return nullptr;
        }
        auto req  = std::make_shared<ToolExecReq>();
        req->args = std::string{
            args_json && args_json->data ? args_json->data : "{}",
            args_json ? static_cast<size_t>(args_json->size) : 0
        };
        req->tid = std::string{
            thread_id && thread_id->data ? thread_id->data : "",
            thread_id ? static_cast<size_t>(thread_id->size) : 0
        };
        req->tcid = std::string{
            tool_call_id && tool_call_id->data ? tool_call_id->data : "",
            tool_call_id ? static_cast<size_t>(tool_call_id->size) : 0
        };

        // op 句柄仅作占位标识 (宿主不解释其内容): 所有权随任务闭包移交 JS 线程,
        // 通知完成后由闭包自身释放 —— 支持同一引擎插件被多实例加载/反复加载
        // (此前成功路径 new 后无人 delete, 每次工具执行泄漏句柄)
        auto*                       op      = new int(0);
        AgentxxPluginOperatorNotify ntfCopy = *notify;
        if (!engine->post([engine, binding, req, ntfCopy, op]() {
                // ---- JS 线程: 执行并上报 (RAII 兜底释放句柄) ----
                struct OpReleaser {
                    int* p;
                    ~OpReleaser() {
                        delete p;
                    }
                } releaser{op};
                if (engine->stopping()) {
                    // 停止中: 不再执行插件 JS 代码, 但已接受的 op 必须终结
                    auto errSv = agentxx::plugin::PluginStringView::fromCstr(
                        "interpreter.js engine stopped"
                    );
                    ntfCopy.done(ntfCopy.host_ud, AGENTXX_PLUGIN_OPERATOR_CANCELLED, &errSv);
                    return;
                }
                engine->doToolExecute(binding, *req);
                if (req->cancelled) {
                    // 引擎停止/取消: 终态为 CANCELLED, 不是普通失败
                    auto errSv = agentxx::plugin::PluginStringView::from(
                        req->error.data(),
                        req->error.size()
                    );
                    ntfCopy.done(ntfCopy.host_ud, AGENTXX_PLUGIN_OPERATOR_CANCELLED, &errSv);
                } else if (!req->error.empty()) {
                    auto errSv = agentxx::plugin::PluginStringView::from(
                        req->error.data(),
                        req->error.size()
                    );
                    ntfCopy.done(ntfCopy.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
                } else {
                    auto resSv = agentxx::plugin::PluginStringView::from(
                        req->result.data(),
                        req->result.size()
                    );
                    ntfCopy.done(ntfCopy.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, &resSv);
                }
            })) {
            // 引擎已停止: 启动失败 (未入队 → 句柄由本函数直接释放)
            delete op;
            if (error_out && engine->engineHost_) {
                *error_out = agentxx::plugin::PluginString::fromCstr(
                    engine->engineHost_,
                    "interpreter.js engine stopped"
                );
            }
            return nullptr;
        }
        return op;
    } catch (...) {
        // 异常分类上报 + 经通知器上报失败 (宿主 io 线程等通知, 必须终结)
        ::agentxx::plugin::reportCurrentException([engine](const char* m) noexcept {
            engine->guardLog(m);
        });
        if (error_out) {
            error_out->data = nullptr;
            error_out->size = 0;
        }
        if (notify && notify->done) {
            auto errSv = agentxx::plugin::PluginStringView::from(nullptr, 0);
            notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
        }
        return nullptr;
    }
}

void* JsEngine::hookStart(
    void*                              ud,
    int32_t                            point,
    const AgentxxPluginStringView*     node_input_json,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
) {
    (void)error_out;
    // C ABI 回调异常守卫: start 由宿主 io 线程调用, 异常经通知器上报
    auto* binding = static_cast<JsHookBinding*>(ud);
    auto* engine = binding ? binding->engine : nullptr; ///< 提升至 try 外 (catch 日志闭包使用)
    try {
        if (!engine || !notify) {
            return nullptr;
        }
        std::string payload{
            node_input_json && node_input_json->data ? node_input_json->data : "",
            node_input_json ? static_cast<size_t>(node_input_json->size) : 0
        };
        int pt               = static_cast<int>(point);
        auto ntfCopy         = *notify;
        if (!engine->post([engine, binding, payload, pt, ntfCopy]() {
                if (engine->stopping()) {
                    // 停止中: 不执行插件 JS 回调, 仍按失败终结本次 hook op
                    auto errSv
                        = agentxx::plugin::PluginStringView::fromCstr("interpreter.js engine stopped");
                    ntfCopy.done(ntfCopy.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
                    return;
                }
                engine->doHookFire(binding, pt, payload, ntfCopy);
            })) {
            // 引擎已停止: 已接受的 op 必须终结 (失败), 不返回悬挂句柄
            auto errSv = agentxx::plugin::PluginStringView::from(nullptr, 0);
            notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
            return nullptr;
        }
        // 已接受: 返回非空 provider 句柄 (binding 由脚本上下文持有到卸载;
        // 宿主 detachAll 时的 provider lease 保证其存活), 完成通知在 JS 线程
        // 于回调真正结束后触发 (见 doHookFire)。
        return binding;
    } catch (...) {
        ::agentxx::plugin::reportCurrentException([engine](const char* m) noexcept {
            if (engine) {
                engine->guardLog(m);
            }
        });
        if (notify && notify->done) {
            auto errSv = agentxx::plugin::PluginStringView::from(nullptr, 0);
            notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
        }
        return nullptr;
    }
}

void JsEngine::eventFire(const AgentxxPluginStringView* event_json, void* ud) {
    auto* binding = static_cast<JsHookBinding*>(ud);
    auto* engine  = binding ? binding->engine : nullptr;
    // C ABI 回调异常守卫: 事件分发由宿主 io 线程调用, 异常不外泄
    agentxx::plugin::guardCallVoid(
        [engine](const char* m) noexcept {
            if (engine) {
                engine->guardLog(m);
            }
        },
        [&] {
            if (!engine) {
                return;
            }
            std::string payload{
                event_json && event_json->data ? event_json->data : "",
                event_json ? static_cast<size_t>(event_json->size) : 0
            };
            engine->post([engine, binding, payload]() {
                if (engine->stopping()) {
                    return; ///< 事件为 fire-and-forget: 停止中不再执行插件 JS
                }
                engine->doEventFire(binding, payload);
            });
        }
    );
}

namespace {

JsPluginCtx::~JsPluginCtx() {
    if (ctx) {
        if (!JS_IsUndefined(tools)) {
            JS_FreeValue(ctx, tools);
        }
        if (!JS_IsUndefined(hooks)) {
            JS_FreeValue(ctx, hooks);
        }
        if (!JS_IsUndefined(agents)) {
            JS_FreeValue(ctx, agents);
        }
        JS_FreeContext(ctx);
        ctx = nullptr;
    }
}

static JsPluginCtx* pluginCtxOf(JSContext* ctx) {
    return static_cast<JsPluginCtx*>(JS_GetContextOpaque(ctx));
}

/// 抛 JS 异常 (字符串消息)
static JSValue throwJsError(JSContext* ctx, const std::string& msg) {
    return JS_Throw(ctx, JS_NewString(ctx, msg.c_str()));
}

} // namespace

// =====================================================================
// agentxx 桥实现 (C 函数, magic 分派)
// =====================================================================

JSValue JsEngine::bridgeCall(
    JSContext*    ctx,
    JSValueConst  this_val,
    int           argc,
    JSValueConst* argv,
    int           magic
) {
    (void)this_val;
    auto* pctx = pluginCtxOf(ctx);
    if (!pctx || !pctx->host || !pctx->engine) {
        return JS_ThrowInternalError(ctx, "agentxx bridge: plugin context invalid");
    }
    const AgentxxPluginHost* host = pctx->host;
    const auto&              vt   = *host->vtable; // 核心: alloc/free/strdup
    (void)vt;
    // COM 风格接口表查询 (进程级静态数据; 各能力经稳定 IID 分发)
    const agentxx::plugin::AgentIfaces iface  = agentxx::plugin::AgentIfaces::query(host);
    auto*                              engine = pctx->engine;

    switch (magic) {
        case B_REGISTER_TOOL: {
            if (argc < 1 || !JS_IsObject(argv[0])) {
                return JS_ThrowTypeError(ctx, "registerTool: spec object required");
            }
            auto        specObj = argv[0];
            std::string name    = jsToCppString(ctx, JS_GetPropertyStr(ctx, specObj, "name"));
            std::string desc   = jsToCppString(ctx, JS_GetPropertyStr(ctx, specObj, "description"));
            JSValue     params = JS_GetPropertyStr(ctx, specObj, "parameters");
            std::string paramsJson = "{}";
            if (JS_IsObject(params)) {
                JSValue json = JS_JSONStringify(ctx, params, JS_UNDEFINED, JS_UNDEFINED);
                if (!JS_IsException(json)) {
                    const char* s = JS_ToCString(ctx, json);
                    if (s) {
                        paramsJson = s;
                        JS_FreeCString(ctx, s);
                    }
                    JS_FreeValue(ctx, json);
                } else {
                    JS_FreeValue(ctx, json);
                }
            }
            JS_FreeValue(ctx, params);
            JSValue execFn = JS_GetPropertyStr(ctx, specObj, "execute");
            if (name.empty() || !JS_IsFunction(ctx, execFn)) {
                JS_FreeValue(ctx, execFn);
                return JS_ThrowTypeError(ctx, "registerTool: name and execute function required");
            }

            // 工具绑定 (宿主 execute 回调 → 本插件)
            auto binding    = std::make_unique<JsToolBinding>();
            binding->engine = engine;
            binding->plugin = pctx->name;
            binding->name   = name;

            AgentxxPluginToolSpec spec{};
            spec.name        = agentxx::plugin::PluginStringView::from(name.data(), name.size());
            spec.description = agentxx::plugin::PluginStringView::from(desc.data(), desc.size());
            spec.parameters_json
                = agentxx::plugin::PluginStringView::from(paramsJson.data(), paramsJson.size());
            // 统一异步操作模型: 异步桥 (JS 线程完成时经通知器上报)
            spec.execute_start  = &JsEngine::toolExecuteStart;
            spec.execute_cancel = nullptr;
            spec.user_data      = binding.get();
            int rc              = iface.tools->register_tool(host, &spec);
            if (rc != 0) {
                JS_FreeValue(ctx, execFn);
                return throwJsError(
                    ctx,
                    fmt::format("registerTool: host registration failed (conflict?): {}", name)
                );
            }

            // JS 侧工具表登记 (供 JS 内 callTool 内联执行)
            JSValue entry = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, entry, "execute", JS_DupValue(ctx, execFn));
            JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, name.c_str()));
            JS_SetPropertyStr(ctx, entry, "description", JS_NewString(ctx, desc.c_str()));
            JS_SetPropertyStr(ctx, pctx->tools, name.c_str(), entry);
            JS_FreeValue(ctx, execFn);

            // 脚本初始化事务: 注册成功登记撤销动作 (顶层失败/脚本卸载时回滚)
            pctx->rollbackActions.push_back([host, toolsIface = iface.tools, name]() {
                auto nameSv = agentxx::plugin::PluginStringView::from(name.data(), name.size());
                toolsIface->unregister_tool(host, &nameSv);
            });
            pctx->toolBindings.push_back(std::move(binding));
            return JS_UNDEFINED;
        }

        case B_UNREGISTER_TOOL: {
            std::string name = argc >= 1 ? jsToCppString(ctx, argv[0]) : "";
            if (name.empty()) {
                return JS_ThrowTypeError(ctx, "unregisterTool: name required");
            }
            auto nameSv = agentxx::plugin::PluginStringView::from(name.data(), name.size());
            iface.tools->unregister_tool(host, &nameSv);
            JS_SetPropertyStr(ctx, pctx->tools, name.c_str(), JS_UNDEFINED);
            return JS_UNDEFINED;
        }

        case B_CALL_TOOL: {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "callTool: name required");
            }
            std::string name     = jsToCppString(ctx, argv[0]);
            std::string argsJson = "{}";
            if (argc >= 2) {
                if (JS_IsString(argv[1])) {
                    argsJson = jsToCppString(ctx, argv[1]);
                } else if (JS_IsObject(argv[1])) {
                    JSValue json = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
                    if (!JS_IsException(json)) {
                        argsJson = jsToCppString(ctx, json);
                        JS_FreeValue(ctx, json);
                    } else {
                        JS_FreeValue(ctx, json);
                    }
                }
            }
            std::string sessionId = argc >= 3 ? jsToCppString(ctx, argv[2]) : "";

            // F17: callTool 始终返回 Promise, 完成事件回到 JS 线程 settle。
            // - 本引擎 JS 工具: 同线程执行并把结果/内部 Promise 链到外层 Promise
            //   (不阻塞等待, 内部 Promise 由外层驱动的 job/timer/队列泵推进)
            // - 宿主插件工具: call_tool_async + 完成回调投递回 JS 线程 (不再
            //   call_tool_blocking 阻塞 JS 线程, 消除 A/B 脚本互调自锁)
            JSValue resolving[2];
            JSValue promise = JS_NewPromiseCapability(ctx, resolving);
            if (JS_IsException(promise)) {
                return promise;
            }

            // 1) 本引擎 JS 工具: 同线程内联执行 (防自锁)
            JSValue entry = JS_GetPropertyStr(ctx, pctx->tools, name.c_str());
            if (JS_IsObject(entry)) {
                JSValue execFn = JS_GetPropertyStr(ctx, entry, "execute");
                JS_FreeValue(ctx, entry);
                if (JS_IsFunction(ctx, execFn)) {
                    JSValue argsObj
                        = JS_ParseJSON(ctx, argsJson.c_str(), argsJson.size(), "<args>");
                    if (JS_IsException(argsObj)) {
                        JS_FreeValue(ctx, argsObj);
                        argsObj = JS_NewObject(ctx);
                    }
                    JSValue ctxObj = JS_NewObject(ctx);
                    JS_SetPropertyStr(
                        ctx,
                        ctxObj,
                        "sessionId",
                        JS_NewString(ctx, sessionId.c_str())
                    );
                    JS_SetPropertyStr(ctx, ctxObj, "toolCallId", JS_NewString(ctx, "js_call"));
                    JSValue argv2[2] = {argsObj, ctxObj};
                    JSValue ret      = JS_Call(ctx, execFn, JS_UNDEFINED, 2, argv2);
                    JS_FreeValue(ctx, argsObj);
                    JS_FreeValue(ctx, ctxObj);
                    JsEngine::chainCallResult(ctx, ret, resolving[0], resolving[1]);
                    JS_FreeValue(ctx, ret);
                    JS_FreeValue(ctx, execFn);
                    return promise;
                }
                JS_FreeValue(ctx, execFn);
                JsEngine::rejectCallNow(
                    ctx,
                    resolving[0],
                    resolving[1],
                    fmt::format("callTool: execute not a function: {}", name)
                );
                return promise;
            }
            JS_FreeValue(ctx, entry);

            // 2) 宿主插件工具: 异步互调, 完成回调经 JS 线程 settle
            auto* bridge        = new JsCallBridge{engine, ctx, resolving[0], resolving[1]};
            auto  nameSv        = agentxx::plugin::PluginStringView::from(name.data(), name.size());
            auto  argsSv        = agentxx::plugin::PluginStringView::from(argsJson.data(), argsJson.size());
            auto  sidSv         = agentxx::plugin::PluginStringView::from(sessionId.data(), sessionId.size());
            AgentxxPluginString err{nullptr, 0};
            auto*               handle = iface.tools->call_tool_async(
                host,
                &nameSv,
                &argsSv,
                &sidSv,
                &JsEngine::onCallToolDone,
                bridge,
                &err
            );
            if (!handle) {
                std::string errStr
                    = err.data ? std::string(err.data, err.size) : "call_tool failed";
                if (err.data) {
                    agentxx::plugin::PluginString::free(host, &err);
                }
                bridge->settled.store(true, std::memory_order_release);
                JsEngine::rejectCallNow(ctx, bridge->resolve, bridge->reject, errStr);
                delete bridge;
                return promise;
            }
            return promise;
        }

        case B_GET_SHARE_STORE: {
            std::string sessionId = argc >= 1 ? jsToCppString(ctx, argv[0]) : "";
            int64_t     id        = 0;
            if (argc >= 2) {
                JS_ToInt64(ctx, &id, argv[1]);
            }
            AgentxxPluginString resp{nullptr, 0};
            auto                sidSv
                = agentxx::plugin::PluginStringView::from(sessionId.data(), sessionId.size());
            iface.session->get_share_store(host, &sidSv, id, &resp);
            if (!resp.data) {
                return JS_NULL;
            }
            JSValue out = JS_NewStringLen(ctx, resp.data, static_cast<size_t>(resp.size));
            agentxx::plugin::PluginString::free(host, &resp);
            return out;
        }

        case B_EMIT_MESSAGE_TIP: {
            std::string sessionId = argc >= 1 ? jsToCppString(ctx, argv[0]) : "";
            std::string text      = argc >= 2 ? jsToCppString(ctx, argv[1]) : "";
            int         level     = 0;
            if (argc >= 3 && JS_IsNumber(argv[2])) {
                int32_t lv = 0;
                JS_ToInt32(ctx, &lv, argv[2]);
                level = lv;
            }
            auto sidSv
                = agentxx::plugin::PluginStringView::from(sessionId.data(), sessionId.size());
            auto txtSv = agentxx::plugin::PluginStringView::from(text.data(), text.size());
            iface.session->emit_message_tip(host, &sidSv, &txtSv, level);
            return JS_UNDEFINED;
        }

        case B_LOG: {
            int level = 2;
            if (argc >= 1 && JS_IsNumber(argv[0])) {
                int32_t lv = 0;
                JS_ToInt32(ctx, &lv, argv[0]);
                level = lv;
            }
            std::string msg   = argc >= 2 ? jsToCppString(ctx, argv[1]) : "";
            auto        msgSv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
            iface.log->log(host, level, &msgSv);
            return JS_UNDEFINED;
        }

        case B_REGISTER_HOOK: {
            if (argc < 2 || !JS_IsNumber(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
                return JS_ThrowTypeError(ctx, "onHook: (point, fn) required");
            }
            int32_t point = 0;
            JS_ToInt32(ctx, &point, argv[0]);
            if (point < 0 || point >= 7) {
                return JS_ThrowTypeError(ctx, "onHook: point out of range");
            }
            // 覆盖语义: 注销同点旧钩子 (每插件每钩子点至多一个)
            JSValue oldFn = JS_GetPropertyUint32(ctx, pctx->hooks, static_cast<uint32_t>(point));
            if (JS_IsFunction(ctx, oldFn)) {
                iface.hooks->unregister_hook(host, static_cast<AgentxxPluginHookPoint>(point));
            }
            JS_FreeValue(ctx, oldFn);

            auto binding    = std::make_unique<JsHookBinding>();
            binding->engine = engine;
            binding->plugin = pctx->name;
            binding->point  = point;
            AgentxxPluginHookSpec hspec{};
            hspec.point       = static_cast<AgentxxPluginHookPoint>(point);
            hspec.hook_start  = &JsEngine::hookStart;
            hspec.hook_cancel = nullptr;
            hspec.user_data   = binding.get();
            int rc            = iface.hooks->register_hook(host, &hspec);
            if (rc != 0) {
                return throwJsError(ctx, "onHook: host registration failed");
            }
            JS_SetPropertyUint32(
                ctx,
                pctx->hooks,
                static_cast<uint32_t>(point),
                JS_DupValue(ctx, argv[1])
            );
            pctx->rollbackActions.push_back([host, hooksIface = iface.hooks, point]() {
                hooksIface->unregister_hook(host, static_cast<AgentxxPluginHookPoint>(point));
            });
            pctx->hookBindings.push_back(std::move(binding));
            return JS_UNDEFINED;
        }

        case B_UNREGISTER_HOOK: {
            if (argc < 1 || !JS_IsNumber(argv[0])) {
                return JS_ThrowTypeError(ctx, "offHook: point required");
            }
            int32_t point = 0;
            JS_ToInt32(ctx, &point, argv[0]);
            iface.hooks->unregister_hook(host, static_cast<AgentxxPluginHookPoint>(point));
            JS_SetPropertyUint32(ctx, pctx->hooks, static_cast<uint32_t>(point), JS_NULL);
            return JS_UNDEFINED;
        }

        case B_SUBSCRIBE: {
            if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
                return JS_ThrowTypeError(ctx, "subscribe: (topic, handler) required");
            }
            std::string topic   = jsToCppString(ctx, argv[0]);
            auto        binding = std::make_unique<JsHookBinding>();
            binding->engine     = engine;
            binding->plugin     = pctx->name;
            binding->point      = -1;
            auto  topicSv = agentxx::plugin::PluginStringView::from(topic.data(), topic.size());
            auto* sub
                = iface.events->subscribe(host, &topicSv, &JsEngine::eventFire, binding.get());
            if (!sub) {
                return throwJsError(
                    ctx,
                    fmt::format("subscribe: host subscription failed: {}", topic)
                );
            }
            // token = agents 数组索引
            JSValue  lenVal = JS_GetPropertyStr(ctx, pctx->agents, "length");
            uint32_t len    = 0;
            JS_ToUint32(ctx, &len, lenVal);
            JS_FreeValue(ctx, lenVal);
            JSValue entry = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, entry, "topic", JS_NewString(ctx, topic.c_str()));
            JS_SetPropertyStr(ctx, entry, "handler", JS_DupValue(ctx, argv[1]));
            JS_SetPropertyStr(ctx, entry, "token", JS_NewInt32(ctx, static_cast<int32_t>(len)));
            // 宿主订阅句柄: 拆高低 32 位存储 (JS number 为 double, 直接存指针
            // 会丢低位精度; BigInt 依赖 libbf 未裁剪)
            uint64_t subPtr = reinterpret_cast<uint64_t>(sub);
            JS_SetPropertyStr(
                ctx,
                entry,
                "subPtrLo",
                JS_NewInt32(ctx, static_cast<int32_t>(subPtr & 0xFFFFFFFFu))
            );
            JS_SetPropertyStr(
                ctx,
                entry,
                "subPtrHi",
                JS_NewInt32(ctx, static_cast<int32_t>(subPtr >> 32))
            );
            JS_SetPropertyUint32(ctx, pctx->agents, len, entry);
            pctx->rollbackActions.push_back([eventsIface = iface.events, sub]() {
                eventsIface->unsubscribe(sub);
            });
            pctx->hookBindings.push_back(std::move(binding));
            return JS_NewInt32(ctx, static_cast<int32_t>(len));
        }

        case B_UNSUBSCRIBE: {
            if (argc < 1 || !JS_IsNumber(argv[0])) {
                return JS_ThrowTypeError(ctx, "unsubscribe: token required");
            }
            uint32_t token = 0;
            JS_ToUint32(ctx, &token, argv[0]);
            JSValue entry = JS_GetPropertyUint32(ctx, pctx->agents, token);
            if (JS_IsObject(entry)) {
                // 释放宿主订阅 (防事件持续投递到已退订的脚本插件:
                // 退订后宿主不再回调, 插件卸载时也无残留)
                uint64_t subPtr = 0;
                JSValue  loV    = JS_GetPropertyStr(ctx, entry, "subPtrLo");
                JSValue  hiV    = JS_GetPropertyStr(ctx, entry, "subPtrHi");
                uint32_t lo = 0, hi = 0;
                JS_ToUint32(ctx, &lo, loV);
                JS_ToUint32(ctx, &hi, hiV);
                JS_FreeValue(ctx, loV);
                JS_FreeValue(ctx, hiV);
                subPtr = (static_cast<uint64_t>(hi) << 32) | lo;
                if (subPtr) {
                    iface.events->unsubscribe(reinterpret_cast<AgentxxPluginSubscription*>(subPtr));
                }
                JS_SetPropertyUint32(ctx, pctx->agents, token, JS_UNDEFINED);
            }
            JS_FreeValue(ctx, entry);
            return JS_UNDEFINED;
        }

        case B_PUBLISH: {
            std::string topic   = argc >= 1 ? jsToCppString(ctx, argv[0]) : "";
            std::string payload = "{}";
            if (argc >= 2) {
                if (JS_IsString(argv[1])) {
                    payload = jsToCppString(ctx, argv[1]);
                } else if (JS_IsObject(argv[1])) {
                    JSValue json = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
                    if (!JS_IsException(json)) {
                        payload = jsToCppString(ctx, json);
                        JS_FreeValue(ctx, json);
                    } else {
                        JS_FreeValue(ctx, json);
                    }
                }
            }
            auto topicSv = agentxx::plugin::PluginStringView::from(topic.data(), topic.size());
            auto paySv   = agentxx::plugin::PluginStringView::from(payload.data(), payload.size());
            iface.events->publish(host, &topicSv, &paySv);
            return JS_UNDEFINED;
        }

        case B_SET_TIMEOUT: {
            if (argc < 2 || !JS_IsFunction(ctx, argv[0])) {
                return JS_ThrowTypeError(ctx, "setTimeout: (fn, ms) required");
            }
            double ms = 0;
            JS_ToFloat64(ctx, &ms, argv[1]);
            auto              id = ++engine->timerSeq_;
            JsEngine::JsTimer t;
            t.due = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(static_cast<int64_t>(ms < 0 ? 0 : ms));
            t.plugin            = pctx->name;
            t.fn                = JS_DupValue(ctx, argv[0]);
            engine->timers_[id] = std::move(t);
            ++engine->timerEpoch_;    // 定时器集合变化: 唤醒等待中的 JS 线程重算
            engine->cv_.notify_all();
            return JS_NewInt32(ctx, static_cast<int32_t>(id));
        }

        case B_CLEAR_TIMEOUT: {
            if (argc < 1 || !JS_IsNumber(argv[0])) {
                return JS_ThrowTypeError(ctx, "clearTimeout: id required");
            }
            int32_t id = 0;
            JS_ToInt32(ctx, &id, argv[0]);
            auto it = engine->timers_.find(static_cast<uint64_t>(id));
            if (it != engine->timers_.end()) {
                ++engine->timerEpoch_;
                JS_FreeValue(ctx, it->second.fn);
                engine->timers_.erase(it);
            }
            return JS_UNDEFINED;
        }

        case B_LIST_PLUGINS: {
            AgentxxPluginString json{nullptr, 0};
            iface.plugins->list_plugins(host, &json);
            if (!json.data) {
                return JS_NewArray(ctx);
            }
            JSValue out = JS_ParseJSON(ctx, json.data, static_cast<size_t>(json.size), "<plugins>");
            if (JS_IsException(out)) {
                JS_FreeValue(ctx, out);
                out = JS_NewStringLen(ctx, json.data, static_cast<size_t>(json.size));
            }
            agentxx::plugin::PluginString::free(host, &json);
            return out;
        }

        case B_GET_PLUGIN: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "getPlugin: name required");
            }
            std::string         name = jsToCppString(ctx, argv[0]);
            AgentxxPluginString json{nullptr, 0};
            auto nameSv = agentxx::plugin::PluginStringView::from(name.data(), name.size());
            iface.plugins->get_plugin(host, &nameSv, &json);
            if (!json.data) {
                return JS_NULL; // 未安装
            }
            JSValue out = JS_ParseJSON(ctx, json.data, static_cast<size_t>(json.size), "<plugin>");
            if (JS_IsException(out)) {
                JS_FreeValue(ctx, out);
                out = JS_NewStringLen(ctx, json.data, static_cast<size_t>(json.size));
            }
            agentxx::plugin::PluginString::free(host, &json);
            return out;
        }

        // ---- 会话资源扩展 (v8): Skill/Memory/MCP 贡献 ----
        // - 注册失败 (与主配置 yaml/其他插件冲突或宿主不支持) 抛 JS 异常;
        //   注销失败 (不存在/不属于本插件) 返回 false
        case B_ADD_SKILL_DIR:
        case B_ADD_MEMORY_FILE: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "path string required");
            }
            std::string p   = jsToCppString(ctx, argv[0]);
            auto        pSv = agentxx::plugin::PluginStringView::fromCstr(p.c_str());
            int rc = (magic == B_ADD_SKILL_DIR) ? iface.resources->register_skill_dir(host, &pSv)
                                                : iface.resources->register_memory_file(host, &pSv);
            if (rc != 0) {
                return throwJsError(
                    ctx,
                    fmt::format("register failed (conflict or unsupported): {}", p)
                );
            }
            const bool isSkillDir = (magic == B_ADD_SKILL_DIR);
            pctx->rollbackActions.push_back(
                [host, resIface = iface.resources, p, isSkillDir]() {
                    auto pSv2 = agentxx::plugin::PluginStringView::from(p.data(), p.size());
                    if (isSkillDir) {
                        resIface->unregister_skill_dir(host, &pSv2);
                    } else {
                        resIface->unregister_memory_file(host, &pSv2);
                    }
                }
            );
            return JS_TRUE;
        }

        case B_REMOVE_SKILL_DIR:
        case B_REMOVE_MEMORY_FILE: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "path string required");
            }
            std::string p   = jsToCppString(ctx, argv[0]);
            auto        pSv = agentxx::plugin::PluginStringView::fromCstr(p.c_str());
            bool        ok  = (magic == B_REMOVE_SKILL_DIR)
                                  ? (iface.resources->unregister_skill_dir(host, &pSv) == 0)
                                  : (iface.resources->unregister_memory_file(host, &pSv) == 0);
            return ok ? JS_TRUE : JS_FALSE;
        }

        case B_ADD_MCP_SERVER: {
            if (argc < 1 || !JS_IsObject(argv[0])) {
                return JS_ThrowTypeError(ctx, "addMcpServer: spec object required");
            }
            auto specObj = argv[0];
            // 命名空间: namespace 字段优先, 兼容 name 简写
            std::string ns = jsToCppString(ctx, JS_GetPropertyStr(ctx, specObj, "namespace"));
            if (ns.empty()) {
                ns = jsToCppString(ctx, JS_GetPropertyStr(ctx, specObj, "name"));
            }
            std::string url = jsToCppString(ctx, JS_GetPropertyStr(ctx, specObj, "url"));
            if (ns.empty() || url.empty()) {
                return JS_ThrowTypeError(ctx, "addMcpServer: namespace/name and url required");
            }
            double timeoutSec = 120;
            {
                JSValue tv = JS_GetPropertyStr(ctx, specObj, "timeout");
                if (JS_IsNumber(tv)) {
                    JS_ToFloat64(ctx, &timeoutSec, tv);
                }
                JS_FreeValue(ctx, tv);
            }
            // spec JSON 拼装经宿主 json_escape (防注入/转义错误)
            AgentxxPluginString nsEsc{nullptr, 0};
            AgentxxPluginString urlEsc{nullptr, 0};
            auto    nsSv  = agentxx::plugin::PluginStringView::from(ns.data(), ns.size());
            auto    urlSv = agentxx::plugin::PluginStringView::from(url.data(), url.size());
            int32_t rc1   = iface.json->json_escape(host, &nsSv, &nsEsc);
            int32_t rc2   = iface.json->json_escape(host, &urlSv, &urlEsc);
            if (rc1 != 0 || rc2 != 0 || !nsEsc.data || !urlEsc.data) {
                if (nsEsc.data) {
                    agentxx::plugin::PluginString::free(host, &nsEsc);
                }
                if (urlEsc.data) {
                    agentxx::plugin::PluginString::free(host, &urlEsc);
                }
                return JS_ThrowInternalError(ctx, "addMcpServer: escape failed");
            }
            long long   t    = static_cast<long long>(timeoutSec < 0 ? 0 : timeoutSec);
            std::string spec = fmt::format(
                "{{\"namespace\":{},\"url\":{},\"timeout\":{}}}",
                nsEsc.data,
                urlEsc.data,
                t
            );
            agentxx::plugin::PluginString::free(host, &nsEsc);
            agentxx::plugin::PluginString::free(host, &urlEsc);
            auto specSv = agentxx::plugin::PluginStringView::fromCstr(spec.c_str());
            if (iface.resources->register_mcp_server(host, &specSv) != 0) {
                return throwJsError(
                    ctx,
                    fmt::format("addMcpServer register failed (conflict?): {}", ns)
                );
            }
            pctx->rollbackActions.push_back([host, resIface = iface.resources, ns]() {
                auto nsSv2 = agentxx::plugin::PluginStringView::from(ns.data(), ns.size());
                resIface->unregister_mcp_server(host, &nsSv2);
            });
            return JS_TRUE;
        }

        case B_REMOVE_MCP_SERVER: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "removeMcpServer: namespace required");
            }
            std::string ns   = jsToCppString(ctx, argv[0]);
            auto        nsSv = agentxx::plugin::PluginStringView::from(ns.data(), ns.size());
            return iface.resources->unregister_mcp_server(host, &nsSv) == 0 ? JS_TRUE : JS_FALSE;
        }

        default:
            return JS_ThrowInternalError(ctx, "unknown bridge magic %d", magic);
    }
}

// =====================================================================
// 插件入口 (宿主 dlsym)
// =====================================================================

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        0,
        agentxx::plugin::PluginStringView::fromCstr("agentxx_javascript_engine"),
        agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
        agentxx::plugin::PluginStringView::fromCstr(
            "JS interpreter plugin (QuickJS): hosts type:js plugins"
        ),
    };
    return &info;
}

/// 能力方法异步启动 (interpreter.js): "load" 加载脚本到引擎 / "unload" 卸载
/// - 统一异步操作模型: 快速校验在 io 线程内联完成; 脚本执行 (慢) 入队 JS
///   线程, 完成后经 notifier 上报 —— 全程无 postSync 阻塞等待, 根除
///   io↔引擎互等死锁面 (旧模型依赖"提供者回调在调用方线程"规则规避)
/// - caller_host: 脚本插件 (C++ 壳) 的宿主句柄 —— 脚本内 agentxx.registerTool
///   等注册动作经此挂到调用方插件实例 (宿主 detachAll 统一清理)
/// - "load" args: {"name": 脚本插件名, "path": 脚本文件路径}; 完成 payload:
///   {"ok": true, "tools": [...]} (JSON, host->alloc)
/// - "unload" args: {"name": 脚本插件名}; 投递式, 内联完成 {"ok": true}
static void* AGENTXX_PLUGIN_CALL jsCapStart(
    void*                              ctx,
    const AgentxxPluginHost*           caller_host,
    const AgentxxPluginStringView*     method,
    const AgentxxPluginStringView*     args_json,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
) {
    // C ABI 回调异常守卫: 能力方法 start 由宿主 op 驱动器在 io 线程调用,
    // 内含接口查询/文件读取/字符串操作等可抛路径, 异常转 error_out 失败
    auto* engine = static_cast<JsEngine*>(ctx); ///< 提升至 try 外 (catch 日志闭包使用)
    try {
        auto setErr = [&](const char* msg) {
            if (error_out && caller_host) {
                *error_out = agentxx::plugin::PluginString::fromCstr(caller_host, msg);
            }
            return nullptr;
        };
        if (!engine || !notify || agentxx::plugin::PluginStringView::empty(method)) {
            return setErr("interpreter.js: invalid invoke");
        }
        std::string methodStr{
            method && method->data ? method->data : "",
            method ? static_cast<size_t>(method->size) : 0
        };
        std::string argsStr{
            args_json && args_json->data ? args_json->data : "{}",
            args_json ? static_cast<size_t>(args_json->size) : 0
        };
        // 参数解析经宿主 client 无关的 agentxx.agent.json 接口表 (对转义/嵌套结构可靠;
        // caller_host 与本插件同进程, 接口表为同一批静态数据)
        const agentxx::plugin::AgentIfaces callerIf
            = agentxx::plugin::AgentIfaces::query(caller_host);
        if (!callerIf.json || !callerIf.json->json_get_string) {
            return setErr("interpreter.js: host lacks agentxx.agent.json interface");
        }
        auto argStr = [&](const char* key, std::string& out) -> bool {
            AgentxxPluginString v{nullptr, 0};
            auto argsSv = agentxx::plugin::PluginStringView::from(argsStr.data(), argsStr.size());
            auto keySv  = agentxx::plugin::PluginStringView::from(key, std::strlen(key));
            callerIf.json->json_get_string(caller_host, &argsSv, &keySv, &v);
            if (!v.data) {
                return false;
            }
            out.assign(v.data, static_cast<size_t>(v.size));
            agentxx::plugin::PluginString::free(caller_host, &v);
            return true;
        };

        if (methodStr == "load") {
            std::string name, path;
            if (!argStr("name", name) || !argStr("path", path)) {
                return setErr("interpreter.js load: name and path (string) required");
            }
            if (name.empty() || path.empty()) {
                return setErr("interpreter.js load: name and path required");
            }
            // 读文件 (本地小文件, 内联可接受); 执行脚本 (慢) 入队 JS 线程
            std::ifstream f(path, std::ios::binary);
            if (!f) {
                return setErr("interpreter.js load: cannot open script file");
            }
            std::stringstream ss;
            ss << f.rdbuf();
            std::string code = ss.str();
            if (code.empty()) {
                return setErr("interpreter.js load: empty script file");
            }
            // JS 线程任务: 直接调用 doLoadScript (已在 JS 线程, 无需 postSync),
            // 完成后取工具清单并经通知器上报
            AgentxxPluginOperatorNotify ntfCopy = *notify;
            if (!engine->post([engine, caller_host, ntfCopy, name, path, code]() {
                    if (engine->stopping()) {
                        // 停止中: 不执行脚本加载, 但仍终结本次能力 op
                        auto errSv = agentxx::plugin::PluginStringView::fromCstr(
                            "interpreter.js engine stopped"
                        );
                        ntfCopy.done(ntfCopy.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
                        return;
                    }
                    std::string err2;
                    const int   rc2
                        = engine->loadScriptOnJsThread(caller_host, name, path, code, err2);
                    if (rc2 != 0) {
                        auto errSv2
                            = agentxx::plugin::PluginStringView::from(err2.data(), err2.size());
                        ntfCopy.done(ntfCopy.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv2);
                        return;
                    }
                    std::string tools      = engine->loadedToolsJsonOnJsThread(name);
                    std::string payloadStr = fmt::format(R"({{"ok": true, "tools": {}}})", tools);
                    auto        resSv      = agentxx::plugin::PluginStringView::from(
                        payloadStr.data(),
                        payloadStr.size()
                    );
                    ntfCopy.done(ntfCopy.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, &resSv);
                })) {
                return setErr("interpreter.js engine stopped");
            }
            // 活动 op 占位 (宿主只等完成通知; execute_poll 留 NULL)。
            // 用实例成员地址而非函数级 static: 同一动态库多实例并存时
            // 每个实例各自持有稳定地址, 不共享可变静态存储。
            return engine->capOpToken();
        }

        if (methodStr == "unload") {
            std::string name;
            if (!argStr("name", name) || name.empty()) {
                return setErr("interpreter.js unload: name (string) required");
            }
            // done 在脚本真正卸载之后 (JS 线程执行 rollback + 定时器清理) 触发
            AgentxxPluginOperatorNotify ntfCopy = *notify;
            if (!engine->post([engine, name, ntfCopy]() {
                    if (engine->stopping()) {
                        // 停止中: 脚本上下文由 JS 线程退出路径统一释放,
                        // 不再执行脚本 rollback (宿主侧注册此时已撤销)。
                        auto errSv = agentxx::plugin::PluginStringView::fromCstr(
                            "interpreter.js engine stopped"
                        );
                        ntfCopy.done(ntfCopy.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
                        return;
                    }
                    engine->unloadScriptOnJsThread(name);
                    auto okSv = agentxx::plugin::PluginStringView::fromCstr("{\"ok\": true}");
                    ntfCopy.done(ntfCopy.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, &okSv);
                })) {
                return setErr("interpreter.js engine stopped");
            }
            return engine->capOpToken();
        }

        {
            std::string _msg = fmt::format("interpreter.js: unknown method `{}`", methodStr);
            setErr(_msg.c_str());
        }
        return nullptr;
    } catch (...) {
        // 异常分类上报 + 尽力设置 error_out (宿主按 OP_FAILED 处理)
        ::agentxx::plugin::reportCurrentException([engine](const char* m) noexcept {
            if (engine) {
                engine->guardLog(m);
            }
        });
        if (error_out && caller_host) {
            *error_out = agentxx::plugin::PluginString::fromCstr(
                caller_host,
                "interpreter.js: internal exception"
            );
        }
        return nullptr;
    }
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: create 只构造上下文 (查询接口 + 装配宿主句柄),
    // 不创建 runtime/线程, 也不提交任何运行时注册 (Reset-v1 §5.2); 异常返回
    // -1 走宿主加载失败清理路径
    JsEngine* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* m) noexcept {
            if (raw) {
                raw->guardLog(m);
            }
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto* engine = new JsEngine();
            raw = engine; ///< create 段内异常路径日志可用 (函数出口随栈帧销毁)
            engine->setEngineHost(host);

            // COM 风格接口表查询: entry 内一次性查询全部已知 IID (存入局部;
            // 不可用函数级 static —— 多实例加载时各宿主接口表指针不同)
            const agentxx::plugin::AgentIfaces s_if = agentxx::plugin::AgentIfaces::query(host);
            if (!s_if.capabilities || !s_if.capabilities->register_capability_ex || !s_if.log) {
                delete engine;
                raw = nullptr;
                return -1;
            }
            *plugin_ctx = engine; ///< 所有权移交宿主 (destroy 时取回归还)
            return 0;
        }
    );
}

/// 引擎 start (Reset-v1 start 事务): 创建 JSRuntime + 专用 JS 线程, 并注册
/// 能力 "interpreter.js"。
///
/// - create 阶段不启动线程/不注册: disable→enable 往返等价于一次 stop+start,
///   引擎线程与脚本上下文都按事务重建, 不存在"线程已存在但实例已停用"的中间态;
/// - 线程启动失败或能力注册失败 → 返回 NULL + error, 宿主回滚本次加载 (注册
///   失败时先停止刚启动的引擎, 不留线程/运行时残留);
/// - 重复 start: 引擎已运行时幂等成功, 能力注册在宿主侧按 owner+名称去重。
static void* AGENTXX_PLUGIN_CALL jsEngineStart(
    void*                              plugin_ctx,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
) {
    auto* engine = static_cast<JsEngine*>(plugin_ctx); ///< 提升至 try 外 (catch 日志闭包使用)
    try {
        auto setErr = [&](const char* msg) {
            if (error_out && engine && engine->host()) {
                *error_out = agentxx::plugin::PluginString::fromCstr(engine->host(), msg);
            }
            return nullptr;
        };
        if (!engine || !notify || !notify->done) {
            return setErr("interpreter.js start: invalid lifecycle call");
        }
        if (!engine->start()) {
            return setErr("interpreter.js start: JS runtime init failed");
        }
        const AgentxxPluginHost* host = engine->host();
        const agentxx::plugin::AgentIfaces iface = agentxx::plugin::AgentIfaces::query(host);
        if (!iface.capabilities || !iface.capabilities->register_capability_ex) {
            engine->requestStop(nullptr);
            return setErr("interpreter.js start: host lacks capabilities interface");
        }
        if (engine->capabilityRegistered()) {
            // 重复 start (幂等): 能力仍由本实例持有, 宿主尚未撤销
        } else {
            // 能力 "interpreter.js" (异步方法处理器三件套): 脚本插件 (C++ 壳) 经
            // invoke_capability(_async) 把脚本代码交给本引擎执行 —— 插件间通信,
            // 宿主不参与; load/unload 均为异步完成 (JS 线程执行)
            auto capSv = agentxx::plugin::PluginStringView::fromCstr("interpreter.js");
            int  rc = iface.capabilities
                          ->register_capability_ex(host, &capSv, &jsCapStart, nullptr, engine);
            if (rc != 0) {
                // start 事务失败: 撤销本事务已启动的引擎线程 (destroy 随后会
                // 等待收尾完成, 不留线程/运行时残留)
                engine->requestStop(nullptr);
                return setErr("interpreter.js start: capability registration failed");
            }
            engine->setCapabilityRegistered(true);
        }
        if (iface.log && iface.log->log) {
            auto loadedSv = agentxx::plugin::PluginStringView::fromCstr(
                "agentxx_javascript_engine started (QuickJS interpreter.js)"
            );
            iface.log->log(host, 2, &loadedSv);
        }
        notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
        return nullptr; ///< 同步完成
    } catch (...) {
        ::agentxx::plugin::reportCurrentException([engine](const char* m) noexcept {
            if (engine) {
                engine->guardLog(m);
            }
        });
        if (notify && notify->done) {
            auto errSv
                = agentxx::plugin::PluginStringView::fromCstr("interpreter.js start: exception");
            notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
        }
        return nullptr;
    }
}

/// 引擎 stop (Reset-v1 stop 事务): 停止 JS 线程并释放 runtime。
///
/// - 立即拒绝新任务: 之后到达的能力调用/工具执行一律失败, 不再排队;
/// - 队列中尚未开始的任务按取消终结 (不执行插件 JS 代码); 已经开始的执行
///   由 drivePromise 观察 stop 标志后按取消完成;
/// - JS 线程的 join 与 JS_FreeRuntime 交给收尾线程执行 (不能在 IO 线程直接
///   join: JS 线程可能在宿主 vtable 调用中等待 IO 线程), done 在收尾完成后
///   从收尾线程上报 (完成通知允许来自任意线程)。
static void* AGENTXX_PLUGIN_CALL jsEngineStop(
    void*                              plugin_ctx,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
) {
    auto* engine = static_cast<JsEngine*>(plugin_ctx); ///< 提升至 try 外 (catch 日志闭包使用)
    try {
        if (!engine || !notify || !notify->done) {
            if (error_out && engine && engine->host()) {
                *error_out = agentxx::plugin::PluginString::fromCstr(
                    engine->host(),
                    "interpreter.js stop: invalid lifecycle call"
                );
            }
            return nullptr;
        }
        AgentxxPluginOperatorNotify ntfCopy = *notify;
        // 能力注册由宿主在停用/卸载时统一撤销 (detachInstanceRegistrations /
        // detachAll), 这里只清本实例的幂等标记。
        engine->setCapabilityRegistered(false);
        engine->requestStop([engine, ntfCopy]() mutable {
            ntfCopy.done(ntfCopy.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
        });
        // 已接受: 返回非空句柄, 完成通知在收尾线程发出 (幂等重复 stop 同样成立)
        return engine->capOpToken();
    } catch (...) {
        ::agentxx::plugin::reportCurrentException([engine](const char* m) noexcept {
            if (engine) {
                engine->guardLog(m);
            }
        });
        if (notify && notify->done) {
            auto errSv
                = agentxx::plugin::PluginStringView::fromCstr("interpreter.js stop: exception");
            notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
        }
        return nullptr;
    }
}

extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_agent_start(
    void*                              plugin_ctx,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               err
) {
    // 入口为 noexcept 守卫函数 (内部 try/catch 已分类上报, 异常不穿越 C ABI)
    return jsEngineStart(plugin_ctx, notify, err);
}

extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_agent_stop(
    void*                              plugin_ctx,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               err
) {
    return jsEngineStop(plugin_ctx, notify, err);
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* engine = static_cast<JsEngine*>(plugin_ctx);
    // C ABI 边界异常守卫: 销毁回调 (同步等待停止收尾 = 停线程 + 释放 runtime)
    // 异常不得外泄, 否则 JS 线程/runtime 泄漏且宿主卸载流程被打断;
    // 先借本实例宿主句柄装配日志闭包再 delete (delete 后不得访问成员)
    const AgentxxPluginHost*     ownHost = engine ? engine->host() : nullptr;
    const AgentxxPluginLogIface* ownLog  = nullptr;
    if (ownHost && ownHost->vtable && ownHost->vtable->query_interface) {
        auto iidSv = agentxx::plugin::PluginStringView::fromCstr(AGENTXX_PLUGIN_IFACE_AGENT_LOG);
        ownLog     = static_cast<const AgentxxPluginLogIface*>(
            ownHost->vtable->query_interface(ownHost, &iidSv)
        );
    }
    agentxx::plugin::guardCallVoid(
        [ownHost, ownLog](const char* m) noexcept {
            agentxx::plugin::logTo(ownHost, ownLog, 4, "agentxx_javascript_engine", m);
        },
        [engine]() noexcept {
            delete engine;
        }
    ); // 停止 JS 线程并释放 runtime
}
