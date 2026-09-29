# 修复 tui markdown 列表文本拖选复制缺开头、多末尾
- 难度: B
- 类型: bug修复
- 基于commit: 2b50779a
- 时间: 2026-09-30 02:53
- 需求:
```md
请修复 tui markdown 选中文本复制时，在列表中的文本会缺失头部、额外复制了末尾之后的字符
```

## 现象与根因

消息列表里拖选 markdown 列表文本时, 复制结果与所见不一致:
- **开头缺/多空格**: 起点落在列表项续行的悬挂缩进列 (项目符号占的列, 没有任何节点
  绘制) 上时, 复制结果开头多出一串空格, 首行的项目符号被空格顶掉;
- **末尾多出整行**: 终点落在这些空列上时, 复制结果里把那一行的正文**整行**带了出来;
- 更严重时整个选区取不到文本 (端点落在消息列表左侧留白内), 或反过来把整条消息
  全部复制出来 (端点落在留白上时选择被夹到子项整块)。

根因在"逐行取文本"的区间算法 (`ftxui::Text` / `markdown::FlowText` /
`FlowCodeBlock` 的 `Select`):

1. 每行的列区间由 `Selection::SaturateHorizontal(行盒)` 得出。它把**落在行内空白列
   上的端点夹到行盒边缘** ("起点不在本行 -> 行首, 终点不在本行 -> 行尾"), 而这些
   空白列 (列表续行的悬挂缩进、面板留白) 不属于任何节点, 端点明明在正文左侧也被
   当成"整行都被选中" -> 复制与高亮都多出没选中的整行文本。
2. 参与判定 (`Box::Intersection(selection.GetBox(), box_)`) 与行区间用的是**容器夹取
   后的选择**: `HBox::Select` 的夹取按"起点在内、终点在外"把终点顶到盒右缘, 矩形被
   收窄/抬高 —— 同一行里其它节点的文本被漏掉, 行范围也会多出几行。
3. 列表项续行的悬挂缩进列是空白格, 不属任何节点: 复制结果里丢失 (与所见不一致),
   多行复制时表现为"正文缺开头"。
4. 消息列表左右留白用 `text("   ")` 表达 (文本节点): 它会进复制结果, 且节点只有一行
   高 -> 复制结果随消息在视口内的位置漂移。

## 修改

- `agent/third_party/FTXUI` (自研分支, 同样受 ExternalProject stamp 限制: 重新编译前
  删除 `agent/build/<cfg>/ftxui_repo-prefix/`)
  - `include/ftxui/dom/selection.hpp` / `src/ftxui/dom/selection.cpp`
    - 新增 `Selection::Root()` (容器夹取出的选择保留指向根选择的指针) 与
      `Selection::RowRange(y, lo, hi)`: 按**文本流**给出第 `y` 行的选中列区间 ——
      起点行到行尾、终点行从行首、两者之间的行整行 (反向拖选方向相反), 单侧无界用
      `kUnboundedMin/Max` 表示; 区间由**根选择的原始端点**算出, 不受容器夹取影响
  - `src/ftxui/dom/text.cpp`: `Text::Select` 改为逐行取 `RowRange` 与自身列范围求交,
    参与判定改用根选择的原始矩形
- `agent/third_party/markdown_ftxui` (自研子模块, 改后须删除
  `agent/build/<cfg>/markdown_ftxui_repo-prefix/` 再编译, 否则 ExternalProject 按
  stamp 跳过)
  - `markdown/include/markdown/flow.hpp` / `markdown/src/flow.cpp`
    - `FlowText::setHangIndent(列数)`: 记录列表项内容左缘到项目符号左缘的列数
    - `FlowText::Select` / `FlowCodeBlock::Select`: 同 `ftxui::Text` 改用
      `Root()` + `RowRange`; `FlowText` 把悬挂缩进列 (续行左端, 无节点绘制的空白格)
      在选择范围内时按空格放进复制结果
    - `FlowText::Render`: 同上把悬挂缩进列按空白格补画 + 高亮 (高亮与复制一致)
  - `markdown/src/dom_builder.cpp`: 新增 `tl_hang_indent` 与 `HangIndentScope`
    (`IndentScope` 同款), `build_list_item` 构建首段内容时按"缩进 + 项目符号列数"记录
    悬挂缩进 (`spans_to_element` 写入 `FlowText`)
- `agent/client`
  - `src/io/tui/components/message_list.cpp`: 左右 3 列留白 `text("   ")` 换成
    `filler() | size(WIDTH, EQUAL, 3)` —— 留白是版面留白, 不参与选择
  - `src/io/tui/lazy_scrollable.cpp` / `src/io/tui/scrollable.cpp`: `Select` 覆写的
    参与判定改用 `selection.Root().GetBox()` (夹取后的矩形可能被收窄)
- 测试
  - `agent/test/client/test_ftxui_text.cpp`: 新增"端点落在同一行空白列上"用例
    (不取整行, 也不取同一行其它节点的整段文本)
  - `agent/test/client/test_markdown_flow.cpp`: 新增"列表项悬挂缩进"用例 ——
    续行整行/部分/正文内部取值, 以及"终点落在缩进列上不带出整行正文"
  - `agent/test/client/test_tui_scroll.cpp`: 新增 `testMarkdownListTextCopy` ——
    消息列表层端到端核对 (整体拖选、右下->左上、终点在续行正文中、续行内部拖选、
    选区落在左侧留白内、第二个列表项), 并核对悬挂缩进列的高亮与复制一致
- 文档: `docs/zh-cn/design/tui.md` §3.2 (逐行区间/根端点/留白不参与选择) 与
  §3.3.1 (FlowText 的列口径与悬挂缩进) 同步

## 验证

- 相关模块: `ftxui_text markdown_block markdown_flow tui_scroll tui_lazy_view tui_stream
  tui_ui_items tui_widget tui_tool_header tui_surface tui_theme tui_interrupt tui_form
  tui_input tui_sidebar tui_context_overlay mermaid_state ui_items ui_kit interrupt_ui`:
  passed=7268 failed=0 (windows-debug)。
- 复现场景 (消息列表 40x12, 正文 "- alpha bravo charlie delta echo foxtrot golf hotel
  india juliet\n- second"):
  - 整体拖选 -> `"  • alpha bravo charlie delta\n    echo foxtrot golf hotel india\n    juliet"`
    (续行保留 4 列悬挂缩进, 不带消息列表留白; 修复前续行无缩进)
  - (20,0)->(2,1) -> `"harlie delta"` (修复前: 20 个空格 + 该行正文 + 整行下一行 + 更多行)
  - (0,0)->(2,1) -> 空 (修复前会把整条消息取出)
