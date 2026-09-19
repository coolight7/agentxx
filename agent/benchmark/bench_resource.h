#pragma once

namespace agentxx {
namespace bench {

/// 模式 1: 同一进程 CLI 资源测试 (startup, 100K, 200K)
void benchResourceCli();

/// 模式 2: 同一进程 TUI 资源测试 (startup, 100K, 200K)
void benchResourceTui();

/// 模式 3: 拆分两进程 CLI+Server 资源测试 (分别测量两者)
void benchResourceSplitCli();

/// 模式 4: 拆分两进程 TUI+Server 资源测试 (分别测量两者)
void benchResourceSplitTui();

/// 对照组: libagentxx_shared 动态库资源测试
void benchResourceFfi();

// ---------------------------------------------------------------------------
// 真实运行场景 (bench_resource_real.cpp)
// ---------------------------------------------------------------------------

/// 场景 1: 真实运行的 TUI (FTXUI 界面线程运行, 真实渲染帧) + 同进程 server
/// - 覆盖: 真实渲染帧耗时/帧数、渲染输出字节、TUI 侧内存 (消息窗口/渲染缓存)
void benchResourceRealTui();

/// 场景 2: 真实 server 子进程单独运行 (无客户端常驻)
/// - 覆盖: 空载稳态/空载漂移/真实 WS 轮次驱动/客户端断开后的回收情况
void benchResourceServerOnly();

/// 场景 3: 真实 TUI 子进程 (伪终端驱动, 等同用户敲键盘) + 真实 server 子进程
/// - 仅 POSIX (需要伪终端); Windows 上跳过并说明
void benchResourceRealTuiChild();

/// 场景 4: 逐插件加载/卸载的内存边际成本与回收量
void benchResourcePluginAttrib();

/// 运行全部真实运行场景
void benchResourceRealAll();

/// 运行全部资源基准测试
void benchResourceAll();

} // namespace bench
} // namespace agentxx
