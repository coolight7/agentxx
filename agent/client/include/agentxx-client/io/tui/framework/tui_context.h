#pragma once

#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/agent/context.h"
#include "agentxx/plugin/client_plugin_manager.h"
#include "ftxui/screen/terminal.hpp"
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
    ///   (TUIClientAgentIO::requestOlderHistory) 内部做请求去重与边界判断
    std::function<void()> requestMoreHistory;

    /// 请求加载更早的持久化会话页 (会话列表分页; 线程安全)
    /// - SessionSelectorOverlay 选择项接近已加载列表末尾时调用; 实现方
    ///   (TUIClientAgentIO::requestNextSessionListPage) 内部做请求去重、
    ///   hasMore 边界判断并以上一页最后一条为 keyset 游标发起请求
    std::function<void()> requestMoreSessions;

    /// 请求服务端列举目录 (跨设备附件选择; 线程安全, 回调由 UI 线程执行)
    std::function<void(
        std::string                                                   path,
        std::vector<std::string>                                      allowedExtensions,
        std::function<void(const agentxx::agent::WireListDirResult&)> callback
    )>
        requestServerListDir;

    /// 屏幕上方 Toast 提示 (UI 线程独占调用)
    std::function<void(std::string)> showToast;

    /// 当前主题 (UI 线程独占, 渲染/事件时直接读取)
    TUITheme* theme = nullptr;

    /// 本 TUI 绑定的会话 sessionId
    std::string sessionId;

    /// 远程地址 (空 = 内置)
    std::string remoteUrl;

    /// 数据文件夹绝对路径 (对应 yaml 配置 data_dir; 空 = 未配置/远程未知)
    std::string dataDir;

    /// 当前会话工作目录绝对路径 (空 = 未配置, 回退进程 CWD)
    std::string workDir;

    /// 客户端设备唯一标识 (32 位 MD5)
    std::string clientDeviceId;

    /// 服务端设备唯一标识 (从 WireHelloAck 获取, 32 位 MD5)
    std::string serverDeviceId;

    /// 服务端当前工作目录绝对路径 (从 WireHelloAck 获取)
    std::string serverWorkDir;

    /// 判断 Server 跟 Client 是否为不同设备
    /// - 内置直连模式 (remoteUrl 为空) 必然为同一设备
    /// - 远程模式下直接比对 serverDeviceId 与 clientDeviceId 是否不同
    bool isServerDifferentDevice() const noexcept {
        if (remoteUrl.empty()) {
            return false;
        }
        if (serverDeviceId.empty() || clientDeviceId.empty()) {
            return false;
        }
        return serverDeviceId != clientDeviceId;
    }

    /// client 插件管理器 (mode_runners 装配后注入; 状态栏/侧边栏渲染与
    /// 命令管线经此读取 UI 注册表快照; 线程安全: uiRegistrySnapshot/hasCommand
    /// 短锁, 渲染可无锁读取返回的 snapshot)
    std::shared_ptr<agentxx::plugin::ClientPluginManager> pluginManager;

    /// 视口/终端尺寸覆盖 (宽, 高; 0 表示未覆盖, 运行时回退本帧尺寸/物理终端)
    /// - 离屏测试夹具或嵌入特定容器时可通过此字段注入确定的视口尺寸,
    ///   避免组件 OnRender 直接读取外部物理终端导致测试结果随终端窗口大小漂移
    int viewportWidth  = 0;
    int viewportHeight = 0;

    /// 本帧终端尺寸 (帧首由主循环经 [refreshFrameSize] 刷新)
    /// - 同一帧内所有组件读到同一尺寸: 终端在帧中途 resize 时不会出现
    ///   "一半组件按旧宽度换行、一半按新宽度" 的错位
    /// - 每帧只做一次尺寸查询 (Terminal::Size 在 Linux 上是 ioctl 系统调用),
    ///   替代此前每个组件各自查询
    ftxui::Dimensions frameSize{0, 0};

    /// 帧首刷新本帧尺寸 (主循环/主渲染器调用; 显式视口优先且不受物理终端影响)
    void refreshFrameSize() {
        if (viewportWidth > 0 && viewportHeight > 0) {
            frameSize = {viewportWidth, viewportHeight};
            return;
        }
        frameSize = ftxui::Terminal::Size();
    }

    /// 获取当前生效的终端/视口尺寸: 显式视口 > 本帧缓存 > 物理终端
    ftxui::Dimensions terminalSize() const {
        if (viewportWidth > 0 && viewportHeight > 0) {
            return {viewportWidth, viewportHeight};
        }
        if (frameSize.dimx > 0 && frameSize.dimy > 0) {
            return frameSize;
        }
        return ftxui::Terminal::Size();
    }
};
