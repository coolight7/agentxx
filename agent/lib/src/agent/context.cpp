#include "agentxx/agent/context.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/agent/session_persistence.h"
#include <fmt/format.h>

namespace agentxx {
namespace agent {

std::string Session::appendHistory(ViewMessage msg) {
    // 强制校验: viewMessages/chainHash/msgIdCounter_ 仅允许 io 线程写入
    assertIoThread();

    // 链式哈希对消息内容 (不含 id): 与旧实现 (json 内容 dump) 语义一致
    chainHash.append(msg.toJson().dump());
    auto id = fmt::format("msg_{:06d}", ++msgIdCounter_);
    msg.id  = id;
    viewMessages.push_back(std::move(msg));
    // 持久化 (尽力而为): 消息 + 追加后计数一起落库, 供重启恢复
    if (hooks_.onAppendMessage) {
        hooks_.onAppendMessage(viewMessages.back(), msgIdCounter_);
    }
    return id;
}

void Session::setPersistenceHooks(SessionPersistenceHooks hooks) {
    assertIoThread();
    hooks_ = std::move(hooks);
}

void Session::restore(std::vector<ViewMessage> messages, uint64_t msgIdCounter) {
    assertIoThread();

    // 重建链式哈希: 与 appendHistory 一致, 对不含 id 的消息内容哈希
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
    }
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

std::shared_ptr<Session> SessionStore::getOrCreate(std::string_view threadId) {
    auto it = sessions_.find(threadId);
    if (it != sessions_.end()) {
        return it->second;
    }
    auto session = std::make_shared<Session>();
    // 拷贝到局部: 供 lambda 按值捕获 (成员无法直接捕获)
    auto persistence = this->persistence;
    if (persistence) {
        // 从 SQLite 恢复该 thread 的历史消息/LLM 上下文, 并绑定持久化回调
        auto loaded = persistence->loadSession(threadId);
        session->restore(std::move(loaded.viewMessages), loaded.msgIdCounter);
        session->llmMessages = std::move(loaded.llmMessages);
        // 捕获 threadId 副本, 回调生命周期随 session, 无悬垂风险
        auto tid = std::string{threadId};
        session->setPersistenceHooks(SessionPersistenceHooks{
            .onAppendMessage =
                [persistence, tid](const ViewMessage& msg, uint64_t counter) {
                    persistence->appendViewMessage(tid, msg, counter);
                },
            .onSaveLlmMessages =
                [persistence, tid](const neograph::json& msgs) {
                    persistence->saveLlmMessages(tid, msgs);
                },
        });
    }
    sessions_.emplace(threadId, session);
    return session;
}

std::shared_ptr<Session> SessionStore::get(std::string_view threadId) {
    auto it = sessions_.find(threadId);
    return it == sessions_.end() ? nullptr : it->second;
}

void SessionStore::remove(std::string_view threadId) {
    sessions_.erase(threadId);
}

std::shared_ptr<Session> AgentContext::getSession(std::string_view threadId) {
    return sessions->getOrCreate(threadId);
}

std::string AgentContext::getSessionCurrentModelName(std::string_view threadId) const {
    std::string selected;
    auto        session = sessions->get(threadId);
    if (session) {
        selected = session->getModelName();
    }
    if (modelRegistry) {
        return modelRegistry->resolveModelName(selected);
    } else {
        return agentConfig->model.modelName;
    }
}

const ModelConfig& AgentContext::getSessionCurrentModelConfig(std::string_view threadId) const {
    return modelRegistry->getModelConfig(getSessionCurrentModelName(threadId));
}

} // namespace agent
} // namespace agentxx
