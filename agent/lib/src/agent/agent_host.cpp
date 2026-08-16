#include "agentxx/agent/agent_host.h"

#include "agentxx/agent/code_agent.h"
#include "agentxx/protocol/a2a_client.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/experimental/channel.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <sstream>

namespace agentxx {
namespace agent {

// ===========================================================================
// AgentRegistry
// ===========================================================================

std::shared_ptr<AgentNode> AgentRegistry::get(std::string_view agentId) const {
    auto it = nodes_.find(agentId);
    return it == nodes_.end() ? nullptr : it->second;
}

bool AgentRegistry::contains(std::string_view agentId) const {
    return nodes_.contains(agentId);
}

void AgentRegistry::insert(std::shared_ptr<AgentNode> node) {
    assert(node && !node->agentId.empty());
    nodes_[node->agentId] = std::move(node);
}

void AgentRegistry::remove(std::string_view agentId) {
    nodes_.erase(agentId);
}

size_t AgentRegistry::size() const {
    return nodes_.size();
}

std::vector<std::shared_ptr<AgentNode>> AgentRegistry::childrenOf(std::string_view parentAgentId
) const {
    std::vector<std::shared_ptr<AgentNode>> out;
    for (const auto& [id, node] : nodes_) {
        if (node->parentAgentId == parentAgentId) {
            out.push_back(node);
        }
    }
    return out;
}

void AgentRegistry::clear() {
    nodes_.clear();
}

// ===========================================================================
// AgentHost
// ===========================================================================

std::shared_ptr<AgentHost> AgentHost::create(Config cfg) {
    std::shared_ptr<AgentHost> host(new AgentHost(std::move(cfg)));
    // 宿主总线: 派生与跨 agent 消息服务
    std::weak_ptr<AgentHost> self = host;

    // agent.spawn (RR): 任意调用方经宿主总线派生独立子代理
    host->hostBus_
        ->getRR<events::ReqHostSpawn, events::RespHostSpawn>(events::HostTopic::AgentSpawn)
        .serve(
            [self](const events::ReqHostSpawn& req, size_t)
                -> asio::awaitable<events::RespHostSpawn> {
                auto ptr = self.lock();
                if (!ptr) {
                    co_return events::RespHostSpawn{
                        .hasError     = true,
                        .errorMessage = "AgentHost no longer available",
                    };
                }
                auto resp = co_await ptr->spawnSubagent(
                    req.name,
                    req.systemPrompt,
                    req.message,
                    /*parentThreadId=*/"",
                    req.cancelToken
                );
                co_return events::RespHostSpawn{
                    .agentId      = resp.agentId,
                    .content      = resp.content,
                    .hasError     = resp.hasError,
                    .errorMessage = resp.errorMessage,
                };
            }
        );

    // agent.message (RR): 任意→任意 agent 消息 (mailbox 路由)
    host->hostBus_
        ->getRR<events::ReqHostMessage, events::RespHostMessage>(events::HostTopic::AgentMessage)
        .serve(
            [self](const events::ReqHostMessage& req, size_t)
                -> asio::awaitable<events::RespHostMessage> {
                auto ptr = self.lock();
                if (!ptr) {
                    co_return events::RespHostMessage{
                        .hasError     = true,
                        .errorMessage = "AgentHost no longer available",
                    };
                }
                co_return co_await ptr->sendMessage(req);
            }
        );

    return host;
}

AgentHost::AgentHost(Config cfg) :
    cfg_(std::move(cfg)) {
    if (cfg_.ioCtx) {
        ioCtx_ = cfg_.ioCtx;
    } else {
        ioCtx_ = std::make_shared<asio::io_context>();
    }
    size_t poolThreads = cfg_.blockingPoolThreads;
    if (0 == poolThreads) {
        poolThreads = std::max(2u, std::thread::hardware_concurrency() / 2);
    }
    blockingPool_ = std::make_shared<asio::thread_pool>(poolThreads);
    hostBus_      = std::make_shared<agentxx::middleware::EventBus>(ioCtx_->get_executor());
}

AgentHost::~AgentHost() = default;

std::shared_ptr<asio::io_context> AgentHost::ioCtx() {
    return ioCtx_;
}

std::shared_ptr<agentxx::middleware::EventBus> AgentHost::hostBus() {
    return hostBus_;
}

std::shared_ptr<asio::thread_pool> AgentHost::blockingPool() {
    return blockingPool_;
}

AgentRegistry& AgentHost::registry() {
    return registry_;
}

size_t AgentHost::runningSubagents() const {
    return runningSubagentCount_;
}

std::shared_ptr<BaseAgent> AgentHost::rootAgent() const {
    return rootAgent_;
}

void AgentHost::attachRoot(std::shared_ptr<BaseAgent> rootAgent) {
    assert(rootAgent && "attachRoot: root agent is null");
    if (rootAgent_) {
        XX_LOGW("AgentHost::attachRoot: root already attached, ignored");
        return;
    }
    rootAgent_ = std::move(rootAgent);

    auto ctx = rootAgent_->getContext();
    assert(ctx && "attachRoot: root agent context is null");
    if (!ctx) {
        return;
    }
    // 注入共享基础设施: blockingPool 上提共享 (避免每 agent 一份线程池),
    // host 引用 (供节点/工具经 AgentContext 感知宿主)
    ctx->blockingPool = blockingPool_;
    ctx->host         = weak_from_this();

    if (!ctx->bus) {
        XX_LOGW("AgentHost::attachRoot: root agent bus is null, subagent delegation unavailable");
        return;
    }

    // 根 agent 全局总线上 serve 子代理委派 (中断路径):
    // - SubAgentManagerTool 抛 NodeInterrupt → BaseAgent 中断循环 →
    //   agentContext->bus->request(service.subagent) → 到达宿主
    // - 宿主派生独立 agent 运行, 结果经 interruptResult channel 回填
    std::weak_ptr<AgentHost> self = weak_from_this();

    // 根 agent 也注册为节点 (平等成员; 由宿主持有, 不随 destroyAgent 回收)
    {
        auto rootNode     = std::make_shared<AgentNode>();
        rootNode->agentId = "root";
        rootNode->name    = ctx->agentConfig ? ctx->agentConfig->agentName : std::string{"root"};
        rootNode->parentAgentId = "";
        rootNode->depth         = 0;
        rootNode->agent         = rootAgent_;
        registry_.insert(std::move(rootNode));
    }

    auto& rr = ctx->bus->getRR<events::ReqSubagentStart, events::RespSubagentResult>(
        events::Topic::Subagent
    );
    subagentServerId_ = rr.serve(
        [self](const events::ReqSubagentStart& req, size_t)
            -> asio::awaitable<events::RespSubagentResult> {
            auto ptr = self.lock();
            if (!ptr) {
                co_return events::RespSubagentResult{
                    .hasError     = true,
                    .errorMessage = "AgentHost no longer available",
                };
            }
            co_return co_await ptr->spawnSubagent(
                req.subagentName,
                req.systemPrompt,
                req.message,
                req.parentThreadId,
                req.cancelToken
            );
        }
    );

    auto& batchRR = ctx->bus->getRR<events::ReqSubagentBatch, events::RespSubagentBatch>(
        events::Topic::SubagentBatch
    );
    batchServerId_ = batchRR.serve(
        [self](const events::ReqSubagentBatch& req, size_t)
            -> asio::awaitable<events::RespSubagentBatch> {
            auto ptr = self.lock();
            if (!ptr) {
                co_return events::RespSubagentBatch{};
            }
            co_return co_await ptr->spawnBatch(req);
        }
    );
}

std::shared_ptr<BaseAgent> AgentHost::createAgentInstance(std::shared_ptr<AgentConfig> config) {
    if (cfg_.agentFactory) {
        return cfg_.agentFactory(std::move(config));
    }
    // 默认: CodeAgent (轻量子代理配置下仅创建廉价内置工具, 不重复建连)
    return std::make_shared<CodeAgent>(std::move(config));
}

std::shared_ptr<AgentConfig> AgentHost::makeSubagentConfig(std::shared_ptr<AgentConfig> parentConfig
) const {
    assert(parentConfig && "makeSubagentConfig: parent config is null");
    auto cfg = std::make_shared<AgentConfig>(*parentConfig);
    // 子代理默认轻量: 不重复建立 MCP 连接 / 不加载插件 / RAG / CodeGraph,
    // 不注入父级 Skill/Memory (子代理上下文独立), 一次性运行不持久化
    cfg->mcpServerUrls.clear();
    cfg->plugins.clear();
    cfg->ragDocsPaths.clear();
    cfg->skillDirPaths.clear();
    cfg->memoryFilePaths.clear();
    cfg->enableSessionPersistence = false;
    cfg->sessionPersistenceRoot.clear();
    // 模型: 与旧 subgraph 语义一致, 默认使用配置的 subagent 模型
    if (cfg->subagentModel.has_value()) {
        cfg->model = *cfg->subagentModel;
    }
    return cfg;
}

std::string AgentHost::nextAgentId() {
    return fmt::format("agent_{}", ++agentIdSeq_);
}

void AgentHost::publishProgress(
    std::string_view agentId,
    std::string_view parentAgentId,
    std::string_view kind,
    std::string_view data
) {
    if (!hostBus_) {
        return;
    }
    asio::co_spawn(
        hostBus_->executor(),
        [bus           = hostBus_,
         agentId       = std::string{agentId},
         parentAgentId = std::string{parentAgentId},
         kind          = std::string{kind},
         data          = std::string{data}]() -> asio::awaitable<void> {
            co_await bus->publish<events::EventHostProgress>(
                events::HostTopic::AgentProgress,
                events::EventHostProgress{
                    .agentId       = agentId,
                    .parentAgentId = parentAgentId,
                    .kind          = kind,
                    .data          = data,
                }
            );
        },
        asio::detached
    );
}

asio::awaitable<events::RespSubagentResult> AgentHost::spawnSubagent(
    std::string_view                              subagentName,
    std::string_view                              systemPrompt,
    std::string_view                              message,
    std::string_view                              parentThreadId,
    std::shared_ptr<neograph::graph::CancelToken> cancelToken
) {
    // ---- 宿主预算检查 (深度 / 并发) ----
    size_t parentDepth = 0;
    if (auto it = threadDepth_.find(parentThreadId); it != threadDepth_.end()) {
        parentDepth = it->second;
    }
    const size_t depth = parentDepth + 1;
    if (depth > cfg_.maxDepth) {
        XX_LOGW(
            "AgentHost::spawnSubagent: `{}` rejected, depth {} exceeds maxDepth {}",
            subagentName,
            depth,
            cfg_.maxDepth
        );
        co_return events::RespSubagentResult{
            .hasError     = true,
            .errorMessage = fmt::format(
                "Subagent `{}` rejected: max nesting depth ({}) exceeded",
                subagentName,
                cfg_.maxDepth
            ),
        };
    }
    if (runningSubagentCount_ >= cfg_.maxConcurrentSubagents) {
        XX_LOGW(
            "AgentHost::spawnSubagent: `{}` rejected, concurrent limit {} reached",
            subagentName,
            cfg_.maxConcurrentSubagents
        );
        co_return events::RespSubagentResult{
            .hasError     = true,
            .errorMessage = fmt::format(
                "Subagent `{}` rejected: concurrent subagent limit ({}) reached",
                subagentName,
                cfg_.maxConcurrentSubagents
            ),
        };
    }

    // ---- 独立 agent 构造 (与根 agent 平等) ----
    auto parentConfig = rootAgent_ ? rootAgent_->getContext()->agentConfig : nullptr;
    if (!parentConfig) {
        co_return events::RespSubagentResult{
            .hasError     = true,
            .errorMessage = "AgentHost: root agent not attached, cannot derive subagent config",
        };
    }
    auto subagent = createAgentInstance(makeSubagentConfig(parentConfig));
    if (!subagent) {
        co_return events::RespSubagentResult{
            .hasError     = true,
            .errorMessage = "AgentHost: failed to create subagent instance",
        };
    }
    auto subCtx = subagent->getContext();
    if (!subCtx) {
        co_return events::RespSubagentResult{
            .hasError     = true,
            .errorMessage = "AgentHost: subagent context is null",
        };
    }
    // 注入共享基础设施与宿主引用
    subCtx->blockingPool = blockingPool_;
    subCtx->host         = weak_from_this();

    // 唯一运行 id 与 thread id
    const auto agentId  = nextAgentId();
    const auto parentId = std::string{parentThreadId};
    const auto subagentThreadId
        = fmt::format("subagent_{}_{}_{}", subagentName, agentId, agentIdSeq_);

    // 注册节点 (平等成员; 父为根节点)
    auto node           = std::make_shared<AgentNode>();
    node->agentId       = agentId;
    node->name          = std::string{subagentName};
    node->parentAgentId = "root";
    node->depth         = depth;
    node->agent         = subagent;
    registry_.insert(node);

    runningSubagentCount_++;
    threadDepth_[subagentThreadId] = depth;

    // 运行边界清理 (成功/错误/取消统一回收节点)
    struct SpawnCleanup {
        std::shared_ptr<AgentHost> host;
        std::string                agentId;
        std::string                subagentThreadId;
        bool                       done = false;

        void cleanup() {
            if (done) {
                return;
            }
            done = true;
            if (host) {
                host->destroyAgent(agentId);
                host->threadDepth_.erase(subagentThreadId);
                if (host->runningSubagentCount_ > 0) {
                    host->runningSubagentCount_--;
                }
            }
        }

        ~SpawnCleanup() {
            cleanup();
        }
    } cleanup{shared_from_this(), agentId, subagentThreadId};

    // ---- 运行 (engine 直跑: 保留调用方 executor, 无锁交错) ----
    co_return co_await agentxx::util::catchErrorAsync<events::RespSubagentResult>(
        [&]() -> asio::awaitable<events::RespSubagentResult> {
            co_await subagent->init();

            std::string sysPrompt{systemPrompt};
            if (sysPrompt.empty()) {
                sysPrompt = "你是一个专门处理用户请求的辅助助手.";
            }

            // HIL 冒泡: 子代理会话继承父会话的 io 与总线
            // (PermissionMiddleware 的 INTERRUPT 询问经 session->bus 派发,
            //  父 IO 已在该总线上注册 interrupt/permission 处理器)
            auto subSession = subCtx->getSession(subagentThreadId);
            if (!parentId.empty() && rootAgent_ && rootAgent_->getContext()) {
                auto parentSession = rootAgent_->getContext()->sessions->get(parentId);
                if (parentSession) {
                    subSession->io  = parentSession->io;
                    subSession->bus = parentSession->bus;
                }
            }

            XX_LOGD("    ## AgentHost spawned subagent `{}` (agentId={})", subagentName, agentId);

            neograph::graph::RunConfig cfg{
                .thread_id        = subagentThreadId,
                .input            = {{
                    "messages",
                    neograph::json::array({
                        {{"role", "system"}, {"content", sysPrompt}},
                        {{"role", "user"}, {"content", std::string{message}}},
                    }),
                }},
                .cancel_token     = cancelToken,
                .resume_if_exists = false,
            };

            std::ostringstream oss;
            auto               runResult = co_await subagent->getEngine()->run_stream_async(
                cfg,
                [&](const neograph::graph::GraphEvent& event) {
                    switch (event.type) {
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
                            if (kind == "content") {
                                oss << token;
                            }
                            // 进度事件 kind 规范: "token" | "thinking"
                            publishProgress(
                                agentId,
                                node->parentAgentId,
                                kind == "thinking" ? "thinking" : "token",
                                token
                            );
                        } break;
                        default:
                            break;
                    }
                }
            );

            events::RespSubagentResult resp;
            if (runResult.interrupted) {
                // 子代理被中断未完成: 显式报错 (子代理作用域不处理中断恢复)
                resp = events::RespSubagentResult{
                    .hasError     = true,
                    .errorMessage = fmt::format(
                        "Sub-agent interrupted at node `{}` (interrupt not handled in subagent scope)",
                        runResult.interrupt_node
                    ),
                };
            } else {
                resp = events::RespSubagentResult{.content = oss.str()};
            }
            resp.agentId = agentId;
            publishProgress(agentId, node->parentAgentId, "turn_end", "");

            // 结束事件 + 节点回收 (cleanup guard)
            if (hostBus_) {
                asio::co_spawn(
                    hostBus_->executor(),
                    [bus = hostBus_, agentId, hasError = resp.hasError, err = resp.errorMessage](
                    ) -> asio::awaitable<void> {
                        co_await bus->publish<events::EventHostDone>(
                            events::HostTopic::AgentDone,
                            events::EventHostDone{
                                .agentId       = agentId,
                                .parentAgentId = "",
                                .hasError      = hasError,
                                .errorMessage  = err,
                            }
                        );
                    },
                    asio::detached
                );
            }
            co_return resp;
        },
        [&agentId](std::string errmsg) -> asio::awaitable<events::RespSubagentResult> {
            // 节点回收由 SpawnCleanup guard 统一处理
            co_return events::RespSubagentResult{
                .hasError     = true,
                .errorMessage = fmt::format("Sub-agent failed: {}", errmsg),
                .agentId      = agentId,
            };
        },
        [&agentId](std::string& errmsg) -> std::optional<events::RespSubagentResult> {
            // 取消类异常: 转为错误结果快速返回 (父轮次随后按取消语义中止)
            return events::RespSubagentResult{
                .hasError     = true,
                .errorMessage = fmt::format("Sub-agent cancelled: {}", errmsg),
                .agentId      = agentId,
            };
        },
        // 传入取消令牌: operation_aborted 按取消语义处理
        cancelToken
    );
}

asio::awaitable<events::RespSubagentBatch> AgentHost::spawnBatch(const events::ReqSubagentBatch& req
) {
    events::RespSubagentBatch batchResp;
    if (req.tasks.empty()) {
        co_return batchResp;
    }
    auto ex                   = co_await asio::this_coro::executor;
    using ItemResult          = events::RespSubagentBatchItem;
    size_t                  n = req.tasks.size();
    std::vector<ItemResult> results(n);

    // 每个子代理 co_spawn 为独立协程, 完成后向 channel 发送 index (wait_for_all)
    auto doneChannel
        = std::make_shared<asio::experimental::channel<void(neograph_asio_error_code, size_t)>>(
            ex,
            static_cast<unsigned>(n)
        );

    for (size_t i = 0; i < n; ++i) {
        const auto& task = req.tasks[i];
        asio::co_spawn(
            ex,
            [this,
             task,
             &results,
             i,
             doneChannel,
             parentThreadId = req.parentThreadId,
             cancelToken    = req.cancelToken]() -> asio::awaitable<void> {
                // RAII 守卫: 无论 spawnSubagent 如何退出都保证发送完成信号
                struct BatchDoneGuard {
                    std::shared_ptr<
                        asio::experimental::channel<void(neograph_asio_error_code, size_t)>>
                           ch;
                    size_t idx;

                    ~BatchDoneGuard() {
                        if (ch) {
                            ch->async_send(
                                neograph_asio_error_code{},
                                idx,
                                [](neograph_asio_error_code) {}
                            );
                        }
                    }
                } guard{doneChannel, i};

                auto r = co_await agentxx::util::catchErrorAsync<events::RespSubagentResult>(
                    [&]() -> asio::awaitable<events::RespSubagentResult> {
                        co_return co_await spawnSubagent(
                            task.subagentName,
                            task.systemPrompt,
                            task.message,
                            parentThreadId,
                            cancelToken
                        );
                    },
                    [](std::string errmsg) -> asio::awaitable<events::RespSubagentResult> {
                        co_return events::RespSubagentResult{
                            .hasError     = true,
                            .errorMessage = fmt::format("Sub-agent failed: {}", std::move(errmsg)),
                        };
                    }
                );
                results[i] = ItemResult{
                    .resultId     = task.resultId,
                    .content      = r.content,
                    .hasError     = r.hasError,
                    .errorMessage = r.errorMessage,
                };
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

asio::awaitable<events::RespHostMessage> AgentHost::sendMessage(events::ReqHostMessage req) {
    // 本地 mailbox 路由 (扩展点: 持久会话 agent 挂接)
    auto it = mailboxes_.find(req.toAgentId);
    if (it != mailboxes_.end()) {
        co_return co_await it->second(req);
    }
    // 远程 agent 路由 (A2A 桥接): 本地与远程 agent 在消息面完全同构
    auto rit = remoteAgents_.find(req.toAgentId);
    if (rit != remoteAgents_.end()) {
        co_return co_await sendViaA2a(rit->second, req);
    }
    co_return events::RespHostMessage{
        .hasError     = true,
        .errorMessage = fmt::format(
            "Agent `{}` has no mailbox and is not registered as a remote agent: "
            "passive message injection requires a persistent agent session",
            req.toAgentId
        ),
    };
}

void AgentHost::registerRemoteAgent(
    std::string_view                            agentId,
    std::shared_ptr<agentxx::server::A2aClient> client
) {
    if (client) {
        remoteAgents_[std::string{agentId}] = std::move(client);
    } else {
        remoteAgents_.erase(agentId);
    }
}

void AgentHost::unregisterRemoteAgent(std::string_view agentId) {
    remoteAgents_.erase(agentId);
}

asio::awaitable<events::RespHostMessage> AgentHost::sendViaA2a(
    std::shared_ptr<agentxx::server::A2aClient> client,
    const events::ReqHostMessage&               req
) {
    // 1) SendMessage → 服务端创建 task
    auto sendResult = co_await client->sendMessage(req.message);
    if (!sendResult.has_value()) {
        co_return events::RespHostMessage{
            .hasError     = true,
            .errorMessage = fmt::format("A2A SendMessage failed: {}", sendResult.error()),
        };
    }
    auto taskId = agentxx::server::A2aClient::extractTaskId(sendResult.value());
    if (taskId.empty()) {
        co_return events::RespHostMessage{
            .hasError     = true,
            .errorMessage = "A2A SendMessage returned no task id",
        };
    }

    // 2) 轮询 GetTask 至终态 (500ms 间隔; 总超时 60s; 取消令牌联动)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        if (req.cancelToken && req.cancelToken->is_cancelled()) {
            co_return events::RespHostMessage{
                .hasError     = true,
                .errorMessage = fmt::format("A2A message to task `{}` cancelled", taskId),
            };
        }
        auto taskResult = co_await client->getTask(taskId);
        if (!taskResult.has_value()) {
            co_return events::RespHostMessage{
                .hasError     = true,
                .errorMessage = fmt::format("A2A GetTask failed: {}", taskResult.error()),
            };
        }
        const auto& task  = taskResult.value();
        auto        state = agentxx::server::A2aClient::extractTaskState(task);
        if (state == "TASK_STATE_COMPLETED") {
            co_return events::RespHostMessage{
                .content = agentxx::server::A2aClient::extractArtifactText(task),
            };
        }
        if (state == "TASK_STATE_FAILED" || state == "TASK_STATE_CANCELED"
            || state == "TASK_STATE_REJECTED") {
            co_return events::RespHostMessage{
                .hasError     = true,
                .errorMessage = fmt::format("A2A task `{}` ended with state {}", taskId, state),
            };
        }
        // 等待轮询间隔 (io 线程协作式挂起, 不阻塞事件循环)
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(500));
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    }

    co_return events::RespHostMessage{
        .hasError     = true,
        .errorMessage = fmt::format("A2A task `{}` timed out after 60s", taskId),
    };
}

void AgentHost::setMailbox(std::string_view agentId, Mailbox mailbox) {
    if (mailbox) {
        mailboxes_[std::string{agentId}] = std::move(mailbox);
    } else {
        mailboxes_.erase(agentId);
    }
}

void AgentHost::destroyAgent(std::string_view agentId) {
    // 递归移除全部子节点 (子树整体回收)
    for (auto& child : registry_.childrenOf(agentId)) {
        destroyAgent(child->agentId);
    }
    if (auto node = registry_.get(agentId)) {
        registry_.remove(agentId);
        // node->agent.reset() 释放独立 AgentContext / engine / SessionStore,
        // 该 agent 的全部会话与中间件状态随析构整体释放
        node->agent.reset();
    }
}

} // namespace agent
} // namespace agentxx
