# TUI 消息列表渲染重构 实施记录 (work.md)

方案原文: [plan.md](plan.md) —— 本文件记录实施进度、已完成/待完成内容与注意事项。

## 进度总览

| # | 提交内容 | 状态 | 提交 |
|---|---|---|---|
| 1 | 方案 `plan.md` | 已完成 (前序会话) | `9931ff44` / `d4112e27` |
| 2 | B1 紧凑文本节点 (ftxui `Text`) | **已完成** | `ed99c480` |
| 3 | B2a `FlowText` (纯文本) + User 纯文本 | **已完成** | `c77aa97a` |
| 4 | B2b `FlowText` 行内样式 + `FlowCodeBlock` | 待完成 | — |
| 5 | C/A 锚点视口模型 + 估算/key 瘦身 | 待完成 | — |
| 6 | A6 稳定块淘汰 + A7 工具渲染去重 + B3 预算标定 | 待完成 | — |
| 7 | 性能测试模块 + 文档同步 | 待完成 | — |

---

## 已完成: 阶段 2 (B1 紧凑文本节点)

### 改动内容

1. `agent/third_party/ftxui/src/ftxui/dom/text.cpp` (子模块提交 `72e11a66f`):
   - `Text` 节点由"每字素一个 `std::string` + 每行字素下标"改为
     **`std::string text_` + `std::vector<int> line_starts_`** (各行起始字节偏移);
     单行文本时 `line_starts_` 保持为空 (不分配), 覆盖大量短标签场景。
   - 构造函数一次线性扫描 (`EatCodePoint` + `IsControl/IsCombining/IsFullWidth`)
     统计每行列数与行数, 得出 `requirement_.min_x/min_y` 后不再重算;
     `ComputeRequirement()` 只复位选择状态 (与旧实现职责一致)。
   - `Render()` 按行解码直写 `screen.CellAt(x, y)`: 全角字符占两格且第二格写为空串
     占位, 组合字符并入前一格, 控制字符不占列, `x > box_.x_max` 即停止 (同样的裁剪
     行为)。
   - `Select()` 用同一套字节偏移: 每行按列区间取文本, 全角字符的保留列不产生字符,
     组合字符随其修饰的字符一起被选中 (与旧逐字素实现逐条对齐)。
   - `VText` 与对外接口未改动。
2. 新增测试模块 `ftxui_text` (含 `operator new` 计量, 仅本模块开关打开):
   行数与列宽、逐格绘制内容、超出盒宽/盒高的裁剪、选择取文本、内存上界。

### 实测结果

| 场景 | 结果 |
|---|---|
| 100 条 1KB 文本节点分配字节数 (新) | **142 KB** (约源文本 1.4 倍) |
| 同上 (旧实现) | 源文本的 30~60 倍 |
| 客户端测试 (18 个模块) | 通过 3461 项断言, 失败 0 |

---

## 已完成: 阶段 3 (B2a `FlowText` + User 纯文本)

### 改动内容

1. `agent/third_party/markdown_ftxui` (子模块提交 `932d854`):
   - 新增 `markdown/include/markdown/flow.hpp` + `markdown/src/flow.cpp`:
     `CellStyle` (逐格样式) / `Span` (带样式片段 + 链接序号) / `LinkBoxTarget`
     (链接区段登记目标) / `FlowText` 节点。
   - `FlowText` 要点: 空格为词边界折行 (行首空格丢弃)、超宽单词按列硬拆 (不丢字符)、
     `'\n'` 硬换行; **折行结果按宽度缓存** (宽度不变时 `SetBox` 只更新盒位置);
     词间空格与该词合并写入同一片段, 使一行通常只剩 1 个片段;
     行内样式逐单元格应用 (未设置的通道保留外层装饰器颜色);
     `Select` 按行/列区间导出文本; 链接可见区段写入 `LinkBoxTarget`
     (构建期 reserve、布局期只写不超容量, `Box const*` 稳定);
     迭代布局与 `ftxui::flexbox` 同口径 (`asked_` + `need_iteration`)。
   - `text_utils.hpp` 增加 `glyph_columns` / `is_zero_width` / `utf8_decode_at`
     三个内联小工具 (折行与自绘节点共用同一套列宽口径)。
2. `markdown_block.{h,cpp}` 新增 `renderPlainText(content, color)`: 纯文本折行元素。
3. `message_list.cpp` 的 `Role::User` 正文改用 `renderPlainText` (原来 `ftxui::paragraph`
   每词一个节点; 现在一个节点, 长路径/URL 按列硬拆不再被右缘裁掉)。
4. 新增测试模块 `markdown_flow` (80 项断言): 折行/缓存/硬拆/宽字符/组合字符/
   行内样式/选择/链接区段。

### 实测结果

| 场景 | 结果 |
|---|---|
| `markdown_flow` + `ftxui_text` | 通过 117 项断言, 失败 0 |
| 客户端回归 (14 个模块) | 通过 2590 项断言, 失败 0 |

---

## 待完成

- 阶段 4 (B2b): `dom_builder.cpp` 的段落/行内/代码块改用 `FlowText`/`FlowCodeBlock`
  (含 `collect_inline_words`/`words_to_element`/`build_wrapping_container`/
  `build_code_block` 改造、链接区段登记改由 `FlowText` 写入), `Theme` 增加单元格
  样式字段 (`link_style` / `code_inline_style` / `table_*_style`), client 侧
  `TUITheme::markdownTheme` 同步填充。
- 阶段 5 (C/A): `lazy_scrollable` 锚点式视口模型 (窗口发现 O(可见)、前插零校正、
  删除 `pendingPrepend_`/估算链路), `message_list` 的估算与 key 瘦身。
- 阶段 6 (A6/A7/B3): 流式稳定块预算淘汰、工具渲染请求去重前置、`sourceBytes` 标定。
- 阶段 7: `agentxx_benchmark render_tui` 模块 + `docs/zh-cn/design/tui.md` 同步。

## 注意事项 (实施中发现, 后续阶段同样适用)

- **ftxui / markdown_ftxui 是 ExternalProject + git 子模块**: 改源码或改其
  `CMakeLists.txt` 后外层构建不会自动重编 —— 需删除
  `agent/build/<cfg>/<name>_repo-prefix/src/<name>_repo-stamp/Debug/` 下的
  `*-configure` / `*-build` / `*-install` / `*-done` stamp 文件 (或整个 prefix 目录)。
- 子模块改动需**先在子模块仓库内提交**, 再在外层仓库提交指针更新 (与既有做法一致)。
- ftxui 用 `/W3 /WX` 编译 (`FTXUI_DEV_WARNINGS=ON`), 改动不得引入任何警告;
  markdown_ftxui 侧沿用工程既有的 `/W4` 口径 (未升级为错误)。
- 源文件里写中文字面量时注意**十六进制转义贪婪匹配**: `"a\x01b"` 会被解析成 `0x1B`,
  应写成 `"a\x01" "b"`。
- 测试断言用**局部期望值**, 不要照抄注释里的示例字符 (曾把 "汉字" 的期望误写成 "好")。
- Windows 上 `windows_debug_build.bat` 末尾把 `exec` 复制到 `windows-debug-output`
  会因正在运行的 agentxx_cli 占用而失败, 属预期 (已忽略); 跑测试用
  `agent/build/windows-debug/exec/agentxx_test.exe`。
- 新增测试模块需要: `test/client/test_xxx.cpp` + `test/include/agentxx-test/client/test_xxx.h`
  + `test.cpp` 里 `runSync("xxx", agentxx::test::testXxx);` (源文件由 CMake glob 收集,
  configure 时自动纳入)。
- `FlowText` 的 `flex_grow_x/flex_shrink_x` 均为 1 (等价于原来的
  `ftxui::paragraph | xflex_shrink`), 放进 hbox 时会占满剩余宽度 —— 替换
  `paragraph(...)` 时不需要再加 `xflex_shrink`。
