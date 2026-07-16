#include "agentxx/middlewares/middleware.h"
#include "agentxx/tools/tool.h"
#include <charconv>

agentxx::middleware::BaseMiddlewareHandleInterface::
    BaseMiddlewareHandleInterface(
        std::string_view in_name,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
    : name(in_name), agentContext(in_agentContext) {}
