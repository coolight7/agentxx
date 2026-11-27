# 插件读写展示历史与 LLM 上下文

## 目标

会话里有两份消息, 插件此前只能读到其中一份的一部分:

| 消息 | 位置 | 插件此前能力 |
| --- | --- | --- |
| 展示历史 (`ViewMessage`) | `Session::viewWindow_` + 会话库 `view_message` 表 | 只能经旧表 `agentxx.agent.session.emit_message_tip` 插一条提示 |
| LLM 上下文 (`ChatMessage`) | `Session::messages_` + 会话库 `llm_context` 表 | 只能经旧表 `agentxx.agent.context.get_messages` / `messages_count` 只读 |

这次把两者都做成**可读可写**, 并合成一组对称的成员放在会话表里:

- 删除整张 `AgentxxPluginContextIface` (`agentxx.agent.context`) 及其
  `get_messages` / `messages_count`;
- 删除 `AgentxxPluginSessionIface.emit_message_tip` (提示消息改由"写一条 `tip` 角色的展示消息"表达);
- `AgentxxPluginSessionIface` 新增 10 个成员, view 与 llm 各一套
  `get_*` / `count_*` / `set_*` / `append_*` / `update_*`;
- `append_*` 收 JSON **数组** (一次多条), `update_*` 收 `[{id, ...}, ...]` 按消息 id 更
  新多条 —— 为此 LLM 上下文消息也获得由会话分配的 id (`get_llm_messages` 的每条消息带
  `id` 字段)。

## 状态

已实施 (2026-11-14)。结论与偏差见文末, 实施记录见 [work.md](work.md)。

## 关键决策

1. **两张表合成一张**: 展示历史与 LLM 上下文都是"某个会话的消息", 按作用域取值方式完全
   一致 (host + agent + session_id), 放在会话表里插件不必区分该查哪张表。
2. **LLM 消息有 id**: `Session` 为每条上下文消息维护一个 id (`llm_000001` 递增), 与
   `messages_` 同寿命; `replace/truncate/setSystem` 一并维护, 插件按 id 更新不受下标位移影响。
   id 不落库 (进程内稳定), 重启后按加载顺序重新分配。
3. **写入即推增量**: 展示历史的 append/update 复用 `EventBridge` 同一条路径 ——
   `Session::appendViewMessage` / `updateViewMessage` 落库 + `WireDelta`
   (`insert_message` / `update_message`) 广播; `set` (整段替换) 让端点重推一次
   尾部同步 (`AgentIOBase::onViewMessagesReplaced`), 客户端 `ViewReplica::reset` 重建。
4. **成败与缺项**: `update_*` 找不到的 id 不算失败 (插件可能持有过期 id), 经
   `payload` 的 `{"updated":N,"missing":[...]}` 交回; 其余错误 (session 不存在 / JSON
   非法 / 元素缺 id) 按 FAILED 交回。
5. **视觉为空的消息不写入展示历史**: 与 `EventBridge` / 加载侧同一判据
   (`ViewMessage::hasVisibleContent`), 避免插件写出"只有一行空白"的行。
6. **版本**: 会话表 `VERSION`/`MIN_VERSION` 重置为 5 (`BASELINE_SIZE` = 完整布局),
   全局插件 API 代次 8 → 9 (删表 + 改表布局都是破坏性改动, 旧插件在加载期被拒)。

## 结论

- 10 个成员按 view / llm 两组对称排布, 全部是异步操作契约 (受理即返回, 结果走 `payload`)。
- 展示历史写入与 agent 自己的写入走同一套落库 + 增量路径, 客户端无需改协议。
- LLM 消息 id 只在进程内稳定 (不落库), 重启后重新分配; 插件不应把它当长期标识。
- 插件写 LLM 上下文不会刷新图状态的只读影子通道 `xx_messagesMeta`; 下一轮节点写入时
  自然刷新 (文档已写明)。

## 偏差

- `update_llm_messages` / `update_view_messages` 的 `missing` 用数组而非布尔, 便于插件
  判断"哪几条已经不存在"。
- 额外修了一处依赖旧表的示例 (`example_graph_node`) 与 JS 侧同名接口 (`getMessages` →
  `getLlmMessages`, `messagesCount` → `countLlmMessages`, `emitMessageTip` 删除)。
- 测试里 17 份 `findPluginDir` 辅助函数的目录判定放宽为"有动态库, 或有清单 + `plugin.js`":
  `example_js` 已是纯脚本插件 (没有库), 旧判定会让相关用例直接跳过或断言失败。
  同时清掉了 `exec/plugins/example_js/` 里上一代 C++ 壳残留的 `libexample_js.dll` / `.pdb`。
