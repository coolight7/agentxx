# 展示历史单写者重构 (viewMessage-protocol) 设计方案

- 难度: D
- 类型: 架构重构 + 破坏性协议变更 (wire 协议 v4 / FFI API v3)
- 时间: 2026-11-07
- 前置: 无 (独立任务)

## 0. 结论摘要

把"一条展示消息 (ViewMessage) 怎么成型"从两处收敛到一处:

- **agent 端是唯一写者**: 消息边界、正文归组、耗时、可见性、折叠默认值、id 分配全部由 agent 决定,
  落进 `Session::viewMessages` (权威历史) 与 `Session::streamingView` (直播槽);
- **client 端退化为副本**: 只做"按 id 追加文本 / 整条 upsert / 丢弃流式项"三种动作, 不再判断
  "这段 token 属于哪条消息""要不要合并""空消息要不要显示";
- **传输是固定操作集合 + 分段整条检查点**, 不是通用 diff/patch: 追加只发文本, 其余一切变化都发整条
  消息, 因此丢 token、乱序、重连都能自然收敛;
- 本轮**不做任何旧版本兼容**: wire 协议 `kVersion`/`kMinVersion` 一起抬到 4, FFI API 版本抬到 3,
  旧代码路径 (客户端本地拼装、`ToolStart`/`ToolEnd` 增量、think 结算包、无 id 回扫等) 全部删除。

## 1. 背景

### 1.1 现状: 一条消息在两处各自成型

| 来源 | agent 端 (权威历史) | client 端 (展示) |
|---|---|---|
| 用户输入 | `BaseAgent::runTurnAsync` 里 `appendViewMessage` + `TurnStart{msgId,text,attachments}` 回显 | 按 `TurnStart` 造一条本地消息 |
| assistant 正文 | `EventBridge::handleChannelWrite` 在模型调用结束时用定稿内容造行 | `onDelta` 里把 `TextToken` 累积成一条消息 (`pushCurrentTokenLocked`) |
| think | 同上 (含 reasoning_tokens/加密标记/耗时) | `ThinkToken` 累积 + `mergeThinkData` 合并 + 结算包回填耗时 |
| 工具调用/结果 | 同上 (assistant 的 tool_calls 展开成 Tool 行, 结果按 `toolCallId` 回填 + `updateViewMessage`) | `ToolStart` 造行、`ToolEnd` 按 `toolCallId` 回扫定位回填 |
| 提示/中断头/压缩 | `InsertMessage` / `UpdateMessage` 整条下发 (已有 id) | 直接插入 / 按 id 覆盖 |

也就是说: **只有"原子消息" (提示、中断头、压缩提示) 已经是"agent 造 + client 收"的模式**,
流式正文与工具这两条最热的路径仍然两边各写一套。

### 1.2 两边必须互相模仿 (证据)

- `agent/client/src/io/tui/agent_tui.cpp::findStepMessageLocked` 的注释直接写着
  "服务端的展示历史按步骤归组 (一个步骤只有最前的一条 Think 和一条 Assistant), 这里把后到的段
  并回同步骤的同角色消息 —— 不并的话实时画面会与重开会话后的历史不一致"。
- `agent_tui.cpp::mergeThinkData` 是为了让客户端拼出来的 think 元数据与服务端一致。
- `agent_tui.cpp::isBlankContentMessage` 的注释记录了一次真实事故: 两边规则不一致导致
  **空的思考消息在实况里显示、重新拉历史后又消失**。
- `resetTrailingRunningToolsLocked` / `ToolEnd` 的 `findTarget` 两级回扫, 是在没有消息 id 的前提下
  "猜"该把那笔工具结果贴到哪一行。
- 时长语义两边各算一遍: agent 端 `EventBridge::finalizeThinkSegment` 用结算包 (空文本 ThinkToken)
  告诉 client think 段的耗时; client 端 `NodeEnd` 分支又按节点时间回填最后一条非 think 消息。

### 1.3 结构性缺口

1. **client 自建的流式消息没有 id**: `TextToken`/`ToolStart` 都不带 `msgId`, client 也不写 id,
   于是 agent 无法更新它。`UpdateMessage` 按 id 定位, 对它永远打不中。
2. **重连可能重复**: 增量补拉按 `msg.id` 去重 (`onIncrementalSync` 的 `knownIds`), 而本地拼装出来的
   消息 id 为空 → 同一条 assistant 正文可能既以"本地拼装"存在, 又以"服务端历史行"再插一次。
3. **上游一次性返回整段时界面空窗**: 非流式 provider / 缓冲型网关只写 `messages` 通道、不产 token,
   而 `handleChannelWrite` 对 assistant 正文不发任何增量 → 这段回复在下次 Sync / 重连前不可见。
4. **多客户端不一致**: 每个 client 各自拼装, 只要有一条规则 (折叠默认值/空白过滤/耗时) 不同,
   两个界面看到的就是两份不同历史。
5. **每加一种消息形态都要在 `onDelta` 里补一遍**: 角色专属字段、特殊时序 (tool_calls 展开顺序、
   加密思考首包) 都要在客户端重演一次。

## 2. 目标 / 非目标

目标:

- **G1** 消息边界与内容只有一处实现 (agent 端 `EventBridge`), 客户端不再包含任何"归组/合并/回填"逻辑。
- **G2** 传出去的每一条消息都带 id, 客户端所有动作都按 id 寻址 (`AppendText` / `Upsert` / `DropStream`)。
- **G3** 丢包、乱序、断线重连、中途接入都能收敛: 分段结束的整条消息就是检查点, 拉取 (分页/增量补拉)
  只做修复与历史加载。
- **G4** 客户端侧共用一份副本实现 (`ViewReplica`): TUI / stdio / FFI 都用它; 插件通道把 op 翻成自己的
  事件 (不建行); jsonl 只输 wire JSON (不变)。
- **G5** 删除本轮因此多余的代码与兼容分支 (见 §8.2), 协议与 FFI 版本一起抬升。

非目标:

- **N1** 不做通用 JSON patch / 结构 diff (版本、乱序、丢包一致性成本远高于收益)。
- **N2** 不把纯界面状态搬到 agent: 生成占位 (等待响应/正在调用工具)、输入框、中断表单交互、
  折叠点击、滚动、动画、本地条数上限清理, 全部留在客户端。
- **N3** 不做"每 token 发整条消息"或"客户端按秒轮询拉全量"。
- **N4** 不改 LLM 上下文 (`Session` 的 typed 上下文) 的任何语义; 本次只动展示历史。
- **N5** 不做旧版本互操作 (本轮明文放弃: 双端必须同版本, 版本不符直接拒绝握手)。

## 3. 设计总览

### 3.1 唯一写者 + 操作日志

agent 端把"展示历史"当成一份**只由自己追加的状态**, 对外只广播固定几类操作:

| 操作 | 载荷 | 语义 |
|---|---|---|
| `stream_begin` | `msgId`, `role`, `startTimeMs`, `think?` | 一段流式消息开始 (客户端据此在末尾起一条流式项) |
| `text_token` | `msgId`, `text` | 往该 id 追加正文 |
| `thinking_token` | `msgId`, `text` | 往该 id 追加思考 |
| `insert_message` | 整条 `ViewMessage` (id 必填) | 历史里出现一行 (分段定稿 / 用户输入 / 工具行 / 提示 / 中断头) |
| `update_message` | 整条 `ViewMessage` (id 必填) | 按 id 整条覆盖 (工具结果回填 / 定稿对账 / 提示运行标记) |
| `stream_end` | `msgId` | 这段流到此为止且**没有历史行** (只用于整段视觉为空); 客户端丢弃该流式项 |

界面状态另走原有的标记类增量, 不进入副本: `turn_start` / `turn_end` / `node_start` / `node_end` /
`gen_status` / `message_tip` (瞬态提示, 不落历史)。

关键取舍: **追加只传文本, 其它一切变化传整条**。所以协议里不存在"部分结构"的一致性问题 ——
丢了若干个 token 也没关系, 该段定稿时的整条消息就是检查点; 乱序也没关系, 整条覆盖总是最后生效。

### 3.2 一条消息的生命周期

```
                 (agent)                          (client)
流开始  beginStreamingView(role,start)  ──stream_begin──▶  末尾起流式项 {id,role}
        appendStreamingText(token)      ──text_token───▶  追加到流式缓冲
        mergeStreamingThink(meta)                                     │
流结束  flushOpenStream()                                             │
        ├─ 可见 → appendViewMessage(预留 id) ─insert_message─▶ 丢弃流式项, 按 id upsert 成正式行
        │        (被取消/出错的段也走这条: 半截内容照常定稿并落盘)
        └─ 整段视觉为空 → clearStreamingView() ──stream_end──▶ 丢弃流式项 (不产生行)
后续变更 (工具结果/定稿对账/提示标记) updateViewMessage ─update_message─▶ 按 id 整条替换
```

- 一"段"= 一次角色切换之间连续输出的文本。思考->正文->思考->正文 这样的交错, 就是 4 段 = 4 行,
  实时与历史一致, 客户端不需要任何合并。
- 段结束时如果整段"视觉为空" (`ViewMessage::hasVisibleContent` 为假), 就不进历史: 发 `stream_end`,
  客户端丢弃这段流式项。这是 `stream_end` 唯一的用途。
- **被取消/出错的段照常定稿成行** (可见则 `appendViewMessage` + `insert_message`, 进历史且落盘):
  与现有实现“取消后半截内容留在界面上”一致, 只是多了一步“也保存进历史”; 客户端不需要任何特例。

### 3.3 三个数据模型

1. **`Session::viewMessages`** (权威历史, 不变): append-only, 参与链式哈希 / 持久化序号 / 分页。
2. **`Session::streamingView`** (新增, 直播槽): 至多一条, **不进** `viewMessages`, 不计哈希、不占
   持久化序号、不落盘、不分页; 仅供参考"中途接入"经 `WireSyncPayload.streaming` 下发。
3. **`agentxx::agent::ViewReplica`** (新增, 客户端副本, 放 lib 供所有端点共用): 持有
   `vector<shared_ptr<ViewMessage>> rows` + 一条 `ViewStreamTail {id, role, text(shared_ptr<string>),
   think}` + 本地行 (`local_` 前缀) + 分页窗口元数据的应用入口。

### 3.4 被否掉的三种做法

- **每 token 发整条消息**: 带宽 O(n²), 且客户端每 token 要做一次整行替换。
- **通用 patch (JSON diff)**: 要自己处理字段级乱序、缺失、版本差异, 出错时的定位成本远高于重传整条。
- **客户端按秒轮询 `GetViewMessages`**: 延迟高 (至少一个轮询周期)、每次都要整页序列化, 且无法做
  平滑的逐字渲染; 只适合作为"修复"手段, 不适合直播。

## 4. 协议 v4

### 4.1 Delta 类型总表 (改前 → 改后)

| 改前 | 改后 | 说明 |
|---|---|---|
| `TextToken` (text/startTimeMs) | `text_token` (**msgId**/text) | 追加正文; 不再带时间戳 (时间在行上) |
| `ThinkToken` (text/think/startTimeMs/durationMs) | `thinking_token` (**msgId**/text) | 追加思考; 元数据与耗时改由定稿整条下发 |
| — | `stream_begin` (msgId/role/startTimeMs/think?) | 新增: 一段流开始 (含加密思考标记) |
| `ToolStart` (msgId/toolName/toolCallId/arguments/startTimeMs) | `insert_message` (role=tool 的整条) | 删除专有类型, 工具行即普通历史行 |
| `ToolEnd` (toolName/toolCallId/result/status/durationMs) | `update_message` (整条) | 同上; 成败/耗时看行字段 (`tool.status`/`durationMs`) |
| `InsertMessage` | `insert_message` | 语义明确为"追加一行, 按 id 幂等 upsert" |
| `UpdateMessage` | `update_message` | "按 id 整条替换" |
| — | `stream_end` (msgId) | 新增: 该流到此为止且不会有历史行 (整段视觉为空) |
| `TurnStart` (text/msgId/attachments/source) | `turn_start` (startTimeMs) | 只做轮次标记; 用户行改走 `insert_message` |
| `TurnEnd` (tps/tailHash/historyCount) | `turn_end` (startTimeMs/durationMs/tps) | 删除没人读的 `tailHash`/`historyCount` |
| `NodeStart`/`NodeEnd` | 保留 (nodeName) | 只用于状态栏节点名; 不再回填消息时长 |
| `GenStatus` | 保留 (genPhase/toolName) | 生成占位提示 (纯界面状态) |
| `MessageUITip` | 保留 (text/tipLevel) | 瞬态提示 (不落历史) |

`WireDelta` 结构随之瘦身 (见 §8.2 删除清单)。

### 4.2 结构与字段

```c++
struct WireDelta {
    enum class Type : uint8_t {
        StreamBegin, TextToken, ThinkToken,          // 一段流的三个动作
        InsertMessage, UpdateMessage,               // 历史行的两个动作
        StreamEnd,                                 // 流收尾且不产生历史行
        TurnStart, TurnEnd, NodeStart, NodeEnd, GenStatus, MessageUITip
    };
    std::string text;      // token / tip 文本
    std::string msgId;     // 所有消息级操作的寻址键 (必填)
    std::string nodeName;  // NodeStart/NodeEnd
    std::shared_ptr<ViewMessage> message;  // insert/update 的整条载荷
    uint64_t seq = 0;                      // 会话内单调, 重放缓冲用
    int64_t  startTimeMs = 0;              // StreamBegin / TurnStart / TurnEnd
    int64_t  durationMs  = 0;              // TurnEnd
    double   tps = 0.0;                    // TurnEnd
    Type       type = Type::TextToken;
    GenPhase   genPhase = GenPhase::Done;
    TipType    tipType  = TipType::Info;
    ViewMessage::Role streamRole = ViewMessage::Role::Assistant;  // StreamBegin
    std::optional<ViewMessage::ThinkData> think;                  // StreamBegin (加密思考)
};
```

`WireSyncPayload` 增加:

```c++
/// 进行中的流式消息 (直播槽): 完整/尾窗同步时携带, 供"中途接入"的客户端补上这半截内容
/// - 空 = 当前没有进行中的流
/// - 不在 messages 里, 也不计入 fromIndex/totalMessages/lastViewSeq
std::optional<ViewMessage> streaming;
```

JSON: `sync` 信封里多一个可选键 `"streaming": {ViewMessage}`; 消息级 op 的 `kind` 取
`stream_begin` / `text_token` / `thinking_token` / `insert_message` / `update_message` / `stream_end`。

### 4.3 语义细则 (客户端按此实现即可, 不需要别的约定)

1. **id 是一切**: 所有消息级 op 的 `msgId` 非空, 由 agent 分配 (`msg_000123`)。客户端自造的行用
   `local_` 前缀 (中断表单 `local_interrupt_<wireId>`、瞬态提示 `local_tip_<n>`), 永远不会与 agent 的
   id 冲突, 也永远不会被 agent 的 op 命中。
2. **insert/update 都是 upsert by id**: 未知 id 的 `insert` 追加到末尾; 已知 id 的 `insert` 等价整条
   替换 (重连重放与增量补拉会重复投递同一条, 必须幂等); 未知 id 的 `update` **忽略** (那条多半是本地
   条数上限清理掉的老消息, 用户上滚分页会重新拿回来)。
3. **`stream_end` 未知 id 视为 no-op**; 命中当前流式项时丢弃该流式项 (仅用于整段视觉为空的收尾)。
4. **token 只在流式项上追加**: 收到 `text_token`/`thinking_token` 时若当前流式项的 id 不同 → 按
   `StreamBegin` 规则重置 (防御: 说明 `stream_begin` 丢了, 客户端用 token 里的 id 自建一条)。
5. **`insert_message` 命中当前流式项的 id 时, 先丢弃流式项再落行** (定稿动作); 若因丢包先收到
   一个新的 `stream_begin` (id 不同), 同样先把旧尾丢弃再开新流。
6. **顺序**: 同一连接的 op 严格按 `seq` 递增投递; 客户端不需要重排, 只按 seq 做去重 (已有的
   `deltaSeq`/`lastSeq` 机制不变)。
7. **未知 kind / 未知 role**: 忽略该 op 并记一次 debug 日志 (向后扩展点)。

### 4.4 时序示例

**(a) 常规一轮 (正文 -> 工具 -> 正文)**

```
insert_message  msgId=msg_120 role=user        "把 README 的标题改成 X"
turn_start      startTimeMs=...
gen_status      phase=waiting
stream_begin    msgId=msg_121 role=assistant startTimeMs=...
text_token      msgId=msg_121 "好的"
text_token      msgId=msg_121 ", 我先看看"
insert_message  msgId=msg_121 role=assistant "好的, 我先看看"     <- 检查点 (历史行)
insert_message  msgId=msg_122 role=tool tool.toolName=read ...    <- 工具行 (未完成)
update_message  msgId=msg_122 tool.toolResult=... toolFinished=true durationMs=12
stream_begin    msgId=msg_123 role=assistant
text_token      msgId=msg_123 "改好了"
insert_message  msgId=msg_123 role=assistant "改好了"
turn_end        durationMs=... tps=...
```

**(b) 思考与正文交错 (一段一行, 不合并)**

```
stream_begin msgId=msg_131 role=thinking
thinking_token msgId=msg_131 "..."
insert_message msgId=msg_131 role=thinking "..."      <- 定稿成行
stream_begin msgId=msg_132 role=assistant
text_token   msgId=msg_132 "..."
insert_message msgId=msg_132 role=assistant "..."
stream_begin msgId=msg_133 role=thinking
thinking_token msgId=msg_133 "..."
insert_message msgId=msg_133 role=thinking "..."      <- 第二段思考 (独立一行)
```

**(c) 非流式 / 网关缓冲 (没有 token, 只有定稿)**

```
insert_message msgId=msg_140 role=assistant "一次性返回的整段内容"
```

**(d) 取消 / 出错 (半截内容照常定稿)**

```
stream_begin msgId=msg_150 role=assistant
text_token   msgId=msg_150 "已经输出的部分"
insert_message msgId=msg_150 role=assistant "已经输出的部分"   <- 取消/出错时定稿成行并落盘
insert_message msgId=msg_151 role=tip "已取消"
turn_end
```

**(e) 中途接入 (client 在流式中连上)**

```
sync {messages:[...已定稿的行...], totalMessages:N, lastViewSeq:S,
      streaming:{id:msg_160, role:assistant, text:"半截内容"}}
---- 之后 ----
text_token     msgId=msg_160 "继续"
insert_message msgId=msg_160 role=assistant "半截内容继续"
```

## 5. agent 侧实现

### 5.1 `Session` (agent/lib/{include/agentxx/agent/context.h, src/agent/context.cpp})

新增直播槽 API (均在 io 线程调用, `assertIoThread()`):

```c++
/// 进行中的流式消息 (直播槽)
/// - 不在 viewMessages 里: 不计链式哈希、不占持久化序号、不落盘、不参与分页
/// - 至多一条; 只用于把半截内容发给中途接入的客户端 (WireSyncPayload::streaming)
std::optional<ViewMessage> streamingView;

/// 开始一段流式消息: 分配 msgId (复用 msgIdCounter_, 不推进 viewSeqCounter_)
std::string beginStreamingView(ViewMessage skeleton);   // skeleton 带 role/startTimeMs/think
/// 往直播槽追加正文/思考文本
void appendStreamingText(std::string_view text);
/// 合并 think 元数据 (加密标记取或, reasoning_tokens 取大)
void mergeStreamingThink(const ViewMessage::ThinkData& data);
/// 定稿: 用预留 id 走正常 appendViewMessage (哈希/序号/落盘/分页照旧)
std::string finalizeStreamingView(ViewMessage finalMsg);
/// 放弃直播槽 (整段视觉为空): 不写历史, 发 stream_end 让客户端丢弃流式项
void clearStreamingView();
```

配套改动:

- `appendViewMessage(ViewMessage msg, std::string_view reuseId = {})`: 允许沿用预留 id
  (定稿路径必须复用直播槽的 id, 否则客户端那条流式项无法收敛到正式行)。
- `restore()` 开头清空 `streamingView` (进程重启后没有半截行)。
- `flushViewMessages()` / `persistNow()` 不碰直播槽 (它本来就没进待落盘队列)。

### 5.2 `EventBridge` (agent/lib/{include/agentxx/event/event_stream.h, src/event/event_stream.cpp})

这是本次改动的重心: 把"消息怎么成型"全部集中到这里。

新增状态:

```c++
enum class OpenStreamKind { None, Think, Assistant };
struct StepRow { ViewMessage::Role role; std::string msgId; };  // 本步骤内已定稿的流段行

OpenStreamKind openKind_ = OpenStreamKind::None;
std::string    openId_;              // = Session::streamingView->id
int64_t        openStartMs_ = 0;
std::vector<StepRow> stepRows_;      // NODE_START 时清空
```

新增方法:

- `void beginStream(ViewMessage::Role role, const std::optional<ViewMessage::ThinkData>& meta)`:
  - 若已有打开的流 → 先 `flushOpenStream()` (角色切换);
  - `id = session_->beginStreamingView(...)`; 记 `openId_/openKind_/openStartMs_`;
  - `emitDelta(StreamBegin{msgId=id, streamRole=role, startTimeMs, think=meta})`。
- `void appendStreamText(std::string_view text, bool thinking)`:
  - `session_->appendStreamingText(text)`; `emitDelta(TextToken/ThinkToken{msgId=openId_, text})`。
- `void mergeStreamThink(const ViewMessage::ThinkData&)` → `session_->mergeStreamingThink`。
- `void flushOpenStream()` (**public**, 段结束/角色切换/轮末都调):
  1. 取直播槽内容, 算 `durationMs = now - openStartMs_`;
  2. 组装 `ViewMessage` (`role`, `text`, `startTimeMs`, `durationMs`, `think`, Think 默认 `collapsed=true`);
  3. `hasVisibleContent()` 为真 → `finalizeStreamingView` + `emitDelta(InsertMessage{msg})`,
     并 `stepRows_.push_back({role, id})`;
  4. 为假 → `clearStreamingView()` + `emitDelta(StreamEnd{id})`;
  5. 清 `openKind_/openId_`。
- `void finishTurn()` (**public**): `flushOpenStream()` + `stepRows_.clear()`。
  取消/出错时同样走 `flushOpenStream()`: 已产出的半截内容照常定稿成行 (可见则进历史并落盘)。

改动到现有 handler:

| handler | 改动 |
|---|---|
| `operator()` (`NODE_START`) | `stepRows_.clear()`; `flushOpenStream()` (防御) |
| `handleLLMToken` | 删除 tps 之外的"段结算包"逻辑; 第一个 token 或角色切换时 `beginStream(...)`; 之后 `appendStreamText`; `think` 元数据走 `mergeStreamThink` (不再塞进 token delta) |
| `handleGenStatusChunk` | `finalizeThinkSegment()` 调用点保留语义 (think 段在工具调用前结束) 但改为 `flushOpenStream()` |
| `finalizeThinkSegment` | **保留**计时累计 (`lastThinkSegStartMs_/lastThinkSegDurationMs_`, 供定稿时填 think 行), **删除**"空文本 ThinkToken 结算包"的 emit |
| `handleChannelWrite` (assistant) | 开头 `flushOpenStream()` (含 `message_tip` 分支); reasoning/content 按 §5.2.1 对账; tool_calls 逐条 `appendViewMessage` + `emitDelta(InsertMessage)`; 保留 `toolCallHistoryIndex_` 加速定位 |
| `handleChannelWrite` (tool) | 回填行后 `updateViewMessage` + `emitDelta(UpdateMessage{row})` (取代 `ToolEnd`) |
| `handleNodeEnd` / `handleError` | 两者都 `flushOpenStream()` (取消/出错的半截内容也定稿成行); 删除 NodeEnd 的时长回填 delta 字段 |
| `handleTurnStart` | 只重置 tps |

#### 5.2.1 定稿对账规则 (assistant 的 `messages` 通道写入)

对 `role in {Think, Assistant}` 的最终字段分别处理, `segs` = 本步骤内该 role 的 `stepRows_`:

1. `segs` 为空 → 最终文本可见时 `appendViewMessage` 新行 + `InsertMessage` (覆盖"非流式/网关缓冲"
   与"只有定稿"两种情况)。
2. `segs.size() == 1` → 最终文本/元数据 (reasoning_tokens/encrypted)/耗时与该行不一致才
   `updateViewMessage` (整条覆盖, 同一 id) + `UpdateMessage`; 完全一致时不发 op (省带宽且幂等)。
3. `segs.size() > 1` (交错多段) → 只把元数据补到第一段, **文本保持分段原样** (与实时所见一致;
   与"上下文用定稿文本、显示用流式文本"这一现状同级, 不新增偏差)。日志记 debug。

`stepRows_` 里的行若在定稿时被判为不可见 (例如模型只吐了 U+200B), 按"不追加、发 `stream_end`"处理,
保证历史里不出现看不见的行 (现有 `fix-tui-blank-message` 的结论继续生效)。

### 5.3 `BaseAgent` (agent/lib/src/agent/base_agent.cpp)

- 用户行: 先 `appendViewMessage` → `emitDelta(InsertMessage{userRow})`, 再 `emitDelta(TurnStart{})`。
  - 顺序固定为"先整条, 后标记": 插件通道 (`PluginChannelAgentIO`) 需要从这条行里取 `TURN_START.text`;
  - 下发副本时清空 `attachments[].dataUrl` (沿用现有"防大包"约定), 完整 dataUrl 留在历史里,
    需要时由 `GetViewMessages` 拉取;
  - 删除 `TurnStart` 上的 text/attachments/source 三个字段的填充。
- 轮末: `eventBridge->finishTurn()` 必须在 `session->persistNow("turn-end")` **之前**, 保证
  `streamingView` 不残留到下一轮。取消/出错的半截内容同样定稿成行并落盘。
- `insertMessageTip` 保持不变 (已经是"行 + InsertMessage")。

### 5.4 `SessionServerAgentIO` (agent/lib/src/agent/io/session_server_agent_io.cpp)

- `buildFullSync` / `buildTailSync`: `p.streaming = sess->streamingView` (`totalMessages`/`fromIndex`
  仍然只统计已定稿行)。
- `sendToPeer`: 大消息 op 不入重放缓冲 —— `message` 序列化后超过 `kDeltaBufferMaxMsgBytes`
  (建议 64 KiB) 时不入 `deltaBuffer_`, 只广播; 断线重连靠 `afterViewSeq` 增量补拉补齐
  (已有语义, 且 upsert 幂等)。
- `sendErrorTip` / 心跳 / 上下文统计: 不变。
- 删除 `WireDelta` 上 `tailHash`/`historyCount` 之后, 这里的相关判断若存在一并清理。

### 5.5 其它写入点 (逐个确认, 都属于"已经正确"的形态)

| 位置 | 现状 | 本次改动 |
|---|---|---|
| `nodes/session_context.cpp::appendUserViewMessage` | 行 + `InsertMessage` | 无需改 |
| `middlewares/summarization.cpp` | 行 + `Insert/UpdateMessage` | 无需改 (正好是 `UpdateMessage` 幂等的现成用例) |
| `session_server_agent_io.cpp::sendErrorTip` | 行 + `InsertMessage` | 无需改 |
| `BaseAgent::insertMessageTip` | 行 + `InsertMessage` | 无需改 |
| `context.cpp` 的持久化失败提示 | `MessageUITip` (瞬态) | 无需改 |

## 6. client 侧实现

### 6.1 `ViewReplica` (新增: agent/lib/include/agentxx/agent/io/view_replica.h + src/agent/io/view_replica.cpp)

放在 lib 而不是 client, 因为 stdio (client) 与 FFI/插件通道 (lib 内的嵌入式端点) 都要用; lib 内不反向
依赖 client 头, 不违反 `boundaries` 规则 3。

```c++
namespace agentxx::agent {

/// 尾部流式项 (agent 端 Session::streamingView / stream_begin 的客户端副本)
struct ViewStreamTail {
    std::string                id;
    ViewMessage::Role          role = ViewMessage::Role::Assistant;
    std::shared_ptr<std::string> text;   // COW: 每 token 只在 UI 持有快照时复制本体
    std::optional<ViewMessage::ThinkData> think;
};

class ViewReplica {
public:
    /// 应用一个消息级 op; 返回是否改动了消息集合 (调用方据此决定重绘)
    bool applyDelta(const WireDelta& delta);
    /// 整体替换 (完整/尾窗同步); 保留本地行
    void reset(const WireSyncPayload& payload);
    /// 增量补拉 (按 id 去重后追加)
    bool appendIncremental(const WireSyncPayload& payload);
    /// 历史分页前插
    bool prependPage(const WireViewMessagesPage& page);
    /// 本地条数上限清理: 丢弃头部 n 条 (调用方已确认可安全丢弃)
    size_t trimHead(size_t count);
    /// 本地行 (中断表单 / 瞬态提示): id 必须 `local_` 前缀
    void upsertLocalRow(ViewMessage row);
    /// 灌入同步里的直播槽 (中途接入 / 重连): 等价于一次 stream_begin + 若干 token
    void seedStream(const ViewMessage& streaming);

    const std::vector<std::shared_ptr<ViewMessage>>& rows() const;
    std::shared_ptr<ViewMessage> rowById(std::string_view id) const;
    const ViewStreamTail& stream() const;   // id 为空 = 没有进行中的流
    bool hasStream() const;
    uint64_t lastViewSeq() const;
};
```

实现要点:

- `rows`/`stream` 的存储形态直接对齐 TUI 现有快照需求 (`shared_ptr`), 于是 TUI 的"整份状态 COW"
  代价不变 (每 op 一次 N 个指针拷贝, 每 token 只动流式字符串)。
- `applyDelta` 只处理 6 个消息级 op; 其余类型返回 false (界面状态由端点自己处理)。
- `reset` 保留界面类本地行 (`local_interrupt_*` / `local_tip_*`) 并按原顺序追加在末尾
  (重连不会丢掉正在显示的中断表单)。
- 空行过滤 (防御): `insert/update` 进来但 `hasVisibleContent()` 为假时直接忽略 (正常不会发生)。

### 6.2 TUI (agent/client/src/io/tui/agent_tui.cpp, framework/tui_state.h, components/message_list.cpp)

`TUIRenderState` 变化:

| 删除 | 新增/替换 |
|---|---|
| `messages` (vector<shared_ptr<ViewMessage>>) | `ViewReplica replica` (rows 从它取) |
| `currentToken` / `currentTokenRole` / `currentTokenEpoch` / `pendingToken*` | `replica.stream()` |

折叠状态**不做额外扩展**, 与今天一样只存在客户端: 消息行的 `msg.collapsed` 仍然由点击直接翻转,
流式项仍然用 `streamThinkOverride` 这一个字段, 定稿行到达时按它写一次 `collapsed`
(等价于今天“提交时写入”)。整份 sync / 整条覆盖会把选择恢复成 agent 给的默认值 —— 与今天一致。

`TUIClientAgentIO::onDelta` 收敛为:

```c++
switch (delta.type) {
    case TextToken / ThinkToken / InsertMessage / UpdateMessage / StreamEnd:
        changed = sharedState_.mutate([&](st){ return st.replica.applyDelta(delta); });
        break;
    case TurnStart:  st.isStreaming = true;  // + 吸附底部
    case TurnEnd:    st.isStreaming = false;
    case GenStatus:  st.genPhase / st.genToolName
    case NodeStart/NodeEnd: st.currentNodeName
    case MessageUITip: st.replica.upsertLocalRow(tip row)   // 本地瞬态提示
}
```

删除 (见 §8.2): `pushCurrentTokenLocked`、`findStepMessageLocked`、`resetTrailingRunningToolsLocked`、
`mergeThinkData`、`ToolEnd` 回扫、`NodeEnd` 时长回填、`isBlankContentMessage` (降级为副本层的防御过滤)、
`pushCurrentTokenLocked` 的十余处调用点。

`MessageListComponent`:

- 减少波及面的做法: `TUIRenderState` 保留一个 `messages()` 只读访问器 (返回 `replica.rows()`),
  其余组件与测试的读法不用改, 只有写方 (onDelta/onSync/trim) 改成操作副本;
- 流式项位置: 恒在列表末尾 (`replica.rows().size()`), 因此 `fillViewport`/`itemCount` 的"流式子项"
  逻辑保持原结构, 只把数据源换成 `replica.stream()`;
- 流式增量渲染器 (`streamRenderer_` / `streamFedLen_` / `streamEpoch_`) 保留, 流身份改用
  `stream().id` 而非 epoch (`id` 天然唯一且随段变化);
- 折叠: 点击仍然直接翻转行上的 `msg.collapsed` (与今天一致); 流式项用 `streamThinkOverride`,
  `insert_message` 落定这条流 (id 等于当前 `stream().id`) 时把该值写到行上 (等价于今天的“提交时写入”);
  不引入覆盖表/LRU 等新结构。`update_message` 整条替换会恢复默认折叠态 (与今天一致);
- 中断表单行: `local_interrupt_<wireId>`, 与今天行为一致 (仍然只在客户端存在, 因为交互通道在客户端);
- 本地条数上限 (`kLocalMessageLimit`) 清理: 调 `replica.trimHead(n)` + 现有的 `noteTrimmedHead` /
  `onHistoryTrimmed` 不动。

`onSync`:

- 整体替换 → `replica.reset(payload)`;
- `payload.streaming` 非空时把流式项种子灌进 `replica` (`seedStream`), 并置 `isStreaming = true`;
- 其余 (队列镜像 / 模型能力 / 连接状态 / 启动进度) 保持现有"不随 Sync 重置"的白名单逻辑。

`onViewMessagesPage` / `onIncrementalSync`: 改为调 `replica.prependPage` / `replica.appendIncremental`,
自身只保留滚动锚定、历史窗口记账与告警日志。

### 6.3 stdio (agent/client/src/io/stdio/agent_stdio.cpp)

stdio 不做消息列表, 只按"打印一次"规则处理:

- `insert_message`: 若该 id 已经有流式内容打印过 → 只补打"未打印的尾部" (按 `printedLen` 前缀比较),
  否则整段打印 (带角色前缀);
- `text_token` / `thinking_token`: 实时打印 (沿用现有 `[Content]` / `[Think]` 前缀切换), 记 `printedLen`;
- `update_message`: 角色为 Tool 时打印 `[Tool] name done/error/interrupted`; 其它角色只在文本前缀不
  匹配时补打差异, 匹配则静默;
- `stream_end`: 无操作 (流式项丢弃);
- `turn_start` / `turn_end` / `gen_status` / `node_*`: 沿用现有输出 (提示符等);
- 删除 `ToolStart`/`ToolEnd` 分支。

### 6.4 FFI / `EmbeddedClientAgentIO` (agent/lib/src/{agent/io/embedded_client_agent_io.cpp, ffi/ffi_client_io.cpp})

- 合批 (`deltaBatchMs_ > 0`) 的 key 从 `kind` 改为 `(msgId, kind)`, 批次载荷增加 `msgId`
  (`AGENTXX_FFI_EVT_DELTA_TEXT_BATCH` 的 JSON: `{"msgId","kind","text"}`), 不允许跨 id 合并;
- `AGENTXX_FFI_EVT_DELTA` 继续透传 wire delta JSON (新 op 自动可见);
- `AGENTXX_FFI_EVT_SYNC` 的 JSON 自动带上 `streaming`;
- `ffi_api.h`: `AGENTXX_FFI_API_VERSION` 2 → 3, 并在注释里列清 delta `kind` 与消息级 op 语义
  (宿主按 id 应用, 或干脆只用 `wire_passthrough` 的 `get_view_messages` 拉取);
- `ffi_runtime.cpp` 的能力清单加上 `AGENTXX_FFI_CAP_VIEW_REPLICA` ("deltas carry message ids; hosts
  may apply ops or pull") —— 便于宿主判断对端行为。

### 6.5 插件通道 (`PluginChannelAgentIO`, agent/lib/src/plugins/plugin_channel_agent_io.cpp)

- `TURN_START.text` / `payload.attachments`: 取自 `turn_start` **之前**那条 `insert_message`
  (role=User/Event) 的行内容 → 插件侧事件载荷字段保持不变;
- `TOOL_START` / `TOOL_END`: 由 Tool 行的 `InsertMessage` / `toolFinished=true` 的 `UpdateMessage` 派生,
  载荷字段 (toolName/toolCallId/arguments/result/status) 从行里取, 另加 `msgId`;
- `DELTA_TEXT` / `DELTA_THINKING`: 来自 token, 另加 `msgId`;
- `InsertMessage`/`UpdateMessage` 不再落入 `default` 分支静默丢弃 (从 Tool 行派生事件);
  `StreamEnd` 无事件可派生 (忽略即可)。

### 6.6 jsonl (`agent/client/src/io/jsonl/jsonl_mode.cpp`)

它跑的是 `SessionServerAgentIO` + JSONL 传输 (服务端视角), 不组装消息; wire JSON 变化自动生效。
只需更新 `jsonl_mode` / `jsonl_runner` 两个测试模块里对 `kind` 的断言。

## 7. 边界与专项

### 7.1 可见性 / 空白

- 判据唯一实现在 `ViewMessage::hasVisibleContent()` (已覆盖零宽字符, 见 `fix-tui-blank-message`)。
- 只在**定稿**处判一次 (agent): 不可见 → 不进历史 + `stream_end`。客户端保留一次防御性过滤,
  但不再用它决定"某段 token 要不要显示"。

### 7.2 折叠

- agent 只决定默认值 (`collapsed`: Think/Tool/Tip/Event 为 true), 不参与运行时的展开/折叠。
- 用户点击直接翻转客户端行上的 `msg.collapsed` (与今天一致), **不上报、不落库**;
  流式项沿用 `streamThinkOverride`, 定稿行到达时写一次。
- 因此整条覆盖 (`update_message`) / 整份 `sync` 会恢复成默认折叠态 —— 与今天完全一样, 本轮不额外扩展。

### 7.3 中断表单 / 本地行

- 中断表单行仍由客户端创建 (`local_interrupt_<wireId>`), 因为交互通道 (提交/取消) 在客户端;
  agent 侧只有"中断头"提示行, 顺序仍是: 中断头 `insert_message` → `WireInterruptRequest` → 客户端插表单行。
- `reset()` (完整/尾窗同步) 保留 `local_` 行, 重连时不会丢掉正在等待用户操作的表单。
- `WireInterruptExpired` / 提交后的状态更新: 沿用客户端本地改写 (只影响本地行)。

### 7.4 瞬态提示

- `message_tip` (模型重试提示 / 插件 `emit_message_tip`) 仍不落历史: 客户端造 `local_tip_<n>` 行。
- agent 端保证"发提示前没有打开的流段": `handleChannelWrite` 任何分支进入时先 `flushOpenStream()`
  (重试提示来自 `messages`/`message_tip` 通道写入, 天然满足)。

### 7.5 多客户端 / 中途接入 / 重连

- 直播槽只有一份 (在 agent), 因此所有客户端看到的半截内容完全一致 (修掉"各拼各的")。
- 中途接入 (新连接 / 切会话): 全量或尾窗 `sync` 携带 `streaming`, 客户端种入流式项后继续追加。
- 重连 (带 `afterViewSeq`): 若缓冲未覆盖该段的 token 也不丢内容 —— 定稿时的整条 `insert_message` 会
  幂等补上; `sync.streaming` 只服务于全量/尾窗同步, 不参与增量补拉。
- 重连: `Hello{lastSeq, afterViewSeq}` 两条路一起走 ——
  - delta 重放 (缓冲内) 带消息级 op, 幂等 upsert;
  - `afterViewSeq` 增量补拉 (库里已定稿行), 同样幂等。
- 客户端本地清理过头部消息时, 未知 id 的 `update` 忽略 (不把它追加到错位置), 用户上滚分页会重新拉回。

### 7.6 headless / 无对端

- `EventBridge::beginStream` 只在 `session_->io != nullptr` 时建槽 (与 `insertMessageTip` 同一判据):
  子代理 / 后台任务等无人观看的会话不做逐 token 累积, 也不发 `stream_begin`/token op;
- 这类会话的定稿路径不变 (channel write 照常写历史), 行为与今天一致。

### 7.7 取消与异常

- 取消/出错: `finishTurn()` 走 `flushOpenStream()` —— 已产出的半截内容**照常定稿成行并落盘**,
  客户端就像普通消息一样显示它; 与现有实现的观感一致 (取消后半截留在界面上), 只是它也进了展示历史。
- 这使展示历史比 LLM 上下文多一条 (上下文里没有这条未说完的 assistant 消息): 有意为之 ——
  用户确实看到过, 重新打开会话也应看到。在 `docs` 里写明。
- 重试场景 (`message_tip` → 新 `stream_begin`): 失败尝试的文本同样已定稿成行, 与新一段并存 (与今天一致)。

### 7.8 持久化与崩溃窗口

- 直播槽不落盘; 定稿才进 `PendingViewOp` (append 一次, 内容即最终文本) → 崩溃最多丢“当前这一段”。
- 取消/出错时同样先定稿再 `persistNow("turn-end")`, 因此重启后这段半截内容仍在 (与“要保存”一致);
  与今天“取消后重开就没了”相比是变好的差异, 需在文档里写明。
- 定稿后的 `UpdateMessage` (工具结果 / 整条对账) 仍走 `updateViewMessage` 覆盖库内行。

### 7.9 性能预算

| 路径 | 今天 | 重构后 |
|---|---|---|
| 每 token (agent) | 组装 `WireDelta` + JSON 序列化 | 同上 + 一次 `std::string::append` 到直播槽 |
| 每 token (client) | append 到 `currentToken` (COW) | 同上 (append 到 `ViewStreamTail::text`) |
| 每段定稿 | 无 (只有 channel write 时一次性建行) | 一次整条序列化 + 一次 `InsertMessage` |
| 每工具调用 | `ToolStart` + `ToolEnd` (result 一次) | `InsertMessage` (arguments) + `UpdateMessage` (arguments+result) |
| 历史行状态 | client 侧靠启发式 | client 侧纯 upsert |

- 工具结果整条下发会让单条 op 变大 (重复一次 `arguments`)。基准里若发现明显带宽/内存问题, 备选方案是
  给工具行保留一个"字段补丁" op; 本轮先按统一形态做 (工具调用频率低, 且 delta 缓冲对大 op 不入队)。
- `deltaBuffer_` 增加字节上限 (`kDeltaBufferMaxMsgBytes`, 建议 64 KiB) 防止大消息占满重放缓冲。

## 8. 版本与删除清单

### 8.1 版本

| 项 | 改前 | 改后 | 位置 |
|---|---|---|---|
| wire `kVersion` | 3 | **4** | `agent_io_transport.h` |
| wire `kMinVersion` | 3 | **4** | 同上 (旧对端直接拒绝握手) |
| FFI `AGENTXX_FFI_API_VERSION` | 2 | **3** | `ffi_api.h` |
| 插件通道事件 | — | 载荷加 `msgId`, 语义改为从行派生 | `plugin_channel_agent_io.cpp` + `plugins.md` §19 |

不新增能力位 (capabilities): 双端必须同版本, 无需降级分支。

### 8.2 删除清单

**协议 / 结构**

- `WireDelta::Type::ToolStart`、`ToolEnd` (及其 JSON 映射与解析)。
- `WireDelta` 字段: `arguments`、`result`、`status`、`toolCallId`、`attachments`、`source`、
  `historyCount`、`tailHash` (`think` 保留, 但只由 `stream_begin` 携带)。
- `WireDelta::TurnStart` 的 text/msgId/attachments/source 填充 (语义降级为标记)。
- `WireSyncPayload` 其余不动, 只新增 `streaming`。

**agent 侧**

- `EventBridge::finalizeThinkSegment` 里的"空文本 ThinkToken 结算包" emit (保留计时累计)。
- `EventBridge::handleNodeEnd` 里对消息时长的回填字段。
- `BaseAgent::runTurnAsync` 里 `TurnStart` 的 text/attachments/source 填充。

**client 侧 (TUI)**

- `TUIClientAgentIO::pushCurrentTokenLocked`、`findStepMessageLocked`、`resetTrailingRunningToolsLocked`、
  `mergeThinkData`、`isBlankContentMessage` (降级为副本内部防御)。
- `onDelta` 里: ToolStart/ToolEnd 分支、NodeEnd 时长回填分支、TurnStart 造行分支、Insert/Update 各自的
  手工定位逻辑。
- `TUIRenderState`: `currentToken`、`currentTokenRole`、`currentTokenEpoch`、`pendingTokenStartTimeMs`、
  `pendingTokenDurationMs`、`pendingTokenThink` (折叠的 `streamThinkOverride` **保留**, 不删)。
- `MessageListComponent`: 无删除项 (折叠点击仍写 `msg.collapsed`, `streamThinkCollapsed` /
  `clearStreamThinkOverride` 保留); 只把渲染数据源改到 `replica`。

**其它端点**

- `StdIOClientAgentIO::onDelta` 的 ToolStart/ToolEnd 分支。
- `PluginChannelAgentIO::onDelta` 的 ToolStart/ToolEnd 分支 (改派生)。
- `EmbeddedClientAgentIO` 合批的 `kind`-only 键。

**测试**

- 所有把 `ToolStart`/`ToolEnd`/`TurnStart.text`/`currentToken` 当契约的断言 (见 §10)。

## 9. 实施步骤

每一步都能单独编译并跑通相关测试模块, 出问题可回退一步。

| 步 | 内容 | 主要文件 | 验证 |
|---|---|---|---|
| S1 | 协议与模型: delta 类型增删 + 字段瘦身 + `WireSyncPayload.streaming` + 版本抬升 + `ViewReplica` 实现与单测 (含 `agent/test/test.cpp` 注册与 `test/include/agentxx-test/core/test_view_replica.h`) | `conversation_types.h`、`wire_protocol.h`、`agent_io_transport.h`、`view_replica.{h,cpp}`、`agent/test/core/test_view_replica.cpp`、`agent/test/test.cpp` | `agentxx_test view_replica wire_schema wire_roundtrip` |
| S2 | `Session` 直播槽 + `appendViewMessage(reuseId)` | `context.h`、`context.cpp` | `agentxx_test session_schema session_persistence` |
| S3 | `EventBridge` 分段/定稿/对账 + `BaseAgent` 时序 + `SessionServerAgentIO` (sync.streaming / 大 op 不入缓冲) | `event_stream.{h,cpp}`、`base_agent.cpp`、`session_server_agent_io.cpp` | `agentxx_test event_bridge session_persistence remote_agent` |
| S4 | TUI: `TUIRenderState` 换 `ViewReplica` + `onDelta` 收敛 + `MessageListComponent` 流式/折叠 | `agent_tui.{h,cpp}`、`tui_state.h`、`message_list.cpp` | `agentxx_test tui_stream tui_scroll tui_widget tui_tool_header tui_form tui_interrupt` |
| S5 | stdio + FFI/嵌入式 + 插件通道 + jsonl 断言 | `agent_stdio.cpp`、`embedded_client_agent_io.cpp`、`ffi_client_io.cpp`、`ffi_api.h`、`plugin_channel_agent_io.cpp` | `agentxx_test ffi_c_api plugin_agent_runtime plugin_example_runtime jsonl_mode jsonl_runner` |
| S6 | 文档与 AGENTS.md 同步 (index.md / tui.md / plugins.md / en) | 见 §11 | `agentxx_test boundaries config_keys` |
| S7 | 全量回归 + 基准抽查 (TUI 帧耗时 / 每 token 开销 / 内存) | — | `agentxx_test` 全量 + `agentxx_benchmark` |

## 10. 测试计划

| 模块 | 用例 (新增/改造) |
|---|---|
| `view_replica` (新) | insert 幂等 / update 未知 id 忽略 / stream_end 丢弃流式项 / token 追加与 `stream_begin` 自愈 / `reset` 保留 `local_` 界面行 / `streaming` 种子 / trimHead / appendIncremental 按 id 去重 |
| `event_bridge` | 每段一行且 id 稳定; 定稿整条内容 == 流式拼接; think 元数据与耗时进定稿行; 交错多段 → 多行; 空白段 → `stream_end` 且不写历史; 非流式 provider → `InsertMessage` 一次; 工具行 id + 结果 `UpdateMessage`; 取消/出错 → 半截内容定稿成行; `message_tip` 前无打开流 |
| `session_schema` | `streamingView` 不参与哈希/序号/落盘; 定稿复用 id; `restore` 清空直播槽 |
| `session_persistence` | 崩溃窗口内直播槽不落盘; 定稿后可读回; 取消/出错的半截行重启后仍在 |
| `wire_schema` / `wire_roundtrip` | 新 delta 类型与字段 round-trip; `sync.streaming` round-trip; 版本 4 拒绝 3 |
| `remote_agent` | 双端 WS: 常规轮 / 中途接入 (streaming) / 断线重连去重 / 大消息不入缓冲后靠增量补拉 |
| `tui_stream` | 端到端 op 流 → 行数/文本/耗时/折叠; 未知 id update 忽略; tail 与定稿行替换无重影; 重放重复投递不产生重复行 |
| `tui_scroll` / `tui_widget` / `tui_tool_header` / `tui_form` / `tui_interrupt` | 折叠行为不变 (点击写 `msg.collapsed`, 定稿后仍展开); 工具头部来自行字段; 中断表单行在 sync 后保留; 本地清理后 update 忽略 |
| `stdio` (无独立模块, 走 `jsonl_mode`/手工) | 打印一次语义 (前缀补打) |
| `ffi_c_api` | `DELTA_TEXT_BATCH` 带 msgId 且不跨 id 合并; `EVT_SYNC` 带 streaming; API 版本 3 |
| `plugin_agent_runtime` / `plugin_example_runtime` | `TURN_START.text` 仍为用户文本; `TOOL_START/TOOL_END` 从行派生且字段齐全 |
| `jsonl_mode` / `jsonl_runner` | `kind` 断言更新; 输出里能看到消息级 op |
| `boundaries` / `config_keys` | 分层与文档引用检查 (无新增配置键) |

回归要求: 全量 `agentxx_test` 通过 (含 `--log-level warn` 输出无新增警告)。

## 11. 文档更新

- `docs/zh-cn/design/index.md`
  - "流式输出" 条目重写: 列出 12 个 delta、**agent 唯一写者**、`streamingView`、客户端副本;
  - "核心对话能力" 里补一句"展示历史只有 agent 生成, 客户端只做副本"。
- `docs/zh-cn/design/tui.md` §2 流式渲染: 数据源改为 `ViewReplica` / `ViewStreamTail`;
  折叠仍为客户端本地行为 (措辞不变); 删除 `currentToken` 相关描述。
- `docs/zh-cn/design/plugins.md` §19: 插件通道事件 `msgId`, TOOL_* 从行派生, TURN_START 取用户行。
- `docs/zh-cn/design/events.md`: 如提到 `MessageUITip`/轮次事件的载荷, 同步措辞 (不改语义)。
- `docs/en/design/index.md`: 同步对应条目。
- `AGENTS.md`: 在"整体设计"处加一条规则 (展示历史单写者 + 版本号), 并更新"流式输出"相关描述。
- 本文档 `index.md` / `work.md` 收尾。

## 12. 风险与备选

| 风险 | 影响 | 应对 |
|---|---|---|
| 工具结果整条下发导致 op 变大 | 带宽/内存 | 大 op 不入重放缓冲; 必要时后续加"字段补丁" op (不影响本次语义) |
| TUI 流式渲染器改动 (epoch → id) | 首 token 丢字/重影 | 保留"流身份变化即重置 fedLen"的不变式, 新增用例覆盖 sync 后接流 |
| 交错多段与定稿文本不一致 | 展示与上下文有差异 | 与今天同类 (今天也是"显示用 token、上下文用定稿"); 只在多段时不做整条覆盖, 记 debug 日志 |
| 折叠 | 与今天完全一致 | 无 (本轮不做覆盖表); `update_message`/`sync` 会恢复默认折叠态, 与今天同 |
| 取消/出错时半截内容进历史 | 展示比上下文多一条 | 有意为之 (与现有实现观感一致); 文档写明 |
| FFI 宿主需要适配新 op | 外部实现成本 | 版本抬升 + `capabilities` 显式声明 + `get_view_messages` 拉取兜底 |

备选方案 (若想进一步缩小改动面): 只做 S1~S3 + TUI 的"按 id upsert", 保留客户端本地拼装作为降级路径
(capability 分流)。本轮**不采用**: 用户已确认一步到位、不留兼容。

## 13. 验收标准

1. `agent/client` 侧不存在任何"决定消息边界/合并/耗时回填"的代码 (grep `pushCurrentTokenLocked`、
   `findStepMessageLocked`、`mergeThinkData`、`ToolStart`、`ToolEnd` 均无残留)。
2. 一条流式消息从开始到定稿, wire 上的 id 与最终历史行 id 一致; 中途接入的客户端看到同一段文本。
3. 非流式 provider 的回复在当轮即可见 (无需 Sync)。
4. 断线重连 (含缓冲溢出回退) 不产生重复行/丢失行。
5. 协议 v4 与旧客户端互连被明确拒绝 (错误信息可读), FFI API 版本为 3。
6. 取消/出错: 半截内容定稿成行、落盘、客户端照常显示, 重启后仍在; 与 LLM 上下文的一条差额已文档化。
7. 全量 `agentxx_test` 通过; TUI 帧耗时与每 token CPU 与重构前相比不劣化 (基准抽查)。

