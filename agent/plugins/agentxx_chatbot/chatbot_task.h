#pragma once

/// 后台任务 (`chatbot_task` 工具 / `/agentxx-cmd task` / 面板「后台任务」段)
///
/// 分工:
/// - **并发与排队归主程序** (`agentxx.agent.runtime` 表的 `submit_async_work`,
///   每队列额度见 `async_work.max_concurrent`): 本文件不自己开线程、不自己限并发;
/// - **本文件管业务字段**: 谁提交的、显示名、终态结果摘要、取消与文本渲染;
/// - 结果回到聊天会话由主程序负责 (`notify_delivery = next-step`): 任务跑完把结果
///   投成一条会话输入, 由人设组织语言回给用户; 通道已经关掉时由入口文件用
///   `ChatbotOutbound` 直接发一条摘要 (被动→主动降级)。
///
/// 线程: 全部入口只在插件 io 线程调用 (与调度层一致) —— 工具工作体先投递到插件
/// io 线程, 任务事件回调由入口文件 post 到插件 io 线程再进来。
///
/// 本头只依赖标准库与 `utilxx_base::Json`, 不引用主程序头 (插件边界),
/// 因此可以直接编进测试 (`chatbot_task` 模块) 做纯逻辑回归。
#include "chatbot_core.h"
#include "chatbot_types.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace chatbot {

/// 任务状态 (`queued` → `running` → 终态)
enum class TaskState : uint8_t {
    Queued = 0,
    Running,
    Done,
    Failed,
    Canceled,
};

/// 状态名 (与主程序任务表同一套: `queued` / `running` / `done` / `failed` / `canceled`)
const char* taskStateName(TaskState state) noexcept;

/// 是不是还在进行 (排队或运行中)
bool taskStatePending(TaskState state) noexcept;

/// 一条任务的业务视图 (工具 / 命令 / 面板共用)
struct TaskRecord {
    /// 主程序分配的 id (`async-7`); 也是任务会话的 sessionId
    std::string asyncId;
    /// 提交它的会话 (`account|type|chatId`)
    std::string chatKey;
    /// 归属人设名
    std::string persona;
    /// 显示名 (提交时给的 `title`; 没给时取正文开头)
    std::string title;
    /// 提交时的正文 (截断后留档, 供 `status` 回看)
    std::string promptHead;
    TaskState state = TaskState::Queued;
    /// 排队时前面还有几条 (起跑后是 0)
    int32_t queueAhead = 0;
    int64_t submittedAtMs = 0;
    int64_t startedAtMs   = 0;
    int64_t finishedAtMs  = 0;
    bool    hasError      = false;
    /// 错误正文 (终态失败/取消时可能有)
    std::string errorText;
    /// 结果正文的开头 (终态成功时才有; 完整结果在会话里)
    std::string resultHead;
};

/// 提交请求 (业务字段; 运行参数由插件入口按人设配置补齐)
struct TaskSubmit {
    std::string chatKey;
    std::string persona;
    /// 显示名 (可空 = 取正文开头)
    std::string title;
    /// 任务正文 (给后台 agent 的输入; 空会被拒)
    std::string prompt;
    /// 模型覆盖 (可空 = 人设的模型 / 主程序默认)
    std::string model;
};

/// 运行期注入点 (由插件入口实现, 转成主程序 SDK 调用)
struct TaskRuntime {
    /// 提交 → asyncId (空 = 失败, 原因写 `err`)
    std::function<std::string(const TaskSubmit&, std::string& err)> submit;
    /// 取消 (返回主程序错误码; 0 = 成功)
    std::function<int32_t(std::string_view asyncId)> cancel;
    /// 单条任务 JSON (主程序任务表; 终态取结果正文用; 拿不到返回空串)
    std::function<std::string(std::string_view asyncId)> resultOf;
};

/// 提交结果 (给模型/用户看的文本与 id)
struct TaskSubmitResult {
    bool        ok = false;
    std::string asyncId;
    std::string message;
};

/// 后台任务业务层
///
/// 只做三件事: 记账 (提交时的业务字段) / 跟状态 (任务事件) / 出文本 (工具与命令)。
/// 真正的执行、并发额度、排队、取消与结果事件都在主程序。
class ChatbotTaskManager {
public:

    /// 生效参数
    struct Options {
        /// 每个会话同时进行 (排队 + 运行) 的任务上限; 超出直接拒绝 (不排队)
        size_t maxPendingPerChat = 4;
        /// 任务记录最多留多少条 (超出丢最旧的已结束记录; 进行中的不丢)
        size_t maxRecords = 40;
        /// 终态记录保留多久 (毫秒; 0 = 一直留到被 maxRecords 挤掉)
        int64_t keepFinishedMs = 30 * 60 * 1000;
        /// 结果摘要保留多少码点
        size_t resultHeadChars = 400;
        /// 正文留档多少码点
        size_t promptHeadChars = 200;
        /// 时钟 (毫秒; 默认单调时钟; 测试注入固定值)
        std::function<int64_t()> clock;
    };

    void init(TaskRuntime runtime, Options options = {});

    /// 提交一条任务
    ///
    /// - 同一会话进行中的条数达到 `maxPendingPerChat` 时直接拒绝 (让模型改用等待)
    /// - 注入点缺失 (老主程序 / 运行时表降级) 时返回可读的失败说明
    TaskSubmitResult submit(const TaskSubmit& request);

    /// 取消
    ///
    /// - `asyncId` 非空: 只取消这一条 (必须属于 `chatKey`)
    /// - `asyncId` 为空: 取消该会话全部进行中的任务
    /// - `return` 给模型/用户看的说明
    std::string cancel(std::string_view chatKey, std::string_view asyncId);

    /// 任务事件 (`phase` = `AGENTXX_PLUGIN_ASYNC_WORK_EVT_*`)
    ///
    /// - 只更新状态, 不做任何跨线程动作; 返回 true = 可见状态变了 (入口据此刷面板)
    bool onEvent(std::string_view asyncId, int32_t phase, std::string_view payloadJson);

    /// 本会话的任务 (最近提交的在前)
    std::vector<TaskRecord> listOf(std::string_view chatKey) const;

    /// 全部任务 (面板用; 最近提交的在前)
    std::vector<TaskRecord> allRecords() const;

    /// 单条任务 (找不到返回 nullptr)
    const TaskRecord* recordOf(std::string_view asyncId) const;

    /// 该会话进行中的条数 (排队 + 运行)
    size_t pendingCount(std::string_view chatKey) const;

    /// 该会话是否还有进行中的任务 (面板刷新节拍据此决定要不要继续转)
    bool hasPending(std::string_view chatKey) const;

    /// 全部进行中的条数 (排队 + 运行; 面板刷新节拍用)
    size_t pendingTotal() const;

    /// 文本: 单个会话的任务清单 (给模型看)
    std::string listText(std::string_view chatKey) const;

    /// 文本: 单条任务详情 (id 为空 = 该会话最近一条)
    std::string statusText(std::string_view chatKey, std::string_view asyncId) const;

    /// 文本: 提交成功的回执 (模型据此知道"已受理, 结果会回来")
    static std::string submittedText(const TaskRecord& record);

    /// 文本: 任务完成/失败/取消的通知正文 (无通道时直接发到群里用)
    static std::string finishedText(const TaskRecord& record);

    /// 文本: 投给会话的「系统提示」(带一句"这不是群成员发言"的说明)
    static std::string resultNote(const TaskRecord& record);

    /// 清空 (stop 时)
    void clear();

    /// 丢掉过期的终态记录 (submit/onEvent 时顺手调; 也可以外部调)
    void prune();

private:

    TaskRecord* mutableRecordOf(std::string_view asyncId);

    /// 现在 (毫秒)
    int64_t nowMs() const;

    TaskRuntime runtime_;
    Options     options_;
    /// 按提交顺序追加 (最新的在尾部)
    std::vector<TaskRecord> records_;
};

/// 一条任务的 JSON (工具/命令输出用)
std::string taskRecordJson(const TaskRecord& record);

/// 时长文本 (`12s` / `3m20s` / `2h05m`; `ms <= 0` 返回 `0s`)
std::string formatTaskElapsed(int64_t ms);

} // namespace chatbot
} // namespace agentxx
