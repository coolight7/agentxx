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
- 追加需求 (同一轮): 附件段不要只给用户消息, 要提升为**所有消息角色共用**的支持

## 结论

两件事都只在**客户端 TUI** 内部完成, 不动 wire 协议、不动会话数据:

1. **附件段是所有角色共用的** (`message_list.cpp`):
   - 展开态 (含不可折叠角色): 列表放在消息**末尾** (正文/参数/结果/中断控件之后),
     首行 `|- n个附件（点击打开）`, 其下每条附件一行 `|- 类型: 文件名 大小`,
     点击行在文件管理器中定位该文件
   - 折叠态: 折叠头行最右侧 (`filler()`) 显示 `n个附件`, 列表不画; 各角色预览的列宽
     预算都扣掉该计数
   - `Assistant` / `Interrupt` 没有折叠形态, 带附件时总是展示列表
   - 三个共用件: `appendAttachmentList` (展开态, 成员函数, 登记命中区) /
     `appendAttachmentCount` (折叠态头部) / `attachmentListLinesOf` (高度估算);
     可折叠角色名单唯一来源 `isCollapsibleRole` (渲染分支 + 折叠命中登记共用)
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
| 附件段抽成所有角色共用 (不写在 User 分支里) | 附件是消息的通用字段; 每个角色分支只负责自己的正文, 末尾统一追一段列表, 避免每个角色各写一份 (且容易漏估高度) |
| 折叠态的计数画在折叠头行最右侧 | 折叠行只有一行, 数量放同一行才能不额外占行; 折叠列表展开会一屏放不下 |
| 可折叠角色名单统一为 `isCollapsibleRole` | 同一名单同时用于渲染分支、折叠命中登记、附件排版与高度估算, 避免两边写不一致 |
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
- 相关模块: `tui_scroll` 979 / `tui_stream` 192 / `tui_tool_header` 169 /
  `tui_interrupt` 303 / `tui_input` 124 / `tui_widget` 194 全绿
  (另跑过 `tui_lazy_view` 3851 / `ui_items` 117 / `ui_snapshot` 22 / `view_replica` 114)
- 客户端 `agentxx` (Debug) 编译通过

## 后续调整 (同日, 用户自己改的)

1. **user 消息不再兼容 event**: `ViewMessage::fromJson` 删掉了把 `role=user` + `source=event:*`
   归一化成 `Role::Event` 的兼容代码, 渲染端 User 行也不再画事件来源头 —— 这类历史消息
   就是普通用户消息 (`Role::Event` 只由 `roleOfInputSource` 在写入侧给出)
2. **折叠态用户消息预览不再 `theme.dim()`**: 与正文同色 (Event/Think 等其它角色的预览仍是 dim)

测试与生成物随之同步: `test_tui_stream` 的老数据断言改为 User / 不折叠 / source 保留;
`test_wire_schema` 的 sync 示例改用 `Role::Event` 并重新生成 `wire-schema.json` 与
`wire-protocol-fields.md` (sync 的 `messages` 字段表补 `collapsed:boolean`)。

细节见同目录 [plan.md](plan.md) 与 [work.md](work.md)。
