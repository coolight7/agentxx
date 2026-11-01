# 工具结果状态重构 (toolcall-result) 设计计划

> 目标一句话: **工具的成败只由返回值里的状态位决定, 工具结果文本只给模型读; 执行层是唯一
> 的"状态 -> 展示文本"格式化处, 全项目不再有任何代码为了判断成败去看文本内容。**

涉及面: lib 工具接口 / 执行层 / Wire 协议 / 客户端渲染 / 插件 C ABI 与 SDK / JS 插件 /
内置插件 / 测试 / 文档。本次**提升插件 API 契约代次与 Wire 协议版本, 不与旧客户端、旧服务端、
旧插件兼容**; 从这次起, 后续版本继续按"追加字段、未知值按默认处理"的规则保持兼容。

---

## 1. 现状核对 (逐层, 基于当前代码)

### 1.1 代码里同时存在两套相反的约定

- 文档约定 (`AGENTS.md` 工具错误约定): 参数检查失败抛 `std::invalid_argument`、运行期错误抛
  `std::runtime_error`, 由 `ToolcallWrapNode` 统一格式化成 `[Error: <msg>]`; 插件侧在 SDK
  边界捕获异常上报 `PLUGINXX_OPERATOR_FAILED`。
- 同一条约定的末尾又写着失败标记还包含"上述失败文本", 于是
  `ToolcallWrapNode` 里有一行 `toolResultTextIsFailure(tool_msg.content)`
  (`agent/lib/src/nodes/toolcall.cpp` 约 1233 行): **返回字符串不再等于成功**。
- 结论: 需要统一到"抛异常 = 失败 / 返回状态 = 结果"这一套, 并把文本从判定里彻底摘掉。

### 1.2 各层现状

| 层 | 现状的错误通道 | 问题 |
|---|---|---|
| lib 工具接口 `XXToolBase::execute_async(const Json&)` (`tools/tool.h`) | 只有 `asio::awaitable<std::string>`; 失败只能抛异常 | 返回值没有状态位; "预期内的失败"(无匹配/路径不存在/表达式语法错)只有写成文本这一条路 |
| 执行层 `ToolcallWrapNode::runPreparedToolCall` | ① 异常 -> `[Error: …]` + `extra["hasError"]`; ② 未执行即定案(工具不存在/权限拒绝/重复调用被拒/中断占位) -> 文本 + 显式 `hasError`; ③ 文本前缀兜底 | ③ 是唯一依赖文本的地方 |
| 内置 lib 工具 | 基本都抛异常; `GitWorktreeTool` 返回 `[Error] … failed (exit=…)` 文本 (`tools/worktree.h:147`) | 后者靠前缀才被认成失败 |
| 插件工具 `PluginTool::execute_async` | `PluginxxOperatorNotify.done(status, payload)`: `OK`/`FAILED`/`CANCELLED` 状态位本来就有; `pluginxx::awaitPluginOp` 把 `FAILED` 抛成 `runtime_error`, `CANCELLED` 抛成 `CancelledException` | 状态被转成了异常文本; **超时分支直接 `return "[Plugin tool timeout]"`**(`plugin_manager_adapters.cpp:136`) 绕过状态位 |
| 插件 SDK `fast_tool` / `blocking_tool` / `polled_tool` (`plugin_kit.h`) | 工具实现体的签名固定返回 `std::string`, 只有抛异常才算 `FAILED` | 插件作者**没有"返回失败"的写法** —— 这就是 `filesystem_impl.h` 23 处、`math_impl.h` 4 处、`execute_command_impl.h` 3 处 `[Error: …]` 文本的来源 |
| JS 插件 (`agentxx_javascript_engine`) | `ToolExecReq` 的 `failed` / `cancelled` 字段 -> `FAILED` / `CANCELLED` | 同样只能靠抛/reject, 返回值只能表达成功 |
| MCP 工具 `McpClientTool` | MCP 协议返回里有 `isError`, 代码**完全忽略** | 现存缺陷: MCP 工具报错在界面上是成功 |
| host tool (FFI / WS 动态工具) | `WireHostToolResult{ok, resultJson, errorMessage}` -> host 侧抛 `runtime_error(errorMessage)` | 通道已存在, 同样是"状态 -> 异常" |
| 未包装的原生 `neograph::Tool` | 只有 `awaitable<std::string>` | 没有状态通道, 一律当成功 |
| 界面 `isToolResultError` (`message_list.cpp:180`) | `ToolData::hasError` **外加** `toolResult.starts_with("[Interrupt]")` | 客户端残留文本判定; "中断/取消"没有独立状态 |
| Wire | `WireDelta::hasError` (ToolEnd) | 只有布尔, 没有"取消/中断"之分 |
| LLM 上下文 `ChatMessage::extra` | `extra["hasError"]` 布尔 | 同上, 且被 `event_stream.cpp` 转成展示字段与工具结束事件 |

### 1.3 需要顺带修掉的既有缺陷

1. MCP 工具 `isError` 被忽略 (上表)。
2. 插件工具超时靠结果文本前缀表达 (上表)。
3. `agentxx_execute_command` 的超时/取消结果以 `[ExitCode]` 开头 (结构化块), 首行不是
   `[Error`, 因此**今天被当成成功**; 而它的块里其实带着 `[Error]` 段。
4. 客户端 `[Interrupt]` 文本判定, 以及"取消"没有独立状态。
5. 关键指标 `KeyMetrics::noteToolCall(Ok)` 在失败文本场景下也记 `Ok`, 统计不准。
6. 客户端与服务端握手只拒绝"对端版本更高", 不拒绝"对端过老" (`WireProtocol::kMinVersion`
   定义了但没人用)。
7. 拼写两套: 占位文本写 `[User canceled]` (一个 l), 代码里的异常名与宏写
   `CancelledException` / `PLUGINXX_OPERATOR_CANCELLED` (两个 l), 注释与文档两种都出现。

---

## 2. 目标契约

### 2.1 状态类型 (新增 `agent/lib/include/agentxx/util/tool_result.h`)

只依赖 `<string>` / `<string_view>`, 主程序与插件都能包含 (插件构建已包含
`agent/lib/include`, 见各插件 `CMakeLists.txt` 的 `AGENTXX_LIB_INCLUDE_DIR`)。

```c++
namespace agentxx {
namespace util {

/// 一次工具调用的终态
enum class ToolStatus {
    Ok = 0,
    Failed,       ///< 调用失败 (参数错/执行错/权限拒绝/超时/工具不存在 ...)
    Interrupted,  ///< 触发了中断 (等待用户确认等), 结果由中断恢复后续补
    Canceled,     ///< 被取消 (会话取消/客户端断开; 拼写统一用 Canceled)
};

/// wire / JSON 上的取值 (小写); 未知取值按 Ok 处理 (向前兼容)
std::string_view toolStatusName(ToolStatus status);
ToolStatus       toolStatusFromName(std::string_view name);

/// 工具执行结果
struct ToolResult {
    /// 给模型读的正文 (成功时的结果文本; 失败时的**错误内容**, 不带 `[Error: ]` 这类包裹)
    std::string text;
    ToolStatus  status = ToolStatus::Ok;

    static ToolResult ok(std::string text);
    static ToolResult fail(std::string errorText);
    static ToolResult interrupted(std::string text = {});
    static ToolResult canceled(std::string text = {});
};

} // namespace util
} // namespace agentxx
```

规则:

- 失败的 `text` **直接写错误内容**, 例如 `path does not exist`、`timed out after 60
  seconds`、`regex compilation failed`。不加 `[Error]` / `[Error: ]` / `[Plugin tool
  timeout]` 之类前缀, 也不写"plugin / 插件"这种模型不需要知道的对象名。
- 多行错误内容允许 (例如 `agentxx_execute_command` 的结构化块), 由执行层决定如何排版。
- 批量操作里"部分条目失败"**不算**整次调用失败: 结果保持 `ok`, 文本里逐条说明。
  这与今天只看首行的行为一致, 不改变语义。

### 2.2 状态来源唯一映射 (写在执行层, 一份)

| 来源 | 状态 | 结果文本 |
|---|---|---|
| 工具返回 `ToolResult` | 用它的 `status` | `result.text` |
| 抛 `std::invalid_argument` / `std::runtime_error` / 其他普通异常 | `Failed` | `formatFailureText(e.what())` |
| 抛 `neograph::graph::CancelledException`、asio 取消 | `Canceled` | 由执行层补占位 `[Canceled]` |
| 抛 `neograph::graph::NodeInterrupt` | `Interrupted` | `[Interrupt]` + `MessageFlag::Interrupt` |
| 未执行即定案: 工具不存在 / 权限拒绝 / 重复调用被拒 | `Failed` | 自解释的一句话 (见 2.6), 同样经 `[Error: …]` 格式化 |
| 插件 `PLUGINXX_OPERATOR_OK` | `Ok` | payload |
| 插件 `PLUGINXX_OPERATOR_FAILED` | `Failed` | payload (错误内容) |
| 插件 `PLUGINXX_OPERATOR_CANCELED` | `Canceled` | payload |
| 插件工具超时 (`default_timeout_ms`) | `Failed` | `timed out after <n> seconds` |
| host tool `ok=false` | `Failed` | `errorMessage` |
| MCP `isError: true` | `Failed` | 汇总后的 content 文本 |
| 未包装的原生 `neograph::Tool` | `Ok` | 文本原样 (无状态通道, 见 3.7) |

### 2.3 模型侧文本格式化 (执行层唯一实现)

只有 `Failed` 会改变文本, 其余状态一律原样透传:

| 状态 | 给模型的文本 |
|---|---|
| `Ok` | 原样, 不做任何包裹 |
| `Failed` | 单行: `[Error: <text>]`; 含换行: 第一行 `[Error]`, 其后原样接 `<text>`; `text` 为空: `[Error]` |
| `Canceled` | 文本原样; 未产出结果的调用由执行层补占位 `[Canceled]` (原文本为 `[User canceled]`, 本次一并改名) |
| `Interrupted` | 文本原样; 占位 `[Interrupt]` + `MessageFlag::Interrupt` (沿用现状) |

`prompt.cpp` 里给模型的说明仍然有效 (`[Error: ...]` 表示"没执行/失败"), 只需补一句
"多行失败文本以 `[Error]` 单独一行开头"。
**失败原因只给内容, 不自带方括号前缀**: 权限拒绝、重复调用被拒、工具不存在、插件超时、
MCP 报错全部拼成一句白文交给执行层, 统一套上 `[Error: …]` —— 不再有
`[Permission denied]` / `[Repeated call denied by user: …]` / `[Plugin tool timeout]` 这类
各自的标记。

### 2.4 界面字段: `hasError` 退出, `status` 接手

字段与键名对照:

| 位置 | 现在 | 本次之后 | 序列化 |
|---|---|---|---|
| `ViewMessage::ToolData` | `bool hasError` | `util::ToolStatus status` | `ViewMessage` JSON 键 `status` (仅非 `Ok` 写入; 旧 `has_error` 只读兼容) |
| `WireDelta` (ToolEnd) | `bool hasError` | `util::ToolStatus status` | `deltaToJson` 键 `status` (仅非 `Ok` 写入) |
| `ChatMessage::extra` | `hasError` = true | `status` = `failed` / `interrupted` / `canceled` | 字符串 (仅非 `Ok` 写入) |
| `EventToolCallEnd` | `bool hasError` | `util::ToolStatus status` | 事件 JSON 键 `status` |

- `WireDelta::status` 只在 `ToolEnd` 有意义, 其它增量不写。
- 渲染: `Failed` 红色/错误样式; `Interrupted` 警告样式 (含 `[Interrupt]` 占位);
  `Canceled` 与 `Ok` 一样按普通样式 (占位文本自己已经说明), 不再看任何文本。
- 客户端**不认识的 status 取值一律按 `Ok` 处理**, 且所有 `switch` 必须带 `default`。

### 2.5 不动的 `hasError` (同名但含义不同)

下面这些 `hasError` 是**轮次/子代理**级别的, 与工具结果无关, 本次保持原样:

- `BaseAgent::TurnResult::hasError` / `WireTurnResult::hasError` /
  `makeTurnResult()` 的 `hasError` (轮次是否失败);
- `WireDelta` 以外的轮次级消息 (如 `WireError`);
- `AgentHost` 的 `RespSubagentBatch::results[].hasError` / 子代理事件里的 `hasError`
  (`subagent_manager.cpp`、`summarization.cpp` 都读它);
- 插件工具结果与它无关: 插件工具失败看 notify status。

改名只发生在**工具结果**这一条链: `ToolData` / `WireDelta`(ToolEnd) / `ChatMessage::extra` /
`EventToolCallEnd`。

### 2.6 agentxx 自动插入的结果标记 (收敛成三类)

下面这些文本都是**程序自己写的**占位或说明 (不是工具返回的)。本次把它们收敛成三类:
`[Canceled]` / `[Interrupt]` / `[Error: …]`。

| 标记 | 何时插入 | 位置 |
|---|---|---|
| `[Canceled]` | 调用被取消且没来得及产出结果 (原 `[User canceled]`) | `toolcall.cpp` 的取消收尾 |
| `[Interrupt]` | 调用触发中断 (等待用户确认); 仅为结构完整, 见下方说明 | `toolcall.cpp` + `MessageFlag::Interrupt` |
| `[Error: <内容>]` / `[Error]` + 正文 | 所有失败: 异常 / 工具给出的错误内容 / 插件 `FAILED` / 超时 / MCP `isError` / **权限拒绝** / **重复调用被拒** / 工具不存在 | `toolcall.cpp` 唯一格式化处 (2.3) |

各短路径给执行层的**内容** (不带方括号前缀):

| 情况 | 内容 | 模型看到 |
|---|---|---|
| 工具不存在 | `tool not found: <name>` | `[Error: tool not found: <name>]` |
| 权限拒绝 | `permission denied: <reason>` (`reason` 为权限模块给的说明; 无说明时就用 `permission denied`) | `[Error: permission denied: …]` |
| 重复调用被用户拒 | `repeated identical call denied by the user: <tool>; change your approach or the arguments` | `[Error: repeated identical …]` |
| 中间件阶段异常中止整批 | `<what> (tool call aborted at <phase> phase)` | `[Error: … (tool call aborted at … phase)]` |
| 插件工具超时 | `timed out after <n> seconds` | `[Error: timed out after …]` |

#### `[Interrupt]` 仍然保留, 但只为了消息结构

核查结论 (基于当前代码): 中断占位**只进**
`MiddlewareContext::graphDataKey_interruptToolcallCache`, 不进会话上下文 —— `isInterrupt` 分支
在 `appendSessionMessages` 之前就抛了 `NodeInterrupt`, resume 后工具重新执行并只写真实结果。
因此:

- 模型与界面都看不到这条占位; 真正携带语义的是 `MessageFlag::Interrupt` (resume 时用它
  把该条从缓存里排除, 让工具重新执行) 与"每条 assistant tool_call 都有对应 tool 回复"的
  结构完整性;
- 占位 `content` 仍保留 `[Interrupt]`: 成本为零, 万一将来某条路径把它透传给模型也是合法
  的非空文本, 调试 cache 时也一眼能看出是哪一条;
- **它不再参与任何判定**: 客户端 `isToolResultError` 里的
  `startsWith("[Interrupt]")` 删除, 中断状态靠 `ToolStatus::Interrupted`;
- `test_message_supplement` 里断言 `content == "[Interrupt]"` 与 flag 的用例保留 (它检的是
  cache 内容), 注释改成"仅为结构占位"。

需要改的说明处:

- **`agent/lib/src/agent/prompt.cpp`**: 「Context Markers」段落里的 `[User canceled]` 改成
  `[Canceled]`; `[Permission denied] …` / `[Repeated call denied by user: …]` 两条说明删掉
  (它们现在是 `[Error: …]` 的一种); 把"首行命中这些前缀就会被记成失败"的写法改成"这些
  标记由程序自动插入, 只为帮助你理解结果类型" —— 判定已经不看文本了, 提示词不能再暗示
  它影响状态。文件头的注释 (那里也列了一份标记清单) 一并改。
- **文档**: `docs/zh-cn/design/index.md` (第 129 行附近"未完成的补 `[User canceled]`
  占位")、`docs/zh-cn/design/roadmap.md` (TOOL-3 行)、`docs/zh-cn/design/tui.md`
  (工具消息折叠预览段)、`AGENTS.md` (工具错误约定段) 同步改成 `[Canceled]` 与新的三类标记;
  `AGENTS.md` 里那四条失败前缀的枚举改成"`[Error: …]` 统一格式"。
- **代码注释与测试**: `agent_tui.cpp` 的注释、`test_cancel` / `test_message_supplement` /
  `test_toolcall_parallel` / `test_race_guards` / `test_tui_stream` 里对
  `[User canceled]` 的断言全部改成 `[Canceled]`; 对 `[Permission denied]` /
  `[Repeated call denied` 的断言改成 `[Error: …]` 形态。

### 2.7 拼写统一规则 (`Canceled`, 一个 l)

本次**把自研的三个 cxx_* 库与 agentxx 一起统一成 `Canceled`**, 不留两种拼写:

| 库 / 位置 | 改名 |
|---|---|
| `cxx_utilxx_base` (`utilxx/cancel.h`) | `CancelledException` -> `CanceledException`; `CancelToken::isCancelled()` -> `isCanceled()`; `throwIfCancelled()` -> `throwIfCanceled()`; 成员 `cancelled_`; 文本 `run cancelled` -> `run canceled` |
| `cxx_utilxx_base` (`utilxx_base/exception.h`) | `ControlFlowKind::Cancelled` -> `Canceled` |
| `cxx_utilxx_base` (`utilxx/async_offload.h`) / `cxx_utilxx` (日志与注释) | `cancelled` -> `canceled` |
| `cxx_pluginxx` (`pluginxx/api/abi.h`) | `PLUGINXX_OPERATOR_CANCELLED` -> `PLUGINXX_OPERATOR_CANCELED` (其余注释/日志同步) |
| agentxx / 内置插件 / 测试 | 新增名字一律 `Canceled` (`ToolStatus::Canceled` / `ToolResult::canceled()` / wire `"canceled"` / `[Canceled]`); 引用依赖库的地方跟着改名 |
| `neograph` (外部) | **不改**: `neograph::graph::CancelledException` 与 `CancelToken::is_cancelled()` 保留原拼写, 引用处原样写 |

- 影响面: 三个 cxx_* 库都是独立发布的自研库, 改名是破坏性变更, 需要与 agentxx 同批发布;
  其它接入这些库的主程序也要跟着改 (与 `awaitPluginOp` 改名同一性质)。
- 写注释时: 自己新增的东西一律 `Canceled`; 只有 neograph 的标识符保留
  `Cancelled` / `is_cancelled`。neograph 的处理见 D8。

---

## 3. 接口与版本

### 3.1 lib 工具接口

```c++
// agentxx/tools/tool.h
virtual asio::awaitable<agentxx::util::ToolResult> execute_async(const utilxx_base::Json&);

// 图兼容桥: neograph::Tool::execute_async 的 override 保持返回 string, 内部取 .text
asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
std::string execute(const neograph::json& arguments) override;   // 同步桥, 同样取 .text
```

- `XXToolBase` 默认实现改为抛 `std::runtime_error("execute_async(Json) not implemented")`
  (与现状一致), 只是返回类型变了。
- `XXToolWrap`: 被包装的原生 `neograph::Tool` 无状态通道 -> `ToolResult::ok(文本)`。
- 执行层调用点 (`runToolCallBody`) 同步改类型; `ToolcallWrapNode::finalizeToolCall`
  (输出压缩) 与 `offloadShareStoreIfOversized` 只处理文本, 保持只接收 `text`。
- **重试只看异常**: `maxRetry` 今天只在抛异常时重试, 保持这一规则 —— 返回
  `ToolResult::fail(...)` 是"已完成的失败", 不重试。

### 3.2 插件 C ABI: 语义变化 + 契约代次 4

**不新增接口表、不新增成员** —— `PluginxxOperatorNotify.done(status, payload)` 已经够用,
本次只是把它的含义说清楚并落到实现上:

| notify status | 现在的含义 (v3 及以前) | 本次之后 (v4) |
|---|---|---|
| `PLUGINXX_OPERATOR_OK` | 文本结果 | `ToolStatus::Ok` + 结果文本 |
| `PLUGINXX_OPERATOR_FAILED` | 被主程序转成异常, 再变成 `[Error: <payload>]` | `ToolStatus::Failed` + **错误内容** (主程序负责格式化) |
| `PLUGINXX_OPERATOR_CANCELED` | 取消 | `ToolStatus::Canceled` (宏名本次一并改名, 见 2.7) |

版本动作:

- `agent/CMakeLists.txt`: `AGENTXX_PLUGIN_API_VERSION` 由 `3` 改为 `4`
  (同时覆盖 `PLUGINXX_API_VERSION` 与 `PLUGINXX_MIN_API_VERSION`)。旧插件声明
  `api_version=3 < 4`, 加载时被拒绝; `PluginManager::diagnosePluginPath` 的错误说明里补
  一句"插件需要按 API v4 重新编译"。
- `agent/lib/include/agentxx/plugin/api/plugin_api.h` 的 `agentxx.agent.tools` 表:
  `AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION` 与 `..._MIN_VERSION` 由 `2` 改为 `3`;
  `_BASELINE_SIZE` 不变 (布局没动), 表定义处补语义说明 (失败由 status 表达, 结果文本不带
  状态前缀)。
- 其它表不动; `agentxx.agent.hooks` 等按原样。
- 插件框架库 `cxx_pluginxx` (本项目自研、独立发布): `awaitXXXOp` 这一族**统一成"返回结果"**, 只留
  `...Result` 一个名字 —— 老的 `awaitPluginOp` / `awaitHostPluginOp` **直接删除**, 不留
  兼容包装:

  ```c++
  // pluginxx/runtime/op_driver.h
  struct PluginOpResult { int32_t status = PLUGINXX_OPERATOR_FAILED; std::string payload; };
  asio::awaitable<PluginOpResult> awaitPluginOpResult(PluginOpAwaitArgs args);
  ```
  - 三态原样返回 (含 `PLUGINXX_OPERATOR_CANCELED`), 本函数**不为业务失败抛异常**;
    等待本身被取消 (协程 `cancellation_slot`) 仍按原样传播;
  - `agentxx/util/cancel_adapter.h`: `awaitHostPluginOp` 同改名为
    `awaitHostPluginOpResult`, 只负责把 `utilxx::CanceledException` 换算成状态
    `PLUGINXX_OPERATOR_CANCELED`;
  - 需要"非 OK 就抛"的调用点 (图节点 / 钩子派发) 统一走一行小工具
    `agentxx::util::requirePluginOpOk(PluginOpResult)`: `OK` -> payload;
    `PLUGINXX_OPERATOR_CANCELED` -> `neograph::graph::CancelledException`; 其它 -> `std::runtime_error(payload)`;
  - 调用点共三处要改: `plugin_graph_node.cpp`、`plugin_manager_hooks.cpp`、
    `PluginTool`(`plugin_manager_adapters.cpp`, 直接拿状态映射成 `ToolResult`);
    `plugin_framework.h` 的 `using pluginxx::awaitPluginOp;` 改成 `using
    pluginxx::awaitPluginOpResult;`;
  - 影响面: 这是 `cxx_pluginxx` 的公开 API 改名与删除, 其它接入该库的主程序也要跟着改;
    库内头注释与示例一并更新;
  - 不在本次范围: `awaitPluginLifecycle` (名字不以 `Op` 结尾, 返回 `bool` + 错误串的
    生命周期专用等待), 保持原样。
- `PluginTool` 的早退分支 (实例已释放 / 已禁用 / `execute_start` 为空) 继续抛异常 ——
  那属于主程序与插件之间的一致性问题, 不是工具执行结果; 执行层照旧记 `Failed`。
- 超时分支从 `asyncWithTimeout<std::string>` 改为 `asyncWithTimeout<ToolResult>`, 回退值
  改成 `ToolResult::fail("timed out after <n> seconds")`。

### 3.3 Wire 协议

- 字段: `WireDelta.hasError` -> `WireDelta.status` (字符串, `ok` 不写), 见
  `conversation_types.h` / `wire_protocol.h` 的 `deltaToJson` / `deltaFromJson`。
- 版本: `WireProtocol::kVersion` `1` -> `2`, `kMinVersion` `0` -> `2`。
- 握手补两条检查 (现在只有"对端更新"一条):
  - 服务端: `hello.protocolVersion < kMinVersion` -> `WireHelloAck{ok=false, error="client
    protocol version N is older than the minimum M; please update the client"}`;
  - 客户端: `helloAck.protocolVersion < kMinVersion` -> 按连接失败处理并在 TUI/CLI 给出
    可读原因 ("服务端版本过老, 请升级服务端")。
  - 两条都写日志 (`[session_ctrl]`), 便于排查。
- 生成物: `docs/zh-cn/design/wire-protocol-fields.md` 与 `agent/schema/wire-schema.json`
  用 `AGENTXX_UPDATE_WIRE_SCHEMA=1 agentxx_test wire_schema` 重新生成。

### 3.4 兼容策略

- **本次不兼容**: 旧客户端 <-> 新服务端、新客户端 <-> 旧服务端、旧插件 -> 新主程序
  (API 3 -> 4 直接拒绝)。理由是这些对端都会按"文本前缀 = 失败"的旧规则理解数据,
  同时保留两套会长期付双份维护成本。
- **从现在起向前兼容**: 新字段一律追加; `status` 取值追加新值时, 老对端必须按 `Ok`
  处理; 表成员追加沿用"表尾追加 + `VERSION` +1 + `ifaceHas`"的既有规则 (见
  `docs/zh-cn/design/plugins.md` §17)。
- 旧会话数据 (会话库里已存在的 `has_error`): `ViewMessage::fromJson` 保留一段
  **只读**兼容 —— 没有 `status` 且 `has_error == true` 时映射成 `failed`, 注释标明
  "旧会话字段, 一段时间后可删"。这只影响历史颜色, 不是协议兼容。

### 3.5 插件 SDK

`plugin_kit.h` 的三个注册函数接受返回 `util::ToolResult` 的工具实现体 (用 `if constexpr`
区分返回类型, 老写法不受影响):

| 工具实现体返回 | 上报 |
|---|---|
| `std::string` / `asio::awaitable<std::string>` | `PLUGINXX_OPERATOR_OK` |
| `util::ToolResult` / `asio::awaitable<util::ToolResult>` | `status == Ok` -> `OK`; 其余 -> `FAILED` (文本用 `text`) |
| 抛异常 / 协程体内异常 | `FAILED` (`e.what()`) |
| 取消令牌生效 | 上报 `PLUGINXX_OPERATOR_CANCELED` (工具状态 `Canceled`) |

`plugin_kit.h` 里加一行 `using agentxx::util::ToolResult;` / `ToolStatus` (纳入
`agentxx::plugin` 命名空间), 插件侧写 `return ToolResult::fail("path does not exist");`
即可。

### 3.6 JS 插件

- `execute` 返回值:
  - 字符串 / 其他值 -> 与今天一致 (JSON 序列化成文本), 状态 `Ok`;
  - 对象且**带 `status` 字符串字段** -> 按 `ToolResult` 处理: `text` 取 `text` 字段
    (缺省用 `JSON.stringify` 结果), 状态由 `status` 映射 (`ok`/`failed`/`interrupted`/
    `canceled`, 未知按 `ok`);
  - `throw` / Promise reject -> `Failed`; 引擎停止/取消 -> `Canceled` (不变)。
- 示例插件 `example_js` 增加一个"返回失败结果"的工具; `example_js_execute_command`
  的运行失败分支改成返回 `{status:"failed", text:…}`。
- 实现: `ToolExecReq` 增加 `status` 字段; `doToolExecute` 的 `Value` 分支识别对象形态;
  上报处按 `status` 选 `OK` / `FAILED` (不再只看 `error` 是否为空)。

### 3.7 MCP / host tool / 原生工具

- `McpClientTool::execute_async`: 读 `resp["isError"]`; 为真 -> `ToolResult::fail(汇总
  文本)`。协议层错误 (无结果/超时) 继续抛, 由执行层记 `Failed`。
- `HostTool::execute_async`: `handler_` 保持"失败抛异常"的形态 (它来自 FFI/客户端,
  `WireHostToolResult.ok` 已表达状态), 执行层统一转 `Failed`; 文本即 `errorMessage`。
- 未包装的 `neograph::Tool`: 结果的字符串一律 `Ok`; 文档里写明"需要状态请用
  `XXToolWrap` 或自建 `XXToolBase` 子类"。

---

## 4. 文件清单 (逐项)

### 4.1 lib 核心

| 文件 | 改动 |
|---|---|
| `agent/lib/include/agentxx/util/tool_result.h` | **新增**: `ToolStatus` / `ToolResult` / 名字互转 (全部 inline, 不需要新 `install` 头列表) |
| `agent/lib/include/agentxx/util/tool_result_text.h` | **删除** (前缀判定整套下线) |
| `agent/lib/include/agentxx/tools/tool.h` | 主接口返回 `ToolResult`; 桥接说明; `XXToolWrap` 说明 |
| `agent/lib/src/tools/tool.cpp` | 基类默认实现、`neograph::json` 桥、`execute()` 同步桥取 `.text`; `XXToolWrap` 返回 `ok(文本)` |
| `agent/lib/include/agentxx/tools/share_store.h` / `src/tools/share_store.cpp` | 返回 `ToolResult` (参数错仍抛) |
| `agent/lib/include/agentxx/tools/subagent.h` / `src/tools/subagent.cpp` | 同上; 子代理失败不算工具失败 (`Ok` + 报告文本) |
| `agent/lib/include/agentxx/tools/event.h` / `src/tools/event.cpp` | 同上; 7 个 `opt` 的提示文本保持原样, 只有真正的失败改成 `fail` |
| `agent/lib/include/agentxx/tools/git_worktree.h` / `src/tools/git_worktree.cpp` | `[Error] … failed (exit=…)` -> `ToolResult::fail("… failed (exit=…)")` |
| `agent/lib/include/agentxx/tools/host_tool.h` | 返回类型跟随 |
| `agent/lib/include/agentxx/protocol/mcp_client.h` / `src/protocol/mcp_client.cpp` | `McpClientTool` 返回 `ToolResult`, 读 `isError` |
| `agent/lib/include/agentxx/plugin/plugin_manager.h` / `src/plugins/plugin_manager_adapters.cpp` | `PluginTool::execute_async` 返回 `ToolResult`; OK/FAILED/CANCELLED 映射; 超时文本改成 `timed out after …` |
| `agent/lib/include/agentxx/util/cancel_adapter.h` | `awaitHostPluginOp` -> `awaitHostPluginOpResult` (旧名删除); 新增 `requirePluginOpOk` |
| `agent/lib/src/nodes/toolcall.cpp` | 状态机改造; `formatFailureText`; 删除前缀判定; 取消占位 `[User canceled]` -> `[Canceled]`; 权限拒绝/重复调用被拒改成 `Failed` + 自解释内容 (不再有自己的前缀); `extra["status"]` 取代 `extra["hasError"]`; 指标按 status 计 |
| `agent/lib/include/agentxx/nodes/toolcall.h` | 注释与私有成员 (`PreparedToolCall` 增加 `ToolStatus`) |
| `agent/lib/include/agentxx/agent/conversation_types.h` | `ToolData.status` / `WireDelta.status`; toJson/fromJson (含旧 `has_error` 只读兼容) |
| `agent/lib/include/agentxx/agent/io/wire_protocol.h` | `deltaToJson` / `deltaFromJson` 用 `status` |
| `agent/lib/include/agentxx/agent/io/agent_io_transport.h` | `WireProtocol::kVersion`/`kMinVersion`; 版本注释 |
| `agent/lib/src/agent/io/session_server_agent_io.cpp` | 握手增加"客户端过老"拒绝 |
| `agent/lib/src/event/event_stream.cpp` | `extra["status"]` -> `ToolData::status` + `EventToolCallEnd::status` |
| `agent/lib/src/plugins/plugin_channel_agent_io.cpp` | 转发给客户端插件的 ToolEnd 载荷 `hasError` -> `status` |
| `agent/lib/include/agentxx/event/events.h` | `EventToolCallEnd::status` |
| `agent/lib/src/agent/prompt.cpp` | 「Context Markers」里 `[User canceled]` 改 `[Canceled]`; 删掉"前缀会被记成失败"的写法; 补"多行失败文本首行是 `[Error]`"; 文件头注释同步 (2.6) |
| `agent/lib/include/agentxx/plugin/api/plugin_api.h` | tools 表 `VERSION`/`MIN_VERSION` 2 -> 3 + 语义说明 |
| `agent/lib/include/agentxx/plugin/api/plugin_kit.h` | 三个注册函数支持 `ToolResult`; 引入 `ToolResult`/`ToolStatus` |
| `agent/third_party/cxx_pluginxx/include/pluginxx/runtime/op_driver.h` | `awaitPluginOp` 改名为 `awaitPluginOpResult` (返回 `PluginOpResult`) 并删除旧名 |
| `agent/lib/include/agentxx/plugin/plugin_framework.h` | `using pluginxx::awaitPluginOp;` -> `awaitPluginOpResult` |
| `agent/lib/src/plugins/plugin_graph_node.cpp` | 改用 `awaitHostPluginOpResult` + `requirePluginOpOk` |
| `agent/lib/src/plugins/plugin_manager_hooks.cpp` | 同上 |
| `agent/lib/src/nodes/modelcall.cpp` | 仅注释里的 `[User canceled]` 改 `[Canceled]` |
| `agent/CMakeLists.txt` | `AGENTXX_PLUGIN_API_VERSION` 3 -> 4 |

### 4.2 依赖库 (`cxx_utilxx_base` / `cxx_utilxx` / `cxx_pluginxx`)

| 文件 | 改动 |
|---|---|
| `cxx_utilxx_base/include/utilxx/cancel.h` | `CancelledException` -> `CanceledException`; `isCancelled()` -> `isCanceled()`; `throwIfCancelled()` -> `throwIfCanceled()`; 成员与文本同步 |
| `cxx_utilxx_base/include/utilxx_base/exception.h` | `ControlFlowKind::Cancelled` -> `Canceled`; 捕获类型改名 |
| `cxx_utilxx_base/include/utilxx/async_offload.h` | 注释与示例里的 `CancelledException` / `isCancelled()` |
| `cxx_utilxx/src/http_server.cpp` (及其它日志文本) | `cancelled` -> `canceled` |
| `cxx_pluginxx/include/pluginxx/api/abi.h` | `PLUGINXX_OPERATOR_CANCELLED` -> `PLUGINXX_OPERATOR_CANCELED` (契约说明同步) |
| `cxx_pluginxx/include/pluginxx/runtime/op_driver.h` / `host/host_core.h` / `host/lifecycle.h` / `kit/kit.h` | 引用改名; `awaitPluginOp` -> `awaitPluginOpResult` (见 3.2) |

### 4.3 客户端

| 文件 | 改动 |
|---|---|
| `agent/client/src/io/tui/agent_tui.cpp` | ToolEnd 读写 `status` |
| `agent/client/src/io/tui/components/message_list.cpp` | `isToolResultError` -> 按 status; `Interrupted` 警告色; 删 `[Interrupt]` 文本判断 |
| `agent/client/src/io/stdio/agent_stdio.cpp` | `[Tool] <名字> <done/error/canceled/interrupted>` |
| `agent/client/src/io/tui/tui_i18n.cpp` | 新增状态文案键 (中英) |
| `agent/client/src/mode_runners.cpp` (或握手处理处) | 服务端版本过老时给出可读失败原因 |
| `agent/client/src/io/jsonl/*` | 只核对文档注释 (delta 原样输出, 自动跟随) |

### 4.4 内置插件 (18 个文件、约 49 处注册)

- 必须改失败文本: `agentxx_filesystem` (23 处)、`agentxx_math` (4 处)、
  `agentxx_execute_command` (3 处 + 超时/取消块的终态)、`agentxx_filesystem.cpp` (1 处)。
- 需要审计"整调用失败但返回成功文本"的地方: `agentxx_string`、`agentxx_system`、
  `agentxx_websearch`、`agentxx_rag_search`、`agentxx_planning`、`agentxx_codegraph`、
  `agentxx_chatbot`、`agentxx_screen_capture`、`agentxx_system_monitor`、
  `agentxx_text_selection_monitor`、`agentxx_computer_use`、`agentxx_audio_stream`。
- 示例: `example_plugin`、`example_multi_mount`、`example_runtime` (工具示例改新写法,
  保留一个"返回字符串 = 成功"的对照)、`example_js`、`example_js_execute_command`。
- `agentxx_javascript_engine`: `doToolExecute` 支持 `{status, text}` 形态。

### 4.5 测试

| 模块 | 改动 |
|---|---|
| `test_message_supplement` / `test_agent` / `test_toolcall_parallel` / `test_cancel` / `test_race_guards` / `test_summarization` | `hasError` 断言改 `status`; `[User canceled]` 断言改 `[Canceled]`; `[Permission denied]` / `[Repeated call denied` 断言改 `[Error: …]`; 新增"文本以 `[Error` 开头但 `status=ok` 不判失败"回归 |
| `test_filesystem_tools` / `test_math_tools` / `test_command_tools` / `test_string_tools` / `test_datetime_tool` | 文本断言保持 (格式化后文本基本不变), 增补状态断言 |
| `test_wire_schema` / `test_wire_roundtrip` | `status` 字段; 生成物重生成 |
| `test_tui_stream` / `test_tui_tool_header` / `test_tui_ui_items` | `ToolData::status` 渲染断言 (失败红/中断黄/取消普通); `[User canceled]` 样本改 `[Canceled]` |
| `test_event_tools` | 删除 `toolResultTextIsFailure` 断言, 改成"成功返回 `status=ok`" |
| `test_plugin_sdk` / `test_plugin_runtime` / `test_plugin_multi_mount` / `test_plugin_host_mount` / `test_client_plugins` / `test_plugins` | API 版本 4 与 tools 表 v3 断言; `ToolResult` 全流程 (OK/FAILED/CANCELLED/超时); `awaitXXXOp` 改名后的调用点 |
| `test_boundaries` | 版本四件套按新值校验; 成员台账不变 |
| 写死版本号的地方 | `test_plugin_runtime` 的偏移/版本表、`test_plugin_sdk`、`test_plugin_multi_mount` 里如果直接写了 `2` / `3`, 一并改成宏; 优先用宏避免下次再改 |
| 新增 | 版本握手用例 (过老客户端被拒 / 过老服务端被拒)、MCP `isError` 映射、插件超时状态、`interruptToolcallCache` 仍按 `MessageFlag::Interrupt` 处理 |
| 依赖库改名波及 | 各测试里 `utilxx::CancelledException` / `ControlFlowKind::Cancelled` / `isCancelled()` 的引用跟着改 |

### 4.6 文档与生成物

`AGENTS.md`(工具错误约定 + 自动插入标记清单) · `docs/zh-cn/design/index.md`(工具调用节、附录 A、
"未完成的补 `[Canceled]` 占位") · `docs/zh-cn/design/roadmap.md`(TOOL-3 行) ·
`docs/zh-cn/design/plugins.md`(工具 SDK、API v4 变更、工具结果状态、`awaitPluginOpResult`) ·
`docs/zh-cn/design/events.md`(§5.1 前缀限制**删除**, 改为"文本只是文案") ·
`docs/zh-cn/design/tui.md`(工具消息按状态渲染 + `[Canceled]`) ·
`docs/zh-cn/design/wire-protocol-fields.md` + `agent/schema/wire-schema.json`(重生成) ·
`docs/zh-cn/design/ffi.md`(若提到工具结果) ·
`resource/history/toolcall-result/work.md`(实施记录, 新建)。

---

## 5. 实施阶段

| 阶段 | 内容 | 验证 |
|---|---|---|
| P0 | 依赖库拼写统一 (`cxx_utilxx_base` / `cxx_utilxx` / `cxx_pluginxx`) + `awaitXXXOp` 改名并删旧名 + agentxx 侧引用跟着改 | lib/test/client/插件全量编译通过; `plugin_runtime` `plugin_sdk` `plugins` |
| P1 | `util/tool_result.h` + `XXToolBase` 改签名 + lib 内 7 个工具 + 执行层状态机与格式化 + 删除前缀判定 + `extra["status"]` + `[Canceled]` 改名 + 权限/重复短路径改 `[Error]` + `prompt.cpp` 标记说明 | lib 编译; `toolcall_parallel` `cancel` `race_guards` `message_supplement` `summarization` `event_tools` |
| P2 | Wire 字段与版本、`ViewMessage`/`ToolData`、`event_stream`/`events.h`、客户端渲染与 CLI | `wire_schema`(重生成) `wire_roundtrip` `tui_stream` `tui_tool_header` `tui_ui_items` `remote_agent` |
| P3 | 插件边界 (`PluginTool` 映射 + 超时) + API 3->4 + tools 表 v3 + SDK 支持 `ToolResult` + JS 引擎 | `plugin_runtime` `plugin_sdk` `plugin_host_mount` `plugin_multi_mount` `client_plugins` `plugins` `boundaries` |
| P4 | 内置插件改造 (filesystem / math / execute_command + 审计其余) | `filesystem_tools` `math_tools` `command_tools` `string_tools` `plugin_example_*` |
| P5 | 测试补齐 (版本握手、MCP、超时、"文本像错误但状态是成功") + 拼写扫描 (`[User canceled]`、`cancelled` 清零, neograph 除外) + 全量回归 | 全部模块 |
| P6 | 文档与生成物 + `work.md`; 分段提交 | `config_keys` `boundaries` 无新增失败 |

每阶段结束都要: 增量编译 lib/test/client -> 复制 `agentxx_test.exe` -> 跑上表模块 ->
`git add` 指定文件后提交 (不碰 `TODOS.md`、`README.md`、`agent/third_party` 里与本次无关
的改动)。

上表每个阶段列出的模块**就是该阶段必须一并改好的测试**: 签名或字段一变, 这些模块会立刻
编译失败或断言失败; P5 只补新增用例 (版本握手、MCP、超时、"文本像错误但状态是成功")
与全量回归, 不承担旧断言迁移。

---

## 6. 风险与遗留决策

### 6.1 风险

1. **签名大范围变更**: `XXToolBase::execute_async` 一旦改返回类型, lib 内 7 个工具、
   测试里十几个假工具全部编译报错。按目录顺序修 (lib -> test -> client), 不要留中间态。
2. **旧数据**: 会话库历史消息只有 `has_error`; 已安排只读兼容, 但 `ChatMessage::extra`
   旧值 (`hasError`) 不再被读, 历史 LLM 上下文里该键直接失效 (只影响调试可读性)。
3. **版本提升让本地已编译插件失效**: 必须全量重编插件 (构建脚本会重新编译
   `agent/plugins`), 否则启动报"插件 API 版本过低"; 这是预期行为, 需要在提交说明里写清。
4. **短路径文本改格式**: 权限拒绝 / 重复调用被拒的文本由 `[Permission denied] …` /
   `[Repeated call denied by user: …]` 改成白文内容 + `Failed`; 模型侧提示词与几个断言要同步,
   否则会出现"提示词说一种、实际另一种"。
5. **`execute_command` 的结构化块**: 超时/取消标记成 `Failed`/`Canceled` 后, 文本会被
   执行层加一行 `[Error]` 前缀, 块本身保持原样; 需要确认模型读起来仍然顺。

### 6.2 取舍清单

**已定** (按本次要求):

| 事项 | 结论 |
|---|---|
| `ToolResult::fail` 的文本 | 只存错误内容, 不带 `[Error: ]` 包裹; 由执行层统一格式化 (2.3) |
| 超时文案 | 只写 `timed out …`, 不提"plugin / 插件" (模型不需要知道这个对象) |
| 界面字段 | 删 `hasError`, 全改 `status`; 渲染与判定同步改 (2.4) |
| 版本策略 | 插件 API 3->4 (`MIN` 同步)、tools 表 v3、Wire 2/2; 不兼容旧客户端/服务端/插件 |
| JS 插件 | 与 C 插件统一: 返回 `{status, text}` 即 `ToolResult` |
| 取消占位文本 | `[User canceled]` -> `[Canceled]`; `prompt.cpp` 与文档的标记说明同步改 (2.6) |
| 标记收敛 | 只留 `[Canceled]` / `[Interrupt]` / `[Error: …]` 三类; 权限拒绝与重复调用被拒也走 `[Error: …]` |
| 拼写 | **三个自研 cxx_* 库与 agentxx 一起统一 `Canceled`**; 仅 neograph 的标识符保留原拼写 (2.7) |
| `awaitXXXOp` | 统一成 `awaitPluginOpResult` / `awaitHostPluginOpResult`, 旧名删除, 不留兼容包装 |
| 接口表 | 不新增表、不新增成员 —— `PluginxxOperatorNotify.status` 已够用 |

**待确认** (建议值已写在表里, 不回复则按建议值实施):

| 编号 | 事项 | 建议 |
|---|---|---|
| D1 | `ToolStatus` 取值集合 | `ok` / `failed` / `interrupted` / `canceled`; 不引入 `partial` (批量部分失败保持 `ok`) |
| D2 | 多行失败文本的排版 | 单行 `[Error: <text>]`, 多行 `[Error]` + 换行 + 原文 |
| D3 | 旧会话 `has_error` 只读兼容 | 保留 (约 6 行), 注释标明可删 |
| D4 | `Canceled` 的文本 | 工具文本原样; 未产出结果的调用仍由执行层补 `[Canceled]` |
| D5 | `execute_command` 超时/取消终态 | 取消 -> `canceled`; 超时 -> `failed`; 其余结构化输出保持 `ok` |
| D6 | `[Interrupt]` 占位的 content 是否保留 | 保留 (只作结构占位与 cache 调试标记, 不参与判定; 见 2.6) |
| D7 | `[Canceled]` 是否另外给中文界面一句翻译 | 不另给: 标记是给模型的固定英文文本, 界面按 `status` 着色与打标签 |
| D8 | neograph 的 `CancelledException` / `is_cancelled()` | 不改 (它不在本项目控制范围内); 如上游自己改了对齐 |

---

## 7. 验收标准

1. 全项目**没有任何代码**为了判断工具成败去看结果文本: `tool_result_text.h` 已删除,
   `starts_with("[Error") / starts_with("[Interrupt") / starts_with("[Permission denied")`
   之类判断在执行层与客户端都不存在 (grep 可验证)。
2. 工具可以只靠 `ToolResult::fail("…")` 表达失败, 文本里不写任何前缀; 模型仍然读到
   `[Error: …]` (由执行层加)。
3. 插件工具: `OK`/`FAILED`/`CANCELED`/超时 四种终态分别映射到 `ok`/`failed`/
   `canceled`/`failed`, 且 `failed` 的文本就是插件给的错误内容 (无 `[Plugin tool timeout]`)。
4. JS 插件: 返回 `{status:"failed", text}`、`throw`、Promise reject 三条路径都得到
   `failed`; 返回字符串仍是 `ok`。
5. MCP 工具 `isError` 得到 `failed`。
6. 客户端只按 `status` 渲染 (失败红、中断黄、取消普通), 未知取值按 `ok`。
7. 版本: 插件 API 4、tools 表 v3、Wire 2/2; 旧插件、旧客户端、旧服务端都被明确拒绝并
   给出可读原因。
8. 测试全绿 (除 `boundaries` 规则 15 的既有 `cxx_pluginxx_ui` 失败), `wire_schema`
   生成物已更新, 文档同步。
9. `[Canceled]` 已全部替换 `[User canceled]` (代码/注释/测试/文档), `prompt.cpp` 的标记
   说明里不再暗示"文本前缀参与状态判定"; `[Permission denied] …` /
   `[Repeated call denied by user: …]` 两种前缀在全仓已不存在 (统一成 `[Error: …]`)。
10. 拼写: 三个自研 cxx_* 库、agentxx、内置插件、测试里找不到 `CancelledException` /
    `isCancelled` / `PLUGINXX_OPERATOR_CANCELLED` / `cancelled` (neograph 除外); 新增名字全为
    `Canceled`。
11. `awaitPluginOp` / `awaitHostPluginOp` 在全仓已不存在, 只剩
    `awaitPluginOpResult` / `awaitHostPluginOpResult`; 图节点与钩子派发的行为与今天一致
    (非 OK 抛异常, 取消抛 `neograph::graph::CancelledException`)。
12. `[Interrupt]` 占位仍留在 `interruptToolcallCache` 里并带 `MessageFlag::Interrupt`,
    但客户端与执行层没有任何代码靠它判断状态。
