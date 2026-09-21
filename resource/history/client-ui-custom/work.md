# 客户端 UI 能力扩展 —— 实施记录

- 方案文档: [plan.md](plan.md)
- 基于 commit: `ecc97b94`（设计文档提交）
- 记录规则: 每完成一个阶段更新本文件并 git 提交

---

## 当前状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| P1.1 | lib 组件描述层（schema 解析/校验/序列化/纯文本降级/文本宽度） | ✅ 已完成 |
| P1.2 | 客户端唯一渲染实现 `ui_components`（全部 kind） | ✅ 已完成 |
| P1.3 | 子区域命中（`UiHitRegion` / `addRegions` / `Scrollable::hitTestItem`） | ✅ 已完成 |
| P1.4 | 接入点收敛（面板 / Info / 装饰 / overlay / 中断内容块） | ✅ 已完成 |
| P1.5 | 中断描述扩展（`raw` 透传 + `custom` 派发 + 扩展组件） | ✅ 已完成 |
| P1.6 | SDK 构建器 `agentxx::ui::Items` + `ClientPluginBase` 便捷方法 | 🟡 构建器已完成，SDK 便捷方法待做 |
| P1.7 | 测试（`ui_items` 193 项 + `tui_ui_items` 98 项） | ✅ 已完成 |
| P1.8 | 文档（plugins.md / tui.md / index.md / AGENTS.md） | ⬜ 待开始 |
| P2 | 表单状态与提交派发、尺寸事件、overlay 选项、状态栏扩展 | ⬜ 待开始 |
| P3 | `agentxx.client.timer` / `agentxx.client.keybind` 新表 | ⬜ 待开始 |

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

## 待完成任务（下一步）

1. P1.6 SDK 便捷方法（`plugin_kit.h`：`setPanelItems` / `showOverlay` / `regionSize` 等）
3. P2：
   - `UiFormState` 接入面板/overlay（控件值编辑、`__submit`/`__cancel`/`commitOnPick` 经
     `dispatchAction` 回传），并把 `InterruptView` 的控件布局迁移到共享实现
   - 尺寸感知：布局快照 + `AGENTXX_CLIENT_EVT_UI_LAYOUT` 事件 + `regionSize()`
   - overlay 尺寸选项（`size` / `width_frac` / `height_frac` / `scroll` / `footer` /
     `stack`）与"已有模态时的替换策略"修正
   - 状态栏 segments / sparkline / meter（单行）
4. P3：`agentxx.client.timer` v1、`agentxx.client.keybind` v1 及对应 SDK/测试
5. 文档：`plugins.md` §9 扩展 + 新增组件 schema 小节；`tui.md` §2.2/§2.6/§3；
   `index.md` 客户端 UI 节；`AGENTS.md` 记忆行

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
