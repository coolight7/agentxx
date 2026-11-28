# 实施记录

## 进度

- [x] 去掉 `agentxx_system_monitor` client 侧状态栏项
- [x] 自动/手动压缩的提示消息追加 `[Result]` 段
- [x] TUI 展开态提示正文改 markdown 渲染
- [x] 测试断言 (summarization T4 / T20 / T21, tui_scroll 场景 9d)
- [x] 文档更新 (zh-cn / en / AGENTS.md)
- [x] 编译 + 相关测试模块回归
- [x] 插件 Debug 产物重新编译并检查

## 改动明细

### 1. `agent/plugins/agentxx_system_monitor/agentxx_system_monitor.cpp`

- `SysMonClientCtx`: 删除 `std::shared_ptr<Content> status;`
- 删除 `kStatusItemId` 与 `buildStatusLine()` (只服务状态栏那一行)
- `refreshUsageDisplay()`: 删除 `if (ctx.status) { ... publish ... }` 分支, 现在只推 Info 段落
- `sysMonClientSetupAsync()`: 删除 `status` 槽位的 `clientRegisterContentAsync` 注册块;
  注释由"三条注册"改为"注册"
- 未改: agent 侧采集/能力、usage 广播订阅、`/sysinfo` 命令、Info 段落渲染

### 2. `agent/plugins/agentxx_system_monitor/plugin.yaml`

- 接口说明注释由"Info 栏与状态栏是 `agentxx.client.info|status` 槽位"改为只提 Info 栏

### 3. `agent/lib/src/middlewares/summarization.cpp`

- 自动压缩 (`onModelcallRunFunc`): 新增 `std::string compactionSummary;`, 在
  `!summary.empty()` 分支赋值; 更新提示消息时非空则追加 `\n\n[Result]\n\n{摘要}`
  (`[Result]` 前后空行, 让它在 markdown 渲染下单独成行)
- 手动压缩 (`compactSessionContext`): 同样处理
- 两处都只改提示消息正文, `tip->running` / `durationMs` / 统计 / 会话写回逻辑不变

### 4. `agent/client/src/io/tui/components/message_list.cpp`

- `Role::Tip` 展开分支: `paragraph(msg.text) | color(contentColor)` 改为
  `renderMarkdown(msg.text, contentColor, theme.markdownTheme, maxWidth)`,
  DomBuilder 推进 `mdBuilders` 保活 (与 Assistant/Think 一致)
- 对应行高估算: `estimateLines` 改为 `estimateMarkdownLines` (与 Assistant/Think 一致,
  否则不可见项高度偏差会造成滚动偏移错误)
- 渲染语义已确认: `parser_cmark.cpp` 把 `CMARK_NODE_SOFTBREAK` 转成 `NodeType::SoftBreak`,
  `dom_builder.cpp` 的 `appendSpan(spans, " ")` 把它渲染为空格 —— 所以提示文本在 `[Result]`
  前后各留一个空行 (段落分隔), 它单独成行, 摘要正文 (以 `## Goal` 这类标题开头) 从下一段开始

### 5. 测试

- `agent/test/core/test_summarization.cpp`
  - T4 (未做 LLM 总结): 断言提示消息不含 `[Result]`
  - T20 (自动压缩): 断言 `vm.text` 含 `\n\n[Result]\n\nS_ViewMsg`
  - T21 (手动压缩): 断言 `vm.text` 含 `\n\n[Result]\n\nS_Manual`
- `agent/test/client/test_tui_scroll.cpp`
  - 新增场景 9d: 展开态 Tip 带 `\n\n[Result]\n\n` 与 `## Summary` / `**bold**`, 断言屏幕上无
    `## Summary` / `**bold**`、文字仍在 (确认走 markdown 渲染), 且 `[Result]` 不接在结果行末尾

### 6. 文档

- `docs/zh-cn/design/index.md`: 插件目录说明由"client 状态栏渲染"改为"client 侧 Info 栏渲染";
  上下文压缩段落补充提示正文带 `[Result]` 段
- `docs/en/design/index.md`: 对应两处
- `docs/zh-cn/design/plugins.md` / `docs/en/design/plugins.md`: 插件表条目改为 Info 栏渲染
- `docs/zh-cn/design/tui.md` §2.16: Tip 展开态按 markdown 渲染 (含 softbreak 合并说明与
  `estimateMarkdownLines` 估算); 相关测试补上场景 9d
- `AGENTS.md`: Tip 展示约定补一句"展开态正文按 markdown 渲染"

## 验证

| 项目 | 结果 |
| --- | --- |
| `cmake --build agent/build/windows-debug --config Debug` | 成功 (仅既有警告) |
| `agentxx_test summarization` | passed=485 failed=0 |
| `agentxx_test client_plugins` | passed=945 failed=0 (首次运行有 2 条与本次改动无关的时序失败, 单跑复测全通过) |
| `agentxx_test cpu_gpu tui_widget tui_surface` | passed=1100 failed=0 |
| `agentxx_test tui_scroll` (含新场景 9d) | passed=1043 failed=0 |
| `agentxx_test tui_surface tui_widget tui_stream tui_ui_items ui_items view_replica` | passed=2785 failed=0 |
| `build_plugins.ps1 agentxx_system_monitor` | 成功; DLL 内 `agentxx_system_monitor.status` / `register status item failed` 均已消失 |

## 备注

- 插件 DLL 的时间戳就是本轮构建产物 (顶层构建时已链接, 单独构建插件脚本报"未重新链接"),
  已用二进制字符串检查确认新代码生效。
- `client_plugins` 里的 2 条失败 (工具行内容渲染的 `g_toolRowContexts` 计数与 part 字段)
  与本次改动无关: 该断言依赖 500 ms 内渲染结果被回填, 单跑通过。
- 文件行尾/编码已检查: 改过的文件均为 UTF-8 无 BOM + LF。
