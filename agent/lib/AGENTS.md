# agent/lib (libagentxx) 局部约束

本文件只写本目录内的硬约束；跨目录规则见仓库根 `AGENTS.md`。详细设计见
[docs/zh-cn/design/index.md](../../docs/zh-cn/design/index.md)。

## 目录职责

- `include/agentxx/**`：公开头（其他目录、嵌入方、插件 SDK 都能引用）。
- `src/**`：私有实现，`agent/client` 与 `agent/plugins` 一律不得引用
  （边界检查见测试模块 `boundaries`）。
- 子目录分工：`agent/`（会话生命周期、BaseAgent/CodeAgent、持久化骨架）、
  `io/`（端点与 wire 协议）、`nodes/`（图节点）、`middlewares/`、`protocol/`
  （provider 与对外协议）、`tools/`、`plugins/`（插件宿主实现）、`util/`（宿主
  专用工具：异常、sqlite、settings_db）。
- 三个自研工具库（`cxx_utilxx_base`/`cxx_utilxx`/`cxx_pluginxx`）在
  `agent/third_party/`，按独立工程维护，本目录不复制它们的实现。

## 新增能力的位置

- 核心骨架只做会话生命周期、上下文、持久化骨架与装配；新能力优先落到
  `nodes/`、`middlewares/`、`tools/`、插件或独立工具库（对应计划 ARC-7）。
- 不要继续把功能堆进 `agent/base_agent.cpp` 的 `init()`：装配步骤按名字登记，
  由统一的装配清单执行（对应计划 ARC-3）。

## 不变量

- **会话是 LLM 上下文的唯一权威**：`Session` 持有 typed 消息；图状态只放
  `xx_messagesMeta`、`xx_savedGraphData` 这类轻量影子信息，不得把 messages
  写进图 checkpoint。
- **取消是控制流**：用 `agentxx::util::catchError*` 系列，不要用 `catch (...)`
  把取消/中断吞成普通工具结果。
- **公开头自包含**：只包含自己需要的头，不假设包含者先包含别的头；不得引用
  `agentxx-client/*`（边界检查会失败）。
- **持久化改动必须走 schema 迁移**：`SessionStore` 的表结构变更写成相邻迁移
  步骤并记录版本，禁止直接改老表结构（对应计划 STO-2）。
- 中间件/节点返回值与工具结果文本形态是协议的一部分，改动要同步测试与文档。

## 构建与测试

- 修改后通过 `agentxx_test` 对应模块验证；新增测试模块在 `agent/test/core` 或
  `agent/test/plugin` 下，头文件使用 `agentxx-test/...` 完整路径。
- 不要修改 `agent/third_party/` 内的代码；确需改动时删除 `build` 内对应目录再
  重新配置编译。
