#include "agentxx-client/io/tui/model/queue_mirror.h"

#include <algorithm>

namespace agentxx {
namespace client {

void MessageQueueMirror::applySnapshot(const std::vector<Entry>& items) {
    entries_ = items;
}

void MessageQueueMirror::applySnapshot(
    const std::vector<Entry>& items,
    const std::string_view    queueState
) {
    entries_ = items;
    // 未知状态文本按 idle 处理 (老服务端不发送该字段, 客户端也不据此做危险动作)
    state_ = queueState.empty() ? std::string{"idle"} : std::string{queueState};
}

bool MessageQueueMirror::removeEntry(const std::string_view id) {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [id](const Entry& e) {
        return e.id == id;
    });
    if (it == entries_.end()) {
        return false;
    }
    entries_.erase(it);
    return true;
}

void MessageQueueMirror::clear() {
    entries_.clear();
    state_ = "idle";
}

const MessageQueueMirror::Entry* MessageQueueMirror::find(const std::string_view id) const {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [id](const Entry& e) {
        return e.id == id;
    });
    return it == entries_.end() ? nullptr : &*it;
}

bool MessageQueueMirror::noteInputAck(const AckRecord& ack) {
    if (ack.requestId == 0) {
        return false;
    }
    if (ackIndex_.count(ack.requestId) != 0) {
        return false; // 同一请求已经记过 (重复/迟到回执)
    }
    acks_.push_back(ack);
    while (acks_.size() > kAckHistoryLimit) {
        acks_.pop_front();
    }
    // 淘汰后重建下标 (容量很小, 重建成本可忽略)
    ackIndex_.clear();
    for (size_t i = 0; i < acks_.size(); ++i) {
        ackIndex_[acks_[i].requestId] = i;
    }
    return true;
}

const MessageQueueMirror::AckRecord* MessageQueueMirror::lastAck(const uint64_t requestId) const {
    const auto it = ackIndex_.find(requestId);
    if (it == ackIndex_.end() || it->second >= acks_.size()) {
        return nullptr;
    }
    return &acks_[it->second];
}

} // namespace client
} // namespace agentxx
