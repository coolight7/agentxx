#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/tools/sub_agent.h"
#include "agentxx/util/log.h"
#include <map>
#include <memory>
#include <neograph/graph/engine.h>
#include <neograph/graph/types.h>
#include <neograph/json.h>
#include <neograph/types.h>
#include <string>

namespace agentxx {
namespace middleware {

/// Subagent
/// - service.subagent: 单个 subagent 委派 (ReqSubagentStart ->
/// RespSubagentResult)
/// - service.subagent.batch: 批量 subagent 并发委派 (wait_for_all)
/// - service.crossagent: 跨 agent 查询路由 (ReqCrossAgent -> RespCrossAgent)
///   - 维护当前运行中 subagent 的注册表, 查询按 toAgent 路由
/// - 把 subagent 的 GraphEvent 转成 SubagentProgress 发布到总线
class SubagentSupervisor {
public:
  std::weak_ptr<agentxx::agent::AgentContext> agentContext;
  size_t serverId = 0;
  size_t batchServerId = 0;
  size_t crossAgentServerId = 0;
  bool registered = false;

  /// 当前运行中 subagent 名单 (用于跨 agent 查询路由)
  /// - subagent 启动时插入, 完成时移除
  /// - 跨 agent 查询按 toAgent 在此查找是否运行中
  /// NOTE: 跨 agent 查询的完整实现 (向 subagent 注入消息并等待应答) 尚未完成,
  ///       当前 handleCrossAgent 返回明确的 "not implemented" 错误,
  ///       此名单仅用于校验目标 agent 是否运行中
  std::map<std::string, bool> runningRegistry_;

  explicit SubagentSupervisor(std::weak_ptr<agentxx::agent::AgentContext> ctx)
      : agentContext(std::move(ctx)) {}

  asio::awaitable<void> start() {
    if (registered) {
      co_return;
    }
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->bus) {
      XX_LOGE("SubagentSupervisor: AgentContext or bus is null");
      co_return;
    }

    // 单个 subagent 处理服务 ===
    auto &rr =
        ctxPtr->bus
            ->getRR<events::ReqSubagentStart, events::RespSubagentResult>(
                events::Topic::Subagent);
    serverId =
        rr.serve([this](const events::ReqSubagentStart &req,
                        size_t) -> asio::awaitable<events::RespSubagentResult> {
          co_return co_await runSubagent(req.subagentName, req.systemPrompt,
                                         req.message);
        });

    // 批量 subagent 处理服务 ===
    auto &batchRR =
        ctxPtr->bus->getRR<events::ReqSubagentBatch, events::RespSubagentBatch>(
            events::Topic::SubagentBatch);
    batchServerId = batchRR.serve(
        [this](const events::ReqSubagentBatch &req,
               size_t) -> asio::awaitable<events::RespSubagentBatch> {
          co_return co_await runBatch(req);
        });

    // 跨 agent 查询路由 ===
    auto &crossRR =
        ctxPtr->bus->getRR<events::ReqCrossAgent, events::RespCrossAgent>(
            events::Topic::CrossAgent);
    crossAgentServerId = crossRR.serve(
        [this](const events::ReqCrossAgent &req,
               size_t) -> asio::awaitable<events::RespCrossAgent> {
          co_return co_await handleCrossAgent(req);
        });

    registered = true;
    co_return;
  }

  void stop() {
    if (!registered) {
      return;
    }
    auto ctxPtr = agentContext.lock();
    if (ctxPtr && ctxPtr->bus) {
      ctxPtr->bus
          ->getRR<events::ReqSubagentStart, events::RespSubagentResult>(
              events::Topic::Subagent)
          .removeServer(serverId);
      ctxPtr->bus
          ->getRR<events::ReqSubagentBatch, events::RespSubagentBatch>(
              events::Topic::SubagentBatch)
          .removeServer(batchServerId);
      ctxPtr->bus
          ->getRR<events::ReqCrossAgent, events::RespCrossAgent>(
              events::Topic::CrossAgent)
          .removeServer(crossAgentServerId);
    }
    registered = false;
  }

  ~SubagentSupervisor() { stop(); }

private:
  /// 运行单个 subagent (单/批量共用)
  asio::awaitable<events::RespSubagentResult>
  runSubagent(const std::string &subagentName,
              const std::string &systemPromptIn, const std::string &message) {
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->subagentManagerToolPtr) {
      co_return events::RespSubagentResult{
          .hasError = true,
          .errorMessage = "SubAgentManagerTool not available",
      };
    }

    auto &subAgentList = ctxPtr->subagentManagerToolPtr->subAgentList;
    auto it = subAgentList.find(subagentName);
    if (it == subAgentList.end() || !it->second) {
      co_return events::RespSubagentResult{
          .hasError = true,
          .errorMessage = fmt::format("Subagent `{}` not found", subagentName),
      };
    }
    auto subagent = it->second;
    auto subgraph = subagent->getSubgraph();
    if (!subgraph) {
      co_return events::RespSubagentResult{
          .hasError = true,
          .errorMessage = "Subgraph not compiled",
      };
    }

    std::string systemPrompt = systemPromptIn;
    if (systemPrompt.empty()) {
      systemPrompt = subagent->systemPrompt;
      if (systemPrompt.empty()) {
        systemPrompt = "你是一个专门处理用户请求的辅助助手.";
      }
    }

    auto subagentId = fmt::format("subagent_{}", subagentName);
    auto busPtr = ctxPtr->bus;

    // 标记 subagent 运行中 (供跨 agent 查询路由校验)
    runningRegistry_[subagentName] = true;

    try {
      neograph::graph::RunConfig cfg{
          .thread_id = fmt::format("session_{}", subagentId),
          .input = {{
              "messages",
              neograph::json::array({
                  {{"role", "system"}, {"content", systemPrompt}},
                  {{"role", "user"}, {"content", message}},
              }),
          }},
          .resume_if_exists = false,
      };

      XX_LOGD("    ## Subagent dispatched - {}", subagent->name);

      std::ostringstream oss;
      co_await subgraph->run_stream_async(
          cfg, [&oss, &busPtr, &subagentName,
                &subagentId](const neograph::graph::GraphEvent &event) {
            switch (event.type) {
            case neograph::graph::GraphEvent::Type::NODE_START:
            case neograph::graph::GraphEvent::Type::NODE_END:
              break;
            case neograph::graph::GraphEvent::Type::LLM_TOKEN: {
              const auto token = event.data.get<std::string>();
              oss << token;
#if XX_IS_DEBUG_D
              std::cout << token << std::flush;
#endif
              if (busPtr) {
                asio::co_spawn(
                    busPtr->executor(),
                    [busPtr, subagentId, agentName = subagentName,
                     token]() -> asio::awaitable<void> {
                      co_await busPtr->publish<events::EventSubagentProgress>(
                          events::Topic::SubagentProgress,
                          events::EventSubagentProgress{
                              .subagentId = subagentId,
                              .agentName = agentName,
                              .kind = "token",
                              .data = token,
                          });
                    },
                    asio::detached);
              }
            } break;
            case neograph::graph::GraphEvent::Type::CHANNEL_WRITE:
            case neograph::graph::GraphEvent::Type::INTERRUPT:
            case neograph::graph::GraphEvent::Type::ERROR:
              break;
            }
          });

      auto result = oss.str();
      co_await subagent->onSubagentEnd(result);
      runningRegistry_.erase(subagentName);
      co_return events::RespSubagentResult{.content = result};
    } catch (const std::exception &e) {
      runningRegistry_.erase(subagentName);
      co_return events::RespSubagentResult{
          .hasError = true,
          .errorMessage = fmt::format("Sub-agent failed: {}", e.what()),
      };
    }
  }

  /// 批量并发运行多个 subagent (真并发: co_spawn + channel wait_for_all)
  /// - 单 io_context 协作式调度: 各 subagent 协程在 co_await 挂起点交替推进
  /// - 等待全部完成才返回, 结果顺序与输入任务一致 (按 index 写入)
  asio::awaitable<events::RespSubagentBatch>
  runBatch(const events::ReqSubagentBatch &req) {
    events::RespSubagentBatch batchResp;
    if (req.tasks.empty()) {
      co_return batchResp;
    }
    auto ex = co_await asio::this_coro::executor;
    using ItemResult = events::RespSubagentBatchItem;
    size_t n = req.tasks.size();
    std::vector<ItemResult> results(n);

    // 每个 subagent co_spawn 为独立协程, 完成后向 channel 发送 index
    // 主协程接收 n 次即代表全部完成 (wait_for_all 语义)
    auto doneChannel = std::make_shared<
        asio::experimental::channel<void(neograph_asio_error_code, size_t)>>(
        ex, static_cast<unsigned>(n));

    for (size_t i = 0; i < n; ++i) {
      const auto &task = req.tasks[i];
      asio::co_spawn(
          ex,
          [this, task, &results, i, doneChannel]() -> asio::awaitable<void> {
            auto r = co_await runSubagent(task.subagentName, task.systemPrompt,
                                          task.message);
            results[i] = ItemResult{
                .resultId = task.resultId,
                .content = r.content,
                .hasError = r.hasError,
                .errorMessage = r.errorMessage,
            };
            doneChannel->async_send(neograph_asio_error_code{}, i,
                                    [](neograph_asio_error_code) {});
          },
          asio::detached);
    }

    // 等待全部完成 (接收 n 次)
    for (size_t i = 0; i < n; ++i) {
      co_await doneChannel->async_receive(asio::as_tuple(asio::use_awaitable));
    }

    batchResp.results = std::move(results);
    co_return batchResp;
  }

  /// 跨 agent 查询路由
  /// - 按 toAgent 查找运行中 subagent
  /// - NOTE: 完整实现 (向 subagent 注入消息并等待应答) 尚未完成
  ///         当前返回明确的 "not implemented" 错误, 避免调用方误以为可用
  ///         runningRegistry_ 仅用于校验目标 agent 是否运行中
  asio::awaitable<events::RespCrossAgent>
  handleCrossAgent(const events::ReqCrossAgent &req) {
    auto it = runningRegistry_.find(req.toAgent);
    if (it == runningRegistry_.end() || !it->second) {
      co_return events::RespCrossAgent{
          .hasError = true,
          .errorMessage = fmt::format(
              "Agent `{}` not running or not registered", req.toAgent),
      };
    }
    // 目标 agent 运行中, 但被动消息注入机制尚未实现
    // (需向 subagent 的 GraphState 注入 user 消息并触发新一轮 LLM 调用,
    //  再捕获输出作为应答 — 当前 subgraph 的 run_stream_async 是一次性运行,
    //  不支持运行中追加消息, 后续需扩展为持久会话模式)
    co_return events::RespCrossAgent{
        .hasError = true,
        .errorMessage =
            fmt::format("Cross-agent query to `{}`: not implemented "
                        "(passive message injection requires persistent "
                        "subagent sessions)",
                        req.toAgent),
    };
  }
};

} // namespace middleware
} // namespace agentxx
