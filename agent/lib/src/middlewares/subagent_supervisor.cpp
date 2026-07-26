#include "agentxx/middlewares/subagent_supervisor.h"

#include "agentxx/tools/sub_agent.h"
#include "agentxx/util/log.h"
#include "fmt/format.h"
#include <neograph/graph/engine.h>
#include <sstream>
#include <vector>

namespace agentxx {
namespace middleware {

SubagentSupervisor::SubagentSupervisor(std::weak_ptr<agentxx::agent::AgentContext> ctx) :
    agentContext(std::move(ctx)) {}

asio::awaitable<void> SubagentSupervisor::start() {
    if (registered) {
        co_return;
    }
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->bus) {
        XX_LOGE("SubagentSupervisor: AgentContext or bus is null");
        co_return;
    }

    // 单个 subagent 处理服务 ===
    auto& rr = ctxPtr->bus->getRR<events::ReqSubagentStart, events::RespSubagentResult>(
        events::Topic::Subagent
    );
    serverId = rr.serve(
        [this](const events::ReqSubagentStart& req, size_t)
            -> asio::awaitable<events::RespSubagentResult> {
            co_return co_await runSubagent(
                req.subagentName,
                req.systemPrompt,
                req.message,
                req.parentThreadId
            );
        }
    );

    // 批量 subagent 处理服务 ===
    auto& batchRR = ctxPtr->bus->getRR<events::ReqSubagentBatch, events::RespSubagentBatch>(
        events::Topic::SubagentBatch
    );
    batchServerId = batchRR.serve(
        [this](const events::ReqSubagentBatch& req, size_t)
            -> asio::awaitable<events::RespSubagentBatch> {
            co_return co_await runBatch(req);
        }
    );

    // 跨 agent 查询路由 ===
    auto& crossRR = ctxPtr->bus->getRR<events::ReqCrossAgent, events::RespCrossAgent>(
        events::Topic::CrossAgent
    );
    crossAgentServerId = crossRR.serve(
        [this](const events::ReqCrossAgent& req, size_t)
            -> asio::awaitable<events::RespCrossAgent> {
            co_return co_await handleCrossAgent(req);
        }
    );

    registered = true;
    co_return;
}

void SubagentSupervisor::stop() {
    if (!registered) {
        return;
    }
    auto ctxPtr = agentContext.lock();
    if (ctxPtr && ctxPtr->bus) {
        ctxPtr->bus
            ->getRR<events::ReqSubagentStart, events::RespSubagentResult>(events::Topic::Subagent)
            .removeServer(serverId);
        ctxPtr->bus
            ->getRR<events::ReqSubagentBatch, events::RespSubagentBatch>(
                events::Topic::SubagentBatch
            )
            .removeServer(batchServerId);
        ctxPtr->bus->getRR<events::ReqCrossAgent, events::RespCrossAgent>(events::Topic::CrossAgent)
            .removeServer(crossAgentServerId);
    }
    registered = false;
}

SubagentSupervisor::~SubagentSupervisor() {
    stop();
}

asio::awaitable<events::RespSubagentResult> SubagentSupervisor::runSubagent(
    std::string_view subagentName,
    std::string_view systemPromptIn,
    std::string_view message,
    std::string_view parentThreadId
) {
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->subagentManagerToolPtr) {
        co_return events::RespSubagentResult{
            .hasError     = true,
            .errorMessage = "SubAgentManagerTool not available",
        };
    }

    auto& subAgentList = ctxPtr->subagentManagerToolPtr->subAgentList;
    auto  it           = subAgentList.find(subagentName);
    if (it == subAgentList.end() || !it->second) {
        co_return events::RespSubagentResult{
            .hasError     = true,
            .errorMessage = fmt::format("Subagent `{}` not found", subagentName),
        };
    }
    auto subagent = it->second;
    auto subgraph = subagent->getSubgraph();
    if (!subgraph) {
        co_return events::RespSubagentResult{
            .hasError     = true,
            .errorMessage = "Subgraph not compiled",
        };
    }

    std::string systemPrompt{systemPromptIn};
    if (systemPrompt.empty()) {
        systemPrompt = subagent->systemPrompt;
        if (systemPrompt.empty()) {
            systemPrompt = "你是一个专门处理用户请求的辅助助手.";
        }
    }

    auto subagentId = fmt::format("subagent_{}", subagentName);
    auto busPtr     = ctxPtr->bus;

    // 标记 subagent 运行中 (供跨 agent 查询路由校验)
    runningRegistry_[std::string{subagentName}] = true;

    try {
        neograph::graph::RunConfig cfg{
            .thread_id        = fmt::format("session_{}", subagentId),
            .input            = {{
                "messages",
                neograph::json::array({
                    {{"role", "system"}, {"content", systemPrompt}},
                    {{"role", "user"}, {"content", message}},
                }),
            }},
            .resume_if_exists = false,
        };

        // subagent 会话继承父会话的 IO (供权限询问/中断交互使用)
        if (false == parentThreadId.empty()) {
            auto parentSession = ctxPtr->sessions->get(parentThreadId);
            if (parentSession) {
                ctxPtr->getSession(cfg.thread_id)->io = parentSession->io;
            }
        }

        XX_LOGD("    ## Subagent dispatched - {}", subagent->name);

        std::ostringstream oss;
        co_await subgraph->run_stream_async(
            cfg,
            [&oss, &busPtr, &subagentName, &subagentId](const neograph::graph::GraphEvent& event) {
                switch (event.type) {
                    case neograph::graph::GraphEvent::Type::NODE_START:
                    case neograph::graph::GraphEvent::Type::NODE_END:
                        break;
                    case neograph::graph::GraphEvent::Type::LLM_TOKEN: {
                        std::string token;
                        std::string kind = "content";
                        if (event.data.is_string()) {
                            token = event.data.get<std::string>();
                        } else if (event.data.is_object()) {
                            neograph::ChatStreamChunk chunk;
                            neograph::from_json(event.data, chunk);
                            token = std::move(chunk.data);
                            if (chunk.type == neograph::ChatStreamChunk::TYPE_THINKING) {
                                kind = "thinking";
                            }
                        }
                        // 仅累积 content token
                        if (kind == "content") {
                            oss << token;
                        }
                        if (busPtr) {
                            asio::co_spawn(
                                busPtr->executor(),
                                [busPtr,
                                 subagentId,
                                 agentName = std::string{subagentName},
                                 token     = std::move(token),
                                 kind      = std::move(kind)]() -> asio::awaitable<void> {
                                    co_await busPtr->publish<events::EventSubagentProgress>(
                                        events::Topic::SubagentProgress,
                                        events::EventSubagentProgress{
                                            .subagentId = subagentId,
                                            .agentName  = agentName,
                                            .kind       = kind == "thinking" ? "thinking" : "token",
                                            .data       = token,
                                        }
                                    );
                                },
                                asio::detached
                            );
                        }
                    } break;
                    case neograph::graph::GraphEvent::Type::CHANNEL_WRITE:
                    case neograph::graph::GraphEvent::Type::INTERRUPT:
                    case neograph::graph::GraphEvent::Type::ERROR:
                        break;
                }
            }
        );

        auto result = oss.str();
        co_await subagent->onSubagentEnd(result);
        runningRegistry_.erase(subagentName);
        co_return events::RespSubagentResult{.content = result};
    } catch (const std::exception& e) {
        runningRegistry_.erase(subagentName);
        co_return events::RespSubagentResult{
            .hasError     = true,
            .errorMessage = fmt::format("Sub-agent failed: {}", e.what()),
        };
    }
}

asio::awaitable<events::RespSubagentBatch>
    SubagentSupervisor::runBatch(const events::ReqSubagentBatch& req) {
    events::RespSubagentBatch batchResp;
    if (req.tasks.empty()) {
        co_return batchResp;
    }
    auto ex                   = co_await asio::this_coro::executor;
    using ItemResult          = events::RespSubagentBatchItem;
    size_t                  n = req.tasks.size();
    std::vector<ItemResult> results(n);

    // 每个 subagent co_spawn 为独立协程, 完成后向 channel 发送 index
    // 主协程接收 n 次即代表全部完成 (wait_for_all 语义)
    auto doneChannel
        = std::make_shared<asio::experimental::channel<void(neograph_asio_error_code, size_t)>>(
            ex,
            static_cast<unsigned>(n)
        );

    for (size_t i = 0; i < n; ++i) {
        const auto& task = req.tasks[i];
        asio::co_spawn(
            ex,
            [this, task, &results, i, doneChannel, parentThreadId = req.parentThreadId](
            ) -> asio::awaitable<void> {
                auto r = co_await runSubagent(
                    task.subagentName,
                    task.systemPrompt,
                    task.message,
                    parentThreadId
                );
                results[i] = ItemResult{
                    .resultId     = task.resultId,
                    .content      = r.content,
                    .hasError     = r.hasError,
                    .errorMessage = r.errorMessage,
                };
                doneChannel
                    ->async_send(neograph_asio_error_code{}, i, [](neograph_asio_error_code) {});
            },
            asio::detached
        );
    }

    // 等待全部完成 (接收 n 次)
    for (size_t i = 0; i < n; ++i) {
        co_await doneChannel->async_receive(asio::as_tuple(asio::use_awaitable));
    }

    batchResp.results = std::move(results);
    co_return batchResp;
}

asio::awaitable<events::RespCrossAgent>
    SubagentSupervisor::handleCrossAgent(const events::ReqCrossAgent& req) {
    auto it = runningRegistry_.find(req.toAgent);
    if (it == runningRegistry_.end() || !it->second) {
        co_return events::RespCrossAgent{
            .hasError     = true,
            .errorMessage = fmt::format("Agent `{}` not running or not registered", req.toAgent),
        };
    }
    // 目标 agent 运行中, 但被动消息注入机制尚未实现
    // (需向 subagent 的 GraphState 注入 user 消息并触发新一轮 LLM 调用,
    //  再捕获输出作为应答 — 当前 subgraph 的 run_stream_async 是一次性运行,
    //  不支持运行中追加消息, 后续需扩展为持久会话模式)
    co_return events::RespCrossAgent{
        .hasError     = true,
        .errorMessage = fmt::format(
            "Cross-agent query to `{}`: not implemented "
            "(passive message injection requires persistent "
            "subagent sessions)",
            req.toAgent
        ),
    };
}

} // namespace middleware
} // namespace agentxx
