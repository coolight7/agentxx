#pragma once

#include "agentxx/plugin/api/client_plugin_api.h"

namespace agentxx {
namespace plugin {

class ClientPluginManager;

/// 注册 lib 内置工具 (没有对应插件的工具) 的工具特化渲染器
///
/// - 覆盖工具: `agentxx_share_store` (会话共享存储 get/insert/set/delete)、
///   `agentxx_subagent` (子代理委派, 单发与批量)
/// - 由 client 侧装配阶段调用一次 (见 client 的 setupClientPlugins): 与插件渲染器
///   走同一条渲染路径 —— 回调在 client io 线程执行, 语义结果写入
///   [ClientToolRenderCache], UI 线程只读缓存
/// - 重复调用幂等 (同一工具名覆盖旧项); 注册项匹配优先级低于插件注册项
///   (插件可覆盖内置渲染), 见 ClientPluginManager::registerBuiltinToolRenderer
/// - 渲染内容为折叠头的显示名与摘要; 展开体保持宿主通用展示 (参数与结果),
///   特化渲染不隐藏工具的原始信息
void registerBuiltinToolRenderers(ClientPluginManager& mgr);

/// 内置渲染回调 (C ABI 形态, 与插件渲染器同签名)
/// - 供 [registerBuiltinToolRenderers] 注册; 也供宿主/测试直接放入 UI 注册表
///   ([ClientUiRegistry::builtinToolRenderers]) 使用
/// - 输出字符串经宿主内存操作分配, 由调用方统一释放
/// - `return` 0 成功 (至少提供显示名); 非 0 表示本次渲染不可用, 调用方回退通用展示
int32_t PLUGINXX_CALL builtinRenderShareStore(
    void*                         userData,
    const AgentxxToolRenderInput* input,
    AgentxxToolRenderOutput*      output
);

int32_t PLUGINXX_CALL builtinRenderSubagent(
    void*                         userData,
    const AgentxxToolRenderInput* input,
    AgentxxToolRenderOutput*      output
);

} // namespace plugin
} // namespace agentxx
