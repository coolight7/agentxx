#pragma once

#include "agentxx/plugin/client_plugin_manager.h"

#include <string>

namespace agentxx {
namespace plugin {

class ClientPluginManager;

/// 注册 lib 内置工具 (没有对应插件的工具) 的工具行内容
///
/// - 覆盖工具: `agentxx_share_store` (会话共享存储 get/new/update)、
///   `agentxx_subagent` (子代理委派, 单发与批量)
/// - 由 client 侧加载阶段调用一次 (见 client 的 setupClientPlugins): 与插件注册的
///   工具行内容走同一条路径 —— 登记成 `agentxx.client.tool` 上的声明项 + 内容,
///   内容在 client io 线程同步算, 结果写入 [ClientToolRenderCache]
/// - 重复调用幂等 (同一工具名覆盖旧项); 匹配优先级低于插件内容
///   (插件可覆盖内置渲染), 见 ClientPluginManager::registerBuiltinToolContent
/// - 只注册显示名与折叠内容; 展开体保持主程序默认展示 (参数与结果),
///   特化渲染不隐藏工具的原始信息
void registerBuiltinToolRows(ClientPluginManager& mgr);

/// 内置工具行的折叠内容 (主程序内部形态, 不是插件接口)
///
/// - 供 [registerBuiltinToolRows] 使用; 也供主程序/测试直接构造工具行内容
/// - 输出一行文本 (**不含**分隔符 `" · "`, 由界面统一补); 空串 = 本次没有内容
///   (参数还没到 / 解析失败, 界面走默认渲染)
std::string builtinShareStoreSummary(const ClientToolRenderInput& input);
std::string builtinSubagentSummary(const ClientToolRenderInput& input);

} // namespace plugin
} // namespace agentxx
