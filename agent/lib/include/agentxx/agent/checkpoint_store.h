/**
 * @file agent/checkpoint_store.h
 * @brief 仅保留最新 checkpoint 的 CheckpointStore (单检查点存储)
 *
 * neograph 的 InMemoryCheckpointStore 会保留 thread 的全部历史 checkpoint
 * (O(super-steps)), 而 agentxx 场景下 engine 的 interrupt/resume 与多轮会话
 * 恢复只依赖 load_latest 返回的最新 checkpoint, parent_id 历史链仅服务于
 * fork / 时间旅行 (get_state_history), agentxx 均未使用。
 *
 * 这里提供 "每个 thread 仅保留最新 checkpoint" 的实现: 在 save 时自动淘汰
 * 该 thread 的历史数据, 将 checkpoint 存储开销从 O(super-steps) 降为
 * O(threads), 无需在每轮会话结束后手动裁剪。
 */
#pragma once

#include "neograph/graph/checkpoint.h"
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agentxx {
namespace agent {

/**
 * @brief 仅保留每个 thread 最新一个 checkpoint 的 CheckpointStore 策略基类
 *
 * 以模板方法实现保留策略: save() 为 final, 先调用 saveImpl() 持久化最新
 * checkpoint, 再调用 evictImpl() 淘汰该 thread 的历史数据。
 *
 * 安全性依据 (见 neograph graph_coordinator.cpp):
 * - engine 恢复执行时只读取 load_latest 的最新 checkpoint, 以及挂载在
 *   该 checkpoint id 上的 pending writes;
 * - 挂载在其他 checkpoint 上的 pending writes 在正常流程中要么随
 *   clear_writes 被清理 (super-step 完成后), 要么因 checkpoint 被淘汰
 *   而不再可达 (如中断路径), 因此随历史 checkpoint 一并丢弃是安全的;
 * - 基类 CheckpointStore 的 *_async 默认实现会回调对应同步虚函数,
 *   因此 engine 的异步调用同样会经过本策略, 子类只需实现同步侧。
 *
 * 代价:
 * - list / get_state_history 最多返回最新一条;
 * - fork / 按历史 checkpoint id 恢复不可用;
 * - 无法回退到更早的 super-step (内存实现本来也不跨进程持久化)。
 */
class SingleCheckpointStore : public neograph::graph::CheckpointStore {
public:

    /// 保存 checkpoint 并淘汰该 thread 的历史数据
    /// - final: 子类通过 saveImpl / evictImpl 参与, 不可覆盖本函数
    void save(const neograph::graph::Checkpoint& cp) final;

protected:

    /// 子类实现: 将 cp 持久化为该 thread 的最新 checkpoint
    virtual void saveImpl(const neograph::graph::Checkpoint& cp) = 0;

    /// 子类实现: 淘汰 sessionId 下除 keepId 外的全部历史数据
    /// (历史 checkpoint, 以及挂载在历史 checkpoint 上的 pending writes)
    virtual void evictImpl(std::string_view sessionId, std::string_view keepId) = 0;
};

/**
 * @brief SingleCheckpointStore 的内存实现
 *
 * 每个 thread 仅存储最新一个 checkpoint; pending writes 按
 * (session_id, parent_checkpoint_id) 组织, save 时随历史 checkpoint 一并淘汰。
 * 线程安全: 单个操作内部以 mutex 保护 (与 neograph::InMemoryCheckpointStore
 * 相同的每调用原子性约定)。
 */
class InMemorySingleCheckpointStore : public SingleCheckpointStore {
public:

    // ── CheckpointStore 同步接口 (async 侧继承基类默认实现) ─────────────

    std::optional<neograph::graph::Checkpoint> load_latest(const std::string& session_id) override;

    /// 仅可能命中某个 thread 的最新 checkpoint; 历史 id 已被淘汰, 返回 nullopt
    std::optional<neograph::graph::Checkpoint> load_by_id(const std::string& id) override;

    /// 最多返回最新一条 (limit <= 0 时返回空, 与 neograph 语义一致)
    std::vector<neograph::graph::Checkpoint>
        list(const std::string& session_id, int limit = 100) override;

    void delete_thread(const std::string& session_id) override;

    // ── pending writes ──────────────────────────────────────────────────

    void put_writes(
        const std::string&                   session_id,
        const std::string&                   parent_checkpoint_id,
        const neograph::graph::PendingWrite& write
    ) override;

    std::vector<neograph::graph::PendingWrite>
        get_writes(const std::string& session_id, const std::string& parent_checkpoint_id) override;

    void clear_writes(const std::string& session_id, const std::string& parent_checkpoint_id)
        override;

    // ── 测试辅助 ────────────────────────────────────────────────────────

    /// 当前存储的 checkpoint 总数 (每个 thread 至多 1 个)
    std::size_t size() const;

    /// 指定 checkpoint 上挂载的 pending writes 数量
    std::size_t pending_writes_count(
        const std::string& session_id,
        const std::string& parent_checkpoint_id
    ) const;

protected:

    void saveImpl(const neograph::graph::Checkpoint& cp) override;
    void evictImpl(std::string_view sessionId, std::string_view keepId) override;

private:

    mutable std::mutex mutex_;
    /// 每个 thread 仅保留最新 checkpoint
    std::map<std::string, neograph::graph::Checkpoint> latest_;
    /// pending writes: (session_id, parent_checkpoint_id) → 按插入顺序的列表
    std::map<std::pair<std::string, std::string>, std::vector<neograph::graph::PendingWrite>>
        pending_;
};

} // namespace agent
} // namespace agentxx
