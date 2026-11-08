# 工具特化渲染前的参数自动修正 — 设计方案

## 1. 背景

模型经常把参数写成"差一层"的形态: 数组参数给单个字符串 (`file_patterns: "a/*.cpp"`)、
对象数组给单个对象 (`edits: {...}`)、数值给字符串 (`offset: "100"`)、枚举大小写不一致。
执行侧早就统一处理了; 展示侧没有, 导致每个特化渲染器各写一份兼容。

## 2. 现状调查 (文件 / 函数)

### 2.1 执行侧: 参数自愈在哪里

- `agent/lib/src/nodes/toolcall.cpp`
    - `ToolcallWrapNode::autoFixArgsType(def, args)`: 按 `def.parameters` (工具参数
      JSON Schema) 修正 `args`
    - 内部 helper: `parseFullNumber` / `numberToJsonString` / `tryDoubleToInt64` /
      `SchemaTypes.getSchemaTypes` / `isStringArrayItems` / `isObjectArrayItems` /
      `makeEnumIgnoreCaseMap` / `autoFixEnumStringCase`
    - 调用点: `prepareToolInvocation` (prepare 阶段, 执行体查找之后)
- 声明: `agent/lib/include/agentxx/nodes/toolcall.h` 的 `static bool autoFixArgsType`
- 测试: `agent/test/test_toolcall_args.cpp` (直接调用该静态函数)

### 2.2 展示侧的参数从哪来

- 实时: `agent/lib/src/event/event_stream.cpp` 的 `EventBridge::handleChannelWrite`
  处理 `channel == "messages"` 时, 对 `role == "assistant"` 按 `tool_calls` 展开 Tool
  展示行, 参数原文写进 `ViewMessage::text` (`m.text = tc.arguments`), 随
  `WireDelta::InsertMessage` 下发并落盘
- 历史: `agent/lib/src/agent/context.cpp`
    - `SessionsManager::getOrCreate` / `getOrCreateAsync` → `SessionStore::loadSession`
      → `session->restoreWindow(...)`
    - 分页: `makeSessionStoreHooks` 的 `onLoadViewPage` → `SessionStore::loadViewMessagesPage`,
      经 `Session::collectViewRange` 使用
- 客户端: `agent/client/src/io/tui/components/message_list.cpp` 把工具行的 `msg.text`
  当作 `argsJson` 传给 `queryToolRender` → `renderClientTool`
  (`agent/lib/src/plugins/client_plugin_manager.cpp`) → 插件渲染器 / 主程序内置渲染器 /
  预设模版摘要。客户端**没有**工具 schema (wire 协议里没有工具定义)

### 2.3 自己做兼容的特化渲染器

- `agent/plugins/agentxx_filesystem/agentxx_fs_plugin.h`
    - `stringListArg`: 值是字符串时当单元素列表 (本该是 `stringArray` 参数)
    - `editSegmentsArg`: `edits` 是单个对象时按单段处理 (本该是 `objectArray`)
- `agent/lib/src/plugins/builtin_tool_renderers.cpp` (Store / Subagent): 没有类型兼容,
  但数值参数靠宽容读取才不会抛 (`offset` 传字符串时 `value<int64_t>` 会抛 → 整个渲染失败)
- `agent/plugins/agentxx_planning`: `todos` 元素对象/字符串两种形态是工具 schema 允许的
  写法, 不是类型兼容, 保持原样

### 2.4 协议面

wire 协议没有任何工具定义/参数 schema 的传递 (`MsgType` 里只有消息、模型、会话等)。
本次**不**新增 wire 消息: 展示历史由 agent 写, 让 agent 在写入/加载时把参数按 schema
自愈即可 (符合"agent 是展示历史唯一写者, 客户端只渲染"的既有约定)。

## 3. 方案

### 3.1 共享实现

新增 `agent/lib/include/agentxx/util/tool_args_fix.h` +
`agent/lib/src/util/tool_args_fix.cpp`:

```cpp
namespace agentxx::util {
/// 按工具参数 JSON Schema 自动修正参数类型 (执行前与展示前共用同一份实现)
bool autoFixToolArgsTypes(const utilxx_base::Json& parameters, utilxx_base::Json& args);

/// 同上, 但输入输出都是参数 JSON 文本: 无变化/无法解析/非对象时原样返回
std::string autoFixedToolArgsJson(const utilxx_base::Json& parameters, std::string_view argsJson);
}
```

- `ToolcallWrapNode::autoFixArgsType(def, args)` 变成薄包装 (保留签名, 测试与老调用点不变)
- 文档注释整体搬到新头文件, 节点侧只留一句指向

### 3.2 schema 查询

`agent/lib/include/agentxx/plugin/tool_registry.h` 增加:

```cpp
/// 登记静态工具的参数 schema (BaseAgent::init 调用; 只保存名字与 parameters)
void setStaticToolSchemas(std::vector<std::pair<std::string, utilxx_base::Json>> schemas);

/// 取工具参数 schema: 静态登记表 → 动态插件工具定义; 未登记返回 false
bool parametersOf(std::string_view name, utilxx_base::Json& out) const;
```

- 插件工具的 `get_definition().parameters` 每次构造有成本, 用 `mutable` 缓存,
  `registerTool` / `unregisterTool` 时清掉对应项
- `BaseAgent::init` 的 `bind_tools` 步骤里, 与 `setStaticToolNames` 同一循环登记

### 3.3 展示写入点 (实时)

`EventBridge::handleChannelWrite` 展开 Tool 行时:

```cpp
m.text = displayArgsOf(ctx, toolName, arguments);   // 内部: parametersOf → autoFixedToolArgsJson
```

- 无 registry / 无该工具 schema / 参数不是 JSON 对象 → 原样, 与今天完全一致
- 与执行侧一致: 不注入 `sessionId` / `tool_call_id`

### 3.4 历史加载 (老会话)

`SessionsManager` 增加:

```cpp
/// 工具行参数自愈 (由 BaseAgent 注入; 捕获 shared_ptr<ToolRegistry>, 无悬垂风险)
std::function<std::string(std::string_view toolName, std::string_view argsJson)> toolArgsNormalizer;
```

- 私有 helper `normalizeToolViewArgs(std::vector<ViewMessage>&)`: 只处理
  `Role::Tool` 且 `text` 非空的行走 normalizer
- `getOrCreate` / `getOrCreateAsync` 在 `restoreWindow` 之前过一遍
- `makeSessionStoreHooks` 多收一个 normalizer 参数, `onLoadViewPage` 返回前过一遍
- `BaseAgent::init` 建 lambda 注入 (捕获 `agentContext->toolRegistry`)

### 3.5 渲染器简化

- `agent/plugins/agentxx_filesystem/agentxx_fs_plugin.h`
    - `stringListArg`: 只按数组读 (去掉 string 分支)
    - `editSegmentsArg`: 去掉 `edits` 单对象分支 (保留 `old_str`/`new_str` 单段旧写法,
      那是工具本身支持的参数形态, 不是类型兼容)
- `agent/lib/src/plugins/builtin_tool_renderers.cpp`: 行为不变, 注释补充"参数已按
  schema 自愈, 数值参数不会是字符串"

## 4. 边界与不做的事

- 不改 LLM 上下文 (assistant `tool_calls.arguments` 保持模型原文, 模型看到的是自己写的)
- 不改 wire 协议 / 客户端不引入 schema 缓存
- 不注入执行期字段 (`sessionId`/`tool_call_id`), 也不做权限相关的任何处理
- 流式参数 (工具调用参数还在接收) 不涉及: 工具行走的是 assistant 消息定稿后的展开
- 插件工具被卸载/未登记 schema 的工具 → 原样展示, 无回归

## 5. 数据兼容

- 库内既有行不动; 尾窗加载与分页读取时在内存里自愈, 所以升级后旧会话的折叠头摘要
  与实时一致
- 行被再次更新 (工具结果回填 `updateViewMessage`) 时会把自愈后的参数写回库, 属预期
- 渲染器简化后, 若某条行的参数既非自愈形态且工具 schema 已不存在 (如插件卸载后的
  历史), 摘要退化为空: 与今天"渲染器不认识该参数形态"的结果一致

## 6. 分步实施

1. 抽 `util/tool_args_fix.*`, 节点改调用; `test_toolcall_args` 保持绿
2. `ToolRegistry` 增 schema 登记/查询; `BaseAgent::init` 填表
3. `EventBridge` 建行时自愈 + 单测 (`test_event_bridge`)
4. `SessionsManager` 加载/分页自愈 + 单测 (有 SessionStore 的模块)
5. fs 插件渲染器简化
6. 文档 (index.md / plugins.md) 同步; debug 构建 + 相关测试模块

## 7. 验证

- 构建: `agent/script/windows_debug_build.bat` (debug)
- 测试模块: `toolcall_args`、`event_bridge`、`session_persistence`、`tui_tool_header` 等
- 手工: 恢复一个旧会话, 观察 Glob/Grep/Edit 折叠头摘要

## 8. 风险

- `parametersOf` 在 io 线程调用插件工具 `get_definition()`: 每个工具行首次查询一次并缓存
- 加载历史时多一次 JSON 解析 (仅 Tool 行, 尾窗规模有上限)
- 共享 util 与插件 SDK `ArgReader` 的宽容读取有重叠: 未改第三方位库
