# 事件消息展示与内容格式 — 设计方案

## 1. 背景

事件中枢（`EventHub`）把值渲染成一条 `user` 消息投递给会话，展示角色为
`ViewMessage::Role::Event`（界面折叠成 `+ [事件] <事件名>`）。当前存在四个问题：

1. 折叠头 `+`/`-` 与 `[事件]` 用的是工具色 (`theme.toolColor`)，与普通工具行没有区分度；
2. 折叠态里事件名与内容没有弱化，和展开态的视觉权重一样；
3. 折叠头 `事件名· 内容` 少一个空格（`sep` 是 `"· "`，而标签 `msg.eventSource`
   结尾没有空格 —— `[提示]` 那条末尾补了空格所以没这个问题）；
4. 正文是"键值 + JSON"，`value: {"asyncId":...,"content":"line1\nline2"}`：
   内容被 JSON 字符串转义包了一层（`\n` 变成字面量 `\n`、引号变 `\"`），模型读多行
   结果（后台任务返回的工具输出/子代理文本）时容易误判；长内容只在单值超过
   `event.value_max_bytes`（32 KB）时才转 share store，而工具输出在 2 KB 就压缩，
   两边规则不一致。

## 2. 现状调查（文件 / 函数级）

| 位置 | 现状 |
|---|---|
| `agent/lib/src/event/hub_text.cpp` `renderEventText` | 首行 `[Event] <名字> (<mode>)`，随后 `wait:`/`status:`/`value:`(或 `values:` 列表)/`note:`/`label:` |
| `agent/lib/src/event/hub.cpp` `EventHub::flushWait` | 逐值调 `valueText()` 定稿，再 `renderEventText(wait, status, texts, note)` |
| `agent/lib/src/event/hub.cpp` `EventHub::valueText` | `rawValueText`（字符串原样，其余 `dump()` 紧凑 JSON）→ 超 `cfg_.valueBytes` 时 `offloadShareStoreIfOversized` |
| `agent/lib/src/tools/async_tools.cpp`（`agentxx_async_await`） | 同样调 `hub->valueText()` + `renderEventText` 生成工具结果 |
| `agent/lib/src/agent/base_agent.cpp` `initEventHub` | `hubConfig.valueBytes = config->eventValueMaxBytes` |
| `agent/lib/include/agentxx/agent/config.h` | `eventValueMaxBytes`（`event.value_max_bytes`，32 KB）、`toolcallSummaryLimitOutputLength`（2048） |
| `agent/lib/src/nodes/toolcall.cpp` `finalizeToolCall` | 工具输出：`autoSummaryOutput` 且 `size() >= toolcallSummaryLimitOutputLength` 时调 `offloadShareStoreIfOversized` |
| `agent/client/src/io/tui/components/message_list.cpp` `Role::Event` 分支 | 标记 + `msg.eventSource` 标签（`" [Event] {}"`）+ `"· "` + 单行预览（正文首行），全部 `theme.toolColor` |
| `agent/client/src/io/tui/tui_i18n.cpp` | `msg.eventSource` = `" [Event] {}"` / `" [事件] {}"` |
| `agent/lib/include/agentxx/util/share_store_offload.h` | `offloadShareStoreIfOversized(ctx, sessionId, text, limitLength)`：按 utf8 字符数判定，写入会话 share store，返回"取回提示 + 开头片段" |

## 3. 方案

### 3.1 正文格式（`hub_text.cpp`，新增 `renderEventValuesText`）

三段固定结构：

```text
[Event] <事件名>
[Info]
mode: first
wait: w-3
status: value
[Content]
<内容>
```

- `[Event]` 行只放事件名（`mode` 移到 `[Info]`）—— 首行越短越容易在折叠预览/日志里认出来；
- `[Info]`：`mode` / `wait` / `status` 三行固定，`ref` / `label` / `note` 有值才写；
- `[Content]`：值的**纯文本**渲染（新函数 `renderEventValuesText`）：
  - 字符串原样（多行就是多行，不转义）
  - 数字/布尔/null 与 JSON 文本一致
  - 对象 → 逐行 `键: 值`；值是嵌套结构或多行字符串时，键后换行、内容缩进 2 格
  - 数组 → 逐行 `- 值`，嵌套同样缩进
  - 多个值（`mode=all`）→ 每段前一行 `#<序号>`
  - 值为空（`ended-empty` / `stream-open` 且无值）→ `(no value)`
  - 嵌套深度上限 8 层，超过退回 `dump()`（防止恶意嵌套把渲染撑爆）
- 末尾换行去掉（保持"一条消息"的原有约定）。

### 3.2 长内容压缩（`hub.cpp`）

- `EventHub::valueText(sessionId, value)` → `EventHub::contentText(sessionId, values)`：
  先 `renderEventValuesText(values)` 得到整段内容，再交给
  `offloadShareStoreIfOversized(ctx, sessionId, content, cfg_.contentLimitLength)`；
- `Config::valueBytes` → `Config::contentLimitLength`（utf8 字符数，0 = 不压缩），由
  `AgentConfig::eventContentLimitLength()` 填充：
  `toolcallSummaryLimitOutputLength` 与 `eventValueMaxBytes` 取较小者（0 视为不限制）。
  理由：字符数 ≤ 字节数，把字节上限当字符阈值用只会更早压缩，不会放过超阈值内容；
  默认两者 = `min(2048, 32768) = 2048`，即"与工具输出同一阈值"；
- 删除逐值卸载（否则默认阈值下"逐值 32 KB → 整段 2 KB"会卸载两次，给模型两个 id）；
- 提示文本沿用 `offloadShareStoreIfOversized` 的输出，因此模型看到的仍是
  `[Content offloaded. Use the `agentxx_share_store` tool ...]` + 开头片段 + `...`。

### 3.3 TUI 展示（`message_list.cpp` + `tui_i18n.cpp`）

- 新增 i18n 键 `msg.eventPrefix` = `" [Event] "` / `" [事件] "`（名字单独渲染才能分别着色）；
  `msg.eventSource` 删除（仅此一处使用）；
- 折叠标记 + 前缀：`theme.accentColor`；
- 事件名：折叠态 `accentColor | dim`，展开态 `accentColor`（与工具名同一规则）；
- 分隔符 `" · "`（补回前导空格），与预览一起 `dim`；
- 预览取 `[Content]` 段第一行（`eventContentPreview`）：跳过分段标记与多值 `#n` 行；
  找不到 `[Content]` 标记时退回正文首行（兼容旧文本与手工构造的正文）；
- 展开态正文仍按行原样输出（`renderPlainText`，工具色）。

## 4. 边界与兼容

| 项 | 处理 |
|---|---|
| 协议 / 展示角色 | 不动（`Role::Event` 与 `source=event:<名字>` 不变，只是正文文本内容变化） |
| 配置键 | 不增删；`event.value_max_bytes` 语义由"单值"改为"内容段"，文档与 `config_keys` 用例说明同步 |
| 老会话历史 | 已是旧格式正文 → 展开照常显示；折叠预览走"找不到 `[Content]` 则首行"的分支 |
| 无 share store（内存模式/无会话 id） | `offloadShareStoreIfOversized` 返回未卸载，正文原样（与工具输出同一降级） |
| 插件事件 | 值仍是任意 JSON，纯文本渲染覆盖对象/数组/标量；空对象渲染 `{}` |

## 5. 分步实施

1. `hub.h`：`Config::valueBytes`→`contentLimitLength`；`valueText`→`contentText`；
   `renderEventText` 签名改为收 `contentText`；新增 `renderEventValuesText` 声明。
2. `hub_text.cpp`：三段渲染 + 纯文本值渲染。
3. `hub.cpp`：`flushWait` 与 `contentText` 改造，删 `rawValueText`/`valueText`。
4. `config.h`/`config.cpp`：`eventValueMaxBytes` 注释 + `eventContentLimitLength()`。
5. `base_agent.cpp`：填 `contentLimitLength`，日志打印它。
6. `async_tools.cpp`：`agentxx_async_await` 改用 `contentText`。
7. `tui_i18n.cpp`：`msg.eventPrefix`（删 `msg.eventSource`）。
8. `message_list.cpp`：配色、分隔符、内容预览。
9. 测试：`event_hub`（格式逐项断言 + 多值 + 空值 + 纯文本无转义）、
   `event_hub`（长内容转 share store 且 hint 指向 `agentxx_share_store`）、
   `plugin_event`（`step: 1` 而非 `"step":1`）、`event_delivery`（首行不再带 `(mode)`）、
   `tui_scroll`（` · ` 与内容预览）、`tui_stream`（正文样例）。
10. 文档：`events.md` 第 5 节与附录表、`config-keys.md`、`AGENTS.md` 摘要。

## 6. 验证

- `agentxx_test event_hub event_delivery event_tools event_subagent async_tools plugin_event tui_scroll tui_stream config_keys`
- 手写样例打印（`renderEventText` 单元断言里带 golden 文本）。
- 全量 `agentxx_test --log-level warn`（至少 core + client 组）。

## 7. 风险

- 格式变化影响面：所有断言事件正文的测试（已列于第 9 步）与提示词里对 `[Event]`
  的描述（提示词只提首行 `[Event] <name> ...`，仍然成立）。
- `renderEventText` 是给模型看的稳定格式，改动要有 golden 断言兜住。
- 纯文本渲染可能把非常大的内容整段拼出来（比 JSON 略大），但紧接着就会走 share store
  卸载，峰值只多一份内容字符串。
