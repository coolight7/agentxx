# 实施记录: 中断消息折叠与摘要

进度: 10 / 10 (完成)

## 任务列表

- [x] 1. 描述层: `InterruptUiSegment.kind` + `InterruptUiSummary` + `InterruptUi.summary`
      (解析 / 序列化 / `empty()` 判定), 见 plan.md §3.2
- [x] 2. 预设: `permissionCard` 三段特化 (权限标记 / 工具名 `kind=tool` / 权限名 i18n 键,
      **读写只差颜色, 不加粗**) + `summary.detail = target`, 见 plan.md §3.3
      (另新增对外 `preset::permissionCategoryLabelKey(category)`)
- [x] 3. 文案: `interrupt.permission.read|write` 新增; `interrupt.header` /
      `interrupt.permissionBadge` / `interrupt.noDescriptor` 去掉重复的 `! `, 见 plan.md §3.8
- [x] 4. 客户端渲染: `InterruptView` 标记列 + 摘要行 + 已处理展开 (只渲染内容块) +
      `estimate`, 见 plan.md §3.4 / §3.6
- [x] 5. 折叠交互: `HitBox::kind` (Form / Fold) + 头行/摘要行 reflect + 点击翻转
      (折叠时清激活、键盘不参与), 见 plan.md §3.5
- [x] 6. 自动折叠: `confirm` / `cancel` / `WireInterruptExpired` 三处把 `msg.collapsed` 置 true,
      创建时置 false, 见 plan.md §3.7
- [x] 7. 工具显示名: `MessageListComponent` 注入 resolver, `InterruptView` 渲染 `kind=tool` 分段时
      按三态处理 (**未就绪留空, 绝不显示原名再跳变**; 无渲染器→原名),
      `handleInterrupt` 预取一次; 见 plan.md §3.4.1
- [x] 8. 测试: `interrupt_ui` / `interrupt_bus` / `tui_interrupt` 三组 (§9 含收尾核对), 见 plan.md §9
- [x] 9. 文档: `docs/zh-cn/design/tui.md` §2.6 补折叠规则; index.md 回填结论
- [x] 10. **收尾核对**: 权限名两档只差颜色 (分段 `bold` 均为 false)、折叠行带结果、
      未处理不可折叠、展开无控件、无"原名 → 显示名"跳变 —— 逐项结果见文末"收尾核对"

## 每步记录 (改了什么 + 怎么验证)

### 1. 描述层字段 (`interrupt_ui.h` / `interrupt_ui.cpp`)

- `InterruptUiSegment` 新增 `kind` (`""` = 普通文本 / `"tool"` = 工具名, 客户端可换特化显示名)。
- 新增 `InterruptUiSummary { segments, detail, detailKey }` + `empty()`。
- `InterruptUi` 新增 `summary`; `empty()` 一并计入 `summary.empty()`。
- `toJson` 保持"空字段不写" (`summary` 整段为空则不写); `fromJson` 缺字段按默认值, 非对象按空。
- 验证: `interrupt_ui` 模块新增 `test_ui_summary_and_segment_kind` (往返 / 空字段不写 /
  只有 summary 也算非空描述 / 非对象 summary 不失效)。

### 2. 权限卡片特化 (`interrupt_presets.h` / `.cpp`)

- 头行三段: 权限标记 (键, error+bold) / 工具名 (`kind="tool"`, accent+bold) /
  权限名 (`labelKey = permissionCategoryLabelKey(category)`, 颜色 `_write` → `thinking`, 其余 `hint`,
  **不加粗**)。
- 权限名的字面文本不再带前导空格 (改由客户端的"相邻分段补空格"规则处理)。
- `ui.summary.detail = target` (无目标时不写)。
- 新增 `preset::permissionCategoryLabelKey()`: `filesystem_read` → `interrupt.permission.read`,
  `filesystem_write` → `interrupt.permission.write`, 其它返回空 (插件自定义分类原样显示)。
- 验证: `interrupt_ui` 的权限卡片用例新增分段语义/颜色/`bold=false`/summary/自定义分类断言;
  `interrupt_bus` 的权限请求描述断言按新形态更新。

### 3. 文案 (`tui_i18n.cpp`)

- `interrupt.header` / `interrupt.permissionBadge` / `interrupt.noDescriptor` 去掉行首 `! `。
- 新增 `interrupt.permission.read` (`Read files` / `读取文件`) 与
  `interrupt.permission.write` (`Write files` / `写入文件`)。
- 屏幕输出不变: 标记列提供 `!` + 空格, 再拼 `[权限] `。

### 4/5/6. 中断视图 (`interrupt_view.h` / `.cpp`)

- `Mode { Waiting, Collapsed, Expanded }` + `modeOf()`: `Waiting` 恒展开 (不读 `collapsed`)。
- 标记列 `buildMark()`: `! ` (error+bold) / `+ ` / `- ` (hint), 与其它角色折叠标记同列。
- 分段统一走 `resolveSegments()` (i18n 键优先 / `kind=="tool"` 查显示名 / 相邻段补空格) +
  `joinSegments()` (列宽计算) + `segElements()` (着色渲染), 渲染与预算同源。
- 折叠摘要 `buildSummaryLine()`: 分段 → detail → 结果, 列宽预算优先级
  "分段不裁 > 结果 (先保状态词, 正文再截) > detail (不足 8 列整段省略)", 单行恒 1 行。
- 已处理展开 `buildProcessed()`: 头行 (标记 + 分段 + 两空格 + 结果) + 内容块
  (`layoutForm(..., contentOnly=true)` 跳过 `control`/`submit`)。
- `HitBox::Kind { Form, Fold }`; `registerFoldHit()` 用 `OwnedReflect` 自持反射框登记整行命中区;
  `handleClick` **先判 Fold** (整块, 不看 region) 再走原控件逻辑; 未处理态不登记 Fold。
- `estimate()`: `Waiting` 现状 / `Collapsed` 恒 1 / `Expanded` = 1 + 内容块行数 (同一布局过程)。
- 自动折叠: `confirm()` → `collapsed = true`; `cancel()` → 同请求全部 `Canceled` + `collapsed = true`;
  `agent_tui.cpp` 的 `WireInterruptExpired` → `collapsed = true`; 建行时 `collapsed = false`。
- 删除了旧的 `buildStatusLine()` (单行状态行被摘要行取代), 结果文案抽成 `statusTextOf()` /
  `statusColorOf()`, 折叠行与展开行共用同一份文案与配色。

### 7. 工具显示名 (`message_list.h` / `.cpp`, `agent_tui.cpp`)

- 抽出 `MessageListComponent::toolDisplayNameOf(st, toolName)` (原
  `genPlaceholderToolDisplayName` 的查询体): 命中显示名返回显示名; 渲染中返回空串
  (本帧不画该段); 无渲染器返回原始工具名。占位提示改为调用它 (行为不变)。
- 构造时注入 `interruptView_.setToolDisplayNameResolver(...)`, 内部走 `toolDisplayNameOf`。
- `agent_tui.cpp`: `handleInterrupt` 建行前 `prefetchInterruptToolNames()` —— 扫
  `header.segments` / `summary.segments` 里 `kind=="tool"` 的分段, 按"只给工具名"的输入特征
  各投递一次渲染请求 (与 UI 侧查询完全同特征, 命中缓存即只读)。
- 粗略高度 (`quickHeight`): 已处理且折叠 → 2 行 (摘要 + 尾部空行), 其余保持原公式。

### 8. 测试

- `interrupt_ui`: 新增 `test_ui_summary_and_segment_kind`; 权限卡片用例补三段语义 /
  读写着色 / `bold=false` / `summary.detail` / 自定义分类 / 无目标。
- `interrupt_bus`: 权限请求描述断言更新 (`kind` / 权限名键与颜色 / 不再有前导空格 / `summary.detail`)。
- `tui_interrupt`:
  - 夹具新增 `stripAnsi()` (`Screen::ToString()` 会为每次样式切换插 SGR, 跨样式断言必须先去),
    `clickText()` (按屏幕行点击头行/摘要行), `clickFold()` / `foldHitCount()`;
  - 新增用例: `test_waiting_is_not_collapsible` / `test_submit_auto_collapses_to_summary` /
    `test_summary_expand_and_collapse` / `test_cancel_and_expired_auto_collapse` /
    `test_collapsed_ignores_keyboard` / `test_tool_display_name_three_states`;
  - 既有用例调整: 头行断言 `filesystem_read` → `读取文件`; 过期态用例同时置 `collapsed`.

### 9. 文档

- `docs/zh-cn/design/tui.md` §2.6: 新增"中断消息的形态由处理状态决定"一条 (三形态表 +
  标记列 + 折叠行列宽权衡规则 + 摘要来源 + 命中只在头行/摘要行) 与工具显示名 / 权限名来源一条。
- `interrupt_ui.h`: `kind` / `summary` 的用途与回退规则; `interrupt_presets.h`: `permissionCard`
  头注释重写 (三段语义 + `summary.detail` + 分类键表); `interrupt_view.h`: 类注释补三形态表与
  命中说明 (含"未处理不可折叠")。
- `resource/history/interrupt-collapse-summary/index.md`: 状态改"已实施" + 结论回填。

### 验证 (Windows debug, ASan)

```text
# 本次改动模块
tui_interrupt / interrupt_ui / interrupt_bus              -> 853 passed, 0 failed
# 界面层回归
ui_items tui_ui_items tui_form tui_widget tui_tool_header
  + tui_interrupt interrupt_ui interrupt_bus               -> 1694 passed, 0 failed
# 展示历史 / 会话 / 滚动回归
tui_stream tui_surface view_replica session_persistence tui_scroll
                                                            -> 2714 passed, 0 failed
# 其余客户端模块
tui_theme tui_input tui_settings banner_art ui_kit
  + plugin_sdk plugin_bridge client_plugins                 -> 7428 passed, 0 failed
```

编译: `agentxx_shared` / `agentxx`(client) / `agentxx_test` 三个内层目标 Debug 构建通过
(注意: 改了 lib 头文件后必须先跑 lib 的 `INSTALL` 目标, 否则 client 仍按安装树里的旧头编译)。

全量 `agentxx_test` 单次跑完耗时过长 (> 30 分钟仍未结束), 改为按上述分组覆盖受影响模块;
另: `boundaries` 模块的失败是既有的 (第三方库 `cxx_pluginxx_ui` 文档里用了停用词 `载体`),
与本次改动无关 (上一提交的回归记录已标注)。

## 与计划不一致的地方 / 临时绕过

1. **权限名字面文本去掉前导空格**: 计划 §3.3 已定 (空格交给客户端拼接规则), 但老客户端
   (不认识 `kind`, 也不做补空格) 会显示成 `agentxx_filesystem_writefilesystem_write`。
   按计划接受 (wire 结构不变; 老客户端属降级路径)。
2. **`buildStatusLine()` 删除**: 计划只提到"已处理折叠成摘要行、展开显示详情", 旧的单行状态行
   在两种形态下都不再出现, 因此直接删掉而不是保留分支。
3. **摘要列宽预算实现**: 计划写"依次牺牲 detail → 结果正文", 实现为
   "分段 → 结果 (状态词必留) → detail (不足 8 列省略)"; 视觉顺序仍是 分段 · detail · 结果,
   与计划一致, 仅分配顺序显式写在代码注释里。
4. **`InterruptView` 新增私有类型** (`Mode` / `ResolvedSegment`) 与 helper
   (`resolveSegments` / `joinSegments` / `segElements` / `firstContentLine` / `textColumns`),
   计划未逐项列出, 属实现细节。
5. **测试侧 `stripAnsi()`**: 本仓 FTXUI 的 `Screen::ToString()` 会输出 SGR 转义,
   跨样式断言 (标记 + 分段) 必须先去转义。这是测试基础设施, 不影响生产代码。
6. **过期态的自动折叠未做端到端用例**: `WireInterruptExpired` 的处理在 `TUIClientAgentIO`
   内部, 现夹具只驱动 `MessageListComponent`; 用例里直接落定"状态 + collapsed"两个字段,
   代码路径本身由实现阅读保证 (见下方"遗留事项")。

## 验证清单

- [x] `agentxx_test interrupt_ui` 通过
- [x] `agentxx_test interrupt_bus` 通过 (权限卡片 JSON 断言已更新)
- [x] `agentxx_test tui_interrupt` 通过 (新增折叠/展开/估算用例)
- [x] 回归: `agentxx_test tui_widget tui_form ui_items tui_ui_items tui_tool_header` 通过
- [x] 回归: `agentxx_test tui_stream tui_surface view_replica session_persistence tui_scroll` 通过
- [x] Windows debug 编译通过 (lib / client / test)
- [ ] TUI 手测: 权限询问未处理态、允许后折叠 (带结果)、再展开、取消后折叠、过期折叠
  (需要人工在真实会话里发一次需要权限的工具调用; 自动用例已覆盖同一套渲染与交互路径)

## 收尾核对 (逐项对照, 均在 `tui_interrupt` / `interrupt_ui` 用例中断言)

| 核对项 | 结论 | 依据 |
|---|---|---|
| 权限名两档只差颜色 (`read` → `hint`, `write` → `thinking`), 分段 `bold` 均为 false | 通过 | `interrupt_ui::test_preset_permission_and_confirm_card` 断言颜色与两个 `bold == false` |
| 折叠行包含处理结果尾缀 | 通过 | 已确认 (`已确认` + 结果值) / 已取消 / 已过期 三个用例均在折叠行文本中断言 |
| 展开已处理消息不出现控件 | 通过 | 展开态断言无 `[   ]` 与 `允许`; 点原控件 id 返回 false (无命中区) |
| 未处理消息点击头行不折叠 | 通过 | `test_waiting_is_not_collapsible`: 点头行后 `collapsed` 仍 false, 且 Fold 命中区为 0 |
| 已处理消息摘要行/头行双向翻转 | 通过 | `test_summary_expand_and_collapse`: 点摘要展开 → 点头行收起 |
| 无"原名 → 显示名"跳变 | 通过 | `test_tool_display_name_three_states`: 渲染中 (resolver 返回空) 该段不出现原名 |

## 遗留事项

- `WireInterruptExpired` 的端到端用例需要一个能注入 wire 消息的 client IO 夹具
  (`TestTUIClientIO` 目前只暴露 delta/sync), 本次未扩展。
