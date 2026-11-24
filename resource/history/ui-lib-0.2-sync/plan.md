# 计划：跟进 cxx_pluginxx_ui 0.2

## 1. 现状调查

### 1.1 库这边的变化（`agent/third_party/cxx_pluginxx_ui`，`fa4e323` → `aaa1d511`）

| 项 | 变化 |
| --- | --- |
| 词汇版本 | `gen::kUiApiVersion` 3 → 4，`kSchemaVersion` 2，基础 kit `kitVersion` 3 |
| 语义色 | `Item::tone` / `ControlOption::tone` / `Threshold::tone` / `TableColumn::tone` / `TableCell::tone` / `TreeNode::tone` / `ListItem::tone` 全部改名 `color`；`KeyValuePair::kTone/vTone` → `kColor/vColor` |
| 动作 | `Action::Kind` 由 `None/Dispatch/Route/Command` 收敛成 `None/Call/Route`；`build::dispatch()` → `build::call(name, argsJson, timeoutMs)`，新增 `build::route()` |
| 按钮 | `Button.variant` 由 `primary/secondary/ghost/link` 换成 `normal/full/simple`；新增 `highlight`（选中态）、`size`（按钮高度）；`label` 可省（纯图标按钮，`icon`/`glyph` 任一即可） |
| 卡片 | 单块容器的孩子统一走 `Item::children`（最多一项）；基础 kit 的 `card` 参数由 `children` 改成 `child` |
| 新块 | `ThemeScope`（子树样式作用域，`themeDataJson`，Optional/Flatten）、`UISlot`（嵌槽位，`slot`/`slotArgsJson`/`slotFallback`，Optional/Skip）；`ListView` 系列补 `data`/`custom`/`path`/`isDir` 与新动作字段 |
| 能力段 | `Limits` 全部归零（不限制）；`Capabilities` 新增 `gap`；C++ 侧暂无 `slots` / `values` 段（只在 Dart 侧） |
| 基础 kit | 新增 `themeScope` / `slot` / `textVar` / `varValue`；已有组件的 `tone` 参数改名 `color` |

### 1.2 agentxx 这边的使用面

| 位置 | 用到的旧写法 |
| --- | --- |
| `agent/lib/src/middlewares/interrupt_ui.cpp` | `Item::tone`、`ControlOption::tone`、`build::dispatch`、按钮变体 `"primary"/"secondary"` |
| `agent/lib/src/middlewares/interrupt_presets.cpp` | `item.tone` |
| `agent/lib/include/agentxx/plugin/api/client_plugin_kit.h` | `Action::Kind::Dispatch`、`btn.tone`、默认变体 `"secondary"`、文档里的变体清单 |
| `agent/client/src/io/tui/ui_components.cpp`（40 处） | `item.tone` / `opt.tone` / `node.tone` / `entry.tone` / `th.tone` / `cell->tone`、变体判断 `primary/secondary/ghost/link` |
| `agent/client/src/io/tui/components/overlays.cpp` | `build::dispatch`、变体 `"primary"/"secondary"` |
| `agent/client/.../plugin_ui_items.h`、`ui_components.h` | 形参名 `tone`、`uiRoleColor(tone)` |
| `agent/client/src/io/tui/components/status_bar.cpp` | 段描述 JSON 里读 `tone` 键 |
| `agent/plugins/*` | chatbot 面板 / planning / codegraph / system_monitor 手写 `"tone"` 参数与 `item.tone` |
| `agent/schema/agentxx-ui-kit.def.json` | 11 处 `tone`（含参数类型 `tone`、`statusTone`）、按钮变体 `primary/ghost`、`card` 的 `children` |
| 用例 | `test_ui_kit` / `test_ui_items` / `test_tui_ui_items` / `test_tui_form` / `test_tui_tool_header` / `test_interrupt_ui` / `test_chatbot_core` / `test_client_plugins` / `test_plugin_sdk` 断言旧字段与旧变体 |

## 2. 方案

1. **先改扩展 kit 定义并重新生成**（生成物是插件可见的 API，先定下来再改代码）：
   `tone` → `color`（含参数类型）、`card` 的 `children` → `child`、
   确认按钮 `primary` → `normal`、取消按钮 `ghost` → `simple`。
2. **agentxx 代码改名**（机械替换，按标识符边界，避免误伤 `tombstone` 一类单词）：
   `.tone` → `.color`、`"tone"` → `"color"`、`kTone/vTone` → `kColor/vColor`、
   `statusTone` → `statusColor`、`todoTone` → `todoColor`、局部参数 `tone` → `color`；
   `Action::Kind::Dispatch` → `Call`、`build::dispatch` → `build::call`。
3. **TUI 渲染按新词汇调整**：
   - 变体映射：`simple` → 原来的幽灵/链接样式，`full` 与 `normal` 同渲染（终端里铺满无意义），
     空串按 `normal`；`highlight` 画成选中态（反色/加粗）；
   - 纯图标按钮（`label` 空、有 `glyph`/`icon`）：`glyph` 当文字，`icon` 无终端图标表则忽略；
   - `Button.size` 用行高（终端至少 1 行，不额外占位）；
   - `ThemeScope` 不声明支持（`adapt` 展开子节点，样式本就不适用）；
   - `UISlot` 不声明支持（TUI 没有内容运行时，`adapt` 跳过；声明了却画不出来更糟）。
   - 能力段 `blocks` 与新组件表对齐（`kTuiBlockNames` 只列真正实现了的）。
4. **插件与用例跟进**（`"tone"` → `"color"`、变体断言改新值、`Kind::Call`）。
5. **另外两个库**：`cxx_pluginxx` 广播默认放开（`plugin.*` 任何人可发、结束不看属主）——
   检查 agentxx 是否有依赖旧规则的地方与断言；`cxx_utilxx` 删网络上限——检查
   `plugin_net` 用例里有没有依赖上限的断言。两句结论写进 `work.md`。
6. **文档**：`docs/zh-cn/design/ui-layer.md`、`plugins.md` 的界面段、`AGENTS.md` 记忆条目
   里的 `tone` 与变体写法。

## 3. 边界与兼容

- 老插件写的 `"tone"` 会被当成未知字段忽略（颜色回落默认值），这是库的
  `uiApiVersion` 4 变更；agentxx 的插件 SDK 与 `Agentxx*` C ABI 结构体没有变化。
- TUI 不实现 `UISlot` / `ThemeScope`，插件在终端里拿到的是"展开后的内容 / 跳过"，
  与库的 `adapt` 规则一致，不算降级缺陷。

## 4. 分步实施

1. 扩展 kit 定义 + 重新生成（Dart 3.14 已就绪，脚本 `agent/script/gen_ui_kit.ps1`）。
2. `agent/lib` 侧改名（interrupt_ui / interrupt_presets / client_plugin_kit）。
3. `agent/client` 侧改名 + 变体与新增字段渲染 + 能力段。
4. 插件手写 JSON 改名。
5. 用例改名与断言调整。
6. 构建 → 全量测试 → 文档。

## 5. 验证

- 顶层 Debug 构建 `EXIT=0`；插件 DLL 全量重链。
- 定向：`ui_kit` / `ui_items` / `tui_ui_items` / `tui_form` / `tui_widget` /
  `tui_tool_header` / `interrupt_ui` / `client_plugins` / `plugin_sdk` /
  `chatbot_core` / `plugin_broadcast` / `plugin_net`。
- 全量 `agentxx_test --log-level warn`，与上一轮基线（43682）对齐（差值只来自本轮删/增的用例）。

## 6. 风险

- 机械改名漏项：靠编译错误 + 全量 grep `tone` 双保险。
- 变体语义映射错：`simple` 与原 `ghost` 是否等价按渲染分支逐个确认，用例里锁住。
- 生成物与定义不同步：提交前跑生成器 `--check`（若可用）并核对 `kitVersion`。
