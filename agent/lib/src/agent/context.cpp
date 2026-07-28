#include "agentxx/agent/context.h"
#include <fmt/format.h>

namespace agentxx {
namespace agent {

std::string Session::appendHistory(neograph::json msgData) {
    // 强制校验: fullHistory/chainHash/msgIdCounter_ 仅允许 io 线程写入
    assertIoThread();

    auto id = fmt::format("msg_{:06d}", ++msgIdCounter_);
    chainHash.append(msgData.dump());
    fullHistory.push_back(HistoryMessage{id, std::move(msgData)});

    // 发布 fullHistory 新快照（原子替换，无锁读取）
    auto snapshot = std::make_shared<const std::vector<HistoryMessage>>(fullHistory);
    historySnapshot_.store(snapshot, std::memory_order_release);

    // 发布 chainHash 新快照（供 UI 线程无锁读取）
    auto hashSnap = std::make_shared<const HashInfo>(
        HashInfo{chainHash.count(), chainHash.tailHex()}
    );
    hashSnapshot_.store(hashSnap, std::memory_order_release);

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

void Session::setModelName(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    modelName_ = name;
}

std::string Session::getModelName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return modelName_;
}

std::shared_ptr<Session> SessionStore::getOrCreate(std::string_view threadId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = sessions_.find(threadId);
    if (it != sessions_.end()) {
        return it->second;
    }
    auto session                     = std::make_shared<Session>();
    sessions_[std::string{threadId}] = session;
    return session;
}

std::shared_ptr<Session> SessionStore::get(std::string_view threadId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = sessions_.find(threadId);
    return it == sessions_.end() ? nullptr : it->second;
}

void SessionStore::remove(std::string_view threadId) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(threadId);
}

std::shared_ptr<Session> AgentContext::getSession(std::string_view threadId) {
    return sessions->getOrCreate(threadId);
}

} // namespace agent
} // namespace agentxx
