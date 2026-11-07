# 方案: TUI 消息列表附件显示与用户消息初始折叠

对应任务卡片见同目录 [index.md](index.md), 实际改动与验证见 [work.md](work.md)。

## 1. 背景与现状 (实施前代码)

### 1.1 附件显示 (改造前)

`agent/client/src/io/tui/components/message_list.cpp` 的 `buildMessageBlock` User 分支:

```text
|- 📷 图像附件: photo.png 1.2 KB ( 点击显示 )       <- 附件卡片, 在正文**上方**
|                                                    <- 有附件时多一行分隔符
- 正文...
```

- 卡片只在 `Role::User` 分支渲染, 每个附件一个 `hbox` + `reflect` 命中区
  (`attachmentHits_`, 点击在文件管理器中定位文件)
- 折叠态也照常显示卡片 (`+ 预览` 行上方), 折叠并没有真的"收起"附件占用的行
- 预估高度 `quickHeight` 里对应 `textLines + attachments.size() + 1(分隔行) + 1(尾部空行)`

### 1.2 用户消息的折叠状态 (改造前)

- `ViewMessage::collapsed` 的默认值由 agent 端决定 (`makeText` 只对 Tip/Event 置折叠),
  用户消息恒为展开
- TUI 侧只有"点击切换" (`handleCollapsibleClick` → `mutableMessage`), 没有默认值逻辑
- 长输入 (粘贴几百行) 一进来就把整屏占满, 需要滚过去才能看到回复

### 1.3 接收路径 (消息行是怎么进 TUI 的)

`agent/client/src/io/tui/agent_tui.cpp` (client io 线程, 全程持 `sharedState_.mutex()`):

| 入口 | 副本动作 |
|---|---|
| `onDelta` 的 `InsertMessage` | `replica.applyDelta(delta)` (按 id upsert) |
| `onSync` (整份) | `replica.reset(payload)` (本地行 `local_` 保留并搬到末尾) |
| `onSync` (增量) | `replica.appendIncremental(payload)` (按 id 去重追加) |
| `onViewMessagesPage` (历史分页) | `replica.prependRows(page.messages)` (前插到窗口上方) |
| 中断输入回显 (本地行) | `pushLocalRowLocked` → `replica.upsertLocalRow` |

`ViewReplica` 的行是 `shared_ptr<ViewMessage>`: 新行由副本 `make_shared` 创建,
`onSync` 时本地行是**搬旧对象**, 因此只有"新创建的行"可以就地改写。

## 2. 方案

### 2.1 附件显示 (统一形态)

```text
折叠态 (单行):
+ 正文首行预览...                                        3个附件

展开态 (正文之后):
- 正文...
|- 3个附件（点击打开）
|- 📷 图像附件: photo.png 12.1 KB
|- 🎬 视频附件: clip.mp4 2.0 MB
```

- 折叠行: `hbox({ "+ ", 预览|xflex_shrink, filler(), "n个附件" })`; 预览的列宽预算
  (`collapsedPreviewBudget`) 扣掉右侧计数 (含 1 列间隔), 避免长预览把计数挤出去
- 展开: 正文行之后依次 push 计数头行与每个附件行; 附件行保留各自的 `reflect` 命中区
- 文案: 新增 `msg.attachCount` (`{}个附件` / `{} attachments`) 与 `msg.attachCountOne`
  (`1个附件` / `1 attachment`); `msg.attachOpen` 从 `( 点击显示 )` 改为
  `（点击打开）` / ` (click to open)` (现在只出现在计数头行)
- `quickHeight` 同步改: 折叠 1 行; 展开 = 正文行数 + (有附件时) `1 + 附件数`; 两者都 +1 尾部空行

### 2.2 用户消息初始折叠

- 判据: `agent/client/include/agentxx-client/io/tui/framework/tui_state.h`
  ```cpp
  inline constexpr size_t kUserMessageFoldMaxNewlines = 5;   // > 5 个 \n -> 折叠
  inline constexpr size_t kUserMessageFoldMaxChars    = 200; // > 200 字  -> 折叠
  bool shouldCollapseUserMessage(std::string_view text) noexcept;
  ```
  字数用 `utilxx_base::utf8GetLength` (码点数), 不按字节/显示列
- 应用点 (`agent_tui.cpp`):
  - `onDelta` 的 `InsertMessage`: 写入副本后按 id 取回新行 (`rowById`) 判定
  - `onSync` 整份: `reset` 之后遍历全部行 (本地行自动跳过)
  - 增量补拉: 只遍历本次追加的尾巴
  - 历史分页前插: 只遍历前插进来的 `prependedCount` 行
  - 中断输入回显 (本地行): 建行时直接 `collapsed = shouldCollapseUserMessage(text)`
- 只写 `true`, 不写 `false`: agent 若自己给了折叠也保留; 用户的点击状态永远以行为准
- 本地行在接收路径里跳过 (原因见 §1.3, 会与 UI 线程读同一对象)

## 3. 边界与取舍

- **整份同步会重置用户的手动展开**: `reset` 重建所有行, 长用户消息又回到折叠。
  与其它角色的折叠状态一致 (agent 也是每次同步下发自己的 `collapsed`), 可接受
- **其它端点 (stdio/CLI/FFI) 不受影响**: 折叠默认值只写在 TUI 的接收逻辑里,
  `ViewReplica` 行为不变
- **附件只在 `Role::User` 上出现**: 事件/工具等角色不会带 `attachments`, 因此
  "统一"指同一条消息在折叠/展开两种形态下都有固定的附件呈现位置
- **超长文件名仍会被右缘裁剪**: 与改造前的卡片一致 (FTXUI 只裁剪不折行), 未额外加折行逻辑

## 4. 分步实施

1. `tui_state.h`: 阈值常量 + `shouldCollapseUserMessage`
2. `message_list.cpp`: User 分支渲染改写 (折叠右侧计数 / 展开末尾列表) + `quickHeight` 同步
3. `message_list.h` 注释 (附件"卡片"→"行")
4. `tui_i18n.cpp`: 新增/更新附件文案
5. `agent_tui.cpp`: 四个接收路径 + 两处本地行
6. `tui_scroll` 新增两个场景 (附件两种形态 / 折叠阈值与接收路径)
7. `docs/zh-cn/design/tui.md` §2.18 重写, §2.19 折叠默认值例外说明

## 5. 验证计划

- 编译客户端与测试 (Debug)
- `tui_scroll` / `tui_stream` / `tui_tool_header` / `tui_interrupt` / `tui_input` 全绿
- 全量 `agentxx_test --log-level warn` 无新增失败
- 抓一帧文本画面确认两种形态的排版 (行序 / 右侧对齐 / 大小文本)
