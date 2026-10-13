/// 后台任务登记表实现 (见 agentxx/util/task_scope.h)
#include "agentxx/util/task_scope.h"

#include "agentxx/util/exception.h"
#include "asio/co_spawn.hpp"
#include "asio/bind_cancellation_slot.hpp"
#include "asio/detached.hpp"
#include "asio/steady_timer.hpp"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/log.h"

namespace agentxx {
namespace util {

void TaskScope::spawn(std::string name, std::function<asio::awaitable<void>()> task) {
    const uint64_t id = ++nextId_;
    auto           signal = std::make_shared<asio::cancellation_signal>();
    entries_[id] = Entry{.name = std::move(name), .signal = signal};
    ++totalSpawned_;

    // 取消信号绑定到任务: 关闭时先发信号, 任务内部的 asio 等待尽快结束;
    // 完成处理器负责摘除登记项 (无论正常结束、异常还是被取消)
    // 注意: 传**可调用对象**给 co_spawn (而不是先调用它拿 awaitable), co_spawn 会把
    // 闭包复制进自己的协程帧, 闭包生命周期与任务一致 —— 内联调用 lambda 协程时帧只
    // 保存临时闭包指针, 语句结束即悬垂 (见 task_scope.h 的 spawn 说明)
    asio::co_spawn(
        executor_,
        std::move(task),
        asio::bind_cancellation_slot(
            signal->slot(),
            [this, id](std::exception_ptr ep) {
                if (ep) {
                    // 后台任务异常不能逃逸到 detached 处理器 (会终止进程):
                    // 在这里统一分类记录, 取消按取消语义处理
                    agentxx::util::catchError<bool>(
                        [&]() -> bool {
                            std::rethrow_exception(ep);
                            return true;
                        },
                        [](std::string errmsg) -> bool {
                            XX_LOGD("background task failed: {}", errmsg);
                            return false;
                        }
                    );
                }
                onDone(id);
            }
        )
    );
}

std::vector<std::string> TaskScope::pendingNames() const {
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) {
        (void)id;
        names.push_back(entry.name);
    }
    return names;
}

size_t TaskScope::cancelAll() {
    size_t sent = 0;
    for (auto& [id, entry] : entries_) {
        (void)id;
        if (entry.signal) {
            entry.signal->emit(asio::cancellation_type::all);
            ++sent;
        }
    }
    return sent;
}

asio::awaitable<bool> TaskScope::awaitIdle(std::chrono::milliseconds timeout) {
    if (entries_.empty()) {
        co_return true;
    }
    const auto ex       = co_await asio::this_coro::executor;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!entries_.empty()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return false;
        }
        asio::steady_timer timer{ex};
        timer.expires_after(std::chrono::milliseconds{2});
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return true;
}

void TaskScope::onDone(uint64_t id) {
    entries_.erase(id);
}

} // namespace util
} // namespace agentxx
