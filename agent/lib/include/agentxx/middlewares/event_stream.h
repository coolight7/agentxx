#pragma once

#include "agentxx/middlewares/middleware.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/experimental/awaitable_operators.hpp"
#include "asio/experimental/channel.hpp"
#include "asio/experimental/channel_traits.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

namespace agentxx {
namespace middleware {

namespace evs_detail {
using namespace ::asio::experimental::awaitable_operators;
} // namespace evs_detail

/// 仅用作 EventStream<> 模板参数的类型擦除基类
/// - 所有具体事件流都是 EventStream<T> 或 RequestResponseStream<TReq,TResp>
/// - EventBus 只持有 shared_ptr<EventStreamInterface> 以做类型无关的注册表
/// - 存储 elementType_ (typeid) 供 EventBus::get/getRR 校验类型一致性, 防 UB
class EventStreamInterface {
public:
  std::string name;
  const std::type_info &elementType_;

  EventStreamInterface(std::string_view in_name,
                       const std::type_info &elementType)
      : name(in_name), elementType_(elementType) {}
  virtual ~EventStreamInterface() = default;
};

/// 单个订阅者: 收到 data 后执行 handle
/// - execHit: 剩余触发次数; >=1 为有限订阅, ==0 为常驻订阅(永不自删)
template <typename _DATA_TYPE> struct EventSubscription {
  size_t id = 0;
  int execHit = 0; // 0 = 常驻
  std::function<asio::awaitable<void>(const _DATA_TYPE &)> handle;
};

/// 单向强类型事件流: publish -> 顺序派发到每个订阅者
/// - 替代旧 EventStreamBase, 修正 id/execHit 混用 bug
template <typename _DATA_TYPE> class EventStream : public EventStreamInterface {
public:
  using DataType = _DATA_TYPE;
  using Handler = std::function<asio::awaitable<void>(const _DATA_TYPE &)>;

private:
  size_t insertId_ = 0;
  std::map<size_t, EventSubscription<_DATA_TYPE>> listeners_{};

public:
  EventStream(std::string_view in_name)
      : EventStreamInterface(in_name, typeid(_DATA_TYPE)) {}

  /// 订阅事件; execHit==0 表示常驻订阅, >0 表示触发 N 次后自动移除
  /// 返回订阅 id, 供 unsubscribe 使用
  size_t subscribe(Handler handle, int execHit = 0) {
    assert(handle);
    assert(execHit >= 0);
    auto id = ++insertId_;
    listeners_[id] = EventSubscription<_DATA_TYPE>{
        .id = id, .execHit = execHit, .handle = std::move(handle)};
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

  /// 发布事件: 顺序派发到每个订阅者
  /// - 单 io_context 协作式调度, 顺序派发在每个 co_await 挂起点让出,
  ///   足够公平且语义清晰; 真并发由 RequestResponseStream 的 co_spawn 承担
  /// - 单个订阅者异常被捕获并记录, 不中断其他订阅者
  /// - execHit>0 的有限订阅: 递减后 <=0 在派发后移除; 常驻 (execHit==0) 不动
  asio::awaitable<void> publish(const _DATA_TYPE &data) {
    if (listeners_.empty()) {
      co_return;
    }
    // 快照当前订阅者, 避免派发过程中 map 迭代器失效
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

    for (auto &sub : snapshot) {
      try {
        co_await sub.handle(data);
      } catch (const std::exception &e) {
        XX_LOGE("EventStream `{}` listener exception: {}", name, e.what());
      } catch (...) {
        XX_LOGE("EventStream `{}` listener unknown exception", name);
      }
    }
  }
};

/// 请求-响应事件流: 用于 HIL (interrupt/permission) 与 subagent 委派
/// - request() 在调用协程内挂起, 直到 serve 端 respond 或超时
/// - correlationId 关联 request 与 response
/// - 每个待响应请求持有一个 channel<TResp> 作为结果槽
template <typename _REQ_TYPE, typename _RESP_TYPE>
class RequestResponseStream : public EventStreamInterface {
public:
  using ReqType = _REQ_TYPE;
  using RespType = _RESP_TYPE;
  using ServerHandler = std::function<asio::awaitable<RespType>(
      const ReqType &req, size_t correlationId)>;

private:
  using RespChannel =
      asio::experimental::channel<void(neograph_asio_error_code, RespType)>;

  struct PendingReq {
    size_t correlationId = 0;
    std::shared_ptr<RespChannel> channel;
  };

  std::atomic_size_t correlationSeq_{0};
  std::atomic_size_t serverId_{0};

  std::map<size_t, ServerHandler> servers_{}; // <serverId, handler>
  std::map<size_t, PendingReq> pending_{};    // <correlationId, slot>

public:
  RequestResponseStream(std::string_view in_name)
      : EventStreamInterface(in_name,
                             typeid(std::pair<_REQ_TYPE, _RESP_TYPE>)) {}

  /// 注册服务端处理者; 返回 serverId
  /// - 同一 topic 可多 server 注册, request 轮询派发
  size_t serve(ServerHandler handler) {
    assert(handler);
    auto id = ++serverId_;
    servers_[id] = std::move(handler);
    return id;
  }

  bool removeServer(size_t serverId) {
    auto it = servers_.find(serverId);
    if (it == servers_.end()) {
      return false;
    }
    servers_.erase(it);
    return true;
  }

  /// 发起请求并等待响应
  /// - timeout 到期返回 nullopt, 并清理 pending 槽
  /// - 同一 io_context 单线程运行, pending_/servers_ 无需加锁
  asio::awaitable<std::optional<RespType>>
  request(ReqType req,
          std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
    if (servers_.empty()) {
      XX_LOGE("RequestResponseStream `{}` request: no server registered", name);
      co_return std::nullopt;
    }

    auto correlationId = ++correlationSeq_;
    auto channel =
        std::make_shared<RespChannel>(co_await asio::this_coro::executor, 1);
    pending_[correlationId] =
        PendingReq{.correlationId = correlationId, .channel = channel};

    // 派发到第一个 server (单 server 场景即该 server)
    // 多 server 时可扩展为负载均衡, 这里取 begin
    auto serverIt = servers_.begin();
    auto handler = serverIt->second;
    auto ex = co_await asio::this_coro::executor;
    // server 协程捕获 channel (shared_ptr) 与 name (副本), 不捕获 this,
    // 避免本流生命周期短于 server 协程时的 use-after-free
    // - 成功/异常都直接 async_send 到 channel; 若请求已超时, 请求方会
    //   channel->cancel() 关闭通道, server 的 async_send 回调收到 error,
    //   server 协程自然结束, 无僵尸协程
    asio::co_spawn(
        ex,
        [streamName = name, handler = std::move(handler), req = std::move(req),
         correlationId, channel]() -> asio::awaitable<void> {
          try {
            auto resp = co_await handler(req, correlationId);
            channel->async_send(neograph_asio_error_code{}, std::move(resp),
                                [](neograph_asio_error_code) {});
          } catch (const neograph::graph::CancelledException &e) {
            // 超时取消, 不必发错误到 channel (请求方已超时返回)
          } catch (const std::exception &e) {
            XX_LOGE("RequestResponseStream `{}` server exception: {}",
                    streamName, e.what());
            channel->async_send(
                asio::experimental::channel_errc::channel_cancelled, RespType{},
                [](neograph_asio_error_code) {});
          } catch (...) {
            XX_LOGE("RequestResponseStream `{}` server unknown exception",
                    streamName);
            channel->async_send(
                asio::experimental::channel_errc::channel_cancelled, RespType{},
                [](neograph_asio_error_code) {});
          }
        },
        asio::detached);

    // 等待响应或超时
    // - operator|| (wait_for_one_success): 先成功者胜, 取消另一者
    // - T=RespType, U=void -> 结果 std::variant<RespType, std::monostate>
    // - 响应先到: index 0 = RespType; 超时先到: index 1 = monostate -> nullopt
    auto timer = asio::steady_timer(ex, timeout);
    using namespace evs_detail;
    auto waitResp = [channel]() -> asio::awaitable<RespType> {
      auto [ec, resp] =
          co_await channel->async_receive(asio::as_tuple(asio::use_awaitable));
      if (ec) {
        throw std::runtime_error("response channel closed");
      }
      co_return std::move(resp);
    };
    auto waitTimeout = [&timer]() -> asio::awaitable<void> {
      co_await timer.async_wait(asio::use_awaitable);
    };

    std::optional<RespType> out;
    try {
      auto result = co_await (waitResp() || waitTimeout());
      if (result.index() == 0) {
        out = std::move(std::get<0>(std::move(result)));
      }
      // index 1 = monostate = 超时
    } catch (const std::exception &e) {
      XX_LOGE("RequestResponseStream `{}` request await failed: {}", name,
              e.what());
    }

    // 超时或异常时关闭 channel, 使 server 的 async_send 失败并自然结束协程,
    // 避免僵尸协程长期占用
    if (!out.has_value()) {
      channel->cancel();
    }
    // 统一清理 pending 槽 (成功/超时/异常路径)
    pending_.erase(correlationId);
    co_return out;
  }

  /// 外部异步回填响应 (供 server 在 handler 之外、稍后回调用)
  /// - 内部 auto-respond 路径已直接通过 channel 回填, 不走这里
  /// - 调用者须保证本流仍存活 (持有 bus 引用)
  void respond(size_t correlationId, RespType resp) {
    auto it = pending_.find(correlationId);
    if (it == pending_.end()) {
      return; // 已超时被清理
    }
    auto channel = it->second.channel;
    pending_.erase(it);
    channel->async_send(neograph_asio_error_code{}, std::move(resp),
                        [](neograph_asio_error_code) {});
  }

  void failRequest(size_t correlationId) {
    auto it = pending_.find(correlationId);
    if (it == pending_.end()) {
      return;
    }
    auto channel = it->second.channel;
    pending_.erase(it);
    channel->async_send(asio::experimental::channel_errc::channel_cancelled,
                        RespType{}, [](neograph_asio_error_code) {});
  }
};

/// 定时器事件流: 订阅时指定延迟, 延迟到点后触发一次
/// - 注意: 可能为临时对象 (EventBus::timer<T>() 返回值), 协程不捕获 this
template <typename _DATA_TYPE> class TimerEventStream {
public:
  using Handler = std::function<asio::awaitable<void>(const _DATA_TYPE &)>;

private:
  asio::any_io_executor executor_;
  std::string name_;
  std::atomic_size_t timerSeq_{0};

public:
  explicit TimerEventStream(asio::any_io_executor ex,
                            std::string name = "TimerEventStream")
      : executor_(ex), name_(std::move(name)) {}

  /// 注册一次性定时器: delay 后触发 handler(data); 返回定时器 id
  /// - 在 executor_ 上 co_spawn 独立协程, 不阻塞调用者
  /// - 注意: TimerEventStream 可能为临时对象, 协程不捕获 this,
  ///   仅捕获 executor 副本与日志用的 name
  size_t once(std::chrono::milliseconds delay, Handler handle,
              _DATA_TYPE data = _DATA_TYPE{}) {
    assert(handle);
    auto id = ++timerSeq_;
    auto ex = executor_;
    auto streamName = name_;
    asio::co_spawn(
        ex,
        [id, delay, handle = std::move(handle), data = std::move(data),
         streamName]() -> asio::awaitable<void> {
          auto timer =
              asio::steady_timer(co_await asio::this_coro::executor, delay);
          co_await timer.async_wait(asio::use_awaitable);
          try {
            co_await handle(data);
          } catch (const std::exception &e) {
            XX_LOGE("TimerEventStream `{}` id={} exception: {}", streamName, id,
                    e.what());
          } catch (...) {
            XX_LOGE("TimerEventStream `{}` id={} unknown exception", streamName,
                    id);
          }
        },
        asio::detached);
    return id;
  }
};

/// EventBus: 按 topic 名注册/查找类型擦除的事件流
/// - 模块只依赖 EventBus + 事件类型头, 不互相可见
/// - 单 io_context 单线程运行, 内部表无需加锁
class EventBus {
public:
  explicit EventBus(asio::any_io_executor executor) : executor_(executor) {}

  asio::any_io_executor executor() const { return executor_; }

  /// 获取或创建一个单向事件流; 同 topic 同类型复用
  template <typename _DATA_TYPE>
  EventStream<_DATA_TYPE> &get(std::string_view topic) {
    auto key = std::string{topic};
    auto it = streams_.find(key);
    if (it == streams_.end()) {
      auto stream = std::make_shared<EventStream<_DATA_TYPE>>(key);
      it = streams_
               .emplace(key,
                        std::static_pointer_cast<EventStreamInterface>(stream))
               .first;
    } else {
      // 类型校验: 同 topic 必须用同一 _DATA_TYPE, 否则 static_cast 是 UB
      assert(it->second->elementType_ == typeid(_DATA_TYPE) &&
             "EventBus topic type mismatch: same topic used with different "
             "EventStream<T> payload type");
    }
    return static_cast<EventStream<_DATA_TYPE> &>(*it->second);
  }

  /// 获取或创建一个请求-响应事件流
  template <typename _REQ_TYPE, typename _RESP_TYPE>
  RequestResponseStream<_REQ_TYPE, _RESP_TYPE> &getRR(std::string_view topic) {
    auto key = std::string{topic};
    auto it = streams_.find(key);
    if (it == streams_.end()) {
      auto stream =
          std::make_shared<RequestResponseStream<_REQ_TYPE, _RESP_TYPE>>(key);
      it = streams_
               .emplace(key,
                        std::static_pointer_cast<EventStreamInterface>(stream))
               .first;
    } else {
      assert(it->second->elementType_ ==
                 typeid(std::pair<_REQ_TYPE, _RESP_TYPE>) &&
             "EventBus topic type mismatch: same topic used with different "
             "RequestResponseStream<Req,Resp> types");
    }
    return static_cast<RequestResponseStream<_REQ_TYPE, _RESP_TYPE> &>(
        *it->second);
  }

  /// 获取定时器事件流 (非类型擦除注册表成员, 每次返回临时对象即可)
  template <typename _DATA_TYPE> TimerEventStream<_DATA_TYPE> timer() {
    return TimerEventStream<_DATA_TYPE>{executor_};
  }

  bool remove(std::string_view topic) {
    auto it = streams_.find(std::string{topic});
    if (it == streams_.end()) {
      return false;
    }
    streams_.erase(it);
    return true;
  }

  /// 单向发布
  template <typename _DATA_TYPE>
  asio::awaitable<void> publish(std::string_view topic,
                                const _DATA_TYPE &data) {
    co_await get<_DATA_TYPE>(topic).publish(data);
  }

  /// 请求-响应
  template <typename _REQ_TYPE, typename _RESP_TYPE>
  asio::awaitable<std::optional<_RESP_TYPE>>
  request(std::string_view topic, _REQ_TYPE req,
          std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
    co_return co_await getRR<_REQ_TYPE, _RESP_TYPE>(topic).request(
        std::move(req), timeout);
  }

private:
  asio::any_io_executor executor_;
  std::map<std::string, std::shared_ptr<EventStreamInterface>> streams_{};
};

} // namespace middleware
} // namespace agentxx
