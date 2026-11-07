# 实施记录: 中断消息折叠与摘要

进度: 0 / 10 (仅设计与文档, 代码未改动)

## 任务列表

- [ ] 1. 描述层: `InterruptUiSegment.kind` + `InterruptUiSummary` + `InterruptUi.summary`
      (解析 / 序列化 / `empty()` 判定), 见 plan.md §3.2
- [ ] 2. 预设: `permissionCard` 三段特化 (权限标记 / 工具名 `kind=tool` / 权限名 i18n 键,
      **读写只差颜色, 不加粗** + `summary.detail = target`, 见 plan.md §3.3
- [ ] 3. 文案: `interrupt.permission.read|write` 新增; `interrupt.header` /
      `interrupt.permissionBadge` / `interrupt.noDescriptor` 去掉重复的 `! `, 见 plan.md §3.8
- [ ] 4. 客户端渲染: `InterruptView` 标记列 + 摘要行 + 已处理展开 (只渲染内容块) +
      `estimate`, 见 plan.md §3.4 / §3.6
- [ ] 5. 折叠交互: `HitBox::kind` (Form / Fold) + 头行 reflect + 点击翻转
      (折叠时清激活、键盘不参与), 见 plan.md §3.5
- [ ] 6. 自动折叠: `confirm` / `cancel` / `WireInterruptExpired` 三处把 `msg.collapsed` 置 true,
      创建时置 false, 见 plan.md §3.7
- [ ] 7. 工具显示名: `MessageListComponent` 注入 resolver, `InterruptView` 渲染 `kind=tool` 分段时
      按三态处理 (**未就绪留空, 绝不显示原名再跳变**; 无渲染器→原名),
      `handleInterrupt` 预取一次; 见 plan.md §3.4.1
- [ ] 8. 测试: `interrupt_ui` / `interrupt_bus` / `tui_interrupt` 三组 ($9 含收尾核对),
      见 plan.md §9
- [ ] 9. 文档: `docs/zh-cn/design/tui.md` §2.6 补折叠规则; index.md 回填结论
- [ ] 10. **收尾核对**: 权限名两档只差颜色 (分段 `bold` 均为 false)、折叠行带结果、
      未处理不可折叠、展开无控件、无"原名 → 显示名"跳变

## 每步记录 (改了什么 + 怎么验证)

(待实施)

## 与计划不一致的地方 / 临时绕过

(待实施)

## 验证清单

- [ ] `agentxx_test interrupt_ui` 通过
- [ ] `agentxx_test interrupt_bus` 通过 (权限卡片 JSON 断言已更新)
- [ ] `agentxx_test tui_interrupt` 通过 (新增折叠/展开/估算用例)
- [ ] 回归: `agentxx_test tui_widget tui_form ui_items tui_ui_items` 通过
- [ ] `agent/script/windows_debug_build.bat` 编译通过 (无新增告警)
- [ ] TUI 手测: 权限询问未处理态、允许后折叠 (带结果)、再展开、取消后折叠、过期折叠
- [ ] 收尾核对: 权限名只用颜色区分 (`bold` 均为 false)、工具名未就绪时不出现原名
