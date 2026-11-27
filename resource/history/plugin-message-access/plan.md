# 插件读写展示历史与 LLM 上下文 —— 设计方案

## 1. 背景

插件是主程序功能的扩展点, 但会话消息这一块长期只有半套能力:

- 展示历史 (`ViewMessage`, 客户端渲染的那份) 插件完全读不到, 只能经
  `agentxx.agent.session.emit_message_tip` 插一条提示消息;
- LLM 上下文 (`ChatMessage`, 发给模型的那份) 只能经
  `agentxx.agent.context.get_messages` / `messages_count` 只读, 且是一张单独的表。

插件想做"整理历史"、"把外部消息补进上下文"、"改一条工具结果"这类事都没有入口。
本方案把两份消息都做成**可读可写**, 并把成员合并到会话表。

## 2. 现状调查 (文件 / 函数级)

### 2.1 接口表声明

- `agent/lib/include/agentxx/plugin/api/plugin_api.h`
  - `AgentxxPluginSessionIface` (IID `agentxx_agent_session`, `VERSION` 4):
    `get_share_store` / `emit_message_tip` / `add_share_store` /
    `get_session_work_dir` / `is_canceled`;
  - `AgentxxPluginContextIface` (IID `agentxx_agent_context`, `VERSION` 3):
    `get_messages` / `messages_count`。
- `agent/lib/include/agentxx/plugin/api/plugin_iface_meta.h`: 每张表一条
  `IfaceMeta` 特化 + 两条 `static_assert`; 删表要一并删特化 (特化数由 `boundaries` 校验)。
- `agent/lib/src/plugins/plugin_manager_vtable.cpp`
  - 实现: `xx_context_get_messages` / `xx_context_messages_count` / `xx_emit_message_tip`;
  - 实例: `g_ifaceContext` / `g_ifaceSession`;
  - 分发: `xx_query_interface` 里按 IID 返回, 删表要去掉对应分支。

### 2.2 会话与消息存储

- `agent/lib/include/agentxx/agent/context.h`
  - `Session`: `messages_` (LLM 上下文, 唯一权威)、`viewWindow_` + `viewWindowBase_`
    + `viewSeqCounter_` + `msgIdCounter_` + `msgIndex_` (展示历史尾部窗口与 id 索引)、
    `streamingView` (直播槽)、`hooks_` (`SessionStoreHooks`)。
  - 写入接口: `appendMessages` / `replaceMessages` / `replaceMessagesFromJson` /
    `appendSettledLlmMessages` / `setSystemMessage` / `truncateMessages`;
    `appendViewMessage` / `updateViewMessage` / `beginStreamingView` /
    `format/finalize/clearStreamingView` / `restore` / `restoreWindow`。
  - 读取接口: `messages()` (const 引用)、`messagesCount()`、`llmMessagesJson()`、
    `llmMessagesJsonText()`、`viewMessageCount()`、`viewMessageAt()`、
    `viewMessageById()`、`collectViewRange()`。
  - `SessionStoreHooks`: `onAppendViewMessage` / `onUpdateViewMessage` /
    `onLoadViewPage` / `onSaveLlmMessages` (由 `makeSessionStoreHooks` 绑定, 见
    `agent/lib/src/agent/context.cpp`)。
- `agent/lib/src/agent/session_store.cpp`: 会话库结构 `view_message(seq, json, msg_id)` +
  `llm_context(id=1, json)` + `meta`; 写入 `appendViewMessage` / `updateViewMessage` /
  `saveLlmMessages`。
- `agent/lib/src/agent/io/session_server_agent_io.cpp`: `buildFullSync` /
  `buildTailSync` / `buildIncrementalSync`; `sendToPeer(WireDelta)` 写重放缓冲后广播。
- `agent/lib/src/agent/io/view_replica.cpp`: 客户端副本, `applyDelta` 处理
  `insert_message` / `update_message`, `reset` 按同步载荷重建 (未知 id 的更新忽略)。

### 2.3 插件侧消费面

- kit: `agent/lib/include/agentxx/plugin/api/agent_plugin_kit.h`
  (`AgentIfaces::context` 成员、`getSessionMessages` / `sessionMessagesCount`);
- 示例插件: `agent/plugins/example_graph_node/example_graph_node.cpp` (用 kit 读上下文);
- JS 桥: `agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`
  (`B_GET_MESSAGES` / `B_MESSAGES_COUNT` / `B_EMIT_MESSAGE_TIP` 与 `def(...)` 注册);
- 主程序宿主侧封装: `PluginMount` (`agent/lib/include/agentxx/plugin/plugin_mount.h`,
  实现散在 `plugin_manager_vtable.cpp` 尾部)。

### 2.4 会被改动牵动的校验

- `agent/test/core/test_boundaries.cpp`: 规则 7 (数量常量与文档一致)、规则 8 (表名集合)、
  规则 9 (四件套 + 特化数 = 表数, 期望 25 张)、规则 10 (成员顺序清单)。
- `agent/test/plugin/test_plugin_host_mount.cpp`: `kInterfaceTableCount` = 19 与 19 个 IID 清单。
- `PluginManager::kInterfaceTableCount` (9 + 10)。
- 文档: `docs/zh-cn/design/plugins.md` (表清单 §8、版本代次 §17)、
  `docs/zh-cn/design/index.md` (接口表小节)、`docs/en/design/plugins.md`、根 `AGENTS.md`。
- `agent/CMakeLists.txt`: `AGENTXX_PLUGIN_API_VERSION` = 8 (含"缓存纠正"分支)。

## 3. 方案

### 3.1 接口表 (会话表 v5)

`AgentxxPluginSessionIface` 新成员顺序 (`emit_message_tip` 删除):

```text
get_share_store | add_share_store | get_session_work_dir | is_canceled
get_view_messages | count_view_messages | set_view_messages |
append_view_messages | update_view_messages
get_llm_messages | count_llm_messages | set_llm_messages |
append_llm_messages | update_llm_messages
```

公共约定 (与其它表一致): 全部是异步操作契约, 参数是借用视图 (主程序先拷贝再执行),
`agent` 引用无效 → 入口返回 `NULL` + `error_out`; 受理后失败 (会话不存在 / JSON 非法)
→ 完成回调 `FAILED` + 原因文本。

| 成员 | 入参 (除 host/agent/session_id) | `payload` |
| --- | --- | --- |
| `get_view_messages` | — | `ViewMessage` JSON 数组 (每条带 `id`) |
| `count_view_messages` | — | 十进制条数 |
| `set_view_messages` | `messages_json` (数组) | 分配的 id 数组 |
| `append_view_messages` | `messages_json` (数组) | 分配的 id 数组 |
| `update_view_messages` | `messages_json` (数组, 每条带 `id`) | `{"updated":N,"missing":["id",…]}` |
| `get_llm_messages` | — | `ChatMessage` JSON 数组 (每条带 `id`) |
| `count_llm_messages` | — | 十进制条数 |
| `set_llm_messages` | `messages_json` (数组) | 分配的 id 数组 |
| `append_llm_messages` | `messages_json` (数组) | 分配的 id 数组 |
| `update_llm_messages` | `messages_json` (数组, 每条带 `id`) | `{"updated":N,"missing":["id",…]}` |

要点:

- `set_*` 是整段替换; `set_view_messages` 在流式输出中 (直播槽非空) 拒绝, 避免把
  未定稿的那段历史接错位置;
- 展示历史写入过滤"视觉为空"的消息 (`ViewMessage::hasVisibleContent`), 与
  `EventBridge` 写入侧同一判据;
- 提示消息改写成 `{"role":"tip","text":"…","tip":{"tip_level":"info|warning|error"}}`
  走 `append_view_messages`;
- LLM 侧消息里的 `id` 字段只在插件读时附加, 写入时忽略 (由会话分配)。

### 3.2 LLM 消息 id

`Session` 增加两条状态:

```cpp
std::vector<std::string> messageIds_;      ///< 与 messages_ 一一对应
uint64_t                 llmMsgIdCounter_;  ///< llm_000001 递增
```

维护点 (全部是 `messages_` 的唯一改写入点):

- `appendMessages`: 每条追加生成 id;
- `replaceMessages` / `replaceMessagesFromJson`: 整体重新分配;
- `truncateMessages`: `messageIds_.resize(count)`;
- `setSystemMessage`: 就地改内容保 id; 插入开头时同时插一个新 id。

新增读取/写入接口:

```cpp
const std::string& messageIdAt(size_t index) const;
std::string llmMessagesJsonWithIds() const;                 ///< get_llm_messages 的载荷
std::vector<std::string> appendMessagesWithIds(std::vector<neograph::ChatMessage>);
std::vector<std::string> replaceMessagesWithIds(std::vector<neograph::ChatMessage>);
struct LlmMessageUpdateResult { size_t updated; std::vector<std::string> missing; };
LlmMessageUpdateResult updateMessagesByIds(std::vector<std::pair<std::string, neograph::ChatMessage>>);
```

`llmMessagesJsonWithIds` 逐条 `neograph::to_json` 后补 `"id"`, 与
`llmMessagesJsonText` 同样不构造整段数组 DOM。

### 3.3 展示历史整段替换

新增一条持久化回调与一个存储方法:

- `SessionStoreHooks::onRewriteViewMessages(const std::vector<ViewMessage>&, uint64_t msgIdCounter, uint64_t viewSeqCounter)`;
- `SessionStore::rewriteViewMessages(sessionId, msgs, msgIdCounter, viewSeqCounter)`:
  单事务 `DELETE FROM view_message` + 逐条 `INSERT` (显式 seq) + 写回
  `meta.msgIdCounter` / `meta.viewSeqCounter`;
- `Session::replaceViewMessages(std::vector<ViewMessage>)`: 直播槽非空时直接返回空
  (未定稿那段不能接错位置); 先丢弃待落盘队列 (它们按旧绝对下标引用), 重置窗口/计数器/
  索引, 逐条分配新 id 与 seq, 再调 `onRewriteViewMessages` 一次落库; 返回分配到的 id。

### 3.4 客户端同步

展示历史 append/update 复用既有增量:

- append: `Session::appendViewMessage` 后发 `WireDelta{InsertMessage}` (seq 取
  `Session::nextDeltaSeq()`);
- update: `Session::updateViewMessage` 后发 `WireDelta{UpdateMessage}` (未知 id 客户端忽略);
- set: `AgentIOBase` 新增虚函数 `onViewMessagesReplaced()` (默认空实现),
  `SessionServerAgentIO` 覆写为 `sendToPeer(buildTailSync(effectiveSyncTailCount()))`,
  客户端 `ViewReplica::reset` 整体重建。

### 3.5 kit / JS / 示例

- kit (`agent_plugin_kit.h`): 删 `AgentIfaces::context` 与
  `getSessionMessages` / `sessionMessagesCount`; 新增 10 个协程入口
  (`getViewMessages` / `countViewMessages` / `setViewMessages` / `appendViewMessages` /
  `updateViewMessages`, `getLlmMessages` / `countLlmMessages` / `setLlmMessages` /
  `appendLlmMessages` / `updateLlmMessages`);
- JS 桥: `getMessages` → `getLlmMessages`, `messagesCount` → `countLlmMessages`,
  删 `emitMessageTip`, 补其余 8 个 (`getViewMessages` … `updateLlmMessages`), 全部保持
  Promise 语义;
- `example_graph_node` 改用 `ctx.getLlmMessages(...)`。

### 3.6 版本

- 会话表: `VERSION` 5 / `MIN_VERSION` 5 / `BASELINE_SIZE` = `sizeof` (布局重置);
- 删除 `AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT_*` 四个宏与 `IfaceMeta` 特化;
- 全局 `AGENTXX_PLUGIN_API_VERSION` 8 → 9 (`agent/CMakeLists.txt` 的默认值与
  "缓存纠正"分支同步改); 旧插件在加载期被拒, 内置插件需重建。

## 4. 边界与约束

- 只读会话 (未加载) 不支持: 一律用 `SessionsManager::get` (已存在才返回), 不顺手创建空会话;
- 插件写 LLM 上下文不刷新图状态影子通道 `xx_messagesMeta`, 下一轮节点写入时刷新;
- `get_*` 是全量返回 (与旧 `get_messages` 一致), 长会话下由插件自行控制调用频次;
  分页参数属于可选追加, 本次不做;
- 展示历史写入必须在 agent io 线程执行: 表调用经 `startTableOp` 投到管理器 io 线程,
  与会话的 `assertIoThread` 约束一致。

## 5. 分步实施

1. 会话与存储: LLM id、`llmMessagesJsonWithIds`、`append/replace/updateMessagesByIds`、
   `replaceViewMessages`、`onRewriteViewMessages` + `SessionStore::rewriteViewMessages`、
   `AgentIOBase::onViewMessagesReplaced` + `SessionServerAgentIO` 覆写。
2. 接口表: `plugin_api.h` 会话表 v5 + 删 context 表; `plugin_iface_meta.h` 删特化;
   `AGENTXX_PLUGIN_API_VERSION` 9。
3. 主程序实现: `plugin_manager_vtable.cpp` 删旧 3 个成员实现与 `g_ifaceContext`,
   新增 10 个成员实现; `PluginMount` 增 10 个方法。
4. 消费面: kit、JS 桥、示例插件。
5. 测试: `test_boundaries` 数量/名字/顺序清单、`test_plugin_host_mount` 表清单、
   `test_plugin_runtime` 新增会话消息表用例 (内存 + 会话库两种)。
6. 文档: `plugins.md` (zh/en)、`index.md`、根 `AGENTS.md`。
7. 构建 + 跑测试 (`boundaries` / `plugin_runtime` / `plugin_host_mount` / `plugin_sdk` 等)。

## 6. 验证

- 编译: `agent/script/windows_debug_build.bat` (顶层) + 插件目标按需重建;
- 测试模块: `boundaries`、`plugin_runtime`、`plugin_host_mount`、`plugin_sdk`、
  `plugin_cleanup`、`plugin_executor`、`session_schema`、`storage_consistency`;
- 新用例覆盖: 空会话计数、append 多条 (返回 id)、update 命中/缺失、set 整段替换
  (库内行被删除后重建)、视觉为空的消息被丢弃、会话不存在报错、LLM id 在
  replace/truncate 后仍能按 id 定位、流式中 `set_view_messages` 被拒。

## 7. 风险

- 展示历史整段替换改动 `view_message` 表内容: 事务化 + 单次重写, 失败只记日志 (与既有
  写入一致), 内存状态已更新 (与 `appendViewMessage` 同策略);
- LLM 消息 id 与 `messages_` 可能失配: 所有写入点集中在 `Session` 的 6 个方法,
  代码里不允许外部直接改 `messages_`; 新增写入点必须同时维护 id (注释写明);
- 旧插件 (含内置) 必须重建: 全局 API 代次 +1, 加载期即被拒。
