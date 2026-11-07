# 方案: 中断消息的折叠与摘要 (未处理恒展开 / 已处理自动折叠 + 权限卡片特化)

- 难度: B+ (动中断描述结构 + 客户端中断视图的渲染与命中; 不改 wire 消息类型)
- 类型: 功能 (展示优化) + 描述层字段追加
- 时间: 2026-11-07
- 状态: 设计完成, 待实施
- 代码依据: 主仓当前工作树 (`agent/lib` `agent/client` `agent/plugins/agentxx_filesystem`)
- 相关文档:
    - [tui.md](../../../docs/zh-cn/design/tui.md) §2.6 (中断表单渲染) / §2.16 (Tip 折叠) / §3.1 (命中区域)
    - [plugins.md](../../../docs/zh-cn/design/plugins.md) §9 (界面描述层) / §18 (工具特化渲染)
    - [wire-protocol-fields.md](../../../docs/zh-cn/design/wire-protocol-fields.md) (中断请求只带 argJson)

阅读路径: 只想知道"改成什么样"看 §0 / §3.1 / §4; 关注实现细节看 §2 / §3.2~§3.7;
评审关注点集中在 §3.2 (描述层字段) · §3.4 (工具显示名来源) · §3.5 (命中不抢控件) · §12 (待确认)。

目录: [§0 结论](#0-结论摘要) · [§1 目标与非目标](#1-目标与非目标) · [§2 现状核对](#2-现状核对) ·
[§3 设计](#3-设计) · [§4 JSON 形态](#4-json-形态与示例) · [§5 关键流程](#5-关键流程) ·
[§6 边界与失败模式](#6-边界与失败模式) · [§7 影响面与兼容性](#7-影响面与兼容性) ·
[§8 分步实施](#8-分步实施) · [§9 测试计划](#9-测试计划) · [§10 风险与备选](#10-风险与备选) ·
[§11 文档更新清单](#11-文档更新清单) · [§12 待确认](#12-待确认)

## 0. 结论摘要

现状: 中断消息 (`ViewMessage::Role::Interrupt`) 一旦进列表就**整份展开且不可折叠** ——
未处理的权限询问一次占 5~7 行; 处理完之后仍然占同样多行、外观也还是"待处理"
(`! [权限] ...` 头 + 目标 + 两个勾选 + 允许/拒绝按钮), 历史里堆几条就既看不清结论又占满屏幕。
另外两处细节没做: 权限分类 `filesystem_read` / `filesystem_write` 是原样英文标识符,
工具名是原始名 `agentxx_filesystem_write`, 而工具消息头部早就有特化显示名 (`Write`/`Read`)。

改法四条:

1. **标记列**: 未处理 `!` (error, bold); 已处理折叠 `+`、展开 `-` (hint)。
   与其它消息的折叠标记同列同时长 (1 列 + 1 空格), 视觉上屏幕仍是 `! [权限] ...`。
2. **形态由状态决定**: `Waiting` 恒展开且**没有折叠命中区** (点了也不折叠);
   `Confirmed` / `Canceled` / `Expired` 在状态落定的那一刻**自动折叠**成一行摘要,
   之后用户可点摘要行展开 (`-`) 看详情、再点收起 (`+`)。
3. **摘要行** = 标记 + 摘要分段 (权限: `[权限]` + 工具显示名 + 权限名) + ` · ` 目标/首行摘要
   + ` · ` 结果 (`已确认: ...` / `已取消` / `已过期`)。权限询问折叠后**只有摘要, 没有表单**。
4. **两处特化**: 权限名由 agent 侧预设给 i18n 键 (`读取文件` / `写入文件`), 客户端只查词表;
   工具名走客户端已有的工具特化渲染 (`renderClientTool` 的"名称查询", 缓存键 `#工具名`),
   描述里用一个 `kind: "tool"` 的分段标记出来。

一条贯穿: **客户端仍然零 permission 语义** —— 权限名、目标、摘要分段全部来自
`preset::permissionCard` 写进描述的数据; 客户端只多了一个"按工具名查显示名"的通用能力。

## 1. 目标与非目标

目标:

1. 未处理的中断消息**不可能被折叠**: 用户的注意力不会被"折叠成一个 +"的待办挡住。
2. 处理完成后**默认只占一行**, 且一眼能看出结论 (允许/拒绝/取消/过期 + 关键值)。
3. 已处理消息可展开回详情 (描述内容块), 但**不再渲染可交互控件**。
4. 权限询问的摘要里: 工具名用特化显示名 (`Write`), 权限名本地化 (`写入文件`), 带目标路径。
5. 描述层改动向后兼容: 老客户端忽略新增字段, 新客户端容忍缺失 (回退到通用摘要规则)。

非目标:

- **不做**等待态折叠 (按需求: 未处理必须展开)。
- **不做**折叠状态持久化: 中断消息是客户端本地行 (`local_interrupt_<id>`), 本来就不落盘,
  重连/整份同步后按描述重建。
- **不改** wire 消息类型 / FFI 版本 / `InterruptUi.version` (只在自由 JSON 里加可选字段)。
- **不改** 行式前端 (stdio CLI) 的问答流程: 它不渲染头行分段, 只打印内容块与控件问答
  (`agent/client/src/io/stdio/agent_stdio.cpp:357` 用 `interruptUiPlainText`)。
- **不做** 权限语义进客户端: 不在客户端判断"这个中断是权限询问"。
- **不重排** 描述里 `blocks` 的内容: 已处理展开态只做"跳过控件块"。

## 2. 现状核对

### 2.1 描述从哪来、怎么下发

- 权限询问卡片由 agent 侧预设生成: `preset::permissionCard(toolName, category, target)`
  (`agent/lib/src/middlewares/interrupt_presets.cpp:510`), 头行三段:
  权限标记 (键 `interrupt.permissionBadge`, error+bold)、工具名 (accent+bold)、
  `" " + category` (hint); 正文块: 目标行 (`• <target>`)、目录目标再补一行
  `interrupt.rememberDir`、`remember` 勾选、`fullAuth` 勾选、允许/拒绝按钮
  (`decision`, 默认 `"false"`)。
- 下发路径: `AgentIOBase::registerOnBus` 的 permission 处理器
  (`agent/lib/src/agent/io/agent_io.cpp:225` 起) 组 `InterruptHandleArg{name="permission",
  arg={category,target}, ui=permissionCard(...)}`, 交给 `handleInterrupt` → `WireInterruptRequest.argJson`。
- 权限分类字符串由 `PermissionMiddlewareHandle::defaultCategory(scope)`
  (`agent/lib/src/middlewares/permission.cpp:208`) 给出: `filesystem_write` / `filesystem_read`;
  插件可用 `ToolPermissionSpec::category` 声明自己的分类文本 (自由字符串)。

### 2.2 客户端本地行

- `TUIClientAgentIO::handleInterrupt` (`agent/client/src/io/tui/agent_tui.cpp:2955`) 解析
  `InterruptHandleArg`, 组一条**本地行**: `role = Interrupt`,
  `id = "local_interrupt_<wireId>"`, `interrupt->ui = handleArg.ui.toJson()`,
  `interruptStatus = Waiting`, 经 `pushLocalRowLocked` 入列表 (`:3014`)。
- 过期通知 `WireInterruptExpired` (`agent_tui.cpp:1875`) 把同 id 的 `Waiting` 行改成 `Expired`。
- 提交/取消在客户端 UI 线程完成 (`InterruptView::confirm` `:701` / `cancel` `:767`),
  状态改成 `Confirmed` (带 `interruptResult` 展示文本) / `Canceled`。

### 2.3 渲染 / 高度 / 缓存

- 渲染入口: `MessageListComponent::buildMessageBlock` 的 `Role::Interrupt` 分支
  (`agent/client/src/io/tui/components/message_list.cpp:2181`) → `InterruptView::build`
  (`interrupt_view.cpp:442`) = `buildHeader` (`:364`) + `layoutForm` (`:282`) 全量块。
- 非 `Waiting` 只渲染一行状态: `buildStatusLine` (`:390`) = `interrupt.header` 前缀 +
  `已确认: <result>` / `已取消` / `已过期`。**描述内容块在已处理态完全看不到。**
- 高度: `InterruptView::estimate` (`:483`) = 1 + 各行; 列表对不可见项的粗估
  (`message_list.cpp:1071`) = `roughRows(text)+5+1`。
- 缓存: 中断消息**恒不可缓存** (`message_list.cpp:1359`: `role != Interrupt` 才能缓存),
  因为控件命中区 (reflect) 要每帧重建。折叠后仍然不可缓存 (折叠行的 reflect 同理), 成本可忽略。
- itemKey 已包含 `m.collapsed` (`message_list.cpp:873`), 所以翻转折叠天然触发重建/重估高度。

### 2.4 折叠机制与为什么不能直接复用

- 可折叠消息登记在 `MessageListComponent::handleCollapsibleClick` (`:477`): 按上一帧
  `visibleBoxes()` 取整块消息区域, 命中即翻转 `msg.collapsed`。参与的角色写死在
  `:326`: `User / Event / Think / Tool / System / Tip` —— **Interrupt 不在内**。
- 点击顺序 (`MessageListComponent::handleClick`): decor → 附件 → **折叠** → 中断控件
  (`interruptView_.handleClick`)。所以如果把 Interrupt 直接加进折叠列表, 展开态点任何控件
  都会先被折叠判定吃掉。→ 折叠/展开的命中必须留在 `InterruptView` 内部, 且只覆盖"头行/摘要行"。

### 2.5 工具特化渲染 (上一轮判断的更正)

`agentxx_filesystem_*` 这些工具**是有特化渲染的**, 上一轮说"只有 planning 注册了渲染"是错的,
原因是我按 C API 名 (`register_tool_renderer` / `registerToolRenderer`) 搜, 而插件用的是
SDK 便捷封装 `ctx.registerTemplate` / `ctx.registerRenderer`
(`agent/lib/include/agentxx/plugin/api/plugin_kit.h:4236` / `:4600`, 内部就是 `register_tool_renderer`)。

`agent/plugins/agentxx_filesystem/agentxx_filesystem.cpp` 的 client 侧 `fsClientSetup` (`:820~983`)
为全部文件系统工具注册了渲染:

| 工具 | 注册方式 | 显示名 |
|---|---|---|
| `agentxx_filesystem_list` | `registerTemplate(..., "List", "path")` | `List` |
| `agentxx_filesystem_write` | `registerTemplate(..., "Write", "path")` | `Write` |
| `agentxx_filesystem_remove` | `registerTemplate(..., "Remove", "path")` | `Remove` |
| `agentxx_filesystem_read` | `registerRenderer` | `Read` |
| `agentxx_filesystem_glob` / `grep` / `edit` / `move` / `copy` | `registerRenderer` | 同名首字母大写 |

解析实现: `agentxx::plugin::renderClientTool` (`agent/lib/src/plugins/client_plugin_manager.cpp:4138`),
模版分支在 `:4306` (`displayName = templateDisplayName`, 与参数无关)。
TUI 侧入口: `queryToolRender` (`message_list.cpp:88`) + 缓存 `ClientToolRenderCache`。

**名称查询是现成能力**: `genPlaceholderToolDisplayName` (`message_list.cpp:1493`) 在
"生成占位提示"阶段就用 `toolCallId="" / args="" / result="" / width=0` 调一次
`queryToolRender`, 命中缓存键 `#工具名`, 未注册渲染器的工具返回空 → 调用方回退原始名。
本次直接复用这条路径。注意它的结果来自**后台渲染**(客户端 io 线程), 第一帧可能还没有值,
所以首帧回退原始工具名, 下一帧换成显示名 (与占位提示同一现象, 可接受)。

### 2.6 权限分类与文案

- `tui_i18n.cpp:277~295` 的中断文案: `interrupt.header` = `"! [Interrupt] "`,
  `interrupt.permissionBadge` = `"! [Permission] "`, `interrupt.confirmed` = `"Confirmed: {}"`,
  `interrupt.Canceled` / `interrupt.expired`, `interrupt.remember` / `.rememberDir` /
  `.fullAuth` / `.allow` / `.deny` 等。
- **权限分类没有 i18n**: `permissionCard` 把 `category` 原样拼进第三段 (`" " + category`),
  所以界面上看到的是 `filesystem_read` / `filesystem_write`。
- 现有测试断言: `test_tui_interrupt.cpp:304/311/314/923` 断言 `"! [中断] "` / `"! [权限] "`;
  `test_interrupt_bus.cpp:1140~1147` 断言头行三段的具体内容。

## 3. 设计

### 3.1 形态由状态决定

一条中断消息只有三种形态, 由 `interruptStatus` + `msg.collapsed` 决定:

| 状态 | 标记 | 内容 | 可折叠 |
|---|---|---|---|
| `Waiting` | `!` (error, bold) | 头行 + 描述全部内容块 + 控件 + 提交行 (与今天一致) | 否 (不登记折叠命中区) |
| `Confirmed`/`Canceled`/`Expired` + `collapsed` | `+` (hint) | 一行摘要 (见 §3.4) | 是 (点摘要行展开) |
| 同上 + `!collapsed` | `-` (hint) | 头行 + 结果文本 + 描述内容块 (跳过控件与提交行) | 是 (点头行收起) |

标记列 = 1 列字形 + 1 个空格, 与其它角色消息的 `+`/`-` 列对齐。

`Waiting` 态**不读** `msg.collapsed` (即使它被外部置 true 也按展开渲染), 从根上保证"未处理不可折叠"。

### 3.2 描述层新增 (agent → client, 全部可选)

`agent/lib/include/agentxx/middlewares/interrupt_ui.h`:

```cpp
/// 分段用途提示 (客户端可按它的含义特化渲染)
/// - 空: 普通文本
/// - "tool": 工具名 —— 客户端可用该工具的特化显示名替换 (如 agentxx_filesystem_write -> Write),
///   并在拿不到显示名时回退本段的字面文本
struct InterruptUiSegment {
    ...
    std::string kind;   // 新增
};

/// 折叠摘要 (已处理的中断消息默认只渲染它; 空 = 客户端按通用规则生成)
struct InterruptUiSummary {
    /// 摘要分段 (空 = 复用 header.segments)
    std::vector<InterruptUiSegment> segments;
    /// 目标 / 补充说明 (单行, 客户端按剩余列宽截断)
    std::string detail;
    /// detail 的 i18n 键 (优先)
    std::string detailKey;
};

struct InterruptUi {
    int version = 1;              // 不变: 只追加可选字段
    InterruptUiHeader header;
    InterruptUiSummary summary;   // 新增
    std::vector<InterruptUiBlock> blocks;
};
```

- `empty()` (`interrupt_ui.h` 内联) 判定加上 `summary` 一并为空。
- 分段拼接规则(**新增, 唯一实现放客户端**): 相邻两段都非空时, 若前一段不以空白结尾且
  后一段不以空白开头, 插入一个空格。这样 `"[权限] "` + `"Write"` + `"写入文件"` 渲染成
  `[权限] Write 写入文件`, 而生产方不必在"键解析结果"里预埋空格 (键解析会丢掉字面文本里的空格)。
  现有生产方的文案 (尾部自带空格) 语义不变。
- `toJson` 保持现有"空字段不写"的约定; `fromJson` 缺字段按默认值。

### 3.3 权限卡片特化 (`preset::permissionCard`)

```cpp
// 1) 权限标记: 去掉字面文本里的 "!" (标记列已经有一个), 键不变
badge.labelKey = "interrupt.permissionBadge";   // "[Permission] " / "[权限] "
badge.color    = "error";  badge.bold = true;

// 2) 工具名: 标成 tool 分段, 让客户端换成特化显示名
tool.text = std::string{toolName};  tool.kind = "tool";
tool.color = "accent";  tool.bold = true;

// 3) 权限名: 已知分类给 i18n 键, 未知分类 (插件自带文案) 只用字面文本
//    读/写只靠颜色区分: 不加粗 (加粗是全局排版语义, 不能当区分手段)
category.text     = std::string{category};
category.labelKey = permissionCategoryLabelKey(category);   // 见下
category.color    = isWriteCategory ? "thinking" : "hint";

// 4) 摘要: 目标路径放 detail, segments 留空 (复用头行三段)
ui.summary.detail = std::string{target};
```

`permissionCategoryLabelKey(category)` (新增, 定义在 `interrupt_presets.cpp` 的匿名命名空间,
对外可放 `interrupt_presets.h` 供其它生产者复用):

| category | labelKey | zh | en |
|---|---|---|---|
| `filesystem_read` | `interrupt.permission.read` | 读取文件 | Read files |
| `filesystem_write` | `interrupt.permission.write` | 写入文件 | Write files |
| 其它 (插件自定义) | 空 | 原样显示 | 原样显示 |

`isWriteCategory`: 分类以 `_write` 结尾 (或等于 `filesystem_write`) → 写类颜色 (`thinking`), 其余读类用
`hint`。判断只影响颜色, 不影响任何语义。

**禁止用加粗/方括号/背景色区分读与写**: 加粗在 TUI 里是统一的排版语义 (标题、运行态、强调),
拿字重表达业务分类会让两档都失去意义; 方括号/背景色会改变行内节奏, 在窄终端更容易挤坏折行。
收尾时需要专门核对这一点 (见 §9 的收尾核对用例)。

### 3.4 客户端渲染 (`InterruptView`)

**标记 + 头行 (`buildHeader`)**: 前面加标记列; 头行 `reflect` 包一层, 登记 `HitBox{kind=Fold}`
(仅已处理态)。分段渲染时:
- `kind == "tool"`: 按 §3.4.1 三态处理 (显示名 / 留空 / 原名), 颜色沿用分段声明。
- 其它: 与今天一致 (`resolveLabel(labelKey, text)`)。

**摘要行 (`buildSummaryLine`, 新增)**:
1. segments = `summary.segments` 非空 ? `summary.segments` : (`header.segments` 非空 ?
   `header.segments` : 默认一段 `interrupt.header`);
2. detail = `summary.detailKey` 解析结果 / `summary.detail`; 都空时**通用回退** =
   首个 `text`/`markdown` 块的正文首行 (去掉行首 `•`、`-`、`*`、`#` 与空白);
3. 结果后缀 = 与 `buildStatusLine` 同一份文案 (`interrupt.confirmed` / `.confirmedEmpty` /
   `interrupt.Canceled` / `interrupt.expired`), dim。**保留** (不看展开): 折叠行就是要能直接看出
   结论; 列宽不足时依次牺牲 "detail → 结果正文", 摘要分段不被裁;
4. 单行拼装: `标记 + 空格 + segments + " · " + detail + " · " + 结果`; 宽度预算按优先级分配:
   1) 摘要分段 (不裁); 2) 结果文本 (先截到其中状态词能放下, 再按剩余空间截正文);
   3) detail (最后压缩, 没空间就整段省略)。均用 `oneLinePreview` 按终端显示列宽截断
   (宽字符按 2 列), 整行恒 1 行;
5. 整行 `xflex_shrink`, `reflect` 包一层并登记 `HitBox{kind=Fold}`。

**已处理展开 (`-`)**: `标记 + 头行 + 两个空格 + 结果文本 (dim)`, 下面按 `blocks` 顺序渲染
**内容块** (text/markdown/diff/separator/gap/表格/树/…), **跳过 `control` 与 `submit`** ——
控件值已体现在结果文本里 (confirm 时按控件顺序拼 `label: value`), 不再重画可点控件。

**未处理**: 与今天完全一致 (头行 + 全量块), 只多一个 `!` 标记列。

#### 3.4.1 工具显示名是怎么拿到的

- `InterruptView` 加一个注入点:
  `using ToolDisplayNameFn = std::function<std::string(std::string_view)>;
   void setToolDisplayNameResolver(ToolDisplayNameFn fn);`
- `MessageListComponent` 构造时注入, 实现为 `renderClientTool`
  (`queryToolRender` 同一条路径, 见 §2.5) 的"只给工具名"查询。
- 只在渲染 `kind == "tool"` 分段时查一次 (一条消息每帧最多 1 次); 命中缓存后只是一次 map 查找。

**为什么会"首帧缺失"**: 自定义渲染器 (`registerRenderer`, 带 `renderFn`) 不得在 UI 线程执行 ——
UI 侧只读 `ClientToolRenderCache`, 未命中时投递 `ClientToolRenderRequest` 给 client io 线程去跑
插件回调, 本帧得到的是 `pendingRender = true`; 结果写入缓存后下一帧才能拿到显示名。
模板渲染器 (`registerTemplate`, 如 filesystem 的 List/Write/Remove) 不调插件代码, 同一帧就
`matched + displayName`, 不存在空窗。本机 TUI 常常是"UI 线程即 client io 线程", 请求会内联完成,
所以大多数情况也看不到空窗; 远程/异步布局才会出现。

**显示规则 (三态, 铁律: 绝不先显示原名再跳变)**:

| 查询结果 | 渲染 | 理由 |
|---|---|---|
| `pendingRender == true` | **该分段留空** (什么都不画) | 原名会在下一帧变成显示名, 属于跳变 |
| `matched == true` 且 `displayName` 非空 | 显示名 (如 `Write`) | 稳定结果 |
| 无渲染器 (`matched == false && !pendingRender`) 或渲染器未给名字 | 原始工具名 | 这两种情况结果不会再变, 不是跳变 |

**预取**: 本地行创建时 (`agent_tui.cpp` 的 `handleInterrupt`, 本身就在 client io 线程) 就从描述里
扫出所有 `kind == "tool"` 分段, 用"只给工具名"的请求投递一次
(`pluginManager_->requestToolRender`), 让首帧大概率已命中缓存; UI 侧仍按三态兜底。

- 未注入 resolver 的环境 (单测直接构造 `InterruptView`): 直接显示原始工具名 (等同于"无渲染器"一档)。
- `genPlaceholderToolDisplayName` 的现有行为 (结果未就绪时不显示原名) 与新规则一致, 不需改动。

### 3.5 命中与交互

- `InterruptView::HitBox` 加 `enum class Kind : uint8_t { Form, Fold }`, 默认 `Form`
  (现有控件命中不变; 测试里按 `controlId`/`blockIndex` 过滤的代码不受影响)。
- `build` 在折叠/展开两条路径上各登记一条 `Fold` 命中区 (行元素框 + 整行区域), box 经
  `shared_ptr` 持有并由 `buildMessageItem` 的 attachments 续命 (与现有控件 box 同一机制)。
- `handleClick` (`:519`) 顺序: **先查 Fold** (整块判定, 不看 region) → 命中即翻转
  `msg.collapsed` (`ctx_.state->mutate` + `mutableMessage`), 折叠时清 `activeMsg_`, 重绘返回 true;
  再走原有控件命中逻辑。
- 未处理态不登记 Fold → 点头行不折叠 (需求 1)。
- `handleKey` (`:620`): 折叠态/非 `Waiting` 直接返回 false (不会有"看不见的表单被回车提交")。
- 不把 `Interrupt` 加进 `MessageListComponent` 的可折叠角色表 (理由见 §2.4)。

### 3.6 高度估算与缓存

- `InterruptView::estimate` (`:483`): `Waiting` → 现状; 已处理且折叠 → 1;
  已处理且展开 → 1 (头行) + 内容块行数 (复用 `layoutForm` 但跳过 control/submit)。
- 列表粗估 (`message_list.cpp:1071`): `Waiting` → 现状公式; 已处理折叠 → 2 (内容 1 行 + 尾部空行);
  已处理展开 → 保持现状公式 (粗估, 进入视口后实测修正)。
- 缓存: 保持不可缓存 (折叠行也要每帧重建 reflect 框), 与今天一致。

### 3.7 自动折叠的触发点

| 位置 | 触发 | 动作 |
|---|---|---|
| `agent_tui.cpp:3005~3014` | 新建本地行 | `collapsed = false` (Waiting) |
| `InterruptView::confirm` (`:701`) | 提交成功 | 置 `Confirmed` 的同时 `collapsed = true` |
| `InterruptView::cancel` (`:767`) | 取消整个请求 | 同请求所有 `Waiting` 行置 `Canceled` + `collapsed = true` |
| `agent_tui.cpp:1875~1898` | `WireInterruptExpired` | 置 `Expired` 的同时 `collapsed = true` |

折叠由状态转移驱动、一次; 之后用户的点击是自由的 (不会被下一帧改回去)。

### 3.8 文案变更 (`agent/client/src/io/tui/tui_i18n.cpp`)

| 键 | 现在 | 改为 | 原因 |
|---|---|---|---|
| `interrupt.header` | `"! [Interrupt] "` / `"! [中断] "` | `"[Interrupt] "` / `"[中断] "` | `!` 交给标记列 |
| `interrupt.permissionBadge` | `"! [Permission] "` / `"! [权限] "` | `"[Permission] "` / `"[权限] "` | 同上 |
| `interrupt.noDescriptor` | `"! [Interrupt] missing UI descriptor"` | `"[Interrupt] missing UI descriptor"` | 同上 |
| `interrupt.permission.read` | — | 新增 `"Read files"` / `"读取文件"` | 权限名本地化 |
| `interrupt.permission.write` | — | 新增 `"Write files"` / `"写入文件"` | 同上 |

渲染结果与今天一致: 标记 `!` + 空格 + `[权限] ` = `! [权限] `, 所以
`test_tui_interrupt.cpp` 现有的 `"! [权限] "` / `"! [中断] "` 断言**仍然成立**
(待处理态)。

## 4. JSON 形态与示例

权限卡片 (未处理, `Waiting`) 的描述:

```json
{
  "version": 1,
  "header": {
    "segments": [
      {"labelKey": "interrupt.permissionBadge", "color": "error", "bold": true},
      {"text": "agentxx_filesystem_write", "kind": "tool", "color": "accent", "bold": true},
      {"text": "filesystem_write", "labelKey": "interrupt.permission.write",
       "color": "thinking"}
    ]
  },
  "summary": {"detail": "C:\\work\\a.txt"},
  "blocks": [
    {"kind": "text", "text": "• C:\\work\\a.txt", "color": "hint", "indent": 2, "wrap": true},
    {"kind": "gap", "lines": 1},
    {"kind": "control", "id": "remember", "control": "checkbox", "labelKey": "interrupt.remember"},
    {"kind": "control", "id": "fullAuth", "control": "checkbox", "labelKey": "interrupt.fullAuth"},
    {"kind": "gap", "lines": 1},
    {"kind": "control", "id": "decision", "control": "buttons", "commitOnPick": true,
     "defaultValue": "false",
     "options": [
       {"value": "true", "labelKey": "interrupt.allow"},
       {"value": "false", "labelKey": "interrupt.deny", "color": "error"}
     ]}
  ]
}
```

三种形态的屏幕结果 (zh):

```text
! [权限] Write 写入文件                     <- Waiting: 头行 (标记 !)
    • C:\work\a.txt
    ☐ 记住此选择
    ☐ 完全授权所有权限
    [ 允许 ] [ 拒绝 ]

+ [权限] Write 写入文件 · C:\work\a.txt · 已确认: decision: true    <- 处理完自动折叠

- [权限] Write 写入文件  已确认: decision: true                   <- 点摘要行展开
    • C:\work\a.txt
```

框架自带预设 (`inputForm` / `confirmCard`) 没写 `summary` 时的折叠行 (通用回退):

```text
+ [中断] · 是否继续执行重复的工具调用?
```

## 5. 关键流程

**提交 (允许)**

```text
用户点 [允许]
  └─ InterruptView::handleClick → 控件命中 (Form, decision=true, commitOnPick)
       └─ confirm(msgIndex)   (:701)
            ├─ validateForm / formValues
            ├─ 状态 → Confirmed, interruptResult = "decision: true, remember: false"
            ├─ msg.collapsed = true          <- 新增: 立即折叠成摘要行
            └─ sendSubmit → 客户端 io 线程 → WireInterruptResponse → agent
```

**过期**

```text
WireInterruptExpired
  └─ agent_tui.cpp:1875 同 id 的 Waiting 行 → Expired, collapsed = true   <- 新增
       └─ postRedraw → 下一帧渲染摘要行 (+ [权限] ... · C:\work\a.txt · 已过期)
```

**展开已处理消息**

```text
用户点摘要行 (Fold 命中)
  └─ InterruptView::handleClick → ctx_.state->mutate → msg.collapsed = false
       └─ itemKey 变化 → LazyScrollable 重建 → build() 走"已处理展开"分支 (-)
```

## 6. 边界与失败模式

- **描述缺失/非法**: 仍是诊断行 (不可交互), 标记 `!`; 不折叠。
- **没有 `summary`、也没有 `header.segments`、也没有内容块**: 折叠行退化为
  `+ [中断] · <结果文本>`, 不会渲染出空行 (detail 为空就只留结果)。
- **工具显示名未就绪**: 该分段留空 (**不写原名**, 见 §3.4.1), 下一帧拿到后补上;
  没有注册渲染器 (或渲染器没给名字) 的工具显示原名 —— 这两种结果不会再变, 不属于跳变。
  插件禁用/卸载后注册项消失, 下一次查询落到"无渲染器"档, 显示原名。
- **超长目标路径**: 按剩余列宽 `oneLinePreview` 截断 (`…`), 绝不折行 (折叠行恒 1 行)。
- **极窄终端**: segments 自己仍在 (可能被裁剪), detail/结果整段省略 (预算 <8 列)。
- **折叠行被点击时消息已不在列表** (本地条数上限清理/整份同步): `handleClick` 内
  `mi >= messages().size()` 直接返回 (与现有控件命中同一防御)。
- **`Waiting` 期间被外部置 `collapsed=true`** (理论上只有测试会这么做): 渲染与命中都按展开处理,
  下一次状态转移仍会把折叠值写成 true。
- **已处理展开态点击内容块**: 不登记控件命中区 → 只有点击头行才收起, 正文不可交互。

## 7. 影响面与兼容性

- **wire**: 不变。`ui` 是 `InterruptHandleArg` 里的自由 JSON
  (`WireInterruptRequest.argJson`), 新增字段对老客户端透明 (忽略), 新客户端对缺失字段有回退。
- **FFI / 嵌入主程序**: 自己渲染 `ui` 的实现忽略 `summary`/`kind` 即可, 行为回到今天。
- **stdio CLI**: 不渲染头行分段, 只打印 `interruptUiPlainText` 的内容块与控件问答 → 无变化
  (权限分类仍然只出现在 TUI 头行/摘要行里)。
- **缓存/内存**: 中断消息本来就不进渲染树缓存; 折叠行更小, 总占用只降不升。
- **性能**: 每帧多一次 `renderClientTool` 名称查询 (仅 `kind=tool` 分段, 命中缓存后是一次
  map 查找); 折叠行比原来的整份表单省下 `layoutForm` 的全部行渲染。
- **语义约束**: `MessageListComponent` 的可折叠角色表**不加** `Interrupt`;
  折叠/展开的命中只在 `InterruptView` 内部 (见 §2.4 / §3.5)。

## 8. 分步实施

1. `interrupt_ui.h/.cpp`: `InterruptUiSegment.kind`、`InterruptUiSummary`、`InterruptUi.summary`,
   `empty()` 纳入 summary, 解析/序列化 + 注释。
2. `interrupt_presets.h/.cpp`: `permissionCard` 三段特化 + `summary.detail` +
   `permissionCategoryLabelKey` 表; 更新头注释。
3. `tui_i18n.cpp`: §3.8 的 5 处文案。
4. `interrupt_view.h/.cpp`: 标记列、`buildSummaryLine`、已处理展开分支、`estimate`、
   `HitBox::Kind`、`handleClick`/`handleKey` 调整、工具显示名注入点 (含"未就绪留空"三态)。
5. `message_list.cpp`: 注入工具显示名 resolver (`genPlaceholderToolDisplayName` 抽出按名的版本);
   粗估高度按状态分支。
6. `agent_tui.cpp`: 新建行 `collapsed=false`; 过期时 `collapsed=true`;
   `handleInterrupt` 里对描述中 `kind==tool` 的分段预取一次显示名。
7. 测试更新/新增 (§9), 包含"权限名只用颜色区分"的收尾核对。
8. `docs/zh-cn/design/tui.md` §2.6 补一段; `index.md` 回填结论。

## 9. 测试计划

**core `interrupt_ui`** (`agent/test/core/test_interrupt_ui.cpp`)

1. `InterruptUiSummary` / `segment.kind` JSON 往返 (含"空字段不写")。
2. `permissionCard` 产出的三段: 权限标记键、工具段 `kind == "tool"`、权限段
   `labelKey == interrupt.permission.write|read` + 颜色 (`thinking` / `hint`);
   **断言两档分段的 `bold` 都是 false** (不用字重区分)。
3. 未知分类 (`"custom_scope"`) → `labelKey` 为空, 字面文本照原样。
4. `summary.detail == target`; 目录目标 (尾斜杠) 不变。

**core `interrupt_bus`** (`agent/test/core/test_interrupt_bus.cpp:1130~1150`)

5. 权限请求的 `ui` JSON 断言按新形态更新 (工具段 `kind`、权限段键、`summary.detail`),
   序列化-反序列化后一致。

**client `tui_interrupt`** (`agent/test/client/test_tui_interrupt.cpp`)

6. 未处理: 渲染含 `! [权限] `, 表单可见; 点击头行不改变 `msg.collapsed`;
   连点 `clickBlock(msg, 0)` (头行所在块) 也不折叠。
7. 提交 (点允许) 后: `msg.collapsed == true`; 渲染行数为 1 (加尾部空行 = 2);
   文本含 `+`, `[权限]`, 工具名 (`read_file` 或显示名), `读取文件`, 目标路径, `已确认`。
8. 点击摘要行 → `collapsed == false`; 渲染含 `-` 与目标行, **不含** `记住此选择`(控件已隐藏)。
9. 取消 (Esc/取消按钮) 后: `collapsed == true`, 文本含 `已取消`。
10. 过期 (构造 `Expired` 状态) → `已过期` + 折叠。
11. `estimate` 与实测行数一致 (折叠 = 1); 粗估分支不破坏滚动总高度 (沿用现有
    "估算 == 实测" 用例)。
12. 折叠态按回车不提交 (通道无消息、`interruptStatus` 不变)。
13. 工具显示名三态: resolver 返回显示名 → 渲染 `Write` 且不含原名; resolver 表示"渲染中" →
    该段留空 (**不能出现原名**); resolver 表示"无渲染器" → 显示原名 `read_file`。
14. **收尾核对** (逐条对照, 并在 work.md 记录):
    - 权限名两档只差颜色 (`read` → hint, `write` → `thinking`), 分段 `bold` 均为 false;
    - 折叠行包含结果后缀; 展开已处理消息不出现控件;
    - 未处理消息点击头行不折叠; 已处理消息点摘要行/头行能双向翻转;
    - 渲染中不出现"原名 → 显示名"的跳变。

**回归**: `ui_items` / `tui_ui_items` / `tui_form` / `tui_widget` / `tui_tool_header`
(工具渲染未被改动, 但 `genPlaceholderToolDisplayName` 会被抽成按名版本)。

## 10. 风险与备选

| 风险 | 影响 | 处理 |
|---|---|---|
| 折叠日志里看不到结论 | 用户要展开才知道允许/拒绝 | 摘要行尾部带结果后缀 (§3.4); 宽度不足时依次牺牲 detail → 结果正文, 摘要分段不裁 |
| 工具显示名首帧未就绪 | 该段先空一帧再出现显示名 | §3.4.1 三态规则: **未就绪就不显示原名**; 创建时预取缩短空窗; 宁可空一帧也不跳变 |
| 显示名是插件写死的英文 (`Write`) | 中文界面仍显示英文 | 与工具消息头一致; 若要中文化需给 `AgentxxToolRenderOutput.displayName` 加键 (本期不做) |
| 头行文案去掉 `!` 影响截图/文档 | 视觉不变 (`!` 由标记列提供) | 只改字符串, 屏幕输出一致; 相关测试断言已复核 |
| 已处理展开不显示控件 | 用户想复核勾选值 | 结果文本已含每个控件的 `label: value`; 若需要只读控件见 §12 |

备选方案 (**不采用**):

- 把折叠状态写进描述 (`summary.collapsedByDefault`): 现在形态完全由 `interruptStatus` 决定,
  不需要额外开关; 描述里再放一个布尔值只会多一处可能互相矛盾的状态源。
- 把 Interrupt 加进 `handleCollapsibleClick`: 会抢掉展开态的控件点击 (§2.4)。
- 客户端按 `category` 前缀判断权限类型做特化: 违反"客户端零 permission 语义", 不采用。

## 11. 文档更新清单

1. `docs/zh-cn/design/tui.md` §2.6: 中断表单段落追加"三种形态 + 标记列 + 摘要来源 + 折叠命中只在
   头行/摘要行"四句, 并说明权限名与工具显示名的来源。
2. `agent/lib/include/agentxx/middlewares/interrupt_ui.h`: `summary` / `kind` 的用途与回退规则。
3. `agent/lib/include/agentxx/middlewares/interrupt_presets.h`: `permissionCard` 的头注释
   (三段语义 + `summary.detail` + 分类键表)。
4. `agent/client/include/agentxx-client/io/tui/components/interrupt_view.h`: 类注释补形态表与
   命中说明 (含"未处理不可折叠")。
5. `resource/history/interrupt-collapse-summary/index.md` 结论 + `work.md` 逐项记录。

## 12. 待确认 / 已定规则

已定 (按评审意见, 实施时不得偏离):

1. **折叠行保留结果后缀**: dim, 列宽不足时依次牺牲"detail → 结果正文", 摘要分段不被裁。
2. **权限名只用颜色区分**: `filesystem_read` → `hint`, `filesystem_write` → `thinking`;
   不加粗 (加粗是 TUI 的全局排版语义)、不加方括号、不加背景色。
3. **工具显示名未就绪时该段留空**, 绝不先显示原名再跳变; 无渲染器/渲染器未给名字 → 原名
   (结果不会再变)。

仍待确认:

4. 已处理展开态是否需要"只读控件" (默认不要: 值已在结果文本里; 若需要再补)。
