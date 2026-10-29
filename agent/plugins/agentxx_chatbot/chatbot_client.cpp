/// agentxx_chatbot 的 client 侧入口: 侧边栏「机器人」栏
///
/// 这一端只做展示, 数据只有一条来源:
/// - **agent 侧**插件把运行态整理成快照, 发布到事件总线主题 `agentxx_chatbot.panel`;
///   主程序把它原样转发成 `WirePluginData{plugin,event,data}` 下发;
/// - **本端**订阅 `AGENTXX_CLIENT_EVT_PLUGIN_DATA`, 认领 `plugin=agentxx_chatbot` /
///   `event=panel` 的那条 JSON, 解析成 `PanelSnapshot` 后重画面板。
///
/// 面板内容 (详见 chatbot_panel_ui.cpp):
/// - 段标题统一 `标题: 数量`, 列表项统一 `|- 内容` 前缀, 段与段之间是一行空白
///   (不画分隔线), 副标题/说明这类减淡内容一律用 hint 色;
/// - 会话列表: 左侧是加载中状态 (正在跑一轮, 转动的点阵与消息列表的加载中同一套字符),
///   右侧是还在排队、没投给模型的消息条数;
/// - 人设: 名字、模型、会话用量/上限、配置的对接规则数、提示词第一行;
/// - 账号: 一个账号一行 (含连接状态), 末尾一行收发计数。
///
/// 「实时」只由 agent 侧的节拍驱动 (有会话在跑时 ~120ms 一份), 本端不轮询; 启动、
/// 连接就绪/重连时各发一次 refresh 请求补一份快照, 晚接入的客户端不会停在占位提示上。
#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_kit.h"

#include "chatbot_panel.h"
#include "chatbot_panel_ui.h"

#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <string>
#include <string_view>

namespace ac = agentxx::chatbot;
using agentxx::plugin::PluginStringView;

namespace {

constexpr const char* kPluginName = "agentxx_chatbot";
constexpr const char* kPanelId    = "agentxx_chatbot.bots";
constexpr const char* kPanelTitle = "机器人";
constexpr const char* kLogTag     = "chatbot-client";

/// agent 侧发布面板快照时用的事件名
constexpr const char* kPanelEvent = "panel";
/// 本端请求 agent 侧重发快照时用的事件名 (主程序发布到 `client.{插件名}.{事件名}`)
constexpr const char* kRefreshEvent = "refresh";

/// client 侧实例上下文
struct ChatbotClientCtx : public agentxx::plugin::ClientPluginBase {
    /// 面板句柄 (主程序持有; 卸载自动清理)
    AgentxxPanel* panel = nullptr;
    /// 最近一次收到的快照
    ac::PanelSnapshot snapshot;
    /// 上一次提交给主程序的面板内容 (内容没变就不重复提交)
    std::string renderedJson;

    /// 重画面板 (内容没变时跳过)
    void render(bool force = false) {
        if (panel == nullptr) {
            return;
        }
        const std::string json = itemsJson(ac::panelItemsOf(snapshot));
        if (!force && json == renderedJson) {
            return;
        }
        renderedJson = json;
        setPanelJson(panel, json);
    }

    /// 处理一条插件事件 (载荷形如 `{"plugin":"…","event":"…","data":"<快照 JSON>"}`)
    void onPluginData(std::string_view payloadJson) {
        utilxx_base::Json payload;
        try {
            payload = utilxx_base::Json::parse(payloadJson);
        } catch (const std::exception&) {
            return;
        }
        if (!payload.is_object() || payload.value("plugin", std::string{}) != kPluginName
            || payload.value("event", std::string{}) != kPanelEvent) {
            return; // 不是本插件的面板快照
        }
        if (!payload.contains("data") || !payload["data"].is_string()) {
            return;
        }
        // data 是快照 JSON 文本 (主程序按字符串原样转发, 不解析语义)
        ac::PanelSnapshot parsed;
        const std::string data = payload["data"].get<std::string>();
        if (!ac::parsePanelSnapshot(data, parsed)) {
            log.warn(
                std::string{"panel snapshot ignored (invalid json or newer schema version), size="}
                + std::to_string(data.size()) + ": " + data.substr(0, 120)
            );
            return;
        }
        snapshot = std::move(parsed);
        render();
    }

    /// 让 agent 侧重发一份快照 (未连接时主程序直接丢弃并记日志)
    void requestRefresh() {
        if (!host || !iface.wire || !iface.wire->send_plugin_data) {
            return;
        }
        auto eventSv = PluginStringView::fromCstr(kRefreshEvent);
        auto dataSv  = PluginStringView::fromCstr("{}");
        iface.wire->send_plugin_data(host, &eventSv, &dataSv);
    }
};

/// 插件事件回调 (client io 线程)
void PLUGINXX_CALL onPluginData(const PluginxxStringView* payload, void* ud) {
    auto* ctx = static_cast<ChatbotClientCtx*>(ud);
    if (ctx == nullptr || payload == nullptr || payload->data == nullptr || payload->size <= 0) {
        return;
    }
    try {
        ctx->onPluginData(
            std::string_view{payload->data, static_cast<size_t>(payload->size)}
        );
    } catch (const std::exception& e) {
        ctx->log.warn(std::string{"["} + kLogTag + "] panel update failed: " + e.what());
    } catch (...) {
        ctx->log.warn(std::string{"["} + kLogTag + "] panel update failed");
    }
}

/// 服务端就绪 (client io 线程): 要一份当前快照
void PLUGINXX_CALL onServerReady(const PluginxxStringView*, void* ud) {
    if (auto* ctx = static_cast<ChatbotClientCtx*>(ud); ctx != nullptr) {
        ctx->requestRefresh();
    }
}

/// 连接状态变化 (client io 线程): 重连成功后同样补一份快照
void PLUGINXX_CALL onConnState(const PluginxxStringView* payload, void* ud) {
    auto* ctx = static_cast<ChatbotClientCtx*>(ud);
    if (ctx == nullptr || payload == nullptr || payload->data == nullptr) {
        return;
    }
    try {
        const auto j = utilxx_base::Json::parse(
            std::string_view{payload->data, static_cast<size_t>(payload->size)}
        );
        if (j.is_object() && j.value("connState", std::string{}) == "connected") {
            ctx->requestRefresh();
        }
    } catch (const std::exception&) {
        // 载荷非法: 面板只是少拿一份快照, 不影响其它功能
    }
}

/// client 侧注册事务 (start 的实际内容)
///
/// 没有界面能力 (CLI / 老主程序) 时直接返回 0: 面板注册失败不影响 agent 侧功能。
int32_t chatbotClientSetup(ChatbotClientCtx& ctx) {
    if (!ctx.iface.ui) {
        return 0;
    }
    if (ctx.iface.ui->register_panel) {
        utilxx_base::Json props = utilxx_base::Json::object();
        props["title"]          = kPanelTitle;
        const std::string propsText = props.dump();
        auto              idSv      = PluginStringView::fromCstr(kPanelId);
        auto              propsSv   = PluginStringView::from(propsText.data(), propsText.size());
        ctx.panel = ctx.iface.ui->register_panel(ctx.host, &idSv, &propsSv);
        if (ctx.panel == nullptr) {
            ctx.log.warn("register panel `agentxx_chatbot.bots` failed (id taken or no sidebar)");
        }
        // 先给一份占位内容: 免得 tab 里一片空白, 也让用户知道在等谁的数据
        ctx.render(true);
    }
    if (!ctx.iface.events || !ctx.iface.events->subscribe) {
        return 0;
    }
    if (ctx.iface.events->subscribe(
            ctx.host,
            AGENTXX_CLIENT_EVT_PLUGIN_DATA,
            onPluginData,
            &ctx
        )
        == nullptr) {
        return -1;
    }
    // 这两个事件老主程序也支持 (READY 早就有了); 订阅失败只影响"主动补快照"
    ctx.iface.events->subscribe(ctx.host, AGENTXX_CLIENT_EVT_READY, onServerReady, &ctx);
    ctx.iface.events->subscribe(ctx.host, AGENTXX_CLIENT_EVT_CONN_STATE, onConnState, &ctx);
    return 0;
}

void* chatbotClientStart(
    ChatbotClientCtx&             ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error
) {
    if (!notify || !notify->done) {
        agentxx::plugin::PluginString::set(ctx.host, error, "client start: notify required");
        return nullptr;
    }
    if (chatbotClientSetup(ctx) != 0) {
        agentxx::plugin::PluginString::set(
            ctx.host,
            error,
            "client start: registration failed (面板/事件订阅)"
        );
        return nullptr;
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

void* chatbotClientStop(
    ChatbotClientCtx&,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    // 面板注册与事件订阅由主程序在本事务之后统一撤销; 本插件没有自管线程/定时器
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

} // namespace

AGENTXX_PLUGIN_CLIENT_EXPORT(
    ChatbotClientCtx,
    "agentxx_chatbot",
    "1.1.0",
    "Chat platform bridge (QQ first): sidebar panels for bot sessions / queue / personas",
    chatbotClientStart,
    chatbotClientStop
)
