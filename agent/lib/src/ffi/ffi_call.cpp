#include "ffi_call.h"

#include "agentxx/ffi_api.h"
#include "ffi_ops.h"
#include "utilxx_base/log.h"
#include <asio/post.hpp>
#include <chrono>
#include <utility>
#include <vector>

namespace agentxx {
namespace ffi {

CallRegistry::CallRegistry(asio::io_context& ioCtx, EmitFn emit) :
    ioCtx_(ioCtx),
    emit_(std::move(emit)),
    timer_(std::make_shared<asio::steady_timer>(ioCtx_)) {}

CallRegistry::~CallRegistry() {
    // 定时器随 io_context 一起收尾: 这里只保证回调不会再碰已析构的对象
    stopping_.store(true, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_release);
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
    int64_t reqId = 0;
    {
        // 收尾已开始: 表已被 failAll 清过一次, 此后登记的条目既不会被它命中, 也不会
        // 排超时定时器 (onTimeout 看到 stopping_ 直接返回) —— 调用方会永远等不到回执。
        // 未受理由调用方按状态错误返回 (不发回执), 与参数校验失败同一条路。
        // 判定与登记在同一把锁里, 才能保证 stop()/failAll() 之间不留缝。
        std::lock_guard<std::mutex> lock(stopMutex_);
        if (stopping_.load(std::memory_order_acquire)) {
            return 0;
        }
        reqId = registry_.begin(def.name, {}, timeoutMs, nowMs());
    }
    if (timeoutMs > 0) {
        reschedule();
    }
    return static_cast<uint64_t>(reqId);
}

bool CallRegistry::isStopping() const {
    return stopping_.load(std::memory_order_acquire);
}

void CallRegistry::abort(uint64_t reqId) {
    // 同步失败: 出表但不发回执
    registry_.settle(static_cast<int64_t>(reqId));
}

void CallRegistry::emitOutcome(const pluginxx::ffi::RequestEntry& entry, CallOutcome outcome) {
    if (!outcome.ok) {
        if (outcome.errorName.empty()) {
            outcome.errorName = "rejected";
        }
        if (outcome.code == 0) {
            // 记账只留 op 名, 默认失败码要回查 op 定义
            const FfiOpDef* def = findOp(entry.op);
            outcome.code        = def ? def->failureCode : AGENTXX_FFI_ERR_REJECTED;
        }
    }
    if (emit_) {
        emit_(static_cast<uint64_t>(entry.id), entry.op, outcome);
    }
}

bool CallRegistry::settle(uint64_t reqId, CallOutcome outcome) {
    pluginxx::ffi::RequestEntry entry;
    if (!registry_.settle(static_cast<int64_t>(reqId), &entry)) {
        return false;
    }
    // 记账里只有一个线程能取出这条请求, 因此这里发出去的就是唯一的那条回执
    emitOutcome(entry, std::move(outcome));
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
    std::vector<pluginxx::ffi::RequestEntry> pending;
    {
        // 与 "判定收尾 + 登记" 互斥: 这里取出之后不会再有人登记进来
        std::lock_guard<std::mutex> lock(stopMutex_);
        pending = registry_.failAll();
    }
    for (const auto& entry : pending) {
        CallOutcome outcome;
        outcome.ok        = false;
        outcome.code      = code;
        outcome.errorName = std::string{errorName};
        outcome.message   = message;
        emitOutcome(entry, std::move(outcome));
    }
    return pending.size();
}

size_t CallRegistry::pendingCount() const {
    return registry_.size();
}

void CallRegistry::stop() {
    {
        std::lock_guard<std::mutex> lock(stopMutex_);
        stopping_.store(true, std::memory_order_release);
    }
    generation_.fetch_add(1, std::memory_order_release);
    if (timer_) {
        asio::post(ioCtx_, [timer = timer_] { timer->cancel(); });
    }
}

void CallRegistry::reschedule() {
    auto self = shared_from_this();
    asio::post(ioCtx_, [self] { self->onTimeout(); });
}

void CallRegistry::armTimer(int64_t nextDeadlineMs) {
    // 已在 io 线程: 直接用最新代次重臂
    const uint64_t gen = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
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
        if (gen != self->generation_.load(std::memory_order_acquire)
            || self->stopping_.load(std::memory_order_acquire)) {
            return;
        }
        self->checkExpired();
    });
}

void CallRegistry::onTimeout() {
    if (stopping_.load(std::memory_order_acquire)) {
        return;
    }
    // 记账里已经到期的会被 nextDeadlineMs 归到"现在", 这里只需要重臂
    const int64_t next = registry_.nextDeadlineMs(nowMs());
    armTimer(next);

    // 到期项就地处理 (armTimer 已保证下一次检查会到来)
    checkExpired();
}

void CallRegistry::checkExpired() {
    const auto expired = registry_.popExpired(nowMs());
    for (const auto& entry : expired) {
        CallOutcome outcome;
        outcome.ok        = false;
        outcome.code      = AGENTXX_FFI_ERR_TIMEOUT;
        outcome.errorName = "timeout";
        outcome.message   = "call timed out before the server replied";
        emitOutcome(entry, std::move(outcome));
        XX_LOGD("[ffi] call #{} timed out", entry.id);
    }
}

} // namespace ffi
} // namespace agentxx
