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

/// 运行全部资源基准测试
void benchResourceAll();

} // namespace bench
} // namespace agentxx
