# TUI 消息列表渲染重构 实施记录 (work.md)

方案原文: [plan.md](plan.md) —— 本文件记录实施进度、已完成/待完成内容与注意事项。

## 进度总览

| # | 提交内容 | 状态 | 提交 |
|---|---|---|---|
| 1 | 方案 `plan.md` | 已完成 (前序会话) | `9931ff44` / `d4112e27` |
| 2 | B1 紧凑文本节点 (ftxui `Text`) | **已完成** | `ed99c480` |
| 3 | B2a `FlowText` (纯文本) + User 纯文本 | 待完成 | — |
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
   - `Render()` 按行解码直写 `screen.CellAt(x, y)`:
     全角字符占两格且第二格写为空串占位, 组合字符并入前一格, 控制字符不占列,
     `x > box_.x_max` 即停止 (与旧实现同样的裁剪行为)。
   - `Select()` 用同一套字节偏移: 每行按列区间取文本, 全角字符的保留列不产生
     字符, 组合字符随其修饰的字符一起被选中 (与旧逐字素实现逐条对齐)。
   - `VText` 未改动。
2. 新增测试模块 `ftxui_text`:
   - `agent/test/client/test_ftxui_text.cpp` + `agent/test/include/agentxx-test/client/test_ftxui_text.h`
   - `agent/test/test.cpp` 注册 (`runSync("ftxui_text", ...)`)。
   - 覆盖: 行数与列宽 (多行/尾随换行/空文本/宽字符/组合字符/控制字符)、
     逐格绘制内容、超出盒宽与盒高的裁剪、选择取文本 (含跨行、宽字符、组合字符)、
     内存上界断言 (用本模块内的 `operator new` 计量, 仅本模块开关打开)。

### 实测结果

| 场景 | 结果 |
|---|---|
| 100 条 1KB 文本节点分配字节数 (新) | **142 KB** (约源文本 1.4 倍) |
| 同上 (旧实现) | 约 3 MB 级 (源文本 30~60 倍) |
| 客户端测试 (config_loader/tui_*/markdown_block/ftxui_text 等 18 个模块) | 通过 3461 项断言, 失败 0 |
| 构建 | `windows_debug_build.bat` 全量通过 (ftxui 与整体均无 error/warning) |

### 注意事项 (实施中踩到的点)

- **ftxui / markdown_ftxui 是 ExternalProject + git 子模块**: 改源码后外层构建
  不会自动重编 —— 需删除 `agent/build/<cfg>/<name>_repo-prefix/src/<name>_repo-stamp/Debug/`
  下的 `*-build` / `*-install` / `*-done` 三个 stamp 文件 (或整个 prefix 目录),
  让嵌套构建与安装重新执行, 否则改动静默不生效。
- 子模块改动需**先在子模块仓库内提交**, 再在外层仓库提交指针更新
  (与既有 `markdown_ftxui` 的做法一致)。
- ftxui 用 `/W3 /WX` 编译 (`FTXUI_DEV_WARNINGS=ON`), 改动不得引入任何警告。
- 源文件里写中文字面量时注意**十六进制转义贪婪匹配**: `"a\x01b"` 会被解析成
  `0x1B`, 应写成 `"a\x01" "b"`。
- Windows 上 `windows_debug_build.bat` 末尾把 `exec` 复制到 `windows-debug-output`
  会因正在运行的 agentxx_cli 占用而失败, 属预期 (已忽略); 跑测试用
  `agent/build/windows-debug/exec/agentxx_test.exe`。
- 测试断言用**局部期望值**, 不要照抄注释里的示例字符 (本轮曾把 "汉字" 的期望
  误写成 "好" 导致假失败)。

---

## 待完成

- 阶段 3 (B2a): `markdown::FlowText` 纯文本自绘折行节点 + `message_list.cpp` 的
  `Role::User` 分支改用纯文本折行 (不解析 markdown); 新增 `markdown_flow` 测试。
- 阶段 4 (B2b): `FlowText` 行内样式 (粗体/斜体/行内代码/链接) + `FlowCodeBlock`,
  `dom_builder.cpp` 的段落/行内/代码块改造, `Theme` 增加单元格样式字段。
- 阶段 5 (C/A): `lazy_scrollable` 锚点式视口模型 (窗口发现 O(可见)、前插零校正、
  删除 `pendingPrepend_`/估算链路), `message_list` 的估算与 key 瘦身。
- 阶段 6 (A6/A7/B3): 流式稳定块预算淘汰、工具渲染请求去重前置、`sourceBytes` 标定。
- 阶段 7: `agentxx_benchmark render_tui` 模块 + `docs/zh-cn/design/tui.md` 同步。

## 注意事项 (后续阶段)

- 本机基准: 微基准源在方案附录 A 的临时目录, 若无则需按 §5.2 重写;
  `AGENTXX_BUILD_BENCHMARK` 当前为 OFF, 跑基准前需重新配置打开。
- 阶段 5 会改动 `scrollOffset()` 口径 (派生实现), 需逐个调用点核对
  (`maybeRequestMoreHistory` / `test_tui_scroll` / `test_tui_stream`)。
- 每阶段结束都要: 编译 (含 ftxui/markdown_ftxui 改动时清 stamp) → 跑相关测试模块
  → 更新本文件的进度/实测/注意事�� → git 提交。
