# markdown 代码块长行折行显示
- 难度: B
- 类型: bug修复
- 基于commit: 2b8edae9bc4d3433eb489b3a17bf01018832aa81
- 时间: 2026-09-24 22:34
- 需求:
```md
请修复 markdown_ui 渲染代码段时不会字段换行的问题，导致可能会被裁剪不显示
```

## 实现
- markdown_ftxui (`markdown/src/dom_builder.cpp`):
  - `build_code_block` 逐行按可用宽度折行 (原来每行只生成一个 `ftxui::text`,
    FTXUI 对超宽元素是裁剪而不是折行, 长代码行只剩前半截); 折行宽度
    = `tl_max_width` - 2 (代码块左右各 1 列内边距) - `tl_indent`
  - 新增 `tl_indent` (外层缩进列数) 与 `IndentScope` 守卫: 块引用前缀 "│ " 每层
    +2 列, 列表项首段前缀按实际列数; `build_table` 的可用宽度同样扣除,
    表格在引用内不再被右缘裁掉边框
  - 折行实现抽为公共函数 `markdown::wrap_line_by_width`
    (`markdown/include/markdown/text_utils.hpp`): 按显示宽度切分, 不丢字符、
    不拆多字节/双宽字符, 宽度预算不足一个字符时也强制推进
- 客户端 (`agent/client/src/io/tui/markdown_block.cpp`):
  `estimateMarkdownLines` 的围栏代码块按同一折行函数统计行数 (此前每源行计 1 行,
  折行后消息高度被低估会导致滚动偏移偏小、底部内容被推出视口)
- 测试: 新增 client 测试模块 `markdown_block` (折行不丢内容/尾部可见/块引用缩进/
  双宽字符/高度估算与实测一致/未限制宽度不折行)
