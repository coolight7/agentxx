#pragma once

#include "agentxx/agent/io/wire_protocol.h"
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace client {

/// 消息队列镜像 (计划 UI-1: 从 TUI 抽出的可单测模型, 无 FTXUI/无 IO 依赖)
///
/// 背景: 服务端把排队输入 (busy 期间收到的用户输入/插件注入) 以
/// `WireMessageQueueUpdate` 整体下发, 客户端界面据此展示"待发送队列"; 输入受理
/// 回执 (`WireInputAck`) 则给出每条输入的投递结果。这两处的判定原先写在 TUI
/// 端点里, 混着渲染状态与线程代码, 无法单独测试。
///
/// 本模型负责:
/// - **快照应用**: 用服务端下发的队列替换本地镜像 (服务端是唯一权威);
/// - **条目增删**: 按条目 id 定位/删除 (本地乐观删除 + 服务端快照校准);
/// - **队列状态**: idle/running/paused/draining 的镜像 (未知文本按 idle 处理);
/// - **回执记账**: 按 requestId 记录最近一次投递结果, 供界面"同一请求只提示一次"
///   (重复回执/迟到回执不会重复打扰用户), 记账表容量有上限。
class MessageQueueMirror {
public:

    /// 一条排队输入 (服务端 `MessageQueueItem` 的镜像; 附件元数据不含内容)
    struct Entry {
        std::string                                  id;
        std::string                                  text;
        std::string                                  model;
        std::vector<agentxx::agent::MediaAttachment> attachments;
        int64_t                                      createdAtMs = 0;
    };

    /// 投递结果 (与 `agentxx::agent::InputStatus` 同取值, 用文本保存以便透传)
    struct AckRecord {
        uint64_t    requestId = 0;
        std::string status; ///< started / queued / steered / rejected
        std::string reason; ///< 拒绝原因 (其余状态为空)
        std::string detail; ///< 人类可读补充说明
        std::string itemId; ///< 对应队列条目 (无则空)
    };

    /// 已记录的投递回执上限 (超过后按最旧淘汰, 记账只服务于"是否已提示")
    static constexpr size_t kAckHistoryLimit = 32;

    /// 应用服务端队列快照 (整体替换; 条目顺序以服务端为准)
    void applySnapshot(const std::vector<Entry>& items);
    /// 应用服务端队列快照 + 队列状态
    void applySnapshot(const std::vector<Entry>& items, std::string_view queueState);

    /// 本地移除一条 (点击删除时乐观更新; 服务端随后下发的快照会校准)
    bool removeEntry(std::string_view id);
    /// 清空 (清空队列 / 切换会话)
    void clear();

    /// 队列状态镜像 (未知/空文本视为 idle)
    const std::string& state() const noexcept {
        return state_;
    }
    bool idle() const noexcept {
        return state_ == "idle" || state_.empty();
    }
    bool paused() const noexcept {
        return state_ == "paused";
    }

    const std::vector<Entry>& entries() const noexcept {
        return entries_;
    }
    bool   empty() const noexcept {
        return entries_.empty();
    }
    size_t size() const noexcept {
        return entries_.size();
    }
    /// 按 id 查找 (未命中返回 nullptr)
    const Entry* find(std::string_view id) const;

    /// 记录一条投递回执
    /// - `return` true = 本次是新回执 (调用方据此提示用户一次);
    ///   false = 同一 requestId 已记录过 (重复/迟到回执, 不再提示)
    bool noteInputAck(const AckRecord& ack);
    /// 最近的投递回执 (未命中返回 nullptr)
    const AckRecord* lastAck(uint64_t requestId) const;
    size_t           ackCount() const noexcept {
        return acks_.size();
    }

private:

    std::vector<Entry>            entries_;
    std::string                   state_ = "idle";
    std::deque<AckRecord>         acks_;
    std::map<uint64_t, size_t>    ackIndex_; ///< requestId → acks_ 下标 (随淘汰重建)
};

} // namespace client
} // namespace agentxx
