#include "ffi_call.h"

#include "agentxx/ffi_api.h"
#include "ffi_ops.h"
#include "utilxx_base/log.h"
#include <asio/post.hpp>
#include <utility>

namespace agentxx {
namespace ffi {

CallRegistry::CallRegistry(asio::io_context& ioCtx, EmitFn emit) :
    ioCtx_(ioCtx),
    emit_(std::move(emit)),
    timer_(std::make_shared<asio::steady_timer>(ioCtx_)) {}

CallRegistry::~CallRegistry() {
    // 定时器随 io_context 一起收尾: 这里只保证回调不会再碰已析构的对象
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    ++generation_;
    if (timer_) {
        timer_->cancel();
    }
}

int64_t CallRegistry::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

uint64_t CallRegistry::begin(const FfiOpDef& def, int32_t timeoutMs) {
    auto    entry = std::make_shared<Entry>();
    entry->def    = &def;
    uint64_t reqId = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reqId = ++nextReqId_;
        // 1 是第一个可用序号; 0 留给"没有 reqId"
        if (reqId == 0) {
            reqId = ++nextReqId_;
        }
        if (timeoutMs > 0) {
            entry->deadlineMs = nowMs() + static_cast<int64_t>(timeoutMs);
            deadlines_.emplace(entry->deadlineMs, reqId);
        }
        entries_.emplace(reqId, entry);
    }
    if (timeoutMs > 0) {
        reschedule();
    }
    return reqId;
}

void CallRegistry::abort(uint64_t reqId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = entries_.find(reqId);
    if (it == entries_.end()) {
        return;
    }
    if (it->second->deadlineMs != 0) {
        auto range = deadlines_.equal_range(it->second->deadlineMs);
        for (auto i = range.first; i != range.second; ++i) {
            if (i->second == reqId) {
                deadlines_.erase(i);
                break;
            }
        }
    }
    entries_.erase(it);
}

bool CallRegistry::settle(uint64_t reqId, CallOutcome outcome) {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = entries_.find(reqId);
        if (it == entries_.end()) {
            return false;
        }
        entry = it->second;
        if (entry->deadlineMs != 0) {
            auto range = deadlines_.equal_range(entry->deadlineMs);
            for (auto i = range.first; i != range.second; ++i) {
                if (i->second == reqId) {
                    deadlines_.erase(i);
                    break;
                }
            }
        }
        entries_.erase(it);
    }

    // 记录已出表, 正常路径下只有一个线程能拿到这条 entry; CAS 是兜底
    // (超时检查与应答同时到达时, 仍只有一个赢家负责回执)
    bool expected = false;
    if (!entry->settled.compare_exchange_strong(expected, true)) {
        return false;
    }
    if (!outcome.ok) {
        if (outcome.errorName.empty()) {
            outcome.errorName = "rejected";
        }
        if (outcome.code == 0) {
            outcome.code = entry->def ? entry->def->failureCode : AGENTXX_FFI_ERR_REJECTED;
        }
    }
    const std::string_view op = entry->def ? entry->def->name : std::string_view{};
    if (emit_) {
        emit_(reqId, op, outcome);
    }
    return true;
}

bool CallRegistry::cancel(uint64_t reqId) {
    CallOutcome outcome;
    outcome.ok        = false;
    outcome.code      = AGENTXX_FFI_ERR_CANCELED;
    outcome.errorName = "canceled";
    outcome.message   = "canceled by caller";
    return settle(reqId, std::move(outcome));
}

size_t CallRegistry::failAll(int32_t code, std::string_view errorName, std::string message) {
    std::vector<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ids.reserve(entries_.size());
        for (const auto& [id, entry] : entries_) {
            ids.push_back(id);
        }
    }
    size_t done = 0;
    for (uint64_t id : ids) {
        CallOutcome outcome;
        outcome.ok        = false;
        outcome.code      = code;
        outcome.errorName = std::string{errorName};
        outcome.message   = message;
        if (settle(id, std::move(outcome))) {
            ++done;
        }
    }
    return done;
}

size_t CallRegistry::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void CallRegistry::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        ++generation_;
    }
    if (timer_) {
        asio::post(ioCtx_, [timer = timer_] { timer->cancel(); });
    }
}

void CallRegistry::reschedule() {
    auto self = shared_from_this();
    asio::post(ioCtx_, [self] { self->onTimeout(); });
}

void CallRegistry::armTimerLocked(int64_t nextDeadlineMs) {
    // 已在 io 线程: 直接用最新代次重臂
    const uint64_t gen = ++generation_;
    if (nextDeadlineMs <= 0) {
        timer_->cancel();
        return;
    }
    const int64_t now = nowMs();
    // 已过期就立刻到期 (1ms), 让处理走同一条路径
    const int64_t waitMs = nextDeadlineMs > now ? (nextDeadlineMs - now) : 1;
    timer_->expires_after(std::chrono::milliseconds(waitMs));
    auto self = shared_from_this();
    timer_->async_wait([self, gen](const std::error_code& ec) {
        if (ec) {
            return; // 被取消/覆盖: 由新的那次检查负责
        }
        std::unique_lock<std::mutex> lock(self->mutex_);
        if (gen != self->generation_ || self->stopping_) {
            return;
        }
        lock.unlock();
        self->checkExpired();
    });
}

void CallRegistry::onTimeout() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_) {
        return;
    }
    // 跳过已经结算的到期项, 找出当前最早的到期时刻
    int64_t next = 0;
    for (auto it = deadlines_.begin(); it != deadlines_.end();) {
        auto e = entries_.find(it->second);
        if (e == entries_.end() || e->second->settled.load(std::memory_order_acquire)) {
            it = deadlines_.erase(it);
            continue;
        }
        next = it->first;
        break;
    }
    lock.unlock();
    armTimerLocked(next);

    // 到期项就地处理 (armTimerLocked 已保证下一次检查会到来)
    checkExpired();
}

void CallRegistry::checkExpired() {
    const int64_t         now = nowMs();
    std::vector<uint64_t> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = deadlines_.begin(); it != deadlines_.end() && it->first <= now;) {
            auto e = entries_.find(it->second);
            if (e != entries_.end() && !e->second->settled.load(std::memory_order_acquire)) {
                expired.push_back(it->second);
            }
            it = deadlines_.erase(it);
        }
    }
    for (uint64_t id : expired) {
        CallOutcome outcome;
        outcome.ok        = false;
        outcome.code      = AGENTXX_FFI_ERR_TIMEOUT;
        outcome.errorName = "timeout";
        outcome.message   = "call timed out before the server replied";
        if (settle(id, std::move(outcome))) {
            XX_LOGD("[ffi] call #{} timed out", id);
        }
    }
}

} // namespace ffi
} // namespace agentxx
