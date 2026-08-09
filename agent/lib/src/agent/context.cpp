#include "agentxx/agent/context.h"
#include "agentxx/agent/model_registry.h"
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
    return id;
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
