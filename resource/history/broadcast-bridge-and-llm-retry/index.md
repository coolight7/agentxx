# 插件广播两向转发 + LLM 重试误判 + 声明重复日志

## 目标

修三个用户可见的问题：

1. `agentxx_codegraph` 的 TUI 侧边栏一直显示 `|- wait for load`；
2. 网络错误偶现直接结束轮次（`Node 'llm' failed after 1 attempt(s): SSE read-body timeout ...`），没有按配置重试；
3. 日志刷屏 `[I] BroadcastRegistry: declaration of \`plugin.agentxx_system_monitor.usage\` replaced (agentxx_system_monitor -> agentxx_system_monitor)`。

## 范围

- `agent/lib`：插件管理器 (广播 ↔ 事件总线两向转发)、LLM 错误分类
- `agent/third_party/cxx_pluginxx`：广播注册表的重复声明幂等（只改 `.cpp`，不动头文件）
- `agent/test`：`plugin_broadcast`、`llm_error` 两个模块的用例

## 结论（事后来看这一页就够）

| 问题 | 根因 | 修法 |
|---|---|---|
| 侧边栏停在 `wait for load` | 广播化重构后少了两向转发：① 总线上的 `agentxx.agent.*` / `agentxx.client.*` 不再进广播注册表（`agentxx.agent.host.client_attached` 送不到 agent 侧插件，重发快照不触发）；② 插件发布的广播不再上总线（`SessionServerAgentIO` 的 `plugin.` 前缀转发收不到，`WirePluginData` 永远不发） | 新增 `PluginManager::attachEventBus`（总线 → 广播镜像，只镜像已声明频道）与 `forwardBroadcastToBuses`（插件发布 → 总线，供端点转发客户端） |
| 网络错误不重试 | `classifyLlmError` 用裸子串匹配状态码，而流式失败诊断后缀带 `recv=403684B` → 命中 `403` 被判成"鉴权失败"（不可重试）→ 首次尝试就抛出 | 状态码改成独立 token 匹配（前后不能是字母数字）；分类前先切掉我们自己生成的 ` &#124; stream: ...` 诊断后缀 |
| 声明重复日志 | SDK `publishPluginBroadcast` 每次发布前都声明一次，注册表对"完全相同的声明"也打 `declaration ... replaced` | 相同声明视为幂等成功，不打日志、不动状态（已结束的频道仍按"开新流"处理） |
