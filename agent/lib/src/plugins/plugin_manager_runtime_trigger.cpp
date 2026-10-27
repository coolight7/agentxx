/// 插件运行时: 事件驱动的触发器与定时器
///
/// 原语只有两个: `send_input` (立即投一条) 与 `register_trigger` (按规则投)。
/// 触发器只做三件事 —— 订阅一个主题、按策略决定何时投递、把载荷转成输入;
/// 定时器只是"事件源换成时间"的同一种投递路径。`BaseAgent` 的执行循环一行不改。
///
/// 三种策略 (取值见 `AGENTXX_PLUGIN_TRIGGER_POLICY_*`):
/// - `QUEUE`: 每条立即投 (会话队列保证串行);
/// - `COALESCE`: 窗口内的条目合并成一条 (窗口每来一条就重置), 到期一起投;
/// - `DROP_IF_BUSY`: 通道正在跑一轮时丢弃并计数。
///
/// 背压: COALESCE 的合并缓冲有两条上限 —— 单条输入 8 KB 文本、累计 64 条。
/// 达到上限先投已累积的内容再开新窗口, 避免一群刷屏把上下文一次打满。
///
/// 线程: 入口都在主程序 IO 线程; 定时器与合并窗口用 `asio::steady_timer` (同一
/// 执行器), 到期回调也在该线程。
#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/user_input_json.h"
#include "agentxx/util/exception.h"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>

namespace agentxx {
namespace plugin {

namespace {

/// 合并窗口默认值等预算常量取自 runtime_state.h (kMaxTriggersPerInstance)

/// COALESCE 合并窗口的默认值 (毫秒)
constexpr int32_t kDefaultDebounceMs = 300;

/// 合并缓冲的条数上限与文本上限 (背压)
constexpr size_t kCoalesceMaxItems    = 64;
constexpr size_t kCoalesceMaxTextSize = 8 * 1024;

/// 输入 JSON 的正文 (用于合并时换行拼接; 解析失败返回原文本)
std::string textOfInputJson(std::string_view inputJson, const utilxx_base::Json& parsed) {
    const auto text = parsed.value("text", std::string{});
    return text.empty() ? std::string{inputJson} : text;
}

/// 把合并缓冲拼成一条输入 JSON
std::string joinInputs(const std::vector<std::string>& parts, std::string_view delivery) {
    std::string joined;
    for (const auto& part : parts) {
        if (!joined.empty()) {
            joined += "\n";
        }
        joined += part;
    }
    utilxx_base::Json out = utilxx_base::Json::object();
    out["text"]           = joined;
    if (!delivery.empty()) {
        out["delivery"] = std::string{delivery};
    }
    return out.dump();
}

/// 事件载荷 -> 输入 JSON
/// - 有 `text` / `attachments` 时按输入同构取;
/// - 都没有时把整条事件文本当输入 (调试方便, 但能给出 text 更好)。
std::string inputJsonOfEvent(std::string_view payload, std::string_view delivery) {
    utilxx_base::Json parsed;
    bool              usable = false;
    try {
        parsed = utilxx_base::Json::parse(payload);
        usable = parsed.is_object()
                 && (parsed.contains("text") || parsed.contains("attachments")
                     || parsed.contains("input"));
    } catch (const std::exception&) {
        usable = false;
    }
    utilxx_base::Json out = utilxx_base::Json::object();
    if (usable) {
        // `input` 是若干示例插件用的写法: 与 `text` 同义, 优先取 text
        if (parsed.contains("text") && parsed["text"].is_string()) {
            out["text"] = parsed["text"];
        } else if (parsed.contains("input") && parsed["input"].is_string()) {
            out["text"] = parsed["input"];
        }
        if (parsed.contains("attachments") && parsed["attachments"].is_array()) {
            out["attachments"] = parsed["attachments"];
        }
        if (parsed.contains("model") && parsed["model"].is_string()) {
            out["model"] = parsed["model"];
        }
    } else if (!payload.empty()) {
        out["text"] = std::string{payload};
    } else {
        out["text"] = std::string{"{}"};
    }
    if (!delivery.empty()) {
        out["delivery"] = std::string{delivery};
    }
    return out.dump();
}

} // namespace

// =====================================================================
// 触发器
// =====================================================================

int32_t PluginManager::runtimeRegisterTrigger(
    PluginInstance*                 inst,
    const AgentxxPluginTriggerSpec* spec,
    int64_t*                        outTriggerId,
    std::string&                    err
) {
    if (inst == nullptr || spec == nullptr || outTriggerId == nullptr) {
        err = "register_trigger: 参数为空";
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    const std::string topic = svToStr(spec->topic);
    if (topic.empty()) {
        err = "register_trigger: topic 必填";
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    if (spec->policy != AGENTXX_PLUGIN_TRIGGER_POLICY_QUEUE
        && spec->policy != AGENTXX_PLUGIN_TRIGGER_POLICY_COALESCE
        && spec->policy != AGENTXX_PLUGIN_TRIGGER_POLICY_DROP_IF_BUSY) {
        err = fmt::format("register_trigger: policy 取值非法 {}", spec->policy);
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    auto* channel = runtimeChannelOf(inst, spec->channel_id);
    if (channel == nullptr) {
        err = fmt::format(
            "register_trigger: 通道 {} 不存在或不属于本实例",
            spec->channel_id
        );
        return AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND;
    }
    const std::string delivery = svToStr(spec->delivery);
    if (!delivery.empty() && !agent::InputDelivery::known(delivery)) {
        err = fmt::format(
            "register_trigger: delivery 取值非法 '{}' (支持 next-turn/next-step/inject/collect)",
            delivery
        );
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    if (runtimeTriggers_.size() + runtimeTimers_.size() >= kMaxTriggersPerInstance) {
        err = fmt::format(
            "register_trigger: 触发器与定时器合计已达上限 {}",
            kMaxTriggersPerInstance
        );
        return AGENTXX_PLUGIN_RUNTIME_ERR_BUDGET;
    }

    auto trigger        = std::make_shared<RuntimeTrigger>();
    trigger->triggerId  = nextRuntimeHandle_++;
    trigger->owner      = inst->name;
    trigger->topic      = qualifyEventTopic(topic);
    trigger->channelId  = spec->channel_id;
    trigger->policy     = spec->policy;
    trigger->debounceMs = spec->debounce_ms > 0 ? spec->debounce_ms : kDefaultDebounceMs;
    trigger->maxQueue   = spec->max_queue > 0 ? spec->max_queue : 20;
    trigger->delivery   = delivery;

    // 订阅走通用事件表的后端 (与插件自己订阅是同一套: 主题补齐、实例租约、撤销)
    auto source = eventSource(nullptr);
    if (!source) {
        err = "register_trigger: 本进程没有事件总线";
        return AGENTXX_PLUGIN_RUNTIME_ERR_UNSUPPORTED;
    }
    std::weak_ptr<PluginManager> weakSelf = weak_from_this();
    auto                         triggerId = trigger->triggerId;
    trigger->subscriptionId                = source->subscribe(
        trigger->topic,
        [weakSelf, triggerId](std::string_view payload) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->onTriggerEvent(triggerId, payload);
        }
    );
    if (trigger->subscriptionId == 0) {
        err = fmt::format("register_trigger: 订阅主题 '{}' 失败", trigger->topic);
        return AGENTXX_PLUGIN_RUNTIME_ERR_FAILED;
    }
    if (trigger->policy == AGENTXX_PLUGIN_TRIGGER_POLICY_COALESCE) {
        trigger->debounceTimer = std::make_shared<asio::steady_timer>(ioExecutor());
    }
    runtimeTriggers_.push_back(trigger);
    *outTriggerId = triggerId;
    XX_LOGI(
        "[runtime] trigger {} 已登记 (topic='{}', channel={}, policy={})",
        triggerId,
        trigger->topic,
        trigger->channelId,
        trigger->policy
    );
    return AGENTXX_PLUGIN_RUNTIME_OK;
}

int32_t PluginManager::runtimeUnregisterTrigger(PluginInstance* inst, int64_t triggerId) {
    if (inst == nullptr || triggerId <= 0) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    auto it = std::find_if(
        runtimeTriggers_.begin(),
        runtimeTriggers_.end(),
        [triggerId](const std::shared_ptr<RuntimeTrigger>& item) {
            return item && item->triggerId == triggerId;
        }
    );
    if (it == runtimeTriggers_.end() || (*it)->owner != inst->name) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND;
    }
    auto trigger = *it;
    if (trigger->subscriptionId != 0) {
        if (auto source = eventSource(nullptr); source) {
            source->unsubscribe(trigger->topic, trigger->subscriptionId);
        }
        trigger->subscriptionId = 0;
    }
    if (trigger->debounceTimer) {
        trigger->debounceTimer->cancel();
    }
    runtimeTriggers_.erase(it);
    return AGENTXX_PLUGIN_RUNTIME_OK;
}

int32_t
    PluginManager::runtimeEmitEvent(PluginInstance* inst, std::string_view topic, std::string_view payload) {
    if (inst == nullptr || topic.empty()) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    return publish(topic, payload);
}

void PluginManager::onTriggerEvent(int64_t triggerId, std::string_view payload) {
    auto it = std::find_if(
        runtimeTriggers_.begin(),
        runtimeTriggers_.end(),
        [triggerId](const std::shared_ptr<RuntimeTrigger>& item) {
            return item && item->triggerId == triggerId;
        }
    );
    if (it == runtimeTriggers_.end()) {
        return;
    }
    auto  trigger = *it;
    auto* channel = runtimeChannelOf(nullptr, trigger->channelId);
    if (channel == nullptr || channel->closed) {
        return;
    }
    auto input = inputJsonOfEvent(payload, trigger->delivery);

    // QUEUE: 每条立即投; 会话队列保证串行
    if (trigger->policy == AGENTXX_PLUGIN_TRIGGER_POLICY_QUEUE) {
        std::string err;
        if (runtimeSendInput(nullptr, channel->channelId, input, err) != AGENTXX_PLUGIN_RUNTIME_OK) {
            trigger->dropped++;
            XX_LOGW("[runtime] trigger {} 投递失败: {}", triggerId, err);
        }
        return;
    }

    // DROP_IF_BUSY: 通道在跑一轮时丢弃 (定时巡检/状态刷新用)
    if (trigger->policy == AGENTXX_PLUGIN_TRIGGER_POLICY_DROP_IF_BUSY) {
        if (channel->turnBusy) {
            trigger->dropped++;
            XX_LOGD("[runtime] trigger {} 丢弃一条 (通道在忙)", triggerId);
            return;
        }
        std::string err;
        if (runtimeSendInput(nullptr, channel->channelId, input, err) != AGENTXX_PLUGIN_RUNTIME_OK) {
            trigger->dropped++;
            XX_LOGW("[runtime] trigger {} 投递失败: {}", triggerId, err);
        }
        return;
    }

    // COALESCE: 累计到窗口结束再投; 超过背压上限先投再开新窗口
    trigger->pending.push_back(std::move(input));
    trigger->merged++;
    size_t totalBytes = 0;
    for (const auto& item : trigger->pending) {
        totalBytes += item.size();
    }
    if (trigger->pending.size() >= kCoalesceMaxItems || totalBytes >= kCoalesceMaxTextSize
        || static_cast<int32_t>(trigger->pending.size()) >= trigger->maxQueue) {
        flushTrigger(triggerId);
        return;
    }
    if (trigger->debounceTimer) {
        trigger->debounceTimer->expires_after(std::chrono::milliseconds(trigger->debounceMs));
        std::weak_ptr<PluginManager> weakSelf = weak_from_this();
        trigger->debounceTimer->async_wait([weakSelf, triggerId](const utilxx_base::AsioErrorCode& ec) {
            if (ec) {
                return; // 定时器被取消 (窗口重置或撤销)
            }
            if (auto self = weakSelf.lock()) {
                self->flushTrigger(triggerId);
            }
        });
    }
}

void PluginManager::flushTrigger(int64_t triggerId) {
    auto it = std::find_if(
        runtimeTriggers_.begin(),
        runtimeTriggers_.end(),
        [triggerId](const std::shared_ptr<RuntimeTrigger>& item) {
            return item && item->triggerId == triggerId;
        }
    );
    if (it == runtimeTriggers_.end()) {
        return;
    }
    auto trigger = *it;
    if (trigger->pending.empty()) {
        return;
    }
    std::vector<std::string> parts;
    parts.swap(trigger->pending);
    if (trigger->debounceTimer) {
        trigger->debounceTimer->cancel();
    }
    auto* channel = runtimeChannelOf(nullptr, trigger->channelId);
    if (channel == nullptr || channel->closed) {
        trigger->dropped += parts.size();
        return;
    }
    std::string err;
    const auto  joined = joinInputs(parts, trigger->delivery);
    if (runtimeSendInput(nullptr, channel->channelId, joined, err) != AGENTXX_PLUGIN_RUNTIME_OK) {
        trigger->dropped += parts.size();
        XX_LOGW("[runtime] trigger {} 合并投递失败: {}", triggerId, err);
    }
}

// =====================================================================
// 定时器
// =====================================================================

int32_t PluginManager::runtimeRegisterTimer(
    PluginInstance*               inst,
    const AgentxxPluginTimerSpec* spec,
    int64_t*                      outTimerId,
    std::string&                  err
) {
    if (inst == nullptr || spec == nullptr || outTimerId == nullptr) {
        err = "register_timer: 参数为空";
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    auto* channel = runtimeChannelOf(inst, spec->channel_id);
    if (channel == nullptr) {
        err = fmt::format("register_timer: 通道 {} 不存在或不属于本实例", spec->channel_id);
        return AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND;
    }
    const std::string inputJson = svToStr(spec->input_json);
    if (inputJson.empty()) {
        err = "register_timer: input_json 必填 (形状同 send_input)";
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    // 输入先解析一次: 参数写错现在就能报, 不用等到第一次触发
    if (auto parsed = agent::parseUserInputJson(inputJson); !parsed.has_value()) {
        err = fmt::format("register_timer: {}", parsed.error().message);
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    if (spec->first_delay_ms < 0 || spec->interval_ms < 0) {
        err = "register_timer: first_delay_ms / interval_ms 不能为负";
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    if (runtimeTriggers_.size() + runtimeTimers_.size() >= kMaxTriggersPerInstance) {
        err = fmt::format(
            "register_timer: 触发器与定时器合计已达上限 {}",
            kMaxTriggersPerInstance
        );
        return AGENTXX_PLUGIN_RUNTIME_ERR_BUDGET;
    }
    if (!ioExecutor()) {
        err = "register_timer: 本进程没有可用的 io 执行器";
        return AGENTXX_PLUGIN_RUNTIME_ERR_UNSUPPORTED;
    }

    auto timer        = std::make_shared<RuntimeTimer>();
    timer->timerId    = nextRuntimeHandle_++;
    timer->owner      = inst->name;
    timer->channelId  = spec->channel_id;
    timer->policy     = (spec->policy == AGENTXX_PLUGIN_TRIGGER_POLICY_QUEUE
                             || spec->policy == AGENTXX_PLUGIN_TRIGGER_POLICY_COALESCE
                             || spec->policy == AGENTXX_PLUGIN_TRIGGER_POLICY_DROP_IF_BUSY)
                            ? spec->policy
                            : AGENTXX_PLUGIN_TRIGGER_POLICY_DROP_IF_BUSY;
    timer->interval   = std::chrono::milliseconds(spec->interval_ms);
    timer->inputJson  = inputJson;
    timer->timer      = std::make_shared<asio::steady_timer>(ioExecutor());
    runtimeTimers_.push_back(timer);

    const auto timerId = timer->timerId;
    scheduleTimer(timerId, std::chrono::milliseconds(spec->first_delay_ms));
    *outTimerId = timerId;
    return AGENTXX_PLUGIN_RUNTIME_OK;
}

void PluginManager::scheduleTimer(int64_t timerId, std::chrono::milliseconds delay) {
    auto it = std::find_if(
        runtimeTimers_.begin(),
        runtimeTimers_.end(),
        [timerId](const std::shared_ptr<RuntimeTimer>& item) {
            return item && item->timerId == timerId;
        }
    );
    if (it == runtimeTimers_.end() || !(*it)->timer) {
        return;
    }
    auto timer = *it;
    if (delay.count() < 0) {
        delay = std::chrono::milliseconds{0};
    }
    timer->timer->expires_after(delay);
    std::weak_ptr<PluginManager> weakSelf = weak_from_this();
    timer->timer->async_wait([weakSelf, timerId](const utilxx_base::AsioErrorCode& ec) {
        if (ec) {
            return; // 撤销/重新计时
        }
        if (auto self = weakSelf.lock()) {
            self->fireTimer(timerId);
        }
    });
}

void PluginManager::fireTimer(int64_t timerId) {
    auto it = std::find_if(
        runtimeTimers_.begin(),
        runtimeTimers_.end(),
        [timerId](const std::shared_ptr<RuntimeTimer>& item) {
            return item && item->timerId == timerId;
        }
    );
    if (it == runtimeTimers_.end()) {
        return;
    }
    auto timer = *it;
    auto* channel = runtimeChannelOf(nullptr, timer->channelId);
    if (channel == nullptr || channel->closed) {
        // 通道关了: 定时器失去意义, 直接注销 (不用等插件自己记得清)
        if (timer->timer) {
            timer->timer->cancel();
        }
        runtimeTimers_.erase(it);
        return;
    }
    timer->fired++;
    const bool busy = channel->turnBusy;
    if (timer->policy != AGENTXX_PLUGIN_TRIGGER_POLICY_DROP_IF_BUSY || !busy) {
        std::string err;
        if (runtimeSendInput(nullptr, channel->channelId, timer->inputJson, err)
            != AGENTXX_PLUGIN_RUNTIME_OK) {
            XX_LOGW("[runtime] timer {} 投递失败: {}", timerId, err);
        }
    } else {
        XX_LOGD("[runtime] timer {} 丢弃一次 (通道在忙)", timerId);
    }
    // 周期定时器挂下一次; 一次性在触发后自动注销
    if (timer->interval.count() > 0) {
        scheduleTimer(timerId, timer->interval);
    } else {
        runtimeTimers_.erase(it);
    }
}

int32_t PluginManager::runtimeUnregisterTimer(PluginInstance* inst, int64_t timerId) {
    if (inst == nullptr || timerId <= 0) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    auto it = std::find_if(
        runtimeTimers_.begin(),
        runtimeTimers_.end(),
        [timerId](const std::shared_ptr<RuntimeTimer>& item) {
            return item && item->timerId == timerId;
        }
    );
    if (it == runtimeTimers_.end() || (*it)->owner != inst->name) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND;
    }
    if ((*it)->timer) {
        (*it)->timer->cancel();
    }
    runtimeTimers_.erase(it);
    return AGENTXX_PLUGIN_RUNTIME_OK;
}

} // namespace plugin
} // namespace agentxx
