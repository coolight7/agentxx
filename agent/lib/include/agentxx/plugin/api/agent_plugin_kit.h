/// 插件开发 SDK (C++ header-only) —— agent 侧业务部分
///
/// 包含本头即可写 agent 侧插件: `PluginBase` (实例上下文基类, 接口表聚合为 `AgentIfaces`)、
/// 工具注册与模式构建、图节点、工具权限声明、功能埋点实现行、会话运行时与后台任务
/// 便捷调用, 以及 `AGENTXX_AGENT_PLUGIN_*_EXPORT` 导出宏。
///
/// 通用设施 (广播 / 网络 / 生命周期入口公共实现) 在 `plugin_kit.h`, 本头已包含它。
#pragma once
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/tool_result_frame.h"
#include "agentxx/util/tool_result.h"

namespace agentxx {
namespace plugin {

/* ==================== 工具结果状态 (agentxx::util) ====================
 *
 * 插件工具的实现体可以返回 `std::string` (成功) 或 `ToolResult` (带状态):
 * - `ToolResult::ok(文本)` 等价于直接返回文本;
 * - `ToolResult::fail(错误内容)` 表示"执行完成但失败", 错误内容只写正文
 *   (不加 `[Error: ...]` 前缀 —— 主程序统一排版);
 * - `ToolResult::canceled()` = 被取消。
 * 任何一方都不要为了判断成败去看结果文本。
 */
using agentxx::util::ToolResult;
using agentxx::util::ToolStatus;

/* ==================== 接口表聚合 ==================== */

/// agent 侧接口表聚合 (一次查询; 成员为 NULL 表示主程序未实现该接口)
struct AgentIfaces {
    const AgentxxPluginToolsIface*      tools      = nullptr; ///< "agentxx.agent.tools"
    const AgentxxPluginPermissionIface* permission = nullptr; ///< "agentxx.agent.permission"
    const PluginxxCapabilitiesIface* capabilities = nullptr; ///< "pluginxx.capabilities"
    const PluginxxSchedulerIface*    scheduler    = nullptr; ///< "pluginxx.scheduler"
    /// "agentxx.agent.session": 会话共享存储 / 提示 / 工作目录 / 取消查询
    const AgentxxPluginSessionIface* session = nullptr; ///< "agentxx.agent.session"
    /// "agentxx.agent.context": 会话 LLM 上下文查询 (图状态不含上下文内容)
    const AgentxxPluginContextIface*   context   = nullptr; ///< "agentxx.agent.context"
    const PluginxxPluginsIface*        plugins   = nullptr; ///< "pluginxx.plugins"
    const PluginxxConfigIface*         config    = nullptr; ///< "pluginxx.config"
    const AgentxxPluginPromptIface*    prompt    = nullptr; ///< "agentxx.agent.prompt"
    const PluginxxJsonIface*           json      = nullptr; ///< "pluginxx.json"
    const PluginxxLogIface*            log       = nullptr; ///< "pluginxx.log"
    const AgentxxPluginResourcesIface* resources = nullptr; ///< "agentxx.agent.resources"
    const AgentxxPluginModelIface*     model     = nullptr; ///< "agentxx.agent.model"
    const PluginxxCancelIface*         cancel    = nullptr; ///< "pluginxx.cancel"
    const AgentxxPluginGraphIface*     graph     = nullptr; ///< "agentxx.agent.graph"
    const PluginxxTasksIface*          tasks     = nullptr; ///< "pluginxx.tasks"
    /// "agentxx.agent.mount": 挂载查询 (诊断与自查)
    const AgentxxPluginMountIface* mount = nullptr;
    /// "agentxx.agent.runtime": 会话运行时 (挂自己的根 / 开会话通道 / 事件驱动)
    const AgentxxPluginRuntimeIface* runtime = nullptr;
    /// "<应用名>.broadcasts": 广播 (频道声明 / 发布 / 订阅 / 读当前值 / 结束)
    /// - 插件的通知面走它 (通知型功能埋点、事件、跨端数据都可用)
    const PluginxxBroadcastsIface* broadcasts = nullptr;
    /// "<应用名>.net": 网络 (HTTP / TCP / WS / UDP; 插件不直接建连接)
    const PluginxxNetIface* net = nullptr;


    /// 从主程序查询全部已知 agent 侧接口表 (host 为空时返回全 NULL 聚合)
    static AgentIfaces query(const PluginxxHost* host) {
        AgentIfaces f;
        if (!host || !host->vtable || !host->vtable->query_interface) {
            return f;
        }
        f.tools = queryInterface<AgentxxPluginToolsIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_TOOLS);
        f.permission = queryInterface<AgentxxPluginPermissionIface>(
            host,
            AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION
        );
        f.broadcasts
            = queryInterface<PluginxxBroadcastsIface>(host, PLUGINXX_IFACE_BROADCASTS);
        f.net = queryInterface<PluginxxNetIface>(host, PLUGINXX_IFACE_NET);
        f.capabilities
            = queryInterface<PluginxxCapabilitiesIface>(host, PLUGINXX_IFACE_CAPABILITIES);
        f.scheduler = queryInterface<PluginxxSchedulerIface>(host, PLUGINXX_IFACE_SCHEDULER);
        f.session
            = queryInterface<AgentxxPluginSessionIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_SESSION);
        f.context
            = queryInterface<AgentxxPluginContextIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT);
        f.plugins = queryInterface<PluginxxPluginsIface>(host, PLUGINXX_IFACE_PLUGINS);
        f.config  = queryInterface<PluginxxConfigIface>(host, PLUGINXX_IFACE_CONFIG);
        f.prompt
            = queryInterface<AgentxxPluginPromptIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_PROMPT);
        f.json      = queryInterface<PluginxxJsonIface>(host, PLUGINXX_IFACE_JSON);
        f.log       = queryInterface<PluginxxLogIface>(host, PLUGINXX_IFACE_LOG);
        f.resources = queryInterface<AgentxxPluginResourcesIface>(
            host,
            AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES
        );
        f.model  = queryInterface<AgentxxPluginModelIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_MODEL);
        f.cancel = queryInterface<PluginxxCancelIface>(host, PLUGINXX_IFACE_CANCEL);
        f.graph  = queryInterface<AgentxxPluginGraphIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_GRAPH);
        f.tasks = queryInterface<PluginxxTasksIface>(host, PLUGINXX_IFACE_TASKS);
        f.mount = queryInterface<AgentxxPluginMountIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_MOUNT);
        f.runtime
            = queryInterface<AgentxxPluginRuntimeIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME);
        return f;
    }
};

namespace detail {

/// 解析结果 JSON (非法 JSON 返回空; 结果 JSON 由本层自己产出, 解析失败只可能是被截断)
inline std::optional<utilxx_base::Json> parseNetJson(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        return utilxx_base::Json::parse(text);
    } catch (...) {
        return std::nullopt;
    }
}

/// 表入口结果码的名字 (写日志 / 拼错误串用)
///
/// 网络表的入口码取自 `pluginxx/api/result.h` 的**通用段**, 名字也用那份表
/// (`pluginxx::result::nameOf`), 本项目不再自己造一套。
inline std::string_view netCodeName(int32_t code) {
    if (const char* name = pluginxx::result::nameOf(code)) {
        return name;
    }
    // 不走结果码表的只剩主程序自定义码, 名字只有它自己知道
    return "unknown";
}

} // namespace detail

inline bool NetResult::ok() const {
    if (!accepted()) {
        return false;
    }
    const auto doc = detail::parseNetJson(json);
    return doc.has_value() && doc->is_object() && doc->value("ok", false);
}

inline std::string NetResult::str(std::string_view key) const {
    const auto doc = detail::parseNetJson(json);
    if (!doc.has_value() || !doc->is_object()) {
        return {};
    }
    return doc->value(key, "");
}

inline int64_t NetResult::status() const {
    const auto doc = detail::parseNetJson(json);
    if (!doc.has_value() || !doc->is_object()) {
        return 0;
    }
    return doc->value("status", int64_t{0});
}

inline std::string NetResult::error() const {
    if (!accepted()) {
        return std::string{detail::netCodeName(code)};
    }
    const auto doc = detail::parseNetJson(json);
    if (!doc.has_value() || !doc->is_object()) {
        return json.empty() ? std::string{"net call failed"} : json;
    }
    const std::string reason  = doc->value("error", "");
    const std::string message = doc->value("message", "");
    if (reason.empty()) {
        return message.empty() ? std::string{"net call failed"} : message;
    }
    return message.empty() ? reason : reason + ": " + message;
}

/// 只提交、不等结果的网络操作 (**动作照常执行, 结果丢弃**)
///
/// - 适用: 关连接 / 心跳 / 应答这类"发出去就行"的动作, 以及析构、回调里调用;
/// - 结果里的 `ok` 只表示**已提交** (`{"ok":true,"requestId":N}`), 业务结论不会回到这里;
///   要业务结果请在协程里 `co_await netCall(...)`; 不要在主程序 io 线程上自旋等待。
inline NetResult netSubmit(
    const PluginxxHost*     host,
    const PluginxxNetIface* net,
    std::string_view        op,
    std::string_view        argsJson = "{}"
) {
    NetResult result;
    if (!host || !net || !net->call || op.empty()) {
        return result;
    }
    const auto      opSv   = PluginStringView::from(op.data(), op.size());
    const auto      argsSv = PluginStringView::from(argsJson.data(), argsJson.size());
    int64_t         requestId = 0;
    PluginxxString  err{nullptr, 0};
    auto*           handle = net->call(host, &opSv, &argsSv, &requestId, nullptr, nullptr, &err);
    if (err.data) {
        result.json.assign(err.data, static_cast<size_t>(err.size));
        PluginString::free(host, &err);
    }
    result.code = handle != nullptr ? PLUGINXX_RESULT_OK : PLUGINXX_RESULT_ERR_STATE;
    if (handle != nullptr) {
        // 只有受理层面的事实: 请求号按十进制放进 JSON, 需要取消时取它
        result.json = std::string{"{\"ok\":true,\"requestId\":"} + std::to_string(requestId)
                      + "}";
    }
    return result;
}

/// 同 [netSubmit], 直接取当前的接口表
template<typename Ctx>
inline NetResult netSubmit(
    const Ctx&       ctx,
    std::string_view op,
    std::string_view argsJson = "{}"
) {
    return netSubmit(ctx.host, ctx.iface.net, op, argsJson);
}


/// HTTP 请求参数 (`http.fetch` / `http.download` 的入参; 只有 url 必填)
///
/// 字段含义与 `<应用名>.net` 的参数一一对应:
/// - [body] 原样发送; [json] 是 JSON 文本, 发送时自动补 `Content-Type: application/json`;
/// - [format] 只支持 `"markdown"` (HTML 转 Markdown) 与空串 (原样);
/// - [tlsVerify] 只对 `https://` 有效 (自签名服务端需要关掉)。
struct NetHttpOptions {
    std::string method          = "GET";
    std::string body;                ///< 请求体原文 (与 [json] / [bodyBase64] 三选一)
    std::string json;                ///< 请求体 JSON 文本 (自动补 Content-Type)
    std::string bodyBase64;          ///< 二进制请求体 (base64; 上传分片/图片用)
    std::string headers         = "{}"; ///< 请求头 JSON 对象文本
    std::string contentType;         ///< 非空且请求头里没有 Content-Type 时补上
    std::string format;              ///< "" 或 "markdown"
    std::string responseType;        ///< "" / "text" / "bytes" (bytes = 结果带 bodyBase64)
    int32_t     timeoutMs       = 0;  ///< <= 0 = 不设超时; 这是**整次请求**的预算
    int64_t     maxBytes        = 0;  ///< <= 0 = 不限正文大小 (超限时结果里 truncated)
    bool        followRedirects = true;
    int32_t     maxRedirects    = 3;
    bool        tlsVerify       = true;

    /// 拼成 `<应用名>.net` 的参数字符串
    std::string toArgsJson(std::string_view url) const;
};

/// 同 [NetHttpOptions::toArgsJson] 的字符串形态 (没有请求头 / 请求体时用它更省事)
inline std::string netHttpArgs(
    std::string_view             url,
    std::string_view             method = "GET",
    std::string_view             jsonBody = {},
    std::string_view             headersJson = {},
    std::string_view             format = {},
    int32_t                      timeoutMs = 0
) {
    NetHttpOptions options;
    options.method = std::string{method};
    options.json   = std::string{jsonBody};
    options.headers = headersJson.empty() ? std::string{"{}"} : std::string{headersJson};
    options.format = std::string{format};
    options.timeoutMs = timeoutMs;
    return options.toArgsJson(url);
}

inline std::string NetHttpOptions::toArgsJson(std::string_view url) const {
    utilxx_base::Json args = utilxx_base::Json::object();
    args["url"]            = std::string{url};
    if (!method.empty()) {
        args["method"] = method;
    }
    // 请求体三选一: 二进制 (base64) → 原文字节 → JSON 文本
    if (!bodyBase64.empty()) {
        args["bodyBase64"] = bodyBase64;
    } else if (!body.empty()) {
        args["body"] = body;
    } else if (!json.empty()) {
        try {
            args["json"] = utilxx_base::Json::parse(json);
        } catch (...) {
            args["body"] = json; // 不是合法 JSON: 按原文发, 至少不会丢请求体
        }
    }
    // 请求头: 传的是 JSON 对象文本; [contentType] 只在没有同名头时补上
    utilxx_base::Json headerObj = utilxx_base::Json::object();
    if (!headers.empty()) {
        try {
            auto parsed = utilxx_base::Json::parse(headers);
            if (parsed.is_object()) {
                headerObj = std::move(parsed);
            }
        } catch (...) {
        }
    }
    if (!contentType.empty() && headerObj.is_object() && !headerObj.contains("Content-Type")) {
        headerObj["Content-Type"] = contentType;
    }
    if (headerObj.is_object() && !headerObj.empty()) {
        args["headers"] = std::move(headerObj);
    }
    if (!format.empty()) {
        args["format"] = format;
    }
    if (!responseType.empty()) {
        args["responseType"] = responseType;
    }
    if (timeoutMs > 0) {
        args["timeoutMs"] = static_cast<int64_t>(timeoutMs);
    }
    if (maxBytes > 0) {
        args["maxBytes"] = maxBytes;
    }
    args["followRedirects"] = followRedirects;
    args["maxRedirects"]    = static_cast<int64_t>(maxRedirects < 0 ? 0 : maxRedirects);
    args["tlsVerify"]       = tlsVerify;
    return args.dump();
}

/** ---------- 给自带 io_context 的插件: asio 异步形态 ---------- */

/// 把一次网络调用做成 asio 异步操作
///
/// - 适用: 插件自己跑一个 `io_context` (例如长连接调度), 协程里用
///   `co_await netFetch(ex, host, net, url, options, asio::use_awaitable)`;
/// - 完成签名是 `void(NetResult)`; 完成回调投递到 handler 的 executor (`ex`) ——
///   不会从主程序回调栈里直接进入插件代码;
/// - 只需要 `ctx.bridge()` 拿不到时 (不是 kit 的协程) 就用这个, 其余情况用
///   [netCall] / [netFetch]。
template<typename CompletionToken>
auto asyncNetCall(
    asio::any_io_executor   ex,
    const PluginxxHost*     host,
    const PluginxxNetIface* net,
    std::string             op,
    std::string             argsJson,
    CompletionToken&&       token
) {
    return asio::async_initiate<CompletionToken, void(NetResult)>(
        [host, net](
            auto       handler,
            asio::any_io_executor ioEx,
            std::string opStr,
            std::string argsStr
        ) {
            using Handler = std::decay_t<decltype(handler)>;
            struct Op {
                Handler                 handler;
                asio::any_io_executor   ex;
            };

            auto self     = std::make_shared<Op>(Op{std::move(handler), std::move(ioEx)});
            auto complete = [](std::shared_ptr<Op> state, NetResult result) {
                asio::post(
                    state->ex,
                    [state = std::move(state), result = std::move(result)]() mutable {
                        auto handler = std::move(state->handler);
                        handler(std::move(result));
                    }
                );
            };

            if (!host || !net || !net->call) {
                NetResult result;
                result.code = PLUGINXX_RESULT_ERR_STATE;
                complete(std::move(self), std::move(result));
                return;
            }

            auto* holder = new std::shared_ptr<Op>(self);
            PluginxxOperatorCallback cb
                = [](void* ud, int32_t status, const PluginxxStringView* payload) {
                auto* hp = static_cast<std::shared_ptr<Op>*>(ud);
                auto  s  = *hp;
                delete hp;

                NetResult result;
                result.code = PLUGINXX_RESULT_OK;
                const std::string body
                    = (payload && payload->data && payload->size > 0)
                          ? std::string{payload->data, static_cast<size_t>(payload->size)}
                          : std::string{};
                if (status == PLUGINXX_OPERATOR_OK) {
                    result.json = body;
                } else if (status == PLUGINXX_OPERATOR_CANCELED) {
                    result.json = R"({"ok":false,"error":"canceled"})";
                } else {
                    result.json
                        = body.empty() ? std::string{R"({"ok":false,"error":"failed"})"} : body;
                }
                asio::post(
                    s->ex,
                    [s, result = std::move(result)]() mutable {
                        auto handler = std::move(s->handler);
                        handler(std::move(result));
                    }
                );
            };

            const auto     opSv   = PluginStringView::from(opStr.data(), opStr.size());
            const auto     argsSv = PluginStringView::from(argsStr.data(), argsStr.size());
            PluginxxString err{nullptr, 0};
            int64_t        requestId = 0;
            auto*          handle    = net->call(
                host,
                &opSv,
                &argsSv,
                &requestId,
                cb,
                holder,
                &err
            );
            if (handle != nullptr) {
                return; // 完成回调随后到 (恰好一次)
            }
            delete holder;
            NetResult result;
            result.code = PLUGINXX_RESULT_ERR_STATE;
            if (err.data) {
                result.json.assign(err.data, static_cast<size_t>(err.size));
                PluginString::free(host, &err);
            }
            complete(std::move(self), std::move(result));
        },
        token,
        std::move(ex),
        std::move(op),
        std::move(argsJson)
    );
}

/// 同 [asyncNetCall], 直接是一个 HTTP 请求 (`http.fetch`; 自带 io_context 的形态)
template<typename CompletionToken>
auto netFetch(
    asio::any_io_executor   ex,
    const PluginxxHost*     host,
    const PluginxxNetIface* net,
    std::string_view        url,
    const NetHttpOptions&   options,
    CompletionToken&&       token
) {
    return asyncNetCall(
        std::move(ex),
        host,
        net,
        std::string{pluginxx::net_op::httpFetch},
        options.toArgsJson(url),
        std::forward<CompletionToken>(token)
    );
}

/* ---------- WebSocket / HTTP 服务端的几个直白入口 (内部就是 netCall) ---------- */

/// 网络事件广播频道 (`plugin.<插件 id>.net`)
///
/// 频道由主程序声明并代发 (插件不用自己声明), 插件订阅得到 `kind` 事件:
/// `ws.open` / `ws.message` / `ws.close` / `http.request` / `http.listening` / …
inline std::string netEventTopic(std::string_view pluginId) {
    std::string topic = "plugin.";
    topic += pluginId;
    topic += ".net";
    return topic;
}

/// 建立 WebSocket 连接 (同 id 已存在时事件里报 `exists`)
///
/// - `url` 形如 `ws://127.0.0.1:9222/devtools/page/<id>`; 也可给
///   `netWsConnectHost(host, net, "127.0.0.1", 9222, "/path")` 的拆开写法;
/// - `subprotocols`: 逗号分隔 (主程序要的是数组);
/// - `timeoutMs`: 握手超时 (`<= 0` 用主程序默认);
/// - **提交即返回**: 结果的 `ok` 只表示已交给网络服务, 握手结果看 `ws.open` /
///   `ws.close` 事件, 之后 `ws.message` 收消息;
/// - 握手完成前 `ws.send` 会以 `not_connected` 失败, 发送请等 `ws.open`; 需要当场看到
///   这个失败就别用本函数, 用 `co_await netCall(ctx, "ws.connect", args)`。
inline NetResult netWsConnect(
    const PluginxxHost*     host,
    const PluginxxNetIface* net,
    std::string_view        url,
    std::string_view        id            = {},
    std::string_view        headersJson   = {},
    std::string_view        subprotocols  = {},
    int32_t                 timeoutMs     = 0,
    int64_t                 maxMessageBytes = 0
) {
    utilxx_base::Json args = utilxx_base::Json::object();
    args["url"]            = std::string{url};
    if (!id.empty()) {
        args["id"] = std::string{id};
    }
    if (!headersJson.empty()) {
        try {
            auto parsed = utilxx_base::Json::parse(headersJson);
            if (parsed.is_object()) {
                args["headers"] = std::move(parsed);
            }
        } catch (...) {
        }
    }
    if (!subprotocols.empty()) {
        utilxx_base::Json list = utilxx_base::Json::array();
        std::string_view  rest = subprotocols;
        while (!rest.empty()) {
            const auto comma = rest.find(',');
            auto       item  = rest.substr(0, comma);
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
                item.remove_prefix(1);
            }
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
                item.remove_suffix(1);
            }
            if (!item.empty()) {
                list.push_back(std::string{item});
            }
            if (comma == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(comma + 1);
        }
        if (!list.empty()) {
            args["subprotocols"] = std::move(list);
        }
    }
    if (timeoutMs > 0) {
        args["timeoutMs"] = static_cast<int64_t>(timeoutMs);
    }
    if (maxMessageBytes > 0) {
        args["maxMessageBytes"] = maxMessageBytes;
    }
    const std::string text = args.dump();
    return netSubmit(host, net, pluginxx::net_op::wsConnect, text);
}

/// 发 WebSocket 文本帧 (**提交即返回**, 不带回 `not_connected` 这类业务结果)
inline NetResult netWsSend(
    const PluginxxHost*     host,
    const PluginxxNetIface* net,
    std::string_view        id,
    std::string_view        text
) {
    utilxx_base::Json args = utilxx_base::Json::object();
    args["id"]             = std::string{id};
    args["text"]           = std::string{text};
    const std::string json = args.dump();
    return netSubmit(host, net, pluginxx::net_op::wsSend, json);
}

/// 关闭 WebSocket 连接 (**提交即返回**; `code` 缺省 1000)
inline NetResult netWsClose(
    const PluginxxHost*     host,
    const PluginxxNetIface* net,
    std::string_view        id,
    std::string_view        reason = {},
    int32_t                 code   = 1000
) {
    utilxx_base::Json args = utilxx_base::Json::object();
    args["id"]             = std::string{id};
    args["code"]           = static_cast<int64_t>(code);
    if (!reason.empty()) {
        args["reason"] = std::string{reason};
    }
    const std::string json = args.dump();
    return netSubmit(host, net, pluginxx::net_op::wsClose, json);
}

/// 回答一条 `http.request` 事件 (**提交即返回**)
inline NetResult netHttpRespond(
    const PluginxxHost*     host,
    const PluginxxNetIface* net,
    int64_t                 requestId,
    std::string_view        body,
    int32_t                 status         = 200,
    std::string_view        contentType    = "text/plain; charset=utf-8",
    std::string_view        extraHeaders   = {}
) {
    utilxx_base::Json args = utilxx_base::Json::object();
    args["requestId"]      = requestId;
    args["status"]         = static_cast<int64_t>(status);
    args["body"]           = std::string{body};
    args["contentType"]    = std::string{contentType};
    if (!extraHeaders.empty()) {
        try {
            auto parsed = utilxx_base::Json::parse(extraHeaders);
            if (parsed.is_object()) {
                args["headers"] = std::move(parsed);
            }
        } catch (...) {
        }
    }
    const std::string json = args.dump();
    return netSubmit(host, net, pluginxx::net_op::httpRespond, json);
}

/* ==================== 提示词描述解析结构 ==================== */

struct ToolPromptText {
    std::string                                     depict;
    std::map<std::string, std::string, std::less<>> args;
};

inline std::string
    toolPromptArgDesc(const ToolPromptText& p, std::string_view key, std::string_view fallback) {
    auto it = p.args.find(key);
    if (it != p.args.end() && !it->second.empty()) {
        return it->second;
    }
    return std::string{fallback};
}

/* ==================== 会话运行时的通道转接 (提前定义: 实例上下文要持有它) ====================
 *
 * 通道事件回调是 C ABI 函数指针 + `user_data`; 实例上下文里按通道 id 存一份转接对象,
 * 关通道时释放。定义放在此处是因为 [PluginBase] 需要它作为成员类型。
 */

/// 一条通道事件 (`kind` 取 `AGENTXX_PLUGIN_CHANNEL_EVT_*`)
///
/// 字符串是**借用视图**, 只在回调期间有效; 要留到回调之后必须自己拷成 `std::string`。
struct ChannelEvent {
    int32_t          kind        = AGENTXX_PLUGIN_CHANNEL_EVT_READY;
    int64_t          channelId   = 0;
    int64_t          interruptId = 0; ///< 仅中断类事件有效
    std::string_view text;
    std::string_view payloadJson;

    bool is(int32_t k) const noexcept {
        return kind == k;
    }
};

namespace detail {

/// 通道事件回调的转接 (堆上对象, 生命周期 = 通道)
///
/// 回调目标放在 `shared_ptr` 里: 插件可能在事件回调内部关通道 (关通道会释放本对象),
/// 转接处先拷一份 `shared_ptr` 再调用, 因此这种重入是安全的。
struct RuntimeChannelShim {
    std::shared_ptr<const std::function<void(const ChannelEvent&)>> fn;
};

} // namespace detail

/* ==================== 声明式模式构建器 ToolSchemaBuilder ==================== */

class ToolSchemaBuilder {
public:

    explicit ToolSchemaBuilder(ToolPromptText prompt = {}) :
        prompt_(std::move(prompt)) {}

    ToolSchemaBuilder& string(
        std::string_view           name,
        std::string_view           desc,
        bool                       required = false,
        std::optional<std::string> defVal   = std::nullopt
    ) {
        utilxx_base::Json prop;
        prop["type"]        = "string";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder& integer(
        std::string_view       name,
        std::string_view       desc,
        bool                   required = false,
        std::optional<int64_t> defVal   = std::nullopt
    ) {
        utilxx_base::Json prop;
        prop["type"]        = "integer";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder& number(
        std::string_view      name,
        std::string_view      desc,
        bool                  required = false,
        std::optional<double> defVal   = std::nullopt
    ) {
        utilxx_base::Json prop;
        prop["type"]        = "number";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder& boolean(
        std::string_view    name,
        std::string_view    desc,
        bool                required = false,
        std::optional<bool> defVal   = std::nullopt
    ) {
        utilxx_base::Json prop;
        prop["type"]        = "boolean";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    /// 对象参数: 值是一个对象 (如 HTTP 请求头 `{"Name": "value"}`)
    /// - 成员类型用 `valueType` 声明 (默认字符串; 传空则不加 `additionalProperties`)
    ToolSchemaBuilder& object(
        std::string_view name,
        std::string_view desc,
        bool             required  = false,
        std::string_view valueType = "string"
    ) {
        utilxx_base::Json prop;
        prop["type"]        = "object";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        if (!valueType.empty()) {
            prop["additionalProperties"] = utilxx_base::Json{
                {"type", std::string(valueType)}
            };
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder&
        stringArray(std::string_view name, std::string_view desc, bool required = false) {
        return array(name, desc, "string", required);
    }

    /// 对象数组参数: 数组元素是对象 (`items` 为给定的对象模式)
    /// - 用于 `[{...}, {...}]` 这类参数 (如 edit 的 `edits` 列表), 元素模式里
    ///   自行声明 `properties` / `required`
    /// - `itemSchema` 为 `items` 的完整内容 (如
    ///   `Json{{"type","object"},{"properties", ...},{"required", ...}}`)
    ToolSchemaBuilder& objectArray(
        std::string_view         name,
        std::string_view         desc,
        const utilxx_base::Json& itemSchema,
        bool                     required = false
    ) {
        utilxx_base::Json prop;
        prop["type"]                   = "array";
        prop["description"]            = toolPromptArgDesc(prompt_, name, desc);
        prop["items"]                  = itemSchema;
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder& array(
        std::string_view name,
        std::string_view desc,
        std::string_view itemType = "string",
        bool             required = false
    ) {
        utilxx_base::Json prop;
        prop["type"]        = "array";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        prop["items"]       = utilxx_base::Json{
                  {"type", std::string(itemType)}
        };
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder& enumString(
        std::string_view           name,
        std::string_view           desc,
        std::vector<std::string>   options,
        bool                       required = false,
        std::optional<std::string> defVal   = std::nullopt
    ) {
        utilxx_base::Json prop;
        prop["type"]        = "string";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        prop["enum"]        = options;
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    std::string build() const {
        utilxx_base::Json schema;
        schema["type"]       = "object";
        schema["properties"] = properties_;
        if (!required_.empty()) {
            schema["required"] = required_;
        } else {
            schema["required"] = utilxx_base::Json::array();
        }
        return schema.dump();
    }

private:

    ToolPromptText           prompt_;
    utilxx_base::Json        properties_ = utilxx_base::Json::object();
    std::vector<std::string> required_;
};

/* ==================== 提示词描述 / 模式构建器的协程形态 ==================== */

/// 解析 `get_tool_prompt` 的结果正文 (`{"depict":..., "args":{...}}`)
///
/// - 空正文、非法 JSON、非对象都返回空描述 (工具照常注册, 只是描述文本用调用方给的默认值)
inline ToolPromptText parseToolPromptJson(std::string_view json) {
    ToolPromptText res;
    if (json.empty()) {
        return res;
    }
    try {
        auto j = utilxx_base::Json::parse(std::string{json});
        if (!j.is_object()) {
            return res;
        }
        if (j.contains("depict") && j["depict"].is_string()) {
            res.depict = j["depict"].get<std::string>();
        }
        if (j.contains("args") && j["args"].is_object()) {
            for (const auto& [k, v] : j["args"].items()) {
                if (v.is_string()) {
                    utilxx_base::insertOrAssignHeterogeneous(res.args, k, v.get<std::string>());
                }
            }
        }
    } catch (...) {
    }
    return res;
}


/* ==================== 插件实例上下文基类 (agentxx 业务扩展) ==================== */

/// agentxx 插件实例上下文基类
///
/// 通用能力 (host / iface / log / config / language / strdup / jsonEscape /
/// cancelRegistry / 协程驱动桥 / 后台协作任务) 全部继承自 [pluginxx::PluginBaseT],
/// 以 agent 侧接口表聚合 [AgentIfaces] 作为其模板实参;
/// 本类只补齐 agent 业务 helper: 会话工作目录 (workDir)、会话取消查询
/// (sessionCanceled)、会话共享存储 (addShareStore), 并在 [onHostReady] 中挂钩业务事件。
/// 工具提示词与模式构建用文件里的 `toolPromptOf` / `schemaOf` 协程入口。
class PluginBase : public pluginxx::PluginBaseT<AgentIfaces> {
public:

    // 配置 / 参数 / 配置路径 / 语言类协程入口由基类
    // [pluginxx::PluginBaseT] 提供 (它已统一改成 asio 协程形态)。

    /// 写入会话共享存储 (**协程形态**; `co_await` 后取 `.text()` 读 id)
    ///
    /// 第一个参数是插件实例上下文 (带驱动桥), 返回 asio 协程操作。
    template<typename Ctx>
    asio::awaitable<pluginxx::TableOpResult> addShareStore(
        Ctx&               ctx,
        const ScopeRef&    agent,
        PluginxxStringView tid,
        std::string_view   content
    ) const {
        auto ref  = agent;
        auto key  = PluginStringView::from(tid.data, tid.size);
        auto text = std::string{content};
        co_return co_await asyncTableOp(
            ctx,
            co_await asio::this_coro::executor,
            [this, ref, key, text](auto cb, auto ud, auto err) {
                auto r  = ref.raw();
                auto sv = PluginStringView::from(text.data(), text.size());
                return iface.session->add_share_store(host, &r, &key, &sv, cb, ud, err);
            }
        );
    }

    /// 会话生效的工作目录 (**协程形态**)
    template<typename Ctx>
    asio::awaitable<pluginxx::TableOpResult>
        workDir(Ctx& ctx, const ScopeRef& agent, std::string_view key = {}) const {
        auto ref = agent;
        auto k   = std::string{key};
        co_return co_await asyncTableOp(
            ctx,
            co_await asio::this_coro::executor,
            [this, ref, k](auto cb, auto ud, auto err) {
                auto r  = ref.raw();
                auto sv = PluginStringView::from(k.data(), k.size());
                return iface.session->get_session_work_dir(host, &r, &sv, cb, ud, err);
            }
        );
    }

    /// 读取会话 LLM 上下文 (**协程形态**: `co_await` 后 `.text()` 是 JSON 数组文本)
    template<typename Ctx>
    asio::awaitable<pluginxx::TableOpResult> getSessionMessages(
        const Ctx&       ctx,
        const ScopeRef&  agent,
        std::string_view tid
    ) const {
        auto ref   = agent;
        auto tidSv = PluginStringView::from(tid.data(), tid.size());
        co_return co_await asyncTableOp(
            ctx,
            co_await asio::this_coro::executor,
            [this, ref, tidSv](auto cb, auto ud, auto err) {
                auto r = ref.raw();
                if (!host || !iface.context || !iface.context->get_messages) {
                    return static_cast<PluginxxOperatorHandle*>(nullptr);
                }
                return iface.context->get_messages(host, &r, &tidSv, cb, ud, err);
            }
        );
    }

    /// 会话当前轮次是否已取消 (经主程序 agentxx.agent.session 接口表)
    /// - 先查本地登记: 主程序下发取消时已写入 [cancelRegistry], 命中即可返回,
    ///   不做跨线程查询;
    /// - 只是主动查询, 权威的取消通知始终是取消回调;
    /// - `scope` 是本挂载的作用域引用 (会话属于某个作用域)
    bool sessionCanceled(const PluginxxScopeRef* scope, PluginxxStringView key) const {
        if (!key.data || key.size == 0) {
            return false;
        }
        std::string_view keySv{key.data, static_cast<size_t>(key.size)};
        if (cancelRegistry.isCanceled(keySv)) {
            return true;
        }
        if (!host || !iface.session || !iface.session->is_canceled) {
            return false;
        }
        return iface.session->is_canceled(host, scope, &key) != 0;
    }

    bool sessionCanceled(const PluginxxScopeRef* scope, std::string_view key) const {
        return sessionCanceled(scope, PluginStringView::from(key.data(), key.size()));
    }

    /// 会话 LLM 上下文条数 (**协程形态**: `co_await` 后 `.integer(-1)`)
    template<typename Ctx>
    asio::awaitable<pluginxx::TableOpResult> sessionMessagesCount(
        const Ctx&       ctx,
        const ScopeRef&  agent,
        std::string_view tid
    ) const {
        auto ref   = agent;
        auto tidSv = PluginStringView::from(tid.data(), tid.size());
        co_return co_await asyncTableOp(
            ctx,
            co_await asio::this_coro::executor,
            [this, ref, tidSv](auto cb, auto ud, auto err) {
                auto r = ref.raw();
                if (!host || !iface.context || !iface.context->messages_count) {
                    return static_cast<PluginxxOperatorHandle*>(nullptr);
                }
                return iface.context->messages_count(host, &r, &tidSv, cb, ud, err);
            }
        );
    }

public:

    /// 轮次开始广播处理器: 清掉该会话的取消标记
    ///
    /// create 事务在 `onHostReady` 里自动订阅本处理器; 公开出来是为了让测试能直接
    /// 订阅同一个处理器 (不起 create 事务也能验行为)。
    static void PLUGINXX_CALL onRoundStartEvent(const PluginxxStringView* ev, void* ud) {
        auto* self = static_cast<PluginBase*>(ud);
        if (!self || !ev || !ev->data) {
            return;
        }
        try {
            auto j = utilxx_base::Json::parse(
                std::string_view{ev->data, static_cast<size_t>(ev->size)}
            );
            std::string sid = j.value("sessionId", "");
            if (!sid.empty()) {
                self->cancelRegistry.clearCanceled(sid);
            }
        } catch (...) {
        }
    }

    /// 业务初始化 (**协程**; create 事务在 `attachHost` 之后 await 它): 订阅会话轮次
    /// 开始广播, 自动为当前会话重置实例级取消登记
    /// (主程序下发 cancel 时已通过 execute_cancel 写入 CancelRegistry)
    asio::awaitable<void> onHostReady() override {
        if (!iface.broadcasts || !iface.broadcasts->subscribe_broadcast) {
            co_return;
        }
        // 订阅主程序总线 (基类不知道 agent 引用, 会话事件一并到达);
        // 协程形态拿到句柄: 插件执行线程上不能同步等表。
        // 这里直接走 framework 的表调用入口 (本类不是模板, 定义点之前拿不到
        // 文件后面的 `subscribePluginBroadcastAsync`)。
        auto result = co_await pluginxx::asyncTableOp(
            *this,
            co_await asio::this_coro::executor,
            [this](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
                -> PluginxxOperatorHandle* {
            static constexpr std::string_view kChannel = "agentxx.agent.round.start";
            auto idSv = PluginStringView::from(kChannel.data(), kChannel.size());
            return iface.broadcasts->subscribe_broadcast(
                host,
                nullptr,
                &idSv,
                &PluginBase::onRoundStartEvent,
                this,
                nullptr,
                cb,
                ud,
                err
            );
        }
        );
        roundStartSub_ = subscriptionOf(result);
        if (!roundStartSub_) {
            // 订阅失败时取消标记只能靠每个工具根开始时自清 (见 detail::registerToolWith*Body),
            // 这里明确提示一次, 避免以后排查只能靠现象
            log.warn(
                "plugin kit: subscribe `agentxx.agent.round.start` failed; session cancel "
                "marks will only be cleared when the next tool root starts"
            );
        }
        co_return;
    }

private:

    /// 会话轮次开始事件的订阅句柄 (实例销毁时由主程序随实例一并清理)
    PluginxxSubscription* roundStartSub_ = nullptr;

public:

    /// 会话运行时的通道回调转接对象 (键 = 通道 id; `openChannel` 写入, `closeChannel` 擦除)
    /// - 只在主程序 io 线程访问 (开/关通道本身就只在那里执行), 无需加锁
    std::map<int64_t, std::unique_ptr<detail::RuntimeChannelShim>> runtimeChannelShims;
};

/* ==================== per-agent 引用与状态表 ==================== */

/// per-agent 引用的值类型: 挂载时下发, 存进 per-agent 状态, 之后任何线程直接用
///
/// - `id()`: agentId; `seq()`: 挂载序号 (同一 id 重新挂载会变)
/// - `raw()`: 传给接口表的借用视图 (指向本对象内部, 不要长期保存)
class AgentRef : public ScopeRef {
public:

    AgentRef() = default;

    /// 从挂载参数构造 (自动拷 id 并带上序号)
    explicit AgentRef(const AgentxxPluginAgentMountSpec& spec) :
        ScopeRef(
            spec.agent_id.data && spec.agent_id.size > 0
                ? std::string(spec.agent_id.data, static_cast<size_t>(spec.agent_id.size))
                : std::string{},
            spec.mount_seq
        ) {}
};

/// per-agent 状态表 (放在 plugin_ctx 里; 只在主程序 io 线程访问, 无需加锁)
///
/// `T` 需要有一个可赋值的 `AgentRef ref` 成员 (或自己持有 `AgentRef`)。
/// 键 = agentId, 但命中需要 **id 与 seq 都匹配** —— 旧挂载留下的引用会被拒绝。
template<typename T>
class MountStore {
public:

    /// 建立一份状态 (id 已存在时返回已有项, 不改 ref)
    T* create(const AgentRef& ref) {
        auto [pos, inserted] = items_.try_emplace(std::string{ref.id()});
        if (inserted) {
            pos->second = std::make_unique<T>();
            pos->second->ref = ref;
        }
        return pos->second.get();
    }

    /// 按引用查找 (id 与 seq 都要匹配; 否则返回 nullptr)
    T* find(const AgentRef& ref) const {
        auto it = items_.find(std::string{ref.id()});
        if (it == items_.end() || !it->second || it->second->ref.seq() != ref.seq()) {
            return nullptr;
        }
        return it->second.get();
    }

    T* findById(std::string_view agentId) const {
        auto it = items_.find(std::string{agentId});
        return it == items_.end() ? nullptr : it->second.get();
    }

    void erase(const AgentRef& ref) {
        auto it = items_.find(std::string{ref.id()});
        if (it != items_.end() && it->second && ref.seq() != 0 && it->second->ref.seq() != ref.seq()) {
            return; // 序号不匹配: 不是本浸挂载的
        }
        items_.erase(std::string{ref.id()});
    }

    size_t size() const noexcept {
        return items_.size();
    }

    bool empty() const noexcept {
        return items_.empty();
    }

    template<typename Fn>
    void forEach(Fn&& fn) const {
        for (const auto& [id, item] : items_) {
            if (item) {
                fn(*item);
            }
        }
    }

private:

    std::map<std::string, std::unique_ptr<T>, std::less<>> items_;
};

/* ==================== 会话运行时 (agentxx.agent.runtime) ====================
 *
 * 三层的便捷封装 (C ABI 见 plugin_api.h 的同名表):
 * - **根 (root)**: 挂到容器上的根 agent, 一份 `AgentConfig` (提示词/工具白名单/插件挂载);
 *   只能在 `start` 这类进程级时机挂, 在 `mount` 回调里挂会被主程序拒绝;
 * - **通道 (channel)**: 一个 `sessionId` 的驱动壳 (输入 / 事件 / 中断 / 查询);
 * - **触发器 (trigger) / 定时器 (timer)**: 把事件或时间转成通道输入。
 *
 * 全部入口是**异步操作契约**的表成员, 这里的便捷层包成"调用即得结果": 调用方本来就在
 * 主程序 IO 线程时主程序内联完成, 不产生等待。建根/开通道/关通道都只是"发起",
 * 结果经通道事件体现 (`READY` / `ERROR` / `CLOSED`); 句柄 id 直接作为返回值。
 * 主程序没有这张表时 (老版本) 全部返回失败值, 不抛异常。
 */

/// 挂载根的参数
struct MountRootOptions {
    /// 展示名 (也用于生成根 id: "qq" → "ext:qq:1")
    std::string name;
    /// `AgentConfig` 覆盖 (字段见插件设计文档"根配置"一节)
    /// - 建议至少给 `prompt.systemPrompt`; 空 = 只用派生默认值 (coding 人设)
    utilxx_base::Json config = utilxx_base::Json::object();
    /// 该根的插件挂载策略 (与 yaml `plugin.mount` 同构):
    /// `{"mode":"none|all|listed","list":[...],"args":{...}}`
    /// - 空 = `none`: 该根只有内置工具 (share_store / subagent / git_worktree)
    std::string mountPlugins;
    /// 创建者插件是否也挂到该根 (默认 false = 切断递归)
    bool includeCreator = false;
};

/// 打开会话通道的参数
struct ChannelOptions {
    std::string rootId;    ///< `mountRoot` 返回的根 id (必填)
    std::string sessionId; ///< 空 = 由主程序生成
    std::string title;     ///< 会话标题 (可空)
    std::string model;     ///< 初始模型 (空 = 用根的默认模型)
    /// 中断等待上限 (0 = 不限; 到点按取消应答)
    std::chrono::milliseconds interruptTimeout{0};
    /// 输入队列上限 (0 = 默认)
    int32_t maxQueue = 0;
};

/// 一条通道事件: 见本头开头的 [ChannelEvent] (因实例上下文要持有转接对象而前移)

/// 触发器参数
struct TriggerOptions {
    /// 主题 (必填全名: `agentxx.agent.*` / `agentxx.client.*` / `plugin.<插件名>.*`)
    /// - 框架不补前缀; 不是三族合法全名时被拒绝
    std::string topic;
    int64_t     channelId = 0;
    /// `AGENTXX_PLUGIN_TRIGGER_POLICY_*` (默认 QUEUE)
    int32_t policy = AGENTXX_PLUGIN_TRIGGER_POLICY_QUEUE;
    /// COALESCE 的合并窗口 (毫秒; 0 = 默认 300)
    int32_t debounceMs = 0;
    /// 合并后待投递上限 (0 = 默认 20)
    int32_t maxQueue = 0;
    /// 投递模式 (空 = next-turn; 另见 next-step / inject / collect)
    std::string delivery;
};

/// 定时器参数
struct RuntimeTimerOptions {
    int64_t     channelId = 0;
    /// 投递的输入 JSON (形状同 `send_input`)
    std::string inputJson;
    /// 首次触发延迟 (毫秒; 相对时间)
    int64_t firstDelayMs = 0;
    /// 周期间隔 (毫秒; 0 = 一次性)
    int64_t intervalMs = 0;
    /// `AGENTXX_PLUGIN_TRIGGER_POLICY_*` (默认 DROP_IF_BUSY)
    int32_t policy = AGENTXX_PLUGIN_TRIGGER_POLICY_DROP_IF_BUSY;
};

/// 主程序是否提供会话运行时 (老主程序没有这张表)
template<typename Ctx>
inline bool hasRuntime(const Ctx& ctx) {
    return ctx.iface.runtime != nullptr && ctx.iface.runtime->mount_root != nullptr;
}

namespace detail {

/// C ABI 回调 → `std::function<void(const ChannelEvent&)>`
/// - 先拷一份转接对象持有的 `shared_ptr`: 回调里可能关通道 (那会释放转接对象本身)
inline void PLUGINXX_CALL
    runtimeChannelEventThunk(void* user_data, const AgentxxPluginChannelEvent* event) {
    auto* shim = static_cast<RuntimeChannelShim*>(user_data);
    if (!shim || !event || !shim->fn) {
        return;
    }
    auto fn = shim->fn; // 拷一份: 回调里可能关通道 (那会释放 shim 本身)
    ChannelEvent ev;
    ev.kind        = event->kind;
    ev.channelId   = event->channel_id;
    ev.interruptId = event->interrupt_id;
    ev.text        = event->text.data
                         ? std::string_view{event->text.data, static_cast<size_t>(event->text.size)}
                         : std::string_view{};
    ev.payloadJson = event->payload_json.data
                         ? std::string_view{
                               event->payload_json.data,
                               static_cast<size_t>(event->payload_json.size)
                         }
                         : std::string_view{};
    try {
        (*fn)(ev);
    } catch (...) {
        // 插件回调抛出的异常不得跨边界: 主程序那边只记日志
    }
}

} // namespace detail

/// 挂载一个根 (**asio 形态**: `co_await` 后 `.ok()` 判成败, `.text()` = 根 id)
///
/// 与 [mountRoot] 同语义, 只是提交与等待都走 asio 协程。
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    mountRootAsync(Ctx& ctx, const MountRootOptions& opt) {
    if (!hasRuntime(ctx)) {
        pluginxx::TableOpResult r;
        r.startError = "plugin runtime iface unavailable";
        co_return r;
    }
    auto       name       = std::string{opt.name};
    auto       mountList  = std::string{opt.mountPlugins};
    auto       configJson = opt.config.is_null() ? std::string{"{}"} : opt.config.dump();
    const bool include    = opt.includeCreator;
    co_return co_await pluginxx::asyncTableOp(
        ctx,
        co_await asio::this_coro::executor,
        [&ctx,
         name       = std::move(name),
         mountList  = std::move(mountList),
         configJson = std::move(configJson),
         include](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            AgentxxPluginMountRootSpec spec{};
            spec.struct_size     = sizeof(AgentxxPluginMountRootSpec);
            spec.name            = PluginStringView::from(name);
            spec.config_json     = PluginStringView::from(configJson);
            spec.mount_plugins   = PluginStringView::from(mountList);
            spec.include_creator = include ? 1 : 0;
            return ctx.iface.runtime->mount_root(ctx.host, &spec, cb, ud, err);
        }
    );
}

namespace detail {

/// 未受理的运行时表调用结果 (运行时会话的 asio 形态用)
inline pluginxx::TableOpResult runtimeTableOpUnsupported() {
    pluginxx::TableOpResult r;
    r.startError = "plugin runtime iface unavailable";
    return r;
}

/// asio 形态的运行时表调用 (取当前协程执行器后提交)
///
/// 与文件后面 `detail::tableOpAwaitAsync` 同一套做法; 这里的副本是因为本段的
/// 位置更靠前 (模板体里的限定名在定义点就要能看到)。
template<typename Ctx, typename StartFn>
inline asio::awaitable<pluginxx::TableOpResult>
    runtimeTableOpAwaitAsync(Ctx& ctx, StartFn start) {
    co_return co_await pluginxx::detail::tableOpAwaitAsyncSlot(ctx, std::move(start));
}

} // namespace detail


/* ---- 运行时会话通道的 asio 形态 ----
 *
 * 与上面的 ticket 形态一一对应: `co_await` 直接拿 `pluginxx::TableOpResult`。
 * 给工具体、`start` 事务这类 `asio::awaitable` 协程用。
 */

/// 卸载一个根 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    unmountRootAsync(Ctx& ctx, std::string_view rootId) {
    if (!hasRuntime(ctx) || !ctx.iface.runtime->unmount_root || rootId.empty()) {
        co_return detail::runtimeTableOpUnsupported();
    }
    auto id = std::string{rootId};
    co_return co_await detail::runtimeTableOpAwaitAsync(
        ctx,
        [&ctx, id = std::move(id)](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            auto idSv = PluginStringView::from(id);
            return ctx.iface.runtime->unmount_root(ctx.host, &idSv, cb, ud, err);
        }
    );
}

/// 投递一段文本 (asio 形态; `delivery` 空 = next-turn)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> sendTextAsync(
    Ctx&             ctx,
    int64_t          channelId,
    std::string_view text,
    std::string_view delivery = {}
) {
    utilxx_base::Json input = utilxx_base::Json::object();
    input["text"]           = std::string{text};
    if (!delivery.empty()) {
        input["delivery"] = std::string{delivery};
    }
    co_return co_await sendInputAsync(ctx, channelId, input.dump());
}

/// 切换通道所用模型 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    selectChannelModelAsync(Ctx& ctx, int64_t channelId, std::string_view model) {
    if (!hasRuntime(ctx) || !ctx.iface.runtime->select_model || channelId <= 0) {
        co_return detail::runtimeTableOpUnsupported();
    }
    auto name = std::string{model};
    co_return co_await detail::runtimeTableOpAwaitAsync(
        ctx,
        [&ctx, channelId, name = std::move(name)](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            auto nameSv = PluginStringView::from(name);
            return ctx.iface.runtime->select_model(ctx.host, channelId, &nameSv, cb, ud, err);
        }
    );
}

/// 通道会话的 LLM 上下文 (asio 形态; 正文是 JSON 数组文本)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    channelContextMessagesAsync(Ctx& ctx, int64_t channelId) {
    if (!hasRuntime(ctx) || !ctx.iface.runtime->get_context_messages || channelId <= 0) {
        co_return detail::runtimeTableOpUnsupported();
    }
    co_return co_await detail::runtimeTableOpAwaitAsync(
        ctx,
        [&ctx, channelId](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            return ctx.iface.runtime->get_context_messages(ctx.host, channelId, cb, ud, err);
        }
    );
}

/// 登记触发器 (asio 形态; `.integer()` 是触发器 id, 失败 0)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    addTriggerAsync(Ctx& ctx, const TriggerOptions& opt) {
    if (!hasRuntime(ctx) || !ctx.iface.runtime->register_trigger || opt.topic.empty()
        || opt.channelId <= 0) {
        co_return detail::runtimeTableOpUnsupported();
    }
    auto topic    = std::string{opt.topic};
    auto delivery = std::string{opt.delivery};
    co_return co_await detail::runtimeTableOpAwaitAsync(
        ctx,
        [&ctx,
         topic    = std::move(topic),
         delivery = std::move(delivery),
         policy   = opt.policy,
         debounce = opt.debounceMs,
         maxQueue = opt.maxQueue,
         channel  = opt.channelId](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            AgentxxPluginTriggerSpec spec{};
            spec.struct_size = sizeof(AgentxxPluginTriggerSpec);
            spec.policy      = policy;
            spec.debounce_ms = debounce;
            spec.max_queue   = maxQueue;
            spec.topic       = PluginStringView::from(topic);
            spec.channel_id  = channel;
            spec.delivery    = PluginStringView::from(delivery);
            return ctx.iface.runtime->register_trigger(ctx.host, &spec, cb, ud, err);
        }
    );
}

/// 注销触发器 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    removeTriggerAsync(Ctx& ctx, int64_t triggerId) {
    if (!hasRuntime(ctx) || !ctx.iface.runtime->unregister_trigger || triggerId <= 0) {
        co_return detail::runtimeTableOpUnsupported();
    }
    co_return co_await detail::runtimeTableOpAwaitAsync(
        ctx,
        [&ctx, triggerId](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            return ctx.iface.runtime->unregister_trigger(ctx.host, triggerId, cb, ud, err);
        }
    );
}

/// 发一条事件 (asio 形态; 写侧只能落在 `plugin.<名字>.*` 下)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> emitEventAsync(
    Ctx&             ctx,
    std::string_view topic,
    std::string_view payloadJson = {}
) {
    if (!hasRuntime(ctx) || !ctx.iface.runtime->emit_event || topic.empty()) {
        co_return detail::runtimeTableOpUnsupported();
    }
    auto topicText   = std::string{topic};
    auto payloadText = std::string{payloadJson};
    co_return co_await detail::runtimeTableOpAwaitAsync(
        ctx,
        [&ctx, topicText = std::move(topicText), payloadText = std::move(payloadText)](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            auto topicSv = PluginStringView::from(topicText);
            auto paySv   = PluginStringView::from(payloadText);
            return ctx.iface.runtime->emit_event(ctx.host, &topicSv, &paySv, cb, ud, err);
        }
    );
}

/// 登记定时器 (asio 形态; `.integer()` 是定时器 id, 失败 0)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    addRuntimeTimerAsync(Ctx& ctx, const RuntimeTimerOptions& opt) {
    if (!hasRuntime(ctx) || !ctx.iface.runtime->register_timer || opt.channelId <= 0
        || opt.inputJson.empty() || opt.firstDelayMs < 0 || opt.intervalMs < 0) {
        co_return detail::runtimeTableOpUnsupported();
    }
    auto input = std::string{opt.inputJson};
    co_return co_await detail::runtimeTableOpAwaitAsync(
        ctx,
        [&ctx,
         input    = std::move(input),
         policy   = opt.policy,
         channel  = opt.channelId,
         first    = opt.firstDelayMs,
         interval = opt.intervalMs](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            AgentxxPluginTimerSpec spec{};
            spec.struct_size    = sizeof(AgentxxPluginTimerSpec);
            spec.policy         = policy;
            spec.channel_id     = channel;
            spec.first_delay_ms = first;
            spec.interval_ms    = interval;
            spec.input_json     = PluginStringView::from(input);
            return ctx.iface.runtime->register_timer(ctx.host, &spec, cb, ud, err);
        }
    );
}

/// 注销定时器 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    removeRuntimeTimerAsync(Ctx& ctx, int64_t timerId) {
    if (!hasRuntime(ctx) || !ctx.iface.runtime->unregister_timer || timerId <= 0) {
        co_return detail::runtimeTableOpUnsupported();
    }
    co_return co_await detail::runtimeTableOpAwaitAsync(
        ctx,
        [&ctx, timerId](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            return ctx.iface.runtime->unregister_timer(ctx.host, timerId, cb, ud, err);
        }
    );
}




/* ==================== 后台任务 (async work) ====================
 *
 * 提交即入队; 结果发到固定事件 `agentxx.agent.async.done` (值里用 `asyncId` 区分),
 * 想当场拿结果就让模型 `agentxx_async_await {id}`。
 * 插件禁用/卸载时主程序按 `plugin:<实例名>` 把未完成的任务取消 (终态照常记录)。
 */

/// 提交参数 (字段语义与 `AgentxxPluginAsyncWorkSpec` 一致; 全部可选)
struct AsyncWorkOptions {
    std::string name = "plugin-task";
    std::string systemPrompt;
    /// 任务正文 (agent 任务必需: 空且没有上下文时提交会被拒)
    std::string message;
    /// 空 = 创建者的 subagent 模型
    std::string model;
    /// 创建者会话 (结果事件与 HIL 的去处)
    std::string sessionId;
    /// 排队与额度分组; 空 = 创建者 agentId
    std::string queueKey;
    std::string workDir;
    std::string language;
    /// 结果额外投递到创建者会话: 空 = 不投递; `next-step` / `inject` / `next-turn`
    std::string notifyDelivery;
    /// 工具策略: 空 = 默认; `[]` = 无工具; `["*"]` = 继承创建者; 否则名单
    utilxx_base::Json tools;
    /// 插件挂载 (与 yaml `plugin.mount` 同构); 空 = 不挂
    /// - 键: `mode` (`all` / `listed` / `none`)、`list`、`args`、`exclude`
    /// - `exclude` 是剔除名单 (`All` / `Listed` 都生效): 插件提交任务时用它把自己排除在外
    utilxx_base::Json plugins;
    utilxx_base::Json mcpServers;
    utilxx_base::Json skills;
    utilxx_base::Json memoryFiles;
    utilxx_base::Json ragDocs;
    utilxx_base::Json permissionAllow;
    utilxx_base::Json permissionDeny;
    /// 中断策略: -1 = 跟随配置, 0 = bubble, 1 = deny, 2 = allow
    int32_t interruptPolicy = -1;
    /// 会话落盘: -1 = 跟随配置, 0 = 否, 1 = 是
    int32_t sessionStore     = -1;
    int64_t interruptTimeoutMs = 0;
    /// 运行时限 (毫秒; 0 = 不限制): 计时从起跑开始, 排队与延时不算
    /// - `kind=agent`: 到点置取消令牌, 引擎在检查点停下
    /// - `kind=tool`: 到点只结束等待, 工具本体可能继续跑 (结果丢弃)
    int64_t runTimeoutMs       = 0;
    /// 起跑延时 (毫秒; 0 = 不延时): 到点之前任务一直是 `queued`, 不占名额
    /// - 提交后没有改延时的成员; 要改就取消后重新提交
    int64_t delayMs            = 0;
    /// 排队时限 (毫秒; 0 = 不限制): 计时起点是**延时结束那一刻** (无延时 = 提交时)
    /// - 到点还没拿到名额: 任务出队并按 `failed` 收尾 (`error` 写 queue timeout)
    int64_t queueTimeoutMs     = 0;
    /// 非空 = 提交一个 `kind=tool` 的后台任务: 把这一次工具调用放到后台跑
    /// - 工具在你挂的这个根的工具表里查 (找不到时任务按失败收尾)
    /// - `message` / 派生配置 / 工具策略都不参与 (不会另起 agent)
    /// - 需要本端 SDK 与主程序都懂追加字段, 否则返回 `ERR_ARG`
    std::string       toolName;
    utilxx_base::Json toolArgs = utilxx_base::Json::object();
    /// 任务事件回调 (可空); **在容器 io 线程调用, 必须快速返回**
    AgentxxPluginAsyncWorkEventFn onEvent  = nullptr;
    void*                          userData = nullptr;
};

namespace detail {

/// 后台任务提交的全部入参 (协程形态要按值带到提交那一刻)
struct AsyncWorkSpecKeep {
    std::string name;
    std::string systemPrompt;
    std::string message;
    std::string model;
    std::string sessionId;
    std::string queueKey;
    std::string workDir;
    std::string language;
    std::string notifyDelivery;
    std::string tools;
    std::string plugins;
    std::string mcpServers;
    std::string skills;
    std::string memoryFiles;
    std::string ragDocs;
    std::string allow;
    std::string deny;
    std::string toolName;
    std::string toolArgs;
    int32_t     kind               = 0;
    int32_t     interruptPolicy    = -1;
    int32_t     sessionStore       = -1;
    int64_t     interruptTimeoutMs = 0;
    int64_t     runTimeoutMs       = 0;
    int64_t     delayMs            = 0;
    int64_t     queueTimeoutMs     = 0;
    AgentxxPluginAsyncWorkEventFn onEvent  = nullptr;
    void*                         userData = nullptr;
};

/// 从选项拷出全部入参 (JSON 在提交前就序列化好)
inline AsyncWorkSpecKeep keepAsyncWorkOptions(const AsyncWorkOptions& opt) {
    auto jsonText = [](const utilxx_base::Json& value) {
        return value.is_null() ? std::string{} : value.dump();
    };
    AsyncWorkSpecKeep k;
    k.name               = opt.name;
    k.systemPrompt       = opt.systemPrompt;
    k.message            = opt.message;
    k.model              = opt.model;
    k.sessionId          = opt.sessionId;
    k.queueKey           = opt.queueKey;
    k.workDir            = opt.workDir;
    k.language           = opt.language;
    k.notifyDelivery     = opt.notifyDelivery;
    k.tools              = jsonText(opt.tools);
    k.plugins            = jsonText(opt.plugins);
    k.mcpServers         = jsonText(opt.mcpServers);
    k.skills             = jsonText(opt.skills);
    k.memoryFiles        = jsonText(opt.memoryFiles);
    k.ragDocs            = jsonText(opt.ragDocs);
    k.allow              = jsonText(opt.permissionAllow);
    k.deny               = jsonText(opt.permissionDeny);
    k.toolName           = opt.toolName;
    k.toolArgs           = jsonText(opt.toolArgs);
    k.kind               = opt.toolName.empty() ? 0 : 1;
    k.interruptPolicy    = opt.interruptPolicy;
    k.sessionStore       = opt.sessionStore;
    k.interruptTimeoutMs = opt.interruptTimeoutMs;
    k.runTimeoutMs       = opt.runTimeoutMs;
    k.delayMs            = opt.delayMs;
    k.queueTimeoutMs     = opt.queueTimeoutMs;
    k.onEvent            = opt.onEvent;
    k.userData           = opt.userData;
    return k;
}

/// 沿用入参填出跨边界 spec (字符串视图指向 `keep` 里的字符串)
inline void fillAsyncWorkSpec(const AsyncWorkSpecKeep& keep, AgentxxPluginAsyncWorkSpec& spec) {
    spec.struct_size           = sizeof(AgentxxPluginAsyncWorkSpec);
    spec.kind                  = keep.kind;
    spec.interrupt_policy      = keep.interruptPolicy;
    spec.session_store         = keep.sessionStore;
    spec.interrupt_timeout_ms  = keep.interruptTimeoutMs;
    spec.run_timeout_ms        = keep.runTimeoutMs;
    spec.session_id            = PluginStringView::from(keep.sessionId);
    spec.queue_key             = PluginStringView::from(keep.queueKey);
    spec.name                  = PluginStringView::from(keep.name);
    spec.system_prompt         = PluginStringView::from(keep.systemPrompt);
    spec.message               = PluginStringView::from(keep.message);
    spec.model                 = PluginStringView::from(keep.model);
    spec.tools_json            = PluginStringView::from(keep.tools);
    spec.plugins_json          = PluginStringView::from(keep.plugins);
    spec.mcp_servers_json      = PluginStringView::from(keep.mcpServers);
    spec.skills_json           = PluginStringView::from(keep.skills);
    spec.memory_files_json     = PluginStringView::from(keep.memoryFiles);
    spec.rag_docs_json         = PluginStringView::from(keep.ragDocs);
    spec.work_dir              = PluginStringView::from(keep.workDir);
    spec.language              = PluginStringView::from(keep.language);
    spec.permission_allow_json = PluginStringView::from(keep.allow);
    spec.permission_deny_json  = PluginStringView::from(keep.deny);
    spec.notify_delivery       = PluginStringView::from(keep.notifyDelivery);
    spec.tool_name             = PluginStringView::from(keep.toolName);
    spec.tool_args_json        = PluginStringView::from(keep.toolArgs);
    spec.delay_ms              = keep.delayMs;
    spec.queue_timeout_ms      = keep.queueTimeoutMs;
    spec.on_event              = keep.onEvent;
    spec.user_data             = keep.userData;
}

} // namespace detail



/* ==================== 业务表便捷入口 (asio 形态) ====================
 *
 * 每个入口都是 `asio::awaitable`: 调用方在自己的协程里 `co_await` 它, 得到
 * `pluginxx::TableOpResult` (`ok()` / `text()` / `integer()` / `flag()`)。
 * 主程序不支持该能力时 `accepted() == false` 且 `startError` 有原因, 不会挂起。
 *
 * - 只能在 **asio 协程**里 `co_await` (插件执行线程上不能等表, 要结果就写协程);
 * - 这些入口本身是协程, **提交发生在开始 `co_await` 的那一刻** (不是调用那一行),
 *   因此想让几条调用并行, 需要用 `asio::co_spawn` 显式开协程。
 *
 * 文本参数一律**按值** (`std::string`): `asio::awaitable` 的帧只会在参数上做复制,
 * 引用/视图形式在“发起后不立即 `co_await`”时 (例如 `spawnIo` 交给另一个执行器)
 * 会指向已释放的对象。调用方可以直接传临时量。
 *
 * 返回 `asio::awaitable<...>` (执行器是默认的 `asio::any_io_executor`): 调用方那个
 * 协程用的也得是它 (asio 只允许 co_await 执行器相同的 awaitable)。
 */

namespace detail {

/// 未受理的结果 (主程序没有这张表 / 参数不全): `accepted()` 为 false
inline pluginxx::TableOpResult tableOpUnsupported() {
    pluginxx::TableOpResult r;
    r.startError = "plugin table call was not accepted";
    return r;
}

} // namespace detail

/// 表调用的 asio 形态 (通用入口; 用法与 [pluginxx::tableOp] 相同, 返回 awaitable)
///
/// 注意: 本函数是协程, 真正提交发生在开始 `co_await` 时 (不是调用这一行)。
template<typename Ctx, typename StartFn>
inline asio::awaitable<pluginxx::TableOpResult> tableOpAsync(Ctx& ctx, StartFn start) {
    co_return co_await pluginxx::detail::tableOpAwaitAsyncSlot(ctx, std::move(start));
}

/// 写入会话共享存储 (asio 形态; 结果是新条目 id, 失败 -1)
///
/// - 主程序没有这张表时 `co_return -1`;
/// - `tid` / `content` 按值: 本协程帧自己持有一份, 调用方可以直接传临时量。
template<typename Ctx>
inline asio::awaitable<int64_t>
    addShareStoreAsync(Ctx& ctx, const ScopeRef& agent, std::string tid, std::string content) {
    if (!ctx.host || !ctx.iface.session || !ctx.iface.session->add_share_store) {
        co_return int64_t{-1};
    }
    auto ex{co_await asio::this_coro::executor};
    auto r = co_await pluginxx::asyncTableOp(
        ctx,
        ex,
        [&ctx, agent, tid = std::move(tid), content = std::move(content)](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            auto ref  = agent.raw();
            auto key  = PluginStringView::from(tid);
            auto text = PluginStringView::from(content);
            return ctx.iface.session->add_share_store(ctx.host, &ref, &key, &text, cb, ud, err);
        }
    );
    co_return r.ok() ? r.integer(-1) : int64_t{-1};
}

/// 把阻塞工作交给主程序工作池 (asio 形态; 完成后回到本协程的执行器)
///
/// `work` 可以不带参数, 也可以接 `const PluginxxCancelToken*` (取消请求来了只会
/// 置标记, 由工作体自己配合处理):
///
/// ```cpp
/// auto collected = co_await agentxx::plugin::offloadAsync(c, [&] {
///     return collectExpensivePaths();   // 目录遍历 / 全文件扫描
/// });
/// ```
///
/// - 没有可用工作池时抛 `runtime_error` (与 `pluginxx::offload` 一致);
/// - 工作体抛出的异常原样重抛到 `co_await` 处, 取消抛 `pluginxx::CanceledException`。
/// offload 工作体的结果类型 (工作体可以不带参数, 也可以收取消令牌)
/// - 两条 `invoke_result` 只能取一条: 用特化而不是 `conditional_t`, 后者的两个分支
///   都会被实例化 (带令牌的 lambda 拿零参形式是非良构)
template<typename WorkFn, bool WithToken>
struct OffloadAsyncTraits;

template<typename WorkFn>
struct OffloadAsyncTraits<WorkFn, false> {
    using Result = std::decay_t<std::invoke_result_t<WorkFn&>>;
};

template<typename WorkFn>
struct OffloadAsyncTraits<WorkFn, true> {
    using Result = std::decay_t<std::invoke_result_t<WorkFn&, const PluginxxCancelToken*>>;
};

template<typename WorkFn>
using OffloadAsyncResult = typename OffloadAsyncTraits<
    WorkFn,
    std::is_invocable_v<WorkFn&, const PluginxxCancelToken*>>::Result;

template<typename Ctx, typename WorkFn>
inline asio::awaitable<OffloadAsyncResult<WorkFn>>
    offloadAsync(Ctx& ctx, WorkFn work) {
    using ResultT = OffloadAsyncResult<WorkFn>;
    auto ex{co_await asio::this_coro::executor};
    auto outcome = co_await pluginxx::asyncOffload(
        ctx,
        ex,
        [work = std::move(work)](const PluginxxCancelToken* token) mutable -> ResultT {
            if constexpr (std::is_invocable_v<WorkFn&, const PluginxxCancelToken*>) {
                return work(token);
            } else {
                (void)token;
                return work();
            }
        }
    );
    if (!outcome.startError.empty()) {
        throw std::runtime_error(outcome.startError);
    }
    if (outcome.workError) {
        std::rethrow_exception(outcome.workError);
    }
    if (outcome.status == PLUGINXX_OPERATOR_CANCELED) {
        throw pluginxx::CanceledException(
            outcome.error.empty() ? "offload canceled" : outcome.error
        );
    }
    if (outcome.status == PLUGINXX_OPERATOR_FAILED) {
        throw std::runtime_error(outcome.error.empty() ? "offload failed" : outcome.error);
    }
    if constexpr (std::is_void_v<ResultT>) {
        co_return;
    } else {
        co_return outcome.value ? std::move(*outcome.value) : ResultT{};
    }
}

/// 打开一个会话通道 (asio 形态; 结果是通道 id, 失败 0)
///
/// 事件回调的转接对象由本协程帧保活到受理成功, 受理后登记到 `ctx.runtimeChannelShims`
/// (关闭通道时摘除)。
template<typename Ctx, typename Fn>
inline asio::awaitable<int64_t> openChannelAsync(Ctx& ctx, ChannelOptions opt, Fn onEvent) {
    if (!hasRuntime(ctx) || !ctx.iface.runtime->open_channel || opt.rootId.empty()) {
        co_return 0;
    }
    auto ex{co_await asio::this_coro::executor};
    auto shim = std::make_unique<detail::RuntimeChannelShim>();
    shim->fn  = std::make_shared<const std::function<void(const ChannelEvent&)>>(
        std::function<void(const ChannelEvent&)>(std::forward<Fn>(onEvent))
    );
    auto rootId    = std::string{opt.rootId};
    auto sessionId = std::string{opt.sessionId};
    auto title     = std::string{opt.title};
    auto model     = std::string{opt.model};
    AgentxxPluginChannelSpec spec{};
    spec.struct_size          = sizeof(AgentxxPluginChannelSpec);
    spec.root_id              = PluginStringView::from(rootId);
    spec.session_id           = PluginStringView::from(sessionId);
    spec.title                = PluginStringView::from(title);
    spec.model                = PluginStringView::from(model);
    spec.max_queue            = opt.maxQueue;
    spec.interrupt_timeout_ms = static_cast<int64_t>(opt.interruptTimeout.count());
    spec.on_event             = &detail::runtimeChannelEventThunk;
    spec.user_data            = shim.get();
    auto r = co_await pluginxx::asyncTableOp(
        ctx,
        ex,
        [&ctx, &spec](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            return ctx.iface.runtime->open_channel(ctx.host, &spec, cb, ud, err);
        }
    );
    const int64_t channelId = r.ok() ? r.integer(0) : 0;
    if (channelId <= 0) {
        co_return 0;
    }
    ctx.runtimeChannelShims[channelId] = std::move(shim);
    co_return channelId;
}

/// 关闭一个通道 (asio 形态; 结果经最后一条 `CLOSED` 事件体现)
///
/// 受理成功后摘掉事件转接对象 (此后主程序不再回调它)。
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> closeChannelAsync(Ctx& ctx, int64_t channelId) {
    auto ex{co_await asio::this_coro::executor};
    if (!hasRuntime(ctx) || !ctx.iface.runtime->close_channel || channelId <= 0) {
        co_return detail::tableOpUnsupported();
    }
    auto r = co_await pluginxx::asyncTableOp(
        ctx,
        ex,
        [&ctx, channelId](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            return ctx.iface.runtime->close_channel(ctx.host, channelId, cb, ud, err);
        }
    );
    if (r.ok()) {
        ctx.runtimeChannelShims.erase(channelId);
    }
    co_return r;
}

/// 投递一条输入 (asio 形态; `.ok()` = 已受理)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    sendInputAsync(Ctx& ctx, int64_t channelId, std::string inputJson) {
    auto ex{co_await asio::this_coro::executor};
    if (!hasRuntime(ctx) || !ctx.iface.runtime->send_input || channelId <= 0) {
        co_return detail::tableOpUnsupported();
    }
    co_return co_await pluginxx::asyncTableOp(
        ctx,
        ex,
        [&ctx, channelId, input = std::move(inputJson)](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            auto jsonSv = PluginStringView::from(input);
            return ctx.iface.runtime->send_input(ctx.host, channelId, &jsonSv, cb, ud, err);
        }
    );
}

/// 取消通道当前轮次 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> cancelChannelAsync(Ctx& ctx, int64_t channelId) {
    auto ex{co_await asio::this_coro::executor};
    if (!hasRuntime(ctx) || !ctx.iface.runtime->cancel || channelId <= 0) {
        co_return detail::tableOpUnsupported();
    }
    co_return co_await pluginxx::asyncTableOp(
        ctx,
        ex,
        [&ctx, channelId](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            return ctx.iface.runtime->cancel(ctx.host, channelId, cb, ud, err);
        }
    );
}

/// 应答一个中断 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> respondInterruptAsync(
    Ctx&             ctx,
    int64_t          channelId,
    int64_t          interruptId,
    std::string      valuesJson
) {
    auto ex{co_await asio::this_coro::executor};
    if (!hasRuntime(ctx) || !ctx.iface.runtime->respond_interrupt || channelId <= 0) {
        co_return detail::tableOpUnsupported();
    }
    co_return co_await pluginxx::asyncTableOp(
        ctx,
        ex,
        [&ctx, channelId, interruptId, values = std::move(valuesJson)](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            auto valuesSv = PluginStringView::from(values);
            return ctx.iface.runtime->respond_interrupt(
                ctx.host,
                channelId,
                interruptId,
                &valuesSv,
                cb,
                ud,
                err
            );
        }
    );
}

/// 通道状态 JSON (asio 形态; `.text()` 是 JSON)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    channelStateJsonAsync(Ctx& ctx, int64_t channelId) {
    auto ex{co_await asio::this_coro::executor};
    if (!hasRuntime(ctx) || !ctx.iface.runtime->get_channel_state || channelId <= 0) {
        co_return detail::tableOpUnsupported();
    }
    co_return co_await pluginxx::asyncTableOp(
        ctx,
        ex,
        [&ctx, channelId](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            return ctx.iface.runtime->get_channel_state(ctx.host, channelId, cb, ud, err);
        }
    );
}

/// 运行时清单 JSON (asio 形态; `.text()` 是 JSON)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> listRuntimeJsonAsync(Ctx& ctx) {
    auto ex{co_await asio::this_coro::executor};
    if (!hasRuntime(ctx) || !ctx.iface.runtime->list_runtime) {
        co_return detail::tableOpUnsupported();
    }
    co_return co_await pluginxx::asyncTableOp(
        ctx,
        ex,
        [&ctx](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            return ctx.iface.runtime->list_runtime(ctx.host, cb, ud, err);
        }
    );
}

/// 提交一个后台任务 (asio 形态; `.text()` = `asyncId`)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    submitAsyncWorkAsync(Ctx& ctx, AsyncWorkOptions opt) {
    auto ex{co_await asio::this_coro::executor};
    if (!hasRuntime(ctx) || !ctx.iface.runtime->submit_async_work) {
        co_return detail::tableOpUnsupported();
    }
    auto keep = detail::keepAsyncWorkOptions(opt);
    co_return co_await pluginxx::asyncTableOp(
        ctx,
        ex,
        [&ctx, keep = std::move(keep)](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            AgentxxPluginAsyncWorkSpec spec{};
            detail::fillAsyncWorkSpec(keep, spec);
            return ctx.iface.runtime->submit_async_work(ctx.host, &spec, cb, ud, err);
        }
    );
}

/// 取消一条后台任务 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    cancelAsyncWorkAsync(Ctx& ctx, std::string asyncId) {
    auto ex{co_await asio::this_coro::executor};
    if (!hasRuntime(ctx) || !ctx.iface.runtime->cancel_async_work || asyncId.empty()) {
        co_return detail::tableOpUnsupported();
    }
    co_return co_await pluginxx::asyncTableOp(
        ctx,
        ex,
        [&ctx, id = std::move(asyncId)](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            auto idSv = PluginStringView::from(id);
            return ctx.iface.runtime->cancel_async_work(ctx.host, &idSv, cb, ud, err);
        }
    );
}

/// 单条后台任务 JSON (asio 形态; `.text()` 是 JSON)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    getAsyncWorkJsonAsync(Ctx& ctx, std::string asyncId) {
    auto ex{co_await asio::this_coro::executor};
    if (!hasRuntime(ctx) || !ctx.iface.runtime->get_async_work || asyncId.empty()) {
        co_return detail::tableOpUnsupported();
    }
    co_return co_await pluginxx::asyncTableOp(
        ctx,
        ex,
        [&ctx, id = std::move(asyncId)](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            auto idSv = PluginStringView::from(id);
            return ctx.iface.runtime->get_async_work(ctx.host, &idSv, cb, ud, err);
        }
    );
}

namespace detail {

/// 手写 get_info 入口的公共实现 (agent 侧): 填插件名/版本/描述与要求的最低主程序版本
inline int32_t fillAgentPluginInfo(
    PluginxxInfo* out,
    const char*   name,
    const char*   version,
    const char*   description
) noexcept {
    return pluginxx::detail::fillPluginInfo(
        out,
        name,
        version,
        description,
        AGENTXX_PLUGIN_MIN_API_VERSION
    );
}


/// 图节点操作的输入键 (kit 写进 `RootRequest::extraFields`, 业务侧用下面的
/// [nodeNameOf] / [nodeConfigOf] / [nodeStateOf] 读取)
inline constexpr std::string_view kGraphNodeNameKey   = "node_name";
inline constexpr std::string_view kGraphNodeConfigKey = "config_json";
inline constexpr std::string_view kGraphNodeStateKey  = "state_json";

/// 工具操作的根请求 (args / 作用域键 (= thread_id) / 工具调用 id)
inline RootRequest toolRootRequest(
    const PluginxxHost*       host,
    const PluginxxStringView* argsJson,
    const PluginxxStringView* scopeKey,
    const PluginxxStringView* callId
) {
    RootRequest req;
    req.host      = host;
    req.argsJson  = RootRequest::copyView(argsJson, "{}");
    req.scopeKey  = RootRequest::copyView(scopeKey);
    req.callId    = RootRequest::copyView(callId);
    return req;
}

/// 图节点操作的根请求 (节点名 / config / 只读 state 快照 / 作用域键)
inline RootRequest graphNodeRootRequest(
    const PluginxxHost*       host,
    const PluginxxStringView* nodeName,
    const PluginxxStringView* configJson,
    const PluginxxStringView* stateJson,
    const PluginxxStringView* scopeKey
) {
    RootRequest req;
    req.host     = host;
    req.scopeKey = RootRequest::copyView(scopeKey);
    req.setExtra(std::string{kGraphNodeNameKey}, RootRequest::copyView(nodeName));
    req.setExtra(std::string{kGraphNodeConfigKey}, RootRequest::copyView(configJson, "{}"));
    req.setExtra(std::string{kGraphNodeStateKey}, RootRequest::copyView(stateJson, "{}"));
    return req;
}

} // namespace detail

namespace detail {

/// `call_tool` 表调用的元数据槽 + 回调适配体
///
/// 工具表的完成回调是四参 (状态/正文/元数据), 而 asio 表调用只给一段正文,
/// 所以元数据在这里单独存到调用方的 `shared_ptr<string>` 里。
struct CallToolMetaCarrier {
    PluginxxOperatorCallback     cb = nullptr;
    void*                        ud = nullptr;
    std::shared_ptr<std::string> meta;
};

/// 四参完成回调 → 表调用回调 (元数据先写槽, 再交回正文)
inline void PLUGINXX_CALL callToolFourArgToTableOp(
    void*                     ud,
    int32_t                   status,
    const PluginxxStringView* result,
    const PluginxxStringView* payload
) {
    auto* carrier   = static_cast<CallToolMetaCarrier*>(ud);
    auto  cb        = carrier->cb;
    void* cbUd      = carrier->ud;
    auto  meta      = carrier->meta;
    delete carrier;
    if (meta && payload && payload->data && payload->size > 0) {
        meta->assign(payload->data, static_cast<size_t>(payload->size));
    }
    if (cb) {
        cb(cbUd, status, result);
    }
}

} // namespace detail

/// 插件互调 (**asio 形态**): 取完整 `ToolResult`
///
/// 与 [callToolResultAsync] 同一份语义, 只是等待方式是 asio 协程:
/// - 取消抛 `CanceledException`;
/// - 启动被拒抛 `std::runtime_error`;
/// - 工具失败不抛, 看返回值的 `status`。
template<typename Ctx>
inline asio::awaitable<ToolResult> callToolResultAsync(
    const Ctx&       ctx,
    const ScopeRef&  agent,
    std::string_view name,
    std::string_view argsJson,
    std::string_view sessionId = {}
) {
    if (!ctx.iface.tools || !ctx.iface.tools->call_tool) {
        throw std::runtime_error("call_tool: tools iface unavailable");
    }
    // 入参按值进协程帧: 调用方可以传临时量
    auto nm   = std::string{name};
    auto args = std::string{argsJson};
    auto tid  = std::string{sessionId};
    auto meta = std::make_shared<std::string>();
    auto ref  = agent;

    auto r = co_await pluginxx::asyncTableOp(
        ctx,
        co_await asio::this_coro::executor,
        [&ctx, ref, nm, args, tid, meta](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            auto* carrier = new detail::CallToolMetaCarrier{cb, ud, meta};
            auto  nameSv  = PluginStringView::from(nm.data(), nm.size());
            auto  argsSv  = PluginStringView::from(args.data(), args.size());
            auto  tidSv   = PluginStringView::from(tid.data(), tid.size());
            auto  r       = ref.raw();
            auto* op      = ctx.iface.tools->call_tool(
                ctx.host,
                &r,
                &nameSv,
                &argsSv,
                &tidSv,
                &detail::callToolFourArgToTableOp,
                carrier,
                err
            );
            if (!op) {
                delete carrier;
            }
            return op;
        }
    );

    if (!r.startError.empty()) {
        throw std::runtime_error("call_tool start failed: " + r.startError);
    }
    if (r.status == PLUGINXX_OPERATOR_CANCELED) {
        throw CanceledException(r.text().empty() ? "call_tool canceled" : std::string{r.text()});
    }
    if (r.status != PLUGINXX_OPERATOR_OK) {
        co_return ToolResult::fromParts(std::string{r.text()}, ToolStatus::Failed, {});
    }
    co_return ToolResult::fromParts(std::string{r.text()}, ToolStatus::Ok, std::move(*meta));
}

/// 插件互调 (**asio 形态**): 只取结果正文
///
/// 与 [callToolResultAsync] 同语义: 失败/取消抛异常; 要自己处理失败用
/// [callToolResultAsync]。
template<typename Ctx>
inline asio::awaitable<std::string> callToolAsync(
    const Ctx&       ctx,
    const ScopeRef&  agent,
    std::string_view name,
    std::string_view argsJson,
    std::string_view sessionId = {}
) {
    auto r = co_await callToolResultAsync(ctx, agent, name, argsJson, sessionId);
    if (r.status != ToolStatus::Ok) {
        throw std::runtime_error(r.text.empty() ? "call_tool failed" : r.text);
    }
    co_return std::move(r.text);
}

/* ---- 业务表便捷入口的 asio 形态 ----
 *
 * 这一节是业务表的唯一形态: 给 `asio::awaitable` 协程 (工具体 / 挂载事务 /
 * 插件自有 io 线程) 用。结果仍是 `pluginxx::TableOpResult` (业务失败不抛异常)。
 */

namespace detail {

/// 取当前协程的执行器后提交一次表调用 (asio 形态的公共部分)
template<typename Ctx, typename StartFn>
inline asio::awaitable<pluginxx::TableOpResult> tableOpAwaitAsync(Ctx& ctx, StartFn start) {
    co_return co_await pluginxx::detail::tableOpAwaitAsyncSlot(ctx, std::move(start));
}

} // namespace detail

/// 读当前完整提示词 JSON (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    promptJsonAsync(Ctx& ctx, const PluginxxScopeRef* agent) {
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, agent](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.prompt || !ctx.iface.prompt->get_prompt) {
                return nullptr;
            }
            return ctx.iface.prompt->get_prompt(ctx.host, agent, cb, ud, err);
        }
    );
}

/// 写回本插件贡献的提示词段 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> setPromptJsonAsync(
    Ctx&                    ctx,
    const PluginxxScopeRef* agent,
    std::string_view        json
) {
    auto text = std::string{json};
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, agent, text = std::move(text)](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.prompt || !ctx.iface.prompt->set_prompt) {
                return nullptr;
            }
            auto sv = PluginStringView::from(text.data(), text.size());
            return ctx.iface.prompt->set_prompt(ctx.host, agent, &sv, cb, ud, err);
        }
    );
}

/// 读该 agent 的主模型及关联配置 JSON (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    modelConfigJsonAsync(Ctx& ctx, const PluginxxScopeRef* agent) {
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, agent](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.model || !ctx.iface.model->get_config) {
                return nullptr;
            }
            return ctx.iface.model->get_config(ctx.host, agent, cb, ud, err);
        }
    );
}

/// 读该 agent 当前执行图 JSON 定义 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    graphJsonAsync(Ctx& ctx, const PluginxxScopeRef* agent) {
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, agent](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.graph || !ctx.iface.graph->get_graph_json) {
                return nullptr;
            }
            return ctx.iface.graph->get_graph_json(ctx.host, agent, cb, ud, err);
        }
    );
}

/// 当前执行图名称 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    graphNameAsync(Ctx& ctx, const PluginxxScopeRef* agent) {
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, agent](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.graph || !ctx.iface.graph->get_graph_name) {
                return nullptr;
            }
            return ctx.iface.graph->get_graph_name(ctx.host, agent, cb, ud, err);
        }
    );
}

/// 写回执行图 JSON 定义 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> setGraphJsonAsync(
    Ctx&                    ctx,
    const PluginxxScopeRef* agent,
    std::string_view        json
) {
    auto text = std::string{json};
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, agent, text = std::move(text)](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.graph || !ctx.iface.graph->set_graph_json) {
                return nullptr;
            }
            auto sv = PluginStringView::from(text.data(), text.size());
            return ctx.iface.graph->set_graph_json(ctx.host, agent, &sv, cb, ud, err);
        }
    );
}

/// 本插件向该 agent 贡献的资源清单 JSON (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    ownResourcesJsonAsync(Ctx& ctx, const PluginxxScopeRef* agent) {
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, agent](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.resources || !ctx.iface.resources->get_own_resources) {
                return nullptr;
            }
            return ctx.iface.resources->get_own_resources(ctx.host, agent, cb, ud, err);
        }
    );
}

/// 资源登记/注销的 asio 形态公共实现 (单个字符串入参)
template<typename Ctx, typename StartFn>
inline asio::awaitable<pluginxx::TableOpResult>
    resourceOpAwaitAsync(Ctx& ctx, std::string_view arg, StartFn start) {
    auto text = std::string{arg};
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, text = std::move(text), start = std::move(start)](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            auto view = PluginStringView::from(text.data(), text.size());
            return start(view, cb, ud, err);
        }
    );
}

/// 登记技能目录 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> registerSkillDirAsync(
    Ctx&                    ctx,
    const PluginxxScopeRef* agent,
    std::string_view        path
) {
    co_return co_await resourceOpAwaitAsync(
        ctx,
        path,
        [&ctx, agent](PluginxxStringView view, PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.resources || !ctx.iface.resources->register_skill_dir) {
                return nullptr;
            }
            return ctx.iface.resources->register_skill_dir(ctx.host, agent, &view, cb, ud, err);
        }
    );
}

/// 撤销技能目录 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> unregisterSkillDirAsync(
    Ctx&                    ctx,
    const PluginxxScopeRef* agent,
    std::string_view        path
) {
    co_return co_await resourceOpAwaitAsync(
        ctx,
        path,
        [&ctx, agent](PluginxxStringView view, PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.resources || !ctx.iface.resources->unregister_skill_dir) {
                return nullptr;
            }
            return ctx.iface.resources->unregister_skill_dir(ctx.host, agent, &view, cb, ud, err);
        }
    );
}

/// 登记记忆文件 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> registerMemoryFileAsync(
    Ctx&                    ctx,
    const PluginxxScopeRef* agent,
    std::string_view        path
) {
    co_return co_await resourceOpAwaitAsync(
        ctx,
        path,
        [&ctx, agent](PluginxxStringView view, PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.resources || !ctx.iface.resources->register_memory_file) {
                return nullptr;
            }
            return ctx.iface.resources->register_memory_file(ctx.host, agent, &view, cb, ud, err);
        }
    );
}

/// 撤销记忆文件 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> unregisterMemoryFileAsync(
    Ctx&                    ctx,
    const PluginxxScopeRef* agent,
    std::string_view        path
) {
    co_return co_await resourceOpAwaitAsync(
        ctx,
        path,
        [&ctx, agent](PluginxxStringView view, PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.resources || !ctx.iface.resources->unregister_memory_file) {
                return nullptr;
            }
            return ctx.iface.resources->unregister_memory_file(ctx.host, agent, &view, cb, ud, err);
        }
    );
}

/// 登记 MCP 服务器 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> registerMcpServerAsync(
    Ctx&                    ctx,
    const PluginxxScopeRef* agent,
    std::string_view        specJson
) {
    co_return co_await resourceOpAwaitAsync(
        ctx,
        specJson,
        [&ctx, agent](PluginxxStringView view, PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.resources || !ctx.iface.resources->register_mcp_server) {
                return nullptr;
            }
            return ctx.iface.resources->register_mcp_server(ctx.host, agent, &view, cb, ud, err);
        }
    );
}

/// 撤销 MCP 服务器 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> unregisterMcpServerAsync(
    Ctx&                    ctx,
    const PluginxxScopeRef* agent,
    std::string_view        nameSpace
) {
    co_return co_await resourceOpAwaitAsync(
        ctx,
        nameSpace,
        [&ctx, agent](PluginxxStringView view, PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.resources || !ctx.iface.resources->unregister_mcp_server) {
                return nullptr;
            }
            return ctx.iface.resources->unregister_mcp_server(ctx.host, agent, &view, cb, ud, err);
        }
    );
}

/// 全部挂载的摘要 JSON 数组 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> listMountsAsync(Ctx& ctx) {
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.mount || !ctx.iface.mount->list) {
                return nullptr;
            }
            return ctx.iface.mount->list(ctx.host, cb, ud, err);
        }
    );
}

/// 按 agentId 查一个挂载的摘要 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    mountInfoOfAsync(Ctx& ctx, std::string_view agentId) {
    auto idText = std::string{agentId};
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, idText = std::move(idText)](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.mount || !ctx.iface.mount->get) {
                return nullptr;
            }
            auto idSv = PluginStringView::from(idText.data(), idText.size());
            return ctx.iface.mount->get(ctx.host, &idSv, cb, ud, err);
        }
    );
}

/// 工具提示词描述 (asio 形态; 用法同 [toolPromptOf])
template<typename Ctx>
inline asio::awaitable<ToolPromptText> toolPromptOfAsync(Ctx& ctx, std::string_view tool) {
    auto name = std::string{tool};
    auto r    = co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, name = std::move(name)](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.prompt || !ctx.iface.prompt->get_tool_prompt) {
                return nullptr;
            }
            auto v = PluginStringView::from(name.data(), name.size());
            return ctx.iface.prompt->get_tool_prompt(ctx.host, &v, cb, ud, err);
        }
    );
    co_return parseToolPromptJson(r.ok() ? r.text() : std::string_view{});
}

/// 声明式模式构建器 (asio 形态; 用法同 [schemaOf])
template<typename Ctx>
inline asio::awaitable<ToolSchemaBuilder> schemaOfAsync(Ctx& ctx, std::string_view tool) {
    co_return ToolSchemaBuilder{co_await toolPromptOfAsync(ctx, tool)};
}

/* ---- 广播 (agent 侧 kit 的 asio 形态) ---- */

/// 声明一条广播频道 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> declarePluginBroadcastAsync(
    Ctx&             ctx,
    std::string_view id,
    bool             anyonePublish = false,
    bool             cacheLatest   = true
) {
    auto text = std::string{id};
    const bool anyone = anyonePublish;
    const bool cache  = cacheLatest;
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, text = std::move(text), anyone, cache](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            if (text.empty() || !ctx.host || !ctx.iface.broadcasts
                || !ctx.iface.broadcasts->declare_broadcast) {
                return nullptr;
            }
            const std::string decl = "{\"id\":\"" + text + "\",\"cache\":\""
                                     + (cache ? "latest" : "none") + "\",\"publish\":\""
                                     + (anyone ? "anyone" : "owner") + "\"}";
            auto declSv = PluginStringView::from(decl.data(), decl.size());
            return ctx.iface.broadcasts->declare_broadcast(ctx.host, &declSv, cb, ud, err);
        }
    );
}

/// 发布一条广播 (asio 形态; 发布前先补声明)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> publishPluginBroadcastAsync(
    Ctx&             ctx,
    std::string_view id,
    std::string_view payload,
    bool             anyonePublish = false
) {
    auto idText      = std::string{id};
    auto payloadText = std::string{payload};
    co_await declarePluginBroadcastAsync(ctx, idText, anyonePublish);
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, idText = std::move(idText), payloadText = std::move(payloadText)](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            if (idText.empty() || !ctx.host || !ctx.iface.broadcasts
                || !ctx.iface.broadcasts->publish_broadcast) {
                return nullptr;
            }
            auto idSv      = PluginStringView::from(idText.data(), idText.size());
            auto payloadSv = PluginStringView::from(payloadText.data(), payloadText.size());
            return ctx.iface.broadcasts->publish_broadcast(
                ctx.host,
                &idSv,
                &payloadSv,
                cb,
                ud,
                err
            );
        }
    );
}

/// 订阅一条广播 (asio 形态; 结果正文是句柄的十进制文本, 用 [subscriptionOf] 转回)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> subscribePluginBroadcastAsync(
    Ctx&                    ctx,
    const PluginxxScopeRef* scope,
    std::string_view        id,
    void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*),
    void*                   ud
) {
    auto text = std::string{id};
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, scope, text = std::move(text), handler, ud](
            PluginxxOperatorCallback cb,
            void*                    opUd,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            if (text.empty() || !handler || !ctx.host || !ctx.iface.broadcasts
                || !ctx.iface.broadcasts->subscribe_broadcast) {
                return nullptr;
            }
            auto idSv = PluginStringView::from(text.data(), text.size());
            return ctx.iface.broadcasts->subscribe_broadcast(
                ctx.host,
                scope,
                &idSv,
                handler,
                ud,
                nullptr,
                cb,
                opUd,
                err
            );
        }
    );
}

/// 撤销一条广播订阅 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult>
    unsubscribePluginBroadcastAsync(Ctx& ctx, PluginxxSubscription* sub) {
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, sub](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (sub == nullptr || !ctx.host || !ctx.iface.broadcasts
                || !ctx.iface.broadcasts->unsubscribe_broadcast) {
                return nullptr;
            }
            return ctx.iface.broadcasts->unsubscribe_broadcast(ctx.host, sub, cb, ud, err);
        }
    );
}

/* ==================== 功能埋点 (实现行 = 能力行) ====================
 *
 * 插件的"给结果 / 补数据"走能力表: 为**主程序或自己声明的功能埋点**登记一行实现
 * (`agentxx.agent.point.*` / `plugin.<自己>.point.*`), 取值时按 scope 过滤与优先级
 * 排序后调用这行; 一行给出 JSON 结果, 首行非空的那行生效。
 *
 * 只想**知道**功能埋点被触发了, 订阅同名广播频道即可 (§20), 不需要登记实现。
 */

/// 功能埋点实现行的登记选项
struct PointImplOptions {
    /// 行内顺序, 小者先 (默认 0); 越界由主程序裁剪并记警告
    int32_t priority = 0;
    /// 自报的最大耗时 (毫秒; 0 = 用功能埋点声明里的值)
    int32_t timeoutMs = 0;
    /// 同一实例内区分多个处理器 (可空)
    std::string_view ownerTag{};
};

/// 规范化实现行的回答: 已带 `value` / `disable` 的原样返回, 其它按整份当值
inline std::string pointAnswer(std::string_view raw) {
    if (raw.empty()) {
        return {};
    }
    std::optional<utilxx_base::Json> parsed;
    try {
        parsed = utilxx_base::Json::parse(raw);
    } catch (const std::exception&) {
        return std::string{raw};
    }
    if (!parsed.has_value()) {
        return std::string{raw};
    }
    if (parsed->is_object() && (parsed->contains("value") || parsed->contains("disable"))) {
        return std::string{raw};
    }
    utilxx_base::Json wrapped = utilxx_base::Json::object();
    wrapped["value"]          = *parsed;
    return wrapped.dump();
}

namespace detail {

/// 把 `(ctx, 载荷)` / `(载荷)` 形态的实现体适配成能力表要的三参处理器
/// (能力表的处理器固定收 `(ctx, method, args)`, 功能埋点的 method 就是点 id)
template<typename Ctx, typename Fn>
struct PointImplAdapter {
    Fn fn;

    template<typename C>
    decltype(auto) operator()(C& c, std::string_view /*method*/, std::string_view payload) {
        if constexpr (std::is_invocable_v<Fn&, Ctx&, std::string_view>) {
            return fn(c, payload);
        } else {
            return fn(payload);
        }
    }
};

} // namespace detail

/// 为一个功能埋点登记一行实现 (**asio 形态**: `co_await` 得到登记结果)
///
/// 与 [providePoint] 参数一致, 只是登记与等待都用 asio 协程:
/// 实现体可以写 `asio::awaitable<T>` (异步) 或直接返回字符串 (同步)。
template<typename Ctx, typename Fn>
inline asio::awaitable<pluginxx::TableOpResult> providePointAsync(
    Ctx&                    ctx,
    const ScopeRef&         ref,
    std::string_view        pointId,
    Fn&&                    fn,
    const PointImplOptions& opts = {}
) {
    pluginxx::CapabilityOptions cap;
    cap.impls     = PLUGINXX_CAPABILITY_IMPLS_MANY;
    cap.merge     = PLUGINXX_CAPABILITY_MERGE_FIRST;
    cap.phases    = PLUGINXX_CAPABILITY_PHASE_MASK_IMPL;
    cap.phase     = PLUGINXX_CAPABILITY_PHASE_IMPL;
    cap.priority  = opts.priority;
    cap.timeoutMs = opts.timeoutMs;
    cap.ownerTag  = opts.ownerTag;
    cap.scope     = ref.empty() ? nullptr : ref.ptr();
    co_return co_await pluginxx::capabilityAsync(
        ctx,
        pointId,
        detail::PointImplAdapter<Ctx, std::decay_t<Fn>>{std::forward<Fn>(fn)},
        cap
    );
}

/// ==================== (kit::tool / point / capability)
/// ====================

namespace detail {

/// 工具实现体的返回值 -> 结果正文 + 元数据 + 终态
/// - `std::string` / `std::string_view` / 字符串字面量: 成功, 文本照搬, 无元数据
/// - `agentxx::util::ToolResult`: 状态与正文照搬; 附件进 `outMetadata` (JSON 串)
///
/// 两段分开是为了不让正文进 JSON: 正文可能是本地代码页的原始字节,
/// JSON 序列化会把非法 UTF-8 换成 U+FFFD (见 `plugin_api.h` 的说明)。
///
/// - `args`:
///     - [value] 实现体返回的值
///     - [outResult] 写入的结果正文 (原样)
///     - [outMetadata] 写入的元数据 JSON (无元数据为空串)
///     - [outStatus] 写入的终态
/// - 相关: 上报映射见 [toolStatusOfOperator]
template<typename T>
inline void toolBodyResultTo(
    T&&          value,
    std::string& outResult,
    std::string& outMetadata,
    ToolStatus&  outStatus
) {
    if constexpr (std::is_same_v<std::decay_t<T>, ToolResult>) {
        outStatus   = value.status;
        outMetadata = value.metadataPayloadJson();
        outResult   = std::move(value.text);
    } else {
        outStatus = ToolStatus::Ok;
        outResult = std::string(std::forward<T>(value));
        outMetadata.clear();
    }
}

/// 工具终态 -> 插件通知状态 (插件面只有成功/失败/取消三态)
/// - `Interrupted` 没有对应值: 插件工具不产生中断, 按失败上报
inline int32_t toolStatusOfOperator(ToolStatus status) noexcept {
    switch (status) {
        case ToolStatus::Ok:
            return PLUGINXX_OPERATOR_OK;
        case ToolStatus::Canceled:
            return PLUGINXX_OPERATOR_CANCELED;
        case ToolStatus::Failed:
        case ToolStatus::Interrupted:
        default:
            return PLUGINXX_OPERATOR_FAILED;
    }
}

/// 只有正文的单段通知器 (插件框架的根只能传一段)
///
/// 用到它的地方: 只要正文的根 (受控轮询工具的失败/取消路径)。
/// 这里只用主程序写进通知器里的函数指针与 `host_ud`, 不引用主程序符号
/// (插件不链接 libagentxx)。
inline PluginxxOperatorNotify textOnlyNotify(const AgentxxPluginToolNotify& tool) noexcept {
    return PluginxxOperatorNotify{tool.done_text, tool.host_ud};
}

/// 整帧通知器: 两段由插件侧拼成帧后经单一字符串通道上报 (受控轮询工具用)
inline PluginxxOperatorNotify frameNotify(const AgentxxPluginToolNotify& tool) noexcept {
    return PluginxxOperatorNotify{tool.done_frame, tool.host_ud};
}

/// 返回值是否 `asio::awaitable<T>` (asio 协程体)
template<typename T>
struct IsAwaitableToolResult : std::false_type {};

template<typename T>
struct IsAwaitableToolResult<asio::awaitable<T>> : std::true_type {};

/// 工具体的可调用形状 (按顺序试第一个成立的; 与旧同步体一致, 便于逐个迁移)
///
/// 1. `(Ctx&, args, OpCtl&)` / `(Ctx&, args, OpCtl)`;
/// 2. `(Ctx&, args, tid, workDir, 取消令牌)`;
/// 3. `(Ctx&, args, tid, 取消令牌)`;
/// 4. `(Ctx&, args, tid, workDir)`;
/// 5. `(Ctx&, args, 取消令牌)`;
/// 6. `(args, 取消令牌)`;
/// 7. `(Ctx&, args, tid)`;
/// 8. `(Ctx&, args)`;
/// 9. `(args)`。
///
/// 返回类型原样转发: `asio::awaitable<T>` (asio 协程体)。
template<typename Fn, typename Ctx>
inline decltype(auto) invokeToolBody(
    Fn&                        fn,
    Ctx&                       ctx,
    std::string_view           args,
    std::string_view           tid,
    std::string_view           workDir,
    OpCtl&                     ctl,
    const PluginxxCancelToken* token
) {
    if constexpr (std::is_invocable_v<Fn&, Ctx&, std::string_view, OpCtl&>) {
        return fn(ctx, args, ctl);
    } else if constexpr (std::is_invocable_v<Fn&, Ctx&, std::string_view, OpCtl>) {
        return fn(ctx, args, ctl);
    } else if constexpr (std::is_invocable_v<
                             Fn&,
                             Ctx&,
                             std::string_view,
                             std::string_view,
                             std::string_view,
                             const PluginxxCancelToken*>) {
        return fn(ctx, args, tid, workDir, token);
    } else if constexpr (std::is_invocable_v<
                             Fn&,
                             Ctx&,
                             std::string_view,
                             std::string_view,
                             const PluginxxCancelToken*>) {
        return fn(ctx, args, tid, token);
    } else if constexpr (std::is_invocable_v<
                             Fn&,
                             Ctx&,
                             std::string_view,
                             std::string_view,
                             std::string_view>) {
        return fn(ctx, args, tid, workDir);
    } else if constexpr (std::is_invocable_v<
                             Fn&,
                             Ctx&,
                             std::string_view,
                             const PluginxxCancelToken*>) {
        return fn(ctx, args, token);
    } else if constexpr (std::is_invocable_v<Fn&, std::string_view, const PluginxxCancelToken*>) {
        return fn(args, token);
    } else if constexpr (std::is_invocable_v<Fn&, Ctx&, std::string_view, std::string_view>) {
        return fn(ctx, args, tid);
    } else if constexpr (std::is_invocable_v<Fn&, Ctx&, std::string_view>) {
        return fn(ctx, args);
    } else {
        return fn(args);
    }
}

/// 工具注册的底层入口 (与 [toolAsync] 共用同一份 spec, 结果经 asio 协程交回)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> registerToolAwaitable(
    Ctx&                          ctx,
    const ScopeRef&               agent,
    const AgentxxPluginToolSpec&  spec
) {
    co_return co_await tableOpAwaitAsync(
        ctx,
        [&ctx, ref = agent, spec](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.iface.tools || !ctx.iface.tools->register_tool) {
                return nullptr;
            }
            auto r = ref.raw();
            return ctx.iface.tools->register_tool(ctx.host, &r, &spec, cb, ud, err);
        }
    );
}




/* ==================== 受控轮询实现 (tool 的 asio 协程体走这条) ====================
 *
 * 适用: 业务体是 **asio 协程**、等待的是**插件本地 reactor 上的内核就绪事件**
 * (socket 收发 / 子进程管道 / 文件 IO / 本地 steady_timer) 的工具。
 *
 * 为什么需要它: 这类等待没有"主程序可见唤醒源" —— driver 里的 `poll_one` 只会执行
 * 已经就绪的 handler, 不会让私有 reactor 的等待对象到期。因此插件在注册时就
 * **声明**"该工具需要受控轮询驱动", 桥据此在有未完成的操作时继续申请请求:
 * - 有进展 (本轮 `poll_one` 执行到了 handler) → 立即续下一次请求;
 * - 无进展 → 退避 `PollOneBridge::kPollIntervalMs` (主程序计时器) 后再驱动;
 * - 连续有进展超过 `PollOneBridge::kPollBurstMax` 步 → 强制让出一次, 交给主程序;
 * - **没有未完成的 polled 操作时不申请请求、不建定时器** (空闲零开销)。
 *
 * 业务签名固定为 5 参数形态 (返回 `asio::awaitable<std::string>`
 * 或 `asio::awaitable<ToolResult>` —— 后者可以返回失败/取消):
 * ```cpp
 * co_await toolAsync(ctx, ref, name, depict, schema,
 *     [](Ctx& c, std::string_view args, std::string_view tid, std::string_view workDir,
 *        const PluginxxCancelToken* cancel) -> asio::awaitable<std::string> {
 *         ArgReader reader(args);
 *         co_return co_await doReadAsync(reader.raw(), workDir);
 *     });
 * ```
 *
 * 约束 (与实现一起遵守):
 * - **不要**在 polled 协程里做重 CPU/阻塞工作: 它运行在主程序 IO 线程上
 *   (目录遍历 / 全文件扫描 / 向量相似度这类用 `onWorkerAsync` 包一层, 交给工作池);
 * - **不要**在 polled 协程里同步等任何东西 (表调用一律 `co_await` 本节后面的
 *   asio 形态入口); 需要等一段时间时用 asio 原生 `steady_timer` (pump 下可用),
 *   或经 `bridge().local_executor()` 投递后续步骤;
 */

/// 受控轮询工具的 Job 侧取消查询 (kit 自造的会话级令牌, 不是主程序 token):
/// 只读 Job 的取消标志与实例 CancelRegistry, 生命周期与该 polled 根相同。
/// 模板参数是 `registerToolWithAwaitableBody` 内的局部 Job 类型 (在其完整之后实例化)。
template<typename Job>
inline int32_t PLUGINXX_CALL polledJobTokenIsRequestedAbi(const PluginxxCancelToken* token) {
    auto* job = (token && token->host_ud) ? static_cast<const Job*>(token->host_ud) : nullptr;
    return (job && job->Canceled()) ? 1 : 0;
}

/// 受控轮询工具的业务体包装 (两条路径共用): 运行业务协程并把终态写回 Job。
///
/// - 异常映射: `CanceledException` → Canceled, 其它异常 → FAILED;
/// - 业务体正常返回但期间已请求取消 → Canceled (payload 为空, 与
///   kit 协程体的取消语义一致);
/// - 本协程自身不决定"由谁上报/释放 Job": 由 [runPolledPumpJob] 收尾。
template<typename Job>
inline asio::awaitable<void> runPolledToolBody(Job* job) {
    try {
        if (job->Canceled()) {
            throw CanceledException("polled tool Canceled before execution");
        }
        // 业务体按可调用形状分发 (与 kit 协程体一致): 拿得到 OpCtl / 会话键 / 工作目录
        // 与取消令牌, 也允许只收其中一部分。
        OpCtl bodyCtl{job->cancelFlag, &job->shim->ctx->cancelRegistry, job->tid, job->shim->agent};
        auto  bodyResult = co_await detail::invokeToolBody(
            job->shim->fn,
            *job->shim->ctx,
            job->args,
            job->tid,
            job->workDir,
            bodyCtl,
            &job->token
        );
        // 实现体返回 `std::string` (成功) 或 `ToolResult` (带状态/附件)
        auto bodyStatus = ToolStatus::Ok;
        toolBodyResultTo(
            std::move(bodyResult),
            job->payload,
            job->resultMetadata,
            bodyStatus
        );
        job->status = toolStatusOfOperator(bodyStatus);
    } catch (const CanceledException& e) {
        job->status  = PLUGINXX_OPERATOR_CANCELED;
        job->payload = e.what();
    } catch (const boost::system::system_error& e) {
        // 协程被取消时, 业务体里正在等的 asio 操作会以 `operation_aborted` 结束。
        // 这是取消而不是失败 (业务体不捕获也能得到 CANCELED 终态)。
        if (e.code() == asio::error::operation_aborted) {
            job->status = PLUGINXX_OPERATOR_CANCELED;
            job->payload.clear();
            job->resultMetadata.clear();
        } else {
            job->status  = PLUGINXX_OPERATOR_FAILED;
            job->payload = e.what();
        }
    } catch (const std::exception& e) {
        job->status  = PLUGINXX_OPERATOR_FAILED;
        job->payload = e.what();
    } catch (...) {
        job->status  = PLUGINXX_OPERATOR_FAILED;
        job->payload = "unknown polled tool error";
    }
    if (job->status == PLUGINXX_OPERATOR_OK && job->Canceled()) {
        job->status = PLUGINXX_OPERATOR_CANCELED;
        job->payload.clear();
        job->resultMetadata.clear();
    }
}

/// 受控轮询工具的业务体包装 (泵路径): 在桥的本地执行器上跑完业务协程, 然后
/// 上报终态并回收 Job (泵路径下 Job 由本协程回收)。
///
/// 停止/关闭路径 (桥 `stop` → [PollOneBridge::failAllPolledRoots]) 可能先一步
/// 认领完成权: 那时 `claimFinish()` 返回 false, 本协程只做退出。
///
/// Job 与取消信号**按值进帧**: 两者都可能被清理路径提前释放, 而帧恢复后还要读它们
/// (以及 asio 结算内层等待时要清一次取消槽), 只存指针就是 use-after-free。
template<typename Job>
inline asio::awaitable<void> runPolledPumpJob(
    std::shared_ptr<Job>                       jobKeep,
    std::shared_ptr<asio::cancellation_signal> signal
) {
    // `signal` 只为在帧里持一份: 协程的取消槽指向该信号内部, 见上面的说明。
    (void)signal;
    Job* job = jobKeep.get();
    co_await runPolledToolBody(job);
    // 先取本地副本: 收尾 (cleanup) 会释放 Job 本身。
    auto          root    = job->root;
    auto*         bridge  = job->bridge;
    const int32_t status  = job->status;
    // 两段在这里拼成帧 (见 `agentxx/plugin/tool_result_frame.h`);
    // 用本地变量: `runCleanup` 释放 Job 之后仍要拿它上报。
    const std::string payload = agentxx::plugin::encodeToolResultFrame(
        job->payload,
        job->resultMetadata
    );
    if (!root || !root->claimFinish()) {
        co_return;
    }
    if (bridge) {
        bridge->removePolledRoot(root);
    }
    root->notifyHost(status, payload);
    // cleanup 释放 Job (幂等); 本地 `root` 保证 PolledRoot 自身活到本语句之后。
    root->runCleanup();
}

/// 注册 asio 协程体工具 (`asio::awaitable<T>` 体) —— 所有工具都走这一条
///
/// 业务体等的是内核就绪对象时 (子进程管道 / 异步文件 IO / 本地 timer): 这类等待没有
/// 主程序可见的唤醒源, 由桥以受控轮询推进 (有操作在跑才申请驱动、空闲零开销)。
/// 纯阻塞工作 (文件读写 / 目录扫描 / 子进程等) 写 `onWorkerAsync` 包装的体, 交给工作池。
template<typename Ctx, typename PolledFn>
inline AgentxxPluginToolSpec buildToolSpecWithAwaitableBody(
    Ctx&             ctx,
    const ScopeRef&  agent,
    std::string_view name,
    std::string_view depict,
    std::string_view schema,
    PolledFn&&       fn,
    int64_t          default_timeout_ms = 0,
    int32_t          flags              = 0
) {
    using PolledFnType = std::decay_t<PolledFn>;
    static_assert(
        detail::IsAwaitableToolResult<std::decay_t<decltype(detail::invokeToolBody(
            std::declval<PolledFnType&>(),
            std::declval<Ctx&>(),
            std::string_view{},
            std::string_view{},
            std::string_view{},
            std::declval<OpCtl&>(),
            std::declval<const PluginxxCancelToken*>()
        ))>>::value,
        "asio 协程体工具必须返回 asio::awaitable<std::string> 或 "
        "asio::awaitable<ToolResult> (阻塞工作写 onWorkerAsync)"
    );

    auto& storage = ctx.storage_;
    // 工具描述用插件自己的字面量: 注册时不再回读主程序的提示词表 —— 那是一次表调用,
    // 插件执行线程上不允许等它 (协作方式: 需要按表里的文本注册就先 co_await
    // toolPromptOf, 再把取到的描述传进来)
    std::string finalDepict(depict);
    storage.push_back(std::move(finalDepict));
    storage.push_back(std::string(schema));

    struct PolledShim {
        Ctx*         ctx = nullptr;
        ScopeRef     agent;
        PolledFnType fn;
    };

    auto shim = ctx.storeShim(
        std::make_unique<PolledShim>(PolledShim{&ctx, agent, std::forward<PolledFn>(fn)})
    );

    /// 一次 polled 根操作的拥有型状态。
    /// 生命周期: 业务协程帧 + [detail::runPolledPumpJob] 的收尾;
    /// 回收由 [detail::PolledRoot] 的 cleanup 处理 (停止/关闭时会提前接管)。
    struct Job {
        PolledShim*                         shim   = nullptr;
        detail::PollOneBridge*              bridge = nullptr;
        std::shared_ptr<detail::PolledRoot> root;
        /// 两段结果的通知器 + 元数据 (根只能传单段, 两段在收尾时拼成帧)
        AgentxxPluginToolNotify notify{nullptr, nullptr};
        /// 取消信号: 主程序取消 (execute_cancel) 时触发, 把正在等的表调用撤掉
        std::shared_ptr<asio::cancellation_signal> cancelSignal
            = std::make_shared<asio::cancellation_signal>();
        /// 业务体返回的元数据 JSON (无元数据为空)
        std::string            resultMetadata;
        detail::RootRequest                 request;
        std::string                         tid;     ///< 作用域键 (= request.scopeKey)
        std::string                         args;    ///< 工具参数 JSON
        std::string                         workDir; ///< 会话工作目录 (IO 线程预取)
        std::shared_ptr<std::atomic<bool>>  cancelFlag;
        /// 传给业务体的取消令牌: kit 自造 (is_requested 读取消标志 + CancelRegistry)。
        PluginxxCancelToken token{nullptr, nullptr};
        /// 终态 (由业务体包装写入; 上报方见上)。
        int32_t     status = PLUGINXX_OPERATOR_OK;
        std::string payload; ///< 结果正文

        /// 是否已请求取消 (本实例取消标志 + 会话级 CancelRegistry)。
        bool Canceled() const noexcept {
            if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                return true;
            }
            if (shim && shim->ctx && !tid.empty() && shim->ctx->cancelRegistry.isCanceled(tid)) {
                return true;
            }
            return false;
        }
    };

    // 自造取消令牌的查询函数: 按 host_ud 还原 Job (Job 为局部类型, 因此用
    // 上面的模板实例化; 此处 Job 已完整)。
    constexpr auto kTokenIsRequested = &detail::polledJobTokenIsRequestedAbi<Job>;

    AgentxxPluginToolSpec spec{};
    // 跨边界结构体长度: 0 = 基线布局; 这里按当前布局填写 (见 plugin_spec.h)
    spec.struct_size = sizeof(AgentxxPluginToolSpec);
    spec.name        = PluginStringView::from(name.data(), name.size());
    spec.description = PluginStringView::from(
        storage[storage.size() - 2].data(),
        storage[storage.size() - 2].size()
    );
    spec.parameters_json    = PluginStringView::from(storage.back().data(), storage.back().size());
    spec.user_data          = shim;
    spec.default_timeout_ms = default_timeout_ms;
    spec.flags              = flags;

    spec.execute_start_invoke = [](void*                          user_data,
                                    const AgentxxPluginToolInvoke* invoke,
                                    PluginxxString*                error_out) -> void* {
        auto* s = static_cast<PolledShim*>(user_data);
        const auto* args_json    = invoke ? invoke->args_json : nullptr;
        const auto* thread_id    = invoke ? invoke->session_id : nullptr;
        const auto* tool_call_id = invoke ? invoke->tool_call_id : nullptr;
        const auto* work_dir     = invoke ? invoke->work_dir : nullptr;
        const auto* notify       = invoke ? invoke->notify : nullptr;
        if (!s || !s->ctx) {
            if (error_out) {
                *error_out = PluginString::fromCstr(nullptr, "polled tool: context released");
            }
            return nullptr;
        }
        auto& c = *s->ctx;
        // 拥有型 Request: args/session/tool_call_id 在整个 polled 根期间有效
        // (协程会挂起并继续读取它们)。
        auto request    = detail::toolRootRequest(c.host, args_json, thread_id, tool_call_id);
        auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
        // 会话级取消标记只对"取消发生时正在运行的操作"有效: 新操作开始时先清掉上一次
        // 留下的标记。只靠轮次开始事件清理不够 —— 该事件不经 agent_start 的路径
        // (中断恢复/取消后重跑) 不会触发, 残留标记会让该会话之后的每次调用都被判为
        // 已取消 (命令工具直接被杀, 主程序侧表现为 [Canceled])。
        if (!request.scopeKey.empty()) {
            c.cancelRegistry.clearCanceled(request.scopeKey);
        }

        // Job 由协程帧与本次调用共同持有: 清理路径 (停止/关闭) 可能在业务协程仍挂起时
        // 执行 cleanup, 之后协程恢复还要读 Job 里的结果, 直接 delete 就是
        // use-after-free。这里改为共享持有, 最后一个持有者释放。
        auto  jobKeep    = std::shared_ptr<Job>(new Job());
        auto* job        = jobKeep.get();
        job->shim        = s;
        job->notify      = notify ? *notify : AgentxxPluginToolNotify{nullptr, nullptr};
        job->request     = std::move(request);
        job->tid         = job->request.scopeKey;
        job->args        = job->request.argsJson;
        // 会话工作目录由主程序随调用带入 (不再为它查表)
        job->workDir.assign(
            work_dir && work_dir->data ? work_dir->data : "",
            work_dir ? static_cast<size_t>(work_dir->size) : 0
        );
        job->cancelFlag = cancelFlag;
        job->root       = std::make_shared<detail::PolledRoot>(detail::frameNotify(job->notify));
        job->token      = PluginxxCancelToken{kTokenIsRequested, job};
        {
            // 补充回收: 停止/关闭路径 (failAllPolledRoots) 会执行它; 正常完成时
            // 由各自的上报方先执行, 这里保持幂等。Job 的真正释放交给持有者
            // (协程帧 / [execute_start_invoke] 的局部变量), 这里只做标记。
            job->root->setCleanup([] {});
        }

        auto* bridge = &c.bridge();
        job->bridge  = bridge;
        bridge->addPolledRoot(job->root);
        try {
            // 业务协程跑在**桥的本地执行器**上: 它的 socket/管道/文件等待因此
            // 注册到桥的 reactor, 由受控轮询推进 (首步恒异步, 不在本调用内执行)。
            asio::co_spawn(
                bridge->local_executor(),
                detail::runPolledPumpJob(jobKeep, job->cancelSignal),
                // 取消信号接到业务协程的取消槽: 业务体里正在等的表调用被 op_cancel 掉
                asio::bind_cancellation_slot(job->cancelSignal->slot(), asio::detached)
            );
        } catch (...) {
            // 首步都无法排队: 按**拒绝**处理 (未产生任何主程序可见副作用),
            // 回收 Job 并返回 NULL + error。
            bridge->removePolledRoot(job->root);
            auto keep = job->root;
            job->root->claimFinish();
            job->root->runCleanup();
            if (error_out) {
                *error_out
                    = PluginString::fromCstr(c.host, "polled tool: failed to schedule coroutine");
            }
            return nullptr;
        }
        // co_spawn 已把首步投递到本地执行器, 但 kit 看不到这次投递
        // (不计入 readySteps_), 因此必须显式 wake —— 否则首步不会被驱动。
        bridge->wake();
        return job;
    };

    spec.execute_cancel = [](void* user_data, void* op) {
        (void)user_data;
        if (!op) {
            return;
        }
        auto* job = static_cast<Job*>(op);
        if (job->cancelFlag) {
            job->cancelFlag->store(true, std::memory_order_release);
        }
        if (job->shim && job->shim->ctx) {
            if (!job->tid.empty()) {
                job->shim->ctx->cancelRegistry.cancel(job->tid);
            }
            // 把业务协程正在等的表调用撤掉 (否则 60s 计时器这类等待会拖住关闭)
            if (job->cancelSignal) {
                job->cancelSignal->emit(asio::cancellation_type::terminal);
            }
            // 取消尚未触发的退避定时器, 让插件立刻得到一次驱动 (不必等满一个退避间隔),
            // 从而尽快在阶段边界看到取消并结束根。
            if (job->bridge) {
                job->bridge->kickPumpWait();
            }
        }
    };

    return spec;
}

/// 注册 asio 协程体工具 (asio 形态: 注册请求本身也用 `co_await` 等)
template<typename Ctx, typename AwaitFn>
inline asio::awaitable<pluginxx::TableOpResult> registerToolWithAwaitableBodyAsync(
    Ctx&             ctx,
    const ScopeRef&  agent,
    std::string_view name,
    std::string_view depict,
    std::string_view schema,
    AwaitFn&&        fn,
    int64_t          default_timeout_ms = 0,
    int32_t          flags              = 0
) {
    co_return co_await detail::registerToolAwaitable(
        ctx,
        agent,
        detail::buildToolSpecWithAwaitableBody(
            ctx,
            agent,
            name,
            depict,
            schema,
            std::forward<AwaitFn>(fn),
            default_timeout_ms,
            flags
        )
    );
}


} // namespace detail
/// 把阻塞函数包成 asio 协程体 (**阻塞工作的唯一写法**)
///
/// 工具体只有协程这一种执行模型。需要同步阻塞的活 (文件 IO / 子进程 / 目录扫描)
/// 传给本函数, 它把调用包进 `co_await offloadAsync(...)`: 工作体在主程序工作池
/// 线程上跑, 插件执行线程与 asio 协程都不被占住。
///
/// - 可调用形状与 [toolAsync] 支持的一致 (`(Ctx&, args, tid, workDir, 取消令牌)` 等);
/// - 工作体里**不要**改只有插件执行线程能改的实例状态 (它在工作池线程上跑);
/// - 工作体里不要做表调用 (阻塞体没有协程帧): 要用表结果就把工作体写成协程体;
/// - 取消: 外层取消令牌原样交给工作体 (`pluginxx_cancel_is_requested`)。
///
/// ```cpp
/// co_await toolAsync(ctx, ref, name, depict, schema,
///     agentxx::plugin::onWorkerAsync([](FsPluginCtx& c, std::string_view args) {
///         return doBlockingWork(args);   // 原样写阻塞代码
///     }));
/// ```
template<typename BlockFn>
auto onWorkerAsync(BlockFn&& fn) {
    using FnType = std::decay_t<BlockFn>;
    return [fn = FnType(std::forward<BlockFn>(fn))](
               auto&                      c,
               std::string_view           args,
               std::string_view           tid,
               std::string_view           workDir,
               const PluginxxCancelToken* token
           ) -> asio::awaitable<
               std::decay_t<decltype(detail::invokeToolBody(
                   std::declval<FnType&>(),
                   std::declval<decltype(c)&>(),
                   std::string_view{},
                   std::string_view{},
                   std::string_view{},
                   std::declval<OpCtl&>(),
                   std::declval<const PluginxxCancelToken*>()
               ))>> {
        using Result = std::decay_t<decltype(detail::invokeToolBody(
            fn,
            c,
            args,
            tid,
            workDir,
            std::declval<OpCtl&>(),
            token
        ))>;
        co_return co_await offloadAsync(c, [&](const PluginxxCancelToken* t) -> Result {
            // 阻塞形状的体不拿 OpCtl (那是协程体的取消入口), 这里给一个空对象占位
            OpCtl ctl{};
            return detail::invokeToolBody(fn, c, args, tid, workDir, ctl, token ? token : t);
        });
    };
}


/// 注册一个工具 (**唯一入口**: 注册请求与等待都用 asio 异步操作)
///
/// 工具体只接受 `asio::awaitable<T>` (T 为 `std::string` / `std::string_view` /
/// `ToolResult`); 阻塞工作写 `onWorkerAsync(...)` 包一层。结果是
/// `pluginxx::TableOpResult` (业务失败不抛异常)。
template<typename Ctx, typename ToolFn>
inline asio::awaitable<pluginxx::TableOpResult> toolAsync(
    Ctx&             ctx,
    const ScopeRef&  agent,
    std::string_view name,
    std::string_view depict,
    std::string_view schema,
    ToolFn&&         fn,
    int64_t          default_timeout_ms = 0,
    int32_t          flags              = 0
) {
    co_return co_await detail::registerToolWithAwaitableBodyAsync(
        ctx,
        agent,
        name,
        depict,
        schema,
        std::forward<ToolFn>(fn),
        default_timeout_ms,
        flags
    );
}

namespace detail {

/// 按可调用性选择图节点业务签名：fn(ctx, request, ctl) / fn(ctx, request)。
/// 返回类型原样转发：快同步节点返回 `std::string`（节点输出 JSON），
/// 锚定协程节点返回 `asio::awaitable<T>`（同上，result 为节点输出 JSON）。
template<typename NodeFn, typename Ctx>
inline decltype(auto) invokeGraphNode(NodeFn& fn, Ctx& ctx, const RootRequest& req, OpCtl ctl) {
    if constexpr (std::is_invocable_v<NodeFn, Ctx&, const RootRequest&, OpCtl>) {
        return fn(ctx, req, std::move(ctl));
    } else {
        return fn(ctx, req);
    }
}

} // namespace detail

/// 图节点实例名 (非图节点操作为空)
inline std::string_view nodeNameOf(const RootRequest& req) noexcept {
    return req.extra(detail::kGraphNodeNameKey);
}

/// 图节点 config JSON (缺省 `"{}"`)
inline std::string_view nodeConfigOf(const RootRequest& req) noexcept {
    return req.extra(detail::kGraphNodeConfigKey);
}

/// 图节点 state 只读快照 JSON (缺省 `"{}"`)
inline std::string_view nodeStateOf(const RootRequest& req) noexcept {
    return req.extra(detail::kGraphNodeStateKey);
}

namespace detail {

/// 图节点实现的上下文绑定 (跨边界 user_data)
template<typename Ctx, typename NodeFn>
struct GraphNodeShim {
    Ctx*                 ctx = nullptr;
    ScopeRef             agent;
    std::decay_t<NodeFn> fn;
};

/// 造一个图节点类型登记的跨边界 spec (`run_start` / `run_cancel` 已绑定)
///
/// 业务体两种: 同步字符串 (快节点)、`asio::awaitable<T>`。
template<typename Ctx, typename NodeFn>
inline AgentxxPluginGraphNodeTypeSpec buildGraphNodeSpec(
    Ctx&             ctx,
    const ScopeRef&  agent,
    std::string_view type,
    std::string_view configSchema,
    NodeFn&&         fn
) {
    using Shim = GraphNodeShim<Ctx, NodeFn>;

    auto shim = ctx.storeShim(
        std::make_unique<Shim>(Shim{&ctx, agent, std::forward<NodeFn>(fn)})
    );

    AgentxxPluginGraphNodeTypeSpec spec{};
    spec.struct_size        = sizeof(AgentxxPluginGraphNodeTypeSpec);
    spec.type               = PluginStringView::from(type.data(), type.size());
    spec.config_schema_json = PluginStringView::from(configSchema.data(), configSchema.size());
    spec.user_data          = shim;

    spec.run_start = [](void*                         user_data,
                        const PluginxxStringView*     node_name,
                        const PluginxxStringView*     config_json,
                        const PluginxxStringView*     state_json,
                        const PluginxxStringView*     thread_id,
                        const PluginxxOperatorNotify* notify,
                        PluginxxString*               error_out) -> void* {
        auto* s = static_cast<Shim*>(user_data);
        (void)error_out;
        if (!s || !s->ctx) {
            CompletionGuard guard(notify);
            guard.failed("graph node context released");
            return nullptr;
        }
        // 输入纳入拥有型 Request: 快同步节点在调用期间有效, 异步节点由根
        // 持有到协程真正结束。
        auto request = graphNodeRootRequest(
            s->ctx->host,
            node_name,
            config_json,
            state_json,
            thread_id
        );
        auto  cancelFlag = std::make_shared<std::atomic<bool>>(false);
        OpCtl ctl{cancelFlag, &s->ctx->cancelRegistry, request.scopeKey, s->agent};

        using NodeRet = decltype(invokeGraphNode(s->fn, *s->ctx, request, ctl));
        if constexpr (std::is_void_v<NodeRet> || std::is_convertible_v<NodeRet, std::string_view>) {
            /// 快同步节点：调用返回即完成；异常统一映射为终态。
            CompletionGuard guard(notify);
            try {
                if constexpr (std::is_void_v<NodeRet>) {
                    invokeGraphNode(s->fn, *s->ctx, request, std::move(ctl));
                    guard.ok();
                } else {
                    guard.ok(invokeGraphNode(s->fn, *s->ctx, request, std::move(ctl)));
                }
            } catch (...) {
                guard.fromCurrentException();
            }
            return nullptr;
        } else if constexpr (pluginxx::detail::IsPluginAwaitable<NodeRet>::value) {
            /// asio 协程节点: 起一条 provider 根; 入参与 OpCtl 由根保活。
            struct Hold {
                RootRequest req;
                OpCtl       ctl;
            };
            auto hold  = std::make_shared<Hold>();
            hold->req  = std::move(request);
            hold->ctl  = std::move(ctl);
            auto pr    = pluginxx::detail::startProviderRoot(
                s->ctx->bridge(),
                notify ? *notify : PluginxxOperatorNotify{nullptr, nullptr},
                [s, hold](std::shared_ptr<pluginxx::detail::ProviderRoot> root) -> NodeRet {
                    root->keepAlive = hold;
                    return invokeGraphNode(s->fn, *s->ctx, hold->req, hold->ctl);
                },
                [](auto&& value) {
                    using ValueT = std::decay_t<decltype(value)>;
                    if constexpr (std::is_void_v<ValueT>) {
                        return std::make_pair(int32_t{PLUGINXX_OPERATOR_OK}, std::string{});
                    } else {
                        return std::make_pair(
                            int32_t{PLUGINXX_OPERATOR_OK},
                            std::string{value}
                        );
                    }
                },
                [](std::exception_ptr ep) {
                    std::string why;
                    try {
                        std::rethrow_exception(ep);
                    } catch (const std::exception& e) {
                        why = e.what();
                    } catch (...) {
                        why = "graph node threw a non-standard exception";
                    }
                    return std::make_pair(int32_t{PLUGINXX_OPERATOR_FAILED}, why);
                }
            );
            return pr.get();
        } else {
            /// 返回类型对不上: 同步节点给 `std::string`, 异步节点给 `asio::awaitable`
            static_assert(
                pluginxx::detail::IsPluginAwaitable<NodeRet>::value,
                "graph_node(): 节点体必须返回 std::string (同步) 或 "
                "asio::awaitable<std::string> (异步)"
            );
            CompletionGuard guard(notify);
            guard.failed("graph node returned an unsupported type");
            return nullptr;
        }
    };

    spec.run_cancel = [](void* user_data, void* op) {
        (void)user_data;
        if (!op) {
            return;
        }
        static_cast<pluginxx::detail::CancelableProvider*>(op)->requestCancel();
    };
    return spec;
}

} // namespace detail

/// 注册插件自定义图节点类型 (**唯一入口**: 登记请求与等待都用 asio 异步操作)
///
/// 节点体返回 `std::string` (同步) 或 `asio::awaitable<std::string>` (异步)。
template<typename Ctx, typename NodeFn>
inline asio::awaitable<pluginxx::TableOpResult> graphNodeAsync(
    Ctx&             ctx,
    const ScopeRef&  agent,
    std::string_view type,
    std::string_view configSchema,
    NodeFn&&         fn
) {
    if (!ctx.iface.graph || !ctx.iface.graph->register_node_type) {
        pluginxx::TableOpResult r;
        r.startError = "plugin graph iface unavailable";
        co_return r;
    }
    const auto spec = detail::buildGraphNodeSpec(
        ctx,
        agent,
        type,
        configSchema,
        std::forward<NodeFn>(fn)
    );
    auto ref = agent;
    co_return co_await pluginxx::asyncTableOp(
        ctx,
        co_await asio::this_coro::executor,
        [&ctx, ref, spec](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            auto r = ref.raw();
            return ctx.iface.graph->register_node_type(ctx.host, &r, &spec, cb, ud, err);
        }
    );
}

/* ==================== 工具权限声明 ====================
 *
 * 工具权限限制由工具来源方 (插件) 在注册工具后声明: 声明"哪些参数是受约束
 * 目标"以及读/写作用域; 具体判定 (白/黑名单、permission.mode 默认规则、
 * 用户记住的选择、工作区隔离、完全授权) 由主程序权限中间件统一执行, 插件
 * 不参与判定。
 *
 * 典型用法 (文件系统类工具):
 *     co_await toolAsync(ctx, ref, "my_read", depict, schema, fn);
 *     co_await registerReadPathPermissionAsync(ctx, ref, "my_read", "path");
 *
 * 权限声明属于附加能力: 主程序未加载权限中间件时返回非 0, 插件可忽略 (工具
 * 照常可用, 只是不参与权限判定)。
 */

/// 工具权限作用域 (读/写各自一套规则, 互不影响)
enum class PermissionScope : int32_t {
    Read  = AGENTXX_PLUGIN_PERMISSION_SCOPE_READ,
    Write = AGENTXX_PLUGIN_PERMISSION_SCOPE_WRITE,
};

/// 工具权限目标来源
enum class PermissionTarget : int32_t {
    None = AGENTXX_PLUGIN_PERMISSION_TARGET_NONE,
    Path = AGENTXX_PLUGIN_PERMISSION_TARGET_PATH,
    Text = AGENTXX_PLUGIN_PERMISSION_TARGET_TEXT,
};

/// 工具权限声明 (字段含义见 C ABI 的 AgentxxPluginToolPermissionSpec)
struct ToolPermissionSpec {
    std::string_view toolName{};                     ///< 目标工具名 (须已注册)
    PermissionScope  scope{PermissionScope::Read};   ///< 权限作用域
    PermissionTarget target{PermissionTarget::None}; ///< 目标来源
    /// 目标参数名 (args 字段名); 目标值按实际 JSON 类型处理: 字符串为单目标,
    /// 数组 (如 `file_patterns`) 逐项判定
    std::string_view targetArg{};
    std::string_view category{}; ///< 权限分类文本 (空 = 按作用域生成)
};

/* ---- 工具权限声明 (asio 形态) ---- */

/// 声明一条工具权限 (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> registerToolPermissionAsync(
    Ctx&                      ctx,
    const ScopeRef&           agent,
    const ToolPermissionSpec& spec
) {
    if (spec.toolName.empty()) {
        co_return detail::tableOpUnsupported();
    }
    AgentxxPluginToolPermissionSpec abiSpec{};
    abiSpec.tool_name   = PluginStringView::from(spec.toolName);
    abiSpec.scope       = static_cast<int32_t>(spec.scope);
    abiSpec.target_kind = static_cast<int32_t>(spec.target);
    abiSpec.target_arg  = PluginStringView::from(spec.targetArg);
    abiSpec.struct_size = sizeof(AgentxxPluginToolPermissionSpec);
    abiSpec.category    = PluginStringView::from(spec.category);
    co_return co_await detail::tableOpAwaitAsync(
        ctx,
        [&ctx, agent, abiSpec](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            if (!ctx.host || !ctx.iface.permission
                || !ctx.iface.permission->register_tool_permission) {
                return nullptr;
            }
            auto ref = agent.raw();
            return ctx.iface.permission->register_tool_permission(
                ctx.host,
                &ref,
                &abiSpec,
                cb,
                ud,
                err
            );
        }
    );
}

/// 声明"路径参数 + 读作用域" (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> registerReadPathPermissionAsync(
    Ctx&             ctx,
    const ScopeRef&  agent,
    std::string_view toolName,
    std::string_view pathArg
) {
    ToolPermissionSpec spec;
    spec.toolName  = toolName;
    spec.scope     = PermissionScope::Read;
    spec.target    = PermissionTarget::Path;
    spec.targetArg = pathArg;
    co_return co_await registerToolPermissionAsync(ctx, agent, spec);
}

/// 声明"路径参数 + 写作用域" (asio 形态)
template<typename Ctx>
inline asio::awaitable<pluginxx::TableOpResult> registerWritePathPermissionAsync(
    Ctx&             ctx,
    const ScopeRef&  agent,
    std::string_view toolName,
    std::string_view pathArg
) {
    ToolPermissionSpec spec;
    spec.toolName  = toolName;
    spec.scope     = PermissionScope::Write;
    spec.target    = PermissionTarget::Path;
    spec.targetArg = pathArg;
    co_return co_await registerToolPermissionAsync(ctx, agent, spec);
}


/// 撤销工具权限声明 (工具注销/插件禁用卸载时由主程序自动撤销, 一般无需手动调用)
template<typename Ctx>
inline int32_t
    unregisterToolPermission(const Ctx& ctx, const ScopeRef& agent, std::string_view toolName) {
    if (!ctx.host || !ctx.iface.permission || !ctx.iface.permission->unregister_tool_permission) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    if (toolName.empty()) {
        return AGENTXX_RESULT_ERR_ARG;
    }
    auto ref    = agent.raw();
    auto nameSv = PluginStringView::from(toolName);
    return ctx.iface.permission->unregister_tool_permission(ctx.host, &ref, &nameSv);
}

/* ---- 路径权限查询 (供模式/前缀参数工具逐项过滤) ----
 *
 * 用途: 声明式权限目标只能描述"扫描起点"(决定是否询问用户一次); 对 glob/grep
 * 这类模式参数, 模式展开后可能触及被拒绝的子目录, 因此工具在枚举出实际路径后
 * 还要逐项查询一次, 只处理已明确允许的路径。
 *
 * 判定三态 (见 PathDecision): 与工具调用权限检查同一套规则, 只是"需要询问"的场合
 * 返回 Ask 而**不发起任何询问/中断** (本查询纯只读)。
 * 工具侧约定: Deny 丢弃; Ask 表示"未获批准", 同样不应访问 (fail-closed)。
 */

/// 路径权限判定结果 (与 C ABI AGENTXX_PLUGIN_PERMISSION_DECISION_* 对应)
enum class PathDecision : int32_t {
    Deny  = AGENTXX_PLUGIN_PERMISSION_DECISION_DENY,  ///< 已明确拒绝
    Allow = AGENTXX_PLUGIN_PERMISSION_DECISION_ALLOW, ///< 已明确允许
    Ask   = AGENTXX_PLUGIN_PERMISSION_DECISION_ASK,   ///< 未获批准 (本查询不询问)
};

/* ---- 路径权限查询的协程形态 (asio) ----
 *
 * 阻塞体不能做表调用 (表调用只有协程形态), 所以模式类工具与写复核都要写成
 * `asio::awaitable` 体: 阻塞的枚举/文件操作经 `offloadAsync` 交给工作池,
 * 权限查询本身在协程里 `co_await` 下面这两个入口。
 */

/// 批量查询路径权限 (**协程形态**; 不发起询问/不产生中断; 失败返回空数组)
template<typename Ctx>
inline asio::awaitable<std::vector<PathDecision>> checkPathDecisionsAsync(
    Ctx&                     ctx,
    ScopeRef                 agent,
    PermissionScope          scope,
    std::string              sessionId,
    std::vector<std::string> paths
) {
    // 单次批量上限与主程序一致 (见 plugin_manager_vtable.cpp); 超出由调用方分批
    constexpr size_t kMaxBatch = 16384;
    if (!ctx.host || !ctx.iface.permission || !ctx.iface.permission->check_paths || paths.empty()
        || paths.size() > kMaxBatch) {
        co_return std::vector<PathDecision>{};
    }
    const size_t expected = paths.size();
    auto         r        = co_await tableOpAsync(
        ctx,
        [&ctx,
         agent     = std::move(agent),
         scope,
         sessionId = std::move(sessionId),
         paths     = std::move(paths)](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            auto                            ref = agent.raw();
            std::vector<PluginxxStringView> views;
            views.reserve(paths.size());
            for (const auto& path : paths) {
                views.push_back(PluginStringView::from(path));
            }
            AgentxxPluginPermissionPathQuery query{};
            query.struct_size = sizeof(AgentxxPluginPermissionPathQuery);
            query.scope       = static_cast<int32_t>(scope);
            query.path_count  = static_cast<int32_t>(views.size());
            query.session_id  = PluginStringView::from(sessionId);
            query.paths       = views.data();
            return ctx.iface.permission->check_paths(ctx.host, &ref, &query, cb, ud, err);
        }
    );
    std::vector<PathDecision> out;
    if (!r.ok()) {
        co_return out;
    }
    // 完成时 payload 是判定数组的 JSON 文本 (与入参 paths 等长、顺序一致)
    try {
        auto arr = utilxx_base::Json::parse(std::string(r.text()));
        if (arr.is_array()) {
            out.reserve(arr.size());
            for (const auto& item : arr) {
                if (item.is_number_integer()) {
                    out.push_back(static_cast<PathDecision>(item.get<int>()));
                }
            }
        }
    } catch (...) {
        co_return std::vector<PathDecision>{};
    }
    if (out.size() != expected) {
        co_return std::vector<PathDecision>{};
    }
    co_return out;
}

/// 批量**过滤**路径 (asio 形态): 返回与 [paths] 等长的标记数组 (1 = 已明确允许)
///
/// - Deny 与 Ask 都标记为不可访问 (fail-closed);
/// - 主程序不支持查询时返回空数组, 调用方应跳过过滤;
/// - 内部按 [batchSize] 分批 (默认 512 项) 并对相同路径去重。
template<typename Ctx>
inline asio::awaitable<std::vector<uint8_t>> filterPathPermissionsAsync(
    Ctx&                     ctx,
    ScopeRef                 agent,
    PermissionScope          scope,
    std::string              sessionId,
    std::vector<std::string> paths,
    size_t                   batchSize = 512
) {
    if (!ctx.host || !ctx.iface.permission || !ctx.iface.permission->check_paths || paths.empty()) {
        co_return std::vector<uint8_t>{};
    }
    if (batchSize == 0) {
        batchSize = 512;
    }
    std::vector<uint8_t>                     allowed(paths.size(), 0);
    std::unordered_map<std::string, uint8_t> queried;
    std::vector<std::string>                 pending;
    std::vector<size_t>                      pendingIndex;

    auto flush = [&]() -> asio::awaitable<bool> {
        if (pending.empty()) {
            co_return true;
        }
        auto batch = co_await checkPathDecisionsAsync(ctx, agent, scope, sessionId, pending);
        if (batch.size() != pending.size()) {
            co_return false; // 查询失败: 交由调用方跳过过滤
        }
        for (size_t i = 0; i < batch.size(); ++i) {
            const uint8_t ok         = batch[i] == PathDecision::Allow ? 1 : 0;
            allowed[pendingIndex[i]] = ok;
            queried.insert_or_assign(pending[i], ok);
        }
        pending.clear();
        pendingIndex.clear();
        co_return true;
    };

    for (size_t i = 0; i < paths.size(); ++i) {
        auto it = queried.find(paths[i]);
        if (it != queried.end()) {
            allowed[i] = it->second;
            continue;
        }
        pending.push_back(paths[i]);
        pendingIndex.push_back(i);
        if (pending.size() >= batchSize) {
            if (!co_await flush()) {
                co_return std::vector<uint8_t>{};
            }
        }
    }
    if (!co_await flush()) {
        co_return std::vector<uint8_t>{};
    }
    co_return allowed;
}


/// Agent 侧插件入口导出
/// (生成 `agentxx_agent_plugin_{get_info,create,stop,destroy,mount,unmount}`)。
///
/// 插件框架提供带符号前缀的通用导出宏 (见 `pluginxx/kit/kit.h` 的
/// [PLUGINXX_EXPORT_PLUGIN]); 入口符号名属于本主程序命名空间, 因此在这里包一层。
/// 运行时由 PluginManager::entrySymbols() 交出同一批符号名。
///
/// - `CreateFn` / `StopFn`: 进程级 (一次), 形如
///   `void(CtxType&, const PluginxxOperatorNotify*, PluginxxString*)`;
///   `CreateFn` 就是注册事务 (旧版的 start), 上下文构造由宏包着的 create 代劳;
/// - `MountFn` / `UnmountFn`: agent 级 (每 agent 一次), 形如
///   `void*(CtxType&, const AgentxxPluginAgentMountSpec*, const PluginxxOperatorNotify*,
///    PluginxxString*)` / `void(CtxType&, void* mount_ud, const PluginxxOperatorNotify*,
///    PluginxxString*)`;
///   **没有 per-agent 状态的插件传 `nullptr, nullptr`**
#define AGENTXX_AGENT_PLUGIN_EXPORT(                                                \
    CtxType, Name, Ver, Desc, CreateFn, StopFn, MountFn, UnmountFn                  \
)                                                                                  \
    PLUGINXX_EXPORT_PLUGIN(agentxx_agent_plugin_, CtxType, Name, Ver, Desc, CreateFn, StopFn) \
    AGENTXX_AGENT_PLUGIN_MOUNT_EXPORT(CtxType, MountFn, UnmountFn)

/// 只生成挂载/卸载两个入口 (与 [AGENTXX_AGENT_PLUGIN_LIFECYCLE_EXPORT] 配套)
///
/// - mount: 同步完成时先 `notify->done(OK)` 再返回 per-agent 状态;
/// - unmount: `void` + 完成通知。
#define AGENTXX_AGENT_PLUGIN_MOUNT_EXPORT(CtxType, MountFn, UnmountFn)                         \
    extern "C" PLUGINXX_EXPORT void* PLUGINXX_CALL agentxx_agent_plugin_mount(                \
        void*                              plugin_ctx,                                       \
        const AgentxxPluginAgentMountSpec* spec,                                             \
        const PluginxxOperatorNotify*      notify,                                           \
        PluginxxString*                    err                                               \
    ) {                                                                                      \
        auto mountFn = static_cast<                                                          \
            void* (*)(                                                                       \
                CtxType&,                                                                    \
                const AgentxxPluginAgentMountSpec*,                                          \
                const PluginxxOperatorNotify*,                                               \
                PluginxxString*                                                              \
            )>(MountFn);                                                                      \
        if (!mountFn) {                                                                       \
            /* 无挂载入口 = 本插件无 agent 侧能力, 仍要给出完成信号 */                           \
            if (notify && notify->done) {                                                     \
                notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);                 \
            }                                                                                 \
            return nullptr;                                                                   \
        }                                                                                     \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                        \
        return agentxx::plugin::detail::callLifecycleEntry(                                   \
            ctx ? ctx->host : nullptr,                                                        \
            plugin_ctx,                                                                       \
            notify,                                                                           \
            err,                                                                              \
            "plugin mount",                                                                   \
            [&]() -> void* { return mountFn(*ctx, spec, notify, err); }                        \
        );                                                                                    \
    }                                                                                         \
    extern "C" PLUGINXX_EXPORT void PLUGINXX_CALL agentxx_agent_plugin_unmount(               \
        void*                         plugin_ctx,                                             \
        void*                         mount_ud,                                               \
        const PluginxxOperatorNotify* notify,                                                 \
        PluginxxString*               err                                                     \
    ) {                                                                                       \
        auto unmountFn = static_cast<                                                         \
            void (*)(CtxType&, void*, const PluginxxOperatorNotify*, PluginxxString*)>(       \
            UnmountFn                                                                         \
        );                                                                                    \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                        \
        pluginxx::detail::callLifecycleEntryVoid(                                             \
            ctx ? ctx->host : nullptr,                                                        \
            plugin_ctx,                                                                       \
            notify,                                                                           \
            err,                                                                              \
            "plugin unmount",                                                                 \
            [&]() {                                                                           \
                if (unmountFn) {                                                              \
                    unmountFn(*ctx, mount_ud, notify, err);                                   \
                } else {                                                                      \
                    /* 无挂载状态 = 没有要撤销的东西, 仍要给出完成信号 */                          \
                    if (notify && notify->done) {                                             \
                        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);         \
                    }                                                                         \
                }                                                                             \
            }                                                                                 \
        );                                                                                    \
    }

/// Agent 侧只导出 stop/mount/unmount 三个入口 (供手写 create/destroy 的插件使用)。
///
/// 与 [AGENTXX_AGENT_PLUGIN_EXPORT] 的生命周期部分同语义, 但由插件自己实现
/// `agentxx_agent_plugin_create` / `agentxx_agent_plugin_destroy`。
#define AGENTXX_AGENT_PLUGIN_LIFECYCLE_EXPORT(CtxType, StopFn, MountFn, UnmountFn) \
    PLUGINXX_EXPORT_PLUGIN_LIFECYCLE(agentxx_agent_plugin_, CtxType, StopFn)       \
    AGENTXX_AGENT_PLUGIN_MOUNT_EXPORT(CtxType, MountFn, UnmountFn)
} // namespace plugin
} // namespace agentxx
