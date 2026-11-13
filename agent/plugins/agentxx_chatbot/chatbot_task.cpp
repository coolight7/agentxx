#include "chatbot_task.h"

#include "utilxx_base/json.h"

#include <algorithm>
#include <chrono>

namespace agentxx {
namespace chatbot {

namespace {

using utilxx_base::Json;

/// 主程序的任务事件阶段 (与 `plugin_api.h` 的 `AGENTXX_PLUGIN_ASYNC_WORK_EVT_*` 一致)
///
/// 这里写一份常量而不是包含 `plugin_api.h`: 本文件要能编进测试目标, 而测试不链接
/// 插件 SDK 头的那套宏 (值必须与 SDK 一致, `boundaries` 之外没有强校验, 改 SDK 时
/// 记得同步这里)。
constexpr int32_t kEventQueued   = 0;
constexpr int32_t kEventStarted  = 1;
constexpr int32_t kEventFinished = 2;
constexpr int32_t kEventCanceled = 3;

std::string textOf(const Json& object, std::string_view key) {
    if (!object.is_object() || !object.contains(key)) {
        return {};
    }
    const auto& value = object[key];
    return value.is_string() ? value.get<std::string>() : std::string{};
}

int64_t int64Of(const Json& object, std::string_view key, int64_t fallback = 0) {
    if (!object.is_object() || !object.contains(key)) {
        return fallback;
    }
    const auto& value = object[key];
    return value.is_number() ? value.get<int64_t>() : fallback;
}

bool boolOf(const Json& object, std::string_view key) {
    if (!object.is_object() || !object.contains(key)) {
        return false;
    }
    const auto& value = object[key];
    return value.is_boolean() ? value.get<bool>() : false;
}

/// 取一段正文的开头 (超出上限补省略号)
std::string headOf(std::string_view text, size_t maxCodePoints) {
    if (maxCodePoints == 0 || utf8CodePointCount(text) <= maxCodePoints) {
        return std::string{text};
    }
    return std::string{utf8Truncate(text, maxCodePoints)} + "…";
}

/// 正文压成一行 (换行/连续空白 → 单个空格), 面板与列表都用单行
std::string singleLine(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool lastSpace = false;
    for (const char ch : text) {
        const bool isSpace = ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ';
        if (isSpace) {
            if (!lastSpace && !out.empty()) {
                out.push_back(' ');
            }
            lastSpace = true;
            continue;
        }
        out.push_back(ch);
        lastSpace = false;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

} // namespace

const char* taskStateName(TaskState state) noexcept {
    switch (state) {
        case TaskState::Queued:
            return "queued";
        case TaskState::Running:
            return "running";
        case TaskState::Done:
            return "done";
        case TaskState::Failed:
            return "failed";
        case TaskState::Canceled:
            return "canceled";
    }
    return "unknown";
}

bool taskStatePending(TaskState state) noexcept {
    return state == TaskState::Queued || state == TaskState::Running;
}

std::string formatTaskElapsed(int64_t ms) {
    if (ms <= 0) {
        return "0s";
    }
    const int64_t totalSeconds = ms / 1000;
    if (totalSeconds < 60) {
        // 不到 1 秒也给 `1s`, 免得面板里一直显示 0
        return std::to_string(std::max<int64_t>(1, totalSeconds)) + "s";
    }
    const int64_t minutes = totalSeconds / 60;
    const int64_t seconds = totalSeconds % 60;
    if (minutes < 60) {
        return std::to_string(minutes) + "m" + std::to_string(seconds / 10) + std::to_string(
            seconds % 10
        ) + "s";
    }
    const int64_t hours = minutes / 60;
    const int64_t mins  = minutes % 60;
    std::string   out   = std::to_string(hours) + "h";
    if (mins < 10) {
        out += "0";
    }
    out += std::to_string(mins) + "m";
    return out;
}

void ChatbotTaskManager::init(TaskRuntime runtime, Options options) {
    runtime_ = std::move(runtime);
    options_ = std::move(options);
    records_.clear();
}

int64_t ChatbotTaskManager::nowMs() const {
    if (options_.clock) {
        return options_.clock();
    }
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void ChatbotTaskManager::clear() {
    records_.clear();
}

void ChatbotTaskManager::prune() {
    if (options_.keepFinishedMs <= 0) {
        return;
    }
    const int64_t now       = nowMs();
    const int64_t keepMs    = options_.keepFinishedMs;
    const auto    tooOld = [now, keepMs](const TaskRecord& record) {
        if (taskStatePending(record.state)) {
            return false;
        }
        return record.finishedAtMs > 0 && now - record.finishedAtMs > keepMs;
    };
    std::erase_if(records_, tooOld);
}

TaskSubmitResult ChatbotTaskManager::submit(const TaskSubmit& request) {
    TaskSubmitResult result;
    const std::string prompt = trimCopy(request.prompt);
    if (prompt.empty()) {
        result.message = "提交失败: 任务正文为空 (把要做的事写清楚)";
        return result;
    }
    if (!runtime_.submit) {
        result.message
            = "提交失败: 当前主程序没有后台任务入口 (需要 agentxx.agent.runtime 表的 "
              "submit_async_work), 请改为在本轮直接做完";
        return result;
    }
    prune();
    if (options_.maxPendingPerChat > 0
        && pendingCount(request.chatKey) >= options_.maxPendingPerChat) {
        result.message = "提交失败: 这个会话已经有 "
                         + std::to_string(pendingCount(request.chatKey))
                         + " 个后台任务在排队/运行 (上限 "
                         + std::to_string(options_.maxPendingPerChat)
                         + "), 先等它们出结果或取消掉一些";
        return result;
    }

    TaskSubmit filled = request;
    filled.prompt     = prompt;
    std::string err;
    const auto  asyncId = runtime_.submit(filled, err);
    if (asyncId.empty()) {
        result.message = "提交失败: " + (err.empty() ? std::string{"主程序拒绝了这次提交"} : err);
        return result;
    }

    TaskRecord record;
    record.asyncId       = asyncId;
    record.chatKey       = request.chatKey;
    record.persona       = request.persona;
    record.title         = request.title.empty()
                               ? headOf(singleLine(prompt), 24)
                               : headOf(singleLine(request.title), 24);
    record.promptHead    = headOf(singleLine(prompt), options_.promptHeadChars);
    record.state         = TaskState::Queued;
    record.submittedAtMs = nowMs();
    records_.push_back(std::move(record));

    // 任务记录只留最近的若干条: 先丢过期的已结束记录, 再丢最旧的已结束记录
    if (options_.maxRecords > 0 && records_.size() > options_.maxRecords) {
        const size_t overflow = records_.size() - options_.maxRecords;
        size_t       dropped  = 0;
        for (auto it = records_.begin(); it != records_.end() && dropped < overflow;) {
            if (taskStatePending(it->state)) {
                ++it;
                continue;
            }
            it      = records_.erase(it);
            ++dropped;
        }
    }

    result.ok      = true;
    result.asyncId = asyncId;
    result.message = submittedText(records_.back());
    return result;
}

std::string ChatbotTaskManager::cancel(std::string_view chatKey, std::string_view asyncId) {
    if (!runtime_.cancel) {
        return "取消失败: 当前主程序没有后台任务入口";
    }
    std::vector<std::string> targets;
    if (!asyncId.empty()) {
        const auto* record = recordOf(asyncId);
        if (record == nullptr || record->chatKey != chatKey) {
            return "取消失败: 这个会话里没有任务 " + std::string{asyncId};
        }
        if (!taskStatePending(record->state)) {
            return "任务 " + std::string{asyncId} + " 已经结束 (" + taskStateName(record->state)
                   + "), 不用取消";
        }
        targets.emplace_back(asyncId);
    } else {
        for (const auto& record : records_) {
            if (record.chatKey == chatKey && taskStatePending(record.state)) {
                targets.push_back(record.asyncId);
            }
        }
        if (targets.empty()) {
            return "这个会话没有进行中的后台任务";
        }
    }

    size_t canceled = 0;
    for (const auto& id : targets) {
        if (runtime_.cancel(id) == 0) {
            ++canceled;
        }
    }
    if (canceled == 0) {
        return "取消失败: 主程序没有接受取消请求 (任务可能刚好结束)";
    }
    return "已取消 " + std::to_string(canceled) + " 个后台任务"
           + (targets.size() > 1 ? "" : (" (" + targets.front() + ")"));
}

TaskRecord* ChatbotTaskManager::mutableRecordOf(std::string_view asyncId) {
    for (auto& record : records_) {
        if (record.asyncId == asyncId) {
            return &record;
        }
    }
    return nullptr;
}

const TaskRecord* ChatbotTaskManager::recordOf(std::string_view asyncId) const {
    for (const auto& record : records_) {
        if (record.asyncId == asyncId) {
            return &record;
        }
    }
    return nullptr;
}

bool ChatbotTaskManager::onEvent(
    std::string_view asyncId,
    int32_t          phase,
    std::string_view payloadJson
) {
    auto* record = mutableRecordOf(asyncId);
    if (record == nullptr) {
        // 不是本实例经手提交的 (或已被任务记录挤掉): 不认
        return false;
    }
    Json payload;
    try {
        payload = payloadJson.empty() ? Json::object() : Json::parse(std::string{payloadJson});
    } catch (const std::exception&) {
        payload = Json::object();
    }

    const int64_t now = nowMs();
    switch (phase) {
        case kEventQueued:
            record->state      = TaskState::Queued;
            record->queueAhead = static_cast<int32_t>(int64Of(payload, "queueAhead"));
            return true;
        case kEventStarted:
            record->state        = TaskState::Running;
            record->queueAhead   = 0;
            record->startedAtMs  = now;
            return true;
        case kEventFinished:
        case kEventCanceled: {
            const bool canceled = phase == kEventCanceled || boolOf(payload, "canceled");
            record->hasError    = boolOf(payload, "hasError");
            record->state
                = canceled ? TaskState::Canceled
                           : (record->hasError ? TaskState::Failed : TaskState::Done);
            record->finishedAtMs = now;
            record->queueAhead   = 0;
            record->errorText    = headOf(textOf(payload, "error"), options_.promptHeadChars);
            if (record->startedAtMs == 0) {
                // 排队中被取消: 没有起跑时间, 用提交时间当作结束时间的起点
                record->startedAtMs = record->submittedAtMs;
            }
            // 结果正文不在事件载荷里 (可能很大): 终态时向主程序任务表要一次
            if (record->state == TaskState::Done && runtime_.resultOf) {
                const auto json = runtime_.resultOf(record->asyncId);
                if (!json.empty()) {
                    try {
                        const auto entry = Json::parse(json);
                        record->resultHead
                            = headOf(textOf(entry, "result"), options_.resultHeadChars);
                    } catch (const std::exception&) {
                        // 任务记录读不到就当没有正文 (会话里仍然有完整结果)
                    }
                }
            }
            return true;
        }
        default:
            return false;
    }
}

std::vector<TaskRecord> ChatbotTaskManager::listOf(std::string_view chatKey) const {
    std::vector<TaskRecord> out;
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if (it->chatKey == chatKey) {
            out.push_back(*it);
        }
    }
    return out;
}

std::vector<TaskRecord> ChatbotTaskManager::allRecords() const {
    std::vector<TaskRecord> out(records_.rbegin(), records_.rend());
    return out;
}

size_t ChatbotTaskManager::pendingCount(std::string_view chatKey) const {
    size_t count = 0;
    for (const auto& record : records_) {
        if (record.chatKey == chatKey && taskStatePending(record.state)) {
            ++count;
        }
    }
    return count;
}

bool ChatbotTaskManager::hasPending(std::string_view chatKey) const {
    return pendingCount(chatKey) > 0;
}

size_t ChatbotTaskManager::pendingTotal() const {
    size_t count = 0;
    for (const auto& record : records_) {
        if (taskStatePending(record.state)) {
            ++count;
        }
    }
    return count;
}

std::string ChatbotTaskManager::listText(std::string_view chatKey) const {
    const auto records = listOf(chatKey);
    if (records.empty()) {
        return "这个会话还没有后台任务";
    }
    size_t pending = 0;
    for (const auto& record : records) {
        if (taskStatePending(record.state)) {
            ++pending;
        }
    }
    std::string out = "本会话后台任务 " + std::to_string(records.size()) + " 条 (进行中 "
                      + std::to_string(pending) + "):";
    const int64_t now = nowMs();
    for (const auto& record : records) {
        out += "\n- " + record.asyncId + " [" + taskStateName(record.state) + "] ";
        if (taskStatePending(record.state)) {
            const int64_t from
                = record.startedAtMs > 0 ? record.startedAtMs : record.submittedAtMs;
            out += formatTaskElapsed(now - from);
            if (record.state == TaskState::Queued && record.queueAhead > 0) {
                out += " 排队第 " + std::to_string(record.queueAhead) + " 位";
            }
        } else if (record.finishedAtMs > record.submittedAtMs) {
            out += formatTaskElapsed(record.finishedAtMs - record.submittedAtMs);
        }
        out += " " + record.title;
        if (record.state == TaskState::Failed && !record.errorText.empty()) {
            out += " (" + headOf(record.errorText, 80) + ")";
        }
    }
    out += "\n用 chat 工具查询单条详情: {op:\"status\", id:\"<id>\"}";
    return out;
}

std::string ChatbotTaskManager::statusText(
    std::string_view chatKey,
    std::string_view asyncId
) const {
    const TaskRecord* record = nullptr;
    /// 自己拿的一份副本 (列表返回的是值; 指针不能指向临时量)
    std::vector<TaskRecord> own;
    if (!asyncId.empty()) {
        record = recordOf(asyncId);
        if (record != nullptr && record->chatKey != chatKey) {
            record = nullptr;
        }
    } else {
        own = listOf(chatKey);
        for (const auto& item : own) {
            if (taskStatePending(item.state)) {
                record = &item;
                break;
            }
        }
        if (record == nullptr && !own.empty()) {
            record = &own.front();
        }
    }
    if (record == nullptr) {
        return asyncId.empty() ? std::string{"这个会话没有后台任务"}
                               : ("这个会话里没有任务 " + std::string{asyncId});
    }

    const int64_t now = nowMs();
    std::string   out = "任务 " + record->asyncId + " [" + taskStateName(record->state) + "]";
    out += "\n名字: " + record->title;
    if (!record->persona.empty()) {
        out += "\n人设: " + record->persona;
    }
    if (taskStatePending(record->state)) {
        const int64_t from
            = record->startedAtMs > 0 ? record->startedAtMs : record->submittedAtMs;
        out += "\n已进行: " + formatTaskElapsed(now - from);
        if (record->state == TaskState::Queued) {
            out += " (排队第 " + std::to_string(record->queueAhead) + " 位)";
        }
    } else {
        out += "\n耗时: "
               + formatTaskElapsed(
                   record->finishedAtMs > record->submittedAtMs
                       ? record->finishedAtMs - record->submittedAtMs
                       : 0
               );
    }
    if (!record->promptHead.empty()) {
        out += "\n内容: " + record->promptHead;
    }
    if (!record->resultHead.empty()) {
        out += "\n结果开头: " + record->resultHead;
    }
    if (!record->errorText.empty()) {
        out += "\n错误: " + record->errorText;
    }
    return out;
}

std::string ChatbotTaskManager::submittedText(const TaskRecord& record) {
    return "已受理后台任务 " + record.asyncId + " (" + record.title + ")\n"
           "它在后台自己跑, 结果跑完会作为一条新输入回到这个会话, 你不用在这里等; "
           "想让用户知道进度可以先用一句话说明, 然后正常结束这一轮。\n"
           "要查状态用 {op:\"status\", id:\"" + record.asyncId
           + "\"}, 要取消用 {op:\"cancel\", id:\"" + record.asyncId + "\"}。";
}

std::string ChatbotTaskManager::finishedText(const TaskRecord& record) {
    std::string out = "后台任务 " + record.asyncId + " ";
    switch (record.state) {
        case TaskState::Done:
            out += "已完成";
            break;
        case TaskState::Failed:
            out += "失败";
            break;
        case TaskState::Canceled:
            out += "已取消";
            break;
        default:
            out += "有进展";
            break;
    }
    if (!record.title.empty()) {
        out += ": " + record.title;
    }
    if (!record.resultHead.empty()) {
        out += "\n" + record.resultHead;
    } else if (!record.errorText.empty()) {
        out += "\n" + record.errorText;
    }
    return out;
}

std::string ChatbotTaskManager::resultNote(const TaskRecord& record) {
    std::string out = "[系统提示] ";
    out += finishedText(record);
    out += "\n(这是插件投递的后台任务结果, 不是群成员发言; 需要的话用 chatbot_send_message "
           "把结论转告用户, 不需要就按静默约定只写 [静默])。";
    return out;
}

std::string taskRecordJson(const TaskRecord& record) {
    Json item = Json::object();
    item["id"]           = record.asyncId;
    item["state"]        = taskStateName(record.state);
    item["title"]        = record.title;
    item["persona"]      = record.persona;
    item["chatKey"]      = record.chatKey;
    item["queueAhead"]   = record.queueAhead;
    item["submittedAtMs"] = record.submittedAtMs;
    item["startedAtMs"]   = record.startedAtMs;
    item["finishedAtMs"]  = record.finishedAtMs;
    item["hasError"]      = record.hasError;
    if (!record.resultHead.empty()) {
        item["resultHead"] = record.resultHead;
    }
    if (!record.errorText.empty()) {
        item["error"] = record.errorText;
    }
    return item.dump();
}

} // namespace chatbot
} // namespace agentxx
