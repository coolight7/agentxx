# 客户端 UI 能力扩展设计（宿主机制 + 组件库分层）—— 设计实现方案

- 状态: 设计定稿（待实施）
- 需求来源:
  - 插件 UI 目前只能"按宿主预设 schema 组合 items"，布局/绘制/尺寸/输入/时间/视图级能力缺失
  - 本版目标: **扩展各方面接口能力**（把缺口补齐），采用"**机制在宿主、组件库在插件 SDK**"的分层，并把该分层与组件库**复用到中断渲染等所有 UI 描述路径**（不局限于插件框架）
  - **完全自绘（canvas）本版只做类型预留**，渲染实现与交互细节留待后续单独设计
- 关联代码（现状，按引用密集度排序）:
  - `agent/lib/include/agentxx/plugin/api/client_plugin_api.h`（`agentxx.client.ui` 表 + `AgentxxOverlaySpec/Type` + 工具渲染器 spec）
  - `agent/lib/include/agentxx/plugin/client_plugin_manager.h`（`ClientUiRegistry` / `dispatchAction` / `ClientToolRenderCache` / `PluginUiAdapter`）
  - `agent/client/include/agentxx-client/io/tui/ui_items_render.h`、`agent/client/src/io/tui/ui_items_render.cpp`（现有 items 行模型与渲染/测量）
  - `agent/client/src/io/tui/agent_tui.cpp`（`renderPluginPanel` 等）、`agent/client/src/io/tui/tui_sidebar_content.cpp`（`appendPluginItems`）
  - `agent/client/src/io/tui/components/{message_list,overlays,interrupt_view}.cpp`、`agent/client/include/agentxx-client/io/tui/plugin_ui_items.h`
  - `agent/client/include/agentxx-client/io/tui/framework/ui_hit.h`、`agent/client/{include,src}/io/tui/{scrollable.h,scrollable.cpp}`
  - `agent/lib/include/agentxx/middlewares/interrupt_ui.h`、`agent/lib/src/middlewares/interrupt_ui.cpp`、`interrupt_presets.h`
  - `agent/lib/include/agentxx/plugin/api/plugin_kit.h`（SDK `ClientPluginBase` 等）
  - `agent/lib/include/agentxx/plugin/plugin_interfaces.h`、`agent/client/include/agentxx-client/io/tui/tui_plugin_adapter.h`、`stdio/cli_plugin_adapter.h`
- 关联文档: `docs/zh-cn/design/index.md`（客户端 UI 节）、`docs/zh-cn/design/plugins.md`（§9 客户端接口表 / 工具特化渲染）、`docs/zh-cn/design/tui.md`

---

## 1. 目标与范围

### 1.1 本版要解决的问题（缺口 → 能力）

| 缺口 | 现状 | 本版方案 | 归属层 |
|---|---|---|---|
| G1 布局 | items 只能纵向顺序堆叠 | 新增组合类组件 `row`（含宽度权重/对齐）、`box`（标题+边框+内边距）、`collapse`；items 支持嵌套 | 描述层 + 机制层 |
| G2 绘制（非自绘部分） | 只有 `progress`（10 格 `#/-`）与固定配色文本 | 新增 `sparkline`、`meter`（阈值配色/宽高可控）、`table`（列对齐/表头/斑马纹）、`kv`；`box` 提供边框绘制 | 描述层 + 机制层 |
| G3 尺寸感知 | 只有工具渲染器有 `max_width`；面板/Info/decor 拿不到可用宽度 | 新增区域尺寸事件 `EVT_UI_LAYOUT` + 注册表内尺寸快照；SDK 便捷查询 | 机制层 |
| G4 状态与输入 | 只有按钮点击；插件无法拥有选中/输入/勾选态 | **把中断的控件块（buttons/select/text/number/checkbox/submit）提升为共享组件**：插件面板/overlay 也可用，宿主管状态与输入，结果经既有 action 通道回传；新增插件快捷键表 | 描述层 + 机制层 |
| G5 时间 | client 插件无定时器/周期回调 | 新增 `agentxx.client.timer` 表（一次性/周期），按可见性与动画等级门控，卸载/禁用自动清理 | 机制层（新表） |
| G6 视图级 | overlay 只有 4 种预设，尺寸不可控；`custom` 块未实现 | overlay 选项（尺寸/滚动/标题栏）、`custom` 块派发到共享组件库（含 `fallback` 降级）；全屏自绘视图留待 canvas | 描述层 + 机制层 |
| G7 组合与复用 | items 不可嵌套/复用；渲染实现分散在 3 处 | 收敛为**唯一实现** `ui_components`；组件库落到 lib/SDK，插件与中断描述共用同一套构建器与渲染 | 机制层 + 组件库 |

### 1.2 明确不做（本版边界）

1. **不做完全自绘**: `canvas` 仅作为 schema 类型预留（解析 + 往返保留 + 降级为纯文本/占位），渲染、命中、输入与定时器专章留待后续设计。
2. 不做插件代码在 UI 线程执行（保持"UI 线程只读宿主快照"这一不变量）。
3. 不做任意绝对定位/图元 API（属于 canvas 范畴）。
4. 不改 wire 协议（客户端插件在 client 进程内，UI 描述不经网络；agent 侧插件经 `WirePluginData` 把数据交给同名 client 插件，路径不变）。
5. 不改 `agentxx.client.ui` 表结构（原因见 §8.1）。

### 1.3 设计原则

1. **机制在宿主、组件库在 SDK**: 宿主提供"描述解析 → 布局测量 → 渲染 → 命中 → 事件/尺寸/时间"机制；具体组件语义由数据描述表达，插件的易用性由 SDK 构建器（C++）提供，宿主不为某个新组件改代码。
2. **单一实现**: 同一 kind 的测量与渲染只有一份代码；所有接入点（面板/Info/装饰/overlay/中断）复用同一实现，禁止再次出现"每处一份 switch"。
3. **复用既有不变量**: 命中用 `UiHitRegistry`（帧首清空+渲染登记）、结果派发用 `dispatchAction`（owner/generation 复查）、插件回调只在 client io 线程、注册信息 COW 快照。
4. **零 ABI 变更优先**: 能力扩展优先走 JSON 数据层（新增 kind / 选项），只有"需要宿主主动回调插件"的能力（定时器、快捷键）才新增接口表。
5. **优雅降级**: 未知/不支持的 kind 有 `fallback` 文本；行式前端（CLI/FFI）有纯文本降级；老宿主按能力名协商失败即降级（不报错、不静默丢内容）。
6. **可测**: 组件渲染器为纯函数式（描述 + 宽度 → 行模型），可做屏幕单元格断言测试，不依赖真实终端。

---

## 2. 现状勘察（设计输入）

### 2.1 现状链路

| 环节 | 现状实现 | 关键约束 |
|---|---|---|
| 数据形态 | `items` JSON 数组（`kind`: text/markdown/diff/separator/gap/button/progress/badge/diagram） | JSON 是唯一跨边界形态 → **新增 kind 不需要 ABI 变更** |
| 注册入口 | `register_panel/update_panel`、`register_info_section/update_info_section`、`update_tool_decor`、`register_tool_renderer(spec.items_json)`、`open_overlay(CUSTOM)` | 全部为 JSON 字符串，新 kind 自动可用 |
| UI 注册表 | `ClientUiRegistry`（COW 快照，`uiRegistrySnapshot()` 短锁拷贝） | io 线程写、UI 线程只读；渲染不得进入插件代码 |
| 渲染实现 | `ui_items_render.{h,cpp}`（行模型：`UiRow{element, lines, box, hitId, hitArgs, hitOwner, hitSub}`；`measureUiItem` 与 `renderUiItem` 同源） | 已是正确的收敛形态，但只有 3 个接入点用了它 |
| 渲染漂移 | `renderPluginPanel`（`agent_tui.cpp:283`）、`appendPluginItems`（`tui_sidebar_content.cpp`）各自一份 switch，且面板缺 markdown/diff/diagram | 必须收敛，否则新 kind 要在 3 处实现 |
| 命中 | `UiHitRegistry<Payload>`（`beginFrame` 帧首清空 + `add()` 附 `reflect` + `findClick`）；消息列表另用 `decorHits_`（`DecorHitBox`） | 滚动容器内子项不可依赖 `reflect`（视口外子项残留测量大框 → 幽灵命中），应按 `visibleBoxes()` 映射 |
| 派发 | UI 线程 → `ClientPluginManager::dispatchAction(plugin, ownerId, actionId, argsJson, generation)` → io 线程二次校验（存在/启用/代次/绑定） → `InflightGuard` 后直调 | 现成的"UI 事件 → 插件回调"通道，应复用 |
| 插件回调线程 | 自定义工具渲染器只在 client io 线程执行，结果写入 `ClientToolRenderCache`（版本号参与消息块缓存 key，上限 512） | 任何新的插件回调都必须遵守同一模型 |
| 缓存 | `MessageListComponent` 对含命中区/动画的消息置 `cacheable=false` | 带可点组件的消息每帧重建（成本可接受，仅少数消息） |
| 尺寸信息 | 仅 `AgentxxToolRenderInput.max_width` 一处 | 需要补全到面板/Info/decor/overlay |
| 滚动 | `Scrollable`/`LazyScrollable`：Pass1 用"测量用临时大框"（`Box{0, contentWidth-1, 0, kTallHeight}`），Pass2 只对可见子项重定位；`visibleBoxes()` 暴露可见区域 | 命中映射的正确来源；需要补 `hitTestItem(x,y)` 级别的 helper |
| 中断渲染 | `InterruptView` 自行处理控件块（`ControlState`）+ 命中（块下标 + 控件 id + 子序号），内容块走 `ui_items_render`；`custom` 块只打 `fallback` | 控件块是"宿主管状态、结果回传"的既有范式，应提升为共享组件 |
| 能力协商 | `plugin.yaml interfaces.{require,optional}` + `TuiPluginAdapter::supportedInterfaces()` + `EVT_READY.interfaces` / `get_client_state().interfaces` | 新能力用能力名声明即可，不需要改表 |
| 前端差异 | TUI 声明 `agentxx.client.ui` 全量（含 msg_decor/action/overlay）；CLI 仅 `toast`/`command` | CLI 需纯文本降级（`interruptUiPlainText` 已有先例） |

### 2.2 关键结论（直接影响设计）

1. **新增 kind = 零 ABI 变更**（数据层），但老宿主会静默忽略未知 kind → 新组件需支持 `fallback` 兜底，且插件应通过 `plugin.yaml interfaces` + `get_client_state().interfaces` 做能力判断。
2. **不能原地扩展 `agentxx.client.ui` 表**：SDK 侧 `pluginxx::kit::validateInterface()` 的判定是 `version != 1 || struct_size < sizeof(Iface)` → 老宿主对"更大的结构体"会**整表判为不可用**（面板/状态栏一起丢）。新增"宿主主动回调插件"的能力必须**新增接口表**（文档既有约定："新增能力 = 新增接口表或表内追加成员并递增该表版本"）。
3. **UI 线程不得进入插件代码**：一切插件回调走 io 线程 + 缓存 + 通知重绘；因此本版的"输入"能力用**宿主渲染的控件**承载（插件只收结果），而不是把按键事件透传给插件（后者属于 canvas/自绘阶段）。
4. **命中必须"每帧登记"**：控件、按钮、画布内子区域都要在渲染时登记；滚动容器内用 `visibleBoxes()` 映射（新增 `Scrollable::hitTestItem`）。
5. **结果通道可完全复用**：表单提交/控件即时事件都可以表示为既有 action 派发（`ownerId` + `actionId` + `argsJson`），无需新 API。
6. **尺寸感知可用"新事件"实现**（`agentxx.client.events` 枚举追加值）：旧宿主对新事件值返回订阅失败（NULL）→ 插件降级，不破坏兼容。
7. **定时器/快捷键需要"宿主主动回调"** → 只能用新表（`agentxx.client.timer` / `agentxx.client.keybind`；后者能力名在 `plugin_interfaces.h` 中已预留 `agentxx.client.keybind`）。
8. **能力复用面比插件框架更大**：中断描述（agent 侧生产、client 渲染）、工具装饰、overlay、状态栏/Info、CLI/FFI 纯文本、测试/基准 —— 组件层必须放在**不依赖 FTXUI 的位置**（lib），渲染实现放在 client。

### 2.3 现有可复用件清单（避免重造）

- `UiHitRegistry` / `kNoBox` / `UiHitMap`（命中登记）
- `Scrollable` / `LazyScrollable`（滚动 + 可见区域）；`scroll_common.h::layoutAndMeasure`（元素测量）
- `PluginButtonDesc` / `parsePluginButton` / `renderPluginButton` / `uiRoleColor` / `renderPluginDiff`（样式与语义收敛点）
- `markdown` 渲染（`renderMarkdown` + `estimateMarkdownLines`）、mermaid 状态图（`parseMermaidStateDiagram`/`renderMermaidStateDiagram`）
- `TUITheme`（语义色）、`TuiI18n::tr/trf`（文案）
- `ClientToolRenderCache`（插件回调结果缓存 + 版本号）、`instanceGenerations`（代次复查）
- `interruptView` 的控件状态与结果契约（`{"values":{id:value}}` + `interruptValueXxx` 读取器）
- `interruptUiPlainText`（纯文本降级的既有范式）

---

## 3. 总体设计

### 3.1 三层结构

```
┌─────────────────────────────── 描述层 (数据, 零 ABI) ───────────────────────────────┐
│  agentxx.ui.item schema (JSON)                                                     │
│   ├─ 插件 items       (panel / info section / tool decor / tool renderer / overlay) │
│   ├─ 中断描述 blocks  (agent 侧生产 → client 渲染; 控件 + 结果契约)                 │
│   └─ 行式前端降级     (CLI / FFI / 日志: ui::plainText)                             │
└────────────────────────────────────────────────────────────────────────────────────┘
                                   │ 解析一次 (lib: agentxx::ui::Item)
                                   ▼
┌────────────────────────────── 机制层 (宿主 lib + client) ───────────────────────────┐
│  解析/校验 → 布局测量 → 渲染(行模型) → 命中登记 → 事件派发 → 尺寸/时间门控           │
│   lib:   agentxx/ui/{item.h,build.h}          (schema、构建器、纯文本降级)           │
│   client: io/tui/ui_components.{h,cpp}        (唯一渲染实现: 测量与渲染同源)         │
│           framework/ui_hit.h + scrollable.*   (子区域命中 / visibleBoxes 映射)       │
│           components/{interrupt_view,overlays,message_list}.cpp (接入点)            │
│   lib:   plugin/client_plugin_manager.*       (注册表、派发、尺寸快照、定时器)       │
└────────────────────────────────────────────────────────────────────────────────────┘
                                   │ 组件库 (构建器 + 组合函数)
                                   ▼
┌──────────────────────────── 组件库 (SDK, 插件/agent 侧共用) ────────────────────────┐
│  agentxx::ui::items::{text,row,box,table,tree,kv,sparkline,meter,control,submit,...}│
│  plugin_kit.h: ClientPluginBase::setPanelItems/updateDecor/showOverlay/form(...)     │
│  中断预设 (interrupt_presets.h) 用同一套构建器生成 blocks                            │
└────────────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 数据流

```
插件(io 线程) --JSON--> ClientPluginManager(io 线程)
      ├─ 解析为 agentxx::ui::Item 树 (一次)  ── 存入 UI 注册表 (COW 快照)
      └─ 能力名/尺寸/定时器 元数据
UI 线程 --快照--> ui_components 渲染 (测量=渲染同源) --> ftxui Element + 命中登记
用户交互(点击/键盘) --UI 线程--> 命中查询 --> dispatchAction / 表单提交
      └─ io 线程: 代次复查 --> 插件回调 (InflightGuard) --> 插件再次推送新 items
尺寸变化(终端/侧栏拖拽) --UI 线程测量--> 快照写回 --> EVT_UI_LAYOUT(io 线程投递) --> 插件重排
定时器(宿主计时器) --io 线程--> 插件回调 --> 插件推送新 items
```

### 3.3 归属与代码位置

| 新增/改动 | 位置 | 说明 |
|---|---|---|
| schema + 解析/校验/序列化 + 纯文本降级 | 新增 `agent/lib/include/agentxx/ui/item.h`、`agent/lib/src/ui/item.cpp` | 不依赖 FTXUI；CLI/FFI/TUI/SDK/中断共用 |
| 组件构建器 | 新增 `agent/lib/include/agentxx/ui/build.h`（header-only） | 插件与 agent 侧中断预设共用 |
| 唯一渲染实现 | 新增 `agent/client/src/io/tui/ui_components.cpp`（+ `include/agentxx-client/io/tui/ui_components.h`） | 由现有 `ui_items_render.{h,cpp}` 演进（保留文件名兼容或直接更名并同步引用） |
| 命中（子区域/滚动映射） | `framework/ui_hit.h`、`scrollable.{h,cpp}` | 新增子区域命中与 `hitTestItem` |
| 接入点收敛 | `agent_tui.cpp`、`tui_sidebar_content.cpp`、`message_list.cpp`、`overlays.cpp`、`interrupt_view.cpp` | 全部改为调用 `ui_components` |
| 中断描述扩展 | `middlewares/interrupt_ui.{h,cpp}` | 内容块支持新组件（`props` 透传复用）；纯文本降级走 `ui::plainText` |
| 管理器能力 | `plugin/client_plugin_manager.{h,cpp}` | 尺寸快照/事件、定时器、快捷键、表单提交派发 |
| 适配器 | `tui_plugin_adapter.h`、`stdio/cli_plugin_adapter.h` | 新能力名声明；CLI 纯文本降级 |
| 接口契约 | `plugin/api/client_plugin_api.h`、`plugin/plugin_interfaces.h` | 新表（timer/keybind）+ 新能力名 |
| SDK 便捷层 | `plugin/api/plugin_kit.h` | `ClientPluginBase` 方法 + 构建器封装 |
| 测试 | `agent/test/{include/agentxx-test,src}/...` | 新模块 `ui_items`、`tui_ui_items`，扩展 `tui_interrupt`/`tui_widget`/`plugin_sdk` |

> 构建侧：`agent/lib/CMakeLists.txt` 与 `agent/client/CMakeLists.txt` 均用
> `file(GLOB_RECURSE ... CONFIGURE_DEPENDS "src/*.cpp")` 收集源文件，**新增 .cpp 不需要改 CMake**；
> 头文件按目录约定放到 `agent/lib/include/agentxx/...` 即可被全部使用方看到
> （插件不链接 libagentxx，而是直接以 `${CMAKE_CURRENT_SOURCE_DIR}/../lib/include`
> 为搜索路径引用纯 C ABI 头与 SDK 头，见 `agent/plugins/CMakeLists.txt:118`），
> 因此 `agentxx/ui/build.h` 可被内置插件与 agent 侧代码同时复用。

---

## 4. 描述层：schema 扩展

### 4.1 通用字段（所有 kind 可用）

| 字段 | 类型 | 含义 |
|---|---|---|
| `kind` | string | 组件类型（缺省 `text`，保持兼容） |
| `id` | string | 组件标识（表单控件必填；其他可选，用于状态保持/诊断） |
| `indent` | int | 左侧缩进空格数（与上下文缩进叠加） |
| `color` / `role` | string | 语义色名（`normal/hint/accent/error/tool/thinking/user/assistant/system/...`）；`role` 兼容旧写法 |
| `bold` / `dim` | bool | 文本属性（`dim` 走 `TUITheme::dim()` 的主题分流） |
| `wrap` | bool | 是否按可用宽度折行（缺省：文本类 true，结构化类 false） |
| `when` | string | 条件显示表达式（预留：`"expanded"` 等宿主状态；本版仅保留字段） |
| `fallback` | string | 该组件不被宿主支持/渲染失败时的降级文本 |
| `action` / `args` | string / object | 点击派发（`action` 为 action id；`args` 原样回传） |

### 4.2 kind 全表

| kind | 状态 | 用途 |
|---|---|---|
| `text` / `markdown` / `diff` / `separator` / `gap` | 已有 | 文本与富文本块 |
| `button`（别名 `action`） | 已有 | 可点按钮（新增 `style: "solid\|text"` 支持文字按钮） |
| `progress` | 已有（保留） | 旧进度条 → 内部映射为 `meter` |
| `badge` | 已有 | 状态点 + 文本 |
| `diagram` | 已有 | mermaid 内联状态图 |
| **`row`** | 新增 | 横向组合（列宽权重、间距、对齐） |
| **`box`** | 新增 | 分组容器（标题、边框、内边距） |
| **`collapse`** | 新增 | 可折叠分组（宿主管展开状态） |
| **`table`** | 新增 | 列对齐表格（表头、单元格、省略号截断） |
| **`tree`** | 新增 | 层级列表（连接线、缩进、节点动作） |
| **`kv`** | 新增 | 键值对（信息面板最常用） |
| **`sparkline`** | 新增 | 迷你趋势图（块字符/柱状） |
| **`meter`** | 新增 | 条形计量（宽度/阈值配色/标签） |
| **`control`** | 新增（从中断提升） | 交互控件（buttons/select/text/number/checkbox） |
| **`submit`** | 新增（从中断提升） | 表单提交/取消行 |
| **`canvas`** | **仅预留** | 完全自绘（本版只解析 + 往返保留 + 降级） |

### 4.3 新增 kind 详规

#### 4.3.1 `row`（横向组合）

```jsonc
{
  "kind": "row", "gap": 1, "align": "left",      // align: left|center|right|stretch
  "items": [
    {"kind": "text", "text": "CPU"},
    {"kind": "sparkline", "data": [1,3,2,7,5], "w": "flex"},   // w: "flex" | 固定列数
    {"kind": "text", "text": "12%", "w": 6, "align": "right"}
  ]
}
```

- 列宽分配：先按各列自然宽度（`ComputeRequirement().min_x`）分配，再把剩余宽度按 `flex` 权重分配；总宽不足时按比例收缩并允许最右列截断。
- 行高 = 子项最大行数；子项内部多行元素（markdown/diff）以固定宽度参与 `hbox` 对齐。
- `align` 作用于列内文本的水平对齐（结构化兼容：`right` 用于数值列）。

#### 4.3.2 `box`（分组容器）

```jsonc
{"kind":"box","title":"Index","border":"round",   // border: none|square|round|light
 "pad":1,"titleColor":"accent","items":[ ... ]}
```

- 边框用 box-drawing 字符绘制（`none` 时用背景色分区，与弹窗面性风格一致）。
- 宽度由容器给出（`flex` 语义），高度 = 内容 + 边框。
- 与 `separator`/`gap` 组合可做"信息面板分组"。

#### 4.3.3 `collapse`（可折叠分组）

```jsonc
{"kind":"collapse","id":"stack","title":"Stack (12)","expanded":false,"items":[ ... ]}
```

- 展开状态由宿主维护（键 = `ownerId + ":" + id`），不进入插件状态；点击标题行切换。
- 命中登记为整行（与按钮同机制）。

#### 4.3.4 `table`

```jsonc
{
  "kind": "table",
  "header": true,
  "columns": [
    {"title": "File", "align": "left",  "w": "flex"},
    {"title": "Size", "align": "right", "w": 8},
    {"title": "State", "align": "left", "w": 10, "color": "hint"}
  ],
  "rows": [
    ["main.cpp", "12.4 KB", "ok"],
    ["gfx/render.cpp", "48.1 KB", {"text": "warn", "color": "error", "action": "open:gfx/render.cpp", "args": {"line": 12}}]
  ]
}
```

- 单元格：字符串 或 item（`{"text","color","action","args"}`；字符串时按列色渲染）。
- 宽度算法：自然宽度 → 超出可用宽度时按"最宽列优先收缩"到最小宽度（默认 4 列），超出部分按显示宽度截断加 `…`（宽字符安全）。
- 单元格可点（登记命中，复用 action 派发）。
- 不支持单元格内多行（`wrap` 保留字段，本版忽略）；需要多行时用 `row` 组合。

#### 4.3.5 `tree`

```jsonc
{
  "kind": "tree", "connector": true,
  "nodes": [
    {"label": "src", "color": "accent", "children": [
        {"label": "main.cpp", "action": "open:src/main.cpp"},
        {"label": "io/", "children": [ ... ]}
    ]}
  ]
}
```

- 渲染用 `├─`/`└─`/`│` 前缀；每节点行可选 `action`（点击派发）。
- `nodes` 为**已展开视图**（v1）：折叠/展开由插件决定内容；后续（P2）支持宿主管理的 `collapse` 折叠态（键 = 节点路径）。

#### 4.3.6 `kv`

```jsonc
{"kind":"kv","sep":" : ","items":[{"k":"Model","v":"gpt-x"},{"k":"Tokens","v":"12.3K","vColor":"accent"}]}
```

- 两列对齐（键列宽 = 最长键，可 `kw` 固定；值超宽截断）。
- 等价于两列 `table` 的语法糖，渲染走同一路径（保证单一实现）。

#### 4.3.7 `sparkline`

```jsonc
{
  "kind": "sparkline", "data": [3,5,2,8,6,9], "height": 1,
  "style": "block",            // block(▁▂▃▄▅▆▇█) | bar(▏▎▍▌▋▊▉█) | auto
  "min": 0, "max": 100,        // 缺省按数据自算
  "color": "accent", "colors": ["normal","accent","error"],  // 可选: 按值分档
  "label": "CPU", "unit": "%", "showLast": true
}
```

- 数据多于可用宽度时按桶聚合（均值）；少于宽度时按比例铺开或右对齐。
- `height > 1`：用纵向块字符（每行 8 级）实现 2~4 行高分辨率图（同一数据分档到多行）。
- 渲染为**多行行模型**（`UiRow.lines = height`），每行一个元素（块字符串 + 可选标签/末值）。

#### 4.3.8 `meter`

```jsonc
{
  "kind": "meter", "value": 72, "total": 100, "width": 24, "label": "GPU", "unit": "%",
  "thresholds": [{"at": 80, "color": "error"}, {"at": 60, "color": "thinking"}]
}
```

- 内部即"填充块 + 背景块 + 百分比文本"的一行；`progress` 兼容映射（`value` 0..1 → `value*100`）。
- `thresholds` 由高到低匹配首个满足项决定填充色。

#### 4.3.9 `control` / `submit`（**从中断提升为共享组件**）

字段与语义**完全复用** `InterruptUiBlock` 的 control/submit 定义（`control`: `buttons|select|text|number|checkbox`；`options`/`default`/`commitOnPick`/`integer`/`min`/`max`/`step`/`multiline`；`submit`：`label/labelKey/cancelLabel/cancelLabelKey`），差别只在结果去向：

| 场景 | 状态与输入 | 结果去向 |
|---|---|---|
| 中断表单（现状） | `InterruptView`（消息内表单，提交即应答中断） | 中断结果 `{"values":{...}}` |
| 插件面板/overlay（新增） | 宿主 per-placement 表单状态（键 = `ownerId + 控件 id`） | **action 派发**：`actionId = "__submit"` / `"__cancel"`，`args = {"values":{...}}`；`commitOnPick` 控件即时派发（`actionId = 控件 id`） |

- 复用点：控件渲染、命中、键盘焦点（"最近点击的控件"）、值校验（number 的 min/max/step、text 非空规则）全部走同一实现。
- 焦点与快捷键优先级：全局快捷键（外层 `CatchEvent`）> 表单控件焦点 > 普通按键。
- 状态生命周期：随注册项（面板/段落/overlay/装饰）存在；禁用/卸载时丢弃。

#### 4.3.10 `canvas`（**本版仅预留**）

```jsonc
{"kind":"canvas","w":0,"h":12,"styles":[...],"rows":[...],"hits":[...],"id":"cpu",
 "fallback":"CPU 12% ▁▂▃▅▇█"}
```

本版行为：
1. 解析并**原样保留**（`Item::canvasJson`），保证插件可以先按未来 schema 推送、往返不丢。
2. 渲染降级：`fallback` 文本存在则渲染之；否则渲染诊断行（`[canvas h=12x0]`）。
3. 能力名不声明（宿主不宣告 canvas 能力），插件应自行判断并降级。

### 4.4 校验与上限

| 项 | 上限 | 越界行为 |
|---|---|---|
| 嵌套深度 | 8 | 丢弃超深子树 + 记日志 |
| 单 items 数组元素数 | 512 | 截断 |
| `table` 行数 / 列数 | 512 / 16 | 截断 |
| `tree` 节点数（展开后） | 1024 | 截断 |
| `sparkline` 数据点数 | 4096 | 按桶聚合压缩 |
| 单条 JSON 字节数 | 1 MiB | 拒绝更新（返回非 0）并记日志 |
| 文本长度（单 item） | 64 KiB | 截断 |

约束：解析失败（JSON 非法/类型不符）**不使整份描述失效**——按 item 级降级（渲染 `fallback` 或跳过），并在注册表保留原始 JSON 供诊断。

### 4.5 降级策略（三级）

1. **组件级**: 单 item 不支持 → 渲染 `fallback`；无 `fallback` → 跳过（保持向前兼容，与现有"未知 kind 忽略"一致）。
2. **能力级**: 插件推送前可用 `get_client_state().interfaces` 判断宿主是否声明新能力名；未声明则走旧 kind 组装或纯文本。
3. **前端级**: 行式前端（CLI/FFI/FFI 文本宿主/日志）用 `ui::plainText(items, width)` 输出（表格 → 对齐文本；sparkline → 末值 + 简图；控件 → "标签: 候选项/默认值"）。

---

## 5. 机制层（宿主实现）

### 5.1 解析与缓存

- 入口处（`update_panel` / `update_info_section` / `update_tool_decor` / 工具渲染器输出 / overlay CUSTOM）**解析一次**为 `agentxx::ui::Items`（`std::vector<Item>`）存入 UI 注册表，渲染只读结构，避免每帧 JSON 解析。
- 注册表条目新增：`items`（解析结果）、`itemsJson`（原文，诊断/回读）、`version`（递增，用于消息块缓存 key）、`size`（最近一次布局得到的可用宽高）、`formState`（控件值，仅当含控件时）。
- 注册表现有 COW 语义不变（io 线程写、UI 线程快照读）。
- 工具渲染器的 `items_json` 同样在写入 `ClientToolRenderCache` 时解析（缓存条目加 `itemsParsed`），避免 UI 线程解析。

### 5.2 行模型与测量（同源）

- 保留 `UiRow`，扩展字段：`region`（该行可命中子区域列表）、`formId`（控件归属）、`lines`（多行组件）。
- `measureUiItem(item, ctx)` 与 `renderUiItem(item, ctx, out)` **继续成对实现**（现有约定），新增容器类 kind 时两者用同一套宽度分配函数（`layoutRow` 返回列宽数组，测量与渲染都调用它）。
- 容器测量：`box` = 内容 + 边框 2；`row` = 子项最大行数；`table` = 表头 + 行数；`tree` = 节点数；`collapse` = 折叠时 1 行，展开时 1 + 内容行数。
- 宽度换算统一：新组件一律用新的 `utf8DisplayWidth`（东宽字符按 2 列、emoji 按 2 列、组合字符按 0 列），避免三套口径（`markdown::utf8_display_width` / `ftxui::string_width` / `estimateLines`）继续分叉。
  - 首选落点：**libagentxx**（`agent/lib/include/agentxx/ui/text_width.h`，与 `ui` 同目录），宿主与测试直接可用，且不必改 `third_party`；
  - 若插件侧确实需要宽度（例如自行拼定宽文本），再把它下沉到 `cxx_utilxx_base/string_util.h`（此时插件静态复用一份；注意：改 `third_party` 后须删除对应 build 目录让其重新编译，见 AGENTS.md）；
  - 现有两套口径（markdown/markdown-ui 与 FTXUI）保持不变，逐步在新组件上替换，避免一次性大范围回归。

### 5.3 渲染

- 实现位置：`ui_components.{h,cpp}`（由 `ui_items_render` 演进），**唯一实现**，五个接入点全部调用它。
- 复用既有子渲染：`markdown`（`renderMarkdown`）、`diff`（`renderPluginDiff`）、`diagram`（mermaid）、按钮（`renderPluginButton`）。
- 宽度分配：`row`/`table` 的列宽算法集中在 `layoutRow`/`layoutTable`（纯计算，返回 `vector<int>` 列宽，便于单测）。
- 宽字符：块字符/框线/emoji 按显示宽度推进；表格截断按显示宽度加 `…`；行尾不留半宽字符。
- 主题：新增 kind 的颜色一律走 `uiRoleColor`（语义色）；`box` 边框色取 `hintColor`（`dim`），`table` 表头取 `accentColor`。

### 5.4 命中与交互

1. **子区域命中**：`UiHitRegistry` 新增 `addRegions(element, payload, regions)`：
   - `regions` 为元素局部坐标矩形 + 各自载荷（按钮/单元格/树节点/折叠头/控件）；
   - 命中时返回 `(payload, localX, localY)`；
   - 帧首清空语义不变（未渲染即未登记）。
2. **滚动容器映射**：`Scrollable` 新增 `bool hitTestItem(int x, int y, size_t& itemIndex, int& localX, int& localY) const`（基于 `visibleBoxes()`），面板/Info/overlay 内的命中一律走它，彻底避免"视口外子项幽灵命中"。
3. **控件状态**：抽出 `UiFormState`（`map<控件id, Json>` + 焦点 id + 校验错误），中断与插件面板共用；`InterruptView::ControlState` 迁移到它（保留行为）。
4. **结果回传**：`__submit` / `__cancel` / `commitOnPick` 全部经 `dispatchAction(ownerId, actionId, {"values":{...}})`（io 线程），无需新 API；命中登记时照旧写入 `generation`（代次复查）。
5. **焦点**：点击控件即聚焦（与中断一致）；`Tab/Shift+Tab` 在表单内移动（新增）；`Esc` 释放焦点（有表单时按中断语义取消）。

### 5.5 尺寸感知（G3）

- **快照**：UI 线程每次布局后把各 placement 的可用宽高写入共享状态（`TUISharedState`/`UiLayoutSnapshot`：`{ownerId → {w,h}}`），节流为"值变化时才写"（不是每帧）。
- **事件**：新增 client 事件 `AGENTXX_CLIENT_EVT_UI_LAYOUT`，载荷 `{"regions":[{"id":"<panel/section/decor id>","w":60,"h":20}]}`；在以下时机投递（io 线程）：首次布局完成、尺寸变化、面板激活/打开、overlay 打开、消息列表宽度变化。
- **查询**：SDK 便捷层提供 `ClientPluginBase::regionSize(ownerId)`（管理器直接读快照，io 线程）。
- 兼容性：旧宿主 `subscribe(EVT_UI_LAYOUT)` 返回 NULL → 插件降级（用固定宽度或旧 kind）。

### 5.6 时间（G5）：新增表 `agentxx.client.timer` v1

```c
typedef struct AgentxxClientTimerIface {
    int32_t  version;      /* == 1 */
    uint32_t struct_size;

    /// 注册一次性/周期定时器 (io 线程回调; repeat<=0 表示只触发一次)
    /// - interval_ms >= 50 (宿主下限, 更小值按 50 处理)
    /// - pause_when_hidden != 0 时: 关联区域不可见 (面板未激活/overlay 已关闭) 时不触发
    /// - 宿主动画等级为 Disabled 时不注册 (返回 NULL)
    AgentxxTimer* (*set_timer)(const PluginxxHost* host, int32_t interval_ms, int32_t repeat,
                               void (*cb)(void* ud), void* ud, const PluginxxStringView* owner_id,
                               int32_t pause_when_hidden);
    void (*cancel_timer)(const PluginxxHost* host, AgentxxTimer* timer);
    /// 关联区域当前是否可见 (1/0; 未知返回 0)
    int32_t (*is_visible)(const PluginxxHost* host, const PluginxxStringView* owner_id);
} AgentxxClientTimerIface;
```

- 宿主实现：`ClientPluginManager` 持有定时器表（asio steady_timer，io 线程），卸载/禁用时全部取消；过期回调经 `InflightGuard`。
- 门控：`TUISettings::isAnimationEnabled(Low)`（设置 `Disabled` 时拒绝注册）；`pause_when_hidden` 由 TUI 上报的可见性快照决定。
- 上限：单实例 ≤ 8 个定时器；间隔下限 50 ms；周期定时器合并同帧多次触发。

### 5.7 视图级（G6）

1. **overlay 选项**（`AgentxxOverlaySpec.extra_json`，数据层，无需 ABI）：
   - `{"size":"auto|compact|normal|large|full"}`（默认 normal）
   - `{"width_frac":0.8,"height_frac":0.9}`（显式比例，与 size 互斥时以 frac 优先）
   - `{"scroll":true|false,"footer":true|false,"resizable":true|false}`（`resizable` 预留）
   - 现状修正：`extra_json` 目前只有 TEXT 的 `markdown` 被消费，`width_frac` 只在注释里 → 本次统一实现。
2. **`custom` 中断块派发**：`component` 为空或 `"components"` 时，用 `props.items` 走共享组件渲染；`component` 指向内置名（如 `"table"`）时用 `props` 作为该组件参数；都没有则 `fallback`（保持既有降级语义）。
3. **状态栏扩展**（P2）：`{"text"}` → 支持 `{"segments":[{"text","color"}],"sparkline":[...],"meter":{...}}`（单行渲染，复用 `sparkline`/`meter` 实现）。
4. **全屏自绘视图**：留待 canvas（届时可用新增 overlay type 或 `size:"full"` + canvas 组件）。

### 5.8 安全与一致性

- 所有插件回调经 `pluginxx` 的异常守卫（`guardCall`）与 `InflightGuard`；解析失败/超限记日志（`XX_LOGW`）并降级。
- 渲染路径不做 IO/加锁（UI 线程只读快照）；表状态只在 UI 线程读写。
- 文案一律 `tr()/trf()`；技术字段不翻译（现有约定）。
- 新组件必须同时提供：测量、渲染、命中（如可交互）、纯文本降级、单测。

---

## 6. 组件库（SDK / lib）

### 6.1 位置与分层

- `agent/lib/include/agentxx/ui/item.h`：数据模型 + `parse/dump/validate/plainText`（无 FTXUI 依赖）。
- `agent/lib/include/agentxx/ui/build.h`：构建器（header-only，fluent），**插件（agent/client 两侧）、中断预设、宿主自身**都可用，天然"不局限于插件框架"。
- 插件侧静态复用（与 `cxx_utilxx*` 同法：随 SDK 头文件引入，不新增动态依赖）。

### 6.2 构建器 API（草案）

```cpp
namespace agentxx::ui {

/// 组件树构建器 (链式; toJson() 产出 schema JSON)
class Items {
public:
    Items& text(std::string_view s, std::string_view role = "normal");
    Items& markdown(std::string_view md);
    Items& diff(std::string_view path, std::string_view oldStr, std::string_view newStr);
    Items& button(std::string_view label, std::string_view actionId, Json args = {}, std::string_view role = "normal");
    Items& separator();
    Items& gap(int lines = 1);
    Items& kv(std::initializer_list<std::pair<std::string, std::string>> entries);
    Items& sparkline(std::span<const double> data, SparklineOpts opts = {});
    Items& meter(double value, double total, MeterOpts opts = {});
    Items& table(TableSpec spec);                       // columns + rows
    Items& tree(TreeSpec spec);
    Items& collapse(std::string_view id, std::string_view title, bool expanded, Items content);
    Items& row(RowSpec spec, std::vector<Items> cols);
    Items& box(std::string_view title, Items content, BoxOpts opts = {});
    /// 表单控件 (与中断同语义)
    Items& checkbox(std::string_view id, std::string_view label, bool def);
    Items& input(std::string_view id, std::string_view label, std::string_view def);
    Items& number(std::string_view id, std::string_view label, double def, NumberOpts opts = {});
    Items& select(std::string_view id, std::string_view label, std::vector<Option> options);
    Items& buttons(std::string_view id, std::vector<Option> options, bool commitOnPick = false);
    Items& submit(std::string_view label = {}, std::string_view cancelLabel = {});

    const Json& json() const;      // 逐步构建
    Json        dump() const;      // 最终 JSON (供 update_* 接口)
private:
    Json items_ = Json::array();
};

} // namespace agentxx::ui
```

用法示例（插件面板）：

```cpp
agentxx::ui::Items ui;
ui.box("System", agentxx::ui::Items{}
        .kv({{"Model", "gpt-x"}, {"Tokens", std::to_string(tokens)}})
        .sparkline(cpuHistory, {.height = 1, .color = "accent", .showLast = true})
        .meter(cpuPct, 100, {.width = 24, .label = "CPU", .thresholds = {{80, "error"}, {60, "thinking"}}}))
  .table({{{"File", "left", 0}, {"Size", "right", 8}}, rows})
  .checkbox("detail", "显示细节", detailOn)
  .submit("应用", "取消");
panel->update(ui.dump());
```

### 6.3 `ClientPluginBase` 便捷方法

```cpp
// plugin_kit.h (ClientPluginBase 内)
ItemsBuilder panelItems(AgentxxPanel* panel);                    // 局部构建 + 提交
int32_t      setPanelItems(AgentxxPanel*, const ui::Items&);
int32_t      setInfoSectionItems(AgentxxInfoSection*, const ui::Items&);
int32_t      setToolDecor(std::string_view toolCallId, DecorSpec);
int32_t      showOverlay(int type, std::string_view title, const ui::Items&, OverlayOpts);
int32_t      registerTimer(int intervalMs, bool repeat, std::function<void()> fn, TimerOpts);
int32_t      registerKeybind(std::string_view keySpec, std::string_view desc, std::function<void()> fn);
bool         hostSupports(std::string_view capability);           // 查 interfaces
RegionSize   regionSize(std::string_view ownerId);                // 布局快照
```

### 6.4 中断预设复用

- `interrupt_presets.h` 改为用同一套构建器产出 blocks（`InterruptUiBlock` 与 `ui::Item` 之间提供 `toItem/fromItem` 映射）。
- 好处：中断描述也能用 `table/tree/sparkline/row/box`（例如权限卡片的路径表格、子代理任务的树、上下文占用的 meter），且与插件 UI 共享渲染与测试。

### 6.5 纯文本降级 API

```cpp
namespace agentxx::ui {
/// 组件树 → 纯文本行 (行式前端/CLI/FFI/日志)
std::string plainText(const Json& items, int width = 0);
}
```

- 规则：`table` → 列对齐文本（分隔线用 `-`）；`tree` → `  └─ ` 前缀；`kv` → `k: v`；`sparkline` → 末值 + 8 级简图；`meter` → `[####----] 72%`；`control` → "标签: 候选项/默认值 (形态)"；`row` → 以 ` | ` 连接；`box` → 标题行 + 内容；`canvas` → `fallback`。
- `interruptUiPlainText` 内部改为调用它（保持输出兼容性测试通过，必要时保留旧格式分支）。

---

## 7. 接入点收敛（复用面）

### 7.1 矩阵

| 接入点 | 数据来源 | 现状实现 | 收敛后 |
|---|---|---|---|
| 侧边栏面板 | `update_panel` | `renderPluginPanel`（`agent_tui.cpp`）自带 switch | `ui_components`（补齐 markdown/diff/diagram/新 kind） |
| Info 段落 | `update_info_section` | `appendPluginItems`（`tui_sidebar_content.cpp`）自带 switch | 同上 |
| 工具消息装饰 | `update_tool_decor` / 工具渲染器 | `ui_items_render`（已收敛） | 扩展新 kind |
| overlay（含表单） | `open_overlay(CUSTOM)` / `extra_json` | `CustomOverlay`（走共享 items） | 扩展新 kind + 尺寸选项 + 表单提交 |
| 中断描述 | `InterruptUiBlock` | 内容块走共享，控件块自实现，`custom` 未实现 | 内容块+容器/表格/图表走共享；控件块状态迁 `UiFormState`（渲染/命中共享）；`custom` 派发 |
| 状态栏 | `update_status_item` | 纯文本 | P2: segments/sparkline/meter（单行） |
| CLI / FFI / 日志 | 同上描述 | `interruptUiPlainText` | `ui::plainText`（新 kind 全覆盖） |

### 7.2 逐点注意事项

- **面板/Info**：内容在 `Scrollable`/`LazyScrollable` 内 → 命中一律用 `Scrollable::hitTestItem`（不再用子项 `reflect`）；面板宽度变化会触发尺寸事件。
- **消息装饰**：含可点组件/控件的消息 `cacheable=false`（沿用现有规则）；装饰 `version` 与工具渲染缓存 `version` 都计入消息块缓存 key，保证"推送后上屏"。
- **overlay**：打开时若已有模态（核心弹窗），当前实现直接 `return`；本版修正为"按 `extra_json.stack` 判定：默认替换（last-wins，符合注释）；`stack:true` 时才排队"，并记录下来。
- **中断**：控件块迁入共享实现时**保持结果契约与命中语义不变**（`tui_interrupt` 回归测试必须全绿）。
- **CLI**：适配器不声明新能力名；含 `require` 的插件按协商跳过（可选依赖只告警）。

---

## 8. ABI 与协商变更清单

### 8.1 为什么不扩 `agentxx.client.ui` 表

SDK 侧校验（`pluginxx/kit/kit.h::validateInterface`）：

```cpp
if (iface->version != 1 || iface->struct_size < sizeof(Iface)) return nullptr;   // 整表判为不可用
```

宿主填 `struct_size = sizeof(宿主编译期的表)`。若在表尾追加成员并把 version 保持 1：新插件（`sizeof` 更大）遇到老宿主 → **整张 ui 表不可用**（面板/状态栏/工具渲染器全部失效），属破坏性变更；把 version 提到 2 同样使老宿主/老插件互相失配。故：**新能力走新表**。

### 8.2 变更清单

| 变更 | 类型 | 兼容性 |
|---|---|---|
| 新增 kind（row/box/collapse/table/tree/kv/sparkline/meter/control/submit） | 数据层 | 新增 kind：老宿主忽略（需 `fallback`）；`progress` 等老 kind 语义不变 |
| `canvas` kind（预留） | 数据层 | 老宿主忽略；本版降级为 `fallback` |
| overlay `extra_json` 选项（size/frac/scroll/footer） | 数据层 | 老宿主忽略未知键 |
| `custom` 块按 `props.items` 渲染 | 数据层（中断） | 老客户端仍走 `fallback` |
| 能力名 `agentxx.client.components`（声明可渲染新组件集） | 协商 | 新插件 `optional`/`require` 声明；老宿主按缺失处理 |
| 能力名 `agentxx.client.form`（声明插件表单可用） | 协商 | 同上 |
| 事件 `AGENTXX_CLIENT_EVT_UI_LAYOUT`（追加枚举值） | 数据层（事件） | 老宿主对新值订阅失败（返回 NULL）→ 插件降级 |
| 新表 `agentxx.client.timer` v1 | 接口表 | 新 IID；`query_interface` 返回 NULL = 不支持 |
| 新表 `agentxx.client.keybind` v1 | 接口表 | 同上（能力名 `agentxx.client.keybind` 已在 `plugin_interfaces.h` 预留） |
| `agentxx.client.ui` 表 | **不动** | — |

`plugin_interfaces.h` 新增常量：

```cpp
inline constexpr std::string_view ClientComponents = "agentxx.client.components";  // 新组件集
inline constexpr std::string_view ClientForm       = "agentxx.client.form";        // 插件表单
inline constexpr std::string_view ClientTimer      = AGENTXX_IFACE_CLIENT_TIMER;   // 新表 IID
// ClientKeybind 已存在 ("agentxx.client.keybind")，本次实现对应表
```

---

## 9. 线程、生命周期与性能

- **线程**：注册/更新/派发/定时器/快捷键一律 io 线程；UI 线程只读快照 + 写布局快照与表单状态；插件回调经 `InflightGuard` 与异常守卫。
- **生命周期**：
  - 禁用：注册项保留（enable 恢复）、定时器暂停、快捷键停用、表单状态保留（面板内容仍在注册表）。
  - 卸载：摘除注册项、取消定时器/快捷键、缓存失效（按插件）、代次递增（旧点击丢弃）。
  - 会话切换：工具装饰与渲染缓存按既有规则清理；面板/Info/状态栏不随会话清理（由插件自行决定）。
- **性能预算**：
  - 渲染：单 placement 的 items 元素 ≤ 512，行模型节点数与可见行数线性；容器测量走 `layoutAndMeasure` 缓存（`Scrollable` 已按宽度缓存高度）。
  - 推送：同一 placement 每帧最多解析一次；连续推送在注册表侧只保留最后一次（自然合并）。
  - 事件/定时器：高频（< 200 ms）间隔需 `pause_when_hidden`；周期回调合并同帧触发。
  - 基准：`benchmark` 增加"含面板（表格 + sparkline + 表单）的 TUI 场景"帧耗时与内存采样。

---

## 10. 分阶段实施计划

### P1 组件层与结构化组件（核心，零 ABI 变更）

| 步骤 | 内容 | 改动文件（估） |
|---|---|---|
| P1.1 | `agentxx::ui::Item` 模型 + 解析/校验/序列化 + `plainText`（含 canvas 预留） | 新增 `lib/include/agentxx/ui/item.h`、`lib/src/ui/item.cpp`（~500 行） |
| P1.2 | 渲染器扩展：`row`/`box`/`collapse`/`table`/`tree`/`kv`/`sparkline`/`meter` + 测量同源 | `client/.../tui/ui_components.{h,cpp}`（由 `ui_items_render` 演进，~700 行） |
| P1.3 | 命中：`UiHitRegistry::addRegions` + `Scrollable::hitTestItem` | `framework/ui_hit.h`、`scrollable.{h,cpp}`（~120 行） |
| P1.4 | 接入点收敛（面板/Info/装饰/overlay/中断内容块） + 面板补齐缺失 kind | `agent_tui.cpp`、`tui_sidebar_content.cpp`、`message_list.cpp`、`overlays.cpp`、`interrupt_view.cpp`（~350 行改） |
| P1.5 | 中断：内容块支持新组件（`props` 透传）+ `custom` 块派发 + 纯文本走 `ui::plainText` | `middlewares/interrupt_ui.{h,cpp}`、`interrupt_presets.h`（~200 行） |
| P1.6 | SDK 构建器 `agentxx::ui::Items` + `ClientPluginBase` 便捷方法 | `lib/include/agentxx/ui/build.h`、`plugin_kit.h`（~350 行） |
| P1.7 | 测试：`ui_items`（lib）+ `tui_ui_items`（渲染/测量/命中）+ 扩展 `tui_interrupt` | `test/...`（~500 行） |
| P1.8 | 文档：`plugins.md` §9 扩展、`tui.md` §2.2/§2.6/§3、`index.md` 客户端 UI 节 | 3 份文档 |

**验收**：5 个接入点均能渲染新组件（至少各 1 个用例）；`table`/`sparkline`/`box`/`row` 的屏幕单元格断言通过；面板/Info/装饰/overlay/中断的渲染结果与测量一致性测试通过；`interruptUiPlainText` 输出兼容；老 kind 行为零回归（现有测试全绿）。

### P2 交互控件与尺寸感知（零 ABI 变更）

| 步骤 | 内容 |
|---|---|
| P2.1 | `UiFormState` 抽取（中断迁移）+ 控件共享渲染/命中/校验 |
| P2.2 | 插件面板/overlay 表单提交：`__submit`/`__cancel`/`commitOnPick` 经 `dispatchAction` 回传 |
| P2.3 | 尺寸感知：布局快照 + `EVT_UI_LAYOUT` + `regionSize()` |
| P2.4 | overlay 选项（size/frac/scroll/footer）+ 已有模态时的策略修正 |
| P2.5 | 状态栏 segments/sparkline/meter（单行） |
| P2.6 | 测试：`tui_form`（新增）、扩展 `tui_widget`/`tui_interrupt`、`plugin_sdk` 的表单用例 |

### P3 时间与快捷键

| 步骤 | 内容 |
|---|---|
| P3.1 | `agentxx.client.timer` v1（宿主定时器 + 门控 + 生命周期） |
| P3.2 | `agentxx.client.keybind` v1（TUI 全局快捷键注册与派发 + 冲突处理） |
| P3.3 | SDK：`registerTimer` / `registerKeybind`；文档与示例 |
| P3.4 | 测试：定时器触发/取消/隐藏暂停/卸载清理；快捷键冲突与优先级 |

### P4（后续独立设计）：`canvas` 完全自绘

- 前置：本版已落地的解析/降级/能力协商；待设计内容：单元格绘制 Node、子区域命中（已在本版铺设）、区域输入事件表 `agentxx.client.region`、焦点与键盘路由、动画与帧预算、安全上限。

---

## 11. 测试计划

| 层 | 模块 | 覆盖 |
|---|---|---|
| lib | `ui_items`（新增） | schema 解析/序列化往返、校验与上限（深度/数量/字节）、非法输入降级、`plainText`（含宽字符与表格对齐）、`canvas` 预留往返 |
| client | `tui_ui_items`（新增） | 各 kind 渲染屏幕断言（`Screen::CellAt`）、测量与渲染一致、`row` 列宽分配边界（极窄/极宽）、`table` 截断、`sparkline` 聚合、`Scrollable::hitTestItem` 命中（滚动后仍正确）、命中登记未渲染不命中 |
| client | `tui_interrupt`（扩展） | 新组件用于中断描述、`custom` 块派发与 `fallback`、控件迁移后结果契约不变 |
| client | `tui_widget` / `tui_sidebar`（扩展） | 面板收敛后的 kind 支持与命中；拖拽宽度变化触发尺寸事件 |
| plugin | `plugin_sdk`（扩展） | 构建器 → JSON → 面板更新的端到端；表单提交 action 派发；定时器生命周期；快捷键注册冲突 |
| plugin | 新增 DSO 测试插件 `test/plugin/dso_plugins/test_ui_components` | 注册面板/overlay/表单/定时器；禁用/启用/卸载语义（注册摘除与恢复、缓存失效、代次复查） |
| 兼容 | `plugin_bridge` / `plugin_runtime`（扩展） | 老宿主（缺新能力）下的降级路径、未知 kind 忽略 |
| 基准 | `benchmark/resource_*`（扩展） | 含面板（表格 + sparkline + 表单）场景的帧耗时/内存增量 |

---

## 12. 风险与回退

| 风险 | 影响 | 缓解 |
|---|---|---|
| 组件种类膨胀导致渲染实现复杂度上升 | 维护成本/回归面 | 强制"测量与渲染同源 + 单一实现 + 每 kind 必测"；新 kind 需评审 |
| 表格/树在大数据量下渲染开销 | 帧耗时 | 上限（§4.4）+ 单元格单行截断 + 行模型而非每格元素；必要时加虚拟化（前缀：可见行窗口，P2 预留） |
| 插件滥用自绘风格（颜色乱用） | 视觉不一致 | 语义色优先、`dim/bold` 走主题、文档明确"组件优先，自绘留 canvas" |
| 表单引入键盘焦点与全局快捷键冲突 | 交互异常 | 明确优先级（全局 > 表单 > 普通），并提供 `Esc` 释放；测试覆盖 |
| 老宿主/未知 kind 静默忽略导致"面板空白" | 用户困惑 | 新组件带 `fallback`；插件按能力名判断；文档给出降级范式 |
| 尺寸事件风暴（拖拽/流式） | io 线程压力 | 仅值变化时投递 + 单帧合并 + 面板可见性过滤 |
| 定时器滥用 | CPU 占用 | 间隔下限、数量上限、动画等级门控、不可见暂停、卸载清理 |

回退策略：各阶段独立可回退（新增 kind 与接入点收敛分离；定时器/快捷键为独立表，直接不实现即"能力缺失"而非破坏）。文档/契约变更同步更新 `docs/zh-cn/design/*`。

---

## 13. 待确认决策点

1. **组件清单**：`row/box/collapse/table/tree/kv/sparkline/meter` 是否符合预期？是否需要优先新增其他组件（如 `bars`（多序列柱状）、`gauge`（环形/仪表）、`log`（带级别色的日志窗））。
2. **控件共享范围**：插件面板/overlay 是否允许承载表单（引入宿主表单状态与焦点模型），还是仅中断使用（插件侧仍需自管状态）？
3. **尺寸事件粒度**：是否接受"事件 + 快照查询"两件套（推荐），还是只做事件（插件自行缓存）？
4. **`plainText` 口径**：CLI/FFI 的表格/图表降级文本是否需要固定列宽上限（例如 80 列）？
5. **实施节奏**：是否按 P1 → P2 → P3 分批合入（推荐，每批独立验收），或一次性完成 P1+P2？
6. **canvas 预留程度**：仅"解析+往返+降级"（推荐），还是顺带把 `hits` 子区域命中也一并实现（为后续自绘铺路，成本 ~1 天）？

---

## 14. 附录 A：schema 全量示例

### A.1 插件侧面板（结构化 + 图表 + 表单）

```jsonc
{"items":[
  {"kind":"box","title":"System","border":"round","items":[
     {"kind":"kv","items":[{"k":"Model","v":"gpt-x"},{"k":"Tokens","v":"12.3K","vColor":"accent"}]},
     {"kind":"row","gap":1,"items":[
        {"kind":"text","text":"CPU"},
        {"kind":"sparkline","data":[12,18,30,25,42,38,55],"height":1,"color":"accent","w":"flex"},
        {"kind":"text","text":"55%","w":4,"align":"right"}]},
     {"kind":"meter","value":55,"total":100,"width":20,"thresholds":[{"at":80,"color":"error"}]}
  ]},
  {"kind":"collapse","id":"files","title":"Files (3)","expanded":true,"items":[
     {"kind":"table","header":true,
      "columns":[{"title":"File","align":"left","w":"flex"},{"title":"Size","align":"right","w":8}],
      "rows":[["main.cpp","12.4 KB"],["render.cpp","48.1 KB"]]}
  ]},
  {"kind":"checkbox","id":"verbose","label":"Verbose","default":false},
  {"kind":"submit","label":"应用","cancelLabel":"取消"}
]}
```

### A.2 overlay（表格 + 表单）

```jsonc
// open_overlay(type=CUSTOM, title="Files", payload={"items":[...]},
//              extra_json={"size":"large","scroll":true,"footer":false})
{"items":[
  {"kind":"row","items":[{"kind":"badge","text":"indexed"},{"kind":"gap"},{"kind":"button","label":"Rebuild","action":"rebuild"}]},
  {"kind":"table","header":true,"columns":[{"title":"Path","w":"flex"},{"title":"Lines","align":"right","w":8}],"rows":[...]},
  {"kind":"select","id":"mode","label":"Mode","options":[{"value":"fast","label":"Fast"},{"value":"safe","label":"Safe"}]},
  {"kind":"submit"}
]}
```

### A.3 中断描述（新组件可用）

```jsonc
{"version":1,"header":{"segments":[{"text":"! ","color":"error"},{"textKey":"permission.header"}]},
 "blocks":[
   {"kind":"text","text":"需要写入权限","color":"error","bold":true},
   {"kind":"table","header":true,
    "columns":[{"title":"路径","w":"flex"},{"title":"作用域","w":6}],
    "rows":[["/data/app/a.txt","写入"],["/data/app/b.txt","写入"]]},
   {"kind":"control","id":"remember","control":"checkbox","label":"记住此选择","default":false},
   {"kind":"submit","labelKey":"permission.allow","cancelLabelKey":"permission.deny"}
 ]}
```

### A.4 canvas 预留（本版降级）

```jsonc
{"kind":"canvas","w":0,"h":6,"fallback":"[chart unavailable]",
 "styles":[{"fg":"accent"}],"rows":[[0,"▁▂▃▅▇█"]],"hits":[]}
```

---

## 15. 附录 B：现状漂移与随本次一并修正

| # | 现象 | 位置 | 处理 |
|---|---|---|---|
| 1 | 面板/Info 各自一份 items switch，且缺 markdown/diff/diagram | `agent_tui.cpp`、`tui_sidebar_content.cpp` | 收敛到 `ui_components` |
| 2 | `ui_items_render.h` 注释称"三处强制复用的唯一实现"，实际未完全达成 | 注释与实现不符 | 本次真正收敛并更新注释 |
| 3 | `agentxx.client.ui` 表 version 宏=1，文档写 "version 2"、注释写 "v3 新增" | `client_plugin_api.h` / `plugins.md` §9 | 统一表述：表 version 保持 1，新能力用新表/新能力名；文档给出准确说明 |
| 4 | `open_overlay` 注释"last-wins（替换当前 overlay）"，实现遇已有模态直接 `return` | `agent_tui.cpp` | P2 修正为按 `extra_json.stack` 决定替换或排队 |
| 5 | overlay `extra_json` 注释提到 `width_frac`，实际只消费 TEXT 的 `markdown` | `client_plugin_api.h` / `overlays.cpp` | P2 统一实现尺寸选项 |
| 6 | `tui.md` §2.6 "agent 侧插件经 wire 下发 UI 描述（panel/info/status/overlay）" | `docs/zh-cn/design/tui.md` | 澄清：这些注册只能由 client 侧入口发起；agent 侧经 `plugin_data` 交给同名 client 插件；仅中断描述是 agent→client 的 UI 结构 |
| 7 | `interrupt_ui.h` 的 `custom` 块 TODO | `middlewares/interrupt_ui.h` | 本次实现（派发共享组件 + `fallback`） |

---

## 16. 附录 C：文档更新清单

- `docs/zh-cn/design/plugins.md`
  - §9 客户端接口表：新增能力名（components/form/timer/keybind）、新表说明、overlay 选项
  - 新增小节"客户端 UI 组件描述 schema（`agentxx.ui.item`）"：字段表 + kind 表 + 降级策略 + 示例
  - 工具特化渲染小节：补充"items 支持新组件"与 canvas 预留说明
- `docs/zh-cn/design/tui.md`
  - §2.2 组件清单：新增 `ui_components`（唯一渲染实现）与接入点收敛说明
  - §2.6 插件 UI：修正 wire 描述、补充表单/尺寸/定时器/快捷键
  - §3 约束：新增"子区域命中用 `Scrollable::hitTestItem`""测量与渲染同源""时间与动画门控"三条
- `docs/zh-cn/design/index.md`
  - "客户端 UI" 节：把"插件 Info 栏段落/面板"扩展为"组件描述能力矩阵（布局/数据/控件/图表/降级）"
- `AGENTS.md`（记忆文件）：待实施完成后补一行"客户端 UI 组件层（`agentxx/ui` + `ui_components`）"要点
