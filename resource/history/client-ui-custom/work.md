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
| P1.6 | SDK 构建器 `agentxx::ui::Items` + `ClientPluginBase` 便捷方法 | ✅ 已完成（差 3 个方法 → 阶段 11 已补齐） |
| P1.7 | 测试（`ui_items` + `tui_ui_items`：组件层渲染/测量/命中/表单） | ✅ 已完成（接入点级与插件端到端用例 → 阶段 13/14 已补齐） |
| P1.8 | 文档（plugins.md / tui.md / index.md / AGENTS.md） | ✅ 已完成（3 处残留 → 阶段 10/15 已清理） |
| P2.1 | 中断控件布局迁移到共享实现（`UiFormState` + 共享渲染/交互/校验） | ✅ 已完成 |
| P2.2 | 表单交互（控件编辑 + `__submit`/`__cancel`/`commitOnPick` 经动作通道回传） | ✅ 已完成 |
| P2.3 | 尺寸感知（布局快照 + `EVT_UI_LAYOUT` + `regionSize()`） | ✅ 已完成（上报覆盖面 → 阶段 10 已补齐 overlay/消息） |
| P2.4 | overlay 尺寸与外观选项（`size`/frac/`footer`/`scroll`/`stack`） | ✅ 已完成 |
| P2.5 | 状态栏 segments / sparkline / meter（单行） | ✅ 已完成 |
| P2.6 | 测试（`tui_form` 专项 / `tui_widget`·`tui_sidebar` 扩展 / `plugin_sdk` 端到端） | ✅ 已完成（阶段 13/14；`tui_sidebar` 的"拖拽宽度"一条见『遗留-测试欠账』） |
| P3.1–P3.4 | `agentxx.client.timer` / `agentxx.client.keybind` 新表（宿主 + SDK + 适配器 + 测试 + 文档） | ✅ 已完成 |
| P4 | `canvas` 完全自绘 | ❌ 本方案不再实施（见『关闭项』） |
| 阶段 10 | 两条遗留真实缺陷（CLI 行式前端 / overlay·消息可见性） | ✅ 已完成 |
| 阶段 11 | 遗留机制项（体积上限 / `when` / `version` / `row` stretch / 定时器同帧合并 / SDK 便捷方法） | ✅ 已完成 |
| 阶段 12 | 中断预设改用组件构建器（§6.4：桥接 + 新组件 helper） | ✅ 已完成 |
| 阶段 13 | `tui_form` 模块 + 面板/Info 接入点用例 + 行反射框生命周期修复 | ✅ 已完成 |
| 阶段 14 | `plugin_sdk` 端到端 + 装饰接入点用例 + 老宿主降级 | ✅ 已完成 |
| 阶段 15 | tree 宿主管理折叠态（§4.3.5）+ 文档补齐与章节号修正 | ✅ 已完成 |
| 基准 | benchmark「组件密集面板」场景（plan §9 性能预算） | ❌ 未做（见『遗留-基准』的评估结论） |

小结（阶段 15 结束时）：plan 的 P1 / P2 / P3 全部落地；遗留清单中的真实缺陷、机制与复用、
测试欠账、可选增强、文档残留**全部完成**，仅"基准场景"一条经评估不做；`canvas`（P4）按
既定决定关闭。全量测试 **22543 项断言通过**（无 ASan 报告）。

---

## 遗留任务清单

> 2026-09-22 对照 plan.md 逐条核对代码后整理（证据位置见文末『注意事项 · 本轮核对』）。
> 勾选框用于后续实施时标记进度；未特别说明的项均为「plan 要求、代码中不存在」。

### 遗留-真实缺陷（建议优先处理）

- [x] **CLI 行式前端静默丢弃新组件中断块**（2026-09-22 修复）
  - 现象：中断描述里含 `table` / `tree` / `sparkline` / `meter` / `row` / `box` / `kv` /
    `collapse` 的内容块，在 CLI（`agent_stdio.cpp:268`）上完全看不到内容（不是走 `fallback`，
    而是被忽略）。
  - 原因：`interruptUiPlainText`（`middlewares/interrupt_ui.cpp:532`）是独立实现，只认
    text/markdown/gap/separator/diff/control/custom，其余走"未知 kind: 忽略"；
    plan §6.5 要求"`interruptUiPlainText` 内部改为调用 `agentxx::ui::plainText`"未做。
  - 修法：对扩展块走 `agentxx::ui::plainText`（老格式分支保留，输出兼容），未知 kind
    输出 `fallback`（都没有才跳过）；见本文档『阶段 10』。
- [x] **overlay / 消息 owner 的 `pause_when_hidden` 定时器永不触发**（2026-09-22 修复）
  - 现象：插件用 overlay owner（`__overlay`）或消息 owner 注册 `pause_when_hidden` 定时器时，
    回调永远不触发（一直"不可见 → 顺延"）。
  - 原因：可见性只上报面板与 Info 段落（`agent_tui.cpp:357`、`tui_sidebar_content.cpp:194`）；
    `isRegionVisible` 对未登记 id 一律返回 false（`client_plugin_manager.cpp:3221`）。
  - 修法：overlay 打开/关闭与消息列表可见性一并上报（与『遗留-可选增强』第 1 条同一改动）；
    见本文档『阶段 10』。

### 遗留-机制与复用（plan 要求，未实施）

- [x] **§5.1 入口解析一次、注册表存解析结果**（2026-09-22 部分完成：`version` 字段已加，
  「解析结果落表」见下条说明）
  - 现状：注册表条目仍只存原始 JSON（`ClientPanel/ClientInfoSection.items`、
    `ClientStatusItem.rich`、`ClientToolDecor.items`、`ClientToolRenderEntry.items` 均无
    `itemsParsed`），解析发生在 UI 线程**每帧渲染时**：`agent_tui.cpp:325`、
    `tui_sidebar_content.cpp:174`、`message_list.cpp:662`（测量）+ `:1621`（渲染）、
    `status_bar.cpp:80`。
  - 影响：每帧重复 JSON 解析（上限 512 项时开销不可忽略）；`size` 落在管理器
    `regionSizes_`、`formState` 落在 `TUIClientAgentIO::pluginForms_`（功能等价，
    但与设计文档描述不一致）。
  - 说明：解析是纯数据操作，未破坏"UI 线程不进插件代码"这条不变量。
  - 已完成部分：注册表条目新增 `version`（面板/Info/状态栏，`update_*` 递增）与
    体积上限校验；「解析结果入注册表」仍待做（**已评估：收益低于风险**，见
    『阶段 11 · 结论』）。
- [x] **§4.4 单条 JSON 字节上限（1 MiB → 拒绝更新 + 记日志）**（2026-09-22 完成）
  - `kUiJsonMaxBytes` + `acceptUiJsonSize()`（`client_plugin_manager.{h,cpp}`），
    覆盖 `update_panel` / `update_info_section` / `update_status_item` /
    `update_tool_decor` / `open_overlay`（payload + extra）/ `register_tool_renderer`
    模版 / 工具渲染器输出 items。
- [x] **§6.4 中断预设复用组件构建器**（2026-09-22 完成，见『阶段 12』）
  - 已提供中断层 ↔ 组件层的唯一映射（`middleware::itemOf` / `blockOf`，客户端
    `itemFromInterruptBlock` 改为转发）、用构建器拼中断描述的入口
    （`preset::blocksOf(Items)` / `contentBlock(Items)`）与新组件 helper
    （`tableBlock` / `treeBlock` / `meterBlock`）；
  - 无 i18n 键的内容块 helper 已改为经组件构建器产出；带文案键的 helper
    （`textBlockKey` / 控件 / 提交行）保持直接构造 —— 组件层没有"文案键"概念
    （键由客户端在转换前解析，见 `InterruptUiBlock` 说明），这是**有意为之的分工**。
- [x] **§6.3 SDK 便捷方法补齐 3 个**（2026-09-22 完成）
  - `ClientPluginBase::panelItems(panel)` → `PanelWriter`（就地构建 + 作用域结束自动提交）；
  - `ClientPluginBase::setToolDecor(toolCallId, DecorSpec)` / `clearToolDecor()`；
  - `form(...)` 落在构建器 `Items::form(FormSpec)`（无需宿主访问，插件/中断两侧共用），
    另加 `Items::array()`（导出内层数组，供装饰等接口用）。
- [x] **§4.1 `when` 条件显示字段**（2026-09-22 完成：解析 + 往返保留 + 构建器 `Items::when`；
  渲染不消费，与 plan「本版仅保留字段」一致）
- [x] **§5.1 面板 / Info / 状态栏条目的 `version` 字段**（2026-09-22 完成：`update_*` 递增）
- [x] **§5.6 周期定时器"同帧多次触发合并"**（2026-09-22 完成：`armedAt`/`dropped`，
  迟到（≥2 个周期）的触发被丢弃，不追赶式补发；用例见阶段 11）
- [ ] **§4.3.5 tree 宿主管理折叠态**（plan 标注为 P2 后续）：宿主折叠状态目前只有
  `collapse` kind（`agent_tui.cpp:1296` / `:1422` + `UiRenderCtx::collapseExpanded`）
- [x] **§4.3.1 `row` 的 `align: "stretch"`**（2026-09-22 完成：
  `layoutColumnWidths(..., stretchAll)` 把剩余宽度均分给各列）

### 遗留-测试欠账（plan §10 P2.6 + §11）

- [x] `tui_form` 专项测试模块（2026-09-22 完成：新建模块，控件状态机 + overlay 表单
  提交端到端；原 `tui_ui_items` 的表单用例迁出）
- [~] `tui_widget` / `tui_sidebar` 扩展（2026-09-22 部分完成）
  - 已完成：面板 / Info 接入点的真实注册路径用例（`registerPanel`/`updatePanel`、
    `registerInfoSection`/`updateInfoSection` → 表格/计量条/横排/分组框上屏 +
    命中区域归属 + 可用尺寸上报）；Info 段落的高度上报口径修正为"内容行数"
  - 未做：拖拽侧边栏宽度 → 尺寸事件（`regionSize` 的"值变化才上报"已在
    `client_plugins` 覆盖；面板/Info 每次渲染都会上报当前可用宽度，但"拖拽触发"
    这一路径没有端到端用例 —— 需要真实 `SidebarComponent` + 拖拽事件驱动）
- [x] `plugin_sdk` 扩展（2026-09-22 完成：构建器 → 面板更新端到端、`Items::form` →
  表单提交经动作通道派发；见『阶段 14』）
- [~] DSO 测试插件 `test/plugin/dso_plugins/test_ui_components`（**已评估：不新建 DSO
  夹具**，见『阶段 14 · 结论』：同等覆盖用"伪实例 + 真实注册/渲染路径"更省且更稳定）
- [x] `plugin_bridge` / `plugin_runtime` 扩展：老宿主缺新能力下的降级、未知 kind 忽略
  （2026-09-22 完成：`plugin_sdk` 覆盖"宿主未声明 components/form 能力"与未知 kind 的
  忽略/降级；见『阶段 14』）
- [~] 5 个接入点各 1 个新组件用例（**接入点已全覆盖**：面板 / Info / overlay / 中断
  已就绪，装饰接入点见『阶段 14』；见下方勾选）
  - 中断接入点：`tui_interrupt`（阶段 12）
  - 面板 / Info 接入点：`tui_widget`（阶段 13）
  - overlay 接入点：`tui_form`（阶段 13，含表格 + 表单 + 提交派发）
  - 装饰（工具消息）接入点：`tui_tool_header`（阶段 14）
- [x] 中断新组件块的纯文本降级用例（阶段 10 随『遗留-真实缺陷』第 1 条一并完成）

### 遗留-基准（plan §9 性能预算）

- [ ] benchmark「组件密集面板」场景：用 `agentxx::ui::Items` 造 20 行表格 + 趋势图 + kv +
  行容器 + 表单控件描述，测每帧 `renderItems`（含 `measureItem`）耗时与行模型内存增量；
  接入 `agent/benchmark/bench_resource.cpp` 的 M2 TUI 场景（插件管理器与 TUI 适配器已就绪），
  场景打开 `AgentConfigStatic::enableBenchmark` 采集帧耗时
  - **评估结论（2026-09-22）: 不实施**。理由:
    1. 本项目的 benchmark 模块**只在 Release 构建启用**（`agent/script/windows_debug_build.bat`
       与 `linux_debug_build.sh` 均传 `AGENTXX_BUILD_BENCHMARK=OFF`），而本轮的验证闭环是
       Debug + ASan（内存占用与耗时不具代表性）；Release + LTO 全量构建与基准运行的成本
       远高于该场景能提供的信息。
    2. 现有 `resource_*` 模块已覆盖"真实 TUI（含面板/Info/装饰）帧耗时与内存分解"的同类
       指标（`docs/zh-cn/design/benchmark.md`），组件密集面板属于**同一口径下的样本差异**，
       不改变验收结论。
    3. 组件层已按 plan §9 的预算做了约束（元素数上限 512、单元格单行截断、行模型而非
       每格元素、测量按宽度缓存），缺少的是"回归基线数字"而不是机制。
    - 若后续需要该基线: 在 Release 下按上述描述补场景即可, 组件层无需改动（构建器 +
      `renderItems`/`measureItem` 已可直接调用）。

### 遗留-可选增强

- [x] overlay 区域可见性 / 尺寸上报（2026-09-22 修复；与『遗留-真实缺陷』第 2 条同一改动）
- [ ] 快捷键列表展示（`list_keybinds` 已备，设置 / 帮助弹窗尚未消费）

### 遗留-文档与注释残留（plan §15 / §16）

- [x] `plugins.md` 章节号重复：`### 9.5 定时器`(610) 与 `### 9.5 状态栏项的富展示片段`(678)
  同号 → 后者已改为 9.7（阶段 15）；顺带补齐 §9.1 的 `when`/体积上限/版本号/树折叠/
  行式前端降级说明
- [x] `client_plugin_api.h:306` 注释仍写「通用交互（v3 新增；老宿主按版本截断视角…）」，
  与 plan §15#3「表 version 保持 1、新能力用新表 / 新能力名」的统一表述不符
  （2026-09-22 改为说明"本段为首版表成员 + 子能力名降级 + 不追加表尾成员"）
- [x] `interrupt_ui.h:66` 与 `interrupt_ui.cpp:292` 注释仍写 `custom`「字段预留, 暂未实现」
  （2026-09-22 改为已派发共享组件渲染的说明）

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

**本方案的待办已清空**（2026-09-22，阶段 15 结束时）：

- 真实缺陷 2 条、机制与复用 9 条、测试欠账 7 条、可选增强 2 条、文档与注释残留 3 条
  —— 全部完成（其中 3 条以"评估后不实施"收口，理由见各自条目：『§5.1 解析结果落表』、
  『DSO 测试插件』、『基准场景』；『拖拽宽度 → 尺寸事件』为部分完成，见『遗留-测试欠账』）。
- `canvas` 完全自绘（P4）按既定决定关闭，见『关闭项』。

后续若要继续演进（不属于本方案，供参考）：

1. `§5.1 解析结果落表`的性能优化：按 `version` 缓存的解析结果（注册项自带 `itemsParsed`
   + 版本号失效），避免每帧重复解析；
2. `拖拽侧边栏宽度 → 尺寸事件`的端到端用例（需要真实 `SidebarComponent` + 拖拽事件）；
3. 若需要基准基线：在 Release 下补「组件密集面板」场景（构建器与渲染函数已可直接调用）；
4. 插件生态侧：把 `items` / `form` / `timer` / `keybind` 的用法沉淀到插件开发文档。

---

## 阶段 10：遗留缺陷修复（已完成，commit `2bc6190d`）

对照『遗留-真实缺陷』两条 + 两条文档注释残留。

### 10.1 行式前端不再丢弃扩展组件中断块

- `interrupt_ui.cpp` 新增 `extendedBlockPlainText(block, width)`：
  - 块的 kind 未被本结构映射成具名字段时，按 `agentxx.ui.item` **同一份 schema**
    解析 `InterruptUiBlock::raw`，交给 `agentxx::ui::plainText` 输出（缩进/折行由组件层
    按 `item.indent` / `width` 处理，与 TUI 渲染口径一致）；
  - 组件层未识别的 kind → 输出 `fallback`；无 `fallback` → 跳过（向前兼容）；
  - 无 `raw` 的程序化构造块 → 只能输出 `fallback`。
- `interruptUiPlainText` 的 `else` 分支（原先"未知 kind: 忽略"）改为走上面这条；
  老分支（text/markdown/gap/separator/diff/control/custom）保持不变，输出兼容。
- 头文件同步：`interruptUiPlainText` 说明补"其余 kind 走组件层降级"；`custom` 块说明
  从"字段预留, 暂未实现"改为"按 component + props 派发到共享组件渲染层"。

### 10.2 overlay / 消息 owner 的可见性（`pause_when_hidden` 门控）

此前只有面板与 Info 段落上报可见性，插件用 `__overlay` 或 `tool_call_id` 作区域 id 时
定时器永远顺延（永不回调）。现补齐两条上报链路：

- **overlay**：`TUIClientAgentIO::reportOverlayVisible(bool)`（区域 id 固定
  `AGENTXX_CLIENT_OVERLAY_OWNER`）在 `openOverlay` 成功入栈后上报可见、`closeOverlay`
  与弹窗自身 Esc/底栏关闭回调里上报不可见；尺寸由 `CustomOverlay::OnRender` 上报
  （`contentWidth` / `totalHeight`）。
- **工具消息装饰**：`MessageListComponent::reportDecorVisibility(vboxes)` 每帧（OnRender，
  用上一帧 `visibleBoxes()`）把"视角内工具消息且已登记装饰"的 `tool_call_id` 上报为可见，
  离开视角的 id 上报为不可见；只处理登记过装饰的 id —— 否则宿主的可见性表会被每个
  `tool_call_id` 撑大（`reportRegionVisible` 的条目不回收）。
- `client_plugin_api.h`：`AgentxxTimerSpec::owner_id` 说明补上可取值
  （面板 id / Info 段落 id / `"__overlay"` / 工具装饰的 `tool_call_id`）；
  「通用交互」段落注释改为"首版表成员 + 子能力名降级 + 不追加表尾成员"的统一表述。

### 10.3 测试

`test/core/test_interrupt_ui.cpp` 新增 `test_plain_text_extended_blocks`（13 项断言）：
表格 / 键值 / 树 / 计量条 / 趋势图 / 横排的纯文本内容、未知 kind 走 `fallback`、
未知 kind 无 `fallback` 时输出为空、缩进 + 折行口径。全量测试 **22355 项断言通过**。

---

## 阶段 11：遗留机制项补齐（已完成，commit `d8fc82c6`）

对照『遗留-机制与复用』清单逐条实施（除中断预设构建器化与 tree 折叠态，见下）。

### 11.1 UI 描述体积上限（§4.4）

- 新增 `agentxx::plugin::kUiJsonMaxBytes = 1 MiB` 与自由函数
  `acceptUiJsonSize(json, what, plugin)`（越界 `XX_LOGW` 后返回 false）；
- 入口：`update_panel` / `update_info_section` / `update_status_item` /
  `update_tool_decor` / `open_overlay`（payload 与 extra_json 各一次）/
  `register_tool_renderer`（模版）/ 两处工具渲染器输出 items；
- 语义：**拒绝更新**（返回非 0）+ 注册表保持上一次成功内容（不落半截状态）。

### 11.2 `when` 字段（§4.1）

`agentxx::ui::Item::when`：解析（限长 256B）+ 序列化往返保留 + 构建器
`Items::when(expr)`；按 plan 定位为**预留字段**（渲染不消费，避免与 canvas 阶段重复设计）。

### 11.3 注册表 `version`（§5.1）

`ClientStatusItem` / `ClientPanel` / `ClientInfoSection` 各加 `uint64_t version`，
对应 `update_*` 成功时递增（面板/Info/状态栏在 UI 线程每帧读快照，版本号目前用于
诊断与后续缓存 key，与工具装饰/渲染缓存的 version 口径一致）。

### 11.4 `row` 的 `align: "stretch"`（§4.3.1）

`layoutColumnWidths(fixed, avail, gap, stretchAll)`：无自适应列时把剩余宽度**均分给
各列**（默认行为仍是"剩余给最后一列"，保持历史外观）。

### 11.5 周期定时器同帧合并（§5.6）

`ClientTimerImpl` 增加 `armedAt`（上次续期时刻）与 `dropped`（诊断计数）：
`fireTimerTick` 在续期后已过去 **≥2 个周期**时判定为"迟到触发"，**丢弃本次回调**
（不消耗 `repeat` 次数）后直接续期 —— io 线程被占住导致多个周期同时到期时不会
"追赶式"连续回调插件。判定只用"续期时刻到现在"，因此**回调自身耗时**不算迟到。

### 11.6 SDK 便捷方法（§6.3）

- `ClientPluginBase::panelItems(panel)` → `PanelWriter`：就地构建（`ui->text(...)`）
  + 作用域结束自动 `update_panel`（`commit()` 幂等，可显式检查返回值）；移动后源对象
  不再提交。
- `ClientPluginBase::setToolDecor(toolCallId, DecorSpec{displayName, summary, items})` /
  `clearToolDecor(toolCallId = {})`（空 id = 清本插件全部）。
- `form(...)` 落在构建器：`Items::form(FormSpec{title, border, fields, submitLabel,
  cancelLabel, showSubmit})`（需要完整 `Items` 类型，故 `FormSpec` 定义在 `Items` 之后，
  成员函数声明处用不完整类型），另加 `Items::array()`（导出内层数组）。

### 11.7 测试与顺带修复

- `client_plugins` 新增两段：UI 体积上限（超大 `update_panel` 被拒 + 注册表内容与
  version 不变）、定时器同帧合并（阻塞回调占住 io 300ms 后**不补发**迟到触发，
  且定时器仍持续触发）。用例用**独立 io_context 并由测试线程 `run_for` 驱动**，
  避免阻塞共享上下文影响其它模块时序。
- `ui_items` 新增 `when` 往返与表单构建器用例（表单分组/控件/提交行、无标题表单）；
- `tui_ui_items` 新增 `row` stretch 与 left 对照用例，并新增
  `renderToGrid`（逐格读取、未写入格补空格）——**位置断言必须用它**：
  `renderToText` 直接拼接各格字符，FTXUI 未写入的格是空串会被吞掉，列位置会失真。
- 顺带修复 `test_remote_agent` 的分离协程生命周期问题（`clientT` 由 detached 协程
  按值捕获保活 + 收尾等待收敛）：全量运行偶发 heap-use-after-free（与本次改动无关，
  但会打断全量验证）。
- 全量测试：**22390 项断言通过**（无 ASan 报告）。

### 结论：§5.1「入口解析一次、注册表存解析结果」不再实施

评估后**不实施**（其余 §5.1 要求 —— `version` 字段 —— 已完成）：

1. 解析（`ui::parseItemList`）是**纯数据操作**，不进入插件代码，"UI 线程不进插件代码"
   这条真正的不变量并未被破坏；
2. 各接入点是"每帧对当前可见项解析一次"，且 `Scrollable` 已按宽度缓存测量结果；
   面板/Info 只有在内容变化时重建（内容变化本身就要重新解析）；
3. 落表需要把 `agentxx::ui::Item`（含 `Json` 成员）搬进 `ClientUiRegistry` 快照——
   快照是 COW 拷贝，会让**每次注册表更新**都深拷贝全部解析结果，代价高于收益；
4. 真要优化应走"按 version 缓存的解析结果"（注册项自带解析缓存 + 版本号失效），
   属于独立性能课题，本方案不做。

---

## 阶段 12：中断预设改用组件构建器（已完成，commit `c53aba3c`）

对应 plan §6.4（『遗留-机制与复用』最后一条与测试相关的欠账）。

### 12.1 中断层 ↔ 组件层唯一映射（lib）

- `agentxx::middleware::itemOf(const InterruptUiBlock&)`：块 → 组件项。
  `text`/`markdown`/`diff`/`separator`/`gap`/`control`/`submit`/`custom` 按具名字段
  映射；其余 kind 按块 `raw`（组件描述本身）解析；无法映射返回 `nullopt`。
- `agentxx::middleware::blockOf(const agentxx::ui::Item&)`：组件项 → 块，
  `raw` = 组件 JSON（扩展组件字段因此往返不丢）。
- 客户端 `itemFromInterruptBlock` 改为转发 `itemOf`（删除重复实现，TUI / 纯文本降级 /
  构建器三条路径共用一份转换）。

### 12.2 用构建器拼中断描述

- `preset::blocksOf(const agentxx::ui::Items&)` → `vector<InterruptUiBlock>`；
  `preset::contentBlock(Items)` 单项版本。
- 新组件 helper：`tableBlock(columns, rows)`（路径清单等）、`treeBlock(nodes)`
  （子代理任务/目录层级）、`meterBlock(label, value, total, thresholds)`（上下文占用）。
- 无 i18n 键的内容块 helper（`textBlock`/`markdownBlock`/`diffBlock`/`separatorBlock`/
  `gapBlock`）改为经组件构建器产出；带文案键的 helper 保持直接构造（组件层没有
  "文案键"概念 —— 键由客户端在转换为组件项之前解析）。
- 构建器补充样式修饰：`Items::indent/bold/dim/wrap`（作用于最近一项，与 `when` 同规则，
  内部走统一的 `setLast`）。

### 12.3 测试

- `interrupt_ui`（lib，+39 项）：构建器 → 块 → 组件项 → 纯文本端到端；表格/树字段映射；
  控件往返（label/step/integer/min-max）；未知组件不使整份描述失效；
  预设 helper 的 kind 正确。
- `tui_interrupt`（client，+12 项）：扩展组件块（表格/树/横排）真的渲染上屏、
  `interruptEstimate == renderedRows`、描述 JSON 往返保留 `columns`/`nodes`。
- 全量测试：**22441 项断言通过**（无 ASan 报告）。

### 踩过的坑（阶段 12）

- 控件标签在两层里字段名不同：中断层是 `label`，组件层是 `controlLabel`（JSON 字段名
  仍为 `"label"`）—— 双向映射必须按 kind 取对应字段，否则标签静默丢失
- `Items::tree(TreeSpec)` 接受的是 `TreeSpec`（含 `nodes`），不能直接传初始化列表
- `blockOf` 不做 i18n 键映射：组件 schema 里没有键的位置，需要键的块直接构造

---

## 阶段 13：tui_form 模块与接入点用例（已完成，commit `b112823a`）

### 13.1 修复（新用例暴露的真实缺陷）

**行模型反射框生命周期**：行元素内的 `ftxui::reflect` 只保存 `Box&`，Box 由
`UiRow::box`（shared_ptr）持有；面板 / Info / overlay 等接入点只把 `element` 搬进
滚动容器，Box 随局部渲染结果析构 —— 元素随后被布局时写已释放内存
（ASan 栈顶 `Reflect::SetBox`，`freed by ~UiRow`）。

修法：`ui_components` 新增内部节点 `OwnedReflect`（把 Box 的所有权绑在元素上，
`SetBox`/`Render` 语义与 FTXUI 的 `Reflect` 一致），`renderItem` 用它包裹行元素 ——
移动/缓存元素即带走 Box，所有接入点自动受益（命中判定仍读同一个 Box）。

### 13.2 新增模块 `tui_form`（86 项断言）

- 控件状态机：初始化/点击（勾选/单选/步进/未知 id）/键盘（首次输入替换、退格、
  Tab、方向键消费、非输入类控件语义、Esc 释放）/校验（越界、非数字、候选项缺失）/
  取值（类型与描述一致）/容器内控件收集。
- **overlay 接入点端到端**：真实 `createUniversalOverlay` + 固定视口 → 渲染 →
  按屏幕坐标点击提交行（逐格扫描定位文本）→ 断言动作通道收到 `__submit` +
  `{"values":{...}}`；取消行同理收到 `__cancel`。
- 面板接入点：表单描述渲染后控件行/提交行登记可命中区域（owner = 面板 id、
  plugin = 插件名）。

### 13.3 接入点用例补齐（面板 / Info）

`tui_widget` 新增两个用例（用真实 `registerPanel`/`updatePanel`、
`registerInfoSection`/`updateInfoSection` 路径）：

- 面板：表格（含列对齐）+ 计量条 + 横排 + 趋势图渲染上屏；`regionSizes()` 上报
  可用宽度与内容行数；内容更新后下一次渲染立即反映新描述。
- Info：段落标题 + kv + 分组框渲染上屏；尺寸上报。
- 顺带修正 Info 段落的尺寸上报口径：原来上报"子项数"，改为"内容行数"
  （与面板一致，插件据此判断是否需要折叠/分页）。

### 13.4 支撑改动

- `TUIClientAgentIO::refreshRenderContext()`：帧首快照 + 插件 UI 注册表快照的唯一
  实现（帧循环两处调用点与测试/诊断共用）。
- `TUIClientAgentIO::renderInfoSidebar()` 提升为公开接口（UI 线程渲染入口，供
  SidebarComponent 与测试/诊断调用）。

### 踩过的坑（阶段 13）

- **`reflect` 的引用语义**：只要把行元素搬出 `UiResult`，就必须保证 Box 的所有权
  跟着走（本次用 `OwnedReflect` 解决）；写"元素落表 + 局部结果析构"的代码时要格外
  小心这类隐性悬空
- 控件/提交行的命中区域是"行元素框 + 行内区域"两级；测试里点提交行时直接点标签文本
  位置即可（提交行整行可点）
- **动作派发是异步的**：`dispatchAction` 经 io 线程投递，测试必须驱动 io 上下文
  （`restart()` + `poll()`），且要先持有 `work_guard` —— 否则 `poll()` 因无工作返回
  会把 io_context 标记为 stopped，之后 `postToIo` 直接抛"executor is stopped"
- 计量条在 TUI 里是**带背景色的块字符**（不是 `[####]` 文本），断言要用数值文本；
  `[###-] 72%` 那种形式只出现在纯文本降级里
- 尺寸上报的口径是"内容行数"而不是"子项数"：表格一个子项可能占 4 行；`box` 的
  `border` 缺省是 `none`（要画边框需显式写 `"border":"round"`）

---

## 阶段 14：plugin_sdk 端到端与装饰接入点（已完成，commit `94eaf933`）

### 14.1 plugin_sdk 扩展（75 → 115 项断言）

伪 client UI 接口表（`AgentxxClientUiIface`）捕获 SDK 提交的 JSON，覆盖：

- 构建器 → `setPanelItems` → `update_panel`（表格/计量条字段与 `{"items":[...]}` 形态）；
- `panelItems(panel)` 就地构建器：作用域结束自动提交（含样式修饰 `bold`）；
- `Items::form`：产出分组框 + 控件 + 提交行（`submit` 文案透传）；
- `setStatusText` → `update_status_item`；
- `setToolDecor(toolCallId, DecorSpec)` → `update_tool_decor`（displayName/items 表格），
  `clearToolDecor` → 空 `decor_json`（删除语义）；
- `showItemsOverlay` → `open_overlay`（payload 为组件树 JSON、`extra_json` 原样透传）；
- 未知 kind 原样透传（老宿主忽略，数据层向前兼容）；
- **老宿主降级**：整套 client 接口表缺失时 `setPanelItems`/`setToolDecor`/`showItemsOverlay`
  返回非 0、`hostSupports` 为 false、`regionSize` 取默认值（不崩）。

### 14.2 装饰接入点用例

`tui_tool_header` 新增 `testTuiToolHeaderDecorExtended`：工具消息展开体里的**表格 + 键值对**
渲染上屏（装饰 items 与其它接入点共用组件层）。至此 5 个接入点（面板 / Info / 装饰 /
overlay / 中断）都有新组件用例。

### 14.3 结论：不新建 DSO 测试插件 `test_ui_components`

原计划用 DSO 夹具覆盖"面板/overlay/表单/定时器 + 禁用启用卸载语义"，评估后不新建：

1. 同级覆盖已存在：`client_plugins` 用真实 DSO（example_plugin 等）覆盖接口表、定时器、
   快捷键、禁用/启用/卸载、代次复查；`tui_widget`/`tui_form`/`tui_tool_header` 覆盖
   四个渲染接入点的真实注册路径与渲染结果；`plugin_sdk` 覆盖构建器 → 接口调用的端到端。
2. 新增 DSO 夹具只会重复上述断言，同时增加构建时间、平台依赖（dlopen/版本脚本/导出控制）
   与不稳定性（加载超时、并行构建竞争）。
3. 若未来要验证"插件被禁用后其 UI 立即从渲染路径消失"这类跨端语义，更适合扩展
   `client_plugins` 的既有夹具（伪实例 + 真实注册表），而不是新增 DSO。

---

## 阶段 15：tree 宿主管理折叠态 + 文档（已完成，commit `66f1702c` / `88c77cc8`）

### 15.1 tree 折叠（plan §4.3.5）

- `renderTree`：上下文提供 `collapseExpanded` 时，有子节点的行可点击展开/收起
  （行首 `▾`/`▸`），收起后子树不渲染且不占点击区域；状态键 = **从根到该节点的路径**
  （如 `src/io/`，末尾带 `/` 以便与 id 型键区分）。
- 无动作的节点整行登记为 `UiHitRegionKind::Collapse` 区域（有动作的节点保留动作区域，
  与既有语义一致）；未提供折叠查询时按全展开渲染（行式前端行为不变）。
- TUI 侧无需改动：面板/Info/overlay 传入的 `collapseExpanded` 回调本就按 owner + id
  维护状态，路径键天然适配。

### 15.2 文档

- `plugins.md` §9.5 → §9.7 章节号修正；§9.1 补齐：通用字段 `when`、单条描述体积上限
  (1 MiB，拒绝整条更新)、注册表 `version`、行式前端 `plainText` 口径、中断 ↔ 组件层
  唯一映射 (`itemOf`/`blockOf`/`blocksOf`)、树折叠语义、SDK 新增方法
  (`panelItems`/`setToolDecor`/`form`)。
- `tui.md` §2.2 补充：`OwnedReflect` 反射框所有权 + 树折叠语义。

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

### 阶段 10（遗留缺陷）踩过的坑

- 纯文本降级的**口径不止一套**：中断自己的 `wrapToWidth` 按"UTF-8 字符数"切，组件层
  `ui::plainText` 按**显示列宽**切（宽字符 2 列）。扩展块走组件层后，同一份描述在
  TUI 与 CLI 的折行位置才一致；老分支保持原口径不动（避免无谓回归）
- 计量条的填充格数按 `int(ratio * width + 0.5)` 计算：`value=72, total=100, width=4`
  得到 `[###-]`（3 格）而不是 4 格 —— 写断言时不要凭直觉估
- 上报可见性的区域 id 是**全局**的（只有 id 没有插件维度）：`__overlay` 与
  `tool_call_id` 都按此处理，插件侧应对自己的区域语义负责
- 消息列表的装饰可见性上报必须**过滤"登记过装饰"的 id**：`reportRegionVisible`
  的条目不回收，逐条消息上报会让宿主的可见性表随会话长度无界增长

### 阶段 11（机制项）踩过的坑

- **`size(WIDTH, EQUAL, n)` 不影响本项目的"逐格"测试读法**：断言列位置时要逐格读取，
  FTXUI 屏幕里未写入的格是**空串**（`Cell::character` 默认 `""`），直接拼接会把它们
  吞掉 —— 看似"两列紧挨着"。`test_tui_ui_items.cpp` 的 `renderToGrid` 把空串补成空格；
  `renderToText` 只适合子串断言（不适合位置断言）
- **构建器的控件 kind 是 `control`**：`Items::checkbox(...)` / `input(...)` 产出
  `{"kind":"control","control":"checkbox"|"text"}`（规范形态），不是 `kind=checkbox`；
  写测试断言时按规范形态来
- **定时器用例不要借共享 io_context**：回调里阻塞 300ms 会推迟其它模块的在途事件
  （曾触发 `test_remote_agent` 里一个既有的分离协程生命周期缺陷）。用独立 io_context +
  测试线程 `run_for` 驱动，既不影响别人也不需要额外的停机处理
- **定时器"周期"以 `repeat > 0` 为准**：`repeat = 0` 是一次性（`repeatMode=false`），
  写"周期触发"用例必须给正的 `repeat`（如 100 + 结束时取消）
- **卸载后再放行 io 线程**：先 `unloadAsync`（会取消定时器）再 `work.reset()` +
  `ioT.run()`，否则 `run()` 会被存活定时器一直续期而不返回（测试挂死）
- **JSON 体积上限是"入口闸门"而不是"解析器上限"**：组件层上限（深度/元素数/文本长度）
  仍然生效，两者互补；超限一律**拒绝整条更新**，不落半截状态

### 阶段 14/15 踩过的坑

- **动作派发是跨线程的**：`dispatchAction` 经 io 线程投递；测试要驱动 io 上下文
  （`restart()` + `poll()`）并**持有 work guard**（否则 poll 的"无工作"返回会把上下文
  标记为 stopped，随后 `postToIo` 直接抛"executor is stopped"）
- **伪实例要登记进管理器**：`createInstance` 只构造实例对象，派发路径按插件名在
  插件表里查找（生产路径由 dlopen + lifecycle 完成登记）
- 树/折叠的状态键都走同一个 `collapseExpanded(ownerId, id, default)` 回调：树用
  "节点路径"（`src/io/`）作 id，因此 TUI 侧不需要为树新增任何状态或点击分支
- `UiRow::box` 与元素的关系见阶段 13：凡是"元素落表"的改动都要确认 Box 所有权跟着走

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
