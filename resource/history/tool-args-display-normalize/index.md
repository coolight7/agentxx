# 工具特化渲染前的参数自动修正 (统一 toolcall 的参数自愈)
- 难度: B
- 类型: 重构
- 时间: 2026-11-08
- 需求:
```md
请修改 tui 特化渲染 前也用 toolcallnode 的自动参数转换，这样统一了设计，且特化渲染内也不再需要单独做各种兼容和转换
```
- 现状问题:
    - 执行前 `ToolcallWrapNode::autoFixArgsType` 会按工具参数 schema 修正类型
      (string→数组、单对象→对象数组、字符串数字→数值、枚举大小写等);
      但**展示侧**拿到的是模型原文: 工具展示行的参数文本来自 assistant 消息里的
      `tool_calls[].arguments`, 特化渲染器/预设模版摘要/展开体参数都用这份原文
    - 于是各特化渲染器自己写兼容: `agentxx_fs_plugin.h` 的 `stringListArg`
      (string 或 array 都认)、`editSegmentsArg`(`edits` 单对象也认);
      `docs/zh-cn/design/plugins.md` 也写着"界面层不能只认其中一种"
- 结论 (做的事):
    - 把参数自愈抽成共享实现 `agentxx::util::autoFixToolArgsTypes` +
      文本级入口 `autoFixedToolArgsJson` (`agent/lib/src/util/tool_args_fix.cpp`), 节点执行前与
      展示侧用同一份
    - `ToolRegistry` 登记静态工具参数 schema 并提供 `parametersOf` 查询
      (静态表 → 动态插件工具定义), `BaseAgent::init` 的 `bind_tools` 填表
    - `EventBridge` 建工具展示行时按 schema 自愈参数文本; `SessionsManager`
      在历史尾窗加载与分页读取时做同一件事 (老会话同样受益)
    - 渲染器侧删掉重复的类型兼容 (`stringListArg` 的 string 分支、
      `editSegmentsArg` 的 `edits` 单对象分支), 直接按 schema 声明的类型读
    - 只改**展示**用的参数文本; LLM 上下文里的 assistant `tool_calls.arguments`
      保持模型原文, 不改 wire 协议、客户端仍不持有 schema
- 验证: debug 构建 0 错误; `toolcall_args`/`event_bridge`/`session_persistence`/
  `client_plugins`/`tui_tool_header` 及 agent 侧与插件面相关模块全绿 (逐项见 work.md)
- 偏差 / 未做:
    - 被删掉的兼容行为对应的旧用例已改为新契约 (`test_client_plugins` 15.3h/15b、
      `test_tui_tool_header` 的 `edits` 单对象用例)
    - 第三方 SDK 的 `ArgReader` (`agent/third_party/cxx_pluginxx`) 仍保留宽容读取
      (字符串转数值等): 它属于插件框架, 与本次统一不冲突, 未改动
    - 库内历史行不会被回写改写: 自愈只发生在内存, 直到该行因工具结果回填等原因
      再次更新时才随之落盘
    - 参数键顺序: 发生转换的行重新 `dump()` 后键按字母序排列 (仅展示文本影响,
      未转换的行逐字节不变)
