#pragma once

#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "utilxx_base/json.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace asio = ::boost::asio;

namespace agentxx {
namespace ffi {

struct FfiOpDef;

/// 一次调用的结局 (成功结果 / 失败码与原因)
struct CallOutcome {
    bool ok = false;
    /// 失败码 (`AGENTXX_FFI_ERR_*`); 0 = 未指定, 由 op 定义里登记的失败码补齐
    int32_t code = 0;
    /// 失败的稳定名字 (`invalid_argument` / `timeout` / `canceled` / `rejected` ...)
    std::string errorName;
    /// 给人看的失败说明
    std::string message;
    /// 成功结果 (按 op 约定; 无结果的 op 为空对象)
    utilxx_base::Json result;
};

/// 待完成调用表 (提交 + 回执模型的核心记账)
///
/// 线程模型:
/// - `begin` / `abort` / `settle` / `cancel` / `failAll` 可从任意线程调用 (mutex 保护)
/// - 超时靠**一个共享定时器** + 按到期时间排序的索引, 定时器跑在注入的 io_context 上
/// - 回执由注入的 [EmitFn] 投递, 调用方自己决定在哪个线程真正回调 (本类不假设线程)
///
/// 保证:
/// - 每个受理成功的 reqId **恰好一条回执**: 服务端应答、取消、超时、收尾都走
///   [settle]; 谁先到谁赢, 后到的返回 false 并丢弃
/// - "同步参数校验失败" 不进表 (见 [abort]), 因此也不会有回执
class CallRegistry : public std::enable_shared_from_this<CallRegistry> {
public:

    /// 回执投递: `(reqId, op 名, 结局)`; op 名在表里查不到时为空串
    using EmitFn = std::function<void(uint64_t reqId, std::string_view op, const CallOutcome&)>;

    /// - `args`:
    ///     - [ioCtx] 超时定时器与 [EmitFn] 的执行器 (运行期的 client io)
    ///     - [emit]  回执投递 (由运行期提供)
    CallRegistry(asio::io_context& ioCtx, EmitFn emit);

    ~CallRegistry();

    CallRegistry(const CallRegistry&)            = delete;
    CallRegistry& operator=(const CallRegistry&) = delete;

    /// 登记一次调用, 返回 reqId
    /// - `timeoutMs > 0` 才登记超时 (`0` = 不限; 本地 op 传 0)
    uint64_t begin(const FfiOpDef& def, int32_t timeoutMs);

    /// 撤销登记 (同步失败: 不发回执)
    void abort(uint64_t reqId);

    /// 结算一次调用
    /// - 返回 true = 由本次负责回执 (failureCode 已按 op 定义补齐); false = 早已结算
    bool settle(uint64_t reqId, CallOutcome outcome);

    /// 按"调用方取消"结算; 返回 false 表示 reqId 不在表里 (已完成/从未发出)
    bool cancel(uint64_t reqId);

    /// 收尾: 把仍在等待的调用全部按取消结算, 返回处理条数
    size_t failAll(int32_t code, std::string_view errorName, std::string message);

    /// 当前未完成的调用条数
    size_t pendingCount() const;

    /// 停止: 不再排定新的超时检查 (收尾后调用; 已排定的检查会自行退出)
    void stop();

private:

    struct Entry {
        const FfiOpDef*   def        = nullptr;
        int64_t           deadlineMs = 0; ///< 到期时刻 (steady 毫秒); 0 = 不限
        std::atomic<bool> settled{false};
    };

    /// 当前毫秒 (steady_clock, 只用于比较)
    static int64_t nowMs();

    /// 排定下一次超时检查 (任意线程; 内部 post 到 io 线程完成定时器操作)
    void reschedule();

    /// io 线程: 按当前最早的到期时刻重臂定时器
    void armTimerLocked(int64_t nextDeadlineMs);

    /// io 线程: 把已到期的条目按超时结算
    void checkExpired();

    /// io 线程: 重臂定时器后立即检查一次
    void onTimeout();

    asio::io_context& ioCtx_;
    EmitFn            emit_;

    mutable std::mutex mutex_;
    /// reqId → 待完成记录 (reqId 稠密自增, 用 unordered_map)
    std::unordered_map<uint64_t, std::shared_ptr<Entry>> entries_;
    /// 到期时刻 → reqId (有序索引; 需要按 reqId 摘除, 故不用堆)
    std::multimap<int64_t, uint64_t> deadlines_;
    uint64_t                         nextReqId_ = 0;
    /// 收尾标记: 置位后不再排定新的超时检查
    bool stopping_ = false;

    /// 定时器代次: 每次重臂 +1, 旧的回调发现自己过期就直接退出
    uint64_t generation_ = 0;

    std::shared_ptr<asio::steady_timer> timer_;
};

} // namespace ffi
} // namespace agentxx
