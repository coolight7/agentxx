# 实施记录

## 进度

- [x] S1 `prompt.h`: 内置段键常量与 `isBuiltinSectionKey`
- [x] S2 `prompt.cpp`: 文本搬家 (systemPrompt 精简 / 三段内置段 / raw string 定界符统一)
- [x] S3 `training.cpp`: 字符变异与 LLM 补丁跳过内置段
- [x] S4 `assembly_snapshot.cpp`: 补 append 段字符数
- [x] S5 测试更新与新增
- [x] S6 文档同步
- [x] S7 编译与测试

## 每步记录

### S1 `prompt.h`

- 新增 `kSectionCoreRuntime` / `kSectionCoreWorkDir` / `kSectionCoreBehavior` 与
  `static bool isBuiltinSectionKey(std::string_view)` (键名 `core-` 前缀)
- `systemPrompt` 注释改为"只放人设"; `appendSystemPrompts` 注释说明出厂内置段
- 未设 order/source 元数据: 内置段直接写进 `appendSystemPrompts`

### S2 `prompt.cpp`

- `systemPrompt` = 身份两行 + `## Working on Code` (讨论先出方案 / 读现有代码 /
  改后验证 / 大改动先规划并复查 / 代码注释写法) + `## Response Style`
- 新内置段: `core-runtime` (`Agent Runtime` + `Context Markers`)、
  `core-workdir` (`Working Directory`)、`core-behavior` (通用准则: 非测试环境、先理解意图、
  工具查证、benchmark 先看负载、破坏性操作先确认、大文档分模块、报告实际做了什么、
  十六进制查看、回答准确具体)
- 段落正文提到匿名命名空间常量 (`kCoreRuntimePrompt` / `kCoreWorkDirPrompt` /
  `kCoreBehaviorPrompt` / `kSkillUsagePrompt`), 初始化列表引用; `skill` 段保持原键名
- 切分时去掉正文前导换行: `core-*` 与 `skill` 段都以 `##` 直接开头, 避免段间多出空行
- raw string 定界符统一为 `R"_(...)"_`: 原文件 22 处空定界符 `R"(...)"`
  也一并改掉 (工具提示词表), 全文件 31 处 open/close 一一对应

### S3 `training.cpp`

- `createChildVariantCharMut`: 跳过 `isBuiltinSectionKey` 的段
- `normalizePromptPatch`: 丢弃补丁里的内置段键 (LLM 按 schema 可能返回 `core-runtime`)

### S4 `assembly_snapshot.cpp`

- 加载快照 `prompt` 段新增 `append_sections_chars` (附加段总字符数);
  `system_prompt_chars` 拆分后明显变小, 两个数一起看才不误判

### S5 测试

- `test_agent.cpp` 新增 `test_agent_system_prompt_builtin_sections`:
  出厂三个内置段存在、`systemPrompt` 不含契约文本、自定义人设后仍拼出契约段、
  内置段与插件段同 order 按键名排序
- `test_summarization.cpp`: 出厂段剥离由"erase 三个插件键"改为清空整表
  (内置 `core-*` 段此前未覆盖, 会拉长 system 消息)

### S6 文档

- `docs/zh-cn/design/index.md`: 提示词章节补"归属"三条 (人设 / 内置段 / 训练不改写)
- `docs/zh-cn/design/plugins.md` §19.3: `prompt.systemPrompt` 与本项目内置段的关系

### S7 编译与测试

- `agent/script/windows_debug_build.bat` 通过 (唯一 error 是测试里 `map[key]` 用了
  `string_view` 键, 改为 `find`)
- `agentxx_test --log-level warn agent prompt_stability summarization training plugin_sdk`
  → 910 项通过, 0 失败
- `agentxx_test --log-level error plugin_sdk plugin_runtime client_plugins tui_widget tui_context_overlay`
  → 1890 项通过, 0 失败

## 与计划不一致的地方

| 计划 | 实际 | 原因 |
|---|---|---|
| 内置段用 `setAppendSection` 设 order (-300/-200/-100) 与 `source = "core"` | 直接写进 `appendSystemPrompts`, 不设元数据 | 用户要求: 不必定义顺序; 训练改用键名前缀判定, 不依赖 source |
| `code_agent.cpp` 的 `git-worktree` 段标 `source = "core"` | 保持原样 (直接赋值) | 同上, 内置段靠 `core-` 键名前缀识别, 该段键名不在前缀内 |
| 训练字符变异按 `meta.source == "core"` 跳过 | 按 `isBuiltinSectionKey(键名)` 跳过 | 内置段没有 meta |
| `prompt.cpp` 只有新切分的段落用 `R"_(...)"_` | 全文件 31 处 raw string 统一为 `R"_(...)"_` | 用户要求, 避免空定界符遇到 `)"` 出问题 |

## 遗留 / 注意

- `BaseAgent` 给图节点的 `ctx.instructions` 仍是 `config->prompt.systemPrompt` (只含人设);
  它只在"会话首条不是 system 消息"的兜底路径生效, 正常轮次由
  `buildSystemPromptStable` 重建, 本次未改
- 配置 JSON 给内置段键 `null` 会删除该段 (允许的定制); 插件用同名 `append:` 键可覆盖
- 段落顺序变化: `core-behavior` 现在排在 `core-runtime` / `core-workdir` 之前 (字典序),
  `## Response Style` 提前到 systemPrompt 内; 属文本顺序变化, 无行为影响
