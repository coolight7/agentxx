# 事件消息的展示与内容格式调整

| 项 | 内容 |
|---|---|
| 任务短名 | `tui-event-message-format` |
| 状态 | **已完成**（实现 / 测试 / 文档） |
| 范围 | `agent/lib`（`event/hub*.{h,cpp}`、`tools/async_tools.cpp`、`agent/config.*`、`agent/base_agent.cpp`）+ `agent/client`（`message_list.cpp`、`tui_i18n.cpp`）+ 测试 + 文档 |
| 目标 | 事件折叠头配色与分隔符修正；事件正文从 JSON 改为 `[Event]`/`[Info]`/`[Content]` 分段纯文本；长内容与工具输出走同一套 share store 压缩 |

## 目标（一句话）

事件消息一眼能认出（高亮头 + 弱化的名字与内容），内容不再被 JSON 转义包一层，
过长时和工具结果一样转存 share store。

## 四项改动

1. **折叠头配色**：`+`/`-` 标记与 `[事件]` 前缀用高亮色 (`theme.accentColor`)；
   折叠态下事件名与内容预览叠加 `dim`（展开态名字保持高亮）。
2. **分隔符空格**：`事件名· 内容` → `事件名 · 内容`（原 `sep` 是 `"· "`，标签结尾没有空格）。
3. **正文格式**：三段式纯文本
   ```text
   [Event] agentxx.agent.async.done
   [Info]
   mode: first
   wait: w-3
   status: value
   [Content]
   {"asyncId":"async-7","kind":"tool","name":"...","hasError":false}
   content:
   <工具输出原文, 多行就是多行>
   ```
   值本身是**别人的 JSON**: 对象/数组原样 `dump()`, 不解释字段; 只有框架自己的
   `agentxx.agent.async.*` 把 `content`(产物正文) 按原文单独给出。
4. **长内容压缩**：`[Content]` 段超过阈值时复用
   `agentxx::util::offloadShareStoreIfOversized`（与 toolcall 输出压缩同一份实现与提示），
   阈值 = `toolcallSummaryLimitOutputLength`（默认 2048 字符）与 `event.value_max_bytes`
   取较小者。

## 后续调整（用户提出）

5. **无值写空段标记**：本次投递没有值时，最后一段的标记从空的 `[Content]` +
   `(no value)` 占位改成 `[Content: empty]`（标记常量放 `agentxx/event/text_sections.h`，
   渲染侧与 TUI 折叠预览共用）。TUI 见到它就不画 ` · ` 与预览 —— 本来就没内容。
   （中间一版写的是 `[No Content]`，用户要求与命令工具的空段统一成 `: empty` 写法后改了名。）
6. **事件 inject 不再前置来源标记**：`drainPendingSessionInputs` 只对非事件来源加
   `[<来源>] ` 前缀。来源前缀原本是给“只在本请求可见、历史里查不到来历”的注入内容
   标出来源（commit `7ffa773d`，记录见 `resource/history/compare-agent/work.md` LOOP-2），
   而事件正文首行已是 `[Event] <名字>`、`[Info]` 里还有 `mode`/`wait`/`status`，
   再叠 `[event:<名字>]` 只是重复。
7. **命令工具的空段**：`agentxx_execute_command` 插件的 `[StdOut]` / `[StdErr]` 段在没有
   输出时写 `[StdOut: empty]` / `[StdErr: empty]`（不再留空行），与事件的
   `[Content: empty]` 同一种写法；示例插件 `example_js_execute_command` 同步。

## 关注点

- `renderEventText` 同时被事件投递（`EventHub::flushWait`）与 `agentxx_async_await`
  工具结果使用，两处一起换格式，避免模型看到两种形态。
- 折叠预览取 `[Content]` 段第一行：首行 `[Event] <名字>` 与头部重复，`[Info]` 只是元信息。
- `EventHub::valueText`（逐值卸载）删除，改为整段内容一次性卸载 —— 保留逐值卸载会在
  默认阈值下变成两次卸载（两个 share store id）。
- 配置键不变（`event.value_max_bytes`），仅语义从"单值"改为"内容段"，文档同步。

详见 [plan.md](plan.md)，实施记录见 [work.md](work.md)。

## 结论与偏差（事后）

- 四项改动全部完成：折叠头高亮 + 折叠态名字/内容 dim、` · ` 空格修正、
  正文改 `[Event]` / `[Info]` / `[Content]` 三段、长内容复用工具输出的 share store 压缩。
- **正文格式后经一次修正**（用户指出后改的）：`[Content]` 不再把值改写成自己
  造的 `键: 值` / `- 项` 文本 —— 值可能是别人自定义的 JSON（插件 / 模型 `agentxx_async_emit`），
  对象/数组一律紧凑 JSON 原样输出；只有框架自己的 `agentxx.agent.async.*` 把
  `content` 产物正文按原文单独给出（它本身可能是 JSON 文本，原文即原文）。
  `eventTextPayloadField` 是唯一的“解释字段”入口，其余事件不碰。
- 配置键无增删：`event.value_max_bytes` 保留（语义由"单值"改为"内容段字节上限"），
  实际阈值与 `toolcallSummaryLimitOutputLength` 取小，默认就是工具输出阈值（2 K 字符）。
- 三处后续调整（见 work.md §4.3 / §4.4）：无值改 `[Content: empty]`（TUI 据此不画预览）；
  事件 `inject` 去掉重复的 `[event:<名字>]` 来源前缀，非事件来源仍保留；命令工具的
  `[StdOut]` / `[StdErr]` 空段改 `[StdOut: empty]` / `[StdErr: empty]`。
- 与计划一致：无协议变更（展示角色与 `source=event:<名>` 未动，只是正文内容变化）；
  `agentxx_async_await` 工具结果同步换格式。
- 计划之外的调整（均记录在 work.md）：长内容只能整段卸载一次（逐值卸载会在默认阈值下
  卸载两次）；TUI 折叠预览改为取 `[Content]` 段首行（跳过空行与转存提示行）。
- 全量回归：42708 passed / 5 failed，5 项全在 `boundaries`（规则 7/8/9/10 是接口表台账、
  规则 15 是用词），属工作区既有状态 —— HEAD 的 `db234597` 刚把 `cxx_pluginxx` 子模块升到带
  `capability_info` / `list_capabilities` 的版本，而 `test_boundaries.cpp` 台账未同步；
  与本次改动无关。
