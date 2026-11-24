#pragma once

#include "agentxx/agent/io/agent_io_transport.h"
#include "asio/io_context.hpp"
#include "utilxx_base/json.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace asio = ::boost::asio;

namespace agentxx {
namespace ffi {

class FfiAgentRuntime;
class FfiClientAgentIO;

/// 一次调用的执行上下文 (runtime 受理后填好交给 op 处理函数)
struct CallContext {
    /// 本次调用的 seq (wire 请求直接把它当 reqId/requestId 用)
    uint64_t reqId = 0;
    /// op 定义 (默认超时 / 失败码 / 名称)
    const struct FfiOpDef* def = nullptr;
    /// 当前绑定会话 (会话作用域由 FFI 自动填, 调用方不用写 sessionId)
    std::string sessionId;
    /// client 端点 (存活到本次调用结束; 收尾后为空)
    std::shared_ptr<FfiClientAgentIO> clientIO;
    /// client io 的执行器 (投递 wire 消息用)
    asio::io_context* clientIoCtx = nullptr;
    /// 本地 op 的完成回调 (wire op 不调用它)
    std::function<void(utilxx_base::Json)> completeLocal;

    /// 投递一条 wire 消息到 client io (任意线程安全)
    void send(agent::WireMessage msg) const;
};

/// op 的完成方式 (决定回执从哪来)
enum class FfiCompletion : uint8_t {
    /// 等对应的 wire 应答 (typed 应答), 应答里的 reqId 与本调用的 reqId 相同
    TypedResponse = 0,
    /// 等通用回执 `WireAck`
    Ack,
    /// 不进 wire: 处理函数当场给出结果
    Local,
};

/// op 定义 (`opsDefinition` 里逐条列出, 名字是稳定字符串)
struct FfiOpDef {
    /// op 名 (`input.send` / `model.list` ...)
    std::string_view name;
    /// 完成方式
    FfiCompletion completion = FfiCompletion::Ack;
    /// 未给 `timeoutMs` 时的默认超时 (毫秒; 0 = 不限)
    int32_t defaultTimeoutMs = 10000;
    /// 失败回执的默认错误码 (`AGENTXX_FFI_ERR_*`)
    int32_t failureCode = -10;
    /// 处理函数: 0 = 已发起 (结果经回执); 非 0 = 同步失败 (err 给原因, 无回执)
    int32_t (*run)(FfiAgentRuntime&, const utilxx_base::Json&, const CallContext&, std::string&)
        = nullptr;
};

/// op 全集 (顺序即能力清单里的顺序)
const std::vector<FfiOpDef>& opsDefinition();

/// 按名查 op; 未知名返回 nullptr
const FfiOpDef* findOp(std::string_view name);

} // namespace ffi
} // namespace agentxx
