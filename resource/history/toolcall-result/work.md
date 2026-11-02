# 工具结果状态重构 (toolcall-result) 实施记录

> 设计见同目录 [plan.md](plan.md)。本文件记录**实际执行顺序、已完成内容、待完成内容与注意事项**,
> 每完成一个阶段就更新一次并单独提交。

## 总体进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 依赖库拼写统一 (`Canceled`) + `awaitXXXOp` 改名 | 已完成 |
| P1 | `util/tool_result.h` + `XXToolBase` 签名 + lib 内工具 + 执行层状态机 | 未开始 |
| P2 | Wire 字段与版本 + `ToolData`/`ViewMessage` + 客户端渲染 | 未开始 |
| P3 | 插件边界 (`PluginTool` 映射/超时) + API 4 + tools 表 v3 + SDK + JS 引擎 | 未开始 |
| P4 | 内置插件改造 (filesystem / math / execute_command + 审计其余) | 未开始 |
| P5 | 测试补齐 + 拼写扫描 + 全量回归 | 未开始 |
| P6 | 文档与生成物 | 未开始 |

## 环境

- Windows debug 构建: `agent/build/windows-debug` (Debug + ASan), 用
  `cmake --build agent/build/windows-debug --config Debug --target <target>` 增量编译
  (环境变量 `VSLANG=1033` / `VCPKG_ROOT=` / `VCPkgLocalAppDataDisabled=1`;
  需要 VS 环境时走 `agent/script/windows_debug_build.bat`)
- 目标名: `cxx_utilxx_base_repo` / `cxx_utilxx_repo` / `cxx_pluginxx_repo`(依赖库, 改头文件后先编它们)、
  `agentxx_lib_repo`、`agentxx_test_repo`、`agentxx_client_repo`
- 测试: `agent/build/windows-debug/exec/agentxx_test.exe <模块...>`

## P0 已完成 (依赖库拼写统一 + awaitXXXOp 改名)

改名的公共契约:
- `utilxx::CancelledException` -> `utilxx::CanceledException`
- `utilxx::CancelToken::isCancelled()` -> `isCanceled()`, `throwIfCancelled()` -> `throwIfCanceled()`
- `utilxx_base::ControlFlowKind::Cancelled` -> `Canceled`
- `pluginxx::CancelledException` -> `pluginxx::CanceledException`; `OpCtl::throw_if_cancelled()` ->
  `throw_if_canceled()`; `CancelRegistry::isCancelled/isCancelledKey` 一族同步改成 `canceled`
- `PLUGINXX_OPERATOR_CANCELLED` -> `PLUGINXX_OPERATOR_CANCELED`
- `pluginxx::awaitPluginOp` -> `awaitPluginOpResult`, 返回新的 `pluginxx::PluginOpResult{status, payload}`
  (**不再为业务失败抛异常**; 等待本身被取消仍按原样抛), 旧名直接删除
- `agentxx::util::awaitHostPluginOpResult` 返回 `PluginOpResult`, 把 `utilxx::CanceledException`
  换算成 `PLUGINXX_OPERATOR_CANCELED`; 新增 `agentxx::util::requirePluginOpOk()` 给
  "非 OK 就抛" 的调用点 (图节点 / 钩子派发, 取消仍抛 `neograph::graph::CancelledException`)
- agentxx 侧 `KeyMetrics::TurnOutcome::Cancelled`/`ToolOutcome::Cancelled`/`turnsCancelled()`/
  `toolCancelled()` 等一并改成 `Canceled`; `agentxx.agent.session` 表的 `is_cancelled` 成员改名为
  `is_canceled` (仅名字, 布局不变)

保留原拼写的地方 (按要求不动): `neograph::graph::CancelledException`、`CancelToken::is_cancelled()`、
`throw_if_cancelled()`, 以及 asio 自带的 `asio::experimental::channel_errc::channel_cancelled`。

依赖库子模块已各自提交 (`cxx_utilxx_base` f5e0c6e / `cxx_utilxx` aacfbe6 / `cxx_pluginxx` 0a8fc82)。

验证: `cxx_utilxx_base_repo`/`cxx_utilxx_repo`/`cxx_pluginxx_repo`/`agentxx_lib_repo`/
`agentxx_test_repo`/`agentxx_client_repo` 全部编译通过;
`agentxx_test plugin_runtime plugin_sdk plugins` = passed 1451 / failed 0。

## 实施中的注意事项 (踩过的坑)

- **PowerShell `-replace` 默认大小写不敏感**: 用它做 "只改大小写" 的替换会连带命中大写形式,
  且 `-ne` 比较也是大小写不敏感 —— 用 `-creplace` + `-cne` 才能可靠判定是否需要写回。
  批量改标识符时先用占位符保护第三方标识符 (`neograph::graph::CancelledException`、
  `is_cancelled`、`channel_cancelled`), 占位符本身不能含被替换的词。
- 改 `agent/third_party/cxx_*` 的头文件后, 要先编 `cxx_*_repo` 目标 (它们把头文件 install 到
  `agent/build/windows-debug/agentxx-project-install/include`), 否则插件编到的是旧头。
- `AGENTS.md` 在工作区里原本就有一处与本任务无关的未提交改动 (注释示例被改写),
  本次只在 P6 追加自己的段落, 不回滚它。
- 阶段结束时只 `git add` 本阶段相关文件, 不碰 `TODOS.md` / `README.md` 与
  `agent/third_party` 里无关的改动。
