#pragma once

/// 插件网络入口: chatbot 的 HTTP / WebSocket 全部经主程序的 `<应用名>.net` 表
///
/// 为什么不自己建连接:
/// - 连接、TLS、超时、取消都由主程序统一管 (`utilxx::NetHub`), 插件只描述"要什么";
/// - 资源按插件实例记账, 插件被禁用/卸载时主程序把它名下的连接一起收掉,
///   不会留下僵住的 socket;
/// - 网络事件由主程序发到 `plugin.<插件 id>.net` 频道, 插件订阅即可。
///
/// 线程: 本结构体不带线程。里面的方法都要在**插件 io 线程**上调用
/// (`QqApi` / `QqGateway` 本来就在那条线程上跑), 完成回调则由主程序投递回来。
#include "agentxx/plugin/api/plugin_kit.h"

#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "utilxx_base/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace asio = ::boost::asio;

namespace agentxx {
namespace chatbot {

/// 一次 HTTP 请求的参数 (`ChatbotNet::fetch` 的入参)
struct ChatbotHttpRequest {
    std::string method = "GET";
    std::string body;         ///< 请求体原文 (JSON 文本也走这里)
    std::string bodyBase64;   ///< 二进制请求体 (与 [body] 二选一)
    std::string contentType;  ///< 非空且请求头里没有同名项时补 `Content-Type`
    std::string headersJson;  ///< 请求头 JSON 对象文本 (空 = 没有额外头)
    int32_t     timeoutMs  = 0; ///< <= 0 = 用主程序默认; 这是整次请求的预算
    int64_t     maxBytes   = 0; ///< <= 0 = 不限正文大小
};

/// 一次 HTTP 请求的结果 (与平台无关; 平台层再转成自己的结构)
struct ChatbotHttpResult {
    /// HTTP 状态 (0 = 没走到 HTTP, 见 [error])
    int         status = 0;
    std::string body;
    /// 传输层错误 (连接失败 / 超时 / TLS / 主程序没有网络能力)
    std::string error;

    bool transportOk() const noexcept {
        return error.empty() && status != 0;
    }

    bool ok() const noexcept {
        return transportOk() && status >= 200 && status < 300;
    }

    /// 把 body 当 JSON 解析 (失败返回 nullopt)
    std::optional<utilxx_base::Json> json() const;
};

/// 网络事件回调 (主程序 io 线程; 与广播表的签名一致)
using ChatbotNetEventHandler = void(PLUGINXX_CALL*)(const PluginxxStringView*, void*);

/// 网络入口 (主程序给的几个句柄 + 几个直白方法)
struct ChatbotNet {
    const PluginxxHost*            host       = nullptr;
    const PluginxxNetIface*        net        = nullptr;
    const PluginxxSchedulerIface*  scheduler  = nullptr;
    const PluginxxBroadcastsIface* broadcasts = nullptr;
    /// 本插件的网络事件频道 `plugin.<插件 id>.net` (插件入口填)
    std::string eventTopic;

    /// 同步操作 (WebSocket 建连/发送/关闭) 可用
    bool ready() const noexcept {
        return host != nullptr && net != nullptr && net->call != nullptr;
    }

    /// 异步操作 (HTTP 请求) 可用
    bool asyncReady() const noexcept {
        return host != nullptr && net != nullptr && net->call_async != nullptr;
    }

    /// 一次 HTTP 请求 (协程; 在插件 io 线程上 `co_await`)
    ///
    /// - 完成回调由主程序投递回传进来的执行器, 不会从主程序回调栈里进入插件代码;
    /// - 非 2xx 也算 `transportOk()`: 平台层自己按状态码判断成功与否。
    asio::awaitable<ChatbotHttpResult> fetch(
        asio::any_io_executor      ex,
        std::string                url,
        const ChatbotHttpRequest&  request
    ) const;

    /// 下载到指定路径 (**阻塞**; 只在插件 io 线程上用)
    ///
    /// - 正文由主程序直接写到磁盘 (`http.download`), 不经 JSON, 大文件也不占内存;
    /// - 主程序先落到它给插件的数据目录, 本方法再搬到 `targetPath`
    ///   (同盘就是改名; 跨盘退化成复制 + 删源);
    /// - 返回空串 = 成功, 非空 = 失败原因 (调用方保留平台地址即可)。
    std::string download(
        std::string_view url,
        const std::string& targetPath,
        int64_t            maxBytes,
        int32_t            timeoutMs
    ) const;

    /// 订阅本插件的网络事件 (先声明频道再订阅; 返回退订句柄, 空 = 失败)
    ///
    /// - `handler` 在主程序 io 线程被调用, 里面只应做"拷贝 + post 回插件 io 线程";
    /// - 用 `broadcasts->unsubscribe_broadcast(handle)` 退订。
    PluginxxSubscription* subscribeEvents(ChatbotNetEventHandler handler, void* ud) const;
};

} // namespace chatbot
} // namespace agentxx
