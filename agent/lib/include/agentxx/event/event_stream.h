#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/event/events.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/container_util.h"
#include "agentxx/util/exception.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/experimental/channel.hpp"
#include "asio/experimental/channel_traits.hpp"
#include "asio/io_context.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "neograph/graph/types.h"
#include "neograph/types.h"
#include <any>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace agentxx {
namespace event {

/// 仅用作 EventStream<> 模板参数的类型擦除基类
/// - 所有具体事件流都是 EventStream<T> 或 RequestResponseStream<TReq,TResp>
/// - EventBus 只持有 shared_ptr<EventStreamInterface> 以做类型无关的注册表
/// - 存储 elementType_ (typeid) 供 EventBus::get/getRR 校验类型一致性, 防 UB
class EventStreamInterface {
public:

    std::string           name;
    const std::type_info& elementType_;

    EventStreamInterface(std::string_view in_name, const std::type_info& elementType);
    virtual ~EventStreamInterface() = default;
};

/// 单个订阅者: 收到 data 后执行 handle
/// - execHit: 剩余触发次数; >=1 为有限订阅, ==0 为常驻订阅
template<typename _DATA_TYPE>
struct EventSubscription {
    size_t                                                  id      = 0;
    int                                                     execHit = 0; // 0 = 常驻
    std::function<asio::awaitable<void>(const _DATA_TYPE&)> handle;
};

/// 单向强类型事件流: publish -> 顺序派发到每个订阅者
template<typename _DATA_TYPE>
class EventStream : public EventStreamInterface {
public:

    using DataType = _DATA_TYPE;
    using Handler  = std::function<asio::awaitable<void>(const _DATA_TYPE&)>;

private:

    size_t                                          insertId_ = 0;
    std::map<size_t, EventSubscription<_DATA_TYPE>> listeners_{};

public:

    EventStream(std::string_view in_name) :
        EventStreamInterface(in_name, typeid(_DATA_TYPE)) {}

    /// 订阅事件; execHit==0 表示常驻订阅, >0 表示触发 N 次后自动移除
    /// 返回订阅 id, 供 unsubscribe 使用
    size_t subscribe(Handler handle, int execHit = 0) {
        assert(handle);
        assert(execHit >= 0);
        auto id        = ++insertId_;
        listeners_[id] = EventSubscription<_DATA_TYPE>{
            .id      = id,
            .execHit = execHit,
            .handle  = std::move(handle)
        };
        return id;
    }

    bool unsubscribe(size_t id) {
        auto it = listeners_.find(id);
        if (it == listeners_.end()) {
            return false;
        }
        listeners_.erase(it);
        return true;
    }

    /// 当前是否有订阅者
    /// - 供高频发布方 (如 EventBridge 每 token 一次) 在 co_spawn/构造事件前跳过空流,
    ///   避免无消费者的协程创建与事件构造开销
    bool hasListeners() const noexcept {
        return !listeners_.empty();
    }

    /// 发布事件: 顺序派发到每个订阅者
    /// - 单 io_context 协作式调度, 顺序派发在每个 co_await 挂起点让出,
    ///   足够公平且语义清晰; 真并发由 RequestResponseStream 的 co_spawn 承担
    /// - 单个订阅者异常被捕获并记录, 不中断其他订阅者
    /// - execHit>0 的有限订阅: 递减后 <=0 在派发后移除; 常驻 (execHit==0) 不动
    asio::awaitable<void> publish(const _DATA_TYPE& data) {
        if (listeners_.empty()) {
            co_return;
        }
        // 暂存当前订阅者, 避免派发过程中 map 迭代器失效
        auto snapshot = std::vector<EventSubscription<_DATA_TYPE>>{};
        snapshot.reserve(listeners_.size());
        for (auto it = listeners_.begin(); it != listeners_.end();) {
            snapshot.push_back(it->second);
            if (it->second.execHit > 0) {
                --it->second.execHit;
                if (it->second.execHit <= 0) {
                    it = listeners_.erase(it);
                    continue;
                }
            }
            ++it;
        }

        for (auto& sub : snapshot) {
            co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {
                    co_await sub.handle(data);
                    co_return true;
                },
                [&](std::string errinfo) -> asio::awaitable<bool> {
                    XX_LOGE("EventStream `{}` listener exception: {}", name, errinfo);
                    co_return false;
                }
            );
        }
    }
};

/// 请求-响应事件流: 用于 HIL (interrupt/permission) 与 subagent 委派
/// - request() 在调用协程内挂起, 直到 registerServer 端 respond 或超时
/// - correlationId 关联 request 与 response
/// - 每个待响应请求持有一个 channel<TResp> 作为结果槽
template<typename _REQ_TYPE, typename _RESP_TYPE>
class RequestResponseStream : public EventStreamInterface {
public:

    using ReqType  = _REQ_TYPE;
    using RespType = _RESP_TYPE;
    using ServerHandler
        = std::function<asio::awaitable<RespType>(const ReqType& req, size_t correlationId)>;

private:

    std::atomic_size_t              correlationSeq_{0};
    std::atomic_size_t              serverId_{0};
    std::map<size_t, ServerHandler> servers_{}; // <serverId, handler>

public:

    RequestResponseStream(std::string_view in_name) :
        EventStreamInterface(in_name, typeid(std::pair<_REQ_TYPE, _RESP_TYPE>)) {}

    /// 注册服务端处理者; 返回 serverId
    /// - 同一 topic 可多 server 注册, request 轮询派发
    size_t registerServer(ServerHandler handler) {
        assert(handler);
        auto id      = ++serverId_;
        servers_[id] = std::move(handler);
        return id;
    }

    bool unregisterServer(size_t serverId) {
        auto it = servers_.find(serverId);
        if (it == servers_.end()) {
            return false;
        }
        servers_.erase(it);
        return true;
    }

    /// 当前注册的 server 数量 (主要用于测试: 验证重注册不累积 handler)
    size_t serverCount() const {
        return servers_.size();
    }

    /// 发起请求并等待响应
    /// - timeout 到期返回 unexpected, timeout <= 0 表示不限制 (无限等待, 默认)
    /// - 同一 io_context 单线程运行, servers_ 无需加锁
    /// - return (resp/error)
    asio::awaitable<std::expected<RespType, std::string>>
        request(ReqType req, std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) {
        if (servers_.empty()) {
            co_return std::unexpected{
                fmt::format("RequestResponseStream `{}` request: no server registered", name)
            };
        }

        auto correlationId = ++correlationSeq_;
        auto serverIt      = servers_.begin();
        auto handler       = serverIt->second;

        auto out = co_await agentxx::util::catchErrorToUnexpectedAsync<RespType>(
            [&]() -> asio::awaitable<std::expected<RespType, std::string>> {
                auto result = co_await agentxx::util::asyncWithTimeout<RespType>(
                    [&]() -> asio::awaitable<RespType> {
                        co_return co_await handler(req, correlationId);
                    },
                    timeout
                );
                co_return std::expected<RespType, std::string>{std::move(result)};
            }
        );
        if (false == out.has_value()) {
            XX_LOGE("RequestResponseStream `{}` request await failed: {}", name, out.error());
        }
        co_return out;
    }
};

/// 定时器事件流: 订阅时指定延迟, 延迟到点后触发一次
/// - 注意: 可能为临时对象 (EventBus::timer<T>() 返回值), 协程不捕获 this
template<typename _DATA_TYPE>
class TimerEventStream {
public:

    using Handler = std::function<asio::awaitable<void>(const _DATA_TYPE&)>;

private:

    asio::any_io_executor executor_;
    std::string           name_;
    std::atomic_size_t    timerSeq_{0};

public:

    explicit TimerEventStream(asio::any_io_executor ex, std::string name = "TimerEventStream") :
        executor_(ex),
        name_(std::move(name)) {}

    /// 注册一次性定时器: delay 后触发 handler(data); 返回定时器 id
    /// - 在 executor_ 上 co_spawn 独立协程, 不阻塞调用者
    /// - 注意: TimerEventStream 可能为临时对象, 协程不捕获 this,
    ///   仅捕获 executor 副本与日志用的 name
    /// - 额外支持 weak 捕获: 若 handler 捕获 weak_ptr<Session/Bus> 等生命周期敏感对象,
    ///   协程内会先 lock(), 失败则静默跳过, 避免 detached 协程悬空
    /// - 支持两类回调: void()/awaitable<void>(), 前者同步执行更高效
    size_t once(std::chrono::milliseconds delay, Handler handle, _DATA_TYPE data = _DATA_TYPE{}) {
        assert(handle);
        auto id         = ++timerSeq_;
        auto ex         = executor_;
        auto streamName = name_;
        // 包装 handler 为 weak-safe: 若 handle 内含 weak_ptr, 由调用方在 lambda 内 lock 校验
        // 此处提供统一的异常隔离, 确保 detached 协程不会因外部对象析构而崩溃
        asio::co_spawn(
            ex,
            [id, delay, handle = std::move(handle), data = std::move(data), streamName](
            ) -> asio::awaitable<void> {
                auto timer = asio::steady_timer(co_await asio::this_coro::executor, delay);
                // 定时器取消不抛异常, 仅返回 error_code, 此处忽略
                neograph_asio_error_code ec;
                co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                if (ec) {
                    co_return;
                }
                co_await agentxx::util::catchErrorAsync<bool>(
                    [&]() -> asio::awaitable<bool> {
                        co_await handle(data);
                        co_return true;
                    },
                    [&](std::string errmsg) -> asio::awaitable<bool> {
                        XX_LOGE(
                            "TimerEventStream `{}` id={} exception: {}",
                            streamName,
                            id,
                            errmsg
                        );
                        co_return false;
                    }
                );
            },
            asio::detached
        );
        return id;
    }

    /// weak-safe 便捷重载: handler 若捕获 weak_ptr, 在定时器触发时先尝试 lock, 失败则跳过
    template<typename T>
    size_t onceWeak(
        std::chrono::milliseconds                                                   delay,
        std::weak_ptr<T>                                                            weak,
        std::function<asio::awaitable<void>(std::shared_ptr<T>, const _DATA_TYPE&)> handler,
        _DATA_TYPE data = _DATA_TYPE{}
    ) {
        assert(handler);
        auto id         = ++timerSeq_;
        auto ex         = executor_;
        auto streamName = name_;
        asio::co_spawn(
            ex,
            [id, delay, weak, handler = std::move(handler), data = std::move(data), streamName](
            ) -> asio::awaitable<void> {
                auto timer = asio::steady_timer(co_await asio::this_coro::executor, delay);
                neograph_asio_error_code ec;
                co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                if (ec) {
                    co_return;
                }
                auto locked = weak.lock();
                if (!locked) {
                    co_return;
                }
                co_await agentxx::util::catchErrorAsync<bool>(
                    [&]() -> asio::awaitable<bool> {
                        co_await handler(std::move(locked), data);
                        co_return true;
                    },
                    [&](std::string errmsg) -> asio::awaitable<bool> {
                        XX_LOGE(
                            "TimerEventStream `{}` id={} exception: {}",
                            streamName,
                            id,
                            errmsg
                        );
                        co_return false;
                    }
                );
            },
            asio::detached
        );
        return id;
    }
};

/// EventBus: 按 topic 名注册/查找类型擦除的事件流
/// - 模块只依赖 EventBus + 事件类型头, 不互相可见
/// - 单 io_context 单线程运行, 内部表无需加锁
class EventBus {
public:

    explicit EventBus(asio::any_io_executor executor);

    asio::any_io_executor executor() const {
        return executor_;
    }

    /// 获取或创建一个单向事件流; 同 topic 同类型复用
    template<typename _DATA_TYPE>
    EventStream<_DATA_TYPE>& get(std::string_view topic) {
        auto it = streams_.find(topic);
        if (it == streams_.end()) {
            // 新建
            auto stream = std::make_shared<EventStream<_DATA_TYPE>>(topic);
            it          = util::insertHeterogeneous(
                     streams_,
                     std::string{topic},
                     std::static_pointer_cast<EventStreamInterface>(stream)
            )
                     .first;
        } else {
            // 类型校验: 同 topic 必须用同一 _DATA_TYPE, 否则 static_cast 是 UB
            assert(
                it->second->elementType_ == typeid(_DATA_TYPE)
                && "EventBus topic type mismatch: same topic used with different "
                   "EventStream<T> payload type"
            );
        }
        return static_cast<EventStream<_DATA_TYPE>&>(*it->second);
    }

    /// 获取或创建一个请求-响应事件流
    template<typename _REQ_TYPE, typename _RESP_TYPE>
    RequestResponseStream<_REQ_TYPE, _RESP_TYPE>& getRR(std::string_view topic) {
        auto it = streams_.find(topic);
        if (it == streams_.end()) {
            auto stream = std::make_shared<RequestResponseStream<_REQ_TYPE, _RESP_TYPE>>(topic);
            it          = util::insertHeterogeneous(
                     streams_,
                     std::string{topic},
                     std::static_pointer_cast<EventStreamInterface>(stream)
            )
                     .first;
        } else {
            assert(
                it->second->elementType_ == typeid(std::pair<_REQ_TYPE, _RESP_TYPE>)
                && "EventBus topic type mismatch: same topic used with different "
                   "RequestResponseStream<Req,Resp> types"
            );
        }
        return static_cast<RequestResponseStream<_REQ_TYPE, _RESP_TYPE>&>(*it->second);
    }

    /// 获取定时器事件流 (非类型擦除注册表成员, 每次返回临时对象即可)
    template<typename _DATA_TYPE>
    TimerEventStream<_DATA_TYPE> timer() {
        return TimerEventStream<_DATA_TYPE>{executor_};
    }

    bool remove(std::string_view topic);

    /// 单向发布
    template<typename _DATA_TYPE>
    asio::awaitable<void> publish(std::string_view topic, const _DATA_TYPE& data) {
        // 前缀订阅分派 (如插件事件转发)
        if (!prefixListeners_.empty()) {
            for (const auto& [_, sub] : prefixListeners_) {
                if (topic.size() >= sub.prefix.size()
                    && topic.compare(0, sub.prefix.size(), sub.prefix) == 0) {
                    sub.handler(topic, std::any(data));
                }
            }
        }
        // 完全 topic 匹配派发
        co_await get<_DATA_TYPE>(topic).publish(data);
    }

    /// 指定 topic 的单向事件流当前是否有订阅者
    /// - 模板类型参数用于校验类型一致 (同 get<>), 类型不匹配视为无订阅者
    /// - 供高频发布方在构造事件/co_spawn 前查询, 避免空流上的无效开销
    template<typename _DATA_TYPE>
    bool hasListeners(std::string_view topic) {
        auto it = streams_.find(topic);
        if (it == streams_.end()) {
            return false;
        }
        if (it->second->elementType_ != typeid(_DATA_TYPE)) {
            return false;
        }
        return static_cast<EventStream<_DATA_TYPE>&>(*it->second).hasListeners();
    }

    /// 请求-响应
    template<typename _REQ_TYPE, typename _RESP_TYPE>
    asio::awaitable<std::expected<_RESP_TYPE, std::string>> request(
        std::string_view          topic,
        _REQ_TYPE                 req,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0}
    ) {
        co_return co_await getRR<_REQ_TYPE, _RESP_TYPE>(topic).request(std::move(req), timeout);
    }

    /// 订阅 topic 前缀 (如 "plugin."): 匹配的 publish 事件经回调转发
    /// - 载荷经 std::any 类型擦除传递, 回调须自行 any_cast 校验类型
    ///   (插件事件均为 std::string; 不匹配直接返回)
    /// - 回调在发布方线程 (io 线程) 同步调用, 须快速返回
    /// - 返回订阅 id (unlistenPrefix 用); 宿主生命周期内有效
    size_t listenPrefix(
        std::string_view                                                  prefix,
        std::function<void(std::string_view topic, const std::any& data)> handler
    ) {
        auto id              = ++nextPrefixListenerId_;
        prefixListeners_[id] = PrefixSub{std::string{prefix}, std::move(handler)};
        return id;
    }

    /// 取消前缀订阅
    bool unlistenPrefix(size_t id) {
        return prefixListeners_.erase(id) > 0;
    }

    /// 注册同线程同步服务函数 (如 token 计数)
    template<typename _FUNC_TYPE>
    void registerService(std::string_view topic, std::function<_FUNC_TYPE> func) {
        syncServices_[std::string{topic}] = std::any(std::move(func));
    }

    /// 注销同步服务函数
    bool unregisterService(std::string_view topic) {
        return syncServices_.erase(std::string{topic}) > 0;
    }

    /// 检查是否有注册的同步服务函数
    bool hasService(std::string_view topic) const {
        return syncServices_.find(topic) != syncServices_.end();
    }

    /// 调用同步服务函数, 若未注册或类型不匹配则返回 std::nullopt
    template<typename _FUNC_TYPE, typename... _ARGS>
    auto callService(std::string_view topic, _ARGS&&... args) const
        -> std::optional<typename std::invoke_result_t<std::function<_FUNC_TYPE>, _ARGS...>> {
        auto it = syncServices_.find(topic);
        if (it == syncServices_.end()) {
            return std::nullopt;
        }
        using FuncType = std::function<_FUNC_TYPE>;
        const auto* fn = std::any_cast<FuncType>(&it->second);
        if (!fn || !(*fn)) {
            return std::nullopt;
        }
        return (*fn)(std::forward<_ARGS>(args)...);
    }

private:

    /// 前缀订阅项 (仅 io 线程读写)
    struct PrefixSub {
        std::string                                            prefix;
        std::function<void(std::string_view, const std::any&)> handler;
    };

    asio::any_io_executor                                                     executor_;
    std::map<std::string, std::shared_ptr<EventStreamInterface>, std::less<>> streams_{};
    std::map<size_t, PrefixSub>                                               prefixListeners_{};
    std::map<std::string, std::any, std::less<>>                              syncServices_{};
    size_t nextPrefixListenerId_ = 0;
};

/// GraphEvent -> 会话增量 WireDelta + EventBus 适配器
/// - 接替 [agentxx::agent::BaseAgent] 的 llm callback 职责: 把 neograph 的 GraphStreamCallback
/// 翻译成:
///   1. 会话增量 WireDelta (TextToken/ThinkToken/ToolStart/ToolEnd/NodeStart/NodeEnd ...),
///      经 emitDelta 发送到对端 (TUI/stdio), 并写入会话历史 (appendViewMessage)
///   2. 强类型总线事件发布 (EventBus: ModelToken/Error 等)
///   3. 可选转发到原始 callback (origCb)
/// - 有状态: 内部维护流式状态 (chunk 类型切换/节点计时/WireDelta seq)
/// - 新增 GraphEvent 处理时只需扩展本类, 无需修改 BaseAgent
/// - 调用者保证 AgentContext 及其 bus、Session、io 在回调期间存活
///   (makeCallback 内部经 shared_from_this 持有本对象)
class EventBridge : public std::enable_shared_from_this<EventBridge> {
public:

    /// - [agentName] 当前 agent 名 (事件 source)
    /// - [sessionId] 当前会话 id
    /// - [ctx]       AgentContext (取 bus; 若 bus 为空则只做 WireDelta 翻译/转发)
    /// - [session]   会话 (appendViewMessage/contextStats/deltaSeq)
    /// - [io]        server-io (发送 WireDelta/ContextStats; 为空表示 headless 场景)
    /// - [origCb]    原始回调 (可空)
    EventBridge(
        std::string                                  agentName,
        std::string                                  sessionId,
        std::weak_ptr<agentxx::agent::AgentContext>  ctx,
        std::shared_ptr<agentxx::agent::Session>     session,
        std::shared_ptr<agentxx::agent::AgentIOBase> io,
        neograph::graph::GraphStreamCallback         origCb = nullptr
    );

    /// 处理一个 GraphEvent (GraphStreamCallback 调用入口)
    void operator()(const neograph::graph::GraphEvent& event);

    /// 发送增量 WireDelta 到对端 (分配会话单调递增 seq; io 为空时丢弃)
    /// - 会话外的增量 (TurnStart/TurnEnd) 也可经此发送, 保证 seq 全局单调
    void emitDelta(agentxx::agent::WireDelta delta);

    /// 包装为 GraphStreamCallback (shared_from_this 持有, 回调期间本对象存活)
    neograph::graph::GraphStreamCallback makeCallback();

    /// 设置 tps 推送间隔 (秒); 默认 5 秒, 测试可调小以缩短等待
    void setTpsPushInterval(double seconds) {
        tpsPushIntervalSec_ = seconds;
    }

    /// 记录一轮会话开始: 重置轮级 tps 统计
    /// - 由 BaseAgent 在发送 TurnStart WireDelta 前调用
    void handleTurnStart();

    /// 取走本轮会话的 LLM API 平均生成速度 (token/s) 并重置轮级统计
    /// - 由 BaseAgent 在发送 TurnEnd WireDelta 前调用, 结果填入 WireDelta::tps
    /// - 计算口径: 本轮所有 ModelCall 的累计估算 token / 累计流式耗时;
    ///   无 LLM 流式输出时返回 0
    double takeTurnTps();

private:

    void handleLLMToken(const neograph::graph::GraphEvent& event);
    void handleChannelWrite(const neograph::graph::GraphEvent& event);
    void handleNodeStart(const neograph::graph::GraphEvent& event);
    void handleNodeEnd(const neograph::graph::GraphEvent& event);
    void handleError(const neograph::graph::GraphEvent& event);

    /// 结算当前 THINKING 流段: 发送空文本 ThinkToken WireDelta (仅携带
    /// startTimeMs/durationMs), client 据此为已提交的 Think 消息回填耗时。
    /// - think 输出完成 (切换到正文/节点结束/出错/输出最终 assistant 消息) 时调用,
    ///   耗时 = 完成时刻 - 段起点; 未处于 THINKING 段时为 no-op (幂等)
    /// - 必须在该段落后续 WireDelta (正文 token/ToolStart/NodeEnd) 之前发送,
    ///   保证 client 先落盘 Think 消息并回填时长, 再处理后续内容
    void finalizeThinkSegment();

    /// 估算 UTF-8 字符串对应的 token 数
    /// - 优先使用 EventBus 上注册的 TokenCount 服务 (由 SummarizationMiddleware 提供,
    ///   上下文压缩/上下文统计共用同一口径)
    /// - 无注册时 (如测试/裸 EventBridge) 回退内置估算:
    ///   ascii ≈ 4 字符/token, 非 ascii ≈ 1.1 字符/token
    double countTokens(std::string_view text);

    /// 每 [tpsPushIntervalSec_] 秒推送一次最近窗口内的平均生成速度 (token/s) 到对端
    /// - 经 WireContextStats.tps 携带, 与上下文统计共用同一通道;
    ///   无流式数据 (io_ 为空/无会话统计) 时静默跳过
    void pushTpsIfDue();

    /// 结算当前进行中的 ModelCall 流: 将流耗时累加到轮级统计, 并重置流级计数
    /// - 在节点结束 (handleNodeEnd) / 出错 (handleError) 时调用, 保证每个流
    ///   的耗时恰好结算一次
    void settleCurrentStream();

    /// 发布总线事件: LLM_TOKEN -> EventModelToken (无订阅者时跳过, 避免无效协程创建)
    void publishModelToken(const std::string& token, std::string_view kind);
    /// 发布总线事件: ERROR -> EventError (无订阅者时跳过)
    void publishError(std::string message, std::string where);

    std::string                                  agentName_;
    std::string                                  sessionId_;
    std::weak_ptr<agentxx::agent::AgentContext>  ctx_;
    std::shared_ptr<agentxx::agent::Session>     session_;
    std::shared_ptr<agentxx::agent::AgentIOBase> io_;
    neograph::graph::GraphStreamCallback         origCb_;

    /// 最近一次 LLM 流式 chunk 类型 (用于 think 流段切换检测)
    int lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_UNKNOWN;
    /// 当前节点开始计时 (NODE_START 重置)
    std::chrono::system_clock::time_point nodeStartTime_{};
    int64_t                               nodeStartTimeMs_ = 0;

    /// THINKING 流段计时 (think 消息耗时统计):
    /// - 进入 THINKING (非 THINKING -> THINKING) 时记录段起点
    /// - 离开时经 finalizeThinkSegment() 结算: 耗时 = 完成时刻 - 段起点,
    ///   以空文本 ThinkToken WireDelta 回传, client 在 Think 输出完成时才显示耗时
    /// - thinkSegActive_ 保证同一段落只结算一次; 仅 io 线程访问
    std::chrono::system_clock::time_point thinkSegStart_{};
    int64_t                               thinkSegStartMs_ = 0;
    bool                                  thinkSegActive_  = false;

    /// toolCallId → viewMessages 索引 映射 (加速 tool 结果回填 O(1) 定位)
    /// - assistant(tool_calls) 消息登记, tool 结果按 id O(1) 定位, 避免每结果
    ///   从历史末尾线性回扫 O(n²); 未命中时回扫兜底 (如历史来自更早轮次)
    /// - viewMessages append-only, 索引不失效
    std::unordered_map<std::string, size_t> toolCallHistoryIndex_;

    /// tps (token/s) 统计: 从每次 ModelCall 流式开始 (节点开始后首个 token) 计时,
    /// 累计估算 token 数, 每 [tpsPushIntervalSec_] 秒推送一次到对端 (WireContextStats.tps)
    /// - 推送口径为"最近一个窗口 (推送间隔) 内的平均生成速度":
    ///   窗口内 token 增量 / 窗口时长, 而非自流开始以来的累计平均
    ///   (长时间流的速度波动不会被早期数据平滑掉, 反映当前实际生成速度)
    /// - 新流开始判定: handleLLMToken 进入时 lastChatChunkType_ == TYPE_UNKNOWN
    ///   (handleNodeStart/handleNodeEnd/handleError 都会重置该标记)
    /// - 仅在 io 线程访问, 无需同步
    std::chrono::steady_clock::time_point tpsStartTime_{};
    double                                tpsTokenCount_ = 0.0; ///< 当前流累计估算 token 数
    size_t                                tpsPendingChars_ = 0; ///< 批量估算待折算字符数
    double                                tpsLastPushSec_ = 0.0; ///< 上次推送时的累计秒数
    double tpsLastPushToken_   = 0.0; ///< 上次推送时的累计 token 数
    double tpsPushIntervalSec_ = 3.0; ///< 推送间隔 (秒)

    /// 轮级 tps (token/s) 统计: 一轮会话内所有 ModelCall 的累计估算 token 与
    /// 累计流式耗时 (仅计 LLM 流式期间, 不含 tool 执行等间隔)
    /// - handleTurnStart 重置; 每个流结束 (settleCurrentStream) 累加;
    ///   takeTurnTps 取平均值并重置
    double turnTpsTokenCount_  = 0.0; ///< 本轮累计估算 token 数
    double turnTpsDurationSec_ = 0.0; ///< 本轮累计流式耗时 (秒)
};

} // namespace event
} // namespace agentxx
