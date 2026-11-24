#include "ffi_ops.h"

#include "agentxx/ffi_api.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/io/user_input_json.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "ffi_client_io.h"
#include "ffi_runtime.h"
#include "utilxx_base/log.h"
#include <algorithm>
#include <utility>

namespace agentxx {
namespace ffi {

void CallContext::send(agent::WireMessage msg) const {
    if (!clientIO || clientIoCtx == nullptr) {
        return;
    }
    auto client = clientIO;
    asio::post(*clientIoCtx, [client, msg = std::move(msg)]() mutable {
        client->sendToPeer(std::move(msg));
    });
}

namespace {

// ---------------------------------------------------------------------------
// 参数读取 (只做类型与必填校验; 取值语义交给 wire 消息与服务端)
// ---------------------------------------------------------------------------

bool requiredString(
    const utilxx_base::Json& args,
    std::string_view         key,
    std::string&             out,
    std::string&             err
) {
    if (!args.contains(key)) {
        err = fmt::format("缺少参数 '{}'", key);
        return false;
    }
    const auto& v = args[key];
    if (!v.is_string()) {
        err = fmt::format("参数 '{}' 必须是字符串", key);
        return false;
    }
    out = v.get<std::string>();
    if (out.empty()) {
        err = fmt::format("参数 '{}' 不能为空", key);
        return false;
    }
    return true;
}

/// 可选字符串 (缺省保持 out 不变; 传了 null 视为未给)
bool optionalString(
    const utilxx_base::Json& args,
    std::string_view         key,
    std::string&             out,
    std::string&             err
) {
    if (!args.contains(key) || args[key].is_null()) {
        return true;
    }
    if (!args[key].is_string()) {
        err = fmt::format("参数 '{}' 必须是字符串", key);
        return false;
    }
    out = args[key].get<std::string>();
    return true;
}

/// 可选整数 (缺省保持 out 不变)
bool optionalInt(
    const utilxx_base::Json& args,
    std::string_view         key,
    int64_t&                 out,
    std::string&             err
) {
    if (!args.contains(key) || args[key].is_null()) {
        return true;
    }
    if (!args[key].is_number_integer()) {
        err = fmt::format("参数 '{}' 必须是整数", key);
        return false;
    }
    out = args[key].get<int64_t>();
    return true;
}

/// 可选布尔 (缺省保持 out 不变)
bool optionalBool(
    const utilxx_base::Json& args,
    std::string_view         key,
    bool&                    out,
    std::string&             err
) {
    if (!args.contains(key) || args[key].is_null()) {
        return true;
    }
    if (!args[key].is_boolean()) {
        err = fmt::format("参数 '{}' 必须是布尔值", key);
        return false;
    }
    out = args[key].get<bool>();
    return true;
}

/// 可选字符串数组 (缺省保持 out 不变)
bool optionalStringArray(
    const utilxx_base::Json&       args,
    std::string_view               key,
    std::vector<std::string>&      out,
    std::string&                   err
) {
    if (!args.contains(key) || args[key].is_null()) {
        return true;
    }
    if (!args[key].is_array()) {
        err = fmt::format("参数 '{}' 必须是字符串数组", key);
        return false;
    }
    out.clear();
    for (const auto& item : args[key]) {
        if (!item.is_string()) {
            err = fmt::format("参数 '{}' 的元素必须是字符串", key);
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

// ---------------------------------------------------------------------------
// 各 op 的处理函数
// ---------------------------------------------------------------------------

int32_t opInputSend(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    // 输入 JSON 解析与参数校验走公共实现 (CLI/FFI/插件同一套规则)
    auto parsed = agent::parseUserInputJsonValue(args);
    if (!parsed.has_value()) {
        err = parsed.error().message;
        return parsed.error().badJson ? AGENTXX_FFI_ERR_JSON : AGENTXX_FFI_ERR_INVALID;
    }
    auto input      = std::move(*parsed);
    input.sessionId = ctx.sessionId;
    // 受理回执按 requestId 回到本次调用 (reqId 直接当 wire requestId 用)
    input.requestId = ctx.reqId;
    ctx.send(std::move(input));
    return 0;
}

int32_t opInputCancel(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& /*args*/,
    const CallContext&       ctx,
    std::string&             /*err*/
) {
    agent::WireCancel cancel;
    cancel.sessionId = ctx.sessionId;
    cancel.reqId     = ctx.reqId;
    ctx.send(std::move(cancel));
    return 0;
}

int32_t opInputQueueClear(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& /*args*/,
    const CallContext&       ctx,
    std::string&             /*err*/
) {
    agent::WireClearMessageQueue msg;
    msg.sessionId = ctx.sessionId;
    msg.reqId     = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opInputQueueRemove(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    std::string itemId;
    if (!requiredString(args, "itemId", itemId, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    agent::WireRemoveQueueItem msg;
    msg.sessionId = ctx.sessionId;
    msg.itemId    = std::move(itemId);
    msg.reqId     = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opInputQueueRunNext(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& /*args*/,
    const CallContext&       ctx,
    std::string&             /*err*/
) {
    agent::WireInterruptAndRunNext msg;
    msg.sessionId = ctx.sessionId;
    msg.reqId     = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opModelList(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& /*args*/,
    const CallContext&       ctx,
    std::string&             /*err*/
) {
    agent::WireGetModel msg;
    msg.sessionId = ctx.sessionId;
    msg.reqId     = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opModelSelect(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    std::string name;
    // 空名字 = 沿用当前选择 (服务端回当前模型名)
    if (!optionalString(args, "name", name, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    agent::WireSelectModel msg;
    msg.sessionId = ctx.sessionId;
    msg.model     = std::move(name);
    msg.reqId     = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opModelAdd(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    if (!args.contains("name") || !args["name"].is_string()
        || args["name"].get<std::string>().empty()) {
        err = "模型配置缺少必填字段 name";
        return AGENTXX_FFI_ERR_CONFIG;
    }
    auto req      = agent::io::addModelFromJson(args);
    req.sessionId = ctx.sessionId;
    req.reqId     = ctx.reqId;
    ctx.send(std::move(req));
    return 0;
}

int32_t opModelRemove(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    std::string name;
    if (!requiredString(args, "name", name, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    agent::WireRemoveModel msg;
    msg.sessionId = ctx.sessionId;
    msg.name      = std::move(name);
    msg.reqId     = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opSessionList(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    agent::WireListSessions msg;
    int64_t                 beforeMs = 0;
    int64_t                 limit    = 50; // 默认一页 50 条 (结果限量)
    if (!optionalInt(args, "beforeMs", beforeMs, err)
        || !optionalInt(args, "limit", limit, err)
        || !optionalString(args, "beforeId", msg.beforeId, err)
        || !optionalString(args, "keyword", msg.keyword, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (limit < 0 || limit > 1000) {
        err = "参数 'limit' 超出范围 [0, 1000]";
        return AGENTXX_FFI_ERR_INVALID;
    }
    msg.beforeMs = beforeMs;
    msg.limit    = static_cast<uint32_t>(limit);
    msg.reqId    = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opSessionSwitch(
    FfiAgentRuntime&         rt,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    std::string sessionId;
    if (!requiredString(args, "sessionId", sessionId, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    // 本地绑定随提交一起切到新会话 (与旧实现一致): 切换之后提交的调用按新会话
    // 作用域处理。要确认切换真的生效, 就等这条回执再发下一条。
    rt.setSessionBinding(sessionId);
    agent::WireSwitchSession msg;
    msg.sessionId = std::move(sessionId);
    msg.reqId     = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opSessionRename(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    std::string title;
    if (!requiredString(args, "title", title, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    agent::WireRenameSession msg;
    msg.sessionId = ctx.sessionId;
    msg.title     = std::move(title);
    msg.reqId     = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opSessionCompact(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& /*args*/,
    const CallContext&       ctx,
    std::string&             /*err*/
) {
    agent::WireCompactContext msg;
    msg.sessionId = ctx.sessionId;
    msg.reqId     = ctx.reqId; // 回执在压缩真的结束后发出
    ctx.send(std::move(msg));
    return 0;
}

int32_t opContextGet(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    int64_t tail = 0;
    if (!optionalInt(args, "tail", tail, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (tail < 0) {
        err = "参数 'tail' 不能为负数 (0 = 整份上下文)";
        return AGENTXX_FFI_ERR_INVALID;
    }
    agent::WireGetContext msg;
    msg.sessionId = ctx.sessionId;
    msg.tail      = static_cast<uint32_t>(std::min<int64_t>(tail, INT32_MAX));
    msg.reqId     = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opContextViewPage(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    int64_t beforeIndex = 0;
    int64_t count       = 0;
    if (!optionalInt(args, "beforeIndex", beforeIndex, err)
        || !optionalInt(args, "count", count, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (beforeIndex < 0 || count < 0) {
        err = "参数 'beforeIndex' / 'count' 不能为负数";
        return AGENTXX_FFI_ERR_INVALID;
    }
    agent::WireGetViewMessages msg;
    msg.sessionId   = ctx.sessionId;
    msg.beforeIndex = static_cast<uint64_t>(beforeIndex);
    msg.count       = static_cast<uint32_t>(std::min<int64_t>(count, INT32_MAX));
    msg.reqId       = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opComponentsGet(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& /*args*/,
    const CallContext&       ctx,
    std::string&             /*err*/
) {
    agent::WireGetAppendComponentInfo msg;
    msg.sessionId = ctx.sessionId;
    msg.reqId     = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opPermissionGetState(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& /*args*/,
    const CallContext&       ctx,
    std::string&             /*err*/
) {
    agent::WireGetPermissionState msg;
    msg.reqId = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opPermissionSetFullAuth(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    bool fullAuth = true;
    if (!optionalBool(args, "fullAuth", fullAuth, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    agent::WireSetFullAuth msg;
    msg.fullAuth = fullAuth;
    msg.reqId    = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opFsListDir(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    agent::WireListDir msg;
    if (!optionalString(args, "path", msg.path, err)
        || !optionalStringArray(args, "allowedExtensions", msg.allowedExtensions, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    msg.reqId = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opInterruptRespond(
    FfiAgentRuntime&         rt,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    int64_t interruptId = 0;
    if (!optionalInt(args, "interruptId", interruptId, err) || interruptId == 0) {
        err = "缺少参数 'interruptId' (EVT_INTERRUPT_REQ 的 interruptId)";
        return AGENTXX_FFI_ERR_INVALID;
    }
    // 应答载荷恒为对象形态 {"values": {...}} (见 middleware::makeInterruptResult);
    // 非对象形态直接拒绝, 避免契约外的载荷被服务端当成"空应答"而误判为拒绝
    if (!args.contains("values") || !args["values"].is_object()) {
        err = "参数 'values' 必须是对象 {\"<控件 id>\": 值}";
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (!rt.hasPendingInterrupt(interruptId)) {
        // 本地已经知道这个中断不在等: 直接同步报错, 比等服务端 ack 更清楚
        err = fmt::format("中断 #{} 不存在、已应答或已过期", interruptId);
        return AGENTXX_FFI_ERR_INTERRUPT;
    }
    agent::WireInterruptResponse msg;
    msg.id     = interruptId;
    msg.result = utilxx_base::Json::object();
    msg.result["values"] = args["values"];
    msg.reqId            = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opHostToolRegister(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    if (!args.contains("tools") || !args["tools"].is_array()) {
        err = "参数 'tools' 必须是工具定义数组";
        return AGENTXX_FFI_ERR_INVALID;
    }
    agent::WireHostToolRegister msg;
    msg.sessionId = ctx.sessionId;
    msg.reqId     = ctx.reqId;
    for (const auto& item : args["tools"]) {
        if (!item.is_object()) {
            err = "参数 'tools' 的元素必须是对象";
            return AGENTXX_FFI_ERR_INVALID;
        }
        agent::WireHostToolInfo info;
        if (!requiredString(item, "name", info.name, err)) {
            return AGENTXX_FFI_ERR_INVALID;
        }
        if (!optionalString(item, "description", info.description, err)) {
            return AGENTXX_FFI_ERR_INVALID;
        }
        if (item.contains("inputSchema") && !item["inputSchema"].is_null()) {
            if (!item["inputSchema"].is_object()) {
                err = fmt::format("工具 '{}' 的 inputSchema 必须是对象", info.name);
                return AGENTXX_FFI_ERR_INVALID;
            }
            info.inputSchema = item["inputSchema"];
        }
        int64_t timeoutSec    = 0;
        int64_t maxConcurrent = 0;
        if (!optionalInt(item, "timeoutSec", timeoutSec, err)
            || !optionalInt(item, "maxConcurrent", maxConcurrent, err)) {
            return AGENTXX_FFI_ERR_INVALID;
        }
        info.timeoutSec    = static_cast<uint32_t>(std::max<int64_t>(0, timeoutSec));
        info.maxConcurrent = static_cast<uint32_t>(std::max<int64_t>(0, maxConcurrent));
        msg.tools.push_back(std::move(info));
    }
    if (msg.tools.empty()) {
        err = "参数 'tools' 不能为空";
        return AGENTXX_FFI_ERR_INVALID;
    }
    ctx.send(std::move(msg));
    return 0;
}

int32_t opHostToolUnregister(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    std::vector<std::string> names;
    if (!optionalStringArray(args, "names", names, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    // 兼容只给一个名字的写法 (`name` 字段)
    if (names.empty()) {
        std::string single;
        if (!optionalString(args, "name", single, err)) {
            return AGENTXX_FFI_ERR_INVALID;
        }
        if (!single.empty()) {
            names.push_back(std::move(single));
        }
    }
    if (names.empty()) {
        err = "参数 'names' 不能为空";
        return AGENTXX_FFI_ERR_INVALID;
    }
    agent::WireHostToolUnregister msg;
    msg.sessionId = ctx.sessionId;
    msg.names     = std::move(names);
    msg.reqId     = ctx.reqId;
    ctx.send(std::move(msg));
    return 0;
}

int32_t opHostToolRespond(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    int64_t callId = 0;
    if (!optionalInt(args, "callId", callId, err) || callId == 0) {
        err = "缺少参数 'callId' (EVT_HOST_TOOL_CALL 的 callId)";
        return AGENTXX_FFI_ERR_INVALID;
    }
    bool ok = true;
    if (!optionalBool(args, "ok", ok, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    agent::WireHostToolResult msg;
    msg.callId = callId;
    msg.ok     = ok;
    msg.reqId  = ctx.reqId;
    if (ok) {
        if (args.contains("result") && !args["result"].is_null()) {
            const auto& result = args["result"];
            // 结果按字符串给: 模型看到的就是这段文本; 对象/数组按紧凑 JSON 给
            msg.resultJson = result.is_string() ? result.get<std::string>() : result.dump();
        }
    } else if (args.contains("error") && !args["error"].is_null()) {
        const auto& error = args["error"];
        msg.errorMessage  = error.is_string() ? error.get<std::string>() : error.dump();
    }
    ctx.send(std::move(msg));
    return 0;
}

int32_t opPluginDataUp(
    FfiAgentRuntime&         /*rt*/,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    std::string topic;
    if (!requiredString(args, "topic", topic, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    agent::WirePluginDataUp msg;
    msg.topic = std::move(topic);
    msg.reqId = ctx.reqId;
    if (args.contains("data") && !args["data"].is_null()) {
        const auto& data = args["data"];
        msg.data         = data.is_string() ? data.get<std::string>() : data.dump();
    }
    ctx.send(std::move(msg));
    return 0;
}

int32_t opDeltaSetBatch(
    FfiAgentRuntime&         rt,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    int64_t maxDelayMs = 0;
    if (!optionalInt(args, "maxDelayMs", maxDelayMs, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (maxDelayMs < 0) {
        err = "参数 'maxDelayMs' 不能为负数 (0 = 关闭合批)";
        return AGENTXX_FFI_ERR_INVALID;
    }
    rt.applyDeltaBatch(static_cast<int32_t>(std::min<int64_t>(maxDelayMs, INT32_MAX)));
    utilxx_base::Json result = utilxx_base::Json::object();
    result["maxDelayMs"]     = maxDelayMs;
    if (ctx.completeLocal) {
        ctx.completeLocal(std::move(result));
    }
    return 0;
}

int32_t opLanguageGet(
    FfiAgentRuntime&         rt,
    const utilxx_base::Json& /*args*/,
    const CallContext&       ctx,
    std::string&             /*err*/
) {
    utilxx_base::Json result = utilxx_base::Json::object();
    result["language"]       = rt.language();
    if (ctx.completeLocal) {
        ctx.completeLocal(std::move(result));
    }
    return 0;
}

int32_t opLanguageSet(
    FfiAgentRuntime&         rt,
    const utilxx_base::Json& args,
    const CallContext&       ctx,
    std::string&             err
) {
    std::string language;
    if (!requiredString(args, "language", language, err)) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    const auto normalized = agent::normalizeLanguage(language);
    rt.applyLanguage(normalized);
    utilxx_base::Json result = utilxx_base::Json::object();
    result["language"]       = normalized;
    if (ctx.completeLocal) {
        ctx.completeLocal(std::move(result));
    }
    return 0;
}

} // namespace

const std::vector<FfiOpDef>& opsDefinition() {
    static const std::vector<FfiOpDef> s_ops = {
        // ---- 输入 ----
        {"input.send",
         FfiCompletion::TypedResponse,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opInputSend},
        {"input.cancel", FfiCompletion::Ack, 10000, AGENTXX_FFI_ERR_REJECTED, &opInputCancel},
        {"input.queue.clear",
         FfiCompletion::Ack,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opInputQueueClear},
        {"input.queue.remove",
         FfiCompletion::Ack,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opInputQueueRemove},
        {"input.queue.run_next",
         FfiCompletion::Ack,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opInputQueueRunNext},
        // ---- 模型 ----
        {"model.list", FfiCompletion::TypedResponse, 10000, AGENTXX_FFI_ERR_REJECTED, &opModelList},
        {"model.select",
         FfiCompletion::Ack,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opModelSelect},
        {"model.add", FfiCompletion::TypedResponse, 30000, AGENTXX_FFI_ERR_CONFIG, &opModelAdd},
        {"model.remove",
         FfiCompletion::TypedResponse,
         30000,
         AGENTXX_FFI_ERR_CONFIG,
         &opModelRemove},
        // ---- 会话 ----
        {"session.list",
         FfiCompletion::TypedResponse,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opSessionList},
        {"session.switch", FfiCompletion::Ack, 60000, AGENTXX_FFI_ERR_REJECTED, &opSessionSwitch},
        {"session.rename",
         FfiCompletion::TypedResponse,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opSessionRename},
        // 压缩可能跑很久: 默认不限时, 由调用方按需给 timeoutMs
        {"session.compact", FfiCompletion::Ack, 0, AGENTXX_FFI_ERR_REJECTED, &opSessionCompact},
        // ---- 上下文与界面 ----
        {"context.get",
         FfiCompletion::TypedResponse,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opContextGet},
        {"context.view_page",
         FfiCompletion::TypedResponse,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opContextViewPage},
        {"components.get",
         FfiCompletion::TypedResponse,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opComponentsGet},
        // ---- 权限 ----
        {"permission.get_state",
         FfiCompletion::TypedResponse,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opPermissionGetState},
        {"permission.set_full_auth",
         FfiCompletion::TypedResponse,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opPermissionSetFullAuth},
        // ---- 文件系统 ----
        {"fs.list_dir",
         FfiCompletion::TypedResponse,
         30000,
         AGENTXX_FFI_ERR_REJECTED,
         &opFsListDir},
        // ---- 中断 ----
        {"interrupt.respond",
         FfiCompletion::Ack,
         10000,
         AGENTXX_FFI_ERR_INTERRUPT,
         &opInterruptRespond},
        // ---- 嵌入方程序工具 ----
        {"hosttool.register",
         FfiCompletion::Ack,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opHostToolRegister},
        {"hosttool.unregister",
         FfiCompletion::Ack,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opHostToolUnregister},
        {"hosttool.respond",
         FfiCompletion::Ack,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opHostToolRespond},
        // ---- 插件上行数据 ----
        {"plugin.data_up",
         FfiCompletion::Ack,
         10000,
         AGENTXX_FFI_ERR_REJECTED,
         &opPluginDataUp},
        // ---- 本地 op (不进 wire) ----
        {"delta.set_batch", FfiCompletion::Local, 0, AGENTXX_FFI_ERR_INVALID, &opDeltaSetBatch},
        {"language.get", FfiCompletion::Local, 0, AGENTXX_FFI_ERR_INVALID, &opLanguageGet},
        {"language.set", FfiCompletion::Local, 0, AGENTXX_FFI_ERR_INVALID, &opLanguageSet},
    };
    return s_ops;
}

const FfiOpDef* findOp(std::string_view name) {
    for (const auto& def : opsDefinition()) {
        if (def.name == name) {
            return &def;
        }
    }
    return nullptr;
}

} // namespace ffi
} // namespace agentxx
