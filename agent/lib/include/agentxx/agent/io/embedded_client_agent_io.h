#pragma once

#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "asio/any_io_executor.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/steady_timer.hpp"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace agentxx {
namespace agent {

/// 嵌入式 client 端点基类: 把 agent 产出的事件转交给主程序的回调
///
/// 与 `TUIClientAgentIO` / `StdIOClientAgentIO` 同级, 都是 client 端点; 区别是
/// 这一类端点的"上层"不是终端界面, 而是**嵌入了 libagentxx 的程序** (FFI 的 C 回调、
/// 插件的通道事件回调)。这些上层共同的约束带来同一套实现:
///
/// - **中断应答**: 服务端发来 `WireInterruptRequest` 后, 把询问交给上层, 挂起等
///   上层回话, 再把答案回送服务端; 停止/关闭时把挂起的中断全部失败掉, 避免
///   协程卡死;
/// - **文本增量合批**: 逐 token 回调太密, 可按时间窗口合并成一段;
/// - **线程判定**: 后端点的状态只在 client io 线程读写, 需要判断"我是不是在那条线程"。
///
/// 子类只需实现"怎么把事件交出去" (两个私有虚函数) 和端点特有的消息处理。
///
/// 线程约定: 本类状态 (`pending_` / `sessionId_` / 合批缓冲) 只在 client io 线程
/// 读写; 中断 id 用原子量保存, 任意线程可无锁查询。
class EmbeddedClientAgentIO : public AgentIOBase {
public:

    using RespChannel
        = asio::experimental::concurrent_channel<void(utilxx_base::AsioErrorCode, utilxx_base::Json)>;

    explicit EmbeddedClientAgentIO(asio::any_io_executor ex);

    ~EmbeddedClientAgentIO() override;

    /// 绑定会话 id (端点自身把会话写进事件载荷时使用)
    void setSessionId(std::string sessionId);

    const std::string& sessionId() const noexcept {
        return sessionId_;
    }

    /// client io 线程 id (启动方注入; 用于判断调用方线程)
    void setClientThreadId(std::thread::id tid);

    /// agent io 线程 id (启动方注入; 用于判断调用方线程)
    void setAgentThreadId(std::thread::id tid);

    bool isOnClientThread() const;
    bool isOnAgentThread() const;

    bool isOnAnyIoThread() const {
        return isOnClientThread() || isOnAgentThread();
    }

    /// 注入自身弱引用: 子类构造完成后 (已有 shared_ptr 时) 调用一次
    /// - 基础类内部的异步回调 (合批定时器) 据此保活, 不会在对象销毁后执行
    void initSelf(const std::shared_ptr<EmbeddedClientAgentIO>& self);

    // -----------------------------------------------------------------------
    // AgentIOBase 实现: 嵌入式端点不参与"从自己这里读输入"
    // -----------------------------------------------------------------------

    /// 输入由上层主动投递 (sendToPeer), 不从本端点拉取: 恒返回 nullopt
    asio::awaitable<std::optional<std::string>> getInput() override;

    /// 本端点不注册会话总线 (HIL 走 onPeerMessage → awaitInterruptResponse),
    /// 此实现只满足纯虚契约
    asio::awaitable<utilxx_base::Json> handleInterrupt(
        std::string_view sessionId,
        std::string_view interruptNode,
        std::string_view interruptValue,
        std::string_view interruptArgJson
    ) override;

    // -----------------------------------------------------------------------
    // 中断应答
    // -----------------------------------------------------------------------

    /// 是否仍有指定 id 的中断等待上层应答 (任意线程, 无锁读取)
    bool hasPendingInterrupt(int64_t interruptId) const;

    /// 提交中断应答 (须在 client io 线程): 完成挂起的等待并触发回送
    /// - id 无效 (不存在/已应答/已过期) 返回 false
    /// - 重复应答按首次为准, 后续调用返回 false
    bool submitInterruptResponse(int64_t interruptId, utilxx_base::Json values);

    /// 失败全部挂起的中断并冲刷合批缓冲 (关闭时调用; 幂等)
    /// - 子类可覆写以在下层清理之外补自己的收尾 (如通知上层"这些调用已取消")
    virtual void failAllPendingInterrupts();

    // -----------------------------------------------------------------------
    // 文本增量合批
    // -----------------------------------------------------------------------

    /// 设置合批窗口 (毫秒; <= 0 关闭合批, 逐条交付)
    void setDeltaBatch(int32_t maxDelayMs);

    /// 立即交付积压的合批文本 (client io 线程)
    void flushDeltaBatch();

protected:

    /// 关闭流程开始: 失败挂起中断 + 冲刷合批 (上层不应再有事件到达)
    void onRuntimeStopping() override;

    // -----------------------------------------------------------------------
    // 被动接收回调
    // -----------------------------------------------------------------------

    /// 增量分发: 文本/思考 token 进合批窗口, 其余立即交付
    void onDelta(const agent::WireDelta& delta) override;

    /// 完整/部分同步 (历史消息校准)
    /// - 嵌入式端点的上层通常不展示历史列表, 默认丢弃
    /// - 需要历史 (如插件把会话回显给聊天界面) 时子类覆写
    void onSync(const agent::WireSyncPayload& /*payload*/) override {}

    // -----------------------------------------------------------------------
    // 中断登记 (子类在 onPeerMessage 里按三步协作)
    // -----------------------------------------------------------------------

    /// 登记一条中断并返回等待用的应答通道
    /// - 调用方随后经 [awaitInterruptResponse] 挂起等待, 并在等待前把询问交给上层
    std::shared_ptr<RespChannel> beginInterrupt(int64_t id);

    /// 等待应答通道; 收到就回送 `WireInterruptResponse` (子类 co_spawn 调用)
    /// - 等待被关闭 (过期/停止) 时静默结束, 不回送响应
    /// - 不捕获自身, 由子类在 co_spawn 时持有 shared_ptr 保活
    asio::awaitable<void> awaitInterruptResponse(int64_t id, std::shared_ptr<RespChannel> ch);

    /// 记录"当前挂起的中断 id" (0 = 无; 供任意线程无锁查询)
    void setCurrentPendingInterrupt(int64_t id);

    /// 对端通知中断过期 (超时/取消): 关闭通道并摘除记录
    void expireInterrupt(int64_t id);

    /// 在 client io 线程执行一段动作 (已在则直接执行)
    void postToClientThread(std::function<void()> fn);

    asio::any_io_executor ex_;

    /// 本端点绑定的会话 id
    std::string sessionId_;

    /// client io 线程 id 与 agent io 线程 id (判调用方线程)
    std::thread::id clientThreadId_;
    std::thread::id agentThreadId_;

private:

    /// 交付一段合批后的增量文本 (kind = "text" / "thinking")
    virtual void emitDeltaText(std::string_view kind, std::string_view text) = 0;

    /// 交付一条非合批的增量 (工具开始/结束、轮次边界等)
    virtual void emitDeltaEvent(const agent::WireDelta& delta) = 0;

    /// 当前挂起等待的中断 ID (0 表示无; 原子量, 跨线程无锁查询)
    std::atomic<int64_t> currentPendingInterruptId_{0};

    /// 挂起的中断应答通道 (wire id → channel; 仅 client io 线程访问)
    std::map<int64_t, std::shared_ptr<RespChannel>> pending_;

    /// 合批窗口毫秒数 (<=0 = 关闭合批)
    int32_t deltaBatchMs_ = 0;
    /// 合批到期定时器 (首次开启合批时创建)
    std::shared_ptr<asio::steady_timer> deltaBatchTimer_;
    /// 积压的增量文本与类别 (类别变化时先冲刷再累积)
    std::string pendingDeltaText_;
    std::string pendingDeltaKind_;

    /// 自身弱引用 (异步回调保活; 由 [initSelf] 注入)
    std::weak_ptr<EmbeddedClientAgentIO> self_;
};

} // namespace agent
} // namespace agentxx
