#include "agentxx/agent/context.h"
#include <fmt/format.h>

namespace agentxx {
namespace agent {

std::string Session::appendHistory(neograph::json msgData) {
    auto id = fmt::format("msg_{:06d}", ++msgIdCounter_);
    chainHash.append(msgData.dump());
    fullHistory.push_back(HistoryMessage{id, std::move(msgData)});
    return id;
}

void Session::setCancelToken(std::shared_ptr<neograph::graph::CancelToken> token) {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelToken_ = std::move(token);
}

std::shared_ptr<neograph::graph::CancelToken> Session::getCancelToken() {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelToken_;
}

void Session::setModelName(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    modelName_ = name;
}

std::string Session::getModelName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return modelName_;
}

std::shared_ptr<Session> SessionStore::getOrCreate(const std::string& threadId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = sessions_.find(threadId);
    if (it != sessions_.end()) {
        return it->second;
    }
    auto session        = std::make_shared<Session>();
    sessions_[threadId] = session;
    return session;
}

std::shared_ptr<Session> SessionStore::get(const std::string& threadId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = sessions_.find(threadId);
    return it == sessions_.end() ? nullptr : it->second;
}

void SessionStore::remove(const std::string& threadId) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(threadId);
}

std::shared_ptr<Session> AgentContext::getSession(const std::string& threadId) {
    return sessions->getOrCreate(threadId);
}

} // namespace agent
} // namespace agentxx
