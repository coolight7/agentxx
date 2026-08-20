#include "agentxx/agent/checkpoint_store.h"

#include <algorithm>

namespace agentxx {
namespace agent {

using neograph::graph::Checkpoint;
using neograph::graph::PendingWrite;

// =========================================================================
// SingleCheckpointStore — 模板方法: 保存最新 checkpoint + 淘汰历史
// =========================================================================
//
// 裁剪安全性依据 (neograph graph_coordinator.cpp / graph_engine.cpp):
// - engine 恢复执行仅通过 load_latest 读取最新 checkpoint, 并仅读取挂载在
//   [最新 checkpoint id] 上的 pending writes (CheckpointCoordinator::
//   load_for_resume_async → get_writes(sessionId, latest.id));
// - 一次成功的 super-step 完成后, engine 会调用 clear_pending_writes(旧 cp id),
//   即旧 cp 上的 writes 本就会被清理; 这里只是在 save 时提前完成同样的清理;
// - super-step 执行中途崩溃/中断时并不会触发 save, 因此彼时挂载在 [当时最新 cp]
//   上的 writes 不会被 evict 误删, 崩溃恢复 (replay) 语义不受影响。
//
// 已知取舍: 并行扇出中某节点中断时, 同 super-step 内已完成兄弟节点的 writes
// 挂载在父 cp 上, 会随父 cp 一并淘汰。但 neograph 的 resume 只读取最新 cp 的
// writes bucket (该场景下为空), 本来就会重放这些兄弟节点, 故淘汰不改变行为。

void SingleCheckpointStore::save(const Checkpoint& cp) {
    // 先持久化为该 session 的最新 checkpoint
    saveImpl(cp);
    // 再淘汰该 session 除最新 checkpoint 外的全部历史数据
    evictImpl(cp.thread_id, cp.id);
}

// =========================================================================
// InMemorySingleCheckpointStore
// =========================================================================

void InMemorySingleCheckpointStore::saveImpl(const Checkpoint& cp) {
    std::lock_guard lock(mutex_);
    // 每个 session 仅保留最新 checkpoint, 直接覆盖
    latest_[cp.thread_id] = cp;
}

void InMemorySingleCheckpointStore::evictImpl(std::string_view sessionId, std::string_view keepId) {
    std::lock_guard   lock(mutex_);
    const std::string tid{sessionId};
    const std::string keep{keepId};
    // 清理挂载在历史 (非 keepId) checkpoint 上的 pending writes
    for (auto it = pending_.begin(); it != pending_.end();) {
        const auto& [t, cpId] = it->first;
        if (t == tid && cpId != keep) {
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<Checkpoint> InMemorySingleCheckpointStore::load_latest(const std::string& sessionId) {
    std::lock_guard lock(mutex_);
    auto            it = latest_.find(sessionId);
    if (it == latest_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<Checkpoint> InMemorySingleCheckpointStore::load_by_id(const std::string& id) {
    std::lock_guard lock(mutex_);
    // 仅可能命中某个 session 的最新 checkpoint; 历史 id 已被淘汰
    for (const auto& [tid, cp] : latest_) {
        if (cp.id == id) {
            return cp;
        }
    }
    return std::nullopt;
}

std::vector<Checkpoint>
    InMemorySingleCheckpointStore::list(const std::string& sessionId, int limit) {
    std::lock_guard         lock(mutex_);
    std::vector<Checkpoint> result;
    if (limit <= 0) {
        return result;
    }
    auto it = latest_.find(sessionId);
    if (it != latest_.end()) {
        result.push_back(it->second);
    }
    return result;
}

void InMemorySingleCheckpointStore::delete_thread(const std::string& sessionId) {
    std::lock_guard lock(mutex_);
    latest_.erase(sessionId);
    // 同步清理该 session 的全部 pending writes
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->first.first == sessionId) {
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

void InMemorySingleCheckpointStore::put_writes(
    const std::string&  sessionId,
    const std::string&  parentCheckpointId,
    const PendingWrite& write
) {
    std::lock_guard lock(mutex_);
    pending_[{sessionId, parentCheckpointId}].push_back(write);
}

std::vector<PendingWrite> InMemorySingleCheckpointStore::get_writes(
    const std::string& sessionId,
    const std::string& parentCheckpointId
) {
    std::lock_guard lock(mutex_);
    auto            it = pending_.find({sessionId, parentCheckpointId});
    if (it == pending_.end()) {
        return {};
    }
    return it->second;
}

void InMemorySingleCheckpointStore::clear_writes(
    const std::string& sessionId,
    const std::string& parentCheckpointId
) {
    std::lock_guard lock(mutex_);
    pending_.erase({sessionId, parentCheckpointId});
}

std::size_t InMemorySingleCheckpointStore::size() const {
    std::lock_guard lock(mutex_);
    return latest_.size();
}

std::size_t InMemorySingleCheckpointStore::pending_writes_count(
    const std::string& sessionId,
    const std::string& parentCheckpointId
) const {
    std::lock_guard lock(mutex_);
    auto            it = pending_.find({sessionId, parentCheckpointId});
    if (it == pending_.end()) {
        return 0;
    }
    return it->second.size();
}

} // namespace agent
} // namespace agentxx
