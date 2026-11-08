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
   asyncId: async-7
   content:
     <原样多行, 无 \n 转义>
   ```
4. **长内容压缩**：`[Content]` 段超过阈值时复用
   `agentxx::util::offloadShareStoreIfOversized`（与 toolcall 输出压缩同一份实现与提示），
   阈值 = `toolcallSummaryLimitOutputLength`（默认 2048 字符）与 `event.value_max_bytes`
   取较小者。

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
  正文改 `[Event]` / `[Info]` / `[Content]` 三段纯文本、长内容复用工具输出的 share store 压缩。
- 配置键无增删：`event.value_max_bytes` 保留（语义由"单值"改为"内容段字节上限"），
  实际阈值与 `toolcallSummaryLimitOutputLength` 取小，默认就是工具输出阈值（2 K 字符）。
- 与计划一致：无协议变更（展示角色与 `source=event:<名>` 未动，只是正文内容变化）；
  `agentxx_async_await` 工具结果同步换格式。
- 计划之外的调整（均记录在 work.md）：数组内对象渲染改为 `- n: 1`（原计划未细化）；
  长内容只能整段卸载一次（逐值卸载会在默认阈值下卸载两次）；
  TUI 折叠预览改为取 `[Content]` 段首行（计划里已提到，实现多了一条"跳过 `#n` 分块标记"）。
- 全量回归：42684 passed / 5 failed，失败全在 `boundaries`（接口表台账与子模块头文件不一致，
  属工作区既有状态，与本次改动无关）。
