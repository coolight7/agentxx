# 修复 TUI 中文乱码 + 末尾消息间距/画面漂移 (控制台代码页被子进程改掉)
- 难度: C
- 类型: bug修复
- 基于commit: 05d4a915
- 时间: 2026-10-25
- 需求:
```md
修复最近的 agentxx 会话，它目前的 TUI 末尾的消息列表渲染很奇怪，tool 消息的边距非常大，
但前面的消息都正常，唯独最近的消息开始有问题。这可能还跟另一个问题有关系：之前出现过
TUI 整个窗口的中文原本都好好的，运行一段时间后偶现中文显示都乱码，且此时 UI 就卡住了，
但会话仍然在运行，所有重启就好了，消息记录什么的都在。
```

## 现象

两个症状其实是同一件事, 常一起出现:

1. 中文整屏乱码: 原本显示正常的中文突然全变成乱码, 英文/路径类文本仍可辨认。
2. 末尾 (消息列表下半部分, 即最新消息处) 间距异常, 像被撑大了边距: 渲染看起来
   "错位", 画面持续往下漂并不断留下空白行, 视觉上像界面卡住 (不再正常刷新)。
   重启进程后立刻恢复, 会话数据没有任何损坏。

## 根因

- TUI 依赖"控制台输出代码页 = UTF-8"与 FTXUI 启动时设置的控制台模式, 而这两项属于
  **控制台**的共享状态 (conhost / Windows Terminal), 不是进程私有的。
- `agentxx_execute_command` (以及 MCP server) 之前创建子进程时只重定向了 std 句柄,
  **控制台仍然共享**: 命令里任何调用 `SetConsoleOutputCP` 的程序 (`chcp`、批处理脚本、
  部分原生工具) 都会改掉父进程所在控制台的代码页, 子进程退出后也不会恢复。
  实测: 父进程 CP=65001, 执行 `cmd /c "chcp 936 >nul"` 之后父进程 CP 变 936;
  同一个子进程改用 `CREATE_NO_WINDOW` 启动则父进程 CP 保持 65001。
- 代码页一改, 后果有两层:
  1. TUI 写的是 UTF-8 字节, 终端按 936 解码 -> 中文整屏乱码。
  2. 列宽假设失效: 一个中文字在 UTF-8 里 3 字节, 按 936 解码约占 3 列, 而 FTXUI
     按 2 列排版 -> 含中文的行实际宽度超出屏幕宽度, 终端自动折行。FTXUI 每帧输出
     用 `Screen::ResetPosition(false)` 的**相对定位** (`\r` + 光标上移 `dimy-1` 行)
     回到帧首, 折行数多了以后定位落在错误的行 -> 画面逐帧下移、底部不断留下空白
     (看起来就是"最新消息的边距特别大"), 并且每帧都在滚屏, 表现为界面卡住。
     重启用 `SetConsoleOutputCP(CP_UTF8)` + 全新一帧, 于是"重启就好"。
- 同源风险: 子进程把 `ENABLE_VIRTUAL_TERMINAL_PROCESSING` / `..._INPUT` 关掉时,
  终端不再解释 ANSI 序列、鼠标与按键事件失效 (同样是"画面乱 + 界面卡住")。

## 排查过程 (消息列表本身没问题)

把出问题的会话 `session.db` 的 `view_message` 导出成 JSON, 用 `tui_scroll` 的测试夹具
按三种方式驱动真实消息列表 (`MessageListComponent`) 渲染并 dump 屏幕:
- 一次性放入尾窗 100 条;
- 逐条追加 + 超过本地窗口上限 (100) 后按 `onHistoryTrimmed` 清理头部 (实时客户端行为);
- 按 wire 增量重放 (`ToolStart` 运行中 -> `ToolEnd` 回填结果, 正文/思考按 token 流式累积)。

三种方式在 140x45 下渲染结果一致且正常 (每条消息 1 行内容 + 1 行尾部空行, 无多余空白),
逐条单独测量的高度也全是 2 行 —— 说明消息列表的布局/高度缓存/懒构建没有回归,
异常来自终端侧 (代码页/列宽), 不是列表排版。

后续又做了**全量**测量 (不留死角): 从 `session.db` 导出该会话全部 1584 条 `view_message`,
逐条单独放进消息列表 (97 列宽, 6 行视口迫使首条被实测) 读 `totalHeight()`:

| role | 条数 | 渲染高度 |
|---|---|---|
| tool | 1021 | **全部 2 行** |
| thinking | 500 | 全部 2 行 |
| tip | 6 | 全部 2 行 |
| assistant | 53 | 44 条 2 行, 5 条 3 行, 4 条含 markdown 表格 61~66 行 |
| user | 4 | 2~6 行 (长 prompt 折行) |

数据侧另有 531 条 tool 结果带 2 个尾部换行 (`\n\n`), 渲染只留 1 行尾部空行 (尾部空行被归一),
内容里也没有 3 行以上连续空行 —— 因此**任何一条 tool 消息都不可能显示出大边距**;
用户看到"一部分 tool 消息边距很大"只能是终端侧漂移 (见下一条)。

## 修改

- 新增 `agent/client/include/agentxx-client/util/console_state.h` +
  `agent/client/src/util/console_state.cpp`
  - `ConsoleStateGuard::check()`: 每帧一次 `GetConsoleOutputCP` 自检; 代码页不是 UTF-8 就
    改回 (输入代码页同样); 控制台模式只看 TUI 真正必需的位 —— 输出
    `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, 输入 `ENABLE_VIRTUAL_TERMINAL_INPUT|ENABLE_WINDOW_INPUT`
    (见下方"第一版判据的错误")。没有控制台 (`GetConsoleOutputCP() == 0`) 或 std 句柄被
    重定向 (`GetConsoleMode` 失败) 时按可用范围处理, 不误报。
  - 写回失败 (终端不支持该位) 时记"放弃重试"标记 (`gaveUp()`): 否则每帧都判定为缺失,
    调用方就会每帧清屏 -> 画面持续闪烁。
  - `consoleRealignSequence()`: `\x1b[0m\x1b[2J\x1b[H`。漂移后 FTXUI 的相对定位无法
    自己回来, 必须清屏 + 回原点, 让下一帧从帧首重新对齐。
- `agent/client/src/io/tui/agent_tui.cpp`
  - 主渲染器 (每帧开头) 调 `consoleStateGuard_.check()`; `needResync` (已成功写回) 时先把
    `consoleRealignSequence()` 写进 `std::cout` (排在 FTXUI 本帧输出之前), 使画面
    立即重新对齐; 日志限频 (首次 + 每 100 次)。
  - `agent_tui.h` 增加 `consoleStateGuard_` 与 `consoleStateRepairCount_` (UI 线程独占)。
- `agent/client/src/io/stdio/agent_stdio.cpp`
  - `cli` (stdio) 模式的行式输出同样会被改掉的代码页影响: `onDelta` / `onPeerMessage`
    开头自检 (只多一次系统调用), 行式输出不需要清屏重排; `StdIOClientAgentIO`
    持有 `consoleStateGuard_`。
- `agent/plugins/agentxx_execute_command/execute_command_impl.h`
  - Windows 子进程加 `CREATE_NO_WINDOW` (`boost::process v2` 的
    `process_creation_flags<CREATE_NO_WINDOW>`): 子进程使用独立的无窗口控制台,
    命令输出仍走管道 —— 从源头避免 `chcp` 等改掉 agent 自己的控制台状态。
    (popen 回退路径无法指定创建标志, 由 TUI/CLI 侧自检兜底)
- `agent/lib/src/protocol/mcp_client.cpp`
  - MCP server 同样改为 `CREATE_NO_WINDOW` 启动: 它只经管道通信, 不需要控制台,
    也不该能改掉同一进程里 TUI 的控制台状态。
- 测试: 新模块 `console_state` (`agent/test/client/test_console_state.cpp`),
  校验代码页被改成 936 后能修复并回报原值、正常状态幂等、缺 VT 位时补回、重新对齐
  序列内容, 以及"行输入/回显位变化不算被改动"的回归用例; 测试会还原所在终端的
  代码页与模式。std 句柄被重定向时 (CI/包装脚本) 测试用 `CONOUT$`/`CONIN$` +
  `SetStdHandle` 临时指向本控制台, 否则模式相关用例测不到。

## 第一版判据的错误 (启动即误报 + 动画闪烁)

第一版把 `ENABLE_LINE_INPUT` / `ENABLE_ECHO_INPUT` 为 0 也当成必需条件 (按 FTXUI
`Install` 里的写法推断: 它会清这两位)。结果:

- 实测运行中的 TUI 控制台 `in mode = 0x03EF`, `LINE_INPUT`/`ECHO_INPUT` 恒为 1 ——
  conhost 在开启 VT 输入后仍保持这两位 (与 FTXUI 写入的值无关), 于是**每帧**都判定
  "被外部改动", 每帧写一次清屏序列 -> 启动动画期间画面持续闪烁, 日志里反复出现
  `控制台模式缺失=true` (而 `输出代码页=0` 表示代码页其实一直是对的)。
- 定位方式: 用 AttachConsole + `CreateFile("CONOUT$"/"CONIN$")` + `GetConsoleMode`
  **只读**读取运行中 TUI 实例的控制台模式, 得到 `out mode=0x000D`
  (`VT_PROCESSING`/`DISABLE_NEWLINE_AUTO_RETURN` 都在), `in mode=0x03EF`
  (`VT_INPUT`/`WINDOW_INPUT` 在, 但 `LINE_INPUT`/`ECHO_INPUT` 也在)。
- 修正: 判据只留 TUI 正常工作必需的位 (输出 VT 处理; 输入 VT 输入 + 窗口事件),
  `LINE_INPUT`/`ECHO_INPUT`/`WRAP_AT_EOL` 等由终端自己维护的位一律不管, 也不改写;
  修复只在真正写回成功 (`needResync`) 时才清屏, 写不回去就停止重试。
- 结论: 这类自检的判据必须是"缺了就一定坏"的位; 把"希望对端保持的位"当判据,
  只会在正常环境里每帧误报。

## 回归保护

`agentxx_test console_state` (23 个断言; Windows 有控制台时) + 既有
`tui_scroll` / `tui_lazy_view` / `tui_surface` / `tui_widget` / `tui_tool_header`
(消息列表布局与渲染) 全部通过。其中 `console_state` 专门覆盖了上面那次判据错误:
把 `LINE_INPUT`/`ECHO_INPUT` 显式打开后自检必须**不**报告修复、也不擅自改这两位。

## 后续注意

- 任何直接写终端的客户端输出 (TUI/CLI 后续新增的表现层) 都应在输出前自检控制台代码页,
  或确保工具子进程不共享控制台 (Windows: `CREATE_NO_WINDOW`)。
- FTXUI 的帧首定位是相对的, 因此**终端列宽必须与 FTXUI 的 `string_width` 一致**:
  除代码页外, 零宽字符、宽窄不确定字符 (ambiguous width)、emoji 也会造成同样的漂移;
  在消息内容里引入这类字符时需要评估 (当前代码页问题是主要来源)。
