- 难度: B
- 类型: 功能调整 (TUI 消息列表)
- 时间: 2026-11-08
- 需求:
```md
请修改 tui 的 消息列表项，统一附件显示，如果消息存在附件，折叠时在消息列表项最右侧显示 `n个附件`，
展开时在该消息下方也就是末尾显示附件列表：
`|- n个附件（点击打开）\n|- xxx\n|- xxx`；
调整 user 消息的折叠状态，tui 收到时，如果 user 消息内容包含超过 5 个换行符 `\n`，
或是超过 200 字，则创建时状态为折叠，否则为展开显示状态
```

## 结论

两件事都只在**客户端 TUI** 内部完成, 不动 wire 协议、不动会话数据:

1. **附件显示统一** (`message_list.cpp` 的 User 分支):
   - 折叠态: 消息行最右侧 (行内 `filler()` 撑开) 显示 `n个附件`, 附件列表不画
   - 展开态: 附件列表放在消息**末尾** (正文之后), 首行 `|- n个附件（点击打开）`,
     其下每条附件一行 `|- 类型: 文件名 大小`, 点击行仍在文件管理器中定位该文件
   - 高度估算 (`quickHeight`) 与实际排版对齐 (折叠 1 行 / 展开 = 正文行 + 1 + 附件数)
   - 新增 i18n 键 `msg.attachCount` / `msg.attachCountOne`; `msg.attachOpen` 文案改为
     `（点击打开）` / `(click to open)`
2. **用户消息初始折叠状态** (`tui_state.h` 的 `shouldCollapseUserMessage` +
   `agent_tui.cpp` 的接收路径): 正文换行符 > 5 个或按码点计 > 200 字 → 建行时折叠,
   否则展开。判定发生在行刚由副本创建的那一刻 (insert_message / 整份同步 / 增量补拉 /
   历史分页前插), 本地行 (中断输入回显) 在创建时按同一规则置位; 用户之后的点击不会被覆盖。

## 关键决策

| 决策 | 理由 |
|---|---|
| 折叠判定放在 TUI 接收侧, 不放到 agent 端 | 按需求"tui 收到时"实现; 只是展示默认值, 不需要写进会话历史, 也不影响其它端点 |
| 判据按 unicode 码点计"字" | 中文按 1 字计, 不按 UTF-8 字节 (3 字节) 或显示列 (2 列) —— 否则中文 70 字就被折叠 |
| 只在行创建时判定, 之后不再改写 | 用户在界面上展开/收起的状态是他们的选择, 不能被后续增量覆盖 |
| 本地行 (`local_` 前缀) 在创建时置位, 接收路径跳过它们 | `onSync` 整体替换时本地行被搬到新状态里, 对象与上一帧快照共享; 在那里改写会与 UI 线程竞争 |
| 附件行仍是自己的命中区 (折叠态没有命中区) | 折叠行只有一行, 整行点击就是展开/收起; 展开态每行点击定位自己的文件, 与改造前行为一致 |
| 阈值用常量而非配置键 | 需求没要求可配置; 常量集中在 `tui_state.h` (`kUserMessageFoldMaxNewlines` / `kUserMessageFoldMaxChars`), 需要时再加配置键 |

## 影响面

- 只改客户端: `agent/client/{include,src}/io/tui/*` 与 `agent/test/client/test_tui_scroll.cpp`,
  文档 `docs/zh-cn/design/tui.md` §2.18/§2.19
- wire 协议、会话库、插件接口表均未改动 (不需要重新编译插件)
- 老数据兼容: 已落盘的用户消息没有 `collapsed` 语义差异, 同步后按同一规则重新定初始状态

## 验证

- 全量 `agentxx_test --log-level warn`: `Total: passed=42295 failed=1`, 唯一失败是
  `boundaries` 规则 15 (称呼与用词) 命中 `agent/third_party/cxx_pluginxx_ui` 里既有的
  "载体" 用词, 与本次改动无关 (改动文件的用词检查通过)
- 相关模块: `tui_scroll` 961 / `tui_stream` 192 / `tui_tool_header` 169 /
  `tui_interrupt` 303 / `tui_input` 124 全绿
- 客户端 `agentxx` (Debug) 编译通过

细节见同目录 [plan.md](plan.md) 与 [work.md](work.md)。
