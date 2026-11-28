# 方案: 压缩提示消息带摘要正文 / 去掉 system_monitor 的状态栏项 / Tip 展开态 markdown 渲染

## 背景

两件互不相关的小改动, 同一批处理:

1. `agentxx_system_monitor` 的 client 侧同时注册了侧边栏 Info 段落
   (`agentxx_system_monitor.usage`, 槽位 `info`) 与状态栏项
   (`agentxx_system_monitor.status`, 槽位 `status`), 两处显示同一份 CPU/内存/GPU 数据。
   状态栏那一行信息量有限, 而且状态栏位置被占用后其他内容（模型、上下文占用、会话名）
   可用的列宽更少, 因此去掉。
2. 上下文压缩完成时, 提示消息只显示结果行 `Summarized LLM Context a->b/max · 耗时`,
   看不到这次压缩实际写回上下文的摘要内容。排查"压缩后模型为什么丢信息"时需要去会话库
   或日志里找, 不方便; 把摘要正文放进提示消息后, 展开提示即可看到。

## 现状调查

### 插件侧 (改动前)

- `SysMonClientCtx::status` 保存状态栏内容句柄; `kStatusItemId = "agentxx_system_monitor.status"`;
  `buildStatusLine(st)` 拼接一行 `CPU x% · RAM y% · GPU z%`;
  `refreshUsageDisplay()` 收到 usage 广播后同时推 Info 段落与状态栏两处内容;
  `sysMonClientSetupAsync()` 里两条 `clientRegisterContentAsync` (info / status)。
- 订阅、命令、agent 侧采集与能力调用与状态栏无关, 不受影响。

### 压缩侧 (改动前)

- `SummarizationMiddlewareHandle::onModelcallRunFunc` (自动压缩) 与
  `compactSessionContext` (手动压缩) 两处:
  - 开始时追加/复用一条 `Role::Tip` 的提示消息 (`Summarizing LLM Context...`, `tip->running = true`);
  - 结束时把同一条消息的正文更新为
    `fmt::format("Summarized LLM Context {}->{}/{} · {}", 旧 token, 新 token, 上限, 耗时)`,
    清除 `running`, 经 `updateViewMessage` + `WireDelta::UpdateMessage` 下发。
- 摘要正文 `summary` 只在 `if (messages.size() > systemCount)` 块内可见, 更新提示消息的代码
  在该块之外, 所以此前没有拿到它。

## 方案

### 1. 去掉状态栏项

删除 `status` 成员、`kStatusItemId`、`buildStatusLine`、`status` 槽位的注册与
`refreshUsageDisplay` 里的推送分支; client start 注释从"三条注册"改为"注册"。
`plugin.yaml` 的接口说明注释去掉 `status` 槽位。

### 2. 提示消息追加 `[Result]` 段

- 两处压缩都在 `if (messages.size() > systemCount)` 之前声明 `std::string compactionSummary;`
  (注释写明: 未压缩/硬截断兜底时为空), 在 `!summary.empty()` 分支里赋值。
- 更新提示消息时追加 (前后空行 = markdown 段落分隔, 让 `[Result]` 单独成行):

  ```cpp
  if (!compactionSummary.empty()) {
      vm.text += fmt::format("\n\n[Result]\n\n{}", compactionSummary);
  }
  ```

- 第一行保持不变 (折叠预览取首行); 展开态整段按 markdown 渲染 (见第 3 节)。
- 只有真正做了 LLM 压缩才追加: 失败保留原消息 / 硬截断兜底不加空段。

### 3. TUI 展开态正文改 markdown 渲染

- `MessageListComponent::buildMessageBlock` 的 `Role::Tip` 展开分支由
  `paragraph(msg.text) | color(contentColor)` 改为
  `renderMarkdown(msg.text, contentColor, theme.markdownTheme, maxWidth)`,
  DomBuilder 推进 `mdBuilders` 保活 (与 Assistant/Think 一致)。
- 行高估算同步改为 `estimateMarkdownLines(msg.text, width)` —— 估算与渲染必须同一套语义
  (softbreak 合并 / 围栏 / mermaid), 否则不可见项高度偏差会造成滚动偏移错误。
- 已确认 cmark-gfm 的 softbreak 在 DomBuilder 里渲染为空格 (`parser_cmark.cpp` 的
  `CMARK_NODE_SOFTBREAK` → `NodeType::SoftBreak` → `appendSpan(spans, " ")`), 所以 `[Result]`
  前后各留一个空行: 它变成独立段落单独成行, 摘要以 `## Goal` 这类标题开头时也从下一段开始。

## 边界

- 提示消息会多出摘要正文 (通常几 KB), 随 `WireDelta` 下发一次、随会话展示历史落盘;
  LLM 上下文不受影响 (那是 `replaceMessages` 写回的另一份数据)。
- 不改协议、不改插件接口表, 老客户端按普通多行文本显示。
- 状态栏项删除只影响本插件的展示; `status` 槽位本身保留给其他插件。

## 验证

- `summarization` 模块: 自动压缩 T20 断言 `\n\n[Result]\n\nS_ViewMsg`; 手动压缩 T21 断言
  `\n\n[Result]\n\nS_Manual`; T4 (未做 LLM 压缩) 断言不含 `[Result]`。
- `tui_scroll` 新场景 9d: 展开态 Tip 正文含 `## Summary` 与 `**bold**`, 断言屏幕上不出现
  `## Summary` / `**bold**`、文本仍在 (确认走的是 markdown 渲染而不是 `paragraph`)。
- `client_plugins` 第 13 段仍断言 Info 段落与 usage 广播渲染 (不含状态栏)。
- `cpu_gpu` / `tui_widget` / `tui_surface` 回归。
- 插件 Debug 产物重新编译后检查 DLL 内已无 `agentxx_system_monitor.status`。

## 风险

- 摘要在提示消息里是纯文本 (消息本身), 渲染才是 markdown; CLI (`agent_stdio.cpp`)
  仍按原文打印, 不解析 markdown。
- markdown 渲染下相邻单换行合为空格: 所以提示文本在 `[Result]` 前后各留一个空行
  (格式 `...\n\n[Result]\n\n{压缩结果}`), 标记在展开渲染时单独成行。
- 提示消息体积变大: 只在每次压缩时发生一次, 可接受。
