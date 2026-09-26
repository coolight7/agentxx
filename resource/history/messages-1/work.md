# 实施记录: 把 LLM 上下文从 neograph `messages` 通道迁出

- 方案: [plan.md](plan.md)
- 类型: 架构调整 (会话成为 LLM 上下文唯一权威)
- 时间: 2026-09-26
- 状态: 主体已实施并通过测试, 文档与收尾见下方"待完成任务"

## 1. 已完成任务

### 阶段 0: 零风险项 (方案 §6 步骤 0)

- [x] 去掉 `StreamMode::VALUES` (`base_agent.cpp` 的轮次 `RunConfig`):
      该模式每 super-step 都发一次 `__state__` 全量状态事件, 而 `EventBridge`
      只处理 `message_tip` / `messages`, 载荷无人消费 → 每步少一次整段序列化。
- [x] 运行结束的"双拷贝"合并: 删除 `agent_runner.cpp` 里
      `result.channel_raw("messages")` + `fromNeographJson` 的整份上下文往返,
      会话即权威结果。
- [x] `plugin_graph_node` 的 state 序列化: 迁移后图状态已与上下文大小无关
      (只含控制通道), 不再需要额外裁剪 (方案原列出的"按需/裁剪"目标由迁移本身达成)。

### 阶段 1: 会话 typed 上下文 + 单一写入 API (方案 §3.1)

- [x] `Session` 增加 typed 上下文存储 (`std::vector<neograph::ChatMessage>`) 与
      单调递增版本号 `messagesVersion`; `llmMessages` (Json) 改为**惰性生成**的
      边界形态 (`llmMessagesJson()`), 不再与 typed 存储双写。
- [x] 写入口收敛为 `appendMessages` / `replaceMessages` / `replaceMessagesFromJson` /
      `truncateMessages` (+ 兼容入口 `appendSettledLlmMessages`), 全部经
      `markMessagesChanged()` 递增版本并失效 Json 缓存。
- [x] 新增 `agentxx/nodes/session_context.h` 的节点/中间件共用入口:
      `sessionMessages()` (读) / `appendSessionMessages()` (追加 + 发通道写事件) /
      `updateMessagesMeta()` (刷新图状态里的上下文影子通道)。

### 阶段 2: 节点/中间件改为会话权威 (方案 §3.2 / §3.3 / §3.4 / §3.5)

- [x] `modelcall`:
  - `build_params(state, ...)` → `build_params(sessionId)`, 直接取会话上下文
    (不再 `state.get_messages()` 深拷贝 + 逐条反序列化);
  - system 消息按轮重建改为就地更新会话首条 (无 system 时前插);
  - assistant 回复定稿 → `appendSessionMessages` (追加 + 发 `messages` 通道写事件),
    不再走 `NodeOutput.writes`;
  - `repairMessages` 的读改写全部落在会话上;
  - 重试耗尽/取消的兜底 assistant 消息同样写入会话。
- [x] **循环路由改为命令式**: `llm` 节点按"本轮是否有 tool_calls"返回
      `Command.goto_node` (`tools` / `agent_end`), 图定义去掉
      `has_tool_calls` 条件边, 只保留静态默认边 `llm -> agent_end`
      (节点级 `xx_autoRoute=false` 可关闭该行为, 交给图定义/自定义路由节点)。
- [x] `toolcall`: 从会话取上下文 (只读借用 + 拷出 tool_calls 后使用),
      工具结果 / 取消补齐消息 / 中断缓存均按会话写入; 异常路径
      (`insertAbortedToolResults`) 也改写会话。
- [x] 图定义: 去掉 `messages` 通道, 新增只读影子通道 `xx_messagesMeta`
      (条数 / 版本 / 角色分布 / 末尾消息角色与 tool_call id; 与上下文大小无关)。
- [x] `summarization`: 压缩读/写全部改为会话 (`replaceMessages`), 保留压缩后
      立即落盘语义; 删除 `newMsgsJson` 死分支与"图状态回写"路径。
- [x] 播种路径: `runTurnAsync` 用户消息写会话; `runInternalAsync` 用
      `replaceMessages` 播种; 子代理 (含同上下文模式) 改为写子会话。
- [x] 异常/中断快照: 删除 `graphDataKey_tempMessages` 的"整份上下文快照 +
      轮末回灌"对账逻辑 (`wrap_handle` / `agent_runner` / `base_agent`);
      会话是权威且不随图状态回滚, 节点抛出前已写入的消息自然保留。
- [x] `EventBridge`: `messages` 通道写事件从"反解消息并追加会话"改为"请求一次
      节流落盘" (消息由写入方在发事件前已写会话), UI 增量逻辑保持不变;
      `WireGetContext` 取 `llmMessagesJson()`。

### 阶段 3: 插件契约 (方案 §3.6, 采用 (a) + (b))

- [x] (a) 影子通道: `xx_messagesMeta` (只读, 载荷与上下文大小无关),
      老插件读 `channels.messages` 不再存在但不会崩。
- [x] (b) 新增宿主服务: 新接口表 `agentxx.agent.context`
      (`get_messages` / `messages_count`), 追加在既有表之后 (不动 session 表,
      避免新插件在旧宿主上丢失整张表); SDK 便捷方法
      `AgentCtx::getSessionMessages` / `sessionMessagesCount`。
- [x] 兼容改写: 插件图节点对 `messages` 通道的写入 (`plugin_graph_node`)
      转成会话上下文写入 (append / overwrite), 并发出与宿主节点同形的通道写事件;
      空列表的 overwrite 视为误写被忽略 (防止按旧契约读到空上下文的插件清空会话)。
- [x] `plugins/example_graph_node`: 意图识别改为经宿主接口表读会话上下文,
      图定义给 llm 节点设 `xx_autoRoute=false` 由自定义路由节点接管。

### 阶段 4: 文档与验收测试

- [x] 文档: `docs/zh-cn/design/plugins.md` (接口表 18 → 19, 新增
      `agentxx.agent.context` 行, graph 表行补充图状态/通道写入改写说明)、
      `docs/zh-cn/design/index.md` (会话权威 / 图状态不含上下文 / EventBridge 挂点 /
      持久化接入点)、`AGENTS.md` (新增 "LLM 上下文归属" 要点)。
- [x] 新增验收测试 `test_agent_context_not_in_graph_state`
      (`agent/test/core/test_agent.cpp`): 8 KB 用户正文 + 唯一标记,
      断言会话上下文含该正文、图状态序列化载荷不含正文且 < 8 KiB。

## 2. 待完成任务

- [ ] 方案 §6 验收指标的量化复测 (每轮"消息正文档位块数 ÷ 消息条数" ≤ 3;
      state 序列化载荷与上下文无关) —— 需要 mock 负载 + 分配统计台账
      (memory-1 的 harness), 本机未跑。**结构性验收已由测试覆盖**:
      `test_agent_context_not_in_graph_state` (会话含用户正文、图状态载荷既不含
      正文也 < 8 KiB)。
- [ ] 内存增长模块 (`memgrowth`) 复跑: 本机被输入法 DLL (`SogouPY.ime`) 注入进程
      触发的 AddressSanitizer heap-use-after-free 打断 (调用栈全在 `SogouPY.ime` /
      `MSCTF.dll` / `USER32.dll`, 与本项目代码无关), 该模块未完成复测。
- [ ] 双轨开关 (`AgentConfig::context_owner`) 未实现 —— 见"注意事项"第 3 条。
- [ ] `docs/en/design/index.md` (英文版) 仍是旧口径: 只更新了中文设计文档与
      `AGENTS.md`; 英文文档的 `llmMessages` / 双消息集表述待同步。
- [ ] `neograph` 侧 `has_tool_calls` 条件与 `GraphState::get_messages()` 内部读点
      已无默认路径使用者, 是否清理 (或标注为"自定义图可用") 待定。

## 3. 注意事项

### 3.1 语义变化 (与方案一致, 需记住)

- 图状态不再有 `messages` 通道: `state.serialize()` / checkpoint / 插件 stateJson
  的载荷与上下文大小无关。
- checkpoint 只保存控制数据; **上下文不在 checkpoint 里**, 恢复依赖会话
  (sqlite + 节流窗口), fork / 时间旅行语义不再覆盖对话内容。
- `has_tool_calls` 条件边不再被默认图使用 (条件注册表本身保留, 自定义图仍可用);
  默认图的循环路由由 `llm` 节点的 `Command.goto_node` 给出。
- 中断/异常不再需要"整份上下文快照 + 回灌": 会话不随图状态回滚, 节点抛出前
  已写入的消息保留; 未定稿的结果 (如中断的 toolcall) 仍只在 graphData 缓存,
  resume 后重新执行节点再写入。

### 3.2 实现注意点

- 会话上下文为 typed (`neograph::ChatMessage`) 权威存储, **Json 形态
  (`llmMessagesJson()`) 是惰性生成的缓存**: 任何变更后引用即失效, 不要跨变更持有。
- 节点/中间件读写上下文一律经 `agentxx::nodes::session_context.h` 的入口,
  不要自行拼 `{"channel":"messages", ...}` 事件 —— 事件与写入必须成对
  (先写会话, 再发事件), 否则 UI 增量与落盘会与权威上下文漂移。
- `Session::messages()` 是只读借用: 不得跨 `co_await` 持有, 且使用期间不得
  追加/替换 (会 realloc)。需要修改时先 `std::vector` 拷贝。
- 会话不存在时 `sessionMessages()` 返回静态空列表; 写入侧 (`appendSessionMessages`、
  节点内的 `getOrCreate`) 会隐式建会话 —— 测试或直接跑图 (不经 `runTurnAsync`)
  的场景需要自己先把上下文写进会话 (见 `test_plugins` 35.2 / `test_summarization`
  的 `runModelcall` / `test_message_supplement` 的 repair 用例)。
- 节点级配置合并: 图定义里节点 JSON 的额外键会合并进 `NodeContext.extra_config`
  (节点级覆盖引擎级), `xx_autoRoute` / `xx_useModelRegistry` 都经此生效。
  **插件重写图定义时务必用宿主侧注册的节点工厂语义** (测试里自建工厂也要做同样合并)。
- 插件兼容: 新接口表 `agentxx.agent.context` 必须按"追加新表"的方式扩展,
  不要往既有表追加字段 —— `pluginxx` 的表可用性判定要求 `version == 1` 且
  `struct_size >= sizeof(本地定义)`, 往老表加字段会让新插件在旧宿主上丢掉整张表。

### 3.3 未做/暂缓

- 未实现方案 §7 的双轨开关 (`context_owner=graph|session`): 迁移后图状态侧
  已无上下文可回退, 双轨需要同时维护两套读写路径, 收益低; 若确需回退,
  建议用 git 回滚本次提交而不是运行期开关。
- 未新增"逐轮长上下文"基准场景 (方案 §6 提到), 分配次数复测需要先补该场景。
- `has_tool_calls` 条件函数与 `neograph` 侧 `get_messages()` 内部读点保留
  (自定义图/引擎内置节点仍可能使用), 只是 agentxx 默认路径不再依赖。

## 4. 提交记录 (按阶段)

见本文件所在目录的 git 提交历史 (`git log --oneline -- resource/history/messages-1/`)。

### 测试结论 (2026-09-26, Windows debug, ASan+UBSan)

| 模块 | 结果 |
|---|---|
| agent / cancel / message_supplement / summarization / checkpoint_store | 全通过 |
| event_stream / event_bridge / interrupt_bus / subagent_bus / subagent_tool / agent_host | 全通过 |
| session_persistence / share_store / plugins / plugin_sdk / plugin_bridge / plugin_multi_instance / client_plugins | 全通过 |
| remote_agent / ffi_c_api / acp / a2a / mcp / openai_provider / anthropic_provider | 全通过 |
| interrupt_ui / training / tui_* / 同步组工具模块 | 全通过 (未受本次改动影响) |
| memgrowth | **未完成**: 进程被 `SogouPY.ime` 触发的 ASan 报错打断 (环境问题) |
