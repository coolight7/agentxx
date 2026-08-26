/*
 * agentxx/plugin/plugin_poll_loop.h —— poll 寄生驱动便捷层 (header-only C++)
 *
 * 定位: 统一异步操作模型下"协程异步插件"的第三种注册姿势。插件用任意
 * 可步进(embed)的异步框架编写协程, 经三件套嫁接到宿主 io 线程协作式交错
 * 执行 —— 与内置工具完全同线程同语义, 零额外线程、零数据竞争:
 *   - start  : 把工作协程 co_spawn 到本实例 PollLoop 的 io_context 上
 *              (仅注册初始化, 立即返回 op 句柄)
 *   - poll   : io_context::poll() 非阻塞推进一步 (执行全部就绪 handler /
 *              恢复就绪协程), 返回建议延迟 (0=让出与其他协程交错 / N=小睡后重来)
 *   - cancel : 置 Job.cancelFlag, 工作协程在阶段边界轮询到后协作退出
 *   - done   : 工作协程终结时上报 (运行在宿主 io 线程的 poll 推进内)
 *
 * 三型选型指引 (详见 docs/agent/plugins.md §4.2.2):
 *   A 快同步 <~1ms             → plugin_tool_sync.h inline 垫片
 *   B 阻塞库调用(磁盘/CPU 密集) → plugin_tool_sync.h sync 垫片 (offload 池)
 *   C 真异步 IO(socket/子进程)  → 本头 polled 三件套 (宿主 io 线程寄生驱动)
 *   不可步进的异步框架(自带线程 runtime) → 自有线程 + 手写三件套 notify
 *   (参考 agentxx_javascript_engine), 或 offload
 *
 * 【硬性约束】工作协程每次"就绪段"(两次挂起之间的同步代码)必须在 ~100ms 内
 * 回到挂起点 (宿主看门狗阈值); 协程体内禁止任何阻塞调用 —— 阻塞会卡住整个
 * 宿主事件循环。CPU/阻塞密集段应切片或改走 offload (B 型混合: offload 完成
 * 回调 post 回本 loop 继续)。协程内调用宿主 io 线程约束接口表 (get_work_dir /
 * is_cancelled 等) 安全 —— 宿主 ioCallSync 检测到已在 io 线程时直接内联执行。
 *
 * 【事件不丢失保证】Linux epoll 为 level-triggered / Windows IOCP 完成包排队,
 * 两次 poll 之间到达的事件不会丢失, 下次 pollOnce 必然取得。
 *
 * 【多实例契约】PollLoop 为 PluginCtx 成员随实例生死; 宿主保证 destroy 回调前
 * 本实例全部 op 已终结 (inflight==0) → 析构时 io 上无在途工作协程。防御性兜底:
 * 工作协程帧持有 io 的 shared_ptr, 即使宿主提前销毁 ctx 也不会悬垂。
 *
 * 【内存约定】与手写路径一致: 结果/错误字符串必须经 host->vtable->strdup
 * 分配 (所有权移交宿主, 由其 free)。
 */
#ifndef AGENTXX_PLUGIN_POLL_LOOP_H
#define AGENTXX_PLUGIN_POLL_LOOP_H

#include "agentxx/plugin/plugin_api.h"

#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"

#include <cstring>
#include <expected>
#include <memory>
#include <stdlib.h>
#include <string>
#include <utility>

/// asio 命名空间别名 (项目约定: 各头自带, 与 lib/其他插件头重复定义无冲突 ——
/// 同名同目标别名允许重复声明)
namespace asio = ::boost::asio;

/* =====================================================================
 * 寄生事件循环 (每实例一个, 典型为 PluginCtx 成员)
 * ===================================================================== */

/// 无就绪事件时的建议轮询粒度 ms (定时器唤醒精度上界)。每次 pollOnce 是
/// 非阻塞 epoll_wait(0)/IOCP 取包 (微秒级), 该值只影响挂起协程的唤醒延迟,
/// 不产生忙等; 默认 15ms 对工具级操作 (秒级) 足够精细。
#ifndef AGENTXX_POLL_IDLE_HINT_MS
#define AGENTXX_POLL_IDLE_HINT_MS 15
#endif

namespace agentxx::plugin {

struct PollLoop {
    /// 插件自有事件循环对象 (无线程驱动; 由宿主 io 线程经 pollOnce 步进)。
    /// shared_ptr 供工作协程帧捕获 —— 即使 ctx 提前析构也不会悬垂。
    std::shared_ptr<asio::io_context> io         = std::make_shared<asio::io_context>();
    int                               idleHintMs = AGENTXX_POLL_IDLE_HINT_MS;

    /// 推进一步 (宿主 io 线程调用; 非阻塞):
    /// - 返回 0  = 本轮执行过 handler (建议宿主 post 让出后再问 —— 与其他
    ///   会话协程公平交错; 不要连续滥用, 见文件头约束)
    /// - 返回 >=1 = 无就绪事件 (建议小睡 N ms 再来; 内部定时器到期由该次
    ///   poll 触发恢复)
    int pollOnce() {
        const size_t n = io->poll(); ///< asio: 非阻塞执行全部就绪 handler
        return n > 0 ? 0 : (idleHintMs > 0 ? idleHintMs : 1);
    }
};

/* =====================================================================
 * 单次操作上下文与结果
 * ===================================================================== */

/// 工作协程结果 (三态映射 OP_OK / OP_FAILED / OP_CANCELLED)
struct PolledOutcome {
    enum class Kind {
        Ok,        ///< payload = 结果 JSON (移交宿主堆)
        Failed,    ///< payload = 错误信息 (移交宿主堆)
        Cancelled, ///< 已取消 (payload 忽略)
    };
    Kind        kind    = Kind::Ok;
    std::string payload{};

    static PolledOutcome ok(std::string resultJson) {
        return {Kind::Ok, std::move(resultJson)};
    }
    static PolledOutcome fail(std::string errorMsg) {
        return {Kind::Failed, std::move(errorMsg)};
    }
    static PolledOutcome cancelled() {
        return {Kind::Cancelled, {}};
    }
};

/// 单次调用上下文 (start 分配, 终结释放; 参数字符串在 start 内拷贝 ——
/// 视图借用仅覆盖本次调用)
struct PolledJob {
    AgentxxOpNotify    notify{};
    const AgentxxHost* host       = nullptr;
    void*              userData   = nullptr; ///< shim.userData (通常为 PluginCtx*)
    char*              args       = nullptr;
    size_t             argsSize   = 0;
    char*              tid        = nullptr;
    size_t             tidSize    = 0;
    char*              tcid       = nullptr;
    size_t             tcidSize   = 0;
    volatile int       cancelFlag = 0;

    AgentxxPluginStringView argsView() const {
        return agentxx_plugin_sv(args ? args : "", argsSize);
    }
    AgentxxPluginStringView tidView() const {
        return agentxx_plugin_sv(tid ? tid : "", tidSize);
    }
    AgentxxPluginStringView tcidView() const {
        return agentxx_plugin_sv(tcid ? tcid : "", tcidSize);
    }
};

/// 工作协程签名: 在 PollLoop 的 io_context 上 spawn; 挂起点让出给宿主,
/// 就绪段禁止阻塞 (见文件头); 异常由适配器兜底转 Failed
using PolledWorkFn = asio::awaitable<PolledOutcome> (*)(PolledJob&);

namespace detail {

/// 终结上报 + Job 释放 (工作协程末尾调用; 恰好一次)
inline void polledJobFinish(PolledJob* job, PolledOutcome&& out) {
    int         status  = AGENTXX_OP_OK;
    const char* payload = nullptr;
    switch (out.kind) {
    case PolledOutcome::Kind::Failed:
        status  = AGENTXX_OP_FAILED;
        payload = out.payload.c_str();
        break;
    case PolledOutcome::Kind::Ok:
        payload = out.payload.c_str();
        break;
    case PolledOutcome::Kind::Cancelled:
    default:
        status = AGENTXX_OP_CANCELLED;
        break;
    }
    char* dup = (job->host && job->host->vtable && job->host->vtable->strdup && payload)
                    ? job->host->vtable->strdup(payload)
                    : nullptr;
    if (job->notify.done) {
        job->notify.done(job->notify.host_ud, status, dup);
    }
    free(job->args);
    free(job->tid);
    free(job->tcid);
    delete job;
}

} // namespace detail

/* =====================================================================
 * 三件套适配器 (插件不得直接使用)
 * ===================================================================== */

/// 每工具适配器 (【调用方内嵌存储】: 典型为 PluginCtx 成员/容器, 随实例
/// 生死; 注册成功后其内容被三件套回调引用, 不得复制/移动)
struct PolledToolShim {
    const AgentxxHost* host     = nullptr;
    PollLoop*          loop     = nullptr;
    PolledWorkFn       fn       = nullptr;
    void*              userData = nullptr;
};

template<typename Shim>
void* agentxx_polled_adapter_start(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    const AgentxxOpNotify*  notify,
    char**                  error_out
) {
    auto* shim = static_cast<Shim*>(user_data);
    if (!shim || !shim->host || !shim->loop || !shim->loop->io || !shim->fn || !notify) {
        if (error_out && shim && shim->host && shim->host->vtable) {
            *error_out = shim->host->vtable->strdup("polled tool: shim not initialized");
        }
        return NULL;
    }
    auto* job = new (std::nothrow) PolledJob();
    if (!job) {
        if (error_out) {
            *error_out = shim->host->vtable->strdup("polled tool: out of memory");
        }
        return NULL;
    }
    job->notify   = *notify;
    job->host     = shim->host;
    job->userData = shim->userData;
    job->argsSize = agentxx_plugin_sv_empty(args_json) ? 2 : args_json.size;
    job->tidSize  = thread_id.size;
    job->tcidSize = tool_call_id.size;
    job->args     = (char*)malloc(job->argsSize ? job->argsSize : 1);
    job->tid      = (char*)malloc(job->tidSize ? job->tidSize : 1);
    job->tcid     = (char*)malloc(job->tcidSize ? job->tcidSize : 1);
    if (!job->args || !job->tid || !job->tcid) {
        free(job->args);
        free(job->tid);
        free(job->tcid);
        delete job;
        if (error_out) {
            *error_out = shim->host->vtable->strdup("polled tool: out of memory");
        }
        return NULL;
    }
    if (agentxx_plugin_sv_empty(args_json)) {
        memcpy(job->args, "{}", 2);
    } else {
        memcpy(job->args, args_json.data, args_json.size);
    }
    if (thread_id.size) {
        memcpy(job->tid, thread_id.data, thread_id.size);
    }
    if (tool_call_id.size) {
        memcpy(job->tcid, tool_call_id.data, tool_call_id.size);
    }

    auto ioKeep = shim->loop->io; ///< 协程帧持 io shared_ptr (析构竞态兜底)
    try {
        asio::co_spawn(
            *ioKeep,
            [shim, job, ioKeep]() -> asio::awaitable<void> {
                PolledOutcome out;
                try {
                    out = co_await shim->fn(*job);
                } catch (const std::exception& e) {
                    out = PolledOutcome::fail(e.what());
                } catch (...) {
                    out = PolledOutcome::fail("polled tool: unknown exception");
                }
                /* 终结上报后本协程帧即结束; io 由 ioKeep 保活至帧完全展开 */
                detail::polledJobFinish(job, std::move(out));
            },
            asio::detached
        );
    } catch (...) {
        /* co_spawn 失败 (分配异常等): 未入队 → 直接失败终结 */
        detail::polledJobFinish(job, PolledOutcome::fail("polled tool: spawn failed"));
    }
    return job;
}

template<typename Shim>
int agentxx_polled_adapter_poll(void* user_data, void* /*op*/) {
    auto* shim = static_cast<Shim*>(user_data);
    if (!shim || !shim->loop) {
        return AGENTXX_OP_POLL_DONE;
    }
    return shim->loop->pollOnce();
}

template<typename Shim>
void agentxx_polled_adapter_cancel(void* /*user_data*/, void* op) {
    auto* job = static_cast<PolledJob*>(op);
    if (job) {
        job->cancelFlag = 1; ///< 工作协程在阶段边界轮询到后协作退出
    }
}

/// 注册 poll 寄生驱动型工具 (统一异步操作模型第三姿势; 返回 0 成功)。
/// 【多实例契约】shim 为调用方提供的持久存储 (典型: PluginCtx 成员), 随
/// 实例销毁一并释放; 注册成功前其内容不得复制移动。
///
/// @param loop      本实例寄生事件循环 (通常为 ctx->pollLoop)
/// @param execute   工作协程 (在 loop 上 spawn; 禁止阻塞, 见文件头)
/// @param userData  经 PolledJob.userData 透传给工作协程 (通常为 ctx 指针)
/// @param timeoutMs 0 = 不限制
inline int register_polled_tool(
    const AgentxxHost*      host,
    AgentxxPluginStringView name,
    AgentxxPluginStringView description,
    AgentxxPluginStringView parameters_json,
    PollLoop&               loop,
    PolledWorkFn            execute,
    void*                   userData,
    PolledToolShim*         shim,
    long                    timeoutMs = 0,
    int                     flags     = AGENTXX_TOOL_FLAG_NONE
) {
    if (!host || !execute || !shim || name.data == nullptr) {
        return -1;
    }
    const AgentxxToolsIface* tools
        = AGENTXX_QUERY_IFACE(host, AgentxxToolsIface, AGENTXX_IFACE_AGENT_TOOLS);
    if (!tools || !tools->register_tool) {
        return -1;
    }
    shim->host     = host;
    shim->loop     = &loop;
    shim->fn       = execute;
    shim->userData = userData;

    AgentxxToolSpec s;
    memset(&s, 0, sizeof(s));
    s.name               = name;
    s.description        = description;
    s.parameters_json    = parameters_json;
    s.execute_start      = &agentxx_polled_adapter_start<PolledToolShim>;
    s.execute_poll       = &agentxx_polled_adapter_poll<PolledToolShim>;
    s.execute_cancel     = &agentxx_polled_adapter_cancel<PolledToolShim>;
    s.user_data          = shim;
    s.default_timeout_ms = timeoutMs;
    s.flags              = flags;
    return tools->register_tool(host, &s);
}

} // namespace agentxx::plugin

#endif /* AGENTXX_PLUGIN_POLL_LOOP_H */
