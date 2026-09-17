#pragma once
// 协议公共基类/工具, 收敛 A2A/ACP/MCP 重复的 JSON-RPC/路由/Http 逻辑
#include "utilxx/http_server.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

namespace agentxx {
namespace protocol {

// JSON-RPC 统一错误码 (MCP/A2A/ACP 共用)
inline constexpr int kJsonRpcParseError     = -32700;
inline constexpr int kJsonRpcInvalidRequest = -32600;
inline constexpr int kJsonRpcMethodNotFound = -32601;
inline constexpr int kJsonRpcInvalidParams  = -32602;
inline constexpr int kJsonRpcInternalError  = -32603;

inline utilxx_base::Json
    jsonRpcError(int code, std::string_view msg, std::optional<utilxx_base::Json> data = {}) {
    utilxx_base::Json err;
    err["code"]    = code;
    err["message"] = std::string(msg);
    if (data) {
        err["data"] = *data;
    }
    return err;
}

inline utilxx_base::Json jsonRpcResponse(utilxx_base::Json id, utilxx_base::Json result) {
    utilxx_base::Json r;
    r["jsonrpc"] = "2.0";
    r["id"]      = std::move(id);
    r["result"]  = std::move(result);
    return r;
}

inline utilxx_base::Json jsonRpcErrorResponse(utilxx_base::Json id, utilxx_base::Json error) {
    utilxx_base::Json r;
    r["jsonrpc"] = "2.0";
    r["id"]      = std::move(id);
    r["error"]   = std::move(error);
    return r;
}

inline void writeJsonResponse(
    utilxx::HttpServer::Response& resp,
    boost::beast::http::status  status,
    const utilxx_base::Json&  body
) {
    resp.result(status);
    resp.set(boost::beast::http::field::content_type, "application/json");
    resp.body() = body.dump();
    resp.prepare_payload();
}

} // namespace protocol

// 兼容别名
namespace server = protocol;

} // namespace agentxx
