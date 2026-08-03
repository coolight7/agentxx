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

    void onLog(const agentxx::util::LogEntry& entry) override {
        std::cerr << entry.message << std::endl;
    }
};

class StdIOClientAgentIO : public agentxx::agent::AgentIOBase {
private:

    std::shared_ptr<StderrLogSink> logSink_;
    bool                           isThinking_ = false;

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

protected:

    // ---- AgentIOBase 被动接收回调 (client 端点实现; 仅由 onPeerMessage 分发) ----
    void onDelta(const agentxx::agent::Delta& delta) override;

    void onSync(const agentxx::agent::SyncPayload& payload) override;

    /// 处理对端消息: 拦截 WireAppendComponentInfo (启动信息统计), 其余委托基类
    void onPeerMessage(agentxx::agent::WireMessage msg) override;
};
