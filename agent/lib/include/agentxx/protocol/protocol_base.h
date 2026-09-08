#pragma once
// 协议公共基类/工具, 收敛 A2A/ACP/MCP 重复的 JSON-RPC/路由/Http 逻辑
#include "agentxx/util/http_server.h"
#include "agentxx/util/json.h"
#include "agentxx/util/log.h"

namespace agentxx {
namespace server {

// JSON-RPC 统一错误码 (MCP/A2A/ACP 共用)
inline constexpr int kJsonRpcParseError     = -32700;
inline constexpr int kJsonRpcInvalidRequest = -32600;
inline constexpr int kJsonRpcMethodNotFound = -32601;
inline constexpr int kJsonRpcInvalidParams  = -32602;
inline constexpr int kJsonRpcInternalError  = -32603;

inline agentxx::util::Json
    jsonRpcError(int code, std::string_view msg, std::optional<agentxx::util::Json> data = {}) {
    agentxx::util::Json err;
    err["code"]    = code;
    err["message"] = std::string(msg);
    if (data) {
        err["data"] = *data;
    }
    return err;
}

inline agentxx::util::Json jsonRpcResponse(agentxx::util::Json id, agentxx::util::Json result) {
    agentxx::util::Json r;
    r["jsonrpc"] = "2.0";
    r["id"]      = std::move(id);
    r["result"]  = std::move(result);
    return r;
}

inline agentxx::util::Json jsonRpcErrorResponse(agentxx::util::Json id, agentxx::util::Json error) {
    agentxx::util::Json r;
    r["jsonrpc"] = "2.0";
    r["id"]      = std::move(id);
    r["error"]   = std::move(error);
    return r;
}

inline void writeJsonResponse(
    util::HttpServer::Response& resp,
    boost::beast::http::status  status,
    const agentxx::util::Json&  body
) {
    resp.result(status);
    resp.set(boost::beast::http::field::content_type, "application/json");
    resp.body() = body.dump();
    resp.prepare_payload();
}

} // namespace server
} // namespace agentxx
