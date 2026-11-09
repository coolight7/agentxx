#pragma once

#include "agentxx/event/events.h"
#include "agentxx/event/event_stream.h"
#include "asio/awaitable.hpp"
#include "asio/steady_timer.hpp"
#include "utilxx_base/json.h"
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asio = ::boost::asio;

namespace neograph::graph {
class CancelToken;
} // namespace neograph::graph

namespace agentxx {
namespace util {
class KeyMetrics;
} // namespace util

namespace agent {

class AgentConfig;
class AgentContext;
class AgentHost;
class AsyncWake;

/// 异步任务种类
/// - `Agent`: 启动一个 agent 跑任务 (子代理形态)
/// - `Tool`: 把一次工具调用放到后台跑 (创建者工具表里的同名工具; 结果同样走事件)
enum class AsyncWorkKind : int { Agent = 0, Tool = 1 };

/// 异步任务状态
enum class AsyncWorkState : uint8_t {
    Queued = 0,
    Running,
    Finished,
    Failed,
    Canceled,
};

/// 后台任务的中断 (权限询问/人工中断) 处理策略
/// - `Bubble`: 冒泡到创建者会话 (创建者已销毁时按拒绝)
/// - `Deny`:   一律拒绝
/// - `Allow`:  一律允许 (人工中断用空结果 resume)
enum class AsyncInterruptPolicy : uint8_t { Bubble = 0, Deny = 1, Allow = 2 };

/// 状态/种类/策略的稳定文本 (`queued`/`running`/... 用于 JSON 与日志)
std::string_view toString(AsyncWorkState state) noexcept;
std::string_view toString(AsyncWorkKind kind) noexcept;
std::string_view toString(AsyncInterruptPolicy policy) noexcept;

/// 中断策略解析 (`bubble`/`deny`/`allow`); 无法识别时返回 false
bool parseInterruptPolicy(std::string_view text, AsyncInterruptPolicy* out) noexcept;

/// 事件阶段 (提交方回调用; 与插件接口表的取值一一对应)
/// - 队列从排队到起跑才发 `Started`; 终态只有 `Finished` 或 `Canceled` 之一
/// - 回调都在容器 io 线程上、同一任务串行触发
enum class AsyncWorkEventPhase : uint8_t { Queued = 0, Started = 1, Finished = 2, Canceled = 3 };

/// 提交方的任务事件回调 (`Queued` / `Started` / `Finished` / `Canceled`)
/// - `payload` 带 `asyncId` / `kind` / `name` / `queueAhead` / `hasError` / `canceled` / `error`
/// - **在容器 io 线程调用, 必须快速返回**: 不得同步阻塞 (整个进程的会话都在这条线程)
using AsyncWorkEventFn = std::function<void(AsyncWorkEventPhase, const utilxx_base::Json&)>;

/// 提交参数: 自包含, 提交后不再回读创建者 (快照在手)
///
/// 派生配置清单默认全空 (= 现状子代理: 只有内置工具), 要什么就列什么。
struct AsyncWorkSpec {
    AsyncWorkKind kind = AsyncWorkKind::Agent;

    /// 任务 id (`async-<n>`; 由管理器分配, 同时是该任务 agent 的 sessionId)
    /// - 提交方不需要填; 内部使用
    std::string asyncId;

    /// 归属账本 (由提交入口填; C++ 调用方可留空 = 不使用创建者上下文)
    std::string creatorAgentId;
    /// 创建者会话: 结果事件与 HIL 的去处; 可空
    std::string creatorSessionId;
    /// 排队与额度的分组键; 空 = creatorAgentId
    std::string queueKey;

    /// 归属标记 (日志/诊断用; 插件提交时填 `plugin:<实例名>`)
    /// - 插件禁用/卸载按它整批取消 (`AgentHost::cancelAsyncWorksOfOwner`)
    /// - 模型工具提交的任务为空 (归属看 creatorAgentId / creatorSessionId)
    std::string owner;

    /// 身份与任务
    std::string         name = "async";
    std::string         systemPrompt;
    std::string         message;
    utilxx_base::Json   messages; ///< 结构化上下文 (数组; 非空时优先于 message)
    /// 同上下文模式: 子代理直接运行在这个 session (内部直调用法, 模型侧不暴露)
    std::string         sameContextSessionId;
    /// 上下文压缩开关 (空 = 继承派生配置)
    std::optional<bool> enableSummarization;
    /// 模型名; 空 = 创建者的 subagent 模型
    std::string                      model;
    std::optional<utilxx_base::Json> tools; ///< 空=默认 / []=无 / ["*"]=继承创建者 / 名单

    /// 显式挂载清单 (空 = 不挂, 与现状子代理一致)
    std::optional<utilxx_base::Json> plugins; ///< 与 yaml `plugin.mount` 同构
    std::vector<std::string>         mcpServers;
    std::vector<std::string>         skills;
    std::vector<std::string>         memoryFiles;
    std::vector<std::string>         ragDocs;
    std::string                      workDir;  ///< 空 = 跟随创建者
    std::string                      language; ///< 空 = 跟随创建者
    std::vector<std::string>         permissionAllowPaths;
    std::vector<std::string>         permissionDenyPaths;

    /// 会话持久化: 目录 = {创建者会话目录}/agent/{asyncId}, sessionId = asyncId
    /// - 空 = 跟随创建者配置 `async_work.session_store`
    /// - 开启时任务自己的会话落盘 (事后可回看; 不做重启后继续跑)
    std::optional<bool> sessionStore;
    std::string         sessionStoreDir; ///< 空 = {创建者会话目录}/agent
    /// 由提交入口算出的存储根 (内部使用; 也是会话独占的键)
    std::string sessionStoreRoot;

    /// 中断策略与时限
    /// - `中断策略` 为空 = 跟随创建者配置 `async_work.interrupt_policy` (默认 bubble);
    ///   单次提交可直接指定, 覆盖配置 (插件/内部调用用法)
    std::optional<AsyncInterruptPolicy> interruptPolicy;
    std::chrono::milliseconds           interruptTimeout{0};

    /// 排队超时 (毫秒; <= 0 = 不限制)
    /// - 计时起点是**延时结束那一刻** (没延时就是提交时刻): 延时本来就规定
    ///   "什么时候才允许跑", 不该再拿排队超时把它自己耗掉
    /// - 到点还没拿到名额: 任务出队并按 `failed` 收尾 (`error` 写明 queue timeout),
    ///   结果事件照发 (等它的人不会挂到自己的超时)
    std::chrono::milliseconds queueTimeout{0};

    /// 运行超时 (毫秒; <= 0 = 不限制)
    /// - 计时起点是**起跑那一刻**: 排队与延时都不算
    /// - `kind=tool`: 到点立即按 `failed` 收尾 (执行体继续跑, 结果丢弃)
    /// - `kind=agent`: 到点置取消令牌 (引擎在下一个检查点停), 同样按 `failed` 收尾
    std::chrono::milliseconds runTimeout{0};

    /// 起跑延时 (毫秒; <= 0 = 不延时)
    /// - 到点之前任务**一直算排队中** (`queued`), 只是不会被 `pump` 拉起;
    ///   它也不挡后面排队的任务 —— 延时项留在队列里被跳过, 名额让给能跑的
    /// - 剩余延时可被 `AgentHost::startAsyncWorkNow` 提前抹掉 (见
    ///   `agentxx_async_await` 的 `start_now`)
    std::chrono::milliseconds delay{0};

    /// 结果: 事件名固定 `agentxx.agent.async.done`, 此处只决定是否额外投递会话
    /// - 空 = 不投递; `next-step` / `inject` / `next-turn`
    std::string notifyDelivery;

    /// 任务事件回调 (空 = 不回调): 排队/起跑/终态各一次
    AsyncWorkEventFn onEvent;

    /// 同上下文模式与工具执行策略 (与 `events::SubagentBatchItem` 同义)
    bool keepProvidedSystemMessage = false;
    bool inheritRequestTools       = false;
    bool toolExecutionDisabled     = false;

    /// ---- 以下两项只在 `kind == Tool` 时使用 ----
    ///
    /// 要后台执行的工具名, 在**创建者**身上按名字查, 两个来源都算:
    /// - 内置 / 中间件工具 (share_store / 事件发布 / 后台任务族等) 由执行图引擎持有,
    ///   经 `AgentContext::builtinToolIndex` 拿到 (任务期间锁住 `AgentContext::engine`);
    /// - 插件与动态注册的工具在 `AgentContext::toolRegistry` 里。
    ///
    /// 两处都查不到 (或创建者已释放) 时任务直接失败。工具参数原样传给它 —— 与前台调用
    /// 同一套执行语义 (权限已在提交时问过, 结果压缩在事件/台账侧按大小截断)。
    std::string       toolName;
    utilxx_base::Json toolArgs = utilxx_base::Json::object();

    /// 任务结果标识 (回填到父 toolcall 的 tool_call_id; 仅日志与台账用)
    std::string resultId;

    /// 以下三项由提交入口按 `AgentConfig` 填入 (调用方可不管)
    size_t  maxConcurrent  = 3;     ///< 本队列的并发额度
    int64_t lingerMs       = 300000; ///< 结束后台账保留时长 (毫秒)
    size_t  resultMaxBytes = 8192;  ///< 台账结果正文上限 (字节)

    /// 结果事件名
    /// - 空 = 固定名 `agentxx.agent.async.done` (单一事件名, 靠值里的 `asyncId` 区分)
    /// - 内部直调路径可显式指定 (P3 之前的旧路径); 模型侧不暴露
    std::string eventName;

    /// 派生基准配置 (提交入口填: 创建者 `AgentContext::agentConfig` 的强引用)
    /// - 持有它即保证"创建者已销毁也能派生"; 为空时退回创建者上下文取
    std::shared_ptr<AgentConfig> baseConfig;
};

/// 在会话总线上注册固定的权限/中断应答器 (`deny` / `allow` 策略)
///
/// 后台任务不保证有前台界面, 不能把所有询问都冒泡给人 (`bubble`)。不冒泡时
/// 总得有人在会话总线上回答:
/// - 权限询问 (`Topic::Permission`, 权限中间件的 INTERRUPT 分支):
///   `allow` → 一律允许, `deny` → 一律拒绝; 两者都不"记住选择"、不切完全授权
/// - 其他 HIL 中断 (`Topic::Interrupt`, 如询问节点): `allow` 用空结果 (`{}`) 恢复
///   (等价跳过该询问), `deny` 应答 `handled=false` 并带 `__Canceled__` 标记
///   (AgentRunner 据此抛取消，不会 resume)
///
/// 响应器只拿策略拷贝, 不引用外部对象; 总线随子会话/上下文销毁, 不需要注销。
///
/// - `args`:
///     - [bus] 会话总线; 为空或 [policy] 为 `Bubble` 时什么都不做
///     - [policy] `Deny` / `Allow`
void installAsyncInterruptResponders(
    const std::shared_ptr<agentxx::events::EventBus>& bus,
    AsyncInterruptPolicy                              policy
);

/// 提交回执 (队列不限条数, 因此只有"受理/拒绝"两种)
struct AsyncWorkSubmitResult {
    bool           accepted = false;
    std::string    asyncId; ///< `async-<n>`; 同时是该任务 agent 的 sessionId
    AsyncWorkState state   = AsyncWorkState::Queued;
    size_t         queueAhead = 0;
    /// 实际生效的起跑延时 (毫秒; 0 = 不延时)
    int64_t        delayMs = 0;
    std::string    error; ///< accepted=false 时的原因
};

/// 任务条目 (台账: 查询与界面展示用; 不含上下文正文)
struct AsyncWorkEntry {
    std::string    asyncId;
    AsyncWorkKind  kind  = AsyncWorkKind::Agent;
    AsyncWorkState state = AsyncWorkState::Queued;
    std::string    queueKey;
    std::string    creatorAgentId;
    std::string    creatorSessionId;
    /// 归属标记 (插件提交时为 `plugin:<实例名>`; 模型提交为空)
    std::string    owner;
    std::string    agentId; ///< 运行期 registry id (结束后仅用于日志关联)
    std::string    name;
    std::string    model;
    int64_t        createdAtMs  = 0;
    int64_t        startedAtMs  = 0;
    int64_t        finishedAtMs = 0;
    /// 提交时的起跑延时原值 (毫秒; 0 = 不延时; 台账展示用)
    int64_t        delayMs = 0;
    /// 排队超时与运行超时 (毫秒; 0 = 不限制; 台账展示用)
    int64_t        queueTimeoutMs = 0;
    int64_t        runTimeoutMs   = 0;
    std::string    resultText; ///< 截断后的结果正文
    std::string    error;
    bool           hasError  = false;
    bool           canceled  = false;
    bool           delivered = false; ///< 结果是否真的投递到了中枢

    /// ---- 下列为管理器内部字段 (查询与展示不使用) ----

    AsyncWorkSpec                                 spec;
    std::shared_ptr<neograph::graph::CancelToken> token;
    std::weak_ptr<AgentContext>                   creatorCtx;
    /// 创建者的关键指标 (提交时取得; 任务收尾时记账)
    std::shared_ptr<agentxx::util::KeyMetrics> metrics;
    size_t                                        maxConcurrent   = 3;
    int64_t                                       lingerMs        = 300000;
    size_t                                        resultMaxBytes  = 8192;
    /// 台账清扫时刻 (steady 毫秒; 0 = 不必清扫)
    int64_t                                       lingerDeadlineSteadyMs = 0;
    /// 起跑到期时刻 (steady 毫秒; 0 = 不延时; 到期后 `pump` 才会拉起它)
    int64_t                                       delayUntilSteadyMs = 0;
    /// 排队超时到期时刻 (steady 毫秒; 0 = 不限制; 到点仍未起跑按失败收尾)
    int64_t                                       queueDeadlineSteadyMs = 0;
    /// 运行期唤醒器 (起跑时创建; 完成/取消/到期都经它叫醒等待方)
    std::shared_ptr<AsyncWake>                    wake;
};

/// 异步任务管理器 (进程一份, 挂在 [AgentHost] 上)
///
/// 一次任务 = 一次后台执行 (`kind` = agent / tool), 提交即可, 结果统一走事件。
/// 每队列 (默认按创建者) 一条 FIFO + 额度, 队列只在内存 (进程退出即丢)。
///
/// 线程模型: 提交/取消/查询可在任意线程 (内部加锁); 队列推进与任务运行在容器
/// io 线程上。
class AsyncWorkManager {
public:

    explicit AsyncWorkManager(std::weak_ptr<AgentHost> host);
    ~AsyncWorkManager();

    AsyncWorkManager(const AsyncWorkManager&)            = delete;
    AsyncWorkManager& operator=(const AsyncWorkManager&) = delete;

    /// 提交一个后台任务 (任意线程)
    /// - 只做校验 + 入队, 立即返回; 只有"未启用"与"参数非法"会被拒,
    ///   **不会因额度/排队条数被拒**
    AsyncWorkSubmitResult submit(AsyncWorkSpec spec, std::shared_ptr<AgentContext> creator);

    /// 取消单条 (运行中 → 置取消令牌并叫醒等待方; 排队中 → 直接出队并按终态收尾)
    /// - 两种情形都发结果事件 (`agentxx.agent.async.done`), 等它的人不会挂到自己的超时
    bool cancel(std::string_view asyncId);

    /// 抹掉一条排队中任务的剩余延时, 让它在本次 pump 里就能起跑
    /// - 只对 `Queued` 条目有效 (运行中/终态/不存在返回 false)
    /// - 原本就没有延时也返回 true (调用方语义是"让它立刻排队起跑")
    bool startNow(std::string_view asyncId);

    /// 调整一条排队中任务的起跑延时 (毫秒; 0 = 移除延时, 立即排队起跑)
    /// - 只对 `Queued` 条目有效; 排队超时的计时起点跟着延时重算
    /// - `return`: `true` = 已应用; `false` = 不存在或不在排队中
    bool setDelay(std::string_view asyncId, int64_t delayMs);

    /// 按创建者批量取消 (`creator` 为空 = 全部)
    size_t cancelByCreator(const std::shared_ptr<AgentContext>& creator);

    /// 按创建者会话批量取消
    size_t cancelBySession(std::string_view creatorSessionId);

    /// 按归属标记批量取消 (插件禁用/卸载; 只置取消令牌, 终态照常记录)
    size_t cancelByOwner(std::string_view owner);

    /// 单条任务 JSON (不存在返回空对象)
    utilxx_base::Json entryJson(std::string_view asyncId) const;

    /// 任务清单 JSON
    /// - `creatorAgentId` 为空 = 全部
    /// - `includeFinished` 为 false 时只给排队 + 运行中
    utilxx_base::Json listJson(
        std::string_view creatorAgentId,
        std::string_view creatorSessionId = {},
        bool             includeFinished  = true,
        size_t           limit            = 20
    ) const;

    /// 运行中的任务数 / 排队中的任务数 (诊断与测试用)
    size_t runningCount() const;
    size_t queuedCount(std::string_view queueKey = {}) const;

    /// 关停: 取消全部在跑与排队项 (`return` 处理条数)
    size_t shutdown(std::string_view reason);

private:

    /// 调度一条队列 (在容器 io 线程上执行)
    /// - 一次把名额允许的条目都拉起; **跳过延时未到的条目** (它们留在队列里)
    void pump(std::string_view queueKey);

    /// 所有队列各推一次 (延时唤醒定时器到点时用)
    void pumpAllQueues();

    /// 排定下一次超时唤醒 (容器 io 线程; 没有待唤醒项时取消定时器)
    /// - 取"最早的延时到期"与"最早的排队超时"两者的最小值
    void armDeadlineTimer();

    /// 超时唤醒定时器到点 (容器 io 线程)
    void onDeadlineTimerFired();

    /// 当前有排队条目的队列键快照
    std::vector<std::string> queueKeysSnapshot() const;

    /// 台账清扫 (结束超过 linger 的条目); 调用方必须已持锁
    void sweepLingerLocked();

    /// 任务主体: 跑子代理 / 工具, 结果发事件 + 回台账 (容器 io 线程)
    /// - 协程帧持有容器强引用: 容器 (含本管理器) 在任务结束前不会被释放
    asio::awaitable<void> runner(std::shared_ptr<AgentHost> host, std::string entryId);

    /// 任务收尾 (唯一入口): 台账 + 结果事件 + 界面通知 + 提交方回调 + 队列补位
    /// - `runner` (正常/失败/取消)、排队中被取消、排队超时三条路径都走它
    /// - 只有它发 `agentxx.agent.async.done`: 否则等它的人 (await / notify) 会挂到超时
    /// - `releaseClaim`: 释放提交时占用的会话存储根 (排队期终态也要释放)
    void finishEntry(
        std::string_view entryId,
        bool             hasError,
        bool             canceled,
        std::string      content,
        std::string      error,
        int64_t          startedAtMs,
        int64_t          finishedAtMs,
        std::string      agentId,
        bool             releaseClaim
    );

    std::weak_ptr<AgentHost> host_;
    mutable std::mutex       mutex_;
    /// 台账 (asyncId -> 条目)
    std::map<std::string, AsyncWorkEntry, std::less<>> entries_;
    /// 每队列的排队 id (FIFO)
    std::map<std::string, std::vector<std::string>, std::less<>> queues_;
    /// 每队列运行中的条数
    std::map<std::string, size_t, std::less<>> runningPerQueue_;
    uint64_t                                   seq_ = 0;

    /// ---- 以下两项只在容器 io 线程访问 (pump 的调用点都是 post 过来的) ----

    /// 超时唤醒定时器 (共用一条: 到点后按需重排)
    /// - 句柄只持容器弱引用, 不让 host → manager → timer → handler → host 成环
    std::unique_ptr<asio::steady_timer> deadlineTimer_;
    /// 已排定的唤醒时刻 (steady 毫秒; 0 = 未排)
    int64_t                             deadlineTimerAtMs_ = 0;
};

} // namespace agent
} // namespace agentxx
