# TODO
- hyperscan 匹配 \w 时包含了换行符
- 移除不必要的锁


1. 修改 /home/coolight/program/agentxx/agent/lib/src/middlewares/summarization.cpp 的 modelSupportMaxToken，仅从模型配置session中提取使用，SummarizationMiddlewareHandle 初始化时改为指定默认值 defaultModelSupportMaxToken ，当 模型配置未指定最大 token时使用该值，并且该默认值默认设置为 256k

2. 正则表达式添加测试是否 \w 会包含换行符，如果是的话请修改 XXRegexp 的实现，让 \w 不要包含换行符 /home/coolight/program/agentxx/agent/lib/src/util/regex.cpp

3. /home/coolight/program/agentxx/agent/lib/include/agentxx/util/router.h 中的 cacheMap 改为使用 LRUCache /home/coolight/program/agentxx/agent/lib/include/agentxx/util/lru_cache.h

4. /home/coolight/program/agentxx/agent/lib/src/agent/deepagent.cpp 实现 runConversationTurnAsync 内的 TODO，根据 resumeInterrupt 已经是 true 时，跳过运行 [engine->run_stream_async]，重新执行 可能没处理的中断 和 [engine->resume_async]恢复