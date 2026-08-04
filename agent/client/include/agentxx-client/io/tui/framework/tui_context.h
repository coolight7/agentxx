#pragma once

#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/agent/context.h"
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

    /// 当前主题 (UI 线程独占, 渲染/事件时直接读取)
    TUITheme* theme = nullptr;

    /// 是否在 Info 侧边栏显示系统资源占用 (CPU/内存);
    /// 指向 TUIClientAgentIO::systemInfoEnabled_, 可被设置弹窗切换,
    /// 渲染线程与资源监控线程均可读取
    std::atomic<bool>* showSystemInfo = nullptr;

    /// 当前会话 (供状态栏等读取 contextStats)
    std::shared_ptr<agentxx::agent::Session> session;

    /// 本 TUI 绑定的会话 thread_id
    std::string threadId;

    /// 远程地址 (空 = 内置)
    std::string remoteUrl;
};
