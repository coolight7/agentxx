# agent/test 局部约束

本文件只写本目录内的硬约束；跨目录规则见仓库根 `AGENTS.md`。

## 目录与 include

- 源码按领域分目录：`core/`（lib 核心）、`plugin/`（插件）、`client/`（界面）。
- 头文件统一放 `include/agentxx-test/{,core/,plugin/,client/}`，与源码目录同名
  对应；`include/` 是测试唯一的 include 根，源码一律写完整路径
  （`#include "agentxx-test/core/test_xxx.h"`），不要用相对路径或短名引用，
  避免与其它库同名头文件歧义。

## 模块注册

- 同步模块在 `test.cpp` 的 `runSync("name", ...)` 列表注册，异步模块在
  `ioCtx` 协程里的 `run(...)` / `runCtx(...)` 列表注册；未注册的测试等于不存在。
- 模块名用小写下划线（如 `session_persistence`），与运行参数
  `agentxx_test <module>` 一致。

## 用例写法

- 断言用 `test_framework.h` 的 `XX_TEST_EXPECT_*` 宏；模块 cpp 内定义匿名
  命名空间计数器（`g_xxx_passed` / `g_xxx_failed`）并用 `#define XX_TEST_PASSED`
  / `XX_TEST_FAILED` 覆盖，函数末尾 `return TestResult{g_xxx_passed, g_xxx_failed};`
  —— 头文件只保留函数声明，不导出计数器，避免跨模块计数错乱。
- 每个功能至少覆盖：正常路径、边界值、失败/异常路径（并发改动要测两种顺序）。
- 测试不得依赖真实网络、真实模型额度或机器特定路径；需要模型响应时用替身
  provider 或本模块内的假实现。
- 临时文件写到 `fs::temp_directory_path()` 下的唯一子目录，测试结束自行清理。
- 加 `XX_TEST_EXPECT_*` 前先想清楚"这条断言失败说明什么"，避免写只能证明
  自己实现的空断言。

## 门禁与快照

- 一键门禁：`agent/script/gate.sh`（Linux/macOS）或 `agent/script/gate.ps1`
  （Windows）。改动渲染、协议、配置、持久化后至少跑一次
  `agentxx_test -f`（fail-fast，含 `boundaries` 边界检查）。
- UI 快照基线在 `agent/test/snapshots/ui/`（固定尺寸纯文本画面 + 命中区清单），
  由 `ui_snapshot` 模块比较；渲染改动后人工确认差异并更新：
  `AGENTXX_UPDATE_UI_SNAPSHOTS=1 agentxx_test ui_snapshot`，然后 review
  git diff 再提交（不要直接无条件更新基线）。
