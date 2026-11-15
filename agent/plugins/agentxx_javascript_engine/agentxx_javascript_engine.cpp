/// agentxx_javascript_engine —— JS 解释器插件 (二期)
///
/// 功能: 注册 "interpreter.js" 脚本引擎, 负责加载/卸载 type: js 的 JS 插件
///
/// 线程模型 (关键设计, 统一异步操作模型):
/// - 专用 JS 线程: 所有 QuickJS 操作集中于该线程 (QuickJS 非线程安全)
/// - 任务队列 (互斥锁 + 条件变量): post 非阻塞投递
/// - 工具 execute / 能力 load: 主程序 io 线程 start 入队 JS 线程后立即返回,
///   JS 线程执行完毕经 PluginxxOperatorNotify 上报完成 (线程安全) —— 全程无阻塞
///   等待, 主程序 io 协程与 JS 任务交错执行; 旧版 postSync 阻塞桥已移除
/// - JS 线程 → io 线程: host vtable 内部经 post_to_io + 同步等待 (主程序实现)
/// - 通知/事件回调: io 线程 → post 到 JS 线程 (fire-and-forget, 不等待)
/// - JS 内 callTool 命中本引擎工具: 同线程内联执行 (防自锁)
/// - 卸载安全: JsPluginCtx 由 shared_ptr 管理; 跨线程经 mirror 表 (互斥锁) 查
///   强引用; 插件卸载 (deleted) 后已入队任务检查标志跳过; JSContext 释放由
///   JsPluginCtx 析构完成 (全部进行中的任务结束后)
///
/// 沙箱: 内存限制 (JS_SetMemoryLimit) + 栈限制 + 指令中断超时; 不引入
/// quickjs-libc (无 os/std 模块); 全局仅注入标准 ECMA 内置 + agentxx 桥
#include "agentxx/plugin/api/agentxx_point_ids.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "fmt/format.h"
#include "quickjs.h"

#include <algorithm>
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
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

class JsEngine;

namespace {

// =====================================================================
// C ABI 边界异常守卫 (由守卫函数调用处显式传入; entry 加载缓存)
// =====================================================================

/// JS 专用线程栈大小: 显式 8MB (与 Linux 默认一致)
///
/// - macOS/iOS 的 pthread 默认线程栈仅 **512KB**, Windows 1MB; 而 quickjs
///   解析/执行使用纯 C 递归 (`js_parse_expr_binary` 等), 其自身栈检查
///   (见 [kStackLimit]) 只在部分入口生效 —— 真实线程栈不足时会在检查触发前
///   先耗尽导致栈溢出崩溃 (实测 Apple Silicon + ASan 下解析稍复杂脚本即触发)。
/// - 统一显式指定 8MB, 使 quickjs 的栈检查成为唯一的深度上限, 跨平台一致。
constexpr size_t kWorkerStackBytes = 8 * 1024 * 1024;

/// 专用 JS 线程的所有权包装 (自定义栈大小; 仅负责句柄, 不改变 join/detach 语义)
class JsWorkerThread {
public:

    JsWorkerThread()                                 = default;
    JsWorkerThread(const JsWorkerThread&)            = delete;
    JsWorkerThread& operator=(const JsWorkerThread&) = delete;

    ~JsWorkerThread() {
        if (joinable()) {
            detach();
        }
    }

    /// 启动线程 (entry(arg), 兼容 pthread 的 void* 返回); 失败返回 false
    bool start(void* (*entry)(void*), void* arg) {
#if XX_IS_WIN_D
        entry_            = entry;
        arg_              = arg;
        const uintptr_t h = _beginthreadex(
            nullptr,
            static_cast<unsigned>(kWorkerStackBytes),
            [](void* self) -> unsigned {
                auto* t = static_cast<JsWorkerThread*>(self);
                t->entry_(t->arg_);
                return 0;
            },
            this,
            0,
            nullptr
        );
        if (h == 0) {
            return false;
        }
        handle_ = reinterpret_cast<HANDLE>(h);
        return true;
#else
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) {
            return false;
        }
        if (pthread_attr_setstacksize(&attr, kWorkerStackBytes) != 0) {
            pthread_attr_destroy(&attr);
            return false;
        }
        const int rc = pthread_create(&handle_, &attr, entry, arg);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            return false;
        }
        joinable_ = true;
        return true;
#endif
    }

    bool joinable() const {
#if XX_IS_WIN_D
        return handle_ != nullptr;
#else
        return joinable_;
#endif
    }

    void join() {
#if XX_IS_WIN_D
        if (handle_) {
            ::WaitForSingleObject(handle_, INFINITE);
            ::CloseHandle(handle_);
            handle_ = nullptr;
        }
#else
        if (joinable_) {
            pthread_join(handle_, nullptr);
            joinable_ = false;
        }
#endif
    }

    void detach() {
#if XX_IS_WIN_D
        if (handle_) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
        }
#else
        if (joinable_) {
            pthread_detach(handle_);
            joinable_ = false;
        }
#endif
    }

private:

#if XX_IS_WIN_D
    HANDLE handle_         = nullptr;
    void* (*entry_)(void*) = nullptr;
    void* arg_             = nullptr;
#else
    pthread_t handle_{};
    bool      joinable_ = false;
#endif
};

} // namespace

namespace {

constexpr size_t kMemoryLimit = 64 * 1024 * 1024; ///< JS 内存上限 64MB
/// JS 栈上限 (quickjs 自身检查; 必须小于 [kWorkerStackBytes])
/// - 取 4MB 而非更小值: quickjs 解析/执行使用 C 递归, 插桩构建 (ASan) 下栈帧
///   显著放大, 512KB 会在合法脚本上误报 "Maximum call stack size exceeded"。
constexpr size_t  kStackLimit         = 4 * 1024 * 1024;
constexpr size_t  kTaskTimeoutMs      = 60000;  ///< 单任务 (工具执行等) 超时
constexpr int64_t kPromiseWaitLimitMs = 120000; ///< Promise 等待上限 (绝对截止时间)

/// client 侧同步回调的等待上限 (io 线程等 JS 线程)
///
/// 渲染内容回调与命令 execute 都要求在主程序 io 线程同步返回结果, 而 JS 只在 JS
/// 线程执行 —— 只能阻塞等待。取一个短上限: 超时就当"这次没有结果" (渲染回退默认
/// 内容 / 命令按失败处理), 不让界面卡在脚本上。脚本里做耗时计算应改为不依赖同步
/// 回调的方式 (例如订阅事件后用 `updateToolDecor` 推数据)。
constexpr int kClientSyncTimeoutMs = 150;

/// 每个任务/定时器之后最多泵多少次 Promise 微任务
/// - 业务回调通常不等待 Promise: `netFetch(...).then(...)` 的续体必须在这里被泵出去,
///   否则永远不会执行 (主循环不在其他地方执行 pending job)
/// - 有上限: 脚本里互相调度的死循环不能卡住 JS 线程 (到限就记一条警告)
constexpr int kMaxJobsPerDrain = 10000;

/// JS 面的通知点序号 → 通知型功能埋点 id
///
/// 通知点已经是"通知型功能埋点": 脚本里 `agentxx.onHook(point, fn)` 等价于订阅同名
/// 广播频道, 序号约定保持不变 (0 = 轮次开始 … 6 = 工具结束), 与
/// `agentxx/plugin/api/agentxx_point_ids.h` 的 7 个通知型点一一对应。
std::string_view hookPointIdOf(int32_t point) {
    switch (point) {
        case 0:
            return agentxx::plugin::points::TurnStart;
        case 1:
            return agentxx::plugin::points::TurnEnd;
        case 2:
            return agentxx::plugin::points::ModelStart;
        case 3:
            return agentxx::plugin::points::ModelRun;
        case 4:
            return agentxx::plugin::points::ModelEnd;
        case 5:
            return agentxx::plugin::points::ToolStart;
        case 6:
            return agentxx::plugin::points::ToolEnd;
        default:
            return {};
    }
}

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
    // 会话资源扩展 (主程序 plugin_api v8)
    B_ADD_SKILL_DIR,
    B_REMOVE_SKILL_DIR,
    B_ADD_MEMORY_FILE,
    B_REMOVE_MEMORY_FILE,
    B_ADD_MCP_SERVER,
    B_REMOVE_MCP_SERVER,
    // 与 C++ 插件 SDK 对齐的查询/声明类接口 (命名同 plugin_kit.h)
    B_SCOPE,
    B_GET_CONFIG,
    B_GET_PLUGIN_ARGS,
    B_GET_LANGUAGE,
    B_SET_LANGUAGE,
    B_REGISTER_TOOL_PERMISSION,
    B_UNREGISTER_TOOL_PERMISSION,
    B_REGISTER_READ_PATH_PERMISSION,
    B_REGISTER_WRITE_PATH_PERMISSION,
    // 网络 (`agentxx.net`): 插件不自己建连接, 一律经主程序的网络服务
    B_NET_CALL,
    B_NET_CALL_ASYNC,
    B_NET_CANCEL,
    B_NET_FETCH,
    B_NET_DOWNLOAD,
    B_NET_WS_CONNECT,
    B_NET_WS_SEND,
    B_NET_WS_CLOSE,
    B_NET_HTTP_SERVE,
    B_NET_HTTP_RESPOND,
    B_NET_INFO,
    // 后台任务 (`agentxx.agent.runtime` v2): 提交 / 取消 / 查询
    B_SUBMIT_ASYNC_WORK,
    B_CANCEL_ASYNC_WORK,
    B_GET_ASYNC_WORK,
    B_LIST_ASYNC_WORK,
    // 提示词 / 会话 / 上下文 / 执行图 / 挂载查询 (与 C++ 插件 SDK 同名)
    B_GET_PROMPT,
    B_SET_PROMPT,
    B_GET_TOOL_PROMPT,
    B_GET_SESSION_WORK_DIR,
    B_IS_CANCELED,
    B_GET_MESSAGES,
    B_MESSAGES_COUNT,
    B_GET_GRAPH_JSON,
    B_GET_GRAPH_NAME,
    B_SET_GRAPH_JSON,
    B_LIST_MOUNTS,
    B_GET_MOUNT,
    // 会话运行时 (`agentxx.agent.runtime`): 自建根 / 会话通道 / 触发器 / 定时器
    B_MOUNT_ROOT,
    B_UNMOUNT_ROOT,
    B_OPEN_CHANNEL,
    B_CLOSE_CHANNEL,
    B_SEND_INPUT,
    B_CANCEL_CHANNEL,
    B_SELECT_MODEL,
    B_RESPOND_INTERRUPT,
    B_GET_CHANNEL_STATE,
    B_GET_CHANNEL_MESSAGES,
    B_LIST_RUNTIME,
    B_REGISTER_TRIGGER,
    B_UNREGISTER_TRIGGER,
    B_REGISTER_TIMER,
    B_UNREGISTER_TIMER,
    B_EMIT_EVENT,
    // 广播声明 / 读当前值 / 结束流, 会话 share_store 写入, 资源与模型查询
    B_DECLARE_BROADCAST,
    B_READ_BROADCAST,
    B_END_BROADCAST,
    B_ADD_SHARE_STORE,
    B_GET_OWN_RESOURCES,
    B_GET_MODEL_CONFIG,
    // 能力表 (`agentxx.capabilities`): 登记实现行 / 注销 / 自省 / 调用
    // - 脚本侧只开"异步处理器"形态: 脚本回调跑在 JS 线程, 结果经 notify 回主程序
    B_PROVIDE_CAPABILITY,
    B_UNPROVIDE_CAPABILITY,
    B_PROVIDE_POINT,
    B_LIST_CAPABILITIES,
    B_CAPABILITY_INFO,
    B_INVOKE_CAPABILITY,
    // 本插件信息 / 配置文件路径 / 路径权限三态查询 (agent 侧; 与 C++ 插件 SDK 同名)
    B_GET_OWN_INFO,
    B_GET_CONFIG_PATH,
    B_CHECK_PATHS,
    B_FILTER_PATHS,
    // client 侧桥 (`agentxx.side == "client"`)。与 agent 侧同义的方法复用上面的
    // magic (log / getPluginArgs / getLanguage / setLanguage / subscribe /
    // unsubscribe / publish / setTimeout / clearTimeout), 其余在这里追加
    B_CLIENT_GET_OWN_INFO,
    B_CLIENT_GET_CONFIG_PATH,
    B_CLIENT_GET_STATE,
    B_CLIENT_SEND_INPUT,
    B_CLIENT_REQUEST_CANCEL,
    B_CLIENT_SEND_PLUGIN_DATA,
    B_CLIENT_SHOW_TOAST,
    B_CLIENT_UPDATE_TOOL_DECOR,
    B_CLIENT_REGISTER_TOOL_RENDERER,
    B_CLIENT_UNREGISTER_TOOL_RENDERER,
    B_CLIENT_OPEN_OVERLAY,
    B_CLIENT_CLOSE_OVERLAY,
    // client 侧展示面 (状态栏项 / 面板 / Info 段落 / 命令 / 动作绑定 / 定时器 / 快捷键)
    B_CLIENT_REGISTER_STATUS_ITEM,
    B_CLIENT_UPDATE_STATUS_ITEM,
    B_CLIENT_UNREGISTER_STATUS_ITEM,
    B_CLIENT_REGISTER_PANEL,
    B_CLIENT_UPDATE_PANEL,
    B_CLIENT_UNREGISTER_PANEL,
    B_CLIENT_REGISTER_INFO_SECTION,
    B_CLIENT_UPDATE_INFO_SECTION,
    B_CLIENT_UNREGISTER_INFO_SECTION,
    B_CLIENT_REGISTER_COMMAND,
    B_CLIENT_UNREGISTER_COMMAND,
    B_CLIENT_BIND_ACTION,
    B_CLIENT_UNBIND_ACTION,
    B_CLIENT_SET_TIMER,
    B_CLIENT_CANCEL_TIMER,
    B_CLIENT_IS_REGION_VISIBLE,
    B_CLIENT_REGISTER_KEYBIND,
    B_CLIENT_UNREGISTER_KEYBIND,
    B_CLIENT_LIST_KEYBINDS,
};

/// 工具执行请求/结果 (execute 桥跨线程传递)
struct ToolExecReq {
    std::string args, tid, tcid;
    std::string result, error;
    /// Promise 被拒绝或超时: 工具调用必须映射为 FAILED (不再当成成功文本)
    bool failed = false;
    /// 引擎停止/取消: 工具调用映射为 Canceled
    bool Canceled = false;
    /// 插件工具返回 `{status, text}` 时的终态 (默认 OK)
    /// - `ok` / `failed` / `interrupted` / `canceled`, 未知取值按 `ok`
    int32_t     status = PLUGINXX_OPERATOR_OK;
    bool done          = false;
};

/// 工具绑定 (注册工具时创建, 由 JsPluginCtx 持有; execute 回调期间存活
/// 由主程序 inflight 计数保证)
struct JsToolBinding {
    JsEngine*   engine = nullptr;
    /// 所属实例键 (脚本名 + 挂载作用域): 同名工具在不同 agent 上各有自己的绑定
    std::string instanceKey;
    std::string name;
};

/// 功能埋点/广播绑定 (订阅时创建, 由 JsPluginCtx 持有; 回调期间存活由主程序
/// inflight 计数保证)
struct JsHookBinding {
    JsEngine*   engine = nullptr;
    /// 所属实例键 (脚本名 + 挂载作用域)
    std::string instanceKey;
    /// 订阅的频道全名 (派发时按它过滤: 一次派发只调该频道的处理器)
    std::string topic;
    /// 本订阅在实例订阅表 (`JsPluginCtx::agents`) 里的下标
    /// - `>= 0` 时只调这一条: 同一频道多个订阅各收一次, 不会互相重复调用
    /// - `-1` = 按 topic 匹配全部 (旧行为/兜底)
    int64_t token = -1;
};

/// 一条由脚本实现的接口表能力行 (功能埋点实现行也是它)
///
/// - 由引擎分配并保留到引擎析构: 主程序把本对象地址作为处理器的 `user_data`,
///   可能在任意线程读 `fnToken` / `dead` (故为原子); 摘除时只标失效不释放,
///   避免还在飞的调用碰到已释放内存
/// - 脚本函数本体按 `fnToken` 存在实例的 `capCallbacks` 里 (JSValue 只在 JS 线程释放)
struct JsCapImpl {
    JsEngine*            engine = nullptr;
    std::string          instanceKey;
    std::string          capId;    ///< 注册时写的 id (短名或全名)
    std::string          ownerTag; ///< 同一实例内区分多行 (可空)
    /// 功能埋点形态: 返回值按 `pointAnswer` 规范化 (`{"value": …}` / `{"disable": true}`)
    bool                 pointMode = false;
    std::atomic<int64_t> fnToken{0};
    std::atomic<bool>    dead{false};
};

/// 一次能力调用的跨线程记账 (处理器启动时建, 结束时销毁)
///
/// 主程序把本对象地址作为 op 句柄回传 `capImplCancel`, 因此必须在结束前一直存活;
/// `settled` 保证完成通知恰好一次 (正常完成 / 取消 / 引擎停止三条路径).
struct JsCapCall {
    JsEngine*                engine = nullptr;
    std::string              instanceKey;
    std::string              capId;
    int64_t                  fnToken = 0;
    std::string              argsJson;
    bool                     pointMode = false;
    PluginxxOperatorNotify   notify{};
    std::atomic<bool>        settled{false};
};

/// 一次 `invokeCapability` 的跨线程完成桥 (与 [JsNetBridge] 同构)
/// - 主程序完成回调在 io 线程, 只复制结果并向 JS 线程投递 settle 任务
/// - Promise 总是 resolve 成结果对象 `{ok, status, value|error}`, 不 reject
struct JsCapInvokeBridge {
    JsEngine*         engine  = nullptr;
    JSContext*        ctx     = nullptr;
    JSValue           resolve = JS_UNDEFINED;
    JSValue           reject  = JS_UNDEFINED;
    std::atomic<bool> settled{false};
};

/// JS 内 callTool 的跨线程完成桥:
/// - 主程序工具完成回调运行在主程序 io 线程, 只复制结果并向 JS 线程投递 settle 任务
/// - resolve/reject 均为 JS 线程所属的 JSValue, 只在 JS 线程 Free
/// - settled 原子标志保证 exactly-once (主程序协议已保证, 这里补充处理)
struct JsCallBridge {
    JsEngine*         engine  = nullptr;
    JSContext*        ctx     = nullptr;
    JSValue           resolve = JS_UNDEFINED;
    JSValue           reject  = JS_UNDEFINED;
    std::atomic<bool> settled{false};
};

/// JS 内 netCallAsync 的跨线程完成桥 (与 [JsCallBridge] 同构)
/// - 完成回调在主程序 io 线程, 只复制结果并向 JS 线程投递 settle 任务
/// - 网络失败不是异常: Promise 总是 resolve 成结果对象, 业务结论看 `ok`
///   (与 C++ 侧 `NetResult` 的用法一致)
struct JsNetBridge {
    JsEngine*            engine  = nullptr;
    JSContext*           ctx     = nullptr;
    JSValue              resolve = JS_UNDEFINED;
    JSValue              reject  = JS_UNDEFINED;
    std::atomic<bool>    settled{false};
    std::atomic<int64_t> requestId{0}; ///< 受理时给出的请求号 (之后用于 netCancel)
};

/// 主程序回调 → JS 线程的登记 (后台任务事件 / 会话通道事件共用)
/// - 与 JS 实例生命周期解耦: 实例先销毁时事件在 JS 线程查不到回调, 直接丢弃
/// - 终态事件 (后台任务 finished/canceled、通道 closed) 到达时释放;
///   引擎销毁时清理尚未收到终态的残留
struct JsCallbackHook {
    JsEngine*   engine = nullptr;
    std::string instanceKey;
    int64_t     token = 0; ///< JS 侧回调表的键
};

/// 一次同步渲染调用 (client io 线程创建, 交给 JS 线程执行)
///
/// - 输入是主程序给的只读借用视图, 跨线程后生命周期不保证: 必须在这里拷一份
/// - 槽在堆上 (`shared_ptr`): io 线程超时返回后任务仍持有它, 不会写已销毁的栈
struct JsRenderSync {
    std::mutex              m;
    std::condition_variable cv;
    bool                    done      = false; ///< JS 线程已写入结果
    std::atomic<bool>       abandoned{false};  ///< 调用方已超时: 任务不必再执行
    bool                    collapsed = true;  ///< true = 折叠内容, false = 展开内容
    // 输入拷贝
    std::string toolCallId;
    std::string toolName;
    std::string argsJson;
    std::string resultText;
    bool        finished = false;
    bool        isError  = false;
    int32_t     maxWidth = 0;
    // 输出
    bool        failed = false; ///< 脚本抛异常 / 形态不对 / 引擎已停
    std::string text;           ///< 折叠内容文本, 或展开内容的 items JSON
};

/// client 侧工具特化渲染注册项
///
/// - 注册/注销在 JS 线程, 回调查找在 client io 线程
/// - **不含 JSValue**: 脚本函数按 token 存在 `JsPluginCtx::clientCallbacks` 里,
///   因此本对象可被任意线程释放 (满足"JSValue 只在 JS 线程释放")
/// - 生命周期覆盖引擎: 它作为 `user_data` 被主程序持有, 摘除时移进
///   `retiredRenders_` 而不就地释放
struct JsRenderReg {
    JsEngine*   engine = nullptr;
    std::string instanceKey;
    std::string toolName;
    /// 脚本函数 token (0 = 脚本没给该回调)
    /// - 原子: io 线程的回调会读它们, JS 线程会写 (抹零)
    std::atomic<int64_t> collapsedToken{0};
    std::atomic<int64_t> expandedToken{0};
    std::atomic<bool>    dead{false}; ///< JS 侧已释放: 回调直接失败
};

/// client 侧脚本回调登记 (命令 / 动作 / 定时器 / 快捷键共用)
///
/// - 主程序以 `user_data` 持有本结构: 由引擎分配并保留到引擎析构
///   (`clientCbs_`), 脚本卸载时只标 `dead`, 不就地释放
/// - 脚本函数本体按 `fnToken` 存在实例的 `clientCallbacks` 里,
///   因此这里同样不含 JSValue
struct JsClientCallback {
    JsEngine*            engine = nullptr;
    std::string          instanceKey;
    /// 脚本函数 token (0 = 已注销); 原子: io 线程的回调会读
    std::atomic<int64_t> fnToken{0};
    std::atomic<bool>    dead{false};
    /// 展示用标签 (命令名 / 归属 id / 键位), 只作日志与回调参数
    std::string label;
};

/// 一次同步脚本回调 (命令 execute; io 线程要求同步返回动作 JSON)
struct JsCallbackSync {
    std::mutex              m;
    std::condition_variable cv;
    bool                    done = false;
    std::atomic<bool>       abandoned{false};
    std::string             argsJson;
    bool                    failed = false;
    std::string             text; ///< 脚本返回值 (对象/数组已序列化)
};

/// client 侧句柄类别 (`JsPluginCtx::ClientHandle::kind`)
///
/// client 表用不透明指针做句柄; JS 的 number 存指针会丢精度, 所以换成整数 token,
/// 按类别查回真实指针。
enum class ClientHandleKind : int32_t {
    StatusItem  = 1, ///< AgentxxStatusItem*
    Panel       = 2, ///< AgentxxPanel*
    InfoSection = 3, ///< AgentxxInfoSection*
    Timer       = 4, ///< AgentxxTimer*
    Keybind     = 5, ///< AgentxxKeybind*
};

/// JS 插件上下文 (生命周期: plugins_ / mirror / 进行中任务共享持有)
/// - 一个实例 = 一份 (脚本, 挂载作用域) 组合: 每个 agent 各自一份 JSContext,
///   脚本里的全局状态因而天然按 agent 隔离 (与 C++ 插件"一插件多 agent 挂载"一致)
/// - 数据成员仅 JS 线程访问 (deleted/inflight 亦仅 JS 线程, 任务串行)
/// - JSContext 释放: 析构函数 (全部进行中的任务结束后由最后一个持有者析构)
struct JsPluginCtx {
    std::string name;        ///< 脚本 (JS 插件) 名
    std::string instanceKey; ///< 实例键 (脚本名 + '\x1f' + 作用域键)
    JSContext*          ctx      = nullptr;
    const PluginxxHost* host     = nullptr; ///< 调用方 (JS 插件薄壳) 的主程序句柄
    JsEngine*           engine   = nullptr;
    bool                deleted  = false; ///< 已卸载 (入队任务检查后跳过)
    size_t              inflight = 0;     ///< 进行中的任务数 (execute/hook/event)
    /// 本实例所属挂载作用域 (agent): 注册/查询类调用都要带它
    std::string      scopeAgentId;
    uint64_t         scopeSeq = 0;
    PluginxxScopeRef scopeRef{};
    /// 工具表: 普通对象 name -> {execute, name, description}
    JSValue tools = JS_UNDEFINED;
    /// 通知回调表: Array(7) 元素为 fn 或 null
    /// 订阅表: Array of {topic, handler, token}
    JSValue                                     agents = JS_UNDEFINED;
    std::vector<std::unique_ptr<JsToolBinding>> toolBindings;
    std::vector<std::unique_ptr<JsHookBinding>> hookBindings;
    /// 后台任务事件回调: token -> fn (提交时登记; 随上下文释放)
    std::map<int64_t, JSValue> asyncWorkCallbacks;
    /// 会话通道事件回调: token -> fn (开通道时登记; 随上下文释放)
    std::map<int64_t, JSValue> channelCallbacks;
    /// 能力实现行回调: token -> fn (provideCapability / providePoint 时登记)
    /// - 引擎侧的能力行只记 token, JSValue 始终由本实例拥有, 只在 JS 线程释放
    std::map<int64_t, JSValue> capCallbacks;
    /// 能力实现行回调 token 自增源 (仅 JS 线程使用)
    int64_t nextCapToken = 0;
    /// client 侧工具特化渲染回调与其它脚本回调: token -> fn
    /// (注册渲染器 / 命令 / 动作 / 定时器 / 快捷键时登记; 随上下文释放)
    /// - 引擎侧只记 token, JSValue 始终由本实例拥有
    std::map<int64_t, JSValue> clientCallbacks;
    /// client 侧广播订阅: token -> 主程序订阅句柄 (退订时用)
    /// client 侧广播订阅: token -> {主程序订阅句柄, 实例订阅表下标}
    /// - 句柄用于退订; 下标用于把订阅表里的处理器置空 (派发按它找回调)
    struct ClientSub {
        PluginxxSubscription* sub   = nullptr;
        int64_t               entry = -1;
    };
    std::map<int64_t, ClientSub> clientSubs;
    /// client 侧展示项/定时器/快捷键句柄: token -> 句柄
    /// - client 表用不透明指针作句柄, JS 数字精度不够直接存指针, 因此换 token
    /// - 只存地址: 句柄本身由主程序持有 (实例卸载时释放)
    struct ClientHandle {
        int32_t     kind   = 0; ///< ClientHandleKind
        void*       handle = nullptr;
        std::string id;         ///< 注册时的 id / keys (注销与日志用)
    };
    std::map<int64_t, ClientHandle> clientHandles;
    /// client 侧回调登记: 键 (`cmd:<名>` / `act:<归属>` / `timer:<token>` / `keybind:<token>`)
    /// -> 回调项; 注销时据此标失效并释放脚本函数
    std::map<std::string, JsClientCallback*, std::less<>> clientCbByKey;
    /// 回调 token 自增源 (仅 JS 线程使用)
    int64_t nextCallbackToken = 0;
    /// 脚本初始化事务的撤销动作 (注册工具/通知/订阅/资源成功时按序追加)。
    /// 顶层执行失败或脚本卸载时逆序执行, 主程序侧不留任何残留注册。
    std::vector<std::function<void()>> rollbackActions;

    /// 填挂载作用域 (agent_id 拷入自有存储, scopeRef.id 指向它, 有效期同本对象)
    void setScope(std::string_view agentId, uint64_t seq) {
        scopeAgentId.assign(agentId.data(), agentId.size());
        scopeSeq             = seq;
        scopeRef             = PluginxxScopeRef{};
        scopeRef.struct_size = static_cast<uint32_t>(sizeof(PluginxxScopeRef));
        scopeRef.flags       = 0;
        scopeRef.id = agentxx::plugin::PluginStringView::from(
            scopeAgentId.data(),
            scopeAgentId.size()
        );
        scopeRef.seq = scopeSeq;
    }

    /// 本实例的挂载引用 (未设作用域时为 nullptr)
    const PluginxxScopeRef* scope() const {
        return scopeAgentId.empty() ? nullptr : &scopeRef;
    }

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

    /// 引擎生命周期状态 (create 只构造; runtime 与 JS 线程属于 start 事务)
    enum class State {
        Stopped,  ///< 未启动或已完全停止 (无 JS 线程、无 runtime、无收尾线程)
        Running,  ///< 运行中: 接受 post
        Stopping, ///< 停止中: 拒绝新 post, 收尾线程正在 join JS 线程/释放 runtime
    };

    /// 本引擎服务的插件侧
    ///
    /// - 同一动态库导出两套入口 (`agentxx_plugin_agent_*` / `agentxx_plugin_client_*`),
    ///   各自 `new JsEngine(Side)`
    /// - 引擎内部只有**桥**与**实例作用域**按侧区分; JS 线程、runtime、实例表、
    ///   定时器、停止收尾全部共用一套
    enum class Side {
        Agent,  ///< agent 侧: 脚本用 agent 表, 每个 agent 挂载一份实例
        Client, ///< client 侧: 脚本用 client 表, 没有挂载作用域 (一份脚本一个实例)
    };

    explicit JsEngine(Side side = Side::Agent) :
        side_(side) {}

    /// 本引擎服务的插件侧
    Side side() const noexcept {
        return side_;
    }

    /// 是否 client 侧 (桥与实例作用域按它分流)
    bool isClientSide() const noexcept {
        return side_ == Side::Client;
    }

    /// 析构: 只处理已经安全停止 (或可以同步收尾) 的对象。
    /// 主程序协议要求 destroy 前 stop 已完成, 正常路径此处是空操作。
    ~JsEngine() {
        stopAndWait();
        // 停止后再无主程序回调, 残留的回调登记可以安全释放
        // (未收到终态的任务在插件 stop 时已被主程序取消, 事件到此为止)
        std::set<JsCallbackHook*> rest;
        {
            std::lock_guard<std::mutex> lk(callbackMtx_);
            rest.swap(callbackHooks_);
        }
        for (auto* hook : rest) {
            delete hook;
        }
    }

    // ==================== 生命周期 (start / stop) ====================

    // ==================== 挂载作用域 (每个 agent 一份) ====================

    /// 一个挂载作用域: agent 标识 + 挂载序号 (序号防 id 复用)
    struct MountScope {
        std::string key;     ///< 作用域键: `<agentId>#<seq>`
        std::string agentId; ///< agent 标识 (`root` / `sub:3` / `ext:qq:1` ...)
        uint64_t    seq = 0;
    };

    /// mount 返回的 per-agent 状态 (主程序原样交回 unmount)
    struct ScopeHandle {
        MountScope scope;
    };

    /// 从挂载参数组装作用域; 无效参数返回空 key
    static MountScope mountScopeOf(const AgentxxPluginAgentMountSpec* spec) {
        MountScope sc;
        if (!spec || !spec->agent_id.data || spec->agent_id.size == 0) {
            return sc;
        }
        sc.agentId.assign(spec->agent_id.data, static_cast<size_t>(spec->agent_id.size));
        sc.seq = spec->mount_seq;
        sc.key = fmt::format("{}#{}", sc.agentId, sc.seq);
        return sc;
    }

    /// client 侧的固定伪作用域
    ///
    /// client 表成员签名里没有 `PluginxxScopeRef`, 但实例键要区分脚本与挂载位置。
    /// 这里给一个固定键 `client`, agentId 留空 —— `JsPluginCtx::scope()` 因此返回
    /// `nullptr`, 所有表调用都按"无作用域"处理 (与 C++ client 插件一致)。
    static MountScope clientScope() {
        MountScope sc;
        sc.key = "client";
        return sc;
    }

    /// 登记挂载作用域, 并为每个已登记脚本建一份该 agent 的实例
    /// - **仅 io 线程调用** (来自插件的 mount 回调)
    /// - 重复挂载同一作用域幂等; 实例创建投递到 JS 线程执行, 不阻塞 mount
    void attachMountScope(const MountScope& scope) {
        if (scope.key.empty() || scopes_.contains(scope.key)) {
            return;
        }
        const std::string key = scope.key;
        scopes_.emplace(key, scope);
        logAt(2, fmt::format(
                      "interpreter.js: mount scope {} (scripts={}, scopes={})",
                      key,
                      scripts_.size(),
                      scopes_.size()
        )
                      .c_str());
        // 每个脚本都为本 agent 建一份实例 (脚本的全局状态因此按 agent 隔离)
        for (const auto& [name, entry] : scripts_) {
            (void)name;
            post([this, entryCopy = entry, scopeCopy = scope]() {
                std::string err;
                if (createInstanceOnJsThread(entryCopy, scopeCopy, err) != 0) {
                    logAt(3, fmt::format(
                                  "interpreter.js: create instance `{}`@{} failed: {}",
                                  entryCopy.name,
                                  scopeCopy.key,
                                  err
                    )
                                  .c_str());
                }
            });
        }
    }

    /// 注销挂载作用域并销毁该 agent 的全部脚本实例
    /// - **仅 io 线程调用** (来自插件的 unmount 回调)
    /// - 主程序已按挂载记录撤销工具/权限/提示词等注册, 这里只收拾 JS 侧
    void detachMountScope(const MountScope& scope) {
        if (scope.key.empty() || !scopes_.contains(scope.key)) {
            return;
        }
        const std::string key = scope.key;
        scopes_.erase(key);
        logAt(2, fmt::format(
                      "interpreter.js: unmount scope {} (scopes={})",
                      key,
                      scopes_.size()
        )
                      .c_str());
        post([this, key]() {
            destroyScopeInstancesOnJsThread(key);
        });
    }

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
    /// - 上一次停止的收尾线程必须先结束 (主程序协议: stop 完成才会再次 start),
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
        if (!thread_.start(
                [](void* self) -> void* {
                    static_cast<JsEngine*>(self)->jsThreadMain();
                    return nullptr;
                },
                this
            )) {
            state_ = State::Stopped;
            return false;
        }
        state_ = State::Running;
        return true;
    }

    /// 请求停止并异步等待收尾完成; `onStopped` 在停止真正完成后调用一次
    /// (可能来自收尾线程, 任意线程安全)。
    ///
    /// - 立即生效: 这是最后一次接受新任务之前的状态, 之后 post() 一律失败;
    /// - 队列中尚未开始的 JS 任务按取消/失败终结, 不再执行插件 JS 代码
    ///   (见各任务的 `stopping()` 前置检查);
    /// - 不在调用线程 join JS 线程: JS 线程可能正阻塞在主程序 vtable 调用
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
                // 引擎停止: 脚本登记与挂载作用域一并清空
                // - JS 插件的注册随引擎挂载记录撤销, 脚本实例随 JS 线程退出释放
                // - 重新 start 时插件会重新 load, 旧登记留着会让 load 命中
                //   "plugin already loaded" (disable/enable 轮次会直接失败)
                scripts_.clear();
                scopes_.clear();
                {
                    std::lock_guard<std::mutex> qlk(mtx_);
                    stop_.store(true, std::memory_order_release); ///< post() 从这里起拒绝新任务
                    // JS 线程空闲 (没有在手中的任务、队列为空) 时, 停止标志生效后
                    // 它只会走线程退出清理路径 (不调用主程序 vtable), 因此可以在
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
    /// 主程序在禁用/卸载时统一撤销注册, 引擎 stop 时同步清位。
    bool capabilityRegistered() const {
        return capabilityRegistered_;
    }

    void setCapabilityRegistered(bool registered) {
        capabilityRegistered_ = registered;
    }

    void setEngineHost(const PluginxxHost* host) {
        engineHost_ = host;
        if (host && host->vtable && host->vtable->query_interface) {
            auto iidSv = agentxx::plugin::PluginStringView::fromCstr(PLUGINXX_IFACE_LOG);
            logIface_
                = static_cast<const PluginxxLogIface*>(host->vtable->query_interface(host, &iidSv));
        }
    }

    /// 守卫异常日志 (noexcept; 经本实例主程序接口表输出 —— 多实例契约:
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

    /// 普通运行日志 (level: 1=trace 2=info 3=warn 4=error)
    void logAt(int level, const char* msg) const noexcept {
        agentxx::plugin::logTo(
            engineHost_,
            logIface_,
            level,
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

    /// JS 运行时插件主程序句柄 (供静态桥回调读取; 可空)
    const PluginxxHost* host() const {
        return engineHost_;
    }

    /// 能力 "load" 的活动 op 占位句柄 (每实例独立; 仅作不透明非空 token)
    void* capOpToken() {
        return &capOpToken_;
    }

private:

    /// 主程序 log 接口表缓存 (setEngineHost 加载; 随实例生死)
    const PluginxxLogIface* logIface_ = nullptr;

public:

    // ==================== 脚本 / 实例 / 挂载作用域 ====================

    /// 已交给引擎的一份脚本 (进程级登记; 仅 io 线程访问)
    /// - 一个脚本在**每个挂载作用域**各有一份实例 (`plugins_` 里以实例键区分)
    struct ScriptEntry {
        std::string         name;
        std::string         path;
        std::string         code;
        const PluginxxHost* host = nullptr; ///< 调用方 (JS 插件薄壳) 的主程序句柄
    };

    /// 实例键分隔符: 脚本名里没有 `@`, 作用域键是 `agentId#seq`
    static constexpr char kInstanceKeySep = '@';

    /// 实例键前缀 (脚本名 + 分隔符)
    static std::string instanceKeyPrefix(std::string_view name) {
        std::string out{name};
        out.push_back(kInstanceKeySep);
        return out;
    }

    /// 实例键: 脚本名 + 分隔符 + 作用域键
    static std::string instanceKeyOf(std::string_view name, std::string_view scopeKey) {
        std::string out{name};
        out.push_back(kInstanceKeySep);
        out.append(scopeKey);
        return out;
    }

    /// 能力 op 失败收尾 (完成通知一次 + 错误文本)
    static void failNotify(const PluginxxOperatorNotify& notify, const std::string& msg) {
        if (!notify.done) {
            return;
        }
        auto errSv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_FAILED, &errSv);
    }

    /// 登记脚本并给当前每个挂载作用域建实例; 完成通知在 JS 线程发出
    /// - **仅 io 线程调用** (能力 load)
    /// - 脚本先入 `scripts_`: 之后再挂载的 agent 由 [attachMountScope] 补建实例
    void loadScriptAsync(
        const PluginxxHost*           host,
        const std::string&            name,
        const std::string&            path,
        const std::string&            code,
        const PluginxxOperatorNotify& notify
    ) {
        if (scripts_.contains(name)) {
            failNotify(notify, fmt::format("plugin already loaded: {}", name));
            return;
        }
        ScriptEntry entry{name, path, code, host};
        scripts_.emplace(name, entry);
        // 作用域快照: JS 线程只认本次投递携带的列表, 不读 io 线程状态
        // - client 侧没有 agent 挂载: 固定一份伪作用域, 脚本顶层立即执行
        std::vector<MountScope> scopes;
        if (isClientSide()) {
            scopes.push_back(clientScope());
        } else {
            scopes.reserve(scopes_.size());
            for (const auto& [key, scope] : scopes_) {
                (void)key;
                scopes.push_back(scope);
            }
        }
        if (!post([this, entry, scopes, notify]() {
                if (stopping()) {
                    failNotify(notify, "interpreter.js engine stopped");
                    return;
                }
                std::string lastErr;
                for (const auto& scope : scopes) {
                    std::string err;
                    if (createInstanceOnJsThread(entry, scope, err) != 0) {
                        lastErr = err;
                    }
                }
                if (!lastErr.empty()) {
                    failNotify(notify, lastErr);
                    return;
                }
                std::string tools      = loadedToolsJsonOnJsThread(entry.name);
                std::string payloadStr = fmt::format(R"_({{"ok": true, "tools": {}}})_", tools);
                auto        resSv
                    = agentxx::plugin::PluginStringView::from(payloadStr.data(), payloadStr.size());
                notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, &resSv);
            })) {
            scripts_.erase(name);
            failNotify(notify, "interpreter.js engine stopped");
        }
    }

    /// 注销脚本及其全部实例; 完成通知在 JS 线程发出
    /// - **仅 io 线程调用** (能力 unload)
    void unloadScriptAsync(const std::string& name, const PluginxxOperatorNotify& notify) {
        scripts_.erase(name);
        if (!post([this, name, notify]() {
                destroyScriptInstancesOnJsThread(name);
                auto okSv = agentxx::plugin::PluginStringView::fromCstr("{\"ok\": true}");
                notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, &okSv);
            })) {
            failNotify(notify, "interpreter.js engine stopped");
        }
    }

    /// 已加载脚本的工具名 JSON 数组 (【必须在 JS 线程调用】)
    /// - 一份脚本在每个挂载作用域各有一份实例: 返回各实例工具名的并集
    std::string loadedToolsJsonOnJsThread(const std::string& name) {
        std::string       out    = "[";
        bool              first  = true;
        const std::string prefix = instanceKeyPrefix(name);
        for (auto& [key, pctx] : plugins_) {
            if (!pctx || !pctx->ctx || key.rfind(prefix, 0) != 0) {
                continue;
            }
            for (auto& k : jsToolsSnapshot(pctx.get())) {
                if (!first) {
                    out += ",";
                }
                first  = false;
                out   += fmt::format("\"{}\"", k);
            }
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

    /// 主程序工具完成 (任意线程): 复制结果并投递回 JS 线程 settle Promise。
    /// - 完成回调由主程序在调用方 IO 线程派发, 调用期间 caller/provider lease
    ///   均被持有, 因此 engine 与其 JS 线程必然存活。
    /// - 引擎已停止时 post 失败: 不触碰 JSValue, 只释放桥 (JSContext 即将整体释放)。
    static void PLUGINXX_CALL
        onCallToolDone(void* ud, int32_t status, const PluginxxStringView* payload) noexcept {
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
    /// Canceled/FAILED → reject(Error(message))
    void settleCall(JsCallBridge* bridge, int32_t status, std::string text) {
        if (!bridge) {
            return;
        }
        JSContext* ctx = bridge->ctx;
        if (status == PLUGINXX_OPERATOR_OK) {
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
                msg = status == PLUGINXX_OPERATOR_CANCELED ? "call_tool Canceled"
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

    /// 【JS 线程】立即拒绝 (主程序同步拒绝 call_tool_async 的路径)
    static void
        rejectCallNow(JSContext* ctx, JSValue resolve, JSValue reject, const std::string& msg) {
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

    /// 网络调用完成 (任意线程): 复制结果并投递回 JS 线程 settle Promise
    /// - 完成回调由主程序在调用方 IO 线程派发; 引擎已停止时 post 失败,
    ///   只释放桥, 不触碰 JSValue (JSContext 即将整体释放)
    static void PLUGINXX_CALL
        onNetCallDone(void* ud, int32_t status, const PluginxxStringView* payload) noexcept {
        auto* bridge = static_cast<JsNetBridge*>(ud);
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
                bridge->engine->settleNetCall(bridge, status, std::move(text));
            })) {
            delete bridge;
        }
    }

    /// 【JS 线程】settle netCallAsync: 总是 resolve 成结果对象
    /// - 业务失败与传输失败同一种形状 (`ok:false`), 脚本不用挨个 catch
    /// - 受理时给出的请求号以 `requestId` 补进结果 (脚本要取消时用)
    void settleNetCall(JsNetBridge* bridge, int32_t status, std::string text) {
        if (!bridge) {
            return;
        }
        JSContext* ctx = bridge->ctx;
        JSValue    out = JS_UNDEFINED;
        if (status == PLUGINXX_OPERATOR_OK) {
            out = netResultValue(ctx, text);
        } else {
            const char* reason
                = status == PLUGINXX_OPERATOR_CANCELED ? "canceled" : "failed";
            // 失败时主程序可能带了错误 JSON: 能解析出对象就用它
            JSValue parsed = text.empty()
                                 ? JS_NULL
                                 : JS_ParseJSON(ctx, text.c_str(), text.size(), "<net>");
            if (!JS_IsException(parsed) && JS_IsObject(parsed)) {
                out = parsed;
                JS_SetPropertyStr(ctx, out, "ok", JS_FALSE);
            } else {
                JS_FreeValue(ctx, parsed);
                out = netErrorObject(ctx, reason, text);
            }
        }
        const int64_t requestId = bridge->requestId.load(std::memory_order_acquire);
        if (requestId > 0 && JS_IsObject(out)) {
            JSValue existing = JS_GetPropertyStr(ctx, out, "requestId");
            if (JS_IsUndefined(existing)) {
                JS_SetPropertyStr(ctx, out, "requestId", JS_NewInt64(ctx, requestId));
            }
            JS_FreeValue(ctx, existing);
        }
        JSValue argv[1] = {out};
        JSValue rc      = JS_Call(ctx, bridge->resolve, JS_UNDEFINED, 1, argv);
        JS_FreeValue(ctx, rc);
        JS_FreeValue(ctx, out);
        JS_FreeValue(ctx, bridge->resolve);
        JS_FreeValue(ctx, bridge->reject);
        delete bridge;
    }

    /// 登记一个主程序回调 (返回交给主程序的 user_data)
    JsCallbackHook* registerCallbackHook(const std::string& instanceKey, int64_t token) {
        auto hook         = new JsCallbackHook{};
        hook->engine      = this;
        hook->instanceKey = instanceKey;
        hook->token       = token;
        std::lock_guard<std::mutex> lk(callbackMtx_);
        callbackHooks_.insert(hook);
        return hook;
    }

    /// 释放回调登记 (终态到达 / 提交失败 / 引擎销毁)
    void releaseCallbackHook(JsCallbackHook* hook) {
        if (!hook) {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(callbackMtx_);
            callbackHooks_.erase(hook);
        }
        delete hook;
    }

    /// 后台任务事件 (容器 io 线程): 复制载荷并投递回 JS 线程
    /// - 必须在 JS 线程调脚本回调: 脚本侧没有任何线程保护
    /// - 终态 (finished / canceled) 之后再不会有事件, 在此释放登记
    static void PLUGINXX_CALL onAsyncWorkEvent(
        void*                     ud,
        int32_t                   phase,
        const PluginxxStringView* payload_json
    ) noexcept {
        auto* hook = static_cast<JsCallbackHook*>(ud);
        if (!hook || !hook->engine) {
            return;
        }
        std::string payload;
        try {
            if (payload_json && payload_json->data) {
                payload.assign(payload_json->data, static_cast<size_t>(payload_json->size));
            }
        } catch (...) {
        }
        auto* engine = hook->engine;
        engine->post([engine, instanceKey = hook->instanceKey, token = hook->token, phase, payload]() {
            engine->doAsyncWorkEvent(instanceKey, token, phase, payload);
        });
        if (phase == AGENTXX_PLUGIN_ASYNC_WORK_EVT_FINISHED
            || phase == AGENTXX_PLUGIN_ASYNC_WORK_EVT_CANCELED) {
            engine->releaseCallbackHook(hook);
        }
    }

    /// 【JS 线程】调用脚本登记的任务事件回调
    /// - 实例已销毁 (agent 卸载 / 脚本卸载): 事件丢弃, 任务本身仍由主程序收尾
    void doAsyncWorkEvent(
        const std::string& instanceKey,
        int64_t            token,
        int32_t            phase,
        const std::string& payload
    ) {
        if (stopping()) {
            return;
        }
        auto it = plugins_.find(instanceKey);
        if (it == plugins_.end() || !it->second) {
            return;
        }
        auto*      pctx = it->second.get();
        const auto fit  = pctx->asyncWorkCallbacks.find(token);
        if (fit == pctx->asyncWorkCallbacks.end()) {
            return;
        }
        JSContext* ctx    = pctx->ctx;
        JSValue    phaseV = JS_NewString(ctx, asyncWorkPhaseName(phase));
        JSValue    dataV  = jsonTextToValue(ctx, payload, "<async_work>");
        JSValue    argv[2] = {phaseV, dataV};
        JSValue    rc      = JS_Call(ctx, fit->second, JS_UNDEFINED, 2, argv);
        if (JS_IsException(rc)) {
            JSValue exc = JS_GetException(ctx);
            logAt(
                3,
                fmt::format(
                    "interpreter.js: async work onEvent threw: {}",
                    valueToJsonString(ctx, exc)
                )
                    .c_str()
            );
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, rc);
        JS_FreeValue(ctx, phaseV);
        JS_FreeValue(ctx, dataV);
        // 终态之后不会再有事件: 把回调从表里删掉 (否则每提交一个任务就留一份函数)
        if (phase == AGENTXX_PLUGIN_ASYNC_WORK_EVT_FINISHED
            || phase == AGENTXX_PLUGIN_ASYNC_WORK_EVT_CANCELED) {
            JS_FreeValue(ctx, fit->second);
            pctx->asyncWorkCallbacks.erase(fit);
        }
    }

    /// 一条通道事件的自有副本 (跨线程传递用)
    struct ChannelEventCopy {
        int32_t     kind        = 0;
        int64_t     channelId   = 0;
        int64_t     interruptId = 0;
        std::string text;
        std::string payload;
    };

    /// 会话通道事件 (该根所属 io 线程): 拷一份载荷并投递回 JS 线程
    /// - 事件只在回调期间有效, 只拷字符串, 不动主程序对象
    /// - `closed` 是通道的最后一条事件, 到这就释放登记
    static void PLUGINXX_CALL onChannelEvent(
        void*                            ud,
        const AgentxxPluginChannelEvent* event
    ) noexcept {
        auto* hook = static_cast<JsCallbackHook*>(ud);
        if (!hook || !hook->engine || !event) {
            return;
        }
        ChannelEventCopy copy;
        try {
            copy.kind        = event->kind;
            copy.channelId   = event->channel_id;
            copy.interruptId = event->interrupt_id;
            if (event->text.data) {
                copy.text.assign(event->text.data, static_cast<size_t>(event->text.size));
            }
            if (event->payload_json.data) {
                copy.payload.assign(
                    event->payload_json.data,
                    static_cast<size_t>(event->payload_json.size)
                );
            }
        } catch (...) {
        }
        auto* engine = hook->engine;
        engine->post([engine,
                      instanceKey = hook->instanceKey,
                      token       = hook->token,
                      copy]() {
            engine->doChannelEvent(instanceKey, token, copy);
        });
        if (copy.kind == AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED) {
            engine->releaseCallbackHook(hook);
        }
    }

    /// 【JS 线程】调用脚本登记的通道事件回调
    /// - 实例已销毁时事件丢弃 (通道仍由主程序自己收尾)
    void doChannelEvent(
        const std::string&     instanceKey,
        int64_t                token,
        const ChannelEventCopy& event
    ) {
        if (stopping()) {
            return;
        }
        auto it = plugins_.find(instanceKey);
        if (it == plugins_.end() || !it->second) {
            return;
        }
        auto*      pctx = it->second.get();
        const auto fit  = pctx->channelCallbacks.find(token);
        if (fit == pctx->channelCallbacks.end()) {
            return;
        }
        JSContext* ctx = pctx->ctx;
        JSValue    ev  = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ev, "channelId", JS_NewInt64(ctx, event.channelId));
        JS_SetPropertyStr(ctx, ev, "kind", JS_NewString(ctx, channelEventName(event.kind)));
        if (event.interruptId != 0) {
            JS_SetPropertyStr(ctx, ev, "interruptId", JS_NewInt64(ctx, event.interruptId));
        }
        if (!event.text.empty()) {
            JS_SetPropertyStr(
                ctx,
                ev,
                "text",
                JS_NewStringLen(ctx, event.text.data(), event.text.size())
            );
        }
        JS_SetPropertyStr(
            ctx,
            ev,
            "payload",
            jsonTextToValue(ctx, event.payload, "<channel_event>")
        );
        JSValue argv[1] = {ev};
        JSValue rc      = JS_Call(ctx, fit->second, JS_UNDEFINED, 1, argv);
        if (JS_IsException(rc)) {
            JSValue exc = JS_GetException(ctx);
            logAt(
                3,
                fmt::format(
                    "interpreter.js: channel onEvent threw: {}",
                    valueToJsonString(ctx, exc)
                )
                    .c_str()
            );
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, rc);
        JS_FreeValue(ctx, ev);
        // `closed` 是通道的最后一条事件: 回调用完就删, 不给长会话留垃圾
        if (event.kind == AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED) {
            JS_FreeValue(ctx, fit->second);
            pctx->channelCallbacks.erase(fit);
        }
    }

    /// 工具 execute 桥 —— 异步启动 (统一异步操作模型):
    /// 【主程序 io 线程】调用; 任务入队 JS 线程后立即返回句柄,
    /// 完成结果由 JS 线程经 notifier 上报 (线程安全), io 线程零阻塞
    static void* PLUGINXX_CALL toolExecuteStart(
        void*                         ud,
        const PluginxxStringView*     args_json,
        const PluginxxStringView*     thread_id,
        const PluginxxStringView*     tool_call_id,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out
    );

    /// 事件回调桥: io 线程调用; post 到 JS 线程 (fire-and-forget) (类外定义)
    static void PLUGINXX_CALL eventFire(const PluginxxStringView* event_json, void* ud);

    /// 流结束回调桥 (`subscribe` 带 `onEnd` 时): io 线程调用; post 到 JS 线程
    static void PLUGINXX_CALL eventEndFire(const PluginxxStringView* reason_json, void* ud);

    /// 能力实现行的处理器 (任意线程): 复制参数与完成通知, 投递到 JS 线程执行脚本
    static void* PLUGINXX_CALL capImplStart(
        void*                         ctx,
        const PluginxxHost*           caller_host,
        const PluginxxStringView*     method,
        const PluginxxStringView*     args_json,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out
    );

    /// 取消一次尚未完成的脚本能力调用 (任意线程; 幂等)
    static void PLUGINXX_CALL capImplCancel(void* ctx, void* op);

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

    /// 查找实例上下文 (跨线程; 参数是实例键; 返回强引用保证存活)
    std::shared_ptr<JsPluginCtx> findPlugin(const std::string& instanceKey) {
        std::lock_guard<std::mutex> lk(mirrorMtx_);
        auto                        it = mirror_.find(instanceKey);
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
        // 脚本行再也执行不了: 在飞的能力调用按取消收尾, 主程序的操作不会悬挂
        drainCapOps();
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

    /// 停止时收尾在飞的能力调用
    /// - 脚本行再也执行不了: 在飞调用按取消收尾, 主程序的操作不会悬挂
    /// - `invokeCapability` 的 Promise 不再结算 (脚本随实例销毁, 没有调用方了)
    void drainCapOps() noexcept {
        std::vector<std::shared_ptr<JsCapCall>> calls;
        {
            std::lock_guard<std::mutex> lk(capMtx_);
            calls.reserve(capCalls_.size());
            for (auto& entry : capCalls_) {
                calls.push_back(entry.second);
            }
            capCalls_.clear();
            capInvokes_.clear();
        }
        for (auto& call : calls) {
            if (call->settled.exchange(true)) {
                continue;
            }
            if (call->notify.done) {
                PluginxxStringView view{};
                call->notify.done(call->notify.host_ud, PLUGINXX_OPERATOR_CANCELED, &view);
            }
        }
    }

    static void setErr(char** err_out, const PluginxxHost* host, const char* msg) {
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
            auto pctx  = findPlugin(timer.instanceKey);
            if (!pctx || pctx->deleted || !pctx->ctx) {
                continue; // 上下文已销毁, fn 引用随 JsPluginCtx 释放
            }
            JSValue ret     = JS_Call(pctx->ctx, timer.fn, JS_UNDEFINED, 0, nullptr);
            auto    outcome = drivePromise(pctx->ctx, ret);
            if (outcome.kind == PromiseOutcome::Kind::Rejected) {
                guardLog(fmt::format(
                             "[interpreter.js] timer callback rejected: {}",
                             rejectedText(pctx->ctx, outcome)
                )
                             .c_str());
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
                    // 执行定时器回调期间视为"忙": 期间可能调用主程序 vtable,
                    // 停止请求必须交给收尾线程 (见 requestStop)。
                    busy_ = true;
                    lk.unlock();
                    fireDueTimersInline();
                    drainPromiseJobs();
                    lk.lock();
                    busy_ = false;
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
                // 任务在手中 (取任务与置忙在同一临界区内完成): 本线程此后可能
                // 调用主程序 vtable, 停止请求不得在调用线程直接 join。
                busy_ = true;
            }
            taskStart_ = std::chrono::steady_clock::now();
            try {
                task();
            } catch (...) {
                // 任务异常不得终止 JS 线程
            }
            // 任务可能 resolve 了 Promise (网络完成 / 定时器 / 回调): 把续体泵完
            // (仍在"忙"状态: 续体同样可能调主程序 vtable)
            drainPromiseJobs();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                busy_ = false;
            }
        }
        // ---- 线程退出前清理 (JS 线程, 安全释放 JSValue) ----
        // 1. 残余定时器 (正常实例卸载路径已清各实例定时器, 此处补充处理)
        for (auto& [id, t] : timers_) {
            (void)id;
            auto pctx = findPlugin(t.instanceKey);
            if (pctx && pctx->ctx) {
                JS_FreeValue(pctx->ctx, t.fn);
            }
        }
        timers_.clear();
        // 2. client 侧渲染注册: 先标记失效 (io 线程的回调据此直接失败),
        //    脚本函数留在实例里随 JSContext 一起释放
        {
            std::lock_guard<std::mutex> lk(renderMtx_);
            for (auto& [tool, reg] : renders_) {
                (void)tool;
                reg->dead.store(true, std::memory_order_release);
                reg->collapsedToken.store(0, std::memory_order_release);
                reg->expandedToken.store(0, std::memory_order_release);
            }
        }
        // 2b. client 侧脚本回调 (命令/动作/定时器/快捷键): 同样只能标失效
        {
            std::lock_guard<std::mutex> lk(clientCbMtx_);
            for (auto& cb : clientCbs_) {
                if (cb) {
                    cb->dead.store(true, std::memory_order_release);
                }
            }
        }
        // 3. 残余实例上下文 (正常路径引擎卸载前 JS 插件已级联卸载; 补充释放,
        //    保证 JS_FreeRuntime 前所有 JSContext 已 Free, 无 gc 断言)
        for (auto& [key, pctx] : plugins_) {
            (void)key;
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
    /// - 首参数是正在执行 JS 的上下文 (quickjs-ng 的 JSInterruptHandler 定义),
    ///   本处只用到 opaque (引擎自身), 由 JS_SetInterruptHandler 注册时传入
    static int interruptHandler(JSContext* ctx, void* opaque) {
        auto* engine = static_cast<JsEngine*>(opaque);
        (void)ctx;
        if (std::chrono::steady_clock::now() - engine->taskStart_
            > std::chrono::milliseconds(kTaskTimeoutMs)) {
            return 1;
        }
        return 0;
    }

    // ==================== 实例加载/卸载 (JS 线程) ====================

    /// 为一份 (脚本, 挂载作用域) 建实例: 新建 JSContext, 注入桥, 执行脚本
    /// - 脚本顶层就是"该 agent 的插件实例初始化", 注册归属 scope
    /// - 已存在同键实例时直接成功 (重复 mount 幂等)
    int createInstanceOnJsThread(
        const ScriptEntry&  entry,
        const MountScope&   scope,
        std::string&        err
    ) {
        const std::string key = instanceKeyOf(entry.name, scope.key);
        if (plugins_.find(key) != plugins_.end()) {
            return 0;
        }
        auto pctx         = std::make_shared<JsPluginCtx>();
        pctx->name        = entry.name;
        pctx->instanceKey = key;
        pctx->host        = entry.host;
        pctx->engine      = this;
        pctx->setScope(scope.agentId, scope.seq);
        pctx->ctx = JS_NewContext(rt_);
        if (!pctx->ctx) {
            err = "JS_NewContext failed";
            return -1;
        }
        JS_SetContextOpaque(pctx->ctx, pctx.get());
        pctx->tools  = JS_NewObject(pctx->ctx);
        // 订阅表: Array of {topic, handler, onEnd?, token, subPtrLo/Hi}
        // - 必须在执行脚本前建好: 脚本顶层的 agentxx.subscribe / onHook 会往里写
        //   (此前它是 JS_UNDEFINED, 写入静默失败, 订阅只登记在主程序、回调永远找不到处理器)
        pctx->agents = JS_NewArray(pctx->ctx);

        if (!injectBridge(pctx.get())) {
            err = "failed to inject agentxx bridge";
            rollbackScriptEffects(pctx);
            return -1;
        }

        // 执行插件脚本 (顶层可调用 agentxx.registerTool 等)
        JSValue ret
            = JS_Eval(pctx->ctx, entry.code.c_str(), entry.code.size(), entry.path.c_str(), 0);
        if (JS_IsException(ret)) {
            err = extractException(pctx->ctx, ret);
            JS_FreeValue(pctx->ctx, ret);
            // 脚本初始化是注册事务 —— 顶层异常时先撤销已产生的全部副作用
            // (工具/通知/订阅/资源/定时器), 再释放 JSContext。
            rollbackScriptEffects(pctx);
            return -1;
        }
        JS_FreeValue(pctx->ctx, ret);

        plugins_[key] = pctx;
        {
            std::lock_guard<std::mutex> lk(mirrorMtx_);
            mirror_[key] = pctx;
        }
        return 0;
    }

    /// 撤销实例产生的主程序注册与定时器 (失败回滚与脚本卸载共用)。
    /// - `runRollback=false` 用于 agent 卸载: 主程序已按挂载记录撤销了该实例的
    ///   工具/权限/提示词/订阅/资源, 此时再去照样注销会碰到已释放的句柄
    ///   (实测 ASan: broadcast 订阅的 shared_ptr 已随实例析构释放)
    void rollbackScriptEffects(const std::shared_ptr<JsPluginCtx>& pctx, bool runRollback = true) {
        if (!pctx) {
            return;
        }
        if (runRollback) {
            pctx->runRollback();
        } else {
            pctx->rollbackActions.clear();
        }
        if (!pctx->ctx) {
            return;
        }
        for (auto tit = timers_.begin(); tit != timers_.end();) {
            if (tit->second.instanceKey == pctx->instanceKey) {
                JS_FreeValue(pctx->ctx, tit->second.fn);
                tit = timers_.erase(tit);
            } else {
                ++tit;
            }
        }
    }

    /// 销毁单个实例 (按实例键)
    /// - `runRollback`: 是否执行 JS 侧注册的注销 (脚本 unload 时是, agent
    ///   unmount 时否)
    void destroyInstanceOnJsThread(const std::string& key, bool runRollback = true) {
        auto it = plugins_.find(key);
        if (it == plugins_.end()) {
            return;
        }
        auto pctx     = it->second;
        pctx->deleted = true;
        pctx->host    = nullptr; // 后续任务不得再调用主程序
        // 脚本卸载同样是注册事务的收尾: 注销该实例注册的工具/通知/订阅/资源,
        // 不留悬垂 user_data (主程序 detachAll 只覆盖引擎实例整体卸载的场景)。
        rollbackScriptEffects(pctx, runRollback);
        plugins_.erase(it);
        {
            std::lock_guard<std::mutex> lk(mirrorMtx_);
            mirror_.erase(key);
        }
        // 全部进行中的任务结束后 shared_ptr 归零 → JsPluginCtx 析构 → JS_FreeContext
    }

    /// 销毁一份脚本的全部实例 (脚本 unload)
    void destroyScriptInstancesOnJsThread(const std::string& name) {
        const std::string prefix = instanceKeyPrefix(name);
        std::vector<std::string> keys;
        for (const auto& [key, pctx] : plugins_) {
            (void)pctx;
            if (key.rfind(prefix, 0) == 0) {
                keys.push_back(key);
            }
        }
        for (const auto& key : keys) {
            // 脚本卸载: 显式撤销本实例的注册 (脚本先于 agent 卸载时主程序未必清理)
            destroyInstanceOnJsThread(key, /*runRollback=*/true);
        }
    }

    /// 销毁某个挂载作用域的全部实例 (agent unmount)
    void destroyScopeInstancesOnJsThread(const std::string& scopeKey) {
        const std::string suffix = std::string(1, kInstanceKeySep) + scopeKey;
        std::vector<std::string> keys;
        for (const auto& [key, pctx] : plugins_) {
            (void)pctx;
            if (key.size() > suffix.size()
                && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
                keys.push_back(key);
            }
        }
        for (const auto& key : keys) {
            // agent 卸载: 主程序已按挂载记录撤销全部注册, 这里只释放 JS 侧状态
            // (再照着 rollback 注销会碰到已随实例析构释放的句柄, 实测 ASan UAF)
            destroyInstanceOnJsThread(key, /*runRollback=*/false);
        }
    }


    // ==================== 工具执行 (JS 线程) ====================

    void doToolExecute(JsToolBinding* binding, ToolExecReq& req) {
        auto pctx = findPlugin(binding->instanceKey);
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
                applyValueOutcome(pctx->ctx, outcome.value, req);
                break;
            case PromiseOutcome::Kind::Rejected:
                req.error  = rejectedText(pctx->ctx, outcome);
                req.failed = true;
                break;
            case PromiseOutcome::Kind::Timeout:
                req.error  = rejectedText(pctx->ctx, outcome);
                req.failed = true;
                break;
            case PromiseOutcome::Kind::Canceled:
                req.error     = rejectedText(pctx->ctx, outcome);
                req.Canceled = true;
                break;
        }
        JS_FreeValue(pctx->ctx, outcome.value);
        JS_FreeValue(pctx->ctx, execFn);
    }

    // ==================== 通知/事件 (JS 线程) ====================

    void doEventFire(JsHookBinding* binding, const std::string& payload) {
        auto pctx = findPlugin(binding->instanceKey);
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

        // 遍历订阅表, 触发本频道的 handler (payload 为功能埋点载荷 JSON 字符串)
        JSValue  lenVal = JS_GetPropertyStr(pctx->ctx, pctx->agents, "length");
        uint32_t len    = 0;
        JS_ToUint32(pctx->ctx, &len, lenVal);
        JS_FreeValue(pctx->ctx, lenVal);
        // 只调本订阅那一条 (`binding->token`): 同一频道的多个订阅各收一次
        const uint32_t first
            = binding->token >= 0 ? static_cast<uint32_t>(binding->token) : 0u;
        const uint32_t last
            = binding->token >= 0 ? first + 1 : len;
        for (uint32_t i = first; i < last && i < len; ++i) {
            JSValue entry = JS_GetPropertyUint32(pctx->ctx, pctx->agents, i);
            if (JS_IsObject(entry)) {
                // 只调这个频道的处理器 (同一插件可以订阅多个频道)
                JSValue     topicVal = JS_GetPropertyStr(pctx->ctx, entry, "topic");
                std::string entryTopic;
                if (JS_IsString(topicVal)) {
                    entryTopic = jsToCppString(pctx->ctx, topicVal);
                }
                JS_FreeValue(pctx->ctx, topicVal);
                if (entryTopic != binding->topic) {
                    JS_FreeValue(pctx->ctx, entry);
                    continue;
                }
                JSValue handler = JS_GetPropertyStr(pctx->ctx, entry, "handler");
                if (JS_IsFunction(pctx->ctx, handler)) {
                    JSValue arg
                        = JS_ParseJSON(pctx->ctx, payload.c_str(), payload.size(), "<event>");
                    if (JS_IsException(arg)) {
                        JS_FreeValue(pctx->ctx, arg);
                        arg = JS_NewString(pctx->ctx, payload.c_str());
                    }
                    JSValue ret     = JS_Call(pctx->ctx, handler, JS_UNDEFINED, 1, &arg);
                    auto    outcome = drivePromise(pctx->ctx, ret);
                    if (outcome.kind == PromiseOutcome::Kind::Rejected) {
                        guardLog(fmt::format(
                                     "[interpreter.js] event handler rejected: {}",
                                     rejectedText(pctx->ctx, outcome)
                        )
                                     .c_str());
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

    /// 流结束 (JS 线程): 调该频道订阅时登记的 `onEnd(reason)`
    ///
    /// 与 [doEventFire] 同一套遍历, 只是取 `onEnd`; 没登记就什么也不做。
    void doEventEndFire(JsHookBinding* binding, const std::string& reason) {
        auto pctx = findPlugin(binding->instanceKey);
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

        JSValue  lenVal = JS_GetPropertyStr(pctx->ctx, pctx->agents, "length");
        uint32_t len    = 0;
        JS_ToUint32(pctx->ctx, &len, lenVal);
        JS_FreeValue(pctx->ctx, lenVal);
        const uint32_t first
            = binding->token >= 0 ? static_cast<uint32_t>(binding->token) : 0u;
        const uint32_t last
            = binding->token >= 0 ? first + 1 : len;
        for (uint32_t i = first; i < last && i < len; ++i) {
            JSValue entry = JS_GetPropertyUint32(pctx->ctx, pctx->agents, i);
            if (JS_IsObject(entry)) {
                JSValue     topicVal = JS_GetPropertyStr(pctx->ctx, entry, "topic");
                std::string entryTopic;
                if (JS_IsString(topicVal)) {
                    entryTopic = jsToCppString(pctx->ctx, topicVal);
                }
                JS_FreeValue(pctx->ctx, topicVal);
                if (entryTopic != binding->topic) {
                    JS_FreeValue(pctx->ctx, entry);
                    continue;
                }
                JSValue onEnd = JS_GetPropertyStr(pctx->ctx, entry, "onEnd");
                if (JS_IsFunction(pctx->ctx, onEnd)) {
                    JSValue arg
                        = JS_ParseJSON(pctx->ctx, reason.c_str(), reason.size(), "<end>");
                    if (JS_IsException(arg)) {
                        JS_FreeValue(pctx->ctx, arg);
                        arg = reason.empty() ? JS_NULL : JS_NewString(pctx->ctx, reason.c_str());
                    }
                    JSValue ret     = JS_Call(pctx->ctx, onEnd, JS_UNDEFINED, 1, &arg);
                    auto    outcome = drivePromise(pctx->ctx, ret);
                    if (outcome.kind == PromiseOutcome::Kind::Rejected) {
                        guardLog(fmt::format(
                                     "[interpreter.js] onEnd rejected: {}",
                                     rejectedText(pctx->ctx, outcome)
                                 )
                                     .c_str());
                    }
                    JS_FreeValue(pctx->ctx, outcome.value);
                    JS_FreeValue(pctx->ctx, ret);
                    JS_FreeValue(pctx->ctx, arg);
                }
                JS_FreeValue(pctx->ctx, onEnd);
            }
            JS_FreeValue(pctx->ctx, entry);
        }
    }

    // ==================== 能力行 (脚本实现的接口表能力) ====================

    /// 收尾一次脚本能力调用 (恰好一次): 先从在飞表摘除, 再回调主程序的完成通知
    /// - 正常完成 / 取消 / 引擎停止三条路径都走这里, `settled` 只是兵底
    void finishCapCall(JsCapCall* call, int32_t status, const std::string& payload);

    /// 登记一条脚本实现的能力行 (JS 线程)
    /// - 一条行 = 一个 [JsCapImpl] (主程序持有它的地址) + 一个回调 token + 一个撤销动作
    /// - 同一 (id, ownerTag) 重复登记先撤旧行 (后登记者覆盖)
    /// - `return` 0 = 成功; 非 0 = 主程序结果码 (`error` 给出原因)
    int32_t provideCapabilityRow(
        JSContext*                       ctx,
        JsPluginCtx*                     pctx,
        const PluginxxCapabilitiesIface* caps,
        const std::string&               id,
        const std::string&               ownerTag,
        JSValueConst                     handler,
        bool                             pointMode,
        int32_t                          impls,
        int32_t                          merge,
        uint32_t                         phases,
        int32_t                          priority,
        int32_t                          timeoutMs,
        std::string*                     error
    );

    /// 撤销本实例的一条能力行 (JS 线程; 不存在的行什么也不做)
    void unprovideCapabilityRow(
        JSContext*                       ctx,
        JsPluginCtx*                     pctx,
        const PluginxxCapabilitiesIface* caps,
        const std::string&               id,
        const std::string&               ownerTag
    );

    /// JS 线程侧执行一次脚本能力调用 (找实例 → 找回调 → 调 → 结果回主程序)
    void doCapCall(JsCapCall* call);

    /// 【JS 线程】结算 invokeCapability 的 Promise
    void settleCapInvoke(JsCapInvokeBridge* bridge, int32_t status, std::string payload);

    /// invokeCapability 的完成回调 (主程序 io 线程)
    static void PLUGINXX_CALL capInvokeDone(
        void*                     ud,
        int32_t                   status,
        const PluginxxStringView* payload
    );

    // ==================== Promise 驱动 ====================

    /// 泵完已入队的 Promise 微任务 (JS 线程主循环在每个任务/定时器之后调用)
    ///
    /// - 业务回调 (`netFetch(...).then(...)` / `Promise.resolve().then(...)`) 不等待
    ///   Promise, 主循环不主动泵的话续体永远不会执行
    /// - 上游任务 (网络完成 / 发送通知) 也是任务: 它们 resolve 的 Promise 在这个循环里结算
    /// - 有上限 ([kMaxJobsPerDrain]), job 抛异常只记日志并继续泵剩下的
    void drainPromiseJobs() noexcept {
        if (!rt_) {
            return;
        }
        for (int i = 0; i < kMaxJobsPerDrain; ++i) {
            JSContext* jobCtx = nullptr;
            const int  rc     = JS_ExecutePendingJob(rt_, &jobCtx);
            if (rc == 0) {
                return;
            }
            if (rc < 0) {
                if (jobCtx) {
                    JSValue exc = JS_GetException(jobCtx);
                    logAt(
                        3,
                        fmt::format(
                            "interpreter.js: promise job threw: {}",
                            valueToJsonString(jobCtx, exc)
                        )
                            .c_str()
                    );
                    JS_FreeValue(jobCtx, exc);
                }
                continue;
            }
        }
        logAt(3, "interpreter.js: promise jobs not drained (per-task limit reached)");
    }

    /// Promise 驱动结果: 调用方必须据此映射 Operation 终态 ——
    /// 拒绝/超时是失败, 引擎停止是取消, 不能当成成功文本。
    struct PromiseOutcome {
        enum class Kind {
            Value,
            Rejected,
            Timeout,
            Canceled
        };

        Kind        kind  = Kind::Value;
        JSValue     value = JS_UNDEFINED; ///< Value=结果值; Rejected=拒绝原因 (均拥有)
        std::string text;                 ///< Timeout/Canceled 的说明文本
    };

    /// 驱动 Promise 直至 settle (JS 线程内调用)
    /// - 非 Promise/普通值原样 Dup 为 Value;
    /// - JS 异常值统一归一为 Rejected (拒绝原因已取出, 不再留在 context 上);
    /// - 另用 steady_clock 绝对截止时间判超时 (interrupt handler 负责打断 CPU 长任务),
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
        const auto deadline
            = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPromiseWaitLimitMs);
        while (JS_PromiseState(ctx, value) == JS_PROMISE_PENDING) {
            if (stop_.load(std::memory_order_acquire)) {
                out.kind = PromiseOutcome::Kind::Canceled;
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
                out.value = JS_IsUndefined(exc) ? JS_NewString(ctx, "pending job exception") : exc;
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
            auto wake = (next == std::chrono::steady_clock::time_point::max())
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

    /// 把 PromiseOutcome 的拒绝原因转成主程序错误文本 (调用方随后 Free outcome.value)
    std::string rejectedText(JSContext* ctx, PromiseOutcome& out) {
        if (out.kind == PromiseOutcome::Kind::Timeout
            || out.kind == PromiseOutcome::Kind::Canceled) {
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
    static std::string valueToJsonString(JSContext* ctx, JSValueConst v) {
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

    /// 读对象属性为字符串 (属性不存在 / 非字符串时返回空)
    static std::string propString(JSContext* ctx, JSValueConst obj, const char* key) {
        JSValue     v = JS_GetPropertyStr(ctx, obj, key);
        std::string out;
        if (JS_IsString(v)) {
            out = jsToCppString(ctx, v);
        }
        JS_FreeValue(ctx, v);
        return out;
    }

    /// 往本实例订阅表追加一条处理器, 返回下标 token
    ///
    /// - client 侧订阅没有 `agents` 之外的登记表 (句柄在 `clientSubs`), 但回调派发
    ///   同样经 `doEventFire`, 所以也要在这里存一份处理器
    static int64_t appendSubEntry(
        JSContext*         ctx,
        JsPluginCtx*       pctx,
        const std::string& topic,
        JSValueConst       handler,
        JSValueConst       on_end
    ) {
        JSValue  lenVal = JS_GetPropertyStr(ctx, pctx->agents, "length");
        uint32_t len    = 0;
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "topic", JS_NewStringLen(ctx, topic.data(), topic.size()));
        JS_SetPropertyStr(ctx, entry, "handler", JS_DupValue(ctx, handler));
        if (JS_IsFunction(ctx, on_end)) {
            JS_SetPropertyStr(ctx, entry, "onEnd", JS_DupValue(ctx, on_end));
        }
        JS_SetPropertyStr(ctx, entry, "token", JS_NewInt32(ctx, static_cast<int32_t>(len)));
        JS_SetPropertyUint32(ctx, pctx->agents, len, entry);
        return static_cast<int64_t>(len);
    }

    /// 读对象属性为 64 位整数 (不存在 / 非数字时返回默认值; 布尔按 1/0)
    static int64_t propInt64(JSContext* ctx, JSValueConst obj, const char* key, int64_t def) {
        JSValue v   = JS_GetPropertyStr(ctx, obj, key);
        int64_t out = def;
        if (JS_IsNumber(v)) {
            JS_ToInt64(ctx, &out, v);
        } else if (JS_IsBool(v)) {
            out = JS_ToBool(ctx, v) != 0 ? 1 : 0;
        }
        JS_FreeValue(ctx, v);
        return out;
    }

    /// 读对象属性为布尔 (不存在时返回默认值; 数字非 0 也算真)
    static bool propBool(JSContext* ctx, JSValueConst obj, const char* key, bool def) {
        JSValue v   = JS_GetPropertyStr(ctx, obj, key);
        bool    out = def;
        if (JS_IsBool(v) || JS_IsNumber(v)) {
            out = JS_ToBool(ctx, v) != 0;
        }
        JS_FreeValue(ctx, v);
        return out;
    }

    /// 读对象属性为 JSON 文本 (对象/数组序列化, 字符串原样, 其它返回空)
    /// - 用于把 JS 对象原样交给主程序的 JSON 参数 (工具策略 / 插件挂载 / 名称列表)
    static std::string propJson(JSContext* ctx, JSValueConst obj, const char* key) {
        JSValue     v = JS_GetPropertyStr(ctx, obj, key);
        std::string out;
        if (JS_IsString(v)) {
            out = jsToCppString(ctx, v);
        } else if (JS_IsObject(v) && !JS_IsFunction(ctx, v)) {
            out = valueToJsonString(ctx, v);
        }
        JS_FreeValue(ctx, v);
        return out;
    }

    /// 参数 JSON 文本: 字符串按原样 (已是 JSON 文本), 对象/数组序列化, 缺省 `{}`
    static std::string argsJsonText(JSContext* ctx, JSValueConst v) {
        if (JS_IsUndefined(v) || JS_IsNull(v)) {
            return "{}";
        }
        std::string text
            = JS_IsString(v) ? jsToCppString(ctx, v) : valueToJsonString(ctx, v);
        return text.empty() ? std::string{"{}"} : text;
    }

    /// `subscribe(topic, handler, options)` 的 options → 主程序的 options_json
    ///
    /// 只带认识的字段 (`intervalMs` = 通知间隔毫秒; `mode` = `delay` / `drop`),
    /// 其它字段忽略; 一个都没给时返回空串, 主程序按"不限频"处理。
    /// `onEnd` 不在这里 —— 它决定订阅用哪个表成员 (`subscribe_broadcast_ex`)。
    static std::string subscribeOptionsJsonOf(JSContext* ctx, JSValueConst opt) {
        if (!JS_IsObject(opt)) {
            return {};
        }
        const int64_t     interval = propInt64(ctx, opt, "intervalMs", 0);
        const std::string mode     = propString(ctx, opt, "mode");
        const bool        modeOk   = mode == "delay" || mode == "drop";
        std::string       out      = "{";
        if (interval > 0) {
            out += fmt::format("\"intervalMs\":{}", interval);
        }
        if (modeOk) {
            if (out.size() > 1) {
                out += ",";
            }
            out += fmt::format("\"mode\":\"{}\"", mode);
        }
        out += "}";
        return out.size() > 2 ? out : std::string{};
    }

    /// 读 JS 数组里的字符串项 (非数组 / 非字符串项跳过)
    static std::vector<std::string> jsStringArray(JSContext* ctx, JSValueConst arr) {
        std::vector<std::string> out;
        if (!JS_IsArray(arr)) {
            return out;
        }
        JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
        uint32_t len   = 0;
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (uint32_t i = 0; i < len; ++i) {
            JSValue item = JS_GetPropertyUint32(ctx, arr, i);
            if (JS_IsString(item)) {
                out.push_back(jsToCppString(ctx, item));
            }
            JS_FreeValue(ctx, item);
        }
        return out;
    }

    /// 权限作用域参数 → `AGENTXX_PLUGIN_PERMISSION_SCOPE_*`
    /// - 接受 `"read"` / `"write"` 或数字; 其它按读处理
    static int32_t permissionScopeOf(JSContext* ctx, JSValueConst v) {
        if (JS_IsString(v)) {
            const std::string text = jsToCppString(ctx, v);
            if (text == "write") {
                return AGENTXX_PLUGIN_PERMISSION_SCOPE_WRITE;
            }
            return AGENTXX_PLUGIN_PERMISSION_SCOPE_READ;
        }
        int32_t num = 0;
        if (JS_IsNumber(v) && JS_ToInt32(ctx, &num, v) == 0
            && num == AGENTXX_PLUGIN_PERMISSION_SCOPE_WRITE) {
            return AGENTXX_PLUGIN_PERMISSION_SCOPE_WRITE;
        }
        return AGENTXX_PLUGIN_PERMISSION_SCOPE_READ;
    }

    /// 能力登记的 `impls` 取值 ("one" | "many", 缺省 many)
    static int32_t capabilityImplsOf(JSContext* ctx, JSValueConst opt) {
        return propString(ctx, opt, "impls") == "one" ? PLUGINXX_CAPABILITY_IMPLS_ONE
                                                       : PLUGINXX_CAPABILITY_IMPLS_MANY;
    }

    /// 能力登记的 `merge` 取值 ("none" | "first" | "veto", 缺省 first)
    static int32_t capabilityMergeOf(JSContext* ctx, JSValueConst opt) {
        const std::string text = propString(ctx, opt, "merge");
        if (text == "none") {
            return PLUGINXX_CAPABILITY_MERGE_NONE;
        }
        if (text == "veto") {
            return PLUGINXX_CAPABILITY_MERGE_VETO;
        }
        return PLUGINXX_CAPABILITY_MERGE_FIRST;
    }

    /// 能力登记的 `phases` 取值 (数组: impl / before / after, 缺省只开 impl)
    static uint32_t capabilityPhasesOf(JSContext* ctx, JSValueConst opt) {
        JSValue phasesVal = JS_GetPropertyStr(ctx, opt, "phases");
        uint32_t mask     = 0;
        for (const auto& name : jsStringArray(ctx, phasesVal)) {
            if (name == "impl") {
                mask |= PLUGINXX_CAPABILITY_PHASE_MASK_IMPL;
            } else if (name == "before") {
                mask |= PLUGINXX_CAPABILITY_PHASE_MASK_BEFORE;
            } else if (name == "after") {
                mask |= PLUGINXX_CAPABILITY_PHASE_MASK_AFTER;
            }
        }
        JS_FreeValue(ctx, phasesVal);
        return mask == 0 ? PLUGINXX_CAPABILITY_PHASE_MASK_IMPL : mask;
    }

    /// JSON 文本 → JS 值 (不是合法 JSON 时按字符串返回; 空文本返回 null)
    static JSValue jsonTextToValue(JSContext* ctx, const std::string& text, const char* tag) {
        if (text.empty()) {
            return JS_NULL;
        }
        JSValue out = JS_ParseJSON(ctx, text.c_str(), text.size(), tag);
        if (JS_IsException(out)) {
            JS_FreeValue(ctx, out);
            out = JS_NewStringLen(ctx, text.data(), text.size());
        }
        return out;
    }

    /// 失败结果对象 `{ok:false, error, message}` (网络与后台任务共用同一种形状)
    static JSValue netErrorObject(
        JSContext*         ctx,
        const std::string& error,
        const std::string& message
    ) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "ok", JS_FALSE);
        JS_SetPropertyStr(ctx, obj, "error", JS_NewStringLen(ctx, error.data(), error.size()));
        if (!message.empty()) {
            JS_SetPropertyStr(
                ctx,
                obj,
                "message",
                JS_NewStringLen(ctx, message.data(), message.size())
            );
        }
        return obj;
    }

    /// 网络结果 JSON → JS 值
    /// - 结果 JSON 一律是对象 (`{ok:...}`); 解析不出来时按失败兜底
    static JSValue netResultValue(JSContext* ctx, const std::string& json) {
        if (json.empty()) {
            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "ok", JS_TRUE);
            return obj;
        }
        JSValue out = JS_ParseJSON(ctx, json.c_str(), json.size(), "<net>");
        if (!JS_IsException(out) && JS_IsObject(out)) {
            return out;
        }
        JS_FreeValue(ctx, out);
        return netErrorObject(ctx, "invalid_result", json);
    }

    /// 网络表入口的结果码 + 结果 JSON → JS 值
    /// - 入口失败与业务失败同一种形状: 调用方只看 `ok`
    static JSValue netOutcomeToValue(JSContext* ctx, int32_t code, const std::string& json) {
        if (code == PLUGINXX_RESULT_OK) {
            return netResultValue(ctx, json);
        }
        const char* name = pluginxx::result::nameOf(code);
        return netErrorObject(ctx, name ? name : "unknown", json);
    }

    /// 后台任务事件阶段 → JS 侧名字
    static const char* asyncWorkPhaseName(int32_t phase) {
        switch (phase) {
            case AGENTXX_PLUGIN_ASYNC_WORK_EVT_QUEUED:
                return "queued";
            case AGENTXX_PLUGIN_ASYNC_WORK_EVT_STARTED:
                return "started";
            case AGENTXX_PLUGIN_ASYNC_WORK_EVT_FINISHED:
                return "finished";
            case AGENTXX_PLUGIN_ASYNC_WORK_EVT_CANCELED:
                return "canceled";
            default:
                return "unknown";
        }
    }

    /// 会话通道事件类型 → JS 侧名字
    static const char* channelEventName(int32_t kind) {
        switch (kind) {
            case AGENTXX_PLUGIN_CHANNEL_EVT_READY:
                return "ready";
            case AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START:
                return "turn_start";
            case AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_TEXT:
                return "delta_text";
            case AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_THINKING:
                return "delta_thinking";
            case AGENTXX_PLUGIN_CHANNEL_EVT_TOOL_START:
                return "tool_start";
            case AGENTXX_PLUGIN_CHANNEL_EVT_TOOL_END:
                return "tool_end";
            case AGENTXX_PLUGIN_CHANNEL_EVT_GEN_STATUS:
                return "gen_status";
            case AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END:
                return "turn_end";
            case AGENTXX_PLUGIN_CHANNEL_EVT_CONTEXT_STATS:
                return "context_stats";
            case AGENTXX_PLUGIN_CHANNEL_EVT_INPUT_ACK:
                return "input_ack";
            case AGENTXX_PLUGIN_CHANNEL_EVT_QUEUE_STATE:
                return "queue_state";
            case AGENTXX_PLUGIN_CHANNEL_EVT_INTERRUPT:
                return "interrupt";
            case AGENTXX_PLUGIN_CHANNEL_EVT_INTERRUPT_EXPIRED:
                return "interrupt_expired";
            case AGENTXX_PLUGIN_CHANNEL_EVT_TIP:
                return "tip";
            case AGENTXX_PLUGIN_CHANNEL_EVT_ERROR:
                return "error";
            case AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED:
                return "closed";
            default:
                return "unknown";
        }
    }

    /// 工具 execute 的正常返回 -> 结果正文与终态
    ///
    /// 两种写法都支持:
    /// - 普通值 / 字符串: 状态 `ok`, 正文按 [valueToJsonString] 转换 (与旧行为一致);
    /// - 对象且带 `status` 字符串字段: 按工具结果处理 —— `text` 字段作正文
    ///   (缺省用整份 JSON), `status` 取 `ok`/`failed`/`interrupted`/`canceled`
    ///   (未知取值按 `ok`; 插件面没有中断态, `interrupted` 记失败)
    ///
    /// - `args`:
    ///     - [ctx] JS 上下文
    ///     - [v]   execute 的返回值
    ///     - [req] 写入 result / status
    void applyValueOutcome(JSContext* ctx, JSValueConst v, ToolExecReq& req) {
        if (JS_IsObject(v)) {
            JSValue statusVal = JS_GetPropertyStr(ctx, v, "status");
            if (JS_IsString(statusVal)) {
                const auto status  = jsToCppString(ctx, statusVal);
                JS_FreeValue(ctx, statusVal);
                JSValue textVal = JS_GetPropertyStr(ctx, v, "text");
                if (JS_IsString(textVal)) {
                    req.result = jsToCppString(ctx, textVal);
                } else {
                    // `text` 不是字符串 (可能是数字/对象): 走统一转换
                    req.result = valueToJsonString(ctx, textVal);
                }
                JS_FreeValue(ctx, textVal);
                if (status == "failed" || status == "interrupted") {
                    req.status = PLUGINXX_OPERATOR_FAILED;
                } else if (status == "canceled") {
                    req.status = PLUGINXX_OPERATOR_CANCELED;
                } else {
                    req.status = PLUGINXX_OPERATOR_OK;
                }
                return;
            }
            JS_FreeValue(ctx, statusVal);
        }
        req.result = valueToJsonString(ctx, v);
        req.status = PLUGINXX_OPERATOR_OK;
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
        // 桥属于哪一侧: 脚本据此决定注册什么 (`agentxx.side` 取值 "agent" | "client")
        JS_SetPropertyStr(
            ctx,
            bridge,
            "side",
            JS_NewString(ctx, isClientSide() ? "client" : "agent")
        );
        if (isClientSide()) {
            // client 侧桥子集: 展示 / 会话 / 自有信息 / 广播。
            // 注意: 渲染内容回调在 client io 线程同步进入 JS 线程, 脚本必须同步
            // 返回结果 (返回 Promise 按"没有自定义内容"处理)。
            def("log", B_LOG, 2);
            def("getOwnInfo", B_CLIENT_GET_OWN_INFO, 0);
            def("getPluginArgs", B_GET_PLUGIN_ARGS, 0);
            def("getPluginConfigPath", B_CLIENT_GET_CONFIG_PATH, 0);
            def("getLanguage", B_GET_LANGUAGE, 0);
            def("setLanguage", B_SET_LANGUAGE, 1);
            def("getClientState", B_CLIENT_GET_STATE, 0);
            def("sendUserInput", B_CLIENT_SEND_INPUT, 2);
            def("requestCancel", B_CLIENT_REQUEST_CANCEL, 1);
            def("sendPluginData", B_CLIENT_SEND_PLUGIN_DATA, 2);
            def("showToast", B_CLIENT_SHOW_TOAST, 2);
            def("updateToolDecor", B_CLIENT_UPDATE_TOOL_DECOR, 2);
            def("registerToolRenderer", B_CLIENT_REGISTER_TOOL_RENDERER, 1);
            def("unregisterToolRenderer", B_CLIENT_UNREGISTER_TOOL_RENDERER, 1);
            def("openOverlay", B_CLIENT_OPEN_OVERLAY, 1);
            def("closeOverlay", B_CLIENT_CLOSE_OVERLAY, 0);
            def("subscribe", B_SUBSCRIBE, 2);
            def("unsubscribe", B_UNSUBSCRIBE, 1);
            def("publish", B_PUBLISH, 2);
            def("setTimeout", B_SET_TIMEOUT, 2);
            def("clearTimeout", B_CLEAR_TIMEOUT, 1);
            // 展示面: 状态栏项 / 面板 / Info 段落 (句柄用 digit token 表示)
            def("registerStatusItem", B_CLIENT_REGISTER_STATUS_ITEM, 1);
            def("updateStatusItem", B_CLIENT_UPDATE_STATUS_ITEM, 2);
            def("unregisterStatusItem", B_CLIENT_UNREGISTER_STATUS_ITEM, 1);
            def("registerPanel", B_CLIENT_REGISTER_PANEL, 1);
            def("updatePanel", B_CLIENT_UPDATE_PANEL, 2);
            def("unregisterPanel", B_CLIENT_UNREGISTER_PANEL, 1);
            def("registerInfoSection", B_CLIENT_REGISTER_INFO_SECTION, 1);
            def("updateInfoSection", B_CLIENT_UPDATE_INFO_SECTION, 2);
            def("unregisterInfoSection", B_CLIENT_UNREGISTER_INFO_SECTION, 1);
            // 交互面: 斜杠命令 / 按钮动作 / 定时器 / 快捷键
            def("registerCommand", B_CLIENT_REGISTER_COMMAND, 1);
            def("unregisterCommand", B_CLIENT_UNREGISTER_COMMAND, 1);
            def("bindAction", B_CLIENT_BIND_ACTION, 2);
            def("unbindAction", B_CLIENT_UNBIND_ACTION, 1);
            def("setClientTimer", B_CLIENT_SET_TIMER, 2);
            def("cancelClientTimer", B_CLIENT_CANCEL_TIMER, 1);
            def("isRegionVisible", B_CLIENT_IS_REGION_VISIBLE, 1);
            def("registerKeybind", B_CLIENT_REGISTER_KEYBIND, 2);
            def("unregisterKeybind", B_CLIENT_UNREGISTER_KEYBIND, 1);
            def("listKeybinds", B_CLIENT_LIST_KEYBINDS, 0);
            // 网络 (`agentxx.net`): 与 agent 侧同名同义; 结果对象里 `ok` 是业务结论
            // - 同步入口用于快速操作; netFetch / netDownload / netCallAsync 返回 Promise
            def("netCall", B_NET_CALL, 2);
            def("netCallAsync", B_NET_CALL_ASYNC, 2);
            def("netCancel", B_NET_CANCEL, 1);
            def("netFetch", B_NET_FETCH, 2);
            def("netDownload", B_NET_DOWNLOAD, 2);
            def("netWsConnect", B_NET_WS_CONNECT, 1);
            def("netWsSend", B_NET_WS_SEND, 2);
            def("netWsClose", B_NET_WS_CLOSE, 3);
            def("netHttpServe", B_NET_HTTP_SERVE, 1);
            def("netHttpRespond", B_NET_HTTP_RESPOND, 1);
            def("netInfo", B_NET_INFO, 0);
            // 通用表: 配置 / 插件清单 / 广播声明与收尾 (与 agent 侧同名同义)
            def("getConfig", B_GET_CONFIG, 0);
            def("listPlugins", B_LIST_PLUGINS, 0);
            def("getPlugin", B_GET_PLUGIN, 1);
            def("declareBroadcast", B_DECLARE_BROADCAST, 1);
            def("readBroadcast", B_READ_BROADCAST, 1);
            def("endBroadcast", B_END_BROADCAST, 2);
        } else {
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
            // 会话资源扩展 (主程序 plugin_api v8): Skill/Memory/MCP 贡献
            def("addSkillDir", B_ADD_SKILL_DIR, 1);
            def("removeSkillDir", B_REMOVE_SKILL_DIR, 1);
            def("addMemoryFile", B_ADD_MEMORY_FILE, 1);
            def("removeMemoryFile", B_REMOVE_MEMORY_FILE, 1);
            def("addMcpServer", B_ADD_MCP_SERVER, 1);
            def("removeMcpServer", B_REMOVE_MCP_SERVER, 1);
            // 与 C++ 插件 SDK 同名同义的查询/声明接口
            def("scope", B_SCOPE, 0);
            def("getConfig", B_GET_CONFIG, 0);
            def("getPluginArgs", B_GET_PLUGIN_ARGS, 0);
            def("getLanguage", B_GET_LANGUAGE, 0);
            def("setLanguage", B_SET_LANGUAGE, 1);
            def("registerToolPermission", B_REGISTER_TOOL_PERMISSION, 1);
            def("unregisterToolPermission", B_UNREGISTER_TOOL_PERMISSION, 1);
            def("registerReadPathPermission", B_REGISTER_READ_PATH_PERMISSION, 2);
            def("registerWritePathPermission", B_REGISTER_WRITE_PATH_PERMISSION, 2);
            // 网络 (`agentxx.net`): 与 C++ 插件 SDK 同名; 结果对象里 `ok` 是业务结论
            def("netCall", B_NET_CALL, 2);
            def("netCallAsync", B_NET_CALL_ASYNC, 2);
            def("netCancel", B_NET_CANCEL, 1);
            def("netFetch", B_NET_FETCH, 2);
            def("netDownload", B_NET_DOWNLOAD, 2);
            def("netWsConnect", B_NET_WS_CONNECT, 1);
            def("netWsSend", B_NET_WS_SEND, 2);
            def("netWsClose", B_NET_WS_CLOSE, 3);
            def("netHttpServe", B_NET_HTTP_SERVE, 1);
            def("netHttpRespond", B_NET_HTTP_RESPOND, 1);
            def("netInfo", B_NET_INFO, 0);
            // 后台任务: 只能碰本实例提交的 (`agentxx.agent.runtime` v2)
            def("submitAsyncWork", B_SUBMIT_ASYNC_WORK, 1);
            def("cancelAsyncWork", B_CANCEL_ASYNC_WORK, 1);
            def("getAsyncWork", B_GET_ASYNC_WORK, 1);
            def("listAsyncWork", B_LIST_ASYNC_WORK, 1);
            // 提示词 / 会话 / 上下文 / 执行图 / 挂载查询
            def("getPrompt", B_GET_PROMPT, 0);
            def("setPrompt", B_SET_PROMPT, 1);
            def("getToolPrompt", B_GET_TOOL_PROMPT, 1);
            def("getSessionWorkDir", B_GET_SESSION_WORK_DIR, 1);
            def("isCanceled", B_IS_CANCELED, 1);
            def("getMessages", B_GET_MESSAGES, 1);
            def("messagesCount", B_MESSAGES_COUNT, 1);
            def("getGraphJson", B_GET_GRAPH_JSON, 0);
            def("getGraphName", B_GET_GRAPH_NAME, 0);
            def("setGraphJson", B_SET_GRAPH_JSON, 1);
            def("listMounts", B_LIST_MOUNTS, 0);
            def("getMount", B_GET_MOUNT, 1);
            // 会话运行时: 自建根 / 会话通道 / 触发器 / 定时器 (容器级接口)
            def("mountRoot", B_MOUNT_ROOT, 1);
            def("unmountRoot", B_UNMOUNT_ROOT, 1);
            def("openChannel", B_OPEN_CHANNEL, 1);
            def("closeChannel", B_CLOSE_CHANNEL, 1);
            def("sendInput", B_SEND_INPUT, 2);
            def("cancelChannel", B_CANCEL_CHANNEL, 1);
            def("selectModel", B_SELECT_MODEL, 2);
            def("respondInterrupt", B_RESPOND_INTERRUPT, 3);
            def("getChannelState", B_GET_CHANNEL_STATE, 1);
            def("getChannelMessages", B_GET_CHANNEL_MESSAGES, 1);
            def("listRuntime", B_LIST_RUNTIME, 0);
            def("registerTrigger", B_REGISTER_TRIGGER, 1);
            def("unregisterTrigger", B_UNREGISTER_TRIGGER, 1);
            def("registerTimer", B_REGISTER_TIMER, 1);
            def("unregisterTimer", B_UNREGISTER_TIMER, 1);
            def("emitEvent", B_EMIT_EVENT, 2);
            // 广播写入方 / share_store 写入 / 资源与模型查询
            def("declareBroadcast", B_DECLARE_BROADCAST, 1);
            def("readBroadcast", B_READ_BROADCAST, 1);
            def("endBroadcast", B_END_BROADCAST, 2);
            def("addShareStore", B_ADD_SHARE_STORE, 2);
            def("getOwnResources", B_GET_OWN_RESOURCES, 0);
            def("getModelConfig", B_GET_MODEL_CONFIG, 0);
            // 自身信息 / 配置文件路径 / 路径权限三态查询 (与 C++ 插件 SDK 同名)
            def("getOwnInfo", B_GET_OWN_INFO, 0);
            def("getPluginConfigPath", B_GET_CONFIG_PATH, 0);
            def("checkPaths", B_CHECK_PATHS, 3);
            def("filterPaths", B_FILTER_PATHS, 3);
            // 能力表: 登记实现行 (功能埋点实现行也是它) / 注销 / 自省 / 调用
            def("provideCapability", B_PROVIDE_CAPABILITY, 1);
            def("unprovideCapability", B_UNPROVIDE_CAPABILITY, 2);
            def("providePoint", B_PROVIDE_POINT, 3);
            def("listCapabilities", B_LIST_CAPABILITIES, 0);
            def("capabilityInfo", B_CAPABILITY_INFO, 1);
            def("invokeCapability", B_INVOKE_CAPABILITY, 3);
        }

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

    /// client 侧桥 C 函数 (同一入口按引擎侧分流; 由 [bridgeCall] 调用)
    static JSValue clientBridgeCall(
        JSContext*    ctx,
        int           argc,
        JSValueConst* argv,
        int           magic,
        JsPluginCtx*  pctx
    );

    /// 网络桥 (agent / client 两侧共用; `net` 取各自的 `<应用名>.net` 表)
    /// - 同步入口 (`netCall` / `netWsConnect` / `netHttpServe` …) 阻塞 JS 线程等结果;
    ///   网络服务自有线程, 不经主程序 io 线程, 所以同步回调里调也不会互等
    /// - 慢操作 (`netFetch` / `netDownload` / `netCallAsync`) 返回 Promise (总是 resolve)
    static JSValue netBridgeCall(
        JSContext*              ctx,
        JsPluginCtx*            pctx,
        int                     magic,
        int                     argc,
        JSValueConst*           argv,
        const PluginxxNetIface* net
    );

    // ==================== client 侧工具特化渲染 (io 线程 ↔ JS 线程) ====================

    /// 折叠内容回调 (client io 线程调用; 输出字符串由主程序释放)
    static int32_t PLUGINXX_CALL renderCollapsed(
        void*                         ud,
        const AgentxxToolRenderInput* input,
        AgentxxToolCollapsedOutput*   output
    );

    /// 展开内容回调 (client io 线程; 只在用户展开该工具行时调用)
    static int32_t PLUGINXX_CALL renderExpanded(
        void*                         ud,
        const AgentxxToolRenderInput* input,
        AgentxxToolExpandedOutput*    output
    );

    /// 把一次渲染调用投递到 JS 线程并同步等待结果
    ///
    /// - 返回 false = 没拿到 JS 结果 (引擎停止 / 超时 / JS 线程正在等主程序),
    ///   调用方按"这次没有自定义内容"处理
    /// - 超时后任务仍可能稍后执行: 槽由任务自己持有 (堆上), 不引用调用方栈
    /// - `reg` 的生命周期覆盖引擎 (摘除时移进 `retiredRenders_`, 不就地释放)
    bool invokeRenderSync(JsRenderReg* reg, const std::shared_ptr<JsRenderSync>& slot);

    /// 在 JS 线程执行一次渲染调用 (查实例 → 取脚本函数 → 调用 → 转换结果)
    void runRenderSyncOnJsThread(JsRenderReg* reg, const std::shared_ptr<JsRenderSync>& slot
    ) noexcept;

    /// 摘除一份渲染注册 (JS 线程调用): 标记失效并从索引里删除
    void retireRenderOnJsThread(const std::string& toolName);

    /// 释放一份渲染注册占用的脚本函数 (JS 线程调用)
    static void dropRenderCallbacksOnJsThread(JsPluginCtx* pctx, JsRenderReg* reg);

    // ==================== client 侧脚本回调 (命令 / 动作 / 定时器 / 快捷键) ====================

    /// 登记一个脚本回调函数, 返回主程序可长期持有的登记项
    /// - **只能 JS 线程调用** (`pctx->clientCallbacks` 只由该线程访问)
    JsClientCallback* newClientCallbackOnJsThread(JsPluginCtx* pctx, JSValueConst fn);

    /// 同步调用一个脚本回调 (命令 execute; 拿不到结果返回 false)
    bool invokeCallbackSync(JsClientCallback* cb, const std::string& argsJson, std::string& out);

    /// 投递式调用一个脚本回调 (动作 / 定时器 / 快捷键; fire-and-forget)
    void postCallback(JsClientCallback* cb, const std::string& argsJson);

    /// 在 JS 线程执行同步回调 (查实例 → 取脚本函数 → 调用 → 转换结果)
    void runCallbackSyncOnJsThread(JsClientCallback* cb, const std::shared_ptr<JsCallbackSync>& slot
    ) noexcept;

    /// 在 JS 线程执行投递式回调
    void runCallbackOnJsThread(JsClientCallback* cb, const std::string& argsJson) noexcept;

    /// 注销一个脚本回调 (JS 线程): 标记失效并释放脚本函数
    void dropClientCallbackOnJsThread(JsPluginCtx* pctx, JsClientCallback* cb);

    /// 按 `clientCbByKey` 的键注销并移除登记 (JS 线程)
    void dropClientCallbackByKeyOnJsThread(JsPluginCtx* pctx, const std::string& key);

    /// 命令 execute 回调 (client io 线程; 返回动作 JSON, 由主程序释放)
    static int32_t PLUGINXX_CALL commandExecute(
        void*                     ud,
        const PluginxxStringView* args_json,
        PluginxxString*           actionOut,
        PluginxxString*           errorOut
    );
    /// 按钮动作回调 (client io 线程; fire-and-forget)
    static void PLUGINXX_CALL uiActionFire(const AgentxxUiActionContext* ctx, void* ud);
    /// 定时器回调 (client io 线程)
    static void PLUGINXX_CALL timerFire(void* ud);
    /// 快捷键回调 (client io 线程)
    static void PLUGINXX_CALL keybindFire(void* ud);

    // ==================== 成员 ====================

    /// 本引擎服务的插件侧 (create 决定, 之后只读)
    Side side_ = Side::Agent;

    /// JS 线程正在调用主程序的层数
    ///
    /// io 线程的同步渲染回调看到非零就直接失败 —— 否则两边互等
    /// (io 线程等 JS 线程, JS 线程等 io 线程处理它的表调用)。
    std::atomic<int> jsHostCallDepth_{0};

    /// client 侧工具特化渲染注册索引 (工具名 → 注册项)
    ///
    /// 注册/注销在 JS 线程, 回调查找在 client io 线程, 因此用互斥保护。
    /// 摘除的注册项移进 `retiredRenders_` 而不就地释放: 它作为 `user_data`
    /// 被主程序持有, 可能还有回调在路上。
    std::mutex renderMtx_;
    std::map<std::string, std::shared_ptr<JsRenderReg>, std::less<>> renders_;
    std::vector<std::shared_ptr<JsRenderReg>>                      retiredRenders_;

    /// client 侧回调登记项 (命令 / 动作 / 定时器 / 快捷键)
    ///
    /// 主程序把 `JsClientCallback*` 当 `user_data` 长期持有, 因此不能在注销时释放:
    /// 脚本卸载只标 `dead`, 整数留给引擎析构。
    std::mutex                                        clientCbMtx_;
    std::vector<std::unique_ptr<JsClientCallback>>    clientCbs_;

    JSRuntime*                        rt_         = nullptr;
    const PluginxxHost*               engineHost_ = nullptr;
    JsWorkerThread                    thread_;
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
    mutable std::mutex lifecycleMtx_;
    State              state_ = State::Stopped;
    std::thread        stopper_; ///< 停止收尾线程 (join JS 线程 + 释放 runtime)
    std::vector<std::function<void()>> stopCompletions_; ///< 停止完成回调 (可能多个: 重复 stop)

    std::chrono::steady_clock::time_point taskStart_;

    /// 能力 "load" 的活动 op 占位 token (主程序只当不透明非空句柄; 每实例独立)
    int capOpToken_ = 0;

    /// 脚本实现行 (能力行) 与在飞调用
    /// - 主程序把 impl 地址当 `user_data`、把 call 地址当 op 句柄, 因此对象必须活到
    ///   引擎析构 (取消/卸载只标失效, 不就地释放)
    std::mutex                                  capMtx_;
    std::vector<std::shared_ptr<JsCapImpl>>     capImpls_;
    std::unordered_map<JsCapCall*, std::shared_ptr<JsCapCall>> capCalls_;
    std::unordered_map<JsCapInvokeBridge*, std::shared_ptr<JsCapInvokeBridge>> capInvokes_;

    /// 定时器集合版本号: 注册/清除/执行都会递增, 作为等待条件的唤醒依据
    /// (只做"是否变化"判断, 不代表业务语义)
    uint64_t timerEpoch_ = 0;

    /// 定时器 (setTimeout; JS 线程访问; fn 为 Dup 引用, 到期执行后 Free)
    struct JsTimer {
        std::chrono::steady_clock::time_point due;
        std::string                           instanceKey; ///< 所属实例
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

    /// JS 插件实例表 (JS 线程访问; 键 = 实例键 `脚本名@作用域键`)
    std::map<std::string, std::shared_ptr<JsPluginCtx>, std::less<>> plugins_;
    /// 脚本登记表 (仅 io 线程访问)
    std::map<std::string, ScriptEntry, std::less<>> scripts_;
    /// 已登记挂载作用域 (仅 io 线程访问)
    std::map<std::string, MountScope, std::less<>> scopes_;
    /// 跨线程镜像 <name, weak_ptr> (互斥锁保护)
    std::mutex                                                     mirrorMtx_;
    std::map<std::string, std::weak_ptr<JsPluginCtx>, std::less<>> mirror_;
    /// 后台任务事件登记 (主程序 io 线程回调时不解引用, 仅作释放记录)
    std::mutex                callbackMtx_;
    std::set<JsCallbackHook*> callbackHooks_;
};

/// 工具 execute 异步桥 (统一异步操作模型):
/// - start: io 线程调用 —— 打包请求入队 JS 线程, 立即返回 op 句柄
///   (execute_poll 留 NULL, 主程序只等完成通知)
/// - JS 线程执行完毕后经 notifier 上报结果 (线程安全; payload host->alloc)
void* JsEngine::toolExecuteStart(
    void*                         ud,
    const PluginxxStringView*     args_json,
    const PluginxxStringView*     thread_id,
    const PluginxxStringView*     tool_call_id,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    // C ABI 回调异常守卫: start 由主程序 io 线程调用 (请求打包/入队含分配),
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

        // op 句柄仅作占位标识 (主程序不解释其内容): 所有权随任务闭包移交 JS 线程,
        // 通知完成后由闭包自身释放 —— 支持同一JS 运行时插件被多实例加载/反复加载
        // (此前成功路径 new 后无人 delete, 每次工具执行泄漏句柄)
        auto*                  op      = new int(0);
        PluginxxOperatorNotify ntfCopy = *notify;
        if (!engine->post([engine, binding, req, ntfCopy, op]() {
                // ---- JS 线程: 执行并上报 (RAII 补充释放句柄) ----
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
                    ntfCopy.done(ntfCopy.host_ud, PLUGINXX_OPERATOR_CANCELED, &errSv);
                    return;
                }
                engine->doToolExecute(binding, *req);
                if (req->Canceled) {
                    // 引擎停止/取消: 终态为 Canceled, 不是普通失败
                    auto errSv = agentxx::plugin::PluginStringView::from(
                        req->error.data(),
                        req->error.size()
                    );
                    ntfCopy.done(ntfCopy.host_ud, PLUGINXX_OPERATOR_CANCELED, &errSv);
                } else if (!req->error.empty()) {
                    auto errSv = agentxx::plugin::PluginStringView::from(
                        req->error.data(),
                        req->error.size()
                    );
                    ntfCopy.done(ntfCopy.host_ud, PLUGINXX_OPERATOR_FAILED, &errSv);
                } else if (req->status != PLUGINXX_OPERATOR_OK) {
                    // 插件返回 `{status, text}`: 终态照搬, 正文就是给主程序读的结果
                    auto resSv = agentxx::plugin::PluginStringView::from(
                        req->result.data(),
                        req->result.size()
                    );
                    ntfCopy.done(ntfCopy.host_ud, req->status, &resSv);
                } else {
                    auto resSv = agentxx::plugin::PluginStringView::from(
                        req->result.data(),
                        req->result.size()
                    );
                    ntfCopy.done(ntfCopy.host_ud, PLUGINXX_OPERATOR_OK, &resSv);
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
        // 异常分类上报 + 经通知器上报失败 (主程序 io 线程等通知, 必须终结)
        ::agentxx::plugin::reportCurrentException([engine](const char* m) noexcept {
            engine->guardLog(m);
        });
        if (error_out) {
            error_out->data = nullptr;
            error_out->size = 0;
        }
        if (notify && notify->done) {
            auto errSv = agentxx::plugin::PluginStringView::from(nullptr, 0);
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &errSv);
        }
        return nullptr;
    }
}


void JsEngine::eventFire(const PluginxxStringView* event_json, void* ud) {
    auto* binding = static_cast<JsHookBinding*>(ud);
    auto* engine  = binding ? binding->engine : nullptr;
    // C ABI 回调异常守卫: 事件分发由主程序 io 线程调用, 异常不外泄
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

void JsEngine::eventEndFire(const PluginxxStringView* reason_json, void* ud) {
    auto* binding = static_cast<JsHookBinding*>(ud);
    auto* engine  = binding ? binding->engine : nullptr;
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
            std::string reason{
                reason_json && reason_json->data ? reason_json->data : "",
                reason_json ? static_cast<size_t>(reason_json->size) : 0
            };
            engine->post([engine, binding, reason]() {
                if (engine->stopping()) {
                    return; ///< 通知为 fire-and-forget: 停止中不再执行插件 JS
                }
                engine->doEventEndFire(binding, reason);
            });
        }
    );
}

// =====================================================================
// 能力行: 脚本实现的接口表能力 (agent / client 两侧共用)
// =====================================================================

void JsEngine::finishCapCall(JsCapCall* call, int32_t status, const std::string& payload) {
    if (call == nullptr) {
        return;
    }
    std::shared_ptr<JsCapCall> keep;
    {
        std::lock_guard<std::mutex> lk(capMtx_);
        auto                        it = capCalls_.find(call);
        if (it == capCalls_.end()) {
            return; ///< 已经收尾 (取消与正常完成相撞)
        }
        keep = it->second;
        capCalls_.erase(it);
    }
    if (keep->settled.exchange(true)) {
        return;
    }
    if (keep->notify.done) {
        PluginxxStringView view{};
        view.data = payload.empty() ? nullptr : payload.data();
        view.size = payload.size();
        keep->notify.done(keep->notify.host_ud, status, &view);
    }
}

void JsEngine::doCapCall(JsCapCall* call) {
    std::shared_ptr<JsCapCall> keep;
    {
        std::lock_guard<std::mutex> lk(capMtx_);
        auto                        it = capCalls_.find(call);
        if (it == capCalls_.end()) {
            return; ///< 已被取消
        }
        keep = it->second;
    }
    if (keep->settled.load()) {
        return;
    }
    auto pctx = findPlugin(keep->instanceKey);
    if (!pctx || pctx->deleted || !pctx->ctx) {
        finishCapCall(call, PLUGINXX_OPERATOR_FAILED, "plugin unloaded");
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

    auto fnIt = pctx->capCallbacks.find(keep->fnToken);
    if (fnIt == pctx->capCallbacks.end() || !JS_IsFunction(pctx->ctx, fnIt->second)) {
        finishCapCall(call, PLUGINXX_OPERATOR_FAILED, "capability handler is gone");
        return;
    }
    JSValue arg = JS_ParseJSON(
        pctx->ctx,
        keep->argsJson.empty() ? "{}" : keep->argsJson.c_str(),
        keep->argsJson.empty() ? 2 : keep->argsJson.size(),
        "<capability>"
    );
    if (JS_IsException(arg)) {
        JS_FreeValue(pctx->ctx, arg);
        arg = JS_NewStringLen(pctx->ctx, keep->argsJson.data(), keep->argsJson.size());
    }
    JSValue argv[1] = {arg};
    JSValue ret     = JS_Call(pctx->ctx, fnIt->second, JS_UNDEFINED, 1, argv);
    JS_FreeValue(pctx->ctx, arg);

    auto outcome = drivePromise(pctx->ctx, ret);
    JS_FreeValue(pctx->ctx, ret);
    switch (outcome.kind) {
        case PromiseOutcome::Kind::Value: {
            // 功能埋点形态按 `pointAnswer` 规范化 (`{"value": …}` / `{"disable": true}`),
            // 普通能力行原样给结果数据; `undefined` / `null` = 没意见 (空正文)
            std::string answer;
            if (!JS_IsUndefined(outcome.value) && !JS_IsNull(outcome.value)) {
                answer = valueToJsonString(pctx->ctx, outcome.value);
            }
            if (keep->pointMode && !answer.empty()) {
                bool raw = false;
                if (JS_IsObject(outcome.value)) {
                    JSValue valueV   = JS_GetPropertyStr(pctx->ctx, outcome.value, "value");
                    JSValue disableV = JS_GetPropertyStr(pctx->ctx, outcome.value, "disable");
                    raw              = !JS_IsUndefined(valueV) || !JS_IsUndefined(disableV);
                    JS_FreeValue(pctx->ctx, valueV);
                    JS_FreeValue(pctx->ctx, disableV);
                }
                if (!raw) {
                    answer = "{\"value\":" + answer + "}";
                }
            }
            finishCapCall(call, PLUGINXX_OPERATOR_OK, answer);
            break;
        }
        case PromiseOutcome::Kind::Rejected:
            finishCapCall(call, PLUGINXX_OPERATOR_FAILED, rejectedText(pctx->ctx, outcome));
            break;
        case PromiseOutcome::Kind::Timeout:
            finishCapCall(call, PLUGINXX_OPERATOR_FAILED, rejectedText(pctx->ctx, outcome));
            break;
        case PromiseOutcome::Kind::Canceled:
            finishCapCall(call, PLUGINXX_OPERATOR_CANCELED, std::string{});
            break;
    }
    JS_FreeValue(pctx->ctx, outcome.value);
}

void* PLUGINXX_CALL JsEngine::capImplStart(
    void*                         ctx,
    const PluginxxHost*           caller_host,
    const PluginxxStringView*     method,
    const PluginxxStringView*     args_json,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    (void)caller_host;
    (void)method;
    (void)error_out;
    auto* impl = static_cast<JsCapImpl*>(ctx);
    if (impl == nullptr || impl->engine == nullptr || !notify || !notify->done) {
        return nullptr;
    }
    auto*         engine = impl->engine;
    const int64_t token  = impl->fnToken.load(std::memory_order_acquire);
    if (impl->dead.load(std::memory_order_acquire) || token == 0) {
        return nullptr; ///< 行已注销: 主程序按"提供者不可用"跳过
    }
    auto call           = std::make_shared<JsCapCall>();
    call->engine        = engine;
    call->instanceKey   = impl->instanceKey;
    call->capId         = impl->capId;
    call->fnToken       = token;
    call->pointMode     = impl->pointMode;
    call->argsJson.assign(
        args_json && args_json->data ? args_json->data : "",
        args_json ? static_cast<size_t>(args_json->size) : 0
    );
    call->notify = *notify;
    JsCapCall* raw = call.get();
    {
        std::lock_guard<std::mutex> lk(engine->capMtx_);
        engine->capCalls_[raw] = call;
    }
    if (!engine->post([engine, raw]() {
            if (engine->stopping()) {
                engine->finishCapCall(raw, PLUGINXX_OPERATOR_CANCELED, std::string{});
                return;
            }
            engine->doCapCall(raw);
        })) {
        // 引擎已停止: 不回调 notify (主程序按启动失败处理), 只丢掉这次调用
        std::lock_guard<std::mutex> lk(engine->capMtx_);
        engine->capCalls_.erase(raw);
        return nullptr;
    }
    return raw;
}

void PLUGINXX_CALL JsEngine::capImplCancel(void* ctx, void* op) {
    auto* impl = static_cast<JsCapImpl*>(ctx);
    if (impl == nullptr || impl->engine == nullptr || op == nullptr) {
        return;
    }
    impl->engine->finishCapCall(static_cast<JsCapCall*>(op), PLUGINXX_OPERATOR_CANCELED, std::string{});
}

void JsEngine::unprovideCapabilityRow(
    JSContext*                       ctx,
    JsPluginCtx*                     pctx,
    const PluginxxCapabilitiesIface* caps,
    const std::string&               id,
    const std::string&               ownerTag
) {
    if (caps == nullptr || caps->unregister_capability == nullptr || pctx == nullptr) {
        return;
    }
    std::vector<std::shared_ptr<JsCapImpl>> matched;
    {
        std::lock_guard<std::mutex> lk(capMtx_);
        for (auto& impl : capImpls_) {
            if (!impl->dead.load() && impl->instanceKey == pctx->instanceKey
                && impl->capId == id && impl->ownerTag == ownerTag) {
                matched.push_back(impl);
            }
        }
    }
    for (auto& impl : matched) {
        impl->dead.store(true);
        const int64_t token = impl->fnToken.exchange(0);
        if (token != 0 && pctx->ctx != nullptr) {
            auto it = pctx->capCallbacks.find(token);
            if (it != pctx->capCallbacks.end()) {
                JS_FreeValue(ctx, it->second);
                pctx->capCallbacks.erase(it);
            }
        }
        auto idSv  = agentxx::plugin::PluginStringView::from(id.data(), id.size());
        auto tagSv = agentxx::plugin::PluginStringView::from(ownerTag.data(), ownerTag.size());
        // 用**脚本实例**的主程序句柄: 行归属于脚本所在插件 (不能落到引擎实例名下)
        caps->unregister_capability(pctx->host, &idSv, &tagSv);
    }
}

int32_t JsEngine::provideCapabilityRow(
    JSContext*                       ctx,
    JsPluginCtx*                     pctx,
    const PluginxxCapabilitiesIface* caps,
    const std::string&               id,
    const std::string&               ownerTag,
    JSValueConst                     handler,
    bool                             pointMode,
    int32_t                          impls,
    int32_t                          merge,
    uint32_t                         phases,
    int32_t                          priority,
    int32_t                          timeoutMs,
    std::string*                     error
) {
    if (caps == nullptr || caps->register_capability == nullptr || pctx == nullptr) {
        if (error != nullptr) {
            *error = "host lacks capabilities interface";
        }
        return PLUGINXX_RESULT_ERR_INTERNAL;
    }
    if (id.empty() || !JS_IsFunction(ctx, handler)) {
        if (error != nullptr) {
            *error = "capability id and handler function required";
        }
        return PLUGINXX_RESULT_ERR_ARG;
    }
    // 同一写法的重复登记按覆盖处理: 先撤掉旧行 (后登记者生效)
    unprovideCapabilityRow(ctx, pctx, caps, id, ownerTag);

    auto impl             = std::make_shared<JsCapImpl>();
    impl->engine          = this;
    impl->instanceKey     = pctx->instanceKey;
    impl->capId           = id;
    impl->ownerTag        = ownerTag;
    impl->pointMode       = pointMode;
    const int64_t token   = ++pctx->nextCapToken;
    pctx->capCallbacks[token] = JS_DupValue(ctx, handler);
    impl->fnToken.store(token, std::memory_order_release);

    auto idSv  = agentxx::plugin::PluginStringView::from(id.data(), id.size());
    auto tagSv = agentxx::plugin::PluginStringView::from(ownerTag.data(), ownerTag.size());
    PluginxxCapabilitySpec spec{};
    spec.version     = PLUGINXX_API_VERSION;
    spec.struct_size = sizeof(PluginxxCapabilitySpec);
    spec.id          = idSv;
    spec.owner_tag   = tagSv;
    spec.impls       = impls;
    spec.merge       = merge;
    spec.phases      = phases;
    spec.phase       = PLUGINXX_CAPABILITY_PHASE_IMPL;
    spec.priority    = priority;
    // 脚本回调在 JS 线程执行: 一律登记成**异步处理器** (返回 token, 稍后 notify->done)
    spec.sync       = 0;
    spec.timeout_ms = timeoutMs;
    spec.run_start  = &JsEngine::capImplStart;
    spec.cancel     = &JsEngine::capImplCancel;
    spec.user_data  = impl.get();

    PluginxxString errOut{nullptr, 0};
    // 行归属于**脚本实例**的插件 (pctx->host), 不是引擎实例: 声明/注销/归属
    // 都跟着脚本所在插件的挂载走 (引擎自己只拥有 `interpreter.js` 能力行)
    const PluginxxHost* instanceHost = pctx->host != nullptr ? pctx->host : engineHost_;
    const int32_t       rc = caps->register_capability(
        instanceHost,
        pctx->scope(),
        &spec,
        &errOut
    );
    if (errOut.data) {
        if (error != nullptr) {
            error->assign(errOut.data, static_cast<size_t>(errOut.size));
        }
        agentxx::plugin::PluginString::free(instanceHost, &errOut);
    }
    if (rc != PLUGINXX_RESULT_OK) {
        // 登记失败: 收回回调 token 与行标记 (不留半截状态)
        impl->dead.store(true);
        impl->fnToken.store(0);
        auto it = pctx->capCallbacks.find(token);
        if (it != pctx->capCallbacks.end()) {
            JS_FreeValue(ctx, it->second);
            pctx->capCallbacks.erase(it);
        }
        return rc;
    }
    {
        std::lock_guard<std::mutex> lk(capMtx_);
        capImpls_.push_back(impl);
    }
    // 脚本卸载 / 顶层异常回滚: 撤销这一行 (主程序通常已按挂载记录撤销, 幂等)
    JsCapImpl*    rawImpl = impl.get();
    JsPluginCtx*  rawCtx  = pctx;
    pctx->rollbackActions.push_back([this, caps, rawImpl, rawCtx, token, id, ownerTag]() {
        rawImpl->dead.store(true);
        rawImpl->fnToken.store(0);
        if (rawCtx->ctx != nullptr) {
            auto it = rawCtx->capCallbacks.find(token);
            if (it != rawCtx->capCallbacks.end()) {
                JS_FreeValue(rawCtx->ctx, it->second);
                rawCtx->capCallbacks.erase(it);
            }
        }
        auto idSv2  = agentxx::plugin::PluginStringView::from(id.data(), id.size());
        auto tagSv2 = agentxx::plugin::PluginStringView::from(ownerTag.data(), ownerTag.size());
        caps->unregister_capability(rawCtx->host, &idSv2, &tagSv2);
    });
    return 0;
}

void PLUGINXX_CALL JsEngine::capInvokeDone(
    void*                     ud,
    int32_t                   status,
    const PluginxxStringView* payload
) {
    auto* bridge = static_cast<JsCapInvokeBridge*>(ud);
    if (bridge == nullptr || bridge->engine == nullptr) {
        return;
    }
    std::string text{
        payload && payload->data ? payload->data : "",
        payload ? static_cast<size_t>(payload->size) : 0
    };
    auto* engine = bridge->engine;
    engine->post([engine, bridge, status, text]() {
        engine->settleCapInvoke(bridge, status, text);
    });
}

void JsEngine::settleCapInvoke(JsCapInvokeBridge* bridge, int32_t status, std::string payload) {
    if (bridge == nullptr) {
        return;
    }
    std::shared_ptr<JsCapInvokeBridge> keep;
    {
        std::lock_guard<std::mutex> lk(capMtx_);
        auto                        it = capInvokes_.find(bridge);
        if (it == capInvokes_.end()) {
            return; ///< 已结算 (引擎停止时已丢弃)
        }
        keep = it->second;
        capInvokes_.erase(it);
    }
    if (keep->settled.exchange(true) || !keep->ctx) {
        return;
    }
    JSContext* ctx = keep->ctx;
    JSValue    obj = JS_NewObject(ctx);
    const bool ok  = status == PLUGINXX_OPERATOR_OK;
    JS_SetPropertyStr(ctx, obj, "ok", ok ? JS_TRUE : JS_FALSE);
    JS_SetPropertyStr(
        ctx,
        obj,
        "status",
        JS_NewString(
            ctx,
            status == PLUGINXX_OPERATOR_OK         ? "ok"
                : status == PLUGINXX_OPERATOR_CANCELED ? "canceled"
                                                     : "failed"
        )
    );
    if (ok) {
        JS_SetPropertyStr(ctx, obj, "value", jsonTextToValue(ctx, payload, "<cap_value>"));
    } else if (!payload.empty()) {
        JS_SetPropertyStr(ctx, obj, "error", JS_NewStringLen(ctx, payload.data(), payload.size()));
    }
    JSValue rc = JS_Call(ctx, keep->resolve, JS_UNDEFINED, 1, &obj);
    JS_FreeValue(ctx, rc);
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, keep->resolve);
    JS_FreeValue(ctx, keep->reject);
    keep->resolve = JS_UNDEFINED;
    keep->reject  = JS_UNDEFINED;
}

namespace {

JsPluginCtx::~JsPluginCtx() {
    if (ctx) {
        if (!JS_IsUndefined(tools)) {
            JS_FreeValue(ctx, tools);
        }
        if (!JS_IsUndefined(agents)) {
            JS_FreeValue(ctx, agents);
        }
        for (auto& [token, fn] : asyncWorkCallbacks) {
            (void)token;
            JS_FreeValue(ctx, fn);
        }
        asyncWorkCallbacks.clear();
        for (auto& [token, fn] : channelCallbacks) {
            (void)token;
            JS_FreeValue(ctx, fn);
        }
        channelCallbacks.clear();
        for (auto& [token, fn] : capCallbacks) {
            (void)token;
            JS_FreeValue(ctx, fn);
        }
        capCallbacks.clear();
        for (auto& [token, fn] : clientCallbacks) {
            (void)token;
            JS_FreeValue(ctx, fn);
        }
        clientCallbacks.clear();
        clientHandles.clear(); ///< 句柄由主程序回收 (实例卸载时统一摘除), 这里只丢记录
        clientSubs.clear();    ///< 同上
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
    auto* engine = pctx->engine;
    if (engine->isClientSide()) {
        // client 侧脚本: 只用 client 表 (agent 专有表在客户端不存在)
        return clientBridgeCall(ctx, argc, argv, magic, pctx);
    }
    const PluginxxHost* host = pctx->host;
    const auto&         vt   = *host->vtable; // 核心: alloc/free/strdup
    (void)vt;
    // COM 风格接口表查询 (进程级静态数据; 各能力经稳定 IID 分发)
    const agentxx::plugin::AgentIfaces iface  = agentxx::plugin::AgentIfaces::query(host);
    (void)engine;

    /// 订阅一个频道并把 (topic / handler / 主程序订阅句柄) 记进 agents 数组
    /// - [on_end] 为函数时走 `subscribe_broadcast_ex` (频道被结束时回调一次);
    ///   主程序没有该成员时退回 `subscribe_broadcast` (脚本收不到流结束而已)
    /// - [optionsJson] 形如 `{"intervalMs":200,"mode":"delay"}` (空 = 不限频)
    /// - `return` token (>= 0, 之后传给 unsubscribe / offHook); -1 = 主程序拒绝订阅
    auto subscribeChannel = [&](const std::string& topic,
                                JSValueConst       fn,
                                JSValueConst       on_end,
                                const std::string& optionsJson) -> int32_t {
        auto binding    = std::make_unique<JsHookBinding>();
        binding->engine = engine;
        binding->instanceKey = pctx->instanceKey;
        binding->topic  = topic;
        auto topicSv = agentxx::plugin::PluginStringView::from(topic.data(), topic.size());
        auto optSv   = agentxx::plugin::PluginStringView::from(
            optionsJson.data(),
            optionsJson.size()
        );
        const PluginxxStringView* optPtr = optionsJson.empty() ? nullptr : &optSv;
        PluginxxSubscription*     sub    = nullptr;
        if (JS_IsFunction(ctx, on_end) && iface.broadcasts->subscribe_broadcast_ex) {
            sub = iface.broadcasts->subscribe_broadcast_ex(
                host,
                pctx->scope(),
                &topicSv,
                &JsEngine::eventFire,
                &JsEngine::eventEndFire,
                binding.get(),
                optPtr,
                nullptr
            );
        } else {
            sub = iface.broadcasts->subscribe_broadcast(
                host,
                pctx->scope(),
                &topicSv,
                &JsEngine::eventFire,
                binding.get(),
                optPtr,
                nullptr
            );
        }
        if (!sub) {
            return -1;
        }
        // token = agents 数组下标
        JSValue  lenVal = JS_GetPropertyStr(ctx, pctx->agents, "length");
        uint32_t len    = 0;
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        binding->token = static_cast<int64_t>(len);
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "topic", JS_NewString(ctx, topic.c_str()));
        JS_SetPropertyStr(ctx, entry, "handler", JS_DupValue(ctx, fn));
        if (JS_IsFunction(ctx, on_end)) {
            // 主程序没有 subscribe_broadcast_ex 时只存不用 (不报错, 只是收不到流结束)
            JS_SetPropertyStr(ctx, entry, "onEnd", JS_DupValue(ctx, on_end));
        }
        JS_SetPropertyStr(ctx, entry, "token", JS_NewInt32(ctx, static_cast<int32_t>(len)));
        // 主程序订阅句柄: 拆高低 32 位存储 (JS number 为 double, 直接存指针
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
        pctx->rollbackActions.push_back([bc = iface.broadcasts, sub]() {
            if (bc && bc->unsubscribe_broadcast) {
                bc->unsubscribe_broadcast(sub);
            }
        });
        pctx->hookBindings.push_back(std::move(binding));
        return static_cast<int32_t>(len);
    };

    /// 退订一个 token (agents 数组下标); 已经是空的就什么也不做
    auto unsubscribeToken = [&](uint32_t token) {
        JSValue entry = JS_GetPropertyUint32(ctx, pctx->agents, token);
        if (JS_IsObject(entry)) {
            // 释放主程序订阅 (防频道继续投递到已退订的 JS 插件;
            // 退订后主程序不再回调, 插件卸载时也无残留)
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
                if (iface.broadcasts && iface.broadcasts->unsubscribe_broadcast) {
                    iface.broadcasts->unsubscribe_broadcast(
                        reinterpret_cast<PluginxxSubscription*>(subPtr)
                    );
                }
            }
            JS_SetPropertyUint32(ctx, pctx->agents, token, JS_UNDEFINED);
        }
        JS_FreeValue(ctx, entry);
    };

    /// 取消订阅某个频道的全部处理器
    auto unsubscribeTopic = [&](const std::string& topic) {
        JSValue  lenVal = JS_GetPropertyStr(ctx, pctx->agents, "length");
        uint32_t len    = 0;
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (uint32_t i = 0; i < len; ++i) {
            JSValue entry = JS_GetPropertyUint32(ctx, pctx->agents, i);
            if (!JS_IsObject(entry)) {
                JS_FreeValue(ctx, entry);
                continue;
            }
            JSValue     topicVal = JS_GetPropertyStr(ctx, entry, "topic");
            std::string entryTopic;
            if (JS_IsString(topicVal)) {
                entryTopic = jsToCppString(ctx, topicVal);
            }
            JS_FreeValue(ctx, topicVal);
            JS_FreeValue(ctx, entry);
            if (entryTopic == topic) {
                unsubscribeToken(i);
            }
        }
    };

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

            // 工具绑定 (主程序 execute 回调 → 本插件)
            auto binding    = std::make_unique<JsToolBinding>();
            binding->engine = engine;
            binding->instanceKey = pctx->instanceKey;
            binding->name   = name;

            AgentxxPluginToolSpec spec{};
            spec.struct_size = sizeof(AgentxxPluginToolSpec);
            spec.name        = agentxx::plugin::PluginStringView::from(name.data(), name.size());
            spec.description = agentxx::plugin::PluginStringView::from(desc.data(), desc.size());
            spec.parameters_json
                = agentxx::plugin::PluginStringView::from(paramsJson.data(), paramsJson.size());
            // 统一异步操作模型: 异步桥 (JS 线程完成时经通知器上报)
            spec.execute_start  = &JsEngine::toolExecuteStart;
            spec.execute_cancel = nullptr;
            spec.user_data      = binding.get();
            int rc              = iface.tools->register_tool(host, pctx->scope(), &spec);
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

            // 脚本初始化事务: 注册成功登记撤销动作 (顶层失败/实例卸载时回滚)
            pctx->rollbackActions.push_back([host, toolsIface = iface.tools, pctxRaw = pctx, name]() {
                auto nameSv = agentxx::plugin::PluginStringView::from(name.data(), name.size());
                toolsIface->unregister_tool(host, pctxRaw->scope(), &nameSv);
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
            iface.tools->unregister_tool(host, pctx->scope(), &nameSv);
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

            // callTool 始终返回 Promise, 完成事件回到 JS 线程 settle。
            // - 本引擎 JS 工具: 同线程执行并把结果/内部 Promise 链到外层 Promise
            //   (不阻塞等待, 内部 Promise 由外层驱动的 job/timer/队列泵推进)
            // - 主程序插件工具: call_tool_async + 完成回调投递回 JS 线程 (不再
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

            // 2) 主程序插件工具: 异步互调, 完成回调经 JS 线程 settle
            auto* bridge = new JsCallBridge{engine, ctx, resolving[0], resolving[1]};
            auto  nameSv = agentxx::plugin::PluginStringView::from(name.data(), name.size());
            auto argsSv = agentxx::plugin::PluginStringView::from(argsJson.data(), argsJson.size());
            auto sidSv
                = agentxx::plugin::PluginStringView::from(sessionId.data(), sessionId.size());
            PluginxxString err{nullptr, 0};
            auto*          handle = iface.tools->call_tool_async(
                host,
                pctx->scope(),
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
            PluginxxString resp{nullptr, 0};
            auto           sidSv
                = agentxx::plugin::PluginStringView::from(sessionId.data(), sessionId.size());
            iface.session->get_share_store(host, pctx->scope(), &sidSv, id, &resp);
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
            iface.session->emit_message_tip(host, pctx->scope(), &sidSv, &txtSv, level);
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
            // onHook(point, fn): 通知点已是"通知型功能埋点", 等价于订阅同名广播频道
            // (序号约定不变: 0 = 轮次开始 … 6 = 工具结束)
            if (argc < 2 || !JS_IsNumber(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
                return JS_ThrowTypeError(ctx, "onHook: (point, fn) required");
            }
            int32_t point = 0;
            JS_ToInt32(ctx, &point, argv[0]);
            const auto pointId = hookPointIdOf(point);
            if (pointId.empty()) {
                return JS_ThrowTypeError(ctx, "onHook: point out of range");
            }
            const std::string topic{pointId};
            // 覆盖语义: 同一个点的旧处理器先退订 (每插件每个点至多一个)
            unsubscribeTopic(topic);
            const int32_t token = subscribeChannel(topic, argv[1], JS_UNDEFINED, std::string{});
            if (token < 0) {
                return throwJsError(ctx, "onHook: host subscription failed");
            }
            return JS_NewInt32(ctx, token);
        }

        case B_UNREGISTER_HOOK: {
            if (argc < 1 || !JS_IsNumber(argv[0])) {
                return JS_ThrowTypeError(ctx, "offHook: point required");
            }
            int32_t point = 0;
            JS_ToInt32(ctx, &point, argv[0]);
            const auto pointId = hookPointIdOf(point);
            if (!pointId.empty()) {
                unsubscribeTopic(std::string{pointId});
            }
            return JS_UNDEFINED;
        }

        case B_SUBSCRIBE: {
            if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
                return JS_ThrowTypeError(ctx, "subscribe: (topic, handler, options?) required");
            }
            const std::string topic = jsToCppString(ctx, argv[0]);
            // 可选第三参: {intervalMs, mode, onEnd} —— 与 C++ 侧
            // `subscribePluginBroadcast` + `subscribe_broadcast_ex` 同一套语义
            JSValue     onEnd = JS_UNDEFINED;
            std::string optionsJson;
            if (argc >= 3 && JS_IsObject(argv[2])) {
                optionsJson      = subscribeOptionsJsonOf(ctx, argv[2]);
                JSValue onEndVal = JS_GetPropertyStr(ctx, argv[2], "onEnd");
                if (JS_IsFunction(ctx, onEndVal)) {
                    onEnd = onEndVal; ///< 接管引用, 下面 Free
                } else {
                    JS_FreeValue(ctx, onEndVal);
                }
            }
            const int32_t token = subscribeChannel(topic, argv[1], onEnd, optionsJson);
            JS_FreeValue(ctx, onEnd);
            if (token < 0) {
                return throwJsError(
                    ctx,
                    fmt::format("subscribe: host subscription failed: {}", topic)
                );
            }
            return JS_NewInt32(ctx, token);
        }

        case B_UNSUBSCRIBE: {
            if (argc < 1 || !JS_IsNumber(argv[0])) {
                return JS_ThrowTypeError(ctx, "unsubscribe: token required");
            }
            uint32_t token = 0;
            JS_ToUint32(ctx, &token, argv[0]);
            unsubscribeToken(token);
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
            agentxx::plugin::publishPluginBroadcast(host, iface.broadcasts, topicSv, paySv);
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
            t.instanceKey       = pctx->instanceKey;
            t.fn                = JS_DupValue(ctx, argv[0]);
            engine->timers_[id] = std::move(t);
            ++engine->timerEpoch_; // 定时器集合变化: 唤醒等待中的 JS 线程重算
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
            PluginxxString json{nullptr, 0};
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
            std::string    name = jsToCppString(ctx, argv[0]);
            PluginxxString json{nullptr, 0};
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
        // - 注册失败 (与主配置 yaml/其他插件冲突或主程序不支持) 抛 JS 异常;
        //   注销失败 (不存在/不属于本插件) 返回 false
        case B_ADD_SKILL_DIR:
        case B_ADD_MEMORY_FILE: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "path string required");
            }
            std::string p   = jsToCppString(ctx, argv[0]);
            auto        pSv = agentxx::plugin::PluginStringView::fromCstr(p.c_str());
            int rc = (magic == B_ADD_SKILL_DIR) ? iface.resources->register_skill_dir(host, pctx->scope(), &pSv)
                                                : iface.resources->register_memory_file(host, pctx->scope(), &pSv);
            if (rc != 0) {
                return throwJsError(
                    ctx,
                    fmt::format("register failed (conflict or unsupported): {}", p)
                );
            }
            const bool isSkillDir = (magic == B_ADD_SKILL_DIR);
            pctx->rollbackActions.push_back([host, resIface = iface.resources, pctxRaw = pctx, p, isSkillDir]() {
                auto pSv2 = agentxx::plugin::PluginStringView::from(p.data(), p.size());
                if (isSkillDir) {
                    resIface->unregister_skill_dir(host, pctxRaw->scope(), &pSv2);
                } else {
                    resIface->unregister_memory_file(host, pctxRaw->scope(), &pSv2);
                }
            });
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
                                  ? (iface.resources->unregister_skill_dir(host, pctx->scope(), &pSv) == 0)
                                  : (iface.resources->unregister_memory_file(host, pctx->scope(), &pSv) == 0);
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
            // spec JSON 拼装经主程序 json_escape (防注入/转义错误; 失败时 SDK 内部回退为本地转义)
            if (!iface.json) {
                return JS_ThrowInternalError(ctx, "addMcpServer: json interface unavailable");
            }
            long long   t    = static_cast<long long>(timeoutSec < 0 ? 0 : timeoutSec);
            std::string spec = fmt::format(
                "{{\"namespace\":{},\"url\":{},\"timeout\":{}}}",
                agentxx::plugin::jsonEscape(host, iface.json, ns),
                agentxx::plugin::jsonEscape(host, iface.json, url),
                t
            );
            auto specSv = agentxx::plugin::PluginStringView::fromCstr(spec.c_str());
            if (iface.resources->register_mcp_server(host, pctx->scope(), &specSv) != 0) {
                return throwJsError(
                    ctx,
                    fmt::format("addMcpServer register failed (conflict?): {}", ns)
                );
            }
            pctx->rollbackActions.push_back([host, resIface = iface.resources, pctxRaw = pctx, ns]() {
                auto nsSv2 = agentxx::plugin::PluginStringView::from(ns.data(), ns.size());
                resIface->unregister_mcp_server(host, pctxRaw->scope(), &nsSv2);
            });
            return JS_TRUE;
        }

        case B_REMOVE_MCP_SERVER: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "removeMcpServer: namespace required");
            }
            std::string ns   = jsToCppString(ctx, argv[0]);
            auto        nsSv = agentxx::plugin::PluginStringView::from(ns.data(), ns.size());
            return iface.resources->unregister_mcp_server(host, pctx->scope(), &nsSv) == 0 ? JS_TRUE : JS_FALSE;
        }

        // ---- 与 C++ 插件 SDK 对齐的查询/声明接口 ----
        case B_SCOPE: {
            // 本脚本实例所属的 agent 挂载 (与 C++ 侧的 PluginxxScopeRef 同义)
            JSValue out = JS_NewObject(ctx);
            JS_SetPropertyStr(
                ctx,
                out,
                "agentId",
                JS_NewStringLen(ctx, pctx->scopeAgentId.data(), pctx->scopeAgentId.size())
            );
            JS_SetPropertyStr(
                ctx,
                out,
                "mountSeq",
                JS_NewInt64(ctx, static_cast<int64_t>(pctx->scopeSeq))
            );
            return out;
        }

        case B_GET_CONFIG:
        case B_GET_PLUGIN_ARGS:
        case B_GET_LANGUAGE: {
            if (!iface.config) {
                return JS_NULL;
            }
            PluginxxString out{nullptr, 0};
            int32_t        rc = -1;
            if (magic == B_GET_CONFIG) {
                rc = iface.config->get_config(host, &out);
            } else if (magic == B_GET_PLUGIN_ARGS) {
                rc = iface.config->get_plugin_args(host, &out);
            } else {
                rc = iface.config->get_language(host, &out);
            }
            if (rc != 0 || !out.data) {
                return JS_NULL;
            }
            JSValue result = JS_ParseJSON(
                ctx,
                out.data,
                static_cast<size_t>(out.size),
                "<config>"
            );
            if (JS_IsException(result)) {
                JS_FreeValue(ctx, result);
                result = JS_NewStringLen(ctx, out.data, static_cast<size_t>(out.size));
            }
            agentxx::plugin::PluginString::free(host, &out);
            return result;
        }

        case B_SET_LANGUAGE: {
            if (!iface.config || argc < 1 || !JS_IsString(argv[0])) {
                return JS_FALSE;
            }
            std::string lang  = jsToCppString(ctx, argv[0]);
            auto        langSv = agentxx::plugin::PluginStringView::from(lang.data(), lang.size());
            return iface.config->set_language(host, &langSv) == 0 ? JS_TRUE : JS_FALSE;
        }

        case B_REGISTER_TOOL_PERMISSION:
        case B_REGISTER_READ_PATH_PERMISSION:
        case B_REGISTER_WRITE_PATH_PERMISSION: {
            // C++ 侧同名便捷函数: 读/写路径权限 = (toolName, argName) 的简写
            if (!iface.permission || !iface.permission->register_tool_permission) {
                return JS_FALSE; // 主程序未加载权限中间件: 附加能力, 忽略
            }
            std::string toolName, targetArg, category;
            int32_t     scope  = AGENTXX_PLUGIN_PERMISSION_SCOPE_READ;
            int32_t     target = AGENTXX_PLUGIN_PERMISSION_TARGET_PATH;
            if (magic == B_REGISTER_TOOL_PERMISSION) {
                if (argc < 1 || !JS_IsObject(argv[0])) {
                    return JS_ThrowTypeError(
                        ctx,
                        "registerToolPermission: spec object required"
                    );
                }
                auto specObj  = argv[0];
                toolName      = jsToCppString(ctx, JS_GetPropertyStr(ctx, specObj, "toolName"));
                targetArg     = jsToCppString(ctx, JS_GetPropertyStr(ctx, specObj, "targetArg"));
                category      = jsToCppString(ctx, JS_GetPropertyStr(ctx, specObj, "category"));
                std::string scopeText
                    = jsToCppString(ctx, JS_GetPropertyStr(ctx, specObj, "scope"));
                std::string targetText
                    = jsToCppString(ctx, JS_GetPropertyStr(ctx, specObj, "target"));
                scope = scopeText == "write" ? AGENTXX_PLUGIN_PERMISSION_SCOPE_WRITE
                                             : AGENTXX_PLUGIN_PERMISSION_SCOPE_READ;
                if (targetText == "none") {
                    target = AGENTXX_PLUGIN_PERMISSION_TARGET_NONE;
                } else if (targetText == "text") {
                    target = AGENTXX_PLUGIN_PERMISSION_TARGET_TEXT;
                }
            } else {
                if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
                    return JS_ThrowTypeError(
                        ctx,
                        "registerReadPathPermission/registerWritePathPermission: "
                        "(toolName, argName) required"
                    );
                }
                toolName = jsToCppString(ctx, argv[0]);
                targetArg = jsToCppString(ctx, argv[1]);
                scope = magic == B_REGISTER_WRITE_PATH_PERMISSION
                            ? AGENTXX_PLUGIN_PERMISSION_SCOPE_WRITE
                            : AGENTXX_PLUGIN_PERMISSION_SCOPE_READ;
            }
            if (toolName.empty()) {
                return JS_ThrowTypeError(ctx, "registerToolPermission: toolName required");
            }
            AgentxxPluginToolPermissionSpec spec{};
            spec.struct_size = sizeof(AgentxxPluginToolPermissionSpec);
            spec.tool_name
                = agentxx::plugin::PluginStringView::from(toolName.data(), toolName.size());
            spec.scope       = scope;
            spec.target_kind = target;
            spec.target_arg
                = agentxx::plugin::PluginStringView::from(targetArg.data(), targetArg.size());
            spec.category
                = agentxx::plugin::PluginStringView::from(category.data(), category.size());
            if (iface.permission->register_tool_permission(host, pctx->scope(), &spec) != 0) {
                return JS_FALSE;
            }
            // 实例卸下时撤销声明 (主程序也会按挂载记录撤销, 这里保持两侧一致)
            pctx->rollbackActions.push_back(
                [host, permIface = iface.permission, pctxRaw = pctx, toolName]() {
                    if (!permIface || !permIface->unregister_tool_permission) {
                        return;
                    }
                    auto nameSv
                        = agentxx::plugin::PluginStringView::from(toolName.data(), toolName.size());
                    permIface->unregister_tool_permission(host, pctxRaw->scope(), &nameSv);
                }
            );
            return JS_TRUE;
        }

        case B_UNREGISTER_TOOL_PERMISSION: {
            if (!iface.permission || !iface.permission->unregister_tool_permission) {
                return JS_FALSE;
            }
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "unregisterToolPermission: toolName required");
            }
            std::string toolName = jsToCppString(ctx, argv[0]);
            auto        nameSv
                = agentxx::plugin::PluginStringView::from(toolName.data(), toolName.size());
            return iface.permission->unregister_tool_permission(host, pctx->scope(), &nameSv) == 0
                       ? JS_TRUE
                       : JS_FALSE;
        }

        // ---- 网络 (`agentxx.net`): agent 与 client 两侧同一份实现 ----
        case B_NET_CALL:
        case B_NET_CALL_ASYNC:
        case B_NET_CANCEL:
        case B_NET_FETCH:
        case B_NET_DOWNLOAD:
        case B_NET_WS_CONNECT:
        case B_NET_WS_SEND:
        case B_NET_WS_CLOSE:
        case B_NET_HTTP_SERVE:
        case B_NET_HTTP_RESPOND:
        case B_NET_INFO:
            return netBridgeCall(ctx, pctx, magic, argc, argv, iface.net);

        // ---- 后台任务 (`agentxx.agent.runtime` v2): 只能碰本实例提交的 ----
        case B_SUBMIT_ASYNC_WORK: {
            if (!iface.runtime || !iface.runtime->submit_async_work) {
                return JS_NewString(ctx, ""); // 主程序不支持: 空 id (与 C++ SDK 同义)
            }
            if (argc < 1 || !JS_IsObject(argv[0])) {
                return JS_ThrowTypeError(ctx, "submitAsyncWork: options object required");
            }
            JSValueConst opt = argv[0];
            // kind: 字符串 `agent`/`tool` 或数字; 给了 toolName 即 `tool`
            int32_t kind = static_cast<int32_t>(propInt64(ctx, opt, "kind", 0));
            {
                JSValue kv = JS_GetPropertyStr(ctx, opt, "kind");
                if (JS_IsString(kv)) {
                    kind = jsToCppString(ctx, kv) == "tool" ? 1 : 0;
                }
                JS_FreeValue(ctx, kv);
            }
            const std::string toolName = propString(ctx, opt, "toolName");
            if (!toolName.empty()) {
                kind = 1;
            }
            // interruptPolicy: `bubble`/`deny`/`allow` 或数字; 缺省跟随创建者
            int32_t interruptPolicy = -1;
            {
                JSValue pv = JS_GetPropertyStr(ctx, opt, "interruptPolicy");
                if (JS_IsString(pv)) {
                    const std::string text = jsToCppString(ctx, pv);
                    interruptPolicy = text == "bubble" ? 0 : (text == "deny" ? 1 : (text == "allow" ? 2 : -1));
                } else if (JS_IsNumber(pv)) {
                    JS_ToInt32(ctx, &interruptPolicy, pv);
                }
                JS_FreeValue(ctx, pv);
            }
            const std::string name           = propString(ctx, opt, "name");
            const std::string systemPrompt   = propString(ctx, opt, "systemPrompt");
            const std::string message        = propString(ctx, opt, "message");
            const std::string model          = propString(ctx, opt, "model");
            const std::string sessionId      = propString(ctx, opt, "sessionId");
            const std::string queueKey       = propString(ctx, opt, "queueKey");
            const std::string workDir        = propString(ctx, opt, "workDir");
            const std::string language       = propString(ctx, opt, "language");
            const std::string notifyDelivery = propString(ctx, opt, "notifyDelivery");
            const std::string toolsJson      = propJson(ctx, opt, "tools");
            const std::string pluginsJson    = propJson(ctx, opt, "plugins");
            const std::string mcpJson        = propJson(ctx, opt, "mcpServers");
            const std::string skillsJson     = propJson(ctx, opt, "skills");
            const std::string memoryJson     = propJson(ctx, opt, "memoryFiles");
            const std::string ragJson        = propJson(ctx, opt, "ragDocs");
            const std::string allowJson      = propJson(ctx, opt, "permissionAllow");
            const std::string denyJson       = propJson(ctx, opt, "permissionDeny");
            const std::string toolArgsJson   = propJson(ctx, opt, "toolArgs");

            // 任务事件回调: 存本实例的回调表, 主程序事件投递回 JS 线程再调
            int64_t          callbackToken = 0;
            JsCallbackHook* hook          = nullptr;
            JSValue          eventFn       = JS_GetPropertyStr(ctx, opt, "onEvent");
            if (JS_IsFunction(ctx, eventFn)) {
                callbackToken = ++pctx->nextCallbackToken;
                pctx->asyncWorkCallbacks[callbackToken] = JS_DupValue(ctx, eventFn);
                hook = engine->registerCallbackHook(pctx->instanceKey, callbackToken);
            }
            JS_FreeValue(ctx, eventFn);

            auto sv = [](const std::string& s) {
                return agentxx::plugin::PluginStringView::from(s.data(), s.size());
            };
            AgentxxPluginAsyncWorkSpec spec{};
            spec.struct_size          = sizeof(AgentxxPluginAsyncWorkSpec);
            spec.kind                 = kind;
            spec.interrupt_policy     = interruptPolicy;
            spec.session_store        = static_cast<int32_t>(propInt64(ctx, opt, "sessionStore", -1));
            spec.interrupt_timeout_ms = propInt64(ctx, opt, "interruptTimeoutMs", 0);
            spec.run_timeout_ms       = propInt64(ctx, opt, "runTimeoutMs", 0);
            spec.delay_ms             = propInt64(ctx, opt, "delayMs", 0);
            spec.queue_timeout_ms     = propInt64(ctx, opt, "queueTimeoutMs", 0);
            spec.session_id           = sv(sessionId);
            spec.queue_key            = sv(queueKey);
            spec.name                 = sv(name);
            spec.system_prompt        = sv(systemPrompt);
            spec.message              = sv(message);
            spec.model                = sv(model);
            spec.tools_json           = sv(toolsJson);
            spec.plugins_json         = sv(pluginsJson);
            spec.mcp_servers_json     = sv(mcpJson);
            spec.skills_json          = sv(skillsJson);
            spec.memory_files_json    = sv(memoryJson);
            spec.rag_docs_json        = sv(ragJson);
            spec.work_dir             = sv(workDir);
            spec.language             = sv(language);
            spec.permission_allow_json = sv(allowJson);
            spec.permission_deny_json  = sv(denyJson);
            spec.notify_delivery       = sv(notifyDelivery);
            spec.tool_name             = sv(toolName);
            spec.tool_args_json        = sv(toolArgsJson);
            spec.on_event              = hook ? &JsEngine::onAsyncWorkEvent : nullptr;
            spec.user_data             = hook;

            PluginxxString out{nullptr, 0};
            const int32_t  rc = iface.runtime->submit_async_work(host, &spec, &out);
            std::string    asyncId;
            if (out.data) {
                asyncId.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            if (rc != AGENTXX_RESULT_OK) {
                // 提交被拒: 回调不会再被调用, 当场释放登记与 JS 回调
                if (hook) {
                    engine->releaseCallbackHook(hook);
                    auto it = pctx->asyncWorkCallbacks.find(callbackToken);
                    if (it != pctx->asyncWorkCallbacks.end()) {
                        JS_FreeValue(ctx, it->second);
                        pctx->asyncWorkCallbacks.erase(it);
                    }
                }
                return JS_NewString(ctx, "");
            }
            return JS_NewStringLen(ctx, asyncId.data(), asyncId.size());
        }

        case B_CANCEL_ASYNC_WORK: {
            if (!iface.runtime || !iface.runtime->cancel_async_work || argc < 1) {
                return JS_FALSE;
            }
            const std::string id   = jsToCppString(ctx, argv[0]);
            auto              idSv = agentxx::plugin::PluginStringView::from(id.data(), id.size());
            return iface.runtime->cancel_async_work(host, &idSv) == AGENTXX_RESULT_OK ? JS_TRUE
                                                                                    : JS_FALSE;
        }

        case B_GET_ASYNC_WORK: {
            if (!iface.runtime || !iface.runtime->get_async_work || argc < 1) {
                return JS_NULL;
            }
            const std::string id   = jsToCppString(ctx, argv[0]);
            auto              idSv = agentxx::plugin::PluginStringView::from(id.data(), id.size());
            PluginxxString    out{nullptr, 0};
            const int32_t     rc = iface.runtime->get_async_work(host, &idSv, &out);
            std::string       json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            // 主程序对不存在的任务返回 `{}`; 这里给 null 更直观
            if (rc != AGENTXX_RESULT_OK || json.empty() || json == "{}") {
                return JS_NULL;
            }
            return jsonTextToValue(ctx, json, "<async_work>");
        }

        case B_LIST_ASYNC_WORK: {
            if (!iface.runtime || !iface.runtime->list_async_work) {
                return JS_NewArray(ctx);
            }
            const std::string sessionId
                = argc >= 1 && JS_IsString(argv[0]) ? jsToCppString(ctx, argv[0]) : std::string{};
            auto           sidSv = agentxx::plugin::PluginStringView::from(
                sessionId.data(),
                sessionId.size()
            );
            PluginxxString out{nullptr, 0};
            const int32_t  rc = iface.runtime->list_async_work(host, &sidSv, &out);
            std::string    json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            if (rc != AGENTXX_RESULT_OK) {
                return JS_NewArray(ctx);
            }
            JSValue arr = jsonTextToValue(ctx, json, "<async_works>");
            if (!JS_IsArray(arr)) {
                JS_FreeValue(ctx, arr);
                return JS_NewArray(ctx);
            }
            return arr;
        }

        // ---- 提示词 / 会话 / 上下文 / 执行图 / 挂载查询 ----
        case B_GET_PROMPT: {
            if (!iface.prompt || !iface.prompt->get_prompt) {
                return JS_NULL;
            }
            PluginxxString out{nullptr, 0};
            const int32_t  rc = iface.prompt->get_prompt(host, pctx->scope(), &out);
            std::string    json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            if (rc != AGENTXX_RESULT_OK) {
                return JS_NULL;
            }
            return jsonTextToValue(ctx, json, "<prompt>");
        }

        case B_SET_PROMPT: {
            if (!iface.prompt || !iface.prompt->set_prompt || argc < 1) {
                return JS_FALSE;
            }
            const std::string json   = argsJsonText(ctx, argv[0]);
            auto              jsonSv = agentxx::plugin::PluginStringView::from(
                json.data(),
                json.size()
            );
            return iface.prompt->set_prompt(host, pctx->scope(), &jsonSv) == AGENTXX_RESULT_OK
                       ? JS_TRUE
                       : JS_FALSE;
        }

        case B_GET_TOOL_PROMPT: {
            if (!iface.prompt || !iface.prompt->get_tool_prompt || argc < 1) {
                return JS_NULL;
            }
            const std::string tool = jsToCppString(ctx, argv[0]);
            auto toolSv = agentxx::plugin::PluginStringView::from(tool.data(), tool.size());
            PluginxxString out{nullptr, 0};
            const int32_t  rc = iface.prompt->get_tool_prompt(host, &toolSv, &out);
            std::string    json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            if (rc != AGENTXX_RESULT_OK || json.empty()) {
                return JS_NULL;
            }
            return jsonTextToValue(ctx, json, "<tool_prompt>");
        }

        case B_GET_SESSION_WORK_DIR: {
            if (!iface.session || !iface.session->get_session_work_dir) {
                return JS_NewString(ctx, "");
            }
            const std::string sessionId
                = argc >= 1 && JS_IsString(argv[0]) ? jsToCppString(ctx, argv[0]) : std::string{};
            auto sidSv = agentxx::plugin::PluginStringView::from(
                sessionId.data(),
                sessionId.size()
            );
            PluginxxString out{nullptr, 0};
            const int32_t  rc
                = iface.session->get_session_work_dir(host, pctx->scope(), &sidSv, &out);
            if (rc != AGENTXX_RESULT_OK || !out.data) {
                return JS_NewString(ctx, "");
            }
            JSValue dir = JS_NewStringLen(ctx, out.data, static_cast<size_t>(out.size));
            agentxx::plugin::PluginString::free(host, &out);
            return dir;
        }

        case B_IS_CANCELED: {
            if (!iface.session || !iface.session->is_canceled) {
                return JS_FALSE;
            }
            const std::string threadId
                = argc >= 1 && JS_IsString(argv[0]) ? jsToCppString(ctx, argv[0]) : std::string{};
            auto tidSv = agentxx::plugin::PluginStringView::from(
                threadId.data(),
                threadId.size()
            );
            return iface.session->is_canceled(host, pctx->scope(), &tidSv) != 0 ? JS_TRUE
                                                                               : JS_FALSE;
        }

        case B_GET_MESSAGES: {
            if (!iface.context || !iface.context->get_messages || argc < 1) {
                return JS_NewArray(ctx);
            }
            const std::string sessionId = jsToCppString(ctx, argv[0]);
            auto sidSv = agentxx::plugin::PluginStringView::from(
                sessionId.data(),
                sessionId.size()
            );
            PluginxxString out{nullptr, 0};
            const int32_t  rc
                = iface.context->get_messages(host, pctx->scope(), &sidSv, &out);
            std::string json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            if (rc != 0) {
                return JS_NewArray(ctx);
            }
            JSValue arr = jsonTextToValue(ctx, json, "<messages>");
            if (!JS_IsArray(arr)) {
                JS_FreeValue(ctx, arr);
                return JS_NewArray(ctx);
            }
            return arr;
        }

        case B_MESSAGES_COUNT: {
            if (!iface.context || !iface.context->messages_count || argc < 1) {
                return JS_NewInt32(ctx, -1);
            }
            const std::string sessionId = jsToCppString(ctx, argv[0]);
            auto sidSv = agentxx::plugin::PluginStringView::from(
                sessionId.data(),
                sessionId.size()
            );
            return JS_NewInt64(
                ctx,
                iface.context->messages_count(host, pctx->scope(), &sidSv)
            );
        }

        case B_GET_GRAPH_JSON: {
            if (!iface.graph || !iface.graph->get_graph_json) {
                return JS_NULL;
            }
            PluginxxString out{nullptr, 0};
            const int32_t  rc = iface.graph->get_graph_json(host, pctx->scope(), &out);
            std::string    json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            if (rc != AGENTXX_RESULT_OK) {
                return JS_NULL;
            }
            return jsonTextToValue(ctx, json, "<graph>");
        }

        case B_GET_GRAPH_NAME: {
            if (!iface.graph || !iface.graph->get_graph_name) {
                return JS_NewString(ctx, "");
            }
            PluginxxString out{nullptr, 0};
            const int32_t  rc = iface.graph->get_graph_name(host, pctx->scope(), &out);
            if (rc != AGENTXX_RESULT_OK || !out.data) {
                return JS_NewString(ctx, "");
            }
            JSValue name = JS_NewStringLen(ctx, out.data, static_cast<size_t>(out.size));
            agentxx::plugin::PluginString::free(host, &out);
            return name;
        }

        case B_SET_GRAPH_JSON: {
            if (!iface.graph || !iface.graph->set_graph_json || argc < 1) {
                return JS_FALSE;
            }
            const std::string json   = argsJsonText(ctx, argv[0]);
            auto              jsonSv = agentxx::plugin::PluginStringView::from(
                json.data(),
                json.size()
            );
            return iface.graph->set_graph_json(host, pctx->scope(), &jsonSv) == AGENTXX_RESULT_OK
                       ? JS_TRUE
                       : JS_FALSE;
        }

        case B_LIST_MOUNTS: {
            if (!iface.mount || !iface.mount->list) {
                return JS_NewArray(ctx);
            }
            PluginxxString out{nullptr, 0};
            if (iface.mount->list(host, &out) != AGENTXX_RESULT_OK || !out.data) {
                return JS_NewArray(ctx);
            }
            std::string json(out.data, static_cast<size_t>(out.size));
            agentxx::plugin::PluginString::free(host, &out);
            JSValue arr = jsonTextToValue(ctx, json, "<mounts>");
            if (!JS_IsArray(arr)) {
                JS_FreeValue(ctx, arr);
                return JS_NewArray(ctx);
            }
            return arr;
        }

        case B_GET_MOUNT: {
            if (!iface.mount || !iface.mount->get) {
                return JS_NULL;
            }
            // 缺省查询本实例所属的 agent
            const std::string agentId
                = argc >= 1 && JS_IsString(argv[0]) ? jsToCppString(ctx, argv[0])
                                                    : pctx->scopeAgentId;
            if (agentId.empty()) {
                return JS_NULL;
            }
            auto agentSv = agentxx::plugin::PluginStringView::from(
                agentId.data(),
                agentId.size()
            );
            PluginxxString out{nullptr, 0};
            if (iface.mount->get(host, &agentSv, &out) != AGENTXX_RESULT_OK || !out.data) {
                return JS_NULL;
            }
            std::string json(out.data, static_cast<size_t>(out.size));
            agentxx::plugin::PluginString::free(host, &out);
            return jsonTextToValue(ctx, json, "<mount>");
        }

        // ---- 会话运行时 (`agentxx.agent.runtime`): 自建根 / 会话通道 / 触发器 / 定时器 ----
        // 容器级接口: 不按 agent 挂载寻址, 句柄由主程序分配。
        // 注意: `mountRoot` 只能在容器级时机调用 (在 mount 回调里调会被拒),
        // 而脚本实例是每个 agent 各一份 —— 多挂载时同一段顶层代码会调多次。
        case B_MOUNT_ROOT: {
            if (!iface.runtime || !iface.runtime->mount_root) {
                return JS_NewString(ctx, "");
            }
            if (argc < 1 || !JS_IsObject(argv[0])) {
                return JS_ThrowTypeError(ctx, "mountRoot: options object required");
            }
            JSValueConst  opt  = argv[0];
            const std::string name       = propString(ctx, opt, "name");
            const std::string configJson = propJson(ctx, opt, "config");
            const std::string mountJson  = propJson(ctx, opt, "mountPlugins");
            auto              sv         = [](const std::string& s) {
                return agentxx::plugin::PluginStringView::from(s.data(), s.size());
            };
            AgentxxPluginMountRootSpec spec{};
            spec.struct_size     = sizeof(AgentxxPluginMountRootSpec);
            spec.name            = sv(name);
            spec.config_json     = sv(configJson);
            spec.mount_plugins   = sv(mountJson);
            spec.include_creator = propBool(ctx, opt, "includeCreator", false) ? 1 : 0;
            PluginxxString out{nullptr, 0};
            const int32_t  rc = iface.runtime->mount_root(host, &spec, &out);
            std::string    rootId;
            if (out.data) {
                rootId.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            if (rc != AGENTXX_RESULT_OK) {
                return JS_NewString(ctx, "");
            }
            return JS_NewStringLen(ctx, rootId.data(), rootId.size());
        }

        case B_UNMOUNT_ROOT: {
            if (!iface.runtime || !iface.runtime->unmount_root || argc < 1) {
                return JS_FALSE;
            }
            const std::string rootId = jsToCppString(ctx, argv[0]);
            auto              idSv   = agentxx::plugin::PluginStringView::from(
                rootId.data(),
                rootId.size()
            );
            return iface.runtime->unmount_root(host, &idSv) == AGENTXX_RESULT_OK ? JS_TRUE
                                                                                : JS_FALSE;
        }

        case B_OPEN_CHANNEL: {
            if (!iface.runtime || !iface.runtime->open_channel) {
                return JS_NewInt64(ctx, 0);
            }
            if (argc < 1 || !JS_IsObject(argv[0])) {
                return JS_ThrowTypeError(ctx, "openChannel: options object required");
            }
            JSValueConst      opt = argv[0];
            const std::string rootId      = propString(ctx, opt, "rootId");
            const std::string sessionId   = propString(ctx, opt, "sessionId");
            const std::string title       = propString(ctx, opt, "title");
            const std::string model       = propString(ctx, opt, "model");
            auto              sv          = [](const std::string& s) {
                return agentxx::plugin::PluginStringView::from(s.data(), s.size());
            };
            // 事件回调: 没有就给一个空实现 (主程序要求非空)
            int64_t          callbackToken = 0;
            JsCallbackHook*  hook          = nullptr;
            JSValue          eventFn       = JS_GetPropertyStr(ctx, opt, "onEvent");
            if (JS_IsFunction(ctx, eventFn)) {
                callbackToken = ++pctx->nextCallbackToken;
                pctx->channelCallbacks[callbackToken] = JS_DupValue(ctx, eventFn);
                hook = engine->registerCallbackHook(pctx->instanceKey, callbackToken);
            }
            JS_FreeValue(ctx, eventFn);
            AgentxxPluginChannelSpec spec{};
            spec.struct_size          = sizeof(AgentxxPluginChannelSpec);
            spec.root_id              = sv(rootId);
            spec.session_id           = sv(sessionId);
            spec.title                = sv(title);
            spec.model                = sv(model);
            spec.max_queue            = static_cast<int32_t>(propInt64(ctx, opt, "maxQueue", 0));
            spec.interrupt_timeout_ms = propInt64(ctx, opt, "interruptTimeoutMs", 0);
            spec.on_event             = &JsEngine::onChannelEvent;
            spec.user_data            = hook;
            int64_t       channelId = 0;
            const int32_t rc        = iface.runtime->open_channel(host, &spec, &channelId);
            if (rc != AGENTXX_RESULT_OK || channelId <= 0) {
                if (hook) {
                    engine->releaseCallbackHook(hook);
                    auto it = pctx->channelCallbacks.find(callbackToken);
                    if (it != pctx->channelCallbacks.end()) {
                        JS_FreeValue(ctx, it->second);
                        pctx->channelCallbacks.erase(it);
                    }
                }
                return JS_NewInt64(ctx, 0);
            }
            return JS_NewInt64(ctx, channelId);
        }

        case B_CLOSE_CHANNEL:
        case B_CANCEL_CHANNEL: {
            if (!iface.runtime || argc < 1) {
                return JS_FALSE;
            }
            int64_t channelId = 0;
            JS_ToInt64(ctx, &channelId, argv[0]);
            if (channelId <= 0) {
                return JS_FALSE;
            }
            const bool close = magic == B_CLOSE_CHANNEL;
            if (close && !iface.runtime->close_channel) {
                return JS_FALSE;
            }
            if (!close && !iface.runtime->cancel) {
                return JS_FALSE;
            }
            const int32_t rc = close ? iface.runtime->close_channel(host, channelId)
                                     : iface.runtime->cancel(host, channelId);
            return rc == AGENTXX_RESULT_OK ? JS_TRUE : JS_FALSE;
        }

        case B_SEND_INPUT: {
            if (!iface.runtime || !iface.runtime->send_input || argc < 2) {
                return JS_FALSE;
            }
            int64_t channelId = 0;
            JS_ToInt64(ctx, &channelId, argv[0]);
            // 第二个参数: 字符串 = 正文; 对象 = 完整输入 JSON ({text,attachments,...})
            const std::string input = argsJsonText(ctx, argv[1]);
            auto              inSv  = agentxx::plugin::PluginStringView::from(
                input.data(),
                input.size()
            );
            return iface.runtime->send_input(host, channelId, &inSv) == AGENTXX_RESULT_OK
                       ? JS_TRUE
                       : JS_FALSE;
        }

        case B_SELECT_MODEL: {
            if (!iface.runtime || !iface.runtime->select_model || argc < 2) {
                return JS_FALSE;
            }
            int64_t channelId = 0;
            JS_ToInt64(ctx, &channelId, argv[0]);
            const std::string modelName = jsToCppString(ctx, argv[1]);
            auto              modelSv   = agentxx::plugin::PluginStringView::from(
                modelName.data(),
                modelName.size()
            );
            return iface.runtime->select_model(host, channelId, &modelSv) == AGENTXX_RESULT_OK
                       ? JS_TRUE
                       : JS_FALSE;
        }

        case B_RESPOND_INTERRUPT: {
            if (!iface.runtime || !iface.runtime->respond_interrupt || argc < 3) {
                return JS_FALSE;
            }
            int64_t channelId = 0;
            JS_ToInt64(ctx, &channelId, argv[0]);
            int64_t interruptId = 0;
            JS_ToInt64(ctx, &interruptId, argv[1]);
            // 第三个参数按 {values:{...}} 传 (对象或 JSON 文本)
            const std::string values = argsJsonText(ctx, argv[2]);
            auto              valSv  = agentxx::plugin::PluginStringView::from(
                values.data(),
                values.size()
            );
            return iface.runtime->respond_interrupt(host, channelId, interruptId, &valSv)
                           == AGENTXX_RESULT_OK
                       ? JS_TRUE
                       : JS_FALSE;
        }

        case B_GET_CHANNEL_STATE: {
            if (!iface.runtime || !iface.runtime->get_channel_state || argc < 1) {
                return JS_NULL;
            }
            int64_t channelId = 0;
            JS_ToInt64(ctx, &channelId, argv[0]);
            PluginxxString out{nullptr, 0};
            const int32_t  rc = iface.runtime->get_channel_state(host, channelId, &out);
            std::string    json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            if (rc != AGENTXX_RESULT_OK) {
                return JS_NULL;
            }
            return jsonTextToValue(ctx, json, "<channel_state>");
        }

        case B_GET_CHANNEL_MESSAGES: {
            if (!iface.runtime || !iface.runtime->get_context_messages || argc < 1) {
                return JS_NewArray(ctx);
            }
            int64_t channelId = 0;
            JS_ToInt64(ctx, &channelId, argv[0]);
            PluginxxString out{nullptr, 0};
            const int32_t  rc = iface.runtime->get_context_messages(host, channelId, &out);
            std::string    json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            if (rc != AGENTXX_RESULT_OK) {
                return JS_NewArray(ctx);
            }
            JSValue arr = jsonTextToValue(ctx, json, "<channel_messages>");
            if (!JS_IsArray(arr)) {
                JS_FreeValue(ctx, arr);
                return JS_NewArray(ctx);
            }
            return arr;
        }

        case B_LIST_RUNTIME: {
            if (!iface.runtime || !iface.runtime->list_runtime) {
                return JS_NewArray(ctx);
            }
            PluginxxString out{nullptr, 0};
            if (iface.runtime->list_runtime(host, &out) != AGENTXX_RESULT_OK || !out.data) {
                return JS_NewArray(ctx);
            }
            std::string json(out.data, static_cast<size_t>(out.size));
            agentxx::plugin::PluginString::free(host, &out);
            return jsonTextToValue(ctx, json, "<runtime>");
        }

        case B_REGISTER_TRIGGER: {
            if (!iface.runtime || !iface.runtime->register_trigger) {
                return JS_NewInt64(ctx, 0);
            }
            if (argc < 1 || !JS_IsObject(argv[0])) {
                return JS_ThrowTypeError(ctx, "registerTrigger: options object required");
            }
            JSValueConst      opt = argv[0];
            const std::string topic    = propString(ctx, opt, "topic");
            const std::string delivery = propString(ctx, opt, "delivery");
            if (topic.empty()) {
                return JS_ThrowTypeError(ctx, "registerTrigger: topic required");
            }
            auto sv = [](const std::string& s) {
                return agentxx::plugin::PluginStringView::from(s.data(), s.size());
            };
            AgentxxPluginTriggerSpec spec{};
            spec.struct_size = sizeof(AgentxxPluginTriggerSpec);
            spec.policy      = static_cast<int32_t>(propInt64(ctx, opt, "policy", 0));
            spec.debounce_ms = static_cast<int32_t>(propInt64(ctx, opt, "debounceMs", 0));
            spec.max_queue   = static_cast<int32_t>(propInt64(ctx, opt, "maxQueue", 0));
            spec.topic       = sv(topic);
            spec.channel_id  = propInt64(ctx, opt, "channelId", 0);
            spec.delivery    = sv(delivery);
            int64_t       triggerId = 0;
            const int32_t rc        = iface.runtime->register_trigger(host, &spec, &triggerId);
            if (rc != AGENTXX_RESULT_OK) {
                return JS_NewInt64(ctx, 0);
            }
            return JS_NewInt64(ctx, triggerId);
        }

        case B_UNREGISTER_TRIGGER:
        case B_UNREGISTER_TIMER: {
            if (!iface.runtime || argc < 1) {
                return JS_FALSE;
            }
            int64_t id = 0;
            JS_ToInt64(ctx, &id, argv[0]);
            if (id <= 0) {
                return JS_FALSE;
            }
            const bool trigger = magic == B_UNREGISTER_TRIGGER;
            if (trigger ? (iface.runtime->unregister_trigger == nullptr)
                        : (iface.runtime->unregister_timer == nullptr)) {
                return JS_FALSE;
            }
            const int32_t rc = trigger ? iface.runtime->unregister_trigger(host, id)
                                       : iface.runtime->unregister_timer(host, id);
            return rc == AGENTXX_RESULT_OK ? JS_TRUE : JS_FALSE;
        }

        case B_REGISTER_TIMER: {
            if (!iface.runtime || !iface.runtime->register_timer) {
                return JS_NewInt64(ctx, 0);
            }
            if (argc < 1 || !JS_IsObject(argv[0])) {
                return JS_ThrowTypeError(ctx, "registerTimer: options object required");
            }
            JSValueConst      opt    = argv[0];
            const std::string input  = propJson(ctx, opt, "input");
            auto              inSv   = agentxx::plugin::PluginStringView::from(
                input.data(),
                input.size()
            );
            AgentxxPluginTimerSpec spec{};
            spec.struct_size    = sizeof(AgentxxPluginTimerSpec);
            spec.policy         = static_cast<int32_t>(propInt64(ctx, opt, "policy", 2));
            spec.channel_id     = propInt64(ctx, opt, "channelId", 0);
            spec.first_delay_ms = propInt64(ctx, opt, "firstDelayMs", 0);
            spec.interval_ms    = propInt64(ctx, opt, "intervalMs", 0);
            spec.input_json     = inSv;
            int64_t       timerId = 0;
            const int32_t rc      = iface.runtime->register_timer(host, &spec, &timerId);
            if (rc != AGENTXX_RESULT_OK) {
                return JS_NewInt64(ctx, 0);
            }
            return JS_NewInt64(ctx, timerId);
        }

        case B_EMIT_EVENT: {
            if (!iface.runtime || !iface.runtime->emit_event || argc < 1) {
                return JS_FALSE;
            }
            const std::string topic   = jsToCppString(ctx, argv[0]);
            const std::string payload = argc >= 2 ? argsJsonText(ctx, argv[1]) : std::string{"{}"};
            auto              topicSv = agentxx::plugin::PluginStringView::from(
                topic.data(),
                topic.size()
            );
            auto paySv = agentxx::plugin::PluginStringView::from(
                payload.data(),
                payload.size()
            );
            return iface.runtime->emit_event(host, &topicSv, &paySv) == AGENTXX_RESULT_OK
                       ? JS_TRUE
                       : JS_FALSE;
        }

        // ---- 广播声明 / 读当前值 / 结束流 ----
        // `publish` / `subscribe` 已有; 这里补上写入方需要的声明与收尾
        case B_DECLARE_BROADCAST: {
            if (!iface.broadcasts || !iface.broadcasts->declare_broadcast || argc < 1) {
                return JS_FALSE;
            }
            const std::string decl = argsJsonText(ctx, argv[0]);
            auto              declSv = agentxx::plugin::PluginStringView::from(
                decl.data(),
                decl.size()
            );
            PluginxxString err{nullptr, 0};
            const int32_t  rc = iface.broadcasts->declare_broadcast(host, &declSv, &err);
            if (rc != PLUGINXX_RESULT_OK) {
                std::string errText
                    = err.data ? std::string(err.data, static_cast<size_t>(err.size))
                               : std::string{"declare failed"};
                if (err.data) {
                    agentxx::plugin::PluginString::free(host, &err);
                }
                return throwJsError(ctx, fmt::format("declareBroadcast: {}", errText));
            }
            if (err.data) {
                agentxx::plugin::PluginString::free(host, &err);
            }
            return JS_TRUE;
        }

        case B_READ_BROADCAST: {
            if (!iface.broadcasts || !iface.broadcasts->read_broadcast || argc < 1) {
                return JS_NULL;
            }
            const std::string id   = jsToCppString(ctx, argv[0]);
            auto              idSv = agentxx::plugin::PluginStringView::from(id.data(), id.size());
            PluginxxString    out{nullptr, 0};
            const int32_t     rc = iface.broadcasts->read_broadcast(host, &idSv, &out);
            std::string       json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            // 没声明 / 没当前值 / 已结束 都返回 null (调用方不需要区分)
            if (rc != PLUGINXX_RESULT_OK) {
                return JS_NULL;
            }
            return jsonTextToValue(ctx, json, "<broadcast>");
        }

        case B_END_BROADCAST: {
            if (!iface.broadcasts || !iface.broadcasts->end_broadcast || argc < 1) {
                return JS_FALSE;
            }
            const std::string id    = jsToCppString(ctx, argv[0]);
            const std::string reason = argc >= 2 ? argsJsonText(ctx, argv[1]) : std::string{};
            auto              idSv  = agentxx::plugin::PluginStringView::from(id.data(), id.size());
            auto              reaSv = agentxx::plugin::PluginStringView::from(
                reason.data(),
                reason.size()
            );
            PluginxxString err{nullptr, 0};
            const int32_t  rc = iface.broadcasts->end_broadcast(host, &idSv, &reaSv, &err);
            if (err.data) {
                agentxx::plugin::PluginString::free(host, &err);
            }
            return rc == PLUGINXX_RESULT_OK ? JS_TRUE : JS_FALSE;
        }

        // ---- 会话 share_store 写入 / 本插件资源 / 模型配置 ----
        case B_ADD_SHARE_STORE: {
            if (!iface.session || !iface.session->add_share_store || argc < 2) {
                return JS_NewInt64(ctx, -1);
            }
            const std::string sessionId = jsToCppString(ctx, argv[0]);
            const std::string content   = JS_IsString(argv[1]) ? jsToCppString(ctx, argv[1])
                                                             : valueToJsonString(ctx, argv[1]);
            auto sidSv = agentxx::plugin::PluginStringView::from(sessionId.data(), sessionId.size());
            auto conSv = agentxx::plugin::PluginStringView::from(content.data(), content.size());
            return JS_NewInt64(
                ctx,
                iface.session->add_share_store(host, pctx->scope(), &sidSv, &conSv)
            );
        }

        case B_GET_OWN_RESOURCES: {
            if (!iface.resources || !iface.resources->get_own_resources) {
                return JS_NULL;
            }
            PluginxxString out{nullptr, 0};
            const int32_t  rc = iface.resources->get_own_resources(host, pctx->scope(), &out);
            std::string    json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            if (rc != AGENTXX_RESULT_OK || json.empty()) {
                return JS_NULL;
            }
            return jsonTextToValue(ctx, json, "<resources>");
        }

        case B_GET_MODEL_CONFIG: {
            if (!iface.model || !iface.model->get_config) {
                return JS_NULL;
            }
            PluginxxString out{nullptr, 0};
            const int32_t  rc = iface.model->get_config(host, pctx->scope(), &out);
            std::string    json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            if (rc != AGENTXX_RESULT_OK || json.empty()) {
                return JS_NULL;
            }
            return jsonTextToValue(ctx, json, "<model_config>");
        }

        // ---- 本插件信息 / 配置文件路径 / 路径权限三态查询 ----
        case B_GET_OWN_INFO: {
            if (!iface.plugins || !iface.plugins->get_own_info) {
                return JS_NULL;
            }
            PluginxxString out{nullptr, 0};
            const int32_t  rc = iface.plugins->get_own_info(host, &out);
            std::string    json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            if (rc != PLUGINXX_RESULT_OK || json.empty()) {
                return JS_NULL;
            }
            return jsonTextToValue(ctx, json, "<own_info>");
        }

        case B_GET_CONFIG_PATH: {
            // 未配置时主程序给空串 (与 C++ 侧语义一致)
            if (!iface.config || !iface.config->get_plugin_config_path) {
                return JS_NewString(ctx, "");
            }
            PluginxxString out{nullptr, 0};
            if (iface.config->get_plugin_config_path(host, &out) != PLUGINXX_RESULT_OK
                || !out.data) {
                return JS_NewString(ctx, "");
            }
            JSValue path = JS_NewStringLen(ctx, out.data, static_cast<size_t>(out.size));
            agentxx::plugin::PluginString::free(host, &out);
            return path;
        }

        case B_CHECK_PATHS:
        case B_FILTER_PATHS: {
            // checkPaths(paths, scope, sessionId?) -> ["allow"|"deny"|"ask"]
            // filterPaths(...) -> 允许的子集 (null = 主程序没有权限中间件, 不过滤)
            // 只做只读判定: 不发起询问、不弹界面 (与 kit 的 filterPathPermissions 同语义)
            if (!iface.permission || !iface.permission->check_paths) {
                return JS_NULL;
            }
            if (argc < 1 || !JS_IsArray(argv[0])) {
                return JS_ThrowTypeError(
                    ctx,
                    "checkPaths: (paths, scope?, sessionId?) required"
                );
            }
            const std::vector<std::string> paths = jsStringArray(ctx, argv[0]);
            if (paths.empty()) {
                return JS_NewArray(ctx);
            }
            const int32_t     scope = permissionScopeOf(ctx, argc >= 2 ? argv[1] : JS_UNDEFINED);
            const std::string sessionId
                = argc >= 3 && JS_IsString(argv[2]) ? jsToCppString(ctx, argv[2])
                                                    : std::string{};
            auto sidSv = agentxx::plugin::PluginStringView::from(
                sessionId.data(),
                sessionId.size()
            );
            // 分批查询: 主程序在 io 线程执行, 单次不宜过大 (同 kit 的 512 项)
            constexpr size_t      kBatch = 512;
            std::vector<int32_t>  decisions(
                paths.size(),
                AGENTXX_PLUGIN_PERMISSION_DECISION_ASK
            );
            for (size_t start = 0; start < paths.size(); start += kBatch) {
                const size_t                    count = std::min(kBatch, paths.size() - start);
                std::vector<PluginxxStringView> views;
                views.reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    const auto& p = paths[start + i];
                    views.push_back(agentxx::plugin::PluginStringView::from(p.data(), p.size()));
                }
                AgentxxPluginPermissionPathQuery query{};
                query.struct_size = sizeof(AgentxxPluginPermissionPathQuery);
                query.scope       = scope;
                query.path_count  = static_cast<int32_t>(count);
                query.session_id  = sidSv;
                query.paths       = views.data();
                std::vector<int32_t> batch(count, AGENTXX_PLUGIN_PERMISSION_DECISION_ASK);
                const int32_t        rc = iface.permission->check_paths(
                    host,
                    pctx->scope(),
                    &query,
                    batch.data()
                );
                if (rc != AGENTXX_RESULT_OK) {
                    return JS_NULL; ///< 查询不可用: 调用方按"不过滤"处理
                }
                for (size_t i = 0; i < count; ++i) {
                    decisions[start + i] = batch[i];
                }
            }
            JSValue out = JS_NewArray(ctx);
            uint32_t n  = 0;
            for (size_t i = 0; i < paths.size(); ++i) {
                const bool allowed = decisions[i] == AGENTXX_PLUGIN_PERMISSION_DECISION_ALLOW;
                if (magic == B_FILTER_PATHS) {
                    if (allowed) {
                        JS_SetPropertyUint32(
                            ctx,
                            out,
                            n++,
                            JS_NewStringLen(ctx, paths[i].data(), paths[i].size())
                        );
                    }
                    continue;
                }
                const char* name = allowed ? "allow"
                    : decisions[i] == AGENTXX_PLUGIN_PERMISSION_DECISION_DENY ? "deny"
                                                                           : "ask";
                JS_SetPropertyUint32(
                    ctx,
                    out,
                    static_cast<uint32_t>(i),
                    JS_NewString(ctx, name)
                );
            }
            return out;
        }

        // ---- 能力表 (`agentxx.capabilities`): 登记脚本实现行 / 注销 / 自省 / 调用 ----
        case B_PROVIDE_CAPABILITY: {
            if (argc < 1 || !JS_IsObject(argv[0])) {
                return JS_ThrowTypeError(ctx, "provideCapability: options object required");
            }
            JSValueConst  opt = argv[0];
            const std::string id = propString(ctx, opt, "id");
            JSValue       fnVal  = JS_GetPropertyStr(ctx, opt, "handler");
            if (id.empty() || !JS_IsFunction(ctx, fnVal)) {
                JS_FreeValue(ctx, fnVal);
                return JS_ThrowTypeError(
                    ctx,
                    "provideCapability: {id, handler} required"
                );
            }
            std::string err;
            const int32_t rc = engine->provideCapabilityRow(
                ctx,
                pctx,
                iface.capabilities,
                id,
                propString(ctx, opt, "ownerTag"),
                fnVal,
                false,
                capabilityImplsOf(ctx, opt),
                capabilityMergeOf(ctx, opt),
                capabilityPhasesOf(ctx, opt),
                static_cast<int32_t>(propInt64(ctx, opt, "priority", 0)),
                static_cast<int32_t>(propInt64(ctx, opt, "timeoutMs", 0)),
                &err
            );
            JS_FreeValue(ctx, fnVal);
            if (rc != 0) {
                return throwJsError(
                    ctx,
                    fmt::format(
                        "provideCapability: {} (rc={}{})",
                        id,
                        rc,
                        err.empty() ? "" : ", " + err
                    )
                );
            }
            return JS_TRUE;
        }

        case B_PROVIDE_POINT: {
            // providePoint(pointId, handler, {priority?, timeoutMs?, ownerTag?})
            // 登记形状与 kit 的 providePoint 同一套: many + first + 只开 impl 阶段
            if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
                return JS_ThrowTypeError(
                    ctx,
                    "providePoint: (pointId, handler, options?) required"
                );
            }
            const std::string pointId = jsToCppString(ctx, argv[0]);
            JSValueConst      opt     = argc >= 3 && JS_IsObject(argv[2]) ? argv[2] : JS_UNDEFINED;
            std::string       err;
            const int32_t     rc      = engine->provideCapabilityRow(
                ctx,
                pctx,
                iface.capabilities,
                pointId,
                JS_IsObject(opt) ? propString(ctx, opt, "ownerTag") : std::string{},
                argv[1],
                true,
                PLUGINXX_CAPABILITY_IMPLS_MANY,
                PLUGINXX_CAPABILITY_MERGE_FIRST,
                PLUGINXX_CAPABILITY_PHASE_MASK_IMPL,
                JS_IsObject(opt) ? static_cast<int32_t>(propInt64(ctx, opt, "priority", 0)) : 0,
                JS_IsObject(opt) ? static_cast<int32_t>(propInt64(ctx, opt, "timeoutMs", 0)) : 0,
                &err
            );
            if (rc != 0) {
                return throwJsError(
                    ctx,
                    fmt::format(
                        "providePoint: {} (rc={}{})",
                        pointId,
                        rc,
                        err.empty() ? "" : ", " + err
                    )
                );
            }
            return JS_TRUE;
        }

        case B_UNPROVIDE_CAPABILITY: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "unprovideCapability: (id, ownerTag?) required");
            }
            engine->unprovideCapabilityRow(
                ctx,
                pctx,
                iface.capabilities,
                jsToCppString(ctx, argv[0]),
                argc >= 2 && JS_IsString(argv[1]) ? jsToCppString(ctx, argv[1]) : std::string{}
            );
            return JS_TRUE;
        }

        case B_LIST_CAPABILITIES: {
            if (!iface.capabilities || !iface.capabilities->list_capabilities) {
                return JS_NewArray(ctx);
            }
            PluginxxString out{nullptr, 0};
            if (iface.capabilities->list_capabilities(host, &out) != PLUGINXX_RESULT_OK
                || !out.data) {
                return JS_NewArray(ctx);
            }
            std::string json(out.data, static_cast<size_t>(out.size));
            agentxx::plugin::PluginString::free(host, &out);
            return jsonTextToValue(ctx, json, "<capabilities>");
        }

        case B_CAPABILITY_INFO: {
            if (!iface.capabilities || !iface.capabilities->capability_info || argc < 1
                || !JS_IsString(argv[0])) {
                return JS_NULL;
            }
            const std::string id   = jsToCppString(ctx, argv[0]);
            auto              idSv = agentxx::plugin::PluginStringView::from(id.data(), id.size());
            PluginxxString    out{nullptr, 0};
            if (iface.capabilities->capability_info(host, &idSv, &out) != PLUGINXX_RESULT_OK
                || !out.data) {
                return JS_NULL; ///< 未声明 / 没有任何实现行
            }
            std::string json(out.data, static_cast<size_t>(out.size));
            agentxx::plugin::PluginString::free(host, &out);
            return jsonTextToValue(ctx, json, "<capability>");
        }

        case B_INVOKE_CAPABILITY: {
            // invokeCapability(id, args?, {phase?, timeoutMs?}) -> Promise
            // - 一律返回 Promise 并 resolve 成 `{ok, status, value|error}`: 不阻塞 JS 线程
            //   (同步等待会让"同引擎里 A 脚本调 B 脚本"自锁)
            if (!iface.capabilities || !iface.capabilities->invoke_capability) {
                return throwJsError(ctx, "invokeCapability: host lacks capabilities interface");
            }
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(
                    ctx,
                    "invokeCapability: (id, args?, options?) required"
                );
            }
            const std::string id       = jsToCppString(ctx, argv[0]);
            const std::string argsJson = argc >= 2 ? argsJsonText(ctx, argv[1]) : std::string{"{}"};
            int32_t           phase    = 0;
            int32_t           timeoutMs = -1; ///< -1 = 用声明里的超时
            if (argc >= 3 && JS_IsObject(argv[2])) {
                phase     = static_cast<int32_t>(propInt64(ctx, argv[2], "phase", 0));
                timeoutMs = static_cast<int32_t>(propInt64(ctx, argv[2], "timeoutMs", -1));
            }
            JSValue resolving[2];
            JSValue promise = JS_NewPromiseCapability(ctx, resolving);
            if (JS_IsException(promise)) {
                return promise;
            }
            auto bridge      = std::make_shared<JsCapInvokeBridge>();
            bridge->engine   = engine;
            bridge->ctx      = ctx;
            bridge->resolve  = resolving[0];
            bridge->reject   = resolving[1];
            JsCapInvokeBridge* rawBridge = bridge.get();
            {
                std::lock_guard<std::mutex> lk(engine->capMtx_);
                engine->capInvokes_[rawBridge] = bridge;
            }
            auto idSv   = agentxx::plugin::PluginStringView::from(id.data(), id.size());
            auto argsSv = agentxx::plugin::PluginStringView::from(argsJson.data(), argsJson.size());
            PluginxxString err{nullptr, 0};
            PluginxxOperatorHandle* handle = nullptr;
            if (iface.capabilities->invoke_capability_ex) {
                handle = iface.capabilities->invoke_capability_ex(
                    host,
                    pctx->scope(),
                    &idSv,
                    &argsSv,
                    phase,
                    timeoutMs,
                    &JsEngine::capInvokeDone,
                    rawBridge,
                    &err
                );
            } else {
                handle = iface.capabilities->invoke_capability(
                    host,
                    pctx->scope(),
                    &idSv,
                    &argsSv,
                    phase,
                    &JsEngine::capInvokeDone,
                    rawBridge,
                    &err
                );
            }
            if (handle == nullptr) {
                // 真正被拒 (能力未声明 / 没有实现行): 也走 resolve, 业务结论看 ok
                std::string message = "capability is not available";
                if (err.data) {
                    message.assign(err.data, static_cast<size_t>(err.size));
                    agentxx::plugin::PluginString::free(host, &err);
                }
                engine->settleCapInvoke(rawBridge, PLUGINXX_OPERATOR_FAILED, message);
            }
            return promise;
        }

        default:
            return JS_ThrowInternalError(ctx, "unknown bridge magic %d", magic);
    }
}

// =====================================================================
// 网络桥 (agent 与 client 两侧共用)
// =====================================================================

JSValue JsEngine::netBridgeCall(
    JSContext*              ctx,
    JsPluginCtx*            pctx,
    int                     magic,
    int                     argc,
    JSValueConst*           argv,
    const PluginxxNetIface* net
) {
    auto*               engine = pctx ? pctx->engine : nullptr;
    const PluginxxHost* host   = pctx ? pctx->host : nullptr;
    if (!engine || !host) {
        return JS_ThrowInternalError(ctx, "agentxx net bridge: plugin context invalid");
    }

    /// 同步网络调用: 阻塞 JS 线程直到网络侧给出结果 (只用于快速操作)
    /// - 网络服务自有线程, **不**经主程序 io 线程: 在命令/渲染这类同步回调里调
    ///   也不会与界面线程互等
    auto netSync = [&](const std::string& op, const std::string& argsJson) -> JSValue {
        if (!net || !net->call) {
            return netOutcomeToValue(ctx, PLUGINXX_RESULT_ERR_STATE, std::string{});
        }
        auto           opSv   = agentxx::plugin::PluginStringView::from(op.data(), op.size());
        auto           argsSv = agentxx::plugin::PluginStringView::from(argsJson.data(), argsJson.size());
        PluginxxString out{nullptr, 0};
        const int32_t  code = net->call(host, &opSv, &argsSv, &out);
        std::string    json;
        if (out.data) {
            json.assign(out.data, static_cast<size_t>(out.size));
            agentxx::plugin::PluginString::free(host, &out);
        }
        return netOutcomeToValue(ctx, code, json);
    };

    /// 异步网络调用 (Promise): 只受理, 结果经完成回调 settle
    /// - Promise 总是 resolve 成结果对象; 受理失败也是对象 (`ok:false`)
    auto netAsync = [&](const std::string& op, const std::string& argsJson) -> JSValue {
        JSValue resolving[2];
        JSValue promise = JS_NewPromiseCapability(ctx, resolving);
        if (JS_IsException(promise)) {
            return promise;
        }
        // 受理失败 (或主程序没提供网络能力): 立即 resolve 错误对象
        const auto resolveNow = [&](JSValue out) {
            JSValue argv[1] = {out};
            JSValue rc      = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, argv);
            JS_FreeValue(ctx, rc);
            JS_FreeValue(ctx, out);
            JS_FreeValue(ctx, resolving[0]);
            JS_FreeValue(ctx, resolving[1]);
        };
        if (!net || !net->call_async) {
            resolveNow(netOutcomeToValue(ctx, PLUGINXX_RESULT_ERR_STATE, std::string{}));
            return promise;
        }
        auto* bridge    = new JsNetBridge{engine, ctx};
        bridge->resolve = resolving[0];
        bridge->reject  = resolving[1];
        auto           opSv   = agentxx::plugin::PluginStringView::from(op.data(), op.size());
        auto           argsSv = agentxx::plugin::PluginStringView::from(argsJson.data(), argsJson.size());
        PluginxxOperatorNotify notify{&JsEngine::onNetCallDone, bridge};
        PluginxxString         acceptedOut{nullptr, 0};
        int64_t                requestId = 0;
        const int32_t          code
            = net->call_async(host, &opSv, &argsSv, &notify, &acceptedOut, &requestId);
        std::string accepted;
        if (acceptedOut.data) {
            accepted.assign(acceptedOut.data, static_cast<size_t>(acceptedOut.size));
            agentxx::plugin::PluginString::free(host, &acceptedOut);
        }
        if (code != PLUGINXX_RESULT_OK) {
            bool expected = false;
            if (bridge->settled.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel
                )) {
                bridge->engine->settleNetCall(bridge, PLUGINXX_OPERATOR_FAILED, std::move(accepted));
            }
            return promise;
        }
        bridge->requestId.store(requestId, std::memory_order_release);
        return promise;
    };

    /// 网络参数 JSON: 第一个参数可以是 url 字符串或选项对象 (字段原样保留),
    /// 再叠加一个字符串字段 (文件名 / 不需要时传空)
    auto netArgs = [&](JSValueConst urlOrOptions,
                       const char*  extraKey,
                       const std::string& extraValue) -> std::string {
        utilxx_base::Json args = utilxx_base::Json::object();
        std::string       url;
        if (JS_IsString(urlOrOptions)) {
            url = jsToCppString(ctx, urlOrOptions);
        } else if (JS_IsObject(urlOrOptions) && !JS_IsFunction(ctx, urlOrOptions)) {
            try {
                auto doc = utilxx_base::Json::parse(valueToJsonString(ctx, urlOrOptions));
                if (doc.is_object()) {
                    args = std::move(doc);
                    url  = args.value("url", std::string{});
                }
            } catch (...) {
            }
        }
        if (!url.empty()) {
            args["url"] = url;
        }
        if (extraKey != nullptr && !extraValue.empty()) {
            args[extraKey] = extraValue;
        }
        return args.dump();
    };

    // 同步入口用于快速操作 (`ws.connect` / `ws.send` / `http.serve` / `net.info` …);
    // 慢操作 (`http.fetch` / `http.download`) 走 Promise, 不占住 JS 线程。
    // 注意: 同一引擎里的脚本共享一份网络资源 (归属是该插件实例), 连接 id 要自己带前缀。
    switch (magic) {
        case B_NET_CALL: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "netCall: op string required");
            }
            return netSync(
                jsToCppString(ctx, argv[0]),
                argc >= 2 ? argsJsonText(ctx, argv[1]) : std::string{"{}"}
            );
        }

        case B_NET_CALL_ASYNC: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "netCallAsync: op string required");
            }
            return netAsync(
                jsToCppString(ctx, argv[0]),
                argc >= 2 ? argsJsonText(ctx, argv[1]) : std::string{"{}"}
            );
        }

        case B_NET_FETCH:
        case B_NET_DOWNLOAD: {
            // netFetch(urlOrOptions) / netDownload(urlOrOptions, fileName?)
            // - 选项对象的字段原样交给网络侧 (`method` / `headers` / `timeoutMs` …)
            const char* op = magic == B_NET_DOWNLOAD ? pluginxx::net_op::httpDownload
                                                     : pluginxx::net_op::httpFetch;
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "netFetch/netDownload: url required");
            }
            std::string fileName;
            if (argc >= 2 && JS_IsString(argv[1])) {
                fileName = jsToCppString(ctx, argv[1]);
            }
            return netAsync(op, netArgs(argv[0], "fileName", fileName));
        }

        case B_NET_WS_CONNECT: {
            // netWsConnect(urlOrOptions): ok 只表示已发起, 握手结果看 `ws.open` 事件
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "netWsConnect: url required");
            }
            return netSync(pluginxx::net_op::wsConnect, netArgs(argv[0], nullptr, std::string{}));
        }

        case B_NET_WS_SEND: {
            if (argc < 2) {
                return JS_ThrowTypeError(ctx, "netWsSend: (id, text) required");
            }
            utilxx_base::Json args = utilxx_base::Json::object();
            args["id"]             = jsToCppString(ctx, argv[0]);
            args["text"]           = jsToCppString(ctx, argv[1]);
            return netSync(pluginxx::net_op::wsSend, args.dump());
        }

        case B_NET_WS_CLOSE: {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "netWsClose: id required");
            }
            utilxx_base::Json args = utilxx_base::Json::object();
            args["id"]             = jsToCppString(ctx, argv[0]);
            int64_t closeCode      = 1000;
            if (argc >= 2 && JS_IsNumber(argv[1])) {
                JS_ToInt64(ctx, &closeCode, argv[1]);
            }
            args["code"] = closeCode;
            if (argc >= 3) {
                args["reason"] = jsToCppString(ctx, argv[2]);
            }
            return netSync(pluginxx::net_op::wsClose, args.dump());
        }

        case B_NET_HTTP_SERVE:
        case B_NET_HTTP_RESPOND: {
            const char* op = magic == B_NET_HTTP_SERVE ? pluginxx::net_op::httpServe
                                                      : pluginxx::net_op::httpRespond;
            return netSync(op, argc >= 1 ? argsJsonText(ctx, argv[0]) : std::string{"{}"});
        }

        case B_NET_INFO:
            return netSync(pluginxx::net_op::info, "{}");

        case B_NET_CANCEL: {
            int64_t requestId = 0;
            if (argc >= 1) {
                JS_ToInt64(ctx, &requestId, argv[0]);
            }
            if (!net || !net->cancel || requestId <= 0) {
                return netOutcomeToValue(ctx, PLUGINXX_RESULT_ERR_ARG, std::string{});
            }
            PluginxxString out{nullptr, 0};
            const int32_t  code = net->cancel(host, requestId, &out);
            std::string    json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            return netOutcomeToValue(ctx, code, json);
        }

        default:
            return JS_ThrowInternalError(ctx, "unknown net bridge magic %d", magic);
    }
}

// =====================================================================
// client 侧桥实现 (脚本作为 client 插件运行)
// =====================================================================

JSValue JsEngine::clientBridgeCall(
    JSContext*    ctx,
    int           argc,
    JSValueConst* argv,
    int           magic,
    JsPluginCtx*  pctx
) {
    auto*               engine = pctx->engine;
    const PluginxxHost* host   = pctx->host;
    if (!engine || !host || !host->vtable) {
        return JS_ThrowInternalError(ctx, "agentxx bridge: plugin context invalid");
    }
    // COM 风格接口表查询 (client 侧的通用表同样由主程序提供)
    const agentxx::plugin::ClientIfaces iface = agentxx::plugin::ClientIfaces::query(host);

    /// 标记"JS 线程正在调用主程序": client io 线程的同步渲染回调据此直接失败
    /// (两边互等: io 线程等 JS 线程, JS 线程等 io 线程处理它的表调用)
    struct HostCallGuard {
        std::atomic<int>& depth;

        explicit HostCallGuard(std::atomic<int>& d) :
            depth(d) {
            depth.fetch_add(1, std::memory_order_acq_rel);
        }

        ~HostCallGuard() {
            depth.fetch_sub(1, std::memory_order_acq_rel);
        }
    } hostCallGuard(engine->jsHostCallDepth_);

    /// 取对象里的文本字段 (非字符串时转成文本; 缺失/不可转给空串)
    auto strField = [&](JSValueConst obj, const char* key) -> std::string {
        if (!JS_IsObject(obj)) {
            return {};
        }
        JSValue     v = JS_GetPropertyStr(ctx, obj, key);
        std::string out;
        if (JS_IsString(v)) {
            out = jsToCppString(ctx, v);
        } else if (!JS_IsUndefined(v) && !JS_IsNull(v) && !JS_IsFunction(ctx, v)) {
            out = valueToJsonString(ctx, v);
        }
        JS_FreeValue(ctx, v);
        return out;
    };

    /// 取对象里的 JSON 字段: 字符串按原样 (已是 JSON 文本), 对象/数组序列化
    auto jsonField = [&](JSValueConst obj, const char* key) -> std::string {
        if (!JS_IsObject(obj)) {
            return {};
        }
        JSValue     v = JS_GetPropertyStr(ctx, obj, key);
        std::string out;
        if (JS_IsString(v)) {
            out = jsToCppString(ctx, v);
        } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            out = valueToJsonString(ctx, v);
        }
        JS_FreeValue(ctx, v);
        return out;
    };

    /// 主程序堆字符串 → JS 值 (JSON 优先, 失败退回原文; 释放主程序字符串)
    auto takeHostJson = [&](PluginxxString& s, const char* tag) -> JSValue {
        if (!s.data) {
            return JS_NULL;
        }
        std::string text(s.data, static_cast<size_t>(s.size));
        agentxx::plugin::PluginString::free(host, &s);
        return jsonTextToValue(ctx, text, tag);
    };

    /// 主程序堆字符串 → JS 字符串 (释放主程序字符串)
    auto takeHostText = [&](PluginxxString& s) -> JSValue {
        if (!s.data) {
            return JS_NewString(ctx, "");
        }
        JSValue out = JS_NewStringLen(ctx, s.data, static_cast<size_t>(s.size));
        agentxx::plugin::PluginString::free(host, &s);
        return out;
    };

    switch (magic) {
        case B_LOG: {
            int level = 2;
            if (argc >= 1 && JS_IsNumber(argv[0])) {
                int32_t lv = 0;
                JS_ToInt32(ctx, &lv, argv[0]);
                level = lv;
            }
            std::string msg   = argc >= 2 ? jsToCppString(ctx, argv[1]) : "";
            auto        msgSv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
            if (iface.log && iface.log->log) {
                iface.log->log(host, level, &msgSv);
            }
            return JS_UNDEFINED;
        }

        case B_CLIENT_GET_OWN_INFO: {
            if (!iface.self || !iface.self->get_own_info) {
                return JS_NULL;
            }
            PluginxxString out{nullptr, 0};
            if (iface.self->get_own_info(host, &out) != 0) {
                return JS_NULL;
            }
            return takeHostJson(out, "<own_info>");
        }

        case B_GET_PLUGIN_ARGS: { // 与 agent 侧同名: client 侧走 self 表
            if (!iface.self || !iface.self->get_plugin_args) {
                return JS_NULL;
            }
            PluginxxString out{nullptr, 0};
            if (iface.self->get_plugin_args(host, &out) != 0) {
                return JS_NULL;
            }
            return takeHostJson(out, "<plugin_args>");
        }

        case B_CLIENT_GET_CONFIG_PATH: {
            if (!iface.self || !iface.self->get_plugin_config_path) {
                return JS_NewString(ctx, "");
            }
            PluginxxString out{nullptr, 0};
            if (iface.self->get_plugin_config_path(host, &out) != 0) {
                return JS_NewString(ctx, "");
            }
            return takeHostText(out);
        }

        case B_GET_LANGUAGE: {
            if (!iface.self || !iface.self->get_language) {
                return JS_NewString(ctx, "en");
            }
            PluginxxString out{nullptr, 0};
            if (iface.self->get_language(host, &out) != 0) {
                return JS_NewString(ctx, "en");
            }
            return takeHostText(out);
        }

        case B_SET_LANGUAGE: {
            if (!iface.self || !iface.self->set_language || argc < 1 || !JS_IsString(argv[0])) {
                return JS_FALSE;
            }
            std::string lang   = jsToCppString(ctx, argv[0]);
            auto        langSv = agentxx::plugin::PluginStringView::from(lang.data(), lang.size());
            return iface.self->set_language(host, &langSv) == 0 ? JS_TRUE : JS_FALSE;
        }

        case B_CLIENT_GET_STATE: {
            if (!iface.session || !iface.session->get_client_state) {
                return JS_NULL;
            }
            PluginxxString out{nullptr, 0};
            if (iface.session->get_client_state(host, &out) != 0) {
                return JS_NULL;
            }
            return takeHostJson(out, "<client_state>");
        }

        case B_CLIENT_SEND_INPUT: {
            if (!iface.session || !iface.session->send_user_input || argc < 2) {
                return JS_ThrowTypeError(ctx, "sendUserInput: (sessionId, text) required");
            }
            std::string sid  = jsToCppString(ctx, argv[0]);
            std::string text = jsToCppString(ctx, argv[1]);
            auto        sidSv = agentxx::plugin::PluginStringView::from(sid.data(), sid.size());
            auto        textSv = agentxx::plugin::PluginStringView::from(text.data(), text.size());
            return iface.session->send_user_input(host, &sidSv, &textSv) == AGENTXX_RESULT_OK
                       ? JS_TRUE
                       : JS_FALSE;
        }

        case B_CLIENT_REQUEST_CANCEL: {
            if (!iface.session || !iface.session->request_cancel) {
                return JS_UNDEFINED;
            }
            std::string sid   = argc >= 1 ? jsToCppString(ctx, argv[0]) : std::string{};
            auto        sidSv = agentxx::plugin::PluginStringView::from(sid.data(), sid.size());
            iface.session->request_cancel(host, &sidSv);
            return JS_UNDEFINED;
        }

        case B_CLIENT_SEND_PLUGIN_DATA: {
            if (!iface.wire || !iface.wire->send_plugin_data || argc < 2) {
                return JS_FALSE;
            }
            std::string topic = jsToCppString(ctx, argv[0]);
            std::string payload
                = JS_IsString(argv[1]) ? jsToCppString(ctx, argv[1])
                                       : valueToJsonString(ctx, argv[1]);
            auto topicSv = agentxx::plugin::PluginStringView::from(topic.data(), topic.size());
            auto paySv = agentxx::plugin::PluginStringView::from(payload.data(), payload.size());
            return iface.wire->send_plugin_data(host, &topicSv, &paySv) == AGENTXX_RESULT_OK
                       ? JS_TRUE
                       : JS_FALSE;
        }

        case B_CLIENT_SHOW_TOAST: {
            if (!iface.ui || !iface.ui->show_toast || argc < 1) {
                return JS_UNDEFINED;
            }
            std::string text  = jsToCppString(ctx, argv[0]);
            int32_t     level = 0;
            if (argc >= 2 && JS_IsNumber(argv[1])) {
                JS_ToInt32(ctx, &level, argv[1]);
            }
            auto textSv = agentxx::plugin::PluginStringView::from(text.data(), text.size());
            iface.ui->show_toast(host, &textSv, level);
            return JS_UNDEFINED;
        }

        case B_CLIENT_UPDATE_TOOL_DECOR: {
            if (!iface.ui || !iface.ui->update_tool_decor || argc < 2) {
                return JS_FALSE;
            }
            std::string tcid  = jsToCppString(ctx, argv[0]);
            std::string decor
                = JS_IsString(argv[1]) ? jsToCppString(ctx, argv[1])
                                       : valueToJsonString(ctx, argv[1]);
            auto tcidSv  = agentxx::plugin::PluginStringView::from(tcid.data(), tcid.size());
            auto decorSv = agentxx::plugin::PluginStringView::from(decor.data(), decor.size());
            return iface.ui->update_tool_decor(host, &tcidSv, &decorSv) == AGENTXX_RESULT_OK
                       ? JS_TRUE
                       : JS_FALSE;
        }

        case B_CLIENT_OPEN_OVERLAY: {
            if (!iface.ui || !iface.ui->open_overlay) {
                return JS_FALSE;
            }
            if (argc < 1 || !JS_IsObject(argv[0])) {
                return JS_ThrowTypeError(ctx, "openOverlay: spec object required");
            }
            const JSValueConst specObj = argv[0];
            // 类型: 数字直接用; 字符串按名字 (`mermaid` / `text` / `diff` / `custom`)
            int32_t type = AGENTXX_OVERLAY_TEXT;
            {
                JSValue tv = JS_GetPropertyStr(ctx, specObj, "type");
                if (JS_IsNumber(tv)) {
                    JS_ToInt32(ctx, &type, tv);
                } else if (JS_IsString(tv)) {
                    const std::string name = jsToCppString(ctx, tv);
                    if (name == "mermaid") {
                        type = AGENTXX_OVERLAY_MERMAID;
                    } else if (name == "diff") {
                        type = AGENTXX_OVERLAY_DIFF;
                    } else if (name == "custom" || name == "items") {
                        type = AGENTXX_OVERLAY_CUSTOM;
                    } else {
                        type = AGENTXX_OVERLAY_TEXT;
                    }
                }
                JS_FreeValue(ctx, tv);
            }
            std::string title      = strField(specObj, "title");
            std::string payload    = strField(specObj, "payload");
            std::string extraJson  = jsonField(specObj, "extra");
            AgentxxOverlaySpec spec{};
            spec.version     = 1;
            spec.struct_size = sizeof(AgentxxOverlaySpec);
            spec.type        = type;
            spec.title       = agentxx::plugin::PluginStringView::from(title.data(), title.size());
            spec.payload = agentxx::plugin::PluginStringView::from(payload.data(), payload.size());
            spec.extra_json
                = agentxx::plugin::PluginStringView::from(extraJson.data(), extraJson.size());
            return iface.ui->open_overlay(host, &spec) == AGENTXX_RESULT_OK ? JS_TRUE : JS_FALSE;
        }

        case B_CLIENT_CLOSE_OVERLAY: {
            if (iface.ui && iface.ui->close_overlay) {
                iface.ui->close_overlay(host);
            }
            return JS_UNDEFINED;
        }

        case B_CLIENT_REGISTER_TOOL_RENDERER: {
            if (!iface.ui || !iface.ui->register_tool_renderer) {
                return JS_FALSE; ///< 主程序不支持工具行定制: 附加能力, 忽略
            }
            if (argc < 1 || !JS_IsObject(argv[0])) {
                return JS_ThrowTypeError(ctx, "registerToolRenderer: spec object required");
            }
            const JSValueConst specObj = argv[0];
            std::string        toolName = strField(specObj, "tool");
            if (toolName.empty()) {
                toolName = strField(specObj, "toolName");
            }
            if (toolName.empty()) {
                return JS_ThrowTypeError(ctx, "registerToolRenderer: tool name required");
            }
            const std::string displayName = strField(specObj, "displayName");
            JSValue           collapsed   = JS_GetPropertyStr(ctx, specObj, "collapsed");
            JSValue           expanded    = JS_GetPropertyStr(ctx, specObj, "expanded");
            const bool        hasCollapsed = JS_IsFunction(ctx, collapsed);
            const bool        hasExpanded  = JS_IsFunction(ctx, expanded);

            auto reg         = std::make_shared<JsRenderReg>();
            reg->engine      = engine;
            reg->instanceKey = pctx->instanceKey;
            reg->toolName    = toolName;
            if (hasCollapsed) {
                const int64_t token = ++pctx->nextCallbackToken;
                reg->collapsedToken.store(token, std::memory_order_release);
                pctx->clientCallbacks[token] = JS_DupValue(ctx, collapsed);
            }
            if (hasExpanded) {
                const int64_t token = ++pctx->nextCallbackToken;
                reg->expandedToken.store(token, std::memory_order_release);
                pctx->clientCallbacks[token] = JS_DupValue(ctx, expanded);
            }
            JS_FreeValue(ctx, collapsed);
            JS_FreeValue(ctx, expanded);

            AgentxxToolRenderSpec spec{};
            spec.struct_size  = sizeof(AgentxxToolRenderSpec);
            spec.version      = 1;
            spec.tool_name
                = agentxx::plugin::PluginStringView::from(toolName.data(), toolName.size());
            spec.display_name
                = agentxx::plugin::PluginStringView::from(displayName.data(), displayName.size());
            spec.collapsed_fn = hasCollapsed ? &JsEngine::renderCollapsed : nullptr;
            spec.expanded_fn  = hasExpanded ? &JsEngine::renderExpanded : nullptr;
            spec.user_data    = reg.get();
            const int32_t rc  = iface.ui->register_tool_renderer(host, &spec);
            if (rc != AGENTXX_RESULT_OK) {
                // 主程序拒绝 (重复注册 / 参数非法): 不留下无人用的脚本函数
                dropRenderCallbacksOnJsThread(pctx, reg.get());
                return JS_FALSE;
            }
            {
                std::lock_guard<std::mutex> lk(engine->renderMtx_);
                auto                        old = engine->renders_.find(toolName);
                if (old != engine->renders_.end()) {
                    // 旧注册项不能就地释放: user_data 可能还有回调在用
                    old->second->dead.store(true, std::memory_order_release);
                    engine->retiredRenders_.push_back(old->second);
                    engine->renders_.erase(old);
                }
                engine->renders_[toolName] = reg;
            }
            // 脚本先卸载时也要撤销 (主程序在插件实例卸载时同样会摘除)
            pctx->rollbackActions.push_back([engine, ui = iface.ui, host, toolName]() {
                engine->retireRenderOnJsThread(toolName);
                if (ui && ui->unregister_tool_renderer) {
                    auto tn
                        = agentxx::plugin::PluginStringView::from(toolName.data(), toolName.size());
                    ui->unregister_tool_renderer(host, &tn);
                }
            });
            return JS_TRUE;
        }

        case B_CLIENT_UNREGISTER_TOOL_RENDERER: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "unregisterToolRenderer: tool name required");
            }
            const std::string toolName = jsToCppString(ctx, argv[0]);
            if (toolName.empty()) {
                return JS_FALSE;
            }
            engine->retireRenderOnJsThread(toolName);
            if (!iface.ui || !iface.ui->unregister_tool_renderer) {
                return JS_FALSE;
            }
            auto tn = agentxx::plugin::PluginStringView::from(toolName.data(), toolName.size());
            return iface.ui->unregister_tool_renderer(host, &tn) == AGENTXX_RESULT_OK ? JS_TRUE
                                                                                     : JS_FALSE;
        }

        // ---- 展示面: 状态栏项 / 面板 / Info 段落 ----
        // 句柄都以整数 token 给脚本 (client 表的句柄是不透明指针, number 存不下),
        // token -> {类别, 指针, id} 记在实例的 clientHandles 里。
        case B_CLIENT_REGISTER_STATUS_ITEM:
        case B_CLIENT_REGISTER_PANEL:
        case B_CLIENT_REGISTER_INFO_SECTION: {
            if (argc < 1 || !JS_IsObject(argv[0])) {
                return JS_ThrowTypeError(ctx, "register*: spec object required");
            }
            const PluginxxHost* realHost = pctx->host;
            const ClientHandleKind kind = magic == B_CLIENT_REGISTER_STATUS_ITEM
                                              ? ClientHandleKind::StatusItem
                                              : (magic == B_CLIENT_REGISTER_PANEL
                                                     ? ClientHandleKind::Panel
                                                     : ClientHandleKind::InfoSection);
            const JSValueConst specObj = argv[0];
            const std::string  id      = strField(specObj, "id");
            if (id.empty()) {
                return JS_ThrowTypeError(ctx, "register*: id required");
            }
            auto idSv = agentxx::plugin::PluginStringView::from(id.data(), id.size());
            // 属性 JSON: 状态项是 {text, tooltip, segments...}, 面板/段落是 {title}
            std::string props = jsonField(specObj, "props");
            if (props.empty()) {
                props = "{}";
            }
            auto propsSv = agentxx::plugin::PluginStringView::from(props.data(), props.size());
            void*  handle = nullptr;
            int32_t rc    = AGENTXX_RESULT_ERR_UNSUPPORTED;
            if (kind == ClientHandleKind::StatusItem && iface.ui && iface.ui->register_status_item) {
                const int32_t align = [&]() {
                    JSValue v = JS_GetPropertyStr(ctx, specObj, "align");
                    int32_t out = 0;
                    if (JS_IsNumber(v)) {
                        JS_ToInt32(ctx, &out, v);
                    } else if (JS_IsString(v)) {
                        out = jsToCppString(ctx, v) == "right" ? 1 : 0;
                    }
                    JS_FreeValue(ctx, v);
                    return out;
                }();
                const int32_t order = [&]() {
                    JSValue v = JS_GetPropertyStr(ctx, specObj, "order");
                    int32_t out = 0;
                    if (JS_IsNumber(v)) {
                        JS_ToInt32(ctx, &out, v);
                    }
                    JS_FreeValue(ctx, v);
                    return out;
                }();
                handle = iface.ui->register_status_item(realHost, &idSv, &propsSv, align, order);
                rc     = handle ? AGENTXX_RESULT_OK : AGENTXX_RESULT_ERR_FAILED;
            } else if (
                kind == ClientHandleKind::Panel && iface.ui && iface.ui->register_panel
            ) {
                handle = iface.ui->register_panel(realHost, &idSv, &propsSv);
                rc     = handle ? AGENTXX_RESULT_OK : AGENTXX_RESULT_ERR_FAILED;
            } else if (
                kind == ClientHandleKind::InfoSection && iface.ui
                && iface.ui->register_info_section
            ) {
                handle = iface.ui->register_info_section(realHost, &idSv, &propsSv);
                rc     = handle ? AGENTXX_RESULT_OK : AGENTXX_RESULT_ERR_FAILED;
            }
            if (rc != AGENTXX_RESULT_OK || !handle) {
                return JS_NewInt64(ctx, 0); ///< 0 = 失败 (主程序不支持或 id 冲突)
            }
            const int64_t token = ++pctx->nextCallbackToken;
            pctx->clientHandles[token]
                = JsPluginCtx::ClientHandle{static_cast<int32_t>(kind), handle, id};
            // 脚本卸载时撤销 (主程序在实例卸载时也会摘除)
            // - 脚本自己注销过就不再碰: 句柄保活到实例析构, 但没必要再交一次
            const auto* uiForRollback = iface.ui;
            pctx->rollbackActions.push_back([uiForRollback, realHost, pctx, kind, token]() {
                auto live = pctx->clientHandles.find(token);
                if (live == pctx->clientHandles.end()) {
                    return;
                }
                void* handle = live->second.handle;
                pctx->clientHandles.erase(live);
                if (!uiForRollback) {
                    return;
                }
                if (kind == ClientHandleKind::StatusItem && uiForRollback->unregister_status_item) {
                    uiForRollback->unregister_status_item(realHost, static_cast<AgentxxStatusItem*>(handle));
                } else if (kind == ClientHandleKind::Panel && uiForRollback->unregister_panel) {
                    uiForRollback->unregister_panel(realHost, static_cast<AgentxxPanel*>(handle));
                } else if (
                    kind == ClientHandleKind::InfoSection && uiForRollback->unregister_info_section
                ) {
                    uiForRollback->unregister_info_section(
                        realHost,
                        static_cast<AgentxxInfoSection*>(handle)
                    );
                }
            });
            return JS_NewInt64(ctx, token);
        }

        case B_CLIENT_UPDATE_STATUS_ITEM:
        case B_CLIENT_UPDATE_PANEL:
        case B_CLIENT_UPDATE_INFO_SECTION: {
            if (argc < 2 || !JS_IsNumber(argv[0])) {
                return JS_ThrowTypeError(ctx, "update*: (handle, content) required");
            }
            int64_t token = 0;
            JS_ToInt64(ctx, &token, argv[0]);
            const ClientHandleKind kind = magic == B_CLIENT_UPDATE_STATUS_ITEM
                                              ? ClientHandleKind::StatusItem
                                              : (magic == B_CLIENT_UPDATE_PANEL
                                                     ? ClientHandleKind::Panel
                                                     : ClientHandleKind::InfoSection);
            auto it = pctx->clientHandles.find(token);
            if (it == pctx->clientHandles.end() || it->second.kind != static_cast<int32_t>(kind)) {
                return JS_FALSE; ///< 句柄无效 / 类别不对
            }
            // 状态项传属性对象; 面板/段落传 items 数组或 {"items":[...]}
            std::string content;
            if (kind == ClientHandleKind::StatusItem) {
                content = JS_IsString(argv[1]) ? jsToCppString(ctx, argv[1])
                                               : valueToJsonString(ctx, argv[1]);
            } else if (JS_IsArray(argv[1])) {
                const std::string arr = valueToJsonString(ctx, argv[1]);
                content               = fmt::format("{{\"items\":{}}}", arr.empty() ? "[]" : arr);
            } else if (JS_IsString(argv[1])) {
                content = jsToCppString(ctx, argv[1]);
            } else {
                content = valueToJsonString(ctx, argv[1]);
            }
            if (content.empty()) {
                return JS_FALSE;
            }
            auto contentSv = agentxx::plugin::PluginStringView::from(content.data(), content.size());
            int32_t rc     = AGENTXX_RESULT_ERR_UNSUPPORTED;
            if (kind == ClientHandleKind::StatusItem && iface.ui && iface.ui->update_status_item) {
                rc = iface.ui->update_status_item(
                    host,
                    static_cast<AgentxxStatusItem*>(it->second.handle),
                    &contentSv
                );
            } else if (kind == ClientHandleKind::Panel && iface.ui && iface.ui->update_panel) {
                rc = iface.ui->update_panel(
                    host,
                    static_cast<AgentxxPanel*>(it->second.handle),
                    &contentSv
                );
            } else if (
                kind == ClientHandleKind::InfoSection && iface.ui && iface.ui->update_info_section
            ) {
                rc = iface.ui->update_info_section(
                    host,
                    static_cast<AgentxxInfoSection*>(it->second.handle),
                    &contentSv
                );
            }
            return rc == AGENTXX_RESULT_OK ? JS_TRUE : JS_FALSE;
        }

        case B_CLIENT_UNREGISTER_STATUS_ITEM:
        case B_CLIENT_UNREGISTER_PANEL:
        case B_CLIENT_UNREGISTER_INFO_SECTION: {
            if (argc < 1 || !JS_IsNumber(argv[0])) {
                return JS_ThrowTypeError(ctx, "unregister*: handle required");
            }
            int64_t token = 0;
            JS_ToInt64(ctx, &token, argv[0]);
            const ClientHandleKind kind = magic == B_CLIENT_UNREGISTER_STATUS_ITEM
                                              ? ClientHandleKind::StatusItem
                                              : (magic == B_CLIENT_UNREGISTER_PANEL
                                                     ? ClientHandleKind::Panel
                                                     : ClientHandleKind::InfoSection);
            auto it = pctx->clientHandles.find(token);
            if (it == pctx->clientHandles.end() || it->second.kind != static_cast<int32_t>(kind)) {
                return JS_FALSE;
            }
            void* handle = it->second.handle;
            pctx->clientHandles.erase(it);
            if (kind == ClientHandleKind::StatusItem && iface.ui && iface.ui->unregister_status_item) {
                iface.ui->unregister_status_item(host, static_cast<AgentxxStatusItem*>(handle));
            } else if (kind == ClientHandleKind::Panel && iface.ui && iface.ui->unregister_panel) {
                iface.ui->unregister_panel(host, static_cast<AgentxxPanel*>(handle));
            } else if (
                kind == ClientHandleKind::InfoSection && iface.ui && iface.ui->unregister_info_section
            ) {
                iface.ui->unregister_info_section(host, static_cast<AgentxxInfoSection*>(handle));
            }
            return JS_TRUE;
        }

        // ---- 斜杠命令 (execute 同步返回动作 JSON) ----
        case B_CLIENT_REGISTER_COMMAND: {
            if (!iface.ui || !iface.ui->register_command) {
                return JS_FALSE; ///< 主程序不支持命令: 附加能力, 忽略
            }
            if (argc < 1 || !JS_IsObject(argv[0])) {
                return JS_ThrowTypeError(ctx, "registerCommand: spec object required");
            }
            const JSValueConst specObj = argv[0];
            const std::string  name    = strField(specObj, "name");
            const std::string  desc    = strField(specObj, "description");
            JSValue            execFn  = JS_GetPropertyStr(ctx, specObj, "execute");
            if (name.empty() || !JS_IsFunction(ctx, execFn)) {
                JS_FreeValue(ctx, execFn);
                return JS_ThrowTypeError(ctx, "registerCommand: (name, execute) required");
            }
            JsClientCallback* cb = engine->newClientCallbackOnJsThread(pctx, execFn);
            JS_FreeValue(ctx, execFn);
            if (!cb) {
                return JS_FALSE;
            }
            cb->label = name;
            auto nameSv = agentxx::plugin::PluginStringView::from(name.data(), name.size());
            auto descSv = agentxx::plugin::PluginStringView::from(desc.data(), desc.size());
            const int32_t rc
                = iface.ui->register_command(host, &nameSv, &descSv, &JsEngine::commandExecute, cb);
            if (rc != AGENTXX_RESULT_OK) {
                engine->dropClientCallbackOnJsThread(pctx, cb);
                return JS_FALSE; ///< 名字冲突 / 参数非法
            }
            pctx->clientCbByKey[fmt::format("cmd:{}", name)] = cb;
            pctx->rollbackActions.push_back([engine, ui = iface.ui, host, name, pctx]() {
                if (ui && ui->unregister_command) {
                    auto nv = agentxx::plugin::PluginStringView::from(name.data(), name.size());
                    ui->unregister_command(host, &nv);
                }
                engine->dropClientCallbackByKeyOnJsThread(pctx, fmt::format("cmd:{}", name));
            });
            return JS_TRUE;
        }

        case B_CLIENT_UNREGISTER_COMMAND: {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(ctx, "unregisterCommand: name required");
            }
            const std::string name = jsToCppString(ctx, argv[0]);
            if (name.empty()) {
                return JS_FALSE;
            }
            engine->dropClientCallbackByKeyOnJsThread(pctx, fmt::format("cmd:{}", name));
            if (!iface.ui || !iface.ui->unregister_command) {
                return JS_FALSE;
            }
            auto nv = agentxx::plugin::PluginStringView::from(name.data(), name.size());
            return iface.ui->unregister_command(host, &nv) == AGENTXX_RESULT_OK ? JS_TRUE : JS_FALSE;
        }

        // ---- 按钮动作绑定 (fire-and-forget) ----
        case B_CLIENT_BIND_ACTION: {
            if (!iface.ui || !iface.ui->bind_action_handler) {
                return JS_FALSE;
            }
            if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
                return JS_ThrowTypeError(ctx, "bindAction: (targetId, handler) required");
            }
            const std::string target = JS_IsString(argv[0]) ? jsToCppString(ctx, argv[0])
                                                            : std::string{};
            JsClientCallback* cb = engine->newClientCallbackOnJsThread(pctx, argv[1]);
            if (!cb) {
                return JS_FALSE;
            }
            cb->label  = target;
            auto tgtSv = agentxx::plugin::PluginStringView::from(target.data(), target.size());
            const int32_t rc
                = iface.ui->bind_action_handler(host, &tgtSv, &JsEngine::uiActionFire, cb);
            if (rc != AGENTXX_RESULT_OK) {
                engine->dropClientCallbackOnJsThread(pctx, cb);
                return JS_FALSE;
            }
            const std::string key = fmt::format("act:{}", target);
            if (auto old = pctx->clientCbByKey.find(key); old != pctx->clientCbByKey.end()) {
                engine->dropClientCallbackOnJsThread(pctx, old->second);
            }
            pctx->clientCbByKey[key] = cb;
            pctx->rollbackActions.push_back([engine, ui = iface.ui, host, target, pctx]() {
                if (ui && ui->unbind_action_handler) {
                    auto tv = agentxx::plugin::PluginStringView::from(target.data(), target.size());
                    ui->unbind_action_handler(host, &tv);
                }
                engine->dropClientCallbackByKeyOnJsThread(pctx, fmt::format("act:{}", target));
            });
            return JS_TRUE;
        }

        case B_CLIENT_UNBIND_ACTION: {
            const std::string target = JS_IsString(argv[0]) ? jsToCppString(ctx, argv[0])
                                                            : std::string{};
            engine->dropClientCallbackByKeyOnJsThread(pctx, fmt::format("act:{}", target));
            if (!iface.ui || !iface.ui->unbind_action_handler) {
                return JS_FALSE;
            }
            auto tv = agentxx::plugin::PluginStringView::from(target.data(), target.size());
            return iface.ui->unbind_action_handler(host, &tv) == AGENTXX_RESULT_OK ? JS_TRUE
                                                                                 : JS_FALSE;
        }

        // ---- 定时器 (主程序驱动) ----
        case B_CLIENT_SET_TIMER: {
            if (!iface.timer || !iface.timer->set_timer) {
                return JS_NewInt64(ctx, 0); ///< 主程序不支持定时器 / 动画关闭
            }
            if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
                return JS_ThrowTypeError(ctx, "setClientTimer: (spec, fn) required");
            }
            const JSValueConst specObj  = argc >= 1 && JS_IsObject(argv[0]) ? argv[0] : JS_UNDEFINED;
            const int32_t      interval = [&]() {
                JSValue v   = JS_GetPropertyStr(ctx, specObj, "intervalMs");
                int32_t out = 100;
                if (JS_IsNumber(v)) {
                    JS_ToInt32(ctx, &out, v);
                }
                JS_FreeValue(ctx, v);
                return out;
            }();
            const int32_t repeat = [&]() {
                JSValue v   = JS_GetPropertyStr(ctx, specObj, "repeat");
                int32_t out = 0;
                if (JS_IsNumber(v)) {
                    JS_ToInt32(ctx, &out, v);
                }
                JS_FreeValue(ctx, v);
                return out;
            }();
            const std::string ownerId = strField(specObj, "ownerId");
            const bool        pauseWhenHidden = [&]() {
                JSValue v = JS_GetPropertyStr(ctx, specObj, "pauseWhenHidden");
                const bool out = JS_ToBool(ctx, v) == 1;
                JS_FreeValue(ctx, v);
                return out;
            }();
            JsClientCallback* cb = engine->newClientCallbackOnJsThread(pctx, argv[1]);
            if (!cb) {
                return JS_NewInt64(ctx, 0);
            }
            auto ownerSv = agentxx::plugin::PluginStringView::from(ownerId.data(), ownerId.size());
            AgentxxTimerSpec spec{};
            spec.struct_size      = sizeof(AgentxxTimerSpec);
            spec.version          = 1;
            spec.interval_ms      = interval;
            spec.repeat           = repeat;
            spec.pause_when_hidden = pauseWhenHidden ? 1 : 0;
            spec.owner_id         = ownerSv;
            spec.on_timer         = &JsEngine::timerFire;
            spec.user_data        = cb;
            auto* handle          = iface.timer->set_timer(host, &spec);
            if (!handle) {
                engine->dropClientCallbackOnJsThread(pctx, cb);
                return JS_NewInt64(ctx, 0);
            }
            const int64_t token = ++pctx->nextCallbackToken;
            cb->label           = fmt::format("timer:{}", token);
            pctx->clientHandles[token]
                = JsPluginCtx::ClientHandle{static_cast<int32_t>(ClientHandleKind::Timer), handle, ownerId};
            pctx->clientCbByKey[cb->label] = cb;
            pctx->rollbackActions.push_back([engine, tm = iface.timer, host, pctx, token, key = cb->label]() {
                auto live = pctx->clientHandles.find(token);
                if (live == pctx->clientHandles.end()) {
                    return; ///< 脚本已取消: 回调也随取消释放了
                }
                auto* handle = static_cast<AgentxxTimer*>(live->second.handle);
                pctx->clientHandles.erase(live);
                if (tm && tm->cancel_timer) {
                    tm->cancel_timer(host, handle);
                }
                engine->dropClientCallbackByKeyOnJsThread(pctx, key);
            });
            return JS_NewInt64(ctx, token);
        }

        case B_CLIENT_CANCEL_TIMER: {
            if (argc < 1 || !JS_IsNumber(argv[0])) {
                return JS_ThrowTypeError(ctx, "cancelClientTimer: handle required");
            }
            int64_t token = 0;
            JS_ToInt64(ctx, &token, argv[0]);
            auto it = pctx->clientHandles.find(token);
            if (it == pctx->clientHandles.end()
                || it->second.kind != static_cast<int32_t>(ClientHandleKind::Timer)) {
                return JS_FALSE;
            }
            void* handle = it->second.handle;
            pctx->clientHandles.erase(it);
            engine->dropClientCallbackByKeyOnJsThread(pctx, fmt::format("timer:{}", token));
            if (iface.timer && iface.timer->cancel_timer) {
                iface.timer->cancel_timer(host, static_cast<AgentxxTimer*>(handle));
            }
            return JS_TRUE;
        }

        case B_CLIENT_IS_REGION_VISIBLE: {
            if (!iface.timer || !iface.timer->is_visible) {
                return JS_FALSE;
            }
            const std::string ownerId = JS_IsString(argv[0]) ? jsToCppString(ctx, argv[0])
                                                              : std::string{};
            auto ownerSv = agentxx::plugin::PluginStringView::from(ownerId.data(), ownerId.size());
            return iface.timer->is_visible(host, &ownerSv) != 0 ? JS_TRUE : JS_FALSE;
        }

        // ---- 全局快捷键 ----
        case B_CLIENT_REGISTER_KEYBIND: {
            if (!iface.keybind || !iface.keybind->register_keybind) {
                return JS_NewInt64(ctx, 0);
            }
            if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
                return JS_ThrowTypeError(ctx, "registerKeybind: (spec, fn) required");
            }
            const JSValueConst specObj = argc >= 1 && JS_IsObject(argv[0]) ? argv[0] : JS_UNDEFINED;
            const std::string  keys    = strField(specObj, "keys");
            const std::string  desc    = strField(specObj, "description");
            if (keys.empty()) {
                return JS_NewInt64(ctx, 0);
            }
            JsClientCallback* cb = engine->newClientCallbackOnJsThread(pctx, argv[1]);
            if (!cb) {
                return JS_NewInt64(ctx, 0);
            }
            cb->label    = keys;
            auto keysSv  = agentxx::plugin::PluginStringView::from(keys.data(), keys.size());
            auto descSv  = agentxx::plugin::PluginStringView::from(desc.data(), desc.size());
            AgentxxKeybindSpec spec{};
            spec.struct_size = sizeof(AgentxxKeybindSpec);
            spec.version     = 1;
            spec.keys        = keysSv;
            spec.description = descSv;
            spec.on_keybind  = &JsEngine::keybindFire;
            spec.user_data   = cb;
            auto* handle     = iface.keybind->register_keybind(host, &spec);
            if (!handle) {
                engine->dropClientCallbackOnJsThread(pctx, cb);
                return JS_NewInt64(ctx, 0); ///< 键位非法 / 已被占用 / 数量超限
            }
            const int64_t token = ++pctx->nextCallbackToken;
            pctx->clientHandles[token]
                = JsPluginCtx::ClientHandle{static_cast<int32_t>(ClientHandleKind::Keybind), handle, keys};
            pctx->clientCbByKey[fmt::format("keybind:{}", token)] = cb;
            pctx->rollbackActions.push_back(
                [engine, kb = iface.keybind, host, pctx, token, key = fmt::format("keybind:{}", token)]() {
                    auto live = pctx->clientHandles.find(token);
                    if (live == pctx->clientHandles.end()) {
                        return; ///< 脚本已注销: 回调也随注销释放了
                    }
                    auto* handle = static_cast<AgentxxKeybind*>(live->second.handle);
                    pctx->clientHandles.erase(live);
                    if (kb && kb->unregister_keybind) {
                        kb->unregister_keybind(host, handle);
                    }
                    engine->dropClientCallbackByKeyOnJsThread(pctx, key);
                }
            );
            return JS_NewInt64(ctx, token);
        }

        case B_CLIENT_UNREGISTER_KEYBIND: {
            if (argc < 1 || !JS_IsNumber(argv[0])) {
                return JS_ThrowTypeError(ctx, "unregisterKeybind: handle required");
            }
            int64_t token = 0;
            JS_ToInt64(ctx, &token, argv[0]);
            auto it = pctx->clientHandles.find(token);
            if (it == pctx->clientHandles.end()
                || it->second.kind != static_cast<int32_t>(ClientHandleKind::Keybind)) {
                return JS_FALSE;
            }
            void* handle = it->second.handle;
            pctx->clientHandles.erase(it);
            engine->dropClientCallbackByKeyOnJsThread(pctx, fmt::format("keybind:{}", token));
            if (iface.keybind && iface.keybind->unregister_keybind) {
                iface.keybind->unregister_keybind(host, static_cast<AgentxxKeybind*>(handle));
            }
            return JS_TRUE;
        }

        case B_CLIENT_LIST_KEYBINDS: {
            if (!iface.keybind || !iface.keybind->list_keybinds) {
                return JS_NewArray(ctx);
            }
            PluginxxString out{nullptr, 0};
            if (iface.keybind->list_keybinds(host, &out) != 0 || !out.data) {
                return JS_NewArray(ctx);
            }
            return takeHostJson(out, "<keybinds>");
        }

        // ---- 网络 (`agentxx.net`: client 插件与 agent 插件同一张表) ----
        // 同步入口用于快速操作; 慢操作走 Promise。网络服务自有线程, 不经 client
        // io 线程 —— 命令/渲染这类同步回调里调也不会与界面互等。
        case B_NET_CALL:
        case B_NET_CALL_ASYNC:
        case B_NET_CANCEL:
        case B_NET_FETCH:
        case B_NET_DOWNLOAD:
        case B_NET_WS_CONNECT:
        case B_NET_WS_SEND:
        case B_NET_WS_CLOSE:
        case B_NET_HTTP_SERVE:
        case B_NET_HTTP_RESPOND:
        case B_NET_INFO:
            return netBridgeCall(ctx, pctx, magic, argc, argv, iface.net);

        // ---- 其余通用表 (与 agent 侧同名同义, 只是数据来自客户端) ----
        case B_GET_CONFIG: {
            // 本插件的配置段 (yaml `plugin.list` 条目 config + args 的内容)
            if (!iface.config || !iface.config->get_config) {
                return JS_NULL;
            }
            PluginxxString out{nullptr, 0};
            const int32_t  rc = iface.config->get_config(host, &out);
            if (rc != 0 || !out.data) {
                return JS_NULL;
            }
            return takeHostJson(out, "<config>");
        }

        case B_LIST_PLUGINS: {
            // 本主程序已加载的插件清单 (两侧各自的清单)
            if (!iface.plugins || !iface.plugins->list_plugins) {
                return JS_NewArray(ctx);
            }
            PluginxxString out{nullptr, 0};
            if (iface.plugins->list_plugins(host, &out) != 0 || !out.data) {
                return JS_NewArray(ctx);
            }
            return takeHostJson(out, "<plugins>");
        }

        case B_GET_PLUGIN: {
            if (!iface.plugins || !iface.plugins->get_plugin || argc < 1
                || !JS_IsString(argv[0])) {
                return JS_NULL;
            }
            const std::string name = jsToCppString(ctx, argv[0]);
            auto              nameSv
                = agentxx::plugin::PluginStringView::from(name.data(), name.size());
            PluginxxString out{nullptr, 0};
            if (iface.plugins->get_plugin(host, &nameSv, &out) != 0 || !out.data) {
                return JS_NULL;
            }
            return takeHostJson(out, "<plugin>");
        }

        case B_DECLARE_BROADCAST: {
            if (!iface.broadcasts || !iface.broadcasts->declare_broadcast || argc < 1) {
                return JS_FALSE;
            }
            const std::string decl   = argsJsonText(ctx, argv[0]);
            auto              declSv = agentxx::plugin::PluginStringView::from(
                decl.data(),
                decl.size()
            );
            PluginxxString err{nullptr, 0};
            const int32_t  rc = iface.broadcasts->declare_broadcast(host, &declSv, &err);
            std::string    errText;
            if (err.data) {
                errText.assign(err.data, static_cast<size_t>(err.size));
                agentxx::plugin::PluginString::free(host, &err);
            }
            if (rc != PLUGINXX_RESULT_OK) {
                return throwJsError(
                    ctx,
                    fmt::format(
                        "declareBroadcast: {}",
                        errText.empty() ? std::string{"declare failed"} : errText
                    )
                );
            }
            return JS_TRUE;
        }

        case B_READ_BROADCAST: {
            if (!iface.broadcasts || !iface.broadcasts->read_broadcast || argc < 1) {
                return JS_NULL;
            }
            const std::string id   = jsToCppString(ctx, argv[0]);
            auto              idSv = agentxx::plugin::PluginStringView::from(id.data(), id.size());
            PluginxxString    out{nullptr, 0};
            const int32_t     rc = iface.broadcasts->read_broadcast(host, &idSv, &out);
            std::string       json;
            if (out.data) {
                json.assign(out.data, static_cast<size_t>(out.size));
                agentxx::plugin::PluginString::free(host, &out);
            }
            // 没声明 / 没当前值 / 已结束 都返回 null
            if (rc != PLUGINXX_RESULT_OK) {
                return JS_NULL;
            }
            return jsonTextToValue(ctx, json, "<broadcast>");
        }

        case B_END_BROADCAST: {
            if (!iface.broadcasts || !iface.broadcasts->end_broadcast || argc < 1) {
                return JS_FALSE;
            }
            const std::string id     = jsToCppString(ctx, argv[0]);
            const std::string reason = argc >= 2 ? argsJsonText(ctx, argv[1]) : std::string{};
            auto              idSv  = agentxx::plugin::PluginStringView::from(id.data(), id.size());
            auto              reaSv = agentxx::plugin::PluginStringView::from(
                reason.data(),
                reason.size()
            );
            PluginxxString err{nullptr, 0};
            const int32_t  rc = iface.broadcasts->end_broadcast(host, &idSv, &reaSv, &err);
            if (err.data) {
                agentxx::plugin::PluginString::free(host, &err);
            }
            return rc == PLUGINXX_RESULT_OK ? JS_TRUE : JS_FALSE;
        }

        case B_SUBSCRIBE: {
            if (!iface.broadcasts || !iface.broadcasts->subscribe_broadcast || argc < 2
                || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
                return JS_ThrowTypeError(ctx, "subscribe: (topic, handler, options?) required");
            }
            const std::string topic = jsToCppString(ctx, argv[0]);
            // 可选第三参 {intervalMs, mode, onEnd}: 与 agent 侧同一套语义
            JSValue     onEnd = JS_UNDEFINED;
            std::string optionsJson;
            if (argc >= 3 && JS_IsObject(argv[2])) {
                optionsJson      = subscribeOptionsJsonOf(ctx, argv[2]);
                JSValue onEndVal = JS_GetPropertyStr(ctx, argv[2], "onEnd");
                if (JS_IsFunction(ctx, onEndVal)) {
                    onEnd = onEndVal; ///< 接管引用, 分支结束时 Free
                } else {
                    JS_FreeValue(ctx, onEndVal);
                }
            }
            auto binding    = std::make_unique<JsHookBinding>();
            binding->engine = engine;
            binding->instanceKey = pctx->instanceKey;
            binding->topic  = topic;
            auto topicSv    = agentxx::plugin::PluginStringView::from(topic.data(), topic.size());
            auto optSv      = agentxx::plugin::PluginStringView::from(
                optionsJson.data(),
                optionsJson.size()
            );
            const PluginxxStringView* optPtr = optionsJson.empty() ? nullptr : &optSv;
            // client 侧没有 agent 作用域: 传 nullptr = 订阅主程序总线
            PluginxxSubscription* sub = nullptr;
            if (JS_IsFunction(ctx, onEnd) && iface.broadcasts->subscribe_broadcast_ex) {
                sub = iface.broadcasts->subscribe_broadcast_ex(
                    host,
                    nullptr,
                    &topicSv,
                    &JsEngine::eventFire,
                    &JsEngine::eventEndFire,
                    binding.get(),
                    optPtr,
                    nullptr
                );
            } else {
                sub = iface.broadcasts->subscribe_broadcast(
                    host,
                    nullptr,
                    &topicSv,
                    &JsEngine::eventFire,
                    binding.get(),
                    optPtr,
                    nullptr
                );
            }
            if (!sub) {
                JS_FreeValue(ctx, onEnd);
                return throwJsError(
                    ctx,
                    fmt::format("subscribe: host subscription failed: {}", topic)
                );
            }
            const int64_t token     = ++pctx->nextCallbackToken;
            // 处理器同时进实例订阅表: 派发 (`doEventFire`) 按它找回调
            // (client 侧没有 agent 挂载作用域, 句柄仍按 token 记在 clientSubs)
            const int64_t entryIdx = appendSubEntry(ctx, pctx, topic, argv[1], onEnd);
            binding->token         = entryIdx;
            pctx->clientSubs[token] = JsPluginCtx::ClientSub{sub, entryIdx};
            JS_FreeValue(ctx, onEnd);
            pctx->hookBindings.push_back(std::move(binding));
            pctx->rollbackActions.push_back([bc = iface.broadcasts, sub]() {
                if (bc && bc->unsubscribe_broadcast) {
                    bc->unsubscribe_broadcast(sub);
                }
            });
            return JS_NewInt64(ctx, token);
        }

        case B_UNSUBSCRIBE: {
            if (argc < 1 || !JS_IsNumber(argv[0])) {
                return JS_ThrowTypeError(ctx, "unsubscribe: token required");
            }
            int64_t token = 0;
            JS_ToInt64(ctx, &token, argv[0]);
            auto it = pctx->clientSubs.find(token);
            if (it != pctx->clientSubs.end()) {
                if (iface.broadcasts && iface.broadcasts->unsubscribe_broadcast
                    && it->second.sub != nullptr) {
                    iface.broadcasts->unsubscribe_broadcast(it->second.sub);
                }
                // 订阅表里的处理器一并置空 (JSValue 只在 JS 线程释放)
                if (it->second.entry >= 0) {
                    JS_SetPropertyUint32(
                        ctx,
                        pctx->agents,
                        static_cast<uint32_t>(it->second.entry),
                        JS_UNDEFINED
                    );
                }
                pctx->clientSubs.erase(it);
            }
            return JS_UNDEFINED;
        }

        case B_PUBLISH: {
            std::string topic = argc >= 1 ? jsToCppString(ctx, argv[0]) : std::string{};
            std::string payload = "{}";
            if (argc >= 2) {
                if (JS_IsString(argv[1])) {
                    payload = jsToCppString(ctx, argv[1]);
                } else if (JS_IsObject(argv[1])) {
                    const std::string text = valueToJsonString(ctx, argv[1]);
                    if (!text.empty()) {
                        payload = text;
                    }
                }
            }
            auto topicSv = agentxx::plugin::PluginStringView::from(topic.data(), topic.size());
            auto paySv = agentxx::plugin::PluginStringView::from(payload.data(), payload.size());
            agentxx::plugin::publishPluginBroadcast(host, iface.broadcasts, topicSv, paySv);
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
            t.instanceKey       = pctx->instanceKey;
            t.fn                = JS_DupValue(ctx, argv[0]);
            engine->timers_[id] = std::move(t);
            ++engine->timerEpoch_; // 定时器集合变化: 唤醒等待中的 JS 线程重算
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

        default:
            return JS_ThrowInternalError(
                ctx,
                "bridge method not available on client side (magic %d)",
                magic
            );
    }
}

// =====================================================================
// client 侧工具特化渲染: io 线程 → JS 线程的同步桥
// =====================================================================

int32_t PLUGINXX_CALL JsEngine::renderCollapsed(
    void*                         ud,
    const AgentxxToolRenderInput* input,
    AgentxxToolCollapsedOutput*   output
) {
    if (!ud || !input || !output) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    auto* reg    = static_cast<JsRenderReg*>(ud);
    auto* engine = reg->engine;
    if (!engine || reg->dead.load(std::memory_order_acquire)
        || reg->collapsedToken.load(std::memory_order_acquire) == 0) {
        return AGENTXX_RESULT_ERR_STATE;
    }
    try {
        auto slot       = std::make_shared<JsRenderSync>();
        slot->collapsed = true;
        slot->toolCallId
            = std::string(agentxx::plugin::PluginStringView::str(input->tool_call_id));
        slot->toolName = std::string(agentxx::plugin::PluginStringView::str(input->tool_name));
        slot->argsJson = std::string(agentxx::plugin::PluginStringView::str(input->args_json));
        slot->resultText
            = std::string(agentxx::plugin::PluginStringView::str(input->result_text));
        slot->finished = input->is_finished != 0;
        slot->isError  = input->is_error != 0;
        slot->maxWidth = input->max_width;
        if (!engine->invokeRenderSync(reg, slot)) {
            return AGENTXX_RESULT_ERR_STATE; ///< 超时 / 引擎停止: 回退默认渲染
        }
        if (slot->failed || slot->text.empty()) {
            return AGENTXX_RESULT_OK; ///< 没有自定义内容 (空输出)
        }
        output->text = agentxx::plugin::PluginString::from(engine->host(), slot->text);
        return AGENTXX_RESULT_OK;
    } catch (...) {
        return AGENTXX_RESULT_ERR_INTERNAL;
    }
}

int32_t PLUGINXX_CALL JsEngine::renderExpanded(
    void*                         ud,
    const AgentxxToolRenderInput* input,
    AgentxxToolExpandedOutput*    output
) {
    if (!ud || !input || !output) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    auto* reg    = static_cast<JsRenderReg*>(ud);
    auto* engine = reg->engine;
    if (!engine || reg->dead.load(std::memory_order_acquire)
        || reg->expandedToken.load(std::memory_order_acquire) == 0) {
        return AGENTXX_RESULT_ERR_STATE;
    }
    try {
        auto slot        = std::make_shared<JsRenderSync>();
        slot->collapsed  = false;
        slot->toolCallId
            = std::string(agentxx::plugin::PluginStringView::str(input->tool_call_id));
        slot->toolName = std::string(agentxx::plugin::PluginStringView::str(input->tool_name));
        slot->argsJson = std::string(agentxx::plugin::PluginStringView::str(input->args_json));
        slot->resultText
            = std::string(agentxx::plugin::PluginStringView::str(input->result_text));
        slot->finished = input->is_finished != 0;
        slot->isError  = input->is_error != 0;
        slot->maxWidth = input->max_width;
        if (!engine->invokeRenderSync(reg, slot)) {
            return AGENTXX_RESULT_ERR_STATE;
        }
        if (slot->failed || slot->text.empty()) {
            return AGENTXX_RESULT_OK;
        }
        output->items_json = agentxx::plugin::PluginString::from(engine->host(), slot->text);
        return AGENTXX_RESULT_OK;
    } catch (...) {
        return AGENTXX_RESULT_ERR_INTERNAL;
    }
}

bool JsEngine::invokeRenderSync(JsRenderReg* reg, const std::shared_ptr<JsRenderSync>& slot) {
    if (!reg || !slot) {
        return false;
    }
    // JS 线程正在等主程序 (client io 线程): 再让 io 线程等它就会互等, 直接放弃
    if (jsHostCallDepth_.load(std::memory_order_acquire) > 0) {
        return false;
    }
    if (!post([this, reg, slot]() {
            if (slot->abandoned.load(std::memory_order_acquire)) {
                return; ///< 调用方已超时: 不必再跑脚本
            }
            runRenderSyncOnJsThread(reg, slot);
        })) {
        return false; ///< 引擎已停止
    }
    std::unique_lock<std::mutex> lk(slot->m);
    const bool                   ok = slot->cv.wait_for(
        lk,
        std::chrono::milliseconds(kClientSyncTimeoutMs),
        [&slot]() {
            return slot->done;
        }
    );
    if (!ok) {
        slot->abandoned.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

void JsEngine::runRenderSyncOnJsThread(
    JsRenderReg*                        reg,
    const std::shared_ptr<JsRenderSync>& slot
) noexcept {
    auto finish = [&slot](bool failed, std::string text) {
        {
            std::lock_guard<std::mutex> lk(slot->m);
            slot->failed = failed;
            slot->text   = std::move(text);
            slot->done   = true;
        }
        slot->cv.notify_all();
    };
    try {
        if (!reg || reg->dead.load(std::memory_order_acquire) || stopping()) {
            finish(true, {});
            return;
        }
        auto pctx = findPlugin(reg->instanceKey);
        if (!pctx || pctx->deleted || !pctx->ctx) {
            finish(true, {});
            return;
        }
        const int64_t token = slot->collapsed ? reg->collapsedToken.load(std::memory_order_acquire)
                                              : reg->expandedToken.load(std::memory_order_acquire);
        if (token == 0) {
            finish(true, {});
            return;
        }
        auto fit = pctx->clientCallbacks.find(token);
        if (fit == pctx->clientCallbacks.end()) {
            finish(true, {});
            return;
        }
        JSContext* ctx = pctx->ctx;
        pctx->inflight++;
        struct InflightGuard {
            JsPluginCtx* p;

            explicit InflightGuard(JsPluginCtx* pctx) :
                p(pctx) {}

            ~InflightGuard() {
                p->inflight--;
            }
        } guard(pctx.get());

        JSValue input = JS_NewObject(ctx);
        JS_SetPropertyStr(
            ctx,
            input,
            "toolCallId",
            JS_NewStringLen(ctx, slot->toolCallId.data(), slot->toolCallId.size())
        );
        JS_SetPropertyStr(
            ctx,
            input,
            "toolName",
            JS_NewStringLen(ctx, slot->toolName.data(), slot->toolName.size())
        );
        JS_SetPropertyStr(
            ctx,
            input,
            "argsJson",
            JS_NewStringLen(ctx, slot->argsJson.data(), slot->argsJson.size())
        );
        JS_SetPropertyStr(
            ctx,
            input,
            "args",
            jsonTextToValue(ctx, slot->argsJson, "<tool_args>")
        );
        JS_SetPropertyStr(
            ctx,
            input,
            "resultText",
            JS_NewStringLen(ctx, slot->resultText.data(), slot->resultText.size())
        );
        JS_SetPropertyStr(ctx, input, "finished", slot->finished ? JS_TRUE : JS_FALSE);
        JS_SetPropertyStr(ctx, input, "isError", slot->isError ? JS_TRUE : JS_FALSE);
        JS_SetPropertyStr(ctx, input, "maxWidth", JS_NewInt32(ctx, slot->maxWidth));

        JSValue argv[1] = {input};
        JSValue ret     = JS_Call(ctx, fit->second, JS_UNDEFINED, 1, argv);
        JS_FreeValue(ctx, input);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(ctx);
            logAt(
                3,
                fmt::format(
                    "interpreter.js: tool renderer threw: {}",
                    valueToJsonString(ctx, exc)
                )
                    .c_str()
            );
            JS_FreeValue(ctx, exc);
            JS_FreeValue(ctx, ret);
            finish(true, {});
            return;
        }

        std::string text;
        bool        shapeOk = true;
        if (slot->collapsed) {
            if (JS_IsString(ret)) {
                text = jsToCppString(ctx, ret);
            } else if (JS_IsObject(ret) && !JS_IsFunction(ctx, ret)) {
                // {text: "..."} 形态 (方便回调顺带返回其它字段)
                JSValue tv = JS_GetPropertyStr(ctx, ret, "text");
                if (JS_IsString(tv)) {
                    text = jsToCppString(ctx, tv);
                }
                JS_FreeValue(ctx, tv);
            } else if (!JS_IsNull(ret) && !JS_IsUndefined(ret)) {
                shapeOk = false; ///< 数组/数字/函数等形态: 按"没有自定义内容"处理
            }
        } else {
            if (JS_IsString(ret)) {
                text = jsToCppString(ctx, ret); ///< 已是 items JSON 文本
            } else if (JS_IsArray(ret)) {
                text = valueToJsonString(ctx, ret);
            } else if (!JS_IsNull(ret) && !JS_IsUndefined(ret)) {
                shapeOk = false;
            }
        }
        JS_FreeValue(ctx, ret);
        finish(!shapeOk, std::move(text));
    } catch (...) {
        finish(true, {});
    }
}

void JsEngine::retireRenderOnJsThread(const std::string& toolName) {
    std::shared_ptr<JsRenderReg> reg;
    {
        std::lock_guard<std::mutex> lk(renderMtx_);
        auto                        it = renders_.find(toolName);
        if (it == renders_.end()) {
            return;
        }
        reg = it->second;
        reg->dead.store(true, std::memory_order_release);
        // 注册项不就地释放: user_data 可能还有回调在用, 留到引擎析构
        retiredRenders_.push_back(reg);
        renders_.erase(it);
    }
    if (auto pctx = findPlugin(reg->instanceKey)) {
        dropRenderCallbacksOnJsThread(pctx.get(), reg.get());
    }
}

void JsEngine::dropRenderCallbacksOnJsThread(JsPluginCtx* pctx, JsRenderReg* reg) {
    if (!pctx || !pctx->ctx || !reg) {
        return;
    }
    auto dropOne = [&](std::atomic<int64_t>& tokenRef) {
        const int64_t token = tokenRef.load(std::memory_order_acquire);
        if (token == 0) {
            return;
        }
        auto it = pctx->clientCallbacks.find(token);
        if (it != pctx->clientCallbacks.end()) {
            JS_FreeValue(pctx->ctx, it->second);
            pctx->clientCallbacks.erase(it);
        }
        tokenRef.store(0, std::memory_order_release);
    };
    dropOne(reg->collapsedToken);
    dropOne(reg->expandedToken);
}

// =====================================================================
// client 侧脚本回调: 命令 execute (同步) 与动作/定时器/快捷键 (投递)
// =====================================================================

JsClientCallback* JsEngine::newClientCallbackOnJsThread(JsPluginCtx* pctx, JSValueConst fn) {
    if (!pctx || !pctx->ctx || !JS_IsFunction(pctx->ctx, fn)) {
        return nullptr;
    }
    auto cb         = std::make_unique<JsClientCallback>();
    cb->engine      = this;
    cb->instanceKey = pctx->instanceKey;
    const int64_t token = ++pctx->nextCallbackToken;
    cb->fnToken.store(token, std::memory_order_release);
    pctx->clientCallbacks[token] = JS_DupValue(pctx->ctx, fn);
    auto* raw                    = cb.get();
    {
        std::lock_guard<std::mutex> lk(clientCbMtx_);
        clientCbs_.push_back(std::move(cb));
    }
    return raw;
}

void JsEngine::dropClientCallbackOnJsThread(JsPluginCtx* pctx, JsClientCallback* cb) {
    if (!cb) {
        return;
    }
    cb->dead.store(true, std::memory_order_release);
    const int64_t token = cb->fnToken.exchange(0, std::memory_order_acq_rel);
    if (!pctx || !pctx->ctx || token == 0) {
        return;
    }
    auto it = pctx->clientCallbacks.find(token);
    if (it != pctx->clientCallbacks.end()) {
        JS_FreeValue(pctx->ctx, it->second);
        pctx->clientCallbacks.erase(it);
    }
}

void JsEngine::dropClientCallbackByKeyOnJsThread(JsPluginCtx* pctx, const std::string& key) {
    if (!pctx) {
        return;
    }
    auto it = pctx->clientCbByKey.find(key);
    if (it == pctx->clientCbByKey.end()) {
        return;
    }
    dropClientCallbackOnJsThread(pctx, it->second);
    pctx->clientCbByKey.erase(it);
}

bool JsEngine::invokeCallbackSync(
    JsClientCallback* cb,
    const std::string& argsJson,
    std::string&       out
) {
    if (!cb || cb->dead.load(std::memory_order_acquire)
        || cb->fnToken.load(std::memory_order_acquire) == 0) {
        return false;
    }
    // JS 线程正在等主程序: 再让主程序等它就会互等, 直接按失败处理
    if (jsHostCallDepth_.load(std::memory_order_acquire) > 0) {
        return false;
    }
    auto slot      = std::make_shared<JsCallbackSync>();
    slot->argsJson = argsJson;
    if (!post([this, cb, slot]() {
            if (slot->abandoned.load(std::memory_order_acquire)) {
                return;
            }
            runCallbackSyncOnJsThread(cb, slot);
        })) {
        return false; ///< 引擎已停止
    }
    std::unique_lock<std::mutex> lk(slot->m);
    const bool                   ok = slot->cv.wait_for(
        lk,
        std::chrono::milliseconds(kClientSyncTimeoutMs),
        [&slot]() {
            return slot->done;
        }
    );
    if (!ok) {
        slot->abandoned.store(true, std::memory_order_release);
        return false;
    }
    if (slot->failed) {
        return false;
    }
    out = std::move(slot->text);
    return true;
}

void JsEngine::postCallback(JsClientCallback* cb, const std::string& argsJson) {
    if (!cb || cb->dead.load(std::memory_order_acquire)) {
        return;
    }
    post([this, cb, argsJson]() {
        runCallbackOnJsThread(cb, argsJson);
    });
}

void JsEngine::runCallbackSyncOnJsThread(
    JsClientCallback*             cb,
    const std::shared_ptr<JsCallbackSync>& slot
) noexcept {
    auto finish = [&slot](bool failed, std::string text) {
        {
            std::lock_guard<std::mutex> lk(slot->m);
            slot->failed = failed;
            slot->text   = std::move(text);
            slot->done   = true;
        }
        slot->cv.notify_all();
    };
    try {
        const int64_t token = cb ? cb->fnToken.load(std::memory_order_acquire) : 0;
        if (!cb || cb->dead.load(std::memory_order_acquire) || token == 0 || stopping()) {
            finish(true, {});
            return;
        }
        auto pctx = findPlugin(cb->instanceKey);
        if (!pctx || pctx->deleted || !pctx->ctx) {
            finish(true, {});
            return;
        }
        auto fit = pctx->clientCallbacks.find(token);
        if (fit == pctx->clientCallbacks.end()) {
            finish(true, {});
            return;
        }
        JSContext* ctx = pctx->ctx;
        pctx->inflight++;
        struct InflightGuard {
            JsPluginCtx* p;

            explicit InflightGuard(JsPluginCtx* pctx) :
                p(pctx) {}

            ~InflightGuard() {
                p->inflight--;
            }
        } guard(pctx.get());

        JSValue arg = jsonTextToValue(ctx, slot->argsJson, "<callback_args>");
        JSValue ret = JS_Call(ctx, fit->second, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(ctx);
            logAt(
                3,
                fmt::format(
                    "interpreter.js: client callback `{}` threw: {}",
                    cb->label,
                    valueToJsonString(ctx, exc)
                )
                    .c_str()
            );
            JS_FreeValue(ctx, exc);
            JS_FreeValue(ctx, ret);
            finish(true, {});
            return;
        }
        std::string text;
        if (JS_IsString(ret)) {
            text = jsToCppString(ctx, ret);
        } else if (JS_IsNull(ret) || JS_IsUndefined(ret)) {
            text.clear();
        } else {
            text = valueToJsonString(ctx, ret);
        }
        JS_FreeValue(ctx, ret);
        finish(false, std::move(text));
    } catch (...) {
        finish(true, {});
    }
}

void JsEngine::runCallbackOnJsThread(JsClientCallback* cb, const std::string& argsJson) noexcept {
    try {
        const int64_t token = cb ? cb->fnToken.load(std::memory_order_acquire) : 0;
        if (!cb || cb->dead.load(std::memory_order_acquire) || token == 0 || stopping()) {
            return;
        }
        auto pctx = findPlugin(cb->instanceKey);
        if (!pctx || pctx->deleted || !pctx->ctx) {
            return;
        }
        auto fit = pctx->clientCallbacks.find(token);
        if (fit == pctx->clientCallbacks.end()) {
            return;
        }
        JSContext* ctx = pctx->ctx;
        pctx->inflight++;
        struct InflightGuard {
            JsPluginCtx* p;

            explicit InflightGuard(JsPluginCtx* pctx) :
                p(pctx) {}

            ~InflightGuard() {
                p->inflight--;
            }
        } guard(pctx.get());

        JSValue arg = jsonTextToValue(ctx, argsJson, "<callback_args>");
        JSValue ret = JS_Call(ctx, fit->second, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(ctx);
            logAt(
                3,
                fmt::format(
                    "interpreter.js: client callback `{}` threw: {}",
                    cb->label,
                    valueToJsonString(ctx, exc)
                )
                    .c_str()
            );
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, ret);
    } catch (...) {
    }
}

int32_t PLUGINXX_CALL JsEngine::commandExecute(
    void*                     ud,
    const PluginxxStringView* args_json,
    PluginxxString*           actionOut,
    PluginxxString*           errorOut
) {
    auto* cb = static_cast<JsClientCallback*>(ud);
    if (!cb || !cb->engine) {
        return AGENTXX_RESULT_ERR_STATE;
    }
    auto*               engine = cb->engine;
    const PluginxxHost* host   = engine->host();
    if (!host) {
        return AGENTXX_RESULT_ERR_STATE;
    }
    try {
        const std::string args
            = (args_json && args_json->data)
                  ? std::string(args_json->data, static_cast<size_t>(args_json->size))
                  : std::string{"{}"};
        std::string out;
        if (!engine->invokeCallbackSync(cb, args, out)) {
            if (errorOut) {
                *errorOut = agentxx::plugin::PluginString::fromCstr(
                    host,
                    "script command callback failed"
                );
            }
            return AGENTXX_RESULT_ERR_STATE;
        }
        if (actionOut) {
            *actionOut = agentxx::plugin::PluginString::from(host, out);
        }
        return AGENTXX_RESULT_OK;
    } catch (...) {
        return AGENTXX_RESULT_ERR_INTERNAL;
    }
}

void PLUGINXX_CALL JsEngine::uiActionFire(const AgentxxUiActionContext* ctx, void* ud) {
    auto* cb = static_cast<JsClientCallback*>(ud);
    if (!cb || !cb->engine || !ctx) {
        return;
    }
    auto*               engine = cb->engine;
    const PluginxxHost* host   = engine->host();
    if (!host) {
        return;
    }
    try {
        const auto* jsonIface
            = pluginxx::queryInterface<PluginxxJsonIface>(host, PLUGINXX_IFACE_JSON);
        const std::string owner{agentxx::plugin::PluginStringView::str(ctx->owner_id)};
        const std::string action{agentxx::plugin::PluginStringView::str(ctx->action_id)};
        std::string       args{agentxx::plugin::PluginStringView::str(ctx->action_args)};
        if (args.empty()) {
            args = "{}";
        }
        const std::string payload = fmt::format(
            "{{\"ownerId\":{},\"actionId\":{},\"args\":{}}}",
            agentxx::plugin::jsonEscape(host, jsonIface, owner),
            agentxx::plugin::jsonEscape(host, jsonIface, action),
            args
        );
        engine->postCallback(cb, payload);
    } catch (...) {
    }
}

void PLUGINXX_CALL JsEngine::timerFire(void* ud) {
    auto* cb = static_cast<JsClientCallback*>(ud);
    if (!cb || !cb->engine) {
        return;
    }
    cb->engine->postCallback(cb, "{}");
}

void PLUGINXX_CALL JsEngine::keybindFire(void* ud) {
    auto* cb = static_cast<JsClientCallback*>(ud);
    if (!cb || !cb->engine) {
        return;
    }
    auto*               engine = cb->engine;
    const PluginxxHost* host   = engine->host();
    if (!host) {
        return;
    }
    try {
        const auto* jsonIface
            = pluginxx::queryInterface<PluginxxJsonIface>(host, PLUGINXX_IFACE_JSON);
        const std::string payload = fmt::format(
            "{{\"keys\":{}}}",
            agentxx::plugin::jsonEscape(host, jsonIface, cb->label)
        );
        engine->postCallback(cb, payload);
    } catch (...) {
    }
}

// =====================================================================
// 插件入口 (主程序 dlsym)
// =====================================================================

extern "C" PLUGINXX_EXPORT const PluginxxInfo* agentxx_plugin_agent_get_info(void) {
    static const PluginxxInfo info{
        PLUGINXX_API_VERSION,
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
/// - caller_host: JS 插件 (C++ 壳) 的主程序句柄 —— 脚本内 agentxx.registerTool
///   等注册动作经此挂到调用方插件实例 (主程序 detachAll 统一清理)
/// - "load" args: {"name": JS 插件名, "path": 脚本文件路径}; 完成 payload:
///   {"ok": true, "tools": [...]} (JSON, host->alloc)
/// - "unload" args: {"name": JS 插件名}; 投递式, 内联完成 {"ok": true}
static void* PLUGINXX_CALL jsCapStart(
    void*                         ctx,
    const PluginxxHost*           caller_host,
    const PluginxxStringView*     method,
    const PluginxxStringView*     args_json,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    // C ABI 回调异常守卫: 能力方法 start 由主程序 op 驱动器在 io 线程调用,
    // 内含接口查询/文件读取/字符串操作等可抛路径, 异常转 error_out 失败
    auto* engine = static_cast<JsEngine*>(ctx); ///< 提升至 try 外 (catch 日志闭包使用)
    try {
        // 本实例的句柄: 查接口表、分配堆字符串一律用它
        const PluginxxHost* selfHost = engine != nullptr ? engine->host() : caller_host;
        // 脚本里的注册 (工具/订阅/定时器) 归属于**调用方插件实例**: 主程序
        // 卸载那个插件时整批清理。能力链会把调用方的主程序视图作为 caller_host
        // 传进来; 主程序自己发起 (无调用方) 时才落到本实例。
        const PluginxxHost* scriptHost = caller_host != nullptr ? caller_host : selfHost;
        auto                setErr   = [&](const char* msg) {
            if (error_out && selfHost) {
                *error_out = agentxx::plugin::PluginString::fromCstr(selfHost, msg);
            }
            return nullptr;
        };
        if (!engine || !notify) {
            return setErr("interpreter.js: invalid invoke");
        }
        // 能力不再有独立的 method 参数: 要做什么写在 args 的 `op` 里 (`load` / `unload`,
        // 缺省按 `load`)
        std::string argsStr{
            args_json && args_json->data ? args_json->data : "{}",
            args_json ? static_cast<size_t>(args_json->size) : 0
        };
        // 参数解析经主程序 agent 侧的 agentxx.agent.json 接口表 (对转义/嵌套结构可靠)
        const agentxx::plugin::AgentIfaces selfIf
            = agentxx::plugin::AgentIfaces::query(selfHost);
        if (!selfIf.json || !selfIf.json->json_get_string) {
            return setErr("interpreter.js: host lacks agentxx.agent.json interface");
        }
        auto argStr = [&](const char* key, std::string& out) -> bool {
            PluginxxString v{nullptr, 0};
            auto argsSv = agentxx::plugin::PluginStringView::from(argsStr.data(), argsStr.size());
            auto keySv  = agentxx::plugin::PluginStringView::from(key, std::strlen(key));
            selfIf.json->json_get_string(selfHost, &argsSv, &keySv, &v);
            if (!v.data) {
                return false;
            }
            out.assign(v.data, static_cast<size_t>(v.size));
            agentxx::plugin::PluginString::free(selfHost, &v);
            return true;
        };
        std::string op = "load";
        argStr("op", op);

        if (op == "load") {
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
            // 脚本登记与实例创建分离: 脚本先入登记表 (此时还没有 agent 挂载就不执行),
            // 每个已挂载 agent 各跑一份 (脚本顶层注册归属该 agent)
            engine->loadScriptAsync(scriptHost, name, path, code, *notify);
            // 活动 op 占位 (主程序只等完成通知; execute_poll 留 NULL)。
            // 用实例成员地址而非函数级 static: 同一动态库多实例并存时
            // 每个实例各自持有稳定地址, 不共享可变静态存储。
            return engine->capOpToken();
        }

        if (op == "unload") {
            std::string name;
            if (!argStr("name", name) || name.empty()) {
                return setErr("interpreter.js unload: name (string) required");
            }
            // done 在各个实例真正卸载之后 (JS 线程执行注册回滚与定时器清理) 触发
            engine->unloadScriptAsync(name, *notify);
            return engine->capOpToken();
        }

        {
            std::string capId{
                method && method->data ? method->data : "",
                method ? static_cast<size_t>(method->size) : 0
            };
            std::string _msg
                = fmt::format("interpreter.js ({}): unknown op `{}`", capId, op);
            setErr(_msg.c_str());
        }
        return nullptr;
    } catch (...) {
        // 异常分类上报 + 尽力设置 error_out (主程序按 OP_FAILED 处理)
        ::agentxx::plugin::reportCurrentException([engine](const char* m) noexcept {
            if (engine) {
                engine->guardLog(m);
            }
        });
        if (error_out) {
            const PluginxxHost* errHost = engine != nullptr ? engine->host() : caller_host;
            if (errHost != nullptr) {
                *error_out = agentxx::plugin::PluginString::fromCstr(
                    errHost,
                    "interpreter.js: internal exception"
                );
            }
        }
        return nullptr;
    }
}

extern "C" PLUGINXX_EXPORT int
    agentxx_plugin_agent_create(const PluginxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: create 只构造上下文 (查询接口 + 加载主程序句柄),
    // 不创建 runtime/线程, 也不提交任何运行时注册; 异常返回
    // -1 走主程序加载失败清理路径
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
            // 不可用函数级 static —— 多实例加载时各主程序接口表指针不同)
            const agentxx::plugin::AgentIfaces s_if = agentxx::plugin::AgentIfaces::query(host);
            if (!s_if.capabilities || !s_if.capabilities->register_capability || !s_if.log) {
                delete engine;
                raw = nullptr;
                return -1;
            }
            *plugin_ctx = engine; ///< 所有权移交主程序 (destroy 时取回归还)
            return 0;
        }
    );
}

/// 引擎 start (start 事务): 创建 JSRuntime + 专用 JS 线程, 并注册
/// 能力 "interpreter.js"。
///
/// - create 阶段不启动线程/不注册: disable→enable 往返等价于一次 stop+start,
///   引擎线程与脚本上下文都按事务重建, 不存在"线程已存在但实例已停用"的中间态;
/// - 线程启动失败或能力注册失败 → 返回 NULL + error, 主程序回滚本次加载 (注册
///   失败时先停止刚启动的引擎, 不留线程/运行时残留);
/// - 重复 start: 引擎已运行时幂等成功, 能力注册在主程序侧按 owner+名称去重。
static void* PLUGINXX_CALL jsEngineStart(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
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
        const PluginxxHost*                host  = engine->host();
        const agentxx::plugin::AgentIfaces iface = agentxx::plugin::AgentIfaces::query(host);
        if (!iface.capabilities || !iface.capabilities->register_capability) {
            engine->requestStop(nullptr);
            return setErr("interpreter.js start: host lacks capabilities interface");
        }
        if (engine->capabilityRegistered()) {
            // 重复 start (幂等): 能力仍由本实例持有, 主程序尚未撤销
        } else {
            // 能力 "interpreter.js" (异步方法处理器三件套): JS 插件 (C++ 壳) 经
            // invoke_capability(_async) 把脚本代码交给本引擎执行 —— 插件间通信,
            // 主程序不参与; load/unload 均为异步完成 (JS 线程执行)
            auto capSv = agentxx::plugin::PluginStringView::fromCstr("interpreter.js");
            // 登记形状: 短名 (主程序补成 `plugin.<本插件 id>.interpreter.js`) + 只实现
            // impl 阶段 + 异步处理器 (jsCapStart 自己回调 notify)
            PluginxxCapabilitySpec spec{};
            spec.version     = PLUGINXX_API_VERSION;
            spec.struct_size = sizeof(PluginxxCapabilitySpec);
            spec.id          = capSv;
            spec.impls       = PLUGINXX_CAPABILITY_IMPLS_ONE;
            spec.merge       = PLUGINXX_CAPABILITY_MERGE_NONE;
            spec.phases      = PLUGINXX_CAPABILITY_PHASE_MASK_IMPL;
            spec.phase       = PLUGINXX_CAPABILITY_PHASE_IMPL;
            spec.sync        = 0;
            spec.run_start   = &jsCapStart;
            spec.cancel      = nullptr;
            spec.user_data   = engine;
            PluginxxString regErr{nullptr, 0};
            int            rc = iface.capabilities->register_capability(host, nullptr, &spec, &regErr);
            if (regErr.data) {
                agentxx::plugin::PluginString::free(host, &regErr);
            }
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
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
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
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &errSv);
        }
        return nullptr;
    }
}

/// 引擎 stop (stop 事务): 停止 JS 线程并释放 runtime。
///
/// - 立即拒绝新任务: 之后到达的能力调用/工具执行一律失败, 不再排队;
/// - 队列中尚未开始的任务按取消终结 (不执行插件 JS 代码); 已经开始的执行
///   由 drivePromise 观察 stop 标志后按取消完成;
/// - JS 线程的 join 与 JS_FreeRuntime 交给收尾线程执行 (不能在 IO 线程直接
///   join: JS 线程可能在主程序 vtable 调用中等待 IO 线程), done 在收尾完成后
///   从收尾线程上报 (完成通知允许来自任意线程)。
static void* PLUGINXX_CALL jsEngineStop(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
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
        PluginxxOperatorNotify ntfCopy = *notify;
        // 能力注册由主程序在停用/卸载时统一撤销 (detachInstanceRegistrations /
        // detachAll), 这里只清本实例的幂等标记。
        engine->setCapabilityRegistered(false);
        engine->requestStop([engine, ntfCopy]() mutable {
            ntfCopy.done(ntfCopy.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
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
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &errSv);
        }
        return nullptr;
    }
}

extern "C" PLUGINXX_EXPORT void* agentxx_plugin_agent_start(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    // 入口为 noexcept 守卫函数 (内部 try/catch 已分类上报, 异常不穿越 C ABI)
    return jsEngineStart(plugin_ctx, notify, err);
}

extern "C" PLUGINXX_EXPORT void* agentxx_plugin_agent_stop(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    return jsEngineStop(plugin_ctx, notify, err);
}

extern "C" PLUGINXX_EXPORT void* agentxx_plugin_agent_mount(
    void*                              plugin_ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    err
) {
    return agentxx::plugin::guardCall(
        [](const char* m) noexcept {
            (void)m;
        },
        static_cast<void*>(nullptr),
        [&]() -> void* {
            auto* engine = static_cast<JsEngine*>(plugin_ctx);
            if (!engine) {
                agentxx::plugin::PluginString::set(
                    nullptr,
                    err,
                    "interpreter.js mount: null context"
                );
                return nullptr;
            }
            if (!notify) {
                agentxx::plugin::PluginString::set(
                    engine->host(),
                    err,
                    "interpreter.js mount: notify required"
                );
                return nullptr;
            }
            // 登记挂载作用域: 已登记的脚本会为这个 agent 各建一份实例;
            // per-agent 状态带回主程序, unmount 时原样交回 (卸载时靠它定位作用域)
            auto  scope = JsEngine::mountScopeOf(spec);
            auto* handle = new JsEngine::ScopeHandle{scope};
            engine->attachMountScope(scope);
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
            return handle;
        }
    );
}

extern "C" PLUGINXX_EXPORT void* agentxx_plugin_agent_unmount(
    void*                         plugin_ctx,
    void*                         mount_ud,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    return agentxx::plugin::guardCall(
        [](const char* m) noexcept {
            (void)m;
        },
        static_cast<void*>(nullptr),
        [&]() -> void* {
            // 注销该 agent 的脚本实例: 工具/权限/提示词/订阅由主程序按挂载记录撤销,
            // 这里只收拾 JS 侧 (每个实例一份 JSContext + 定时器)
            auto*       engine = static_cast<JsEngine*>(plugin_ctx);
            auto*       handle = static_cast<JsEngine::ScopeHandle*>(mount_ud);
            if (engine && handle) {
                engine->detachMountScope(handle->scope);
            }
            delete handle;
            if (notify && notify->done) {
                notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
            }
            return nullptr;
        }
    );
}

extern "C" PLUGINXX_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* engine = static_cast<JsEngine*>(plugin_ctx);
    // C ABI 边界异常守卫: 销毁回调 (同步等待停止收尾 = 停线程 + 释放 runtime)
    // 异常不得外泄, 否则 JS 线程/runtime 泄漏且主程序卸载流程被打断;
    // 先借本实例主程序句柄加载日志闭包再 delete (delete 后不得访问成员)
    const PluginxxHost*     ownHost = engine ? engine->host() : nullptr;
    const PluginxxLogIface* ownLog  = nullptr;
    if (ownHost && ownHost->vtable && ownHost->vtable->query_interface) {
        auto iidSv = agentxx::plugin::PluginStringView::fromCstr(PLUGINXX_IFACE_LOG);
        ownLog
            = static_cast<const PluginxxLogIface*>(ownHost->vtable->query_interface(ownHost, &iidSv)
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

// =====================================================================
// client 侧入口 (同一动态库双入口)
//
// 与 agent 侧完全同一套引擎: 线程 / 运行时 / 实例 / 定时器 / 停止收尾共用, 只有
// 桥与实例作用域按侧分流 (见 [JsEngine::Side])。client 表成员不带作用域, 因此
// 一份脚本一个实例, 没有 mount/unmount 入口。
// =====================================================================

extern "C" PLUGINXX_EXPORT const AgentxxClientPluginInfo* agentxx_plugin_client_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        static_cast<const AgentxxClientPluginInfo*>(nullptr),
        [&]() -> const AgentxxClientPluginInfo* {
            static const AgentxxClientPluginInfo info{
                AGENTXX_CLIENT_PLUGIN_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("agentxx_javascript_engine"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "JS interpreter plugin (QuickJS): hosts type:js plugins (client side)"
                ),
            };
            return &info;
        }
    );
}

extern "C" PLUGINXX_EXPORT int
    agentxx_plugin_client_create(const PluginxxHost* host, void** plugin_ctx) {
    // create 只构造上下文 (不创建 runtime/线程, 不做任何注册)
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
            auto* engine = new JsEngine(JsEngine::Side::Client);
            raw          = engine; ///< create 段内异常路径日志可用
            engine->setEngineHost(host);
            // 引擎自身要用的通用表 (日志 / JSON 参数解析 / 能力注册) 必须存在
            const agentxx::plugin::ClientIfaces s_if
                = agentxx::plugin::ClientIfaces::query(host);
            const ::PluginxxCapabilitiesIface* caps
                = pluginxx::queryInterface<::PluginxxCapabilitiesIface>(
                    host,
                    PLUGINXX_IFACE_CAPABILITIES
                );
            if (!s_if.log || !s_if.json || !caps) {
                delete engine;
                raw = nullptr;
                return -1;
            }
            *plugin_ctx = engine; ///< 所有权移交主程序 (destroy 时取回归还)
            return 0;
        }
    );
}

extern "C" PLUGINXX_EXPORT void* PLUGINXX_CALL agentxx_plugin_client_start(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    // 与 agent 侧同一套 start 事务: 建 runtime + JS 线程, 注册能力 "interpreter.js"
    return jsEngineStart(plugin_ctx, notify, err);
}

extern "C" PLUGINXX_EXPORT void* PLUGINXX_CALL agentxx_plugin_client_stop(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    return jsEngineStop(plugin_ctx, notify, err);
}

extern "C" PLUGINXX_EXPORT void agentxx_plugin_client_destroy(void* plugin_ctx) {
    auto* engine = static_cast<JsEngine*>(plugin_ctx);
    const PluginxxHost*     ownHost = engine ? engine->host() : nullptr;
    const PluginxxLogIface* ownLog  = nullptr;
    if (ownHost && ownHost->vtable && ownHost->vtable->query_interface) {
        auto iidSv = agentxx::plugin::PluginStringView::fromCstr(PLUGINXX_IFACE_LOG);
        ownLog
            = static_cast<const PluginxxLogIface*>(ownHost->vtable->query_interface(ownHost, &iidSv)
            );
    }
    agentxx::plugin::guardCallVoid(
        [ownHost, ownLog](const char* m) noexcept {
            agentxx::plugin::logTo(ownHost, ownLog, 4, "agentxx_javascript_engine", m);
        },
        [engine]() noexcept {
            delete engine;
        }
    );
}
