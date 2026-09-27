# 客户端界面层（描述层 / 适配 / kit / GUI 接入指引）

> 相关文档: [index.md](index.md) (整体设计) · [plugins.md](plugins.md) (插件系统, §9 是插件侧用法)
> · [tui.md](tui.md) (TUI 实现) · 描述层库的接入指引: 库仓库 `docs/integration.md`

插件不写界面代码，只交一份 JSON 声明，由客户端画。这份声明由**独立库**
`cxx_pluginxx_ui`（界面描述层）定义，与渲染器无关：同一份描述经过"适配"之后，
既能画在终端（字符列/行），也能画在图形界面（逻辑像素）。

```text
插件 JSON ──parse──► 规范模型 ──adapt(caps)──► 客户端能画的模型 ──► 客户端渲染
                        │
                        └─ 校验上限与未知项（截断 / fallback / 提示）
```

本项目的 TUI 就是其中一个渲染器；将来的 GUI 客户端只需实现渲染与能力上报，
描述层、适配规则、纯文本降级、基础 kit 都直接复用（见文末"GUI 客户端接入指引"）。

---

## 1. 分层与代码位置

| 层 | 位置 | 职责 |
|---|---|---|
| 描述层（库） | `agent/third_party/cxx_pluginxx_ui`（submodule） | 模型 / 解析 / 适配 / 纯文本降级 / 构建器 / kit / 能力段 |
| 插件 SDK | `agent/lib/include/agentxx/plugin/api/plugin_kit.h` | 领域 helper + `agentxx::ui`（描述层）+ `agentxx::ui::kit`（扩展 kit）+ 提交入口 |
| 扩展 kit（生成物） | `agent/lib/include/agentxx/plugin/api/agentxx_ui_kit.g.h`、`agent/js/agentxx_ui_kit.js` | agentxx 的留白口径与常用组合（定义在 `agent/schema/agentxx-ui-kit.def.json`） |
| TUI 渲染 | `agent/client/{include,src}/agentxx-client/io/tui/ui_components.*` | 消费**适配后**的模型；`measureItem` 与渲染同源 |
| 中断桥接 | `agent/lib/include/agentxx/middlewares/interrupt_ui.h` | 中断描述块 ↔ 组件项（域内词汇与描述层词汇的映射） |
| 能力上报 | `agent/client/include/agentxx-client/io/tui/tui_plugin_adapter.h` | `uiCapabilitiesJson()` → 客户端状态快照 `ui` 段 |

库的 CMake 目标：`cxx_pluginxx_ui`（只有头）与 `cxx_pluginxx_ui_static`（解析/适配/纯文本实现）。
`agent/lib`、`agent/client`、`agent/test` 与 `agent/plugins`（独立动态库插件）都已链接静态变体，
插件可以直接用描述层与 kit，不必手写组件 JSON。

---

## 2. 组件词汇与渲染支持

组件全集、字段与枚举见库生成的 `docs/ui-schema.md`；本项目 TUI 实际支持的组件是
`ui_components.cpp` 的 `kTuiBlockNames`（24 个）：

`Text` / `Divider` / `Gap` / `Button` / `Block` / `Row` / `Column` / `Expanded` / `Spacer` /
`SizedBox` / `Padding` / `Align` / `Collapse` / `KV` / `Table` / `Tree` / `Progress` / `Badge` /
`Control` / `Markdown` / `Icon` / `Diff` / `Sparkline` / `Diagram`。

客户端**没实现**的可选块不列进去，由适配自动降级：`Stack` → 最后一个子节点、
`Image` → `alt` 文本、`musicxx.Shader`（musicxx 专属块）→ 跳过。

新增客户端只需要在渲染层加分支、**把组件名加进 `kTuiBlockNames`**（或将来 GUI 客户端的
同名清单）——少声明只会让内容被降级，多声明会画不出来。

## 3. 尺寸：一个数值单位 u

- 数值（u，允许小数）：GUI 1u = 1 逻辑像素；终端按能力段的 `cell`（默认 `{width: 8, height: 20}`）
  换算成列/行（四舍五入；留白换算成 0 就消失；固定尺寸给正数至少占 1 格）；
- `{"percent": n}`：基准是**直接父容器**本轮可分配的空间；终端高度无界时退化成内容尺寸；
- `"auto"`：内容决定；
- `Edges`：`padding` / `margin` 一个字段（数值 = 四边相同，`{horizontal, vertical}` 两轴，单边对象）；
- **不允许负值**：解析时按 0 处理并记提示；
- `aspect`（宽/高）在终端按字符格比例近似：`rows = ceil(列数 × 格宽 / (比例 × 格高))`。

想按格对齐时用扩展 kit 的 `agentxx::ui::kit::cols(n, env)` / `rows(n, env)`。

## 4. 能力段与协商

TUI 客户端上报的能力（`tuiUiCapabilities()`）与渲染前 `adaptItems` 用的是同一份：

```jsonc
{
  "apiVersion": 1,
  "kind": "tui",
  "blocks": ["Text", "Divider", "…"],     // 实际支持（<= kTuiBlockNames）
  "controls": ["buttons", "select", "checkbox", "switch", "text", "number"],
  "cell": { "width": 8, "height": 20 },   // 每个字符格相当于多少 u
  "gap": 12,                              // 默认行距
  "percent": true, "aspect": true,
  "limits": { "maxDepth": 8, "maxItems": 512, "…": 0 }
}
```

发布通道是**客户端状态快照 `get_client_state()` 的 `ui` 段**（`PluginUiAdapter::uiCapabilitiesJson()`），
插件侧读法：

```c++
// 能力摘要（按实例缓存；也可当 env 传给 kit 让 kit 挑合适的变体）
const pluginxx::ui::Capabilities& caps = ctx.uiCapabilities();
if (caps.supportsBlock("Table")) { /* … */ }
items.push_back(agentxx::ui::kit::listRow({{"title", "切歌次数"}}, ctx.kitEnv()));
```

粗粒度能力名（`agentxx.client.components` / `agentxx.client.form` / `agentxx.client.layout`）
仍然保留，用于"新组件还是旧写法"这种整体判断；细粒度判断一律看 `blocks` / `controls`。

## 5. 适配：渲染层不写"我不支持谁"

```c++
// ui_components.cpp: 渲染前统一适配（结果只含本客户端支持的组件）
std::vector<pluginxx::ui::Item> adaptItems(const std::vector<pluginxx::ui::Item>& items);
```

- 适配规则表在库的 `schema/ui.def.json` 的 `adapt` 段，由生成器产出，两个绑定同一份；
- 渲染层**不要**再写"这个组件终端画不了"的判断——那是第二套实现，早晚与规则表分叉；
- 适配保证收敛：最多降到 `Text`（`Text` 是所有客户端的收敛终点）；
- `ParseReport` / `AdaptReport` 的提示记日志（`XX_LOGW("[tui] ui adapt: …")`），是排障第一手线索。

## 6. kit：插件侧的装配便利

| kit | 命名空间 | 内容 |
|---|---|---|
| 基础 kit（库） | `pluginxx::ui::kit` | `title` / `hint` / `text` / `badge` / `icon` / `gap` / `divider` / `button` / `actionsRow` / `card` / `listRow` / `section` / `kv` / `table` / `tree` / `sparkline` / `progressRow` + `cols` / `rows` |
| 扩展 kit（本项目） | `agentxx::ui::kit` | 上面全部 + agentxx 口径的覆盖与常用组合：`card` / `listRow`（终端留白更紧）、`progressRow`（默认按百分比）、`toolCallRow`、`thinkingBlock`、`sessionStats`、`pathDiffRow`、`diagramBlock`、`interruptRow` |

- 插件写 `agentxx::ui::Item` / `agentxx::ui::build::button` / `agentxx::ui::kit::toolCallRow`
  （`agentxx::ui` 与 `pluginxx::ui` 是同一批名字，扩展 kit 在 `agentxx::ui::kit`）；
- kit 只装配、不含逻辑；参数用 `utilxx_base::Json` 对象传，键即参数名；
- 传 `env`（能力摘要）时 kit 会挑更合适的变体（例如目标不支持 `Diff` 时
  `pathDiffRow` 退化成等宽文本）；不传就是中立描述，由客户端 `adapt` 收口；
- 组件说明见生成的 `docs/zh-cn/design/agentxx-ui-kit.md`；
- 改 kit：改 `agent/schema/agentxx-ui-kit.def.json` → 跑 `pwsh -NoProfile -File agent/script/gen_ui_kit.ps1`
  → 生成物一起提交（`agentxx_ui_kit.g.h` / `agentxx_ui_kit.js` / 生成的文档）；
- JS 插件：`agent/js/agentxx_ui_kit.js`（全局 `pluginxx.ui.kit`）。**当前 JS 桥还没有界面注册入口**
  （见 `agent/js/README.md`），所以这份 JS kit 是"就绪未接通"。

## 7. 控件与动作

- **库不提供"表单 + 提交"这一层**：控件只在**值变化时**派发自己的 `action`，
  客户端把 `{id, value}` 合并进参数（插件自己写的 `args` 保留，冲突以客户端补的为准）；
- 动作 kind：`dispatch`（字符串短写即它）/ `route` / `command` / `none`；
- 控件值类型：`checkbox`/`switch` → 布尔；`text` → 字符串；`number` → 数值；
  `buttons`/`select` → 候选项原值；
- 控件状态由客户端维护（焦点、编辑中的文本、本地覆盖值、校验提示），不属于描述；
- **本项目域内的"一起提交"**（面板 / overlay / 中断表单）由客户端自己实现：
  提交动作 id 是域内约定 `__submit` / `__cancel`（`middleware::kInterruptSubmitActionId`
  与客户端 `kFormSubmitActionId` 同源），提交参数形如 `{"values": {"<控件 id>": 值}}`；
  中断的"点击候选项即提交"由 `UiRenderCtx::commitOnPick` 提供（描述层不表达提交语义）。

## 8. 行式前端：纯文本降级

CLI / 日志 / FFI 文本宿主走库的 `plainText`（`interruptUiPlainText` 是中断侧入口）：

- 表格转列对齐文本、树转连接线前缀、趋势图转块字符 + 末值、`Progress` 转 `[####----] 72%`、
  控件转 "标签: 候选项/值 (形态)"；
- `Gap` 与 `Padding` 的左留白都保留（缩进按默认格宽折算成列）；
- 内容块之间不因为"空"被跳过（空行仍占一行）。

## 9. 提交入口与体积上限

| 入口 | 说明 |
|---|---|
| `ClientPluginBase::setPanelItems(panel, items)` / `panelItems(panel)` | 面板内容（就地构建 + 自动提交见 `PanelWriter`） |
| `setInfoSectionItems(section, items)` | Info 段落内容 |
| `setToolDecor(toolCallId, decor)` | 工具消息装饰（折叠头显示名/摘要 + 展开体组件树） |
| `showItemsOverlay(title, items, extraJson)` | 自定义 overlay 的内容 |
| `setPanelJson` / `setStatusJson` | 已有 JSON 直接提交（例如 JS 侧产出的内容） |

- 单条描述体积上限 **1 MiB**（`agentxx::plugin::kUiJsonMaxBytes`）：越界**拒绝整条更新**并记日志，
  注册表保持上一次成功内容；
- 注册表条目带内容 `version`，供缓存 key 与诊断使用。

## 10. 测试

| 模块 | 覆盖 |
|---|---|
| `ui_kit` | 扩展 kit 各组件装配结果、`env` 变体选择、能力矩阵下的适配收敛、dump→parse 往返 |
| `ui_items` | 中断块 ↔ 组件项桥接、预设结构、纯文本降级、显示列宽 |
| `tui_ui_items` | 组件渲染（含 `percent` / `aspect` / `Align` / `Spacer` / 折叠嵌套 / 表格自动列宽）、测量与布局一致、命中区域 |
| 库自带单测 | 解析/往返/上限/适配规则/纯文本金文件（在库仓库跑） |

```bash
# 常用（在 exec 目录）
./agentxx_test ui_kit ui_items tui_ui_items tui_widget
```

---

## 11. GUI 客户端接入指引

将来的 agentxx GUI 客户端（Flutter/Qt/Web…）要做的事只有四件：**声明能力 → 解析 → 适配 → 渲染 + 接动作**。
描述层、适配规则、纯文本降级、基础 kit、夹具都可直接复用，**不需要改库**。

### 11.1 需要实现的能力（组件）

必须实现（核心组件）：`Text` / `Divider` / `Gap` / `Button` / `Block` / `Row` / `Column` /
`Expanded` / `Spacer` / `SizedBox` / `Padding` / `Align` / `Collapse` / `KV` / `Table` / `Tree` /
`Progress` / `Badge` / `Control` / `Markdown`。

可选（不实现就别写进 `blocks`，由适配降级）：`Icon`（GUI 用 `name`，不认识就用 `glyph`）、
`Image`（取不到源用 `alt`）、`Stack`、`Diff`、`Sparkline`、`Diagram` 与客户端专属块。

实现要点（与 TUI 一致，只是量纲换成像素）：

- `Row`/`Column` 的 `main`（含 `spaceBetween`）、`cross`、`gap`；
- `Expanded`/`Spacer` 按 `flex` 分**剩余**空间（不是按比例分整宽）；
- `SizedBox` 的 `width`/`height`/`aspect`；`percent` 基准 = 直接父容器；
- `Edges` 三种写法；`KV.keyWidth` 的 auto/数值/percent；
- `Table` 列宽三态（省略/auto 自动分配、数值、percent），超出可用宽度按比例压缩；
- `Collapse` 按 `id` 维护展开状态（客户端状态）；`Control` 的派发与本地覆盖值（见 §7）。

### 11.2 适配接入

```c++
pluginxx::ui::ParseReport parseReport;
auto doc = pluginxx::ui::parseDocument(json, limits, &parseReport);

pluginxx::ui::AdaptReport adaptReport;
auto renderable = pluginxx::ui::adaptDocument(doc, guiCapabilities(), &adaptReport);
// renderable.blocks 只含 guiCapabilities().blocks 里声明过的组件，最多降到 Text
```

- 能力段用库生成：`pluginxx::ui::capabilitiesToJson(caps).dump()`；
- 不要在自己这边再写"我不支持谁"的判断；失败/降级原因写日志；
- 开发期可以调 `lintWithCaps(items, caps)` 做检查（u 值不是格整数倍、固定宽度与 flex 混用、
  `percent` 出现在高度无界的父容器里…）。

### 11.3 能力上报

一处内容、三处发布（不要各写一份）：

1. 客户端状态快照 `get_client_state()` 的 `ui` 段（`PluginUiAdapter::uiCapabilitiesJson()`）；
2. 与 TUI 相同口径的粗粒度能力名（`agentxx.client.components` 等）；
3. 客户端自己的调试/诊断界面（用于回答"插件为什么没给我某个组件"）。

### 11.4 动作与控件通道

- 守卫 `dispatch`：交回内容归属方（面板/Info/装饰/overlay 由插件实例的动作绑定接住），
  客户端把 `{id, value}` 合并进动作参数；
- `route` / `command`：由客户端定义可跳范围与官方动作；
- 控件状态（焦点、编辑中的值、本地覆盖值、校验）在客户端，描述里的 `value` 只是初值；
- 域内提交（如果需要）自己实现，不要指望描述层有提交语义。

### 11.5 复用清单

| 直接复用 | 说明 |
|---|---|
| `cxx_pluginxx_ui`（C++/Dart/JS 绑定） | 模型、解析、适配、纯文本、构建器、基础 kit |
| `agentxx::ui::kit`（本项目扩展 kit） | agentxx 口径的留白与常用组合 |
| 中断桥接 `interrupt_ui.h` / `interrupt_presets.h` | 中断描述与组件项互转、预设模板（域内词汇在这一层收口） |
| 能力段与上报通道 | `uiCapabilitiesJson()` + 粗粒度能力名 |
| 测试夹具 | 库 `fixtures/*.json`（解析/适配/纯文本），可直接拿来跑新渲染器的回归 |

**接口已经留好的位置**：能力段带 `kind`（`tui` / `gui`）供插件粗判断（例如不给终端推图片），
`blocks` 表让插件精确挑组件；GUI 客户端只要填一份自己的 `blocks` 清单与渲染分支即可。
