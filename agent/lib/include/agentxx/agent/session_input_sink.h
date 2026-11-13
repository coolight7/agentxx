#pragma once

#include <string_view>

namespace agentxx {
namespace agent {

/// 会话输入入口 (由"当前正在驱动这个会话的端点"实现)
///
/// 不变式: **一个会话在同一时刻只有一个驱动者** —— 事件投递不能自己 `co_spawn`
/// 一轮 `runTurnAsync`, 否则两次轮次会同时改同一个 `Session` 的上下文。因此
/// "往会话里塞一轮输入"统一经这个接口交回给当前驱动者排队。
///
/// 实现方: `SessionServerAgentIO` (服务端会话端点)。端点把弱引用登记在
/// `Session::inputSink` 上, 投递方 (事件中枢) 经 `Session` 取一次。
class SessionInputSink {
public:

    virtual ~SessionInputSink() = default;

    /// 会话是否还有待跑的轮次 (正在跑, 或队列里还有排队的输入)
    /// - 队列里还有条目也算"忙": 否则事件值会被插到它们后面, 白等一轮
    virtual bool turnActive() const = 0;

    /// 作为新一轮输入入队 (会话空闲时由驱动循环立即执行)
    ///
    /// - `args`:
    ///     - [text]          输入正文 (事件投递时为渲染好的 `[Event] ...` 文本)
    ///     - [source]        来源标记 (`event:<名字>`; 供诊断与消息来源显示)
    ///     - [unpausePaused] 是否解除"上一轮失败/中断"造成的暂停态
    /// - `return` false = 拒绝受理 (端点已停止 / 内容为空)
    virtual bool submitTurnInput(
        std::string_view text,
        std::string_view source,
        bool             unpausePaused
    ) = 0;

    /// 会话的后台任务表变了 (提交 / 起跑 / 终态)
    ///
    /// 后台任务段 (`WireContextStats::asyncWorks`) 由端点随上下文统计下发, 任务表变化
    /// 本身不会触发任何下发 —— 界面就看不到排队转运行、任务结束这些变化。端点实现
    /// 本回调发出后可以把最新快照重新下发 (TUI 信息侧边栏「后台任务」段)。
    ///
    /// - 未实现 (= 默认) 时什么都不做: 任务表变化对界面不可见, 只影响展示, 不影响任务
    /// - 可能在任意线程调用, 实现方需自行投递到自己的事件循环; 不得同步阻塞
    virtual void onAsyncWorksChanged() {}
};

} // namespace agent
} // namespace agentxx
