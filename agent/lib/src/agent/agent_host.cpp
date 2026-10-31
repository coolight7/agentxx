#include "agentxx/agent/agent_host.h"

#include "agentxx/agent/agent_runner.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/protocol/a2a_client.h"
#include "agentxx/protocol/provider_common.h"
#include "agentxx/tools/subagent.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/task_scope.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/experimental/channel.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <filesystem>
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
    if (!node || node->agentId.empty()) {
        return;
    }
    // 重复 id 一律拒绝 (不覆盖): 覆盖会让旧节点成为孤儿 ——
    // 它的总线服务、子代理、会话仍在, 但谁也找不到它了
    if (nodes_.contains(node->agentId)) {
        XX_LOGE(
            "AgentRegistry::insert: agentId '{}' already registered, insert rejected",
            node->agentId
        );
        return;
    }
    nodes_.emplace(node->agentId, std::move(node));
}

void AgentRegistry::remove(std::string_view agentId) {
    // 异构查找删除, 免除 string_view→string 拷贝 (libc++ 无 C++23 异构 erase)
    utilxx_base::eraseHeterogeneous(nodes_, agentId);
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
    // agent 节点容器总线: 派生与跨 agent 消息服务
    std::weak_ptr<AgentHost> self = host;

    // 主 agent 上下文先挂上容器与共享线程池 (不等到 attachRoot):
    // 插件在 `start` 里就会建自己的根 (`agentxx.agent.runtime` 的 mount_root),
    // 而 start 发生在 attachRoot 之前 —— 容器引用只在 attachRoot 的 injectShared
    // 里给的话, 那时插件已经因为"本进程没有 agent 容器"被拒。
    // 两处赋值同一个值, 后到的 injectShared 不产生变化。
    if (auto primary = host->cfg_.primaryAgent.lock(); primary != nullptr) {
        primary->host       = self;
        primary->threadPool = host->threadPool_;
    }

    // agent.spawn (RR): 任意调用方经 agent 节点容器总线派生独立子代理
    // (包装为单任务批量请求, 统一走 spawnBatch 路径)
    host->hostBus_
        ->getRR<events::ReqHostSpawn, events::RespHostSpawn>(events::HostTopic::AgentSpawn)
        .registerServer(
            [self](const events::ReqHostSpawn& req, size_t)
                -> asio::awaitable<events::RespHostSpawn> {
                auto ptr = self.lock();
                if (!ptr) {
                    co_return events::RespHostSpawn{
                        .hasError     = true,
                        .errorMessage = "AgentHost no longer available",
                    };
                }
                auto batchResp = co_await ptr->spawnBatch(events::ReqSubagentBatch{
                    .parentAgentName = "",
                    .parentSessionId  = req.parentAgentId,
                    .cancelToken     = req.cancelToken,
                    .tasks           = {
                        events::SubagentBatchItem{
                            .subagentName = req.name,
                            .systemPrompt = req.systemPrompt,
                            .message      = req.message,
                            .resultId     = "",
                        },
                    },
                });
                if (batchResp.results.empty()) {
                    co_return events::RespHostSpawn{
                        .hasError     = true,
                        .errorMessage = "AgentHost: spawn returned no result",
                    };
                }
                const auto& item = batchResp.results[0];
                co_return events::RespHostSpawn{
                    .agentId      = item.agentId,
                    .content      = item.content,
                    .hasError     = item.hasError,
                    .errorMessage = item.errorMessage,
                };
            }
        );

    // agent.message (RR): 任意→任意 agent 消息 (mailbox 路由)
    host->hostBus_
        ->getRR<events::ReqHostMessage, events::RespHostMessage>(events::HostTopic::AgentMessage)
        .registerServer(
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
    size_t poolThreads = cfg_.threadPoolWorkers;
    if (0 == poolThreads) {
        poolThreads = std::max(2u, std::thread::hardware_concurrency() / 2);
    }
    threadPool_ = std::make_shared<asio::thread_pool>(poolThreads);
    hostBus_    = std::make_shared<agentxx::events::EventBus>(ioCtx_->get_executor());
}

AgentHost::~AgentHost() = default;

std::shared_ptr<asio::io_context> AgentHost::ioCtx() {
    return ioCtx_;
}

std::shared_ptr<agentxx::events::EventBus> AgentHost::hostBus() {
    return hostBus_;
}

std::shared_ptr<asio::thread_pool> AgentHost::threadPool() {
    return threadPool_;
}

AgentRegistry& AgentHost::registry() {
    return registry_;
}

std::shared_ptr<agentxx::plugin::PluginManager> AgentHost::pluginHost() const {
    return pluginHost_;
}

asio::awaitable<void> AgentHost::loadPluginsAsync() {
    if (pluginHost_) {
        co_return; // 幂等: 插件只加载一次
    }
    if (cfg_.pluginList.empty()) {
        co_return;
    }
    // 管理器级: 加载阶段还没有挂载, 但插件 `start` 的进程级注册需要主 agent 的上下文
    // (总线 / 语言 / 模型), 所以由调用方把主 agent 交给管理器
    pluginHost_ = std::make_shared<agentxx::plugin::PluginManager>(cfg_.primaryAgent);
    // offload 工作线程池直接注入: 插件阻塞工具不依赖主 agent 上下文是否已挂载
    pluginHost_->setWorkerThreadPool(threadPool_);
    pluginHost_->setIoExecutor(ioCtx_->get_executor());
    XX_LOGI("Loading {} plugin(s) into the agent host", cfg_.pluginList.size());
    co_await pluginHost_->loadConfiguredPlugins(cfg_.pluginList);
    XX_LOGI("Plugin host ready: {} instance(s)", pluginHost_->list().size());
    co_return;
}

size_t AgentHost::runningSubagents() const {
    return runningSubagentCount_;
}

std::shared_ptr<BaseAgent> AgentHost::rootAgent() const {
    return rootAgent_;
}

namespace {

/// 规范化会话存储根 (比较用): 绝对路径 + 消解符号链接; Windows 统一小写
std::string normalizeStoreRoot(std::string_view root) {
    namespace fs = std::filesystem;
    if (root.empty()) {
        return {};
    }
    std::error_code ec;
    fs::path        path = utilxx_base::utf8ToPath(std::string{root});
    fs::path        abs  = fs::absolute(path, ec);
    if (ec) {
        abs = path;
    }
    ec.clear();
    fs::path canon = fs::weakly_canonical(abs, ec);
    if (ec) {
        canon = abs.lexically_normal();
    }
    std::string out = utilxx_base::pathToUtf8Generic(canon);
    // 去掉末尾分隔符: `weakly_canonical` 会保留输入里的结尾分隔符,
    // 否则 `.../a` 与 `.../a/` 会变成两个不同的键
    while (out.size() > 1 && (out.back() == '/' || out.back() == '\\')) {
        out.pop_back();
    }
#if XX_IS_WIN_D
    // Windows 路径不区分大小写: 统一小写后再比较
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return out;
}

} // namespace

bool AgentHost::claimSessionStoreRoot(AgentContext& ctx, std::string_view agentId) {
    if (!ctx.sessions || !ctx.sessions->sessionStore) {
        // 没有会话存储 (子代理默认): 不占用存储根, 也就不用比较
        return true;
    }
    const std::string root = normalizeStoreRoot(ctx.sessions->sessionStore->rootDir());
    if (root.empty()) {
        return true;
    }
    const std::string id{agentId.empty() ? std::string_view{"root"} : agentId};
    auto              it = storeRoots_.find(root);
    if (it != storeRoots_.end() && it->second != id) {
        XX_LOGE(
            "会话存储根 '{}' 已被 agent '{}' 打开, 拒绝 agent '{}' 再打开同一存储根 "
            "(同一份磁盘会话只能由一个 agent 写)",
            root,
            it->second,
            id
        );
        return false;
    }
    storeRoots_[root] = id;
    return true;
}

void AgentHost::releaseSessionStoreRoot(std::string_view agentId) {
    std::erase_if(storeRoots_, [agentId](const auto& item) {
        return item.second == agentId;
    });
}

utilxx_base::Json AgentHost::sessionStoreOwnersJson() const {
    utilxx_base::Json out = utilxx_base::Json::object();
    for (const auto& [root, agentId] : storeRoots_) {
        out[root] = agentId;
    }
    return out;
}

void AgentHost::attachRoot(std::shared_ptr<BaseAgent> rootAgent) {
    assert(rootAgent && "attachRoot: root agent is null");
    if (rootAgent_) {
        XX_LOGW("AgentHost::attachRoot: root already attached, ignored");
        return;
    }
    MountSpec spec;
    spec.agentId  = "root";
    spec.primary  = true;
    spec.owner    = {};
    if (mountRoot(std::move(rootAgent), std::move(spec)).empty()) {
        XX_LOGE("AgentHost::attachRoot: 拒绝挂载该 agent (见上方原因)");
    }
}

void AgentHost::injectShared(std::shared_ptr<agentxx::agent::AgentContext> ctx) {
    if (!ctx) {
        return;
    }
    // 共享基础设施: threadPool 上提共享 (避免每 agent 一份线程池),
    // host 引用 (供节点/工具经 AgentContext 知道 agent 节点容器)
    ctx->threadPool = threadPool_;
    ctx->host       = weak_from_this();
    // 插件管理器: 一律用容器这一份 (进程唯一)。
    // 不注入的话 BaseAgent::init 会自建一份管理器 (ownsPluginHost_=true),
    // 那就又跑了一份插件实例 —— 插件的根也因此会跟着建第二份管理器。
    // policy 为 None 的 agent 不挂任何插件, 也不注管理器。
    if (pluginHost_ && !ctx->pluginManager
        && ctx->agentConfig->pluginMountPolicy != PluginMountPolicy::None) {
        ctx->pluginManager      = pluginHost_;
        ctx->pluginManagerOwned = false; // 进程共享管理器: 不随本 agent 析构卸载
    }
}

size_t AgentHost::registerSubagentService(std::shared_ptr<BaseAgent> rootAgent) {
    if (!rootAgent) {
        return 0;
    }
    auto ctx = rootAgent->getContext();
    if (!ctx || !ctx->bus) {
        XX_LOGW("AgentHost: root agent bus is null, subagent delegation unavailable for it");
        return 0;
    }
    // 根的全局总线上 registerServer 子代理委派 (中断路径, 统一批量语义):
    // - SubAgentManagerTool 抛 NodeInterrupt → BaseAgent 中断循环 →
    //   agentContext->bus->request(service.subagent, ReqSubagentBatch) → 到达本容器
    // - 容器派生独立 agent 运行 (单任务 = 1 个 task), 结果经 interruptResult 回填
    // - 闭包把该根的上下文带进 spawnBatch: 派生配置/工具继承/用量记账
    //   都落到那一个根 (多根场景下不能串到内置根)
    std::weak_ptr<AgentHost>    self = weak_from_this();
    std::weak_ptr<AgentContext> rootCtx = ctx;
    auto&                       rr = ctx->bus->getRR<events::ReqSubagentBatch, events::RespSubagentBatch>(
        events::Topic::Subagent
    );
    return rr.registerServer(
        [self, rootCtx](const events::ReqSubagentBatch& req, size_t)
            -> asio::awaitable<events::RespSubagentBatch> {
            auto ptr = self.lock();
            if (!ptr) {
                co_return events::RespSubagentBatch{};
            }
            co_return co_await ptr->spawnBatch(req, rootCtx.lock());
        }
    );
}

std::string AgentHost::mountRoot(std::shared_ptr<BaseAgent> agent, MountSpec spec) {
    assert(agent && "mountRoot: agent is null");
    if (!agent) {
        return {};
    }
    auto ctx = agent->getContext();
    assert(ctx && "mountRoot: agent context is null");
    if (!ctx) {
        return {};
    }

    // ---- 预算: 根数上限 ----
    if (roots_.size() >= cfg_.maxRoots) {
        XX_LOGE(
            "AgentHost::mountRoot: 拒绝挂载, 根数已达上限 {} (AgentHost::Config::maxRoots)",
            cfg_.maxRoots
        );
        return {};
    }

    // ---- agentId: 空则生成 `ext:<owner>:<n>` ----
    std::string agentId = spec.agentId;
    if (agentId.empty()) {
        const std::string owner = spec.owner.empty() ? std::string{"host"} : spec.owner;
        agentId                 = fmt::format("ext:{}:{}", owner, ++rootSeq_);
    }
    // 重复 id 一律拒绝: 覆盖会让旧节点成为孤儿 (它的总线服务还在, 但谁也找不到)
    if (registry_.contains(agentId) || findMountedRoot(agentId) != nullptr) {
        XX_LOGE(
            "AgentHost::mountRoot: 拒绝挂载, agentId '{}' 已被占用 (不覆盖现有节点)",
            agentId
        );
        return {};
    }
    if (!ctx->agentId.empty() && ctx->agentId != "root" && ctx->agentId != agentId) {
        // 上下文里的 agentId 是插件引用的身份, 与容器的登记 id 必须一致
        XX_LOGW(
            "AgentHost::mountRoot: 上下文 agentId '{}' 与挂载 id '{}' 不一致, 以挂载 id 为准",
            ctx->agentId,
            agentId
        );
    }
    ctx->agentId = agentId;

    // ---- 会话存储根: 同一份磁盘会话只能由一个 agent 写 ----
    if (!claimSessionStoreRoot(*ctx, agentId)) {
        XX_LOGE("AgentHost::mountRoot: 拒绝挂载 '{}' (会话存储根已被别的 agent 占用)", agentId);
        return {};
    }

    // ---- 共享基础设施 + 插件管理器 ----
    injectShared(ctx);

    // ---- 节点登记: 根一律 depth=0 / 无父节点 ----
    auto node               = std::make_shared<AgentNode>();
    node->agentId           = agentId;
    node->name              = ctx->agentConfig ? ctx->agentConfig->agentName : agentId;
    node->parentAgentId     = "";
    node->depth             = 0;
    node->agent             = agent;
    registry_.insert(std::move(node));

    // ---- 子代理委派服务 (每根一个) ----
    MountedRoot mounted;
    mounted.agentId = agentId;
    mounted.owner   = spec.owner;
    mounted.primary = spec.primary;
    mounted.agent   = agent;
    if (spec.registerSubagentService) {
        mounted.subagentServerId = registerSubagentService(agent);
    }
    if (spec.primary) {
        rootAgent_ = agent;
    }
    roots_.push_back(std::move(mounted));

    XX_LOGI(
        "AgentHost: mounted root '{}' (owner='{}', primary={}, subagentService={}, roots={}/{})",
        agentId,
        spec.owner,
        spec.primary,
        roots_.back().subagentServerId != 0,
        roots_.size(),
        cfg_.maxRoots
    );
    return agentId;
}

std::shared_ptr<BaseAgent> AgentHost::createAgent(std::shared_ptr<AgentConfig> config) {
    auto agent = createAgentInstance(std::move(config));
    if (!agent) {
        return nullptr;
    }
    // 根与容器共用同一条 IO 线程
    //
    // `BaseAgent` 构造时会自建一个 io_context, 那条线程只有人主动 run() 才会动。
    // 子代理无所谓 (它的引擎在调用方 executor 上直跑), 但根不是: 会话驱动循环、
    // 事件总线、通道的收发协程全在这条线程上, 没有线程 run 就永远卡住。
    // 因此这里把新建根的 ioCtx 换成容器这一条, 并把总线重建到同一执行器上
    // (构造期建的总线挂在旧执行器上; 此刻还没有任何订阅, 重建是安全的)。
    if (agent->ioCtx.get() != ioCtx_.get()) {
        agent->ioCtx = ioCtx_;
        if (agent->agentContext) {
            agent->agentContext->bus
                = std::make_shared<agentxx::events::EventBus>(ioCtx_->get_executor());
        }
    }
    return agent;
}

MountedRoot* AgentHost::findMountedRoot(std::string_view agentId) {
    for (auto& item : roots_) {
        if (item.agentId == agentId) {
            return &item;
        }
    }
    return nullptr;
}

bool AgentHost::unmountRoot(std::string_view agentId) {
    auto it = std::find_if(roots_.begin(), roots_.end(), [agentId](const MountedRoot& item) {
        return item.agentId == agentId;
    });
    if (it == roots_.end()) {
        return false;
    }
    if (it->primary) {
        // 内置根不随外部请求卸载: 它是进程自己的会话入口
        XX_LOGW("AgentHost::unmountRoot: 内置根 '{}' 不能卸载", agentId);
        return false;
    }
    // 调用方看到的结果立即生效: 子代理服务注销、根表与节点表摘掉、会话目录占用释放。
    // 之后 rootCount() / findRoot() / registry().contains() / 挂根预算都不再算这个根。
    std::string id{agentId};
    unregisterSubagentService(id);
    roots_.erase(it);
    if (rootAgent_ && rootAgent_->getContext() && rootAgent_->getContext()->agentId == id) {
        rootAgent_.reset();
    }
    auto pending = detachNodeForClose(id);
    releaseSessionStoreRoot(id);
    // 真正的销毁要等该 agent 的插件 stop 与 session 落盘 (agent->shutdownAsync), 只能是
    // 协程: 投递到容器 io 线程保活执行, 不在当前调用栈同步做 —— 插件在自己的 stop 里
    // 卸根时, 实例还挂在该根的挂载上 (实例未进入 Closed, 插件 unmount 回调还能跑),
    // 同步打掉 agent 会留下"实例已关但挂载记录还在"的残局。
    if (!pending.empty()) {
        scheduleShutdownAsync(std::move(pending));
    }
    XX_LOGI("AgentHost: unmounted root '{}'", agentId);
    return true;
}

std::vector<AgentHost::PendingClose> AgentHost::detachNodeForClose(std::string_view agentId) {
    std::vector<PendingClose> out;
    // 先子后父: 父上下文不能在子节点关闭完成前释放 (与 destroyAgentAsync 同一顺序)
    for (auto& child : registry_.childrenOf(agentId)) {
        if (!child) {
            continue;
        }
        auto sub = detachNodeForClose(child->agentId);
        out.insert(
            out.end(),
            std::make_move_iterator(sub.begin()),
            std::make_move_iterator(sub.end())
        );
    }
    if (auto node = registry_.get(agentId)) {
        auto agent = node->agent;
        registry_.remove(agentId);
        if (agent) {
            out.push_back(PendingClose{std::move(agent), std::string{agentId}});
        }
    }
    return out;
}

void AgentHost::scheduleShutdownAsync(std::vector<PendingClose> pending) {
    auto self = weak_from_this().lock();
    if (!self) {
        // 容器已在销毁: 节点留给 pending 按引用计数释放
        XX_LOGW("AgentHost: cannot schedule shutdown ({} agent(s)); host is going away", pending.size());
        return;
    }
    auto ex = ioCtx_->get_executor();
    asio::post(
        ex,
        [self, pending = std::move(pending)]() mutable {
            asio::co_spawn(
                self->ioCtx_->get_executor(),
                [self, pending = std::move(pending)]() mutable -> asio::awaitable<void> {
                    for (auto& item : pending) {
                        if (!item.agent) {
                            continue;
                        }
                        if (!co_await item.agent->shutdownAsync(std::chrono::seconds{30})) {
                            XX_LOGE(
                                "AgentHost: deferred shutdown failed for agent `{}`",
                                item.agentId
                            );
                        }
                    }
                },
                asio::detached
            );
        }
    );
}

size_t AgentHost::unmountRootsByOwner(std::string_view owner) {
    // 先取 id 快照: unmountRoot 会改 roots_
    std::vector<std::string> ids;
    for (const auto& item : roots_) {
        if (!item.primary && item.owner == owner) {
            ids.push_back(item.agentId);
        }
    }
    size_t removed = 0;
    for (const auto& id : ids) {
        if (unmountRoot(id)) {
            ++removed;
        }
    }
    return removed;
}

void AgentHost::unregisterSubagentService(std::string_view agentId) {
    auto* mounted = findMountedRoot(agentId);
    if (!mounted || mounted->subagentServerId == 0) {
        return;
    }
    if (auto ctx = mounted->agent ? mounted->agent->getContext() : nullptr; ctx && ctx->bus) {
        ctx->bus->getRR<events::ReqSubagentBatch, events::RespSubagentBatch>(
                      events::Topic::Subagent
        )
            .unregisterServer(mounted->subagentServerId);
    }
    mounted->subagentServerId = 0;
}

std::shared_ptr<BaseAgent> AgentHost::findRoot(std::string_view agentId) const {    for (const auto& item : roots_) {
        if (item.agentId == agentId) {
            return item.agent;
        }
    }
    return nullptr;
}

std::vector<MountedRoot> AgentHost::roots() const {
    return roots_;
}

bool AgentHost::claimSession(
    std::string_view storeRoot,
    std::string_view sessionId,
    std::string_view owner
) {
    if (sessionId.empty()) {
        return false;
    }
    const std::string key = normalizeStoreRoot(storeRoot) + "|" + std::string{sessionId};
    std::lock_guard<std::mutex> lock(sessionOccupancyMutex_);
    auto                        it = sessionOccupancy_.find(key);
    if (it != sessionOccupancy_.end()) {
        if (it->second == owner) {
            return true; // 同一归属者重复占用: 幂等
        }
        XX_LOGE(
            "AgentHost::claimSession: 会话 '{}' (存储根 '{}') 已被 '{}' 占用, 拒绝 '{}' 驱动同一会话",
            sessionId,
            storeRoot,
            it->second,
            owner
        );
        return false;
    }
    sessionOccupancy_.emplace(key, std::string{owner});
    return true;
}

void AgentHost::releaseSession(
    std::string_view storeRoot,
    std::string_view sessionId,
    std::string_view owner
) {
    if (sessionId.empty()) {
        return;
    }
    const std::string           key = normalizeStoreRoot(storeRoot) + "|" + std::string{sessionId};
    std::lock_guard<std::mutex> lock(sessionOccupancyMutex_);
    auto                        it = sessionOccupancy_.find(key);
    if (it == sessionOccupancy_.end()) {
        return;
    }
    if (it->second != owner) {
        XX_LOGW(
            "AgentHost::releaseSession: 会话 '{}' 属于 '{}', 拒绝 '{}' 释放 (防误释放别人的会话)",
            sessionId,
            it->second,
            owner
        );
        return;
    }
    sessionOccupancy_.erase(it);
}

utilxx_base::Json AgentHost::sessionOccupancyJson() const {
    utilxx_base::Json out = utilxx_base::Json::object();
    std::lock_guard<std::mutex> lock(sessionOccupancyMutex_);
    for (const auto& [key, owner] : sessionOccupancy_) {
        out[key] = owner;
    }
    return out;
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
    // 子代理默认不挂载任何插件 (最小权限; 要工具由调用方显式给 Listed)
    cfg->pluginMountPolicy = PluginMountPolicy::None;
    cfg->pluginMountList.clear();
    cfg->ragDocsPaths.clear();
    cfg->skillDirPaths.clear();
    cfg->memoryFilePaths.clear();
    cfg->enableSessionStore = false;
    cfg->sessionStoreDirectory.clear();
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
    auto task = [bus           = hostBus_,
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
    };
    // 直接投到容器总线上 (不借某个根的 taskScope): 多根时进度不该挂在某一个根上,
    // 根的关闭也不能把别的根的进度任务一并取消
    asio::co_spawn(hostBus_->executor(), std::move(task), asio::detached);
}

asio::awaitable<events::RespSubagentBatchItem> AgentHost::spawnOneTask(
    const events::SubagentBatchItem&              task,
    std::string_view                              parentSessionId,
    std::shared_ptr<neograph::graph::CancelToken> cancelToken,
    std::shared_ptr<AgentContext>                 parentAgentCtx
) {
    if (cancelToken && cancelToken->is_cancelled()) {
        co_return events::RespSubagentBatchItem{
            .resultId     = task.resultId,
            .hasError     = true,
            .cancelled    = true,
            .errorMessage = "Subagent cancelled before execution",
        };
    }

    const auto& subagentName = task.subagentName;
    // ---- agent 节点容器预算检查 (深度 / 并发) ----
    size_t parentDepth = 0;
    if (auto it = sessionDepth_.find(parentSessionId); it != sessionDepth_.end()) {
        parentDepth = it->second;
    }
    const size_t depth = parentDepth + 1;
    if (depth > cfg_.maxDepth) {
        XX_LOGW(
            "AgentHost::spawnOneTask: `{}` rejected, depth {} exceeds maxDepth {}",
            subagentName,
            depth,
            cfg_.maxDepth
        );
        co_return events::RespSubagentBatchItem{
            .resultId     = task.resultId,
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
            "AgentHost::spawnOneTask: `{}` rejected, concurrent limit {} reached",
            subagentName,
            cfg_.maxConcurrentSubagents
        );
        co_return events::RespSubagentBatchItem{
            .resultId     = task.resultId,
            .hasError     = true,
            .errorMessage = fmt::format(
                "Subagent `{}` rejected: concurrent subagent limit ({}) reached",
                subagentName,
                cfg_.maxConcurrentSubagents
            ),
        };
    }

    // ---- 独立 agent 构造 (与根 agent 平等) ----
    // 父配置取**发起请求那一个根**的: 多根时子代理的工具继承/记账/提示词
    // 都该跟随它所在的根, 不能一律用内置根
    auto requestCtx = parentAgentCtx ? parentAgentCtx : (rootAgent_ ? rootAgent_->getContext() : nullptr);
    auto parentConfig = requestCtx ? requestCtx->agentConfig : nullptr;
    if (!parentConfig) {
        co_return events::RespSubagentBatchItem{
            .resultId     = task.resultId,
            .hasError     = true,
            .errorMessage = "AgentHost: root agent not attached, cannot derive subagent config",
        };
    }
    auto subCfg = makeSubagentConfig(parentConfig);
    /// 同上下文模式: 任务指定了 sessionId (子代理与父会话共享上下文前缀与模型)
    const bool sameContext = !task.sessionId.empty();

    // ---- worktree 绑定继承 (worktree 模式) ----
    // - 父会话已绑定 worktree 时, 子代理的工作目录预置为同一 worktree:
    //   resolvedWorkDir()/permission Ask 默认放行规则/子进程 cwd 一并跟随,
    //   无需子代理再了解绑定机制; inheritedWorktreePath 供提示词中间件注入
    //   "继承隔离" 提醒 (避免子代理再创建嵌套 worktree 或改写主检出)
    if (parentAgentCtx) {
        auto parentSession = parentAgentCtx->sessions->get(parentSessionId);
        if (parentSession && !parentSession->getWorktreeBinding().path.empty()) {
            const auto& wb                = parentSession->getWorktreeBinding();
            subCfg->workDir               = wb.path;
            subCfg->inheritedWorktreePath = wb.path;
        }
    }

    // ---- 子代理工具策略 (无工具 / 继承父 / 自定义白名单) ----
    if (task.tools.has_value()) {
        if (task.tools->is_array()) {
            subCfg->enableToolFiltering = true;
            subCfg->toolWhitelist.clear();
            if (task.tools->empty()) {
                // []: 无工具 (纯文本回答)
                XX_LOGD("spawnOneTask `{}`: tool policy = none", subagentName);
            } else if (task.tools->size() == 1 && (*task.tools)[0].is_string()
                       && (*task.tools)[0].get<std::string>() == "*") {
                // ["*"]: 全部继承父 agent 工具 (解析为父工具名白名单;
                // 子代理未创建的父工具名自然跳过, 不报错)
                // 父 = 发起请求那个根 (多根时不能一律取内置根)
                if (requestCtx) {
                    subCfg->toolWhitelist = requestCtx->toolNames;
                }
                XX_LOGD(
                    "spawnOneTask `{}`: tool policy = inherit parent ({} tools)",
                    subagentName,
                    subCfg->toolWhitelist.size()
                );
            } else {
                // [name, ...]: 自定义白名单
                for (const auto& item : *task.tools) {
                    if (item.is_string()) {
                        subCfg->toolWhitelist.push_back(item.get<std::string>());
                    }
                }
                XX_LOGD(
                    "spawnOneTask `{}`: tool policy = custom ({} tools)",
                    subagentName,
                    subCfg->toolWhitelist.size()
                );
            }
        } else {
            XX_LOGW("spawnOneTask `{}`: invalid `tools` (not array), ignored", subagentName);
        }
    }

    // ---- 子代理上下文压缩开关 (缺省继承父 config 拷贝) ----
    if (task.enableSummarization.has_value()) {
        subCfg->enableSummarization = *task.enableSummarization;
        XX_LOGD(
            "spawnOneTask `{}`: enableSummarization = {}",
            subagentName,
            *task.enableSummarization
        );
    }

    // ---- 同上下文运行: 前缀复用与工具执行策略 ----
    // - keepProvidedSystemMessage: 保留调用方给的 system 消息 (子代理按自己的配置
    //   重建时会缺少运行时提示词段落, 前缀在 system 内部就不一致)
    // - inheritRequestTools: 继承父会话上一次请求的工具定义 (请求前缀一致)
    // - toolExecutionDisabled: 本次运行不执行任何工具 (工具定义只用于保持前缀一致)
    if (sameContext) {
        subCfg->keepProvidedSystemMessage = task.keepProvidedSystemMessage;
        subCfg->toolExecutionDisabled     = task.toolExecutionDisabled;

        if (task.inheritRequestTools) {
            // 父会话上一次请求的工具定义 (父上下文优先, 嵌套委派时是上一级子代理)
            std::shared_ptr<const std::vector<neograph::ChatTool>> parentTools = nullptr;
            if (parentAgentCtx && parentAgentCtx->lastRequestTools) {
                parentTools = parentAgentCtx->lastRequestTools;
            } else if (requestCtx) {
                parentTools = requestCtx->lastRequestTools;
            }
            if (parentTools && !parentTools->empty()) {
                // 继承时父工具定义必须原样出现在请求里 (数量/顺序/内容逐字节一致),
                // 因此关闭工具白名单过滤: 子代理本来也不执行工具
                subCfg->enableToolFiltering = false;
                subCfg->toolWhitelist.clear();
                subCfg->requestToolsOverride = std::move(parentTools);
                XX_LOGD(
                    "spawnOneTask `{}`: inherit request tools ({} definitions)",
                    subagentName,
                    subCfg->requestToolsOverride->size()
                );
            } else {
                // 捕获不到 (进程刚启动 / 没有历史请求): 退回旧行为, 不承诺命中
                XX_LOGD(
                    "spawnOneTask `{}`: no captured request tools, inherit skipped",
                    subagentName
                );
            }
        }
    }

    auto subagent = createAgentInstance(std::move(subCfg));
    if (!subagent) {
        co_return events::RespSubagentBatchItem{
            .resultId     = task.resultId,
            .hasError     = true,
            .errorMessage = "AgentHost: failed to create subagent instance",
        };
    }
    auto subCtx = subagent->getContext();
    if (!subCtx) {
        co_return events::RespSubagentBatchItem{
            .resultId     = task.resultId,
            .hasError     = true,
            .errorMessage = "AgentHost: subagent context is null",
        };
    }
    /// 注入共享基础设施与 agent 节点容器引用
    subCtx->threadPool = threadPool_;
    subCtx->host       = weak_from_this();
    // 插件管理器: 子代理按自己的挂载策略走 (默认 None = 不挂载任何插件)
    if (pluginHost_ && !subCtx->pluginManager
        && subCtx->agentConfig->pluginMountPolicy != PluginMountPolicy::None) {
        subCtx->pluginManager      = pluginHost_;
        subCtx->pluginManagerOwned = false; // 进程共享管理器: 不随子代理析构卸载
    }

    // 用量记账目标: 子代理的模型调用计入父会话 (用户看的会话与台账都是父会话)
    // - 子代理自己有独立 AgentContext/SessionsManager, 没有 sessionStore, 记不下来
    // - 嵌套委派沿用最外层已确定的目标, 保证一次调用只记一笔 (不重复计数)
    // - 自动压缩这类内部委派经总线到达 (`spawnBatch(req)` 不带父上下文),
    //   此时回退到**发起方所在根**的会话存储 (多根时必须按根回退,
    //   否则挂载根的用量会被记到内置根上)
    if (parentAgentCtx) {
        if (parentAgentCtx->usageAccountSessions) {
            subCtx->usageAccountSessions  = parentAgentCtx->usageAccountSessions;
            subCtx->usageAccountSessionId = parentAgentCtx->usageAccountSessionId;
        } else if (parentAgentCtx->sessions) {
            subCtx->usageAccountSessions  = parentAgentCtx->sessions;
            subCtx->usageAccountSessionId = std::string{parentSessionId};
        }
    }
    if (!subCtx->usageAccountSessions && requestCtx && requestCtx->sessions) {
        subCtx->usageAccountSessions  = requestCtx->sessions;
        subCtx->usageAccountSessionId = std::string{parentSessionId};
    }

    // 同上下文模式: 子代理的 share_store 工具桥接到父会话的 store
    // (id 空间一致: 子代理写入的长内容, 父会话按摘要中的 id 可直接读取)
    if (sameContext && requestCtx && requestCtx->middlewareHandleContext) {
        subCtx->agentConfig->sharedShareStoreContext = requestCtx->middlewareHandleContext;
    }

    // 唯一运行 id 与 session id
    const auto agentId  = nextAgentId();
    const auto parentId = std::string{parentSessionId};
    // 会话存储根独占: 子代理默认没有会话存储 (直接通过); 调用方自己开了存储时
    // 同一存储根已被别的 agent 占用就拒绝派生 (否则写库时才会静默串数据)
    if (!claimSessionStoreRoot(*subCtx, agentId)) {
        co_return events::RespSubagentBatchItem{
            .resultId     = task.resultId,
            .hasError     = true,
            .errorMessage = fmt::format(
                "AgentHost: session store root already opened by another agent, refusing "
                "to spawn `{}`",
                agentId
            ),
        };
    }
    // 同上下文模式: 子代理直接运行在指定 session (与父会话同 session),
    // 共享上下文前缀; 默认模式使用独立 subagent 线程 id (上下文隔离)
    const auto subagentSessionId
        = sameContext ? task.sessionId
                      : fmt::format("subagent_{}_{}_{}", subagentName, agentId, agentIdSeq_);

    // 注册节点 (平等成员; 父为根节点)
    auto node           = std::make_shared<AgentNode>();
    node->agentId       = agentId;
    node->name          = std::string{subagentName};
    node->parentAgentId = "root";
    node->depth         = depth;
    node->agent         = subagent;
    registry_.insert(node);

    runningSubagentCount_++;
    if (!sameContext) {
        // 同上下文模式共享父线程: 不覆盖父线程深度记录
        utilxx_base::insertOrAssignHeterogeneous(sessionDepth_, subagentSessionId, depth);
    }

    // 运行边界清理 (成功/错误/取消统一回收节点)
    struct SpawnCleanup {
        std::shared_ptr<AgentHost> host;
        std::string                agentId;
        std::string                subagentSessionId;
        bool                       sharedSession = false;
        bool                       done          = false;

        void releaseAccounting() {
            if (done) {
                return;
            }
            done = true;
            if (host) {
                if (!sharedSession) {
                    utilxx_base::eraseHeterogeneous(host->sessionDepth_, subagentSessionId);
                }
                if (host->runningSubagentCount_ > 0) {
                    host->runningSubagentCount_--;
                }
            }
        }

        ~SpawnCleanup() {
            // 节点释放由 spawnOneTask 在所属 IO executor 上显式 await；
            // 析构只负责预算计数，避免同步 reset 仍在运行的 AgentContext。
            releaseAccounting();
        }
    } cleanup{shared_from_this(), agentId, subagentSessionId, sameContext};

    // ---- 运行 (engine 直跑: 保留调用方 executor, 无锁交错) ----
    auto response = co_await agentxx::util::catchErrorAsync<events::RespSubagentBatchItem>(
        [&]() -> asio::awaitable<events::RespSubagentBatchItem> {
            co_await subagent->init();

            // 同上下文模式 + messages 原样转发: 初始上下文由调用方原样给出
            // (须含 system, 或为空时由 llm 节点按父 system prompt 补齐),
            // 不再插入子代理默认提示, 保证消息前缀与父会话一致
            std::string sysPrompt{task.systemPrompt};
            if (sysPrompt.empty() && !sameContext) {
                sysPrompt = "你是一个专门处理用户请求的辅助助手.";
            }

            // HIL 冒泡: 子代理会话继承父会话的 io 与总线
            // (PermissionMiddleware 的 INTERRUPT 询问经 session->bus 派发,
            //  父 IO 已在该总线上注册 interrupt/permission 处理器)
            // - 父会话从 parentAgentCtx (父 agent 的 SessionStore) 查找:
            //   嵌套委派时父上下文是上一级子代理 (而非根), 保证任意深度
            //   的 HIL 都能找到正确的父会话链
            auto subSession = subCtx->getSession(subagentSessionId);
            if (!parentId.empty()) {
                // 父上下文优先 (嵌套委派时是上一级子代理), 否则用发起请求那个根
                auto parentCtx = parentAgentCtx ? parentAgentCtx : requestCtx;
                if (parentCtx) {
                    auto parentSession = parentCtx->sessions->get(parentId);
                    if (parentSession) {
                        subSession->io  = parentSession->io;
                        subSession->bus = parentSession->bus;
                        if (sameContext) {
                            // 同上下文模式: 强制使用父会话当前模型 (忽略 subagentModel),
                            // 与父会话相同模型 + 相同 session + 相同上下文前缀,
                            // 三条件共同保证命中 provider KV/prefix cache
                            subSession->setModelName(parentSession->getModelName());
                        }
                    }
                }
            }

            // ---- 子代理委派总线 registerServer (扁平化核心) ----
            // 子代理作用域内的 "subagent" 中断 (嵌套委派) 经本总线请求,
            // 与根委派完全同路径; agent 节点容器是唯一 spawn 服务者, 深度/并发预算
            // 与取消级联在此统一执行
            // - handler 捕获弱引用 (parentCtx weak): 避免 ctx->bus -> registerServer
            //   handler -> ctx 的 shared_ptr 循环引用 (子代理回收时总线随 ctx
            //   析构, 无悬空)
            if (subCtx->bus) {
                std::weak_ptr<AgentContext> parentCtxWeak = subCtx;
                std::string                 parentSession = subagentSessionId;
                std::weak_ptr<AgentHost>    selfWeak      = weak_from_this();
                subCtx->bus
                    ->getRR<events::ReqSubagentBatch, events::RespSubagentBatch>(
                        events::Topic::Subagent
                    )
                    .registerServer(
                        [selfWeak, parentCtxWeak, parentSession](
                            const events::ReqSubagentBatch& req,
                            size_t
                        ) -> asio::awaitable<events::RespSubagentBatch> {
                            auto ptr = selfWeak.lock();
                            if (!ptr) {
                                co_return events::RespSubagentBatch{};
                            }
                            // 防御: 深度预算按本子代理线程查表 (请求方
                            // AgentRunner 已填 parentSessionId, 此处保证不被
                            // 外部构造的请求改写)
                            auto r            = req;
                            r.parentSessionId = parentSession;
                            // 嵌套委派时父上下文是上一级子代理 (HIL 继承查找用)
                            co_return co_await ptr->spawnBatch(r, parentCtxWeak.lock());
                        }
                    );
            }

            XX_LOGD("    ## AgentHost spawned subagent `{}` (agentId={})", subagentName, agentId);

            // 初始消息: messages 结构化原样转发优先 (可含 system, 原样转发);
            // 否则回退 systemPrompt + message 文本 (默认独立行为)
            // - 上下文以会话为唯一权威: 直接写入子会话的上下文, 不再经
            //   RunConfig.input 播种 (图状态不再持有 messages 通道)
            std::vector<neograph::ChatMessage> inputMessages;
            if (task.messages.has_value()) {
                // task.messages 为 utilxx_base::Json (事件层类型): 逐条转 typed
                if (task.messages->is_array()) {
                    inputMessages.reserve(task.messages->size());
                    for (const auto& item : *task.messages) {
                        neograph::ChatMessage msg;
                        auto                  neoItem = agentxx::util::toNeographJson(item);
                        neograph::from_json(neoItem, msg);
                        inputMessages.push_back(std::move(msg));
                    }
                }
            } else {
                inputMessages.push_back(
                    neograph::ChatMessage{.role = "system", .content = sysPrompt}
                );
                inputMessages.push_back(
                    neograph::ChatMessage{.role = "user", .content = std::string{task.message}}
                );
            }
            subSession->replaceMessages(std::move(inputMessages), false);

            neograph::graph::RunConfig cfg{
                .thread_id        = subagentSessionId,
                .cancel_token     = cancelToken,
                .resume_if_exists = false,
            };

            std::ostringstream oss;
            // 事件回调: token 收集 (content) + hostBus 进度发布
            // (run/resume 共用; 如果 resume 不传 callback, 中断恢复后的
            //  token 不进入输出/进度事件, 此处统一修复)
            auto progressCb = [&](const neograph::graph::GraphEvent& event) {
                switch (event.type) {
                    case neograph::graph::GraphEvent::Type::LLM_TOKEN: {
                        std::string token;
                        std::string kind = "content";
                        if (event.data.is_string()) {
                            token = event.data.get<std::string>();
                        } else if (event.data.is_object()) {
                            neograph::ChatStreamChunk chunk;
                            neograph::from_json(event.data, chunk);
                            // 生成阶段标记 (见 protocol/provider_common.h): 不是模型
                            // 输出文本, 跳过 (否则工具名等标记会被当正文拼进子代理结果)
                            if (agentxx::protocol::chunk_type::isStatusChunk(chunk.type)) {
                                break;
                            }
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
            };

            // 统一的 "运行 + 中断处理 + 恢复" 循环 (与根 agent 共用 AgentRunner):
            // - 语义与旧内联循环完全一致 (无 checkpoint 持久化 / 无 MessageTip /
            //   HIL 超时统一取 IO 端点配置, 与根同策略)
            // - 嵌套委派: 经本子代理总线 (service.subagent 由 agent 节点容器 registerServer) 请求,
            //   与根委派完全同路径 (扁平化: 无 agent 节点容器函数直调)
            // - HIL (权限询问等): 子代理会话继承父会话总线, 请求冒泡到父 IO
            //   (弹窗/确认), 结果注入后 resume
            auto runnerOutcome = co_await AgentRunner{}.run(
                subCtx,
                subagent->getEngine(),
                subagentSessionId,
                std::move(cfg),
                cancelToken,
                AgentRunner::Hooks{
                    .eventCallback  = progressCb,
                    .onInterruptTip = nullptr,
                    .onBeforeResume = nullptr,
                    .onRunResult    = nullptr,
                }
            );

            events::RespSubagentBatchItem respItem;
            if (cancelToken && cancelToken->is_cancelled()) {
                respItem = events::RespSubagentBatchItem{
                    .resultId     = task.resultId,
                    .hasError     = true,
                    .cancelled    = true,
                    .errorMessage = "Sub-agent cancelled",
                    .agentId      = agentId,
                };
            } else if (runnerOutcome.unresolvedInterrupt) {
                // 中断仍未完成 (无处理者/未响应): 显式报错
                respItem = events::RespSubagentBatchItem{
                    .resultId     = task.resultId,
                    .hasError     = true,
                    .cancelled    = false,
                    .errorMessage = fmt::format(
                        "Sub-agent interrupted at node `{}` (interrupt not handled in subagent scope)",
                        runnerOutcome.interruptNode
                    ),
                    .agentId = agentId,
                };
            } else {
                respItem = events::RespSubagentBatchItem{
                    .resultId  = task.resultId,
                    .content   = oss.str(),
                    .hasError  = false,
                    .cancelled = false,
                    .agentId   = agentId,
                };
            }
            publishProgress(agentId, node->parentAgentId, "turn_end", "");

            // 结束事件 + 节点回收 (cleanup guard)
            if (hostBus_) {
                asio::co_spawn(
                    hostBus_->executor(),
                    [bus = hostBus_,
                     agentId,
                     hasError = respItem.hasError,
                     err      = respItem.errorMessage]() -> asio::awaitable<void> {
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
            co_return respItem;
        },
        [&agentId, &task](std::string errmsg) -> asio::awaitable<events::RespSubagentBatchItem> {
            // 节点回收由 SpawnCleanup guard 统一处理
            co_return events::RespSubagentBatchItem{
                .resultId     = task.resultId,
                .hasError     = true,
                .cancelled    = false,
                .errorMessage = fmt::format("Sub-agent failed: {}", errmsg),
                .agentId      = agentId,
            };
        },
        [&agentId, &task](std::string& errmsg) -> std::optional<events::RespSubagentBatchItem> {
            // 取消类异常: 转为错误结果快速返回 (父轮次随后按取消语义中止)
            return events::RespSubagentBatchItem{
                .resultId     = task.resultId,
                .hasError     = true,
                .cancelled    = true,
                .errorMessage = fmt::format("Sub-agent cancelled: {}", errmsg),
                .agentId      = agentId,
            };
        },
        // 传入取消令牌: operation_aborted 按取消语义处理
        cancelToken
    );
    if (cleanup.host) {
        if (!co_await cleanup.host->destroyAgentAsync(agentId)) {
            XX_LOGE("AgentHost: deferred shutdown failed for agent `{}`", agentId);
        }
    }
    cleanup.releaseAccounting();
    co_return response;
}

asio::awaitable<events::RespSubagentBatch> AgentHost::spawnBatch(
    const events::ReqSubagentBatch& req,
    std::shared_ptr<AgentContext>   parentAgentCtx
) {
    events::RespSubagentBatch batchResp;
    if (req.tasks.empty()) {
        co_return batchResp;
    }
    auto ex          = co_await asio::this_coro::executor;
    using ItemResult = events::RespSubagentBatchItem;
    size_t n         = req.tasks.size();
    // results 由 shared_ptr 持有, 避免 spawnBatch 协程取消析构后子协程 UAF
    auto results = std::make_shared<std::vector<ItemResult>>(n);

    // 每个子代理 co_spawn 为独立协程, 完成后向 channel 发送 index (wait_for_all)
    auto doneChannel
        = std::make_shared<asio::experimental::channel<void(utilxx_base::AsioErrorCode, size_t)>>(
            ex,
            static_cast<unsigned>(n)
        );

    for (size_t i = 0; i < n; ++i) {
        const auto& task = req.tasks[i];
        asio::co_spawn(
            ex,
            [this,
             task,
             parentAgentCtx,
             results,
             i,
             doneChannel,
             parentSessionId = req.parentSessionId,
             cancelToken     = req.cancelToken]() -> asio::awaitable<void> {
                // RAII 守卫: 无论 spawnOneTask 如何退出都保证发送完成信号
                struct BatchDoneGuard {
                    std::shared_ptr<
                        asio::experimental::channel<void(utilxx_base::AsioErrorCode, size_t)>>
                           ch;
                    size_t idx;

                    ~BatchDoneGuard() {
                        if (ch) {
                            ch->async_send(
                                utilxx_base::AsioErrorCode{},
                                idx,
                                [](utilxx_base::AsioErrorCode) {}
                            );
                        }
                    }
                } guard{doneChannel, i};

                auto r = co_await agentxx::util::catchErrorAsync<events::RespSubagentBatchItem>(
                    [&]() -> asio::awaitable<events::RespSubagentBatchItem> {
                        co_return co_await spawnOneTask(
                            task,
                            parentSessionId,
                            cancelToken,
                            parentAgentCtx
                        );
                    },
                    [&task](std::string errmsg) -> asio::awaitable<events::RespSubagentBatchItem> {
                        co_return events::RespSubagentBatchItem{
                            .resultId     = task.resultId,
                            .hasError     = true,
                            .cancelled    = false,
                            .errorMessage = fmt::format("Sub-agent failed: {}", std::move(errmsg)),
                        };
                    },
                    [&task](std::string& errmsg) -> std::optional<events::RespSubagentBatchItem> {
                        return events::RespSubagentBatchItem{
                            .resultId  = task.resultId,
                            .hasError  = true,
                            .cancelled = true,
                            .errorMessage
                            = fmt::format("Sub-agent cancelled: {}", std::move(errmsg)),
                        };
                    },
                    cancelToken
                );
                (*results)[i] = std::move(r);
            },
            asio::detached
        );
    }

    // 等待全部完成 (接收 n 次)
    for (size_t i = 0; i < n; ++i) {
        co_await doneChannel->async_receive(asio::as_tuple(asio::use_awaitable));
    }

    batchResp.results = std::move(*results);
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
    std::string_view                              agentId,
    std::shared_ptr<agentxx::protocol::A2aClient> client
) {
    if (client) {
        utilxx_base::insertOrAssignHeterogeneous(remoteAgents_, agentId, std::move(client));
    } else {
        utilxx_base::eraseHeterogeneous(remoteAgents_, agentId); // 异构删除免拷贝
    }
}

void AgentHost::unregisterRemoteAgent(std::string_view agentId) {
    utilxx_base::eraseHeterogeneous(remoteAgents_, agentId);
}

asio::awaitable<events::RespHostMessage> AgentHost::sendViaA2a(
    std::shared_ptr<agentxx::protocol::A2aClient> client,
    const events::ReqHostMessage&                 req
) {
    // 1) SendMessage → 服务端创建 task
    auto sendResult = co_await client->sendMessage(req.message);
    if (!sendResult.has_value()) {
        co_return events::RespHostMessage{
            .hasError     = true,
            .errorMessage = fmt::format("A2A SendMessage failed: {}", sendResult.error()),
        };
    }
    auto taskId = agentxx::protocol::A2aClient::extractTaskId(sendResult.value());
    if (taskId.empty()) {
        co_return events::RespHostMessage{
            .hasError     = true,
            .errorMessage = "A2A SendMessage returned no task id",
        };
    }

    // 2) 轮询 GetTask 至终态 (500ms 间隔; 总超时 60s; 取消令牌同步生效)
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
        auto        state = agentxx::protocol::A2aClient::extractTaskState(task);
        if (state == "TASK_STATE_COMPLETED") {
            co_return events::RespHostMessage{
                .content = agentxx::protocol::A2aClient::extractArtifactText(task),
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
        utilxx_base::insertOrAssignHeterogeneous(mailboxes_, agentId, std::move(mailbox));
    } else {
        utilxx_base::eraseHeterogeneous(mailboxes_, agentId); // 异构删除免拷贝
    }
}

asio::awaitable<bool>
    AgentHost::destroyAgentAsync(std::string_view agentId, std::chrono::milliseconds timeout) {
    // Children must close before their parent so dependency/session callbacks
    // cannot observe a parent context that has already been destroyed.
    for (auto& child : registry_.childrenOf(agentId)) {
        if (!co_await destroyAgentAsync(child->agentId, timeout)) {
            co_return false;
        }
    }

    auto node = registry_.get(agentId);
    if (!node || !node->agent) {
        co_return true;
    }
    if (!co_await node->agent->shutdownAsync(timeout)) {
        XX_LOGE("AgentHost: plugin shutdown failed for agent `{}`", agentId);
        co_return false;
    }
    unregisterSubagentService(agentId);
    registry_.remove(agentId);
    node->agent.reset();
    releaseSessionStoreRoot(agentId);
    // 若是根, 把它从根表里摘掉 (调用方显式销毁就尊重它)
    if (rootAgent_ && rootAgent_->getContext() && rootAgent_->getContext()->agentId == agentId) {
        rootAgent_.reset();
    }
    std::erase_if(roots_, [agentId](const MountedRoot& item) {
        return item.agentId == agentId;
    });
    co_return true;
}

} // namespace agent
} // namespace agentxx
