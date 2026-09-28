# 修复 tui 消息列表两处显示问题 (markdown 拖选无高亮 / 折叠长消息后底部留白)
- 难度: B
- 类型: bug修复
- 基于commit: d63eba367bd4c70638b343a75cb6ec6b3dd3c6a3
- 时间: 2026-09-28 22:20
- 需求:
```md
请修复 tui markdown 渲染的文本 在鼠标选中时没有背景颜色高亮变化；
以及 tui 消息列表在展开长消息后略上滑离开底部，然后折叠消息，此时消息列表底部会有大量的空白
```

## 现象与根因

### 1. markdown 文本拖选无反色高亮
消息正文由自绘节点渲染 (段落/行内 = `markdown::FlowText`, 围栏代码块 =
`markdown::FlowCodeBlock`), 这两个节点都实现了 `Select` (拖选能把文本交给
`Selection`, 复制功能一直正常), 但 `Render` 只画字符与行内样式 ——
没有像 `ftxui::Text` 那样对选中单元格执行 `Screen::GetSelectionStyle()`。
于是"能复制、但拖动时看不到选中高亮"。

### 2. 折叠展开过的长消息后底部大片空白
消息列表用 `LazyScrollable` 的锚点模型: 唯一滚动状态是
`(anchorIndex_, anchorRow_)`。展开的长消息 (Think/Tool) 在吸附状态下锚点就是该条
(行偏移 = 高度 - 一屏); 用户上滑一点解除吸附后, 锚点仍停在该条内;
此时折叠该条 -> 它的高度骤减, 行偏移被夹到 `高度-1`, 于是"从视口顶行到内容末尾"
只剩一两行, 视口下半全是空白 (修复前没有任何回夹动作, 空白会一直保持)。

## 修改

- `agent/third_party/markdown_ftxui/markdown/include/markdown/flow.hpp`
  / `.../src/flow.cpp` (自研库, 子模块)
  - `FlowText` / `FlowCodeBlock` 新增拖选高亮状态 (`selectionRows_` 每行 {起列, 终列}
    + `selectionFirstLine_`), 协议与 `ftxui::Text` 完全一致:
    - `Select` 每帧填充 (无选择时框架根本不调用 `Select`)
    - `ComputeRequirement` 清空 (每帧在所有参与布局的节点上先执行一次; 跳过热路径
      `ComputeRequirement` 的宿主由 `LazyScrollable::resetSelectionHighlight` 显式调用)
    - `Render` 对落在区间内的单元格执行 `Screen::GetSelectionStyle()`; 宽字符的保留格
      与 `ftxui::Text` 同样高亮; 折行后的每一行各自按自己的列区间判定
  - 注意: 改 third_party 子模块后必须删除 `agent/build/<cfg>/markdown_ftxui_repo-prefix/`
    再编译, 否则 ExternalProject 不会重编 (本次踩到: 旧 `markdown-ui.lib` 与新头文件
    混用, 在 ASan 下表现为 `FlowText::plain` 构造越界)。

- `agent/client/include/agentxx-client/io/tui/lazy_scrollable.h`
  / `agent/client/src/io/tui/lazy_scrollable.cpp`
  - 抽出两个私有方法:
    - `anchorToTailWindow(contentWidth, count)`: 尾部窗口发现 (原吸附底部分支内联代码)
    - `layoutViewportFromAnchor(contentWidth, count, laidRows)`: 从锚点向后测量定位
  - 定位后新增回夹判断: `nextIndex == count && laidRows < vh` (从视口顶行到内容末尾
    不足一屏) 时按尾部窗口重取锚点并重新定位 —— 等价于把滚动位置夹到
    `totalHeight - viewportHeight`。锚点已在内容开头时尾部窗口必为同一点, 直接跳过
    (短内容每帧零成本); 回夹不改动 `stickToBottom_` (用户上滚的意图保留), 下一次下滚
    因"内容末尾已在视口内"恢复吸附。
  - 第二遍定位前清空第一遍登记的命中盒 (尾部窗口起点不会晚于原锚点, 第二遍覆盖的条目
    是第一遍的超集)。

- 测试
  - `agent/test/client/test_markdown_flow.cpp`: 新增"选中高亮"用例 —— 按 App 的帧顺序
    (`ComputeRequirement -> SetBox -> Select -> Render`) 渲染并断言: 选区内单元格反色、
    区外不反色、`ComputeRequirement` 复位后无高亮、宽字符保留格一起高亮、折行跨行选区
    逐行高亮、代码块只高亮被绘制的字符格。
  - `agent/test/client/test_tui_lazy_view.cpp`: 夹具新增 `heightOf`/`keyOf` (按条目身份
    给高度与 key 增量); 新增"内容收缩后回夹"用例 —— 折叠末条长消息后视口逐行都是内容、
    末行是折叠条目的最后一行、派生偏移等于内容底部; 并补"收缩发生在锚点上方且余下仍够
    一屏时不回夹 (视口逐行不变)"的对照用例。

- `docs/zh-cn/design/tui.md`: §3.2 补"拖选复制与高亮"约定 (自绘节点必须同时做文本收集
  与高亮, 懒列表需显式清除残留高亮); §3.3.1 补自绘节点的选中高亮协议;
  §3.3.2 补"内容收缩后回夹到底部窗口"与回归保护条目。

## 验证
- `agentxx_test markdown_flow tui_lazy_view`: passed=4002 failed=0。
- 客户端相关模块全量: `ftxui_text markdown_block markdown_flow tui_scroll tui_lazy_view
  tui_stream tui_ui_items tui_widget tui_tool_header tui_surface tui_theme tui_interrupt
  tui_form tui_input tui_sidebar tui_context_overlay mermaid_state ui_items ui_kit
  interrupt_ui`: passed=7218 failed=0。
- `agentxx_cli` / 客户端库 / 测试目标编译链接通过 (windows-debug)。
