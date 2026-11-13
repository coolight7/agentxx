#include "agentxx/agent/async_work.h"

#include "agentxx/agent/async_wake.h"
#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/config_static.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/session_input_sink.h"
#include "agentxx/event/hub.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "agentxx/util/observability.h"
#include "agentxx/util/share_store_offload.h"
#include "agentxx/util/tool_args_fix.h"
#include "agentxx/util/tool_result.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "neograph/graph/engine.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>

namespace agentxx {
namespace agent {

void installAsyncInterruptResponders(
    const std::shared_ptr<agentxx::events::EventBus>& bus,
    AsyncInterruptPolicy                              policy
) {
    if (!bus || policy == AsyncInterruptPolicy::Bubble) {
        return;
    }
    const bool allow = policy == AsyncInterruptPolicy::Allow;
    // 权限询问: 按策略直接作答, 不让询问流向前台界面 (后台任务不一定有界面)
    bus->getRR<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
           agentxx::events::Topic::Permission
    )
        .registerServer([allow](const agentxx::events::ReqPermission&, size_t)
                            -> asio::awaitable<agentxx::events::RespPermission> {
            co_return agentxx::events::RespPermission{
                .reason   = allow ? "async work: interrupt_policy=allow (auto approved)"
                                 : "async work: interrupt_policy=deny (auto denied)",
                .decision = allow ? agentxx::events::RespPermission::Decision::Allow
                                  : agentxx::events::RespPermission::Decision::Deny,
                .remember = false,
                .fullAuth = false,
            };
        });
    // 其他 HIL 中断: allow 用空结果恢复, deny 按取消收尾
    bus->getRR<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
           agentxx::events::Topic::Interrupt
    )
        .registerServer([allow](const agentxx::events::ReqInterrupt&, size_t)
                            -> asio::awaitable<agentxx::events::RespInterrupt> {
            if (allow) {
                co_return agentxx::events::RespInterrupt{.handled = true, .resultJson = "{}"};
            }
            co_return agentxx::events::RespInterrupt{
                .handled    = false,
                .resultJson = R"_({"__Canceled__":true})_",
            };
        });
}

namespace {

/// 清掉过期 (超过 `keepDays` 天未改动) 的任务会话目录
///
/// 目录结构是 `{存储根}/{asyncId}/session.db`, 一个子目录 = 一个后台任务的会话。
/// 清理只在“提交一个带落盘的任务”时做一次 (不另起定时器), 失败只记日志 —— 运维事实
/// 上的保留期, 不影响任务本身。
///
/// - `args`:
///     - [root] 存储根 (`{创建者会话目录}/agent`)
///     - [keepDays] 保留天数; <= 0 由调用方拦住, 此处不做
void purgeExpiredSessionDirs(const std::string& root, size_t keepDays) {
    if (root.empty() || keepDays == 0) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return;
    }
    const auto cutoff = std::filesystem::file_time_type::clock::now()
                        - std::chrono::hours{24 * static_cast<int64_t>(keepDays)};
    size_t removed = 0;
    for (std::filesystem::directory_iterator
             it(root, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         it != end;
         it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code subEc;
        if (!it->is_directory(subEc)) {
            continue;
        }
        const auto mtime = std::filesystem::last_write_time(it->path(), subEc);
        if (subEc || mtime > cutoff) {
            continue;
        }
        std::error_code rmEc;
        std::filesystem::remove_all(it->path(), rmEc);
        if (rmEc) {
            XX_LOGW(
                "[async_work] could not purge expired session dir `{}`: {}",
                it->path().string(),
                rmEc.message()
            );
            continue;
        }
        ++removed;
    }
    if (removed > 0) {
        XX_LOGI(
            "[async_work] purged {} expired session dir(s) older than {} day(s) under `{}`",
            removed,
            keepDays,
            root
        );
    }
}

/// 结果事件名 (单一事件名, 靠值里的 `asyncId` 区分是哪一条任务)
constexpr std::string_view kAsyncDoneEvent = agentxx::events::naming::kAgentAsyncDoneEvent;

/// 结果事件与任务表的归属标记 (`plugin.` 之外的自有标记, 只用于日志/诊断)
constexpr std::string_view kAsyncOwner = agentxx::events::naming::kAgentAsyncNotifyOwner;

/// 等终态票据的分片等待上限 (毫秒)
/// - 取消令牌只有轮询接口 (`is_cancelled`), 分片让取消最多晚这么久被看到;
///   任务真正结束时票据会立刻叫醒, 所以这只是一次检查上限
constexpr int64_t kTicketSliceMs = 100;

/// 任务是否已到终态
bool isTerminalState(AsyncWorkState state) {
    return state == AsyncWorkState::Finished || state == AsyncWorkState::Failed
           || state == AsyncWorkState::Canceled;
}
int64_t systemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()
    )
        .count();
}

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

/// 通知创建者会话的端点: 任务表变了 (提交 / 起跑 / 终态)
///
/// 后台任务队列的状态变化不会自己触发任何下发 —— 界面上看到的「后台任务」段是随
/// 上下文统计来的, 只有端点 (SessionServerAgentIO) 能推。端点实现了
/// [SessionInputSink::onAsyncWorksChanged] 就会重发一次快照, 界面随之更新。
///
/// - 投递到容器 io 线程执行: `SessionsManager` 与 `Session::inputSink` 都只在该线程
///   访问 (调用方可能在任意线程: 模型工具 / 插件线程 / 取消路径)
/// - 会话或端点不在了 (创建者已销毁 / 未绑定端点) 时静默跳过
///
/// - `args`:
///     - [host]      容器 (弱引用由调用方给, 可能在投递前被释放)
///     - [creator]   任务创建者的上下文 (弱引用, 同上)
///     - [sessionId] 创建者会话 id (任务表里登记的那个)
void postAsyncWorksChanged(
    const std::shared_ptr<AgentHost>&    host,
    const std::shared_ptr<AgentContext>& creator,
    const std::string&                   sessionId
) {
    if (!host || !creator || sessionId.empty()) {
        return;
    }
    asio::post(
        host->ioCtx()->get_executor(),
        [weakCreator = std::weak_ptr<AgentContext>{creator}, sessionId]() {
            auto ctx = weakCreator.lock();
            if (!ctx || !ctx->sessions) {
                return;
            }
            auto session = ctx->sessions->get(sessionId);
            if (!session) {
                return;
            }
            if (auto sink = session->inputSink.lock()) {
                sink->onAsyncWorksChanged();
            }
        }
    );
}

/// 按字节上限截断 (退到 UTF-8 字符边界, 避免留下半个字符)
std::string truncateUtf8(std::string_view text, size_t maxBytes) {
    if (maxBytes == 0 || text.size() <= maxBytes) {
        return std::string{text};
    }
    size_t cut = maxBytes;
    // 回退到字符首字节之后 (续字节不能作为切点)
    while (cut > 0 && cut < text.size()
           && utilxx_base::utf8IsContinuationChar(static_cast<unsigned char>(text[cut]))) {
        --cut;
    }
    std::string out{text.substr(0, cut)};
    out += fmt::format("...(truncated, {} bytes total)", text.size());
    return out;
}

/// 触发提交方的任务事件回调
/// - 回调属外部代码 (插件): 抛异常只记日志, 不影响任务状态与结果投递
/// - 调用点都在容器 io 线程 (排队那条经 post 回到同一线程, 与后续阶段保序)
void fireAsyncWorkEvent(
    const AsyncWorkEventFn&   cb,
    AsyncWorkEventPhase       phase,
    const utilxx_base::Json& payload
) {
    if (!cb) {
        return;
    }
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            cb(phase, payload);
            return true;
        },
        [phase](std::string errinfo) -> bool {
            XX_LOGW(
                "[async_work] on_event callback threw (phase={}): {}",
                static_cast<int>(phase),
                errinfo
            );
            return false;
        }
    );
}

/// 从 spec 构造派生任务 (身份/工具/前缀字段)
events::SubagentBatchItem taskOfSpec(const AsyncWorkSpec& spec) {
    events::SubagentBatchItem task;
    task.subagentName             = spec.name;
    task.systemPrompt             = spec.systemPrompt;
    task.message                  = spec.message;
    if (spec.messages.is_array()) {
        task.messages = spec.messages;
    }
    task.sessionId                 = spec.sameContextSessionId;
    if (spec.tools.has_value()) {
        task.tools = spec.tools;
    }
    task.enableSummarization       = spec.enableSummarization;
    task.keepProvidedSystemMessage = spec.keepProvidedSystemMessage;
    task.inheritRequestTools       = spec.inheritRequestTools;
    task.toolExecutionDisabled     = spec.toolExecutionDisabled;
    task.resultId                  = spec.resultId;
    return task;
}

/// 后台工具任务的执行体解析结果
struct ToolTarget {
    /// 执行体 (只借; 寿命由下面两个 shared_ptr 之一保证)
    neograph::Tool* tool = nullptr;
    /// 动态注册表里的工具 (插件/动态注册): 由注册表 weak 共享持有
    std::shared_ptr<agentxx::tools::XXToolBase> keepAlive;
    /// 内置工具: 工具对象归执行图引擎所有, 任务期间锁住引擎即保证不悬垂
    std::shared_ptr<neograph::graph::GraphEngine> engine;
};

/// 在创建者身上按名字找出要后台执行的工具 (与前台 ToolcallWrapNode 同顺序)
///
/// - 内置 / 中间件工具 (share_store / 事件发布 / 后台任务族 / subagent) 由执行图
///   引擎持有, 经 `AgentContext::builtinToolIndex` 拿到; 引擎弱引用锁不上时说明
///   创建者已释放, 不能去解引用索引里的旧指针
/// - 插件 / 动态注册的工具在 `AgentContext::toolRegistry` 里, 以 shared_ptr 保活
///   (插件卸载会等 inflight 归零, 执行期间代码段保持有效)
///
/// - `return` 找到为 true; false 时 [error] 已写明原因
bool resolveCreatorTool(
    const std::shared_ptr<AgentContext>& creator,
    std::string_view                     toolName,
    ToolTarget&                          out,
    std::string&                         error
) {
    out = ToolTarget{};
    if (!creator) {
        error = "async tool: creator context is gone";
        return false;
    }
    if (auto engine = creator->engine.lock()) {
        if (auto it = creator->builtinToolIndex.find(toolName);
            it != creator->builtinToolIndex.end() && it->second != nullptr) {
            out.tool   = it->second;
            out.engine = std::move(engine);
            return true;
        }
    }
    if (creator->toolRegistry) {
        if (auto registered = creator->toolRegistry->find(toolName)) {
            out.tool      = registered.get();
            out.keepAlive = std::move(registered);
            return true;
        }
    }
    error = fmt::format(
        "async tool: tool `{}` not found for the creator (not a built-in tool of this agent, "
        "not in its dynamic tool registry, or the agent was released)",
        toolName
    );
    return false;
}

/// 跑一个工具任务 (`kind == Tool`): 在创建者的工具表里执行同名工具
///
/// 取消是"尽力而为": 等待靠 [AsyncWake] 通知 (完成 / 取消 / 运行超时三者任一都会叫醒),
/// 置位后立即按取消收尾 (工具本身如果不认识取消, 它会在后台继续跑到自己的超时, 结果被
/// 丢弃); 协程状态用 shared_ptr 持有, 不会因为本函数提前退出而悬挂。
asio::awaitable<events::RespSubagentBatchItem> runToolWork(
    const std::shared_ptr<AgentContext>&          creator,
    const AsyncWorkSpec&                          spec,
    const std::shared_ptr<neograph::graph::CancelToken>& token,
    const std::shared_ptr<AsyncWake>&              wake
) {
    auto fail = [&spec](std::string message, bool canceled = false) {
        return events::RespSubagentBatchItem{
            .resultId     = spec.resultId,
            .hasError     = true,
            .Canceled     = canceled,
            .errorMessage = std::move(message),
        };
    };
    ToolTarget  target;
    std::string lookupError;
    if (!resolveCreatorTool(creator, spec.toolName, target, lookupError)) {
        co_return fail(std::move(lookupError));
    }
    // 取消已经开始就不必启动执行体
    if (token && token->is_cancelled()) {
        co_return fail("async tool canceled", /*canceled=*/true);
    }

    struct RunState {
        std::atomic<bool>        done{false};
        agentxx::util::ToolResult result;
    };
    auto state = std::make_shared<RunState>();
    auto args  = spec.toolArgs; // 拷一份进协程帧: 提交方的对象可能早就不在了    // 参数类型自动修正: 与前台 ToolcallWrapNode 执行前同一份实现 (内置工具与插件工具
    // 一视同仁), 后台跑的工具同样按自己声明的 schema 拿到正确类型
    try {
        const auto parameters
            = agentxx::util::fromNeographJson(target.tool->get_definition().parameters);
        agentxx::util::autoFixToolArgsTypes(parameters, args, spec.toolName);
    } catch (const std::exception& e) {
        XX_LOGW("Async tool auto-fix arg type failed: {}", e.what());
    }
    // 会话与调用 id 由执行层补齐 (与前台 ToolcallWrapNode 一致): 下游工具会读它们
    if (!spec.creatorSessionId.empty() && args.is_object()) {
        args["sessionId"] = spec.creatorSessionId;
        if (!spec.resultId.empty()) {
            args["tool_call_id"] = spec.resultId;
        }
    }
    // 工具的行为开关 (autoSummaryOutput 等) 拷一份出来: `target` 随后被移交给后台
    // 协程, 而结果压缩要在等待结束后用它判断"这个工具的输出要不要转 share store"
    const std::map<std::string, std::string> toolExtra = target.tool->extra;
    const auto ex = co_await asio::this_coro::executor;
    // 必须把**可调用对象**交给 co_spawn (不要在这里内联调用成 `{...}()`):
    // lambda 协程的帧只保存闭包指针, 内联调用产生的是临时闭包 —— 语句结束即析构,
    // 而这具协程要在本函数返回后继续跑 (取消时本函数先退出, 工具仍在后台跑到自己的
    // 超时)。传可调用对象时闭包由 co_spawn 复制进自己的协程帧, 生命周期与任务一致,
    // 后台协程访问 state/target/args 才不悬垂 (同 [TaskScope::spawn] 的约定)
    asio::co_spawn(
        ex,
        [state, wake, target = std::move(target), args = std::move(args)]() -> asio::awaitable<void> {
            state->result = co_await agentxx::util::catchErrorAsync<agentxx::util::ToolResult>(
                [&]() -> asio::awaitable<agentxx::util::ToolResult> {
                    if (auto* xxTool = dynamic_cast<agentxx::tools::XXToolBase*>(target.tool)) {
                        co_return co_await xxTool->execute_async(args);
                    }
                    // 原生 neograph::Tool (如未包装的 MCP 原样转发工具): 经桥接调用,
                    // 原生工具没有状态通道, 文本一律按成功
                    auto neoArgs = agentxx::util::toNeographJson(args);
                    co_return agentxx::util::ToolResult::ok(
                        co_await target.tool->execute_async(neoArgs)
                    );
                },
                [](std::string errmsg) -> asio::awaitable<agentxx::util::ToolResult> {
                    co_return agentxx::util::ToolResult::fail(std::move(errmsg));
                }
            );
            state->done.store(true, std::memory_order_release);
            // 完成时叫醒等待方 (与取消/到期走同一个唤醒器)
            if (wake) {
                wake->signal();
            }
            co_return;
        },
        asio::detached
    );

    // 等完成 / 等取消 / 等到期: 不再轮询 —— 三者任一发生都会把等待叫醒
    // (`spec.runTimeout` 为 0 时不设到期, 只等完成或取消)
    const int64_t runTimeoutMs = std::max<int64_t>(0, spec.runTimeout.count());
    const int64_t deadline
        = runTimeoutMs > 0 ? steadyNowMs() + runTimeoutMs : 0;
    while (!state->done.load(std::memory_order_acquire)) {
        if (token && token->is_cancelled()) {
            co_return fail("async tool canceled", /*canceled=*/true);
        }
        int64_t waitMs = 0; // 0 = 不设到期
        if (deadline != 0) {
            const auto now = steadyNowMs();
            if (now >= deadline) {
                co_return fail(fmt::format(
                    "run timeout: still running after {} ms (async_work run limit); the tool "
                    "keeps running in the background and its result is discarded",
                    runTimeoutMs
                ));
            }
            waitMs = deadline - now;
        }
        if (!wake) {
            co_return fail("async tool: wake handle is missing");
        }
        co_await wake->wait(std::chrono::milliseconds{waitMs});
    }

    // 工具输出压缩: 与前台工具调用同一份实现/同一套阈值 (工具的 autoSummaryOutput
    // 声明说了算), 因此它的结果正文进事件投递时已经裁剪过 —— 事件侧不再整段卸载
    agentxx::util::ToolResult result = state->result;
    agentxx::util::offloadToolResultIfOversized(
        creator,
        toolExtra,
        spec.creatorSessionId,
        result
    );
    events::RespSubagentBatchItem out;
    out.resultId = spec.resultId;
    switch (result.status) {
        case agentxx::util::ToolStatus::Ok:
            out.content = result.text;
            break;
        case agentxx::util::ToolStatus::Canceled:
            out.hasError     = true;
            out.Canceled     = true;
            out.errorMessage = result.text.empty() ? std::string{"async tool canceled"} : result.text;
            break;
        case agentxx::util::ToolStatus::Interrupted:
        case agentxx::util::ToolStatus::Failed:
        default:
            out.hasError     = true;
            out.errorMessage = result.text.empty() ? std::string{"async tool failed"} : result.text;
            break;
    }
    co_return out;
}

} // namespace

// ===========================================================================
// 枚举文本
// ===========================================================================

std::string_view toString(AsyncWorkState state) noexcept {
    switch (state) {
        case AsyncWorkState::Queued:
            return "queued";
        case AsyncWorkState::Running:
            return "running";
        case AsyncWorkState::Finished:
            return "done";
        case AsyncWorkState::Failed:
            return "failed";
        case AsyncWorkState::Canceled:
            return "canceled";
    }
    return "unknown";
}

std::string_view toString(AsyncWorkKind kind) noexcept {
    switch (kind) {
        case AsyncWorkKind::Agent:
            return "agent";
        case AsyncWorkKind::Tool:
            return "tool";
    }
    return "unknown";
}

std::string_view toString(AsyncInterruptPolicy policy) noexcept {
    switch (policy) {
        case AsyncInterruptPolicy::Bubble:
            return "bubble";
        case AsyncInterruptPolicy::Deny:
            return "deny";
        case AsyncInterruptPolicy::Allow:
            return "allow";
    }
    return "bubble";
}

bool parseInterruptPolicy(std::string_view text, AsyncInterruptPolicy* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    if (text.empty() || text == "bubble") {
        *out = AsyncInterruptPolicy::Bubble;
        return true;
    }
    if (text == "deny") {
        *out = AsyncInterruptPolicy::Deny;
        return true;
    }
    if (text == "allow") {
        *out = AsyncInterruptPolicy::Allow;
        return true;
    }
    return false;
}

// ===========================================================================
// AsyncWorkManager
// ===========================================================================

AsyncWorkManager::AsyncWorkManager(std::weak_ptr<AgentHost> host) :
    host_(std::move(host)) {}

AsyncWorkManager::~AsyncWorkManager() {
    std::map<std::string, AsyncWorkEntry, std::less<>> entries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries.swap(entries_);
        queues_.clear();
        runningPerQueue_.clear();
    }
    // 只置取消令牌 (任务收尾时照常记账), 不在这里等它们结束: 析构发生在
    // AgentHost 释放期间, 容器 io 线程可能已经停了
    size_t canceled = 0;
    for (auto& [id, entry] : entries) {
        if (entry.token && !entry.token->is_cancelled()) {
            entry.token->cancel();
            ++canceled;
        }
    }
    if (canceled > 0) {
        XX_LOGI("[async_work] manager destroyed, cancel {} unfinished task(s)", canceled);
    }
}

void AsyncWorkManager::sweepLingerLocked() {
    const auto               now = steadyNowMs();
    std::vector<std::string> expired;
    for (const auto& [id, entry] : entries_) {
        if (entry.lingerDeadlineSteadyMs != 0 && entry.lingerDeadlineSteadyMs <= now) {
            expired.push_back(id);
        }
    }
    for (const auto& id : expired) {
        entries_.erase(id);
    }
}

AsyncWorkSubmitResult
    AsyncWorkManager::submit(AsyncWorkSpec spec, std::shared_ptr<AgentContext> creator) {
    AsyncWorkSubmitResult out;
    if (spec.kind == AsyncWorkKind::Tool) {
        // 异步工具: 在**创建者**的工具表里执行, 所以必须有上下文 (工具表在它身上)
        if (spec.toolName.empty()) {
            out.error = "tool name is empty (kind `tool` needs `toolName`)";
            return out;
        }
        if (!creator) {
            out.error = "kind `tool` needs the creator context (tool registry)";
            return out;
        }
    } else if (spec.message.empty() && !spec.messages.is_array()) {
        out.error = "message is empty (need `message` or `messages`)";
        return out;
    }
    auto host = host_.lock();
    if (!host) {
        out.error = "agent host is gone";
        return out;
    }
    auto metrics = creator ? creator->metrics : nullptr;

    // ---- 自包含快照: 配置强引用与配额一次带走, 运行期不再回读创建者 ----
    if (!spec.baseConfig && creator && creator->agentConfig) {
        spec.baseConfig = creator->agentConfig;
    }
    if (spec.baseConfig) {
        spec.maxConcurrent = std::max<size_t>(1, spec.baseConfig->asyncWorkMaxConcurrent);
        spec.lingerMs      = spec.baseConfig->asyncWorkLingerMs;
        spec.resultMaxBytes
            = std::max<size_t>(1, spec.baseConfig->asyncWorkResultMaxBytes);
    }
    if (spec.resultMaxBytes == 0) {
        spec.resultMaxBytes = 8192;
    }
    if (spec.lingerMs < 0) {
        spec.lingerMs = 300000;
    }
    if (spec.queueKey.empty()) {
        spec.queueKey = spec.creatorAgentId.empty() ? "root" : spec.creatorAgentId;
    }
    // 中断策略: 未指定时跟随创建者配置 (配置里的取值已在加载期校验过, 认不出用默认)
    if (!spec.interruptPolicy.has_value()) {
        AsyncInterruptPolicy policy = AsyncInterruptPolicy::Bubble;
        if (spec.baseConfig
            && !parseInterruptPolicy(spec.baseConfig->asyncWorkInterruptPolicy, &policy)) {
            policy = AsyncInterruptPolicy::Bubble;
        }
        spec.interruptPolicy = policy;
    }

    // 会话持久化 (可选): 未指定时跟随配置; 开启时算出存储根
    // - 目录 = {创建者会话目录}/agent, 会话 id = asyncId
    //   → 库文件 `{存储根}/{asyncId}/session.db`
    if (!spec.sessionStore.has_value()) {
        spec.sessionStore = spec.baseConfig ? spec.baseConfig->asyncWorkSessionStore : false;
    }
    if (*spec.sessionStore) {
        std::string root = spec.sessionStoreDir;
        if (root.empty() && spec.baseConfig && !spec.creatorSessionId.empty()) {
            std::string creatorRoot = spec.baseConfig->sessionStoreDirectory;
            if (creatorRoot.empty() && !spec.baseConfig->dataDir.empty()) {
                // 未配 dataDir = 不落盘 (与 agent 自身的会话存储开关同规则)
                creatorRoot = AgentConfigStatic::getSessionsDir(spec.baseConfig->dataDir);
            }
            if (!creatorRoot.empty()) {
                root = (std::filesystem::path(creatorRoot) / spec.creatorSessionId / "agent")
                           .string();
            }
        }
        if (root.empty()) {
            out.error = "session_store requested but no store directory "
                        "(dataDir/sessionStoreDirectory empty)";
            return out;
        }
        spec.sessionStoreRoot = std::move(root);
        // 保留天数 (`session_store_keep_days`, 0 = 不自动清理): 顺手清掉过期的
        // 任务会话目录 (只在提交带落盘的任务时做一次, 不另起定时器)
        if (spec.baseConfig && spec.baseConfig->asyncWorkSessionStoreKeepDays > 0) {
            purgeExpiredSessionDirs(
                spec.sessionStoreRoot,
                spec.baseConfig->asyncWorkSessionStoreKeepDays
            );
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        sweepLingerLocked();

        AsyncWorkEntry entry;
        entry.asyncId          = fmt::format("async-{}", ++seq_);
        entry.kind             = spec.kind;
        entry.state            = AsyncWorkState::Queued;
        entry.queueKey         = spec.queueKey;
        entry.creatorAgentId   = spec.creatorAgentId;
        entry.creatorSessionId = spec.creatorSessionId;
        entry.owner            = spec.owner;
        entry.name             = spec.name;
        entry.model            = spec.model;
        entry.createdAtMs      = systemNowMs();
        entry.maxConcurrent    = std::max<size_t>(1, spec.maxConcurrent);
        entry.lingerMs         = spec.lingerMs;
        entry.resultMaxBytes   = spec.resultMaxBytes;
        // 起跑延时: 到点之前一直是排队中 (pump 跳过它)
        const auto delayMs       = std::max<int64_t>(0, spec.delay.count());
        entry.delayMs            = delayMs;
        entry.delayUntilSteadyMs = delayMs > 0 ? steadyNowMs() + delayMs : 0;
        // 排队超时 / 运行超时: 计时起点分别是"延时结束那一刻"与"起跑那一刻"
        entry.queueTimeoutMs = std::max<int64_t>(0, spec.queueTimeout.count());
        entry.runTimeoutMs   = std::max<int64_t>(0, spec.runTimeout.count());
        entry.queueDeadlineSteadyMs
            = entry.queueTimeoutMs > 0
                  ? (entry.delayUntilSteadyMs > 0 ? entry.delayUntilSteadyMs : steadyNowMs())
                        + entry.queueTimeoutMs
                  : 0;
        entry.token            = std::make_shared<neograph::graph::CancelToken>();
        entry.creatorCtx       = creator;
        entry.metrics          = creator ? creator->metrics : nullptr;
        spec.asyncId           = entry.asyncId;
        entry.spec             = spec;

        // 会话落盘: 同一存储根 + 会话 id 只能有一个驱动者 (撞上就拒绝, 早失败)
        if (!spec.sessionStoreRoot.empty()
            && !host->claimSession(
                spec.sessionStoreRoot,
                entry.asyncId,
                fmt::format("async_work:{}", entry.asyncId)
            )) {
            out.error = fmt::format(
                "session store `{}` is busy with session `{}`",
                spec.sessionStoreRoot,
                entry.asyncId
            );
            return out;
        }

        out.accepted   = true;
        out.asyncId    = entry.asyncId;
        out.state      = AsyncWorkState::Queued;
        out.queueAhead = queues_[spec.queueKey].size();
        out.delayMs    = delayMs;

        queues_[spec.queueKey].push_back(entry.asyncId);
        entries_.insert_or_assign(entry.asyncId, std::move(entry));

        // 指标: 受理 + 当前排队数峰值
        if (metrics) {
            metrics->noteAsyncWorkSubmitted(queues_[spec.queueKey].size());
        }
    }

    XX_LOGI(
        "[async_work] submitted {} ({}) queue={} creator={} session={} ahead={} delay={}ms",
        out.asyncId,
        toString(spec.kind),
        spec.queueKey,
        spec.creatorAgentId,
        spec.creatorSessionId,
        out.queueAhead,
        out.delayMs
    );

    // 提交方回调: 受理 (排到位) —— 投递到容器 io 线程, 与后续 Started/Finished 保序
    if (spec.onEvent) {
        auto            cb      = spec.onEvent;
        auto            payload = utilxx_base::Json{
            {"asyncId", out.asyncId  },
            {"kind",    toString(spec.kind)},
            {"name",    spec.name    },
            {"queueAhead", out.queueAhead},
            {"delayMs", out.delayMs  },
        };
        const auto executor = host->ioCtx()->get_executor();
        asio::post(
            executor,
            [cb = std::move(cb), payload = std::move(payload)]() {
                fireAsyncWorkEvent(cb, AsyncWorkEventPhase::Queued, payload);
            }
        );
    }

    // 结果回到会话 (`notifyDelivery`): 不做任何登记 —— 任务收尾时直接交给
    // 会话端点 (见 [finishEntry]); 若那时有 await 在等, 正文交给等待方, 不再重复投递

    // 推进队列 (容器 io 线程); 提交方不等待
    // - 投递体持有容器强引用: 容器 (含本管理器) 在任务排空之前不会被释放, `this` 始终有效
    asio::post(host->ioCtx()->get_executor(), [host, key = spec.queueKey]() {
        host->asyncWork_->pump(key);
    });

    // 界面: 新任务入队 (排队数变了, 同一队列里其他人的"前面还有几个"也变了)
    postAsyncWorksChanged(host, creator, spec.creatorSessionId);
    return out;
}

void AsyncWorkManager::pump(std::string_view queueKey) {
    auto host = host_.lock();
    if (!host) {
        return;
    }
    /// 本趟要起跑的条目 (锁内只收集, 锁外再触发回调与建协程)
    struct Launch {
        std::string                   id;
        AsyncWorkEventFn              cb;
        utilxx_base::Json             payload;
        std::shared_ptr<AgentContext> creator;
        std::string                   sessionId;
    };
    std::vector<Launch>      launches;
    /// 排队超时到期的条目 (锁外走 finishEntry: 它会再持锁并重写任务表)
    std::vector<std::string> queueTimeouts;
    std::vector<int64_t>     queueTimeoutLimits;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        qit = queues_.find(queueKey);
        if (qit == queues_.end()) {
            return;
        }
        auto&      queue   = qit->second;
        const auto now     = steadyNowMs();
        auto       rit     = runningPerQueue_.find(queueKey);
        size_t     running = rit == runningPerQueue_.end() ? 0 : rit->second;
        size_t     index   = 0;
        while (index < queue.size()) {
            const std::string& queuedId = queue[index];
            auto               eit      = entries_.find(queuedId);
            if (eit == entries_.end() || eit->second.state != AsyncWorkState::Queued) {
                // 任务表已回收 / 已被别的路径置终态: 跳过
                queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            auto& entry = eit->second;
            if (entry.queueDeadlineSteadyMs != 0 && now >= entry.queueDeadlineSteadyMs) {
                // 排队超时: 移出队列, 锁外按失败收尾 (含结果事件)
                queueTimeouts.push_back(queuedId);
                queueTimeoutLimits.push_back(entry.queueTimeoutMs);
                queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            if (entry.delayUntilSteadyMs > now) {
                // 延时未到: 留在队列里等唤醒, 名额让给后面能跑的
                ++index;
                continue;
            }
            const size_t quota = std::max<size_t>(1, entry.maxConcurrent);
            if (running >= quota) {
                break; // 队列满额: 等某个任务收尾时再推
            }
            Launch launch;
            launch.id        = queuedId;
            launch.cb        = entry.spec.onEvent;
            launch.payload   = utilxx_base::Json{
                {"asyncId", queuedId          },
                {"kind",    toString(entry.kind)},
                {"name",    entry.name       },
            };
            launch.creator   = entry.creatorCtx.lock();
            launch.sessionId = entry.creatorSessionId;
            launches.push_back(std::move(launch));

            ++running;
            entry.state             = AsyncWorkState::Running;
            entry.startedAtMs       = systemNowMs();
            entry.delayUntilSteadyMs = 0;
            queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
        }
        if (running == 0) {
            runningPerQueue_.erase(std::string{queueKey});
        } else {
            runningPerQueue_[std::string{queueKey}] = running;
        }
        auto qit2 = queues_.find(queueKey);
        if (qit2 != queues_.end() && qit2->second.empty()) {
            queues_.erase(qit2);
        }
    }
    for (size_t i = 0; i < queueTimeouts.size(); ++i) {
        const auto& id    = queueTimeouts[i];
        const auto  limit = queueTimeoutLimits[i];
        XX_LOGW("[async_work] {} queue timeout (limit={}ms)", id, limit);
        finishEntry(
            id,
            /*hasError=*/true,
            /*canceled=*/false,
            std::string{},
            fmt::format(
                "queue timeout: still waiting for a queue slot after {} ms (async_work queue "
                "limit)",
                limit
            ),
            /*startedAtMs=*/0,
            systemNowMs(),
            std::string{},
            /*releaseClaim=*/true
        );
    }
    for (auto& launch : launches) {
        // 提交方回调 (起跑) 在锁外触发, 避免外部代码重入引发死锁
        fireAsyncWorkEvent(launch.cb, AsyncWorkEventPhase::Started, launch.payload);
        // 界面: 排队 → 运行 (在跑/排队两边计数都变了)
        postAsyncWorksChanged(host, launch.creator, launch.sessionId);
        XX_LOGD("[async_work] launch {} (queue={})", launch.id, queueKey);
        asio::co_spawn(
            host->ioCtx()->get_executor(),
            [host, id = std::move(launch.id)]() -> asio::awaitable<void> {
                co_await host->asyncWork_->runner(std::move(host), id);
            },
            asio::detached
        );
    }
    // 队列里还留着未到期的条目 (延时 / 排队超时): 排一次唤醒
    armDeadlineTimer();
}

void AsyncWorkManager::pumpAllQueues() {
    for (const auto& key : queueKeysSnapshot()) {
        pump(key);
    }
}

void AsyncWorkManager::armDeadlineTimer() {
    auto host = host_.lock();
    if (!host) {
        return;
    }
    int64_t earliest = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto                  now = steadyNowMs();
        for (const auto& [id, entry] : entries_) {
            (void)id;
            if (entry.state != AsyncWorkState::Queued) {
                continue;
            }
            // 延时到期与排队超时取最早的那个; 已到期的取"现在" (等一次 pump)
            const auto mark = [&earliest, now](int64_t at) {
                if (at == 0) {
                    return;
                }
                const auto when = at > now ? at : now;
                if (earliest == 0 || when < earliest) {
                    earliest = when;
                }
            };
            mark(entry.delayUntilSteadyMs);
            mark(entry.queueDeadlineSteadyMs);
        }
    }
    if (earliest == 0) {
        // 没有待唤醒的条目: 取消已排定的唤醒
        if (deadlineTimer_) {
            deadlineTimer_->cancel();
        }
        deadlineTimerAtMs_ = 0;
        return;
    }
    if (deadlineTimerAtMs_ != 0 && deadlineTimerAtMs_ <= earliest) {
        return; // 已排定的唤醒不晚于这个时刻, 不必重排
    }
    const auto now = steadyNowMs();
    if (!deadlineTimer_) {
        deadlineTimer_ = std::make_unique<asio::steady_timer>(host->ioCtx()->get_executor());
    }
    deadlineTimerAtMs_ = earliest;
    deadlineTimer_->expires_after(std::chrono::milliseconds{std::max<int64_t>(0, earliest - now)});
    // 只持容器弱引用: 不让 host → manager → timer → handler → host 成环
    deadlineTimer_->async_wait([weakHost = std::weak_ptr<AgentHost>{host}](
                                   const utilxx_base::AsioErrorCode& ec
                               ) {
        if (ec) {
            return; // 被重排或管理器停止取消
        }
        auto host = weakHost.lock();
        if (!host || !host->asyncWork_) {
            return;
        }
        host->asyncWork_->onDeadlineTimerFired();
    });
}

void AsyncWorkManager::onDeadlineTimerFired() {
    deadlineTimerAtMs_ = 0;
    pumpAllQueues();
}

std::vector<std::string> AsyncWorkManager::queueKeysSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string>    keys;
    keys.reserve(queues_.size());
    for (const auto& [key, queue] : queues_) {
        (void)queue;
        keys.push_back(key);
    }
    return keys;
}

bool AsyncWorkManager::startNow(std::string_view asyncId) {
    std::string                   queueKey;
    bool                          queued   = false;
    bool                          changed  = false;
    std::shared_ptr<AgentContext> creator;
    std::string                   sessionId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = entries_.find(asyncId);
        if (it == entries_.end() || it->second.state != AsyncWorkState::Queued) {
            return false;
        }
        auto& entry = it->second;
        queued      = true;
        changed     = entry.delayUntilSteadyMs != 0;
        entry.delayUntilSteadyMs = 0;
        // 剩余延时被抹掉: 排队超时的计时起点重新从"现在"算
        entry.queueDeadlineSteadyMs
            = entry.queueTimeoutMs > 0 ? steadyNowMs() + entry.queueTimeoutMs : 0;
        queueKey                 = entry.queueKey;
        creator                  = entry.creatorCtx.lock();
        sessionId                = entry.creatorSessionId;
    }
    auto host = host_.lock();
    if (!host) {
        return queued;
    }
    if (changed) {
        XX_LOGI("[async_work] {} start now (delay cleared)", asyncId);
    }
    asio::post(host->ioCtx()->get_executor(), [host, key = std::move(queueKey)]() {
        host->asyncWork_->pump(key);
    });
    // 界面: 延时信息没了 / 可能已经从排队变成运行
    postAsyncWorksChanged(host, creator, sessionId);
    return queued;
}

asio::awaitable<void>
    AsyncWorkManager::runner(std::shared_ptr<AgentHost> host, std::string entryId) {
    if (!host) {
        co_return;
    }
    AsyncWorkSpec                                 spec;
    std::shared_ptr<neograph::graph::CancelToken> token;
    std::shared_ptr<AgentContext>                 creator;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = entries_.find(entryId);
        if (it == entries_.end()) {
            co_return;
        }
        spec    = it->second.spec;
        token   = it->second.token;
        creator = it->second.creatorCtx.lock();
    }
    // 完成 / 取消 / 到期都经同一个唤醒器叫醒等待方
    // (工具任务用它取代旧的 50 ms 轮询; 取消请求走 [cancel] → `wake->signal()`)
    auto wake = std::make_shared<AsyncWake>(host->ioCtx()->get_executor());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = entries_.find(entryId); it != entries_.end()) {
            it->second.wake = wake;
        }
    }
    const auto startedAtMs = systemNowMs();

    auto task   = taskOfSpec(spec);
    const bool isToolTask = spec.kind == agentxx::agent::AsyncWorkKind::Tool;

    // 运行超时 (`kind=agent`): 到点置取消令牌 —— 引擎在下一个检查点停下, 收尾按失败报
    // - 工具任务由 `runToolWork` 自己的等待算到点, 这里不必再挂定时器
    // - 处理器只持 shared_ptr 拷贝: 计时器先于任务释放也不会悬垂
    auto             timedOut = std::make_shared<std::atomic<bool>>(false);
    asio::steady_timer runTimer{co_await asio::this_coro::executor};
    const auto       runTimeoutMs = std::max<int64_t>(0, spec.runTimeout.count());
    if (!isToolTask && runTimeoutMs > 0) {
        runTimer.expires_after(std::chrono::milliseconds{runTimeoutMs});
        runTimer.async_wait([timedOut, wake, token](const utilxx_base::AsioErrorCode& ec) {
            if (ec) {
                return; // 任务先结束, 定时器被撤销
            }
            timedOut->store(true);
            if (token && !token->is_cancelled()) {
                token->cancel();
            }
            if (wake) {
                wake->signal();
            }
        });
    }

    auto result = co_await agentxx::util::catchErrorAsync<events::RespSubagentBatchItem>(
        [&]() -> asio::awaitable<events::RespSubagentBatchItem> {
            if (isToolTask) {
                co_return co_await runToolWork(creator, spec, token, wake);
            }
            co_return co_await host->spawnOneTask(
                task,
                spec.creatorSessionId,
                token,
                creator,
                &spec
            );
        },
        [&task](std::string errmsg) -> asio::awaitable<events::RespSubagentBatchItem> {
            co_return events::RespSubagentBatchItem{
                .resultId     = task.resultId,
                .hasError     = true,
                .Canceled    = false,
                .errorMessage = fmt::format("Async work failed: {}", std::move(errmsg)),
            };
        },
        [&task](std::string& errmsg) -> std::optional<events::RespSubagentBatchItem> {
            return events::RespSubagentBatchItem{
                .resultId     = task.resultId,
                .hasError     = true,
                .Canceled    = true,
                .errorMessage = fmt::format("Async work canceled: {}", std::move(errmsg)),
            };
        },
        token
    );
    // 任务已结束: 撤销运行超时定时器 (处理器持 shared_ptr, 迟到的取消不会悬垂)
    runTimer.cancel();
    const auto finishedAtMs = systemNowMs();

    // ---- 结果终态 (取消优先: 取消时即使带错误也按取消收尾) ----
    bool        canceled = result.Canceled || (token && token->is_cancelled());
    bool        hasError = result.hasError && !canceled;
    std::string content  = result.content;
    std::string error    = result.errorMessage;
    // 运行超时导致的"取消"不是用户取消: 按失败报, 并说明是哪个限制
    if (timedOut->load()) {
        canceled = false;
        hasError = true;
        error    = fmt::format(
            "run timeout: still running after {} ms (async_work run limit)",
            runTimeoutMs
        );
        content.clear();
    }
    if ((hasError || canceled) && content.empty()) {
        content = error.empty() ? std::string{"(no result)"} : error;
    }

    finishEntry(
        entryId,
        hasError,
        canceled,
        std::move(content),
        std::move(error),
        startedAtMs,
        finishedAtMs,
        result.agentId,
        /*releaseClaim=*/true
    );
    co_return;
}

void AsyncWorkManager::finishEntry(
    std::string_view entryId,
    bool             hasError,
    bool             canceled,
    std::string      content,
    std::string      error,
    int64_t          startedAtMs,
    int64_t          finishedAtMs,
    std::string      agentId,
    bool             releaseClaim
) {
    auto host = host_.lock();
    if (!host) {
        return;
    }
    AsyncWorkKind                              kind = AsyncWorkKind::Agent;
    std::string                                name;
    std::string                                queueKey;
    std::string                                sessionId;
    std::string                                eventName;
    std::string                                notifyDelivery;
    std::string                                sessionStoreRoot;
    std::vector<std::weak_ptr<AsyncResultTicket>> waiters;
    std::shared_ptr<AgentContext>              creator;
    AsyncWorkEventFn                           onEvent;
    std::shared_ptr<agentxx::util::KeyMetrics> metrics;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = entries_.find(entryId);
        if (it == entries_.end()) {
            return;
        }
        auto& entry = it->second;
        if (entry.state != AsyncWorkState::Queued && entry.state != AsyncWorkState::Running) {
            return; // 已经收过尾: 幂等
        }
        kind             = entry.kind;
        name             = entry.name;
        queueKey         = entry.queueKey;
        sessionId        = entry.creatorSessionId;
        eventName        = entry.spec.eventName;
        notifyDelivery   = entry.spec.notifyDelivery;
        sessionStoreRoot = entry.spec.sessionStoreRoot;
        creator          = entry.creatorCtx.lock();
        onEvent          = entry.spec.onEvent;
        metrics          = entry.metrics;
        entry.wake.reset();
        // 正在等终态的调用: 取走票据 (锁外叫醒它们), 本次不再自动投递
        waiters.swap(entry.waiters);
        if (entry.state == AsyncWorkState::Queued) {
            // 排队中终止: 从队列摘除 (取消 / 排队超时都走这里)
            if (auto qit = queues_.find(queueKey); qit != queues_.end()) {
                auto& queued = qit->second;
                queued.erase(
                    std::remove(queued.begin(), queued.end(), std::string{entryId}),
                    queued.end()
                );
                if (queued.empty()) {
                    queues_.erase(qit);
                }
            }
            entry.delayUntilSteadyMs = 0;
        } else {
            // 运行中终止: 归还队列名额 (否则后续任务永远起不来)
            if (auto rit = runningPerQueue_.find(queueKey); rit != runningPerQueue_.end()) {
                if (rit->second > 0) {
                    --rit->second;
                }
                if (rit->second == 0) {
                    runningPerQueue_.erase(rit);
                }
            }
        }
        entry.state                  = canceled ? AsyncWorkState::Canceled
                                                : (hasError ? AsyncWorkState::Failed
                                                            : AsyncWorkState::Finished);
        entry.finishedAtMs           = finishedAtMs;
        entry.agentId                = std::move(agentId);
        entry.hasError               = hasError;
        entry.canceled               = canceled;
        entry.error                  = error;
        entry.resultText             = truncateUtf8(content, entry.resultMaxBytes);
        entry.lingerDeadlineSteadyMs = steadyNowMs() + entry.lingerMs;
    }

    // ---- 叫醒等终态的调用 ----
    // 它们醒来后自己读任务表 (正文只有一份来源); 有人接了正文时本次不再自动投递。
    // 排队中取消/超时的任务同样叫醒: 否则等待方只能挂到自己的超时
    size_t woken = 0;
    for (auto& weak : waiters) {
        if (auto ticket = weak.lock()) {
            ticket->ready.store(true, std::memory_order_release);
            ticket->wake->signal();
            ++woken;
        }
    }

    // ---- 结果事件: 纯广播 (插件 / 外部订阅者) ----
    // 不再承担"把结果交给会话"的职责 (那个直接在下面做)
    const std::string evName = eventName.empty() ? std::string{kAsyncDoneEvent} : eventName;
    auto              value  = utilxx_base::Json{
        {"asyncId",          entryId    },
        {"kind",             toString(kind)},
        {"name",             name       },
        {"content",          content    },
        {"hasError",         hasError   },
        {"canceled",         canceled   },
        {"errorMessage",     error      },
        {"creatorSessionId", sessionId  },
        {"startedAtMs",      startedAtMs},
        {"finishedAtMs",     finishedAtMs},
    };

    // ---- 结果回到创建者会话 (`notify_delivery`) ----
    // 没有等待方时直接交给会话端点: 文本与旧的事件输入同一形态
    // (`[Event] ... / [Info] ... / [Content] ...`), 界面与协议零改动
    bool        deliveredToSession = false;
    std::string deliveryReason;
    if (woken == 0 && !notifyDelivery.empty() && creator && creator->eventHub) {
        agentxx::events::EventWait synthetic;
        synthetic.spec.event = evName;
        synthetic.spec.mode  = "first";
        synthetic.spec.ref   = std::string{entryId};
        // 这里不是事件等待 (直接交付): `[Info]` 的 wait 行给 `-`, 不再编一个不存在的 id
        synthetic.waitId = "-";
        const std::vector<utilxx_base::Json> values{value};
        const auto contentText = creator->eventHub->contentText(sessionId, evName, values);
        agentxx::events::EventDelivery delivery;
        delivery.sessionId     = sessionId;
        delivery.text          = agentxx::events::renderEventText(synthetic, "value", contentText);
        delivery.source        = std::string{agentxx::agent::kInputSourceEventPrefix} + evName;
        delivery.delivery      = notifyDelivery;
        delivery.eventName     = evName;
        delivery.ref           = std::string{entryId};
        // 已跑完的事必须进上下文: 会话停在暂停态也要解除
        delivery.unpausePaused = true;
        const auto result      = creator->eventHub->deliver(delivery, &deliveryReason);
        deliveredToSession     = result == agentxx::events::DeliverResult::Queued
                             || result == agentxx::events::DeliverResult::Deferred;
    }

    bool delivered = false;
    if (creator && creator->eventHub) {
        creator->eventHub->postValue(evName, std::move(value), kAsyncOwner);
        creator->eventHub->postEnd(
            evName,
            utilxx_base::Json{
                {"asyncId",  entryId              },
                {"ok",       !hasError && !canceled},
                {"hasError", hasError             },
                {"canceled", canceled             },
            },
            kAsyncOwner
        );
        delivered = true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = entries_.find(entryId); it != entries_.end()) {
            it->second.delivered = true;
        }
    }

    if (metrics) {
        metrics->noteAsyncWorkFinished(hasError, canceled, finishedAtMs - startedAtMs);
    }
    XX_LOGI(
        "[async_work] finished {} state={} hasError={} delivered={} toSession={} awaited={} "
        "elapsed={}ms",
        entryId,
        toString(canceled ? AsyncWorkState::Canceled
                          : (hasError ? AsyncWorkState::Failed : AsyncWorkState::Finished)),
        hasError,
        delivered,
        deliveredToSession,
        woken,
        finishedAtMs - startedAtMs
    );
    if (!delivered) {
        XX_LOGW(
            "[async_work] {} finished but the creator has no event hub, result kept in the "
            "task table only (creatorSession={})",
            entryId,
            sessionId
        );
    } else if (!notifyDelivery.empty() && woken == 0 && !deliveredToSession) {
        XX_LOGW(
            "[async_work] {} result not put into the session ({}; creatorSession={})",
            entryId,
            deliveryReason.empty() ? std::string{"not delivered"} : deliveryReason,
            sessionId
        );
    }

    // 会话存储占用: 提交时就 claim, 排队期终态也要释放
    // (否则同一存储根的下一个任务会被报 "session store ... is busy")
    if (releaseClaim && !sessionStoreRoot.empty()) {
        host->releaseSession(
            sessionStoreRoot,
            std::string{entryId},
            fmt::format("async_work:{}", entryId)
        );
    }

    // 界面: 终态 (完成/失败/取消; 结束后会在任务表里 linger 一段时间供回看)
    postAsyncWorksChanged(host, creator, sessionId);

    // 提交方回调: 终态 (取消优先: 取消时即使带错误也按取消上报)
    if (onEvent) {
        fireAsyncWorkEvent(
            onEvent,
            canceled ? AsyncWorkEventPhase::Canceled : AsyncWorkEventPhase::Finished,
            utilxx_base::Json{
                {"asyncId",  entryId         },
                {"kind",     toString(kind)  },
                {"name",     name           },
                {"hasError", hasError        },
                {"canceled", canceled        },
                {"error",    error           },
            }
        );
    }

    // 排队补位 (先取执行器再构造投递体: 两者不要互相依赖求值顺序)
    if (!queueKey.empty()) {
        auto ex = host->ioCtx()->get_executor();
        asio::post(ex, [host, key = std::move(queueKey)]() {
            host->asyncWork_->pump(key);
        });
    }
}

bool AsyncWorkManager::cancel(std::string_view asyncId) {
    bool                       canceledRunning = false;
    /// 创建者 (界面通知用; 锁内取得, 锁外投递)
    std::shared_ptr<AgentContext> canceledCreator;
    std::string                   canceledSessionId;
    bool                          dequeued = false;
    std::shared_ptr<AsyncWake>    wake;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = entries_.find(asyncId);
        if (it == entries_.end()) {
            return false;
        }
        auto& e = it->second;
        if (e.state == AsyncWorkState::Queued) {
            dequeued         = true;
            canceledCreator  = e.creatorCtx.lock();
            canceledSessionId = e.creatorSessionId;
        } else if (e.state == AsyncWorkState::Running) {
            wake = e.wake;
            if (e.token && !e.token->is_cancelled()) {
                e.token->cancel();
            }
            canceledRunning = true;
        } else {
            return false; // 已终态
        }
    }
    if (dequeued) {
        // 排队中: 统一走收尾入口 (出队 + 终态 + 发结果事件)
        // - 结果事件不能省: 否则 `await` 等一条排队中被取消的任务会挂到自己的超时
        XX_LOGI("[async_work] cancel {} (dequeued)", asyncId);
        finishEntry(
            asyncId,
            /*hasError=*/false,
            /*canceled=*/true,
            std::string{},
            "canceled while queued",
            /*startedAtMs=*/0,
            systemNowMs(),
            std::string{},
            /*releaseClaim=*/true
        );
        return true;
    }
    XX_LOGI("[async_work] cancel {} (cancel token set)", asyncId);
    // 叫醒等待方: 工具任务立刻按取消收尾 (旧实现要等下一次 50 ms 轮询)
    if (wake) {
        wake->signal();
    }
    return canceledRunning;
}

bool AsyncWorkManager::setDelay(std::string_view asyncId, int64_t delayMs) {
    std::string                   queueKey;
    std::shared_ptr<AgentContext> creator;
    std::string                   sessionId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = entries_.find(asyncId);
        if (it == entries_.end() || it->second.state != AsyncWorkState::Queued) {
            return false;
        }
        auto&      entry = it->second;
        const auto now   = steadyNowMs();
        entry.delayMs    = std::max<int64_t>(0, delayMs);
        entry.delayUntilSteadyMs = entry.delayMs > 0 ? now + entry.delayMs : 0;
        // 排队超时的计时起点跟着延时重算 (延时本来就不算排队时间)
        entry.queueDeadlineSteadyMs
            = entry.queueTimeoutMs > 0
                  ? (entry.delayUntilSteadyMs > 0 ? entry.delayUntilSteadyMs : now)
                        + entry.queueTimeoutMs
                  : 0;
        queueKey  = entry.queueKey;
        creator   = entry.creatorCtx.lock();
        sessionId = entry.creatorSessionId;
    }
    auto host = host_.lock();
    if (!host) {
        return true;
    }
    XX_LOGI("[async_work] {} delay set to {}ms", asyncId, delayMs);
    asio::post(host->ioCtx()->get_executor(), [host, key = std::move(queueKey)]() {
        host->asyncWork_->pump(key);
    });
    postAsyncWorksChanged(host, creator, sessionId);
    return true;
}

size_t AsyncWorkManager::cancelByCreator(const std::shared_ptr<AgentContext>& creator) {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, entry] : entries_) {
            if (entry.state != AsyncWorkState::Queued && entry.state != AsyncWorkState::Running) {
                continue;
            }
            if (creator) {
                auto ctx = entry.creatorCtx.lock();
                if (!ctx || ctx != creator) {
                    continue;
                }
            }
            ids.push_back(id);
        }
    }
    size_t count = 0;
    for (const auto& id : ids) {
        if (cancel(id)) {
            ++count;
        }
    }
    if (count > 0) {
        XX_LOGI("[async_work] canceled {} task(s) by creator", count);
    }
    return count;
}

size_t AsyncWorkManager::cancelBySession(std::string_view creatorSessionId) {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, entry] : entries_) {
            if (entry.state != AsyncWorkState::Queued && entry.state != AsyncWorkState::Running) {
                continue;
            }
            if (entry.creatorSessionId == creatorSessionId) {
                ids.push_back(id);
            }
        }
    }
    size_t count = 0;
    for (const auto& id : ids) {
        if (cancel(id)) {
            ++count;
        }
    }
    if (count > 0) {
        XX_LOGI("[async_work] canceled {} task(s) of session `{}`", count, creatorSessionId);
    }
    return count;
}

size_t AsyncWorkManager::cancelByOwner(std::string_view owner) {
    if (owner.empty()) {
        return 0;
    }
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, entry] : entries_) {
            if (entry.state != AsyncWorkState::Queued && entry.state != AsyncWorkState::Running) {
                continue;
            }
            if (entry.owner == owner) {
                ids.push_back(id);
            }
        }
    }
    size_t count = 0;
    for (const auto& id : ids) {
        if (cancel(id)) {
            ++count;
        }
    }
    if (count > 0) {
        XX_LOGI("[async_work] canceled {} task(s) of owner `{}`", count, owner);
    }
    return count;
}

utilxx_base::Json AsyncWorkManager::entryJson(std::string_view asyncId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = entries_.find(asyncId);
    if (it == entries_.end()) {
        return utilxx_base::Json::object();
    }
    const auto& e = it->second;
    auto        j = utilxx_base::Json{
        {"asyncId",          e.asyncId          },
        {"kind",             toString(e.kind)   },
        {"name",             e.name             },
        {"state",            toString(e.state)  },
        {"queueKey",         e.queueKey         },
        {"creatorAgentId",   e.creatorAgentId   },
        {"creatorSessionId", e.creatorSessionId },
        {"owner",            e.owner            },
        {"agentId",          e.agentId          },
        {"model",            e.model            },
        {"createdAtMs",      e.createdAtMs      },
        {"startedAtMs",      e.startedAtMs      },
        {"finishedAtMs",     e.finishedAtMs     },
        {"hasError",         e.hasError         },
        {"canceled",         e.canceled         },
        {"delivered",        e.delivered        },
        // 中断策略属于提交快照, 诊断/测试用它确认"从配置解析到了什么"
        {"interruptPolicy",
         toString(e.spec.interruptPolicy.value_or(AsyncInterruptPolicy::Bubble))},
    };
    if (!e.error.empty()) {
        j["error"] = e.error;
    }
    // 起跑延时与两个超时: `delayMs`/`xxTimeoutMs` 是提交时的原值,
    // `delayRemainingMs` 是此刻还剩多少延时
    if (e.delayMs > 0) {
        j["delayMs"] = e.delayMs;
    }
    if (e.queueTimeoutMs > 0) {
        j["queueTimeoutMs"] = e.queueTimeoutMs;
    }
    if (e.runTimeoutMs > 0) {
        j["runTimeoutMs"] = e.runTimeoutMs;
    }
    if (e.state == AsyncWorkState::Queued && e.delayUntilSteadyMs > 0) {
        const auto now        = steadyNowMs();
        j["delayRemainingMs"] = e.delayUntilSteadyMs > now ? e.delayUntilSteadyMs - now : 0;
    }
    if (e.state == AsyncWorkState::Queued && e.queueDeadlineSteadyMs != 0) {
        const auto now        = steadyNowMs();
        j["queueRemainingMs"] = e.queueDeadlineSteadyMs > now ? e.queueDeadlineSteadyMs - now : 0;
    }
    if (!e.resultText.empty()) {
        j["result"] = e.resultText;
    }
    return j;
}

asio::awaitable<AsyncWorkManager::WaitOutcome> AsyncWorkManager::waitTerminal(
    std::string_view                              asyncId,
    int64_t                                       timeoutMs,
    std::shared_ptr<neograph::graph::CancelToken> token
) {
    auto host = host_.lock();
    if (!host || asyncId.empty()) {
        co_return WaitOutcome::AlreadyTerminal;
    }

    // 票据只在内存里, 任务收尾时被取走; 不参与正文 (正文只有任务表一份)
    auto ticket = std::make_shared<AsyncResultTicket>(host->ioCtx()->get_executor());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = entries_.find(asyncId);
        if (it == entries_.end() || isTerminalState(it->second.state)) {
            // 已经结束 (或已被 linger 清扫): 调用方直接去读任务表
            co_return WaitOutcome::AlreadyTerminal;
        }
        it->second.waiters.push_back(ticket);
    }

    /// 离开时摘票 (已终态时票据已被管理器取走, 摘不到也没关系)
    struct WaiterGuard {
        AsyncWorkManager*                        self = nullptr;
        std::string                              asyncId;
        std::shared_ptr<AsyncResultTicket>        ticket;
        ~WaiterGuard() {
            if (self != nullptr) {
                self->removeWaiter(asyncId, ticket);
            }
        }
    } guard{this, std::string{asyncId}, ticket};

    const auto deadline = timeoutMs > 0 ? steadyNowMs() + timeoutMs : int64_t{0};
    while (true) {
        if (ticket->ready.load(std::memory_order_acquire)) {
            co_return WaitOutcome::Filled;
        }
        if (token && token->is_cancelled()) {
            co_return WaitOutcome::Canceled;
        }
        const auto now = steadyNowMs();
        if (deadline > 0 && now >= deadline) {
            co_return WaitOutcome::TimedOut;
        }
        // 分片等待: 取消令牌只有轮询接口 (没有回调), 分片让取消最多晚一片被看到;
        // 任务真正结束时票据会立刻叫醒, 所以这只是一次上限
        int64_t slice = kTicketSliceMs;
        if (deadline > 0) {
            slice = std::min<int64_t>(slice, deadline - now);
        }
        co_await ticket->wake->wait(std::chrono::milliseconds{std::max<int64_t>(1, slice)});
    }
}

void AsyncWorkManager::removeWaiter(
    std::string_view                   asyncId,
    const std::shared_ptr<AsyncResultTicket>& ticket
) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = entries_.find(asyncId);
    if (it == entries_.end()) {
        return;
    }
    auto& waiters = it->second.waiters;
    waiters.erase(
        std::remove_if(
            waiters.begin(),
            waiters.end(),
            [&](const auto& weak) { return weak.lock() == ticket; }
        ),
        waiters.end()
    );
}

utilxx_base::Json AsyncWorkManager::listJson(
    std::string_view creatorAgentId,
    std::string_view creatorSessionId,
    bool             includeFinished,
    size_t           limit
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        arr = utilxx_base::Json::array();
    size_t                      n   = 0;
    for (const auto& [id, e] : entries_) {
        if (n >= limit) {
            break;
        }
        if (!creatorAgentId.empty() && e.creatorAgentId != creatorAgentId) {
            continue;
        }
        if (!creatorSessionId.empty() && e.creatorSessionId != creatorSessionId) {
            continue;
        }
        const bool finished = e.state == AsyncWorkState::Finished
                              || e.state == AsyncWorkState::Failed
                              || e.state == AsyncWorkState::Canceled;
        if (!includeFinished && finished) {
            continue;
        }
        auto item = utilxx_base::Json{
            {"asyncId",      e.asyncId         },
            {"kind",         toString(e.kind)  },
            {"name",         e.name            },
            {"state",        toString(e.state) },
            {"owner",        e.owner           },
            {"startedAtMs",  e.startedAtMs     },
            {"finishedAtMs", e.finishedAtMs    },
            {"hasError",     e.hasError        },
            {"canceled",     e.canceled        },
        };
        if (e.state == AsyncWorkState::Queued) {
            // 排队位置: 同一队列里在它前面的条数 (给模型看"还要等几个")
            int64_t ahead = 0;
            if (auto qit = queues_.find(e.queueKey); qit != queues_.end()) {
                for (const auto& queuedId : qit->second) {
                    if (queuedId == e.asyncId) {
                        break;
                    }
                    ++ahead;
                }
            }
            item["queueAhead"] = ahead;
            if (e.delayMs > 0) {
                item["delayMs"] = e.delayMs;
            }
            if (e.delayUntilSteadyMs > 0) {
                const auto now = steadyNowMs();
                item["delayRemainingMs"]
                    = e.delayUntilSteadyMs > now ? e.delayUntilSteadyMs - now : 0;
            }
            if (e.queueDeadlineSteadyMs != 0) {
                const auto now = steadyNowMs();
                item["queueRemainingMs"]
                    = e.queueDeadlineSteadyMs > now ? e.queueDeadlineSteadyMs - now : 0;
            }
        }
        arr.push_back(std::move(item));
        ++n;
    }
    return arr;
}

size_t AsyncWorkManager::runningCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t                      n = 0;
    for (const auto& [id, entry] : entries_) {
        if (entry.state == AsyncWorkState::Running) {
            ++n;
        }
    }
    return n;
}

size_t AsyncWorkManager::queuedCount(std::string_view queueKey) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!queueKey.empty()) {
        auto it = queues_.find(queueKey);
        return it == queues_.end() ? 0 : it->second.size();
    }
    size_t n = 0;
    for (const auto& [key, q] : queues_) {
        n += q.size();
    }
    return n;
}

size_t AsyncWorkManager::shutdown(std::string_view reason) {
    XX_LOGI("[async_work] shutdown: {}", reason);
    return cancelByCreator(nullptr);
}

} // namespace agent
} // namespace agentxx
