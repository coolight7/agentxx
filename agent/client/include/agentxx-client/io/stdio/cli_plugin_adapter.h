#pragma once

#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx/plugin/client_plugin_manager.h"
#include <iostream>
#include <memory>
#include <string>

namespace agentxx {
namespace client {

/// CLI 插件 UI 适配器: UI 无关语义层 → stdio 终端
///
/// 能力 (uiCaps):
/// - TOAST: 输出到 stderr (保持 stdout 纯净 —— stdout 是消息渲染通道,
///   未来程序化 stdin/stdout 协议依赖此约定)
/// - 命令: 命令属于输入管线 (stdin 行解析), 必然支持, 不作为 cap 位;
///   输入循环在发送前拦截 "/" 开头的行 (见 mode_runners)
/// - 其余 UI 能力 (状态栏/面板) 不支持: 插件注册时被 ClientPluginManager
///   拒绝 (返回 NULL / 非 0), 插件自行降级
///
/// 线程模型: 回调在 client io 线程同步调用 (输入循环所在线程);
/// sendPluginMessage 经端点 sendPluginUserInput 发送 (与用户输入同路径)
class CliPluginAdapter : public agentxx::plugin::PluginUiAdapter {
public:

    explicit CliPluginAdapter(std::weak_ptr<StdIOClientAgentIO> io) :
        io_(std::move(io)) {}

    uint32_t uiCaps() const override {
        return AGENTXX_UI_CAP_TOAST;
    }

    // ---- toast (stderr, 保持 stdout 纯净) ----
    void onToast(const std::string& text, int level) override {
        const char* prefix = level >= 2 ? "[Error] " : (level == 1 ? "[Warning] " : "[Info] ");
        std::cerr << std::endl << prefix << text << std::endl;
    }

    // ---- send 动作: 代发用户消息 (与用户输入同路径) ----
    void sendPluginMessage(const std::string& text) override {
        if (auto io = io_.lock()) {
            io->sendPluginUserInput(text);
        }
    }

    void requestCancel(const std::string& threadId) override {
        if (auto io = io_.lock()) {
            io->requestCancel(threadId);
        }
    }

    bool sendPluginData(
        const std::string& plugin,
        const std::string& event,
        const std::string& json
    ) override {
        auto io = io_.lock();
        return io ? io->sendPluginDataUp(plugin, event, json) : false;
    }

private:

    std::weak_ptr<StdIOClientAgentIO> io_;
};

} // namespace client
} // namespace agentxx
