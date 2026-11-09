#include "agentxx/agent/async_work.h"

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
#include "agentxx/util/tool_args_fix.h"
#include "agentxx/util/tool_result.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <chrono>
#include <filesystem>

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
                .resultJson = R"({"__Canceled__":true})",
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
constexpr std::string_view kAsyncDoneEvent = "agentxx.agent.async.done";

/// 结果事件与台账的归属标记 (`plugin.` 之外的自有标记, 只用于日志/诊断)
constexpr std::string_view kAsyncOwner = "async_work";

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

/// 通知创建者会话的端点: 台账变了 (提交 / 起跑 / 终态)
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
///     - [sessionId] 创建者会话 id (台账里登记的那个)
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

/// 跑一个工具任务 (`kind == Tool`): 在创建者的工具表里执行同名工具
///
/// 取消是"尽力而为": 每 50ms 查一次取消令牌, 置位后立即按取消收尾 (工具本身如果
/// 不认识取消, 它会在后台继续跑到自己的超时, 结果被丢弃); 协程状态用 shared_ptr
/// 持有, 不会因为本函数提前退出而悬挂。
asio::awaitable<events::RespSubagentBatchItem> runToolWork(
    const std::shared_ptr<AgentContext>&          creator,
    const AsyncWorkSpec&                          spec,
    const std::shared_ptr<neograph::graph::CancelToken>& token
) {
    auto fail = [&spec](std::string message, bool canceled = false) {
        return events::RespSubagentBatchItem{
            .resultId     = spec.resultId,
            .hasError     = true,
            .Canceled     = canceled,
            .errorMessage = std::move(message),
        };
    };
    if (!creator || !creator->toolRegistry) {
        co_return fail("async tool: creator has no tool registry");
    }
    auto tool = creator->toolRegistry->find(spec.toolName);
    if (!tool) {
        // lib 内置工具由引擎持有, 不在注册表里: 它们本来就是快工具, 不支持后台跑
        co_return fail(fmt::format(
            "async tool: tool `{}` not found in the creator's tool registry "
            "(only plugin / dynamically registered tools can run in the background)",
            spec.toolName
        ));
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
    auto args  = spec.toolArgs; // 拷一份进协程帧: 提交方的对象可能早就不在了
    // 参数类型自动修正: 与前台 ToolcallWrapNode 执行前同一份实现 (内置工具与插件工具
    // 一视同仁), 后台跑的工具同样按自己声明的 schema 拿到正确类型
    try {
        const auto parameters = agentxx::util::fromNeographJson(tool->get_definition().parameters);
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
    const auto ex = co_await asio::this_coro::executor;
    // 必须把**可调用对象**交给 co_spawn (不要在这里内联调用成 `{...}()`):
    // lambda 协程的帧只保存闭包指针, 内联调用产生的是临时闭包 —— 语句结束即析构,
    // 而这具协程要在本函数返回后继续跑 (取消时本函数先退出, 工具仍在后台跑到自己的
    // 超时)。传可调用对象时闭包由 co_spawn 复制进自己的协程帧, 生命周期与任务一致,
    // 后台协程访问 state/tool/args 才不悬垂 (同 [TaskScope::spawn] 的约定)
    asio::co_spawn(
        ex,
        [state, tool, args = std::move(args)]() -> asio::awaitable<void> {
            state->result = co_await agentxx::util::catchErrorAsync<agentxx::util::ToolResult>(
                [&]() -> asio::awaitable<agentxx::util::ToolResult> {
                    co_return co_await tool->execute_async(args);
                },
                [](std::string errmsg) -> asio::awaitable<agentxx::util::ToolResult> {
                    co_return agentxx::util::ToolResult::fail(std::move(errmsg));
                }
            );
            state->done.store(true, std::memory_order_release);
            co_return;
        },
        asio::detached
    );

    // 等完成 / 等取消 (两者都不阻塞 io 线程)
    asio::steady_timer poll{ex};
    while (!state->done.load(std::memory_order_acquire)) {
        if (token && token->is_cancelled()) {
            co_return fail("async tool canceled", /*canceled=*/true);
        }
        poll.expires_after(std::chrono::milliseconds{50});
        utilxx_base::AsioErrorCode ec;
        co_await poll.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }

    const auto& result = state->result;
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

        queues_[spec.queueKey].push_back(entry.asyncId);
        entries_.insert_or_assign(entry.asyncId, std::move(entry));

        // 指标: 受理 + 当前排队数峰值
        if (metrics) {
            metrics->noteAsyncWorkSubmitted(queues_[spec.queueKey].size());
        }
    }

    XX_LOGI(
        "[async_work] submitted {} ({}) queue={} creator={} session={} ahead={}",
        out.asyncId,
        toString(spec.kind),
        spec.queueKey,
        spec.creatorAgentId,
        spec.creatorSessionId,
        out.queueAhead
    );

    // 提交方回调: 受理 (排到位) —— 投递到容器 io 线程, 与后续 Started/Finished 保序
    if (spec.onEvent) {
        auto            cb      = spec.onEvent;
        auto            payload = utilxx_base::Json{
            {"asyncId", out.asyncId  },
            {"kind",    toString(spec.kind)},
            {"name",    spec.name    },
            {"queueAhead", out.queueAhead},
        };
        const auto executor = host->ioCtx()->get_executor();
        asio::post(
            executor,
            [cb = std::move(cb), payload = std::move(payload)]() {
                fireAsyncWorkEvent(cb, AsyncWorkEventPhase::Queued, payload);
            }
        );
    }

    // 结果直接唤醒创建者会话 (`notify_delivery`): 入队前就替它登记定向等待,
    // 任务完成时值经中枢投递成一条会话输入 (don't rely on the model to watch)
    if (!spec.notifyDelivery.empty() && creator && creator->eventHub) {
        auto hub           = creator->eventHub;
        const auto asyncId = out.asyncId;
        const auto session = spec.creatorSessionId;
        const auto delivery = spec.notifyDelivery;
        agentxx::events::EventWaitSpec waitSpec;
        waitSpec.event         = std::string{kAsyncDoneEvent};
        waitSpec.mode          = "first";
        waitSpec.delivery      = delivery;
        waitSpec.originSession = session;
        waitSpec.owner         = std::string{kAsyncOwner};
        waitSpec.ref           = asyncId;
        waitSpec.match         = utilxx_base::Json{{"asyncId", asyncId}};
        waitSpec.replay        = false;
        bool executed          = false;
        hub->callSync(
            [&](agentxx::events::EventHub& h) {
                h.watch(std::move(waitSpec));
            },
            &executed
        );
        if (!executed) {
            XX_LOGW(
                "[async_work] {}: could not register the session wake-up wait (hub busy)",
                asyncId
            );
        }
    }

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
    std::string        nextId;
    AsyncWorkEventFn   startCb;
    utilxx_base::Json  startPayload;
    /// 创建者 (界面通知用; 锁内取得, 锁外投递)
    std::shared_ptr<AgentContext> nextCreator;
    std::string                   nextSessionId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        qit = queues_.find(queueKey);
        if (qit == queues_.end()) {
            return;
        }
        auto& queue = qit->second;
        while (!queue.empty()) {
            const std::string headId = queue.front();
            auto              eit    = entries_.find(headId);
            if (eit == entries_.end() || eit->second.state != AsyncWorkState::Queued) {
                // 台账已回收 / 已被别的路径置终态: 跳过
                queue.erase(queue.begin());
                continue;
            }
            auto         rit     = runningPerQueue_.find(queueKey);
            const size_t running = rit == runningPerQueue_.end() ? 0 : rit->second;
            const size_t quota   = std::max<size_t>(1, eit->second.maxConcurrent);
            if (running >= quota) {
                break; // 队列满额: 等某个任务收尾时再推
            }
            nextId = headId;
            queue.erase(queue.begin());
            runningPerQueue_[std::string{queueKey}] = running + 1;
            eit->second.state                       = AsyncWorkState::Running;
            eit->second.startedAtMs                 = systemNowMs();
            nextCreator                             = eit->second.creatorCtx.lock();
            nextSessionId                           = eit->second.creatorSessionId;
            // 提交方回调 (起跑) 在锁外触发, 避免外部代码重入引发死锁
            startCb      = eit->second.spec.onEvent;
            startPayload = utilxx_base::Json{
                {"asyncId", headId              },
                {"kind",    toString(eit->second.kind)},
                {"name",    eit->second.name    },
            };
            break;
        }
        auto qit2 = queues_.find(queueKey);
        if (qit2 != queues_.end() && qit2->second.empty()) {
            queues_.erase(qit2);
        }
    }
    if (nextId.empty()) {
        return;
    }
    fireAsyncWorkEvent(startCb, AsyncWorkEventPhase::Started, startPayload);
    // 界面: 排队 → 运行 (在跑/排队两边计数都变了)
    postAsyncWorksChanged(host, nextCreator, nextSessionId);
    XX_LOGD("[async_work] launch {} (queue={})", nextId, queueKey);
    asio::co_spawn(
        host->ioCtx()->get_executor(),
        [host, id = std::move(nextId)]() -> asio::awaitable<void> {
            co_await host->asyncWork_->runner(std::move(host), id);
        },
        asio::detached
    );
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
    // 会话落盘占用在任务收尾时释放 (存储根本身留在磁盘上供事后回看)
    struct SessionClaimRelease {
        std::shared_ptr<AgentHost> host;
        std::string                root;
        std::string                sessionId;
        std::string                owner;

        ~SessionClaimRelease() {
            if (host && !root.empty()) {
                host->releaseSession(root, sessionId, owner);
            }
        }
    } claimRelease;
    claimRelease.host      = host;
    claimRelease.root      = spec.sessionStoreRoot;
    claimRelease.sessionId = entryId;
    claimRelease.owner     = fmt::format("async_work:{}", entryId);
    const auto startedAtMs = systemNowMs();

    auto task   = taskOfSpec(spec);
    const bool isToolTask = spec.kind == agentxx::agent::AsyncWorkKind::Tool;
    auto       result    = co_await agentxx::util::catchErrorAsync<events::RespSubagentBatchItem>(
        [&]() -> asio::awaitable<events::RespSubagentBatchItem> {
            if (isToolTask) {
                co_return co_await runToolWork(creator, spec, token);
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
    const auto finishedAtMs = systemNowMs();

    // ---- 结果终态 (取消优先: 取消时即使带错误也按取消收尾) ----
    const bool  canceled = result.Canceled || (token && token->is_cancelled());
    const bool  hasError = result.hasError && !canceled;
    std::string content  = result.content;
    std::string error    = result.errorMessage;
    if ((hasError || canceled) && content.empty()) {
        content = error.empty() ? std::string{"(no result)"} : error;
    }

    // ---- 结果投递: 事件 (主路) ----
    bool delivered = false;
    if (creator && creator->eventHub) {
        const std::string eventName
            = spec.eventName.empty() ? std::string{kAsyncDoneEvent} : spec.eventName;
        auto value = utilxx_base::Json{
            {"asyncId",          entryId               },
            {"kind",             toString(spec.kind)   },
            {"name",             spec.name             },
            {"content",          content               },
            {"hasError",         hasError              },
            {"canceled",         canceled              },
            {"errorMessage",     error                 },
            {"creatorSessionId", spec.creatorSessionId },
            {"startedAtMs",      startedAtMs           },
            {"finishedAtMs",     finishedAtMs          },
        };
        creator->eventHub->postValue(eventName, std::move(value), kAsyncOwner);
        creator->eventHub->postEnd(
            eventName,
            utilxx_base::Json{
                {"asyncId",  entryId              },
                {"ok",       !hasError && !canceled},
                {"hasError", hasError             },
                {"canceled", canceled             },
            },
            kAsyncOwner
        );
        delivered = true;
    }

    // ---- 台账 + 队列补位 ----
    std::string queueKey;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = entries_.find(entryId);
        if (it != entries_.end()) {
            auto& entry                  = it->second;
            entry.state                  = canceled ? AsyncWorkState::Canceled
                                                     : (hasError ? AsyncWorkState::Failed
                                                                 : AsyncWorkState::Finished);
            entry.finishedAtMs           = finishedAtMs;
            entry.agentId                = result.agentId;
            entry.hasError               = hasError;
            entry.canceled               = canceled;
            entry.error                  = error;
            entry.delivered              = delivered;
            entry.resultText             = truncateUtf8(content, entry.resultMaxBytes);
            entry.lingerDeadlineSteadyMs = steadyNowMs() + entry.lingerMs;
            queueKey                     = entry.queueKey;
            if (entry.metrics) {
                entry.metrics->noteAsyncWorkFinished(
                    hasError,
                    canceled,
                    finishedAtMs - startedAtMs
                );
            }
            auto rit                     = runningPerQueue_.find(queueKey);
            if (rit != runningPerQueue_.end()) {
                if (rit->second > 0) {
                    --rit->second;
                }
                if (rit->second == 0) {
                    runningPerQueue_.erase(rit);
                }
            }
        }
    }
    XX_LOGI(
        "[async_work] finished {} state={} hasError={} delivered={} elapsed={}ms",
        entryId,
        toString(canceled ? AsyncWorkState::Canceled
                          : (hasError ? AsyncWorkState::Failed : AsyncWorkState::Finished)),
        hasError,
        delivered,
        finishedAtMs - startedAtMs
    );
    if (!delivered) {
        XX_LOGW(
            "[async_work] {} finished but the creator has no event hub, result kept in the "
            "ledger only (creatorSession={})",
            entryId,
            spec.creatorSessionId
        );
    }

    // 界面: 终态 (完成/失败/取消; 结束后会在台账里 linger 一段时间供回看)
    postAsyncWorksChanged(host, creator, spec.creatorSessionId);

    // 提交方回调: 终态 (取消优先: 取消时即使带错误也按取消上报)
    if (spec.onEvent) {
        fireAsyncWorkEvent(
            spec.onEvent,
            canceled ? AsyncWorkEventPhase::Canceled : AsyncWorkEventPhase::Finished,
            utilxx_base::Json{
                {"asyncId",  entryId              },
                {"kind",     toString(spec.kind)  },
                {"name",     spec.name            },
                {"hasError", hasError             },
                {"canceled", canceled             },
                {"error",    error                },
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
    co_return;
}

bool AsyncWorkManager::cancel(std::string_view asyncId) {
    std::string       queueKey;
    bool              removedFromQueue = false;
    bool              canceledRunning  = false;
    AsyncWorkEventFn  canceledCb;
    utilxx_base::Json canceledPayload;
    /// 创建者 (界面通知用; 锁内取得, 锁外投递)
    std::shared_ptr<AgentContext> canceledCreator;
    std::string                   canceledSessionId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = entries_.find(asyncId);
        if (it == entries_.end()) {
            return false;
        }
        auto& e  = it->second;
        queueKey = e.queueKey;
        if (e.state == AsyncWorkState::Queued) {
            auto qit = queues_.find(queueKey);
            if (qit != queues_.end()) {
                auto& v = qit->second;
                v.erase(std::remove(v.begin(), v.end(), e.asyncId), v.end());
                if (v.empty()) {
                    queues_.erase(qit);
                }
            }
            e.state                  = AsyncWorkState::Canceled;
            e.canceled               = true;
            e.finishedAtMs           = systemNowMs();
            e.error                  = "canceled while queued";
            e.lingerDeadlineSteadyMs = steadyNowMs() + e.lingerMs;
            removedFromQueue         = true;
            canceledCreator          = e.creatorCtx.lock();
            canceledSessionId        = e.creatorSessionId;
            // 排队中直接被取消: 任务不会起跑, 终态回调在这里发
            canceledCb      = e.spec.onEvent;
            canceledPayload = utilxx_base::Json{
                {"asyncId",  e.asyncId           },
                {"kind",     toString(e.kind)     },
                {"name",     e.name              },
                {"hasError", false                },
                {"canceled", true                 },
                {"error",    e.error             },
            };
        } else if (e.state == AsyncWorkState::Running) {
            if (e.token && !e.token->is_cancelled()) {
                e.token->cancel();
            }
            canceledRunning = true;
        }
    }
    if (!removedFromQueue && !canceledRunning) {
        return false; // 已终态
    }
    XX_LOGI(
        "[async_work] cancel {} ({})",
        asyncId,
        removedFromQueue ? "dequeued" : "cancel token set"
    );
    if (removedFromQueue) {
        fireAsyncWorkEvent(canceledCb, AsyncWorkEventPhase::Canceled, canceledPayload);
        // 出队后让后面的任务有机会起跑
        if (auto h = host_.lock(); h) {
            // 界面: 排队中的任务直接取消 (没入过运行态, 终态推送只在这里发)
            postAsyncWorksChanged(h, canceledCreator, canceledSessionId);
            asio::post(h->ioCtx()->get_executor(), [h, key = std::move(queueKey)]() {
                h->asyncWork_->pump(key);
            });
        }
    }
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
    if (!e.resultText.empty()) {
        j["result"] = e.resultText;
    }
    return j;
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
