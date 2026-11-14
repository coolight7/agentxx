# 子代理工具对齐主 agent + JS 插件多挂载作用域

- 状态: 已完成 (JS SDK 仅剩 runtime / 工具特化渲染未对齐)
- 时间: 2026-11-14 (JS SDK 网络/后台任务/查询面补齐: 2026-11-15)
- 类型: 功能 + 重构 (lib 配置派生 / 事件结构 / 工具注册表 / JS 解释器插件)

## 目标

1. `agentxx_subagent` 派生出的子代理**默认加载与主会话完全一致的插件 / 工具 / MCP / skill / memory**,
   而其它派生路径 (`makeSubagentConfig` 的其它调用方、上下文压缩、插件自建根) **保持现有轻量行为不变**。
2. `makeSubagentConfig` 增加显式参数, 由调用方逐项指定要继承什么; 缺省值 = 今天的轻量子代理。
3. `agentxx_javascript_engine` 重构为**每 agent 一份脚本实例**(多挂载作用域), 让 JS 插件与 C++ 插件
   在"一插件多 agent 挂载"上行为一致; JS 侧 SDK 逐步向 C++ 侧对齐。

## 现状结论 (对应需求 1)

- 缺省子代理只有 7 个内置/中间件工具 (`agentxx_share_store` / `agentxx_async_emit` /
  `agentxx_async_*` 四件 / `agentxx_subagent`, 另 `agentxx_git_worktree` 视 `worktree.enable`)。
- 原因: `AgentHost::makeSubagentConfig` 强制 `pluginMountPolicy = None` 并清空 mcp/skill/memory/rag;
  插件工具是 mount 时写进 per-agent `ToolRegistry` 的动态工具, 不挂载即不存在。
- 运行时实测: 子代理内 `agentxx_filesystem_list` / `agentxx_execute_windows_command` 报工具不存在,
  `agentxx_share_store` 可用 (单发委派走"同上下文"模式, 共享父会话 store)。

## 关键决策

- 继承开关放在**派生入口的参数**上, 不放全局配置: 同一进程内不同委派来源可以有不同结论。
- `agentxx_subagent` 工具本身把"继承全部"写进中断参数, 由 `AgentHost` 执行;
  内部路径 (压缩/事件等待) 不写 = 现行为。
- `tools: []` / `toolExecutionDisabled` 视为"无工具运行", 即使要求继承也**不挂插件**(省掉无意义的挂载)。
- `tools` 显式列表要能过滤**插件动态工具**, 否则挂载后 `[]` / 自定义名单会失效;
  `toolNames` 也要收动态名, 否则 `["*"]` 不等于父集合。
- JS 引擎改为"按 (脚本, 挂载作用域) 一份 JSContext": 注册用调用方 host + 该挂载的 scope ref,
  不再依赖"最后一次 mount 的作用域"。

## 结论

- `makeSubagentConfig(parentConfig, inherit)` 已按开关派生; **缺省值与旧行为逐字一致**,
  只有 `agentxx_subagent` (含经 `agentxx_async_run` 提交的同类任务) 写 `SubagentInherit::full()`,
  子代理因此加载与主会话相同的插件 / 工具 / MCP / Skill / Memory。
- `tools` 缺省 = 与父 agent 同一套工具; `tools: []` / `toolExecutionDisabled` 仍是无工具运行
  (跳过插件挂载); 自定义名单现在也能拦住插件动态工具 (ToolRegistry 名称过滤)。
- `agentxx_javascript_engine` 已改成"每 (脚本, agent 挂载) 一份实例": 同一 JS 插件可以同时挂在
  主 agent 与子代理上, 注册各自独立; agent 卸载只清自己的实例。JS SDK 向 C++ 侧对齐了
  权限/配置/作用域/广播/资源等基础面, 又补上了网络 (`netCall` / `netFetch` / WebSocket /
  HTTP 服务端)、后台任务 (`submitAsyncWork` + 事件回调) 与提示词·会话·上下文·执行图·挂载
  查询; 剩下的只有会话运行时 `runtime` 与工具特化渲染 (仍写在 C++ 壳里)。
- 验证: lib + 插件 + test 均编译通过; `agent_host` / `plugins` / `plugin_*` / `async_*` /
  `event_*` / `agent` / `boundaries` / `config_keys` 合计 3400+ 断言全绿。

## 偏差

- 未新增任何 yaml 配置键: 继承意图由委派调用本身表达 (`agentxx_subagent` 固定全开)。
- JS SDK 未一次性对齐全部 20 张表 (仅剩 `runtime` 与工具特化渲染, 见 work.md 的"剩余事项")。
