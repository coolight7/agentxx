#include "agentxx-client/util/util.h"

#include "agentxx/agent/config.h"
#include "agentxx/util/string_util.h"

std::shared_ptr<agentxx::agent::AgentConfig> buildDefaultConfig() {
    auto config               = std::make_shared<agentxx::agent::AgentConfig>();
    config->currentSystemName = agentxx::util::getSystemName();
    config->isSystemWSL       = agentxx::util::isRunningInWSL();
    // 会话 SQLite 持久化: 消息上下文/展示历史/share store 落库
    // ~/.agentxx/sqlite/{threadId}/, 重启后恢复会话
    config->enableSessionPersistence = true;

    return config;
}

std::shared_ptr<agentxx::agent::AgentConfig> makeSubAgentConfig(
    std::shared_ptr<agentxx::agent::AgentConfig> base,
    std::string_view                             systemPrompt
) {
    auto cfg                                   = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model                                 = base->getSubagentModel();
    cfg->agentName                             = fmt::format("{}_sub", base->agentName);
    cfg->agentNameView                         = base->agentNameView;
    cfg->prompt.systemPrompt                   = systemPrompt;
    cfg->logPrintToolcall                      = false;
    cfg->logPrintMessagesBeforeLLM             = false;
    cfg->logPrintSummarizationResultTokenCount = false;
    return cfg;
}
