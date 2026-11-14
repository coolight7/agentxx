# 方案：工具特化渲染 = 显示名 + 折叠内容 + 展开内容

## 1. 背景与问题

工具消息的特化渲染（折叠头、展开体）由插件按 `tool_name` 注册，注册项只有**一个**
渲染回调（或一段预设模版 JSON）。它同时产出三件事：显示名、一行摘要、展开体 items。

三个问题：

1. **显示名只能来自回调**，而回调按约定只能在 client io 线程执行，UI 线程首帧拿不到
   结果 —— 于是首帧把工具名藏起来（占位行只剩 `[工具] 正在调用工具...`）。名字其实是
   静态信息，不需要渲染器算。
2. **折叠摘要与展开体绑在一次调用里**：展开体（Edit 的 diff、Plan 的状态图 + 待办）
   只在用户展开那一行时才需要，但折叠态每次渲染都会把它一并算出来并深拷进缓存。
3. **模版机制是一套只够取单个参数的迷你 DSL**（`template_json` 里塞 `displayName` /
   `summaryKey`，后者从未被读取的 `summaryTemplate` 还留在结构里），为了一个字段
   引入 JSON 解析与 1 MiB 校验；折叠头格式还各写各的（摘要字符串自带前导 `" · "`，
   TUI 侧再嗅探一次 `starts_with(" ·")`）。

围绕"名字要异步拿"还长出了一整套"名称结果"机制（按工具名额外投递一次渲染请求、
`lookupLatest("#工具名")`、两处 itemKey 版本项、中断预取）。

## 2. 方案

### 2.1 注册表与行结构

注册项从 `<toolName, 单一回调>` 改成：

```
<toolName, { displayName, 折叠内容渲染, 展开内容渲染 }>
```

- `displayName`：注册时确定，UI 线程同步读；空值在注册时归一为 `toolName`；
- 折叠内容渲染：可选，产出一行文本（不含前导 `" · "`）；
- 展开内容渲染：可选，产出 items（组件描述层数组）；
- 两者都空 = 只改名（内容全走默认渲染）。

工具行骨架因此**固定**（TUI 不再判断"有没有特化/是不是 decor"来决定画什么）：

- 折叠：`+ [工具] <displayName> · <内容>`
- 展开：第一行 `- [工具] <displayName> <耗时>`，第二行起 `<展开内容>`

各状态取什么内容。约定：渲染回调返回非 0，或返回成功但内容为空，都算 **"null"** —— 表示
这次没有自定义内容，由主程序走默认渲染（结果同样写缓存，不会每帧重试）。

折叠（` · ` 之后）按顺序判定：

1. 失败 (`Failed`) → 结果首行（错误色）；
2. 有自定义折叠内容（已返回的内容，或 `pending` 时沿用的上一份）→ 用它；
3. 注册了折叠内容渲染、但还没有任何可用内容 (`pending`) → **不画 ` · `**：该行只有名字，
   等内容上屏，不拿默认内容顶替（否则会先闪一句"正在调用工具..."再换成特化内容）；
4. 未注册折叠内容渲染 / 渲染返回 null → 默认渲染：运行中（还没完成）→ `正在调用工具...`；
   已完成 → 结果首行（结果为空则不画）。

展开（首行之后）按顺序判定：

1. 失败 (`Failed`) → 参数 JSON，下一行 结果（错误色，不渲染自定义 items）；
2. 有自定义展开 items（已返回的，或 `pending` 时沿用的上一份）→ 渲染 items；
3. 注册了展开内容渲染、但还没有任何可用 items (`pending`) → 首行之后留空；
4. 未注册展开内容渲染 / 渲染返回 null → 默认渲染：参数 JSON，
   下一行 运行中 `正在调用工具...` / 已完成 结果（空的不画）。

要点：

- 工具行只在该 tool 调用**生成完成**（参数完整）之后才存在，所以运行中的工具行同样走特化
  渲染：回调拿得到完整参数，返回什么就显示什么，不等工具执行完，也不看 `is_finished`；
- `正在调用工具...` 只出现在没有自定义内容可显示的两处：占位行、默认渲染的运行中行
  （两处用同一句文案 `msg.genToolCalling`）；
- `pending` 不拿默认内容顶替（折叠不画 ` · `、展开留空），避免"默认内容 → 自定义内容"
  的可见跳变；
- 显示名为空时（占位阶段工具名可能还没解析出来）不画 ` · `。

占位行（`GenPhase::ToolCall`，单行、**不可点击、不可展开**）：

```
{运行中符号} [工具] <displayName> · 正在调用工具...
```

- 角色标签 `[工具]` 与工具行一致（它最终被工具行取代）；
- 工具名还没解析出来时不画 ` · `，只有 `{符号} [工具] 正在调用工具...`；
- 它没有折叠/展开状态，也没有命中区，只是列表末尾的一行提示。

`update_tool_decor`（实例级、按 `tool_call_id`）覆盖同一组三要素，字段名不变
（`displayName` / `summary` / `items`），语义对应：名字 → 头行、`summary` → 折叠内容、
`items` → 展开内容。优先级：decor > 类型级注册项 > 默认渲染；decor 缺某一项时
**回落注册项**（不是"清空"）。

### 2.2 C ABI（`agentxx/plugin/api/client_plugin_api.h`）

```c
typedef struct AgentxxToolRenderInput { ... } AgentxxToolRenderInput;   // 不变

/// 折叠内容 (一行文本)
typedef struct AgentxxToolCollapsedOutput {
    PluginxxString text;   ///< 一行内容 (主程序补 " · " 分隔符; 空 = 无自定义内容)
} AgentxxToolCollapsedOutput;

/// 展开内容 (items)
typedef struct AgentxxToolExpandedOutput {
    PluginxxString items_json;  ///< items JSON 数组; 空 = 无自定义内容 (主程序走默认渲染)
} AgentxxToolExpandedOutput;

typedef int32_t(PLUGINXX_CALL* AgentxxToolCollapsedRenderFn)(
    void* user_data, const AgentxxToolRenderInput* input, AgentxxToolCollapsedOutput* output);

typedef int32_t(PLUGINXX_CALL* AgentxxToolExpandedRenderFn)(
    void* user_data, const AgentxxToolRenderInput* input, AgentxxToolExpandedOutput* output);

typedef struct AgentxxToolRenderSpec {
    uint32_t                     struct_size;   ///< sizeof(AgentxxToolRenderSpec)
    int32_t                      version;       ///< == 1
    PluginxxStringView           tool_name;     ///< 目标工具名 (必填)
    PluginxxStringView           display_name;  ///< 显示名 (空 = 用 tool_name)
    AgentxxToolCollapsedRenderFn collapsed_fn;  ///< 折叠内容渲染 (可 NULL)
    AgentxxToolExpandedRenderFn  expanded_fn;   ///< 展开内容渲染 (可 NULL)
    void*                        user_data;     ///< 两个回调共用
} AgentxxToolRenderSpec;
```

布局（64 位；`PluginxxStringView` = 16 字节）：

| 字段 | 偏移 |
|---|---|
| `struct_size` | 0 |
| `version` | 4 |
| `tool_name` | 8 |
| `display_name` | 24 |
| `collapsed_fn` | 40 |
| `expanded_fn` | 48 |
| `user_data` | 56 |
| `sizeof` | 64 |

删除：`AgentxxToolRenderFn`、`AgentxxToolRenderOutput`（含 `displayName`）、
`template_json`、`summary_key`；`AgentxxToolRenderInput` 与
`AGENTXX_TOOL_RENDER_SPEC_BASELINE_SIZE`（= `sizeof`）不变。

### 2.3 注册表与校验（`client_plugin_manager.h/.cpp`）

```cpp
struct ClientToolRenderReg {
    std::string plugin;
    std::string toolName;
    std::string displayName;                       ///< 归一后恒非空
    AgentxxToolCollapsedRenderFn collapsedFn = nullptr;
    AgentxxToolExpandedRenderFn  expandedFn  = nullptr;
    void* userData = nullptr;                      ///< 两个回调共用
    std::shared_ptr<ClientToolRendererLease> lease;
    bool builtin = false;
};
```

注册校验：

| 输入 | 结果 |
|---|---|
| `version != 1` / `struct_size` 不合法 / `tool_name` 为空 | `AGENTXX_RESULT_ERR_ARG` |
| `display_name` 为空 | 归一为 `tool_name`（不报错） |
| 两个回调都为 NULL | 允许 = 只改名 |
| 任一回调非 NULL | 按对应部分注册（未给的那部分走默认渲染） |

- 字符串字段在注册时立刻拷贝（与现有 `tool_name` 一致）；
- 没有 JSON 解析、没有 1 MiB 校验（原 `template_json` 路径整段删除）；
- `registerBuiltinToolRenderer(toolName, displayName, collapsedFn, expandedFn, userData)`
  —— 内置项 `Store` / `Subagent` 只给折叠内容。

### 2.4 渲染查询（两部分，各自缓存）

缓存按"部分"分条：`ClientToolRenderRequest` 增加 `part`（`Collapsed` / `Expanded`），
缓存键为 `keyFor(toolCallId, toolName) + "#c" / "#e"`，`inputHash`（同一组输入特征）/ `version` /
`beginRequest` 全部按部分独立 —— 折叠内容在结果到达时失效重算，展开体不受影响；反过来也一样。

```cpp
struct ClientToolCollapsedResult {
    std::string text;              ///< 折叠内容 (不含 " · "; 空 = 无内容)
    bool        pending = false;   ///< 自定义内容还在 client io 线程算 (text 可能是上一份)
    std::string pendingPlugin;     ///< 诊断
    bool        isDecor = false;   ///< text 来自 decor.summary
};

struct ClientToolExpandedResult {
    utilxx_base::Json items = utilxx_base::Json::array();
    bool              pending = false;     ///< 同上 (items 可能是上一份)
    std::string       pendingPlugin;
    bool              isDecor = false;     ///< items 来自 decor.items
    std::string       decorPlugin;          ///< items 里按钮 owner=tool_call_id 的归属
    std::string       decorToolCallId;
};

ClientToolCollapsedResult renderClientToolCollapsed(reg, cache, callId, toolName,
                                                    args, result, finished, error, maxWidth);
ClientToolExpandedResult  renderClientToolExpanded (reg, cache, callId, toolName,
                                                    args, result, finished, error, maxWidth);
```

判定口径：`pending == false` 且内容为空 = 没注册或回调返回 null → 默认渲染；
`pending == true` = 正在算，此时空内容留空、有内容就沿用上一份。

折叠内容查询流程：

```
decor 命中且 decor.summary 非空 -> text = decor.summary, isDecor = true
否则注册项有 collapsedFn:
    缓存命中 -> text = entry.text        (空 = 回调返回 null -> 默认渲染)
    未命中   -> pending = true; text = lookupLatest(同调用同部分) 的 text
                (有 toolCallId 且归属插件一致才取; 没有上一份就为空)
否则     -> 无自定义 (pending = false)
```

展开内容查询流程：

```
decor 命中且 decor.items 非空 -> items = decor.items, isDecor = true
否则注册项有 expandedFn:
    缓存命中 -> items = entry.items       (空 = 回调返回 null -> 默认渲染)
    未命中   -> pending = true; items = lookupLatest(同调用同部分) 的 items (没有就为空)
否则     -> 无自定义 (pending = false)
```

- 过渡一律沿用"同一调用同一部分的上一份结果"（结果到达 / 宽度变化使输入特征变化的那一帧
  继续显示屏幕上已有内容），**不用占位文字顶替**：`pending` 且没有上一份时，折叠不画
  ` · `、展开留空；
- 只有"展开态"的行才调用 `renderClientToolExpanded`（这是拆分的主要收益：
  Edit 的 diff、Plan 的图不会为折叠行计算）；
- `pendingPlugin` 保留（诊断/测试）；`matched` 字段删除（调用方不再需要）；
- `ClientToolRenderEntry`：删除 `displayName`，字段为
  `key/plugin/generation/inputHash/matched/text/items`（按部分只有一侧非空；
  `matched` = 回调有返回内容，`false` 即 null）。

显示名查询（同步，不触发请求）：

```cpp
/// 生效显示名: decor 的 displayName > 注册表注册名; 都为空返回空 (调用方回退原始工具名)
/// - toolCallId 为空时只查注册表 (占位行/中断分段没有调用 id)
/// - 返回 string_view 指向注册表快照内部 (快照存活期内有效)
std::string_view toolDisplayNameFor(const ClientUiRegistry* reg,
                                   std::string_view toolCallId,
                                   std::string_view toolName);
```

### 2.5 TUI 改动（`client/src/io/tui/components/message_list.{h,cpp}`）

| 位置 | 改动 |
|---|---|
| `toolDisplayNameOf` | 改为 `plugin::toolDisplayNameFor` 查表；未命中返回原始 `toolName`；不再投递请求、不再有空串三态 |
| `genPlaceholderToolDisplayName` | 不变（沿用上面），注释更新 |
| 折叠头 | 固定骨架 `+ [工具] <name> · <内容>`，`<内容>` 按 §2.1 的判定顺序；`pending` 且无上一份内容时不画 ` · `；删除 `starts_with(" ·")` 嗅探与"运行中显示参数预览" |
| 展开头 | 固定 `- [工具] <name> <耗时>`（原来只有 decor 才换名） |
| 展开体 | `pending` 且无上一份 → 首行之后留空；items 非空且非失败 → 自定义渲染；否则默认展开体（参数 JSON + 下一行 运行中 `正在调用工具...` / 已完成 结果）；删掉 `tool.running` 文案（默认展开体的运行中行改用 `msg.genToolCalling`） |
| `queryToolRender` | 拆成 `queryToolCollapsed` / `queryToolExpanded`（后者仅在展开态调用） |
| `itemKey` | 折叠态计入 `version(collapsedKey)` + 显示名哈希；展开态再加 `version(expandedKey)`；decor 版本照旧 |
| `agent_tui.cpp` `prefetchInterruptToolNames` | 删除（函数 + 调用点）：显示名不再需要预取 |
| `interrupt_view.h` `ToolDisplayNameFn` 注释 | 去掉"返回空串 = 未就绪、该段留空"的约定 |

折叠内容的截断：渲染回调拿到 `max_width` 可自行缩短；主程序仍按剩余列宽
（`collapsedPreviewBudget`）截断一次，保证一行内。

占位行（`GenPhase::ToolCall`）是单行、**不可点击、不可展开**的瞬态项：
`{运行中符号} [工具] <name> · 正在调用工具...`（工具名尚未解析出来时不画 ` · `）。
它按同一套骨架排版（角色标签、名字高亮、提示文字弱化），但从占位到工具行会多一个 `+`/`-`
折叠标记与耗时列 —— 只看文字是连续的。

### 2.6 插件与内置渲染器

| 文件 | 改动 |
|---|---|
| `agentxx_filesystem` | `List`/`Write`/`Remove`：模版 → 折叠内容 lambda（取 `path`）；`Read`/`Glob`/`Grep`/`Move`/`Copy`：折叠内容 lambda；`Edit`：折叠内容 + 展开内容（diff items）；删 `out.displayName` 与所有前导 `" · "` |
| `agentxx_websearch` | `Search`/`Fetch`：折叠内容 lambda（取 `query` / `url`） |
| `agentxx_execute_command` | `Bash` ×2：折叠内容 lambda（取 `command`） |
| `agentxx_planning` | 一个函数拆成两个薄封装（共用 `buildPlanDecorParts`）：折叠内容 = todos 摘要，展开内容 = 图 + 待办 items；删 `out.displayName` |
| 内置 `share_store` / `subagent` | 注册时给 `Store` / `Subagent`，只注册折叠内容；文本去掉前导 `" · "` |

SDK（`plugin_kit.h`）：

```cpp
struct ToolCollapsedOutput { std::string text; };
struct ToolExpandedOutput  { utilxx_base::Json items = utilxx_base::Json::array(); };

/// 两个回调都可空; 空 = 该部分走默认渲染
struct ToolRenderers {
    std::function<void(const ToolRenderInput&, ToolCollapsedOutput&)> collapsed;
    std::function<void(const ToolRenderInput&, ToolExpandedOutput&)>  expanded;
};

int32_t registerToolRenderer(const PluginxxHost*, const AgentxxClientUiIface*,
                             std::string_view toolName, std::string_view displayName,
                             ToolRenderers renderers, shimStorage);

// ClientPluginBase
void registerRenderer(std::string_view tool, std::string_view displayName, ToolRenderers renderers);
```

- shim 持有一份 `ToolRenderers`，两个 trampoline 分别调用对应 `std::function`；
- 删除 `registerToolTemplate` / `ClientPluginBase::registerTemplate` / `ToolRenderOutput`。

## 3. 版本与兼容

- **接口代次保持 5**（尚未发布，本次改动并入）：结构体布局直接改，不写 `specHas`
  分支、不保留旧布局；
- `AgentxxToolRenderSpec.version` 保持 1；
- 插件必须重建：旧 DLL 声明同代次却按旧布局填 spec，新主程序会按新布局读
  （`collapsed_fn` 会读到旧 `render_fn` 之外的内存）—— 构建脚本重建 `exec/plugins`，
  实施记录里固定这一步；
- 不涉及线协议 / 会话库 / 展示历史 / FFI；cli 不声明 `agentxx.client.msg_decor`。

## 4. 清理清单（老逻辑与文档）

| 类别 | 删除内容 |
|---|---|
| C ABI | `AgentxxToolRenderFn`、`AgentxxToolRenderOutput`、`template_json`、`summary_key` |
| SDK | `ToolRenderOutput`、`registerToolTemplate`、`ClientPluginBase::registerTemplate`、模版 JSON 拼装 |
| 注册表 | `ClientToolRenderReg::templateJson/templateDisplayName/templateSummaryKey/templateSummaryTemplate`；注册路径的 JSON 解析与体积校验 |
| 渲染核心 | `ClientToolRenderResult`（换成两个部分结果）、`renderClientTool`（换成两个函数）、`truncateToolSummary`（只服务模版）、`lookupLatest("#工具名")` 与"归属插件一致才沿用显示名"分支、`ClientToolRenderEntry::displayName` |
| TUI | `starts_with(" ·")` 嗅探、"运行中显示参数预览"分支、`pendingRender` 分支里的"名称结果"逻辑、`queryToolRender` 的单一入口、两条 `#工具名` itemKey 版本项、`prefetchInterruptToolNames`、`tool.running` 文案及其 i18n 条目 |
| 约定 | 摘要字符串自带前导 `" · "`（所有插件/内置渲染器/TUI 一处不留） |
| 文档 | 各处"双轨机制 / 预设模版 / 名称结果 / 未就绪帧不显示工具名 / 展开头只有 decor 改名"的说明（见 §8） |

## 5. 分步实施

| 步骤 | 内容 | 主要文件 |
|---|---|---|
| 1 | C ABI + SDK：两个回调类型/输出、spec 字段、kit 注册封装 | `api/client_plugin_api.h`、`api/plugin_kit.h` |
| 2 | 注册表 + 两部分渲染查询 + 显示名 helper + 按部分缓存 | `plugin/client_plugin_manager.{h,cpp}` |
| 3 | 内置渲染器 + 4 个插件（模版改成折叠 lambda） | `builtin_tool_renderers.{h,cpp}`、`agentxx_filesystem.cpp`、`agentxx_websearch.cpp`、`agentxx_execute_command.cpp`、`agentxx_planning.cpp` |
| 4 | TUI：固定骨架、两部分查询、itemKey、删预取与嗅探 | `components/message_list.{h,cpp}`、`io/tui/agent_tui.cpp`、`components/interrupt_view.h` |
| 5 | 测试更新 + 新用例 | `test_plugin_runtime.cpp`、`test_client_plugins.cpp`、`test_tui_tool_header.cpp` |
| 6 | 文档 | `plugins.md` §9、`tui.md` §2.2/§2.14、`docs/en/design/plugins.md`、`AGENTS.md` |
| 7 | 编译 + 插件重建 + 跑测试 | 见 §6 |

## 6. 验证

1. 编译 `agent/script/windows_debug_build.bat`（只看 `error|warn|Built target`）。
2. `pwsh -File agent/script/build_plugins.ps1 agentxx_filesystem agentxx_planning agentxx_execute_command agentxx_websearch`，
   确认 `exec/plugins/*` 时间戳已更新。
3. 测试（`--log-level warn`）：
   - `plugin_runtime`：新布局偏移与 `sizeof`（64 字节）；
   - `client_plugins`：首帧折叠内容可用、`pending` 过渡（无上一份时留空）、返回 null 走默认渲染、
     部分独立缓存（结果到达只让折叠条目失效）、只注册折叠内容时展开体走默认渲染、
     内置渲染器、插件覆盖内置、装饰缺项回落注册项、注册校验；
   - `tui_tool_header`：占位行 `{符号} [工具] Read · 正在调用工具...`（不可点击）、
     折叠/展开的各判定分支（失败、自定义内容、`pending` 留空、无自定义走默认、已有结果）、
     展开首行显示名+耗时、默认展开体（参数 JSON + `正在调用工具...`/结果）、未知工具回退；
   - 回归：`plugin_sdk`、`tui_widget`、`tui_form`、`tui_ui_items`、`ui_items`。
4. 手工冒烟（TUI）：`agentxx_filesystem_read` / `agentxx_filesystem_edit` / `agentxx_planning`：
   占位行 `- [工具] Read · 正在调用工具...` → 工具行先只有名字、内容就位后出现 ` · [0, 100] /a.cpp`；
   展开态首行 `- [工具] Edit 00:03`，第二行起 diff；历史回溯一致；禁用插件后名字与内容回退默认渲染。

## 7. 风险与取舍

| 风险/取舍 | 说明与处理 |
|---|---|
| 显示名只能注册时给 | 每次调用改名/装饰用 `update_tool_decor`（优先级更高）。in-tree 渲染器都是固定名 |
| 两部分回调需要写两份 | 渲染逻辑共用的（planning）用薄封装调用同一构建函数；收益是折叠行不再计算展开体 |
| `pending` 期间留空 | 自定义内容还没算出来且没有上一份时，折叠只有名字、展开只有首行；不用默认内容顶替（否则会闪一下再换掉）。未注册或返回 null 才走默认渲染 |
| 折叠内容为纯文本 | 少掉富组件折叠头；折叠头必须是一行，纯文本是唯一能保证高度稳定的形态 |
| 块缓存键改口径 | 折叠/展开各自的版本 + 显示名哈希；插件禁用/启用后名字与内容回退能重建（测试覆盖） |
| 旧插件 DLL | 同代次但旧布局会误读 spec；固定"先改代码，再重建插件"的顺序 |
| 第三方插件 | 未发布，无兼容义务；SDK 签名变更在编译期直接报错 |

## 8. 文档更新

- `docs/zh-cn/design/plugins.md` §9「工具特化渲染架构」：按新的注册表与固定骨架重写
  第 1 条（`display_name` + 折叠/展开两个回调、折叠/展开判定顺序、SDK 示例）、
  第 2 条（内置渲染器）、第 6 条（删除"名称结果 / 未就绪帧 / 模版"说明，改为"名字同步、
  `pending` 留空不用默认内容顶替、返回 null 走默认渲染、展开内容按需计算"）；
- `docs/zh-cn/design/tui.md` §2.14（占位行格式与不可点击）与 §2.2（中断工具名分段
  不再预取）；
- `docs/en/design/plugins.md` 对应章节；
- `AGENTS.md` 补一条约定（显示名注册时声明；折叠/展开内容分开渲染；折叠内容不带 `" · "`）。

## 9. 明确不做

- 不把显示名放进线协议 / agent 侧工具注册表；
- 不允许 UI 线程调用渲染回调（异步模型不变）；
- 不为 `update_tool_decor` 增加静态名字段（它本来就是每次调用一份）；
- 不做富组件折叠头（一行文本）；
- 不改 cli / FFI / 会话库 / 展示历史格式。
