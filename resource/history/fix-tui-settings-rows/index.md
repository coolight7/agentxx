# 修复 tui 设置弹窗条目版式不一致 (测试报错)
- 难度: A
- 类型: bug修复
- 基于commit: 08aa0da5c29eefe8bd715a44544fbdfbb30f1d80
- 时间: 2026-09-24 14:20
- 需求:
```md
请修复测试错误
```

## 现象
`agentxx_test tui_settings` 6 处断言失败 (`test_settings_overlay_check_update_now_entry` 1 处、
`test_settings_overlay_groups` 5 处): 测试按"一个条目两行 (标签行 + 值行)"断言, 而代码
在 `76d75e4e` 之后已经改成"一个条目一行"。

## 结论 (按需求确认)
设置条目就是**一行**: 文字取 `settings.*Value` (整句已含条目名称与当前值/动作文案, 如
`主题: Dark`、`启动时检查更新: 开`、`立即检查`), 不再另起一行重复显示条目名称。
代码是对的, 过时的是测试与文档。

## 修改
- `agent/test/client/test_tui_settings.cpp`
  - `test_settings_overlay_groups`: "更新"组内改为一行一条目的行号断言
    (标题行 +1 = 开关条目, +2 = 条目间距空行, +3 = "立即检查" 条目)。
  - `test_settings_overlay_check_update_now_entry`: 去掉已不存在的条目名称文案断言
    ("Check for Updates Now"), 改为断言条目文字 "立即检查" / "Check Now" 与分组标题。
  - `test_settings_overlay_short_terminal_scroll` / `_scroll_indicator`: 内容行数注释
    由 30 行 (9 条目 × 2) 更正为 25 行 (9 条目 + 4 分组标题 + 8 条目间距 + 4 分组前空行)。
  - 其它注释里"点击标签行即激活"等两行版式说法改为"条目占一行, 整行可点"。
- `agent/test/client/test_tui_surface.cpp`: 设置弹窗区段的"值行"注释改为"整行"。
- `agent/client/src/io/tui/tui_i18n.cpp`: 删除 `settings.*Label` 一组已无引用的翻译键
  (`themeLabel`/`animLabel`/`logLabel`/`thinkLabel`/`langLabel`/`infoLabel`/`keybindLabel`/
  `updateToggleLabel`/`checkUpdateLabel`) —— 两行版式取消后它们不会再有使用者。
- `docs/zh-cn/design/tui.md`: §2.4 结构体清单删去已不存在的 `value` 字段并说明 `rowBuilder`
  的多行用法 (现仅会话弹窗用); §2.5 设置弹窗版式改为一行; §2.8 更新检查条目的文案键更正。
- `agent/client/include/agentxx-client/io/tui/framework/ui_action_list.h`: 用法示例改用
  现有键 (`settings.themeValue` / `settings.aboutValue`), `hint` 说明去掉设置弹窗两行的描述。

## 验证
- `agentxx_test tui_settings` 463 项全通过; `tui_surface` 645 / `tui_widget` 129 全通过。
- `agentxx_test` 全量: passed=23044 failed=0 (修复前 passed=23038 failed=6)。
- `agentxx_cli` 目标编译链接通过。
