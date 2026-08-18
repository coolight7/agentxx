#pragma once

#include "agentxx/agent/io/agent_io.h"
#include "asio/awaitable.hpp"
#include "neograph/json.h"
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

class StderrLogSink : public agentxx::util::ThreadedLogSink {
public:

    ~StderrLogSink() override {
        // 在虚表仍为本类时停止日志线程 (见 ThreadedLogSink::shutdownThread):
        // 若延迟到基类析构, 线程执行纯虚 onLog 虚调用 -> purecall -> abort
        // (进程退出瞬间日志刚入队时最易触发, 如无配置文件启动立即退出)
        shutdownThread();
    }

    void onLog(const agentxx::util::LogEntry& entry) override {
        std::cerr << entry.message << std::endl;
    }
};

class StdIOClientAgentIO : public agentxx::agent::AgentIOBase {
private:

    std::shared_ptr<StderrLogSink> logSink_;
    bool                           isThinking_ = false;
    /// 当前会话 thread_id (mode_runners 装配时设置; 插件代发消息用)
    std::string threadId_;

public:

    asio::awaitable<std::optional<std::string>> getInput() override;

    asio::awaitable<neograph::json> handleInterrupt(
        std::string_view threadId,
        std::string_view interruptNode,
        std::string_view interruptValue,
        std::string_view interruptArgJson
    ) override;

    StdIOClientAgentIO();
    ~StdIOClientAgentIO() override;

    /// 设置当前会话 thread_id (mode_runners 在建立会话时调用)
    void setThreadId(std::string threadId) {
        threadId_ = std::move(threadId);
    }

    // -----------------------------------------------------------------------
    // 插件适配器接口 (CliPluginAdapter 在 client io 线程调用; 线程安全)
    // -----------------------------------------------------------------------

    /// 代发用户消息 (与用户输入同路径: 经 transport 发送 WireUserInput;
    /// 发送后通知事件接收器)
    void sendPluginUserInput(const std::string& text);

    /// 跨端插件数据上行: WirePluginDataUp → agent 侧插件
    /// 返回 true 表示已投递 (未连接等失败返回 false)
    bool sendPluginDataUp(
        const std::string& plugin,
        const std::string& event,
        const std::string& json
    );

protected:

    // ---- AgentIOBase 被动接收回调 (client 端点实现; 仅由 onPeerMessage 分发) ----
    void onDelta(const agentxx::agent::Delta& delta) override;

    void onSync(const agentxx::agent::SyncPayload& payload) override;

    /// 处理对端消息: 拦截 WireAppendComponentInfo (启动信息统计), 其余委托基类
    void onPeerMessage(agentxx::agent::WireMessage msg) override;
};
