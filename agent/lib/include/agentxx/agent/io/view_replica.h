#pragma once

#include "agentxx/agent/conversation_types.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agentxx {
namespace agent {

/// 客户端自造行的 id 前缀 (中断表单 `local_interrupt_<wireId>` / 瞬态提示 `local_tip_<n>`)
///
/// - agent 分配的 id 形如 `msg_000123`, 永远不会与本地行冲突;
///   本地行也永远不会被 agent 的消息级操作命中 (未知 id 的 update 直接忽略)
inline constexpr std::string_view kLocalRowIdPrefix = "local_";

/// 尾部流式项: agent 端 `Session::streamingView` 的客户端副本
///
/// 语义 = "正在输出、还没定稿的那一段":
/// - `id` 为空表示当前没有进行中的流
/// - `text` 是 shared_ptr<string>: 每个 token 只在 UI 正持有快照时复制本体
///   (与改造前的 `TUIRenderState::currentToken` 同一套 COW 思路)
struct ViewStreamTail {
    std::string                           id;
    ViewMessage::Role                     role = ViewMessage::Role::Assistant;
    std::shared_ptr<std::string>          text = std::make_shared<std::string>();
    std::optional<ViewMessage::ThinkData> think;

    /// 是否有进行中的流
    bool active() const noexcept {
        return !id.empty();
    }

    /// 丢弃这段流 (回到"没有进行中的流")
    void clear() {
        id.clear();
        role = ViewMessage::Role::Assistant;
        text = std::make_shared<std::string>();
        think.reset();
    }
};

/// 展示历史的客户端副本 (所有端点共用: TUI / stdio / FFI / 插件通道)
///
/// 设计 (展示历史单写者):
/// - 消息边界、正文归组、耗时、可见性、折叠默认值与 id 全部由 agent 决定,
///   本类只做三种动作: **按 id 追加文本**、**整条 upsert**、**丢弃流式项**;
/// - 没有"归组/合并/回填"一类启发式 —— 那些规则只存在于 agent 端 EventBridge;
/// - "按 id 追加文本": 文本增量 (text_token/thinking_token) 通常落在尾部流式项上;
///   同一模型步骤里思考/正文交错返回时, 后一段的目标 id 是该角色已落定的那条行,
///   此时按 id 追加到那一行 (换新对象, 渲染缓存以指针失效);
/// - `insert_message` / `update_message` 都是 by-id upsert:
///   未知 id 的 insert 追加到末尾, 已知 id 的 insert 等价整条替换 (重连重放幂等);
///   未知 id 的 update 忽略 (多半是被本地条数上限清理掉的老消息);
/// - 纯界面状态 (折叠点击、滚动、中断表单交互) 不在这里: 本类只保留行对象本身。
///
/// 线程约束: 无内部锁, 调用方自行保证同一时刻只有一个线程读写
/// (TUI 侧由 TUISharedState 的 mutex 保护)。
class ViewReplica {
public:

    /// 应用一个消息级操作; 返回是否改动了消息集合 (调用方据此决定重绘)
    /// - 只处理 6 个消息级 op (stream_begin/token/insert/update/stream_end);
    ///   界面状态类增量 (turn/node/gen/tip) 返回 false, 由端点自己处理
    bool applyDelta(const WireDelta& delta);

    /// 整体替换 (完整/尾窗同步); 保留本地行 (`local_` 前缀) 并按原顺序追加在末尾
    /// - `payload.streaming` 非空时一并灌入尾部流式项 (中途接入/重连)
    void reset(const WireSyncPayload& payload);

    /// 增量补拉: 按 id 去重后追加到尾部 (不整体替换, 不动尾部流式项)
    bool appendIncremental(const WireSyncPayload& payload);

    /// 历史分页: 把更早的一页前插到窗口上方 (按 id 去重)
    bool prependRows(const std::vector<ViewMessage>& rows);

    /// 本地条数上限清理: 丢弃头部 count 条; 返回实际丢弃条数
    size_t trimHead(size_t count);

    /// 本地行 upsert (中断表单 / 瞬态提示): id 必须以 `local_` 开头
    bool upsertLocalRow(ViewMessage row);

    /// 灌入同步里的直播槽 (等价于一次 stream_begin + 若干 token)
    void seedStream(const ViewMessage& streaming);

    /// 丢弃尾部流式项 (如轮次结束/整体重置)
    void clearStream() {
        stream_.clear();
    }

    /// 客户端本地改写某一行的对象 (折叠点击); 越界忽略
    /// - 每次都换新对象: 渲染缓存以消息指针为失效 key, 指针变了才重绘
    bool replaceRow(size_t index, std::shared_ptr<ViewMessage> row);

    /// 客户端本地折叠改写 (按 id): 替换行对象 (渲染缓存以指针失效)
    /// - 用于把用户对流式思考的折叠点击写到刚定稿的行上
    bool setRowCollapsed(std::string_view id, bool collapsed);

    const std::vector<std::shared_ptr<ViewMessage>>& rows() const noexcept {
        return rows_;
    }

    /// 按 id 取行; 不存在返回空指针
    std::shared_ptr<ViewMessage> rowById(std::string_view id) const;

    const ViewStreamTail& stream() const noexcept {
        return stream_;
    }

    bool hasStream() const noexcept {
        return stream_.active();
    }

    /// 最近一次同步携带的展示历史持久化序号 (0 = 未提供)
    uint64_t lastViewSeq() const noexcept {
        return lastViewSeq_;
    }

private:

    /// 整条写入 (按 id upsert); 不可见的行 (防御) 直接忽略
    bool upsertRow(ViewMessage row);
    /// 追加一条历史行 (内部用; 已按 id 去重与可见性过滤)
    void appendRow(const ViewMessage& row);
    /// 文本增量追加 (按 id 定位目标)
    /// - 目标是尾部流式项 → 直接追加 (含 stream_begin 丢失时按 token id 自建一条)
    /// - 目标是已落定的行 (同一模型步骤里交错返回的后一段) → 换新对象追加到那一行
    bool appendToken(const WireDelta& delta, bool thinking);
    /// 重建 id → 下标索引 (trimHead/prepend 后调用; 远低于逐条查找的频率)
    void rebuildIndex() const;

    std::vector<std::shared_ptr<ViewMessage>> rows_;
    /// id → 下标索引 (mutable: rowById 是 const 方法, 索引缺失时按需重建)
    mutable std::unordered_map<std::string, size_t> index_;
    ViewStreamTail                                  stream_;
    uint64_t                                        lastViewSeq_ = 0;
};

} // namespace agent
} // namespace agentxx
