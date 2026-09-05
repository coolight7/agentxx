#pragma once

#include "agentxx-client/config_loader.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include <memory>
#include <string>
#include <vector>

namespace agentxx {
namespace client {

/// 生成尽量唯一的会话 sessionId:
/// 高精度时间戳 + 进程 PID + 随机数 + 自增序号 (见
/// [mode_runners.cpp](/agent/client/src/mode_runners.cpp) 实现注释)
std::string generateUniqueSessionId();

/// client 侧插件配置列表 (yaml `plugins` 段; sides 过滤在 ClientPluginManager
/// 内完成; 传入空列表 = 不加载 client 插件)
using ClientPluginConfigs = std::vector<agent::PluginConfig>;

void runLocalCliUnified(std::shared_ptr<agent::CodeAgent> agent, ClientPluginConfigs plugins = {});

void runLocalTuiUnified(
    std::shared_ptr<agent::CodeAgent> agent,
    agent::PermissionMode             permissionMode = agent::PermissionMode::Ask,
    ClientPluginConfigs               plugins        = {}
);

void runRemoteCli(
    std::string_view    url,
    std::string_view    token,
    std::string_view    model,
    ClientPluginConfigs plugins = {}
);

void runRemoteTui(
    std::string_view      url,
    std::string_view      token,
    std::string_view      model,
    agent::PermissionMode permissionMode = agent::PermissionMode::Ask,
    ClientPluginConfigs   plugins        = {}
);

} // namespace client
} // namespace agentxx
