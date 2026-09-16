#include "agentxx/agent/io/agent_io.h"

#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/middlewares/interrupt_presets.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/middlewares/permission.h"
#include "neograph/graph/cancel.h"

namespace agentxx {
namespace agent {

AgentIOBase::~AgentIOBase() {
    unregisterFromBus();
}

// ---------------------------------------------------------------------------
// Transport 管理
// ---------------------------------------------------------------------------

void AgentIOBase::setTransport(std::shared_ptr<AgentIOTransportBase> transport) {
    transport_ = std::move(transport);
}

std::shared_ptr<AgentIOTransportBase> AgentIOBase::transport() const noexcept {
    return transport_;
}

void AgentIOBase::setEventSink(std::shared_ptr<ClientEventSink> sink) {
    eventSink_ = std::move(sink);
}

asio::awaitable<void> AgentIOBase::runTransportLoop() {
    // 捕获局部 transport 引用: 服务端同一 sessionId 的新连接替换旧连接
    // (AgentServer::serveTransport 中 setTransport) 时, 本协程应继续处理旧
    // transport 直至其关闭自然退出, 而不是跟随成员 transport_ 切换到新
    // transport 上发起第二个接收循环 (消息被两个循环瓜分 / 协程泄漏)
    auto transport = transport_;
    if (!transport) {
        co_return;
    }
    while (transport->alive()) {
        auto msg = co_await transport->recv();
        if (!msg.has_value()) {
            break;
        }
        onPeerMessage(std::move(*msg));
    }
}

void AgentIOBase::sendToPeer(WireMessage msg) {
    if (!transport_) {
        // 端点间通信强制要求 transport; 走到这里说明装配遗漏 (如未 setTransport),
        // 记录错误便于定位, 避免静默丢消息
        XX_LOGE(
            "[io] sendToPeer without transport, message dropped (variant index {})",
            msg.index()
        );
        return;
    }
    transport_->send(std::move(msg));
}

// ---------------------------------------------------------------------------
// 默认命令实现 (经 transport 发送)
// ---------------------------------------------------------------------------

void AgentIOBase::requestCancel(std::string sessionId) {
    sendToPeer(WireCancel{std::move(sessionId)});
}

void AgentIOBase::requestSelectModel(std::string sessionId, std::string model) {
    sendToPeer(WireSelectModel{std::move(sessionId), std::move(model)});
}

void AgentIOBase::requestAppendComponentInfo(std::string sessionId) {
    sendToPeer(WireGetAppendComponentInfo{std::move(sessionId)});
}

void AgentIOBase::requestViewMessagesPage(
    std::string sessionId,
    uint64_t    beforeIndex,
    uint32_t    count
) {
    sendToPeer(WireGetViewMessages{std::move(sessionId), beforeIndex, count});
}

void AgentIOBase::requestSessionListPage(int64_t beforeMs, std::string beforeId, uint32_t count) {
    sendToPeer(WireListSessions{beforeMs, std::move(beforeId), count});
}

void AgentIOBase::sendUserInput(std::string sessionId, std::string text) {
    // 通知事件接收器 (client 插件系统订阅用户输入事件)
    emitEventSink([&](ClientEventSink& sink) {
        sink.onUserInput(sessionId, text);
    });
    sendToPeer(WireUserInput{std::move(sessionId), std::move(text)});
}

void AgentIOBase::onServerReady() {
    // 基类默认实现通知事件接收器 (client 插件系统据此开始注册 UI/订阅事件)
    emitEventSink([&](ClientEventSink& sink) {
        sink.onReady();
    });
}

// ---------------------------------------------------------------------------
// 默认消息分发 (子类覆写以扩展)
// ---------------------------------------------------------------------------

void AgentIOBase::onPeerMessage(WireMessage msg) {
    std::visit(
        [this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, WireDelta>) {
                emitEventSink([&](ClientEventSink& sink) {
                    sink.onDelta(m);
                });
                onDelta(m);
            } else if constexpr (std::is_same_v<T, WireSyncPayload>) {
                onSync(m);
            } else if constexpr (std::is_same_v<T, WireTurnResult>) {
                emitEventSink([&](ClientEventSink& sink) {
                    sink.onTurnResult(m);
                });
                onTurnResult(m);
            } else if constexpr (std::is_same_v<T, WireContextStats>) {
                onContextStats(m);
            } else if constexpr (std::is_same_v<T, WirePluginData>) {
                // 插件事件转发: 通知事件接收器 (client 插件系统据此分发到
                // 订阅 EVT_PLUGIN_DATA 的插件回调)
                emitEventSink([&](ClientEventSink& sink) {
                    sink.onPluginData(m);
                });
            }
        },
        std::move(msg)
    );
}

void AgentIOBase::unregisterFromBus() {
    auto bus = registeredBus_.lock();
    if (!bus) {
        return;
    }
    if (interruptServerId_ != 0) {
        bus->getRR<events::ReqInterrupt, events::RespInterrupt>(events::Topic::Interrupt)
            .unregisterServer(interruptServerId_);
        interruptServerId_ = 0;
    }
    if (permissionServerId_ != 0) {
        bus->getRR<events::ReqPermission, events::RespPermission>(events::Topic::Permission)
            .unregisterServer(permissionServerId_);
        permissionServerId_ = 0;
    }
}

void AgentIOBase::registerOnBus(std::shared_ptr<agentxx::events::EventBus> sessionBus) {
    if (!sessionBus) {
        return;
    }

    // 重复注册时先移除旧处理器, 避免 handler 累积/泄漏, 以及旧 IO 销毁后悬空 this
    unregisterFromBus();
    registeredBus_ = sessionBus;

    // 注册 interrupt 处理器
    // - 取消 (WireCancel → onCancel) 与 HIL 等待互斥: handleInterrupt
    //   被取消信号打断时返回特殊标记 `{"__cancelled__":true}`, 调用方
    //   (AgentRunner) 据此不 resume 而直接抛 CancelledException,
    //   避免"取消 HIL 弹窗后自动以空结果 resume 继续执行"的死循环
    auto& interruptRR
        = sessionBus->getRR<events::ReqInterrupt, events::RespInterrupt>(events::Topic::Interrupt);
    interruptServerId_ = interruptRR.registerServer(
        [this](const events::ReqInterrupt& req, size_t /*corrId*/)
            -> asio::awaitable<events::RespInterrupt> {
            auto result = co_await this->handleInterrupt(
                req.sessionId,
                req.interruptNode,
                req.interruptValue,
                req.interruptArgsJson
            );
            // 取消标记: handleInterrupt 被取消时返回 {"__cancelled__":true},
            // handled=false 使调用方不写回 resume 值 (见 AgentRunner)
            if (result.is_object() && result.value("__cancelled__", false)) {
                co_return events::RespInterrupt{.handled = false, .resultJson = result.dump()};
            }
            // 结果恒为对象形态 {"values": {控件 id: 值}} (见 makeInterruptResult):
            // 图状态 resume 只接收值对象, 故取 values 写回 (消费端按控件 id 取值)
            if (!result.is_object() || !result.contains("values")) {
                // 契约违规 (非对象形态): 按中断未应答处理并告警
                XX_LOGW(
                    "[io] interrupt `{}` result is not an object with values, dropped: {}",
                    req.interruptNode,
                    result.dump()
                );
                co_return events::RespInterrupt{.handled = false, .resultJson = "{}"};
            }
            co_return events::RespInterrupt{
                .handled    = true,
                .resultJson = result["values"].dump(),
            };
        }
    );

    // 注册 permission 处理器
    auto& permRR
        = sessionBus->getRR<events::ReqPermission, events::RespPermission>(events::Topic::Permission
        );
    permissionServerId_ = permRR.registerServer(
        [this](const events::ReqPermission& req, size_t /*corrId*/)
            -> asio::awaitable<events::RespPermission> {
            auto arg     = agentxx::middleware::InterruptHandleArg{};
            arg.name     = "permission";
            arg.resultId = "";
            // 透传权限上下文给客户端 (记住权限选择时使用):
            // - category: 权限分类 ("filesystem_read" / "filesystem_write")
            // - target:   受约束目标 (已标准化的绝对路径, 与中间件规则匹配口径一致)
            arg.arg = agentxx::util::Json{
                {"category", req.category},
                {"target",   req.target  },
            };
            // 中断 UI 描述 (预设模板生成): 权限卡片形态由此描述数据决定, 客户端
            // 不含任何 permission 分支 —— 与普通中断共用同一套通用渲染/交互实现;
            // 结果控件: decision (允许 "true" / 拒绝 "false") + remember (勾选项)
            arg.ui = agentxx::middleware::preset::permissionCard(
                req.toolName,
                req.category,
                req.target
            );

            auto result = co_await this->handleInterrupt(
                req.sessionId,
                "permission",
                req.argumentsJson,
                arg.toJson().dump()
            );

            // 取消透传: HIL 等待被 WireCancel 打断时 handleInterrupt 返回
            // {"__cancelled__":true}, 此处必须抛取消而非判为拒绝,
            // 否则取消权限弹窗会被当成"[Permission denied]"正常继续执行
            if (result.is_object() && result.value("__cancelled__", false)) {
                throw neograph::graph::CancelledException("permission interrupted by cancel");
            }
            // 结果恒为对象形态 {"values": {控件 id: 值}} (见 makeInterruptResult);
            // 非对象形态按"未应答"处理并告警。权限卡片控件: decision + remember + fullAuth
            bool                allowed  = false;
            bool                remember = false;
            bool                fullAuth = false;
            agentxx::util::Json values   = agentxx::util::Json::object();
            if (!result.is_object() || !result.contains("values")) {
                XX_LOGW(
                    "[io] permission result is not an object with values, denied: {}",
                    result.dump()
                );
            } else {
                const auto& valueObj = result["values"];
                if (valueObj.is_object()) {
                    values = valueObj;
                }
                allowed  = agentxx::middleware::interruptValueBool(valueObj, "decision", false);
                remember = agentxx::middleware::interruptValueBool(valueObj, "remember", false);
                fullAuth = agentxx::middleware::interruptValueBool(valueObj, "fullAuth", false);
            }
            // 记住本次选择: 客户端只回传表单值 (是否勾选 remember), 规则注册由
            // 请求方 ([PermissionMiddlewareHandle]) 按响应中的 remember 自行完成
            // —— 规则表归中间件所有, 它订阅的是 agent 全局总线, 而本端点的权限
            // 服务注册在会话总线上, 端点不能跨总线直接改规则表 (会丢失)
            const bool rememberRule = remember && confirmedValues(values);
            const bool fullAuthRule = fullAuth && allowed && confirmedValues(values);
            co_return events::RespPermission{
                .decision = allowed ? events::RespPermission::Decision::Allow
                                    : events::RespPermission::Decision::Deny,
                .reason   = allowed ? "" : "user denied",
                .remember = rememberRule,
                .fullAuth = fullAuthRule,
            };
        }
    );
}

/// 结果是否包含已确认的输入值 (空对象 = 用户取消/中断过期, 不注册规则)
bool AgentIOBase::confirmedValues(const agentxx::util::Json& values) {
    return values.is_object() && !values.empty();
}

} // namespace agent
} // namespace agentxx
