#pragma once

#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/cancellation_signal.hpp"
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace asio = ::boost::asio;

namespace agentxx {
namespace util {

/// 后台任务登记表 (计划 ARC-5)
///
/// 用途: agent 自己发起的"短后台任务"(事件发布、进度通知一类 fire-and-forget 协程)
/// 统一登记到一张表, 关闭时先取消再等待它们收敛, 避免进程退出时后台协程还在跑。
///
/// 边界 (刻意收窄):
/// - **不接管当前轮次**: 会话轮次是受控调用 (调用方 await), 关闭时不等待也不取消,
///   否则退出时间会被长工具拖住 (见 plan.md ARC-5 的核定结论);
/// - **不接管插件任务**: 插件自己的线程/定时器由插件 `stop` 负责 (PluginManager
///   的生命周期骨架已经处理);
/// - 只记录"发起后不需要返回值"的任务, 有返回值的工作请直接 `co_await`。
///
/// 线程约定: 只在所属 io 线程调用 (与 Session/middleware 的无锁模型一致)。
class TaskScope {
public:

    explicit TaskScope(asio::any_io_executor executor) :
        executor_(std::move(executor)) {}

    TaskScope(const TaskScope&)            = delete;
    TaskScope& operator=(const TaskScope&) = delete;

    /// 登记并启动一个后台任务 (发起即返回, 不等待完成)
    /// - `name` 只用于诊断与关闭日志 (如 "event-publish" / "host-progress")
    /// - 取消信号在关闭时发送: 任务内挂起的 asio 异步操作会以 operation_aborted
    ///   结束; 任务自身如需特殊清理, 应捕获该错误码
    void spawn(std::string name, asio::awaitable<void> task);

    /// 仍在运行的任务数
    size_t pending() const noexcept {
        return entries_.size();
    }

    /// 仍在运行的任务名 (诊断用)
    std::vector<std::string> pendingNames() const;

    /// 累计发起任务数 (诊断/测试用)
    uint64_t totalSpawned() const noexcept {
        return totalSpawned_;
    }

    /// 向全部运行中的任务发送取消信号; 返回发送条数
    size_t cancelAll();

    /// 等待任务收敛 (无任务时立即返回 true; 超时返回 false)
    asio::awaitable<bool> awaitIdle(std::chrono::milliseconds timeout);

private:

    /// 任务完成时摘除登记项 (由 co_spawn 的完成处理器调用)
    void onDone(uint64_t id);

    struct Entry {
        std::string                                name;
        std::shared_ptr<asio::cancellation_signal> signal;
    };

    asio::any_io_executor     executor_;
    std::map<uint64_t, Entry> entries_;
    uint64_t                  nextId_       = 0;
    uint64_t                  totalSpawned_ = 0;
};

} // namespace util
} // namespace agentxx
