#include "agentxx/agent/context.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/tools/subagent.h"
#include "utilxx/async_offload.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/log.h"
#include "neograph/graph/registry.h"
#include <chrono>
#include <fmt/format.h>
#include <fmt/ranges.h>

namespace agentxx {
namespace agent {

namespace {

/// 当前 steady 时钟毫秒数 (节流时间戳用, 单调不受系统时钟调整影响)
int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

} // namespace

AgentContext::AgentContext() = default;

asio::awaitable<bool> AgentContext::shutdownPluginsAsync(std::chrono::milliseconds timeout) {
    if (!pluginManager) {
        co_return true;
    }
    co_return co_await pluginManager->shutdownAsync(timeout);
}

AgentContext::~AgentContext() {
    // 插件系统先卸载全部插件, 断开中间件↔实例循环引用
    // (handles 由 middlewareHandleContext 持有, 其析构晚于 pluginManager)
    if (pluginManager) {
        pluginManager->shutdownAll();
        if (pluginManager->hasPendingClose()) {
            // 析构无法等待异步 stop 事务: 这些实例保持 CloseFailed 并保留
            // 上下文/动态库。owner 应在停止 IO executor 前 await shutdownAsync()。
            XX_LOGW("AgentContext destroyed with plugins still closing; owner should await "
                    "agent->shutdownAsync() before stopping the agent IO executor");
        }
    }
}

Session::~Session() {
    // 析构线程安全守卫:
    // - pendingViewOps_ 与 hooks_ 严格约定仅在 bound io 线程访问
    // - 若 Session 在非 io 线程析构 (如 SessionsManager::remove 或 AgentHost::destroyAgent
    //   在其他线程释放最后引用), 禁止跨线程触发 SQLite 落库或并发读写 pendingViewOps_;
    //   此时宿主/进程正在拆卸, 直接丢弃待落盘操作并记录警告
    if (isIoThread()) {
        flushPendingViewOps();
    } else {
        if (!pendingViewOps_.empty()) {
            XX_LOGW(
                "Session destructor called off io thread; discarding {} pending view ops",
                pendingViewOps_.size()
            );
            pendingViewOps_.clear();
        }
    }
}

std::string Session::appendViewMessage(ViewMessage msg) {
    // 强制校验: viewMessages/chainHash/msgIdCounter_ 仅允许 io 线程写入
    assertIoThread();

    // 链式哈希对消息内容 (不含 id)
    chainHash.append(msg.toJson().dump());
    auto id = fmt::format("msg_{:06d}", ++msgIdCounter_);
    msg.id  = id;
    viewMessages.push_back(std::move(msg));
    // 维护 msgId → 下标索引 (updateViewMessage 的 O(1) 定位用)
    msgIndex_.insert_or_assign(id, viewMessages.size() - 1);
    // 持久化 (节流, 尽力而为): 压入待落盘队列 — 首次立即落库, 节流窗口内合并,
    // 待下次触发或轮末 flushViewMessages() 补存 (消息 + 追加后计数同事务)
    if (hooks_.onAppendViewMessage) {
        enqueueViewPersist(PendingViewOp{
            .isAppend = true,
            .msg      = viewMessages.back(),
            .counter  = msgIdCounter_,
        });
    }
    return id;
}

void Session::updateViewMessage(ViewMessage msg) {
    // 强制校验: viewMessages/chainHash/msgIdCounter_ 仅允许 io 线程写入
    assertIoThread();

    if (msg.id.empty()) {
        XX_LOGW("Session::updateViewMessage: empty msg id, skipped");
        return;
    }
    // 定位同 id 消息: 先走 msgId 索引 (O(1)); 索引缺失或指向的消息已变
    // 则回退线性扫描并修复索引 (viewMessages/索引仅本类维护, 回退属防御)
    size_t index = viewMessages.size();
    if (auto idxIt = msgIndex_.find(msg.id); idxIt != msgIndex_.end()) {
        if (idxIt->second < viewMessages.size() && viewMessages[idxIt->second].id == msg.id) {
            index = idxIt->second;
        }
    }
    if (index == viewMessages.size()) {
        for (size_t i = 0; i < viewMessages.size(); ++i) {
            if (viewMessages[i].id == msg.id) {
                index = i;
                msgIndex_.insert_or_assign(msg.id, i); // 修复索引
                break;
            }
        }
    }
    if (index < viewMessages.size()) {
        auto& m = viewMessages[index];
        m       = std::move(msg);
        // 持久化 (节流, 尽力而为): 覆盖库内对应行, 供重启恢复
        if (hooks_.onUpdateViewMessage) {
            enqueueViewPersist(PendingViewOp{
                .isAppend = false,
                .msg      = m,
                .counter  = 0,
            });
        }
        return;
    }
    XX_LOGW("Session::updateViewMessage: msg id {} not found in history", msg.id);
}

void Session::setStoreHooks(SessionStoreHooks hooks) {
    assertIoThread();
    hooks_ = std::move(hooks);
}

void Session::restore(std::vector<ViewMessage> messages, uint64_t msgIdCounter) {
    assertIoThread();

    // 重建链式哈希: 与 appendViewMessage 一致, 对不含 id 的消息内容哈希
    chainHash.reset();
    for (const auto& m : messages) {
        auto content = m;
        content.id.clear();
        chainHash.append(content.toJson().dump());
    }
    viewMessages  = std::move(messages);
    msgIdCounter_ = msgIdCounter;
    // 重建 msgId → 下标索引 (与 viewMessages 同步; 恢复的历史全量建索引)
    msgIndex_.clear();
    msgIndex_.reserve(viewMessages.size());
    for (size_t i = 0; i < viewMessages.size(); ++i) {
        if (false == viewMessages[i].id.empty()) {
            msgIndex_.insert_or_assign(viewMessages[i].id, i);
        }
    }
}

void Session::saveLlmMessages() {
    assertIoThread();
    if (hooks_.onSaveLlmMessages) {
        hooks_.onSaveLlmMessages(llmMessages);
        // 记录落盘时刻供节流判定 (轮末权威保存同样刷新窗口)
        llmLastSaveMs_ = steadyNowMs();
    }
}

void Session::appendSettledLlmMessages(const utilxx_base::Json& settledMsgs) {
    assertIoThread();
    if (!settledMsgs.is_array() || settledMsgs.empty()) {
        return;
    }
    for (const auto& m : settledMsgs) {
        llmMessages.push_back(m);
    }
    requestSaveLlmMessages();
}

void Session::requestSaveLlmMessages() {
    assertIoThread();
    if (!hooks_.onSaveLlmMessages) {
        return;
    }
    const auto nowMs = steadyNowMs();
    if (llmLastSaveMs_ == 0 || nowMs - llmLastSaveMs_ >= kPersistThrottleMs) {
        // 首次触发 / 距上次落盘已超窗口: 立即保存
        saveLlmMessages();
    }
    // 窗口内: 仅更新内存 (llm 内容本身在 llmMessages 中, 无需单独排队),
    // 待下次结算触发或轮末 saveLlmMessages() 统一落盘
}

void Session::flushViewMessages() {
    assertIoThread();
    flushPendingViewOps();
}

void Session::enqueueViewPersist(PendingViewOp op) {
    pendingViewOps_.push_back(std::move(op));
    const auto nowMs = steadyNowMs();
    if (viewLastPersistMs_ == 0 || nowMs - viewLastPersistMs_ >= kPersistThrottleMs) {
        // 首次触发 / 距上次落盘已超窗口: 立即回放全部待落盘操作 (含本条)
        flushPendingViewOps();
    }
}

void Session::flushPendingViewOps() {
    if (pendingViewOps_.empty()) {
        return;
    }
    for (const auto& op : pendingViewOps_) {
        if (op.isAppend) {
            if (hooks_.onAppendViewMessage) {
                hooks_.onAppendViewMessage(op.msg, op.counter);
            }
        } else {
            if (hooks_.onUpdateViewMessage) {
                hooks_.onUpdateViewMessage(op.msg);
            }
        }
    }
    pendingViewOps_.clear();
    viewLastPersistMs_ = steadyNowMs();
}

void Session::setCancelToken(std::shared_ptr<neograph::graph::CancelToken> token) {
    assertIoThread();
    cancelToken_ = std::move(token);
}

std::shared_ptr<neograph::graph::CancelToken> Session::getCancelToken() {
    assertIoThread();
    return cancelToken_;
}

void Session::setModelName(std::string_view name) {
    assertIoThread();
    modelName_ = name;
}

std::string Session::getModelName() const {
    assertIoThread();
    return modelName_;
}

std::shared_ptr<Session> SessionsManager::getOrCreate(std::string_view sessionId) {
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        return it->second;
    }
    auto session = std::make_shared<Session>();
    // 拷贝到局部: 供 lambda 按值捕获 (成员无法直接捕获)
    auto sessionStore = this->sessionStore;
    if (sessionStore) {
        // 从 SQLite 恢复该 session 的历史消息/LLM 上下文, 并绑定持久化回调
        auto loaded = sessionStore->loadSession(sessionId);
        session->restore(std::move(loaded.viewMessages), loaded.msgIdCounter);
        session->llmMessages = std::move(loaded.llmMessages);
        // 捕获 sessionId 副本, 回调生命周期随 session, 无悬垂风险
        auto tid = std::string{sessionId};
        session->setStoreHooks(SessionStoreHooks{
            .onAppendViewMessage =
                [sessionStore, tid](const ViewMessage& msg, uint64_t counter) {
                    sessionStore->appendViewMessage(tid, msg, counter);
                },
            .onUpdateViewMessage =
                [sessionStore, tid](const ViewMessage& msg) {
                    sessionStore->updateViewMessage(tid, msg);
                },
            .onSaveLlmMessages =
                [sessionStore, tid](const utilxx_base::Json& msgs) {
                    sessionStore->saveLlmMessages(tid, msgs);
                },
        });
    }
    utilxx_base::insertHeterogeneous(sessions_, std::string{sessionId}, session);
    return session;
}

asio::awaitable<std::shared_ptr<Session>>
    SessionsManager::getOrCreateAsync(std::string_view sessionId, asio::thread_pool* pool) {
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        co_return it->second;
    }

    auto                        sessionStore = this->sessionStore;
    SessionStore::LoadedSession loaded;
    if (sessionStore) {
        if (pool) {
            loaded = co_await utilxx::offloadAsync<SessionStore::LoadedSession>(
                *pool,
                [sessionStore,
                 sid = std::string(sessionId)]() -> asio::awaitable<SessionStore::LoadedSession> {
                    co_return sessionStore->loadSession(sid);
                }
            );
        } else {
            loaded = sessionStore->loadSession(sessionId);
        }
    }

    it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        co_return it->second;
    }

    auto session = std::make_shared<Session>();
    if (sessionStore) {
        session->restore(std::move(loaded.viewMessages), loaded.msgIdCounter);
        session->llmMessages = std::move(loaded.llmMessages);
        auto tid             = std::string{sessionId};
        session->setStoreHooks(SessionStoreHooks{
            .onAppendViewMessage =
                [sessionStore, tid](const ViewMessage& msg, uint64_t counter) {
                    sessionStore->appendViewMessage(tid, msg, counter);
                },
            .onUpdateViewMessage =
                [sessionStore, tid](const ViewMessage& msg) {
                    sessionStore->updateViewMessage(tid, msg);
                },
            .onSaveLlmMessages =
                [sessionStore, tid](const utilxx_base::Json& msgs) {
                    sessionStore->saveLlmMessages(tid, msgs);
                },
        });
    }
    utilxx_base::insertHeterogeneous(sessions_, std::string{sessionId}, session);
    co_return session;
}

std::shared_ptr<Session> SessionsManager::get(std::string_view sessionId) {
    auto it = sessions_.find(sessionId);
    return it == sessions_.end() ? nullptr : it->second;
}

void SessionsManager::remove(std::string_view sessionId) {
    // 异构查找删除, 免除 string_view→string 拷贝 (libc++ 无 C++23 异构 erase)
    utilxx_base::eraseHeterogeneous(sessions_, sessionId);
}

std::shared_ptr<Session> AgentContext::getSession(std::string_view sessionId) {
    return sessions->getOrCreate(sessionId);
}

asio::awaitable<std::shared_ptr<Session>> AgentContext::getSessionAsync(std::string_view sessionId
) {
    if (sessions) {
        co_return co_await sessions->getOrCreateAsync(sessionId, threadPool.get());
    }
    co_return nullptr;
}

void AgentContext::setSessionWorkDir(std::string_view sessionId, std::string_view absWorkDir) {
    if (sessionId.empty()) {
        return;
    }
    // mutex 保护: 端点线程 (如 ACP HTTP handler) 与 io 线程并发读写安全
    std::lock_guard lk(sessionWorkDirMu_);
    if (absWorkDir.empty()) {
        utilxx_base::eraseHeterogeneous(sessionWorkDirs_, sessionId);
        return;
    }
    utilxx_base::insertOrAssignHeterogeneous(
        sessionWorkDirs_,
        std::string{sessionId},
        std::string{absWorkDir}
    );
}

void AgentContext::clearSessionWorkDir(std::string_view sessionId) {
    std::lock_guard lk(sessionWorkDirMu_);
    utilxx_base::eraseHeterogeneous(sessionWorkDirs_, sessionId);
}

std::string AgentContext::getSessionWorkDir(std::string_view sessionId) {
    // worktree 绑定优先 (worktree 模式; Session 可变状态仅 io 线程读写,
    // 本方法约定在 io 线程调用 —— 插件宿主侧经 ioCallSync 投递)
    auto session = sessions->get(sessionId);
    if (session) {
        const auto& wb = session->getWorktreeBinding();
        if (!wb.path.empty()) {
            return wb.path;
        }
    }
    // 会话工作目录覆写次之 (各会话独立, 如 ACP 客户端注入的 cwd;
    // mutex 保护, 任意线程可读)
    {
        std::lock_guard lk(sessionWorkDirMu_);
        if (auto it = sessionWorkDirs_.find(sessionId); it != sessionWorkDirs_.end()) {
            return it->second;
        }
    }
    // 回退 agent 级配置 (yaml work_dir / 进程 cwd), 保持旧行为兜底
    if (agentConfig) {
        auto wd = agentConfig->resolvedWorkDir();
        if (!wd.empty()) {
            return wd;
        }
    }
    return {};
}

std::string AgentContext::getSessionCurrentModelName(std::string_view sessionId) const {
    std::string selected;
    auto        session = sessions->get(sessionId);
    if (session) {
        selected = session->getModelName();
    }
    if (modelRegistry) {
        return modelRegistry->resolveModelName(selected);
    } else {
        return agentConfig->model.modelName;
    }
}

const ModelConfig& AgentContext::getSessionCurrentModelConfig(std::string_view sessionId) const {
    if (modelRegistry) {
        return modelRegistry->getModelConfig(getSessionCurrentModelName(sessionId));
    }
    // 未初始化 registry 时(测试/嵌入场景)回退主模型
    return agentConfig ? agentConfig->model : ModelConfig::defaultModelConfig;
}

std::string AgentContext::buildSystemPrompt(std::string_view sessionId) const {
    if (!agentConfig) {
        return "";
    }
    std::string combined         = agentConfig->prompt.systemPrompt;
    auto        appendIfNonEmpty = [&](const std::string& seg) {
        if (seg.empty()) {
            return;
        }
        if (!combined.empty() && combined.back() != '\n') {
            combined += "\n";
        }
        if (!combined.empty() && combined.size() >= 2
            && combined.compare(combined.size() - 2, 2, "\n\n") != 0) {
            combined += "\n";
        }
        combined += seg;
    };

    const auto& appendMap   = agentConfig->prompt.appendSystemPrompts;
    auto        appendByKey = [&](const std::string& key) {
        auto it = appendMap.find(key);
        if (it != appendMap.end()) {
            appendIfNonEmpty(it->second);
        }
    };
    appendByKey("planning");
    appendByKey("skill");
    appendByKey("codegraph");
    for (const auto& kv : appendMap) {
        if (kv.first == "planning" || kv.first == "skill" || kv.first == "codegraph"
            || kv.first == "summarization") {
            continue;
        }
        appendIfNonEmpty(kv.second);
    }

    if (middlewareHandleContext && !sessionId.empty()) {
        const auto& appendSystemMsgList
            = middlewareHandleContext->getGraphDataItemValue<std::vector<std::string>>(
                sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_appendSystemMessage
            );
        if (!appendSystemMsgList.empty()) {
            std::string appendJoined = fmt::format("{}", fmt::join(appendSystemMsgList, "\n"));
            appendIfNonEmpty(appendJoined);
        }
    }
    return combined;
}

} // namespace agent
} // namespace agentxx
