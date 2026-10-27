#pragma once

/// 插件运行时记账 (根 / 通道 / 触发器 / 定时器)
///
/// 这是 `agentxx.agent.runtime` 表的**主程序侧数据**: 插件经接口表挂根、开通道、
/// 登记触发器后, 主程序在这里记"谁挂了什么"。全部字段只在主程序 io 线程读写
/// (接口表入口一律经 `ioCallSyncKeep` 投递到该线程), 因此不需要锁。
///
/// 三条归属规则:
/// - 每个对象都带 `owner` (插件实例名): 插件禁用/卸载时按它整批回收;
/// - 根与通道用 `int64_t` / `std::string` 句柄寻址, 插件手里只有 id, 不传裸指针;
/// - 通道记录持有 [AgentSessionRuntime] (两端点 + 驱动协程), 关通道就是停它。
#include "agentxx/agent/config.h"
#include "agentxx/agent/session_runtime.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "asio/steady_timer.hpp"
#include "fmt/format.h"
#include "utilxx_base/json.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace agentxx {
namespace agent {

class BaseAgent;
class AgentHost;

} // namespace agent

namespace plugin {

class PluginChannelAgentIO;
class PluginInstance;

/// 一个挂载的根 (插件自己的 agent)
struct RuntimeRoot {
    /// 根 id (= 该根的 agentId; 也是 `host->mountRoot` 用的 id)
    std::string rootId;
    /// 归属者 (插件实例名)
    std::string owner;
    /// 展示名 (spec.name)
    std::string name;
    /// 派生后生效的配置 (保活: 根的 agent 持有同一份 shared_ptr)
    std::shared_ptr<agent::AgentConfig> config;
    /// 根 agent (init 完成前也在这里: 记录它的进度)
    std::shared_ptr<agent::BaseAgent> agent;
    /// 该根的插件挂载策略摘要 (诊断用: 与 `plugin.mount` 同构的 JSON)
    std::string mountSummary;

    /// 是否已 init 完成并挂进容器 (未就绪时开通道会排队)
    bool ready = false;
    /// init 是否失败 (失败后该根下通道收到错误并关闭)
    bool failed = false;
    /// 失败原因 / 卸载原因 (诊断用)
    std::string error;

    /// 该根下已建立的通道 id
    std::vector<int64_t> channels;
    /// 等待该根就绪后再建立的通道 id
    std::vector<int64_t> waitingChannels;

    /// 会话存储根 (规范化前; 通道登记会话占用时用)
    std::string storeRoot;

    /// 会话占用键里的"作用域"片段
    /// - 有会话存储: 就是存储根 (同一份磁盘会话只能有一个驱动者);
    /// - 没有会话存储 (内存会话): 用根 id —— 内存会话按 agent 各自存放,
    ///   不同根下的同名会话互不相干, 用空串会把它们误判成同一份
    std::string sessionKey() const {
        return storeRoot.empty() ? fmt::format("mem:{}", rootId) : storeRoot;
    }

    bool alive() const noexcept {
        return !failed && agent != nullptr;
    }
};

/// 一条会话通道 (一个 sessionId 的驱动壳)
struct RuntimeChannel {
    int64_t channelId = 0;
    /// 归属者 (插件实例名)
    std::string owner;
    /// 绑定的根 id
    std::string rootId;
    /// 会话 id (空 = 由框架生成后写回)
    std::string sessionId;
    /// 会话标题 (可空)
    std::string title;
    /// 初始模型 (可空 = 根的默认模型)
    std::string model;
    /// 中断等待上限 (0 = 不限)
    std::chrono::milliseconds interruptTimeout{0};
    /// 事件回调与其上下文 (由插件提供; 通道关闭后不再调用)
    AgentxxPluginChannelEventFn onEvent   = nullptr;
    void*                        userData = nullptr;

    /// 驱动壳 (根就绪后才建)
    std::shared_ptr<agent::AgentSessionRuntime> runtime;
    /// client 端点 (事件来源; 与 runtime 同生命周期)
    std::shared_ptr<PluginChannelAgentIO> clientIO;

    /// 是否已就绪 (收到 EVENT_READY 之前为 false)
    bool ready = false;
    /// 是否已关闭 (关闭后不再有事件)
    bool closed = false;
    /// 是否还在等根就绪
    bool waiting = false;
    /// 当前是否在跑一轮 (DROP_IF_BUSY 与队列背压的判断依据)
    bool turnBusy = false;
    /// 已丢弃的触发器投递条数 (诊断用)
    uint64_t droppedInputs = 0;
};

/// 触发器投递策略取值的包装 (供记录与 SDK 共用)
struct RuntimeTrigger {
    int64_t     triggerId = 0;
    std::string owner;
    /// 最终主题 (已补齐 `plugin.` 前缀)
    std::string topic;
    int64_t     channelId = 0;
    /// AGENTXX_PLUGIN_TRIGGER_POLICY_*
    int32_t  policy = AGENTXX_PLUGIN_TRIGGER_POLICY_QUEUE;
    int32_t  debounceMs = 300;
    int32_t  maxQueue   = 20;
    /// 投递模式 (InputDelivery; 空 = next-turn)
    std::string delivery;
    /// 事件订阅句柄 (0 = 未订阅)
    size_t subscriptionId = 0;
    /// COALESCE 的合并窗口定时器 (仅该策略使用)
    std::shared_ptr<asio::steady_timer> debounceTimer;
    /// 待合并的输入 JSON 文本 (按到达顺序)
    std::vector<std::string> pending;
    /// 已丢弃 / 已合并的条数 (诊断用)
    uint64_t dropped = 0;
    uint64_t merged  = 0;
};

/// 定时触发 (一次性或周期)
struct RuntimeTimer {
    int64_t     timerId = 0;
    std::string owner;
    int64_t     channelId = 0;
    /// AGENTXX_PLUGIN_TRIGGER_POLICY_*
    int32_t policy = AGENTXX_PLUGIN_TRIGGER_POLICY_DROP_IF_BUSY;
    /// 周期间隔 (0 = 一次性)
    std::chrono::milliseconds interval{0};
    /// 投递的输入 JSON
    std::string inputJson;
    /// 定时器 (到期回调里挂下一次)
    std::shared_ptr<asio::steady_timer> timer;
    /// 已触发次数 (诊断用)
    uint64_t fired = 0;
};

/// 每个根的通道数上限 (群数量级; 与设计文档 §11 的资源预算一致)
inline constexpr size_t kMaxChannelsPerRoot = 64;

/// 每实例触发器 + 定时器的总数上限
inline constexpr size_t kMaxTriggersPerInstance = 256;

/// 通道输入队列上限的默认值 (spec.max_queue = 0 时使用)
inline constexpr int32_t kDefaultChannelQueue = 100;

} // namespace plugin
} // namespace agentxx
