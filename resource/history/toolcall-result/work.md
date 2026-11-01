# 工具结果状态重构 (toolcall-result) 实施记录

> 设计见同目录 [plan.md](plan.md)。本文件记录**实际执行顺序、已完成内容、待完成内容与注意事项**,
> 每完成一个阶段就更新一次并单独提交。

## 总体进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 依赖库拼写统一 (`Canceled`) + `awaitXXXOp` 改名 | 未开始 |
| P1 | `util/tool_result.h` + `XXToolBase` 签名 + lib 内工具 + 执行层状态机 | 未开始 |
| P2 | Wire 字段与版本 + `ToolData`/`ViewMessage` + 客户端渲染 | 未开始 |
| P3 | 插件边界 (`PluginTool` 映射/超时) + API 4 + tools 表 v3 + SDK + JS 引擎 | 未开始 |
| P4 | 内置插件改造 (filesystem / math / execute_command + 审计其余) | 未开始 |
| P5 | 测试补齐 + 拼写扫描 + 全量回归 | 未开始 |
| P6 | 文档与生成物 | 未开始 |

## 环境

- Windows debug 构建: `agent/script/windows_debug_build.bat` (Debug + ASan)
  产物目录 `agent/build/windows-debug/exec`, 测试程序 `agentxx_test.exe`
- 单模块测试: `agentxx_test <模块名> ...`

## 注意事项 (实施中随时补)

- `neograph` 的 `CancelledException` / `is_cancelled()` **不改**, 引用处原样保留。
- 改了 `agent/third_party/cxx_*` 后需让 ExternalProject 重新编译对应目录
  (本次改的是头文件与源文件, 构建系统会自行判定重编)。
- 阶段结束时只 `git add` 本阶段相关文件, 不碰 `TODOS.md` / `README.md` 与
  `agent/third_party` 里无关的改动。
