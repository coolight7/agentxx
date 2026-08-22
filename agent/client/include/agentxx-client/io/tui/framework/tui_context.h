#pragma once

#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/agent/context.h"
#include "agentxx/plugin/client_plugin_manager.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

/// TUI 组件共享上下文 (类似 Flutter 的 BuildContext / InheritedWidget)
///
/// 所有 TUI 组件通过此结构访问共享状态, 避免直接依赖 TUIClientAgentIO 全部接口。
/// TUIClientAgentIO 在 start() 时构建并传递给各组件。
///
/// 线程模型不变:
/// - 渲染阶段: 读取 frameState (本帧快照, 无锁)
/// - 事件阶段: 经 state.mutate() 做 短锁 + COW 写入
/// - 需要重绘时调用 postRedraw()
struct TUICtx {
    /// 跨线程共享状态 (COW)
    TUISharedState* state = nullptr;

    /// 本帧状态快照 (每帧开头由主渲染器填充, 渲染期间无锁读取)
    std::shared_ptr<TUIRenderState> frameState;

    /// 触发 UI 重绘 (线程安全, 可从任意线程调用)
    std::function<void()> postRedraw;

    /// 请求加载更早的历史消息 (历史分页; 线程安全)
    /// - MessageListComponent 检测到滚动接近窗口顶部时调用; 实现方
    ///   (TUIClientAgentIO::requestOlderHistory) 内部做在途去重与边界判断
    std::function<void()> requestMoreHistory;

    /// 当前主题 (UI 线程独占, 渲染/事件时直接读取)
    TUITheme* theme = nullptr;

    /// 本 TUI 绑定的会话 thread_id
    std::string sessionId;

    /// 远程地址 (空 = 内置)
    std::string remoteUrl;

    /// client 插件管理器 (mode_runners 装配后注入; 状态栏/侧边栏渲染与
    /// 命令管线经此读取 UI 注册表快照; 线程安全: uiRegistrySnapshot/hasCommand
    /// 短锁, 渲染可无锁读取返回的 snapshot)
    std::shared_ptr<agentxx::plugin::ClientPluginManager> pluginManager;
};
