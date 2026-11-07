#include "agentxx/agent/io/view_replica.h"

#include "agentxx/agent/io/wire_protocol.h"
#include "utilxx_base/log.h"
#include <algorithm>
#include <utility>

namespace agentxx {
namespace agent {

namespace {

/// 行是否带客户端本地 id (`local_` 前缀)
bool isLocalRowId(std::string_view id) noexcept {
    return id.starts_with(kLocalRowIdPrefix);
}

} // namespace

void ViewReplica::rebuildIndex() const {
    index_.clear();
    index_.reserve(rows_.size());
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i] && !rows_[i]->id.empty()) {
            index_.insert_or_assign(rows_[i]->id, i);
        }
    }
}

std::shared_ptr<ViewMessage> ViewReplica::rowById(std::string_view id) const {
    if (id.empty()) {
        return nullptr;
    }
    auto it = index_.find(std::string{id});
    if (it != index_.end() && it->second < rows_.size() && rows_[it->second]
        && rows_[it->second]->id == id) {
        return rows_[it->second];
    }
    // 索引缺失/过期: 线性回扫并重建 (防御, 正常路径不会走到)
    rebuildIndex();
    it = index_.find(std::string{id});
    if (it != index_.end() && it->second < rows_.size()) {
        return rows_[it->second];
    }
    return nullptr;
}

bool ViewReplica::appendToken(const WireDelta& delta, bool thinking) {
    if (delta.msgId.empty()) {
        // 消息级 op 一律按 id 寻址: 没有 id 的 token 无法定位 (协议违规)
        XX_LOGD("[view_replica] token without msgId ignored");
        return false;
    }
    if (stream_.id != delta.msgId) {
        // 目标是一条已经落定的行: 同一模型步骤里思考/正文交错返回时, 后一段追加到
        // 该角色先落定的那条行上 (agent 端按步骤归组, 一条行会多次收到 token)
        if (auto row = rowById(delta.msgId); row) {
            if (delta.text.empty()) {
                return false;
            }
            // 换成新对象再追加: 渲染缓存以消息指针为失效 key, 原地改文本不会重绘
            auto copy = std::make_shared<ViewMessage>(*row);
            copy->text.append(delta.text);
            // rowById 命中时索引必然可用 (缺失时它已重建)
            auto it = index_.find(std::string{delta.msgId});
            if (it == index_.end() || it->second >= rows_.size()) {
                return false; // 防御: 索引与表不同步
            }
            rows_[it->second] = std::move(copy);
            return true;
        }
        // 防御: stream_begin 丢了 (或跨段乱序), 用 token 里的 id 自建一条
        stream_.clear();
        stream_.id   = delta.msgId;
        stream_.role = thinking ? ViewMessage::Role::Think : ViewMessage::Role::Assistant;
    }
    if (delta.text.empty()) {
        return false;
    }
    // 按需 COW: UI 正持有快照时复制字符串本体, 未持有时直接原地追加 (O(n) 总代价)
    if (stream_.text.use_count() > 1) {
        stream_.text = std::make_shared<std::string>(*stream_.text);
    }
    stream_.text->append(delta.text);
    return true;
}

bool ViewReplica::applyDelta(const WireDelta& delta) {
    using T = WireDelta::Type;
    switch (delta.type) {
        case T::StreamBegin: {
            if (delta.msgId.empty()) {
                return false;
            }
            const bool changed = stream_.id != delta.msgId || stream_.role != delta.streamRole;
            stream_.clear();
            stream_.id    = delta.msgId;
            stream_.role  = delta.streamRole;
            stream_.think = delta.think;
            return changed;
        }
        case T::TextToken:
            return appendToken(delta, false);
        case T::ThinkToken:
            return appendToken(delta, true);
        case T::InsertMessage: {
            if (!delta.message || delta.message->id.empty()) {
                return false;
            }
            bool changed = false;
            // 定稿动作: 该 id 的流式项先丢弃, 再落成正式行
            if (stream_.id == delta.message->id) {
                stream_.clear();
                changed = true;
            }
            if (!delta.message->hasVisibleContent()) {
                // 防御: 正常的 agent 端不会给不可见的行 (它只在定稿处判一次)
                return changed;
            }
            return upsertRow(*delta.message) || changed;
        }
        case T::UpdateMessage: {
            if (!delta.message || delta.message->id.empty()) {
                return false;
            }
            if (stream_.id == delta.message->id) {
                // 同一段既收到 update 又还在流: 说明定稿行是先到的, 丢弃流式项
                stream_.clear();
            }
            if (index_.empty() && !rows_.empty()) {
                rebuildIndex();
            }
            auto it = index_.find(delta.message->id);
            if (it == index_.end() || it->second >= rows_.size()) {
                // 未知 id: 忽略 (那条多半是被本地条数上限清理掉的老行,
                // 追加到末尾会把工具结果贴到错位置; 上滚分页会重新拉回来)
                return false;
            }
            if (!delta.message->hasVisibleContent()) {
                return false;
            }
            rows_[it->second] = std::make_shared<ViewMessage>(*delta.message);
            return true;
        }
        case T::StreamEnd: {
            if (!stream_.active() || stream_.id != delta.msgId) {
                return false; // 未知 id 视为 no-op
            }
            stream_.clear();
            return true;
        }
        default:
            return false;
    }
}

void ViewReplica::appendRow(const ViewMessage& row) {
    auto sp = std::make_shared<ViewMessage>(row);
    if (!sp->id.empty()) {
        index_.insert_or_assign(sp->id, rows_.size());
    }
    rows_.push_back(std::move(sp));
}

bool ViewReplica::upsertRow(ViewMessage row) {
    if (row.id.empty()) {
        return false;
    }
    if (index_.empty() && !rows_.empty()) {
        rebuildIndex();
    }
    auto it = index_.find(row.id);
    if (it != index_.end() && it->second < rows_.size()) {
        rows_[it->second] = std::make_shared<ViewMessage>(std::move(row));
        return true;
    }
    appendRow(row);
    return true;
}

bool ViewReplica::upsertLocalRow(ViewMessage row) {
    if (!isLocalRowId(row.id)) {
        XX_LOGW("[view_replica] local row id must start with `{}` (got `{}`), ignored",
                kLocalRowIdPrefix,
                row.id);
        return false;
    }
    if (!row.hasVisibleContent()) {
        return false;
    }
    return upsertRow(std::move(row));
}

void ViewReplica::reset(const WireSyncPayload& payload) {
    // 本地行 (中断表单 / 瞬态提示) 在整体替换后继续显示: 重连不该让正在等待
    // 用户操作的表单消失
    std::vector<std::shared_ptr<ViewMessage>> locals;
    for (const auto& row : rows_) {
        if (row && isLocalRowId(row->id)) {
            locals.push_back(row);
        }
    }
    rows_.clear();
    index_.clear();
    rows_.reserve(payload.messages.size() + locals.size());
    for (const auto& vm : payload.messages) {
        if (!vm.hasVisibleContent()) {
            continue;
        }
        appendRow(vm);
    }
    for (auto& local : locals) {
        if (index_.count(local->id) == 0) {
            rows_.push_back(std::move(local));
        }
    }
    rebuildIndex();
    lastViewSeq_ = payload.lastViewSeq;
    if (payload.streaming.has_value() && !payload.streaming->id.empty()) {
        seedStream(*payload.streaming);
    } else {
        stream_.clear();
    }
}

bool ViewReplica::appendIncremental(const WireSyncPayload& payload) {
    if (index_.empty() && !rows_.empty()) {
        rebuildIndex();
    }
    bool changed = false;
    for (const auto& vm : payload.messages) {
        if (vm.id.empty() || index_.count(vm.id) != 0 || !vm.hasVisibleContent()) {
            continue;
        }
        appendRow(vm);
        changed = true;
    }
    if (payload.lastViewSeq > lastViewSeq_) {
        lastViewSeq_ = payload.lastViewSeq;
    }
    return changed;
}

bool ViewReplica::prependRows(const std::vector<ViewMessage>& rows) {
    if (index_.empty() && !rows_.empty()) {
        rebuildIndex();
    }
    std::vector<std::shared_ptr<ViewMessage>> prepend;
    prepend.reserve(rows.size());
    for (const auto& vm : rows) {
        if (vm.id.empty() || index_.count(vm.id) != 0 || !vm.hasVisibleContent()) {
            continue;
        }
        prepend.push_back(std::make_shared<ViewMessage>(vm));
        index_.insert_or_assign(vm.id, 0); // 占位: 插完统一重建
    }
    if (prepend.empty()) {
        return false;
    }
    rows_.insert(rows_.begin(), prepend.begin(), prepend.end());
    rebuildIndex();
    return true;
}

size_t ViewReplica::trimHead(size_t count) {
    const size_t dropped = std::min(count, rows_.size());
    if (dropped == 0) {
        return 0;
    }
    rows_.erase(rows_.begin(), rows_.begin() + static_cast<std::ptrdiff_t>(dropped));
    rebuildIndex();
    return dropped;
}

void ViewReplica::seedStream(const ViewMessage& streaming) {
    stream_.clear();
    if (streaming.id.empty()) {
        return;
    }
    stream_.id    = streaming.id;
    stream_.role  = streaming.role;
    stream_.think = streaming.think;
    if (!streaming.text.empty()) {
        stream_.text = std::make_shared<std::string>(streaming.text);
    }
}

bool ViewReplica::replaceRow(size_t index, std::shared_ptr<ViewMessage> row) {
    if (index >= rows_.size() || !row) {
        return false;
    }
    const auto oldId = rows_[index] ? rows_[index]->id : std::string{};
    rows_[index]     = std::move(row);
    if (!oldId.empty() && oldId != rows_[index]->id) {
        rebuildIndex();
    }
    return true;
}

bool ViewReplica::setRowCollapsed(std::string_view id, bool collapsed) {
    auto row = rowById(id);
    if (!row || row->collapsed == collapsed) {
        return false;
    }
    auto copy       = std::make_shared<ViewMessage>(*row);
    copy->collapsed = collapsed;
    // 按 id 重新定位下标 (replaceRow 只按位置替换)
    if (index_.empty() && !rows_.empty()) {
        rebuildIndex();
    }
    auto it = index_.find(std::string{id});
    if (it == index_.end() || it->second >= rows_.size()) {
        return false;
    }
    rows_[it->second] = std::move(copy);
    return true;
}

} // namespace agent
} // namespace agentxx
