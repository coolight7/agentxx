/// 插件网络能力: 通用网络服务 (`utilxx::NetHub`) 的插件面适配
///
/// 分工:
/// - 通用部分在 `cxx_utilxx` 的 `utilxx::NetHub`: HTTP 客户端/服务端、TCP、WebSocket、
///   UDP, 自有网络线程, 按**属主** (这里是插件实例名) 隔离资源;
/// - 本类只做三件"插件面"的事: 属主 id 用插件实例名、事件发到 `plugin.<插件 id>.net`
///   广播、完成回调映射到 `PluginxxOperatorNotify` 的状态位;
/// - 表入口 (`<应用名>.net`) 与生命周期挂在 `PluginManager` 上 (见 plugin_manager_net.cpp)。
///
/// 线程: 本类的方法可在任意线程调用 (`NetHub` 自己投递到网络线程);
/// 完成回调与事件分发都在调用方线程 (由 [Hooks::postToCaller] 决定, 传空则就地执行)。
#pragma once

#include "pluginxx/api/abi.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace utilxx {
class NetHub;
} // namespace utilxx

namespace agentxx {
namespace plugin {

class PluginNetService {
public:
    /// 与调用方 (插件管理器) 的接口
    struct Hooks {
        /// 发一条网络事件给插件 (`owner` = 插件 id; 由调用方自己投递到它的线程)
        std::function<void(std::string_view owner, std::string_view payloadJson)> publishEvent;

        /// 把函数投递到调用方线程 (完成回调/事件分发用); 空 = 就地执行
        std::function<void(std::function<void()>)> postToCaller;
    };

    /// 建服务并起网络线程
    /// - `dataDir`: 属主私有数据根 (`http.download` 落盘到 `<dataDir>/<owner>/data/downloads/`)
    static std::shared_ptr<PluginNetService>
        create(Hooks hooks, const std::string& dataDir = {});

    ~PluginNetService();
    PluginNetService(const PluginNetService&)            = delete;
    PluginNetService& operator=(const PluginNetService&) = delete;

    /// 同步操作 (返回 `utilxx::net::*` 结果码; 结果 JSON 写进 `outJson`)
    int32_t call(std::string_view owner, std::string_view op, std::string_view argsJson,
                 std::string* outJson);

    /// 异步操作: 受理成功写 `requestId`, 结果恰好一次经 `cb` 给出
    ///
    /// - `guard` 是插件实例的弱引用: 结算时实例已销毁就丢弃这次回调 (按取消处理),
    ///   避免回调落到已卸载的插件代码上;
    /// - 受理失败返回负值, 不回调。
    int32_t callAsync(std::string_view            owner,
                      std::string_view            op,
                      std::string_view            argsJson,
                      PluginxxOperatorCallback    cb,
                      void*                       ud,
                      std::string*                outJson,
                      int64_t*                    outRequestId,
                      std::weak_ptr<void>         guard = {});

    /// 取消一个进行中的请求 (不存在/已完成返回 `utilxx::net::ErrNotFound`)
    int32_t cancel(std::string_view owner, int64_t requestId, std::string* outJson);

    /// 关掉某个插件 (属主) 的全部服务、连接与未完成请求
    ///
    /// 未完成的请求按 "已取消" 结算 (`PLUGINXX_OPERATOR_CANCELED` + 空载荷)。
    /// **在调用方线程调用** (插件卸载/禁用路径)。
    void shutdownOwner(std::string_view owner);

    /// 停掉全部服务与连接 (管理器关闭时)
    void stop();

private:
    PluginNetService();

    struct State;
    std::shared_ptr<State> state_;
    std::shared_ptr<utilxx::NetHub> hub_;
};

} // namespace plugin
} // namespace agentxx
