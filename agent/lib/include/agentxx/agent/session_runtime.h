#pragma once

#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace agentxx {
namespace agent {

class BaseAgent;
class AgentHost;

/// 一个会话的启动壳: 把"进程内两个端点接起来 → 初始化 → 驱动会话 → 关闭"收在一处
///
/// 以前这套装配顺序在本地直连 (TUI/CLI 的模式启动)、FFI 运行时、以及插件将要
/// 做的通道驱动里各写一遍, 七步完全相同:
///
/// ```
/// 1. ChannelAgentIOTransport::makePair(clientEx, agentEx)
/// 2. clientIO->setTransport(client 端)
/// 3. 建 SessionServerAgentIO(agentEx, agent, cfg) + setTransport(server 端)
/// 4. co_spawn(server 端的 transport 接收循环)
/// 5. co_spawn(client 端的 transport 接收循环)
/// 6. co_spawn( 建主程序容器/加载插件 → agent->init() → 登记根 → 拉取启动信息
///              → 通知客户端就绪 → server 端驱动循环 run() )
/// 7. client 端发 hello, 触发同步/历史回放
/// ```
///
/// 三处差异只有"客户端端点是谁"、"历史同步尾窗多大"、"init 进度往哪报"、"agent
/// 是否已经初始化"四点, 全部做成 [Config] 的字段。客户端端点仍按使用方各自实现
/// (终端界面 / FFI 回调 / 插件通道回调), 本类只负责组织装配与关闭顺序。
///
/// 线程: [start] / [stop] 与各控制命令可在任意线程调用, 内部一律投递到对应
/// io 线程, 不等待收尾; 要确认"确实关完了"就轮询 [running] 或订阅自己的
/// 关闭通知 (见 [Config::onStopped])。
class AgentSessionRuntime : public std::enable_shared_from_this<AgentSessionRuntime> {
public:

    struct Config {
        /// 会话 id (两端的 Wire 消息都用它定位会话)
        std::string sessionId;

        /// 客户端 transport 的执行器 (两端接起来的 client 一侧)
        /// - 必填: 客户端端点的接收循环与会话命令都投递到它上面执行
        asio::any_io_executor clientExecutor;

        /// 中断/权限等待客户端应答的超时; <=0 表示不限制
        std::chrono::milliseconds interruptTimeout{0};

        /// delta 重放缓冲容量 (按条数)
        /// - 0 = 不保留重放 (没有重连需求的嵌入式调用方)
        /// - 终端界面按需给 (支持断线重连后补发)
        size_t deltaBufferCap = 0;

        /// 首次接入/切换会话时同步的历史消息窗口大小
        /// - 0 = 完整同步; N > 0 = 只同步末尾 N 条 (终端界面按需分页拉更早的)
        size_t initialSyncTailCount = 0;

        /// `collect` 投递模式的静默合并窗口 (与会话驱动端点默认值一致)
        std::chrono::milliseconds collectWindow{std::chrono::milliseconds{400}};

        /// 是否在就绪后自动拉取启动信息 (插件/MCP/Skill/Memory 清单)
        bool autoRequestComponents = true;

        /// 是否由本类把 agent 的 init 进度转给客户端端点 ([AgentIOBase::onServerProgress])
        /// - 终端界面需要 (启动 banner 逐步展示)
        /// - 自带进度去处 (如 FFI 写日志环形缓冲) 的调用方置 false, 自己设
        ///   [AgentContext::initNotifier]
        bool forwardInitProgress = true;

        /// agent 节点容器 (进程级容器: 共享 ioCtx / 线程池 / 事件总线)
        /// - 空 = 由本类新建一个 (本地直连 / FFI)
        /// - 非空 = 复用它 (插件通道: 复用建根时所在的那一个)
        std::shared_ptr<AgentHost> host;

        /// agent 是否已经初始化
        /// - false = 由本类负责 `init()` 与容器登记 (本地直连 / FFI)
        /// - true  = agent 已就绪 (插件挂载根在建根时已 init 并登记), 本类只驱动会话
        bool skipInit = false;

        /// 是否由本类在容器上加载插件 (create/start 各一次)
        /// - 本地直连: true (主程序代劳, 插件加载完再 init, 各 agent 只挂载)
        /// - FFI: false —— 加载要 co_await 插件的 start/挂载事务, 而 FFI 的 io 由
        ///   嵌入方驱动; 交给 agent 自己在 init 里加载 (与单 agent 模式同一路径)
        bool loadPluginsInHost = true;

        /// 客户端 hello 携带的界面语言
        std::string language = "en";

        /// hello 的断线重放参数 (0 / 空 = 从头同步)
        uint64_t    lastSeq  = 0;
        std::string tailHash;

        /// 客户端接收循环就绪后回调 (client io 线程): 调用方在此发 hello 与
        /// 首个查询 (如当前模型信息)
        std::function<void()> onTransportStarted;

        /// 初始化失败回调 (agent io 线程; 默认记错误日志并停止驱动循环)
        /// - 调用方只做"上报/记录", 不需要自己停端点 (返回后本类会 stop)
        std::function<void(std::string_view)> onInitError;

        /// 服务端就绪回调 (agent io 线程, 在通知客户端端点之前)
        /// - 调用方在此把自己的状态改为"可交互" (如 FFI 的 Ready 状态):
        ///   客户端就绪回调里可能立即发起操作, 此时状态必须已经就位
        std::function<void()> onServerReady;

        /// 驱动循环退出后回调 (agent io 线程, 每条通道的最后一个通知点)
        /// - 关掉通道、回一句"已关闭"这类收尾放这里
        std::function<void()> onStopped;
    };

    /// 装配两个端点 (建传输对、设 transport、建服务端点); 不启动任何协程
    ///
    /// - `agent` 须已构造 (可以尚未 init, 见 [Config::skipInit])
    /// - `clientIO` 是使用方自己的客户端端点, 本类只负责把它接到服务端点
    ///
    /// `return` 装配失败 (参数不全) 返回空
    static std::shared_ptr<AgentSessionRuntime> create(
        std::shared_ptr<BaseAgent>   agent,
        std::shared_ptr<AgentIOBase> clientIO,
        Config                       cfg
    );

    ~AgentSessionRuntime();

    AgentSessionRuntime(const AgentSessionRuntime&)            = delete;
    AgentSessionRuntime& operator=(const AgentSessionRuntime&) = delete;

    /// 启动 (幂等): 拉起两个接收循环与 init/驱动主协程; 结果经端点事件体现
    void start();

    /// 发起关闭 (不等待): 通知客户端端点 → 关 client 传输 → 停服务端点
    /// - 服务端点的驱动循环退出后触发 [Config::onStopped]
    void stop();

    /// 驱动循环是否仍在运行
    bool running() const;

    /// 关闭是否已发起
    bool stopping() const noexcept {
        return stopping_.load(std::memory_order_acquire);
    }

    /// 已发起 start 且尚未走到失败处
    bool started() const noexcept {
        return started_.load(std::memory_order_acquire);
    }

    /// 初始化是否已失败 (失败后 running() 会随之转 false)
    bool failed() const noexcept {
        return failed_.load(std::memory_order_acquire);
    }

    // -----------------------------------------------------------------------
    // 控制命令 (任意线程; 内部投递到 client io 线程后发 wire 消息)
    // -----------------------------------------------------------------------

    /// 投递一条用户输入
    /// - 未指定 sessionId 时补成本通道的会话; requestId 为 0 时自动分配
    ///   (服务端只在 requestId > 0 时回受理回执, 调用方才能确认"已开始/已排队/被拒")
    void sendInput(WireUserInput input);

    /// 请求取消当前轮次
    void cancel();

    /// 请求切换会话模型
    void selectModel(std::string modelName);

    /// 请求当前会话的 LLM 上下文
    void requestContextMessages();

    /// 结束会话 (切到另一个会话由调用方自建新通道, 不在这里换)
    std::shared_ptr<SessionServerAgentIO> serverIO() const {
        return serverIO_;
    }

    std::shared_ptr<AgentIOBase> clientIO() const {
        return clientIO_;
    }

    std::shared_ptr<BaseAgent> agent() const {
        return agent_;
    }

    /// agent 节点容器 (start 之后才有; skipInit 时返回传入的那一个)
    std::shared_ptr<AgentHost> host() const {
        return host_;
    }

    const std::string& sessionId() const noexcept {
        return cfg_.sessionId;
    }

    /// 最近一次分配的输入请求序号 (诊断用)
    uint64_t lastInputRequestId() const noexcept {
        return inputRequestId_.load(std::memory_order_relaxed);
    }

private:

    AgentSessionRuntime(std::shared_ptr<BaseAgent> agent, std::shared_ptr<AgentIOBase> clientIO, Config cfg);

    /// agent io 线程: 装配容器 → init → 登记根 → 就绪通知 → 驱动循环
    asio::awaitable<void> runMain();

    /// 驱动循环结束后的收尾 (置状态 + 回调)
    void finishMain();

    Config cfg_;

    std::shared_ptr<BaseAgent>            agent_;
    std::shared_ptr<AgentIOBase>          clientIO_;
    std::shared_ptr<SessionServerAgentIO> serverIO_;
    std::shared_ptr<AgentHost>            host_;

    /// 输入请求序号 (自动分配; 服务端只在 requestId > 0 时回受理回执)
    std::atomic<uint64_t> inputRequestId_{0};

    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> failed_{false};
};

} // namespace agent
} // namespace agentxx
