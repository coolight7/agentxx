# 客户端 UI 能力扩展 —— 实施记录

- 方案文档: [plan.md](plan.md)
- 基于 commit: `ecc97b94`（设计文档提交）
- 最后核对: 2026-09-22（对照 plan.md 逐条核对代码，结论见『状态总览』与『遗留任务清单』；
  核对时代码版本 `38e874f4`）
- 记录规则: 每完成一个阶段更新本文件并 git 提交

---

## 状态总览（2026-09-22 核对）

| 阶段 | 内容 | 状态 |
|---|---|---|
| P1.1 | lib 组件描述层（schema 解析/校验/序列化/纯文本降级/文本宽度） | ✅ 已完成 |
| P1.2 | 客户端唯一渲染实现 `ui_components`（全部 kind） | ✅ 已完成 |
| P1.3 | 子区域命中（`UiHitRegion` / `addRegions` / `Scrollable::hitTestItem`） | ✅ 已完成 |
| P1.4 | 接入点收敛（面板 / Info / 装饰 / overlay / 中断内容块） | ✅ 已完成 |
| P1.5 | 中断描述扩展（`raw` 透传 + `custom` 派发 + 扩展组件） | ✅ 已完成 |
| P1.6 | SDK 构建器 `agentxx::ui::Items` + `ClientPluginBase` 便捷方法 | ✅ 已完成（差 3 个方法，见『遗留-机制与复用』） |
| P1.7 | 测试（`ui_items` + `tui_ui_items`：组件层渲染/测量/命中/表单） | ✅ 已完成（接入点级与插件端到端用例见『遗留-测试欠账』） |
| P1.8 | 文档（plugins.md / tui.md / index.md / AGENTS.md） | ✅ 已完成（3 处残留见『遗留-文档与注释残留』） |
| P2.1 | 中断控件布局迁移到共享实现（`UiFormState` + 共享渲染/交互/校验） | ✅ 已完成 |
| P2.2 | 表单交互（控件编辑 + `__submit`/`__cancel`/`commitOnPick` 经动作通道回传） | ✅ 已完成 |
| P2.3 | 尺寸感知（布局快照 + `EVT_UI_LAYOUT` + `regionSize()`） | ✅ 已完成（上报覆盖面见『遗留-真实缺陷』第 2 条） |
| P2.4 | overlay 尺寸与外观选项（`size`/frac/`footer`/`scroll`/`stack`） | ✅ 已完成 |
| P2.5 | 状态栏 segments / sparkline / meter（单行） | ✅ 已完成 |
| P2.6 | 测试（`tui_form` 专项 / `tui_widget`·`tui_sidebar` 扩展 / `plugin_sdk` 端到端） | ⚠️ 未做（见『遗留-测试欠账』） |
| P3.1–P3.4 | `agentxx.client.timer` / `agentxx.client.keybind` 新表（宿主 + SDK + 适配器 + 测试 + 文档） | ✅ 已完成 |
| P4 | `canvas` 完全自绘 | ❌ 本方案不再实施（见『关闭项』） |

小结：plan 的 P1 / P2（除 P2.6 测试）/ P3 均已落地；剩余工作为
『遗留-真实缺陷』2 条、『遗留-机制与复用』9 条、『遗留-测试欠账』7 条、
『遗留-基准』1 条、『遗留-可选增强』2 条、『遗留-文档与注释残留』3 条。

---

## 遗留任务清单

> 2026-09-22 对照 plan.md 逐条核对代码后整理（证据位置见文末『注意事项 · 本轮核对』）。
> 勾选框用于后续实施时标记进度；未特别说明的项均为「plan 要求、代码中不存在」。

### 遗留-真实缺陷（建议优先处理）

- [ ] **CLI 行式前端静默丢弃新组件中断块**
  - 现象：中断描述里含 `table` / `tree` / `sparkline` / `meter` / `row` / `box` / `kv` /
    `collapse` 的内容块，在 CLI（`agent_stdio.cpp:268`）上完全看不到内容（不是走 `fallback`，
    而是被忽略）。
  - 原因：`interruptUiPlainText`（`middlewares/interrupt_ui.cpp:532`）是独立实现，只认
    text/markdown/gap/separator/diff/control/custom，其余走"未知 kind: 忽略"；
    plan §6.5 要求"`interruptUiPlainText` 内部改为调用 `agentxx::ui::plainText`"未做。
  - 修法：对 `InterruptUiBlock::raw` 走 `agentxx::ui::plainText`（保留老格式分支以维持
    现有输出兼容），并补一条降级用例；同时清理无用/重复的纯文本代码路径。
- [ ] **overlay / 消息 owner 的 `pause_when_hidden` 定时器永不触发**
  - 现象：插件用 overlay owner（`__overlay`）或消息 owner 注册 `pause_when_hidden` 定时器时，
    回调永远不触发（一直"不可见 → 顺延"）。
  - 原因：可见性只上报面板与 Info 段落（`agent_tui.cpp:357`、`tui_sidebar_content.cpp:194`）；
    `isRegionVisible` 对未登记 id 一律返回 false（`client_plugin_manager.cpp:3221`）。
  - 修法：overlay 打开/关闭与消息列表可见性一并上报（与『遗留-可选增强』第 1 条同一改动）。

### 遗留-机制与复用（plan 要求，未实施）

- [ ] **§5.1 入口解析一次、注册表存解析结果**
  - 现状：注册表条目仍只存原始 JSON（`ClientPanel/ClientInfoSection.items`、
    `ClientStatusItem.rich`、`ClientToolDecor.items`、`ClientToolRenderEntry.items` 均无
    `itemsParsed`），解析发生在 UI 线程**每帧渲染时**：`agent_tui.cpp:325`、
    `tui_sidebar_content.cpp:174`、`message_list.cpp:662`（测量）+ `:1621`（渲染）、
    `status_bar.cpp:80`。
  - 影响：每帧重复 JSON 解析（上限 512 项时开销不可忽略）；`size` 落在管理器
    `regionSizes_`、`formState` 落在 `TUIClientAgentIO::pluginForms_`（功能等价，
    但与设计文档描述不一致）。
  - 说明：解析是纯数据操作，未破坏"UI 线程不进插件代码"这条不变量。
- [ ] **§4.4 单条 JSON 字节上限（1 MiB → 拒绝更新 + 记日志）**
  - 现状：`updatePanel` / `updateInfoSection` / `updateToolDecor` / `open_overlay` 等入口
    只判 JSON 合法性，全库无字节上限校验（`ParseLimits` 只管深度/元素数/文本长度）。
- [ ] **§6.4 中断预设复用组件构建器**
  - 现状：`interrupt_presets.{h,cpp}` 仍逐个手写 `InterruptUiBlock`（只有
    text/markdown/diff/separator/gap/submit/control），没有改用 `agentxx::ui::Items`，
    也没有 `Item ↔ InterruptUiBlock` 的 `toItem/fromItem` 映射，更没有
    table/tree/sparkline/row/box 等新组件 preset。
  - 影响：agent 侧中断要用新组件只能手写 raw JSON；plan 举例的"权限卡片路径表格、
    子代理任务树、上下文占用 meter"无从表达。
- [ ] **§6.3 SDK 便捷方法补齐 3 个**：`panelItems(panel)`（局部构建 + 提交）、
  `setToolDecor(toolCallId, DecorSpec)`、`form(...)`
- [ ] **§4.1 `when` 条件显示字段**（plan 要求"本版仅保留字段"）：`agentxx/ui/item.h`、
  `item.cpp`、`build.h` 中完全没有该键，往返也不保留
- [ ] **§5.1 面板 / Info / 状态栏条目的 `version` 字段**（供消息块缓存 key）：目前只有
  `ClientToolDecor.version` 与渲染缓存 version
- [ ] **§5.6 周期定时器"同帧多次触发合并"**：`fireTimerTick` / `armTimer`
  （`client_plugin_manager.cpp:3079` / `:3129`）每个定时器独立 `async_wait`，无合并逻辑
  （间隔下限 50 ms、单实例 8 个、不可见顺延均已实现）
- [ ] **§4.3.5 tree 宿主管理折叠态**（plan 标注为 P2 后续）：宿主折叠状态目前只有
  `collapse` kind（`agent_tui.cpp:1296` / `:1422` + `UiRenderCtx::collapseExpanded`）
- [ ] **§4.3.1 `row` 的 `align: "stretch"`**：`ui_components.cpp:1080` 起只处理
  left / center / right

### 遗留-测试欠账（plan §10 P2.6 + §11）

- [ ] `tui_form` 专项测试模块（当前表单用例并入 `tui_ui_items`）
- [ ] `tui_widget` / `tui_sidebar` 扩展：面板收敛后的 kind 支持与命中、拖拽宽度变化触发
  尺寸事件（现状：client 测试里没有 `registerPanel/updatePanel` 用例，只直接造
  `ClientUiRegistry` 快照且仅 statusItems）
- [ ] `plugin_sdk` 扩展：构建器 → JSON → 面板更新端到端、表单提交动作派发端到端
- [ ] DSO 测试插件 `test/plugin/dso_plugins/test_ui_components`（面板 / overlay / 表单 /
  定时器 + 禁用启用卸载语义、缓存失效、代次复查）
- [ ] `plugin_bridge` / `plugin_runtime` 扩展：老宿主缺新能力下的降级、未知 kind 忽略
- [ ] 5 个接入点各 1 个新组件用例（当前新组件只在 `ui_components` 渲染 harness 与 lib
  解析层验证，没有"面板/Info/装饰/overlay/中断真的把 table 渲染上屏"的用例）
- [ ] 中断新组件块的纯文本降级用例（与『遗留-真实缺陷』第 1 条配对）

### 遗留-基准（plan §9 性能预算）

- [ ] benchmark「组件密集面板」场景：用 `agentxx::ui::Items` 造 20 行表格 + 趋势图 + kv +
  行容器 + 表单控件描述，测每帧 `renderItems`（含 `measureItem`）耗时与行模型内存增量；
  接入 `agent/benchmark/bench_resource.cpp` 的 M2 TUI 场景（插件管理器与 TUI 适配器已就绪），
  场景打开 `AgentConfigStatic::enableBenchmark` 采集帧耗时

### 遗留-可选增强

- [ ] overlay 区域可见性 / 尺寸上报（当前只上报面板与 Info 段落；与『遗留-真实缺陷』
  第 2 条为同一改动）
- [ ] 快捷键列表展示（`list_keybinds` 已备，设置 / 帮助弹窗尚未消费）

### 遗留-文档与注释残留（plan §15 / §16）

- [ ] `plugins.md` 章节号重复：`### 9.5 定时器`(610) 与 `### 9.5 状态栏项的富展示片段`(678)
  同号 → 后者应为 9.7，并顺带调整小节顺序
- [ ] `client_plugin_api.h:306` 注释仍写「通用交互（v3 新增；老宿主按版本截断视角…）」，
  与 plan §15#3「表 version 保持 1、新能力用新表 / 新能力名」的统一表述不符
  （表 version 宏本身为 1，属注释表述问题）
- [ ] `interrupt_ui.h:66` 与 `interrupt_ui.cpp:292` 注释仍写 `custom`「字段预留, 暂未实现」
  （实际已派发共享组件渲染，仅解析 / 序列化以 `raw` 保留）

---

## 关闭项（本方案不再实施）

### P4 `canvas` 完全自绘 —— 关闭

- 决定时间：2026-09-22
- 决定：**本方案的 canvas 范围到此为止**，只保留"类型预留 + 降级"：
  - 保留：`canvas` kind 的解析与往返保留（`Item::canvas`）、渲染降级（`fallback` 文本，
    无 fallback 时输出诊断占位）、不声明 `canvas` 能力名（宿主不宣告，插件自行判断降级）；
  - 关闭：plan §10 的 P4 阶段（单元格绘制 Node、区域输入事件表 `agentxx.client.region`、
    焦点与键盘路由、动画与帧预算、自绘安全上限）以及 §13 决策点 6（是否顺带实现 `hits`
    子区域命中）—— 一并**不再作为本方案的待办**。
- 关闭理由：
  1. 本方案的目标是"补齐现有展示路径的布局 / 绘制 / 尺寸 / 输入 / 时间能力"；canvas 属于
     全新的自绘体系，与本方案的机制边界（宿主渲染控件、插件只收结果、UI 线程不进插件代码）
     不是同一类工作，硬塞进来会让范围与验收标准都失焦。
  2. 现有组件层（`row/box/collapse/table/tree/kv/sparkline/meter` + 控件 + 定时器 +
     尺寸事件 + 快捷键）已覆盖插件的常规展示需求，canvas 当前的收益 / 成本比不成立。
  3. 预留部分已足够支撑未来接入：`canvas` 往返不丢、`fallback` 降级、`UiHitRegion` 子区域
     命中机制、`Scrollable::hitTestItem` 滚动映射、`timer` / `keybind` 两表。未来若要做自绘，
     另立方案文档（可用新增 overlay type 或 `size:"full"` + canvas 组件）即可，不需要本方案
     再开阶段。
- 影响：`plan.md` 中 canvas 相关条目（§4.2 表、§4.3.10、§10 P4、§13#6）保持其
  "预留 / 不做"的原始描述，不再推进；本文档不再列 canvas 待办。
  另：plan §5.7.1 的 overlay `resizable` 选项本就标注"预留"，同样不列为待办。

---

## 阶段 1：组件描述层与渲染收敛（已完成）

### 新增文件

- `agent/lib/include/agentxx/ui/text_width.h`
- `agent/lib/src/ui/text_width.cpp`
  - 终端显示列宽（宽字符 2 列 / 组合字符 0 列 / emoji 2 列）、按列宽截断、右侧补齐、
    按列宽取前缀；不依赖任何 UI 库，宿主与插件都可直接用
- `agent/lib/include/agentxx/ui/item.h`
- `agent/lib/src/ui/item.cpp`
  - `agentxx.ui.item` schema 的规范化模型 + 解析（容错、上限、未知 kind 标记 `known=false`）
    + 序列化（往返保留）+ 纯文本降级 `plainText`
  - 支持的 kind：`text` / `markdown` / `diff` / `separator` / `gap` / `badge` / `diagram` /
    `button`(别名 `action`) / `progress`(归一化为 `meter`) / `meter` / `sparkline` /
    `kv` / `table` / `tree` / `row` / `box` / `collapse` / `control` / `submit` /
    `custom` / `canvas`(仅解析与降级)
- `agent/lib/include/agentxx/ui/build.h`
  - 链式组件树构建器 `agentxx::ui::Items`（header-only，产出插件接口能直接吃的 JSON）
- `agent/client/include/agentxx-client/io/tui/ui_components.h`
- `agent/client/src/io/tui/ui_components.cpp`
  - 终端侧唯一渲染实现（面板 / Info 段落 / 工具消息装饰 / 通用 overlay / 中断内容块共用）
  - 渲染产出"行模型"：元素 + 行数 + 元素内可命中区域；`measureItem` 复用同一条渲染路径，
    估算与渲染不会有第二套判定

### 删除 / 更名

- `ui_items_render.h` / `ui_items_render.cpp` → 由 `ui_components.{h,cpp}` 取代

### 主要改动

- `framework/ui_hit.h`：新增 `UiHitRegion`（局部坐标矩形 + 类型 + 标识 + 参数 + 子序号 +
  归属信息）、`UiHitRegistry::addRegions` / `findRegion` / `findRegionClick`、
  `matchUiHitRegion`
- `scrollable.{h,cpp}`：`ScrollItem::hits` 承载子项内可命中区域；新增
  `hitTestItem(x,y,index,localX,localY)`（含被视口裁剪时仍按子项顶边换算局部坐标）；
  离开视口的子项显式清空布局框，消除"看不见却点得中"的残留命中区
- `agent_tui.{h,cpp}`：面板渲染改用共享组件层；点击改经 `handleSidebarRegionClick`
  （滚动容器定位子项 + 区域判定）；新增折叠展开状态表；面板 markdown 生命周期按帧轮换保存
- `tui_sidebar_content.cpp`：Info 段落渲染改用共享组件层
- `components/message_list.{h,cpp}`：装饰渲染改用共享组件层；`DecorHitBox` 增加 `regions`，
  点击按局部坐标定位到具体区域（表格单元格等）
- `components/overlays.cpp`：`CustomOverlay` 渲染与命中改用共享组件层与
  `Scrollable::hitTestItem`；面性弹窗的分隔线风格经 `UiRenderCtx::separatorStyle` 保留
  （弹窗用整行浅色区块，不画横线）
- `components/interrupt_view.{h,cpp}`：内容块经共享组件层渲染，未知 kind 走
  `InterruptUiBlock::raw` 解析（中断描述可直接使用表格/树/横排/分组/趋势图），
  `custom` 块派发到共享组件（`component` + `props`），无内容时降级 `fallback`
- `middlewares/interrupt_ui.{h,cpp}`：`InterruptUiBlock` 新增 `raw`（解析时保留原始 JSON，
  序列化以它为底，扩展字段往返不丢）
- `tui_i18n.cpp`：新增 `ui.submit` / `ui.cancel` / `ui.none` 三条共用文案

### 验证

- Windows Debug 编译通过（lib / client / test 三个目标）
- `agentxx_test.exe` 全量：**22137 项断言全部通过**（含全部 TUI 与中断用例）

---

## 阶段 2：组件层测试（已完成）

### 新增测试模块

- `test/core/test_ui_items.cpp`（lib，193 项断言）：schema 解析与归一化、往返序列化、
  解析上限（深度/元素数/文本长度）、纯文本降级、显示列宽工具、构建器产出形状
- `test/client/test_tui_ui_items.cpp`（client，98 项断言）：各 kind 的屏幕渲染、
  估算行数与真实布局高度一致、元素内可命中区域坐标与标识、
  `Scrollable::hitTestItem` 滚动前后命中映射、面性弹窗分隔线风格

### 测试中发现并修复的问题

1. `renderKeyValue` 在 `std::move(lines)` 之后读 `lines.size()` → 移动后行数变 0
   （kv 只占 1 行，滚动定位会错）；已在 move 前取行数
2. `submit` 行在无表单状态时也登记提交区域 → 与控件口径不一致（没有状态就没有值可提交）；
   改为只有 `ctx.form != nullptr` 时才登记
3. `Item::interactive()` 未递归树节点子级 → 子节点带动作时漏判
4. 构建器/序列化里 `Json{x}` 会走 `Json(std::initializer_list<Json>)`，构造出
   **单元素数组**而不是标量；全部改为 `Json(x)` 或逐字段赋值
   （`Json::object({{"k", Json{k}}})` 这种写法同样中招）

### 测试辅助经验

- 断言屏幕内容时不要用 `Screen::ToString()`：它会把颜色转义序列一起输出，
  子串会被转义码打断；应逐格读 `PixelAt(x,y).character` 拼文本
  （参考 `test/client/test_tui_ui_items.cpp` 的 `renderToText`）
- 测量一致性断言：`measureItem` 的行数须等于元素真实布局高度
  （`layoutAndMeasure`），这是"测量与渲染同源"的回归保护

---

## 阶段 3：SDK 便捷方法（已完成）

`agent/lib/include/agentxx/plugin/api/plugin_kit.h` 的 `ClientPluginBase` 新增：

- `setPanelItems(panel, ui)` / `setPanelJson(panel, json)`：面板内容直接提交组件树
- `setInfoSectionItems(section, ui)` / `setStatusText(item, text)` / `setStatusJson(item, json)`
- `showItemsOverlay(title, ui, extraJson)` / `showOverlay(type, title, payload, extraJson)`
- `hostSupports(capability)` / `clientStateJson()`：能力协商查询（解析客户端状态里的 `interfaces`）

---

## 阶段 4：表单交互（已完成）

控件（checkbox / select / buttons / number / text）与提交行在**面板、Info 段落、通用 overlay**
里都可交互，状态由宿主维护、结果经既有动作通道回传：

- 共享交互实现：`ui_components.h` 的 `handleFormControlHit` / `handleFormSubmitHit` /
  `handleFormKeyInput` / `validateForm` / `formValues` / `initFormState`
- 归属状态：`TUIClientAgentIO::pluginForms_`（键 = 面板 id / 段落 id）；
  `CustomOverlay` 内另有独立表单状态（键 = `__overlay`）
- 结果契约：提交 → 动作 id `__submit`、参数 `{"values":{控件 id: 值}}`；
  取消 → `__cancel`；`commitOnPick` 的候选项点击即提交（`actionId = 控件 id`）
- 键盘：点击控件即聚焦（`formFocusedOwner_`），字符键/退格/Delete 编辑，Tab 在控件间移动，
  Esc 释放焦点，回车提交；数值框过滤非数字字符，提交前校验范围（失败写提示不提交）
- 面板/段落内容更新时按描述补齐并清理控件状态（描述里已删除的控件不残留）

### 测试

- `tui_ui_items` 新增 51 项断言：初始化、点击命中（勾选/选中/步进/越界/未知控件）、
  键盘输入（替换缺省值/退格/Tab/数值过滤/Esc）、校验与取值、容器内控件

### 踩坑

- `controlAliasOf(item.kind)` 返回的是 `string_view`，指向 `item.kind` 的缓冲区；
  先取 view 再改写 `item.kind` 会让它悬空（短字符串共用同一缓冲区）→ 必须先拷贝成 `std::string`
- `formValues` 递归合并时不要把内层 `{"values":{...}}` 整包并进外层（会多一层嵌套），
  应把控件值收集到同一层对象

---

## 阶段 5：通用 overlay 尺寸与外观选项（已完成）

`open_overlay` 的 `extra_json` 从"只消费 TEXT 的 markdown"扩展为完整选项
（数据层，零 ABI 变更；老宿主忽略未知键）：

- `size`: auto / compact / normal（缺省）/ large / full
- `width_frac` / `height_frac`: 显式比例（与 `size` 同时给出时比例优先）
- `footer`: 是否显示底栏提示；`scroll: false` 视为内容不需滚动（隐藏提示）
- `stack`: 已有 overlay/核心弹窗时的策略 —— 缺省**替换**（last-wins，修正此前
  "已有模态直接 return" 与接口注释不一致的漂移），`stack: true` 保留现有并丢弃本次

实现：`OverlayOptions`（`components/overlays.h` 的 `fromJson` / `resolveFractions`）+
`overlayFrame` 接受选项 + `createUniversalOverlay` 统一设置；测试见 `tui_surface`。

---

## 阶段 6：展示区域尺寸感知（已完成）

- 新事件 `AGENTXX_CLIENT_EVT_UI_LAYOUT`（枚举追加值，老宿主对新值订阅失败即降级）：
  载荷 `{"regions":[{"id","w","h"}]}`，在尺寸变化时向订阅者投递（io 线程）
- 宿主快照：`ClientPluginManager::reportRegionSize(id, w, h)`（UI 线程，值未变化直接返回）
  + `regionSizes()`；同时进入 `get_client_state().regions`，SDK 提供
  `ClientPluginBase::regionSize(regionId)` 查询
- UI 侧上报点：面板渲染回调与 Info 段落渲染（内容行数 + 可用宽度）
- 能力名 `agentxx.client.layout`（TUI 适配器声明）
- 测试：`client_plugins` 覆盖上报/去重/覆盖/非法值忽略与状态快照

---

## 阶段 7：状态栏富展示片段（已完成）

状态栏项 JSON 除 `text`/`tooltip` 外支持单行富片段（`segments` / `sparkline` / `meter`），
由共享组件层渲染为一行；只给 `text` 时行为与之前完全一致（含 24 字截断）。实现要点：

- `ClientStatusItem` 增加 `rich`（注册与更新时解析，随 UI 注册表 COW 快照走）
- `status_bar.cpp` 的 `statusRichElement()` 把片段组装成一个 `row` 组件树交
  `ui_components` 渲染（sparkline 高度强制 1；单侧宽度预算 = 终端宽度 / 3）
- 状态栏的注册表来源改为"本帧快照优先，其次管理器快照"（可测性 + 与其它组件一致）
- 测试：`tui_widget` 覆盖富片段渲染成功与降级文本不出现、纯文本项行为不变

---

## 阶段 8：中断表单迁移到共享实现（已完成）

P1.4/P2.1 的收尾：`InterruptView` 不再自己渲染控件，全部走共享组件层。

### 共享层扩展（`ui_components.cpp`）

- 控件外观统一到一套样式（中断与插件表单同款）：勾选项 `[ ✓ ] / [   ]`（勾选时强调色加粗）、
  单选列表选中项 `▸ ` + 反色底（未选中同宽占位）、输入框为背景填充的字段（聚焦时加粗下划线）、
  数值控件 `[ - ] value [ + ]`（步进按钮用按钮底色）
- `handleFormKeyInput` 补齐非输入类控件的键盘语义（此前只有字符/退格/Tab/Esc）：
  `buttons` 左右切换选中项、`select` 上下切换、`number` 上下步进（受 min/max 约束）、
  `checkbox` 空格翻转；输入框的左右方向键被消费（单行无光标定位，不落到滚动）
- 输入框首次输入**替换缺省值**（原先共享实现是"在缺省值后追加"，与中断不一致，已统一为替换）
- `validateForm` 增加"候选项缺失"校验（`buttons`/`select` 无 options → 写提示并拒绝提交）；
  `formValues` 对未知控件形态不再产出值，数值控件按 `integer` 写整数（保持中断结果契约）
- 新增 i18n 文案 `ui.noOptions`

### `InterruptView` 改动

- 状态结构改为共享层类型：`using ControlState = UiFormControlState`、`using FormState = UiFormState`
  （`FormState::control()` 访问器删除，改用 `find()`）
- 删除 `layoutControl` / `layoutSubmit` / `initControlState` / `step` / `renderValueButton`
  及配套的数值/选项私有 helper（净减 ~570 行）
- 新增 `formItems()`（描述 → 控件/提交行组件项，标签与说明的 i18n 键在此解析；提交行缺省文案
  沿用中断词表 `确认`/`✕`）、`plainItems()`、`firstControlId()`、`registerBlockHits()`
- `layoutForm`：控件块与提交行交给 `renderItem`（`rc.form` 传表单状态），内容块/扩展组件不变；
  命中区域由行模型的 `UiHitRegion` 归因到"块下标 + 控件 id + 子序号"（只登记控件与提交行区域，
  扩展组件内的普通动作区域在中断里没有去处，不参与命中）
- `handleClick`：命中判定改为"行元素框 + 行内区域"两级；控件语义交给
  `handleFormControlHit`（含 commitOnPick → 提交），提交行按 `__submit`/`__cancel` 映射确认/取消
- `handleKey`：回车提交、Esc 释放激活状态；其余按键交给 `handleFormKeyInput`
  （焦点缺失时回落到首个控件；`Tab` 现在也能在控件间移动焦点）
- `confirm`：校验与取值改为 `validateForm` + `formValues`，结果展示文本仍按块顺序拼"标签: 值"

### 测试

- `test_tui_interrupt.cpp`：点击辅助改为按"行元素框 + 区域内点"点击（`clickHit`），
  控件状态读取改用 `find()`；数值非法输入用例改为输入 `1.2.3`（数值框现在过滤字母）；
  未知控件诊断行断言改为共享层的 `[control: xxx]`
- `test_tui_ui_items.cpp`：单选列表断言改为 `▸ B` / `  A`；输入框首次输入断言改为替换语义；
  新增非输入类控件键盘用例（buttons 左右/select 上下/checkbox 空格）与数值方向键步进
- Windows Debug 全量测试：**22245 项断言全部通过**（含 `tui_interrupt` 218 项、`tui_ui_items` 169 项）

### 文档

- `plugins.md` §9.2：补齐键盘语义（方向键/空格/首次输入替换）与"中断表单复用同一实现"的说明
- `tui.md` §2.2/§2.6：说明中断全部块都经 `ui_components` 渲染、命中为两级判定
- `interrupt_ui.h`：`custom` 块的"未实现"TODO 更新为已派发共享组件的说明

### 现在只在一处实现的清单

| 能力 | 唯一实现 |
|---|---|
| 组件解析/校验/上限/纯文本降级 | `agentxx/ui/item.cpp` |
| 组件渲染 + 测量 | `ui_components.cpp` |
| 控件状态/点击/键盘/校验/取值 | `ui_components.cpp` 表单函数 |
| 命中区域 | `UiHitRegion`（行模型）+ `Scrollable::hitTestItem` |
| 结果回传 | 插件表单 = 动作通道；中断 = 结果通道（同一份控件语义） |

---

## 阶段 9：定时器与全局快捷键（P3，已完成）

两张新接口表（老宿主 `query_interface` 返回 NULL → 插件降级；能力名同名）：
`agentxx.client.timer` 与 `agentxx.client.keybind`（表 version 均为 1）。

### 定时器 `agentxx.client.timer`

- 表成员：`set_timer(host, spec)` / `cancel_timer(host, timer)` / `is_visible(host, owner_id)`
- `AgentxxTimerSpec`：`interval_ms`（宿主下限 50，更小按 50 收敛）/ `repeat`（0 = 一次性，
  > 0 = 周期触发次数上限）/ `pause_when_hidden` / `owner_id` / `on_timer` + `user_data`
- 宿主实现（`ClientPluginManager`）：asio `steady_timer` 挂在 client io 执行器上，
  回调在 io 线程经 `InflightGuard` 调用；单实例上限 8 个（只统计存活定时器）
- 门控：动画等级 `Disabled` 时拒绝注册（`setAnimationEnabled`，TUI 启动与设置弹窗
  切换动画等级时同步）；`pause_when_hidden` 且关联区域不可见时**顺延**（不消耗触发次数，
  可见后下一次到时触发）；能力名未声明时拒绝注册（与其它 register_* 一致）
- 生命周期：`detachDomainRegistrations`（禁用/卸载共用）取消全部定时器；
  句柄保持地址稳定（已结束/已取消的 impl 保留到实例销毁），重复 `cancel_timer` 安全
- 可见性：UI 每帧 `reportSidebarRegionVisibility()` 上报（面板 = 当前激活 tab，
  Info 段落 = Info tab 激活），管理器按值变化去重；`is_visible` 与
  `get_client_state().regions` 各自独立快照

### 全局快捷键 `agentxx.client.keybind`

- 表成员：`register_keybind(host, spec)` / `unregister_keybind(host, bind)` /
  `list_keybinds(host, out)`
- 键位口径：`normalizeKeybindSpec`（小写 + 修饰键固定顺序 ctrl/alt/shift/super，
  别名归一 `control→ctrl`、`cmd/win/meta→super`、`return→enter`）；主键支持单字符与
  `f1~f24`/`esc`/`enter`/`tab`/`space`/`backspace`/`delete`/`insert`/方向键/`home`/
  `end`/`pageup`/`pagedown`；**无修饰键的可打印字符不参与匹配**（注册失败，避免抢输入）
- 界面侧：`tui_keybind.h::keybindOfEvent(ftxui::Event)` 把按键转成同口径字符串
  （Ctrl/Alt 字母组合、F 键、方向键及其 Ctrl 变体）
- 派发：TUI 主事件处理器中先查 `hasKeybind(keys)`（短锁）→ 命中即拦截并
  `postKeybindInvocation(keys)` → io 线程复查插件存在/启用 → `InflightGuard` → 回调；
  优先级 = 全局快捷键 > 表单控件焦点 > 普通按键，模态弹窗打开时不触发
- 冲突：同键位只允许一个注册者（先注册者优先，后来者 NULL）；单实例上限 16；
  注销/禁用/卸载时从 UI 注册表摘除（句柄保活到实例析构）

### SDK（`plugin_kit.h`）

- `ClientIfaces` 增加 `timer` / `keybind` 两表查询
- `registerTimer(intervalMs, fn, TimerOptions)` → `std::shared_ptr<TimerHandle>`
  （析构自动 `cancel_timer`）；`isRegionVisible(regionId)`
- `registerKeybind(keys, description, fn)` → `std::shared_ptr<KeybindHandle>`
  （析构自动注销）；`keybindListJson()`

### 测试与文档

- `client_plugins` 新增两段（+67 项断言）：键位规范化（别名/顺序/非法）、注册/冲突/
  派发/注销/卸载清理；一次性与周期定时器触发次数、间隔收敛、可见性顺延、取消幂等、
  动画等级门控、单实例上限、卸载后不再回调
- `tui_input` 新增 `test_keybind_event_mapping`：界面按键 → 宿主口径（含鼠标与可打印
  字符返回空、界面产物能被 `normalizeKeybindSpec` 原样接受）
- `plugins.md` 新增 §9.5（定时器）/§9.6（快捷键）；`tui.md` §2.6 补充派发与可见性；
  `AGENTS.md` 记忆行补充新表
- 全量测试：**22343 项断言全部通过**

---

## 待完成任务（下一步）

见文档开头『遗留任务清单』（2026-09-22 整理）：

- 原先列在这里的 3 条（基准场景 / overlay 可见性上报 / 快捷键列表展示）已并入该清单；
- 补充了对照 plan.md 逐条核对后发现的其余未实施项（2 条真实缺陷、9 条机制与复用欠账、
  7 条测试欠账、3 处文档与注释残留）；
- `canvas` 完全自绘已**移入『关闭项』**（本方案不再实施）。

---

## 注意事项（实施中记录）

### 兼容性

- `progress` kind 在解析时归一化为 `meter`（`value` 由 0..1 比例换算为 0..100 绝对值），
  渲染只有一个实现
- 按钮 action id 三种历史写法都要认：`action`（新）、`action_id`（kind=button 旧写法）、
  `id`（kind=action 旧写法）
- `text` + `button` 相邻项仍按历史行为合并为一行（面板/Info/overlay/装饰统一），
  合并行内的按钮区域坐标按文本显示宽度右移
- 面性弹窗（overlay）的分隔线不能画 `─`（有测试断言"弹窗内无框线/分隔线字符"），
  经 `UiRenderCtx::separatorStyle` 区分

### 踩坑

- `ftxui::color()` 会被同名的局部变量/形参遮蔽（`std::string_view color`），
  组件渲染函数内的颜色变量统一命名 `colorName` / `textColor` / `fillColor`
- 滚动容器内**不能**用子项元素内的 `reflect` 做命中判定：容器测量子项时会用
  "局部坐标大框"布局，视口外子项会残留该框造成幽灵命中（本次已改为
  `hitTestItem` + 区域映射，并在子项离开视口时清空其布局框）
- `bgcolor` 装饰器按"元素被分配的框"着色：要整行铺背景，装饰器必须套在整个行元素上
  （放进 `hbox` 内的子元素只会拿到自身宽度）
- markdown 的 `DomBuilder` 必须与 Element 同生命周期；面板/Info 每帧重建，故按帧保留两代

### 约定

- 新增组件只需改三处：`agentxx/ui/item.h`（字段与解析）、`ui_components.cpp`（渲染 + 测量）、
  对应测试；插件接口表不动
- 测量一律走 `measureItem`（内部渲染一次统计行数），禁止另写一套估算
- 控件（`control`/`submit`）的渲染、点击、键盘、校验、取值只有 `ui_components.cpp` 一份实现：
  新接入点一律传 `UiRenderCtx::form`（`UiFormState`），不要再写控件分支
- 中断/插件表单的差异只允许出现在"结果去处"：中断 → 结果通道（`InterruptSend`），
  插件 → 动作通道（`__submit`/`__cancel`/`commitOnPick` 经 `dispatchAction`）

### 中断迁移（阶段 8）踩过的坑

- 命中必须"行元素框 + 行内区域"两级判定：共享层只给出"元素 + 元素内区域"，
  中断需要把区域归因到（块下标、控件 id、子序号），且区域矩形要一起存下来
  （整行区域 `w <= 0` 时取区域左端点击，`w > 0` 取区域中点）
- 结果 JSON 的类型要按描述保持：整数控件必须写整数（`{"count": 42}` 而非 `42.0`），
  否则中断结果消费方（`interruptValueInteger` 等）与历史契约不一致
- 未知控件形态（`control: "future_widget"`）不参与结果：`formValues` 里别用 `else` 兜底，
  只对已知形态（checkbox/buttons/select/number/text）写结果
- 中断的提交行缺省文案是 `确认` / `✕`（`interrupt.confirm` / `interrupt.cancel`），
  与插件表单的 `提交` / `取消`（`ui.submit` / `ui.cancel`）不同 —— 转换块时要补默认值
- 数值框会过滤非数字字符，所以"输入非法值再校验"的用例要改用可键入但无法解析的内容
  （如 `1.2.3`），而不是 `abc`

### 定时器/快捷键（阶段 9）踩过的坑

- **句柄不能在注销时立即释放**：`unregisterKeybind` 若把 `shared_ptr` 从实例表摘除，
  插件重复注销（幂等调用）就会解引用已释放对象（ASan 报 heap-use-after-free）。
  沿用面板/状态栏项规则：句柄保活到实例析构，注销只把 `inst` 置空作为失效标记
- 定时器句柄同理：取消/到期后 impl **保留在实例表内**（不 erase，地址保持稳定，
  避免"旧句柄命中被复用地址的新定时器"）；数量上限只统计 `alive == true` 的定时器
- `pause_when_hidden` 的语义是"顺延"而不是"消耗"：跳过回调时**不减触发次数**，
  否则一次性定时器在面板不可见时会被静默吃掉
- 能力门控要与其他 `register_*` 一致：`set_timer`/`register_keybind` 都要先查
  `hostSupportedInterfaces()`（Mock 适配器与 TUI 适配器都得声明这两个能力，
  否则插件注册失败且测试的宿主接口清单断言会失效）
- `plugin_kit.h` 里不能写 `&临时对象`（`&PluginStringView::from(...)` 是 C2102 取地址需要左值），
  先落到局部变量再取地址
- 静态成员函数（如 `SettingsOverlay::cycleAnimationLevel`）不能访问成员回调；
  需要"切换后通知外部"的动作要改成非静态（与 `cycleLanguage` 同规则）
- 跨边界向量的类型安全：`ClientPluginInstance::timers` 用
  `vector<shared_ptr<ClientTimerImpl>>`（而不是 `vector<shared_ptr<void>>`），
  利于按指针身份查找与类型安全清理

### 本轮核对（2026-09-22）

核对方法：逐条读 plan.md（§4~§16）→ 在代码中定位对应实现 → 记下"未实施项 + 证据位置"。
核对时代码版本 `38e874f4`（本轮只更新本文档，未改代码）。

关键证据位置（供后续快速复核，结论对应『遗留任务清单』）：

| 结论 | 证据 |
|---|---|
| 注册表只存原始 JSON，解析在 UI 线程每帧发生 | 条目字段：`ClientPanel/ClientInfoSection.items`、`ClientStatusItem.rich`、`ClientToolDecor.items`、`ClientToolRenderEntry.items`；解析点：`agent_tui.cpp:325`、`tui_sidebar_content.cpp:174`、`message_list.cpp:662`/`:1621`、`status_bar.cpp:80`、`overlays.cpp:1819` |
| 无 JSON 字节上限 | `client_plugin_manager.cpp` 的 `updatePanel` / `updateInfoSection` / `updateToolDecor` 等入口只判 JSON 合法性，无 size 校验 |
| `when` 字段不存在 | `agentxx/ui/item.h`、`item.cpp`、`build.h` 中无该键 |
| 中断预设未构建器化 | `interrupt_presets.{h,cpp}` 逐个 `b.kind = "text"/"control"/...` 手写结构，无 `ui::Items` / `toItem` |
| `interruptUiPlainText` 仍是独立实现 | `interrupt_ui.cpp:532`（只认 text/markdown/gap/separator/diff/control/custom，其余"未知 kind: 忽略"）；CLI 调用点 `agent_stdio.cpp:268` |
| 可见性只覆盖面板 / Info | 上报点 `agent_tui.cpp:357`、`tui_sidebar_content.cpp:194`；`isRegionVisible` 对未登记 id 返回 false（`client_plugin_manager.cpp:3221`） |
| 定时器无同帧合并 | `fireTimerTick` / `armTimer`（`client_plugin_manager.cpp:3079` / `:3129`）每定时器独立 `async_wait` |
| 测试缺口 | client 测试无 `registerPanel/updatePanel` 用例（只直接造 `ClientUiRegistry` 快照）；`test_plugin_sdk.cpp` 无 `ui::Items` / `setPanelItems` / `__submit`；`test/plugin/dso_plugins/` 仅有 2 个回滚夹具 |
| 文档残留 | `plugins.md:610` 与 `:678` 同为 `### 9.5`；`client_plugin_api.h:306`「v3 新增」；`interrupt_ui.h:66`、`interrupt_ui.cpp:292`「暂未实现」 |
