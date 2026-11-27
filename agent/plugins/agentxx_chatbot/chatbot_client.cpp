/// agentxx_chatbot 的 client 侧入口: 侧边栏「机器人」栏
///
/// 这一端只做展示, 数据只有一条来源:
/// - **agent 侧**插件把运行态整理成快照, 发布到事件总线主题 `agentxx_chatbot.panel`;
///   主程序把它原样转发成 `WirePluginData{plugin,event,data}` 下发;
/// - **本端**订阅频道 `plugin.agentxx_chatbot.panel` (主程序把对端发的数据原样发布到
///   同名频道), 解析成 `PanelSnapshot` 后重画面板。
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
#include "agentxx/plugin/api/agentxx_broadcast_ids.h"
#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"

#include "chatbot_panel.h"
#include "chatbot_panel_ui.h"

#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <string>
#include <string_view>

namespace ac = agentxx::chatbot;
using agentxx::plugin::PluginStringView;

namespace {

constexpr const char* kPanelId    = "agentxx_chatbot.bots";
constexpr const char* kPanelTitle = "机器人";
constexpr const char* kLogTag     = "chatbot-client";

/// agent 侧发布面板快照的频道 (`plugin.<插件名>.<事件>`; 主程序转发到同名频道)
constexpr const char* kPanelTopic = "plugin.agentxx_chatbot.panel";
/// 本端请求 agent 侧重发快照的主题 (client 侧自己的族)
constexpr const char* kRefreshTopic = "agentxx.client.plugin.agentxx_chatbot.refresh";

/// client 侧实例上下文
struct ChatbotClientCtx : public agentxx::plugin::ClientPluginBase {
    /// 面板内容 (声明项 + 内容; 卸载时由主程序撤销)
    std::shared_ptr<agentxx::plugin::Content> panel;
    /// 最近一次收到的快照 (只在 client io 线程读写)
    ac::PanelSnapshot snapshot;
    /// 上一次交给主程序的面板内容 (内容没变就不重复提交)
    std::string renderedJson;

    /// 重画面板 (内容没变时跳过)
    ///
    /// 文档算在 client io 线程上 (快照也在这里更新), 算好交给 [Content::publish]:
    /// 它记住这一份, 主程序之后来问生成函数时回的是这份拷贝 (跨线程不读别人的状态)。
    void render(bool force = false) {
        if (!panel) {
            return;
        }
        const std::string json = blocksJson(ac::panelItemsOf(snapshot));
        if (!force && json == renderedJson) {
            return;
        }
        renderedJson = json;
        panel->publish(json);
    }

    /// 处理一条频道值 (payload 就是面板快照 JSON 文本)
    void onPanelSnapshot(std::string_view payloadJson) {
        // data 是快照 JSON 文本 (主程序按字符串原样转发, 不解析语义)
        ac::PanelSnapshot parsed;
        if (!ac::parsePanelSnapshot(std::string{payloadJson}, parsed)) {
            log.warn(
                std::string{"panel snapshot ignored (invalid json or newer schema version), size="}
                + std::to_string(payloadJson.size()) + ": "
                + std::string{payloadJson.substr(0, 120)}
            );
            return;
        }
        snapshot = std::move(parsed);
        render();
    }

    /// 让 agent 侧重发一份快照 (未连接时主程序直接丢弃并记日志)
    void requestRefresh() {
        agentxx::plugin::clientSendPluginData(host, iface.wire, kRefreshTopic, "{}");
    }
};

/// 面板快照频道回调 (client io 线程)
void PLUGINXX_CALL onPanelSnapshot(const PluginxxStringView* payload, void* ud) {
    auto* ctx = static_cast<ChatbotClientCtx*>(ud);
    if (ctx == nullptr || payload == nullptr || payload->data == nullptr || payload->size <= 0) {
        return;
    }
    try {
        ctx->onPanelSnapshot(
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

/// client 侧注册事务 (协程形态)
///
/// 没有界面能力 (CLI / 老主程序) 时直接返回: 面板注册失败不影响 agent 侧功能。
asio::awaitable<void> chatbotClientSetupAsync(ChatbotClientCtx& ctx) {
    const PluginxxHost* host = ctx.host;
    const auto          ui   = ctx.iface.ui;
    if (!ui) {
        co_return;
    }
    {
        utilxx_base::Json panelData = utilxx_base::Json::object();
        panelData["title"]          = kPanelTitle;
        ctx.panel                   = co_await agentxx::plugin::clientRegisterContentAsync(
            ctx,
            agentxx::plugin::ContentSpec{
                .slot  = "panel",
                .id    = kPanelId,
                .title = kPanelTitle,
                .data  = panelData,
            }
        );
        if (!ctx.panel) {
            ctx.log.warn("register panel `agentxx_chatbot.bots` failed (id taken or no sidebar)");
        }
        // 先给一份占位内容: 免得 tab 里一片空白, 也让用户知道在等谁的数据
        ctx.render(true);
    }
    const auto broadcasts = ctx.iface.broadcasts;
    if (!broadcasts || !broadcasts->subscribe_broadcast) {
        co_return;
    }
    struct SubSpec {
        std::string_view                                  id;
        void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*);
    };
    // 第一条必需 (面板数据全靠它), 后两条失败只影响“主动补快照”
    const SubSpec required[] = {{kPanelTopic, onPanelSnapshot}};
    const SubSpec optional[] = {
        {agentxx::plugin::broadcasts::ClientReady,           onServerReady},
        {agentxx::plugin::broadcasts::ClientConnectionState, onConnState  },
    };
    for (const auto& sub : required) {
        const auto idSv = agentxx::plugin::PluginStringView::from(sub.id.data(), sub.id.size());
        auto res = co_await agentxx::plugin::clientTableOpAsync(
            ctx,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* e) {
                return broadcasts->subscribe_broadcast(
                    host,
                    nullptr,
                    &idSv,
                    sub.handler,
                    &ctx,
                    nullptr,
                    cb,
                    ud,
                    e
                );
            }
        );
        if (!res.ok()) {
            throw std::runtime_error(
                fmt::format("subscribe `{}` failed: {}", sub.id, res.text())
            );
        }
    }
    for (const auto& sub : optional) {
        const auto idSv = agentxx::plugin::PluginStringView::from(sub.id.data(), sub.id.size());
        auto res = co_await agentxx::plugin::clientTableOpAsync(
            ctx,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* e) {
                return broadcasts->subscribe_broadcast(
                    host,
                    nullptr,
                    &idSv,
                    sub.handler,
                    &ctx,
                    nullptr,
                    cb,
                    ud,
                    e
                );
            }
        );
        if (!res.ok()) {
            ctx.log.warn(fmt::format("subscribe `{}` failed: {}", sub.id, res.text()));
        }
    }
    co_return;
}

void chatbotClientStart(
    ChatbotClientCtx&             ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error
) {
    if (!notify || !notify->done) {
        agentxx::plugin::PluginString::set(ctx.host, error, "client start: notify required");
        return;
    }
    auto later = agentxx::plugin::finishLater(notify);
    ctx.spawn([later](ChatbotClientCtx& c, pluginxx::OpCtl&) -> asio::awaitable<void> {
        try {
            co_await chatbotClientSetupAsync(c);
        } catch (const std::exception& e) {
            later.failed(
                fmt::format("client start: registration failed (面板/事件订阅): {}", e.what())
            );
            co_return;
        }
        later.ok();
        co_return;
    });
}

void chatbotClientStop(
    ChatbotClientCtx&,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    // 面板注册与事件订阅由主程序在本事务之后统一撤销; 本插件没有自管线程/定时器
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return;
}

} // namespace

AGENTXX_CLIENT_PLUGIN_EXPORT(
    ChatbotClientCtx,
    "agentxx_chatbot",
    "1.1.0",
    "Chat platform bridge (QQ first): sidebar panels for bot sessions / queue / personas",
    chatbotClientStart,
    chatbotClientStop
)
