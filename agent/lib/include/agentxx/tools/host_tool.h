#pragma once

#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "neograph/types.h"
#include "utilxx_base/json.h"
#include <asio/awaitable.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace agentxx {
namespace tools {

/// 宿主 / 客户端反向调用工具桥接
///
/// 供 FFI 宿主与 WebSocket 客户端注册的动态工具使用:
/// - 继承 XXToolBase, 接入 Agent 统一工具调用调度与类型纠正
/// - get_definition 返回宿主注册的描述与 JSON Schema
/// - execute_async 委托给 SessionServerAgentIO 或 FFI 运行时, 向宿主发送调用并协程等待应答
class HostTool : public XXToolBase {
public:

    using CallHandler = std::function<asio::awaitable<std::string>(
        std::string_view         name,
        const utilxx_base::Json& args,
        uint32_t                 timeoutSec
    )>;

    HostTool(
        agent::WireHostToolInfo                     info,
        std::weak_ptr<agentxx::agent::AgentContext> ctx,
        CallHandler                                 handler
    ) :
        XXToolBase(info.name, ctx, false, false, 0, false, false),
        info_(std::move(info)),
        handler_(std::move(handler)) {}

    neograph::ChatTool get_definition() const override {
        neograph::ChatTool def;
        def.name        = info_.name;
        def.description = info_.description;
        // neograph::ChatTool::parameters 是 neograph::json 类型; 本层一律用
        // utilxx_base::Json, 跨图边界时经桥接转换
        def.parameters = agentxx::util::toNeographJson(info_.inputSchema);
        return def;
    }

    asio::awaitable<std::string> execute_async(const utilxx_base::Json& arguments) override {
        if (!handler_) {
            throw std::runtime_error("host tool handler not available");
        }
        co_return co_await handler_(info_.name, arguments, info_.timeoutSec);
    }

    const agent::WireHostToolInfo& info() const noexcept {
        return info_;
    }

private:

    agent::WireHostToolInfo info_;
    CallHandler             handler_;
};

} // namespace tools
} // namespace agentxx
