#pragma once

#include "agentxx/agent/io/agent_io_transport.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "utilxx_base/asio_error.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace agentxx {
namespace agent {
namespace io {

/// 单行 JSONL 文本的长度上限 (超出按非法行处理, 避免畸形输入占用无限内存)
/// - 附件 (Base64) 走同行内联, 因此上限取得比常见请求大得多
inline constexpr size_t kJsonlMaxLineBytes = 64u * 1024u * 1024u;

/// 一行 JSONL 的解码结果 (计划 PRO-8)
struct JsonlFrame {
    /// 解析出的消息; 为空表示该行没有产出消息 (空行被跳过, 或 `error` 非空)
    std::optional<WireMessage> message;
    /// 拒绝原因 (给对端看的可读文本; 空 = 该行被正常处理或按空行跳过)
    std::string error;
};

/// 解码一行 JSONL 文本为 Wire 消息
///
/// - 自动去掉首尾空白与行尾 `\r` (Windows 文本管道的换行是 `\r\n`)
/// - 空行 / 纯空白行: 返回空 frame (`message` 与 `error` 都为空), 调用方直接跳过
/// - 非法 JSON、根节点不是对象、缺 `type`、`type` 不认识、超出长度上限:
///   返回 `error` (可直接回给对端), 不抛异常
JsonlFrame jsonlDecodeLine(std::string_view line);

/// 编码一条 Wire 消息为一行文本 (含结尾 `'\n'`)
/// - 复用 WebSocket 传输的同一份序列化实现 (`agentxx::agent::io::serialize`),
///   两种传输的报文 JSON 完全一致, 只是分帧方式不同
/// - JSON 字符串内的换行已被转义, 因此一条消息一定在同一行内
std::string jsonlEncodeMessage(const WireMessage& msg);

} // namespace io

/// stdio JSONL 传输 (计划 PRO-8: 一次性运行 / 脚本驱动的进程外客户端)
///
/// 用途: 让外部脚本或其它语言的进程直接经 stdin/stdout 驱动一个 agent 会话,
/// 不需要 WebSocket 服务, 也不需要写 C++ 客户端。协议与 `server` 模式相同
/// (同一份 `WireMessage` 定义), 只把分帧方式换成"一行一条 JSON"。
///
/// 数据流:
/// ```
///   外部进程 --(stdin: 一行一条 client→server 消息)--> 本传输 --recv()--> 服务端点
///   外部进程 <--(stdout: 一行一条 server→client 消息)-- 本传输 <--send()-- 服务端点
/// ```
///
/// 约定:
/// - **stdout 只写协议行** (每行一条消息, 写后立即 flush); 日志/诊断一律走日志系统
///   (宿主默认 sink 为 stderr), 保证 stdout 能被逐行解析
/// - **收到请求回执不等于轮次完成**: `user_input` 的受理回执 ([WireInputAck]) 只表示
///   "已受理", 轮次结束以 `turn_result` 为准; 调用方要等 `turn_result` 再收尾
/// - 非法行不中断会话: 本端回一条 `WireError`(`InvalidArgs`) 后继续读下一行
/// - 起始连接阶段为 [WireConnectionStage::Unhandshaken]: 端点只接受 `hello`,
///   握手成功后经 `setStage` 推进到 [WireConnectionStage::Ready]
/// - **输入结束与关闭分开**: 输入 (stdin) EOF 之后 [recv] 返回 nullopt, 表示"不会再有
///   新输入", 但 [send] 仍然把消息写出去 (对端关掉写入端不等于不想读结果);
///   需要两个方向都停才调用 [close]
///
/// 实现说明: 默认从 stdin 读行需要阻塞等待, 因此用一个后台线程读行并投递到
/// asio 通道 (`recv()` 在 io 线程上异步等待), 与 `client` 侧的 StdinReader 同一做法;
/// 线程只持有共享状态 (通道 + 停止标记), 不持有本对象, 因此析构后线程不会访问已
/// 释放的对象 (线程在下次读到行或 EOF 时自行退出, 与本对象生命周期无关)
class JsonlAgentIOTransport : public AgentIOTransportBase {
public:

    /// 行读写注入点 (测试用内存缓冲; 两项都缺省时: 读 stdin / 写 stdout)
    struct LineIo {
        /// 读取下一行; 返回 false 表示输入结束 (EOF)
        /// - 该回调在 io 线程上同步调用 (注入的读取器必须立即返回, 不能长时间阻塞)
        std::function<bool(std::string& line)>     readLine;
        /// 写出**一整行** (含结尾换行符, 见 [jsonlEncodeMessage]); 实现负责 flush
        std::function<void(std::string_view line)> writeLine;
    };

    JsonlAgentIOTransport(asio::any_io_executor ex, LineIo io = {});
    ~JsonlAgentIOTransport() override;

    void send(WireMessage msg) override;

    asio::awaitable<std::optional<WireMessage>> recv() override;

    void close() override;

    bool alive() const noexcept override;

    /// 输入是否已结束 (stdin EOF 或注入的读取器耗尽)
    /// - 一次性运行模式据此判断"不再有新输入", 开始等待轮次与队列收尾;
    ///   此时 [alive] 仍为 true, [send] 照常写出 (把结果给对端读完)
    bool inputEnded() const noexcept {
        return inputEnded_.load(std::memory_order_acquire);
    }

    WireConnectionStage stage() const noexcept override {
        return stage_.load(std::memory_order_acquire);
    }

    void setStage(WireConnectionStage stage, std::string_view reason) override;

    /// 统计 (诊断与测试): 已读行数 / 已写行数 / 被拒绝的非法行数
    struct Stats {
        uint64_t linesRead   = 0; ///< 从输入读到的非空行数 (含非法行)
        uint64_t linesWritten = 0; ///< 成功写出的协议行数
        uint64_t invalidLines = 0; ///< 被拒绝的行数 (已回 WireError)
    };

    Stats stats() const noexcept;

private:

    using LineChannel
        = asio::experimental::concurrent_channel<void(utilxx_base::AsioErrorCode, std::string)>;

    /// 读行线程的共享状态 (线程只捕获它, 不捕获本对象)
    struct ReaderState {
        std::atomic<bool>            stop{false};
        std::shared_ptr<LineChannel> lines;
    };

    /// 写出一行 (加锁 + flush; 行文本自身含换行); 返回是否写出
    bool writeLine(std::string_view line);

    /// 回一条 `WireError` 给对端 (非法行处理)
    void reportInvalidLine(std::string_view reason);

    /// 启动后台读行线程 (仅默认 stdin 输入; 幂等)
    void startStdioReader();

    /// 从注入的读取器同步取一行; 无输入返回 nullopt
    std::optional<std::string> pollInjectedLine();

    asio::any_io_executor        ex_;
    LineIo                       io_;
    std::shared_ptr<ReaderState> reader_;
    std::thread                  readerThread_;
    std::atomic<bool>            closed_{false};
    std::atomic<bool>            inputEnded_{false};
    std::mutex                   writeMu_;
    std::atomic<WireConnectionStage> stage_{WireConnectionStage::Unhandshaken};
    std::atomic<uint64_t>        linesRead_{0};
    std::atomic<uint64_t>        linesWritten_{0};
    std::atomic<uint64_t>        invalidLines_{0};
};

} // namespace agent
} // namespace agentxx
