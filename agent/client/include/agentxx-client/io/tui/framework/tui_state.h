#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/expand/get_cpu_gpu_use.h"
#include "asio/experimental/concurrent_channel.hpp"
#include "neograph/api.h"
#include "neograph/define.h"
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

/// 中断输入结果回传通道 (UI 线程 → client 线程):
/// - 参数 1: 输入项序号 inputIndex (对应 TUIMessage::inputIndex);
///   负数 (-1) 表示整体取消 (仅当 value 为 nullopt)
/// - 参数 2: 用户确认值 (bool 规范化 "true"/"false", 其余原样字符串);
///   nullopt 表示该输入项无结果 (取消/整体取消)
/// 同一次中断请求的所有输入项共享同一 channel; client 线程 handleInterrupt
/// 挂起接收, UI 线程 (消息列表控件交互) 确认/取消后发送。
using InterruptResultChannel = asio::experimental::concurrent_channel<
    void(neograph_asio_error_code, int, std::optional<std::string>)>;

/// TUI 消息模型: 统一使用 agentxx::agent::ViewMessage
/// (与 server Session::viewMessages / wire Sync 同型, 见 conversation_types.h)
///
/// 设计说明:
/// - 通用字段 (role/text/startTimeMs/durationMs/collapsed) 平铺,
///   角色专属字段按 role 放入 optional 子结构 (tool/system/interrupt)
/// - 纯 UI 交互状态 (中断输入框编辑文本/选中项/校验提示/结果回传通道)
///   不属于消息内容, 由 MessageListComponent 独立维护 (InterruptUIState 表)
using TUIMessage = agentxx::agent::ViewMessage;

/// 排队等待发送的用户输入
struct TUIPendingInput {
    std::string text;
    bool        expanded = false;
};

/// 跨线程共享的渲染状态 (COW 语义)
///
/// 所有被 client 线程 (onDelta/onSync) 和 UI 线程 (渲染/事件) 并发访问的数据
/// 收敛于此结构, 经 shared_ptr + mutex 保护:
/// - 写方: 短锁 + COW (mutableState) 后修改
/// - 读方: 短锁拷贝 shared_ptr 快照, 之后无锁渲染
///
/// 性能设计 (流式输出的热路径):
/// - currentToken 为 shared_ptr<string>: 流式追加 token 时按需 COW 字符串本体,
///   避免每 token 深拷贝整个已累积文本 (O(n²) -> O(n))
/// - contextMessages 为 shared_ptr<json>: neograph::json 拷贝是深拷贝
///   (yyjson_mut_val_mut_copy 全树复制), 若放在 COW 全量拷贝内,
///   每 token 都会复制整个上下文 JSON; 指针化后 COW 拷贝仅 O(1)
struct TUIRenderState {
    std::vector<std::shared_ptr<TUIMessage>> messages;
    std::shared_ptr<std::string>             currentToken;
    TUIMessage::Role                         currentTokenRole = TUIMessage::Role::Assistant;
    bool                                     isStreaming      = false;

    int64_t pendingTokenDurationMs  = 0;
    int64_t pendingTokenStartTimeMs = 0;

    std::string currentNodeName;

    std::vector<std::string> modelNames;
    std::string              cachedModelName;
    /// 是否已收到服务端模型信息响应 (WireModelInfo):
    /// - false: 模型列表数据尚未加载 (弹窗应显示 loading)
    /// - true:  已收到响应 (若列表仍为空则说明确实无可用模型)
    bool modelInfoLoaded = false;

    std::deque<TUIPendingInput> pendingInputs;

    /// 上下文消息快照 (弹窗展示用); 为 null 表示尚未获取
    std::shared_ptr<neograph::json> contextMessages;

    bool showContextOverlay = false;

    std::vector<agentxx::agent::AppendComponentNotification> appendComponents;

    /// 系统资源占用快照 (CPU/内存/GPU), 由资源监控线程周期写入; 为 null 表示尚未采集
    std::shared_ptr<agentxx::expand::CpuGpuUsage> systemUsage;
};

/// COW 共享状态容器 (封装 mutex + shared_ptr + COW 辅助)
///
/// 线程模型:
/// - client 线程: 经 mutableState() 短锁 COW 写入
/// - UI 线程渲染: 经 snapshot() 短锁拷贝 shared_ptr, 之后无锁读取
/// - UI 线程事件: 经 mutableState() 短锁 COW 写入
class TUISharedState {
public:

    TUISharedState() :
        state_(std::make_shared<TUIRenderState>()) {}

    /// COW 写入: 获取可写引用; 若被快照共享 (use_count > 1) 则深拷贝结构
    /// 调用方须持有 lock()
    TUIRenderState& mutableState() {
        if (state_.use_count() > 1) {
            state_ = std::make_shared<TUIRenderState>(*state_);
        }
        return *state_;
    }

    /// COW 写入单条消息: 总是复制消息对象
    /// 调用方须持有 lock()
    ///
    /// 为什么总是复制 (而非 use_count > 1 时才复制):
    /// 1. 消除 use_count 读取与修改之间的数据竞争 —— UI 线程渲染结束释放快照时
    ///    (shared_ptr 析构) 与 client 线程读 use_count 无锁并发, 是未定义行为
    /// 2. 保证 "消息内容变化 -> 消息对象指针变化" 恒成立: 消息列表渲染缓存
    ///    以消息指针为失效 key, 指针不变即内容未变, 可直接复用缓存的 Element,
    ///    免去每帧对全部消息文本重新哈希的开销
    /// 复制成本: 低频事件 (Tool 结束 / 节点结束 / 折叠点击) 才触发, 可接受
    TUIMessage& mutableMessage(TUIRenderState& st, size_t idx) {
        st.messages[idx] = std::make_shared<TUIMessage>(*st.messages[idx]);
        return *st.messages[idx];
    }

    /// 快照: 拷贝 shared_ptr (纳秒级), 供 UI 线程本帧无锁渲染
    /// 调用方须持有 lock()
    std::shared_ptr<TUIRenderState> snapshot() {
        return state_;
    }

    std::mutex& mutex() {
        return mutex_;
    }

    /// 便捷: 加锁 + COW 写入 + 解锁
    void mutate(std::function<void(TUIRenderState&)> fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        fn(mutableState());
    }

    /// 便捷: 加锁 + 读取快照 + 解锁
    std::shared_ptr<TUIRenderState> readSnapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot();
    }

private:

    std::mutex                      mutex_;
    std::shared_ptr<TUIRenderState> state_;
};
