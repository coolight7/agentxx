# 实施记录: 适配 cxx_pluginxx 0.2.0 的插件体系重构

> 方案见同目录 `plan.md`；任务卡片见 `index.md`。
> 本轮（2026-11-10）只完成方案设计，代码实施尚未开始。
> 计划分 8 步：步骤 0 改库 → 步骤 1~7 改主程序/插件/测试/文档（见 `plan.md` §6）。

## 当前进度

| 阶段 | 状态 |
|---|---|
| 方案设计（plan.md） | 已完成 |
| 步骤 0 库改动（`cxx_pluginxx` L1~L4/L6/L7、`cxx_utilxx` L5）+ 子模块指针 | 待开始 |
| 步骤 1 构建与接口表骨架 | 待开始 |
| 步骤 2 agent 侧（DomainServices + 广播 + 中枢） | 待开始 |
| 步骤 3 功能埋点（替换钩子与功能点） | 待开始 |
| 步骤 4 网络（`agentxx.net` + 4 个插件迁移） | 待开始 |
| 步骤 5 client 侧（广播 + net） | 待开始 |
| 步骤 6 插件全量迁移 + JS 引擎 | 待开始 |
| 步骤 7 测试、文档、收尾 | 待开始 |

## 实施记录（按步骤回填）

### 步骤 0：库改动

- 库提交号（`cxx_pluginxx` / `cxx_utilxx`）与 tag：
- 改动文件：
- 验证（两库编过 + `api_abi_check`）：
- 与计划的偏差：

### 步骤 1：构建与接口表骨架

- 改动文件：
- 验证：
- 与计划的偏差：

### 步骤 2：agent 侧（DomainServices + 广播 + 中枢）

- 改动文件：
- 验证：
- 与计划的偏差：

### 步骤 3：功能埋点

- 改动文件：
- 验证（`plugin_point`）：
- 与计划的偏差：

### 步骤 4：网络

- 改动文件：
- 验证（`plugin_net` + 4 插件本地回环）：
- 与计划的偏差：

### 步骤 5：client 侧

- 改动文件：
- 验证：
- 与计划的偏差：

### 步骤 6：插件全量迁移

- 改动文件（按 §4.12 分批记录）：
- 验证：
- 与计划的偏差：

### 步骤 7：测试、文档、收尾

- 新增/改造测试模块：
- 文档：
- 与计划的偏差：

## 验证清单（实施完成后逐条勾）

- [ ] 两个库 Debug/Release 编过，`api_abi_check` 与新增静态断言通过
- [ ] `windows_debug_build.bat` 全绿（lib + client + test + benchmark + 插件目标）
- [ ] `agent/script/build_plugins.ps1` 全部插件产物时间戳更新（26 个）
- [ ] `agentxx_test --log-level warn` 全模块通过（debug + ASan）
- [ ] `boundaries` 规则 7（21 / 17 张表）、`IfaceMeta` 特化数、埋点 id 头与声明表一致
- [ ] 能力行 scope 隔离 / 宿主发起调用 / 广播 end / 埋点原语 四项库语义各有一条测试
- [ ] 4 个网络插件本地回环往返通过；`grep utilxx/http_client.h agent/plugins/` 为空
- [ ] 手工：空列表启动 26 插件全加载、插件禁用→启用账目归零再回原值、chatbot 断网重连、
      客户端重连后插件面板与装饰重建
- [ ] 文档：`plugins.md`、`index.md`、`events.md`、`AGENTS.md`、`plugins/AGENTS.md`、
      `docs/zh-cn/design/` 内相关分册

## 遗留与注意事项

- 库改动（L1~L4）是本轮前提：能力行带 scope、宿主（无插件租约）发起调用、广播 `end`、
  功能埋点原语。主程序侧**不再**自己实现这些，避免与库两套语义。
- 库仓库当前没有自带测试工程，L1~L4 的语义由本项目 `plugin_point` / `plugin_broadcast` /
  `plugin_net` 覆盖；若库仓库后续补测试，把这三处断言搬过去即可。
- 若某插件迁移成本远超预期（例如 chatbot 的客户端事件面 / CDP 帧细节），
  拆出独立任务记录，不阻塞主线；`plan.md` §8 的 R6/R7 已列出等价性核对点。
- 抬代次后**主程序与插件必须同版本发布**：只替换主程序会让全部动态库插件被拒（`plan.md` §8 R8）。
