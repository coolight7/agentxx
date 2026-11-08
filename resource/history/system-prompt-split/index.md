# 系统提示词拆分: 人设与宿主契约分离

状态: 实施中

## 目标

- `AgentPrompt::systemPrompt` 只保留人设: 身份两行 + 代码工作方式 + 回答风格
- 宿主契约 (上下文状态标记说明 / 会话路径) 与通用作业准则移到
  `appendSystemPrompts` 的内置段, 自定义提示词时不必再补一遍通用部分

## 范围

| 文件 | 改动 |
|---|---|
| `agent/lib/include/agentxx/agent/prompt.h` | 内置段键/顺序/来源常量, `isBuiltinSectionKey` |
| `agent/lib/src/agent/prompt.cpp` | 段文本搬家 (systemPrompt 精简, 新增 3 个内置段) |
| `agent/lib/src/agent/training.cpp` | 字符级变异与 LLM 补丁不覆盖内置段 |
| `agent/lib/src/agent/assembly_snapshot.cpp` | 加载快照补 append 段字符数 |
| `agent/test/core/test_agent.cpp` | 新增"自定义人设仍带契约段"回归断言 |
| `agent/test/core/test_summarization.cpp` | 剥离出厂房段改为清空整表 |
| `docs/zh-cn/design/index.md` / `plugins.md` | 提示词归属说明 |

不在范围: 插件 ABI 与 wire 协议不变; 运行时来源 (memory / skills) 的注册与刷新机制不变;
客户端界面不改。

## 关键决策

| # | 决策 | 结论 |
|---|---|---|
| 1 | 拆分口径 | 描述宿主程序行为或环境事实的内容拆出; 与写代码直接相关的工作准则留在 `systemPrompt` (默认预设是代码 agent) |
| 2 | 内置段落放在哪 | 直接写进 `appendSystemPrompts` (键名 `core-` 前缀), 不新增字段、不设元数据 —— 与既有 `skill` 段同一机制, 配置/插件接口无需改 |
| 3 | 顺序 | 不单独设 order: 与插件段一起按 order 0 + 键名字典序 (`core-behavior` → `core-runtime` → `core-workdir`) |
| 4 | 是否可被删改 | 允许 (配置/插件按同名键覆盖, JSON `null` 删除), 但训练字符变异与 LLM 补丁按 `isBuiltinSectionKey` 跳过 |
| 5 | `## Response Style` | 留在 `systemPrompt` (属人设风格); 顺序因此提前到契约段之前, 无实质影响 |
| 6 | 兜底路径 | `BaseAgent` 把 `systemPrompt` 塞进图节点 `instructions` 只用于"会话首条非 system"的兜底, 正常轮次由 `buildSystemPromptStable` 重建, 本次不改 |

## 结论

- `systemPrompt` 只剩人设 (身份两行 + `## Working on Code` + `## Response Style`), 契约与通用准则不再重复写
- 三段宿主契约/通用准则成为附加段的内置段 (`core-runtime` / `core-workdir` / `core-behavior`),
  按字典序拼在 `systemPrompt` 之后; 自定义人设时自动带上
- 内置段不被训练改写: 字符变异与 LLM 补丁按 `AgentPrompt::isBuiltinSectionKey` (`core-` 前缀) 跳过
- `prompt.cpp` 全部 raw string 统一为 `R"_(...)"_` 定界符 (31 处, 含工具提示词表)
- 验证: windows-debug 编译通过;
  `agentxx_test agent prompt_stability summarization training plugin_sdk` = 910 项通过,
  另跑 `plugin_runtime client_plugins tui_widget tui_context_overlay` = 1890 项通过
