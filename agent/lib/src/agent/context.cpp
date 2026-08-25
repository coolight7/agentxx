#include "agentxx/agent/context.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/container_util.h"
#include "agentxx/util/log.h"
#include <chrono>
#include <fmt/format.h>

namespace agentxx {
namespace agent {

namespace {

/// 当前 steady 时钟毫秒数 (节流时间戳用, 单调不受系统时钟调整影响)
int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

AgentContext::~AgentContext() {
    // 插件系统先卸载全部插件, 断开中间件↔实例循环引用
    // (handles 由 middlewareHandleContext 持有, 其析构晚于 pluginManager)
    if (pluginManager) {
        pluginManager->shutdownAll();
    }
}

std::string Session::appendViewMessage(ViewMessage msg) {
    // 强制校验: viewMessages/chainHash/msgIdCounter_ 仅允许 io 线程写入
    assertIoThread();

    // 链式哈希对消息内容 (不含 id): 与旧实现 (json 内容 dump) 语义一致
    chainHash.append(msg.toJson().dump());
    auto id = fmt::format("msg_{:06d}", ++msgIdCounter_);
    msg.id  = id;
    viewMessages.push_back(std::move(msg));
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
    // 定位同 id 消息 (历史 append-only, 顺序线性扫描即可; 低频操作)
    for (auto& m : viewMessages) {
        if (m.id == msg.id) {
            m = std::move(msg);
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
}

void Session::saveLlmMessages() {
    assertIoThread();
    if (hooks_.onSaveLlmMessages) {
        hooks_.onSaveLlmMessages(llmMessages);
        // 记录落盘时刻供节流判定 (轮末权威保存同样刷新窗口)
        llmLastSaveMs_ = steadyNowMs();
    }
}

void Session::appendSettledLlmMessages(const neograph::json& settledMsgs) {
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
                [sessionStore, tid](const neograph::json& msgs) {
                    sessionStore->saveLlmMessages(tid, msgs);
                },
        });
    }
    util::insertHeterogeneous(sessions_, std::string{sessionId}, session);
    return session;
}

std::shared_ptr<Session> SessionsManager::get(std::string_view sessionId) {
    auto it = sessions_.find(sessionId);
    return it == sessions_.end() ? nullptr : it->second;
}

void SessionsManager::remove(std::string_view sessionId) {
    // 异构查找删除, 免除 string_view→string 拷贝 (libc++ 无 C++23 异构 erase)
    util::eraseHeterogeneous(sessions_, sessionId);
}

std::shared_ptr<Session> AgentContext::getSession(std::string_view sessionId) {
    return sessions->getOrCreate(sessionId);
}

std::string AgentContext::resolveSessionWorkDir(std::string_view sessionId) {
    // worktree 绑定优先 (worktree 模式; Session 可变状态仅 io 线程读写,
    // 本方法约定在 io 线程调用 —— 插件宿主侧经 ioCallSync 投递)
    auto session = sessions->get(sessionId);
    if (session) {
        const auto& wb = session->getWorktreeBinding();
        if (!wb.path.empty()) {
            return wb.path;
        }
    }
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

} // namespace agent
} // namespace agentxx
