#pragma once

#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/plugin/client_plugin_manager.h"
#include <memory>
#include <string>

namespace agentxx {
namespace client {

/// TUI 插件 UI 适配器: 把 UI 无关语义层 (ClientPluginManager) 的注册/更新/
/// 交互信号翻译为 TUI 组件操作
///
/// 线程模型:
/// - 各回调由 ClientPluginManager 在 client io 线程同步调用 (快速返回约定)
/// - 组件树操作 (sidebar tab / toast) 经 TUIClientAgentIO::postToUi 投递到
///   UI 线程执行 (TUI 组件 UI 线程独占)
/// - sendPluginMessage / requestCancel / sendPluginDataUp 在 io 线程直接执行
///   (端点方法线程安全或本就在 io 线程)
///
/// 面板挂载: onPanelRegistered 时经 postToUi 调 sidebar()->addTab, render 回调
/// 读取 TUIClientAgentIO::renderPluginPanel (UI 线程渲染, 从注册表快照读取);
/// 移除时 removeTab。
class TuiPluginAdapter : public agentxx::plugin::PluginUiAdapter {
public:

    explicit TuiPluginAdapter(std::weak_ptr<TUIClientAgentIO> tui) :
        tui_(std::move(tui)) {}

    agentxx::plugin::InterfaceSet supportedInterfaces() const override {
        namespace pi = agentxx::plugin::plugin_interfaces;
        return {
            std::string{pi::ClientStatusItem},
            std::string{pi::ClientPanel},
            std::string{pi::ClientToast},
            std::string{pi::ClientInfoSection},
            std::string{pi::ClientCommand},
            // 工具消息装饰 (ui 表 v2 update_tool_decor): 消息列表按装饰
            // items 通用渲染插件推送的工具体内容
            std::string{pi::ClientMsgDecor},
        };
    }

    // ---- 状态栏项 ----
    void onStatusItemRegistered(
        const std::string& id,
        const neograph::json& /*props*/,
        int /*align*/,
        int /*order*/
    ) override {
        // 状态栏每帧从注册表快照渲染, 无需额外动作; 仅触发重绘
        if (auto tui = tui_.lock()) {
            tui->requestRedraw();
        }
    }

    void onStatusItemUpdated(const std::string& /*id*/, const neograph::json& /*props*/) override {
        if (auto tui = tui_.lock()) {
            tui->requestRedraw();
        }
    }

    void onStatusItemRemoved(const std::string& /*id*/) override {
        if (auto tui = tui_.lock()) {
            tui->requestRedraw();
        }
    }

    // ---- 侧边栏面板 ----
    void onPanelRegistered(const std::string& id, const neograph::json& props) override {
        auto tui = tui_.lock();
        if (!tui) {
            return;
        }
        std::string title
            = props.is_object() && props.contains("title") ? props["title"].get<std::string>() : id;
        // 组件树 UI 线程独占: 投递 addTab (render 回调读取 renderPluginPanel)
        tui->postToUi([tui, id, title]() {
            tui->addPluginPanelTab(id, title);
        });
    }

    void onPanelUpdated(const std::string& /*id*/, const neograph::json& /*items*/) override {
        if (auto tui = tui_.lock()) {
            tui->requestRedraw();
        }
    }

    void onPanelRemoved(const std::string& id) override {
        auto tui = tui_.lock();
        if (!tui) {
            return;
        }
        tui->postToUi([tui, id]() {
            tui->removePluginPanelTab(id);
        });
    }

    // ---- Info 栏段落 (渲染进内置 Info tab; 每帧从注册表快照读取) ----
    void onInfoSectionRegistered(
        const std::string& /*id*/,
        const neograph::json& /*props*/
    ) override {
        // 触发重绘即可, Info tab 渲染时从注册表快照读取段落
        if (auto tui = tui_.lock()) {
            tui->requestRedraw();
        }
    }

    void onInfoSectionUpdated(
        const std::string& /*id*/,
        const neograph::json& /*items*/
    ) override {
        if (auto tui = tui_.lock()) {
            tui->requestRedraw();
        }
    }

    void onInfoSectionRemoved(const std::string& /*id*/) override {
        if (auto tui = tui_.lock()) {
            tui->requestRedraw();
        }
    }

    // ---- toast ----
    void onToast(const std::string& text, int level) override {
        if (auto tui = tui_.lock()) {
            tui->uiToast(text, level);
        }
    }

    // ---- send 动作: 代发用户消息 (io 线程; 与用户输入同排队语义) ----
    void sendPluginMessage(const std::string& text) override {
        if (auto tui = tui_.lock()) {
            tui->sendPluginUserInput(text);
        }
    }

    void requestCancel(const std::string& sessionId) override {
        if (auto tui = tui_.lock()) {
            tui->requestCancel(sessionId);
        }
    }

    bool
        sendPluginData(const std::string& plugin, const std::string& event, const std::string& json)
            override {
        auto tui = tui_.lock();
        return tui ? tui->sendPluginDataUp(plugin, event, json) : false;
    }

private:

    std::weak_ptr<TUIClientAgentIO> tui_;
};

} // namespace client
} // namespace agentxx
