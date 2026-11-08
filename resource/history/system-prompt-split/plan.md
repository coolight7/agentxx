# 系统提示词拆分: 设计与实施

## 1. 背景

`systemPrompt` 目前把三件不同性质的东西写在同一段文本里:

1. 人设 (身份 + 回答风格)
2. 宿主契约 (程序写进上下文的标记说明、会话工作目录/临时目录)
3. 通用作业准则 (查证后再动手、大任务先规划、破坏性操作先确认)

后果: 任何自定义人设的入口 (FFI 的 `agentxx.agent.prompt` 写 `system` 键、插件
`mount_root` 的 `prompt.systemPrompt`、`client/util.cpp` 的 `makeSubAgentConfig`、
训练流程) 只要换掉 `systemPrompt`, 宿主契约就一起丢 —— 模型开始把 `[Error: ...]`
当用户正文、把 `[User canceled]` 当批准, 也不知道临时目录放哪。

## 2. 现状

### 2.1 拼装链 (改动不涉及)

`AgentContext::buildSystemPromptStable` / `buildSystemPrompt` =
`systemPrompt` → `orderedAppendSections()` (order 升序, 同 order 按键名) →
运行时稳定段 (memory 100 / skills 200) → 动态段 (仅界面查看) →
最后统一替换 `${work_dir}` / `${temp_dir}` / `${session_id}` / `${platform}`。

`buildSystemPromptStable` 在每次 modelcall 开头重建会话首条 system 消息
(`nodes/modelcall.cpp`), 因此拆出的段落仍进 system 消息、仍属稳定前缀。

### 2.2 现有自定义入口

| 入口 | 位置 | 换掉什么 |
|---|---|---|
| FFI/插件 prompt 接口表 | `plugin_manager_vtable.cpp` 的 `system` 键 | 只写 `systemPrompt` |
| 插件挂根 agent | `plugin_manager_runtime_config.cpp` 的 `prompt.systemPrompt` | 同上 (人设不继承父) |
| 客户端子系统配置 | `client/src/util/util.cpp::makeSubAgentConfig` | 新建 `AgentConfig`, 只换 `systemPrompt` |
| 训练 | `training.cpp` (`systemPrompt` + `appendSystemPrompts` 变异/补丁) | 字符串级改写 |

前三个入口都不改 `appendSystemPrompts`, 因此内置段能自动带上。

### 2.3 会受影响的位置

| 位置 | 影响 |
|---|---|
| `test_agent.cpp` 的 `## Working Directory` 断言 | 仍通过 (段照样拼进 system) |
| `test_summarization.cpp` 的出厂段剥离 | 目前只 erase 三个插件键, 内置段会漏; 改为清空整表 |
| `training.cpp` 变异 / 补丁 | 会把契约文本变异成乱码或整段覆盖; 需跳过内置段 |
| `assembly_snapshot.cpp` 的 `system_prompt_chars` | 从 ~7K 掉到 ~0.5K, 需补 append 段字符数 |
| `base_agent.cpp` 的 `ctx.instructions` | 只用于"会话首条非 system"的兜底, 不改 (正常轮次会重建) |

## 3. 方案

### 3.1 段划分

| 段 | 键 | 内容 |
|---|---|---|
| 人设 (留 `systemPrompt`) | — | 身份两行 + `## Working on Code` + `## Response Style` |
| 宿主运行时契约 | `core-runtime` | `## Agent Runtime` + `### Context Markers` |
| 会话路径 | `core-workdir` | `## Working Directory` (含路径占位符) |
| 通用准则 | `core-behavior` | `## Core Behavior` 中与具体代码任务无关的条目 |
| 技能使用说明 (既有) | `skill` | `## How to Use Skills` |
| 工作树 (既有) | `git-worktree` | `## Git Worktree Mode` |

顺序不单独设元数据: 这些段与插件段同为 order 0, 按 `(order, 键名)` 排列 →
`core-behavior` → `core-runtime` → `core-workdir` → `git-worktree` → `skill`。

`## Core Behavior` 里留在人设的条目 (代码 agent 专属):

- 用户只想讨论方案时先给方案, 确认后再写代码
- 改代码前先读懂现有代码并遵循项目约定
- 改完代码尽量验证 (构建 / 跑测试)
- 大改动先规划并在测试通过后复查
- `**Writing code comments**` 整块

拆到 `core-behavior` 的条目: 不是测试环境、先理解意图、用工具查证、benchmark 前先看
系统负载、破坏性操作先确认、大文档分模块写、报告实际做了什么、十六进制查看乱码、
回答准确具体。

### 3.2 代码形态

`prompt.h` 只增加键名常量与判定:

```cpp
inline static constexpr std::string_view kSectionCoreRuntime  = "core-runtime";
inline static constexpr std::string_view kSectionCoreWorkDir  = "core-workdir";
inline static constexpr std::string_view kSectionCoreBehavior = "core-behavior";
static bool isBuiltinSectionKey(std::string_view key) noexcept;
```

`prompt.cpp` 把三段正文直接写进 `appendSystemPrompts` 的初始化列表 (不设 order/source,
也不走 `setAppendSection`), `skill` 段同理; 段落文本先提到匿名命名空间的常量
(`R"_(...)"_` 定界符), 便于阅读。

### 3.3 训练防护

- `createChildVariantCharMut`: 跳过 `isBuiltinSectionKey` 的段 (契约文本被变异即失效)
- `normalizePromptPatch`: 丢弃补丁里的内置段键 (LLM 按 schema 可能返回 `core-runtime`)

## 4. 数据兼容

- `toJson` / `fromJson` / `mergeFromJson` 结构不变: 内置段就是普通 append 段,
  旧配置里没有这些键 → 保持出厂文本; 显式给 `null` → 删除 (允许的定制)
- `promptHash` 自动包含内置段 (段文本参与哈希), 训练种群去重语义不变
- 插件 `getPromptJson` / FFI 读到的提示词对象多三个段, 属预期 (与既有 `skill` 段一致)
- 无 ABI / 协议 / 配置键变化

## 5. 分步实施

| 步骤 | 内容 |
|---|---|
| S1 | `prompt.h`: 常量 + `isBuiltinSectionKey` + 注释 |
| S2 | `prompt.cpp`: 文本搬家, 三个内置段, `skill` 段统一走 `setAppendSection` |
| S3 | `training.cpp`: 变异与补丁跳过内置段 |
| S4 | `assembly_snapshot.cpp`: 补 `append_sections_chars` |
| S5 | 测试: `test_summarization` 清空出厂段; `test_agent` 新增回归断言 |
| S6 | 文档: `docs/zh-cn/design/index.md` 提示词章节、`plugins.md` §19 说明 |
| S7 | 编译与测试 |

## 6. 验证

- `agentxx_test agent prompt_stability summarization plugin_sdk plugin_runtime`
- 新增断言: 自定义 `systemPrompt` 后 `buildSystemPrompt` 仍含 `## Context Markers` /
  `## Working Directory` / `## Core Behavior`; `core-*` 段在 `orderedAppendSections`
  里排在 `skill` 之前
- 手工: 加载快照 `prompt.append_sections` 含三个 `core-*` 键, `system_prompt_chars`
  明显变小

## 7. 风险

| 风险 | 处理 |
|---|---|
| 契约段被配置/插件覆盖或删除 | 允许 (定制入口), 但训练不改; 诊断里 `owner` 显示 `core` |
| 前缀缓存一次性失效 | 文本顺序变化, 只影响切换后的第一次请求 |
| 段落顺序变化 (Response Style 提前) | 模型对顺序不敏感; 需要时可用负 order 调整 |
| 兜底路径 `instructions_` 只含人设 | 只在会话首条非 system 时触发; 正常轮次由 `buildSystemPromptStable` 重建, 不改 |
