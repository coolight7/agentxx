// event_queue.cpp —— FFI 异步安全事件队列实现
//
// 背景见 ffi_api.h "异步安全事件队列" 注释: on_event 在内部 client-io 线程
// 同步回调且 payload 仅回调期间有效, 无法同步拷贝 payload 的宿主语言运行时
// (Dart NativeCallable.listener 等) 经本桥接安全接收事件。
//
// 线程模型:
// - 入队 (agentxx_event_queue_on_event): 任意线程 (实际为 client-io 线程),
//   仅做字符串拷贝 + mutex/deque 操作, 不阻塞 io 调度
// - 出队 (agentxx_event_queue_pop): 宿主线程, condition_variable 有界等待
// - free: 唤醒全部等待者并置 closed, 等待者以 AGENTXX_ERR_STATE 返回

#include "agentxx/ffi_api.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

struct AgentxxEventQueue {
    /// 队列积压上限 (超限丢最旧): 流式 delta 高频场景下宿主停轮询的兜底,
    /// 防止无界增长 OOM; 正常 CLI/GUI 宿主轮询间隔 << 1s, 远达不到该量级
    static constexpr size_t kMaxQueued = 16384;

    std::mutex              m;
    std::condition_variable cv;
    std::deque<std::pair<int32_t, std::string>> items;
    /// 正在 pop 内部 (持锁或即将持锁) 的等待者计数: free 时等其归零再 delete,
    /// 避免 "notify 后立即 delete" 与被唤醒者重新上锁之间的 use-after-free
    std::atomic<int>        waiters{0};
    bool                    closed = false;
};

extern "C" {

AgentxxEventQueue* agentxx_event_queue_create(void) {
    try {
        return new AgentxxEventQueue{};
    } catch (...) {
        return nullptr;
    }
}

void agentxx_event_queue_free(AgentxxEventQueue* q) {
    if (q == nullptr) {
        return;
    }
    // closed 置位与 notify_all 必须持锁完成:
    // - 保证与并发 on_event/pop 的加锁顺序互斥, 不会出现 "对方刚检查完
    //   closed==false 还没来得及 notify, 本函数就 delete" 的夹缝 UAF
    // - 唤醒全部在途 pop (其以 ERR_STATE 返回)
    {
        std::lock_guard<std::mutex> lock(q->m);
        q->closed = true;
        // 积压事件随对象析构释放 (std::string RAII)
        decltype(q->items){}.swap(q->items);
        q->cv.notify_all();
    }
    // 等待全部在途 pop 退出后再释放对象: 被唤醒者可能尚未重新上锁检查 closed。
    // 最坏自旋时长 = 单次 pop 的 timeout_ms (pop 恒有界返回, 不会死等)。
    // 注意: 本函数返回后句柄立即失效, 调用方须保证其他线程不再以该句柄调用
    // 任何 event_queue API (裸指针无法防御 free 之后新进入的调用)。
    while (q->waiters.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    delete q;
}

void agentxx_event_queue_on_event(AgentxxEventType type, const char* payload_json, void* user_data) {
    auto* q = static_cast<AgentxxEventQueue*>(user_data);
    if (q == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(q->m);
        if (q->closed) {
            return; // 队列已销毁/正在销毁: 丢弃 (宿主已停止消费)
        }
        if (q->items.size() >= AgentxxEventQueue::kMaxQueued) {
            // 超限丢最旧, 并补发一条错误提示让宿主可感知丢失
            q->items.pop_front();
            q->items.emplace_back(
                static_cast<int32_t>(AGENTXX_EVT_ERROR),
                std::string(R"({"code":-99,"message":"event queue overflow, oldest events dropped"})")
            );
        }
        try {
            q->items.emplace_back(static_cast<int32_t>(type),
                                  payload_json == nullptr ? std::string{} : std::string(payload_json));
        } catch (...) {
            return; // OOM 等异常: 丢弃本条, 保证 io 线程不受影响
        }
        // notify 必须持锁: 解锁后再 notify 会与 free 的 "上锁置 closed →
        // 解锁 → 自旋等 waiters → delete" 形成夹缝, 访问已析构的 cv (UAF)
        q->cv.notify_one();
    }
}

int agentxx_event_queue_pop(AgentxxEventQueue* q, int32_t* type_out, char** json_out, uint32_t timeout_ms) {
    if (json_out != nullptr) {
        *json_out = nullptr;
    }
    if (q == nullptr || json_out == nullptr || type_out == nullptr) {
        return AGENTXX_ERR_INVALID;
    }

    // 在途标记 (free 等待其归零); RAII 保证异常路径也递减
    struct WaiterGuard {
        AgentxxEventQueue* q;

        ~WaiterGuard() {
            q->waiters.fetch_sub(1, std::memory_order_release);
        }
    } guard{q};
    q->waiters.fetch_add(1, std::memory_order_acq_rel);

    std::pair<int32_t, std::string> item;
    {
        std::unique_lock<std::mutex> lock(q->m);
        if (!q->cv.wait_for(lock, std::chrono::milliseconds{timeout_ms}, [&] {
                return q->closed || !q->items.empty();
            })) {
            return AGENTXX_ERR_TIMEOUT; // 等待超时且无事件
        }
        if (q->closed && q->items.empty()) {
            return AGENTXX_ERR_STATE; // 队列已销毁
        }
        item = std::move(q->items.front());
        q->items.pop_front();
    }

    *type_out = item.first;

    // payload 拷出为独立分配的 NUL 结尾 UTF-8 字符串 (统一经 agentxx_malloc
    // 分配通道, 宿主用后 agentxx_free 释放)
    char* out = nullptr;
    try {
        const size_t n = item.second.size();
        out = static_cast<char*>(agentxx_malloc(n + 1));
        if (out == nullptr) {
            return AGENTXX_ERR_OOM;
        }
        if (n > 0) {
            std::memcpy(out, item.second.data(), n);
        }
        out[n] = '\0';
    } catch (...) {
        return AGENTXX_ERR_INTERNAL;
    }
    *json_out = out;
    return AGENTXX_OK;
}

} // extern "C"
