# 压缩提示消息带摘要正文 / 去掉 system_monitor 的状态栏项 / Tip 展开态 markdown 渲染

## 目标

1. `agentxx_system_monitor` 的 client 侧不再往状态栏注册「系统占用」项, 只保留侧边栏
   Info 栏的段落与 `/sysinfo` 开关。
2. 上下文压缩 (自动与手动) 完成时, 把本次压缩写回上下文的摘要正文写进那条压缩提示
   的展示消息, 提示正文形如:

   ```text
   Summarized LLM Context {旧}->{新}/{上限} · {耗时}
   [Result]
   {压缩结果}
   ```

   实际字符串为 `...\n\n[Result]\n\n{压缩结果}`: `[Result]` 前后各一个空行, 让它在 TUI
   展开态的 markdown 渲染下单独成行。

3. TUI 展开态的提示消息正文按 markdown 渲染 (同一轮追加需求): 摘要原文是 `## Goal`
   这类标题 + 列表, 按纯文本画会露出 `##` / `**` 标记。

## 范围

- `agent/plugins/agentxx_system_monitor/agentxx_system_monitor.cpp` (client 侧注册与推送)
- `agent/plugins/agentxx_system_monitor/plugin.yaml` (接口说明注释)
- `agent/lib/src/middlewares/summarization.cpp` (自动压缩 `onModelcallRunFunc` 与手动压缩
  `compactSessionContext`)
- `agent/test/core/test_summarization.cpp` (T4 / T20 / T21 断言)
- `agent/client/src/io/tui/components/message_list.cpp` (Tip 展开态渲染与高度估算)
- `agent/test/client/test_tui_scroll.cpp` (场景 9d 断言)
- `docs/{zh-cn,en}/design/{index,plugins}.md` (插件渲染位置与压缩提示正文说明)
- `docs/zh-cn/design/tui.md` §2.16 (Tip 展开态 markdown 渲染)

## 状态

已完成。

## 关键决策

- 状态栏项整体删除, 不保留"关掉时推空文本"的分支: 同一份数据在 Info 栏已经能看到,
  状态栏只画一行信息量有限; `/sysinfo` 仍控制 Info 栏的显示/隐藏, 语义不变。
- 摘要正文只在**真正做了 LLM 压缩**时追加 (`compactionSummary` 非空); 硬截断兜底与
  "压缩失败保留原消息"这两条路径不加 `[Result]` 段, 避免提示消息出现空段。
- 结果行 (第一行) 保持不变: 折叠态的预览取首行, 所以列表里的显示与原来看不出区别,
  展开这条提示才能看到摘要正文; 测试里的 `starts_with("Summarized LLM Context ")`
  断言全部继续成立。
- Tip 展开态用 `renderMarkdown` (与 Assistant/Think 同一套) + `estimateMarkdownLines`
  估算行高, 不再用 `paragraph` + `estimateLines`; DomBuilder 由 `mdBuilders` 持有到该块销毁。
- `[Result]` 前后各留一个空行 (markdown 段落分隔): 相邻单换行 (softbreak) 会被合并成空格,
  不留空行时标记会接在结果行末尾。

## 结论

- 插件: `SysMonClientCtx` 去掉 `status` 成员, 删除 `kStatusItemId` / `buildStatusLine`
  与 `status` 槽位的注册、推送; client start 注册事务只剩 Info 段落、广播订阅与命令。
- 压缩: 两处压缩路径都把摘要文本留在作用域外 (`compactionSummary`), 更新提示消息时
  末尾追加 `\n\n[Result]\n\n{摘要}`。
- TUI: `Role::Tip` 展开态改走 `renderMarkdown(msg.text, contentColor, markdownTheme, maxWidth)`,
  行高估算改 `estimateMarkdownLines` (与 Assistant/Think 一致)。
- 验证: `summarization` 485 条断言通过 (新增 3 条: 自动压缩带 `[Result]`、手动压缩带
  `[Result]`、无 LLM 压缩时不带); `client_plugins` 945 条、`cpu_gpu` 55 条、
  `tui_widget` 206 条、`tui_surface` 839 条全部通过; `agentxx_system_monitor` 插件
  Debug 产物已重新链接 (DLL 内已无 `agentxx_system_monitor.status`).
- TUI markdown 渲染验证: `tui_scroll` 1043 条 (新增场景 9d: 展开态正文的 `##` / `**` 标记
  不出现、文字仍在), `tui_surface` 839 / `tui_widget` 206 / `tui_stream` 193 /
  `tui_ui_items` 277 / `ui_items` 117 / `view_replica` 114 全部通过。
