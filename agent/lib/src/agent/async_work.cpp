#include "agentxx/agent/async_work.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/hub.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/observability.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <chrono>

namespace agentxx {
namespace agent {

namespace {

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
    if (spec.kind != AsyncWorkKind::Agent) {
        // 异步工具 (kind=tool) 结构已预留, 本期只落 agent
        out.error = "kind `tool` is not supported yet";
        return out;
    }
    if (spec.message.empty() && !spec.messages.is_array()) {
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
    return out;
}

void AsyncWorkManager::pump(std::string_view queueKey) {
    auto host = host_.lock();
    if (!host) {
        return;
    }
    std::string nextId;
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
    const auto startedAtMs = systemNowMs();

    auto task   = taskOfSpec(spec);
    auto result = co_await agentxx::util::catchErrorAsync<events::RespSubagentBatchItem>(
        [&]() -> asio::awaitable<events::RespSubagentBatchItem> {
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
            }
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
    std::string queueKey;
    bool        removedFromQueue = false;
    bool        canceledRunning  = false;
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
        // 出队后让后面的任务有机会起跑
        if (auto h = host_.lock(); h) {
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
        {"agentId",          e.agentId          },
        {"model",            e.model            },
        {"createdAtMs",      e.createdAtMs      },
        {"startedAtMs",      e.startedAtMs      },
        {"finishedAtMs",     e.finishedAtMs     },
        {"hasError",         e.hasError         },
        {"canceled",         e.canceled         },
        {"delivered",        e.delivered        },
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
