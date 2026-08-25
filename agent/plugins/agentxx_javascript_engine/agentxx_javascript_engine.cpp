/*
 * agentxx_javascript_engine —— JS 解释器插件 (二期)
 *
 * 功能: 注册 "interpreter.js" 脚本引擎, 承载加载/卸载 type: js 的脚本插件
 *
 * 线程模型 (关键设计, 统一异步操作模型):
 * - 专用 JS 线程: 所有 QuickJS 操作集中于该线程 (QuickJS 非线程安全)
 * - 任务队列 (互斥锁 + 条件变量): post 非阻塞投递
 * - 工具 execute / 能力 load: 宿主 io 线程 start 入队 JS 线程后立即返回,
 *   JS 线程执行完毕经 AgentxxOpNotify 上报完成 (线程安全) —— 全程无阻塞
 *   等待, 宿主 io 协程与 JS 任务交错执行; 旧版 postSync 阻塞桥已移除
 * - JS 线程 → io 线程: host vtable 内部经 post_to_io + 同步等待 (宿主实现)
 * - 钩子/事件回调: io 线程 → post 到 JS 线程 (fire-and-forget, 不等待)
 * - JS 内 callTool 命中本引擎工具: 同线程内联执行 (防自锁)
 * - 卸载安全: JsPluginCtx 由 shared_ptr 管理; 跨线程经 mirror 表 (互斥锁) 查
 *   强引用; 插件卸载 (deleted) 后已入队任务检查标志跳过; JSContext 释放由
 *   JsPluginCtx 析构完成 (在途任务全部结束后)
 *
 * 沙箱: 内存限制 (JS_SetMemoryLimit) + 栈限制 + 指令中断超时; 不引入
 * quickjs-libc (无 os/std 模块); 全局仅注入标准 ECMA 内置 + agentxx 桥
 */
#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_iface_helper.h"
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

constexpr size_t kMemoryLimit   = 64 * 1024 * 1024; ///< JS 内存上限 64MB
constexpr size_t kStackLimit    = 512 * 1024;       ///< JS 栈上限 512KB
constexpr size_t kTaskTimeoutMs = 60000;            ///< 单任务 (工具执行等) 超时

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
    bool        done = false;
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

/// 脚本插件上下文 (生命周期: plugins_ / mirror / 在途任务共享)
/// - 数据成员仅 JS 线程访问 (deleted/inflight 亦仅 JS 线程, 任务串行)
/// - JSContext 释放: 析构函数 (在途任务全部结束后由最后一个持有者析构)
struct JsPluginCtx {
    std::string        name;
    JSContext*         ctx      = nullptr;
    const AgentxxHost* host     = nullptr; ///< 脚本插件宿主句柄
    JsEngine*          engine   = nullptr;
    bool               deleted  = false; ///< 已卸载 (入队任务检查后跳过)
    size_t             inflight = 0;     ///< 在途访问任务数 (execute/hook/event)
    /// 工具表: 普通对象 name -> {execute, name, description}
    JSValue tools = JS_UNDEFINED;
    /// 钩子表: Array(7) 元素为 fn 或 null
    JSValue hooks = JS_UNDEFINED;
    /// 订阅表: Array of {topic, handler, token}
    JSValue                                     agents = JS_UNDEFINED;
    std::vector<std::unique_ptr<JsToolBinding>> toolBindings;
    std::vector<std::unique_ptr<JsHookBinding>> hookBindings;

    ~JsPluginCtx();
};

} // namespace

// =====================================================================
// JsEngine
// =====================================================================

class JsEngine {
public:

    JsEngine() {
        rt_ = JS_NewRuntime();
        JS_SetMemoryLimit(rt_, kMemoryLimit);
        JS_SetMaxStackSize(rt_, kStackLimit);
        JS_SetInterruptHandler(rt_, &JsEngine::interruptHandler, this);
        taskStart_ = std::chrono::steady_clock::now();
        thread_    = std::thread(&JsEngine::jsThreadMain, this);
    }

    ~JsEngine() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join(); // 处理完已入队任务 (含在途 execute) 后退出
        }
        if (rt_) {
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
        }
    }

    void setEngineHost(const AgentxxHost* host) {
        engineHost_ = host;
    }

    /// 引擎插件宿主句柄 (供静态桥回调读取; 可空)
    const AgentxxHost* host() const {
        return engineHost_;
    }

    /// JS 线程内加载脚本 (公共转发; 供能力方法异步任务在 JS 线程内直调)
    int loadScriptOnJsThread(
        const AgentxxHost* host,
        const std::string& name,
        const std::string& path,
        const std::string& code,
        std::string&       err
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
        std::string out = "[";
        bool        first = true;
        for (auto& k : jsToolsSnapshot(pctx.get())) {
            if (!first) {
                out += ",";
            }
            first  = false;
            out   += "\"" + k + "\"";
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

    /// 工具 execute 桥 —— 异步启动 (统一异步操作模型):
    /// 【宿主 io 线程】调用; 任务入队 JS 线程后立即返回句柄,
    /// 完成结果由 JS 线程经 notifier 上报 (线程安全), io 线程零阻塞
    static void* toolExecuteStart(
        void*                   ud,
        AgentxxPluginStringView args_json,
        AgentxxPluginStringView thread_id,
        AgentxxPluginStringView tool_call_id,
        const AgentxxOpNotify*  notify,
        char**                  error_out
    );

    /// 钩子回调桥 —— 异步启动 (统一异步操作模型):
    /// 【宿主 io 线程】调用; post 到 JS 线程 (fire-and-forget, 与旧语义一致)
    /// 后立即内联完成通知
    static void* hookStart(
        void*                   ud,
        AgentxxHookPoint        point,
        AgentxxPluginStringView node_input_json,
        const AgentxxOpNotify*  notify,
        char**                  error_out
    );

    /// 事件回调桥: io 线程调用; post 到 JS 线程 (fire-and-forget) (类外定义)
    static void eventFire(AgentxxPluginStringView event_json, void* ud);

    // ==================== 任务队列 ====================

    /// 投递任务到 JS 线程 (非阻塞); 返回 false = 引擎已停止 (未入队)
    bool post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stop_) {
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
            if (stop_) {
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

    static void setErr(char** err_out, const AgentxxHost* host, const char* msg) {
        if (err_out && host) {
            *err_out = host->vtable->strdup(msg);
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
    void fireDueTimersInline() {
        auto now = std::chrono::steady_clock::now();
        for (auto it = timers_.begin(); it != timers_.end();) {
            if (it->second.due > now) {
                ++it;
                continue;
            }
            auto timer = std::move(it->second);
            it         = timers_.erase(it);
            auto pctx  = findPlugin(timer.plugin);
            if (!pctx || pctx->deleted || !pctx->ctx) {
                continue; // 上下文已销毁, fn 引用随 JsPluginCtx 释放
            }
            JSValue ret = JS_Call(pctx->ctx, timer.fn, JS_UNDEFINED, 0, nullptr);
            JSValue r   = drivePromise(pctx.get(), ret);
            JS_FreeValue(pctx->ctx, r);
            JS_FreeValue(pctx->ctx, ret);
            JS_FreeValue(pctx->ctx, timer.fn);
        }
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
                        return stop_ || !queue_.empty();
                    });
                } else {
                    cv_.wait_until(lk, next, [&]() {
                        return stop_ || !queue_.empty();
                    });
                }
                // 退出条件仅看 stop_ + 队列: 未到期长定时器不再导致退出前忙循环
                // (定时器引用在下方 break 前统一释放)
                if (stop_ && queue_.empty()) {
                    break;
                }
                if (queue_.empty()) {
                    // 仅定时器到期: 直接执行后继续等待
                    lk.unlock();
                    fireDueTimersInline();
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            taskStart_ = std::chrono::steady_clock::now();
            try {
                task();
            } catch (...) {
                // 任务异常不得终止 JS 线程
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
        const AgentxxHost* host,
        const std::string& name,
        const std::string& path,
        const std::string& code,
        std::string&       err
    ) {
        if (plugins_.find(name) != plugins_.end()) {
            err = "plugin already loaded: " + name;
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
            return -1;
        }

        // 执行插件脚本 (顶层可调用 agentxx.registerTool 等)
        JSValue ret = JS_Eval(pctx->ctx, code.c_str(), code.size(), path.c_str(), 0);
        if (JS_IsException(ret)) {
            err = extractException(pctx->ctx, ret);
            JS_FreeValue(pctx->ctx, ret);
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

    void doUnloadScript(const std::string& name) {
        auto it = plugins_.find(name);
        if (it == plugins_.end()) {
            return;
        }
        auto pctx     = it->second;
        pctx->deleted = true;
        pctx->host    = nullptr; // 后续任务不得再调用宿主
        plugins_.erase(it);
        // 清理该插件未到期定时器 (JS 线程; fn 引用在上下文 Free 前释放)
        for (auto tit = timers_.begin(); tit != timers_.end();) {
            if (tit->second.plugin == name) {
                JS_FreeValue(pctx->ctx, tit->second.fn);
                tit = timers_.erase(tit);
            } else {
                ++tit;
            }
        }
        {
            std::lock_guard<std::mutex> lk(mirrorMtx_);
            mirror_.erase(name);
        }
        // 在途任务全部结束后 shared_ptr 归零 → JsPluginCtx 析构 → JS_FreeContext
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
            req.error = "tool not found in plugin: " + binding->name;
            JS_FreeValue(pctx->ctx, entry);
            return;
        }
        JSValue execFn = JS_GetPropertyStr(pctx->ctx, entry, "execute");
        JS_FreeValue(pctx->ctx, entry);
        if (!JS_IsFunction(pctx->ctx, execFn)) {
            req.error = "tool execute not a function: " + binding->name;
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

        JSValue result = drivePromise(pctx.get(), ret);
        JS_FreeValue(pctx->ctx, ret);
        if (JS_IsException(result)) {
            req.error = extractException(pctx->ctx, result);
            JS_FreeValue(pctx->ctx, result);
        } else {
            req.result = valueToJsonString(pctx->ctx, result);
            JS_FreeValue(pctx->ctx, result);
        }
        JS_FreeValue(pctx->ctx, execFn);
    }

    // ==================== 钩子/事件 (JS 线程) ====================

    void doHookFire(JsHookBinding* binding, int point, const std::string& payload) {
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

        JSValue fn = JS_GetPropertyUint32(pctx->ctx, pctx->hooks, static_cast<uint32_t>(point));
        if (JS_IsFunction(pctx->ctx, fn)) {
            JSValue arg = JS_ParseJSON(pctx->ctx, payload.c_str(), payload.size(), "<hook>");
            if (JS_IsException(arg)) {
                JS_FreeValue(pctx->ctx, arg);
                arg = JS_NewString(pctx->ctx, payload.c_str());
            }
            JSValue ret = JS_Call(pctx->ctx, fn, JS_UNDEFINED, 1, &arg);
            JSValue r   = drivePromise(pctx.get(), ret);
            JS_FreeValue(pctx->ctx, r);
            JS_FreeValue(pctx->ctx, ret);
            JS_FreeValue(pctx->ctx, arg);
        }
        JS_FreeValue(pctx->ctx, fn);
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
                    JSValue r   = drivePromise(pctx.get(), ret);
                    JS_FreeValue(pctx->ctx, r);
                    JS_FreeValue(pctx->ctx, ret);
                    JS_FreeValue(pctx->ctx, arg);
                }
                JS_FreeValue(pctx->ctx, handler);
            }
            JS_FreeValue(pctx->ctx, entry);
        }
    }

    // ==================== Promise 驱动 ====================

    /// 驱动 Promise 直至 settle (JS 线程内调用)
    /// - 非 Promise 原样 Dup; Promise 驱动 job 队列; 返回结果值 (调用方 Free)
    JSValue drivePromise(JsPluginCtx* pctx, JSValue value) {
        if (!JS_IsPromise(value)) {
            return JS_DupValue(pctx->ctx, value);
        }
        // 等待上限: interrupt handler (60s) 负责打断长任务; 此处 120s 兜底
        // (sleep 期间 interrupt 不检查, 由本 guard 控制总等待)
        int guard = 0;
        while (JS_PromiseState(pctx->ctx, value) == JS_PROMISE_PENDING && guard++ < 120000) {
            JSContext* jobCtx = nullptr;
            int        rc     = JS_ExecutePendingJob(rt_, &jobCtx);
            if (rc < 0) {
                return JS_NewString(pctx->ctx, "[pending job exception]");
            }
            if (rc == 0) {
                // 无 job: 可能等待外部事件 (setTimeout); 执行到期定时器后
                // 等待下一个定时器到期 (避免 1ms 忙轮询; 新定时器注册会
                // notify 唤醒, stop_ 时尽快退出)
                fireDueTimersInline();
                auto next = nextTimerLocked();
                if (next == std::chrono::steady_clock::time_point::max()) {
                    // 无任何定时器: 无法推进, 短暂让出后重试 (Promise 可能
                    // 依赖 job 队列, 由循环头 JS_ExecutePendingJob 重新检查)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } else {
                    std::unique_lock<std::mutex> lk(mtx_);
                    cv_.wait_until(lk, next, [&]() {
                        return stop_;
                    });
                }
            }
        }
        if (JS_PromiseState(pctx->ctx, value) == JS_PROMISE_FULFILLED) {
            return JS_PromiseResult(pctx->ctx, value); // 新引用, 调用方 Free
        }
        if (JS_PromiseState(pctx->ctx, value) == JS_PROMISE_REJECTED) {
            JSValue     r = JS_PromiseResult(pctx->ctx, value);
            std::string s = valueToJsonString(pctx->ctx, r);
            return JS_NewString(pctx->ctx, s.c_str());
        }
        return JS_NewString(pctx->ctx, "[promise timeout]");
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
            // 注意: JS_SetProperty* 为 move 语义 (消费传入值), 不得再 Free
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
    const AgentxxHost*                engineHost_ = nullptr;
    std::thread                       thread_;
    std::mutex                        mtx_;
    std::condition_variable           cv_;
    std::deque<std::function<void()>> queue_;
    bool                              stop_ = false;

    std::chrono::steady_clock::time_point taskStart_;

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
    void*                   ud,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    const AgentxxOpNotify*  notify,
    char**                  error_out
) {
    auto* binding = static_cast<JsToolBinding*>(ud);
    auto* engine  = binding ? binding->engine : nullptr;
    if (!engine || !notify) {
        return nullptr;
    }
    auto req   = std::make_shared<ToolExecReq>();
    req->args  = std::string{args_json.data ? args_json.data : "{}", args_json.size};
    req->tid   = std::string{thread_id.data ? thread_id.data : "", thread_id.size};
    req->tcid  = std::string{tool_call_id.data ? tool_call_id.data : "", tool_call_id.size};

    // op 句柄仅作占位标识 (生命周期由本函数管理; 宿主不解释其内容)
    auto*      op = new int(0);
    if (!engine->post([engine, binding, req, notify]() {
            // ---- JS 线程: 执行并上报 ----
            engine->doToolExecute(binding, *req);
            const AgentxxHost* host = engine->engineHost_;
            if (!req->error.empty()) {
                char* payload = host ? host->vtable->strdup(req->error.c_str()) : nullptr;
                notify->done(notify->host_ud, AGENTXX_OP_FAILED, payload);
            } else {
                char* payload = host ? host->vtable->strdup(req->result.c_str()) : nullptr;
                notify->done(notify->host_ud, AGENTXX_OP_OK, payload);
            }
        })) {
        // 引擎已停止: 启动失败
        delete op;
        if (error_out && engine->engineHost_) {
            *error_out = engine->engineHost_->vtable->strdup("interpreter.js engine stopped");
        }
        return nullptr;
    }
    return op;
}

void* JsEngine::hookStart(
    void*                   ud,
    AgentxxHookPoint        point,
    AgentxxPluginStringView node_input_json,
    const AgentxxOpNotify*  notify,
    char**                  error_out
) {
    (void)error_out;
    auto* binding = static_cast<JsHookBinding*>(ud);
    auto* engine  = binding ? binding->engine : nullptr;
    if (!engine || !notify) {
        return nullptr;
    }
    std::string payload{node_input_json.data ? node_input_json.data : "", node_input_json.size};
    int         pt = static_cast<int>(point);
    engine->post([engine, binding, payload, pt]() {
        engine->doHookFire(binding, pt, payload);
    });
    // fire-and-forget: 投递成功即内联完成 (JS 执行结果不回传)
    notify->done(notify->host_ud, AGENTXX_OP_OK, nullptr);
    return nullptr;
}

void JsEngine::eventFire(AgentxxPluginStringView event_json, void* ud) {
    auto* binding = static_cast<JsHookBinding*>(ud);
    auto* engine  = binding ? binding->engine : nullptr;
    if (!engine) {
        return;
    }
    std::string payload{event_json.data ? event_json.data : "", event_json.size};
    engine->post([engine, binding, payload]() {
        engine->doEventFire(binding, payload);
    });
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
    const AgentxxHost* host   = pctx->host;
    const auto&        vt     = *host->vtable; // 核心: alloc/free/strdup
    // COM 风格接口表查询 (进程级静态数据; 各能力经稳定 IID 分发)
    const agentxx::plugin::AgentIfaces iface = agentxx::plugin::AgentIfaces::query(host);
    auto*              engine = pctx->engine;

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

            AgentxxToolSpec spec{};
            spec.name            = agentxx_plugin_sv(name.data(), name.size());
            spec.description     = agentxx_plugin_sv(desc.data(), desc.size());
            spec.parameters_json = agentxx_plugin_sv(paramsJson.data(), paramsJson.size());
            // 统一异步操作模型: 异步桥 (JS 线程完成时经通知器上报)
            spec.execute_start   = &JsEngine::toolExecuteStart;
            spec.execute_poll    = nullptr; ///< 只等完成通知
            spec.execute_cancel  = nullptr;
            spec.user_data       = binding.get();
            int rc               = iface.tools->register_tool(host, &spec);
            if (rc != 0) {
                JS_FreeValue(ctx, execFn);
                return throwJsError(
                    ctx,
                    "registerTool: host registration failed (conflict?): " + name
                );
            }

            // JS 侧工具表登记 (供 JS 内 callTool 内联执行)
            JSValue entry = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, entry, "execute", JS_DupValue(ctx, execFn));
            JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, name.c_str()));
            JS_SetPropertyStr(ctx, entry, "description", JS_NewString(ctx, desc.c_str()));
            JS_SetPropertyStr(ctx, pctx->tools, name.c_str(), entry);
            JS_FreeValue(ctx, execFn);

            pctx->toolBindings.push_back(std::move(binding));
            return JS_UNDEFINED;
        }

        case B_UNREGISTER_TOOL: {
            std::string name = argc >= 1 ? jsToCppString(ctx, argv[0]) : "";
            if (name.empty()) {
                return JS_ThrowTypeError(ctx, "unregisterTool: name required");
            }
            iface.tools->unregister_tool(host, agentxx_plugin_sv(name.data(), name.size()));
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
                    JSValue result = engine->drivePromise(pctx, ret);
                    JS_FreeValue(ctx, ret);
                    JS_FreeValue(ctx, execFn);
                    return result;
                }
                JS_FreeValue(ctx, execFn);
                return throwJsError(ctx, "callTool: execute not a function: " + name);
            }
            JS_FreeValue(ctx, entry);

            // 2) 宿主插件工具 (同步互调; vtable 内部保证线程安全)
            char* err  = nullptr;
            char* resp = iface.tools->call_tool(
                host,
                agentxx_plugin_sv(name.data(), name.size()),
                agentxx_plugin_sv(argsJson.data(), argsJson.size()),
                agentxx_plugin_sv(sessionId.data(), sessionId.size()),
                &err
            );
            if (!resp) {
                std::string errStr = err ? err : "call_tool failed";
                if (err) {
                    vt.free(err);
                }
                return throwJsError(ctx, errStr);
            }
            JSValue out = JS_ParseJSON(ctx, resp, std::strlen(resp), "<resp>");
            if (JS_IsException(out)) {
                JS_FreeValue(ctx, out);
                out = JS_NewString(ctx, resp);
            }
            vt.free(resp);
            return out;
        }

        case B_GET_SHARE_STORE: {
            std::string sessionId = argc >= 1 ? jsToCppString(ctx, argv[0]) : "";
            int64_t     id        = 0;
            if (argc >= 2) {
                JS_ToInt64(ctx, &id, argv[1]);
            }
            char* resp = iface.session->get_share_store(
                host,
                agentxx_plugin_sv(sessionId.data(), sessionId.size()),
                id
            );
            if (!resp) {
                return JS_NULL;
            }
            JSValue out = JS_NewString(ctx, resp);
            vt.free(resp);
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
            iface.session->emit_message_tip(
                host,
                agentxx_plugin_sv(sessionId.data(), sessionId.size()),
                agentxx_plugin_sv(text.data(), text.size()),
                level
            );
            return JS_UNDEFINED;
        }

        case B_LOG: {
            int level = 2;
            if (argc >= 1 && JS_IsNumber(argv[0])) {
                int32_t lv = 0;
                JS_ToInt32(ctx, &lv, argv[0]);
                level = lv;
            }
            std::string msg = argc >= 2 ? jsToCppString(ctx, argv[1]) : "";
            iface.log->log(host, level, agentxx_plugin_sv(msg.data(), msg.size()));
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
                iface.hooks->unregister_hook(host, static_cast<AgentxxHookPoint>(point));
            }
            JS_FreeValue(ctx, oldFn);

            auto binding    = std::make_unique<JsHookBinding>();
            binding->engine = engine;
            binding->plugin = pctx->name;
            binding->point  = point;
            AgentxxHookSpec hspec{};
            hspec.point      = static_cast<AgentxxHookPoint>(point);
            hspec.hook_start = &JsEngine::hookStart;
            hspec.hook_poll  = nullptr;
            hspec.hook_cancel = nullptr;
            hspec.user_data  = binding.get();
            int rc           = iface.hooks->register_hook(host, &hspec);
            if (rc != 0) {
                return throwJsError(ctx, "onHook: host registration failed");
            }
            JS_SetPropertyUint32(
                ctx,
                pctx->hooks,
                static_cast<uint32_t>(point),
                JS_DupValue(ctx, argv[1])
            );
            pctx->hookBindings.push_back(std::move(binding));
            return JS_UNDEFINED;
        }

        case B_UNREGISTER_HOOK: {
            if (argc < 1 || !JS_IsNumber(argv[0])) {
                return JS_ThrowTypeError(ctx, "offHook: point required");
            }
            int32_t point = 0;
            JS_ToInt32(ctx, &point, argv[0]);
            iface.hooks->unregister_hook(host, static_cast<AgentxxHookPoint>(point));
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
            auto* sub           = iface.events->subscribe(
                host,
                agentxx_plugin_sv(topic.data(), topic.size()),
                &JsEngine::eventFire,
                binding.get()
            );
            if (!sub) {
                return throwJsError(ctx, "subscribe: host subscription failed: " + topic);
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
                    iface.events->unsubscribe(reinterpret_cast<AgentxxSubscription*>(subPtr));
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
            iface.events->publish(
                host,
                agentxx_plugin_sv(topic.data(), topic.size()),
                agentxx_plugin_sv(payload.data(), payload.size())
            );
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
            engine->cv_.notify_all(); // 唤醒等待中的 JS 线程 (wait_until 更新)
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
                JS_FreeValue(ctx, it->second.fn);
                engine->timers_.erase(it);
            }
            return JS_UNDEFINED;
        }

        case B_LIST_PLUGINS: {
            char* json = iface.plugins->list_plugins(host);
            if (!json) {
                return JS_NewArray(ctx);
            }
            JSValue out = JS_ParseJSON(ctx, json, std::strlen(json), "<plugins>");
            if (JS_IsException(out)) {
                JS_FreeValue(ctx, out);
                out = JS_NewString(ctx, json);
            }
            vt.free(json);
            return out;
        }

        case B_GET_PLUGIN: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "getPlugin: name required");
            }
            std::string name = jsToCppString(ctx, argv[0]);
            char*       json = iface.plugins->get_plugin(host, agentxx_plugin_sv(name.data(), name.size()));
            if (!json) {
                return JS_NULL; // 未安装
            }
            JSValue out = JS_ParseJSON(ctx, json, std::strlen(json), "<plugin>");
            if (JS_IsException(out)) {
                JS_FreeValue(ctx, out);
                out = JS_NewString(ctx, json);
            }
            vt.free(json);
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
            std::string p = jsToCppString(ctx, argv[0]);
            int         rc = (magic == B_ADD_SKILL_DIR)
                                 ? iface.resources->register_skill_dir(host, agentxx_plugin_sv_cstr(p.c_str()))
                                 : iface.resources->register_memory_file(host, agentxx_plugin_sv_cstr(p.c_str()));
            if (rc != 0) {
                return throwJsError(ctx, "register failed (conflict or unsupported): " + p);
            }
            return JS_TRUE;
        }

        case B_REMOVE_SKILL_DIR:
        case B_REMOVE_MEMORY_FILE: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "path string required");
            }
            std::string p = jsToCppString(ctx, argv[0]);
            bool ok = (magic == B_REMOVE_SKILL_DIR)
                          ? iface.resources->unregister_skill_dir(host, agentxx_plugin_sv_cstr(p.c_str())) == 0
                          : iface.resources->unregister_memory_file(host, agentxx_plugin_sv_cstr(p.c_str())) == 0;
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
            char* nsEsc  = iface.json->json_escape(host, agentxx_plugin_sv(ns.data(), ns.size()));
            char* urlEsc = iface.json->json_escape(host, agentxx_plugin_sv(url.data(), url.size()));
            if (!nsEsc || !urlEsc) {
                if (nsEsc) vt.free(nsEsc);
                if (urlEsc) vt.free(urlEsc);
                return JS_ThrowInternalError(ctx, "addMcpServer: escape failed");
            }
            std::string spec = std::string("{\"namespace\":") + nsEsc + ",\"url\":" + urlEsc;
            vt.free(nsEsc);
            vt.free(urlEsc);
            long long t = static_cast<long long>(timeoutSec < 0 ? 0 : timeoutSec);
            spec += ",\"timeout\":" + std::to_string(t);
            spec += "}";
            if (iface.resources->register_mcp_server(host, agentxx_plugin_sv_cstr(spec.c_str())) != 0) {
                return throwJsError(ctx, "addMcpServer register failed (conflict?): " + ns);
            }
            return JS_TRUE;
        }

        case B_REMOVE_MCP_SERVER: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "removeMcpServer: namespace required");
            }
            std::string ns = jsToCppString(ctx, argv[0]);
            return iface.resources->unregister_mcp_server(host, agentxx_plugin_sv(ns.data(), ns.size())) == 0
                       ? JS_TRUE
                       : JS_FALSE;
        }

        default:
            return JS_ThrowInternalError(ctx, "unknown bridge magic %d", magic);
    }
}

// =====================================================================
// 插件入口 (宿主 dlsym)
// =====================================================================

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_javascript_engine"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("JS interpreter plugin (QuickJS): hosts type:js plugins"),
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
static void* jsCapStart(
    void*                   ctx,
    const AgentxxHost*      caller_host,
    AgentxxPluginStringView method,
    AgentxxPluginStringView args_json,
    const AgentxxOpNotify*  notify,
    char**                  error_out
) {
    auto* engine = static_cast<JsEngine*>(ctx);
    auto  setErr = [&](const char* msg) {
        if (error_out && caller_host) {
            *error_out = caller_host->vtable->strdup(msg);
        }
        return nullptr;
    };
    if (!engine || !notify || agentxx_plugin_sv_empty(method)) {
        return setErr("interpreter.js: invalid invoke");
    }
    std::string methodStr{method.data, method.size};
    std::string argsStr{args_json.data ? args_json.data : "{}", args_json.size};
    // 参数解析经宿主 client 无关的 agentxx.agent.json 接口表 (对转义/嵌套结构可靠;
    // caller_host 与本插件同进程, 接口表为同一批静态数据)
    const agentxx::plugin::AgentIfaces callerIf = agentxx::plugin::AgentIfaces::query(caller_host);
    if (!callerIf.json || !callerIf.json->json_get_string) {
        return setErr("interpreter.js: host lacks agentxx.agent.json interface");
    }
    auto argStr = [&](const char* key, std::string& out) -> bool {
        char* v = callerIf.json->json_get_string(
            caller_host,
            agentxx_plugin_sv(argsStr.data(), argsStr.size()),
            agentxx_plugin_sv(key, std::strlen(key))
        );
        if (!v) {
            return false;
        }
        out = v;
        caller_host->vtable->free(v);
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
        if (!engine->post([engine, caller_host, notify, name, path, code]() {
                std::string err2;
                const int   rc2 = engine->loadScriptOnJsThread(caller_host, name, path, code, err2);
                const AgentxxHost* eh = engine->host();
                if (rc2 != 0) {
                    char* payload = eh ? eh->vtable->strdup(err2.c_str()) : nullptr;
                    notify->done(notify->host_ud, AGENTXX_OP_FAILED, payload);
                    return;
                }
                std::string tools = engine->loadedToolsJsonOnJsThread(name);
                char* payload = eh ? eh->vtable->strdup(("{\"ok\": true, \"tools\": " + tools + "}").c_str()) : nullptr;
                notify->done(notify->host_ud, AGENTXX_OP_OK, payload);
            })) {
            return setErr("interpreter.js engine stopped");
        }
        // 活动 op 占位 (宿主只等完成通知; execute_poll 留 NULL)
        static int kCapOpSentinel = 0;
        return &kCapOpSentinel;
    }

    if (methodStr == "unload") {
        std::string name;
        if (!argStr("name", name) || name.empty()) {
            return setErr("interpreter.js unload: name (string) required");
        }
        engine->unloadScript(name.c_str()); // 投递式
        char* payload
            = engine->host() ? engine->host()->vtable->strdup("{\"ok\": true}") : nullptr;
        notify->done(notify->host_ud, AGENTXX_OP_OK, payload);
        return nullptr; ///< 内联完成
    }

    setErr(("interpreter.js: unknown method `" + methodStr + "`").c_str());
    return nullptr;
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    auto* engine = new JsEngine();
    engine->setEngineHost(host);

    // COM 风格接口表查询: entry 内一次性查询缓存全部已知 IID
    static const agentxx::plugin::AgentIfaces s_if = agentxx::plugin::AgentIfaces::query(host);
    if (!s_if.capabilities || !s_if.capabilities->register_capability_ex || !s_if.log) {
        delete engine;
        return -1;
    }

    // 注册能力 "interpreter.js" (agentxx.agent.capabilities 接口表, 异步方法
    // 处理器三件套): 脚本插件 (C++ 壳) 经 invoke_capability(_async) 把脚本代码
    // 交给本引擎执行 —— 插件间通信, 宿主不参与; load 为异步完成 (JS 线程执行),
    // unload 内联完成 (fire-and-forget)
    int rc = s_if.capabilities->register_capability_ex(
        host,
        AGENTXX_SV("interpreter.js"),
        &jsCapStart,
        nullptr,
        nullptr,
        engine
    );
    if (rc != 0) {
        delete engine;
        return -1;
    }
    *plugin_ctx = engine;
    s_if.log->log(host, 2, AGENTXX_SV("agentxx_javascript_engine loaded (QuickJS interpreter.js)"));
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* plugin_ctx) {
    auto* engine = static_cast<JsEngine*>(plugin_ctx);
    if (engine) {
        delete engine; // 停止 JS 线程并释放 runtime
    }
}
